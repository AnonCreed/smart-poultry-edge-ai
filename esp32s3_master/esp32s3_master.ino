// ============================================================================
// esp32s3_master.ino -- Poultry Telemetry ESP32-S3 master node.
//
// Sits between the ESP32 DevKit V1 sensor node and the Django backend:
//   1. Receives raw {temperature, humidity, ammonia_ppm, hour, month} samples
//      from the sensor node over ESP-NOW (onDataRecv).
//   2. Feeds a rolling 6-sample feature window into the on-device TFLite
//      Micro model (model_data.h / scaler_params.h -- trained artifacts,
//      shipped as-is from the training pipeline, do not hand-edit) to
//      forecast next-tick temperature/ammonia and an ammonia-spike
//      probability.
//   3. POSTs the combined record (raw + forecast + spike risk) to Django's
//      /api/telemetry/submit/, the same contract the Python simulator and
//      the sensor node's predecessor firmware used.
//   4. Relays the classification Django computes back to the sensor node
//      over ESP-NOW so it can drive its own fan/heater actuators -- the
//      control loop still closes end-to-end, just across two boards now.
// ============================================================================

#include <math.h>
#include <esp_now.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

#include "model_data.h"
#include "scaler_params.h"
#include "config_master.h"

#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"

namespace {

constexpr int kRawHistoryLen = 3;
constexpr int kTensorArenaSize = 100 * 1024;
constexpr int kRegressionOutputSize = 2;

// Wire contract shared with esp32/src/main.cpp on the sensor node. Field
// order/types must stay byte-identical -- this struct is populated directly
// from the ESP-NOW payload with memcpy, no serializer.
struct SensorPacket {
  uint32_t seq;
  float temperature;
  float humidity;
  float ammonia_ppm;
  uint8_t hour;
  uint8_t month;
};

// Classification relayed back to the sensor node. Must remain byte-identical
// to the struct of the same name in esp32/src/main.cpp.
struct ActuatorCommand {
  char state[24];
};

struct RawSample {
  float temperature;
  float humidity;
  float ammonia_ppm;
  uint8_t hour;
  uint8_t month;
};

SensorPacket g_lastPacket;
volatile bool g_newPacket = false;
portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;

RawSample g_rawHistory[kRawHistoryLen];
int g_rawCount = 0;

float g_featureWindow[kWindowSize][kFeatureCount];
int g_featureCount = 0;

const tflite::Model *g_model = nullptr;
tflite::MicroInterpreter *g_interpreter = nullptr;
TfLiteTensor *g_input = nullptr;
uint8_t g_tensorArena[kTensorArenaSize];

uint8_t g_sensorNodeMac[6] = SENSOR_NODE_MAC_ADDR;

float safeDivide(float a, float b) {
  if (fabsf(b) < 1e-6f) {
    return 0.0f;
  }
  return a / b;
}

float sampleStd3(float a, float b, float c) {
  float mean = (a + b + c) / 3.0f;
  float v1 = a - mean;
  float v2 = b - mean;
  float v3 = c - mean;
  float variance = (v1 * v1 + v2 * v2 + v3 * v3) / 2.0f;
  return sqrtf(fmaxf(variance, 0.0f));
}

void shiftRawHistoryAndAppend(const RawSample &sample) {
  if (g_rawCount < kRawHistoryLen) {
    g_rawHistory[g_rawCount++] = sample;
    return;
  }

  g_rawHistory[0] = g_rawHistory[1];
  g_rawHistory[1] = g_rawHistory[2];
  g_rawHistory[2] = sample;
}

void shiftFeatureWindowAndAppend(const float *featureRow) {
  if (g_featureCount < static_cast<int>(kWindowSize)) {
    memcpy(g_featureWindow[g_featureCount], featureRow, sizeof(float) * kFeatureCount);
    g_featureCount++;
    return;
  }

  for (size_t i = 0; i < kWindowSize - 1; ++i) {
    memcpy(g_featureWindow[i], g_featureWindow[i + 1], sizeof(float) * kFeatureCount);
  }
  memcpy(g_featureWindow[kWindowSize - 1], featureRow, sizeof(float) * kFeatureCount);
}

bool buildFeatureRowFromHistory(float *outFeatures) {
  if (g_rawCount < kRawHistoryLen) {
    return false;
  }

  const RawSample &s0 = g_rawHistory[0];
  const RawSample &s1 = g_rawHistory[1];
  const RawSample &s2 = g_rawHistory[2];

  float hourAngle = (2.0f * static_cast<float>(M_PI) * static_cast<float>(s2.hour)) / 24.0f;
  float monthAngle = (2.0f * static_cast<float>(M_PI) * static_cast<float>(s2.month)) / 12.0f;

  outFeatures[0] = s2.temperature;
  outFeatures[1] = s2.humidity;
  outFeatures[2] = s2.ammonia_ppm;
  outFeatures[3] = sinf(hourAngle);
  outFeatures[4] = cosf(hourAngle);
  outFeatures[5] = sinf(monthAngle);
  outFeatures[6] = cosf(monthAngle);
  outFeatures[7] = s2.ammonia_ppm - s1.ammonia_ppm;
  outFeatures[8] = s2.temperature - s1.temperature;
  outFeatures[9] = s2.humidity - s1.humidity;
  outFeatures[10] = s2.temperature * s2.humidity;
  outFeatures[11] = safeDivide(s2.ammonia_ppm, s2.humidity);
  outFeatures[12] = s2.ammonia_ppm * s2.temperature;

  outFeatures[13] = (s0.temperature + s1.temperature + s2.temperature) / 3.0f;
  outFeatures[14] = sampleStd3(s0.temperature, s1.temperature, s2.temperature);

  outFeatures[15] = (s0.humidity + s1.humidity + s2.humidity) / 3.0f;
  outFeatures[16] = sampleStd3(s0.humidity, s1.humidity, s2.humidity);

  outFeatures[17] = (s0.ammonia_ppm + s1.ammonia_ppm + s2.ammonia_ppm) / 3.0f;
  outFeatures[18] = sampleStd3(s0.ammonia_ppm, s1.ammonia_ppm, s2.ammonia_ppm);

  return true;
}

void normalizeWindowToInputTensor() {
  int idx = 0;
  for (size_t t = 0; t < kWindowSize; ++t) {
    for (size_t f = 0; f < kFeatureCount; ++f) {
      float x = g_featureWindow[t][f];
      float s = kFeatureScale[f];
      if (fabsf(s) < 1e-8f) {
        s = 1.0f;
      }
      g_input->data.f[idx++] = (x - kFeatureMean[f]) / s;
    }
  }
}

int tensorElementCount(const TfLiteTensor *tensor) {
  int count = 1;
  for (int i = 0; i < tensor->dims->size; ++i) {
    count *= tensor->dims->data[i];
  }
  return count;
}

// ---------------------------------------------------------------------------
// WiFi + Django uplink (the ESP32-S3-only leg; the sensor node never touches
// this). Joining the same AP as the sensor node also guarantees both boards
// land on the same ESP-NOW channel, which the protocol requires.
// ---------------------------------------------------------------------------
void connectWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.printf("[WIFI] Associating with SSID '%s'\n", WIFI_SSID);
  for (int attempt = 0; attempt < 10; ++attempt) {
    if (WiFi.status() == WL_CONNECTED) {
      Serial.printf("[WIFI] Link up. IP=%s Channel=%d\n",
                    WiFi.localIP().toString().c_str(), WiFi.channel());
      return;
    }
    delay(2000);
    Serial.printf("[WIFI] retry %d/10\n", attempt + 1);
  }
  Serial.println("[WIFI] Association failed; will retry on next tick.");
}

