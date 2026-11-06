/*
 * bsp_cores3.c — see bsp_cores3.h.
 *
 * [M] The AXP2101 and AW9523 register sequences below are the ones that bring
 * this board up: rail enables, LCD reset and backlight all work as written.
 * They were derived from the M5CoreS3 schematic and the M5Unified sources, then
 * confirmed on hardware — a wrong value here does not degrade anything, it
 * leaves the panel dark.
 */
#include "bsp_cores3.h"
#include "esp_timer.h"
#include "driver/i2c_master.h"
#include "driver/temperature_sensor.h"
#include "esp_log.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "bsp_cores3";

static i2c_master_bus_handle_t s_i2c_bus;
static i2c_master_dev_handle_t s_axp, s_aw, s_touch;
static SemaphoreHandle_t s_spi_sem;

/* ---------- I2C helpers ---------- */
static esp_err_t i2c_w8(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t val)
{
    uint8_t b[2] = { reg, val };
    return i2c_master_transmit(dev, b, sizeof(b), 100);
}

static uint8_t i2c_r8(i2c_master_dev_handle_t dev, uint8_t reg)
{
    uint8_t v = 0xFF;
    i2c_master_transmit_receive(dev, &reg, 1, &v, 1, 100);
    return v;
}

static esp_err_t i2c_add(uint8_t addr, i2c_master_dev_handle_t *out)
{
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = addr,
        .scl_speed_hz    = BSP_I2C_FREQ_HZ,
    };
    return i2c_master_bus_add_device(s_i2c_bus, &cfg, out);
}

/* Attach a device to the INTERNAL I2C bus already initialised by the BSP. For
 * components such as the AW88298 amplifier that live on that shared bus and
 * must not re-initialise it — there is one bus, configured once. */
esp_err_t bsp_i2c_add_device(uint8_t addr, i2c_master_dev_handle_t *out)
{
    if (!s_i2c_bus) return ESP_ERR_INVALID_STATE;   /* bsp_init not called yet */
    return i2c_add(addr, out);
}

/* Diagnostic scan of the internal I2C bus: try every 7-bit address and log the
 * ones that acknowledge. It exists to SEE what is actually on the bus rather
 * than assume — useful when an expected chip does not answer and the question
 * is whether it sits at another address or is absent. */
void bsp_i2c_scan(void)
{
    if (!s_i2c_bus) { ESP_LOGW(TAG, "i2c scan: bus not ready"); return; }
    ESP_LOGI(TAG, "i2c scan: looking for devices on the internal bus...");
    int found = 0;
    for (uint8_t a = 0x03; a < 0x78; a++) {
        esp_err_t r = i2c_master_probe(s_i2c_bus, a, 50);
        if (r == ESP_OK) {
            ESP_LOGI(TAG, "  i2c 0x%02x: presente (ACK)", a);
            found++;
        }
    }
    ESP_LOGI(TAG, "i2c scan: %d device trovati", found);
}

