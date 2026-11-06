/*
 * display.c — CoreS3 ILI9342C panel over esp_lcd, with a FRAMEBUFFER.
 *
 * A "virtual display" arrangement: drawing always happens into a framebuffer in
 * PSRAM (fill and text are RAM writes, no SPI at all), and display_flush()
 * pushes it to the panel through a bounce buffer in internal RAM, band by band,
 * synchronising on the on_color_trans_done semaphore.
 *
 * Two things follow from that. There is no malloc/free race against the DMA —
 * the source of the "corruption" that used to appear — and a flush becomes an
 * atomic SPI burst with clean boundaries, which is what makes coexistence with
 * the W5500 on the same bus possible at all.
 */
#include "display.h"
#include "bsp_cores3.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_ili9341.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "font8x8_basic.h"
#include "lvgl.h"

static const char *TAG = "display";

/* --- Orientation --- */
#define LCD_SWAP_XY   false
#define LCD_MIRROR_X  false
#define LCD_MIRROR_Y  false

#define LCD_INVERT    true

#define FB_W       BSP_LCD_H_RES   /* 320 */
#define FB_H       BSP_LCD_V_RES   /* 240 */
#define BAND_ROWS  24              /* rows per flush band */

static esp_lcd_panel_handle_t s_panel;

/* 180 degree rotation, that is mirroring on BOTH axes.
 *
 * The panel CONTROLLER does it, by rewriting its scan register: zero runtime
 * cost, no pixel touched by the CPU, no impact on the SPI bus we share with the
 * beat. Doing it in LVGL would have meant transforming every pixel on every
 * flush, on core 1, which is already the busier one.
 *
 * MIND THIS: it rotates the IMAGE only. Touch must be mirrored separately, and
 * bsp_touch_read() does it. Rotating one without the other produces an
 * interface that responds at the point opposite the one touched. */
static bool s_flipped = false;

void display_set_flipped(bool on)
{
    if (!s_panel || on == s_flipped) return;
    s_flipped = on;
    esp_lcd_panel_mirror(s_panel, LCD_MIRROR_X ^ on, LCD_MIRROR_Y ^ on);
}

bool display_flipped(void) { return s_flipped; }

static esp_lcd_panel_io_handle_t s_io;
static uint16_t *s_fb;                                    /* framebuffer, in PSRAM */
static uint16_t  s_bounce[FB_W * BAND_ROWS] __attribute__((aligned(4))); /* internal RAM, for DMA */
static SemaphoreHandle_t s_done;

/* The panel wants the bytes BIG-ENDIAN, and nothing else.
 *
 * [M] Measured, not deduced: three bands of pure red were drawn in three
 * different encodings, and we looked at which one came out red. 0x00F8 wins —
 * that is 0xF800 with the two bytes swapped.
 *
 * THE BYTE SWAP IS THE ONLY TRANSFORMATION: no red/blue exchange on top of it.
 * An extra swap would go unnoticed for a long time — display_fill is used
 * almost only with BLACK, which is invariant under any swap, and random colours
 * do not show one either. */
static inline uint16_t sw(uint16_t c) { return __builtin_bswap16(c); }

/* The same conversion, EXPOSED to anyone drawing straight into the framebuffer.
 *
 * It is needed because the framebuffer does NOT hold standard RGB565: it holds
 * what the panel wants, the same bits with the two BYTES SWAPPED, and nothing
 * else. Anyone writing their own colours — or copying pixels from an image
 * generated for LVGL — must come through here, or the colours come out wrong.
 *
 * [M] Which is exactly what happened when sampling an image inside an effect:
 * the image is in LVGL's format, and LVGL performs the swap at flush time.
 * Copying it verbatim into the framebuffer produced a psychedelic mess instead
 * of a picture. */
uint16_t display_rgb565_to_fb(uint16_t rgb565) { return sw(rgb565); }

static bool on_trans_done(esp_lcd_panel_io_handle_t io,
                          esp_lcd_panel_io_event_data_t *e, void *ctx)
{
    (void)io; (void)e; (void)ctx;
    BaseType_t hp = pdFALSE;
    /* THE LOCK IS RELEASED HERE, at the end of the DMA — not after the
     * semaphore in the caller. The DMA is asynchronous: releasing earlier would
     * hand the bus to somebody else while the transfer is still running.
     *
     * on_flush_done does the same on the LVGL path: if the bus was never taken,
     * bsp_spi_unlock_isr does nothing. */
    bsp_spi_unlock_isr(&hp);
    xSemaphoreGiveFromISR(s_done, &hp);
    return hp == pdTRUE;
}

/* Wake whoever is waiting in display_flush_band().
 *
 * NEEDED BECAUSE LVGL REPLACES THE CALLBACK. ui_init registers its own
 * on_flush_done on the same panel IO, and from that moment on_trans_done above
 * is never called again. Without this hook display_flush_band would block on
 * its semaphore forever — and the direct drawing of the About screen would not
 * start at all.
 *
 * To be called FROM the end-of-transfer ISR, whichever one that is. */
void display_trans_done_isr(BaseType_t *hp)
{
    if (s_done) xSemaphoreGiveFromISR(s_done, hp);
}

#if DJB_DEVELOPER
/* Copies a flushed area into the framebuffer, so a screenshot can find it.
 *
 * WHY THIS IS NEEDED AT ALL: LVGL hands its own buffers straight to the panel
 * and never touches s_fb, which serves only the primitives here — the text and
 * images of the About screen. Read without this, s_fb holds whatever About left
 * behind, which on a fresh boot is nothing: [M] the first screenshots came out
 * black.
 *
 * THE PIXELS ARRIVE ALREADY SWAPPED, big-endian, because the caller does that
 * before taking the bus. That is the same order s_fb uses, so this is a plain
 * copy — no conversion, no bus, just memory.
 *
 * Developer builds only: in a normal one nothing reads s_fb outside About, and
 * a memcpy per flush would be paid for nothing. */
