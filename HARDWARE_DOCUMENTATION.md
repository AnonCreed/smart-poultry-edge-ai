# Hardware & Embedded Firmware Architecture Defense Guide

## 1. Stack Overview & Library Analysis
- Microcontroller Hardware: ESP32 DevKit V1 class board, selected via board = esp32dev in PlatformIO.
- Core Framework: PlatformIO build system with Arduino C++ framework on Espressif32.

### Build and Runtime Context
- MCU family: ESP32 (dual-core Xtensa, integrated Wi-Fi, hardware PWM via LEDC, ADC1/ADC2 blocks).
- Firmware environment: [env:esp32dev] in esp32/platformio.ini.
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
  - Mature timing-critical implementation for DHT22 one-wire style protocol.
  - Stable API, broad adoption, and good interoperability with Arduino ecosystem.
  - Lower integration risk versus writing custom bit-banged timing logic (which is error-prone under RTOS scheduling).
- How it operates inside the code:
  - Constructed once as static DHT dht(PIN_DHT_DATA, DHT22).
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

3. bblanchon/ArduinoJson @ ^7.0.4
- Which library is used:
  - JSON serialization/deserialization engine for request payload and response parsing.
- Why it was chosen over alternatives:
  - Deterministic static-document option (StaticJsonDocument) supports low-fragmentation embedded design.
  - Strong type-safe API and robust parse error reporting.
  - More memory-control-oriented than ad hoc String concatenation parsers.
- How it operates inside the code:
  - StaticJsonDocument<256> doc for outgoing payload.
  - serializeJson(doc, body) into HTTP POST body.
  - StaticJsonDocument<384> respDoc for server response.
  - deserializeJson(respDoc, http.getStream()) then extract record.predicted_class.

### Dependency Breakdown from include directives in main.cpp
1. Arduino.h
- Which library is used:
  - Arduino core API for ESP32 (timing, serial, GPIO, PWM wrapper functions).
- Why chosen:
  - Required base runtime for framework=arduino.
- How it operates inside the code:
  - Provides setup(), loop(), millis(), delay(), analogRead(), ledcSetup(), ledcWrite(), String, Serial.

2. WiFi.h
- Which library is used:
  - ESP32 Wi-Fi station client stack wrapper.
- Why chosen:
  - Native ESP32 Arduino networking API with reliable STA mode support.
- How it operates inside the code:
  - WiFi.mode(WIFI_STA), WiFi.setSleep(false), WiFi.begin(...), WiFi.status(), WiFi.disconnect(...), WiFi.localIP(), WiFi.RSSI().

3. HTTPClient.h
- Which library is used:
  - ESP32 HTTP client abstraction over TCP.
- Why chosen:
  - Minimal code footprint to implement REST POST/headers/timeouts.
  - Simpler than raw WiFiClient + manual HTTP framing.
- How it operates inside the code:
  - HTTPClient http; http.begin(base+path); http.addHeader(...); http.POST(body); http.getStream(); http.end().

4. ArduinoJson.h
- Used for deterministic JSON encode/decode as described above.

5. DHT.h
- Used to read DHT22 sensor values via DHT class.

6. math.h
- Which library is used:
  - Standard math functions.
- Why chosen:
  - Required for powf used in MQ-137 transfer function conversion.
- How it operates inside the code:
  - ppm = MQ137_A * powf(ratio, MQ137_B).

7. config.h (custom project header)
- Which header is used:
  - Firmware compile-time constants for networking, timing, pins, PWM, ADC calibration, MQ curve constants.
- Why chosen:
  - Separation of policy/constants from logic enables easier viva discussion, portability, and safer maintenance.
- How it operates inside the code:
  - Supplies all literal values referenced in setup/loop and helper functions.

### Custom C++ Source/Header Inventory
- Source files:
  - esp32/src/main.cpp
- Header files:
  - esp32/include/config.h
- No additional custom C++ compilation units were found in the PlatformIO workspace.

## 2. Complete GPIO & Circuit Pinout Table

