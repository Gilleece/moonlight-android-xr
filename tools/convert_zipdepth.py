#!/usr/bin/env python3
#
# Rebuilds app/src/main/assets/zipdepth_512x288_fp16.tflite, the depth model
# an XR2 Gen 2 headset runs on the GPU delegate.
#
# Source model is ZipDepth, MIT License, Copyright (c) 2026 Fabio Tosi,
# https://github.com/fabiotosi92/ZipDepth, fetched as a shallow clone at
# REPO_COMMIT. Its README says the weights were distilled from Depth Anything
# V2 Large pseudo labels.
#
# 512x288 is 16:9, so a 16:9 frame reaches the model unsquashed, and it has
# the pixels of 384 square, the size the ZipDepth repo and paper run at. Both
# sides are multiples of 32. Nothing in the graph is tied to a square.
#
# The model normalizes its own input (ImageNet mean and std inside
# ZipDepth.forward), so the graph takes plain RGB in 0..1 like MiDaS, and its
# output is relative inverse depth, larger is nearer, the same convention.
# The wrapper below only drops the channel axis, so the output is 1xHxW.
#
# The export is the repo's own scripts/export.py, which makes the graph
# static, plus three changes for the LiteRT GPU delegate:
#   - mean_pools: the strip and global pools as mean reductions, since the
#     delegate computes the large AVERAGE_POOL_2D filters wrongly
#   - convex_head: the base checkpoint's convex upsample restated in ops the
#     delegate takes, so the whole graph stays on the GPU
#   - dcr_shuffle: the final pixel shuffle written so it converts to a single
#     DEPTH_TO_SPACE
#
# --head npu exports zipdepth_base_npu.pth instead, which differs only in the
# last upsampling layer. The XR2 Gen 1 int8 copy,
# zipdepth_256_int8dr.tflite, is built from that export at 256 square by
# quantize_depth.py. Any export other than the base head at 512x288 is
# written to tools/build rather than the assets.
#
# compare() checks the tflite against the torch model on the repo's example
# photo and a few frames of its example clip (cut out with ffmpeg when it is
# installed), both as exported and as the repo runs it unpatched.
#
# Setup is convert_midas.py's venv plus torch and torchvision.
#   ./venv/bin/python tools/convert_zipdepth.py [--width W --height H]
#   ./venv/bin/python tools/convert_zipdepth.py [size] [--head base|npu]

import argparse
import os
import shutil
import subprocess
import sys
import types

import torch
import torch.nn.functional as F

from convert_common import BUILD_DIR, fold, run_onnx2tf, verify


def parse_args():
    parser = argparse.ArgumentParser()
    parser.add_argument("size", nargs="?", type=int)
    parser.add_argument("--head", choices=("base", "npu"), default="base")
    parser.add_argument("--width", type=int)
    parser.add_argument("--height", type=int)
    return parser.parse_args()


ARGS = parse_args()
if (ARGS.width is None) != (ARGS.height is None):
    raise SystemExit("give both --width and --height")
if ARGS.size is not None and ARGS.width is not None:
    raise SystemExit("give a square size or --width and --height, not both")
SQUARE = ARGS.size is not None
WIDTH = ARGS.size or ARGS.width or 512
HEIGHT = ARGS.size or ARGS.height or 288
if WIDTH % 32 != 0 or HEIGHT % 32 != 0:
    raise SystemExit("the size has to be a multiple of 32")
# "256" for a square, "512x288" for a rectangle, as the asset names have it
SHAPE = "%d" % WIDTH if SQUARE else "%dx%d" % (WIDTH, HEIGHT)
# The fp16 exports the app loads, so the ones written to the assets
SHIPPED = ("512x288",)
HEAD = ARGS.head
VARIANT = "base"
GLOBAL_MODE = "balanced"
REPO_URL = "https://github.com/fabiotosi92/ZipDepth"
# The commit the shipped assets were exported from
REPO_COMMIT = "91f3fd21e131641f51e8d35736d1958350180e3a"
REPO_DIR = os.path.join(BUILD_DIR, "ZipDepth")
ASSET_DIR = "app/src/main/assets"

# Both checkpoints are in the repository at REPO_COMMIT
CHECKPOINT = os.path.join(REPO_DIR, "checkpoints", "zipdepth_base_npu.pth"
                          if HEAD == "npu" else "zipdepth_base.pth")
