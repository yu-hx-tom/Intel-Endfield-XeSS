#!/usr/bin/env python3
"""Bounded, offline metadata analysis. Reads only explicitly named JSONL files.

No game process, pixels, memory, Vulkan calls or external modules are opened.
An equal image/generation is a historical identity match, never a frame match.
"""
from __future__ import annotations

import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import re
import sys

PROPERTIES = (
    "r_input_motion_vectors", "r_input_depth", "r_input_color_jittered",
    "rw_dilated_motion_vectors", "rw_dilated_depth",
    "rw_reconstructed_previous_nearest_depth", "rw_farthest_depth", "rw_current_luma",
)
ROLES = ("motion", "depth", "real_color")
SUMMARY_KEY_BUDGET = 100
FI_ROLES = {
    ("reconstruct_previous_depth", "r_dilated_motion_vectors"): "motion",
    ("reconstruct_previous_depth", "r_dilated_depth"): "depth",
    ("frame_inpainting", "r_present_backbuffer"): "real_color",
}
RP_ISSUES = (
    "DriverFailed", "InvalidInput", "Capacity", "Lifecycle", "StaleGeneration",
    "UnknownObject", "UnsupportedExtension", "UnsupportedImage", "UnsupportedView",
    "UnsupportedFormat", "UnsupportedLayout", "UnsupportedAttachment", "MultipleSubpasses",
    "Imageless", "Secondary", "DynamicRendering", "Aliasing", "IncompatibleFramebuffer",
    "IncompleteRenderPass", "SimultaneousUse", "OneTimeResubmitted", "DeviceLost", "Stopped",
)
SUMMARIES = {
    "upscale_source_summary", "fsr_callback_summary", "image_sync_summary",
    "rp_metadata_summary", "rp_projection_summary", "rp_rejection_counts", "destroy_device",
    "fsr_observer_install", "upscale_observer_install", "fsr_observer_install_summary", "upscale_observer_install_summary",
}
EVENTS = SUMMARIES | {
    "image_sync_start", "create_device", "upscale_source_snapshot", "upscale_image_metadata_match",
    "fsr_image_metadata_match", "image_sync_candidate", "rp_projection_submission",
    "rp_projection_layout_operation", "rp_swapchain_image_observation", "wsi_image_metadata",
    "rp_rejection_sample", "image_sync_submit_evidence",
}
IMAGE_FIELDS = ("format", "width", "height", "depth", "mip_levels", "array_layers", "samples",
                "usage", "flags", "tiling", "sharing_mode", "initial_layout", "candidate_extent_matches")
FALSE_CLAIMS = (
    "same_frame_verified", "undilated_verified", "motion_semantics_verified", "snapshot_atomic",
    "source_association_known", "candidate_semantics_verified", "gpu_complete", "gpu_completion_verified",
    "submitted_layout_known", "resource_lifetime_pinned", "copy_permitted", "xefg_integrated", "xeFG_integrated",
)
LIMITATIONS = [
    "Callback counters, FI selectionGeneration and RP selection_serial are separate namespaces; none is a frame ID.",
    "Identity overlap means the same observed device/image/inventory generation somewhere in this retained scope; not simultaneous use, same contents or current liveness.",
    "Eight complete property observations are not an atomic, pinned resource set. Input-motion property names do not verify undilated motion or pixel semantics.",
    "RP operations are declared attachment layout metadata, not explicit barriers, final layout, last writer, memory-disjointness, GPU completion or copy permission.",
    "No-direct-attachment-reference excludes only direct attachment layout operations within recorded supported passes; it does not prove the candidate was never used or memory cannot alias.",
    "Detail quotas and refresh without detail mean absent/latest retained samples cannot establish current bindings or disabled features.",
    "WSI removed counts have no per-image removal list. Historical identities are retained; no liveness reconstruction is attempted.",
    "Line order is log emission order and may be delayed across locks; it is not CPU execution order or GPU order. Each input file is isolated.",
]


