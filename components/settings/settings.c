#include "settings.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

static const char *TAG = "settings";
static const char *NS  = "aggeggino";
static const char *K_PHASE = "phase_us";      /* offset, source DJ Link  */
static const char *K_PHASE_R = "phase_r_us";  /* offset, source Ableton  */
static const char *K_WIFI_MODE = "wifi_mode";
static const char *K_WIFI_SSID = "wifi_ssid";
static const char *K_WIFI_PASS = "wifi_pass";
static const char *K_WIFI_PWR  = "wifi_pwr";
static const char *K_AP_NAME   = "ap_name";
static const char *K_AP_PASS   = "ap_pass";
static const char *K_AUDIO     = "audio_on";
static const char *K_DISP_BR   = "disp_br";
static const char *K_LED_BR    = "led_br";
static const char *K_STARTSYNC = "startsync";
static const char *K_STOPSYNC  = "stopsync";
static const char *K_DRIFTCOMP = "driftcomp";
static const char *K_SDPLAYLIST = "sd_playlist";
static const char *K_SDTHRESH   = "sd_thresh";
static const char *K_SHOTS      = "shots";
static const char *K_OSC_CUST   = "osc_cust";
static const char *K_MAGIC     = "magic";
/* OSC output. The address is a string because it is shown and edited as one;
 * port and preset fit in small integers. */
static const char *K_OSC_EN    = "osc_en";
/* MIDI. Short keys because NVS limits key names to 15 characters. */
static const char *K_MIDI_NET_EN = "mnet_en";     /* MIDI over network       */
static const char *K_MIDI_MDNS   = "mnet_mdns";   /* _midi2._udp announcement */
static const char *K_MIDI_DIN_EN = "mdin_en";      /* MIDI over DIN           */
static const char *K_MIDI_USB_EN = "musb_en";      /* MIDI over USB-C         */
static const char *K_REMEMBER   = "remember";      /* remember bridge state   */
/* NVS KEY NAMES ARE FROZEN. They still carry the old forward/reverse wording,
 * and must NOT be renamed: the key IS the identity of the stored value, so
 * changing it would silently lose the settings on every device already in the
 * field. The C names around them follow the current lexicon. */
static const char *K_BR_FWD     = "br_fwd";        /* source = DJ Link        */
static const char *K_BR_REV     = "br_rev";        /* source = Ableton        */
static const char *K_DJL_OBS    = "djl_obs";       /* observer only, 7..15    */
static const char *K_SCR_FLIP   = "scr_flip";      /* screen rotated 180      */
static const char *K_CV_EN      = "cv_en";         /* CV output               */
static const char *K_CV_SOUND   = "cv_sound";      /* sound index             */
static const char *K_CV_WIDTH   = "cv_width";      /* pulse width index       */
static const char *K_CV_POL     = "cv_pol";        /* 1 = idle high           */
static const char *K_CV_PPQN    = "cv_ppqn";       /* division index          */
static const char *K_CV_OFF_DJ  = "cv_off_dj";     /* CV offset, DJ source    */
static const char *K_CV_OFF_LK  = "cv_off_lk";     /* CV offset, Link source  */
static const char *K_MIDI_OFF_DJ = "midi_off_dj"; /* phase offset, DJ source */
static const char *K_MIDI_OFF_LK = "midi_off_lk"; /* phase offset, Link src  */
static const char *K_OSC_HOST  = "osc_host";
static const char *K_OSC_PORT  = "osc_port";
static const char *K_OSC_PRESET= "osc_preset";
/* OSC output offset, ONE PER SOURCE: the delay to compensate depends both on
 * the chain downstream and on who provides the clock, so moving from one
 * bridge direction to the other changes which value is right. Keeping two
 * avoids redoing by ear an adjustment already made. */
static const char *K_OSC_OFF_DJ = "osc_off_dj";   /* Pioneer DJ Link source   */
static const char *K_OSC_OFF_LK = "osc_off_lk";   /* Ableton Link source      */

/* NVS layout magic: identifies our settings block and its version. Bump the
 * low byte when the default set or layout changes to force a clean re-init on
 * the next boot. 'DJ' + version. */
/* v8: the order of the OSC preset enum changed, so an already stored value
 * would select a different preset than the one chosen. The bump forces the
 * defaults to be rewritten. */
