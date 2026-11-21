r"""check_rt_hit_identity.py: do the car's ray traced shaders still compile to the same thing?  (dev only; runs dxc, builds nothing of the game)

  python -I tools\check_rt_hit_identity.py (--base <git ref> | --selftest) [--src <src dir>] [--dxc <dxc.exe>] [--spirv-opt <spirv-opt.exe>] [--spirv-val <spirv-val.exe>] [--spirv-dis <spirv-dis.exe>] [--keep <dir>] [-v]

CITY-RT M2 package H4 moved what every hit shading shares out of the car's hit shading (hlsl/car_rt_hit_shade.hlsli) into hlsl/rt_hit_common.hlsli (CarMat* became RtHit*, bodies
unchanged). The car's picture must not change by a bit. This tool is the proof, and the one to run again after any later edit of the shared file: it takes the shader sources of
the commit `--base` (git archive of <src dir>/gpu/hlsl and <src dir>/gpu/*.hlsl of that ref: nothing is checked out) and of the working tree, compiles the same entry points with
the flags the build uses (src/gpu/CMakeLists.txt: -T cs_6_5 -HV 2021 -all-resources-bound -Wno-ignored-attributes, and for Vulkan -spirv -fvk-use-dx-layout
-fspv-target-env=vulkan1.2), and compares:
  * DXIL      the disassembly (dxc -Fc) of the two modules, line for line, without the module's own hash line: every instruction must be the same;
  * SPIR-V    the two modules after `spirv-opt --strip-debug` (names are not code), disassembled; both also go through spirv-val.
Two levels of "the same": IDENTICAL (every line equal) and EQUIVALENT: the same instructions in the same order with the same dataflow, up to the numbering of the values and the
order of the phi nodes at the head of a block. dxc's output has no fixed phi order (LLVM orders them by pointer, so a rename or a moved function can swap two of them: the gamma curve's
`scale` and `offset` did when the shared code moved), which changes no value and no operation; the check canonicalises it away (a block's phi run is sorted by its text, the values and
labels are numbered by first appearance) and then compares, so a changed operand or constant or operation still fails.
The entry points are the car's: car_rt_refl_cs (the player's pass), the same with CAR_RT_REFL_OPPONENTS=1 (the opponents' pass) and CAR_RT_REFL_GLASS=1 (the glass pass), all three with
CITY_REFL_NO_LAMPS=1 (REFL-LAMPS: the world frame's lamp term of hlsl/city_refl.hlsli left out: the parent's code) and without it (the *_lamps_cs entries, no base), car_rt_refl_denoise_cs, car_rt_ao_cs, car_rt_check_cs
and the city's shadow pass city_rt_shadow_cs (which does not use the shared file; it is the control: it must not change either). The city hit library's reference consumer
(city_hit_shade_check_cs) and the world frame's (rt_world_probe_cs, rt_world_lamp_light_cs) have no base: they are compiled for DXIL and SPIR-V and validated (spirv-val), nothing more.
Exit 0 when every entry point is identical or equivalent, 1 when one is not (the first differing lines are printed), 2 when a tool or a source is missing.
"""
import argparse
import difflib
import io
import os
import subprocess
import sys
import tarfile
import tempfile

