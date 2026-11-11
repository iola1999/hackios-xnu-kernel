#!/usr/bin/env python3
"""Check the complete heap implementation against native Metal resources."""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "tools/userland-driver/annibalemetal_heap.inc"
FIXTURE = Path(__file__).with_name("metal-heap-protection.m")


def check(output, baseline):
    output = output.resolve()
    source = SOURCE.read_text()
    start = source.index('\tSEL protection = sel_getUid("protectionOptions");')
    end = source.index('\n\t_owner =', start)
    variants = {"candidate": source, "without_rejection": source[:start] + source[end:]}
    if baseline:
        variants["baseline"] = subprocess.check_output(
            ["git", "show", baseline + ":tools/userland-driver/annibalemetal_heap.inc"], cwd=ROOT, text=True)
    results = {}
    for name, text in variants.items():
        include = output / (name + ".inc")
        include.write_text(text)
        binary = output / name
        command = ["xcrun", "clang", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
                   "-fno-objc-arc", "-framework", "Foundation", "-framework", "Metal",
                   '-DHEAP_INCLUDE="' + str(include) + '"', str(FIXTURE), "-o", str(binary)]
        compiled = subprocess.run(command, capture_output=True, text=True)
        assert compiled.returncode == 0, compiled.stderr
        results[name] = {}
        for case in ("ordinary", "protected"):
            run = subprocess.run([str(binary), case], capture_output=True, text=True, timeout=30)
            results[name][case] = {"returncode": run.returncode, "stdout": run.stdout, "stderr": run.stderr}
    assert all(r["returncode"] == 0 for r in results["candidate"].values()), results["candidate"]
    assert results["without_rejection"]["ordinary"]["returncode"] == 0
    assert results["without_rejection"]["protected"]["returncode"] != 0
    if baseline:
        assert results["baseline"]["ordinary"]["returncode"] != 0
        assert results["baseline"]["protected"]["returncode"] != 0
    (output / "results.json").write_text(json.dumps(results, indent=2) + "\n")
    print("PASS production heap/native contracts; old getter and removed protection rejection fail")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline-ref")
    parser.add_argument("--output-dir", type=Path)
    args = parser.parse_args()
    if args.output_dir:
        args.output_dir.mkdir(parents=True, exist_ok=True)
        check(args.output_dir, args.baseline_ref)
    else:
        with tempfile.TemporaryDirectory(prefix="metal-heap-") as directory:
            check(Path(directory), args.baseline_ref)


if __name__ == "__main__":
    main()
