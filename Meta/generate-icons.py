#!/usr/bin/env python3
#
# Copyright (c) 2026-present, the Ladyflock developers.
#
# SPDX-License-Identifier: BSD-2-Clause

"""Generate all Ladyflock platform icons deterministically from tracked sources in UI/Icons/.

Tracked inputs:
  - UI/Icons/sources/Ladyflock-App-Icon.png
  - UI/Icons/ladyflock-emblem.svg
  - UI/Icons/ladyflock-emblem-mono.svg

Required tools:
  - Python 3 with Pillow (PIL)
  - rsvg-convert (librsvg)
"""

from __future__ import annotations

import argparse
import io
import re
import shutil
import subprocess
import tempfile
from pathlib import Path

from PIL import Image

REPO_ROOT = Path(__file__).resolve().parent.parent
ICONS_DIR = REPO_ROOT / "UI" / "Icons"
SOURCE_APP_ICON = ICONS_DIR / "sources" / "Ladyflock-App-Icon.png"
SOURCE_EMBLEM_SVG = ICONS_DIR / "ladyflock-emblem.svg"
SOURCE_MONO_SVG = ICONS_DIR / "ladyflock-emblem-mono.svg"
APP_ID = "io.github.kayyo321.Ladyflock"

EMBLEM_VB_W = 240.0
EMBLEM_VB_H = 196.0

# GNOME-style 1024x1024 squircle shell (100..924 with subtle bottom rim & drop shadow).
FREEDESKTOP_SQUIRCLE_SHADOW = """  <g fill="#1c1238" fill-opacity="0.0137">
    <rect x="0" y="-30" width="1024" height="1010" rx="310"/>
    <rect x="17.06" y="-17" width="989.88" height="969.93" rx="298"/>
    <rect x="34.12" y="-4" width="955.76" height="929.87" rx="286"/>
    <rect x="51.18" y="9" width="921.65" height="889.8" rx="274"/>
    <rect x="68.24" y="22" width="887.53" height="849.73" rx="262"/>
    <rect x="85.29" y="35" width="853.41" height="809.67" rx="250"/>
    <rect x="102.35" y="48" width="819.29" height="769.6" rx="238"/>
    <rect x="119.41" y="61" width="785.18" height="729.53" rx="226"/>
    <rect x="136.47" y="74" width="751.06" height="689.47" rx="214"/>
    <rect x="153.53" y="87" width="716.94" height="649.4" rx="202"/>
  </g>"""

