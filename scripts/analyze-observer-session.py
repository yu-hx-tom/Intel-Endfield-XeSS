#!/usr/bin/env python3
"""Summarize one explicitly selected observer session; never open game resources.

Only observer.jsonl, launch.json, smoke-result.json, and its fixed installer
alias result.json in SESSION are read.
The caller must select an output path outside SESSION (which may be protected).
This is metadata correlation, not a frame-generation or texture-content test.
"""

from __future__ import annotations

import argparse
from collections import Counter, defaultdict
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import re
import sys
import tempfile


LOG_LIMIT = 32 * 1024 * 1024
OPTIONAL_LIMIT = 256 * 1024
LINE_LIMIT = 256 * 1024
TARGET_TAGS = {0: "depth", 1: "motion_vectors", 2: "HUD_less_color", 53: "backbuffer"}
LIFECYCLES = {0: "only_valid_now", 1: "valid_until_present", 2: "valid_until_evaluate"}
FORMATS = {37: "R8G8B8A8_UNORM", 43: "R8G8B8A8_SRGB", 44: "B8G8R8A8_UNORM",
           50: "B8G8R8A8_SRGB", 83: "R16G16_SFLOAT", 97: "R16G16B16A16_SFLOAT",
           100: "R32_SFLOAT", 126: "D32_SFLOAT", 129: "D24_UNORM_S8_UINT", 130: "D32_SFLOAT_S8_UINT"}
IMAGE_FIELDS = ("format", "width", "height", "depth", "mip_levels", "array_layers",
                "samples", "usage", "flags", "tiling", "sharing_mode", "initial_layout")
DEVICE_FIELDS = ("result", "physical_device", "enabled_extensions", "queue_create_info_count", "feature_chain_types",
                 "compute_observer_opt_in", "compute_observer_active",
                 "original_enabled_extensions", "original_timeline_feature_struct_present", "original_timeline_semaphore_requested",
                 "bridge_device_opt_in", "bridge_device_plan_status", "bridge_device_plan_reason", "bridge_device_plan_applied",
                 "bridge_device_create_attempts", "bridge_device_added_extensions", "bridge_device_timeline_changed",
                 "bridge_device_synchronous_loader_payload", "bridge_device_query_status", "instance_api_version_requested",
                 "bridge_device_physical_api_version", "bridge_device_extension_query_result", "bridge_device_extension_query_complete",
                 "bridge_device_timeline_query_complete", "bridge_device_timeline_supported", "bridge_device_refused_structure_type",
                 "timeline_feature_struct_present", "timeline_semaphore_requested",
                 "synchronization2_feature_struct_present", "synchronization2_requested")
CONSTANT_FIELDS = ("call", "result", "readable", "version", "jitter", "motionScale", "depthInverted",
                   "cameraMotionIncluded", "motionVectorsJittered", "reset", "near", "far", "fov",
                   "cameraViewToClip", "clipToCameraView", "clipToPrevClip", "prevClipToClip")
KNOWN_EVENTS = frozenset(("create_instance", "destroy_instance", "enumerate_physical_devices",
    "physical_device_properties", "create_device", "destroy_device", "create_swapchain",
    "destroy_swapchain", "queue_present_sample", "create_image", "destroy_image",
    "image_create_log_limit", "image_destroy_log_limit", "create_win32_surface", "destroy_surface", "get_device_queue", "get_device_queue2", "swapchain_images",
    "queue_submit_sample", "queue_submit2_sample", "observer_event_too_large",
    "sl_observer_install", "sl_tags", "sl_constants", "sl_frame_token", "sl_observer_summary",
    "compute_observer_start", "compute_observer_summary", "compute_shader_metadata", "compute_pipeline", "compute_dispatch_sample",
    "fsr_observer_install", "fsr_observer_install_summary", "fsr_callback_summary", "fsr_prepare_input_candidate", "fsr_image_metadata_match",
    "fsr_reconstruct_previous_depth_candidate", "fsr_frame_inpainting_candidate", "fsr_unknown_property_set",
    "image_sync_start", "image_sync_candidate", "image_sync_submit_evidence", "image_sync_summary"))
COMPUTE_LIMITS = {"contexts": 128, "shaders": 8192, "pipelines": 16384, "dispatches": 32768}
COMPUTE_FLAGS = ("metadata_truncated", "unresolved_decoration_groups", "log_names_truncated",
                 "log_bindings_truncated", "read_limit_exceeded", "names_truncated", "bindings_truncated",
                 "entry_points_truncated", "string_truncated")
COMPUTE_COUNTERS = ("shader_create_calls", "compute_modules", "shader_bytes_inspected", "parse_refused",
                    "unsupported_pipeline_metadata", "dispatch_recording_calls", "unknown_pipeline_dispatches",
                    "state_limit_hits", "event_limit_hits", "trace_failures")
FSR_PASS_EVENTS = {"fsr_prepare_input_candidate": "prepare_input",
    "fsr_reconstruct_previous_depth_candidate": "reconstruct_previous_depth",
    "fsr_frame_inpainting_candidate": "frame_inpainting"}
FSR_EVENTS = frozenset(("fsr_observer_install", "fsr_observer_install_summary", "fsr_callback_summary",
    "fsr_image_metadata_match", "fsr_unknown_property_set", *FSR_PASS_EVENTS))
FSR_LIMITS = {"contexts": 128, "samples": 27648, "samplesPerContext": 216, "samplesPerPass": 72, "bindingsPerSample": 5,
              "imageMetadataMatches": 138240, "imageMetadataMatchesPerContext": 1080, "peNtDifferences": 16,
              "unknownPropertySets": 1024, "unknownPropertySetsPerContext": 8}
FSR_INSTALL_FIELDS = ("state", "requested", "device_creation_boundaries", "attempted", "modules_pinned",
    "hook_created", "enable_called", "enable_succeeded", "sampling_allowed", "stop_requested",
    "trampoline_verified_before_enable", "trampoline_flush_succeeded", "trampoline_flush_error",
    "post_enable_bytes_verified", "post_enable_protection_restored", "target_flush_succeeded", "target_flush_error",
    "target_rva", "body_rva", "hook_check", "hook_bytes_still_active", "target_protection_still_restored",
    "trace_failures", "installation_boundary", "all_threads_quiescent_verified", "hot_retry", "hook_lifetime")
FSR_COUNTERS = ("callback_calls", "prepare_property_matches", "samples", "other_property_combinations",
    "unreadable_payloads", "trace_failures", "emitted", "reconstruct_property_matches", "inpainting_property_matches",
    "unknown_property_combinations", "prepare_samples", "reconstruct_samples", "inpainting_samples",
    "prepare_resume_samples", "reconstruct_resume_samples", "inpainting_resume_samples", "unknown_property_samples",
    *(f"{p}_refresh_{s}" for p in ("prepare", "reconstruct", "inpainting")
      for s in ("attempts", "parsed", "native_candidates", "last_uptime_ms")))
FSR_SAMPLE_FIELDS = ("matched_call", "payload", "graph_context", "status", "graph_entry_count", "parser_read_calls",
    "parser_bytes_requested", "prefilter_read_calls", "prefilter_bytes_requested", "all_prepare_properties", "all_pass_properties",
    "context_bounds_unchanged", "sample_reason", "callback_gap_ms", "parsed_pass", "payload_extent")
FSR_BINDING_FIELDS = ("property", "property_address", "graph_handle", "graph_status", "entry_type", "texture_id",
    "native_status", "native_object_candidate", "image_candidate", "width_candidate", "height_candidate")
FSR_PROPERTIES = frozenset(("r_input_motion_vectors", "r_input_depth", "rw_reconstructed_depth_previous_frame",
                            "rw_dilated_motion_vectors", "rw_dilated_depth"))
FSR_PASS_PROPERTIES = {"prepare_input": FSR_PROPERTIES,
    "reconstruct_previous_depth": frozenset(("r_dilated_motion_vectors", "r_dilated_depth", "r_current_interpolation_source",
        "r_input_distortion_field", "rw_reconstructed_depth_interpolated_frame")),
    "frame_inpainting": frozenset(("r_optical_flow_scd", "r_inpainting_pyramid", "r_present_backbuffer",
        "r_current_interpolation_source", "rw_output"))}
FSR_IMAGE_MATCH_FIELDS = ("matched_call", "property", "image_candidate", "match_status", "inventory_active_count",
    "numeric_live_handle_match", "image_generation", "candidate_extent_matches") + IMAGE_FIELDS
FSR_IMAGE_MATCH_STATUSES = frozenset(("live_handle_observed", "not_observed", "capacity_exceeded", "duplicate_create",
    "generation_exhausted", "stopped", "device_or_inventory_unavailable", "device_session_retired", "inventory_lookup_failed"))
IMAGE_SYNC_EVENTS = frozenset(("image_sync_start", "image_sync_candidate", "image_sync_submit_evidence", "image_sync_summary"))
IMAGE_SYNC_ROLES = {"motion": "reconstruct_previous_depth", "depth": "reconstruct_previous_depth", "real_color": "frame_inpainting"}
IMAGE_SYNC_LIMITS = {"contexts": 128, "candidates": 32768, "candidatesPerContext": 256,
    "submissions": 98304, "submissionsPerContext": 768, "submissionsPerRole": 256, "currentRoles": 3}
IMAGE_SYNC_FALSE_FIELDS = ("complete_hook_coverage", "gpu_complete", "last_writer_known", "resource_lifetime_pinned",
    "copy_permitted", "game_frame_inputs_verified", "submitted_layout_known")
IMAGE_SYNC_COUNTERS = ("selection_changes", "candidate_details", "candidate_details_omitted", "successful_submissions",
    "failed_submissions", "recorded_barriers", "ignored_barriers", "retired_operations_skipped", "selected_images",
    "command_buffers", "overlapping_queue_submits", "trace_failures",
    *(f"{role}_submission_{field}" for role in IMAGE_SYNC_ROLES for field in ("details", "details_omitted")))
IMAGE_SYNC_SUBMIT_FIELDS = ("candidate_submission_count", "submission_serial", "command_generation", "queue_family",
    "old_layout", "new_layout", "issues", "recording_issues", "src_queue_family", "dst_queue_family", "src_stages",
    "dst_stages", "src_access", "dst_access", "aspect_mask", "base_mip", "level_count", "base_layer", "layer_count")
IMAGE_SYNC_CURRENT_FIELDS = ("candidate_submissions", "latest_barrier_submission", "command_generation", "queue_family",
    "latest_device_submission", "recording_issues", "raw_old_layout", "raw_new_layout", "issues")


def integer(value):
    return value if isinstance(value, int) and not isinstance(value, bool) else None


def process_id(value):
    number = integer(value)
    return number if number is not None and number > 0 else None


def handle(value):
    if not isinstance(value, str) or not re.fullmatch(r"0[xX][0-9a-fA-F]{1,16}", value):
        return None
    number = int(value, 16)
    return hex(number) if number else None


def pick(record, fields):
    return {key: record[key] for key in fields if key in record}


