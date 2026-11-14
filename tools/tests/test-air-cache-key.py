#!/usr/bin/env python3
"""Check production AIR identities and specialization ownership with native Metal."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile


KERNEL = Path(__file__).resolve().parents[2]
TESTS = Path(__file__).resolve().parent
SOURCE = KERNEL / "tools/userland-driver/annibalemetal_stitch.inc"


def run_checks(source, output):
    text = source.read_text()
    key_start = text.index("static BOOL\nrw_air_key_bytes")
    key_stop = text.index("static NSData *\nrw_air_cache_lookup", key_start)
    (output / "air-cache-key-production.inc").write_text(text[key_start:key_stop])
    start = text.index("@interface AnnibaleMetalSpecializedFunction")
    stop = text.index("/* Once per process, on the first device:", start)
    (output / "air-cache-specialization-production.inc").write_text(text[start:stop])
    results = []
    for name in ("air-cache-key", "air-cache-specialization"):
        binary = output / name
        arguments = ["xcrun", "clang", "-fno-objc-arc", "-fblocks", "-O1",
                     "-Wall", "-Wextra", "-Werror", "-Wno-deprecated-declarations",
                     "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                     "-framework", "Foundation", "-framework", "Metal",
                     "-I" + str(output), str(TESTS / (name + "-native.m")),
                     "-o", str(binary)]
        (output / (name + "-arguments.json")).write_text(json.dumps(arguments, indent=2) + "\n")
        build = subprocess.run(arguments, capture_output=True, text=True, timeout=45)
        (output / (name + "-compile.log")).write_text(build.stdout + build.stderr)
        if build.returncode:
            raise RuntimeError(build.stderr)
        modes = [None] if name == "air-cache-key" else [None, "missing-stats", "missing-internal"]
        for mode in modes:
            label = name + ("-" + mode if mode else "")
            cache = output / (label + "-cache")
            command = [str(binary)] if name == "air-cache-key" else [str(binary), str(cache), mode or "fixture"]
            result = subprocess.run(command, capture_output=True, text=True, timeout=45)
            (output / (label + "-run.log")).write_text(result.stderr)
            if result.returncode:
                raise RuntimeError(result.stdout + result.stderr)
            parsed = json.loads(result.stdout)
            (output / (label + "-result.json")).write_text(json.dumps(parsed, indent=2) + "\n")
            results.append({"test": label, **parsed})
    report = {"source_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
              "status": "passed", "checks": sum(row["checks"] for row in results),
              "results": results}
    (output / "result.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({"status": report["status"], "checks": report["checks"],
                      "source_sha256": report["source_sha256"]}))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=SOURCE)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if args.output:
        args.output.mkdir(parents=True, exist_ok=True)
        run_checks(args.source, args.output.resolve())
    else:
        with tempfile.TemporaryDirectory(prefix="air-cache-key-") as directory:
            run_checks(args.source, Path(directory))


if __name__ == "__main__":
    main()
