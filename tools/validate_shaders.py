#!/usr/bin/env python3
import shutil
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SHADER_DIR = ROOT / "assets" / "shaders"


def main():
    validator = shutil.which("glslangValidator")
    if validator is None:
        print("error: glslangValidator not found", file=sys.stderr)
        return 2

    failures = []
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

    if failures:
        for failure in failures:
            print(f"FAILED {failure['shader']}", file=sys.stderr)
            if failure["stdout"]:
                print(failure["stdout"], file=sys.stderr)
            if failure["stderr"]:
                print(failure["stderr"], file=sys.stderr)
        return 1

    print(f"validated {len(list(SHADER_DIR.glob('*.frag')))} fragment shaders")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
