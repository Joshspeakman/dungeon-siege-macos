/* The environment reverb behind DirectSound's EAX: a room-HF filter on the input, early reflections from a tapped
 * delay line, and a late reverb from a feedback delay network.
 *
 * - Input: the mono sum of the sounds sent to the room, through a one-pole low-pass whose gain at 5 kHz is room_hf.
 * - Early reflections: four taps on one delay line, from reflections_delay on, alternating left and right.
 * - Late reverb: after reflections_delay + reverb_delay, two all-pass diffusers (their strength from diffusion) feed
 *   eight delay lines mixed through a Householder matrix (lossless, every line into every other). Each line's gain
 *   per pass makes the whole fall 60 dB in decay seconds, and a one-pole low-pass in each makes 5 kHz fall 60 dB in
 *   decay x decay_hf_ratio seconds. The lines' lengths follow the room's size. Odd lines go left, even right.
 * Levels: the reflections at room + reflections mB, the late reverb at room + reverb mB (its whole energy, as a share
 * of the sound's). */
#include "eax_reverb.h"
#include <math.h>
#include <stdatomic.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define LINES 8
#define MAX_LINE_S 1.0f                     /* the longest delay line (a 100 m room) */
#define MAX_PRE_S 0.5f                      /* reflections delay (0.3 s at most) + reverb delay (0.1 s) */
static const float BASE_MS[LINES] = {29.7f, 37.1f, 41.1f, 43.7f, 47.3f, 53.3f, 59.9f, 67.7f};   /* mutually prime-ish */
static const float AP_MS[2] = {4.7f, 7.3f};
static const float TAP[4] = {1.0f, 1.31f, 1.62f, 2.04f}, TAP_GAIN[4] = {0.62f, 0.55f, 0.45f, 0.38f};

