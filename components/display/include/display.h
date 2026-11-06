/*
 * display.h — CoreS3 ILI9342C panel over esp_lcd, on the SHARED SPI bus
 * created by bsp_spi_bus_init().
 *
 * THE PIN THAT COMPLICATES EVERYTHING: the panel's DC line sits on GPIO35,
 * which is also the bus MISO. Reconciling who owns that pin and when is the
 * central problem of this component — see the notes in display.c and
 * bsp_cores3.h.
 *
 * Three ways of drawing text live here, in increasing order of quality and
 * cost: a fixed 8x8 font, the same font advanced proportionally, and LVGL's
 * antialiased fonts. They coexist because the cheapest one is still the right
 * answer for a debug overlay.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Handy RGB565 colours. */
#define RGB565(r,g,b) ((uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)))
#define COL_BLACK  0x0000
#define COL_WHITE  0xFFFF
#define COL_GREEN  0x07E0
#define COL_RED    0xF800
#define COL_AMBER  0xFD20
#define COL_CYAN   0x07FF
#define COL_GREY   0x8410

/* Create panel IO and the ILI9342 panel on the shared bus, and turn it on. */
/* The framebuffer as it is right now: RGB565, FB_W x FB_H, row by row.
 *
 * READ ONLY. Everything drawn passes through it before reaching the panel, so
 * it is what is on the glass — but it changes under the reader at every redraw,
 * and the bytes are in the PANEL's order, big-endian. */
const uint16_t *display_framebuffer(void);

#if DJB_DEVELOPER
/* Puts a flushed area into the framebuffer, so that display_framebuffer() has
 * the whole screen and not just what the About screen drew.
 *
 * Pixels must ALREADY be big-endian, the panel's order — which is what a
 * flush hands over after its own swap. Developer builds only. */
void display_capture_area(int x, int y, int w, int h, const uint16_t *px);
#endif


esp_err_t display_init(void);

/* Push the whole framebuffer to the panel in an SPI burst. Call after drawing
 * with fill/text/printf to make the changes visible. */
void display_flush(void);

/* Push only a horizontal band: y0 is the first row, rows how many. Cost is
 * proportional to the area, so this is far cheaper than a full flush — see
 * the note in display.c. */
void display_flush_band(int y0, int rows);

/* Push a RECTANGLE of the framebuffer rather than a whole band. For effects
 * that move a few small objects: it costs what the rectangle costs, not what
 * the screen costs. */
void display_flush_rect(int x0, int y0, int w, int h);

/* The framebuffer itself, 320x240 RGB565, for drawing pixels without LVGL. */
uint16_t *display_fb(void);

/* Fill the whole screen with one colour. */
void display_fill(uint16_t color);

/* Convert a standard RGB565 value into the framebuffer's format.
 *
 * The framebuffer does NOT hold standard RGB565: it holds what the panel
 * wants, which is the same bits with the two BYTES SWAPPED. Nothing else — the
 * red/blue ordering is handled by the panel, not by us. Anyone writing their
 * own colours, or copying pixels from an image generated for LVGL, must go
 * through here or the colours come out wrong. */
uint16_t display_rgb565_to_fb(uint16_t rgb565);

/* --- fixed 8x8 text -------------------------------------------------------- */

/* Draw scaled 8x8 text (scale >= 1). Pass bg == fg for a transparent
 * background. */
void display_text(int x, int y, const char *s, uint16_t fg, uint16_t bg, int scale);

/* Convenience printf, up to 128 characters per line. */
void display_printf(int x, int y, uint16_t fg, uint16_t bg, int scale,
                    const char *fmt, ...) __attribute__((format(printf, 6, 7)));

/* --- proportional 8x8 text ------------------------------------------------- */

/* TRANSPARENT and PROPORTIONAL text: lights only the glyph's own pixels, and
 * advances by each glyph's REAL width instead of the font's fixed eight
 * columns. 'gap' is the spacing between characters, in columns (1 or 2).
 * See the extended note in display.c. */
void display_text_tr(int x, int y, const char *s, uint16_t fg, int scale, int gap);

/* Width the same string would occupy, for centring or for knowing how much to
 * redraw. */
int  display_text_tr_width(const char *s, int scale, int gap);

/* --- LVGL fonts (Montserrat) ----------------------------------------------
 *
 * Proportional and ANTIALIASED, blended with whatever is already in the
 * framebuffer. Usable while drawing outside LVGL, as long as its lock is
 * held. Strings are UTF-8. See the extended note in display.c. */