#define SETTINGS_MAGIC 0x444A0008u   /* "DJ" + v8, OSC preset order */

static nvs_handle_t s_h;
static bool s_ok = false;

/* Write the full default set to NVS. Called on first boot (or after a magic
 * mismatch). Keeps every default in one place so a fresh device is fully and
 * explicitly initialized, then stamps the magic last so a power loss midway
 * doesn't leave a half-initialized block marked as valid. */
static void settings_write_defaults(void)
{
    nvs_set_i32(s_h, K_PHASE,      0);
    nvs_set_i32(s_h, K_PHASE_R,    0);
    nvs_set_u8 (s_h, K_WIFI_MODE,  WIFI_MODE_PREF_AP);
    nvs_set_u8 (s_h, K_OSC_EN,     0);              /* OSC output off */
    /* EVERY output starts OFF, like OSC. One rule for all of them: an output
     * comes on because somebody asked for it, never by itself. That holds even
     * for the ones with nothing to configure — consistency between menus is
     * worth more than saving a tap. */
    nvs_set_u8 (s_h, K_MIDI_NET_EN, 0);
    /* The mDNS announcement, by contrast, stays ON: it is not an output but a
     * PARAMETER of the network output, and it only matters once that one is
     * already on. It stands to "Enabled" as Preset, Host and Port stand to the
     * OSC enable — which likewise ship with useful starting values. Off, it
     * would make the endpoint undiscoverable at the very moment it is switched
     * on. */
    nvs_set_u8 (s_h, K_MIDI_MDNS,   1);
    nvs_set_u8 (s_h, K_MIDI_DIN_EN, 0);
    nvs_set_u8 (s_h, K_MIDI_USB_EN, 0);
    nvs_set_u8 (s_h, K_REMEMBER, 1);   /* on: the useful behaviour */
    nvs_set_u8 (s_h, K_BR_FWD,   0);
    nvs_set_u8 (s_h, K_BR_REV,   0);
    nvs_set_u8 (s_h, K_DJL_OBS,  0);
    nvs_set_u8 (s_h, K_SCR_FLIP, 0);
    nvs_set_u8 (s_h, K_CV_EN,    0);
    nvs_set_u8 (s_h, K_CV_SOUND, 0);
    nvs_set_u8 (s_h, K_CV_WIDTH, 3);   /* 10 ms: the most common         */
    nvs_set_u8 (s_h, K_CV_POL,   0);   /* idle low: the convention       */
    nvs_set_u8 (s_h, K_CV_PPQN,  0);   /* 1 PPQN: eurorack convention    */
    nvs_set_i32(s_h, K_CV_OFF_DJ, 0);
    nvs_set_i32(s_h, K_CV_OFF_LK, 0);
    nvs_set_i32(s_h, K_MIDI_OFF_DJ, 0);
    nvs_set_i32(s_h, K_MIDI_OFF_LK, 0);
    nvs_set_str(s_h, K_OSC_HOST,   "172.16.42.100");/* first SoftAP client */
    nvs_set_u16(s_h, K_OSC_PORT,   7000);           /* common outside Resolume
                                                     * too: it is the most
                                                     * widespread OSC port      */
    nvs_set_u8 (s_h, K_OSC_PRESET, OSC_PRESET_GENERIC);
    nvs_set_i32(s_h, K_OSC_OFF_DJ, 0);
    nvs_set_i32(s_h, K_OSC_OFF_LK, 0);
    nvs_set_str(s_h, K_WIFI_SSID,  "");
    nvs_set_str(s_h, K_WIFI_PASS,  "");
    nvs_set_u8 (s_h, K_WIFI_PWR,   WIFI_POWER_MID);
    nvs_set_str(s_h, K_AP_NAME,    "aggeggino");
    nvs_set_str(s_h, K_AP_PASS,    "m5stack1");
    nvs_set_u8 (s_h, K_AUDIO,      1);            /* sounds on */
    nvs_set_u8 (s_h, K_DISP_BR,    BRIGHT_HIGH);   /* display: max, as at boot */
    nvs_set_u8 (s_h, K_LED_BR,     LED_BRIGHT_MED);
    nvs_set_u8 (s_h, K_STARTSYNC,  0);            /* Start Sync: off */
    nvs_set_u8 (s_h, K_STOPSYNC,   0);            /* Stop Sync: off  */
    nvs_set_u8 (s_h, K_DRIFTCOMP,  0);            /* Drift Compensation: off */
    nvs_set_u8 (s_h, K_SDPLAYLIST, 0);            /* playlist on SD: off     */
    nvs_set_u8 (s_h, K_SDTHRESH,  60);            /* played for a minute     */
    nvs_set_u8 (s_h, K_SHOTS,      0);            /* screenshots: off        */
    nvs_set_u32(s_h, K_MAGIC,      SETTINGS_MAGIC);   /* stamp last */
    nvs_commit(s_h);
    ESP_LOGI(TAG, "NVS defaults written (magic 0x%08X)", (unsigned)SETTINGS_MAGIC);
}