void ensureWifi() {
  if (WiFi.status() == WL_CONNECTED) return;
  Serial.println("[WIFI] Link lost -- reconnecting.");
  WiFi.disconnect(true, true);
  connectWifi();
}

/**
 * POST the combined record to Django. Returns the parsed predicted_class
 * into outState, or an empty string on any failure (network, non-201,
 * parse error) so the caller can skip the ESP-NOW relay back to the sensor
 * node rather than forwarding a stale/garbage command.
 */
bool postToDjango(const RawSample &raw, float tempNext, float ammoniaNext,
                   float spikeProbability, String &outState) {
  outState = "";
  if (WiFi.status() != WL_CONNECTED) return false;

  StaticJsonDocument<320> doc;
  doc["temperature"] = raw.temperature;
  doc["humidity"] = raw.humidity;
  doc["ammonia_level"] = raw.ammonia_ppm;
  doc["predicted_temperature"] = tempNext;
  doc["predicted_ammonia"] = ammoniaNext;
  doc["predicted_spike_probability"] = spikeProbability;

  String body;
  serializeJson(doc, body);

  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.begin(String(API_BASE_URL) + API_SUBMIT_PATH);
  http.addHeader("Content-Type", "application/json");

  const int code = http.POST(body);
  if (code != 201) {
    Serial.printf("[HTTP] POST failed: %d %s\n", code, http.errorToString(code).c_str());
    http.end();
    return false;
  }

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

/** Relay Django's classification back to the sensor node so it can actuate. */
void relayClassificationToSensor(const String &state) {
  ActuatorCommand cmd{};
  strncpy(cmd.state, state.c_str(), sizeof(cmd.state) - 1);

  const esp_err_t result = esp_now_send(
      g_sensorNodeMac, reinterpret_cast<const uint8_t *>(&cmd), sizeof(cmd));
  if (result != ESP_OK) {
    Serial.println("[ESPNOW] Relay to sensor node failed to queue.");
  }
}

void publishPrediction(const RawSample &raw) {
  TfLiteTensor *regTensor = nullptr;
  TfLiteTensor *clfTensor = nullptr;

  for (size_t i = 0; i < static_cast<size_t>(g_interpreter->outputs_size()); ++i) {
    TfLiteTensor *out = g_interpreter->output(i);
    int n = tensorElementCount(out);
    if (n == kRegressionOutputSize) {
      regTensor = out;
    } else if (n == 1) {
      clfTensor = out;
    }
  }

  if (regTensor == nullptr || clfTensor == nullptr) {
    Serial.println("Output tensor mapping failed");
    return;
  }

  float tempNext = regTensor->data.f[0] * kRegScale[0] + kRegMean[0];
  float ammoniaNext = regTensor->data.f[1] * kRegScale[1] + kRegMean[1];
  float spikeProb = clfTensor->data.f[0];
  int spikePred = spikeProb >= kSpikeThreshold ? 1 : 0;

  Serial.print("Pred next temp=");
  Serial.print(tempNext, 2);
  Serial.print(" C, next ammonia=");
  Serial.print(ammoniaNext, 2);
  Serial.print(" ppm, spike_prob=");
  Serial.print(spikeProb, 4);
  Serial.print(", spike_pred=");
  Serial.println(spikePred);

  ensureWifi();
  String state;
  if (postToDjango(raw, tempNext, ammoniaNext, spikeProb, state)) {
    Serial.printf("[HTTP] Ingested OK -> STATE=%s\n", state.c_str());
    relayClassificationToSensor(state);
  } else {
    Serial.println("[HTTP] Ingestion failed; sensor node actuators held at last state.");
  }
}

void onDataRecv(const esp_now_recv_info_t *info, const uint8_t *incomingData, int len) {
  if (len != sizeof(SensorPacket)) {
    return;
  }

  portENTER_CRITICAL_ISR(&g_mux);
  memcpy(&g_lastPacket, incomingData, sizeof(SensorPacket));
  g_newPacket = true;
  portEXIT_CRITICAL_ISR(&g_mux);
}

bool initModel() {
  g_model = tflite::GetModel(g_model_data);
  if (g_model->version() != TFLITE_SCHEMA_VERSION) {
    Serial.println("Model schema mismatch");
    return false;
  }

  static tflite::MicroMutableOpResolver<12> resolver;
  resolver.AddFullyConnected();
  resolver.AddReshape();
  resolver.AddAdd();
  resolver.AddSub();
  resolver.AddMul();
  resolver.AddLogistic();
  resolver.AddRelu();
  resolver.AddMean();
  resolver.AddRsqrt();
  resolver.AddSquare();
  resolver.AddQuantize();
  resolver.AddDequantize();

  static tflite::MicroInterpreter staticInterpreter(
      g_model, resolver, g_tensorArena, kTensorArenaSize);
  g_interpreter = &staticInterpreter;

  TfLiteStatus allocStatus = g_interpreter->AllocateTensors();
  if (allocStatus != kTfLiteOk) {
    Serial.println("AllocateTensors failed");
    return false;
  }

  g_input = g_interpreter->input(0);
  return true;
}

bool initEspNow() {
  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed");
    return false;
  }

  esp_now_register_recv_cb(onDataRecv);

  // Register the sensor node as a peer so we can send the classification
  // back to it after each successful Django ingestion.
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, g_sensorNodeMac, 6);
  peer.channel = 0;  // 0 == use the current WiFi STA channel.
  peer.encrypt = false;
  if (esp_now_add_peer(&peer) != ESP_OK) {
    Serial.println("[ESPNOW] Failed to register sensor node as peer.");
    return false;
  }

  return true;
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(1500);

  connectWifi();

  Serial.print("Receiver MAC: ");
  Serial.println(WiFi.macAddress());

  if (!initEspNow()) {
    while (true) {
      delay(1000);
    }
  }

  if (!initModel()) {
    while (true) {
      delay(1000);
    }
  }

  Serial.println("Receiver ready");
}

