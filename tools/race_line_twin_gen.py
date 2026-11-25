r"""race_line_twin_gen.py: writes race_line_twin_gen.h in the build directory from the player's recompiled game sources.

  python -I tools\race_line_twin_gen.py <generated dir> <out file>
  python -I tools\race_line_twin_gen.py <generated dir> <out file> --check   exit 1 when <out file> is missing or differs

What it does: the two functions and the 9 helpers they call (the angle functions 8216C900 / 8216C9C0 and the acos behind them, 8216D390; the sample builders
8216F288 / 8216D1C8 / 8216D9A0 / 8216CA78; the compiler's __savefpr_N / __restfpr_N stubs) are copied, instruction for instruction, out of the SDK's generated text of the
game (<generated dir>\larecomp_recomp.*.cpp) into plain C++ over a LOCAL register file instead of the thread's PPCContext. Every statement keeps the recompiler's
meaning (the same single-precision-as-double-rounded-once rule, the same fused multiply-adds, the same simde vector operations, the same flush-mode switches, the
same byte swaps); what changes is only where a guest register lives (a local, so the compiler keeps it in a host register and drops the stores nothing reads),
that loads and stores are plain (the recompiled ones are volatile), that the fused multiply-add is the CPU's instruction, and that the calls between the
functions are inlined. The unit test (race_line_test.cpp, with the originals tools\race_line_extract.py copies out of the same generated text) runs both versions
on the same inputs and compares every byte of memory and every register.

Statements are rewritten by the table below. A statement that matches no rule and is not one of the plain scalar / integer forms stops the run with its text: a new
codegen that emits something else is never copied silently. Keep the generated header in the local build directory.
"""
import argparse
import re
from pathlib import Path

ROOTS = {"e408": "sub_8216E408", "f900": "sub_8216F900"}

NAME = r"(?:sub_[0-9A-F]{8}|__(?:save|rest)(?:fpr|vmx)_\d+)"
DEF = re.compile(r"^DEFINE_REX_FUNC\(((?:rex_)?" + NAME + r")\) \{$", re.M)
CALL = re.compile(r"\b(?:rex_)?(" + NAME + r")\(ctx, base\)")
TAIL = re.compile(r"REX_MUSTTAIL return (?:rex_)?(" + NAME + r")\(ctx, base\);")


def index_generated(gen: Path):
    idx = {}
    for f in sorted(gen.glob("larecomp_recomp.*.cpp")):
        text = f.read_text(encoding="utf-8", errors="replace")
        for m in DEF.finditer(text):
            end = text.find("\n}\n", m.end())
            if end < 0:
                raise SystemExit(f"{f}: no end of function at {m.group(1)}")
            idx[m.group(1).replace("rex_", "")] = text[m.start():end + 3]
    return idx


def closure(idx, root):
    order, seen = [], set()

    def visit(a):
        if a in seen:
            return
        seen.add(a)
        if a not in idx:
            raise SystemExit(f"function {a} is called but not found in the generated tree")
        for m in list(CALL.finditer(idx[a])) + list(TAIL.finditer(idx[a])):
            visit(m.group(1))
        order.append(a)  # callees first

    visit(root)
    return order


# ------------------------------------------------------------------------------------------------ statement rules
V = r"(c\.v\d+)"
SI = r"simde_mm_store_si128\(\(simde__m128i\*\)"
LD = r"simde_mm_load_si128\(\(simde__m128i\*\)"
LDPS = r"simde_mm_load_ps\("
STPS = r"simde_mm_store_ps\("