struct _lv_font_t;

/* Width of the string in pixels, kerning included. */
int  display_font_width(const struct _lv_font_t *font, const char *s);

/* Height of one line of text in this font. */
int  display_font_height(const struct _lv_font_t *font);

/* STATIC text: rasterise and draw in one go. (x, y) is the top-left corner of
 * the LINE, not of the first letter. */
void display_font_text(const struct _lv_font_t *font, int x, int y,
                       const char *s, uint16_t fg);

/* --- text that MOVES -------------------------------------------------------
 *
 * Rasterising glyphs on every frame repeats work whose result never changes.
 * A mask does it ONCE; afterwards each frame costs only the blend with the
 * background.
 *
 * A single allocation backs the whole thing: the pixels and the per-row
 * ranges live and die together. Release with display_text_mask_free(), never
 * with free(). */
typedef struct {
    uint8_t  *a;        /* per-pixel opacity, already scaled to 0..32, w*h */
    int16_t   w, h;
    uint16_t *row_x0;   /* first non-empty column of each row */
    uint16_t *row_x1;   /* one past the last                  */

    /* INK box: the pixels actually written, within w x h.
     *
     * It matters to whoever pushes to the panel, which is the dominant cost
     * ([M] about 0.9 us per pixel). A line of text is as wide as its advance
     * and as tall as the font's line height, but the ink occupies less: only
     * accents reach the top, only descenders the bottom, and the final
     * advance is never drawn. Pushing w x h pushes unchanged background.
     *
     * Empty when ink_x1 <= ink_x0. */
    int16_t   ink_x0, ink_x1;   /* [x0, x1) */
    int16_t   ink_y0, ink_y1;   /* [y0, y1) */

    /* Where each character ENDS, in mask columns.
     *
     * The mask is one block of pixels and no longer knows where a letter
     * began: revealing it a piece at a time — a teletype, a karaoke line, an
     * advancing underline — needs to know where to cut. Character i occupies
     * columns [adv[i-1], adv[i]), with adv[-1] = 0. These are the very
     * positions the drawing used, so a cut always falls between two letters
     * and never inside one.
     *
     * Spaces count as characters: they advance without leaving ink, and a
     * progressive reveal should pause on them too. */
    uint16_t *adv;
    int16_t   n_char;
} display_text_mask_t;

bool display_text_mask_build(const struct _lv_font_t *font, const char *s,
                             display_text_mask_t *m);
void display_text_mask_free(display_text_mask_t *m);
void display_text_mask_draw(const display_text_mask_t *m, int x, int y,
                            uint16_t fg);

/* As above, but draws only columns [col0, col1) of the mask. Combined with
 * m->adv this yields "the first N characters" without rasterising anything
 * new. */
void display_text_mask_draw_span(const display_text_mask_t *m, int x, int y,
                                 uint16_t fg, int col0, int col1);

/* Release the glyph conversion buffer (2.3 KB of INTERNAL RAM). Call after
 * building the masks, once nothing rasterises any more. Optional: the buffer
 * is recreated on the next glyph. */
void display_font_cache_release(void);

/* --- panel handles and orientation ----------------------------------------- */

/* esp_lcd panel and IO handles, for wiring LVGL through esp_lvgl_port. */
esp_lcd_panel_handle_t    display_get_panel(void);
esp_lcd_panel_io_handle_t display_get_io(void);

/* Rotate the image by 180 degrees. The panel controller does it, so it costs
 * nothing at runtime. It does NOT affect touch: bsp_touch_read() mirrors that
 * separately, and the two must be kept in step. */
void display_set_flipped(bool on);
bool display_flipped(void);

/* Call from the end-of-DMA ISR when LVGL has replaced the panel callback:
 * it unblocks display_flush_band(). See the note in display.c. */
void display_trans_done_isr(BaseType_t *hp);

/* --- transfer accounting ---------------------------------------------------
 *
 * Measures time spent INSIDE the push functions, separating the transfer
 * itself from waiting on the quiet window around the beat. The two have
 * different causes and different remedies: the transfer scales with area and
 * shrinks by dirtying less, the wait does not scale at all and depends on
 * where the frame falls relative to the beat. Covers display_flush_rect() and
 * display_flush_band(). */
void display_flush_stats_reset(void);
void display_flush_stats(uint32_t *n, int64_t *total_us, int64_t *wait_us);

#ifdef __cplusplus
}
#endif
