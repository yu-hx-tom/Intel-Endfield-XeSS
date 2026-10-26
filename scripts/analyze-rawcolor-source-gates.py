#!/usr/bin/env python3
"""Read only selected saved logs through the bounded scene JSONL reader.

Report observed raw3 descriptor, recorded-prefix and memory facts. This does
not infer GPU readiness, frame identity, pixel capture or copy permission.
"""
from __future__ import annotations

import argparse
from collections import Counter
import importlib.util
import json
from pathlib import Path
import sys

sys.dont_write_bytecode = True
SPEC = importlib.util.spec_from_file_location("rawcolor_bounded_reader", Path(__file__).with_name("analyze-dual-input-scene.py"))
READER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(READER)
BASE_ANALYZER = READER.Analyzer
IDENTITY = ("pid", "device", "dispatch_call", "command", "command_generation", "record_ordinal", "pipeline", "pipeline_generation")
RAW = {"raw_depth": (16, 5, 130, 2), "raw_color": (32, 6, 122, 1), "raw_motion": (8, 7, 83, 1)}
LAYOUT_ISSUES = ("QueryMissing", "IdentityMismatch", "PrefixMismatch", "RecordingUnsupported", "UnknownObject", "StaleObject", "UnsupportedShape", "RangeAmbiguous", "NoTransitionAnchor", "ChainMismatch", "DeclaredLayoutMismatch", "UnsupportedPNext", "UnsupportedDependency", "OwnershipTransfer", "JournalOverflow", "OpaqueControl", "PassIdentityUnknown", "PassUnsupported", "CombinedAspectRequired", "UnsupportedBarrierScope", "UnsupportedLayout")
MEMORY_ISSUES = ("InvalidInput", "DriverFailed", "Capacity", "UnknownObject", "StaleObject", "Lifecycle", "UnsupportedResource", "UnsupportedAllocation", "UnsupportedBinding", "MissingRequirements", "InvalidRequirements", "OutOfBounds", "Alignment", "MemoryType", "DedicatedMismatch", "BindingUnknown", "MissingBinding", "Overlap", "CoverageIncomplete", "IsolationUnknown", "GenerationExhausted", "Stopped", "UnsupportedDeviceScope", "AliasGranularityUnknown", "AliasRangeOverflow")
ROW_FIELDS = IDENTITY + ("uptime_ms", "thread_id", "set_index", "binding", "array_element", "descriptor_type", "set", "set_generation", "descriptor_version", "view", "view_generation", "view_type", "view_format", "image", "image_generation", "declared_layout", "aspect_mask", "base_mip", "level_count", "base_layer", "layer_count", "format", "width", "height", "depth", "mip_levels", "array_layers", "samples", "usage", "flags", "tiling", "sharing_mode", "initial_layout", "source_match_mask", "same_image_as_source_sample")
HEADER_FIELDS = IDENTITY + ("uptime_ms", "thread_id", "metadata_accepted", "issues", "sample_reason", "binding_count", "groups", "matched_dispatch_call")


def names(mask, table):
    if READER.uint(mask) is None:
        return ["unknown_mask"]
    result = [name for bit, name in enumerate(table) if mask & (1 << bit)]
    unknown = mask & ~((1 << len(table)) - 1)
    return result + (["UnknownBits:" + hex(unknown)] if unknown else [])


def small(value, depth=0):
    """Copy only bounded JSON objects used in the observed nested schema."""
    if depth > 4:
        return None
    if type(value) in (int, bool, str) or value is None:
        return value if not isinstance(value, str) or len(value) <= 512 else None
    if isinstance(value, list) and len(value) <= 64:
        return [small(item, depth + 1) for item in value]
    if isinstance(value, dict) and len(value) <= 80:
        return {key: small(item, depth + 1) for key, item in value.items() if isinstance(key, str) and len(key) <= 80}
    return None


def pick(record, fields):
    return {field: small(record[field]) for field in fields if field in record}


def exact_pattern(row, source):
    bit, binding, _, _ = RAW[source]
    mask = READER.uint(row.get("source_match_mask"))
    return (mask is not None and bool(mask & bit) and row.get("same_image_as_source_sample") is True
            and row.get("set_index") == 0 and row.get("binding") == binding and row.get("descriptor_type") == 2
            and all(READER.handle(row.get(field)) and READER.uint(row.get(field + "_generation"), True)
                    for field in ("image", "view", "set")))


