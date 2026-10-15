"""Generates SpaceWindow's application icon.

A modern take on the original 1.x icon - three overlapping boxes, red, yellow
and green, like folders in the treemap - drawn with anti-aliasing,
rounded corners, title bands and soft shadows.  Standard library only.

    python make_icon.py <output-directory>

writes SpaceWindow.ico (16-256 px) and SpaceWindow.png (256 px).
"""

import struct
import sys
import zlib
from pathlib import Path

SUPERSAMPLE = 4
SIZES = [16, 20, 24, 32, 40, 48, 64, 256]

# (left, top, right, bottom) in units of the icon size, back to front - the
# same arrangement as the original 32x32 icon.
BOXES = [
    ((0.04, 0.04, 0.60, 0.57), (232, 72, 72)),    # red
    ((0.24, 0.27, 0.78, 0.78), (250, 196, 40)),   # yellow
    ((0.45, 0.48, 0.96, 0.96), (52, 190, 90)),    # green
]


def shade(color, factor):
    return tuple(max(0, min(255, round(c * factor))) for c in color)


def inside_rounded(px, py, left, top, right, bottom, radius):
    if px < left or px >= right or py < top or py >= bottom:
        return False
    cx = min(max(px, left + radius), right - radius)
    cy = min(max(py, top + radius), bottom - radius)
    dx, dy = px - cx, py - cy
    return dx * dx + dy * dy <= radius * radius


class Canvas:
    """Premultiplied RGBA floats, composited with "source over"."""

    def __init__(self, size):
        self.size = size
        self.pixels = [[0.0, 0.0, 0.0, 0.0] for _ in range(size * size)]

    def blend(self, x, y, color, alpha):
        p = self.pixels[y * self.size + x]
        inv = 1.0 - alpha
        p[0] = color[0] * alpha + p[0] * inv
        p[1] = color[1] * alpha + p[1] * inv
        p[2] = color[2] * alpha + p[2] * inv
        p[3] = alpha + p[3] * inv

    def rounded_rect(self, left, top, right, bottom, radius, color_at, alpha=1.0):
        """Fills a rounded rectangle given in pixels; color_at(fy) gives the colour at a height fraction."""
        x0, y0 = max(0, int(left)), max(0, int(top))
        x1, y1 = min(self.size, int(right) + 1), min(self.size, int(bottom) + 1)
        height = max(1.0, bottom - top)
        for y in range(y0, y1):
            color = color_at((y + 0.5 - top) / height)
            for x in range(x0, x1):
                if inside_rounded(x + 0.5, y + 0.5, left, top, right, bottom, radius):
                    self.blend(x, y, color, alpha)


def render(size):
    s = size * SUPERSAMPLE
    canvas = Canvas(s)
    detailed = size >= 32
    unit = s
    for (l, t, r, b), base in BOXES:
        left, top, right, bottom = l * unit, t * unit, r * unit, b * unit
        radius = 0.07 * unit
        border = max(SUPERSAMPLE, 0.035 * unit)

        if detailed:
            # Soft shadow: stacked, slightly larger, translucent copies.
            for i in range(6, 0, -1):
                spread = i * 0.006 * unit
                canvas.rounded_rect(left - spread + 0.015 * unit, top - spread + 0.03 * unit,
                                    right + spread + 0.015 * unit, bottom + spread + 0.03 * unit,
                                    radius + spread, lambda fy: (0, 0, 0), 0.035)

        dark = shade(base, 0.62)
        canvas.rounded_rect(left, top, right, bottom, radius, lambda fy, c=dark: c)

        light = shade(base, 1.18)
        face = base
        canvas.rounded_rect(left + border, top + border, right - border, bottom - border,
                            max(0.0, radius - border),
                            lambda fy, a=light, f=face: tuple(a[k] + (f[k] - a[k]) * fy for k in range(3)))

        # The folder's title band, with a stroke of "text" in it.
        band = 0.20 * (bottom - top)
        canvas.rounded_rect(left + border, top + border, right - border, top + border + band,
                            max(0.0, radius - border), lambda fy, c=shade(base, 0.86): c)
        if detailed:
            canvas.rounded_rect(left + border * 2.2, top + border + band * 0.38,
                                left + (right - left) * 0.55, top + border + band * 0.66,
                                band * 0.14, lambda fy: (255, 255, 255), 0.85)

    # Average SUPERSAMPLE x SUPERSAMPLE blocks, then un-premultiply.
    out = []
    n = SUPERSAMPLE * SUPERSAMPLE
    for y in range(size):
        row = []
        for x in range(size):
            acc = [0.0, 0.0, 0.0, 0.0]
            for sy in range(SUPERSAMPLE):
                base_index = (y * SUPERSAMPLE + sy) * s + x * SUPERSAMPLE
                for sx in range(SUPERSAMPLE):
                    p = canvas.pixels[base_index + sx]
                    acc[0] += p[0]; acc[1] += p[1]; acc[2] += p[2]; acc[3] += p[3]
            a = acc[3] / n
            if a <= 0.0:
                row.append((0, 0, 0, 0))
            else:
                row.append(tuple(min(255, round(acc[k] / n / a)) for k in range(3)) + (min(255, round(a * 255)),))
        out.append(row)
    return out


def png_bytes(pixels):
    size = len(pixels)
    raw = b"".join(b"\x00" + bytes(c for px in row for c in px) for row in pixels)

    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def dib_bytes(pixels):
    """A 32-bpp icon image: BITMAPINFOHEADER, bottom-up BGRA, then the AND mask."""
    size = len(pixels)
    header = struct.pack("<IiiHHIIiiII", 40, size, size * 2, 1, 32, 0, 0, 0, 0, 0, 0)
    xor = b"".join(bytes((px[2], px[1], px[0], px[3])) for row in reversed(pixels) for px in row)
    mask_row = ((size + 31) // 32) * 4
    mask = bytearray()
    for row in reversed(pixels):
        bits = bytearray(mask_row)
        for x, px in enumerate(row):
            if px[3] == 0:
                bits[x // 8] |= 0x80 >> (x % 8)
        mask += bits
    return header + xor + bytes(mask)


def main():
    out_dir = Path(sys.argv[1] if len(sys.argv) > 1 else ".")
    out_dir.mkdir(parents=True, exist_ok=True)
    images = {size: render(size) for size in SIZES}

    blobs = [png_bytes(images[size]) if size >= 256 else dib_bytes(images[size]) for size in SIZES]
    header = struct.pack("<HHH", 0, 1, len(SIZES))
    offset = 6 + 16 * len(SIZES)
    entries = b""
    for size, blob in zip(SIZES, blobs):
        dim = 0 if size >= 256 else size
        entries += struct.pack("<BBBBHHII", dim, dim, 0, 0, 1, 32, len(blob), offset)
        offset += len(blob)
    (out_dir / "SpaceWindow.ico").write_bytes(header + entries + b"".join(blobs))
    (out_dir / "SpaceWindow.png").write_bytes(png_bytes(images[256]))
    print(f"wrote {out_dir / 'SpaceWindow.ico'} and {out_dir / 'SpaceWindow.png'}")


if __name__ == "__main__":
    main()
