#!/usr/bin/env python3
"""Static audit of every Plume descriptor set the renderer makes (the Python twin of src/gpu/descriptor_count_test.cpp).

Why: Device::CreateDescriptorSet(builder, views, samplers, name) takes the counts the caller says its builder describes and compares them with
what the backend allocated. D3D12 allocates one view for every descriptor of every range (a sampler range: samplers) and throws a set away whose
count differs; Vulkan does not count. A builder with two ranges made with a count of 1 therefore failed on D3D12 every frame while Vulkan stayed
healthy (the city shadow pass, 2026-11-21: the pass never ran on D3D12; the probe field's two sets were the same mistake). The binding code has the
same trap the other way round: a descriptor index past the set's ranges writes into another set's slots of the shared D3D12 heap, a buffer written
into a texture range asserts (or is garbage in a release build), a structured buffer without its structure view reads a null pointer.

What it reads (every .cpp / .h under the folder it is given, never a *_test.cpp):
  * every builder: `X.begin(); X.addTexture(..); for (..) X.addReadWriteTexture(i); Helper(X); X.end(boundless, size);`
  * every `->CreateDescriptorSet(builder, views, samplers, name)` call: the counts are evaluated (literals, constexpr names, enumerators, casts,
    sums, ternaries, `rules::kName` through namespace aliases) and compared with the builder's ranges as D3D12 counts them. A builder that is a
    parameter (or a member set from one) is followed to the callers.
  * every setTexture / setBuffer / setSampler / setAccelerationStructure call on a created set: the descriptor index is inside the set, the range there
    has the type the call fills, a structured buffer gets its structure view; a range of the set that nothing in the file writes is a WARN.
Anything it cannot evaluate is an UNJUDGED line naming the file and line (never silently passed).

Usage: check_descriptor_counts.py <src dir> [--strict] [--print] [--selftest]
       check_descriptor_counts.py --ref <git ref> [--ref ..] [--repo <clone>] [--strict] [--print]   (a branch read from the object database, no checkout)
  exit 0 = no ERROR (with --strict also no UNJUDGED), 1 = a mismatch, 2 = usage.  --print also lists the sets that agree.
The C++ test (src/gpu/descriptor_count_test.cpp) prints the same lines for the same folder; the two are kept in step.
"""
import os
import sys

# ----------------------------------------------------------------------------------------------------------------------------- lexer

MULTI3 = ("...", "<<=", ">>=")
MULTI2 = ("::", "->", "<<", ">>", "&&", "||", "==", "!=", "<=", ">=", "++", "--", "+=", "-=", "*=", "/=", "|=", "&=", "^=", "%=")
CAST_TYPES = ("uint32_t", "uint64_t", "int", "unsigned", "size_t", "int32_t", "int64_t", "uint16_t", "uint8_t", "long", "short")
KEYWORD_FILES = ("RenderDescriptorSetBuilder", "CreateDescriptorSet", "createDescriptorSet")


def lex(text):
    """Tokens (kind, text, line): kind is 'id', 'num', 'str' or 'p'. Comments and preprocessor lines are dropped (the includes are returned)."""
    toks = []
    includes = []
    i, n, line = 0, len(text), 1
    line_start = True
    while i < n:
        c = text[i]
        if c == "\n":
            line += 1
            i += 1
            line_start = True
            continue
        if c in " \t\r\f\v":
            i += 1
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            while i < n and text[i] != "\n":
                i += 1
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "*":
            j = text.find("*/", i + 2)
            if j < 0:
                j = n - 2
            line += text.count("\n", i, j + 2)
            i = j + 2
            continue
        if c == "#" and line_start:
            j = i
            while True:
                k = text.find("\n", j)
                if k < 0:
                    k = n
                    break
                if (k > 0 and text[k - 1] == "\\") or (k > 1 and text[k - 1] == "\r" and text[k - 2] == "\\"):
                    j = k + 1
                    line += 1
                    continue
                break
            d = text[i + 1:k].lstrip()
            if d.startswith("include"):
                rest = d[7:].strip()
                if rest[:1] in ('"', "<"):
                    close = '"' if rest[0] == '"' else ">"
                    e = rest.find(close, 1)
                    if e > 0:
                        includes.append(rest[1:e])
            i = k
            continue
        line_start = False
        if c.isalpha() or c == "_":
            j = i + 1
            while j < n and (text[j].isalnum() or text[j] == "_"):
                j += 1
            word = text[i:j]
            if j < n and text[j] == '"' and word in ("R", "u8R", "uR", "UR", "LR"):
                d = text.find("(", j)
                delim = text[j + 1:d]
                e = text.find(")" + delim + '"', d)
                if e < 0:
                    e = n - 1
                end = e + len(delim) + 2
                toks.append(("str", "", line))
                line += text.count("\n", i, end)
                i = end
                continue
            toks.append(("id", word, line))
            i = j
            continue
        if c.isdigit() or (c == "." and i + 1 < n and text[i + 1].isdigit()):
            j = i + 1
            while j < n and (text[j].isalnum() or text[j] in "._'"):
                if text[j] in "eEpP" and j + 1 < n and text[j + 1] in "+-" and not text[i:j].lower().startswith("0x"):
                    j += 1
                j += 1
            toks.append(("num", text[i:j], line))
            i = j
            continue
        if c == '"' or c == "'":
            j = i + 1
            while j < n and text[j] != c:
                if text[j] == "\\":
                    j += 1
                if j < n and text[j] == "\n":
                    line += 1
                j += 1
            toks.append(("str", "", line))
            i = j + 1
            continue
        three = text[i:i + 3]
        two = text[i:i + 2]
        if three in MULTI3:
            toks.append(("p", three, line))
            i += 3
        elif two in MULTI2:
            toks.append(("p", two, line))
            i += 2
        else:
            toks.append(("p", c, line))
            i += 1
    return toks, includes


def number_value(text):
    """The integer a literal spells, or None (a float, junk)."""
    t = text.replace("'", "").lower()
    while t and t[-1] in "ul":
        t = t[:-1]
    try:
        if t.startswith("0x"):
            return int(t, 16)
        if t.startswith("0b"):
            return int(t, 2)
        if "." in t or "e" in t or "f" in t:
            return None
        if len(t) > 1 and t[0] == "0":
            return int(t, 8)
        return int(t)
    except ValueError:
        return None


def jt(toks):
    """Tokens joined into one string ('a.b->c' keeps its shape)."""
    return "".join(t[1] if t[0] != "str" else '""' for t in toks)


def split_top(toks, sep):
    """toks split at the top level separator token text (parentheses, brackets, braces nest)."""
    out, cur, d = [], [], 0
    for t in toks:
        if t[0] == "p" and t[1] in "([{":
            d += 1
        elif t[0] == "p" and t[1] in ")]}":
            d -= 1
        if t[0] == "p" and t[1] == sep and d == 0:
            out.append(cur)
            cur = []
        else:
            cur.append(t)
    out.append(cur)
    return out


# ----------------------------------------------------------------------------------------------------------------------------- one file


class Const:
    def __init__(self, name, expr, scopes, types, file, idx, local):
        self.name, self.expr, self.scopes, self.types, self.file, self.idx, self.local = name, expr, scopes, types, file, idx, local
        self.auto = False  # an enumerator without a value: the one before plus one
        self.prev = None


class Def:
    """A function definition with a descriptor set builder parameter."""

    def __init__(self, name, quals, params, body, file):
        self.name, self.quals, self.params, self.body, self.file = name, quals, params, body, file  # params: [(name, is_builder, has_default)]