# (regex over the whole statement text, replacement); the statement has had `ctx.` replaced by `c.`
VECTOR_RULES = [
    (SI + V + r"\.u8, simde_mm_shuffle_epi8\(" + LD + r"REX_RAW_ADDR\(ea\)\), " + LD + r"kVectorMaskL\)\)\);", r"LVX(base, \1, ea);"),
    (SI + r"REX_RAW_ADDR\(ea\), simde_mm_shuffle_epi8\(" + LD + V + r"\.u8\), " + LD + r"kVectorMaskL\)\)\);", r"STVX(base, ea, \1);"),
    (SI + V + r"\.u8, " + LD + V + r"\.u8\)\);", r"VMOV(\1, \2);"),
    (SI + V + r"\.u8, simde_mm_and_si128\(" + LD + V + r"\.u8\), " + LD + V + r"\.u8\)\)\);", r"VAND(\1, \2, \3);"),
    (SI + V + r"\.u8, simde_mm_or_si128\(" + LD + V + r"\.u8\), " + LD + V + r"\.u8\)\)\);", r"VOR(\1, \2, \3);"),
    (SI + V + r"\.u8, simde_mm_andnot_si128\(" + LD + V + r"\.u8\), " + LD + V + r"\.u8\)\)\);", r"VANDC(\1, \3, \2);"),  # andnot(b, a) = a & ~b
    (SI + V + r"\.u8, simde_mm_or_si128\(simde_mm_andnot_si128\(" + LD + V + r"\.u8\), " + LD + V + r"\.u8\)\), simde_mm_and_si128\(" + LD + V + r"\.u8\), " + LD + V + r"\.u8\)\)\)\);",
     r"VSEL(\1, \3, \5, \2);"),
    (SI + V + r"\.u8, simde_mm_setzero_si128\(\)\);", r"VZERO(\1);"),
    (SI + V + r"\.u32, simde_mm_set1_epi32\(int\((\w+)\)\)\);", r"VSPLTISW(\1, \2);"),
    (SI + V + r"\.u32, simde_mm_shuffle_epi32\(" + LD + V + r"\.u32\), (\w+)\)\);", r"VSHUF32(\1, \2, \3);"),
    (SI + V + r"\.u32, simde_mm_unpackhi_epi32\(" + LD + V + r"\.u32\), " + LD + V + r"\.u32\)\)\);", r"VUNPCKHI(\1, \2, \3);"),
    (SI + V + r"\.u32, simde_mm_unpacklo_epi32\(" + LD + V + r"\.u32\), " + LD + V + r"\.u32\)\)\);", r"VUNPCKLO(\1, \2, \3);"),
    (SI + V + r"\.u8, simde_mm_cmpeq_epi32\(" + LD + V + r"\.u32\), " + LD + V + r"\.u32\)\)\);", r"VCMPEQUW(\1, \2, \3);"),
    (STPS + V + r"\.f32, simde_mm_mul_ps\(" + LDPS + V + r"\.f32\), " + LDPS + V + r"\.f32\)\)\);", r"VMULFP(\1, \2, \3);"),
    (STPS + V + r"\.f32, simde_mm_sub_ps\(" + LDPS + V + r"\.f32\), " + LDPS + V + r"\.f32\)\)\);", r"VSUBFP(\1, \2, \3);"),
    (STPS + V + r"\.f32, simde_mm_add_ps\(" + LDPS + V + r"\.f32\), " + LDPS + V + r"\.f32\)\)\);", r"VADDFP(\1, \2, \3);"),
    (STPS + V + r"\.f32, simde_mm_max_ps\(" + LDPS + V + r"\.f32\), " + LDPS + V + r"\.f32\)\)\);", r"VMAXFP(\1, \2, \3);"),
    (STPS + V + r"\.f32, simde_mm_min_ps\(" + LDPS + V + r"\.f32\), " + LDPS + V + r"\.f32\)\)\);", r"VMINFP(\1, \2, \3);"),
    (STPS + V + r"\.f32, simde_mm_cmpge_ps\(" + LDPS + V + r"\.f32\), " + LDPS + V + r"\.f32\)\)\);", r"VCMPGEFP(\1, \2, \3);"),
    (STPS + V + r"\.f32, simde_mm_cmpgt_ps\(" + LDPS + V + r"\.f32\), " + LDPS + V + r"\.f32\)\)\);", r"VCMPGTFP(\1, \2, \3);"),
    (STPS + V + r"\.f32, simde_mm_cmpeq_ps\(" + LDPS + V + r"\.f32\), " + LDPS + V + r"\.f32\)\)\);", r"VCMPEQFP(\1, \2, \3);"),
    (STPS + V + r"\.f32, simde_mm_add_ps\(simde_mm_mul_ps\(" + LDPS + V + r"\.f32\), " + LDPS + V + r"\.f32\)\), " + LDPS + V + r"\.f32\)\)\);", r"VMADDFP(\1, \2, \3, \4);"),
    (STPS + V + r"\.f32, simde_mm_xor_ps\(simde_mm_sub_ps\(simde_mm_mul_ps\(" + LDPS + V + r"\.f32\), " + LDPS + V + r"\.f32\)\), " + LDPS + V
     + r"\.f32\)\), simde_mm_castsi128_ps\(simde_mm_set1_epi32\(int\(0x80000000\)\)\)\)\);", r"VNMSUBFP(\1, \2, \3, \4);"),
    (STPS + V + r"\.f32, simde_mm_div_ps\(simde_mm_set1_ps\(1\), simde_mm_sqrt_ps\(" + LDPS + V + r"\.f32\)\)\)\);", r"VRSQRTEFP(\1, \2);"),
    (STPS + V + r"\.f32, simde_mm_div_ps\(simde_mm_set1_ps\(1\), " + LDPS + V + r"\.f32\)\)\);", r"VREFP(\1, \2);"),
    (STPS + V + r"\.f32, rex::ppc::Vmsum3fp128\(" + LDPS + V + r"\.f32\), " + LDPS + V + r"\.f32\)\)\);", r"VMSUM3FP(\1, \2, \3);"),
    (STPS + V + r"\.f32, rex::ppc::Vmsum4fp128\(" + LDPS + V + r"\.f32\), " + LDPS + V + r"\.f32\)\)\);", r"VMSUM4FP(\1, \2, \3);"),
    (r"c\.cr6\.SetFromMask\(" + LDPS + V + r"\.f32\), (\w+)\);", r"CR6_MASK(\1, \2);"),
]
# the multi-line shift (vslw128): the lines of one instruction, joined with \n
VSLW = re.compile(r"\{\nsimde__m128i a = simde_mm_load_si128\(\(simde__m128i\*\)" + V + r"\.u8\);\nsimde__m128i b = simde_mm_load_si128\(\(simde__m128i\*\)" + V + r"\.u8\);\n"
                  r"simde__m128i shift = simde_mm_and_si128\(b, simde_mm_set1_epi32\(0x1F\)\);\n" + SI + V + r"\.u8, simde_mm_sllv_epi32\(a, shift\)\);\n\}$")

