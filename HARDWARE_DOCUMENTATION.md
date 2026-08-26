# Hardware & Embedded Firmware Architecture Defense Guide

> **Two-board architecture as of the ESP-NOW revision.** The system is now
> two microcontrollers, not one: an **ESP32 DevKit V1 sensor node**
> (`hardware/esp32/`, unchanged hardware, revised firmware) reads DHT11/MQ-137 and
> sends raw samples over **ESP-NOW** to an **ESP32-S3 master node**
> (`hardware/esp32s3_master/`, new), which runs an on-device TFLite Micro model and
> owns the WiFi/HTTP leg to Django. Everything below that describes the
> single-board HTTP pipeline has been updated to describe the sensor node's
> half only; the master node's half is documented where it diverges, and in
> full in `hardware/esp32s3_master/README.md`.

## 1. Stack Overview & Library Analysis
- Microcontroller Hardware: ESP32 DevKit V1 class board (sensor node), selected via board = esp32dev in PlatformIO, plus an ESP32-S3 board (master node) running an Arduino IDE sketch.
- Core Framework: PlatformIO build system with Arduino C++ framework on Espressif32 for the sensor node; Arduino IDE with the same Espressif "esp32" board package for the master node (required by its TensorFlowLite_ESP32 library dependency -- see hardware/esp32s3_master/README.md's "Why Arduino IDE, not PlatformIO").

### Build and Runtime Context
- MCU family: ESP32 (dual-core Xtensa, integrated Wi-Fi, hardware PWM via LEDC, ADC1/ADC2 blocks).
- Firmware environment: [env:esp32dev] in hardware/esp32/platformio.ini.
- Monitor/upload settings:
  - monitor_speed = 115200 (matches Serial.begin(115200)).
  - upload_speed = 921600.
- Build diagnostics:
  - CORE_DEBUG_LEVEL=3 (INFO level logs in ESP core).

### Dependency Breakdown from platformio.ini
1. adafruit/DHT sensor library @ ^1.4.6
- Which library is used:
  - DHT sensor driver used through DHT.h and DHT class object.
- Why it was chosen over alternatives:
  - Mature timing-critical implementation for DHT11 one-wire style protocol.
  - Stable API, broad adoption, and good interoperability with Arduino ecosystem.
  - Lower integration risk versus writing custom bit-banged timing logic (which is error-prone under RTOS scheduling).
- How it operates inside the code:
  - Constructed once as static DHT dht(PIN_DHT_DATA, DHT11).
  - Initialized by dht.begin() in setup().
  - dht.readHumidity() and dht.readTemperature() called per sample tick in sampleDht().
  - NAN return values are treated as invalid data and the tick is skipped.

2. adafruit/Adafruit Unified Sensor @ ^1.1.14
- Which library is used:
  - Shared abstraction layer dependency required by Adafruit DHT stack.
- Why it was chosen over alternatives:
  - Official dependency path for Adafruit sensor drivers; avoids mismatch issues.
- How it operates inside the code:
  - Indirect use through DHT library internals; not directly referenced in source.

3. ESP-NOW (`esp_now.h`, bundled with the esp32 Arduino core -- no lib_deps entry)
- Which library is used:
  - Espressif's connectionless peer-to-peer WiFi-radio protocol, used in place of the JSON/HTTP request-response pattern the single-board design used.
- Why it was chosen over alternatives:
  - The sensor node no longer needs a full IP/HTTP stack to reach the master -- ESP-NOW is a raw, low-latency link over the same 2.4 GHz radio, well suited to a compact fixed-size struct sent every 5 s.
  - No AP round-trip per message; delivery is acknowledged at the radio layer via a send-status callback (`onDataSent`), which is enough for this system's hold-last-state fault model.
- How it operates inside the code:
  - `esp_now_init()`, `esp_now_register_send_cb`/`esp_now_register_recv_cb`, and `esp_now_add_peer()` with the master's MAC address (`MASTER_MAC_ADDR` in config.h) in `initEspNow()`.
  - `esp_now_send()` transmits a `SensorPacket` struct (not JSON -- a fixed-layout binary struct, memcpy'd directly into/out of the ESP-NOW payload on both boards).
  - `onDataRecv()` receives an `ActuatorCommand` struct relayed back from the master, under a critical section since the callback runs in the WiFi task context.
- Note: the master node (`hardware/esp32s3_master/`) still uses `bblanchon/ArduinoJson` and `HTTPClient.h` exactly as described in the original single-board design below -- it has simply moved from this board to that one. See `hardware/esp32s3_master/README.md`.

### Dependency Breakdown from include directives in main.cpp
1. Arduino.h
- Which library is used:
  - Arduino core API for ESP32 (timing, serial, GPIO, PWM wrapper functions).
- Why chosen:
  - Required base runtime for framework=arduino.
- How it operates inside the code:
  - Provides setup(), loop(), millis(), delay(), analogRead(), ledcSetup(), ledcWrite(), String, Serial.

2. WiFi.h / esp_wifi.h
- Which library is used:
  - ESP32 Wi-Fi radio wrapper (Arduino core) plus the lower-level esp_wifi_set_channel() from the IDF.
- Why chosen:
  - This board never associates with any access point -- no credentials, no IP, no HTTP, no NTP. WiFi.h is retained purely to bring the radio hardware up (WiFi.mode(WIFI_STA)) and read its own MAC address; esp_wifi_set_channel() then pins that radio to a fixed channel (ESPNOW_WIFI_CHANNEL, config.h) so ESP-NOW can reach the ESP32-S3 master without ever joining its AP. See config.h's "Radio" section for why the channel has to be fixed at compile time instead of negotiated by joining an AP (as this board used to do, and as the master still does for its own WiFi/HTTP leg to Django).
- How it operates inside the code:
  - WiFi.mode(WIFI_STA), WiFi.setSleep(false), WiFi.disconnect() (defensive, prevents any auto-join attempt), esp_wifi_set_channel(ESPNOW_WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE), WiFi.macAddress().

3. esp_now.h
- Which library is used:
  - ESP32 ESP-NOW peer-to-peer protocol, bundled with the esp32 Arduino core.
- Why chosen:
  - Replaces HTTPClient.h/ArduinoJson.h on this board (see the ESP-NOW dependency entry above) now that Django ingestion happens on the master node instead.
- How it operates inside the code:
  - esp_now_init(), esp_now_register_send_cb/esp_now_register_recv_cb, esp_now_add_peer(), esp_now_send().

4. DHT.h
- Used to read DHT11 sensor values via DHT class.

5. math.h
- Which library is used:
  - Standard math functions.
- Why chosen:
  - Required for powf used in MQ-137 transfer function conversion.
- How it operates inside the code:
  - ppm = MQ137_A * powf(ratio, MQ137_B).

6. config.h (custom project header)
- Which header is used:
  - Firmware compile-time constants for networking, timing, pins, PWM, ADC calibration, MQ curve constants.
- Why chosen:
  - Separation of policy/constants from logic enables easier viva discussion, portability, and safer maintenance.
- How it operates inside the code:
  - Supplies all literal values referenced in setup/loop and helper functions.

### Custom C++ Source/Header Inventory
- Sensor node (PlatformIO, ESP32 DevKit V1):
  - Source: hardware/esp32/src/main.cpp
  - Header: hardware/esp32/include/config.h
- Master node (Arduino IDE sketch, ESP32-S3):
  - Sketch: hardware/esp32s3_master/esp32s3_master.ino
  - Headers: hardware/esp32s3_master/config_master.h (network/pairing config),
    hardware/esp32s3_master/model_data.h and hardware/esp32s3_master/scaler_params.h (trained
    TFLite Micro model + feature normalization -- generated artifacts, not
    hand-written logic).

## 2. Complete GPIO & Circuit Pinout Table

| Component Name | Pin / GPIO # | Component Type (Sensor/Actuator/Power) | Signal Type (Analog / Digital / I2C / SPI) | Active State (HIGH/LOW) | Technical Purpose |
| :--- | :--- | :--- | :--- | :--- | :--- |
| DHT11 Data Line | GPIO4 (PIN_DHT_DATA) | Sensor | Digital single-wire style data | Idle HIGH via pull-up, data pulses active low/high timing encoded | Reads ambient temperature and humidity in sampleDht(). |
| MQ-137 Analog Output | GPIO34 (PIN_MQ_DATA, ADC1_CH6) | Sensor | Analog (ADC1, 12-bit, 11 dB attenuation) | N/A (continuous analog voltage) | Reads NH3 proxy voltage, oversampled then converted to PPM in mqCountsToPpm(). |
| Fan PWM (speed) | GPIO32 (PIN_PWM_FAN) | Actuator | Digital PWM (LEDC channel 0, 25 kHz, 8-bit) | Duty is a *speed target*, not a power switch | Wired directly to pin 4 of a standard 4-pin PC fan (its own internal driver IC). **Not a MOSFET gate** -- the fan's +12V/GND leads are wired straight to the rail, always powered; this line only tells the fan's onboard controller what speed to target. Many such fans don't honor 0% duty as a true stop (idle at a minimum RPM floor instead) -- see PIN_FAN_ENABLE below for the actual hard-off. |
| Fan hard cutoff | GPIO27 (PIN_FAN_ENABLE) | Actuator | Digital output | HIGH = fan powered, LOW = fan de-energized | Gates an external N-MOSFET on the fan's GND leg for a genuine power cutoff, independent of the PWM line's speed-target semantics above. Driven alongside `PWM_FAN_CHANNEL` in `applyActuators()`: HIGH whenever `fan_pwm > 0`, LOW otherwise. |
| Heater Relay | GPIO25 (PIN_HEATER_RELAY) | Actuator | Digital output | Active-LOW: LOW = heater ON, HIGH = heater OFF | Switches the PTC heater's relay whenever the master-relayed `heater_pwm` byte is nonzero (master computes it from LOW_TEMP_ALERT classification, or a dashboard MANUAL override); ON/OFF only, no speed target -- unlike the fan, this is a plain relay module, not a PWM-driven MOSFET gate. |
| USB-UART Serial (console) | UART0 default pins (board routed via USB bridge) | Debug/Interface | Digital UART | TX active-high line logic | Boot logs, diagnostics, telemetry printouts at 115200 baud. |

### Electrical and Pin-Selection Rationale
- GPIO34 is input-only and mapped to ADC1, which remains usable while Wi-Fi is active; this is critical because ADC2 is unavailable during active Wi-Fi on classic ESP32. This still holds with ESP-NOW: the radio stays powered on a fixed channel for ESP-NOW the whole time (see below), even though this board never actually associates with an access point -- the same ADC1-vs-radio-active constraint applies either way.
- Fan PWM (GPIO32) and its hard-cutoff MOSFET gate (GPIO27) are both general-purpose, non-strapping pins with nothing else assigned to them. The heater relay (GPIO25) is likewise general-purpose/non-strapping; unlike the fan, it's a plain ON/OFF relay module, not a PWM/LEDC channel.
- DHT11 data line on GPIO4 is a common safe GPIO with external 10 kOhm pull-up assumption.

## 3. Communication & Data Transmission Pipeline

Two hops now, not one:

```
Sensor node (ESP32)  --ESP-NOW-->  Master node (ESP32-S3)  --WiFi/HTTP-->  Django
       ^                                                          |
       `------------------ ESP-NOW (classification) --------------'
```

- Hop 1 -- sensor node to master, ESP-NOW:
  - Physical/link: IEEE 802.11 Wi-Fi radio, used in ESP-NOW connectionless mode (no association/handshake, no IP). The sensor node never joins any access point at all -- it has no WiFi credentials, gets no IP, and speaks nothing but ESP-NOW; only the ESP32-S3 master joins the AP (for its own WiFi/HTTP leg to Django).
  - Transport/application: a fixed-layout `SensorPacket` C struct, sent with `esp_now_send()` directly to the master's MAC address -- no serialization, no HTTP.
  - Both radios must still share a WiFi channel for ESP-NOW to work even without either side "connecting" in the usual sense -- previously guaranteed by having both boards join the same AP and inherit its channel; now the sensor node fixes its channel at compile time instead (`ESPNOW_WIFI_CHANNEL`, `esp32/include/config.h`), since it no longer has an AP to negotiate one from. The master still gets its channel from the router as before and logs a boot-time warning if the two ever disagree (see `esp32s3_master/include/config.h`'s `ESPNOW_WIFI_CHANNEL`).
- Hop 2 -- master to Django, WiFi/HTTP (this is the leg the rest of this document originally described end-to-end; it is unchanged in mechanism, only relocated to the ESP32-S3 board):
  - Physical/link: IEEE 802.11 Wi-Fi in station mode.
  - Transport: TCP/IP.
  - Application: HTTP REST POST with JSON payload.
  - Endpoint: `POST http://{discovered-django-host}:8000/api/telemetry/submit/` -- the host IP is no longer hardcoded; the master finds it via a UDP broadcast discovery handshake at boot (see `hardware/esp32s3_master/README.md` "Discovery" section and `telemetry/discovery.py`).
- Return path -- Django's classification travels master -> sensor node over ESP-NOW as an `ActuatorCommand` struct, the mirror image of hop 1.

### Sensor Node Request Flow (per 5 s tick)
1. initRadio() runs once at boot (WiFi.mode(WIFI_STA) + esp_wifi_set_channel()) -- no per-loop WiFi maintenance call exists anymore, since there's no AP association to lose or reconnect.
2. Any pending ActuatorCommand relayed from the master since the last iteration is applied via applyActuators() -- checked every loop iteration, independent of the sampling cadence.
3. Sampling tick gate uses millis() against nextSampleAt; executes every SAMPLE_INTERVAL_MS (5000 ms).
4. sampleDht() obtains humidity and temperature; invalid NAN data aborts current tick.
5. sampleMq137() gathers 16 ADC samples on GPIO34 and averages.
6. sendSample() populates a SensorPacket (temperature/humidity/ammonia_ppm only -- no hour/month; the master supplies hour itself from its own NTP sync, see the Master Node Flow below) and calls esp_now_send() to the master's MAC address.
7. onDataSent() (registered callback) logs delivery failure asynchronously; no application-level retry -- the next 5 s tick supersedes a lost sample.

### Master Node Flow (per received packet)
1. onDataRecv() copies the incoming SensorPacket out of the ESP-NOW payload under a critical section (runs in the WiFi task context).
2. loop() folds the fresh sample into a rolling 10-minute accumulation bucket. Once that bucket finalizes, its lag/rolling/delta features are computed against a 7-bucket history (model_runner.cpp) and fed into the TFLite Micro model as a flat 44-feature vector -- **not** a small raw-sample window; the retrained model needs 7 finalized 10-minute buckets (lag6 = 60 min back + the current bucket) before its first inference, i.e. **~70 minutes after boot**, not the ~45 s the earlier windowed model needed. See hardware/esp32s3_master/README.md's "Model architecture" section for the full feature list and why the wait is what it is (the model was trained on 10-minute-resampled data, not the sensor's real 5s cadence).
3. Once warmed up, the interpreter runs one inference per finalized bucket, producing predicted_temperature and predicted_ammonia (~10 minutes ahead -- this "10 minutes" is the forecast's time horizon, unrelated to the ~70-minute warm-up wait above) plus two spike probabilities (predicted_spike_probability for ammonia, and a temperature-spike probability not yet surfaced to Django/the dashboard).
4. postToDjango() builds the combined JSON record (raw + predictions, null while still warming up) and POSTs it -- same StaticJsonDocument/HTTPClient pattern the single-board design used.
5. On HTTP 201, the response's record.predicted_class is parsed and relayed back to the sensor node via esp_now_send().
6. On any failure (WiFi down, non-201, parse error), nothing is relayed -- the sensor node's actuators simply hold their last commanded state, the same fail-safe behavior the original single-board design had.

### Payload Inspection

ESP-NOW SensorPacket (sensor node -> master, binary struct, not JSON):
- seq (uint32_t)
- temperature (float)
- humidity (float)
- ammonia_ppm (float)

No hour/month fields -- the sensor node has no WiFi/NTP of its own (see
"Hop 1" above), so it can't supply either. The master now reads its own
NTP-synced clock (currentHour() in main.cpp) to source the hour-of-day
cyclic feature the on-device model needs; month was already unused by the
retrained model before this change.

ESP-NOW ActuatorCommand (master -> sensor node, binary struct):
- state (char[24], one of the four EnvironmentalState values)

Outgoing JSON fields from the master's postToDjango() (unchanged contract from the original single-board transmit(), plus one new field):
- temperature (float)
- humidity (float)
- ammonia_level (float)
- predicted_temperature (float or null)
- predicted_ammonia (float or null)
- predicted_spike_probability (float in [0, 1] or null) -- new: the TFLite Micro classifier head's ammonia-spike probability, absent/null until the master node is linked.

Example payload (master node, prediction populated):
{
  "temperature": 30.25,
  "humidity": 62.10,
  "ammonia_level": 11.40,
  "predicted_temperature": 30.90,
  "predicted_ammonia": 12.00,
  "predicted_spike_probability": 0.14
}

Example payload (master node, before its ~70-minute bucket-history warm-up completes -- see hardware/esp32s3_master/README.md's "Model architecture" section):
{
  "temperature": 30.25,
  "humidity": 62.10,
  "ammonia_level": 11.40,
  "predicted_temperature": null,
  "predicted_ammonia": null,
  "predicted_spike_probability": null
}

### Server Response Parsing (Backend -> Master Node)
- Expected status code: 201 Created only.
- Expected response envelope from Django:
  - status: "ok"
  - record: object containing persisted telemetry, including predicted_class.
- Parsed key path in firmware:
  - respDoc["record"]["predicted_class"]
- Valid states expected by firmware switch logic:
  - CRITICAL_AMMONIA
  - HEAT_STRESS_WARNING
  - LOW_TEMP_ALERT
  - OPTIMAL_ENVIRONMENT
- This classification is then re-sent to the sensor node as an ActuatorCommand over ESP-NOW -- the master never applies fan/heater state itself, it only relays.

### Fault Tolerance and Error Behavior
- Wi-Fi disconnection handling (both boards):
  - ensureWifi() checks WiFi.status().
  - If disconnected: WiFi.disconnect(true, true) then connectWifi().
  - connectWifi() retries up to WIFI_MAX_RETRIES (10) with WIFI_RETRY_MS (2000 ms).
- ESP-NOW delivery handling (both hops):
  - Send status is checked asynchronously via the registered send callback; a failure is logged, not retried -- the next 5 s tick supersedes a lost sample or a lost classification.
  - No acknowledgment-based retry loop exists in either direction; this is a deliberate simplicity trade-off appropriate for a 5 s telemetry cadence where a single dropped packet is inconsequential.
- Timeout handling (master node only, HTTP leg):
  - HTTP timeout set to HTTP_TIMEOUT_MS (4000 ms), bounding stall duration inside postToDjango().
- HTTP error handling (master node only):
  - Any status != 201 is treated as failure and logged; no classification is relayed back for that sample.
  - No in-function retry; retry naturally occurs on next received ESP-NOW sample.
- JSON parse failure handling (master node only):
  - deserializeJson error returns failure; ESP-NOW relay to the sensor node is skipped.
- Packet loss/outage behavior:
  - Any break in the chain (ESP-NOW sensor->master, master's WiFi/HTTP, or ESP-NOW master->sensor) results in no new actuator command reaching the sensor node.
  - Actuators remain at previous duty values (held state), because applyActuators() only runs when a fresh ActuatorCommand arrives. This is unchanged from the original single-board design, just spread across an additional hop -- more links in the chain means more places a single sample can be lost, though the failure mode at the actuator is identical.
- Sensor fault handling (sensor node):
  - DHT NAN -> skip tick.
  - MQ rail/open-circuit voltage guard returns 0.0 ppm instead of inf/NaN propagation.
  - NH3 output clamped to [0, 500] ppm to align backend admissible range.
- Clock fault handling (sensor node):
  - NTP sync failure at boot is logged but non-fatal; currentHourMonth() falls back to the last successfully-read hour/month (seeded to a neutral noon/June default on cold start) so a clock outage degrades forecast accuracy rather than stalling sampling.

## 4. Execution Flow & Logic Breakdown

### Setup Initialization (setup())
1. Serial monitor configuration and baud:
  - Serial.begin(115200), then short delay(50), then boot log print.
2. Pin/peripheral initialization:
  - dht.begin() initializes DHT11 stack.
  - analogReadResolution(12) sets ADC to 12-bit.
  - analogSetAttenuation(ADC_11db) configures approximately 0..3.3V range.
3. Actuator setup:
  - Fan: ledcSetup()/ledcAttachPin() configure and attach the LEDC PWM
    channel on PIN_PWM_FAN, then ledcWrite() to PWM_DUTY_OFF; PIN_FAN_ENABLE
    is set as a digital output and driven LOW (de-energized) immediately, so
    the fan doesn't glitch on before its first control cycle.
  - Heater: pinMode(OUTPUT) + digitalWrite for PIN_HEATER_RELAY, driven to
    HEATER_RELAY_OFF (HIGH, active-LOW module) immediately, same
    don't-glitch-on-boot precaution.
4. Wi-Fi connection loop and handling:
  - connectWifi() enters STA mode, disables modem sleep, begins association, retries with bounded loop.
5. NTP time sync:
  - syncNtpTime() calls configTime() then attempts one getLocalTime() read (bounded by NTP_SYNC_TIMEOUT_MS) purely for a diagnostic boot-time log line; a failure here is not fatal, see currentHourMonth()'s fallback.
6. ESP-NOW bring-up:
  - initEspNow() registers send/recv callbacks and adds the master node (MASTER_MAC_ADDR) as a peer; setup() halts in an infinite delay loop if this fails, since nothing downstream can function without it.
7. Sensor warmup/stabilization:
  - No explicit timed warmup state machine; first sample can run immediately after setup via nextSampleAt = millis().
  - Practical note: MQ-137 typically needs burn-in and runtime thermal stabilization externally managed.

### Main Loop Mechanics (loop())
1. Non-blocking timing strategy:
  - Uses millis() schedule check:
    - if (int32_t(now - nextSampleAt) < 0) return.
  - This signed-delta pattern is rollover-safe across millis() wrap (~49.7 days).
2. Connectivity maintenance:
  - ensureWifi() runs every iteration, not gated by the sample timer.
3. Actuator relay check (also ungated by the sample timer):
  - If g_newCommand is set (populated by the onDataRecv ESP-NOW callback), copy it out under a critical section and call applyActuators().
4. Schedule advancement:
  - nextSampleAt = now + SAMPLE_INTERVAL_MS.
5. Sensor sequence:
  - Read DHT11 via sampleDht(); abort tick on failure.
  - Read MQ via sampleMq137() with oversampling and conversion.
6. Transmission sequence:
  - sendSample() populates a SensorPacket (including currentHourMonth()'s wall-clock fields) and calls esp_now_send() to the master.
7. Failure branch behavior:
  - Logs a failure message on a local send error; actuators are unaffected by this board's own send failures (they only change in response to an incoming ActuatorCommand, or lack thereof).

Forecasting and classification no longer happen on this board at all -- see the Master Node Flow above and hardware/esp32s3_master/README.md for where that logic now lives.

## 5. Exhaustive Function & Logic Index

### 5.1 static void connectWifi()
- Function Signature: static void connectWifi()
- Exact Job:
  - Bring Wi-Fi STA link up with bounded retry and diagnostics.
- Step-by-Step Logic:
1. Configure Wi-Fi mode to station.
2. Disable Wi-Fi sleep for lower control-loop latency.
3. Start association using SSID/password constants.
4. Print association attempt log.
5. Loop up to WIFI_MAX_RETRIES:
  - Check WiFi.status() for WL_CONNECTED.
  - On success, print IP and RSSI, then return.
  - Otherwise delay(WIFI_RETRY_MS) and print retry count.
6. If loop exits without success, print failure message and return.

### 5.2 static void ensureWifi()
- Function Signature: static void ensureWifi()
- Exact Job:
  - Maintain link continuity during runtime by reconnecting when disconnected.
- Step-by-Step Logic:
1. If currently connected, immediate return.
2. Print link-lost log.
3. Force disconnect and erase existing state (disconnect(true, true)).
4. Call connectWifi() to re-associate.

### 5.3 static bool sampleDht(float& temperatureC, float& humidityPct)
- Function Signature: static bool sampleDht(float& temperatureC, float& humidityPct)
- Exact Job:
  - Acquire valid DHT11 humidity and temperature reading pair.
- Step-by-Step Logic:
1. Read humidity.
2. Read temperature in Celsius.
3. Check either value for NAN.
4. If NAN present, print diagnostic and return false.
5. Otherwise return true with reference outputs populated.

### 5.4 static float mqCountsToPpm(uint32_t sumCounts, uint16_t samples)
- Function Signature: static float mqCountsToPpm(uint32_t sumCounts, uint16_t samples)
- Exact Job:
  - Convert averaged ADC counts from MQ-137 output into bounded NH3 PPM estimate.
- Step-by-Step Logic:
1. Compute avgCounts = sumCounts / samples.
2. Convert counts to voltage vOut using ADC_MAX_COUNTS and ADC_MAX_VOLTS.
3. Rail/open-circuit guard:
  - If vOut < 0.05V or vOut > (MQ137_VC_VOLTS - 0.05V), return 0.0.
4. Compute sensor resistance:
  - rs = RL * (Vc - vOut) / vOut.
5. Compute ratio = rs / R0.
6. Convert via power-law fit:
  - ppm = MQ137_A * powf(ratio, MQ137_B).
7. Clamp negative to 0.0.
8. Clamp above 500.0 to 500.0.
9. Return ppm.

### 5.5 static float sampleMq137()
- Function Signature: static float sampleMq137()
- Exact Job:
  - Perform oversampled analog acquisition then convert to NH3 PPM.
- Step-by-Step Logic:
1. Initialize 32-bit sum accumulator.
2. For i in 0..MQ_ADC_SAMPLES-1:
  - Read analog value from PIN_MQ_DATA.
  - Add to sum.
  - delayMicroseconds(200) for ADC settling.
3. Call mqCountsToPpm(sum, MQ_ADC_SAMPLES).
4. Return computed ppm.

### 5.6 static void syncNtpTime() / static void currentHourMonth(uint8_t&, uint8_t&)
- Function Signature: static void syncNtpTime(); static void currentHourMonth(uint8_t& hour, uint8_t& month)
- Exact Job:
  - Source wall-clock hour-of-day/month-of-year for the master's model features, without letting a clock fault stall sensor sampling.
- Step-by-Step Logic:
1. syncNtpTime() calls configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER), then getLocalTime() once with a bounded timeout purely to log success/failure at boot.
2. currentHourMonth() calls getLocalTime() with a zero timeout (non-blocking check) on every sample tick.
3. On success, updates and returns the static lastHour/lastMonth (seeded to 12/6 on cold start).
4. On failure, silently returns the last-known values -- no error path, since a stale clock reading is preferable to skipping a sample.

### 5.7 static bool sendSample(float temperatureC, float humidityPct, float ammoniaPpm)
- Function Signature: static bool sendSample(float temperatureC, float humidityPct, float ammoniaPpm)
- Exact Job:
  - Marshal a SensorPacket and hand it to the ESP-NOW stack for delivery to the master. Replaces the original transmit()'s JSON-over-HTTP responsibility with a binary-struct-over-ESP-NOW one; there is no response to parse here, since the classification comes back later, asynchronously, via onDataRecv().
- Step-by-Step Logic:
1. Populate a SensorPacket: incrementing seq, the three live readings, and hour/month from currentHourMonth().
2. Call esp_now_send(masterMac, ...) with the packet's raw bytes.
3. Return true iff esp_now_send() itself queued successfully (ESP_OK) -- this reports local queuing, not remote delivery; actual delivery success/failure surfaces later via the onDataSent callback.

### 5.8 static void onDataSent(...) / static void onDataRecv(...)
- Function Signature: static void onDataSent(const uint8_t* mac, esp_now_send_status_t status); static void onDataRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len)
- Exact Job:
  - Asynchronous ESP-NOW callbacks, invoked from the WiFi task context (not the Arduino loop() thread).
- Step-by-Step Logic:
1. onDataSent(): logs a delivery-failure diagnostic if status != ESP_NOW_SEND_SUCCESS; no other action.
2. onDataRecv(): validates the incoming length matches sizeof(ActuatorCommand); if so, memcpy's the payload into g_lastCommand and sets g_newCommand under a critical section (portENTER_CRITICAL_ISR/portEXIT_CRITICAL_ISR) so loop() can safely consume it without a torn read.

### 5.9 static void applyActuators(uint8_t fan_pwm, uint8_t heater_pwm)
- Function Signature: static void applyActuators(uint8_t fan_pwm, uint8_t heater_pwm)
- Exact Job:
  - Apply two bytes the master has already fully computed -- this board makes
    no actuation decisions of its own any more (no `state`/classification
    parameter at all). fan_pwm is real PWM (0-255 duty, from the master's
    MLP-predicted NH3 error or a dashboard MANUAL override); heater_pwm is a
    0-255 byte (from the classification -- LOW_TEMP_ALERT -> 255, else 0 --
    or a dashboard MANUAL override) that this board only thresholds, since
    its heater is a plain active-LOW relay with no speed target.
- Step-by-Step Logic:
1. heaterOn = (heater_pwm > 0).
2. digitalWrite(PIN_HEATER_RELAY, heaterOn ? HEATER_RELAY_ON : HEATER_RELAY_OFF)
   -- HEATER_RELAY_ON is LOW (active-LOW relay module), HEATER_RELAY_OFF is HIGH.
3. digitalWrite(PIN_FAN_ENABLE, fan_pwm > 0 ? HIGH : LOW) -- hard power
   cutoff, independent of the PWM duty's own speed-target semantics (see
   GPIO27 row in the pinout table).
4. ledcWrite(PWM_FAN_CHANNEL, fan_pwm) -- write the speed target to the
   fan's PWM line.
- **Not interlocked:** fan and heater CAN be ON simultaneously -- fan_pwm and
  heater_pwm are computed independently on the master (and either can come
  from a MANUAL dashboard override, decoupled from the other), so e.g. a
  nonzero MANUAL fan override alongside a nonzero heater_pwm runs the fan and
  energizes the heater relay at once. Earlier revisions of this function
  guaranteed mutual exclusion (a single classification string drove both
  outputs, decided locally on this board); that guarantee no longer holds
  and is not currently re-enforced anywhere in either firmware.

### 5.10 void setup()
- Function Signature: void setup()
- Exact Job:
  - One-time initialization of serial, sensors, ADC, PWM, Wi-Fi, NTP, ESP-NOW, and scheduler seed.
- Step-by-Step Logic:
1. Start serial at 115200.
2. Print boot banner.
3. Initialize DHT sensor.
4. Configure ADC resolution and attenuation.
5. Configure and attach the fan's LEDC PWM channel/pin (PWM_FAN_CHANNEL on
   PIN_PWM_FAN); configure PIN_FAN_ENABLE and PIN_HEATER_RELAY as plain
   digital outputs.
6. Force both actuators OFF before anything else runs: fan PWM duty 0 +
   PIN_FAN_ENABLE LOW; heater relay HEATER_RELAY_OFF (HIGH, active-LOW
   module).
7. Establish Wi-Fi connection (connectWifi()).
8. Sync NTP time (syncNtpTime()).
9. Bring up ESP-NOW and register the master as a peer (initEspNow()); halt in an infinite delay loop on failure.
10. Seed nextSampleAt with current millis for immediate first cycle.

### 5.11 void loop()
- Function Signature: void loop()
- Exact Job:
  - Real-time cooperative control loop: maintain connectivity, apply any relayed actuator command, sample sensors, and transmit telemetry over ESP-NOW.
- Step-by-Step Logic:
1. ensureWifi() to maintain link.
2. Apply any pending ActuatorCommand from the master (checked every iteration, not gated by the sample timer).
3. Read now = millis().
4. If not yet sample time (signed delta negative), return quickly.
4. Schedule next sample time by adding SAMPLE_INTERVAL_MS.
5. Read DHT values; if invalid, abort tick.
6. Read ammonia ppm via sampleMq137().
7. Compute forecast values and validity flag.
8. Call sendSample() over ESP-NOW with the live readings (this board no
   longer computes a forecast itself -- see item 2 above for where
   applyActuators(fan_pwm, heater_pwm) actually runs, gated on a fresh
   ActuatorCommand rather than on this step).
9. If the send succeeded, print the formatted `[TX]` telemetry line.
10. Else, print the failure line and retain current actuator state (held
    from the last applyActuators() call).

## 6. Evaluator Defense Guide: 10 Tough Questions Teachers Will Ask

1. Network: How does data get from the sensor to Django, and why ESP-NOW for one hop and HTTP POST for the other?
- Answer:
  - Two different jobs, two different protocols. Sensor node -> master node needs to move a small fixed-size struct between two boards a few meters apart, every 5 s, with minimal latency and no need for either board to run a full IP stack against each other -- ESP-NOW (a connectionless link directly over the WiFi radio) fits that exactly, and is simpler and lower-latency than standing up a second HTTP server on the master just to receive from the sensor node. Master node -> Django is a different job: crossing onto the LAN/internet to reach a server that already speaks HTTP, where POST is correct semantically because each payload creates a new telemetry record server-side (non-idempotent create), while GET risks caching/proxy behavior. WebSockets would suit a continuous duplex stream but add connection-lifecycle complexity neither side needs for one compact sample every 5 s.

2. Concurrency: Why is non-blocking millis timing used instead of delay(), and what happens if millis overflows after 49 days?
- Answer:
  - The control loop uses cooperative scheduling so connectivity checks and future compute tasks can run each loop iteration. The check uses signed subtraction: int32_t(now - nextSampleAt) < 0. That delta arithmetic is rollover-safe, so when millis wraps, relative ordering remains valid and scheduling continues correctly.

3. Circuitry: How do your digital outputs physically trigger relays to switch high-voltage fans and heaters?
- Answer:
  - Both the fan (GPIO32) and heater (GPIO25) now use the same actuation strategy: a plain digital ON/OFF output into a relay module's IN pin, not a bare relay coil and not a PWM/MOSFET-gate speed control. Relay modules integrate their own driver transistor and flyback diode on-board, so a direct GPIO connection is the module's intended interface -- no external MOSFET/transistor stage is needed on this board's side for either actuator. High-voltage isolation and flyback protection for both loads' inductive switching are handled inside the respective relay modules themselves.

4. Data Parsing: Which library parses the incoming server JSON, and how do you prevent buffer overflow attacks or low memory issues on the MCU?
- Answer:
  - This now happens on the master node (ESP32-S3), not the sensor node: ArduinoJson is used there with fixed-capacity StaticJsonDocument (320 bytes outgoing, 384 bytes incoming), preventing uncontrolled heap growth and reducing fragmentation risk. Parse errors are explicitly checked via DeserializationError. Only one expected field path is extracted for control action, limiting attack surface from oversized or malformed payloads. The sensor node itself parses no JSON at all any more -- its two payloads (SensorPacket out, ActuatorCommand in) are fixed-size C structs copied with memcpy and a length check, which removes JSON parsing from that board's attack surface entirely.

5. Reliability: What happens to the physical heaters/fans if the Wi-Fi drops, the Python backend crashes, or a board goes offline?
- Answer:
  - The chain has three links now (sensor-to-master ESP-NOW, master-to-Django HTTP, master-to-sensor ESP-NOW), and a break at any one of them has the same net effect: the sensor node's applyActuators() is not called, so previous actuator duty values are held -- deterministic hold-last-state behavior, unchanged in spirit from the original single-board design. Concretely: if the master node is powered off, the sensor node keeps sampling and sending, but esp_now_send() calls will simply fail to reach a live peer and nothing comes back; if Django is down, the master's postToDjango() returns false and no ESP-NOW relay is sent; if WiFi drops on either board, ensureWifi() retries independently on each. In production, a watchdog-safe policy can be added (for example, fail-safe fan ON after N consecutive failures) depending on farm safety requirements -- not implemented here on either board.

6. Sensors: How do you handle sensor noise, erratic analog readings, or floating inputs?
- Answer:
  - DHT errors are detected through NAN and discarded for that tick. MQ analog noise is reduced by oversampling 16 reads with microsecond settling. Rail guards identify likely wiring/open-circuit conditions and return 0.0 instead of unstable math. Final PPM is clamped to [0,500], matching backend plausibility bounds.

7. Power & Hardware: What are the voltage levels (3.3V vs 5V) for your sensors and relays, and did you need logic level shifters?
- Answer:
  - ESP32 GPIO/ADC are 3.3V domain. GPIO34 ADC input must never exceed 3.3V; config comments explicitly require external scaling if MQ module output can approach 5V. MQ sensor supply Vc in model is 5.0V, but ADC pin sees conditioned voltage. Relay power stages for fan/heater are external modules and should include proper level compatibility and isolation as required by the chosen relay hardware.

8. Memory: How is RAM/Flash usage managed on these microcontrollers?
- Answer:
  - Sensor node: static/global objects throughout (no JSON documents at all now), fixed-size SensorPacket/ActuatorCommand structs, no dynamic allocation in the loop. Master node: the same StaticJsonDocument discipline the original design used for its HTTP leg, plus a large but fixed 220 KB tensor arena (kTensorArenaSize) for the TFLite Micro interpreter -- sized once at compile time, never grown at runtime, which is why the ESP32-S3 (more RAM than the classic ESP32) was chosen for this role rather than adding the model to the existing DevKit V1.

9. Payload Construction: How are continuous float/integer sensor values converted for transmission?
- Answer:
  - Sensor node -> master: no conversion at all -- a SensorPacket C struct is sent as raw bytes over ESP-NOW (memcpy on both ends), which is both simpler and smaller on the wire than JSON for a fixed, known schema. Master -> Django: values remain numeric in the JSON tree (doc[field] = float) and are serialized by ArduinoJson via serializeJson into a String body, exactly as the original single-board design did. No manual sprintf JSON framing is used on that leg, reducing formatting errors and type coercion issues; Django's decoder reads numeric JSON tokens directly into Python float validation.

10. Security: How could this system be secured in a production environment?
- Answer:
  - Two legs, two hardening stories. ESP-NOW (both directions) supports AES-CCMP encryption via esp_now_add_peer()'s peer.encrypt/peer.lmk fields -- currently disabled (peer.encrypt = false) since this is a private/trusted LAN deployment; enabling it and provisioning a shared LMK per peer pair would be the production step. Master -> Django HTTP should be replaced with HTTPS via WiFiClientSecure, pinning the server certificate or CA chain and enforcing TLS version/cipher policy, plus device authentication (API key or mTLS client cert), replay protection (timestamp/nonce + HMAC), and network segmentation (VLAN/firewall). Both boards should also move their compile-time WiFi/MAC constants out of version-controlled headers and into secure onboarding/NVS provisioning for a real fleet deployment.

### Additional Defense Notes for Viva
- Classification precedence is intentionally ammonia-dominant:
  - Backend classifier evaluates ammonia > 15.0 first, then heat+humidity conjunction, then low temperature. The master node's spike-risk prediction is a separate, advisory-only signal (predicted_spike_probability) and never itself changes this precedence or the resulting predicted_class.
- Firmware and backend state vocabularies are byte-aligned:
  - STATE_* constants in the sensor node's firmware, and the classification strings the master node relays, must remain identical to backend EnvironmentalState values.
- ESP-NOW packet structs must stay byte-identical across both boards:
  - SensorPacket and ActuatorCommand are defined independently in hardware/esp32/src/main.cpp and hardware/esp32s3_master/esp32s3_master.ino (no shared header, since they're built by two different toolchains). Changing field order/types in one without the other silently breaks the link -- onDataRecv()'s length check (len != sizeof(...)) will reject the mismatched packets outright, which at least fails loudly rather than silently misinterpreting bytes.
- Important current limitations to disclose clearly:
  - On repeated communication failures anywhere in the three-hop chain, actuator policy is hold-last-state, not explicit fail-safe override; safety strategy should be reviewed for deployment.
  - Single sensor node, single master node only -- see hardware/esp32s3_master/README.md's "Known limitations" for what multi-node support would require.
  - MAC addresses are paired manually at flash time (copy-paste from Serial Monitor output); there is no dynamic pairing/discovery handshake.
