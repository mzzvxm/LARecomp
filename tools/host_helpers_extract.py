r"""host_helpers_extract.py: copy the recompiled originals that host_helpers_test.cpp compares against out of a generated tree (dev tool, reads files, builds nothing).

  python tools\host_helpers_extract.py <generated dir> <out dir>

The unit test of the native helpers (src/mc_engine/hooks/host_helpers_test.cpp) proves them against the game's own recompiled functions. Those are the game's code
translated by the SDK, so they never go into a commit: this script copies them from a generated folder on the dev PC (for example
C:\mcv\larecomp\mingw011\src\generated, a rexglue 0.11 codegen run of the project) into <out dir>\hh_originals.inc, which the test compiles in with
-DHH_WITH_ORIGINALS -I <out dir>. Without that define the test still runs, against its plain reference models only.

The include file holds, for each function of FUNCS, its definition under the name `orig_<name>` (the generated `DEFINE_REX_FUNC(name)` is rewritten to a plain function
with the same body, so nothing is weak or aliased) and the callees it needs: other recompiled functions listed in FUNCS are copied the same way and calls to them
are renamed; a callee that is NOT copied becomes a stub that counts the call (hh_stub_calls) so the test can tell that the original took a path the native version
declines (the ring-wrap routine 82422EF8 for the constant bank flush). The prologue / epilogue helpers __savegprlr_N / __restgprlr_N are the template
rex_savegprlr<N> of the generated header with an aborting fallback.
"""
import re
import sys
from pathlib import Path

# name in the generated code -> test-side name; the order does not matter
FUNCS = {
    "rex_sub_8244D5C0": "memcpy",
    "rex_sub_823DADD8": "memcpy_words",
    "rex_sub_824593D0": "memcpy_blocks",
    "rex_sub_823D9890": "memset",
    "rex_sub_8218A6E0": "row_write",
    "rex_sub_82417678": "shader_const_f",
    "D3DDevice_FlushConstantBank": "flush_bank",
}
# callees that are stubs in the test
STUBS = ("rex_sub_82422EF8",)


def find_function(gen: Path, name: str) -> str:
    pat = re.compile(r"^DEFINE_REX_FUNC\(" + re.escape(name) + r"\) \{\n", re.M)
    for f in sorted(gen.glob("larecomp_recomp.*.cpp")):
        text = f.read_text(encoding="utf-8", errors="replace")
        m = pat.search(text)
        if not m:
            continue
        end = text.find("\n}\n", m.end())
        if end < 0:
            raise SystemExit(f"{f}: no end of {name}")
        return text[m.start():end + 3]
    raise SystemExit(f"{name} not found in {gen}")


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    gen, out = Path(sys.argv[1]), Path(sys.argv[2]) / "hh_originals.inc"
    out.parent.mkdir(parents=True, exist_ok=True)
    parts = ["// Written by tools/host_helpers_extract.py from " + str(gen) + ": the game's recompiled code. NOT for a commit.\n"]
    for name in FUNCS:
        body = find_function(gen, name)
        body = body.replace("DEFINE_REX_FUNC(" + name + ") {", "extern \"C\" void orig_" + FUNCS[name] + "(PPCContext& __restrict ctx, u8* base) {", 1)
        for other, short in FUNCS.items():
            body = re.sub(r"\b" + re.escape(other) + r"\(ctx, base\)", "orig_" + short + "(ctx, base)", body)
        for stub in STUBS:
            body = re.sub(r"\b" + re.escape(stub) + r"\(ctx, base\)", "hh_stub_" + stub[-8:] + "(ctx, base)", body)
        # the weak tail calls: REX_MUSTTAIL return f(ctx, base); is the same call
        parts.append(body)
    out.write_text("\n".join(parts), encoding="utf-8")
    print(f"wrote {out}: {len(FUNCS)} functions")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
