"""Validator of the four launcher-follows-the-game contract files (LAUNCHER-FOLLOWS-GAME.md): settings-ui.json, machine.json, install-manifest.json,
launcher-recipe.json (and the small --describe document). It checks a JSON against its schema in schemas/ and then the rules a schema cannot express
(section 3, "Writers"): an expression parses, only declared namespaces and functions, every fact path is in fact-dictionary.json, a setting a row names exists,
a value id is valid for its setting, `apply` agrees with `life`, `live.*` has a `??` fallback, presets cover what they should, facts_used is what the rules read.

    python -I documentation/contract/validate_contract.py [--kind K] [--strict] [--quiet] FILE...     check files (kind from their "schema" word)
    python -I documentation/contract/validate_contract.py --selftest                                   the validator's own tests (examples must pass, mutations must fail)
    python -I documentation/contract/validate_contract.py --facts-used FILE                           print the facts_used list of a settings-ui.json

Output: one line per finding, `ERROR|WARN|INFO <file>: <where>: <what>`; exit 0 when there is no ERROR (with --strict, no WARN either).
As a module: validate(doc, kind=None, dictionary=None, strict=False, manifest=None) -> Report(errors, warnings, infos). Pure Python 3, no packages (the JSON Schema
subset it needs is built in: see SUPPORTED_KEYWORDS; the selftest fails if a schema uses another keyword).
"""
import argparse
import copy
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.dont_write_bytecode = True

KINDS = {
    'larecomp.settings-ui': 'settings-ui',
    'larecomp.machine': 'machine',
    'larecomp.install-manifest': 'install-manifest',
    'larecomp.launcher-recipe': 'launcher-recipe',
    'larecomp.describe': 'describe',
}
SCHEMA_FILES = {
    'settings-ui': 'settings-ui.schema.json',
    'machine': 'machine.schema.json',
    'install-manifest': 'install-manifest.schema.json',
    'launcher-recipe': 'launcher-recipe.schema.json',
    'describe': 'describe.schema.json',
}
FACT_NAMESPACES = ('gpu', 'api', 'features', 'os', 'display', 'cpu', 'build', 'install', 'live')
FUNCTIONS = {'resolved': 1, 'auto_of': 1, 'known': 1, 'contains': 2, 'min': 2, 'max': 2}
KNOWN_KINDS = ('choice', 'bool', 'number', 'text', 'color', 'action', 'link', 'carbon')
SUPPORTED_KEYWORDS = {
    '$schema', '$id', '$ref', '$defs', 'title', 'description', 'type', 'properties', 'required', 'additionalProperties', 'items', 'enum', 'const',
    'minimum', 'maximum', 'minItems', 'maxItems', 'uniqueItems', 'minLength', 'pattern', 'minProperties', 'oneOf', 'allOf', 'not', 'default', 'examples',
}


class Report:
    def __init__(self):
        self.errors, self.warnings, self.infos = [], [], []

    def error(self, where, msg):
        self.errors.append(f'{where}: {msg}')

    def warn(self, where, msg):
        self.warnings.append(f'{where}: {msg}')

    def info(self, where, msg):
        self.infos.append(f'{where}: {msg}')


# ---- a JSON Schema validator for the subset the contract's schemas use ---------------------------------------------------------------------

def _type_ok(value, t):
    if t == 'null':
        return value is None
    if t == 'boolean':
        return isinstance(value, bool)
    if t == 'integer':
        return isinstance(value, int) and not isinstance(value, bool) or (isinstance(value, float) and value == int(value))
    if t == 'number':
        return isinstance(value, (int, float)) and not isinstance(value, bool)
    if t == 'string':
        return isinstance(value, str)
    if t == 'array':
        return isinstance(value, list)
    if t == 'object':
        return isinstance(value, dict)
    return False


def schema_errors(value, schema, root, path='$'):
    """Yield (path, message) for every way `value` does not fit `schema` (JSON Schema draft 2020-12, the subset of SUPPORTED_KEYWORDS)."""
    if '$ref' in schema:
        ref = schema['$ref']
        assert ref.startswith('#/'), 'only local refs'
        target = root
        for part in ref[2:].split('/'):
            target = target[part]
        yield from schema_errors(value, target, root, path)
    if 'type' in schema:
        types = schema['type'] if isinstance(schema['type'], list) else [schema['type']]
        if not any(_type_ok(value, t) for t in types):
            yield path, f'is {type(value).__name__}, expected {"/".join(types)}'
            return
    if 'const' in schema and value != schema['const']:
        yield path, f'must be {schema["const"]!r}'
    if 'enum' in schema and value not in schema['enum']:
        yield path, f'{value!r} is not one of {schema["enum"]}'
    if isinstance(value, (int, float)) and not isinstance(value, bool):
        if 'minimum' in schema and value < schema['minimum']:
            yield path, f'{value} is below {schema["minimum"]}'
        if 'maximum' in schema and value > schema['maximum']:
            yield path, f'{value} is above {schema["maximum"]}'
    if isinstance(value, str):
        if 'minLength' in schema and len(value) < schema['minLength']:
            yield path, 'is empty' if schema['minLength'] == 1 else f'is shorter than {schema["minLength"]}'
        if 'pattern' in schema and not re.search(schema['pattern'], value):
            yield path, f'{value!r} does not match {schema["pattern"]}'
    if isinstance(value, list):
        if 'minItems' in schema and len(value) < schema['minItems']:
            yield path, f'has {len(value)} items, needs {schema["minItems"]}'
        if 'maxItems' in schema and len(value) > schema['maxItems']:
            yield path, f'has {len(value)} items, at most {schema["maxItems"]}'
        if schema.get('uniqueItems'):
            seen = []
            for v in value:
                if v in seen:
                    yield path, f'{v!r} is listed twice'
                seen.append(v)
        if 'items' in schema:
            for i, v in enumerate(value):
                yield from schema_errors(v, schema['items'], root, f'{path}[{i}]')
    if isinstance(value, dict):
        for r in schema.get('required', []):
            if r not in value:
                yield path, f'is missing "{r}"'
        if 'minProperties' in schema and len(value) < schema['minProperties']:
            yield path, f'has {len(value)} properties, needs {schema["minProperties"]}'
        props = schema.get('properties', {})
        for k, v in value.items():
            if k in props:
                yield from schema_errors(v, props[k], root, f'{path}.{k}')
            elif isinstance(schema.get('additionalProperties'), dict):
                yield from schema_errors(v, schema['additionalProperties'], root, f'{path}.{k}')
            elif schema.get('additionalProperties') is False:
                yield f'{path}.{k}', 'is not a known property'
    if 'allOf' in schema:
        for sub in schema['allOf']:
            yield from schema_errors(value, sub, root, path)
    if 'oneOf' in schema:
        matches = sum(1 for sub in schema['oneOf'] if not list(schema_errors(value, sub, root, path)))
        if matches != 1:
            yield path, f'must fit exactly one of {len(schema["oneOf"])} shapes, fits {matches}'
    if 'not' in schema and not list(schema_errors(value, schema['not'], root, path)):
        yield path, 'has something it must not have'


def schema_keywords(schema, found=None):
    """Every keyword the schema document uses (for the check that only SUPPORTED_KEYWORDS appear)."""
    found = set() if found is None else found
    if isinstance(schema, dict):
        for k, v in schema.items():
            if k in ('properties', '$defs'):
                for sub in v.values():
                    schema_keywords(sub, found)
                continue
            found.add(k)
            if k in ('items', 'additionalProperties', 'not') and isinstance(v, dict):
                schema_keywords(v, found)
            if k in ('oneOf', 'allOf'):
                for sub in v:
                    schema_keywords(sub, found)
    return found


# ---- the condition language, parsed (the same grammar as cpp/ui_expr_ref.h and Core\Contract\UiExpr.cs) --------------------------------------

class Expr:
    def __init__(self):
        self.ok = False
        self.error = ''
        self.unsupported_call = False
        self.root = None
        self.paths = []
        self.unguarded_live = []


_TWO = ('||', '&&', '==', '!=', '<=', '>=', '??')


