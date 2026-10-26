#!/usr/bin/env python3
"""Check scope joins and unknown/refusal handling using one saved raw3 group."""
import argparse
from copy import deepcopy
import importlib.util
import json
from pathlib import Path
import sys
import tempfile

sys.dont_write_bytecode = True
spec = importlib.util.spec_from_file_location("raw_gate_analysis", Path(__file__).with_name("analyze-rawcolor-source-gates.py"))
gate = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gate)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--analysis", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    saved = json.loads(args.analysis.read_text(encoding="utf-8"))
    example = saved["pattern_groups"][0]
    records = [{**deepcopy(example["header"]), "event": "descriptor_pre_dispatch"}] + [{**deepcopy(row), "event": "descriptor_image_binding"} for row in example["bindings"]]
    results = []

    def check(name, items, verify):
        with tempfile.TemporaryDirectory(prefix="rawcolor-gate-analysis-") as directory:
            path = Path(directory) / "observer.jsonl"
            path.write_text("".join(json.dumps(item) + "\n" for item in items), encoding="utf-8")
            result = gate.analyze([path])
            verify(result)
            results.append({"name": name, "passed": True})

    def equal(value, expected):
        if value != expected:
            raise AssertionError(f"{value!r} != {expected!r}")

    check("accepted exact recording with color resolved and depth refusal", records,
          lambda r: equal((r["groups_with_exact_sampled_5_6_7"], r["patterns_emitted_checks_satisfied"]), (1, 1)))
    for field in ("command_generation", "record_ordinal", "pipeline_generation"):
        changed = deepcopy(records)
        changed[2][field] += 1
        check("never joins different " + field, changed, lambda r: equal(r["groups_with_exact_sampled_5_6_7"], 0))
    changed = deepcopy(records)
    changed[0]["metadata_accepted"] = False
    check("refused descriptor header is never qualifying", changed, lambda r: equal(r["groups_with_exact_sampled_5_6_7"], 0))
    color = next(index for index, item in enumerate(records) if item.get("source_match_mask", 0) & 32)
    changed = deepcopy(records)
    changed[color]["recorded_source_layout"]["prefix_order"] += 1
    check("RP prefix mismatch is an explicit refusal", changed,
          lambda r: equal(r["pattern_known_refusal_fields"].get("raw_color.layout_identity.prefix_order"), 1))
    changed = deepcopy(records)
    changed[color]["resource_memory"]["global_issues"] = 262144
    check("incomplete memory coverage refuses the emitted recipe", changed,
          lambda r: equal(r["pattern_known_refusal_fields"].get("raw_color.memory.global_issues"), 1))
    changed = deepcopy(records)
    del changed[color]["resource_memory"]["alias_free_observed"]
    check("missing alias observation remains unknown", changed,
          lambda r: equal((r["patterns_emitted_checks_satisfied"], r["pattern_unknown_fields"].get("raw_color.memory.alias_free_observed")), (0, 1)))
    changed = deepcopy(records)
    changed[color]["recorded_source_layout"]["queried"] = False
    check("zero issues on unqueried layout cannot pass", changed,
          lambda r: equal(r["pattern_known_refusal_fields"].get("raw_color.layout.queried"), 1))
    changed = deepcopy(records)
    changed[0]["recording_facts"]["begin_usage_flags"] = 0
    changed[0]["recording_facts"]["reasons"] = 2048
    check("known flags zero is allowed separately from one time", changed, lambda r: equal(r["patterns_emitted_checks_satisfied"], 1))
    changed = deepcopy(records)
    changed[0]["recording_facts"]["begin_usage_flags"] = 4
    check("simultaneous use is refused", changed,
          lambda r: equal(r["pattern_known_refusal_fields"].get("recording.unsupported_begin_usage_flags"), 1))
    changed = deepcopy(records)
    changed[color]["image_generation"] = 0
    check("zero image generation cannot join an exact source pattern", changed, lambda r: equal(r["groups_with_exact_sampled_5_6_7"], 0))
    check("duplicate color binding is ambiguous", records + [deepcopy(records[color])], lambda r: equal(r["groups_with_exact_sampled_5_6_7"], 0))
    report = {"schema": "rawcolor-source-gates-analyzer-validation-v1", "passed": True, "cases": len(results), "results": results,
              "saved_analysis": str(args.analysis.resolve()), "game_replayed": False, "gpu_used": False, "installed_payload_changed": False}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"passed": True, "cases": len(results)}))


if __name__ == "__main__":
    main()
