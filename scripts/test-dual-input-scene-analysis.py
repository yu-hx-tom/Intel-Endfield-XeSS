#!/usr/bin/env python3
"""Small synthetic JSONL tests. No game, GPU, Vulkan loader or shared build."""
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.dont_write_bytecode = True
SPEC = importlib.util.spec_from_file_location("dual_scene", Path(__file__).with_name("analyze-dual-input-scene.py"))
M = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(M)


def event(name, **values):
    return {"event": name, "pid": 7, "device": "0xD", **values}


def match(prop=None, **values):
    return event("upscale_image_metadata_match", source="upscaler_prepare_e7a7b0", callback_call=1,
        property=prop or M.PROPERTIES[0], image_candidate="0xA", image_generation=30,
        match_status="live_handle_observed", numeric_live_handle_match=True, candidate_extent_matches=True,
        **values)


def candidate(**values):
    return event("image_sync_candidate", **{"role": "motion", "pass": "reconstruct_previous_depth",
        "accepted": True, "selectionGeneration": 42, "image": "0xa", "image_generation": 30, **values})


def snapshot(**values):
    flags = {key: True for key in ("snapshot_captured", "all_properties_present", "descriptors_unchanged",
        "graph_entries_unchanged", "binding_metadata_unchanged", "context_bounds_unchanged")}
    return event("upscale_source_snapshot", **{"source": "upscaler_prepare_e7a7b0", "callback_call": 1,
        "status": "SnapshotParsed", "bindings": [{"property": prop, "image_candidate": "0xA"} for prop in M.PROPERTIES],
        "graph_texture_ids_read": 8, "native_candidates_read": 8,
        **flags, **values})


def projection(**values):
    return event("rp_projection_submission", **{"role": "motion", "image": "0xa", "image_generation": 30,
        "selection_serial": 5, "source_token": 0, "pass_token": 0, "classification": "refused",
        "metadata_accepted": False, "driver_result": 0, "issues": 262148, **values})


def run_records(records, **limits):
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / "observer.jsonl"
        path.write_text("".join(json.dumps(record) + "\n" for record in records), encoding="utf-8")
        return M.analyze([path], **limits)


