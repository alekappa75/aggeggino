/* about — see about.h for why this component exists. */

#include "about.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdlib.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_lvgl_port.h"
#include "display.h"
#include "lvgl.h"
#include "bsp_cores3.h"
#include "audio.h"
#include "qr_url.h"
#include "esp_app_desc.h"
#include <stdio.h>

static const char *TAG = "about";

/* ============================================================================
 * GEOMETRY AND BUDGET
 *
 * The arithmetic that decides everything: the panel is on SPI at 20 MHz, that
 * is 2.5 MB/s. A whole screen in RGB565 is 153,600 bytes, so 61 ms, and at most
 * 16 frames per second.
 *
 * That is why nothing here sends the whole screen every frame. Effects send
 * only the rectangles they dirtied, which is what makes 30 frames per second
 * possible at all.
 *
 * The bus is shared with the W5500 that carries the clock, and that traffic is
 * protected by the beat's quiet window: the disturbance falls where it does no
 * harm.
 * ==========================================================================*/
#define FX_W          320
#define FX_Y            0          /* full screen: this is a reveal         */
#define FX_H          240          /* 10 x BAND_ROWS                        */
/* Frame period, 33 ms, which is 30 frames per second.
 *
 * It is affordable only because the effects send dirty rectangles rather than
 * the full screen. [M] A full-screen frame costs about 61 ms of SPI: at 30 fps
 * that would be well over 100% of the bus. An effect that changes everywhere
 * at once — a fire, a starfield — would need a different geometry, and at that
 * point one goes back to sending a band. */
#define FX_FPS_MS      33          /* 30 frames per second: see above       */
#define FX_MAX_MS   60000          /* safety exit: one minute               */

/* The task runs at LOW priority on purpose. This is entertainment, and it must
 * not take a microsecond from the clock transports living on core 1 at
 * priorities 8 to 10. It sits on core 0, where the load is lighter. */
#define FX_TASK_PRIO    3
#define FX_TASK_STACK 3072
#define FX_TASK_CORE     0

static volatile bool s_run  = false;
static volatile bool s_stop = false;
static about_done_fn s_done_cb = NULL;




static const uint16_t *s_tex = NULL;
static int s_tex_w = 0, s_tex_h = 0;


void about_set_texture(const uint16_t *rgb565, int w, int h)
{
    s_tex = rgb565; s_tex_w = w; s_tex_h = h;
}




/* ============================================================================
 * SLIDE — the lines of text come in from the left
 *
 * One line at a time, with a delay between them, like closing credits. Each
 * line starts off screen to the left and settles at its position.
 *
 * DECELERATION, not constant speed: each frame covers a FRACTION of the
 * remaining distance. That is the motion the eye reads as "arrives and settles"
 * rather than "stops dead", and it costs one multiplication.
 *
 * COST: the dirty strip is as tall as the text and as wide as needed. [M] A
 * 320x24 line costs 6.1 ms to send — the bulk of it — against about 0.4 ms to
 * restore the background and 0.1 to draw the text. With two lines moving at
 * once that is 37% of a frame at 30 frames per second.
 *
 * THE BACKGROUND IS RESTORED on every frame and the text goes over it
 * transparently: it is the only way to slide something across without leaving
 * trails.
 * ==========================================================================*/
/* --- FONT: Montserrat, for real --------------------------------------------
 *
 * WHY THE MASK AND NOT display_font_text. The lines move: every frame would
 * otherwise need the cmap lookup, the kerning and the glyph conversion again,
 * to produce the identical result each time. The mask does that work ONCE at
 * the start of the phase and leaves the frame only the blend with the
 * background. Masks live in PSRAM (about 5.5 KB per line) and internal RAM is
 * left alone; in fact, once they are built, the 2.3 KB conversion buffer is
 * handed back.
 *
 * IF CONSTRUCTION FAILS the code falls back to the proportional 8x8 font, which
 * still works: an About screen should not stay blank for want of memory. */
#define SL_SCALE     2             /* 8x8 at scale 2, fallback only          */
#define SL_GAP       2             /* inter-character columns, fallback only */
#define SL_H_FALLBK 20             /* strip height, fallback only            */

/* --- THE RIGHT MARGIN, one for everything ----------------------------------
 *
 * EVERYTHING closes on this vertical: the QR, the firmware notes, the headword,
 * the phonetics and the three lines of the definition. One number governs the
 * whole panel, and a few pixels of discrepancy between any two blocks are
 * enough to make it look crooked.
 *
 * Right alignment is computed at runtime from the width of each rasterised
 * line (see slide_init), so no width has to be measured by hand and no constant
 * needs redoing when the text or the font size changes. */
#define TXT_R       303            /* where every block ends                 */

/* The dictionary entry occupies the LOWER HALF: the QR and the firmware notes
 * close at y=126, and the five lines are 108 px tall, so they run 129..237.
 * There are only 3 px left at the bottom — if the description ever grows a
 * line, or the font size changes, this is the number that gives way first. */
#define SL_Y0       129            /* top of the first line                  */

/* --- THE PALETTE IS THE ONE FROM THE OTHER SCREENS -------------------------
 *
 * The background is the same dark blue-grey as the dashboard and the menus, not
 * a photograph: over an image any text competes with what lies beneath it, and
 * About would look like a screen from a different program.
 *
 * The values are COPIED from ui.c (C_BG, C_AMBER). Copied rather than shared
 * because this component does NOT depend on ui — it is ui that depends on it, by
 * way of settings_ui, and including from this side would close a cycle. If the
 * palette changes over there, these must be updated by hand. */
#define RGB24_565(c) ((uint16_t)((((c) >> 19) & 0x1F) << 11 | \
                                 (((c) >> 10) & 0x3F) <<  5 | \
                                 (((c) >>  3) & 0x1F)))
#define FX_BG      RGB24_565(0x0E1116)   /* C_BG from the dashboard */
/* Pure white is gone. It was the only colour on the screen without a reason,
 * and it broke the reference this palette is making: a phosphor terminal never
 * had white. The green is half the quotation. */
#define FX_GREEN_HI RGB24_565(0xCCFFCC)  /* green so pale it reads white */
#define FX_GREEN   RGB24_565(0x33FF33)   /* headword and phonetics       */
#define FX_GREEN_2 RGB24_565(0x26B826)   /* the definition               */
#define FX_GREEN_3 RGB24_565(0x1F9E1F)   /* the technical notes beside   */

/* THREE LEVELS, NOT ONE. With a single green everything carries equal weight
 * and the screen reads as a list. Scaling the brightness gives the hierarchy of
 * a printed page: the headword first, then the definition, then the notes in
 * smaller type.
 *
 * Contrast against the background falls from 10.9:1 to 5.8:1 to 4.3:1. Below
 * 4:1 small text on a dark background starts to cost effort, so the third level
 * is the last one that can be spent — it is not a value picked by eye and left
 * there. */

/* --- SPACE RESERVED AT THE TOP ---------------------------------------------
 *
 * The upper half holds the QR code pointing at the repository, and the firmware
 * notes beside it. The space is reserved HERE because it is the height left
 * over that decides the font sizes and the line breaks of the entry below, not
 * the other way round.
 *
 * The code is 25 modules per side at 4 pixels each, and the modules must come
 * out WHOLE or it becomes unreadable — see qr_url.h. */
/* The QR sits at the TOP RIGHT, and its vertical position is chosen so that the
 * visible margin matches the one on the right: the first module lands at y=17,
 * exactly 320-TXT_R from the right edge. The box begins 8 px earlier because
 * the quiet zone is background, and invisible. */