SQUIRCLE_OUTER_D = "M337 100L687 100C774.14 100 772.46 99.75 818.27 118.31C834.53 124.9 856.81 141.79 869.51 154.49C882.21 167.19 899.1 189.47 905.69 205.73C924.25 251.54 924 249.86 924 337L924 687C924 774.14 924.25 772.46 905.69 818.27C899.1 834.53 882.21 856.81 869.51 869.51C856.81 882.21 834.53 899.1 818.27 905.69C772.46 924.25 774.14 924 687 924L337 924C249.86 924 251.54 924.25 205.73 905.69C189.47 899.1 167.19 882.21 154.49 869.51C141.79 856.81 124.9 834.53 118.31 818.27C99.75 772.46 100 774.14 100 687L100 337C100 249.86 99.75 251.54 118.31 205.73C124.9 189.47 141.79 167.19 154.49 154.49C167.19 141.79 189.47 124.9 205.73 118.31C251.54 99.75 249.86 100 337 100Z"
SQUIRCLE_RIM_D = "M337 100L687 100C774.14 100 772.46 99.75 818.27 118.31C834.53 124.9 856.81 141.79 869.51 154.49C882.21 167.19 899.1 189.47 905.69 205.73C924.25 251.54 924 249.86 924 337L924 677C924 764.14 924.25 762.46 905.69 808.27C899.1 824.53 882.21 846.81 869.51 859.51C856.81 872.21 834.53 889.1 818.27 895.69C772.46 914.25 774.14 914 687 914L337 914C249.86 914 251.54 914.25 205.73 895.69C189.47 889.1 167.19 872.21 154.49 859.51C141.79 846.81 124.9 824.53 118.31 808.27C99.75 762.46 100 764.14 100 677L100 337C100 249.86 99.75 251.54 118.31 205.73C124.9 189.47 141.79 167.19 154.49 154.49C167.19 141.79 189.47 124.9 205.73 118.31C251.54 99.75 249.86 100 337 100Z"
SQUIRCLE_INNER_BG_D = "M340.14 110L683.86 110C758.37 110 767.91 105.56 815.1 126.97C832.48 134.86 848.8 146.82 862.99 161.01C877.18 175.2 889.14 191.52 897.03 208.9C918.44 256.09 914 265.63 914 340.14L914 683.86C914 758.37 918.44 767.91 897.03 815.1C889.14 832.48 877.18 848.8 862.99 862.99C848.8 877.18 832.48 889.14 815.1 897.03C767.91 918.44 758.37 914 683.86 914L340.14 914C265.63 914 256.09 918.44 208.9 897.03C191.52 889.14 175.2 877.18 161.01 862.99C146.82 848.8 134.86 832.48 126.97 815.1C105.56 767.91 110 758.37 110 683.86L110 340.14C110 265.63 105.56 256.09 126.97 208.9C134.86 191.52 146.82 175.2 161.01 161.01C175.2 146.82 191.52 134.86 208.9 126.97C256.09 105.56 265.63 110 340.14 110Z"
SQUIRCLE_INNER_D = "M340.14 110L683.86 110C758.37 110 767.91 105.56 815.1 126.97C832.48 134.86 848.8 146.82 862.99 161.01C877.18 175.2 889.14 191.52 897.03 208.9C918.44 256.09 914 265.63 914 340.14L914 673.86C914 748.37 918.44 757.91 897.03 805.1C889.14 822.48 877.18 838.8 862.99 852.99C848.8 867.18 832.48 879.14 815.1 887.03C767.91 908.44 758.37 904 683.86 904L340.14 904C265.63 904 256.09 908.44 208.9 887.03C191.52 879.14 175.2 867.18 161.01 852.99C146.82 838.8 134.86 822.48 126.97 805.1C105.56 757.91 110 748.37 110 673.86L110 340.14C110 265.63 105.56 256.09 126.97 208.9C134.86 191.52 146.82 175.2 161.01 161.01C175.2 146.82 191.52 134.86 208.9 126.97C256.09 105.56 265.63 110 340.14 110Z"


def extract_svg_inner(svg_text: str) -> tuple[str, str]:
    """Split a canonical emblem SVG into (defs_inner, body_inner)."""
    defs_match = re.search(r"<defs>(.*?)</defs>", svg_text, re.DOTALL)
    defs_inner = defs_match.group(1).strip() if defs_match else ""
    body = re.sub(r"^<svg[^>]*>\s*", "", svg_text.strip(), flags=re.DOTALL)
    body = re.sub(r"\s*</svg>\s*$", "", body, flags=re.DOTALL)
    if defs_match:
        body = re.sub(r"<defs>.*?</defs>\s*", "", body, count=1, flags=re.DOTALL)
    return defs_inner, body.strip()


def recolor_emblem(svg_text: str, replacements: dict[str, str]) -> str:
    result = svg_text
    for old, new in replacements.items():
        result = result.replace(old, new)
    return result


def centered_transform(canvas_size: float, target_width: float, center_y: float | None = None) -> str:
    scale = target_width / EMBLEM_VB_W
    tx = (canvas_size - EMBLEM_VB_W * scale) / 2.0
    cy = canvas_size / 2.0 if center_y is None else center_y
    ty = cy - (EMBLEM_VB_H * scale) / 2.0
    return f"translate({tx:.2f} {ty:.2f}) scale({scale:.5f})"


