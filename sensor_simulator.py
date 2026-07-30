#!/usr/bin/env python3
"""
sensor_simulator.py -- Virtual ESP32 telemetry client.

Runs fully independently of the Django process. Generates a continuous
stream of physically plausible environmental readings and transmits them
to the ingestion API over real HTTP.

Signal model:
- Baseline behavior is a bounded random walk per channel: each tick applies
  a small Gaussian step plus a mean-reversion pull toward the channel's
  setpoint. This produces smooth drift instead of white-noise jumping.
- Two deterministic anomaly generators are superimposed:
    1. THERMAL SPIKE: every HEAT_SPIKE_PERIOD_S seconds, a transient heat
       event ramps temperature above 37 C and decays back over ~30 s.
    2. AMMONIA ACCUMULATION CYCLE: NH3 climbs monotonically (litter gas
       buildup) until it breaches 30 PPM, at which point a simulated
       ventilation fan engages and drives the level back down to baseline;
       the cycle then re-arms.

Payload structure (matches the Edge-AI-ready ingestion contract):
    {
        "temperature": float,
        "humidity": float,
        "ammonia_level": float,
        "predicted_temperature": float | null,
        "predicted_ammonia": float | null,
        "predicted_spike_probability": float | null
    }
The forecast channels emulate a lightweight on-device TinyML regressor
(short-horizon exponential extrapolation) and are enabled by default so
the dashboard's KPI forecast lines, dashed chart series, and console
(Pred: ...) markers have live numbers during demos. Pass --no-edge-ai
to transmit null forecasts, matching the deployment state before the
physical ESP32-S3 inference firmware is linked.

`predicted_spike_probability` emulates the ESP32-S3 master node's TFLite
Micro spike classifier (real firmware: esp32s3_master/), which is only
reachable over ESP-NOW from a physical sensor node. The simulator stands in
with a logistic function of ammonia proximity to the ventilation trigger, so
the dashboard's spike-risk tile has plausible live numbers before the
master-node hardware is linked.

Usage:
    pip install requests
    python sensor_simulator.py [--url http://127.0.0.1:8000/api/telemetry/submit/] [--no-edge-ai]
"""

import argparse
import math
import random
import sys
import time
from datetime import datetime

import requests

# ---------------------------------------------------------------------------
# Timing configuration
# ---------------------------------------------------------------------------
TRANSMIT_INTERVAL_S = 5          # One payload every 5 seconds
HEAT_SPIKE_PERIOD_S = 120        # Thermal anomaly injected every 2 minutes
HEAT_SPIKE_DURATION_S = 30       # Spike rise-and-decay envelope length

# ---------------------------------------------------------------------------
# Channel setpoints and physical clamps
# ---------------------------------------------------------------------------
TEMP_SETPOINT_C = 27.0
TEMP_CLAMP = (10.0, 45.0)
TEMP_STEP_SIGMA = 0.25           # Random-walk step scale per tick
TEMP_REVERSION = 0.06            # Mean-reversion coefficient toward setpoint

HUMIDITY_SETPOINT_PCT = 62.0
HUMIDITY_CLAMP = (30.0, 95.0)
HUMIDITY_STEP_SIGMA = 0.60
HUMIDITY_REVERSION = 0.05

AMMONIA_BASELINE_PPM = 6.0
AMMONIA_CLAMP = (0.0, 60.0)
AMMONIA_NOISE_SIGMA = 0.20       # Sensor jitter on top of the accumulation ramp
AMMONIA_ACCUMULATION_RATE = 0.55  # PPM gained per tick while fan is off
AMMONIA_FAN_TRIGGER_PPM = 30.0   # Ventilation engages above this level
AMMONIA_FAN_DECAY = 0.78         # Multiplicative decay per tick while venting
AMMONIA_FAN_RELEASE_PPM = 7.0    # Fan disengages once level returns near baseline

# Spike-risk emulation (stands in for the ESP32-S3 master's TFLite classifier
# output). Logistic curve centered below the fan trigger so risk visibly
# climbs before the ventilation cycle actually engages.
SPIKE_RISK_CENTER_PPM = AMMONIA_FAN_TRIGGER_PPM * 0.75
SPIKE_RISK_SCALE_PPM = 4.0
SPIKE_RISK_NOISE_SIGMA = 0.02


def clamp(value: float, bounds: tuple) -> float:
    """Constrain a channel to its physical envelope."""
    lower, upper = bounds
    return max(lower, min(upper, value))