def _tokenize(s):
    toks, i = [], 0
    ident_start = lambda c: c.isascii() and (c.isalpha() or c == '_')
    ident_char = lambda c: ident_start(c) or ('0' <= c <= '9')
    while True:
        while i < len(s) and s[i] in ' \t\r\n':
            i += 1
        if i >= len(s):
            toks.append(('end', None, i))
            return toks
        c, pos = s[i], i
        if c in '\'"':
            q, i, text, closed = c, i + 1, [], False
            while i < len(s):
                d = s[i]
                i += 1
                if d == q:
                    closed = True
                    break
                if d == '\\':
                    if i >= len(s):
                        raise ValueError(f'unterminated text at column {pos + 1}')
                    e = s[i]
                    i += 1
                    if e in '\\\'"':
                        text.append(e)
                    else:
                        raise ValueError(f'unknown escape at column {i - 1}')
                else:
                    text.append(d)
            if not closed:
                raise ValueError(f'unterminated text at column {pos + 1}')
            toks.append(('str', ''.join(text), pos))
            continue
        if '0' <= c <= '9' or (c == '-' and i + 1 < len(s) and '0' <= s[i + 1] <= '9'):
            start = i
            if c == '-':
                i += 1
            while i < len(s) and '0' <= s[i] <= '9':
                i += 1
            if i + 1 < len(s) and s[i] == '.' and '0' <= s[i + 1] <= '9':
                i += 1
                while i < len(s) and '0' <= s[i] <= '9':
                    i += 1
            if i < len(s) and s[i] in 'eE':
                save = i
                i += 1
                if i < len(s) and s[i] in '+-':
                    i += 1
                if i < len(s) and '0' <= s[i] <= '9':
                    while i < len(s) and '0' <= s[i] <= '9':
                        i += 1
                else:
                    i = save
            if i < len(s) and ident_char(s[i]):
                raise ValueError(f'a number runs into a word at column {start + 1}')
            toks.append(('num', float(s[start:i]), pos))
            continue
        if ident_start(c):
            segs = []
            while True:
                start = i
                while i < len(s) and ident_char(s[i]):
                    i += 1
                segs.append(s[start:i])
                if i + 1 < len(s) and s[i] == '.' and ident_start(s[i + 1]):
                    i += 1
                    continue
                break
            toks.append(('ident', segs, pos))
            continue
        if s[i:i + 2] in _TWO:
            toks.append(('p', s[i:i + 2], pos))
            i += 2
            continue
        if c in '!<>()[],':
            toks.append(('p', c, pos))
            i += 1
            continue
        raise ValueError(f"unexpected character '{c}' at column {i + 1}")


class _Parser:
    def __init__(self, toks):
        self.t, self.i, self.unsupported = toks, 0, False

    def peek(self):
        return self.t[self.i]

    def take(self):
        tok = self.t[self.i]
        if self.i < len(self.t) - 1:
            self.i += 1
        return tok

    def is_p(self, p):
        k = self.peek()
        return k[0] == 'p' and k[1] == p

    def is_word(self, w):
        k = self.peek()
        return k[0] == 'ident' and len(k[1]) == 1 and k[1][0] == w

    def fail(self, what):
        raise ValueError(f'{what} at column {self.peek()[2] + 1}')

    def run(self):
        n = self.or_()
        if self.peek()[0] != 'end':
            self.fail('unexpected text after the expression')
        return n

    def or_(self):
        left = self.and_()
        while self.is_p('||'):
            self.take()
            left = ('or', left, self.and_())
        return left

    def and_(self):
        left = self.not_()
        while self.is_p('&&'):
            self.take()
            left = ('and', left, self.not_())
        return left

    def not_(self):
        if self.is_p('!'):
            self.take()
            return ('not', self.not_())
        return self.cmp_()

    def literal(self):
        k = self.peek()
        if k[0] == 'str':
            self.take()
            return ('lit', 'string', k[1])
        if k[0] == 'num':
            self.take()
            return ('lit', 'number', k[1])
        for word, val in (('true', True), ('false', False), ('null', None)):
            if self.is_word(word):
                self.take()
                return ('lit', 'bool' if val is not None else 'null', val)
        self.fail('expected a text, number, true, false or null')

    def cmp_(self):
        left = self.coalesce()
        k = self.peek()
        if k[0] == 'p' and k[1] in ('==', '!=', '<', '<=', '>', '>='):
            self.take()
            return ('cmp', k[1], left, self.coalesce())
        if self.is_word('in'):
            self.take()
            if not self.is_p('['):
                self.fail("expected '[' after in")
            self.take()
            items = [self.literal()]
            while self.is_p(','):
                self.take()
                items.append(self.literal())
            if not self.is_p(']'):
                self.fail("expected ',' or ']'")
            self.take()
            return ('in', left, items)
        return left

    def coalesce(self):
        left = self.primary()
        while self.is_p('??'):
            self.take()
            left = ('coalesce', left, self.primary())
        return left

    def primary(self):
        k = self.peek()
        if k[0] in ('str', 'num') or self.is_word('true') or self.is_word('false') or self.is_word('null'):
            return self.literal()
        if self.is_p('('):
            self.take()
            inner = self.or_()
            if not self.is_p(')'):
                self.fail("expected ')'")
            self.take()
            return inner
        if k[0] == 'ident':
            nxt = self.t[self.i + 1]
            if nxt[0] == 'p' and nxt[1] == '(':
                if len(k[1]) > 1:
                    self.fail('a function name has no dots')
                name = k[1][0]
                self.take()
                self.take()
                args = []
                if not self.is_p(')'):
                    args.append(self.or_())
                    while self.is_p(','):
                        self.take()
                        args.append(self.or_())
                if not self.is_p(')'):
                    self.fail("expected ',' or ')'")
                self.take()
                if FUNCTIONS.get(name) != len(args):
                    self.unsupported = True
                return ('call', name, args)
            if len(k[1]) == 1 and k[1][0] == 'in':
                self.fail('unexpected in')
            self.take()
            return ('path', k[1])
        self.fail('expected a value')


def parse_expr(text):
    e = Expr()
    try:
        toks = _tokenize(text)
        p = _Parser(toks)
        e.root = p.run()
        e.unsupported_call = p.unsupported
    except ValueError as ex:
        e.error = str(ex)
        return e
    e.ok = True
    paths, live = set(), set()

    def walk(n, guarded):
        if n[0] == 'path':
            dotted = '.'.join(n[1])
            paths.add(dotted)
            if n[1][0] == 'live' and not guarded:
                live.add(dotted)
        elif n[0] in ('and', 'or'):
            walk(n[1], guarded)
            walk(n[2], guarded)
        elif n[0] == 'not':
            walk(n[1], guarded)
        elif n[0] == 'cmp':
            walk(n[2], guarded)
            walk(n[3], guarded)
        elif n[0] == 'in':
            walk(n[1], guarded)
        elif n[0] == 'coalesce':
            walk(n[1], True)
            walk(n[2], guarded)
        elif n[0] == 'call':
            for a in n[2]:
                walk(a, guarded)

    walk(e.root, False)
    e.paths, e.unguarded_live = sorted(paths), sorted(live)
    return e


# ---- Title Case (the same rule as Core\Contract\TitleCase.cs; vectors in ui-text-case.json) ------------------------------------------------

_WORD = re.compile(r"[A-Za-z0-9]+(?:['./][A-Za-z0-9]+)*")


class TitleRule:
    def __init__(self, doc):
        self.acronyms = {a.upper() for a in doc['acronyms']}
        self.spellings = {k.upper(): v for k, v in doc['spellings'].items()}
        self.small = {w.lower() for w in doc['small_words']}
        self.plain = {w.upper() for w in doc.get('plain_words', [])}

    def word(self, w, allow_small):
        key = w.upper()
        if key in self.spellings:
            return self.spellings[key]
        if key in self.acronyms:
            return key
        if '/' in w:
            return '/'.join(self.word(p, False) for p in w.split('/'))
        if any(c.isdigit() for c in key):
            m = re.fullmatch(r'(\d+(?:\.\d+)?)X', key)
            if m:
                return m.group(1) + 'x'
            m = re.fullmatch(r'(\d+)X(\d+)', key)
            if m:
                return f'{m.group(1)}x{m.group(2)}'
            m = re.fullmatch(r'(\d+)(P|M|MS|HZ)', key)
            if m:
                return m.group(1) + {'P': 'p', 'M': 'm', 'MS': 'ms', 'HZ': 'Hz'}[m.group(2)]
            return key
        low = w.lower()
        if allow_small and low in self.small:
            return low
        return low[:1].upper() + low[1:]

    def convert(self, label):
        words = list(_WORD.finditer(label))
        out, prev_end = [], 0
        for i, m in enumerate(words):
            sep = label[prev_end:m.start()]
            out.append(sep)
            allow = not (i == 0 or i == len(words) - 1 or ':' in sep or '(' in sep)
            out.append(self.word(m.group(), allow))
            prev_end = m.end()
        out.append(label[prev_end:])
        return ''.join(out)

    def guesses(self, label):
        """Short all-capital words that are neither an acronym, a spelling, a small word nor a known plain word: the ones a convert() had to guess."""
        found = []
        for m in _WORD.finditer(label):
            w = m.group()
            if 2 <= len(w) <= 4 and w.isalpha() and w.isupper():
                key = w.upper()
                if key not in self.acronyms and key not in self.spellings and w.lower() not in self.small and key not in self.plain:
                    found.append(w)
        return found