void display_capture_area(int x, int y, int w, int h, const uint16_t *px)
{
    if (!s_fb || !px) return;
    if (x < 0 || y < 0 || x + w > FB_W || y + h > FB_H) return;

    for (int r = 0; r < h; r++)
        memcpy(&s_fb[(size_t)(y + r) * FB_W + x],
               &px[(size_t)r * w],
               (size_t)w * sizeof(uint16_t));
}
#endif


/* The framebuffer, for whoever wants to save a picture of the screen.
 *
 * IT IS THE REAL THING, not a copy: everything drawn goes through here before
 * reaching the panel, so what it holds is exactly what is on the glass. The
 * caller must not write to it, and must not hold the pointer across a redraw
 * expecting the contents to stand still.
 *
 * Format is RGB565, ROW BY ROW, FB_W pixels each — and BIG-ENDIAN, which is the
 * panel's order and not the one a file will want (see display.c's note on the
 * byte swap: it was determined by drawing three bands of red, not by
 * assumption). */
const uint16_t *display_framebuffer(void)
{
    return s_fb;
}


esp_err_t display_init(void)
{
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num   = BSP_LCD_CS_GPIO,
        .dc_gpio_num   = BSP_LCD_DC_GPIO,   /* == 35 == MISO: conflict handled in M0c-2 */
        .spi_mode      = 0,
        .pclk_hz       = BSP_LCD_SPI_HZ,    /* 20 MHz (bsp) */
        .trans_queue_depth = 10,
        .lcd_cmd_bits  = 8,
        .lcd_param_bits = 8,
        .on_color_trans_done = on_trans_done,
    };
    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)BSP_SPI_HOST, &io_cfg, &s_io),
        TAG, "panel io");

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = -1,   /* reset via AW9523 in bsp_power_init */
        .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_BGR,  /* LVGL does the byteswap; the panel does the R/B */
        .bits_per_pixel = 16,
    };
    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_ili9341(s_io, &panel_cfg, &s_panel), TAG, "panel");

    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    esp_lcd_panel_invert_color(s_panel, LCD_INVERT);
    esp_lcd_panel_swap_xy(s_panel, LCD_SWAP_XY);
    esp_lcd_panel_mirror(s_panel, LCD_MIRROR_X, LCD_MIRROR_Y);

    /* Orientation: see display_set_flipped(). We ALWAYS start upright here. */
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));

    s_done = xSemaphoreCreateBinary();
    s_fb = heap_caps_malloc((size_t)FB_W * FB_H * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    if (!s_fb) { ESP_LOGE(TAG, "PSRAM framebuffer not allocated"); return ESP_ERR_NO_MEM; }

    ESP_LOGI(TAG, "display init ok (%dx%d framebuffer in PSRAM)", FB_W, FB_H);
    display_fill(COL_BLACK);
    display_flush();
    return ESP_OK;
}

/* ---- drawing into the framebuffer (no SPI involved) ---- */
void display_fill(uint16_t color)
{
    uint16_t v = sw(color);
    for (int i = 0; i < FB_W * FB_H; i++) s_fb[i] = v;
}

