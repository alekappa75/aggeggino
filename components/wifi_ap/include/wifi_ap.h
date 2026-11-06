/* ============================================================================
 * wifi_ap.h - SoftAP mode: create the bridge's own WiFi network.
 *
 * One of the two WiFi modes (sibling of wifi_client). Creates the "aggeggino"
 * network on 172.16.42.1/24 that other devices join. Global WiFi init is done
 * by wifi_core - call wifi_core_init() before wifi_ap_start().
 * ==========================================================================*/
#pragma once
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Start SoftAP mode (netif AP, custom IP, DHCP, config, start).
 * Requires wifi_core_init() to have run. */
esp_err_t wifi_ap_start(void);

/* Stop SoftAP mode (for the hot switch to client). Safe if not started. */
void wifi_ap_stop(void);

/* Number of stations currently associated to our SoftAP. */
int wifi_ap_sta_count(void);

#ifdef __cplusplus
}
#endif
