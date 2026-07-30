"""
Unit coverage for the classifier decision framework and API contracts.
Run with: python manage.py test telemetry
"""
import json

from django.test import TestCase
from django.urls import reverse

from .classifier import classify_environment
from .models import EnvironmentalState, PoultryTelemetry


class ClassifierRuleTests(TestCase):
    def test_ammonia_dominates_all_other_conditions(self):
        # Even under heat-stress conditions, NH3 breach takes precedence.
        self.assertEqual(
            classify_environment(temperature=38.0, humidity=80.0, ammonia_level=26.0),
            EnvironmentalState.CRITICAL_AMMONIA,
        )

    def test_heat_stress_requires_conjunction(self):
        self.assertEqual(
            classify_environment(temperature=36.0, humidity=71.0, ammonia_level=10.0),
            EnvironmentalState.HEAT_STRESS_WARNING,
        )
        # High temperature alone with low humidity does not trigger the warning.
        self.assertEqual(
            classify_environment(temperature=36.0, humidity=50.0, ammonia_level=10.0),
            EnvironmentalState.OPTIMAL_ENVIRONMENT,
        )

    def test_low_temperature_alert(self):
        self.assertEqual(
            classify_environment(temperature=17.9, humidity=55.0, ammonia_level=5.0),
            EnvironmentalState.LOW_TEMP_ALERT,
        )

    def test_optimal_band(self):
        self.assertEqual(
            classify_environment(temperature=27.0, humidity=60.0, ammonia_level=8.0),
            EnvironmentalState.OPTIMAL_ENVIRONMENT,
        )


class SubmitEndpointTests(TestCase):
    def _post(self, payload):
        return self.client.post(
            reverse("telemetry-submit"),
            data=json.dumps(payload),
            content_type="application/json",
        )

    def test_valid_payload_persists_with_classification(self):
        response = self._post({"temperature": 30.0, "humidity": 60.0, "ammonia_level": 30.0})
        self.assertEqual(response.status_code, 201)
        record = PoultryTelemetry.objects.get()
        self.assertEqual(record.predicted_class, EnvironmentalState.CRITICAL_AMMONIA)

    def test_missing_field_rejected(self):
        response = self._post({"temperature": 30.0, "humidity": 60.0})
        self.assertEqual(response.status_code, 400)
        self.assertEqual(PoultryTelemetry.objects.count(), 0)

    def test_non_numeric_field_rejected(self):
        response = self._post({"temperature": "hot", "humidity": 60.0, "ammonia_level": 5.0})
        self.assertEqual(response.status_code, 400)

    def test_out_of_bounds_reading_rejected(self):
        response = self._post({"temperature": 500.0, "humidity": 60.0, "ammonia_level": 5.0})
        self.assertEqual(response.status_code, 400)

    def test_forecast_fields_default_to_null_when_absent(self):
        # Hardware-not-linked case: payload omits forecast keys entirely.
        response = self._post({"temperature": 27.0, "humidity": 60.0, "ammonia_level": 5.0})
        self.assertEqual(response.status_code, 201)
        record = PoultryTelemetry.objects.get()
        self.assertIsNone(record.predicted_temperature)
        self.assertIsNone(record.predicted_ammonia)
        body = response.json()["record"]
        self.assertIsNone(body["predicted_temperature"])
        self.assertIsNone(body["predicted_ammonia"])

    def test_forecast_fields_accept_explicit_null(self):
        response = self._post({
            "temperature": 27.0, "humidity": 60.0, "ammonia_level": 5.0,
            "predicted_temperature": None, "predicted_ammonia": None,
        })
        self.assertEqual(response.status_code, 201)
        self.assertIsNone(PoultryTelemetry.objects.get().predicted_temperature)

    def test_forecast_fields_persist_when_provided(self):
        response = self._post({
            "temperature": 27.0, "humidity": 60.0, "ammonia_level": 5.0,
            "predicted_temperature": 28.4, "predicted_ammonia": 6.2,
        })
        self.assertEqual(response.status_code, 201)
        record = PoultryTelemetry.objects.get()
        self.assertAlmostEqual(record.predicted_temperature, 28.4)
        self.assertAlmostEqual(record.predicted_ammonia, 6.2)
        # Classification must remain driven by live readings only.
        self.assertEqual(record.predicted_class, EnvironmentalState.OPTIMAL_ENVIRONMENT)

    def test_non_numeric_forecast_rejected(self):
        response = self._post({
            "temperature": 27.0, "humidity": 60.0, "ammonia_level": 5.0,
            "predicted_temperature": "warm",
        })
        self.assertEqual(response.status_code, 400)
        self.assertEqual(PoultryTelemetry.objects.count(), 0)


class HistoricalEndpointTests(TestCase):
    def test_returns_ascending_chronology_and_thresholds(self):
        for temp in (20.0, 21.0, 22.0):
            PoultryTelemetry.objects.create(
                temperature=temp, humidity=50.0, ammonia_level=5.0,
                predicted_class=EnvironmentalState.OPTIMAL_ENVIRONMENT,
            )
        response = self.client.get(reverse("telemetry-historical"))
        body = response.json()
        self.assertEqual(response.status_code, 200)
        self.assertEqual(body["count"], 3)
        stamps = [point["timestamp"] for point in body["data"]]
        self.assertEqual(stamps, sorted(stamps))
        self.assertIn("ammonia_critical_ppm", body["thresholds"])

    def test_invalid_hours_param_rejected(self):
        response = self.client.get(reverse("telemetry-historical"), {"hours": "yesterday"})
        self.assertEqual(response.status_code, 400)