def wrap_on_1024(svg_text: str, target_width: float, center_y: float = 512.0) -> str:
    defs_inner, body_inner = extract_svg_inner(svg_text)
    transform = centered_transform(1024.0, target_width, center_y)
    defs_block = f"  <defs>\n{defs_inner}\n  </defs>\n" if defs_inner else ""
    return (
        '<svg xmlns="http://www.w3.org/2000/svg" width="1024" height="1024" viewBox="0 0 1024 1024">\n'
        f"{defs_block}"
        f'  <g transform="{transform}">\n'
        f"{body_inner}\n"
        "  </g>\n"
        "</svg>\n"
    )


def render_svg_to_png_bytes(svg_text: str, width: int, height: int) -> bytes:
    rsvg = shutil.which("rsvg-convert")
    if not rsvg:
        raise RuntimeError("rsvg-convert is required to render SVG icons")
    with tempfile.NamedTemporaryFile("w", suffix=".svg", delete=False) as tmp:
        tmp.write(svg_text)
        tmp_path = Path(tmp.name)
    try:
        return subprocess.check_output([rsvg, "-w", str(width), "-h", str(height), str(tmp_path)])
    finally:
        tmp_path.unlink(missing_ok=True)


def render_svg_to_image(svg_text: str, size: int) -> Image.Image:
    png_bytes = render_svg_to_png_bytes(svg_text, size, size)
    return Image.open(io.BytesIO(png_bytes)).convert("RGBA")


def build_private_svg(shaded_svg: str) -> str:
    # Violet-dark squircle tile (#2F2347 -> #1D1430, QuietDesign.md §7) with soft lavender emblem.
    private_emblem = recolor_emblem(
        shaded_svg,
        {
            "#c674f8": "#eee3ff",
            "#9c50da": "#d8c8ff",
            "#742dc5": "#b99dff",
            "#4d0b9e": "#916df0",
            "#4e1599": "#e6dbff",
            "#1e0440": "#120824",
        },
    )
    defs_inner, body_inner = extract_svg_inner(private_emblem)
    # Match master Ladyflock-App-Icon.png 88..936 squircle geometry (848x848) and 87.1% emblem width (738px).
    transform = centered_transform(1024.0, 738.0, 512.0)
    return f'''<svg xmlns="http://www.w3.org/2000/svg" width="1024" height="1024" viewBox="0 0 1024 1024">
  <defs>
    <linearGradient id="priv-bg" x1="88" y1="88" x2="936" y2="936" gradientUnits="userSpaceOnUse">
      <stop offset="0%" stop-color="#352852"/>
      <stop offset="55%" stop-color="#271c3e"/>
      <stop offset="100%" stop-color="#19112b"/>
    </linearGradient>
    <linearGradient id="priv-rim" x1="0" y1="88" x2="0" y2="936" gradientUnits="userSpaceOnUse">
      <stop offset="0%" stop-color="#584878"/>
      <stop offset="100%" stop-color="#221738"/>
    </linearGradient>
{defs_inner}
  </defs>
  <rect x="88" y="88" width="848" height="848" rx="190" fill="url(#priv-bg)" stroke="url(#priv-rim)" stroke-width="10"/>
  <g transform="{transform}">
{body_inner}
  </g>
</svg>
'''


