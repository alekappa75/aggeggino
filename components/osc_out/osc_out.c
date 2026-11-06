/*
 * osc_out.c — formatting and sending OSC messages.
 *
 * OSC 1.0 FORMAT, the part that matters here:
 *   [address pattern]  NUL-terminated ASCII string, padded with NULs to the
 *                      next multiple of 4
 *   [type tag]         ',' followed by one character per argument ('f' float,
 *                      'i' int32), padded by the same rule
 *   [arguments]        in order, big-endian, 4 bytes each
 *
 * The padding to multiples of 4 is mandatory: a receiver that finds a
 * misaligned message discards it silently, and it is the commonest mistake
 * when implementing OSC by hand.
 */
#include "osc_out.h"
#include "esp_log.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include <fcntl.h>          /* O_NONBLOCK, fcntl */
#include <string.h>
#include <math.h>
#include <stdio.h>          /* snprintf, for placeholder substitution */
#include <stdlib.h>         /* strtol, strtof, strtod: the value is text */

static const char *TAG = "osc";

static int              s_sock = -1;
static struct sockaddr_in s_dst;
static bool             s_enabled = false;
static bool             s_dst_ok  = false;
static osc_out_preset_t s_preset  = OSC_OUT_GENERIC;

static uint32_t s_sent = 0, s_err = 0;

/* ---------------------------------------------------------------- encoding */

/* Write an OSC string, padding included, and return how many bytes it used. */
static size_t osc_str(uint8_t *p, const char *s)
{
    size_t n = strlen(s);
    memcpy(p, s, n);
    p[n++] = 0;
    while (n % 4) p[n++] = 0;      /* at least one NUL, then up to a multiple of 4 */
    return n;
}

static size_t osc_i32(uint8_t *p, int32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >>  8); p[3] = (uint8_t)v;
    return 4;
}

static size_t osc_f32(uint8_t *p, float f)
{
    /* The float goes out with the IEEE 754 bit pattern, big-endian. It passes
     * through an integer of the same width rather than a pointer cast, which
     * would violate strict aliasing. */
    uint32_t bits;
    memcpy(&bits, &f, 4);
    return osc_i32(p, (int32_t)bits);
}

/* Single-argument message, which covers every case we need. */
/* A message with NO argument: the path and an empty type tag. Legal OSC, and
 * what a trigger wants when the event itself is the message — a resync, a
 * downbeat marker. */
static void osc_send0(const char *addr)
{
    if (s_sock < 0 || !s_dst_ok) return;

    uint8_t buf[128];
    size_t n = 0;
    n += osc_str(buf + n, addr);
    n += osc_str(buf + n, ",");

    int r = sendto(s_sock, buf, n, 0, (struct sockaddr *)&s_dst, sizeof(s_dst));
    if (r < 0) s_err++; else s_sent++;
}

/* A message carrying a STRING. The custom profile needs it for the metadata;
 * neither fixed profile ever sends one.
 *
 * OSC pads every string to a multiple of four bytes with NULs, which osc_str
 * already does — the same rule as the address and the type tag. */
static void osc_send_str(const char *addr, const char *s)
{
    if (s_sock < 0 || !s_dst_ok) return;

    uint8_t buf[256];
    size_t n = 0;
    n += osc_str(buf + n, addr);
    n += osc_str(buf + n, ",s");
    n += osc_str(buf + n, s ? s : "");

    int r = sendto(s_sock, buf, n, 0, (struct sockaddr *)&s_dst, sizeof(s_dst));
    if (r < 0) s_err++; else s_sent++;
}


static void osc_send1(const char *addr, char type, int32_t iv, float fv)
{
    if (s_sock < 0 || !s_dst_ok) return;

    uint8_t buf[128];
    size_t n = 0;
    n += osc_str(buf + n, addr);
    char tt[3] = { ',', type, 0 };
    n += osc_str(buf + n, tt);
    n += (type == 'f') ? osc_f32(buf + n, fv) : osc_i32(buf + n, iv);

    int r = sendto(s_sock, buf, n, 0, (struct sockaddr *)&s_dst, sizeof(s_dst));
    if (r < 0) s_err++; else s_sent++;
}

/* ------------------------------------------------------------------- API -- */

