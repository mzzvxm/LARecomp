"""Configure a synthetic runtime tree, without building the game or SDK."""

import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile


ROOT = Path(__file__).resolve().parents[2]
CONTRACT = ROOT / 'documentation/contract'
VALIDATOR = CONTRACT / 'validate_contract.py'
PIPELINE_PREBUILD = 'src/gpu/pipeline_prebuild.cpp'
MSPACK_SIDECARS = ('larecomp_mspack_license.txt', 'larecomp_mspack_notice.txt')
CONSTANT_SOURCES = (
    'larecomp_manifest.toml', 'src/cpu_level.cpp', 'src/larecomp_version.h',
    'src/settings/larecomp_app_settings.h',
    'src/isoinstaller/larecomp_iso_installer.cpp', 'src/saveporter/larecomp_save_porter.cpp',
    'src/mc_engine/online/online_tables_core.h', 'src/mc_engine/online/server_browser/join_server.cpp',
    'src/shaderpack/pack_format.h',
    'src/citypack/pack_format.h', 'src/citypack/city_hit_format.h', 'src/gpu/pipelines.cpp', PIPELINE_PREBUILD,
    'launcher-recipe.json', 'src/settings_ui/ui_data.h.in',
)
# After the real CMakeLists.txt lines that hand min_launcher to the exe (settings_ui_data.h for settings-ui.json, a compile definition of
# machine_facts.cpp for machine.json), the fixture records what they produced.
WIRING_REPORT = '''
get_source_file_property(larecomp_wired_definitions src/machine_facts/machine_facts.cpp COMPILE_DEFINITIONS)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/exe-definitions.txt" "${larecomp_wired_definitions}")
'''
BAD_MIN_LAUNCHERS = ('1.x', '', '1.2.3.4.5', ' 1.0', 'v1.0', 7, None)
FIXTURE = '''
cmake_minimum_required(VERSION 3.25)
project(ManifestFixture NONE)
set(CMAKE_CXX_COMPILE_OBJECT "${CMAKE_COMMAND} -E touch <OBJECT>")
set(CMAKE_CXX_LINK_EXECUTABLE "${CMAKE_COMMAND} -E touch <TARGET>")
set(CMAKE_CXX_CREATE_SHARED_LIBRARY "${CMAKE_COMMAND} -E touch <TARGET>")
set(WIN32 ${fixture_windows})
set(CMAKE_EXECUTABLE_SUFFIX "${fixture_suffix}")
set(CMAKE_SHARED_LIBRARY_PREFIX "${fixture_lib_prefix}")
set(CMAKE_SHARED_LIBRARY_SUFFIX "${fixture_lib_suffix}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/runtime")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
add_executable(larecomp dummy.cpp)
add_executable(larecomp_online_tool dummy.cpp)
set_target_properties(larecomp larecomp_online_tool PROPERTIES LINKER_LANGUAGE CXX)
set_target_properties(larecomp larecomp_online_tool PROPERTIES SUFFIX "${fixture_suffix}")
if(fixture_gpu)
    add_custom_target(larecomp_gpu)
endif()
if(fixture_exports)
    set_target_properties(larecomp PROPERTIES LARECOMP_EXPORTS_SETTINGS_UI TRUE LARECOMP_PROBES_MACHINE TRUE)
endif()
function(rexglue_get_runtime_files target out)
    set(module "$<TARGET_FILE_DIR:${target}>/sdk_module${fixture_lib_suffix}")
    set(${out} "${module};${module}" PARENT_SCOPE)
endfunction()
# Capture the copy rules without building the fixture targets.
function(add_custom_command)
    file(APPEND "${CMAKE_CURRENT_BINARY_DIR}/copies.txt" "${ARGV}\n")
endfunction()
if(fixture_windows AND fixture_gpu)
    set(LARECOMP_FFX_KIT "${CMAKE_CURRENT_SOURCE_DIR}/kit/Kits/FidelityFX")
    set(LARECOMP_STREAMLINE_KIT "${CMAKE_CURRENT_SOURCE_DIR}/kit")
    set(LARECOMP_FFX_HAS_FRAMEGENERATION ${fixture_full})
    set(LARECOMP_FFX_HAS_DENOISER TRUE)
    set(LARECOMP_STREAMLINE_HAS_FRAMEGEN ${fixture_full})
    set(LARECOMP_STREAMLINE_HAS_RR ${fixture_full})
    set(LARECOMP_GPU_BACKENDS VULKAN)
    if(fixture_full)
        list(APPEND LARECOMP_GPU_BACKENDS D3D12)
    endif()
else()
    set(LARECOMP_DXC_LINUX_DIR "${CMAKE_CURRENT_SOURCE_DIR}/dxc-kit" CACHE PATH "")
endif()
set_source_files_properties(thirdparty/libmspack/libmspack/mspack/lzxd.c PROPERTIES HEADER_FILE_ONLY ON)
include(setup-stage.cmake)
set_target_properties(larecomp_mspack PROPERTIES LINKER_LANGUAGE CXX)
include(game-stage.cmake)
include(exe-wiring.cmake)
set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/lfg/describe_json.cpp" PROPERTIES HEADER_FILE_ONLY ON)
'''