void loop() {
  ensureWifi();

  SensorPacket packet;
  bool hasPacket = false;

  portENTER_CRITICAL(&g_mux);
  if (g_newPacket) {
    packet = g_lastPacket;
    g_newPacket = false;
    hasPacket = true;
  }
  portEXIT_CRITICAL(&g_mux);

  if (!hasPacket) {
    delay(2);
    return;
  }

  RawSample s;
  s.temperature = packet.temperature;
  s.humidity = packet.humidity;
  s.ammonia_ppm = packet.ammonia_ppm;
  s.hour = packet.hour;
  s.month = packet.month;

  shiftRawHistoryAndAppend(s);

  float featureRow[kFeatureCount];
  if (!buildFeatureRowFromHistory(featureRow)) {
    Serial.println("Waiting raw warmup (need 3 samples)");
    return;
  }

  shiftFeatureWindowAndAppend(featureRow);

  if (g_featureCount < static_cast<int>(kWindowSize)) {
    Serial.print("Waiting feature warmup: ");
    Serial.print(g_featureCount);
    Serial.print("/");
    Serial.println(kWindowSize);
    return;
  }

  normalizeWindowToInputTensor();

  if (g_interpreter->Invoke() != kTfLiteOk) {
    Serial.println("Inference failed");
    return;
  }

  publishPrediction(s);
}