def stamp(record, line):
    return {"line": line, "uptimeMs": record.get("uptime_ms", record.get("uptimeMs")),
            "threadId": record.get("thread_id", record.get("threadId"))}


def decode_json(data):
    # Python's permissive NaN support would otherwise generate nonstandard JSON.
    def reject_constant(value):
        raise ValueError("non-finite JSON number: " + value)
    return json.loads(data.decode("utf-8-sig"), parse_constant=reject_constant)


def generation(value):
    number = integer(value)
    return number if number is not None and 0 < number <= 0xffffffffffffffff else None


class ComputeAnalysis:
    """Correlate explicit generations within one PID/device observer context.

    Module/pipeline destruction is not emitted by this observer. Generation links
    are historical metadata references, not assertions of current object lifetime.
    """
    def __init__(self, warn):
        self.warn = warn
        self.limits = dict(COMPUTE_LIMITS)
        self.contexts, self.shaders, self.pipelines, self.dispatches = [], [], [], []
        self.current = {}
        self.shader_index, self.pipeline_index = defaultdict(list), defaultdict(list)
        self.seen, self.dropped = Counter(), Counter()
        self.unscoped = 0

    def context(self, record, line, start=False):
        pid, device = process_id(record.get("pid")), handle(record.get("device"))
        if pid is None or device is None:
            self.unscoped += 1
            self.warn("compute_record_missing_explicit_pid_or_device", line)
            return None
        key = (pid, device)
        item = self.current.get(key)
        if start or item is None:
            if len(self.contexts) >= self.limits["contexts"]:
                self.dropped["contexts"] += 1
                self.warn("analyzer_compute_context_limit", line)
                # Never attribute records of an untracked new context to an old one.
                self.current[key] = None
                return None
            if item is not None:
                item["supersededAtLine"] = line
            item = {"id": len(self.contexts) + 1, "pid": pid, "device": device,
                    "firstObservedLine": line, "start": None, "supersededAtLine": None,
                    "deviceDestroyObservation": None, "summarySnapshotCount": 0,
                    "counterMaxima": {}, "lastSummary": None, "shaderByteBudgetExhaustedObserved": False,
                    "dispatchRecordingCallsLowerBound": 0}
            self.contexts.append(item)
            self.current[key] = item
            if not start:
                self.warn("compute_context_without_start_record", line)
        if start:
            item["start"] = {**stamp(record, line), **pick(record, ("shader_byte_budget", "metadata_only"))}
        return item

    def device_destroyed(self, record, line):
        context = self.current.get((process_id(record.get("pid")), handle(record.get("device"))))
        if context:
            # The runtime's final compute summary follows destroy_device.
            context["deviceDestroyObservation"] = stamp(record, line)

    def bounded_text(self, value, item, line):
        if not isinstance(value, str):
            return ""
        if len(value) > 1024:
            item["analyzerMetadataTruncated"] = True
            self.warn("analyzer_compute_text_limit", line)
            return ""  # Do not classify a misleading prefix as a complete name.
        return value

    def metadata_array(self, record, field, item, line):
        values = record.get(field, [])
        if not isinstance(values, list):
            self.warn("compute_metadata_array_invalid", line, field)
            return []
        if len(values) > 128:
            item["analyzerMetadataTruncated"] = True
            self.warn("analyzer_compute_array_limit", line, field)
        return values[:128]

    @staticmethod
    def hints(item):
        fsr, fidelity, resources = [], [], []
        sources = []
        for name in item["names"]:
            sources.append({"source": "names", **name})
        for binding in item["bindings"]:
            sources.append({"source": "bindings", **binding})
        sources += [{"source": "entry_points", "name": name} for name in item["entry_points"]]
        for evidence in sources:
            name = evidence.get("name", "")
            folded = name.casefold()
            if "fsr" in folded:
                fsr.append(evidence)
            if "fidelityfx" in folded or "ffx" in folded:
                fidelity.append(evidence)
            # Only object/descriptor names are resource hints. A debug OpString or
            # struct-member name is retained as such, never relabeled as an object.
            if (evidence["source"] == "bindings" or
                    (evidence["source"] == "names" and integer(evidence.get("kind")) == 0)):
                if any(token in folded for token in ("depth", "motion", "velocity", "optical", "flow",
                        "interpol", "reconstruct", "dilat", "disoccl", "color", "colour", "exposure", "hud")):
                    resources.append(evidence)
        item["fsrIdentifierHints"], item["fidelityFxIdentifierHints"], item["resourceNameHints"] = fsr, fidelity, resources

    def consume(self, record, line):
        event = record["event"]
        self.seen[event] += 1
        context = self.context(record, line, start=event == "compute_observer_start")
        if context is None or event == "compute_observer_start":
            return
        if event == "compute_observer_summary":
            context["summarySnapshotCount"] += 1
            context["lastSummary"] = {**stamp(record, line), **pick(record, COMPUTE_COUNTERS +
                ("shader_byte_budget_exhausted", "live_shaders", "live_pipelines", "live_command_buffers"))}
            for field in COMPUTE_COUNTERS:
                value = integer(record.get(field))
                if value is not None and value >= 0:
                    context["counterMaxima"][field] = max(value, context["counterMaxima"].get(field, 0))
            context["shaderByteBudgetExhaustedObserved"] |= record.get("shader_byte_budget_exhausted") is True
            context["dispatchRecordingCallsLowerBound"] = max(context["dispatchRecordingCallsLowerBound"],
                context["counterMaxima"].get("dispatch_recording_calls", 0))
            return
        if context["deviceDestroyObservation"] is not None:
            self.warn("compute_detail_after_device_destroy", line)
            self.dropped["details_after_device_destroy"] += 1
            return
        category = {"compute_shader_metadata": "shaders", "compute_pipeline": "pipelines",
                    "compute_dispatch_sample": "dispatches"}[event]
        output = getattr(self, category)
        if len(output) >= self.limits[category]:
            self.dropped[category] += 1
            self.warn("analyzer_compute_record_limit", line, category)
            return
        item = {"id": len(output) + 1, "contextId": context["id"], "pid": context["pid"],
                "device": context["device"], **stamp(record, line)}
        if category == "shaders":
            item.update(pick(record, ("shader", "shader_generation", "code_bytes", "fnv1a64_correlation_only",
                                     "parse_status") + COMPUTE_FLAGS))
            item["analyzerMetadataTruncated"] = False
            item["entry_points"] = [self.bounded_text(value, item, line)
                for value in self.metadata_array(record, "entry_points", item, line) if isinstance(value, str)]
            item["names"], item["bindings"] = [], []
            for field, fields in (("names", ("id", "kind", "member_index")), ("bindings", ("id", "set", "binding"))):
                for value in self.metadata_array(record, field, item, line):
                    if isinstance(value, dict):
                        item[field].append({**pick(value, fields), "name": self.bounded_text(value.get("name"), item, line)})
                    else:
                        self.warn("compute_metadata_member_invalid", line, field)
            self.hints(item)
            number = generation(record.get("shader_generation"))
            if number and handle(record.get("shader")):
                self.shader_index[(context["id"], number)].append(item["id"])
            else:
                self.warn("compute_shader_missing_identity", line)
        elif category == "pipelines":
            item.update(pick(record, ("pipeline", "pipeline_generation", "shader", "shader_generation", "pipeline_layout",
                                      "entry_point_truncated", "batch_result", "shader_metadata_available")))
            item["entry_point"] = self.bounded_text(record.get("entry_point"), item, line)
            number = generation(record.get("pipeline_generation"))
            if number and handle(record.get("pipeline")):
                self.pipeline_index[(context["id"], number)].append(item["id"])
            else:
                self.warn("compute_pipeline_missing_identity", line)
        else:
            item.update(pick(record, ("recording_call", "command_buffer", "recording_generation", "pipeline", "pipeline_generation",
                "shader_generation", "pipeline_dispatch_recordings", "kind", "groups_known", "groups",
                "dispatch_base_coordinates_captured", "indirect_buffer", "indirect_offset")))
            value = integer(record.get("recording_call"))
            if value is not None and value >= 0:
                context["dispatchRecordingCallsLowerBound"] = max(context["dispatchRecordingCallsLowerBound"], value)
        output.append(item)

    def finish(self):
        shader_associations, dispatch_associations = Counter(), Counter()
        shader_pipelines, shader_samples = defaultdict(list), defaultdict(list)
        for pipeline in self.pipelines:
            number = generation(pipeline.get("shader_generation"))
            candidates = self.shader_index.get((pipeline["contextId"], number), []) if number else []
            pipeline["shaderMetadataId"] = None
            pipeline["dispatchRecordingSampleIds"] = []
            pipeline["dispatchRecordingsLowerBound"] = 0
            if not number:
                association = "shader_generation_unavailable"
            elif not candidates:
                association = "shader_metadata_not_observed_in_context"
            elif len(candidates) != 1:
                association = "ambiguous_shader_generation"
            else:
                shader = self.shaders[candidates[0] - 1]
                if handle(pipeline.get("shader")) != handle(shader.get("shader")):
                    association = "shader_handle_mismatch"
                elif shader["line"] >= pipeline["line"]:
                    association = "shader_metadata_not_observed_before_pipeline"
                else:
                    association = "explicit_generation_and_handle"
                    pipeline["shaderMetadataId"] = shader["id"]
                    pipeline["entryPointPresentInLoggedShaderMetadata"] = pipeline.get("entry_point") in shader["entry_points"]
                    shader_pipelines[shader["id"]].append(pipeline["id"])
            pipeline["shaderAssociation"] = association
            shader_associations[association] += 1
        for sample in self.dispatches:
            number = generation(sample.get("pipeline_generation"))
            candidates = self.pipeline_index.get((sample["contextId"], number), []) if number else []
            sample["pipelineRecordId"] = sample["shaderMetadataId"] = None
            if not generation(sample.get("recording_generation")) or not handle(sample.get("command_buffer")):
                association = "command_recording_identity_unavailable"
            elif not number:
                association = "pipeline_generation_unavailable"
            elif not candidates:
                association = "pipeline_metadata_not_observed_in_context"
            elif len(candidates) != 1:
                association = "ambiguous_pipeline_generation"
            else:
                pipeline = self.pipelines[candidates[0] - 1]
                if handle(sample.get("pipeline")) != handle(pipeline.get("pipeline")):
                    association = "pipeline_handle_mismatch"
                elif pipeline["line"] >= sample["line"]:
                    association = "pipeline_metadata_not_observed_before_recording"
                elif integer(sample.get("shader_generation")) != integer(pipeline.get("shader_generation")):
                    association = "shader_generation_mismatch"
                else:
                    association = "explicit_generation_and_handle"
                    sample["pipelineRecordId"] = pipeline["id"]
                    sample["shaderMetadataId"] = pipeline["shaderMetadataId"]
                    pipeline["dispatchRecordingSampleIds"].append(sample["id"])
                    value = integer(sample.get("pipeline_dispatch_recordings"))
                    if value is not None and value >= 0:
                        pipeline["dispatchRecordingsLowerBound"] = max(pipeline["dispatchRecordingsLowerBound"], value)
                    if sample["shaderMetadataId"] is not None:
                        shader_samples[sample["shaderMetadataId"]].append(sample["id"])
            sample["pipelineAssociation"] = association
            dispatch_associations[association] += 1
        candidates = {"fsrIdentifierCandidates": [], "fidelityFxIdentifierCandidates": [], "resourceNameCandidates": []}
        for shader in self.shaders:
            for field, hints in (("fsrIdentifierCandidates", "fsrIdentifierHints"),
                                 ("fidelityFxIdentifierCandidates", "fidelityFxIdentifierHints"),
                                 ("resourceNameCandidates", "resourceNameHints")):
                if shader[hints]:
                    candidates[field].append({"shaderMetadataId": shader["id"], "contextId": shader["contextId"],
                        "pid": shader["pid"], "device": shader["device"], "shaderGeneration": shader.get("shader_generation"),
                        "hints": shader[hints], "pipelineRecordIds": shader_pipelines[shader["id"]],
                        "dispatchRecordingSampleIds": shader_samples[shader["id"]],
                        "metadataLimits": pick(shader, COMPUTE_FLAGS + ("analyzerMetadataTruncated",))})
        return {"status": "metadata_only" if self.seen else "no_compute_observer_records",
                "gpuExecutionVerified": False, "resourceSemanticsVerified": False, "gameXeFGInputsVerified": False,
                "contexts": self.contexts, "shaders": self.shaders, "pipelines": self.pipelines,
                "dispatchRecordingSamples": self.dispatches, **candidates,
                "associationCounts": {"pipelineToShader": dict(shader_associations), "dispatchToPipeline": dict(dispatch_associations)},
                "observedEventCounts": dict(self.seen), "unscopedRecords": self.unscoped,
                "analyzerLimits": self.limits, "analyzerOmittedRecords": dict(self.dropped),
                "dispatchRecordingCallsLowerBound": sum(c["dispatchRecordingCallsLowerBound"] for c in self.contexts),
                "meaning": "Explicit PID/device/context generation metadata and command-recording samples only. FSR identifier hints are textual candidates; resource names alone do not identify FSR or verify depth/motion. A recording can be reset, never submitted, or submitted repeatedly. Shader binaries, descriptor contents and GPU execution are not observed."}