esp_err_t settings_init(void)
{
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS needs reformatting");
        nvs_flash_erase();
        e = nvs_flash_init();
    }
    if (e != ESP_OK) { ESP_LOGE(TAG, "nvs_flash_init: %s", esp_err_to_name(e)); return e; }

    e = nvs_open(NS, NVS_READWRITE, &s_h);
    if (e != ESP_OK) { ESP_LOGE(TAG, "nvs_open: %s", esp_err_to_name(e)); return e; }
    s_ok = true;

    /* First-boot / new-device detection: if the magic is missing or doesn't
     * match, this NVS wasn't initialized by us (blank device, or an older
     * layout). Write the full default set so every setting has a known value. */
    uint32_t magic = 0;
    if (nvs_get_u32(s_h, K_MAGIC, &magic) != ESP_OK || magic != SETTINGS_MAGIC) {
        ESP_LOGW(TAG, "NVS magic mismatch (got 0x%08X, want 0x%08X): writing defaults",
                 (unsigned)magic, (unsigned)SETTINGS_MAGIC);
        settings_write_defaults();
    }

    ESP_LOGI(TAG, "NVS ready (namespace '%s')", NS);
    return ESP_OK;
}

int settings_get_phase_offset_dj_us(void)
{
    int32_t v = 0;
    if (s_ok) nvs_get_i32(s_h, K_PHASE, &v);   /* if absent: stays 0 */
    return (int)v;
}

void settings_set_phase_offset_dj_us(int us)
{
    if (!s_ok) return;
    if (nvs_set_i32(s_h, K_PHASE, (int32_t)us) == ESP_OK)
        nvs_commit(s_h);
}

int settings_get_phase_offset_abl_us(void)
{
    int32_t v = 0;
    if (s_ok) nvs_get_i32(s_h, K_PHASE_R, &v);   /* if absent: stays 0 */
    return (int)v;
}

void settings_set_phase_offset_abl_us(int us)
{
    if (!s_ok) return;
    if (nvs_set_i32(s_h, K_PHASE_R, (int32_t)us) == ESP_OK)
        nvs_commit(s_h);
}

/* --- WiFi mode ------------------------------------------------------------ */
wifi_mode_pref_t settings_get_wifi_mode(void)
{
    uint8_t v = WIFI_MODE_PREF_AP;   /* default: AP */
    if (s_ok) nvs_get_u8(s_h, K_WIFI_MODE, &v);
    /* fallback: an unknown/corrupt value falls back to the default */
    if (v != WIFI_MODE_PREF_AP && v != WIFI_MODE_PREF_CLIENT)
        v = WIFI_MODE_PREF_AP;
    return (wifi_mode_pref_t)v;
}

void settings_set_wifi_mode(wifi_mode_pref_t m)
{
    if (!s_ok) return;
    if (nvs_set_u8(s_h, K_WIFI_MODE, (uint8_t)m) == ESP_OK)
        nvs_commit(s_h);
}

/* --- WiFi client credentials ---------------------------------------------- */
void settings_get_wifi_ssid(char *out, int out_size)
{
    if (!out || out_size <= 0) return;
    out[0] = 0;
    if (!s_ok) return;
    size_t len = out_size;
    nvs_get_str(s_h, K_WIFI_SSID, out, &len);   /* leaves "" if absent */
}

void settings_set_wifi_ssid(const char *ssid)
{
    if (!s_ok || !ssid) return;
    if (nvs_set_str(s_h, K_WIFI_SSID, ssid) == ESP_OK)
        nvs_commit(s_h);
}

