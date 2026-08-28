"""
Data model for the poultry environmental telemetry pipeline.

Design decisions:
- `timestamp` is auto-populated (`auto_now_add`) and carries a B-tree index:
  every read path in the system (historical windows, latest-N audit slices)
  filters or orders on this column, so the index is the primary performance
  lever for time-series retrieval.
- `predicted_class` is constrained by TextChoices to guarantee the API layer
  and the front-end badge renderer share a closed vocabulary of states.
- Sensor magnitudes are stored as raw floats; unit normalization (Celsius,
  %RH, PPM) is enforced at the ingestion boundary, not in the schema.
"""

from django.db import models


class EnvironmentalState(models.TextChoices):
    """Closed classification vocabulary shared by backend and UI layers."""

    CRITICAL_AMMONIA = "CRITICAL_AMMONIA", "Critical Ammonia"
    HEAT_STRESS_WARNING = "HEAT_STRESS_WARNING", "Heat Stress Warning"
    LOW_TEMP_ALERT = "LOW_TEMP_ALERT", "Low Temperature Alert"
    OPTIMAL_ENVIRONMENT = "OPTIMAL_ENVIRONMENT", "Optimal Environment"


class ActuatorMode(models.TextChoices):
    """AUTO mirrors the sensor node's original state-driven duty mapping;
    MANUAL lets an operator drive fan/heater duty directly from the dashboard."""

    AUTO = "AUTO", "Automatic (classifier-driven)"
    MANUAL = "MANUAL", "Manual override"


class PoultryTelemetry(models.Model):
    """
    One immutable telemetry record per sensor transmission.

    Records are append-only from the application's perspective; the audit
    log view treats this table as an event stream.
    """

    timestamp = models.DateTimeField(
        auto_now_add=True,
        db_index=True,
        help_text="Server-side ingestion time (UTC). Indexed for range scans.",
    )
    temperature = models.FloatField(help_text="Ambient temperature in degrees Celsius.")
    humidity = models.FloatField(help_text="Relative humidity in percent.")
    ammonia_level = models.FloatField(help_text="NH3 concentration in PPM.")

    # Edge-AI forecast channels. Nullable by design: the ESP32-S3 inference
    # firmware is not yet linked, so ingestion must accept payloads with these
    # keys absent or explicitly null and remain structurally ready for them.
    predicted_temperature = models.FloatField(
        null=True,
        blank=True,
        help_text="ESP32-S3 Edge-AI temperature forecast in degrees Celsius. Null until hardware is linked.",
    )
    predicted_ammonia = models.FloatField(
        null=True,
        blank=True,
        help_text="ESP32-S3 Edge-AI NH3 forecast in PPM. Null until hardware is linked.",
    )
    predicted_spike_probability = models.FloatField(
        null=True,
        blank=True,
        help_text=(
            "Master-node TFLite Micro classifier output in [0, 1]: probability of an "
            "imminent ammonia spike, computed from a rolling window of sensor-node "
            "readings relayed over ESP-NOW. Null until the ESP32-S3 master is linked."
        ),
    )

    predicted_class = models.CharField(
        max_length=32,
        choices=EnvironmentalState.choices,
        help_text="Server-computed environmental classification at ingestion time.",
    )

    # Countdown, not a reading: how many seconds the master's on-device model
    # reported (at transmission time) until its next prediction -- the very
    # first one during the ~70-minute warm-up, or the next steady-state
    # refresh once warmed up. Nullable for the same reason the forecast
    # channels above are: absent whenever the ESP32-S3 master isn't linked,
    # or (in principle) if a future master firmware build predates this
    # field. Not itself a forecast value, so it's kept out of the
    # "Edge-AI forecast channels" block above.
    model_seconds_to_next = models.PositiveIntegerField(
        null=True,
        blank=True,
        help_text=(
            "Seconds until the master node's on-device model produces its next "
            "prediction, as reported by the master at transmission time. Covers "
            "both the initial ~70-minute warm-up and every ~10-minute refresh "
            "after. Null until the ESP32-S3 master is linked."
        ),
    )

    class Meta:
        ordering = ["-timestamp"]
        verbose_name = "Poultry telemetry record"
        verbose_name_plural = "Poultry telemetry records"

    def __str__(self) -> str:
        return (
            f"[{self.timestamp:%Y-%m-%d %H:%M:%S}] "
            f"T={self.temperature:.2f}C RH={self.humidity:.2f}% "
            f"NH3={self.ammonia_level:.2f}ppm -> {self.predicted_class}"
        )

    def as_payload(self) -> dict:
        """Serialize to the canonical JSON contract consumed by chart clients."""
        return {
            "id": self.pk,
            "timestamp": self.timestamp.isoformat(),
            "temperature": round(self.temperature, 2),
            "humidity": round(self.humidity, 2),
            "ammonia_level": round(self.ammonia_level, 2),
            # Forecast channels serialize as JSON null when absent so chart
            # clients can rely on key presence and simply skip null points.
            "predicted_temperature": (
                round(self.predicted_temperature, 2)
                if self.predicted_temperature is not None else None
            ),
            "predicted_ammonia": (
                round(self.predicted_ammonia, 2)
                if self.predicted_ammonia is not None else None
            ),
            "predicted_spike_probability": (
                round(self.predicted_spike_probability, 4)
                if self.predicted_spike_probability is not None else None
            ),
            "predicted_class": self.predicted_class,
            "model_seconds_to_next": self.model_seconds_to_next,
        }


