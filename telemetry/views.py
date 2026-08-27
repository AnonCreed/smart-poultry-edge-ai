"""
HTTP layer for the telemetry pipeline.

Implemented with plain Django views + JsonResponse to keep the dependency
surface minimal; the request/response contracts are DRF-compatible, so a
migration to serializers/viewsets is a drop-in change if the API grows.

Endpoints:
- POST /api/telemetry/submit/      machine ingestion, classify-then-persist
- GET  /api/telemetry/historical/  time-window query for chart clients
- GET  /                           dashboard shell (server-rendered chrome,
                                   data hydrated client-side via the GET API)
"""

import csv
import json
import logging
import os
from datetime import datetime, timedelta

from django.contrib.staticfiles import finders
from django.db.models import Avg, Count, Max, Min
from django.http import HttpRequest, HttpResponse, JsonResponse
from django.shortcuts import render
from django.utils import timezone
from django.utils.dateparse import parse_date, parse_datetime
from django.views.decorators.csrf import csrf_exempt
from django.views.decorators.http import require_GET, require_http_methods, require_POST

from . import classifier
from .ml import reel as test_case_reel_builder
from .models import ActuatorControl, ActuatorMode, EnvironmentalState, FlockProfile, PoultryTelemetry

# Dedicated ingestion logger. Server-side log lines mirror the front-end
# console feed format so `journalctl -u <service>` and the browser view show
# identical text and can be cross-referenced during audits.
logger = logging.getLogger("telemetry.ingest")

# Ingestion validation bounds. These are plausibility gates for sensor faults
# (disconnected probe, I2C noise), not classification thresholds.
FIELD_BOUNDS = {
    "temperature": (-40.0, 85.0),   # Generous envelope covering DHT11 (0-50C) and SHT31-class sensors
    "humidity": (0.0, 100.0),       # Relative humidity is bounded by definition
    "ammonia_level": (0.0, 500.0),  # MQ-137 electrochemical sensing ceiling
}

# Edge-AI forecast channels transmitted by the ESP32-S3 inference firmware.
# Optional and nullable: hardware is not yet linked, so absent keys and
# explicit nulls are both accepted. When present, values are bounds-checked
# against the same physical envelope as their live counterparts.
OPTIONAL_PREDICTION_BOUNDS = {
    "predicted_temperature": FIELD_BOUNDS["temperature"],
    "predicted_ammonia": FIELD_BOUNDS["ammonia_level"],
}

# Master-node (ESP32-S3) spike classifier output: a bare probability, so its
# bounds are the unit interval rather than a physical sensor envelope.
SPIKE_PROBABILITY_BOUNDS = (0.0, 1.0)

DEFAULT_WINDOW_HOURS = 24
MAX_WINDOW_HOURS = 24 * 30  # Hard cap to bound query cost on large tables


def _error(message: str, status: int = 400) -> JsonResponse:
    """Uniform machine-readable error envelope."""
    return JsonResponse({"status": "error", "detail": message}, status=status)


