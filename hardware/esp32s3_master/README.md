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

## Swapping in a retrained model

`model_data.h` and `include/scaler_params.h` are the only files a retrain
should touch -- regenerate both from the training pipeline, drop them in
place of the existing ones, and rebuild. Nothing else needs to change
**as long as the new model keeps the same input feature layout
(`kFeatureCount` engineered features x `kWindowSize` rows, see
`buildFeatureRowFromHistory()` in `model_runner.cpp`) and the same two
output heads** (a 2-element regression head for next temp/ammonia, a
1-element classifier head for spike probability). If the training pipeline
ever changes which features it derives, `buildFeatureRowFromHistory()` in
`model_runner.cpp` needs matching edits -- everything else in that file is
generic glue (windowing, normalization, tensor plumbing) that doesn't care
what the model looks like.

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
2. This board keeps a rolling 3-sample raw history and a 6-row engineered
   feature window (`model_runner.cpp`) -- both need to fill before the
   first inference; expect `[MODEL] warming up` lines for the first
   ~30-40 seconds after boot.
3. Once warmed up, every new sample runs one inference producing
   `predicted_temperature`, `predicted_ammonia`, and
   `predicted_spike_probability`. While still warming up, those three
   fields POST as `null` and Django classifies from live readings alone.
4. That triple, plus the raw reading, POSTs to
   `http://{DJANGO_HOST}:{DJANGO_PORT}{SUBMIT_PATH}` using the same JSON
   contract the Python simulator and Django's ingestion validation already
   expect (see the project root `README.md`).
5. Django's rule-based classification (`predicted_class`) comes back in the
   response and is relayed to the sensor node over ESP-NOW so it can drive
   its fan/heater -- independent of the spike-risk prediction, which is an
   advisory signal only and never itself flips the classification.

## Spike-risk threshold

`scaler_params.h`'s `kSpikeThreshold` is the calibrated cutoff above which
the model's classifier head predicts an imminent ammonia spike. Django
echoes the same value as `ammonia_spike_risk_threshold` in the
`/api/telemetry/historical/` response (`telemetry/classifier.py`'s
`AMMONIA_SPIKE_RISK_THRESHOLD`) so the dashboard never hardcodes a second
copy. If the model is ever retrained with a different threshold, update both
places together.

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