class FlockProfile(models.Model):
    """
    Singleton settings row for the active flock: the chick age (or a fully
    custom override) that drives the age-appropriate temperature band used
    at ingestion classification and echoed to chart clients as the active
    threshold lines.

    `is_configured` defaults False so a freshly-migrated deployment keeps
    classifying against the classifier module's original fixed constants --
    identical to pre-profile behavior -- until a user explicitly saves an
    age or custom profile from the dashboard. A single row is sufficient
    because this deployment tracks one house/one flock at a time (see
    PoultryTelemetry); a multi-house rollout would key this by house instead
    of hardcoding SINGLETON_ID.
    """

    SINGLETON_ID = 1

    is_configured = models.BooleanField(
        default=False,
        help_text="False until a user has saved a profile from the dashboard; while false, ingestion uses the classifier's fixed defaults.",
    )
    age_weeks = models.PositiveSmallIntegerField(
        default=1,
        help_text="Current flock age in weeks. Drives the age-appropriate temperature band (Table 4-2) unless custom thresholds are enabled.",
    )
    use_custom_thresholds = models.BooleanField(
        default=False,
        help_text="When true, the custom_* fields below override the age-derived temperature band and the default ammonia critical threshold.",
    )
    custom_temp_min_c = models.FloatField(
        null=True, blank=True,
        help_text="Custom low-temperature alert threshold in degrees Celsius.",
    )
    custom_temp_max_c = models.FloatField(
        null=True, blank=True,
        help_text="Custom heat-stress temperature threshold in degrees Celsius (paired with the fixed >70% humidity conjunction).",
    )
    custom_ammonia_critical_ppm = models.FloatField(
        null=True, blank=True,
        help_text="Custom ammonia critical threshold in PPM, overriding the classifier's 15.0 ppm default.",
    )
    updated_at = models.DateTimeField(auto_now=True)

    class Meta:
        verbose_name = "Flock profile"
        verbose_name_plural = "Flock profile"

    def save(self, *args, **kwargs):
        self.pk = self.SINGLETON_ID
        super().save(*args, **kwargs)

    @classmethod
    def current(cls) -> "FlockProfile":
        obj, _ = cls.objects.get_or_create(pk=cls.SINGLETON_ID)
        return obj

    def active_temperature_band(self) -> tuple[float, float]:
        """Resolve the (low_temp_c, heat_stress_temp_c) pair currently in effect."""
        from . import classifier

        if not self.is_configured:
            return classifier.LOW_TEMP_C, classifier.HEAT_STRESS_TEMP_C
        if (
            self.use_custom_thresholds
            and self.custom_temp_min_c is not None
            and self.custom_temp_max_c is not None
        ):
            return self.custom_temp_min_c, self.custom_temp_max_c
        return classifier.temperature_band_for_age(self.age_weeks)

    def active_ammonia_critical_ppm(self) -> float:
        from . import classifier

        if (
            self.is_configured
            and self.use_custom_thresholds
            and self.custom_ammonia_critical_ppm is not None
        ):
            return self.custom_ammonia_critical_ppm
        return classifier.AMMONIA_CRITICAL_PPM

    def as_payload(self) -> dict:
        low_temp_c, heat_stress_temp_c = self.active_temperature_band()
        return {
            "is_configured": self.is_configured,
            "age_weeks": self.age_weeks,
            "use_custom_thresholds": self.use_custom_thresholds,
            "custom_temp_min_c": self.custom_temp_min_c,
            "custom_temp_max_c": self.custom_temp_max_c,
            "custom_ammonia_critical_ppm": self.custom_ammonia_critical_ppm,
            "active_low_temp_c": low_temp_c,
            "active_heat_stress_temp_c": heat_stress_temp_c,
            "active_ammonia_critical_ppm": self.active_ammonia_critical_ppm(),
        }


