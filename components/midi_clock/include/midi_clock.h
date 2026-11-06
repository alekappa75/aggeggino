/*
 * midi_clock.h — MIDI clock engine (24 PPQN).
 *
 * WHAT IT DOES, AND WHAT IT DOES NOT
 *
 * It builds the MIDI clock's time grid from the bridge's phase and hands it to
 * a SINK as "emit this status byte, now". It knows nothing about transports:
 * no network, no UART, no USB. That is deliberate — the hard part is the grid,
 * and the grid is identical for all three intended transports (Network MIDI
 * 2.0, DIN, USB). Written once.
 *
 * WHY "A TIMER EVERY 1/24 OF A BEAT" IS NOT ENOUGH
 *
 * The phase source (clock_next_beat) says where the NEXT BEAT falls, carrying
 * whatever measurement jitter it has. MIDI clock wants 24 pulses per quarter
 * note, and the 23 in between have no anchor at all: they must be interpolated.
 *
 * Recomputing the grid at every beat would transfer that measurement jitter
 * straight onto the pulses; never recomputing it would let the grid drift. The
 * solution is the same one that worked elsewhere in this project: a FREE-
 * RUNNING PHASE ACCUMULATOR at the current period, corrected gently IN
 * FREQUENCY at each known beat. Position error is never fixed with a jump, but
 * by lengthening or shortening the following 24 pulses slightly.
 *
 * The intended consequence is that the error decays geometrically by (1-CORR_K)
 * per beat, and phase NEVER runs backwards. Two pulses too close together
 * confuse receivers that estimate tempo from intervals, and there are many of
 * those.
 *
 * A HARD re-lock exists, but only for genuine discontinuities — bridge
 * direction changed, track jumped, phase reacquired after a gap. Above a wide
 * threshold there is nothing sensible to chase: start over, and count it.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Pulses per quarter note. Fixed by the MIDI standard, not a choice. */
#define MIDI_CLOCK_PPQN   24

/* System Real Time status bytes emitted by this engine. */
#define MIDI_CLOCK_TICK      0xF8   /* Timing Clock  */
#define MIDI_CLOCK_START     0xFA   /* Start         */
#define MIDI_CLOCK_CONTINUE  0xFB   /* Continue      */
#define MIDI_CLOCK_STOP      0xFC   /* Stop          */

/*
 * Phase source. Same semantics, and same signature, as clock_next_beat():
 * returns false when there is no trustworthy phase, and in that case the
 * engine emits NOTHING — never an invented instant.
 */
typedef bool (*midi_clock_src_fn)(int64_t now, int64_t *t_beat,
                                  uint8_t *bb, double *bpm);

/*
 * Phase offset in microseconds, re-read at every beat. May be NULL, meaning
 * zero. It is a function rather than a value because the user changes it live
 * from the buttons, and the engine must see the effect on the next beat.
 */
typedef int (*midi_clock_off_fn)(void);

/*
 * Sink: emits the status byte. Called FROM THE ENGINE TASK, at high priority
 * and on the critical path — do not block, do not allocate, do not take
 * contended locks. 'due_us' is the intended instant, not the actual one, so a
 * sink can measure its own delivery latency.
 */
typedef void (*midi_clock_sink_fn)(uint8_t status, int64_t due_us, void *ctx);

typedef struct {
    midi_clock_src_fn  src;         /* required */
    midi_clock_sink_fn sink;        /* required */
    void              *sink_ctx;
    midi_clock_off_fn  offset_us;   /* NULL => no offset */

    int core_id;                    /* -1 => no affinity   */
    int priority;                   /* 0  => default (9)   */
    int stack;                      /* 0  => default (3072) */
} midi_clock_config_t;

/* Start the engine: creates timer and task. The grid locks on to the first
 * usable reading from the source. */
esp_err_t midi_clock_start(const midi_clock_config_t *cfg);

/* Stop the engine and release its resources. If transport was running it emits
 * a Stop first: leaving receivers in play and then falling silent is the worst
 * possible way to leave. */
void midi_clock_stop(void);

/*
 * Transport. TICK is emitted continuously while the grid is locked, regardless
 * of this flag — that is the convention DAWs follow, and receivers that want a
 * free-running clock expect it. Start and Stop only mark the run.
 *
 * Callable from any task: the byte is not emitted here but by the engine, on
 * the first usable pulse. Start is aligned to the beginning of a bar, Stop goes
 * out immediately. Worst-case delay is one pulse (~20 ms at 128 BPM).
 */
void midi_clock_set_running(bool running);

/* True when the grid is locked to a valid phase and emitting. */
bool midi_clock_is_locked(void);

typedef struct {
    uint32_t ticks;        /* pulses emitted in the window                   */
    uint32_t beats;        /* gentle corrections applied, one per beat       */
    uint32_t resyncs;      /* HARD re-locks: genuine discontinuities         */
    uint32_t skips;        /* pulses dropped because their instant had passed */
    int32_t  jit_min_us;   /* task wake-up against the intended instant      */
    int32_t  jit_max_us;
    int32_t  err_last_us;  /* last deviation of the grid from its anchor     */
    bool     locked;
} midi_clock_stats_t;

/* Read the statistics and RESET the windowed values (jitter, counters). Meant
 * to be called from the heartbeat, like bsp_spi_stats(). */
void midi_clock_stats(midi_clock_stats_t *out);

#ifdef __cplusplus
}
#endif
