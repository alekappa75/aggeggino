# Pioneer Pro DJ Link & Ableton Link — operational notes

**A knowledge base obtained in the field**
Document version: **2.0** · July 2026
Previous version: 1.0 · June 2026 (LUMEN development, read-only)

> An Italian original of this document is kept alongside it as
> [`protocol-notes.it.md`](protocol-notes.it.md). Where the two disagree, the
> Italian is the one that was written while measuring.

---

## 0. Purpose, method, and how much to trust each claim

This document collects what has been decoded and validated in the field for the
two protocols **Pioneer Pro DJ Link** and **Ableton Link**. Version 1.0 came out
of the development of LUMEN (an audio→DMX application), which uses both
protocols **read-only** and with modest accuracy requirements. This version 2.0
was rewritten during the development of **djbridge**, a bidirectional bridge on
an ESP32 (M5Stack CoreS3-SE) that must also **transmit** and be accepted as
tempo master by the players — a far stricter requirement, which forced us to
measure things we had previously been able to guess at.

The reverse engineering is **clean-room**: direct observation of the traffic
with real hardware (2× Pioneer **XDJ-700**, **Ableton Live**, **Push 3**
standalone). Where reference reimplementations exist (Deep Symmetry's
beat-link/dysentery, `ableton-link-rs`) they were used **for orientation only**,
never copied, and in at least one case we established that their choice **does
not coincide** with ours (§1.5).

### 0.1 The reliability scale — read this before using the document

Every non-obvious claim is marked. The distinction is deliberate: in an earlier
revision some plausible hypotheses had been written down as established fact,
and that led to wrong design decisions. **A declared uncertainty is worth more
than an invented certainty.**

| Mark | Meaning |
|------|---------|
| **[M]** | **Measured** directly on real hardware. Includes the reference to the capture or the log. |
| **[D]** | **Deduced** from measurements, with the reasoning spelled out. Solid, but not observed directly. |
| **[I]** | An unverified **hypothesis**. Reported because it is useful; confirm it before relying on it. |
| **[E]** | From an **external source** (third-party documentation). Not verified by us except where stated. |

The **reference capture** cited as `[cap07]` is: two XDJ-700s, device 1 master
and device 2 in sync, 69.7 s, 1430 packets — pitch-fader movements on the master
at various speeds, then jog wheel on the master in both directions.

### 0.2 Conventions

- Byte offsets are **0-based**, in hexadecimal (`0x..`).
- **BE** = big-endian, **LE** = little-endian.
- `u8/u16/u32/u64/i64` = unsigned/signed integers of the stated width.
- Ableton Link times are in **microseconds (µs)** unless noted otherwise.

---

## 1. Pioneer Pro DJ Link

A UDP protocol on the local network (typically **wired Ethernet**) with which
Pioneer players announce their presence, their status and their beats.

### 1.1 Ports, magic, types

| Port | Use | Transport | Type (`0x0a`) | Actual length [M] |
|------|-----|-----------|---------------|-------------------|
| **50000** | Keep-alive / announce | broadcast | `0x06` | **54 bytes** |
| **50001** | **Beat packet** | broadcast | `0x28` | **96 bytes** |
| **50002** | **CDJ status** | broadcast/unicast | `0x0a` | **284 bytes** |

**Magic** (the first 10 bytes of every packet): `51 73 70 74 31 57 6d 4a 4f 4c`
→ `"Qspt1WmJOL"`.

The **type** is the byte at `0x0a`. The **device number** is at `0x21`.

The lengths in the table are the ones observed on the XDJ-700s [M, cap07]: 69
keep-alives, 308 beats, 1049 statuses, all of the stated length without
exception. Different models may use longer statuses (dysentery documents
variants for the CDJ-2000NXS2) — always read with a length check, and do not
assume.

### 1.2 Keep-alive (port 50000, type `0x06`)

Its job is to announce oneself and be admitted onto the network. The layout,
**validated byte by byte against a real XDJ-700** [M]:

```
0000  51 73 70 74 31 57 6d 4a 4f 4c 06 00 58 44 4a 2d      Qspt1WmJOL..XDJ-
0010  37 30 30 00 00 00 00 00 00 00 00 00 00 00 00 00      700.............
0020  01 02 00 36 01 01 c8 3d fc 09 8f 65 a9 fe 8f 65      ...6...=...e...e
0030  02 00 00 00 01 00                                    ......
```

