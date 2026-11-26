"""Static analysis of LARecomp's hand-written C++ with LLVM 23 (an optional developer tool).

The game's normal build does not need LLVM 23. This script reads the compile commands of a
configured, already built CMake build directory and runs three checks over the hand-written
sources under src/ (generated code and thirdparty code are left out):

  * clang++ lifetime safety (-Wlifetime-safety) and the -Wdangling family,
  * the Clang Static Analyzer (clang-analyzer-core, cplusplus and unix checks, run by clang-tidy),
  * a short list of clang-tidy bug checks (use after move, suspicious code, mt-unsafe calls).

  python tools/static_analysis/static_analysis.py --new-only    any new findings?
  python tools/static_analysis/static_analysis.py --only src/gpu/edram.cpp --new-only
  python tools/static_analysis/static_analysis.py --out scan.json    everything, with the traces

Exit 0 means clean, 1 means findings, and 2 means the run was incomplete (a tool or a source
could not be run; the lines above the verdict say which).

LLVM is taken from --llvm-bin, else the LARECOMP_LLVM23_BIN environment variable, else clang++
and clang-tidy on PATH. The build directory defaults to the only one under out/build.

baseline.json (next to this script) lists the findings that were reviewed and are not bugs.
Each entry has a category (false-positive or style) and a reason. A real bug is fixed in the
code and never goes in the baseline. To add a reviewed finding, scan with --out out/static-scan.json, then
use the accept mode; documentation/STATIC-ANALYSIS.md has the steps.
"""

import argparse
from collections import Counter, defaultdict
from concurrent.futures import ThreadPoolExecutor, as_completed
import ctypes
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
from tempfile import TemporaryDirectory


HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
MIN_LLVM = 23
LLVM_ENV = 'LARECOMP_LLVM23_BIN'
LLVM_HELP = (
    f'This check needs LLVM {MIN_LLVM} or newer (clang++ and clang-tidy); the game\'s normal build does not.\n'
    f'Get the LLVM {MIN_LLVM} release for your system from https://github.com/llvm/llvm-project/releases and unpack it anywhere.\n'
    f'Then pass its bin folder with --llvm-bin, or set {LLVM_ENV} to it, or put it first on PATH.')
CHECKS = ','.join((
    '-*', 'clang-analyzer-core.*', 'clang-analyzer-cplusplus.*', 'clang-analyzer-unix.*',
    'bugprone-use-after-move', 'bugprone-dangling-handle', 'bugprone-unhandled-self-assignment',
    'bugprone-undefined-memory-manipulation', 'bugprone-suspicious-*', 'concurrency-mt-unsafe',
    'misc-redundant-expression', 'performance-move-const-arg',
))
WARNINGS = ('-Wlifetime-safety', '-Wdangling', '-Wdangling-gsl', '-Wreturn-stack-address')
TIDY = ('--config={}', '--checks=' + CHECKS, '--use-color=false')
DIAGNOSTIC = re.compile(r'^(.+?):(\d+):(\d+): warning: (.+?) \[([^\]]+)\]\s*$', re.M)
ABSOLUTE_ROOT = r'(?:[A-Za-z]:[/\\]|\\\\|/)'
MESSAGE_LOCATION = re.compile(
    r'(?P<file>\b[\w.+-]+\.[A-Za-z][\w+-]*):\d+(?::\d+)?\b'
    r'|\b(?:line|column)\s+\d+(?::\d+)?\b', re.IGNORECASE)
SOURCE_EXCERPT = re.compile(r'[ \t]*(\d+)[ \t]+\| (.*)')
BASELINE_VERSION = 1
CATEGORIES = ('false-positive', 'style')
SOURCE_EXTENSIONS = {'.c', '.cc', '.cpp', '.cxx'}
LINUX_STUBS = 'src/compat/linux/linux_stubs.cpp'
# The native runtime that LARECOMP_NATIVE_GFX_LEGACY=OFF takes out of LARECOMP_SOURCES (root CMakeLists.txt), plus src/native_gfx/d3d12/.
LEGACY_OFF = {'src/native_gfx/native_gfx.cpp', 'src/native_gfx/geometry.cpp',
              'src/native_gfx/telemetry.cpp', 'src/native_gfx/guest/sampler_state.cpp'}
GIB = 1024**3


def write_text(path, text):
    # Always LF and ASCII, whatever the platform (Path.write_text alone writes CRLF on Windows).
    path.write_text(text, encoding='ascii', newline='\n')


def command_line_args(command):
    if os.name != 'nt':
        return shlex.split(command)
    parse = ctypes.windll.shell32.CommandLineToArgvW
    parse.argtypes = (ctypes.c_wchar_p, ctypes.POINTER(ctypes.c_int))
    parse.restype = ctypes.POINTER(ctypes.c_wchar_p)
    free = ctypes.windll.kernel32.LocalFree
    free.argtypes = (ctypes.c_void_p,)
    free.restype = ctypes.c_void_p
    count = ctypes.c_int()
    argv = parse(command, ctypes.byref(count))
    if not argv:
        raise ctypes.WinError()
    try:
        return argv[:count.value]
    finally:
        free(argv)


# -------------------------------------------------------------------------------- toolchain

def locate_llvm(folder):
    """(clang++, clang-tidy): --llvm-bin, else LARECOMP_LLVM23_BIN, else PATH."""
    ext = '.exe' if os.name == 'nt' else ''
    names = ('clang++', 'clang-tidy')
    folder = folder or os.environ.get(LLVM_ENV)
    if folder:
        found = [Path(folder) / (name + ext) for name in names]
        absent = [str(path) for path in found if not path.is_file()]
        if absent:
            raise ValueError(f'not found: {", ".join(absent)}\n{LLVM_HELP}')
        return tuple(str(path) for path in found)
    found = [shutil.which(name) for name in names]
    if not all(found):
        absent = [name for name, path in zip(names, found) if not path]
        raise ValueError(f'{" and ".join(absent)} not found on PATH\n{LLVM_HELP}')
    return tuple(found)