class FsrCallbackAnalysis:
    """Bounded callback evidence; numeric image links never verify the ID domain."""
    def __init__(self, warn, associate_image):
        self.warn, self.associate_image = warn, associate_image
        self.contexts, self.by_key, self.samples = [], {}, []
        self.events, self.dropped, self.associations = Counter(), Counter(), Counter()
        self.statuses, self.properties, self.native_statuses, self.graph_statuses = Counter(), Counter(), Counter(), Counter()
        self.limits = dict(FSR_LIMITS)
        self.unscoped = 0
        self.image_ids = set()
        self.metadata_matches = []
        self.metadata_statuses, self.metadata_associations, self.pe_states = Counter(), Counter(), Counter()
        self.unknown_sets = []

    def pass_identity(self, record, line, expected=None):
        explicit = "pass" in record
        value = record.get("pass", "prepare_input" if expected in (None, "prepare_input") else None)
        valid = isinstance(value, str) and value in FSR_PASS_PROPERTIES and (expected is None or value == expected)
        if not valid:
            self.warn("fsr_pass_identity_invalid", line)
        return {"pass": value if valid else "unknown", "passIdentityExplicit": explicit,
                "passIdentityValid": valid}

    @staticmethod
    def eligible(sample):
        complete = (sample.get("all_pass_properties") is True if sample.get("passIdentityExplicit") else
                    sample.get("all_prepare_properties") is True)
        return (sample.get("passIdentityValid") is True and sample.get("status") == "SnapshotParsed"
                and sample.get("context_bounds_unchanged") is True and complete
                and ("parsed_pass" not in sample or sample["parsed_pass"] == sample["pass"]))

    def constants(self, raw, line):
        if not isinstance(raw, dict):
            self.warn("fsr_constants_invalid", line)
            return None
        item = self.fields(raw, ("status", "descriptor", "buffer_id", "byte_offset", "byte_size", "unknown_bc", "cpu_pointer",
            "descriptor_read", "descriptor_unchanged", "graph_bounds_unchanged", "captured"), line)
        item.update(snapshotAtomicVerified=False, resourceLifetimePinned=False, shaderSemanticsVerified=False)
        bits = raw.get("raw_u32_bits")
        eligible = (raw.get("status") == "ConstantsSnapshotReadUnverified" and raw.get("captured") is True
            and integer(raw.get("byte_size")) == 224 and raw.get("descriptor_read") is True
            and raw.get("descriptor_unchanged") is True and raw.get("graph_bounds_unchanged") is True)
        valid_bits = isinstance(bits, list) and len(bits) == 56 and all(integer(v) is not None and 0 <= v <= 0xffffffff for v in bits)
        item["rawSnapshotRetained"] = bool(eligible and valid_bits)
        if item["rawSnapshotRetained"]:
            item["raw_u32_bits"] = list(bits)
        elif bits is not None or raw.get("captured") is True:
            self.warn("fsr_constants_raw_snapshot_invalid_or_inconsistent", line)
        return item

    def unknown_properties(self, record, line, context):
        if (len(self.unknown_sets) >= self.limits["unknownPropertySets"] or
                context["retainedUnknownPropertySetCount"] >= self.limits["unknownPropertySetsPerContext"]):
            self.dropped["unknownPropertySets"] += 1
            self.warn("analyzer_fsr_unknown_property_set_limit", line)
            return
        item = {"contextId": context["id"], **self.stamp(record, line),
                **self.fields(record, ("unknown_call", "payload", "graph_context"), line), "properties": []}
        values = record.get("properties")
        if not isinstance(values, list) or len(values) != 5:
            self.warn("fsr_unknown_property_set_invalid", line)
        elif all(isinstance(v, dict) for v in values):
            item["properties"] = [self.fields(v, ("slot", "address", "rva", "property"), line) for v in values]
        else:
            self.warn("fsr_unknown_property_set_invalid", line)
        self.unknown_sets.append(item)
        context["retainedUnknownPropertySetCount"] += 1

    @staticmethod
    def stamp(record, line):
        raw = stamp(record, line)
        return {key: value if integer(value) is not None and 0 <= value <= 0xffffffffffffffff else None
                for key, value in raw.items()}

    def fields(self, record, fields, line):
        result = {}
        for name in fields:
            if name not in record:
                continue
            value = record[name]
            if value is None or isinstance(value, bool) or (integer(value) is not None and abs(value) <= 0xffffffffffffffff):
                result[name] = value
            elif isinstance(value, str) and len(value) <= 256:
                result[name] = value
            else:
                self.dropped["invalidOrOversizedFields"] += 1
                self.warn("fsr_callback_field_invalid_or_oversized", line, name)
                result["analyzerFieldsOmitted"] = True
        return result

    def context(self, record, line):
        pid, device = process_id(record.get("pid")), handle(record.get("device"))
        if pid is None or device is None:
            self.unscoped += 1
            self.warn("fsr_callback_missing_explicit_pid_or_device", line)
            return None
        key = (pid, device)
        if key not in self.by_key:
            if len(self.contexts) >= self.limits["contexts"]:
                self.dropped["contexts"] += 1
                self.warn("analyzer_fsr_context_limit", line)
                return None
            context = {"id": len(self.contexts) + 1, "pid": pid, "device": device, "firstObservedLine": line,
                "deviceDestroyLine": None, "installationEventCount": 0, "installationSummaryCount": 0,
                "firstInstallationRecord": None, "lastInstallationRecord": None, "callbackSummaryCount": 0,
                "lastCallbackSummary": None, "counterMaxima": {}, "candidateRecordsObserved": 0,
                "retainedSampleCount": 0, "sampleIds": [], "imageMetadataMatchRecordsObserved": 0,
                "retainedImageMetadataMatchCount": 0, "imageMetadataMatchIds": [],
                "retainedSamplesByPass": {}, "retainedUnknownPropertySetCount": 0}
            self.contexts.append(context)
            self.by_key[key] = context
        return self.by_key[key]

    def device_destroyed(self, record, line):
        context = self.by_key.get((process_id(record.get("pid")), handle(record.get("device"))))
        if context is not None and context["deviceDestroyLine"] is None:
            context["deviceDestroyLine"] = line

    @staticmethod
    def counter_name(value):
        return value if isinstance(value, str) and 0 < len(value) <= 128 else "missing_or_invalid"

    def pe_guard(self, raw, line):
        if not isinstance(raw, dict):
            self.warn("fsr_pe_guard_invalid", line)
            return None
        item = self.fields(raw, ("attempted", "passed", "state", "source", "stage", "reason", "observed",
            "expected_or_limit", "invalid_section_index", "comparison_performed", "nt_different_bytes",
            "section_counts_match", "section_different_bytes", "different_sections", "nt_difference_limit",
            "actual_module_base", "live_image_base_matches_actual_module_base", "nt_differences_outside_image_base",
            "image_base_normalization_eligible", "image_base_normalization_applied",
            "additional_memory_reads_for_diff"), line)
        nested = {
            "failed_read": ("operation", "api_called", "api_succeeded", "win32_error", "offset", "requested_bytes", "actual_bytes"),
            "disk_reads": ("calls", "requested_bytes", "actual_bytes"),
            "live_reads": ("calls", "requested_bytes", "actual_bytes"),
            "disk": ("dos_read", "nt_read", "sections_read", "dos_magic", "nt_offset", "nt_signature", "machine",
                     "section_count", "optional_header_size", "optional_magic", "image_size", "headers_size", "image_base"),
            "live": ("dos_read", "nt_read", "sections_read", "dos_magic", "nt_offset", "nt_signature", "machine",
                     "section_count", "optional_header_size", "optional_magic", "image_size", "headers_size", "image_base"),
            "first_section_difference": ("index", "section_offset", "field", "disk_byte", "live_byte"),
        }
        for name, fields in nested.items():
            if name not in raw:
                continue
            if raw[name] is None:
                item[name] = None
            elif isinstance(raw[name], dict):
                item[name] = self.fields(raw[name], fields, line)
            else:
                self.warn("fsr_pe_guard_nested_invalid", line, name)
        differences = raw.get("nt_first_differences", [])
        if not isinstance(differences, list):
            self.warn("fsr_pe_guard_differences_invalid", line)
            differences = []
        cap = self.limits["peNtDifferences"]
        if len(differences) > cap:
            self.warn("analyzer_fsr_pe_difference_limit", line)
            item["analyzerDifferencesTruncated"] = True
            self.dropped["peNtDifferences"] += len(differences) - cap
        item["nt_first_differences"] = []
        for difference in differences[:cap]:
            if isinstance(difference, dict):
                item["nt_first_differences"].append(self.fields(difference,
                    ("nt_offset", "field", "directory_index", "disk_byte", "live_byte"), line))
            else:
                self.warn("fsr_pe_guard_difference_invalid", line)
        self.pe_states[self.counter_name(item.get("state"))] += 1
        return item

    def image_metadata_match(self, record, line, context):
        context["imageMetadataMatchRecordsObserved"] += 1
        if (len(self.metadata_matches) >= self.limits["imageMetadataMatches"] or
                context["retainedImageMetadataMatchCount"] >= self.limits["imageMetadataMatchesPerContext"]):
            self.dropped["imageMetadataMatches"] += 1
            self.warn("analyzer_fsr_image_metadata_match_limit", line)
            return
        item = {"id": len(self.metadata_matches) + 1, "contextId": context["id"], "pid": context["pid"], "device": context["device"],
                **self.stamp(record, line), **self.fields(record, FSR_IMAGE_MATCH_FIELDS, line),
                **self.pass_identity(record, line),
                "resourceLifetimePinned": False, "nativeIdDomainVerified": False, "vulkanImageVerified": False,
                "pixelSemanticsVerified": False, "synchronizationVerified": False, "gameFrameInputsVerified": False}
        status = item.get("match_status")
        self.metadata_statuses[status if status in FSR_IMAGE_MATCH_STATUSES else "unknown_or_invalid"] += 1
        valid_description = all(integer(item.get(key)) is not None and item[key] > 0
                                for key in ("width", "height", "depth", "mip_levels", "array_layers", "samples"))
        valid_description &= all(integer(item.get(key)) is not None and item[key] >= 0 for key in ("format", "usage"))
        item["inventoryNumericMatchObserved"] = bool(item.get("numeric_live_handle_match") is True
            and status == "live_handle_observed" and generation(item.get("image_generation")) is not None
            and handle(item.get("image_candidate")) is not None and valid_description and context["deviceDestroyLine"] is None)
        if item.get("numeric_live_handle_match") is True and not item["inventoryNumericMatchObserved"]:
            self.warn("fsr_numeric_inventory_match_inconsistent_or_after_destroy", line)
        self.metadata_matches.append(item)
        context["retainedImageMetadataMatchCount"] += 1
        context["imageMetadataMatchIds"].append(item["id"])

    def consume(self, record, line):
        event = record["event"]
        self.events[event] += 1
        context = self.context(record, line)
        if context is None:
            return
        if event in ("fsr_observer_install", "fsr_observer_install_summary"):
            key = "installationEventCount" if event == "fsr_observer_install" else "installationSummaryCount"
            context[key] += 1
            item = {"event": event, **self.stamp(record, line), **self.fields(record, FSR_INSTALL_FIELDS, line)}
            for field in ("mh_initialize", "mh_create", "mh_enable"):
                if field in record:
                    status = record[field]
                    if status is None:
                        item[field] = None
                    elif isinstance(status, dict):
                        item[field] = self.fields(status, ("code", "name"), line)
                    else:
                        self.warn("fsr_minhook_status_invalid", line, field)
            if "pe_guard" in record:
                item["pe_guard"] = self.pe_guard(record["pe_guard"], line)
            if context["firstInstallationRecord"] is None:
                context["firstInstallationRecord"] = item
            context["lastInstallationRecord"] = item
            return
        if event == "fsr_image_metadata_match":
            self.image_metadata_match(record, line, context)
            return
        if event == "fsr_unknown_property_set":
            self.unknown_properties(record, line, context)
            return
        if event == "fsr_callback_summary":
            context["callbackSummaryCount"] += 1
            context["lastCallbackSummary"] = {**self.stamp(record, line), **self.fields(record, ("sampling_enabled",) + FSR_COUNTERS, line)}
            for name in FSR_COUNTERS:
                value = integer(record.get(name))
                if value is not None and 0 <= value <= 0xffffffffffffffff:
                    context["counterMaxima"][name] = max(value, context["counterMaxima"].get(name, 0))
            return
        context["candidateRecordsObserved"] += 1
        identity = self.pass_identity(record, line, FSR_PASS_EVENTS[event])
        pass_count = context["retainedSamplesByPass"].get(identity["pass"], 0)
        # Three independently bounded pass streams; counters are not frame IDs.
        if (len(self.samples) >= self.limits["samples"] or context["retainedSampleCount"] >= self.limits["samplesPerContext"]
                or pass_count >= self.limits["samplesPerPass"]):
            self.dropped["candidateSamples"] += 1
            self.warn("analyzer_fsr_sample_limit", line)
            return
        sample = {"id": len(self.samples) + 1, "contextId": context["id"], "pid": context["pid"], "device": context["device"],
            **self.stamp(record, line), **self.fields(record, FSR_SAMPLE_FIELDS, line), **identity, "bindings": [],
            "snapshotAtomicVerified": False, "nativeIdDomainVerified": False, "vulkanImageVerified": False}
        if "constants_fi" in record:
            sample["constants_fi"] = self.constants(record["constants_fi"], line)
        self.statuses[self.counter_name(sample.get("status"))] += 1
        bindings = record.get("bindings")
        if not isinstance(bindings, list):
            bindings = []
            sample["analyzerBindingsInvalid"] = True
            self.warn("fsr_callback_bindings_invalid", line)
        if len(bindings) > self.limits["bindingsPerSample"]:
            sample["analyzerBindingsTruncated"] = True
            self.dropped["bindings"] += len(bindings) - self.limits["bindingsPerSample"]
            self.warn("analyzer_fsr_binding_limit", line)
        for index, binding in enumerate(bindings[:self.limits["bindingsPerSample"]]):
            if not isinstance(binding, dict):
                self.dropped["invalidBindings"] += 1
                self.warn("fsr_callback_binding_invalid", line)
                continue
            item = {"index": index, **self.fields(binding, FSR_BINDING_FIELDS, line),
                    "nativeIdDomainVerified": False, "vulkanImageVerified": False}
            property_name = item.get("property")
            item["recognizedPreparePropertyName"] = property_name in FSR_PROPERTIES
            item["recognizedPassPropertyName"] = property_name in FSR_PASS_PROPERTIES.get(identity["pass"], ())
            self.properties[property_name if item["recognizedPassPropertyName"] else "other_or_missing"] += 1
            self.native_statuses[self.counter_name(item.get("native_status"))] += 1
            self.graph_statuses[self.counter_name(item.get("graph_status"))] += 1
            if context["deviceDestroyLine"] is not None:
                association = {"association": "callback_context_device_destroyed", "imageId": None}
            elif not self.eligible(sample):
                association = {"association": "snapshot_not_eligible_for_numeric_association", "imageId": None}
            elif not item["recognizedPassPropertyName"] or item.get("graph_status") != "GraphTextureIdRead" or item.get("native_status") != "NativeCandidateReadUnverified":
                association = {"association": "native_candidate_not_read_or_property_unrecognized", "imageId": None}
            else:
                association = self.associate_image(context["pid"], context["device"], item, line)
            item.update(association)
            self.associations[item["association"]] += 1
            if item["imageId"] is not None:
                self.image_ids.add(item["imageId"])
            sample["bindings"].append(item)
        self.samples.append(sample)
        context["retainedSampleCount"] += 1
        context["retainedSamplesByPass"][identity["pass"]] = pass_count + 1
        context["sampleIds"].append(sample["id"])

    def finish(self, images):
        sample_index = defaultdict(list)
        for sample in self.samples:
            call = generation(sample.get("matched_call"))
            if call is not None:
                sample_index[(sample["contextId"], sample["pass"], sample["passIdentityExplicit"], call)].append(sample)
        for item in self.metadata_matches:
            item["candidateSampleId"], item["candidateBindingIndex"] = None, None
            candidates = sample_index.get((item["contextId"], item["pass"], item["passIdentityExplicit"], generation(item.get("matched_call"))), [])
            if not candidates:
                association = "candidate_sample_not_observed_in_context"
            elif len(candidates) != 1:
                association = "ambiguous_matched_call_in_context"
            else:
                sample = candidates[0]
                if not item["passIdentityValid"] or not self.eligible(sample):
                    association = "candidate_snapshot_not_eligible"
                else:
                    bindings = [b for b in sample["bindings"] if b.get("property") == item.get("property")
                        and b.get("recognizedPassPropertyName") is True
                        and handle(b.get("image_candidate")) is not None
                        and handle(b.get("image_candidate")) == handle(item.get("image_candidate"))
                        and b.get("graph_status") == "GraphTextureIdRead" and b.get("native_status") == "NativeCandidateReadUnverified"]
                    if len(bindings) != 1:
                        association = "candidate_property_or_handle_ambiguous_or_unmatched"
                    else:
                        association = "explicit_context_matched_call_property_and_numeric_handle"
                        item["candidateSampleId"], item["candidateBindingIndex"] = sample["id"], bindings[0]["index"]
                        bindings[0].setdefault("imageMetadataMatchIds", []).append(item["id"])
            item["candidateAssociation"] = association
            self.metadata_associations[association] += 1
        return {"status": "metadata_candidates_only" if self.samples else "inventory_metadata_only" if self.metadata_matches else "installation_or_summary_only" if self.events else "no_fsr_callback_records",
            "contexts": self.contexts, "candidateSamples": self.samples, "observedEventCounts": dict(self.events),
            "unknownPropertySets": self.unknown_sets,
            "readStatusCountsInRetainedSamples": dict(self.statuses), "propertyCountsInRetainedSamples": dict(self.properties),
            "nativeStatusCountsInRetainedSamples": dict(self.native_statuses), "graphStatusCountsInRetainedSamples": dict(self.graph_statuses),
            "associationCounts": dict(self.associations), "unscopedRecords": self.unscoped,
            "peGuardStateCountsInObservedRecords": dict(self.pe_states),
            "imageMetadataMatches": self.metadata_matches,
            "imageMetadataMatchStatusCountsInRetainedRecords": dict(self.metadata_statuses),
            "imageMetadataMatchAssociationCounts": dict(self.metadata_associations),
            "inventoryNumericMatchesObserved": sum(item["inventoryNumericMatchObserved"] for item in self.metadata_matches),
            "analyzerLimits": self.limits, "analyzerOmittedRecords": dict(self.dropped),
            "referencedImageLifetimes": [pick(image, ("id", "pid", "deviceId", "device", "image", "generation", "source",
                "beginLine", "endLine", "endReason", "swapchainId", "metadata")) for image in images if image["id"] in self.image_ids],
            "snapshotAtomicVerified": False, "nativeIdDomainVerified": False, "vulkanImageVerified": False,
            "resourceSemanticsVerified": False, "gameFrameInputsVerified": False, "pixelsRead": False, "pixelContentVerified": False,
            "xeFGIntegrated": False, "generatedFramesVerified": False,
            "samplingMeaning": "Each recognized pass has at most 64 regular plus 8 resume-gap samples. Three passes total at most 216; legacy observers sampled only Prepare. Callback counts, gap observations and matched_call are not frame identities or FG-state proof. Explicit pass identity participates in sample association; legacy records only associate with legacy records.",
            "associationMeaning": "Numeric handle equality within the same explicit PID/device and observed log lifetime; neither Unity native-ID domain nor live VkImage type or GPU use is proven.",
            "inventoryMatchMeaning": "Runtime inventory snapshots are separate point-in-time numeric handle/creation-metadata observations. They may exist without sampled create_image records. Inventory generation is not the analyzer's log-lifetime generation; a match does not pin resources or establish semantic identity, GPU completion, layout or validity at later use."}


