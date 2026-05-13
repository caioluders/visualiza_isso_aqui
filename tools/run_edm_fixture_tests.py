#!/usr/bin/env python3
import argparse
import json
import shutil
import subprocess
import sys
from pathlib import Path
from urllib.request import Request, urlopen


ROOT = Path(__file__).resolve().parents[1]
MANIFEST = ROOT / "assets" / "test_fixtures" / "edm_fixtures.json"
FIXTURE_DIR = ROOT / "build" / "edm_fixtures"
REPORT = ROOT / "build" / "edm_fixture_report.json"


def run(cmd):
    return subprocess.run(cmd, cwd=ROOT, check=True, text=True, capture_output=True)


def ensure_tool(name):
    if shutil.which(name) is None:
        raise RuntimeError(f"Required tool not found: {name}")


def cmake_build(build_dir):
    run([
        "cmake",
        "-S",
        ".",
        "-B",
        str(build_dir),
        "-DCMAKE_BUILD_TYPE=Release",
        "-DVISUALIZA_BUILD_APP=OFF",
        "-DVISUALIZA_BUILD_TEST_ENGINE=ON",
    ])
    run(["cmake", "--build", str(build_dir), "--target", "analysis_probe", "-j2"])


def convert_to_wav(src, wav):
    ensure_tool("ffmpeg")
    cmd = [
        "ffmpeg",
        "-y",
        "-hide_banner",
        "-loglevel",
        "error",
        "-i",
        str(src),
        "-acodec",
        "pcm_s16le",
        "-ar",
        "48000",
        str(wav),
    ]
    run(cmd)


def download(url, destination):
    request = Request(url, headers={"User-Agent": "visualiza_isso_aqui-edm-test-engine/0.1"})
    with urlopen(request) as response, destination.open("wb") as out:
        shutil.copyfileobj(response, out)


def evaluate(metrics, expected):
    failures = []
    audio_minutes = max(metrics["audio_seconds"] / 60.0, 1e-9)
    beat_rate_per_minute = metrics["beat_count"] / audio_minutes
    checks = {
        "min_audio_seconds": metrics["audio_seconds"] >= expected["min_audio_seconds"],
        "min_avg_rms": metrics["avg_rms"] >= expected["min_avg_rms"],
        "min_max_onset": metrics["max_onset"] >= expected["min_max_onset"],
        "min_beat_count": metrics["beat_count"] >= expected["min_beat_count"],
        "max_beat_rate_per_minute": beat_rate_per_minute <= expected.get("max_beat_rate_per_minute", float("inf")),
        "max_realtime_ratio": metrics["realtime_ratio"] <= expected["max_realtime_ratio"],
        "max_p95_block_realtime_ratio": metrics["p95_block_realtime_ratio"] <= expected.get("max_p95_block_realtime_ratio", expected["max_realtime_ratio"]),
        "max_block_realtime_ratio": metrics["max_block_realtime_ratio"] <= expected.get("max_block_realtime_ratio", 1.0),
    }
    for name, passed in checks.items():
        if not passed:
            failures.append(name)
    return failures


def clamp01(value):
    return max(0.0, min(1.0, value))


