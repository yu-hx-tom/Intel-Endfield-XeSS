#!/usr/bin/env python3
"""Synthetic metadata fixtures only; no game files, process, or Vulkan calls."""
import importlib.util
import json
from pathlib import Path
import sys
import unittest

sys.dont_write_bytecode = True
SPEC = importlib.util.spec_from_file_location("observer_analysis", Path(__file__).with_name("analyze-observer-session.py"))
M = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(M)


def event(name, **values):
    return {"event": name, "pid": 7, "device": "0xD", "uptime_ms": 100, "thread_id": 9, **values}


def identity(role="motion", **values):
    return {"role": role, "pass": M.IMAGE_SYNC_ROLES.get(role) if isinstance(role, str) else None, "selectionGeneration": 12,
            "image": "0xA", "image_generation": 30, **values}


def candidate(role="motion", **values):
    return event("image_sync_candidate", **identity(role, accepted=True, previous_selection_generation=11,
        format=83, width=1920, height=1080, **values))


def submit(role="motion", **values):
    return event("image_sync_submit_evidence", **identity(role, **{
        "selection_is_current": True, "candidate_submission_count": 600, "submission_serial": 33,
        "command_generation": 2, "command": "0xC", "queue": "0xB", "queue_family": 0,
        "fence": "0x0", "old_layout": 1, "new_layout": 5, "layout_fields_ignored": False,
        "submitted_layout_known": False, "issues": 8, "recording_issues": 4, "selected_before_begin": False,
        "src_queue_family": 0xffffffff, "dst_queue_family": 0xffffffff,
        "src_stages": 0x100000000, "dst_stages": 2, "src_access": 0x200000000, "dst_access": 4,
        "aspect_mask": 1, "base_mip": 0, "level_count": 1, "base_layer": 0, "layer_count": 1, **values}))


def analyze(records, **limits):
    a = M.Analyzer()
    a.image_sync.limits.update(limits)
    a.parse(("\n".join(json.dumps(r) for r in records) + "\n").encode())
    return a.finish({"directory": str(Path.cwd()), "exists": True}, {})