# plain scalar / integer statements that are kept as they are (after the global renames): anything else is an error
PLAIN = [re.compile(p) for p in (
    r"^c\.[rf]\d+\.(?:u64|s64|u32|s32|f64) = [^;]*;$",
    r"^c\.f\d+\.u64 = c\.f\d+\.u64 (?:\^|&) ~?0x[0-9A-Fa-f]+;$",
    r"^c\.f\d+\.s64 = [^;]*;$",
    r"^c\.xer\.ca = [^;]*;$",
    r"^temp\.(?:u32|s64|f32) = [^;]*;$",
    r"^ea = [^;]*;$",
    r"^c\.(?:cr0|cr6)\.Compare(?:<(?:i32|u32)>)?\([^;]*\);$",
    r"^c\.lr = [^;]*;$",
    r"^if \(!?c\.cr[06]\.(?:lt|gt|eq)\) (?:goto loc_[0-9A-F]{8}|return);$",
    r"^goto loc_[0-9A-F]{8};$",
    r"^return;$",
    r"^c\.fpscr\.(?:SetFlushMode\(c\.vscr\.nj\)|DisableFlushMode\(\)|DisableFlushModeUnconditional\(\));$",
    r"^(?:ST(?:8|32|64)|LD(?:8|32|64))\(.*$",
    r"^c\.lr = c\.r12\.u64;$",
    r"^[a-z]+\w*\(c, base\);$",
    r"^return f_\w+\(c, base\);$",
    r"^f_\w+\(c, base\);$",
    r"^PPCRegister temp\{\};$",
    r"^u32 ea\{\};$",
    r"^temp\.u32 = RLT_(?:LD|STACK_LD)\w*\(.*\);$",
)]


