/*
 * midi_clock.c — MIDI clock engine (24 PPQN). See midi_clock.h for the
 * reasoning; here are the numbers and the traps.
 */
#include <math.h>
#include <string.h>
#include "midi_clock.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "midi_clock";

/* --- Tuning constants ------------------------------------------------------
 *
 * CORR_K  gain of the frequency correction. 0.35 is the same value that worked
 *         on LUMEN's cursor, against an identical problem: a trustworthy but
 *         noisy anchor, with a grid to keep smooth in between. The error decays
 *         by (1-K) per beat, halving in a little under two beats.
 *
 * CORR_MAX_FRAC  ceiling on the correction, as a fraction of the nominal
 *         period. It stops ONE bad anchor from deforming the grid audibly:
 *         without a ceiling, a large error produces an absurd period for a
 *         whole beat. With it, correction takes a few beats longer but no
 *         pulse ever leaves the fence.
 *
 * RESYNC_US  above this deviation there is nothing sensible left to chase:
 *         something else happened — bridge direction changed, track jumped,
 *         phase reacquired after a gap. Start over, and COUNT it: a re-lock
 *         nobody records is a re-lock nobody investigates. 50 ms is about a
 *         tenth of a beat at 128 BPM — wide enough not to trip on jitter,
 *         tight enough not to let a real discontinuity through.
 *
 * LEAD_US  how far ahead of the intended instant to fire, to pay for the sink's
 *         delivery latency. Zero with a dummy sink, since there is nothing to
 *         pay; it will become a per-sink parameter once the real transports
 *         arrive, whose latencies differ widely — DIN under a millisecond,
 *         network a few milliseconds.
 */
#define CORR_K          0.35
#define CORR_MAX_FRAC   0.20
#define RESYNC_US       50000LL
#define LEAD_US         0

#define RETRY_US        100000LL   /* retry when phase is unavailable */
#define MIN_ARM_US      200LL      /* below this margin, skip instead of arming */

/* --- State --------------------------------------------------------------- */
static midi_clock_src_fn  s_src;
static midi_clock_sink_fn s_sink;
static void              *s_sink_ctx;
static midi_clock_off_fn  s_off;

static esp_timer_handle_t s_timer;
static TaskHandle_t       s_task;
static volatile bool      s_stopping;

/* The grid. s_tick_us is the INTENDED instant of the next pulse; the
 * fractional part of the period accumulates separately, because rounding to
 * whole microseconds would by itself produce a drift of several ppm. */
static int64_t s_tick_us;
static double  s_frac;
static double  s_period_us;
static int     s_tick_idx;        /* 0..23; 0 coincides with the beat */
static bool    s_locked;
static bool    s_armed;

/* Transport. s_running is written by other tasks, s_sent_start only by the engine. */
static volatile bool s_running;
static bool          s_sent_start;

/* Windowed statistics. */
static uint32_t s_n_ticks, s_n_beats, s_n_resync, s_n_skip;
static int32_t  s_jit_min = INT32_MAX, s_jit_max = INT32_MIN;
static int32_t  s_err_last;

static void schedule_next(int64_t now);

/* --- Timer and task ------------------------------------------------------- */

static void timer_cb(void *arg)
{
    (void)arg;
    BaseType_t hpw = pdFALSE;
    if (s_task) vTaskNotifyGiveFromISR(s_task, &hpw);
    if (hpw) portYIELD_FROM_ISR();
}

static inline void emit(uint8_t status, int64_t due)
{
    if (s_sink) s_sink(status, due, s_sink_ctx);
}

static void midi_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (s_stopping) break;

        if (s_armed) {
            int64_t now = esp_timer_get_time();
            int32_t jit = (int32_t)(now - s_tick_us);
            if (jit > s_jit_max) s_jit_max = jit;
            if (jit < s_jit_min) s_jit_min = jit;

            /* Transport BEFORE the pulse. Start aligns to the beginning of a
             * bar: many receivers count position from there, and landing it
             * mid-bar throws them out by exactly that much. Stop goes out
             * immediately, because transport that stops late is worse than
             * transport that stops off-grid. */
            if (s_running && !s_sent_start && s_tick_idx == 0) {
                emit(MIDI_CLOCK_START, s_tick_us);
                s_sent_start = true;
            } else if (!s_running && s_sent_start) {
                emit(MIDI_CLOCK_STOP, s_tick_us);
                s_sent_start = false;
            }

            emit(MIDI_CLOCK_TICK, s_tick_us);
            s_n_ticks++;
        }

        schedule_next(esp_timer_get_time());
    }

    /* Orderly exit: transport is not left hanging. */
    if (s_sent_start) { emit(MIDI_CLOCK_STOP, esp_timer_get_time()); s_sent_start = false; }
    s_task = NULL;
    vTaskDelete(NULL);
}