class SrcFile:
    def __init__(self, path, text):
        self.path = path
        self.toks, self.includes = lex(text)
        n = len(self.toks)
        self.pair = [-1] * n
        stack = []
        for i, t in enumerate(self.toks):
            if t[0] != "p":
                continue
            s = t[1]
            if s in ("(", "[", "{"):
                stack.append(i)
            elif s in (")", "]", "}"):
                want = {")": "(", "]": "[", "}": "{"}[s]
                while stack and self.toks[stack[-1]][1] != want:
                    stack.pop()
                if stack:
                    o = stack.pop()
                    self.pair[o] = i
                    self.pair[i] = o
        self.scopes = []  # (kind, name, open, close)
        self._scan_scopes()
        self.consts = []
        self.aliases = {}
        self._scan_consts()
        self.defs = []
        self._scan_defs()

    def stmt_start(self, i):
        """Index of the first token of the statement that the token before i belongs to (after the previous ; { or } at the same nesting)."""
        j = i - 1
        while j >= 0:
            t = self.toks[j]
            if t[0] == "p":
                if t[1] in (")", "]") and self.pair[j] >= 0:
                    j = self.pair[j] - 1
                    continue
                if t[1] in (";", "{", "}"):
                    return j + 1
            j -= 1
        return 0

    def _scan_scopes(self):
        for i, t in enumerate(self.toks):
            if t[0] != "p" or t[1] != "{" or self.pair[i] < 0:
                continue
            head = self.toks[self.stmt_start(i):i]
            texts = [h[1] for h in head]
            words = [h[1] for h in head if h[0] == "id"]
            has_paren = "(" in texts
            kind, name = "block", ""
            if "=" in texts:
                kind = "block"
            elif "namespace" in words and not has_paren:
                kind = "ns"
                k = max(j for j, h in enumerate(head) if h[1] == "namespace")
                ids = [h[1] for h in head[k + 1:] if h[0] == "id"]
                name = ids[-1] if ids else ""
            elif "extern" in words and not has_paren:
                kind = "ns"
            elif "enum" in words:
                kind = "enum"
                k = [j for j, h in enumerate(head) if h[1] == "enum"][0]
                ids = [h[1] for h in head[k + 1:] if h[0] == "id" and h[1] not in ("class", "struct")]
                name = ids[0] if ids else ""
            else:
                for j, h in enumerate(head):
                    if h[0] == "id" and h[1] in ("struct", "class", "union") and "(" not in texts[:j]:
                        ids = [x[1] for x in head[j + 1:] if x[0] == "id"]
                        if ids and ids[0] != "final":
                            kind, name = "type", ids[0]
                        break
            self.scopes.append((kind, name, i, self.pair[i]))

    def chain_at(self, p):
        """The scopes around token p, outermost first."""
        out = [sc for sc in self.scopes if sc[2] < p < sc[3]]
        out.sort(key=lambda sc: sc[2])
        return out

    def local_block(self, p):
        for sc in self.chain_at(p):
            if sc[0] == "block":
                return (sc[2], sc[3])
        return None

    def enclosing_classes(self, p):
        """Names of the classes a use at p can see: the lexical ones and the qualifiers of the function definition that holds p."""
        names = []
        chain = self.chain_at(p)
        for sc in chain:
            if sc[0] == "type":
                names.append(sc[1])
        for sc in chain:
            if sc[0] == "block":
                head = self.toks[self.stmt_start(sc[2]):sc[2]]
                for j, h in enumerate(head):
                    if h[1] == "(":
                        k = j - 1
                        while k >= 2 and head[k][0] == "id" and head[k - 1][1] == "::" and head[k - 2][0] == "id":
                            names.append(head[k - 2][1])
                            k -= 2
                        break
                break
        return names

    def _scan_consts(self):
        toks, n = self.toks, len(self.toks)
        i = 0
        while i < n:
            s = i
            j = i
            while j < n:
                t = toks[j]
                if t[0] == "p":
                    if t[1] in ("(", "[") and self.pair[j] >= 0:
                        j = self.pair[j] + 1
                        continue
                    if t[1] in (";", "{", "}"):
                        break
                j += 1
            if j < n and toks[j][1] == ";" and j > s:
                self._stmt(s, toks[s:j])
            i = j + 1
        for sc in self.scopes:
            if sc[0] == "enum":
                self._enum(sc)

    def _names_of(self, chain):
        scopes = [sc[1] for sc in chain if sc[0] in ("ns", "type", "enum") and sc[1]]
        types = [sc[1] for sc in chain if sc[0] == "type" and sc[1]]
        return scopes, types

    def _stmt(self, s, stmt):
        texts = [t[1] for t in stmt]
        if texts[0] == "namespace" and "=" in texts:
            k = texts.index("=")
            ids = [t[1] for t in stmt[k + 1:] if t[0] == "id"]
            if ids and k >= 2:
                self.aliases.setdefault(texts[1], []).append(ids[-1])
            return
        if "constexpr" not in texts and "const" not in texts:
            return
        if "=" not in texts:
            return
        eq = texts.index("=")
        pre = stmt[:eq]
        if "(" in [t[1] for t in pre] or not pre or pre[-1][0] != "id":
            return
        expr = split_top(stmt[eq + 1:], ",")[0]
        scopes, types = self._names_of(self.chain_at(s))
        self.consts.append(Const(pre[-1][1], expr, scopes, types, self, s, self.local_block(s)))

    def _enum(self, sc):
        _, ename, o, c = sc
        scopes, types = self._names_of(self.chain_at(o))
        local = self.local_block(o)
        prev = None
        for item in split_top(self.toks[o + 1:c], ","):
            if not item or item[0][0] != "id":
                continue
            name = item[0][1]
            has_value = len(item) > 2 and item[1][1] == "="
            cst = Const(name, item[2:] if has_value else [], scopes + ([ename] if ename else []), types, self, o, local)
            if not has_value:
                cst.auto = True
                cst.prev = prev
            self.consts.append(cst)
            prev = cst

    def _scan_defs(self):
        toks, n = self.toks, len(self.toks)
        for i in range(1, n):
            if toks[i][0] != "p" or toks[i][1] != "(" or self.pair[i] < 0 or toks[i - 1][0] != "id":
                continue
            c = self.pair[i]
            j = c + 1
            while j < n and toks[j][0] == "id" and toks[j][1] in ("const", "noexcept", "override", "final"):
                j += 1
            if j >= n or toks[j][1] != "{" or self.pair[j] < 0:
                continue
            plist, any_builder = [], False
            for p in split_top(toks[i + 1:c], ","):
                texts = [t[1] for t in p]
                eq = texts.index("=") if "=" in texts else len(texts)
                ids = [t[1] for t in p[:eq] if t[0] == "id"]
                is_b = "RenderDescriptorSetBuilder" in texts[:eq]
                any_builder = any_builder or is_b
                plist.append((ids[-1] if ids else "", is_b, "=" in texts))
            if not any_builder:
                continue
            quals = []
            k = i - 1
            while k >= 2 and toks[k - 1][1] == "::" and toks[k - 2][0] == "id":
                quals.append(toks[k - 2][1])
                k -= 2
            self.defs.append(Def(toks[i - 1][1], quals, plist, (j, self.pair[j]), self))


# ----------------------------------------------------------------------------------------------------------------------------- the tree


class Tree:
    """The sources: a folder on disk, or (for a sweep over git trees) any object with names(), read(name), key(name) and has_keywords(name)."""

    def __init__(self, root, source=None, cache=None):
        self.root = root
        self.source = source
        self.files = cache if cache is not None else {}  # key -> SrcFile (lexed on demand)
        names = []
        if source is None:
            for d, dirs, fnames in os.walk(root):
                dirs[:] = sorted(x for x in dirs if x not in (".git", "thirdparty", "build", "out"))
                for nm in sorted(fnames):
                    names.append(os.path.relpath(os.path.join(d, nm), root).replace(os.sep, "/"))
        else:
            names = list(source.names())
        self.paths = sorted(p for p in names if p.endswith((".cpp", ".h", ".hpp", ".cc")) and not p.endswith("_test.cpp") and not p.startswith("thirdparty/"))
        self.by_base = {}
        for p in self.paths:
            self.by_base.setdefault(os.path.basename(p), []).append(p)
        self._closure = {}
        self.interesting = [p for p in self.paths if self.has_keywords(p)]

    def read(self, p):
        if self.source is not None:
            return self.source.read(p)
        with open(os.path.join(self.root, p), encoding="utf-8", errors="replace") as fh:
            return fh.read()

    def has_keywords(self, p):
        if self.source is not None:
            return self.source.has_keywords(p)
        raw = self.read(p)
        return any(k in raw for k in KEYWORD_FILES)

    def get(self, p):
        k = self.source.key(p) if self.source is not None else p
        f = self.files.get(k)
        if f is None:
            f = SrcFile(p, self.read(p))
            self.files[k] = f
        return f

    def resolve_include(self, inc, from_path):
        cands = self.by_base.get(os.path.basename(inc), [])
        if not cands:
            return None
        inc_n = inc.replace("\\", "/")
        best = [c for c in cands if c == inc_n or c.endswith("/" + inc_n)]
        if best:
            return best[0]
        here = [c for c in cands if os.path.dirname(c) == os.path.dirname(from_path)]
        return (here or cands)[0]

    def closure(self, p):
        """The file and every header it includes, transitively (in include order)."""
        got = self._closure.get(p)
        if got is not None:
            return got
        seen, todo = [], [p]
        while todo:
            q = todo.pop(0)
            if q in seen:
                continue
            seen.append(q)
            for inc in self.get(q).includes:
                r = self.resolve_include(inc, q)
                if r:
                    todo.append(r)
        self._closure[p] = seen
        return seen


# ----------------------------------------------------------------------------------------------------------------------------- expressions

CAP = 64