def run(args, *, error=None):
    result = subprocess.run(args, capture_output=True, text=True, encoding='utf-8', timeout=45)
    output = result.stdout + result.stderr
    if error is not None:
        assert result.returncode and error in ' '.join(output.split()), output
    elif result.returncode:
        raise AssertionError(output)
    return output


def read(path):
    return json.loads(path.read_text(encoding='utf-8'))


def pipeline_key_settings(source):
    settings = []
    for line in (source / PIPELINE_PREBUILD).read_text(encoding='utf-8').splitlines():
        if line.strip().startswith('LARECOMP_PIPELINE_KEY_LINE('):
            names = line.split('(', 2)[2].split(')', 1)[0]
            settings.extend(name.strip() for name in names.split(','))
    assert settings
    return list(dict.fromkeys(settings))


def write(path, contents='fixture\n'):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(contents, encoding='utf-8', newline='\n')


def check_deps():
    sys.dont_write_bytecode = True
    spec = importlib.util.spec_from_file_location('validate_contract', VALIDATOR)
    validator = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(validator)
    schema = read(CONTRACT / 'schemas/deps.schema.json')
    assert validator.schema_keywords(schema) <= validator.SUPPORTED_KEYWORDS
    for path in (ROOT / 'cmake/deps.json', CONTRACT / 'examples/deps/deps.example.json'):
        data = read(path)
        assert not (errors := list(validator.schema_errors(data, schema, schema))), errors
        for kit in data['kits'].values():
            paths = [entry['path'] for entry in kit['files']]
            assert len(paths) == len(set(paths))
            assert {entry['group'] for entry in kit['files']} == set(kit['groups'])
            report = validator.Report()
            validator.lint_recipe({'steps': [
                {'id': group, 'when': values['when']}
                for group, values in kit['groups'].items() if 'when' in values
            ]}, report, validator.FactTypes(validator.dictionary()), strict=True)
            assert not report.errors and not report.warnings, report
            check_gpu_names(kit)
    streamline = read(ROOT / 'cmake/deps.json')['kits']['streamline']['groups']
    assert all('gpu_name' in streamline[group] and 'gpu_vendor' in streamline[group] for group in ('core', 'frame_generation'))
    # The pins file is read by hand-edited CMake and is not generated: its min_launcher is a static copy of the recipe's, so a bump that
    # forgets it fails here.
    recipe = read(ROOT / 'launcher-recipe.json')
    assert read(ROOT / 'cmake/deps.json').get('min_launcher') == recipe.get('min_launcher'), (
        'cmake/deps.json has another min_launcher than launcher-recipe.json (the one place to edit): copy it there')
    return read(ROOT / 'cmake/deps.json')


def check_gpu_names(kit):
    # A group's `gpu_vendor` and `gpu_name` pattern give the answers the name tests of the game's code give; its `when` names the same vendor
    # (src/gpu/dlss_card_table_test.cpp checks the same vectors against that code and evaluates the `when` itself).
    vectors = read(CONTRACT / 'adapter-name-vectors.json')['vectors']
    answers = {'core': 'rtx', 'frame_generation': 'dlss_card'}
    for group, values in kit['groups'].items():
        if 'gpu_name' not in values:
            continue
        pattern = values['gpu_name']
        assert re.fullmatch(r'[A-Za-z0-9 |*+?\[\]-]{1,200}', pattern), pattern
        assert group in answers, f'no vectors for the {group} group'
        assert 'gpu_vendor' in values, f'the {group} group has a gpu_name and no gpu_vendor'
        vendor = values['gpu_vendor']
        assert f"gpu.vendor == '{vendor}'" in values['when'], '`when` and `gpu_vendor` of a group name the same vendor'
        for vector in vectors:
            applies = vector['vendor'] == vendor and re.search(pattern, vector['name'], re.IGNORECASE) is not None
            assert applies == vector[answers[group]], (group, pattern, vector['name'])


