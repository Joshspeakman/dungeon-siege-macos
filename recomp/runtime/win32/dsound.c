/* DirectSound on Core Audio: a primary buffer that gates output (and carries the 3D listener), and secondary buffers
 * that are rings in guest memory, mixed by the Core Audio render callback (any PCM rate/format, volume in 1/100 dB,
 * pan). Play and write cursors advance with the device.
 *
 * The Miles EAX providers enable environmental audio only on a device with hardware
 * 3D buffers (at least 16) whose 3D buffers answer EAX through IKsPropertySet. So the device reports 3D hardware, 3D
 * buffers are placed relative to the listener (inverse-distance rolloff between their minimum and maximum distance, an
 * equal-power pan from the listener's right axis), and EAX 2.0 (listener and buffer properties) and EAX 1.0 (reverb)
 * are kept and rendered by the reverb below.
 *
 * Each buffer's guest object holds four interface pointers, each followed by the buffer's index: +0 the buffer, +8 its
 * IDirectSound3DBuffer (the primary buffer's: IDirectSound3DListener), +16 its IKsPropertySet, +24 spare. */
#include "w32.h"
#include "eax_reverb.h"
#include <AudioToolbox/AudioToolbox.h>
#include <math.h>
#include <stdatomic.h>

enum { DS_OK = 0, DSERR_INVALIDPARAM = 0x80070057u, DSERR_NOINTERFACE = 0x80004002u, DSERR_UNSUPPORTED = 0x80004001u,
       DSERR_GENERIC = 0x80004005u, DSBCAPS_PRIMARYBUFFER = 1, DSBPLAY_LOOPING = 1, DSBSTATUS_PLAYING = 1, DSBSTATUS_LOOPING = 4,
       DSBLOCK_FROMWRITECURSOR = 1, DSBLOCK_ENTIREBUFFER = 2, OUT_RATE = 44100 };
/* EAX 2.0 property layouts (as the game passes them) */
typedef struct { int32_t room, room_hf; float room_rolloff, decay, decay_hf_ratio; int32_t reflections; float reflections_delay;
                 int32_t reverb; float reverb_delay; uint32_t environment; float env_size, env_diffusion, air_absorption_hf; uint32_t flags; } EaxListener;
typedef struct { int32_t direct, direct_hf, room, room_hf; float room_rolloff; int32_t obstruction; float obstruction_lf;
                 int32_t occlusion; float occlusion_lf, occlusion_room; int32_t outside_hf; float air_absorption; uint32_t flags; } EaxBuffer;
_Static_assert(sizeof(EaxListener) == 56 && sizeof(EaxBuffer) == 52, "EAX 2.0 layouts");
typedef struct { float pos[3], vel[3]; uint32_t cone[2]; float coneor[3]; int32_t conevol; float mind, maxd; uint32_t mode; } Spatial;
_Static_assert(sizeof(Spatial) == 60, "3D ABI");
typedef struct Buf {
    int used, primary, refs; uint32_t obj, mem, size, flags;           /* guest object, guest ring, bytes */
    int ch, bits, rate, align; double gain_l, gain_r; int32_t vol, pan; uint32_t freq;
    _Atomic uint32_t play; double frac; _Atomic int playing, looping;
    int *mem_refs;                                                     /* buffers sharing mem (DuplicateSoundBuffer) */
    int is3d, spatial_dirty; Spatial spatial, spatial_next;
    EaxBuffer eax, eax_next; int eax_dirty; float eax1_mix;                                     /* EAX 2.0 buffer properties; EAX 1.0 reverb mix */
    float lp_l, lp_r, lp_send;                                         /* the direct path's and the room send's HF filters */
} Buf;
#define NBUF 512
static Buf bufs[NBUF]; static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
typedef struct { float pos[3], vel[3], front[3], top[3], dist, rolloff, doppler; } Listener;
static Listener lis = {{0}, {0}, {0, 0, 1}, {0, 1, 0}, 1, 1, 1};
static Listener lis_next; static int lis_dirty;
static EaxListener eax_next; static int eax_dirty;
static EaxListener eax;                                                /* the environment (eax_defaults) */
static EaxReverb *reverb; static _Atomic int eax_used;                 /* the room; whether the game uses EAX at all */
#define SLICE 8192
static float send[SLICE];                                              /* the room's input: the 3D sounds sent to it */
static uint32_t vt_ds, vt_buf, vt_3d, vt_lis, vt_ks, ds_obj; static int ds_refs;
static AudioComponentInstance au; static _Atomic int primary_playing;
static _Atomic uint32_t slice_max = 512;                  /* largest render the device asked for: the write cursor's lead */

static void audio_unlock(pthread_mutex_t **m) { pthread_mutex_unlock(*m); }
#define AUDIO_GUARD pthread_mutex_t *audio_guard __attribute__((cleanup(audio_unlock))) = &lock; pthread_mutex_lock(audio_guard)

static Buf *B(uint32_t obj)                            /* any of a buffer's interface pointers -> the buffer */
{
    uint32_t k = rt_r32(G_MEM, obj + 4);
    return k < NBUF && bufs[k].used && obj >= bufs[k].obj && obj - bufs[k].obj < 32 && !((obj - bufs[k].obj) & 7) ? &bufs[k] : 0;
}
static float F(uint32_t v) { float f; memcpy(&f, &v, 4); return f; }
/* Cone interpolation and Doppler use the same source/listener geometry as panning. */
static double cone_fraction(const Buf *b)
{
    if (!b->is3d || b->spatial.mode==2 || b->spatial.cone[0]>=360) return 0;
    double dot=0,d2=0,o2=0;
    for(int k=0;k<3;k++) {double r=b->spatial.mode==1?-b->spatial.pos[k]:lis.pos[k]-b->spatial.pos[k]; dot+=r*b->spatial.coneor[k];d2+=r*r;o2+=(double)b->spatial.coneor[k]*b->spatial.coneor[k];}
    if(d2<1e-12 || o2<1e-12) return 0;
    double angle=acos(fmax(-1,fmin(1,dot/sqrt(d2*o2))))*360/M_PI;
    double inner=b->spatial.cone[0],outer=b->spatial.cone[1];
    return angle<=inner?0:angle>=outer?1:(angle-inner)/(outer-inner);
}
static double doppler(const Buf *b)
{
    if(!b->is3d || b->spatial.mode==2 || lis.doppler<=0)return 1;
    double r[3],d=0,vl=0,vs=0;
    for(int k=0;k<3;k++){r[k]=b->spatial.pos[k]-(b->spatial.mode==1?0:lis.pos[k]);d+=r[k]*r[k];}
    if(d<1e-12)return 1;d=sqrt(d);
    for(int k=0;k<3;k++){vs+=b->spatial.vel[k]*r[k]/d; if(b->spatial.mode!=1)vl+=lis.vel[k]*r[k]/d;}
    double scale=fmax(0,lis.dist)*lis.doppler,c=343.3;
    vl=fmax(-.9*c,fmin(.9*c,vl*scale));vs=fmax(-.9*c,fmin(.9*c,vs*scale));
    return fmax(.125,fmin(8,(c+vl)/(c+vs)));
}
/* the 3D gains of a buffer: distance attenuation and pan relative to the listener; *dist gets the distance */
static void gains3d(const Buf *b, double *gl, double *gr, double *dist)
{
    *gl = b->gain_l; *gr = b->gain_r; *dist = 0;
    if (!b->is3d || b->spatial.mode == 2) return;                                  /* DS3DMODE_DISABLE */
    float r[3];
    for (int k = 0; k < 3; k++) r[k] = b->spatial.mode == 1 ? b->spatial.pos[k] : b->spatial.pos[k] - lis.pos[k];   /* HEADRELATIVE */
    double d = sqrt((double)r[0] * r[0] + (double)r[1] * r[1] + (double)r[2] * r[2]), mn = b->spatial.mind > 0 ? b->spatial.mind : 1, mx = b->spatial.maxd > mn ? b->spatial.maxd : mn;
    double dd = d < mn ? mn : d > mx ? mx : d, att = mn / (mn + lis.rolloff * (dd - mn));
    *dist = d;
    if ((b->flags & 0x20000) && d >= mx) att=0; /* MUTE3DATMAXDISTANCE */
    att *= pow(10.0, b->spatial.conevol * cone_fraction(b) / 2000.0);
    const float *f = lis.front, *t = lis.top;
    double rx = t[1] * f[2] - t[2] * f[1], ry = t[2] * f[0] - t[0] * f[2], rz = t[0] * f[1] - t[1] * f[0];      /* right = top x front */
    double rl = sqrt(rx * rx + ry * ry + rz * rz), side = d > 1e-6 && rl > 1e-6 ? (r[0] * rx + r[1] * ry + r[2] * rz) / (d * rl) : 0;
    if (b->spatial.mode==1) side=d>1e-6?r[0]/d:0;
    if (side > 1) side = 1; if (side < -1) side = -1;
    double a = (side + 1) * 0.7853981633974483;                            /* equal power: left cos, right sin, 1/sqrt2 each in the centre */
    *gl *= att * cos(a) * 1.4142135623730951; *gr *= att * sin(a) * 1.4142135623730951;
    if (*gl > b->gain_l * att) *gl = b->gain_l * att; if (*gr > b->gain_r * att) *gr = b->gain_r * att;
}
/* ---- EAX state ----
 * The 26 standard environments as EAX 2.0 listener settings (size m, decay s, decay HF ratio, room HF mB, reflections
 * mB, reflections delay s, reverb mB, reverb delay s, diffusion); room -1000 mB for all. Approximations of the published
 * presets: the game sets its own values per area (through Miles) after choosing one. */
