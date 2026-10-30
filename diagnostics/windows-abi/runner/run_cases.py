#!/usr/bin/env python3
"""Run the Windows BonDriver ABI probe cases and classify the observations.

Controls (same toolchain for host and DLL) must pass.  Cross-toolchain
results are recorded as observations: a failure there is the experiment's
result, not a reason to hide it.  Toolchain/fixture failures are reported
separately and fail the gate so they are not misread as ABI evidence.
"""

import argparse
import json
import os
import subprocess
import sys
import time
from pathlib import Path

CASES = [
    ("control_msvc_direct", "msvc", "msvc", "direct"),
    ("control_msvc_struct", "msvc", "msvc", "struct"),
    ("control_mingw_direct", "mingw", "mingw", "direct"),
    ("control_mingw_struct", "mingw", "mingw", "struct"),
    ("cross_msvc_host_mingw_dll_direct", "msvc", "mingw", "direct"),
    ("cross_msvc_host_mingw_dll_struct", "msvc", "mingw", "struct"),
    ("cross_mingw_host_msvc_dll_direct", "mingw", "msvc", "direct"),
    ("cross_mingw_host_msvc_dll_struct", "mingw", "msvc", "struct"),
]

GATE_FAILURE_STATUSES = {"LOAD_OR_SYMBOL_FAIL", "NO_RELEASE_MARKER", "UNKNOWN"}


def rc_hex(returncode):
    if returncode is None:
        return "timeout"
    return "{} (0x{:08X})".format(returncode, returncode & 0xFFFFFFFF)


def as_text(value):
    if value is None:
        return ""
    if isinstance(value, bytes):
        return value.decode("utf-8", errors="replace")
    return value


def parse_lines(stdout):
    last_stage = ""
    result_line = ""
    build_host = ""
    build_dll = ""
    cast_line = ""
    failed_checks = []
    for raw in stdout.splitlines():
        line = raw.rstrip()
        if line.startswith("STAGE "):
            last_stage = line[len("STAGE "):]
        elif line.startswith("RESULT "):
            result_line = line[len("RESULT "):]
        elif line.startswith("BUILD host "):
            build_host = line[len("BUILD host "):]
        elif line.startswith("BUILD dll "):
            build_dll = line[len("BUILD dll "):]
        elif line.startswith("CAST "):
            cast_line = line[len("CAST "):]
        elif line.startswith("CHECK ") and line.endswith(" FAIL"):
            failed_checks.append(line[len("CHECK "):-len(" FAIL")])
    return {
        "last_stage": last_stage,
        "result": result_line,
        "build_host": build_host,
        "build_dll": build_dll,
        "cast": cast_line,
        "failed_checks": failed_checks,
    }


def classify(returncode, timed_out, marker_ok, stdout):
    if timed_out:
        return "TIMEOUT"
    if returncode is None:
        return "UNKNOWN"
    if returncode == 0:
        return "PASS" if marker_ok else "NO_RELEASE_MARKER"
    if returncode == 3:
        return "CAST_NULL"
    if returncode == 4:
        return "MISMATCH"
    if returncode == 2:
        return "LOAD_OR_SYMBOL_FAIL"
    if returncode == 5:
        # The host reserves 5 for a caught C++ exception and prints the
        # EXCEPTION line.  A bare 5 is a loader/process failure, not a
        # caught exception.
        return "EXCEPTION" if "EXCEPTION " in stdout else "CRASH_OR_ERROR"
    return "CRASH_OR_ERROR"