#define QR_TOP        9            /* top of the QR box                      */
/* --- THE QR SITS ON THE RIGHT ----------------------------------------------
 *
 * It ends exactly where the widest line of the description ends, and the
 * technical notes align against it from the left.
 *
 * The QR is the rigid block: its width comes from the data. So it is the QR
 * that is aligned to the text margin, and the notes that lean against the QR —
 * putting the notes on the outside instead would anchor them to the screen
 * edge, and they would close a few pixels past where the description does.
 *
 * IT IS THE FIRST MODULE THAT ALIGNS, not the edge of the box, which is the
 * background colour and therefore invisible. Hence the two setbacks: the quiet
 * zone, and the difference between the side of the box and that of the modules
 * alone. */
#define QR_LEFT_X   (TXT_R - QR_MOD * QR_SCALE - QR_QUIET * QR_SCALE)

/* Top of the first line of notes, aligned to the QR's FIRST MODULE rather than
 * to the edge of its box: the box is the background colour and invisible, so
 * what the eye sees begin is the module, eight pixels lower. On top of that, a
 * line of text does not start drawing at its very top — above the capitals of
 * Montserrat 14 there are 3 empty pixels.
 *
 *      first module = QR_TOP + quiet(8)  = 128
 *      ink          = INFO_Y + 3
 *      -> INFO_Y = 125
 *
 * Written as a formula rather than as 125, so it stays right if the QR's scale
 * or position changes. The 3 px are measured against the font instead: that is
 * the one piece that would need redoing at a different size. */
#define INFO_INK     3             /* [M] empty pixels above the capitals, Montserrat 14 */
#define INFO_Y      (QR_TOP + QR_QUIET * QR_SCALE - INFO_INK)

/* RIGHT edge of the notes, 12 px from the QR's first module. The lines vary a
 * great deal in length — from 37 px to 113 — and left-aligned they left a
 * ragged right margin; right-aligned they form a column that leans against the
 * code. The widest starts at x=73, well inside the screen. */
#define INFO_R      (QR_LEFT_X + QR_QUIET * QR_SCALE - 12)

/* The SAME green as the crest of the scan bar. The code and the blade that
 * uncovers the image are the two brightest things on screen, and giving them
 * one colour ties them together instead of making them look like two elements
 * taken from different places. Contrast against the background is 16.8:1 —
 * more than the ivory used before (14.4:1), so legibility gains too. */
#define QR_FG        FX_GREEN_HI   /* modules: the inverted code             */
#define QR_BG        FX_BG         /* background: the screen's own           */
/* The QR grew from 87 to 116 px to be readable hand-held, and the 29 px came
 * from the SPACING BETWEEN LINES, not from the font sizes: the height of the
 * lines (106 px altogether) cannot be squeezed without shrinking the text,
 * which is the very thing that had just been enlarged. Montserrat's line
 * heights already include their own leading, so at zero gap the lines still
 * stand apart. */
/* The side is NOT a chosen number: it comes from the data (QR_PX in qr_url.h)
 * and is modules times scale, with an integer scale. Picking it by hand and
 * fitting the grid to it is how one ends up with a code that will not scan. */
/* --- TELETYPE --------------------------------------------------------------
 *
 * The text does not slide in from the left: it is TYPED, one character at a
 * time, with a cursor block running ahead of each line and disappearing when
 * the line is finished.
 *
 * THE LINES OVERLAP IN TIME. Each declares in its 'start' field at which
 * character it begins: the dictionary entry staggered three by three, the
 * technical notes much later. For a couple of seconds several cursors advance
 * together at different apparent speeds — the lines differ in length and finish
 * at staggered times. It is that simultaneity that makes it read as a
 * science-fiction terminal rather than a typewriter.
 *
 * IT IS CHEAP BY CONSTRUCTION. A frame changes only the last character typed
 * and the cursor, some twenty pixels of width per active line, and only those
 * rectangles are sent. [M] For comparison, repainting a full-width band of one
 * line of text costs 9.9 ms per frame — more than the whole frame budget at 30
 * frames per second.
 *
 * With a proportional font the cursor advances in irregular steps, where a real
 * terminal was fixed pitch. That is a deliberate choice: changing font would
 * have changed the look of the whole dictionary entry, not just the
 * animation. */
#define TT_FRAMES    2             /* frames per character                   */

LV_FONT_DECLARE(lv_font_montserrat_bold_18);
LV_FONT_DECLARE(lv_font_montserrat_ipa_18);

/* --- THE DICTIONARY ENTRY --------------------------------------------------
 *
 * Each line has its OWN font. The headword is bold at the SAME size as the
 * rest: weight alone is enough to tell it from the definition, which is how a
 * printed dictionary does it. At bold 24 it weighed too much — across 320
 * pixels a headword as tall as two lines of definition does not make hierarchy,
 * it makes signage.
 *
 * THE TRANSCRIPTION IS ITALIAN IPA. In Italian a double consonant is held long,
 * so "gg" before e or i is /ddʒ/, and the stress falls on the penultimate
 * syllable: ag-ge-GGI-no, like cappuccino. Hence
 *
 *      /addʒedˈdʒino/
 *
 * where \u0292 is the ezh and \u02c8 the primary stress mark. They are written
 * as universal character names rather than as hex bytes ON PURPOSE:
 * "\xCA\x92ed" would be read by the compiler as one enormous escape, because
 * \x devours every hex digit it finds — and 'e' and 'd' are hex digits.
 *
 * The ʤ ligature as a single glyph does not exist in the font, but the extended
 * form d+ʒ is the correct one for modern IPA anyway.
 *
 * THE WIDTHS ARE MEASURED, not eyeballed: from the generated fonts' advance
 * tables, the widest line is 286 px out of 296 usable (320 less the two 12 px
 * margins).
 *
 * The line breaking is not free. "A small device that persuades two" measures
 * 313 px and overflows — and the line that would not fit was not the one that
 * looked guilty: shortening the THIRD line was tried and failed, because even
 * without the article it stayed over the limit and became ungrammatical.
 * Moving "machines" down brings the third line to 286, and drying the second is
 * then enough: "persuading" instead of "that persuades" takes it to 280. All
 * the words that mattered survive — small, device, persuade, downbeat — and the
 * participle sits closer to the register of a dictionary entry. */
typedef struct {
    const lv_font_t *font;
    const char      *txt;          /* may point at a buffer filled at runtime */
    int16_t          x;            /* column: left, or RIGHT when right=true  */
    bool             right;        /* right aligned */
    int16_t          y;            /* -1 = below the previous one, using its gap */
    int8_t           gap;          /* empty space below the line */
    uint16_t         col;
    int16_t          start;        /* when typing begins, IN CHARACTERS */
} sl_line_t;

/* Version and build date are NOT written by hand. ESP-IDF already puts them in
 * the application header, along with the build time, and reading them from
 * there means they cannot diverge from the binary actually running — which is
 * exactly what a hand-maintained constant does on the first forgotten flash.
 *
 * The version comes from PROJECT_VER: if the CMakeLists does not declare one,
 * ESP-IDF falls back to git describe and then to "1". Worth stating
 * explicitly. */
static char s_fw_ver[24]  = "?";
static char s_fw_date[24] = "?";

