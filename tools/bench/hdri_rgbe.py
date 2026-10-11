"""Radiance RGBE (.hdr) reader and writer on numpy: header `-Y H +X W`, flat or new-style RLE scanlines in, flat out.

Decoding mirrors tools/bench12/image.cpp LoadHdr (texel value = (mantissa + 0.5) * 2^(e-136), 0 where e == 0) so the
prepared panorama is read back by the bench exactly as the numbers here describe it.
"""
import numpy as np


def _read_header(data):
    pos = 0
    def line():
        nonlocal pos
        end = data.find(b"\n", pos)
        end = len(data) if end < 0 else end
        text = data[pos:end].rstrip(b"\r"); pos = end + 1
        return text.decode("latin-1")
    if not line().startswith("#?"):
        raise ValueError("not a Radiance HDR")
    while line() != "":
        if pos >= len(data):
            raise ValueError("truncated HDR header")
    parts = line().split()
    if len(parts) != 4 or parts[0] != "-Y" or parts[2] != "+X":
        raise ValueError("requires -Y H +X W")
    return int(parts[1]), int(parts[3]), pos


def _decode_rle_scanline(data, pos, w):
    scan = np.empty((4, w), np.uint8)
    for c in range(4):
        x = 0
        while x < w:
            count = data[pos]; pos += 1
            if count > 128:
                count -= 128
                if x + count > w:
                    raise ValueError("bad RLE run")
                scan[c, x:x + count] = data[pos]; pos += 1
            else:
                if count == 0 or x + count > w:
                    raise ValueError("bad RLE literal")
                scan[c, x:x + count] = np.frombuffer(data, np.uint8, count, pos); pos += count
            x += count
    return scan.T, pos


def read_hdr(path):
    """Returns a float32 array (H, W, 3)."""
    with open(path, "rb") as f:
        data = f.read()
    h, w, pos = _read_header(data)
    rgbe = np.empty((h, w, 4), np.uint8)
    for y in range(h):
        if data[pos] == 2 and data[pos + 1] == 2 and ((data[pos + 2] << 8) | data[pos + 3]) == w and 8 <= w < 0x8000:
            rgbe[y], pos = _decode_rle_scanline(data, pos + 4, w)
        else:
            rgbe[y] = np.frombuffer(data, np.uint8, w * 4, pos).reshape(w, 4); pos += w * 4
    e = rgbe[..., 3].astype(np.int32)
    scale = np.where(e > 0, np.ldexp(np.float32(1.0 / 256), e - 128), 0).astype(np.float32)
    return (rgbe[..., :3].astype(np.float32) + 0.5) * scale[..., None]


def write_hdr(path, img):
    """Flat RGBE, standard encoding. img is float (H, W, 3)."""
    img = np.maximum(np.asarray(img, np.float32), 0)
    h, w = img.shape[:2]
    peak = img.max(axis=2)
    mant, exp = np.frexp(peak)  # peak = mant * 2^exp, mant in [0.5, 1)
    exp = np.where(peak > 1e-32, exp, 0)
    div = np.ldexp(np.float32(1.0), -exp) * 256  # value / 2^exp * 256 -> mantissa
    rgbe = np.zeros((h, w, 4), np.uint8)
    rgbe[..., :3] = np.clip(np.floor(img * div[..., None]), 0, 255)
    rgbe[..., 3] = np.where(peak > 1e-32, exp + 128, 0)
    with open(path, "wb") as f:
        f.write(f"#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y {h} +X {w}\n".encode("ascii"))
        f.write(rgbe.tobytes())
