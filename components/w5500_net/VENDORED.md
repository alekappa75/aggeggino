# ioLibrary — third-party sources, copied into the project

Source: https://github.com/Wiznet/ioLibrary_Driver
Licence: BSD 3-clause, Copyright (c) 2013 WIZnet Co., LTD.
(the notice is retained in each file's header, as the licence requires)

Only the files the W5500 needs are copied, under `ioLibrary/`:

    wizchip_conf.c/.h     chip configuration and the SPI callbacks
    socket.c/.h           the BSD-like socket layer
    W5500/w5500.c/.h      the W5500 register driver

`fetch_iolibrary.sh` (and its PowerShell twin) can re-fetch them from upstream.
It is kept for updating, not as a build step: the sources are committed, so
`idf.py build` works straight after a clone.

## Why they are COMMITTED and not fetched at build time

Because this component deliberately works around a **defect in the library's
own `recv()`**, and that workaround is tied to a specific upstream state.

In the IPv4 branch, `recv()` checks `SF_IO_NONBLOCK` *before* looking for data:

    #else
        if (sock_io_mode & (1 << sn)) return SOCK_BUSY;   <- was
        if (recvsize != 0) break;                          <- never reached
    #endif

With `SF_IO_NONBLOCK` nothing is ever read. The IPv6 branch has the two checks
in the right order. `w5500_net.c` therefore does not call the faulty function at
all: it performs the check the library is missing (RX_RSR says how many bytes
there really are) and then does what `recv()` would do after its wrong guard.
See `rx_drain()` and the comment above it.

A fetch at build time does `git clone --depth 1` of the default branch. Whoever
clones this repository next year would get a different revision than the one
this code was written and measured against — and if upstream changes or fixes
that guard, the workaround's premise shifts with no sign that anything happened.
Committing the sources pins them.

## What has NOT been modified

Nothing. The files are upstream's, byte for byte, comments in their original
language included. The Italian-to-English pass done across the rest of the
project deliberately stopped at this directory: a local edit would make it
impossible to tell, later, whether a difference against upstream is theirs or
ours.

`_WIZCHIP_` must be defined as W5500. The component's `CMakeLists.txt` already
passes `-D_WIZCHIP_=5500`.