# ---- loading -----------------------------------------------------------------------------------------------------------------------------

def load_json(path):
    with open(path, encoding='utf-8-sig') as f:
        return json.load(f)


_cache = {}


def dictionary(path=None):
    path = path or os.path.join(HERE, 'fact-dictionary.json')
    if path not in _cache:
        _cache[path] = load_json(path)
    return _cache[path]


def schema_of(kind):
    path = os.path.join(HERE, 'schemas', SCHEMA_FILES[kind])
    if path not in _cache:
        _cache[path] = load_json(path)
    return _cache[path]


def title_rule():
    path = os.path.join(HERE, 'ui-text-case.json')
    if path not in _cache:
        _cache[path] = TitleRule(load_json(path))
    return _cache[path]


# ---- expression checks (lint 2 and 3) ---------------------------------------------------------------------------------------------------

class FactTypes:
    def __init__(self, dct):
        self.facts = dct['facts']
        self.open = tuple(dct.get('open_prefixes', []))
        self.not_readable = tuple(dct.get('not_readable', []))
        self.groups = set()
        for p in self.facts:
            parts = p.split('.')
            for i in range(1, len(parts)):
                self.groups.add('.'.join(parts[:i]))

    def lookup(self, dotted):
        """('fact', entry) | ('open', None) | ('group', None) | ('unreadable', None) | ('missing', None)."""
        for nr in self.not_readable:
            if dotted == nr or dotted.startswith(nr + '.'):
                return 'unreadable', None
        if dotted in self.facts:
            return 'fact', self.facts[dotted]
        for pre in self.open:
            if dotted.startswith(pre):
                return 'open', None
        if dotted in self.groups:
            return 'group', None
        return 'missing', None


_KIND_OF = {'bool': 'bool', 'int': 'number', 'number': 'number', 'string': 'string', 'path': 'string', 'list': 'list'}


class ExprChecker:
    """Checks the expressions of one file: parse, functions, namespaces, fact paths, the static types, live fallbacks. Collects the fact paths for facts_used."""

    def __init__(self, report, facts, settings=None, defs=None, allow_live=True, allow_settings=True, l0_only=False, extra_install=()):
        self.r, self.facts = report, facts
        self.settings = settings if settings is not None else {}
        self.defs = defs if defs is not None else {}
        self.allow_live, self.allow_settings, self.l0_only = allow_live, allow_settings, l0_only
        self.extra_install = set(extra_install)
        self.fact_paths = set()
        self.def_refs = {}   # def name -> set of def names it reads (for the cycle check)

    # type of an AST node: 'bool' | 'number' | 'string' | 'list' | 'null' | None (unknown)
    def type_of(self, n):
        k = n[0]
        if k == 'lit':
            return n[1]
        if k == 'path':
            segs = n[1]
            if segs[0] == 's' and len(segs) == 2 and segs[1] in self.settings:
                return _KIND_OF.get(self.settings[segs[1]].get('type'))
            if segs[0] == 'renderer':
                return 'string'
            if segs[0] in FACT_NAMESPACES:
                kind, entry = self.facts.lookup('.'.join(segs))
                if kind == 'fact':
                    return _KIND_OF.get(entry['type'])
            return None
        if k in ('and', 'or', 'not', 'cmp', 'in'):
            return 'bool'
        if k == 'call':
            name = n[1]
            if name in ('known', 'contains'):
                return 'bool'
            if name in ('min', 'max'):
                return 'number'
            if name == 'resolved' and n[2] and n[2][0][0] == 'path':
                return self.type_of(n[2][0])
            return None
        if k == 'coalesce':
            a, b = self.type_of(n[1]), self.type_of(n[2])
            return a if a == b else None
        return None

    def check(self, text, where, bool_needed=True, live_only=False, rule_name='condition'):
        e = parse_expr(text)
        if not e.ok:
            self.r.error(where, f'{rule_name} does not parse: {e.error}: {text!r}')
            return e
        if e.unsupported_call:
            self.r.error(where, f'{rule_name} calls a function outside the v1 set (or with the wrong number of arguments): {text!r}')
        for p in e.paths:
            segs = p.split('.')
            first = segs[0]
            if first == 's':
                if not self.allow_settings:
                    self.r.error(where, f'{p}: settings are not readable here')
                elif len(segs) != 2 or segs[1] not in self.settings:
                    self.r.error(where, f'{p}: no such setting in "settings"')
            elif first == 'd':
                if not self.allow_settings:
                    self.r.error(where, f'{p}: defs are not readable here')
                elif len(segs) != 2 or segs[1] not in self.defs:
                    self.r.error(where, f'{p}: no such def')
            elif first == 'renderer':
                if len(segs) != 1 or not self.allow_settings:
                    self.r.error(where, f'{p}: not a path')
            elif first in FACT_NAMESPACES:
                if first == 'live' and not self.allow_live:
                    self.r.error(where, f'{p}: live facts are not readable here')
                    continue
                kind, entry = self.facts.lookup(p)
                if kind == 'unreadable':
                    self.r.error(where, f'{p}: not readable by a rule')
                elif kind == 'group':
                    self.r.error(where, f'{p}: names a group of facts, not a fact')
                elif kind == 'missing':
                    self.r.error(where, f'{p}: not in the fact dictionary (a typo is an error, not a null)')
                elif kind == 'open' and p not in self.extra_install and self.extra_install:
                    self.r.warn(where, f'{p}: no install-manifest fact of that name was given')
                elif kind == 'fact' and self.l0_only and not entry.get('l0'):
                    self.r.error(where, f'{p}: the launcher does not have this fact before the first probe (only l0 facts here)')
                if kind in ('fact', 'open'):
                    self.fact_paths.add(p)
            else:
                self.r.error(where, f'{p}: unknown namespace "{first}" (s, d, renderer, {", ".join(FACT_NAMESPACES)})')
        if e.unguarded_live and not live_only:
            self.r.error(where, f'{", ".join(e.unguarded_live)}: a live fact needs a ?? fallback the launcher can evaluate, or "live_only": true on the rule (lint 3)')
        self.type_walk(e.root, where, bool_needed)
        self.def_refs_of = [p.split('.')[1] for p in e.paths if p.startswith('d.') and len(p.split('.')) == 2]
        return e

    def type_walk(self, n, where, bool_needed):
        k = n[0]
        if bool_needed:
            t = self.type_of(n)
            if t not in (None, 'bool'):
                self.r.error(where, f'the result is a {t}, a bool is needed (a non-boolean result counts as null)')
        if k in ('and', 'or'):
            for sub in (n[1], n[2]):
                self.operand_bool(sub, where, k)
                self.type_walk(sub, where, False)
        elif k == 'not':
            self.operand_bool(n[1], where, '!')
            self.type_walk(n[1], where, False)
        elif k == 'cmp':
            a, b = self.type_of(n[2]), self.type_of(n[3])
            if a and b and a != b and 'null' not in (a, b):
                self.r.error(where, f'{n[1]} compares a {a} with a {b}: always null')
            if n[1] in ('<', '<=', '>', '>=') and (a not in (None, 'number') or b not in (None, 'number')):
                self.r.error(where, f'{n[1]} needs two numbers')
            self.type_walk(n[2], where, False)
            self.type_walk(n[3], where, False)
        elif k == 'in':
            a = self.type_of(n[1])
            for lit in n[2]:
                if a and lit[1] != a and lit[1] != 'null':
                    self.r.error(where, f'in [...] lists a {lit[1]} where the left side is a {a}: that term is always null')
            self.type_walk(n[1], where, False)
        elif k == 'coalesce':
            self.type_walk(n[1], where, False)
            self.type_walk(n[2], where, False)
        elif k == 'call':
            name, args = n[1], n[2]
            if name == 'resolved' and len(args) == 1:
                a = args[0]
                if not (a[0] == 'path' and len(a[1]) == 2 and a[1][0] == 's'):
                    self.r.error(where, 'resolved() takes exactly a setting path: resolved(s.name)')
            if name == 'auto_of' and len(args) == 1:
                a = args[0]
                if a[0] == 'lit' and a[1] == 'string':
                    if a[2] not in self.settings:
                        self.r.error(where, f"auto_of('{a[2]}'): no such setting")
                    elif not self.settings[a[2]].get('auto'):
                        self.r.warn(where, f"auto_of('{a[2]}'): the setting has no auto descriptor (always null)")
                elif self.type_of(a) not in (None, 'string'):
                    self.r.error(where, 'auto_of() takes the name of a setting as text')
            if name == 'contains' and len(args) == 2:
                lt = self.type_of(args[0])
                if lt not in (None, 'list'):
                    self.r.error(where, 'contains() needs a list as its first argument')
            if name in ('min', 'max'):
                for a in args:
                    if self.type_of(a) not in (None, 'number'):
                        self.r.error(where, f'{name}() needs numbers')
            for a in args:
                self.type_walk(a, where, False)

    def operand_bool(self, n, where, op):
        t = self.type_of(n)
        if t not in (None, 'bool'):
            self.r.error(where, f'an operand of {op} is a {t}: a non-boolean counts as null')


