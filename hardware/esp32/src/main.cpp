// ============================================================================
// main.cpp -- Poultry Telemetry ESP32 sensor-node firmware.
//
// Responsibilities, in order of execution priority:
//   1. Maintain WiFi association; reconnect with linear back-off on drop.
//      (Needed only for channel alignment with the ESP32-S3 master and for
//      NTP time -- see the network note in config.h. This node no longer
//      speaks HTTP to Django; the master owns that leg.)
//   2. Sample DHT11 (temperature, humidity) and MQ-137 (ammonia PPM) on a
//      fixed 5 s cadence.
//   3. Send a compact binary SensorPacket over ESP-NOW to the ESP32-S3
//      master node, which runs the TFLite Micro forecast/spike model and
//      forwards the combined record to Django.
//   4. Receive fan_pwm/heater_pwm bytes back from the master over ESP-NOW
//      (both fully computed there, including any dashboard MANUAL override)
//      and apply them: fan (PWM speed) and PTC heater (ON/OFF relay --
//      see HEATER_RELAY_ON/OFF in config.h for this board's polarity),
//      closing the environmental control loop end-to-end across two boards.
//   5. Drive a 16x2 I2C LCD (RG1602A-IIC(P), PCF8574T backpack, SDA=GPIO21/
//      SCL=GPIO22) showing this board's own current temperature/humidity/
//      ammonia reading -- current data only, no forecast, no actuation
//      depends on it.
//
// BENCHMARK_MODE (compile-time only, see the esp32dev_benchmark PlatformIO
// environment in platformio.ini): replaces step 2's real DHT11/MQ-137 reads
// with a scripted sequence of scenarios, for exercising the master's
// predictive model + Django's classifier + the real actuator response
// on-demand and repeatably. Everything else in the pipeline (ESP-NOW, real
// on-device inference, real classification, real actuation) stays genuine
// -- only the sensor read itself is synthetic. See sampleBenchmark().
//
// The main loop is non-blocking; all timing is millis()-based.
// ============================================================================

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <time.h>
#include <DHT.h>
#include <math.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

#include "config.h"

// ----------------------------------------------------------------------------
// Peripheral objects
// ----------------------------------------------------------------------------
static DHT dht(PIN_DHT_DATA, DHT11);
static LiquidCrystal_I2C lcd(LCD_I2C_ADDR, LCD_COLS, LCD_ROWS);

// Millis() timestamp of the last completed sample cycle. Deliberately
// separate from FreeRTOS timers so the schedule survives temporary WiFi
// stalls without drift accumulating on the reconnect side.
static uint32_t nextSampleAt = 0;
static uint32_t txSeq = 0;

// ----------------------------------------------------------------------------
// Wire contract shared with esp32s3_master/esp32s3_inference_receiver.ino.
// Field order/types must stay byte-identical on both sides -- this struct
// is copied directly out of the ESP-NOW payload with memcpy, no serializer.
// ----------------------------------------------------------------------------
struct SensorPacket {
    uint32_t seq;
    float temperature;
    float humidity;
    float ammonia_ppm;
    uint8_t hour;   // 0-23, local time
    uint8_t month;  // 1-12
};

// Classification vocabulary the master relays back -- must remain
// byte-identical to the Django telemetry.EnvironmentalState enum.
//
// fan_pwm is a 0-255 PWM duty target computed on the master from its
// MLP-predicted NH3 error (or a dashboard MANUAL override %) -- applied to
// the fan verbatim. heater_pwm is a 0-255 byte computed on the master from
// the classification (LOW_TEMP_ALERT -> 255, else 0), or a dashboard MANUAL
// override -- this board's heater is a plain relay, so it's only ever
// thresholded at >0. `state` itself is no longer used for actuation on this
// board (kept for logging/diagnostics) -- both fan and heater decisions are
// now fully computed on the master and just applied here. See
// applyActuators().
struct ActuatorCommand {
    char state[24];
    uint8_t fan_pwm;
    uint8_t heater_pwm;  // NEW
    uint8_t has_prediction;       // NEW: 1 once predicted_temperature/
                                   // predicted_ammonia below are valid (the
                                   // master's model has warmed up), 0 during
                                   // its ~30-40s post-boot warm-up window.
    float predicted_temperature;  // NEW: display-only, shown on the LCD's
                                   // "Predicted" screen -- not used for
                                   // actuation.
    float predicted_ammonia;      // NEW: display-only, same as above.
};

static uint8_t masterMac[6] = MASTER_MAC_ADDR;