/* --- TEXT WITH LVGL FONTS ---------------------------------------------------
 *
 * BEWARE THE PLAUSIBLE EXPLANATION. "The Montserrat fonts are compressed and
 * LV_USE_FONT_COMPRESSED is off" sounds like a diagnosis and is a deduction:
 * regenerating the font without compression yields a font identical to the one
 * already there, because these fonts are not compressed to begin with.
 *
 * FOUR FACTS, read in the LVGL 9.5.0 sources — the version in
 * dependencies.lock — rather than deduced:
 *
 * 1. THE FONTS ARE NOT COMPRESSED.
 *      lv_font_montserrat_20.c           .bitmap_format = 0   (PLAIN)
 *      lv_font_montserrat_28_compressed  .bitmap_format = 1
 *    A separate file exists for the compressed variant precisely because the
 *    normal one is not. LV_USE_FONT_COMPRESSED has nothing to do with it.
 *
 * 2. THE FUNCTION RETURNS THE STRUCT, NOT THE PIXELS. In lv_font_fmt_txt.c the
 *    PLAIN branch converts to A8, writes into draw_buf->data, and then:
 *
 *        return draw_buf;          <-- lv_draw_buf_t *, not ->data
 *
 *    and that struct begins with { lv_image_header_t; uint32_t; uint8_t *; }.
 *    Reading the return value as a bitmap meant reading magic, format, width,
 *    height and the pointer itself, all taken for opacity values. Real DRAM,
 *    not null, and not recognisable as a glyph under any interpretation of the
 *    bits — exactly the symptom that had been observed.
 *
 * 3. g.format IS THE BPP OF THE FONT IN FLASH, NOT OF THE RETURNED BUFFER.
 *    lv_font_get_glyph_dsc_fmt_txt sets "dsc_out->format = fdsc->bpp" first,
 *    and get_glyph_bitmap never touches it. The fmt=4 measured after the call
 *    was correct and told the truth — about the data in flash. What comes back
 *    is already A8, one byte per pixel. Reading nibbles, introduced after that
 *    measurement, was a second error stacked on the first.
 *
 * 4. lv_font_get_glyph_static_bitmap returned NULL for a reason of its own:
 *
 *        if(font_p->static_bitmap == 0) { LV_LOG_WARN(...); return NULL; }
 *
 *    which is a flag of the font, not "Montserrat does not expose raw data".
 *    That path is not worth taking anyway: in the PLAIN format without stride
 *    the nibbles run CONTINUOUSLY across rows, without realigning to a byte at
 *    each row. Reading them row by row would give the unreadable interleaving
 *    the old comment feared.
 *
 * So: call lv_font_get_glyph_bitmap with a draw_buf, read draw_buf->data, and
 * the data is A8 with an A8 stride over box_w. Three lines.
 *
 * --- HOW IT IS USED HERE, which decides how it is written -------------------
 *
 * The need is not merely "draw some text": the About intro brings lines in at
 * thirty frames per second, over an image. Redoing the cmap lookup, the kerning
 * and the conversion of every glyph on every frame means repeating thirty times
 * a second work whose result never changes.
 *
 * Hence two distinct paths:
 *
 *   display_font_text()   static text, drawn once. Simple.
 *   display_text_mask_*() the text is rasterised ONCE into an opacity mask, and
 *                         each frame costs only the blend. This is the path for
 *                         animation.
 *
 * --- THE BLEND, which is the dominant cost ----------------------------------
 *
 * An antialiased pixel must be MIXED with the background. The naive version
 * does six multiplications and three divisions by 255, per pixel. Over a line
 * of text 250 wide and 22 tall, three lines, thirty frames a second, those
 * numbers make themselves felt — and every read of the background is a PSRAM
 * access.
 *
 * Instead the blend is PACKED: the three channels of an RGB565 are spread
 * across a 32-bit integer so that they stay separated by enough empty space not
 * to spill into one another,
 *
 *      RGB565  RRRRRGGG GGGBBBBB
 *      packed  (c | c<<16) & 0x07E0F81F   ->  B at 0..4, R at 11..15,
 *                                             G at 21..26
 *
 * and all three are interpolated together with TWO multiplications and a shift.
 * Opacity is carried as 0..32 — five bits plus the full value — so the final
 * division is a shift.
 *
 * A NOTE ON THE FORM, and on an error avoided by a hair. It had been written
 * here that the compact form
 *
 *      bgx + (((fgx - bgx) * a) >> 5)
 *
 * was wrong, because with a foreground darker than the background the
 * subtraction goes below zero and the borrow propagates into the neighbouring
 * channel. That was a plausible, unverified deduction — precisely the vice this
 * revision was correcting elsewhere. [M] Measured, it is FALSE:
 *
 *   - 47.5 million comparisons between the two forms on the worst cases
 *     (foreground at its minima against background at its maxima and the
 *     reverse, over all 65536 backgrounds and all 33 opacity levels): ZERO
 *     differences;
 *   - the borrow does occur, but the bits it dirties land in 0x08000000, and
 *     PK_MASK is 0x07E0F81F: the intersection is empty, and the final mask
 *     clears them all.
 *
 * So the short form is used, at one multiplication instead of two. The headroom
 * is verified too: the highest channel is G at 0x07E00000, which times 32 gives
 * 0xFC000000 and still fits in 32 bits.
 *
 * TWO SHORTCUTS that remove work where there is most of it:
 *   - opacity 0   nothing is touched, and those are the majority of pixels in a
 *                 line of text;
 *   - opacity 32  the already converted colour is written, with no PSRAM read
 *                 of the background and no blending. In the solid body of the
 *                 letters that is the normal case.
 *
 * Edge clipping is computed ONCE per row as a range of visible columns, rather
 * than with two comparisons per pixel: while a line slides in from the left,
 * much of it is off screen.
 *
 * WORK HAPPENS IN STANDARD RGB565 and conversion comes only at the end: the
 * framebuffer wants the bytes swapped, and mixing in that space would give
 * meaningless results.
 *
 * UTF-8: LVGL's fonts are indexed by code point, not by byte. Feeding bytes one
 * at a time works for ASCII and is wrong for everything else — accents
 * included, and the phonetic alphabet the dictionary entry needs is entirely
 * outside ASCII. The decoder is below: LVGL's own lives in a private header and
 * cannot be included from an external component. */

/* Next code point. '*i' advances; at the end of the string it returns 0.
 * Malformed bytes are delivered as they are: better a wrong glyph than a loop
 * that fails to advance. */
static uint32_t utf8_next(const char *s, int *i)
{
    uint8_t c = (uint8_t)s[*i];
    if (!c) return 0;
    if (c < 0x80) { (*i)++; return c; }

    int n = (c >= 0xF0) ? 3 : (c >= 0xE0) ? 2 : (c >= 0xC0) ? 1 : 0;
    if (!n) { (*i)++; return c; }                    /* lone continuation byte */

    uint32_t cp = c & (0x3F >> n);
    for (int k = 1; k <= n; k++) {
        uint8_t cc = (uint8_t)s[*i + k];
        if ((cc & 0xC0) != 0x80) { (*i)++; return c; }   /* truncated */
        cp = (cp << 6) | (cc & 0x3F);
    }
    *i += n + 1;
    return cp;
}

/* --- packed blending --- */
#define PK_MASK 0x07E0F81Fu

static inline uint32_t pk_expand(uint16_t c)
{
    return ((uint32_t)c | ((uint32_t)c << 16)) & PK_MASK;
}