def translate_statement(s: str):
    """One generated statement (leading tabs stripped) -> the twin's text, or None when no rule fits."""
    s = s.replace("ctx.", "c.").replace("(ctx, base", "(c, base")
    for pat, rep in VECTOR_RULES:
        m = re.fullmatch(pat, s)
        if m:
            if rep.startswith("VSEL(") and m.group(2) != m.group(4):
                return None  # a select whose two selector registers differ is not vsel
            return m.expand(rep)
    # memory
    s = re.sub(r"\bREX_(?:STACK_)?LOAD_U(8|16|32|64)\(", lambda m: f"LD{m.group(1)}(", s)
    s = re.sub(r"\bREX_(?:STACK_)?STORE_U(8|16|32|64)\(", lambda m: f"ST{m.group(1)}(", s)
    # the double operation rounded to float once
    m = re.fullmatch(r"(c\.f\d+\.f64) = f64\(f32\((.*)\)\);", s)
    if m:
        s = f"{m.group(1)} = S({m.group(2)});"
    s = s.replace("std::fma(", "Fma(").replace("std::sqrt(", "Sqrt(")
    s = re.sub(r"(?<![\w:])sqrt\(", "Sqrt(", s)
    m = re.fullmatch(r"rex_savegprlr<(\d+)>\(c, base, __savegprlr_\d+\);", s)
    if m:  # the compiler's prologue stub: r<N>..r31 and r12 into the 8 * (33 - N) bytes below r1 (the SDK's rex_savegprlr, flat stack)
        n = int(m.group(1))
        return "\n".join([f"ST64(c.r1.u32 - {8 * (33 - k)}u, c.r{k}.u64);" for k in range(n, 32)] + ["ST32(c.r1.u32 - 8u, c.r12.u32);"])
    m = re.fullmatch(r"rex_restgprlr<(\d+)>\(c, base, __restgprlr_\d+\);", s)
    if m:
        n = int(m.group(1))
        return "\n".join([f"c.r{k}.u64 = LD64(c.r1.u32 - {8 * (33 - k)}u);" for k in range(n, 32)] + ["c.r12.u64 = LD32(c.r1.u32 - 8u);", "c.lr = c.r12.u64;"])
    s = re.sub(r"\b__(save|rest)(fpr|vmx)_(\d+)\(c, base\);", r"f___\1\2_\3(c, base);", s)
    s = re.sub(r"\b(?:rex_)?sub_([0-9A-F]{8})\(c, base\)", r"f_\1(c, base)", s)
    s = re.sub(r"^REX_MUSTTAIL return (f_\w+\(c, base\));", r"return \1;", s)
    for p in PLAIN:
        if p.match(s):
            return s
    return None


# ------------------------------------------------------------------------------------------------ one function
REG = re.compile(r"\bc\.([rfv]\d+)\b")
CRX = re.compile(r"\bc\.(cr0|cr6|xer|lr|fpscr|vscr)\b")


def convert_function(addr: str, text: str, used: set, cov: list):
    lines = text.split("\n")
    assert lines[0].startswith("DEFINE_REX_FUNC(")
    body = lines[1:-1] if lines[-1] == "" else lines[1:]
    assert body[-1] == "}", (addr, body[-1])
    body = body[:-1]
    out = [f"RLT_FN void f_{addr.replace('sub_', '')}(Regs& c, uint8_t* base) {{"]

    def point(name):  # a coverage point: COV(n) counts the visits in the unit test's coverage build and is nothing otherwise
        cov.append(f"{addr.replace('sub_', '')} {name}")
        return f"COV({len(cov) - 1});"

    out.append("    " + point("entry"))
    groups = []  # (comment or None, [statements])
    i = 0
    cur_comment = None
    cur = []
    labels = []

    def flush():
        nonlocal cur_comment, cur
        if cur_comment is not None or cur:
            groups.append((cur_comment, cur))
        cur_comment, cur = None, []

    for raw in body:
        s = raw.strip()
        if s == "":
            continue
        if s == "REX_FUNC_PROLOGUE();":
            continue
        if s.startswith("// "):
            flush()
            cur_comment = s[3:].rstrip()
        elif re.fullmatch(r"loc_[0-9A-F]{8}:", s):
            flush()
            groups.append((None, [s]))
        else:
            cur.append(s)
    flush()
    lines_out = []
    for comment, stmts in groups:
        if len(stmts) == 1 and re.fullmatch(r"loc_[0-9A-F]{8}:", stmts[0]):
            lines_out.append(stmts[0])
            lines_out.append("    " + point(stmts[0][:-1]))
            continue
        text_stmts = "\n".join(stmts)
        # the vslw128 block
        m = VSLW.fullmatch(text_stmts.replace("ctx.", "c."))
        if m:
            tr = [f"VSLW({m.group(3)}, {m.group(1)}, {m.group(2)});"]
        else:
            tr = []
            for st in stmts:
                t = translate_statement(st)
                if t is None:
                    raise SystemExit(f"{addr}: no rule for the statement:\n    {st}\n  (after `{comment}`)")
                tr.append(t)
        tr = [line for t in tr for line in t.split("\n")]
        if len(tr) == 2:  # lfs: a float widened to double; stfs: a double rounded to float (the scratch `temp` is dead after the instruction)
            m1 = re.fullmatch(r"temp\.u32 = LD32\((.*)\);", tr[0])
            m2 = re.fullmatch(r"(c\.f\d+\.f64) = f64\(temp\.f32\);", tr[1])
            m3 = re.fullmatch(r"temp\.f32 = f32\((c\.f\d+\.f64)\);", tr[0])
            m4 = re.fullmatch(r"ST32\((.*), temp\.u32\);", tr[1])
            if m1 and m2:
                tr = [f"{m2.group(1)} = LDS({m1.group(1)});"]
            elif m3 and m4:
                tr = [f"STS({m4.group(1)}, {m3.group(1)});"]
        for t in tr:
            used.update(m_.group(1) for m_ in REG.finditer(t))
            for m_ in CRX.finditer(t):
                used.add(m_.group(1))
        if len(tr) == 1 and comment:
            lines_out.append(f"    {tr[0]}{' ' * max(1, 60 - len(tr[0]))}// {comment}")
        else:
            if comment:
                lines_out.append(f"    // {comment}")
            lines_out.extend(f"    {t}" for t in tr)
        if tr and tr[-1].startswith("if (") and (" goto " in tr[-1] or tr[-1].endswith("return;")):
            lines_out.append("    " + point(f"falls through `{comment}`"))
    out.extend(lines_out)
    out.append("}")
    return out


