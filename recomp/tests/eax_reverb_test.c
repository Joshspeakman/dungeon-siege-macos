/* eax_reverb_test: the EAX environment reverb (runtime/win32/eax_reverb.c) measured from its impulse responses.
 *   decay        the late reverb falls 60 dB in the decay time asked (Schroeder integration, -5 to -35 dB), +-20%
 *   hf ratio     with decay_hf_ratio 0.3 the treble (first difference) dies at least twice as fast as with 1.0
 *   timing       nothing before the reflections delay; the late reverb's energy starts after reflections + reverb delay
 *   levels       reverb -600 mB has a quarter of the late energy of 0 mB (+-1.5 dB); room -10000 is silence; the late
 *                reverb's whole energy is its level (0 mB: the impulse's energy) +-1.5 dB from a 2 m room to a 100 m
 *                space and from 0.5 s to 4 s
 *   stereo       left and right differ (correlation below 0.5)
 * Build: clang -O2 -I runtime/win32 tests/eax_reverb_test.c runtime/win32/eax_reverb.c -o /tmp/eax_reverb_test */
#include "eax_reverb.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RATE 44100
static int fails;
static void check(int ok, const char *what, const char *fmt, double a, double b)
{
    printf("%s %-11s ", ok ? "ok  " : "FAIL", what); printf(fmt, a, b); putchar('\n'); if (!ok) fails++;
}
static EaxrParams cave(void) { return (EaxrParams){-1000, -100, 2.91f, 1.0f, -602, 0.015f, 0, 0.022f, 14.6f, 1.0f}; }
/* the stereo impulse response, n frames */
static float *impulse(EaxrParams p, int n)
{
    EaxReverb *r = eaxr_new(RATE); eaxr_set(r, &p);
    float *in = calloc((size_t)n, sizeof(float)), *out = calloc((size_t)n * 2, sizeof(float)); in[0] = 1;
    for (int at = 0; at < n; at += 512) eaxr_process(r, in + at, out + 2 * at, n - at < 512 ? n - at : 512);
    free(in); eaxr_free(r); return out;
}
/* the decay time from the energy decay curve of a mono signal, between -5 and -35 dB */
static double t60(const float *x, int n, int from)
{
    double *e = calloc((size_t)n, sizeof(double)), acc = 0;
    for (int i = n - 1; i >= from; i--) { acc += (double)x[i] * x[i]; e[i] = acc; }
    int a = -1, b = -1;
    for (int i = from; i < n; i++) {
        double db = 10 * log10(e[i] / e[from] + 1e-30);
        if (a < 0 && db <= -5) a = i;
        if (b < 0 && db <= -35) { b = i; break; }
    }
    free(e);
    return a < 0 || b < 0 ? -1 : (double)(b - a) / RATE * 60.0 / 30.0;
}
int main(void)
{
    int n = RATE * 8;
    {   /* decay: several times */
        float decays[3] = {0.8f, 2.91f, 5.0f};
        for (int k = 0; k < 3; k++) {
            EaxrParams p = cave(); p.decay = decays[k]; float *o = impulse(p, n);
            float *m = malloc(sizeof(float) * (size_t)n); for (int i = 0; i < n; i++) m[i] = o[2 * i] + o[2 * i + 1];
            double t = t60(m, n, (int)(0.05 * RATE));
            char what[32]; snprintf(what, sizeof what, "decay %.2f", decays[k]);
            check(t > decays[k] * 0.8 && t < decays[k] * 1.2, what, "measured %.2f s for %.2f s", t, decays[k]);
            free(m); free(o);
        }
    }
    {   /* HF ratio: the treble's decay time against the whole's */
        double ts[2]; float ratios[2] = {1.0f, 0.3f};
        for (int k = 0; k < 2; k++) {
            EaxrParams p = cave(); p.decay_hf_ratio = ratios[k]; float *o = impulse(p, n);
            float *d = malloc(sizeof(float) * (size_t)n); d[0] = 0;
            for (int i = 1; i < n; i++) d[i] = (o[2 * i] + o[2 * i + 1]) - (o[2 * i - 2] + o[2 * i - 1]);
            ts[k] = t60(d, n, (int)(0.05 * RATE)); free(d); free(o);
        }
        check(ts[1] > 0 && ts[1] < ts[0] * 0.5, "hf ratio", "treble decay %.2f s at ratio 0.3, %.2f s at 1.0", ts[1], ts[0]);
    }
    {   /* timing */
        EaxrParams p = cave(); p.reflections_delay = 0.05f; p.reverb_delay = 0.03f; float *o = impulse(p, RATE);
        int first = -1; for (int i = 0; i < RATE && first < 0; i++) if (fabsf(o[2 * i]) + fabsf(o[2 * i + 1]) > 1e-7f) first = i;
        check(first >= (int)(0.05 * RATE) && first <= (int)(0.05 * RATE) + 3, "timing", "first output at %.4f s (reflections delay %.4f s)", (double)first / RATE, 0.05);
        free(o);
        p.reflections = -10000; o = impulse(p, RATE);
        first = -1; for (int i = 0; i < RATE && first < 0; i++) if (fabsf(o[2 * i]) + fabsf(o[2 * i + 1]) > 1e-7f) first = i;
        check(first >= (int)(0.08 * RATE), "timing", "late reverb from %.4f s (reflections + reverb delay %.4f s)", (double)first / RATE, 0.08);
        free(o);
    }
    {   /* levels */
        double en[2]; float lv[2] = {0, -600};
        for (int k = 0; k < 2; k++) {
            EaxrParams p = cave(); p.reverb = lv[k]; p.reflections = -10000; float *o = impulse(p, n);
            double e = 0; for (int i = 0; i < 2 * n; i++) e += (double)o[i] * o[i]; en[k] = e; free(o);
        }
        double db = 10 * log10(en[1] / en[0]);
        check(fabs(db + 6) < 1.5, "levels", "reverb -600 mB: %.2f dB of the 0 mB energy (expected %.0f)", db, -6.0);
        double worst = 0; float ds[3] = {0.5f, 1.5f, 4.0f}, ss[3] = {2, 7.5f, 100};
        for (int a = 0; a < 3; a++) for (int b = 0; b < 3; b++) {
            EaxrParams q = {0, 0, ds[a], 1.0f, -10000, 0.01f, 0, 0.01f, ss[b], 1.0f}; int m = RATE * 25; float *o = impulse(q, m);
            double e = 0; for (int i = 0; i < 2 * m; i++) e += (double)o[i] * o[i]; free(o);
            if (fabs(10 * log10(e)) > fabs(worst)) worst = 10 * log10(e);
        }
        check(fabs(worst) < 1.5, "levels", "late energy at 0 mB: %+.2f dB at worst over 9 rooms (limit %.1f)", worst, 1.5);
        EaxrParams p = cave(); p.room = -10000; float *o = impulse(p, RATE);
        double e = 0; for (int i = 0; i < 2 * RATE; i++) e += (double)o[i] * o[i];
        check(e == 0, "levels", "room -10000 mB: energy %g (expected %g)", e, 0.0); free(o);
    }
    {   /* stereo */
        float *o = impulse(cave(), RATE * 2); double lr = 0, ll = 0, rr = 0;
        for (int i = (int)(0.05 * RATE); i < RATE * 2; i++) { lr += (double)o[2 * i] * o[2 * i + 1]; ll += (double)o[2 * i] * o[2 * i]; rr += (double)o[2 * i + 1] * o[2 * i + 1]; }
        double corr = lr / sqrt(ll * rr + 1e-30);
        check(fabs(corr) < 0.5 && ll > 0 && rr > 0, "stereo", "left/right correlation %.3f (limit %.1f)", corr, 0.5); free(o);
    }
    printf(fails ? "%d FAILED\n" : "all passed%.0d\n", fails);
    return fails != 0;
}
