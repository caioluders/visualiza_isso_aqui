#!/usr/bin/env python3
import json
import sys
from pathlib import Path


REQUIRED_EXPECTED = {
    "min_audio_seconds": (0.1, 3600.0),
    "min_avg_rms": (0.0, 1.0),
    "min_max_onset": (0.0, 1.0),
    "min_beat_count": (0, 10000),
    "max_beat_rate_per_minute": (30.0, 300.0),
    "max_realtime_ratio": (0.001, 1.0),
    "max_p95_block_realtime_ratio": (0.001, 1.0),
    "max_block_realtime_ratio": (0.001, 4.0),
}


def valid_number(value, lo, hi):
    return isinstance(value, (int, float)) and lo <= value <= hi


def main():
    manifest = Path(sys.argv[1]) if len(sys.argv) > 1 else Path("assets/test_fixtures/edm_fixtures.json")
    data = json.loads(manifest.read_text())
    failures = []
    fixtures = data.get("fixtures")
    if not isinstance(fixtures, list) or not fixtures:
        failures.append("fixtures must be a non-empty list")
    else:
        seen = set()
        for index, fixture in enumerate(fixtures):
            prefix = f"fixture[{index}]"
            fixture_id = fixture.get("id")
            if not isinstance(fixture_id, str) or not fixture_id:
                failures.append(f"{prefix}.id must be non-empty")
            elif fixture_id in seen:
                failures.append(f"{prefix}.id duplicates {fixture_id}")
            seen.add(fixture_id)
            for field in ("title", "source", "license", "url"):
                if not isinstance(fixture.get(field), str) or not fixture[field]:
                    failures.append(f"{prefix}.{field} must be non-empty")
            url = fixture.get("url", "")
            if not (url.startswith("https://") or url.startswith("http://")):
                failures.append(f"{prefix}.url must be http(s)")
            expected = fixture.get("expected")
            if not isinstance(expected, dict):
                failures.append(f"{prefix}.expected must be an object")
                continue
            for key, (lo, hi) in REQUIRED_EXPECTED.items():
                if key not in expected:
                    failures.append(f"{prefix}.expected.{key} is missing")
                elif not valid_number(expected[key], lo, hi):
                    failures.append(f"{prefix}.expected.{key} must be in [{lo}, {hi}]")
            if expected.get("max_p95_block_realtime_ratio", 0) > expected.get("max_block_realtime_ratio", 0):
                failures.append(f"{prefix}.expected p95 block ratio cannot exceed max block ratio")

    if failures:
        print(json.dumps({"passed": False, "failures": failures}, indent=2))
        return 1
    print(json.dumps({"passed": True, "fixture_count": len(fixtures)}, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