# ---- settings-ui.json -------------------------------------------------------------------------------------------------------------------

_NUM = re.compile(r'^-?\d+(\.\d+)?([eE][+-]?\d+)?$')


def value_ok_for_setting(text, setting):
    """None when `text` is a valid file value of the setting, else the reason."""
    t = setting.get('type')
    if t == 'bool':
        return None if text in ('true', 'false') else 'a bool setting holds true or false'
    if t in ('int', 'number'):
        if not _NUM.match(text):
            return 'a number setting holds a number'
        n = float(text)
        if t == 'int' and n != int(n):
            return 'an int setting holds a whole number'
        rng = setting.get('range')
        if rng and not (rng[0] <= n <= rng[1]):
            return f'outside the setting\'s range {rng}'
        return None
    allowed = setting.get('allowed')
    if allowed is not None and text not in allowed:
        return f'not one of the setting\'s allowed words {allowed}'
    return None


def effective_values(row, doc):
    """The value list a row (or variant) offers: values, else the value set it points to, else values_from's fallback_values."""
    if 'values' in row:
        return row['values']
    if 'values_ref' in row:
        return doc.get('value_sets', {}).get(row['values_ref'])
    if 'fallback_values' in row:
        return row['fallback_values']
    return None


def lint_settings_ui(doc, rep, facts, strict=False):
    settings = doc.get('settings', {})
    defs = doc.get('defs', {})
    value_sets = doc.get('value_sets', {})
    rule = title_rule()
    ec = ExprChecker(rep, facts, settings, defs)
    all_fact_paths = set()

    # -- settings catalog
    for name, s in settings.items():
        w = f'settings.{name}'
        if not re.fullmatch(r'[a-z][a-z0-9_]*', name):
            rep.error(w, 'a flat name is lower case words with underscores')
        reason = value_ok_for_setting(s['default'], s) if isinstance(s.get('default'), str) else None
        if reason:
            rep.error(w, f'the default {s["default"]!r} is invalid: {reason}')
        au = s.get('auto')
        if au:
            by, cells = au.get('by'), au.get('cells', {})
            keys = {'class': {'low', 'mid', 'high'}, 'rt_rung': {'none', 'weak', 'strong'}}.get(by)
            if keys is not None:
                if set(cells) != keys:
                    rep.error(w, f'auto by {by} needs cells for exactly {sorted(keys)}, has {sorted(cells)}')
                for k, v in list(cells.items()) + list(au.get('handheld', {}).items()):
                    if 'auto_value' in au:
                        if not isinstance(v, (int, float)):
                            rep.error(w, f'auto cell {k}: a numeric auto row has numeric cells')
                    elif isinstance(v, str):
                        r2 = value_ok_for_setting(v, s)
                        if r2:
                            rep.error(w, f'auto cell {k}={v!r}: {r2}')
                if by != 'class' and au.get('handheld'):
                    rep.error(w, 'handheld cells only exist for auto by class')
            if by == 'preset' and not doc.get('presets'):
                rep.error(w, 'auto by preset needs a presets block')
            if 'auto_value' in au and s.get('type') not in ('int', 'number'):
                rep.error(w, 'auto_value is for numeric settings')
            if 'auto_value' not in au and s.get('type') == 'string' and 'auto' not in (s.get('allowed') or ['auto']):
                rep.error(w, 'a setting with an auto descriptor must allow the word auto')
        if s.get('type') in ('int', 'number') and s.get('range') and not (s['range'][0] <= s['range'][1]):
            rep.error(w, 'range is [min, max]')

    # -- defs
    for name, d in defs.items():
        w = f'defs.{name}'
        refs = set()
        if isinstance(d, str):
            e = ec.check(d, w, bool_needed=False)
            refs |= {p.split('.')[1] for p in e.paths if p.startswith('d.') and len(p.split('.')) == 2}
        else:
            if not d.get('cases'):
                rep.warn(w, 'a cases def with no case is always null')
            for i, c in enumerate(d.get('cases', [])):
                cw = f'{w}.cases[{i}]'
                lo = bool(c.get('live_only'))
                if 'when' in c:
                    e = ec.check(c['when'], cw, live_only=lo)
                    refs |= {p.split('.')[1] for p in e.paths if p.startswith('d.') and len(p.split('.')) == 2}
                elif i != len(d['cases']) - 1:
                    rep.error(cw, 'a case without when is the default: it must be the last')
                if isinstance(c['value'], dict):
                    e = ec.check(c['value']['expr'], cw + '.value.expr', bool_needed=False, live_only=lo)
                    refs |= {p.split('.')[1] for p in e.paths if p.startswith('d.') and len(p.split('.')) == 2}
        ec.def_refs[name] = refs
    # def cycles
    state = {}

    def visit(n, stack):
        if state.get(n) == 2:
            return
        if state.get(n) == 1:
            rep.error(f'defs.{n}', 'defs read each other in a loop: ' + ' -> '.join(stack + [n]))
            return
        state[n] = 1
        for m in ec.def_refs.get(n, ()):
            if m in defs:
                visit(m, stack + [n])
        state[n] = 2

    for n in defs:
        visit(n, [])

    # -- tabs, sections, rows
    tab_ids, section_ids, row_ids = set(), {}, set()
    row_section = {}   # row id -> section id
    referenced_settings = set()
    sections_by_id = {}
    for ti, tab in enumerate(doc.get('tabs', [])):
        tw = f'tabs[{ti}:{tab.get("id")}]'
        if tab['id'] in tab_ids:
            rep.error(tw, 'tab id is used twice')
        tab_ids.add(tab['id'])
        for field in ('hidden_unless',):
            if field in tab:
                ec.check(tab[field], f'{tw}.{field}')
        for si, sec in enumerate(tab.get('sections', [])):
            sw = f'{tw}.sections[{si}:{sec.get("id")}]'
            if sec['id'] in section_ids:
                rep.error(sw, 'section id is used twice (ids are unique across the file)')
            section_ids[sec['id']] = sw
            sections_by_id[sec['id']] = sec
            if 'hidden_unless' in sec:
                ec.check(sec['hidden_unless'], f'{sw}.hidden_unless')
            for obj, ow in ((tab, tw), (sec, sw)):
                if 'title' in obj and 'label' in obj:
                    t2 = rule.convert(obj['label'])
                    if t2 != obj['title']:
                        rep.info(ow, f'title {obj["title"]!r} differs from the rule\'s {t2!r} (an override)')
            if not sec.get('debug') and not tab.get('debug') and not sec.get('rows'):
                rep.warn(sw, 'a section with no rows')
    for ti, tab in enumerate(doc.get('tabs', [])):
        for si, sec in enumerate(tab.get('sections', [])):
            for ri, row in enumerate(sec.get('rows', [])):
                rw = f'row {row.get("id")}'
                if row['id'] in row_ids:
                    rep.error(rw, 'row id is used twice')
                row_ids.add(row['id'])
                row_section[row['id']] = sec['id']
                lint_row(doc, rep, ec, row, rw, sec, tab, section_ids, referenced_settings, rule, settings, value_sets, strict)
                all_fact_paths |= set()

    # -- presets
    if 'presets' in doc:
        lint_presets(doc, rep, ec, row_section, sections_by_id, settings, value_sets)

    # -- facts_used
    used = sorted(ec.fact_paths)
    given = doc.get('facts_used')
    if given is None:
        rep.warn('facts_used', f'missing; the rules read {len(used)} facts')
    elif list(given) != used:
        missing = sorted(set(used) - set(given))
        extra = sorted(set(given) - set(used))
        rep.error('facts_used', f'is not what the rules read (sorted, no duplicates): missing {missing}, not read {extra}')
    return used