class VirtualSensorArray:
    """
    Stateful signal generator for one simulated poultry house.

    Holds per-channel state across ticks so each transmitted point is a
    small perturbation of the previous one (true random walk), and drives
    the two anomaly state machines.
    """

    def __init__(self, edge_ai_enabled: bool = False) -> None:
        self.temperature = TEMP_SETPOINT_C
        self.humidity = HUMIDITY_SETPOINT_PCT
        self.ammonia = AMMONIA_BASELINE_PPM
        self.fan_active = False
        self.start_monotonic = time.monotonic()

        # Edge-AI emulation state. When disabled (default, matching current
        # hardware reality), forecast keys transmit as null. When enabled,
        # a trend-following exponential smoother produces a plausible
        # short-horizon forecast per channel.
        self.edge_ai_enabled = edge_ai_enabled
        self._prev_temperature = None
        self._prev_ammonia = None

    # -- Baseline random walk -------------------------------------------------
    def _walk(self, current: float, setpoint: float, sigma: float,
              reversion: float, bounds: tuple) -> float:
        """One Ornstein-Uhlenbeck-style step: Gaussian noise + pull to setpoint."""
        step = random.gauss(0.0, sigma)
        pull = reversion * (setpoint - current)
        return clamp(current + step + pull, bounds)

    # -- Anomaly generator 1: periodic thermal spike ---------------------------
    def _thermal_spike_offset(self) -> float:
        """
        Triangular spike envelope. Peaks mid-window at a magnitude guaranteed
        to push the reading past 37 C relative to the setpoint band.
        """
        elapsed = time.monotonic() - self.start_monotonic
        phase = elapsed % HEAT_SPIKE_PERIOD_S
        if phase >= HEAT_SPIKE_DURATION_S:
            return 0.0
        # Normalized position in the spike window: 0 -> 1 -> 0
        half = HEAT_SPIKE_DURATION_S / 2.0
        envelope = 1.0 - abs(phase - half) / half
        peak_magnitude = 11.5  # 27 C setpoint + 11.5 C peak > 37 C requirement
        return envelope * peak_magnitude

    # -- Anomaly generator 2: ammonia accumulation / ventilation cycle ---------
    def _step_ammonia(self) -> None:
        if self.fan_active:
            # Exponential extraction while the ventilation fan runs.
            self.ammonia *= AMMONIA_FAN_DECAY
            if self.ammonia <= AMMONIA_FAN_RELEASE_PPM:
                self.fan_active = False
                log_event("VENTILATION FAN DISENGAGED", f"NH3 restored to {self.ammonia:.2f} PPM")
        else:
            # Linear litter off-gassing accumulation with sensor jitter.
            self.ammonia += AMMONIA_ACCUMULATION_RATE + random.gauss(0.0, AMMONIA_NOISE_SIGMA)
            if self.ammonia >= AMMONIA_FAN_TRIGGER_PPM:
                self.fan_active = True
                log_event("VENTILATION FAN ENGAGED", f"NH3 breached {self.ammonia:.2f} PPM")
        self.ammonia = clamp(self.ammonia, AMMONIA_CLAMP)

    # -- Public tick ------------------------------------------------------------
    def read(self) -> dict:
        """Advance all channels one tick and return the transmit payload."""
        self.temperature = self._walk(
            self.temperature, TEMP_SETPOINT_C, TEMP_STEP_SIGMA, TEMP_REVERSION, TEMP_CLAMP
        )
        self.humidity = self._walk(
            self.humidity, HUMIDITY_SETPOINT_PCT, HUMIDITY_STEP_SIGMA, HUMIDITY_REVERSION, HUMIDITY_CLAMP
        )
        self._step_ammonia()

        effective_temperature = clamp(
            self.temperature + self._thermal_spike_offset(), TEMP_CLAMP
        )

        predicted_temperature, predicted_ammonia = self._edge_ai_forecast(
            effective_temperature, self.ammonia
        )

        return {
            "temperature": round(effective_temperature, 2),
            "humidity": round(self.humidity, 2),
            "ammonia_level": round(self.ammonia, 2),
            # Nullable forecast channels; JSON null until edge hardware links.
            "predicted_temperature": predicted_temperature,
            "predicted_ammonia": predicted_ammonia,
            "predicted_spike_probability": self._spike_probability(self.ammonia),
        }

    # -- Master-node spike-risk emulation ---------------------------------------
    def _spike_probability(self, ammonia: float):
        """
        Return a plausible stand-in for the ESP32-S3 master's TFLite Micro
        spike-classifier probability, or None when Edge-AI emulation is off.

        Logistic curve of ammonia level relative to the ventilation trigger:
        risk rises smoothly as the accumulation cycle approaches the trigger
        point and falls back once the simulated fan starts extracting.
        """
        if not self.edge_ai_enabled:
            return None
        x = (ammonia - SPIKE_RISK_CENTER_PPM) / SPIKE_RISK_SCALE_PPM
        probability = 1.0 / (1.0 + math.exp(-x))
        probability += random.gauss(0.0, SPIKE_RISK_NOISE_SIGMA)
        return round(clamp(probability, (0.0, 1.0)), 4)

    # -- Edge-AI forecast emulation ---------------------------------------------
    def _edge_ai_forecast(self, temperature: float, ammonia: float):
        """
        Return (predicted_temperature, predicted_ammonia).

        Disabled mode: (None, None), serialized as JSON null -- the structural
        default while the ESP32-S3 inference firmware is not yet linked.

        Enabled mode: one-tick-ahead linear extrapolation from the previous
        reading (x_next ~= x + (x - x_prev)) with small Gaussian model noise,
        mimicking a lightweight on-device TinyML regressor.
        """
        if not self.edge_ai_enabled:
            return None, None

        if self._prev_temperature is None:
            forecast = (round(temperature, 2), round(ammonia, 2))
        else:
            temp_next = temperature + (temperature - self._prev_temperature)
            nh3_next = ammonia + (ammonia - self._prev_ammonia)
            forecast = (
                round(clamp(temp_next + random.gauss(0.0, 0.15), TEMP_CLAMP), 2),
                round(clamp(nh3_next + random.gauss(0.0, 0.10), AMMONIA_CLAMP), 2),
            )

        self._prev_temperature = temperature
        self._prev_ammonia = ammonia
        return forecast


