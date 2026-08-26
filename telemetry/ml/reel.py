"""
reel.py -- builds the Test Cases tab's demo reel: a full, precomputed,
deterministic sequence of "frames" covering every case in scenarios.py,
fed through the REAL classifier and REAL actuator duty logic plus a
pure-Python reimplementation of the real trained model -- so playback
shows genuinely computed behavior, not canned numbers, without waiting on
the model's real ~70-minute warm-up.

This module itself never writes to the database: classify_environment()
and ActuatorControl.auto_duty_for_state() are called as pure functions
with scenario-supplied arguments, not the live ActuatorControl singleton
row -- the real PoultryTelemetry table and the real ActuatorControl row
are never touched HERE. FlockProfile.current() is read-only here (same
call every other view already makes; it can lazily create the singleton
with defaults on first-ever use, same as any other page load -- it never
persists anything scenario-specific).

v3 note: the dashboard's JS now ALSO pushes each returned frame's
actuator duty to the real ActuatorControl row via the existing
/api/telemetry/control/ endpoint, so it actually drives the real
fan/heater during playback -- but that side effect lives entirely in
dashboard.js's renderTestCaseFrame(), not here. This function's job is
still just "compute and return," same as before.
"""
import math

from django.utils import timezone

from .. import classifier
from ..models import ActuatorControl, FlockProfile
from . import forecast_model
from .scenarios import SCENARIOS


def _lerp(start: float, end: float, frac: float) -> float:
    return start + (end - start) * frac


def _make_bucket(temperature: float, humidity: float, ammonia_ppm: float,
                  hour_sin: float, hour_cos: float) -> dict:
    return {
        "temperature": temperature,
        "humidity": humidity,
        "ammonia_ppm": ammonia_ppm,
        "hour_sin": hour_sin,
        "hour_cos": hour_cos,
        "temp_hum_interaction": temperature * humidity,
    }


def build_demo_reel() -> dict:
    """{"frames": [...], "thresholds": {...}} -- the full flat list of
    frames across all cases in scenarios.py, in order, plus the exact thresholds
    they were classified against (the active flock profile's, which may
    differ from classifier.py's bare module defaults if a custom
    override is set) so the frontend's chart reference lines never need
    to duplicate/hardcode a second copy of these numbers. Deterministic
    given the current flock profile and current hour (both read once, up
    front, not re-read per frame)."""
    profile = FlockProfile.current()
    age_weeks = profile.age_weeks
    low_temp_c, heat_stress_temp_c = profile.active_temperature_band()
    ammonia_critical_ppm = profile.active_ammonia_critical_ppm()

    hour = timezone.now().hour
    hour_angle = (2.0 * math.pi * hour) / 24.0
    hour_sin = math.sin(hour_angle)
    hour_cos = math.cos(hour_angle)

    frames: list[dict] = []

    for scenario_index, scenario in enumerate(SCENARIOS):
        n = scenario.frames
        # Pre-seed all 7 buckets at the scenario's starting condition, as
        # if it had already been steady for the past 60+ minutes -- this
        # is what makes frame 0 have a complete lag history immediately,
        # with no warm-up wait.
        history = [
            _make_bucket(scenario.temp_start, scenario.humidity_start,
                         scenario.ammonia_start, hour_sin, hour_cos)
            for _ in range(forecast_model.BUCKET_HISTORY_LEN)
        ]

        for frame_index in range(n):
            frac = frame_index / (n - 1) if n > 1 else 0.0
            temperature = _lerp(scenario.temp_start, scenario.temp_end, frac)
            humidity = _lerp(scenario.humidity_start, scenario.humidity_end, frac)
            ammonia = _lerp(scenario.ammonia_start, scenario.ammonia_end, frac)

            if frame_index > 0:
                # Roll the window forward one new bucket -- the same
                # operation as shiftBucketHistoryAndAppend() in
                # model_runner.cpp. Frame 0 needs no shift: its "current"
                # reading already equals the pre-seeded buckets' value.
                history = history[1:] + [
                    _make_bucket(temperature, humidity, ammonia, hour_sin, hour_cos)
                ]

            feature_row = forecast_model.build_feature_row(history, age_weeks)
            normalized = forecast_model.normalize(feature_row)
            forecast = forecast_model.forward(normalized)

            state = classifier.classify_environment(
                temperature, humidity, ammonia,
                temp_min_c=low_temp_c, temp_max_c=heat_stress_temp_c,
                ammonia_critical_ppm=ammonia_critical_ppm,
            )
            is_low_temperature = temperature < low_temp_c

            # Pure classmethod call -- no DB read/write of the actuator
            # row. This is the exact function AUTO mode uses for real
            # traffic; MANUAL mode / the safety-floor-over-MANUAL fix
            # aren't exercised here since there's no "operator setting"
            # in a demo reel, only "what would AUTO do right now."
            fan_pct, heater_pct = ActuatorControl.auto_duty_for_state(
                state, forecast["predicted_ammonia"], is_low_temperature,
            )

            frames.append({
                "scenario_key": scenario.key,
                "scenario_label": scenario.label,
                "scenario_description": scenario.description,
                "scenario_index": scenario_index,
                "scenario_count": len(SCENARIOS),
                "frame_index": frame_index,
                "frames_in_scenario": n,
                "reading": {
                    "temperature": round(temperature, 2),
                    "humidity": round(humidity, 2),
                    "ammonia_level": round(ammonia, 2),
                },
                "classification": state,
                "is_low_temperature": is_low_temperature,
                "forecast": {
                    "predicted_temperature": round(forecast["predicted_temperature"], 2),
                    "predicted_ammonia": round(forecast["predicted_ammonia"], 2),
                    "ammonia_spike_probability": round(forecast["ammonia_spike_probability"], 4),
                    "ammonia_spike_predicted": forecast["ammonia_spike_predicted"],
                    "temp_spike_probability": round(forecast["temp_spike_probability"], 4),
                    "temp_spike_predicted": forecast["temp_spike_predicted"],
                },
                "actuator": {
                    "fan_pct": fan_pct,
                    "heater_pct": heater_pct,
                },
            })

    return {
        "frames": frames,
        "thresholds": {
            "heat_stress_temp_c": heat_stress_temp_c,
            "low_temp_c": low_temp_c,
            "ammonia_critical_ppm": ammonia_critical_ppm,
        },
    }