def _binop(a, b, fn):
    if a is None or b is None:
        return None
    out = set()
    for x in a:
        for y in b:
            v = fn(x, y)
            if v is None:
                return None
            out.add(v)
            if len(out) > CAP:
                return None
    return frozenset(out)


def _div(x, y):
    return None if y == 0 else x // y


def _mod(x, y):
    return None if y == 0 else x % y


class Evaluator:
    def __init__(self, tree):
        self.tree = tree
        self.memo = {}
        self.busy = set()

    def alias_targets(self, files, q):
        out = [q]
        for path in files:
            for t in self.tree.get(path).aliases.get(q, ()):
                if t not in out:
                    out.append(t)
        return out

    def lookup(self, f, p, names):
        """The values of the constant named by `names` (['A', 'B', 'kName']) used in file f at token p, or None."""
        name, quals = names[-1], names[:-1]
        files = self.tree.closure(f.path)
        enclosing = f.enclosing_classes(p) if not quals else []
        cands = []  # (same file, local, const)
        for path in files:
            sf = self.tree.get(path)
            for c in sf.consts:
                if c.name != name:
                    continue
                if c.local is not None and not (sf is f and c.local[0] < p < c.local[1]):
                    continue
                if quals:
                    inner = c.scopes[-1] if c.scopes else ""
                    if inner not in self.alias_targets(files, quals[-1]):
                        continue
                elif c.types and not any(t in enclosing for t in c.types):
                    continue
                cands.append((sf is f, c.local is not None, c))
        if not cands:
            return None
        for pick in (1, 0):
            sub = [x for x in cands if x[pick]]
            if sub:
                cands = sub
                break
        vals = set()
        for _, _, c in cands:
            v = self.const_value(c)
            if v is None:
                return None
            vals |= v
        return frozenset(vals) if len(vals) <= CAP else None

    def const_value(self, c):
        key = id(c)
        if key in self.memo:
            return self.memo[key]
        if key in self.busy:
            return None
        self.busy.add(key)
        if c.auto:
            if c.prev is None:
                v = frozenset([0])
            else:
                pv = self.const_value(c.prev)
                v = frozenset(x + 1 for x in pv) if pv is not None else None
        else:
            v = self.eval(c.expr, c.file, c.idx)
        self.busy.discard(key)
        self.memo[key] = v
        return v

    def eval(self, toks, f, p, env=None):
        """The set of values of the expression, or None when it is not something this check can evaluate."""
        parser = Parser(self, toks, f, p, env or {})
        v = parser.expr()
        if parser.bad or parser.i != len(toks):
            return None
        return v

    def one(self, toks, f, p, env=None):
        v = self.eval(toks, f, p, env)
        return next(iter(v)) if v is not None and len(v) == 1 else None


class Parser:
    def __init__(self, ev, toks, f, p, env):
        self.ev, self.toks, self.f, self.p, self.env = ev, toks, f, p, env
        self.i = 0
        self.bad = False

    def peek(self, k=0):
        j = self.i + k
        return self.toks[j][1] if j < len(self.toks) else None

    def take(self):
        t = self.toks[self.i]
        self.i += 1
        return t

    def expr(self):
        c = self.lor()
        if self.peek() == "?":
            self.i += 1
            a = self.expr()
            if self.peek() != ":":
                self.bad = True
                return None
            self.i += 1
            b = self.expr()
            if c is not None and c == frozenset([0]):
                return b
            if c is not None and 0 not in c:
                return a
            if a is None or b is None:
                return None
            u = a | b
            return u if len(u) <= CAP else None
        return c

    def binary(self, sub, ops):
        a = sub()
        while self.peek() in ops:
            op = self.take()[1]
            a = _binop(a, sub(), ops[op])
        return a

    def lor(self):
        return self.binary(self.land, {"||": lambda x, y: int(bool(x) or bool(y))})

    def land(self):
        return self.binary(self.bor, {"&&": lambda x, y: int(bool(x) and bool(y))})

    def bor(self):
        return self.binary(self.bxor, {"|": lambda x, y: x | y})

    def bxor(self):
        return self.binary(self.band, {"^": lambda x, y: x ^ y})

    def band(self):
        return self.binary(self.eq, {"&": lambda x, y: x & y})

    def eq(self):
        return self.binary(self.rel, {"==": lambda x, y: int(x == y), "!=": lambda x, y: int(x != y)})

    def rel(self):
        return self.binary(self.shift, {"<": lambda x, y: int(x < y), ">": lambda x, y: int(x > y), "<=": lambda x, y: int(x <= y), ">=": lambda x, y: int(x >= y)})

    def shift(self):
        return self.binary(self.add, {"<<": lambda x, y: x << y if 0 <= y < 64 else None, ">>": lambda x, y: x >> y if 0 <= y < 64 else None})

    def add(self):
        return self.binary(self.mul, {"+": lambda x, y: x + y, "-": lambda x, y: x - y})

    def mul(self):
        return self.binary(self.unary, {"*": lambda x, y: x * y, "/": _div, "%": _mod})

    def unary(self):
        t = self.peek()
        if t == "-":
            self.i += 1
            a = self.unary()
            return None if a is None else frozenset(-x for x in a)
        if t == "+":
            self.i += 1
            return self.unary()
        if t == "!":
            self.i += 1
            a = self.unary()
            return None if a is None else frozenset(int(not x) for x in a)
        if t == "~":
            self.i += 1
            a = self.unary()
            return None if a is None else frozenset((~x) & 0xFFFFFFFF for x in a)
        return self.primary()

    def close_paren(self):
        if self.peek() != ")":
            self.bad = True
            return False
        self.i += 1
        return True

    def primary(self):
        if self.i >= len(self.toks):
            self.bad = True
            return None
        kind, t, _ = self.toks[self.i]
        if kind == "num":
            self.i += 1
            v = number_value(t)
            return None if v is None else frozenset([v])
        if t == "(":
            if self.peek(1) in CAST_TYPES and self.peek(2) == ")":
                ty = self.peek(1)
                self.i += 3
                return self.cast(self.unary(), ty)
            self.i += 1
            a = self.expr()
            return a if self.close_paren() else None
        if kind == "id":
            if t in ("true", "false"):
                self.i += 1
                return frozenset([1 if t == "true" else 0])
            if t == "static_cast" and self.peek(1) == "<":
                j = self.i + 2
                ty = ""
                while j < len(self.toks) and self.toks[j][1] != ">":
                    ty = self.toks[j][1]
                    j += 1
                self.i = j + 1
                if self.peek() != "(":
                    self.bad = True
                    return None
                self.i += 1
                a = self.expr()
                return self.cast(a, ty) if self.close_paren() else None
            if t in CAST_TYPES and self.peek(1) == "(":
                self.i += 2
                a = self.expr()
                return self.cast(a, t) if self.close_paren() else None
            names = [t]
            self.i += 1
            while self.peek() == "::" and self.i + 1 < len(self.toks) and self.toks[self.i + 1][0] == "id":
                names.append(self.toks[self.i + 1][1])
                self.i += 2
            if self.peek() in ("(", ".", "->", "[", "<"):
                self.swallow()  # a call, a member, a template: not a constant here
                return None
            if len(names) == 1 and names[0] in self.env:
                return self.env[names[0]]
            return self.ev.lookup(self.f, self.p, names)
        self.bad = True
        return None

    def swallow(self):
        while self.i < len(self.toks):
            t = self.toks[self.i][1]
            if t in ("(", "["):
                d = 1
                self.i += 1
                while self.i < len(self.toks) and d:
                    u = self.toks[self.i][1]
                    if u in ("(", "["):
                        d += 1
                    elif u in (")", "]"):
                        d -= 1
                    self.i += 1
                continue
            if t in (".", "->", "::") or (self.toks[self.i][0] == "id" and self.i > 0 and self.toks[self.i - 1][1] in (".", "->", "::")):
                self.i += 1
                continue
            break

    def cast(self, a, ty):
        if a is None:
            return None
        if ty in ("uint32_t", "unsigned"):
            return frozenset(x & 0xFFFFFFFF for x in a)
        return a


# ----------------------------------------------------------------------------------------------------------------------------- builders

KIND_TYPE = {
    "ConstantBuffer": "cbv", "FormattedBuffer": "srv-formatted", "ReadWriteFormattedBuffer": "uav-formatted", "Texture": "texture", "ReadWriteTexture": "rw-texture",
    "Sampler": "sampler", "ImmutableSampler": "sampler-immutable", "StructuredBuffer": "structured", "ReadWriteStructuredBuffer": "rw-structured",
    "ByteAddressBuffer": "byte", "ReadWriteByteAddressBuffer": "rw-byte", "AccelerationStructure": "accel",
}
BUFFER_TYPES = ("cbv", "srv-formatted", "uav-formatted", "structured", "rw-structured", "byte", "rw-byte")
CALL_TYPES = {
    "setTexture": ("texture", "rw-texture"),
    "setBuffer": BUFFER_TYPES,
    "setSampler": ("sampler", "sampler-immutable"),
    "setAccelerationStructure": ("accel",),
}


