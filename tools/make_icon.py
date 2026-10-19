"""Generates res/TaskbarTuner.ico.

The icon is drawn procedurally: a rounded tile with a diagonal gradient, a thin
line at the top edge (the setting this tool is best known for) and three bars
standing in for taskbar buttons. Rendering uses 4x supersampling, which is
plenty for a 16 px tray icon.
"""

import struct
import sys
import zlib
from pathlib import Path

SIZES = [16, 32, 48, 256]
SS = 4


def lerp(a, b, t):
    return a + (b - a) * t


def rounded_box_covers(x, y, left, top, right, bottom, radius):
    """True when the point is inside a rounded rectangle."""
    if x < left or x > right or y < top or y > bottom:
        return False

    cx = min(max(x, left + radius), right - radius)
    cy = min(max(y, top + radius), bottom - radius)
    dx = x - cx
    dy = y - cy
    return dx * dx + dy * dy <= radius * radius


def render(size):
    scale = size * SS
    pixels = [[(0, 0, 0, 0)] * scale for _ in range(scale)]

    inset = scale * 0.045
    radius = scale * 0.22
    left, top = inset, inset
    right, bottom = scale - inset, scale - inset

    # Background gradient, top-left to bottom-right.
    for y in range(scale):
        for x in range(scale):
            if not rounded_box_covers(x, y, left, top, right, bottom, radius):
                continue
            t = (x / scale + y / scale) / 2.0
            r = int(lerp(43, 109, t))
            g = int(lerp(108, 76, t))
            b = int(lerp(246, 255, t))
            pixels[y][x] = (r, g, b, 255)

    def blend_rect(x0, y0, x1, y1, rad, color, alpha):
        """Draws a white-ish rounded bar on top of the tile."""
        for y in range(int(y0), int(y1) + 1):
            for x in range(int(x0), int(x1) + 1):
                if not (0 <= x < scale and 0 <= y < scale):
                    continue
                if not rounded_box_covers(x, y, x0, y0, x1, y1, rad):
                    continue
                r0, g0, b0, a0 = pixels[y][x]
                r1, g1, b1 = color
                a1 = alpha / 255.0
                pixels[y][x] = (
                    int(r0 * (1 - a1) + r1 * a1),
                    int(g0 * (1 - a1) + g1 * a1),
                    int(b0 * (1 - a1) + b1 * a1),
                    max(a0, int(255 * a1)),
                )

    white = (255, 255, 255)

    # Top edge line.
    line_h = scale * 0.035
    blend_rect(
        left + scale * 0.14,
        top + scale * 0.12,
        right - scale * 0.14,
        top + scale * 0.12 + line_h,
        line_h / 2,
        white,
        235,
    )

    # Three taskbar bars.
    bar_h = scale * 0.085
    bar_radius = bar_h / 2
    widths = [0.40, 0.30, 0.34]
    for index, width in enumerate(widths):
        y = top + scale * (0.36 + index * 0.155)
        x0 = left + scale * 0.14
        blend_rect(x0, y, x0 + scale * width, y + bar_h, bar_radius, white, 240)

    # Downsample.
    result = []
    for y in range(size):
        row = []
        for x in range(size):
            r = g = b = a = 0
            for sy in range(SS):
                for sx in range(SS):
                    pr, pg, pb, pa = pixels[y * SS + sy][x * SS + sx]
                    r += pr
                    g += pg
                    b += pb
                    a += pa
            n = SS * SS
            row.append((r // n, g // n, b // n, a // n))
        result.append(row)

    return result


def encode_bmp(rows):
    """Encodes the pixels as a 32bpp bottom-up DIB without the file header."""
    size = len(rows)
    header = struct.pack(
        "<IiiHHIIiiII",
        40,          # biSize
        size,        # biWidth
        size * 2,    # biHeight (XOR + AND mask)
        1,           # biPlanes
        32,          # biBitCount
        0,           # biCompression
        0,           # biSizeImage
        0, 0, 0, 0,
    )

    xor = bytearray()
    for y in range(size - 1, -1, -1):
        for x in range(size):
            r, g, b, a = rows[y][x]
            xor += bytes((b, g, r, a))

    # Fully opaque image: the AND mask is all zeros, padded to 4 bytes per row.
    mask_stride = ((size + 31) // 32) * 4
    and_mask = bytes(mask_stride * size)

    return header + bytes(xor) + and_mask


def encode_png(rows):
    """Encodes the pixels as an RGBA PNG, which the 256 px entry uses.

    Uncompressed, that entry alone is 256 KB; PNG keeps the whole icon around
    25 KB. Windows reads PNG-compressed icons since Vista.
    """
    size = len(rows)

    raw = bytearray()
    for row in rows:
        raw.append(0)  # filter type: none
        for r, g, b, a in row:
            raw += bytes((r, g, b, a))

    def chunk(tag, data):
        payload = tag + data
        return (struct.pack(">I", len(data)) + payload +
                struct.pack(">I", zlib.crc32(payload) & 0xFFFFFFFF))

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    png += chunk(b"IEND", b"")
    return png


def main():
    output = Path(sys.argv[1] if len(sys.argv) > 1 else "res/TaskbarTuner.ico")

    images = []
    for size in SIZES:
        rows = render(size)
        images.append((size, encode_png(rows) if size >= 256 else encode_bmp(rows)))

    offset = 6 + 16 * len(images)
    directory = bytearray(struct.pack("<HHH", 0, 1, len(images)))
    payload = bytearray()

    for size, data in images:
        directory += struct.pack(
            "<BBBBHHII",
            0 if size >= 256 else size,
            0 if size >= 256 else size,
            0,
            0,
            1,
            32,
            len(data),
            offset + len(payload),
        )
        payload += data

    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(directory + payload)
    print(f"wrote {output} ({output.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
