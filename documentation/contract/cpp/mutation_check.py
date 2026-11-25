"""Mutation check of the contract's C++ reference evaluator: break ui_expr_ref.h on purpose, one change at a time, and make sure
condition-tests.json (read by ui_expr_ref_test.cpp) catches every break. A mutant the vectors do not catch means a missing test vector.

  python -I documentation/contract/cpp/mutation_check.py [--clang <clang++.exe>] [--keep] [--only <name,...>] [--jobs N]

Each mutant is a text replacement that must match exactly once (else the check stops: the header changed, update the table). The test is built
from a temporary copy of the cpp folder with the mutated header; the unmodified header is built first and must pass. Exit 0 = every mutant caught.
No game, no SDK; the folder it uses is the system temp folder (removed unless --keep).
"""
import argparse
import concurrent.futures as cf
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
CONTRACT = os.path.dirname(HERE)
DEFAULT_CLANG = r'C:\mcv\larecomp\labuild\tools\clang+llvm-20.1.8-x86_64-pc-windows-msvc\bin\clang++.exe'

# (name, old text, new text). Each old text occurs exactly once in ui_expr_ref.h.
MUTANTS = [
    ('and: false no longer wins over null', 'if (x.IsFalse() || y.IsFalse()) return Value::Bool(false);', 'if (false) return Value::Bool(false);'),
    ('or: true no longer wins over null', 'if (x.IsTrue() || y.IsTrue()) return Value::Bool(true);', 'if (false) return Value::Bool(true);'),
    ('or: null || false is false', 'if (x.IsFalse() && y.IsFalse()) return Value::Bool(false);', 'if (!x.IsTrue() && !y.IsTrue()) return Value::Bool(false);'),
    ('and: null && true is true', 'if (x.IsTrue() && y.IsTrue()) return Value::Bool(true);', 'if (!x.IsFalse() && !y.IsFalse()) return Value::Bool(true);'),
    ('not: a non-bool operand is not null', 'inline Value Tri(const Value& v) { return v.kind == Value::Kind::kBool ? v : Value::Null(); }',
     'inline Value Tri(const Value& v) { return v; }'),
    ('not: !null is true', '  return t.IsNull() ? t : Value::Bool(!t.b);', '  return Value::Bool(!t.b);'),
    ('cmp: kind mismatch is false, not null', 'if (a.IsNull() || b.IsNull() || a.kind != b.kind) return Value::Null();',
     'if (a.IsNull() || b.IsNull()) return Value::Null();\n  if (a.kind != b.kind) return Value::Bool(op == "!=");'),
    ('cmp: null == null is true', 'if (a.IsNull() || b.IsNull() || a.kind != b.kind) return Value::Null();',
     'if (a.IsNull() && b.IsNull()) return Value::Bool(op == "==");\n  if (a.IsNull() || b.IsNull() || a.kind != b.kind) return Value::Null();'),
    ('cmp: lists compare', 'if (a.kind == Value::Kind::kList) return Value::Null();', ''),
    ('cmp: text has an order', 'if (a.kind != Value::Kind::kNumber) return Value::Null();', 'if (a.kind != Value::Kind::kNumber) return Value::Bool(a.s < b.s);'),
    ('cmp: <= is <', 'if (op == "<=") return Value::Bool(a.n <= b.n);', 'if (op == "<=") return Value::Bool(a.n < b.n);'),
    ('cmp: >= is >', 'if (op == ">=") return Value::Bool(a.n >= b.n);', 'if (op == ">=") return Value::Bool(a.n > b.n);'),
    ('cmp: text compare ignores case', 'case Value::Kind::kString: return a.s == b.s;',
     'case Value::Kind::kString: { if (a.s.size() != b.s.size()) return false; for (size_t i = 0; i < a.s.size(); ++i) if ((a.s[i] | 32) != (b.s[i] | 32)) return false; return true; }'),
    ('in: a miss is null', 'Value acc = Value::Bool(false);', 'Value acc = Value::Null();'),
    ('in: a null left side is false', 'Value x = EvalNode(*n.kids[0], env, depth);\n      Value acc', 'Value x = EvalNode(*n.kids[0], env, depth);\n      if (x.IsNull()) return Value::Bool(false);\n      Value acc'),
    ('coalesce: false counts as null', 'return a.IsNull() ? b : a;', 'return a.IsTrue() ? a : b;'),
    ('coalesce: the right side wins', 'return a.IsNull() ? b : a;', 'return b.IsNull() ? a : b;'),
    ('parser: comparison operands are not coalesced', 'auto right = Coalesce();\n      if (!right) return nullptr;\n      auto n = Make(Node::Kind::kCmp);',
     'auto right = Primary();\n      if (!right) return nullptr;\n      auto n = Make(Node::Kind::kCmp);'),
    ('parser: trailing text is accepted', 'if (Peek().t != Tok::T::kEnd) {', 'if (false) {'),
    ('parser: negative numbers are not literals', "if (IsDigit(c) || (c == '-' && i + 1 < s.size() && IsDigit(s[i + 1]))) {", 'if (IsDigit(c)) {'),
    ('parser: a call with the wrong arity is supported', 'if (it == kArity.end() || it->second != n->kids.size()) unsupported = true;', 'if (it == kArity.end()) unsupported = true;'),
    ('parser: an unknown function is supported', 'if (it == kArity.end() || it->second != n->kids.size()) unsupported = true;', 'if (it != kArity.end() && it->second != n->kids.size()) unsupported = true;'),
    ('eval: an unsupported call still evaluates', 'if (!p.ok || p.unsupported_call || !p.root) return Value::Null();', 'if (!p.ok || !p.root) return Value::Null();'),
    ('known: true only for true', 'return Value::Bool(!EvalNode(*n.kids[0], env, depth).IsNull());', 'return Value::Bool(EvalNode(*n.kids[0], env, depth).IsTrue());'),
    ('contains: an unknown list is false', 'if (l.kind != Value::Kind::kList || x.IsNull()) return Value::Null();',
     'if (l.kind != Value::Kind::kList) return Value::Bool(false);\n        if (x.IsNull()) return Value::Null();'),
    ('contains: a null item is false', 'if (l.kind != Value::Kind::kList || x.IsNull()) return Value::Null();',
     'if (l.kind != Value::Kind::kList) return Value::Null();\n        if (x.IsNull()) return Value::Bool(false);'),
    ('min and max swapped', 'f == "min" ? std::min(a.n, b.n) : std::max(a.n, b.n)', 'f == "min" ? std::max(a.n, b.n) : std::min(a.n, b.n)'),
    ('min: a null argument is ignored', 'if (a.kind != Value::Kind::kNumber || b.kind != Value::Kind::kNumber) return Value::Null();',
     'if (a.kind != Value::Kind::kNumber) return b;\n        if (b.kind != Value::Kind::kNumber) return a;'),
    ('resolved: a non-auto value is not returned', 'if (!is_auto) return v;', 'if (!is_auto) return Value::Null();'),
    ('resolved: auto_value is ignored', '(d && d->has_auto_value) ?', 'false ?'),
    ('resolved: a null value of a known setting counts as auto', 'Value v = env.Setting(setting);\n        if (v.IsNull()) return v;',
     'Value v = env.Setting(setting);\n        if (v.IsNull() && env.Auto(setting)) v = Value::String("auto");\n        if (v.IsNull()) return v;'),
    ('parser: && is not parsed under ||', 'auto left = And();\n    while (left && IsPunct("||")) {\n      Take();\n      auto right = And();',
     'auto left = NotExpr();\n    while (left && IsPunct("||")) {\n      Take();\n      auto right = NotExpr();'),
    ('cell: the handheld override is ignored', 'if (env.Fact("gpu.handheld").IsTrue()) {', 'if (false) {'),
    ('cell: an unknown handheld is a handheld', 'if (env.Fact("gpu.handheld").IsTrue()) {', 'if (!env.Fact("gpu.handheld").IsFalse()) {'),
    ('cell: by rt_rung reads the class', 'Value rung = env.Fact("gpu.rt_rung");', 'Value rung = env.Fact("gpu.class");'),
    ('cell: by preset takes the last entry', 'if (!entry.first || !Matches(Evaluate(*entry.first, env, depth + 1))) continue;', 'if (!entry.first) continue;'),
    ('cell: by preset reads unknown as a match', 'if (!entry.first || !Matches(Evaluate(*entry.first, env, depth + 1))) continue;',
     'if (!entry.first || Evaluate(*entry.first, env, depth + 1).IsFalse()) continue;'),
    ('def: a null when matches', 'if (c.when && !Matches(Evaluate(*c.when, env, depth + 1))) continue;', 'if (c.when && Evaluate(*c.when, env, depth + 1).IsFalse()) continue;'),
    ('def: the depth limit is gone', 'constexpr int kMaxDepth = 32;', 'constexpr int kMaxDepth = 1000000;'),
    ('def: the last case is skipped', 'return c.literal;\n  }\n  return Value::Null();', 'continue;\n  }\n  return Value::Null();'),
    ('path: paths is a namespace', 'w == "build" || w == "install" || w == "live";', 'w == "build" || w == "install" || w == "live" || w == "paths";'),
    ('path: live is not a namespace', ' || w == "install" || w == "live";', ' || w == "install";'),
    ('path: a lone namespace reads a fact', 'if (IsFactNamespace(s[0]) && s.size() >= 2) return env.Fact(Joined(s));', 'if (IsFactNamespace(s[0])) return env.Fact(Joined(s));'),
    ('path: s with more words reads the setting', 'if (s[0] == "s") return s.size() == 2 ? env.Setting(s[1]) : Value::Null();', 'if (s[0] == "s") return env.Setting(s[1]);'),
    ('path: renderer reads another word', 'if (s[0] == "renderer") return s.size() == 1 ? env.Renderer() : Value::Null();', 'if (s[0] == "renderer") return env.Renderer();'),
    ('lint: the guard is the right operand', 'bool g = guarded || (n.kind == Node::Kind::kCoalesce && i == 0);', 'bool g = guarded || (n.kind == Node::Kind::kCoalesce && i == 1);'),
    ('lint: nothing is guarded', 'bool g = guarded || (n.kind == Node::Kind::kCoalesce && i == 0);', 'bool g = guarded;'),
    ('lint: everything is guarded', 'bool g = guarded || (n.kind == Node::Kind::kCoalesce && i == 0);', 'bool g = true;'),
    ('typed: text "True" is a bool', 'raw.s == "true") return Value::Bool(true);', 'raw.s == "true" || raw.s == "True") return Value::Bool(true);'),
    ('typed: an int may have a fraction', 'if (type == "int" && n != std::floor(n)) return Value::Null();', ''),
    ('typed: a number is a string', 'return raw.kind == Value::Kind::kString ? raw : Value::Null();', 'return raw;'),
    ('typed: empty text is zero', 'if (t.empty()) return false;', 'if (t.empty()) { *out = 0; return true; }'),
    ('result: a false is listed', 'inline bool ListedUnlessFalse(const Value& v) { return !v.IsFalse(); }', 'inline bool ListedUnlessFalse(const Value& v) { return v.IsTrue(); }'),
    ('result: a null matches', 'inline bool Matches(const Value& v) { return v.IsTrue(); }', 'inline bool Matches(const Value& v) { return !v.IsFalse(); }'),
    ('result: a missing enabled is disabled', 'return !enabled_field_or_null.IsFalse();', 'return enabled_field_or_null.IsTrue();'),
]


