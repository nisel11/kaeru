#!/usr/bin/env python3
"""
gen_font.py — font spritesheet generator for normally looks font in custom fastboot ui.

Renders ASCII 32..126 (95 glyphs) into a fixed 12×8 grid of 56×56-pixel
cells and writes a self-contained PNG: advance widths are embedded in a
metadata row at y=0 so board-device.c needs no per-font C arrays.

Spritesheet format
------------------
  Width = 12 * 56 = 672 px
  Height = 1 + 8 * 56 = 449 px

  Row y=0 — metadata: pixel x=i has advance of glyph i in R channel.
  Rows y=1+ — glyph tiles, row-major. R=G=B=255, A=coverage (0..255).

Choosing --font-size
--------------------
  The right size depends on your display resolution:

    2400×1080  →  --font-size 32  (default)
    1600×720   →  --font-size 20

  For cell=56 keep font-size in the range 20..48; above 48 they clip the cell edges.

Usage
-----
  python3 gen_font.py Inter_28pt-Medium.ttf img49.png
  python3 gen_font.py Inter_28pt-SemiBold.ttf img50.png

  Then add this img to logo.bin via LogoBuilder or some other tool
"""

import argparse
import os
import sys

from PIL import Image, ImageDraw, ImageFilter, ImageFont

COLS = 12
ROWS = 8
CELL = 56
GLYPHS = 95
FIRST_CH = 32


def render_glyph_aa(font_2x, ch, top_pad):
    canvas = CELL * 2
    hi_res = Image.new("L", (canvas, canvas), 0)
    draw  = ImageDraw.Draw(hi_res)
    draw.text((3, top_pad * 2), ch, fill=255, font=font_2x, anchor="la")
    hi_res = hi_res.filter(ImageFilter.SHARPEN)
    lo_res = hi_res.resize((CELL, CELL), Image.LANCZOS)
    full = Image.new("L", (CELL, CELL), 255)
    return Image.merge("RGBA", (full, full, full, lo_res))


def generate(ttf_path, output_png, font_size):
    print(f"Loading '{ttf_path}'  font-size={font_size}px  cell={CELL}px ...")
    try:
        font_2x = ImageFont.truetype(ttf_path, size=font_size * 2)
        font_1x = ImageFont.truetype(ttf_path, size=font_size)
    except OSError as exc:
        print(f"Error: {exc}", file=sys.stderr)
        sys.exit(1)

    # Check if any glyph exceeds 96x96 limit
    chars = [chr(c) for c in range(FIRST_CH, FIRST_CH + GLYPHS)]
    for ch in chars:
        bbox = font_1x.getbbox(ch) # (left, top, right, bottom)
        if bbox:
            w = bbox[2] - bbox[0]
            h = bbox[3] - bbox[1]
            if w > 96 or h > 96:
                print(f"ERROR: Glyph '{ch}' is too large ({w}x{h}). Max allowed is 96x96.")
                sys.exit(1)

    ascent, descent = font_1x.getmetrics()
    top_pad = max(0, (CELL - ascent - descent) // 2)
    print(f"Metrics: ascent={ascent}  descent={descent}  top_pad={top_pad}px")

    chars    = [chr(c) for c in range(FIRST_CH, FIRST_CH + GLYPHS)]
    advances = [max(1, min(int(round(font_1x.getlength(ch))) + 1, CELL)) for ch in chars]
    print(f"Advances: min={min(advances)}  max={max(advances)}  space={advances[0]}px")

    sheet_w = COLS * CELL
    sheet_h = 1 + ROWS * CELL
    sheet   = Image.new("RGBA", (sheet_w, sheet_h), (0, 0, 0, 0))

    # Metadata row: pixel x=idx contains advance for glyph idx.
    # We use pixel index 95 (last) to store overall font metrics.
    meta_pixels = [(adv, 0, 0, 0) for adv in advances]
    
    # Store metrics in the last pixel: R=line_height, G=CELL, B=ascent, A=descent
    line_height = ascent + descent
    meta_pixels.append((line_height, CELL, ascent, descent))
    meta_pixels += [(0, 0, 0, 0)] * (sheet_w - len(meta_pixels))
    
    meta_row = Image.new("RGBA", (sheet_w, 1))
    meta_row.putdata(meta_pixels)
    sheet.paste(meta_row, (0, 0))

    for i, ch in enumerate(chars):
        if i >= COLS * ROWS:
            break
        sheet.paste(
            render_glyph_aa(font_2x, ch, top_pad),
            ((i % COLS) * CELL, 1 + (i // COLS) * CELL),
        )

    sheet.save(output_png, "PNG")
    size_kb = os.path.getsize(output_png) / 1024
    print(f"Saved: {output_png}  ({sheet_w}x{sheet_h} px, {size_kb:.1f} KB)")
    print(f"LogoBuilder:  image:{output_png} {sheet_w} {sheet_h} 4")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(
        description="Generate a self-contained font spritesheet for custom font in kaeru",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    parser.add_argument("ttf",    help="Path to .ttf / .otf font file.")
    parser.add_argument("output", help="Output PNG path (e.g. img49.png).")
    parser.add_argument("--font-size", type=int, default=32, metavar="N",
                        help="Render size in pixels (default 32). ")
    args = parser.parse_args()
    generate(args.ttf, args.output, args.font_size)
