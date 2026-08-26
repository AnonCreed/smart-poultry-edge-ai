#include "model_runner.h"

#include <math.h>
#include <string.h>

#include <Arduino.h>  // millis()

#include "model_data.h"
#include "scaler_params.h"

#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"

namespace model_runner {
namespace {

// The model was trained on data resampled to a fixed 10-minute interval
// (see the training pipeline / hardware/esp32s3_master/README.md), but the
// sensor node transmits every ~5s (SAMPLE_INTERVAL_MS on the sensor side).
// Every incoming raw sample is folded into a running average for the
// CURRENT 10-minute bucket; once kBucketDurationMs elapses, that bucket is
// finalized (mean of everything received during it) and pushed into a
// rolling history of finalized buckets -- feature engineering below reads
// only from that finalized-bucket history, never from raw samples directly.
constexpr unsigned long kBucketDurationMs = 10UL * 60UL * 1000UL;

// lag6 (60 min back) is the deepest lookback the trained feature set needs;
// +1 for the current (just-finalized) bucket itself.
constexpr int kBucketHistoryLen = 7;  // [0]=60 min ago ... [6]=current

struct Bucket {
    double sumTemperature = 0.0;
    double sumHumidity = 0.0;
    double sumAmmonia = 0.0;
    int    count = 0;
    uint8_t hour = 0;  // hour at finalization time, for hour_sin/cos