// Latest actuator command from the master, handed off from the ESP-NOW recv
// callback (runs in the WiFi task context) to the main loop via a critical
// section -- same pattern the master firmware uses for its own inbound path.
static ActuatorCommand g_lastCommand;
static volatile bool g_newCommand = false;
static portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;

// Last-known wall-clock fields, used as a fallback if NTP sync fails so the
// model's cyclic features stay in a plausible (not pathological) range.
static uint8_t lastHour = 12;
static uint8_t lastMonth = 6;

// ----------------------------------------------------------------------------
// LCD display state
// ----------------------------------------------------------------------------
// Latest live reading the LCD renders -- current data only, no forecast, no
// rotation. Kept separate from the ephemeral locals in loop() so updateLcd()
// doesn't need extra parameters threaded through.
static float g_lcdTemperature = 0.0f;
static float g_lcdHumidity    = 0.0f;
static float g_lcdAmmonia     = 0.0f;
static bool  g_haveReading    = false;

// ============================================================================
// WiFi lifecycle
// ============================================================================
static void connectWifi() {
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);  // Latency over power savings for control loop.
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    Serial.printf("[WIFI] Associating with SSID '%s'\n", WIFI_SSID);
    for (int attempt = 0; attempt < WIFI_MAX_RETRIES; ++attempt) {
        if (WiFi.status() == WL_CONNECTED) {
            Serial.printf("[WIFI] Link up. IP=%s RSSI=%d dBm Channel=%d\n",
                          WiFi.localIP().toString().c_str(), WiFi.RSSI(), WiFi.channel());
            return;
        }
        delay(WIFI_RETRY_MS);
        Serial.printf("[WIFI] retry %d/%d\n", attempt + 1, WIFI_MAX_RETRIES);
    }
    Serial.println("[WIFI] Association failed; will retry on next tick.");
}

static void ensureWifi() {
    if (WiFi.status() == WL_CONNECTED) return;
    Serial.println("[WIFI] Link lost -- reconnecting.");
    WiFi.disconnect(true, true);
    connectWifi();
}

/**
 * Sync wall-clock time over NTP. Only meaningful while WiFi is associated;
 * called once at boot and again on every reconnect since a dropped link may
 * have let the RTC drift uncorrected for a while.
 */
static void syncNtpTime() {
    configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER);
    struct tm timeinfo;
    if (getLocalTime(&timeinfo, NTP_SYNC_TIMEOUT_MS)) {
        Serial.printf("[NTP] Synced: %04d-%02d-%02d %02d:%02d:%02d\n",
                      timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                      timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    } else {
        Serial.println("[NTP] Sync failed -- hour/month features will use last-known/default values.");
    }
}

/**
 * Read current local hour/month for the model's cyclic features. Falls back
 * to the last successfully-read values (seeded to a neutral noon/June
 * default) rather than propagating a hard failure, since sensor sampling
 * must not stall on a clock read.
 */
static void currentHourMonth(uint8_t& hour, uint8_t& month) {
    struct tm timeinfo;
    if (getLocalTime(&timeinfo, 0)) {
        lastHour = static_cast<uint8_t>(timeinfo.tm_hour);
        lastMonth = static_cast<uint8_t>(timeinfo.tm_mon + 1);
    }
    hour = lastHour;
    month = lastMonth;
}

// ============================================================================
// Sensor sampling
// ============================================================================

/**
 * Read DHT11. The library returns NAN on checksum failure or if polled
 * faster than the sensor's 500 ms internal refresh; both cases signal an
 * invalid tick to the caller.
 */
static bool sampleDht(float& temperatureC, float& humidityPct) {
    humidityPct  = dht.readHumidity();
    temperatureC = dht.readTemperature();  // Celsius by default.
    if (isnan(humidityPct) || isnan(temperatureC)) {
        Serial.println("[DHT] Read failed -- checksum or timing violation.");
        return false;
    }
    return true;
}

/**
 * Convert an averaged MQ-137 ADC reading to NH3 PPM using the datasheet
 * characteristic curve. The intermediate Rs value is derived from the
 * sensor's resistor divider; PPM comes from a power-law fit of the log-log
 * NH3 curve. Numerical guards clamp the output to a plausible envelope so
 * a disconnected sensor floats to a diagnosable rail rather than a random
 * huge number.
 */