class ImageSyncAnalysis:
    """Bounded, explicit selection metadata; never a GPU/layout/frame proof.

    Candidate notices are logged after releasing the lifetime lock and may be
    interleaved with submissions. Exact identities can link either log order;
    order is reported explicitly and does not establish runtime precedence.
    """
    def __init__(self, warn):
        self.warn = warn
        self.limits = dict(IMAGE_SYNC_LIMITS)
        self.contexts, self.current, self.candidates, self.submissions = [], {}, [], []
        self.events, self.dropped = Counter(), Counter()
        self.unscoped = 0

    def fields(self, record, line, numbers=(), booleans=(), handles=()):
        result = {}
        for name in numbers + booleans + handles:
            if name not in record:
                continue
            value = record[name]
            valid = (integer(value) is not None and 0 <= value <= 0xffffffffffffffff) if name in numbers else (
                isinstance(value, bool) if name in booleans else
                isinstance(value, str) and re.fullmatch(r"0[xX][0-9a-fA-F]{1,16}", value) is not None)
            if valid:
                result[name] = hex(int(value, 16)) if name in handles else value
            else:
                self.dropped["invalidFields"] += 1
                self.warn("image_sync_field_invalid", line, name)
        return result

    def claims(self, record, line):
        for name in IMAGE_SYNC_FALSE_FIELDS:
            if name in record and record[name] is not False:
                self.warn("image_sync_unsupported_verification_claim", line, name)
        return dict.fromkeys(IMAGE_SYNC_FALSE_FIELDS, False)

    def context(self, record, line):
        pid, device = process_id(record.get("pid")), handle(record.get("device"))
        if pid is None or device is None:
            self.unscoped += 1
            self.warn("image_sync_missing_explicit_pid_or_device", line)
            return None
        key = (pid, device)
        context = self.current.get(key)
        start = record["event"] == "image_sync_start"
        if start or context is None:
            if len(self.contexts) >= self.limits["contexts"]:
                # A new start cannot accidentally continue the old generation.
                self.current.pop(key, None)
                self.dropped["contextRecords"] += 1
                self.warn("analyzer_image_sync_context_limit", line)
                return None
            if context is not None:
                context["supersededAtLine"] = line
            context = {"id": len(self.contexts) + 1, "pid": pid, "device": device,
                "firstObservedLine": line, "start": None, "supersededAtLine": None, "deviceDestroyLine": None,
                "summarySnapshotCount": 0, "lastSummary": None, "counterMaxima": {}, "observedIssueMaskOR": 0,
                "candidateRecordsObserved": 0, "submissionRecordsObserved": 0,
                "retainedCandidateCount": 0, "retainedSubmissionCount": 0, "retainedSubmissionsByRole": {}}
            self.contexts.append(context)
            self.current[key] = context
            if not start:
                self.warn("image_sync_context_without_start_record", line)
        return context

    def device_destroyed(self, record, line):
        context = self.current.get((process_id(record.get("pid")), handle(record.get("device"))))
        if context:
            context["deviceDestroyLine"] = line

    def identity(self, record, line):
        role, expected_pass = record.get("role"), record.get("pass")
        valid_role = isinstance(role, str) and role in IMAGE_SYNC_ROLES and expected_pass == IMAGE_SYNC_ROLES[role]
        if not valid_role:
            self.warn("image_sync_role_or_pass_invalid", line)
        item = {"role": role if valid_role else "unknown", "pass": expected_pass if valid_role else "unknown",
                "roleIdentityValid": valid_role,
                **self.fields(record, line, ("selectionGeneration", "image_generation"), handles=("image",))}
        item["selectionIdentityValid"] = bool(valid_role and generation(item.get("selectionGeneration"))
            and generation(item.get("image_generation")) and handle(item.get("image")))
        return item

    def current_roles(self, record, line, context):
        roles = record.get("current_roles", [])
        if not isinstance(roles, list):
            self.warn("image_sync_current_roles_invalid", line)
            return []
        if len(roles) > self.limits["currentRoles"]:
            self.dropped["currentRoles"] += len(roles) - self.limits["currentRoles"]
            self.warn("analyzer_image_sync_current_roles_limit", line)
        result = []
        seen = set()
        for raw in roles[:self.limits["currentRoles"]]:
            if not isinstance(raw, dict):
                self.warn("image_sync_current_role_invalid", line)
                continue
            item = {"contextId": context["id"], "pid": context["pid"], "device": context["device"],
                **FsrCallbackAnalysis.stamp(record, line), **self.identity(raw, line),
                **self.fields(raw, line, IMAGE_SYNC_CURRENT_FIELDS, ("barrier_submitted", "selected_before_begin"), ("queue",)),
                **self.claims(raw, line)}
            if item["role"] in seen:
                item["selectionIdentityValid"] = False
                self.warn("image_sync_current_role_duplicate", line)
                for previous in result:
                    if previous["role"] == item["role"]:
                        previous["selectionIdentityValid"] = False
            seen.add(item["role"])
            result.append(item)
        return result

    def consume(self, record, line):
        event = record["event"]
        self.events[event] += 1
        context = self.context(record, line)
        if context is None:
            return
        common = {"contextId": context["id"], "pid": context["pid"], "device": context["device"],
                  **FsrCallbackAnalysis.stamp(record, line), **self.claims(record, line)}
        if event == "image_sync_start":
            context["start"] = {**common, **self.fields(record, line,
                ("tracker_storage_bytes", "helper_storage_bytes", "candidate_detail_limit", "submission_detail_limit_per_role"),
                ("enabled_by_fsr_observer",))}
            return
        if event == "image_sync_summary":
            context["summarySnapshotCount"] += 1
            item = {**common, **self.fields(record, line, IMAGE_SYNC_COUNTERS + ("issues",), ("retired",)),
                    "current_roles": self.current_roles(record, line, context)}
            context["lastSummary"] = item
            for name in IMAGE_SYNC_COUNTERS:
                value = item.get(name)
                if integer(value) is not None:
                    context["counterMaxima"][name] = max(value, context["counterMaxima"].get(name, 0))
            context["observedIssueMaskOR"] |= item.get("issues", 0)
            for role in item["current_roles"]:
                context["observedIssueMaskOR"] |= role.get("issues", 0)
            return
        candidate = event == "image_sync_candidate"
        context["candidateRecordsObserved" if candidate else "submissionRecordsObserved"] += 1
        if context["deviceDestroyLine"] is not None:
            self.dropped["detailAfterDeviceDestroy"] += 1
            self.warn("image_sync_detail_after_device_destroy", line)
            return
        category = "candidates" if candidate else "submissions"
        retained = self.candidates if candidate else self.submissions
        count_field = "retainedCandidateCount" if candidate else "retainedSubmissionCount"
        if (len(retained) >= self.limits[category] or context[count_field] >= self.limits[category + "PerContext"]):
            self.dropped[category] += 1
            self.warn("analyzer_image_sync_record_limit", line, category)
            return
        item = {"id": len(retained) + 1, **common, **self.identity(record, line)}
        if candidate:
            item.update(self.fields(record, line, ("previous_selection_generation", "format", "width", "height"), ("accepted",)))
        else:
            role = item["role"]
            if context["retainedSubmissionsByRole"].get(role, 0) >= self.limits["submissionsPerRole"]:
                self.dropped[category] += 1
                self.warn("analyzer_image_sync_role_limit", line, role)
                return
            item.update(self.fields(record, line, IMAGE_SYNC_SUBMIT_FIELDS,
                ("selection_is_current", "layout_fields_ignored", "selected_before_begin"), ("command", "queue", "fence")))
            context["retainedSubmissionsByRole"][role] = context["retainedSubmissionsByRole"].get(role, 0) + 1
            context["observedIssueMaskOR"] |= item.get("issues", 0)
        retained.append(item)
        context[count_field] += 1

    def finish(self):
        index = defaultdict(list)
        for item in self.candidates:
            if item["selectionIdentityValid"]:
                index[(item["contextId"], item["role"], item["selectionGeneration"])].append(item)
        association_counts = Counter()
        def associate(item):
            item["candidateRecordId"] = None
            item["candidateLogOrder"] = None
            context = self.contexts[item["contextId"] - 1]
            matches = index.get((item["contextId"], item["role"], item.get("selectionGeneration")), [])
            if not item["selectionIdentityValid"]:
                status = "no_valid_selection_identity"
            elif context["start"] is None:
                status = "context_start_not_observed"
            elif not matches:
                status = "no_retained_candidate_record"
            elif len(matches) != 1:
                status = "ambiguous_selection_generation"
            elif (matches[0].get("image") != item.get("image") or
                    matches[0].get("image_generation") != item.get("image_generation")):
                status = "image_identity_mismatch"
            elif matches[0].get("accepted") is not True:
                status = "candidate_not_accepted"
            else:
                status = "matching_selection_metadata"
                item["candidateRecordId"] = matches[0]["id"]
                item["candidateLogOrder"] = "preceding" if matches[0]["line"] < item["line"] else "following"
            item["candidateAssociation"] = status
            return status
        for item in self.submissions:
            association_counts[associate(item)] += 1
        for context in self.contexts:
            if context["lastSummary"]:
                for item in context["lastSummary"]["current_roles"]:
                    associate(item)
        return {"status": "metadata_only" if self.events else "no_image_sync_records",
            "contexts": self.contexts, "candidateRecords": self.candidates, "submissionEvidence": self.submissions,
            "observedEventCounts": dict(self.events), "unscopedRecords": self.unscoped,
            "submissionAssociationCounts": dict(association_counts), "analyzerLimits": self.limits,
            "analyzerOmittedRecords": dict(self.dropped), "completeHookCoverage": False, "submittedLayoutKnown": False,
            "gpuComplete": False, "lastWriterKnown": False, "resourceLifetimePinned": False, "copyPermitted": False,
            "gameFrameInputsVerified": False, "sameFrameVerified": False, "xeFGIntegrated": False,
            "meaning": "Exact PID/device/start-context/role/selection-generation/image-generation metadata links only. "
                "Selection generations are observer identities, not frame IDs or image lifetime leases. Candidate notices may be logged "
                "after matching submission records; log order is reported and does not prove execution order. Counters and current_roles "
                "do not join different roles into one frame. Raw submitted barriers do not prove a current layout, completion, last writer, "
                "or safe copy. Omitted candidate detail stays unresolved; summary identities never manufacture candidate records."}


