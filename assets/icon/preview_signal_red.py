#!/usr/bin/env python3
"""Draws one picture comparing the app icon as it is with the same mark in the Signal red.

This writes a PREVIEW and nothing else. It does not touch any release asset: generate_icons.py is
imported for its drawing functions only, with its two colour constants replaced in memory, and the
.ico, .icns, .png and mipmap files that script writes are not written here. The colours are the
shared theme's (apps/shared/theme/assets/qml/Theme.qml, palette signal, light): paper #f3f2f2, ink
#201e1d, accent #ec3013.

    python assets/icon/preview_signal_red.py [output.png]

The default output is assets/icon/proposals/signal-red-before-after.png.
"""

from __future__ import annotations

import pathlib
import sys

from PIL import Image, ImageDraw, ImageFont

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import generate_icons as gi  # noqa: E402 - found on the path set just above

REPO_ROOT = gi.REPO_ROOT
FONTS = REPO_ROOT / "apps" / "shared" / "theme" / "assets" / "fonts"

PAPER = (0xF3, 0xF2, 0xF2, 255)
INK = (0x20, 0x1E, 0x1D, 255)
SIGNAL = (0xEC, 0x30, 0x13, 255)
SURFACE = (0xEA, 0xE9, 0xE9, 255)

# (heading, caption, badge background, bars)
VARIANTS = (
    ("Now", "#0b1220 with #60a5fa", gi.BG, gi.ACCENT),
    ("Signal red on ink", "#201e1d with #ec3013", INK, SIGNAL),
    ("Signal red on paper", "#f3f2f2 with #ec3013", PAPER, SIGNAL),
)


def luminance(rgba) -> float:
    def channel(c: int) -> float:
        v = c / 255
        return v / 12.92 if v <= 0.03928 else ((v + 0.055) / 1.055) ** 2.4

    r, g, b = rgba[:3]
    return 0.2126 * channel(r) + 0.7152 * channel(g) + 0.0722 * channel(b)


def contrast(a, b) -> float:
    hi, lo = sorted((luminance(a), luminance(b)), reverse=True)
    return (hi + 0.05) / (lo + 0.05)


def badge(size: int, bg, bars) -> Image.Image:
    gi.BG, gi.ACCENT = bg, bars  # in memory only; generate_icons.main() is never called
    return gi.render_badge(size)


def font(name: str, px: int) -> ImageFont.FreeTypeFont:
    return ImageFont.truetype(str(FONTS / name), px)


def main() -> None:
    default = HERE / "proposals" / "signal-red-before-after.png"
    out = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else default
    out.parent.mkdir(parents=True, exist_ok=True)

    col_w, margin = 520, 48
    width = margin * 2 + col_w * len(VARIANTS)
    height = 800
    img = Image.new("RGBA", (width, height), PAPER)
    draw = ImageDraw.Draw(img)

    title = font("Archivo-ExtraBold.ttf", 34)
    head = font("Archivo-SemiBold.ttf", 28)
    small = font("Archivo-Regular.ttf", 20)
    draw.text((margin, 30), "App icon: the colour of the mark", font=title, fill=INK)
    draw.text(
        (margin, 78),
        "The same mark (assets/icon/generate_icons.py) in three colour pairs. "
        "The Signal red is #ec3013, the accent of all three apps.",
        font=small,
        fill=INK,
    )

    # Two bands: the icon on a light surface and on a dark one, as a task bar or dock shows it.
    # In each: 192, 48, 32 and 16 px (the .ico frames), then the 16 px frame at four times.
    band_h = 236
    bands = ((SURFACE, 170), (INK, 170 + band_h + 14))
    for x_index, (name, caption, bg, bars) in enumerate(VARIANTS):
        x0 = margin + x_index * col_w
        draw.text((x0, 124), name, font=head, fill=INK)
        for surface, top in bands:
            draw.rectangle([x0, top, x0 + col_w - 24, top + band_h], fill=surface)
            y_mid = top + band_h // 2
            x = x0 + 22
            for px in (192, 48, 32, 16):
                img.alpha_composite(badge(px, bg, bars), (x, y_mid - px // 2))
                x += px + 22
            # the 16 px frame, enlarged four times without smoothing, for the eye
            big = badge(16, bg, bars).resize((64, 64), Image.NEAREST)
            img.alpha_composite(big, (x + 6, y_mid - 32))
        foot = bands[1][1] + band_h + 18
        draw.text((x0, foot), caption, font=small, fill=INK)
        ratio = contrast(bars, bg)
        draw.text(
            (x0, foot + 28), f"mark against its background {ratio:.2f}:1", font=small, fill=INK
        )

    img.convert("RGB").save(out)
    print(f"wrote {out}")
    for name, _caption, bg, bars in VARIANTS:
        print(f"{name}: {contrast(bars, bg):.2f}:1")


if __name__ == "__main__":
    main()