static const float PRESETS[26][9] = {
    {7.5f, 1.49f, 0.83f, -100, -2602, 0.007f, 200, 0.011f, 1.0f},     {1.4f, 0.17f, 0.10f, -6000, -1204, 0.001f, 207, 0.002f, 1.0f},
    {1.9f, 0.40f, 0.83f, -454, -1646, 0.002f, 53, 0.003f, 1.0f},      {1.4f, 1.49f, 0.54f, -1200, -370, 0.007f, 1030, 0.011f, 1.0f},
    {2.5f, 0.50f, 0.10f, -6000, -1376, 0.003f, -1104, 0.004f, 1.0f},  {11.6f, 2.31f, 0.64f, -300, -711, 0.012f, 83, 0.017f, 1.0f},
    {21.6f, 4.32f, 0.59f, -476, -789, 0.020f, -289, 0.030f, 1.0f},    {19.6f, 3.92f, 0.70f, -500, -1230, 0.020f, -2, 0.029f, 1.0f},
    {14.6f, 2.91f, 1.30f, 0, -602, 0.015f, -302, 0.022f, 1.0f},       {36.2f, 7.24f, 0.33f, -698, -1166, 0.020f, 16, 0.030f, 1.0f},
    {50.3f, 10.05f, 0.23f, -1000, -602, 0.020f, 198, 0.030f, 1.0f},   {1.9f, 0.30f, 0.10f, -4000, -1831, 0.002f, -1630, 0.030f, 1.0f},
    {1.8f, 1.49f, 0.59f, -300, -1219, 0.007f, 441, 0.011f, 1.0f},     {13.5f, 2.70f, 0.79f, -237, -1214, 0.013f, 395, 0.020f, 1.0f},
    {7.5f, 1.49f, 0.86f, -270, -1204, 0.007f, -4, 0.011f, 0.3f},      {38.0f, 1.49f, 0.54f, -3300, -2560, 0.162f, -229, 0.088f, 0.3f},
    {7.5f, 1.49f, 0.67f, -800, -2273, 0.007f, -1691, 0.011f, 0.5f},   {100.0f, 1.49f, 0.21f, -2500, -2780, 0.300f, -1434, 0.100f, 0.27f},
    {17.5f, 1.49f, 0.83f, -1000, -10000, 0.061f, 500, 0.025f, 1.0f},  {42.5f, 1.49f, 0.50f, -2000, -2466, 0.179f, -1926, 0.100f, 0.21f},
    {8.3f, 1.65f, 1.50f, 0, -1363, 0.008f, -1153, 0.012f, 1.0f},      {1.7f, 2.81f, 0.14f, -1000, 429, 0.014f, 1023, 0.021f, 0.8f},
    {1.8f, 1.49f, 0.10f, -4000, -449, 0.007f, 1700, 0.011f, 1.0f},    {1.9f, 8.39f, 1.39f, 0, -115, 0.002f, 985, 0.030f, 0.5f},
    {1.8f, 17.23f, 0.56f, -400, -1713, 0.020f, -613, 0.030f, 0.6f},   {1.0f, 7.56f, 0.91f, -151, -626, 0.020f, 774, 0.030f, 0.5f}};