class Analyzer:
    def __init__(self):
        self.warnings = Counter()
        self.warning_examples = []
        self.events = Counter()
        self.pids = Counter()
        self.instances = []
        self.physical_devices = {}
        self.devices = []
        self.device_active = {}
        self.images = []
        self.image_active = {}
        self.images_by_handle = defaultdict(set)
        self.image_history_by_handle = defaultdict(list)
        self.image_generations = Counter()
        self.swapchains = []
        self.swapchain_active = {}
        self.frames = {}
        self.sl_summaries = {}
        self.sl_installs = []
        self.sl_tokens = []
        self.tag_types = Counter()
        self.tag_associations = Counter()
        self.tags_observed = 0
        self.successful_readable_tags = 0
        self.matching_successful_tags = 0
        self.parsed_lines = 0
        self.blank_lines = 0
        self.malformed_lines = 0
        self.unterminated_final_line = False
        self.compute = ComputeAnalysis(self.warn)
        self.fsr_callback = FsrCallbackAnalysis(self.warn, self.associate_fsr_image)
        self.image_sync = ImageSyncAnalysis(self.warn)
        self.image_destroy_limit_devices = set()

    def warn(self, kind, line=None, detail=None):
        self.warnings[kind] += 1
        if len(self.warning_examples) < 80:
            self.warning_examples.append({"kind": kind, "line": line, "detail": detail})

    def device(self, pid, raw_handle, line, new=False):
        name = handle(raw_handle)
        if name is None:
            return None
        key = (pid, name)
        previous = self.device_active.get(key)
        if new and previous is not None:
            self.close_device(previous, line, "another_create_record_for_same_handle")
            self.warn("device_handle_reused_without_destroy", line, name)
        if new or previous is None:
            item = {"id": len(self.devices) + 1, "pid": pid, "device": name,
                    "firstObservedLine": line, "create": None, "destroy": None,
                    "queues": [], "presentSampleRecords": 0, "presentCallsLowerBound": 0,
                    "presentNegativeResultsLowerBound": 0, "presentCallsFinal": None,
                    "queueSubmitCallsLowerBound": 0, "observedImageCreateRecords": 0,
                    "observedImageDestroyRecords": 0, "imageCreateLogLimitLine": None}
            self.devices.append(item)
            self.device_active[key] = item
            return item
        return previous

    def close_image(self, item, line, reason):
        if item["endLine"] is not None:
            return
        item["endLine"] = line
        item["endReason"] = reason
        self.image_active.pop((item["deviceId"], item["image"]), None)
        self.images_by_handle[(item["pid"], item["image"])].discard(item["id"])

    def close_device(self, item, line, reason):
        for image in list(self.image_active.values()):
            if image["deviceId"] == item["id"]:
                self.close_image(image, line, reason)
        for key, swapchain in list(self.swapchain_active.items()):
            if key[0] == item["id"]:
                swapchain["endLine"] = line
                swapchain["endReason"] = reason
                del self.swapchain_active[key]
        self.device_active.pop((item["pid"], item["device"]), None)

    def add_image(self, device, raw_handle, record, line, source, swapchain=None):
        name = handle(raw_handle)
        if device is None or name is None:
            self.warn("image_without_valid_device_or_handle", line)
            return
        key = (device["id"], name)
        previous = self.image_active.get(key)
        if previous is not None:
            if source == "swapchain_images" and previous["swapchainId"] == swapchain:
                return  # vkGetSwapchainImagesKHR can enumerate the same images repeatedly.
            self.close_image(previous, line, "another_image_record_for_same_handle")
            self.warn("image_handle_reused_without_destroy", line, name)
        generation_key = (device["pid"], device["device"], name)
        self.image_generations[generation_key] += 1
        image = {"id": len(self.images) + 1, "pid": device["pid"], "deviceId": device["id"],
                 "device": device["device"], "image": name,
                 "generation": self.image_generations[generation_key], "source": source,
                 "beginLine": line, "endLine": None, "endReason": None,
                 "swapchainId": swapchain, "metadata": pick(record, IMAGE_FIELDS), "tagReferences": 0}
        image["metadata"]["formatName"] = FORMATS.get(record.get("format")) if integer(record.get("format")) is not None else None
        usage = integer(record.get("usage"))
        image["metadata"]["transferSrcUsageBit"] = bool(usage & 1) if usage is not None else None
        self.images.append(image)
        self.image_active[key] = image
        self.images_by_handle[(device["pid"], name)].add(image["id"])
        self.image_history_by_handle[(device["pid"], name)].append(image["id"])

    def frame(self, record, line):
        pid = process_id(record.get("pid"))
        frame, viewport = (integer(record.get(k)) for k in ("frame", "viewport"))
        complete = pid is not None and frame is not None and viewport is not None
        # Pointer tokens can be reused and must never substitute for a frame number.
        key = (pid, frame, viewport) if complete else ("unresolved", line)
        if key not in self.frames:
            self.frames[key] = {"pid": pid, "frame": frame, "viewport": viewport,
                                "grouping": "explicit_pid_frame_viewport" if complete else "single_call_missing_identity",
                                "tagCalls": [], "constants": []}
        return self.frames[key]

    def associate_tag(self, record, tag, line, index):
        pid = process_id(record.get("pid"))
        result = {"index": index, "metadata": pick(tag, ("type", "lifecycle", "extent", "native", "view",
                  "resourceType", "state", "format", "width", "height", "mips", "layers", "usage", "flags",
                  "readable", "resourceReadable")), "imageId": None}
        tag_type = integer(tag.get("type"))
        result["typeName"] = TARGET_TAGS.get(tag_type, "other_or_unreadable")
        result["declaredLifecycle"] = LIFECYCLES.get(integer(tag.get("lifecycle")), "unknown")
        image_handle = handle(tag.get("native"))
        readable = tag.get("readable") is not False and tag.get("resourceReadable") is not False
        if tag_type is not None:
            self.tag_types[tag_type] += 1
        if not readable:
            association = "unreadable_tag_or_resource"
        elif pid is None:
            association = "missing_explicit_pid"
        elif integer(tag.get("resourceType")) != 0:
            association = "not_a_declared_texture2d"
        elif image_handle is None:
            association = "missing_or_null_native_handle"
        else:
            candidates = sorted(self.images_by_handle.get((pid, image_handle), ()))
            if len(candidates) > 1:
                association = "ambiguous_handle_across_devices"
                result["candidateImageIds"] = candidates
                for candidate in candidates:
                    self.images[candidate - 1]["tagReferences"] += 1
            elif not candidates:
                association = "no_observed_active_image"
                prior = self.image_history_by_handle.get((pid, image_handle), [])
                if prior:
                    # Context only: never revive a destroyed lifetime as an active match.
                    result["lastEndedImageId"] = prior[-1]
                    self.images[prior[-1] - 1]["tagReferences"] += 1
            else:
                association = "unique_observed_active_image"
                image = self.images[candidates[0] - 1]
                image["tagReferences"] += 1
                result["imageId"] = image["id"]
                device = self.devices[image["deviceId"] - 1]
                result["imageLogLimitAlreadyReached"] = (device["imageCreateLogLimitLine"] is not None
                                                         or device["observedImageDestroyRecords"] >= 4096)
                comparisons = {}
                for sl_field, vk_field in (("format", "format"), ("width", "width"), ("height", "height"),
                                            ("mips", "mip_levels"), ("layers", "array_layers"), ("usage", "usage")):
                    supplied, created = integer(tag.get(sl_field)), integer(image["metadata"].get(vk_field))
                    comparisons[sl_field] = ("not_provided_or_zero" if not supplied else
                        "no_creation_metadata" if created is None else "equal" if supplied == created else "different")
                result["slVersusImageMetadata"] = comparisons
                result["extentAssessment"] = self.extent_assessment(tag.get("extent"), image["metadata"])
                if integer(record.get("result")) == 0:
                    self.matching_successful_tags += 1
        if (readable and image_handle is not None and integer(tag.get("resourceType")) == 0
                and integer(record.get("result")) == 0):
            self.successful_readable_tags += 1
        self.tag_associations[association] += 1
        result["association"] = association
        return result

    def associate_fsr_image(self, pid, device_handle, binding, line):
        result = {"imageId": None}
        image_handle = handle(binding.get("image_candidate"))
        device = self.device_active.get((pid, device_handle))
        if image_handle is None:
            result["association"] = "missing_or_null_image_candidate"
        elif device is None:
            result["association"] = "no_observed_active_device"
        else:
            result["deviceId"] = device["id"]
            result["imageCreateLogLimitAlreadyReached"] = device["imageCreateLogLimitLine"] is not None
            result["imageDestroyLogLimitAlreadyReached"] = device["id"] in self.image_destroy_limit_devices
            image = self.image_active.get((device["id"], image_handle))
            if image is None:
                result["association"] = "no_observed_active_image_same_pid_device"
            elif image["source"] != "create_image":
                result["association"] = "candidate_is_observed_swapchain_image_not_create_image"
            elif result["imageDestroyLogLimitAlreadyReached"]:
                # End records may be omitted; do not revive a possibly stale handle.
                result["association"] = "image_destroy_log_gap_lifetime_unresolved"
            elif image["beginLine"] >= line:
                result["association"] = "image_creation_not_observed_before_candidate"
            else:
                result["association"] = "unique_observed_active_image_numeric_candidate"
                result["imageId"] = image["id"]
                result["dimensionComparisons"] = {}
                for candidate, image_field in (("width_candidate", "width"), ("height_candidate", "height")):
                    supplied, known = integer(binding.get(candidate)), integer(image["metadata"].get(image_field))
                    result["dimensionComparisons"][image_field] = ("unavailable" if supplied is None or known is None else
                        "equal" if supplied == known else "different")
        return result

    @staticmethod
    def extent_assessment(extent, metadata):
        if not isinstance(extent, list) or len(extent) != 4 or any(integer(v) is None for v in extent):
            return "missing_or_invalid_extent"
        if extent == [0, 0, 0, 0]:
            return "default_whole_resource_extent_declared"
        width, height = integer(metadata.get("width")), integer(metadata.get("height"))
        if width is None or height is None:
            return "no_image_extent_metadata"
        left, top, w, h = extent
        return ("within_image_metadata" if left >= 0 and top >= 0 and w > 0 and h > 0
                and left + w <= width and top + h <= height else "outside_or_empty_image_extent")

    def consume(self, record, line):
        event = record.get("event")
        if not isinstance(event, str):
            self.warn("record_missing_event_name", line)
            return
        if event not in KNOWN_EVENTS:
            self.events["unknown_event"] += 1
            self.warn("unknown_event", line, event[:100])
            return
        self.events[event] += 1
        pid = process_id(record.get("pid"))
        if pid is None:
            self.warn("record_missing_valid_pid", line, event)
        else:
            self.pids[pid] += 1
        successful = integer(record.get("result")) == 0
        if event in FSR_EVENTS:
            self.fsr_callback.consume(record, line)
            return
        if event in IMAGE_SYNC_EVENTS:
            self.image_sync.consume(record, line)
            return
        if event.startswith("compute_"):
            self.compute.consume(record, line)
            return
        if event == "destroy_device":
            self.compute.device_destroyed(record, line)
            self.fsr_callback.device_destroyed(record, line)
            self.image_sync.device_destroyed(record, line)
        if event in ("create_instance", "destroy_instance", "create_win32_surface", "destroy_surface"):
            self.instances.append({"event": event, "pid": pid, **stamp(record, line), **pick(record,
                ("instance", "result", "api_version_requested", "enabled_extensions", "enabled_layers", "surface", "hwnd", "flags"))})
        elif event == "physical_device_properties":
            physical = handle(record.get("physical_device"))
            key = (pid, physical, handle(record.get("instance")))
            self.physical_devices[key] = {"pid": pid, **stamp(record, line), **pick(record,
                ("physical_device", "instance", "device_name", "vendor_id", "device_id", "api_version", "driver_version", "device_type"))}
        elif event in ("sl_tags", "sl_constants"):
            frame = self.frame(record, line)
            if event == "sl_constants":
                frame["constants"].append({**stamp(record, line), **pick(record, CONSTANT_FIELDS)})
            else:
                tags = record.get("tags")
                if not isinstance(tags, list):
                    self.warn("sl_tags_array_missing", line)
                    tags = []
                call = {**stamp(record, line), **pick(record,
                    ("call", "result", "token", "commandBuffer", "tagCount", "sampleTruncated")), "tags": []}
                for index, tag in enumerate(tags):
                    self.tags_observed += 1
                    if not isinstance(tag, dict):
                        self.warn("sl_tag_not_object", line, index)
                        continue
                    call["tags"].append(self.associate_tag(record, tag, line, index))
                frame["tagCalls"].append(call)
                if record.get("sampleTruncated") is True:
                    self.warn("sl_tag_sample_truncated", line)
        elif event == "sl_observer_install":
            self.sl_installs.append({"pid": pid, **stamp(record, line), **pick(record, ("state", "installedMask"))})
        elif event == "sl_observer_summary":
            # These are cumulative counters. Repeated periodic snapshots are not additive.
            summary = self.sl_summaries.setdefault(pid, {"pid": pid, "snapshotCount": 0, "counterMaxima": {}})
            summary["snapshotCount"] += 1
            summary["lastObservedLine"] = line
            summary["installedMaskLast"] = record.get("installedMask")
            if "hookSlotsStillActive" in record or "hookSlotCheck" in record or "hookSlotStates" in record:
                summary["lastSlotObservation"] = {**stamp(record, line), **pick(record,
                    ("hookSlotsStillActive", "hookSlotCheck", "hookSlotStates"))}
            for field in ("tagCalls", "tagsTotal", "constantsCalls", "tokenCalls", "emitted", "traceFailures"):
                value = integer(record.get(field))
                if value is not None and value >= 0:
                    summary["counterMaxima"][field] = max(value, summary["counterMaxima"].get(field, 0))
        elif event == "sl_frame_token":
            self.sl_tokens.append({"pid": pid, **stamp(record, line), **pick(record,
                ("call", "result", "token", "frame", "requestedFrame"))})
        elif event == "observer_event_too_large":
            self.warn("observer_dropped_oversized_event", line)
        else:
            device = self.device(pid, record.get("device"), line, new=(event == "create_device" and successful))
            if event == "create_device" and not successful:
                self.warn("device_creation_failed", line, record.get("result"))
                return
            if device is None:
                if event != "enumerate_physical_devices":
                    self.warn("event_missing_device_handle", line, event)
                return
            if event == "create_device":
                device["create"] = {**stamp(record, line), **pick(record, DEVICE_FIELDS)}
            elif event == "destroy_device":
                device["destroy"] = {**stamp(record, line), **pick(record, ("present_calls", "present_negative_results",
                    "image_create_calls", "image_destroy_calls", "queue_submit_calls"))}
                for source, target in (("present_calls", "presentCallsFinal"), ("present_negative_results", "presentNegativeResultsFinal")):
                    device[target] = integer(record.get(source))
                self.close_device(device, line, "device_destroy_record")
            elif event in ("get_device_queue", "get_device_queue2"):
                device["queues"].append({**stamp(record, line), **pick(record, ("queue", "family", "index", "flags"))})
            elif event == "queue_present_sample":
                device["presentSampleRecords"] += 1
                for source, target in (("present_calls", "presentCallsLowerBound"),
                                       ("present_negative_results", "presentNegativeResultsLowerBound")):
                    value = integer(record.get(source))
                    if value is not None:
                        device[target] = max(device[target], value)
                device["lastPresentSample"] = {**stamp(record, line), **pick(record,
                    ("queue", "result", "swapchain_count", "wait_semaphore_count"))}
            elif event in ("queue_submit_sample", "queue_submit2_sample"):
                device["queueSubmitCallsLowerBound"] = max(device["queueSubmitCallsLowerBound"], integer(record.get("call")) or 0)
            elif event == "create_image":
                device["observedImageCreateRecords"] += 1
                if successful:
                    self.add_image(device, record.get("image"), record, line, "create_image")
            elif event == "image_create_log_limit":
                device["imageCreateLogLimitLine"] = line
                self.warn("observer_image_create_log_limit", line, device["device"])
            elif event == "image_destroy_log_limit":
                self.image_destroy_limit_devices.add(device["id"])
                self.warn("observer_image_destroy_log_limit", line, device["device"])
            elif event == "destroy_image":
                device["observedImageDestroyRecords"] += 1
                image = self.image_active.get((device["id"], handle(record.get("image"))))
                if image is not None:
                    self.close_image(image, line, "destroy_image_record")
                elif handle(record.get("image")):
                    self.warn("image_destroy_without_observed_active_creation", line, record.get("image"))
            elif event == "create_swapchain" and successful:
                name = handle(record.get("swapchain"))
                if name is None:
                    self.warn("swapchain_missing_handle", line)
                    return
                key = (device["id"], name)
                if key in self.swapchain_active:
                    self.warn("swapchain_handle_reused_without_destroy", line, name)
                    self.close_swapchain(self.swapchain_active[key], line, "another_create_record_for_same_handle")
                swapchain = {"id": len(self.swapchains) + 1, "pid": pid, "deviceId": device["id"],
                             "swapchain": name, "beginLine": line, "endLine": None, "endReason": None,
                             "metadata": pick(record, ("old_swapchain", "surface", "format", "color_space", "width", "height",
                                 "image_usage", "minimum_image_count", "array_layers", "present_mode", "sharing_mode", "flags"))}
                self.swapchains.append(swapchain)
                self.swapchain_active[key] = swapchain
            elif event == "destroy_swapchain":
                swapchain = self.swapchain_active.get((device["id"], handle(record.get("swapchain"))))
                if swapchain is not None:
                    self.close_swapchain(swapchain, line, "destroy_swapchain_record")
            elif event == "swapchain_images" and integer(record.get("result")) in (0, 5):
                swapchain = self.swapchain_active.get((device["id"], handle(record.get("swapchain"))))
                names = record.get("images")
                if not isinstance(names, list):
                    self.warn("swapchain_images_array_missing", line)
                    return
                if (integer(record.get("count")) or 0) > len(names):
                    self.warn("swapchain_image_handles_sampled", line)
                if swapchain is None:
                    self.warn("swapchain_images_without_create_record", line)
                    return
                metadata = dict(swapchain["metadata"])
                metadata["usage"] = metadata.get("image_usage")
                for name in names:
                    self.add_image(device, name, metadata, line, "swapchain_images", swapchain["id"])

    def close_swapchain(self, swapchain, line, reason):
        swapchain["endLine"], swapchain["endReason"] = line, reason
        self.swapchain_active.pop((swapchain["deviceId"], swapchain["swapchain"]), None)
        for image in list(self.image_active.values()):
            if image["swapchainId"] == swapchain["id"]:
                self.close_image(image, line, reason)

    def parse(self, data):
        self.unterminated_final_line = bool(data) and not data.endswith(b"\n")
        if self.unterminated_final_line:
            self.warn("unterminated_final_line_may_be_live_or_truncated")
        for number, line in enumerate(data.split(b"\n"), 1):
            if not line.strip():
                if line:
                    self.blank_lines += 1
                continue
            if len(line) > LINE_LIMIT:
                self.warn("line_exceeds_256_KiB_parse_limit", number)
                self.malformed_lines += 1
                continue
            try:
                record = decode_json(line)
                if not isinstance(record, dict):
                    raise ValueError("JSON line must be an object")
            except (UnicodeDecodeError, ValueError, RecursionError) as error:
                self.malformed_lines += 1
                self.warn("unparseable_json_line", number, str(error)[:160])
                continue
            self.parsed_lines += 1
            self.consume(record, number)

    def finish(self, source, optional):
        frames = list(self.frames.values())
        frames_with_all = 0
        for frame in frames:
            observed, successful, matched = set(), set(), set()
            all_in_one_call = 0
            for call in frame["tagCalls"]:
                call_types = set()
                for tag in call["tags"]:
                    tag_type = integer(tag["metadata"].get("type"))
                    if tag_type is None:
                        continue
                    observed.add(tag_type)
                    if integer(call.get("result")) == 0 and tag["metadata"].get("readable") is not False:
                        successful.add(tag_type)
                        call_types.add(tag_type)
                        if tag["association"] == "unique_observed_active_image":
                            matched.add(tag_type)
                if set(TARGET_TAGS) <= call_types:
                    all_in_one_call += 1
            frame["tagTypesObservedAcrossCalls"] = sorted(observed)
            frame["tagTypesOnSuccessfulCalls"] = sorted(successful)
            frame["tagTypesWithUniqueImageAssociationOnSuccessfulCalls"] = sorted(matched)
            frame["targetTagTypesNotObserved"] = sorted(set(TARGET_TAGS) - observed)
            frame["successfulCallsContainingAllFourTargetTagTypes"] = all_in_one_call
            frame["successfulReadableConstantsRecords"] = sum(
                integer(c.get("result")) == 0 and c.get("readable") is not False and "version" in c
                for c in frame["constants"])
            if set(TARGET_TAGS) <= matched and frame["grouping"] == "explicit_pid_frame_viewport":
                frames_with_all += 1
        present_lower = sum(max(d["presentCallsLowerBound"], d["presentCallsFinal"] or 0) for d in self.devices)
        all_devices_closed = bool(self.devices) and all(d["presentCallsFinal"] is not None for d in self.devices)
        lifetime_images = [image for image in self.images if image["tagReferences"]]
        formats = Counter(str(image["metadata"].get("format")) for image in self.images)
        compute = self.compute.finish()
        return {
            "schemaVersion": 2, "generatedAtUtc": datetime.now(timezone.utc).isoformat(),
            "source": source, "optionalRecords": optional,
            "assessment": {
                "status": "metadata_only" if self.successful_readable_tags else "no_successful_texture_tag_metadata_observed",
                "inputMetadataObserved": bool(self.successful_readable_tags),
                "inputMetadataCorrelatedToObservedVulkanImages": bool(self.matching_successful_tags),
                "gameFrameInputsVerified": False, "pixelsRead": False, "pixelContentVerified": False,
                "xeFGIntegrated": False, "generatedFramesVerified": False,
                "explanation": "Observed input metadata does not establish correct pixels, depth/motion semantics, GPU synchronization, or XeSS frame generation integration."
            },
            "parsing": {"validJsonObjectLines": self.parsed_lines, "malformedLines": self.malformed_lines,
                        "blankLines": self.blank_lines, "unterminatedFinalLine": self.unterminated_final_line,
                        "allReadLinesParsed": self.malformed_lines == 0,
                        "warningCounts": dict(self.warnings), "warningExamples": self.warning_examples,
                        "warningExamplesOmitted": max(0, sum(self.warnings.values()) - len(self.warning_examples))},
            "eventCounts": dict(sorted(self.events.items())),
            "processes": [{"pid": pid, "observedEventRecords": count} for pid, count in sorted(self.pids.items())],
            "vulkan": {
                "instanceEvents": self.instances, "physicalDevices": list(self.physical_devices.values()),
                "devices": self.devices, "swapchains": self.swapchains,
                "presents": {"callsObservedLowerBound": present_lower,
                    "allObservedDeviceLifetimesHaveFinalCounters": all_devices_closed,
                    "finalCounterSum": present_lower if all_devices_closed else None,
                    "meaning": "vkQueuePresentKHR API calls; not displayed, successful, or generated frame counts."},
                "images": {"observedLifetimes": len(self.images), "noEndRecord": len(self.image_active),
                    "formatCounts": dict(formats), "referencedByStreamlineTags": lifetime_images,
                    "unreferencedLifetimesOmitted": len(self.images) - len(lifetime_images)}
            },
            "streamline": {
                "targetTags": {str(k): v for k, v in TARGET_TAGS.items()},
                "installEvents": self.sl_installs, "cumulativeSummaryMaximaByPid": list(self.sl_summaries.values()),
                "frameTokenSamples": self.sl_tokens, "tagRecordsObserved": self.tags_observed,
                "tagTypeCounts": {str(k): v for k, v in sorted(self.tag_types.items())},
                "associationCounts": dict(self.tag_associations), "frameViewportGroups": frames,
                "groupsWithAllFourTargetTagTypesAssociatedOnSuccessfulCalls": frames_with_all
            },
            "compute": compute,
            "fsrCallback": self.fsr_callback.finish(self.images),
            "imageSync": self.image_sync.finish(),
            "limitations": [
                "Only explicitly selected session metadata files were read; image pixels and game resources were not opened.",
                "Image association uses explicit PID, normalized handle, and log-record lifetime order. Missing PID/frame/viewport is never inferred from pointer tokens.",
                "Source wrappers emit after downstream calls. Concurrent threads and sampled/dropped records can make record order differ from resource-use order; an active observed handle is not a synchronization proof.",
                "SL resource lifecycle and state are declarations. They do not prove actual VkImage layout, queue-family ownership, GPU completion, pixel contents, or correctness until present.",
                "Tags collected in separate calls of one frame/viewport are a union of observations; they need not be simultaneously valid or belong to one feature evaluation.",
                "Image creation has a first-call budget and an additional bounded large render/storage sample budget; destruction, tags/constants/presents/submits also have version-dependent limits. Absence of a record does not establish absence of that operation.",
                "Compute metadata is linked only by explicit PID/device/context, shader/pipeline generations, matching handles and prior log records. FNV fingerprints are not security identities. Missing or stripped names and truncated metadata do not prove absence of a feature.",
                "Compute dispatch samples describe command-buffer recording, not GPU execution or descriptor-bound VkImages. No shader name, descriptor set/binding number, or image dimension proves XeFG-compatible frame inputs.",
                "FSR callback candidates preserve property/read statuses, not verified texture semantics. Log-lifetime links use the same PID/device and prior active create_image metadata. Separate runtime inventory matches retain only point-in-time numeric handle metadata; they do not fill missing create/destroy history or establish resource lifetime. Callback samples have no frame identity or GPU-execution proof.",
                "Same-frame constants are retained as supplied, without asserting projection conventions, motion-vector scaling, depth encoding, or compatibility with XeFG.",
                "installedMask records installation, not continued slot activity. Optional lastSlotObservation is only a point-in-time read and does not establish uninterrupted tracing.",
                "No entry in this report verifies game XeFG integration; success in a self-owned smoke test is not a game input or frame-generation test."
            ]
        }