def run_tool(args, directory, env, timeout):
    proc = subprocess.run(args, cwd=directory, env=env, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, encoding='utf-8', errors='replace', timeout=timeout)
    return proc.returncode, proc.stdout


def tool_version(tool, directory, env):
    code, output = run_tool([tool, '--version'], directory, env, 15)
    match = None if 'Apple' in output else re.search(r'\bversion (\d+\.\d+\.\d+)', output)
    if code or not match:
        raise ValueError(f'{tool} is not LLVM {MIN_LLVM} or newer: {output.strip().splitlines()[0] if output.strip() else "no output"}\n{LLVM_HELP}')
    if int(match[1].split('.')[0]) < MIN_LLVM:
        raise ValueError(f'{tool} is LLVM {match[1]}, but LLVM {MIN_LLVM} or newer is needed\n{LLVM_HELP}')
    return match[1]


def verify_tools(args, env):
    """Check that both tools are LLVM 23 or newer and that clang-tidy accepts the check list."""
    versions = {tool_version(tool, args.tree, env) for tool in (args.clang, args.tidy)}
    if len(versions) != 1:
        raise ValueError(f'clang++ and clang-tidy are different LLVM versions ({", ".join(sorted(versions))}); use one install')
    code, output = run_tool([args.tidy, *TIDY, '--verify-config'], args.tree, env, 15)
    if code:
        raise ValueError(f'clang-tidy configuration failed: {output.strip()}')
    args.llvm_version = versions.pop()


def toolchain_env():
    """The environment for clang. On Windows, when INCLUDE is not set (no Developer PowerShell),
    the MSVC and Windows SDK headers are looked up the way Visual Studio keeps them; if they are
    not found clang's own search is left to do its work."""
    env = dict(os.environ)
    if os.name != 'nt' or env.get('INCLUDE'):
        return env
    include = []
    program_files = Path(env.get('ProgramFiles(x86)', r'C:\Program Files (x86)'))
    vswhere = program_files / 'Microsoft Visual Studio/Installer/vswhere.exe'
    try:
        code, output = run_tool([str(vswhere), '-latest', '-products', '*', '-requires',
                                 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64',
                                 '-property', 'installationPath'], None, env, 30)
        root = Path(output.strip().splitlines()[0]) if code == 0 and output.strip() else None
        if root:
            version = (root / 'VC/Auxiliary/Build/Microsoft.VCToolsVersion.default.txt').read_text(encoding='utf-8').strip()
            include.append(root / 'VC/Tools/MSVC' / version / 'include')
        kits = program_files / 'Windows Kits/10/Include'
        versions = sorted((p for p in kits.iterdir() if re.fullmatch(r'\d+\.\d+\.\d+\.\d+', p.name)
                           and (p / 'ucrt').is_dir()), key=lambda p: tuple(map(int, p.name.split('.'))))
        if versions:
            include += [versions[-1] / name for name in ('ucrt', 'um', 'shared', 'winrt')]
    except (OSError, subprocess.SubprocessError, IndexError):
        pass
    if include:
        env['INCLUDE'] = ';'.join(str(path) for path in include)
    return env


def available_memory():
    """Free physical memory in bytes, or None when it cannot be read."""
    try:
        if os.name == 'nt':
            # MEMORYSTATUSEX: length, load, available/total physical and virtual memory.
            status = (ctypes.c_uint64 * 8)()
            status[0] = ctypes.sizeof(status)
            if ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(status)):
                return status[2]
            return None
        meminfo = Path('/proc/meminfo')
        if meminfo.is_file():
            match = re.search(r'^MemAvailable:\s+(\d+) kB', meminfo.read_text(), re.M)
            return int(match[1]) * 1024 if match else None
        return os.sysconf('SC_AVPHYS_PAGES') * os.sysconf('SC_PAGE_SIZE')
    except (OSError, ValueError, AttributeError):
        return None


