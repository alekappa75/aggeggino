/* ============================================================================
 * settings_ui.c - Settings screen (navigation + WiFi config).
 * See settings_ui.h for the contract with the dashboard.
 *
 * Model: each SCREEN is a menu (title + list of rows). Rows come either from a
 * STATIC table (s_screens[]) or from a DYNAMIC provider (built at runtime -
 * WiFi scan list, mode-conditional Network menu, live Status). Navigation is a
 * stack; tap a row to enter/act, physical bar centre = back, arrows = scroll.
 *
 * Row types:
 *   - normal: label + value (amber, right-aligned) or a filled triangle if it
 *     enters a submenu.
 *   - substatus: label + a small coloured line underneath (green/amber/red) -
 *     used by "Status" to show the current situation at a glance.
 * ==========================================================================*/
#include "settings_ui.h"
#include "lvgl.h"
#include "wifi_core.h"
#include "esp_wifi_types.h"    /* WIFI_AUTH_* for per-encryption key rules */
#include "linkbridge.h"        /* transport sync */
#include "wifi_client.h"
#include "settings.h"
#include "dhcp_lite.h"
#include "sdcard.h"
#include "assets/ui_assets.h"
#include "audio.h"
#include "leds.h"
#include "bsp_cores3.h"
#include "display.h"    /* display_set_flipped */
#include "about.h"     /* the About screen intro */
#include "esp_system.h"   /* esp_restart */
#include "esp_app_desc.h"
#include "esp_timer.h"
#include <string.h>
#include <stdlib.h>   /* strtoul */
#include "esp_log.h"
#include <stdio.h>

/* ---- palette: same as the dashboard ---- */
#define C_BG       lv_color_hex(0x0E1116)
#define C_PANEL    lv_color_hex(0x151A21)
#define C_ROW      lv_color_hex(0x141920)
#define C_DIV      lv_color_hex(0x242A32)
#define C_CYAN     lv_color_hex(0x2DD4D4)
#define C_WHITE    lv_color_hex(0xFFFFFF)
#define C_LABEL    lv_color_hex(0xC8CCD2)
#define C_MUTED    lv_color_hex(0x8A9098)
#define C_MUTED2   lv_color_hex(0x565E68)
#define C_GREEN    lv_color_hex(0x4CC94C)
#define C_AMBER    lv_color_hex(0xEFA02A)
#define C_RED      lv_color_hex(0xE0524A)
#define C_SBTRACK  lv_color_hex(0x1A1F26)
#define C_SBTHUMB  lv_color_hex(0x3A4450)

/* ---- geometry ---- */
#define SCR_W        320
#define SCR_H        240
#define HDR_H        36
#define ROW_H        65
#define ROW_GAP      8
#define ROW_X        10
#define ROW_W        290
#define LIST_TOP     (HDR_H + 8)
#define LIST_BOTTOM  SCR_H
#define ROWS_VISIBLE ((LIST_BOTTOM - LIST_TOP) / ROW_H)

#define BAR_Y         248
#define BAR_LEFT_X    145
#define BAR_RIGHT_X   195

#define SB_W          4
#define SB_X          (SCR_W - SB_W - 3)
#define SB_TOP        LIST_TOP
#define SB_H          (ROWS_VISIBLE * ROW_H - ROW_GAP)
#define SB_MIN_THUMB  16

/* ============================================================================
 * SCREEN IDS
 * ==========================================================================*/
typedef enum {
    SCR_NONE = 0,
    SCR_MAIN,
    SCR_NET,           /* DYNAMIC: mode-conditional Network menu */
    SCR_NET_SCAN,      /* DYNAMIC: WiFi scan results */
    SCR_NET_PASS,      /* SPECIAL: password entry (custom layout, not a row list) */
    SCR_NET_STATUS,    /* DYNAMIC: live status (mode-dependent) */
    SCR_SYS,
    SCR_SYS_BRIDGE,
    SCR_BRIDGE_OSC,    /* OSC output: enable, destination, preset */
    SCR_OSC_CUSTOM,    /* the custom profile: the list of messages */
    SCR_OSC_MSG,       /* one message of that list                */
    /* MIDI gets its own level rather than sitting under Bridge. There are
     * three MIDI outputs — network, DIN, USB — and each will have its own
     * options: hanging them off Bridge would have bloated it and hidden a whole
     * family behind an entry about something else. */
    SCR_SINK,          /* list of clock outputs      */
    SCR_MIDI_NET,      /* Network MIDI 2.0 (UMP/UDP) */
    SCR_MIDI_DIN,      /* MIDI DIN su Port C         */
    SCR_MIDI_USB,      /* MIDI su USB-C (TinyUSB)    */
    SCR_SINK_CV,       /* CV output                  */
    SCR_SINK_MIDI,     /* MIDI family: the three transports */
    SCR_ABOUT,         /* full-screen intro, no list */
    SCR_IPENTRY,       /* IPv4 entry with the keypad, under test */
    SCR_SYS_DEV,       /* Developer: test actions. The enum VALUE stays even
                        * without DJB_DEVELOPER — it costs nothing, while
                        * removing it would renumber everything after and the
                        * screen table is indexed by these. */
    SCR_DEVICE,        /* first-level: Audio, display/LED brightness */
    SCR_SYS_INFO,

    /* THE WIRED SIDE, its own branch. The LAN is where the players live, and
     * until now the only thing about it in the menus was a device number
     * buried under Clock Bridge. Address, how it was obtained and DJ Link
     * identity are one subject and belong together. */
    SCR_LAN,           /* wired network: status and identity */
    SCR_LAN_STATUS,    /* DYNAMIC: address, how it was got, DJ Link ID */
    SCR_CLOCK,         /* where the tempo comes from and where it goes */
    SCR_SDCARD,        /* the microSD slot and what we write on it     */
    SCR_COUNT
} screen_id_t;

/* ============================================================================
 * STATIC MENU TABLE (screens that are plain lists)
 * ==========================================================================*/
typedef struct {
    const char *label;
    const char *value;
    screen_id_t target;
} menu_item_t;

typedef struct {
    const char *title;
    const menu_item_t *items;
    uint8_t count;
} screen_def_t;

/* THE TWO NETWORKS FIRST: nothing works until they are right, and they are what
 * anybody opening these menus is there to check. WiFi carries Ableton Link, LAN
 * carries the players.
 *
 * What the device does with the tempo comes next, then UI and System, which are
 * touched once and forgotten. */
static const menu_item_t items_main[] = {
    { "Clock",        NULL, SCR_CLOCK },
    { "WiFi",         NULL, SCR_NET },   /* dynamic value: current mode */
    { "LAN",          NULL, SCR_LAN },
    { "SD card",      NULL, SCR_SDCARD },
    { "UI",           NULL, SCR_DEVICE },
    { "System",       NULL, SCR_SYS },
};

/* --- THE TEMPO, both directions ---------------------------------------------
 *
 * Where it comes from and where it goes: one subject, two entries. */
/* --- THE CARD ---------------------------------------------------------------
 *
 * One switch for now. The threshold a track must be played for, and the file
 * name prefix, are fixed — see sdcard.h — and become settings once the writing
 * itself has been used in anger. */
static const menu_item_t items_sdcard[] = {
    { "Store playlist", NULL, SCR_NONE },   /* ON / OFF                     */
    { "Track after",    NULL, SCR_NONE },   /* seconds of continuous play   */
    { "Format card",    NULL, SCR_NONE },   /* erases it, twice-confirmed   */
};


static const menu_item_t items_clock[] = {
    { "Bridge",  NULL, SCR_SYS_BRIDGE },
    { "Sink",    NULL, SCR_SINK },
};

/* --- CLOCK OUTPUTS, all in one place ---------------------------------------
 *
 * THREE FAMILIES, not five transports. The three MIDI variants are versions of
 * ONE thing, while OSC and CV are different things; listing all five at the
 * same level mixed two criteria. Grouping keeps the list short and homogeneous,
 * and leaves room should other MIDI transports arrive.
 *
 * The three MIDI transports share the same clock engine but not the same
 * options — the network one has announcement and sessions, the DIN a physical
 * port, USB the device identity — so each gets its own level.
 *
 * OSC belongs here and not under "Bridge": the bridge is what RECEIVES the
 * tempo, OSC is an output like the others.
 *
 * The order is by family: the three MIDI, then OSC, then CV. */
static const menu_item_t items_sink[] = {
    { "MIDI",       NULL, SCR_SINK_MIDI },
    { "OSC",        NULL, SCR_BRIDGE_OSC },
    { "CV",         NULL, SCR_SINK_CV },
};

/* The order follows convenience of use rather than the alphabet: USB-C is the
 * port always at hand, the DIN needs the module, the network needs
 * configuration at the other end. */
static const menu_item_t items_sink_midi[] = {
    { "MIDI USB-C",  NULL, SCR_MIDI_USB },
    { "MIDI DIN Plug",         NULL, SCR_MIDI_DIN },
    { "Network MIDI 2.0",      NULL, SCR_MIDI_NET },
};

/* The order is by practical importance: DIVISION first of all, because getting
 * it wrong means playing at double speed or at a twenty-fourth — the rest are
 * refinements. The SAM sound goes last: it is an extra, and it needs the
 * external module. */
static const menu_item_t items_sink_cv[] = {
    { "Enabled",     NULL, SCR_NONE },
    { "Division",    NULL, SCR_NONE },
    { "Pulse width", NULL, SCR_NONE },
    { "Polarity",    NULL, SCR_NONE },
    { "Sound",       NULL, SCR_NONE },
};

/* A single row, as for the DIN. The state of the hardware — here "the host has
 * enumerated me" — does not deserve an entry of its own: it appears as a
 * subtitle. */
static const menu_item_t items_midi_usb[] = {
    { "Enabled",   NULL, SCR_NONE },
};

static const menu_item_t items_midi_net[] = {
    { "Enabled",  NULL, SCR_NONE },   /* dynamic value: ON/OFF */
    { "Announce", NULL, SCR_NONE },   /* dynamic value: ON/OFF */
};

/* The DIN has its own switch, independent of the network one: without it the
 * transport stayed on forever and the MIDI destination never disappeared from
 * the phase selector, whatever was done with the network.
 *
 * A single row. The outcome of the detection does not deserve an entry of its
 * own: it is a STATE of this one, not a setting in itself, and when it matters
 * it appears as a red subtitle under the label. */
static const menu_item_t items_midi_din[] = {
    { "Enabled",   NULL, SCR_NONE },
};

/* --- THE WIRED SIDE ---------------------------------------------------------
 *
 * Status first, as under WiFi: what is happening now, before what can be
 * changed. Below it the one thing there is to decide — whether we take a player
 * number at all.
 *
 * DJ Link ID belongs here rather than under Clock Bridge: it is not about the
 * bridge, it is about who we are on the wire. */
static const menu_item_t items_lan[] = {
    { "Status",      NULL, SCR_LAN_STATUS },
    { "DJ Link ID",  NULL, SCR_NONE },        /* AUTO / OBSERVER */
};

/* Read-only, all of it. The values are filled in by render(). */
static const menu_item_t items_lan_status[] = {
    { "IP address",  "--", SCR_NONE },
    { "Obtained by", "--", SCR_NONE },   /* DHCP or auto-ip */
    { "DJ Link ID",  "--", SCR_NONE },   /* the number actually held */
};



/* System is flat: an intermediate "Maintenance" level added no information,
 * only one more tap to reach the three entries that mattered. Developer stays
 * here as long as it is useful, then disappears from the final build.
 *
 * All three entries carry a confirmation. */
/* Developer is the sixth entry, and disappears with DJB_DEVELOPER. */
#if DJB_DEVELOPER
#define SYS_ITEMS 6
#else
#define SYS_ITEMS 5
#endif

static const menu_item_t items_sys[] = {
    { "Status",        NULL,  SCR_SYS_INFO },
    { "About",         NULL,  SCR_ABOUT },
    { "Power off",     NULL,  SCR_NONE },
    { "Restart",       NULL,  SCR_NONE },
    { "Factory reset", NULL,  SCR_NONE },
#if DJB_DEVELOPER
    /* LAST ON PURPOSE. Being last means the count above it is unchanged when
     * it is compiled out — no index shifts, and the touch handler keeps
     * working on the same numbers. */
    { "Developer",     NULL,  SCR_SYS_DEV },
#endif
};

#if DJB_DEVELOPER
/* Developer menu: test actions triggered by hand. Entries can be added freely
 * here, with the matching case in activate_item. */
static const menu_item_t items_sys_dev[] = {
    { "Save screenshots", NULL, SCR_NONE },   /* ON / OFF */
};
#endif

static const menu_item_t items_sys_bridge[] = {
    { "Start Sync",      NULL,   SCR_NONE },   /* dynamic value */
    { "Stop Sync",       NULL,   SCR_NONE },   /* dynamic value */
    { "Drift Comp.",     NULL,   SCR_NONE },   /* dynamic value */
    { "Remember mode",   NULL,   SCR_NONE },   /* dynamic value */
    /* "DJ Link ID" moved to LAN: it is about who we are on the wire, not
     * about the bridge. */
    /* "OSC output" lives under Clock Sink, with all the other outputs. */
};

/* OSC output. All four entries are live: Enabled and Preset toggle on touch,
 * Host and Port open the keyboard editor — the same one as SSID and password —
 * with validation character by character. */
static const menu_item_t items_bridge_osc[] = {
    { "Enabled",    NULL,  SCR_NONE },       /* dynamic value: ON/OFF       */
    { "Preset",     NULL,  SCR_NONE },       /* dynamic value: cycles       */
    { "Host",       NULL,  SCR_NONE },       /* dynamic value: IP address   */
    { "Port",       NULL,  SCR_NONE },       /* dynamic value: number       */
    { "Custom",     NULL,  SCR_OSC_CUSTOM }, /* the message list            */
};

/* first-level Device menu: all values are set dynamically in screen_row */
/* Order: from the least to the most specific to the screen. Audio has nothing
 * to do with the display, the LEDs are the only thing visible with the device
 * off, then the two display entries — first how bright it is, then how it is
 * oriented. */
static const menu_item_t items_device[] = {
    { "Audio",              NULL,  SCR_NONE },
    { "LED brightness",     NULL,  SCR_NONE },
    { "Screen brightness",  NULL,  SCR_NONE },
    { "Screen flip",        NULL,  SCR_NONE },
};

/* Identity supplied by main, see settings_ui_set_ident. */
static uint8_t s_ident_mac[6] = { 0 };
static uint8_t s_ident_ip[4]  = { 0 };
static uint8_t s_ident_dev    = 0;
static bool    s_bridge_active = false;   /* pushed in by main, see the header */

/* The custom OSC list, owned by main. See the note in the header. */
static osc_custom_msg_t *s_oc      = NULL;
static int              *s_oc_n    = NULL;
static void            (*s_oc_save)(void) = NULL;
static int               s_oc_edit = 0;      /* which one the Message page shows */

/* THE LIST HAS CHANGED AND IS NOT YET IN NVS.
 *
 * Writing on every touch was audible: a commit is 848 bytes to flash, and on
 * this chip a flash write DISABLES THE CACHE FOR BOTH CORES — every piece of
 * code not in IRAM stops, the audio task included, and the beep comes out
 * broken. Cycling through the triggers made it unmistakable.
 *
 * So the edit happens in RAM, where the output reads it at once, and the write
 * is deferred to the moment the user leaves the page: one commit instead of
 * one per tap. Nothing is lost on the way — the only path out of these screens
 * is nav_pop, which flushes. */
static bool              s_oc_dirty = false;

/* Applies the list to the output WITHOUT touching flash. */
static void osc_touched(void)
{
    s_oc_dirty = true;
    if (s_oc && s_oc_n) osc_out_set_custom(s_oc, *s_oc_n);
}

/* The deferred write. Called on leaving the Custom screens. */
static void osc_flush(void)
{
    if (!s_oc_dirty) return;
    s_oc_dirty = false;
    if (s_oc_save) s_oc_save();
}

void settings_ui_set_osc_custom(osc_custom_msg_t *msgs, int *count,
                                void (*save)(void))
{
    s_oc = msgs; s_oc_n = count; s_oc_save = save;
}
#if DJB_DEVELOPER
/* Told when the screenshot option changes. A callback because ui is the one
 * that needs to know and this component must not depend on it. */
static settings_ui_shots_fn s_shots_handler = NULL;
void settings_ui_set_shots_handler(settings_ui_shots_fn fn) { s_shots_handler = fn; }
#endif

void settings_ui_set_bridge_active(bool active) { s_bridge_active = active; }

void settings_ui_set_ident(const uint8_t mac[6], const uint8_t ip[4],
                           uint8_t djlink_device)
{
    if (mac) memcpy(s_ident_mac, mac, 6);
    if (ip)  memcpy(s_ident_ip,  ip,  4);
    s_ident_dev = djlink_device;
}

static const menu_item_t items_sys_info[] = {
    { "Firmware",  "v0.x",         SCR_NONE },
    { "MAC",       "--",           SCR_NONE },
    { "Uptime",    "--",           SCR_NONE },
    /* "DJ Link ID" moved to LAN > Status, with the address it goes with. */
    { "CPU temp",  "--",           SCR_NONE },
};

static const screen_def_t s_screens[SCR_COUNT] = {
    [SCR_MAIN]       = { "Settings",    items_main,       6 },
    [SCR_SYS]        = { "System",      items_sys,        SYS_ITEMS },
    [SCR_SYS_BRIDGE] = { "Bridge",      items_sys_bridge, 4 },
    [SCR_BRIDGE_OSC] = { "OSC",         items_bridge_osc, 5 },
    /* The list is built row by row, so it has no table: its length is the
     * number of messages plus the "Add" row. */
    [SCR_OSC_CUSTOM] = { "Custom",      NULL,             0 },
    [SCR_OSC_MSG]    = { "Message",     NULL,             0 },
    [SCR_SINK]       = { "Sink",        items_sink,       3 },
    [SCR_SINK_MIDI]  = { "MIDI",        items_sink_midi,  3 },
    /* About: no item table — the drawing all happens in render(). */
    [SCR_ABOUT]      = { "About",       NULL,             0 },
    [SCR_IPENTRY]    = { "IP address",  NULL,             0 },
    [SCR_SINK_CV]    = { "CV",          items_sink_cv,    5 },
    [SCR_MIDI_NET]   = { "Network MIDI 2.0",items_midi_net,   2 },
    [SCR_MIDI_DIN]   = { "MIDI DIN Plug",items_midi_din,   1 },
    [SCR_MIDI_USB]   = { "MIDI USB-C",  items_midi_usb,   1 },
#if DJB_DEVELOPER
    [SCR_SYS_DEV]    = { "Developer",   items_sys_dev,    1 },
#endif
    [SCR_DEVICE]     = { "UI",          items_device,     4 },
    [SCR_SYS_INFO]   = { "Status",      items_sys_info,   4 },
    [SCR_LAN]        = { "LAN",         items_lan,        2 },
    [SCR_LAN_STATUS] = { "Status",      items_lan_status, 3 },
    [SCR_CLOCK]      = { "Clock",       items_clock,      2 },
    [SCR_SDCARD]     = { "SD card",     items_sdcard,     3 },
};

/* ============================================================================
 * NAVIGATION STATE
 * ==========================================================================*/
/* HOW DEEP THE MENUS MAY GO.
 *
 * [M] The deepest path is Settings > Clock > Sink > OSC > Custom > Message >
 * keyboard, which is SEVEN. With the previous value of 6 that last push was
 * refused — silently, because nav_push simply returns when the stack is full —
 * and the keyboard never opened, with nothing in the log to say why.
 *
 * The margin is deliberate: a screen added below an existing one costs a level,
 * and the failure mode is a page that will not open rather than anything
 * noisier. Each level costs two ints. */