def visible_color_gates(header, rows):
    """Diagnose emitted facts; not an invocation of the production probe."""
    failures, unknown = [], []
    f = header.get("recording_facts") or {}
    for field in ("allocation_known", "pool_known", "begin_flags_known", "state_known", "recording", "scope_known"):
        if field not in f:
            unknown.append("recording." + field)
        elif f[field] is not True:
            failures.append("recording." + field)
    for field, expected in (("level", 0), ("queue_family", 0), ("in_render_pass", False), ("in_dynamic_rendering", False)):
        if field not in f:
            unknown.append("recording." + field)
        elif f[field] != expected:
            failures.append("recording." + field)
    if type(f.get("begin_usage_flags")) is not int:
        unknown.append("recording.begin_usage_flags")
    elif f["begin_usage_flags"] & ~1:
        failures.append("recording.unsupported_begin_usage_flags")
    if type(f.get("pool_flags")) is not int:
        unknown.append("recording.pool_flags")
    elif f["pool_flags"] & 4:
        failures.append("recording.protected_pool")
    if type(f.get("reasons")) is not int:
        unknown.append("recording.reasons")
    elif f["reasons"] & ~2048:  # flags0 NotOneTime is explicitly admitted by raw probe.
        failures.append("recording.control_reasons")
    color, depth, motion = (rows[key] for key in ("raw_color", "raw_depth", "raw_motion"))
    for source, row in rows.items():
        _, _, fmt, aspect = RAW[source]
        for field, expected in (("view_format", fmt), ("aspect_mask", aspect), ("base_mip", 0), ("level_count", 1), ("base_layer", 0), ("layer_count", 1)):
            if field not in row:
                unknown.append(source + "." + field)
            elif row[field] != expected:
                failures.append(source + "." + field)
    for field, expected in (("format", 122), ("depth", 1), ("mip_levels", 1), ("array_layers", 1), ("samples", 1), ("flags", 0), ("tiling", 0), ("sharing_mode", 0), ("view_type", 1)):
        if field not in color:
            unknown.append("raw_color." + field)
        elif color[field] != expected:
            failures.append("raw_color." + field)
    for field in ("width", "height"):
        values = [row.get(field) for row in (color, depth, motion)]
        if any(READER.uint(value, True) is None for value in values):
            unknown.append("raw3." + field)
        elif len(set(values)) != 1 or values[0] > 4096:
            failures.append("raw3." + field)
    usage = READER.uint(color.get("usage"))
    if usage is None:
        unknown.append("raw_color.usage")
    elif not usage & 1:
        failures.append("raw_color.transfer_src_absent")
    if len({(row.get("image"), row.get("image_generation")) for row in (color, depth, motion)}) != 3:
        failures.append("raw3.same_image")
    layout, memory = color.get("recorded_source_layout") or {}, color.get("resource_memory") or {}
    for field in ("queried", "recorded_layout_resolved", "recorded_prefix_supported"):
        if field not in layout:
            unknown.append("raw_color.layout." + field)
        elif layout[field] is not True:
            failures.append("raw_color.layout." + field)
    for field, expected in (("issues", 0), ("recorded_layout", 5), ("resolved_layout_range", [1, 0, 1, 0, 1])):
        if field not in layout:
            unknown.append("raw_color.layout." + field)
        elif layout[field] != expected:
            failures.append("raw_color.layout." + field)
    for field, expected in (("image", color.get("image")), ("image_generation", color.get("image_generation")), ("device", f.get("device")), ("context_incarnation", f.get("context_incarnation")), ("command", f.get("command")), ("rp_command_generation", f.get("rp_command_generation")), ("prefix_order", f.get("common_record_order"))):
        if field not in layout or expected is None:
            unknown.append("raw_color.layout_identity." + field)
        elif layout[field] != expected:
            failures.append("raw_color.layout_identity." + field)
    for field in ("queried", "bounds_known", "alias_free_observed"):
        if field not in memory:
            unknown.append("raw_color.memory." + field)
        elif memory[field] is not True:
            failures.append("raw_color.memory." + field)
    for field in ("issues", "local_issues", "global_issues"):
        if type(memory.get(field)) is not int:
            unknown.append("raw_color.memory." + field)
        elif memory[field]:
            failures.append("raw_color.memory." + field)
    if not READER.handle(memory.get("memory")) or READER.uint(memory.get("memory_generation"), True) is None:
        failures.append("raw_color.memory.identity_unknown")
    return {"known_refusal_fields": failures, "unknown_fields": unknown,
            "all_emitted_checked_fields_satisfy_recipe": not failures and not unknown,
            "copy_permitted": False, "probe_was_invoked": False}


