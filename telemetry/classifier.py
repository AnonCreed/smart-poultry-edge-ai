"""
Environmental state classifier.

Isolated as a pure function so the decision framework is unit-testable
without HTTP or ORM machinery, and so future ML-backed classifiers can be
swapped in behind the same signature.

Rule precedence (first match wins):
1. NH3 toxicity dominates every other condition: ammonia > 25.0 PPM is an
   immediate CRITICAL_AMMONIA regardless of thermal state, because ammonia
   exposure damage is faster-acting than thermal stress.
2. Combined heat + humidity load (T > 35.0 AND RH > 70.0) yields
   HEAT_STRESS_WARNING; high humidity suppresses evaporative cooling, so
   the two conditions are only dangerous in conjunction.
3. Hypothermia risk: temperature < 18.0 yields LOW_TEMP_ALERT.
4. Fallback: OPTIMAL_ENVIRONMENT.
"""

from .models import EnvironmentalState

# Threshold constants exposed at module level so the API layer can echo the
# active thresholds to the front end without duplicating magic numbers.
AMMONIA_CRITICAL_PPM = 25.0
HEAT_STRESS_TEMP_C = 35.0
HEAT_STRESS_HUMIDITY_PCT = 70.0
LOW_TEMP_C = 18.0


def classify_environment(temperature: float, humidity: float, ammonia_level: float) -> str:
    """Map a raw sensor triplet to a classification label. Pure and stateless."""
    if ammonia_level > AMMONIA_CRITICAL_PPM:
        return EnvironmentalState.CRITICAL_AMMONIA

    if temperature > HEAT_STRESS_TEMP_C and humidity > HEAT_STRESS_HUMIDITY_PCT:
        return EnvironmentalState.HEAT_STRESS_WARNING

    if temperature < LOW_TEMP_C:
        return EnvironmentalState.LOW_TEMP_ALERT

    return EnvironmentalState.OPTIMAL_ENVIRONMENT