def merged(row, variant):
    m = {k: v for k, v in row.items() if k not in ('variants', 'id')}
    m.update({k: v for k, v in variant.items() if k != 'when'})
    return m


def lint_row(doc, rep, ec, row, rw, sec, tab, section_ids, referenced_settings, rule, settings, value_sets, strict):
    debug = bool(row.get('debug') or sec.get('debug') or tab.get('debug'))
    kind = row.get('kind')
    surfaces = row.get('surfaces', ['game', 'launcher'])
    if kind not in KNOWN_KINDS and 'fallback_kind' not in row and 'values' not in row:
        rep.warn(rw, f'kind {kind!r} is new: say how an older reader draws it ("fallback_kind"), or give it values')
    if surfaces == ['launcher'] and not row['id'].startswith('L_'):
        rep.error(rw, 'a launcher-only row has an id of the form L_<setting>')
    if 'native' in row:
        rep.info(rw, f'dynamics still native ({row["native"]}): the launcher draws it plain')
    if 'title' in row:
        t2 = rule.convert(row['label'])
        if t2 != row['title']:
            rep.info(rw, f'title {row["title"]!r} differs from the rule\'s {t2!r} (an override)')
        g = rule.guesses(row['label'])
        if g:
            rep.info(rw, f'the Title Case rule had to guess the word(s) {g}: add them to acronyms or plain_words of ui-text-case.json')
    if row.get('hidden_unless'):
        ec.check(row['hidden_unless'], f'{rw}.hidden_unless', live_only=bool(row.get('live_only')))
    if kind == 'link':
        if row.get('target') not in section_ids:
            rep.error(rw, f'a link row names the section it opens ("target"): {row.get("target")!r} is not a section id')
    # help
    needs_help = (not debug) and kind not in ('link', 'action', 'carbon') and 'launcher' in surfaces
    if needs_help and not row.get('help'):
        rep.error(rw, 'a non-debug row has help (the player text)')
    # variants are checked as rows of their own
    forms = [(rw, row)]
    for vi, v in enumerate(row.get('variants', [])):
        vw = f'{rw}.variants[{vi}]'
        ec.check(v['when'], f'{vw}.when', live_only=bool(v.get('live_only') or row.get('live_only')))
        forms.append((vw, merged(row, v)))
        if 'title' in v and 'label' in v:
            t2 = rule.convert(v['label'])
            if t2 != v['title']:
                rep.info(vw, f'title {v["title"]!r} differs from the rule\'s {t2!r} (an override)')
        for si2, st in enumerate(v.get('states', [])):
            lint_state(ec, rep, st, f'{vw}.states[{si2}]', bool(row.get('live_only') or v.get('live_only')))
    for si2, st in enumerate(row.get('states', [])):
        lint_state(ec, rep, st, f'{rw}.states[{si2}]', bool(row.get('live_only')))
    for fw, f in forms:
        lint_row_form(doc, rep, ec, f, fw, row, settings, value_sets, referenced_settings, surfaces, debug)


def lint_state(ec, rep, st, w, row_live_only):
    lo = bool(st.get('live_only') or row_live_only)
    ec.check(st['when'], f'{w}.when', live_only=lo)
    suffix = st.get('suffix', '')
    if 'RESTART' in suffix.upper() and not st.get('needs_restart'):
        rep.error(w, f'the suffix {suffix!r} says RESTART: the state needs "needs_restart": true (the "Restart now?" prompt)')
    if st.get('needs_restart') and 'RESTART' not in suffix.upper() and not st.get('tag'):
        rep.warn(w, 'needs_restart with no RESTART suffix and no tag: the player is not told why')
    if st.get('enabled') is False and not (st.get('reason') or st.get('tag') or suffix):
        rep.warn(w, 'a state that greys the row says why (reason, tag or suffix)')


def lint_row_form(doc, rep, ec, f, fw, row, settings, value_sets, referenced_settings, surfaces, debug):
    kind = f.get('kind')
    setting_name = f.get('setting')
    setting = settings.get(setting_name) if setting_name else None
    if setting_name:
        referenced_settings.add(setting_name)
        if setting is None:
            rep.error(fw, f'setting {setting_name!r} is not in "settings"')
    for r in f.get('reads', []):
        referenced_settings.add(r)
        if r not in settings:
            rep.error(fw, f'reads {r!r}: not in "settings"')
    if setting_name is None and 'launcher' in surfaces and kind in ('choice', 'bool', 'number', 'text', 'color'):
        if not f.get('reads') or not f.get('decode'):
            rep.error(fw, 'a row with a null setting reads several settings (reads) and says how to decode them (decode)')
    values = effective_values(f, doc)
    if 'values_ref' in f and f['values_ref'] not in value_sets:
        rep.error(fw, f'values_ref {f["values_ref"]!r} is not in "value_sets"')
    if 'values_from' in f and not f.get('fallback_values'):
        rep.error(fw, 'values_from needs fallback_values (a reader that does not know the generator uses them)')
    if kind == 'choice' and values is None and 'values_from' not in f and not row_has_variants(f):
        rep.error(fw, 'a choice row lists its values (values, values_ref or values_from)')
    ids = []
    if values is not None:
        for vi, v in enumerate(values):
            vw = f'{fw}.values[{vi}:{v["id"]}]'
            if v['id'] in ids:
                rep.error(vw, 'value id is used twice in the row')
            ids.append(v['id'])
            if 'title' in v:
                t2 = title_rule().convert(v['label'])
                if t2 != v['title']:
                    rep.info(vw, f'title {v["title"]!r} differs from the rule\'s {t2!r} (an override)')
            if 'when' in v:
                ec.check(v['when'], f'{vw}.when', live_only=bool(v.get('live_only') or row.get('live_only')))
            for li, lr in enumerate(v.get('label_rules', [])):
                ec.check(lr['when'], f'{vw}.label_rules[{li}].when', live_only=bool(lr.get('live_only') or v.get('live_only') or row.get('live_only')))
            if v.get('auto_label') and v['id'] not in ('auto',) and not (setting and 'auto_value' in (setting.get('auto') or {})):
                rep.warn(vw, 'auto_label on a value that is not "auto" nor the setting\'s auto_value')
            writes = v.get('writes')
            if writes:
                for wi, wr in enumerate(writes):
                    ws = settings.get(wr['setting'])
                    referenced_settings.add(wr['setting'])
                    if ws is None:
                        rep.error(f'{vw}.writes[{wi}]', f'setting {wr["setting"]!r} is not in "settings"')
                        continue
                    val = wr['value']
                    if val == '$id':
                        val = v['id']
                    if val != '$previous':
                        reason = value_ok_for_setting(val, ws)
                        if reason:
                            rep.error(f'{vw}.writes[{wi}]', f'writes {val!r} to {wr["setting"]}: {reason}')
            elif setting is not None and kind in ('choice', 'number'):
                reason = value_ok_for_setting(v['id'], setting)
                if reason and not (setting.get('auto') and 'auto_value' in setting['auto'] and v['id'] == 'auto'):
                    rep.error(vw, f'id {v["id"]!r} for {setting_name}: {reason}')
    if setting is not None and kind == 'choice' and setting.get('allowed') and values is not None and 'values_from' not in f:
        for a in setting['allowed']:
            if a not in ids and not any(wr.get('setting') == setting_name for v in values for wr in v.get('writes', [])):
                rep.info(fw, f'the setting allows {a!r} but the row does not list it')
    if 'default' in f and ids and f['default'] not in ids:
        rep.error(fw, f'default {f["default"]!r} is not one of the row\'s value ids')
    dec = f.get('decode')
    if isinstance(dec, str):
        ec.check(dec, f'{fw}.decode', bool_needed=False)
        if not re.fullmatch(r'd\.[a-z0-9_]+', dec):
            rep.warn(fw, 'a decode that is a text is the name of a def (d.name)')
    elif isinstance(dec, dict):
        for ci, c in enumerate(dec.get('cases', [])):
            cw = f'{fw}.decode.cases[{ci}]'
            if 'when' in c:
                ec.check(c['when'], f'{cw}.when', live_only=bool(c.get('live_only') or row_live(f)))
            elif ci != len(dec['cases']) - 1:
                rep.error(cw, 'a case without when is the default: it must be the last')
            if ids and c['value'] not in ids:
                rep.error(cw, f'decodes to {c["value"]!r}, which is not a value id of the row')
    # apply and life (lint 4)
    apply_ = f.get('apply')
    lives = []
    if setting is not None:
        lives.append(setting.get('life'))
    for r in f.get('reads', []):
        if r in settings:
            lives.append(settings[r].get('life'))
    if apply_ and lives and not f.get('apply_note'):
        is_live_apply = apply_ == 'live'
        is_restart_apply = apply_ in ('restart', 'next_start', 'next_host')
        if is_restart_apply and all(l == 'live' for l in lives):
            rep.error(fw, f'apply says {apply_} but the setting is live (lint 4: say why in apply_note)')
        if is_live_apply and any(l not in ('live', None) for l in lives):
            rep.error(fw, f'apply says live but the setting\'s life is {[l for l in lives if l != "live"][0]} (lint 4: say why in apply_note)')


