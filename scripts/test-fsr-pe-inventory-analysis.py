#!/usr/bin/env python3
"""In-memory fixtures only: PE diagnostic bounds and runtime inventory metadata."""
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


def candidate(**fields):
    record = event("fsr_prepare_input_candidate", matched_call=1, status="SnapshotParsed", all_prepare_properties=True,
        context_bounds_unchanged=True, bindings=[{"property": "r_input_depth", "image_candidate": "0xA",
        "graph_status": "GraphTextureIdRead", "native_status": "NativeCandidateReadUnverified", "width_candidate": 100, "height_candidate": 50}])
    record.update(fields)
    return record


def inventory(**fields):
    record = event("fsr_image_metadata_match", matched_call=1, property="r_input_depth", image_candidate="0xa",
        match_status="live_handle_observed", numeric_live_handle_match=True, inventory_active_count=10, image_generation=93,
        format=100, width=100, height=50, depth=1, mip_levels=1, array_layers=1, samples=1, usage=7, flags=0,
        tiling=0, sharing_mode=0, initial_layout=0, candidate_extent_matches=True,
        resource_lifetime_pinned=False, synchronization_verified=False, pixel_semantics_verified=False, game_frame_inputs_verified=False)
    record.update(fields)
    return record


def analyze(records, **limits):
    analyzer = MODULE.Analyzer()
    analyzer.fsr_callback.limits.update(limits)
    analyzer.parse(("\n".join(json.dumps(r) for r in records) + "\n").encode())
    return analyzer.finish({"directory": str(Path.cwd()), "exists": True}, {})


