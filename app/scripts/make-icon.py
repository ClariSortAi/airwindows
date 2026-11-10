"""Render the application icon from its SVG sources and write src/app.ico.

    python app/scripts/make-icon.py [--preview PATH.png]

src/app-icon.svg is the drawing. src/app-icon-small.svg is the same idea with
the fine detail removed, used for 16 to 32 pixels where detail turns to mud.
Each size is rendered on its own by Inkscape, so small sizes are drawn, not
shrunk. Sizes below 256 are stored as 32-bit bitmaps and 256 as PNG, the
layout every Windows tool accepts.

Run this only when the drawing changes; src/app.ico is checked in. Needs
Inkscape 1.x (on PATH or in Program Files) and Pillow.
"""
import argparse
import io
import os
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

from PIL import Image

SOURCE = Path(__file__).resolve().parents[1] / "src"
SIZES = (16, 20, 24, 32, 40, 48, 64, 128, 256)
SMALL_UP_TO = 32


def find_inkscape():
    for name in ("inkscape.com", "inkscape"):
        found = shutil.which(name)
        if found:
            return found
    for base in (os.environ.get("ProgramFiles"), os.environ.get("ProgramFiles(x86)")):
        if base:
            candidate = Path(base) / "Inkscape" / "bin" / "inkscape.com"
            if candidate.is_file():
                return str(candidate)
    sys.exit("Inkscape was not found. Install Inkscape 1.x or put it on PATH.")


def render(inkscape, svg, size, folder):
    target = Path(folder) / f"{svg.stem}-{size}.png"
    subprocess.run(
        [inkscape, str(svg), "--export-type=png", f"--export-filename={target}", "-w", str(size), "-h", str(size)],
        check=True, capture_output=True,
    )
    image = Image.open(target).convert("RGBA")
    if image.size != (size, size):
        sys.exit(f"{svg.name} rendered at {image.size}, expected {size}")
    return image


def bitmap_entry(image):
    """A 32-bit device-independent bitmap as .ico stores it: rows bottom-up, then an AND mask."""
    size = image.width
    pixels = image.tobytes("raw", "BGRA")
    rows = [pixels[y * size * 4:(y + 1) * size * 4] for y in range(size)]
    mask_stride = ((size + 31) // 32) * 4
    header = struct.pack("<IiiHHIIiiII", 40, size, size * 2, 1, 32, 0, size * size * 4, 0, 0, 0, 0)
    # Alpha carries the transparency, so the one-bit mask stays empty.
    return header + b"".join(reversed(rows)) + bytes(mask_stride * size)


def png_entry(image):
    data = io.BytesIO()
    image.save(data, format="PNG", optimize=True)
    return data.getvalue()


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--preview", type=Path, help="also write the 256 pixel render here")
    args = parser.parse_args()
    inkscape = find_inkscape()
    entries = []
    with tempfile.TemporaryDirectory() as folder:
        for size in SIZES:
            svg = SOURCE / ("app-icon-small.svg" if size <= SMALL_UP_TO else "app-icon.svg")
            image = render(inkscape, svg, size, folder)
            entries.append((size, png_entry(image) if size == 256 else bitmap_entry(image)))
            if size == 256 and args.preview:
                image.save(args.preview)
    directory = struct.pack("<HHH", 0, 1, len(entries))
    offset = 6 + 16 * len(entries)
    body = b""
    for size, data in entries:
        # Width and height of 0 mean 256 in this format.
        directory += struct.pack("<BBBBHHII", size % 256, size % 256, 0, 0, 1, 32, len(data), offset + len(body))
        body += data
    target = SOURCE / "app.ico"
    target.write_bytes(directory + body)
    print(f"Wrote {target} ({len(directory) + len(body)} bytes, sizes {', '.join(map(str, SIZES))})")


if __name__ == "__main__":
    main()