/* ---------- PMIC and IO expander ---------- */
esp_err_t bsp_power_init(void)
{
    i2c_master_bus_config_t bc = {
        .clk_source       = I2C_CLK_SRC_DEFAULT,
        .i2c_port         = BSP_I2C_PORT,
        .sda_io_num       = BSP_I2C_SDA_GPIO,
        .scl_io_num       = BSP_I2C_SCL_GPIO,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bc, &s_i2c_bus), TAG, "i2c bus");
    ESP_RETURN_ON_ERROR(i2c_add(BSP_AXP2101_ADDR, &s_axp), TAG, "axp add");
    ESP_RETURN_ON_ERROR(i2c_add(BSP_AW9523_ADDR,  &s_aw),  TAG, "aw add");
    ESP_RETURN_ON_ERROR(i2c_add(BSP_FT6336_ADDR,  &s_touch), TAG, "touch add");

    /* --- I2C probe: do both chips answer? --- */
    uint8_t axp_id = i2c_r8(s_axp, 0x03);   /* AXP2101 IC type, expect ~0x4A */
    uint8_t aw_id  = i2c_r8(s_aw,  0x10);   /* AW9523 ID, expect 0x23        */
    ESP_LOGI(TAG, "I2C probe: AXP2101 reg0x03=0x%02x (expect ~0x4A), AW9523 reg0x10=0x%02x (expect 0x23)",
             axp_id, aw_id);

    /* --- AW9523B: GPIO mode, push-pull, both ports as outputs --- */
    i2c_w8(s_aw, 0x12, 0xFF);   /* P0 -> GPIO mode, not LED  */
    i2c_w8(s_aw, 0x13, 0xFF);   /* P1 -> GPIO mode           */
    i2c_w8(s_aw, 0x11, 0x10);   /* P0 push-pull              */
    i2c_w8(s_aw, 0x04, 0x00);   /* P0 all outputs            */
    i2c_w8(s_aw, 0x05, 0x00);   /* P1 all outputs            */
    /* P1: BOOST_EN (bit 7) = 1, LCD_RST (bit 1) = 0 -> panel held in reset */
    i2c_w8(s_aw, 0x03, 0x80);
    i2c_w8(s_aw, 0x02, 0x00);   /* P0 low: touch reset and others */
    vTaskDelay(pdMS_TO_TICKS(20));

    /* --- AXP2101: rail sequence --- */
    i2c_w8(s_axp, 0x90, 0xBF);  /* LDOs ON: ALDO1-4, BLDO1-2, DLDO1 */
    i2c_w8(s_axp, 0x92, 0x0D);  /* ALDO1 = 1.8V (18-5)              */
    i2c_w8(s_axp, 0x93, 0x1C);  /* ALDO2 = 3.3V (33-5)              */
    i2c_w8(s_axp, 0x94, 0x1C);  /* ALDO3 = 3.3V                     */
    i2c_w8(s_axp, 0x95, 0x1C);  /* ALDO4 = 3.3V                     */
    i2c_w8(s_axp, 0x27, 0x00);  /* PowerKey hold and off            */
    i2c_w8(s_axp, 0x69, 0x11);  /* CHGLED                           */
    i2c_w8(s_axp, 0x10, 0x30);  /* PMU common config                */
    i2c_w8(s_axp, 0x30, 0x0F);  /* ADC enabled                      */
    /* DLDO1 feeds the LCD backlight, about 3.3 V. */
    i2c_w8(s_axp, 0x99, 0x1C);
    vTaskDelay(pdMS_TO_TICKS(30));

    /* --- release the panel from reset: LCD_RST (P1_1) 0 -> 1 --- */
    i2c_w8(s_aw, 0x03, 0x82);   /* BOOST_EN=1, LCD_RST=1 */
    vTaskDelay(pdMS_TO_TICKS(120));

    /* --- release the touch controller: FT6336_RST (P0_0) 0 -> 1 --- */
    i2c_w8(s_aw, 0x02, 0x01);   /* P0_0 high = touch out of reset */
    vTaskDelay(pdMS_TO_TICKS(30));

    ESP_LOGI(TAG, "bsp_power_init: real sequence applied (backlight DLDO1, LCD reset released)");
    return ESP_OK;
}

void bsp_set_backlight_level(int level)
{
    /* DLDO1, register 0x99, feeds the backlight. AXP2101 encoding is
     * 0.5 V + N x 0.1 V, so 0x1C = 28 gives 3.3 V — the value used at init, and
     * maximum brightness. The range is conservative: [M] below about 2.5 V the
     * backlight switches off or flickers. */
    static const uint8_t v[3] = {
        0x15,   /* LOW  ~2.6V */
        0x17,   /* MED  ~2.8V */
        0x1C,   /* HIGH ~3.3 V, the init default */
    };
    if (level < 0) level = 0;
    if (level > 2) level = 2;
    if (!s_axp) return;
    i2c_w8(s_axp, 0x99, v[level]);
    ESP_LOGI(TAG, "backlight level %d -> DLDO1 0x%02X", level, v[level]);
}

/* ---------- shared SPI bus ---------- */
esp_err_t bsp_spi_bus_init(void)
{
    spi_bus_config_t buscfg = {
        .sclk_io_num     = BSP_SPI_SCLK,
        .mosi_io_num     = BSP_SPI_MOSI,
        .miso_io_num     = BSP_SPI_MISO,   /* == the display's DC */
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        /* Max transfer: a full RGB565 frame, for a whole-screen draw_bitmap. */
        .max_transfer_sz = BSP_LCD_H_RES * BSP_LCD_V_RES * 2 + 64,
    };
    /* automatic DMA channel */
    esp_err_t err = spi_bus_initialize(BSP_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "shared SPI bus on host %d (SCK=%d MOSI=%d MISO/DC=%d)",
             BSP_SPI_HOST, BSP_SPI_SCLK, BSP_SPI_MOSI, BSP_SPI_MISO);

    /* Semaphore serialising the bus between the display (LVGL flush) and the
     * W5500: the two must never touch the SPI hardware at the same time. */
    if (!s_spi_sem) {
        s_spi_sem = xSemaphoreCreateBinary();
        if (!s_spi_sem) return ESP_ERR_NO_MEM;
        xSemaphoreGive(s_spi_sem);   /* starts free */
    }
    return ESP_OK;
}

