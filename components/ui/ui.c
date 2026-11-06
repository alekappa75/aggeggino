#include "ui.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <stdarg.h>

#include "display.h"
#include "bsp_cores3.h"
#include "audio.h"
#include "settings.h"
#include "osc_out.h"
#include "settings_ui.h"

static const char *TAG = "ui";
static lv_display_t *s_disp;
static esp_lcd_panel_handle_t s_panel;

/* --- palette --- */
#define C_BG       lv_color_hex(0x0E1116)
#define C_PANEL    lv_color_hex(0x151A21)
#define C_STRIP    lv_color_hex(0x1A1F26)
#define C_DIV      lv_color_hex(0x242A32)
#define C_AMBER    lv_color_hex(0xEFA02A)
#define C_AMBERD   lv_color_hex(0x5A4A2A)
#define C_CYAN     lv_color_hex(0x2DD4D4)
#define C_CYANBG   lv_color_hex(0x123C42)
#define C_WHITE    lv_color_hex(0xFFFFFF)
#define C_GREY     lv_color_hex(0x8A9098)
#define C_MUTED    lv_color_hex(0x8A9098)
#define C_MUTED2   lv_color_hex(0x565E68)
#define C_GREEN    lv_color_hex(0x4CC94C)
#define C_RED      lv_color_hex(0xE24B4A)
#define C_DIM      lv_color_hex(0x3C444E)
/* Grey for the "up but inert" state of the strip badges.
 *
 * Deliberately VERY dark: the inert state must not draw the eye, it is
 * recognised by position. That way the change to green or red is a clean jump
 * rather than a shift in hue. The ink follows it automatically (badge_ink
 * divides it by 5) and lands just below C_BG.
 *
 * HUE. The value is chosen to match the same slate cast as C_BG (0x0E1116), a
 * cold grey with a BLUE bias: ratios R:G:B = 1 : 1.21 : 1.57. Note, though,
 * that deepening the slate is NOT simply raising the blue: blue weighs only
 * 0.0722 in luminance against green's 0.7152, so it is the green that must be
 * held still to preserve brightness, while the blue can move almost freely.
 *
 * PANEL CONSTRAINT. The display is RGB565: red and blue have 32 levels, a step
 * of 8, green has 64, a step of 4. Adjustments finer than that fall in the same
 * bucket and produce NO effect — 0x2A3038 and 0x293138 are the same colour on
 * screen. Variations must therefore be reasoned in whole buckets, not in units.
 *
 * This value is the earlier 0x2A3038 with the blue raised by ONE bucket: the
 * rendering goes from (41,49,58) to (41,49,66) and the B/R ratio from 1.41 to
 * 1.61, in line with C_BG, at a cost of +3% luminance, imperceptible.
 * The value it started from, should this prove too cold:
 *
 *     #define C_BADGEGREY lv_color_hex(0x2A3038)     <- the neutral version
 *
 * A further step of blue (0x2A304A, B/R 1.80) exists but leans towards indigo
 * and loses the character of a grey. */
#define C_BADGEGREY lv_color_hex(0x2A3042)
#define C_DIMTXT   lv_color_hex(0x4E5660)
#define C_LABEL    lv_color_hex(0xC8CCD2)

#define OFFSET_STEP_US 500      /* adjustment step: 0.5 ms */
#define OFFSET_MAX_US  500000   /* +-500 ms */

/* --- shared state (bridge <-> UI), guarded by a spinlock --- */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static ui_state_t   s_state;
/* At start-up there is NO source: the user chooses it, deliberately. The
 * settings module restores the saved one here by calling
 * ui_set_clock_src_initial() once, before the refresh timer starts — see ui.h.
 *
 * ONE VALUE, not two flags: the two directions are mutually exclusive, so
 * "both at once" is a state that cannot be written down. */
static clock_src_t  s_clock_src = CLOCK_SRC_NONE;

/* Diagnostics: the Ableton source has switched itself off at least twice, once
 * with the deactivation sound — which ONLY the touch handler emits. These lines
 * record which route it went through. */
static bool         s_log_touch_abl = false;
static bool         s_ableton_src_available = true; /* false: four players, no 1..4 identity */

/* TRACK on the master. Written by the bridge (core 0), read by the refresh (core 1). */
#define TRACK_TXT_MAX      64
#define TRACK_ALTERNATE_MS 3000        /* how often title and artist alternate */
#define TRACK_W            278         /* usable width: the label starts at x=8 and
                                        * the padlock at x=292, so 8+278=286 leaves
                                        * 6 px of margin. Changing one without the
                                        * other runs text over the padlock. */
static bool         s_trk_dirty = false;   /* track changed: needs remeasuring */
static bool         s_trk_fits  = false;   /* do title and artist fit together? */
static char         s_trk_both[TRACK_TXT_MAX * 2 + 4] = {0};
/* Memory for the label's change detection. It lives at file scope, not inside
 * refresh_timer, because the MEASUREMENT must be able to invalidate it: to
 * measure we write the text into the real label, and that text must then be
 * rewritten with the right one. Without this invalidation the change detection
 * saw no difference and the label kept the string written by the measurement —
 * showing the track name even with playback stopped. */
static char         s_p_txt[TRACK_TXT_MAX * 2 + 4] = { 1 };   /* != "": forces the first draw */
static int          s_p_istitle = -1;
static char         s_trk_title[TRACK_TXT_MAX]  = {0};
static char         s_trk_artist[TRACK_TXT_MAX] = {0};
static bool         s_trk_valid = false;

/* Touch on an UNAVAILABLE control: a brief red flash says "I felt the touch,
 * but this cannot be done". Without feedback, in the dark, one assumes the
 * touchscreen is broken. */
#define REJECT_FLASH_US  300000
static int64_t      s_reject_flash = 0;
static int64_t      s_lock_flash   = 0;   /* the padlock says "no" */
#if DJB_DEVELOPER
/* Who actually writes the file. A callback rather than a call, so ui does not
 * have to know about the card: it knows a gesture happened, main knows what to
 * do with it. */
static ui_screenshot_fn s_shot_fn = NULL;

/* The setting, kept here rather than read where it is used.
 *
 * The flush runs on every frame and settings_get_screenshots() goes to NVS —
 * far more expensive than the copy it guards. IT IS NOT POLLED EITHER: the
 * value changes only when somebody touches the menu, so it is read once at
 * start-up and set again from the handler that changed it. Re-reading it at
 * 20 Hz would be work for a question whose answer nobody has altered. */
static bool s_shots_on = false;



void ui_set_screenshots(bool on) { s_shots_on = on; }
void ui_set_screenshot_fn(ui_screenshot_fn fn) { s_shot_fn = fn; }
#endif
static lv_obj_t    *s_lock_part[3];       /* body and the two legs  */
static lv_obj_t    *s_lock_arc;           /* the shackle            */
static bool         s_ui_locked    = false;  /* future: lock via the centre button */
static lv_obj_t    *s_dash_scr     = NULL;    /* dashboard screen, to return to from settings */
static int          s_offset_dj_us    = 0;      /* FWD offset: DJ Link -> Ableton, us */
static int          s_offset_abl_us = 0;   /* REV offset: Ableton -> DJ Link, us */
/* OSC output offset, indexed by the clock SOURCE:
 *   [0] = Pioneer DJ Link   (forward bridge active)
 *   [1] = Ableton Link      (reverse bridge active)
 * The delay to compensate depends on both ends of the chain, so switching
 * bridge makes a different value the right one. Keeping two avoids redoing by
 * ear an adjustment already made. */
static int          s_offset_osc_us[2] = { 0, 0 };
/* MIDI offset, with the same structure as the OSC one: indexed by the clock
 * SOURCE, because trimming phase towards a DAW while the tempo comes from
 * DJ Link is a different matter from trimming it when it comes from Ableton. */
static int          s_offset_midi_us[2] = { 0, 0 };
/* A MIDI transport is up: main says so, since it knows whether the network host
 * started and whether the DIN UART is active. midi2_net is not included here.
 *
 * VOLATILE, and not out of pedantry: it is WRITTEN by the main task on core 0
 * and READ by off_dest_active() from the LVGL task on core 1. Without it the
 * compiler may keep a copy in a register — and off_dest_active is inlined into
 * off_dest_count(), which calls it in a loop: exactly the case where the read
 * is hoisted out of the loop and the new value is never seen. All the other
 * shared state in this file goes through the s_mux critical section; this was
 * the only one outside it. */
static volatile bool s_midi_out_up = false;
/* CV offset, indexed by source like OSC and MIDI. Kept SEPARATE from the MIDI
 * one on purpose: CV goes through the synthesiser, which adds a latency of its
 * own — compensating it with the MIDI number would put one or the other out of
 * phase. */
static int          s_offset_cv_us[2] = { 0, 0 };
static volatile bool s_cv_out_up = false;

/* Index of the active source. With the reverse bridge the clock comes from
 * Ableton Link, otherwise from the players — the same rule clock_next_beat
 * uses. */
static inline int osc_src_idx(void) { return s_clock_src == CLOCK_SRC_ABLETON ? 1 : 0; }

/* ===========================================================================
 * OFFSET DESTINATION SELECTOR
 *
 * A phase offset compensates the latency of the chain DOWNSTREAM, so one is
 * needed per destination: the far end of the bridge, the OSC output, the MIDI
 * output. The label names the destination because that is what the value acts
 * on; the source stays implicit, being whichever bridge is active at the time.
 *
 * ONE FIELD THAT CYCLES, rather than a box per destination: the screen is
 * already dense, a grid would grow with every new output, and the information
 * needed at any moment is always just one of them.
 *
 * The "focus" is deliberately weak: it is not a selection to be maintained,
 * only what is currently being looked at. The arrows below the screen always
 * adjust the destination on show.
 *
 * With ONE active destination the element simplifies itself: generic label, no
 * indicator, nothing to learn. With none it disappears.
 * =========================================================================== */
typedef enum {
    OFF_DEST_BRIDGE = 0,   /* the far end of the active bridge */
    OFF_DEST_OSC,
    OFF_DEST_MIDI,
    OFF_DEST_CV,
    OFF_DEST_COUNT
} off_dest_t;

static off_dest_t   s_off_focus = OFF_DEST_BRIDGE;   /* what is being looked at */

/* Which destinations exist RIGHT NOW. */
static bool off_dest_active(off_dest_t d)
{
    switch (d) {
    case OFF_DEST_BRIDGE: return s_clock_src != CLOCK_SRC_NONE;
    /* OSC appears among the destinations only when it is genuinely going out.
     * The settings flag is not enough: a valid destination, an open socket AND
     * an active bridge to supply the clock are all required.
     *
     * That last condition was missing, and the result was a "PHASE OFFSET"
     * field appearing with the bridge off — OSC enabled but silent, because
     * without a bridge clock_next_beat has no phase to give. One was adjusting
     * the offset of a stopped output. */
    case OFF_DEST_OSC:    return osc_out_active() &&
                                 (s_clock_src != CLOCK_SRC_NONE);
    /* MIDI: the same two conditions as OSC. The transport must be up — main
     * says so through ui_set_midi_out_active — AND a bridge must supply the
     * clock, or one would be trimming the phase of a stopped output. */
    case OFF_DEST_MIDI:   return s_midi_out_up &&
                                 (s_clock_src != CLOCK_SRC_NONE);
    /* CV: the same two conditions. */
    case OFF_DEST_CV:     return s_cv_out_up &&
                                 (s_clock_src != CLOCK_SRC_NONE);
    default:              return false;
    }
}

static int off_dest_count(void)
{
    int n = 0;
    for (int d = 0; d < OFF_DEST_COUNT; d++) if (off_dest_active((off_dest_t)d)) n++;
    return n;
}

/* Name of the DESTINATION the offset acts on. For the bridge that is the far
 * end: with the forward one on we are delaying what reaches Ableton, with the
 * reverse one what reaches the players. */
static const char *off_dest_name(off_dest_t d)
{
    switch (d) {
    case OFF_DEST_BRIDGE: return (s_clock_src == CLOCK_SRC_ABLETON) ? "DJ LINK" : "ABLETON";
    case OFF_DEST_OSC:    return "OSC";
    case OFF_DEST_MIDI:   return "MIDI";
    case OFF_DEST_CV:     return "CV";
    /* The default stays but must no longer be REACHABLE: it was what produced
     * the empty label when the focus landed on CV. Adding a destination to the
     * enum means adding a line here too, and the way to notice is precisely
     * this string — if it appears, a case is missing. */
    default:              return "?";
    }
}

/* The focus must always point at something active: if the destination being
 * looked at is switched off, it falls back to the first available one. */
static void off_focus_validate(void)
{
    if (off_dest_active(s_off_focus)) return;
    for (int d = 0; d < OFF_DEST_COUNT; d++)
        if (off_dest_active((off_dest_t)d)) { s_off_focus = (off_dest_t)d; return; }
    s_off_focus = OFF_DEST_BRIDGE;
}

