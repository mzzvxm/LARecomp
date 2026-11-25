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


ROOT = Path(__file__).resolve().parents[2]
CONTRACT = ROOT / 'documentation/contract'
VALIDATOR = CONTRACT / 'validate_contract.py'
CONSTANT_SOURCES = (
    'larecomp_manifest.toml', 'src/cpu_level.cpp', 'src/larecomp_version.h',
    'src/isoinstaller/larecomp_iso_installer.cpp', 'src/saveporter/larecomp_save_porter.cpp',
    'src/mc_engine/online/online_tables_core.h', 'src/shaderpack/pack_format.h',
    'src/citypack/pack_format.h', 'src/citypack/city_hit_format.h', 'src/gpu/pipelines.cpp',
)
FIXTURE = '''
cmake_minimum_required(VERSION 3.25)
project(ManifestFixture NONE)
set(CMAKE_CXX_COMPILE_OBJECT "${CMAKE_COMMAND} -E touch <OBJECT>")
set(CMAKE_CXX_LINK_EXECUTABLE "${CMAKE_COMMAND} -E touch <TARGET>")
set(CMAKE_CXX_CREATE_SHARED_LIBRARY "${CMAKE_COMMAND} -E touch <TARGET>")
set(WIN32 ${fixture_windows})
set(CMAKE_EXECUTABLE_SUFFIX "${fixture_suffix}")
set(CMAKE_SHARED_LIBRARY_SUFFIX "${fixture_lib_suffix}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/runtime")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
add_executable(larecomp dummy.cpp)
add_executable(larecomp_online_tool dummy.cpp)
add_library(larecomp_mspack SHARED dummy.cpp)
set_target_properties(larecomp larecomp_online_tool larecomp_mspack PROPERTIES LINKER_LANGUAGE CXX)
set_target_properties(larecomp larecomp_online_tool PROPERTIES SUFFIX "${fixture_suffix}")
set_target_properties(larecomp_mspack PROPERTIES PREFIX "" SUFFIX "${fixture_lib_suffix}")
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
set(LARECOMP_MSPACK_NOTICES larecomp_mspack_notice.txt larecomp_mspack_license.txt)
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
include(setup-stage.cmake)
include(game-stage.cmake)
set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/lfg/describe_json.cpp" PROPERTIES HEADER_FILE_ONLY ON)
'''


def run(args):
    result = subprocess.run(args, capture_output=True, text=True, encoding='utf-8', timeout=45)
    if result.returncode:
        raise AssertionError(result.stdout + result.stderr)
    return result.stdout


def read(path):
    return json.loads(path.read_text(encoding='utf-8'))


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
    return read(ROOT / 'cmake/deps.json')


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
        'LICENSES/larecomp_mspack_notice.txt', 'LICENSES/larecomp_mspack_license.txt',
    ):
        write(source / filename)
    for kit in deps['kits'].values():
        for item in kit['files']:
            write(source / 'kit' / item['path'])
    cmakelists = (ROOT / 'CMakeLists.txt').read_text(encoding='utf-8')
    setup = cmakelists.split('    # BS-G: Record the SDK', 1)[1].split('    # Third-party code', 1)[0]
    mspack = cmakelists.split('    # BS-G: The library', 1)[1].split('    target_link_libraries(larecomp PRIVATE larecomp_mspack)', 1)[0]
    stage = cmakelists.split('    # BS-G game runtime staging.', 1)[1].split('    # Floating-point contraction', 1)[0]
    write(source / 'setup-stage.cmake', '#' + setup + '#' + mspack)
    write(source / 'game-stage.cmake', '#' + stage)
    write(source / 'CMakeLists.txt', FIXTURE)


