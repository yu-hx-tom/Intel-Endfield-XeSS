#!/usr/bin/env python3
"""Small in-memory fixtures for explicit compute and FSR callback metadata only."""
import importlib.util
import json
from pathlib import Path
import sys
import unittest

sys.dont_write_bytecode = True
SPEC = importlib.util.spec_from_file_location("observer_analysis", Path(__file__).with_name("analyze-observer-session.py"))
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


def event(name, **fields):
    return {"event": name, "pid": 7, "device": "0xd", **fields}


def start(**fields):
    return event("compute_observer_start", **fields)


def shader(**fields):
    return event("compute_shader_metadata", shader="0x10", shader_generation=1,
        entry_points=["CS"], names=[{"id": 8, "kind": 0, "member_index": None, "name": "r_input_motion_vectors"}],
        bindings=[{"id": 8, "set": 0, "binding": 3, "name": "r_input_motion_vectors"}], **fields)


def pipeline(**fields):
    return event("compute_pipeline", **{ "pipeline": "0x20", "pipeline_generation": 2,
        "shader": "0x10", "shader_generation": 1, "entry_point": "CS", "batch_result": 0, **fields})


def dispatch(**fields):
    return event("compute_dispatch_sample", **{"pipeline": "0x20", "pipeline_generation": 2,
        "shader_generation": 1, "command_buffer": "0x30", "recording_generation": 3,
        "recording_call": 1, "pipeline_dispatch_recordings": 1, "groups": [1, 1, 1], "groups_known": True,
        "kind": "dispatch", **fields})


def analyze(records, limits=None):
    analyzer = MODULE.Analyzer()
    if limits:
        analyzer.compute.limits.update(limits)
    analyzer.parse(("\n".join(json.dumps(record) for record in records) + "\n").encode())
    return analyzer.finish({"directory": str(Path.cwd()), "exists": True}, {})


class ComputeFixtures(unittest.TestCase):
    def test_start_before_device_and_cumulative_summary(self):
        records = [start(), event("create_device", result=0), shader(), pipeline(), dispatch(),
            event("compute_observer_summary", dispatch_recording_calls=24, trace_failures=1, live_shaders=2),
            event("destroy_device", present_calls=24, present_negative_results=0),
            event("compute_observer_summary", dispatch_recording_calls=24, trace_failures=1, live_shaders=0)]
        result = analyze(records)
        compute = result["compute"]
        self.assertEqual(len(result["vulkan"]["devices"]), 1)
        self.assertEqual(compute["dispatchRecordingCallsLowerBound"], 24)
        self.assertEqual(compute["contexts"][0]["counterMaxima"]["trace_failures"], 1)
        self.assertEqual(compute["contexts"][0]["lastSummary"]["live_shaders"], 0)
        self.assertEqual(compute["dispatchRecordingSamples"][0]["shaderMetadataId"], 1)
        self.assertEqual(compute["pipelines"][0]["shaderMetadataId"], 1)
        self.assertFalse(compute["gpuExecutionVerified"])
        self.assertFalse(result["assessment"]["gameFrameInputsVerified"])

    def test_explicit_names_and_provenance_only(self):
        record = shader()
        record["names"] += [{"id": 9, "kind": 2, "name": "FSR3_FrameInterpolation"},
                            {"id": 10, "kind": 1, "member_index": 2, "name": "depthScale"}]
        result = analyze([start(), record, pipeline(), dispatch()])["compute"]
        self.assertEqual(len(result["fsrIdentifierCandidates"]), 1)
        self.assertEqual(result["fsrIdentifierCandidates"][0]["hints"][0]["kind"], 2)
        self.assertEqual(result["fsrIdentifierCandidates"][0]["dispatchRecordingSampleIds"], [1])
        self.assertTrue(all(x.get("kind") != 1 and x.get("kind") != 2
            for x in result["resourceNameCandidates"][0]["hints"]))
        generic = shader(); generic["names"] = []; generic["bindings"] = []
        result = analyze([start(), generic, pipeline(), dispatch()])["compute"]
        self.assertEqual(result["fsrIdentifierCandidates"], [])
        self.assertEqual(result["resourceNameCandidates"], [])

    def test_pid_and_device_do_not_cross(self):
        records = [start(), shader(), start(device="0xe"), pipeline(device="0xe"), dispatch(device="0xe"),
                   start(pid=8), pipeline(pid=8), dispatch(pid=8)]
        result = analyze(records)["compute"]
        self.assertTrue(all(p["shaderMetadataId"] is None for p in result["pipelines"]))
        self.assertTrue(all(s["shaderMetadataId"] is None for s in result["dispatchRecordingSamples"]))
        self.assertEqual(len(result["contexts"]), 3)

    def test_new_context_and_duplicate_generation(self):
        result = analyze([start(), shader(), pipeline(), start(), dispatch()])["compute"]
        self.assertEqual(result["dispatchRecordingSamples"][0]["pipelineAssociation"],
                         "pipeline_metadata_not_observed_in_context")
        result = analyze([start(), shader(), shader(), pipeline(), pipeline(), dispatch()])["compute"]
        self.assertEqual(result["pipelines"][0]["shaderAssociation"], "ambiguous_shader_generation")
        self.assertEqual(result["dispatchRecordingSamples"][0]["pipelineAssociation"], "ambiguous_pipeline_generation")

    def test_generation_handles_and_log_order_required(self):
        result = analyze([start(), shader(), pipeline(shader="0x11"), dispatch(shader_generation=4)])["compute"]
        self.assertEqual(result["pipelines"][0]["shaderAssociation"], "shader_handle_mismatch")
        self.assertEqual(result["dispatchRecordingSamples"][0]["pipelineAssociation"], "shader_generation_mismatch")
        result = analyze([start(), dispatch(), pipeline(), shader()])["compute"]
        self.assertEqual(result["pipelines"][0]["shaderAssociation"], "shader_metadata_not_observed_before_pipeline")
        self.assertEqual(result["dispatchRecordingSamples"][0]["pipelineAssociation"], "pipeline_metadata_not_observed_before_recording")
        result = analyze([start(), shader(), pipeline(), dispatch(recording_generation=0)])["compute"]
        self.assertEqual(result["dispatchRecordingSamples"][0]["pipelineAssociation"], "command_recording_identity_unavailable")

    def test_limits_and_source_truncation_survive(self):
        first = shader(metadata_truncated=True, unresolved_decoration_groups=True, log_names_truncated=True,
                       log_bindings_truncated=True)
        second = shader(); second["shader_generation"] = 4
        result = analyze([start(), first, second, pipeline(shader_generation=4),
            event("compute_observer_summary", parse_refused=2, state_limit_hits=3, event_limit_hits=4,
                  trace_failures=5, shader_byte_budget_exhausted=True)], {"shaders": 1})["compute"]
        self.assertEqual(result["analyzerOmittedRecords"]["shaders"], 1)
        self.assertIsNone(result["pipelines"][0]["shaderMetadataId"])
        self.assertTrue(result["shaders"][0]["unresolved_decoration_groups"])
        self.assertTrue(result["resourceNameCandidates"][0]["metadataLimits"]["log_names_truncated"])
        self.assertEqual(result["contexts"][0]["counterMaxima"]["trace_failures"], 5)
        self.assertTrue(result["contexts"][0]["shaderByteBudgetExhaustedObserved"])

    def test_missing_identity_and_long_metadata_not_classified(self):
        first = shader(); first["pid"] = None
        result = analyze([first])["compute"]
        self.assertEqual(result["unscopedRecords"], 1)
        self.assertEqual(result["shaders"], [])
        first = shader(); first["names"] = [{"id": 8, "kind": 0, "name": "FSR" + "x" * 1024}]
        first["bindings"] = []
        result = analyze([start(), first])["compute"]
        self.assertTrue(result["shaders"][0]["analyzerMetadataTruncated"])
        self.assertEqual(result["fsrIdentifierCandidates"], [])