esp_err_t osc_out_init(void)
{
    if (s_sock >= 0) return ESP_OK;
    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_sock < 0) {
        ESP_LOGW(TAG, "UDP socket not created: no OSC output");
        return ESP_FAIL;
    }
    /* Non-blocking: if the stack is busy we lose a beat rather than stall the
     * task sending it. For a clock that is the right trade — a late beat is
     * worse than a missing one. */
    int fl = fcntl(s_sock, F_GETFL, 0);
    fcntl(s_sock, F_SETFL, fl | O_NONBLOCK);
    return ESP_OK;
}

void osc_out_configure(const char *host, uint16_t port,
                       osc_out_preset_t preset, bool enabled)
{
    s_preset  = preset;
    s_enabled = enabled;
    s_dst_ok  = false;

    if (!host || !*host || port == 0) return;

    memset(&s_dst, 0, sizeof(s_dst));
    s_dst.sin_family = AF_INET;
    s_dst.sin_port   = htons(port);
    if (inet_pton(AF_INET, host, &s_dst.sin_addr) != 1) {
        ESP_LOGW(TAG, "invalid address: %s", host);
        return;
    }
    s_dst_ok = true;
    ESP_LOGI(TAG, "destination %s:%u preset=%d %s",
             host, (unsigned)port, (int)preset, enabled ? "active" : "off");

    /* The socket follows the enable flag, and both are handled HERE because
     * this is the only place that knows both states.
     *
     * Opening and closing must live together, or the symmetry is lost: a
     * version that only added the close, leaving the open in init(), ended up
     * with the socket closed on the first disable and reopened by nobody —
     * dead output until reboot. */
    if (enabled) osc_out_init();
    else         osc_out_close_socket();
}

bool osc_out_active(void)
{
    return s_enabled && s_dst_ok && s_sock >= 0;
}

bool osc_out_socket_ok(void) { return s_sock >= 0; }

/* Close the socket.
 *
 * Not a per-cycle leak — the guard at the top of the open prevents reopening —
 * but a held lwIP descriptor with no reason to be held, from a pool that has
 * proven to be the scarce resource here. */
void osc_out_close_socket(void)
{
    if (s_sock < 0) return;
    /* lwip_close() rather than close(): see the extended note in midi2_net.c.
     * In short, close() belongs to the VFS and does not recognise lwIP
     * descriptors, which start at an offset — it returns -1 without closing,
     * and the socket stays dangling. */
    int rc = lwip_close(s_sock);
    ESP_LOGI(TAG, "socket closed (fd=%d, rc=%d)", s_sock, rc);
    s_sock = -1;
}
bool osc_out_dest_ok(void)   { return s_dst_ok;    }

/* ===========================================================================
 * THE CUSTOM PROFILE: placeholders and dispatch
 * ==========================================================================*/
static osc_custom_msg_t s_custom[OSC_CUSTOM_MAX];
static int              s_custom_n = 0;

/* What the placeholders may refer to. Held here because a message can ask for
 * a title on a beat trigger, long after the track changed: the value must
 * survive the event that produced it. */
static struct {
    double   bpm;
    uint8_t  bar;
    uint8_t  player;
    uint32_t id;
    char     title[OSC_VALUE_MAX];
    char     artist[OSC_VALUE_MAX];
    bool     playing;
} s_now;

/* The names, in the order the keyboard offers them. Kept in ONE place: the
 * substitution below and the menu that inserts them read the same table, so a
 * name added here appears on screen without touching anything else. */
static const char *const s_ph[] = {
    "bpm", "bpm-rel", "bar", "title", "artist", "id", "player", NULL
};

const char *osc_out_placeholder(int i)
{
    if (i < 0) return NULL;
    for (int k = 0; s_ph[k]; k++) if (k == i) return s_ph[k];
    return NULL;
}

/* Writes the value of one placeholder. Returns the characters written.
 *
 * AN UNKNOWN NAME WRITES NOTHING and is not an error: the text was typed on a
 * keyboard and a name may survive a firmware that no longer offers it. Better a
 * gap in the string than a message that stops going out. */
