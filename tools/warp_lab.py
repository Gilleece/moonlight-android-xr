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
#   cap_<tag>_guidetex.raw    256*256*4 uint8, guide colour in rgb, depth in a
#   cap_<tag>_upsampled.raw   (W/4)*(H/4) uint8, the upsampled depth
#   cap_<tag>_depthraw.raw    256*256 float32, raw model output before
#                             normalization
#   cap_<tag>_modelinput.raw  256*256*3 float32, the RGB the model was given
#
# Orientation: source, left, right, depthtex and upsampled all come back from
# glReadPixels bottom row first, and agree with each other, so the warp runs on
# them as they are. depthraw and modelinput are top row first, the way the
# model sees them. Anything written out as a PNG is flipped to top first.
#
# This reproduces the shipped shader: joint bilateral upsample of the depth
# guided by colour, then an occlusion aware gather that inverts the forward
# map. Checked against a device capture: the upsample matches to a mean of
# 0.001, which is the 8 bit quantisation floor, and the warped eyes to a mean
# of 1.0 of 255 over the whole frame.
#
# That 99th percentile of about 18 is expected and not a fidelity problem. At a
# fold the search picks between competing crossings, so a tiny numeric
# difference flips which one wins and moves that pixel by the whole disparity.
# The error is concentrated at fold boundaries; everywhere else it is under a
# quantisation step. Judge changes on the stretch statistics and the images,
# not on this number moving by a few tenths.

import argparse
import os
import sys

import numpy as np
from PIL import Image

DEPTH_SIZE = 256
SIGMA_S = 1.5
CHUNK_ROWS = 108


def load_raw(directory, tag, what, dtype, count):
    path = os.path.join(directory, "cap_%s_%s.raw" % (tag, what))
    if not os.path.exists(path):
        return None
    data = np.fromfile(path, dtype=dtype)
    if count is not None and data.size != count:
        raise SystemExit("%s has %d values, expected %d" % (path, data.size, count))
    return data


def save_png(path, array, flip=True):
    a = np.clip(array, 0.0, 255.0).astype(np.uint8)
    if flip:
        a = a[::-1]
    Image.fromarray(a).save(path)
    print("wrote %s" % path)


def jbu_upsample(source, guide_rgb, depth_tex, uw, uh, sigma_r, sharp=0.0):
    """The upsample pass. Depth is snapped onto colour edges, and optionally
    pushed to whichever side of the local range it is nearer."""
    h, w = source.shape[:2]
    out = np.zeros((uh, uw), np.float32)
    vx = (np.arange(uw) + 0.5) / uw
    vy = (np.arange(uh) + 0.5) / uh
    sx = np.clip((vx * w).astype(int), 0, w - 1)
    sy = np.clip((vy * h).astype(int), 0, h - 1)
    lp_x = vx * DEPTH_SIZE - 0.5
    lp_y = vy * DEPTH_SIZE - 0.5
    bx = np.floor(lp_x).astype(int)
    by = np.floor(lp_y).astype(int)

    for y in range(uh):
        hi = source[sy[y], sx] / 255.0
        num = np.zeros(uw, np.float32)
        den = np.zeros(uw, np.float32)
        lo_d = np.full(uw, 1.0, np.float32)
        hi_d = np.zeros(uw, np.float32)
        for dy in range(-2, 3):
            qy = np.clip(by[y] + dy, 0, DEPTH_SIZE - 1)
            for dx in range(-2, 3):
                qx = np.clip(bx + dx, 0, DEPTH_SIZE - 1)
                sa = depth_tex[qy, qx]
                srgb = guide_rgb[qy, qx]
                offx = qx - lp_x
                offy = float(qy) - lp_y[y]
                ws = np.exp(-(offx * offx + offy * offy) / (2 * SIGMA_S ** 2))
                cd = hi - srgb
                wr = np.exp(-(cd * cd).sum(axis=1) / (2 * sigma_r ** 2))
                wgt = ws * wr
                num += wgt * sa
                den += wgt
                lo_d = np.minimum(lo_d, sa)
                hi_d = np.maximum(hi_d, sa)
        d = num / np.maximum(den, 1e-6)
        if sharp > 0.0:
            span = hi_d - lo_d
            u = np.clip((d - lo_d) / np.maximum(span, 1e-6), 0.0, 1.0)
            snapped = lo_d + span / (1.0 + np.exp(-24.0 * (u - 0.5)))
            d = np.where(span < 0.05, d, d + sharp * (snapped - d))
        out[y] = d
    return out