static float eax_size_prev;
static void eax_changed(void);
static void eax_environment(uint32_t env)                 /* an environment's preset; the flags and air absorption kept */
{
    if (env > 25) env = 0;
    const float *p = PRESETS[env]; uint32_t fl = eax.flags ? eax.flags : 0x3f; float air = eax.air_absorption_hf ? eax.air_absorption_hf : -5.0f;
    eax = (EaxListener){-1000, (int32_t)p[3], 0, p[1], p[2], (int32_t)p[4], p[5], (int32_t)p[6], p[7], env, p[0], p[8], air, fl};
    eax_size_prev = p[0];
}
static void eax_size_changed(void)                        /* EAX 2.0: a new size scales what the flags say, as hardware does */
{
    if (eax_size_prev <= 0 || eax.env_size <= 0) return;
    float r = eax.env_size / eax_size_prev;
    if (eax.flags & 0x01) eax.decay *= r;                                                          /* DECAYTIMESCALE */
    if (eax.flags & 0x02) eax.reflections -= (int32_t)(2000 * log10f(r));                        /* REFLECTIONSSCALE */
    if (eax.flags & 0x04) eax.reflections_delay *= r;
    if (eax.flags & 0x08) eax.reverb -= (int32_t)(2000 * log10f(r));                             /* REVERBSCALE */
    if (eax.flags & 0x10) eax.reverb_delay *= r;
}
/* EAX 1.0 {environment, volume 0-1, decay time s, damping}: the reverb's level, its decay, and its decay HF ratio */
static void eax1_get(float v[4])
{
    v[0] = 0; v[1] = eax.room <= -10000 ? 0 : powf(10, eax.room / 2000.0f); v[2] = eax.decay; v[3] = eax.decay_hf_ratio;
}
static void eax1_set(const float v[4])
{
    eax.room = v[1] <= 0.0001f ? -10000 : (int32_t)(2000 * log10f(v[1] > 1 ? 1 : v[1]));
    if (v[2] >= 0.1f) eax.decay = v[2] > 20 ? 20 : v[2];
    if (v[3] > 0) eax.decay_hf_ratio = v[3] < 0.1f ? 0.1f : v[3] > 2 ? 2 : v[3];
}
static void eax_buffer_defaults(Buf *b)
{
    b->eax = (EaxBuffer){0, 0, 0, 0, 0, 0, 0, 0, 0.25f, 0.5f, 0, 1.0f, 7}; b->eax1_mix = -1;     /* EAX 1.0: -1 = automatic */
}
static void set_gain(Buf *b)
{
    double v = b->vol <= -10000 ? 0 : pow(10.0, b->vol / 2000.0);                         /* 1/100 dB */
    double l = b->pan > 0 ? pow(10.0, -b->pan / 2000.0) : 1, r = b->pan < 0 ? pow(10.0, b->pan / 2000.0) : 1;
    b->gain_l = v * l; b->gain_r = v * r;
}
static inline float sample(Buf *b, uint32_t frame, int chn)
{
    uint32_t at = frame * b->align + (b->ch == 2 ? chn : 0) * (b->bits / 8);
    return b->bits == 16 ? (int16_t)rt_r16(G_MEM, b->mem + at) / 32768.0f : ((int)G_MEM[b->mem + at] - 128) / 128.0f;
}
void w32_audio_out(const float *lr, uint32_t frames, int rate, int source);         /* winmm.c: mute, level log, dump */
static OSStatus render(void *u, AudioUnitRenderActionFlags *fl, const AudioTimeStamp *ts, UInt32 bus, UInt32 frames, AudioBufferList *io)
{
    (void)u; (void)fl; (void)ts; (void)bus;
    float *out = io->mBuffers[0].mData; memset(out, 0, frames * 8);
    if (pthread_mutex_trylock(&lock)) return 0;
    if (frames > slice_max) slice_max = frames;
    if (frames <= SLICE) memset(send, 0, frames * sizeof(float));
    if (primary_playing) for (int k = 0; k < NBUF; k++) {
        Buf *b = &bufs[k]; if (!b->used || b->primary || !b->playing) continue;
        double gl, gr, dist; gains3d(b, &gl, &gr, &dist);
        /* EAX: the direct path's level and HF loss (direct, obstruction, occlusion, air), and what goes to the room */
        float da = 0, sa = 0, sg = 0;
        if (b->is3d && eax_used && reverb && frames <= SLICE) {
            const EaxBuffer *e = &b->eax; const float w = 2 * (float)M_PI * 5000 / OUT_RATE;
            float lf = e->direct + e->obstruction * e->obstruction_lf + e->occlusion * e->occlusion_lf;
            float hf = e->direct_hf + e->obstruction * (1 - e->obstruction_lf) + e->occlusion * (1 - e->occlusion_lf)
                       + e->outside_hf * (float)cone_fraction(b);
            float air = eax.air_absorption_hf * e->air_absorption * (float)fmax(0,dist-b->spatial.mind) * fmaxf(0,lis.dist);
            if(e->flags & 1) hf += air;
            double dg = lf <= -10000 ? 0 : pow(10.0, lf / 2000.0); gl *= dg; gr *= dg;
            da = eaxr_onepole(hf >= 0 ? 1 : (float)pow(10.0, hf / 2000.0), w);
            float room = e->room + e->occlusion * e->occlusion_lf * e->occlusion_room, rolloff = eax.room_rolloff + e->room_rolloff;
            double mn = b->spatial.mind > 0 ? b->spatial.mind : 1, dd = dist < mn ? mn : dist, ratt = mn / (mn + rolloff * (dd - mn));
            if (e->flags & 2) ratt *= sqrt(mn/dd); /* automatic room attenuation */
            if (b->eax1_mix >= 0) room = b->eax1_mix <= 0.0001f ? -10000 : 2000 * log10f(b->eax1_mix);   /* EAX 1.0's reverb mix */
            sg = room <= -10000 ? 0 : (float)(pow(10.0, room / 2000.0) * ratt * 0.5 * (b->gain_l + b->gain_r));
            float roomhf=e->room_hf+e->occlusion*(1-e->occlusion_lf)*e->occlusion_room;
            if(e->flags & 4)roomhf+=air;
            sa = eaxr_onepole(roomhf >= 0 ? 1 : (float)pow(10.0, roomhf / 2000.0), w);
        }
        uint32_t nframes = b->size / b->align, pos = b->play / b->align; double step = (double)(b->freq ? b->freq : (uint32_t)b->rate) / OUT_RATE * doppler(b), fr = b->frac;
        for (UInt32 i = 0; i < frames; i++) {
            uint32_t p0 = pos, p1 = pos + 1; float t = (float)fr;
            if (!b->looping && p1 >= nframes) p1 = p0;
            /* Share wrapped interpolation endpoints between channels. */
            if (p0 >= nframes) p0 %= nframes;
            if (p1 >= nframes) p1 %= nframes;
            float l = sample(b, p0, 0) * (1 - t) + sample(b, p1, 0) * t, r = sample(b, p0, 1) * (1 - t) + sample(b, p1, 1) * t;
            float dry_mono = 0.5f * (l + r);
            if (da > 0) { b->lp_l = (1 - da) * l + da * b->lp_l; b->lp_r = (1 - da) * r + da * b->lp_r; l = b->lp_l; r = b->lp_r; }
            if (sg > 0) { float m = dry_mono; b->lp_send = (1 - sa) * m + sa * b->lp_send; send[i] += b->lp_send * sg; }
            out[2 * i] += (float)(l * gl); out[2 * i + 1] += (float)(r * gr);
            fr += step; while (fr >= 1) { fr -= 1; pos++; }
            if (pos >= nframes) { if (b->looping) pos %= nframes; else { b->playing = 0; pos = 0; break; } }
        }
        b->frac = fr; b->play = pos * b->align;
    }
    if (reverb && eax_used && frames <= SLICE) {                                  /* the room */
        static int lg = -1; static double se, re, n; if (lg < 0) lg = getenv("W32_EAXLOG") != 0;
        if (lg) { for (UInt32 i = 0; i < frames; i++) { se += (double)send[i] * send[i]; re -= (double)out[2 * i] * out[2 * i] + (double)out[2 * i + 1] * out[2 * i + 1]; } }
        eaxr_process(reverb, send, out, (int)frames);
        if (lg) {
            for (UInt32 i = 0; i < frames; i++) re += (double)out[2 * i] * out[2 * i] + (double)out[2 * i + 1] * out[2 * i + 1];
            if ((n += frames) >= OUT_RATE) { fprintf(stderr, "dsound: room send rms %.4f, energy added by the room %.5f (environment %u, room %d mB, decay %.2f s)\n",
                                                     sqrt(se / n), re / n, eax.environment, eax.room, eax.decay); se = re = n = 0; }
        }
    }
    pthread_mutex_unlock(&lock);
    w32_audio_out(out, frames, OUT_RATE, 1);
    if (getenv("W32_AUDIO_MUTE")) memset(out, 0, frames * 8);
    return 0;
}
static int start_output(void)
{
    if (au) return 0;
    AudioComponentDescription d = {kAudioUnitType_Output, kAudioUnitSubType_DefaultOutput, kAudioUnitManufacturer_Apple, 0, 0};
    AudioComponent comp = AudioComponentFindNext(0, &d);
    if (!comp || AudioComponentInstanceNew(comp, &au)) return -1;
    AudioStreamBasicDescription f = {OUT_RATE, kAudioFormatLinearPCM, kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked, 8, 1, 8, 2, 32, 0};
    AudioUnitSetProperty(au, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &f, sizeof f);
    UInt32 maxf = 4096; AudioUnitSetProperty(au, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0, &maxf, sizeof maxf);   /* Bluetooth/USB devices render in big slices */
    AURenderCallbackStruct rc = {render, 0};
    AudioUnitSetProperty(au, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &rc, sizeof rc);
    if (AudioUnitInitialize(au) || AudioOutputUnitStart(au)) { AudioComponentInstanceDispose(au); au = 0; return -1; }
    return 0;
}
static void set_format(Buf *b, uint32_t wfx)
{
    b->ch = rt_r16(G_MEM, wfx + 2); b->rate = (int)rt_r32(G_MEM, wfx + 4); b->bits = rt_r16(G_MEM, wfx + 14);
    b->align = b->ch * b->bits / 8; if (b->align <= 0) b->align = 4;
}
#define LOG(...) do { if (getenv("W32_DSLOG")) fprintf(stderr, "dsound: " __VA_ARGS__); } while (0)

