#!/usr/bin/env python3
"""Regenerate src/fonts/dejavu_ext.cpp: DejaVu9/12 base ASCII glyphs (copied
verbatim from LovyanGFX) plus extra Unicode ranges rendered from DejaVuSans.ttf.

Extra ranges (see EXTRA below):
  0xA0-0xFF    Latin-1 Supplement (£, ©, ®, °, accented letters, ...)
  0x2013-0x2026 General Punctuation (en/em dash, curly quotes, bullet, ellipsis)
  0x20AC        Euro sign

Requires: freetype-py, and DejaVuSans.ttf + LovyanGFX DejaVu*.h (paths below).
Calibrated sizes: DejaVu9 -> 9.0px, DejaVu12 -> 12.0px (freetype mono hinting)."""
import re, freetype

# --- adjust these paths for your environment ---
TTF="/Users/matt/Documents/Arduino/libraries/lvgl/scripts/built_in_font/DejaVuSans.ttf"
LIB="/Users/matt/Downloads/cyd_dashboard/.pio/libdeps/esp32dev/LovyanGFX/src/lgfx/Fonts/Custom/"
OUT="/Users/matt/Downloads/cyd_dashboard/src/fonts/dejavu_ext.cpp"

# Extra codepoint ranges to append after the base ASCII block (inclusive).
EXTRA = [(0xA0, 0xFF), (0x2013, 0x2026), (0x20AC, 0x20AC)]

FL = freetype.FT_LOAD_RENDER | freetype.FT_LOAD_TARGET_MONO


def parse_font(path, name):
    """Parse a LovyanGFX GFXfont header into its raw tables.

    Args:
        path: Path to the LovyanGFX DejaVu*.h header.
        name: Font base name (e.g. "DejaVu9") used to match the array symbols.

    Returns:
        A tuple (bitmaps, glyphs, first, last, yAdvance):
        - bitmaps: list[int] of packed 1-bpp glyph rows.
        - glyphs:  list of (bitmapOffset, w, h, xAdvance, xOffset, yOffset) tuples.
        - first, last: inclusive ASCII codepoint bounds (expected 0x20..0x7E).
        - yAdvance: line height in pixels.
    """
    txt = open(path).read()
    m = re.search(re.escape(name)+r"Bitmaps\[\][^{]*\{([^}]*)\}", txt, re.S)
    bmp = [int(x, 16) for x in re.findall(r"0x[0-9a-fA-F]+", m.group(1))]
    m = re.search(re.escape(name)+r"Glyphs\[\][^{]*\{(.*?)\n\};", txt, re.S)
    glyphs = []
    for row in re.findall(r"\{\s*(-?\d+),\s*(-?\d+),\s*(-?\d+),\s*(-?\d+),\s*(-?\d+),\s*(-?\d+)\s*\}", m.group(1)):
        glyphs.append(tuple(int(v) for v in row))       # off,w,h,xadv,xoff,yoff
    m = re.search(re.escape(name)+r"\s+PROGMEM\s*=\s*\{[^,]*,[^,]*,\s*(0x[0-9a-fA-F]+),\s*(0x[0-9a-fA-F]+),\s*(\d+)", txt, re.S)
    return bmp, glyphs, int(m.group(1), 16), int(m.group(2), 16), int(m.group(3))


def render(face, cp):
    """Render one codepoint to a packed 1-bpp bitmap via freetype (mono hinting).

    Args:
        face: An open freetype Face, already sized.
        cp:   Unicode codepoint to render.

    Returns:
        A tuple (width, height, xAdvance, xOffset, yOffset, data) where `data`
        is the row-packed 1-bpp bitmap bytes for the glyph.
    """
    face.load_char(chr(cp), FL)
    g = face.glyph; b = g.bitmap
    bits = []
    for r in range(b.rows):
        for c in range(b.width):
            bits.append((b.buffer[r*b.pitch + (c >> 3)] >> (7-(c & 7))) & 1)
    out = bytearray()
    for i in range(0, len(bits), 8):
        byte = 0
        for k in range(8):
            if i+k < len(bits) and bits[i+k]:
                byte |= (0x80 >> k)
        out.append(byte)
    return b.width, b.rows, (g.advance.x+32) >> 6, g.bitmap_left, 1-g.bitmap_top, bytes(out)