| Offset | Contents |
|--------|----------|
| `0x0a` | `0x06` = keep-alive type |
| `0x0c`–`0x1f` | device name, ASCII, zero-padded |
| `0x20`–`0x21` | `01 02` |
| `0x22`–`0x23` | `00 36` = 54, total length |
| `0x24` | device number |
| `0x25` | `0x01` |
| `0x26`–`0x2b` | **MAC** of the interface the packet leaves from |
| `0x2c`–`0x2f` | **IP** of the same interface |
| `0x30` | **`0x02`** |
| `0x34` | **`0x01`** |

The bytes at `0x30` and `0x34` are critical: an early implementation used
`0x30=01 0x31=01` and **the player would not admit the device** [M]. The MAC and
IP must be those of the interface actually used (§3.2).

Retransmission rate: **≈1.5 s** [M].

### 1.3 Beat packet (port 50001, type `0x28`, 96 bytes)

Emitted **once per beat** [M, cap07: 154 packets in 69 s at ~132 BPM = 2.22/s,
exactly the tempo]. The `beat in bar` field cycles `1,2,3,4` [M].

| Offset | Field | Type | Notes |
|--------|-------|------|-------|
| `0x00`–`0x09` | magic | 10B | `Qspt1WmJOL` |
| `0x0a` | type | u8 | `0x28` |
| `0x0b`–`0x1e` | device name | ASCII | zero-padded |
| `0x1f` | — | u8 | `0x01` |
| `0x20` | subtype | u8 | `0x00` |
| `0x21` | **device number** | u8 | 1–4 |
| `0x22`–`0x23` | len_r | u16 BE | `0x003c` |
| `0x24`–`0x27` | **next beat** | u32 BE | ms to the next beat |
| `0x28`–`0x2b` | **2nd beat** | u32 BE | ms to the second beat |
| `0x2c`–`0x2f` | **next bar** | u32 BE | ms to the next downbeat |
| `0x30`–`0x33` | **4th beat** | u32 BE | |
| `0x34`–`0x37` | **2nd bar** | u32 BE | |
| `0x38`–`0x3b` | **8th beat** | u32 BE | |
| `0x3c`–`0x53` | padding | 24B | all `0xff` [M] |
| `0x54`–`0x57` | **pitch** | u32 BE | `PITCH_UNITY = 0x100000` = 1.0× |
| `0x5a`–`0x5b` | **track BPM** | u16 BE | nominal BPM × 100 |
| `0x5c` | **beat in bar** | u8 | 1–4 |
| `0x5f` | device number | u8 | redundant copy |

#### Formulas for the time fields [M, cap07]

Verified against the real values, with `beat_ms = 60000 / track_bpm`:

```
next beat = beat_ms
2nd beat  = beat_ms × 2
next bar  = beat_ms × (5 - beat_in_bar)
4th beat  = beat_ms × 4
2nd bar   = next_bar + beat_ms × 4
8th beat  = beat_ms × 8
```

Evidence from device 1 (track at 132 BPM, `beat_ms` = 454 ms): the observed
sequence of `next bar` is **1363 → 909 → 454** as `beat_in_bar` goes 2→3→4, that
is exactly `454 × (5-bb)`. And `2nd bar` read 3181, 2727, 2272, that is
`next_bar + 1818`. [M]

#### An important and counter-intuitive point [M]

**`beat_ms` is computed from the NOMINAL `track_bpm`, not from the effective
tempo.**

Device 2, with a track at 125 BPM and pitch +5.6% (so playing at 132), declared
`next beat = 480 ms` = 60000/125, **not** 454 ms = 60000/132. [M]

During a jog the discrepancy becomes glaring: the master declared
`next beat = 454 ms` while its beats were really falling at **396–441 ms** [M].

**The `pitch` field (`0x54`), on the other hand, FOLLOWS the jog bend**: during
the manoeuvre it went from +2.65% to +14.33% and back [M]. So the instantaneous
effective tempo is obtained from `track_bpm × pitch`, while `next beat` and its
derivatives are informative and do not reflect the pitch.

> **[I]** We do not know why the time fields ignore the pitch. Hypothesis: they
> are meant for the user interface's *quantize* (aligning an effect to the
> nominal grid) and not for synchronisation. Whoever wants to know when the next
> beat really falls has to compute it from `track_bpm × pitch`, or — better —
> use the packet's **moment of emission** (§1.7).

### 1.4 CDJ status packet (port 50002, type `0x0a`, 284 bytes)

