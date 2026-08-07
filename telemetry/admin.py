"""Admin registration: read-oriented list view over the telemetry event stream."""
from django.contrib import admin

from .models import ActuatorControl, FlockProfile, PoultryTelemetry


@admin.register(PoultryTelemetry)
class PoultryTelemetryAdmin(admin.ModelAdmin):
    list_display = ("timestamp", "temperature", "humidity", "ammonia_level", "predicted_class")
    list_filter = ("predicted_class",)
    date_hierarchy = "timestamp"
    ordering = ("-timestamp",)
    readonly_fields = ("timestamp",)


@admin.register(FlockProfile)
class FlockProfileAdmin(admin.ModelAdmin):
    """Singleton settings row -- disable add/delete so only the one row exists."""

    list_display = (
        "age_weeks", "is_configured", "use_custom_thresholds",
        "custom_temp_min_c", "custom_temp_max_c", "custom_ammonia_critical_ppm",
        "updated_at",
    )

    def has_add_permission(self, request):
        return not FlockProfile.objects.exists()

    def has_delete_permission(self, request, obj=None):
        return False


@admin.register(ActuatorControl)
class ActuatorControlAdmin(admin.ModelAdmin):
    """Singleton settings row -- disable add/delete so only the one row exists."""

    list_display = ("mode", "fan_speed_pct", "heater_power_pct", "updated_at")

    def has_add_permission(self, request):
        return not ActuatorControl.objects.exists()

    def has_delete_permission(self, request, obj=None):
        return False