class Item:
    """One range of a builder, as the fill code adds it."""

    def __init__(self, kind, count, binding, file, line, assigned, via):
        self.kind, self.count, self.reps, self.binding, self.file, self.line, self.assigned, self.via = kind, count, 1, binding, file, line, assigned, via
        self.start = 0
        self.length = 0
        self.placed = False

    @property
    def ty(self):
        return KIND_TYPE.get(self.kind, "?")


class Region:
    def __init__(self, file, recv, line):
        self.file, self.recv, self.line = file, recv, line
        self.items = []
        self.reasons = []  # why the totals are not exact
        self.boundless = None  # the size of end(true, size)
        self.is_boundless = False
        self.assigned = {}
        self.total_views = self.total_samplers = self.total = 0

    def exact(self):
        return not self.reasons

    def finish(self):
        """Positions, totals (views, samplers) as D3D12 counts them, and the index variables."""
        pos = views = samplers = 0
        n = len(self.items)
        for k, it in enumerate(self.items):
            if it.count is None:
                self.reasons.append("the range at line %d has a count that is not a number" % it.line)
                continue
            c = it.count * it.reps
            if k == n - 1 and self.is_boundless:
                if self.boundless is None:
                    self.reasons.append("the boundless size is not a number")
                    continue
                c = self.boundless
            it.start, it.length, it.placed = pos, c, True
            pos += c
            if it.ty == "sampler":
                samplers += c
            else:
                views += c
            if it.assigned:
                self.assigned[it.assigned] = it.start
        self.total_views, self.total_samplers, self.total = views, samplers, pos

    def type_at(self, i):
        for it in self.items:
            if it.placed and it.start <= i < it.start + it.length:
                return it
        return None


class Site:
    """A builder that can arrive at a CreateDescriptorSet call: the regions that fill it, and what the caller says of the other parameters."""

    def __init__(self, who, regions, env):
        self.who, self.regions, self.env = who, regions, env


