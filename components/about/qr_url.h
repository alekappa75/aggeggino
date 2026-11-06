/* ============================================================================
 * qr_url.h — QR code pointing at the repository. GENERATED.
 *
 *      github.com/alekappa75/aggeggino
 *
 * DO NOT EDIT BY HAND. To change the address, regenerate it:
 *
 *      python3 -c "import segno; q=segno.make('URL', error='L'); \
 *                  print(q.version, len(q.matrix))"
 *
 * and rewrite the table with the same convention: one entry per row, BIT 0 is
 * the LEFTMOST column.
 *
 * WHY A TABLE AND NOT AN IMAGE. A QR code is not a picture, it is a grid of
 * squares that must stay WHOLE. Going through an image inherits a scale
 * someone else decided, and any rescaling or antialiasing blurs the module
 * edges and makes it unreadable to scanners. Here each module is a solid
 * square of side QR_SCALE, by construction. It is also 100 bytes instead of
 * roughly 15 KB.
 *
 * [M] VERIFIED, not merely generated: the same grid drawn with these
 * parameters was fed back to a decoder and returned the exact address. Without
 * the quiet zone it is NOT decoded — those pixels on each side are part of the
 * code, not an aesthetic margin.
 *
 * Version 2, error correction L (7%). Error correction exists to survive
 * scratches and crooked printing; on a clean backlit screen the lowest level
 * is enough, and it keeps the address within the 25x25 modules that fit the
 * available space.
 * ==========================================================================*/
#ifndef QR_URL_H
#define QR_URL_H

#include <stdint.h>

#define QR_MOD    25          /* modules per side */

/* Pixels per module. MUST be an integer, or the module edges blur.
 *
 * At 3 px the code was 87 px across: a decoder read it, but a hand-held phone
 * struggled. On this panel a 3 px module is roughly a quarter of a millimetre,
 * about the limit of what a camera can resolve. At 4 px it grows by a third. */
#define QR_SCALE   4

/* THE QUIET ZONE IS NOT NEGOTIABLE. Going from 2 modules to 1 would save 8 px,
 * but the standard asks for 4 and we are already at half: it is the margin
 * that absorbs viewing angle and reflections. Removing it to gain space would
 * have hurt exactly the readability it was meant to improve. */
#define QR_QUIET   2          /* quiet-zone modules per side */

#define QR_PX     ((QR_MOD + 2 * QR_QUIET) * QR_SCALE)   /* 116 px across */

/* One entry per row, bit 0 = column 0. A set bit is a dark module. */
static const uint32_t QR_BITS[QR_MOD] = {
    0x1FC257F, 0x1046641, 0x175B65D, 0x1743A5D, 0x175405D,
    0x1040A41, 0x1FD557F, 0x0002D00, 0x105965B, 0x0F88D01,
    0x129AAC9, 0x17DC40D, 0x0AF5EC4, 0x051A991, 0x1F2E1EF,
    0x0F204B1, 0x15FF859, 0x0F12700, 0x155087F, 0x0B1A841,
    0x0BFD15D, 0x1C74B5D, 0x1FB4E5D, 0x1D64541, 0x1251F7F,
};

#endif /* QR_URL_H */
