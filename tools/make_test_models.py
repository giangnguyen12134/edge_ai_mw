#!/usr/bin/env python3
"""Generates the tiny ONNX models used by the CPU-runtime tests.

    python3 tools/make_test_models.py tests/data

The generated .onnx files are committed; run this only to regenerate them
(needs `pip install onnx numpy`). The models are synthetic: they exercise the
runtime (layouts, types, dynamic shapes, outputs), they do not detect anything.
"""
import os
import sys

import numpy as np
import onnx
from onnx import TensorProto as T
from onnx import helper as h


def save(graph, path):
    model = h.make_model(graph, opset_imports=[h.make_opsetid("", 13)])
    model.ir_version = 8  # Readable by ONNX Runtime >= 1.10.
    onnx.checker.check_model(model)
    onnx.save(model, path)


def channel_mean(name, dims, elem, axes):
    """Output [1, 3]: the mean of each colour channel."""
    nodes, source = [], "x"
    if elem == T.UINT8:
        nodes.append(h.make_node("Cast", ["x"], ["xf"], to=T.FLOAT))
        source = "xf"
    nodes.append(h.make_node("ReduceMean", [source], ["y"], axes=axes,
                             keepdims=0))
    return h.make_graph(
        nodes, name, [h.make_tensor_value_info("x", elem, dims)],
        [h.make_tensor_value_info("y", T.FLOAT, [1, 3])])


def detector(name, rows):
    """Output [1, N, 6] = constant rows + 0 * mean(input) (depends on input)."""
    rows = np.array([rows], dtype=np.float32)
    return h.make_graph(
        [h.make_node("ReduceMean", ["x"], ["m"], keepdims=0),
         h.make_node("Mul", ["m", "zero"], ["z"]),
         h.make_node("Add", ["rows", "z"], ["y"])],
        name, [h.make_tensor_value_info("x", T.FLOAT, [1, "h", "w", 3])],
        [h.make_tensor_value_info("y", T.FLOAT, [1, rows.shape[1], 6])],
        initializer=[h.make_tensor("rows", T.FLOAT, rows.shape, rows.flatten()),
                     h.make_tensor("zero", T.FLOAT, [], [0.0])])


def speaker(name):
    """Output [1, 4] = mean, max, min, mean(|x|) of a [1, n] float32 window."""
    return h.make_graph(
        [h.make_node("ReduceMean", ["x"], ["a"], axes=[1], keepdims=0),
         h.make_node("ReduceMax", ["x"], ["b"], axes=[1], keepdims=0),
         h.make_node("ReduceMin", ["x"], ["c"], axes=[1], keepdims=0),
         h.make_node("Abs", ["x"], ["ax"]),
         h.make_node("ReduceMean", ["ax"], ["d"], axes=[1], keepdims=0),
         h.make_node("Concat", ["a", "b", "c", "d"], ["y"], axis=0),
         ],
        name, [h.make_tensor_value_info("x", T.FLOAT, [1, "n"])],
        [h.make_tensor_value_info("y", T.FLOAT, [4])])


def main(out):
    os.makedirs(out, exist_ok=True)
    f, u = T.FLOAT, T.UINT8
    save(channel_mean("chan_nhwc_f32", [1, 32, 32, 3], f, [1, 2]),
         f"{out}/chan_nhwc_f32.onnx")
    save(channel_mean("chan_nchw_f32", [1, 3, 32, 32], f, [2, 3]),
         f"{out}/chan_nchw_f32.onnx")
    save(channel_mean("chan_nchw_dyn", [1, 3, "h", "w"], f, [2, 3]),
         f"{out}/chan_nchw_dyn.onnx")
    save(channel_mean("chan_nhwc_u8", [1, 32, 32, 3], u, [1, 2]),
         f"{out}/chan_nhwc_u8.onnx")
    save(detector("det_face", [[64, 64, 256, 256, 0.9, 0]]),
         f"{out}/det_face.onnx")
    save(detector("det_gesture", [[60, 60, 160, 160, 0.8, 1]]),
         f"{out}/det_gesture.onnx")
    save(detector("det_object_xywh",
                  [[208, 208, 100, 100, 0.7, 2], [100, 100, 50, 50, 0.1, 0]]),
         f"{out}/det_object_xywh.onnx")
    save(speaker("speaker_stats"), f"{out}/speaker_stats.onnx")


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "tests/data")
