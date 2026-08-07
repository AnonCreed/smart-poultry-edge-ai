"""
Telemetry app URL map.

/api/telemetry/submit/      POST       hardware ingestion + classification
/api/telemetry/historical/  GET        windowed time-series for chart clients
/api/telemetry/profile/     GET/POST   flock age & custom threshold profile
/api/telemetry/control/     GET/POST   fan/heater actuator mode + manual duty
/api/telemetry/export/      GET        CSV export over an hours or start/end window
/api/telemetry/report/      GET        aggregate summary over the same window
/                           GET        analytical dashboard shell
"""
from django.urls import path

from . import views

urlpatterns = [
    path("api/telemetry/submit/", views.submit_telemetry, name="telemetry-submit"),
    path("api/telemetry/historical/", views.historical_telemetry, name="telemetry-historical"),
    path("api/telemetry/profile/", views.flock_profile, name="telemetry-profile"),
    path("api/telemetry/control/", views.actuator_control, name="telemetry-control"),
    path("api/telemetry/export/", views.export_telemetry, name="telemetry-export"),
    path("api/telemetry/report/", views.report_summary, name="telemetry-report"),
    path("", views.dashboard, name="telemetry-dashboard"),
]
