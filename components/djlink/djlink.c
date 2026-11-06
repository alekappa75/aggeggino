/*
 * djlink.c — see djlink.h.
 *
 * [M] Every offset and every fixed byte in this file was validated against
 * real XDJ-700 hardware, most of it from Wireshark captures of two players
 * talking to each other. Where a value is not self-evident, the note says
 * what was measured rather than what was assumed.
 */
#include "djlink.h"
#include <string.h>

/* Pro DJ Link magic: "Qspt1WmJOL" */
const uint8_t djlink_magic[10] = {
    0x51, 0x73, 0x70, 0x74, 0x31, 0x57, 0x6d, 0x4a, 0x4f, 0x4c
};

bool djlink_has_magic(const uint8_t *buf, size_t len)
{
    return (len >= 10) && (memcmp(buf, djlink_magic, 10) == 0);
}

/* big-endian readers */
static inline uint16_t rd_u16be(const uint8_t *p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}
static inline uint32_t rd_u32be(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

float djlink_effective_bpm(uint16_t track_bpm, uint32_t pitch)
{
    /* track_bpm is BPM x 100; pitch/unity is the speed ratio. */
    return ((float)track_bpm / 100.0f) * ((float)pitch / (float)DJLINK_PITCH_UNITY);
}

int djlink_packet_type(const uint8_t *buf, size_t len)
{
    if (buf == NULL || len < 0x0b) return -1;
    if (memcmp(buf, djlink_magic, sizeof(djlink_magic)) != 0) return -1;
    return buf[0x0a];
}

bool djlink_parse_beat(const uint8_t *buf, size_t len, djlink_beat_t *out)
{
    if (out == NULL) return false;
    if (djlink_packet_type(buf, len) != DJLINK_TYPE_BEAT) return false;
    if (len < 0x5d) return false;  /* shortest beat packet worth reading */

    out->device      = buf[0x21];
    out->pitch       = rd_u32be(&buf[0x54]);
    out->track_bpm   = rd_u16be(&buf[0x5a]);
    out->beat_in_bar = buf[0x5c];

    /* --- SENTINELS: a beat packet may carry NO usable information ----------
     *
     * The protocol uses special values to mean "not available", and without
     * checking for them one produces nonsense instead of discarding:
     *
     *   track_bpm == 0xFFFF   "no tempo", meaning no track loaded.
     *                         djlink_effective_bpm would return 655.35 x pitch.
     *   beat_in_bar == 0      "not available", typical when the track was not
     *                         analysed by rekordbox. Taken as a real bar
     *                         position it would skew the phase anchor.
     *
     * The packet is DISCARDED rather than reported to the caller: a beat with
     * neither tempo nor bar position serves neither of the two uses we make of
     * it — tempo and phase anchor — and refusing it here protects EVERY caller
     * instead of demanding a guard in each one.
     *
     * [E] It should rarely happen: CDJs emit beat packets only while playing a
     * rekordbox track. MIXERS, however, emit them continuously as a fallback
     * metronome, and that is where these values appear. */
    if (out->track_bpm == 0xFFFF || out->track_bpm == 0) return false;
    if (out->beat_in_bar < 1 || out->beat_in_bar > 4)    return false;

    out->bpm         = djlink_effective_bpm(out->track_bpm, out->pitch);
    return true;
}

bool djlink_parse_status(const uint8_t *buf, size_t len, djlink_status_t *out)
{
    if (out == NULL) return false;
    if (djlink_packet_type(buf, len) != DJLINK_TYPE_STATUS) return false;
    /* LONG ENOUGH FOR EVERY FIELD WE READ, and the furthest is Pitch4 at 0xc4:
     * 0xc8 bytes. The old bound of 0x94 covered device, master flag and BPM,
     * and fields further in were added later without moving it — a packet
     * between the two would have been read past its end.
     *
     * IT EXCLUDES NOTHING REAL. [E] The shortest status any documented player
     * sends is 0xd0 (pre-nexus); Nexus send 0xd4, XDJ-1000 0x11b, XDJ-700 and
     * nxs2 0x11c, recent firmware 0x124, CDJ-3000 0x200. All of them carry
     * every field this parser touches. */
    if (len < 0xc8) return false;

    /* NO SUBTYPE CHECK IS NEEDED. [E] The byte at 0x20 differs between senders —
     * 00 for a mixer, 03 and up for players — but the TYPE at 0x0a already
     * separates them: a mixer sends type 0x29, not 0x0a. Filtering on the
     * subtype as well would add nothing and would reject a future player using
     * a value we have not seen. */

    out->device      = buf[0x21];
    out->is_master    = (buf[0x89] & 0x20) != 0;  /* flag F bit 5 = MASTER */
    out->is_playing   = (buf[0x89] & 0x40) != 0;  /* flag F bit 6 = PLAY   */
    out->master_handoff = (len > 0x9f) ? buf[0x9f] : 0xff;  /* Mh: yielding to */
    /* A status packet carries FOUR copies of the pitch. [M] Measured by moving
     * first the fader, then the jog wheel:
     *
     *   fader:  all four move together
     *   jog:    P1 (0x8c) and P3 (0xc0) stay FROZEN at the fader value,
     *           P2 (0x98) and P4 (0xc4) follow the nudge
     *
     * So P1 is the fader position and P2 is the EFFECTIVE pitch, jog bend
     * included. Tempo is computed from P2 because that is the speed the platter
     * is really turning at — the same one a synced player chases, as shown by
     * it staying glued to the master even through a nudge. */
    out->pitch       = rd_u32be(&buf[0x8c]);   /* fader position, for diagnostics */
    /* Too short to hold P2, as on older players: fall back to the fader. What
     * is lost is jog responsiveness, not correctness. */
    out->pitch_eff   = (len >= 0x9c) ? rd_u32be(&buf[0x98]) : out->pitch;
    out->track_bpm   = rd_u16be(&buf[0x92]);
    /* Mm, "master meaningful": 00 not master, 01 master with a rekordbox
     * track, 02 master but the track is NOT rekordbox. It is not the track
     * type in general — that is Tr at 0x2a. See the note on s_master_mm in
     * main.c. */
    out->mm          = (len > 0x9e) ? buf[0x9e] : 0;
    /* beat_in_bar: 0 means NOT AVAILABLE, not "bar position zero"; consumers
     * must treat it as such. Unlike the beat packet, nothing is discarded
     * here — the other fields stay valid and useful. */
    out->beat_in_bar = (len > 0xa6) ? buf[0xa6] : 0;
    out->bpm         = djlink_effective_bpm(out->track_bpm, out->pitch_eff);

    /* Identity of the loaded track, used to query the master's dbserver for
     * what is playing. */
    out->track_device = buf[0x28];                    /* Dr: which device  */
    out->track_slot   = buf[0x29];                    /* Sr: which slot    */
    out->track_type   = buf[0x2a];                    /* Tr: 1 = rekordbox */
    out->track_id     = rd_u32be(&buf[0x2c]);         /* rekordbox id      */
    return true;
}

size_t djlink_build_keepalive(uint8_t *buf, uint8_t device,
                              const uint8_t mac[6], const uint8_t ip[4],
                              const char *name, uint8_t peers)
{
    memset(buf, 0, DJLINK_KEEPALIVE_LEN);
    memcpy(buf, djlink_magic, 10);
    buf[0x0a] = 0x06;                 /* keep-alive type */
    buf[0x0b] = 0x00;
    if (name) {
        size_t n = strlen(name); if (n > 20) n = 20;
        memcpy(&buf[0x0c], name, n);  /* device name, 20 bytes, zero padded */
    }
    buf[0x20] = 0x01;
    buf[0x21] = 0x02;
    buf[0x22] = 0x00; buf[0x23] = 0x36;   /* total length, 54, big-endian */
    buf[0x24] = device;                    /* device number */
    buf[0x25] = 0x01;                      /* 01 = CDJ */
    memcpy(&buf[0x26], mac, 6);
    memcpy(&buf[0x2c], ip, 4);

    /* --- 0x30 IS A COUNTER, NOT A CONSTANT --------------------------------
     *
     * [E] Labelled `p` in the Deep Symmetry analysis: the NUMBER OF DEVICES
     * seen on the network, ourselves included. [M] It starts at 1, grows when
     * another device powers up, and falls again about ten seconds after one
     * goes away.
     *
     * It is worth stating plainly because a fixed value looks perfectly
     * plausible here — copy a capture made with two devices present and 2 is
     * "right", until the day there are three.
     *
     * A RELATED WARNING, since the mistaken version circulates. It is often
     * said that 0x30 and 0x34 are critical because an early implementation
     * using `0x30=01 0x31=01` was rejected by the players. The rejection was
     * real, the explanation was not: those two bytes shifted the counter and
     * the rest of the tail, producing a malformed packet. The POSITION was
     * critical, not the value.
     *
     * The minimum is 1 — we exist — hence the guard below. Being a uint8_t,
     * an upper bound would be dead code. */


    buf[0x30] = (peers == 0) ? 1 : peers;
    buf[0x34] = 0x01;                      /* 01 = CDJ, 02 = mixer */
    return DJLINK_KEEPALIVE_LEN;
}

/* Shared header: magic, type, and the 20-byte device name. */
static void lay_header(uint8_t *buf, size_t len, uint8_t type, const char *name)
{
    memset(buf, 0, len);
    memcpy(buf, djlink_magic, 10);
    buf[0x0a] = type;
    buf[0x0b] = 0x00;
    if (name) { size_t n = strlen(name); if (n > 20) n = 20; memcpy(&buf[0x0c], name, n); }
}

size_t djlink_build_hello(uint8_t *buf, const char *name)
{
    lay_header(buf, DJLINK_HELLO_LEN, 0x0a, name);
    buf[0x20] = 0x01; buf[0x21] = 0x04; buf[0x22] = 0x00; buf[0x23] = 0x26;
    buf[0x24] = 0x01; buf[0x25] = 0x40;
    return DJLINK_HELLO_LEN;
}

size_t djlink_build_claim1(uint8_t *buf, const uint8_t mac[6], const char *name, uint8_t ctr)
{
    lay_header(buf, DJLINK_CLAIM1_LEN, 0x00, name);
    buf[0x20] = 0x01; buf[0x21] = 0x03; buf[0x22] = 0x00; buf[0x23] = 0x2c;
    buf[0x24] = ctr;                    /* counter, 1..3 */
    buf[0x25] = 0x01;
    memcpy(&buf[0x26], mac, 6);
    return DJLINK_CLAIM1_LEN;
}

size_t djlink_build_claim2(uint8_t *buf, const uint8_t ip[4], const uint8_t mac[6],
                           const char *name, uint8_t number, uint8_t ctr, uint8_t autoassign)
{
    lay_header(buf, DJLINK_CLAIM2_LEN, 0x02, name);
    buf[0x20] = 0x01; buf[0x21] = 0x03; buf[0x22] = 0x00; buf[0x23] = 0x32;
    memcpy(&buf[0x24], ip, 4);          /* our IP  */
    memcpy(&buf[0x28], mac, 6);         /* our MAC */
    buf[0x2e] = number;                 /* number being claimed */
    buf[0x2f] = ctr;                    /* counter, 1..3 */
    buf[0x30] = 0x00;
    buf[0x31] = autoassign;             /* 1 = self-assign, 2 = fixed number */
    return DJLINK_CLAIM2_LEN;
}

size_t djlink_build_claim3(uint8_t *buf, const char *name, uint8_t number, uint8_t ctr)
{
    lay_header(buf, DJLINK_CLAIM3_LEN, 0x04, name);
    buf[0x20] = 0x01; buf[0x21] = 0x03; buf[0x22] = 0x00; buf[0x23] = 0x26;
    buf[0x24] = number;                 /* number being claimed */
    buf[0x25] = ctr;                    /* counter, 1..3 */
    return DJLINK_CLAIM3_LEN;
}

static void put_be(uint8_t *p, uint32_t v, int n)   /* big-endian, n bytes */
{
    for (int i = 0; i < n; i++) p[i] = (uint8_t)(v >> (8 * (n - 1 - i)));
}

size_t djlink_build_status(uint8_t *buf, const char *name, uint8_t device,
                           double bpm, uint32_t beat, uint8_t bb,
                           uint32_t sync_n, uint32_t packet_count,
                           bool playing, bool master, bool sync, bool on_air,
                           uint8_t my)
{
    /* [M] Validated against a Wireshark capture of TWO REAL XDJ-700s, 1281
     * status packets. Every fixed byte below is what they send. */
    memset(buf, 0, DJLINK_STATUS_LEN);
    memcpy(buf, djlink_magic, 10);
    buf[0x0a] = 0x0a;                       /* status type */
    if (name) { size_t n = strlen(name); if (n > 20) n = 20; memcpy(&buf[0x0b], name, n); }

    buf[0x1f]=0x01; buf[0x20]=0x04; buf[0x23]=0xf8;
    /* [M] 0x26 and 0x27 are ALWAYS zero on real players, 1281 out of 1281. */

#if DJB_DECLARE_MEDIA
    /* Declared track: we pretend to hold a USB stick carrying a rekordbox
     * database. [M] As a consequence players try to MOUNT that stick over NFS
     * to read its audio — see the portmapper note in djlink.h. */
    buf[0x28]=device;                       /* Dr: medium is on our device     */
    buf[0x29]=0x03;                         /* Sr: USB slot                    */
    buf[0x2a]=0x01;                         /* Tr: rekordbox track             */
    buf[0x2f]=0x01;                         /* rekordbox id = 1                */
    buf[0x33]=0x01;                         /* track number = 1                */
    buf[0x37]=0x05;                         /* [M] 5 when a track is loaded    */
    buf[0x3a]=0x02; buf[0x3b]=0xd7;         /* duration, 0x2d7 = 727 s         */
    buf[0x47]=0x0c;                         /* [M] 12 when a track is loaded   */
#endif

    buf[0x68]=0x01; buf[0x6a]=0x04; buf[0x6b]=0x04;
    buf[0x73]=0x04; buf[0x74]=0x01;         /* [M] real: 0x74=01, 0x75=00      */
    buf[0x7c]=0x31; buf[0x7d]=0x2e;
    buf[0x7e]=0x31; buf[0x7f]=0x33;         /* firmware string "1.13"          */
    buf[0x8d]=0x10;
    buf[0x90]=0x80;                         /* [M] Mv=0x8000: without it the
                                             * players ignore our tempo      */
    buf[0x94]=0x7f; buf[0x95]=0xff; buf[0x96]=0xff; buf[0x97]=0xff;
    /* THE FOUR PITCH FIELDS, all at unity (0x00100000).
     *
     * A real CDJ fills all four, and [M] they mean different things: P1 (0x8c)
     * and P3 (0xc0) carry the fader position, P2 (0x98) and P4 (0xc4) the
     * effective pitch including bend.
     *
     * All four must be populated. Filling only one leaves the rest at zero,
     * and anything reading P1 — which beat-link documents as "the raw
     * effective device pitch", so the likeliest choice — would compute
     * track_bpm * 0 = 0 BPM.
     *
     * We declare unity rather than a pitch because we have no fader: the tempo
     * we impose lives entirely in track_bpm (0x92). Unity in all four says
     * exactly that, whichever field the player chooses to read. */
    buf[0x8d]=0x10;                         /* P1 = unity */
    buf[0x99]=0x10;                         /* P2 = unity */
    buf[0xc1]=0x10;                         /* P3 = unity */
    buf[0xc5]=0x10;                         /* P4 = unity */
    buf[0xa4]=0x01; buf[0xa5]=0xff;         /* cue countdown 0x1ff = no cue    */
    buf[0xb6]=0x01;
    buf[0xc1]=0x10; buf[0xc5]=0x10;
    buf[0xcc]=0x0f; buf[0xcd]=0x01;
    buf[0xd0]=0x12; buf[0xd1]=0x34; buf[0xd2]=0x56; buf[0xd3]=0x78;
    buf[0xd7]=0x01; buf[0xd8]=0x01; buf[0xd9]=0x01; buf[0xda]=0x01; buf[0xdb]=0x01;
    buf[0x113]=0x05;                        /* [M] always 0x05 on real players */
    /* [M] 0x116, 0x117, 0x11a and 0x11b are always zero on real players. */

    /* --- variable fields --- */
    buf[0x21]=device; buf[0x24]=device;
    buf[0x7b]= playing ? 0x03 : 0x00;       /* [M] real players: 03 / 00       */
    put_be(&buf[0x84], sync_n, 4);
    buf[0x89]= 0x84 | (playing?0x40:0) | (master?0x20:0)
                    | (sync?0x10:0)   | (on_air?0x08:0);      /* flag F */
    buf[0x8a]= (uint8_t)(packet_count / 2); /* [M] a counter that ADVANCES     */
    buf[0x8b]= playing ? 0xfa : 0xfe;       /* [M] real players: fa / fe       */
    put_be(&buf[0x92], (uint32_t)(bpm*100.0 + 0.5), 2);       /* BPM*100 */
    buf[0x9d]= playing ? 0x09 : 0x00;       /* [M] real players: 09 / 00       */
    buf[0x9e]= master ? 1 : 0;              /* Mm                              */
    buf[0x9f]= my;                          /* Mh: who we yield master to      */
    put_be(&buf[0xa0], beat, 4);            /* absolute beat counter           */
    buf[0xa6]= (uint8_t)(((bb + 3) % 4) + 1);                 /* Bb 1..4 */
    put_be(&buf[0xc8], packet_count, 4);
    return DJLINK_STATUS_LEN;
}

size_t djlink_build_takeover(uint8_t *buf, const char *name, uint8_t device)
{
    memset(buf, 0, DJLINK_TAKEOVER_LEN);
    memcpy(buf, djlink_magic, 10);
    buf[0x0a] = 0x26;                       /* type: takeover request */
    if (name) { size_t n = strlen(name); if (n > 20) n = 20; memcpy(&buf[0x0b], name, n); }
    /* payload from 0x1f: 0x01 0x00 D [len_r=0x0004] 0x00 0x00 0x00 D */
    buf[0x1f]=0x01; buf[0x20]=0x00; buf[0x21]=device;
    buf[0x22]=0x00; buf[0x23]=0x04;          /* len_r = 4 */
    buf[0x24]=0x00; buf[0x25]=0x00; buf[0x26]=0x00; buf[0x27]=device;
    return DJLINK_TAKEOVER_LEN;
}

size_t djlink_build_beat(uint8_t *buf, const char *name, uint8_t device,
                         double bpm, uint8_t bb)
{
    memset(buf, 0, DJLINK_BEATPKT_LEN);
    memcpy(buf, djlink_magic, 10);
    buf[0x0a] = 0x28;                        /* tipo beat */
    if (name) { size_t n = strlen(name); if (n > 20) n = 20; memcpy(&buf[0x0b], name, n); }
    buf[0x1f] = 0x01;                        /* byte after the name */
    buf[0x20] = 0x00;                        /* subtype 00 */
    buf[0x21] = device;                      /* D */
    buf[0x22] = 0x00; buf[0x23] = 0x3c;      /* len_r = 0x003c */

    /* intervals (ms) to the next beats/bars, from the current period */
    uint32_t beat_ms = (bpm > 1.0) ? (uint32_t)(60000.0 / bpm + 0.5) : 500;
    uint8_t  b       = (uint8_t)(((bb + 3) % 4) + 1);          /* 1..4 */
    uint32_t to_bar  = beat_ms * (uint32_t)(5 - b);            /* to the next downbeat */
    put_be(&buf[0x24], beat_ms,          4);   /* nextBeat */
    put_be(&buf[0x28], beat_ms * 2,      4);   /* 2ndBeat  */
    put_be(&buf[0x2c], to_bar,           4);   /* nextBar  */
    put_be(&buf[0x30], beat_ms * 4,      4);   /* 4thBeat  */
    put_be(&buf[0x34], to_bar + beat_ms*4, 4); /* 2ndBar   */
    put_be(&buf[0x38], beat_ms * 8,      4);   /* 8thBeat  */
    memset(&buf[0x3c], 0xff, 0x18);            /* 0xff padding */

    put_be(&buf[0x54], 0x00100000, 4);         /* Pitch: +0% (unity) */
    put_be(&buf[0x5a], (uint32_t)(bpm * 100.0 + 0.5), 2);   /* BPM*100 */
    buf[0x5c] = b;                             /* Bb: beat in bar */
    buf[0x5f] = device;                        /* redundant copy of D */
    return DJLINK_BEATPKT_LEN;
}

size_t djlink_build_takeover_resp(uint8_t *buf, const char *name, uint8_t device)
{
    memset(buf, 0, DJLINK_TKRESP_LEN);
    memcpy(buf, djlink_magic, 10);
    buf[0x0a] = 0x27;                        /* tipo: takeover response */
    if (name) { size_t n = strlen(name); if (n > 20) n = 20; memcpy(&buf[0x0b], name, n); }
    buf[0x1f] = 0x01;
    buf[0x20] = 0x00;                        /* subtype 00 */
    buf[0x21] = device;                      /* D: our number */
    buf[0x22] = 0x00; buf[0x23] = 0x08;      /* len_r = 0x0008 */
    buf[0x24] = 0x00; buf[0x25] = 0x00; buf[0x26] = 0x00;
    buf[0x27] = device;                      /* D again */
    buf[0x28] = 0x00; buf[0x29] = 0x00; buf[0x2a] = 0x00; buf[0x2b] = 0x01;
    return DJLINK_TKRESP_LEN;
}

uint8_t djlink_parse_takeover_req(const uint8_t *buf, size_t len)
{
    if (len < 0x28) return 0;
    if (memcmp(buf, djlink_magic, 10) != 0) return 0;
    if (buf[0x0a] != 0x26) return 0;
    return buf[0x21];                        /* who wants to become master */
}

/* ---- Media query and response (0x05 / 0x06) -------------------------------
 *
 * The player asks "what medium do you have in slot S?". [M] If we declare a
 * rekordbox track in our status packets but never answer this, it times out
 * and displays "E-8309 Link Access Error". */

bool djlink_parse_media_query(const uint8_t *buf, size_t len, uint8_t *from_dev, uint8_t *slot)
{
    if (len < 0x26) return false;
    if (memcmp(buf, djlink_magic, 10) != 0) return false;
    if (buf[0x0a] != 0x05) return false;
    if (from_dev) *from_dev = buf[0x21];      /* D: who is asking */
    if (slot)     *slot     = buf[0x25];      /* Sr: which slot */
    return true;
}

/* Write 's' as UTF-16BE into max_bytes, zero padded. */
static void put_utf16(uint8_t *p, const char *s, size_t max_bytes)
{
    memset(p, 0, max_bytes);
    size_t n = strlen(s);
    for (size_t i = 0; i < n && (2 * i + 1) < max_bytes; i++) {
        p[2 * i]     = 0x00;
        p[2 * i + 1] = (uint8_t)s[i];
    }
}

size_t djlink_build_media_resp(uint8_t *buf, const char *name, uint8_t device,
                               uint8_t slot, const char *media_name, uint16_t tracks)
{
    memset(buf, 0, DJLINK_MEDIARSP_LEN);
    memcpy(buf, djlink_magic, 10);
    buf[0x0a] = 0x06;                       /* media response */
    buf[0x0b] = 0x00;                       /* subtype */
    if (name) { size_t n = strlen(name); if (n > 20) n = 20; memcpy(&buf[0x0c], name, n); }
    buf[0x20] = 0x00; buf[0x21] = device;   /* D: who answers, us */
    buf[0x22] = 0x00; buf[0x23] = 0x9c;     /* len_r = 0x009c */
    buf[0x27] = device;                     /* Dr: device owning the slot */
    buf[0x2b] = slot;                       /* Sr: the slot asked about */

    if (!media_name) {
        /* EMPTY SLOT: nothing mounted. Everything zero — name, date, track
         * count — and Tr=0, meaning no database. It is the honest answer for
         * the slots we do NOT declare in our status packets. */
        buf[0xaa] = 0x00;                   /* Tr = 0: no database */
        return DJLINK_MEDIARSP_LEN;
    }

    put_utf16(&buf[0x2c], media_name, 0x40);            /* medium name      */
    put_utf16(&buf[0x6c], "2026-01-01", 0x28);          /* creation date    */
    put_be(&buf[0xa6], tracks, 2);          /* rekordbox track count        */
    buf[0xa8] = 0x00;                       /* col: UI colour, default      */
    buf[0xaa] = 0x01;                       /* Tr: 01 = rekordbox database  */
    buf[0xab] = 0x00;                       /* set: no "My Settings"        */
    put_be(&buf[0xae], 1, 2);               /* plists: one playlist         */
    put_be(&buf[0xb0], 1000000, 4);         /* total space, invented        */
    put_be(&buf[0xb8], 900000, 4);          /* free space, invented         */
    return DJLINK_MEDIARSP_LEN;
}


/* Fader Start (type 0x02). See djlink.h for the semantics, which are NOT
 * play/pause but those of a DJM channel fader. Same structure as the other
 * :50001 packets — magic, type, name, 0x01, subtype, device, length, payload —
 * except that here the payload is the four commands, so len_r = 4. */
size_t djlink_build_fader_start(uint8_t *buf, const char *name, uint8_t device,
                                const uint8_t cmd[4])
{
    memset(buf, 0, DJLINK_FADER_LEN);
    memcpy(buf, djlink_magic, 10);
    buf[0x0a] = 0x02;                        /* fader start */
    if (name) { size_t n = strlen(name); if (n > 20) n = 20; memcpy(&buf[0x0b], name, n); }
    buf[0x1f] = 0x01;                        /* byte after the name */
    buf[0x20] = 0x00;                        /* subtype 00 */
    buf[0x21] = device;                      /* D: our number */
    buf[0x22] = 0x00; buf[0x23] = 0x04;      /* len_r = 4: the four commands */
    buf[0x24] = cmd ? cmd[0] : DJLINK_FADER_NONE;   /* C1 -> player 1 */
    buf[0x25] = cmd ? cmd[1] : DJLINK_FADER_NONE;   /* C2 -> player 2 */
    buf[0x26] = cmd ? cmd[2] : DJLINK_FADER_NONE;   /* C3 -> player 3 */
    buf[0x27] = cmd ? cmd[3] : DJLINK_FADER_NONE;   /* C4 -> player 4 */
    return DJLINK_FADER_LEN;
}