/* --- Bus contention instrumentation ----------------------------------------
 *
 * How long the SPI bus is WAITED FOR, and how long it is HELD. The waiting time
 * is the jitter that would go straight into the Pioneers' phase if a beat
 * packet ever had to wait behind a flush.
 *
 * The counters are SEPARATE for the two contenders. Shared, they were useless:
 * there was no telling whether the milliseconds of holding belonged to a flush
 * or to the W5500. */
static volatile uint32_t s_net_wait_max = 0, s_net_wait_sum = 0, s_net_wait_n = 0;
static volatile uint32_t s_net_hold_max = 0;
static volatile uint32_t s_d_n = 0, s_d_hold_max = 0, s_d_hold_tot = 0;
static volatile uint32_t s_d_px_max = 0, s_d_defer_n = 0, s_d_defer_max = 0;

static int64_t s_hold_start_us = 0;
static volatile bool s_hold_is_display = false;   /* who holds the bus right now */

/* WHO held the bus during the longest wait of the window.
 *
 * Needed because knowing HOW LONG we waited says NOTHING about what to do:
 * 2 ms behind a display flush is cured by the quiet window, 2 ms behind another
 * network transaction is cured in an entirely different way. [M] The beat was
 * found to take about 2.2 ms to send despite waking on time and doing only six
 * transactions, so it was waiting for somebody — this says who.
 *
 * Sampled ONLY on the maximum, so it costs nothing in the normal case. */
static volatile bool     s_net_wait_by_disp = false;  /* was the longest wait behind the display? */
static volatile uint32_t s_net_wait_long_n  = 0;      /* waits over the threshold     */
static volatile uint32_t s_net_wait_long_d  = 0;      /* ...of which behind the display */
#define NET_WAIT_LONG_US 500u

/* --- TARGETED probe on a single caller -------------------------------------
 *
 * The counters above cover EVERY locker that is not the display: thousands of
 * acquisitions per second, dominated by the main loop. The beat takes the bus
 * sixty times a minute, so it is invisible in there — [M] and indeed those
 * statistics said 90% of long waits were behind the display, which is true FOR
 * THE MAIN LOOP, unprotected by the quiet window, and says nothing about the
 * beat.
 *
 * This probe measures the wait of ONLY the task that opens it, between begin
 * and end. Filtering on the task handle is necessary: while the beat is blocked
 * on the semaphore another task may take the bus, and without the filter that
 * would land in the beat's own figures. */
static TaskHandle_t     s_watch_task = NULL;
static volatile uint32_t s_w_sum = 0, s_w_max = 0, s_w_n = 0;
static volatile bool     s_w_max_disp = false;



void bsp_spi_lock(void)                    /* the W5500, and anyone but the display */
{
    int64_t t0 = esp_timer_get_time();
    /* Who holds the bus NOW, which is who will make us wait. It must be read
     * BEFORE xSemaphoreTake: once we have it, s_hold_is_display is us. */
    bool blocked_by_display = s_hold_is_display;

    xSemaphoreTake(s_spi_sem, portMAX_DELAY);

    uint32_t w = (uint32_t)(esp_timer_get_time() - t0);
    if (w > s_net_wait_max) { s_net_wait_max = w; s_net_wait_by_disp = blocked_by_display; }
    if (w >= NET_WAIT_LONG_US) {
        s_net_wait_long_n++;
        if (blocked_by_display) s_net_wait_long_d++;
    }
    s_net_wait_sum += w; s_net_wait_n++;

    /* Targeted probe: counts only for the task that opened it. */
    if (s_watch_task && s_watch_task == xTaskGetCurrentTaskHandle()) {
        s_w_sum += w; s_w_n++;
        if (w > s_w_max) { s_w_max = w; s_w_max_disp = blocked_by_display; }
    }

    s_hold_start_us   = esp_timer_get_time();
    s_hold_is_display = false;
}

void bsp_spi_watch_begin(void)
{
    s_w_sum = s_w_max = s_w_n = 0;
    s_w_max_disp = false;
    s_watch_task = xTaskGetCurrentTaskHandle();
}