| Component Name | Pin / GPIO # | Component Type (Sensor/Actuator/Power) | Signal Type (Analog / Digital / I2C / SPI) | Active State (HIGH/LOW) | Technical Purpose |
| :--- | :--- | :--- | :--- | :--- | :--- |
| DHT22 Data Line | GPIO4 (PIN_DHT_DATA) | Sensor | Digital single-wire style data | Idle HIGH via pull-up, data pulses active low/high timing encoded | Reads ambient temperature and humidity in sampleDht(). |
| MQ-137 Analog Output | GPIO34 (PIN_MQ_DATA, ADC1_CH6) | Sensor | Analog (ADC1, 12-bit, 11 dB attenuation) | N/A (continuous analog voltage) | Reads NH3 proxy voltage, oversampled then converted to PPM in mqCountsToPpm(). |
| Fan MOSFET Gate PWM | GPIO32 (PIN_PWM_FAN) | Actuator | Digital PWM (LEDC channel 0, 25 kHz, 8-bit) | Effective ON when duty > 0 (configured ON duty 220/255) | Drives ventilation fan for CRITICAL_AMMONIA and HEAT_STRESS_WARNING states. |
| Heater MOSFET Gate PWM | GPIO33 (PIN_PWM_HEATER) | Actuator | Digital PWM (LEDC channel 1, 25 kHz, 8-bit) | Effective ON when duty > 0 (configured ON duty 220/255) | Drives heater for LOW_TEMP_ALERT state. |
| USB-UART Serial (console) | UART0 default pins (board routed via USB bridge) | Debug/Interface | Digital UART | TX active-high line logic | Boot logs, diagnostics, telemetry printouts at 115200 baud. |

### Electrical and Pin-Selection Rationale
- GPIO34 is input-only and mapped to ADC1, which remains usable while Wi-Fi is active; this is critical because ADC2 is unavailable during active Wi-Fi on classic ESP32.
- PWM fan/heater on GPIO32/33 avoids strapping/conflict pins and supports LEDC hardware channels with independent duty control.
- DHT22 data line on GPIO4 is a common safe GPIO with external 10 kOhm pull-up assumption.

## 3. Communication & Data Transmission Pipeline
- Network Protocol:
  - Physical/link: IEEE 802.11 Wi-Fi in station mode.
  - Transport: TCP/IP.
  - Application: HTTP REST POST with JSON payload.
  - Endpoint: POST http://192.168.1.100:8000/api/telemetry/submit/.

### Client Request Flow (MCU -> Backend)
1. loop() calls ensureWifi() each cycle; if disconnected, firmware performs full reconnect sequence.
2. Sampling tick gate uses millis() against nextSampleAt; executes every SAMPLE_INTERVAL_MS (5000 ms).
3. sampleDht() obtains humidity and temperature; invalid NAN data aborts current tick.
4. sampleMq137() gathers 16 ADC samples on GPIO34 and averages.
5. mqCountsToPpm() converts averaged ADC counts to NH3 PPM via resistor-divider and power-law calibration curve.
6. edgeAiForecast() computes one-step extrapolated predicted values when ENABLE_EDGE_AI_STUB=1.
7. transmit() builds JSON document, serializes to String body, configures HTTPClient timeout, sets Content-Type: application/json, and posts body.
8. On HTTP 201 response, firmware deserializes response stream and extracts record.predicted_class.
9. loop() invokes applyActuators(state) using parsed classification.

### Payload Inspection (Exact Structure Sent by MCU)
Outgoing JSON fields from transmit():
- temperature (float)
- humidity (float)
- ammonia_level (float)
- predicted_temperature (float or null)
- predicted_ammonia (float or null)

Example payload when prediction is valid:
{
  "temperature": 30.25,
  "humidity": 62.10,
  "ammonia_level": 11.40,
  "predicted_temperature": 30.90,
  "predicted_ammonia": 12.00
}

Example payload when prediction disabled/invalid:
{
  "temperature": 30.25,
  "humidity": 62.10,
  "ammonia_level": 11.40,
  "predicted_temperature": null,
  "predicted_ammonia": null
}

### Server Response Parsing (Backend -> MCU)
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

