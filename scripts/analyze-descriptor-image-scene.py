#!/usr/bin/env python3
"""Bounded offline descriptor analysis using the existing audited JSONL reader.

Only explicitly named saved logs are read. Source counters and recording
ordinals are independent namespaces; this never establishes game frame input.
"""
from __future__ import annotations

import argparse
from collections import Counter
import importlib.util
import json
from pathlib import Path
import sys

sys.dont_write_bytecode = True
SPEC = importlib.util.spec_from_file_location("bounded_dual_scene", Path(__file__).with_name("analyze-dual-input-scene.py"))
READER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(READER)
BASE_ANALYZER = READER.Analyzer
ISSUES = ("DriverFailed", "InvalidInput", "Capacity", "UnknownObject", "StaleGeneration", "Lifecycle",
          "UnsupportedExtension", "UnsupportedDescriptor", "ImmutableSampler", "CrossBinding", "UndefinedDescriptor",
          "DescriptorChanged", "IncompatibleLayout", "Secondary", "UnsupportedDispatch", "UnsupportedView",
          "UnsupportedLayout", "Stopped", "GenerationExhausted", "MissingBinding", "SimultaneousUse", "TemplateReadFailed")
SOURCES = ("fi_motion", "fi_depth", "fi_real_color", "raw_motion", "raw_depth", "raw_color", "dilated_motion",
           "dilated_depth", "reconstructed_depth", "farthest_depth", "luma")
CLAIMS = ("complete_resource_set", "snapshot_atomic", "same_frame_verified", "source_semantics_verified",
          "undilated_verified", "shader_semantics_verified", "layout_verified", "submitted", "gpu_complete",
          "resource_lifetime_pinned", "copy_permitted", "pixels_captured", "xefg_integrated")
DESCRIPTOR_EVENTS = {"descriptor_pre_dispatch", "descriptor_image_binding", "descriptor_image_summary"}
BINDING_FIELDS = ("dispatch_call", "command", "command_generation", "record_ordinal", "pipeline", "pipeline_generation",
                  "set_index", "binding", "array_element", "descriptor_type", "set", "set_generation", "descriptor_version",
                  "view", "view_generation", "view_type", "view_format", "image", "image_generation", "declared_layout",
                  "aspect_mask", "base_mip", "level_count", "base_layer", "layer_count", "format", "width", "height",
                  "depth", "mip_levels", "array_layers", "samples", "usage", "flags", "tiling", "sharing_mode", "initial_layout",
                  "source_match_mask", "same_image_as_source_sample")


def issue_names(mask):
    if READER.uint(mask) is None:
        return ["invalid_or_missing_mask"]
    result = [name for bit, name in enumerate(ISSUES) if mask & (1 << bit)]
    unknown = mask & ~((1 << len(ISSUES)) - 1)
    return result + (["UnknownBits:" + hex(unknown)] if unknown else [])