def uint(value, positive=False):
    return value if type(value) is int and (0 < value if positive else 0 <= value) and value <= 0xffffffffffffffff else None


def handle(value):
    if not isinstance(value, str) or not re.fullmatch(r"0x[0-9a-fA-F]{1,16}", value):
        return None
    number = int(value, 16)
    return hex(number) if number else None


def text(value):
    return value[:120] if isinstance(value, str) else "<missing_or_invalid>"


def issue_names(mask):
    value = uint(mask)
    if value is None:
        return ["invalid_or_missing_mask"]
    names = [name for bit, name in enumerate(RP_ISSUES) if value & (1 << bit)]
    unknown = value & ~((1 << len(RP_ISSUES)) - 1)
    if unknown:
        names.append("UnknownBits:" + hex(unknown))
    return names


def scalar_record(record, limit=100):
    """Fixed scalar budget; no raw application payloads or arbitrary arrays."""
    result = {}
    for key, value in record.items():
        if len(result) >= limit:
            break
        if isinstance(key, str) and len(key) <= 80 and (value is None or type(value) in (str, bool, int)):
            if type(value) is int and not -(1 << 63) <= value < 1 << 64:
                continue
            result[key] = text(value) if isinstance(value, str) else value
    return result


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("duplicate JSON key")
        result[key] = value
    return result