static const sl_line_t SL[] = {
 /* font                        text                                  x  right     y gap  colour     start */
 { &lv_font_montserrat_bold_18, "AGGEGGINO",                      TXT_R, true,    -1, 2, FX_GREEN,     0 },
 { &lv_font_montserrat_ipa_18,  "/add\u0292ed\u02c8d\u0292ino/", TXT_R, true,    -1, 6, FX_GREEN,     3 },
 { &lv_font_montserrat_ipa_18,  "n. [It., dim. of aggeggio: gizmo]",TXT_R, true,   -1, 0, FX_GREEN_2,   6 },
 { &lv_font_montserrat_ipa_18,  "A small device persuading two",   TXT_R, true,    -1, 0, FX_GREEN_2,   9 },
 { &lv_font_montserrat_ipa_18,  "machines to share a downbeat.",   TXT_R, true,    -1, 0, FX_GREEN_2,  12 },

  /* --- TECHNICAL NOTES, right of the QR and right-aligned ---------------
   *
   * They begin at character 20, when the dictionary entry is already well
   * under way: first one reads what an aggeggino is, then the data arrive.
   * Starting together would have filled the screen all at once.
   *
   * Size 14, the Montserrat already compiled into LVGL — no new font in flash
   * for six lines of service text. */
 { &lv_font_montserrat_14,      "FIRMWARE",                       INFO_R, true, INFO_Y, 0, FX_GREEN_3, 15 },
 { &lv_font_montserrat_14,      s_fw_ver,                         INFO_R, true,     -1, 5, FX_GREEN,   17 },
 { &lv_font_montserrat_14,      "BUILD DATE",                     INFO_R, true,     -1, 0, FX_GREEN_3, 19 },
 { &lv_font_montserrat_14,      s_fw_date,                        INFO_R, true,     -1, 5, FX_GREEN,   21 },
 { &lv_font_montserrat_14,      "LICENSE",                        INFO_R, true,     -1, 0, FX_GREEN_3, 23 },
 { &lv_font_montserrat_14,      "GPL-3.0-or-later",               INFO_R, true,     -1, 0, FX_GREEN_2, 25 },
};
#define SL_LINES ((int)(sizeof(SL) / sizeof(SL[0])))



static int16_t s_sl_y[SL_LINES];   /* top of each line, accumulated in slide_init */
static int16_t s_sl_px[SL_LINES];  /* effective column, after alignment       */
static int16_t s_tt_n[SL_LINES];   /* characters already typed on this line   */
static int     s_tt_tick;          /* frames since the phase began. Shared by
                                    * the typing and the rollback, and zeroed
                                    * on entering each: the two count from
                                    * their own start, not from the screen's. */
static display_text_mask_t s_sl_m[SL_LINES];   /* rasterised text */
static bool    s_sl_mask_ok = false;

/* Must be called WITH THE LVGL LOCK ALREADY HELD: it reads the fonts, which are
 * LVGL's data. In the task that is the case — this phase begins inside the
 * loop, well after lvgl_port_lock(). */
static void slide_init(void)
{
    for (int i = 0; i < SL_LINES; i++) s_tt_n[i] = 0;
    s_tt_tick = 0;

    /* Build data BEFORE the masks: what gets rasterised is whatever the buffers
     * hold, so filling them afterwards would yield masks showing a question
     * mark. */
    const esp_app_desc_t *app = esp_app_get_description();
    if (app) {
        /* The 'v' is prepended here, but only if it is not already there: with
         * PROJECT_VER derived from 'git describe' the string often arrives as
         * "v1.0.0", and one would end up with "vv1.0.0". */
        const char *v = app->version;
        snprintf(s_fw_ver, sizeof(s_fw_ver),
                 (v[0] == 'v' || v[0] == 'V') ? "%.23s" : "v%.22s", v);
        snprintf(s_fw_date, sizeof(s_fw_date), "%.23s", app->date);
    }

    s_sl_mask_ok = true;
    int64_t ta = esp_timer_get_time();
    for (int i = 0; i < SL_LINES; i++)
        if (!display_text_mask_build(SL[i].font, SL[i].txt, &s_sl_m[i]))
            s_sl_mask_ok = false;
    int64_t tb = esp_timer_get_time();

    /* The lines stack: each below the previous one, as tall as its own font. A
     * fixed step would not do now that the sizes differ. With the 8x8 fallback
     * the fonts do not matter and a single step is used. */
    int y = SL_Y0;
    for (int i = 0; i < SL_LINES; i++) {
        if (SL[i].y >= 0) y = SL[i].y;      /* this line opens a new block */
        s_sl_y[i] = (int16_t)y;
        y += (s_sl_mask_ok ? SL[i].font->line_height : SL_H_FALLBK) + SL[i].gap;

        /* --- RIGHT ALIGNMENT -------------------------------------------
         *
         * Computed HERE and not in the table because it needs the width of the
         * text, which is known only after rasterising it. The setback is
         * ink_x1, the right edge of the INK rather than the declared width:
         * the advance of the last character includes spacing that is not
         * visible, and aligning on that would leave the lines visibly out of
         * line with each other.
         *
         * Without a mask there is no width to back off from, and the line
         * stays where it is: the 8x8 fallback loses the alignment, not the
         * text. */
        s_sl_px[i] = SL[i].x;
        if (SL[i].right && s_sl_mask_ok) {
            const int width = s_sl_m[i].ink_x1;
            if (width > 0) s_sl_px[i] = (int16_t)(SL[i].x - width);
        }
    }
    if (y > FX_H)
        ESP_LOGI(TAG, "text reaches y=%d: past the bottom of the screen (%d)",
                 y, FX_H);

    if (!s_sl_mask_ok) {
        ESP_LOGW(TAG, "text masks not built: falling back to 8x8");
        for (int i = 0; i < SL_LINES; i++) display_text_mask_free(&s_sl_m[i]);
    } else {
        /* Everything is rasterised: the conversion buffer is no longer needed. */
        display_font_cache_release();
        /* The measurement is the point: this cost is paid ONCE, and is to be
         * weighed against what would otherwise be paid on every frame. */
        int px = 0;
        for (int i = 0; i < SL_LINES; i++) px += s_sl_m[i].w * s_sl_m[i].h;
        ESP_LOGI(TAG, "masks: %d lines, %d pixels, %lld us in all | "
                      "internal heap %u", SL_LINES, px, tb - ta,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    }
}

static void slide_free(void)
{
    for (int i = 0; i < SL_LINES; i++) display_text_mask_free(&s_sl_m[i]);
    s_sl_mask_ok = false;
}

/* --- THE QR CODE TOWARDS THE REPOSITORY ------------------------------------
 *
 * Drawn ONCE, as the text finishes arriving: that is the moment the screen
 * settles and a reader can reach for their phone.
 *
 * ON A SOLID PANEL. The rest of the intro blends with the background; this does
 * not. A decoder looks for squares of exactly two colours, and any shading
 * underneath would give unpredictable contrast. [M] The quiet zone is part of
 * the code, not a margin — without it, reading fails.
 *
 * NO SHADING, NO HALF PIXELS: every module is a solid square of side QR_SCALE.
 * That is why the modules arrive as a bit table rather than as an image to be
 * rescaled. */
/* Repaint a rectangle of background. The background is a colour rather than an
 * image, so copying it from a bitmap would be work to obtain the same pixel
 * every time. */
static void fill_rect(uint16_t *fb, int x, int y, int w, int h, uint16_t col)
{
    const uint16_t c = display_rgb565_to_fb(col);
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > FX_W) w = FX_W - x;
    if (y + h > FX_H) h = FX_H - y;
    for (int r = 0; r < h; r++) {
        uint16_t *dst = &fb[(size_t)(FX_Y + y + r) * FX_W + x];
        for (int i = 0; i < w; i++) dst[i] = c;
    }
}

/* Interpolate two RGB565 colours. Needed ONCE per frame, not per pixel, so the
 * cost is irrelevant and there is nothing to be clever about. t runs 0 to 32. */
static uint16_t mix565(uint16_t a, uint16_t b, int t)
{
    const int ra = (a >> 11) & 0x1F, ga = (a >> 5) & 0x3F, ba = a & 0x1F;
    const int rb = (b >> 11) & 0x1F, gb = (b >> 5) & 0x3F, bb = b & 0x1F;
    const int r = (ra * (32 - t) + rb * t) >> 5;
    const int g = (ga * (32 - t) + gb * t) >> 5;
    const int c = (ba * (32 - t) + bb * t) >> 5;
    return (uint16_t)((r << 11) | (g << 5) | c);
}

