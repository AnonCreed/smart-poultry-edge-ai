"""
Telemetry app URL map.

/api/telemetry/submit/      POST  hardware ingestion + classification
/api/telemetry/historical/  GET   windowed time-series for chart clients
/                           GET   analytical dashboard shell
"""
from django.urls import path

from . import views

urlpatterns = [
    path("api/telemetry/submit/", views.submit_telemetry, name="telemetry-submit"),
    path("api/telemetry/historical/", views.historical_telemetry, name="telemetry-historical"),
    path("", views.dashboard, name="telemetry-dashboard"),
]
