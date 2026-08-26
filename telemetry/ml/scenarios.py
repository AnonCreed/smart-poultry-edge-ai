"""
scenarios.py -- the Test Cases demo reel's case definitions.

v3: a focused set of six cases, each a HELD state (not a ramp) long enough
for the real fan/heater to actually respond and be seen/heard -- this reel
now drives the real ActuatorControl row via the dashboard's JS (see
dashboard.js's renderTestCaseFrame()), not just a chart, so pacing matters:
each case must outlast the master's 3s CONTROL_POLL_MS poll comfortably.

Replaces the earlier v2 list (13 scenarios mirroring hardware/esp32/src/
main.cpp's BENCHMARK_SCENARIOS, animated as smooth ramps for a pure-chart
simulation) -- this version demonstrates six distinct fan/heater outcomes
directly, one case per outcome, rather than a broad firmware-parity sweep.
"""
from dataclasses import dataclass

# Seconds each frame holds before advancing -- see dashboard.js's
# TEST_CASE_FRAME_MS. Comfortably longer than CONTROL_POLL_MS (3s,
# hardware/esp32s3_master/include/config.h) so the master's poll is
# guaranteed to pick up and relay every frame before it's replaced.
FRAME_HOLD_SECONDS = 6


@dataclass(frozen=True)
class Scenario:
    key: str
    label: str
    description: str
    temp_start: float
    temp_end: float
    humidity_start: float
    humidity_end: float
    ammonia_start: float
    ammonia_end: float
    frames: int = 1  # >1 only for a deliberate before/after cut (see FAN_HIGH_SPIKE)


SCENARIOS: list[Scenario] = [
    Scenario(
        "FAN_OFF", "Fan off (optimal)",
        "Nothing to react to -- fan and heater both stay off.",
        24.0, 24.0, 55.0, 55.0, 3.0, 3.0,
    ),
    Scenario(
        "FAN_LOW", "Fan low (predictive)",
        "Ammonia elevated but not yet critical -- the forecast pulls the "
        "fan up proportionally, ahead of any live threshold breach.",
        24.0, 24.0, 55.0, 55.0, 10.0, 10.0,
    ),
    Scenario(
        "FAN_HIGH_SPIKE", "Fan high (sudden spike)",
        "Ammonia jumps straight to 40ppm, well past the 15ppm critical "
        "threshold -- fan snaps to 100% the instant it's crossed.",
        24.0, 24.0, 55.0, 55.0, 3.0, 40.0,
        frames=2,  # frame 0 = before (3ppm), frame 1 = after (40ppm, critical)
    ),
    Scenario(
        "HEATER_ON", "Heater on (cold)",
        "Below the cold threshold -- heater relay energizes.",
        10.0, 10.0, 55.0, 55.0, 3.0, 3.0,
    ),
    Scenario(
        "HEATER_OFF", "Heater off (warm)",
        "Back to a comfortable temperature -- heater relay de-energizes.",
        24.0, 24.0, 55.0, 55.0, 3.0, 3.0,
    ),
    Scenario(
        "FAN_HEATER_COMBINED", "Fan + heater together",
        "Cold AND critical ammonia at once -- classification resolves to "
        "CRITICAL_AMMONIA (ammonia checked first), but the safety floor "
        "forces fan AND heater to 100% simultaneously, not just one.",
        15.0, 15.0, 55.0, 55.0, 40.0, 40.0,
    ),
]