/* Like mix565, but with t from 0 to 255.
 *
 * THE QR NEEDS IT, the bar does not. [M] Between the background and the pale
 * green, RGB565 offers 100 distinct colours; interpolating in 32 steps reaches
 * 33 of them, and over the narrow upper range the breath uses, just 9 instead
 * of 25. The pulse looked stepped not because the frames were too few but
 * because the COLOUR jumped: it held for four frames and then moved two levels
 * at once.
 *
 * On the bar the problem does not arise: there the gradient is spatial, across
 * sixteen adjacent rows, and the eye has no way of noticing the steps. */
static uint16_t mix565f(uint16_t a, uint16_t b, int t)
{
    const int ra = (a >> 11) & 0x1F, ga = (a >> 5) & 0x3F, ba = a & 0x1F;
    const int rb = (b >> 11) & 0x1F, gb = (b >> 5) & 0x3F, bb = b & 0x1F;
    const int r = (ra * (255 - t) + rb * t) / 255;
    const int g = (ga * (255 - t) + gb * t) / 255;
    const int c = (ba * (255 - t) + bb * t) / 255;
    return (uint16_t)((r << 11) | (g << 5) | c);
}

/* --- THE GREEN PASS, and the table that makes it cheap ---------------------
 *
 * The blocks of the selfie arrive first in MONOCHROME GREEN and take their real
 * colours a moment later. The green is FX_GREEN, the entry's own, so the two
 * halves of the storyboard share one palette.
 *
 * THE THIRTY-THREE SHADES ARE PRECOMPUTED. Interpolating per pixel would mean
 * calling mix565 a thousand times per block — two thousand per frame — to
 * produce only 33 distinct values, since the luminance that drives it is a
 * 0..32 quantity. The table is built once and costs 66 bytes.
 *
 * The luminance is the usual weighted sum, green counting for about three
 * quarters of what the eye perceives. It is computed in integers straight on
 * the 5/6/5 fields without expanding to 8 bits: the result only picks a row of
 * the table, so the low bits would be thrown away regardless. */
static uint16_t s_green_lut[33];

static void green_lut_build(void)
{
    /* Stored ALREADY CONVERTED for the framebuffer. mix565 works on standard
     * RGB565 — that is what RGB24_565 produces — while the panel wants the two
     * bytes the other way round, and doing the swap here means doing it 33
     * times instead of once per pixel. */
    for (int t = 0; t <= 32; t++)
        s_green_lut[t] = display_rgb565_to_fb(mix565(FX_BG, FX_GREEN, t));
}

static void blit_green(uint16_t *fb, const uint16_t *tex, int sx, int sy,
                       int dx, int dy, int w, int h)
{
    if (!tex) { fill_rect(fb, dx, dy, w, h, 0); return; }
    for (int row = 0; row < h; row++) {
        const uint16_t *s = tex + (size_t)(sy + row) * FX_W + sx;
        uint16_t       *d = fb  + (size_t)(dy + row) * FX_W + dx;
        for (int col = 0; col < w; col++) {
            /* The texture is STANDARD RGB565, so the fields are read straight
             * from it — swapping first would take the bits apart in the wrong
             * places, and the greens came out violet. The conversion belongs on
             * the way OUT, and it is already inside the table. */
            const uint16_t p = s[col];
            int lum = (((p >> 11) & 0x1f) * 54
                    + (((p >>  5) & 0x3f) >> 1) * 183
                    +  ((p        & 0x1f))      * 19) >> 8;   /* 0..31 */
            if (lum > 32) lum = 32;
            d[col] = s_green_lut[lum];
        }
    }
}

/* Draw the QR with its modules at a given fade level: 0 is invisible, the
 * background colour, and 255 is full.
 *
 * FADING A TWO-COLOUR IMAGE IS NOT DONE PER PIXEL. Doing so would mean, for
 * each one, reading the background from PSRAM and blending: ten thousand times
 * a frame to obtain two values. Here the COLOUR is interpolated once and the
 * modules are filled with it — the result on screen is identical and the
 * computation disappears.
 *
 * ONLY THE MODULES ARE REDRAWN, not the surrounding box: the quiet zone is
 * already the screen's background colour, so there is nothing to repaint there.
 * That is 100x100 pixels instead of 116x116, and the transfer is the cost that
 * matters. */
static void fx_qr_level(uint16_t *fb, int t)
{
    const uint16_t module = display_rgb565_to_fb(mix565f(QR_BG, QR_FG, t));
    const int x_ink = QR_LEFT_X + QR_QUIET * QR_SCALE;
    const int y_ink = QR_TOP    + QR_QUIET * QR_SCALE;

    for (int mr = 0; mr < QR_MOD; mr++) {
        uint32_t row = QR_BITS[mr];
        const int y0 = y_ink + mr * QR_SCALE;
        for (int mc = 0; mc < QR_MOD; mc++) {
            /* UNSET modules are repainted like the rest: halfway through the
             * fade, the background between two lit modules is the screen's own
             * colour, and skipping them would leave residue from the previous
             * frame. */
            const uint16_t col = (row & (1u << mc)) ? module
                                                    : display_rgb565_to_fb(QR_BG);
            const int x0 = x_ink + mc * QR_SCALE;
            for (int r = 0; r < QR_SCALE; r++) {
                uint16_t *dst = &fb[(size_t)(FX_Y + y0 + r) * FX_W + x0];
                for (int c = 0; c < QR_SCALE; c++) dst[c] = col;
            }
        }
    }
    display_flush_rect(x_ink, FX_Y + y_ink, QR_MOD * QR_SCALE, QR_MOD * QR_SCALE);
}


/* ============================================================================
 * THE SELFIE ARRIVES BY BLOCKS — the same teletype, on a coarser grid
 *
 * A cursor runs RIGHT TO LEFT and bottom to top, and behind it the portrait
 * appears one 32x32 block at a time, IN MONOCHROME GREEN. The colour does not
 * come by itself: the user brings it out with a finger, afterwards — see the
 * note on the brush.
 *
 * WHY BLOCKS AND NOT ROWS. The grid is the same idea as the typing — something
 * writes, a cursor precedes it — carried to a scale where a whole image can
 * appear in about three seconds. And it costs little: [M] the panel gives 61 ms
 * for a full screen, so one 32x32 block is 0.8 ms.
 *
 * ONE BLOCK PER FRAME, deliberately slow. At two the whole image landed in two
 * seconds and the eye had no time to follow the front.
 *
 * IT STARTS WHILE THE TEXT IS STILL WITHDRAWING, and can start early because
 * BOTH MOVE FROM THE RIGHT. The entry is right-aligned, so the rollback eats it
 * from the right edge inwards; the blocks set off from the bottom right corner.
 * The front is therefore chasing a text edge that is retreating ahead of it,
 * not walking into one.
 *
 * [D] That is what makes 12 possible where a first, cruder estimate said 18.
 * Treating the text as vanishing all at once — and the blocks as arriving all
 * at once — gives a bound of 17 frames; following both edges pixel by pixel
 * gives 70 px of clearance at 12, and 6 px at 10. The margin at 12 covers the
 * approximation left in the model, which uses an AVERAGE character width while
 * the real ones vary by a third either way.
 *
 * The symptom of getting it wrong is a block landing on a line that has not
 * withdrawn yet — visible at once, on the bottom right.
 * ==========================================================================*/
