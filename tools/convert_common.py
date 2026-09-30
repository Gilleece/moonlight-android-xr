#!/usr/bin/env python3
#
# Shared by the depth model conversion scripts: the calibration data onnx2tf
# asks for, the MiDaS download, the onnxruntime fold, the onnx2tf call and a
# check that the written tflite has the shapes the app expects.

import os
import subprocess
import sys
import urllib.request

import numpy as np

# Downloads, clones and intermediate files. Covered by the build/ line in
# .gitignore; only the tflite files under app/src/main/assets are kept.
BUILD_DIR = "tools/build"

# onnx2tf downloads this for its strict mode accuracy correction, but the
# bucket it used is gone (NoSuchBucket), so generate it locally. It only
# needs plausible image statistics, not any particular picture.
CALIB_FILE = "calibration_image_sample_data_20x128x128x3_float32.npy"

# MiDaS v2.1 small as its authors publish it, and its length in bytes
MIDAS_ONNX_URL = ("https://github.com/isl-org/MiDaS/releases/download/v2_1"
                  "/model-small.onnx")
MIDAS_ONNX_BYTES = 66772999


def pattern(rng, width, height=None):
    """One plausible image: soft waves, hard blobs and a little noise."""
    height = height or width
    yy = np.mgrid[0:height, 0:width][0] / (height - 1.0)
    xx = np.mgrid[0:height, 0:width][1] / (width - 1.0)
    img = np.zeros((height, width, 3), np.float32)
    for c in range(3):
        img[..., c] = 0.3 + 0.4 * np.sin(
            6.28 * (rng.uniform(0.5, 3) * xx + rng.uniform(0, 1))
        ) * np.cos(6.28 * (rng.uniform(0.5, 3) * yy + rng.uniform(0, 1)))
    for _ in range(6):
        cx, cy, r = rng.uniform(0.2, 0.8, 3)
        r *= 0.25
        img[((xx - cx) ** 2 + (yy - cy) ** 2) < r * r] = rng.uniform(0, 1, 3)
    img += rng.normal(0, 0.03, img.shape).astype(np.float32)
    return np.clip(img, 0, 1)


def download(url, path, size):
    """Fetches url to path unless a whole copy is already there."""
    if os.path.isfile(path) and os.path.getsize(path) == size:
        return path
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    print("downloading " + url)
    part = path + ".part"
    urllib.request.urlretrieve(url, part)
    got = os.path.getsize(part)
    if got != size:
        os.remove(part)
        raise SystemExit("%s came down as %d bytes, expected %d" % (url, got, size))
    os.replace(part, path)
    return path


def make_calibration_data():
    if os.path.isfile(CALIB_FILE):
        return
    rng = np.random.default_rng(0)
    n, s = 20, 128
    out = np.zeros((n, s, s, 3), np.float32)
    for i in range(n):
        out[i] = pattern(rng, s)
    np.save(CALIB_FILE, out)


def fold(onnx_file, folded_file):
    """
    Constant folds the shape arithmetic a torch export leaves behind.

    A model that reads its own tensor shapes exports them as Shape and
    Gather nodes, the intermediate shapes come out dynamic, and onnx2tf
    gives up. onnxruntime's basic optimization pass folds them using
    standard ops only. onnxsim would do the same but segfaults on ZipDepth.
    """
    import onnxruntime as ort

    options = ort.SessionOptions()
    options.graph_optimization_level = ort.GraphOptimizationLevel.ORT_ENABLE_BASIC
    options.optimized_model_filepath = folded_file
    ort.InferenceSession(onnx_file, options, providers=["CPUExecutionProvider"])
    return folded_file


def run_onnx2tf(onnx_file, out_dir, extra=()):
    """Runs onnx2tf and returns the path of the fp16 tflite it wrote."""
    make_calibration_data()
    env = dict(os.environ, TF_USE_LEGACY_KERAS="1")
    subprocess.run([sys.executable, "-m", "onnx2tf", "-i", onnx_file,
                    "-o", out_dir, "-osd"] + list(extra), env=env, check=True)
    stem = os.path.splitext(os.path.basename(onnx_file))[0]
    return os.path.join(out_dir, stem + "_float16.tflite")


def verify(asset, width, height=None):
    """
    Loads the tflite the way the app does and runs one synthetic image.

    The input has to be 1 x height x width x 3 float32 and the output one
    value per pixel, finite and not flat.
    """
    import tensorflow as tf

    interpreter = tf.lite.Interpreter(model_path=asset)
    interpreter.allocate_tensors()
    inp = interpreter.get_input_details()[0]
    out = interpreter.get_output_details()[0]
    print("input %s %s, output %s %s" % (inp["shape"], inp["dtype"].__name__,
                                         out["shape"], out["dtype"].__name__))
    height = height or width
    if list(inp["shape"]) != [1, height, width, 3] or inp["dtype"] != np.float32:
        raise SystemExit("input is not 1x%dx%dx3 float32" % (height, width))
    if int(np.prod(out["shape"])) != width * height:
        raise SystemExit("output is not %d elements" % (width * height))

    image = pattern(np.random.default_rng(1), width, height)[None].astype(np.float32)
    interpreter.set_tensor(inp["index"], image)
    interpreter.invoke()
    depth = interpreter.get_tensor(out["index"])
    if not np.isfinite(depth).all():
        raise SystemExit("output is not finite")
    lo, hi = float(depth.min()), float(depth.max())
    if hi - lo < 1e-3:
        raise SystemExit("output range is flat: %f to %f" % (lo, hi))
    print("calibration run: %f to %f, mean %f" % (lo, hi, float(depth.mean())))
    print("wrote %s (%d bytes)" % (asset, os.path.getsize(asset)))