void settings_get_wifi_password(char *out, int out_size)
{
    if (!out || out_size <= 0) return;
    out[0] = 0;
    if (!s_ok) return;
    size_t len = out_size;
    nvs_get_str(s_h, K_WIFI_PASS, out, &len);
}

void settings_set_wifi_password(const char *password)
{
    if (!s_ok || !password) return;
    if (nvs_set_str(s_h, K_WIFI_PASS, password) == ESP_OK)
        nvs_commit(s_h);
}

wifi_power_t settings_get_wifi_power(void)
{
    uint8_t v = WIFI_POWER_MID;   /* default: MID */
    if (s_ok) nvs_get_u8(s_h, K_WIFI_PWR, &v);
    /* fallback: an unknown/corrupt value (e.g. blank NVS or an old build)
     * falls back to the default so the UI and radio never see a bad level */
    if (v > WIFI_POWER_HIGH)
        v = WIFI_POWER_MID;
    return (wifi_power_t)v;
}

void settings_set_wifi_power(wifi_power_t p)
{
    if (!s_ok) return;
    if (nvs_set_u8(s_h, K_WIFI_PWR, (uint8_t)p) == ESP_OK)
        nvs_commit(s_h);
}

/* SoftAP name/password: fall back to the compiled defaults when unset in NVS,
 * so a blank NVS still yields a working AP. */
void settings_get_ap_name(char *out, int out_size)
{
    if (!out || out_size <= 0) return;
    out[0] = 0;
    size_t len = out_size;
    if (!s_ok || nvs_get_str(s_h, K_AP_NAME, out, &len) != ESP_OK || out[0] == 0)
        strncpy(out, "aggeggino", out_size - 1);      /* default AP name */
    out[out_size - 1] = 0;
}

void settings_set_ap_name(const char *name)
{
    if (!s_ok || !name) return;
    if (nvs_set_str(s_h, K_AP_NAME, name) == ESP_OK)
        nvs_commit(s_h);
}

void settings_get_ap_password(char *out, int out_size)
{
    if (!out || out_size <= 0) return;
    out[0] = 0;
    size_t len = out_size;
    if (!s_ok || nvs_get_str(s_h, K_AP_PASS, out, &len) != ESP_OK || out[0] == 0)
        strncpy(out, "m5stack1", out_size - 1);      /* default AP password */
    out[out_size - 1] = 0;
}

void settings_set_ap_password(const char *password)
{
    if (!s_ok || !password) return;
    if (nvs_set_str(s_h, K_AP_PASS, password) == ESP_OK)
        nvs_commit(s_h);
}

bool settings_get_audio_enabled(void)
{
    uint8_t v = 1;   /* default: sounds ON */
    if (s_ok) nvs_get_u8(s_h, K_AUDIO, &v);
    return v != 0;
}

void settings_set_audio_enabled(bool on)
{
    if (!s_ok) return;
    if (nvs_set_u8(s_h, K_AUDIO, on ? 1 : 0) == ESP_OK)
        nvs_commit(s_h);
}

bool settings_get_start_sync(void)
{
    uint8_t v = 0;   /* default: off - must be enabled explicitly, as in Live */
    if (s_ok) nvs_get_u8(s_h, K_STARTSYNC, &v);
    return v != 0;
}

void settings_set_start_sync(bool on)
{
    if (!s_ok) return;
    if (nvs_set_u8(s_h, K_STARTSYNC, on ? 1 : 0) == ESP_OK)
        nvs_commit(s_h);
}

bool settings_get_stop_sync(void)
{
    uint8_t v = 0;   /* default: off */
    if (s_ok) nvs_get_u8(s_h, K_STOPSYNC, &v);
    return v != 0;
}

void settings_set_stop_sync(bool on)
{
    if (!s_ok) return;
    if (nvs_set_u8(s_h, K_STOPSYNC, on ? 1 : 0) == ESP_OK)
        nvs_commit(s_h);
}

/* --- OSC ------------------------------------------------------------------ */

bool settings_get_osc_enabled(void)
{
    uint8_t v = 0;   /* default: off, like every new output */
    if (s_ok) nvs_get_u8(s_h, K_OSC_EN, &v);
    return v != 0;
}

void settings_set_osc_enabled(bool on)
{
    if (!s_ok) return;
    if (nvs_set_u8(s_h, K_OSC_EN, on ? 1 : 0) == ESP_OK) nvs_commit(s_h);
}