def log_event(tag: str, detail: str) -> None:
    """Structured single-line stdout logging. No decorative characters."""
    stamp = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    print(f"{stamp} | {tag:<28} | {detail}")


def transmit(session: requests.Session, url: str, payload: dict) -> None:
    """POST one payload; failures are logged and skipped, never fatal."""
    try:
        response = session.post(url, json=payload, timeout=4)
        if response.status_code == 201:
            body = response.json()
            classification = body.get("record", {}).get("predicted_class", "UNKNOWN")
            pt = payload.get("predicted_temperature")
            pa = payload.get("predicted_ammonia")
            forecast = (
                f"  FCAST[T={pt:.2f} NH3={pa:.2f}]"
                if pt is not None and pa is not None else ""
            )
            spike = payload.get("predicted_spike_probability")
            spike_suffix = f"  SPIKE_RISK={spike * 100:.0f}%" if spike is not None else ""
            log_event(
                "TX OK",
                f"T={payload['temperature']:>6.2f} C  "
                f"RH={payload['humidity']:>6.2f} %  "
                f"NH3={payload['ammonia_level']:>6.2f} PPM  "
                f"CLASS={classification}{forecast}{spike_suffix}",
            )
        else:
            log_event("TX REJECTED", f"HTTP {response.status_code}: {response.text[:120]}")
    except requests.RequestException as exc:
        log_event("TX FAILED", f"{type(exc).__name__}: {exc}")


def main() -> int:
    parser = argparse.ArgumentParser(description="Virtual ESP32 poultry sensor client.")
    parser.add_argument(
        "--url",
        default="http://127.0.0.1:8000/api/telemetry/submit/",
        help="Ingestion endpoint URL.",
    )
    # Edge-AI forecasts are ON by default so the dashboard KPI forecast lines,
    # dashed chart series, and console (Pred: ...) markers have real numbers
    # to display during demos. Pass --no-edge-ai to transmit null forecasts,
    # matching the deployment state before the ESP32-S3 firmware is linked.
    parser.add_argument(
        "--no-edge-ai",
        dest="edge_ai",
        action="store_false",
        help="Disable ESP32-S3 Edge-AI forecast emulation; forecast keys transmit as null.",
    )
    parser.set_defaults(edge_ai=True)
    args = parser.parse_args()

    sensors = VirtualSensorArray(edge_ai_enabled=args.edge_ai)
    session = requests.Session()

    log_event("SIMULATOR START", f"Target endpoint: {args.url}")
    log_event(
        "CONFIG",
        f"Interval={TRANSMIT_INTERVAL_S}s  HeatSpikePeriod={HEAT_SPIKE_PERIOD_S}s  "
        f"EdgeAI={'ENABLED' if args.edge_ai else 'DISABLED (forecast keys transmit null)'}",
    )

    try:
        while True:
            tick_start = time.monotonic()
            transmit(session, args.url, sensors.read())
            # Drift-corrected sleep keeps the cadence locked to 5 s regardless
            # of network latency on the POST.
            elapsed = time.monotonic() - tick_start
            time.sleep(max(0.0, TRANSMIT_INTERVAL_S - elapsed))
    except KeyboardInterrupt:
        log_event("SIMULATOR STOP", "Interrupted by operator.")
        return 0


if __name__ == "__main__":
    sys.exit(main())