class DualInputFixtures(unittest.TestCase):
    def test_eight_rows_without_epoch_never_enable_snapshot_association(self):
        context = run_records([snapshot(), *[match(prop) for prop in M.PROPERTIES]])["contexts"][0]
        group = context["retained_upscale_callback_groups"][0]
        self.assertTrue(group["eight_numeric_matches_observed"])
        self.assertTrue(group["association_disabled_without_start"])
        self.assertFalse(group["snapshot_and_eight_metadata_rows_agree"])

    def test_summary_key_union_has_fixed_budget_across_different_records(self):
        records = [event("image_sync_start")]
        for row in range(10):
            records.append(event("rp_projection_summary", **{f"counter_{row}_{i}": i for i in range(90)}))
            records.append(event("rp_projection_summary", **{f"issues_{row}_{i}": i for i in range(90)}))
        records.append(event("rp_projection_summary", issues=4, counter_0_80=0))
        result = run_records(records)
        summary = result["contexts"][0]["summary_snapshots"]["rp_projection_summary"]
        self.assertEqual(len(summary["maxima"]) + len(summary["mask_unions"]), M.SUMMARY_KEY_BUDGET)
        self.assertLessEqual(len(summary["regressed_fields"]), M.SUMMARY_KEY_BUDGET)
        self.assertGreater(summary["numeric_field_observations_omitted"], 0)
        self.assertGreater(result["analyzer_omitted_or_rejected"]["summary_numeric_field_capacity"], 0)
        self.assertEqual(summary["latest"]["issues"], 4)

    def test_periodic_metadata_without_snapshot_is_retained_without_invented_agreement(self):
        context = run_records([event("image_sync_start"),
            *[{**match(prop), "callback_call": 1200, "sample_reason": "periodic_refresh"} for prop in M.PROPERTIES]])["contexts"][0]
        group = context["retained_upscale_callback_groups"][0]
        self.assertTrue(group["eight_numeric_matches_observed"])
        self.assertEqual(group["metadata_sample_reasons"], {"periodic_refresh": 8})
        self.assertFalse(group["snapshot_and_eight_metadata_rows_agree"])

    def test_start_then_create_is_one_epoch_and_id_overlap_is_only_historical(self):
        result = run_records([event("image_sync_start"), event("create_device", result=0),
            candidate(), snapshot(), *[match(prop) for prop in M.PROPERTIES]])
        self.assertEqual(len(result["contexts"]), 1)
        context = result["contexts"][0]
        self.assertTrue(context["retained_upscale_callback_groups"][0]["snapshot_and_eight_metadata_rows_agree"])
        self.assertEqual(len(context["historical_identity_overlaps"]), 1)
        self.assertEqual(context["historical_identity_overlaps"][0]["association"], "historical_same_inventory_identity_only")
        self.assertFalse(result["same_frame_verified"])
        self.assertFalse(result["undilated_verified"])
        self.assertFalse(result["copy_permitted"])

    def test_no_start_no_cross_process_device_generation_or_recreated_device_join(self):
        records = [candidate(), match(), event("image_sync_start"), candidate(),
            {**match(), "pid": 8}, {**match(), "device": "0xe"}, {**match(), "image_generation": 31},
            event("destroy_device"), match(), event("image_sync_start"), match()]
        result = run_records(records)
        self.assertTrue(all(not context["historical_identity_overlaps"] for context in result["contexts"]))
        self.assertEqual(result["analyzer_omitted_or_rejected"]["detail_after_device_destroy"], 1)

    def test_files_are_isolated_even_same_pid_device_and_counters(self):
        with tempfile.TemporaryDirectory() as directory:
            paths = [Path(directory) / str(i) for i in range(2)]
            for path, record in zip(paths, [candidate(), match()]):
                path.write_text(json.dumps(event("image_sync_start")) + "\n" + json.dumps(record) + "\n", encoding="utf-8")
            result = M.analyze(paths)
        self.assertTrue(all(not context["historical_identity_overlaps"] for context in result["contexts"]))

    def test_eight_row_match_requires_snapshot_flags_and_unique_properties(self):
        for malformed in (snapshot(descriptors_unchanged=False), snapshot(status="BindingRejected"),
            snapshot(bindings=[{"property": M.PROPERTIES[0], "image_candidate": "0xa"}] * 8),
            snapshot(source="different_callback")):
            result = run_records([event("image_sync_start"), malformed, *[match(p) for p in M.PROPERTIES]])
            group = result["contexts"][0]["retained_upscale_callback_groups"][0]
            self.assertTrue(group["eight_numeric_matches_observed"])
            self.assertFalse(group["snapshot_and_eight_metadata_rows_agree"])
        result = run_records([event("image_sync_start"), snapshot(), *[match(p) for p in M.PROPERTIES],
            {**match(), "image_generation": 31}])
        group = result["contexts"][0]["retained_upscale_callback_groups"][0]
        self.assertEqual(group["conflicting_properties"], [M.PROPERTIES[0]])
        self.assertFalse(group["snapshot_and_eight_metadata_rows_agree"])

    def test_named_input_is_not_undilated_and_counter_domains_never_pair_frames(self):
        records = [event("image_sync_start"), candidate(selectionGeneration=1),
            {**match(), "callback_call": 99}, projection(selection_serial=1, source_token=0, pass_token=0)]
        result = run_records(records)
        self.assertEqual(len(result["contexts"][0]["historical_identity_overlaps"]), 1)
        self.assertFalse(result["undilated_verified"])
        self.assertFalse(result["same_frame_verified"])
        self.assertFalse(result["gpu_completion_verified"])

    def test_literal_match_status_and_positive_generation_required(self):
        for altered in ({"numeric_live_handle_match": False}, {"match_status": "not_observed"},
                        {"image_generation": 0}, {"image_generation": True}, {"image_candidate": "0x0"}):
            result = run_records([event("image_sync_start"), candidate(), {**match(), **altered}])
            self.assertFalse(result["contexts"][0]["historical_identity_overlaps"])

    def test_projection_refusals_masks_and_bound_state_no_final_layout_claim(self):
        records = [event("image_sync_start"), projection(), projection(image="0x0", image_generation=0, selection_serial=0, issues=262196),
            projection(classification="no_direct_attachment_reference", metadata_accepted=True, issues=0),
            event("rp_projection_layout_operation", role="motion", image="0xa", image_generation=30,
                  source_kind="unused_initial_to_final_transition", old_layout=0, new_layout=5, journal_pass_count=32)]
        context = run_records(records)["contexts"][0]
        self.assertEqual(context["projection_sample_issue_names"], ["Capacity", "StaleGeneration", "UnknownObject", "IncompleteRenderPass"])
        self.assertEqual(context["observed_counts"]["projection_sample_binding_state"], {"bound": 2, "unbound_or_missing_identity": 1})
        self.assertEqual(context["historical_identities"][0]["projection_source_kinds"], {"motion:unused_initial_to_final_transition": 1})
        self.assertEqual(context["bounded_samples"][-1]["journal_pass_count"], 32)

    def test_summaries_keep_latest_max_mask_union_and_regressions_not_sum(self):
        records = [event("image_sync_start"),
            event("rp_projection_summary", accepted_slots=10, layout_operations=6, issues=4, omitted_summaries=900),
            event("rp_projection_summary", accepted_slots=8, layout_operations=2, issues=32, omitted_summaries=700)]
        summary = run_records(records)["contexts"][0]["summary_snapshots"]["rp_projection_summary"]
        self.assertEqual(summary["latest"]["accepted_slots"], 8)
        self.assertEqual(summary["maxima"]["accepted_slots"], 10)
        self.assertEqual(summary["mask_unions"]["issues"], 36)
        self.assertIn("accepted_slots", summary["regressed_fields"])
        self.assertEqual(summary["maxima"]["omitted_summaries"], 900)

    def test_wsi_common_generation_join_keeps_removed_count_nonidentity(self):
        records = [event("image_sync_start"), candidate(), match(), event("wsi_image_metadata", status="images_registered",
            added=1, removed=0, swapchain_generation=999, images=[{"origin": "swapchain_enumeration", "image": "0xa", "generation": 30}]),
            event("wsi_image_metadata", status="chain_destroyed", added=0, removed=1, images=[]),
            event("rp_swapchain_image_observation", image="0xa", image_generation=30, issues=128)]
        context = run_records(records)["contexts"][0]
        identity = context["historical_identities"][0]
        self.assertEqual(identity["wsi_identity_observations"], 1)
        self.assertEqual(identity["image_generation"], 30)
        self.assertEqual(identity["rp_wsi_decoded_issues"], ["UnsupportedImage"])
        self.assertEqual(len(context["historical_identity_overlaps"]), 1)
        self.assertNotIn("currently_live", identity)

    def test_limits_remain_explicit_and_summaries_continue_after_storage_cap(self):
        records = [event("image_sync_start"), candidate(), *[match(p) for p in M.PROPERTIES], projection(), projection(),
            event("rp_projection_summary", accepted_slots=123, omitted_operations=456)]
        result = run_records(records, max_records=1, max_samples=1)
        self.assertGreater(result["analyzer_omitted_or_rejected"]["callback_groups_capacity"], 0)
        self.assertGreater(result["analyzer_omitted_or_rejected"]["samples"], 0)
        self.assertEqual(result["contexts"][0]["summary_snapshots"]["rp_projection_summary"]["latest"]["accepted_slots"], 123)
        self.assertTrue(run_records(records, max_lines=2)["read_truncated"])
        result = run_records(records, max_bytes=20)
        self.assertEqual(result["inputs"][0]["bytes_read"], 20)
        self.assertTrue(result["read_truncated"])
        self.assertTrue(run_records(records, max_line_bytes=20)["read_truncated"])
        self.assertTrue(run_records([{}], max_line_bytes=2)["read_truncated"])

    def test_duplicate_json_keys_partial_lines_and_nonfinite_reject(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "observer.jsonl"
            path.write_bytes(b'{"event":"image_sync_start","pid":7,"pid":8,"device":"0xd"}\n'
                b'{"event":"upscale_source_snapshot","value":NaN}\n' + b'{"partial":')
            result = M.analyze([path])
        self.assertEqual(result["inputs"][0]["malformed_lines"], 3)
        self.assertEqual(result["inputs"][0]["unterminated_lines"], 1)
        self.assertFalse(result["contexts"])

    def test_malformed_nested_values_fail_closed_and_contexts_bounded(self):
        records = [event("image_sync_start"), {**match(), "property": []},
            event("fsr_image_metadata_match", property={}, pass_=[]), projection(role={}),
            event("image_sync_summary", current_roles=[{"role": {}}]),
            *[event("image_sync_start", device=hex(100+i)) for i in range(80)]]
        result = run_records(records)
        self.assertEqual(len(result["contexts"]), 64)
        self.assertGreater(result["analyzer_omitted_or_rejected"]["contexts_capacity"], 0)


if __name__ == "__main__":
    unittest.main()