/* fgx already packed, a in 0..32. Returns standard RGB565. */
static inline uint16_t pk_blend(uint16_t bg, uint32_t fgx, uint32_t a)
{
    uint32_t bgx = pk_expand(bg);
    uint32_t o   = (bgx + (((fgx - bgx) * a) >> 5)) & PK_MASK;
    return (uint16_t)(o | (o >> 16));
}

/* Opacity from 0..255 to 0..32. The font is four bits, so the real values are
 * the sixteen multiples of 17: 255 lands exactly on 32 and 0 on 0, which are
 * precisely the two cases that have a shortcut. */
static inline uint32_t a8_to_a32(uint8_t a) { return ((uint32_t)a + 4u) >> 3; }

/* --- rasterising one glyph -------------------------------------------------
 *
 * The conversion buffer is created on the first letter and never released: it
 * is needed by every glyph of every draw. 48x48 in A8 is 2.3 KB and covers
 * Montserrat 20 with room to spare; beyond that size the glyph is SKIPPED
 * rather than written past the end of the buffer, which is the quiet way to
 * corrupt the heap. It lives in internal RAM: it is small, and it is read one
 * byte at a time. */
#define GLYPH_BUF_DIM 48
static lv_draw_buf_t *s_glyph_buf = NULL;

/* Rasterise the glyph described by 'g' and return a pointer to its A8 pixels,
 * with the stride in '*stride'. NULL when there is nothing to draw.
 * The caller remains responsible for lv_font_glyph_release_draw_data(). */
static const uint8_t *glyph_a8(lv_font_glyph_dsc_t *g, uint32_t *stride)
{
    if (g->box_w <= 0 || g->box_h <= 0) return NULL;          /* a space */
    if (g->box_w > GLYPH_BUF_DIM || g->box_h > GLYPH_BUF_DIM) return NULL;

    if (!s_glyph_buf) {
        s_glyph_buf = lv_draw_buf_create(GLYPH_BUF_DIM, GLYPH_BUF_DIM,
                                         LV_COLOR_FORMAT_A8, 0);
        if (!s_glyph_buf) return NULL;
    }
    if (!lv_font_get_glyph_bitmap(g, s_glyph_buf)) return NULL;

    /* THE STRIDE COMES FROM LVGL, and must be asked for box_w in A8: it is
     * exactly the calculation the function used to write. Deriving it by hand
     * from the arithmetic works as long as LV_DRAW_BUF_STRIDE_ALIGN is 1, and
     * stops working silently the day somebody changes it. */
    *stride = lv_draw_buf_width_to_stride(g->box_w, LV_COLOR_FORMAT_A8);
    return s_glyph_buf->data;
}

/* Font baseline, measured from the top of the text line. */
static inline int font_baseline(const lv_font_t *font)
{
    return font->line_height - font->base_line;
}

/* Release the glyph conversion buffer.
 *
 * It is only needed while rasterising. Code that builds masks and then animates
 * those has no further use for it, and it is 2.3 KB of INTERNAL RAM — the
 * scarce resource in this project, the one that drops to 13 KB with an Ableton
 * Link peer connected. Calling this is optional and breaks nothing: the buffer
 * recreates itself on the next glyph. */
void display_font_cache_release(void)
{
    if (!s_glyph_buf) return;
    lv_draw_buf_destroy(s_glyph_buf);
    s_glyph_buf = NULL;
}

/* Width in pixels the string would occupy in this font. */
int display_font_width(const lv_font_t *font, const char *s)
{
    if (!font || !s) return 0;
    int w = 0, i = 0;
    uint32_t cp = utf8_next(s, &i);
    while (cp) {
        int j = i;
        uint32_t next = utf8_next(s, &j);           /* needed for kerning */
        lv_font_glyph_dsc_t g;
        if (lv_font_get_glyph_dsc(font, &g, cp, next)) {
            w += g.adv_w;
            lv_font_glyph_release_draw_data(&g);
        }
        cp = next; i = j;
    }
    return w;
}

int display_font_height(const lv_font_t *font)
{
    return font ? font->line_height : 0;
}

/* Draw a string into the framebuffer, blending it with what is already there.
 * (x, y) is the top-left corner of the LINE, not of the first letter.
 *
 * This is the path for STATIC text. Moving text uses the mask below: same
 * result, without redoing the rasterisation on every frame. */
void display_font_text(const lv_font_t *font, int x, int y,
                       const char *s, uint16_t fg)
{
    if (!font || !s || !s_fb) return;

    const uint32_t fgx   = pk_expand(fg);
    const uint16_t fg_sw = sw(fg);
    const int base = font_baseline(font);

    int i = 0;
    uint32_t cp = utf8_next(s, &i);
    while (cp) {
        int j = i;
        uint32_t next = utf8_next(s, &j);
        lv_font_glyph_dsc_t g;

        if (!lv_font_get_glyph_dsc(font, &g, cp, next)) { cp = next; i = j; continue; }

        uint32_t stride = 0;
        const uint8_t *bm = glyph_a8(&g, &stride);
        if (bm) {
            /* ofs_y is measured from the text's BASELINE upwards. */
            const int gx0 = x + g.ofs_x;
            const int gy0 = y + base - g.box_h - g.ofs_y;

            /* Clipping computed once, not per pixel. */
            int c0 = (gx0 < 0) ? -gx0 : 0;
            int c1 = g.box_w;
            if (gx0 + c1 > FB_W) c1 = FB_W - gx0;
            int r0 = (gy0 < 0) ? -gy0 : 0;
            int r1 = g.box_h;
            if (gy0 + r1 > FB_H) r1 = FB_H - gy0;

            for (int r = r0; r < r1; r++) {
                const uint8_t *src = &bm[(size_t)r * stride];
                uint16_t *dst = &s_fb[(size_t)(gy0 + r) * FB_W + gx0];
                for (int c = c0; c < c1; c++) {
                    uint32_t a = a8_to_a32(src[c]);
                    if (!a) continue;
                    if (a >= 32u) { dst[c] = fg_sw; continue; }
                    dst[c] = sw(pk_blend(__builtin_bswap16(dst[c]), fgx, a));
                }
            }
        }
        lv_font_glyph_release_draw_data(&g);
        x += g.adv_w;
        cp = next; i = j;
    }
}

