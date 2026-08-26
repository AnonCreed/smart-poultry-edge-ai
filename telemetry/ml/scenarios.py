"""
scenarios.py -- the Test Cases demo reel's scenario definitions.

Transcribed verbatim from BENCHMARK_SCENARIOS in
hardware/esp32/src/main.cpp (search that file for the same names) so the
web demo shows the identical named conditions someone would see running
the real sensor node in esp32dev_benchmark mode -- same names, same
start/end values, just replayed here instantly instead of over real
5-second-cadence ESP-NOW packets.
"""
from dataclasses import dataclass

# How many animation frames each scenario gets in the demo reel. Frame 0
# uses the scenario's *_start values with a full pre-seeded bucket history
# (see reel.py) -- no warm-up wait. Frames 1..N-1 interpolate toward
# *_end, rolling the bucket window forward one new bucket per frame. 12
# frames/scenario x 13 scenarios = 156 frames; at ~500ms/frame client-side
# that's ~78s total playback, comfortably inside a short demo window.
FRAMES_PER_SCENARIO = 12


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


# Order matches hardware/esp32/src/main.cpp's BENCHMARK_SCENARIOS array
# exactly -- do not reorder without reordering both.
SCENARIOS: list[Scenario] = [
    Scenario(
        "OPTIMAL_BASELINE", "Optimal baseline",
        "Warm-up baseline; nothing should trip.",
        24.0, 24.0, 55.0, 55.0, 3.0, 3.0,
    ),
    Scenario(
        "AMMONIA_STEP_CRITICAL", "Ammonia step -- critical",
        "Instant jump past the 15ppm CRITICAL_AMMONIA threshold.",
        24.0, 24.0, 55.0, 55.0, 40.0, 40.0,
    ),
    Scenario(
        "AMMONIA_STEP_RECOVER", "Ammonia step -- recover",
        "Instant drop back to baseline.",
        24.0, 24.0, 55.0, 55.0, 3.0, 3.0,
    ),
    Scenario(
        "AMMONIA_RAMP_TO_SPIKE", "Ammonia ramp -- rising",
        "Gradual rise through the threshold -- does the forecast lead the live reading?",
        24.0, 24.0, 55.0, 55.0, 3.0, 30.0,
    ),
    Scenario(
        "AMMONIA_RAMP_RECOVER", "Ammonia ramp -- falling",
        "Gradual fall back down.",
        24.0, 24.0, 55.0, 55.0, 30.0, 3.0,
    ),
    Scenario(
        "AMMONIA_BOUNDARY_LOW", "Ammonia boundary -- just under",
        "Held just under the 15ppm threshold.",
        24.0, 24.0, 55.0, 55.0, 14.9, 14.9,
    ),
    Scenario(
        "AMMONIA_BOUNDARY_HIGH", "Ammonia boundary -- just over",
        "Held just over the 15ppm threshold.",
        24.0, 24.0, 55.0, 55.0, 15.1, 15.1,
    ),
    Scenario(
        "HEAT_STRESS_STEP", "Heat stress",
        "Temperature + humidity combined, both past threshold.",
        38.0, 38.0, 80.0, 80.0, 3.0, 3.0,
    ),
    Scenario(
        "HEAT_STRESS_RECOVER", "Heat stress -- recover",
        "Back to baseline.",
        24.0, 24.0, 55.0, 55.0, 3.0, 3.0,
    ),
    Scenario(
        "LOW_TEMP_STEP", "Low temperature",
        "Below the cold threshold -- heater reactivity.",
        15.0, 15.0, 55.0, 55.0, 3.0, 3.0,
    ),
    Scenario(
        "LOW_TEMP_RECOVER", "Low temperature -- recover",
        "Back to baseline.",
        24.0, 24.0, 55.0, 55.0, 3.0, 3.0,
    ),
    Scenario(
        "COLD_AND_CRITICAL_AMMONIA", "Cold AND critical ammonia",
        "Both at once -- classification resolves to CRITICAL_AMMONIA (ammonia "
        "checked first), but fan AND heater must both activate, not just the fan.",
        15.0, 15.0, 55.0, 55.0, 40.0, 40.0,
    ),
    Scenario(
        "COLD_AND_CRITICAL_RECOVER", "Cold AND critical ammonia -- recover",
        "Back to baseline.",
        24.0, 24.0, 55.0, 55.0, 3.0, 3.0,
    ),
]
