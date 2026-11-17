#!/usr/bin/env python3
"""LiveWDF Unified Validation Platform (Phase 3).

Executes the Phase 3 validation platform against real amplifier documents:
  1. DC operating point verification against regression threshold (< 1e-6 V).
  2. Multi-rate continuous AC and discretization convergence at 48/96/192 kHz.
  3. Large-signal high-drive stability at 10 mV, 100 mV, and 1.0 V (requiring finite output and bounded convergence).
  4. Plugin vs Engine Reference parity across block sizes (16..1024) (bit-exact / sub-microvolt).
  5. Runtime facade bit-identical parity.
  6. B1 stage physical diagnosis (convex law rectification and DC shifts).
  7. NGSPICE golden-reference comparison when ngspice is installed.
  8. Vendored module provenance and license checks.

Usage:
  python tools/validate.py [--circuit <file>] [--all] [--json <file>] [--no-spice]
"""

import argparse
import json
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path


DEFAULT_BENCH_EXE = Path(
    r"build\plugin\LiveWDF_PluginBench_artefacts\Release\LiveWDF_PluginBench.exe"
)
DEFAULT_NGSPICE_EXE = Path(r"C:\Code\test\Spice64\bin\ngspice_con.exe")
DOCUMENTS_DIR = Path(r"C:\Users\PC\Documents")

DEFAULT_CIRCUITS = [
    DOCUMENTS_DIR / "mark iic+ preamp.livewdf",
    DOCUMENTS_DIR / "mark iic+ 2C+ 2C++ fullamp.livewdf",
]


def check_provenance():
    """Verify license files and notices for vendored third-party modules."""
    results = {"status": "PASS", "checked": []}
    repo_root = Path(__file__).resolve().parent.parent

    required_files = [
        ("JUCE", repo_root / "modules" / "JUCE" / "LICENSE.md"),
    ]

    for name, path in required_files:
        exists = path.is_file()
        size = path.stat().st_size if exists else 0
        valid = exists and size > 100
        results["checked"].append({
            "module": name,
            "path": str(path),
            "exists": exists,
            "valid": valid,
        })
        if not valid:
            results["status"] = "FAIL"

    return results


def run_bench_validation(bench_exe, circuit_path):
    """Run LiveWDF_PluginBench --validate on circuit."""
    cmd = [str(bench_exe), str(circuit_path), "--validate"]
    proc = subprocess.run(cmd, capture_output=True, text=True, check=False)
    output = proc.stdout + "\n" + proc.stderr
    passed = (proc.returncode == 0) and ("FINAL VALIDATION VERDICT: ALL TESTS PASSED" in output)
    return {
        "passed": passed,
        "returncode": proc.returncode,
        "output": output,
    }


def run_b1_diag(bench_exe, circuit_path):
    """Run LiveWDF_PluginBench --b1-diag on circuit."""
    cmd = [str(bench_exe), str(circuit_path), "--b1-diag"]
    proc = subprocess.run(cmd, capture_output=True, text=True, check=False)
    output = proc.stdout + "\n" + proc.stderr
    return {
        "passed": proc.returncode == 0,
        "output": output,
    }


def run_ngspice_compare(bench_exe, ngspice_exe, circuit_path, work_dir):
    """Run ngspice export and comparison on circuit."""
    work_dir = Path(work_dir)
    work_dir.mkdir(parents=True, exist_ok=True)

    # 1. Export SPICE deck
    cmd_spice = [str(bench_exe), str(circuit_path), "--spice", str(work_dir)]
    proc_spice = subprocess.run(cmd_spice, capture_output=True, text=True, check=False)
    if proc_spice.returncode != 0:
        return {"passed": False, "error": f"--spice failed: {proc_spice.stderr}"}

    # Find the generated .cir file
    cir_files = list(work_dir.glob("*.cir"))
    if not cir_files:
        return {"passed": False, "error": "No .cir file found in output"}
    cir_file = cir_files[0]

    # 2. Run ngspice in batch mode
    log_file = work_dir / "spice.log"
    cmd_ngspice = [str(ngspice_exe), "-b", "-o", str(log_file), str(cir_file)]
    proc_ng = subprocess.run(cmd_ngspice, cwd=str(work_dir), capture_output=True, text=True, check=False)
    if proc_ng.returncode != 0:
        return {"passed": False, "error": f"ngspice failed with code {proc_ng.returncode}"}

    # 3. Generate engine probe at 48 kHz
    engine_tran_csv = work_dir / "engine_tran.csv"
    cmd_probe = [str(bench_exe), str(circuit_path), "--probe", str(engine_tran_csv), "--probe-rate", "48000"]
    proc_probe = subprocess.run(cmd_probe, capture_output=True, text=True, check=False)
    if proc_probe.returncode != 0:
        return {"passed": False, "error": f"--probe failed: {proc_probe.stderr}"}

    # 4. Run compare_ngspice.py
    compare_script = Path(__file__).resolve().parent.parent / "plugin" / "compare_ngspice.py"
    cmd_cmp = [sys.executable, str(compare_script), str(work_dir)]
    proc_cmp = subprocess.run(cmd_cmp, capture_output=True, text=True, check=False)
    cmp_output = proc_cmp.stdout + "\n" + proc_cmp.stderr

    # Parse worst DC diff
    worst_dc_match = re.search(r"worst DC \|diff\| = ([0-9.eE+-]+) V", cmp_output)
    worst_dc = float(worst_dc_match.group(1)) if worst_dc_match else None
    dc_passed = worst_dc is not None and worst_dc < 1e-6

    return {
        "passed": dc_passed,
        "worst_dc": worst_dc,
        "dc_passed": dc_passed,
        "output": cmp_output,
    }