void settings_get_osc_host(char *out, size_t len)
{
    if (!out || !len) return;
    out[0] = 0;
    if (!s_ok) return;
    size_t l = len;
    if (nvs_get_str(s_h, K_OSC_HOST, out, &l) != ESP_OK) out[0] = 0;
}

void settings_set_osc_host(const char *host)
{
    if (!s_ok || !host) return;
    if (nvs_set_str(s_h, K_OSC_HOST, host) == ESP_OK) nvs_commit(s_h);
}

uint16_t settings_get_osc_port(void)
{
    uint16_t v = 7000;
    if (s_ok) nvs_get_u16(s_h, K_OSC_PORT, &v);
    return v;
}

void settings_set_osc_port(uint16_t p)
{
    if (!s_ok) return;
    if (nvs_set_u16(s_h, K_OSC_PORT, p) == ESP_OK) nvs_commit(s_h);
}

uint8_t settings_get_osc_preset(void)
{
    uint8_t v = OSC_PRESET_GENERIC;
    if (s_ok) nvs_get_u8(s_h, K_OSC_PRESET, &v);
    return (v < OSC_PRESET_COUNT) ? v : OSC_PRESET_GENERIC;
}

void settings_set_osc_preset(uint8_t p)
{
    if (!s_ok || p >= OSC_PRESET_COUNT) return;
    if (nvs_set_u8(s_h, K_OSC_PRESET, p) == ESP_OK) nvs_commit(s_h);
}

bool settings_get_midi_net_enabled(void)
{
    /* OFF, like every other output. This default matters MORE than the one
     * written in settings_write_defaults(): it is the one that applies to
     * anybody upgrading the firmware without erasing NVS, where the key simply
     * does not exist yet. */
    uint8_t v = 0;
    if (s_ok) nvs_get_u8(s_h, K_MIDI_NET_EN, &v);
    return v != 0;
}

void settings_set_midi_net_enabled(bool on)
{
    if (!s_ok) return;
    if (nvs_set_u8(s_h, K_MIDI_NET_EN, on ? 1 : 0) == ESP_OK) nvs_commit(s_h);
}

bool settings_get_midi_din_enabled(void)
{
    uint8_t v = 0;   /* default OFF: see the comment in settings_defaults */
    if (s_ok) nvs_get_u8(s_h, K_MIDI_DIN_EN, &v);
    return v != 0;
}

void settings_set_midi_din_enabled(bool on)
{
    if (!s_ok) return;
    if (nvs_set_u8(s_h, K_MIDI_DIN_EN, on ? 1 : 0) == ESP_OK) nvs_commit(s_h);
}

bool settings_get_midi_usb_enabled(void)
{
    uint8_t v = 0;   /* off, like every other output */
    if (s_ok) nvs_get_u8(s_h, K_MIDI_USB_EN, &v);
    return v != 0;
}

void settings_set_midi_usb_enabled(bool on)
{
    if (!s_ok) return;
    if (nvs_set_u8(s_h, K_MIDI_USB_EN, on ? 1 : 0) == ESP_OK) nvs_commit(s_h);
}

bool settings_get_midi_net_mdns(void)
{
    uint8_t v = 1;   /* default ON: without the announce nobody finds us */
    if (s_ok) nvs_get_u8(s_h, K_MIDI_MDNS, &v);
    return v != 0;
}

void settings_set_midi_net_mdns(bool on)
{
    if (!s_ok) return;
    if (nvs_set_u8(s_h, K_MIDI_MDNS, on ? 1 : 0) == ESP_OK) nvs_commit(s_h);
}

int settings_get_phase_offset_midi_us(bool src_link)
{
    int32_t v = 0;
    if (s_ok) nvs_get_i32(s_h, src_link ? K_MIDI_OFF_LK : K_MIDI_OFF_DJ, &v);
    return (int)v;
}

void settings_set_phase_offset_midi_us(bool src_link, int us)
{
    if (!s_ok) return;
    if (nvs_set_i32(s_h, src_link ? K_MIDI_OFF_LK : K_MIDI_OFF_DJ,
                    (int32_t)us) == ESP_OK) nvs_commit(s_h);
}

