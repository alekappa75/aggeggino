/* ============================================================================
 * wifi_ap.c - SoftAP mode. See wifi_ap.h.
 *
 * Renamed from the old "wifiap". Global WiFi init (NVS, netif, event loop,
 * esp_wifi_init, PS) now lives in wifi_core; this component only does the
 * AP-specific part: AP netif, custom IP 172.16.42.1/24, DHCP pool, config,
 * set_mode(AP), start. Adds wifi_ap_stop() for the hot switch to client mode.
 * ==========================================================================*/
#include "wifi_ap.h"
#include <string.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "dhcpserver/dhcpserver.h"
#include "settings.h"

static const char *TAG = "wifi_ap";

/* --- SoftAP config --- */
#define AP_SSID     "aggeggino"
#define AP_PASS     "m5stack1"   /* WPA2 needs >= 8 chars */
#define AP_CHANNEL  6            /* 2.4 GHz */
#define AP_MAX_CONN 4

static volatile int s_sta_count = 0;
static esp_netif_t *s_ap_netif = NULL;
static esp_event_handler_instance_t s_h_wifi = NULL;
static bool s_started = false;

int wifi_ap_sta_count(void) { return s_sta_count; }

static void on_wifi_evt(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base;
    if (id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *e = (wifi_event_ap_staconnected_t *)data;
        s_sta_count++;
        ESP_LOGI(TAG, "STA connected " MACSTR " aid=%d (tot=%d)",
                 MAC2STR(e->mac), e->aid, s_sta_count);
    } else if (id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t *e = (wifi_event_ap_stadisconnected_t *)data;
        if (s_sta_count > 0) s_sta_count--;
        ESP_LOGI(TAG, "STA disconnected " MACSTR " aid=%d (tot=%d)",
                 MAC2STR(e->mac), e->aid, s_sta_count);
    }
}

esp_err_t wifi_ap_start(void)
{
    if (s_started) return ESP_OK;

    s_ap_netif = esp_netif_create_default_wifi_ap();

    /* --- custom IP 172.16.42.1/24 + DHCP pool .100-.150 --- */
    ESP_ERROR_CHECK(esp_netif_dhcps_stop(s_ap_netif));
    esp_netif_ip_info_t ipinfo;
    memset(&ipinfo, 0, sizeof(ipinfo));
    IP4_ADDR(&ipinfo.ip,      172, 16, 42, 1);
    IP4_ADDR(&ipinfo.gw,      172, 16, 42, 1);
    IP4_ADDR(&ipinfo.netmask, 255, 255, 255, 0);
    ESP_ERROR_CHECK(esp_netif_set_ip_info(s_ap_netif, &ipinfo));

    dhcps_lease_t lease;
    memset(&lease, 0, sizeof(lease));
    lease.enable = true;
    IP4_ADDR(&lease.start_ip, 172, 16, 42, 100);
    IP4_ADDR(&lease.end_ip,   172, 16, 42, 150);
    ESP_ERROR_CHECK(esp_netif_dhcps_option(s_ap_netif, ESP_NETIF_OP_SET,
                    ESP_NETIF_REQUESTED_IP_ADDRESS, &lease, sizeof(lease)));
    ESP_ERROR_CHECK(esp_netif_dhcps_start(s_ap_netif));

    /* --- WiFi AP config --- */
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_evt, NULL, &s_h_wifi));

    wifi_config_t wc;
    memset(&wc, 0, sizeof(wc));
    /* name/password come from settings (which fall back to the compiled
     * defaults when unset), so the AP config screen actually takes effect. */
    char ap_name[33], ap_pass[65];
    settings_get_ap_name(ap_name, sizeof(ap_name));
    settings_get_ap_password(ap_pass, sizeof(ap_pass));
    /* SSID: copy exactly as many bytes as we declare in ssid_len. The field is
     * 32 bytes and is NOT null-terminated when full, so memcpy + explicit length
     * is correct here (strncpy with sizeof-1 would copy 31 while ssid_len said
     * 32, leaving a stray byte in the beacon). */
    size_t nlen = strlen(ap_name);
    if (nlen > sizeof(wc.ap.ssid)) nlen = sizeof(wc.ap.ssid);   /* 32 max */
    memcpy(wc.ap.ssid, ap_name, nlen);
    wc.ap.ssid_len = nlen;
    strncpy((char *)wc.ap.password, ap_pass, sizeof(wc.ap.password) - 1);
    wc.ap.channel        = AP_CHANNEL;
    wc.ap.max_connection = AP_MAX_CONN;
    wc.ap.authmode       = WIFI_AUTH_WPA2_PSK;
    wc.ap.pmf_cfg.required = false;
    /* client isolation OFF: needed for Ableton Link unicast between stations. */

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());

    s_started = true;
    ESP_LOGI(TAG, "SoftAP '%s' up on 172.16.42.1/24 (DHCP .100-.150, ch %d, WPA2)",
             ap_name, AP_CHANNEL);
    return ESP_OK;
}

void wifi_ap_stop(void)
{
    if (!s_started) return;
    esp_wifi_stop();
    if (s_h_wifi) {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, s_h_wifi);
        s_h_wifi = NULL;
    }
    if (s_ap_netif) {
        esp_netif_destroy_default_wifi(s_ap_netif);
        s_ap_netif = NULL;
    }
    s_sta_count = 0;
    s_started = false;
    ESP_LOGI(TAG, "SoftAP stopped");
}