static float mqCountsToPpm(uint32_t sumCounts, uint16_t samples) {
    const float avgCounts = static_cast<float>(sumCounts) / samples;
    const float vOut = (avgCounts / ADC_MAX_COUNTS) * ADC_MAX_VOLTS;

    // Open-circuit / rail conditions: return a diagnostic zero rather than
    // dividing by a near-zero denominator and producing an inf.
    if (vOut < 0.05f || vOut > (MQ137_VC_VOLTS - 0.05f)) {
        return 0.0f;
    }

    const float rs    = MQ137_RL_KOHMS * (MQ137_VC_VOLTS - vOut) / vOut;
    const float ratio = rs / MQ137_R0_KOHMS;
    float ppm = MQ137_A * powf(ratio, MQ137_B);

    if (ppm < 0.0f)   ppm = 0.0f;
    if (ppm > 500.0f) ppm = 500.0f;  // Backend ingestion envelope ceiling.
    return ppm;
}

static float sampleMq137() {
    uint32_t sum = 0;
    for (uint16_t i = 0; i < MQ_ADC_SAMPLES; ++i) {
        sum += analogRead(PIN_MQ_DATA);
        delayMicroseconds(200);  // ADC settling; shorter than any RTOS tick.
    }
    return mqCountsToPpm(sum, MQ_ADC_SAMPLES);
}

#ifdef BENCHMARK_MODE
// ============================================================================
// Benchmark mode -- synthetic sensor data (see the esp32dev_benchmark
// PlatformIO environment in platformio.ini). Replaces sampleDht()/
// sampleMq137() with a scripted sequence of scenarios so the master's
// on-device predictive model, Django's classifier, and the actuator
// response can be exercised on-demand and repeatably, without a physical
// sensor attached or waiting on real environmental drift. Only the sensor
// READ is synthetic -- everything downstream (ESP-NOW, real TFLite
// inference on the master, real classification, real actuator relay) is
// the genuine end-to-end pipeline.
//
// This is a *compile-time-only* mode: BENCHMARK_MODE is set via a
// dedicated PlatformIO environment, never via a runtime toggle, so a board
// can never accidentally start reporting fake data without an explicit
// reflash -- and the boot banner/LCD both say so loudly if it is.
// ============================================================================
struct BenchmarkScenario {
    const char* name;
    float tempStart,  tempEnd;    // degC -- linearly interpolated across the scenario
    float humidStart, humidEnd;   // %RH
    float nh3Start,   nh3End;     // ppm
};

// A constant scenario (Start == End) held back-to-back with a different
// constant next scenario produces a step transition between them for free.
// A scenario with Start != End is a ramp within itself -- useful for
// testing predictive lead-time (does predicted_ammonia rise before the
// live reading actually crosses the classifier's threshold?).
static const BenchmarkScenario BENCHMARK_SCENARIOS[] = {
    // name                        tempStart tempEnd  humidStart humidEnd  nh3Start nh3End
    {"OPTIMAL_BASELINE",           24.0f,    24.0f,   55.0f,     55.0f,     3.0f,    3.0f},   // warm-up baseline
    {"AMMONIA_STEP_CRITICAL",      24.0f,    24.0f,   55.0f,     55.0f,    40.0f,   40.0f},   // instant jump past the 25ppm threshold
    {"AMMONIA_STEP_RECOVER",       24.0f,    24.0f,   55.0f,     55.0f,     3.0f,    3.0f},   // instant drop back to baseline
    {"AMMONIA_RAMP_TO_SPIKE",      24.0f,    24.0f,   55.0f,     55.0f,     3.0f,   30.0f},   // gradual rise through the threshold
    {"AMMONIA_RAMP_RECOVER",       24.0f,    24.0f,   55.0f,     55.0f,    30.0f,    3.0f},   // gradual fall back down
    {"AMMONIA_BOUNDARY_LOW",       24.0f,    24.0f,   55.0f,     55.0f,    24.9f,   24.9f},   // just under AMMONIA_CRITICAL_PPM
    {"AMMONIA_BOUNDARY_HIGH",      24.0f,    24.0f,   55.0f,     55.0f,    25.1f,   25.1f},   // just over it
    {"HEAT_STRESS_STEP",           38.0f,    38.0f,   80.0f,     80.0f,     3.0f,    3.0f},   // T + RH combined, both past threshold
    {"HEAT_STRESS_RECOVER",        24.0f,    24.0f,   55.0f,     55.0f,     3.0f,    3.0f},
    {"LOW_TEMP_STEP",              15.0f,    15.0f,   55.0f,     55.0f,     3.0f,    3.0f},   // heater reactivity
    {"LOW_TEMP_RECOVER",           24.0f,    24.0f,   55.0f,     55.0f,     3.0f,    3.0f},
};
static const size_t BENCHMARK_SCENARIO_COUNT =
    sizeof(BENCHMARK_SCENARIOS) / sizeof(BENCHMARK_SCENARIOS[0]);