#define NAV_DEPTH 10
static screen_id_t s_stack[NAV_DEPTH];
static int         s_stack_scroll[NAV_DEPTH];   /* saved scroll pos per level */
static int         s_depth = 0;
static int         s_scroll = 0;
static bool        s_active = false;

static lv_obj_t *s_scr;
static lv_obj_t *s_title;
static lv_obj_t *s_row_bg[ROWS_VISIBLE];
static lv_obj_t *s_row_lbl[ROWS_VISIBLE];
static lv_obj_t *s_row_val[ROWS_VISIBLE];
static lv_obj_t *s_row_sub[ROWS_VISIBLE];   /* substatus line (under the label) */
/* Called FROM the intro's TASK when it ends, on a touch or at the timeout.
 *
 * It touches no LVGL object: calling render() from there would mean drawing
 * from a context that is not the LVGL task. It merely records that we must go
 * back, and the UI tick — which runs on the right task — does it. */
static volatile bool s_fx_done = false;
static void on_fx_done(void) { s_fx_done = true; }
/* Defined much further down: it is needed here because refresh_tick goes back
 * when the intro ends. */
static void nav_pop(void);



/* ============================================================================
 * IPv4 ENTRY — numeric keypad
 *
 * WHY THE PASSWORD SCREEN IS NOT REUSED. With the character selector,
 * "192.168.1.50" costs up to FIFTEEN swipes, each needing its own aim. And,
 * above all, there is no validation: one can compose 999.888.1.2 and find out
 * only afterwards.
 *
 * An IPv4 address is not text: it is FOUR NUMBERS between 0 and 255. If the
 * interface knows that, wrong input becomes impossible by construction rather
 * than reported after the fact.
 *
 * GEOMETRY: five columns, because the ten digits fit into exactly TWO rows with
 * no odd key left over. The third row takes up the three wide keys the password
 * screen already uses for backspace, space and OK — and uses the SAME symbols,
 * so the gesture learned there works here too.
 *
 * ONE OBJECT PER KEY, not two: a label with a background rather than a
 * rectangle plus text. Thirteen objects instead of twenty-six, on a heap that
 * is the scarce resource of this project.
 *
 * FIXED OCTET WIDTH: every octet occupies the space of three digits even when
 * it shows one. If the box shrank while typing, the whole address would shift
 * under the finger — and the aim for the next key would be lost.
 * ==========================================================================*/
/* For the keypad test screen only. */
static const char *IPK_TAG = "ipk";

/* --- THE FIELD IS PARAMETRIC -----------------------------------------------
 *
 * The same screen serves an IPv4 address (four groups of three digits, 0..255,
 * separated by dots) and a port (one group of five, 1..65535, no separator).
 * The differences are FIVE NUMBERS and one character; everything else — key
 * geometry, hit testing, creation and show/hide — is identical.
 *
 * Cloning the screen would have meant two coordinate tables, and the first
 * change made to only one of them would have produced a key that is seen in one
 * place and answers in another. There stays ONE table. */
typedef struct {
    uint8_t  groups;      /* 4 for an IP, 1 for a port          */
    uint8_t  digits;      /* digits per group: 3 or 5            */
    uint32_t vmin, vmax;  /* per GROUP: 0..255 or 1..65535      */
    char     sep;         /* '.' or 0 for no separator          */
    const char *title;
    /* Called on OK with the value already composed and already valid: nothing
     * needs rechecking here, the screen does not let anything else be
     * composed. */
    void (*commit)(const char *value);
} ipk_field_t;

#define IPK_MAX_GROUPS  4
#define IPK_MAX_DIGITS  5

/* Derived geometry, not hand-written: a slot is digits x 18 px wide, which for
 * three digits gives the 54 px the screen was tuned to by hand — so the formula
 * generalises the already validated case rather than changing it. */
#define IPK_DIGIT_W   18
#define IPK_DOT_W     26         /* separator width: see the note below       */
#define IPK_BOX_PAD    4         /* how far the box overhangs the slot        */

/* THE CRITERION WORTH REMEMBERING: the box around the active octet overhangs
 * the slot by IPK_BOX_PAD, to give the digits room to breathe. The distance
 * between its edge and the separator glyph is IPK_DOT_W/2 - IPK_BOX_PAD, and so
 * depends on the SEPARATOR'S SPACE, not on the slot width. With IPK_DOT_W at 12
 * only 2 px were left and the box ran into it. */

static lv_obj_t *s_ipk_oct[IPK_MAX_GROUPS];
static lv_obj_t *s_ipk_dot[IPK_MAX_GROUPS - 1];
static lv_obj_t *s_ipk_box;
static lv_obj_t *s_ipk_key[13];

static char s_ipk_val[IPK_MAX_GROUPS][IPK_MAX_DIGITS + 1];
static int  s_ipk_act;               /* group being entered  */
static ipk_field_t s_ipk_f;          /* current field        */
static int  s_ipk_slot_w, s_ipk_x0;  /* derived geometry     */

/* Key geometry, in one place: both the drawing AND the hit testing use it. Two
 * separate tables would have diverged at the first change. */
typedef struct { int16_t x, y, w, h; const char *lbl; } ipk_key_t;

/* NOT const: the three bottom entries change with the field. Without a
 * separator, backspace and confirm each widen to half the screen instead of
 * leaving a gap where the dot was. */
static ipk_key_t s_ipk_geo[13] = {
    {   8,  96, 56, 42, "1" }, {  70,  96, 56, 42, "2" }, { 132,  96, 56, 42, "3" },
    { 194,  96, 56, 42, "4" }, { 256,  96, 56, 42, "5" },
    {   8, 144, 56, 42, "6" }, {  70, 144, 56, 42, "7" }, { 132, 144, 56, 42, "8" },
    { 194, 144, 56, 42, "9" }, { 256, 144, 56, 42, "0" },
    /* Same symbols and same sides as the password screen: backspace on the
     * left in red, confirm on the right in green. */
    {   8, 192, 96, 40, LV_SYMBOL_BACKSPACE },
    { 110, 192, 96, 40, "." },
    { 212, 192,100, 40, LV_SYMBOL_OK },
};

/* --- CONFIRMING DESTRUCTIVE ACTIONS ---------------------------------------
 *
 * Factory reset, Restart and Power off do not execute on the first touch: the
 * row ARMS itself and shows a red subtitle, and only a SECOND touch on the same
 * row carries it out.
 *
 * WHY INLINE AND NOT A CONFIRMATION SCREEN: on a small display a dedicated
 * window loses the context — one can no longer see which entry was being
 * touched — and it costs an SCR_* with its own table. Here the text stays
 * beside the entry it describes.
 *
 * IT DISARMS ITSELF: on leaving the screen, on touching another row, or after
 * the timeout. An armed action left armed is worse than none at all: the user
 * comes back ten minutes later, touches out of curiosity, and erases
 * everything. */
#define CONFIRM_TIMEOUT_US   (10 * 1000000)

static int     s_confirm_row = -1;   /* ARMED index, -1 = none */
static int64_t s_confirm_us  = 0;

static void confirm_clear(void) { s_confirm_row = -1; }

/* True if the row is armed and not expired. It has an intended side effect: it
 * disarms when it finds an expired arming, so the check and the cleanup live in
 * one place. */
static bool confirm_armed(int idx)
{
    if (s_confirm_row != idx) return false;
    if (esp_timer_get_time() - s_confirm_us > CONFIRM_TIMEOUT_US) {
        confirm_clear();
        return false;
    }
    return true;
}
static lv_obj_t *s_sb_track;
static lv_obj_t *s_sb_thumb;

/* ---- password screen objects (custom layout) ---- */
static lv_obj_t *s_pw_field;            /* the typed password */
static lv_obj_t *s_pw_fieldbg;         /* password field background */
static lv_obj_t *s_pw_ssid;            /* target ssid, top-right */
static lv_obj_t *s_pw_sel[5];          /* 5-char selector: [prev2,prev1,CUR,next1,next2] */
static lv_obj_t *s_pw_sel_box;         /* highlight box behind the centre char */
/* FIVE, because a field may offer the placeholder mode. The fifth exists
 * always and is hidden where it does not apply — see s_te.placeholders. */
#define PW_MODES     5
#define PW_MODE_PH   4           /* the placeholder mode, the last one */
static lv_obj_t *s_pw_mode_bg[PW_MODES];
static lv_obj_t *s_pw_mode_lbl[PW_MODES];
static lv_obj_t *s_pw_act_bg[3];       /* action backgrounds (canc/space/connect) */
static lv_obj_t *s_pw_act_lbl[3];      /* action labels */

static bool     s_touch_down = false;
static uint16_t s_touch_x = 0, s_touch_y = 0;

/* scan list cache */
#define SCAN_MAX 24
static wifi_core_ap_t s_scan_aps[SCAN_MAX];
static int            s_scan_n = 0;

/* network chosen from the scan list (for the upcoming password + connect step) */
static char s_sel_ssid[33] = {0};
static bool s_sel_secured  = false;
static uint8_t s_sel_auth  = 0;     /* wifi_auth_mode_t of the selected network */

/* ---- generic text-entry state ----
 * The entry screen (SCR_NET_PASS) is a reusable text editor: the character
 * selector, charsets, touch handling and rendering are all generic. What it
 * edits is described by a context set before entering, so the same screen
 * serves the client password, the AP name, the AP password, etc. */
typedef struct {
    const char *title;         /* screen title / breadcrumb ("Password", "AP name") */
    const char *hint;          /* optional text shown top-right (e.g. target ssid), or NULL */
    bool        secret;        /* true = mask the typed text with '*' */
    int         max_len;       /* max characters accepted (0 = buffer limit) */
    /* A DEDICATED character set. NULL = the four standard ones with the mode
     * keys, as it has always been. With a dedicated set the keys disappear:
     * for a port the alphabetic sets are noise, and for an IP one would have to
     * navigate to "#?!" for every dot. */
    const char *charset;
    /* A FIFTH MODE offering the OSC placeholders, where the wheel scrolls
     * whole names instead of letters.
     *
     * IT IS A PROPERTY OF THE FIELD, not of the screen: every other entry —
     * passwords, SSIDs, the AP name — leaves it false and sees exactly what it
     * always has, four mode keys and a five-character wheel. Nothing is
     * resized for them. */
    bool        placeholders;
    /* Per-character filter: returns NULL if 'c' is allowed after 'cur',
     * otherwise a SHORT explanation of the refusal. NULL accepts everything.
     *
     * It prevents what is already WRONG while typing, not what is INCOMPLETE:
     * "192.168.1" has every character legitimate and is not an address.
     * on_confirm is still needed. */
    const char *(*accept_char)(const char *cur, char c);
    /* Called with the typed text on OK. Return NULL to accept (the screen
     * closes), or a short error message to reject: the screen stays open and
     * the message is shown, so a rejected entry is never silently discarded. */
    const char *(*on_confirm)(const char *text);
    /* Where to go after an accepted confirmation. SCR_NONE means back by one,
     * that is to the screen we came from, with its scroll position intact.
     *
     * A different one is needed only when the natural return would be useless:
     * after choosing a network from the scan and entering its password,
     * returning to the list of networks makes no sense, so those three fields
     * ask for SCR_NET. A fixed destination for every field would send those
     * opened from other menus to the wrong place. */
    screen_id_t return_to;
} text_entry_ctx_t;

/* ---- validators for the numeric fields ----
 *
 * Two levels, with different jobs: accept_char prevents WRITING something
 * malformed, on_confirm checks that it is COMPLETE. The first cannot replace
 * the second, because "192.168.1" has every character legitimate and is not an
 * address. */

/* four character sets, selected by the touch mode-keys */
static const char *s_charset[4] = {
    "abcdefghijklmnopqrstuvwxyz",       /* abc */
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ",       /* ABC */
    "0123456789",                       /* 123 */
    "!@#$%^&*()-_=+[]{};:,.<>?/",       /* #?! */
};
static const char *s_charset_name[PW_MODES] = { "abc", "ABC", "123", "#?!", "{..}" };
static int  s_pw_set  = 0;              /* active charset index */
static int  s_pw_char = 0;              /* highlighted char index within the set */
static char s_pw_buf[65] = {0};         /* text being typed (max = longest field) */
static int  s_pw_len  = 0;
static text_entry_ctx_t s_te;           /* active entry context */
static const char *s_te_err = NULL;     /* validation error to show, NULL = none */
static lv_timer_t *s_te_err_timer = NULL;   /* auto-hide timer for the error */

/* REJECTION FEEDBACK on a single character.
 *
 * If the character simply fails to appear, whoever is typing assumes the touch
 * did not register. And a beep is not enough: audio can be switched off.
 *
 * So the selected character FLASHES red for a moment — where the eye is already
 * looking — and in parallel the usual error line explains why. The flash is
 * brief because one types quickly; the message lasts longer because it has to
 * be read. */
#define TE_REJECT_MS 350
static bool s_pw_reject = false;
static lv_timer_t *s_pw_reject_timer = NULL;


/* mode-change handler (registered by main) + debounce timer */
static settings_ui_mode_handler_t s_mode_handler = NULL;
static lv_timer_t *s_mode_debounce = NULL;

/* connect handler (registered by main): saves creds + starts the connection */
static settings_ui_connect_handler_t s_connect_handler = NULL;
static settings_ui_ap_restart_handler_t s_ap_restart_handler = NULL;
static settings_ui_drift_handler_t s_drift_handler = NULL;
static settings_ui_midinet_handler_t s_midinet_handler = NULL;
/* Result of the last electrical detection of the DIN module, reported by main.
 *
 * [M] The detection is validated in the field, and it measures something more
 * precise than "the module is plugged in": it reports whether the module is
 * present AND in SEPARATE mode, the only configuration in which our TX reaches
 * the DIN OUT. See midi_din_detect() in main.c. It is informative and gates
 * nothing — one can still set whatever one wants. */
static bool s_din_hw_seen = false;

void settings_ui_set_din_detected(bool seen) { s_din_hw_seen = seen; }

/* A USB host has enumerated us. Informative, like the DIN detection: it does
 * not prevent setting whatever one wants. */
static bool s_usb_mounted_seen = false;
void settings_ui_set_usb_mounted(bool seen) { s_usb_mounted_seen = seen; }

static settings_ui_sound_name_fn s_snd_fn = NULL;
static int s_snd_count = 0;
void settings_ui_set_sound_table(settings_ui_sound_name_fn fn, int count)
{
    s_snd_fn = fn; s_snd_count = count;
}

/* ---- helpers ---- */
static lv_obj_t *mk_label(lv_obj_t *p, int x, int y, const char *t,
                          const lv_font_t *f, lv_color_t c)
{
    lv_obj_t *l = lv_label_create(p);
    lv_label_set_text(l, t ? t : "");
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, c, 0);
    lv_obj_set_pos(l, x, y);
    return l;
}

