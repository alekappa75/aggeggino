#include "leds.h"
#include "led_strip.h"
#include "esp_log.h"

static const char *TAG = "leds";

#define LED_GPIO   5     /* from the Bottom3 schematic: M5Bus pin8 RGB = GPIO5 */
#define LED_COUNT  10

static led_strip_handle_t s_strip;
static uint8_t s_brightness = 100;   /* percent, applied to every pixel write */
/* raw (unscaled) values as written by callers, so a brightness change can be
 * re-applied to the pixels already on the strip without the caller redrawing */
static uint8_t s_raw[LED_COUNT][3];

/* scale a channel by the global brightness (rounded) */
static inline uint8_t scale(uint8_t v)
{
    if (s_brightness >= 100) return v;
    return (uint8_t)(((uint16_t)v * s_brightness + 50) / 100);
}

void leds_set_brightness(uint8_t percent)
{
    uint8_t p = (percent > 100) ? 100 : percent;
    if (p == s_brightness) return;
    s_brightness = p;
    /* re-apply to what's already on the strip: callers use change-detection and
     * won't redraw just because the brightness changed. */
    if (s_strip) {
        for (int i = 0; i < LED_COUNT; i++)
            led_strip_set_pixel(s_strip, i, scale(s_raw[i][0]),
                                            scale(s_raw[i][1]),
                                            scale(s_raw[i][2]));
        led_strip_refresh(s_strip);
    }
}

uint8_t leds_get_brightness(void)
{
    return s_brightness;
}

esp_err_t leds_init(void)
{
    led_strip_config_t sc = {
        .strip_gpio_num = LED_GPIO,
        .max_leds       = LED_COUNT,
        .led_model      = LED_MODEL_WS2812,
        /* WS2812 = GRB order. NOTE: in led_strip 3.x the field is
         * 'color_component_format'; if the build complains, replace with:
         *   .led_pixel_format = LED_PIXEL_FORMAT_GRB,   (led_strip 2.x) */
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags.invert_out = false,
    };
    led_strip_rmt_config_t rc = {
        .clk_src        = RMT_CLK_SRC_DEFAULT,
        .resolution_hz  = 10 * 1000 * 1000,   /* 10 MHz */
        .flags.with_dma = false,
    };
    esp_err_t e = led_strip_new_rmt_device(&sc, &rc, &s_strip);
    if (e != ESP_OK) { ESP_LOGE(TAG, "led_strip init: %s", esp_err_to_name(e)); return e; }
    led_strip_clear(s_strip);
    ESP_LOGI(TAG, "%d WS2812 su GPIO%d", LED_COUNT, LED_GPIO);
    return ESP_OK;
}

void leds_set(int i, uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_strip || i < 0 || i >= LED_COUNT) return;
    s_raw[i][0] = r; s_raw[i][1] = g; s_raw[i][2] = b;
    led_strip_set_pixel(s_strip, i, scale(r), scale(g), scale(b));
}
void leds_fill(uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_strip) return;
    uint8_t sr = scale(r), sg = scale(g), sb = scale(b);
    for (int i = 0; i < LED_COUNT; i++) {
        s_raw[i][0] = r; s_raw[i][1] = g; s_raw[i][2] = b;
        led_strip_set_pixel(s_strip, i, sr, sg, sb);
    }
}
void leds_show(void)  { if (s_strip) led_strip_refresh(s_strip); }
void leds_clear(void) { if (s_strip) led_strip_clear(s_strip); }
int  leds_count(void) { return LED_COUNT; }
