#!/usr/bin/env python3
#
# Rebuilds app/src/main/assets/midas_v21_small_256_fp16.tflite.
#
# Source model is MiDaS v2.1 small (256x256), MIT licensed, Intel ISL.
# We start from the ONNX export published by the MiDaS authors and do the
# TFLite conversion ourselves. The frozen graph they also publish
# (model-small.pb) is an onnx-tf export that expands every depthwise
# convolution into per channel Conv2D, 17001 of them, which is useless for
# the GPU delegate. onnx2tf gives a clean NHWC graph instead: 73 CONV_2D,
# 24 DEPTHWISE_CONV_2D, 5 RESIZE_BILINEAR.
#
# The ImageNet mean/std normalization is baked into the ONNX graph, so the
# model takes plain RGB in 0..1. Output is relative inverse depth with an
# arbitrary scale, larger is nearer, normalized per frame on device.
#
# Setup (macOS, python 3.9 is fine):
#   python3 -m venv venv
#   ./venv/bin/pip install tensorflow tf_keras onnx onnxruntime \
#       onnx_graphsurgeon sng4onnx simple_onnx_processing_tools psutil \
#       onnx2tf "numpy<2"
#   ./venv/bin/python tools/convert_midas.py
#
# Takes about a minute. fp16 and fp32 outputs are numerically identical on
# our test images, so we ship fp16 and halve the APK cost.

import os
import subprocess
import sys
import urllib.request

import numpy as np

ONNX_URL = "https://github.com/isl-org/MiDaS/releases/download/v2_1/model-small.onnx"
ONNX_FILE = "model-small.onnx"
OUT_DIR = "tf_out"
ASSET = "app/src/main/assets/midas_v21_small_256_fp16.tflite"

# onnx2tf downloads this for its strict mode accuracy correction, but the
# bucket it used is gone (NoSuchBucket), so generate it locally. It only
# needs plausible image statistics, not any particular picture.
CALIB_FILE = "calibration_image_sample_data_20x128x128x3_float32.npy"


def make_calibration_data():
    if os.path.isfile(CALIB_FILE):
        return
    rng = np.random.default_rng(0)
    n, s = 20, 128
    out = np.zeros((n, s, s, 3), np.float32)
    yy, xx = np.mgrid[0:s, 0:s] / (s - 1.0)
    for i in range(n):
        img = np.zeros((s, s, 3), np.float32)
        for c in range(3):
            img[..., c] = 0.3 + 0.4 * np.sin(
                6.28 * (rng.uniform(0.5, 3) * xx + rng.uniform(0, 1))
            ) * np.cos(6.28 * (rng.uniform(0.5, 3) * yy + rng.uniform(0, 1)))
        for _ in range(6):
            cx, cy, r = rng.uniform(0.2, 0.8, 3)
            r *= 0.25
            img[((xx - cx) ** 2 + (yy - cy) ** 2) < r * r] = rng.uniform(0, 1, 3)
        img += rng.normal(0, 0.03, img.shape).astype(np.float32)
        out[i] = np.clip(img, 0, 1)
    np.save(CALIB_FILE, out)


def main():
    if not os.path.isfile(ONNX_FILE):
        print("downloading " + ONNX_URL)
        urllib.request.urlretrieve(ONNX_URL, ONNX_FILE)

    make_calibration_data()

    env = dict(os.environ, TF_USE_LEGACY_KERAS="1")
    subprocess.run([sys.executable, "-m", "onnx2tf", "-i", ONNX_FILE,
                    "-o", OUT_DIR, "-osd"], env=env, check=True)

    src = os.path.join(OUT_DIR, "model-small_float16.tflite")
    os.replace(src, ASSET)
    print("wrote %s (%.1f MB)" % (ASSET, os.path.getsize(ASSET) / 1e6))


if __name__ == "__main__":
    main()