def prepare_tree(source, deps):
    for filename in CONSTANT_SOURCES:
        write(source / filename, (ROOT / filename).read_text(encoding='utf-8'))
    for path in (ROOT / 'cmake').iterdir():
        if path.is_file():
            write(source / 'cmake' / path.name, path.read_text(encoding='utf-8'))
    for filename in (
        'dummy.cpp', 'src/assets/fake.own', 'assets/gamecontrollerdb.txt', 'assets/mcla_pipelines.bin',
        'assets/D3D12/LICENCE-MIT.txt', 'src/achievements.toml', 'src/keycaps/key.png',
        'dxc-kit/lib/libdxcompiler.so', 'dxc-kit/LICENSE-LLVM.txt',
        'thirdparty/libmspack/libmspack/mspack/lzxd.c',
    ):
        write(source / filename)
    (source / 'LICENSES').mkdir()
    for name in MSPACK_SIDECARS:
        shutil.copyfile(ROOT / 'LICENSES' / name, source / 'LICENSES' / name)
    for kit in deps['kits'].values():
        for item in kit['files']:
            write(source / 'kit' / item['path'])
    cmakelists = (ROOT / 'CMakeLists.txt').read_text(encoding='utf-8')
    setup = cmakelists.split('    # BS-G: Record the SDK', 1)[1].split('    # Third-party code', 1)[0]
    mspack = cmakelists.split('    # libmspack (LGPL-2.1', 1)[1].split('    target_link_libraries(larecomp PRIVATE larecomp_mspack)', 1)[0]
    stage = cmakelists.split('    # BS-G game runtime staging.', 1)[1].split('    # Floating-point contraction', 1)[0]
    write(source / 'setup-stage.cmake', '#' + setup + '#' + mspack)
    write(source / 'game-stage.cmake', '#' + stage)
    # The lines that give min_launcher to settings-ui.json (settings_ui_data.h) and machine.json (a compile definition), as they are.
    header = re.search(r'^ *set\(GX_MIN_LAUNCHER [^\n]*\n *configure_file\(src/settings_ui/ui_data\.h\.in settings_ui_data\.h @ONLY\)\n',
                       cmakelists, re.M)
    definition = re.search(r'^ *if\(NOT "\$\{LARECOMP_MIN_LAUNCHER\}" STREQUAL ""\)\n.*?^ *endif\(\)\n', cmakelists, re.M | re.S)
    assert header and definition, 'CMakeLists.txt no longer hands min_launcher to the settings-ui.json and machine.json writers'
    assert 'machine_facts.cpp' in definition.group(0)
    write(source / 'exe-wiring.cmake', header.group(0) + definition.group(0) + WIRING_REPORT)
    write(source / 'CMakeLists.txt', FIXTURE)


def configure(cmake, source, output, *, windows, mingw=False, gpu=True, full=False, exports=False, error=None):
    write(output / 'runtime/assets/default.xex', 'synthetic codegen input')
    write(output / 'copies.txt', '')
    run([cmake, '-S', str(source), '-B', str(output), '-G', 'Ninja',
         f'-Dfixture_windows={int(windows)}', f'-Dfixture_gpu={int(gpu)}',
         f'-Dfixture_full={int(full)}', f'-Dfixture_exports={int(exports)}',
         '-Dfixture_suffix=' + ('.exe' if windows else ''),
         '-Dfixture_lib_prefix=' + ('lib' if mingw or not windows else ''),
         '-Dfixture_lib_suffix=' + ('.dll' if windows else '.so')], error=error)
    if error is not None:
        return None
    path = output / 'runtime/install-manifest.json'
    describe = output / 'describe.json'
    embedded = (output / 'lfg/describe_json.cpp').read_text(encoding='utf-8')
    write(describe, embedded.split('R"lfg(', 1)[1].split(')lfg"', 1)[0])
    run([sys.executable, '-I', str(VALIDATOR), '--strict', str(path), str(describe)])
    return read(path), read(describe)


def check_min_launcher_outputs(out, manifest, describe, expected):
    """expected: the recipe's min_launcher, or None when the recipe has none. Every file a configure makes carries it, or none does."""
    keys = list(manifest)
    if expected is None:
        assert 'min_launcher' not in manifest and 'min_launcher' not in describe
    else:
        assert manifest['min_launcher'] == describe['min_launcher'] == expected, (manifest.get('min_launcher'), describe.get('min_launcher'), expected)
        assert keys[keys.index('min_reader') + 1] == 'min_launcher'
        assert list(describe)[list(describe).index('schema_version') + 1] == 'min_launcher'
    # settings-ui.json reads it from settings_ui_data.h, machine.json from a compile definition of machine_facts.cpp.
    header = (out / 'settings_ui_data.h').read_text(encoding='utf-8')
    assert f'kSettingsUiMinLauncher = "{expected or ""}";' in header, header
    definitions = (out / 'exe-definitions.txt').read_text(encoding='utf-8')
    assert definitions == ('NOTFOUND' if expected is None else f'LARECOMP_MIN_LAUNCHER="{expected}"'), definitions


def check_min_launcher(cmake, source, out):
    """The recipe's min_launcher in every writer: a given value is copied, none stays none, a malformed one stops the configure."""
    path = source / 'launcher-recipe.json'
    original = path.read_text(encoding='utf-8')
    recipe = json.loads(original)
    try:
        for name, value in (('copied', '2.5.0.7'), ('one-number', '12'), ('zero', '0'), ('absent', None)):
            changed = dict(recipe)
            changed.pop('min_launcher', None)
            if value is not None:
                changed['min_launcher'] = value
            write(path, json.dumps(changed, indent=2))
            manifest, describe = configure(cmake, source, out / name, windows=True, exports=True)
            check_min_launcher_outputs(out / name, manifest, describe, value)
        for i, value in enumerate(BAD_MIN_LAUNCHERS):
            write(path, json.dumps({**recipe, 'min_launcher': value}, indent=2))
            configure(cmake, source, out / f'bad-{i}', windows=True, exports=True,
                      error='launcher-recipe.json: min_launcher must be ' + ('a text' if not isinstance(value, str) else '1 to 4 numbers'))
        # A recipe that is not JSON must not read as "no requirement".
        write(path, original.replace('"min_launcher": ', '"min_launcher" ', 1))
        configure(cmake, source, out / 'not-json', windows=True, exports=True, error='launcher-recipe.json is not valid JSON')
    finally:
        write(path, original)


