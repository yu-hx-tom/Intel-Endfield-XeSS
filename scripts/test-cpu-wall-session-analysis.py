#!/usr/bin/env python3
"""Small offline counter-difference and lifecycle-boundary fixtures."""
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.dont_write_bytecode = True
SPEC = importlib.util.spec_from_file_location("cpu_wall_analysis", Path(__file__).with_name("analyze-cpu-wall-session.py"))
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


def row(step=0, **changes):
    value = {"event": "continuous_present", "pid": 7, "device": "0xD", "uptime_ms": 1000 + 5000 * step,
             "cpu_wall_owner_generation": 1, "output_width": 1920, "output_height": 1080,
             "present_mode": 2, "cpu_wall_control_epoch": 3, "present_calls": 300 + 300 * step,
             "cpu_wall_cumulative": True, "cpu_wall_max_scope": "owner_lifetime",
             "sdk_output_succeeded": True, "sdk_status_valid": True, "sdk_frames_presented": 1, "output_occluded": False,
             "complete_frame_timing": True, "input_matched": True,
             "sdk_enabled": False, "requested_enabled": False, "menu_requested_enabled": False}
    for name in MODULE.CORE + MODULE.OPTIONAL:
        value[name] = {"count": 10 + 10 * step, "total_ns": 1000 + 2000 * step, "max_ns": 500}
    value["cpu_tracking_recording"] = {"count": 0, "total_ns": 0, "max_ns": 0}
    value["cpu_tracking_empty_scope"] = {"count": 64, "total_ns": 6400, "max_ns": 200}
    value.update(changes)
    return value


def analyze(*records):
    analyzer = MODULE.Analyzer()
    for record in records:
        analyzer.accept(record)
    return analyzer.finish()


def timed_off_row(step=0, **changes):
    return row(step, **{"input_matched": False, "aux_capture_policy": "skip_observed_off",
                       "menu_input_observed": True, "skipped_off_captures": 100 + 10 * step, **changes})