/* ---- IDirectSoundBuffer ---- */
static int guid_is(uint32_t g, uint32_t d1, uint16_t d2, uint16_t d3, const uint8_t d4[8])
{
    return rt_r32(G_MEM, g) == d1 && rt_r16(G_MEM, g + 4) == d2 && rt_r16(G_MEM, g + 6) == d3 && !memcmp(GP(g + 8), d4, 8);
}
static void b_QueryInterface(Ctx *c)                   /* the buffer, its 3D interface (the listener on the primary), its property set */
{ AUDIO_GUARD;
    Buf *b = B(ARG(0)); uint32_t iid = rt_r32(G_MEM, ARG(1)), out = ARG(2), r = 0;
    if (!b || !out) RET(DSERR_INVALIDPARAM, 3);
    if (iid == 0 || iid == 0x279afa85u || iid == 0x6825a449u) r = b->obj;                       /* IUnknown, IDirectSoundBuffer(8) */
    else if (iid == 0x279afa86u && !b->primary && b->is3d) r = b->obj + 8;                       /* IDirectSound3DBuffer */
    else if (iid == 0x279afa84u && b->primary && b->is3d) r = b->obj + 8;                        /* IDirectSound3DListener */
    else if (iid == 0x31efac30u) r = b->obj + 16;                                                 /* IKsPropertySet */
    LOG("buffer QueryInterface %08x -> %s\n", iid, r ? "ok" : "no interface");
    rt_w32(G_MEM, out, r); if (r) b->refs++;
    RET(r ? DS_OK : DSERR_NOINTERFACE, 3);
}
static void b_AddRef(Ctx *c) { AUDIO_GUARD; Buf *b = B(ARG(0)); RET(b ? ++b->refs : 0, 1); }
static void b_Release(Ctx *c)
{ AUDIO_GUARD;
    Buf *b = B(ARG(0)); if (!b) RET(0, 1);
    int r = --b->refs;
    if (!r) {
        b->playing = 0; b->used = 0;
        if (b->mem_refs && --*b->mem_refs == 0) { free(b->mem_refs); if (b->mem) heap_free(w32_process_heap, b->mem); }   /* the last user of the samples */
        b->mem_refs = 0;
        LOG("release buffer\n");
    }
    RET(r, 1);
}
static void b_GetCaps(Ctx *c)
{ AUDIO_GUARD;
    Buf *b = B(ARG(0)); uint32_t p = ARG(1); if (!b) RET(DSERR_INVALIDPARAM, 2);
    rt_w32(G_MEM, p + 4, b->flags | (b->flags & 0x4 ? 0 : 0x8) /* where it was asked for: LOCHARDWARE, else LOCSOFTWARE */);
    rt_w32(G_MEM, p + 8, b->size); rt_w32(G_MEM, p + 12, 0); rt_w32(G_MEM, p + 16, 0);
    RET(DS_OK, 2);
}
static uint32_t write_ahead(Buf *b) { return (uint32_t)((uint64_t)(b->freq ? b->freq : (uint32_t)b->rate) * slice_max / OUT_RATE + 1) * b->align; }
static void b_GetCurrentPosition(Ctx *c)
{ AUDIO_GUARD;
    Buf *b = B(ARG(0)); if (!b) RET(DSERR_INVALIDPARAM, 3);
    uint32_t p = b->primary ? 0 : b->play, w = b->primary ? 0 : (p + write_ahead(b)) % b->size;
    if (ARG(1)) rt_w32(G_MEM, ARG(1), p); if (ARG(2)) rt_w32(G_MEM, ARG(2), w);
    RET(DS_OK, 3);
}
static void b_GetFormat(Ctx *c)
{ AUDIO_GUARD;
    Buf *b = B(ARG(0)); uint32_t p = ARG(1), cap = ARG(2); if (!b) RET(DSERR_INVALIDPARAM, 4);
    uint8_t w[18] = {0}; uint16_t v16; uint32_t v32;
    v16 = 1; memcpy(w, &v16, 2); v16 = (uint16_t)b->ch; memcpy(w + 2, &v16, 2); v32 = (uint32_t)b->rate; memcpy(w + 4, &v32, 4);
    v32 = (uint32_t)(b->rate * b->align); memcpy(w + 8, &v32, 4); v16 = (uint16_t)b->align; memcpy(w + 12, &v16, 2); v16 = (uint16_t)b->bits; memcpy(w + 14, &v16, 2);
    if (p) memcpy(GP(p), w, cap < 18 ? cap : 18);
    if (ARG(3)) rt_w32(G_MEM, ARG(3), 18);
    RET(DS_OK, 4);
}
static void b_GetVolume(Ctx *c) { AUDIO_GUARD; Buf *b = B(ARG(0)); if (!b) RET(DSERR_INVALIDPARAM, 2); rt_w32(G_MEM, ARG(1), (uint32_t)b->vol); RET(DS_OK, 2); }
static void b_GetPan(Ctx *c) { AUDIO_GUARD; Buf *b = B(ARG(0)); if (!b) RET(DSERR_INVALIDPARAM, 2); rt_w32(G_MEM, ARG(1), (uint32_t)b->pan); RET(DS_OK, 2); }
static void b_GetFrequency(Ctx *c) { AUDIO_GUARD; Buf *b = B(ARG(0)); if (!b) RET(DSERR_INVALIDPARAM, 2); rt_w32(G_MEM, ARG(1), b->freq ? b->freq : (uint32_t)b->rate); RET(DS_OK, 2); }
static void b_GetStatus(Ctx *c)
{ AUDIO_GUARD;
    Buf *b = B(ARG(0)); if (!b) RET(DSERR_INVALIDPARAM, 2);
    int pl = b->primary ? primary_playing : b->playing;
    rt_w32(G_MEM, ARG(1), (pl ? DSBSTATUS_PLAYING : 0) | (pl && b->looping ? DSBSTATUS_LOOPING : 0)); RET(DS_OK, 2);
}
static void b_Initialize(Ctx *c) { AUDIO_GUARD; RET(0x88780082u /* DSERR_ALREADYINITIALIZED */, 3); }
static void b_Lock(Ctx *c)
{ AUDIO_GUARD;
    Buf *b = B(ARG(0)); uint32_t off = ARG(1), n = ARG(2), p1 = ARG(3), l1 = ARG(4), p2 = ARG(5), l2 = ARG(6), fl = ARG(7);
    if (!b || b->primary || !b->size) RET(DSERR_INVALIDPARAM, 8);
    if (fl & DSBLOCK_FROMWRITECURSOR) off = (b->play + write_ahead(b)) % b->size;
    if (fl & DSBLOCK_ENTIREBUFFER) n = b->size;
    if (n > b->size || off >= b->size) RET(DSERR_INVALIDPARAM, 8);
    uint32_t a = b->size - off < n ? b->size - off : n;
    rt_w32(G_MEM, p1, b->mem + off); rt_w32(G_MEM, l1, a);
    if (p2) rt_w32(G_MEM, p2, n > a ? b->mem : 0); if (l2) rt_w32(G_MEM, l2, n - a);
    RET(DS_OK, 8);
}
static void b_Play(Ctx *c)
{ AUDIO_GUARD;
    Buf *b = B(ARG(0)); if (!b) RET(DSERR_INVALIDPARAM, 4);
    if (start_output()) RET(DSERR_GENERIC, 4);
    b->looping = (ARG(3) & DSBPLAY_LOOPING) != 0;
    if (b->primary) primary_playing = 1; else b->playing = 1;
    LOG("play %s looping %d\n", b->primary ? "primary" : "secondary", b->looping);
    RET(DS_OK, 4);
}
static void b_SetCurrentPosition(Ctx *c) { AUDIO_GUARD; Buf *b = B(ARG(0)); if (!b || b->primary) RET(DSERR_INVALIDPARAM, 2); b->play = ARG(1) % (b->size ? b->size : 1) / b->align * b->align; b->frac = 0; RET(DS_OK, 2); }
static void b_SetFormat(Ctx *c)
{ AUDIO_GUARD;
    Buf *b = B(ARG(0)); if (!b) RET(DSERR_INVALIDPARAM, 2);
    set_format(b, ARG(1)); LOG("format %d Hz %d bits %d ch (%s)\n", b->rate, b->bits, b->ch, b->primary ? "primary" : "secondary");
    RET(DS_OK, 2);
}
static void b_SetVolume(Ctx *c) { AUDIO_GUARD; Buf *b = B(ARG(0)); if (!b) RET(DSERR_INVALIDPARAM, 2); b->vol = (int32_t)ARG(1); set_gain(b); RET(DS_OK, 2); }
static void b_SetPan(Ctx *c) { AUDIO_GUARD; Buf *b = B(ARG(0)); if (!b) RET(DSERR_INVALIDPARAM, 2); b->pan = (int32_t)ARG(1); set_gain(b); RET(DS_OK, 2); }
static void b_SetFrequency(Ctx *c) { AUDIO_GUARD; Buf *b = B(ARG(0)); if (!b) RET(DSERR_INVALIDPARAM, 2); b->freq = ARG(1); RET(DS_OK, 2); }
static void b_Stop(Ctx *c)
{ AUDIO_GUARD;
    Buf *b = B(ARG(0)); if (!b) RET(DSERR_INVALIDPARAM, 1);
    if (b->primary) primary_playing = 0; else b->playing = 0;
    RET(DS_OK, 1);
}
static void b_Unlock(Ctx *c) { AUDIO_GUARD; RET(DS_OK, 5); }
static void b_Restore(Ctx *c) { AUDIO_GUARD; RET(DS_OK, 1); }

