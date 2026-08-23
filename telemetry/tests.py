"""
Unit coverage for the classifier decision framework and API contracts.
Run with: python manage.py test telemetry
"""
import json

from django.test import TestCase
from django.urls import reverse

from .classifier import classify_environment
from .models import ActuatorControl, EnvironmentalState, PoultryTelemetry


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

    def test_spike_probability_defaults_to_null_when_absent(self):
        # Master node (ESP32-S3) not yet linked: key omitted entirely.
        response = self._post({"temperature": 27.0, "humidity": 60.0, "ammonia_level": 5.0})
        self.assertEqual(response.status_code, 201)
        self.assertIsNone(PoultryTelemetry.objects.get().predicted_spike_probability)
        self.assertIsNone(response.json()["record"]["predicted_spike_probability"])

    def test_spike_probability_persists_when_provided(self):
        response = self._post({
            "temperature": 27.0, "humidity": 60.0, "ammonia_level": 5.0,
            "predicted_spike_probability": 0.42,
        })
        self.assertEqual(response.status_code, 201)
        self.assertAlmostEqual(
            PoultryTelemetry.objects.get().predicted_spike_probability, 0.42
        )

    def test_spike_probability_out_of_unit_interval_rejected(self):
        response = self._post({
            "temperature": 27.0, "humidity": 60.0, "ammonia_level": 5.0,
            "predicted_spike_probability": 1.5,
        })
        self.assertEqual(response.status_code, 400)
        self.assertEqual(PoultryTelemetry.objects.count(), 0)

    def test_non_numeric_spike_probability_rejected(self):
        response = self._post({
            "temperature": 27.0, "humidity": 60.0, "ammonia_level": 5.0,
            "predicted_spike_probability": "high",
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
        self.assertIn("ammonia_spike_risk_threshold", body["thresholds"])

    def test_invalid_hours_param_rejected(self):
        response = self.client.get(reverse("telemetry-historical"), {"hours": "yesterday"})
        self.assertEqual(response.status_code, 400)


class ActuatorControlEndpointTests(TestCase):
    def test_defaults_to_auto_with_zero_manual_duty(self):
        response = self.client.get(reverse("telemetry-control"))
        body = response.json()
        self.assertEqual(response.status_code, 200)
        self.assertEqual(body["control"]["mode"], "AUTO")
        self.assertEqual(body["control"]["effective_fan_pct"], 0)
        self.assertEqual(body["control"]["effective_heater_pct"], 0)

    def test_auto_effective_duty_derives_from_latest_classification(self):
        PoultryTelemetry.objects.create(
            temperature=30.0, humidity=5.0, ammonia_level=30.0,
            predicted_class=EnvironmentalState.CRITICAL_AMMONIA,
        )
        response = self.client.get(reverse("telemetry-control"))
        control = response.json()["control"]
        self.assertEqual(control["effective_fan_pct"], 100)
        self.assertEqual(control["effective_heater_pct"], 0)

    def test_auto_fan_is_predictive_when_forecast_available(self):
        # OPTIMAL_ENVIRONMENT live reading, but the Edge-AI forecast sees
        # ammonia building toward the 5 ppm setpoint -- fan should scale
        # proportionally (15 ppm error / 25 ppm scale -> 60%), mirroring
        # hardware/esp32s3_master/src/main.cpp's fanPwm calculation, not
        # sit at 0% just because the live classification is still OK.
        PoultryTelemetry.objects.create(
            temperature=25.0, humidity=50.0, ammonia_level=3.0,
            predicted_class=EnvironmentalState.OPTIMAL_ENVIRONMENT,
            predicted_ammonia=20.0,
        )
        response = self.client.get(reverse("telemetry-control"))
        control = response.json()["control"]
        self.assertEqual(control["effective_fan_pct"], 60)
        self.assertEqual(control["effective_heater_pct"], 0)

    def test_auto_fan_safety_floor_ignores_low_forecast_during_critical_reading(self):
        # A live CRITICAL_AMMONIA reading must force the fan to 100% even if
        # the forecast alone would call for less -- the safety floor always
        # wins over the predictive leg.
        PoultryTelemetry.objects.create(
            temperature=25.0, humidity=50.0, ammonia_level=40.0,
            predicted_class=EnvironmentalState.CRITICAL_AMMONIA,
            predicted_ammonia=6.0,
        )
        response = self.client.get(reverse("telemetry-control"))
        control = response.json()["control"]
        self.assertEqual(control["effective_fan_pct"], 100)

    def test_auto_heater_reacts_to_low_temperature(self):
        # Default (unconfigured) profile's low_temp_c is 18.0 -- 10.0 is
        # comfortably below it.
        PoultryTelemetry.objects.create(
            temperature=10.0, humidity=50.0, ammonia_level=3.0,
            predicted_class=EnvironmentalState.LOW_TEMP_ALERT,
        )
        response = self.client.get(reverse("telemetry-control"))
        control = response.json()["control"]
        self.assertEqual(control["effective_heater_pct"], 100)

    def test_auto_heater_not_masked_by_stored_critical_ammonia_state(self):
        # The record's stored predicted_class is CRITICAL_AMMONIA (ammonia
        # dominates classify_environment()'s priority order), but the
        # temperature on the very same record is also below threshold --
        # the heater must still react to that, not just the fan. This is
        # the live-endpoint counterpart to
        # SubmitEndpointTests.test_submit_low_temperature_alert_not_masked_by_critical_ammonia.
        PoultryTelemetry.objects.create(
            temperature=10.0, humidity=50.0, ammonia_level=40.0,
            predicted_class=EnvironmentalState.CRITICAL_AMMONIA,
        )
        response = self.client.get(reverse("telemetry-control"))
        control = response.json()["control"]
        self.assertEqual(control["effective_fan_pct"], 100)
        self.assertEqual(control["effective_heater_pct"], 100)

    def test_manual_mode_overrides_with_saved_percentages(self):
        response = self.client.post(
            reverse("telemetry-control"),
            data=json.dumps({"mode": "MANUAL", "fan_speed_pct": 40, "heater_power_pct": 100}),
            content_type="application/json",
        )
        body = response.json()
        self.assertEqual(response.status_code, 200)
        self.assertEqual(body["control"]["effective_fan_pct"], 40)
        self.assertEqual(body["control"]["effective_heater_pct"], 100)
        self.assertEqual(ActuatorControl.objects.get().mode, "MANUAL")

    def test_invalid_mode_rejected(self):
        response = self.client.post(
            reverse("telemetry-control"),
            data=json.dumps({"mode": "TURBO"}),
            content_type="application/json",
        )
        self.assertEqual(response.status_code, 400)

    def test_out_of_range_percentage_rejected(self):
        response = self.client.post(
            reverse("telemetry-control"),
            data=json.dumps({"fan_speed_pct": 150}),
            content_type="application/json",
        )
        self.assertEqual(response.status_code, 400)

    def test_non_binary_heater_percentage_rejected(self):
        # The heater is relay-switched (on/off only) -- anything other than
        # 0 or 100 doesn't correspond to a real hardware state.
        response = self.client.post(
            reverse("telemetry-control"),
            data=json.dumps({"heater_power_pct": 65}),
            content_type="application/json",
        )
        self.assertEqual(response.status_code, 400)

    def test_submit_response_echoes_effective_control(self):
        ActuatorControl.objects.create(mode="MANUAL", fan_speed_pct=77, heater_power_pct=100)
        response = self.client.post(
            reverse("telemetry-submit"),
            data=json.dumps({"temperature": 27.0, "humidity": 55.0, "ammonia_level": 4.0}),
            content_type="application/json",
        )
        body = response.json()
        self.assertEqual(response.status_code, 201)
        self.assertEqual(body["control"]["effective_fan_pct"], 77)
        self.assertEqual(body["control"]["effective_heater_pct"], 100)

    def test_submit_low_temperature_alert_true_when_genuinely_cold(self):
        # Plain LOW_TEMP_ALERT case (nothing else in play) -- heater should
        # be on in AUTO and low_temperature_alert should be echoed True.
        response = self.client.post(
            reverse("telemetry-submit"),
            data=json.dumps({"temperature": 10.0, "humidity": 50.0, "ammonia_level": 3.0}),
            content_type="application/json",
        )
        body = response.json()
        self.assertEqual(response.status_code, 201)
        self.assertEqual(body["record"]["predicted_class"], EnvironmentalState.LOW_TEMP_ALERT)
        self.assertTrue(body["low_temperature_alert"])
        self.assertEqual(body["control"]["effective_heater_pct"], 100)

    def test_submit_low_temperature_alert_not_masked_by_critical_ammonia(self):
        # The bug this pair of fields exists to fix: classify_environment()
        # checks ammonia first, so a reading that's simultaneously critical
        # on ammonia AND genuinely cold classifies only as
        # CRITICAL_AMMONIA -- predicted_class == LOW_TEMP_ALERT would be
        # False here even though it's cold. low_temperature_alert must stay
        # True regardless, and the heater must still turn on, not just the
        # fan.
        response = self.client.post(
            reverse("telemetry-submit"),
            data=json.dumps({"temperature": 10.0, "humidity": 50.0, "ammonia_level": 40.0}),
            content_type="application/json",
        )
        body = response.json()
        self.assertEqual(response.status_code, 201)
        self.assertEqual(body["record"]["predicted_class"], EnvironmentalState.CRITICAL_AMMONIA)
        self.assertTrue(body["low_temperature_alert"])
        self.assertEqual(body["control"]["effective_fan_pct"], 100)
        self.assertEqual(body["control"]["effective_heater_pct"], 100)


class ExportAndReportEndpointTests(TestCase):
    def setUp(self):
        PoultryTelemetry.objects.create(
            temperature=20.0, humidity=50.0, ammonia_level=5.0,
            predicted_class=EnvironmentalState.OPTIMAL_ENVIRONMENT,
        )
        PoultryTelemetry.objects.create(
            temperature=40.0, humidity=80.0, ammonia_level=30.0,
            predicted_class=EnvironmentalState.CRITICAL_AMMONIA,
        )

    def test_export_returns_csv_with_header_and_rows(self):
        response = self.client.get(reverse("telemetry-export"))
        self.assertEqual(response.status_code, 200)
        self.assertEqual(response["Content-Type"], "text/csv")
        body = response.content.decode("utf-8")
        lines = body.strip().splitlines()
        self.assertEqual(lines[0].split(","), [
            "timestamp", "temperature_c", "humidity_pct", "ammonia_ppm",
            "predicted_temperature_c", "predicted_ammonia_ppm",
            "predicted_spike_probability", "predicted_class",
        ])
        self.assertEqual(len(lines), 3)  # header + 2 records

    def test_export_invalid_hours_rejected(self):
        response = self.client.get(reverse("telemetry-export"), {"hours": "nope"})
        self.assertEqual(response.status_code, 400)

    def test_export_invalid_date_rejected(self):
        response = self.client.get(reverse("telemetry-export"), {"start": "not-a-date"})
        self.assertEqual(response.status_code, 400)

    def test_report_summary_counts_and_averages(self):
        response = self.client.get(reverse("telemetry-report"))
        body = response.json()
        self.assertEqual(response.status_code, 200)
        self.assertEqual(body["count"], 2)
        self.assertEqual(body["averages"]["avg_temperature"], 30.0)
        self.assertEqual(body["state_counts"]["CRITICAL_AMMONIA"], 1)
        self.assertEqual(body["state_counts"]["OPTIMAL_ENVIRONMENT"], 1)
        self.assertEqual(body["state_counts"]["LOW_TEMP_ALERT"], 0)

    def test_report_start_after_end_rejected(self):
        response = self.client.get(
            reverse("telemetry-report"), {"start": "2026-08-05", "end": "2026-08-01"}
        )
        self.assertEqual(response.status_code, 400)
