"""
forecast_model.py -- pure-Python reimplementation of the on-device TFLite
Micro model, for the Test Cases demo reel (telemetry/ml/reel.py).

The REAL model only exists as TFLite Micro C++ on the ESP32-S3 master
(hardware/esp32s3_master/src/model_runner.cpp) -- Django can't call into
firmware, so this is a from-scratch port of the same trained weights, not
a wrapper around the firmware. Every function here has a named C++
counterpart it must stay numerically identical to; see each docstring.

Deliberately pure Python (lists + `math`, no numpy/TensorFlow) so the main
Django .venv gains zero new dependencies -- this whole module is maybe a
few thousand multiply-adds per call, trivial without numpy. TensorFlow is
only ever needed by export_weights.py, a standalone script run by hand
from a separate venv, never imported here or by Django at request time.

Trained weights/normalization constants load once from weights.json
(committed to git -- see export_weights.py for how it's produced) rather
than the ephemeral training scratchpad, so this module has no dependency
on that scratchpad surviving between sessions.
"""
import json
import math
from pathlib import Path

_WEIGHTS_PATH = Path(__file__).with_name("weights.json")

# Populated by reload_weights() below, called once at import time.
LAYERS: dict = {}
FEATURE_COLS: list[str] = []
FEATURE_MEAN: list[float] = []
FEATURE_SCALE: list[float] = []
REG_MEAN: list[float] = []
REG_SCALE: list[float] = []
AMMONIA_SPIKE_THRESHOLD: float = 0.35
TEMP_SPIKE_THRESHOLD: float = 0.35

FEATURE_COUNT = 44
BUCKET_HISTORY_LEN = 7  # h[0]=60min ago ... h[6]=current -- see reel.py

# Base columns each get the same 7 derived features -- must match
# feature_cols order in the training script / hardware/esp32s3_master/
# src/model_runner.cpp's buildFeatureRow() exactly.
BASE_COLUMNS = ["temperature", "humidity", "ammonia_ppm", "hour_sin", "hour_cos", "temp_hum_interaction"]


def reload_weights(path: str | Path | None = None) -> None:
    """(Re)load weights.json into this module's globals. Called once at
    import time with the default path; export_weights.py's --verify step
    also calls this explicitly (with the path it just wrote) before
    comparing against the real Keras model.
    """
    global LAYERS, FEATURE_COLS, FEATURE_MEAN, FEATURE_SCALE, REG_MEAN, REG_SCALE
    global AMMONIA_SPIKE_THRESHOLD, TEMP_SPIKE_THRESHOLD

    p = Path(path) if path else _WEIGHTS_PATH
    with open(p) as f:
        data = json.load(f)

    LAYERS = data["layers"]
    FEATURE_COLS = data["feature_cols"]
    FEATURE_MEAN = data["feature_mean"]
    FEATURE_SCALE = data["feature_scale"]
    REG_MEAN = data["reg_mean"]
    REG_SCALE = data["reg_scale"]
    # best_thresholds order matches clf_targets = ['ammonia_spike', 'temp_spike'].
    thresholds = data["best_thresholds"]
    AMMONIA_SPIKE_THRESHOLD = thresholds[0]
    TEMP_SPIKE_THRESHOLD = thresholds[1]


def _dense(x: list[float], layer_name: str) -> list[float]:
    """y = x @ W + b for one Dense layer, no activation."""
    layer = LAYERS[layer_name]
    w, b = layer["w"], layer["b"]  # w: [in][out], b: [out]
    out_dim = len(b)
    in_dim = len(x)
    y = list(b)
    for i in range(in_dim):
        xi = x[i]
        if xi == 0.0:
            continue
        row = w[i]
        for j in range(out_dim):
            y[j] += xi * row[j]
    return y


def _relu(x: list[float]) -> list[float]:
    return [v if v > 0.0 else 0.0 for v in x]


def _sigmoid(x: float) -> float:
    return 1.0 / (1.0 + math.exp(-x))


def forward_raw(features_normalized: list[float]) -> tuple[list[float], list[float]]:
    """Raw forward pass, no unscaling -- returns (next_values[2], spike_flags[2])
    exactly as the Keras model's two named outputs would, for apples-to-
    apples comparison in export_weights.py's --verify step. Real runtime
    callers want forward() below, not this.

    Topology (see export_weights.py's module docstring for layer-name
    provenance): dense(44->64,relu) -> two branches off the same output --
    temperature: dense_1(64->32,relu) -> t_reg(->linear), t_clf(->sigmoid);
    ammonia: dense_2(64->64,relu) -> dense_3(64->32,relu) -> a_reg(->linear),
    a_clf(->sigmoid). Dropout layers are inference-time no-ops, omitted.
    next_values=[t_reg,a_reg], spike_flags=[a_clf,t_clf] -- matches
    Concatenate(name='next_values')([t_reg,a_reg]) / Concatenate(name=
    'spike_flags')([a_clf,t_clf]) in the training script.
    """
    shared = _relu(_dense(features_normalized, "dense"))

    h_t = _relu(_dense(shared, "dense_1"))
    t_reg = _dense(h_t, "t_reg")[0]
    t_clf = _sigmoid(_dense(h_t, "t_clf")[0])

    h_a1 = _relu(_dense(shared, "dense_2"))
    h_a2 = _relu(_dense(h_a1, "dense_3"))
    a_reg = _dense(h_a2, "a_reg")[0]
    a_clf = _sigmoid(_dense(h_a2, "a_clf")[0])

    return [t_reg, a_reg], [a_clf, t_clf]


