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

import json
import logging
from datetime import timedelta

from django.http import HttpRequest, JsonResponse
from django.shortcuts import render
from django.utils import timezone
from django.views.decorators.csrf import csrf_exempt
from django.views.decorators.http import require_GET, require_POST

from . import classifier
from .models import PoultryTelemetry

# Dedicated ingestion logger. Server-side log lines mirror the front-end
# console feed format so `journalctl -u <service>` and the browser view show
# identical text and can be cross-referenced during audits.
logger = logging.getLogger("telemetry.ingest")

# Ingestion validation bounds. These are plausibility gates for sensor faults
# (disconnected probe, I2C noise), not classification thresholds.
FIELD_BOUNDS = {
    "temperature": (-40.0, 85.0),   # DHT22/SHT31 physical operating envelope
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

    # Classification remains driven exclusively by live sensor readings;
    # forecasts are advisory overlays and never alter the stored state.
    predicted = classifier.classify_environment(
        temperature=values["temperature"],
        humidity=values["humidity"],
        ammonia_level=values["ammonia_level"],
    )

    record = PoultryTelemetry.objects.create(
        predicted_class=predicted, **values, **predictions
    )

    # Structured ingestion log. Format matches the browser console feed so
    # the two views can be cross-referenced during audits without translation.
    def _fmt(value: float | None, suffix: str) -> str:
        return f"{value:.1f}{suffix}" if value is not None else "n/a"

    logger.info(
        "[%s] INGESTION SUCCESS -> "
        "T: %.1fC (Pred: %s) | RH: %.1f%% | NH3: %.1fppm (Pred: %s) | STATE: %s",
        record.timestamp.strftime("%Y-%m-%d %H:%M:%S"),
        values["temperature"],
        _fmt(predictions["predicted_temperature"], "C"),
        values["humidity"],
        values["ammonia_level"],
        _fmt(predictions["predicted_ammonia"], "ppm"),
        predicted,
    )

    return JsonResponse(
        {"status": "ok", "record": record.as_payload()},
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

    return JsonResponse(
        {
            "status": "ok",
            "window_hours": hours,
            "count": queryset.count(),
            "thresholds": {
                "ammonia_critical_ppm": classifier.AMMONIA_CRITICAL_PPM,
                "heat_stress_temp_c": classifier.HEAT_STRESS_TEMP_C,
                "heat_stress_humidity_pct": classifier.HEAT_STRESS_HUMIDITY_PCT,
                "low_temp_c": classifier.LOW_TEMP_C,
            },
            "data": [record.as_payload() for record in queryset],
        }
    )


def dashboard(request: HttpRequest):
    """Serve the analytical dashboard shell. All data arrives via the JSON API."""
    return render(request, "telemetry/dashboard.html")
