# ESP32-S3 Master Node -- Firmware

Receives raw sensor samples from the ESP32 DevKit V1 sensor node
(`hardware/esp32/`) over ESP-NOW, runs an on-device TFLite Micro model to forecast
next-tick temperature/ammonia and an ammonia-spike probability, forwards the
combined record to the Django backend, and relays the resulting
classification back to the sensor node so it can drive its own actuators.

```
esp32 (DevKit V1)  --ESP-NOW-->  esp32s3_master (this board)  --WiFi/HTTP-->  Django
   sensors + fan/heater              inference + relay              /api/telemetry/submit/
        ^                                    |
        `------------ ESP-NOW (classification) <----------'
```

Builds with PlatformIO (same toolchain as the sensor node in `hardware/esp32/`).

## Contents

| File | Purpose |
|---|---|
| `src/main.cpp` | ESP-NOW receive, WiFi/HTTP forward to Django, actuator relay. |
| `src/model_runner.cpp` / `include/model_runner.h` | Feature engineering, TFLite Micro inference, output scaling. |
| `include/model_data.h` | Trained TFLite Micro model, flatbuffer byte array. **Trained artifact -- do not hand-edit.** |
| `include/scaler_params.h` | Feature normalization (mean/scale) and the calibrated spike-risk decision threshold. **Trained artifact -- do not hand-edit.** |
| `include/config.h` | Django API port/path, discovery protocol constants, NTP, timing constants. No IP to edit -- see Discovery below. |
| `include/credentials.h` (gitignored, copy from `.example`) | WiFi SSID/password. |

## Model architecture (retrained 2026-08-26)

A bifurcated multi-task MLP (Keras) trained offline against
`poultry_sensor_data_cleaned.csv` (9,708 rows), replacing the earlier
windowed model:

- **Input**: a flat 44-element engineered feature vector -- no on-device
  windowed tensor. The "sliding window" lives entirely in feature
  engineering (pandas `.shift()`/`.rolling()` at training time,
  `buildFeatureRow()` in `model_runner.cpp` on-device): 6 base columns
  (`temperature`, `humidity`, `ammonia_ppm`, `hour_sin`, `hour_cos`,
  `temp_hum_interaction`) x 7 derived values each (`lag1`, `lag2`, `lag3`,
  `lag6`, `roll_mean3`, `roll_std3`, `delta1`) = 42, plus `ammonia_accel`
  (second-order ammonia momentum) and `week` (flock age in weeks, clipped
  to `[1, 5]`) = 44 total. Exact order is fixed by `feature_cols` in the
  training script and mirrored feature-for-feature in `buildFeatureRow()`.
- **Output**: two 2-element heads --
  `next_values` (regression: standardized next-tick temperature and
  log1p-transformed next-tick ammonia, both needing the inverse transform
  in `readPrediction()`) and `spike_flags` (classification: sigmoid
  ammonia-spike and temperature-spike probabilities). TFLite conversion
  does **not** guarantee output tensor order matches the Python
  `outputs=[...]` declaration order -- `readPrediction()` disambiguates at
  runtime via a value-range heuristic (sigmoid outputs stay in `[0, 1]`;
  standardized regression outputs don't), documented in-line with the exact
  verification snippet, with a fallback to the empirically verified fixed
  order if that heuristic is ever ambiguous.
- **Training cadence vs. real sensor cadence**: the training pipeline
  resamples to a fixed 10-minute interval, but the sensor node transmits
  every ~5 s (`SAMPLE_INTERVAL_MS`, matching this project's
  `HARDWARE_DOCUMENTATION.md` / report Section 5.5.6). Feeding raw 5s
  samples straight into "lag1" would silently redefine it from "10 minutes
  ago" to "5 seconds ago" and make every learned weight meaningless.
  `model_runner.cpp` bridges this by averaging incoming samples into a
  rolling 10-minute `Bucket`, finalizing it into a `FinalizedBucket` history
  ring buffer (7 deep -- lag6 reaches 60 min back, plus the current bucket)
  once `kBucketDurationMs` elapses, and computing all lag/rolling/delta
  features only from that finalized-bucket history. Consequence: the first
  real inference doesn't happen until **~70 minutes after boot** (7 buckets
  x 10 min), during which `[MODEL] warming up` prints instead.
- **Export format -- must stay pure float32, not quantized**: an earlier
  export used `tf.lite.TFLiteConverter`'s dynamic-range quantization
  (`converter.optimizations = [tf.lite.Optimize.DEFAULT]`), which produced
  a smaller `.tflite` (~19 KB vs ~49 KB) that ran fine on a desktop
  `tf.lite.Interpreter` but made TFLite Micro's `Invoke()` silently
  overwrite the input tensor with NaN on real hardware -- confirmed by
  Serial-printing the input tensor's values immediately before and after
  `Invoke()` (finite before, all-NaN after), isolating the corruption to
  the embedded kernel's handling of hybrid-quantized `FULLY_CONNECTED`
  weights rather than any host-side bug. Fix: re-export from the saved
  `.keras` model with no `converter.optimizations` set at all. **Any future
  retrain must export the same way** (see below) or this bug reappears.

## Swapping in a retrained model

`model_data.h` and `include/scaler_params.h` are the only files a retrain
should touch -- regenerate both from the training pipeline's
`export_meta.pkl` + the trained `.tflite`, drop them in place of the
existing ones, and rebuild. Nothing else needs to change **as long as the
new model keeps the same 44-feature input layout and the same two
2-element output heads** described above. If the training pipeline ever
changes which features it derives (`feature_cols` in the training script),
`buildFeatureRow()` in `model_runner.cpp` needs matching edits -- everything
else in that file (bucketing, normalization, tensor plumbing) is generic
and doesn't care what the model looks like.

Steps, in order:

1. Retrain and save the Keras model as usual, then export to TFLite
   **without quantization** (see "must stay pure float32" above):
   ```python
   converter = tf.lite.TFLiteConverter.from_keras_model(model)
   # Deliberately no converter.optimizations -- see README for why.
   tflite_model = converter.convert()
   open('model_esp32.tflite', 'wb').write(tflite_model)
   ```
2. Verify the output tensor order didn't shift:
   ```python
   import tensorflow as tf, numpy as np
   interp = tf.lite.Interpreter(model_path='model_esp32.tflite')
   interp.allocate_tensors()
   inp, out = interp.get_input_details(), interp.get_output_details()
   x = np.zeros((1, 44), dtype=np.float32)
   interp.set_tensor(inp[0]['index'], x); interp.invoke()
   for d in out: print(d['name'], '->', interp.get_tensor(d['index']))
   ```
   If the order changed, `readPrediction()`'s runtime heuristic in
   `model_runner.cpp` should still cope automatically -- but double-check
   its documented fallback order comment matches the new result anyway.
3. Verify the op list didn't grow beyond what `init()`'s
   `MicroMutableOpResolver<3>` registers (`FullyConnected`, `Concatenation`,
   `Logistic`):
   ```python
   from tensorflow.lite.python import schema_py_generated as schema_fb
   buf = bytearray(open('model_esp32.tflite', 'rb').read())
   m = schema_fb.ModelT.InitFromObj(schema_fb.Model.GetRootAsModel(buf, 0))
   codes = {(oc.deprecatedBuiltinCode if oc.builtinCode == 0 else oc.builtinCode) for oc in m.operatorCodes}
   names = {v: k for k, v in vars(schema_fb.BuiltinOperator).items() if isinstance(v, int)}
   print([names.get(c, c) for c in codes])
   ```
   A new op means adding the matching `Add*()` call in `init()` and bumping
   the resolver's template argument.
4. Regenerate `model_data.h` as a C byte array from the `.tflite` file
   (standard `xxd -i` / equivalent one-liner), and `scaler_params.h` from
   `export_meta.pkl`'s `feature_mean`/`feature_scale`/`reg_mean`/
   `reg_scale`/`best_thresholds` -- keep the reg/clf ordering comments in
   that file in sync with `reg_targets`/`clf_targets` from the training
   script.
5. Rebuild, reflash, and confirm on Serial that early inferences (after the
   ~70-minute warm-up) are finite and plausible -- not NaN.

## Flashing

1. Install PlatformIO (CLI or the VS Code extension).
2. Copy `include/credentials.h.example` to `include/credentials.h` and fill
   in `WIFI_SSID` / `WIFI_PASSWORD` -- **must be the same network the sensor
   node joins.** ESP-NOW requires both peers on the same WiFi channel;
   joining the same AP is the simplest way to guarantee that.
3. `pio run -t upload` (or use the PlatformIO IDE build/upload buttons),
   then open the serial monitor at 115200 baud. No Django IP to configure --
   see Discovery below.

## Discovery (no hardcoded Django IP)

This board finds the Django host at runtime instead of using a baked-in IP,
so the same firmware image works unmodified on any WiFi network the board
and the Django PC both join:

1. On boot (and again from `loop()` any time the host is unknown), it
   broadcasts a UDP packet to the local subnet's broadcast address on port
   `DISCOVERY_PORT` (`config.h`).
2. `telemetry/discovery.py` on the Django side listens on that same UDP
   port (started automatically by `manage.py runserver`) and replies to
   whoever sent the request.
3. The board reads the reply's **source IP** as the Django host -- not
   the payload -- so no address is ever hand-configured.
4. If POSTs start failing against a previously-discovered host (e.g. the
   Django PC got a new DHCP lease), the board drops it and re-discovers
   automatically after a few consecutive failures.

Requirements: the Django PC and this board must be on the same subnet
(broadcast doesn't cross routers/VLANs), and `python manage.py runserver`
must be bound to `0.0.0.0:8000` (not `127.0.0.1`) so both the HTTP API and
the discovery responder are reachable from the board.

## MAC pairing

Unlike the sensor node, this board doesn't need a MAC address configured up
front -- `onDataRecv()` in `main.cpp` auto-learns the sensor node's MAC from
the first ESP-NOW packet it receives and registers it as a peer so it can
send `ActuatorCommand`s back.

1. Flash this board first. On boot it prints:
   ```
   [BOOT] Master MAC (WiFi STA): AA:BB:CC:DD:EE:FF
   ```
   Copy that into `hardware/esp32/include/config.h`'s `MASTER_MAC_ADDR` on
   the sensor node.
2. Flash the sensor node (`hardware/esp32/`, see its README). No MAC needs
   copying back the other way -- this board learns it automatically.

## Data flow per sample

1. Sensor node sends a `SensorPacket` (temperature, humidity, ammonia_ppm,
   hour, month) over ESP-NOW every 5 s.
2. This board averages incoming samples into a rolling 10-minute bucket and
   keeps a 7-bucket finalized history (`model_runner.cpp`) -- see "Model
   architecture" above for why. That history needs to fill before the first
   inference; expect `[MODEL] warming up` lines for the first ~70 minutes
   after boot.
3. Once warmed up, each bucket finalization runs one inference producing
   `predicted_temperature`, `predicted_ammonia`, and
   `predicted_spike_probability` (ammonia-spike probability; the model also
   computes a `temp_spike_probability` but that isn't wired to Django/the
   dashboard yet). While still warming up, the three POSTed fields are
   `null` and Django classifies from live readings alone.
4. That triple, plus the raw reading, POSTs to
   `http://{DJANGO_HOST}:{DJANGO_PORT}{SUBMIT_PATH}` using the same JSON
   contract the Python simulator and Django's ingestion validation already
   expect (see the project root `README.md`).