class Audit:
    def __init__(self, tree):
        self.tree = tree
        self.ev = Evaluator(tree)
        self.lines = []  # (level, file, line, kind, text)
        self.stats = {"calls": 0, "judged": 0, "unjudged": 0, "errors": 0, "bindings": 0, "bindings_judged": 0}
        self.regions = {}  # (path, recv) -> [Region]
        self.defs = {}  # name -> [Def]
        self.agree = []
        self.members = {}  # a helper's returned struct member -> its range type ('' when two disagree)

    def add(self, level, path, line, kind, text):
        self.lines.append((level, path, line, kind, text))

    def error(self, path, line, kind, text):
        self.stats["errors"] += 1
        self.add("ERROR", path, line, kind, text)

    # ---- fill code: the ranges between begin() and end()
    def fill(self, f, lo, hi, rt, region, via, depth):
        toks = f.toks
        i = lo
        while i < hi:
            t = toks[i]
            nxt_is_paren = i + 1 < hi and toks[i + 1][1] == "(" and f.pair[i + 1] >= 0
            if t[0] == "id" and t[1] == "for" and nxt_is_paren:
                c = f.pair[i + 1]
                b_lo, b_hi, nxt = self.body_of(f, c + 1, hi)
                trip = self.trip(f, i + 1, c)
                sub = Region(f, region.recv, region.line)
                self.fill(f, b_lo, b_hi, rt, sub, via, depth)
                if trip is None and sub.items:
                    region.reasons.append("line %d: a loop whose count is not a number holds ranges" % t[2])
                for it in sub.items:
                    it.reps *= trip if trip is not None else 1
                region.items.extend(sub.items)
                region.reasons.extend(sub.reasons)
                i = nxt
                continue
            if t[0] == "id" and t[1] in ("if", "switch", "while") and nxt_is_paren:
                b_lo, b_hi, nxt = self.body_of(f, f.pair[i + 1] + 1, hi)
                sub = Region(f, region.recv, region.line)
                self.fill(f, b_lo, b_hi, rt, sub, via, depth)
                if sub.items:
                    region.reasons.append("line %d: ranges are added under a condition" % t[2])
                region.items.extend(sub.items)
                region.reasons.extend(sub.reasons)
                i = nxt
                continue
            if t[0] == "id" and self.match_recv(f, i, rt):
                k = i + len(rt)
                if k + 2 < len(toks) and toks[k][1] == "." and toks[k + 1][0] == "id" and toks[k + 1][1].startswith("add") and toks[k + 2][1] == "(" and f.pair[k + 2] >= 0:
                    self.add_range(f, i, k, rt, region, via)
                    i = f.pair[k + 2] + 1
                    continue
            if t[0] == "id" and nxt_is_paren and t[1] not in ("sizeof", "return", "static_cast"):
                args = self.split_args(f, i + 1)
                hit = -1
                for ai, a in enumerate(args):
                    if jt(a).lstrip("&*") == jt(rt):
                        hit = ai
                if hit >= 0:
                    self.helper_call(f, i, args, hit, rt, region, depth)
                    i = f.pair[i + 1] + 1
                    continue
            i += 1

    def body_of(self, f, b, hi):
        """The statement (a block or a single statement) that starts at token b: (first token, end, the token after it)."""
        toks = f.toks
        if b < hi and toks[b][1] == "{" and f.pair[b] >= 0:
            return b + 1, f.pair[b], f.pair[b] + 1
        e = b
        while e < hi and toks[e][1] != ";":
            if toks[e][1] in ("(", "[", "{") and f.pair[e] >= 0:
                e = f.pair[e]
            e += 1
        return b, e, e + 1

    def add_range(self, f, i, k, rt, region, via):
        toks = f.toks
        kind = toks[k + 1][1][3:]
        line = toks[k + 1][2]
        args = self.split_args(f, k + 2)
        assigned = None
        if i >= 2 and toks[i - 1][1] == "=":
            j = i - 2
            while j >= 0 and (toks[j][0] == "id" or toks[j][1] in (".", "->", "::")):
                j -= 1
            assigned = jt(toks[j + 1:i - 1])
        if kind not in KIND_TYPE:
            region.reasons.append("line %d: add%s is not a range this check knows" % (line, kind))
        count = 1
        if kind == "ImmutableSampler":
            if len(args) >= 3:
                count = self.ev.one(args[2], f, i)
        elif len(args) >= 2:
            count = self.ev.one(args[1], f, i)
        binding = self.ev.one(args[0], f, i) if args else None
        region.items.append(Item(kind, count, binding, f, line, assigned, via))

    def helper_call(self, f, i, args, hit, rt, region, depth):
        toks = f.toks
        name = toks[i][1]
        qual = toks[i - 2][1] if i >= 2 and toks[i - 1][1] == "::" and toks[i - 2][0] == "id" else None
        cands = self.defs.get(name, [])
        if qual is not None:
            cands = [d for d in cands if d.quals and d.quals[0] == qual] or cands
        for d in cands:
            if hit < len(d.params) and d.params[hit][1] and depth < 4:
                lo2, hi2 = d.body
                sub = Region(d.file, region.recv, region.line)
                self.fill(d.file, lo2 + 1, hi2, [("id", d.params[hit][0], 0)], sub, name, depth + 1)
                for it in sub.items:
                    it.via = name
                    if it.assigned and "." in it.assigned:
                        member = it.assigned.split(".")[-1]
                        old = self.members.get(member)
                        self.members[member] = it.ty if old in (None, it.ty) else ""
                region.items.extend(sub.items)
                region.reasons.extend(sub.reasons)
                return
        if name not in ("addDescriptorSet", "CreateDescriptorSet"):
            region.reasons.append("line %d: the builder is passed to %s, which this check could not find" % (toks[i][2], name))

    def match_recv(self, f, i, rt):
        toks = f.toks
        if i > 0 and toks[i - 1][1] in (".", "->", "::"):
            return False
        for k, r in enumerate(rt):
            if i + k >= len(toks) or toks[i + k][1] != r[1]:
                return False
        return True

    def split_args(self, f, open_idx):
        close = f.pair[open_idx]
        args = split_top(f.toks[open_idx + 1:close], ",")
        return [] if args == [[]] else args

    def trip(self, f, o, c):
        """Iterations of `for (init; cond; step)` in the usual form `for (T i = A; i < B; ++i)`, or None."""
        parts = split_top(f.toks[o + 1:c], ";")
        if len(parts) != 3:
            return None
        init, cond, step = parts
        texts = [t[1] for t in init]
        if "=" not in texts:
            return None
        var = init[texts.index("=") - 1][1]
        a = self.ev.one(init[texts.index("=") + 1:], f, o)
        if len(cond) < 3 or cond[0][1] != var or cond[1][1] not in ("<", "<="):
            return None
        b = self.ev.one(cond[2:], f, o)
        if a is None or b is None or jt(step) not in ("++" + var, var + "++", var + "+=1"):
            return None
        return max(0, b - a + (1 if cond[1][1] == "<=" else 0))

    # ---- finding things
    def collect_defs(self):
        for p in self.tree.interesting:
            for d in self.tree.get(p).defs:
                self.defs.setdefault(d.name, []).append(d)

    def collect_regions(self, f):
        toks = f.toks
        n = len(toks)
        for i in range(2, n - 2):
            if not (toks[i][1] == "begin" and toks[i][0] == "id" and toks[i + 1][1] == "(" and toks[i + 2][1] == ")" and toks[i - 1][1] in (".", "->")):
                continue
            j = i - 2
            while j >= 0 and (toks[j][0] == "id" or toks[j][1] in (".", "->", "::")):
                j -= 1
            rt = toks[j + 1:i - 1]
            if not rt or (j >= 0 and toks[j][1] in (")", "]")):
                continue  # a layout builder's begin(a, b) has arguments; a receiver that is a call or an element is not followed
            end = None
            for k in range(i + 3, n - 1):
                if toks[k][1] == "end" and toks[k][0] == "id" and toks[k - 1][1] in (".", "->") and toks[k + 1][1] == "(" and self.match_recv(f, k - 1 - len(rt), rt):
                    end = k
                    break
            if end is None:
                continue
            region = Region(f, jt(rt), toks[i][2])
            self.fill(f, i + 3, end - 1 - len(rt), rt, region, None, 0)
            args = self.split_args(f, end + 1)
            if args and jt(args[0]) == "true":
                region.is_boundless = True
                region.boundless = self.ev.one(args[1], f, end) if len(args) > 1 else None
            region.finish()
            self.regions.setdefault((f.path, region.recv), []).append(region)

    # ---- the calls
    def calls(self, f):
        toks = f.toks
        creators = self.set_names(f)
        for i in range(1, len(toks) - 1):
            if toks[i][0] != "id" or toks[i][1] != "CreateDescriptorSet" or toks[i + 1][1] != "(" or toks[i - 1][1] not in (".", "->") or f.pair[i + 1] < 0:
                continue
            self.stats["calls"] += 1
            args = self.split_args(f, i + 1)
            line = toks[i][2]
            if len(args) != 4:
                self.error(f.path, line, "args", "CreateDescriptorSet takes (builder, views, samplers, name); this call has %d arguments" % len(args))
                continue
            label = jt(args[0]).lstrip("*&")
            sites = self.resolve(f, i, args[0])
            if sites is None:
                self.unjudged(f, line, label, "no builder with this name is filled (begin() .. end()) in this file, and it is not a parameter whose callers are found")
                continue
            judged, whole = False, True
            exact_regions = []
            for site in sites:
                views = self.ev.eval(args[1], f, i, site.env)
                samplers = self.ev.eval(args[2], f, i, site.env)
                if views is None or samplers is None:
                    self.unjudged(f, line, label, "the counts '%s', '%s' are not numbers this check can evaluate" % (jt(args[1]), jt(args[2])))
                    whole = False
                    continue
                for r in site.regions:
                    if not r.exact():
                        self.unjudged(f, line, label, "the ranges of %s are not exact: %s" % (site.who, "; ".join(r.reasons)))
                        whole = False
                        continue
                    judged = True
                    exact_regions.append(r)
                    if r.total_views in views and r.total_samplers in samplers:
                        self.agree.append("%s:%d %s %d views + %d samplers" % (f.path, line, label, r.total_views, r.total_samplers))
                    else:
                        self.error(f.path, line, "count", "%s: CreateDescriptorSet says %s views + %s samplers, the ranges of %s (%s:%d) add up to %d + %d: D3D12 refuses the set (Vulkan does not count)" % (
                            label, self.fmt(views), self.fmt(samplers), site.who, r.file.path, r.line, r.total_views, r.total_samplers))
            if judged:
                self.stats["judged"] += 1
            if whole and len(exact_regions) == 1:
                self.bindings(f, i, self.lhs_of(f, i), exact_regions[0], label, creators)

    @staticmethod
    def fmt(s):
        return "/".join(str(x) for x in sorted(s))

    def unjudged(self, f, line, label, why):
        self.stats["unjudged"] += 1
        self.add("UNJUDGED", f.path, line, "unjudged", "%s: %s" % (label, why))

    def lhs_of(self, f, i):
        """The assigned variable of `x = device->CreateDescriptorSet(` as text, `[..]` removed; '' when the call is not an assignment."""
        toks = f.toks
        j = i - 1
        while j >= 0 and (toks[j][0] == "id" or toks[j][1] in (".", "->", "::")):
            j -= 1
        if j >= 1 and toks[j][1] == "=":
            k = j - 1
            parts = []
            while k >= 0:
                t = toks[k]
                if t[1] == "]" and f.pair[k] >= 0:
                    k = f.pair[k] - 1
                    continue
                if t[0] == "id" or t[1] in (".", "->"):
                    parts.append(t[1])
                    k -= 1
                    continue
                break
            parts.reverse()
            return "".join(parts)
        return ""

    def set_names(self, f):
        """How many CreateDescriptorSet calls in the file assign to each last name."""
        out = {}
        toks = f.toks
        for i in range(1, len(toks) - 1):
            if toks[i][1] == "CreateDescriptorSet" and toks[i + 1][1] == "(" and toks[i - 1][1] in (".", "->"):
                lhs = self.lhs_of(f, i)
                if lhs:
                    last = lhs.split(".")[-1].split("->")[-1]
                    out[last] = out.get(last, 0) + 1
        return out

    def enclosing_def(self, f, i):
        best = None
        for d in f.defs:
            if d.body[0] < i < d.body[1] and (best is None or d.body[0] > best.body[0]):
                best = d
        return best

    def param_env(self, d, pi, args):
        """What a caller says of the other parameters: nullptr / 0 / false are 0, an address is nonzero; anything else is unknown."""
        env = {}
        for j, (pn, is_b, _) in enumerate(d.params):
            if j == pi or j >= len(args) or not pn:
                continue
            t = jt(args[j])
            if t in ("nullptr", "0", "false", "NULL"):
                env[pn] = frozenset([0])
            elif t.startswith("&"):
                env[pn] = frozenset([1])
        return env

    def resolve(self, f, i, arg):
        """The builders that can arrive at the call i as a list of Site, or None when the builder cannot be found."""
        text = jt(arg).lstrip("*&")
        direct = self.regions.get((f.path, text))
        if direct:
            return [Site(text, direct, {})]
        d = self.enclosing_def(f, i)
        param = None
        via_member = True
        if d is not None:
            for pi, (pn, is_b, _) in enumerate(d.params):
                if is_b and pn == text:
                    param = (d, pi)
                    via_member = False
        if param is None:
            # `member = &param;` or `member = param;` inside a definition of this file that has the builder parameter
            for dd in f.defs:
                for k in range(dd.body[0], dd.body[1]):
                    if f.toks[k][1] == text and f.toks[k + 1][1] == "=":
                        rhs = f.toks[k + 2:k + 4]
                        nm = rhs[1][1] if len(rhs) > 1 and rhs[0][1] == "&" else (rhs[0][1] if rhs else "")
                        for pi, (pn, is_b, _) in enumerate(dd.params):
                            if is_b and pn == nm:
                                param = (dd, pi)
        if param is None:
            return None
        d, pi = param
        sites = []
        for p2 in self.tree.interesting:
            g = self.tree.get(p2)
            toks = g.toks
            for k in range(len(toks) - 1):
                if toks[k][0] != "id" or toks[k][1] != d.name or toks[k + 1][1] != "(" or g.pair[k + 1] < 0:
                    continue
                args = self.split_args(g, k + 1)
                # the default arguments live in the header: any count from the builder's position up to all of them is a call
                if pi >= len(args) or len(args) > len(d.params) or any(x[1] == "RenderDescriptorSetBuilder" for x in args[pi]):
                    continue  # the definition or a declaration is not a call
                cand = jt(args[pi]).lstrip("&*")
                regs = self.regions.get((g.path, cand))
                if regs:
                    sites.append(Site("%s (passed at %s:%d)" % (cand, g.path, toks[k][2]), regs, {} if via_member else self.param_env(d, pi, args)))
        return sites or None

    # ---- the binding code
    def receiver_of(self, f, k):
        """The receiver chain of the call whose name is token k (after '.' or '->'), without [..] and .get(), as text."""
        toks = f.toks
        j = k - 2
        chain = []
        while j >= 0:
            u = toks[j]
            if u[1] == "]" and f.pair[j] >= 0:
                j = f.pair[j] - 1
                continue
            if u[1] == ")" and f.pair[j] >= 2 and toks[f.pair[j] - 1][1] == "get" and toks[f.pair[j] - 2][1] in (".", "->"):
                j = f.pair[j] - 3
                continue
            if u[0] == "id" or u[1] in (".", "->"):
                chain.append(u[1])
                j -= 1
                continue
            break
        chain.reverse()
        return "".join(chain)

    def binding_calls(self, f):
        """[(token index of the call name, receiver text)] of every set* call in the file."""
        toks = f.toks
        out = []
        for k in range(2, len(toks) - 1):
            t = toks[k]
            if t[0] == "id" and t[1] in CALL_TYPES and toks[k + 1][1] == "(" and toks[k - 1][1] in (".", "->") and f.pair[k + 1] >= 0:
                out.append((k, self.receiver_of(f, k)))
        return out

    def bindings(self, f, call_idx, key, r, label, creators):
        if not key:
            return
        toks = f.toks
        last = key.split(".")[-1].split("->")[-1]
        calls = [(k, recv) for k, recv in self.binding_calls(f) if recv == key or (recv.split(".")[-1].split("->")[-1] == last and creators.get(last, 0) <= 1)]
        if not calls:
            self.add("NOTE", f.path, toks[call_idx][2], "nobinding", "%s: no setTexture/setBuffer call on '%s' in this file (the set is filled elsewhere): the descriptors are not checked" % (label, key))
            return
        written = set()
        all_known = True
        for k, recv in calls:
            self.stats["bindings"] += 1
            api = toks[k][1]
            args = self.split_args(f, k + 1)
            line = toks[k][2]
            idx_vals = None
            if args:
                a0 = jt(args[0])
                if a0 in r.assigned:
                    idx_vals = frozenset([r.assigned[a0]])
                else:
                    idx_vals = self.ev.eval(args[0], f, k, self.loop_env(f, k))
            if idx_vals is None:
                all_known = False
                continue
            self.stats["bindings_judged"] += 1
            for iv in sorted(idx_vals):
                if iv >= r.total:
                    self.error(f.path, line, "index", "%s: %s(%d, ..) is past the set: the builder (%s:%d) has %d descriptors (D3D12 writes into the next set's slots of the shared heap)" % (
                        label, api, iv, r.file.path, r.line, r.total))
                    continue
                it = r.type_at(iv)
                if it is None:
                    continue
                written.add(iv)
                self.check_call(f, line, label, api, args, it, iv, r)
        if all_known:
            for it in r.items:
                if not it.placed or it.via:
                    continue  # a helper's ranges are written where the helper's struct is used
                for d in range(it.start, min(it.start + it.length, it.start + 64)):
                    if d not in written:
                        self.add("WARN", f.path, toks[call_idx][2], "unwritten", "%s: descriptor %d (%s, line %d of the builder) is never written in this file" % (label, d, it.kind, it.line))
                        break

    def check_call(self, f, line, label, api, args, it, iv, r):
        if it.ty not in CALL_TYPES[api]:
            self.error(f.path, line, "type", "%s: %s fills descriptor %d, which the builder (%s:%d) declares as a %s range" % (label, api, iv, r.file.path, it.line, it.kind))
            return
        if api == "setBuffer" and it.ty in ("structured", "rw-structured"):
            view = args[3] if len(args) >= 4 else None
            if view is None or jt(view) in ("nullptr", "0", "NULL"):
                self.error(f.path, line, "view", "%s: setBuffer fills descriptor %d, a structured buffer range (%s:%d), without its structure view (D3D12 reads stride and element count from it)" % (label, iv, r.file.path, it.line))

    def member_calls(self, f):
        """set* calls on a set that is a parameter, whose index is a member of a helper's returned struct (b.pool): the member's range type is checked."""
        known = set()
        toks = f.toks
        for i in range(1, len(toks) - 1):
            if toks[i][1] == "CreateDescriptorSet" and toks[i + 1][1] == "(" and toks[i - 1][1] in (".", "->"):
                known.add(self.lhs_of(f, i))
        for k, recv in self.binding_calls(f):
            if recv in known:
                continue
            args = self.split_args(f, k + 1)
            if not args:
                continue
            a0 = args[0]
            if not a0 or a0[-1][0] != "id" or a0[-1][1] not in self.members or len(a0) not in (1, 3):
                continue
            ty = self.members[a0[-1][1]]
            if not ty:
                continue
            api = toks[k][1]
            line = toks[k][2]
            label = jt(a0)
            if ty not in CALL_TYPES[api]:
                self.error(f.path, line, "type", "%s: %s fills a %s range of the helper's builder" % (label, api, ty))
            elif api == "setBuffer" and ty in ("structured", "rw-structured") and (len(args) < 4 or jt(args[3]) in ("nullptr", "0", "NULL")):
                self.error(f.path, line, "view", "%s: setBuffer fills a structured buffer range of the helper's builder without its structure view" % label)

    def loop_env(self, f, k):
        """Loop variables (`for (T i = A; i < B; ++i)`) around token k as value sets."""
        env = {}
        toks = f.toks
        for i, t in enumerate(toks[:k]):
            if t[0] == "id" and t[1] == "for" and toks[i + 1][1] == "(" and f.pair[i + 1] >= 0:
                lo, hi, _ = self.body_of(f, f.pair[i + 1] + 1, len(toks))
                if lo - 1 <= k <= hi:
                    c = f.pair[i + 1]
                    head = toks[i + 2:c]
                    texts = [x[1] for x in head]
                    if "=" in texts and ";" in texts:
                        var = head[texts.index("=") - 1][1]
                        n = self.trip(f, i + 1, c)
                        a = self.ev.one(head[texts.index("=") + 1:texts.index(";")], f, i)
                        if n is not None and a is not None:
                            env[var] = frozenset(range(a, a + n))
        return env

    # ---- run
    def run(self):
        self.collect_defs()
        for p in self.tree.interesting:
            self.collect_regions(self.tree.get(p))
        for p in self.tree.interesting:
            self.calls(self.tree.get(p))
        for p in self.tree.interesting:
            self.member_calls(self.tree.get(p))
        for p in self.tree.interesting:
            f = self.tree.get(p)
            if os.path.basename(p) == "device.cpp":
                continue
            toks = f.toks
            for i in range(2, len(toks) - 2):
                if toks[i][1] == "createDescriptorSet" and toks[i + 1][1] == "(":
                    self.add("WARN", p, toks[i][2], "bypass", "createDescriptorSet is called on the Plume device directly: the counts are not checked (use Device::CreateDescriptorSet)")
                if toks[i][1] == "create" and toks[i + 1][1] == "(" and toks[i - 1][1] == "." and toks[i - 2][0] == "id" and (p, toks[i - 2][1]) in self.regions:
                    self.add("WARN", p, toks[i][2], "bypass", "%s.create(..) makes a set from a descriptor builder outside Device::CreateDescriptorSet" % toks[i - 2][1])

    def report(self, show_agree=False):
        order = {"ERROR": 0, "WARN": 1, "UNJUDGED": 2, "NOTE": 3}
        out = []
        for lv, fl, ln, kind, text in sorted(set(self.lines), key=lambda x: (order[x[0]], x[1], x[2], x[3], x[4])):
            out.append("%s %s:%d [%s] %s" % (lv, fl, ln, kind, text))
        if show_agree:
            for a in sorted(set(self.agree)):
                out.append("OK %s" % a)
        s = self.stats
        out.append("descriptor sets: %d CreateDescriptorSet calls, %d judged against their builders, %d unjudged, %d binding calls (%d with a known index), %d errors" % (
            s["calls"], s["judged"], s["unjudged"], s["bindings"], s["bindings_judged"], s["errors"]))
        return out