### Fault Tolerance and Error Behavior
- Wi-Fi disconnection handling:
  - ensureWifi() checks WiFi.status().
  - If disconnected: WiFi.disconnect(true, true) then connectWifi().
  - connectWifi() retries up to WIFI_MAX_RETRIES (10) with WIFI_RETRY_MS (2000 ms).
- Timeout handling:
  - HTTP timeout set to HTTP_TIMEOUT_MS (4000 ms), bounding stall duration inside transmit().
- HTTP error handling:
  - Any status != 201 is treated as failure and logged.
  - No in-function retry; retry naturally occurs on next 5-second sampling tick.
- JSON parse failure handling:
  - deserializeJson error returns failure; actuator update skipped.
- Packet loss/server outage behavior:
  - POST failure causes no new state application.
  - Actuators remain at previous duty values (held state), because applyActuators() is called only on successful classified response.
- Sensor fault handling:
  - DHT NAN -> skip tick.
  - MQ rail/open-circuit voltage guard returns 0.0 ppm instead of inf/NaN propagation.
  - NH3 output clamped to [0, 500] ppm to align backend admissible range.

## 4. Execution Flow & Logic Breakdown

### Setup Initialization (setup())
1. Serial monitor configuration and baud:
  - Serial.begin(115200), then short delay(50), then boot log print.
2. Pin/peripheral initialization:
  - dht.begin() initializes DHT22 stack.
  - analogReadResolution(12) sets ADC to 12-bit.
  - analogSetAttenuation(ADC_11db) configures approximately 0..3.3V range.
3. PWM setup:
  - ledcSetup channel 0 and 1 at 25 kHz, 8-bit.
  - ledcAttachPin GPIO32->channel0, GPIO33->channel1.
  - Initial duty both set to 0 to avoid startup actuator glitch.
4. Wi-Fi connection loop and handling:
  - connectWifi() enters STA mode, disables modem sleep, begins association, retries with bounded loop.
5. Sensor warmup/stabilization:
  - No explicit timed warmup state machine; first sample can run immediately after setup via nextSampleAt = millis().
  - Practical note: MQ-137 typically needs burn-in and runtime thermal stabilization externally managed.

### Main Loop Mechanics (loop())
1. Non-blocking timing strategy:
  - Uses millis() schedule check:
    - if (int32_t(now - nextSampleAt) < 0) return.
  - This signed-delta pattern is rollover-safe across millis() wrap (~49.7 days).
2. Schedule advancement:
  - nextSampleAt = now + SAMPLE_INTERVAL_MS.
3. Sensor sequence:
  - Read DHT22 via sampleDht(); abort tick on failure.
  - Read MQ via sampleMq137() with oversampling and conversion.
4. Forecast sequence:
  - edgeAiForecast() computes one-step linear extrapolation from last sample state.
5. Network transmission sequence:
  - transmit() serializes JSON, posts to API, parses classification.
6. Actuator update sequence:
  - On successful classify response only, applyActuators(state).
  - Serial telemetry log prints live + predicted values + state.
7. Failure branch behavior:
  - Logs failure message and keeps previous actuator output unchanged.

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
  - Acquire valid DHT22 humidity and temperature reading pair.
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

### 5.6 static void edgeAiForecast(float tNow, float ppmNow, float& tPred, float& ppmPred, bool& valid)
- Function Signature: static void edgeAiForecast(float tNow, float ppmNow, float& tPred, float& ppmPred, bool& valid)
- Exact Job:
  - Provide one-tick-ahead forecast values with a compile-time enable switch.
- Step-by-Step Logic:
1. If ENABLE_EDGE_AI_STUB is enabled:
  - If historical values are NAN (cold start): set predictions equal to current values.
  - Else linear extrapolation:
    - tPred = tNow + (tNow - lastTemperature)
    - ppmPred = ppmNow + (ppmNow - lastAmmonia)
  - Update lastTemperature and lastAmmonia with current sample.
  - Set valid = true.
2. If stub disabled:
  - Mark inputs as intentionally unused.
  - Set valid = false (payload emits null predictions).

