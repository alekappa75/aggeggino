/* ============================================================================
 * wifi_client.h - WiFi station (STA) mode: connect, retry, report state.
 *
 * Sibling of the "wifi_ap" component (SoftAP). This one JOINS an existing WiFi
 * network. The two are coordinated from above; only one WiFi mode is active at
 * a time. Global WiFi init is owned by wifi_core.
 *
 * SCANNING is NOT here - it lives in wifi_core (mode-independent, works from AP
 * too). This component only connects, retries, and reports STA state.
 *
 * RESPONSIBILITY BOUNDARY: WiFi only - it does NOT store credentials.
 * Persistence is the job of the "settings" component. The caller reads the
 * saved SSID/password from settings and passes them to wifi_client_connect().
 *
 * All strings/identifiers in English. See wifi_client.c for the implementation.
 * ==========================================================================*/
#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- state, for UI and dashboard ---- */
typedef enum {
    WIFI_CLIENT_IDLE = 0,     /* started but not connecting */
    WIFI_CLIENT_CONNECTING,   /* association/DHCP in progress */
    WIFI_CLIENT_CONNECTED,    /* associated, got an IP */
    WIFI_CLIENT_FAILED,       /* last attempt failed (will retry) */
    WIFI_CLIENT_RETRYING,     /* waiting before the next attempt */
} wifi_client_state_t;

/* Why the last connection attempt failed. Telling "the network is not there"
 * from "the network is there but refuses me" changes what to do: in the first
 * case you wait or switch network, in the second the key is wrong. */
typedef enum {
    WIFI_FAIL_NONE = 0,   /* no failure recorded (or connected)           */
    WIFI_FAIL_NO_AP,      /* configured network not found                 */
    WIFI_FAIL_AUTH,       /* network found but attempt refused (key)      */
    WIFI_FAIL_OTHER,      /* some other reason                           */
} wifi_client_fail_t;

/* Outcome of the last attempt. Cleared on every new connection requestnessione
 * and when the connection succeeds. */
wifi_client_fail_t wifi_client_last_fail(void);

/* ---- lifecycle ---- */

/* Initialize the STA side (netif + event handlers). Does NOT connect yet.
 * Requires wifi_core_init() to have run. */
esp_err_t wifi_client_start(void);

/* Tear down the STA side (disconnect, stop, delete netif). For the hot switch
 * back to SoftAP. Safe to call even if not connected. */
void wifi_client_stop(void);

/* ---- connection ---- */

/* Start connecting to a network with the given credentials. Credentials are
 * used, NOT stored (persistence belongs to "settings"). If it fails, the
 * component enters RETRYING and retries on its own every few seconds.
 * password may be NULL/empty for an open network. */
esp_err_t wifi_client_connect(const char *ssid, const char *password);

/* Stop connecting / disconnect, but keep the STA side up (state -> IDLE). */
void wifi_client_disconnect(void);

/* ---- state / reporting ---- */

wifi_client_state_t wifi_client_state(void);

/* true if the STA side has been started (wifi_client_start succeeded). Lets the
 * caller tell "not started / init error" from "started but not connected". */
bool wifi_client_started(void);

/* Copy the current IPv4 address as a string ("192.168.1.42") into out (size
 * >= 16). Empty string if not connected. Returns true if connected. */
bool wifi_client_get_ip(char *out, int out_size);

/* Copy the current target SSID (the network we're connected to or trying to
 * connect to) into out (size >= 33). Empty string if none set. */
void wifi_client_get_ssid(char *out, int out_size);

/* Current RSSI in dBm (0 if not connected). */
int wifi_client_get_rssi(void);

#ifdef __cplusplus
}
#endif