#define BLK          32                    /* side of one block               */
#define BLK_NX       (FX_W / BLK)          /* 10 across                       */
#define BLK_NY       ((FX_H + BLK - 1) / BLK)  /* 8 up, the top one clipped   */
#define BLK_N        (BLK_NX * BLK_NY)
#define BLK_START    12                    /* frame at which the front starts */

static int s_blk_n;                        /* blocks revealed so far          */

/* Where block b sits. They run RIGHT TO LEFT, bottom to top, so b=0 is the
 * bottom RIGHT corner — the one the storyboard starts from. The carriage
 * returns like a typewriter's: at the end of a row it goes back to the right,
 * one row higher. */
static void blk_pos(int b, int *x, int *y, int *h)
{
    const int row = b / BLK_NX;
    const int col = BLK_NX - 1 - (b % BLK_NX);   /* right to left */
    const int y0  = FX_H - (row + 1) * BLK;
    *x = col * BLK;
    *y = (y0 < 0) ? 0 : y0;                /* the top row is shorter          */
    *h = (y0 < 0) ? BLK + y0 : BLK;
}

/* One frame of the reveal. Returns true when the last block is in place. */
static bool fx_blocks(uint16_t *fb, const uint16_t *tex, int tick)
{
    if (tick < BLK_START) return false;    /* still letting the text withdraw */
    if (s_blk_n >= BLK_N)  return true;

    int x, y, h;
    blk_pos(s_blk_n, &x, &y, &h);
    blit_green(fb, tex, x, y, x, y, BLK, h);
    display_flush_rect(x, FX_Y + y, BLK, h);
    s_blk_n++;

    /* The cursor: a solid block where the next one will land. It is drawn over
     * the background, not over the image, so nothing has to be restored — the
     * block that follows covers it.
     *
     * THE SAME GREEN AS THE TEXT CURSOR, and not FX_GREEN_HI: that pale one is
     * reserved for the QR, and at luminance 29 of 31 against the text cursor's
     * 23 it reads as a different object — brighter than anything else on
     * screen, and out of key with the typing it is quoting. */
    if (s_blk_n < BLK_N) {
        blk_pos(s_blk_n, &x, &y, &h);
        fill_rect(fb, x, y, BLK, h, FX_GREEN);
        display_flush_rect(x, FX_Y + y, BLK, h);
    }
    return s_blk_n >= BLK_N;
}

/* ============================================================================
 * THE BRUSH — the colour is brought out with a finger
 *
 * The portrait arrives in green and stays that way. Dragging a finger across
 * the glass reveals the real colours underneath, as if wiping mist from a
 * window. It is the one moment in the whole interface where the screen is not
 * a set of controls but a surface.
 *
 * A ROUND STAMP, SOFT AT THE EDGES. A hard circle would leave visible discs and
 * read as a stencil; the softness is what makes it a brush. The profile is
 * flat out to 65% of the radius and then falls to nothing, which is roughly a
 * Photoshop tip at medium hardness.
 *
 * IT ACCUMULATES. One pass does not reach full colour: the stamp blends towards
 * the texture at BRUSH_A of 32, so going over the same place two or three times
 * saturates it. That is what makes the gesture feel like painting rather than
 * uncovering, and it comes for free — the blend reads the FRAMEBUFFER, which
 * already holds the result of the previous pass.
 *
 * THE ALPHA PROFILE IS PRECOMPUTED, 48x48 bytes in BSS. Working it out per
 * pixel would mean a square root and a curve for every one of 1800 pixels, 60
 * times a second; here it is a table lookup and one blend.
 *
 * [M] COST: the stamp touches a 48x48 square, 2304 px, which is 1.8 ms of
 * transfer — well inside the 16 ms budget of the painting phase.
 * ==========================================================================*/
#define BRUSH_R      24                    /* radius, in pixels               */
#define BRUSH_D      (BRUSH_R * 2)
#define BRUSH_A      20                    /* opacity of one pass, of 32      */
#define BRUSH_HARD   65                    /* per cent of the radius kept flat */
#define BRUSH_STEP   10                    /* px between stamps when dragging  */

static uint8_t s_brush_a[BRUSH_D * BRUSH_D];   /* alpha profile, 0..BRUSH_A   */

static void brush_build(void)
{
    const int flat = BRUSH_R * BRUSH_HARD / 100;
    for (int dy = 0; dy < BRUSH_D; dy++) {
        for (int dx = 0; dx < BRUSH_D; dx++) {
            const int ex = dx - BRUSH_R, ey = dy - BRUSH_R;
            const int d2 = ex * ex + ey * ey;
            uint8_t a = 0;
            if (d2 < BRUSH_R * BRUSH_R) {
                /* Integer distance, good enough for a profile that is then
                 * quantised to 21 levels anyway. */
                int d = 0;
                while ((d + 1) * (d + 1) <= d2) d++;
                if (d <= flat) a = BRUSH_A;
                else {
                    /* Squared fall-off from the flat edge outwards: gentler at
                     * the start than a straight ramp, so the stroke has no
                     * visible rim. */
                    const int t = (BRUSH_R - d) * 32 / (BRUSH_R - flat);
                    a = (uint8_t)(BRUSH_A * t * t / (32 * 32));
                }
            }
            s_brush_a[dy * BRUSH_D + dx] = a;
        }
    }
}

/* One stamp centred on (cx, cy). Clipped to the screen, so the edges can be
 * painted too. */
static void brush_stamp(uint16_t *fb, const uint16_t *tex, int cx, int cy)
{
    if (!tex) return;
    for (int dy = 0; dy < BRUSH_D; dy++) {
        const int y = cy - BRUSH_R + dy;
        if (y < 0 || y >= FX_H) continue;
        const uint8_t  *a = &s_brush_a[dy * BRUSH_D];
        const uint16_t *s = tex + (size_t)y * FX_W;
        uint16_t       *d = fb  + (size_t)(FX_Y + y) * FX_W;
        for (int dx = 0; dx < BRUSH_D; dx++) {
            const int x = cx - BRUSH_R + dx;
            if (x < 0 || x >= FX_W || !a[dx]) continue;
            /* The framebuffer holds the bytes swapped, the texture does not.
             * display_rgb565_to_fb is its own inverse — it only swaps — so the
             * same call serves both ways. */
            const uint16_t cur = display_rgb565_to_fb(d[x]);
            d[x] = display_rgb565_to_fb(mix565(cur, s[x], a[dx]));
        }
    }
    int x0 = cx - BRUSH_R, y0 = cy - BRUSH_R, w = BRUSH_D, h = BRUSH_D;
    if (x0 < 0) { w += x0; x0 = 0; }
    if (y0 < 0) { h += y0; y0 = 0; }
    if (x0 + w > FX_W) w = FX_W - x0;
    if (y0 + h > FX_H) h = FX_H - y0;
    if (w > 0 && h > 0) display_flush_rect(x0, FX_Y + y0, w, h);
}

/* A stroke from the previous position to the current one. Stamping only where
 * the finger IS would leave gaps: at 60 Hz a quick movement covers tens of
 * pixels between one reading and the next, and the trail would come out as
 * separate discs. */
static void brush_stroke(uint16_t *fb, const uint16_t *tex,
                         int x0, int y0, int x1, int y1)
{
    const int ex = x1 - x0, ey = y1 - y0;
    int d = 0;
    while ((d + 1) * (d + 1) <= ex * ex + ey * ey) d++;
    const int n = (d + BRUSH_STEP - 1) / BRUSH_STEP;
    if (n <= 1) { brush_stamp(fb, tex, x1, y1); return; }
    for (int k = 1; k <= n; k++)
        brush_stamp(fb, tex, x0 + ex * k / n, y0 + ey * k / n);
}





