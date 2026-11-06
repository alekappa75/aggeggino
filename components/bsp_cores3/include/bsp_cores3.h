/*
 * bsp_cores3.h — hardware bring-up for the M5Stack CoreS3 (SE).
 *
 * Responsibilities:
 *   1) internal I2C (AXP2101 PMIC + AW9523 IO expander), which powers and
 *      resets the LCD panel;
 *   2) a SINGLE SPI bus (spi_master) on the M-Bus pins, shared by
 *        - the ILI9342C display  (esp_lcd device, write only)
 *        - the W5500 LAN module  (ioLibrary device, read and write);
 *   3) the CoreS3 catch: MISO and the display's DC are the SAME PIN, and it
 *      must be switched between input and output at the right moments.
 *
 * That third point is what makes this component necessary rather than
 * boilerplate. Everything below about locking, quiet windows and contention
 * statistics exists because two devices with very different timing needs share
 * one bus — and one of them, the network, pays for every microsecond it spends
 * waiting.
 *
 * [M] The pin assignments are validated by the system working: a wrong I2C pin
 * means the PMIC never answers and the panel never lights.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "driver/spi_master.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --- internal I2C ---------------------------------------------------------- */
#define BSP_I2C_PORT     0
#define BSP_I2C_SDA_GPIO 12
#define BSP_I2C_SCL_GPIO 11
#define BSP_I2C_FREQ_HZ  400000

#define BSP_AXP2101_ADDR 0x34   /* PMIC                                        */
#define BSP_AW9523_ADDR  0x58   /* IO expander: LCD reset, bus and backlight   */
#define BSP_FT6336_ADDR  0x38   /* capacitive touch controller                 */

/* --- shared SPI bus (CoreS3 M-Bus) -----------------------------------------
 *
 * MIND GPIO35: it is MISO *and* the display's DC line. See the bsp_dc_*
 * functions at the bottom of this header. */
#define BSP_SPI_HOST     SPI2_HOST
#define BSP_SPI_SCLK     36
#define BSP_SPI_MOSI     37
#define BSP_SPI_MISO     35     /* == display DC, see bsp_dc_*  */
#define BSP_LCD_DC_GPIO  35     /* the very same physical pin   */
#define BSP_LCD_CS_GPIO  3
#define BSP_W5500_CS_GPIO 1     /* [M] confirmed by chip scan, VERSIONR = 0x04 */
#define BSP_W5500_RST_GPIO -1   /* not wired: reset goes through the M-Bus     */

/* INTn of the LAN module, on M-Bus pin 2.
 *
 * The module offers a jumper between pin 2 (G10) and pin 26 (G14). Pin 26 is
 * NOT usable here: G14 carries I2S data for the audio amplifier.
 *
 * THIS JUMPER IS REQUIRED. Beat reception is driven by this interrupt — there
 * is no polling fallback — so a value below zero stops the build with an
 * #error in main.c rather than yielding a device that never reads a beat. */
#define BSP_W5500_INT_GPIO 10

/* SPI clocks. The display could go faster; the W5500 must stay at or below
 * 20 MHz on the CoreS3. */
#define BSP_LCD_SPI_HZ    (20 * 1000 * 1000)
#define BSP_W5500_SPI_HZ  (20 * 1000 * 1000)

/* Panel size. */
#define BSP_LCD_H_RES 320
#define BSP_LCD_V_RES 240

/* --- power, panel, I2C ----------------------------------------------------- */

/* Bring up I2C, PMIC and IO expander, then power and reset the panel. */
esp_err_t bsp_power_init(void);

/* LCD backlight, level 0..2 (low, medium, high). Acts on the AXP2101 DLDO1
 * rail that feeds the backlight. The range is deliberately conservative:
 * going lower would switch the backlight off entirely. */
void bsp_set_backlight_level(int level);

/* Real power-off through the PMU. Does not return if it succeeds. */
void bsp_power_off(void);

/* Attach a device to the BSP's internal I2C bus, already initialised. For
 * components sharing that bus, such as the AW88298 audio amplifier at 0x36. */
esp_err_t bsp_i2c_add_device(uint8_t addr, i2c_master_dev_handle_t *out);

/* Scan the internal I2C bus and log the addresses that answer. Diagnostic. */
void bsp_i2c_scan(void);

/* Die temperature of the ESP32-S3 — not ambient, which typically sits 10 to 20
 * degrees lower. bsp_temp_read returns false if the sensor was not initialised
 * or the reading fails. */
esp_err_t bsp_temp_init(void);
bool      bsp_temp_read(float *celsius);