/* Factory reset: the namespace is ERASED rather than having the defaults
 * rewritten one by one.
 *
 * The difference matters. Rewriting the defaults would leave behind the keys
 * we no longer know about — those of removed features, or written by an older
 * firmware — and a "reset" that leaves debris is not a reset. nvs_erase_all()
 * on our namespace removes everything, and on the next boot the magic check
 * rebuilds the complete set.
 *
 * It does NOT touch other NVS namespaces: the WiFi credentials stored by
 * esp_wifi live elsewhere and are not ours to erase. */
void settings_erase_all(void)
{
    if (!s_ok) return;
    nvs_erase_all(s_h);
    nvs_commit(s_h);
}

/* --- REMEMBERING THE BRIDGE STATE ------------------------------------------
 *
 * What it is for: if the device is power-cycled DURING a set — a power switch,
 * a knocked power strip — it comes back as you left it instead of with
 * everything off. In a booth those seconds count.
 *
 * The default is ON because that is the useful behaviour in the very situation
 * the feature exists to cover. Anyone preferring to always start from a
 * standstill switches it off, and then the two states are not even saved.
 *
 * THE TWO STATES ARE SEPARATE from the setting that governs them: switching
 * the memory off does not erase what was stored, it simply stops using it.
 * Switching it back on resumes from there, which is less surprising than
 * finding everything cleared. */
bool settings_get_remember_bridge(void)
{
    uint8_t v = 1;   /* ON when the key is missing: the useful default */
    if (s_ok) nvs_get_u8(s_h, K_REMEMBER, &v);
    return v != 0;
}

void settings_set_remember_bridge(bool on)
{
    if (!s_ok) return;
    if (nvs_set_u8(s_h, K_REMEMBER, on ? 1 : 0) == ESP_OK) nvs_commit(s_h);
}

void settings_get_bridge_state(bool *fwd, bool *rev)
{
    uint8_t f = 0, r = 0;
    if (s_ok) { nvs_get_u8(s_h, K_BR_FWD, &f); nvs_get_u8(s_h, K_BR_REV, &r); }
    if (fwd) *fwd = (f != 0);
    if (rev) *rev = (r != 0);
}

void settings_set_bridge_state(bool fwd, bool rev)
{
    if (!s_ok) return;
    /* A single commit for both values: they are one fact, and writing them
     * separately would expose a mixed state if power were lost in between. */
    nvs_set_u8(s_h, K_BR_FWD, fwd ? 1 : 0);
    nvs_set_u8(s_h, K_BR_REV, rev ? 1 : 0);
    nvs_commit(s_h);
}

/* --- DJ LINK IDENTITY: AUTO or OBSERVER -----------------------------------
 *
 * AUTO      claims a PLAYER number (1..4). Required to be accepted as tempo
 *           master, and therefore for the Ableton Link -> DJ Link direction.
 * OBSERVER  stays on the virtual numbers 7..15. The bridge reads everything
 *           but CANNOT become master, so that direction is unavailable.
 *
 * What OBSERVER is for: in a booth with four real players there are no free
 * numbers, and insisting on claiming one means contending for the channel with
 * a real device. Anyone who only wants to read the tempo — for MIDI, for OSC,
 * for CV — does not need to be a player. */
bool settings_get_djlink_observer(void)
{
    uint8_t v = 0;   /* AUTO: the historical behaviour */
    if (s_ok) nvs_get_u8(s_h, K_DJL_OBS, &v);
    return v != 0;
}

void settings_set_djlink_observer(bool on)
{
    if (!s_ok) return;
    if (nvs_set_u8(s_h, K_DJL_OBS, on ? 1 : 0) == ESP_OK) nvs_commit(s_h);
}

/* --- SCREEN ROTATION -------------------------------------------------------
 *
 * Persisted across reboots.
 *
 * [M] Validated on the device across screen, silkscreened strip and numeric
 * keypad. That validation was the precondition for persisting it at all:
 * rotating the image also rotates touch input, and a wrong correspondence
 * would leave the device unusable with no way back through the menu. */
bool settings_get_screen_flipped(void)
{
    uint8_t v = 0;
    if (s_ok) nvs_get_u8(s_h, K_SCR_FLIP, &v);
    return v != 0;
}

void settings_set_screen_flipped(bool on)
{
    if (!s_ok) return;
    if (nvs_set_u8(s_h, K_SCR_FLIP, on ? 1 : 0) == ESP_OK) nvs_commit(s_h);
}