/* --- TYPING ONE CHARACTER ---------------------------------------------------
 *
 * Draw line i as it stands now: 'n' characters already typed and, if any
 * remain, the cursor block sitting where the next one will go.
 *
 * ONLY THE BAND THAT CHANGES IS REPAINTED, from where the cursor was to where
 * it ends now. All the text already typed to its left has been still for frames
 * and has no reason to be sent to the panel again: it is the transfer that
 * costs, not the drawing.
 *
 * Character j occupies mask columns [adv[j-1], adv[j]), with adv[-1] = 0 — the
 * same table display.c filled while rasterising, so the cut always falls
 * between two letters.
 *
 * THE CURSOR IS THE NEXT CHARACTER, SOLID. As wide as the letter about to
 * arrive rather than a fixed size: with a proportional font a fixed width would
 * give a block that sometimes overshoots the letter and sometimes falls short,
 * and it would show on every keystroke. */
static void line_type(uint16_t *fb, int i, int n_before, int n_now, int y)
{
    if (!s_sl_mask_ok) {
        /* The 8x8 fallback has no mask to cut: it writes the whole line, which
         * appears all at once rather than character by character. This path
         * runs only if memory ran out.
         *
         * It clears only FROM this column onwards: now that the notes sit right
         * of the QR, sweeping the full width would erase the column beside. */
        fill_rect(fb, s_sl_px[i], y, FX_W - s_sl_px[i], SL_H_FALLBK, FX_BG);
        display_text_tr(s_sl_px[i], FX_Y + y, SL[i].txt, SL[i].col, SL_SCALE, SL_GAP);
        display_flush_rect(s_sl_px[i], FX_Y + y, FX_W - s_sl_px[i], SL_H_FALLBK);
        return;
    }

    const display_text_mask_t *m = &s_sl_m[i];
    if (m->n_char <= 0) return;

    /* Band to repaint: between the two positions, in whichever order they come.
     *
     * IT WORKS BACKWARDS TOO. With n_ora BELOW n_prima the same code erases
     * instead of writing: the band is repainted, the text is drawn only as far
     * as the new position, and the cursor lands there — so it retreats by
     * itself, which is the whole of the rollback effect. Nothing here knows
     * which of the two directions it is serving. */
    const int n_lo  = (n_now < n_before) ? n_now : n_before;
    const int n_hi  = (n_now < n_before) ? n_before : n_now;
    const int x_from = (n_lo > 0) ? m->adv[n_lo - 1] : 0;
    const int x_to   = (n_hi < m->n_char) ? m->adv[n_hi] : m->adv[m->n_char - 1];
    if (x_to <= x_from) return;

    const int ry = y + m->ink_y0;
    const int rh = m->ink_y1 - m->ink_y0;

    const int x0 = s_sl_px[i];
    fill_rect(fb, x0 + x_from, ry, x_to - x_from, rh, FX_BG);

    /* The typed text, only across the part being repainted. */
    const int x_text = (n_now > 0) ? m->adv[n_now - 1] : 0;
    if (x_text > x_from)
        display_text_mask_draw_span(m, x0, FX_Y + y, SL[i].col, x_from, x_text);

    /* The cursor, when the line is neither finished nor empty.
     *
     * THE ZERO CASE MATTERS because of the rollback: at n_now = 0 the line has
     * withdrawn completely, and drawing a cursor there would leave a solid block
     * over the first character — visible for good, since nothing comes after to
     * cover it. During typing the case never arises, the count starting at 1. */
    if (n_now > 0 && n_now < m->n_char)
        fill_rect(fb, x0 + x_text, ry, m->adv[n_now] - x_text, rh, SL[i].col);

    display_flush_rect(x0 + x_from, FX_Y + ry, x_to - x_from, rh);
}

/* One frame of typing. Returns true once every line is complete.
 *
 * THE STAGGER IS COUNTED IN CHARACTERS, NOT IN FRAMES. If TT_FRAMES is ever
 * changed to make the typing slower or faster, the lines stay offset by the
 * same NUMBER OF LETTERS and the effect keeps its shape. Counting it in frames
 * would tie it to the speed. */
/* A pause before anything is typed. The QR has just appeared with the
 * background, and half a second of stillness lets it be read as what it is —
 * something to frame — before the text starts moving beside it. */
#define TYPE_START_F  15         /* frames of stillness: about 0.5 s          */

static bool fx_type(uint16_t *fb)
{
    s_tt_tick++;
    if (s_tt_tick <= TYPE_START_F) return false;   /* the pause, see above */
    bool finite = true;

    for (int i = 0; i < SL_LINES; i++) {
        const int n_max = s_sl_mask_ok ? s_sl_m[i].n_char : 1;
        if (s_tt_n[i] >= n_max) continue;                 /* line finished */
        finite = false;

        /* How many characters this line should have typed BY NOW. The delay is
         * declared by the line itself, so the technical notes can start much
         * later without the formula needing to know which lines are which.
         *
         * COUNTED FROM THE END OF THE PAUSE, not from the start of the phase:
         * the tick runs during the half second the QR is left alone, and taking
         * it raw made the first line appear already eight characters long. */
        const int due = (s_tt_tick - TYPE_START_F) / TT_FRAMES - SL[i].start;
        if (due <= 0) {
            /* Not started yet. */
            continue;
        }
        const int n_now = (due < n_max) ? due : n_max;
        const int n_pri = (s_tt_n[i] > 0) ? s_tt_n[i] : 0;
        if (n_now == n_pri) continue;                     /* nothing new */

        line_type(fb, i, n_pri, n_now, s_sl_y[i]);
        s_tt_n[i] = (int16_t)n_now;
    }
    return finite;
}

/* --- THE ROLLBACK: the same teletype, running backwards --------------------
 *
 * Every line retreats towards its own start and the cursor goes back with it —
 * line_type needs no special case, it simply receives a position lower than the
 * previous one.
 *
 * SEVERAL LINES AT ONCE, staggered, exactly as the typing does: one line starts
 * every UNTYPE_DELAY frames while the ones before are still withdrawing, so at
 * the busiest moment about six are moving together. Retreating them strictly one
 * after another would read as a different machine from the one that wrote them.
 *
 * THE ORDER IS BY POSITION ON SCREEN, from the bottom up — NOT by index in SL.
 * The two differ: the table holds the five lines of the entry first and the six
 * firmware notes after, but the notes sit at the TOP beside the QR. Going by
 * index the rollback began up there, which is both wrong to look at and wrong
 * for the selfie, which needs the bottom cleared first.
 *
 * SPEED IS NOT TT_FRAMES. Two characters leave per frame, so the longest line —
 * 33 characters — clears in 17 frames and the whole rollback takes about 1.55 s:
 * the same scale as the typing, which is what makes it read as the same machine
 * running backwards rather than as a fast-forward.
 *
 * [D] THE SELFIE'S START IS TIED TO THESE TWO NUMBERS. The blocks must not land
 * on a line that has not withdrawn, but the constraint is looser than it looks:
 * the entry is right-aligned and the blocks come from the right, so the front
 * chases a retreating edge rather than meeting it. See the note on BLK_START
 * for the arithmetic. Making the rollback slower, or its stagger wider, eats
 * into that clearance.
 *
 * Returns true when nothing is left on screen. */
#define UNTYPE_SPEED  2          /* characters removed per frame              */
#define UNTYPE_DELAY  3          /* frames between one line and the next up   */

/* The QR fades WHILE the notes beside it withdraw, not before and not after.
 *
 * The notes are ranks 5 to 10, so the first of them starts at frame
 * 5*UNTYPE_DELAY = 15: the code begins to go at the same moment as the text
 * around it, and the whole upper right corner empties as one gesture.
 *
 * The fade lasts as long as the notes take to clear — 6 more ranks plus the
 * length of the longest, about 35 frames — so nothing is left glowing over an
 * area that is already bare. */