def check_runtime(cmake, source, out, *, windows, mingw=False, gpu=True, full=False, exports=False):
    manifest, describe = configure(cmake, source, out, windows=windows, mingw=mingw, gpu=gpu, full=full, exports=exports)
    # The recipe at the repo root is the one place min_launcher is edited: every file of the build carries its value.
    check_min_launcher_outputs(out, manifest, describe, read(ROOT / 'launcher-recipe.json').get('min_launcher'))
    stage = {entry['from']: entry for entry in manifest['stage']}
    action = {entry['id']: entry for entry in manifest['actions']}
    assert len(stage) == len(manifest['stage'])
    assert len(action) == (7 if exports else 5)
    assert list(action) == describe['actions']
    for field in ('game_version', 'commit', 'contracts'):
        assert manifest[field] == describe[field]
    assert ('settings_ui' in manifest['contracts']) == exports
    assert ('machine' in manifest['contracts']) == exports
    assert manifest['pipeline_key_settings'] == pipeline_key_settings(ROOT)
    # In the fixture this source is only an input of the configure-time exporter.
    assert PIPELINE_PREBUILD in (out / 'build.ninja').read_text(encoding='utf-8').replace('\\', '/')
    budgets = {
        'build_shader_packs': (25, 260), 'precache_pipelines': (20, 130),
        'build_city_pack': (25, 300), 'build_city_hit': (25, 260), 'check_city_hit': (5, 0),
    }
    if exports:
        budgets.update(dump_settings_ui=(1, 1), probe_machine=(1, 1))
    assert {name: (entry['timeout_min'], entry['estimated_disk_mib'])
            for name, entry in action.items()} == budgets
    assert manifest['exit_codes']['cpu_level_refused'] == 0x4C43
    program = 'larecomp.exe' if windows else 'larecomp'
    assert manifest['launch'] == {'program': program, 'game_root': '--game_data_root={game_root}', 'debug_menu': '--debug_menu=true'}
    assert stage[program]['kind'] == 'program'
    assert manifest['game']['size'] == 9252864
    assert manifest['game']['required_files'] == [
        'default.xex', 'xarchive_audio.rpf', 'xarchive_audlo.rpf', 'xarchive_cache.rpf',
    ]
    assert manifest['markers'] == {'algo': 'fnv1a64-lower-path', 'version': 1}
    assert manifest['files']['join_status'] == 'logs/join_server.txt'
    assert manifest['files']['online_tool'] == ('larecomp_online_tool.exe' if windows else 'larecomp_online_tool')
    assert manifest['files']['online_tables'] == 'logs/online_tables.json'
    stdout = {
        'build_shader_packs': ('shaderpack', ['ok', 'up to date', 'not needed']),
        'precache_pipelines': ('pipelines', ['ok', 'up to date', 'not needed', 'no list']),
        'build_city_pack': ('citypack', ['ok', 'up to date', 'not needed']),
        'build_city_hit': ('citypack', ['ok', 'up to date', 'not needed']),
        'check_city_hit': ('citypack', ['up to date', 'not needed']),
    }
    if exports:
        stdout.update(dump_settings_ui=('settingsui', ['ok']), probe_machine=('machine', ['ok']))
        for key, filename, step in (('machine', 'machine.json', 'probe_machine'),
                                    ('settings_ui', 'settings-ui.json', 'dump_settings_ui')):
            assert manifest['files'][key] == filename
            assert action[step]['outputs'] == [f'{{game_folder}}/{filename}']
    else:
        assert 'machine' not in manifest['files'] and 'settings_ui' not in manifest['files']
    for step, (prefix, statuses) in stdout.items():
        assert action[step]['stdout'] == {'prefix': f'@{prefix}|', 'grammar': 1, 'ok_results': statuses}
    extension = '.dll' if windows else '.so'
    assert stage['sdk_module' + extension]['kind'] == 'module'
    library = ('lib' if mingw or not windows else '') + 'larecomp_mspack' + extension
    assert stage[library] == {'from': library, 'to': library, 'kind': 'dll', 'required': True}
    for name in MSPACK_SIDECARS:
        assert stage[name] == {'from': name, 'to': name, 'kind': 'licence', 'required': True, 'licence_for': library}
    assert stage['assets/fake.own']['to'] == 'assets/fake.own'
    assert 'assets/**' not in stage and 'assets/default.xex' not in stage
    assert stage['gamecontrollerdb.txt']['required']
    assert stage['install-manifest.json']['required']
    assert ('D3D12/**' in stage) == windows
    assert ('keycaps/**' in stage) != windows
    assert ('D3D12/libdxcompiler.so' in stage) != windows
    assert ('assets/achievements.toml' in stage) != windows
    if windows and gpu:
        assert not stage['sl.interposer.dll']['required']
        assert 'amd_fidelityfx_loader_vk.dll' in stage
        assert ('amd_fidelityfx_loader_dx12.dll' in stage) == full
        assert ('amd_fidelityfx_denoiser_dx12.dll' in stage) == full
        assert ('nvngx_dlssg.dll' in stage) == full
        assert ('nvngx_dlssd.dll' in stage) == full
        assert ('dlss_ray_reconstruction' in manifest['features']) == full
        assert ('ray_regeneration' in manifest['features']) == full
    else:
        assert 'sl.interposer.dll' not in stage
        assert 'amd_fidelityfx_loader_vk.dll' not in stage
    assert set(manifest['facts']['install.streamline_frame_generation']['all_of']) == {
        'sl.dlss_g.dll', 'nvngx_dlssg.dll', 'sl.reflex.dll', 'sl.pcl.dll',
    }
    assert action['build_shader_packs']['when'] == ('true' if gpu else 'false')
    assert action['build_shader_packs']['args'] == [manifest['launch']['game_root']]
    assert action['build_city_pack']['when'] == ("true" if gpu else "false") + " && gpu.rt_tier != 'none'"
    assert action['build_city_pack']['after'] == ['build_shader_packs'] + (['probe_machine'] if exports else [])
    assert action['build_city_hit']['after'] == ['build_city_pack']
    assert action['check_city_hit']['args'][-1] == '--check_city_hit_deep=true'
    assert action['precache_pipelines']['outputs'] == [
        '{pipeline_cache_dir}/mcla_gpu_pipelines_{api}.bin', '{pipeline_cache_dir}/mcla_gpu_pipelines_{api}.stamp',
    ]
    assert action['build_shader_packs']['outputs'][0] == '{shader_pack_dir}/mcla_shaders.fixture.pack'
    assert action['build_city_hit']['outputs'][-1] == '{shader_pack_dir}/city/mcla_city_hit.stamp'
    copies = (out / 'copies.txt').read_text(encoding='utf-8')
    assert re.search(r'copy_if_different;[^;]+/src/assets/fake\.own;\$<TARGET_FILE_DIR:larecomp>/assets/fake\.own;', copies)
    for name in MSPACK_SIDECARS:
        command = f'copy_if_different;{(source / "LICENSES" / name).as_posix()};$<TARGET_FILE_DIR:larecomp_mspack>/{name};'
        assert sum(line.startswith('TARGET;larecomp_mspack;POST_BUILD;') and command in line
                   for line in copies.splitlines()) == 1, command


