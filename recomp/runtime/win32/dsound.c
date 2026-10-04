/* DirectSound on Core Audio: what the recompiled Miles Sound System mixes into on Windows ("DirectSound - MSS
 * Mixer"). Software buffers only: a primary buffer that just gates output, and secondary buffers that are rings in guest
 * memory, mixed by the Core Audio render callback (any PCM rate/format, volume in 1/100 dB, pan). Play and write cursors
 * advance with the device, as Miles' fragment mixer expects. */
#include "w32.h"
#include <AudioToolbox/AudioToolbox.h>
#include <math.h>
#include <stdatomic.h>

enum { DS_OK = 0, DSERR_INVALIDPARAM = 0x80070057u, DSERR_NOINTERFACE = 0x80004002u, DSERR_UNSUPPORTED = 0x80004001u,
       DSERR_GENERIC = 0x80004005u, DSBCAPS_PRIMARYBUFFER = 1, DSBPLAY_LOOPING = 1, DSBSTATUS_PLAYING = 1, DSBSTATUS_LOOPING = 4,
       DSBLOCK_FROMWRITECURSOR = 1, DSBLOCK_ENTIREBUFFER = 2, OUT_RATE = 44100 };
typedef struct Buf {
    int used, primary, refs; uint32_t obj, mem, size, flags;           /* guest object, guest ring, bytes */
    int ch, bits, rate, align; double gain_l, gain_r; int32_t vol, pan; uint32_t freq;
    _Atomic uint32_t play; double frac; _Atomic int playing, looping;
} Buf;
static Buf bufs[64]; static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t vt_ds, vt_buf, ds_obj; static int ds_refs;
static AudioComponentInstance au; static _Atomic int primary_playing;
static _Atomic uint32_t slice_max = 512;                  /* largest render the device asked for: the write cursor's lead */

