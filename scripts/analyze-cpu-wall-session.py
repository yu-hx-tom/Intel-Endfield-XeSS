#!/usr/bin/env python3
"""Difference explicit observer CPU-wall snapshots; never open a process or capture."""
from __future__ import annotations

import argparse
from collections import Counter
import json
from pathlib import Path
import re
import sys

EVENTS = {"continuous_present", "presentation_off_present"}
BARRIERS = {"create_swapchain", "presentation_off_create_swapchain", "destroy_swapchain",
            "create_device", "destroy_device", "continuous_present_timing_off_fallback"}
CORE = ("cpu_runtime_copy", "cpu_native_present", "cpu_output", "cpu_bridge_publish",
        "cpu_previous_d3d_wait", "cpu_dxgi_present", "cpu_xell_sleep")
OPTIONAL = ("cpu_tracking_descriptor_update", "cpu_tracking_descriptor_lifetime",
            "cpu_tracking_bind", "cpu_tracking_dispatch", "cpu_tracking_recording",
            "cpu_upscale_observe")
MAX_BYTES, MAX_LINE_BYTES, MAX_LINES = 64 << 20, 128 << 10, 250000
LIMITATIONS = [
    "Intervals use adjacent eligible snapshots with stable PID/device/owner/extent/control epoch/present mode; observed rebuilds and invalid snapshots break continuity. Unsampled changes remain unknown.",
    "Cadence counts successful ordinary SDK present calls, not physical displayed FPS, GPU time, generated FPS or an engine frame ID.",
    "Runtime copy/native-present/output may be compared individually. Output details are nested; XeLL is cross-thread; tracking and upscale observation may overlap runtime. Never add these stages.",
    "Tracking covers selected wrappers' own wall time across threads, excluding driver calls and counter-lock wait; it is not total observer overhead. Wall time per completed present is amortized work, not critical-path latency or proof of a frame-rate deficit.",
    "Runtime copy and bridge/previous-D3D waits can block on earlier game rendering and synchronization. For example, 15 ms wall time is not 15 ms of added copy GPU cost; these counters locate blocking, not incremental overhead or a 20 FPS cause.",
    "max_ns is an owner/process lifetime maximum, never a difference or interval maximum. No p95 can be derived from these counters.",
    "Empty-scope calibration is reported unchanged and is never subtracted. Probe/metadata counters can have broader ownership than the presentation owner.",
    "cpu_tracking_timing_enabled=false excludes tracking stages and empty-scope calibration: zero counters mean disabled measurement, not zero overhead. Missing flags retain legacy enabled interpretation; enabled-state changes break intervals.",
    "Endpoint input/timing checks are sampled evidence, not proof that every intervening frame was valid. A provider-absent OFF fixture is not game-input validation.",
    "The timed-OFF-output scope validates menu OFF, timing and SDK single-frame output only; auxiliary inputs and a per-packet capture skip are not validated. Cumulative skipped captures show aggregate activity, never this packet's identity; zero delta does not prove the optimization ran.",
    "Performance benefit comparisons require the same scene and settings. Different scenes and fluctuating cadence are not a controlled A/B result.",
]


def uint(value, positive=False):
    return value if type(value) is int and (value > 0 if positive else value >= 0) and value < 1 << 64 else None


def device(value):
    if isinstance(value, str) and re.fullmatch(r"0x[0-9a-fA-F]{1,16}", value) and int(value, 16):
        return hex(int(value, 16))
    return None


def counter(value):
    if not isinstance(value, dict) or any(uint(value.get(k)) is None for k in ("count", "total_ns", "max_ns")):
        return None
    result = {k: value[k] for k in ("count", "total_ns", "max_ns")}
    if result["max_ns"] > result["total_ns"] or (not result["count"] and (result["total_ns"] or result["max_ns"])):
        return None
    return result


def stage_kind(name):
    if name in CORE[:3]:
        return "runtime_serial_stage"
    if name == "cpu_xell_sleep":
        return "cross_thread_xell"
    if name.startswith("cpu_tracking_"):
        return "aggregate_tracking_wall_not_critical_path"
    if name == "cpu_upscale_observe":
        return "aggregate_upscale_probe_overlaps_runtime"
    return "nested_output_detail"