@csrf_exempt  # Endpoint is consumed by headless hardware clients, not browsers.
@require_POST
def submit_telemetry(request: HttpRequest) -> JsonResponse:
    """
    Ingest one sensor payload.

    Pipeline: parse -> validate types -> validate bounds -> classify -> persist.
    Classification happens synchronously before the INSERT so the stored row
    is complete and immutable; no post-hoc update pass is required.
    """
    try:
        payload = json.loads(request.body.decode("utf-8"))
    except (json.JSONDecodeError, UnicodeDecodeError):
        return _error("Request body must be valid UTF-8 JSON.")

    if not isinstance(payload, dict):
        return _error("Payload root must be a JSON object.")

    values = {}
    for field, (lower, upper) in FIELD_BOUNDS.items():
        if field not in payload:
            return _error(f"Missing required field: '{field}'.")
        raw = payload[field]
        # bool is an int subclass in Python; reject it explicitly.
        if isinstance(raw, bool) or not isinstance(raw, (int, float)):
            return _error(f"Field '{field}' must be numeric, got {type(raw).__name__}.")
        value = float(raw)
        if not (lower <= value <= upper):
            return _error(
                f"Field '{field}'={value} outside plausible sensor range "
                f"[{lower}, {upper}]. Reading rejected as probable sensor fault."
            )
        values[field] = value

    # Optional Edge-AI forecast channels: absent key == explicit null == no
    # forecast. Only numeric, in-envelope values are persisted.
    predictions = {}
    for field, (lower, upper) in OPTIONAL_PREDICTION_BOUNDS.items():
        raw = payload.get(field)
        if raw is None:
            predictions[field] = None
            continue
        if isinstance(raw, bool) or not isinstance(raw, (int, float)):
            return _error(
                f"Field '{field}' must be numeric or null, got {type(raw).__name__}."
            )
        value = float(raw)
        if not (lower <= value <= upper):
            return _error(
                f"Field '{field}'={value} outside plausible forecast range "
                f"[{lower}, {upper}]. Reading rejected."
            )
        predictions[field] = value

    # Master-node (ESP32-S3) spike-risk probability: same absent-key ==
    # explicit-null == no-forecast contract as the regression channels above,
    # bounded to the unit interval instead of a physical sensor envelope.
    raw_spike = payload.get("predicted_spike_probability")
    if raw_spike is None:
        predictions["predicted_spike_probability"] = None
    elif isinstance(raw_spike, bool) or not isinstance(raw_spike, (int, float)):
        return _error(
            f"Field 'predicted_spike_probability' must be numeric or null, "
            f"got {type(raw_spike).__name__}."
        )
    else:
        spike_value = float(raw_spike)
        lower, upper = SPIKE_PROBABILITY_BOUNDS
        if not (lower <= spike_value <= upper):
            return _error(
                f"Field 'predicted_spike_probability'={spike_value} outside "
                f"[{lower}, {upper}]. Reading rejected."
            )
        predictions["predicted_spike_probability"] = spike_value

    # Classification remains driven exclusively by live sensor readings;
    # forecasts are advisory overlays and never alter the stored state. The
    # flock profile's active thresholds default to the classifier's own
    # fixed constants until a user has explicitly saved an age/custom
    # profile, so ingestion behavior is unchanged out of the box.
    profile = FlockProfile.current()
    low_temp_c, heat_stress_temp_c = profile.active_temperature_band()
    predicted = classifier.classify_environment(
        temperature=values["temperature"],
        humidity=values["humidity"],
        ammonia_level=values["ammonia_level"],
        temp_min_c=low_temp_c,
        temp_max_c=heat_stress_temp_c,
        ammonia_critical_ppm=profile.active_ammonia_critical_ppm(),
    )

    # Raw temperature-vs-threshold check, independent of `predicted` above.
    # classify_environment() returns one mutually-exclusive label with
    # ammonia checked first, so a reading that's simultaneously
    # CRITICAL_AMMONIA and genuinely cold classifies only as
    # CRITICAL_AMMONIA -- `predicted == LOW_TEMP_ALERT` would then be False
    # even though it's cold. ActuatorControl's heater duty needs this raw
    # boolean instead, so cold stress is never silently masked by ammonia
    # (see ActuatorControl.auto_duty_for_state()'s docstring).
    is_low_temperature = values["temperature"] < low_temp_c

    record = PoultryTelemetry.objects.create(
        predicted_class=predicted, **values, **predictions
    )

    # Structured ingestion log. Format matches the browser console feed so
    # the two views can be cross-referenced during audits without translation.
    def _fmt(value: float | None, suffix: str) -> str:
        return f"{value:.1f}{suffix}" if value is not None else "n/a"

    spike_probability = predictions["predicted_spike_probability"]
    spike_suffix = (
        f" | SPIKE_RISK: {spike_probability * 100:.0f}%"
        if spike_probability is not None else ""
    )

    logger.info(
        "[%s] INGESTION SUCCESS -> "
        "T: %.1fC (Pred: %s) | RH: %.1f%% | NH3: %.1fppm (Pred: %s) | STATE: %s%s",
        record.timestamp.strftime("%Y-%m-%d %H:%M:%S"),
        values["temperature"],
        _fmt(predictions["predicted_temperature"], "C"),
        values["humidity"],
        values["ammonia_level"],
        _fmt(predictions["predicted_ammonia"], "ppm"),
        predicted,
        spike_suffix,
    )

    # Echo the actuator control state resolved against *this* record's fresh
    # classification, so a firmware client gets one ready-to-apply command
    # (mode-resolved fan/heater duty) in the same round trip it already
    # makes to submit a sample -- no separate poll of /api/telemetry/control/
    # needed on the hot path.
    control = ActuatorControl.current()

    return JsonResponse(
        {
            "status": "ok",
            "record": record.as_payload(),
            "control": control.as_payload(record.predicted_class, record.predicted_ammonia, is_low_temperature),
            # Relayed separately (not just baked into `control`) so the
            # master's own on-device AUTO heater logic can check this raw
            # boolean directly instead of re-deriving it from
            # predicted_class -- see the ActuatorCommand comment in
            # hardware/esp32s3_master/src/main.cpp for why that string
            # alone isn't enough (ammonia-critical + cold both true at once
            # would otherwise mask the cold condition).
            "low_temperature_alert": is_low_temperature,
        },
        status=201,
    )