/* --- Locking and correction ----------------------------------------------- */

/* Hard re-lock: the grid restarts from the anchor. Used at start-up, when
 * phase becomes available again after a gap, and on genuine discontinuities. */
static void reanchor(int64_t t_beat, double bpm, bool count)
{
    s_period_us = 60000000.0 / (bpm * MIDI_CLOCK_PPQN);
    s_tick_us   = t_beat;
    s_frac      = 0.0;
    s_tick_idx  = 0;
    s_locked    = true;
    if (count) s_n_resync++;
}

/*
 * Advance the grid by one pulse and, if the pulse just scheduled is the one
 * starting a beat, apply the gentle correction against the fresh anchor.
 *
 * The delicate part: the correction acts on the PERIOD of the following 24
 * pulses, not on the position of this one. That way an instant already
 * computed never moves backwards, and the error is reabsorbed spread out.
 */
static void advance(int64_t now)
{
    /* Free-running step, with the fraction carried rather than lost. */
    double step = s_period_us + s_frac;
    int64_t whole = (int64_t)step;
    s_frac = step - (double)whole;
    s_tick_us += whole;

    s_tick_idx = (s_tick_idx + 1) % MIDI_CLOCK_PPQN;
    if (s_tick_idx != 0) return;            /* intermediate pulse: nothing to do */

    /* Start of a beat: there is an anchor to compare against. */
    int64_t t_beat; uint8_t bb; double bpm;
    if (!s_src(now, &t_beat, &bb, &bpm)) {
        s_locked = false;                   /* phase has gone */
        return;
    }

    int off = s_off ? s_off() : 0;
    int64_t anchor = t_beat + (int64_t)off;

    /* The anchor is the NEXT beat according to the source; our prediction is
     * s_tick_us. If they differ by more than one beat we are on different
     * beats: normalise by moving the anchor to the beat nearest the
     * prediction, or a mere late wake-up would look like a discontinuity and
     * trigger a pointless re-lock. */
    double beat_us = s_period_us * MIDI_CLOCK_PPQN;
    if (beat_us > 1000.0) {
        double k = round((double)(s_tick_us - anchor) / beat_us);
        anchor += (int64_t)(k * beat_us);
    }

    int64_t err = s_tick_us - anchor;       /* > 0 means we are late */
    s_err_last = (int32_t)err;

    if (err > RESYNC_US || err < -RESYNC_US) {
        reanchor(anchor, bpm, true);
        return;
    }

    /* Nominal period from the CURRENT BPM: a tempo change is a communicated
     * fact, not noise, and must be taken up at once rather than filtered. It is
     * the same principle as re-anchoring on a pitch change in the bridge. */
    double nominal = 60000000.0 / (bpm * MIDI_CLOCK_PPQN);
    double corr    = CORR_K * (double)err / MIDI_CLOCK_PPQN;
    double cap     = nominal * CORR_MAX_FRAC;
    if (corr >  cap) corr =  cap;
    if (corr < -cap) corr = -cap;

    s_period_us = nominal - corr;
    s_n_beats++;
}

/* --- Scheduling ----------------------------------------------------------- */

