"""Render the MSIX logo assets from the application icon SVGs.

    python app/scripts/make-store-assets.py [--preview PATH.png]

Writes every PNG that app/packaging/msix/AppxManifest.template.xml references
into app/packaging/msix/Assets/ and removes PNGs there that are no longer part
of the set. The list follows "Construct your Windows app's icon" on Microsoft
Learn (https://learn.microsoft.com/windows/apps/design/iconography/app-icon-construction):

- AppList (Square44x44Logo): scale-100/125/150/200/400, and targetsize
  16..256 in plain, _altform-unplated and _altform-lightunplated forms. Without
  the unplated forms the taskbar and Start draw the icon on a plate.
- SmallTile (Square71x71Logo), MedTile (Square150x150Logo), WideTile
  (Wide310x150Logo), LargeTile (Square310x310Logo): scale-100/125/150/200/400.
  Windows 11 ignores tiles; the Store still requires MedTile.
- StoreLogo (package logo): scale-100/125/150/200/400. Required by the Store.

src/app-icon-small.svg draws the icon when it is 32 px or smaller, as
make-icon.py does; src/app-icon.svg draws everything larger. Each icon size is
rendered by Inkscape at that exact size, never shrunk. Run this only when the
drawing changes; the PNGs are checked in. Needs Inkscape 1.x and Pillow.
"""
import argparse
import importlib.util
import tempfile
from pathlib import Path

from PIL import Image

APP = Path(__file__).resolve().parents[1]
SOURCE = APP / "src"
ASSETS = APP / "packaging" / "msix" / "Assets"
SMALL_UP_TO = 32
SCALES = (100, 125, 150, 200, 400)
TARGET_SIZES = (16, 20, 24, 30, 32, 36, 40, 48, 60, 64, 72, 80, 96, 256)
# Base size and the share of the shorter side the icon covers. AppList and
# StoreLogo are the icon itself; tiles keep the icon smaller, centred on a
# transparent tile, as the shell expects.
FAMILIES = {
    "AppList": ((44, 44), 1.0),
    "StoreLogo": ((50, 50), 1.0),
    "SmallTile": ((71, 71), 0.6),
    "MedTile": ((150, 150), 0.6),
    "WideTile": ((310, 150), 0.6),
    "LargeTile": ((310, 310), 0.6),
}


def load_make_icon():
    spec = importlib.util.spec_from_file_location("make_icon", Path(__file__).with_name("make-icon.py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def scaled(value, scale):
    # Learn's table rounds half up: 44 px at 125% is 55, 71 px at 150% is 107.
    return int(value * scale / 100 + 0.5)


def plan():
    """Map file name -> (canvas width, canvas height, icon size)."""
    files = {}
    for family, ((width, height), share) in FAMILIES.items():
        for scale in SCALES:
            w, h = scaled(width, scale), scaled(height, scale)
            files[f"{family}.scale-{scale}.png"] = (w, h, max(1, round(min(w, h) * share)))
    for size in TARGET_SIZES:
        for suffix in ("", "_altform-unplated", "_altform-lightunplated"):
            files[f"AppList.targetsize-{size}{suffix}.png"] = (size, size, size)
    return files


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--preview", type=Path, help="also write a contact sheet of every asset here")
    args = parser.parse_args()
    make_icon = load_make_icon()
    inkscape = make_icon.find_inkscape()
    files = plan()
    ASSETS.mkdir(parents=True, exist_ok=True)
    icons = {}
    with tempfile.TemporaryDirectory() as folder:
        for size in sorted({icon for _, _, icon in files.values()}):
            svg = SOURCE / ("app-icon-small.svg" if size <= SMALL_UP_TO else "app-icon.svg")
            icons[size] = make_icon.render(inkscape, svg, size, folder)
    for name, (width, height, size) in files.items():
        canvas = Image.new("RGBA", (width, height), (0, 0, 0, 0))
        canvas.alpha_composite(icons[size], ((width - size) // 2, (height - size) // 2))
        if canvas.getextrema()[3][1] == 0:
            raise SystemExit(f"{name} rendered fully transparent")
        canvas.save(ASSETS / name, format="PNG", optimize=True)
    stale = [path for path in ASSETS.glob("*.png") if path.name not in files]
    for path in stale:
        path.unlink()
        print(f"Removed stale {path.name}")
    if args.preview:
        images = [Image.open(ASSETS / name) for name in sorted(files)]
        sheet = Image.new("RGBA", (sum(i.width for i in images) + 4 * len(images), max(i.height for i in images)), (128, 128, 128, 255))
        x = 0
        for image in images:
            sheet.alpha_composite(image.convert("RGBA"), (x, 0))
            x += image.width + 4
        sheet.save(args.preview)
    print(f"Wrote {len(files)} assets to {ASSETS}")


if __name__ == "__main__":
    main()
