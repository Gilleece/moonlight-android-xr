#!/usr/bin/env python3
#
# Rebuilds app/src/main/assets/zipdepth_256_int8dr.tflite, the depth model an
# XR2 Gen 1 headset runs on the CPU with XNNPACK.
#
# Dynamic range quantization: int8 weights, float activations, no
# representative dataset. XNNPACK has kernels for it on CONV_2D and
# FULLY_CONNECTED; the GPU delegate does not want it, which is fine since the
# Gen 2 headsets run the fp16 export on the GPU instead.
#
# The source is the npu checkpoint exported at 256 square by
# convert_zipdepth.py ("256 --head npu"). This script converts from the saved
# model onnx2tf leaves in tools/build, and when that is gone it reruns the
# earlier stages, which needs convert_zipdepth.py's packages.
#
#   ./venv/bin/python tools/quantize_depth.py

import os
import subprocess
import sys

from convert_common import BUILD_DIR, fold, run_onnx2tf, verify

ASSET = "app/src/main/assets/zipdepth_256_int8dr.tflite"
SIZE = 256
FP16 = os.path.join(BUILD_DIR, "zipdepth_npu_256_fp16.tflite")
SCRIPT = "tools/convert_zipdepth.py"
SCRIPT_ARGS = ("256", "--head", "npu")
ONNX = os.path.join(BUILD_DIR, "zipdepth_256.onnx")
FOLDED = os.path.join(BUILD_DIR, "zipdepth_256_folded.onnx")
SAVED_MODEL = os.path.join(BUILD_DIR, "zipdepth_tf_out_dgc")

# The graph has a group convolution, which onnx2tf will not put in a saved
# model, so this conversion splits it into separable convolutions. The fp16
# file keeps the group convolution.
GROUP_CONV_ARGS = ("-dgc",)


def has_saved_model():
    return os.path.isfile(os.path.join(SAVED_MODEL, "saved_model.pb"))


def saved_model():
    """The saved model, rebuilt from whatever stage is still on disk."""
    if has_saved_model():
        return SAVED_MODEL

    os.makedirs(BUILD_DIR, exist_ok=True)
    if not os.path.isfile(FOLDED) and not os.path.isfile(ONNX):
        # Nothing of the torch export is left, so the whole convert script
        # runs, writing its fp16 file to tools/build on the way
        print("no ONNX for zipdepth, running " + SCRIPT)
        subprocess.run([sys.executable, SCRIPT] + list(SCRIPT_ARGS), check=True)
        if has_saved_model():
            return SAVED_MODEL

    source = FOLDED if os.path.isfile(FOLDED) else fold(ONNX, FOLDED)
    run_onnx2tf(source, SAVED_MODEL, GROUP_CONV_ARGS)
    return SAVED_MODEL


def convert(path):
    """
    Dynamic range, by the plain route first and the awkward ones after.

    from_saved_model can refuse a graph outright, so the fallbacks are the
    saved model's own signature key and the converter flag. Whatever each
    attempt said comes back for the report.
    """
    import tensorflow as tf

    failures = []
    for name in ("default", "signature", "new converter"):
        try:
            if name == "signature":
                loaded = tf.saved_model.load(path)
                keys = list(loaded.signatures.keys())
                if not keys:
                    failures.append("signature: the saved model has none")
                    continue
                print("retrying with signature key " + keys[0])
                converter = tf.lite.TFLiteConverter.from_saved_model(
                    path, signature_keys=[keys[0]])
            else:
                converter = tf.lite.TFLiteConverter.from_saved_model(path)
            converter.optimizations = [tf.lite.Optimize.DEFAULT]
            if name == "new converter":
                converter.experimental_new_converter = True
            return converter.convert(), name, failures
        except Exception as e:
            failures.append("%s: %s" % (name, str(e).strip().splitlines()[0]))
    raise SystemExit("the converter refused the model:\n  "
                     + "\n  ".join(failures))


def op_summary(path):
    """
    Counts of each op in the file.

    Tensors are left unallocated on purpose: allocating lets XNNPACK take
    the graph and the details come back as DELEGATE nodes instead of ops.
    """
    import tensorflow as tf

    interpreter = tf.lite.Interpreter(model_path=path)
    try:
        ops = interpreter._get_ops_details()
    except AttributeError:
        print("no op details from this runtime")
        return
    counts = {}
    for op in ops:
        counts[op["op_name"]] = counts.get(op["op_name"], 0) + 1
    print("ops: " + ", ".join("%s %d" % (k, counts[k])
                              for k in sorted(counts)))


def main():
    data, route, failures = convert(saved_model())
    for line in failures:
        print("refused, " + line)
    print("converted by the " + route + " route")

    with open(ASSET, "wb") as f:
        f.write(data)

    if os.path.isfile(FP16):
        print("fp16 " + FP16)
        verify(FP16, SIZE)
    print("int8dr " + ASSET)
    verify(ASSET, SIZE)
    op_summary(ASSET)


if __name__ == "__main__":
    main()
