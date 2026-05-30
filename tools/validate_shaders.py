#!/usr/bin/env python3
import shutil
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SHADER_DIR = ROOT / "assets" / "shaders"
PROCESSING_DIR = ROOT / "assets" / "processing"


def main():
    validator = shutil.which("glslangValidator")
    if validator is None:
        print("error: glslangValidator not found", file=sys.stderr)
        return 2

    failures = []
    validated = 0
    for shader in sorted(SHADER_DIR.glob("*.frag")):
        completed = subprocess.run(
            [validator, "-S", "frag", str(shader)],
            cwd=ROOT,
            text=True,
            capture_output=True,
        )
        if completed.returncode != 0:
            failures.append({
                "shader": str(shader.relative_to(ROOT)),
                "stdout": completed.stdout,
                "stderr": completed.stderr,
            })
        validated += 1

    node = shutil.which("node")
    if node and PROCESSING_DIR.exists():
        processing_scripts = [
            PROCESSING_DIR / "p5_engine_server.js",
            PROCESSING_DIR / "engine.js",
        ]
        processing_scripts.extend(sorted((PROCESSING_DIR / "sketches").glob("*.js")))
        for script in processing_scripts:
            if not script.exists():
                continue
            completed = subprocess.run(
                [node, "--check", str(script)],
                cwd=ROOT,
                text=True,
                capture_output=True,
            )
            if completed.returncode != 0:
                failures.append({
                    "shader": str(script.relative_to(ROOT)),
                    "stdout": completed.stdout,
                    "stderr": completed.stderr,
                })
            validated += 1
    elif PROCESSING_DIR.exists():
        print("warning: node not found; skipped p5 Processing syntax checks", file=sys.stderr)

    if failures:
        for failure in failures:
            print(f"FAILED {failure['shader']}", file=sys.stderr)
            if failure["stdout"]:
                print(failure["stdout"], file=sys.stderr)
            if failure["stderr"]:
                print(failure["stderr"], file=sys.stderr)
        return 1

    print(f"validated {validated} shaders/scripts")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