bool settings_get_cv_enabled(void)
{
    uint8_t v = 0;   /* off, like every other output */
    if (s_ok) nvs_get_u8(s_h, K_CV_EN, &v);
    return v != 0;
}

void settings_set_cv_enabled(bool on)
{
    if (!s_ok) return;
    if (nvs_set_u8(s_h, K_CV_EN, on ? 1 : 0) == ESP_OK) nvs_commit(s_h);
}

/* Index into the sound table, which lives in main. Here it is just a number:
 * settings does not know what it means, and that is right.
 *
 * FUTURE WARNING: if the ORDER of that table changes, an already stored index
 * will select a different sound than the one chosen. It is the same situation
 * that made the magic bump to v8 necessary for the OSC presets. */
uint8_t settings_get_cv_sound(void)
{
    uint8_t v = 0;
    if (s_ok) nvs_get_u8(s_h, K_CV_SOUND, &v);
    return v;
}

void settings_set_cv_sound(uint8_t idx)
{
    if (!s_ok) return;
    if (nvs_set_u8(s_h, K_CV_SOUND, idx) == ESP_OK) nvs_commit(s_h);
}

/* --- CV PULSE PARAMETERS ---------------------------------------------------
 *
 * Three indices into tables that live in main, where their meaning is known.
 * Here they are numbers, as with the sound index.
 *
 * FUTURE WARNING: if the ORDER of those tables changes, a stored index will
 * select a different value. Same situation as the OSC presets. */
uint8_t settings_get_cv_width(void)
{
    uint8_t v = 3;                       /* 10 ms */
    if (s_ok) nvs_get_u8(s_h, K_CV_WIDTH, &v);
    return v;
}
void settings_set_cv_width(uint8_t i)
{
    if (s_ok && nvs_set_u8(s_h, K_CV_WIDTH, i) == ESP_OK) nvs_commit(s_h);
}

bool settings_get_cv_polarity(void)
{
    uint8_t v = 0;                       /* idle low */
    if (s_ok) nvs_get_u8(s_h, K_CV_POL, &v);
    return v != 0;
}
void settings_set_cv_polarity(bool idle_high)
{
    if (s_ok && nvs_set_u8(s_h, K_CV_POL, idle_high ? 1 : 0) == ESP_OK) nvs_commit(s_h);
}

uint8_t settings_get_cv_ppqn(void)
{
    uint8_t v = 0;                       /* 1 PPQN */
    if (s_ok) nvs_get_u8(s_h, K_CV_PPQN, &v);
    return v;
}
void settings_set_cv_ppqn(uint8_t i)
{
    if (s_ok && nvs_set_u8(s_h, K_CV_PPQN, i) == ESP_OK) nvs_commit(s_h);
}

int settings_get_phase_offset_cv_us(bool src_link)
{
    int32_t v = 0;
    if (s_ok) nvs_get_i32(s_h, src_link ? K_CV_OFF_LK : K_CV_OFF_DJ, &v);
    return (int)v;
}

void settings_set_phase_offset_cv_us(bool src_link, int us)
{
    if (!s_ok) return;
    if (nvs_set_i32(s_h, src_link ? K_CV_OFF_LK : K_CV_OFF_DJ,
                    (int32_t)us) == ESP_OK) nvs_commit(s_h);
}

int settings_get_phase_offset_osc_us(bool src_link)
{
    int32_t v = 0;
    if (s_ok) nvs_get_i32(s_h, src_link ? K_OSC_OFF_LK : K_OSC_OFF_DJ, &v);
    return (int)v;
}

void settings_set_phase_offset_osc_us(bool src_link, int us)
{
    if (!s_ok) return;
    if (nvs_set_i32(s_h, src_link ? K_OSC_OFF_LK : K_OSC_OFF_DJ, (int32_t)us) == ESP_OK)
        nvs_commit(s_h);
}

bool settings_get_drift_comp(void)
{
    uint8_t v = 0;   /* default: off — the bridge behaves as it always has
                      * until this is explicitly enabled */
    if (s_ok) nvs_get_u8(s_h, K_DRIFTCOMP, &v);
    return v != 0;
}

void settings_set_drift_comp(bool on)
{
    if (!s_ok) return;
    if (nvs_set_u8(s_h, K_DRIFTCOMP, on ? 1 : 0) == ESP_OK)
        nvs_commit(s_h);
}

