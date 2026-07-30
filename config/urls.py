"""
Root URL configuration.

Routing strategy: all machine-facing endpoints live under /api/ and all
human-facing views are mounted at the root. The telemetry app owns both
namespaces via a single include to keep the app fully self-contained.
"""
from django.contrib import admin
from django.urls import include, path

urlpatterns = [
    path("admin/", admin.site.urls),
    path("", include("telemetry.urls")),
]