def emit(gen: Path) -> str:
    idx = index_generated(gen)
    parts = []
    cov = []  # the coverage points of both roots, in order
    parts.append("// GENERATED by tools/race_line_twin_gen.py: do not edit; rerun the tool (perf\\RACELINE-TWIN.md). Host twins of the Race Line's path functions rex_sub_8216E408\n"
                 "// and rex_sub_8216F900 and their callees: the recompiled instructions over a local register file (see the tool's header for what is and is not changed).\n"
                 "// The text of the generated tree is NOT in this file: each line is one instruction's meaning written with the macros of race_line_twin.h.\n")
    parts.append("#pragma once\n// (included by race_line_twin.h, which defines the macros, helpers and register types used below)\n\nnamespace race_line::twin {\nnamespace gen {\n")
    cov_at = len(parts)  # the coverage table goes here once all the points are known
    for tag, root in ROOTS.items():
        order = closure(idx, root)
        used = set()
        funcs = []
        for a in order:
            fn = "\n".join(convert_function(a, idx[a], used, cov))
            if a != root:  # a function of its own as a call, for the unit test's direct runs of the helpers (inline: nothing is compiled unless a test names it)
                fn += ("\n\nRLT_ENTRY inline void Run_" + a.replace("sub_", "") + "(PPCContext& ctx, uint8_t* base) {\n    Regs c;\n    c.Load(ctx);\n    f_"
                       + a.replace("sub_", "") + "(c, base);\n    c.Store(ctx);\n}")
            funcs.append(fn)
        # the registers the code names, in a stable order
        gprs = sorted((x for x in used if x[0] == "r"), key=lambda x: int(x[1:]))
        fprs = sorted((x for x in used if x[0] == "f" and x[1:].isdigit()), key=lambda x: int(x[1:]))
        vrs = sorted((x for x in used if x[0] == "v" and x[1:].isdigit()), key=lambda x: int(x[1:]))
        parts.append(f"// ---------------------------------------------------------------------------------------------- {tag}: {root.replace('sub_', '')} and its callees\n")
        parts.append(f"namespace {tag} {{\n")
        parts.append("struct Regs {\n")
        parts.append("    PPCRegister " + ", ".join(gprs) + ";\n")
        parts.append("    PPCRegister " + ", ".join(fprs) + ";\n")
        parts.append("    PPCVRegister " + ", ".join(vrs) + ";\n")
        parts.append("    PPCCRRegister cr0, cr6;\n    PPCXERRegister xer;\n    u64 lr;\n    PPCFPSCRRegister fpscr;\n    PPCVSCRRegister vscr;\n")
        regs_all = gprs + fprs + vrs + ["cr0", "cr6", "xer", "lr", "fpscr", "vscr"]
        parts.append("    RLT_FN void Load(const PPCContext& x) {\n" + "".join(f"        {r} = x.{r};\n" for r in regs_all) + "    }\n")
        parts.append("    RLT_FN void Store(PPCContext& x) const {\n" + "".join(f"        x.{r} = {r};\n" for r in regs_all) + "    }\n")
        parts.append("};\n\n")
        parts.append("\n\n".join(funcs))
        root_fn = "f_" + root.replace("sub_", "")
        parts.append("\n\n// The whole function as a call: the registers in, the code, the registers out (the same final register state as the recompiled function's).\n"
                     f"RLT_ENTRY inline void Run(PPCContext& ctx, uint8_t* base) {{\n    Regs c;\n    c.Load(ctx);\n    {root_fn}(c, base);\n    c.Store(ctx);\n}}\n")
        diff = ["\n// The first register in which two contexts differ (nullptr when they do not): the registers this function names, bit for bit, except that two NaNs are equal.\n"
                "inline const char* FirstRegDifference(const PPCContext& a, const PPCContext& b) {\n"]
        for r in gprs:
            diff.append(f'    if (a.{r}.u64 != b.{r}.u64) return "{r}";\n')
        for r in fprs:
            diff.append(f'    if (!SameF64(a.{r}.u64, b.{r}.u64)) return "{r}";\n')
        for r in vrs:
            diff.append(f'    if (!SameV128(a.{r}.u32, b.{r}.u32)) return "{r}";\n')
        diff.append('    if (a.cr0.GetRaw() != b.cr0.GetRaw()) return "cr0";\n    if (a.cr6.GetRaw() != b.cr6.GetRaw()) return "cr6";\n'
                    '    if (a.xer.so != b.xer.so || a.xer.ov != b.xer.ov || a.xer.ca != b.xer.ca) return "xer";\n'
                    '    if (a.lr != b.lr) return "lr";\n    if (a.fpscr.csr != b.fpscr.csr) return "fpscr";\n'
                    '    if (a.vscr.nj != b.vscr.nj || a.vscr.sat != b.vscr.sat) return "vscr";\n    return nullptr;\n}\n')
        parts.append("".join(diff))
        parts.append(f"\n}}  // namespace {tag}\n\n")
    table = ("// Coverage of the unit test (race_line_test.cpp, -DRL_TWIN_COVERAGE): COV(n) counts how often point n (a function's entry, a branch target, the fall-through of a\n"
             "// conditional branch) was reached; nothing in the shipped build.\n"
             "#ifdef RL_TWIN_COVERAGE\n"
             f"inline constexpr int kCovPoints = {len(cov)};\n"
             "inline unsigned g_cov[kCovPoints] = {};\n"
             "inline const char* const kCovNames[kCovPoints] = {\n" + "".join(f'    "{n}",\n' for n in cov) + "};\n"
             "#define COV(n) (++race_line::twin::gen::g_cov[n])\n"
             "#else\n#define COV(n) ((void)0)\n#endif\n\n")
    parts.insert(cov_at, table)
    parts.append('}  // namespace gen\n}  // namespace race_line::twin\n\n'
                 '#undef COV\n#undef LD8\n#undef LD16\n#undef LD32\n#undef LD64\n#undef LDS\n#undef ST8\n#undef ST16\n#undef ST32\n#undef ST64\n#undef STS\n')
    return "".join(parts)


def main() -> int:
    parser = argparse.ArgumentParser(description="Generate the local Race Line twin header.")
    parser.add_argument("generated_dir", type=Path)
    parser.add_argument("out", type=Path)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    out = args.out
    text = emit(args.generated_dir)
    if args.check:
        same = out.exists() and out.read_text(encoding="utf-8") == text
        print("race_line_twin_gen.h is up to date" if same else "race_line_twin_gen.h differs from what the generator writes now")
        return 0 if same else 1
    out.write_text(text, encoding="utf-8", newline="\n")
    print(f"wrote {out} ({len(text.splitlines())} lines, {len(text)} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
