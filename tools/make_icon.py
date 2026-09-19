"""Generate src/StockTool.ico: a rising price line on a rounded blue tile.

Each size is rendered at 4x and downsampled so edges stay crisp. Run from the
repo root:  python tools/make_icon.py
"""

from __future__ import annotations

from pathlib import Path

from PIL import Image, ImageDraw

SIZES: tuple[int, ...] = (16, 24, 32, 48, 64, 128, 256)
SUPERSAMPLE: int = 4
MAX_SIZE: int = 256

BG_TOP: tuple[int, int, int, int] = (26, 115, 232, 255)
BG_BOTTOM: tuple[int, int, int, int] = (13, 71, 161, 255)
LINE: tuple[int, int, int, int] = (255, 255, 255, 255)
BARS: tuple[int, int, int, int] = (255, 255, 255, 70)
DOT: tuple[int, int, int, int] = (52, 211, 120, 255)

# Chart polyline in unit coordinates (0..1), drawn left to right.
POINTS: tuple[tuple[float, float], ...] = (
    (0.14, 0.72), (0.30, 0.56), (0.42, 0.64), (0.58, 0.40), (0.70, 0.48), (0.86, 0.24),
)
BAR_HEIGHTS: tuple[float, ...] = (0.10, 0.16, 0.12, 0.20, 0.14, 0.22)


def _gradient_tile(px: int, radius: int) -> Image.Image:
    """Rounded square with a vertical gradient."""
    assert px > 0 and radius >= 0
    tile = Image.new("RGBA", (px, px), (0, 0, 0, 0))
    grad = Image.new("RGBA", (px, px), BG_TOP)
    gdraw = ImageDraw.Draw(grad)
    for y in range(px):
        t = y / max(px - 1, 1)
        colour = tuple(int(a + (b - a) * t) for a, b in zip(BG_TOP, BG_BOTTOM))
        gdraw.line([(0, y), (px, y)], fill=colour)
    mask = Image.new("L", (px, px), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, px - 1, px - 1], radius=radius, fill=255)
    tile.paste(grad, (0, 0), mask)
    assert tile.size == (px, px)
    return tile


def render(size: int) -> Image.Image:
    """Render one icon frame at `size` pixels."""
    assert 0 < size <= MAX_SIZE
    px = size * SUPERSAMPLE
    img = _gradient_tile(px, radius=px // 5)
    draw = ImageDraw.Draw(img)

    # Faint volume bars along the bottom (skipped at tiny sizes: just noise).
    if size >= 32:
        n = len(BAR_HEIGHTS)
        slot = px * 0.72 / n
        bar_w = slot * 0.55
        for i, h in enumerate(BAR_HEIGHTS):
            x0 = px * 0.14 + i * slot + (slot - bar_w) / 2
            y0 = px * 0.88 - h * px
            draw.rectangle([x0, y0, x0 + bar_w, px * 0.88], fill=BARS)

    # Rising price line.
    width = max(px // 12, 1)
    pts = [(x * px, y * px) for x, y in POINTS]
    draw.line(pts, fill=LINE, width=width, joint="curve")
    for x, y in (pts[0], pts[-1]):  # round the ends
        draw.ellipse([x - width / 2, y - width / 2, x + width / 2, y + width / 2], fill=LINE)

    # Green marker on the latest point.
    r = width * 0.95
    ex, ey = pts[-1]
    draw.ellipse([ex - r, ey - r, ex + r, ey + r], fill=DOT)

    out = img.resize((size, size), Image.Resampling.LANCZOS)
    assert out.size == (size, size)
    return out


def main() -> int:
    root = Path(__file__).resolve().parent.parent
    target = root / "src" / "StockTool.ico"
    frames = [render(s) for s in SIZES]
    assert len(frames) == len(SIZES)
    frames[-1].save(target, format="ICO", sizes=[(s, s) for s in SIZES], append_images=frames[:-1])
    assert target.exists() and target.stat().st_size > 0
    print(f"wrote {target} ({target.stat().st_size} bytes, sizes {SIZES})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