def read_session_file(session, name, limit, analyzer, required=False):
    path = session / name
    info = {"name": name, "exists": False, "bytesRead": 0, "limitBytes": limit}
    try:
        if not path.exists():
            if required:
                analyzer.warn("observer_log_missing")
            return None, info
        if path.is_symlink() or path.resolve().parent != session:
            raise ValueError("Session input must be a regular file directly in the selected directory")
        if not path.is_file():
            raise ValueError("Session input is not a regular file")
        with path.open("rb") as stream:
            before = os.fstat(stream.fileno()).st_size
            data = stream.read(limit)
            after = os.fstat(stream.fileno()).st_size
        info.update(exists=True, bytesRead=len(data), sizeBeforeRead=before, sizeAfterRead=after,
                    readLimited=before > limit or after > limit, changedDuringRead=before != after,
                    reachedLogByteLimit=len(data) == limit)
        if info["readLimited"]:
            analyzer.warn("file_read_limited", detail=name)
        if info["changedDuringRead"]:
            analyzer.warn("file_changed_during_read", detail=name)
        if name == "observer.jsonl" and len(data) >= limit - 32768:
            analyzer.warn("observer_log_near_cap_may_have_stopped", detail=name)
        return data, info
    except (OSError, ValueError) as error:
        analyzer.warn("input_read_failed", detail=name + ": " + str(error))
        info["error"] = str(error)
        return None, info


