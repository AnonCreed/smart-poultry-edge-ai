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
        }