static void off_focus_next(void)
{
    for (int i = 1; i <= OFF_DEST_COUNT; i++) {
        off_dest_t d = (off_dest_t)((s_off_focus + i) % OFF_DEST_COUNT);
        if (off_dest_active(d)) { s_off_focus = d; return; }
    }
}
static bool         s_offset_force  = false;  /* offset changed -> force at once */

/* State of the silkscreened buttons, LVGL task only. */
static int      s_btn = 0;               /* -1 left, +1 right, 0 none or centre */
static int64_t  s_btn_start = 0, s_btn_rep = 0;
static bool     s_center_fired = false;   /* centre long-press already fired */
static int64_t  s_offset_touched_us = 0;  /* for the visual feedback */
static bool     s_offset_dirty = false;
static int64_t  s_offset_change_us = 0;    /* for the save debounce */

/* --- references to the dynamic objects --- */
static lv_obj_t *s_bpm_big, *s_bpm_lbl;                 /* the giant shared BPM */
static lv_obj_t *s_bpm_dj, *s_bpm_link;                 /* two grey BPMs, sync off */
static lv_obj_t *s_bpm_dj_l, *s_bpm_link_l;             /* labels under those two */
static lv_obj_t *s_tl, *s_tl_arrow;                     /* left toggle */
static lv_obj_t *s_tr, *s_tr_arrow;                     /* right toggle, the reverse one */
static lv_obj_t *s_master, *s_beat[4], *s_peer, *s_offset_val, *s_lock;
static lv_obj_t *s_offset_lbl;   /* the "PHASE OFFSET" label: hides along with the value */
static lv_obj_t *s_offset_dest;  /* line above: destination name, when more than one */
static lv_obj_t *s_offset_dot[OFF_DEST_COUNT];  /* position indicators in the cycle */
/* Geometry of the dots: the CENTRE is the fixed point and the corner is derived
 * from it, so the focused dot can grow without moving.
 *
 * PARITY CONSTRAINT. The division is integer, so the true centre of a box of
 * diameter d falls at X + (d-1)/2: with d EVEN that lands on half a pixel, with
 * d ODD on a pixel. Two diameters of different parity CANNOT share the same
 * centre on the grid, however the origin is shifted — which was the case with 6
 * and 7, giving centres of 307.5 and 308.0. The focused dot grew only to the
 * right, and it showed.
 *
 * The two diameters must therefore keep the SAME parity: 5 and 7 both land on
 * exactly 308.0. If they are changed, change them as a pair. */
#define OFF_DOT_D_ON   7        /* focused */
#define OFF_DOT_D_OFF  5        /* the others: same parity, see above */
#define OFF_DOT_CX   308
#define OFF_DOT_CY0  161
#define OFF_DOT_STEP 11
#define OFF_DOT_X(d)     (OFF_DOT_CX - (d) / 2)
#define OFF_DOT_Y(i, d)  (OFF_DOT_CY0 + (i) * OFF_DOT_STEP - (d) / 2)
static lv_obj_t *s_offset_hit;   /* transparent rectangle that catches the touch */
static lv_obj_t *s_track;                               /* track on the master, at the top */
/* ===========================================================================
 * STATUS STRIP (y 209..239): a row of SOLID badges.
 *
 * Every component gets its badge: background in the state colour, initials in
 * dark ink on top — the same style as the little square holding the master
 * number. Three colours only, and NO BLINKING:
 *
 *     red    faulty: something is not working
 *     grey   up but inert: it works, it is doing nothing
 *     green  up and in use
 *
 * The badges report the state of the INFRASTRUCTURE, not of the protocol: the
 * LAN badge turns green when the chip answers and the cable is in, regardless
 * of what DJ Link is doing. Protocol state is already on screen where it
 * genuinely matters — the MASTER badge, the beat cells, the BPM — and
 * duplicating it down here added nothing. The side LEDs do stay on the
 * protocol, with their six states and their blinking: they are two different
 * indicators, see ui.h.
 *
 * ON THE LEFT the LAN chain and the auxiliary outputs, in the order LAN,
 * [MIDI], OSC. ON THE RIGHT the WiFi chain, anchored to the right edge:
 *   SoftAP -> [WIFI AP] followed by the number of connected clients
 *   client -> [WIFI] followed by the signal strength bars, preceded by a red
 *             [AUTH] when the key is refused
 * The badges COMPACT from both sides: if one disappears the others slide up and
 * stay adjacent, without leaving the hole where it was.
 * =========================================================================== */
#define STRIP_Y        215     /* badge row: the single vertical anchor        */
#define STRIP_H         18
#define STRIP_PAD_X      5     /* air left and right of the initials          */
#define STRIP_GAP        6     /* between one element and the next            */
#define STRIP_L_X0       8     /* origin of the left group                    */
#define STRIP_R_END    313     /* first pixel AFTER the right group           */
/* --- signal strength glyph (client mode) ---
 * The geometry is DERIVED from STRIP_H, not written by hand: the base of the
 * bars and the top of the tallest must line up with the bottom and top edges of
 * the badge beside them. A literal 18 would happen to equal STRIP_H and look
 * right, then break silently at the first adjustment of the badge height. */
#define BAR_W            4     /* width of one bar                            */
#define BAR_SP           3     /* space between two bars                      */
#define BAR_STEP         4     /* height step from one bar to the next        */
#define STRIP_BARS_W     (4 * BAR_W + 3 * BAR_SP)
/* Air between the WIFI badge and the glyph. Wider than STRIP_GAP, which
 * separates two SOLID rectangles of equal visual weight: here what sits to the
 * right is a sparse glyph, bottom-heavy, and at only 6 px it looked stuck to
 * the badge. */
#define STRIP_BARS_GAP  10
#define STRIP_NUM_MAX   12     /* count buffer: room for a whole int */

/* Strip font: Montserrat BOLD at 12 px. LVGL does not ship bold variants — its
 * lv_font_montserrat_* are all Medium weight — so this one is generated and
 * lives in fonts/lv_font_montserrat_bold_12.c.
 *
 * Same line_height (13) and same base_line (2) as the Medium, so it drops in
 * without shifting anything vertically; the initials widen by less than a
 * pixel, and the badge width adapts by itself because it is MEASURED
 * (strip_text_w).
 *
 * TO REGENERATE IT, or to produce other sizes. Google Fonts now distributes
 * only the variable font, which must first be instanced at the wanted weight:
 *
 *   pip install fonttools
 *   python -c "from fontTools import ttLib; \
 *     from fontTools.varLib.instancer import instantiateVariableFont as I; \
 *     I(ttLib.TTFont('Montserrat[wght].ttf'), {'wght':700}).save('Montserrat-Bold.ttf')"
 *   npm install -g lv_font_conv
 *   lv_font_conv --no-compress --no-prefilter --bpp 4 --size 12 \
 *       --font Montserrat-Bold.ttf -r 0x20-0x7F \
 *       --format lvgl --force-fast-kern-format --lv-include lvgl.h \
 *       -o lv_font_montserrat_bold_12.c
 *
 * The options are NOT arbitrary: they are the same ones LVGL uses to generate
 * its own built-in fonts (scripts/built_in_font/built_in_font_gen.py). They
 * were verified by regenerating the lv_font_montserrat_12.c that LVGL 9.5 ships
 * and comparing it with the original: structure, descriptor and tables match.
 * When changing size, change the symbol name and the line in CMakeLists.txt
 * too. */
LV_FONT_DECLARE(lv_font_montserrat_bold_12);
#define STRIP_FONT (&lv_font_montserrat_bold_12)

typedef struct {
    lv_obj_t *box;     /* the solid rectangle            */
    lv_obj_t *txt;     /* the initials, in dark ink      */
    int       w;       /* current width, measured        */
    bool      shown;   /* visible right now              */
} badge_t;

static bool      s_wifi_ap_initial = true;   /* SoftAP unless told otherwise */

void ui_set_wifi_ap_initial(bool ap) { s_wifi_ap_initial = ap; }


static badge_t   s_bg_lan, s_bg_midi, s_bg_osc, s_bg_cv, s_bg_sd, s_bg_wifi, s_bg_auth, s_bg_num;
/* The right-hand badge changes its initials, and therefore its width, with the
 * WiFi mode: the two widths are measured once at start-up. */
static int       s_w_wifiap = 0, s_w_wifi = 0;
static lv_obj_t *s_sig_bars[4];   /* signal bars, client mode               */
static lv_obj_t *s_mbadge, *s_mtxt, *s_msq;   /* MASTER badge: recoloured when it is us */



void ui_publish(const ui_state_t *s)
{
    portENTER_CRITICAL(&s_mux);
    s_state = *s;
    portEXIT_CRITICAL(&s_mux);
}
int ui_get_phase_offset_dj_us(void)      /* direct (DJ -> Ableton) */
{
    portENTER_CRITICAL(&s_mux);
    int v = s_offset_dj_us;
    portEXIT_CRITICAL(&s_mux);
    return v;
}

/* OSC output offset: it compensates the latency of the chain downstream, so it
 * is independent of the bridge offsets. Adjustable from the selector and
 * persisted per source. */
int ui_get_phase_offset_osc_us(void)
{
    portENTER_CRITICAL(&s_mux);
    int v = s_offset_osc_us[osc_src_idx()];
    portEXIT_CRITICAL(&s_mux);
    return v;
}

int ui_get_phase_offset_midi_us(void)
{
    portENTER_CRITICAL(&s_mux);
    int v = s_offset_midi_us[osc_src_idx()];
    portEXIT_CRITICAL(&s_mux);
    return v;
}

void ui_set_midi_out_active(bool on)
{
    s_midi_out_up = on;
}

int ui_get_phase_offset_cv_us(void)
{
    portENTER_CRITICAL(&s_mux);
    int v = s_offset_cv_us[osc_src_idx()];
    portEXIT_CRITICAL(&s_mux);
    return v;
}

void ui_set_cv_out_active(bool on)
{
    s_cv_out_up = on;
}

int ui_get_phase_offset_abl_us(void)  /* reverse: Ableton -> DJ */
{
    portENTER_CRITICAL(&s_mux);
    int v = s_offset_abl_us;
    portEXIT_CRITICAL(&s_mux);
    return v;
}

/* True ONCE if the offset has just been changed: the bridge uses it to force an
 * immediate re-lock while the user is adjusting. */
bool ui_offset_changed(void)
{
    portENTER_CRITICAL(&s_mux);
    bool f = s_offset_force;
    s_offset_force = false;
    portEXIT_CRITICAL(&s_mux);
    return f;
}

/* Largest multiple of STEP <= v: floor onto the grid, correct for negatives too. */
static int floor_grid(int v)
{
    int q = v / OFFSET_STEP_US;
    if (v % OFFSET_STEP_US != 0 && v < 0) q--;
    return q * OFFSET_STEP_US;
}

/* dir = +1 or -1: step to the next grid point in the direction pressed,
 * absorbing any off-grid remainder — clean 0.5 ms increments. */
static void offset_bump(int dir)
{
    /* With no active destination there is nothing to adjust: the control is
     * inert, consistent with the offset not even being shown. */
    if (off_dest_count() == 0) return;   /* no destination to adjust */

    portENTER_CRITICAL(&s_mux);
    /* The FOCUSED destination is adjusted, that is the one the field is
     * showing: the arrows and the display cannot refer to different things. */
    bool osc  = (s_off_focus == OFF_DEST_OSC);
    bool midi = (s_off_focus == OFF_DEST_MIDI);
    bool cv   = (s_off_focus == OFF_DEST_CV);
    bool abl  = (s_clock_src == CLOCK_SRC_ABLETON);   /* which source gives the tempo */
    int cur = osc  ? s_offset_osc_us[osc_src_idx()]
            : midi ? s_offset_midi_us[osc_src_idx()]
            : cv   ? s_offset_cv_us[osc_src_idx()]
                   : (abl ? s_offset_abl_us : s_offset_dj_us);
    int nv  = (dir > 0) ? floor_grid(cur) + OFFSET_STEP_US       /* next one up   */
                        : -floor_grid(-cur) - OFFSET_STEP_US;    /* next one down */
    if (nv < -OFFSET_MAX_US) nv = -OFFSET_MAX_US;
    if (nv >  OFFSET_MAX_US) nv =  OFFSET_MAX_US;
    if (osc)       s_offset_osc_us[osc_src_idx()]  = nv;
    else if (midi) s_offset_midi_us[osc_src_idx()] = nv;
    else if (cv)   s_offset_cv_us[osc_src_idx()]   = nv;
    else if (abl)  s_offset_abl_us = nv;
    else           s_offset_dj_us     = nv;
    s_offset_force = true;     /* real-time feedback: re-lock at once */
    portEXIT_CRITICAL(&s_mux);
    s_offset_touched_us = esp_timer_get_time();
    s_offset_change_us  = s_offset_touched_us;
    s_offset_dirty = true;
    audio_play(AUDIO_PHASE_STEP);   /* a dry click at every phase step */
}

clock_src_t ui_clock_src(void)
{
    portENTER_CRITICAL(&s_mux);
    clock_src_t s = s_clock_src;
    portEXIT_CRITICAL(&s_mux);
    return s;
}