def check_mspack_sidecars(cmake, source, out):
    for name in MSPACK_SIDECARS:
        path = source / 'LICENSES' / name
        path.unlink()
        try:
            configure(cmake, source, out / name, windows=False, error=f'LICENSES/{name} is missing')
        finally:
            shutil.copyfile(ROOT / 'LICENSES' / name, path)


def check_pipeline_key_export(cmake, source, out):
    # A new setting in the stamp source must flow into the manifest without changing CMake.
    path = source / PIPELINE_PREBUILD
    original = path.read_text(encoding='utf-8')
    updated, count = re.subn(r'(?m)^(\s*LARECOMP_PIPELINE_KEY_LINE\(samples, \([^\n)]+)\)',
                              r'\1, fixture_setting, graphics_backend)', original)
    assert count == 1
    write(path, updated)
    try:
        manifest, _ = configure(cmake, source, out, windows=True)
        assert manifest['pipeline_key_settings'] == pipeline_key_settings(ROOT) + ['fixture_setting']
    finally:
        write(path, original)


def check_fetchers(cmake, source, out, deps):
    # The readers use their normal cache validation with synthetic bytes.
    readers = {'fidelityfx': 'fidelityfx-sdk', 'streamline': 'streamline', 'dxc_host': 'dxc-host'}
    deps['kits']['dxc_host']['version'] = 'fixture'
    for kit_id, kit in deps['kits'].items():
        if kit_id not in readers:
            continue  # A kit with no CMake reader in this tree.
        kit['url'] = (source / 'offline').as_uri()
        folder = readers[kit_id]
        for member in kit['files']:
            path = out / '_deps' / f'{folder}-{kit["version"]}' / member['path']
            write(path)
            member['sha256'] = hashlib.sha256(path.read_bytes()).hexdigest()
    write(source / 'cmake/deps.json', json.dumps(deps))
    write(source / 'fetch-fixture.cmake', '''
set(LARECOMP_STREAMLINE_ZIP "${CMAKE_CURRENT_LIST_DIR}/offline.zip")
include("${CMAKE_CURRENT_LIST_DIR}/cmake/FetchFidelityFX.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/cmake/FetchStreamline.cmake")
if(NOT LARECOMP_FFX_SDK_DIR OR NOT LARECOMP_STREAMLINE_DIR)
    message(FATAL_ERROR "Both kits must recognize their caches")
endif()
''')
    write(source / 'CMakeLists.txt', '''
cmake_minimum_required(VERSION 3.25)
project(FetchFixture NONE)
if(fixture_check_other_kits)
    include(fetch-fixture.cmake)
endif()
set(CMAKE_HOST_WIN32 TRUE)
set(CMAKE_HOST_SYSTEM_NAME Windows)
set(CMAKE_HOST_SYSTEM_PROCESSOR AMD64)
if(fixture_linux)
    set(CMAKE_HOST_WIN32 FALSE)
    set(CMAKE_HOST_SYSTEM_NAME Linux)
    set(CMAKE_SYSTEM_NAME Linux)
    set(CMAKE_HOST_SYSTEM_PROCESSOR x86_64)
    set(CMAKE_SYSTEM_PROCESSOR x86_64)
endif()
set(DIRECTX_DXC_TOOL fixture-dxc-bin CACHE INTERNAL "")
include(cmake/FetchDxcHost.cmake)
if(NOT LARECOMP_DXC_HOST_TOOL STREQUAL fixture_dxc_tool OR NOT DIRECTX_DXC_TOOL STREQUAL "fixture-dxc-bin")
    message(FATAL_ERROR "Expected ${fixture_dxc_tool}, got ${LARECOMP_DXC_HOST_TOOL}, dxc-bin=${DIRECTX_DXC_TOOL}")
endif()
if(fixture_linux AND NOT LARECOMP_DXC_LINUX_LIB STREQUAL "${LARECOMP_DXC_LINUX_DIR}/lib/libdxcompiler.so")
    message(FATAL_ERROR "The Linux host lookup must also resolve the runtime library")
endif()
''')
    configure_dxc(cmake, source, out, (out / '_deps/dxc-host-fixture/bin/x64/dxc.exe').as_posix(),
                  '-Dfixture_check_other_kits=ON')