def fsr_sample(**fields):
    binding = {"property": "r_input_depth", "graph_handle": "0x9", "graph_status": "GraphTextureIdRead",
        "native_status": "NativeCandidateReadUnverified", "image_candidate": "0x99", "width_candidate": 1920,
        "height_candidate": 1080, "entry_type": 2, "texture_id": 4}
    return event("fsr_prepare_input_candidate", **{"status": "SnapshotParsed", "matched_call": 1,
        "all_prepare_properties": True, "context_bounds_unchanged": True, "bindings": [binding], **fields})


class FsrCallbackFixtures(unittest.TestCase):
    def test_install_before_device_refusal_and_latest_state(self):
        result = analyze([event("fsr_observer_install", state="refused_process_path", requested=True,
                mh_initialize=None, mh_create=None, mh_enable=None, hook_created=False),
            event("create_device", result=0),
            event("fsr_observer_install_summary", state="refused_process_path", hook_bytes_still_active=None),
            event("fsr_observer_install", state="stopped_selected_device_destroyed", sampling_allowed=False),
            event("destroy_device", present_calls=24)])
        self.assertEqual(result["schemaVersion"], 2)
        self.assertEqual(len(result["vulkan"]["devices"]), 1)
        fsr = result["fsrCallback"]
        self.assertEqual(fsr["candidateSamples"], [])
        context = fsr["contexts"][0]
        self.assertEqual(context["firstInstallationRecord"]["state"], "refused_process_path")
        self.assertIsNone(context["firstInstallationRecord"]["mh_enable"])
        self.assertEqual(context["lastInstallationRecord"]["state"], "stopped_selected_device_destroyed")
        self.assertEqual(context["counterMaxima"], {})  # No callback summary means unavailable, not measured zero.
        self.assertFalse(fsr["xeFGIntegrated"])

    def test_numeric_candidate_does_not_verify_native_domain(self):
        result = analyze([event("create_device", result=0), event("create_image", result=0, image="0x99", width=1920, height=1080),
            fsr_sample(), event("fsr_callback_summary", callback_calls=20, prepare_property_matches=10, samples=4,
                unreadable_payloads=2, trace_failures=1, sampling_enabled=True),
            event("fsr_callback_summary", callback_calls=20, prepare_property_matches=10, samples=4,
                unreadable_payloads=2, trace_failures=1, sampling_enabled=False),
            event("destroy_image", image="0x99")])["fsrCallback"]
        binding = result["candidateSamples"][0]["bindings"][0]
        self.assertEqual(binding["association"], "unique_observed_active_image_numeric_candidate")
        self.assertEqual(binding["imageId"], 1)
        self.assertEqual(binding["dimensionComparisons"], {"width": "equal", "height": "equal"})
        self.assertFalse(binding["vulkanImageVerified"])
        self.assertFalse(binding["nativeIdDomainVerified"])
        self.assertEqual(result["referencedImageLifetimes"][0]["endLine"], 6)
        self.assertEqual(result["contexts"][0]["counterMaxima"]["callback_calls"], 20)
        self.assertEqual(result["contexts"][0]["counterMaxima"]["trace_failures"], 1)
        self.assertFalse(result["contexts"][0]["lastCallbackSummary"]["sampling_enabled"])

    def test_scope_lifetime_missing_creation_and_sampling_gaps(self):
        records = [event("create_device", result=0), event("create_image", result=0, image="0x99"),
            event("create_device", result=0, device="0xe"), fsr_sample(device="0xe"), fsr_sample(pid=8),
            event("destroy_image", image="0x99"), fsr_sample(),
            event("image_create_log_limit"), fsr_sample(),
            event("create_image", result=0, image="0x99"), fsr_sample(),
            event("image_destroy_log_limit"), fsr_sample(),
            event("destroy_device"), event("create_device", result=0), event("create_image", result=0, image="0x99"), fsr_sample()]
        fsr = analyze(records)["fsrCallback"]
        links = [sample["bindings"][0] for sample in fsr["candidateSamples"]]
        self.assertEqual([link["imageId"] for link in links], [None, None, None, None, 2, None, None])
        self.assertTrue(links[4]["imageCreateLogLimitAlreadyReached"])
        self.assertEqual(links[5]["association"], "image_destroy_log_gap_lifetime_unresolved")
        self.assertEqual(links[6]["association"], "callback_context_device_destroyed")
        self.assertEqual(fsr["referencedImageLifetimes"][0]["generation"], 2)

    def test_read_failures_and_flags_do_not_become_valid_inputs(self):
        invalid = fsr_sample()
        invalid["bindings"][0]["native_status"] = "Unreadable"
        records = [event("create_device", result=0), event("create_image", result=0, image="0x99"),
            fsr_sample(status="ContextBoundsChanged", context_bounds_unchanged=False), invalid,
            fsr_sample(all_prepare_properties=False), fsr_sample(pid=None)]
        fsr = analyze(records)["fsrCallback"]
        self.assertTrue(all(s["bindings"][0]["imageId"] is None for s in fsr["candidateSamples"]))
        self.assertEqual(fsr["unscopedRecords"], 1)
        self.assertEqual(fsr["nativeStatusCountsInRetainedSamples"]["Unreadable"], 1)
        for key in ("snapshotAtomicVerified", "nativeIdDomainVerified", "vulkanImageVerified", "resourceSemanticsVerified",
                    "gameFrameInputsVerified", "pixelsRead", "pixelContentVerified", "xeFGIntegrated", "generatedFramesVerified"):
            self.assertFalse(fsr[key])

    def test_bounded_fields_samples_and_binding_arrays(self):
        analyzer = MODULE.Analyzer()
        analyzer.fsr_callback.limits["samplesPerContext"] = 1
        first = fsr_sample(); first["bindings"] *= 7
        first["payload"] = "x" * 300
        first["uptime_ms"] = {"oversized": "x" * 1000}
        records = [first, fsr_sample()]
        analyzer.parse(("\n".join(json.dumps(record) for record in records) + "\n").encode())
        fsr = analyzer.finish({"directory": str(Path.cwd()), "exists": True}, {})["fsrCallback"]
        self.assertEqual(len(fsr["candidateSamples"]), 1)
        self.assertEqual(len(fsr["candidateSamples"][0]["bindings"]), 5)
        self.assertTrue(fsr["candidateSamples"][0]["analyzerBindingsTruncated"])
        self.assertIsNone(fsr["candidateSamples"][0]["uptimeMs"])
        self.assertNotIn("payload", fsr["candidateSamples"][0])
        self.assertEqual(fsr["analyzerOmittedRecords"]["candidateSamples"], 1)
        self.assertEqual(fsr["analyzerOmittedRecords"]["bindings"], 2)


if __name__ == "__main__":
    unittest.main()