def build(clang, src_dir, out_exe, extra=()):
    cmd = [clang, '-std=c++20', '-O0', '-I', src_dir, os.path.join(src_dir, 'ui_expr_ref_test.cpp'), '-o', out_exe, *extra]
    return subprocess.run(cmd, capture_output=True, text=True, encoding='utf-8', errors='replace')


def run_test(exe, timeout=60):
    try:
        r = subprocess.run([exe, CONTRACT], capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=timeout)
    except subprocess.TimeoutExpired:
        return 124, 'timeout'
    return r.returncode, (r.stdout or '').strip().splitlines()[-1:] and (r.stdout or '').strip().splitlines()[-1] or ''


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--clang', default=DEFAULT_CLANG)
    ap.add_argument('--keep', action='store_true')
    ap.add_argument('--only', default='')
    ap.add_argument('--jobs', type=int, default=6)
    args = ap.parse_args()
    if not os.path.exists(args.clang):
        print('clang++ not found:', args.clang)
        return 2
    header = open(os.path.join(HERE, 'ui_expr_ref.h'), encoding='utf-8', newline='').read().replace('\r\n', '\n')
    root = tempfile.mkdtemp(prefix='uiexpr_mut_')
    try:
        for name, old, _ in MUTANTS:
            n = header.count(old)
            if n != 1:
                print(f'STOP: the text of mutant "{name}" occurs {n} times in ui_expr_ref.h (must be once)')
                return 2
        base = os.path.join(root, 'base')
        shutil.copytree(HERE, base, ignore=shutil.ignore_patterns('*.py', '__pycache__'))
        r = build(args.clang, base, os.path.join(base, 't.exe'))
        if r.returncode != 0:
            print('the unmodified header does not build:\n', r.stderr[-2000:])
            return 2
        code, last = run_test(os.path.join(base, 't.exe'))
        print(f'unmodified: exit {code} ({last})')
        if code != 0:
            return 2
        only = {x.strip() for x in args.only.split(',') if x.strip()}

        def one(i_mut):
            i, (name, old, new) = i_mut
            if only and name not in only:
                return name, 'SKIPPED', ''
            d = os.path.join(root, f'm{i}')
            shutil.copytree(base, d)
            os.remove(os.path.join(d, 't.exe'))
            with open(os.path.join(d, 'ui_expr_ref.h'), 'w', encoding='utf-8', newline='\n') as f:
                f.write(header.replace(old, new, 1))
            b = build(args.clang, d, os.path.join(d, 't.exe'))
            if b.returncode != 0:
                return name, 'INVALID', (b.stderr or '').strip().splitlines()[0] if b.stderr else 'did not build'
            code, last = run_test(os.path.join(d, 't.exe'))
            return name, ('CAUGHT' if code != 0 else 'SURVIVED'), f'exit {code}: {last}'

        results = []
        with cf.ThreadPoolExecutor(max_workers=max(1, args.jobs)) as pool:
            for res in pool.map(one, enumerate(MUTANTS)):
                results.append(res)
                print(f'{res[1]:9s} {res[0]}   {res[2]}')
        caught = sum(1 for r in results if r[1] == 'CAUGHT')
        survived = [r[0] for r in results if r[1] == 'SURVIVED']
        invalid = [r[0] for r in results if r[1] == 'INVALID']
        print(f'{caught} caught, {len(survived)} survived, {len(invalid)} did not build, {len(MUTANTS)} mutants')
        if survived:
            print('SURVIVED (add a vector for each):', *survived, sep='\n  ')
        return 0 if not survived and not invalid else 1
    finally:
        if not args.keep:
            shutil.rmtree(root, ignore_errors=True)


if __name__ == '__main__':
    sys.exit(main())
