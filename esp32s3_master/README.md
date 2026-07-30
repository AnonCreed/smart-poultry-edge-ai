# ESP32-S3 Master Node -- Firmware

Receives raw sensor samples from the ESP32 DevKit V1 sensor node
(`esp32/`) over ESP-NOW, runs an on-device TFLite Micro model to forecast
next-tick temperature/ammonia and an ammonia-spike probability, forwards the
combined record to the Django backend, and relays the resulting
classification back to the sensor node so it can drive its own actuators.

```
esp32 (DevKit V1)  --ESP-NOW-->  esp32s3_master (this board)  --WiFi/HTTP-->  Django
   sensors + fan/heater              inference + relay              /api/telemetry/submit/
        ^                                    |
        `------------ ESP-NOW (classification) <----------'
```

## Contents

| File | Purpose |
|---|---|
| `esp32s3_master.ino` | Main sketch: ESP-NOW receive, TFLite inference, WiFi/HTTP forward, actuator relay. |
| `model_data.h` | Trained TFLite Micro model, flatbuffer byte array. **Trained artifact -- do not hand-edit.** |
| `scaler_params.h` | Feature normalization (mean/scale) and the calibrated spike-risk decision threshold. **Trained artifact -- do not hand-edit.** |
| `config_master.h` | WiFi credentials, Django API URL, and the sensor node's MAC address. Edit this before flashing. |

## Why Arduino IDE, not PlatformIO

The sensor node (`esp32/`) builds with PlatformIO. This board uses the
Arduino IDE instead because the TFLite Micro integration was trained and
packaged against the `TensorFlowLite_ESP32` Arduino library, and re-pinning
that dependency graph under PlatformIO risks silently breaking the already-
validated model integration. Two toolchains for two boards is an intentional
trade-off, not an oversight.

## Flashing

1. Arduino IDE: install board package **"esp32" by Espressif Systems**, then
   select your ESP32-S3 board (the one with the trained model, not the
   DevKit V1).
2. Install libraries via Library Manager:
   - `TensorFlowLite_ESP32`
   - `ArduinoJson` (bblanchon) -- needed for the new Django uplink; not
     required by the original inference-only bundle.
3. Edit `config_master.h`:
   - `WIFI_SSID` / `WIFI_PASSWORD` -- **must be the same network the sensor
     node joins.** ESP-NOW requires both peers on the same WiFi channel;
     joining the same AP is the simplest way to guarantee that.
   - `API_BASE_URL` -- your Django host's LAN IP.
   - `SENSOR_NODE_MAC_ADDR` -- see the pairing procedure below.
4. Open `esp32s3_master.ino`, upload, then open the Serial Monitor at
   115200 baud.

## MAC pairing procedure (do this once, both directions)

ESP-NOW peers must know each other's MAC address in advance -- there's no
discovery handshake in this setup.

1. Flash this board first. On boot it prints:
   ```
   Receiver MAC: XX:XX:XX:XX:XX:XX
   ```
   Copy that into `esp32/include/config.h`'s `MASTER_MAC_ADDR` on the sensor
   node.
2. Flash the sensor node (`esp32/`, see its README). On boot it prints its
   own MAC in the `[ESPNOW] Ready. Local MAC=...` line. Copy that into this
   board's `config_master.h`'s `SENSOR_NODE_MAC_ADDR`.
3. Reflash whichever board you edited last so both sides have the other's
   real address instead of the `FF:FF:FF:FF:FF:FF` placeholder.

## Data flow per sample

1. Sensor node sends a `SensorPacket` (temperature, humidity, ammonia_ppm,
   hour, month) over ESP-NOW every 5 s.
2. This board keeps a rolling 3-sample raw history and a 6-row engineered
   feature window (see `esp32s3_master.ino`'s `buildFeatureRowFromHistory`)
   -- both need to fill before the first inference; expect
   `Waiting raw warmup` / `Waiting feature warmup` lines for the first
   ~30-40 seconds after boot.
3. Once warmed up, every new sample runs one inference producing
   `predicted_temperature`, `predicted_ammonia`, and
   `predicted_spike_probability`.
4. That triple, plus the raw reading, POSTs to
   `{API_BASE_URL}{API_SUBMIT_PATH}` using the same JSON contract the Python
   simulator and Django's ingestion validation already expect (see the
   project root `README.md`).
5. Django's rule-based classification (`predicted_class`) comes back in the
   response and is relayed to the sensor node over ESP-NOW so it can drive
   its fan/heater -- independent of the spike-risk prediction, which is an
   advisory signal only and never itself flips the classification.

## Spike-risk threshold

`scaler_params.h`'s `kSpikeThreshold` (0.21) is the calibrated cutoff above
which the model's classifier head predicts an imminent ammonia spike. Django
echoes the same value as `ammonia_spike_risk_threshold` in the
`/api/telemetry/historical/` response (`telemetry/classifier.py`'s
`AMMONIA_SPIKE_RISK_THRESHOLD`) so the dashboard never hardcodes a second
copy. If the model is ever retrained with a different threshold, update both
places together.

## Known limitations

- Single sensor node only: `SENSOR_NODE_MAC_ADDR` and the feature/history
  buffers assume one upstream board. Supporting multiple sensor nodes would
  need per-node history buffers and a node-identity field threaded through
  the ESP-NOW packet, Django's model, and the dashboard -- not implemented
  here.
- No ESP-NOW encryption -- fine on a private/trusted LAN, not for anything
  internet-facing. See `HARDWARE_DOCUMENTATION.md`'s security section for
  the same caveat on the original HTTP-only design.
- If the Django POST fails (network blip, backend restart), that sample's
  classification never reaches the sensor node and its actuators simply
  hold their last commanded state -- same fail-safe behavior as the
  original single-board firmware.