static size_t   benchmarkScenarioIdx      = 0;
static uint32_t benchmarkSampleInScenario = 0;

/**
 * Produce the next synthetic reading: linearly interpolates within the
 * current scenario across BENCHMARK_SAMPLES_PER_SCENARIO samples, then
 * advances to the next scenario (wrapping around at the end of the list).
 * Logs which scenario/sample is active on every call so live serial output
 * can be correlated against the master's [MODEL]/[ACTUATE] lines and the
 * dashboard's classification.
 */
static void sampleBenchmark(float& temperatureC, float& humidityPct, float& ammoniaPpm) {
    const BenchmarkScenario& s = BENCHMARK_SCENARIOS[benchmarkScenarioIdx];
    const float frac = (BENCHMARK_SAMPLES_PER_SCENARIO <= 1)
        ? 1.0f
        : (float)benchmarkSampleInScenario / (float)(BENCHMARK_SAMPLES_PER_SCENARIO - 1);

    temperatureC = s.tempStart  + (s.tempEnd  - s.tempStart)  * frac;
    humidityPct  = s.humidStart + (s.humidEnd - s.humidStart) * frac;
    ammoniaPpm   = s.nh3Start   + (s.nh3End   - s.nh3Start)   * frac;

    Serial.printf("[BENCHMARK] scenario=%s (%u/%u)  T=%.1fC RH=%.1f%% NH3=%.1fppm\n",
                  s.name, (unsigned)(benchmarkSampleInScenario + 1),
                  (unsigned)BENCHMARK_SAMPLES_PER_SCENARIO,
                  temperatureC, humidityPct, ammoniaPpm);

    if (++benchmarkSampleInScenario >= BENCHMARK_SAMPLES_PER_SCENARIO) {
        benchmarkSampleInScenario = 0;
        benchmarkScenarioIdx = (benchmarkScenarioIdx + 1) % BENCHMARK_SCENARIO_COUNT;
        Serial.printf("[BENCHMARK] -- advancing to scenario '%s' --\n",
                      BENCHMARK_SCENARIOS[benchmarkScenarioIdx].name);
    }
}
#endif  // BENCHMARK_MODE

// ============================================================================
// LCD display -- I2C 16x2, static current-reading readout (no rotation, no
// forecast -- this board's LCD shows only its own live sensor data).
// ============================================================================

/**
 * Print `text` on one LCD row, space-padded to LCD_COLS so a shorter string
 * fully overwrites whatever longer string was there before -- the PCF8574T
 * backpack has no clear-to-end-of-line primitive, so a naive lcd.print()
 * would leave trailing characters from the previous line behind. Width comes
 * from LCD_COLS itself (via the "%-*s" runtime-width specifier) rather than
 * a hardcoded literal, so this stays correct if LCD_COLS is ever changed.
 */
static void lcdPrintRow(uint8_t row, const char* text) {
    char buf[LCD_COLS + 1];
    snprintf(buf, sizeof(buf), "%-*s", LCD_COLS, text);
    buf[LCD_COLS] = '\0';  // truncate defensively if text ran long
    lcd.setCursor(0, row);
    lcd.print(buf);
}

/**
 * Redraw both rows from the latest live reading: temperature+humidity on
 * row 0, ammonia on row 1. Called once at boot (shows a placeholder) and
 * again every time a fresh DHT11/MQ-137 sample completes.
 */
static void updateLcd() {
    char line0[LCD_COLS + 1];
    char line1[LCD_COLS + 1];

    if (g_haveReading) {
        snprintf(line0, sizeof(line0), "T:%.1fC H:%.0f%%", g_lcdTemperature, g_lcdHumidity);
        snprintf(line1, sizeof(line1), "NH3: %.1f ppm", g_lcdAmmonia);
    } else {
        snprintf(line0, sizeof(line0), "Poultry Telem.");
        snprintf(line1, sizeof(line1), "No data yet");
    }

    lcdPrintRow(0, line0);
    lcdPrintRow(1, line1);
}

// ============================================================================
// ESP-NOW transport
// ============================================================================

/**
 * Fired by the ESP-NOW stack (WiFi task context) once the previous send
 * completes. Diagnostic only -- failures are logged, not retried, since the
 * next 5 s tick naturally supersedes a stale sample.
 */