# The npu build files keep the plain names quantize_depth.py reads
TAG = "" if HEAD == "npu" else "_convex"
EXAMPLE = os.path.join(REPO_DIR, "assets", "examples", "im0.jpg")
CLIP = os.path.join(REPO_DIR, "assets", "examples", "clip.mp4")
CLIP_SECONDS = (2, 6, 10)
FRAME_DIR = os.path.join(BUILD_DIR, "zipdepth_frames")
ASSET = (os.path.join(ASSET_DIR, "zipdepth_%s_fp16.tflite" % SHAPE)
         if HEAD == "base" and SHAPE in SHIPPED
         else os.path.join(BUILD_DIR, "zipdepth%s_%s_fp16.tflite"
                           % ("_npu" if HEAD == "npu" else "", SHAPE)))
ONNX = os.path.join(BUILD_DIR, "zipdepth%s_%s.onnx" % (TAG, SHAPE))
FOLDED = os.path.join(BUILD_DIR, "zipdepth%s_%s_folded.onnx" % (TAG, SHAPE))
OUT_DIR = os.path.join(BUILD_DIR, "zipdepth%s_tf_out" % TAG)


class Squeezed(torch.nn.Module):
    """RGB in 0..1 in, one channel of inverse depth out without its axis."""

    def __init__(self, model):
        super().__init__()
        self.model = model

    # The repo's exporter reaches for model.encoder.cross_scale to give it a
    # static shape, so the wrapper passes that through
    @property
    def encoder(self):
        return self.model.encoder

    def forward(self, x):
        return self.model(x).squeeze(1)


def fetch():
    os.makedirs(BUILD_DIR, exist_ok=True)
    if not os.path.isdir(REPO_DIR):
        subprocess.run(["git", "init", "-q", REPO_DIR], check=True)
        subprocess.run(["git", "-C", REPO_DIR, "fetch", "--depth", "1", REPO_URL,
                        REPO_COMMIT], check=True)
        subprocess.run(["git", "-C", REPO_DIR, "checkout", "-q", "FETCH_HEAD"],
                       check=True)
    head = subprocess.run(["git", "-C", REPO_DIR, "rev-parse", "HEAD"], check=True,
                          capture_output=True, text=True).stdout.strip()
    if head != REPO_COMMIT:
        raise SystemExit("%s is at %s, not %s" % (REPO_DIR, head, REPO_COMMIT))
    if not os.path.isfile(CHECKPOINT):
        raise SystemExit(CHECKPOINT + " is missing from the clone")


def repo_export():
    """The repository's scripts/export.py, with its onnxsim pass blocked."""
    sys.path.insert(0, os.path.join(REPO_DIR, "scripts"))
    sys.modules["onnxsim"] = None
    import export

    return export


def mean_pools(model):
    """
    The strip and global pools as mean reductions instead of average pools.

    Same arithmetic: the strips average a whole row or column, the global
    one the whole map. onnx2tf writes these as MEAN, which the GPU delegate
    gets right, where the 32x1, 1x32 and 16x16 AVERAGE_POOL_2D it replaces
    came out as a flat map with a bright band down the left edge.

    The two strips are also copied out to the full map before they are
    added, since the delegate does not broadcast an elementwise op on both
    sides at once. The copy is a concatenation: a tile gets folded back into
    the add by the converter and the two sided broadcast returns.
    """
    for m in model.modules():
        name = type(m).__name__
        if name == "StripPoolingAttention":
            def strip(self_m, x):
                h, w = int(x.shape[2]), int(x.shape[3])
                rows = torch.cat([x.mean(3, keepdim=True)] * w, 3)
                cols = torch.cat([x.mean(2, keepdim=True)] * h, 2)
                return x * self_m.gate_conv(rows + cols)

            m.forward = types.MethodType(strip, m)
        elif name == "GlobalContextBlock":
            def context(self_m, x):
                return x + self_m.transform(x.mean((2, 3), keepdim=True))

            m.forward = types.MethodType(context, m)


def edge_pad(x):
    """x with its edge rows and columns repeated once, as a replicate pad."""
    x = torch.cat([x[:, :, :1], x, x[:, :, -1:]], 2)
    return torch.cat([x[:, :, :, :1], x, x[:, :, :, -1:]], 3)