### 5.7 static bool transmit(float temperatureC, float humidityPct, float ammoniaPpm, float predTempC, float predAmmoniaPpm, bool predValid, String& outState)
- Function Signature: static bool transmit(float temperatureC, float humidityPct, float ammoniaPpm, float predTempC, float predAmmoniaPpm, bool predValid, String& outState)
- Exact Job:
  - Marshal payload, POST to API, parse response class, return success/failure and state string.
- Step-by-Step Logic:
1. Clear outState.
2. If Wi-Fi not connected, return false.
3. Build StaticJsonDocument<256> with required live fields.
4. Conditionally assign prediction fields:
  - Numeric values if predValid true.
  - null if predValid false.
5. Serialize JSON into String body.
6. Configure HTTPClient timeout and endpoint URL (API_BASE_URL + API_SUBMIT_PATH).
7. Add Content-Type header.
8. Execute POST and capture status code.
9. If code != 201:
  - Print error code and HTTP client message.
  - Close session with http.end().
  - Return false.
10. Parse response stream into StaticJsonDocument<384>.
11. Close session.
12. If parse error, print parse message and return false.
13. Extract outState = respDoc["record"]["predicted_class"] defaulting to empty.
14. Return true only if outState is non-empty.

### 5.8 static void applyActuators(const String& state)
- Function Signature: static void applyActuators(const String& state)
- Exact Job:
  - Enforce backend classification into mutually exclusive fan/heater actuation.
- Step-by-Step Logic:
1. Initialize fanDuty and heaterDuty to OFF.
2. If state is CRITICAL_AMMONIA or HEAT_STRESS_WARNING:
  - fanDuty = ON.
3. Else if state is LOW_TEMP_ALERT:
  - heaterDuty = ON.
4. For OPTIMAL_ENVIRONMENT or unknown state:
  - Both remain OFF.
5. Write duty values to LEDC fan/heater channels.

### 5.9 void setup()
- Function Signature: void setup()
- Exact Job:
  - One-time initialization of serial, sensors, ADC, PWM, Wi-Fi, and scheduler seed.
- Step-by-Step Logic:
1. Start serial at 115200.
2. Print boot banner.
3. Initialize DHT sensor.
4. Configure ADC resolution and attenuation.
5. Configure and attach LEDC channels/pins.
6. Force both actuator outputs OFF.
7. Establish Wi-Fi connection.
8. Seed nextSampleAt with current millis for immediate first cycle.

### 5.10 void loop()
- Function Signature: void loop()
- Exact Job:
  - Real-time cooperative control loop: maintain connectivity, sample sensors, transmit telemetry, and update actuators.
- Step-by-Step Logic:
1. ensureWifi() to maintain link.
2. Read now = millis().
3. If not yet sample time (signed delta negative), return quickly.
4. Schedule next sample time by adding SAMPLE_INTERVAL_MS.
5. Read DHT values; if invalid, abort tick.
6. Read ammonia ppm via sampleMq137().
7. Compute forecast values and validity flag.
8. Call transmit() with live + predicted channels.
9. If transmit successful:
  - applyActuators(parsed_state).
  - print formatted telemetry/classification line.
10. Else:
  - print failure line and retain current actuator state.

## 6. Evaluator Defense Guide: 10 Tough Questions Teachers Will Ask

1. Network: How does the board send data, and why use HTTP POST over GET or WebSockets?
- Answer:
  - The board uses Wi-Fi STA + TCP + HTTP POST to a REST endpoint. POST is correct semantically because each payload creates a new telemetry record server-side (non-idempotent create operation), while GET is intended for retrieval and risks caching/proxy behavior. WebSockets are optimal for continuous duplex streams but add connection lifecycle complexity and statefulness on both MCU and server; this system sends one compact sample every 5 s, so request/response POST is simpler, auditable, and sufficient.

2. Concurrency: Why is non-blocking millis timing used instead of delay(), and what happens if millis overflows after 49 days?
- Answer:
  - The control loop uses cooperative scheduling so connectivity checks and future compute tasks can run each loop iteration. The check uses signed subtraction: int32_t(now - nextSampleAt) < 0. That delta arithmetic is rollover-safe, so when millis wraps, relative ordering remains valid and scheduling continues correctly.

