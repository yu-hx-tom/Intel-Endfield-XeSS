#!/usr/bin/env python3
"""Synthetic scope and bounded-reader regression; no live process or GPU."""
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.dont_write_bytecode = True
SPEC = importlib.util.spec_from_file_location("descriptor_scene", Path(__file__).with_name("analyze-descriptor-image-scene.py"))
M = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(M)


def event(name, **values):
    return {"event": name, "pid": 7, "device": "0xd", **{claim: False for claim in M.CLAIMS}, **values}


def group(**values):
    return event("descriptor_pre_dispatch", dispatch_call=1, command="0xa", command_generation=8,
                 record_ordinal=1, pipeline="0xb", pipeline_generation=9, metadata_accepted=True, issues=0,
                 binding_count=1, groups=[1, 1, 1], **values)


def binding(**values):
    return event("descriptor_image_binding", dispatch_call=1, command="0xa", command_generation=8,
                 record_ordinal=1, pipeline="0xb", pipeline_generation=9, image="0x10", image_generation=10,
                 view="0x11", view_generation=11, set="0x12", set_generation=12,
                 source_samples=[{"source_key": "raw_motion", "source_counter": 120}], **values)


def analyze(rows, **limits):
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / "observer.jsonl"
        path.write_text("".join(json.dumps(row) + "\n" for row in rows), encoding="utf-8")
        return M.analyze([path], **limits)


class DescriptorSceneTests(unittest.TestCase):
    def test_source_match_is_only_numeric_and_flags_are_unknown(self):
        result = analyze([event("create_device", result=0), group(), binding()])
        context = result["descriptor_contexts"][0]
        self.assertTrue(context["usable_binding_metadata_observed"])
        self.assertEqual(context["source_binding_matches"][0]["retained_binding_rows"], 1)
        self.assertEqual(context["command_begin_flags"], "unknown_not_emitted_by_observed_schema")
        self.assertFalse(result["same_frame_verified"])
        self.assertFalse(result["copy_permitted"])

    def test_unanchored_or_missing_header_never_qualifies_binding(self):
        for rows in ([group(), binding()], [event("create_device", result=0), binding()]):
            self.assertFalse(analyze(rows)["descriptor_contexts"][0]["usable_binding_metadata_observed"])

    def test_different_recording_generation_does_not_join(self):
        changed = binding()
        changed["command_generation"] = 90
        context = analyze([event("create_device", result=0), group(), changed])["descriptor_contexts"][0]
        self.assertFalse(context["usable_binding_metadata_observed"])

    def test_retention_and_file_reader_bounds(self):
        result = analyze([event("create_device", result=0), group(), binding(), group()], max_records=1)
        self.assertEqual(result["descriptor_omitted_or_rejected"]["detail_capacity"], 2)
        limited = analyze([event("create_device", result=0), group()], max_lines=1)
        self.assertTrue(limited["read_truncated"])
        self.assertEqual(limited["inputs"][0]["lines_read"], 1)

    def test_summary_issue_counts_overlap_and_source_description_requires_exact_generation(self):
        metadata = event("upscale_image_metadata_match", image_candidate="0x10", image_generation=10,
                         numeric_live_handle_match=True, format=83, width=31, height=19)
        summary = event("descriptor_image_summary", dispatch_calls=100, accepted_dispatches=0,
                        observed_issues=1152, fatal_issues=0, object_usage=[],
                        refused_issue_records=[{"issue_bit": 128, "count": 100}, {"issue_bit": 1024, "count": 90}],
                        source_samples=[{"source_key": "raw_motion", "image": "0x10", "image_generation": 11, "source_counter": 240}])
        context = analyze([event("create_device", result=0), metadata, summary])["descriptor_contexts"][0]
        self.assertEqual(context["latest_summary"]["observed_issue_names"], ["UnsupportedDescriptor", "UndefinedDescriptor"])
        self.assertIsNone(context["latest_source_samples"][0]["historical_description_for_exact_inventory_identity"])

    def test_template_summary_domain_and_capacity_are_retained_without_copy_claim(self):
        usages = [{"kind": "kind" + str(i), "live": 1, "capacity": 9, "capacity_refusals": 0} for i in range(9)]
        usages.append({"kind": "descriptor_sets", "live": 8, "capacity": 8, "capacity_refusals": 3})
        summary = event("descriptor_image_summary", dispatch_calls=10, capacity_refusals=3,
                        template_update_refusals=3, observed_issues=1 << 21, fatal_issues=0,
                        object_usage=usages, source_samples=[], refused_issue_records=[],
                        template_refused_issue_records=[{"issue_bit": 1 << 21, "count": 3}])
        result = analyze([event("create_device", result=0), summary])
        context = result["descriptor_contexts"][0]
        self.assertEqual(len(context["latest_summary"]["object_usage"]), 10)
        self.assertEqual(context["latest_summary"]["template_refused_issue_records"][0]["issue_names"], ["TemplateReadFailed"])
        self.assertEqual(context["object_observations"]["descriptor_sets"]["first_capacity_refusal"]["live"], 8)
        self.assertIsNotNone(context["first_template_refusal_summary"])
        self.assertFalse(result["copy_permitted"])


if __name__ == "__main__":
    unittest.main()
