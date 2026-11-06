/* ============================================================================
 * wifi_client.c - WiFi station mode. See wifi_client.h for the contract.
 *
 * Handles connection + retry + state reporting for STA mode. Scanning is NOT
 * here: it lives in wifi_core (mode-independent, via temporary APSTA), because
 * scanning touches the global radio mode and must work from AP mode too.
 *
 *  - Retry: on connection failure or a dropped link we enter RETRYING and a
 *    timer re-attempts every RETRY_INTERVAL_MS.
 *  - No credential storage here: connect() receives ssid/password and uses them.
 * ==========================================================================*/
#include "wifi_client.h"
#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "wifi_client";

/* ---- tunables ---- */
#define RETRY_INTERVAL_MS   5000    /* wait between connection attempts (field-tune) */

/* ---- state (protected by s_lock) ---- */
static SemaphoreHandle_t   s_lock;
static wifi_client_state_t s_state = WIFI_CLIENT_IDLE;
static wifi_client_fail_t  s_last_fail = WIFI_FAIL_NONE;  /* last attempt's outcome */
static esp_netif_t        *s_netif = NULL;
static bool                s_started = false;

/* current target credentials (for connect + retry) */
static char s_ssid[33];
static char s_pass[65];
static bool s_have_target = false;

/* retry timer */
static esp_timer_handle_t s_retry_timer = NULL;

/* event handler registration handles */
static esp_event_handler_instance_t s_h_wifi = NULL;
static esp_event_handler_instance_t s_h_ip   = NULL;

static void set_state(wifi_client_state_t st)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_state = st;
    xSemaphoreGive(s_lock);
}

/* ---- retry / connection ---- */
static void do_connect_attempt(void)
{
    if (!s_have_target) return;
    wifi_config_t wc;
    memset(&wc, 0, sizeof(wc));
    strncpy((char *)wc.sta.ssid, s_ssid, sizeof(wc.sta.ssid) - 1);
    strncpy((char *)wc.sta.password, s_pass, sizeof(wc.sta.password) - 1);
    wc.sta.threshold.authmode = (s_pass[0] != 0) ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    set_state(WIFI_CLIENT_CONNECTING);
    ESP_LOGI(TAG, "connecting to '%s'...", s_ssid);
    esp_wifi_connect();
}

static void retry_timer_cb(void *arg)
{
    (void)arg;
    if (s_have_target) {
        ESP_LOGI(TAG, "retry: attempting reconnect");
        do_connect_attempt();
    }
}

static void schedule_retry(void)
{
    set_state(WIFI_CLIENT_RETRYING);
    if (s_retry_timer) {
        esp_timer_stop(s_retry_timer);
        esp_timer_start_once(s_retry_timer, (uint64_t)RETRY_INTERVAL_MS * 1000);
    }
}

/* ---- event handlers ---- */

/* Classifies the disconnection reason reported by esp_wifi. The codes that
 * indicate a refusal during authentication/handshake (wrong key) must be kept
 * apart from "AP not found": the first is a configuration error, the second an
 * environmental condition. */
static wifi_client_fail_t classify_reason(uint8_t reason)
{
    switch (reason) {
        case WIFI_REASON_NO_AP_FOUND:
            return WIFI_FAIL_NO_AP;
        case WIFI_REASON_AUTH_EXPIRE:
        case WIFI_REASON_AUTH_FAIL:
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        case WIFI_REASON_HANDSHAKE_TIMEOUT:
            return WIFI_FAIL_AUTH;
        default:
            return WIFI_FAIL_OTHER;
    }
}

static void on_wifi_evt(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base;
    if (id == WIFI_EVENT_STA_START) {
        if (s_have_target) do_connect_attempt();
    } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *e = (wifi_event_sta_disconnected_t *)data;
        uint8_t reason = e ? e->reason : 0;
        if (s_have_target) {
            s_last_fail = classify_reason(reason);
            ESP_LOGW(TAG, "disconnected (reason %u -> %s); will retry in %d ms",
                     reason,
                     s_last_fail == WIFI_FAIL_NO_AP ? "network not found" :
                     s_last_fail == WIFI_FAIL_AUTH  ? "rejected (key?)"   : "other",
                     RETRY_INTERVAL_MS);
            set_state(WIFI_CLIENT_FAILED);
            schedule_retry();
        } else {
            s_last_fail = WIFI_FAIL_NONE;
            set_state(WIFI_CLIENT_IDLE);
        }
    }
}