#define QR_FADE_FROM  (5 * UNTYPE_DELAY)   /* frame at which the fade starts */
#define QR_FADE_LEN   35                   /* frames from full to gone       */

static bool fx_untype(uint16_t *fb)
{
    s_tt_tick++;
    bool all_empty = true;

    for (int i = 0; i < SL_LINES; i++) {
        if (s_tt_n[i] <= 0) continue;                     /* already cleared */
        all_empty = false;

        /* Rank by vertical position: how many lines sit BELOW this one. The
         * lowest has rank 0 and starts at once. Computed here rather than kept
         * in a table because SL_LINES is eleven and the loop runs once per
         * frame — and because a table would be one more thing to keep in step
         * with the layout. */
        int rank = 0;
        for (int k = 0; k < SL_LINES; k++)
            if (s_sl_y[k] > s_sl_y[i]) rank++;

        if (s_tt_tick <= rank * UNTYPE_DELAY) continue;  /* not its turn yet */

        const int n_pri = s_tt_n[i];
        int n_now = n_pri - UNTYPE_SPEED;
        if (n_now < 0) n_now = 0;

        line_type(fb, i, n_pri, n_now, s_sl_y[i]);
        s_tt_n[i] = (int16_t)n_now;
    }
    return all_empty;
}




/* --- the task --------------------------------------------------------------
 *
 * IT HOLDS THE LVGL LOCK for its whole duration. That is not a detail: without
 * it the LVGL task would keep redrawing its own objects over ours, and the two
 * would contend for the panel.
 *
 * CONSEQUENCE: while the lock is held, the touch read callback — which lives on
 * the LVGL task — does NOT run. So we read touch ourselves through
 * bsp_touch_read(). It is simpler too: no need to go through the indev. */
