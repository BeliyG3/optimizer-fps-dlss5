"""Tone-mapped before | after preview of hdri_prepare, with the found sun disc circled on `before`."""
import numpy as np
from PIL import Image, ImageDraw


def _tonemap(img, key=0.18):
    """Exposure so the median luminance maps to `key`, then Reinhard and gamma 2.2 (so both halves are comparable)."""
    lum = img @ np.array([0.2126, 0.7152, 0.0722], np.float32)
    x = img * (key / max(float(np.median(lum)), 1e-6))
    x = x / (1 + x)
    return Image.fromarray((np.clip(x, 0, 1) ** (1 / 2.2) * 255 + 0.5).astype(np.uint8))


def write_preview(path, before, after, disc, report, width=1024):
    """before | after stacked one above the other (an equirect is 2:1, so side by side would be unreadable)."""
    row, col, radius = disc
    scale = width / before.shape[1]
    a = _tonemap(before).resize((width, int(before.shape[0] * scale)), Image.LANCZOS)
    b = _tonemap(after).resize(a.size, Image.LANCZOS)
    draw = ImageDraw.Draw(a)
    r = max(6.0, 4 * radius * scale)
    cx, cy = col * scale, row * scale
    for dx in (-width, 0, width):  # wrap around the seam
        draw.ellipse([cx + dx - r, cy - r, cx + dx + r, cy + r], outline=(255, 40, 40), width=2)
    label = ["before (sun circled, tone mapped)", "after (sun removed, rotated %.1f deg, x%.4g)" % (report["rotation_deg"], report["scale"])]
    canvas = Image.new("RGB", (a.width, a.height * 2 + 2), (0, 0, 0))
    canvas.paste(a, (0, 0)); canvas.paste(b, (0, a.height + 2))
    d = ImageDraw.Draw(canvas)
    for y, text in ((4, label[0]), (a.height + 6, label[1])):
        d.rectangle([2, y - 2, 8 + 7 * len(text), y + 12], fill=(0, 0, 0)); d.text((6, y), text, fill=(255, 255, 255))
    canvas.save(path)