/* Playlist on the SD card. OFF by default: writing to a card the user has not
 * asked us to touch is not a thing to do by surprise. */
bool settings_get_sd_playlist(void)
{
    uint8_t v = 0;
    if (s_ok) nvs_get_u8(s_h, K_SDPLAYLIST, &v);
    return v != 0;
}

void settings_set_sd_playlist(bool on)
{
    if (!s_ok) return;
    if (nvs_set_u8(s_h, K_SDPLAYLIST, on ? 1 : 0) == ESP_OK) nvs_commit(s_h);
}

/* How long a track must play before it counts. Also how much silence separates
 * one playlist file from the next — see sdcard.h.
 *
 * Stored in seconds in a u8, so 255 is the ceiling; the menu offers a few
 * sensible values rather than a free number. */
uint8_t settings_get_sd_threshold_s(void)
{
    uint8_t v = 60;
    if (s_ok) nvs_get_u8(s_h, K_SDTHRESH, &v);
    return v ? v : 60;
}

void settings_set_sd_threshold_s(uint8_t s)
{
    if (!s_ok || s == 0) return;
    if (nvs_set_u8(s_h, K_SDTHRESH, s) == ESP_OK) nvs_commit(s_h);
}

/* Screenshots: whether a long press saves the screen instead of locking it.
 * Off by default — the long press means "lock" to anyone who has not gone
 * looking for this. */
bool settings_get_screenshots(void)
{
    uint8_t v = 0;
    if (s_ok) nvs_get_u8(s_h, K_SHOTS, &v);
    return v != 0;
}

void settings_set_screenshots(bool on)
{
    if (!s_ok) return;
    if (nvs_set_u8(s_h, K_SHOTS, on ? 1 : 0) == ESP_OK) nvs_commit(s_h);
}

/* ===========================================================================
 * THE OSC CUSTOM PROFILE: an opaque blob
 *
 * STORED AS BYTES, not as a structure this component knows. The shape of a
 * message is defined in osc_out.h, and making settings include it would turn a
 * base library — which today depends on nvs_flash and nothing else — into
 * something that knows about the OSC output.
 *
 * So the caller hands over bytes and gets bytes back; it is the caller's job to
 * check that what came back is the size it expects. A firmware that changes the
 * message layout will read a blob of the wrong length and discard it, which is
 * the right outcome: better an empty list than one misread field by field.
 * ==========================================================================*/
size_t settings_get_osc_custom(void *out, size_t cap)
{
    if (!s_ok || !out || !cap) return 0;
    size_t len = cap;
    if (nvs_get_blob(s_h, K_OSC_CUST, out, &len) != ESP_OK) return 0;
    return len;
}

void settings_set_osc_custom(const void *data, size_t len)
{
    if (!s_ok) return;
    esp_err_t e = (data && len) ? nvs_set_blob(s_h, K_OSC_CUST, data, len)
                                : nvs_erase_key(s_h, K_OSC_CUST);
    /* Erasing a key that was never written returns NOT_FOUND, which is not a
     * failure: it means there was nothing to remove. */
    if (e == ESP_OK || e == ESP_ERR_NVS_NOT_FOUND) nvs_commit(s_h);
}

brightness_t settings_get_display_brightness(void)
{
    uint8_t v = BRIGHT_HIGH;                  /* default: HIGH (as the boot init) */
    if (s_ok) nvs_get_u8(s_h, K_DISP_BR, &v);
    if (v > BRIGHT_HIGH) v = BRIGHT_HIGH;     /* fallback on unknown value */
    return (brightness_t)v;
}

void settings_set_display_brightness(brightness_t b)
{
    if (!s_ok) return;
    if (nvs_set_u8(s_h, K_DISP_BR, (uint8_t)b) == ESP_OK)
        nvs_commit(s_h);
}

led_brightness_t settings_get_led_brightness(void)
{
    uint8_t v = LED_BRIGHT_MED;
    if (s_ok) nvs_get_u8(s_h, K_LED_BR, &v);
    if (v > LED_BRIGHT_HIGH) v = LED_BRIGHT_MED;   /* fallback on unknown value */
    return (led_brightness_t)v;
}

void settings_set_led_brightness(led_brightness_t b)
{
    if (!s_ok) return;
    if (nvs_set_u8(s_h, K_LED_BR, (uint8_t)b) == ESP_OK)
        nvs_commit(s_h);
}
