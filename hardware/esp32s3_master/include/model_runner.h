// ============================================================================
// model_runner.h -- On-device TFLite Micro inference for the master node.
//
// Retrained model (2026-08-26): flat 44-feature input (no more windowed 2D
// tensor -- the "sliding window" lives entirely in the feature engineering
// below, not in the tensor shape), two 2-element output heads: next_values
// (regression: next temperature + log-ammonia) and spike_flags
// (classification: ammonia-spike + temperature-spike probabilities).
//
// Owns the rolling raw-sample-to-bucket accumulator, the finalized-bucket
// history, and the interpreter. To deploy a further retrain: regenerate
// model_data.h and scaler_params.h from the training pipeline's
// export_meta.pkl + model_esp32.tflite (see
// hardware/esp32s3_master/README.md's "Swapping in a retrained model"
// section for the exact steps, including the output-tensor-order
// verification this file's readPrediction() depends on) -- nothing in this
// header or model_runner.cpp needs to change as long as the new model
// keeps the same 44-feature layout and the same two 2-element output heads.
// If the training pipeline changes which features it derives (feature_cols
// in the training script), buildFeatureRow() in model_runner.cpp needs
// matching edits.
// ============================================================================
#pragma once

#include <cstdint>

// One raw sample as received from the sensor node over ESP-NOW, with hour
// filled in locally by this board (currentHour() in main.cpp) rather than
// the sensor -- it has no WiFi/NTP of its own to source a clock from (see
// esp32/include/config.h's "Radio" section). No month field: the training
// pipeline never derives a month-cyclic feature, so there was never a
// reason to carry one once the sensor stopped supplying its own hour too.
struct RawSample {
    float   temperature;
    float   humidity;
    float   ammonia_ppm;
    uint8_t hour;   // 0-23 local time
};

struct ModelPrediction {
    float temperature_next;             // deg C, ~10 min ahead
    float ammonia_next;                 // ppm, ~10 min ahead
    float ammonia_spike_probability;    // 0-1, sigmoid output
    bool  ammonia_spike_predicted;      // >= kAmmoniaSpikeThreshold
    float temp_spike_probability;       // 0-1, sigmoid output
    bool  temp_spike_predicted;         // >= kTempSpikeThreshold
};

namespace model_runner {

// Loads g_model_data, validates the schema, and allocates the interpreter
// and tensor arena. Call once from setup(). Returns false on any failure
// (schema mismatch, tensor allocation failure) -- treat as fatal, the
// board has nothing useful to forecast without it.
bool init();

// Feeds one new raw sample (arrives ~every 5s from the sensor node) into a
// rolling 10-minute accumulation bucket. The model was trained on data
// resampled to a fixed 10-minute interval -- see the training pipeline and
// hardware/esp32s3_master/README.md -- so raw 5s-spaced samples are
// averaged into 10-minute buckets internally before any lag/rolling
// feature is computed; feeding 5s-spaced values straight into "lag1" would
// silently redefine it from "10 minutes ago" to "5 seconds ago" and make
// every learned weight meaningless. `flockAgeWeeks` is the current flock
// age (synced from Django's FlockProfile via the independent control poll
// in main.cpp -- pass 1 if not yet known), clipped internally to [1, 5] to
// match the training pipeline's `week` feature.
//
// Returns false (leaving `out` untouched) on almost every call -- a fresh
// prediction is only produced once each ~10-minute bucket finalizes, and
// only once 7 buckets (~70 minutes after boot) have accumulated (lag6
// reaches 60 minutes back, plus the current bucket itself).
bool predict(const RawSample& sample, uint8_t flockAgeWeeks, ModelPrediction& out);

// Seconds until the next prediction becomes available -- whichever comes
// next, the very first one (still accumulating toward 7 finalized buckets,
// ~70 min after boot) or the next steady-state refresh once warmed up (a
// fresh inference every ~10 minutes thereafter). Both cases are really the
// same question -- "how much of the current bucket, plus how many more full
// buckets, stand between now and the next finalized bucket that satisfies
// the 7-bucket history" -- so one formula covers both without the caller
// needing to know which phase it's in. Purely informational: read by
// main.cpp to report progress to Django/the dashboard, never consumed by
// predict() itself. Returns 0 if called before the first predict() call has
// started the first bucket.
uint32_t secondsUntilNextPrediction();

}  // namespace model_runner