def snapshot(row):
    if row["event"] == "presentation_off_present" and "present_calls" not in row:
        row = {**row, "present_calls": row.get("xefg_off_frames")}
    positive = ("pid", "cpu_wall_owner_generation", "output_width", "output_height", "present_calls")
    nonnegative = ("uptime_ms", "cpu_wall_control_epoch", "present_mode")
    if any(uint(row.get(k), True) is None for k in positive) or any(uint(row.get(k)) is None for k in nonnegative) or not device(row.get("device")):
        return None, "missing_or_invalid_identity"
    if row.get("cpu_wall_cumulative") is not True or row.get("cpu_wall_max_scope") != "owner_lifetime":
        return None, "missing_cumulative_contract"
    if row["present_mode"] not in (0, 2):
        return None, "unsupported_or_unknown_present_mode"
    if row.get("sdk_output_succeeded") is not True or row.get("output_occluded") is not False:
        return None, "unsuccessful_or_occluded_or_unknown_output"
    if row["event"] == "continuous_present":
        if any(row.get(k) is not True for k in ("sdk_status_valid", "complete_frame_timing")):
            return None, "startup_or_missing_game_input_timing"
        if any(row.get(k) is not False for k in ("sdk_enabled", "requested_enabled", "menu_requested_enabled")):
            return None, "not_requested_and_actual_off"
        if type(row.get("sdk_frames_presented")) is not int or row["sdk_frames_presented"] != 1:
            return None, "off_sdk_frame_count_not_one"
        if row.get("input_matched") is True:
            scope = "sampled_game_input_off"
        elif row.get("input_matched") is False and row.get("aux_capture_policy") == "skip_observed_off" and row.get("menu_input_observed") is True:
            scope = "sampled_timed_off_output_aux_not_validated"
        else:
            return None, "startup_or_missing_game_input_timing"
    else:
        if row.get("result") != 0:
            return None, "unsuccessful_off_fixture"
        scope = "provider_absent_off_not_game_input_validation"
    tracking_enabled = row.get("cpu_tracking_timing_enabled", True)
    if type(tracking_enabled) is not bool:
        return None, "invalid_tracking_timing_state"
    stages = {}
    for name in CORE + OPTIONAL:
        if not tracking_enabled and name.startswith("cpu_tracking_"):
            continue
        if name not in row and name in OPTIONAL:
            continue
        value = counter(row.get(name))
        if value is None:
            return None, "missing_or_invalid_counter"
        stages[name] = value
    return {"pid": row["pid"], "device": device(row["device"]), "scope": scope,
            "owner_generation": row["cpu_wall_owner_generation"],
            "output_extent": [row["output_width"], row["output_height"]],
            "control_epoch": row["cpu_wall_control_epoch"], "present_mode": row["present_mode"],
            "cpu_tracking_timing_enabled": tracking_enabled,
            "aux_capture_policy": row.get("aux_capture_policy") if isinstance(row.get("aux_capture_policy"), str) else None,
            "skipped_off_captures": uint(row.get("skipped_off_captures")),
            "uptime_ms": row["uptime_ms"], "present_calls": row["present_calls"], "stages": stages,
            "empty_scope_calibration": counter(row.get("cpu_tracking_empty_scope")) if tracking_enabled else None}, None


def difference(before, after):
    if before["cpu_tracking_timing_enabled"] != after["cpu_tracking_timing_enabled"]:
        return None, "tracking_timing_state_changed"
    identity = ("scope", "owner_generation", "output_extent", "control_epoch", "present_mode", "aux_capture_policy",
                "cpu_tracking_timing_enabled")
    if any(before[k] != after[k] for k in identity) or before["stages"].keys() != after["stages"].keys():
        return None, "identity_extent_control_mode_or_schema_changed"
    elapsed = after["uptime_ms"] - before["uptime_ms"]
    presents = after["present_calls"] - before["present_calls"]
    if elapsed <= 0 or presents <= 0:
        return None, "time_or_present_count_not_increasing"
    stages = {}
    for name, end in after["stages"].items():
        start = before["stages"][name]
        if any(end[k] < start[k] for k in end):
            return None, "counter_decreased"
        calls, total = end["count"] - start["count"], end["total_ns"] - start["total_ns"]
        if calls == 0 and (total or end["max_ns"] != start["max_ns"]):
            return None, "counter_total_changed_without_calls"
        stages[name] = {"kind": stage_kind(name), "delta_count": calls, "delta_total_ns": total,
                        "mean_wall_ns_per_call": total / calls if calls else None,
                        "mean_status": "measured" if calls else "no_calls",
                        "wall_ns_per_completed_present": total / presents,
                        "max_ns_at_end_lifetime": end["max_ns"]}
    skipped_before, skipped_after = before["skipped_off_captures"], after["skipped_off_captures"]
    skipped_delta = skipped_after - skipped_before if skipped_before is not None and skipped_after is not None and skipped_after >= skipped_before else None
    observation = {"skipped_off_captures_start": skipped_before, "skipped_off_captures_end": skipped_after,
                   "skipped_off_captures_delta": skipped_delta, "current_packet_skip_verified": False} if after["aux_capture_policy"] == "skip_observed_off" else None
    return {**{k: after[k] for k in ("pid", "device") + identity},
            "start_uptime_ms": before["uptime_ms"], "end_uptime_ms": after["uptime_ms"],
            "elapsed_ms": elapsed, "delta_completed_presents": presents,
            "ordinary_sdk_present_cadence_hz": presents * 1000 / elapsed,
            "stages": stages, "empty_scope_calibration_at_end": after["empty_scope_calibration"],
            "off_aux_capture_observation": observation}, None