DEFAULT_DXC = r'C:\mcv\larecomp\downloads\dxc-v1.9.2609\x\bin\x64\dxc.exe'
DEFAULT_SPIRV_BIN = r'C:\VulkanSDK\1.4.363.0\Bin'
# REFL-LAMPS: the reflection passes (the player's paint, the opponents' variant, the glass variant) take the world frame's lamp term and glow in hlsl/city_refl.hlsli unless CITY_REFL_NO_LAMPS is defined. The
# three compiled WITH that define must still be what the parent's were (the lamp term left out changes no instruction: the A/B of the debug setting is a runtime flag on top of this); the same three without
# it (the *_lamps_cs entries) have no base: they only have to compile (DXIL, SPIR-V) and validate.
ENTRIES = [
    ('car_rt_refl_cs', 'car_rt_refl_cs.hlsl', ['CITY_REFL_NO_LAMPS=1']),
    ('car_rt_refl_opp_cs', 'car_rt_refl_cs.hlsl', ['CAR_RT_REFL_OPPONENTS=1', 'CITY_REFL_NO_LAMPS=1']),
    ('car_rt_refl_glass_cs', 'car_rt_refl_cs.hlsl', ['CAR_RT_REFL_GLASS=1', 'CITY_REFL_NO_LAMPS=1']),
    ('car_rt_refl_lamps_cs', 'car_rt_refl_cs.hlsl', []),  # REFL-LAMPS: the paint with the world frame's lamp term: new, it only has to compile and validate
    ('car_rt_refl_opp_lamps_cs', 'car_rt_refl_cs.hlsl', ['CAR_RT_REFL_OPPONENTS=1']),
    ('car_rt_refl_glass_lamps_cs', 'car_rt_refl_cs.hlsl', ['CAR_RT_REFL_GLASS=1']),
    ('car_rt_refl_denoise_cs', 'car_rt_refl_denoise_cs.hlsl', []),
    ('car_rt_ao_cs', 'car_rt_ao_cs.hlsl', []),
    ('car_rt_check_cs', 'car_rt_check_cs.hlsl', []),
    ('city_rt_shadow_cs', 'city_rt_shadow_cs.hlsl', []),
    ('city_hit_shade_check_cs', 'city_hit_shade_check_cs.hlsl', []),  # the city hit library's reference consumer: new, it only has to compile and validate
    ('rt_world_probe_cs', 'rt_world_probe_cs.hlsl', []),  # the world frame's reference consumer (CITY-RT FRAME, hlsl/rt_world_frame.hlsli): new, it only has to compile and validate
    ('rt_world_lamp_light_cs', 'rt_world_lamp_light_cs.hlsl', []),  # the lamp term through the world frame's grid beside the city hit library (R7's loop): new, it only has to compile and validate
]


def run(cmd, what):
    p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors='replace')
    if p.returncode != 0:
        print('FAILED (%s): %s\n%s' % (what, ' '.join(cmd[:3]), p.stdout[-1500:]))
    return p.returncode == 0