def configure_dxc(cmake, source, out, expected, *options, error=None):
    # Use a regular configure so ReadDeps records its configure dependency.
    return run([cmake, '-S', str(source), '-B', str(out), '-G', 'Ninja',
                f'-Dfixture_dxc_tool={expected}', *options], error=error)


def check_dxc_host(cmake, source, out, deps):
    kit = deps['kits']['dxc_host']
    archive = source / 'fixture-dxc.zip'
    with zipfile.ZipFile(archive, 'w') as zip_file:
        for member in kit['files']:
            zip_file.writestr(member['path'], b'fixture\n')
        zip_file.writestr('LICENSE-LLVM.txt', 'fixture licence\n')
    kit['url'] = archive.as_uri()
    kit['zip_size'] = archive.stat().st_size
    kit['zip_sha256'] = hashlib.sha256(archive.read_bytes()).hexdigest()
    write(source / 'cmake/deps.json', json.dumps(deps))

    fallback = 'fixture-dxc-bin'
    fetch_off = '-DLARECOMP_DXC_HOST_FETCH=OFF'
    local_zip = f'-DLARECOMP_DXC_HOST_ARCHIVE={archive.as_posix()}'
    local = out / 'local'
    local_tool = local / '_deps/dxc-host-fixture/bin/x64/dxc.exe'
    configure_dxc(cmake, source, local, local_tool.as_posix(), local_zip, fetch_off)
    configure_dxc(cmake, source, local, fallback, '-DLARECOMP_DXC_HOST_ARCHIVE=')
    configure_dxc(cmake, source, out / 'unpacked', (source / 'kit/bin/x64/dxc.exe').as_posix(),
                  f'-DLARECOMP_DXC_HOST_DIR={(source / "kit").as_posix()}', fetch_off)

    fetch = out / 'fetch'
    fetched_root = fetch / '_deps/dxc-host-fixture'
    fetched_tool = (fetched_root / 'bin/x64/dxc.exe').as_posix()
    configure_dxc(cmake, source, fetch, fetched_tool)
    write(fetched_root / 'bin/x64/dxcompiler.dll', 'corrupt cache')
    configure_dxc(cmake, source, fetch, fetched_tool)
    assert (fetched_root / 'bin/x64/dxcompiler.dll').read_bytes() == b'fixture\n'

    linux_dir = (source / 'dxc-kit').as_posix()
    linux_options = ('-Dfixture_linux=ON', f'-DLARECOMP_DXC_LINUX_DIR={linux_dir}')
    write(source / 'dxc-kit/bin/dxc')
    configure_dxc(cmake, source, out / 'linux', f'LD_LIBRARY_PATH={linux_dir}/lib;{linux_dir}/bin/dxc', *linux_options)
    (source / 'dxc-kit/bin/dxc').unlink()
    configure_dxc(cmake, source, out / 'linux', fallback, *linux_options)

    valid_zip_sha = kit['zip_sha256']
    kit['zip_sha256'] = '0' * 64
    write(source / 'cmake/deps.json', json.dumps(deps))
    configure_dxc(cmake, source, out / 'bad-zip', fallback, local_zip, error='does not match its SHA-256')
    kit['zip_sha256'] = valid_zip_sha
    kit['files'][-1]['sha256'] = '0' * 64
    write(source / 'cmake/deps.json', json.dumps(deps))
    configure_dxc(cmake, source, out / 'bad-file', fallback, local_zip, error='of the zip does not match its SHA-256')
    archive.unlink()
    offline = out / 'offline'
    assert 'could not fetch' in configure_dxc(cmake, source, offline, fallback)
    assert not (offline / '_deps/fixture-dxc.zip.part').exists()


