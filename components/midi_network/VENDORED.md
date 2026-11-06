# midi_network — a third-party component, copied into the project

Source: https://github.com/DatanoiseTV/midi2-ump-net-esp-idf
Version: 0.3.0 · Licence: MIT (see LICENSE)

## A note on the names

The DIRECTORY is called `midi_network` (and so that is the component's name to
ESP-IDF, and the one to put in REQUIRES). Files, headers, functions and Kconfig
symbols stay as upstream has them — `midi2_net.h`, `midi2_net_host_start()`,
`CONFIG_MIDI2_NET_*` — deliberately: renaming them would make it impossible to
compare this code against upstream when one needs to tell whether a difference
is ours or theirs.

## Why it is COPIED and not taken as a managed dependency

Writing a Network MIDI 2.0 implementation from scratch would mean reinventing a
solved problem — and discovering for oneself traps such as UMP Stream Endpoint
Discovery, without which the endpoint connects but exposes NO MIDI port to hosts
like macOS's "MIDI UMP".

Copied rather than referenced because it is a small, little-used project: we
want to be able to pin its version, tune its Kconfig and fix it without an
upstream update changing the behaviour under our feet.

## What has NOT been verified

The code has been READ, not tested by us. Before relying on it in production it
should be validated against a real client (macOS "MIDI UMP" or Windows MIDI
Services).

## Tuning applied (in sdkconfig.defaults)

The component's defaults are sized for streams of notes; we send ONE word at a
time (a System Real Time), hence:

    MIDI2_NET_RETX_RING      16 -> 2   retransmission ring
    MIDI2_NET_RETX_MAX_WORDS 16 -> 4   words kept per command
    MIDI2_NET_TX_QUEUE_LEN   16 -> 4   output queue

On their own these are worth about 7 KB of internal RAM saved.