/* ---- IDirectSound ---- */
static void d_QueryInterface(Ctx *c) { AUDIO_GUARD; RET(DSERR_NOINTERFACE, 3); }
static void d_AddRef(Ctx *c) { AUDIO_GUARD; RET(++ds_refs, 1); }
static void d_Release(Ctx *c) { AUDIO_GUARD; int r = --ds_refs; RET(r < 0 ? 0 : r, 1); }
static uint32_t new_buf(Buf **out)
{
    int k = 0; while (k < NBUF && bufs[k].used) k++;
    if (k == NBUF) { static int once; if (!once++) fprintf(stderr, "dsound: all %d buffers in use\n", NBUF); return 0; }
    Buf *b = &bufs[k]; uint32_t keep = b->obj; memset(b, 0, sizeof *b); b->used = 1; b->refs = 1; b->spatial.mind = 1; b->spatial.maxd = 1e9f; b->spatial.cone[0]=b->spatial.cone[1]=360; b->spatial.coneor[2]=1; set_gain(b);
    eax_buffer_defaults(b);
    b->obj = keep ? keep : heap_alloc(w32_process_heap, 8, 32);       /* a slot's guest object is reused with it */
    for (uint32_t i = 0; i < 4; i++) rt_w32(G_MEM, b->obj + 8 * i + 4, (uint32_t)k);
    rt_w32(G_MEM, b->obj, vt_buf); rt_w32(G_MEM, b->obj + 8, vt_3d); rt_w32(G_MEM, b->obj + 16, vt_ks); rt_w32(G_MEM, b->obj + 24, 0);
    *out = b; return b->obj;
}
static void d_CreateSoundBuffer(Ctx *c)
{ AUDIO_GUARD;
    uint32_t desc = ARG(1), out = ARG(2), fl = rt_r32(G_MEM, desc + 4), bytes = rt_r32(G_MEM, desc + 8), wfx = rt_r32(G_MEM, desc + 16);
    Buf *b; uint32_t obj = new_buf(&b); if (!obj) RET(DSERR_GENERIC, 4);
    b->flags = fl; b->primary = (fl & DSBCAPS_PRIMARYBUFFER) != 0; b->is3d = (fl & 0x10) != 0;   /* DSBCAPS_CTRL3D */
    if (b->primary) rt_w32(G_MEM, obj + 8, vt_lis);
    b->ch = 2; b->bits = 16; b->rate = 22050; b->align = 4;
    if (wfx) set_format(b, wfx);
    if (!b->primary) {
        if (!wfx || bytes < 4) { b->used = 0; RET(DSERR_INVALIDPARAM, 4); }
        b->size = bytes / b->align * b->align; b->mem = heap_alloc(w32_process_heap, 8, b->size);
        b->mem_refs = malloc(sizeof *b->mem_refs); *b->mem_refs = 1;
        memset(GP(b->mem), b->bits == 8 ? 0x80 : 0, b->size);
    }
    LOG("CreateSoundBuffer flags %#x, %u bytes, %d Hz %d bits %d ch%s\n", fl, bytes, b->rate, b->bits, b->ch, b->primary ? " (primary)" : "");
    rt_w32(G_MEM, out, obj); RET(DS_OK, 4);
}
static void d_GetCaps(Ctx *c)
{ AUDIO_GUARD;
    uint32_t p = ARG(1); uint32_t sz = rt_r32(G_MEM, p); memset(GP(p + 4), 0, sz > 4 ? sz - 4 : 0);
    rt_w32(G_MEM, p + 4, 0x1 | 0x2 | 0x4 | 0x8 | 0x10 | 0x40 | 0x100 | 0x200 | 0x400 | 0x800);  /* primary/secondary mono/stereo 8/16 bit, continuous rate, certified */
    rt_w32(G_MEM, p + 8, 100); rt_w32(G_MEM, p + 12, 200000); rt_w32(G_MEM, p + 16, 1);
    /* hardware mixing and 3D buffers (all, static, streaming; maximum, then free): the game wants 16 3D voices */
    if (sz >= 0x44) for (uint32_t o = 0x14; o < 0x44; o += 4) rt_w32(G_MEM, p + o, 64);
    RET(DS_OK, 2);
}
static void d_DuplicateSoundBuffer(Ctx *c)                       /* (original, out): the same samples, its own cursor and volume */
{ AUDIO_GUARD;
    Buf *o = B(ARG(1)), *b; if (!o || o->primary) RET(DSERR_INVALIDPARAM, 3);
    uint32_t obj = new_buf(&b); if (!obj) RET(DSERR_GENERIC, 3);
    b->flags = o->flags; b->ch = o->ch; b->bits = o->bits; b->rate = o->rate; b->align = o->align; b->size = o->size; b->mem = o->mem;
    b->vol = o->vol; b->pan = o->pan; b->freq = o->freq; set_gain(b);
    b->is3d = o->is3d; b->spatial.mode = o->spatial.mode; memcpy(b->spatial.pos, o->spatial.pos, 12); memcpy(b->spatial.vel, o->spatial.vel, 12); b->spatial.mind = o->spatial.mind; b->spatial.maxd = o->spatial.maxd; b->spatial = o->spatial;
    b->eax = o->eax; b->eax1_mix = o->eax1_mix;
    b->mem_refs = o->mem_refs; if (b->mem_refs) ++*b->mem_refs;
    rt_w32(G_MEM, ARG(2), obj); RET(DS_OK, 3);
}
static void d_SetCooperativeLevel(Ctx *c) { AUDIO_GUARD; RET(DS_OK, 3); }
static void d_Compact(Ctx *c) { AUDIO_GUARD; RET(DS_OK, 1); }
static void d_GetSpeakerConfig(Ctx *c) { AUDIO_GUARD; rt_w32(G_MEM, ARG(1), 4 /* DSSPEAKER_STEREO */); RET(DS_OK, 2); }
static void d_SetSpeakerConfig(Ctx *c) { AUDIO_GUARD; RET(DS_OK, 2); }
static void d_Initialize(Ctx *c) { AUDIO_GUARD; RET(DS_OK, 2); }