class RawGateAnalyzer(BASE_ANALYZER):
    def __init__(self, max_records=4096, max_samples=128):
        super().__init__(max_records, max_samples)
        self.groups = {}
        self.detail_count = 0
        self.all_events = Counter()
        self.latest_summaries = {}
        self.raw_events = []
        self.omissions = Counter()

    def observe(self, record, file_id, line):
        if not isinstance(record, dict):
            return
        event = record.get("event")
        if not isinstance(event, str):
            return
        if event in self.all_events or len(self.all_events) < 256:
            self.all_events[event] += 1
        if event.startswith("rawcolor_probe_") and len(self.raw_events) < 64:
            self.raw_events.append({"file_id": file_id, "line": line, **small(record)})
        if event in {"descriptor_image_summary", "resource_memory_summary", "resource_memory_observer_summary", "render_pass_summary", "render_pass_observer_summary"}:
            key = file_id, record.get("pid"), record.get("device"), event
            if key in self.latest_summaries or len(self.latest_summaries) < 64:
                self.latest_summaries[key] = {"file_id": file_id, "line": line, **small(record)}
        if event not in {"descriptor_pre_dispatch", "descriptor_image_binding"}:
            return
        key = (file_id,) + tuple(record.get(field) for field in IDENTITY)
        if any(type(value) not in (int, str) for value in key):
            self.omissions["invalid_identity"] += 1
            return
        if self.detail_count >= self.max_records:
            self.omissions["detail_capacity"] += 1
            return
        if event == "descriptor_image_binding":
            mask = READER.uint(record.get("source_match_mask"))
            if mask is None or not mask & 56:
                return
        self.detail_count += 1
        group = self.groups.setdefault(key, {"file_id": file_id, "header": None, "bindings": []})
        output = {"line": line, **pick(record, HEADER_FIELDS if event == "descriptor_pre_dispatch" else ROW_FIELDS)}
        for field in ("recording_facts", "recorded_source_layout", "resource_memory", "source_samples"):
            if field in record:
                output[field] = small(record[field])
        if event == "descriptor_pre_dispatch":
            if group["header"] is not None:
                self.omissions["duplicate_header"] += 1
            else:
                group["header"] = output
        elif len(group["bindings"]) < 32:
            group["bindings"].append(output)
        else:
            self.omissions["bindings_per_group_capacity"] += 1

    def finish(self, limits):
        facts, patterns = [], []
        layout_counts = {source: Counter() for source in RAW}
        memory_counts = {source: Counter() for source in RAW}
        counterexamples, unknown, tested = Counter(), Counter(), 0
        accepted, refused = 0, 0
        for group in self.groups.values():
            header = group["header"] or {}
            qualified = header.get("metadata_accepted") is True and header.get("issues") == 0
            accepted += qualified
            refused += header.get("metadata_accepted") is False
            if not group["bindings"]:
                continue
            row_sources = {}
            pattern = {}
            for source, (bit, _, _, _) in RAW.items():
                selected = [row for row in group["bindings"] if type(row.get("source_match_mask")) is int and row["source_match_mask"] & bit]
                if selected:
                    row_sources[source] = selected
                exact = [row for row in selected if exact_pattern(row, source)]
                if len(exact) == 1:
                    pattern[source] = exact[0]
                for row in selected:
                    layout, memory = row.get("recorded_source_layout") or {}, row.get("resource_memory") or {}
                    layout_counts[source][str(layout.get("status", "unknown"))] += 1
                    for issue in names(layout.get("issues"), LAYOUT_ISSUES):
                        layout_counts[source]["issue:" + issue] += 1
                    memory_counts[source][str(memory.get("status", "unknown"))] += 1
                    for issue in names(memory.get("issues"), MEMORY_ISSUES):
                        memory_counts[source]["issue:" + issue] += 1
            full = qualified and len(pattern) == 3
            item = {**group, "accepted_descriptor_header": qualified, "all_raw3_sources_in_same_recorded_dispatch": qualified and len(row_sources) == 3,
                    "exact_sampled_bindings_5_6_7_pattern": full}
            for row in item["bindings"]:
                layout, memory = row.get("recorded_source_layout") or {}, row.get("resource_memory") or {}
                layout["issue_names"] = names(layout.get("issues"), LAYOUT_ISSUES)
                memory["issue_names"] = names(memory.get("issues"), MEMORY_ISSUES)
            if full:
                item["visible_rawcolor_recipe_gates"] = visible_color_gates(header, pattern)
                counterexamples.update(item["visible_rawcolor_recipe_gates"]["known_refusal_fields"])
                unknown.update(item["visible_rawcolor_recipe_gates"]["unknown_fields"])
                tested += item["visible_rawcolor_recipe_gates"]["all_emitted_checked_fields_satisfy_recipe"]
                patterns.append(item)
            facts.append(item)
        return {"schema": "rawcolor-source-gates-scene-v1", "inputs": self.files, "limits": limits,
                "read_truncated": any(item["truncated"] for item in self.files), "events": dict(self.all_events),
                "omitted": dict(self.omissions), "retained_detail_records": self.detail_count,
                "retained_accepted_headers": accepted, "retained_refused_headers": refused,
                "raw_source_layout_counts": {key: dict(value) for key, value in layout_counts.items()},
                "raw_source_memory_counts": {key: dict(value) for key, value in memory_counts.items()},
                "groups_with_raw_detail": len(facts), "groups_with_all_raw3": sum(item["all_raw3_sources_in_same_recorded_dispatch"] for item in facts),
                "groups_with_exact_sampled_5_6_7": len(patterns), "patterns_emitted_checks_satisfied": tested,
                "pattern_known_refusal_fields": dict(counterexamples), "pattern_unknown_fields": dict(unknown),
                "pattern_groups": patterns[:self.max_samples], "other_raw_groups": [item for item in facts if not item["exact_sampled_bindings_5_6_7_pattern"]][:self.max_samples],
                "rawcolor_events": self.raw_events, "latest_summaries": list(self.latest_summaries.values()),
                "limitations": ["Only emitted, sampled detail is evaluated; missing detail does not mean never bound or always refused.",
                    "Descriptor generation, RP recording generation, prefix order and callback source counters are separate namespaces, not frame IDs.",
                    "Layout refusal of depth or motion is recorded independently; this single-color recipe consumes the color layout and memory facts.",
                    "Emitted recipe checks are an offline comparison, not evidence the live probe ran or accepted, armed or copied.",
                    "Native producer dependencies, shader semantics, complete hook coverage, final execution layout and lifetime permission remain unproven.",
                    "The startup concurrency runtime flag was not logged; scene cause remains an inference until direct runtime evidence."],
                "same_frame_verified": False, "gpu_completion_verified": False, "copy_permitted": False,
                "game_pixels_captured": False, "xefg_integrated": False}


def analyze(paths, **limits):
    READER.Analyzer = RawGateAnalyzer
    try:
        return READER.analyze(paths, **limits)
    finally:
        READER.Analyzer = BASE_ANALYZER


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("logs", nargs="+", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    paths = [(path / "observer.jsonl" if path.is_dir() else path).resolve() for path in args.logs]
    if args.output.resolve() in paths:
        parser.error("Output must not replace a source log")
    result = analyze(paths, max_bytes=64 * 1024 * 1024, max_lines=250000, max_line_bytes=64 * 1024, max_records=4096, max_samples=128)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, ensure_ascii=False, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    print(json.dumps({key: result[key] for key in ("groups_with_raw_detail", "groups_with_all_raw3", "groups_with_exact_sampled_5_6_7", "patterns_emitted_checks_satisfied", "pattern_known_refusal_fields", "pattern_unknown_fields", "raw_source_layout_counts", "raw_source_memory_counts", "read_truncated")}, indent=2))


if __name__ == "__main__":
    main()