typedef struct { float *buf; int size, pos; } Delay;
struct EaxReverb {
    int rate; pthread_mutex_t param_lock; EaxrParams pending; _Atomic int changed; int active;
    Delay pre, line[LINES], ap[2];
    /* the current coefficients */
    float in_a, in_lp, g_refl, g_rev, late_norm, ap_k, line_g[LINES], line_hg[LINES], line_a[LINES], line_lp[LINES];
    int tap[4], late_tap, line_len[LINES];
};
static int delay_init(Delay *d, int n) { d->size = n; d->pos = 0; d->buf = calloc((size_t)n, sizeof(float)); return d->buf != 0; }
static inline float delay_read(const Delay *d, int back) { int i = d->pos - back; if (i < 0) i += d->size; return d->buf[i]; }
static inline void delay_write(Delay *d, float v) { d->buf[d->pos] = v; if (++d->pos == d->size) d->pos = 0; }
static float mb(float v) { return v <= -10000 ? 0 : powf(10.0f, v / 2000.0f); }
/* the one-pole low-pass y = (1 - a) x + a y' whose gain at angular frequency w is g (0 < g < 1); 0 for g >= 1 */
float eaxr_onepole(float g, float w)
{
    if (g >= 0.9999f) return 0; if (g < 0.0001f) g = 0.0001f;
    double c = cos(w), g2 = (double)g * g, A = 1 - g2, B = 2 - 2 * g2 * c, disc = B * B - 4 * A * A;
    double a = (B - sqrt(disc > 0 ? disc : 0)) / (2 * A);
    return (float)(a < 0 ? 0 : a > 0.999 ? 0.999 : a);
}
EaxReverb *eaxr_new(int rate)
{
    EaxReverb *r = calloc(1, sizeof *r); if (!r) return 0;
    pthread_mutex_init(&r->param_lock, NULL);
    r->rate = rate; int ok = delay_init(&r->pre, (int)(MAX_PRE_S * rate) + 8);
    for (int k = 0; k < LINES; k++) ok &= delay_init(&r->line[k], (int)(MAX_LINE_S * rate) + 8);
    for (int k = 0; k < 2; k++) ok &= delay_init(&r->ap[k], (int)(AP_MS[k] * 0.001f * rate) + 1);
    if (!ok) { eaxr_free(r); return 0; }
    r->pending = (EaxrParams){-10000, 0, 1.49f, 0.83f, -2602, 0.007f, 200, 0.011f, 7.5f, 1.0f};
    r->changed = 1;
    return r;
}
void eaxr_free(EaxReverb *r) {
    if (!r) return; free(r->pre.buf);
    for(int k=0;k<LINES;k++) free(r->line[k].buf);
    for(int k=0;k<2;k++) free(r->ap[k].buf);
    pthread_mutex_destroy(&r->param_lock); free(r);
}
void eaxr_set(EaxReverb *r, const EaxrParams *p) {
    pthread_mutex_lock(&r->param_lock); r->pending=*p; atomic_store(&r->changed,1); pthread_mutex_unlock(&r->param_lock);
}
static void apply(EaxReverb *r, EaxrParams p)
{ float R = (float)r->rate, w = 2 * (float)M_PI * 5000 / R;
    int was = r->active; r->active = p.room > -9000 && (p.reflections > -9000 || p.reverb > -9000);
    if (r->active && !was) {                                   /* a room again: no stale tail from the last one */
        memset(r->pre.buf, 0, sizeof(float) * (size_t)r->pre.size);
        for (int k = 0; k < LINES; k++) memset(r->line[k].buf, 0, sizeof(float) * (size_t)r->line[k].size);
        for (int k = 0; k < 2; k++) memset(r->ap[k].buf, 0, sizeof(float) * (size_t)r->ap[k].size);
        for (int k = 0; k < LINES; k++) r->line_lp[k] = 0; r->in_lp = 0;
    }
    float decay = p.decay < 0.1f ? 0.1f : p.decay > 20 ? 20 : p.decay;
    float ratio = p.decay_hf_ratio < 0.1f ? 0.1f : p.decay_hf_ratio > 2 ? 2 : p.decay_hf_ratio;
    float refl_d = p.reflections_delay < 0 ? 0 : p.reflections_delay > 0.3f ? 0.3f : p.reflections_delay;
    float rev_d = p.reverb_delay < 0 ? 0 : p.reverb_delay > 0.1f ? 0.1f : p.reverb_delay;
    float size = p.env_size < 1 ? 1 : p.env_size > 100 ? 100 : p.env_size;
    float scale = sqrtf(size / 7.5f); if (scale < 0.35f) scale = 0.35f; if (scale > 3.5f) scale = 3.5f;
    r->in_a = eaxr_onepole(mb(p.room_hf), w);
    r->g_refl = mb(p.room + p.reflections); r->g_rev = mb(p.room + p.reverb);
    float diff = p.diffusion < 0 ? 0 : p.diffusion > 1 ? 1 : p.diffusion; r->ap_k = 0.2f + 0.5f * diff;
    for (int k = 0; k < 4; k++) { int n = (int)(refl_d * TAP[k] * R) + 1; r->tap[k] = n < r->pre.size - 1 ? n : r->pre.size - 1; }
    { int n = (int)((refl_d + rev_d) * R) + 1; r->late_tap = n < r->pre.size - 1 ? n : r->pre.size - 1; }
    float mean = 0;
    for (int k = 0; k < LINES; k++) {
        int n = (int)(BASE_MS[k] * 0.001f * scale * R); if (n < 16) n = 16; if (n > r->line[k].size - 1) n = r->line[k].size - 1;
        r->line_len[k] = n;
        float g = powf(10.0f, -3.0f * n / (decay * R)), ghf = powf(10.0f, -3.0f * n / (decay * ratio * R));
        r->line_g[k] = g; r->line_hg[k] = ghf; r->line_a[k] = ghf > g ? expf(-2 * (float)M_PI * 250 / R) : eaxr_onepole(ghf / g, w); mean += (float)n / LINES;
    }
    /* the late reverb's whole energy = its level, whatever the decay and size: an impulse's first pass through the lines
     * carries about 0.45 of its energy, and each pass after keeps 10^(-6 mean / (decay R)) of the one before */
    r->late_norm = sqrtf((1 - powf(10.0f, -6.0f * mean / (decay * R))) / 0.45f);
}
void eaxr_process(EaxReverb *r, const float *in, float *out, int frames)
{
    if (atomic_load(&r->changed) && !pthread_mutex_trylock(&r->param_lock)) {
        EaxrParams p=r->pending; atomic_store(&r->changed,0); pthread_mutex_unlock(&r->param_lock); apply(r,p);
    }
    if (!r->active) return;
    const float hh = 2.0f / LINES, in_g = 1.0f / sqrtf(LINES), out_g = 2.0f / sqrtf(LINES);
    for (int i = 0; i < frames; i++) {
        r->in_lp = (1 - r->in_a) * in[i] + r->in_a * r->in_lp;
        delay_write(&r->pre, r->in_lp);
        float t[4]; for (int k = 0; k < 4; k++) t[k] = delay_read(&r->pre, r->tap[k] + 1) * TAP_GAIN[k];
        float l = r->g_refl * (t[0] - t[2]), rr = r->g_refl * (t[1] - t[3]);
        float u = delay_read(&r->pre, r->late_tap + 1);
        for (int k = 0; k < 2; k++) {                          /* all-pass diffusers */
            float d = delay_read(&r->ap[k], r->ap[k].size), v = u + r->ap_k * d;
            delay_write(&r->ap[k], v); u = d - r->ap_k * v;
        }
        float y[LINES], sum = 0;
        for (int k = 0; k < LINES; k++) {
            float s = delay_read(&r->line[k], r->line_len[k]);
            r->line_lp[k] = (1 - r->line_a[k]) * s + r->line_a[k] * r->line_lp[k];
            y[k] = r->line_hg[k] > r->line_g[k] ? r->line_g[k] * r->line_lp[k] + r->line_hg[k] * (s-r->line_lp[k]) : r->line_lp[k] * r->line_g[k];
            sum += y[k];
            float o = s * out_g * r->g_rev * r->late_norm * ((k & 2) ? -1 : 1);
            if (k & 1) rr += o; else l += o;
        }
        for (int k = 0; k < LINES; k++) delay_write(&r->line[k], y[k] - hh * sum + u * in_g);
        out[2 * i] += l; out[2 * i + 1] += rr;
    }
}
