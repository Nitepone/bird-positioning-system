"""Converts the BirdNET v2.4 TensorFlow SavedModel to ONNX.

Used by fetch-birdnet.sh; needs tensorflow-cpu, tf2onnx and onnxruntime.

BirdNET's spectrogram layers compute `Cast(RFFT(frames), float32)`, which in
TensorFlow keeps the real part of the FFT. tf2onnx cannot convert an RFFT
unless it feeds a ComplexAbs, so each RFFT + Cast pair is replaced by the
equivalent product with a cosine basis (Re(DFT(x))[k] = sum_n x[n] cos(2 pi k n / N))
before conversion. The result is checked against TensorFlow.

Usage: python convert_birdnet_v24.py SAVED_MODEL_DIR OUTPUT.onnx
"""

import sys

import numpy as np
import onnxruntime as ort
import tensorflow as tf
import tf2onnx
from tensorflow.python.framework.convert_to_constants import convert_variables_to_constants_v2
from tensorflow.python.framework import tensor_util

WINDOW = 144_000


def main(saved_model: str, output: str) -> None:
    model = tf.saved_model.load(saved_model)
    fn = model.signatures["basic"]
    frozen = convert_variables_to_constants_v2(fn)
    graph_def = frozen.graph.as_graph_def()
    nodes = {n.name: n for n in graph_def.node}

    replaced = 0
    for cast in list(graph_def.node):
        if cast.op != "Cast" or cast.attr["SrcT"].type != tf.complex64.as_datatype_enum:
            continue
        rfft = nodes[cast.input[0].split(":")[0]]
        if rfft.op != "RFFT" or cast.attr["DstT"].type != tf.float32.as_datatype_enum:
            continue
        frames_name = rfft.input[0]
        fft_len = int(tensor_util.MakeNdarray(nodes[rfft.input[1].split(":")[0]].attr["value"].tensor)[0])
        frame_len = frozen.graph.get_tensor_by_name(
            frames_name if ":" in frames_name else frames_name + ":0"
        ).shape[-1]
        assert frame_len is not None and frame_len <= fft_len, (frame_len, fft_len)

        n = np.arange(frame_len)[:, None]
        k = np.arange(fft_len // 2 + 1)[None, :]
        basis = np.cos(2.0 * np.pi * n * k / fft_len).astype(np.float32)

        const = graph_def.node.add()
        const.name = cast.name + "/cos_basis"
        const.op = "Const"
        const.attr["dtype"].type = tf.float32.as_datatype_enum
        const.attr["value"].tensor.CopyFrom(tensor_util.make_tensor_proto(basis))

        # Reuse the Cast's name so its consumers keep working unchanged.
        cast.op = "BatchMatMulV2"
        del cast.input[:]
        cast.input.extend([frames_name, const.name])
        cast.attr.clear()
        cast.attr["T"].type = tf.float32.as_datatype_enum
        cast.attr["adj_x"].b = False
        cast.attr["adj_y"].b = False
        graph_def.node.remove(rfft)
        replaced += 1
        print(f"replaced {rfft.name}: frames of {frame_len}, FFT length {fft_len}")
    assert replaced > 0, "no RFFT + Cast pairs found; has the model changed?"

    input_name = frozen.inputs[0].name
    output_name = frozen.outputs[0].name
    tf2onnx.convert.from_graph_def(
        graph_def, input_names=[input_name], output_names=[output_name], opset=17, output_path=output
    )
    print(f"wrote {output}")

    # Verify against TensorFlow on noise and a tone sweep.
    rng = np.random.default_rng(0)
    t = np.arange(WINDOW) / 48000.0
    batch = np.stack([
        rng.normal(0, 0.05, WINDOW),
        0.3 * np.sin(2 * np.pi * (2000 + 1000 * t) * t),
    ]).astype(np.float32)
    expected = list(fn(inputs=tf.constant(batch)).values())[0].numpy()
    sess = ort.InferenceSession(output, providers=["CPUExecutionProvider"])
    actual = sess.run(None, {sess.get_inputs()[0].name: batch})[0]
    diff = float(np.max(np.abs(expected - actual)))
    print(f"max |TF - ONNX| logit difference: {diff:.2e}")
    if diff > 1e-2:
        sys.exit("ONNX output does not match TensorFlow")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])