| Offset | Field | Type | Notes |
|--------|-------|------|-------|
| `0x00`–`0x09` | magic | 10B | |
| `0x0a` | type | u8 | `0x0a` |
| `0x21` | **device number** | u8 | 1–4 |
| `0x84`–`0x87` | sync counter | u32 BE | |
| `0x89` | **flag F** | u8 | see below |
| `0x8c`–`0x8f` | **pitch P1** | u32 BE | see §1.5 |
| `0x92`–`0x93` | **track BPM** | u16 BE | × 100 |
| `0x98`–`0x9b` | **pitch P2** | u32 BE | see §1.5 |
| `0x9e` | **`Mm`** | u8 | **`2` = NOT a rekordbox track** |
| `0xa0`–`0xa3` | beat counter | u32 BE | absolute, increasing |
| `0xa6` | **beat in bar** | u8 | 1–4 |
| `0xc0`–`0xc3` | **pitch P3** | u32 BE | see §1.5 |
| `0xc4`–`0xc7` | **pitch P4** | u32 BE | see §1.5 |

#### Flag F (`0x89`) [E, partly verified]

| Bit | Name | Meaning |
|-----|------|---------|
| `0x02` | BPM_SYNC | the player was in sync but the DJ used the pitch bend: it chases the tempo but no longer aligns the beats |
| `0x08` | ON_AIR | the channel is open on the mixer |
| `0x10` | SYNCED | the player is in sync |
| `0x20` | **MASTER** | **it is the tempo master** |
| `0x40` | PLAYING | it is playing |

Values observed [M, cap07]: master `0xe4` = MASTER+PLAYING; slave in sync
`0xd4` = SYNCED+PLAYING.

On the **BPM_SYNC** bit we have two observations that agree:
- in `[cap07]`, where the jog was moved **on the master**, the bit never appears
  on either player: the slave followed without coming unstuck [M];
- in an earlier log, moving the jog **on the player in sync**, the bit appeared
  on precisely that one (`0xd4 → 0xd6`) and then went off again [M].

**[D]** The bit marks the player **whose beats have been moved by hand relative
to the master**, so it does not appear on the master. A bridge that wants to
know whether a player has "come unstuck" can use it, but must not expect it on
the master.

The counter at `0xa0` and the `beat in bar` at `0xa6` advance together and are
consistent with the beat packets [M]: whoever listens to the statuses alone can
reconstruct the position in the bar without seeing the beats.

**`Mm = 2`** signals a track that rekordbox has not analysed: the beat grid is
not reliable and `beat in bar` should be ignored.

### 1.5 The FOUR pitch fields of the status — measured semantics

This is the most important discovery of revision 2.0, and it resolved a
responsiveness problem that had stayed open for days.

The status contains **four** copies of the pitch, at `0x8c`, `0x98`, `0xc0`,
`0xc4` (called P1..P4 here). External sources document them as "slightly
different under different circumstances" without specifying how [E]. Measured
[M, cap07]:

| situation | P1 (`0x8c`) | P2 (`0x98`) | P3 (`0xc0`) | P4 (`0xc4`) |
|---|---|---|---|---|
| at rest | +2.65% | +2.65% | +2.65% | +2.65% |
| **fader** movement | all four move together |
| **jog wheel** (bend) | **+2.65%** | **+18.01%** | **+2.65%** | **+18.01%** |

