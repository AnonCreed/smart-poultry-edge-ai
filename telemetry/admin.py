"""Admin registration: read-oriented list view over the telemetry event stream."""
from django.contrib import admin

from .models import PoultryTelemetry


@admin.register(PoultryTelemetry)
class PoultryTelemetryAdmin(admin.ModelAdmin):
    list_display = ("timestamp", "temperature", "humidity", "ammonia_level", "predicted_class")
    list_filter = ("predicted_class",)
    date_hierarchy = "timestamp"
    ordering = ("-timestamp",)
    readonly_fields = ("timestamp",)