class Context:
    def __init__(self, analyzer, file_id, pid, device, ordinal, line, anchor):
        self.a = analyzer
        self.scope = {"file_id": file_id, "pid": pid, "device": device, "epoch": ordinal,
                      "first_line": line, "start_anchor": anchor, "start_observed": bool(anchor)}
        self.closed = False
        self.created = anchor == "create_device"
        self.counts = {}
        self.summaries = {}
        self.identities = {}
        self.groups = {}
        self.samples = []
        self.issue_union = 0

    def count(self, category, key, amount=1):
        counter = self.counts.setdefault(category, Counter())
        key = text(key)
        if key not in counter and len(counter) >= 64:
            key = "<additional_keys_omitted>"
        counter[key] += amount

    def sample(self, kind, record, line, **extra):
        if self.a.samples >= self.a.max_samples or self.a.sample_kinds[kind] >= 24:
            self.a.omitted["samples"] += 1
            self.a.omitted["samples:" + kind] += 1
            return
        self.a.samples += 1
        self.a.sample_kinds[kind] += 1
        self.samples.append({**scalar_record(record, 55), **extra, "kind": kind, "line": line})

    def identity(self, record, image_field="image", generation_field="image_generation"):
        image, generation = handle(record.get(image_field)), uint(record.get(generation_field), True)
        if image is None or generation is None:
            self.a.omitted["invalid_identity"] += 1
            return None
        key = image, generation
        if key not in self.identities:
            if not self.a.reserve("identities"):
                return None
            self.identities[key] = {"image": image, "image_generation": generation,
                "upscale_properties": Counter(), "fi_accepted_roles": Counter(), "fi_metadata_roles": Counter(),
                "fi_summary_roles": Counter(), "projection_roles": Counter(), "projection_source_kinds": Counter(),
                "wsi_identity_observations": 0, "rp_wsi_issues_union": 0,
                "descriptions": [], "descriptor_variants_omitted": 0}
        value = self.identities[key]
        description = {field: record[field] for field in IMAGE_FIELDS if field in record and type(record[field]) in (int, bool)}
        if description and description not in value["descriptions"]:
            if len(value["descriptions"]) < 3:
                value["descriptions"].append(description)
            else:
                value["descriptor_variants_omitted"] += 1
        return value

    def group(self, call):
        call = uint(call, True)
        if call is None:
            self.a.omitted["invalid_upscale_callback_counter"] += 1
            return None
        if call not in self.groups:
            if not self.a.reserve("callback_groups"):
                return None
            self.groups[call] = {"callback_call": call, "snapshot_observations": 0,
                "valid_snapshot_observations": 0, "snapshot_images": None, "properties": {},
                "duplicate_metadata_rows": 0, "conflicting_properties": [], "snapshot_conflict": False,
                "metadata_sample_reasons": {}}
        return self.groups[call]

    def summary(self, event, record, line):
        entry = self.summaries.setdefault(event, {"observations": 0, "maxima": {}, "regressed_fields": [], "mask_unions": {},
            "numeric_key_budget": SUMMARY_KEY_BUDGET, "numeric_field_observations_omitted": 0})
        entry["observations"] += 1
        entry["latest_line"] = line
        current = scalar_record(record)
        previous = entry.get("latest", {})
        entry["latest"] = current
        for key, value in current.items():
            if uint(value) is None or key in ("pid", "uptime_ms", "thread_id", "selectionGeneration"):
                continue
            if key not in entry["maxima"] and key not in entry["mask_unions"] and len(entry["maxima"]) + len(entry["mask_unions"]) >= SUMMARY_KEY_BUDGET:
                entry["numeric_field_observations_omitted"] += 1
                self.a.omitted["summary_numeric_field_capacity"] += 1
                continue
            if "issues" in key or key == "observed_issue_history":
                entry["mask_unions"][key] = entry["mask_unions"].get(key, 0) | value
            else:
                entry["maxima"][key] = max(entry["maxima"].get(key, 0), value)
                if uint(previous.get(key)) is not None and value < previous[key] and key not in entry["regressed_fields"]:
                    entry["regressed_fields"].append(key)
        if event in ("rp_projection_summary", "rp_metadata_summary"):
            for key in ("issues", "observed_issue_history", "fatal_issues"):
                mask = uint(record.get(key))
                if mask is not None and key in entry["mask_unions"]:
                    entry.setdefault("decoded_issue_unions", {})[key] = issue_names(entry["mask_unions"][key])
        if event == "rp_rejection_counts":
            sites = record.get("sites")
            entry["latest_sites"] = [scalar_record(item, 4) for item in sites[:16] if isinstance(item, dict)] if isinstance(sites, list) else []
        if event == "image_sync_summary":
            roles = record.get("current_roles")
            entry["latest_roles"] = []
            if isinstance(roles, list):
                for item in roles[:3]:
                    if not isinstance(item, dict) or item.get("role") not in ROLES:
                        continue
                    entry["latest_roles"].append(scalar_record(item, 24))
                    if uint(item.get("selectionGeneration"), True) and record.get("retired") is False:
                        identity = self.identity(item)
                        if identity is not None:
                            identity["fi_summary_roles"][item["role"]] += 1

    def observe(self, event, record, line):
        self.count("events", event)
        for flag in FALSE_CLAIMS:
            if record.get(flag) is True:
                self.count("unexpected_positive_input_claims", flag)
        if event in SUMMARIES:
            self.summary(event, record, line)
            return
        if event == "upscale_source_snapshot":
            self.count("upscale_snapshot_status", record.get("status"))
            group = self.group(record.get("callback_call"))
            if group is None:
                return
            group["snapshot_observations"] += 1
            bindings = record.get("bindings")
            flags = ("snapshot_captured", "all_properties_present", "descriptors_unchanged",
                     "graph_entries_unchanged", "binding_metadata_unchanged", "context_bounds_unchanged")
            valid = record.get("source") == "upscaler_prepare_e7a7b0" and record.get("status") == "SnapshotParsed" and all(record.get(flag) is True for flag in flags)
            valid = valid and record.get("graph_texture_ids_read") == 8 and record.get("native_candidates_read") == 8
            valid = valid and isinstance(bindings, list) and len(bindings) == 8
            values = {}
            if valid:
                for binding in bindings:
                    if not isinstance(binding, dict):
                        valid = False
                        break
                    prop, image = binding.get("property"), handle(binding.get("image_candidate"))
                    if prop not in PROPERTIES or prop in values or image is None:
                        valid = False
                        break
                    values[prop] = image
            if valid and len(values) == 8:
                group["valid_snapshot_observations"] += 1
                if group["snapshot_images"] is not None and group["snapshot_images"] != values:
                    group["snapshot_conflict"] = True
                else:
                    group["snapshot_images"] = values
            else:
                self.sample("upscale_snapshot_refusal", record, line)
            return
        if event in ("upscale_image_metadata_match", "fsr_image_metadata_match"):
            upscale = event.startswith("upscale")
            prop = record.get("property")
            self.count("upscale_match_status" if upscale else "fi_match_status", record.get("match_status"))
            matched = record.get("numeric_live_handle_match") is True and record.get("match_status") == "live_handle_observed"
            identity = self.identity(record, "image_candidate") if matched else None
            if upscale:
                if prop not in PROPERTIES or record.get("source") != "upscaler_prepare_e7a7b0":
                    self.a.omitted["unknown_upscale_property_or_source"] += 1
                    return
                self.count("upscale_property_rows", prop)
                group = self.group(record.get("callback_call"))
                if group is not None:
                    reason = record.get("sample_reason")
                    reason = reason if reason in ("initial_detail", "periodic_refresh") else "absent_or_unrecognized"
                    group["metadata_sample_reasons"][reason] = group["metadata_sample_reasons"].get(reason, 0) + 1
                    observation = {"image": handle(record.get("image_candidate")), "generation": uint(record.get("image_generation"), True),
                                   "matched": matched and identity is not None, "extent_matches": record.get("candidate_extent_matches") is True}
                    old = group["properties"].get(prop)
                    if old is not None:
                        group["duplicate_metadata_rows"] += 1
                        if old != observation and prop not in group["conflicting_properties"]:
                            group["conflicting_properties"].append(prop)
                    else:
                        group["properties"][prop] = observation
                if identity is not None:
                    identity["upscale_properties"][prop] += 1
            else:
                role = FI_ROLES.get((record.get("pass"), prop)) if isinstance(record.get("pass"), str) and isinstance(prop, str) else None
                if identity is not None and role:
                    identity["fi_metadata_roles"][role] += 1
            if not matched:
                self.sample("numeric_identity_match_refused", record, line)
            return
        if event == "image_sync_candidate":
            role = record.get("role")
            self.count("fi_candidate_acceptance", "accepted" if record.get("accepted") is True else "not_accepted")
            expected_pass = "frame_inpainting" if role == "real_color" else "reconstruct_previous_depth"
            if role in ROLES and record.get("pass") == expected_pass and record.get("accepted") is True and uint(record.get("selectionGeneration"), True):
                identity = self.identity(record)
                if identity is not None:
                    identity["fi_accepted_roles"][role] += 1
            return
        if event.startswith("rp_projection_"):
            role = record.get("role")
            if role not in ROLES:
                self.a.omitted["unknown_projection_role"] += 1
                return
            if event == "rp_projection_submission":
                classification = text(record.get("classification"))
                self.count("projection_classifications", role + ":" + classification)
                bound = handle(record.get("image")) is not None and uint(record.get("image_generation"), True) and uint(record.get("selection_serial"), True)
                self.count("projection_sample_binding_state", "bound" if bound else "unbound_or_missing_identity")
                mask = uint(record.get("issues"))
                if mask is not None:
                    self.issue_union |= mask
                    for name in issue_names(mask):
                        self.count("projection_issue_record_counts", name)
                if classification == "refused":
                    self.sample("projection_refusal", record, line, decoded_issues=issue_names(mask))
                accepted = record.get("metadata_accepted") is True and type(record.get("driver_result")) is int and record["driver_result"] == 0 and mask == 0
                if accepted and classification in ("direct_attachment_layouts", "no_direct_attachment_reference"):
                    identity = self.identity(record)
                    if identity is not None:
                        identity["projection_roles"][role + ":" + classification] += 1
            else:
                self.count("projection_operation_source_kinds", record.get("source_kind"))
                identity = self.identity(record)
                if identity is not None:
                    # An operation event has no driver_result or ticket field:
                    # retain emitted metadata, do not invent a successful batch.
                    kind = record.get("source_kind")
                    if kind in ("initial_layout_assertion", "subpass_reference_transition", "final_layout_transition", "unused_initial_to_final_transition"):
                        identity["projection_source_kinds"][role + ":" + kind] += 1
                self.sample("projection_emitted_operation", record, line)
            return
        if event == "wsi_image_metadata":
            self.count("wsi_status", record.get("status"))
            self.count("wsi_inventory_status", record.get("inventory_status"))
            for field in ("added", "removed", "ownership_mismatches"):
                value = uint(record.get(field))
                if value is not None:
                    self.count("wsi_sampled_totals_not_global", field, value)
            images = record.get("images")
            if isinstance(images, list):
                if len(images) > 32:
                    self.a.omitted["wsi_array_over_limit"] += 1
                else:
                    for image in images:
                        if isinstance(image, dict) and image.get("origin") == "swapchain_enumeration":
                            identity = self.identity(image, generation_field="generation")
                            if identity is not None:
                                identity["wsi_identity_observations"] += 1
            if record.get("status") not in ("create_pending", "chain_observed", "count_observed", "images_registered", "images_repeated", "chain_destroyed", "stopped"):
                self.sample("wsi_status_detail", record, line)
            return
        if event == "rp_swapchain_image_observation":
            identity = self.identity(record)
            mask = uint(record.get("issues"))
            self.count("rp_wsi_outcome", "accepted_description" if mask == 0 else "refused_description")
            if identity is not None and mask is not None:
                identity["rp_wsi_issues_union"] |= mask
            if mask:
                self.sample("rp_wsi_refusal", record, line, decoded_issues=issue_names(mask))
            return
        if event == "rp_rejection_sample":
            self.count("rp_rejection_sample_sites", record.get("site"))
            self.sample("rp_rejection_detail", record, line, decoded_issues=issue_names(record.get("issues")))

    def finish(self):
        rows, overlaps = [], []
        for value in self.identities.values():
            value = dict(value)
            value["rp_wsi_decoded_issues"] = issue_names(value["rp_wsi_issues_union"])
            rows.append(value)
            if value["upscale_properties"] and (value["fi_accepted_roles"] or value["fi_metadata_roles"] or value["fi_summary_roles"]):
                if self.scope["start_observed"]:
                    overlaps.append({"image": value["image"], "image_generation": value["image_generation"],
                        "upscale_properties": list(value["upscale_properties"]),
                        "fi_accepted_roles": list(value["fi_accepted_roles"]), "fi_metadata_roles": list(value["fi_metadata_roles"]),
                        "fi_summary_roles": list(value["fi_summary_roles"]), "same_frame_verified": False,
                        "association": "historical_same_inventory_identity_only"})
        groups = []
        for group in self.groups.values():
            properties = group["properties"]
            complete = set(properties) == set(PROPERTIES) and all(value["matched"] for value in properties.values())
            verified = complete and group["valid_snapshot_observations"] > 0 and not group["snapshot_conflict"] and not group["conflicting_properties"]
            aligned = self.scope["start_observed"] and verified and all(group["snapshot_images"][p] == properties[p]["image"] for p in PROPERTIES)
            groups.append({**group, "eight_numeric_matches_observed": complete,
                "snapshot_and_eight_metadata_rows_agree": bool(aligned), "snapshot_atomic": False,
                "association_disabled_without_start": not self.scope["start_observed"],
                "all_candidate_extents_match": complete and all(value["extent_matches"] for value in properties.values()),
                "same_frame_verified": False, "missing_properties": [p for p in PROPERTIES if p not in properties]})
        return {"scope": {**self.scope, "destroy_observed": self.closed}, "observed_counts": self.counts,
            "summary_snapshots": self.summaries, "projection_sample_issue_union": self.issue_union,
            "projection_sample_issue_names": issue_names(self.issue_union), "historical_identities": rows,
            "historical_identity_overlaps": overlaps, "retained_upscale_callback_groups": groups,
            "bounded_samples": self.samples, "association_disabled_without_start": not self.scope["start_observed"]}