def convex_head(model):
    """
    The base checkpoint's convex upsample in ops the GPU delegate takes.
    The idea of shipping the learned head on the delegate rather than the
    npu checkpoint's plain one came from Nightfall (github.com/tB0nE/
    nightfall, GPLv3), whose export does the same; this restatement is our
    own.

    FastConvexUpsample predicts 9 weights for each pixel of every 2x2 block,
    softmaxes each 9 and sums the 3x3 neighbourhood of the half size depth
    with them. As written, the replicate pad, the unfold and the 5D softmax
    fall off the GPU. Here the pad is edge_pad, the unfold a fixed 3x3
    convolution with one one hot kernel per neighbour, and the mask's last
    1x1 convolution is split into one per sub pixel so each gets a 9 channel
    softmax on the channel axis. A fixed 1x1 convolution of ones then sums
    each 9 into its sub pixel. The relu moves ahead of the pixel shuffle,
    which only moves pixels.
    """
    for m in model.modules():
        if type(m).__name__ != "FastConvexUpsample" or not m.use_unfold:
            continue
        s2 = m.scale * m.scale
        last = m.mask_pred[-1]
        weight = last.weight.detach() / m.temperature
        bias = last.bias.detach() / m.temperature
        groups = torch.nn.ModuleList()
        for s in range(s2):
            conv = torch.nn.Conv2d(last.in_channels, 9, 1)
            conv.weight.data.copy_(weight[s::s2])
            conv.bias.data.copy_(bias[s::s2])
            groups.append(conv)
        total = torch.zeros(s2, 9 * s2, 1, 1)
        for s in range(s2):
            total[s, 9 * s:9 * s + 9] = 1
        m.mask_trunk = m.mask_pred[:-1]
        m.mask_groups = groups
        m.register_buffer("pick", torch.eye(9).view(9, 1, 3, 3))
        m.register_buffer("total", total)

        def convex(self_m, feat, depth):
            hidden = self_m.mask_trunk(feat)
            near = F.conv2d(edge_pad(depth), self_m.pick)
            parts = [torch.softmax(g(hidden), 1) * near
                     for g in self_m.mask_groups]
            up = F.relu(F.conv2d(torch.cat(parts, 1), self_m.total))
            return F.pixel_shuffle(up, self_m.scale)

        feat = torch.randn(1, m.mask_pred[0].in_channels, 24, 24)
        depth = torch.rand(1, 1, 24, 24)
        with torch.no_grad():
            before = F.relu(m._forward_unfold(feat, depth))
            m.forward = types.MethodType(convex, m)
            after = m(feat, depth)
        print("convex head restated: max abs diff %.3g"
              % float((before - after).abs().max()))


def dcr_shuffle(onnx_file):
    """
    Rewrites the one channel DepthToSpace in DCR order, in place.

    torch exports pixel_shuffle as DepthToSpace in CRD order, which onnx2tf
    spells as a 6D reshape and transpose. With one output channel the two
    orders are the same permutation, and DCR converts to DEPTH_TO_SPACE.
    """
    import onnx
    from onnx import helper, shape_inference

    model = onnx.load(onnx_file)
    graph = shape_inference.infer_shapes(model).graph
    shapes = {vi.name: [d.dim_value for d in vi.type.tensor_type.shape.dim]
              for vi in list(graph.value_info) + list(graph.output)}
    count = 0
    for node in model.graph.node:
        if node.op_type != "DepthToSpace":
            continue
        block = next(a.i for a in node.attribute if a.name == "blocksize")
        if shapes.get(node.input[0], [0, 0])[1] != block * block:
            raise SystemExit("%s has more than one output channel" % node.name)
        kept = [a for a in node.attribute if a.name != "mode"]
        del node.attribute[:]
        node.attribute.extend(kept + [helper.make_attribute("mode", "DCR")])
        count += 1
    onnx.save(model, onnx_file)
    print("DepthToSpace nodes set to DCR: %d" % count)


def export_onnx(export, model):
    """
    The repository's exporter, with mean_pools applied over its own patches.

    It patches the pooling modules itself inside export_onnx, so the only
    place to get in after it and before the trace is the export call.
    """
    original = torch.onnx.export

    def hook(*args, **kwargs):
        mean_pools(model)
        return original(*args, **kwargs)

    torch.onnx.export = hook
    try:
        export.export_onnx(model, (1, 3, HEIGHT, WIDTH), ONNX, opset=17)
    finally:
        torch.onnx.export = original


def probe_images():
    """The repo's example photo and a few frames of its example clip."""
    paths = [EXAMPLE]
    if shutil.which("ffmpeg") is None:
        print("no ffmpeg, so the photo alone is compared")
        return paths
    os.makedirs(FRAME_DIR, exist_ok=True)
    for second in CLIP_SECONDS:
        path = os.path.join(FRAME_DIR, "clip_%02ds.png" % second)
        if not os.path.isfile(path):
            subprocess.run(["ffmpeg", "-v", "error", "-ss", str(second), "-i",
                            CLIP, "-frames:v", "1", path], check=True)
        paths.append(path)
    return paths