class Analyzer:
    def __init__(self):
        self.previous = {}
        self.intervals = []
        self.skipped = Counter()
        self.summaries = self.eligible = self.lines = self.invalid_lines = 0
        self.spans = {}

    def barrier(self, row):
        pid, dev = uint(row.get("pid"), True), device(row.get("device"))
        for key in list(self.previous):
            if pid is None or key[0] == pid and (dev is None or key[1] == dev):
                del self.previous[key]

    def accept(self, row):
        if not isinstance(row, dict):
            self.previous.clear()
            self.invalid_lines += 1
            return
        event = row.get("event")
        if not isinstance(event, str):
            return
        if event in BARRIERS:
            self.barrier({"pid": row.get("pid")} if "create_swapchain" in event else row)
            self.skipped["observed_lifecycle_barrier"] += 1
        if event == "presentation_continuous_present" and row.get("result") != 0:
            self.barrier(row)
            self.skipped["unsuccessful_runtime_attempt"] += 1
        if event not in EVENTS:
            return
        self.summaries += 1
        if not any(name in row for name in CORE + OPTIONAL) and "cpu_wall_cumulative" not in row:
            self.skipped["summary_without_cpu_metrics"] += 1
            return
        pid, dev, timestamp = uint(row.get("pid"), True), device(row.get("device")), uint(row.get("uptime_ms"))
        if pid is not None and dev is not None and timestamp is not None:
            first, last = self.spans.get((pid, dev), (timestamp, timestamp))
            self.spans[(pid, dev)] = (min(first, timestamp), max(last, timestamp))
        sample, reason = snapshot(row)
        if sample is None:
            self.barrier(row)
            self.skipped[reason] += 1
            return
        self.eligible += 1
        key = (sample["pid"], sample["device"])
        before = self.previous.get(key)
        self.previous[key] = sample
        if before is None:
            self.skipped["first_snapshot_after_boundary"] += 1
            return
        interval, reason = difference(before, sample)
        if interval is None:
            self.skipped[reason] += 1
        else:
            self.intervals.append(interval)

    def read(self, path):
        if path.stat().st_size > MAX_BYTES:
            raise ValueError("Explicit JSONL exceeds the 64 MiB analysis budget")
        with path.open("rb") as stream:
            for raw in stream:
                self.lines += 1
                if self.lines > MAX_LINES or len(raw) > MAX_LINE_BYTES:
                    raise ValueError("JSONL exceeds the line-count/line-size analysis budget")
                try:
                    row = json.loads(raw)
                except (ValueError, UnicodeError):
                    self.previous.clear()
                    self.invalid_lines += 1
                    continue
                self.accept(row)

    def finish(self):
        covered_ms = sum(i["elapsed_ms"] for i in self.intervals)
        observed_ms = sum(last - first for first, last in self.spans.values())
        return {"schema": "observer_cpu_wall_differences_v1",
                "status": "stable_intervals_found" if self.intervals else "insufficient_evidence",
                "coverage": {"lines_read": self.lines, "invalid_lines": self.invalid_lines,
                             "summary_snapshots": self.summaries, "eligible_snapshots": self.eligible,
                             "eligible_snapshot_fraction": self.eligible / self.summaries if self.summaries else None,
                             "accepted_intervals": len(self.intervals),
                             "observed_device_span_ms": observed_ms,
                             "accepted_device_interval_ms": covered_ms,
                             "accepted_device_time_fraction": covered_ms / observed_ms if observed_ms else None,
                             "completed_presents_in_accepted_intervals": sum(i["delta_completed_presents"] for i in self.intervals)},
                "skipped": dict(self.skipped), "intervals": self.intervals, "limitations": LIMITATIONS}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("observer_jsonl", type=Path, help="One explicit observer.jsonl; no directory discovery")
    parser.add_argument("--output", type=Path, help="Optional new analysis JSON outside the source session")
    args = parser.parse_args()
    source = args.observer_jsonl.resolve(strict=True)
    if not source.is_file():
        parser.error("Input must be an explicit JSONL file")
    if args.output and source.parent in args.output.resolve().parents:
        parser.error("Output must be outside the source session directory")
    analyzer = Analyzer()
    analyzer.read(source)
    result = {"input": str(source), **analyzer.finish()}
    encoded = json.dumps(result, indent=2, ensure_ascii=False, allow_nan=False) + "\n"
    if args.output:
        with args.output.open("x", encoding="utf-8") as stream:
            stream.write(encoded)
    else:
        sys.stdout.write(encoded)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError) as error:
        print(f"CPU wall analysis refused: {error}", file=sys.stderr)
        raise SystemExit(2)
