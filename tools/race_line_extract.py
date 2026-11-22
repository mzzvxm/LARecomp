r"""race_line_extract.py: copy the recompiled Race Line functions that race_line_test.cpp compares against out of a generated tree (dev tool, reads files, builds nothing).

  python tools\race_line_extract.py <generated dir> <out dir>

The unit test of the native Race Line passes (src/mc_engine/hooks/race_line_native.h) proves them against the game's own recompiled functions. Those are the game's code
translated by the SDK, so they never go into a commit: this script copies them from a generated folder on the dev PC (for example
C:\mcv\larecomp\mingw011\src\generated, a rexglue 0.11 codegen run of the project) into <out dir>\rl_originals.inc, which the test compiles in with -DRL_WITH_ORIGINALS -I <out dir>.
Without that define the test still runs, against its reference checks only.

rl_originals.inc holds the whole "UpdateRaceLine" job: rex_sub_82171078 and everything it calls (30 functions: the optimizer loop 82171078 / 82170B98 / 82170F38, the path chain
8216F900 / 8216DD38 / 8216E408 / 8216CBC0 / 8216CDD0 and their helpers, memcpy, memset, acos), found by following the calls in the generated text. It is written twice, as two
sets of plain functions with the same bodies:
  orig_<addr>   every call goes to the recompiled copy (the oracle);
  twin_<addr>   the calls to 8216CBC0 and 8216CDD0 go to twin_hook_8216CBC0 / twin_hook_8216CDD0 (the test defines them: they run the native pass), everything else as orig_.
The prologue / epilogue helpers (__savegprlr_N, __savefpr_N ...) are declared extern "C": the test defines them. A callee that is not copied would be a link error, which is the point:
the test then knows that the job reached something it did not model.
"""
import re
import sys
from pathlib import Path

ROOTS = ("82171078", "8216CBC0", "8216CDD0")
# calls that the "twin" copy sends to the native passes instead of the recompiled copy
TWIN_HOOKS = {"8216CBC0": "twin_hook_8216CBC0", "8216CDD0": "twin_hook_8216CDD0"}
CALL = re.compile(r"\b(?:rex_)?sub_([0-9A-F]{8})\(ctx, base\)")


def index_generated(gen: Path):
    idx = {}
    pat = re.compile(r"^DEFINE_REX_FUNC\((?:rex_)?sub_([0-9A-F]{8})\) \{$", re.M)
    for f in sorted(gen.glob("larecomp_recomp.*.cpp")):
        text = f.read_text(encoding="utf-8", errors="replace")
        for m in pat.finditer(text):
            end = text.find("\n}\n", m.end())
            if end < 0:
                raise SystemExit(f"{f}: no end of function at {m.group(1)}")
            idx[m.group(1)] = text[m.start():end + 3]
    return idx


def closure(idx, roots):
    seen, stack = {}, list(roots)
    while stack:
        a = stack.pop()
        if a in seen:
            continue
        if a not in idx:
            raise SystemExit(f"function {a} is called but not found in the generated tree")
        seen[a] = idx[a]
        for m in CALL.finditer(idx[a]):
            stack.append(m.group(1))
    return seen


def rename(body: str, addr: str, prefix: str, known, hooks):
    body = re.sub(r"^DEFINE_REX_FUNC\((?:rex_)?sub_" + addr + r"\) \{",
                  "extern \"C\" void " + prefix + addr + "(PPCContext& __restrict ctx, u8* base) {", body, count=1, flags=re.M)

    def call(m):
        a = m.group(1)
        if a in hooks:
            return hooks[a] + "(ctx, base)"
        return prefix + a + "(ctx, base)"
    return CALL.sub(call, body)


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    gen, out = Path(sys.argv[1]), Path(sys.argv[2]) / "rl_originals.inc"
    out.parent.mkdir(parents=True, exist_ok=True)
    idx = index_generated(gen)
    funcs = closure(idx, ROOTS)
    parts = ["// Written by tools/race_line_extract.py from " + str(gen) + ": the game's recompiled code. NOT for a commit.\n",
             f"// {len(funcs)} functions, twice (orig_ and twin_).\n"]
    for a in funcs:
        parts.append(f"extern \"C\" void orig_{a}(PPCContext& __restrict ctx, u8* base);\n")
        parts.append(f"extern \"C\" void twin_{a}(PPCContext& __restrict ctx, u8* base);\n")
    for h in TWIN_HOOKS.values():
        parts.append(f"extern \"C\" void {h}(PPCContext& __restrict ctx, u8* base);\n")
    for prefix, hooks in (("orig_", {}), ("twin_", TWIN_HOOKS)):
        for a, body in funcs.items():
            parts.append(rename(body, a, prefix, funcs, hooks))
    out.write_text("".join(parts), encoding="utf-8")
    print(f"{len(funcs)} functions -> {out} ({out.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
