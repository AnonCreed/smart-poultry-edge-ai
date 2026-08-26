"""
Environmental state classifier.

Isolated as a pure function so the decision framework is unit-testable
without HTTP or ORM machinery, and so future ML-backed classifiers can be
swapped in behind the same signature.

Rule precedence (first match wins):
1. NH3 toxicity dominates every other condition: ammonia > 15.0 PPM is an
   immediate CRITICAL_AMMONIA regardless of thermal state, because ammonia
   exposure damage is faster-acting than thermal stress. 15.0 ppm is also
   where Table 4-1's risk scale (AMMONIA_RISK_LEVELS below) starts labeling
   readings "High risk" -- the reactive cutoff and that display band now
   start at the same value, deliberately.
2. Combined heat + humidity load (T > 35.0 AND RH > 70.0) yields
   HEAT_STRESS_WARNING; high humidity suppresses evaporative cooling, so
   the two conditions are only dangerous in conjunction.
3. Hypothermia risk: temperature < 18.0 yields LOW_TEMP_ALERT.
4. Fallback: OPTIMAL_ENVIRONMENT.
"""

from .models import EnvironmentalState

# Threshold constants exposed at module level so the API layer can echo the
# active thresholds to the front end without duplicating magic numbers.
AMMONIA_CRITICAL_PPM = 15.0
HEAT_STRESS_TEMP_C = 35.0
HEAT_STRESS_HUMIDITY_PCT = 70.0
LOW_TEMP_C = 18.0

# Decision threshold for the ESP32-S3 master node's TFLite Micro spike
# classifier (predicted_spike_probability >= this -> spike predicted). Must
# stay numerically identical to kSpikeThreshold in
# esp32s3_master/scaler_params.h; the two are trained/calibrated together and
# are echoed here only so the API/dashboard don't hardcode a second copy.
AMMONIA_SPIKE_RISK_THRESHOLD = 0.21

# Age-based temperature bands (husbandry reference table, Table 4-2). Each row
# is (min_week, max_week_or_None, temp_min_c, temp_max_c); max_week=None marks
# the open-ended final row (the source table's "Week 4+" line, which sits
# below its own "Week 4" row -- read as guidance for week 5 onward, not a
# restatement of week 4). Ordered ascending so the first matching row wins.
AGE_TEMPERATURE_BANDS_C = [
    (1, 1, 32.0, 35.0),
    (2, 2, 29.0, 32.0),
    (3, 3, 27.0, 29.0),
    (4, 4, 24.0, 27.0),
    (5, None, 20.0, 22.0),
]

# Ammonia concentration risk scale (reference table, Table 4-1). Each row is
# (key, label, min_ppm, max_ppm_or_None); this is an informational overlay
# for display, independent of the CRITICAL_AMMONIA classification cutoff
# above (which a flock profile's custom threshold can move independently).
AMMONIA_RISK_LEVELS = [
    ("no_risk", "No risk", 0.0, 1.0),
    ("low_risk", "Low risk", 2.0, 9.0),
    ("moderate_risk", "Moderate risk", 10.0, 14.0),
    ("high_risk", "High risk", 15.0, 20.0),
    ("very_high_risk", "Very high risk", 21.0, None),
]


def temperature_band_for_age(age_weeks: int) -> tuple[float, float]:
    """Look up the (low_temp_c, heat_stress_temp_c) target band for a flock age.

    Ages past the table's last explicit week fall into the open-ended final
    band; ages below week 1 clamp to the week-1 band.
    """
    age_weeks = max(1, age_weeks)
    for _min_week, max_week, temp_min, temp_max in AGE_TEMPERATURE_BANDS_C:
        if max_week is None or age_weeks <= max_week:
            return temp_min, temp_max
    return AGE_TEMPERATURE_BANDS_C[-1][2], AGE_TEMPERATURE_BANDS_C[-1][3]


def ammonia_risk_level(ammonia_ppm: float) -> tuple[str, str]:
    """Map an NH3 reading to its (key, label) risk tier from Table 4-1."""
    for key, label, _low, high in AMMONIA_RISK_LEVELS:
        if high is None or ammonia_ppm <= high:
            return key, label
    return AMMONIA_RISK_LEVELS[-1][0], AMMONIA_RISK_LEVELS[-1][1]


def classify_environment(
    temperature: float,
    humidity: float,
    ammonia_level: float,
    *,
    temp_min_c: float | None = None,
    temp_max_c: float | None = None,
    ammonia_critical_ppm: float | None = None,
) -> str:
    """Map a raw sensor triplet to a classification label. Pure and stateless.

    temp_min_c/temp_max_c/ammonia_critical_ppm are optional overrides -- how
    a flock profile's age-derived band or custom thresholds reach
    classification. Omitted (the default), thresholds fall back to the fixed
    module constants exactly as before, so existing callers are unaffected.
    """
    low_temp_c = temp_min_c if temp_min_c is not None else LOW_TEMP_C
    heat_stress_temp_c = temp_max_c if temp_max_c is not None else HEAT_STRESS_TEMP_C
    critical_ppm = ammonia_critical_ppm if ammonia_critical_ppm is not None else AMMONIA_CRITICAL_PPM

    if ammonia_level > critical_ppm:
        return EnvironmentalState.CRITICAL_AMMONIA

    if temperature > heat_stress_temp_c and humidity > HEAT_STRESS_HUMIDITY_PCT:
        return EnvironmentalState.HEAT_STRESS_WARNING

    if temperature < low_temp_c:
        return EnvironmentalState.LOW_TEMP_ALERT

    return EnvironmentalState.OPTIMAL_ENVIRONMENT