def expand(depth_small, w, h):
    """Bilinear expand to full resolution, matching the sampler."""
    uh, uw = depth_small.shape
    ys = (np.arange(h) + 0.5) / h * uh - 0.5
    xs = (np.arange(w) + 0.5) / w * uw - 0.5
    y0 = np.clip(np.floor(ys).astype(int), 0, uh - 1)
    y1 = np.clip(y0 + 1, 0, uh - 1)
    x0 = np.clip(np.floor(xs).astype(int), 0, uw - 1)
    x1 = np.clip(x0 + 1, 0, uw - 1)
    ty = (ys - np.floor(ys))[:, None]
    tx = (xs - np.floor(xs))[None, :]
    top = depth_small[np.ix_(y0, x0)] * (1 - tx) + depth_small[np.ix_(y0, x1)] * tx
    bot = depth_small[np.ix_(y1, x0)] * (1 - tx) + depth_small[np.ix_(y1, x1)] * tx
    return top * (1 - ty) + bot * ty


def warp_rows(source, depth, y0, y1, sign, disp, convergence, span=20):
    """Occlusion aware gather. A source pixel at offset t lands here with error
    e(t) = t + disp * (d(x + t) - convergence), so every zero crossing is a
    source that genuinely lands on this pixel and the nearest surface wins."""
    h, w = source.shape[:2]
    xs = np.arange(w)
    best_t = np.zeros((y1 - y0, w), np.float32)
    best_d = np.full((y1 - y0, w), -1.0, np.float32)
    prev_e = prev_t = None
    for t in np.arange(-span, span + 1, dtype=np.float32):
        sx = np.clip(xs + t, 0, w - 1).astype(int)
        d = depth[y0:y1][:, sx]
        e = t + sign * disp * (d - convergence)
        if prev_e is not None:
            crossed = (np.sign(e) != np.sign(prev_e)) & (np.abs(e - prev_e) > 1e-6)
            frac = np.where(crossed, prev_e / (prev_e - e + 1e-9), 0.0)
            take = crossed & (d > best_d)
            best_t = np.where(take, prev_t + frac, best_t)
            best_d = np.where(take, d, best_d)
        prev_e, prev_t = e, t

    gx = np.clip(xs[None, :] + best_t, 0, w - 1)
    g0 = np.floor(gx).astype(int)
    g1 = np.clip(g0 + 1, 0, w - 1)
    fx = (gx - g0)[..., None]
    rows = np.arange(y0, y1)[:, None]
    return source[rows, g0] * (1 - fx) + source[rows, g1] * fx, best_t


