"""Analyze saved recorder data only; never opens a game or starts a collector.

Usage: python -I tools/analyze-descriptor-cpu.py <capture-directory>
New counters are unavailable in older recordings, never inferred as zero.
"""
import argparse
import json
from collections import defaultdict
from pathlib import Path


def records(directory):
    seen = set()
    for path in sorted(directory.glob("measurements-summary-*.jsonl")):
        with path.open("rb") as source:
            for raw in source:
                if b'"tracking_cpu_window"' not in raw and b'"thread_cpu_' not in raw:
                    continue
                try:
                    row = json.loads(raw)
                except ValueError:
                    continue
                key = (row.get("event"), row.get("pid"), row.get("device"), row.get("sampler_generation", 0), row.get("sequence"),
                       row.get("thread_id"), row.get("creation_filetime"),
                       row.get("start_module") if row.get("event") == "thread_cpu_group" else None,
                       row.get("thread_name") if row.get("event") == "thread_cpu_group" else None)
                if key not in seen:
                    seen.add(key)
                    yield row


def summarize(rows, start, end, frequency):
    result = {"schema": 1, "attribution": "thread origin; not sampled execution stacks"}
    def inside(row):
        freq = row.get("qpc_frequency", frequency)
        return (freq > 0 and row.get("begin_qpc", 0) > 0 and
                start / frequency <= row["begin_qpc"] / freq < row.get("end_qpc", 0) / freq <= end / frequency)
    rows = list(rows)
    tracking = sorted((r for r in rows if r.get("event") == "tracking_cpu_window" and inside(r)), key=lambda r: r["end_qpc"])
    devices = {(r.get("pid"), r.get("device")) for r in tracking}
    result["tracking"] = {"available": False, "report_windows": len(tracking)}
    if len(tracking) >= 2 and len(devices) == 1:
        first, last = tracking[0], tracking[-1]
        seconds = last["end_qpc"] / last["qpc_frequency"] - first["end_qpc"] / first["qpc_frequency"]
        def delta(name):
            a, b = first.get(name), last.get(name)
            return b - a if isinstance(a, (int, float)) and isinstance(b, (int, float)) and b >= a else None
        tracking_result = {"available": True, "seconds": seconds, "report_windows": len(tracking),
                           "mask_policy": last.get("reference_mask_policy")}
        retired, consumed, unused = (delta("reference_mask_" + key + "_builds") for key in ("retired", "consumed", "unconsumed"))
        measured = all(r.get("reference_consumption_measured") is True for r in tracking)
        tracking_result["consumption"] = {"measured": measured, "retired_builds": retired, "consumed_builds": consumed,
                                           "unconsumed_builds": unused, "pending_at_end": last.get("reference_mask_pending_builds"),
                                           "unconsumed_fraction": unused / retired if measured and retired and unused is not None else None}
        tracking_result["builds_per_second"] = delta("reference_mask_builds") / seconds if delta("reference_mask_builds") is not None else None
        tracking_result["reference_edge_overflows"] = delta("reference_edge_overflows")
        groups = {}
        for key in last:
            if key.startswith("cpu_tracking_") and isinstance(last[key], dict):
                a, b = first.get(key, {}), last[key]
                ns = b.get("total_ns", 0) - a.get("total_ns", 0)
                calls = b.get("count", 0) - a.get("count", 0)
                groups[key] = {"ms_per_second": ns / 1e6 / seconds, "calls_per_second": calls / seconds,
                               "ns_per_call": ns / calls if calls else None}
        tracking_result["groups"] = groups
        queries = {}
        for role in ("bind", "draw"):
            prefix = "reference_query_" + role + "_"
            values = {key: delta(prefix + key) for key in ("count", "sets", "contended", "total_ns", "wait_ns")}
            values["ms_per_second"] = values["total_ns"] / 1e6 / seconds if values["total_ns"] is not None else None
            if role == "bind":
                bind_ns = last.get("cpu_tracking_bind", {}).get("total_ns", 0) - first.get("cpu_tracking_bind", {}).get("total_ns", 0)
                values["share_of_bind"] = values["total_ns"] / bind_ns if bind_ns and values["total_ns"] is not None else None
            queries[role] = values
        tracking_result["queries"] = queries
        hits, misses = delta("identity_token_cache_hits"), delta("identity_token_cache_misses")
        tracking_result["identity_token_cache"] = {"hits": hits, "misses": misses,
            "hit_fraction": hits/(hits+misses) if hits is not None and misses is not None and hits+misses else None}
        peaks = []
        for row in tracking:
            for key, value in row.items():
                if key.startswith("cpu_tracking_") and isinstance(value, dict) and value.get("max_ns", 0):
                    peaks.append({"group": key, "max_ns": value["max_ns"], "end_qpc": row.get(key + "_peak_qpc"),
                                  "thread": row.get(key + "_peak_thread"), "window_sequence": row["sequence"]})
        tracking_result["largest_window_peaks"] = sorted(peaks, key=lambda p: p["max_ns"], reverse=True)[:20]
        result["tracking"] = tracking_result
    windows = {(r.get("pid"), r.get("sampler_generation", 0), r["sequence"]): r for r in rows
               if r.get("event") == "thread_cpu_window" and not r.get("baseline") and inside(r)}
    result["threads"] = {"available": False, "report_windows": len(windows)}
    if windows:
        seconds = sum((r["end_qpc"] - r["begin_qpc"]) / r["qpc_frequency"] for r in windows.values())
        grouped, individual = defaultdict(lambda: [0, 0]), defaultdict(lambda: [0, 0])
        row_count = defaultdict(int)
        retained_cpu = 0
        for row in rows:
            identity = (row.get("pid"), row.get("sampler_generation", 0), row.get("sequence"))
            if row.get("event") != "thread_cpu_thread" or identity not in windows:
                continue
            row_count[identity] += 1
            if not row.get("cpu_known"):
                continue
            retained_cpu += row["user_ns"] + row["kernel_ns"]
            module = row.get("start_module") or "<unknown>"
            name = row.get("thread_name") or "<unnamed>"
            key = (module, name)
            thread_key = (row.get("thread_id"), row.get("creation_filetime"), module, name)
            for target in (grouped[key], individual[thread_key]):
                target[0] += row["user_ns"]
                target[1] += row["kernel_ns"]
        def table(groups, by_thread=False):
            output = []
            for key, (user, kernel) in groups.items():
                module, name = key[-2:]
                value = {"start_module": module, "thread_name": name, "user_ms_per_second": user / 1e6 / seconds,
                         "kernel_ms_per_second": kernel / 1e6 / seconds, "cores": (user + kernel) / 1e9 / seconds}
                if by_thread:
                    value.update(thread_id=key[0], creation_filetime=key[1])
                output.append(value)
            return sorted(output, key=lambda r: r["cores"], reverse=True)
        declared_cpu = sum(r.get("observed_user_ns", 0) + r.get("observed_kernel_ns", 0) for r in windows.values())
        missing_windows = []
        generations = defaultdict(list)
        for (pid, generation, sequence) in windows:
            generations[(pid, generation)].append(sequence)
        for (pid, generation), sequences in generations.items():
            missing_windows.extend({"pid": pid, "generation": generation, "sequence": n}
                                   for n in range(min(sequences), max(sequences)+1) if n not in sequences)
        result["threads"] = {"available": True, "seconds": seconds, "report_windows": len(windows),
                             "by_origin_and_name": table(grouped), "by_thread": table(individual, True),
                             "row_count_matches": all(row_count[k] == r["enumerated"] for k, r in windows.items()),
                             "retained_thread_rows": sum(row_count.values()), "expected_thread_rows": sum(r["enumerated"] for r in windows.values()),
                             "cpu_time_coverage": retained_cpu / declared_cpu if declared_cpu else None,
                             "missing_cpu_ms_per_second": (declared_cpu-retained_cpu) / 1e6 / seconds if declared_cpu else None,
                             "missing_window_sequences": missing_windows,
                             "unreadable_thread_samples": sum(r.get("unreadable", 0) for r in windows.values()),
                             "ended_thread_samples": sum(r.get("ended_threads", 0) for r in windows.values()),
                             "collection_cpu_ms_per_second": sum(r["collection_cpu_ns"] for r in windows.values()) / 1e6 / seconds,
                             "collection_wall_ms_per_second": sum(r["collection_wall_ns"] for r in windows.values()) / 1e6 / seconds,
                             "process_cpu_ms_per_second": sum(r.get("process_user_ns", 0) + r.get("process_kernel_ns", 0) for r in windows.values() if r.get("process_known")) / 1e6 / seconds}
    # New schema carries an empirical calibration per window. Old captures
    # retain ledger values, never receive a hardcoded clock conversion.
    calibrated = {k: w for k, w in windows.items() if w.get("cycle_calibration_valid") is True
                  and isinstance(w.get("tsc_hz"), (int, float)) and w["tsc_hz"] > 1e6}
    result["cycle_runtime"] = {"available": bool(calibrated), "report_windows": len(calibrated),
                               "semantics": "locally calibrated estimate; no user/kernel split"}
    if calibrated:
        duration = sum((w["end_qpc"]-w["begin_qpc"])/w["qpc_frequency"] for w in calibrated.values())
        by_thread, by_group = defaultdict(float), defaultdict(float)
        retained_ns = 0.0
        for row in rows:
            identity = (row.get("pid"), row.get("sampler_generation", 0), row.get("sequence"))
            w = calibrated.get(identity)
            if row.get("event") != "thread_cpu_thread" or not w or not row.get("cycles_known"):
                continue
            ns = int(row["cycles"]) * 1e9 / w["tsc_hz"]
            retained_ns += ns
            module, name = row.get("start_module") or "<unknown>", row.get("thread_name") or "<unnamed>"
            by_thread[(row.get("thread_id"), row.get("creation_filetime"), module, name)] += ns
            by_group[(module, name)] += ns
        def cycle_table(groups, individual=False):
            output = []
            for key, ns in groups.items():
                value = dict(start_module=key[-2], thread_name=key[-1], estimated_cores=ns/1e9/duration,
                             estimated_ms_per_second=ns/1e6/duration)
                if individual:
                    value.update(thread_id=key[0], creation_filetime=key[1])
                output.append(value)
            return sorted(output, key=lambda r: r["estimated_cores"], reverse=True)
        observed_ns = sum(int(w.get("observed_cycles", 0))*1e9/w["tsc_hz"] for w in calibrated.values())
        process_windows = [w for w in calibrated.values() if w.get("process_cycles_known")]
        process_seconds = sum((w["end_qpc"]-w["begin_qpc"])/w["qpc_frequency"] for w in process_windows)
        process_ns = sum(int(w["process_cycles"])*1e9/w["tsc_hz"] for w in process_windows)
        matched_observed = sum(int(w.get("observed_cycles", 0))*1e9/w["tsc_hz"] for w in process_windows)
        result["cycle_runtime"].update(seconds=duration, by_thread=cycle_table(by_thread, True),
            by_origin_and_name=cycle_table(by_group), retained_estimated_cores=retained_ns/1e9/duration,
            observed_estimated_cores=observed_ns/1e9/duration,
            retained_to_observed_cycle_ratio=retained_ns/observed_ns if observed_ns else None,
            process_report_windows=len(process_windows), process_seconds=process_seconds,
            process_estimated_cores=process_ns/1e9/process_seconds if process_seconds else None,
            observed_to_process_cycle_ratio=matched_observed/process_ns if process_ns else None)
    if windows:
        result["threads"]["semantics"] = "GetThreadTimes/GetProcessTimes accounting reference; can miss short bursts"
        result["threads"]["primary_metric"] = "cycle_runtime" if calibrated else "raw ledger only; calibrated runtime unavailable"
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=Path)
    args = parser.parse_args()
    window = json.loads((args.capture / "window.json").read_text(encoding="utf-8-sig"))
    result = summarize(records(args.capture), window["startQpcTicks"], window["endQpcTicks"], window["qpcFrequency"])
    result["capture"] = str(args.capture.resolve())
    print(json.dumps(result, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