/* ---------------------------------------------------------------------------
 * TOUCH DEBOUNCE
 *
 * In the field the reverse bridge switched itself off several times, and the
 * diagnostics said it came through the TOUCH: the capacitive panel produces
 * phantom contacts. A toggle that flips by itself during a set is not an
 * annoyance, it is a fault.
 *
 * The filter is on STABILITY, not on elapsed time: a state change is reported
 * to LVGL only after the raw reading confirms it for N consecutive samples. A
 * spurious contact is typically an isolated sample, and so does not get
 * through.
 *
 * The position reported is that of the FIRST sample of the run, not the last:
 * during the confirming reads the finger may already have moved, and using the
 * last would displace the touch from where it began.
 *
 * COST: the recognition delay is N times the indev read period. With three
 * samples it stays under a tenth of a second, which on a toggle is not
 * perceptible. The release uses the same threshold, so a micro-detachment
 * during a press does not produce a spurious release. */
/* N=2: a single confirming read is enough. It filters out the spurious contacts
 * of ONE isolated sample — the ones that were switching the bridge off — and
 * nothing more. N=3 was too much: it ate short taps and the delay was felt at
 * the fingertip. */
#define TOUCH_DEBOUNCE_N   2

static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    static lv_indev_state_t reported = LV_INDEV_STATE_RELEASED;  /* reported state */
    static int32_t  px = 0, py = 0;                              /* reported point */
    static uint8_t  agreed = 0;                               /* agreeing reads */
    static lv_indev_state_t candidate = LV_INDEV_STATE_RELEASED;
    (void)indev;

    uint16_t x, y;
    bool down = bsp_touch_read(&x, &y) && y <= 239;
    lv_indev_state_t raw = down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;

    if (raw == reported) {
        agreed = 0;                       /* nothing to confirm */
    } else if (raw != candidate) {
        candidate = raw;                     /* new candidate: start over */
        agreed = 1;
        if (down) { px = (int32_t)x; py = (int32_t)y; }  /* starting point */
    } else if (++agreed >= TOUCH_DEBOUNCE_N) {
        reported = candidate;                /* confirmed */
        agreed = 0;
    }

    data->point.x = px;
    data->point.y = py;
    data->state   = reported;
}

static lv_obj_t *rect(lv_obj_t *p, int x, int y, int w, int h,
                      lv_color_t bg, lv_opa_t bo, lv_color_t bd, int bw, int r)
{
    lv_obj_t *o = lv_obj_create(p);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(o, x, y); lv_obj_set_size(o, w, h);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_set_style_shadow_width(o, 0, 0);
    lv_obj_set_style_bg_color(o, bg, 0); lv_obj_set_style_bg_opa(o, bo, 0);
    lv_obj_set_style_border_color(o, bd, 0); lv_obj_set_style_border_width(o, bw, 0);
    lv_obj_set_style_radius(o, r, 0);
    return o;
}
static lv_obj_t *label(lv_obj_t *p, int x, int y, const char *t,
                       const lv_font_t *f, lv_color_t c)
{
    lv_obj_t *l = lv_label_create(p);
    lv_label_set_text(l, t);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, c, 0);
    lv_obj_set_pos(l, x, y);
    return l;
}


/* Vector padlock glyph, drawn into the parent container `p` at origin (x,y).
 * Body = filled rounded rect; shackle = a semicircular arc (opening down) plus
 * two short vertical legs. The whole thing fits in a 24x24 box like the old
 * bitmap. Same anti-clipping trick as the AP arc: pad the arc box so the 3px
 * stroke ends aren't cut off. */
/* Colours the padlock: grey normally, red for the instant after a refused
 * touch. Two callers — the flash edge and the unlock — so the four objects that
 * make up the glyph are painted in one place rather than two. */
static void lock_glyph_color(lv_color_t c)
{
    if (!s_lock_arc) return;                  /* not built yet */
    for (int i = 0; i < 3; i++) {
        lv_obj_set_style_bg_color(s_lock_part[i], c, 0);
        lv_obj_set_style_border_color(s_lock_part[i], c, 0);
    }
    lv_obj_set_style_arc_color(s_lock_arc, c, LV_PART_MAIN);
}


