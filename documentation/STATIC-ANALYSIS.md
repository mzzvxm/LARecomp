# Static analysis (optional)

`tools/static_analysis/static_analysis.py` reads the game's own C++ sources with LLVM 23's analyzers and reports mistakes the compiler does not: a reference, iterator or view used after the object behind it was moved, resized or destroyed, memory used after it is released, null and uninitialized reads, calls that are not thread safe.

It is an optional developer tool. The game's normal build does not need LLVM 23, and nothing in CMake runs the check.

## What it checks

Over the hand-written sources under `src/` (generated code and `thirdparty/` are left out; headers under `src/` are checked through the sources that include them):

- **Lifetime safety**: `clang++ -Wlifetime-safety` and the `-Wdangling` family.
- **The Clang Static Analyzer** (run by clang-tidy): the `clang-analyzer-core`, `cplusplus` and `unix` checks.
- **A short clang-tidy list**: use after move, dangling handles, suspicious code, `concurrency-mt-unsafe` (`getenv`, `localtime` and `strerror` on worker threads; use `src/os/environment.h` for the environment).

The exact lists are `CHECKS` and `WARNINGS` at the top of the script.

## What you need

- **LLVM 23 or newer** (`clang++` and `clang-tidy`). Download the LLVM 23 release for your system from <https://github.com/llvm/llvm-project/releases> and unpack it anywhere. The script looks for it in this order:
  1. `--llvm-bin <bin folder>`
  2. the `LARECOMP_LLVM23_BIN` environment variable
  3. `clang++` and `clang-tidy` on `PATH`

  It stops with a message if it finds none, or finds an older LLVM (the Clang that Visual Studio ships is older).
- **A configured and built build directory** of this checkout (see the README). The check reads that directory's compile commands and generated headers and builds nothing. It uses the only directory under `out/build` (or the only `-relwithdebinfo` one); otherwise pass `--build out/build/<name>`.
- Python 3.11 or newer. On Windows a normal PowerShell is enough: the Visual Studio headers are found without a Developer prompt.

## Any new findings?

```powershell
$env:LARECOMP_LLVM23_BIN = "C:\llvm-23\bin"      # once per shell, unless LLVM is on PATH
python tools\static_analysis\static_analysis.py --new-only
```

`--new-only` compares the result with `tools/static_analysis/baseline.json`, the findings that were reviewed and are not bugs. The exit code is 0 for clean, 1 for new findings and 2 when the run was incomplete (a tool or source could not run, or the build directory lacks a source; the lines above the verdict say which). A new finding prints as:

```
NEW src/gpu/example.cpp:42:9 [lifetime/-Wlifetime-safety-use-after-free] allocated object does not live long enough (id 1a2b3c4d5e6f)
```

One file, for a quick look while you work (a few seconds):

```powershell
python tools\static_analysis\static_analysis.py --only src/gpu/example.cpp --new-only
```

`--plan` lists what would be analysed without running LLVM. `selftest` checks the tool itself (`selftest --probe-tools` also runs LLVM on a small made-up source).

Time and memory: one file takes 3 to 15 seconds. The whole tree (about 775 sources, some analysed under several configurations) took about 30 minutes with 7 jobs on a fast desktop; expect 30 to 60 minutes. Each job needs about 4 GB. The script starts at most 4 jobs by default and fewer when free memory is low (it keeps 8 GB free); `--jobs N` changes the cap, never past what memory allows. Close the game and other heavy programs before a full scan.

## When it reports something

Read the trace (`--out out/static-scan.json` writes every finding with its notes; `out/` is not tracked). If it is a real bug, **fix the code**. A real bug is never added to the baseline.

If it is not a bug (a false positive, or style that is fine here), add it to the baseline with the reason, so the next run stays quiet:

```powershell
python tools\static_analysis\static_analysis.py --only src/gpu/example.cpp --out out/static-scan.json
python tools\static_analysis\static_analysis.py accept --report out/static-scan.json --id 1a2b3c4d5e6f `
    --category false-positive --reason "The later read uses the container itself, not an iterator from before the append."
```

- `--id` is the id printed on the `NEW` line (8 or more characters of it, unique in the report; repeat `--id` for several findings with the same reason).
- `--category` is `false-positive` or `style`. `--reason` is required: say why it is not a bug, in a sentence.
- The entry is added to `baseline.json` (sorted, LF line endings). Commit it with the change. The id is made from the file path, the check, the message and the text of the source line, so it survives edits elsewhere in the file but not an edit of that line; the same line then needs a fresh look.
- If the same finding appears more often than the baseline expects, run `accept` again with the same id: the count is updated and the reason stays.

The baseline was reviewed with LLVM 23.1.6. A newer LLVM can report something differently; the script prints a note when the versions differ. Review such a finding like any other.