def build_freedesktop_svg(shaded_svg: str) -> str:
    defs_inner, body_inner = extract_svg_inner(shaded_svg)
    transform = centered_transform(1024.0, 708.0, 507.0)
    return f'''<svg xmlns="http://www.w3.org/2000/svg" width="1024" height="1024" viewBox="0 0 1024 1024">
  <defs>
    <linearGradient id="rim" gradientUnits="userSpaceOnUse" x1="0" y1="100" x2="0" y2="914">
      <stop offset="0" stop-color="#e2ddee"/>
      <stop offset="1" stop-color="#d7d1e5"/>
    </linearGradient>
    <linearGradient id="fill" gradientUnits="userSpaceOnUse" x1="0" y1="110" x2="0" y2="904">
      <stop offset="0" stop-color="#f5f2fb"/>
      <stop offset="1" stop-color="#e9e5f2"/>
    </linearGradient>
{defs_inner}
  </defs>
{FREEDESKTOP_SQUIRCLE_SHADOW}
  <path fill="#cfc9df" d="{SQUIRCLE_OUTER_D}"/>
  <path fill="url(#rim)" d="{SQUIRCLE_RIM_D}"/>
  <path fill="#e0dbec" d="{SQUIRCLE_INNER_BG_D}"/>
  <path fill="url(#fill)" d="{SQUIRCLE_INNER_D}"/>
  <g transform="{transform}">
{body_inner}
  </g>
</svg>
'''


def build_windows_ico(shaded_svg: str, output_path: Path) -> None:
    # Tile-free transparent emblem tuned for legibility on both light (#f3f3f3) and dark (#1c1c1e) Windows taskbars.
    win_emblem = recolor_emblem(
        shaded_svg,
        {
            "#c674f8": "#e0b3ff",
            "#9c50da": "#bc76fa",
            "#742dc5": "#9a4fed",
            "#4d0b9e": "#7c33db",
            "#4e1599": "#853ce6",
            "#1e0440": "#1a0738",
        },
    )
    win_svg = wrap_on_1024(win_emblem, target_width=948.0, center_y=512.0)
    sizes = [16, 24, 32, 48, 64, 128, 256]
    # Render crisp PNG frames at each exact target size and pack into a multi-resolution ICO.
    frames = [render_svg_to_image(win_svg, s) for s in sizes]
    output_path.parent.mkdir(parents=True, exist_ok=True)
    frames[-1].save(output_path, format="ICO", append_images=frames[:-1])


def build_small_tiled_icon(shaded_svg: str, size: int, margin_ratio: float = 0.086) -> Image.Image:
    """Optically crisp small tiled icon for 16px/32px/48px sizes with clean contrast."""
    # Boost small-size ribbon/bird contrast so the loop and birds stay legible at 16px and 32px.
    small_emblem = recolor_emblem(
        shaded_svg,
        {
            "#c674f8": "#aa4ee8",
            "#9c50da": "#7e2cc7",
            "#742dc5": "#5d19b0",
            "#4d0b9e": "#3c0682",
            "#4e1599": "#36077a",
        },
    )
    defs_inner, body_inner = extract_svg_inner(small_emblem)
    margin = 1024.0 * margin_ratio
    tile = 1024.0 - 2.0 * margin
    radius = tile * 0.224
    emblem_w = tile * (0.91 if size <= 16 else 0.89)
    transform = centered_transform(1024.0, emblem_w, 512.0)
    svg = f'''<svg xmlns="http://www.w3.org/2000/svg" width="1024" height="1024" viewBox="0 0 1024 1024">
  <defs>
    <linearGradient id="tile-bg" x1="{margin:.1f}" y1="{margin:.1f}" x2="{1024-margin:.1f}" y2="{1024-margin:.1f}" gradientUnits="userSpaceOnUse">
      <stop offset="0%" stop-color="#f9f7fe"/>
      <stop offset="100%" stop-color="#eae3f6"/>
    </linearGradient>
{defs_inner}
  </defs>
  <rect x="{margin:.1f}" y="{margin:.1f}" width="{tile:.1f}" height="{tile:.1f}" rx="{radius:.1f}" fill="url(#tile-bg)" stroke="#c7badd" stroke-width="18"/>
  <g transform="{transform}">
{body_inner}
  </g>
</svg>
'''
    return render_svg_to_image(svg, size)


