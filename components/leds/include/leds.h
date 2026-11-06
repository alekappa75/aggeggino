/*
 * leds.h — striscia WS2812C del M5GO Bottom3 (10 LED su GPIO5, da schematico).
 * Driven over RMT (the espressif/led_strip component). Future plan: userli come
 * them as at-a-glance status indicators (green=Link, blue=AP only, red=W5500 KO).
 */
#pragma once
#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t leds_init(void);
void leds_set(int i, uint8_t r, uint8_t g, uint8_t b);  /* one pixel */
void leds_fill(uint8_t r, uint8_t g, uint8_t b);        /* all      */
void leds_show(void);                                   /* send     */
void leds_clear(void);
int  leds_count(void);

/* Global brightness 0..100 (%): scales EVERY colour passed to leds_set /
 * leds_fill. Default 100 (no attenuation). Applied at the moment the pixel
 * is written, so it holds for every caller. */
void leds_set_brightness(uint8_t percent);
uint8_t leds_get_brightness(void);

#ifdef __cplusplus
}
#endif