/* --- PRE-RASTERISED TEXT MASK ----------------------------------------------
 *
 * A line of text rasterised once: one opacity byte per pixel, already scaled to
 * 0..32 so that not even that conversion remains at draw time.
 *
 * IT LIVES IN PSRAM. The CPU reads it a byte at a time — it is not a DMA buffer
 * like LVGL's, which must sit in internal RAM. Internal RAM is the scarce
 * resource in this project (13 KB free with an Ableton Link peer connected) and
 * a line 250 wide by 22 tall is 5.5 KB: putting it there would be a serious
 * waste for an About panel.
 *
 * EVERY ROW CARRIES ITS OWN USEFUL RANGE. With a line height of 22 the top rows
 * hold only accents and ascenders: they are nearly all empty, and scanning them
 * in full on every frame is wasted work. Two integers per row are enough to
 * skip them. They live in the same allocation as the mask, at its tail: one
 * malloc, one free, and no way to release half of it. */

bool display_text_mask_build(const lv_font_t *font, const char *s,
                             display_text_mask_t *m)
{
    if (!font || !s || !m) return false;
    memset(m, 0, sizeof(*m));

    const int w = display_font_width(font, s);
    const int h = font->line_height;
    if (w <= 0 || h <= 0) return false;

    /* Count the characters first, because the allocation is a SINGLE one and
     * must also hold their advances. Walking the string twice costs nothing
     * against a second malloc that would have to be freed in step with the
     * first. */
    int n_char = 0;
    { int k = 0; while (utf8_next(s, &k)) n_char++; }

    /* mask + two ranges per row + advances, in one block */
    const size_t mask_sz = (size_t)w * h;
    const size_t rows_sz = (size_t)h * 2 * sizeof(uint16_t);
    const size_t adv_sz  = (size_t)n_char * sizeof(uint16_t);
    uint8_t *blk = heap_caps_calloc(1, mask_sz + rows_sz + adv_sz, MALLOC_CAP_SPIRAM);
    if (!blk) { ESP_LOGE(TAG, "text mask %dx%d not allocated", w, h); return false; }

    m->a      = blk;
    m->w      = (int16_t)w;
    m->h      = (int16_t)h;
    m->row_x0 = (uint16_t *)(blk + mask_sz);
    m->row_x1 = m->row_x0 + h;
    m->adv    = m->row_x1 + h;
    m->n_char = (int16_t)n_char;

    for (int r = 0; r < h; r++) { m->row_x0[r] = (uint16_t)w; m->row_x1[r] = 0; }

    const int base = font_baseline(font);
    int pen = 0, i = 0, n_scritti = 0;
    uint32_t cp = utf8_next(s, &i);

    while (cp) {
        int j = i;
        uint32_t next = utf8_next(s, &j);
        lv_font_glyph_dsc_t g;

        /* Glyph missing: the pen does not advance, but the character EXISTS
         * and must still be counted, or the advances would fall out of step
         * with the string and a progressive reveal would skip positions. */
        if (!lv_font_get_glyph_dsc(font, &g, cp, next)) {
            if (n_scritti < n_char) m->adv[n_scritti++] = (uint16_t)(pen < w ? pen : w);
            cp = next; i = j; continue;
        }

        uint32_t stride = 0;
        const uint8_t *bm = glyph_a8(&g, &stride);
        if (bm) {
            const int gx0 = pen + g.ofs_x;
            const int gy0 = base - g.box_h - g.ofs_y;

            for (int r = 0; r < g.box_h; r++) {
                const int my = gy0 + r;
                if (my < 0 || my >= h) continue;            /* accents outside the line */
                const uint8_t *src = &bm[(size_t)r * stride];
                uint8_t *dst = &m->a[(size_t)my * w];

                for (int c = 0; c < g.box_w; c++) {
                    const int mx = gx0 + c;
                    if (mx < 0 || mx >= w) continue;
                    uint32_t a = a8_to_a32(src[c]);
                    if (!a) continue;
                    /* Glyphs can overlap, with tight kerning or italics: the
                     * MORE OPAQUE value is kept rather than overwriting, so a
                     * pixel does not go dark where two letters touch. */
                    if (a > dst[mx]) dst[mx] = (uint8_t)a;
                    if (mx < m->row_x0[my]) m->row_x0[my] = (uint16_t)mx;
                    if (mx + 1 > m->row_x1[my]) m->row_x1[my] = (uint16_t)(mx + 1);
                }
            }
        }
        lv_font_glyph_release_draw_data(&g);
        pen += g.adv_w;
        /* End of this character: the pen has already advanced, so it is here.
         * Clamped to w because a final letter with side bearing could push the
         * pen past the declared width. */
        if (n_scritti < n_char)
            m->adv[n_scritti++] = (uint16_t)(pen < w ? pen : w);
        cp = next; i = j;
    }

    /* Ink box, derived from the per-row ranges already computed: it costs no
     * second pass over the pixels. */
    m->ink_x0 = (int16_t)w; m->ink_x1 = 0;
    m->ink_y0 = (int16_t)h; m->ink_y1 = 0;
    for (int r = 0; r < h; r++) {
        if (m->row_x1[r] <= m->row_x0[r]) continue;      /* empty row */
        if ((int16_t)m->row_x0[r] < m->ink_x0) m->ink_x0 = (int16_t)m->row_x0[r];
        if ((int16_t)m->row_x1[r] > m->ink_x1) m->ink_x1 = (int16_t)m->row_x1[r];
        if (r < m->ink_y0) m->ink_y0 = (int16_t)r;
        if (r + 1 > m->ink_y1) m->ink_y1 = (int16_t)(r + 1);
    }
    return true;
}