def analyze_session(session):
    session = Path(session).expanduser().resolve(strict=True)
    if not session.is_dir():
        raise ValueError("SESSION must be a directory")
    analyzer = Analyzer()
    data, source = read_session_file(session, "observer.jsonl", LOG_LIMIT, analyzer, required=True)
    source["directory"] = str(session)
    optional = {}
    for name in ("launch.json", "smoke-result.json", "result.json"):
        content, info = read_session_file(session, name, OPTIONAL_LIMIT, analyzer)
        optional[name] = {"source": info}
        if content is not None and not info.get("readLimited"):
            try:
                record = decode_json(content)
                if not isinstance(record, dict):
                    raise ValueError("Expected an object")
                # Do not follow paths or copy environment data from the metadata.
                fields = ("startedAt", "launcherProcessId", "observerDllSha256", "explicitLayerInstalled",
                    "streamlineTraceRequested", "runtimeStreamlineHooksRequested", "bridgeDeviceRequirementsRequested",
                    "computeObserverRequested", "fsrCallbackObserverRequested", "fsrObserverRequested", "runtimeFsrHooksRequested", "modifiedGameFiles") if name == "launch.json" else (
                    "test", "capturedAt", "passed", "elevated", "frames", "exitCode", "timedOut", "eventCount",
                    "observerDllSha256", "smokeSha256", "computeObserverRequested", "bridgeDeviceRequested", "fsrCallbackObserverRequested", "fsrObserverRequested", "gameTested")
                optional[name]["metadata"] = pick(record, fields)
            except (UnicodeDecodeError, ValueError, RecursionError) as error:
                analyzer.warn("optional_json_unparseable", detail=name + ": " + str(error)[:160])
    if data is not None:
        if not data:
            analyzer.warn("observer_log_empty")
        analyzer.parse(data)
    return analyzer.finish(source, optional)