def score_fixture(metrics, expected):
    audio_minutes = max(metrics["audio_seconds"] / 60.0, 1e-9)
    beat_rate_per_minute = metrics["beat_count"] / audio_minutes
    onset_density_per_minute = metrics["max_onset"] / audio_minutes
    low = max(metrics["avg_low"], 0.0)
    mid = max(metrics["avg_mid"], 0.0)
    high = max(metrics["avg_high"], 0.0)
    total_band_energy = low + mid + high
    if total_band_energy > 1e-9:
        band_shares = {
            "low": low / total_band_energy,
            "mid": mid / total_band_energy,
            "high": high / total_band_energy,
        }
    else:
        band_shares = {"low": 0.0, "mid": 0.0, "high": 0.0}

    components = {
        "signal": clamp01(metrics["avg_rms"] / expected["min_avg_rms"]),
        "onset_response": clamp01(metrics["max_onset"] / expected["min_max_onset"]),
        "beat_response": clamp01(metrics["beat_count"] / expected["min_beat_count"]),
        "beat_rate_control": clamp01(expected.get("max_beat_rate_per_minute", beat_rate_per_minute) / max(beat_rate_per_minute, 1e-9)),
        "live_headroom": clamp01((expected["max_realtime_ratio"] - metrics["realtime_ratio"]) / expected["max_realtime_ratio"]),
        "live_jitter": clamp01((expected.get("max_p95_block_realtime_ratio", expected["max_realtime_ratio"]) - metrics["p95_block_realtime_ratio"]) / expected.get("max_p95_block_realtime_ratio", expected["max_realtime_ratio"])),
        "spectral_nuance": (
            0.45 * clamp01(mid / max(low * 0.08, 1e-9)) +
            0.25 * clamp01(high / max(mid * 0.04, 1e-9)) +
            0.30 * clamp01(metrics["avg_percussive_ratio"] / 0.35)
        ),
    }
    overall = (
        0.20 * components["signal"] +
        0.20 * components["onset_response"] +
        0.18 * components["beat_response"] +
        0.07 * components["beat_rate_control"] +
        0.14 * components["live_headroom"] +
        0.06 * components["live_jitter"] +
        0.15 * components["spectral_nuance"]
    )

    diagnostics = []
    if components["live_headroom"] < 0.75:
        diagnostics.append("processing is close to the real-time budget")
    if components["live_jitter"] < 0.75:
        diagnostics.append("per-block processing jitter is close to the live budget")
    if components["beat_response"] < 1.0:
        diagnostics.append("beat detector under-triggered against fixture threshold")
    if components["beat_rate_control"] < 1.0:
        diagnostics.append("beat detector over-triggered against fixture threshold")
    if band_shares["mid"] < 0.05:
        diagnostics.append("mid-band response is weak relative to bass")
    if band_shares["high"] < 0.005:
        diagnostics.append("high-band response is weak relative to low/mid energy")

    return {
        "overall": round(overall * 100.0, 2),
        "components": {k: round(v * 100.0, 2) for k, v in components.items()},
        "beat_rate_per_minute": round(beat_rate_per_minute, 2),
        "onset_density_per_minute": round(onset_density_per_minute, 4),
        "band_shares": {k: round(v, 4) for k, v in band_shares.items()},
        "diagnostics": diagnostics,
    }


def summarize(results):
    if not results:
        return {"overall_score": 0.0, "passed": False}
    scores = [r["score"]["overall"] for r in results]
    realtime = [r["metrics"]["realtime_ratio"] for r in results]
    p95_realtime = [r["metrics"]["p95_block_realtime_ratio"] for r in results]
    beats = [r["score"]["beat_rate_per_minute"] for r in results]
    return {
        "passed": all(r["passed"] for r in results),
        "overall_score": round(sum(scores) / len(scores), 2),
        "min_fixture_score": round(min(scores), 2),
        "max_realtime_ratio": round(max(realtime), 8),
        "max_p95_block_realtime_ratio": round(max(p95_realtime), 8),
        "avg_beat_rate_per_minute": round(sum(beats) / len(beats), 2),
        "fixture_count": len(results),
    }


def main():
    parser = argparse.ArgumentParser(description="Download EDM fixtures and score the live audio-analysis engine.")
    parser.add_argument("--build-dir", default="build/edm_tests")
    parser.add_argument("--manifest", default=str(MANIFEST))
    parser.add_argument("--limit-seconds", type=float, default=90.0)
    parser.add_argument("--skip-download", action="store_true")
    args = parser.parse_args()

    ensure_tool("cmake")
    build_dir = ROOT / args.build_dir
    manifest = Path(args.manifest)
    FIXTURE_DIR.mkdir(parents=True, exist_ok=True)
    cmake_build(build_dir)

    probe = build_dir / "analysis_probe"
    if not probe.exists():
        raise RuntimeError(f"analysis_probe was not built at {probe}")

    data = json.loads(manifest.read_text())
    results = []
    for fixture in data["fixtures"]:
        ext = Path(fixture["url"].split("?")[0]).suffix or ".audio"
        raw = FIXTURE_DIR / f"{fixture['id']}{ext}"
        wav = FIXTURE_DIR / f"{fixture['id']}.wav"
        if not args.skip_download or not raw.exists():
            print(f"Downloading {fixture['id']} from {fixture['source']}...", file=sys.stderr)
            download(fixture["url"], raw)
        if not wav.exists() or wav.stat().st_mtime < raw.stat().st_mtime:
            convert_to_wav(raw, wav)

        completed = run([str(probe), "--wav", str(wav), "--limit-seconds", str(args.limit_seconds)])
        metrics = json.loads(completed.stdout)
        failures = evaluate(metrics, fixture["expected"])
        score = score_fixture(metrics, fixture["expected"])
        results.append({
            "fixture": fixture,
            "wav": str(wav),
            "metrics": metrics,
            "score": score,
            "failures": failures,
            "passed": not failures,
        })

    summary = summarize(results)
    report = {"summary": summary, "results": results, "passed": summary["passed"]}
    REPORT.parent.mkdir(parents=True, exist_ok=True)
    REPORT.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"error: {exc}", file=sys.stderr)
        raise SystemExit(2)