def main():
    ap = argparse.ArgumentParser(description="LiveWDF Unified Validation Platform (Phase 3)")
    ap.add_argument("--circuit", help="Path to a single .livewdf circuit document")
    ap.add_argument("--all", action="store_true", help="Validate all standard amp documents")
    ap.add_argument("--json", dest="json_out", help="Write structured results to JSON file")
    ap.add_argument("--bench", default=str(DEFAULT_BENCH_EXE), help="Path to LiveWDF_PluginBench.exe")
    ap.add_argument("--ngspice", default=str(DEFAULT_NGSPICE_EXE), help="Path to ngspice_con.exe")
    ap.add_argument("--no-spice", action="store_true", help="Skip ngspice execution even if installed")
    args = ap.parse_args()

    bench_exe = Path(args.bench).resolve()
    if not bench_exe.is_file():
        # Try relative to repo
        repo_root = Path(__file__).resolve().parent.parent
        bench_exe = (repo_root / args.bench).resolve()
        if not bench_exe.is_file():
            sys.exit(f"Error: bench executable not found at {args.bench}")

    ngspice_exe = Path(args.ngspice)
    has_ngspice = (not args.no_spice) and ngspice_exe.is_file()

    circuits = []
    if args.circuit:
        p = Path(args.circuit)
        if not p.is_file():
            sys.exit(f"Error: circuit document not found: {args.circuit}")
        circuits.append(p)
    else:
        circuits = [c for c in DEFAULT_CIRCUITS if c.is_file()]
        if not circuits:
            sys.exit("Error: no default circuit documents found in Documents directory")

    print("=" * 80)
    print("LIVEWDF PHASE 3 VALIDATION PLATFORM")
    print(f"Bench Binary:   {bench_exe}")
    print(f"NGSPICE Binary: {ngspice_exe if has_ngspice else 'NOT AVAILABLE (skipped)'}")
    print("=" * 80)

    report = {
        "provenance": check_provenance(),
        "circuits": {},
        "overall_status": "PASS",
    }

    print("\n[PROVENANCE & LICENSING AUDIT]")
    prov = report["provenance"]
    for item in prov["checked"]:
        status = "OK" if item["valid"] else "MISSING"
        print(f"  {item['module']:15}: {status} ({item['path']})")
    if prov["status"] != "PASS":
        print("  PROVENANCE CHECK FAILED!")
        report["overall_status"] = "FAIL"
    else:
        print("  PROVENANCE CHECK PASSED: All vendored licenses intact.")

    scratch_base = Path(tempfile.gettempdir()) / "livewdf_validation"
    scratch_base.mkdir(parents=True, exist_ok=True)

    for circuit in circuits:
        c_name = circuit.name
        print("\n" + "=" * 80)
        print(f"VALIDATING CIRCUIT: {c_name}")
        print("=" * 80)

        c_data = {}

        # 1. Bench validation suite (DC, multi-rate AC, high drive, audit, runtime parity)
        bench_res = run_bench_validation(bench_exe, circuit)
        c_data["bench_validation"] = {
            "passed": bench_res["passed"],
        }
        print(bench_res["output"].strip())
        if not bench_res["passed"]:
            report["overall_status"] = "FAIL"

        # 2. B1 Diagnosis (on preamp)
        if "preamp" in c_name.lower():
            b1_res = run_b1_diag(bench_exe, circuit)
            c_data["b1_diag"] = {
                "passed": b1_res["passed"],
            }
            print("\n" + b1_res["output"].strip())

        # 3. NGSPICE cross-check
        if has_ngspice and "preamp" in c_name.lower():
            work_dir = scratch_base / circuit.stem
            spice_res = run_ngspice_compare(bench_exe, ngspice_exe, circuit, work_dir)
            c_data["ngspice_compare"] = spice_res
            print("\n[NGSPICE GOLDEN-REFERENCE AUDIT]")
            if spice_res["passed"]:
                print(f"  PASS: worst DC |diff| = {spice_res.get('worst_dc', 0):.3e} V (< 1e-6 V threshold)")
            else:
                print(f"  FAIL / WARN: {spice_res.get('error', 'DC diff exceeded threshold')}")
                report["overall_status"] = "FAIL"

        report["circuits"][c_name] = c_data

    print("\n" + "=" * 80)
    print(f"OVERALL PLATFORM VERDICT: {report['overall_status']}")
    print("=" * 80)

    if args.json_out:
        with open(args.json_out, "w", encoding="utf-8") as f:
            json.dump(report, f, indent=2)
        print(f"Structured results written to {args.json_out}")

    sys.exit(0 if report["overall_status"] == "PASS" else 1)


if __name__ == "__main__":
    main()