@require_GET
def historical_telemetry(request: HttpRequest) -> JsonResponse:
    """
    Return records in the trailing time window, ascending chronologically.

    Query params:
        hours (optional int/float, default 24): trailing window size.

    The response shape is a flat array of point objects plus a `thresholds`
    block so chart clients can render baseline/limit lines without
    hardcoding backend constants.
    """
    raw_hours = request.GET.get("hours", str(DEFAULT_WINDOW_HOURS))
    try:
        hours = float(raw_hours)
    except ValueError:
        return _error(f"Query param 'hours' must be numeric, got '{raw_hours}'.")

    if not (0 < hours <= MAX_WINDOW_HOURS):
        return _error(f"Query param 'hours' must be in (0, {MAX_WINDOW_HOURS}].")

    cutoff = timezone.now() - timedelta(hours=hours)

    # order_by("timestamp") flips the model's default DESC ordering to ASC,
    # which is the natural axis direction for time-series chart libraries.
    queryset = (
        PoultryTelemetry.objects
        .filter(timestamp__gte=cutoff)
        .order_by("timestamp")
    )

    # Thresholds echo whatever is *currently active* for ingestion -- the
    # flock profile's age-derived or custom band when configured, otherwise
    # the classifier's fixed defaults -- so the chart's threshold lines and
    # the dashboard's sub-text never drift from what's actually classifying
    # new rows.
    profile = FlockProfile.current()
    active_low_temp_c, active_heat_stress_temp_c = profile.active_temperature_band()

    return JsonResponse(
        {
            "status": "ok",
            "window_hours": hours,
            "count": queryset.count(),
            "thresholds": {
                "ammonia_critical_ppm": profile.active_ammonia_critical_ppm(),
                "heat_stress_temp_c": active_heat_stress_temp_c,
                "heat_stress_humidity_pct": classifier.HEAT_STRESS_HUMIDITY_PCT,
                "low_temp_c": active_low_temp_c,
                "ammonia_spike_risk_threshold": classifier.AMMONIA_SPIKE_RISK_THRESHOLD,
                # Static reference tables, echoed once per poll so the
                # dashboard can do client-side lookups (live age-band
                # preview, ammonia risk-tier label) without duplicating the
                # husbandry reference tables in JS.
                "ammonia_risk_levels": [
                    {"key": key, "label": label, "min_ppm": low, "max_ppm": high}
                    for key, label, low, high in classifier.AMMONIA_RISK_LEVELS
                ],
                "age_temperature_bands": [
                    {
                        "min_week": min_week, "max_week": max_week,
                        "temp_min_c": temp_min, "temp_max_c": temp_max,
                    }
                    for min_week, max_week, temp_min, temp_max in classifier.AGE_TEMPERATURE_BANDS_C
                ],
            },
            "profile": profile.as_payload(),
            "data": [record.as_payload() for record in queryset],
        }
    )