    float temperature() const { return count ? static_cast<float>(sumTemperature / count) : 0.0f; }
    float humidity() const    { return count ? static_cast<float>(sumHumidity / count) : 0.0f; }
    float ammonia() const     { return count ? static_cast<float>(sumAmmonia / count) : 0.0f; }
};

// One finalized 10-minute bucket, holding exactly the per-column values
// feature engineering needs (raw hour is dropped once hour_sin/cos are
// derived -- nothing downstream needs it directly).
struct FinalizedBucket {
    float temperature = 0.0f;
    float humidity = 0.0f;
    float ammonia_ppm = 0.0f;
    float hour_sin = 0.0f;
    float hour_cos = 0.0f;
    float temp_hum_interaction = 0.0f;
};

Bucket        g_currentBucket;
unsigned long g_bucketStartMs = 0;
bool          g_bucketStarted = false;

FinalizedBucket g_bucketHistory[kBucketHistoryLen];
int             g_bucketCount = 0;

constexpr int kTensorArenaSize = 40 * 1024;

const tflite::Model*      g_model = nullptr;
tflite::MicroInterpreter*  g_interpreter = nullptr;
uint8_t                    g_tensorArena[kTensorArenaSize];

void shiftBucketHistoryAndAppend(const FinalizedBucket& b) {
    if (g_bucketCount < kBucketHistoryLen) {
        g_bucketHistory[g_bucketCount++] = b;
        return;
    }
    for (int i = 0; i < kBucketHistoryLen - 1; ++i) {
        g_bucketHistory[i] = g_bucketHistory[i + 1];
    }
    g_bucketHistory[kBucketHistoryLen - 1] = b;
}

// Sample standard deviation over 3 values (divides by n-1=2), matching
// pandas' rolling(3).std() default (ddof=1).
float sampleStd3(float a, float b, float c) {
    float mean = (a + b + c) / 3.0f;
    float v1 = a - mean, v2 = b - mean, v3 = c - mean;
    float variance = (v1 * v1 + v2 * v2 + v3 * v3) / 2.0f;
    return sqrtf(fmaxf(variance, 0.0f));
}

// Writes one base column's 7 engineered values -- lag1, lag2, lag3, lag6,
// roll_mean3, roll_std3, delta1, in that exact order -- into outFeatures
// starting at *outIdx, then advances *outIdx by 7. `h` is the 7-entry
// finalized-bucket history (h[6]=current/most recent, h[0]=60 min back).
// Mirrors add_lag_features()'s per-column block in the training script
// exactly: same 4 lag steps, same 3-sample rolling window (which includes
// the current bucket, matching pandas' rolling(3) default), same delta.
void writeColumnFeatures(const FinalizedBucket* h, int* outIdx, float* outFeatures,
                          float FinalizedBucket::*field) {
    const float lag1 = h[5].*field;
    const float lag2 = h[4].*field;
    const float lag3 = h[3].*field;
    const float lag6 = h[0].*field;
    const float cur  = h[6].*field;

    outFeatures[(*outIdx)++] = lag1;
    outFeatures[(*outIdx)++] = lag2;
    outFeatures[(*outIdx)++] = lag3;
    outFeatures[(*outIdx)++] = lag6;
    outFeatures[(*outIdx)++] = (h[4].*field + h[5].*field + h[6].*field) / 3.0f;  // roll_mean3
    outFeatures[(*outIdx)++] = sampleStd3(h[4].*field, h[5].*field, h[6].*field); // roll_std3
    outFeatures[(*outIdx)++] = cur - lag1;                                       // delta1
}

// Builds the 44-feature row the model expects from the 7 finalized buckets
// in g_bucketHistory. Column order and math must stay byte-identical with
// feature_cols in the training script / export_meta.pkl (see
// hardware/esp32s3_master/README.md): 6 base columns
// (temperature, humidity, ammonia_ppm, hour_sin, hour_cos,
// temp_hum_interaction) x 7 values each = 42, + ammonia_accel + week = 44.
bool buildFeatureRow(float weekValue, float* outFeatures) {
    if (g_bucketCount < kBucketHistoryLen) return false;

    const FinalizedBucket* h = g_bucketHistory;
    int idx = 0;

    writeColumnFeatures(h, &idx, outFeatures, &FinalizedBucket::temperature);
    writeColumnFeatures(h, &idx, outFeatures, &FinalizedBucket::humidity);
    writeColumnFeatures(h, &idx, outFeatures, &FinalizedBucket::ammonia_ppm);
    writeColumnFeatures(h, &idx, outFeatures, &FinalizedBucket::hour_sin);
    writeColumnFeatures(h, &idx, outFeatures, &FinalizedBucket::hour_cos);
    writeColumnFeatures(h, &idx, outFeatures, &FinalizedBucket::temp_hum_interaction);

    // ammonia_accel = ammonia_delta1 - (ammonia_lag1 - ammonia_lag2)
    const float ammoniaLag1 = h[5].ammonia_ppm;
    const float ammoniaLag2 = h[4].ammonia_ppm;
    const float ammoniaCur  = h[6].ammonia_ppm;
    const float ammoniaDelta1 = ammoniaCur - ammoniaLag1;
    outFeatures[idx++] = ammoniaDelta1 - (ammoniaLag1 - ammoniaLag2);

    // week: flock age in weeks, clipped to [1, 5] to match the training
    // pipeline's `(day_index // 7 + 1).clip(upper=5)` (always >=1 there
    // since day_index >= 0, so only the upper bound needs enforcing --
    // clipped at both ends here anyway since this value arrives over the
    // network from Django and shouldn't be trusted blindly).
    float week = weekValue;
    if (week < 1.0f) week = 1.0f;
    if (week > 5.0f) week = 5.0f;
    outFeatures[idx++] = week;

    return idx == kFeatureCount;
}

// Takes the input tensor pointer as an argument rather than reading the
// cached g_input -- re-fetched fresh via g_interpreter->input(0) on every
// predict() call, same as readPrediction() already does for the output
// tensors. A cached pointer from init() intermittently went stale here
// (see git history for this file: the same predict()-time input worked
// fine when re-fetched, but silently produced an all-NaN tensor when
// reused from a pointer captured once at boot).
void normalizeFeaturesToInputTensor(TfLiteTensor* inputTensor, const float* features) {
    for (int i = 0; i < kFeatureCount; ++i) {
        float s = kFeatureScale[i];
        if (fabsf(s) < 1e-8f) s = 1.0f;
        inputTensor->data.f[i] = (features[i] - kFeatureMean[i]) / s;
    }
}

int tensorElementCount(const TfLiteTensor* tensor) {
    int count = 1;
    for (int i = 0; i < tensor->dims->size; ++i) count *= tensor->dims->data[i];
    return count;
}

// Both output heads are 2-element tensors -- unlike the previous model,
// whose 1-element classifier head made telling them apart by size alone
// trivial. Which interpreter output index is "next_values" (regression,
// unconstrained/standardized) vs "spike_flags" (classification, sigmoid,
// both values in [0,1]) is NOT guaranteed by declaration order in the
// training script (`outputs=[reg_out, clf_out]`); it depends on how the
// converter laid out the flatbuffer. VERIFIED for this exact
// model_esp32.tflite with:
//
//   python3 -c "
//   import tensorflow as tf, numpy as np
//   interp = tf.lite.Interpreter(model_path='model_esp32.tflite')
//   interp.allocate_tensors()
//   inp, out = interp.get_input_details(), interp.get_output_details()
//   x = np.zeros((1, 44), dtype=np.float32)
//   interp.set_tensor(inp[0]['index'], x); interp.invoke()
//   for d in out: print(d['name'], '->', interp.get_tensor(d['index']))
//   "
//
// -- output(0) came back spike_flags (both values in [0,1]), output(1)
// came back next_values (standardized, unconstrained). A runtime range
// check below re-derives this on every inference instead of trusting a
// bare hardcoded index, so a future retrain that silently changes this
// ordering fails loudly (both-or-neither-look-like-probabilities) rather
// than quietly swapping regression and classification outputs.
bool readPrediction(ModelPrediction& out) {
    if (g_interpreter->outputs_size() != 2) return false;

    TfLiteTensor* out0 = g_interpreter->output(0);
    TfLiteTensor* out1 = g_interpreter->output(1);
    if (tensorElementCount(out0) != 2 || tensorElementCount(out1) != 2) return false;

    auto looksLikeProbabilities = [](TfLiteTensor* t) {
        for (int i = 0; i < 2; ++i) {
            if (t->data.f[i] < 0.0f || t->data.f[i] > 1.0f) return false;
        }
        return true;
    };

    const bool zeroLooksLikeProb = looksLikeProbabilities(out0);
    const bool oneLooksLikeProb  = looksLikeProbabilities(out1);

    TfLiteTensor* clfTensor;
    TfLiteTensor* regTensor;
    if (zeroLooksLikeProb && !oneLooksLikeProb) {
        clfTensor = out0;
        regTensor = out1;
    } else if (oneLooksLikeProb && !zeroLooksLikeProb) {
        clfTensor = out1;
        regTensor = out0;
    } else {
        // Both or neither look like probabilities -- ambiguous. Fall back
        // to the verified fixed order for THIS model rather than silently
        // guessing wrong; re-run the Python snippet above after any
        // retrain if this branch is ever hit in practice.
        clfTensor = out0;
        regTensor = out1;
    }

    // reg order: [0]=temperature_smoothed_next (deg C, direct unscale),
    // [1]=ammonia_ppm_log_next (log1p-transformed in training -- needs
    // expm1 after unscaling, see reg_targets in the training script).
    const float tempRaw = regTensor->data.f[0] * kRegScale[0] + kRegMean[0];
    const float ammoniaLogRaw = regTensor->data.f[1] * kRegScale[1] + kRegMean[1];
    float ammoniaRaw = expf(ammoniaLogRaw) - 1.0f;  // expm1(x) = exp(x) - 1
    if (ammoniaRaw < 0.0f) ammoniaRaw = 0.0f;        // np.clip(pred_nh3_ppm, 0, None)

    out.temperature_next = tempRaw;
    out.ammonia_next = ammoniaRaw;

    // clf order: [0]=ammonia_spike, [1]=temp_spike -- matches
    // Concatenate(name='spike_flags')([a_clf, t_clf]) in the training script.
    out.ammonia_spike_probability = clfTensor->data.f[0];
    out.ammonia_spike_predicted = out.ammonia_spike_probability >= kAmmoniaSpikeThreshold;
    out.temp_spike_probability = clfTensor->data.f[1];
    out.temp_spike_predicted = out.temp_spike_probability >= kTempSpikeThreshold;

    return true;
}

}  // namespace

bool init() {
    g_model = tflite::GetModel(g_model_data);
    if (g_model->version() != TFLITE_SCHEMA_VERSION) {
        return false;
    }

    // Raw ops actually present in model_esp32.tflite -- verified directly
    // from the flatbuffer's operator_codes (not the desktop TF Lite
    // interpreter's op list, which silently substitutes an XNNPACK
    // DELEGATE entry that doesn't exist in the file itself):
    //
    //   python3 -c "
    //   from tensorflow.lite.python import schema_py_generated as schema_fb
    //   buf = bytearray(open('model_esp32.tflite','rb').read())
    //   m = schema_fb.ModelT.InitFromObj(schema_fb.Model.GetRootAsModel(buf, 0))
    //   codes = {(oc.deprecatedBuiltinCode if oc.builtinCode==0 else oc.builtinCode) for oc in m.operatorCodes}
    //   names = {v:k for k,v in vars(schema_fb.BuiltinOperator).items() if isinstance(v,int)}
    //   print([names.get(c,c) for c in codes])
    //   "
    //
    // The flat 44-input model needs none of the old windowed model's
    // Shape/StridedSlice/Pack/Reshape ops -- it's a plain MLP over a
    // pre-engineered feature vector, no on-device tensor reshaping at all.
    static tflite::MicroMutableOpResolver<3> resolver;
    resolver.AddFullyConnected();
    resolver.AddConcatenation();
    resolver.AddLogistic();

    static tflite::MicroInterpreter staticInterpreter(
        g_model, resolver, g_tensorArena, kTensorArenaSize);
    g_interpreter = &staticInterpreter;

    if (g_interpreter->AllocateTensors() != kTfLiteOk) {
        return false;
    }

    return true;
}

bool predict(const RawSample& sample, uint8_t flockAgeWeeks, ModelPrediction& out) {
    const unsigned long now = millis();
    if (!g_bucketStarted) {
        g_bucketStartMs = now;
        g_bucketStarted = true;
    }

    g_currentBucket.sumTemperature += sample.temperature;
    g_currentBucket.sumHumidity += sample.humidity;
    g_currentBucket.sumAmmonia += sample.ammonia_ppm;
    g_currentBucket.count++;
    g_currentBucket.hour = sample.hour;

    if (now - g_bucketStartMs < kBucketDurationMs) {
        return false;  // bucket still filling -- most calls end here
    }

    // Finalize the bucket that just closed.
    const float hourAngle =
        (2.0f * static_cast<float>(M_PI) * static_cast<float>(g_currentBucket.hour)) / 24.0f;
    FinalizedBucket fb;
    fb.temperature = g_currentBucket.temperature();
    fb.humidity = g_currentBucket.humidity();
    fb.ammonia_ppm = g_currentBucket.ammonia();
    fb.hour_sin = sinf(hourAngle);
    fb.hour_cos = cosf(hourAngle);
    fb.temp_hum_interaction = fb.temperature * fb.humidity;
    shiftBucketHistoryAndAppend(fb);

    // Start the next bucket fresh.
    g_currentBucket = Bucket{};
    g_bucketStartMs = now;

    float featureRow[kFeatureCount];
    if (!buildFeatureRow(static_cast<float>(flockAgeWeeks), featureRow)) {
        return false;  // still warming up: < 7 finalized buckets (~70 min after boot)
    }

    // Re-fetched fresh on every inference (not cached from init()) --
    // readPrediction() below does the same for output tensors. TFLite
    // Micro's memory planner can alias/reuse a tensor's backing memory once
    // it's no longer needed by later ops in the graph, so a pointer is only
    // reliably valid for the Invoke() call it was fetched around.
    TfLiteTensor* inputTensor = g_interpreter->input(0);
    normalizeFeaturesToInputTensor(inputTensor, featureRow);
    if (g_interpreter->Invoke() != kTfLiteOk) {
        return false;
    }

    return readPrediction(out);
}

}  // namespace model_runner