class ImageSyncFixtures(unittest.TestCase):
    def test_exact_selection_scope_preserves_full_raw_barrier(self):
        result = analyze([event("image_sync_start"), candidate(), candidate("depth"),
            submit(), submit("depth")])
        sync = result["imageSync"]
        self.assertEqual([v["candidateRecordId"] for v in sync["submissionEvidence"]], [1, 2])
        first = sync["submissionEvidence"][0]
        self.assertEqual((first["pid"], first["device"], first["contextId"]), (7, "0xd", 1))
        self.assertEqual((first["selectionGeneration"], first["image_generation"]), (12, 30))
        self.assertEqual(first["src_stages"], 0x100000000)
        self.assertEqual(first["src_access"], 0x200000000)
        self.assertEqual(first["fence"], "0x0")
        self.assertEqual(first["candidateLogOrder"], "preceding")
        self.assertNotIn("unknown_event", result["parsing"]["warningCounts"])

    def test_image_generation_handle_and_selection_mismatch_never_link(self):
        sync = analyze([event("image_sync_start"), candidate(), submit(image_generation=31),
            submit(image="0xe"), submit(selectionGeneration=13)])["imageSync"]
        self.assertEqual([v["candidateAssociation"] for v in sync["submissionEvidence"]],
            ["image_identity_mismatch", "image_identity_mismatch", "no_retained_candidate_record"])
        self.assertTrue(all(v["candidateRecordId"] is None for v in sync["submissionEvidence"]))

    def test_pid_device_and_new_start_are_separate_contexts(self):
        records = [event("image_sync_start"), candidate(), event("image_sync_start", pid=8),
            submit(pid=8), event("image_sync_start", device="0xe"), submit(device="0xe"),
            event("image_sync_start"), submit()]
        sync = analyze(records)["imageSync"]
        self.assertEqual(len(sync["contexts"]), 4)
        self.assertEqual([v["contextId"] for v in sync["submissionEvidence"]], [2, 3, 4])
        self.assertTrue(all(v["candidateRecordId"] is None for v in sync["submissionEvidence"]))
        self.assertEqual(sync["contexts"][0]["supersededAtLine"], 7)

    def test_destroy_blocks_details_until_new_explicit_start(self):
        sync = analyze([event("image_sync_start"), candidate(), event("destroy_device"), submit(),
            event("image_sync_summary", retired=True), event("image_sync_start"), submit()])["imageSync"]
        self.assertEqual(sync["analyzerOmittedRecords"]["detailAfterDeviceDestroy"], 1)
        self.assertEqual(len(sync["submissionEvidence"]), 1)
        self.assertIsNone(sync["submissionEvidence"][0]["candidateRecordId"])
        self.assertTrue(sync["contexts"][0]["lastSummary"]["retired"])

    def test_missing_start_or_explicit_scope_is_not_inferred(self):
        sync = analyze([candidate(), submit(), submit(pid=None), submit(device="not-a-handle")])["imageSync"]
        self.assertEqual(sync["unscopedRecords"], 2)
        self.assertEqual(sync["submissionEvidence"][0]["candidateAssociation"], "context_start_not_observed")

    def test_duplicate_rejected_and_invalid_role_records_do_not_link(self):
        rejected = candidate(); rejected["accepted"] = False
        records = [event("image_sync_start"), candidate(), candidate(), submit(),
                   rejected, submit(selectionGeneration=0), submit(role=["motion"])]
        # A malformed role may have no valid pass at all.
        records[-1]["pass"] = "reconstruct_previous_depth"
        sync = analyze(records)["imageSync"]
        self.assertEqual(sync["submissionEvidence"][0]["candidateAssociation"], "ambiguous_selection_generation")
        self.assertTrue(all(v["candidateRecordId"] is None for v in sync["submissionEvidence"]))
        only_rejected = analyze([event("image_sync_start"), rejected, submit()])["imageSync"]
        self.assertEqual(only_rejected["submissionEvidence"][0]["candidateAssociation"], "candidate_not_accepted")

    def test_late_candidate_notice_links_metadata_without_inventing_order_or_frame(self):
        sync = analyze([event("image_sync_start"), submit(), candidate()])["imageSync"]
        self.assertEqual(sync["submissionEvidence"][0]["candidateRecordId"], 1)
        self.assertEqual(sync["submissionEvidence"][0]["candidateLogOrder"], "following")
        self.assertFalse(sync["sameFrameVerified"])
        self.assertFalse(sync["gpuComplete"])

    def test_last_summary_keeps_roles_counter_maxima_and_issue_union(self):
        current = identity("real_color", candidate_submissions=3, latest_barrier_submission=23,
            command_generation=4, barrier_submitted=True, queue="0xB", queue_family=2,
            latest_device_submission=30, recording_issues=7, selected_before_begin=False,
            raw_old_layout=1, raw_new_layout=5, issues=8)
        records = [event("image_sync_start", tracker_storage_bytes=3000, candidate_detail_limit=256),
            candidate("real_color"), event("image_sync_summary", selection_changes=3, successful_submissions=99,
                issues=2, real_color_submission_details=4, current_roles=[]),
            event("image_sync_summary", selection_changes=2, successful_submissions=77, issues=4,
                retired=True, current_roles=[current, identity("motion", selectionGeneration=0, image="0x0", image_generation=0)])]
        sync = analyze(records)["imageSync"]
        ctx = sync["contexts"][0]
        self.assertEqual(ctx["summarySnapshotCount"], 2)
        self.assertEqual(ctx["counterMaxima"]["successful_submissions"], 99)
        self.assertEqual(ctx["observedIssueMaskOR"], 14)
        role = ctx["lastSummary"]["current_roles"][0]
        self.assertEqual(role["candidateRecordId"], 1)
        self.assertEqual(role["latest_device_submission"], 30)
        self.assertEqual(role["queue"], "0xb")
        self.assertFalse(ctx["lastSummary"]["current_roles"][1]["selectionIdentityValid"])

    def test_per_role_and_context_detail_caps_keep_counts_and_summaries(self):
        records = [event("image_sync_start"), candidate(), candidate("depth"), candidate("real_color"),
            submit(), submit(), submit("depth"), submit("real_color"),
            event("image_sync_summary", candidate_details_omitted=900, motion_submission_details_omitted=800,
                current_roles=[identity()]*10)]
        sync = analyze(records, candidatesPerContext=2, submissionsPerRole=1, submissionsPerContext=2)["imageSync"]
        self.assertEqual(len(sync["candidateRecords"]), 2)
        self.assertEqual(len(sync["submissionEvidence"]), 2)
        self.assertEqual(sync["analyzerOmittedRecords"]["candidates"], 1)
        self.assertEqual(sync["analyzerOmittedRecords"]["submissions"], 2)
        self.assertEqual(sync["analyzerOmittedRecords"]["currentRoles"], 7)
        ctx = sync["contexts"][0]
        self.assertEqual(ctx["submissionRecordsObserved"], 4)
        self.assertEqual(ctx["counterMaxima"]["candidate_details_omitted"], 900)
        self.assertTrue(all(v["candidateRecordId"] is None for v in ctx["lastSummary"]["current_roles"]))

    def test_global_caps_and_context_exhaustion_cannot_resume_old_context(self):
        sync = analyze([event("image_sync_start"), candidate(), submit(), event("image_sync_start"),
            candidate(), submit()], contexts=1)["imageSync"]
        self.assertEqual(len(sync["contexts"]), 1)
        self.assertEqual(sync["analyzerOmittedRecords"]["contextRecords"], 3)
        self.assertEqual(len(sync["submissionEvidence"]), 1)
        sync = analyze([event("image_sync_start"), candidate(), submit(),
            event("image_sync_start", device="0xe"), candidate(device="0xe"), submit(device="0xe")],
            candidates=1, submissions=1)["imageSync"]
        self.assertEqual(sync["analyzerOmittedRecords"]["candidates"], 1)
        self.assertEqual(sync["analyzerOmittedRecords"]["submissions"], 1)

    def test_untrusted_claims_and_nested_fields_never_become_verification(self):
        claims = dict.fromkeys(M.IMAGE_SYNC_FALSE_FIELDS, True)
        bad = submit(**claims, issues=[1, 2], queue="x"*1000, command_generation=True)
        summary = event("image_sync_summary", **claims, successful_submissions=-1,
            current_roles=[identity(**claims, candidate_submissions={"wrong": 1})])
        result = analyze([event("image_sync_start", **claims), candidate(), bad, summary])
        sync = result["imageSync"]
        item = sync["submissionEvidence"][0]
        self.assertTrue(all(item[k] is False for k in M.IMAGE_SYNC_FALSE_FIELDS))
        self.assertNotIn("issues", item)
        self.assertNotIn("queue", item)
        self.assertNotIn("command_generation", item)
        self.assertFalse(sync["copyPermitted"])
        self.assertFalse(sync["lastWriterKnown"])
        self.assertIn("image_sync_unsupported_verification_claim", result["parsing"]["warningCounts"])

    def test_malformed_summary_arrays_and_id_types_are_bounded(self):
        records = [event("image_sync_start"), candidate(selectionGeneration=True), submit(image_generation="30"),
            event("image_sync_summary", current_roles=[None, [], {"role": "depth", "pass": "wrong"}]),
            event("image_sync_summary", current_roles={"not": "an array"})]
        result = analyze(records)
        sync = result["imageSync"]
        self.assertFalse(sync["candidateRecords"][0]["selectionIdentityValid"])
        self.assertFalse(sync["submissionEvidence"][0]["selectionIdentityValid"])
        self.assertEqual(sync["contexts"][0]["lastSummary"]["current_roles"], [])
        self.assertIn("image_sync_current_roles_invalid", result["parsing"]["warningCounts"])


if __name__ == "__main__":
    unittest.main()