def audit_tree(root, source=None, cache=None):
    a = Audit(Tree(root, source, cache))
    a.run()
    return a


# ----------------------------------------------------------------------------------------------------------------------------- selftest

TWO = "void F() {\n b_.begin();\n b_.addReadWriteTexture(0);\n b_.addReadWriteByteAddressBuffer(1);\n b_.end();\n"
BASE = "void F() {\n b_.begin();\n a_ = b_.addReadWriteTexture(0);\n c_ = b_.addReadWriteByteAddressBuffer(1);\n b_.end();\n s_ = d->CreateDescriptorSet(b_, 2, 0, \"x\");\n"
ENS = "bool Ens(S& out, plume::RenderDescriptorSetBuilder& builder, int second, const char* name) {\n out.set = d->CreateDescriptorSet(builder, second ? 2u : 1u, 0, name);\n return true;\n}\n"
HIT_H = "struct Hit { static constexpr uint32_t kBindingCount = 4; };\nstruct World { static constexpr uint32_t kBindingCount = rt::kBindingCount; };\nnamespace rt { inline constexpr uint32_t kBindingCount = 2; }\n"
HIT_CPP = ("Hit::Bindings Hit::AddB(plume::RenderDescriptorSetBuilder& builder) {\n  Bindings o;\n  o.a = builder.addByteAddressBuffer(6);\n  o.b = builder.addStructuredBuffer(7);\n"
           "  o.c = builder.addStructuredBuffer(8);\n  o.d = builder.addStructuredBuffer(9);\n  return o;\n}\n"
           "World::Bindings World::AddB(plume::RenderDescriptorSetBuilder& builder) {\n  Bindings o;\n  o.w = builder.addStructuredBuffer(10);\n  o.l = builder.addStructuredBuffer(11);\n  return o;\n}\n")
