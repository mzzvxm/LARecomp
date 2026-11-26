# BS-G: The manifest lives in the runtime output folder. Both stage paths are
# relative to their respective folders, even when the builds use a preset.
function(larecomp_json_string out value)
    string(REPLACE "\\" "\\\\" value "${value}")
    string(REPLACE "\"" "\\\"" value "${value}")
    string(REPLACE "\n" "\\n" value "${value}")
    string(REPLACE "\r" "\\r" value "${value}")
    string(REPLACE "\t" "\\t" value "${value}")
    set(${out} "\"${value}\"" PARENT_SCOPE)
endfunction()

function(larecomp_json_strings out)
    set(items "")
    foreach(value IN LISTS ARGN)
        larecomp_json_string(item "${value}")
        list(APPEND items "${item}")
    endforeach()
    list(JOIN items ", " body)
    set(${out} "[${body}]" PARENT_SCOPE)
endfunction()

# Register a runtime artifact. SOURCE adds the post-build copy that produces it.
# Existing build products (the exe, an SDK module, etc.) already have a producer.
function(larecomp_stage target path kind)
    cmake_parse_arguments(PARSE_ARGV 3 stage "OPTIONAL;DIRECTORY" "SOURCE;LICENCE_FOR;GROUP" "")
    if(stage_UNPARSED_ARGUMENTS OR stage_KEYWORDS_MISSING_VALUES)
        message(FATAL_ERROR "Invalid stage entry for ${path}: ${stage_UNPARSED_ARGUMENTS} ${stage_KEYWORDS_MISSING_VALUES}")
    endif()
    if(stage_SOURCE)
        set(dest "$<TARGET_FILE_DIR:${target}>/${path}")
        if(stage_DIRECTORY)
            add_custom_command(TARGET ${target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_directory "${stage_SOURCE}" "${dest}"
                VERBATIM)
        else()
            cmake_path(GET path PARENT_PATH subdir)
            add_custom_command(TARGET ${target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${target}>/${subdir}"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${stage_SOURCE}" "${dest}"
                VERBATIM)
        endif()
    endif()
    if(stage_OPTIONAL)
        set(required false)
    else()
        set(required true)
    endif()
    if(stage_DIRECTORY)
        larecomp_json_string(from "${path}/**")
        larecomp_json_string(to "${path}/")
    else()
        larecomp_json_string(from "${path}")
        set(to "${from}")
    endif()
    set(entry "{ \"from\": ${from}, \"to\": ${to}, \"kind\": \"${kind}\", \"required\": ${required}")
    if(stage_LICENCE_FOR)
        larecomp_json_string(licence "${stage_LICENCE_FOR}")
        string(APPEND entry ", \"licence_for\": ${licence}")
    endif()
    if(stage_GROUP)
        string(APPEND entry ", \"group\": \"${stage_GROUP}\"")
    endif()
    set_property(DIRECTORY APPEND PROPERTY LARECOMP_STAGE_ENTRIES "${entry} }")
endfunction()

function(larecomp_stage_sdk_runtime target)
    rexglue_get_runtime_files(${target} runtime_files)
    list(REMOVE_DUPLICATES runtime_files)
    foreach(runtime_file IN LISTS runtime_files)
        string(REPLACE "$<TARGET_FILE_DIR:${target}>/" "" name "${runtime_file}")
        larecomp_stage(${target} "${name}" module)
    endforeach()
endfunction()

# Read a named literal owned by the game. A rename or expression in place of a
# literal is a configure error so a stale name never lands in the manifest.
function(larecomp_manifest_literal out relative_path pattern)
    set(path "${CMAKE_CURRENT_SOURCE_DIR}/${relative_path}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${path}")
    file(STRINGS "${path}" lines REGEX "${pattern}" LIMIT_COUNT 2)
    list(LENGTH lines count)
    if(NOT count EQUAL 1 OR NOT lines MATCHES "${pattern}")
        message(FATAL_ERROR "Cannot read one manifest value from ${relative_path}: ${pattern}")
    endif()
    set(${out} "${CMAKE_MATCH_1}" PARENT_SCOPE)
endfunction()

function(larecomp_manifest_pack_outputs out source folder)
    set(outputs "")
    foreach(name IN LISTS ARGN)
        larecomp_manifest_literal(filename "${source}" "^[^/]* ${name} = \"([^\"]+)\"")
        list(APPEND outputs "${folder}/${filename}")
    endforeach()
    set(${out} "${outputs}" PARENT_SCOPE)
endfunction()

function(larecomp_manifest_commit out)
    find_package(Git QUIET)
    if(Git_FOUND)
        execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse HEAD
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            OUTPUT_VARIABLE commit OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        execute_process(COMMAND "${GIT_EXECUTABLE}" symbolic-ref -q HEAD
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            OUTPUT_VARIABLE ref OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        foreach(git_path HEAD ${ref})
            execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse --path-format=absolute --git-path "${git_path}"
                WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
                OUTPUT_VARIABLE path OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
            if(EXISTS "${path}")
                set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${path}")
            endif()
        endforeach()
    endif()
    string(LENGTH "${commit}" commit_length)
    if(NOT commit_length EQUAL 40 OR NOT commit MATCHES "^[0-9a-f]+$")
        string(REPEAT "0" 40 commit)
    endif()
    set(${out} "${commit}" PARENT_SCOPE)
endfunction()

# The oldest launcher program that can set up this game version: min_launcher of launcher-recipe.json, the one place it is
# edited. The manifest, the --describe text, settings-ui.json and machine.json all take it from here. An empty result means
# the recipe has none (no requirement): every file then leaves the field out. A value that is not 1 to 4 dot-separated
# numbers is a configure error, so a typo never reaches a player.
function(larecomp_recipe_min_launcher out)
    set(path "${CMAKE_CURRENT_SOURCE_DIR}/launcher-recipe.json")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${path}")
    file(READ "${path}" recipe)
    string(JSON members ERROR_VARIABLE error LENGTH "${recipe}")
    if(NOT error STREQUAL "NOTFOUND")
        message(FATAL_ERROR "launcher-recipe.json is not valid JSON: ${error}")
    endif()
    string(JSON type ERROR_VARIABLE error TYPE "${recipe}" min_launcher)
    if(NOT error STREQUAL "NOTFOUND")
        set(${out} "" PARENT_SCOPE)
        return()
    endif()
    if(NOT type STREQUAL "STRING")
        message(FATAL_ERROR "launcher-recipe.json: min_launcher must be a text like \"1.0.0\", not ${type}")
    endif()
    string(JSON value GET "${recipe}" min_launcher)
    if(NOT value MATCHES "^[0-9]+(\\.[0-9]+(\\.[0-9]+(\\.[0-9]+)?)?)?$")
        message(FATAL_ERROR "launcher-recipe.json: min_launcher must be 1 to 4 numbers separated by dots, like 1.0.0 (got \"${value}\")")
    endif()
    set(${out} "${value}" PARENT_SCOPE)
endfunction()

function(larecomp_add_manifest_action id prefix timeout when)
    cmake_parse_arguments(PARSE_ARGV 4 action "" "FORCE_FLAG" "AFTER;ARGS;OUTPUTS;PRODUCES")
    larecomp_json_strings(after ${action_AFTER})
    larecomp_json_strings(args ${action_ARGS})
    larecomp_json_strings(outputs ${action_OUTPUTS})
    larecomp_json_string(condition "${when}")
    set(entry "{ \"id\": \"${id}\", \"flag\": \"--${id}\", \"args\": ${args}, \"after\": ${after}, \"when\": ${condition}, \"outputs\": ${outputs}")
    string(APPEND entry ", \"stdout\": { \"prefix\": \"@${prefix}|\", \"grammar\": 1 }, \"exit\": { \"ok\": [0], \"fail\": [1] }, \"never_fails_setup\": true, \"timeout_min\": ${timeout}")
    if(action_FORCE_FLAG)
        string(APPEND entry ", \"force_flag\": \"${action_FORCE_FLAG}\"")
    endif()
    if(action_PRODUCES)
        larecomp_json_strings(produces ${action_PRODUCES})
        string(APPEND entry ", \"produces\": ${produces}")
    endif()
    list(APPEND manifest_actions "${entry} }")
    list(APPEND describe_actions "${id}")
    set(manifest_actions "${manifest_actions}" PARENT_SCOPE)
    set(describe_actions "${describe_actions}" PARENT_SCOPE)
endfunction()

function(larecomp_add_manifest_probe name test)
    larecomp_json_strings(files ${ARGN})
    list(APPEND manifest_facts "\"install.${name}\": { \"${test}\": ${files} }")
    set(manifest_facts "${manifest_facts}" PARENT_SCOPE)
endfunction()

function(larecomp_generate_install_manifest target)
    larecomp_manifest_literal(version src/larecomp_version.h [=[^#define LARECOMP_VERSION_LABEL "([^"]+)"]=])
    larecomp_json_string(version "${version}")
    larecomp_manifest_commit(commit)
    larecomp_manifest_literal(sdk_generation larecomp_manifest.toml [=[^sdk_version = "([0-9]+\.[0-9]+)\.]=])
    larecomp_manifest_literal(title_id src/saveporter/larecomp_save_porter.cpp [=[^constexpr uint32_t kTitleId = 0x([0-9A-F]+)u]=])
    larecomp_manifest_literal(cpu_refused src/cpu_level.cpp [=[^constexpr unsigned kExitCode = (0x[0-9A-Fa-f]+)]=])
    math(EXPR cpu_refused "${cpu_refused}")

    # Read the same required files checked by IsGameInstalled. The codegen
    # manifest names the XEX; the hash and size below identify the supported disc release.
    set(installer "${CMAKE_CURRENT_SOURCE_DIR}/src/isoinstaller/larecomp_iso_installer.cpp")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${installer}")
    file(READ "${installer}" install_source)
    string(REGEX MATCH "kRequiredFiles\\[\\] = \\{([^}]+)\\}" required_decl "${install_source}")
    set(required_decl "${CMAKE_MATCH_1}")
    larecomp_manifest_literal(default_xex src/isoinstaller/larecomp_iso_installer.cpp [=[^constexpr std::string_view kDefaultXex = "([^"]+)"]=])
    string(REPLACE "kDefaultXex" "\"${default_xex}\"" required_decl "${required_decl}")
    string(REGEX MATCHALL "\"[^\"]+\"" required_files "${required_decl}")
    if(NOT required_files)
        message(FATAL_ERROR "Cannot read IsGameInstalled's required files")
    endif()
    list(JOIN required_files ", " required_files)

    set(contracts "{ \"install_manifest\": 1 }")
    set(features xenia)
    if(TARGET larecomp_gpu)
        list(APPEND features gpu)
    endif()
    if(LARECOMP_FFX_KIT)
        list(APPEND features fidelityfx)
        if(LARECOMP_FFX_HAS_FRAMEGENERATION)
            list(APPEND features fidelityfx_frame_generation)
        endif()
        if(LARECOMP_FFX_HAS_DENOISER AND "D3D12" IN_LIST LARECOMP_GPU_BACKENDS)
            list(APPEND features ray_regeneration)
        endif()
    endif()
    if(LARECOMP_STREAMLINE_KIT)
        list(APPEND features dlss)
        if(LARECOMP_STREAMLINE_HAS_FRAMEGEN)
            list(APPEND features dlss_frame_generation)
        endif()
        if(LARECOMP_STREAMLINE_HAS_RR)
            list(APPEND features dlss_ray_reconstruction)
        endif()
    endif()
    set(files "{}")
    if(TARGET larecomp_online_tool)
        list(APPEND features online_tool)
        larecomp_manifest_literal(online_tables src/mc_engine/online/online_tables_core.h [=[^inline constexpr char kFileName\[\] = "([^"]+)"]=])
        set(files "{ \"online_tool\": \"$<TARGET_FILE_NAME:larecomp_online_tool>\", \"online_tables\": \"logs/${online_tables}\" }")
    endif()
    larecomp_json_strings(features ${features})

    set(manifest_actions "")
    set(describe_actions "")
    # The app accepts these five flags even on an SDK-only build, where they
    # report "not needed". Only offer their setup work on a native GPU build.
    set(gpu_when false)
    if(TARGET larecomp_gpu)
        set(gpu_when true)
    endif()
    # GX and MF opt in from their own target blocks once their dispatch exists.
    get_target_property(settings_ui ${target} LARECOMP_EXPORTS_SETTINGS_UI)
    if(settings_ui)
        string(JSON contracts SET "${contracts}" settings_ui 1)
        larecomp_add_manifest_action(dump_settings_ui settingsui 1 true
            OUTPUTS "{game_folder}/settings-ui.json" PRODUCES settings-ui)
    endif()
    set(city_after build_shader_packs)
    get_target_property(machine ${target} LARECOMP_PROBES_MACHINE)
    if(machine)
        string(JSON contracts SET "${contracts}" machine 1)
        larecomp_add_manifest_action(probe_machine machine 1 true
            OUTPUTS "{game_folder}/machine.json" PRODUCES machine)
        list(APPEND city_after probe_machine)
    endif()

    set(rt_when "${gpu_when} && gpu.rt_tier != 'none'")
    set(game_root "--game_data_root={game_root}")
    larecomp_manifest_literal(debug_menu src/settings/larecomp_app_settings.h
        [=[^  REX_SETTING\(bool, (debug_menu), false,]=])
    larecomp_manifest_pack_outputs(shader_outputs src/shaderpack/pack_format.h "{shader_pack_dir}"
        kPackFileName kStampFileName)
    larecomp_manifest_pack_outputs(city_outputs src/citypack/pack_format.h "{shader_pack_dir}/city"
        kPackFileName kStampFileName)
    larecomp_manifest_pack_outputs(hit_outputs src/citypack/city_hit_format.h "{shader_pack_dir}/city"
        kHitFileName kTexFileName kHitStampFileName)
    larecomp_manifest_literal(pipeline_file src/gpu/pipelines.cpp [=[^  cache_path_ = .*std::format\("([^"]+)", backend\)]=])
    string(REPLACE "{}" "{api}" pipeline_file "${pipeline_file}")
    cmake_path(REPLACE_EXTENSION pipeline_file LAST_ONLY ".stamp" OUTPUT_VARIABLE pipeline_stamp)
    larecomp_add_manifest_action(build_shader_packs shaderpack 25 "${gpu_when}"
        ARGS "${game_root}" FORCE_FLAG --build_shader_packs_force OUTPUTS ${shader_outputs})
    larecomp_add_manifest_action(precache_pipelines pipelines 20 "${gpu_when}"
        ARGS "${game_root}" FORCE_FLAG --precache_pipelines_force AFTER build_shader_packs
        OUTPUTS "{pipeline_cache_dir}/${pipeline_file}" "{pipeline_cache_dir}/${pipeline_stamp}")
    larecomp_add_manifest_action(build_city_pack citypack 25 "${rt_when}"
        ARGS "${game_root}" FORCE_FLAG --build_city_pack_force AFTER ${city_after} OUTPUTS ${city_outputs})
    larecomp_add_manifest_action(build_city_hit citypack 25 "${rt_when}"
        ARGS "${game_root}" FORCE_FLAG --build_city_hit_force AFTER build_city_pack OUTPUTS ${hit_outputs})
    larecomp_add_manifest_action(check_city_hit citypack 5 "${rt_when}"
        ARGS "${game_root}" --check_city_hit_deep=true AFTER build_city_hit)

    larecomp_json_strings(describe_actions ${describe_actions})
    list(JOIN manifest_actions ",\n    " actions)

    # Presence alone cannot tell whether a generated pack's stamp is current.
    set(manifest_facts "")
    larecomp_add_manifest_probe(fidelityfx_files any_of amd_fidelityfx_loader_dx12.dll amd_fidelityfx_loader_vk.dll)
    larecomp_add_manifest_probe(fidelityfx_frame_generation any_of amd_fidelityfx_framegeneration_dx12.dll amd_fidelityfx_framegeneration_vk.dll)
    larecomp_add_manifest_probe(fidelityfx_ray_regeneration all_of amd_fidelityfx_denoiser_dx12.dll)
    larecomp_add_manifest_probe(streamline_files all_of sl.interposer.dll sl.common.dll sl.dlss.dll nvngx_dlss.dll)
    foreach(group frame_generation ray_reconstruction)
        larecomp_kit_group_files("${larecomp_streamline_pins}" "${group}" paths)
        list(FILTER paths INCLUDE REGEX "^bin/x64/.*\\.dll$")
        list(TRANSFORM paths REPLACE "^bin/x64/" "")
        larecomp_add_manifest_probe("streamline_${group}" all_of ${paths})
    endforeach()
    list(JOIN manifest_facts ",\n    " facts)
    get_property(stage DIRECTORY PROPERTY LARECOMP_STAGE_ENTRIES)
    list(JOIN stage ",\n    " stage)

    set(identity "\"game_version\": ${version},\n  \"commit\": \"${commit}\",\n  \"contracts\": ${contracts}")
    larecomp_recipe_min_launcher(min_launcher)
    set(launcher_gate "")
    if(NOT "${min_launcher}" STREQUAL "")
        set(launcher_gate "\"min_launcher\": \"${min_launcher}\",\n  ")
    endif()
    string(CONFIGURE [=[
{
  "schema": "larecomp.install-manifest",
  "schema_version": 1,
  "min_reader": 1,
  @launcher_gate@@identity@,
  "sdk": { "generation": "@sdk_generation@" },
  "features": @features@,
  "stage": [
    @stage@
  ],
  "facts": {
    @facts@
  },
  "actions": [
    @actions@
  ],
  "launch": { "game_root": "@game_root@", "debug_menu": "--@debug_menu@=true" },
  "game": {
    "title_id": "@title_id@",
    "xex_sha256": ["c386f4001fa569e6ad4b982f441f67412f00b3f47c166134555cd4b59854a432"],
    "size": 9252864,
    "required_files": [@required_files@]
  },
  "layout": {
    "game_files_folder": "MCLA_Game_Files",
    "mod_roots": ["modloader", "mods", "models"],
    "textures": "textures_replace",
    "disabled": ".disabled",
    "saved_game": "mc4.sav",
    "profile_id": "B13EBABEBABEBABE",
    "reserved_mod_folders": ["textures_dump", "textures_replace", "music"]
  },
  "files": @files@,
  "exit_codes": { "cpu_level_refused": @cpu_refused@ },
  "markers": { "algo": "fnv1a64-lower-path", "version": 1 }
}
]=] manifest @ONLY)
    file(GENERATE OUTPUT "$<TARGET_FILE_DIR:${target}>/install-manifest.json" CONTENT "${manifest}")

    # The exe and the staged manifest share the build's identity and actions.
    # CHECK: run the linked GUI executable with stdout piped on Windows and
    # compare its output with this manifest after staging on Windows and Linux.
    set(describe_source "${CMAKE_CURRENT_BINARY_DIR}/lfg/describe_json.cpp")
    file(CONFIGURE OUTPUT "${describe_source}" CONTENT [=[
namespace larecomp {
extern const char kDescribeJson[] = R"lfg({
  "schema": "larecomp.describe",
  "schema_version": 1,
  @launcher_gate@@identity@,
  "actions": @describe_actions@
}
)lfg";
}  // namespace larecomp
]=] @ONLY)
    target_sources(${target} PRIVATE "${describe_source}")
endfunction()
