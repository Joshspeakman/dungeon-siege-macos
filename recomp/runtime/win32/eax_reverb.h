/* The environment reverb behind DirectSound's EAX (dsound.c): what a sound card with EAX added to the 3D sounds of
 * a room. Levels are in millibels (1/100 dB) as EAX gives them, times in seconds; self-contained, so it is tested on its
 * own (tests/eax_reverb_test.c). */
#ifndef EAX_REVERB_H
#define EAX_REVERB_H
typedef struct {
    float room, room_hf;                  /* the whole room effect's level, and its level at 5 kHz relative to that */
    float decay, decay_hf_ratio;          /* the late reverb's time to fall 60 dB, and that time at 5 kHz as a ratio */
    float reflections, reflections_delay; /* early reflections: level relative to the room, delay after the sound */
    float reverb, reverb_delay;           /* late reverb: level relative to the room, delay after the reflections */
    float env_size, diffusion;            /* the room's size (m), 7.5 a typical room; echo density 0-1 */
} EaxrParams;
typedef struct EaxReverb EaxReverb;
EaxReverb *eaxr_new(int rate);
void eaxr_free(EaxReverb *r);
/* new settings (any thread); the next eaxr_process picks them up */
void eaxr_set(EaxReverb *r, const EaxrParams *p);
/* mono input (the sum of the sounds sent to the room) -> added to interleaved stereo output */
void eaxr_process(EaxReverb *r, const float *in, float *out_lr, int frames);
/* the one-pole low-pass y = (1 - a) x + a y' with gain g (0-1) at angular frequency w: its a (0 when g >= 1) */
float eaxr_onepole(float g, float w);
#endif