def warp_frame(source, depth, sign, disp, convergence):
    h, w = source.shape[:2]
    out = np.zeros((h, w, 3), np.float32)
    s15 = s50 = total = 0
    for y0 in range(0, h, CHUNK_ROWS):
        y1 = min(h, y0 + CHUNK_ROWS)
        rows, best_t = warp_rows(source, depth, y0, y1, sign, disp, convergence)
        out[y0:y1] = rows
        # Local stretch is where the disocclusion shows. Peak matters more
        # than area: a narrow hard stretch reads worse than a wide soft one,
        # which is what killed the depth sharpening experiment.
        st = 1.0 + np.diff(best_t, axis=1)
        s15 += int((st > 1.15).sum())
        s50 += int((st > 1.5).sum())
        total += st.size
    return out, 100.0 * s15 / total, 100.0 * s50 / total


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("directory")
    ap.add_argument("--tag", default="1")
    ap.add_argument("--width", type=int, default=3840)
    ap.add_argument("--height", type=int, default=2160)
    ap.add_argument("--separation", type=float, default=0.005,
                    help="frame widths, the seekbar value over 1000")
    ap.add_argument("--convergence", type=float, default=0.5)
    ap.add_argument("--sigma", type=float, default=0.25, help="upsample range sigma")
    ap.add_argument("--sharp", type=float, default=0.0,
                    help="depth boundary sharpening, 0 to 1")
    ap.add_argument("--out", default=None, help="where to write PNGs")
    args = ap.parse_args()

    w, h = args.width, args.height
    uw, uh = w // 4, h // 4
    out = args.out or os.path.join(args.directory, "out")
    os.makedirs(out, exist_ok=True)

    source = load_raw(args.directory, args.tag, "source", np.uint8, w * h * 4)
    if source is None:
        raise SystemExit("no capture tagged %s in %s" % (args.tag, args.directory))
    source = source.reshape(h, w, 4)[:, :, :3].astype(np.float32)

    guide = load_raw(args.directory, args.tag, "guidetex", np.uint8,
                     DEPTH_SIZE * DEPTH_SIZE * 4)
    if guide is None:
        raise SystemExit("capture has no guidetex, it predates the upsample pass")
    guide = guide.reshape(DEPTH_SIZE, DEPTH_SIZE, 4).astype(np.float32) / 255.0
    depth_tex = guide[:, :, 3]

    print("source %dx%d, depth %dx%d, upsampled %dx%d" % (w, h, DEPTH_SIZE, DEPTH_SIZE, uw, uh))
    print("depth percentiles: 2%% %.3f  25%% %.3f  50%% %.3f  75%% %.3f  98%% %.3f"
          % tuple(np.percentile(depth_tex, [2, 25, 50, 75, 98])))
    print("interquartile span %.3f of the 0..1 range"
          % (np.percentile(depth_tex, 75) - np.percentile(depth_tex, 25)))

    upsampled = jbu_upsample(source, guide[:, :, :3], depth_tex, uw, uh,
                             args.sigma, args.sharp)

    device_ups = load_raw(args.directory, args.tag, "upsampled", np.uint8, uw * uh)
    if device_ups is not None and args.sharp == 0.0:
        device_ups = device_ups.reshape(uh, uw).astype(np.float32) / 255.0
        err = np.abs(upsampled - device_ups)
        print("upsample vs device: mean %.4f, 99th %.4f (quantisation floor %.4f)"
              % (err.mean(), np.percentile(err, 99), 1 / 255))

    # The upsample target is RGBA8, so the warp reads a quantised depth. Skip
    # this and the offset search drifts enough to show up as an edge mismatch
    # against the device.
    depth_full = expand(np.round(upsampled * 255.0) / 255.0, w, h)
    disp = args.separation * w

    for name, sign in (("left", 1.0), ("right", -1.0)):
        mine, s15, s50 = warp_frame(source, depth_full, sign, disp, args.convergence)
        save_png(os.path.join(out, "%s_%s_mine.png" % (args.tag, name)), mine)
        print("%-5s stretched >15%% %.3f%% of pixels, >50%% %.3f%%" % (name, s15, s50))
        device = load_raw(args.directory, args.tag, name, np.uint8, w * h * 4)
        if device is None or args.sharp != 0.0:
            continue
        device = device.reshape(h, w, 4)[:, :, :3].astype(np.float32)
        diff = np.abs(device - mine)
        print("%-5s vs device: mean |diff| %.2f, 99th pct %.0f, max %.0f (of 255)"
              % (name, diff.mean(), np.percentile(diff, 99), diff.max()))

    save_png(os.path.join(out, "%s_source.png" % args.tag), source)
    save_png(os.path.join(out, "%s_depth.png" % args.tag), depth_full * 255.0)
    print("one depth texel covers %.1f x %.1f output pixels"
          % (w / DEPTH_SIZE, h / DEPTH_SIZE))


if __name__ == "__main__":
    sys.exit(main())
