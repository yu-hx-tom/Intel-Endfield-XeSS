#!/usr/bin/env python3
"""Synthetic logs: pass identity, opaque constants, quotas, legacy compatibility."""
import importlib.util
from pathlib import Path
import unittest

SPEC = importlib.util.spec_from_file_location("fixtures", Path(__file__).with_name("test-fsr-pe-inventory-analysis.py"))
F = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(F)


def sample(kind, name="r_current_interpolation_source", **fields):
    event_name = next(k for k, v in F.MODULE.FSR_PASS_EVENTS.items() if v == kind)
    value = F.candidate(event=event_name, all_pass_properties=True,
        all_prepare_properties=kind == "prepare_input",
        bindings=[{"property": name, "image_candidate": "0xA", "graph_status": "GraphTextureIdRead",
                   "native_status": "NativeCandidateReadUnverified", "width_candidate": 100, "height_candidate": 50}])
    value["pass"] = kind
    value.update(fields)
    return value


def match(kind, name="r_current_interpolation_source", **fields):
    value = F.inventory(property=name, **fields)
    value["pass"] = kind
    return value


def constants(**fields):
    value = dict(status="ConstantsSnapshotReadUnverified", byte_size=224, descriptor_read=True,
                 descriptor_unchanged=True, graph_bounds_unchanged=True, captured=True, raw_u32_bits=list(range(56)))
    value.update(fields)
    return value


class MultiPassFixtures(unittest.TestCase):
    def test_same_call_property_handle_is_scoped_by_pass_even_when_interleaved(self):
        result = F.analyze([match("frame_inpainting"), sample("reconstruct_previous_depth"),
                           sample("frame_inpainting"), match("reconstruct_previous_depth")])["fsrCallback"]
        self.assertEqual([v["candidateSampleId"] for v in result["imageMetadataMatches"]], [2, 1])
        self.assertFalse(result["gameFrameInputsVerified"])

    def test_legacy_missing_pass_cannot_match_new_pass(self):
        result = F.analyze([sample("prepare_input", "r_input_depth"), F.inventory(),
                           F.candidate(matched_call=2), match("prepare_input", "r_input_depth", matched_call=2)])
        self.assertTrue(all(v["candidateSampleId"] is None for v in result["fsrCallback"]["imageMetadataMatches"]))

    def test_unknown_malformed_or_event_mismatch_pass_is_ineligible(self):
        for value in ("unknown", "prepare_input", [], None):
            with self.subTest(value=value):
                s = sample("frame_inpainting")
                s["pass"] = value
                m = match("frame_inpainting")
                m["pass"] = value
                result = F.analyze([s, m])
                self.assertIsNone(result["fsrCallback"]["imageMetadataMatches"][0]["candidateSampleId"])
                self.assertIn("fsr_pass_identity_invalid", result["parsing"]["warningCounts"])

    def test_real_color_and_generated_output_remain_distinct_properties(self):
        result = F.analyze([sample("frame_inpainting", "r_present_backbuffer"),
                           match("frame_inpainting", "rw_output"), match("frame_inpainting", "r_present_backbuffer")])["fsrCallback"]
        self.assertIsNone(result["imageMetadataMatches"][0]["candidateSampleId"])
        self.assertEqual(result["imageMetadataMatches"][1]["candidateSampleId"], 1)

    def test_racing_parsed_pass_mismatch_does_not_link_images(self):
        result = F.analyze([sample("frame_inpainting", parsed_pass="reconstruct_previous_depth"),
                           match("frame_inpainting")])["fsrCallback"]
        self.assertIsNone(result["imageMetadataMatches"][0]["candidateSampleId"])
        self.assertEqual(result["candidateSamples"][0]["parsed_pass"], "reconstruct_previous_depth")

    def test_valid_constants_retained_as_exact_bits_without_semantic_flags(self):
        result = F.analyze([sample("frame_inpainting", constants_fi=constants(shader_semantics_verified=True))])
        value = result["fsrCallback"]["candidateSamples"][0]["constants_fi"]
        self.assertEqual(value["raw_u32_bits"], list(range(56)))
        self.assertTrue(value["rawSnapshotRetained"])
        self.assertFalse(value["shaderSemanticsVerified"])
        self.assertFalse(value["resourceLifetimePinned"])

    def test_invalid_constants_never_publish_partial_raw_data(self):
        for patch in ({"captured": False}, {"raw_u32_bits": [1] * 57}, {"raw_u32_bits": [1] * 55 + [True]},
                      {"raw_u32_bits": [1] * 55 + [0x100000000]}, {"status": "Unreadable"},
                      {"descriptor_unchanged": False}, {"graph_bounds_unchanged": False}, {"byte_size": 200}, {"byte_size": 224.0}):
            with self.subTest(patch=patch):
                result = F.analyze([sample("frame_inpainting", constants_fi=constants(**patch))])
                value = result["fsrCallback"]["candidateSamples"][0]["constants_fi"]
                self.assertNotIn("raw_u32_bits", value)
                self.assertFalse(value["rawSnapshotRetained"])

    def test_independent_pass_quota_and_summary_are_not_frame_counts(self):
        result = F.analyze([sample("frame_inpainting"), sample("frame_inpainting", matched_call=2),
                           sample("reconstruct_previous_depth"), F.event("fsr_callback_summary", callback_calls=90,
                           prepare_property_matches=5, reconstruct_property_matches=40, inpainting_property_matches=45,
                           prepare_resume_samples=1, reconstruct_resume_samples=2, inpainting_resume_samples=3,
                           prepare_refresh_attempts=22, prepare_refresh_parsed=21, prepare_refresh_native_candidates=105,
                           prepare_refresh_last_uptime_ms=50000)], samplesPerPass=1)["fsrCallback"]
        self.assertEqual(len(result["candidateSamples"]), 2)
        self.assertEqual(result["analyzerOmittedRecords"]["candidateSamples"], 1)
        self.assertEqual(result["contexts"][0]["counterMaxima"]["inpainting_property_matches"], 45)
        self.assertEqual(result["contexts"][0]["counterMaxima"]["prepare_refresh_attempts"], 22)

    def test_unknown_sets_are_separate_bounded_metadata(self):
        record = F.event("fsr_unknown_property_set", properties=[{"slot": n, "address": "0xa", "rva": None,
                       "property": "unknown"} for n in range(5)])
        result = F.analyze([record] * 10)["fsrCallback"]
        self.assertEqual(len(result["unknownPropertySets"]), 8)
        self.assertEqual(len(result["candidateSamples"]), 0)
        self.assertEqual(result["analyzerOmittedRecords"]["unknownPropertySets"], 2)


if __name__ == "__main__":
    unittest.main()