static void onDataSent(const uint8_t* /*mac*/, esp_now_send_status_t status) {
    if (status != ESP_NOW_SEND_SUCCESS) {
        Serial.println("[ESPNOW] Delivery to master failed.");
    }
}

/**
 * Fired by the ESP-NOW stack when the master relays back a classification.
 * Runs in the WiFi task context, so the payload is copied out under a
 * critical section for the main loop to consume -- mirrors the pattern the
 * master firmware uses for its own inbound sensor packets.
 */
static void onDataRecv(const uint8_t* /*mac*/, const uint8_t* data, int len) {
    if (len != sizeof(ActuatorCommand)) return;

    portENTER_CRITICAL_ISR(&g_mux);
    memcpy(&g_lastCommand, data, sizeof(ActuatorCommand));
    g_newCommand = true;
    portEXIT_CRITICAL_ISR(&g_mux);
}

static bool initEspNow() {
    if (esp_now_init() != ESP_OK) {
        Serial.println("[ESPNOW] Init failed.");
        return false;
    }
    esp_now_register_send_cb(onDataSent);
    esp_now_register_recv_cb(onDataRecv);

    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, masterMac, 6);
    peer.channel = 0;  // 0 == use the current WiFi STA channel.
    peer.encrypt = false;
    if (esp_now_add_peer(&peer) != ESP_OK) {
        Serial.println("[ESPNOW] Failed to register master as peer.");
        return false;
    }

    Serial.printf("[ESPNOW] Ready. Local MAC=%s  Master peer=%02X:%02X:%02X:%02X:%02X:%02X\n",
                  WiFi.macAddress().c_str(),
                  masterMac[0], masterMac[1], masterMac[2],
                  masterMac[3], masterMac[4], masterMac[5]);
    return true;
}

/** Send one sample to the master. Returns false only on a local send error. */
static bool sendSample(float temperatureC, float humidityPct, float ammoniaPpm) {
    SensorPacket packet{};
    packet.seq = txSeq++;
    packet.temperature = temperatureC;
    packet.humidity = humidityPct;
    packet.ammonia_ppm = ammoniaPpm;
    currentHourMonth(packet.hour, packet.month);

    const esp_err_t result = esp_now_send(
        masterMac, reinterpret_cast<const uint8_t*>(&packet), sizeof(packet)
    );
    return result == ESP_OK;
}

// ============================================================================
// Actuator control -- closes the environmental loop
// ============================================================================

/**
 * Apply the master-computed fan_pwm/heater_pwm bytes to the fan/heater
 * outputs. Both decisions (including the heater's classification check and
 * any dashboard MANUAL override) are made entirely on the master now -- this
 * board just applies whatever it's told, with no local reference to `state`:
 *   fan_pwm    -> fan PWM duty (proportional, real speed control)
 *   heater_pwm -> heater relay ON iff nonzero, OFF otherwise (see
 *                 HEATER_RELAY_ON/OFF in config.h for this board's relay
 *                 polarity -- no speed target either way)
 *
 * NOTE: fan and heater are NOT interlocked -- both bytes are independent, so
 * e.g. a nonzero MANUAL fan override alongside a nonzero heater_pwm (MANUAL
 * or LOW_TEMP_ALERT-driven) will run the fan and energize the heater relay
 * at once. If that combination is unsafe for the physical setup, enforce it
 * explicitly (either server-side, or by forcing fan_pwm to 0 here whenever
 * heaterOn).
 */
static void applyActuators(uint8_t fan_pwm, uint8_t heater_pwm) {
    const bool heaterOn = (heater_pwm > 0);
    digitalWrite(PIN_HEATER_RELAY, heaterOn ? HEATER_RELAY_ON : HEATER_RELAY_OFF);

    // Hard power cutoff, independent of the PWM duty's own speed-target
    // semantics (see PIN_FAN_ENABLE comment in config.h). Gates the fan's
    // actual power; PWM_FAN_CHANNEL below still carries the speed target.
    digitalWrite(PIN_FAN_ENABLE, fan_pwm > 0 ? HIGH : LOW);
    ledcWrite(PWM_FAN_CHANNEL, fan_pwm);
}