class CpuWallFixtures(unittest.TestCase):
    def test_stable_delta_and_calibration_not_subtracted(self):
        result = analyze(row(), row(1))
        interval = result["intervals"][0]
        measured = interval["stages"]["cpu_runtime_copy"]
        self.assertEqual((measured["delta_count"], measured["delta_total_ns"], measured["mean_wall_ns_per_call"]), (10, 2000, 200))
        self.assertEqual(measured["max_ns_at_end_lifetime"], 500)
        self.assertEqual(interval["ordinary_sdk_present_cadence_hz"], 60)
        self.assertEqual(interval["empty_scope_calibration_at_end"]["total_ns"], 6400)
        no_calls = interval["stages"]["cpu_tracking_recording"]
        self.assertEqual(no_calls["mean_status"], "no_calls")
        self.assertIsNone(no_calls["mean_wall_ns_per_call"])
        self.assertEqual(interval["stages"]["cpu_xell_sleep"]["kind"], "cross_thread_xell")
        self.assertEqual(interval["stages"]["cpu_bridge_publish"]["kind"], "nested_output_detail")
        self.assertEqual(result["coverage"]["accepted_device_time_fraction"], 1)

    def test_tracking_disabled_preserves_runtime_without_numeric_tracking(self):
        before, after = row(cpu_tracking_timing_enabled=False), row(1, cpu_tracking_timing_enabled=False)
        # Disabled counters can be zero, absent, or stale; none is a measurement.
        before["cpu_tracking_descriptor_update"] = {"count": 0, "total_ns": 0, "max_ns": 0}
        after.pop("cpu_tracking_descriptor_update")
        after["cpu_tracking_bind"] = None
        interval = analyze(before, after)["intervals"][0]
        self.assertIs(interval["cpu_tracking_timing_enabled"], False)
        self.assertFalse(any(name.startswith("cpu_tracking_") for name in interval["stages"]))
        self.assertIsNone(interval["empty_scope_calibration_at_end"])
        self.assertEqual(interval["stages"]["cpu_runtime_copy"]["delta_total_ns"], 2000)
        self.assertIn("cpu_upscale_observe", interval["stages"])

    def test_tracking_enabled_state_changes_break_a_b_a(self):
        for enabled in (True, False):
            result = analyze(row(cpu_tracking_timing_enabled=enabled),
                             row(1, cpu_tracking_timing_enabled=not enabled),
                             row(2, cpu_tracking_timing_enabled=enabled))
            self.assertEqual(result["intervals"], [])
            self.assertEqual(result["skipped"]["tracking_timing_state_changed"], 2)
        self.assertEqual(analyze(row(), row(1, cpu_tracking_timing_enabled=False))["intervals"], [])

    def test_tracking_missing_flag_is_legacy_enabled(self):
        for before, after in ((row(), row(1)), (row(), row(1, cpu_tracking_timing_enabled=True))):
            interval = analyze(before, after)["intervals"][0]
            self.assertIs(interval["cpu_tracking_timing_enabled"], True)
            self.assertEqual(interval["stages"]["cpu_tracking_bind"]["delta_total_ns"], 2000)
            self.assertIsNotNone(interval["empty_scope_calibration_at_end"])

    def test_tracking_flag_requires_boolean(self):
        for invalid in (0, 1, "false", "true", None):
            result = analyze(row(), row(1, cpu_tracking_timing_enabled=invalid), row(2))
            self.assertEqual(result["intervals"], [])
            self.assertEqual(result["skipped"]["invalid_tracking_timing_state"], 1)

    def test_unknown_old_log_never_upgraded(self):
        old = row()
        del old["device"]
        del old["cpu_wall_owner_generation"]
        result = analyze(old, {**old, "uptime_ms": 9000})
        self.assertEqual(result["status"], "insufficient_evidence")
        self.assertEqual(result["skipped"]["missing_or_invalid_identity"], 2)

    def test_startup_on_occluded_and_unmatched_break_continuity(self):
        for field, value in (("complete_frame_timing", False), ("input_matched", False),
                             ("sdk_output_succeeded", False), ("output_occluded", True),
                             ("menu_requested_enabled", True), ("sdk_enabled", True)):
            with self.subTest(field=field):
                result = analyze(row(), row(1, **{field: value}), row(2))
                self.assertEqual(result["intervals"], [])
                self.assertEqual(result["coverage"]["eligible_snapshots"], 2)

    def test_identity_changes_do_not_bridge_a_b_a(self):
        for field, value in (("output_width", 1280), ("present_mode", 0),
                             ("cpu_wall_control_epoch", 4), ("cpu_wall_owner_generation", 2)):
            with self.subTest(field=field):
                self.assertEqual(analyze(row(), row(1, **{field: value}), row(2))["intervals"], [])
        self.assertEqual(analyze(row(), row(1, pid=8), row(2, device="0xe"))["intervals"], [])

    def test_same_pid_swapchain_barrier_even_for_other_device(self):
        for event in ("create_swapchain", "presentation_off_create_swapchain"):
            result = analyze(row(), {"event": event, "pid": 7, "device": "0xe"}, row(1), row(2))
            self.assertEqual(len(result["intervals"]), 1)
            self.assertEqual(result["intervals"][0]["start_uptime_ms"], 6000)
            self.assertEqual(result["coverage"]["accepted_device_time_fraction"], 0.5)

    def test_sibling_without_metrics_ignored_unknown_mode_rejected(self):
        sibling = {"event": "presentation_continuous_present", "pid": 7, "result": 0}
        self.assertEqual(len(analyze(row(), sibling, row(1))["intervals"]), 1)
        self.assertEqual(analyze(row(), row(1, present_mode=0xffffffff))["intervals"], [])
        self.assertEqual(analyze(row(), {**sibling, "result": -1}, row(1))["intervals"], [])
        for count in (0, 2, True, None):
            self.assertEqual(analyze(row(), row(1, sdk_frames_presented=count))["intervals"], [])

    def test_counter_reset_and_inconsistent_zero_calls_rejected(self):
        for replacement in ({"count": 1, "total_ns": 100, "max_ns": 50},
                            {"count": 10, "total_ns": 1001, "max_ns": 500},
                            {"count": 10, "total_ns": 1000, "max_ns": 501}):
            later = row(1)
            later["cpu_output"] = replacement
            self.assertEqual(analyze(row(), later)["intervals"], [])
        self.assertEqual(analyze(row(), row(1, uptime_ms=1000))["intervals"], [])

    def test_optional_schema_boundary_then_no_calls_valid(self):
        first = row()
        del first["cpu_upscale_observe"]
        result = analyze(first, row(1), row(2))
        self.assertEqual(len(result["intervals"]), 1)
        self.assertIn("cpu_upscale_observe", result["intervals"][0]["stages"])

    def test_provider_absent_fixture_has_separate_scope(self):
        values = [row(i, event="presentation_off_present", result=0) for i in (0, 1)]
        for value in values:
            for field in ("complete_frame_timing", "input_matched", "requested_enabled", "menu_requested_enabled", "sdk_enabled"):
                del value[field]
            value["xefg_off_frames"] = value.pop("present_calls")
        interval = analyze(*values)["intervals"][0]
        self.assertEqual(interval["scope"], "provider_absent_off_not_game_input_validation")

    def test_explicit_timed_off_is_not_aux_input_validation(self):
        interval = analyze(timed_off_row(), timed_off_row(1))["intervals"][0]
        self.assertEqual(interval["scope"], "sampled_timed_off_output_aux_not_validated")
        self.assertEqual(interval["off_aux_capture_observation"]["skipped_off_captures_delta"], 10)
        self.assertFalse(interval["off_aux_capture_observation"]["current_packet_skip_verified"])
        self.assertEqual(interval["ordinary_sdk_present_cadence_hz"], 60)
        no_skip = analyze(timed_off_row(), timed_off_row(1, skipped_off_captures=100))["intervals"][0]
        self.assertEqual(no_skip["off_aux_capture_observation"]["skipped_off_captures_delta"], 0)
        unknown = analyze(timed_off_row(), timed_off_row(1, skipped_off_captures=None))["intervals"][0]
        self.assertIsNone(unknown["off_aux_capture_observation"]["skipped_off_captures_delta"])

    def test_timed_off_requires_current_off_state_not_cumulative_skip(self):
        for field, value in (("aux_capture_policy", None), ("aux_capture_policy", "unknown"),
                             ("menu_input_observed", False), ("menu_input_observed", None),
                             ("input_matched", None), ("menu_requested_enabled", True),
                             ("requested_enabled", True), ("sdk_enabled", True),
                             ("complete_frame_timing", False), ("sdk_status_valid", False),
                             ("sdk_output_succeeded", False), ("sdk_frames_presented", 2), ("output_occluded", True)):
            with self.subTest(field=field, value=value):
                result = analyze(timed_off_row(), timed_off_row(1, **{field: value}), timed_off_row(2))
                self.assertEqual(result["intervals"], [])
        self.assertEqual(analyze(row(input_matched=False, skipped_off_captures=100),
                                 row(1, input_matched=False, skipped_off_captures=200))["intervals"], [])

    def test_full_input_scope_has_priority_and_scope_transitions_break(self):
        full = [timed_off_row(i, input_matched=True, menu_input_observed=False) for i in (0, 1)]
        self.assertEqual(analyze(*full)["intervals"][0]["scope"], "sampled_game_input_off")
        self.assertEqual(analyze(row(), timed_off_row(1), row(2))["intervals"], [])
        self.assertEqual(analyze(row(), timed_off_row(1, input_matched=True))["intervals"], [])

    def test_stream_bad_line_breaks_baseline_and_bounds(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "observer.jsonl"
            path.write_text(json.dumps(row()) + "\n{broken\n" + json.dumps(row(1)) + "\n" + json.dumps(row(2)) + "\n", encoding="utf-8")
            analyzer = MODULE.Analyzer()
            analyzer.read(path)
            result = analyzer.finish()
            self.assertEqual(result["coverage"]["invalid_lines"], 1)
            self.assertEqual(len(result["intervals"]), 1)
            path.write_bytes(b"x" * (MODULE.MAX_LINE_BYTES + 1))
            with self.assertRaises(ValueError):
                MODULE.Analyzer().read(path)


if __name__ == "__main__":
    unittest.main()
