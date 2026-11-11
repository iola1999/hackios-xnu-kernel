#!/usr/bin/env python3
"""Compile MiniAppleARMBacklight request handlers with host IOKit and hardware stubs."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import tempfile


SOURCE = Path("xnu/files/iokit/Kernel/MiniAppleARMBacklight.cpp")
FIXTURE = Path(__file__).with_name("backlight_fixture.cpp")


def block_end(source, start):
    depth, state, pos = 0, "code", start
    while pos < len(source):
        char, pair = source[pos], source[pos:pos + 2]
        if state == "line":
            if char == "\n":
                state = "code"
        elif state == "comment":
            if pair == "*/":
                state = "code"
                pos += 1
        elif state in ("string", "char"):
            if char == "\\":
                pos += 1
            elif char == ('"' if state == "string" else "'"):
                state = "code"
        elif pair in ("//", "/*"):
            state = "line" if pair == "//" else "comment"
            pos += 1
        elif char in ('"', "'"):
            state = "string" if char == '"' else "char"
        elif char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return pos + 1
        pos += 1
    raise ValueError("incomplete function body")


def function(source, name):
    match = re.search(r"(?m)^" + re.escape(name) + r"\(", source)
    if match is None:
        raise ValueError(f"missing function {name}")
    start = source.rfind("\n", 0, match.start() - 1) + 1
    body = source.index("{", match.end())
    return source[start:block_end(source, body)] + "\n"


def extract(source):
    definitions = "\n".join(re.findall(r"(?m)^#define kBL[^\n]+", source)) + "\n"
    table_start = source.index("static const uint32_t kBLMarketingMilliNits[]")
    definitions += source[table_start:source.index(";", table_start) + 1] + "\n"
    state_start = source.index("static struct {")
    state_end = block_end(source, source.index("{", state_start))
    state_end = source.index(";", state_end) + 1
    names = (
        "range", "AppleARMBacklight::publish", "list_keys", "bl_print",
        "bl_log", "bl_take", "AppleARMBacklight::setProperties",
        "MiniAppleARMBacklightFramebufferProperties",
    )
    return definitions + function(source, "fixed_nits") + source[state_start:state_end] + "\n" + "\n".join(
        function(source, name) for name in names
    )


def replace(source, before, after):
    if source.count(before) != 1:
        raise ValueError(f"mutation not unique: {before!r}")
    return source.replace(before, after, 1)


def mutations(source):
    plain = "millinits = (n16 < kBLMilliNitsMax / 1000 ? n16 : kBLMilliNitsMax / 1000) * 1000;"
    fixed = "millinits = (n16 < fixed_nits(kBLMilliNitsMax) ? n16 : fixed_nits(kBLMilliNitsMax)) * 1000 >> 16;"
    linear = (
        "\tif (slider_moved) {\n"
        "\t\tmillinits = kBLMilliNitsMin + slider_pos * (kBLMilliNitsMax - kBLMilliNitsMin) / 65536;\n"
        "\t\tv = kBLValueMin + (millinits - kBLMilliNitsMin) * (kBLValueMax - kBLValueMin) / (kBLMilliNitsMax - kBLMilliNitsMin);\n"
        "\t} else if (nits != NULL) {\n\t\tuint64_t n16 ="
    )
    return {
        "dictionary_uses_fixed": replace(source, "plain_nits = nits != NULL;", "plain_nits = false;"),
        "nvram_uses_plain": replace(source,
            'nits = OSDynamicCast(OSNumber, dict->getObject("DisplayBrightnessNitsNVRAM"));',
            'nits = OSDynamicCast(OSNumber, dict->getObject("DisplayBrightnessNitsNVRAM"));\n\t\t\tplain_nits = nits != NULL;'),
        "physical_priority_removed": source.replace('db_dict->getObject("NitsPhysical")', 'db_dict->getObject("TEMP")').replace(
            'db_dict->getObject("Nits")', 'db_dict->getObject("NitsPhysical")').replace(
            'db_dict->getObject("TEMP")', 'db_dict->getObject("Nits")'),
        "plain_wrong_scale": replace(source, plain, plain[:-5] + "100;"),
        "fixed_uses_plain": replace(source, fixed, plain),
        "plain_clamp_removed": replace(source, plain, "millinits = n16 * 1000;"),
        "fixed_clamp_removed": replace(source, fixed, "millinits = n16 * 1000 >> 16;"),
        "low_fixed_discarded": replace(source, "if (s16 <= 0)", "if (s16 < 65536)"),
        "dictionary_overrides_top": replace(source,
            "if (value_key != NULL && nits == NULL && value == NULL)", "if (value_key != NULL)"),
        "off_treated_as_min": replace(source, "if (s16 <= 0)", "if (false)"),
        "persist_idle": replace(source, "if (touch_recent || slider_moved)", "if (true)"),
        "persist_removed": replace(source,
            "ramfs_backlight_save((uint32_t) v, (uint32_t) millinits);", "(void) v;"),
        "mid_brightness_guess_restored": replace(source, fixed,
            fixed + "\n\t\t\tif (n16 == fixed_nits(kBLMilliNitsMid)) millinits = kBLMilliNitsMin;"),
        "slider_linear_override_restored": replace(source,
            "\tif (nits != NULL) {\n\t\tuint64_t n16 =", linear),
        "slider_tracking_removed": replace(source, "if (diff >= 64)", "if (false)"),
        "slider_clamp_removed": replace(source, "slider_pos = pos > 65536 ? 65536 : pos;", "slider_pos = pos;"),
    }


def run(name, source, output, compiler):
    directory = output / name
    directory.mkdir(parents=True, exist_ok=True)
    included = extract(source)
    (directory / "compiled-backlight.inc").write_text(included)
    executable = directory / "fixture"
    command = [
        compiler, "-std=c++17", "-O1", "-g", "-fsanitize=address,undefined",
        "-fno-omit-frame-pointer", "-Wno-format", str(FIXTURE),
        "-I", str(directory), "-o", str(executable),
    ]
    compiled = subprocess.run(command, text=True, capture_output=True, timeout=60)
    (directory / "compile.log").write_text(compiled.stdout + compiled.stderr)
    if compiled.returncode:
        raise RuntimeError(f"{name} failed to compile; see {directory / 'compile.log'}")
    result = subprocess.run([str(executable)], text=True, capture_output=True, timeout=10)
    (directory / "result.log").write_text(result.stdout + result.stderr)
    failures = [line for line in result.stdout.splitlines() if line.startswith("FAIL ")]
    passes = [line for line in result.stdout.splitlines() if line.startswith("PASS ")]
    summary = {
        "exit_code": result.returncode,
        "passed": len(passes),
        "failed": len(failures),
        "failures": failures,
        "source_sha256": hashlib.sha256(source.encode()).hexdigest(),
        "included_sha256": hashlib.sha256(included.encode()).hexdigest(),
    }
    print(f"{name}: {len(passes)} passed, {len(failures)} failed", flush=True)
    return summary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", type=Path, default=Path(__file__).resolve().parents[2],
                        help="kernel repository or a directory with the same source layout")
    parser.add_argument("--baseline-ref", help="Git revision whose backlight source must expose a regression")
    parser.add_argument("--mutations", action="store_true", help="compile and reject protection-removal variants")
    parser.add_argument("--output-dir", type=Path, help="store compiler and fixture outputs here; default is a temporary directory")
    args = parser.parse_args()
    compiler = shutil.which("clang++")
    if compiler is None:
        parser.error("clang++ is required")
    root = args.source_root.resolve()
    output = args.output_dir.resolve() if args.output_dir else Path(tempfile.mkdtemp(prefix="backlight-host-"))
    output.mkdir(parents=True, exist_ok=True)
    source = (root / SOURCE).read_text()
    results = {"source": run("source", source, output, compiler)}
    if args.baseline_ref:
        baseline = subprocess.run(
            ["git", "-C", str(root), "cat-file", "blob", f"{args.baseline_ref}:{SOURCE.as_posix()}"],
            text=True, capture_output=True, check=True, timeout=10,
        ).stdout
        results["baseline"] = run("baseline", baseline, output, compiler)
    if args.mutations and results["source"]["exit_code"] == 0:
        for name, variant in mutations(source).items():
            results[name] = run(name, variant, output, compiler)
    (output / "results.json").write_text(json.dumps(results, indent=2) + "\n")
    print(f"outputs: {output}")
    if results["source"]["exit_code"] != 0:
        raise SystemExit("backlight source failed")
    if args.baseline_ref and results["baseline"]["failed"] == 0:
        raise SystemExit("baseline did not expose a regression")
    if any(item["failed"] == 0 for name, item in results.items() if name not in ("source", "baseline")):
        raise SystemExit("a protection variant unexpectedly passed")


if __name__ == "__main__":
    main()