MUTATIONS = (
    ('setup-stage.cmake', 'larecomp_stage_sdk_runtime(larecomp)', ''),
    ('setup-stage.cmake', 'larecomp_stage(larecomp_mspack "$<TARGET_FILE_NAME:larecomp_mspack>" dll)', ''),
    ('setup-stage.cmake', '$<TARGET_FILE_NAME:larecomp_mspack>" dll', 'larecomp_mspack.dll" dll'),
    ('setup-stage.cmake', 'larecomp_mspack_notice.txt larecomp_mspack_license.txt)', 'larecomp_mspack_notice.txt)'),
    ('setup-stage.cmake', 'larecomp_mspack_notice.txt larecomp_mspack_license.txt)', 'larecomp_mspack_license.txt)'),
    ('setup-stage.cmake', 'if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/LICENSES/${_larecomp_mspack_notice}")', 'if(FALSE)'),
    ('setup-stage.cmake', '"${_larecomp_mspack_notice}" licence', '"${_larecomp_mspack_notice}" licence OPTIONAL'),
    ('setup-stage.cmake', 'SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSES/${_larecomp_mspack_notice}"', ''),
    ('setup-stage.cmake', 'LICENCE_FOR "$<TARGET_FILE_NAME:larecomp_mspack>"', 'LICENCE_FOR "larecomp_mspack.dll"'),
    ('cmake/InstallManifest.cmake', 'set(dest "$<TARGET_FILE_DIR:${target}>/${path}")',
     'set(dest "$<TARGET_FILE_DIR:${target}>/wrong/${path}")'),
    ('cmake/InstallManifest.cmake', 'set(required true)', 'set(required false)'),
    ('game-stage.cmake', 'larecomp_stage(larecomp "assets/${_larecomp_asset}" data',
     'larecomp_stage(larecomp "assets/**" data'),
    ('game-stage.cmake', '_larecomp_ffx_api STREQUAL "dx12"', '_larecomp_ffx_api STREQUAL "dx13"'),
    ('game-stage.cmake', 'if(LARECOMP_STREAMLINE_HAS_RR)', 'if(FALSE)'),
    ('cmake/InstallManifest.cmake', 'list(APPEND city_after probe_machine)', ''),
    ('cmake/InstallManifest.cmake', "gpu.rt_tier != 'none'", "gpu.rt_tier == 'none'"),
    ('cmake/InstallManifest.cmake', 'OUTPUTS ${shader_outputs}', 'OUTPUTS "{shader_pack_dir}/mcla_shaders.v8.pack"'),
    ('cmake/InstallManifest.cmake', '"actions": @describe_actions@', '"actions": []'),
    ('cmake/InstallManifest.cmake', '"game_root": "@game_root@"', '"game_root": "--other_root={game_root}"'),
    ('cmake/InstallManifest.cmake', '--@debug_menu@=true', '--other_debug=true'),
    ('cmake/InstallManifest.cmake', '"size": 9252864', '"size": 9252865'),
    ('cmake/InstallManifest.cmake', '\\"estimated_disk_mib\\": ${disk_mib}', '\\"estimated_disk_mib\\": 0'),
    ('cmake/InstallManifest.cmake', '\\"timeout_min\\": ${timeout}', '\\"timeout_min\\": 1'),
    ('cmake/InstallManifest.cmake', '  "pipeline_key_settings": @pipeline_key_settings@,', '  "pipeline_key_settings": [],'),
    ('cmake/InstallManifest.cmake', 'set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${source}")', ''),
    ('cmake/InstallManifest.cmake', 'list(REMOVE_DUPLICATES settings)', ''),
    (PIPELINE_PREBUILD, '(mcla_gpu_msaa, mcla_gpu_upscaler)', '(mcla_gpu_msaa)'),
    ('cmake/InstallManifest.cmake', 'logs/${join_status}', 'logs/other_status.txt'),
    ('cmake/InstallManifest.cmake', '"program": "$<TARGET_FILE_NAME:@target@>"', '"program": "larecomp"'),
    ('cmake/InstallManifest.cmake', ' "no list"', ''),
    ('cmake/InstallManifest.cmake', 'set(machine_file "machine.json")', 'set(machine_file "other.json")'),
    ('cmake/InstallManifest.cmake', 'set(settings_ui_file "settings-ui.json")', 'set(settings_ui_file "other.json")'),
    # min_launcher: the manifest and the --describe text drop it, carry a wrong value, a malformed or non-text value is accepted, a missing
    # one gets a default, a "0" is lost, settings-ui.json (settings_ui_data.h) and machine.json (the compile definition) stop following the recipe.
    ('cmake/InstallManifest.cmake', '"min_reader": 1,\n  @launcher_gate@@identity@,', '"min_reader": 1,\n  @identity@,'),
    ('cmake/InstallManifest.cmake', '"schema_version": 1,\n  @launcher_gate@@identity@,', '"schema_version": 1,\n  @identity@,'),
    ('cmake/InstallManifest.cmake', 'set(launcher_gate "\\"min_launcher\\": \\"${min_launcher}\\",\\n  ")',
     'set(launcher_gate "\\"min_launcher\\": \\"9.9.9\\",\\n  ")'),
    ('cmake/InstallManifest.cmake', 'if(NOT value MATCHES "^[0-9]+(\\\\.[0-9]+(\\\\.[0-9]+(\\\\.[0-9]+)?)?)?$")', 'if(FALSE)'),
    ('cmake/InstallManifest.cmake', 'if(NOT type STREQUAL "STRING")', 'if(FALSE)'),
    ('cmake/InstallManifest.cmake', 'message(FATAL_ERROR "launcher-recipe.json is not valid JSON: ${error}")', 'set(ignored ON)'),
    ('cmake/InstallManifest.cmake', '        set(${out} "" PARENT_SCOPE)\n        return()', '        set(${out} "1.0.0" PARENT_SCOPE)\n        return()'),
    ('cmake/InstallManifest.cmake', 'if(NOT "${min_launcher}" STREQUAL "")', 'if(min_launcher)'),
    ('setup-stage.cmake', 'larecomp_recipe_min_launcher(LARECOMP_MIN_LAUNCHER)', ''),
    ('exe-wiring.cmake', 'set(GX_MIN_LAUNCHER "${LARECOMP_MIN_LAUNCHER}")', 'set(GX_MIN_LAUNCHER "")'),
    ('exe-wiring.cmake', '"LARECOMP_MIN_LAUNCHER=\\"${LARECOMP_MIN_LAUNCHER}\\""', '"LARECOMP_MIN_LAUNCHER=\\"1.0.0\\""'),
    ('exe-wiring.cmake', 'if(NOT "${LARECOMP_MIN_LAUNCHER}" STREQUAL "")', 'if(TRUE)'),
    ('src/settings_ui/ui_data.h.in', 'kSettingsUiMinLauncher = "@GX_MIN_LAUNCHER@"', 'kSettingsUiMinLauncher = ""'),
)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cmake', default='cmake')
    parser.add_argument('--mutations', action='store_true')
    args = parser.parse_args()
    deps = check_deps()
    run([sys.executable, '-I', str(VALIDATOR), '--strict', str(ROOT / 'launcher-recipe.json')])
    cases = (
        dict(windows=True, full=True, exports=True),
        dict(windows=True, mingw=True),
        dict(windows=False),
        dict(windows=True, gpu=False),
    )
    with tempfile.TemporaryDirectory(prefix='lfg manifest ') as temp:
        temp = Path(temp)
        source = temp / 'source'
        prepare_tree(source, deps)
        pack = source / 'src/shaderpack/pack_format.h'
        contents, count = re.subn(r'(?m)^(inline constexpr const char\* kPackFileName = ")[^"]+',
                                  r'\1mcla_shaders.fixture.pack', pack.read_text(encoding='utf-8'))
        assert count == 1
        write(pack, contents)
        for i, case in enumerate(cases):
            check_runtime(args.cmake, source, temp / f'build-{i}', **case)
        print(f'{len(cases)} runtime configurations and their describe output validated')
        check_mspack_sidecars(args.cmake, source, temp / 'mspack-missing')
        print(f'{len(MSPACK_SIDECARS)} missing libmspack sidecars stop the configure')
        check_pipeline_key_export(args.cmake, source, temp / 'pipeline-key-changed')
        print('pipeline key additions and deduplication checked')
        check_min_launcher(args.cmake, source, temp / 'min-launcher')
        print(f'min_launcher: copied from launcher-recipe.json into the manifest, the describe text, settings_ui_data.h and machine_facts.cpp; '
              f'absent stays absent; {len(BAD_MIN_LAUNCHERS)} malformed values stop the configure')
        if args.mutations:
            for i, (name, old, new) in enumerate(MUTATIONS):
                path = source / name
                before = path.read_text(encoding='utf-8')
                assert old in before, (name, old)
                write(path, before.replace(old, new, 1))
                try:
                    for j, case in enumerate(cases):
                        check_runtime(args.cmake, source, temp / f'mutant-{i}-{j}', **case)
                    check_pipeline_key_export(args.cmake, source, temp / f'mutant-{i}-changed')
                    check_min_launcher(args.cmake, source, temp / f'mutant-{i}-min-launcher')
                    check_mspack_sidecars(args.cmake, source, temp / f'mutant-{i}-mspack-missing')
                except (AssertionError, KeyError, FileNotFoundError):
                    pass
                else:
                    raise AssertionError(f'mutation survived: {name}: {old}')
                finally:
                    write(path, before)
            print(f'{len(MUTATIONS)} mutations caught')
        check_fetchers(args.cmake, source, temp / 'fetch-cache', deps)
        print('3 kit caches checked, 2 dependency files and recipe validated')
        check_dxc_host(args.cmake, source, temp / 'dxc-host', deps)
        print('10 DXC host configure cases checked')


if __name__ == '__main__':
    main()
