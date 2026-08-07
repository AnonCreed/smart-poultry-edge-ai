"""
Django settings for the Poultry Telemetry Control System.

Architecture notes:
- SQLite is used as the default persistence layer for portability; the schema
  and ORM usage are engine-agnostic and can be pointed at PostgreSQL by
  swapping the DATABASES block (recommended for production time-series load).
- The `telemetry` app owns the data model, classifier, API layer, and the
  server-rendered dashboard shell. All chart data is delivered via JSON APIs
  so the front end can be replaced by any SPA framework without backend changes.
"""

from pathlib import Path

BASE_DIR = Path(__file__).resolve().parent.parent

# SECURITY: replace with an environment-sourced secret in deployment.
SECRET_KEY = "dev-only-insecure-key-rotate-before-deployment"

DEBUG = True

# Dev-only project (see SECRET_KEY above) served on a local/farm LAN whose
# address changes with whatever WiFi network it's on -- rather than hardcode
# an IP that breaks every time the network changes, accept any Host header
# while DEBUG is on. Tighten this to explicit hostnames before any real
# deployment.
ALLOWED_HOSTS = ["*"] if DEBUG else []

INSTALLED_APPS = [
    "django.contrib.admin",
    "django.contrib.auth",
    "django.contrib.contenttypes",
    "django.contrib.sessions",
    "django.contrib.messages",
    "django.contrib.staticfiles",
    "telemetry",
]

MIDDLEWARE = [
    "django.middleware.security.SecurityMiddleware",
    "django.contrib.sessions.middleware.SessionMiddleware",
    "django.middleware.common.CommonMiddleware",
    "django.middleware.csrf.CsrfViewMiddleware",
    "django.contrib.auth.middleware.AuthenticationMiddleware",
    "django.contrib.messages.middleware.MessageMiddleware",
    "django.middleware.clickjacking.XFrameOptionsMiddleware",
]

ROOT_URLCONF = "config.urls"

TEMPLATES = [
    {
        "BACKEND": "django.template.backends.django.DjangoTemplates",
        "DIRS": [],
        "APP_DIRS": True,
        "OPTIONS": {
            "context_processors": [
                "django.template.context_processors.debug",
                "django.template.context_processors.request",
                "django.contrib.auth.context_processors.auth",
                "django.contrib.messages.context_processors.messages",
            ],
        },
    },
]

WSGI_APPLICATION = "config.wsgi.application"

DATABASES = {
    "default": {
        "ENGINE": "django.db.backends.sqlite3",
        "NAME": BASE_DIR / "db.sqlite3",
    }
}

AUTH_PASSWORD_VALIDATORS = []

LANGUAGE_CODE = "en-us"
TIME_ZONE = "UTC"
USE_I18N = True
# USE_TZ=True guarantees timezone-aware datetimes in the telemetry index,
# which is mandatory for correct time-window queries across DST boundaries.
USE_TZ = True

# Logging: the ingestion view emits one structured line per accepted POST
# that mirrors the browser's live console feed format, so operators can
# cross-reference server logs and the UI without translation.
LOGGING = {
    "version": 1,
    "disable_existing_loggers": False,
    "formatters": {
        "ingest": {"format": "%(asctime)s [%(levelname)s] telemetry.ingest %(message)s"},
    },
    "handlers": {
        "console": {"class": "logging.StreamHandler", "formatter": "ingest"},
    },
    "loggers": {
        "telemetry.ingest": {"handlers": ["console"], "level": "INFO", "propagate": False},
    },
}

STATIC_URL = "static/"

DEFAULT_AUTO_FIELD = "django.db.models.BigAutoField"