def build(name, px):
    """Build an extended font: verbatim base ASCII plus the EXTRA Unicode ranges.

    Preserves the original 95 ASCII glyphs/bitmaps byte-for-byte and appends each
    codepoint in EXTRA (rendering it from the TTF, or a blank space-width glyph
    when the face lacks it).

    Args:
        name: Font base name to load from LIB (e.g. "DejaVu9").
        px:   Pixel size to render the extra glyphs at.

    Returns:
        A tuple (bitmap, glyphs, order, yAdvance): the combined bitmap bytes,
        glyph metric rows, the codepoint order list, and the line height.
    """
    bmp, gl, f, l, ya = parse_font(LIB+name+".h", name)
    assert f == 0x20 and l == 0x7e
    bitmap = bytearray(bmp)               # existing bytes preserved verbatim
    glyphs = [list(g) for g in gl]        # existing 95 ASCII glyphs
    order  = [(0x20+i) for i in range(95)]
    face = freetype.Face(TTF); face.set_char_size(0, round(px*64), 72, 72)
    for lo, hi in EXTRA:
        for cp in range(lo, hi+1):
            order.append(cp)
            if face.get_char_index(cp) == 0:              # absent → blank, space width
                glyphs.append([len(bitmap), 0, 0, gl[0x20-f][3], 0, 0]); continue
            w, h, xadv, left, yoff, data = render(face, cp)
            glyphs.append([len(bitmap), w, h, xadv, left, yoff])
            bitmap += data
    return bitmap, glyphs, order, ya


def emit(fh, name, bitmap, glyphs, order, ya):
    """Write one C++ GFXfont definition (bitmaps, glyphs, EncodeRanges, font).

    Args:
        fh:     Open output file handle.
        name:   C++ symbol name for the emitted font (e.g. "DejaVu9_ext").
        bitmap: Combined bitmap bytes from build().
        glyphs: Glyph metric rows from build().
        order:  Codepoint order list from build() (parallel to `glyphs`).
        ya:     yAdvance (line height) to record in the font struct.

    Returns:
        None. Output is written directly to `fh`.
    """
    fh.write(f"static const uint8_t {name}_bitmaps[] PROGMEM = {{"
             + ",".join(str(x) for x in bitmap) + "};\n\n")
    fh.write(f"static const lgfx::GFXglyph {name}_glyphs[] PROGMEM = {{\n")
    for g, cp in zip(glyphs, order):
        fh.write(f"  {{ {g[0]:4d},{g[1]:3d},{g[2]:3d},{g[3]:3d},{g[4]:3d},{g[5]:3d} }}, // 0x{cp:04x}\n")
    fh.write("};\n\n")
    # EncodeRange table: base ASCII, then each EXTRA range at its running base index.
    ranges = [(0x20, 0x7E, 0)]
    base = 95
    for lo, hi in EXTRA:
        ranges.append((lo, hi, base)); base += hi-lo+1
    last = EXTRA[-1][1]
    fh.write(f"static const lgfx::EncodeRange {name}_ranges[] PROGMEM = {{\n  "
             + " ".join(f"{{ 0x{s:X}, 0x{e:X}, {b} }}," for s, e, b in ranges)
             + "\n};\n\n")
    fh.write(f"extern const lgfx::GFXfont {name} PROGMEM = {{\n"
             f"  (uint8_t*){name}_bitmaps, (lgfx::GFXglyph*){name}_glyphs,\n"
             f"  0x20, 0x{last:X}, {ya}, {len(ranges)}, (lgfx::EncodeRange*){name}_ranges }};\n\n")


with open(OUT, "w") as fh:
    fh.write("// AUTO-GENERATED — DejaVu9/12 extended with Latin-1 (0xA0-0xFF),\n"
             "// General Punctuation (0x2013-0x2026: dashes, curly quotes, bullet,\n"
             "// ellipsis) and the Euro sign (0x20AC).\n"
             "// Base ASCII (0x20-0x7E) glyphs copied verbatim from LovyanGFX's DejaVu*;\n"
             "// the extra glyphs are rendered from DejaVuSans.ttf (freetype, mono).\n"
             "// Regenerate with tools/gen_dejavu_ext.py — do not edit by hand.\n"
             "#include <LovyanGFX.hpp>\n\n")
    for name, base, px in (("DejaVu9_ext", "DejaVu9", 9.0), ("DejaVu12_ext", "DejaVu12", 12.0),
                           ("DejaVu18_ext", "DejaVu18", 18.0)):
        bitmap, glyphs, order, ya = build(base, px)
        emit(fh, name, bitmap, glyphs, order, ya)
        print(f"{name}: {len(bitmap)} bitmap bytes, {len(glyphs)} glyphs (yAdv {ya})")
print("wrote", OUT)
