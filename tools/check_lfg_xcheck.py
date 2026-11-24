r"""check_lfg_xcheck.py: the game's side of the launcher-follows-the-game gate, in one run  (dev only; builds only small standalone programs, never the game)

  python -I tools\check_lfg_xcheck.py [--out <dir>] [--skip-build]

Runs, in order, and prints one line each (exit 1 when any fails; every step runs whatever the one before said):
  1. the static lints that read the menu sources and the rules: check_settings_exposure, check_menu_rows, check_row_tags, check_live_readers, src/settings/ui_rules_lint;
  2. the evaluator against the shared vectors (src/settings/ui_expr_test.cpp: the 397 expressions and the row cases of documentation/contract/condition-tests.json);
  3. the exporter's own test (src/settings_ui/export_test.cpp);
  4. the real dump (tools/check_settings_dump.py: the registry, the menu tables, the rules; twice, the same bytes; the contract validator; the width and vocabulary lints);
  5. the lints against broken dumps (tools/check_settings_ui_lints.py: 27 breaks, every one must be reported);
  6. the drift report (tools/lfg_drift.py: every setting the menu tables name is in the dump);
  7. the fleet: fleet_test makes the machine.json files with the game's own writer and rules, the contract validator accepts them, and they are the files in documentation/contract/examples/fleet;
  8. the cross-check vectors: ui_xcheck_vectors_test runs every expression of the dump through the game's evaluator over the fleet and writes <out>\xcheck-vectors.json for the launcher's tests.
The programs are built with clang from the labuild tools into <out> (default C:\mcv\larecomp\perf\lfg-xcheck\bin). --skip-build reuses them.
"""
import argparse
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
CLANG = r"C:\mcv\larecomp\labuild\tools\clang+llvm-20.1.8-x86_64-pc-windows-msvc\bin\clang++.exe"
TOML = r"C:\mcv\larecomp\native\tree-t11\thirdparty\tomlplusplus\include"
DUMP_DIR = r"C:\mcv\larecomp\perf\lfg-xcheck\dump"
ONLINE = ["src/mc_engine/online/server_browser/browser_query.cpp", "src/mc_engine/online/relay_info.cpp", "src/mc_engine/online/rate_my_ride/rmr_net.cpp",
          "src/mc_engine/online/rate_my_ride/rmr_protocol.cpp"]

results = []


def run(argv, cwd=ROOT, timeout=900):
    r = subprocess.run(argv, cwd=cwd, capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=timeout)
    return r.returncode, (r.stdout or "") + (r.stderr or "")


def step(name, ok, detail):
    results.append((name, ok, detail))
    print(("PASS  " if ok else "FAIL  ") + name + (": " + detail if detail else ""))
    sys.stdout.flush()


def last_line(text):
    lines = [l for l in text.strip().splitlines() if l.strip()]
    return lines[-1][:200] if lines else ""


def compile_run(name, sources, out, args, extra=(), run_args=()):
    exe = os.path.join(out, name + ".exe")
    if not args.skip_build:
        cmd = [CLANG, "-fuse-ld=lld", "-std=c++23", "-Isrc", "-isystem", TOML, *sources, *extra, "-o", exe]
        code, text = run(cmd)
        if code:
            return code, "does not build: " + last_line(text)
    return run([exe, *run_args])


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default=r"C:\mcv\larecomp\perf\lfg-xcheck\bin")
    ap.add_argument("--skip-build", action="store_true")
    args = ap.parse_args()
    out = os.path.abspath(args.out)
    os.makedirs(out, exist_ok=True)
    py = [sys.executable, "-I"]

    for tool in ("check_settings_exposure.py", "check_menu_rows.py", "check_row_tags.py", "check_live_readers.py"):
        code, text = run(py + [os.path.join("tools", tool)])
        step("lint " + tool, code == 0, last_line(text))
    code, text = run(py + [os.path.join("src", "settings", "ui_rules_lint.py")])
    step("lint src/settings/ui_rules_lint.py", code == 0, last_line(text))

    code, text = compile_run("ui_expr_test", ["src/settings/ui_expr_test.cpp"], out, args, run_args=[os.path.join(ROOT, "documentation", "contract")])
    step("the evaluator against the shared vectors", code == 0 and "0 failed" in text, last_line(text))

    code, text = compile_run("export_test", ["src/settings_ui/export.cpp", "src/settings_ui/export_test.cpp", "src/os/file_replace.cpp"], out, args,
                             run_args=[os.path.join(out, "export_test.json")])
    step("the exporter's test", code == 0 and "0 failed" in text, last_line(text))

    code, text = run(py + [os.path.join("tools", "check_settings_dump.py"), "--out", DUMP_DIR] + (["--no-build"] if args.skip_build else []))
    step("the real dump: made twice with the same bytes, the contract validator and the width and vocabulary lints clean, every row of the tables in it", code == 0, last_line(text))
    for l in text.splitlines():
        if l.startswith("FAIL"):
            print("      " + l[:260])
    dump = os.path.join(DUMP_DIR, "settings-ui.json")

    code, text = run(py + [os.path.join("tools", "check_settings_ui_lints.py"), dump])
    step("the lints report every break of the dump", code == 0, last_line(text))

    code, text = run(py + [os.path.join("tools", "lfg_drift.py"), dump])
    step("every setting the menu tables name is in the dump", code == 0, text.splitlines()[0][:200] if text else "")

    fleet_dir = os.path.join(out, "fleet")
    code, text = compile_run("fleet_test", ["src/machine_facts/fleet_test.cpp", "src/machine_facts/document.cpp", "src/os/file_replace.cpp", *ONLINE], out, args,
                             extra=["-lws2_32", "-Isrc/machine_facts"], run_args=[fleet_dir])
    ok = code == 0 and "0 failed" in text
    if ok:
        files = sorted(f for f in os.listdir(fleet_dir) if f.endswith(".machine.json"))
        committed = os.path.join(ROOT, "documentation", "contract", "examples", "fleet")
        same = files == sorted(os.listdir(committed)) and all(
            open(os.path.join(fleet_dir, f), "rb").read() == open(os.path.join(committed, f), "rb").read() for f in files)
        code, v = run(py + [os.path.join("documentation", "contract", "validate_contract.py"), "--strict", "--quiet"] + [os.path.join(fleet_dir, f) for f in files])
        step("the fleet: the game's writer makes the committed files, and the validator accepts them", code == 0 and same, f"{len(files)} files, " + ("same as committed" if same else "DIFFERENT from the committed ones"))
    else:
        step("the fleet", False, last_line(text))

    vectors = os.path.join(os.path.dirname(out), "out", "xcheck-vectors.json")
    code, text = compile_run("ui_xcheck_vectors", ["src/settings/ui_xcheck_vectors_test.cpp"], out, args,
                             run_args=[dump, os.path.join(ROOT, "documentation", "contract", "examples", "fleet"), vectors])
    step("the cross-check vectors are made", code == 0, last_line(text))

    failed = [r for r in results if not r[1]]
    print(f"{len(results) - len(failed)} of {len(results)} steps passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