class DescriptorAnalyzer(BASE_ANALYZER):
    def __init__(self, max_records=4096, max_samples=128):
        super().__init__(max_records, max_samples)
        self.descriptor_scopes = {}
        self.descriptor_events = Counter()
        self.descriptor_omitted = Counter()
        self.retained_descriptor_records = 0
        self.retained_source_metadata = 0

    def observe(self, record, file_id, line):
        super().observe(record, file_id, line)
        if not isinstance(record, dict):
            return
        event = record.get("event")
        if event not in DESCRIPTOR_EVENTS | {"upscale_image_metadata_match", "fsr_image_metadata_match"}:
            return
        pid, device = READER.uint(record.get("pid"), True), READER.handle(record.get("device"))
        if pid is None or device is None:
            self.descriptor_omitted["invalid_scope"] += 1
            return
        current = self.active.get((file_id, pid, device))
        if current and current.closed:
            self.descriptor_omitted["detail_after_device_destroy"] += 1
            return
        epoch = current.scope["epoch"] if current else "unanchored"
        key = file_id, pid, device, epoch
        if key not in self.descriptor_scopes:
            if len(self.descriptor_scopes) >= 64:
                self.descriptor_omitted["scope_capacity"] += 1
                return
            self.descriptor_scopes[key] = {
                "scope": dict(current.scope) if current else {"file_id": file_id, "pid": pid, "device": device, "epoch": epoch, "start_observed": False},
                "events": Counter(), "claim_violations": Counter(), "groups": [], "bindings": [],
                "summaries": 0, "maxima": {}, "latest_summary": None, "image_metadata": {},
                "object_observations": {}, "first_capacity_summary": None, "first_template_refusal_summary": None,
            }
        state = self.descriptor_scopes[key]
        if event in {"upscale_image_metadata_match", "fsr_image_metadata_match"}:
            image = READER.handle(record.get("image_candidate"))
            generation = READER.uint(record.get("image_generation"), True)
            if record.get("numeric_live_handle_match") is True and image and generation:
                identity = image, generation
                if identity not in state["image_metadata"] and self.retained_source_metadata >= self.max_records:
                    self.descriptor_omitted["source_metadata_capacity"] += 1
                else:
                    if identity not in state["image_metadata"]:
                        self.retained_source_metadata += 1
                    state["image_metadata"][identity] = {**READER.scalar_record(record, 55), "line": line}
            return
        self.descriptor_events[event] += 1
        state["events"][event] += 1
        for claim in CLAIMS:
            if record.get(claim) is not False:
                state["claim_violations"][claim] += 1
        if event == "descriptor_image_summary":
            state["summaries"] += 1
            summary = READER.scalar_record(record, 100)
            summary["line"] = line
            for field in ("object_usage", "source_samples", "refused_issue_records", "template_refused_issue_records"):
                values = record.get(field)
                if values is None and field == "template_refused_issue_records":
                    values = []
                if not isinstance(values, list) or len(values) > {"object_usage": 10, "source_samples": 11, "refused_issue_records": 64, "template_refused_issue_records": 64}[field]:
                    self.descriptor_omitted["invalid_summary_array:" + field] += 1
                    summary[field] = []
                else:
                    summary[field] = [READER.scalar_record(item, 12) for item in values if isinstance(item, dict)]
            for issue in summary["refused_issue_records"] + summary["template_refused_issue_records"]:
                issue["issue_names"] = issue_names(issue.get("issue_bit"))
            for usage in summary["object_usage"]:
                kind = usage.get("kind")
                if not isinstance(kind, str) or len(kind) > 64:
                    continue
                if kind not in state["object_observations"] and len(state["object_observations"]) >= 10:
                    self.descriptor_omitted["object_kind_capacity"] += 1
                    continue
                obj = state["object_observations"].setdefault(kind, {"minimum_observed_live": None, "maximum_observed_live": 0,
                          "latest": {}, "first_capacity_refusal": None, "first_observed_at_capacity": None})
                live, capacity = READER.uint(usage.get("live")), READER.uint(usage.get("capacity"))
                if live is not None:
                    obj["minimum_observed_live"] = live if obj["minimum_observed_live"] is None else min(obj["minimum_observed_live"], live)
                    obj["maximum_observed_live"] = max(obj["maximum_observed_live"], live)
                point = {"line": line, "uptime_ms": record.get("uptime_ms"), "dispatch_calls": record.get("dispatch_calls"), **usage}
                obj["latest"] = point
                if capacity is not None and live is not None and live >= capacity and obj["first_observed_at_capacity"] is None:
                    obj["first_observed_at_capacity"] = point
                if READER.uint(usage.get("capacity_refusals")) and obj["first_capacity_refusal"] is None:
                    obj["first_capacity_refusal"] = point
            for name, count in (("first_capacity_summary", "capacity_refusals"), ("first_template_refusal_summary", "template_update_refusals")):
                if READER.uint(record.get(count)) and state[name] is None:
                    state[name] = summary
            summary["observed_issue_names"] = issue_names(summary.get("observed_issues"))
            summary["fatal_issue_names"] = issue_names(summary.get("fatal_issues"))
            for field, value in summary.items():
                if READER.uint(value) is not None and (field in state["maxima"] or len(state["maxima"]) < 100):
                    state["maxima"][field] = max(value, state["maxima"].get(field, 0))
            state["latest_summary"] = summary
            return
        if self.retained_descriptor_records >= self.max_records:
            self.descriptor_omitted["detail_capacity"] += 1
            return
        self.retained_descriptor_records += 1
        if event == "descriptor_pre_dispatch":
            group = READER.scalar_record(record, 60)
            group["line"] = line
            group["issue_names"] = issue_names(record.get("issues"))
            group["groups"] = record.get("groups") if isinstance(record.get("groups"), list) and len(record["groups"]) == 3 else []
            state["groups"].append(group)
        else:
            binding = {key: record[key] for key in BINDING_FIELDS if key in record and type(record[key]) in (str, int, bool)}
            binding["line"] = line
            samples = record.get("source_samples")
            binding["source_samples"] = [READER.scalar_record(value, 6) for value in samples[:11] if isinstance(value, dict)] if isinstance(samples, list) else []
            binding["inventory_identity_valid"] = all(READER.handle(record.get(field)) and READER.uint(record.get(field + "_generation"), True)
                                                       for field in ("image", "view", "set"))
            state["bindings"].append(binding)

    def finish(self, limits):
        base = super().finish(limits)
        contexts = []
        for state in self.descriptor_scopes.values():
            latest = state["latest_summary"] or {}
            sources = []
            for sample in latest.get("source_samples", []):
                key = READER.handle(sample.get("image")), READER.uint(sample.get("image_generation"), True)
                metadata = state["image_metadata"].get(key)
                sources.append({**sample, "historical_description_for_exact_inventory_identity": metadata,
                                "view": "unknown_without_accepted_binding_detail", "view_aspect": "unknown_without_accepted_binding_detail",
                                "declared_layout": "unknown_without_accepted_binding_detail", "same_frame_verified": False})
            matches = []
            groups = state["groups"]
            identity_fields = ("dispatch_call", "command", "command_generation", "record_ordinal", "pipeline", "pipeline_generation")
            accepted_headers = {tuple(group.get(field) for field in identity_fields) for group in groups
                                if group.get("metadata_accepted") is True and group.get("issues") == 0}
            qualified_bindings = [binding for binding in state["bindings"] if state["scope"]["start_observed"]
                                  and binding["inventory_identity_valid"]
                                  and tuple(binding.get(field) for field in identity_fields) in accepted_headers]
            for source in ("raw_motion", "raw_depth", "raw_color", "fi_real_color"):
                bindings = [binding for binding in qualified_bindings if any(sample.get("source_key") == source for sample in binding["source_samples"])]
                matches.append({"source": source, "retained_binding_rows": len(bindings), "examples": bindings[:4]})
            contexts.append({"scope": state["scope"], "event_counts": dict(state["events"]), "claim_violations": dict(state["claim_violations"]),
                             "retained_accepted_groups": sum(group.get("metadata_accepted") is True for group in groups),
                             "retained_refused_groups": sum(group.get("metadata_accepted") is False for group in groups),
                             "retained_issue_masks": dict(Counter(str(group.get("issues")) for group in groups)),
                             "retained_dispatch_groups": groups, "retained_binding_count": len(state["bindings"]),
                             "summary_observations": state["summaries"], "summary_maxima": state["maxima"], "latest_summary": latest,
                             "object_observations": state["object_observations"], "first_capacity_summary": state["first_capacity_summary"],
                             "first_template_refusal_summary": state["first_template_refusal_summary"],
                             "latest_source_samples": sources, "source_binding_matches": matches,
                             "command_begin_flags": "unknown_not_emitted_by_observed_schema", "same_recording_submit_count": "unknown_no_descriptor_submission_record",
                             "secondary_relationships": "unknown_no_relationship_record", "usable_binding_metadata_observed": bool(qualified_bindings)})
        return {"schema": "descriptor-image-scene-analysis-v1", "inputs": base["inputs"], "limits": base["limits"],
                "read_truncated": base["read_truncated"], "descriptor_event_counts": dict(self.descriptor_events),
                "descriptor_omitted_or_rejected": dict(self.descriptor_omitted), "descriptor_contexts": contexts,
                "source_and_render_pass_analysis": base,
                "limitations": ["Descriptor metadata acceptance does not prove shader reads or writes, submitted producer order, or copy permission.",
                                "Source callback counters, dispatch calls, command generations and record ordinals are independent; none is a frame ID.",
                                "A numeric source mask indicates only a cached inventory identity; raw inputs and late FI real color are not a jointly ready resource set.",
                                "No accepted binding rows means view/aspect/descriptor layout and dispatch-point source membership are unobserved.",
                                "Begin flags and descriptor-recording submission counts are unknown unless emitted explicitly; absence of a SimultaneousUse bit does not prove ONE_TIME_SUBMIT.",
                                "Detail quotas omit later rejected dispatches; cumulative summary issue counts may overlap and are not exclusive categories."],
                "same_frame_verified": False, "game_pixels_copied": False, "gpu_completion_verified": False,
                "copy_permitted": False, "xefg_integration_verified": False}


def analyze(paths, **limits):
    READER.Analyzer = DescriptorAnalyzer
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
        parser.error("Output must not replace an input")
    result = analyze(paths, max_bytes=64*1024*1024, max_lines=250000, max_records=4096, max_samples=128)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, ensure_ascii=False, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    print(str(args.output.resolve()))


if __name__ == "__main__":
    main()