def export_base(repo_src, ref, dest):
    """git archive of <repo>/src/gpu/hlsl at `ref` into dest."""
    repo = os.path.dirname(os.path.abspath(repo_src))
    rel = os.path.relpath(os.path.join(repo_src, 'gpu', 'hlsl'), repo).replace('\\', '/')
    p = subprocess.run(['git', '-C', repo, 'archive', ref, rel], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if p.returncode != 0:
        print('git archive failed: %s' % p.stderr.decode(errors='replace'))
        return None
    with tarfile.open(fileobj=io.BytesIO(p.stdout)) as tar:
        tar.extractall(dest)
    return os.path.join(dest, *rel.split('/'))


def normalise_dxil(text):
    out = []
    for line in text.split('\n'):
        if line.startswith('; shader hash:') or line.startswith('; Name') or 'source_filename' in line:
            continue
        out.append(line)
    return out


def canonical(lines, phi_word):
    """A listing in a form that does not depend on the numbering of values and labels or on the order of a block's phi run (see the module's comment)."""
    import re
    number = re.compile(r'%\d+')
    stripped = [number.sub('%N', l) for l in lines]
    out = []
    i = 0
    n = len(lines)
    while i < n:
        if phi_word in lines[i]:
            j = i
            while j < n and phi_word in lines[j]:
                j += 1
            run = sorted(range(i, j), key=lambda k: stripped[k])
            out.extend(lines[k] for k in run)
            i = j
        else:
            out.append(lines[i])
            i += 1
    ids = {}

    def rename(match):
        key = match.group(0)
        if key not in ids:
            ids[key] = '%%v%d' % len(ids)
        return ids[key]

    return [number.sub(rename, l) for l in out]


def canonical_spirv(lines):
    """A SPIR-V listing (spirv-dis --raw-id) in a form that does not depend on the numbering of ids, on the order of the module's types, constants and variables, or on the order of a block's
    OpPhi run: every id defined before the first OpFunction is replaced by a key of what it IS (its instruction with its operands' keys, and its decorations), the ids inside the
    functions are numbered by first appearance, and the module's own lines and the phi runs are sorted."""
    import hashlib
    import re
    ident = re.compile(r'%\d+')
    lines = [l.strip() for l in lines if l.strip()]
    first_function = next((i for i, l in enumerate(lines) if ' = OpFunction ' in l), len(lines))
    module, body = lines[:first_function], lines[first_function:]
    decorations = {}
    for l in module:
        m = re.match(r'(OpDecorate|OpMemberDecorate|OpDecorateId) (%\d+) (.*)$', l)
        if m:
            decorations.setdefault(m.group(2), []).append(m.group(1) + ' ' + m.group(3))
    keys = {}

    def key_of(token):
        return keys.get(token, token)

    for l in module:
        m = re.match(r'(%\d+) = (Op\w+)(.*)$', l)
        if not m:
            continue
        text = m.group(2) + ident.sub(lambda x: key_of(x.group(0)), m.group(3))
        deco = sorted(ident.sub(lambda x: key_of(x.group(0)), d) for d in decorations.get(m.group(1), []))
        keys[m.group(1)] = '<' + hashlib.md5((text + '|' + ';'.join(deco)).encode()).hexdigest()[:12] + '>'
    module_out = []
    for l in module:
        if re.match(r'(OpDecorate|OpMemberDecorate|OpDecorateId) ', l):
            continue
        m = re.match(r'(%\d+) = (Op\w+)(.*)$', l)
        if m:
            module_out.append(keys[m.group(1)] + ' = ' + m.group(2) + ident.sub(lambda x: key_of(x.group(0)), m.group(3)))
        else:
            module_out.append(ident.sub(lambda x: key_of(x.group(0)) if x.group(0) in keys else '%F', l))
    module_out.sort()
    for token, items in decorations.items():
        module_out.append('decorations ' + key_of(token) + ': ' + ' | '.join(sorted(ident.sub(lambda x: key_of(x.group(0)), d) for d in items)))
    module_out.sort()
    # the functions: module ids by key, the function-local ones sorted-phi then numbered by first appearance
    def local_text(l):
        return ident.sub(lambda x: key_of(x.group(0)) if x.group(0) in keys else '%L', l)
    stripped = [local_text(l) for l in body]
    ordered = []
    i = 0
    while i < len(body):
        if ' = OpPhi ' in body[i]:
            j = i
            while j < len(body) and ' = OpPhi ' in body[j]:
                j += 1
            ordered.extend(body[k] for k in sorted(range(i, j), key=lambda k: stripped[k]))
            i = j
        else:
            ordered.append(body[i])
            i += 1
    local = {}

    def number(match):
        token = match.group(0)
        if token in keys:
            return keys[token]
        if token not in local:
            local[token] = '%%v%d' % len(local)
        return local[token]

    return module_out + [ident.sub(number, l) for l in ordered]


def disassemble(spirv_dis, path):
    p = subprocess.run([spirv_dis, '--raw-id', '--no-header', '--no-indent', path], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors='replace')
    return p.stdout.split('\n')


def selftest():
    """The comparison must call a renumbering and a phi swap the same and a changed operand, constant or operation different (no dxc needed)."""
    base = ['define void @main() {', '; <label>:1', '  %5 = phi float [ 1.0, %2 ], [ 2.0, %3 ]', '  %6 = phi float [ 3.0, %2 ], [ 4.0, %3 ]', '  %7 = fmul fast float %5, %6', '  %8 = fadd fast float %7, 1.0',
            '  call void @dx.op.storeOutput.f32(i32 5, float %8)', '}']
    renumbered = [l.replace('%5', '%50').replace('%6', '%60').replace('%7', '%70').replace('%8', '%80') for l in base]
    swapped = list(base)
    swapped[2], swapped[3] = base[3].replace('%6', '%5'), base[2].replace('%5', '%6')
    swapped[4] = '  %7 = fmul fast float %6, %5'
    operand = list(base)
    operand[4] = '  %7 = fmul fast float %5, %5'
    constant = list(base)
    constant[5] = '  %8 = fadd fast float %7, 1.5'
    operation = list(base)
    operation[5] = '  %8 = fsub fast float %7, 1.0'
    ok = True
    ok = ok and canonical(base, ' = phi ') == canonical(renumbered, ' = phi ')
    ok = ok and canonical(base, ' = phi ') == canonical(swapped, ' = phi ')
    ok = ok and canonical(base, ' = phi ') != canonical(operand, ' = phi ')
    ok = ok and canonical(base, ' = phi ') != canonical(constant, ' = phi ')
    ok = ok and canonical(base, ' = phi ') != canonical(operation, ' = phi ')
    spv = ['OpCapability Shader', 'OpDecorate %3 DescriptorSet 0', 'OpDecorate %3 Binding 1', '%1 = OpTypeFloat 32', '%2 = OpConstant %1 0.5', '%4 = OpConstant %1 2', '%3 = OpVariable %1 UniformConstant',
           '%9 = OpFunction %1 None %1', '%10 = OpLabel', '%11 = OpLoad %1 %3', '%12 = OpFAdd %1 %11 %2', '%13 = OpFMul %1 %12 %4', 'OpReturnValue %13', 'OpFunctionEnd']
    spv_moved = [spv[0], spv[1], spv[2], spv[3], spv[5].replace('%4', '%40'), spv[4], spv[6], spv[7], spv[8], spv[9].replace('%3', '%3'), spv[10], spv[11].replace('%4', '%40'), spv[12], spv[13]]
    spv_moved = [l.replace('%4 ', '%40 ') if l.startswith('%4 ') else l for l in spv_moved]
    spv_changed = [l.replace('%2 = OpConstant %1 0.5', '%2 = OpConstant %1 0.25') for l in spv]
    ok = ok and canonical_spirv(spv) == canonical_spirv(spv_moved)
    ok = ok and canonical_spirv(spv) != canonical_spirv(spv_changed)
    print('selftest: %s' % ('passed' if ok else 'FAILED'))
    return 0 if ok else 1


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser()
    ap.add_argument('--base', help='the git ref of the shaders before the change')
    ap.add_argument('--selftest', action='store_true', help='check the comparison itself on made-up listings and exit')
    ap.add_argument('--src', default=os.path.join(os.path.dirname(here), 'src'))
    ap.add_argument('--dxc', default=DEFAULT_DXC)
    ap.add_argument('--spirv-opt', default=os.path.join(DEFAULT_SPIRV_BIN, 'spirv-opt.exe'))
    ap.add_argument('--spirv-val', default=os.path.join(DEFAULT_SPIRV_BIN, 'spirv-val.exe'))
    ap.add_argument('--spirv-dis', default=os.path.join(DEFAULT_SPIRV_BIN, 'spirv-dis.exe'))
    ap.add_argument('--keep', default='')
    ap.add_argument('-v', action='store_true')
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    if not a.base:
        ap.error('--base is required')
    for tool in (a.dxc, a.spirv_opt, a.spirv_val, a.spirv_dis):
        if not os.path.exists(tool):
            print('missing tool: %s' % tool)
            return 2
    new_dir = os.path.join(a.src, 'gpu', 'hlsl')
    work = a.keep or tempfile.mkdtemp(prefix='rt_hit_identity_')
    os.makedirs(work, exist_ok=True)
    base_dir = export_base(a.src, a.base, os.path.join(work, 'base'))
    if base_dir is None or not os.path.isdir(base_dir):
        return 2
    bad = 0
    for name, source, defines in ENTRIES:
        results = {}
        for tag, directory in (('base', base_dir), ('new', new_dir)):
            path = os.path.join(directory, source)
            if tag == 'base' and name.endswith('_lamps_cs'):
                results[tag] = None  # REFL-LAMPS: the entries with the lamp term have no base
                continue
            if not os.path.exists(path):
                print('%-24s %s: no %s' % (name, tag, path))
                results[tag] = None
                continue
            common = [a.dxc, '-T', 'cs_6_5', '-HV', '2021', '-all-resources-bound', '-Wno-ignored-attributes', '-I', directory]
            for d in defines:
                common += ['-D', d]
            base_name = os.path.join(work, '%s.%s' % (name, tag))
            ok = run(common + ['-Fo', base_name + '.dxil', '-Fc', base_name + '.dxil.txt', path], '%s %s DXIL' % (name, tag))
            ok = run(common + ['-spirv', '-fvk-use-dx-layout', '-fspv-target-env=vulkan1.2', '-Fo', base_name + '.spv', path], '%s %s SPIR-V' % (name, tag)) and ok
            if ok:
                ok = run([a.spirv_val, '--target-env', 'vulkan1.2', base_name + '.spv'], '%s %s spirv-val' % (name, tag))
            if ok:
                ok = run([a.spirv_opt, '--strip-debug', base_name + '.spv', '-o', base_name + '.stripped.spv'], '%s %s spirv-opt' % (name, tag))
            results[tag] = base_name if ok else None
        if results.get('base') is None or results.get('new') is None:
            if results.get('base') is None and results.get('new') is not None:
                print('%-24s the base does not have this entry point: not compared; the new one compiles (DXIL, SPIR-V) and validates' % name)
                continue
            print('%-24s NOT COMPARED (a compile failed)' % name)
            bad += 1
            continue
        dxil_a = normalise_dxil(open(results['base'] + '.dxil.txt', encoding='utf-8', errors='replace').read())
        dxil_b = normalise_dxil(open(results['new'] + '.dxil.txt', encoding='utf-8', errors='replace').read())
        spv_a = disassemble(a.spirv_dis, results['base'] + '.stripped.spv')
        spv_b = disassemble(a.spirv_dis, results['new'] + '.stripped.spv')
        verdicts = []
        for what, la, lb, phi in (('DXIL', dxil_a, dxil_b, ' = phi '), ('SPIR-V', spv_a, spv_b, None)):
            if la == lb:
                verdicts.append('%s IDENTICAL (%d lines)' % (what, len(lb)))
                continue
            ca, cb = (canonical(la, phi), canonical(lb, phi)) if phi else (canonical_spirv(la), canonical_spirv(lb))
            exact = sum(1 for x, y in zip(la, lb) if x != y) + abs(len(la) - len(lb))
            if ca == cb:
                verdicts.append('%s EQUIVALENT (%d lines, %d of them numbered or ordered differently)' % (what, len(lb), exact))
            else:
                verdicts.append('%s DIFFERENT' % what)
                bad += 1
                print('    first differences in %s:' % what)
                shown = 0
                for line in difflib.unified_diff(ca, cb, 'base', 'new', n=1, lineterm=''):
                    print('      ' + line[:170])
                    shown += 1
                    if shown >= 24:
                        break
        print('%-24s %s' % (name, ', '.join(verdicts)))
    print('RESULT: %s' % ('every entry point identical or equivalent' if bad == 0 else '%d difference(s)' % bad))
    return 0 if bad == 0 else 1


if __name__ == '__main__':
    sys.exit(main())