static void build_lock_glyph(lv_obj_t *p, int x, int y, lv_color_t col)
{
    const int PAD = 4;
    /* body: rounded rect, lower portion (y 11..23, x 2..21) */
    s_lock_part[0] = rect(p, x + 2, y + 11, 20, 12, col, LV_OPA_COVER, col, 0, 3);

    /* shackle arc: a semicircle opening downward. Centre at (x+12, y+8),
     * radius 7, stroke 3. Angles 180..360 = the top half (opens down). */
    int cx = x + 12, cy = y + 8, r = 7;
    int box = r * 2 + PAD * 2;
    lv_obj_t *arc = lv_arc_create(p);
    s_lock_arc = arc;
    lv_obj_set_size(arc, box, box);
    lv_obj_set_pos(arc, cx - box / 2, cy - box / 2);
    lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_clip_corner(arc, false, 0);
    lv_obj_set_style_pad_all(arc, PAD, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, 3, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, col, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(arc, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_arc_set_bg_angles(arc, 180, 360);   /* top half, opening downward */

    /* two vertical legs of the shackle, from the arc ends down into the body */
    s_lock_part[1] = rect(p, x + 4,  y + 8, 3, 4, col, LV_OPA_COVER, col, 0, 0);
    s_lock_part[2] = rect(p, x + 17, y + 8, 3, 4, col, LV_OPA_COVER, col, 0, 0);

    /* THE PIECES DO NOT TAKE TOUCHES. They are children of the full-screen
     * container, and lv_obj_create makes objects clickable by default: a tap
     * landing on the padlock itself would stop at the glyph and never reach the
     * handler on the container, so that one corner of a locked screen would
     * answer differently from the rest. */
    for (int i = 0; i < 3; i++) lv_obj_remove_flag(s_lock_part[i], LV_OBJ_FLAG_CLICKABLE);

}

/* Build 4 vertical signal bars (phone-style, growing left-to-right) for client
 * mode. They live at the given position; caller keeps refs in s_sig_bars[].
 * Heights: 6, 10, 14, 18 px. Filled = green, empty = dark grey (C_MUTED2). */
static void build_signal_bars(lv_obj_t *p, int x, int y)
{
    for (int i = 0; i < 4; i++) {
        int h  = STRIP_H - (3 - i) * BAR_STEP;   /* the last is as tall as the badge */
        int bx = x + i * (BAR_W + BAR_SP);
        int by = y + (STRIP_H - h);              /* bottom-aligned */
        s_sig_bars[i] = rect(p, bx, by, BAR_W, h,
                             C_BADGEGREY, LV_OPA_COVER, C_BADGEGREY, 0, 1);
    }
}

/* Update the signal bars from an RSSI value (dBm). Thresholds:
 *   >= -55 -> 4 bars, -65..-55 -> 3, -75..-65 -> 2, < -75 -> 1, 0/none -> 0. */
static void signal_bars_set_rssi(int rssi)
{
    int lit;
    if (rssi == 0)       lit = 0;   /* not connected */
    else if (rssi >= -55) lit = 4;
    else if (rssi >= -65) lit = 3;
    else if (rssi >= -75) lit = 2;
    else                  lit = 1;

    /* Change detection on the NUMBER of bars, not on the RSSI. The dBm figure
     * wanders continuously (-61, -63, -62...) while the bars change only when a
     * threshold is crossed, which is almost never. Comparing the RSSI meant
     * rewriting four styles a second to change nothing visible, and every write
     * is an invalidated area, a redraw and a flush contending for the SPI bus
     * with the W5500. */
    static int p_lit = -1;
    if (lit == p_lit) return;
    p_lit = lit;

    for (int i = 0; i < 4; i++) {
        /* Unlit bar: the SAME grey as the inert badges, not a near black darker
         * than the background — that would make unlit bars disappear entirely
         * instead of showing them as "present but off", and give this one
         * element a grammar of its own. */
        lv_color_t c = (i < lit) ? C_GREEN : C_BADGEGREY;
        lv_obj_set_style_bg_color(s_sig_bars[i], c, 0);
    }
}

/* ---------------------------------------------------------------------------
 * STATUS STRIP BADGES
 * ------------------------------------------------------------------------- */

/* Width in pixels of a string in the strip font.
 *
 * Measured with a TEMPORARY label hung off the screen, not inside the badge: a
 * label that is the child of a narrow box would wrap instead of telling us how
 * wide it is, and would answer with the parent's width. Hung off the screen it
 * has no constraints, and its width is the text's. */
static int strip_text_w(const char *txt)
{
    lv_obj_t *t = lv_label_create(lv_screen_active());
    lv_obj_set_style_text_font(t, STRIP_FONT, 0);
    lv_label_set_text(t, txt);
    lv_obj_update_layout(t);
    int w = lv_obj_get_width(t);
    lv_obj_delete(t);
    return w;
}

/* Ink for the initials: a very dark variant of the SAME hue as the badge
 * background, channels divided by 5. Never a neutral black — on saturated
 * backgrounds it jars, and a variant of the hue adds no colours to the palette.
 *
 * ONE RULE for every badge, live and inert alike. It has a consequence worth
 * knowing, because it is intended and not a defect:
 *
 *   background            ink           contrast
 *   green  0x4CC94C       0x0F280F        7.3     reads clearly
 *   red    0xE24B4A       0x2D0F0E        4.5     reads clearly
 *   grey   0x2A3038       0x08090B        1.5     barely reads
 *
 * On the grey the ink lands at 0x08090B, JUST DARKER than the dashboard
 * background (C_BG = 0x0E1116): the initials read as a shadow engraved into the
 * badge rather than as text. That is the effect wanted — the inert state must
 * not draw the eye, it is recognised by position — and it is why the jump to
 * green or red comes across as clean.
 *
 * A note for anyone adjusting C_BADGEGREY: the ink follows it automatically,
 * always keeping that ratio. An earlier version reversed the direction on dark
 * backgrounds (light ink, contrast about 2.1); it was abandoned because the
 * inert badge ended up more conspicuous than the live ones. */
static lv_color_t badge_ink(lv_color_t c)
{
    return lv_color_make(c.red / 5, c.green / 5, c.blue / 5);
}

static void badge_set_text(badge_t *b, const char *txt, int w)
{
    b->w = w;
    lv_obj_set_width(b->box, w);
    lv_label_set_text(b->txt, txt);
    lv_obj_center(b->txt);
}

static void badge_make(badge_t *b, lv_obj_t *parent, const char *txt, int w)
{
    b->box = rect(parent, 0, STRIP_Y, w, STRIP_H,
                  C_BADGEGREY, LV_OPA_COVER, C_BADGEGREY, 0, 2);
    b->txt = label(b->box, 0, 0, txt, STRIP_FONT, C_BG);
    badge_set_text(b, txt, w);
    lv_obj_add_flag(b->box, LV_OBJ_FLAG_HIDDEN);
    b->shown = false;
}

static void badge_show(badge_t *b, bool on)
{
    if (b->shown == on) return;
    b->shown = on;
    if (on) lv_obj_remove_flag(b->box, LV_OBJ_FLAG_HIDDEN);
    else    lv_obj_add_flag(b->box, LV_OBJ_FLAG_HIDDEN);
}

/* Update a badge, touching NOTHING if the state has not changed.
 *
 * This is where the strip's cost on the SPI bus is decided: every
 * lv_obj_set_style_*() INVALIDATES the object, hence a redraw and a flush
 * contending for the bus with the W5500. With the blinking removed, a badge is
 * repainted ONLY when its state really changes — which is almost never — and at
 * rest the strip costs exactly zero. That was the flaw of the blinking version:
 * four invalidations a second per badge, forever, and precisely in the most
 * frequent states.
 *
 * The BORDER colour is not written: badges are created with a border width of 0
 * (see badge_make), so it would be an invalidation for a pixel that does not
 * exist. If they are ever given a border, it must be added here. */
static void badge_update(badge_t *b, int state, lv_color_t c, int *p_state)
{
    if (state == *p_state) return;
    lv_obj_set_style_bg_color(b->box, c, 0);
    lv_obj_set_style_text_color(b->txt, badge_ink(c), 0);
    *p_state = state;
}

/* Recomputes the positions of the LEFT group. Hidden badges take no room: the
 * fixed order is walked and x accumulates only over the visible ones, so that
 * removing one in the middle leaves the others adjacent.
 *
 * The order is fixed: LAN, MIDI, OSC, CV. */
static void strip_reflow_left(void)
{
    badge_t *const order[] = { &s_bg_lan, &s_bg_midi, &s_bg_osc, &s_bg_cv, &s_bg_sd };
    int x = STRIP_L_X0;
    for (unsigned i = 0; i < sizeof(order) / sizeof(order[0]); i++) {
        if (!order[i]->shown) continue;
        lv_obj_set_pos(order[i]->box, x, STRIP_Y);
        x += order[i]->w + STRIP_GAP;
    }
}

/* Recomputes the positions of the RIGHT group, which grows LEFTWARDS from a
 * fixed edge. It is walked backwards: the trailing element first (the client
 * count or the signal bars, which differ in width), then the WiFi badge, and
 * finally AUTH if present. That way the group stays glued to the edge whatever
 * it is made of. */
static void strip_reflow_right(bool ap_mode)
{
    int x = STRIP_R_END;

    if (ap_mode) {
        /* The count badge exists only with at least one client: when it is
         * absent the WiFi badge slides to the edge instead of leaving its place
         * empty. Its width changes between one and two digits, and is already
         * updated by whoever wrote the text. */
        if (s_bg_num.shown) {
            x -= s_bg_num.w;
            lv_obj_set_pos(s_bg_num.box, x, STRIP_Y);
            x -= STRIP_GAP;
        }
    } else {
        x -= STRIP_BARS_W + STRIP_BARS_GAP;  /* the little bars never move */
    }

    x -= s_bg_wifi.w;
    lv_obj_set_pos(s_bg_wifi.box, x, STRIP_Y);

    if (s_bg_auth.shown) {
        x -= STRIP_GAP + s_bg_auth.w;
        lv_obj_set_pos(s_bg_auth.box, x, STRIP_Y);
    }
}

/* Colour for each state. These three tables are the only place where the strip
 * decides how it looks: changing its appearance requires touching nothing
 * else. */
static lv_color_t lan_color(ui_lan_t s)
{
    switch (s) {
    case UI_LAN_CHIP_KO:  return C_RED;
    case UI_LAN_NO_CABLE: return C_BADGEGREY;
    default:              return C_GREEN;
    }
}

static lv_color_t wifi_color(ui_wifi_t s)
{
    switch (s) {
    case UI_WIFI_KO:   return C_RED;
    case UI_WIFI_IDLE: return C_BADGEGREY;
    default:           return C_GREEN;
    }
}

static lv_color_t osc_color(ui_osc_t s)
{
    switch (s) {
    case UI_OSC_FAULT: return C_RED;
    case UI_OSC_IDLE:  return C_BADGEGREY;
    default:           return C_GREEN;
    }
}

static lv_color_t midi_color(ui_midi_t s)
{
    switch (s) {
    case UI_MIDI_FAULT: return C_RED;
    case UI_MIDI_IDLE:  return C_BADGEGREY;
    default:            return C_GREEN;
    }
}

static lv_color_t cv_color(ui_cv_t s)
{
    switch (s) {
    case UI_CV_FAULT: return C_RED;
    case UI_CV_IDLE:  return C_BADGEGREY;
    default:          return C_GREEN;
    }
}

static lv_color_t sd_color(ui_sd_t s)
{
    switch (s) {
    case UI_SD_ERROR: return C_RED;
    case UI_SD_IDLE:  return C_BADGEGREY;
    default:          return C_GREEN;
    }
}

/* Refreshing the strip. Change detection on every element: with no blinking,
 * at rest nothing is written. The reflow runs only when the COMPOSITION of a
 * group changes — a badge appears, disappears, or changes width — not on every
 * pass. */
/* The MIDI badge sits BEFORE the OSC one in the strip: when it appears or
 * disappears the OSC badge shifts, and its colour must be reapplied even though
 * its own state has not changed. Without this, switching MIDI off left OSC in
 * the right place but with the colour from the time before. */
static bool p_osc_force = false;

static void refresh_strip(const ui_state_t *s)
{
    /* --- LAN: always present --- */
    static int p_lan = -1;
    badge_update(&s_bg_lan, (int)s->lan, lan_color(s->lan), &p_lan);

    /* --- CV: last of the group, so it does NOT shift the others when it
     * appears. Hence no p_*_force is needed, unlike for OSC. --- */
    static int p_cv = -1;
    bool cv_on = (s->cv != UI_CV_OFF);
    if (cv_on != s_bg_cv.shown) {
        badge_show(&s_bg_cv, cv_on);
        strip_reflow_left();
        p_cv = -1;
    }
    if (cv_on) badge_update(&s_bg_cv, (int)s->cv, cv_color(s->cv), &p_cv);

    /* --- SD: after CV for the same reason — appearing last, it shifts
     * nothing. An empty slot hides it entirely rather than showing a grey
     * box for a feature nobody is using. --- */
    static int p_sd = -1;
    bool sd_on = (s->sd != UI_SD_ABSENT);
    if (sd_on != s_bg_sd.shown) {
        badge_show(&s_bg_sd, sd_on);
        strip_reflow_left();
        p_sd = -1;
    }
    if (sd_on) badge_update(&s_bg_sd, (int)s->sd, sd_color(s->sd), &p_sd);

    /* --- MIDI: absent when no transport is enabled in the settings --- */
    static int p_midi = -1;
    bool midi_on = (s->midi != UI_MIDI_OFF);
    if (midi_on != s_bg_midi.shown) {
        badge_show(&s_bg_midi, midi_on);
        strip_reflow_left();
        p_midi = -1;
        p_osc_force = true;   /* OSC has shifted too: reapply its colour */
    }
    if (midi_on) badge_update(&s_bg_midi, (int)s->midi, midi_color(s->midi), &p_midi);

    /* --- OSC: absent when the output is off in the settings --- */
    static int p_osc = -1;
    if (p_osc_force) { p_osc = -1; p_osc_force = false; }
    bool osc_on = (s->osc != UI_OSC_OFF);
    if (osc_on != s_bg_osc.shown) {
        badge_show(&s_bg_osc, osc_on);
        strip_reflow_left();            /* the set changed: compact it again   */
        p_osc = -1;                     /* and the colour must be reapplied    */
    }
    if (osc_on) badge_update(&s_bg_osc, (int)s->osc, osc_color(s->osc), &p_osc);

    /* --- WiFi: the badge changes label, and with it width, when the mode changes */
    /* Seeded with the mode the badge was DRAWN in, not with -1: at -1 the
     * first refresh always finds a change and redraws a badge that is already
     * correct — which is the flicker this avoids. */
    static int p_ap_mode = -2;
    if (p_ap_mode == -2) p_ap_mode = (int)s_wifi_ap_initial;
    static int p_clients = -1;
    bool relayout = false;
    if ((int)s->wifi_ap != p_ap_mode) {
        if (s->wifi_ap) {
            badge_set_text(&s_bg_wifi, "WIFI AP", s_w_wifiap);
            for (int i = 0; i < 4; i++) lv_obj_add_flag(s_sig_bars[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            badge_set_text(&s_bg_wifi, "WIFI", s_w_wifi);
            badge_show(&s_bg_num, false);     /* the count only means anything in AP */
            for (int i = 0; i < 4; i++) lv_obj_remove_flag(s_sig_bars[i], LV_OBJ_FLAG_HIDDEN);
        }
        p_ap_mode = (int)s->wifi_ap;
        p_clients = -1;                       /* to be redrawn in the new mode */
        relayout  = true;
    }

    /* --- SoftAP client count: a green badge holding only the number, absent
     * when there is nobody. Fixed green, set at construction: it has no states,
     * it is either there or not. Changing digit changes the width too, so it
     * must be remeasured and the group relaid out. */
    if (s->wifi_ap && s->wifi_clients != p_clients) {
        bool show = (s->wifi_clients > 0);
        if (show) {
            /* The SoftAP is configured for 4 clients (AP_MAX_CONN), so one
             * digit would do. But GCC reasons about the TYPE, not the domain:
             * to it an int prints up to 11 characters, and with
             * -Werror=format-truncation a narrow buffer is a compile error. So
             * the value is clamped — which also keeps the badge to two digits,
             * the width it is designed for — and the buffer is kept roomy
             * anyway, so none of this depends on how well the optimiser tracks
             * the range. */
            int n = s->wifi_clients;
            if (n < 0)  n = 0;
            if (n > 99) n = 99;
            char buf[STRIP_NUM_MAX];
            snprintf(buf, sizeof buf, "%d", n);
            badge_set_text(&s_bg_num, buf, strip_text_w(buf) + 2 * STRIP_PAD_X);
        }
        badge_show(&s_bg_num, show);
        p_clients = s->wifi_clients;
        relayout  = true;
    }

    /* --- AUTH: appears left of the WiFi badge when the key is refused. Fixed
     * colour, red, set at construction: it goes on and off and that is all, it
     * has no states to update. */
    if (s->wifi_auth != s_bg_auth.shown) {
        badge_show(&s_bg_auth, s->wifi_auth);
        relayout = true;
    }
    if (relayout) strip_reflow_right(s->wifi_ap);

    static int p_wifi = -1;
    badge_update(&s_bg_wifi, (int)s->wifi, wifi_color(s->wifi), &p_wifi);

    if (!s->wifi_ap) {
        /* Signal bars from the RSSI, at most once a second. The function
         * guards itself against rewriting when the NUMBER of bars does not
         * change, which is the ordinary case. */
        static int64_t last_rssi_us = 0;
        int64_t now_us = esp_timer_get_time();
        if (now_us - last_rssi_us >= 1000000) {
            signal_bars_set_rssi(s->wifi_rssi);
            last_rssi_us = now_us;
        }
    }
}

/* Saves the state of the two bridges, if the memory is on.
 *
 * OUTSIDE the critical section: an NVS write can block for milliseconds
 * (erasing a page), and holding the lock for that long would stop the
 * beat engine that shares it.
 *
 * With the memory off nothing is written: the saved values stay those of
 * the last time it was on, and turning it back on resumes from there. */
static void save_bridge_state(void)
{
    if (!settings_get_remember_bridge()) return;
    portENTER_CRITICAL(&s_mux);
    clock_src_t src = s_clock_src;
    portEXIT_CRITICAL(&s_mux);
    settings_set_bridge_state(src == CLOCK_SRC_DJLINK, src == CLOCK_SRC_ABLETON);
}

/* Touch on the offset field: move to the next active destination. It changes no
 * value — only what is being looked at — so even a spurious contact would be
 * harmless. With a single destination it does nothing. */
static void on_offset_tap(lv_event_t *e)
{
    (void)e;
    if (off_dest_count() < 2) return;      /* nothing to cycle through */
    portENTER_CRITICAL(&s_mux);
    off_focus_next();
    portEXIT_CRITICAL(&s_mux);
    audio_play(AUDIO_NAV_BEEP);
}

/* Any touch at all, while the screen is locked. The padlock's container covers
 * the whole screen and is on top, so this is the ONE handler that runs — every
 * tap gets the same answer wherever it lands.
 *
 * THE PADLOCK FLASHES, not the control under the finger. What was refused is
 * not that button in particular: it is the whole surface, and the padlock is
 * what says so. */
static void on_locked_tap(lv_event_t *e)
{
    (void)e;
    portENTER_CRITICAL(&s_mux);
    s_lock_flash = esp_timer_get_time();
    portEXIT_CRITICAL(&s_mux);
    audio_play(AUDIO_REJECT);
}


/* Touch on a toggle: it selects that source, or clears it if it was already
 * selected. The two toggles share this one handler because the action is the
 * same — only which source they name differs — and the mutual exclusion comes
 * for free: assigning one value cannot leave two of them set. */
static void on_toggle(clock_src_t which)
{
    if (which == CLOCK_SRC_ABLETON && !ui_ableton_src_available()) {
        portENTER_CRITICAL(&s_mux);
        s_reject_flash = esp_timer_get_time();
        portEXIT_CRITICAL(&s_mux);
        audio_play(AUDIO_REJECT);           /* the touch is refused: a low beep */
        return;
    }
    bool now_on = false;
    portENTER_CRITICAL(&s_mux);
    s_clock_src = (s_clock_src == which) ? CLOCK_SRC_NONE : which;
    now_on      = (s_clock_src == which);
    if (which == CLOCK_SRC_ABLETON)
        s_log_touch_abl = true;          /* printed outside the critical section */
    portEXIT_CRITICAL(&s_mux);

    audio_play(now_on ? AUDIO_BRIDGE_ON : AUDIO_BRIDGE_OFF);
    save_bridge_state();
    if (s_log_touch_abl) {
        s_log_touch_abl = false;
        ESP_LOGI(TAG, "[toggle] ableton source %s from TOUCH", now_on ? "ON" : "off");
    }
}

/* LVGL wants callbacks with a fixed signature, so each toggle gets a one-line
 * wrapper naming its own source. */
static void on_toggle_dj(lv_event_t *e)      { (void)e; on_toggle(CLOCK_SRC_DJLINK); }
static void on_toggle_ableton(lv_event_t *e) { (void)e; on_toggle(CLOCK_SRC_ABLETON); }

/* The bridge can clear the source by itself — for instance after yielding the
 * master role to a player. Asking for Ableton when it is unavailable leaves
 * NONE rather than silently succeeding. */
void ui_set_clock_src(clock_src_t src)
{
    clock_src_t was;
    if (src == CLOCK_SRC_ABLETON && !ui_ableton_src_available()) src = CLOCK_SRC_NONE;
    portENTER_CRITICAL(&s_mux);
    was = s_clock_src;
    s_clock_src = src;
    portEXIT_CRITICAL(&s_mux);
    /* Outside the critical section: ESP_LOG can block. */
    if (was != src)
        ESP_LOGW(TAG, "[clock] source %d -> %d from CODE (ui_set_clock_src)",
                 (int)was, (int)src);
}

/* The reverse bridge is only possible with a real identity, a device number in
 * 1..4. With four players on the network none is left, and the toggle goes
 * inert. */
void ui_set_ableton_src_available(bool avail)
{
    bool switched_off = false;
    portENTER_CRITICAL(&s_mux);
    s_ableton_src_available = avail;
    if (!avail && s_clock_src == CLOCK_SRC_ABLETON) {
        s_clock_src = CLOCK_SRC_NONE; switched_off = true;
    }
    portEXIT_CRITICAL(&s_mux);
    if (switched_off)
        ESP_LOGW(TAG, "[clock] ableton source cleared: NO LONGER AVAILABLE "
                      "(identity lost)");
}

bool ui_ableton_src_available(void)
{
    portENTER_CRITICAL(&s_mux);
    bool a = s_ableton_src_available;
    portEXIT_CRITICAL(&s_mux);
    return a;
}

/* Restore the saved source. main calls it at start-up when the bridge memory is
 * enabled.
 *
 * If Ableton is not AVAILABLE as a source — four players on the network and no
 * free 1..4 identity — it is not restored even if it was saved. Better to start
 * with no source than to show a bridge that is carrying nothing. */
void ui_set_clock_src_initial(clock_src_t src)
{
    if (src == CLOCK_SRC_ABLETON && !ui_ableton_src_available()) src = CLOCK_SRC_NONE;
    portENTER_CRITICAL(&s_mux);
    s_clock_src = src;
    portEXIT_CRITICAL(&s_mux);
}

void ui_set_track(const char *title, const char *artist)
{
    portENTER_CRITICAL(&s_mux);
    if (title  && *title)  snprintf(s_trk_title,  sizeof(s_trk_title),  "%s", title);
    else                   s_trk_title[0]  = 0;
    if (artist && *artist) snprintf(s_trk_artist, sizeof(s_trk_artist), "%s", artist);
    else                   s_trk_artist[0] = 0;
    s_trk_valid = (s_trk_title[0] != 0) || (s_trk_artist[0] != 0);
    s_trk_dirty = true;      /* the UI task does the MEASURING: LVGL is not thread-safe */
    portEXIT_CRITICAL(&s_mux);
}

void ui_clear_track(void)
{
    portENTER_CRITICAL(&s_mux);
    s_trk_title[0] = s_trk_artist[0] = 0;
    s_trk_valid = false;
    portEXIT_CRITICAL(&s_mux);
}

/* formats a float with the STANDARD snprintf (LVGL does not handle %f) */
static void set_fmt(lv_obj_t *lbl, const char *fmt, ...)
{
    char buf[24];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    lv_label_set_text(lbl, buf);
}

/* --- silkscreened buttons (strip below y~250): they adjust the phase offset ---
 * Runs on the LVGL task, so there is no race on I2C with the pointer. Period
 * 1 ms, a sustained press repeats quickly, and the NVS save is debounced. */
static void buttons_timer(lv_timer_t *t)
{
    (void)t;
    uint16_t x, y;
    bool touched = bsp_touch_read(&x, &y);
    int64_t now = esp_timer_get_time();


#if DJB_DEVELOPER
    /* A SCREENSHOT CAN BE TAKEN FROM ANY SCREEN, so the long press on the
     * circle is recognised HERE, before the touch is handed to whoever owns the
     * screen. Left further down it only ever fired on the dashboard: with
     * settings open the branch below returns long before reaching it, and the
     * settings pages — the ones most worth photographing for a manual — could
     * not be captured.
     *
     * With the option off this does nothing at all; with it on, the gesture
     * belongs to the screenshot on every screen alike. */
    static int64_t shot_start = 0;
    static bool    shot_fired = false;

    /* THE RELEASE HAS TO BE EATEN TOO, and this comes first because on release
     * the coordinates mean nothing.
     *
     * Consuming only while the finger is DOWN was not enough: the lift went
     * through, and whoever owns the screen read it as a short tap — Settings
     * opening from the dashboard, or closing when already inside. The gesture
     * ends when the finger leaves, and everything it produces belongs to the
     * screenshot. */

    if (shot_fired) {
        if (!touched) {
            shot_fired = false;
            shot_start = 0;
            /* AND THE BUTTON STATE GOES WITH IT. The press reached the
             * dashboard's own tracking before the shot fired, leaving s_btn
             * at 2; left there, the next long press would find a stale
             * start time and fire at once. */
            s_btn = 0;
            s_center_fired = false;
            /* AND THE SETTINGS FORGET THEIRS. They remember a press until a
             * release acts on it: with that release eaten here, the memory
             * would sit waiting, and the NEXT release — the end of some other
             * gesture entirely — would be read as the end of this one. That is
             * what sent Settings back a level after every screenshot. */
            if (settings_ui_active()) settings_ui_touch_cancel();
        }
        return;
    }

    /* NOT WHILE THE ABOUT SCREEN IS PLAYING. It reads the touch itself, on its
     * own task, because it draws outside LVGL — so a press there is heard
     * TWICE: once by About, which takes it as "leave", and once here, which
     * after 1.5 s takes it as a screenshot. Two beeps, one on top of the
     * other, and a picture nobody asked for.
     *
     * About is an animation rather than a screen worth photographing, so the
     * touch is left to it. */
    if (s_shots_on && !settings_ui_about_running() &&
        touched && y >= 248 && x >= 125 && x <= 195) {
        if (!shot_start) shot_start = now;
        else if (now - shot_start > 1500000) {
            shot_fired = true;
            audio_play(AUDIO_NAV_BEEP);
            if (s_shot_fn) s_shot_fn();
            return;
        }
    } else {
        shot_start = 0;
    }
#endif


    /* --- With SETTINGS open, the whole touch goes to them --- */
    if (settings_ui_active()) {
        settings_ui_touch(x, y, touched);
        if (!settings_ui_active()) {
            /* settings have just closed (back from the last level):
             * return to the dashboard */
            if (s_dash_scr) lv_screen_load(s_dash_scr);
        }
        s_btn = 0;
        return;
    }

    /* --- DASHBOARD: input from the physical strip below, y >= 248 --- */
    int b = 0;                                  /* -1 left, +1 right, 2 centre, 0 none */
    if (touched && y >= 248) {
        /* --- ZONES, WITH DEAD BANDS BETWEEN THEM ------------------------
         *
         * [M] The silkscreened circle sits at 160, measured. With contiguous
         * zones one pixel changed the action, and here a mistake is expensive:
         * aiming at the circle to open Settings, one landed on an arrow and
         * moved the PHASE OFFSET — that is, put the clock out of phase in the
         * middle of a set.
         *
         *      0 ...... 100 | 125 ... 195 | 220 ...... 319
         *      left arrow   ^   circle    ^   right arrow
         *                  dead          dead
         *
         * The centre stays 70 px wide because it carries two functions — it
         * opens Settings, and a long press locks the interface — so it deserves
         * a generous target. The dead bands are 25 px on each side. */
        if (x < 100)      b = -1;               /* left arrow */
        else if (x > 220) b = +1;               /* right arrow */
        else if (x >= 125 && x <= 195) b = 2;   /* centre circle */
        /* else: dead band, b stays 0 and nothing happens */

        /* With the screen ROTATED the two arrows swap.
         *
         * It is not a remapping of coordinates — the strip is printed on the
         * glass and does not rotate — but of MEANING: the arrow physically on
         * the left, seen on a turned device, is on the user's right, and its
         * upside-down glyph looks exactly like a right arrow.
         *
         * The circle (b = 2) is untouched: it is symmetric. */
        if (display_flipped() && (b == -1 || b == 1)) b = -b;
    }

    if (b == 2) {
        /* Centre button:
         *  - long press >= 1.5 s -> toggle the lock, once per press
         *  - short tap, released before 1.5 s -> enter Settings */
        if (s_btn != 2) {
            s_btn = 2; s_btn_start = now; s_center_fired = false;
        } else if (!s_center_fired && (now - s_btn_start) > 1500000) {
            s_center_fired = true;

            bool locked_now;
            portENTER_CRITICAL(&s_mux);
            s_ui_locked = !s_ui_locked;
            locked_now = s_ui_locked;
            portEXIT_CRITICAL(&s_mux);
            audio_play(locked_now ? AUDIO_LOCK_ON : AUDIO_LOCK_OFF);
        }
    } else if ((b == -1 || b == +1) && !s_ui_locked &&
               (s_clock_src != CLOCK_SRC_NONE)) {
        /* Arrows: they adjust the offset, but only if the interface is not
         * locked AND a bridge is active. With the bridge off the field is not
         * even on screen, so pressing them would change a value with no visible
         * response — and a value with no direction to apply it to. */
        if (b != s_btn) {
            offset_bump(b);
            s_btn = b; s_btn_start = now; s_btn_rep = now;
        } else if (now - s_btn_start > 400000 && now - s_btn_rep > 90000) {
            offset_bump(b);                     /* fast repeat */
            s_btn_rep = now;
        }
    } else {
        /* Release. If the CENTRE was being pressed and the long press did NOT
         * fire, it was a short tap -> enter Settings. Not from a locked
         * interface, though: there the centre only serves to unlock with a long
         * press. */
        if (s_btn == 2 && !s_center_fired && !s_ui_locked) {
            settings_ui_enter();
        }
        s_btn = 0;
    }

    /* persistent save, debounced (1.5 s after the last change) */
    if (s_offset_dirty && (now - s_offset_change_us) > 1500000) {
        settings_set_phase_offset_dj_us(ui_get_phase_offset_dj_us());
        settings_set_phase_offset_abl_us(ui_get_phase_offset_abl_us());
        settings_set_phase_offset_osc_us(false, s_offset_osc_us[0]);
        settings_set_phase_offset_osc_us(true,  s_offset_osc_us[1]);
        settings_set_phase_offset_midi_us(false, s_offset_midi_us[0]);
        settings_set_phase_offset_midi_us(true,  s_offset_midi_us[1]);
        settings_set_phase_offset_cv_us(false, s_offset_cv_us[0]);
        settings_set_phase_offset_cv_us(true,  s_offset_cv_us[1]);
        s_offset_dirty = false;
    }
}

/* refresh at 10 Hz: updates objects ONLY when the values change */
/* formats a BPM without %f (which is heavy on the stack): integer arithmetic */
static void set_bpm(lv_obj_t *lbl, float v)
{
    int t = (int)(v * 10.0f + 0.5f);
    if (t < 0) t = 0;
    set_fmt(lbl, "%d.%d", t / 10, t % 10);
}

/* --- INITIAL SILENCE -------------------------------------------------------
 *
 * For the first second after start-up the dashboard does NOT update.
 *
 * WHY: the badges reflect detections that have not yet been made at the first
 * instant — the W5500 has not answered, the players have not announced
 * themselves, the DIN module has not been read, the USB host has not
 * enumerated. Drawing them at once shows an INCOHERENT state for a fraction of
 * a second, which then changes before the user's eyes.
 *
 * It is not an arbitrary wait: it is the time the first readings need. One
 * second covers the DJ Link census (keep-alives every 1.5 s, though players
 * announce themselves at once), the first query of the W5500, and the first
 * pass of the slow loop.
 *
 * Meanwhile the strip stays as ui_init() drew it, that is at rest — which is
 * honest: we do not know anything yet.
 *
 * THE DELAY APPLIES ONLY TO THE BADGES. The rest of the dashboard — bridge
 * buttons, BPM, offset, padlock — reflects state we know from the first
 * instant, and is drawn immediately. Holding everything back, as the first
 * version did, left the buttons looking as created for a whole second.
 *
 * THE REFERENCE INSTANT is the end of ui_init(), not the boot of the chip.
 * lv_tick_get() would have been ambiguous: with esp_lvgl_port it counts from
 * power-on, and if ui_init() ran a second later the delay would have no effect
 * at all. Anchored to when the dashboard EXISTS, the second of silence is the
 * one the user actually sees. */
#define UI_SETTLE_US   (1000 * 1000)

static int64_t s_ui_ready_us = 0;

static void refresh_timer(lv_timer_t *t)
{
    (void)t;


    /* If settings are on screen we do NOT update the dashboard: its objects
     * live on a background screen. Modifying them would invalidate them,
     * triggering redraws and flushes on a screen nobody sees — a source of
     * stalls with our custom flush_cb and its quiet window. The dashboard
     * updates again as soon as we return. */
    if (settings_ui_active()) return;

    ui_state_t s; bool on;
    portENTER_CRITICAL(&s_mux);
    s = s_state;
    bool dj  = (s_clock_src == CLOCK_SRC_DJLINK);   /* left toggle:  DJ Link gives the tempo */
    bool abl = (s_clock_src == CLOCK_SRC_ABLETON); /* right toggle: Ableton gives it        */
    on = dj || abl;                /* a bridge active in EITHER direction */
    /* The value shown is that of the focused destination. For the bridge it
     * depends on the direction; the auxiliary outputs each have their own. */
    int off = (s_off_focus == OFF_DEST_OSC)  ? s_offset_osc_us[osc_src_idx()]
            : (s_off_focus == OFF_DEST_MIDI) ? s_offset_midi_us[osc_src_idx()]
            : (s_off_focus == OFF_DEST_CV)   ? s_offset_cv_us[osc_src_idx()]
                                             : (abl ? s_offset_abl_us : s_offset_dj_us);
    bool trk_ok    = s_trk_valid;
    bool trk_dirty = s_trk_dirty; s_trk_dirty = false;
    bool playing   = s.track_playing;
    char trk_title[TRACK_TXT_MAX], trk_artist[TRACK_TXT_MAX];
    memcpy(trk_title,  s_trk_title,  sizeof(trk_title));
    memcpy(trk_artist, s_trk_artist, sizeof(trk_artist));
    portEXIT_CRITICAL(&s_mux);

    /* MEASURE ONCE per track, not at 20 Hz.
     *
     * If title and artist fit together on the line they are shown concatenated
     * and the alternation is unnecessary: with the size 12 font there is often
     * room to spare.
     *
     * The measurement uses the label object itself, whose width follows its
     * content, so it is LVGL that does the arithmetic over font and UTF-8 —
     * track names are full of accents and apostrophes, and an estimate "by
     * characters" would get it wrong. The cost, one layout recalculation, is
     * paid at every TRACK CHANGE, that is every few minutes: nothing. */
    if (trk_dirty) {
        s_trk_fits = false;
        s_trk_both[0] = 0;
        if (trk_title[0] && trk_artist[0]) {
            snprintf(s_trk_both, sizeof(s_trk_both), "%s - %s", trk_title, trk_artist);
            lv_label_set_long_mode(s_track, LV_LABEL_LONG_CLIP);
            lv_obj_set_width(s_track, LV_SIZE_CONTENT);
            lv_label_set_text(s_track, s_trk_both);
            lv_obj_update_layout(s_track);
            s_trk_fits = (lv_obj_get_width(s_track) <= TRACK_W);
            lv_obj_set_width(s_track, TRACK_W);          /* restore */
            lv_label_set_long_mode(s_track, LV_LABEL_LONG_DOT);
            /* The measurement has DIRTIED the label with the concatenated
             * text. The change-detection memory is invalidated so the block
             * below rewrites it with the right one — which may well be "", if
             * playback is stopped. */
            s_p_txt[0] = 1; s_p_txt[1] = 0;
        }
    }

    static int p_on = -1, p_beat = -1, p_master = -1, p_peers = -1,
               /* the BPMs shown, kept x10 so that a tenth of a difference
                * is a change and rounding noise is not */
               p_bpm_big = -999, p_bpm_dj = -999, p_bpm_link = -999;

    if ((int)on != p_on) {
        if (on) {
            lv_obj_remove_flag(s_bpm_big, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(s_bpm_lbl, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_bpm_dj, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_bpm_link, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_bpm_dj_l, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_bpm_link_l, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_bpm_big, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_bpm_lbl, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(s_bpm_dj, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(s_bpm_link, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(s_bpm_dj_l, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(s_bpm_link_l, LV_OBJ_FLAG_HIDDEN);
        }
        p_on = on;
        p_bpm_big = p_bpm_dj = p_bpm_link = -999;   /* force a BPM refresh after the switch */
    }

    /* Left toggle: lit only when DJ Link is the source. */
    static int p_tog_dj = -1;
    if ((int)dj != p_tog_dj) {
        lv_obj_set_style_bg_color(s_tl, dj ? C_CYANBG : C_PANEL, 0);
        lv_obj_set_style_border_color(s_tl, dj ? C_CYAN : C_DIM, 0);
        lv_obj_set_style_text_color(s_tl_arrow, dj ? C_CYAN : C_DIMTXT, 0);
        p_tog_dj = dj;
    }

    /* Right toggle, the reverse bridge. Three states:
     *   on            -> cyan
     *   off           -> normal grey, clickable
     *   UNAVAILABLE   -> dull grey: without a real 1..4 identity we cannot
     *                    become master, so the reverse bridge is impossible.
     *                    The button stays there but is inert. */
    bool abl_ok  = s_ableton_src_available;
    bool rejected = (esp_timer_get_time() - s_reject_flash) < REJECT_FLASH_US;
    static int p_tog_abl = -1, p_ablok = -1, p_rej = -1;
    if ((int)abl != p_tog_abl || (int)abl_ok != p_ablok || (int)rejected != p_rej) {
        if (rejected) {                     /* red flash: "not allowed" */
            lv_obj_set_style_bg_color(s_tr, C_PANEL, 0);
            lv_obj_set_style_border_color(s_tr, C_RED, 0);
            lv_obj_set_style_border_opa(s_tr, LV_OPA_COVER, 0);
            lv_obj_set_style_text_color(s_tr_arrow, C_RED, 0);
            lv_obj_set_style_text_opa(s_tr_arrow, LV_OPA_COVER, 0);
        } else if (!abl_ok) {
            lv_obj_set_style_bg_color(s_tr, C_PANEL, 0);
            lv_obj_set_style_border_color(s_tr, C_DIM, 0);
            lv_obj_set_style_border_opa(s_tr, LV_OPA_30, 0);
            lv_obj_set_style_text_color(s_tr_arrow, C_DIM, 0);
            lv_obj_set_style_text_opa(s_tr_arrow, LV_OPA_40, 0);
        } else {
            lv_obj_set_style_border_opa(s_tr, LV_OPA_COVER, 0);
            lv_obj_set_style_text_opa(s_tr_arrow, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(s_tr, abl ? C_CYANBG : C_PANEL, 0);
            lv_obj_set_style_border_color(s_tr, abl ? C_CYAN : C_DIM, 0);
            lv_obj_set_style_text_color(s_tr_arrow, abl ? C_CYAN : C_DIMTXT, 0);
        }
        p_tog_abl = abl; p_ablok = abl_ok; p_rej = rejected;
    }

    if (on) {
        int v = (int)lroundf(s.link_bpm * 10.0f);
        if (v != p_bpm_big) { set_bpm(s_bpm_big, s.link_bpm); p_bpm_big = v; }
    } else {
        int vd = (int)lroundf(s.dj_bpm * 10.0f);
        int vl = (int)lroundf(s.link_bpm * 10.0f);
        if (vd != p_bpm_dj)   { set_bpm(s_bpm_dj, s.dj_bpm);   p_bpm_dj = vd; }
        if (vl != p_bpm_link) { set_bpm(s_bpm_link, s.link_bpm); p_bpm_link = vl; }
    }

    if (s.master_dev != p_master) {
        if (s.master_dev) set_fmt(s_master, "%d", s.master_dev);
        else              lv_label_set_text(s_master, "");
        p_master = s.master_dev;
    }

    /* MASTER badge, three states:
     *   - negotiation under way -> BLINKING AMBER, the role is not ours yet
     *   - master acquired       -> steady GREEN
     *   - negotiation failed    -> blinking RED, after which the bridge stops
     * Otherwise steady amber (master = a Pioneer player). */
    static int p_mstate = -1;
    int mstate = s.master_failed ? 3 : (s.we_are_master ? 2 : (s.master_negotiating ? 1 : 0));
    bool blink_on = ((lv_tick_get() / 250) % 2) == 0;      /* ~2Hz */
    lv_color_t mc;
    if      (mstate == 2) mc = C_GREEN;
    else if (mstate == 3) mc = blink_on ? C_RED   : C_DIM;
    else if (mstate == 1) mc = blink_on ? C_AMBER : C_DIM;
    else                  mc = C_AMBER;

    /* Change detection on (state, blink phase): those are the two values that
     * determine the colour. In LVGL 9 lv_color_t no longer has a .full field,
     * so colours are not compared directly. */
    static int p_blink = -1;
    int blink_phase = (mstate == 1 || mstate == 3) ? (int)blink_on : 0;
    if (mstate != p_mstate || blink_phase != p_blink) {
        lv_obj_set_style_border_color(s_mbadge, mc, 0);
        lv_obj_set_style_text_color(s_mtxt, mc, 0);
        lv_obj_set_style_bg_color(s_msq, mc, 0);
        lv_obj_set_style_border_color(s_msq, mc, 0);
        p_mstate = mstate;
        p_blink  = blink_phase;
    }

    if (s.beat != p_beat) {
        for (int i = 0; i < 4; i++) {
            bool lit = (s.beat == i + 1);
            lv_obj_set_style_bg_color(s_beat[i], lit ? C_AMBER : C_PANEL, 0);
            lv_obj_set_style_border_width(s_beat[i], lit ? 0 : 1, 0);
        }
        p_beat = s.beat;
    }

    if (s.peers != p_peers) {
        set_fmt(s_peer, "%d peer%s", s.peers, s.peers == 1 ? "" : "s");
        p_peers = s.peers;
    }

    /* INITIAL SILENCE, but ONLY on the badges: see the note on UI_SETTLE_US.
     *
     * The problem the delay exists to solve concerns only the status strip —
     * LAN, MIDI, OSC, CV, WIFI — whose values depend on detections not yet
     * made. Everything else on the dashboard reflects state known from the
     * first instant, and must be drawn at once. */
    if (s_ui_ready_us &&
        esp_timer_get_time() - s_ui_ready_us >= UI_SETTLE_US)
        refresh_strip(&s);

    /* Padlock: shown only when the interface is locked; there is no open symbol. */
    static int p_locked = -1;
    /* Last colour WRITTEN to the glyph: 0 grey, 1 red. Declared here because
     * both the unlock below and the flash edge further down set it. */
    static int p_lockrej = 0;

    if ((int)s_ui_locked != p_locked) {
        if (s_ui_locked) {
            /* TO THE FRONT, and not just visible. In LVGL the stacking order
             * is the order of CREATION, and the padlock's container is built
             * before the two bridge buttons and the offset field — so left
             * where it was born it sits UNDER them, and their touches reach
             * them as if nothing were locked. Raising it here rather than
             * reordering the construction keeps it correct even if something
             * new is added to the screen later. */
            lv_obj_move_foreground(s_lock);
            lv_obj_clear_flag(s_lock, LV_OBJ_FLAG_HIDDEN);
        }
        else {
            lv_obj_add_flag(s_lock, LV_OBJ_FLAG_HIDDEN);
            /* UNLOCKING WHILE THE FLASH IS ON would hide a red glyph and show
             * it red again at the next lock, until the edge below caught up.
             * It is put back to grey here, while it is invisible and the
             * repaint costs nothing. */
            s_lock_flash = 0;
            lock_glyph_color(C_LABEL);
            p_lockrej    = 0;
        }
        p_locked = s_ui_locked;
    }
    /* THE PADLOCK GOES RED for a moment when a touch is refused. It is the
     * padlock and not the control under the finger, because what was refused is
     * the whole surface — and a single control lighting up would say the wrong
     * thing about which.
     *
     * REDRAWN ONLY ON THE EDGE, twice per flash: once to go red and once to
     * come back. The colour is compared against what was last WRITTEN, so a
     * screen sitting locked and untouched costs one comparison per refresh and
     * no drawing at all — which matters, because drawing goes over the same SPI
     * bus as the beats.
     *
     * p_lockrej STARTS AT 0, not -1, because the glyph is built grey: starting
     * from "unknown" would repaint it grey the first time round, which is a
     * flush of the whole corner to change nothing. */
    const bool lockrej = (esp_timer_get_time() - s_lock_flash) < REJECT_FLASH_US;
    if (s_ui_locked && (int)lockrej != p_lockrej) {
        lock_glyph_color(lockrej ? C_RED : C_LABEL);
        p_lockrej = lockrej;
    }

    /* PHASE OFFSET.
     *
     * It is only meaningful with a bridge active: the offset DIFFERS between
     * the two directions, and with the bridge off there is no direction to
     * adjust. Showing one at random — it used to always show the DJ->Ableton
     * one — is misleading. So with the bridge off no number is shown. */
    off_focus_validate();          /* the focus always points at something active */
    int ndest = off_dest_count();

    static int p_off = -999999;
    static int p_offon = -1, p_ndest = -1, p_focus = -1;
    if (off != p_off || (int)on != p_offon ||
        ndest != p_ndest || (int)s_off_focus != p_focus) {
        if (ndest == 0) {
            /* No destination: the whole group is hidden. Writing "--" under a
             * PHASE OFFSET caption left a field on screen that means nothing. */
            lv_obj_add_flag(s_offset_dest, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_offset_lbl, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_offset_val, LV_OBJ_FLAG_HIDDEN);
            for (int d = 0; d < OFF_DEST_COUNT; d++)
                lv_obj_add_flag(s_offset_dot[d], LV_OBJ_FLAG_HIDDEN);
        } else {
            int a = off < 0 ? -off : off;
            set_fmt(s_offset_val, "%s%d.%d ms", off < 0 ? "-" : "+",
                    a / 1000, (a % 1000) / 100);   /* fraction is 0 or 5 */

            /* Three lines with several destinations, two with one. In the
             * second case the two lines align with those on the left —
             * "ABLETON LINK" at 158 and the peer count at 174 — so the band
             * reads as one row rather than two staggered blocks. */
            bool multi = (ndest >= 2);
            lv_obj_set_pos(s_offset_lbl, 196, multi ? 171 : 158);
            lv_obj_set_pos(s_offset_val, 196, multi ? 185 : 174);

            if (multi) {
                set_fmt(s_offset_dest, "%s", off_dest_name(s_off_focus));
                lv_obj_remove_flag(s_offset_dest, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(s_offset_dest, LV_OBJ_FLAG_HIDDEN);
            }
            lv_obj_remove_flag(s_offset_lbl, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(s_offset_val, LV_OBJ_FLAG_HIDDEN);

            /* One dot per active destination, lit on the one being shown.
             * With a single one they do not appear: there is no cycle to announce. */
            int slot = 0;
            for (int d = 0; d < OFF_DEST_COUNT; d++) {
                if (!multi || !off_dest_active((off_dest_t)d)) continue;
                bool cur = ((off_dest_t)d == s_off_focus);
                int  dia = cur ? OFF_DOT_D_ON : OFF_DOT_D_OFF;  /* focus also reads from the size */
                lv_obj_set_size(s_offset_dot[slot], dia, dia);
                lv_obj_set_pos(s_offset_dot[slot], OFF_DOT_X(dia), OFF_DOT_Y(slot, dia));
                lv_obj_set_style_bg_color(s_offset_dot[slot],
                        cur ? C_AMBER : C_MUTED2, 0);
                lv_obj_remove_flag(s_offset_dot[slot], LV_OBJ_FLAG_HIDDEN);
                slot++;
            }
            for (int d = slot; d < OFF_DEST_COUNT; d++)
                lv_obj_add_flag(s_offset_dot[d], LV_OBJ_FLAG_HIDDEN);
        }
        p_off = off; p_offon = on; p_ndest = ndest; p_focus = s_off_focus;
    }
    /* Offset colour: this was the ONLY unguarded LVGL write in the refresh, and
     * it was expensive. In LVGL every lv_obj_set_style_*() INVALIDATES the
     * object EVEN IF THE VALUE IS IDENTICAL — there is no internal check. So
     * this line, called 20 times a second, marked the label "to be redrawn"
     * twenty times a second: with the screen completely still, we were flushing
     * continuously and taking the SPI bus for nothing. Now it is written only
     * when the colour actually changes. */
    bool recent = on && (esp_timer_get_time() - s_offset_touched_us) < 800000;
    int  offcol = !on ? 0 : (recent ? 1 : 2);
    static int p_offcol = -1;
    if (offcol != p_offcol) {
        lv_obj_set_style_text_color(s_offset_val,
                                    offcol == 0 ? C_DIM
                                  : offcol == 1 ? C_WHITE : C_LABEL, 0);
        p_offcol = offcol;
    }

    /* TRACK.
     *  - with playback stopped: HIDDEN, but the data stays in memory, so if the
     *    same piece resumes there is no fresh query to the database
     *  - if title and artist fit together: concatenated, no alternation
     *  - otherwise: they alternate every TRACK_ALTERNATE_MS
     *  - if only one is known: always that one, with no pointless flashing
     * The title is lighter than the artist: it tells them apart without labels
     * that would steal space.
     * The label is rewritten ONLY when it really changes: the refresh runs at
     * 20 Hz and every set_text dirties an area of the display, which then has
     * to be redrawn over the SPI shared with the W5500. */
    const char *txt = "";
    bool is_title = true;
    if (trk_ok && playing) {
        bool ha_t = trk_title[0]  != 0;
        bool ha_a = trk_artist[0] != 0;
        if (ha_t && ha_a && s_trk_fits) {
            txt = s_trk_both;            /* both fit: no alternation */
            is_title = true;
        } else if (ha_t && ha_a) {
            is_title = ((lv_tick_get() / TRACK_ALTERNATE_MS) % 2) == 0;
            txt = is_title ? trk_title : trk_artist;
        } else {
            is_title = ha_t;
            txt = is_title ? trk_title : trk_artist;
        }
    }
    if (strncmp(txt, s_p_txt, sizeof(s_p_txt)) != 0 || (int)is_title != s_p_istitle) {
        lv_label_set_text(s_track, txt);
        lv_obj_set_style_text_color(s_track, is_title ? C_LABEL : C_MUTED, 0);
        snprintf(s_p_txt, sizeof(s_p_txt), "%s", txt);
        s_p_istitle = is_title;
    }
}

static void build_dashboard(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, C_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* TRACK on the master: title and artist, alternating every 3 s.
     * Width 280: the padlock begins at x=292, so we stop short of it with a
     * margin. Truncation with dots (LONG_DOT) rather than scrolling: a
     * continuous scroll would cost cycles on a core already contended by the
     * SPI bus, and the flourish would not be worth it. */
    s_track = label(scr, 8, 4, "", &lv_font_montserrat_12, C_MUTED);
    lv_obj_set_width(s_track, TRACK_W);
    lv_label_set_long_mode(s_track, LV_LABEL_LONG_DOT);
    /* Padlock: a white vector glyph, shown ONLY when the interface is locked.
     * It lives in a container so it can be shown and hidden as one block.
     *
     * THE CONTAINER IS THE WHOLE SCREEN, and that is what makes the lock work:
     * shown, it sits above everything and takes every touch, so a locked screen
     * answers the same way wherever the finger lands. Leaving each control to
     * check the lock for itself is how the answer came to depend on WHERE one
     * tapped — a beep here, a red flash there, silence over the metronome. */
    s_lock = lv_obj_create(scr);
    lv_obj_remove_style_all(s_lock);
    lv_obj_set_pos(s_lock, 0, 0);
    lv_obj_set_size(s_lock, 320, 240);
    lv_obj_clear_flag(s_lock, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_lock, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_lock, on_locked_tap, LV_EVENT_CLICKED, NULL);
    build_lock_glyph(s_lock, 292, 6, C_LABEL);   /* 24x24, top-right corner */
    lv_obj_add_flag(s_lock, LV_OBJ_FLAG_HIDDEN); /* hidden until it locks */

    label(scr, 8, 22, "DJ LINK", &lv_font_montserrat_12, C_AMBER);
    s_mbadge = rect(scr, 8, 40, 92, 26, C_BG, LV_OPA_TRANSP, C_AMBER, 1, 4);
    lv_obj_t *badge = s_mbadge;
    s_mtxt = label(badge, 0, 0, "MASTER", &lv_font_montserrat_12, C_AMBER);
    lv_obj_t *mtxt = s_mtxt;
    lv_obj_align(mtxt, LV_ALIGN_LEFT_MID, 6, 0);
    s_msq = rect(badge, 0, 0, 18, 20, C_AMBER, LV_OPA_COVER, C_AMBER, 0, 2);
    lv_obj_t *msq = s_msq;
    lv_obj_align(msq, LV_ALIGN_RIGHT_MID, -4, 0);
    s_master = label(msq, 0, 0, "2", &lv_font_montserrat_16, lv_color_hex(0x17130A));
    lv_obj_center(s_master);

    int bx[4] = { 116, 166, 216, 266 };
    for (int i = 0; i < 4; i++)
        s_beat[i] = rect(scr, bx[i], 42, 44, 22, C_PANEL, LV_OPA_COVER, C_AMBERD, 1, 3);

    /* left toggle (interactive) */
    s_tl = rect(scr, 6, 86, 52, 50, C_CYANBG, LV_OPA_COVER, C_CYAN, 2, 8);
    lv_obj_add_flag(s_tl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_tl, on_toggle_dj, LV_EVENT_CLICKED, NULL);
    s_tl_arrow = label(s_tl, 0, 0, LV_SYMBOL_DOWN, &lv_font_montserrat_20, C_CYAN);
    lv_obj_center(s_tl_arrow);

    /* right toggle (interactive: reverse bridge Ableton->DJ) */
    s_tr = rect(scr, 262, 86, 52, 50, C_PANEL, LV_OPA_COVER, C_DIM, 2, 8);
    lv_obj_add_flag(s_tr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_tr, on_toggle_ableton, LV_EVENT_CLICKED, NULL);
    s_tr_arrow = label(s_tr, 0, 0, LV_SYMBOL_UP, &lv_font_montserrat_20, C_DIMTXT);
    lv_obj_center(s_tr_arrow);

    /* the shared giant BPM */
    s_bpm_big = label(scr, 0, 0, "128.0", &lv_font_montserrat_48, C_WHITE);
    lv_obj_align(s_bpm_big, LV_ALIGN_TOP_MID, 0, 80);
    s_bpm_lbl = label(scr, 0, 0, "BPM", &lv_font_montserrat_12, C_MUTED);
    lv_obj_align(s_bpm_lbl, LV_ALIGN_TOP_MID, 0, 132);

    /* the two grey BPMs (sync OFF), hidden by default */
    s_bpm_dj = label(scr, 0, 0, "128.0", &lv_font_montserrat_20, C_GREY);
    lv_obj_align(s_bpm_dj, LV_ALIGN_TOP_MID, -62, 92);
    s_bpm_link = label(scr, 0, 0, "124.5", &lv_font_montserrat_20, C_GREY);
    lv_obj_align(s_bpm_link, LV_ALIGN_TOP_MID, 62, 92);
    s_bpm_dj_l = label(scr, 0, 0, "DJ LINK", &lv_font_montserrat_12, C_AMBER);
    lv_obj_align(s_bpm_dj_l, LV_ALIGN_TOP_MID, -62, 120);
    s_bpm_link_l = label(scr, 0, 0, "ABLETON", &lv_font_montserrat_12, C_CYAN);
    lv_obj_align(s_bpm_link_l, LV_ALIGN_TOP_MID, 62, 120);
    /* INITIAL LAYOUT CONSISTENT WITH THE BRIDGE.
     *
     * The two groups are mutually exclusive: the giant BPM when a bridge is
     * active, the two separate BPMs when both are off. THOSE TWO ARE CREATED
     * HIDDEN: created visible like everything else, the large number would
     * appear for an instant at power-on and then be hidden by the first
     * refresh — a visible flicker.
     *
     * Here the right group is chosen at once by reading the real state of the
     * toggles, so the first image is already the final one. */
    {
        bool bridge_on = s_clock_src != CLOCK_SRC_NONE;
        lv_obj_t *big[]  = { s_bpm_big, s_bpm_lbl };
        lv_obj_t *pair[] = { s_bpm_dj, s_bpm_link, s_bpm_dj_l, s_bpm_link_l };
        for (unsigned i = 0; i < sizeof(big) / sizeof(big[0]); i++)
            if (bridge_on) lv_obj_remove_flag(big[i], LV_OBJ_FLAG_HIDDEN);
            else           lv_obj_add_flag(big[i], LV_OBJ_FLAG_HIDDEN);
        for (unsigned i = 0; i < sizeof(pair) / sizeof(pair[0]); i++)
            if (bridge_on) lv_obj_add_flag(pair[i], LV_OBJ_FLAG_HIDDEN);
            else           lv_obj_remove_flag(pair[i], LV_OBJ_FLAG_HIDDEN);
    }

    rect(scr, 0, 150, 320, 1, C_DIV, LV_OPA_COVER, C_DIV, 0, 0);

    label(scr, 8, 158, "ABLETON LINK", &lv_font_montserrat_12, C_CYAN);
    s_peer = label(scr, 8, 174, "1 peer", &lv_font_montserrat_16, C_WHITE);
    /* Touch area: created BEFORE the labels, so it sits under them in draw
     * order and cannot affect how they are rendered. LVGL still delivers the
     * click here, because it looks for the first CLICKABLE object and labels
     * are not. Same mechanism as the two toggles. */
    s_offset_hit = rect(scr, 192, 154, 122, 50, C_BG, LV_OPA_TRANSP, C_BG, 0, 0);
    lv_obj_add_flag(s_offset_hit, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_offset_hit, on_offset_tap, LV_EVENT_CLICKED, NULL);

    /* The destination name sits on a line of ITS OWN, above the other two:
     * stretching "PHASE OFFSET" horizontally would have invaded the space of
     * the dots, whereas vertically the band has room.
     *
     *     OSC              <- only with several destinations
     *     PHASE OFFSET
     *     +0.5 ms
     *
     * With a single destination the first line disappears and the other two
     * rise back to the centre, exactly where they have always been. */
    s_offset_dest = label(scr, 196, 157, "", &lv_font_montserrat_12, C_AMBER);
    s_offset_lbl  = label(scr, 196, 166, "PHASE OFFSET", &lv_font_montserrat_12, C_MUTED2);
    s_offset_val  = label(scr, 196, 182, "+0 ms", &lv_font_montserrat_16, C_LABEL);
    lv_obj_add_flag(s_offset_dest, LV_OBJ_FLAG_HIDDEN);

    /* Cycle indicators: a column of dots at the far right of the band, where
     * they take space from neither the caption nor the value. They appear only
     * when more than one destination is active. */
    for (int d = 0; d < OFF_DEST_COUNT; d++) {
        /* Diameter OFF_DOT_D_OFF, becoming OFF_DOT_D_ON on the focused one,
         * see the refresh. The position is recomputed from the CENTRE so the
         * dot does not move as it grows — but only because the two diameters
         * share the same parity: see the comment on the macros. The pitch is 11
         * because at 7 px the neighbour would be grazed. Horizontal centre
         * fixed at 308. */
        s_offset_dot[d] = rect(scr, OFF_DOT_X(OFF_DOT_D_OFF),
                               OFF_DOT_Y(d, OFF_DOT_D_OFF),
                               OFF_DOT_D_OFF, OFF_DOT_D_OFF,
                               C_MUTED, LV_OPA_COVER, C_MUTED, 0, LV_RADIUS_CIRCLE);
        lv_obj_add_flag(s_offset_dot[d], LV_OBJ_FLAG_HIDDEN);
    }

    if (!(s_clock_src != CLOCK_SRC_NONE)) {   /* same criterion as the refresh */
        lv_obj_add_flag(s_offset_lbl, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_offset_val, LV_OBJ_FLAG_HIDDEN);
    }

    rect(scr, 0, 208, 320, 1, C_STRIP, LV_OPA_COVER, C_STRIP, 0, 0);

    /* --- status strip (see the comment block on the badges) --- */
    badge_make(&s_bg_lan, scr, "LAN", strip_text_w("LAN") + 2 * STRIP_PAD_X);
    badge_make(&s_bg_midi, scr, "MIDI", strip_text_w("MIDI") + 2 * STRIP_PAD_X);
    badge_make(&s_bg_osc, scr, "OSC", strip_text_w("OSC") + 2 * STRIP_PAD_X);
    badge_make(&s_bg_cv,  scr, "CV",  strip_text_w("CV")  + 2 * STRIP_PAD_X);
    badge_make(&s_bg_sd,  scr, "SD",  strip_text_w("SD")  + 2 * STRIP_PAD_X);

    /* The right-hand badge is created as "WIFI" but its initials change with
     * the mode: both widths are measured at once, so the reflow never has to
     * remeasure anything at runtime. */
    s_w_wifiap = strip_text_w("WIFI AP") + 2 * STRIP_PAD_X;
    s_w_wifi   = strip_text_w("WIFI")    + 2 * STRIP_PAD_X;
    /* Drawn in the mode we are actually in, so the first frame is already
     * right — see ui_set_wifi_ap_initial. */
    badge_make(&s_bg_wifi, scr,
               s_wifi_ap_initial ? "WIFI AP" : "WIFI",
               s_wifi_ap_initial ? s_w_wifiap : s_w_wifi);

    /* AUTH has one appearance only — red — so the colour is set here once and
     * for all, and at runtime there is nothing left but to show or hide it.
     * Likewise the client count badge, which is always green: it is created
     * wide enough for two digits, and every change of count remeasures it. */
    badge_make(&s_bg_auth, scr, "AUTH", strip_text_w("AUTH") + 2 * STRIP_PAD_X);
    lv_obj_set_style_bg_color(s_bg_auth.box, C_RED, 0);
    lv_obj_set_style_text_color(s_bg_auth.txt, badge_ink(C_RED), 0);

    badge_make(&s_bg_num, scr, "0", strip_text_w("00") + 2 * STRIP_PAD_X);
    lv_obj_set_style_bg_color(s_bg_num.box, C_GREEN, 0);
    lv_obj_set_style_text_color(s_bg_num.txt, badge_ink(C_GREEN), 0);

    /* LAN and WIFI are always present; OSC appears only when that output is on,
     * AUTH only if the key is refused, the count only with at least one client:
     * the first refresh decides. */
    badge_show(&s_bg_lan,  true);
    badge_show(&s_bg_wifi, true);
    strip_reflow_left();
    /* The right group must be placed AT ONCE, and IN THE MODE WE ARE IN: the
     * first refresh arrives 50 ms later, and until then the badge sits wherever
     * this call puts it. In client mode the signal bars take room at the edge
     * and the badge sits to their left; in SoftAP there are no bars and it goes
     * against the edge. Placed for the wrong mode, it visibly jumps once the
     * refresh corrects it. */
    strip_reflow_right(s_wifi_ap_initial);

    /* Signal bars: they belong to client mode, and they are hidden or shown
     * from the start for the same reason the badge is placed from the start —
     * appearing at the first refresh is a change the eye catches. */
    build_signal_bars(scr, STRIP_R_END - STRIP_BARS_W, STRIP_Y);
    for (int i = 0; i < 4; i++) {
        if (s_wifi_ap_initial) lv_obj_add_flag(s_sig_bars[i], LV_OBJ_FLAG_HIDDEN);
        else                   lv_obj_remove_flag(s_sig_bars[i], LV_OBJ_FLAG_HIDDEN);
    }
}

/* --- display flush: serialised with the W5500 through the BSP semaphore ---
 * The flush is an asynchronous DMA. The semaphore is taken before starting and
 * released (from the ISR) when the DMA ends, in on_flush_done: that way the
 * W5500 cannot touch the bus while the display is transferring. */
static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    (void)disp;
    int w = area->x2 - area->x1 + 1;
    int h = area->y2 - area->y1 + 1;

    /* BYTE SWAP **BEFORE** TAKING THE BUS.
     *
     * The panel wants the pixels big-endian, and this conversion touches EVERY
     * pixel of the area: pure CPU work, which needs no SPI bus at all. Doing it
     * inside the lock, as it once was, meant shutting the W5500 out for the
     * whole duration of the swap — milliseconds, over large areas — without a
     * single byte travelling on the wire.
     *
     * The bus is now taken ONLY for the DMA, which is the one thing that really
     * uses it. */
    lv_draw_sw_rgb565_swap(px_map, (uint32_t)(w * h));

#if DJB_DEVELOPER
    /* KEEP A COPY for screenshots, but ONLY WITH THE OPTION ON.
     *
     * It goes here, after the swap and before the bus is taken: the pixels are
     * already in the panel's byte order, which is also the framebuffer's, so it
     * is a plain memcpy — and outside the lock it never touches the bus.
     *
     * THE CONDITION IS NOT A DETAIL. A copy on every flush is a cost paid on
     * every frame, and the figures this device is judged by — flush duration,
     * bus possession, beat jitter — would carry it whether or not anybody ever
     * asks for a picture. Left unconditional it would quietly move the very
     * numbers used to decide everything else.
     *
     * Without the copy the framebuffer holds only what About drew through
     * display.c's primitives, and a screenshot of the dashboard comes out
     * black — which is what the option turns on. */
    if (s_shots_on)
        display_capture_area(area->x1, area->y1, w, h, (const uint16_t *)px_map);
#endif

    /* Quiet window: a beat packet is about to go out, so the SPI bus must be
     * free — a flush lasts up to about 6.5 ms and would put the beat out of
     * phase. We WAIT for the window to pass rather than skipping the flush.
     *
     * Skipping used to mean calling lv_display_flush_ready(): LVGL marked the
     * area as updated and never redrew it, so that piece of screen stayed stale
     * forever ("ABLE" instead of "ABLETON"). Invalidating the whole screen
     * would be worse: an avalanche of full redraws, longer and longer, more and
     * more likely to land back here. Waiting is simple and correct: no area is
     * lost, and the flush starts as soon as the beat has left. The LVGL task
     * runs at low priority, so while it sleeps the beat flies.
     *
     * THE WAIT LIVES HERE, NOT AT THE TOP OF THE FUNCTION. The check used to
     * come first and the byte swap after it, which on a large area costs
     * milliseconds of CPU: between "the window is closed" and actually taking
     * the bus, all that time passed, and the window could open in between.
     * Checking immediately before bsp_spi_lock_display() cuts that distance to
     * the minimum.
     *
     * A known limit remains, which no ordering can remove: the window prevents
     * a flush from STARTING, it does not shorten one already running. But
     * BEAT_QUIET_US (14 ms) is more than twice the longest flush measured
     * (6.5 ms), so even a DMA started an instant before the window opens
     * finishes with wide margin before T_send. */
    int64_t t_defer = esp_timer_get_time();
    while (bsp_spi_in_quiet_window()) {
        vTaskDelay(1);              /* one tick = 1 ms, FREERTOS_HZ=1000 */
    }
    uint32_t defer_us = (uint32_t)(esp_timer_get_time() - t_defer);
    if (defer_us < 200) defer_us = 0;      /* not the window: just the tick */

    bsp_spi_lock_display(w, h, defer_us);
    esp_lcd_panel_draw_bitmap(s_panel, area->x1, area->y1,
                              area->x2 + 1, area->y2 + 1, px_map);
}

static bool on_flush_done(esp_lcd_panel_io_handle_t io,
                          esp_lcd_panel_io_event_data_t *ed, void *ctx)
{
    (void)io; (void)ed;
    BaseType_t hpw = pdFALSE;
    bsp_spi_unlock_isr(&hpw);                 /* end of DMA: release the bus */
    /* Also unblock whoever draws DIRECTLY into the framebuffer, that is the
     * About screen: this callback has replaced the one in display.c, so it is
     * the only one that sees the end of the transfer. */
    display_trans_done_isr(&hpw);
    lv_display_flush_ready((lv_display_t *)ctx);
    return hpw == pdTRUE;
}

esp_err_t ui_init(void)
{
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_affinity = 1;
    port_cfg.task_priority = 2;
    /* [M] Measured in the field: the task uses 4800 bytes, so 8 KB still leaves
     * 40% of headroom. The stack comes from the INTERNAL heap, which with an
     * Ableton peer connected falls to about 13 KB free. */
    port_cfg.task_stack    = 8192;
    ESP_RETURN_ON_ERROR(lvgl_port_init(&port_cfg), TAG, "lvgl_port_init");

    s_panel = display_get_panel();
    esp_lcd_panel_io_handle_t io = display_get_io();

    lvgl_port_lock(0);

    /* A MANUAL display: we own the flush_cb, with its semaphore, rather than
     * leaving it to esp_lvgl_port, so that it can be serialised against the
     * W5500. */
    s_disp = lv_display_create(BSP_LCD_H_RES, BSP_LCD_V_RES);
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(s_disp, flush_cb);

    /* Partial render buffers, in internal RAM because the flush_cb sends them
     * to the panel over DMA — they cannot be moved to PSRAM.
     *
     * Going from 40 rows to 24 frees 20 KB of internal RAM, but the benefit is
     * not only memory: [M] the flush drops from about 10.7 ms to 6.4, so the
     * display holds the SPI bus for less time. It is the same bus as the W5500,
     * and in the logs the network waited up to 25 ms for it — jitter that lands
     * straight on DJ Link packet timing.
     *
     * COST: 10 passes for a full screen instead of 6. The dashboard updates
     * small areas at 20 Hz rather than redrawing whole screens, and the display
     * occupies 0.1% of the time. */
    size_t bufsz = BSP_LCD_H_RES * 24 * sizeof(lv_color16_t);
    void *b1 = heap_caps_malloc(bufsz, MALLOC_CAP_DMA);
    void *b2 = heap_caps_malloc(bufsz, MALLOC_CAP_DMA);
    if (!b1 || !b2) { lvgl_port_unlock(); ESP_LOGE(TAG, "draw buffer alloc"); return ESP_ERR_NO_MEM; }
    lv_display_set_buffers(s_disp, b1, b2, bufsz, LV_DISPLAY_RENDER_MODE_PARTIAL);

    /* end-of-DMA callback: releases the semaphore and signals flush done */
    const esp_lcd_panel_io_callbacks_t io_cbs = { .on_color_trans_done = on_flush_done };
    esp_lcd_panel_io_register_event_callbacks(io, &io_cbs, s_disp);

    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, touch_read_cb);
    lv_indev_set_display(indev, s_disp);
    build_dashboard();
    s_dash_scr = lv_screen_active();            /* remember the dashboard screen */
    settings_ui_init();                          /* create the settings screen, hidden */
    s_offset_osc_us[0] = settings_get_phase_offset_osc_us(false);  /* da DJ Link */
    s_offset_osc_us[1] = settings_get_phase_offset_osc_us(true);   /* da Ableton */
    s_offset_midi_us[0] = settings_get_phase_offset_midi_us(false);
    s_offset_midi_us[1] = settings_get_phase_offset_midi_us(true);
    s_offset_cv_us[0] = settings_get_phase_offset_cv_us(false);
    s_offset_cv_us[1] = settings_get_phase_offset_cv_us(true);
    s_offset_dj_us     = settings_get_phase_offset_dj_us();       /* persistent */
    s_offset_abl_us = settings_get_phase_offset_abl_us();
    /* The initial-silence count starts here: the dashboard exists and is
     * drawn in its resting state. */
    s_ui_ready_us = esp_timer_get_time();
    lv_timer_create(refresh_timer, 50, NULL);       /* 20 Hz (smoother beat) */
    lv_timer_create(buttons_timer, 40, NULL);       /* screen-printed keys ~25Hz */
    lvgl_port_unlock();

    ESP_LOGI(TAG, "dashboard live (refresh 20Hz, interactive toggles)");
    return ESP_OK;
}
