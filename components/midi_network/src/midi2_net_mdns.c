/*
 * midi2_net_mdns.c - DNS-SD advertising of the "_midi2._udp" service.
 *
 * Uses the espressif/mdns managed component. We initialise mDNS if needed,
 * publish a service with the UMPEndpointName / ProductInstanceId TXT records
 * (Table 6) and the listening UDP port (SRV). On stop we remove only our
 * service, leaving any mDNS the application may also use intact.
 *
 * SPDX-License-Identifier: MIT
 */
#include <string.h>
#include <ctype.h>

#include "esp_log.h"
#include "esp_err.h"
#include "mdns.h"

#include "midi2_net_proto.h"

static const char *TAG = "midi2_mdns";
static bool s_service_added;

/* Turn a display name into a safe DNS label: lowercase alnum + '-', <=32 chars. */
static void sanitize_hostname(const char *in, char *out, size_t outsz)
{
    size_t j = 0;
    for (size_t i = 0; in[i] && j + 1 < outsz && j < 32; i++) {
        char c = in[i];
        if (isalnum((unsigned char)c)) {
            out[j++] = (char)tolower((unsigned char)c);
        } else if ((c == ' ' || c == '-' || c == '_') && j > 0 && out[j - 1] != '-') {
            out[j++] = '-';
        }
    }
    while (j > 0 && out[j - 1] == '-') j--; /* no trailing dash */
    if (j == 0) {
        const char *fallback = "midi2-host";
        size_t n = strlen(fallback);
        if (n >= outsz) n = outsz - 1;
        memcpy(out, fallback, n);
        j = n;
    }
    out[j] = '\0';
}

esp_err_t midi2_net_mdns_start(const char *hostname, uint16_t port,
                               const char *endpoint_name, const char *product_id)
{
    char host[33];
    if (hostname && hostname[0]) {
        strlcpy(host, hostname, sizeof(host));
    } else {
        sanitize_hostname(endpoint_name, host, sizeof(host));
    }

    /* mdns_init() is safe to call once globally; if the app already started
     * mDNS this still succeeds (returns ESP_OK). */
    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mdns_init failed: %s", esp_err_to_name(err));
        return err;
    }

    mdns_hostname_set(host);

    mdns_txt_item_t txt[] = {
        { MIDI2_NET_TXT_KEY_NAME, (char *)endpoint_name },
        { MIDI2_NET_TXT_KEY_PID,  (char *)product_id },
    };

    /* Service instance name: the display name is fine; clients show the TXT
     * UMPEndpointName regardless (4.2). */
    err = mdns_service_add(endpoint_name, MIDI2_NET_SERVICE_TYPE,
                           MIDI2_NET_SERVICE_PROTO, port,
                           txt, sizeof(txt) / sizeof(txt[0]));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mdns_service_add failed: %s", esp_err_to_name(err));
        return err;
    }
    s_service_added = true;
    ESP_LOGI(TAG, "advertising %s.%s.%s on %s.local:%u",
             endpoint_name, MIDI2_NET_SERVICE_TYPE, MIDI2_NET_SERVICE_PROTO, host, port);
    return ESP_OK;
}

void midi2_net_mdns_stop(void)
{
    if (s_service_added) {
        mdns_service_remove(MIDI2_NET_SERVICE_TYPE, MIDI2_NET_SERVICE_PROTO);
        s_service_added = false;
    }
    /* Intentionally do not call mdns_free(): the application may rely on mDNS. */
}