/* --- touch ------------------------------------------------------------------ */

/* Read the FT6336 capacitive touch. Returns true when a touch is present and
 * fills *x,*y with coordinates in 0..319, 0..239. */
bool bsp_touch_read(uint16_t *x, uint16_t *y);

/* Mirror touch coordinates on both axes, to follow a rotated image. Must be
 * kept in step with display_set_flipped(). */
void bsp_touch_set_flipped(bool on);

/* --- the shared SPI bus ----------------------------------------------------- */

/* Create the shared SPI bus. Call ONCE, before registering the display and the
 * W5500 as devices on the same host. */
esp_err_t bsp_spi_bus_init(void);

/* Serialising the bus between the display (LVGL flush) and the W5500. Whoever
 * touches the bus takes the lock; a display flush releases it from the
 * end-of-DMA ISR via bsp_spi_unlock_isr(). */
void bsp_spi_lock(void);

/* The DISPLAY taking the bus. Kept separate from bsp_spi_lock() for a precise
 * reason: with a single "hold" metric we could never tell WHO held the bus —
 * a flush, or the network — and so could not tell where the time went. The two
 * paths are now counted apart, and for flushes we also know how many there
 * were, how large, and how long the quiet window delayed them. */
void bsp_spi_lock_display(int w, int h, uint32_t defer_us);

void bsp_spi_unlock(void);
void bsp_spi_unlock_isr(BaseType_t *hpw);

/* SPI contention statistics, in microseconds. Reading them RESETS the window. */
typedef struct {
    /* W5500 side: whoever WAITS is whoever pays in beat jitter. */
    uint32_t net_wait_max, net_wait_avg, net_hold_max;

    /* WHO caused the wait. Knowing how long we waited is not enough to decide:
     * a wait behind the display is cured by the quiet window, a wait behind
     * another network transaction is not. */
    bool     net_wait_max_by_display;  /* was the longest wait behind a flush? */
    uint32_t net_wait_long_n;          /* waits over 500 us                    */
    uint32_t net_wait_long_by_display; /* ...of which behind a flush           */

    /* Display side: a flush is the only thing that holds the bus for long. */
    uint32_t disp_n;            /* flushes in the window                   */
    uint32_t disp_hold_max;     /* the longest one                         */
    uint32_t disp_hold_tot;     /* TOTAL time with the bus in display hands */
    uint32_t disp_px_max;       /* pixels of the largest area              */
    uint32_t disp_defer_n;      /* flushes delayed by the quiet window     */
    uint32_t disp_defer_max;    /* longest delay the window imposed        */
} bsp_spi_stats_t;

/* Read the statistics and RESET them for the next window. */
void bsp_spi_stats(bsp_spi_stats_t *out);

/* Quiet window: the display starts no flush between from_us and to_us, given
 * as absolute esp_timer instants. Anchored to the transmission of the beat
 * packet, whose emission instant is what players lock their phase to. */
void bsp_spi_set_quiet_window(int64_t from_us, int64_t to_us);
bool bsp_spi_in_quiet_window(void);

/* TARGETED probe on the bus waits of ONE caller.
 *
 * The general statistics are dominated by the main loop, which takes the bus
 * thousands of times per second; the beat, at sixty times a minute, is
 * invisible in there. This pair measures only the task that calls begin, and
 * must be closed with end from that same task.
 *   wait_sum_us     total wait accumulated over the stretch
 *   wait_max_us     the single longest wait
 *   n_lock          how many times the bus was taken
 *   max_by_display  who held the bus during the longest wait
 * Not reentrant, and not meant for concurrent use: one probe at a time. */
void bsp_spi_watch_begin(void);
void bsp_spi_watch_end(uint32_t *wait_sum_us, uint32_t *wait_max_us,
                       uint32_t *n_lock, bool *max_by_display);

/* --- MISO / DC handling -----------------------------------------------------
 *
 * BSP_SPI_MISO (35) is also the display's DC line. The operating rule is:
 *   - before any transaction that MUST read (the W5500): pin = INPUT (MISO)
 *   - for the display: DC must be driven as OUTPUT, low for command, high for
 *     data.
 *
 * These are hooked as pre_cb/post_cb on the SPI devices
 * (spi_device_interface_config_t), or called by hand. */
void bsp_dc_set_data(bool is_data);   /* display: DC as output, data or command */
void bsp_dc_as_miso(void);            /* W5500 or SD: pin as input (MISO)       */
void bsp_dc_as_output(void);          /* restore the DC role after a read       */

#ifdef __cplusplus
}
#endif
