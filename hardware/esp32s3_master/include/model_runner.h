// ============================================================================
// model_runner.h -- On-device TFLite Micro inference for the master node.
//
// Owns the rolling raw/feature history and the interpreter. The only files
// tied to a specific trained model are model_data.h (weights) and
// scaler_params.h (normalization + spike threshold) -- to deploy a
// retrained model, regenerate those two files from the training pipeline
// and rebuild; nothing in this header or model_runner.cpp needs to change
// as long as the new model keeps the same input feature layout and output
// tensor shapes (see buildFeatureRowFromHistory() in model_runner.cpp).
// ============================================================================
#pragma once

#include <cstdint>

// One raw sample as received from the sensor node over ESP-NOW.
struct RawSample {
    float   temperature;
    float   humidity;
    float   ammonia_ppm;
    uint8_t hour;   // 0-23 local time
    uint8_t month;  // 1-12
};

struct ModelPrediction {
    float temperature_next;
    float ammonia_next;
    float spike_probability;
    bool  spike_predicted;
};

namespace model_runner {

// Loads g_model_data, validates the schema, and allocates the interpreter
// and tensor arena. Call once from setup(). Returns false on any failure
// (schema mismatch, tensor allocation failure) -- treat as fatal, the
// board has nothing useful to forecast without it.
bool init();

// Feeds one new raw sample into the rolling 3-sample history and 6-row
// feature window, then runs inference once the window is full. Returns
// false (leaving `out` untouched) while still warming up after boot --
// expect ~9 samples (~45s at the sensor node's 5s cadence) before the
// first true return.
bool predict(const RawSample& sample, ModelPrediction& out);

}  // namespace model_runner