HIT_USE = "#include \"h.h\"\nvoid G() {\n b_.begin();\n b_.addStructuredBuffer(0);\n Hit::AddB(b_);\n World::AddB(b_);\n b_.end();\n s_ = d->CreateDescriptorSet(b_, 1 + Hit::kBindingCount + World::kBindingCount, 0, \"x\");\n}\n"
LOOP3 = "void F() {\n b_.begin();\n for (uint32_t i = 0; i < 3; ++i) {\n  b_.addReadWriteTexture(i);\n }\n b_.end();\n s_ = d->CreateDescriptorSet(b_, 3, 0, \"x\");\n"
INIT = "void I(plume::RenderDescriptorSetBuilder& output_builder) {\n o_ = &output_builder;\n}\nbool S() {\n set_ = d->CreateDescriptorSet(*o_, %d, 0, \"x\");\n return true;\n}\n"
INIT_USE = "void R() {\n q_.begin();\n q_.addReadWriteTexture(0);\n q_.addReadWriteTexture(1);\n q_.end();\n g_.I(q_);\n}\n"

# (name, {file: text}, error kinds, unjudged count, warn kinds)
CASES = [
    ("the city shadow mistake: two ranges, a count of 1", {"a.cpp": TWO + " s_ = d->CreateDescriptorSet(b_, 1, 0, \"x\");\n}\n"}, ["count"], 0, []),
    ("two ranges, two views", {"a.cpp": TWO + " s_ = d->CreateDescriptorSet(b_, 2, 0, \"x\");\n}\n"}, [], 0, []),
    ("a u suffix", {"a.cpp": TWO + " s_ = d->CreateDescriptorSet(b_, 2u, 0u, \"x\");\n}\n"}, [], 0, []),
    ("one view too many", {"a.cpp": TWO + " s_ = d->CreateDescriptorSet(b_, 3, 0, \"x\");\n}\n"}, ["count"], 0, []),
    ("a sampler count on a view set", {"a.cpp": TWO + " s_ = d->CreateDescriptorSet(b_, 2, 1, \"x\");\n}\n"}, ["count"], 0, []),
    ("a comment", {"a.cpp": TWO + " // s_ = d->CreateDescriptorSet(b_, 1, 0, \"x\");\n}\n"}, [], 0, []),
    ("a constant count", {"a.cpp": "constexpr uint32_t kViews = 2;\n" + TWO + " s_ = d->CreateDescriptorSet(b_, kViews, 0, \"x\");\n}\n"}, [], 0, []),
    ("a wrong constant count", {"a.cpp": "constexpr uint32_t kViews = 5;\n" + TWO + " s_ = d->CreateDescriptorSet(b_, kViews, 0, \"x\");\n}\n"}, ["count"], 0, []),
    ("an unknown count", {"a.cpp": TWO + " s_ = d->CreateDescriptorSet(b_, n, 0, \"x\");\n}\n"}, [], 1, []),
    ("a namespace alias constant", {"r.h": "namespace gi_update { inline constexpr uint32_t kSetViews = 2; }\n",
                                    "a.cpp": "#include \"r.h\"\nnamespace rules = gi_update;\nconstexpr uint32_t kSetViews = rules::kSetViews;\n" + TWO + " s_ = d->CreateDescriptorSet(b_, kSetViews, 0, \"x\");\n}\n"}, [], 0, []),
    ("an enumerator", {"a.cpp": "enum class Bind : uint32_t { kA, kB, kC };\n" + TWO + " s_ = d->CreateDescriptorSet(b_, uint32_t(Bind::kC), 0, \"x\");\n}\n"}, [], 0, []),
    ("class constants and helpers that add ranges", {"h.h": HIT_H, "a.cpp": HIT_CPP + HIT_USE}, [], 0, []),
    ("a helper that adds one more than its constant says", {"h.h": "struct Hit { static constexpr uint32_t kBindingCount = 3; };\n",
                                                          "a.cpp": "#include \"h.h\"\nvoid G() {\n b_.begin();\n Hit::AddB(b_);\n b_.end();\n s_ = d->CreateDescriptorSet(b_, Hit::kBindingCount, 0, \"x\");\n}\n" + HIT_CPP}, ["count"], 0, []),
    ("a loop of ranges", {"a.cpp": "constexpr uint32_t kN = 5;\nvoid F() {\n b_.begin();\n for (uint32_t i = 0; i < kN; ++i) {\n  b_.addReadWriteTexture(i);\n }\n b_.end();\n s_ = d->CreateDescriptorSet(b_, uint32_t(kN), 0, \"x\");\n}\n"}, [], 0, []),
    ("a loop of ranges, one short", {"a.cpp": "constexpr uint32_t kN = 5;\nvoid F() {\n b_.begin();\n for (uint32_t i = 0; i < kN; ++i) {\n  b_.addReadWriteTexture(i);\n }\n b_.end();\n s_ = d->CreateDescriptorSet(b_, 4, 0, \"x\");\n}\n"}, ["count"], 0, []),
    ("samplers", {"a.cpp": "void F() {\n b.begin();\n b.addSampler(0, 8);\n b.addTexture(1);\n b.end();\n x = d.CreateDescriptorSet(b, 1, 8, \"s\");\n}\n"}, [], 0, []),
    ("samplers, views swapped", {"a.cpp": "void F() {\n b.begin();\n b.addSampler(0, 8);\n b.addTexture(1);\n b.end();\n x = d.CreateDescriptorSet(b, 9, 0, \"s\");\n}\n"}, ["count"], 0, []),
    ("a boundless range", {"a.cpp": "constexpr uint32_t kT = 32768;\nvoid F() {\n b.begin();\n b.addTexture(0, kT);\n b.end(true, kT);\n x = d.CreateDescriptorSet(b, kT, 0, \"s\");\n}\n"}, [], 0, []),
    ("a boundless range, wrong size", {"a.cpp": "constexpr uint32_t kT = 32768;\nvoid F() {\n b.begin();\n b.addTexture(0, kT);\n b.end(true, 100);\n x = d.CreateDescriptorSet(b, kT, 0, \"s\");\n}\n"}, ["count"], 0, []),
    ("a struct member builder", {"a.cpp": "void F(B& b) {\n b.tlas_builder.begin();\n b.tlas_index = b.tlas_builder.addAccelerationStructure(0);\n b.tlas_builder.end();\n b.tlas_set = d.CreateDescriptorSet(b.tlas_builder, 1, 0, \"x\");\n b.tlas_set->setAccelerationStructure(b.tlas_index, t);\n}\n"}, [], 0, []),
    ("check_set_builder_ is not set_builder_", {"a.cpp": "void F() {\n check_set_builder_.begin();\n check_set_builder_.addReadWriteTexture(0);\n check_set_builder_.addReadWriteByteAddressBuffer(1);\n check_set_builder_.end();\n"
                                                 " set_builder_.begin();\n set_builder_.addAccelerationStructure(0);\n set_builder_.addStructuredBuffer(1);\n set_builder_.addByteAddressBuffer(2);\n set_builder_.addByteAddressBuffer(3);\n set_builder_.end();\n"
                                                 " a = d.CreateDescriptorSet(set_builder_, 4, 0, \"a\");\n b = d.CreateDescriptorSet(check_set_builder_, 2, 0, \"b\");\n}\n"}, [], 0, []),
    ("a ternary count over a parameter builder", {"a.cpp": "void F(B& o) {\n o_.begin();\n o_.addReadWriteTexture(0);\n o_.addReadWriteTexture(1);\n o_.end();\n n_.begin();\n n_.addReadWriteTexture(0);\n n_.end();\n"
                                                  " Ens(a, o_, &aux, \"x\");\n Ens(b, n_, nullptr, \"y\");\n}\n" + ENS}, [], 0, []),
    ("a ternary count that misses a builder", {"a.cpp": "void F(B& o) {\n o_.begin();\n o_.addReadWriteTexture(0);\n o_.addReadWriteTexture(1);\n o_.addReadWriteTexture(2);\n o_.end();\n Ens(a, o_, &aux, \"x\");\n}\n" + ENS}, ["count"], 0, []),
    ("a member builder set from a parameter", {"a.cpp": INIT % 2 + INIT_USE}, [], 0, []),
    ("a member builder set from a parameter, wrong count", {"a.cpp": INIT % 3 + INIT_USE}, ["count"], 0, []),
    # the binding code
    ("binding: right", {"a.cpp": BASE + " s_->setTexture(0, t, l, v);\n s_->setBuffer(1, buf, 4);\n}\n"}, [], 0, []),
    ("binding: through the index variables", {"a.cpp": BASE + " s_->setTexture(a_, t, l, v);\n s_->setBuffer(c_, buf, 4);\n}\n"}, [], 0, []),
    ("binding: an index past the set", {"a.cpp": BASE + " s_->setTexture(0, t, l, v);\n s_->setBuffer(2, buf, 4);\n}\n"}, ["index"], 0, ["unwritten"]),
    ("binding: a buffer into a texture range", {"a.cpp": BASE + " s_->setBuffer(0, t, 4);\n s_->setBuffer(1, buf, 4);\n}\n"}, ["type"], 0, []),
    ("binding: a texture into a buffer range", {"a.cpp": BASE + " s_->setTexture(0, t, l, v);\n s_->setTexture(1, t, l, v);\n}\n"}, ["type"], 0, []),
    ("binding: a descriptor never written", {"a.cpp": BASE + " s_->setTexture(0, t, l, v);\n}\n"}, [], 0, ["unwritten"]),
    ("binding: a structured buffer without its view", {"a.cpp": "void F() {\n b_.begin();\n b_.addStructuredBuffer(0);\n b_.end();\n s_ = d->CreateDescriptorSet(b_, 1, 0, \"x\");\n s_->setBuffer(0, buf, 16);\n}\n"}, ["view"], 0, []),
    ("binding: a structured buffer with its view", {"a.cpp": "void F() {\n b_.begin();\n b_.addStructuredBuffer(0);\n b_.end();\n s_ = d->CreateDescriptorSet(b_, 1, 0, \"x\");\n s_->setBuffer(0, buf, 16, &rows);\n}\n"}, [], 0, []),
    ("binding: a loop over the index", {"a.cpp": LOOP3 + " for (size_t i = 0; i < 3; ++i) {\n  s_->setTexture(uint32_t(i), t[i], l, v);\n }\n}\n"}, [], 0, []),
    ("binding: a loop one past", {"a.cpp": LOOP3 + " for (size_t i = 0; i < 4; ++i) {\n  s_->setTexture(uint32_t(i), t[i], l, v);\n }\n}\n"}, ["index"], 0, []),
    ("binding: a register that is not the position", {"a.cpp": "void F() {\n b_.begin();\n b_.addByteAddressBuffer(3);\n b_.end();\n s_ = d->CreateDescriptorSet(b_, 1, 0, \"x\");\n s_->setBuffer(3, buf, 4);\n}\n"}, ["index"], 0, ["unwritten"]),
    ("binding: a helper's struct member in the wrong kind of call", {"h.h": HIT_H, "a.cpp": HIT_CPP + HIT_USE + "void W(plume::RenderDescriptorSet* set, const Hit::Bindings& b) {\n set->setTexture(b.a, t, l, v);\n set->setBuffer(b.b, buf, 8);\n set->setBuffer(b.c, buf, 8, &rows);\n}\n"}, ["type", "view"], 0, []),
    ("a conditional range", {"a.cpp": "void F(bool c) {\n b_.begin();\n b_.addReadWriteTexture(0);\n if (c) {\n  b_.addReadWriteTexture(1);\n }\n b_.end();\n s_ = d->CreateDescriptorSet(b_, 1, 0, \"x\");\n}\n"}, [], 1, []),
    ("an unresolved builder", {"a.cpp": "void F() {\n s_ = d->CreateDescriptorSet(other_, 1, 0, \"x\");\n}\n"}, [], 1, []),
    ("the wrong number of arguments", {"a.cpp": "void F() {\n s_ = d->CreateDescriptorSet(other_, 1, 0);\n}\n"}, ["args"], 0, []),
    ("a bypass", {"a.cpp": "void F() {\n b_.begin();\n b_.addTexture(0);\n b_.end();\n x = dev->createDescriptorSet(desc);\n}\n"}, [], 0, ["bypass"]),
    ("a set made in a test file is not read", {"a_test.cpp": TWO + " s_ = d->CreateDescriptorSet(b_, 1, 0, \"x\");\n}\n"}, [], 0, []),
]