def row_live(f):
    return bool(f.get('live_only'))


def row_has_variants(f):
    return bool(f.get('variants'))


def lint_presets(doc, rep, ec, row_section, sections_by_id, settings, value_sets):
    pr = doc['presets']
    w = 'presets'
    order = pr['order']
    items = pr['items']
    if pr['setting'] not in settings:
        rep.error(w, f'setting {pr["setting"]!r} is not in "settings"')
    else:
        allowed = settings[pr['setting']].get('allowed')
        if allowed is not None and sorted(allowed) != sorted(order):
            rep.error(w, f'the preset setting allows {sorted(allowed)}, presets.order has {sorted(order)}')
    for name in order:
        if name not in items:
            rep.error(w, f'{name!r} is in order but not in items')
    for name in items:
        if name not in order:
            rep.error(w, f'{name!r} is in items but not in order')
    if 'first_start' in pr and pr['first_start'] not in order:
        rep.error(w, 'first_start is not in order')
    flip_to = pr.get('flip', {}).get('to')
    if pr.get('flip') and flip_to not in order:
        rep.error(w, f'flip.to {flip_to!r} is not in order')
    derived = set()
    for name, it in items.items():
        derived |= set(it.get('values', {}))
        if it.get('needs'):
            ec.check(it['needs'], f'{w}.items.{name}.needs', live_only=False)
            if not it.get('needs_reason'):
                rep.warn(f'{w}.items.{name}', 'a preset with a needs says why it is not offered (needs_reason)')
    # DISPLAY and UPSCALING rows are out of scope for presets (owner's scope rule)
    protected = set()
    for rid, sid in row_section.items():
        if sid in ('display', 'upscaling'):
            protected.add(rid)
    protected_settings = set()
    for sec_id in ('display', 'upscaling'):
        sec = sections_by_id.get(sec_id)
        if not sec:
            continue
        for row in sec['rows']:
            for f in [row] + row.get('variants', []):
                if f.get('setting'):
                    protected_settings.add(f['setting'])
                protected_settings |= set(f.get('reads', []))
                for v in (effective_values(f, doc) or []):
                    protected_settings |= {wr['setting'] for wr in v.get('writes', [])}
    for name, it in items.items():
        for s, v in it.get('values', {}).items():
            if s not in settings:
                rep.error(f'{w}.items.{name}', f'{s!r} is not in "settings"')
                continue
            if s in protected_settings:
                rep.error(f'{w}.items.{name}', f'{s!r} belongs to the DISPLAY or UPSCALING section: presets never touch those (lint 6)')
            reason = value_ok_for_setting(v, settings[s])
            if reason:
                rep.error(f'{w}.items.{name}', f'{s}={v!r}: {reason}')
    if 'covers' in pr and sorted(pr['covers']) != sorted(derived):
        rep.error(w, f'covers must be every setting any preset gives a value to: missing {sorted(derived - set(pr["covers"]))}, extra {sorted(set(pr["covers"]) - derived)}')
    for name, it in items.items():
        if name == flip_to:
            continue
        missing = sorted(derived - set(it.get('values', {})))
        if missing:
            rep.error(f'{w}.items.{name}', f'a preset has a value for every covered setting: missing {missing} (lint 6)')
    for ci, c in enumerate(pr['class_to_preset']):
        ec.check(c['when'], f'{w}.class_to_preset[{ci}].when')
        if c['preset'] not in items:
            rep.error(f'{w}.class_to_preset[{ci}]', f'preset {c["preset"]!r} is not in items')
    if pr['class_to_preset'] and pr['class_to_preset'][-1]['when'] != 'true':
        rep.warn(w, 'the last class_to_preset entry should be { "when": "true" } so every PC has a recommendation')
    for sid in pr.get('scope', []):
        if sid not in sections_by_id:
            rep.error(w, f'scope names the section {sid!r}, which does not exist')
        elif sid in ('display', 'upscaling'):
            rep.error(w, f'scope names {sid!r}: presets never cover DISPLAY or UPSCALING')


# ---- machine.json -------------------------------------------------------------------------------------------------------------------------

def flatten(obj, prefix=''):
    for k, v in obj.items():
        p = f'{prefix}{k}'
        if isinstance(v, dict):
            yield from flatten(v, p + '.')
        else:
            yield p, v


def lint_machine(doc, rep, facts, strict=False):
    for p, v in flatten({k: doc[k] for k in doc if k in FACT_NAMESPACES}):
        kind, entry = facts.lookup(p)
        if kind == 'unreadable':
            continue
        if kind == 'missing':
            rep.info(p, 'not in the fact dictionary (allowed: an additive fact; add it there once a rule reads it)')
            continue
        if kind != 'fact' or v is None:
            continue
        t = entry['type']
        ok = {'bool': isinstance(v, bool), 'int': isinstance(v, int) and not isinstance(v, bool), 'number': isinstance(v, (int, float)) and not isinstance(v, bool),
              'string': isinstance(v, str), 'list': isinstance(v, list)}[t]
        if not ok:
            rep.error(p, f'is {type(v).__name__}, the dictionary says {t}')
        elif 'enum' in entry and v not in entry['enum']:
            rep.error(p, f'{v!r} is not one of {entry["enum"]}')
        elif t == 'list' and any(not isinstance(x, str) for x in v):
            rep.error(p, 'a list fact holds texts')
    gpu = doc.get('gpu', {})
    sel = [a for a in gpu.get('adapters', []) if a.get('selected')]
    if gpu.get('adapters') and len(sel) != 1:
        rep.error('gpu.adapters', f'exactly one adapter is selected, {len(sel)} are')
    if doc.get('source') == 'game' and 'final' not in doc:
        rep.warn('final', 'a file written by a start says whether the last asynchronous probe has landed')
    if gpu.get('rt_tier') and gpu.get('rt_rung') and gpu['rt_tier'] != gpu['rt_rung']:
        rep.info('gpu.rt_rung', 'differs from rt_tier (allowed: the rung is what the Auto cells read)')


# ---- install-manifest.json ----------------------------------------------------------------------------------------------------------------

_PLACEHOLDER = re.compile(r'\{([^{}]*)\}')