def job_count(requested):
    # One clang-tidy parse of a large source takes about 3 GB, plus a clang++ pass beside it:
    # a job is budgeted at 4 GB and 8 GB stay free for the rest of the PC.
    memory = available_memory()
    memory_jobs = max(1, (memory - 8 * GIB) // (4 * GIB)) if memory is not None else 2
    cpus = getattr(os, 'process_cpu_count', os.cpu_count)() or 1
    return min(requested or 4, memory_jobs, cpus)


# -------------------------------------------------------------------------------- build directory

def default_build(tree):
    builds = sorted(p.parent for p in (tree / 'out/build').glob('*/CMakeCache.txt'))
    if len(builds) > 1:
        preferred = [b for b in builds if b.name.endswith('-relwithdebinfo')]
        builds = preferred if len(preferred) == 1 else builds
    if len(builds) == 1:
        return builds[0]
    if not builds:
        raise ValueError(f'no configured build under {tree / "out/build"}: configure and build the game first '
                         '(see the README) or pass --build')
    raise ValueError('several build directories under out/build (' + ', '.join(b.name for b in builds) + '): pass --build')


def cache_value(build, name):
    match = re.search(rf'^{name}:[^=]+=(.+)$', (build / 'CMakeCache.txt').read_text(encoding='utf-8'), re.M)
    if not match:
        raise ValueError(f'{name} missing from {build / "CMakeCache.txt"}')
    return match[1]


def compilation_entries(build):
    db = build / 'compile_commands.json'
    if db.is_file():
        return json.loads(db.read_text(encoding='utf-8-sig')), db
    ninja = cache_value(build, 'CMAKE_MAKE_PROGRAM')
    if not Path(ninja).is_file():
        ninja = shutil.which('ninja') or ninja
    # Ninja compdb reads the configured rules, including generated dependencies.
    # It does not run any of the commands. Expand response files in the output.
    proc = subprocess.run([ninja, '-C', str(build), '-t', 'compdb', '-x'],
                          capture_output=True, encoding='utf-8', errors='replace', timeout=45)
    if proc.returncode:
        raise ValueError(f'ninja -t compdb failed: {proc.stderr.strip()}')
    return json.loads(proc.stdout), build / 'build.ninja'


def relative_source(file, tree):
    try:
        relative = file.resolve().relative_to(tree)
    except ValueError:
        return None
    if len(relative.parts) > 1 and relative.parts[0] == 'src':
        return relative.as_posix()
    return None


def analysis_args(args, original, source, build_tree, tree, directory):
    """Strip the object/dependency outputs and remap includes of hand-written code."""
    if Path(args[0]).stem.lower() not in {'clang', 'clang++'}:
        raise ValueError(f'expected the build\'s clang/clang++ command for {original}')

    def map_include(value):
        rel = relative_source(directory / value, build_tree)
        if rel or (directory / value).resolve() == build_tree / 'src':
            return (tree / (rel or 'src')).as_posix()
        return value

    flags = []
    remaining = iter(args[1:])
    for arg in remaining:
        if arg in ('-o', '-MT', '-MF', '-MQ', '-MJ'):
            next(remaining)
        elif arg in ('-c', '-MD', '-MMD', '-MP'):
            continue
        elif arg in ('-I', '-isystem', '-iquote', '-idirafter', '-include', '-imacros'):
            flags.extend((arg, map_include(next(remaining))))
        elif arg.startswith('-I') and len(arg) > 2:
            flags.append('-I' + map_include(arg[2:]))
        elif not arg.startswith('-') and (directory / arg).resolve() == original:
            continue
        else:
            flags.append(arg)
    # Preserve target-specific include precedence (including test stubs).
    # Older configurations may lack src for newly source-root-relative headers.
    return ['-I' + tree.as_posix(), *flags, '-I' + (tree / 'src').as_posix(),
            '-Wno-error', source.as_posix()]


def source_jobs(entries, build_tree, tree, only):
    jobs = defaultdict(set)
    for entry in entries:
        directory = Path(entry['directory']).resolve()
        original = (directory / entry['file']).resolve()
        if original.suffix.lower() not in SOURCE_EXTENSIONS:
            continue
        relative = relative_source(original, build_tree)
        if not relative or (only and relative != only) or not (tree / relative).is_file():
            continue
        args = entry.get('arguments') or command_line_args(entry['command'])
        flags = analysis_args(args, original, tree / relative, build_tree, tree, directory)
        jobs[relative].add((str(directory), tuple(flags)))
    return {relative: sorted(configs) for relative, configs in sorted(jobs.items())}


# -------------------------------------------------------------------------------- the tree's own source list

def cmake_block(text, start):
    """The text between the parentheses that follow the first match of `start` (nesting counted)."""
    match = re.search(start, text)
    if not match:
        return ''
    depth, end = 1, match.end()
    while end < len(text) and depth:
        depth += {'(': 1, ')': -1}.get(text[end], 0)
        end += 1
    return text[match.end():end - 1]


def cmake_cpp_names(block):
    names = []
    for line in block.split('\n'):
        for token in line.split('#', 1)[0].split():
            token = token.strip('"')
            if token.endswith('.cpp') and '$' not in token:
                names.append(token)
    return names


def expected_sources(tree):
    """(sources, conditional): every .cpp the tree's CMake files give the game, the gpu, shader pack
    and city pack libraries; `conditional` are the ones a library takes only under a condition."""
    def read(path):
        try:
            return (tree / path).read_text(encoding='utf-8', errors='replace')
        except OSError:
            return ''

    sources = set()
    conditional = set()
    root = read('CMakeLists.txt')
    sources.update(cmake_cpp_names(cmake_block(root, r'set\(LARECOMP_SOURCES')))
    for match in re.finditer(r'list\(APPEND LARECOMP_SOURCES([^)]*)\)', root):
        sources.update(name for name in cmake_cpp_names(match[1]) if not name.startswith('src/compat/linux/'))
    sources.add('src/cpu_level.cpp')
    for cmake, library, kind, prefix in (('src/gpu/CMakeLists.txt', 'larecomp_gpu', 'OBJECT', 'src/gpu/'),
                                         ('src/shaderpack/CMakeLists.txt', 'larecomp_shaderpack', 'STATIC', 'src/shaderpack/'),
                                         ('src/citypack/CMakeLists.txt', 'larecomp_citypack', 'STATIC', 'src/citypack/')):
        text = read(cmake)
        names = cmake_cpp_names(cmake_block(text, r'add_library\(' + library + ' ' + kind))
        for match in re.finditer(r'target_sources\(\s*' + library + r'\s+(?:PRIVATE|PUBLIC)\s+([^)]*)\)', text):
            for name in cmake_cpp_names(match[1]):
                names.append(name)
                conditional.add(prefix + name)
        sources.update(prefix + name for name in names)
    sources = {path for path in sources if path not in LEGACY_OFF and not path.startswith('src/native_gfx/d3d12/')}
    sources.add(LINUX_STUBS)
    sources = {path for path in sources if (tree / path).is_file()}
    return sources, conditional & sources


# -------------------------------------------------------------------------------- diagnostics and ids

def find_diagnostics(output, tree, directory, origin):
    matches = list(DIAGNOSTIC.finditer(output))
    for index, match in enumerate(matches):
        path, line, column, message, check = match.groups()
        relative = relative_source(Path(directory) / path, tree)
        if not relative:
            continue
        tool = origin
        if check.startswith('clang-analyzer-'):
            tool = 'analyzer'
        elif check.startswith('clang-diagnostic-'):
            # The independent compiler pass already captures compiler warnings.
            continue
        elif origin == 'lifetime' and not check.startswith(('-Wlifetime-safety-', '-Wdangling', '-Wreturn-stack-address')):
            continue
        end = matches[index + 1].start() if index + 1 < len(matches) else len(output)
        yield {'tool': tool, 'check': check, 'path': relative, 'line': int(line),
               'column': int(column), 'message': message, 'trace': output[match.start():end].rstrip()}


def run_source(relative, configs, args, db, env, header_filter):
    messages = []
    errors = []
    source = (args.tree / relative).as_posix()
    commands = [('lifetime', directory, [args.clang, *flags, '-fsyntax-only', '-fno-color-diagnostics',
                 '-fdiagnostics-format=clang', '-Werror=unknown-warning-option', *WARNINGS])
                for directory, flags in configs]
    commands.append(('tidy', str(args.tree), [args.tidy, *TIDY, header_filter, '-p', str(db), source]))
    for tool, directory, command in commands:
        try:
            code, output = run_tool(command, directory, env, args.timeout)
            messages.extend(find_diagnostics(output, args.tree, directory, tool))
            if code:
                errors.append({'path': relative, 'tool': tool, 'error': f'exit {code}', 'output': output})
        except subprocess.TimeoutExpired as exc:
            output = (exc.output or b'').decode('utf-8', errors='replace')
            messages.extend(find_diagnostics(output, args.tree, directory, tool))
            errors.append({'path': relative, 'tool': tool, 'error': f'timed out after {args.timeout}s',
                           'output': output})
        except OSError as exc:
            errors.append({'path': relative, 'tool': tool, 'error': str(exc)})
    return messages, errors


def message_paths(tree):
    # Clang can print a checkout path containing spaces without quotes.
    root = r'[/\\]'.join(re.escape(part) for part in tree.as_posix().split('/'))
    return re.compile(
        rf"""(?:(?P<quote>['"]){ABSOLUTE_ROOT}(?:(?!(?P=quote))[^\r\n])+(?P=quote)"""
        rf"""|(?<![\w./\\])(?:{root}[/\\]|{ABSOLUTE_ROOT})[^\s:'"<>()\[\]{{}},;]+)"""
        r'(?::\d+){0,2}', re.IGNORECASE if os.name == 'nt' else 0)


def digest(*fields):
    return hashlib.sha256(json.dumps(fields).encode('ascii')).hexdigest()


def finding_id(finding, anchor, paths):
    message = paths.sub('<path>', finding['message'])
    message = MESSAGE_LOCATION.sub(lambda match: match['file'] or '<location>', message)
    return digest(finding['path'], finding['check'], ' '.join(message.split()), anchor)


def trace_anchor(finding):
    # Only the source excerpt immediately after the warning belongs to its location.
    excerpt = finding.get('trace', '').partition('\n')[2].partition('\n')[0]
    match = SOURCE_EXCERPT.fullmatch(excerpt)
    if match and int(match[1]) == finding['line']:
        return match[2].strip()
    return None


def source_anchor(finding, lines):
    index = finding['line'] - 1
    anchor = lines[index] if 0 <= index < len(lines) else ''
    excerpt = trace_anchor(finding)
    # A stale location can only be recovered when the reported text is unique.
    if excerpt and excerpt != anchor and lines.count(excerpt) == 1:
        return excerpt
    return anchor


def identify(findings, tree):
    """Deduplicate shared-header diagnostics, then anchor each finding to its source line.
    The id is made of the path, the check, the message and the stripped source line, so it
    survives edits elsewhere in the file and does not depend on where the checkout is."""
    distinct = {}
    for finding in findings:
        key = tuple(finding[field] for field in ('path', 'line', 'column', 'tool', 'check', 'message'))
        distinct.setdefault(key, finding)
    paths = message_paths(tree)
    lines = {}
    for key in sorted(distinct):
        finding = distinct[key]
        path = finding['path']
        if path not in lines:
            source = (tree / path).read_text(encoding='utf-8', errors='replace')
            lines[path] = [line.strip() for line in source.splitlines()]
        finding['id'] = finding_id(finding, source_anchor(finding, lines[path]), paths)
        yield finding


# -------------------------------------------------------------------------------- the reviewed baseline

def profile():
    return {'llvm_min': MIN_LLVM, 'tidy_checks': CHECKS, 'compiler_warnings': list(WARNINGS)}


def baseline_counts(data):
    accepted = {}
    for entry in data['findings']:
        if (not re.fullmatch(r'[0-9a-f]{64}', entry['id']) or entry['id'] in accepted
                or entry['category'] not in CATEGORIES or not entry['reason'].strip()
                or type(entry['count']) is not int or entry['count'] < 1):
            raise ValueError(f'invalid baseline entry: {entry}')
        accepted[entry['id']] = entry['count']
    return accepted


def read_baseline(file):
    data = json.loads(file.read_text(encoding='utf-8-sig'))
    if data['version'] != BASELINE_VERSION or data['profile'] != profile():
        raise ValueError(f'baseline version or analysis profile changed: {file}')
    return data


def baseline_for(file):
    data = read_baseline(file)
    return data['reviewed'] is True, baseline_counts(data), data.get('reviewed_with')


def baseline_text(data):
    data = dict(data, findings=sorted(data['findings'], key=lambda e: (e['path'], e['lines'], e['check'], e['id'])))
    return json.dumps(data, indent=2) + '\n'


def accept_findings(data, report, prefixes, category, reason):
    """The baseline data with the findings of a scan report (those whose id starts with one of
    `prefixes`) added or, when the id is already there, their count and lines updated."""
    if report['profile'] != profile():
        raise ValueError('the scan report was made with another analysis profile; scan again')
    groups = defaultdict(list)
    for finding in report['findings']:
        groups[finding['id']].append(finding)
    entries = {entry['id']: dict(entry) for entry in data['findings']}
    for prefix in prefixes:
        ids = [key for key in groups if key.startswith(prefix)]
        if len(ids) != 1:
            raise ValueError(f'id prefix {prefix!r} matches {len(ids)} findings in the report; use a longer prefix')
        key = ids[0]
        group = groups[key]
        lines = sorted(finding['line'] for finding in group)
        if key in entries:
            entries[key].update(count=len(group), lines=lines)
        else:
            if category not in CATEGORIES or not reason.strip():
                raise ValueError(f'a new entry needs --category ({" or ".join(CATEGORIES)}) and a --reason')
            entries[key] = {'id': key, 'count': len(group), 'category': category, 'reason': reason.strip(),
                            'path': group[0]['path'], 'lines': lines, 'check': group[0]['check']}
    result = dict(data, findings=list(entries.values()))
    baseline_counts(result)
    return result


def classify(findings, accepted):
    remaining = Counter(accepted)
    for finding in findings:
        key = finding['id']
        finding['status'] = 'accepted' if remaining[key] > 0 else 'new'
        remaining[key] -= 1


# -------------------------------------------------------------------------------- running

def scan(args, jobs, env):
    root = r'[/\\]'.join(re.escape(part) for part in args.tree.as_posix().split('/'))
    header_filter = '--header-filter=^' + root + r'[/\\]src[/\\]'
    with TemporaryDirectory(prefix='larecomp-static-') as temp:
        db = Path(temp)
        entries = [{'directory': directory, 'file': (args.tree / rel).as_posix(),
                    'arguments': [args.clang, *flags]} for rel, configs in jobs.items() for directory, flags in configs]
        write_text(db / 'compile_commands.json', json.dumps(entries))
        findings = []
        errors = []
        with ThreadPoolExecutor(max_workers=min(len(jobs), job_count(args.jobs))) as pool:
            todo = [pool.submit(run_source, rel, configs, args, db, env, header_filter)
                    for rel, configs in jobs.items()]
            for index, result in enumerate(as_completed(todo), 1):
                messages, failures = result.result()
                findings.extend(messages)
                errors.extend(failures)
                if index % 25 == 0 or index == len(todo):
                    print(f'Progress {index}/{len(todo)} sources ({len(errors)} tool errors)', flush=True)
    return list(identify(findings, args.tree)), sorted(errors, key=lambda err: (err['path'], err['tool']))


def evaluate(args):
    build_tree = Path(cache_value(args.build, 'CMAKE_HOME_DIRECTORY')).resolve()
    entries, database = compilation_entries(args.build)
    jobs = source_jobs(entries, build_tree, args.tree, args.only)
    if not jobs:
        raise ValueError(f'no matching hand-written source in {database}')
    expected, conditional = expected_sources(args.tree)
    uncovered = expected - jobs.keys() if not args.only else set()
    unconfigured = sorted(uncovered & (conditional | {LINUX_STUBS}))
    missing = sorted(uncovered - set(unconfigured))
    total = sum(map(len, jobs.values()))
    print(f'{len(jobs)} source files, {total} compile configurations, {min(len(jobs), job_count(args.jobs))} jobs')
    print(f'Tree {args.tree}\nCompile input {database}')
    for path in unconfigured:
        print(f'UNCONFIGURED {path}')
    if args.plan:
        print(f'clang-tidy checks: {CHECKS}\nclang++ warnings: {" ".join(WARNINGS)}')
        for path in missing:
            print(f'MISSING COMMAND {path}')
        if missing:
            print('The build directory has no compile command for these sources: configure it again '
                  '(cmake --preset ...) or a CMake option leaves them out.')
        print(f'VERDICT {"CHECK" if missing else "PLAN"}: {len(missing)} required source commands missing')
        return 2 if missing else 0
    if args.new_only:
        reviewed, accepted, reviewed_with = baseline_for(args.baseline)
        if not reviewed:
            raise ValueError(f'baseline review pending: {args.baseline}')
    else:
        accepted, reviewed_with = {}, None
    clang, tidy = locate_llvm(args.llvm_bin)
    args.clang, args.tidy = clang, tidy
    env = toolchain_env()
    verify_tools(args, env)
    print(f'LLVM {args.llvm_version} ({clang})')
    if reviewed_with and reviewed_with != f'LLVM {args.llvm_version}':
        print(f'NOTE the baseline was reviewed with {reviewed_with}: a finding that differs only by LLVM version '
              'is not a bug of yours; review it like any other')
    findings, errors = scan(args, jobs, env)
    classify(findings, accepted)
    counts = Counter((finding['tool'], finding['status']) for finding in findings)
    for tool in ('analyzer', 'lifetime', 'tidy'):
        print(f'{tool}: {counts[tool, "new"]} new, {counts[tool, "accepted"]} accepted')
    for finding in findings:
        if finding['status'] == 'new':
            print('NEW {path}:{line}:{column} [{tool}/{check}] {message} (id {short})'.format(short=finding['id'][:12], **finding))
    for path in missing:
        print(f'MISSING COMMAND {path}')
    for error in errors:
        print('TOOL ERROR {path} [{tool}] {error}'.format(**error))
    if missing or errors:
        verdict, code = 'CHECK', 2
    else:
        verdict, code = ('FAIL', 1) if any(f['status'] == 'new' for f in findings) else ('PASS', 0)
    summary = f'VERDICT {verdict}: {len(findings)} findings, {len(missing)} missing commands, {len(errors)} tool errors'
    if args.out:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        write_text(args.out, json.dumps({'profile': profile(), 'llvm': args.llvm_version,
                                         'source_tree': str(args.tree), 'database': str(database),
                                         'sources': len(jobs), 'configurations': total,
                                         'findings': findings, 'missing': missing, 'unconfigured': unconfigured,
                                         'errors': errors, 'summary': summary}, indent=2) + '\n')
    print(summary)
    return code


# -------------------------------------------------------------------------------- selftest

def identity_selftest(root, reference, tree):
    path = 'src/identity.cpp'
    anchor = 'const char* name = "two  spaces";'
    previous = reference / path
    current = tree / path
    write_text(previous, '  ' + anchor + '\n')
    write_text(current, '// added\n\t' + anchor + '\n')
    old_message = (f"read {reference.as_posix()}/src/identity.cpp:1:3 via "
                   r"'C:\old sdk\include\rule.h:3:4'; see local.h:5:6 at line 1, column 3 (size 8)")
    new_message = (f'read "{current}":2:8 via '
                   r'\\host\sdk\include\rule.h:8:9; see local.h:7:6 at line 2, column 8 (size 8)')
    check = '-Wlifetime-safety-use-after-free'

    def diagnostic(file, line, column, message):
        output = f'{file}:{line}:{column}: warning: {message} [{check}]\n {line} |    {anchor}\n'
        return next(find_diagnostics(output, file.parent.parent, root, 'lifetime'))

    old = diagnostic(previous, 1, 3, old_message)
    new = diagnostic(current, 2, 8, new_message)
    old_id = next(identify([old.copy()], reference))['id']
    assert next(identify([new.copy()], tree))['id'] == old_id
    normalized = 'read <path> via <path>; see local.h at <location>, <location> (size 8)'
    assert old_id == digest(path, check, normalized, anchor)
    assert finding_id(dict(new, message=new_message.replace('size 8', 'size 9')),
                      anchor, message_paths(tree)) != old_id
    assert finding_id(dict(new, check='different-check'), anchor, message_paths(tree)) != old_id
    assert finding_id(dict(new, path='src/other.cpp'), anchor, message_paths(tree)) != old_id

    stale = diagnostic(current, 1, 3, new_message)
    assert next(identify([stale.copy()], tree))['id'] == old_id
    write_text(current, '// added\n' + anchor + '\n' + anchor + '\n')
    assert next(identify([stale.copy()], tree))['id'] != old_id
    assert next(identify([new.copy()], tree))['id'] == old_id
    write_text(current, '// added\n' + anchor.replace('two  spaces', 'two spaces') + '\n')
    assert next(identify([new.copy()], tree))['id'] != old_id


def accept_selftest(root, tree):
    path = 'src/accept.cpp'
    write_text(tree / path, 'int a;\nint b;\n')
    findings = [dict(tool='tidy', check='bugprone-x', path=path, line=line, column=1, message='m', trace='')
                for line in (2, 1, 2)]
    findings[0]['id'] = 'a' * 64
    findings[1]['id'] = 'b' * 64
    findings[2]['id'] = 'a' * 64
    report = {'profile': profile(), 'findings': findings}
    data = {'version': BASELINE_VERSION, 'profile': profile(), 'reviewed': True, 'findings': []}
    added = accept_findings(data, report, ['aaaa', 'bbbb'], 'false-positive', ' It is a fixture. ')
    first, second = added['findings']
    assert (first['count'], first['lines'], first['reason'], second['count']) == (2, [2, 2], 'It is a fixture.', 1)
    assert baseline_counts(added) == {'a' * 64: 2, 'b' * 64: 1}
    # An id that is already there keeps its reason and category; only the count and the lines follow the report.
    report['findings'].append(dict(findings[1], line=2))
    again = accept_findings(added, report, ['bbbb'], '', '')
    assert [e['reason'] for e in again['findings']] == ['It is a fixture.'] * 2
    assert next(e for e in again['findings'] if e['id'] == 'b' * 64)['count'] == 2
    text = baseline_text(again)
    assert text.endswith('}\n') and '\r' not in text and json.loads(text)['version'] == BASELINE_VERSION
    for bad, error in ((dict(prefixes=['c'], category='style', reason='x'), 'matches 0'),
                       (dict(prefixes=[''], category='style', reason='x'), 'matches 2'),
                       (dict(prefixes=['a'], category='real-bug', reason='x'), '--category'),
                       (dict(prefixes=['a'], category='style', reason='  '), '--reason')):
        try:
            accept_findings(data, report, **bad)
        except ValueError as exc:
            assert error in str(exc), exc
        else:
            raise AssertionError(f'expected {error}')
    try:
        accept_findings(data, dict(report, profile=dict(profile(), llvm_min=1)), ['a'], 'style', 'x')
    except ValueError as exc:
        assert 'another analysis profile' in str(exc)
    else:
        raise AssertionError('accepted a report of another profile')


def selftest(args):
    with TemporaryDirectory(prefix='larecomp-static-test-') as temp:
        root = Path(temp).resolve()
        reference, tree, build = (root / name for name in ('build tree', 'checkout', 'build'))
        for folder in (reference / 'src', tree / 'src', build):
            folder.mkdir(parents=True)
        original = reference / 'src/probe.cpp'
        source = tree / 'src/probe.cpp'
        header = tree / 'src/checks/probe.h'
        header.parent.mkdir()
        stubs = tree / 'src/stubs'
        stubs.mkdir()
        write_text(stubs / 'priority.h', '// target-specific stub\n')
        write_text(tree / 'src/priority.h', '#error the configured stub must win\n')
        includes = '#include <checks/probe.h>\n#include <priority.h>\n'
        write_text(source, includes)
        write_text(header, 'inline int sample() { return 0; }\n')
        command = ['clang++', '-std=gnu++23', '-I', str(reference / 'src/stubs'), '-isystem',
                   str(build / 'generated'), '-MD', '-MT', 'old.obj', '-MF', 'old.d',
                   '-o', 'old.obj', '-c', str(original)]
        quoted = subprocess.list2cmdline(command) if os.name == 'nt' else shlex.join(command)
        assert command_line_args(quoted) == command
        entry = {'directory': str(build), 'file': str(original), 'command': quoted}
        excluded = [{'directory': str(build), 'file': f'{reference}/{folder}/probe.cpp', 'command': ''}
                    for folder in ('generated', 'thirdparty')]
        jobs = source_jobs([entry, entry, *excluded], reference, tree, None)
        assert list(jobs) == ['src/probe.cpp'] and len(jobs['src/probe.cpp']) == 1
        _, flags = jobs['src/probe.cpp'][0]
        assert flags == ('-I' + tree.as_posix(), '-std=gnu++23', '-I', (tree / 'src/stubs').as_posix(),
                         '-isystem', str(build / 'generated'), '-I' + (tree / 'src').as_posix(),
                         '-Wno-error', source.as_posix())

        analyzer = f'{header}:1:5: warning: sample [clang-analyzer-cplusplus.NewDelete]\n    note: trace\n'
        dependency = f'{reference}/src/probe.h:1:5: warning: external [bugprone-use-after-move]\n'
        compiler = f'{source}:1:5: warning: sample [-Wlifetime-safety-use-after-free]\n'
        redundant = f'{source}:1:5: warning: ignored [clang-diagnostic-return-stack-address]\n'
        found = list(find_diagnostics(analyzer + dependency + redundant, tree, build, 'tidy'))
        found += list(find_diagnostics(compiler, tree, build, 'lifetime'))
        assert len(found) == 2 and {f['tool'] for f in found} == {'analyzer', 'lifetime'}
        assert found[0]['trace'].endswith('note: trace')
        findings = list(identify(found + [found[0].copy()], tree))
        assert len(findings) == 2
        accepted_finding = next(f for f in findings if f['tool'] == 'analyzer')
        new_finding = next(f for f in findings if f['tool'] == 'lifetime')
        accepted_id = accepted_finding['id']
        entry = {'id': accepted_id, 'count': 1, 'category': 'false-positive', 'reason': 'Fixture only'}
        baseline = root / 'baseline.json'
        data = {'version': BASELINE_VERSION, 'profile': profile(), 'reviewed': True,
                'reviewed_with': 'LLVM 23.0.0', 'findings': [entry]}
        write_text(baseline, json.dumps(data))
        reviewed, accepted, reviewed_with = baseline_for(baseline)
        assert reviewed and accepted == {accepted_id: 1} and reviewed_with == 'LLVM 23.0.0'
        repeats = [new_finding, accepted_finding, accepted_finding.copy()]
        classify(repeats, accepted)
        assert [f['status'] for f in repeats] == ['new', 'accepted', 'new']
        write_text(header, '\ninline int sample() { return 0; }\n')
        shifted = dict(accepted_finding, line=2)
        assert next(identify([shifted], tree))['id'] == accepted_id
        write_text(header, '\ninline int sample() { return 1; }\n')
        assert next(identify([shifted], tree))['id'] != accepted_id
        data['reviewed'] = False
        write_text(baseline, json.dumps(data))
        assert baseline_for(baseline)[0] is False
        for key, value in (('reason', ' '), ('category', 'real-bug'), ('count', 0)):
            data['findings'] = [dict(entry, **{key: value})]
            write_text(baseline, json.dumps(data))
            try:
                baseline_for(baseline)
            except ValueError:
                continue
            raise AssertionError(f'accepted invalid baseline {key}')

        identity_selftest(root, reference, tree)
        accept_selftest(root, tree)

        # The source list reads the tree's own CMake files.
        cmake = tree / 'CMakeLists.txt'
        write_text(cmake, 'set(LARECOMP_SOURCES\n  src/probe.cpp # note\n  src/missing.cpp\n  "src/stubs/a.cpp"\n)\n'
                          'list(APPEND LARECOMP_SOURCES src/compat/linux/x.cpp)\n')
        write_text(tree / 'src/stubs/a.cpp', '\n')
        write_text(tree / 'src/cpu_level.cpp', '\n')
        (tree / LINUX_STUBS).parent.mkdir(parents=True, exist_ok=True)
        write_text(tree / LINUX_STUBS, '\n')
        sources, conditional = expected_sources(tree)
        assert sources == {'src/probe.cpp', 'src/stubs/a.cpp', 'src/cpu_level.cpp', LINUX_STUBS} and not conditional, sources

        # The default build directory is the only one, else the only relwithdebinfo one, else the message says which.
        def default_error():
            try:
                default_build(tree)
            except ValueError as exc:
                return str(exc)
            raise AssertionError('default_build accepted the checkout')

        assert 'no configured build' in default_error()
        for name in ('win-amd64-debug', 'win-amd64-relwithdebinfo'):
            (tree / 'out/build' / name).mkdir(parents=True)
            write_text(tree / 'out/build' / name / 'CMakeCache.txt', '\n')
            if name == 'win-amd64-debug':
                assert default_build(tree) == tree / 'out/build' / name
        assert default_build(tree) == tree / 'out/build/win-amd64-relwithdebinfo'
        (tree / 'out/build/linux-amd64-relwithdebinfo').mkdir()
        write_text(tree / 'out/build/linux-amd64-relwithdebinfo/CMakeCache.txt', '\n')
        assert 'several build directories' in default_error()

        # The LLVM lookup says what is missing and where to get it.
        saved = os.environ.pop(LLVM_ENV, None)
        try:
            try:
                locate_llvm(root / 'no-llvm-here')
            except ValueError as exc:
                assert 'not found' in str(exc) and LLVM_ENV in str(exc) and 'releases' in str(exc)
            else:
                raise AssertionError('accepted a folder without LLVM')
        finally:
            if saved is not None:
                os.environ[LLVM_ENV] = saved

        if args.probe_tools:
            write_text(header, 'inline int leak() { int* p = new int(1); delete p; return *p; }\n')
            write_text(source, includes + 'int use() { return leak(); }\n'
                               'void suspicious(bool b) { if (b); {} }\n')
            args.tree = tree
            args.clang, args.tidy = locate_llvm(args.llvm_bin)
            env = toolchain_env()
            verify_tools(args, env)
            print(f'LLVM {args.llvm_version} ({args.clang})')
            actual, errors = scan(args, jobs, env)
            assert not errors, errors
            checks = {(item['tool'], item['check']) for item in actual}
            assert ('lifetime', '-Wlifetime-safety-use-after-free') in checks, actual
            assert ('analyzer', 'clang-analyzer-cplusplus.NewDelete') in checks, actual
            assert ('tidy', 'bugprone-suspicious-semicolon') in checks, actual
    print('static_analysis selftest PASS' + (' (three LLVM diagnostic families)' if args.probe_tools else ''))
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('mode', choices=['selftest', 'accept'], nargs='?',
                        help='selftest: check the tool itself; accept: add reviewed findings of a scan (--report) to the baseline')
    parser.add_argument('-Plan', '--plan', action='store_true', help='list what would be analysed and stop')
    parser.add_argument('--tree', type=Path, default=REPO, help='the checkout to analyse (default: this repository)')
    parser.add_argument('--build', type=Path, help='configured, built CMake build directory (default: the only one under out/build)')
    parser.add_argument('--llvm-bin', type=Path, help=f'bin folder of LLVM {MIN_LLVM} or newer (default: ${LLVM_ENV}, then PATH)')
    parser.add_argument('--jobs', type=int, help='parallel jobs (default 4), always bounded by free RAM (about 4 GB each) and CPU count')
    parser.add_argument('--timeout', type=int, default=180, help='seconds per compiler or clang-tidy run')
    parser.add_argument('--only', help='analyse one file, as a path relative to --tree with forward slashes')
    parser.add_argument('--new-only', action='store_true', help='accept the findings of the reviewed baseline; fail only for new ones')
    parser.add_argument('--baseline', type=Path, default=HERE / 'baseline.json', help='reviewed findings (default: baseline.json next to this script)')
    parser.add_argument('--out', type=Path, help='write all findings with their traces and the tool errors to this JSON file')
    parser.add_argument('--report', type=Path, help='accept mode: the scan file made with --out')
    parser.add_argument('--id', action='append', default=[], help='accept mode: finding id (a unique prefix of 8 or more characters); repeat for several')
    parser.add_argument('--category', choices=CATEGORIES, help='accept mode: why the finding is not a bug')
    parser.add_argument('--reason', help='accept mode: the reason, in a sentence (required)')
    parser.add_argument('--probe-tools', action='store_true', help='selftest: also run LLVM on one synthetic source')
    args = parser.parse_args()
    if args.mode == 'selftest':
        try:
            return selftest(args)
        except (OSError, ValueError, subprocess.SubprocessError) as exc:
            print(f'VERDICT CHECK: {exc}', file=sys.stderr)
            return 2
    if args.mode == 'accept':
        if not args.report or not args.id or any(len(prefix) < 8 for prefix in args.id):
            parser.error('accept needs --report SCAN.json and at least one --id of 8 or more characters')
        try:
            report = json.loads(args.report.read_text(encoding='utf-8-sig'))
            data = accept_findings(read_baseline(args.baseline), report, args.id, args.category or '', args.reason or '')
            if report.get('llvm'):
                data.setdefault('reviewed_with', f'LLVM {report["llvm"]}')
            write_text(args.baseline, baseline_text(data))
        except (OSError, ValueError, KeyError) as exc:
            print(f'VERDICT CHECK: {exc}', file=sys.stderr)
            return 2
        print(f'{args.baseline}: {len(data["findings"])} reviewed entries')
        return 0
    if (args.jobs is not None and args.jobs < 1) or args.timeout < 1:
        parser.error('--jobs and --timeout must be positive')
    args.tree = args.tree.resolve()
    try:
        args.build = (args.build or default_build(args.tree)).resolve()
        return evaluate(args)
    except (OSError, ValueError, KeyError, StopIteration, subprocess.SubprocessError) as exc:
        print(f'VERDICT CHECK: {exc}', file=sys.stderr)
        return 2


if __name__ == '__main__':
    sys.exit(main())