/* DirectSound3D state is committed atomically for all sources and the listener. */
static void spatial_set(Buf *b, size_t offset, const void *v, size_t n, uint32_t defer)
{
    if (defer) {
        if (!b->spatial_dirty) b->spatial_next = b->spatial;
        memcpy((char *)&b->spatial_next + offset, v, n); b->spatial_dirty = 1;
    } else {
        memcpy((char *)&b->spatial + offset, v, n);
        if (b->spatial_dirty) memcpy((char *)&b->spatial_next + offset, v, n);
    }
}
static void listener_set(size_t offset, const void *v, size_t n, uint32_t defer)
{
    if (defer) {
        if (!lis_dirty) lis_next = lis;
        memcpy((char *)&lis_next + offset, v, n); lis_dirty = 1;
    } else {
        memcpy((char *)&lis + offset, v, n);
        if (lis_dirty) memcpy((char *)&lis_next + offset, v, n);
    }
}
static void t_GetAllParameters(Ctx *c) { AUDIO_GUARD; Buf *b=B(ARG(0)); if (!b || rt_r32(G_MEM,ARG(1))!=64) RET(DSERR_INVALIDPARAM,2); memcpy(GP(ARG(1)+4), &b->spatial,60); RET(0,2); }
static void t_SetAllParameters(Ctx *c) { AUDIO_GUARD; Buf *b=B(ARG(0)); if (!b || rt_r32(G_MEM,ARG(1))!=64 || ARG(2)>1) RET(DSERR_INVALIDPARAM,3); spatial_set(b,0,GP(ARG(1)+4),60,ARG(2)); RET(0,3); }
static void l_GetAllParameters(Ctx *c) { AUDIO_GUARD; if(rt_r32(G_MEM,ARG(1))!=64) RET(DSERR_INVALIDPARAM,2); memcpy(GP(ARG(1)+4),&lis,60); RET(0,2); }
static void l_SetAllParameters(Ctx *c) { AUDIO_GUARD; if(rt_r32(G_MEM,ARG(1))!=64 || ARG(2)>1) RET(DSERR_INVALIDPARAM,3); listener_set(0,GP(ARG(1)+4),60,ARG(2)); RET(0,3); }
static void l_CommitDeferredSettings(Ctx *c) { AUDIO_GUARD;
    if(lis_dirty){lis=lis_next;lis_dirty=0;}
    for(int k=0;k<NBUF;k++) if(bufs[k].used && bufs[k].spatial_dirty){bufs[k].spatial=bufs[k].spatial_next;bufs[k].spatial_dirty=0;}
    RET(0,1);
}
static void t_GetConeAngles(Ctx *c) { AUDIO_GUARD; Buf *b=B(ARG(0)); if(!b) RET(DSERR_INVALIDPARAM,3); rt_w32(G_MEM,ARG(1),b->spatial.cone[0]); rt_w32(G_MEM,ARG(2),b->spatial.cone[1]); RET(0,3); }
static void t_SetConeAngles(Ctx *c) { AUDIO_GUARD; Buf *b=B(ARG(0)); if(!b) RET(DSERR_INVALIDPARAM,4); if(ARG(3)>1) RET(DSERR_INVALIDPARAM,4); uint32_t v[]={ ARG(1), ARG(2) }; spatial_set(b,offsetof(Spatial,cone),v,sizeof v,ARG(3)); RET(0,4); }
static void t_GetConeOrientation(Ctx *c) { AUDIO_GUARD; Buf *b=B(ARG(0)); if(!b) RET(DSERR_INVALIDPARAM,2); memcpy(GP(ARG(1)),&b->spatial.coneor,12); RET(0,2); }
static void t_SetConeOrientation(Ctx *c) { AUDIO_GUARD; Buf *b=B(ARG(0)); if(!b) RET(DSERR_INVALIDPARAM,5); if(ARG(4)>1) RET(DSERR_INVALIDPARAM,5); float v[]={ F(ARG(1)), F(ARG(2)), F(ARG(3)) }; for(int k=0;k<3;k++) if(!isfinite(v[k])) RET(DSERR_INVALIDPARAM,5); spatial_set(b,offsetof(Spatial,coneor),v,sizeof v,ARG(4)); RET(0,5); }
static void t_GetConeOutsideVolume(Ctx *c) { AUDIO_GUARD; Buf *b=B(ARG(0)); if(!b) RET(DSERR_INVALIDPARAM,2); memcpy(GP(ARG(1)),&b->spatial.conevol,4); RET(0,2); }
static void t_SetConeOutsideVolume(Ctx *c) { AUDIO_GUARD; Buf *b=B(ARG(0)); if(!b) RET(DSERR_INVALIDPARAM,3); if(ARG(2)>1) RET(DSERR_INVALIDPARAM,3); uint32_t v[]={ ARG(1) }; spatial_set(b,offsetof(Spatial,conevol),v,sizeof v,ARG(2)); RET(0,3); }
static void t_GetMaxDistance(Ctx *c) { AUDIO_GUARD; Buf *b=B(ARG(0)); if(!b) RET(DSERR_INVALIDPARAM,2); memcpy(GP(ARG(1)),&b->spatial.maxd,4); RET(0,2); }
static void t_SetMaxDistance(Ctx *c) { AUDIO_GUARD; Buf *b=B(ARG(0)); if(!b) RET(DSERR_INVALIDPARAM,3); if(ARG(2)>1) RET(DSERR_INVALIDPARAM,3); float v[]={ F(ARG(1)) }; for(int k=0;k<1;k++) if(!isfinite(v[k])) RET(DSERR_INVALIDPARAM,3); spatial_set(b,offsetof(Spatial,maxd),v,sizeof v,ARG(2)); RET(0,3); }
static void t_GetMinDistance(Ctx *c) { AUDIO_GUARD; Buf *b=B(ARG(0)); if(!b) RET(DSERR_INVALIDPARAM,2); memcpy(GP(ARG(1)),&b->spatial.mind,4); RET(0,2); }
static void t_SetMinDistance(Ctx *c) { AUDIO_GUARD; Buf *b=B(ARG(0)); if(!b) RET(DSERR_INVALIDPARAM,3); if(ARG(2)>1) RET(DSERR_INVALIDPARAM,3); float v[]={ F(ARG(1)) }; for(int k=0;k<1;k++) if(!isfinite(v[k])) RET(DSERR_INVALIDPARAM,3); spatial_set(b,offsetof(Spatial,mind),v,sizeof v,ARG(2)); RET(0,3); }
static void t_GetMode(Ctx *c) { AUDIO_GUARD; Buf *b=B(ARG(0)); if(!b) RET(DSERR_INVALIDPARAM,2); memcpy(GP(ARG(1)),&b->spatial.mode,4); RET(0,2); }
static void t_SetMode(Ctx *c) { AUDIO_GUARD; Buf *b=B(ARG(0)); if(!b) RET(DSERR_INVALIDPARAM,3); if(ARG(2)>1) RET(DSERR_INVALIDPARAM,3); uint32_t v[]={ ARG(1) }; spatial_set(b,offsetof(Spatial,mode),v,sizeof v,ARG(2)); RET(0,3); }
static void t_GetPosition(Ctx *c) { AUDIO_GUARD; Buf *b=B(ARG(0)); if(!b) RET(DSERR_INVALIDPARAM,2); memcpy(GP(ARG(1)),&b->spatial.pos,12); RET(0,2); }
static void t_SetPosition(Ctx *c) { AUDIO_GUARD; Buf *b=B(ARG(0)); if(!b) RET(DSERR_INVALIDPARAM,5); if(ARG(4)>1) RET(DSERR_INVALIDPARAM,5); float v[]={ F(ARG(1)), F(ARG(2)), F(ARG(3)) }; for(int k=0;k<3;k++) if(!isfinite(v[k])) RET(DSERR_INVALIDPARAM,5); spatial_set(b,offsetof(Spatial,pos),v,sizeof v,ARG(4)); RET(0,5); }
static void t_GetVelocity(Ctx *c) { AUDIO_GUARD; Buf *b=B(ARG(0)); if(!b) RET(DSERR_INVALIDPARAM,2); memcpy(GP(ARG(1)),&b->spatial.vel,12); RET(0,2); }
static void t_SetVelocity(Ctx *c) { AUDIO_GUARD; Buf *b=B(ARG(0)); if(!b) RET(DSERR_INVALIDPARAM,5); if(ARG(4)>1) RET(DSERR_INVALIDPARAM,5); float v[]={ F(ARG(1)), F(ARG(2)), F(ARG(3)) }; for(int k=0;k<3;k++) if(!isfinite(v[k])) RET(DSERR_INVALIDPARAM,5); spatial_set(b,offsetof(Spatial,vel),v,sizeof v,ARG(4)); RET(0,5); }
static void l_GetDistanceFactor(Ctx *c) { AUDIO_GUARD; memcpy(GP(ARG(1)),&lis.dist,4); RET(0,2); }
static void l_SetDistanceFactor(Ctx *c) { AUDIO_GUARD; if(ARG(2)>1) RET(DSERR_INVALIDPARAM,3); float v[]={ F(ARG(1)) }; for(int k=0;k<1;k++) if(!isfinite(v[k])) RET(DSERR_INVALIDPARAM,3); listener_set(offsetof(Listener,dist),v,sizeof v,ARG(2)); RET(0,3); }
static void l_GetDopplerFactor(Ctx *c) { AUDIO_GUARD; memcpy(GP(ARG(1)),&lis.doppler,4); RET(0,2); }
static void l_SetDopplerFactor(Ctx *c) { AUDIO_GUARD; if(ARG(2)>1) RET(DSERR_INVALIDPARAM,3); float v[]={ F(ARG(1)) }; for(int k=0;k<1;k++) if(!isfinite(v[k])) RET(DSERR_INVALIDPARAM,3); listener_set(offsetof(Listener,doppler),v,sizeof v,ARG(2)); RET(0,3); }
static void l_GetOrientation(Ctx *c) { AUDIO_GUARD; memcpy(GP(ARG(1)),lis.front,12); memcpy(GP(ARG(2)),lis.top,12); RET(0,3); }
static void l_SetOrientation(Ctx *c) { AUDIO_GUARD; if(ARG(7)>1) RET(DSERR_INVALIDPARAM,8); float v[]={ F(ARG(1)), F(ARG(2)), F(ARG(3)), F(ARG(4)), F(ARG(5)), F(ARG(6)) }; for(int k=0;k<6;k++) if(!isfinite(v[k])) RET(DSERR_INVALIDPARAM,8); listener_set(offsetof(Listener,front),v,sizeof v,ARG(7)); RET(0,8); }
static void l_GetPosition(Ctx *c) { AUDIO_GUARD; memcpy(GP(ARG(1)),&lis.pos,12); RET(0,2); }
static void l_SetPosition(Ctx *c) { AUDIO_GUARD; if(ARG(4)>1) RET(DSERR_INVALIDPARAM,5); float v[]={ F(ARG(1)), F(ARG(2)), F(ARG(3)) }; for(int k=0;k<3;k++) if(!isfinite(v[k])) RET(DSERR_INVALIDPARAM,5); listener_set(offsetof(Listener,pos),v,sizeof v,ARG(4)); RET(0,5); }
static void l_GetRolloffFactor(Ctx *c) { AUDIO_GUARD; memcpy(GP(ARG(1)),&lis.rolloff,4); RET(0,2); }
static void l_SetRolloffFactor(Ctx *c) { AUDIO_GUARD; if(ARG(2)>1) RET(DSERR_INVALIDPARAM,3); float v[]={ F(ARG(1)) }; for(int k=0;k<1;k++) if(!isfinite(v[k])) RET(DSERR_INVALIDPARAM,3); listener_set(offsetof(Listener,rolloff),v,sizeof v,ARG(2)); RET(0,3); }
static void l_GetVelocity(Ctx *c) { AUDIO_GUARD; memcpy(GP(ARG(1)),&lis.vel,12); RET(0,2); }
static void l_SetVelocity(Ctx *c) { AUDIO_GUARD; if(ARG(4)>1) RET(DSERR_INVALIDPARAM,5); float v[]={ F(ARG(1)), F(ARG(2)), F(ARG(3)) }; for(int k=0;k<3;k++) if(!isfinite(v[k])) RET(DSERR_INVALIDPARAM,5); listener_set(offsetof(Listener,vel),v,sizeof v,ARG(4)); RET(0,5); }