void display_text_mask_free(display_text_mask_t *m)
{
    if (!m || !m->a) return;
    heap_caps_free(m->a);          /* row_x0/row_x1 are in the same block */
    memset(m, 0, sizeof(*m));
}

/* Draw the mask with (x, y) as its top-left corner. This is the function that
 * runs on every frame: nothing inside it depends on the font, only reading
 * bytes and blending. */
void display_text_mask_draw_span(const display_text_mask_t *m, int x, int y,
                                 uint16_t fg, int col0, int col1)
{
    if (!m || !m->a || !s_fb) return;
    if (col0 < 0) col0 = 0;
    if (col1 > m->w) col1 = m->w;
    if (col1 <= col0) return;

    const uint32_t fgx   = pk_expand(fg);
    const uint16_t fg_sw = sw(fg);

    int r0 = (y < 0) ? -y : 0;
    int r1 = m->h;
    if (y + r1 > FB_H) r1 = FB_H - y;

    for (int r = r0; r < r1; r++) {
        /* Useful range of this row, clipped first to the requested span and
         * then to the screen. They are two independent clips: the first says
         * how much text to reveal, the second where the panel ends. */
        int c0 = m->row_x0[r];
        int c1 = m->row_x1[r];
        if (c0 < col0) c0 = col0;
        if (c1 > col1) c1 = col1;
        if (c1 <= c0) continue;                       /* empty row, or outside the span */
        if (x + c0 < 0) c0 = -x;
        if (x + c1 > FB_W) c1 = FB_W - x;
        if (c1 <= c0) continue;                       /* row off screen */

        const uint8_t *src = &m->a[(size_t)r * m->w];
        uint16_t *dst = &s_fb[(size_t)(y + r) * FB_W + x];

        for (int c = c0; c < c1; c++) {
            uint32_t a = src[c];
            if (!a) continue;
            if (a >= 32u) { dst[c] = fg_sw; continue; }
            dst[c] = sw(pk_blend(__builtin_bswap16(dst[c]), fgx, a));
        }
    }
}

/* The whole mask: the normal case. The span variant exists only for revealing
 * text a piece at a time. */
void display_text_mask_draw(const display_text_mask_t *m, int x, int y,
                            uint16_t fg)
{
    if (!m) return;
    display_text_mask_draw_span(m, x, y, fg, 0, m->w);
}

/* --- TRANSPARENT, PROPORTIONAL TEXT ----------------------------------------
 *
 * TWO THINGS AT ONCE, and it is the second that changes the look.
 *
 * TRANSPARENT: only the glyph's own pixels are lit, the rest stays as it is.
 * That is what allows writing OVER an image without dragging a rectangle of
 * background along, which is what display_text does with its bg parameter. It
 * costs nothing extra: the bit test replaces the write it avoids.
 *
 * PROPORTIONAL: the font is fixed width, so every character occupies eight
 * columns even when it uses four — and at scale 2 that is sixteen pixels for an
 * "i". Measuring each glyph's REAL width and advancing by that turns the text
 * proportional instead of gridded: [M] "The quick brown fox" goes from 304 to
 * 250 pixels, 18% narrower, and above all it reads as text rather than as a
 * table.
 *
 * Measuring costs a scan of eight bytes per character, entirely negligible. It
 * could be precomputed, but it is not worth the complication.
 *
 * A NOTE ON BIT ORDER: in this font bit 0 is the LEFTMOST column, so the width
 * is the position of the HIGHEST set bit. */

/* Real width of a glyph, in columns. */
static int glyph_w(const char *glyph)
{
    int w = 0;
    for (int gy = 0; gy < 8; gy++) {
        uint8_t row = (uint8_t)glyph[gy];
        for (int gx = 7; gx >= 0; gx--)
            if (row & (1 << gx)) { if (gx + 1 > w) w = gx + 1; break; }
    }
    return w > 0 ? w : 3;          /* the space character: three columns */
}

int display_text_tr_width(const char *s, int scale, int gap)
{
    if (scale < 1) scale = 1;
    int x = 0;
    for (int i = 0; s[i]; i++) {
        unsigned char c = (unsigned char)s[i];
        const char *g = (c < 128) ? font8x8_basic[c] : font8x8_basic['?'];
        x += (glyph_w(g) + gap) * scale;
    }
    return x - gap * scale;        /* no gap after the last one */
}

void display_text_tr(int x, int y, const char *s, uint16_t fg, int scale, int gap)
{
    if (scale < 1) scale = 1;
    uint16_t fgs = sw(fg);
    int cx = x;

    for (int i = 0; s[i]; i++) {
        unsigned char c = (unsigned char)s[i];
        const char *glyph = (c < 128) ? font8x8_basic[c] : font8x8_basic['?'];
        int gw = glyph_w(glyph);

        for (int gy = 0; gy < 8; gy++) {
            uint8_t row = (uint8_t)glyph[gy];
            for (int gx = 0; gx < gw; gx++) {
                if (!(row & (1 << gx))) continue;   /* pixel off: skip it */
                for (int sy = 0; sy < scale; sy++) {
                    for (int sx = 0; sx < scale; sx++) {
                        int px = cx + gx * scale + sx;
                        int py = y + gy * scale + sy;
                        if (px >= 0 && px < FB_W && py >= 0 && py < FB_H)
                            s_fb[py * FB_W + px] = fgs;
                    }
                }
            }
        }
        cx += (gw + gap) * scale;
    }
}