class Analyzer:
    def __init__(self, max_records=4096, max_samples=128):
        self.max_records, self.max_samples = max_records, max_samples
        self.records = self.samples = 0
        self.sample_kinds = Counter()
        self.omitted = Counter()
        self.contexts = []
        self.active = {}
        self.events = Counter()
        self.files = []

    def reserve(self, kind):
        if self.records >= self.max_records:
            self.omitted[kind + "_capacity"] += 1
            return False
        self.records += 1
        return True

    def observe(self, record, file_id, line):
        if not isinstance(record, dict):
            self.omitted["non_object_json"] += 1
            return
        event = record.get("event")
        if not isinstance(event, str):
            self.omitted["missing_event"] += 1
            return
        self.events[event if event in EVENTS else "other_events"] += 1
        if event not in EVENTS:
            return
        pid, device = uint(record.get("pid"), True), handle(record.get("device"))
        if pid is None or device is None:
            self.omitted["invalid_scope"] += 1
            return
        key = file_id, pid, device
        current = self.active.get(key)
        start = event == "image_sync_start"
        created = event == "create_device" and type(record.get("result")) is int and record["result"] == 0
        new = current is None or start or (created and (current.closed or current.created or not current.scope["start_observed"]))
        if current and current.closed and not (created or start):
            self.omitted["detail_after_device_destroy"] += 1
            return
        if new:
            if len(self.contexts) >= 64:
                self.omitted["contexts_capacity"] += 1
                if key in self.active:
                    self.active[key] = None  # Never resume an older context after overflow.
                return
            ordinal = 1 + (current.scope["epoch"] if current else 0)
            current = Context(self, file_id, pid, device, ordinal, line, "image_sync_start" if start else "create_device" if created else None)
            self.contexts.append(current)
            self.active[key] = current
        if created:
            current.created = True
            current.scope["create_device_line"] = line
        current.observe(event, record, line)
        if event == "destroy_device":
            current.closed = True

    def finish(self, limits):
        contexts = [context.finish() for context in self.contexts]
        truncated = any(file["truncated"] for file in self.files)
        return {"schema": "dual-input-scene-analysis-v1", "limits": limits, "inputs": self.files,
            "read_truncated": truncated, "analyzer_omitted_or_rejected": dict(self.omitted),
            "event_counts_in_read_prefix": dict(self.events), "contexts": contexts,
            "limitations": LIMITATIONS, "same_frame_verified": False, "undilated_verified": False,
            "gpu_completion_verified": False, "resource_lifetime_pinned": False, "copy_permitted": False,
            "xefg_integration_verified": False}