**Conclusion [D]:**
- **P1 and P3** = **sustained** pitch (the fader's position, or the value imposed by sync)
- **P2 and P4** = **instantaneous** pitch, jog-wheel bend included

An independent and strong confirmation: device 2 had its **physical fader
centred** but reported `P1 = +5.60%` — which with its track at 125 BPM gives
exactly 132, the master's tempo [M]. So **P1 is not the physical position of the
slider**: it is the pitch the player is actually sustaining, which for a player
in sync is the one imposed by the master.

> **[I]** We do not know what the difference is between P1 and P3, nor between
> P2 and P4: in our captures the pairs always came out identical. They may
> differ on other models, or under conditions we did not exercise (loop, slip
> mode, reverse playback).

**The practical implication.** Whoever wants the *instantaneous effective* tempo
— for instance a bridge that must chase the pitch bend as well — has to read
**P2**. Whoever wants the *sustained* tempo reads P1.

**A deliberate divergence from beat-link.** The beat-link library uses **P1
(`0x8d`)** for `getEffectiveTempo()` [E, verified by reading the source]. The
choice is consistent with its purpose (there "effective" means "nominal ×
pitch", as opposed to the track's BPM alone), but it **does not capture the
bend**. djbridge reads **P2** deliberately, and that is what allowed it to
follow the jog wheel. Anyone porting code from one to the other needs to know.

### 1.6 Transmission rate — it is ADAPTIVE [M]

The fact that explained the "instant responsiveness" observed between two
Pioneer players.

**A CDJ does not transmit at a fixed rate.** Measured on the master in
`[cap07]`, in one-second windows:

| condition | status interval | frequency |
|---|---|---|
| at rest | **130–165 ms** | ~6.6 Hz |
| **while the pitch is moving** | **64 ms** | **~15.6 Hz** |

And the return to the slow rate is **immediate**: at 13 s the player was at
64 ms with the fader moving, at 14 s it was already at 136 ms with the fader
still. There is no timed "tail": the fast rate coincides with the state *it is
changing* [M].

The **beat packets**, by contrast, stay at one packet per beat under every
condition [M].

### 1.7 How a player in sync behaves — it does not chase, it repeats [M]

Measured by comparing master and slave at the same instant:

```
t=55.779   master P2=+9.61%  → 144.69 BPM      slave → 144.69 BPM
t=55.843   master P2=+18.01% → 155.77 BPM      slave → 155.77 BPM
```

Values **identical to the second decimal place** [M]. The player in sync
computes

```
own_pitch = master_effective_bpm / own_track_bpm − 1
```

and applies it. There is no tracking loop and no transient.

**Measured reaction delay: 4–5 ms**, constant, both with the fader and with the
jog wheel (20 samples during the jog: mean 4.3 ms, maximum 5 ms) [M].

**Accuracy of the beat instants.** Integrating the tempo declared in the
statuses and checking where the beats fall, the number of beats between two
consecutive beats comes out as [M]:

| phase | beats | typical deviation |
|---|---|---|
| at rest | 1.0000 | 0.33 % |
| fader movement | 0.9999 | 0.31 % |
| **jog wheel** | **1.0002** | **0.23 %** |

On a 440 ms beat, 0.3% is **1.3 ms** — and that figure includes network and
capture jitter, so the CDJ's own jitter is **below 1.3 ms**, even while the disc
is accelerating by 18%.

**Alignment between the two players** (the difference between the instants of
their respective beat packets) [M]:

| phase | mean | spread | max |
|---|---|---|---|
| at rest | −0.04 ms | 0.82 ms | 2.57 ms |
| fader | +0.05 ms | 2.30 ms | 4.48 ms |
| jog | +0.41 ms | 1.17 ms | 2.32 ms |

No drift, no progressive recovery, under any condition.

> **The consequence for anyone designing a bridge.** The promptness of a player
> in sync **is the rate at which the master talks to it**. There is nothing to
> optimise in its behaviour: its 4–5 ms are already excellent. All the
> observable delay belongs to the transmitter. In djbridge, transmitting a
> status every 200 ms on a fixed schedule produced delays of 46–238 ms on the
> players — exactly the uniform distribution of the wait for the next packet [M].

### 1.8 Master election, device number, takeover

**Election:** the master is the device whose status has bit `0x20` of `0x89` set.
There is at most one master on the network [M].

**Device number.** For reading alone, a high number can be used (7, say),
outside the players' own 1–4. To **transmit** and be accepted as master,
however, a **real number (1–4)** is needed, obtained through a staged claim
procedure [M, djbridge]: census of the numbers in use, then repeated
announcement of the chosen number in several stages, then adoption.

**Master takeover** [M, djbridge]: it is negotiated through the statuses. A
device that wants the master role asks for it, the current master yields by
naming its successor's number in its own status (the *master handoff* field),
and at that point the requester assumes the role. **The negotiation goes through
the statuses**: a device that stops transmitting them while it is master leaves
the network deadlocked, because nobody can take the role back. Whoever
implements a bridge must keep transmitting statuses even when not master,
simply declaring the truth.

### 1.9 Transmitting: what it takes to be a credible master

A summary of what was learnt building djbridge.

**All four pitch fields must be populated.** An early version filled only P2 and
left P1, P3 and P4 at zero: a player reading P1 (as beat-link does) would derive
`track_bpm × 0 = 0 BPM` from it [D — the defect is certain, the effect on the
player was not isolated experimentally]. Having no disc, the consistent choice is
**pitch unity on all four** and the whole tempo in `track_bpm`.

> A note on the semantics: a CDJ says *"track at 132 with pitch +5.6%"*, we say
> *"track at 139.4 with pitch 0%"*. Whoever reads it multiplies and gets the
> same number. The difference is invisible downstream, and it has the advantage
> of making `next beat` = 60000/track_bpm physically true as well — which on a
> real player it is not (§1.3).

**The beat packet's time formulas must be respected** (§1.3): they were verified
identical between our implementation and the real players [M].

**The rate must be adaptive** (§1.6): 150 ms at rest, ~64 ms while the tempo is
changing, with an immediate return. And above all, **on a tempo change the
status must go out at once**, not at the next periodic slot.

**The beat packet's moment of emission is the phase reference.** The players
align to that, not to the packet's fields (§1.3). Jitter in the emission
transfers directly onto their phase.

### 1.10 Metadata for the playing track — the `dbserver` protocol

The beat packet and the status say *how fast* and *where in the bar*, but not
**what** is playing. Title, artist and the rest are obtained by querying the
player that owns the media, over a separate TCP protocol.

The information needed to identify the track is in the master's **status**:
rekordbox id, the device hosting the media, the slot and the track type. With
those, the dialogue opens.

**Sequence [M, djbridge — validated on an XDJ-700]:**

| step | port | contents |
|---|---|---|
| 1 | TCP **12523** | the "switchboard": send the string `RemoteDBServer` preceded by its length (19 bytes in all: `00 00 00 0f` + `"RemoteDBServer"` + `00`); it replies naming **which port** will serve the data |
| 2 | TCP **1051** | a 5-byte handshake, which the player echoes back |
| 3 | " | **setup** (message `0x0000`): declares who we are |
| 4 | " | **metadata** (`0x2002`): "how many items do you have for track N?" |
| 5 | " | **render** (`0x3000`): "send them to me" → title, artist and tempo arrive |

On the players observed, the port named at step 1 always turned out to be
**1051**, the same one they use between themselves.

#### Which device number to present — two constraints, both measured [M]

This is the point that took the most attempts, and neither constraint is
documented anywhere else as far as we know.

1. **It must be a number between 1 and 4.** Presenting a high number (7, typical
   of a read-only Virtual CDJ), **the player closes the connection without
   replying**. Verified with Wireshark: the setup packet is byte for byte
   identical to a real player's except for the device number, and the answer is
   a FIN.

2. **It must be a number PRESENT ON THE NETWORK**, not merely free. Posing as
   device 3 — free, but announced by nobody — the player accepts the setup but
   answers **"0 items"**: it does not know us and has no menu for us. Posing as
   device 4, which was ours and was being announced, it answers with **11 items**
   and the metadata arrives.

**[D]** The data service therefore seems to keep state for each *announced*
device, and the menu exists only for whoever is present on the network. The
strategy adopted in djbridge: if our number is already 1–4 we use our own,
otherwise we **borrow** the number of a player that really is present.

#### When to query

Only on a change of the pair **(master device, track id)** [M]: that covers both
a track change on the same player and a change of master, without flooding the
player with requests. A reply with an absurd count (0, or implausible values)
should be treated as "try again later", not as a permanent error: it happens
when the player has just loaded and has not indexed yet.

---

### 1.11 The "E-8309 Link Access Error" and the RPC portmapper

**Symptom:** both players show `E-8309 Link Access Error` on their displays,
persistently, while continuing to work.

**Cause, found with Wireshark [M]:** when a device declares in its status that it
is **playing from a USB slot**, the players want to read its tracks — and to do
so they try to **mount its filesystem over NFS**. They therefore send an
**ONC-RPC PORTMAP GETPORT** request on **UDP port 111**, asking where the `MOUNT`
service answers.

If the device **stays silent**, the players retry **every second, forever**, and
after a while report the error on the display.

**Remedy [M]:** there is no need to implement NFS. It is enough to **say no**, in
a formally correct way. A valid RPC reply with **port = 0** means "that program
is not registered here" and is sufficient to make the players stop reporting the
error.

The structure of the reply (28 bytes, or 24 if it is not a GETPORT):

```
XID              4B   identical to the request's
message type     4B   0x00000001  = REPLY
reply state      4B   0x00000000  = MSG_ACCEPTED
verifier flavor  4B   0x00000000  = AUTH_NULL
verifier length  4B   0x00000000
accept state     4B   0x00000000  = SUCCESS
port             4B   0x00000000  = not registered     (GETPORT only, proc 3)
```

Answer **GETPORT (proc 3)** and, as a courtesy, **NULL (proc 0)** as well.

> **The general point, which holds beyond this case:** on a DJ Link network,
> silence is not neutral. The players read the absence of an answer as a fault
> and persist; an explicit negative answer closes the matter. The same holds for
> the *media queries* — the slot interrogations a player sends to find out what
> media the others hold, which this document does not cover in a section of its
> own — and, we suspect, for other services we have not come across.

**An implementation cost worth bearing in mind:** the portmapper occupies one of
the W5500's sockets, and it has **eight in all**. In djbridge it is the eighth,
so the allotment is full: any further service will require freeing one or
multiplexing. [M]

**An unverified alternative [I]:** if a bridge **did not declare** that it has
media playing from USB, perhaps the players would not ask for the mount at all,
making the portmapper unnecessary. We have not tried it, because declaring
playback serves another purpose (a stopped master does not drag the others'
phase).

---

## 2. Ableton Link

A peer-to-peer UDP protocol on the local network (typically **WiFi**). There is
no master: all peers share a common **timeline**. Two phases: **discovery**
(multicast) and **measurement** (unicast).

### 2.1 Discovery — multicast `224.76.78.75:20808`, magic `_asdp_v`

**Header (20 bytes):**

| Offset | Field | Type | Notes |
|--------|-------|------|-------|
| `0x00`–`0x06` | magic | 7B | `_asdp_v` |
| `0x07` | version | u8 | `0x01` |
| `0x08` | message type | u8 | `0x01`=ALIVE, `0x02`=RESPONSE, `0x03`=BYEBYE |
| `0x09` | TTL | u8 | typically `0x05` |
| `0x0a`–`0x0b` | reserved | 2B | zero |
| `0x0c`–`0x13` | peer id | 8B | **ephemeral**, see §2.6 |

**Payload: TLV** (`key` 4 ASCII + `len` u32 BE + `value`). Known keys:

| Key | Len | Contents |
|-----|-----|----------|
| `tmln` | 24 | **timeline**: 3× u64 BE = `tempo` (µs/beat), `beatOrigin` (microbeat), `timeOrigin` (ghost µs) |
| `sess` | 8 | **session id** (= the founder's peer id) |
| `stst` | 17 | **start/stop state** |
| `mep4` | 6 | **measurement endpoint** IPv4: `IP`(4B) + `port`(u16 BE) |

Notes:
- The **RESPONSE (`0x02`)** is sent **unicast** (invisible to a multicast-only
  sniffer). There is also a unicast state exchange (~107 bytes) with the **same
  format** as the ALIVE.
- A second multicast channel on **port 20909** (magic `ablsd_v`, payload with
  `local.transfer`) is **Ableton-specific and unrelated to the clock** → ignore
  it.

**The `stst` field (17 bytes):**

| Offset in the value | Field | Type |
|---|---|---|
| 0 | `playing` | u8 (`0x01`=play) |
| 1–8 | `beats` | i64 BE (microbeat) |
| 9–16 | `timestamp` | i64 BE (µs) |

The transport state is **shared across the session**. The **timeline is
continuous** and always advances, even with the transport stopped: tempo, beat
and phase are derived from the timeline alone.

### 2.2 Measurement — unicast to the `mep4`, magic `_link_v`

**Header (9 bytes):** magic `_link_v` (7B) + version `0x01` + message type
(`0x01`=PING, `0x02`=PONG). Then TLV with **u64** values:

**PING** (measurer → `mep4`): `__ht` = the measurer's host time (µs);
`_pgt` = previous ghost time, **absent in the first ping**.
→ first PING 25 bytes, subsequent ones 41.

**PONG** (peer → source port): `sess`, `__gt` (the responder's ghost time),
`__ht` (**echoed back**, for pairing), `_pgt` (echoed if present).
→ first PONG 57 bytes, subsequent ones 73.

The measurement is **bidirectional**. For a passive peer the active direction is
enough. The responder answers anyone who sends it a ping on the `mep4`.

### 2.3 GhostXForm

For every ping→pong pair matched through the echo of `__ht`:

```
host_mid = (host_send + host_recv) / 2
offset   = ghost_resp − host_mid
```

Keep a sliding window (16 samples, say) and take its **median** (robust against
outliers). `ghost(now) = host(now) + median_offset`.

The host↔ghost slope is ≈1.0 (both are real-time clocks in µs): drift on the
scale of seconds is negligible and the offset is re-estimated continuously, so
the median suffices. About 4 samples are needed to lock.

### 2.4 Phase math

```
bpm       = 60_000_000 / tempo
ghost_now = host_now_µs + median_offset
beats     = beatOrigin/1e6 + (ghost_now − timeOrigin) / tempo
phase     = beats mod quantum
```

`beatOrigin` is in **microbeats**. For the phase within the beat use `mod 1.0`,
for the phase within the bar `mod 4`.

### 2.5 Session merging

When two peers discover each other they **converge on a single `session id`**:
one wins and the other adopts its.

> **[I]** We have not verified the criterion for winning. Observed once: the
> Push's session prevailed, with Live abandoning its own after a few packets
> [M, a single episode]. Link's documentation speaks of a founding timestamp
> with a tie-break on the id [E]. **For a peer that does not want to impose its
> own tempo, the practical rule is to adopt whichever session prevails.**

### 2.6 Ephemeral peer ids

The `peer id` (and therefore the founder's `session id`) is **regenerated for
every session**: it is not a stable identifier for the device. Use it only to
group peers within the current session, never as a persistent identity.

### 2.7 Tempo propagation

A tempo change propagates **in real time to all peers**: they report the same
`tempo` at practically the same instant [M].

**Independent verification [M, djbridge]:** sampling the session's tempo at
20 Hz during changes imposed by Ableton Live, every change came out as a
**single step** — never a ramp. Five changes out of five: `0 ms, 1 step`. The
measurement's resolution is however 50 ms, so the correct statement is *"it does
not ramp perceptibly at 20 Hz"*, not *"it jumps instantaneously"*.

### 2.8 Using the official library (`abl_link`, C API)

For those who use the library instead of reimplementing it [E, from the header's
documentation, verified by reading the source]:

- **`abl_link_create(double bpm)` requires an initial tempo**: there is no
  constructor without one. But the instance is **initially disabled** and does
  not communicate on the network, so that tempo is imposed on nobody until it
  is enabled.
- Imposition happens only on a **commit** of the session state: the
  documentation says explicitly that the committed state **replaces** the
  session's and is communicated to the other peers.
- **There is no "I participate but do not propose" mode**: an enabled instance
  has a timeline of its own regardless, and it enters the merge.

**[D]** The practical consequence for a bridge that must be able to sit on the
network without imposing anything: it is not enough to abstain from calling
`set_tempo`, one has to verify that **no code path** commits. In djbridge a
deferred-flush path escaped the check and handed the tempo downstream after the
bridge had been switched off [M].

---

## 3. Network lessons (expensive in debugging)

### 3.1 Firewall / inbound UDP

*Little Snitch* (macOS) blocks **inbound** UDP to unsigned applications
**silently, without showing any warning**. Consequences observed: DJ Link
keep-alives received = 0; Link measurement that never locks. It holds for
**every** peer, Ableton Live included. Solution: an explicit inbound rule for
the ports used (DJ Link 50000–50002; Link 20808 + measurement), or sign the
bundle.

**Traffic from a terminal (`python`, `tcpdump`) gets through anyway**: the
sniffer sees everything and the application sees nothing, which makes the
diagnosis very confusing.

### 3.2 Choosing the interface (multi-homed)

With several interfaces (WiFi + LAN), do **not** choose by routing (on macOS the
link-local `169.254/16` route ends up on the WiFi) nor by subnet match alone
(ambiguous with several link-local addresses). The correct criterion: identify
**which interface the packets ARRIVE ON**, with **`IP_RECVIF`** (macOS/BSD) or
**`IP_PKTINFO`** (Linux) + `recvmsg()` + `if_indextoname()`.

### 3.3 WiFi and client isolation

Link's measurement is **unicast peer-to-peer**. With *client isolation* active on
the access point, the discovery multicast gets through but unicast between
clients is blocked → the peers see each other but **do not merge**. It has to be
disabled.

### 3.4 Sniffing the unicast

To capture the measurement with `tcpdump`, the sniffing machine must be **one
end** of the conversation (on WiFi a third client does not see other clients'
unicast even in promiscuous mode).

### 3.5 Hardware

The **Push 3 standalone is WiFi-only**; the XDJ-700s speak DJ Link over the
wire. In a mixed setup **the two protocols are on different networks**, and the
bridge has to be on both.

---

## 4. Implementation on ESP32 — lessons from djbridge

### 4.1 Parsing and memory

Small packets (≤284 bytes), parsing at fixed offsets (DJ Link) or linear TLV
(Link). **No dynamic allocation** needed.

**Internal memory is the scarce resource.** With one Ableton Link peer connected,
a CoreS3's internal heap dropped to **13 KB** [M]. The main items measured:

| item | before | after | notes |
|---|---|---|---|
| LVGL render buffers (2×) | 51,200 B | 30,720 B | 40 → 24 rows |
| LVGL task stack | 16,384 B | 8,192 B | it was using 4,800 |

Recovered: **+22 KB** of internal heap [M]. The LVGL buffers have to live in
internal RAM (the flush hands them to DMA): they **cannot** be moved to PSRAM.

### 4.2 Timing and clocks

- Host time in µs: **`esp_timer_get_time()`** (monotonic int64).
- The software timer is **punctual to within ~120 µs** [M]; the delay that
  matters is the one between the wake-up and the byte on the wire.

### 4.3 SPI bus contention — the main problem

On the CoreS3 the **W5500 (network), the display and the SD card share the same
SPI bus**, and the MISO pin is also the display's DC. Consequences measured [M]:

- a display flush held the bus for up to **10.7 ms** with 40-row buffers,
  **6.5 ms** with 24 rows;
- a network transaction can hold it for up to **18 ms**;
- the beat packet, which is the players' phase reference, was going out as much
  as **8.6 ms** after the timer's wake-up.

**The remedy adopted:** a *quiet window* around the beat's instant, in which the
bus's consumers abstain. It has to be enforced on **everybody**: in a first
version only the display honoured it, and the network drains went on taking the
bus. After extending it to all, the typical emission came down from ~7.5 ms to
~2.9 ms [M].

**A known limitation:** the window prevents a transaction from *starting*, it
does not shorten one already under way. A residual peak at ~7.4 ms remains [M],
attributable to a long transaction begun just before the window opened.

> For comparison: the real CDJs' jitter is **below 1.3 ms** under every
> condition (§1.7). That is the yardstick to use.

### 4.4 Crystal drift

The CoreS3's crystal and the player's do not coincide. Measured [M]:

- typical residual drift: **13–24 µs/s** (≈20 ppm);
- **the crystal warms up**: starting from cold, the ratio between the two clocks
  moved by **~20 ppm in the first five minutes**, with a time constant of ~130 s.

**[D]** A stored calibration constant is therefore not a solution: the value
that is right on a cold machine is wrong after ten minutes. An **online**
correction is needed.

**The approach that worked** [M]: feedback on the **slope** of the phase
deviation (not on its absolute value), with a fixed 60 s measurement window,
plus a slow term that recovers the accumulated offset. Result: RMS of the
deviation **0.89 ms**, no forced re-lock in over 6 minutes.

**A documented trap:** a moving average or an EMA on the deviation **does not
work** in the presence of phase re-locks. Every re-lock zeroes the deviation and
therefore introduces a **step** into the series; a linear regression reads that
as a slope. For a step `h` halfway through a window `T`, the spurious slope is
about `1.5·h/T` — with h=4 ms and T=30 s that is **200 ppm** against a true
drift of 20 [D, verified in simulation]. The correct road, if a continuous
window is wanted, is to **reconstruct the series** by adding back the jumps that
were applied (which are known exactly).

### 4.5 Responsiveness to known discontinuities

**A principle arrived at the hard way:** when a discontinuity is a **communicated
fact** (the status packet says the BPM has changed), filtering it is wrong. A
low-pass filter on the phase exists to smooth *measurement jitter*; applied to a
known change, it spends seconds chasing a value that is already available.

In djbridge the correct remedy turned out to be **re-anchoring** the phase
estimate to the fresh datum, exactly as was already done for hot-cue jumps — not
modifying the locking mechanism. Three earlier attempts (variable α, suspending
the forcing, deferred realignment) all failed because they acted on the
mechanism instead of on the information being given to it [M].

---

## 5. Open questions

Listed so that they do **not** become certainties by omission.

1. **Why the beat packet's time fields ignore the pitch** (§1.3). Measured with
   certainty, not explained. Hypothesis: they serve the UI's quantize.

2. **The difference between P1 and P3, and between P2 and P4** (§1.5). In our
   captures the pairs were always identical. We did not exercise loop, slip mode
   or reverse playback.

3. **The criterion for merging Link sessions** (§2.5). A single episode
   observed, external documentation unverified.

4. **A player occasionally slow to follow.** In two separate trials an XDJ-700
   took **~2 seconds** instead of the usual 50–200 ms to reach the imposed
   tempo, while the other arrived in 74 ms [M]. It happened once upwards and
   once downwards, so it is **not** the limit of the pitch's range. Cause
   unknown: it could depend on the player, on the track, or on the state it was
   in.

5. **The delay between the beat packet and the audio output.** All our phase
   measurements compare *packets* with *packets*. If a player had a systematic
   delay between the beat packet and the audio leaving its outputs, we would not
   see it. An acoustic reference or signal analysis would be needed.

6. **Whether the drift compensation holds across a change of master.** The
   choice implemented is to *keep* the estimate (it is a ratio between crystals,
   and our crystal does not change). Argued for, but **never observed in
   action**: in every test session the corrector was still in its measuring
   phase.

---

## 6. Code references

**LUMEN** (Python, read-only, pure stdlib):
- `djlink.py` — beat/status parsing, Virtual CDJ keep-alive, interface selection
  via `IP_RECVIF`/`IP_PKTINFO`, master selection.
- `link.py` — passive Ableton Link + measurement, median GhostXForm.

**djbridge** (C, ESP-IDF, bidirectional) — this repository:
- `components/djlink/` — parsing and **construction** of keep-alive, beat and
  status packets.
- `main/main.c` — the network state machine (census → claim → observer →
  master), drift compensation, the beat engine on transmit, the quiet window on
  the SPI bus.
- `components/linkbridge/` — a wrapper over the official `abl_link` library.

---

*Document revised July 2026. Claims marked **[M]** were measured on real
hardware; those marked **[D]** deduced from measurements with the reasoning
spelled out; those marked **[I]** are hypotheses declared as such; those marked
**[E]** come from external sources and have not been verified except where
stated.*