/* ---- IKsPropertySet: EAX 2.0 listener and buffer properties, EAX 1.0 reverb ---- */
static const uint8_t EAX20_LISTENER[8] = {0x99, 0xe5, 0x00, 0x00, 0xe8, 0xd8, 0xc7, 0x22};   /* 0306a6a8 / 0306a6a7-b224-11d2-... */
static const uint8_t EAX10[8] = {0xb7, 0x3a, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00};            /* 4a4e6fc1 / 4a4e6fc0-c341-11d1-... */
enum { SET_NONE, SET_EAX2_LISTENER, SET_EAX2_BUFFER, SET_EAX1_LISTENER, SET_EAX1_BUFFER };
static int prop_set(uint32_t g)
{
    if (guid_is(g, 0x0306a6a8u, 0xb224, 0x11d2, EAX20_LISTENER)) return SET_EAX2_LISTENER;
    if (guid_is(g, 0x0306a6a7u, 0xb224, 0x11d2, EAX20_LISTENER)) return SET_EAX2_BUFFER;
    if (guid_is(g, 0x4a4e6fc1u, 0xc341, 0x11d1, EAX10)) return SET_EAX1_LISTENER;
    if (guid_is(g, 0x4a4e6fc0u, 0xc341, 0x11d1, EAX10)) return SET_EAX1_BUFFER;
    return SET_NONE;
}
/* EAX 1.0 and 2.0 property IDs/layouts follow Creative's published headers.
 * Deferred writes are isolated until NONE (COMMITDEFERREDSETTINGS). Unknown
 * extensions fail QuerySupport so Miles can select a supported provider. */
static int property_valid(int set, uint32_t id)
{
    return set == SET_EAX2_LISTENER ? id <= 15 : set == SET_EAX2_BUFFER ? id <= 14 :
           set == SET_EAX1_LISTENER ? id <= 4 : set == SET_EAX1_BUFFER ? id <= 1 : 0;
}
static void k_QuerySupport(Ctx *c)
{ AUDIO_GUARD;
    int set=prop_set(ARG(1)); uint32_t id=ARG(2)&0x7fffffffu;
    if (!ARG(3)) RET(DSERR_INVALIDPARAM,4);
    rt_w32(G_MEM,ARG(3),0);
    if(!property_valid(set,id)) RET(0x80070490u,4);
    rt_w32(G_MEM,ARG(3),(set<=SET_EAX2_BUFFER && id==0)?2:3); RET(0,4);
}
static void *prop_field(int set, Buf *b, uint32_t id, uint32_t *len)
{
    if(set==SET_EAX2_LISTENER && id>=1 && id<=15) {
        *len=id==1?sizeof eax:4; return (char *)&eax+(id==1?0:4*(id-2));
    }
    if(set==SET_EAX2_BUFFER && b && id>=1 && id<=14) {
        *len=id==1?sizeof b->eax:4; return (char *)&b->eax+(id==1?0:4*(id-2));
    }
    if(set==SET_EAX1_BUFFER && b && id<=1) { *len=4; return &b->eax1_mix; }
    return 0;
}
static int between(float v,float lo,float hi) { return isfinite(v) && v>=lo && v<=hi; }
static int valid_listener(const EaxListener *e)
{
    return between(e->room,-10000,0) && between(e->room_hf,-10000,0) && between(e->room_rolloff,0,10) &&
        between(e->decay,.1f,20) && between(e->decay_hf_ratio,.1f,2) && between(e->reflections,-10000,1000) &&
        between(e->reflections_delay,0,.3f) && between(e->reverb,-10000,2000) && between(e->reverb_delay,0,.1f) &&
        e->environment<=25 && between(e->env_size,1,100) && between(e->env_diffusion,0,1) && between(e->air_absorption_hf,-100,0) && !(e->flags&~63u);
}
static int valid_buffer(const EaxBuffer *e)
{
    return between(e->direct,-10000,1000) && between(e->direct_hf,-10000,0) && between(e->room,-10000,1000) &&
        between(e->room_hf,-10000,0) && between(e->room_rolloff,0,10) && between(e->obstruction,-10000,0) &&
        between(e->obstruction_lf,0,1) && between(e->occlusion,-10000,0) && between(e->occlusion_lf,0,1) &&
        between(e->occlusion_room,0,10) && between(e->outside_hf,-10000,0) && between(e->air_absorption,0,10) && !(e->flags&~7u);
}
static void k_Get(Ctx *c)
{ AUDIO_GUARD;
    Buf *b=B(ARG(0)); int set=prop_set(ARG(1)); uint32_t id=ARG(2)&0x7fffffffu,n=0,data=ARG(5),cap=ARG(6);
    if(ARG(7)) rt_w32(G_MEM,ARG(7),0);
    if(!b || !property_valid(set,id)) RET(0x80070490u,8);
    uint32_t all[4]; void *p;
    if(set==SET_EAX1_LISTENER) {
        float v[4]; eax1_get(v); all[0]=eax.environment; memcpy(all+1,v+1,12);
        p=id?(void *)(all+id-1):(void *)all; n=id?4:16;
    } else p=prop_field(set,b,id,&n);
    if(!p) RET(0x80070490u,8);
    if(ARG(7)) rt_w32(G_MEM,ARG(7),n);
    if(!data || cap<n) RET(DSERR_INVALIDPARAM,8);
    memcpy(GP(data),p,n); RET(0,8);
}
static void eax_commit(void)
{
    if(eax_dirty){eax=eax_next;eax_dirty=0;}
    for(int i=0;i<NBUF;i++) if(bufs[i].used && bufs[i].eax_dirty){bufs[i].eax=bufs[i].eax_next;bufs[i].eax_dirty=0;}
    eax_changed();
}
static void k_Set(Ctx *c)
{ AUDIO_GUARD;
    Buf *b=B(ARG(0)); int set=prop_set(ARG(1)); uint32_t raw=ARG(2),id=raw&0x7fffffffu,data=ARG(5),n=ARG(6),len=0;
    int deferred=(raw>>31) && (set==SET_EAX2_LISTENER || set==SET_EAX2_BUFFER);
    if(!b || !property_valid(set,id)) RET(0x80070490u,7);
    if((set==SET_EAX2_LISTENER || set==SET_EAX2_BUFFER) && id==0) {if(!deferred)eax_commit();RET(0,7);}
    if(!data) RET(DSERR_INVALIDPARAM,7);
    EaxListener old=eax; EaxBuffer oldbuf=b->eax; float oldmix=b->eax1_mix;
    if(deferred) { if(eax_dirty)eax=eax_next; if(b->eax_dirty)b->eax=b->eax_next; }
    int valid=1;
    if(set==SET_EAX1_LISTENER) {
        len=id?4:16;
        if(n<len) valid=0;
        else {
            float v[4]; eax1_get(v);
            if(id==0 || id==1) {
                uint32_t env=rt_r32(G_MEM,data); if(env>25)valid=0; else eax_environment(env);
            }
            if(id==0)memcpy(v+1,GP(data+4),12);
            else if(id>=2)memcpy(v+id-1,GP(data),4);
            if(id!=1) {
                if(!between(v[1],0,1)||!between(v[2],.1f,20)||!between(v[3],0,2)) valid=0;
                else eax1_set(v);
            }
        }
    } else {
        void *p=prop_field(set,b,id,&len);
        if(!p || n<len)valid=0;
        else {
            float oldsize=eax.env_size;
            memcpy(p,GP(data),len);
            if(set==SET_EAX2_LISTENER && id==11) {
                if(eax.environment>25)valid=0; else eax_environment(eax.environment);
            }
            if(set==SET_EAX2_LISTENER && id==12) {
                eax_size_prev=oldsize; eax_size_changed();
                eax.decay=fminf(20,fmaxf(.1f,eax.decay));
                eax.reflections=(int32_t)fminf(1000,fmaxf(-10000,eax.reflections));
                eax.reverb=(int32_t)fminf(2000,fmaxf(-10000,eax.reverb));
                eax.reflections_delay=fminf(.3f,eax.reflections_delay);eax.reverb_delay=fminf(.1f,eax.reverb_delay);
            }
            valid &= set==SET_EAX2_LISTENER?valid_listener(&eax):set==SET_EAX2_BUFFER?valid_buffer(&b->eax):
                (b->eax1_mix==-1 || between(b->eax1_mix,0,1));
        }
    }
    if(!valid){eax=old;b->eax=oldbuf;b->eax1_mix=oldmix;RET(DSERR_INVALIDPARAM,7);}
    if(deferred) {
        if(set==SET_EAX2_LISTENER){eax_next=eax;eax_dirty=1;}
        else {b->eax_next=b->eax;b->eax_dirty=1;}
        eax=old;b->eax=oldbuf;
    } else {
        if(eax_dirty && (set==SET_EAX1_LISTENER || set==SET_EAX2_LISTENER)) {
            if(set==SET_EAX1_LISTENER || id==1 || id==11 || id==12)eax_next=eax;
            else memcpy((char *)&eax_next+4*(id-2),(char *)&eax+4*(id-2),4);
        }
        if(b->eax_dirty && set==SET_EAX2_BUFFER) {
            if(id==1)b->eax_next=b->eax;
            else memcpy((char *)&b->eax_next+4*(id-2),(char *)&b->eax+4*(id-2),4);
        }
        eax_changed();
    }
    LOG("EAX set %d id %u (%u bytes)%s value %08x\n",set,id,n,deferred?" deferred":"",n>=4?rt_r32(G_MEM,data):0);
    RET(0,7);
}

