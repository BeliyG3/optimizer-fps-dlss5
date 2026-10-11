"""Readers for the foliage diagnosis' raw fields.

OFMV (bench12 --dump-raw motion, raw_dump.h): {char magic[4]="OFMV"; u32 width, height, frame} then width*height
float32 (x, y), current -> previous (or t-K with --motion-span K), render pixels, +x right, +y down, rows from the top.
OFDP (bench12 --dump-raw depth): the same header with "OFDP", then width*height float32, the depth guide at render size in
the bench's convention (standard z/w by default; ghost_metrics.view_depth turns it into metres).
OFPD (bench12 --dump-raw prevdepth): the same, "OFPD": the view depth in metres each pixel's surface point had in the
camera of frame t - span (motion/README.md), 0 on the sky.
OFAC (core DebugTemporalDumpAcc, core/frame/acc_dump.h): {char magic[4]="OFAC"; u32 version, width, height, eval,
fullPassIndex; f32 mvScaleX, mvScaleY} then width*height float32 pairs model motion, then the same for the chain;
motion-texture pixels, current -> residual frame; the "none" marker (4*width, 0) is kept. Version 2 adds width*height
bytes of the marker's reason (PwTMarkerReason: 0 kept, 1 off screen, 2 depth test, 3 colour gate, 4 non-finite). Version 3
(DebugTemporalCarryChainRule above 0) then adds width*height float32 pairs of the chain the carried frames reproject along
(the carry chain; the main chain on a full pass).
"""
import struct

import numpy as np

REASONS = {0: "keep", 1: "off screen", 2: "depth", 3: "colour", 4: "non-finite"}


def _payload(path, header, magic, pairs, extra=lambda data: 0):
    data = open(path, "rb").read()
    if data[:4] != magic:
        raise ValueError(f"{path}: magic {data[:4]!r}, expected {magic!r}")
    w, h = struct.unpack_from("<II", data, 8 if magic == b"OFAC" else 4)
    need = header + pairs * w * h * 2 * 4 + extra(data) * w * h
    if len(data) != need:
        raise ValueError(f"{path}: {len(data)} bytes, expected {need} for {w}x{h}")
    return data, w, h


def read_ofmv(path):
    """-> (frame, float32[h, w, 2])"""
    data, w, h = _payload(path, 16, b"OFMV", 1)
    frame = struct.unpack_from("<I", data, 12)[0]
    return frame, np.frombuffer(data, np.float32, w * h * 2, 16).reshape(h, w, 2)


def read_ofdp(path, magic=b"OFDP"):
    """-> (frame, float32[h, w])"""
    data, w, h = _payload(path, 16, magic, 0, lambda data: 4)
    frame = struct.unpack_from("<I", data, 12)[0]
    return frame, np.frombuffer(data, np.float32, w * h, 16).reshape(h, w)


def read_ofpd(path):
    """prevdepth_<frame>.f32 -> (frame, float32[h, w]): view depth in metres in the span-back camera, 0 on the sky."""
    return read_ofdp(path, b"OFPD")


def read_ofac(path, with_carry=False):
    """-> (header dict, model float32[h, w, 2], chain float32[h, w, 2], reason uint8[h, w] or None for v1);
    with_carry: one more, the carry plane float32[h, w, 2] (None before v3)."""
    version = lambda data: struct.unpack_from("<I", data, 4)[0]
    data, w, h = _payload(path, 32, b"OFAC", 2, lambda d: {2: 1, 3: 9}.get(version(d), 0))
    v, _, _, ev, full, sx, sy = struct.unpack_from("<IIIIIff", data, 4)
    if v not in (1, 2, 3):
        raise ValueError(f"{path}: OFAC version {v}, expected 1, 2 or 3")
    n = w * h * 2
    model = np.frombuffer(data, np.float32, n, 32).reshape(h, w, 2)
    chain = np.frombuffer(data, np.float32, n, 32 + n * 4).reshape(h, w, 2)
    reason = np.frombuffer(data, np.uint8, w * h, 32 + n * 8).reshape(h, w) if v >= 2 else None
    head = dict(version=v, width=w, height=h, eval=ev, fullPassIndex=full, mvScaleX=sx, mvScaleY=sy)
    if not with_carry:
        return head, model, chain, reason
    carry = np.frombuffer(data, np.float32, n, 32 + n * 8 + w * h).reshape(h, w, 2) if v == 3 else None
    return head, model, chain, reason, carry