static void schedule_next(int64_t now)
{
    if (!s_timer || s_stopping) return;
    esp_timer_stop(s_timer);

    if (!s_locked) {
        /* Not locked: try to lock, and if phase is unavailable retry shortly
         * rather than stop. The bridge may lock at any moment, and until then
         * NOTHING is emitted. */
        int64_t t_beat; uint8_t bb; double bpm;
        if (!s_src(now, &t_beat, &bb, &bpm)) {
            s_armed = false;
            esp_timer_start_once(s_timer, (uint64_t)RETRY_US);
            return;
        }
        int off = s_off ? s_off() : 0;
        reanchor(t_beat + (int64_t)off, bpm, false);
        s_sent_start = false;               /* new grid: transport must be redone */
    } else {
        advance(now);
        if (!s_locked) {                    /* phase vanished during advance */
            s_armed = false;
            esp_timer_start_once(s_timer, (uint64_t)RETRY_US);
            return;
        }
    }

    /* If the instant has already passed, pulses are SKIPPED rather than fired
     * in a burst. Losing a few pulses shifts perceived tempo for a moment; a
     * burst destroys it, because receivers estimate tempo from the intervals.
     * Skips are counted: if they appear the engine is struggling, and that is
     * to be investigated rather than tolerated.
     *
     * The guard uses a LOCAL counter and not s_n_skip, which is reset by
     * reading the statistics: tying a safety condition to a counter somebody
     * else clears is a refined way of not having one. */
    int guard = 0;
    int64_t dt = (s_tick_us - LEAD_US) - esp_timer_get_time();
    while (dt < MIN_ARM_US) {
        advance(esp_timer_get_time());
        if (!s_locked) { s_armed = false; esp_timer_start_once(s_timer, (uint64_t)RETRY_US); return; }
        s_n_skip++;
        if (++guard > 4 * MIDI_CLOCK_PPQN) {   /* something is wrong: start over */
            s_locked = false;
            s_armed  = false;
            esp_timer_start_once(s_timer, (uint64_t)RETRY_US);
            return;
        }
        dt = (s_tick_us - LEAD_US) - esp_timer_get_time();
    }

    /* The delay is measured NOW, not from the 'now' that entered the function:
     * between the two lies all the work of advance(), and at 24 pulses per
     * beat that difference would become a systematic lag. */
    dt = (s_tick_us - LEAD_US) - esp_timer_get_time();
    if (dt < 1) dt = 1;

    s_armed = true;
    esp_timer_start_once(s_timer, (uint64_t)dt);
}

/* --- API ----------------------------------------------------------------- */

esp_err_t midi_clock_start(const midi_clock_config_t *cfg)
{
    if (!cfg || !cfg->src || !cfg->sink) return ESP_ERR_INVALID_ARG;
    if (s_task) return ESP_ERR_INVALID_STATE;

    s_src = cfg->src; s_sink = cfg->sink; s_sink_ctx = cfg->sink_ctx;
    s_off = cfg->offset_us;
    s_stopping = false;
    s_locked = false; s_armed = false; s_sent_start = false;
    s_tick_idx = 0; s_frac = 0.0; s_period_us = 0.0;
    s_n_ticks = s_n_beats = s_n_resync = s_n_skip = 0;
    s_jit_min = INT32_MAX; s_jit_max = INT32_MIN; s_err_last = 0;

    const esp_timer_create_args_t ta = {
        .callback = timer_cb, .dispatch_method = ESP_TIMER_TASK, .name = "midi",
    };
    if (esp_timer_create(&ta, &s_timer) != ESP_OK) return ESP_FAIL;

    int prio  = cfg->priority ? cfg->priority : 9;
    int stack = cfg->stack    ? cfg->stack    : 3072;
    BaseType_t ok = xTaskCreatePinnedToCore(midi_task, "midi_clk", stack, NULL,
                                            prio, &s_task,
                                            cfg->core_id < 0 ? tskNO_AFFINITY
                                                             : cfg->core_id);
    if (ok != pdPASS) {
        esp_timer_delete(s_timer); s_timer = NULL; s_task = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "engine started (core %d, prio %d, %d PPQN)",
             cfg->core_id, prio, MIDI_CLOCK_PPQN);
    schedule_next(esp_timer_get_time());
    return ESP_OK;
}

void midi_clock_stop(void)
{
    if (!s_task) return;
    s_stopping = true;
    if (s_timer) { esp_timer_stop(s_timer); }
    xTaskNotifyGive(s_task);
    /* the task emits the Stop, deletes itself and clears s_task */
    for (int i = 0; i < 50 && s_task; i++) vTaskDelay(pdMS_TO_TICKS(2));
    if (s_timer) { esp_timer_delete(s_timer); s_timer = NULL; }
    s_locked = false; s_armed = false;
}

void midi_clock_set_running(bool running)
{
    s_running = running;
}

bool midi_clock_is_locked(void)
{
    return s_locked && s_armed;
}

void midi_clock_stats(midi_clock_stats_t *out)
{
    if (!out) return;
    out->ticks   = s_n_ticks;
    out->beats   = s_n_beats;
    out->resyncs = s_n_resync;
    out->skips   = s_n_skip;
    out->jit_min_us = (s_jit_min == INT32_MAX) ? 0 : s_jit_min;
    out->jit_max_us = (s_jit_max == INT32_MIN) ? 0 : s_jit_max;
    out->err_last_us = s_err_last;
    out->locked = s_locked && s_armed;

    s_n_ticks = s_n_beats = s_n_resync = s_n_skip = 0;
    s_jit_min = INT32_MAX; s_jit_max = INT32_MIN;
}
