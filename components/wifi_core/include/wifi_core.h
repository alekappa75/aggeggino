/* ============================================================================
 * wifi_core.h - global WiFi initialization, owned in ONE place.
 *
 * The WiFi subsystem needs a set of one-time, mode-independent init steps
 * (NVS, netif, default event loop, esp_wifi_init, power-save policy). Neither
 * the AP nor the client component should own these - they belong here, so that
 * either mode can start without depending on the other having run first.
 *
 * Call wifi_core_init() once at boot, before choosing AP or client mode.
 * It is idempotent: calling it again is a no-op.
 * ==========================================================================*/
#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One-time global WiFi init (NVS, netif, event loop, esp_wifi_init, PS=NONE).
 * Idempotent. Must be called before wifi_ap_start() or wifi_client_start(). */
esp_err_t wifi_core_init(void);

/* Apply a TX power level (0=LOW, 1=MID, 2=HIGH). Safe to call anytime after
 * init; applies to both AP and client mode. */
void wifi_core_set_power(int level);

/* Read the TX power actually set in the radio, in dBm (rounded). Returns the
 * real value from the driver, which may differ from what we requested if it
 * was clamped to hardware limits. Returns 0 if unavailable. */
int wifi_core_get_power_dbm(void);

/* ---- scan (progressive, channel by channel, mode-independent) ------------
 * Scanning touches the radio mode (APSTA), a global WiFi resource - so it lives
 * here, not in wifi_ap or wifi_client. It works in either mode: it temporarily
 * enables APSTA for the duration of the scan and restores the previous mode
 * afterwards. Note: the single radio leaves the operating channel while
 * sweeping, so the AP (or the STA link) sees brief gaps during a scan - the
 * impact on Ableton Link is to be assessed in the field.
 * -------------------------------------------------------------------------- */

/* one scanned network (deduplicated by SSID, best RSSI kept) */
typedef struct {
    char    ssid[33];
    int8_t  rssi;
    uint8_t channel;
    bool    secured;                /* quick check: anything but an open network */
    uint8_t authmode;               /* wifi_auth_mode_t: drives key length rules */
} wifi_core_ap_t;

/* Start a progressive scan (channel by channel) in a task. Returns immediately.
 * Poll the state via wifi_core_scanning() and read results repeatedly to watch
 * the list grow. No-op if a scan is already running. */
esp_err_t wifi_core_scan_start(void);

/* true while a scan is in progress. */
bool wifi_core_scanning(void);

/* Copy up to max_out accumulated results into out[], sorted by RSSI (best
 * first). Returns the number written. Safe during or after a scan. */
int wifi_core_scan_get_results(wifi_core_ap_t *out, int max_out);

#ifdef __cplusplus
}
#endif