static void on_ip_evt(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base;
    if (id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "got IP " IPSTR, IP2STR(&e->ip_info.ip));
        s_last_fail = WIFI_FAIL_NONE;      /* connected: no failure pending */
        set_state(WIFI_CLIENT_CONNECTED);
    }
}

/* ============================================================================
 * PUBLIC API
 * ==========================================================================*/
esp_err_t wifi_client_start(void)
{
    if (s_started) return ESP_OK;
    if (!s_lock) { s_lock = xSemaphoreCreateMutex(); if (!s_lock) return ESP_ERR_NO_MEM; }

    /* Global WiFi init (netif, event loop, esp_wifi_init, PS) is done by
     * wifi_core_init(). Here we only add the STA netif and handlers. */
    s_netif = esp_netif_create_default_wifi_sta();
    if (!s_netif) return ESP_FAIL;

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_evt, NULL, &s_h_wifi));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, on_ip_evt, NULL, &s_h_ip));

    if (!s_retry_timer) {
        esp_timer_create_args_t ta = { .callback = retry_timer_cb, .name = "wifi_retry" };
        ESP_ERROR_CHECK(esp_timer_create(&ta, &s_retry_timer));
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    s_started = true;
    set_state(WIFI_CLIENT_IDLE);
    ESP_LOGI(TAG, "STA started");
    return ESP_OK;
}

void wifi_client_stop(void)
{
    if (!s_started) return;
    s_have_target = false;
    if (s_retry_timer) esp_timer_stop(s_retry_timer);
    esp_wifi_disconnect();
    esp_wifi_stop();
    if (s_h_wifi) { esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, s_h_wifi); s_h_wifi = NULL; }
    if (s_h_ip)   { esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, s_h_ip); s_h_ip = NULL; }
    if (s_netif)  { esp_netif_destroy_default_wifi(s_netif); s_netif = NULL; }
    s_started = false;
    set_state(WIFI_CLIENT_IDLE);
    ESP_LOGI(TAG, "STA stopped");
}

esp_err_t wifi_client_connect(const char *ssid, const char *password)
{
    if (!ssid || !ssid[0]) return ESP_ERR_INVALID_ARG;
    strncpy(s_ssid, ssid, sizeof(s_ssid) - 1); s_ssid[sizeof(s_ssid) - 1] = 0;
    if (password) { strncpy(s_pass, password, sizeof(s_pass) - 1); s_pass[sizeof(s_pass) - 1] = 0; }
    else s_pass[0] = 0;
    s_have_target = true;
    s_last_fail   = WIFI_FAIL_NONE;   /* new attempt: clear the previous outcome */
    if (s_retry_timer) esp_timer_stop(s_retry_timer);
    do_connect_attempt();
    return ESP_OK;
}

void wifi_client_disconnect(void)
{
    s_have_target = false;
    if (s_retry_timer) esp_timer_stop(s_retry_timer);
    esp_wifi_disconnect();
    set_state(WIFI_CLIENT_IDLE);
}

wifi_client_state_t wifi_client_state(void)
{
    /* safe before wifi_client_start(): s_lock may not exist yet. s_state is a
     * plain enum initialized to IDLE, so reading it without the lock is fine. */
    if (!s_lock) return s_state;
    wifi_client_state_t st;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    st = s_state;
    xSemaphoreGive(s_lock);
    return st;
}

bool wifi_client_started(void)
{
    return s_started;
}

wifi_client_fail_t wifi_client_last_fail(void)
{
    return s_last_fail;
}

bool wifi_client_get_ip(char *out, int out_size)
{
    if (out && out_size > 0) out[0] = 0;
    if (!s_netif) return false;
    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(s_netif, &ip) != ESP_OK) return false;
    if (ip.ip.addr == 0) return false;
    if (out && out_size >= 16) snprintf(out, out_size, IPSTR, IP2STR(&ip.ip));
    return true;
}

int wifi_client_get_rssi(void)
{
    /* An RSSI reading is only meaningful once we're actually associated with an
     * IP. During the association attempt esp_wifi_sta_get_ap_info() can already
     * return OK with a populated rssi, which would show phantom signal bars
     * before the connection is up. */
    if (wifi_client_state() != WIFI_CLIENT_CONNECTED) return 0;
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) return ap.rssi;
    return 0;
}

void wifi_client_get_ssid(char *out, int out_size)
{
    if (!out || out_size <= 0) return;
    out[0] = 0;
    if (s_have_target) {
        strncpy(out, s_ssid, out_size - 1);
        out[out_size - 1] = 0;
    }
}