3. Circuitry: How do your digital outputs physically trigger relays to switch high-voltage fans and heaters?
- Answer:
  - Firmware outputs are PWM control signals on GPIO32/33 through LEDC channels. In hardware, these should drive MOSFET gate networks (with gate resistors already noted) or transistor relay-driver stages, not relay coils directly from GPIO. The logic layer provides duty command; power-stage isolation and flyback protection must be implemented in hardware for inductive loads.

4. Data Parsing: Which library parses the incoming server JSON, and how do you prevent buffer overflow attacks or low memory issues on the MCU?
- Answer:
  - ArduinoJson is used with fixed-capacity StaticJsonDocument (256 bytes outgoing, 384 bytes incoming), preventing uncontrolled heap growth and reducing fragmentation risk. Parse errors are explicitly checked via DeserializationError. Only one expected field path is extracted for control action, limiting attack surface from oversized or malformed payloads.

5. Reliability: What happens to the physical heaters/fans if the Wi-Fi drops or the Python backend crashes?
- Answer:
  - On link loss, ensureWifi() initiates reconnect. On POST failure/non-201/parse failure, applyActuators() is not called, so previous actuator duty values are held. This is deterministic hold-last-state behavior. In production, a watchdog-safe policy can be added (for example, fail-safe fan ON after N consecutive failures) depending on farm safety requirements.

6. Sensors: How do you handle sensor noise, erratic analog readings, or floating inputs?
- Answer:
  - DHT errors are detected through NAN and discarded for that tick. MQ analog noise is reduced by oversampling 16 reads with microsecond settling. Rail guards identify likely wiring/open-circuit conditions and return 0.0 instead of unstable math. Final PPM is clamped to [0,500], matching backend plausibility bounds.

7. Power & Hardware: What are the voltage levels (3.3V vs 5V) for your sensors and relays, and did you need logic level shifters?
- Answer:
  - ESP32 GPIO/ADC are 3.3V domain. GPIO34 ADC input must never exceed 3.3V; config comments explicitly require external scaling if MQ module output can approach 5V. MQ sensor supply Vc in model is 5.0V, but ADC pin sees conditioned voltage. Relay/MOSFET power stages for fan/heater are external and should include proper level compatibility and isolation as required by chosen modules.

8. Memory: How is RAM/Flash usage managed on this microcontroller?
- Answer:
  - Key design choices are static/global objects and static JSON documents to avoid repeated dynamic allocation in the loop. Payload size is constrained. HTTP transaction object is stack-local and cleaned by http.end(). Forecast history stores only two floats. This keeps per-tick memory deterministic and fragmentation risk low.

9. Payload Construction: How are continuous float/integer sensor values converted into strings or JSON packets for transmission?
- Answer:
  - Values remain numeric in the JSON tree (doc[field] = float) and are serialized by ArduinoJson via serializeJson into a String body. No manual sprintf JSON framing is used, reducing formatting errors and type coercion issues. Server-side Django decoder reads numeric JSON tokens directly into Python float validation.

10. Security: How could this HTTP transmission be secured in a production environment (for example, HTTPS/TLS)?
- Answer:
  - Replace HTTPClient endpoint with HTTPS and a WiFiClientSecure transport, pin server certificate or CA chain, and enforce TLS version/cipher policy. Add device authentication (API key or mTLS client cert), replay protection (timestamp/nonce + HMAC), and network segmentation (VLAN/firewall). Also remove plaintext credentials from compile-time header and provision secrets through secure onboarding/NVS.

### Additional Defense Notes for Viva
- Classification precedence is intentionally ammonia-dominant:
  - Backend classifier evaluates ammonia > 25.0 first, then heat+humidity conjunction, then low temperature.
- Firmware and backend state vocabularies are byte-aligned:
  - STATE_* constants in firmware must remain identical to backend EnvironmentalState values.
- Important current limitation to disclose clearly:
  - On repeated communication failures, actuator policy is hold-last-state, not explicit fail-safe override; safety strategy should be reviewed for deployment.