static uint32_t vtable(const char *iface, void (*const *fns)(Ctx *), int n)
{
    uint32_t t = heap_alloc(w32_process_heap, 8, 4u * (uint32_t)n);
    for (int k = 0; k < n; k++) { char nm[96]; snprintf(nm, sizeof nm, "dsound!%s.%d", iface, k); rt_w32(G_MEM, t + 4u * (uint32_t)k, w32_thunk_register(nm, fns[k])); }
    return t;
}
static void eax_changed(void)                             /* the game set an EAX property: the room follows (under lock) */
{
    eax_size_prev = eax.env_size; eax_used = 1;
    if (!reverb) return;
    EaxrParams p = {(float)eax.room, (float)eax.room_hf, eax.decay, eax.decay_hf_ratio, (float)eax.reflections, eax.reflections_delay,
                    (float)eax.reverb, eax.reverb_delay, eax.env_size, eax.env_diffusion};
    eaxr_set(reverb, &p);
}
IMPL(dsound, DirectSoundCreate)
{
    AUDIO_GUARD;
    if (!vt_ds) {
        static void (*const ds[])(Ctx *) = { d_QueryInterface, d_AddRef, d_Release, d_CreateSoundBuffer, d_GetCaps, d_DuplicateSoundBuffer,
            d_SetCooperativeLevel, d_Compact, d_GetSpeakerConfig, d_SetSpeakerConfig, d_Initialize };
        static void (*const bf[])(Ctx *) = { b_QueryInterface, b_AddRef, b_Release, b_GetCaps, b_GetCurrentPosition, b_GetFormat, b_GetVolume,
            b_GetPan, b_GetFrequency, b_GetStatus, b_Initialize, b_Lock, b_Play, b_SetCurrentPosition, b_SetFormat, b_SetVolume,
            b_SetPan, b_SetFrequency, b_Stop, b_Unlock, b_Restore };
        static void (*const td[])(Ctx *) = { b_QueryInterface, b_AddRef, b_Release, t_GetAllParameters, t_GetConeAngles, t_GetConeOrientation,
            t_GetConeOutsideVolume, t_GetMaxDistance, t_GetMinDistance, t_GetMode, t_GetPosition, t_GetVelocity, t_SetAllParameters,
            t_SetConeAngles, t_SetConeOrientation, t_SetConeOutsideVolume, t_SetMaxDistance, t_SetMinDistance, t_SetMode, t_SetPosition, t_SetVelocity };
        static void (*const ls[])(Ctx *) = { b_QueryInterface, b_AddRef, b_Release, l_GetAllParameters, l_GetDistanceFactor, l_GetDopplerFactor,
            l_GetOrientation, l_GetPosition, l_GetRolloffFactor, l_GetVelocity, l_SetAllParameters, l_SetDistanceFactor, l_SetDopplerFactor,
            l_SetOrientation, l_SetPosition, l_SetRolloffFactor, l_SetVelocity, l_CommitDeferredSettings };
        static void (*const ks[])(Ctx *) = { b_QueryInterface, b_AddRef, b_Release, k_Get, k_Set, k_QuerySupport };
        _Static_assert(sizeof ds / sizeof *ds == 11 && sizeof bf / sizeof *bf == 21 && sizeof td / sizeof *td == 21 && sizeof ls / sizeof *ls == 18 &&
                       sizeof ks / sizeof *ks == 6, "vtable sizes");
        vt_ds = vtable("IDirectSound", ds, 11); vt_buf = vtable("IDirectSoundBuffer", bf, 21); vt_3d = vtable("IDirectSound3DBuffer", td, 21);
        vt_lis = vtable("IDirectSound3DListener", ls, 18); vt_ks = vtable("IKsPropertySet", ks, 6);
        eax_environment(0); reverb = eaxr_new(OUT_RATE);
        ds_obj = heap_alloc(w32_process_heap, 8, 8); rt_w32(G_MEM, ds_obj, vt_ds);
    }
    { const char *m = getenv("W32_DSOUND"); if (m && !strcmp(m, "0")) RET(DSERR_GENERIC, 3); }   /* W32_DSOUND=0: Miles uses waveOut */
    ds_refs++; rt_w32(G_MEM, ARG(1), ds_obj); LOG("DirectSoundCreate\n");
    RET(DS_OK, 3);
}
IMPL(dsound, DirectSoundEnumerateA)
{
    uint32_t cb = ARG(0), ctx = ARG(1);
    uint32_t s = heap_alloc(w32_process_heap, 8, 64); strcpy((char *)GP(s), "Primary Sound Driver"); strcpy((char *)GP(s + 32), "");
    uint32_t a[4] = {0, s, s + 32, ctx}; w32_callback(c, cb, 4, a);
    RET(DS_OK, 2);
}