def lint_manifest(doc, rep, facts, strict=False):
    ids = [a['id'] for a in doc.get('actions', [])]
    for i, a in enumerate(doc.get('actions', [])):
        w = f'actions[{a["id"]}]'
        if ids.count(a['id']) > 1:
            rep.error(w, 'action id is used twice')
        for r in a.get('after', []):
            if r not in ids:
                rep.error(w, f'after names {r!r}, which is not an action')
        flags = [a['flag']] + ([a['force_flag']] if 'force_flag' in a else [])
        for other in doc['actions'][:i]:
            if set(flags) & {other['flag'], other.get('force_flag')}:
                rep.error(w, 'flag is used by another action')
        for field in ('args', 'outputs'):
            for t in a.get(field, []):
                for ph in _PLACEHOLDER.findall(t):
                    if not re.fullmatch(r'[a-z][a-z0-9_]*', ph):
                        rep.error(w, f'{field}: placeholder {{{ph}}} is a lower case word')
        for t in a.get('outputs', []):
            if re.match(r'^([A-Za-z]:|/|\\\\)', t) or '..' in t.split('/'):
                rep.error(w, f'output {t!r} is relative to a placeholder folder')
        if 'when' in a:
            ExprChecker(rep, facts, allow_live=False, allow_settings=False, extra_install=doc.get('facts', {})).check(a['when'], f'{w}.when', rule_name='when')
        if a.get('stdout') and not a['stdout'].get('prefix'):
            rep.warn(w, 'stdout has no prefix: the launcher cannot tell its progress lines from other output')
    # cycles in `after`
    graph = {a['id']: a.get('after', []) for a in doc.get('actions', [])}
    state = {}

    def visit(n, stack):
        if state.get(n) == 2 or n not in graph:
            return
        if state.get(n) == 1:
            rep.error('actions', 'after loops: ' + ' -> '.join(stack + [n]))
            return
        state[n] = 1
        for m in graph[n]:
            visit(m, stack + [n])
        state[n] = 2

    for n in graph:
        visit(n, [])
    for name in doc.get('facts', {}):
        if not name.startswith('install.'):
            rep.error(f'facts.{name}', 'a file probe defines an install.* fact')
    for i, s in enumerate(doc.get('stage', [])):
        w = f'stage[{i}:{s["from"]}]'
        for field in ('from', 'to'):
            v = s[field]
            if re.match(r'^([A-Za-z]:|/|\\)', v) or '..' in re.split(r'[\\/]', v):
                rep.error(w, f'{field} {v!r} is a relative path inside the folder (no drive, no ..)')
        if s['kind'] == 'licence' and not s.get('licence_for'):
            rep.warn(w, 'a licence file says which file it is for (licence_for)')
        if s['to'].endswith('/') != s['from'].endswith('/**') and s['from'].endswith('/**'):
            rep.error(w, 'a folder "from" (ending /**) goes to a folder "to" (ending /)')
    froms = [s['from'] for s in doc.get('stage', [])]
    for f in set(froms):
        if froms.count(f) > 1:
            rep.warn('stage', f'{f!r} is staged twice')
    for k, v in doc.get('exit_codes', {}).items():
        if not (0 <= v <= 0xFFFFFFFF):
            rep.error(f'exit_codes.{k}', 'is an exit code')


# ---- launcher-recipe.json ---------------------------------------------------------------------------------------------------------------

def lint_recipe(doc, rep, facts, strict=False):
    names = [o['name'] for o in doc.get('options', [])]
    for i, o in enumerate(doc.get('options', [])):
        w = f'options[{o["name"]}]'
        if names.count(o['name']) > 1 and not o.get('when'):
            rep.warn(w, 'the option is listed twice and this entry has no when')
        if ('value' in o) == ('from' in o):
            rep.error(w, 'an option has either a value or a from (the launcher supplies it by name)')
        if 'when' in o:
            ExprChecker(rep, facts, allow_live=False, allow_settings=False, l0_only=True).check(o['when'], f'{w}.when', rule_name='when')
    for i, s in enumerate(doc.get('steps', [])):
        if 'when' in s:
            ExprChecker(rep, facts, allow_live=False, allow_settings=False, l0_only=True).check(s['when'], f'steps[{s["id"]}].when', rule_name='when')
    ids = [s['id'] for s in doc.get('steps', [])]
    for i in ids:
        if ids.count(i) > 1:
            rep.error('steps', f'step id {i!r} is used twice')
    if 'codegen' in doc and not doc['codegen'].get('inputs'):
        rep.warn('codegen', 'inputs lists the files the code generation reads (the launcher checks they exist before it runs the tool)')


def lint_describe(doc, rep, facts, strict=False):
    pass


LINTS = {
    'settings-ui': lint_settings_ui,
    'machine': lint_machine,
    'install-manifest': lint_manifest,
    'launcher-recipe': lint_recipe,
    'describe': lint_describe,
}


def validate(doc, kind=None, dictionary_doc=None, strict=False):
    """Schema + rules for one parsed document. Returns a Report."""
    rep = Report()
    if kind is None:
        kind = KINDS.get(doc.get('schema')) if isinstance(doc, dict) else None
    if kind is None:
        rep.error('schema', f'no known "schema" word (one of {sorted(KINDS)})')
        return rep
    schema = schema_of(kind)
    for path, msg in schema_errors(doc, schema, schema):
        rep.error(path, msg)
    if rep.errors:
        return rep   # the rules assume the shape
    facts = FactTypes(dictionary_doc or dictionary())
    used = LINTS[kind](doc, rep, facts, strict)
    rep.used = used
    return rep


def facts_used_of(doc):
    rep = Report()
    doc = copy.deepcopy(doc)
    doc.pop('facts_used', None)
    rep2 = validate(doc, 'settings-ui')
    return getattr(rep2, 'used', [])


# ---- selftest ------------------------------------------------------------------------------------------------------------------------------