class PeAndInventoryFixtures(unittest.TestCase):
    def test_pe_failure_detail_survives_without_becoming_success(self):
        result = analyze([event("fsr_observer_install", state="refused_pe_live_headers", pe_guard={
            "attempted": True, "passed": False, "state": "refused_pe_live_headers", "source": "live", "stage": "nt", "reason": "read_failed",
            "failed_read": {"operation": "ReadProcessMemory", "api_called": True, "api_succeeded": False,
                "win32_error": 299, "offset": 344, "requested_bytes": 264, "actual_bytes": 7},
            "live": {"dos_read": True, "nt_read": False}, "nt_first_differences": [],
            "comparison_performed": False, "unknown_payload": "do not retain"})])
        pe = result["fsrCallback"]["contexts"][0]["firstInstallationRecord"]["pe_guard"]
        self.assertEqual(pe["failed_read"]["win32_error"], 299)
        self.assertEqual(pe["failed_read"]["actual_bytes"], 7)
        self.assertFalse(pe["live"]["nt_read"])
        self.assertNotIn("unknown_payload", pe)
        self.assertFalse(result["fsrCallback"]["gameFrameInputsVerified"])

    def test_pe_diff_cap_malformed_nested_and_oversized_fields(self):
        result = analyze([event("fsr_observer_install_summary", pe_guard={"state": "refused_pe_nt_mismatch",
            "nt_first_differences": [{"field": "OptionalHeader.ImageBase", "nt_offset": i, "disk_byte": 0, "live_byte": 1} for i in range(100)],
            "failed_read": [], "live": {"machine": "x" * 300}})])
        pe = result["fsrCallback"]["contexts"][0]["lastInstallationRecord"]["pe_guard"]
        self.assertEqual(len(pe["nt_first_differences"]), 16)
        self.assertTrue(pe["analyzerDifferencesTruncated"])
        self.assertNotIn("failed_read", pe)
        self.assertNotIn("machine", pe["live"])
        self.assertIn("fsr_pe_guard_nested_invalid", result["parsing"]["warningCounts"])

    def test_exact_base_compatibility_metadata_preserves_original_differences(self):
        actual = 140707188965376
        result = analyze([event("fsr_observer_install", pe_guard={"state": "passed", "passed": True,
            "reason": "validated_headers_with_actual_module_image_base", "actual_module_base": actual,
            "live_image_base_matches_actual_module_base": True, "nt_differences_outside_image_base": 0,
            "image_base_normalization_eligible": True, "image_base_normalization_applied": True,
            "nt_different_bytes": 4, "nt_first_differences": [{"field": "OptionalHeader.ImageBase", "nt_offset": 50,
                "disk_byte": 0, "live_byte": 4}], "disk": {"image_base": 6442450944}, "live": {"image_base": actual}})])
        pe = result["fsrCallback"]["contexts"][0]["firstInstallationRecord"]["pe_guard"]
        self.assertEqual(pe["actual_module_base"], actual)
        self.assertTrue(pe["image_base_normalization_applied"])
        self.assertEqual(pe["nt_different_bytes"], 4)
        self.assertEqual(pe["nt_first_differences"][0]["disk_byte"], 0)
        self.assertEqual(pe["disk"]["image_base"], 6442450944)
        self.assertFalse(result["fsrCallback"]["gameFrameInputsVerified"])
        self.assertFalse(result["fsrCallback"]["xeFGIntegrated"])

    def test_inventory_links_without_fabricating_create_lifetime(self):
        result = analyze([candidate(), inventory()])
        fsr = result["fsrCallback"]
        self.assertEqual(result["vulkan"]["images"]["observedLifetimes"], 0)
        item = fsr["imageMetadataMatches"][0]
        self.assertTrue(item["inventoryNumericMatchObserved"])
        self.assertEqual(item["candidateSampleId"], 1)
        self.assertEqual(item["candidateBindingIndex"], 0)
        self.assertEqual(item["image_generation"], 93)
        self.assertEqual(fsr["candidateSamples"][0]["bindings"][0]["imageMetadataMatchIds"], [1])
        self.assertFalse(fsr["candidateSamples"][0]["bindings"][0]["vulkanImageVerified"])
        self.assertFalse(item["resourceLifetimePinned"])
        self.assertFalse(fsr["generatedFramesVerified"])

    def test_interleaved_log_order_and_explicit_device_scoping(self):
        result = analyze([inventory(), candidate(), inventory(device="0xe"), inventory(pid=8)])
        values = result["fsrCallback"]["imageMetadataMatches"]
        self.assertEqual(values[0]["candidateSampleId"], 1)
        self.assertIsNone(values[1]["candidateSampleId"])
        self.assertIsNone(values[2]["candidateSampleId"])

    def test_missing_or_ambiguous_candidate_is_not_guessed(self):
        result = analyze([candidate(), candidate(), inventory(), inventory(matched_call=2), inventory(property="r_input_motion_vectors")])
        values = result["fsrCallback"]["imageMetadataMatches"]
        self.assertEqual(values[0]["candidateAssociation"], "ambiguous_matched_call_in_context")
        self.assertIsNone(values[1]["candidateSampleId"])
        self.assertIsNone(values[2]["candidateSampleId"])

    def test_retired_inventory_and_forged_semantic_flags_never_verify(self):
        result = analyze([candidate(), inventory(match_status="device_session_retired", numeric_live_handle_match=False),
            inventory(game_frame_inputs_verified=True, pixel_semantics_verified=True, resource_lifetime_pinned=True)])
        fsr = result["fsrCallback"]
        self.assertFalse(fsr["imageMetadataMatches"][0]["inventoryNumericMatchObserved"])
        second = fsr["imageMetadataMatches"][1]
        for name in ("gameFrameInputsVerified", "pixelSemanticsVerified", "resourceLifetimePinned", "synchronizationVerified"):
            self.assertFalse(second[name])
        self.assertFalse(fsr["xeFGIntegrated"])

    def test_inconsistent_numeric_match_or_after_destroy_not_accepted(self):
        result = analyze([candidate(), inventory(image_generation=0), inventory(match_status="not_observed"),
            inventory(width=True), event("destroy_device"), inventory()])
        self.assertTrue(all(not x["inventoryNumericMatchObserved"] for x in result["fsrCallback"]["imageMetadataMatches"]))
        self.assertIn("fsr_numeric_inventory_match_inconsistent_or_after_destroy", result["parsing"]["warningCounts"])

    def test_new_metadata_record_cap_is_separate_from_candidate_cap(self):
        result = analyze([candidate(), inventory(), inventory(), inventory(), candidate(matched_call=2)],
                         imageMetadataMatchesPerContext=2)
        fsr = result["fsrCallback"]
        self.assertEqual(len(fsr["imageMetadataMatches"]), 2)
        self.assertEqual(len(fsr["candidateSamples"]), 2)
        self.assertEqual(fsr["analyzerOmittedRecords"]["imageMetadataMatches"], 1)

    def test_missing_pid_and_bad_nested_input_remain_bounded(self):
        result = analyze([inventory(pid=None), event("fsr_observer_install", pe_guard="invalid"),
            event("fsr_observer_install_summary", pe_guard={"nt_first_differences": [None], "failed_read": None})])
        self.assertEqual(result["fsrCallback"]["unscopedRecords"], 1)
        self.assertEqual(result["fsrCallback"]["imageMetadataMatches"], [])
        self.assertIn("fsr_pe_guard_invalid", result["parsing"]["warningCounts"])


if __name__ == "__main__":
    unittest.main()