def write_log(path, record):
    with path.open("w", encoding="utf-8") as handle:
        handle.write("CASE {}\n".format(record["name"]))
        handle.write("COMMAND {}\n".format(" ".join(record["command"])))
        handle.write("HOST_BUILD {}\n".format(record["build_host"] or "unknown"))
        handle.write("DLL_BUILD {}\n".format(record["build_dll"] or "unknown"))
        handle.write("STATUS {}\n".format(record["status"]))
        handle.write("EXIT {}\n".format(record["exit"]))
        handle.write("DURATION {:.3f}s\n".format(record["duration"]))
        handle.write("LAST_STAGE {}\n".format(record["last_stage"] or "none"))
        handle.write("MARKER {}\n".format("present" if record["marker_ok"] else "absent"))
        handle.write("CAST {}\n".format(record["cast"] or "none"))
        handle.write("RESULT {}\n".format(record["result"] or "none"))
        if record["failed_checks"]:
            handle.write("FAILED_CHECKS {}\n".format(", ".join(record["failed_checks"])))
        handle.write("STDOUT\n")
        handle.write(record["stdout"])
        if record["stdout"] and not record["stdout"].endswith("\n"):
            handle.write("\n")
        handle.write("STDERR\n")
        handle.write(record["stderr"])
        if record["stderr"] and not record["stderr"].endswith("\n"):
            handle.write("\n")


def run_case(name, host_key, dll_key, mode, paths, outdir, timeout):
    host = paths[host_key]["host"]
    dll = paths[dll_key]["dll"]
    marker = outdir / "markers" / (name + ".marker")
    if marker.exists():
        marker.unlink()
    marker.parent.mkdir(parents=True, exist_ok=True)

    env = os.environ.copy()
    env["ABI_PROBE_MARKER"] = str(marker.resolve())
    command = [str(host), mode, str(dll)]

    print("== {} ({} host + {} dll, {})".format(name, host_key, dll_key, mode), flush=True)
    started = time.monotonic()
    timed_out = False
    returncode = None
    stdout = ""
    stderr = ""
    try:
        completed = subprocess.run(
            command,
            cwd=str(outdir),
            env=env,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=timeout,
        )
        returncode = completed.returncode
        stdout = as_text(completed.stdout)
        stderr = as_text(completed.stderr)
    except subprocess.TimeoutExpired as exc:
        timed_out = True
        stdout = as_text(exc.stdout)
        stderr = as_text(exc.stderr)
        print("   TIMEOUT after {:.0f}s".format(timeout), flush=True)

    duration = time.monotonic() - started
    marker_text = ""
    if marker.exists():
        marker_text = marker.read_text(encoding="utf-8", errors="replace")
    marker_ok = "release" in marker_text
    parsed = parse_lines(stdout)
    status = classify(returncode, timed_out, marker_ok, stdout)

    record = {
        "name": name,
        "group": "control" if host_key == dll_key else "cross",
        "host": host_key,
        "dll": dll_key,
        "mode": mode,
        "command": command,
        "exit": rc_hex(returncode),
        "duration": duration,
        "last_stage": parsed["last_stage"],
        "result": parsed["result"],
        "build_host": parsed["build_host"],
        "build_dll": parsed["build_dll"],
        "cast": parsed["cast"],
        "failed_checks": parsed["failed_checks"],
        "marker_ok": marker_ok,
        "marker_text": marker_text.strip(),
        "status": status,
        "stdout": stdout,
        "stderr": stderr,
    }
    write_log(outdir / "logs" / (name + ".log"), record)
    print("   status={} exit={} stage={} marker={}".format(
        status, record["exit"], record["last_stage"] or "none",
        "present" if marker_ok else "absent"), flush=True)
    return record