5. Django's rule-based classification (`predicted_class`) comes back in the
   response and is relayed to the sensor node over ESP-NOW so it can drive
   its fan/heater -- independent of the spike-risk prediction, which is an
   advisory signal only and never itself flips the classification.
6. The same `ActuatorCommand` also carries `has_prediction` +
   `predicted_temperature` + `predicted_ammonia` (the just-computed
   `pred.temperature_next` / `pred.ammonia_next`, or the last cached
   prediction when this send is the independent MANUAL-override poll rather
   than a fresh inference -- see `g_lastPrediction` in `main.cpp`) purely so
   the sensor node's LCD can show a "Predicted" screen. These three fields
   are display-only; no actuation decision reads them back on either board.

## Spike-risk threshold

`scaler_params.h`'s `kAmmoniaSpikeThreshold` is the calibrated cutoff above
which the model's classifier head predicts an imminent ammonia spike (the
model also has a `kTempSpikeThreshold` for the sibling temperature-spike
head, currently used on-device only -- see "Data flow per sample" above).
Django echoes `kAmmoniaSpikeThreshold`'s value as
`ammonia_spike_risk_threshold` in the `/api/telemetry/historical/` response
(`telemetry/classifier.py`'s `AMMONIA_SPIKE_RISK_THRESHOLD`) so the
dashboard never hardcodes a second copy. If the model is ever retrained
with different thresholds, update both places together.

## Known limitations

- Single sensor node only: the feature/history buffers assume one upstream
  board. Supporting multiple sensor nodes would need per-node history
  buffers and a node-identity field threaded through the ESP-NOW packet,
  Django's model, and the dashboard -- not implemented here.
- No ESP-NOW encryption -- fine on a private/trusted LAN, not for anything
  internet-facing. See `HARDWARE_DOCUMENTATION.md`'s security section for
  the same caveat on the original HTTP-only design.
- If the Django POST fails (network blip, backend restart), that sample's
  classification never reaches the sensor node and its actuators simply
  hold their last commanded state.