@require_http_methods(["GET", "POST"])
@csrf_exempt  # Dashboard-only endpoint, but kept exempt for parity with submit/; CSRF isn't meaningful for same-origin JSON fetch here.
def flock_profile(request: HttpRequest) -> JsonResponse:
    """
    GET  -> current flock profile (age, custom overrides, active thresholds).
    POST -> update age_weeks / use_custom_thresholds / custom_* fields.
             Saving from here is what flips `is_configured` on, switching
             ingestion from the classifier's fixed defaults to this profile.
    """
    profile = FlockProfile.current()

    if request.method == "GET":
        return JsonResponse({"status": "ok", "profile": profile.as_payload()})

    try:
        payload = json.loads(request.body.decode("utf-8"))
    except (json.JSONDecodeError, UnicodeDecodeError):
        return _error("Request body must be valid UTF-8 JSON.")
    if not isinstance(payload, dict):
        return _error("Payload root must be a JSON object.")

    if "age_weeks" in payload:
        raw = payload["age_weeks"]
        if isinstance(raw, bool) or not isinstance(raw, (int, float)) or raw < 1:
            return _error("Field 'age_weeks' must be a positive number.")
        profile.age_weeks = int(raw)

    if "use_custom_thresholds" in payload:
        raw = payload["use_custom_thresholds"]
        if not isinstance(raw, bool):
            return _error("Field 'use_custom_thresholds' must be a boolean.")
        profile.use_custom_thresholds = raw

    for field in ("custom_temp_min_c", "custom_temp_max_c", "custom_ammonia_critical_ppm"):
        if field not in payload:
            continue
        raw = payload[field]
        if raw is None:
            setattr(profile, field, None)
            continue
        if isinstance(raw, bool) or not isinstance(raw, (int, float)):
            return _error(f"Field '{field}' must be numeric or null.")
        setattr(profile, field, float(raw))

    if (
        profile.use_custom_thresholds
        and profile.custom_temp_min_c is not None
        and profile.custom_temp_max_c is not None
        and profile.custom_temp_min_c >= profile.custom_temp_max_c
    ):
        return _error("custom_temp_min_c must be less than custom_temp_max_c.")

    # Any explicit save is what opts ingestion into profile-driven
    # thresholds -- a fresh deployment's untouched profile keeps the
    # classifier's original fixed defaults.
    profile.is_configured = True
    profile.save()

    return JsonResponse({"status": "ok", "profile": profile.as_payload()})


@require_http_methods(["GET", "POST"])
@csrf_exempt  # Dashboard-only endpoint, kept exempt for parity with profile/.
def actuator_control(request: HttpRequest) -> JsonResponse:
    """
    GET  -> current actuator control state, plus the effective fan/heater
             duty that's actually in force right now (AUTO-derived from the
             latest classification, or the MANUAL fields verbatim).
    POST -> update mode / fan_speed_pct / heater_power_pct. Switching to
             MANUAL immediately pins fan/heater duty to the saved percentages
             on the next ingestion round trip (see submit_telemetry's
             embedded `control` block); switching back to AUTO resumes the
             classifier-driven mapping with no further action needed.
    """
    control = ActuatorControl.current()
    latest = PoultryTelemetry.objects.order_by("-timestamp").first()
    latest_state = latest.predicted_class if latest else None
    latest_predicted_ammonia = latest.predicted_ammonia if latest else None

    # Raw temperature-vs-threshold check against the *current* active
    # profile, independent of latest_state -- see auto_duty_for_state()'s
    # docstring for why the classification label alone can silently mask a
    # simultaneous cold + critical-ammonia reading.
    profile = FlockProfile.current()
    low_temp_c, _heat_stress_temp_c = profile.active_temperature_band()
    latest_is_low_temperature = latest is not None and latest.temperature < low_temp_c

    if request.method == "GET":
        return JsonResponse({
            "status": "ok",
            # Top-level, not nested under "control" -- the ESP32-S3 master's
            # on-device predictive model takes flock age as an input feature
            # (see hardware/esp32s3_master/README.md's model-input notes)
            # and polls this endpoint independently of dashboard actuator
            # mode, so it needs this regardless of AUTO/MANUAL.
            "age_weeks": profile.age_weeks,
            "control": control.as_payload(latest_state, latest_predicted_ammonia, latest_is_low_temperature),
        })

    try:
        payload = json.loads(request.body.decode("utf-8"))
    except (json.JSONDecodeError, UnicodeDecodeError):
        return _error("Request body must be valid UTF-8 JSON.")
    if not isinstance(payload, dict):
        return _error("Payload root must be a JSON object.")

    if "mode" in payload:
        raw_mode = payload["mode"]
        if raw_mode not in ActuatorMode.values:
            return _error(f"Field 'mode' must be one of {list(ActuatorMode.values)}.")
        control.mode = raw_mode

    if "fan_speed_pct" in payload:
        raw = payload["fan_speed_pct"]
        if isinstance(raw, bool) or not isinstance(raw, (int, float)):
            return _error("Field 'fan_speed_pct' must be numeric.")
        value = int(raw)
        if not (0 <= value <= 100):
            return _error(f"Field 'fan_speed_pct'={value} must be within [0, 100].")
        control.fan_speed_pct = value

    if "heater_power_pct" in payload:
        raw = payload["heater_power_pct"]
        if isinstance(raw, bool) or not isinstance(raw, (int, float)):
            return _error("Field 'heater_power_pct' must be numeric.")
        value = int(raw)
        # The heater is switched by a relay (see hardware/esp32/include/config.h
        # PIN_HEATER_RELAY) -- there's no PWM/variable power on this leg like
        # there is for the fan, so only fully-off or fully-on is a real state.
        if value not in (0, 100):
            return _error(f"Field 'heater_power_pct'={value} must be 0 (off) or 100 (on) -- the heater is relay-switched, no variable power.")
        control.heater_power_pct = value

    control.save()
    return JsonResponse({
        "status": "ok",
        "control": control.as_payload(latest_state, latest_predicted_ammonia, latest_is_low_temperature),
    })


