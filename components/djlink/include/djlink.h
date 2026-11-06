/*
 * djlink.h — Pioneer Pro DJ Link: packet decoding and construction.
 *
 * Clean-room implementation from direct observation of real hardware (2x
 * XDJ-700), documented in docs/protocolli_DJLink_AbletonLink_specifiche.md.
 * Offsets are 0-based, big-endian where noted. No dynamic allocation: parsing
 * happens in place on caller-provided buffers.
 *
 * The module covers BOTH directions. Reading — beat, status, master election —
 * is the easy half. Transmitting, and being ACCEPTED as tempo master by real
 * players, is considerably stricter: it forces exact packet layouts, a device
 * number claimed through a staged negotiation, and answers to queries that
 * would otherwise time out. Most of the non-obvious constants below exist
 * because of that second half.
 *
 * Evidence markers used throughout this project:
 *   [M] measured on real hardware   [D] deduced from measurements
 *   [U] unverified assumption       [E] from an external source
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- ports, packet types, lengths ---------------------------------------- */

/* Pro DJ Link UDP ports (broadcast). */
#define DJLINK_PORT_KEEPALIVE 50000
#define DJLINK_PORT_BEAT      50001
#define DJLINK_PORT_STATUS    50002

/* Packet types, at offset 0x0a. */
#define DJLINK_TYPE_KEEPALIVE 0x06
#define DJLINK_TYPE_STATUS    0x0a
#define DJLINK_TYPE_BEAT      0x28

/* Channel defence: a device that hears its OWN number being claimed sends this
 * to port 50000 of the newcomer, which MUST then back off. 41 bytes (0x29):
 * type at 0x0a, name at 0x0c..0x1f, length at 0x22-0x23, the DEFENDED number
 * at 0x24, the defender's IP at 0x25..0x28. */
#define DJLINK_TYPE_CONFLICT  0x08

/* Packet lengths. */
#define DJLINK_KEEPALIVE_LEN  54
#define DJLINK_HELLO_LEN      38
#define DJLINK_CLAIM1_LEN     44
#define DJLINK_CLAIM2_LEN     50
#define DJLINK_CLAIM3_LEN     38
#define DJLINK_STATUS_LEN     0x11c   /* 284 bytes */
#define DJLINK_BEATPKT_LEN    0x60    /*  96 bytes: beat packet      (0x28) */
#define DJLINK_TAKEOVER_LEN   0x28    /*  40 bytes: master takeover  (0x26) */
#define DJLINK_TKRESP_LEN     0x30    /*  48 bytes: takeover reply   (0x27) */
#define DJLINK_FADER_LEN      0x28    /*  40 bytes: fader start      (0x02) */
#define DJLINK_MEDIARSP_LEN   0xc0    /* 192 bytes: media query reply (0x06) */

/* [M] Observed lengths are exact on XDJ-700, with no exceptions. Other models
 * may use longer status packets — dysentery documents variants for the
 * CDJ-2000NXS2 — so always check length before reading, never assume. */

/* Device number of our Virtual CDJ when acting as an OBSERVER.
 *
 * 7 is the first of a 7..15 range. Do NOT go below it: CDJ-3000s use channels
 * 5 and 6 as legitimate player numbers, and taking one would mean contending
 * with a real device. beat-link uses the same range, for the same reason.
 *
 * Not to be confused with a PLAYER number (1..4), which is mandatory when
 * fetching metadata from the dbserver — see the extended note in main.c. */
#define DJLINK_VCDJ_DEVICE    0x07

/* Ten-byte signature shared by every DJ Link packet. */
extern const uint8_t djlink_magic[10];
bool djlink_has_magic(const uint8_t *buf, size_t len);

/* Pitch "unity", meaning 1.0x. */
#define DJLINK_PITCH_UNITY 0x100000u

/* --- Fader Start (type 0x02, port 50001) -----------------------------------
 *
 * Starts and stops players. MIND THE ACTUAL SEMANTICS: this is not play/pause,
 * it is the behaviour of a DJM channel fader.
 *   PLAY  starts ONLY if the player sits at its cue point. A deck paused
 *         mid-track will NOT resume: the command is ignored.
 *   STOP  stops the player and RETURNS it to the cue point. Not a pause but a
 *         jump backwards — potentially destructive during a live set.
 *   NONE  leaves the player alone.
 * [E] Not all hardware supports it: XDJ-XZ and CDJ-3000 ignore it. */
#define DJLINK_FADER_PLAY  0x00
#define DJLINK_FADER_STOP  0x01
#define DJLINK_FADER_NONE  0x02