def write_summary(summary, destination):
    # Atomic replacement applies only to this explicitly selected result file.
    destination = Path(destination).expanduser().resolve()
    source = Path(summary["source"]["directory"])
    if destination == source or source in destination.parents:
        raise ValueError("--output must be outside the source session directory (source is read-only)")
    if destination.name.lower() != "summary.json":
        raise ValueError("--output must name a summary.json file")
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", newline="\n", prefix=".observer-summary-",
                                         suffix=".tmp", dir=destination.parent, delete=False) as stream:
            temporary = stream.name
            json.dump(summary, stream, ensure_ascii=False, indent=2, allow_nan=False)
            stream.write("\n")
        os.replace(temporary, destination)
    finally:
        if temporary is not None and os.path.exists(temporary):
            os.unlink(temporary)
    return destination


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("session", help="One diagnostic session directory; no directory scanning")
    parser.add_argument("--output", required=True, help="Explicit output path outside SESSION, ending in summary.json")
    args = parser.parse_args(argv)
    try:
        summary = analyze_session(args.session)
        destination = write_summary(summary, args.output)
    except (OSError, ValueError, RecursionError) as error:
        print("Observer analysis failed: " + str(error), file=sys.stderr)
        return 1
    print("Observer metadata analysis: " + str(destination))
    print("PIDs: {0}; devices: {1}; present calls observed >= {2}; SL tag samples: {3}; unique image associations: {4}".format(
        ", ".join(str(p["pid"]) for p in summary["processes"]) or "none", len(summary["vulkan"]["devices"]),
        summary["vulkan"]["presents"]["callsObservedLowerBound"], summary["streamline"]["tagRecordsObserved"],
        summary["streamline"]["associationCounts"].get("unique_observed_active_image", 0)))
    warnings = summary["parsing"]["warningCounts"]
    if warnings:
        print("Warnings: " + ", ".join(f"{key}={value}" for key, value in warnings.items()))
    print("Pixels/content, synchronization, and game XeFG integration: NOT VERIFIED.")
    compute = summary["compute"]
    print("Compute metadata: {0} shaders; {1} pipelines; {2} dispatch recording samples; {3} explicit FSR-name candidates (not execution proof).".format(
        len(compute["shaders"]), len(compute["pipelines"]), len(compute["dispatchRecordingSamples"]), len(compute["fsrIdentifierCandidates"])))
    fsr = summary["fsrCallback"]
    print("FSR callback metadata: {0} contexts; {1} retained candidate samples; {2} numeric matches to image create/destroy log history (not input proof).".format(
        len(fsr["contexts"]), len(fsr["candidateSamples"]), fsr["associationCounts"].get("unique_observed_active_image_numeric_candidate", 0)))
    print("Runtime image inventory: {0} retained observations; {1} reported numeric live-handle matches (point-in-time metadata only).".format(
        len(fsr["imageMetadataMatches"]), fsr["inventoryNumericMatchesObserved"]))
    sync = summary["imageSync"]
    print("Image sync metadata: {0} contexts; {1} retained selections; {2} submitted-barrier samples (no same-frame or copy proof).".format(
        len(sync["contexts"]), len(sync["candidateRecords"]), len(sync["submissionEvidence"])))
    if fsr["peGuardStateCountsInObservedRecords"]:
        print("PE guard states in observed records: " + ", ".join(
            "{0}={1}".format(key, value) for key, value in list(fsr["peGuardStateCountsInObservedRecords"].items())[:8]))
    source = summary["source"]
    return 2 if (not source["exists"] or "error" in source or not source["bytesRead"]
                 or source.get("readLimited") or source.get("changedDuringRead")
                 or summary["parsing"]["malformedLines"] or summary["parsing"]["unterminatedFinalLine"]) else 0


if __name__ == "__main__":
    raise SystemExit(main())
