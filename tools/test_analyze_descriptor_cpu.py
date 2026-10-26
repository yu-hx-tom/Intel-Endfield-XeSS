import importlib.util
import json
import tempfile
import unittest
from pathlib import Path

spec = importlib.util.spec_from_file_location("descriptor_cpu", Path(__file__).with_name("analyze-descriptor-cpu.py"))
analysis = importlib.util.module_from_spec(spec)
spec.loader.exec_module(analysis)


class AnalysisTests(unittest.TestCase):
    def test_calibrated_cycles_use_only_matching_valid_windows(self):
        windows = [dict(event="thread_cpu_window", pid=1, sequence=seq, begin_qpc=seq*10,
            end_qpc=seq*10+10, qpc_frequency=10, enumerated=1, collection_cpu_ns=0, collection_wall_ns=0,
            cycle_calibration_valid=(seq == 1), tsc_hz=3e9, observed_cycles="3000000000",
            process_cycles_known=True, process_cycles="3600000000") for seq in (1, 3)]
        rows = windows + [dict(event="thread_cpu_thread", pid=1, sequence=seq, thread_id=7,
            cpu_known=True, user_ns=0, kernel_ns=0, cycles_known=True, cycles="3000000000") for seq in (1, 2, 3)]
        result = analysis.summarize(rows, 0, 50, 10)
        cycles = result["cycle_runtime"]
        self.assertEqual(cycles["report_windows"], 1)
        self.assertEqual(cycles["retained_estimated_cores"], 1)
        self.assertAlmostEqual(cycles["process_estimated_cores"], 1.2)
        self.assertAlmostEqual(cycles["observed_to_process_cycle_ratio"], 1/1.2)
        self.assertEqual(result["threads"]["by_thread"][0]["cores"], 0)
        self.assertEqual(result["threads"]["primary_metric"], "cycle_runtime")

    def test_old_capture_is_unavailable(self):
        result = analysis.summarize([{"event": "measurement_summary"}], 0, 100, 10)
        self.assertFalse(result["tracking"]["available"])
        self.assertFalse(result["threads"]["available"])
        self.assertFalse(result["cycle_runtime"]["available"])

    def test_consumption_pending_and_identity_share(self):
        def row(at, retired, consumed, unused, bind, query):
            return dict(event="tracking_cpu_window", device="0x1", sequence=at, begin_qpc=at-10, end_qpc=at, qpc_frequency=10,
                        reference_consumption_measured=True, reference_mask_retired_builds=retired,
                        reference_mask_consumed_builds=consumed, reference_mask_unconsumed_builds=unused,
                        reference_mask_pending_builds=7, reference_mask_builds=retired+7,
                        cpu_tracking_bind=dict(count=at, total_ns=bind, max_ns=1), reference_query_bind_total_ns=query)
        result = analysis.summarize([row(20, 10, 4, 6, 100, 30), row(40, 30, 9, 21, 300, 80)], 10, 50, 10)["tracking"]
        self.assertEqual(result["consumption"]["unconsumed_fraction"], 0.75)
        self.assertEqual(result["consumption"]["pending_at_end"], 7)
        self.assertEqual(result["queries"]["bind"]["share_of_bind"], 0.25)

    def test_thread_groups_use_window_time_once(self):
        window = dict(event="thread_cpu_window", pid=1, sequence=1, begin_qpc=10, end_qpc=60, qpc_frequency=10,
                      enumerated=2, collection_cpu_ns=1000, collection_wall_ns=2000)
        rows = [window]
        for tid in (3, 4):
            rows.append(dict(event="thread_cpu_thread", pid=1, sequence=1, thread_id=tid, creation_filetime=str(tid),
                             cpu_known=True, start_module="worker.dll", thread_name="worker", user_ns=1_000_000_000, kernel_ns=0))
        result = analysis.summarize(rows, 0, 70, 10)["threads"]
        self.assertTrue(result["row_count_matches"])
        self.assertAlmostEqual(result["by_origin_and_name"][0]["cores"], 0.4)
        self.assertEqual(len(result["by_thread"]), 2)
        window["enumerated"] = 3
        self.assertFalse(analysis.summarize(rows, 0, 70, 10)["threads"]["row_count_matches"])

    def test_duplicate_snapshots_and_corrupt_tail(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            row = dict(event="thread_cpu_window", pid=1, sequence=1)
            text = json.dumps(row) + '\n{"event":"thread_cpu_window"'
            (root / "measurements-summary-0.stream.jsonl").write_text(text)
            (root / "measurements-summary-0.snapshot.jsonl").write_text(text)
            self.assertEqual(list(analysis.records(root)), [row])

    def test_group_identity_and_missing_window_are_not_silently_lost(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            groups = [dict(event="thread_cpu_group", pid=1, sequence=1, start_module="a.dll", thread_name=name)
                      for name in ("one", "two")]
            (root / "measurements-summary-0.stream.jsonl").write_text("\n".join(json.dumps(row) for row in groups))
            self.assertEqual(list(analysis.records(root)), groups)
        windows = [dict(event="thread_cpu_window", pid=1, sequence=seq, begin_qpc=seq*10,
                        end_qpc=seq*10+10, qpc_frequency=10, enumerated=2,
                        collection_cpu_ns=0, collection_wall_ns=0, observed_user_ns=100, observed_kernel_ns=0)
                   for seq in (1, 3)]
        result = analysis.summarize(windows, 0, 50, 10)["threads"]
        self.assertEqual(result["missing_window_sequences"], [dict(pid=1, generation=0, sequence=2)])
        self.assertEqual(result["cpu_time_coverage"], 0)


if __name__ == "__main__":
    unittest.main()