def _parse_range_boundary(raw: str, *, end_of_day: bool):
    """Parse a `start`/`end` query param as either a full ISO datetime or a
    bare date (YYYY-MM-DD), returning a timezone-aware datetime or None on a
    malformed string. A bare date resolves to that day's start/end so
    `start=2026-08-01&end=2026-08-01` covers the whole day inclusively."""
    parsed = parse_datetime(raw)
    if parsed is None:
        date_only = parse_date(raw)
        if date_only is None:
            return None
        time_component = datetime.max.time().replace(microsecond=0) if end_of_day else datetime.min.time()
        parsed = datetime.combine(date_only, time_component)
    if timezone.is_naive(parsed):
        parsed = timezone.make_aware(parsed)
    return parsed


def _windowed_queryset(request: HttpRequest):
    """
    Shared time-range resolution for /export/ and /report/: explicit
    `start`/`end` query params take precedence; otherwise falls back to the
    same trailing `hours` window semantics as /historical/.

    Returns (queryset, None) on success, or (None, JsonResponse) on a
    malformed query param -- callers should return the second element as-is.
    """
    start_raw = request.GET.get("start")
    end_raw = request.GET.get("end")

    if start_raw or end_raw:
        start = _parse_range_boundary(start_raw, end_of_day=False) if start_raw else None
        end = _parse_range_boundary(end_raw, end_of_day=True) if end_raw else None
        if start_raw and start is None:
            return None, _error(f"Query param 'start' is not a valid date/datetime: '{start_raw}'.")
        if end_raw and end is None:
            return None, _error(f"Query param 'end' is not a valid date/datetime: '{end_raw}'.")
        if start and end and start > end:
            return None, _error("Query param 'start' must not be after 'end'.")

        queryset = PoultryTelemetry.objects.order_by("timestamp")
        if start:
            queryset = queryset.filter(timestamp__gte=start)
        if end:
            queryset = queryset.filter(timestamp__lte=end)
        return queryset, None

    raw_hours = request.GET.get("hours", str(DEFAULT_WINDOW_HOURS))
    try:
        hours = float(raw_hours)
    except ValueError:
        return None, _error(f"Query param 'hours' must be numeric, got '{raw_hours}'.")
    if not (0 < hours <= MAX_WINDOW_HOURS):
        return None, _error(f"Query param 'hours' must be in (0, {MAX_WINDOW_HOURS}].")

    cutoff = timezone.now() - timedelta(hours=hours)
    queryset = PoultryTelemetry.objects.filter(timestamp__gte=cutoff).order_by("timestamp")
    return queryset, None