def compare(asset, references):
    """
    The written tflite against torch models on the probe images.

    references is a list of (name, model). Each line gives the correlation
    and mean absolute difference against each, and the mean of that model's
    map so the difference has a scale.
    """
    import numpy as np
    import tensorflow as tf
    from PIL import Image

    interpreter = tf.lite.Interpreter(model_path=asset)
    interpreter.allocate_tensors()
    totals = {name: [0.0, 0.0] for name, _ in references}
    paths = probe_images()
    for path in paths:
        image = Image.open(path).convert("RGB").resize((WIDTH, HEIGHT),
                                                       Image.BILINEAR)
        image = np.asarray(image, np.float32)[None] / 255.0
        interpreter.set_tensor(interpreter.get_input_details()[0]["index"],
                               image)
        interpreter.invoke()
        out = interpreter.get_tensor(
            interpreter.get_output_details()[0]["index"]).ravel()
        line = []
        for name, model in references:
            with torch.no_grad():
                ref = model(torch.from_numpy(image).permute(0, 3, 1, 2))
            ref = ref.numpy().ravel()
            corr = float(np.corrcoef(ref, out)[0, 1])
            diff = float(np.abs(ref - out).mean())
            totals[name][0] += corr
            totals[name][1] += diff
            line.append("%s %.6f / %.5f (mean %.3f)"
                        % (name, corr, diff, float(ref.mean())))
        print("%s: %s" % (os.path.basename(path), ", ".join(line)))
    print("torch match, correlation / mean abs diff over %d images: %s"
          % (len(paths), ", ".join("%s %.6f / %.5f"
                                   % (name, totals[name][0] / len(paths),
                                      totals[name][1] / len(paths))
                                   for name, _ in references)))


def graph_report(asset):
    """
    Op counts, and any elementwise op that broadcasts on both sides.

    The delegate gets those wrong, so the list should say none. Shapes come
    from the interpreter; the analyzer is only asked for its GPU notes.
    """
    import contextlib
    import io

    import tensorflow as tf

    # Left unallocated on purpose: allocating hands the graph to XNNPACK and
    # the ops come back as one DELEGATE node
    interpreter = tf.lite.Interpreter(model_path=asset)
    shapes = {t["index"]: list(t["shape"])
              for t in interpreter.get_tensor_details()}

    counts = {}
    two_sided = []
    for op in interpreter._get_ops_details():
        counts[op["op_name"]] = counts.get(op["op_name"], 0) + 1
        if op["op_name"] not in ("ADD", "SUB", "MUL") or len(op["inputs"]) != 2:
            continue
        out = shapes.get(op["outputs"][0])
        ins = [shapes.get(i) for i in op["inputs"]]
        if all(s is not None and s != out for s in ins):
            two_sided.append("%s %d: %s with %s -> %s"
                             % (op["op_name"], op["index"], ins[0], ins[1], out))
    print("ops: " + ", ".join("%s %d" % (k, counts[k]) for k in sorted(counts)))
    # The strip and global pools, to show their sizes follow the input
    means = []
    for op in interpreter._get_ops_details():
        if op["op_name"] == "MEAN":
            line = "%s -> %s" % (shapes.get(op["inputs"][0]),
                                 shapes.get(op["outputs"][0]))
            if line not in means:
                means.append(line)
    print("means: " + "; ".join(means))
    print("two sided broadcasts: "
          + ("none" if not two_sided else "; ".join(two_sided)))

    text = io.StringIO()
    with contextlib.redirect_stdout(text):
        tf.lite.experimental.Analyzer.analyze(model_path=asset,
                                              gpu_compatibility=True)
    warnings = sorted({line.strip() for line in text.getvalue().splitlines()
                       if "COMPATIBILITY WARNING" in line})
    print("analyzer: " + ("; ".join(warnings) if warnings
                          else "no GPU compatibility warnings"))


def load(export):
    return export.load_model(CHECKPOINT, VARIANT, GLOBAL_MODE, "cpu",
                             upsample_unfold=HEAD == "base")


def main():
    fetch()
    export = repo_export()
    model = load(export)
    if HEAD == "base":
        convex_head(model)
    wrapped = Squeezed(model).eval()
    with torch.no_grad():
        print("torch output",
              tuple(wrapped(torch.zeros(1, 3, HEIGHT, WIDTH)).shape))
    export_onnx(export, wrapped)

    folded = fold(ONNX, FOLDED)
    if HEAD == "base":
        dcr_shuffle(folded)
    os.replace(run_onnx2tf(folded, OUT_DIR), ASSET)
    verify(ASSET, WIDTH, HEIGHT)
    compare(ASSET, [("export", wrapped), ("repo", Squeezed(load(export)).eval())])
    graph_report(ASSET)


if __name__ == "__main__":
    main()