static void fx_task(void *arg)
{
    (void)arg;
    uint16_t *fb = display_fb();
    if (!fb) { ESP_LOGE(TAG, "framebuffer not available"); goto out; }

    /* The texture may be missing: without it there is nothing to reveal, and
     * the screen stays on its background colour until the touch closes it. */
    bool use_tex = (s_tex != NULL);
    bool first    = true;

    /* --- THE STORYBOARD, in four acts --------------------------------------
     *
     *   TEXT      the QR is already there; after half a second the lines are
     *             typed, the entry first and the firmware notes after
     *   WAIT1     still: waiting for a touch on the glass
     *   ROLLBACK  three things at once — the text withdraws from the bottom up,
     *             the QR fades, and the portrait rises by blocks from the
     *             bottom right, in green
     *   PAINT     the portrait stays green until a finger brings the colour
     *             out of it; the circle, or the timeout, closes About
     *
     * The two automatic transitions — text finished, image finished — are not
     * on a timer: what is observed is that there is nothing left to animate.
     * Waiting a fixed number of seconds would mean guessing durations that
     * depend on parameters still being tuned.
     *
     * A TOUCH ON THE GLASS DOES NOTHING WHILE SOMETHING MOVES: a finger resting
     * on the screen would otherwise skip to the end of a story nobody asked to
     * skip. Both moving acts end by themselves, and the circle on the strip
     * below leaves at any moment — see the note on the touch further down. */
    enum { ST_TEXT = 0, ST_WAIT1, ST_ROLLBACK, ST_PAINT };
    int st = ST_TEXT;

    lvgl_port_lock(0);

    int      paint_prev_x = -1;                     /* last stamp, -1 = none */
    int      paint_prev_y = -1;
    int      qr_level = 255;                        /* QR level, 255 = full */
    uint16_t qr_col = mix565f(QR_BG, QR_FG, 255);   /* last colour rendered */

    int64_t t_wait = 0;              /* when the portrait was completed, 0 = not yet */
    int     t  = 0;
    bool    touch_was_down = true;   /* the touch that opened the intro does not count */
    bool    touch_pending  = false;  /* pressed: we act when it is released  */

    /* --- PER-PHASE MEASUREMENTS, not one average -------------------------
     *
     * An average over all frames said nothing useful: the intro sits still
     * until it is closed, and those frames — where fx_type returns at once,
     * drawing and sending nothing — were the great majority of the sample.
     * [M] 2984 us/f over 1804 frames mixed a few hundred frames of real work
     * with over fifteen hundred that did nothing, and neither number was
     * readable.
     *
     * Each phase now has its own counter, so a comparison is made against the
     * right phase. */
    enum { PH_TEXT = 0, PH_REVEAL, PH_IDLE, PH_N };
    int64_t ph_us[PH_N] = {0};
    int     ph_f[PH_N]  = {0};
    int     frames = 0;
    display_flush_stats_reset();

    while (!s_stop) {
        int64_t t_frame = esp_timer_get_time();
        int phase = PH_IDLE;      /* the drawing branch decides it */


        uint16_t tx, ty;
        bool down = bsp_touch_read(&tx, &ty);
        bool tap  = false;
        static uint16_t tap_y = 0;
        if (!down) {
            if (touch_pending) { tap = true; touch_pending = false; }
            touch_was_down = false;        /* lifted: touches count from here */
        } else if (!touch_was_down) {
            touch_pending = true;          /* pressed: waiting for the release */
            tap_y = ty;                    /* where it began, see ui.c         */
        }

        /* Handy names for the painting phase: it needs the position while
         * the finger is DOWN, not the tap that comes on release. */
        const bool paint_down = down;
        const int  paint_x    = (int)tx;
        const int  paint_y    = (int)ty;

        /* Without a texture there is nothing to show: the About screen
         * stays on its background colour until the touch closes it. */
        if (use_tex) {
            if (first) {
                /* Opens on the screen's BACKGROUND, not on the portrait: that
                 * arrives later, uncovered by the scan bar. One full transfer,
                 * once. */
                fill_rect(fb, 0, 0, FX_W, FX_H, FX_BG);
                /* The QR appears AT ONCE, at full level, in the same transfer
                 * as the background: it is the one element that does not
                 * animate, and a code that assembles itself is a code nobody
                 * can frame while it does. */
                fx_qr_level(fb, 255);
                display_flush_band(FX_Y, FX_H);
                slide_init();
                first = false;
            }
            switch (st) {
            case ST_TEXT: {
                /* --- TEXT AND QR TOGETHER -------------------------------
                 *
                 * The QR does not wait for the text to finish: the fade begins
                 * when the LAST LINE starts typing, that is, when all five are
                 * in motion. It is the moment the screen is most alive, and the
                 * code emerges underneath rather than arriving afterwards, once
                 * everything has stopped.
                 *
                 * The starting point is derived from the typing schedule rather
                 * than being a separate number: changing the speed or the
                 * stagger keeps it anchored to the same EVENT.
                 *
                 * [M] Together they cost about 1.2 ms for the text and 9.0 for
                 * the QR: some 10 out of the 33 available, so the overlap fits
                 * comfortably. */
                const bool text_done = fx_type(fb);

                if (!text_done) phase = PH_TEXT;
                else st = ST_WAIT1;
                break;
            }

            case ST_ROLLBACK: {
                /* THE THREE THINGS HAPPEN IN THE SAME FRAME, and that is the
                 * whole of this transition: the text withdraws, the QR fades,
                 * and the portrait rises to take their place. */
                const bool testo_via = fx_untype(fb);
                const bool img_fatta = fx_blocks(fb, s_tex, s_tt_tick);

                /* The QR goes with them. Only when the LEVEL really changes:
                 * neighbouring levels give the same RGB565, and redrawing then
                 * would cost 8 ms a frame to change no pixel — the same lesson
                 * the breathing taught. */
                const int f = s_tt_tick - QR_FADE_FROM;
                if (f >= 0 && qr_level > 0) {
                    int level = 255 - f * 255 / QR_FADE_LEN;
                    if (level < 0) level = 0;
                    const uint16_t col = mix565f(QR_BG, QR_FG, level);
                    if (col != qr_col) { fx_qr_level(fb, level); qr_col = col; }
                    qr_level = level;
                }

                /* The image is what decides: it starts last and finishes last,
                 * so waiting for the other two would be waiting for something
                 * already done. */
                (void)testo_via;
                if (img_fatta) {
                    st = ST_PAINT;
                    t_wait = esp_timer_get_time();   /* the timeout starts HERE */
                }
                phase = PH_REVEAL;
                break;
            }

            case ST_WAIT1:
                break;                       /* still: nothing is drawn */

            case ST_PAINT:
                /* The finger paints. Only while it is DOWN — a stroke is a
                 * drag, not a tap — and from the previous position, so quick
                 * movements leave a trail and not a row of discs. */
                if (paint_down) {
                    if (paint_prev_x >= 0)
                        brush_stroke(fb, s_tex, paint_prev_x, paint_prev_y, paint_x, paint_y);
                    else
                        brush_stamp(fb, s_tex, paint_x, paint_y);
                    paint_prev_x = paint_x; paint_prev_y = paint_y;
                    t_wait = esp_timer_get_time();   /* the timeout restarts */
                } else {
                    paint_prev_x = -1;               /* the stroke is over   */
                }
                phase = PH_IDLE;
                break;

            default:
                break;
            }
        }
        /* NO GENERAL TRANSFER HERE.
         *
         * Each effect sends the rectangles it dirtied, and a blanket transfer
         * at this point would undo all of that: [M] 61 ms for a full screen
         * against about one per small rectangle.
         *
         * Whoever draws knows what they dirtied, and is the only one who can
         * know it: the transfer belongs to the effect, not to the loop. */
        ph_us[phase] += esp_timer_get_time() - t_frame;
        ph_f[phase]++;
        frames++;

        /* --- THE TOUCH, and WHERE it lands ---------------------------------
         *
         * TWO SURFACES, two meanings. The glass shows the story; the
         * silkscreened strip below it (y >= 248) is the device's own control,
         * and its centre circle means "leave" everywhere else in the interface.
         * Here it means the same.
         *
         * THE GLASS IS DEAF WHILE SOMETHING IS MOVING. Letting a touch complete
         * the typing sounds helpful and is not: the screen is a short read that
         * ends by itself, and a finger resting on it — or a stray contact —
         * skipped to the end of something nobody had asked to skip. So during
         * TEXT and during ROLLBACK the glass does nothing at all, and the way
         * out is the circle.
         *
         * IN WAIT1 the glass advances the story, starting the rollback. In
         * PAINT it paints, so lifting the finger ends a stroke and nothing
         * more: from there only the circle or the timeout gets out.
         *
         * ON RELEASE, not on press, as everywhere else: leaving on the press
         * meant the release landed on the screen underneath and was read as a
         * touch of its own. The position remembered is the one of the PRESS —
         * during the confirming reads the finger may already have moved. */

        if (tap) {
            /* LEAVING MAKES NO SOUND OF ITS OWN. What follows is a nav_pop —
             * the callback below ends the intro and the settings go back a
             * level — and that already plays the navigation beep. Sounding
             * here as well announces one event twice, and since the two clips
             * differ in atomicity the second cuts the first off partway: [M]
             * beep_down runs 162 ms and is not atomic, so a nav_beep arriving
             * inside that window replaces it rather than waiting. */

            /* The circle always leaves, whatever is on screen. */
            if (tap_y >= 248) goto done;

            /* Without a texture there is nothing to reveal: the glass closes. */
            if (!use_tex) goto done;

            switch (st) {
            case ST_TEXT:
            case ST_ROLLBACK:
                break;                     /* deaf: it finishes by itself */

            case ST_WAIT1:
                /* The text withdraws before the image arrives: the rollback
                 * clears the lower half, and the portrait follows it up. */
                audio_play(AUDIO_NAV_BEEP);
                s_tt_tick = 0;
                s_blk_n   = 0;
                st = ST_ROLLBACK;
                break;

            case ST_PAINT:
            default:
                /* The glass PAINTS here: lifting the finger ends a stroke, it
                 * does not mean "leave". Only the circle, or the timeout, gets
                 * out of this last act — otherwise the first brush stroke would
                 * close the screen it had just started to colour. */
                break;
            }
        }

        /* The timeout only applies once the portrait is up, and it is counted
         * FROM THAT MOMENT — not from entering the screen.
         *
         * Measuring from the start of the screen meant the reading time counted
         * against it: lingering on the QR used the whole allowance, and the
         * portrait appeared only to vanish at once. The safety exit exists
         * for a device left on a shelf, and what it must measure is how long the
         * LAST image has been sitting there. */
        if (st == ST_PAINT && (esp_timer_get_time() - t_wait) / 1000 > FX_MAX_MS)
            goto done;


        t++;
        int64_t used = (esp_timer_get_time() - t_frame) / 1000;
        /* The painting phase runs at twice the rate: the touch has a 15 ms
         * cache, so at 33 ms we were asking for half the positions the sensor
         * could give, and a brush at 30 Hz leaves a broken trail. A stamp
         * costs 1.8 ms, well inside the halved budget. */
        const int period = (st == ST_PAINT) ? FX_FPS_MS / 2 : FX_FPS_MS;
        int wait = period - (int)used;
        vTaskDelay(pdMS_TO_TICKS(wait > 1 ? wait : 1));
    }
done:

#if DJB_DEVELOPER
    /* THE INTRO'S MEASUREMENTS: microseconds per frame, how much time went
     * into the transfer. They are instruments, not logs: they existed to tell
     * whether the animation fitted its budget, and in production they are
     * five lines on the serial port that nobody reads. */
    if (frames) {
        static const char *const PH_NAME[PH_N] = { "text", "reveal", "still" };
        for (int i = 0; i < PH_N; i++)
            if (ph_f[i])
                ESP_LOGW(TAG, "about | %-11s %4d f | %5lld us/f out of %d available",
                         PH_NAME[i], ph_f[i],
                         ph_us[i] / ph_f[i], FX_FPS_MS * 1000);

/* The transfer, measured where it happens: taken anywhere else the time lands
 * inside the drawing figure instead, and this column reads zero. */
        uint32_t fn = 0; int64_t ftot = 0, fwait = 0;
        display_flush_stats(&fn, &ftot, &fwait);
        if (fn)
            ESP_LOGW(TAG, "transfer | %u calls | %lld us total | %lld us "
                          "(%lld%%) waiting for the quiet window",
                     fn, ftot, fwait, ftot ? fwait * 100 / ftot : 0);
    }
#endif

    /* The masks are released HERE, with the lock still held and before any
     * exit: they are the only allocation the intro carries. */
    slide_free();

    lvgl_port_unlock();

out:
    s_run = false;
    if (s_done_cb) s_done_cb();
    vTaskDelete(NULL);
}

bool about_start(about_done_fn on_done)
{
    if (s_run) return false;

    static bool tables = false;
    if (!tables) { green_lut_build(); brush_build(); tables = true; }

    s_done_cb = on_done;
    s_stop = false;
    s_run  = true;

    if (xTaskCreatePinnedToCore(fx_task, "about", FX_TASK_STACK, NULL,
                                FX_TASK_PRIO, NULL, FX_TASK_CORE) != pdPASS) {
        s_run = false;
        ESP_LOGE(TAG, "task not created");
        return false;
    }
    return true;
}

bool about_running(void) { return s_run; }
void about_stop(void)    { s_stop = true; }
