#!/usr/bin/env python3
#
# Offline bench for the stereo warp. Reads a frame capture dumped by the
# renderer and reproduces the warp here, so shader changes can be tried and
# measured on real frames without a build and install cycle.
#
# Capturing on device, while a VR stream is running:
#   adb shell setprop debug.moonlight.capture 1
#   adb shell ls /sdcard/Android/data/com.limelight.debug/files/
#   adb pull /sdcard/Android/data/com.limelight.debug/files/ captures/
#
# Then:
#   ./venv/bin/python tools/warp_lab.py captures/ --tag 1
#
# Files in a capture set, all headerless:
#   cap_<tag>_source.raw      W*H*4 uint8 RGBA, the unwarped frame
#   cap_<tag>_left.raw        W*H*4 uint8 RGBA, what the left eye was shown
#   cap_<tag>_right.raw       W*H*4 uint8 RGBA, likewise
#   cap_<tag>_depthtex.raw    256*256 uint8, the depth texture the warp read
#   cap_<tag>_depthraw.raw    256*256 float32, raw model output before
#                             normalization
#   cap_<tag>_modelinput.raw  256*256*3 float32, the RGB the model was given
#
# Orientation: source, left, right and depthtex all come back from glReadPixels
# bottom row first, and agree with each other, so the warp runs on them as they
# are. depthraw and modelinput are top row first, the way the model sees them.
# Anything written out as a PNG is flipped to top first.

import argparse
import os
import sys

import numpy as np
from PIL import Image

DEPTH_SIZE = 256


def load_raw(directory, tag, what, dtype, count):
    path = os.path.join(directory, "cap_%s_%s.raw" % (tag, what))
    if not os.path.exists(path):
        return None
    data = np.fromfile(path, dtype=dtype)
    if count is not None and data.size != count:
        raise SystemExit("%s has %d values, expected %d" % (path, data.size, count))
    return data


def sample_bilinear(image, xs, ys):
    """Matches GL_LINEAR with CLAMP_TO_EDGE. xs and ys are normalized."""
    h, w = image.shape[:2]
    fx = xs * w - 0.5
    fy = ys * h - 0.5
    x0 = np.floor(fx)
    y0 = np.floor(fy)
    tx = (fx - x0)[..., None] if image.ndim == 3 else fx - x0
    ty = (fy - y0)[..., None] if image.ndim == 3 else fy - y0

    x0 = x0.astype(np.int64)
    y0 = y0.astype(np.int64)
    x1 = np.clip(x0 + 1, 0, w - 1)
    y1 = np.clip(y0 + 1, 0, h - 1)
    x0 = np.clip(x0, 0, w - 1)
    y0 = np.clip(y0, 0, h - 1)

    top = image[y0, x0] * (1.0 - tx) + image[y0, x1] * tx
    bot = image[y1, x0] * (1.0 - tx) + image[y1, x1] * tx
    return top * (1.0 - ty) + bot * ty


def warp(source, depth, disparity, convergence=0.5):
    """The current shader: one gather step using depth at the destination."""
    h, w = source.shape[:2]
    xs = (np.arange(w) + 0.5) / w
    ys = (np.arange(h) + 0.5) / h
    gx, gy = np.meshgrid(xs, ys)

    d = sample_bilinear(depth, gx, gy)
    tc = gx - disparity * (d - convergence)
    return sample_bilinear(source, tc, gy), d


def save_png(path, array, flip=True):
    a = np.clip(array, 0.0, 255.0).astype(np.uint8)
    if flip:
        a = a[::-1]
    Image.fromarray(a).save(path)
    print("wrote %s" % path)