def normalize(features: list[float]) -> list[float]:
    """(x - mean) / scale per feature. Matches
    normalizeFeaturesToInputTensor() in model_runner.cpp exactly, including
    the near-zero-scale guard (a feature with ~zero variance in training
    would otherwise divide by ~zero)."""
    out = []
    for i, x in enumerate(features):
        scale = FEATURE_SCALE[i]
        if abs(scale) < 1e-8:
            scale = 1.0
        out.append((x - FEATURE_MEAN[i]) / scale)
    return out


def forward(features_normalized: list[float]) -> dict:
    """Full forward pass + unscale, matching readPrediction() in
    model_runner.cpp exactly: reg[0]=temperature (direct unscale),
    reg[1]=ammonia_ppm_log (expm1 after unscale, clipped >=0)."""
    next_values, spike_flags = forward_raw(features_normalized)

    temperature_next = next_values[0] * REG_SCALE[0] + REG_MEAN[0]
    ammonia_log = next_values[1] * REG_SCALE[1] + REG_MEAN[1]
    ammonia_next = max(math.expm1(ammonia_log), 0.0)

    ammonia_spike_probability = spike_flags[0]
    temp_spike_probability = spike_flags[1]

    return {
        "predicted_temperature": temperature_next,
        "predicted_ammonia": ammonia_next,
        "ammonia_spike_probability": ammonia_spike_probability,
        "ammonia_spike_predicted": ammonia_spike_probability >= AMMONIA_SPIKE_THRESHOLD,
        "temp_spike_probability": temp_spike_probability,
        "temp_spike_predicted": temp_spike_probability >= TEMP_SPIKE_THRESHOLD,
    }


def _sample_std3(a: float, b: float, c: float) -> float:
    """Sample standard deviation (ddof=1) over 3 values, matching pandas'
    rolling(3).std() default -- and sampleStd3() in model_runner.cpp."""
    mean = (a + b + c) / 3.0
    v1, v2, v3 = a - mean, b - mean, c - mean
    variance = (v1 * v1 + v2 * v2 + v3 * v3) / 2.0
    return math.sqrt(max(variance, 0.0))


def _write_column_features(history: list[dict], field: str) -> list[float]:
    """One base column's 7 engineered values -- lag1, lag2, lag3, lag6,
    roll_mean3, roll_std3, delta1, in that order. `history` is the 7-entry
    finalized-bucket history (history[6]=current, history[0]=60 min back).
    Mirrors writeColumnFeatures() in model_runner.cpp exactly."""
    lag1 = history[5][field]
    lag2 = history[4][field]
    lag3 = history[3][field]
    lag6 = history[0][field]
    cur = history[6][field]

    roll_mean3 = (history[4][field] + history[5][field] + history[6][field]) / 3.0
    roll_std3 = _sample_std3(history[4][field], history[5][field], history[6][field])
    delta1 = cur - lag1

    return [lag1, lag2, lag3, lag6, roll_mean3, roll_std3, delta1]


def build_feature_row(history: list[dict], age_weeks: float) -> list[float]:
    """The 44-feature row the model expects, from a 7-entry bucket history.
    Column order/math matches feature_cols / buildFeatureRow() in
    model_runner.cpp exactly: 6 base columns x 7 values = 42, +
    ammonia_accel + week = 44."""
    assert len(history) == BUCKET_HISTORY_LEN

    features: list[float] = []
    for column in BASE_COLUMNS:
        features.extend(_write_column_features(history, column))

    ammonia_lag1 = history[5]["ammonia_ppm"]
    ammonia_lag2 = history[4]["ammonia_ppm"]
    ammonia_cur = history[6]["ammonia_ppm"]
    ammonia_delta1 = ammonia_cur - ammonia_lag1
    ammonia_accel = ammonia_delta1 - (ammonia_lag1 - ammonia_lag2)
    features.append(ammonia_accel)

    week = max(1.0, min(5.0, age_weeks))
    features.append(week)

    assert len(features) == FEATURE_COUNT
    return features


reload_weights()
