/* ============================================================================
 * audio_samples.h — declarations for the UI feedback samples, held in flash.
 *
 * The data lives in audio_samples.c as const int16_t, INTERLEAVED STEREO
 * (L,R,L,R,...) with L=R, at 16 kHz. Each _len is the TOTAL number of int16
 * values, that is twice the mono sample count: it is exactly what to hand to
 * i2s_channel_write().
 * ==========================================================================*/
#ifndef AUDIO_SAMPLES_H
#define AUDIO_SAMPLES_H

#include <stdint.h>

#define AUDIO_SAMPLES_RATE 16000

extern const int16_t  audio_smp_beep_up[];
extern const uint32_t audio_smp_beep_up_len;
extern const int16_t  audio_smp_beep_down[];
extern const uint32_t audio_smp_beep_down_len;
extern const int16_t  audio_smp_click[];
extern const uint32_t audio_smp_click_len;
extern const int16_t  audio_smp_lock_on[];
extern const uint32_t audio_smp_lock_on_len;
extern const int16_t  audio_smp_lock_off[];
extern const uint32_t audio_smp_lock_off_len;
extern const int16_t  audio_smp_reject[];
extern const uint32_t audio_smp_reject_len;
extern const int16_t  audio_smp_boot[];
extern const uint32_t audio_smp_boot_len;
extern const int16_t  audio_smp_nav_beep[];
extern const uint32_t audio_smp_nav_beep_len;

#endif /* AUDIO_SAMPLES_H */
