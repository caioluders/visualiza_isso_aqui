#!/usr/bin/env python3
import argparse
import json
import math
import struct
import subprocess
from pathlib import Path


def chunk(chunk_id, payload):
    pad = b"\x00" if len(payload) % 2 else b""
    return chunk_id + struct.pack("<I", len(payload)) + payload + pad


def write_wav(path, audio_format, bits_per_sample, channels, sample_rate, frames, extra_unknown_chunk=False):
    byte_rate = sample_rate * channels * bits_per_sample // 8
    block_align = channels * bits_per_sample // 8
    fmt_payload = struct.pack(
        "<HHIIHH",
        audio_format,
        channels,
        sample_rate,
        byte_rate,
        block_align,
        bits_per_sample,
    )

    data = bytearray()
    for i in range(frames):
        x = math.sin(2.0 * math.pi * 440.0 * i / sample_rate) * 0.5
        for c in range(channels):
            y = x if c == 0 else x * 0.75
            if audio_format == 3 and bits_per_sample == 32:
                data.extend(struct.pack("<f", y))
            elif bits_per_sample == 8:
                data.extend(struct.pack("<B", max(0, min(255, int((y * 127.0) + 128)))))
            elif bits_per_sample == 16:
                data.extend(struct.pack("<h", max(-32768, min(32767, int(y * 32767.0)))))
            elif bits_per_sample == 24:
                v = max(-8388608, min(8388607, int(y * 8388607.0)))
                data.extend(struct.pack("<i", v)[0:3])
            elif bits_per_sample == 32:
                data.extend(struct.pack("<i", max(-2147483648, min(2147483647, int(y * 2147483647.0)))))
            else:
                raise ValueError(bits_per_sample)

    chunks = []
    if extra_unknown_chunk:
        chunks.append(chunk(b"JUNK", b"odd-size!"))
    chunks.append(chunk(b"fmt ", fmt_payload))
    chunks.append(chunk(b"data", bytes(data)))
    riff_payload = b"WAVE" + b"".join(chunks)
    path.write_bytes(b"RIFF" + struct.pack("<I", len(riff_payload)) + riff_payload)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--probe", required=True)
    parser.add_argument("--out-dir", required=True)
    args = parser.parse_args()

    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    cases = [
        ("pcm_u8_mono_junk", 1, 8, 1),
        ("pcm_s16_stereo", 1, 16, 2),
        ("pcm_s24_stereo_junk", 1, 24, 2),
        ("pcm_s32_mono", 1, 32, 1),
        ("float32_stereo", 3, 32, 2),
    ]

    failures = []
    for name, audio_format, bits, channels in cases:
        wav = out_dir / f"{name}.wav"
        write_wav(wav, audio_format, bits, channels, 48000, 4096, "junk" in name)
        completed = subprocess.run(
            [args.probe, "--wav", str(wav), "--limit-seconds", "0.05"],
            text=True,
            capture_output=True,
        )
        if completed.returncode != 0:
            failures.append({"case": name, "stderr": completed.stderr})
            continue
        metrics = json.loads(completed.stdout)
        if metrics["channels"] != channels or metrics["sample_rate"] != 48000 or metrics["blocks"] == 0 or metrics["avg_rms"] <= 0.0:
            failures.append({"case": name, "metrics": metrics})

    if failures:
        print(json.dumps({"passed": False, "failures": failures}, indent=2))
        return 1

    print(json.dumps({"passed": True, "cases": [c[0] for c in cases]}, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