static size_t ph_expand(const char *name, size_t len, char *out, size_t cap)
{
    #define IS(s)  (len == strlen(s) && strncmp(name, s, len) == 0)
    if (IS("bpm"))    return (size_t)snprintf(out, cap, "%.2f", s_now.bpm);
    if (IS("bpm-rel")) {
        /* The same normalisation Resolume wants, available to anyone: a tempo
         * between 20 and 500 BPM mapped onto 0..1. */
        double v = (s_now.bpm - 20.0) / (500.0 - 20.0);
        if (v < 0.0) v = 0.0;
        if (v > 1.0) v = 1.0;
        return (size_t)snprintf(out, cap, "%.4f", v);
    }
    if (IS("bar"))    return (size_t)snprintf(out, cap, "%u", (unsigned)s_now.bar);
    if (IS("title"))  return (size_t)snprintf(out, cap, "%s", s_now.title);
    if (IS("artist")) return (size_t)snprintf(out, cap, "%s", s_now.artist);
    if (IS("id"))     return (size_t)snprintf(out, cap, "%lu", (unsigned long)s_now.id);
    if (IS("player")) return (size_t)snprintf(out, cap, "%u", (unsigned)s_now.player);
    #undef IS
    return 0;
}

/* Copies src into dst replacing every {name}. An unclosed brace is copied as it
 * stands: it is what the user typed, and guessing at it would be worse. */
static void ph_substitute(const char *src, char *dst, size_t cap)
{
    size_t o = 0;
    for (const char *p = src; *p && o + 1 < cap; ) {
        if (*p != '{') { dst[o++] = *p++; continue; }
        const char *end = strchr(p + 1, '}');
        if (!end) { dst[o++] = *p++; continue; }       /* unclosed: taken literally */
        o += ph_expand(p + 1, (size_t)(end - p - 1), dst + o, cap - o);
        if (o >= cap) o = cap - 1;
        p = end + 1;
    }
    dst[o] = '\0';
}

/* ===========================================================================
 * THE FORMAT IS DEDUCED FROM THE VALUE
 *
 * There is no Format setting. It existed in an earlier shape of this, where the
 * value was CHOSEN from a closed list and saying how to encode it was a second,
 * separate question. With the value now free text carrying placeholders, that
 * second question allowed combinations with no meaning — a title as an integer
 * — and asked the user to hold in mind a distinction the placeholder buttons
 * exist precisely to remove.
 *
 * THE RULE, tried in this order:
 *
 *   empty                     no argument at all
 *   ONE numeric placeholder   that placeholder's own type
 *                               {bpm} {bpm-rel}      float
 *                               {bar} {id} {player}  int
 *   a plain number            int, or float if it has a point
 *   anything else             string
 *
 * The last line catches {title}, {artist}, free text, and any combination of
 * placeholders — all of which are text by nature.
 *
 * The third exists for the constants: a message whose value is "1" is a trigger
 * marker, and sending it as the string "1" would be received as text by
 * software expecting a number.
 * ==========================================================================*/
static bool ph_is(const char *v, const char *name)
{
    size_t n = strlen(name);
    return v[0] == '{' && strncmp(v + 1, name, n) == 0 &&
           v[1 + n] == '}' && v[2 + n] == '\0';
}

static osc_fmt_t fmt_deduce(const char *v)
{
    if (!v || !v[0]) return OSC_FMT_NONE;

    if (ph_is(v, "bpm") || ph_is(v, "bpm-rel"))              return OSC_FMT_FLOAT;
    if (ph_is(v, "bar") || ph_is(v, "id") || ph_is(v, "player")) return OSC_FMT_INT;

    /* A plain number? strtod says so: it stops at the first character it cannot
     * use, and if that is the end of the string the whole thing was a number. */
    char *end = NULL;
    strtod(v, &end);
    if (end && end != v && *end == '\0')
        return strchr(v, '.') ? OSC_FMT_FLOAT : OSC_FMT_INT;

    return OSC_FMT_STRING;
}



/* Sends one message of the list, with its placeholders already resolved. */
static void custom_send(const osc_custom_msg_t *m)
{
    if (!m->path[0]) return;

    char path[OSC_PATH_MAX + OSC_VALUE_MAX];
    char val[OSC_VALUE_MAX * 2];
    ph_substitute(m->path, path, sizeof(path));

    switch (fmt_deduce(m->value)) {
    case OSC_FMT_NONE:
        osc_send0(path);
        break;
    case OSC_FMT_STRING:
        ph_substitute(m->value, val, sizeof(val));
        osc_send_str(path, val);
        break;
    case OSC_FMT_INT:
        ph_substitute(m->value, val, sizeof(val));
        osc_send1(path, 'i', (int32_t)strtol(val, NULL, 10), 0.0f);
        break;
    case OSC_FMT_FLOAT:
    default:
        ph_substitute(m->value, val, sizeof(val));
        osc_send1(path, 'f', 0, strtof(val, NULL));
        break;
    }
}