static Buf *B(uint32_t obj) { uint32_t k = rt_r32(G_MEM, obj + 4); return k < 64 && bufs[k].used && bufs[k].obj == obj ? &bufs[k] : 0; }
static void set_gain(Buf *b)
{
    double v = b->vol <= -10000 ? 0 : pow(10.0, b->vol / 2000.0);                         /* 1/100 dB */
    double l = b->pan > 0 ? pow(10.0, -b->pan / 2000.0) : 1, r = b->pan < 0 ? pow(10.0, b->pan / 2000.0) : 1;
    b->gain_l = v * l; b->gain_r = v * r;
}
static inline float sample(Buf *b, uint32_t frame, int chn)
{
    uint32_t at = (frame % (b->size / b->align)) * b->align + (b->ch == 2 ? chn : 0) * (b->bits / 8);
    return b->bits == 16 ? (int16_t)rt_r16(G_MEM, b->mem + at) / 32768.0f : ((int)G_MEM[b->mem + at] - 128) / 128.0f;
}
void w32_audio_out(const float *lr, uint32_t frames, int rate, int source);         /* winmm.c: mute, level log, dump */
static OSStatus render(void *u, AudioUnitRenderActionFlags *fl, const AudioTimeStamp *ts, UInt32 bus, UInt32 frames, AudioBufferList *io)
{
    (void)u; (void)fl; (void)ts; (void)bus;
    float *out = io->mBuffers[0].mData; memset(out, 0, frames * 8);
    if (frames > slice_max) slice_max = frames;
    if (primary_playing) for (int k = 0; k < 64; k++) {
        Buf *b = &bufs[k]; if (!b->used || b->primary || !b->playing) continue;
        uint32_t nframes = b->size / b->align, pos = b->play / b->align; double step = (double)(b->freq ? b->freq : (uint32_t)b->rate) / OUT_RATE, fr = b->frac;
        for (UInt32 i = 0; i < frames; i++) {
            uint32_t p0 = pos, p1 = pos + 1; float t = (float)fr;
            if (!b->looping && p1 >= nframes) p1 = p0;
            float l = sample(b, p0, 0) * (1 - t) + sample(b, p1, 0) * t, r = sample(b, p0, 1) * (1 - t) + sample(b, p1, 1) * t;
            out[2 * i] += (float)(l * b->gain_l); out[2 * i + 1] += (float)(r * b->gain_r);
            fr += step; while (fr >= 1) { fr -= 1; pos++; }
            if (pos >= nframes) { if (b->looping) pos -= nframes; else { b->playing = 0; pos = 0; break; } }
        }
        b->frac = fr; b->play = pos * b->align;
    }
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
static void b_QueryInterface(Ctx *c) { RET(DSERR_NOINTERFACE, 3); }
static void b_AddRef(Ctx *c) { Buf *b = B(ARG(0)); RET(b ? ++b->refs : 0, 1); }
static void b_Release(Ctx *c)
{
    Buf *b = B(ARG(0)); if (!b) RET(0, 1);
    int r = --b->refs; if (!r) { b->playing = 0; pthread_mutex_lock(&lock); b->used = 0; pthread_mutex_unlock(&lock); LOG("release buffer\n"); }
    RET(r, 1);
}
static void b_GetCaps(Ctx *c)
{
    Buf *b = B(ARG(0)); uint32_t p = ARG(1); if (!b) RET(DSERR_INVALIDPARAM, 2);
    rt_w32(G_MEM, p + 4, b->flags | 0x40000 /* DSBCAPS_LOCSOFTWARE */); rt_w32(G_MEM, p + 8, b->size); rt_w32(G_MEM, p + 12, 0); rt_w32(G_MEM, p + 16, 0);
    RET(DS_OK, 2);
}
static uint32_t write_ahead(Buf *b) { return (uint32_t)((uint64_t)(b->freq ? b->freq : (uint32_t)b->rate) * slice_max / OUT_RATE + 1) * b->align; }
static void b_GetCurrentPosition(Ctx *c)
{
    Buf *b = B(ARG(0)); if (!b) RET(DSERR_INVALIDPARAM, 3);
    uint32_t p = b->primary ? 0 : b->play, w = b->primary ? 0 : (p + write_ahead(b)) % b->size;
    if (ARG(1)) rt_w32(G_MEM, ARG(1), p); if (ARG(2)) rt_w32(G_MEM, ARG(2), w);
    RET(DS_OK, 3);
}
static void b_GetFormat(Ctx *c)
{
    Buf *b = B(ARG(0)); uint32_t p = ARG(1), cap = ARG(2); if (!b) RET(DSERR_INVALIDPARAM, 4);
    uint8_t w[18] = {0}; uint16_t v16; uint32_t v32;
    v16 = 1; memcpy(w, &v16, 2); v16 = (uint16_t)b->ch; memcpy(w + 2, &v16, 2); v32 = (uint32_t)b->rate; memcpy(w + 4, &v32, 4);
    v32 = (uint32_t)(b->rate * b->align); memcpy(w + 8, &v32, 4); v16 = (uint16_t)b->align; memcpy(w + 12, &v16, 2); v16 = (uint16_t)b->bits; memcpy(w + 14, &v16, 2);
    if (p) memcpy(GP(p), w, cap < 18 ? cap : 18);
    if (ARG(3)) rt_w32(G_MEM, ARG(3), 18);
    RET(DS_OK, 4);
}
static void b_GetVolume(Ctx *c) { Buf *b = B(ARG(0)); if (!b) RET(DSERR_INVALIDPARAM, 2); rt_w32(G_MEM, ARG(1), (uint32_t)b->vol); RET(DS_OK, 2); }
static void b_GetPan(Ctx *c) { Buf *b = B(ARG(0)); if (!b) RET(DSERR_INVALIDPARAM, 2); rt_w32(G_MEM, ARG(1), (uint32_t)b->pan); RET(DS_OK, 2); }
static void b_GetFrequency(Ctx *c) { Buf *b = B(ARG(0)); if (!b) RET(DSERR_INVALIDPARAM, 2); rt_w32(G_MEM, ARG(1), b->freq ? b->freq : (uint32_t)b->rate); RET(DS_OK, 2); }
static void b_GetStatus(Ctx *c)
{
    Buf *b = B(ARG(0)); if (!b) RET(DSERR_INVALIDPARAM, 2);
    int pl = b->primary ? primary_playing : b->playing;
    rt_w32(G_MEM, ARG(1), (pl ? DSBSTATUS_PLAYING : 0) | (pl && b->looping ? DSBSTATUS_LOOPING : 0)); RET(DS_OK, 2);
}
static void b_Initialize(Ctx *c) { RET(0x88780082u /* DSERR_ALREADYINITIALIZED */, 3); }
static void b_Lock(Ctx *c)
{
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
{
    Buf *b = B(ARG(0)); if (!b) RET(DSERR_INVALIDPARAM, 4);
    if (start_output()) RET(DSERR_GENERIC, 4);
    b->looping = (ARG(3) & DSBPLAY_LOOPING) != 0;
    if (b->primary) primary_playing = 1; else b->playing = 1;
    LOG("play %s looping %d\n", b->primary ? "primary" : "secondary", b->looping);
    RET(DS_OK, 4);
}
static void b_SetCurrentPosition(Ctx *c) { Buf *b = B(ARG(0)); if (!b || b->primary) RET(DSERR_INVALIDPARAM, 2); b->play = ARG(1) % (b->size ? b->size : 1) / b->align * b->align; b->frac = 0; RET(DS_OK, 2); }
static void b_SetFormat(Ctx *c)
{
    Buf *b = B(ARG(0)); if (!b) RET(DSERR_INVALIDPARAM, 2);
    set_format(b, ARG(1)); LOG("format %d Hz %d bits %d ch (%s)\n", b->rate, b->bits, b->ch, b->primary ? "primary" : "secondary");
    RET(DS_OK, 2);
}
static void b_SetVolume(Ctx *c) { Buf *b = B(ARG(0)); if (!b) RET(DSERR_INVALIDPARAM, 2); b->vol = (int32_t)ARG(1); set_gain(b); RET(DS_OK, 2); }
static void b_SetPan(Ctx *c) { Buf *b = B(ARG(0)); if (!b) RET(DSERR_INVALIDPARAM, 2); b->pan = (int32_t)ARG(1); set_gain(b); RET(DS_OK, 2); }
static void b_SetFrequency(Ctx *c) { Buf *b = B(ARG(0)); if (!b) RET(DSERR_INVALIDPARAM, 2); b->freq = ARG(1); RET(DS_OK, 2); }
static void b_Stop(Ctx *c)
{
    Buf *b = B(ARG(0)); if (!b) RET(DSERR_INVALIDPARAM, 1);
    if (b->primary) primary_playing = 0; else b->playing = 0;
    RET(DS_OK, 1);
}
static void b_Unlock(Ctx *c) { RET(DS_OK, 5); }
static void b_Restore(Ctx *c) { RET(DS_OK, 1); }

/* ---- IDirectSound ---- */
static void d_QueryInterface(Ctx *c) { RET(DSERR_NOINTERFACE, 3); }
static void d_AddRef(Ctx *c) { RET(++ds_refs, 1); }
static void d_Release(Ctx *c) { int r = --ds_refs; RET(r < 0 ? 0 : r, 1); }
static uint32_t new_buf(Buf **out)
{
    pthread_mutex_lock(&lock);
    int k = 0; while (k < 64 && bufs[k].used) k++;
    if (k == 64) { pthread_mutex_unlock(&lock); return 0; }
    Buf *b = &bufs[k]; memset(b, 0, sizeof *b); b->used = 1; b->refs = 1; set_gain(b);
    b->obj = heap_alloc(w32_process_heap, 8, 16); rt_w32(G_MEM, b->obj, vt_buf); rt_w32(G_MEM, b->obj + 4, (uint32_t)k);
    pthread_mutex_unlock(&lock);
    *out = b; return b->obj;
}
static void d_CreateSoundBuffer(Ctx *c)
{
    uint32_t desc = ARG(1), out = ARG(2), fl = rt_r32(G_MEM, desc + 4), bytes = rt_r32(G_MEM, desc + 8), wfx = rt_r32(G_MEM, desc + 16);
    Buf *b; uint32_t obj = new_buf(&b); if (!obj) RET(DSERR_GENERIC, 4);
    b->flags = fl; b->primary = (fl & DSBCAPS_PRIMARYBUFFER) != 0;
    b->ch = 2; b->bits = 16; b->rate = 22050; b->align = 4;
    if (wfx) set_format(b, wfx);
    if (!b->primary) {
        if (!wfx || bytes < 4) { b->used = 0; RET(DSERR_INVALIDPARAM, 4); }
        b->size = bytes / b->align * b->align; b->mem = heap_alloc(w32_process_heap, 8, b->size);
        memset(GP(b->mem), b->bits == 8 ? 0x80 : 0, b->size);
    }
    LOG("CreateSoundBuffer flags %#x, %u bytes, %d Hz %d bits %d ch%s\n", fl, bytes, b->rate, b->bits, b->ch, b->primary ? " (primary)" : "");
    rt_w32(G_MEM, out, obj); RET(DS_OK, 4);
}
static void d_GetCaps(Ctx *c)
{
    uint32_t p = ARG(1); uint32_t sz = rt_r32(G_MEM, p); memset(GP(p + 4), 0, sz > 4 ? sz - 4 : 0);
    rt_w32(G_MEM, p + 4, 0x1 | 0x2 | 0x4 | 0x8 | 0x10 | 0x40 | 0x100 | 0x200 | 0x400 | 0x800);  /* primary/secondary mono/stereo 8/16 bit, continuous rate, certified */
    rt_w32(G_MEM, p + 8, 100); rt_w32(G_MEM, p + 12, 200000); rt_w32(G_MEM, p + 16, 1);
    RET(DS_OK, 2);
}
static void d_DuplicateSoundBuffer(Ctx *c) { RET(DSERR_UNSUPPORTED, 3); }
static void d_SetCooperativeLevel(Ctx *c) { RET(DS_OK, 3); }
static void d_Compact(Ctx *c) { RET(DS_OK, 1); }
static void d_GetSpeakerConfig(Ctx *c) { rt_w32(G_MEM, ARG(1), 4 /* DSSPEAKER_STEREO */); RET(DS_OK, 2); }
static void d_SetSpeakerConfig(Ctx *c) { RET(DS_OK, 2); }
static void d_Initialize(Ctx *c) { RET(DS_OK, 2); }

static uint32_t vtable(const char *iface, void (*const *fns)(Ctx *), int n)
{
    uint32_t t = heap_alloc(w32_process_heap, 8, 4u * (uint32_t)n);
    for (int k = 0; k < n; k++) { char nm[96]; snprintf(nm, sizeof nm, "dsound!%s.%d", iface, k); rt_w32(G_MEM, t + 4u * (uint32_t)k, w32_thunk_register(nm, fns[k])); }
    return t;
}
IMPL(dsound, DirectSoundCreate)
{
    if (!vt_ds) {
        static void (*const ds[])(Ctx *) = { d_QueryInterface, d_AddRef, d_Release, d_CreateSoundBuffer, d_GetCaps, d_DuplicateSoundBuffer,
            d_SetCooperativeLevel, d_Compact, d_GetSpeakerConfig, d_SetSpeakerConfig, d_Initialize };
        static void (*const bf[])(Ctx *) = { b_QueryInterface, b_AddRef, b_Release, b_GetCaps, b_GetCurrentPosition, b_GetFormat, b_GetVolume,
            b_GetPan, b_GetFrequency, b_GetStatus, b_Initialize, b_Lock, b_Play, b_SetCurrentPosition, b_SetFormat, b_SetVolume,
            b_SetPan, b_SetFrequency, b_Stop, b_Unlock, b_Restore };
        _Static_assert(sizeof ds / sizeof *ds == 11 && sizeof bf / sizeof *bf == 21, "vtable sizes");
        vt_ds = vtable("IDirectSound", ds, 11); vt_buf = vtable("IDirectSoundBuffer", bf, 21);
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