def edge_alignment(depth_full, luma, threshold=0.04, search=48):
    """Mean horizontal distance from a depth edge to the nearest colour edge.

    This is the halo, measured. A depth boundary that sits 15 px away from the
    colour boundary it belongs to drags the wrong pixels across the edge.
    """
    dd = np.abs(np.diff(depth_full, axis=1))
    dl = np.abs(np.diff(luma, axis=1))
    depth_edges = dd > threshold
    colour_edges = dl > (0.10 * 255.0)

    distances = []
    rows = np.unique(np.linspace(0, depth_full.shape[0] - 1, 240).astype(int))
    for y in rows:
        de = np.flatnonzero(depth_edges[y])
        ce = np.flatnonzero(colour_edges[y])
        if de.size == 0 or ce.size == 0:
            continue
        idx = np.searchsorted(ce, de)
        left = ce[np.clip(idx - 1, 0, ce.size - 1)]
        right = ce[np.clip(idx, 0, ce.size - 1)]
        nearest = np.minimum(np.abs(de - left), np.abs(de - right))
        distances.append(nearest[nearest <= search])
    if not distances:
        return None
    return np.concatenate(distances)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("directory")
    ap.add_argument("--tag", default="1")
    ap.add_argument("--width", type=int, default=3840)
    ap.add_argument("--height", type=int, default=2160)
    ap.add_argument("--separation", type=float, default=0.015,
                    help="frame widths, the seekbar value over 1000")
    ap.add_argument("--out", default=None, help="where to write PNGs")
    args = ap.parse_args()

    w, h = args.width, args.height
    out = args.out or os.path.join(args.directory, "out")
    os.makedirs(out, exist_ok=True)

    source = load_raw(args.directory, args.tag, "source", np.uint8, w * h * 4)
    if source is None:
        raise SystemExit("no capture tagged %s in %s" % (args.tag, args.directory))
    source = source.reshape(h, w, 4)[:, :, :3].astype(np.float32)

    depth_u8 = load_raw(args.directory, args.tag, "depthtex", np.uint8,
                        DEPTH_SIZE * DEPTH_SIZE)
    depth = depth_u8.reshape(DEPTH_SIZE, DEPTH_SIZE).astype(np.float32) / 255.0

    print("source %dx%d, depth %dx%d" % (w, h, DEPTH_SIZE, DEPTH_SIZE))
    print("depth texture percentiles: 2%% %.3f  25%% %.3f  50%% %.3f  75%% %.3f  98%% %.3f"
          % tuple(np.percentile(depth, [2, 25, 50, 75, 98])))
    print("interquartile span %.3f of the 0..1 range"
          % (np.percentile(depth, 75) - np.percentile(depth, 25)))

    raw = load_raw(args.directory, args.tag, "depthraw", np.float32,
                   DEPTH_SIZE * DEPTH_SIZE)
    if raw is not None:
        print("raw model output: min %.3f max %.3f mean %.3f"
              % (raw.min(), raw.max(), raw.mean()))

    # Reproduce what the device drew, and check we agree with it
    for name, sign in (("left", 1.0), ("right", -1.0)):
        device = load_raw(args.directory, args.tag, name, np.uint8, w * h * 4)
        mine, d_full = warp(source, depth, sign * args.separation)
        save_png(os.path.join(out, "%s_%s_mine.png" % (args.tag, name)), mine)
        if device is None:
            continue
        device = device.reshape(h, w, 4)[:, :, :3].astype(np.float32)
        diff = np.abs(device - mine)
        print("%-5s vs device: mean |diff| %.2f, 99th pct %.0f, max %.0f (of 255)"
              % (name, diff.mean(), np.percentile(diff, 99), diff.max()))

    _, d_full = warp(source, depth, 0.0)
    save_png(os.path.join(out, "%s_source.png" % args.tag), source)
    save_png(os.path.join(out, "%s_depth.png" % args.tag), d_full * 255.0)

    luma = source @ np.array([0.299, 0.587, 0.114], dtype=np.float32)
    dist = edge_alignment(d_full, luma)
    if dist is not None and dist.size:
        print("depth edge to colour edge: median %.1f px, 90th pct %.1f px, n=%d"
              % (np.median(dist), np.percentile(dist, 90), dist.size))
    print("one depth texel covers %.1f x %.1f output pixels"
          % (w / DEPTH_SIZE, h / DEPTH_SIZE))


if __name__ == "__main__":
    sys.exit(main())
