#!/usr/bin/env python3
import argparse
import json
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
GENERATOR = ROOT / "tools" / "generate_edm_fixtures.js"
COMPARATOR = ROOT / "tools" / "compare_generated_fixture_truth.py"


def run(cmd):
    return subprocess.run(cmd, cwd=ROOT, check=True, text=True, capture_output=True)


def average(values):
    return sum(values) / len(values) if values else 0.0


def summarize_results(results, role_scores, focus_role_scores):
    return {
        "overall_score": round(average([item["overall_score"] for item in results]), 2),
        "fixture_count": len(results),
        "role_average_scores": {
            role: round(average(scores), 2)
            for role, scores in sorted(role_scores.items())
        },
        "focus_role_average_scores": {
            role: round(average(scores), 2)
            for role, scores in sorted(focus_role_scores.items())
        },
    }


def main():
    parser = argparse.ArgumentParser(description="Generate deterministic EDM fixtures and compare analysis_probe to the known truth.")
    parser.add_argument("--probe", default="build/probe_runner/analysis_probe")
    parser.add_argument("--out-dir", default="build/generated_edm_fixtures")
    parser.add_argument("--sample-rate", type=int, default=48000)
    parser.add_argument("--block-size", type=int, default=1024)
    parser.add_argument("--report", default="build/generated_fixture_truth_report.json")
    args = parser.parse_args()

    out_dir = ROOT / args.out_dir
    run(["node", str(GENERATOR), "--out-dir", str(out_dir), "--sample-rate", str(args.sample_rate), "--block-size", str(args.block_size)])

    manifest = json.loads((out_dir / "generated_fixtures_manifest.json").read_text())
    results = []
    role_scores = {}
    focus_role_scores = {}
    split_results = {}
    for fixture in manifest["fixtures"]:
        completed = run([
            sys.executable,
            str(COMPARATOR),
            "--probe",
            str(ROOT / args.probe),
            "--truth-json",
            fixture["truth"],
        ])
        result = json.loads(completed.stdout)
        result["focus_roles"] = fixture.get("focus_roles", [])
        result["split"] = fixture.get("split", "dev")
        result["style"] = fixture.get("style", "generic")
        results.append(result)
        for role, role_result in result["roles"].items():
            role_scores.setdefault(role, []).append(role_result["score"])
        for role in fixture.get("focus_roles", []):
            if role in result["roles"]:
                focus_role_scores.setdefault(role, []).append(result["roles"][role]["score"])
        split_bucket = split_results.setdefault(result["split"], {
            "results": [],
            "role_scores": {},
            "focus_role_scores": {},
        })
        split_bucket["results"].append(result)
        for role, role_result in result["roles"].items():
            split_bucket["role_scores"].setdefault(role, []).append(role_result["score"])
        for role in fixture.get("focus_roles", []):
            if role in result["roles"]:
                split_bucket["focus_role_scores"].setdefault(role, []).append(result["roles"][role]["score"])

    overall_summary = summarize_results(results, role_scores, focus_role_scores)
    split_summaries = {
        split: {
            **summarize_results(bucket["results"], bucket["role_scores"], bucket["focus_role_scores"]),
            "fixtures": [item["fixture_id"] for item in bucket["results"]],
        }
        for split, bucket in sorted(split_results.items())
    }
    report = {
        "generated_manifest": str(out_dir / "generated_fixtures_manifest.json"),
        **overall_summary,
        "split_summaries": split_summaries,
        "results": results,
    }
    report_path = ROOT / args.report
    report_path.parent.mkdir(parents=True, exist_ok=True)
    report_path.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except subprocess.CalledProcessError as exc:
        sys.stderr.write(exc.stderr or "")
        raise SystemExit(exc.returncode)
    except Exception as exc:
        print(f"error: {exc}", file=sys.stderr)
        raise SystemExit(2)