/* --- declaring a loaded medium --------------------------------------------
 *
 * We claim to have one. A real player with a track loaded declares slot,
 * rekordbox database and track id, and we do the same: it is consistent with
 * Mv=0x8000 ("rekordbox track loaded"), which [M] is REQUIRED for players to
 * accept our tempo.
 *
 * CONSEQUENCE, found with Wireshark: players then want to READ that track's
 * audio, and ask over ONC-RPC (portmapper, UDP :111) where our MOUNT/NFS
 * service lives. [M] They ask even when we declare no medium at all, so we may
 * as well be consistent and declare one.
 *
 * WHAT MATTERS IS ANSWERING. While we stayed silent the player timed out and
 * displayed "E-8309 Link Access Error"; answering "I do not have that service"
 * (GETPORT -> port 0) makes the error disappear. On this network silence is
 * not neutral — an explicit no closes the question. See portmap_tick() in
 * main.c. */
#define DJB_DECLARE_MEDIA  1

/* Media slots, same values as in status packets. On a player the three buttons
 * map to:
 *   REKORDBOX  a networked PC running rekordbox      (Sr=04)
 *   LINK       the PHYSICAL media of OTHER players, seen over the network
 *   USB        the player's own USB slot             (Sr=03)
 *
 * We pretend to have a USB stick: the SMALLEST possible fiction, one medium
 * holding one track, and its contents appear to other players under LINK.
 * Declaring ourselves a rekordbox PC would invite questions about an entire
 * collection — playlists, genres, artists, search — which is a far larger
 * commitment. */
#define DJLINK_SLOT_NONE  0x00
#define DJLINK_SLOT_CD    0x01    /* CD drive                     */
#define DJLINK_SLOT_SD    0x02    /* SD slot                      */
#define DJLINK_SLOT_USB   0x03    /* USB slot: our fiction        */
#define DJLINK_SLOT_PC    0x04    /* rekordbox collection on a laptop */

/* --- parsed packets ------------------------------------------------------- */

/* Beat packet (port 50001, type 0x28). */
typedef struct {
    uint8_t  device;      /* device number 1..4                 (0x21)      */
    uint32_t pitch;       /* u32 BE, unity = 0x100000           (0x54..0x57) */
    uint16_t track_bpm;   /* u16 BE, BPM x 100                  (0x5a..0x5b) */
    uint8_t  beat_in_bar; /* 1..4                               (0x5c)      */
    float    bpm;         /* effective BPM = track_bpm/100 * pitch/unity    */
} djlink_beat_t;

/* CDJ status packet (port 50002, type 0x0a).
 *
 * NOTE ON THE TWO PITCH FIELDS. The status carries four copies of the pitch;
 * we read two of them, and [M] they are not interchangeable:
 *   pitch     (P1, 0x8c)  SUSTAINED pitch — the fader position, or the value
 *                         imposed by sync. Does not follow jog-wheel bend.
 *   pitch_eff (P2, 0x98)  INSTANTANEOUS pitch, jog bend included.
 *
 * A bridge that must follow bend reads P2. This is a deliberate divergence
 * from beat-link, which uses P1 for getEffectiveTempo() — consistent with its
 * own purpose, but it does not capture the bend. */
typedef struct {
    uint8_t  device;         /* device number 1..4                 (0x21) */
    bool     is_master;      /* flag F bit 5 (0x20)                (0x89) */
    bool     is_playing;     /* flag F bit 6 (0x40)                (0x89) */
    uint8_t  master_handoff; /* Mh: device the master is yielding to, 0xff = none */
    uint32_t pitch;          /* P1: sustained pitch                (0x8c) */
    uint32_t pitch_eff;      /* P2: effective pitch                (0x98) */
    uint16_t track_bpm;      /* u16 BE, BPM x 100          (0x92..0x93) */
    uint8_t  mm;             /* [M] 2 = track not analysed by rekordbox: the
                              * beat grid is unreliable and beat_in_bar must
                              * be ignored                        (0x9e) */
    uint8_t  beat_in_bar;    /* 1..4                              (0xa6) */
    float    bpm;            /* effective BPM                            */

    /* Identity of the loaded track, used to query the dbserver. */
    uint8_t  track_device;   /* Dr: which device holds the medium (0x28) */
    uint8_t  track_slot;     /* Sr: 1=CD 2=SD 3=USB 4=rekordbox   (0x29) */
    uint8_t  track_type;     /* Tr: 1=rekordbox                   (0x2a) */
    uint32_t track_id;       /* rekordbox id               (0x2c..0x2f) */
} djlink_status_t;

/* --- decoding ------------------------------------------------------------- */

/* Check the "Qspt1WmJOL" magic and return the packet type (byte 0x0a), or -1
 * if the magic is wrong or the buffer too short. */
int djlink_packet_type(const uint8_t *buf, size_t len);

/* Parse a beat packet.
 *
 * Also returns false for FORMALLY VALID packets carrying no usable
 * information: track_bpm = 0xFFFF ("no tempo"), or beat_in_bar outside 1..4
 * ("not available"). Those are the protocol's sentinels, and they are checked
 * here rather than in callers because a beat without tempo or position is of
 * no use to anyone, and one guard is less fragile than one per caller. */
