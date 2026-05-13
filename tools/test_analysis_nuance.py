#!/usr/bin/env python3
import argparse
import json
import math
import struct
import subprocess
from pathlib import Path


def write_sine_wav(path, hz, seconds=1.0, sample_rate=48000, amp=0.5):
    frames = int(seconds * sample_rate)
    payload = bytearray()
    for i in range(frames):
        sample = 0.0 if hz <= 0 else math.sin(2.0 * math.pi * hz * i / sample_rate) * amp
        v = max(-32768, min(32767, int(sample * 32767.0)))
        payload.extend(struct.pack("<h", v))

    fmt = struct.pack("<HHIIHH", 1, 1, sample_rate, sample_rate * 2, 2, 16)
    chunks = [
        b"fmt " + struct.pack("<I", len(fmt)) + fmt,
        b"data" + struct.pack("<I", len(payload)) + bytes(payload),
    ]
    riff_payload = b"WAVE" + b"".join(chunks)
    path.write_bytes(b"RIFF" + struct.pack("<I", len(riff_payload)) + riff_payload)


def probe(probe_path, wav):
    completed = subprocess.run(
        [probe_path, "--wav", str(wav), "--limit-seconds", "1.0"],
        text=True,
        capture_output=True,
        check=True,
    )
    return json.loads(completed.stdout)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--probe", required=True)
    parser.add_argument("--out-dir", required=True)
    args = parser.parse_args()

    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    cases = {
        "bass": 80.0,
        "body": 1000.0,
        "air": 8000.0,
        "silence": 0.0,
    }

    metrics = {}
    for name, hz in cases.items():
        wav = out_dir / f"{name}.wav"
        write_sine_wav(wav, hz)
        metrics[name] = probe(args.probe, wav)

    failures = []
    bass = metrics["bass"]
    body = metrics["body"]
    air = metrics["air"]
    silence = metrics["silence"]

    if not (bass["avg_low"] > bass["avg_mid"] * 4.0 and bass["avg_low"] > bass["avg_high"] * 10.0):
        failures.append("bass tone did not dominate low band")
    if not (body["avg_mid"] > body["avg_low"] * 2.0 and body["avg_mid"] > body["avg_high"] * 2.0):
        failures.append("body tone did not dominate mid band")
    if not (air["avg_high"] > air["avg_mid"] * 2.0 and air["avg_high"] > air["avg_low"] * 2.0):
        failures.append("air tone did not dominate high band")
    if not (silence["avg_rms"] < 1e-6 and silence["max_onset"] < 1e-6 and silence["beat_count"] == 0):
        failures.append("silence produced non-zero detector response")

    report = {"passed": not failures, "failures": failures, "metrics": metrics}
    print(json.dumps(report, indent=2))
    return 0 if not failures else 1


if __name__ == "__main__":
    raise SystemExit(main())