def selftest():
    failures = []

    def check(cond, what):
        if not cond:
            failures.append(what)

    dct = dictionary()
    # 1. the schemas only use keywords this validator implements
    for kind in SCHEMA_FILES:
        used = schema_keywords(schema_of(kind))
        check(used <= SUPPORTED_KEYWORDS, f'{kind} schema uses unsupported keywords {sorted(used - SUPPORTED_KEYWORDS)}')
    # 2. the golden examples have no error and no warning
    ex_dir = os.path.join(HERE, 'examples')
    examples = sorted(f for f in os.listdir(ex_dir) if f.endswith('.json'))
    for f in examples:
        doc = load_json(os.path.join(ex_dir, f))
        rep = validate(doc)
        check(not rep.errors and not rep.warnings, f'{f}: {(rep.errors + rep.warnings)[:3]}')
    check(len(examples) >= 7, 'the examples folder has the golden files')
    # 3. the parser agrees with the shared condition vectors (syntax, unsupported calls, paths, unguarded live)
    tests = load_json(os.path.join(HERE, 'condition-tests.json'))
    n = 0
    for c in tests['cases']:
        if 'expr' not in c:
            continue
        e = parse_expr(c['expr'])
        n += 1
        check(e.ok != c.get('syntax_error', False), f'{c["id"]}: parse {"worked" if e.ok else "failed"}')
        if e.ok:
            check(e.unsupported_call == c.get('unsupported_call', False), f'{c["id"]}: unsupported_call')
            if 'paths' in c:
                check(e.paths == c['paths'], f'{c["id"]}: paths {e.paths}')
            if 'unguarded_live' in c:
                check(e.unguarded_live == c['unguarded_live'], f'{c["id"]}: unguarded_live {e.unguarded_live}')
    check(n >= 300, 'the vectors have the cases')
    # 4. Title Case against its vectors, and idempotence
    tdoc = load_json(os.path.join(HERE, 'ui-text-case.json'))
    rule = TitleRule(tdoc)
    for c in tdoc['cases']:
        got = rule.convert(c['label'])
        check(got == c['title'], f'title case {c["label"]!r}: {got!r}, expected {c["title"]!r}')
        check(rule.convert(got) == got, f'title case is not idempotent on {got!r}')
    check(rule.guesses('XYZ SETTING') == ['XYZ'] and rule.guesses('GPU MODE') == [], 'guesses')
    # 5. the dictionary: every entry has a type, enum words fit the type
    for p, e in dct['facts'].items():
        check(e['type'] in dct['types'], f'dictionary {p}: type')
    # 6. mutations of the examples are caught
    base = load_json(os.path.join(ex_dir, 'settings-ui.upcoming.example.json'))

    def mutate(fn):
        d = copy.deepcopy(base)
        fn(d)
        return validate(d, 'settings-ui')

    def rows(d):
        return [r for t in d['tabs'] for s in t['sections'] for r in s['rows']]

    def row(d, rid):
        return next(r for r in rows(d) if r['id'] == rid)

    def expect(name, fn, fragment):
        rep = mutate(fn)
        hit = any(fragment in m for m in rep.errors + rep.warnings)
        check(hit, f'mutation "{name}" is not reported (wanted "{fragment}"): {(rep.errors + rep.warnings)[:2]}')

    expect('a row names a setting that does not exist', lambda d: row(d, 'PM_RxFpsCap').update(setting='fps_limt'), 'not in "settings"')
    expect('a value id the setting does not allow', lambda d: row(d, 'PM_RxGpuShadowDetail').update(values_ref='rt_levels'), 'allowed')
    expect('a typo in a fact', lambda d: d['defs'].update(frame_gen_on='gpu.clas == \'high\''), 'not in the fact dictionary')
    expect('an unknown function', lambda d: d['defs'].update(frame_gen_on='fooo(1)'), 'outside the v1 set')
    expect('an expression that does not parse', lambda d: row(d, 'PM_RxFpsCap')['states'][0].update(when='a &&'), 'does not parse')
    expect('a live fact with no fallback', lambda d: row(d, 'PM_RxFpsCap')['states'][0].update(when="live.city_data == 'ready'"), 'needs a ?? fallback')
    expect('a bool needed, a string given', lambda d: row(d, 'PM_RxFpsCap')['states'][0].update(when='gpu.class'), 'a bool is needed')
    expect('a comparison of different kinds', lambda d: row(d, 'PM_RxFpsCap')['states'][0].update(when="gpu.class == 3"), 'always null')
    expect('a row without help', lambda d: row(d, 'PM_RxFpsCap').pop('help'), 'has help')
    expect('apply says restart for a live setting', lambda d: row(d, 'PM_RxFpsCap').update(apply='restart'), 'lint 4')
    expect('a RESTART suffix without needs_restart',
           lambda d: next(s for s in row(d, 'PM_RxGpuDenoiser')['states'] if s.get('needs_restart')).pop('needs_restart'),
           'needs_restart')
    expect('a preset that misses a covered setting', lambda d: d['presets']['items']['enhanced']['values'].pop('mcla_gpu_rt_reflections'), 'lint 6')
    expect('a preset that touches DISPLAY', lambda d: d['presets']['items']['enhanced']['values'].update(fps_limit='60'), 'DISPLAY or UPSCALING')
    expect('facts_used out of date', lambda d: d['facts_used'].pop(), 'facts_used')
    expect('a duplicate row id', lambda d: row(d, 'PM_RxFpsCap').update(id='PM_RxDisplayMode'), 'used twice')
    expect('a def loop', lambda d: d['defs'].update(loop_a='d.loop_b', loop_b='d.loop_a'), 'loop')
    expect('a link row with no target', lambda d: rows(d)[0].update(kind='link'), 'link row names the section')
    expect('values_from with no fallback', lambda d: row(d, 'PM_RxGpuUpscMode')['variants'][0].pop('fallback_values'), 'fallback_values')
    expect('a launcher-only row without the L_ prefix', lambda d: row(d, 'PM_RxFpsCap').update(surfaces=['launcher']), 'L_<setting>')
    expect('a schema error: schema_version 0', lambda d: d.update(schema_version=0), 'below 1')
    expect('a setting default that is not valid', lambda d: d['settings']['fps_limit'].update(default='fast'), 'default')
    # the other kinds
    mdoc = load_json(os.path.join(ex_dir, 'machine.example.json'))
    for name, fn, fragment in [
        ('machine: a class the dictionary does not know', lambda d: d['gpu'].update(**{'class': 'ultra'}), 'is not one of'),
        ('machine: a bool as text', lambda d: d['api'].update(car_rt='yes'), 'expected boolean'),
        ('machine: two selected adapters', lambda d: d['gpu']['adapters'][1].update(selected=True), 'exactly one adapter'),
    ]:
        d = copy.deepcopy(mdoc)
        fn(d)
        rep = validate(d)
        check(any(fragment in m for m in rep.errors), f'mutation "{name}" not reported: {rep.errors[:2]}')
    idoc = load_json(os.path.join(ex_dir, 'install-manifest.example.json'))
    for name, fn, fragment in [
        ('manifest: an action after a missing one', lambda d: d['actions'][1].update(after=['nope']), 'not an action'),
        ('manifest: after loops', lambda d: (d['actions'][0].update(after=['precache_pipelines'])), 'after loops'),
        ('manifest: a stage path with ..', lambda d: d['stage'][0].update(to='../x.exe'), 'relative path'),
        ('manifest: a when with a setting', lambda d: d['actions'][2].update(when="s.fps_limit == 60"), 'settings are not readable'),
        ('manifest: a fact not install.*', lambda d: d['facts'].update(**{'gpu.foo': {'any_of': ['a']}}), 'install.* fact'),
    ]:
        d = copy.deepcopy(idoc)
        fn(d)
        rep = validate(d)
        check(any(fragment in m for m in rep.errors), f'mutation "{name}" not reported: {rep.errors[:2]}')
    rdoc = load_json(os.path.join(ex_dir, 'launcher-recipe.example.json'))
    for name, fn, fragment in [
        ('recipe: a when needing a probed fact', lambda d: d['options'][2].update(when="gpu.class == 'high'"), 'before the first probe'),
        ('recipe: an option with both value and from', lambda d: d['options'][1].update(value='x'), 'either a value or a from'),
        ('recipe: no targets', lambda d: d.update(targets=[]), 'needs 1'),
    ]:
        d = copy.deepcopy(rdoc)
        fn(d)
        rep = validate(d)
        check(any(fragment in m for m in rep.errors), f'mutation "{name}" not reported: {rep.errors[:2]}')
    rep = mutate(lambda d: row(d, 'PM_RxFpsCap')['states'][0].update(when="live.city_data == 'ready'", live_only=True))
    check(not any('needs a ?? fallback' in m for m in rep.errors), 'live_only on a rule must silence lint 3')
    if failures:
        print('SELFTEST FAILED')
        for f in failures:
            print('  -', f)
        return 1
    print(f'selftest ok: {len(examples)} examples valid, {n} vectors parsed, {len(tdoc["cases"])} title cases, mutations caught')
    return 0


# ---- command line -------------------------------------------------------------------------------------------------------------------------

def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('files', nargs='*')
    ap.add_argument('--kind', choices=sorted(SCHEMA_FILES))
    ap.add_argument('--strict', action='store_true', help='warnings are errors')
    ap.add_argument('--quiet', action='store_true', help='print only errors (and warnings with --strict)')
    ap.add_argument('--selftest', action='store_true')
    ap.add_argument('--facts-used', metavar='FILE', help='print the facts_used list of a settings-ui.json (what the rules read) and exit')
    ap.add_argument('--dictionary', help='another fact-dictionary.json')
    args = ap.parse_args(argv)
    if args.selftest:
        return selftest()
    if args.facts_used:
        print(json.dumps(facts_used_of(load_json(args.facts_used)), indent=2))
        return 0
    if not args.files:
        ap.error('give a file, --selftest or --facts-used')
    dct = load_json(args.dictionary) if args.dictionary else None
    bad = 0
    for path in args.files:
        try:
            doc = load_json(path)
        except (OSError, ValueError) as ex:
            print(f'ERROR {path}: cannot read: {ex}')
            bad += 1
            continue
        rep = validate(doc, args.kind, dct, args.strict)
        for m in rep.errors:
            print(f'ERROR {path}: {m}')
        for m in rep.warnings:
            print(f'WARN  {path}: {m}')
        if not args.quiet:
            for m in rep.infos:
                print(f'INFO  {path}: {m}')
        if rep.errors or (args.strict and rep.warnings):
            bad += 1
        elif not args.quiet:
            print(f'ok    {path} ({len(rep.warnings)} warnings, {len(rep.infos)} notes)')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
