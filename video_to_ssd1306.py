#!/usr/bin/env python3
"""
Video ➜ SSD1306 *single-file movie* converter with Floyd–Steinberg dithering

- Downscales with letterboxing to target (default 128x64)
- True Floyd–Steinberg dithering (serpentine scan)
- Packs bits in SSD1306 page format (8px vertical per byte, bit0 = top)
- Writes **one `/movie.bin`** with a tiny binary header so your ESP32-S3 can FPS-sync
- Optional `--raw` to omit header (legacy mode: just frames back-to-back)
- Optional previews to eyeball dithering

Header layout (little-endian):
    0  : 8 bytes  magic = b"SSD1306V1"
    8  : u16      width
   10  : u16      height  (must be multiple of 8)
   12  : u32      fps_milli  (fps * 1000, e.g. 15000 for 15 fps)
   16  : u32      frame_count
   20  : u8       flags  (bit0 = inverted)
   21  : 11 bytes reserved (zeros)  → total header = 32 bytes
   32  : frame0 (frame_size bytes), frame1, ...

Usage examples:
    python video_to_ssd1306.py komiop.mp4 movie.bin --width 128 --height 64 --fps 15 --preview
    python video_to_ssd1306.py komiop.mp4 movie.bin --raw   # write frames only (no header)

Requirements:
    pip install opencv-python numpy Pillow
"""

import argparse
import struct
import sys
from pathlib import Path

import cv2
import numpy as np
from PIL import Image

MAGIC = b"SSD1306V1"
HEADER_SIZE = 32


def letterbox_resize(frame: np.ndarray, target_w: int, target_h: int, bg=0) -> np.ndarray:
    h, w = frame.shape[:2]
    gray = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
    scale = min(target_w / w, target_h / h)
    nw, nh = int(round(w * scale)), int(round(h * scale))
    resized = cv2.resize(gray, (nw, nh), interpolation=cv2.INTER_AREA)
    canvas = np.full((target_h, target_w), bg, dtype=np.uint8)
    x0 = (target_w - nw) // 2
    y0 = (target_h - nh) // 2
    canvas[y0:y0+nh, x0:x0+nw] = resized
    return canvas


def floyd_steinberg_dither(img: np.ndarray, serpentine: bool = True) -> np.ndarray:
    h, w = img.shape
    f = img.astype(np.float32) / 255.0
    out = np.zeros_like(f)
    for y in range(h):
        if serpentine and (y % 2 == 1):
            xs = range(w - 1, -1, -1)
            dir_sign = -1
        else:
            xs = range(w)
            dir_sign = 1
        for x in xs:
            old = f[y, x]
            new = 1.0 if old >= 0.5 else 0.0
            out[y, x] = new
            err = old - new
            xn = x + dir_sign
            if 0 <= xn < w:
                f[y, xn] += err * (7/16)
            y1 = y + 1
            if y1 < h:
                xdl = x - dir_sign
                if 0 <= xdl < w:
                    f[y1, xdl] += err * (3/16)
                f[y1, x] += err * (5/16)
                xdr = x + dir_sign
                if 0 <= xdr < w:
                    f[y1, xdr] += err * (1/16)
    return out >= 0.5


def pack_ssd1306_pages(bits: np.ndarray) -> bytes:
    h, w = bits.shape
    if h % 8 != 0:
        raise ValueError("Height must be multiple of 8 for SSD1306.")
    pages = h // 8
    out = bytearray(w * pages)
    idx = 0
    for page in range(pages):
        y0 = page * 8
        for x in range(w):
            byte = 0
            for b in range(8):
                if bits[y0 + b, x]:
                    byte |= (1 << b)
            out[idx] = byte
            idx += 1
    return bytes(out)


def write_header(f, width: int, height: int, fps: float, frame_count: int, inverted: bool):
    fps_milli = int(round(fps * 1000))
    flags = 1 if inverted else 0
    reserved = bytes(11)
    header = struct.pack(
        '<8sHHIIB11s',
        MAGIC, width, height, fps_milli, frame_count, flags, reserved
    )
    assert len(header) == HEADER_SIZE
    f.write(header)


def main():
    ap = argparse.ArgumentParser(description='Convert a video to SSD1306 movie.bin (Floyd–Steinberg dither).')
    ap.add_argument('video', help='Input video path')
    ap.add_argument('outfile', help='Output .bin path (with header unless --raw)')
    ap.add_argument('--width', type=int, default=128)
    ap.add_argument('--height', type=int, default=64)
    ap.add_argument('--fps', type=float, default=None, help='Target FPS (defaults to source fps)')
    ap.add_argument('--invert', action='store_true', help='Invert pixels after dithering')
    ap.add_argument('--no-serpentine', action='store_true', help='Disable serpentine scan')
    ap.add_argument('--preview', action='store_true', help='Write preview PNGs (preview_00000.png, …)')
    ap.add_argument('--raw', action='store_true', help='Write frames only (no header)')

    args = ap.parse_args()

    if args.height % 8 != 0:
        print('Error: --height must be a multiple of 8 for SSD1306.', file=sys.stderr)
        sys.exit(1)

    cap = cv2.VideoCapture(args.video)
    if not cap.isOpened():
        print('Error: cannot open video', file=sys.stderr)
        sys.exit(1)

    src_fps = cap.get(cv2.CAP_PROP_FPS)
    if not src_fps or src_fps <= 0:
        src_fps = 30.0
    target_fps = args.fps if args.fps and args.fps > 0 else src_fps
    frame_interval = int(round(src_fps / target_fps)) if target_fps < src_fps else 1

    serpentine = not args.no_serpentine

    out_path = Path(args.outfile)
    out_f = open(out_path, 'wb')

    # Optionally reserve space for header; fill later when we know frame_count
    if not args.raw:
        out_f.seek(HEADER_SIZE)

    frame_idx = 0
    kept_idx = 0

    while True:
        ret, frame = cap.read()
        if not ret:
            break
        if frame_idx % frame_interval != 0:
            frame_idx += 1
            continue

        gray = letterbox_resize(frame, args.width, args.height, bg=0)
        bits = floyd_steinberg_dither(gray, serpentine=serpentine)
        if args.invert:
            bits = np.logical_not(bits)
        packed = pack_ssd1306_pages(bits)

        out_f.write(packed)

        if args.preview:
            img = (bits.astype(np.uint8) * 255)
            Image.fromarray(img, mode='L').save(f'preview_{kept_idx:05d}.png')

        kept_idx += 1
        frame_idx += 1

    cap.release()

    if not args.raw:
        # Write header now that we know frame_count
        out_f.flush()
        out_f.seek(0)
        write_header(out_f, args.width, args.height, target_fps, kept_idx, args.invert)

    out_f.close()

    frame_size = args.width * (args.height // 8)
    print(f"Done. {kept_idx} frame(s) • frame_size={frame_size} bytes • output={out_path}")


if __name__ == '__main__':
    main()
