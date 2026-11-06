/* ============================================================================
 * wifi_core.c - global WiFi initialization. See wifi_core.h.
 * ==========================================================================*/
#include "wifi_core.h"
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "wifi_core";
static bool s_inited = false;

esp_err_t wifi_core_init(void)
{
    if (s_inited) return ESP_OK;   /* idempotent */

    /* NVS (required by the WiFi stack) */
    esp_err_t r = nvs_flash_init();
    if (r == ESP_ERR_NVS_NO_FREE_PAGES || r == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    /* No power-save: latency matters for Ableton Link. Applies to both modes. */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    s_inited = true;
    ESP_LOGI(TAG, "WiFi core initialized");
    return ESP_OK;
}

void wifi_core_set_power(int level)
{
    /* esp_wifi_set_max_tx_power() takes power in 0.25 dBm units.
     * Valid range on ESP32-S3 is roughly 8 (2 dBm) .. 84 (21 dBm).
     * Map the three UI levels to sensible points: */
    int8_t p;
    switch (level) {
        case 0:  p = 34; break;   /* LOW  ~8.5 dBm */
        case 2:  p = 80; break;   /* HIGH ~20 dBm */
        case 1:
        default: p = 60; break;   /* MID  ~15 dBm (default) */
    }
    esp_err_t e = esp_wifi_set_max_tx_power(p);
    if (e != ESP_OK)
        ESP_LOGW(TAG, "set_max_tx_power(%d) failed: %s", p, esp_err_to_name(e));
    else
        ESP_LOGI(TAG, "WiFi TX power level %d -> %d (0.25dBm units)", level, p);
}

int wifi_core_get_power_dbm(void)
{
    int8_t p = 0;   /* in 0.25 dBm units */
    if (esp_wifi_get_max_tx_power(&p) != ESP_OK || p <= 0)
        return 0;
    /* convert 0.25 dBm units to dBm, rounded to nearest */
    return (p + 2) / 4;
}

/* ============================================================================
 * PROGRESSIVE SCAN (mode-independent, via temporary APSTA)
 * ==========================================================================*/
#define SCAN_FIRST_CH   1
#define SCAN_LAST_CH    13
#define SCAN_PER_CH_MS  120
#define SCAN_MAX_APS    24

static SemaphoreHandle_t s_scan_lock;
static wifi_core_ap_t    s_aps[SCAN_MAX_APS];
static int               s_ap_count = 0;
static TaskHandle_t      s_scan_task = NULL;
static volatile bool     s_scanning = false;

static void acc_add(const wifi_ap_record_t *r)
{
    if (r->ssid[0] == 0) return;   /* skip hidden/empty */
    xSemaphoreTake(s_scan_lock, portMAX_DELAY);
    for (int i = 0; i < s_ap_count; i++) {
        if (strncmp(s_aps[i].ssid, (const char *)r->ssid, 32) == 0) {
            if (r->rssi > s_aps[i].rssi) {
                s_aps[i].rssi    = r->rssi;
                s_aps[i].channel = r->primary;
            }
            xSemaphoreGive(s_scan_lock);
            return;
        }
    }
    if (s_ap_count < SCAN_MAX_APS) {
        wifi_core_ap_t *a = &s_aps[s_ap_count++];
        strncpy(a->ssid, (const char *)r->ssid, 32);
        a->ssid[32] = 0;
        a->rssi     = r->rssi;
        a->channel  = r->primary;
        a->secured  = (r->authmode != WIFI_AUTH_OPEN);
        a->authmode = (uint8_t)r->authmode;
    }
    xSemaphoreGive(s_scan_lock);
}

static void scan_task(void *arg)
{
    (void)arg;

    /* remember current mode; ensure STA is present for scanning (APSTA) */
    wifi_mode_t prev_mode = WIFI_MODE_NULL;
    esp_wifi_get_mode(&prev_mode);
    wifi_mode_t scan_mode = prev_mode;
    if (prev_mode == WIFI_MODE_AP)   scan_mode = WIFI_MODE_APSTA;
    else if (prev_mode == WIFI_MODE_NULL) scan_mode = WIFI_MODE_STA;
    if (scan_mode != prev_mode) esp_wifi_set_mode(scan_mode);

    ESP_LOGI(TAG, "scan: start (channels %d..%d, mode %d)",
             SCAN_FIRST_CH, SCAN_LAST_CH, scan_mode);

    xSemaphoreTake(s_scan_lock, portMAX_DELAY);
    s_ap_count = 0;
    xSemaphoreGive(s_scan_lock);

    for (int ch = SCAN_FIRST_CH; ch <= SCAN_LAST_CH; ch++) {
        wifi_scan_config_t sc = {
            .ssid = NULL, .bssid = NULL,
            .channel = ch,
            .show_hidden = false,
            .scan_type = WIFI_SCAN_TYPE_ACTIVE,
            .scan_time.active = { .min = 60, .max = SCAN_PER_CH_MS },
        };
        if (esp_wifi_scan_start(&sc, true) != ESP_OK) continue;
        uint16_t n = 0;
        esp_wifi_scan_get_ap_num(&n);
        if (n > 0) {
            wifi_ap_record_t *recs = malloc(n * sizeof(wifi_ap_record_t));
            if (recs) {
                if (esp_wifi_scan_get_ap_records(&n, recs) == ESP_OK) {
                    for (uint16_t i = 0; i < n; i++) acc_add(&recs[i]);
                }
                free(recs);
            }
        }
    }

    /* restore the previous mode */
    if (scan_mode != prev_mode) esp_wifi_set_mode(prev_mode);

    ESP_LOGI(TAG, "scan: done (%d networks)", s_ap_count);
    s_scanning = false;
    s_scan_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t wifi_core_scan_start(void)
{
    if (!s_inited) return ESP_ERR_INVALID_STATE;
    if (s_scanning) return ESP_OK;
    if (!s_scan_lock) { s_scan_lock = xSemaphoreCreateMutex(); if (!s_scan_lock) return ESP_ERR_NO_MEM; }
    s_scanning = true;
    if (xTaskCreate(scan_task, "wifi_scan", 4096, NULL, 4, &s_scan_task) != pdPASS) {
        s_scanning = false;
        return ESP_FAIL;
    }
    return ESP_OK;
}

bool wifi_core_scanning(void) { return s_scanning; }

static int cmp_rssi(const void *a, const void *b)
{
    const wifi_core_ap_t *x = a, *y = b;
    return (y->rssi - x->rssi);
}

int wifi_core_scan_get_results(wifi_core_ap_t *out, int max_out)
{
    if (!out || max_out <= 0 || !s_scan_lock) return 0;
    xSemaphoreTake(s_scan_lock, portMAX_DELAY);
    int n = s_ap_count < max_out ? s_ap_count : max_out;
    memcpy(out, s_aps, n * sizeof(wifi_core_ap_t));
    xSemaphoreGive(s_scan_lock);
    qsort(out, n, sizeof(wifi_core_ap_t), cmp_rssi);
    return n;
}
