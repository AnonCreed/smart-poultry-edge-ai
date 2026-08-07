#include "model_runner.h"

#include <math.h>
#include <string.h>

#include "model_data.h"
#include "scaler_params.h"

#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"

namespace model_runner {
namespace {

constexpr int kRawHistoryLen = 3;
constexpr int kTensorArenaSize = 100 * 1024;
constexpr int kRegressionOutputSize = 2;

RawSample g_rawHistory[kRawHistoryLen];
int       g_rawCount = 0;

float g_featureWindow[kWindowSize][kFeatureCount];
int   g_featureCount = 0;

const tflite::Model*     g_model = nullptr;
tflite::MicroInterpreter* g_interpreter = nullptr;
TfLiteTensor*             g_input = nullptr;
uint8_t                   g_tensorArena[kTensorArenaSize];

float safeDivide(float a, float b) {
    if (fabsf(b) < 1e-6f) return 0.0f;
    return a / b;
}

float sampleStd3(float a, float b, float c) {
    float mean = (a + b + c) / 3.0f;
    float v1 = a - mean, v2 = b - mean, v3 = c - mean;
    float variance = (v1 * v1 + v2 * v2 + v3 * v3) / 2.0f;
    return sqrtf(fmaxf(variance, 0.0f));
}

void shiftRawHistoryAndAppend(const RawSample& sample) {
    if (g_rawCount < kRawHistoryLen) {
        g_rawHistory[g_rawCount++] = sample;
        return;
    }
    g_rawHistory[0] = g_rawHistory[1];
    g_rawHistory[1] = g_rawHistory[2];
    g_rawHistory[2] = sample;
}

void shiftFeatureWindowAndAppend(const float* featureRow) {
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

// Engineered feature layout the model was trained on. This is the one part
// of this file a retrained model COULD force changes to (if the training
// pipeline changes which features it derives) -- weights/normalization
// alone (model_data.h / scaler_params.h) are always drop-in, this function
// is drop-in only if the feature set itself is unchanged.
bool buildFeatureRowFromHistory(float* outFeatures) {
    if (g_rawCount < kRawHistoryLen) return false;

    const RawSample& s0 = g_rawHistory[0];
    const RawSample& s1 = g_rawHistory[1];
    const RawSample& s2 = g_rawHistory[2];

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
            if (fabsf(s) < 1e-8f) s = 1.0f;
            g_input->data.f[idx++] = (x - kFeatureMean[f]) / s;
        }
    }
}

int tensorElementCount(const TfLiteTensor* tensor) {
    int count = 1;
    for (int i = 0; i < tensor->dims->size; ++i) count *= tensor->dims->data[i];
    return count;
}

// Output tensors aren't guaranteed to come back in a fixed order, so pick
// them out by element count: the 2-element head is the regression output
// (next temp/ammonia), the 1-element head is the spike-risk probability.
bool readPrediction(ModelPrediction& out) {
    TfLiteTensor* regTensor = nullptr;
    TfLiteTensor* clfTensor = nullptr;

    for (size_t i = 0; i < static_cast<size_t>(g_interpreter->outputs_size()); ++i) {
        TfLiteTensor* t = g_interpreter->output(i);
        int n = tensorElementCount(t);
        if (n == kRegressionOutputSize) {
            regTensor = t;
        } else if (n == 1) {
            clfTensor = t;
        }
    }

    if (regTensor == nullptr || clfTensor == nullptr) {
        return false;
    }

    out.temperature_next = regTensor->data.f[0] * kRegScale[0] + kRegMean[0];
    out.ammonia_next     = regTensor->data.f[1] * kRegScale[1] + kRegMean[1];
    out.spike_probability = clfTensor->data.f[0];
    out.spike_predicted   = out.spike_probability >= kSpikeThreshold;
    return true;
}

}  // namespace

bool init() {
    g_model = tflite::GetModel(g_model_data);
    if (g_model->version() != TFLITE_SCHEMA_VERSION) {
        return false;
    }

    // Op set is whatever this specific model_data.h actually uses -- if a
    // retrained model changes shape/architecture, re-derive this list from
    // the new flatbuffer's operator_codes rather than guessing.
    static tflite::MicroMutableOpResolver<6> resolver;
    resolver.AddShape();
    resolver.AddStridedSlice();
    resolver.AddPack();
    resolver.AddReshape();
    resolver.AddFullyConnected();
    resolver.AddLogistic();

    static tflite::MicroInterpreter staticInterpreter(
        g_model, resolver, g_tensorArena, kTensorArenaSize);
    g_interpreter = &staticInterpreter;

    if (g_interpreter->AllocateTensors() != kTfLiteOk) {
        return false;
    }

    g_input = g_interpreter->input(0);
    return true;
}

bool predict(const RawSample& sample, ModelPrediction& out) {
    shiftRawHistoryAndAppend(sample);

    float featureRow[kFeatureCount];
    if (!buildFeatureRowFromHistory(featureRow)) {
        return false;  // raw history still warming up
    }

    shiftFeatureWindowAndAppend(featureRow);
    if (g_featureCount < static_cast<int>(kWindowSize)) {
        return false;  // feature window still warming up
    }

    normalizeWindowToInputTensor();
    if (g_interpreter->Invoke() != kTfLiteOk) {
        return false;
    }

    return readPrediction(out);
}

}  // namespace model_runner