class ActuatorControl(models.Model):
    """
    Singleton settings row for fan/heater actuator control.

    AUTO (default) has two layers:
    - Heater: driven by a raw temperature-vs-threshold check
      (`is_low_temperature`, computed by the caller -- see submit_telemetry()
      and actuator_control() in views.py), NOT by the classification label.
      classify_environment() returns one mutually-exclusive state with
      ammonia checked first, so a reading that's simultaneously
      CRITICAL_AMMONIA and genuinely cold classifies only as
      CRITICAL_AMMONIA -- checking `state == LOW_TEMP_ALERT` would then
      leave the heater off while it's actually cold, exactly when a chick
      is both gassed and freezing. Checking the raw temperature directly
      sidesteps that masking entirely.
    - Fan: predictive when the ESP32-S3 master's Edge-AI forecast is
      available, mirroring the on-device proportional control it actually
      applies to hardware (see the `fanPwm` calculation in
      hardware/esp32s3_master/src/main.cpp) -- duty scales with forecast
      ammonia error above a 5 ppm setpoint, not the live reading. A live
      CRITICAL_AMMONIA/HEAT_STRESS_WARNING reading always forces the fan to
      100% regardless of the forecast, since a reading already past the
      safety threshold must not wait on a prediction to react. Falls back
      to the pre-ESP32S3-link binary classification mapping (fan 100% on
      CRITICAL_AMMONIA/HEAT_STRESS_WARNING, else 0%) when no forecast is
      available yet.

    MANUAL lets an operator pin fan speed and heater state directly from
    the dashboard, independent of the live classification -- ingestion
    still classifies and persists the environmental state as normal either
    way; only the *actuator* duty is overridden. The heater is
    relay-switched (on/off only, no variable power) -- only the fan has
    real PWM speed control.

    MANUAL is not absolute, though: the same safety floor AUTO applies
    (see `_apply_safety_floor()`) still wins over a MANUAL setting. Fan
    and heater running together isn't blanket-forbidden -- there are
    legitimate conditions where both are genuinely needed at once (e.g.
    cold air that still needs circulating), so this isn't a fan-XOR-heater
    interlock -- but an operator's MANUAL duty is only honored right up
    until a live reading crosses a real safety threshold (critical
    ammonia, heat stress, or genuine cold), at which point that threshold
    overrides the manual setting automatically rather than waiting on the
    operator to notice and intervene. This matters even with no animals
    currently in the coop: the policy needs to be correct for when there
    are.

    Effective duty is resolved server-side (not on-device) so a firmware
    client only has to relay one ready-to-apply command rather than
    re-implement this policy switch itself.
    """

    SINGLETON_ID = 1

    mode = models.CharField(
        max_length=8, choices=ActuatorMode.choices, default=ActuatorMode.AUTO,
        help_text="AUTO derives fan/heater duty from the latest classification; MANUAL uses the fields below.",
    )
    fan_speed_pct = models.PositiveSmallIntegerField(
        default=0,
        help_text=(
            "Manual fan speed, 0-100%. Only applied while mode=MANUAL, and "
            "even then only up until a live reading crosses a safety "
            "threshold (critical ammonia or heat stress) -- see "
            "effective_duty()'s safety floor, which overrides this."
        ),
    )
    heater_power_pct = models.PositiveSmallIntegerField(
        default=0,
        help_text=(
            "Manual heater state, 0 (off) or 100 (on) only -- the heater is a "
            "relay switch, not PWM, so there's no variable power in between. "
            "Only applied while mode=MANUAL, and even then only up until a "
            "live reading crosses a safety threshold (genuine cold forces "
            "it on, heat stress forces it off) -- see effective_duty()'s "
            "safety floor, which overrides this."
        ),
    )
    updated_at = models.DateTimeField(auto_now=True)

    class Meta:
        verbose_name = "Actuator control"
        verbose_name_plural = "Actuator control"

    def save(self, *args, **kwargs):
        self.pk = self.SINGLETON_ID
        super().save(*args, **kwargs)

    @classmethod
    def current(cls) -> "ActuatorControl":
        obj, _ = cls.objects.get_or_create(pk=cls.SINGLETON_ID)
        return obj

    # Fan setpoint/scale for the predictive AUTO leg -- must stay numerically
    # identical to the `nh3_error`/`pwm` calculation in hardware/esp32s3_master/
    # src/main.cpp's loop(), or the dashboard's displayed effective_fan_pct
    # drifts from what the master is actually driving to the real fan.
    AUTO_FAN_AMMONIA_SETPOINT_PPM = 5.0
    AUTO_FAN_AMMONIA_SCALE_PPM = 25.0

    @staticmethod
    def _apply_safety_floor(
        fan_pct: int, heater_pct: int, state: str | None, is_low_temperature: bool,
    ) -> tuple[int, int]:
        """Force fan/heater to a safe duty when a live reading is already past
        a critical threshold -- applied on top of EITHER mode's requested
        duty (AUTO's forecast-derived value, or an operator's MANUAL
        setting), never skipped for either. This is what makes MANUAL "not
        absolute" per the class docstring: normal operation honors it
        verbatim, but a live safety threshold always overrides it rather
        than requiring the operator to notice and intervene.

        Fan and heater are not blanket-interlocked here -- both can still
        be forced on at once (e.g. genuinely cold air that still needs
        circulating for air quality is a real condition, not a fault) --
        this only forces specific safe values for specific thresholds:
          - CRITICAL_AMMONIA or HEAT_STRESS_WARNING -> fan forced to 100%
            (ventilate), regardless of forecast or MANUAL fan_speed_pct.
          - HEAT_STRESS_WARNING -> heater forced to 0% (never heat while
            already dangerously hot), regardless of MANUAL heater_power_pct.
          - is_low_temperature (raw check, not the `state` label -- see
            auto_duty_for_state()'s doc comment for why) -> heater forced
            to 100%, regardless of MANUAL heater_power_pct. Can coexist
            with a fan already forced on above (e.g. cold plus a
            simultaneous critical ammonia reading) -- see class docstring.
        """
        if state in (EnvironmentalState.CRITICAL_AMMONIA, EnvironmentalState.HEAT_STRESS_WARNING):
            fan_pct = 100
        if state == EnvironmentalState.HEAT_STRESS_WARNING:
            heater_pct = 0
        if is_low_temperature:
            heater_pct = 100
        return fan_pct, heater_pct

    @classmethod
    def auto_duty_for_state(
        cls, state: str | None, predicted_ammonia: float | None = None,
        is_low_temperature: bool = False,
    ) -> tuple[int, int]:
        """(fan_pct, heater_pct) for AUTO mode. See the class docstring for the policy.

        heater_pct is driven by `is_low_temperature` -- a raw temperature-vs-
        threshold check computed independently of `state` -- rather than by
        `state == LOW_TEMP_ALERT`. classify_environment() returns a single
        mutually-exclusive label with ammonia checked first: a reading that's
        simultaneously CRITICAL_AMMONIA and genuinely cold classifies only as
        CRITICAL_AMMONIA, so `state == LOW_TEMP_ALERT` would silently read
        false and leave the heater off in exactly the case a chick is both
        gassed and freezing. `is_low_temperature` sidesteps that by checking
        the real temperature directly, so the heater reacts to cold whether
        or not ammonia also happens to be critical at the same time. Fan and
        heater are still otherwise fully independent -- this only fixes the
        heater's *input*, not the (lack of) relationship between the two.
        """
        heater_pct = 100 if is_low_temperature else 0

        if predicted_ammonia is not None:
            nh3_error = max(predicted_ammonia - cls.AUTO_FAN_AMMONIA_SETPOINT_PPM, 0.0)
            fan_pct = round(min(nh3_error / cls.AUTO_FAN_AMMONIA_SCALE_PPM, 1.0) * 100)
        else:
            # No forecast yet (Edge-AI link not up) -- pre-ESP32S3-link fallback.
            fan_pct = 0

        # Safety floor: a live reading already past threshold always wins,
        # regardless of what the forecast says. (Redundant with the
        # heater_pct computed above for a real reading -- is_low_temperature
        # and HEAT_STRESS_WARNING can't both be true for the same reading --
        # but shares the one implementation with MANUAL's copy below rather
        # than risk the two drifting apart.)
        return cls._apply_safety_floor(fan_pct, heater_pct, state, is_low_temperature)

    def effective_duty(
        self, latest_state: str | None, predicted_ammonia: float | None = None,
        is_low_temperature: bool = False,
    ) -> tuple[int, int]:
        """Resolve the (fan_pct, heater_pct) that should actually be applied right now."""
        if self.mode == ActuatorMode.MANUAL:
            return self._apply_safety_floor(
                self.fan_speed_pct, self.heater_power_pct, latest_state, is_low_temperature
            )
        return self.auto_duty_for_state(latest_state, predicted_ammonia, is_low_temperature)

    def as_payload(
        self, latest_state: str | None = None, predicted_ammonia: float | None = None,
        is_low_temperature: bool = False,
    ) -> dict:
        effective_fan_pct, effective_heater_pct = self.effective_duty(
            latest_state, predicted_ammonia, is_low_temperature
        )
        return {
            "mode": self.mode,
            "fan_speed_pct": self.fan_speed_pct,
            "heater_power_pct": self.heater_power_pct,
            "effective_fan_pct": effective_fan_pct,
            "effective_heater_pct": effective_heater_pct,
            "updated_at": self.updated_at.isoformat(),
        }
