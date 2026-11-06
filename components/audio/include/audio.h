/* ============================================================================
 * audio — UI sound feedback (M5Stack CoreS3-SE, AW88298 amplifier over I2S)
 *
 * Samples are PRE-RENDERED and live in flash (audio_samples.c): no RAM, no
 * synthesis at runtime. The I2S DMA reads them straight out of flash.
 *
 * A dedicated task on core 1 sleeps on a queue and only wakes when there is a
 * sound to play, so idle cost is zero. audio_play() never blocks: it enqueues
 * and returns, because the interaction path must never wait — the same rule
 * that governs access to the SPI bus.
 *
 * A new event REPLACES the one in progress, cut at the first zero crossing:
 * feedback stays in step with the user, and the transition is clean instead of
 * clicking.
 *
 * I2S is a bus of its own, so none of this contends with the display or the
 * W5500. MCLK on GPIO0 is deliberately unused: that pin is a strapping pin and
 * driving it disturbed the W5500.
 * ==========================================================================*/
#ifndef AUDIO_H
#define AUDIO_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One event per UI interaction. */
typedef enum {
    AUDIO_BRIDGE_ON = 0,   /* rising beep        */
    AUDIO_BRIDGE_OFF,      /* falling beep       */
    AUDIO_PHASE_STEP,      /* dry click          */
    AUDIO_LOCK_ON,         /* double click       */
    AUDIO_LOCK_OFF,        /* single click       */
    AUDIO_REJECT,          /* short low beep     */
    AUDIO_BOOT,            /* start-up chime     */
    AUDIO_NAV_BEEP,        /* settings navigation */
    AUDIO_EVENT_COUNT
} audio_event_t;

/* Bring up amplifier, I2S and the core-1 task. Returns ESP_OK only if all of
 * it succeeded; otherwise audio stays off and audio_play() is a silent no-op.
 * Call once, after bsp_init(). */
esp_err_t audio_init(void);

/* True when audio is operational. */
bool audio_ready(void);

/* Play an event. Does NOT block: enqueues and returns. Safe to call from any
 * task, typically the UI thread. A new event replaces one already playing.
 * No-op when audio is not ready. */
void audio_play(audio_event_t ev);

/* Volume, 0..100. */
void audio_set_volume(uint8_t vol_0_100);

/* Enable or disable all sounds globally — this is the "Audio" setting in the
 * UI. When disabled, audio_play() is a silent no-op. Enabled by default. */
void audio_set_enabled(bool on);
bool audio_get_enabled(void);

#ifdef __cplusplus
}
#endif
#endif /* AUDIO_H */