// ============================================================================
// Arduino entrypoints
// ============================================================================
void setup() {
    Serial.begin(115200);
    delay(50);
    Serial.println("\n[BOOT] Poultry Telemetry sensor node starting.");
#ifdef BENCHMARK_MODE
    Serial.println("[BOOT] *** BENCHMARK MODE -- synthetic scripted sensor data, NOT real readings ***");
#endif

    // Sensors
    dht.begin();
    analogReadResolution(12);
    analogSetAttenuation(ADC_11db);  // 0..3.3 V range on GPIO34.

    // Fan PWM peripheral. Attach BEFORE writing duty so the first control
    // cycle doesn't glitch the MOSFET gate high.
    ledcSetup(PWM_FAN_CHANNEL, PWM_FREQ_HZ, PWM_RESOLUTION_BITS);
    ledcAttachPin(PIN_PWM_FAN, PWM_FAN_CHANNEL);
    ledcWrite(PWM_FAN_CHANNEL, PWM_DUTY_OFF);

    // Fan hard-cutoff pin -- set LOW (fan de-energized) before anything
    // else runs, same "don't glitch on" precaution as the PWM line above.
    pinMode(PIN_FAN_ENABLE, OUTPUT);
    digitalWrite(PIN_FAN_ENABLE, LOW);

    // Heater relay -- see HEATER_RELAY_ON/OFF in config.h for this board's
    // polarity. Set before anything else runs so the relay doesn't glitch
    // energized on boot.
    pinMode(PIN_HEATER_RELAY, OUTPUT);
    digitalWrite(PIN_HEATER_RELAY, HEATER_RELAY_OFF);

    // LCD -- explicit Wire.begin(SDA, SCL) rather than relying on the
    // board's default I2C pins, so this stays correct even on a DevKit
    // variant whose defaults differ from GPIO21/22.
    Wire.begin(PIN_LCD_SDA, PIN_LCD_SCL);
    lcd.init();
    lcd.backlight();
#ifdef BENCHMARK_MODE
    lcdPrintRow(0, "** BENCHMARK **");
    lcdPrintRow(1, "Fake data!");
    delay(1500);  // Long enough to actually read before it starts cycling.
#else
    lcdPrintRow(0, "Poultry Telem.");
    lcdPrintRow(1, "Booting...");
#endif

    connectWifi();
    syncNtpTime();

    if (!initEspNow()) {
        Serial.println("[BOOT] ESP-NOW bring-up failed; halting.");
        lcdPrintRow(0, "ESP-NOW init");
        lcdPrintRow(1, "FAILED");
        while (true) delay(1000);
    }

    nextSampleAt = millis();  // First tick fires immediately.
}

void loop() {
    ensureWifi();

    // Apply any actuator command relayed back from the master since the
    // last loop iteration, independent of the sampling cadence below.
    if (g_newCommand) {
        ActuatorCommand cmd;
        portENTER_CRITICAL(&g_mux);
        cmd = g_lastCommand;
        g_newCommand = false;
        portEXIT_CRITICAL(&g_mux);

        applyActuators(cmd.fan_pwm, cmd.heater_pwm);
        Serial.printf("[RX] Master classification: %s  fan_pwm=%u  heater_pwm=%u\n",
                      cmd.state, cmd.fan_pwm, cmd.heater_pwm);
        // cmd.has_prediction/predicted_temperature/predicted_ammonia (the
        // master's forecast) are still received here but intentionally
        // unused -- this board's LCD shows its own current reading only.
    }

    const uint32_t now = millis();
    if (static_cast<int32_t>(now - nextSampleAt) < 0) {
        return;  // Not time yet; cooperatively yield.
    }
    nextSampleAt = now + SAMPLE_INTERVAL_MS;

    // -- Sample --------------------------------------------------------------
    float temperatureC = 0.0f, humidityPct = 0.0f, ammoniaPpm = 0.0f;
#ifdef BENCHMARK_MODE
    sampleBenchmark(temperatureC, humidityPct, ammoniaPpm);
#else
    if (!sampleDht(temperatureC, humidityPct)) {
        return;  // Skip tick on invalid DHT read; next tick retries.
    }
    ammoniaPpm = sampleMq137();
#endif

    g_lcdTemperature = temperatureC;
    g_lcdHumidity = humidityPct;
    g_lcdAmmonia = ammoniaPpm;
    g_haveReading = true;
    updateLcd();

    // -- Transmit over ESP-NOW -------------------------------------------------
    if (sendSample(temperatureC, humidityPct, ammoniaPpm)) {
        Serial.printf("[TX] T=%.2fC RH=%.2f%% NH3=%.2fppm -> master\n",
                      temperatureC, humidityPct, ammoniaPpm);
    } else {
        Serial.println("[TX] ESP-NOW send failed; actuators held at last state.");
    }
}
