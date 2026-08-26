"""
export_weights.py -- one-time, dev-only export of the trained MLP's weights
to a plain-JSON artifact Django can load without TensorFlow installed.

NOT imported by Django at runtime, NOT part of the request path -- this is a
standalone script you run by hand, once, whenever the model is retrained,
from the SEPARATE TensorFlow-capable venv used for training (the main
Django .venv deliberately has no TensorFlow/numpy dependency; see
forecast_model.py's module docstring for why).

Usage:
    <mlvenv>/bin/python export_weights.py <path-to-trained-model.keras> \
        telemetry/ml/weights.json --verify

The trained .keras model and export_meta.pkl currently live only in an
ephemeral scratchpad directory (see the training session that produced
hardware/esp32s3_master/include/model_data.h/scaler_params.h) -- this
script's output is the first time those trained weights get committed
anywhere durable in the repo, independent of that scratchpad surviving.

Layer names below match the functional-API model definition in the
training script (train_model.py): shared 'dense' -> temperature branch
'dense_1' -> {'t_reg', 't_clf'}; ammonia branch 'dense_2' -> 'dense_3' ->
{'a_reg', 'a_clf'}. Extracted by name (not positional get_weights() order)
specifically so a future retrain that reorders Keras's internal layer list
can't silently swap two layers' weights without this script erroring out
(a missing/renamed layer raises immediately).
"""
import argparse
import json
import pickle
import sys
from pathlib import Path

LAYER_NAMES = ["dense", "dense_1", "dense_2", "dense_3", "t_reg", "a_reg", "t_clf", "a_clf"]


def export(model_path: str, export_meta_path: str | None, out_path: str) -> dict:
    import tensorflow as tf

    model = tf.keras.models.load_model(model_path)

    weights = {}
    for name in LAYER_NAMES:
        layer = model.get_layer(name)
        w, b = layer.get_weights()
        weights[name] = {"w": w.tolist(), "b": b.tolist()}

    payload = {"layers": weights}

    if export_meta_path:
        with open(export_meta_path, "rb") as f:
            meta = pickle.load(f)
        payload["feature_cols"] = meta["feature_cols"]
        payload["feature_mean"] = meta["feature_mean"]
        payload["feature_scale"] = meta["feature_scale"]
        payload["reg_mean"] = meta["reg_mean"]
        payload["reg_scale"] = meta["reg_scale"]
        payload["reg_targets"] = meta["reg_targets"]
        payload["clf_targets"] = meta["clf_targets"]
        payload["best_thresholds"] = meta["best_thresholds"]

    with open(out_path, "w") as f:
        json.dump(payload, f)

    return payload


def verify(model_path: str, weights_json_path: str, n_samples: int = 20, tol: float = 1e-4) -> None:
    """Feed random inputs through both the real Keras model and the
    weights.json-driven pure-Python forward pass (forecast_model.py) and
    assert they agree within `tol`. Catches a wrong layer name, transposed
    weight matrix, or missing bias immediately, rather than shipping a
    subtly-wrong dashboard number that only gets noticed by eyeballing
    the demo.
    """
    import random

    import numpy as np
    import tensorflow as tf

    sys.path.insert(0, str(Path(__file__).resolve().parent.parent.parent))
    from telemetry.ml import forecast_model  # noqa: E402  (path hack above)

    forecast_model.reload_weights(weights_json_path)

    model = tf.keras.models.load_model(model_path)
    feature_count = len(forecast_model.FEATURE_MEAN)

    random.seed(0)
    max_abs_diff = 0.0
    for _ in range(n_samples):
        x = [random.uniform(-2.0, 2.0) for _ in range(feature_count)]

        keras_out = model.predict(np.array([x], dtype=np.float32), verbose=0)
        # Keras model.predict on a functional model with two named outputs
        # returns them in the model.output_names order -- read positionally
        # from that same order rather than assuming dict/list shape.
        out_by_name = dict(zip(model.output_names, keras_out))
        keras_next = out_by_name["next_values"][0]
        keras_spike = out_by_name["spike_flags"][0]

        py_next, py_spike = forecast_model.forward_raw(x)

        for a, b in zip(list(keras_next) + list(keras_spike), py_next + py_spike):
            max_abs_diff = max(max_abs_diff, abs(float(a) - float(b)))

    print(f"[verify] max abs diff over {n_samples} random inputs: {max_abs_diff:.2e}")
    if max_abs_diff > tol:
        raise SystemExit(f"[verify] FAILED -- exceeds tolerance {tol:.0e}")
    print("[verify] OK")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("model_path", help="Path to the trained .keras model")
    parser.add_argument("out_path", help="Where to write weights.json")
    parser.add_argument("--export-meta", default=None,
                         help="Path to export_meta.pkl (adds feature_cols/mean/scale/reg_*/thresholds to the JSON)")
    parser.add_argument("--verify", action="store_true",
                         help="After exporting, cross-check the pure-Python forward pass against Keras")
    args = parser.parse_args()

    export(args.model_path, args.export_meta, args.out_path)
    print(f"Wrote {args.out_path}")

    if args.verify:
        verify(args.model_path, args.out_path)


if __name__ == "__main__":
    main()