def save_png(image: Image.Image, path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    image.save(path, format="PNG", optimize=True)


def generate_all(output_root: Path) -> None:
    shaded_svg = SOURCE_EMBLEM_SVG.read_text()
    mono_svg = SOURCE_MONO_SVG.read_text()
    master_app = Image.open(SOURCE_APP_ICON).convert("RGBA")

    # 1. macOS 26+ Icon Composer layers (UI/Icons/macos/AppIcon.icon/Assets/)
    appicon_assets = output_root / "UI" / "Icons" / "macos" / "AppIcon.icon" / "Assets"
    appicon_assets.mkdir(parents=True, exist_ok=True)

    mark_light = wrap_on_1024(shaded_svg, target_width=810.0, center_y=512.0)
    mark_dark = wrap_on_1024(
        recolor_emblem(
            shaded_svg,
            {
                "#c674f8": "#e2b4ff",
                "#9c50da": "#c68cff",
                "#742dc5": "#a567f5",
                "#4d0b9e": "#7d43dc",
                "#4e1599": "#d8c0ff",
                "#1e0440": "#120526",
            },
        ),
        target_width=810.0,
        center_y=512.0,
    )
    mark_mono = wrap_on_1024(mono_svg, target_width=810.0, center_y=512.0)

    (appicon_assets / "mark.svg").write_text(mark_light)
    (appicon_assets / "mark-dark.svg").write_text(mark_dark)
    (appicon_assets / "mark-mono.svg").write_text(mark_mono)

    # 2. macOS fallback .icns iconset (10 PNGs)
    iconset_dir = output_root / "UI" / "Icons" / "macos" / "app_icon.iconset"
    iconset_dir.mkdir(parents=True, exist_ok=True)
    iconset_specs = [
        ("icon_16x16.png", 16),
        ("icon_16x16@2x.png", 32),
        ("icon_32x32.png", 32),
        ("icon_32x32@2x.png", 64),
        ("icon_128x128.png", 128),
        ("icon_128x128@2x.png", 256),
        ("icon_256x256.png", 256),
        ("icon_256x256@2x.png", 512),
        ("icon_512x512.png", 512),
        ("icon_512x512@2x.png", 1024),
    ]
    for filename, px in iconset_specs:
        if px <= 32:
            img = build_small_tiled_icon(shaded_svg, px, margin_ratio=0.06)
        elif px == 1024:
            img = master_app
        else:
            img = master_app.resize((px, px), Image.Resampling.LANCZOS)
        save_png(img, iconset_dir / filename)

    # 3. Qt normal & private window icons (256x256)
    save_png(master_app.resize((256, 256), Image.Resampling.LANCZOS), output_root / "UI" / "Icons" / "ladyflock.png")
    private_svg = build_private_svg(shaded_svg)
    save_png(render_svg_to_image(private_svg, 256), output_root / "UI" / "Icons" / "ladyflock-private.png")

    # 4. Internal page favicon & Task Manager browser icon (48x48, tight margin for 16x16 tab clarity)
    save_png(build_small_tiled_icon(shaded_svg, 48, margin_ratio=0.045), output_root / "Base" / "res" / "icons" / "48x48" / "app-browser.png")

    # 5. Linux / Freedesktop scalable SVG
    freedesktop_svg_path = output_root / "Meta" / "CMake" / "freedesktop" / f"{APP_ID}.svg"
    freedesktop_svg_path.parent.mkdir(parents=True, exist_ok=True)
    freedesktop_svg_path.write_text(build_freedesktop_svg(shaded_svg))

    # 6. Windows multi-resolution tile-free ICO
    build_windows_ico(shaded_svg, output_root / "UI" / "Icons" / "windows" / "ladyflock.ico")

    print(f"Generated all Ladyflock platform icons under {output_root}")


def main() -> int:
    parser = argparse.ArgumentParser(description="Generate Ladyflock platform icons from tracked UI/Icons/ sources.")
    parser.add_argument("--output-root", type=Path, default=REPO_ROOT, help="Repository root to write generated icons into.")
    args = parser.parse_args()
    generate_all(args.output_root.resolve())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