bool djlink_parse_beat(const uint8_t *buf, size_t len, djlink_beat_t *out);

/* Parse a status packet. */
bool djlink_parse_status(const uint8_t *buf, size_t len, djlink_status_t *out);

/* If buf is a valid takeover REQUEST (0x26), return the device number of the
 * device that wants to become master; otherwise 0. */
uint8_t djlink_parse_takeover_req(const uint8_t *buf, size_t len);

/* Media query (0x05): a player asks about the medium mounted in one slot.
 * Failing to answer leads to its "Link Access Error" timeout. */
bool djlink_parse_media_query(const uint8_t *buf, size_t len, uint8_t *from_dev, uint8_t *slot);

/* Effective BPM from track_bpm (x100) and pitch. */
float djlink_effective_bpm(uint16_t track_bpm, uint32_t pitch);

/* --- construction --------------------------------------------------------- */

/* Virtual CDJ keep-alive, 54 bytes. [M] Layout validated byte by byte against
 * an XDJ-700 — in particular the bytes at 0x30 and 0x34, which an early
 * implementation got wrong, and the player then refused to admit the device.
 * mac and ip must belong to the interface the packet actually leaves from.
 *
 * 'peers' is the number of devices seen on the network, OURSELVES INCLUDED. It
 * is not a constant: the protocol uses it as a counter at byte 0x30. The
 * minimum accepted is 1, and it is forced internally. */
size_t djlink_build_keepalive(uint8_t *buf, uint8_t device,
                              const uint8_t mac[6], const uint8_t ip[4],
                              const char *name, uint8_t peers);

/* Device-number claim sequence (port 50000, broadcast):
 * hello (0x0a) -> claim1 (0x00) -> claim2 (0x02) -> claim3 (0x04), each sent
 * three times at 300 ms. 'ctr' is the 1..3 counter. */
size_t djlink_build_hello (uint8_t *buf, const char *name);
size_t djlink_build_claim1(uint8_t *buf, const uint8_t mac[6], const char *name, uint8_t ctr);
size_t djlink_build_claim2(uint8_t *buf, const uint8_t ip[4], const uint8_t mac[6],
                           const char *name, uint8_t number, uint8_t ctr, uint8_t autoassign);
size_t djlink_build_claim3(uint8_t *buf, const char *name, uint8_t number, uint8_t ctr);

/* CDJ status packet for TRANSMISSION on port 50002.
 *
 * bpm in real BPM; beat is the absolute counter within the track; bb is
 * beat-in-bar 1..4; my is the device we are yielding master to, 0xff if none.
 *
 * All four pitch fields are populated with unity, and the whole tempo goes
 * into track_bpm. A player says "132 BPM track at +5.6% pitch" where we say
 * "139.4 BPM track at 0%"; anything reading the two and multiplying gets the
 * same number. An early version filled only P2 and left the others at zero,
 * which [D] would give 0 BPM to anything reading P1, as beat-link does. */
size_t djlink_build_status(uint8_t *buf, const char *name, uint8_t device,
                           double bpm, uint32_t beat, uint8_t bb,
                           uint32_t sync_n, uint32_t packet_count,
                           bool playing, bool master, bool sync, bool on_air,
                           uint8_t my);

/* Beat packet, broadcast on port 50001 AT the instant of the beat.
 *
 * THE ARRIVAL INSTANT IS THE BEAT: the packet carries no timestamp, and [M]
 * players align to when it arrives, not to the intervals inside it. Any jitter
 * in emission transfers directly onto their phase.
 *
 * The nextBeat/nextBar fields are intervals towards future beats, computed
 * from the current period. */
size_t djlink_build_beat(uint8_t *buf, const char *name, uint8_t device,
                         double bpm, uint8_t bb);

/* Master takeover request (0x26), sent to port 50001 of the current master.
 * 'device' is OUR number, that is, whoever wants the role. */
size_t djlink_build_takeover(uint8_t *buf, const char *name, uint8_t device);

/* Takeover consent (0x27), sent by the OUTGOING master to port 50001 of the
 * requester. */
size_t djlink_build_takeover_resp(uint8_t *buf, const char *name, uint8_t device);

/* Fader start packet, for port 50001, broadcast: each player reads the slot
 * matching its own number. cmd[0..3] holds the command for players 1..4, one
 * of DJLINK_FADER_*. 'device' is OUR number. */
size_t djlink_build_fader_start(uint8_t *buf, const char *name, uint8_t device,
                                const uint8_t cmd[4]);

/* Reply to a media query (0x06), describing our virtual medium.
 * media_name = NULL means an EMPTY slot. */
size_t djlink_build_media_resp(uint8_t *buf, const char *name, uint8_t device,
                               uint8_t slot, const char *media_name, uint16_t tracks);

#ifdef __cplusplus
}
#endif