def markdown_summary(records, gate_ok, toolchain_errors):
    lines = []
    lines.append("## Windows BonDriver ABI probe")
    lines.append("")
    lines.append("- controls gate: **{}**".format("PASS" if gate_ok else "FAIL"))
    lines.append("- toolchain/fixture errors: **{}**".format(len(toolchain_errors)))
    lines.append("- control cases: {}".format(
        ", ".join(r["name"] for r in records if r["group"] == "control")))
    lines.append("")
    lines.append("| case | group | host | dll | mode | status | exit | last stage | marker |")
    lines.append("|---|---|---|---|---|---|---|---|---|")
    for record in records:
        lines.append("| {} | {} | {} | {} | {} | {} | {} | {} | {} |".format(
            record["name"], record["group"], record["host"], record["dll"],
            record["mode"], record["status"], record["exit"],
            record["last_stage"] or "none",
            "yes" if record["marker_ok"] else "no"))
    lines.append("")
    for record in records:
        lines.append("- **{}**: host build `{}`; DLL build `{}`".format(
            record["name"], record["build_host"] or "unknown",
            record["build_dll"] or "unknown"))
        if record["cast"]:
            lines.append("  - cast: `{}`".format(record["cast"]))
        if record["failed_checks"]:
            lines.append("  - failed checks: {}".format(", ".join(record["failed_checks"])))
    lines.append("")
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--msvc-host", required=True)
    parser.add_argument("--msvc-dll", required=True)
    parser.add_argument("--mingw-host", required=True)
    parser.add_argument("--mingw-dll", required=True)
    parser.add_argument("--outdir", required=True)
    parser.add_argument("--timeout", type=float, default=30.0)
    args = parser.parse_args()

    paths = {
        "msvc": {"host": Path(args.msvc_host).resolve(), "dll": Path(args.msvc_dll).resolve()},
        "mingw": {"host": Path(args.mingw_host).resolve(), "dll": Path(args.mingw_dll).resolve()},
    }
    missing = []
    for key, pair in paths.items():
        for kind, path in pair.items():
            if not path.is_file():
                missing.append("{} {}: {}".format(key, kind, path))
    if missing:
        for item in missing:
            print("MISSING {}".format(item), file=sys.stderr)
        return 1

    outdir = Path(args.outdir).resolve()
    (outdir / "logs").mkdir(parents=True, exist_ok=True)
    (outdir / "markers").mkdir(parents=True, exist_ok=True)

    print("MSVC host:  {}".format(paths["msvc"]["host"]))
    print("MSVC dll:   {}".format(paths["msvc"]["dll"]))
    print("MinGW host: {}".format(paths["mingw"]["host"]))
    print("MinGW dll:  {}".format(paths["mingw"]["dll"]))
    print("timeout:    {:.0f}s per case".format(args.timeout), flush=True)

    records = []
    for name, host_key, dll_key, mode in CASES:
        records.append(run_case(
            name, host_key, dll_key, mode, paths, outdir, args.timeout))

    controls = [r for r in records if r["group"] == "control"]
    controls_ok = all(r["status"] == "PASS" for r in controls)
    toolchain_errors = [r for r in records if r["status"] in GATE_FAILURE_STATUSES]
    gate_ok = controls_ok and not toolchain_errors

    summary = markdown_summary(records, gate_ok, toolchain_errors)
    (outdir / "summary.md").write_text(summary, encoding="utf-8")

    plain = ["WINDOWS ABI PROBE SUMMARY",
             "controls_gate: {}".format("PASS" if controls_ok else "FAIL"),
             "gate: {}".format("PASS" if gate_ok else "FAIL"),
             "toolchain_fixture_errors: {}".format(len(toolchain_errors)),
             ""]
    for record in records:
        plain.append(
            "{name}\t{group}\thost={host}\tdll={dll}\tmode={mode}\tstatus={status}\texit={exit}\tstage={stage}\tmarker={marker}".format(
                stage=record["last_stage"] or "none",
                marker="yes" if record["marker_ok"] else "no",
                **record))
    text = "\n".join(plain) + "\n"
    (outdir / "summary.txt").write_text(text, encoding="utf-8")
    (outdir / "summary.json").write_text(
        json.dumps({"gate": gate_ok, "controls": controls_ok,
                    "toolchain_errors": [r["name"] for r in toolchain_errors],
                    "cases": records}, indent=2),
        encoding="utf-8")

    print()
    print(text, flush=True)
    return 0 if gate_ok else 1


if __name__ == "__main__":
    sys.exit(main())
