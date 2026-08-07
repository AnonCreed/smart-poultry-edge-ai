import os
import sys

from django.apps import AppConfig


class TelemetryConfig(AppConfig):
    default_auto_field = "django.db.models.BigAutoField"
    name = "telemetry"

    def ready(self):
        # Only start the UDP discovery responder for an actual `runserver`
        # process -- not for migrate/test/shell/etc, which also import the
        # app registry and would otherwise each try to bind the same port.
        if "runserver" not in sys.argv:
            return

        # With the StatReloader (the default), Django re-execs itself: the
        # original process just watches files and never serves, while a
        # child with RUN_MAIN=true does the actual serving -- but *both*
        # populate the app registry and call ready(). Only start in the
        # child (or in the one-and-only process when --noreload is passed,
        # where RUN_MAIN is never set at all) so we don't bind the
        # discovery port twice.
        run_main = os.environ.get("RUN_MAIN")
        if run_main != "true" and not (run_main is None and "--noreload" in sys.argv):
            return

        from . import discovery

        discovery.start()