void bsp_spi_watch_end(uint32_t *wait_sum_us, uint32_t *wait_max_us,
                       uint32_t *n_lock, bool *max_by_display)
{
    s_watch_task = NULL;
    if (wait_sum_us)    *wait_sum_us    = s_w_sum;
    if (wait_max_us)    *wait_max_us    = s_w_max;
    if (n_lock)         *n_lock         = s_w_n;
    if (max_by_display) *max_by_display = s_w_max_disp;
}

void bsp_spi_lock_display(int w, int h, uint32_t defer_us)
{
    xSemaphoreTake(s_spi_sem, portMAX_DELAY);
    s_hold_start_us   = esp_timer_get_time();
    s_hold_is_display = true;
    s_d_n++;
    uint32_t px = (uint32_t)w * (uint32_t)h;
    if (px > s_d_px_max) s_d_px_max = px;
    if (defer_us) {
        s_d_defer_n++;
        if (defer_us > s_d_defer_max) s_d_defer_max = defer_us;
    }
}

static inline void note_hold(void)
{
    uint32_t hold = (uint32_t)(esp_timer_get_time() - s_hold_start_us);
    if (s_hold_is_display) {
        if (hold > s_d_hold_max) s_d_hold_max = hold;
        s_d_hold_tot += hold;
    } else {
        if (hold > s_net_hold_max) s_net_hold_max = hold;
    }
}

void bsp_spi_unlock(void)
{
    note_hold();
    xSemaphoreGive(s_spi_sem);
}

void bsp_spi_unlock_isr(BaseType_t *hpw)
{
    note_hold();
    xSemaphoreGiveFromISR(s_spi_sem, hpw);
}

/* --- SPI quiet window -------------------------------------------------------
 *
 * The display must NOT start a flush ([M] about 3.1 ms of DMA) close to the
 * instant a beat packet is due to leave: the bus wait would go straight into
 * the Pioneers' phase.
 *
 * A single instant of truth, T_send, governs both the send timer and this
 * window, which makes it impossible for the two to drift apart. */
static volatile int64_t s_quiet_from = 0, s_quiet_to = 0;

void bsp_spi_set_quiet_window(int64_t from_us, int64_t to_us)
{
    s_quiet_from = from_us;
    s_quiet_to   = to_us;
}



bool bsp_spi_in_quiet_window(void)
{
    int64_t t = esp_timer_get_time();
    return (s_quiet_from != 0) && (t >= s_quiet_from) && (t <= s_quiet_to);
}

void bsp_spi_stats(bsp_spi_stats_t *out)
{
    if (out) {
        out->net_wait_max  = s_net_wait_max;
        out->net_wait_avg  = s_net_wait_n ? (s_net_wait_sum / s_net_wait_n) : 0;
        out->net_hold_max  = s_net_hold_max;
        out->net_wait_max_by_display  = s_net_wait_by_disp;
        out->net_wait_long_n          = s_net_wait_long_n;
        out->net_wait_long_by_display = s_net_wait_long_d;
        out->disp_n        = s_d_n;
        out->disp_hold_max = s_d_hold_max;
        out->disp_hold_tot = s_d_hold_tot;
        out->disp_px_max   = s_d_px_max;
        out->disp_defer_n  = s_d_defer_n;
        out->disp_defer_max= s_d_defer_max;
    }
    s_net_wait_max = s_net_wait_sum = s_net_wait_n = s_net_hold_max = 0;
    s_net_wait_by_disp = false;
    s_net_wait_long_n = s_net_wait_long_d = 0;
    s_d_n = s_d_hold_max = s_d_hold_tot = s_d_px_max = 0;
    s_d_defer_n = s_d_defer_max = 0;
}

/* ---------- MISO and DC on one pin ---------- */
/*
 * The dual-role pin, handled with gpio_set_direction on GPIO35.
 *
 * The display is driven through esp_lcd_panel_io_spi with a dedicated DC pin,
 * and the pin is switched to MISO only for W5500 transactions.
 */