/* Everything in the list that answers to this trigger. */
static void custom_fire(osc_trigger_t t)
{
    if (!osc_out_active()) return;
    for (int i = 0; i < s_custom_n; i++)
        if (s_custom[i].trigger == (uint8_t)t) custom_send(&s_custom[i]);
}

void osc_out_set_custom(const osc_custom_msg_t *msgs, int n)
{
    if (n < 0) n = 0;
    if (n > OSC_CUSTOM_MAX) n = OSC_CUSTOM_MAX;
    if (msgs && n) memcpy(s_custom, msgs, (size_t)n * sizeof(s_custom[0]));
    s_custom_n = n;
    ESP_LOGI(TAG, "custom profile: %d messages", n);
}

void osc_out_note_track(uint8_t player, uint32_t id,
                        const char *title, const char *artist)
{
    /* ONLY A REAL CHANGE FIRES. This is called from the metadata path, which
     * may hand over the same track more than once — a player becoming master,
     * a lookup repeated — and a message per repetition would be noise on the
     * wire. */
    if (id == s_now.id && player == s_now.player) return;

    s_now.player = player;
    s_now.id     = id;
    snprintf(s_now.title,  sizeof(s_now.title),  "%s", title  ? title  : "");
    snprintf(s_now.artist, sizeof(s_now.artist), "%s", artist ? artist : "");
    if (s_preset == OSC_OUT_CUSTOM) custom_fire(OSC_TRIG_TRACK);
}

void osc_out_note_transport(bool playing)
{
    if (playing == s_now.playing) return;
    s_now.playing = playing;
    if (s_preset == OSC_OUT_CUSTOM) custom_fire(OSC_TRIG_TRANSPORT);
}


void osc_out_send_beat(double bpm, uint8_t bb)
{
    if (!osc_out_active()) return;

    /* The placeholders see this beat, whatever the profile: switching to
     * CUSTOM must not require waiting for a tempo change to have a value. */
    const bool tempo_moved = (s_now.bpm > 0.0) && (fabs(bpm - s_now.bpm) > 0.004);
    s_now.bpm = bpm;
    s_now.bar = bb;

    switch (s_preset) {
    case OSC_OUT_RESOLUME: {
        /* Resolume does not accept individual beats: we send the TEMPO, and a
         * resync on the downbeat so it realigns its grid to ours.
         *
         * The tempo must be NORMALISED to 0..1 over the 20-500 BPM range: it
         * is an interface parameter, not a beat count. Sending 132.0 pins
         * Resolume at maximum, and it is the commonest mistake. */
        float norm = (float)((bpm - 20.0) / (500.0 - 20.0));
        if (norm < 0.0f) norm = 0.0f;
        if (norm > 1.0f) norm = 1.0f;
        osc_send1("/composition/tempocontroller/tempo", 'f', 0, norm);
        if (bb == 1)
            osc_send1("/composition/tempocontroller/resync", 'i', 1, 0.0f);
        break;
    }
    case OSC_OUT_CUSTOM:
        /* THE ORDER IS DELIBERATE: the tempo first, because a receiver that
         * uses both wants to know the new tempo before the beat it applies
         * to. Then the beat, then the downbeat — which is a beat too, so a
         * message on each fires both, in that order. */
        if (tempo_moved) custom_fire(OSC_TRIG_TEMPO);
        custom_fire(OSC_TRIG_BEAT);
        if (bb == 1) custom_fire(OSC_TRIG_DOWNBEAT);
        break;

    case OSC_OUT_GENERIC:
    default:
        /* One message per beat: here the phase is OURS, bar by bar, and the
         * tempo travels as a real number rather than normalised. */
        osc_send1("/bpm",      'f', 0, (float)bpm);
        osc_send1("/beat",     'i', 1, 0.0f);
        osc_send1("/beat/bar", 'i', (int32_t)bb, 0.0f);
        break;
    }
}

void osc_out_stats(uint32_t *sent, uint32_t *err)
{
    if (sent) *sent = s_sent;
    if (err)  *err  = s_err;
    s_sent = 0; s_err = 0;
}
