#!/usr/bin/env python3
import argparse
import json
import math
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def run(cmd):
    return subprocess.run(cmd, cwd=ROOT, check=True, text=True, capture_output=True)


def clamp01(value):
    return max(0.0, min(1.0, value))


def pearson(xs, ys):
    n = min(len(xs), len(ys))
    if n < 2:
        return 0.0
    mx = sum(xs[:n]) / n
    my = sum(ys[:n]) / n
    num = 0.0
    dx = 0.0
    dy = 0.0
    for i in range(n):
        ax = xs[i] - mx
        ay = ys[i] - my
        num += ax * ay
        dx += ax * ax
        dy += ay * ay
    if dx <= 1e-12 or dy <= 1e-12:
        return 0.0
    return num / math.sqrt(dx * dy)


def mean_abs_error(xs, ys):
    n = min(len(xs), len(ys))
    if n == 0:
        return 1.0
    return sum(abs(xs[i] - ys[i]) for i in range(n)) / n


def mean_value(values):
    if not values:
        return 0.0
    return sum(values) / len(values)


def dynamic_range(values):
    if not values:
        return 0.0
    return max(values) - min(values)


def load_truth(truth_path):
    return json.loads(Path(truth_path).read_text())


def run_probe(probe_path, wav_path, block_size, limit_seconds):
    cmd = [
        str(probe_path),
        "--wav",
        str(wav_path),
        "--block",
        str(block_size),
        "--frames-jsonl",
    ]
    if limit_seconds > 0:
        cmd.extend(["--limit-seconds", str(limit_seconds)])
    completed = run(cmd)
    frames = []
    for line in completed.stdout.splitlines():
        line = line.strip()
        if not line:
            continue
        frames.append(json.loads(line))
    return frames


def probe_role_series(frames, name):
    if name == "kick":
        return [clamp01(f["kick_impact"]) for f in frames]
    if name == "bass":
        return [clamp01(f["bass_body"]) for f in frames]
    if name == "harmonic":
        return [clamp01(f["harmonic_body"]) for f in frames]
    if name == "lead":
        return [clamp01(f["lead_presence"]) for f in frames]
    if name == "air":
        return [clamp01(f["air_presence"]) for f in frames]
    if name == "perc":
        return [clamp01(0.75 * max(f["snare_impact"], f["hat_tick"]) + 0.25 * f["percussive_focus"]) for f in frames]
    if name == "energy":
        return [clamp01(f["energy_level"]) for f in frames]
    if name == "tension":
        return [clamp01(f["tension"]) for f in frames]
    if name == "release":
        return [clamp01(f["release"]) for f in frames]
    raise KeyError(name)


def truth_role_series(frames, name):
    return [clamp01(f[name]) for f in frames]


def compare_fixture(truth, probe_frames):
    truth_frames = truth["frames"]
    n = min(len(truth_frames), len(probe_frames))
    truth_frames = truth_frames[:n]
    probe_frames = probe_frames[:n]
    roles = ["kick", "bass", "harmonic", "lead", "air", "perc", "energy", "tension", "release"]
    role_reports = {}
    score_sum = 0.0
    for role in roles:
        truth_values = truth_role_series(truth_frames, role)
        probe_values = probe_role_series(probe_frames, role)
        corr = pearson(truth_values, probe_values)
        mae = mean_abs_error(truth_values, probe_values)
        truth_range = dynamic_range(truth_values)
        probe_range = dynamic_range(probe_values)
        truth_mean = mean_value(truth_values)
        probe_mean = mean_value(probe_values)
        if truth_range < 0.02:
            if truth_mean < 0.05:
                suppression = clamp01(1.0 - probe_mean / 0.20)
                stability = clamp01(1.0 - probe_range / 0.25)
                score = clamp01(0.60 * suppression + 0.25 * stability + 0.15 * (1.0 - mae))
            else:
                mean_match = clamp01(1.0 - abs(probe_mean - truth_mean) / max(0.10, truth_mean))
                stability = clamp01(1.0 - probe_range / 0.40)
                score = clamp01(0.55 * mean_match + 0.25 * stability + 0.20 * (1.0 - mae))
        else:
            range_match = min(1.0, probe_range / max(0.05, truth_range))
            score = clamp01(0.65 * ((corr + 1.0) * 0.5) + 0.20 * (1.0 - mae) + 0.15 * range_match)
        score_sum += score
        role_reports[role] = {
            "correlation": round(corr, 4),
            "mae": round(mae, 4),
            "truth_mean": round(truth_mean, 4),
            "probe_mean": round(probe_mean, 4),
            "truth_range": round(truth_range, 4),
            "probe_range": round(probe_range, 4),
            "score": round(score * 100.0, 2),
        }

    overall = score_sum / len(roles)
    return {
        "fixture_id": truth["id"],
        "frames_compared": n,
        "overall_score": round(overall * 100.0, 2),
        "roles": role_reports,
    }


def main():
    parser = argparse.ArgumentParser(description="Compare generated EDM fixture truth against analysis_probe frames.")
    parser.add_argument("--probe", default="build/probe_runner/analysis_probe")
    parser.add_argument("--truth-json", required=True)
    parser.add_argument("--wav")
    parser.add_argument("--out-json")
    parser.add_argument("--limit-seconds", type=float, default=0.0)
    args = parser.parse_args()

    truth = load_truth(args.truth_json)
    wav_path = Path(args.wav) if args.wav else Path(truth["wav"])
    probe_path = Path(args.probe)
    probe_frames = run_probe(probe_path, wav_path, truth["block_size"], args.limit_seconds or truth["duration_sec"])
    report = compare_fixture(truth, probe_frames)
    rendered = json.dumps(report, indent=2)
    if args.out_json:
        Path(args.out_json).write_text(rendered + "\n")
    print(rendered)
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