void bsp_dc_set_data(bool is_data)
{
    gpio_set_direction(BSP_LCD_DC_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(BSP_LCD_DC_GPIO, is_data ? 1 : 0);
}

void bsp_dc_as_miso(void)
{
    /* Hand the pin back to the SPI peripheral as an input (MISO). */
    gpio_set_direction(BSP_SPI_MISO, GPIO_MODE_INPUT);
    /* If the SPI driver ever detached the pin from its matrix, the peripheral's
     * MISO function would have to be reattached to GPIO35 here. */
}

void bsp_dc_as_output(void)
{
    /* Put GPIO35 back to output so esp_lcd can drive it as DC. The level is set
     * by esp_lcd itself on the next transaction. */
    gpio_set_direction(BSP_LCD_DC_GPIO, GPIO_MODE_OUTPUT);
}

/* ---------- ESP32-S3 internal temperature sensor ----------
 *
 * It measures the DIE temperature, not ambient: at idle it typically sits 10 to
 * 20 degrees above the surrounding air. Useful for two things — noticing that
 * the unit is overheating in a booth, and, less obviously, corroborating the
 * crystal drift, which [M] was measured moving by about 20 ppm over the first
 * five minutes, exactly while the chip warms up.
 *
 * The -10..80 degree range is the one with the best resolution, about 0.4
 * degrees; outside it readings stay valid but less precise. */
static temperature_sensor_handle_t s_tsens = NULL;

esp_err_t bsp_temp_init(void)
{
    temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
    esp_err_t e = temperature_sensor_install(&cfg, &s_tsens);
    if (e != ESP_OK) { s_tsens = NULL; return e; }
    e = temperature_sensor_enable(s_tsens);
    if (e != ESP_OK) { temperature_sensor_uninstall(s_tsens); s_tsens = NULL; }
    return e;
}

bool bsp_temp_read(float *celsius)
{
    if (!s_tsens || !celsius) return false;
    return temperature_sensor_get_celsius(s_tsens, celsius) == ESP_OK;
}

/* ---------- FT6336 touch ---------- */
/* --- TOUCH READ CACHE ------------------------------------------------------
 *
 * WHY. bsp_touch_read() has TWO callers, both periodic and both after the same
 * data:
 *   touch_read_cb   the LVGL indev callback, every 33 ms
 *   buttons_timer   the silkscreened buttons, every 40 ms
 * The two timers drift against each other and often fire within a few
 * milliseconds, which means two I2C transactions to read the same panel state.
 * With a validity window the second reuses the first.
 *
 * WHY HERE AND NOT IN THE CALLERS. This way it is transparent: no change to
 * ui.c, no caller to remember to convert, and any future third caller benefits
 * without knowing.
 *
 * WHY WITHOUT A LOCK. Verified: touch_read_cb is the indev read_cb and
 * buttons_timer is an lv_timer — both run on the LVGL TASK, served by
 * lv_timer_handler(). There is no concurrency, and a mutex here would be pure
 * cost. IF some other task ever called this function that premise would fall,
 * and a critical section would be needed.
 *
 * THE WINDOW is 15 ms, less than half the period of the faster caller (33 ms).
 * So nobody ever receives data older than half their own sampling interval, and
 * nobody is left without fresh readings.
 *
 * It does NOT change semantics: same return value, same coordinates. And it
 * cannot lose a touch — a touch lasts hundreds of milliseconds, and the
 * debounce upstream requires two agreeing samples anyway.
 *
 * [M] Measured in the field: 38% of reads saved, 975 real out of 1583 calls in
 * 30 s, exactly what the simulation predicted. */
/* Full scale of the capacitive panel. X matches the display; Y does NOT,
 * because it also covers the silkscreened strip below the screen.
 *
 * [M] Measured with a probe: X runs 0..319, Y runs 0..279. Rows 0..239 are the
 * display, 240..279 the capacitive strip underneath. */
#define BSP_TOUCH_X_MAX         319
#define BSP_TOUCH_SCREEN_Y_MAX  239    /* last row of the DISPLAY */

static bool s_touch_flipped = false;


void bsp_touch_set_flipped(bool on) { s_touch_flipped = on; }

#define TOUCH_CACHE_US   15000

static int64_t  s_tc_us = 0;      /* instant of the cached read, 0 = none */
static bool     s_tc_ok = false;
static uint16_t s_tc_x = 0, s_tc_y = 0;

/* The real read from the panel. A single transaction when nothing is touching:
 * the active-points register returns zero and we are done. */
static bool touch_read_raw(uint16_t *x, uint16_t *y)
{
    /* reg 0x02 = number of active points, bits 3..0 */
    uint8_t reg = 0x02, np = 0;
    if (i2c_master_transmit_receive(s_touch, &reg, 1, &np, 1, 50) != ESP_OK) return false;
    if ((np & 0x0F) == 0) return false;

    /* reg 0x03..0x06 = X_hi, X_lo, Y_hi, Y_lo of the first point, 12 bits */
    uint8_t b[4]; reg = 0x03;
    if (i2c_master_transmit_receive(s_touch, &reg, 1, b, 4, 50) != ESP_OK) return false;
    uint16_t rx = (uint16_t)(((b[0] & 0x0F) << 8) | b[1]);
    uint16_t ry = (uint16_t)(((b[2] & 0x0F) << 8) | b[3]);

    /* --- 180 DEGREE ROTATION ------------------------------------------------
     *
     * Mirrored on both axes, to follow the image the panel rotates on its own
     * (display_set_flipped). Mirroring only one produces an interface that
     * responds at the point OPPOSITE the one touched.
     *
     * THE Y FULL SCALE IS NOT 239. The sensitive area does not end with the
     * display: below it lies the silkscreened capacitive strip, which the code
     * recognises as y >= 248. Mirroring with 239 - y would send the strip to
     * negative coordinates and make it UNREACHABLE exactly when it is needed —
     * to go back and turn the rotation off.
     *
     * [M] The full scales were measured with a probe, not deduced: X 0..319,
     * Y 0..279, with rows 240..279 belonging to the strip. */

    if (s_touch_flipped) {
        /* THE STRIP IS NOT MIRRORED, on either axis.
         *
         * Its three marks are PRINTED on the glass: they do not rotate. The
         * user touches the circle where it is, and the panel always reports the
         * same coordinates — [M] x between 178 and 196, y = 279.
         *
         * Mirroring X with 319 - x moved them to 123..141, below the centre
         * band (145..195), which was then read as the left arrow. The band is
         * centred on 170 and the mirror axis is at 159.5, so mirroring moves it
         * to 124..174 and the circle falls outside.
         *
         * Left intact, the already calibrated bands keep working exactly as
         * they do with the screen upright.
         *
         * THE ARROWS do swap, but that is a matter of ACTION rather than
         * coordinates: settings_ui inverts the direction when the screen is
         * rotated. The arrow physically on the left, seen on a turned device,
         * appears to the user on the right — and its upside-down glyph looks
         * exactly like a right arrow. */
        if (ry <= BSP_TOUCH_SCREEN_Y_MAX)
            rx = (rx > BSP_TOUCH_X_MAX) ? 0 : (uint16_t)(BSP_TOUCH_X_MAX - rx);

        /* Same reason for Y: mirrored inside the display, not on the strip.
         * Mirroring the whole height swapped the two worlds — the top of the
         * screen ended up in the strip and vice versa. */
        if (ry <= BSP_TOUCH_SCREEN_Y_MAX)
            ry = (uint16_t)(BSP_TOUCH_SCREEN_Y_MAX - ry);
    }

    if (x) *x = rx;
    if (y) *y = ry;
    return true;
}

/* A real power-off, not a suspend: the PMU is asked to drop the rails.
 *
 * AXP2101 register 0x10 (PMU common config), bit 0 is soft power off. The value
 * is READ and REWRITTEN with only bit 0 raised: writing the whole register to a
 * fixed value would risk switching something else on or off, and on a PMU that
 * is the kind of mistake nobody diagnoses — the device merely misbehaves.
 *
 * [M] Verified on hardware: the CoreS3-SE powers down. Register and bit came
 * from the AXP2101 documentation and from M5Unified, and the test confirmed
 * them.
 *
 * Returns only on failure: on success the chip is off. */
void bsp_power_off(void)
{
    uint8_t v = i2c_r8(s_axp, 0x10);
    i2c_w8(s_axp, 0x10, (uint8_t)(v | 0x01));
}

bool bsp_touch_read(uint16_t *x, uint16_t *y)
{
    int64_t now = esp_timer_get_time();

    if (s_tc_us && (now - s_tc_us) < TOUCH_CACHE_US) {
        if (s_tc_ok) { if (x) *x = s_tc_x; if (y) *y = s_tc_y; }
        return s_tc_ok;
    }

    uint16_t rx = 0, ry = 0;
    bool ok = touch_read_raw(&rx, &ry);
    s_tc_us = now; s_tc_ok = ok; s_tc_x = rx; s_tc_y = ry;

    if (ok) { if (x) *x = rx; if (y) *y = ry; }
    return ok;
}

