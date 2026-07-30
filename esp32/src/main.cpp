// ============================================================================
// main.cpp -- Poultry Telemetry ESP32 edge-node firmware.
//
// Responsibilities, in order of execution priority:
//   1. Maintain WiFi association; reconnect with linear back-off on drop.
//   2. Sample DHT22 (temperature, humidity) and MQ-137 (ammonia PPM) on a
//      fixed 5 s cadence matching the dashboard's polling interval.
//   3. Serialize a JSON payload conforming to the Django ingestion contract
//      and POST it to /api/telemetry/submit/.
//   4. Parse the server's classification response and drive the PWM_FAN /
//      PWM_HEATER outputs accordingly, closing the environmental control
//      loop end-to-end.
//
// The main loop is non-blocking; all timing is millis()-based so WiFi
// housekeeping and future TFLite Micro inference can share the CPU without
// starving each other.
// ============================================================================

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <DHT.h>
#include <math.h>

#include "config.h"

// ----------------------------------------------------------------------------
// Peripheral objects
// ----------------------------------------------------------------------------
static DHT dht(PIN_DHT_DATA, DHT22);

// Rolling state used by the Edge-AI forecast stub. Held in RAM only; a
// reboot re-seeds from the first fresh reading, so no NVS churn.
static float lastTemperature = NAN;
static float lastAmmonia     = NAN;

// Millis() timestamp of the last completed sample cycle. Deliberately
// separate from FreeRTOS timers so the schedule survives temporary WiFi
// stalls without drift accumulating on the reconnect side.
static uint32_t nextSampleAt = 0;

// ----------------------------------------------------------------------------
// Classification vocabulary -- must remain byte-identical to the Django
// telemetry.EnvironmentalState enum. Adding a state anywhere requires a
// coordinated update across three surfaces: model, backend classifier,
// and this actuator switch.
// ----------------------------------------------------------------------------
static constexpr const char* STATE_OPTIMAL  = "OPTIMAL_ENVIRONMENT";
static constexpr const char* STATE_HEAT     = "HEAT_STRESS_WARNING";
static constexpr const char* STATE_LOW_TEMP = "LOW_TEMP_ALERT";
static constexpr const char* STATE_CRITICAL = "CRITICAL_AMMONIA";

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
            Serial.printf("[WIFI] Link up. IP=%s RSSI=%d dBm\n",
                          WiFi.localIP().toString().c_str(), WiFi.RSSI());
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

// ============================================================================
// Sensor sampling
// ============================================================================

/**
 * Read DHT22. The library returns NAN on checksum failure or if polled
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

// ============================================================================
// Edge-AI forecast stub
// ============================================================================

/**
 * One-tick-ahead linear extrapolation. Placeholder for the TFLite Micro
 * regressor that will run on-device once the model is trained. The payload
 * shape does not change when the real model replaces this function, so the
 * dashboard and backend need zero updates at that time.
 */
static void edgeAiForecast(float tNow, float ppmNow,
                           float& tPred, float& ppmPred, bool& valid) {
#if ENABLE_EDGE_AI_STUB
    if (isnan(lastTemperature) || isnan(lastAmmonia)) {
        // Cold start: no delta available -- forecast equals current.
        tPred   = tNow;
        ppmPred = ppmNow;
    } else {
        tPred   = tNow   + (tNow   - lastTemperature);
        ppmPred = ppmNow + (ppmNow - lastAmmonia);
    }
    lastTemperature = tNow;
    lastAmmonia     = ppmNow;
    valid = true;
#else
    (void)tNow; (void)ppmNow; (void)tPred; (void)ppmPred;
    valid = false;
#endif
}

// ============================================================================
// HTTP transmit + response handling
// ============================================================================

/**
 * Build payload, POST, and if the server accepts, apply the returned
 * classification to the actuator PWM outputs. Returns the parsed
 * classification string (into `outState`) or an empty string on any failure.
 */
static bool transmit(float temperatureC, float humidityPct, float ammoniaPpm,
                     float predTempC, float predAmmoniaPpm, bool predValid,
                     String& outState) {
    outState = "";
    if (WiFi.status() != WL_CONNECTED) return false;

    // Serialize payload. StaticJsonDocument avoids heap fragmentation on
    // constrained MCUs; 256 B is comfortably above worst-case size.
    StaticJsonDocument<256> doc;
    doc["temperature"]   = temperatureC;
    doc["humidity"]      = humidityPct;
    doc["ammonia_level"] = ammoniaPpm;
    if (predValid) {
        doc["predicted_temperature"] = predTempC;
        doc["predicted_ammonia"]     = predAmmoniaPpm;
    } else {
        doc["predicted_temperature"] = nullptr;
        doc["predicted_ammonia"]     = nullptr;
    }

    String body;
    serializeJson(doc, body);

    HTTPClient http;
    http.setTimeout(HTTP_TIMEOUT_MS);
    http.begin(String(API_BASE_URL) + API_SUBMIT_PATH);
    http.addHeader("Content-Type", "application/json");

    const int code = http.POST(body);
    if (code != 201) {
        Serial.printf("[HTTP] POST failed: %d %s\n",
                      code, http.errorToString(code).c_str());
        http.end();
        return false;
    }

    // Parse response to extract predicted_class for actuator control.
    StaticJsonDocument<384> respDoc;
    const DeserializationError err = deserializeJson(respDoc, http.getStream());
    http.end();
    if (err) {
        Serial.printf("[HTTP] Response parse error: %s\n", err.c_str());
        return false;
    }

    outState = respDoc["record"]["predicted_class"] | "";
    return outState.length() > 0;
}