def configure(cmake, source, output, *, windows, gpu=True, full=False, exports=False):
    write(output / 'runtime/assets/default.xex', 'synthetic codegen input')
    write(output / 'copies.txt', '')
    run([cmake, '-S', str(source), '-B', str(output), '-G', 'Ninja',
         f'-Dfixture_windows={int(windows)}', f'-Dfixture_gpu={int(gpu)}',
         f'-Dfixture_full={int(full)}', f'-Dfixture_exports={int(exports)}',
         '-Dfixture_suffix=' + ('.exe' if windows else ''),
         '-Dfixture_lib_suffix=' + ('.dll' if windows else '.so')])
    path = output / 'runtime/install-manifest.json'
    describe = output / 'describe.json'
    embedded = (output / 'lfg/describe_json.cpp').read_text(encoding='utf-8')
    write(describe, embedded.split('R"lfg(', 1)[1].split(')lfg"', 1)[0])
    run([sys.executable, '-I', str(VALIDATOR), '--strict', str(path), str(describe)])
    return read(path), read(describe)


def check_runtime(cmake, source, out, *, windows, gpu=True, full=False, exports=False):
    manifest, describe = configure(cmake, source, out, windows=windows, gpu=gpu, full=full, exports=exports)
    stage = {entry['from']: entry for entry in manifest['stage']}
    action = {entry['id']: entry for entry in manifest['actions']}
    assert len(stage) == len(manifest['stage'])
    assert len(action) == (7 if exports else 5)
    assert list(action) == describe['actions']
    for field in ('game_version', 'commit', 'contracts'):
        assert manifest[field] == describe[field]
    assert ('settings_ui' in manifest['contracts']) == exports
    assert ('machine' in manifest['contracts']) == exports
    assert manifest['exit_codes']['cpu_level_refused'] == 0x4C43
    assert manifest['game']['required_files'] == [
        'default.xex', 'xarchive_audio.rpf', 'xarchive_audlo.rpf', 'xarchive_cache.rpf',
    ]
    assert manifest['markers'] == {'algo': 'fnv1a64-lower-path', 'version': 1}
    extension = '.dll' if windows else '.so'
    assert stage['sdk_module' + extension]['kind'] == 'module'
    assert stage['larecomp_mspack_license.txt']['licence_for'] == 'larecomp_mspack' + extension
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


def check_fetchers(cmake, source, out, deps):
    # The two readers use their normal cache validation with synthetic bytes.
    readers = {'fidelityfx': 'fidelityfx-sdk', 'streamline': 'streamline'}
    for kit_id, kit in deps['kits'].items():
        if kit_id not in readers:
            continue  # a kit with no CMake reader in this tree (dxc_host: cmake/FetchDxcHost.cmake comes with deps/dxc-unify)
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
    # Run as a regular configure so ReadDeps records its configure dependency.
    write(source / 'CMakeLists.txt', 'cmake_minimum_required(VERSION 3.25)\n'
          'project(FetchFixture NONE)\ninclude(fetch-fixture.cmake)\n')
    run([cmake, '-S', str(source), '-B', str(out), '-G', 'Ninja'])


MUTATIONS = (
    ('setup-stage.cmake', 'larecomp_stage_sdk_runtime(larecomp)', ''),
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
        dict(windows=True),
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
        if args.mutations:
            for i, (name, old, new) in enumerate(MUTATIONS):
                path = source / name
                before = path.read_text(encoding='utf-8')
                assert old in before, (name, old)
                write(path, before.replace(old, new, 1))
                try:
                    for j, case in enumerate(cases):
                        check_runtime(args.cmake, source, temp / f'mutant-{i}-{j}', **case)
                except (AssertionError, KeyError):
                    pass
                else:
                    raise AssertionError(f'mutation survived: {name}: {old}')
                finally:
                    write(path, before)
            print(f'{len(MUTATIONS)} mutations caught')
        check_fetchers(args.cmake, source, temp / 'fetch-cache', deps)
        print('2 kit caches checked, 2 dependency files and recipe validated')


if __name__ == '__main__':
    main()