static lv_obj_t *mk_rect(lv_obj_t *p, int x, int y, int w, int h, lv_color_t c)
{
    lv_obj_t *r = lv_obj_create(p);
    lv_obj_remove_style_all(r);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(r, x, y);
    lv_obj_set_size(r, w, h);
    lv_obj_set_style_bg_color(r, c, 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(r, 6, 0);
    return r;
}

static screen_id_t cur(void)
{
    return (s_depth > 0) ? s_stack[s_depth - 1] : SCR_NONE;
}

static void render(void);   /* forward */

/* ============================================================================
 * WiFi mode helper (reads the persisted preference)
 * ==========================================================================*/
static bool mode_is_client(void)
{
    return settings_get_wifi_mode() == WIFI_MODE_PREF_CLIENT;
}

/* ============================================================================
 * CONTENT PROVIDER
 * Rows come from a static table or are built at runtime. A row can carry:
 *   label, value (right, amber), a filled triangle (enters), or a substatus
 *   line (coloured, under the label).
 * ==========================================================================*/

/* row descriptor filled by the provider for one visible row */
typedef struct {
    const char *label;
    const char *value;       /* right-aligned text, or "" */
    bool        enter;       /* show filled triangle, tap enters */
    const char *sub;         /* substatus line under label, or NULL */
    lv_color_t  sub_col;     /* substatus colour */
    lv_color_t  val_col;     /* colour for `value` */
    bool        val_col_set; /* true = use val_col instead of the default amber */
    bool        val_small;   /* true = render the value in the small font */
    /* Colour of the LABEL. It serves entries that open a submenu and must
     * still show a state: there the right-hand column is taken by the triangle,
     * and writing ON/OFF over it would destroy the one sign that says "this row
     * opens". The colour carries the state without taking anything away. */
    lv_color_t  lbl_col;
    bool        lbl_col_set; /* true = use lbl_col instead of C_LABEL */
} row_t;

/* ---- Network (mode-conditional) ---- */
/* rows: Mode (toggle), then either Client rows or SoftAP rows, then common */
static const char *wifi_power_str(void)
{
    switch (settings_get_wifi_power()) {
        case WIFI_POWER_LOW:  return "LOW";
        case WIFI_POWER_HIGH: return "HIGH";
        case WIFI_POWER_MID:
        default:              return "MID";
    }
}

static int net_count(void)
{
    /* Status + Power + Mode + [Choose network | AP name + AP password] */
    return mode_is_client() ? 4 : 5;
}

static void net_item(int idx, row_t *r)
{
    bool client = mode_is_client();

    /* row 0: Status (with coloured substatus) - the state at a glance */
    if (idx == 0) {
        r->label = "Status"; r->enter = true;
        if (client) {
            wifi_client_state_t st = wifi_client_state();
            static char ssidbuf[33];
            wifi_client_get_ssid(ssidbuf, sizeof(ssidbuf));
            const char *ssid = ssidbuf[0] ? ssidbuf : "no network";
            r->sub = ssid;   /* the colour conveys the state, not the text */
            if (st == WIFI_CLIENT_CONNECTED)                                    r->sub_col = C_GREEN;
            else if (st == WIFI_CLIENT_CONNECTING || st == WIFI_CLIENT_RETRYING) r->sub_col = C_AMBER;
            else                                                                r->sub_col = C_RED;
        } else {
            static char apname[33];
            settings_get_ap_name(apname, sizeof(apname));   /* the real name, not a fixed one */
            r->sub = apname;          /* AP name; colour = active state */
            r->sub_col = C_GREEN;
        }
        return;
    }

    /* row 1: Power (WiFi TX power: LOW / MID / HIGH) */
    if (idx == 1) {
        r->label = "TX Power";
        r->value = wifi_power_str();
        return;
    }

    /* row 2: Mode toggle */
    if (idx == 2) {
        r->label = "Mode";
        r->value = client ? "Client" : "SoftAP";
        return;
    }

    if (client) {
        /* 3: Choose network */
        switch (idx) {
            case 3: r->label = "Choose network"; r->enter = true; break;
        }
    } else {
        /* SoftAP: 3: AP name, 4: AP password */
        static char apname[33];
        switch (idx) {
            case 3: settings_get_ap_name(apname, sizeof(apname));
                    r->label = "AP name";  r->value = apname; break;
            case 4: r->label = "Password"; r->value = "*****"; break;
        }
    }
}

/* ---- Status detail screen (mode-dependent) ---- */
static int status_count(void)
{
    return mode_is_client() ? 4 : 3;   /* client: SSID, IP, RSSI, TX Power ; AP: IP, clients, TX Power */
}

static void status_item(int idx, row_t *r)
{
    static char v0[40], v1[24], v2[24], v3[24];
    if (mode_is_client()) {
        char ip[16]; bool up = wifi_client_get_ip(ip, sizeof(ip));
        switch (idx) {
            case 0: { r->label = "Network"; char sb[33]; wifi_client_get_ssid(sb, sizeof(sb)); snprintf(v0, sizeof(v0), "%s", sb[0] ? sb : "--"); r->value = v0; break; }
            case 1: r->label = "IP";      snprintf(v1, sizeof(v1), "%s", up ? ip : "--"); r->value = v1; break;
            case 2: r->label = "Signal";  snprintf(v2, sizeof(v2), "%d dBm", wifi_client_get_rssi()); r->value = v2; break;
            case 3: r->label = "TX Power"; snprintf(v3, sizeof(v3), "%d dBm", wifi_core_get_power_dbm()); r->value = v3; break;
        }
    } else {
        switch (idx) {
            case 0: r->label = "IP";      r->value = "172.16.42.1"; break;
            case 1: r->label = "Clients"; snprintf(v0, sizeof(v0), "%d", 0); r->value = v0; break;
            case 2: r->label = "TX Power"; snprintf(v1, sizeof(v1), "%d dBm", wifi_core_get_power_dbm()); r->value = v1; break;
        }
    }
}

/* ---- scan list ---- */
static int scan_count(void) { return s_scan_n == 0 ? 1 : s_scan_n; }


static void scan_item(int idx, row_t *r)
{
    if (s_scan_n == 0) {
        r->label = wifi_core_scanning() ? "Scanning..." : "No networks";
        return;
    }
    if (idx < 0 || idx >= s_scan_n) return;
    r->label = s_scan_aps[idx].ssid;

    /* The network we are ALREADY connected to is marked by a green name.
     *
     * WHY THE LABEL AND NOT THE VALUE: the right-hand column already carries
     * the signal level, with its own colour code (green/amber/red). A second
     * green there would mean two different things in the same place — "good
     * signal" and "you are connected here" — and at a glance they would not be
     * told apart. On the NAME the green is unambiguous, and it is also where
     * the eye looks for the network it is after.
     *
     * Compared only in the CONNECTED state: during the attempt the SSID is
     * already the target one, and colouring it would state something not yet
     * true. */
    if (wifi_client_state() == WIFI_CLIENT_CONNECTED) {
        char cur[33];
        wifi_client_get_ssid(cur, sizeof(cur));
        if (cur[0] && strncmp(cur, s_scan_aps[idx].ssid,
                              sizeof(s_scan_aps[idx].ssid)) == 0) {
            r->lbl_col = C_GREEN;
            r->lbl_col_set = true;
        }
    }

    /* signal strength as dBm, coloured by quality so the list can be scanned at
     * a glance. Small font: SSIDs can be long and would otherwise collide. */
    static char vbuf[16];
    int rssi = s_scan_aps[idx].rssi;
    snprintf(vbuf, sizeof(vbuf), "%d dBm", rssi);
    r->value = vbuf;
    r->val_col = (rssi >= -60) ? C_GREEN : (rssi >= -75) ? C_AMBER : C_RED;
    r->val_col_set = true;
    r->val_small = true;
    r->enter = true;
}

/* ---- unified accessors ---- */
static const char *screen_title(screen_id_t id)
{
    switch (id) {
        case SCR_NET:        return "WiFi";
        case SCR_NET_SCAN:   return "Choose network";
        case SCR_NET_PASS:   return s_te.title ? s_te.title : "Entry";
        case SCR_NET_STATUS: return "Status";
        case SCR_LAN:        return "LAN";
        case SCR_LAN_STATUS: return "Status";
        case SCR_CLOCK:      return "Clock";
        case SCR_SDCARD:     return "SD card";
        case SCR_OSC_CUSTOM: return "Custom";
        case SCR_OSC_MSG:    return "Message";
        /* The keypad serves different fields, so its title is the field's
         * rather than a fixed one. That way the breadcrumb composes itself —
         * "Settings > Clock > Sink > OSC > Host" — instead of being overwritten
         * by hand, which lost the whole path. */
        case SCR_IPENTRY:    return s_ipk_f.title ? s_ipk_f.title : "Entry";
        default:             return s_screens[id].title;
    }
}

/* The breadcrumb, from the nav stack: "Settings > WiFi > Choose network".
 *
 * EVERY LEVEL IS SHOWN, "Settings" included. [M] The longest path the menus can
 * produce — Settings > Clock > Sink > OSC > Custom > Message — was measured on
 * the screen and fits, with nothing to spare. The keyboard screens are not a
 * worry: they carry a short title of their own rather than the path.
 *
 * WHEN IT DOES NOT FIT the TAIL is kept, not the head: the last levels say
 * where one is, the first ones how one got there, and it is the first question
 * being asked. The elision is marked, or a path cut short would read as a
 * shorter path. With no margin left, this is what a longer title lands on. */
static const char *breadcrumb(void)
{
    static char buf[64];

    /* Measured backwards: start from the last level and take as many as fit,
     * which is the same as dropping the oldest. */
    int from = 0;
    for (;;) {
        int n = 0;
        for (int i = from; i < s_depth; i++)
            n += (int)strlen(screen_title(s_stack[i])) + ((i > from) ? 3 : 0);
        if (from > 0) n += 6;                  /* the "... > " marker */
        if (n < (int)sizeof(buf) - 1 || from >= s_depth - 1) break;
        from++;
    }

    int used = 0;
    buf[0] = 0;
    if (from > 0)
        used += snprintf(buf, sizeof(buf), "%s", "... > ");
    for (int i = from; i < s_depth; i++) {
        int add = snprintf(buf + used, sizeof(buf) - used,
                           "%s%s", (i == from) ? "" : " > ",
                           screen_title(s_stack[i]));
        if (add < 0 || used + add >= (int)sizeof(buf)) break;
        used += add;
    }
    return buf;
}

static int screen_count(screen_id_t id)
{
    switch (id) {
        case SCR_NET:        return net_count();
        case SCR_NET_SCAN:   return scan_count();
        case SCR_NET_STATUS: return status_count();
        /* The Custom row belongs to the CUSTOM preset and to no other: with
         * GENERIC or RESOLUME it would open a list that nothing reads. */
        case SCR_BRIDGE_OSC: return (settings_get_osc_preset() == OSC_OUT_CUSTOM)
                                    ? 5 : 4;
        /* The messages plus one "Add" row — unless the list is full, where
         * offering to add would be a row that only ever refuses. */
        case SCR_OSC_CUSTOM: return (s_oc_n ? *s_oc_n : 0) +
                                    ((s_oc_n && *s_oc_n >= OSC_CUSTOM_MAX) ? 0 : 1);
        case SCR_OSC_MSG:    return 4;   /* When, Path, Value, Delete */
        default:             return s_screens[id].count;
    }
}

/* --- the OSC custom profile: names and rows --------------------------------
 *
 * The names are SHORT because they sit in the value column, which the row
 * shares with the label: "EVERY BEAT" fits, "every beat of the bar" does not. */
static const char *osc_trig_name(uint8_t t)
{
    switch (t) {
    case OSC_TRIG_BEAT:      return "EVERY BEAT";
    case OSC_TRIG_DOWNBEAT:  return "DOWNBEAT";
    case OSC_TRIG_TEMPO:     return "TEMPO CHANGE";
    case OSC_TRIG_TRACK:     return "TRACK CHANGE";
    case OSC_TRIG_TRANSPORT: return "PLAY / STOP";
    default:                 return "?";
    }
}


/* A row of the list: the trigger as the label, the path as the value. Reading
 * down the column one sees WHEN each message goes out, which is the question
 * one has when looking at a list of them. */
static void osc_list_row(int idx, row_t *r)
{
    const int n = s_oc_n ? *s_oc_n : 0;
    if (idx >= n) {                      /* the last row, when there is room */
        r->label = "+ Add message";
        return;
    }
    r->label = osc_trig_name(s_oc[idx].trigger);
    r->value = s_oc[idx].path[0] ? s_oc[idx].path : "--";
    r->val_small = true;                 /* paths are long */
    r->enter = true;
}

/* A row of the Message page. */
static void osc_msg_row(int idx, row_t *r)
{
    if (!s_oc || !s_oc_n || s_oc_edit >= *s_oc_n) return;
    const osc_custom_msg_t *m = &s_oc[s_oc_edit];

    switch (idx) {
    case 0: r->label = "When";   r->value = osc_trig_name(m->trigger); break;
    case 1: r->label = "Path";   r->value = m->path[0] ? m->path : "--";
            r->val_small = true; break;
    case 2: r->label = "Value";  r->value = m->value[0] ? m->value : "--";
            r->val_small = true;
            break;
    case 3: r->label = "Delete"; r->lbl_col = C_RED; r->lbl_col_set = true; break;
    default: break;
    }
}


/* fill a row_t for row idx of screen id */
static void screen_row(screen_id_t id, int idx, row_t *r)
{
    r->label = ""; r->value = ""; r->enter = false; r->sub = NULL; r->sub_col = C_MUTED;
    r->lbl_col_set = false;
    r->val_col = C_AMBER; r->val_col_set = false; r->val_small = false;
    r->lbl_col = C_LABEL;   /* initialised even when lbl_col_set is false, so
                             * that screen_signature() can always include it
                             * without repeating the drawing condition */
    switch (id) {
        case SCR_NET:        net_item(idx, r);    return;
        case SCR_NET_SCAN:   scan_item(idx, r);   return;
        case SCR_NET_STATUS: status_item(idx, r); return;
        case SCR_OSC_CUSTOM: osc_list_row(idx, r); return;
        case SCR_OSC_MSG:    osc_msg_row(idx, r);  return;
        default: break;
    }
    /* static screen */
    const menu_item_t *it = &s_screens[id].items[idx];
    r->label = it->label;
    if (it->value)              r->value = it->value;   /* plain value (amber) */
    /* THE TRIANGLE MEANS "A LIST OPENS HERE", not merely "something happens".
     * About is a target like the others as far as navigation goes, but what it
     * opens is a full-screen piece that plays and returns by itself — nothing
     * to move through, nothing to come back from. Promising a submenu there
     * would be promising the wrong thing. */
    if (it->target != SCR_NONE && it->target != SCR_ABOUT)
        r->enter = true;                                /* render draws the triangle */
    /* The WiFi entry does NOT show the current mode.
     *
     * It is a SUBMENU like the others, and a value beside it broke the
     * consistency: entries that lead elsewhere carry the triangle, entries that
     * change something carry a value. Having both confused the two meanings.
     *
     * The mode stays readable in two more appropriate places: the Mode row
     * inside the submenu, and the WIFI badge on the dashboard. */
    /* Device screen: live values for audio and the two brightness levels */
    if (id == SCR_BRIDGE_OSC) {
        static char hostb[20], portb[8];
        switch (idx) {
        case 0: r->value = settings_get_osc_enabled() ? "ON" : "OFF"; break;
        case 1: {
            static const char *nm[OSC_PRESET_COUNT] = { "GENERIC", "RESOLUME", "CUSTOM" };
            uint8_t pr = settings_get_osc_preset();
            r->value = (pr < OSC_PRESET_COUNT) ? nm[pr] : "?";
            break;
        }
        case 2:
            settings_get_osc_host(hostb, sizeof(hostb));
            r->value = hostb[0] ? hostb : "--";
            break;
        case 3:
            snprintf(portb, sizeof(portb), "%u", (unsigned)settings_get_osc_port());
            r->value = portb;
            break;
        default: break;
        }
    }
    if (id == SCR_SINK) {
        /* The state of each transport is read from the COLOUR of the label:
         * green if it is live, the standard colour if it is off.
         *
         * Not from the value on the right: these rows open a submenu, and that
         * column is taken by the triangle that says so. Writing ON/OFF there
         * would remove the triangle, and with it the only hint that the row
         * opens — creating an exception in a list where every other entry with
         * a submenu behaves the same way.
         *
         * GREEN MEANS THE TRANSPORT IS ACTUALLY GOING OUT. For the DIN the
         * setting alone is not enough: without the module nothing leaves, and
         * calling it green would be a lie. The MIDI entry is green if AT LEAST
         * ONE of the three transports is going out: the upper level says
         * "something leaves from here", the detail is read by entering. */
        bool midi_any = settings_get_midi_net_enabled()
                     || (settings_get_midi_din_enabled() && s_din_hw_seen)
                     || (settings_get_midi_usb_enabled() && s_usb_mounted_seen);
        bool on = (idx == 0) ? midi_any
                : (idx == 1) ? settings_get_osc_enabled()
                             : (settings_get_cv_enabled() && s_din_hw_seen);
        if (on) { r->lbl_col = C_GREEN; r->lbl_col_set = true; }
    }
    if (id == SCR_MIDI_NET) {
        if (idx == 0) r->value = settings_get_midi_net_enabled() ? "ON" : "OFF";
        if (idx == 1) r->value = settings_get_midi_net_mdns()    ? "ON" : "OFF";
    }
    if (id == SCR_SYS && idx >= 2 && idx <= 4) {
        /* The three destructive actions. The text says what will ACTUALLY
         * happen, not a generic "are you sure?": that is the only useful
         * information at that moment, and the difference between losing the
         * settings and merely restarting is entirely there. */
        if (confirm_armed(idx)) {
            r->sub     = (idx == 2) ? "TAP AGAIN TO POWER OFF"
                       : (idx == 3) ? "TAP AGAIN TO RESTART"
                                    : "TAP AGAIN: ERASES ALL SETTINGS";
            r->sub_col = C_RED;
            r->val_col = C_RED; r->val_col_set = true;
            r->value   = "?";
        }
    }
    if (id == SCR_SINK_MIDI) {
        bool on = (idx == 0) ? (settings_get_midi_usb_enabled() && s_usb_mounted_seen)
                : (idx == 1) ? (settings_get_midi_din_enabled() && s_din_hw_seen)
                             : settings_get_midi_net_enabled();
        if (on) { r->lbl_col = C_GREEN; r->lbl_col_set = true; }
    }
    if (id == SCR_SINK_CV) {
        if (idx == 0) {
            r->value = settings_get_cv_enabled() ? "ON" : "OFF";
            /* WHERE TO PLUG THE CABLE, said the way the connector is labelled
             * rather than the way the code names the pin: G8 appears nowhere on
             * the housing, while "Port B, pin 1" is what somebody with a jack
             * in hand is actually looking at.
             *
             * (In the firmware it is CV_GPIO 8, the serial console's RX,
             * reclaimed as an output since the log only ever writes.)
             *
             * No warning about the SAM here: the pulse leaves from the GPIO and
             * needs no external hardware. The module is wanted only for the
             * note, which is the row below. */
            r->sub = "OUTPUT ON PORT B - PIN 1";
        }
        if (idx == 1) {
            static const char *pp[4] = { "1 PPQN", "2 PPQN", "4 PPQN", "24 PPQN" };
            uint8_t k = settings_get_cv_ppqn();
            r->value = pp[k < 4 ? k : 0];
        }
        if (idx == 2) {
            static const char *wd[5] = { "1 ms", "2 ms", "5 ms", "10 ms", "20 ms" };
            uint8_t k = settings_get_cv_width();
            r->value = wd[k < 5 ? k : 3];
        }
        if (idx == 3) r->value = settings_get_cv_polarity() ? "IDLE HIGH" : "IDLE LOW";
        if (idx == 4) {
            r->value = s_snd_fn ? s_snd_fn(settings_get_cv_sound()) : "-";
            r->val_small = true;      /* the names are long */
            /* THIS ONE DOES need the module: the note is played by the
             * SAM2695, unlike the pulse above. Same wording as under MIDI DIN,
             * because it is the same absence.
             *
             * KEEP IT SHORT: the sub-line starts at x=24 and the value at
             * x=136, so a long one runs under the other. The limit is in
             * PIXELS, not characters — spaces and punctuation are far narrower
             * than capitals in Montserrat — so a well-spaced phrase fits where
             * a shorter run of solid letters does not. Check it on the screen
             * rather than counting. */
            if (!s_din_hw_seen) {
                r->sub     = "NO MODULE ON PORT C";
                r->sub_col = C_RED;
            }
        }
    }
    if (id == SCR_MIDI_USB && idx == 0) {
        /* Logic REVERSED with respect to the DIN, and the difference is deliberate.
         *
         * For the DIN, "MODULE NOT DETECTED" in red is right: the module ought
         * to be there, and its absence is a problem to act on.
         *
         * For USB it is not. Not being attached to a host is the normal RESTING
         * state: the bridge works perfectly well without a computer connected.
         * Flagging it in red would suggest a fault where nothing is broken. So
         * the POSITIVE is signalled — a successful connection, in green — and
         * silence when there is none. */
        r->value = settings_get_midi_usb_enabled() ? "ON" : "OFF";
        if (s_usb_mounted_seen) {
            r->sub     = "CONNECTED TO USB HOST";
            r->sub_col = C_GREEN;
        }
    }
    if (id == SCR_MIDI_DIN && idx == 0) {
        /* The value always shows the INTENDED state, and is always editable:
         * the setting belongs to the user and the hardware must not be able to
         * take it away. If the module is missing the red subtitle says so, and
         * on reconnecting it the transport restarts by itself with the setting
         * the user had already left there. */
        r->value = settings_get_midi_din_enabled() ? "ON" : "OFF";
        if (!s_din_hw_seen) {
            r->sub     = "NO MODULE ON PORT C";
            r->sub_col = C_RED;
        }
    }
    if (id == SCR_SYS_BRIDGE) {
        if (idx == 0) r->value = settings_get_start_sync() ? "ON" : "OFF";
        if (idx == 1) r->value = settings_get_stop_sync()  ? "ON" : "OFF";
        if (idx == 2) r->value = settings_get_drift_comp() ? "ON" : "OFF";
        if (idx == 3) r->value = settings_get_remember_bridge() ? "ON" : "OFF";
    }
    if (id == SCR_SYS_INFO) {
        /* Live values: when they are unavailable the dash from the static
         * table remains, never an invented number. */
        static char fwb[32], mcb[24], upb[16], tbuf[16];
        switch (idx) {
        case 0: {   /* Firmware: the version if set, else the build date */
            const esp_app_desc_t *d = esp_app_get_description();
            if (d) {
                /* The explicit precision is for the compiler: the fields of
                 * esp_app_desc_t are arrays of 32 and 16 bytes, not strings of
                 * known length, and without a ceiling it warns about possible
                 * truncation. They would not fit the UI row anyway. */
                if (d->version[0] && strcmp(d->version, "1") != 0)
                    snprintf(fwb, sizeof(fwb), "%.31s", d->version);
                else
                    snprintf(fwb, sizeof(fwb), "%.15s", d->date);
                r->value = fwb;
            }
            break;
        }
        case 1:     /* Full MAC of the DJ Link interface: it is what the players
                     * see in our keep-alives, and comparing it against a capture
                     * needs it whole.
                     *
                     * It stays in the normal font: the "MAC" label is short and
                     * the value field is 150 px wide, right-aligned, which is
                     * enough for 17 characters. Should it ever come out clipped,
                     * the levers are val_small or widening s_row_val. */
            if (s_ident_mac[0] || s_ident_mac[3]) {
                snprintf(mcb, sizeof(mcb), "%02X:%02X:%02X:%02X:%02X:%02X",
                         s_ident_mac[0], s_ident_mac[1], s_ident_mac[2],
                         s_ident_mac[3], s_ident_mac[4], s_ident_mac[5]);
                r->value = mcb;
            }
            break;
        case 2: {   /* Uptime, compact: seconds, then minutes, then hours. */
            int64_t s = esp_timer_get_time() / 1000000;
            if      (s < 60)    snprintf(upb, sizeof(upb), "%ds", (int)s);
            else if (s < 3600)  snprintf(upb, sizeof(upb), "%dm %ds",
                                         (int)(s / 60), (int)(s % 60));
            else if (s < 86400) snprintf(upb, sizeof(upb), "%dh %dm",
                                         (int)(s / 3600), (int)((s % 3600) / 60));
            else                snprintf(upb, sizeof(upb), "%dd %dh",
                                         (int)(s / 86400), (int)((s % 86400) / 3600));
            r->value = upb;
            break;
        }
        case 3: {   /* die temperature, read on every render */
            float c = 0.0f;
            if (bsp_temp_read(&c)) {
                snprintf(tbuf, sizeof(tbuf), "%d.%d C",
                         (int)c, (int)((c - (int)c) * 10));
                r->value = tbuf;
            }
            break;
        }
        default: break;
        }
    }
    if (id == SCR_SDCARD) {
        if (idx == 0) {
            r->value = settings_get_sd_playlist() ? "ON" : "OFF";
            /* The switch can be on with no card in the slot: the setting is
             * what the user wants, not what is possible right now. The sub-line
             * says so, rather than letting a green ON suggest that something is
             * being written. */
            if (sdcard_state() == SDCARD_ABSENT && settings_get_sd_playlist()) {
                r->sub     = "NO CARD IN THE SLOT";
                r->sub_col = C_RED;
            }
        }
        if (idx == 1) {
            static char sb[12];
            snprintf(sb, sizeof(sb), "%u s", (unsigned)settings_get_sd_threshold_s());
            r->value = sb;
        }
        if (idx == 2) {
            /* GREY MEANS UNAVAILABLE, and needs no sentence to explain itself:
             * with no card there is nothing to erase, and with a bridge running
             * the format would hold the bus the beats go out on. The second
             * line is for what an operation is DOING, not for why one cannot
             * start. */
            const sdcard_format_t f = sdcard_format_state();
            const bool blocked = (sdcard_state() == SDCARD_ABSENT) ||
                                 s_bridge_active ||
                                 f == SDCARD_FMT_BUSY || f == SDCARD_FMT_FAILED;
            if (blocked) { r->lbl_col = C_MUTED2; r->lbl_col_set = true; }

            /* "~2 MIN" IS AN ORDER OF MAGNITUDE, not a promise. [M] 98 s on
             * one card; a larger or slower one takes longer, and the figure
             * belongs to that card rather than to the operation. What the
             * user needs is to know this is minutes and not seconds, because
             * the screen cannot redraw meanwhile — the card holds the bus,
             * and GPIO 35 is the display's DC line as well as its MISO. */
            if (f == SDCARD_FMT_BUSY) {
                r->sub     = "FORMATTING, PLEASE WAIT ~2 MIN";
                r->sub_col = C_AMBER;
            } else if (f == SDCARD_FMT_DONE) {
                r->sub     = "CARD FORMATTED";
                r->sub_col = C_GREEN;
            } else if (f == SDCARD_FMT_FAILED) {
                r->sub     = "FORMAT FAILED";
                r->sub_col = C_RED;
            } else if (confirm_armed(idx)) {
                r->sub     = "TAP AGAIN: ERASES THE CARD";
                r->sub_col = C_RED;
                r->val_col = C_RED; r->val_col_set = true;
                r->value   = "?";
            }
        }
    }

#if DJB_DEVELOPER
    if (id == SCR_SYS_DEV && idx == 0) {
        r->value = settings_get_screenshots() ? "ON" : "OFF";
        if (settings_get_screenshots() && sdcard_state() == SDCARD_ABSENT) {
            r->sub     = "NO CARD IN THE SLOT";
            r->sub_col = C_RED;
        }
    }
#endif

    if (id == SCR_LAN) {
        /* The one thing there is to decide here. In OBSERVER we stay a virtual
         * device: no player number, and therefore no metadata and no driving
         * the players — the note beside the setting in main.c has the full
         * cost. */
        if (idx == 1) r->value = settings_get_djlink_observer() ? "OBSERVER" : "AUTO";
    }
    if (id == SCR_LAN_STATUS) {
        static char ipb[20], idb[8];
        switch (idx) {
        case 0:     /* the address the chip answers to right now */
            if (s_ident_ip[0] || s_ident_ip[3]) {
                snprintf(ipb, sizeof(ipb), "%u.%u.%u.%u",
                         s_ident_ip[0], s_ident_ip[1], s_ident_ip[2], s_ident_ip[3]);
                r->value = ipb;
            }
            break;
        case 1:
            /* HOW it was obtained, which the address alone does not say: a
             * 169.254 could be a link-local we chose or one a server handed
             * out, and knowing which tells whether a DHCP server was found. */
            r->value = dhcp_lite_state() == DHCP_ST_OFF ? "auto-ip" : "DHCP";
            break;
        case 2:     /* the player number actually held, not the one wanted */
            if (s_ident_dev) {
                snprintf(idb, sizeof(idb), "%u", (unsigned)s_ident_dev);
                r->value = idb;
            }
            break;
        default: break;
        }
    }

    if (id == SCR_DEVICE) {
        static const char *bn[3]  = { "LOW", "MED", "HIGH" };
        static const char *lbn[4] = { "OFF", "LOW", "MED", "HIGH" };
        if (idx == 0) r->value = audio_get_enabled() ? "ON" : "OFF";
        if (idx == 1) r->value = lbn[settings_get_led_brightness()];
        if (idx == 2) r->value = bn[settings_get_display_brightness()];
        if (idx == 3) r->value = display_flipped() ? "ON" : "OFF";
    }
}

/* ============================================================================
 * REFRESH TIMER (scan progress + live status)
 * ==========================================================================*/
static lv_timer_t *s_refresh_timer = NULL;

/* The screens whose values change BY THEMSELVES, without the user touching
 * anything. One function, because the list is needed in two places — the tick
 * that switches itself off, and the arming after navigation — and two lists
 * kept aligned by hand are a defect waiting to happen: add one to only one of
 * them and the screen stays frozen, or the timer never stops.
 *
 * MIDI DIN belongs here because the module detection is continuous: unplugging
 * or reconnecting the cable must change the row while one is looking at it. So
 * does the MIDI level, where the green of the DIN entry depends on the same
 * measurement.
 *
 * About is "live" not because its content changes but because a tick is needed
 * to notice the end of the intro. Without it the timer would not run and nobody
 * would be listening for on_fx_done. */
static bool screen_is_live(screen_id_t id)
{
    if (id == SCR_ABOUT) return true;

    /* LIVE MEANS "something here can change without the user touching it".
     *
     * The refresh does NOT redraw on a timer: it computes a signature of what
     * the rows would say and redraws only when that differs — see
     * screen_signature. So listing a screen here costs a comparison every
     * 400 ms, not a repaint.
     *
     * SD card is live because the slot is: a card going in or coming out
     * changes what "Store playlist" has to say about itself, and a page that
     * only tells the truth when reopened is worse than one that says nothing.
     *
     * LAN > Status likewise: the address can change under the reader when a
     * lease is renewed or a cable is pulled and put back. */
    return id == SCR_NET_SCAN || id == SCR_NET || id == SCR_NET_STATUS ||
           id == SCR_LAN_STATUS || id == SCR_SDCARD ||
           id == SCR_OSC_CUSTOM || id == SCR_OSC_MSG ||
           id == SCR_SYS_INFO || id == SCR_SINK || id == SCR_MIDI_DIN ||
           id == SCR_MIDI_USB || id == SCR_SINK_CV ||
           id == SCR_SINK_MIDI;
}

/* A fingerprint of WHAT IS ON SCREEN: the screen, the scroll position, the
 * number of rows, and for every visible row its text, colours and triangle.
 *
 * It is needed because the timer beats at 2.5 Hz and render() redraws
 * EVERYTHING: without a comparison the whole screen would be invalidated ten
 * times every four seconds while sitting still on a menu that is not changing —
 * and [M] every redraw is SPI bus time taken from the network, up to 7.5 ms for
 * a full area.
 *
 * A FINGERPRINT is compared rather than the individual values because it is
 * generic: it works for any screen, present or future, without listing by hand
 * what can change in each. It costs a few microseconds — five rows of short
 * strings — against milliseconds of drawing.
 *
 * THE COLOURS COUNT AS MUCH AS THE TEXT: the "MIDI DIN" entry turns green
 * without a single letter changing, and with a text-only signature it would
 * stay grey. */
static inline uint32_t sig_bytes(uint32_t h, const void *p, size_t n)
{
    const uint8_t *b = (const uint8_t *)p;
    while (n--) { h ^= *b++; h *= 16777619u; }
    return h;
}

static inline uint32_t sig_str(uint32_t h, const char *s)
{
    static const char nul = 0;
    return s ? sig_bytes(h, s, strlen(s) + 1) : sig_bytes(h, &nul, 1);
}

static uint32_t screen_signature(void)
{
    screen_id_t id = cur();
    int count = screen_count(id);
    uint32_t h = 2166136261u;                    /* FNV-1a */
    h = sig_bytes(h, &id, sizeof(id));
    h = sig_bytes(h, &s_scroll, sizeof(s_scroll));
    h = sig_bytes(h, &count, sizeof(count));
    for (int i = 0; i < ROWS_VISIBLE; i++) {
        int idx = s_scroll + i;
        if (idx >= count) break;
        row_t r;
        screen_row(id, idx, &r);
        h = sig_str(h, r.label);
        h = sig_str(h, r.value);
        h = sig_str(h, r.sub);
        h = sig_bytes(h, &r.enter,       sizeof(r.enter));
        h = sig_bytes(h, &r.sub_col,     sizeof(r.sub_col));
        h = sig_bytes(h, &r.val_col,     sizeof(r.val_col));
        h = sig_bytes(h, &r.val_col_set, sizeof(r.val_col_set));
        h = sig_bytes(h, &r.val_small,   sizeof(r.val_small));
        h = sig_bytes(h, &r.lbl_col,     sizeof(r.lbl_col));
        h = sig_bytes(h, &r.lbl_col_set, sizeof(r.lbl_col_set));
    }
    return h;
}

static void refresh_tick(lv_timer_t *t)
{
    (void)t;
    screen_id_t id = cur();

    /* The intro has ended, on a touch or at the timeout, and asked to go back.
     * It is done HERE and not in its callback because that runs on the intro's
     * task: nav_pop touches LVGL objects, and those must be touched by the LVGL
     * task. */
    if (s_fx_done) {
        s_fx_done = false;
        if (id == SCR_ABOUT) {
            /* --- EVERYTHING IS REDRAWN, AND IT IS NECESSARY ---------------
             *
             * The intro writes DIRECTLY into the framebuffer, without going
             * through LVGL. When it ends, LVGL does not know those pixels have
             * changed: none of its objects have moved, so it has nothing to
             * redraw, and the last frame of the intro stays on screen —
             * underneath the menu entries.
             *
             * It must be done HERE, where the intro has already finished
             * drawing: asking for it while the intro is still running would
             * achieve nothing, because the next frame would cover everything
             * again.
             *
             * AFTER nav_pop, not before. nav_pop changes screen and puts the
             * menu entries back: invalidating first means declaring the OLD
             * state dirty, and there is no guarantee that area survives the
             * change. Invalidating afterwards declares dirty what is on screen
             * now, which is what has to be covered.
             *
             * The ACTIVE screen is invalidated rather than s_scr: if nav_pop
             * led elsewhere, s_scr would no longer be the one in front and the
             * wrong screen would be cleaned — silently. */
            nav_pop();
            lv_obj_invalidate(lv_screen_active());
            ESP_LOGI("about", "intro finished: screen redrawn");
            return;
        }
    }
    /* keep the timer only while on a screen that needs live updates */
    if (!screen_is_live(id)) {
        if (s_refresh_timer) { lv_timer_del(s_refresh_timer); s_refresh_timer = NULL; }
        return;
    }
    /* The scan must be COLLECTED regardless, before the comparison: it is data
     * arriving from outside, and without reading it the fingerprint would never
     * change. */
    if (id == SCR_NET_SCAN) {
        s_scan_n = wifi_core_scan_get_results(s_scan_aps, SCAN_MAX);
    }
    static uint32_t p_sig = 0;
    uint32_t sig = screen_signature();
    if (sig == p_sig) return;      /* nothing changed: the screen is left alone */
    p_sig = sig;
    render();
}

/* The Info page has a live value, the temperature, so it belongs among those
 * that keep the refresh timer running. */
static void ensure_refresh_timer(void)
{
    if (!s_refresh_timer) {
        s_refresh_timer = lv_timer_create(refresh_tick, 400, NULL);  /* ~2.5 Hz */
    }
}

/* Arm the refresh timer if the screen we just landed on needs live updates.
 * Must run after EVERY navigation change, not just push: refresh_tick deletes
 * itself while on a static screen (e.g. text entry), so coming back via pop
 * would otherwise leave the screen frozen on its last render. */
static void arm_refresh_if_needed(void)
{
    if (screen_is_live(cur())) ensure_refresh_timer();
}

/* ============================================================================
 * PASSWORD ENTRY SCREEN (custom layout)
 * ==========================================================================*/
/* show/hide all password-screen objects together */
static void pw_modes_layout(void);   /* fwd: it has the last word, see below */

static void pw_screen_show(bool on)
{
    lv_obj_t *objs[] = {
        s_pw_field, s_pw_fieldbg, s_pw_ssid, s_pw_sel_box,
        s_pw_sel[0], s_pw_sel[1], s_pw_sel[2], s_pw_sel[3], s_pw_sel[4],
        s_pw_mode_bg[0], s_pw_mode_bg[1], s_pw_mode_bg[2], s_pw_mode_bg[3],
        s_pw_mode_bg[4],
        s_pw_mode_lbl[0], s_pw_mode_lbl[1], s_pw_mode_lbl[2], s_pw_mode_lbl[3],
        s_pw_mode_lbl[4],
        s_pw_act_bg[0], s_pw_act_bg[1], s_pw_act_bg[2],
        s_pw_act_lbl[0], s_pw_act_lbl[1], s_pw_act_lbl[2],
    };
    for (unsigned i = 0; i < sizeof(objs)/sizeof(objs[0]); i++) {
        if (!objs[i]) continue;
        if (on) lv_obj_clear_flag(objs[i], LV_OBJ_FLAG_HIDDEN);
        else    lv_obj_add_flag(objs[i], LV_OBJ_FLAG_HIDDEN);
    }

    /* THE LAYOUT HAS THE LAST WORD. Showing the screen reveals EVERY object,
     * including the fifth mode key and the two outer wheel cells that the
     * placeholder layout deliberately hides — and render() calls this on every
     * repaint, so the hiding was undone continuously. [M] It showed as the
     * outer cells reappearing at their old narrow width, overlapping the wide
     * ones: a long press on backspace made it unmistakable, but any redraw did
     * it.
     *
     * Rather than teaching this function which objects are conditional, it
     * hands the question back to the one place that knows. */
    if (on) pw_modes_layout();
}

/* redraw the entry screen contents from state + context */
/* The set in use: the context's dedicated one if there is one, otherwise the
 * standard one chosen with the mode keys. One place only, so that drawing and
 * hit testing cannot diverge. */
static const char *pw_set_active(void)
{
    return s_te.charset ? s_te.charset : s_charset[s_pw_set];
}

/* True while the wheel is scrolling PLACEHOLDER NAMES rather than letters. */
static bool pw_in_ph(void)
{
    return s_te.placeholders && s_pw_set == PW_MODE_PH;
}

/* How many entries the wheel has in the current mode. */
static int pw_items(void)
{
    if (!pw_in_ph()) return (int)strlen(pw_set_active());
    int n = 0;
    while (osc_out_placeholder(n)) n++;
    return n;
}

static void pw_render(void);   /* forward: used by the error timeout */

/* ---- transient validation error ----
 * The error takes over the title bar (centred, red) because the top-right hint
 * area shares the header band with the breadcrumb and the two would collide.
 * It auto-hides after a short time, and any user action clears it at once. */
#define TE_ERR_MS 2200

static void te_error_restore_title(void)
{
    lv_obj_set_pos(s_title, 12, 12);
    lv_obj_set_width(s_title, LV_SIZE_CONTENT);
    lv_obj_set_style_text_align(s_title, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_color(s_title, C_CYAN, 0);
    /* the entry screen keeps its short title; elsewhere the full breadcrumb */
    if (cur() == SCR_NET_PASS)
        lv_label_set_text(s_title, s_te.title ? s_te.title : "Entry");
    else
        lv_label_set_text(s_title, breadcrumb());
}

static void te_error_clear(void)
{
    if (s_te_err_timer) { lv_timer_del(s_te_err_timer); s_te_err_timer = NULL; }
    if (!s_te_err) return;
    s_te_err = NULL;
    te_error_restore_title();
}

static void te_error_timeout_cb(lv_timer_t *t)
{
    (void)t;
    s_te_err_timer = NULL;      /* one-shot: LVGL deletes it after this call */
    s_te_err = NULL;
    te_error_restore_title();
    if (cur() == SCR_NET_PASS) pw_render();   /* bring the length hint back */
}

static void te_error_show(const char *msg)
{
    s_te_err = msg;
    /* centred over the full width, replacing the breadcrumb */
    lv_obj_set_pos(s_title, 0, 12);
    lv_obj_set_width(s_title, 320);
    lv_obj_set_style_text_align(s_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_title, C_RED, 0);
    lv_label_set_text(s_title, msg);
    /* (re)arm the auto-hide */
    if (s_te_err_timer) lv_timer_del(s_te_err_timer);
    s_te_err_timer = lv_timer_create(te_error_timeout_cb, TE_ERR_MS, NULL);
    lv_timer_set_repeat_count(s_te_err_timer, 1);   /* one-shot */
}

/* Places the mode keys, and the wheel with them.
 *
 * TWO LAYOUTS, chosen by whether the field offers placeholders:
 *
 *   without   4 keys of 68 px, wheel of 5 cells of 20 — unchanged, which is
 *             what every existing entry screen gets
 *   with      5 keys of 52 px, and in placeholder mode a wheel of 3 cells of
 *             86, because a name needs the room a letter does not
 *
 * Called on entering the screen and on every mode change: the wheel's shape
 * follows the mode, not merely the field. */
static void pw_modes_layout(void)
{
    const bool ph   = s_te.placeholders;
    const int  n    = ph ? PW_MODES : 4;
    const int  w    = ph ? 52 : 68;
    const int  step = ph ? 59 : 74;

    for (int i = 0; i < PW_MODES; i++) {
        if (i >= n) {                       /* the fifth, where it does not apply */
            lv_obj_add_flag(s_pw_mode_bg[i],  LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_pw_mode_lbl[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        const int x = 12 + i * step;
        lv_obj_set_pos(s_pw_mode_bg[i], x, 150);
        lv_obj_set_size(s_pw_mode_bg[i], w, 30);
        lv_obj_set_pos(s_pw_mode_lbl[i], x, 157);
        lv_obj_set_width(s_pw_mode_lbl[i], w);
    }

    /* THE WHEEL. In placeholder mode three wide cells; otherwise the five
     * narrow ones. The two outer cells are hidden rather than moved: there is
     * no room for five names, and a name cut in half reads as another name. */
    const bool wide = ph && (s_pw_set == PW_MODE_PH);
    const int  cw   = wide ? 86 : 20;
    const int  cstep= wide ? 92 : 34;
    for (int i = 0; i < 5; i++) {
        if (wide && (i == 0 || i == 4)) {
            lv_obj_add_flag(s_pw_sel[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_remove_flag(s_pw_sel[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(s_pw_sel[i], 160 + (i - 2) * cstep - cw / 2, 106);
        lv_obj_set_width(s_pw_sel[i], cw);
    }
    /* The highlight box follows the centre cell. */
    lv_obj_set_pos(s_pw_sel_box, 160 - (wide ? 48 : 20), 96);
    lv_obj_set_size(s_pw_sel_box, wide ? 96 : 40, 40);
}


static void pw_render(void)
{
    /* top-right: the accepted length for this field. Hidden while a validation
     * error is showing, since the error spans the whole header band. */
    if (s_te_err) {
        lv_obj_add_flag(s_pw_ssid, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_label_set_text(s_pw_ssid, s_te.hint ? s_te.hint : "");
        lv_obj_clear_flag(s_pw_ssid, LV_OBJ_FLAG_HIDDEN);
    }

    /* the typed text. Secret fields mask everything except the last character,
     * which stays readable so you can check what you just typed. */
    if (s_pw_buf[0] == 0) {
        lv_label_set_text(s_pw_field, "");
    } else if (s_te.secret) {
        static char masked[65];
        int n = s_pw_len < 64 ? s_pw_len : 64;
        for (int i = 0; i < n - 1; i++) masked[i] = '*';
        masked[n - 1] = s_pw_buf[n - 1];     /* last char in clear */
        masked[n] = 0;
        lv_label_set_text(s_pw_field, masked);
    } else {
        lv_label_set_text(s_pw_field, s_pw_buf);
    }

    /* selector: 5 chars around the highlighted index in the active set */
    const char *set = pw_set_active();
    const int   n   = pw_items();
    for (int i = 0; i < 5; i++) {
        int idx = s_pw_char + (i - 2);         /* -2..+2 around the centre */
        if (pw_in_ph()) {
            /* NAMES WITHOUT BRACES. The braces are syntax, added when the name
             * is inserted: showing them would cost four characters of a cell
             * and tell the user about a detail they never have to type. */
            const char *nm = (idx >= 0 && idx < n) ? osc_out_placeholder(idx) : "";
            lv_label_set_text(s_pw_sel[i], nm ? nm : "");
        } else {
            char c[2] = { 0, 0 };
            if (idx >= 0 && idx < n) c[0] = set[idx];
            lv_label_set_text(s_pw_sel[i], c);
        }
    }
    /* The centre character turns red for the duration of the rejection. */
    lv_obj_set_style_text_color(s_pw_sel[2], s_pw_reject ? C_RED : C_GREEN, 0);

    /* Mode keys: with a dedicated character set there is nothing to choose, so
     * they disappear rather than sitting there inert.
     *
     * HOW MANY there are, and where, is pw_modes_layout's business — called on
     * entering and on every mode change. Here only their COLOUR is decided, so
     * the two do not both claim the geometry. */
    const int modes = s_te.placeholders ? PW_MODES : 4;
    for (int i = 0; i < modes; i++) {
        if (s_te.charset) {
            lv_obj_add_flag(s_pw_mode_bg[i],  LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_pw_mode_lbl[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_remove_flag(s_pw_mode_bg[i],  LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_pw_mode_lbl[i], LV_OBJ_FLAG_HIDDEN);
        bool active = (i == s_pw_set);
        lv_obj_set_style_bg_color(s_pw_mode_bg[i], active ? C_CYAN : C_ROW, 0);
        lv_obj_set_style_text_color(s_pw_mode_lbl[i], active ? C_BG : C_MUTED, 0);
    }
}

/* Open the generic text-entry screen. Copies the context and pre-fills the
 * buffer with initial_value (may be NULL/empty). Caller then nav_push()es
 * SCR_NET_PASS, or we could do it here; we let the caller navigate. */
static void text_entry_open(const text_entry_ctx_t *ctx, const char *initial_value)
{
    s_te = *ctx;
    if (initial_value) {
        strncpy(s_pw_buf, initial_value, sizeof(s_pw_buf) - 1);
        s_pw_buf[sizeof(s_pw_buf) - 1] = 0;
    } else {
        s_pw_buf[0] = 0;
    }
    s_pw_len = (int)strlen(s_pw_buf);
    s_pw_set = 0; s_pw_char = 0;
    te_error_clear();                /* no stale error from a previous entry */
}

static void pw_enter_screen(void)
{
    /* state (buffer/len) is already set by text_entry_open; just reset the
     * selector position and paint. */
    s_pw_set = 0; s_pw_char = 0;
    pw_modes_layout();      /* four keys or five, by what the field offers */
    pw_render();
}

static void pw_char_prev(void)
{
    if (s_pw_char > 0) { s_pw_char--; te_error_clear(); audio_play(AUDIO_PHASE_STEP); pw_render(); }
}

static void pw_char_next(void)
{
    const int n = pw_items();      /* names in placeholder mode, letters otherwise */
    if (s_pw_char < n - 1) { s_pw_char++; te_error_clear(); audio_play(AUDIO_PHASE_STEP); pw_render(); }
}
static void pw_reject_timeout_cb(lv_timer_t *t)
{
    (void)t;
    s_pw_reject = false;
    s_pw_reject_timer = NULL;      /* one-shot: LVGL deletes it itself */
    pw_render();
}

/* Visible rejection: the character flashes red and the error line explains. */
static void pw_reject(const char *why)
{
    s_pw_reject = true;
    if (s_pw_reject_timer) lv_timer_del(s_pw_reject_timer);
    s_pw_reject_timer = lv_timer_create(pw_reject_timeout_cb, TE_REJECT_MS, NULL);
    lv_timer_set_repeat_count(s_pw_reject_timer, 1);
    audio_play(AUDIO_REJECT);      /* with sound off, the colour remains */
    if (why) te_error_show(why);   /* te_error_show redraws by itself */
    else     pw_render();
}

static void pw_add_char(void)
{
    /* respect the field's max length (falls back to the buffer size) */
    int cap = (int)sizeof(s_pw_buf) - 1;
    if (s_te.max_len > 0 && s_te.max_len < cap) cap = s_te.max_len;
    if (s_pw_len >= cap) { pw_reject("max length"); return; }

    /* A PLACEHOLDER GOES IN WHOLE, braces and all: it is one choice for the
     * user, so it must be one action — and backspacing it character by
     * character is the price, which seven taps makes plain enough. */
    if (pw_in_ph()) {
        const char *nm = osc_out_placeholder(s_pw_char);
        if (!nm) return;
        const int add = (int)strlen(nm) + 2;          /* the two braces */
        if (s_pw_len + add > cap) { pw_reject("max length"); return; }
        s_pw_len += snprintf(s_pw_buf + s_pw_len, sizeof(s_pw_buf) - s_pw_len,
                             "{%s}", nm);
        te_error_clear();
        audio_play(AUDIO_NAV_BEEP);
        pw_render();
        return;
    }

    char c = pw_set_active()[s_pw_char];

    /* Per-character filter: rejected BEFORE writing, with an explanation. */
    if (s_te.accept_char) {
        const char *why = s_te.accept_char(s_pw_buf, c);
        if (why) { pw_reject(why); return; }
    }

    s_pw_buf[s_pw_len++] = c;
    s_pw_buf[s_pw_len] = 0;
    te_error_clear();             /* any action hides the error */
    audio_play(AUDIO_NAV_BEEP);   /* nav beep: char selected */
    pw_render();
}
static void pw_set_charset(int set)
{
    /* The fifth mode exists only where the field offers placeholders. */
    const int n = s_te.placeholders ? PW_MODES : 4;
    if (set >= 0 && set < n) {
        s_pw_set = set;
        s_pw_char = 0;      /* reset to the first entry of the new mode */
        te_error_clear();   /* any action hides the error */
        /* THE WHEEL CHANGES SHAPE WITH THE MODE, so the layout is redone here
         * and not only on entering the screen. */
        pw_modes_layout();
        audio_play(AUDIO_NAV_BEEP);
        pw_render();
    }
}
static void pw_backspace(void)
{
    /* The beep ALWAYS sounds, even on an empty field: it confirms the key was
     * pressed, not that something was removed. A rejection would say the
     * gesture failed, and it did not — it succeeded, there was simply nothing
     * to remove — and silence on an empty field would be indistinguishable
     * from a missed touch. */
    audio_play(AUDIO_NAV_BEEP);
    if (s_pw_len > 0) {
        s_pw_buf[--s_pw_len] = 0;
        te_error_clear();
        pw_render();
    }
}
static void pw_space(void)
{
    /* same length cap as pw_add_char */
    int cap = (int)sizeof(s_pw_buf) - 1;
    if (s_te.max_len > 0 && s_te.max_len < cap) cap = s_te.max_len;
    if (s_pw_len < cap) {
        s_pw_buf[s_pw_len++] = ' ';
        s_pw_buf[s_pw_len] = 0;
        te_error_clear();
        audio_play(AUDIO_NAV_BEEP);
        pw_render();
    }
}

/* forward decls used by pw_connect */
static void nav_pop(void);
static void nav_pop_to(screen_id_t id);
static void nav_push(screen_id_t id);

static void request_connect(const char *ssid, const char *pass)
{
    if (s_connect_handler) s_connect_handler(ssid, pass);
}

/* ---- confirm callbacks: one per editable field ---- */

/* client password: connect to the selected network with the typed password */
/* true if the string is made only of hex digits */
static bool all_hex(const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        if (!hex) return false;
    }
    return true;
}

/* Validate a network key against the AP's authentication mode. Key length rules
 * differ per encryption type:
 *   OPEN : no key at all
 *   WEP  : 5 or 13 ASCII chars (64/128-bit), or 10/26 hex digits
 *   WPA* : 8..63 ASCII passphrase, or exactly 64 hex digits (raw PSK)
 * Returns NULL if acceptable, or a short error message. */
static const char *validate_key(uint8_t authmode, const char *text)
{
    size_t n = text ? strlen(text) : 0;

    if (authmode == WIFI_AUTH_OPEN)
        return NULL;                       /* open network: nothing to check */

    /* Enterprise networks need an identity + credentials (EAP), which this UI
     * can't collect: tell the user instead of failing silently at connect time */
    if (authmode == WIFI_AUTH_WPA2_ENTERPRISE)
        return "enterprise n/a";

    if (authmode == WIFI_AUTH_WEP) {
        if (n == 5 || n == 13) return NULL;               /* ASCII key */
        if ((n == 10 || n == 26) && all_hex(text, n)) return NULL;  /* hex key */
        return "WEP: 5 or 13 chars";
    }

    /* WPA / WPA2 / WPA3 / WAPI and anything else PSK-based */
    if (n == 64 && all_hex(text, n)) return NULL;         /* raw PSK in hex */
    if (n < 8)  return "min 8 chars";
    if (n > 63) return "max 63 chars";
    return NULL;
}

static const char *te_confirm_client_pass(const char *text)
{
    const char *err = validate_key(s_sel_auth, text);
    if (err) return err;
    request_connect(s_sel_ssid, text);
    return NULL;
}

/* ---- openers: build the context and navigate to the entry screen ---- */

/* open the entry screen to type the password for the selected (secured) network */
static void open_entry_client_pass(void)
{
    /* the accepted key length depends on the encryption: WEP keys are short
     * (max 26 hex), WPA allows up to a 64-hex raw PSK */
    bool wep = (s_sel_auth == WIFI_AUTH_WEP);
    int cap = wep ? 26 : 64;
    text_entry_ctx_t ctx = {
        .title      = "Password",
        .hint       = wep ? "5 or 13 chars" : "8-63 chars",   /* accepted length */
        .secret     = true,
        .max_len    = cap,
        .on_confirm = te_confirm_client_pass,
        .return_to  = SCR_NET,   /* not to the scan list */
    };
    text_entry_open(&ctx, NULL);           /* password always starts empty */
    nav_push(SCR_NET_PASS);
}

/* ---- OSC: destination address and port ---- */

/* --- Path and Value of a custom OSC message --------------------------------
 *
 * Both accept PLACEHOLDERS, chosen from a mode of the keyboard rather than
 * typed: see the note in osc_out.h. Nothing is validated beyond the length —
 * a path that means nothing to the receiving software is not something this
 * device can know, and refusing one that looks odd would block the unusual
 * cases that are exactly what a custom profile is for.
 *
 * The leading slash is the one exception: OSC requires it, and a path without
 * one is refused by every receiver in silence. */
static const char *te_confirm_osc_path(const char *text)
{
    if (!s_oc || !s_oc_n || s_oc_edit >= *s_oc_n) return NULL;
    if (!text || !text[0]) return "path required";
    if (text[0] != '/')    return "must start with /";
    snprintf(s_oc[s_oc_edit].path, OSC_PATH_MAX, "%s", text);
    osc_touched();
    return NULL;
}

static const char *te_confirm_osc_value(const char *text)
{
    if (!s_oc || !s_oc_n || s_oc_edit >= *s_oc_n) return NULL;
    /* An empty value is legitimate: with a format of none nothing is sent, and
     * clearing it is how one goes back to that. */
    snprintf(s_oc[s_oc_edit].value, OSC_VALUE_MAX, "%s", text ? text : "");
    osc_touched();
    return NULL;
}

static void open_entry_osc(bool is_path)
{
    if (!s_oc || !s_oc_n || s_oc_edit >= *s_oc_n) return;
    const osc_custom_msg_t *m = &s_oc[s_oc_edit];
    text_entry_ctx_t ctx = {
        .title      = is_path ? "Path" : "Value",
        .hint       = is_path ? "/your/address" : "{bpm} or plain text",
        .secret     = false,
        .max_len    = is_path ? OSC_PATH_MAX - 1 : OSC_VALUE_MAX - 1,
        .on_confirm = is_path ? te_confirm_osc_path : te_confirm_osc_value,
        /* THE VALUE ONLY. An OSC address is a fixed name by convention — the
         * receiving software matches on it — so a placeholder inside one
         * would address something different at every beat. The fifth key does
         * not appear while editing the path. */
        .placeholders = !is_path,
        .return_to  = SCR_OSC_MSG,
    };
    text_entry_open(&ctx, is_path ? m->path : m->value);
    nav_push(SCR_NET_PASS);
}


/* AP name: save the new SoftAP SSID */
static const char *te_confirm_ap_name(const char *text)
{
    size_t n = text ? strlen(text) : 0;
    if (n == 0)  return "name required";
    if (n > 32)  return "max 32 chars";          /* SSID limit */
    char cur[33];
    settings_get_ap_name(cur, sizeof(cur));
    if (strcmp(cur, text) == 0) return NULL;     /* unchanged: accept, do nothing */
    settings_set_ap_name(text);
    /* recreate the AP at runtime, but only if we're actually in SoftAP mode */
    if (!mode_is_client() && s_ap_restart_handler) s_ap_restart_handler();
    return NULL;
}
static void open_entry_ap_name(void)
{
    char cur[33];
    settings_get_ap_name(cur, sizeof(cur));
    text_entry_ctx_t ctx = {
        .title      = "AP name",
        .hint       = "1-32 chars",
        .secret     = false,
        .max_len    = 32,                  /* SSID max length */
        .on_confirm = te_confirm_ap_name,
        .return_to  = SCR_NET,   /* not to the scan list */
    };
    text_entry_open(&ctx, cur);            /* pre-fill with current value */
    nav_push(SCR_NET_PASS);
}

/* AP password: save the new SoftAP password */
static const char *te_confirm_ap_pass(const char *text)
{
    /* WPA2 needs 8..63 chars. Rejecting here (instead of silently falling back
     * to the compiled default) means the user sees why nothing changed. */
    size_t n = text ? strlen(text) : 0;
    if (n < 8)  return "min 8 chars";
    if (n > 63) return "max 63 chars";
    char cur[65];
    settings_get_ap_password(cur, sizeof(cur));
    if (strcmp(cur, text) == 0) return NULL;     /* unchanged: accept, do nothing */
    settings_set_ap_password(text);
    if (!mode_is_client() && s_ap_restart_handler) s_ap_restart_handler();
    return NULL;
}
static void open_entry_ap_pass(void)
{
    char cur[65];
    settings_get_ap_password(cur, sizeof(cur));
    text_entry_ctx_t ctx = {
        .title      = "AP password",
        .hint       = "8-63 chars",
        .secret     = true,
        .max_len    = 63,                  /* WPA2 passphrase max */
        .on_confirm = te_confirm_ap_pass,
        .return_to  = SCR_NET,   /* not to the scan list */
    };
    text_entry_open(&ctx, cur);            /* pre-fill with current value */
    nav_push(SCR_NET_PASS);
}

static void pw_connect(void)
{
    /* Run the context's confirm action. If it returns an error message the
     * entry is rejected: stay on the screen, show why, and play the reject
     * sound - never close silently as if it had worked. */
    const char *err = s_te.on_confirm ? s_te.on_confirm(s_pw_buf) : NULL;
    if (err) {
        te_error_show(err);
        audio_play(AUDIO_REJECT);
        pw_render();
        return;
    }
    te_error_clear();
    audio_play(AUDIO_NAV_BEEP);    /* nav beep: entry confirmed */
    if (s_te.return_to != SCR_NONE) nav_pop_to(s_te.return_to);
    else                            nav_pop();
}

/* ============================================================================
 * RENDERING
 * ==========================================================================*/
/* Defined further down with the rest of the validation: only the prototype is
 * needed here, because ipk_refresh() uses it to colour the confirm key. */
static bool ipk_complete(void);

/* Apply the field's parameters: it rewrites the bottom key table and
 * repositions the row's objects. To be called BEFORE entering. */
static void ipk_set_field(const ipk_field_t *f)
{
    s_ipk_f = *f;
    s_ipk_slot_w = f->digits * IPK_DIGIT_W;
    int tot = f->groups * s_ipk_slot_w + (f->groups - 1) * IPK_DOT_W;
    s_ipk_x0 = (320 - tot) / 2;

    /* Bottom row. With a separator: three keys. Without: two, twice as wide,
     * because a gap in the middle would waste space on a screen that has little
     * of it. Setting the middle key's width to 0 excludes it from the hit
     * search without needing a special case there. */
    if (f->sep) {
        s_ipk_geo[10].x =   8; s_ipk_geo[10].w =  96;
        s_ipk_geo[11].x = 110; s_ipk_geo[11].w =  96;
        s_ipk_geo[12].x = 212; s_ipk_geo[12].w = 100;
    } else {
        s_ipk_geo[10].x =   8; s_ipk_geo[10].w = 148;
        s_ipk_geo[11].x =   0; s_ipk_geo[11].w =   0;
        s_ipk_geo[12].x = 164; s_ipk_geo[12].w = 148;
    }

    for (int i = 0; i < IPK_MAX_GROUPS; i++) {
        int x = s_ipk_x0 + i * (s_ipk_slot_w + IPK_DOT_W);
        lv_obj_set_pos(s_ipk_oct[i], x, 58);
        lv_obj_set_width(s_ipk_oct[i], s_ipk_slot_w);
        if (i < IPK_MAX_GROUPS - 1)
            lv_obj_set_pos(s_ipk_dot[i], x + s_ipk_slot_w, 58);
    }
    lv_obj_set_size(s_ipk_box, s_ipk_slot_w + 2 * IPK_BOX_PAD, 32);
    for (int i = 0; i < 13; i++) {
        lv_obj_set_pos(s_ipk_key[i], s_ipk_geo[i].x, s_ipk_geo[i].y);
        lv_obj_set_size(s_ipk_key[i], s_ipk_geo[i].w, s_ipk_geo[i].h);
    }
    lv_label_set_text(s_ipk_key[11], f->sep ? "." : "");
}

/* Update text, colours and box. It creates nothing. */
static void ipk_refresh(void)
{
    for (int i = 0; i < IPK_MAX_GROUPS; i++) {
        bool used = (i < s_ipk_f.groups);
        if (used) {
            lv_obj_clear_flag(s_ipk_oct[i], LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(s_ipk_oct[i], s_ipk_val[i]);
            lv_obj_set_style_text_color(s_ipk_oct[i],
                                        (i == s_ipk_act) ? C_GREEN : C_LABEL, 0);
        } else {
            lv_obj_add_flag(s_ipk_oct[i], LV_OBJ_FLAG_HIDDEN);
        }
        if (i < IPK_MAX_GROUPS - 1) {
            if (i < s_ipk_f.groups - 1) lv_obj_clear_flag(s_ipk_dot[i], LV_OBJ_FLAG_HIDDEN);
            else                        lv_obj_add_flag(s_ipk_dot[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (s_ipk_f.sep) lv_obj_clear_flag(s_ipk_key[11], LV_OBJ_FLAG_HIDDEN);
    else             lv_obj_add_flag(s_ipk_key[11], LV_OBJ_FLAG_HIDDEN);

    /* The confirm key is GREEN only when there is really something to confirm.
     * Grey does not mean "disabled" - the key still answers, with the rejection
     * sound - but it says at a glance that something is missing, before the user
     * finds out by tapping. It is what the keypad knows and the user does not.
     * which digits would be enough to complete the field. */
    lv_obj_set_style_text_color(s_ipk_key[12],
                                ipk_complete() ? C_GREEN : C_MUTED2, 0);

    lv_obj_set_x(s_ipk_box,
                 s_ipk_x0 + s_ipk_act * (s_ipk_slot_w + IPK_DOT_W) - IPK_BOX_PAD);

    /* NO HINT TEXT, in either field.
     *
     * The accepted range is unnecessary: a digit outside the limits does not go
     * in, and the confirm key stays grey until the value is valid. The
     * interface already GUIDES, and text repeating in words what the colours
     * show is only visual noise. */
}

static void ipk_reset(void)
{
    for (int i = 0; i < IPK_MAX_GROUPS; i++) s_ipk_val[i][0] = 0;
    s_ipk_act = 0;
}

/* Preload the current value, so opening the field shows what is there instead
 * of an empty one — and whoever wants to change a single digit need not retype
 * everything.
 *
 * If the text cannot be parsed it starts empty: better than showing something
 * half-formed. */
static void ipk_preset(const char *text)
{
    for (int i = 0; i < IPK_MAX_GROUPS; i++) s_ipk_val[i][0] = 0;
    s_ipk_act = 0;
    if (!text || !*text) return;

    int g = 0; size_t n = 0;
    for (const char *p = text; *p; p++) {
        if (s_ipk_f.sep && *p == s_ipk_f.sep) {
            if (++g >= s_ipk_f.groups) return;      /* one separator too many */
            n = 0; continue;
        }
        if (*p < '0' || *p > '9') { ipk_reset(); return; }
        if (n >= s_ipk_f.digits)  { ipk_reset(); return; }
        s_ipk_val[g][n++] = *p;
        s_ipk_val[g][n]   = 0;
    }
    /* It opens on the LAST group: that is where one comes back most often. */
    s_ipk_act = g;
}

/* Builds the value into a string, with separators where they belong. */
static void ipk_text(char *out, size_t cap)
{
    out[0] = 0;
    for (int i = 0; i < s_ipk_f.groups; i++) {
        if (i && s_ipk_f.sep) {
            size_t l = strlen(out);
            if (l + 2 < cap) { out[l] = s_ipk_f.sep; out[l+1] = 0; }
        }
        strncat(out, s_ipk_val[i], cap - strlen(out) - 1);
    }
}

/* Show or hide every object of the screen in one go. */
static void ipk_show(bool on)
{
    lv_obj_t *o[] = { s_ipk_box,
                      s_ipk_oct[0], s_ipk_oct[1], s_ipk_oct[2], s_ipk_oct[3],
                      s_ipk_dot[0], s_ipk_dot[1], s_ipk_dot[2] };
    for (unsigned i = 0; i < sizeof(o)/sizeof(o[0]); i++) {
        if (!o[i]) continue;
        if (on) lv_obj_clear_flag(o[i], LV_OBJ_FLAG_HIDDEN);
        else    lv_obj_add_flag(o[i], LV_OBJ_FLAG_HIDDEN);
    }
    for (int i = 0; i < 13; i++) {
        if (!s_ipk_key[i]) continue;
        if (on) lv_obj_clear_flag(s_ipk_key[i], LV_OBJ_FLAG_HIDDEN);
        else    lv_obj_add_flag(s_ipk_key[i], LV_OBJ_FLAG_HIDDEN);
    }
}

/* One digit. Returns false if the key is to be refused - and the refusal is
 * the point: an invalid value is not LET in and then reported. */
static bool ipk_digit(char d)
{
    char *v = s_ipk_val[s_ipk_act];
    size_t n = strlen(v);
    if (n >= s_ipk_f.digits) return false;
    /* A leading zero followed by anything else makes no sense in a number. */
    if (n == 1 && v[0] == '0') return false;

    char cand[IPK_MAX_DIGITS + 1];
    memcpy(cand, v, n); cand[n] = d; cand[n+1] = 0;
    if ((uint32_t)strtoul(cand, NULL, 10) > s_ipk_f.vmax) return false;

    strcpy(v, cand);
    /* Group full: advance on our own, so the separator key is only needed when
     * a group is shorter than the maximum. */
    if (strlen(v) == s_ipk_f.digits && s_ipk_act < s_ipk_f.groups - 1) s_ipk_act++;
    return true;
}

static bool ipk_dot(void)
{
    if (!s_ipk_f.sep) return false;               /* field with no separator */
    if (s_ipk_act >= s_ipk_f.groups - 1) return false;   /* the last has nothing after */
    if (s_ipk_val[s_ipk_act][0] == 0) return false;  /* empty octet */
    s_ipk_act++;
    return true;
}

static bool ipk_back(void)
{
    char *v = s_ipk_val[s_ipk_act];
    size_t n = strlen(v);
    if (n > 0) { v[n-1] = 0; return true; }
    /* Octet already empty: go back to the previous one and delete its last
     * digit, which is what one expects from a backspace. */
    if (s_ipk_act == 0) return false;
    s_ipk_act--;
    v = s_ipk_val[s_ipk_act];
    n = strlen(v);
    if (n > 0) v[n-1] = 0;
    return true;
}

/* True if the value is complete AND acceptable.
 *
 * The MAXIMUM is already guaranteed by construction — a digit that would exceed
 * it is refused at the moment of the touch. Two things remain that cannot be
 * prevented during entry: a group left empty, and a MINIMUM not reached (a port
 * cannot be 0, an octet can). */
static bool ipk_complete(void)
{
    for (int i = 0; i < s_ipk_f.groups; i++) {
        if (s_ipk_val[i][0] == 0) return false;
        if ((uint32_t)strtoul(s_ipk_val[i], NULL, 10) < s_ipk_f.vmin) return false;
    }
    return true;
}

/* --- OSC: host and port go through the KEYPAD ------------------------------
 *
 * With the keypad, validation on confirm is unnecessary because getting it
 * wrong becomes impossible: the limits live in the field, and a digit outside
 * the range does not go in.
 *
 * The host stays an ADDRESS, not a name: accepting names would mean DNS
 * resolution, a cache, and one more way to fail in the field. */
static void ipk_commit_osc_host(const char *v) { settings_set_osc_host(v); }
static void ipk_commit_osc_port(const char *v)
{
    settings_set_osc_port((uint16_t)strtoul(v, NULL, 10));
}

static const ipk_field_t IPK_F_OSC_HOST =
    { 4, 3, 0, 255,   '.', "Host", ipk_commit_osc_host };
static const ipk_field_t IPK_F_OSC_PORT =
    { 1, 5, 1, 65535,  0,  "Port", ipk_commit_osc_port };

static void open_entry_osc_host(void)
{
    char cur[40];
    settings_get_osc_host(cur, sizeof(cur));
    ipk_set_field(&IPK_F_OSC_HOST);
    ipk_preset(cur);
    nav_push(SCR_IPENTRY);
}

static void open_entry_osc_port(void)
{
    char cur[8];
    snprintf(cur, sizeof(cur), "%u", (unsigned)settings_get_osc_port());
    ipk_set_field(&IPK_F_OSC_PORT);
    ipk_preset(cur);
    nav_push(SCR_IPENTRY);
}

static void render(void)
{
    screen_id_t id = cur();
    if (id == SCR_NONE || id >= SCR_COUNT) return;

    /* --- A SINGLE INVALIDATION ON SCREEN CHANGE ----------------------------
     *
     * THE PROBLEM. LVGL merges two invalid areas only when one CONTAINS the
     * other; if they overlap partially it keeps them separate. Changing screen
     * hides about 22 objects and shows as many in different positions: no area
     * contains the others, so they all stay distinct and the same bands of the
     * screen are drawn and sent SEVERAL TIMES.
     *
     * [M] Measured entering the keypad: 17 flushes with a maximum area of 7675
     * px, that is a full buffer (320x24 = 7680). Seventeen bands where ten are
     * enough for a whole screen: almost two complete redraws.
     *
     * For comparison, entering About — which moves MORE pixels — behaves
     * better: there a full-screen object CONTAINS all the other areas, so they
     * merge into one.
     *
     * THE FIX. Invalidate everything first: the invalidations that follow fall
     * inside the big one and are absorbed, and the redraw happens once.
     *
     * [M] Verified on the device: from 17 flushes to 10, with a maximum area of
     * 7680 px, the full band. Ten is the THEORETICAL MINIMUM — 240 rows divided
     * by a 24-row buffer — so not a single pixel is left drawn twice. Total
     * redraw time from 78 to 65 ms.
     *
     * The maximum hold stays at about 6.5 ms: that is the cost of ONE full
     * band, and this change does not touch it. The gain is entirely in the
     * NUMBER of transfers, not in their cost.
     *
     * FOR THE KEYPAD ONLY, and the restriction is deliberate.
     *
     * [M] The other transitions do NOT have the problem, and that was verified
     * on the device: scrolling between menu rows redraws with no perceptible
     * slowness. The reason is that there five ALIGNED rows change, overlapping
     * in a column, which LVGL handles well; the keypad instead replaces
     * horizontal rows with twenty-one objects scattered over the whole surface.
     *
     * Extending the invalidation to every transition would be a change that
     * fixes nothing where nothing is broken, and can only make things worse: a
     * full redraw where a few pieces suffice today. If some other screen ever
     * shows the same symptom, its id is added here — after measuring it, not by
     * analogy. */
    {
        static screen_id_t p_id = SCR_NONE;
        if (id != p_id) {
            if (id == SCR_IPENTRY || p_id == SCR_IPENTRY)
                lv_obj_invalidate(s_scr);
            p_id = id;
        }
    }

    /* the transient error owns the title bar while it is visible */
    if (!s_te_err) {
        /* the entry screen uses a short title: the full breadcrumb would run
         * into the length hint on the right */
        if (id == SCR_NET_PASS)
            lv_label_set_text(s_title, s_te.title ? s_te.title : "Entry");
        else
            lv_label_set_text(s_title, breadcrumb());
    }

    /* password screen: custom layout, not the row list */
    if (id == SCR_NET_PASS) {
        for (int i = 0; i < ROWS_VISIBLE; i++) {
            lv_obj_add_flag(s_row_bg[i],  LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_row_lbl[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_row_val[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_row_sub[i], LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_add_flag(s_sb_track, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_sb_thumb, LV_OBJ_FLAG_HIDDEN);
        pw_screen_show(true);
        pw_render();
        return;
    }
    /* IPv4: like About, everything else is hidden — except the title here,
     * because it says which address is being entered. */
    if (id == SCR_IPENTRY) {
        for (int i = 0; i < ROWS_VISIBLE; i++) {
            lv_obj_add_flag(s_row_bg[i],  LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_row_lbl[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_row_val[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_row_sub[i], LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_add_flag(s_sb_track, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_sb_thumb, LV_OBJ_FLAG_HIDDEN);
        pw_screen_show(false);
        ipk_show(true);
        ipk_refresh();
        return;
    }
    ipk_show(false);

    /* About: the intro draws over the whole screen, so EVERYTHING else is
     * hidden — rows and scrollbar alike — or it would end up underneath. */
    if (id == SCR_ABOUT) {
        for (int i = 0; i < ROWS_VISIBLE; i++) {
            lv_obj_add_flag(s_row_bg[i],  LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_row_lbl[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_row_val[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_row_sub[i], LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_add_flag(s_sb_track, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_sb_thumb, LV_OBJ_FLAG_HIDDEN);
        pw_screen_show(false);
        /* NO BACKGROUND IMAGE. The intro opens on the screen's background
         * colour, the same as the dashboard and the menus, and the lines read
         * without competing with a photograph.
         *
         * Hiding all the menu rows leaves the LVGL screen background exposed,
         * which is already C_BG: nothing needs drawing.
         *
         * If the intro fails to start — task not created, or already running —
         * the screen stays empty. A poorer fallback, but a coherent one: About
         * without its intro is not something one looks at. */
        if (!about_running()) {
            s_fx_done = false;
            /* The portrait as the TEXTURE the intro uncovers. Passed in from
             * here rather than included by the About component: the image lives here
             * and that component must not depend on this one. */
            about_set_texture((const uint16_t *)author_selfie.data,
                               (int)author_selfie.header.w,
                               (int)author_selfie.header.h);
            about_start(on_fx_done);
        }
        return;
    }
    /* About was left by another route: the intro must be stopped, or it would
     * keep drawing over the new screen.
     *
     * about_stop is NOT immediate — the intro closes on the next frame — so
     * the invalidation here is a partial safety net: if that last frame arrived
     * afterwards, it would leave dirt behind. The clean case is the one above,
     * where the intro has already finished. */
    if (about_running()) { about_stop(); lv_obj_invalidate(s_scr); }

    /* any other screen: make sure the password screen is hidden */
    pw_screen_show(false);
    /* BOTH PARTS COME BACK. The three screens that draw over everything —
     * About, the keypad, the password entry — hide the track AND the thumb;
     * bringing back only the track leaves the bar drawn but empty, with no
     * mark of where one is in the list, for the rest of the session. */
    lv_obj_clear_flag(s_sb_track, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_sb_thumb, LV_OBJ_FLAG_HIDDEN);

    int count = screen_count(id);
    bool scrollable = count > ROWS_VISIBLE;

    /* scrollbar */
    {
        int denom = scrollable ? count : ROWS_VISIBLE;
        int thumb_h = SB_H * ROWS_VISIBLE / denom;
        if (thumb_h < SB_MIN_THUMB) thumb_h = SB_MIN_THUMB;
        if (thumb_h > SB_H)         thumb_h = SB_H;
        int thumb_y = SB_TOP;
        if (scrollable) {
            int max_scroll = count - ROWS_VISIBLE;
            int travel = SB_H - thumb_h;
            thumb_y = SB_TOP + (travel * s_scroll) / max_scroll;
        }
        lv_obj_set_height(s_sb_thumb, thumb_h);
        lv_obj_set_y(s_sb_thumb, thumb_y);
    }

    for (int i = 0; i < ROWS_VISIBLE; i++) {
        int idx = s_scroll + i;
        if (idx < count) {
            row_t r;
            screen_row(id, idx, &r);
            lv_obj_clear_flag(s_row_bg[i], LV_OBJ_FLAG_HIDDEN);
            /* label: centred if no substatus, raised if there is one */
            int y = LIST_TOP + i * ROW_H;
            if (r.sub) {
                lv_obj_set_y(s_row_lbl[i], y + 11);
                lv_label_set_text(s_row_sub[i], r.sub);
                lv_obj_set_style_text_color(s_row_sub[i], r.sub_col, 0);
                lv_obj_clear_flag(s_row_sub[i], LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_set_y(s_row_lbl[i], y + 20);
                lv_obj_add_flag(s_row_sub[i], LV_OBJ_FLAG_HIDDEN);
            }
            lv_label_set_text(s_row_lbl[i], r.label);
            lv_obj_set_style_text_color(s_row_lbl[i],
                                        r.lbl_col_set ? r.lbl_col : C_LABEL, 0);
            lv_obj_clear_flag(s_row_lbl[i], LV_OBJ_FLAG_HIDDEN);
            /* value / triangle */
            /* The value label's y is its TOP-left corner (lv_obj_set_pos), so
             * a smaller font would raise the text instead of leaving it
             * centred. It is compensated with half the difference in line
             * height, taken from the font itself: that way it holds for any
             * size, today and tomorrow, with no hand-written numbers. */
            const lv_font_t *vf;
            if (r.enter && r.value[0] == 0) {
                /* Submenu triangle: a size smaller than the text, because it
                 * is a navigation sign and not a datum — at 16 it weighed as
                 * much as the value beside it. */
                vf = &lv_font_montserrat_14;
                lv_label_set_text(s_row_val[i], LV_SYMBOL_PLAY);
                lv_obj_set_style_text_color(s_row_val[i], C_MUTED, 0);
            } else {
                /* small font keeps long SSIDs from colliding with the value */
                vf = r.val_small ? &lv_font_montserrat_12 : &lv_font_montserrat_16;
                lv_label_set_text(s_row_val[i], r.value);
                lv_obj_set_style_text_color(s_row_val[i],
                                            r.val_col_set ? r.val_col : C_AMBER, 0);
            }
            lv_obj_set_style_text_font(s_row_val[i], vf, 0);
            lv_obj_set_y(s_row_val[i], y + 20 +
                         (lv_font_montserrat_16.line_height - vf->line_height) / 2);
            lv_obj_clear_flag(s_row_val[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_row_bg[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_row_lbl[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_row_val[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_row_sub[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

/* ============================================================================
 * NAVIGATION
 * ==========================================================================*/
static void nav_push(screen_id_t id)
{
    if (s_depth >= NAV_DEPTH) {
        /* SAYING SO RATHER THAN RETURNING IN SILENCE. A refused push shows as
         * a page that does not open, and there is nothing to connect that to a
         * full stack: it cost a round of diagnostics to find once. */
        ESP_LOGE(IPK_TAG, "[nav] stack full (%d levels): screen %d not opened",
                 NAV_DEPTH, (int)id);
        return;
    }

    /* AN ARMED CONFIRMATION DOES NOT TRAVEL. It is remembered by ROW INDEX
     * alone, so carrying it to another screen would leave the row at the same
     * position armed there — row 2 is "Format card" here and "Power off" under
     * System, and a first tap would run it outright. nav_pop already disarms;
     * this is the other way out. */
    confirm_clear();
    te_error_clear();                 /* never carry the error to another screen */
    if (s_depth > 0) s_stack_scroll[s_depth - 1] = s_scroll;  /* remember where we were */
    s_stack[s_depth++] = id;
    s_scroll = 0;
    audio_play(AUDIO_NAV_BEEP);   /* nav beep: entering a menu */
    if (id == SCR_NET_SCAN) {
        s_scan_n = 0;
        wifi_core_scan_start();
    }
    if (id == SCR_NET_PASS) {
        pw_enter_screen();
    }
    arm_refresh_if_needed();
    render();
}

static void nav_pop(void)
{
    /* Disarming HERE covers every exit: centre button, return from a
     * sub-screen, closing the settings. Doing it in the individual handlers
     * would have left uncovered the one route that gets forgotten. */
    confirm_clear();
    te_error_clear();                 /* never carry the error to another screen */

    /* LEAVING SD CARD MEANS THE FORMAT'S OUTCOME HAS BEEN READ. It stays on
     * screen until then rather than expiring on a timer: a format takes
     * minutes, and whoever starts one is not watching. */
    if (cur() == SCR_SDCARD) sdcard_format_ack();

    /* LEAVING THE CUSTOM SCREENS WRITES THE LIST. One commit for a whole
     * editing session instead of one per tap: see the note on s_oc_dirty.
     * Both screens flush, because either can be the last one left. */
    if (cur() == SCR_OSC_MSG || cur() == SCR_OSC_CUSTOM) osc_flush();
    if (s_depth > 0) s_depth--;
    audio_play(AUDIO_NAV_BEEP);   /* nav beep: going back */
    if (s_depth == 0) {
        s_active = false;
    } else {
        s_scroll = s_stack_scroll[s_depth - 1];   /* restore where we were */
        arm_refresh_if_needed();
        render();
    }
}

/* pop screens until the given id is on top (or the stack empties) */
/* Jump back to a given screen after completing an action (e.g. confirming a
 * password). Unlike nav_pop, this lands at the TOP of the list rather than
 * restoring the previous scroll: the first row is where the outcome shows
 * (the Status row), so it should be in view straight away. */
static void nav_pop_to(screen_id_t id)
{
    while (s_depth > 1 && s_stack[s_depth - 1] != id) s_depth--;
    s_scroll = 0;
    s_stack_scroll[s_depth - 1] = 0;   /* so a later back doesn't jump elsewhere */
    arm_refresh_if_needed();
    render();
}

/* debounce timer callback: fires the real transition once the mode has settled */
static void mode_debounce_cb(lv_timer_t *t)
{
    (void)t;
    if (s_mode_debounce) { lv_timer_del(s_mode_debounce); s_mode_debounce = NULL; }
    if (s_mode_handler) {
        s_mode_handler(mode_is_client());   /* run the hot-switch for the settled mode */
    }
}

/* toggle the WiFi mode preference (persisted). Updates the menu immediately for
 * feedback, but the actual radio transition is DEBOUNCED (~800ms) so a nervous
 * back-and-forth produces only one real transition. */
/* cycle WiFi TX power LOW -> MID -> HIGH -> LOW, persist and apply immediately */
static void cycle_wifi_power(void)
{
    wifi_power_t p = settings_get_wifi_power();
    p = (p == WIFI_POWER_HIGH) ? WIFI_POWER_LOW : (wifi_power_t)(p + 1);
    settings_set_wifi_power(p);
    wifi_core_set_power((int)p);             /* apply now (non-blocking) */
    audio_play(AUDIO_NAV_BEEP);              /* nav beep: option changed */
    render();                                /* immediate visual feedback */
}

static void toggle_mode(void)
{
    wifi_mode_pref_t m = settings_get_wifi_mode();
    settings_set_wifi_mode(m == WIFI_MODE_PREF_CLIENT
                           ? WIFI_MODE_PREF_AP : WIFI_MODE_PREF_CLIENT);
    s_scroll = 0;
    audio_play(AUDIO_NAV_BEEP);              /* nav beep: mode changed */
    render();                                /* immediate visual feedback */

    /* (re)arm the debounce: a further toggle before it fires restarts the wait */
    if (s_mode_debounce) lv_timer_del(s_mode_debounce);
    s_mode_debounce = lv_timer_create(mode_debounce_cb, 800, NULL);
    lv_timer_set_repeat_count(s_mode_debounce, 1);   /* one-shot */
}

static void activate_item(int visible_row)
{
    screen_id_t id = cur();
    if (id == SCR_NONE) return;
    int idx = s_scroll + visible_row;
    if (idx >= screen_count(id)) return;

    /* Touching a row OTHER than the armed one disarms it: the user has changed
     * their mind, and leaving it armed would be a trap. */
    if (s_confirm_row >= 0 && s_confirm_row != idx) confirm_clear();

    if (id == SCR_NET) {
        if (idx == 0) { nav_push(SCR_NET_STATUS); return; }  /* Status */
        if (idx == 1) { cycle_wifi_power();       return; }  /* Power: LOW->MID->HIGH */
        if (idx == 2) { toggle_mode();            return; }  /* Mode toggle */
        bool client = mode_is_client();
        if (client) {
            if (idx == 3) { nav_push(SCR_NET_SCAN); return; }   /* Choose network */
        } else {
            if (idx == 3) { open_entry_ap_name(); return; }     /* AP name */
            if (idx == 4) { open_entry_ap_pass(); return; }     /* AP password */
        }
        return;
    }

    if (id == SCR_NET_SCAN) {
        if (s_scan_n > 0 && idx < s_scan_n) {
            strncpy(s_sel_ssid, s_scan_aps[idx].ssid, sizeof(s_sel_ssid) - 1);
            s_sel_ssid[sizeof(s_sel_ssid) - 1] = 0;
            s_sel_secured = s_scan_aps[idx].secured;
            s_sel_auth    = s_scan_aps[idx].authmode;
            if (s_sel_secured) {
                open_entry_client_pass();      /* secured: type the password */
            } else {
                request_connect(s_sel_ssid, "");  /* open network: connect directly */
                nav_pop();                     /* back to Network; Status shows result */
            }
        }
        return;
    }

    if (id == SCR_NET_STATUS) return;   /* read-only */

    /* Developer screen: test actions triggered by hand */
    if (id == SCR_SYS) {
        /* The three destructive rows are 2, 3 and 4 — power off, restart,
         * factory reset — and only those go through the confirmation. The
         * others open a screen and return. */
        if (idx == 0) { confirm_clear(); nav_push(SCR_SYS_INFO); return; }
        if (idx == 1) { confirm_clear(); nav_push(SCR_ABOUT);    return; }
#if DJB_DEVELOPER
        if (idx == 5) { confirm_clear(); nav_push(SCR_SYS_DEV);  return; }
#endif
        if (idx < 2 || idx > 4) return;

        if (!confirm_armed(idx)) {
            /* First touch: it only arms. A REJECTION sound, not a confirming
             * one: what the user asked for has not happened, and the sound must
             * say so before they even read the text. */
            s_confirm_row = idx;
            s_confirm_us  = esp_timer_get_time();
            audio_play(AUDIO_REJECT);
            render();
            return;
        }

        confirm_clear();
        audio_play(AUDIO_NAV_BEEP);
        if (idx == 4) {
            /* The settings are erased and the device RESTARTS: leaving the
             * firmware running with NVS wiped underneath it would mean modules
             * still using values read earlier, and a half state is worse than
             * either extreme. */
            settings_erase_all();
            esp_restart();
        } else if (idx == 3) {
            esp_restart();
        } else {
            bsp_power_off();
            /* If it returns, the PMU did not obey: say so, rather than
             * standing silent. */
            audio_play(AUDIO_REJECT);
        }
        return;
    }
#if DJB_DEVELOPER
    if (id == SCR_SYS_DEV) {
        if (idx == 0) {
            const bool on = !settings_get_screenshots();
            settings_set_screenshots(on);
            /* The flush needs to know NOW: it is told rather than made to look,
             * and this is the one place the answer changes. */
            if (s_shots_handler) s_shots_handler(on);
            audio_play(AUDIO_NAV_BEEP);
            render();
        }
        return;
    }
#endif

    /* Bridge screen: Start Sync / Stop Sync.
     * The library's start/stop sync is the CHANNEL: it must be kept open if at
     * least one of the two filters is active, because without it nothing passes
     * in either direction. It is then the two settings that decide which
     * transitions cross the channel. */
    if (id == SCR_BRIDGE_OSC) {
        if (idx == 0) {                       /* Enabled: toggles */
            settings_set_osc_enabled(!settings_get_osc_enabled());
            audio_play(AUDIO_NAV_BEEP);
            render();
            return;
        }
        if (idx == 1) {                       /* Preset: cycles */
            uint8_t pr = (uint8_t)((settings_get_osc_preset() + 1) % OSC_PRESET_COUNT);
            settings_set_osc_preset(pr);
            audio_play(AUDIO_NAV_BEEP);
            render();
            return;
        }
        if (idx == 2) { open_entry_osc_host(); return; }
        if (idx == 3) { open_entry_osc_port(); return; }
        if (s_screens[id].items[idx].target == SCR_NONE) return;
    }
    if (id == SCR_MIDI_NET) {
        if (idx == 0) settings_set_midi_net_enabled(!settings_get_midi_net_enabled());
        else if (idx == 1) settings_set_midi_net_mdns(!settings_get_midi_net_mdns());
        else return;
        /* The network host reads its configuration at start-up, so applying a
         * change means restarting it. Here we only flag that: the actual
         * restart is done by main, from the loop that owns the host's
         * lifetime, because doing it here would freeze the UI for as long as
         * closing sockets and tasks takes. */
        if (s_midinet_handler) s_midinet_handler();
        audio_play(AUDIO_NAV_BEEP);
        render();
        return;
    }
    if (id == SCR_SINK_CV) {
        if (idx == 0) {
            settings_set_cv_enabled(!settings_get_cv_enabled());
        } else if (idx == 1) {
            settings_set_cv_ppqn((uint8_t)((settings_get_cv_ppqn() + 1) % 4));
        } else if (idx == 2) {
            settings_set_cv_width((uint8_t)((settings_get_cv_width() + 1) % 5));
        } else if (idx == 3) {
            settings_set_cv_polarity(!settings_get_cv_polarity());
        } else if (idx == 4 && s_snd_count > 0) {
            /* Cycles through the candidates. Changing the sound with the
             * output on is legitimate: main resends program change and
             * envelope. */
            uint8_t n = (uint8_t)((settings_get_cv_sound() + 1) % s_snd_count);
            settings_set_cv_sound(n);
        } else return;
        if (s_midinet_handler) s_midinet_handler();
        audio_play(AUDIO_NAV_BEEP);
        render();
        return;
    }
    if (id == SCR_MIDI_USB) {
        if (idx != 0) return;
        settings_set_midi_usb_enabled(!settings_get_midi_usb_enabled());
        if (s_midinet_handler) s_midinet_handler();
        audio_play(AUDIO_NAV_BEEP);
        render();
        return;
    }
    if (id == SCR_MIDI_DIN) {
        if (idx != 0) return;
        /* Switchable with the module absent too: one sets what is WANTED, and
         * the transport will make it happen as soon as the hardware is there. */
        settings_set_midi_din_enabled(!settings_get_midi_din_enabled());
        if (s_midinet_handler) s_midinet_handler();   /* same notice to main */
        audio_play(AUDIO_NAV_BEEP);
        render();
        return;
    }
    if (id == SCR_OSC_CUSTOM) {
        if (!s_oc || !s_oc_n) return;
        if (idx < *s_oc_n) { s_oc_edit = idx; nav_push(SCR_OSC_MSG); return; }

        /* The Add row. A NEW MESSAGE IS NOT EMPTY: it starts on every beat with
         * a plausible path, so that confirming nothing still gives something
         * that works. An empty one would send nothing and look broken. */
        if (*s_oc_n >= OSC_CUSTOM_MAX) { audio_play(AUDIO_REJECT); return; }
        osc_custom_msg_t *m = &s_oc[*s_oc_n];
        memset(m, 0, sizeof(*m));
        m->trigger = OSC_TRIG_BEAT;

        snprintf(m->path,  sizeof(m->path),  "%s", "/beat");
        snprintf(m->value, sizeof(m->value), "%s", "{bpm}");
        s_oc_edit = (*s_oc_n)++;
        osc_touched();
        nav_push(SCR_OSC_MSG);
        return;
    }

    if (id == SCR_OSC_MSG) {
        if (!s_oc || !s_oc_n || s_oc_edit >= *s_oc_n) return;
        osc_custom_msg_t *m = &s_oc[s_oc_edit];

        if (idx == 0) {                                   /* When  */
            m->trigger = (uint8_t)((m->trigger + 1) % OSC_TRIG_COUNT);
        } else if (idx == 3) {                            /* Delete */
            /* THE LAST ONE MOVES INTO THE GAP. Keeping the order would mean
             * shifting every message after it, and the order carries no
             * meaning here — they all fire on their own trigger. */
            if (s_oc_edit < *s_oc_n - 1) s_oc[s_oc_edit] = s_oc[*s_oc_n - 1];
            (*s_oc_n)--;
            osc_touched();
            audio_play(AUDIO_NAV_BEEP);
            nav_pop();
            return;
        } else {
            open_entry_osc(idx == 1);     /* 1 Path, 2 Value */
            return;
        }
        osc_touched();
        audio_play(AUDIO_NAV_BEEP);
        render();
        return;
    }


    if (id == SCR_SDCARD) {
        if (idx == 0) {
            settings_set_sd_playlist(!settings_get_sd_playlist());
            audio_play(AUDIO_NAV_BEEP);
            render();
            return;
        }
        if (idx == 1) {
            /* A FEW SENSIBLE VALUES, cycled, rather than a number to dial in.
             * The choice is coarse by nature — thirty seconds or two minutes
             * is a different idea of what counts as played — and a keypad for
             * it would be more machinery than the decision deserves. */
            static const uint8_t steps[] = { 15, 30, 60, 90, 120, 180 };
            const uint8_t now_s = settings_get_sd_threshold_s();
            size_t k = 0;
            while (k < sizeof(steps) - 1 && steps[k] < now_s) k++;
            settings_set_sd_threshold_s(steps[(k + 1) % sizeof(steps)]);
            audio_play(AUDIO_NAV_BEEP);
            render();
            return;
        }
        if (idx == 2) {
            /* BARRED, NOT REFUSED SILENTLY. With no card there is nothing to
             * erase; with a bridge running the format would hold the SPI bus
             * for seconds, and the beats go out over that bus. Either way the
             * row is grey and says why — a rejection sound tells the user the
             * tap was seen and the answer is no. */
            if (sdcard_state() == SDCARD_ABSENT || s_bridge_active ||
                sdcard_format_state() == SDCARD_FMT_BUSY ||
                sdcard_format_state() == SDCARD_FMT_FAILED) {
                audio_play(AUDIO_REJECT);
                return;
            }

            if (!confirm_armed(idx)) {
                /* First tap arms it. The REJECTION sound, not the confirming
                 * one: nothing has happened yet, and the sound should say so
                 * before the text is read. */
                s_confirm_row = idx;
                s_confirm_us  = esp_timer_get_time();
                audio_play(AUDIO_REJECT);
                render();
                return;
            }

            confirm_clear();
            audio_play(AUDIO_NAV_BEEP);
            /* Requested, not run: it takes seconds, and doing it here would
             * freeze the screen for their whole length — including the line
             * that says what is happening. */
            sdcard_request_format();
            render();
            return;
        }
    }

    if (id == SCR_LAN) {
        if (idx == 1) {                       /* DJ Link ID: AUTO <-> OBSERVER */
            settings_set_djlink_observer(!settings_get_djlink_observer());
            audio_play(AUDIO_NAV_BEEP);
            render();
            return;
        }
        /* Status has a target and must reach the nav_push at the end. */
    }

    if (id == SCR_SYS_BRIDGE) {
        if (idx == 0 || idx == 1) {
            if (idx == 0) settings_set_start_sync(!settings_get_start_sync());
            else          settings_set_stop_sync (!settings_get_stop_sync());
            linkbridge_set_start_stop_sync(settings_get_start_sync() ||
                                           settings_get_stop_sync());
            audio_play(AUDIO_NAV_BEEP);
            render();
            return;
        }
        if (idx == 3) {                       /* Remember mode ON<->OFF */
            settings_set_remember_bridge(!settings_get_remember_bridge());
            /* main takes the snapshot of the current state on the rising edge:
             * doing it here would need ui_clock_src(), which lives in ui.h —
             * and ui already depends on settings_ui, so that would be a
             * cycle. */
            audio_play(AUDIO_NAV_BEEP);
            render();
            return;
        }
        if (idx == 2) {                       /* Drift Compensation */
            bool on = !settings_get_drift_comp();
            settings_set_drift_comp(on);
            if (s_drift_handler) s_drift_handler(on);
            audio_play(AUDIO_NAV_BEEP);
            render();
            return;
        }
        /* Entries with no target are placeholders and do nothing; those WITH a
         * target must reach the nav_push at the end of the function, though - an
         * unconditional return here swallowed them all. */
        if (s_screens[id].items[idx].target == SCR_NONE) return;
    }

    /* Device screen: audio toggle + brightness cycling */
    if (id == SCR_DEVICE) {
        if (idx == 0) {                       /* Audio ON/OFF */
            bool on = !audio_get_enabled();
            audio_set_enabled(on);
            settings_set_audio_enabled(on);
            audio_play(AUDIO_NAV_BEEP);       /* only heard if turning ON */
            render();
            return;
        }
        if (idx == 1) {                       /* LED brightness OFF->LOW->MED->HIGH */
            led_brightness_t b = settings_get_led_brightness();
            b = (b == LED_BRIGHT_HIGH) ? LED_BRIGHT_OFF : (led_brightness_t)(b + 1);
            settings_set_led_brightness(b);
            /* OFF/LOW/MED/HIGH -> 0/20/40/80 % of the base LED level */
            static const uint8_t pct[4] = { 0, 20, 40, 80 };
            leds_set_brightness(pct[b]);              /* apply now */
            audio_play(AUDIO_NAV_BEEP);
            render();
            return;
        }
        if (idx == 2) {                       /* Screen brightness LOW->MED->HIGH */
            brightness_t b = settings_get_display_brightness();
            b = (b == BRIGHT_HIGH) ? BRIGHT_LOW : (brightness_t)(b + 1);
            settings_set_display_brightness(b);
            bsp_set_backlight_level((int)b);          /* apply now */
            audio_play(AUDIO_NAV_BEEP);
            render();
            return;
        }
        if (idx == 3) {                       /* Screen flip ON<->OFF */
            bool on = !display_flipped();
            /* The two rotations go together and in THIS order: touch first,
             * then the image. Rotating the image first would have a touch
             * arriving in between read with the old orientation. */
            bsp_touch_set_flipped(on);
            display_set_flipped(on);
            settings_set_screen_flipped(on);
            audio_play(AUDIO_NAV_BEEP);
            /* The panel changed its scan direction: what is in memory no longer
             * matches. */
            lv_obj_invalidate(s_scr);
            render();
            return;
        }
    }

    /* static screen */
    const menu_item_t *it = &s_screens[id].items[idx];
    if (it->target != SCR_NONE) nav_push(it->target);
}

/* ============================================================================
 * PUBLIC API
 * ==========================================================================*/
void settings_ui_init(void)
{
    s_scr = lv_obj_create(NULL);
    lv_obj_remove_style_all(s_scr);
    lv_obj_clear_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_scr, SCR_W, SCR_H);
    lv_obj_set_style_bg_color(s_scr, C_BG, 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);

    mk_rect(s_scr, 0, 0, SCR_W, HDR_H, C_PANEL);
    s_title = mk_label(s_scr, 12, 12, "Settings", &lv_font_montserrat_12, C_CYAN);
    mk_rect(s_scr, 0, HDR_H, SCR_W, 1, C_DIV);

    for (int i = 0; i < ROWS_VISIBLE; i++) {
        int y = LIST_TOP + i * ROW_H;
        s_row_bg[i]  = mk_rect(s_scr, ROW_X, y, ROW_W, ROW_H - ROW_GAP, C_ROW);
        s_row_lbl[i] = mk_label(s_scr, ROW_X + 14, y + 20, "", &lv_font_montserrat_16, C_LABEL);
        s_row_val[i] = mk_label(s_scr, ROW_X + ROW_W - 164, y + 20, "", &lv_font_montserrat_16, C_AMBER);
        lv_obj_set_width(s_row_val[i], 150);
        lv_obj_set_style_text_align(s_row_val[i], LV_TEXT_ALIGN_RIGHT, 0);
        lv_label_set_long_mode(s_row_val[i], LV_LABEL_LONG_CLIP);   /* no wrapping */
        s_row_sub[i] = mk_label(s_scr, ROW_X + 14, y + 34, "", &lv_font_montserrat_12, C_GREEN);
        lv_obj_add_flag(s_row_bg[i],  LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_row_lbl[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_row_val[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_row_sub[i], LV_OBJ_FLAG_HIDDEN);
    }

    /* --- IPv4 entry: the keypad, hidden until it is entered --- */
    /* The box BEFORE the digits, so it stays underneath: created afterwards, it
     * would cover them. */
    s_ipk_box = mk_rect(s_scr, 0, 52, 10, 32, C_ROW);
    lv_obj_set_style_border_color(s_ipk_box, C_GREEN, 0);
    lv_obj_set_style_border_width(s_ipk_box, 1, 0);
    lv_obj_set_style_radius(s_ipk_box, 4, 0);

    /* Provisional positions and widths: ipk_set_field() fixes them according to
     * the field, since they depend on how many digits and groups it has. */
    for (int i = 0; i < IPK_MAX_GROUPS; i++) {
        s_ipk_oct[i] = mk_label(s_scr, 0, 58, "", &lv_font_montserrat_20, C_LABEL);
        lv_obj_set_style_text_align(s_ipk_oct[i], LV_TEXT_ALIGN_CENTER, 0);
        if (i < IPK_MAX_GROUPS - 1) {
            s_ipk_dot[i] = mk_label(s_scr, 0, 58, ".",
                                    &lv_font_montserrat_20, C_MUTED2);
            lv_obj_set_width(s_ipk_dot[i], IPK_DOT_W);
            lv_obj_set_style_text_align(s_ipk_dot[i], LV_TEXT_ALIGN_CENTER, 0);
        }
    }

    for (int i = 0; i < 13; i++) {
        const ipk_key_t *k = &s_ipk_geo[i];
        /* ONE object per key: a label with a background, not a rectangle plus text. */
        lv_color_t c = (i == 10) ? C_RED : (i == 12) ? C_GREEN : C_LABEL;
        s_ipk_key[i] = mk_label(s_scr, k->x, k->y, k->lbl,
                                &lv_font_montserrat_16, c);
        lv_obj_set_size(s_ipk_key[i], k->w, k->h);
        lv_obj_set_style_bg_color(s_ipk_key[i], C_ROW, 0);
        lv_obj_set_style_bg_opa(s_ipk_key[i], LV_OPA_COVER, 0);
        lv_obj_set_style_radius(s_ipk_key[i], 4, 0);
        lv_obj_set_style_text_align(s_ipk_key[i], LV_TEXT_ALIGN_CENTER, 0);
        /* vertical padding: centres the text within the key height */
        lv_obj_set_style_pad_top(s_ipk_key[i], (k->h - 20) / 2, 0);
    }
    ipk_show(false);

    s_sb_track = mk_rect(s_scr, SB_X, SB_TOP, SB_W, SB_H, C_SBTRACK);
    s_sb_thumb = mk_rect(s_scr, SB_X, SB_TOP, SB_W, SB_MIN_THUMB, C_SBTHUMB);

    /* ---- password screen (custom layout, hidden until entered) ---- */
    /* target ssid, top-right of the header area */
    s_pw_ssid = mk_label(s_scr, 180, 12, "", &lv_font_montserrat_12, C_MUTED);
    lv_obj_set_width(s_pw_ssid, 128);
    lv_obj_set_style_text_align(s_pw_ssid, LV_TEXT_ALIGN_RIGHT, 0);

    /* password field: a framed box just under the header */
    lv_obj_t *pwfld_bg = mk_rect(s_scr, 12, HDR_H + 8, 296, 34, C_ROW);
    s_pw_field = mk_label(s_scr, 22, HDR_H + 17, "", &lv_font_montserrat_16, C_WHITE);
    s_pw_fieldbg = pwfld_bg;

    /* character selector: highlight box + 5 chars centred at y~120 */
    s_pw_sel_box = mk_rect(s_scr, 140, 96, 40, 40, C_ROW);
    lv_obj_set_style_border_color(s_pw_sel_box, C_GREEN, 0);
    lv_obj_set_style_border_width(s_pw_sel_box, 1, 0);
    lv_obj_set_style_bg_opa(s_pw_sel_box, LV_OPA_TRANSP, 0);
    /* 5 selector chars: centre at x=160, spaced 34px, all SAME size and on the
     * same baseline (horizontal row). Hierarchy is conveyed by colour: the
     * centre bright, the neighbours much darker the further out they are.
     * Dedicated greys (not the shared C_MUTED) for a strong contrast step. */
    int sel_x[5] = { 160 - 68, 160 - 34, 160, 160 + 34, 160 + 68 };
    lv_color_t c_far  = lv_color_hex(0x2A2F37);   /* ±2: very dark, just above bg */
    lv_color_t c_near = lv_color_hex(0x515861);   /* ±1: dark grey */
    lv_color_t sel_c[5] = { c_far, c_near, C_GREEN, c_near, c_far };
    for (int i = 0; i < 5; i++) {
        s_pw_sel[i] = mk_label(s_scr, sel_x[i] - 10, 106, "", &lv_font_montserrat_20,
                               sel_c[i]);
        lv_obj_set_width(s_pw_sel[i], 20);
        lv_obj_set_style_text_align(s_pw_sel[i], LV_TEXT_ALIGN_CENTER, 0);
    }

    /* MODE KEYS. Five objects are built, and the geometry is decided later by
     * pw_modes_layout(): a field without placeholders shows four wide ones, as
     * it always has. Building them here with a fixed width would mean two
     * places that know the layout. */
    for (int i = 0; i < PW_MODES; i++) {
        s_pw_mode_bg[i]  = mk_rect(s_scr, 12, 150, 68, 30, C_ROW);
        s_pw_mode_lbl[i] = mk_label(s_scr, 12, 157, s_charset_name[i],
                                    &lv_font_montserrat_16, C_MUTED);
        lv_obj_set_style_text_align(s_pw_mode_lbl[i], LV_TEXT_ALIGN_CENTER, 0);
    }

    /* action keys: 3 across at y~188 */
    const char *act[3] = { LV_SYMBOL_BACKSPACE, "space", LV_SYMBOL_OK };
    lv_color_t  actc[3] = { C_RED, C_MUTED, C_GREEN };
    for (int i = 0; i < 3; i++) {
        int x = 12 + i * 100;
        s_pw_act_bg[i]  = mk_rect(s_scr, x, 188, 94, 34, C_ROW);
        s_pw_act_lbl[i] = mk_label(s_scr, x, 197, act[i], &lv_font_montserrat_16, actc[i]);
        lv_obj_set_width(s_pw_act_lbl[i], 94);
        lv_obj_set_style_text_align(s_pw_act_lbl[i], LV_TEXT_ALIGN_CENTER, 0);
    }

    /* hide all password objects initially */
    pw_screen_show(false);
}

void settings_ui_enter(void)
{
    s_depth = 0;
    nav_push(SCR_MAIN);
    s_active = true;
    s_touch_down = false;
    lv_screen_load(s_scr);
    render();
}

bool settings_ui_active(void)
{
    return s_active;
}

void settings_ui_set_mode_handler(settings_ui_mode_handler_t fn)
{
    s_mode_handler = fn;
}

void settings_ui_set_connect_handler(settings_ui_connect_handler_t fn)
{
    s_connect_handler = fn;
}

void settings_ui_set_ap_restart_handler(settings_ui_ap_restart_handler_t fn)
{
    s_ap_restart_handler = fn;
}

void settings_ui_set_midinet_handler(settings_ui_midinet_handler_t fn)
{
    s_midinet_handler = fn;
}

void settings_ui_set_drift_handler(settings_ui_drift_handler_t fn)
{
    s_drift_handler = fn;
}


/* --- AUTO-REPEAT ON THE STRIP ARROWS ---------------------------------------
 *
 * The rest of this screen acts on RELEASE: where the finger went down is
 * remembered, and the reaction comes when it lifts. That is the right choice
 * for touches that CHOOSE — a row, a key — because it lets one correct the aim
 * by sliding before lifting.
 *
 * But for the ARROWS it is wrong: scrolling an alphabet of dozens of
 * characters, or a long list, one touch at a time is tiring. Here the same
 * repetition the dashboard has is needed.
 *
 * THE SAME CADENCE as the dashboard's: first step immediate, then 400 ms of
 * pause and from there one step every 90 ms. Identical on purpose — two
 * different cadences for the same gesture on the same hardware would be felt as
 * a defect.
 *
 * THE RELEASE MUST BE SUPPRESSED. If the repetition has already acted during
 * the press, the release logic further down would act ONCE MORE on the same
 * arrow: a short touch would count as two steps. That is why s_rep_zone is
 * consumed at the start of the release branch. */
/* --- LONG PRESS ON BACKSPACE: clear the field ------------------------------
 *
 * It works on ALL the entry screens — password, address, port — because the
 * gesture is the same and learning it once should be enough.
 *
 * WHY IT IS NEEDED: emptying a full field otherwise takes as many touches as
 * there are characters. On a long password that is dozens; on an address
 * already entered, twelve.
 *
 * It is NOT the arrows' repetition: there equal steps accumulate, here there is
 * ONE step only, different from the short one. That is why the threshold is
 * higher (600 ms against 400) — it must be clear that the gesture was intended,
 * and not happen through a slightly long press.
 *
 * THE RELEASE MUST BE SUPPRESSED, as for the arrows: without that, after the
 * clearing the release would also delete the last character of an
 * already-empty field — harmless, but with the rejection sound immediately
 * after the confirming one. */
#define BKSP_HOLD_US   600000

static int64_t s_bksp_us   = 0;      /* start of the press, 0 = not pressed     */
static bool    s_bksp_done = false;  /* the field has already been cleared      */

#define REP_DELAY_US   400000
#define REP_EVERY_US    90000

static int     s_rep_zone  = 0;    /* -1 left, +1 right, 0 none */
static int64_t s_rep_start = 0;
static int64_t s_rep_last  = 0;

/* Perform the arrow's action on the current screen. Returns false if the arrows
 * do nothing here — and then there is nothing to repeat. */
/* True if the touch falls on the current screen's backspace. The geometry comes
 * from the same sources used to draw it: s_ipk_geo for the keypad, the action
 * row constants for the password screen. */
static bool bksp_hit(uint16_t x, uint16_t y)
{
    screen_id_t id = cur();
    if (id == SCR_IPENTRY) {
        const ipk_key_t *k = &s_ipk_geo[10];
        return k->w && x >= k->x && x < k->x + k->w &&
                       y >= k->y && y < k->y + k->h;
    }
    if (id == SCR_NET_PASS)
        return y >= 188 && y < 224 && x >= 12 && x < 112;
    return false;
}

/* Clear the field being entered. Returns false if it was already empty: there
 * is nothing to clear, and the gesture must not sound as though it succeeded. */
static bool bksp_clear_all(void)
{
    screen_id_t id = cur();
    if (id == SCR_IPENTRY) {
        bool empty = true;
        for (int i = 0; i < s_ipk_f.groups; i++)
            if (s_ipk_val[i][0]) { empty = false; break; }
        if (empty) return false;
        ipk_reset();
        return true;
    }
    if (id == SCR_NET_PASS) {
        if (s_pw_len == 0) return false;
        s_pw_buf[0] = 0;
        s_pw_len    = 0;
        te_error_clear();
        return true;
    }
    return false;
}

static bool arrow_act(int dir)
{
    screen_id_t id = cur();

    /* On About the arrows do nothing. Saying so HERE, rather than letting the
     * generic path fall through, means the repetition never arms itself on a
     * screen that has only one behaviour: any touch closes it — a touch in the
     * arrow zone would leave it open. */
    if (id == SCR_ABOUT)   return false;
    if (id == SCR_IPENTRY) return false;   /* the keys are on screen */

    if (id == SCR_NET_PASS) {
        if (dir < 0) pw_char_prev(); else pw_char_next();
        return true;
    }

    int count = (id != SCR_NONE) ? screen_count(id) : 0;
    if (count <= ROWS_VISIBLE) return false;          /* not scrollable */
    if (dir < 0) {
        if (s_scroll == 0) return false;              /* already at the top */
        s_scroll--;
    } else {
        if (s_scroll + ROWS_VISIBLE >= count) return false;  /* already at the bottom */
        s_scroll++;
    }
    audio_play(AUDIO_NAV_BEEP);
    render();
    return true;
}

/* Forget the touch in progress: no release will come for it.
 *
 * A press is remembered until the finger lifts, and the release is what acts on
 * it. When somebody else consumes that release — a long press taken as a
 * screenshot — the memory would otherwise sit here waiting, and the FIRST
 * release to arrive afterwards, whatever it was meant for, would be read as the
 * end of the old press. */
/* Whether the About intro is playing.
 *
 * ASKED OF THIS COMPONENT because it is the one that starts and stops it, and
 * because ui already depends on this: the other direction would close a
 * cycle. */
bool settings_ui_about_running(void)
{
    return s_active && cur() == SCR_ABOUT && about_running();
}

void settings_ui_touch_cancel(void)
{
    s_touch_down = false;
    s_rep_zone   = 0;
    s_bksp_us    = 0;
}


void settings_ui_touch(uint16_t x, uint16_t y, bool pressed)
{
    if (!s_active) return;

    if (pressed) {
        int64_t now = esp_timer_get_time();

        /* Backspace held down: clear the field. It must be evaluated DURING
         * the press — the release would come too late to tell a short touch
         * from a long one. */
        if (bksp_hit(x, y)) {
            if (s_bksp_us == 0) { s_bksp_us = now; s_bksp_done = false; }
            else if (!s_bksp_done && now - s_bksp_us > BKSP_HOLD_US) {
                s_bksp_done = true;      /* either way: do not retry every tick */
                /* ALWAYS the confirming beep, even if the field was already
                 * empty. A rejection would say the gesture failed, and it did
                 * not: it succeeded, there was simply nothing to remove. The
                 * sound confirms the GESTURE, not its effect — as with every
                 * other key in this interface. */
                audio_play(AUDIO_NAV_BEEP);
                if (bksp_clear_all()) render();
            }
            s_touch_down = true; s_touch_x = x; s_touch_y = y;
            return;
        }
        s_bksp_us = 0;

        /* Arrow zone on the physical strip: here and only here it repeats. */
        int zone = 0;
        if (y >= BAR_Y) {
            if (x < BAR_LEFT_X)       zone = -1;
            else if (x > BAR_RIGHT_X) zone = +1;
        }
        if (zone != 0) {
            if (zone != s_rep_zone) {
                /* first step: immediate, so a short touch stays responsive */
                if (arrow_act(zone)) {
                    s_rep_zone = zone; s_rep_start = now; s_rep_last = now;
                } else {
                    /* the action has no effect (end of list): no repetition,
                     * but the zone is recorded so it is not retried every tick */
                    s_rep_zone = zone; s_rep_start = now;
                    s_rep_last  = now + 3600LL * 1000000;   /* mai */
                }
            } else if (now - s_rep_start > REP_DELAY_US &&
                       now - s_rep_last  > REP_EVERY_US) {
                if (arrow_act(zone)) s_rep_last = now;
                else                 s_rep_last = now + 3600LL * 1000000;
            }
        } else {
            s_rep_zone = 0;
        }
        s_touch_down = true;
        s_touch_x = x;
        s_touch_y = y;
        return;
    }
    if (!s_touch_down) return;
    s_touch_down = false;
    x = s_touch_x;
    y = s_touch_y;

    /* IPv4: the hit search uses THE SAME table as the drawing. Two separate
     * tables — one to draw, one to hit — would diverge at the first change of
     * geometry, and the symptom would be a key seen in one place and answering
     * in another. */
    if (cur() == SCR_IPENTRY) {
        s_rep_zone = 0;
        if (y >= BAR_Y) { nav_pop(); return; }     /* the strip: leaves */

        /* Touch on the ADDRESS ROW: select that group.
         *
         * It is the shortcut needed by anyone correcting a single digit in the
         * middle: without it, getting back to the second octet from the fourth
         * takes two backspaces that DELETE, or one starts again.
         *
         * The sensitive area is the slot PLUS the separator that follows, so no
         * dead bands are left between one group and the next — a touch halfway
         * lands on the group to its left rather than in the void. */
        if (y >= 52 && y < 90 && s_ipk_f.groups > 1) {
            int step = s_ipk_slot_w + IPK_DOT_W;
            for (int g = 0; g < s_ipk_f.groups; g++) {
                int x0 = s_ipk_x0 + g * step;
                int x1 = x0 + ((g == s_ipk_f.groups - 1) ? s_ipk_slot_w : step);
                if (x < x0 || x >= x1) continue;
                if (g == s_ipk_act) { audio_play(AUDIO_REJECT); return; }
                s_ipk_act = g;
                audio_play(AUDIO_NAV_BEEP);
                render();          /* render() already calls ipk_refresh() */
                return;
            }
            return;
        }
        for (int i = 0; i < 13; i++) {
            const ipk_key_t *k = &s_ipk_geo[i];
            if (k->w == 0) continue;         /* key absent in this field */
            if (x < k->x || x >= k->x + k->w) continue;
            if (y < k->y || y >= k->y + k->h) continue;
            bool ok;
            if (i <= 9) {
                /* keys 0..9 are laid out 1,2,3,4,5,6,7,8,9,0 */
                ok = ipk_digit((i == 9) ? '0' : (char)('1' + i));
            } else if (i == 10) {
                ok = ipk_back();
            } else if (i == 11) {
                ok = ipk_dot();
            } else {
                ok = ipk_complete();
                if (ok) {
                    char v[IPK_MAX_GROUPS * (IPK_MAX_DIGITS + 1) + 1];
                    ipk_text(v, sizeof(v));
                    ESP_LOGI(IPK_TAG, "%s: %s", s_ipk_f.title, v);
                    if (s_ipk_f.commit) s_ipk_f.commit(v);
                    audio_play(AUDIO_NAV_BEEP);
                    nav_pop();
                    return;
                }
            }
            /* BACKSPACE ALWAYS SOUNDS THE BEEP, even on an empty field: the
             * sound confirms the GESTURE, not its effect. On the OTHER keys the
             * rejection stays and makes sense — a digit out of range, a dot on
             * an empty group, a confirm on an incomplete field are actions that
             * DO NOT go through. */
            bool bksp = (i == 10);
            audio_play((ok || bksp) ? AUDIO_NAV_BEEP : AUDIO_REJECT);
            if (ok) render();      /* render() already calls ipk_refresh() */
            return;
        }
        return;    /* touch outside the keys: no effect */
    }

    /* Backspace already cleared during the press: the release must not delete a
     * character as well. */
    if (s_bksp_done) { s_bksp_done = false; s_bksp_us = 0; return; }
    s_bksp_us = 0;

    /* About: any touch closes it, and this is evaluated BEFORE the release
     * suppression — otherwise a touch landing in the arrow zone would close
     * nothing. There is nothing to select, and hunting for the strip in the
     * dark under a full-screen image would be a memory test. */
    if (cur() == SCR_ABOUT) { s_rep_zone = 0; nav_pop(); return; }

    /* The arrow already acted during the press: the release must not act a
     * second time. */
    if (s_rep_zone != 0) { s_rep_zone = 0; return; }

    /* ================= PASSWORD SCREEN (custom interaction) ================= */
    if (cur() == SCR_NET_PASS) {
        /* physical bar: arrows scroll the alphabet, centre = back */
        if (y >= BAR_Y) {
            /* The ARROWS are not here: arrow_act() serves them during the
             * press, with repetition. Only the centre remains here. */
            if (x >= BAR_LEFT_X && x <= BAR_RIGHT_X) nav_pop();
            return;
        }
        /* character selector band (y ~96..136): tap centre adds the char */
        if (y >= 96 && y < 140) {
            pw_add_char();
            return;
        }
        /* MODE KEYS (y ~150..180). The pitch follows the layout: four wide
         * ones normally, five narrow where the field offers placeholders. The
         * two numbers live here and in pw_modes_layout, and must agree. */
        if (y >= 150 && y < 182) {
            const int n    = s_te.placeholders ? PW_MODES : 4;
            const int step = s_te.placeholders ? 59 : 74;
            int k = (x - 12) / step;
            if (k >= 0 && k < n) pw_set_charset(k);
            return;
        }
        /* action keys (y ~188..222): canc / space / connect, each 100px */
        if (y >= 188 && y < 224) {
            int k = (x - 12) / 100;
            if (k == 0)      pw_backspace();
            else if (k == 1) pw_space();
            else if (k == 2) pw_connect();
            return;
        }
        return;   /* ignore taps elsewhere on the password screen */
    }

    /* --- PHYSICAL BAR (y >= 248): left/centre/right --- */
    if (y >= BAR_Y) {
        /* Scrolling with the arrows is done by arrow_act() during the press,
         * with auto-repeat. Only the centre is left here. */
        if (x >= BAR_LEFT_X && x <= BAR_RIGHT_X) nav_pop();
        return;
    }

    /* --- DISPLAY AREA (rows) --- */
    if (y >= LIST_TOP && y < LIST_BOTTOM) {
        int row = (y - LIST_TOP) / ROW_H;
        if (row >= 0 && row < ROWS_VISIBLE) {
            activate_item(row);
        }
    }
}