// ============================================================================
// Actuator control -- closes the environmental loop
// ============================================================================

/**
 * Map the backend classification to fan/heater duty cycles.
 *
 * Rules (independent of the classifier's precedence -- these describe the
 * mechanical response, not the diagnostic):
 *   CRITICAL_AMMONIA    -> fan ON, heater OFF (ventilate)
 *   HEAT_STRESS_WARNING -> fan ON, heater OFF (cool by exhaust)
 *   LOW_TEMP_ALERT      -> heater ON, fan OFF
 *   OPTIMAL_ENVIRONMENT -> both OFF
 *
 * Fan and heater are never both ON simultaneously; the interlock is
 * inherent to the classification since a single state is returned.
 */
static void applyActuators(const String& state) {
    uint32_t fanDuty    = PWM_DUTY_OFF;
    uint32_t heaterDuty = PWM_DUTY_OFF;

    if (state == STATE_CRITICAL || state == STATE_HEAT) {
        fanDuty = PWM_DUTY_ON;
    } else if (state == STATE_LOW_TEMP) {
        heaterDuty = PWM_DUTY_ON;
    }

    ledcWrite(PWM_FAN_CHANNEL,    fanDuty);
    ledcWrite(PWM_HEATER_CHANNEL, heaterDuty);
}

// ============================================================================
// Arduino entrypoints
// ============================================================================
void setup() {
    Serial.begin(115200);
    delay(50);
    Serial.println("\n[BOOT] Poultry Telemetry edge node starting.");

    // Sensors
    dht.begin();
    analogReadResolution(12);
    analogSetAttenuation(ADC_11db);  // 0..3.3 V range on GPIO34.

    // PWM peripherals for fan and heater. Attach BEFORE writing duty so the
    // first control cycle doesn't glitch the MOSFET gates high.
    ledcSetup(PWM_FAN_CHANNEL,    PWM_FREQ_HZ, PWM_RESOLUTION_BITS);
    ledcSetup(PWM_HEATER_CHANNEL, PWM_FREQ_HZ, PWM_RESOLUTION_BITS);
    ledcAttachPin(PIN_PWM_FAN,    PWM_FAN_CHANNEL);
    ledcAttachPin(PIN_PWM_HEATER, PWM_HEATER_CHANNEL);
    ledcWrite(PWM_FAN_CHANNEL,    PWM_DUTY_OFF);
    ledcWrite(PWM_HEATER_CHANNEL, PWM_DUTY_OFF);

    connectWifi();
    nextSampleAt = millis();  // First tick fires immediately.
}

void loop() {
    ensureWifi();

    const uint32_t now = millis();
    if (static_cast<int32_t>(now - nextSampleAt) < 0) {
        return;  // Not time yet; cooperatively yield.
    }
    nextSampleAt = now + SAMPLE_INTERVAL_MS;

    // -- Sample --------------------------------------------------------------
    float temperatureC = 0.0f, humidityPct = 0.0f;
    if (!sampleDht(temperatureC, humidityPct)) {
        return;  // Skip tick on invalid DHT read; next tick retries.
    }
    const float ammoniaPpm = sampleMq137();

    float predTempC = 0.0f, predAmmoniaPpm = 0.0f;
    bool  predValid = false;
    edgeAiForecast(temperatureC, ammoniaPpm, predTempC, predAmmoniaPpm, predValid);

    // -- Transmit + close the control loop -----------------------------------
    String state;
    const bool ok = transmit(temperatureC, humidityPct, ammoniaPpm,
                             predTempC, predAmmoniaPpm, predValid, state);
    if (ok) {
        applyActuators(state);
        Serial.printf("[TX] T=%.2fC RH=%.2f%% NH3=%.2fppm "
                      "PredT=%.2f PredNH3=%.2f STATE=%s\n",
                      temperatureC, humidityPct, ammoniaPpm,
                      predTempC, predAmmoniaPpm, state.c_str());
    } else {
        Serial.println("[TX] Transmit or classification failed; actuators held.");
    }
}
