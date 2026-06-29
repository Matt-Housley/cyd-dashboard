#!/usr/bin/env python3
"""Convert a TTF/OTF font to LovyanGFX-compatible Adafruit GFX font headers.

Usage:
    python3 tools/convert_font.py path/to/AkzidenzGrotesk.ttf

Generates include/fonts/AkzidenzGrotesk_<size>pt.h for each required size.
Requires: pip install freetype-py
"""

import sys
import os
import math

try:
    import freetype
except ImportError:
    print("Install freetype-py first:  pip install freetype-py")
    sys.exit(1)

SIZES = [9, 12, 18, 24, 40, 56, 72]
FIRST_CHAR = 0x20  # space
LAST_CHAR = 0x7E   # tilde

def convert_font(ttf_path):
    face = freetype.Face(ttf_path)
    font_name = os.path.splitext(os.path.basename(ttf_path))[0].replace("-", "_").replace(" ", "_")

    out_dir = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "include", "fonts")
    os.makedirs(out_dir, exist_ok=True)

    all_header_path = os.path.join(out_dir, f"{font_name}.h")

    includes = []

    for size in SIZES:
        face.set_char_size(size * 64)

        bitmaps = []
        glyphs = []
        bitmap_offset = 0

        for char_code in range(FIRST_CHAR, LAST_CHAR + 1):
            face.load_char(chr(char_code), freetype.FT_LOAD_RENDER | freetype.FT_LOAD_TARGET_MONO)
            glyph = face.glyph
            bitmap = glyph.bitmap

            width = bitmap.width
            height = bitmap.rows
            x_offset = glyph.bitmap_left
            y_offset = -glyph.bitmap_top
            x_advance = glyph.advance.x >> 6

            bits = []
            for row in range(height):
                for col in range(width):
                    byte_index = col // 8
                    bit_index = 7 - (col % 8)
                    if row < len(bitmap.buffer) // max(bitmap.pitch, 1):
                        pixel = (bitmap.buffer[row * bitmap.pitch + byte_index] >> bit_index) & 1
                    else:
                        pixel = 0
                    bits.append(pixel)

            packed = []
            for i in range(0, len(bits), 8):
                byte = 0
                for j in range(8):
                    if i + j < len(bits):
                        byte |= bits[i + j] << (7 - j)
                packed.append(byte)

            bitmaps.extend(packed)
            glyphs.append((bitmap_offset, width, height, x_advance, x_offset, y_offset))
            bitmap_offset += len(packed)

        var_prefix = f"{font_name}_{size}pt"
        header_name = f"{var_prefix}.h"
        header_path = os.path.join(out_dir, header_name)
        includes.append(header_name)

        with open(header_path, "w") as f:
            guard = f"{var_prefix}_H".upper()
            f.write(f"#ifndef {guard}\n#define {guard}\n\n")
            f.write('#include <lgfx/v1/lgfx_fonts.hpp>\n\n')

            f.write(f"const uint8_t {var_prefix}_Bitmaps[] PROGMEM = {{\n")
            for i, b in enumerate(bitmaps):
                if i % 12 == 0:
                    f.write("    ")
                f.write(f"0x{b:02X}")
                if i < len(bitmaps) - 1:
                    f.write(", ")
                if i % 12 == 11:
                    f.write("\n")
            f.write("\n};\n\n")

            f.write(f"const GFXglyph {var_prefix}_Glyphs[] PROGMEM = {{\n")
            for i, (off, w, h, xa, xo, yo) in enumerate(glyphs):
                char_code = FIRST_CHAR + i
                f.write(f"    {{ {off:5d}, {w:3d}, {h:3d}, {xa:3d}, {xo:4d}, {yo:4d} }}")
                if i < len(glyphs) - 1:
                    f.write(",")
                comment_char = chr(char_code) if 0x21 <= char_code <= 0x7E else " "
                f.write(f"  // 0x{char_code:02X} '{comment_char}'\n")
            f.write("};\n\n")

            y_advance = face.size.height >> 6
            f.write(f"const GFXfont {var_prefix}_Font PROGMEM = {{\n")
            f.write(f"    (uint8_t  *){var_prefix}_Bitmaps,\n")
            f.write(f"    (GFXglyph *){var_prefix}_Glyphs,\n")
            f.write(f"    0x{FIRST_CHAR:02X}, 0x{LAST_CHAR:02X}, {y_advance}\n")
            f.write("};\n\n")
            f.write(f"#endif // {guard}\n")

        print(f"  Generated {header_path}")

    with open(all_header_path, "w") as f:
        guard = f"{font_name}_H".upper()
        f.write(f"#ifndef {guard}\n#define {guard}\n\n")
        for inc in includes:
            f.write(f'#include "fonts/{inc}"\n')
        f.write(f"\n#endif // {guard}\n")
    print(f"  Generated {all_header_path}")
    print(f"\nFont name prefix: {font_name}")
    print("Update font references to use:  (const lgfx::GFXfont*) &{font_name}_<size>pt_Font")

if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(f"Usage: {sys.argv[0]} <path-to-ttf>")
        sys.exit(1)
    convert_font(sys.argv[1])