def run_sources(sources):
    import tempfile
    with tempfile.TemporaryDirectory() as d:
        for name, text in sources.items():
            path = os.path.join(d, name)
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "w", encoding="utf-8", newline="\n") as fh:
                fh.write(text)
        return audit_tree(d)


def selftest():
    fails = 0
    for name, sources, errors, unjudged, warns in CASES:
        a = run_sources(sources)
        ek = sorted(k for lv, _, _, k, _ in a.lines if lv == "ERROR")
        uj = sum(1 for lv, *_ in a.lines if lv == "UNJUDGED")
        wk = sorted(k for lv, _, _, k, _ in a.lines if lv == "WARN")
        if ek != sorted(errors) or uj != unjudged or wk != sorted(warns):
            fails += 1
            print("SELFTEST FAIL %s: errors %s (want %s), unjudged %d (want %d), warns %s (want %s)" % (name, ek, sorted(errors), uj, unjudged, wk, sorted(warns)))
            for line in a.report():
                print("   ", line)
    print("selftest: %s (%d cases)" % ("PASS" if fails == 0 else "%d FAILED" % fails, len(CASES)))
    return fails


class GitSource:
    """The sources of a git ref (its src folder) read straight from the object database, nothing is checked out."""

    def __init__(self, repo, ref):
        import subprocess
        self.repo = repo
        tree = ref + ":src"
        probe = subprocess.run(["git", "-C", repo, "rev-parse", "--verify", "-q", tree], capture_output=True, text=True)
        if probe.returncode != 0:
            tree = ref
        out = subprocess.run(["git", "-C", repo, "ls-tree", "-r", tree], capture_output=True, text=True, check=True).stdout
        self.map = {}
        for line in out.splitlines():
            meta, name = line.split("\t", 1)
            self.map[name] = meta.split()[2]
        self.proc = subprocess.Popen(["git", "-C", repo, "cat-file", "--batch"], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
        self.keywords = {}

    def names(self):
        return list(self.map)

    def read(self, name):
        self.proc.stdin.write((self.map[name] + "\n").encode())
        self.proc.stdin.flush()
        size = int(self.proc.stdout.readline().decode().split()[2])
        data = self.proc.stdout.read(size)
        self.proc.stdout.read(1)
        return data.decode("utf-8", "replace")

    def key(self, name):
        return self.map[name]

    def has_keywords(self, name):
        if not name.startswith("gpu/"):
            return False  # the descriptor code is all under src/gpu: nothing else is read for its keywords
        sha = self.map[name]
        if sha not in self.keywords:
            raw = self.read(name)
            self.keywords[sha] = any(k in raw for k in KEYWORD_FILES)
        return self.keywords[sha]

    def close(self):
        self.proc.stdin.close()
        self.proc.wait()


def main(argv):
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(newline="\n")
    show = "--print" in argv
    strict = "--strict" in argv
    refs, repo, args = [], ".", []
    i = 1
    while i < len(argv):
        a = argv[i]
        if a == "--ref" and i + 1 < len(argv):
            refs.append(argv[i + 1])
            i += 1
        elif a == "--repo" and i + 1 < len(argv):
            repo = argv[i + 1]
            i += 1
        elif not a.startswith("--"):
            args.append(a)
        i += 1
    if "--selftest" in argv:
        return 1 if selftest() else 0
    if refs:
        worst = 0
        cache = {}
        for ref in refs:
            src = GitSource(repo, ref)
            a = audit_tree("", src, cache)
            src.close()
            print("== %s" % ref)
            for line in a.report(show):
                print(line)
            if a.stats["errors"] or (strict and a.stats["unjudged"]):
                worst = 1
        return worst
    if len(args) != 1 or not os.path.isdir(args[0]):
        print(__doc__)
        return 2
    a = audit_tree(args[0])
    for line in a.report(show):
        print(line)
    if a.stats["errors"]:
        return 1
    return 1 if strict and a.stats["unjudged"] else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