def analyze(paths, *, max_bytes=64*1024*1024, max_lines=250000, max_line_bytes=64*1024, max_records=4096, max_samples=128):
    if not 1 <= len(paths) <= 16:
        raise ValueError("Select 1..16 observer JSONL files; files are never correlated with each other")
    if any(type(value) is not int or value <= 0 for value in (max_bytes, max_lines, max_line_bytes, max_records, max_samples)):
        raise ValueError("All limits must be positive integers")
    analyzer = Analyzer(max_records, max_samples)
    remaining_bytes, remaining_lines = max_bytes, max_lines
    for file_id, selected in enumerate(paths, 1):
        path = Path(selected).resolve()
        if path.is_dir():
            path = path / "observer.jsonl"
        before = path.stat()
        result = {"file_id": file_id, "path": str(path), "size_at_open": before.st_size,
            "bytes_read": 0, "lines_read": 0, "parsed_events": 0, "malformed_lines": 0,
            "unterminated_lines": 0, "truncated": False, "truncation_reasons": []}
        digest = hashlib.sha256()
        with path.open("rb", buffering=0) as stream:
            ceiling = min(before.st_size, remaining_bytes)
            while result["bytes_read"] < ceiling and remaining_lines:
                data = stream.readline(min(max_line_bytes + 1, ceiling - result["bytes_read"]))
                if not data:
                    break
                result["bytes_read"] += len(data)
                remaining_bytes -= len(data)
                result["lines_read"] += 1
                remaining_lines -= 1
                digest.update(data)
                if len(data) > max_line_bytes:
                    result["truncation_reasons"].append("line_byte_limit")
                    break
                if not data.endswith(b"\n"):
                    result["unterminated_lines"] += 1
                    if result["bytes_read"] < before.st_size:
                        result["truncation_reasons"].append("byte_limit_inside_line")
                        break
                if not data.strip():
                    continue
                try:
                    decoded = data.decode("utf-8-sig" if result["lines_read"] == 1 else "utf-8")
                    record = json.loads(decoded, object_pairs_hook=unique_object,
                        parse_constant=lambda value: (_ for _ in ()).throw(ValueError("nonfinite JSON")))
                    analyzer.observe(record, file_id, result["lines_read"])
                    result["parsed_events"] += 1
                except (ValueError, UnicodeError, RecursionError):
                    result["malformed_lines"] += 1
            if result["bytes_read"] < before.st_size:
                if not remaining_lines:
                    result["truncation_reasons"].append("total_line_limit")
                if not remaining_bytes:
                    result["truncation_reasons"].append("total_byte_limit")
                if not result["truncation_reasons"]:
                    result["truncation_reasons"].append("short_read_or_line_limit")
                result["truncated"] = True
            if result["truncation_reasons"]:
                result["truncated"] = True
        after = path.stat()
        result["size_after_read"] = after.st_size
        result["file_changed_during_read"] = after.st_size != before.st_size or after.st_mtime_ns != before.st_mtime_ns
        result["read_prefix_sha256"] = digest.hexdigest()
        analyzer.files.append(result)
    return analyzer.finish({"total_bytes": max_bytes, "total_lines": max_lines, "line_bytes": max_line_bytes,
        "retained_identity_and_callback_keys": max_records, "samples": max_samples, "contexts": 64,
        "samples_per_kind": 24, "files": 16, "wsi_images_per_event": 32, "counter_keys_per_category": 64})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("logs", nargs="+", type=Path, help="Explicit JSONL files or directories containing observer.jsonl")
    parser.add_argument("--output", type=Path, help="Optional JSON report path, must not replace an input log")
    parser.add_argument("--max-bytes", type=int, default=64*1024*1024)
    parser.add_argument("--max-lines", type=int, default=250000)
    parser.add_argument("--max-line-bytes", type=int, default=64*1024)
    parser.add_argument("--max-records", type=int, default=4096)
    args = parser.parse_args()
    try:
        inputs = [(path / "observer.jsonl" if path.is_dir() else path).resolve() for path in args.logs]
        if args.output and args.output.resolve() in inputs:
            raise ValueError("Output must not replace an input log")
        result = analyze(inputs, max_bytes=args.max_bytes, max_lines=args.max_lines,
            max_line_bytes=args.max_line_bytes, max_records=args.max_records)
        serialized = json.dumps(result, ensure_ascii=False, indent=2) + "\n"
        if args.output:
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(serialized, encoding="utf-8")
            print(str(args.output.resolve()))
        else:
            print(serialized, end="")
        return 0
    except (OSError, ValueError) as error:
        print(str(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