@require_GET
def export_telemetry(request: HttpRequest) -> HttpResponse:
    """
    CSV export of telemetry history.

    Query params (either style, `start`/`end` takes precedence if given):
        hours (optional float, default 24): trailing window, as /historical/.
        start / end (optional ISO date or datetime): explicit inclusive range.
    """
    queryset, error_response = _windowed_queryset(request)
    if error_response is not None:
        return error_response

    response = HttpResponse(content_type="text/csv")
    filename = f"poultry-telemetry-{timezone.now():%Y%m%d-%H%M%S}.csv"
    response["Content-Disposition"] = f'attachment; filename="{filename}"'

    writer = csv.writer(response)
    writer.writerow([
        "timestamp", "temperature_c", "humidity_pct", "ammonia_ppm",
        "predicted_temperature_c", "predicted_ammonia_ppm",
        "predicted_spike_probability", "predicted_class",
    ])
    for record in queryset.iterator():
        writer.writerow([
            record.timestamp.isoformat(),
            record.temperature,
            record.humidity,
            record.ammonia_level,
            record.predicted_temperature if record.predicted_temperature is not None else "",
            record.predicted_ammonia if record.predicted_ammonia is not None else "",
            record.predicted_spike_probability if record.predicted_spike_probability is not None else "",
            record.predicted_class,
        ])
    return response


@require_GET
def report_summary(request: HttpRequest) -> JsonResponse:
    """
    Aggregate summary over the same `hours` or `start`/`end` window as
    /export/, for the dashboard's Reports tab: record count, min/avg/max per
    sensor channel, and a count per classification state.
    """
    queryset, error_response = _windowed_queryset(request)
    if error_response is not None:
        return error_response

    aggregates = queryset.aggregate(
        avg_temperature=Avg("temperature"), min_temperature=Min("temperature"), max_temperature=Max("temperature"),
        avg_humidity=Avg("humidity"), min_humidity=Min("humidity"), max_humidity=Max("humidity"),
        avg_ammonia_level=Avg("ammonia_level"), min_ammonia_level=Min("ammonia_level"), max_ammonia_level=Max("ammonia_level"),
    )
    state_counts_raw = dict(
        queryset.values_list("predicted_class").annotate(count=Count("id")).order_by()
    )

    first_record = queryset.first()
    last_record = queryset.last()

    return JsonResponse({
        "status": "ok",
        "count": queryset.count(),
        "range": {
            "start": first_record.timestamp.isoformat() if first_record else None,
            "end": last_record.timestamp.isoformat() if last_record else None,
        },
        "averages": {
            key: (round(value, 2) if value is not None else None)
            for key, value in aggregates.items()
        },
        "state_counts": {
            state: state_counts_raw.get(state, 0) for state in EnvironmentalState.values
        },
    })


@require_GET
def test_case_reel(request: HttpRequest) -> JsonResponse:
    """
    GET -> the full Test Cases demo reel: a precomputed, deterministic
    sequence of frames covering every named scenario in telemetry/ml/
    scenarios.py, each one run through the REAL classify_environment()
    and REAL ActuatorControl.auto_duty_for_state() plus a pure-Python
    reimplementation of the on-device model (telemetry/ml/forecast_model.py
    -- the real one only exists as TFLite Micro C++ on the ESP32-S3, this
    is a from-scratch port of the same trained weights, verified to match
    within 1e-6, see telemetry/ml/export_weights.py).

    Purely a read/compute endpoint: no PoultryTelemetry record is ever
    created, and the real ActuatorControl singleton row is never read via
    .current() or written via .save() -- see telemetry/ml/reel.py's module
    docstring. Intended for the dashboard's Test Cases tab to fetch once
    and then animate through client-side; not polled.
    """
    return JsonResponse({"status": "ok", **test_case_reel_builder.build_demo_reel()})


def dashboard(request: HttpRequest):
    """Serve the analytical dashboard shell. All data arrives via the JSON API.

    dashboard_js_version is dashboard.js's own mtime, appended as a
    cache-busting ?v= query string in the template -- this file gets edited
    often during development, and the plain {% static %} URL never changes,
    so a browser (or an intermediate proxy) that already cached an old copy
    keeps serving it indefinitely with no visible sign anything is stale.
    A real incident this caused: a bug fix landed and was verified correct
    server-side (confirmed serving the updated file, confirmed the API
    returning the right data) while a browser tab kept rendering the old
    broken behavior regardless of a manual refresh, because the cached
    script was still the one actually executing. Deriving the version from
    mtime means every edit automatically busts the cache with no manual
    version bump to remember.
    """
    js_path = finders.find("telemetry/dashboard.js")
    js_version = int(os.path.getmtime(js_path)) if js_path else 0
    return render(request, "telemetry/dashboard.html", {"dashboard_js_version": js_version})