void display_text(int x, int y, const char *s, uint16_t fg, uint16_t bg, int scale)
{
    if (scale < 1) scale = 1;
    uint16_t fgs = sw(fg), bgs = sw(bg);
    for (int i = 0; s[i]; i++) {
        unsigned char c = (unsigned char)s[i];
        const char *glyph = (c < 128) ? font8x8_basic[c] : font8x8_basic['?'];
        for (int gy = 0; gy < 8; gy++) {
            uint8_t row = (uint8_t)glyph[gy];
            for (int gx = 0; gx < 8; gx++) {
                uint16_t col = (row & (1 << gx)) ? fgs : bgs;
                for (int sy = 0; sy < scale; sy++) {
                    for (int sx = 0; sx < scale; sx++) {
                        int px = x + i * 8 * scale + gx * scale + sx;
                        int py = y + gy * scale + sy;
                        if (px >= 0 && px < FB_W && py >= 0 && py < FB_H)
                            s_fb[py * FB_W + px] = col;
                    }
                }
            }
        }
    }
}

void display_printf(int x, int y, uint16_t fg, uint16_t bg, int scale,
                    const char *fmt, ...)
{
    char line[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    display_text(x, y, line, fg, bg, scale);
}

/* ---- push the framebuffer to the panel, as SPI bursts in bands ---- */
void display_flush(void)
{
    /* Same reason as the other two: drain a pulse left pending by an earlier
     * transfer, or the first wait would return immediately and the memcpy would
     * race the DMA. See the extended note in display_flush_band. */
    xSemaphoreTake(s_done, 0);

    for (int y0 = 0; y0 < FB_H; y0 += BAND_ROWS) {
        int rows = (y0 + BAND_ROWS <= FB_H) ? BAND_ROWS : (FB_H - y0);
        memcpy(s_bounce, &s_fb[y0 * FB_W], (size_t)rows * FB_W * sizeof(uint16_t));
        esp_lcd_panel_draw_bitmap(s_panel, 0, y0, FB_W, y0 + rows, s_bounce);
        /* wait for the transfer to finish before reusing the bounce buffer */
        xSemaphoreTake(s_done, portMAX_DELAY);
    }
}

/* Push ONLY a horizontal band to the screen.
 *
 * [M] Redrawing the whole screen costs 61 ms at 20 MHz; a 40-row band costs 10.
 * It is the same thing display_flush() already does internally band by band —
 * here it is exposed with a window chosen by the caller rather than by the
 * loop. */
/* Push an arbitrary RECTANGLE rather than a full-width band.
 *
 * WHY IT IS NEEDED. An effect that moves a few small objects dirties only their
 * rectangles. [M] Sending the whole band would cost 61 ms for a full screen
 * against 1 ms per object: sixty times as much to draw the same things.
 *
 * THE BOUNCE BUFFER MUST BE REPACKED. The framebuffer has rows of FB_W pixels,
 * while the panel wants the rectangle's pixels CONTIGUOUS: it is copied row by
 * row, skipping the rest. That is also why a pointer into s_fb cannot simply be
 * passed straight through.
 *
 * THE ARBITRATION IS IDENTICAL to display_flush_band: quiet window before
 * taking the bus, lock, release in the ISR. Here, though, it is sent in ONE go
 * rather than in bands: a small rectangle already fits the bounce buffer, and
 * splitting it would add waits without giving the beat a usable window. */
/* --- TRANSFER ACCOUNTING ---------------------------------------------------
 *
 * It became necessary because the About screen's metrics reported "send 0 us/f": sending
 * had never been measured where it happens, and was being counted inside the
 * drawing time instead. A column that always reads zero informs nobody, and it
 * hid the fact that the time spent in there is not all of one kind.
 *
 * TWO things are separated, because they have different causes and different
 * remedies:
 *
 *   - the actual TRANSFER: memcpy into the bounce buffer plus the SPI burst. It
 *     scales with area, and to shorten it one dirties less;
 *   - the WAIT ON THE QUIET WINDOW around the beat, during which consumers of
 *     the bus hold off. It does NOT scale with area: it depends on where the
 *     frame falls relative to the beat, and to shorten it one adjusts the
 *     window, not the drawing.
 *
 * Conflating them would lead to optimising the wrong thing. */
static int64_t  s_fl_us = 0, s_fl_wait_us = 0;
static uint32_t s_fl_n = 0;

static inline void fl_account(int64_t t_in, int64_t wait_us)
{
    s_fl_us      += esp_timer_get_time() - t_in;
    s_fl_wait_us += wait_us;
    s_fl_n++;
}

void display_flush_stats_reset(void)
{
    s_fl_us = s_fl_wait_us = 0; s_fl_n = 0;
}

void display_flush_stats(uint32_t *n, int64_t *total_us, int64_t *wait_us)
{
    if (n)        *n        = s_fl_n;
    if (total_us) *total_us = s_fl_us;
    if (wait_us)  *wait_us  = s_fl_wait_us;
}

void display_flush_rect(int x0, int y0, int w, int h)
{
    /* s_bounce is a STATIC array: its address is always valid and testing it
     * would be meaningless. Only the framebuffer is checked, which is allocated
     * and can be absent. */
    if (!s_fb) return;
    if (x0 < 0) { w += x0; x0 = 0; }
    if (y0 < 0) { h += y0; y0 = 0; }
    if (x0 + w > FB_W) w = FB_W - x0;
    if (y0 + h > FB_H) h = FB_H - y0;
    if (w <= 0 || h <= 0) return;

    const int64_t t_in = esp_timer_get_time();
    int64_t wait_us = 0;

    /* Same reason as in display_flush_band: drain any pulse left behind by a
     * previous transfer. */
    xSemaphoreTake(s_done, 0);

    /* The bounce buffer is sized for BAND_ROWS rows at full width: a larger
     * rectangle does not fit and must be split horizontally. */
    int max_rows = (FB_W * BAND_ROWS) / w;
    if (max_rows < 1) max_rows = 1;

    for (int y = y0; y < y0 + h; y += max_rows) {
        int n = (y + max_rows <= y0 + h) ? max_rows : (y0 + h - y);
        for (int r = 0; r < n; r++)
            memcpy(&s_bounce[(size_t)r * w], &s_fb[(size_t)(y + r) * FB_W + x0],
                   (size_t)w * sizeof(uint16_t));

        int64_t t_defer = esp_timer_get_time();
        while (bsp_spi_in_quiet_window()) vTaskDelay(1);
        uint32_t defer_us = (uint32_t)(esp_timer_get_time() - t_defer);
        wait_us += defer_us;
        if (defer_us < 200) defer_us = 0;

        bsp_spi_lock_display(w, n, defer_us);
        esp_lcd_panel_draw_bitmap(s_panel, x0, y, x0 + w, y + n, s_bounce);
        xSemaphoreTake(s_done, portMAX_DELAY);
    }

    fl_account(t_in, wait_us);
}

void display_flush_band(int y0, int rows)
{
    if (y0 < 0) { rows += y0; y0 = 0; }
    if (y0 >= FB_H || rows <= 0) return;
    if (y0 + rows > FB_H) rows = FB_H - y0;

    const int64_t t_in = esp_timer_get_time();
    int64_t wait_us = 0;

    /* THE SEMAPHORE IS DRAINED BEFORE STARTING.
     *
     * s_done is BINARY and is given by the ISR at the end of every transfer,
     * including LVGL's, which never waits on it. Entering here right after an
     * LVGL flush, the semaphore already holds a pending pulse: the first wait
     * would return IMMEDIATELY, and the memcpy of the next band would overwrite
     * the bounce buffer WHILE THE DMA IS STILL READING IT.
     *
     * [M] The result is a band arriving with another band's content — seen as a
     * displaced image — and trails of dirty pixels behind moving objects. Those
     * were exactly the two symptoms observed. */
    xSemaphoreTake(s_done, 0);

    /* --- BUS ARBITRATION, as on the LVGL path ------------------------------
     *
     * The panel and the W5500 share the same SPI, and that W5500 carries the
     * clock. Two things are required, and this function once did neither:
     *
     *   THE LOCK, because two transfers at once on the same bus corrupt each
     *   other.
     *
     *   THE QUIET WINDOW, the interval around the instant the beat is due to
     *   leave. We stop BEFORE taking the bus: once a DMA has started it cannot
     *   be interrupted, and a 6 ms transfer begun an instant too late would
     *   delay the beat by just as much.
     *
     * WHY BAND BY BAND RATHER THAN ALL AT ONCE. Releasing the bus between bands
     * lets the window open IN THE MIDDLE of a tall band, and the beat gets
     * through. Holding it for the whole band would block the beat for twice as
     * long — the opposite of what is wanted.
     *
     * VISUAL COST: a band can reach the screen in two instalments rather than
     * one, if a beat falls in between. On a 20 frame per second animation that
     * shows up as an occasional half-drawn frame, far less serious than a clock
     * losing time. */
    for (int y = y0; y < y0 + rows; y += BAND_ROWS) {
        int n = (y + BAND_ROWS <= y0 + rows) ? BAND_ROWS : (y0 + rows - y);
        memcpy(s_bounce, &s_fb[y * FB_W], (size_t)n * FB_W * sizeof(uint16_t));

        int64_t t_defer = esp_timer_get_time();
        while (bsp_spi_in_quiet_window()) {
            vTaskDelay(1);                 /* 1 tick = 1 ms */
        }
        uint32_t defer_us = (uint32_t)(esp_timer_get_time() - t_defer);
        wait_us += defer_us;
        if (defer_us < 200) defer_us = 0;  /* not the window, just the tick */

        bsp_spi_lock_display(FB_W, n, defer_us);
        esp_lcd_panel_draw_bitmap(s_panel, 0, y, FB_W, y + n, s_bounce);
        xSemaphoreTake(s_done, portMAX_DELAY);
        /* The lock is released by on_flush_done from the end-of-DMA ISR, as for
         * the LVGL path: here we only wait for the DMA to finish. */
    }

    fl_account(t_in, wait_us);
}

/* The framebuffer, for whoever draws pixels. In PSRAM: sequential writes are
 * fine, scattered accesses much less so. */
uint16_t *display_fb(void) { return s_fb; }

/* ---- accessors for LVGL (esp_lvgl_port uses these handles for the flush) ---- */
esp_lcd_panel_handle_t    display_get_panel(void) { return s_panel; }
esp_lcd_panel_io_handle_t display_get_io(void)    { return s_io; }
