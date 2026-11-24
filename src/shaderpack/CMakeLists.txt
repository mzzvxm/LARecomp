# The shader pack builder (src/shaderpack).
#
# LARecomp builds its shader packs itself, on the player's PC, from the player's game files:
# the Xbox 360 shader containers are translated to HLSL by LAR-XenosRecomp (library target
# XenosRecompLib) and compiled with a dxcompiler.dll that is loaded at run time. See
# src/shaderpack/builder.h for the interface and shaderpack/PIPELINE-PLAN.md for the design.
#
# This directory defines one target, the static library larecomp_shaderpack. It has no dependency
# on the ReXGlue runtime or on Plume, so larecomp links it and tools/shaderpack_tool builds it on
# its own. It expects, from whoever links it (larecomp already has them in LARECOMP_SOURCES; the
# tool compiles them itself): src/mc_engine/modloader/{rpf3,aes256,texture,objmesh}.cpp,
# src/native_gfx/shader_identity.cpp and src/os/thread_priority.cpp.
#
# Cache variables:
#   LARECOMP_XENOSRECOMP_DIR     the LAR-XenosRecomp checkout (default thirdparty/LAR-XenosRecomp;
#                                pass a clone's path for a local build)
#   LARECOMP_DXC_INCLUDE_DIR     the folder with dxcapi.h (default thirdparty/dxc-bin/inc, else the
#                                one inside LAR-XenosRecomp's own thirdparty/dxc-bin)
#   LARECOMP_XXHASH_INCLUDE_DIR  the folder with xxhash.h (default: the one inside LAR-XenosRecomp's
#                                thirdparty/xxHash; rexglue 0.11's installed headers do not carry xxHash)
#   LARECOMP_HDR_HLSLI           the HDR scene's curve, hdr_tonemap_shoulder.hlsli, which the HDR scene
#                                variant embeds (default src/gpu/hlsl/hdr_tonemap_shoulder.hlsli; without
#                                the file there is no HDR variant, and the variant pack holds the others)

get_filename_component(LARECOMP_SHADERPACK_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

set(LARECOMP_XENOSRECOMP_DIR "${LARECOMP_SHADERPACK_REPO_ROOT}/thirdparty/LAR-XenosRecomp"
    CACHE PATH "LAR-XenosRecomp checkout (the shader translator, library target XenosRecompLib)")
set(LARECOMP_DXC_INCLUDE_DIR "" CACHE PATH "Folder with dxcapi.h (empty: look in thirdparty/dxc-bin/inc)")
set(LARECOMP_XXHASH_INCLUDE_DIR "" CACHE PATH "Folder with xxhash.h (empty: LAR-XenosRecomp's)")
set(LARECOMP_HDR_HLSLI "" CACHE FILEPATH
    "The HDR scene's curve for the HDR variant step (empty: src/gpu/hlsl/hdr_tonemap_shoulder.hlsli if it exists)")

# ---- the translator ------------------------------------------------------------------------
if(NOT EXISTS "${LARECOMP_XENOSRECOMP_DIR}/XenosRecomp/xenos_recomp.h")
    message(FATAL_ERROR
        "larecomp_shaderpack: LAR-XenosRecomp with its library interface is not at "
        "${LARECOMP_XENOSRECOMP_DIR} (XenosRecomp/xenos_recomp.h is missing). Set "
        "LARECOMP_XENOSRECOMP_DIR to a checkout of branch feat/library-api or later.")
endif()
if(NOT TARGET XenosRecompLib)
    # Only the library: no command line tool, so none of zstd, xxHash, smol-v or DXC from its
    # submodules. Its fmt::fmt comes from the target that already exists (the SDK's); only a build
    # without one falls back to LAR-XenosRecomp's own thirdparty/fmt.
    set(XENOS_RECOMP_BUILD_TOOL OFF CACHE BOOL "" FORCE)
    add_subdirectory("${LARECOMP_XENOSRECOMP_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/lar-xenosrecomp" EXCLUDE_FROM_ALL)
endif()

# ---- headers from elsewhere -----------------------------------------------------------------
set(_larecomp_dxc_include "${LARECOMP_DXC_INCLUDE_DIR}")
if(NOT _larecomp_dxc_include)
    foreach(_candidate "${LARECOMP_SHADERPACK_REPO_ROOT}/thirdparty/dxc-bin/inc"
                       "${LARECOMP_XENOSRECOMP_DIR}/thirdparty/dxc-bin/inc")
        if(EXISTS "${_candidate}/dxcapi.h")
            set(_larecomp_dxc_include "${_candidate}")
            break()
        endif()
    endforeach()
endif()
if(NOT _larecomp_dxc_include)
    message(FATAL_ERROR
        "larecomp_shaderpack: dxcapi.h not found (looked in thirdparty/dxc-bin/inc and in "
        "LAR-XenosRecomp's thirdparty/dxc-bin/inc). Set LARECOMP_DXC_INCLUDE_DIR.")
endif()

set(_larecomp_xxhash_include "${LARECOMP_XXHASH_INCLUDE_DIR}")
if(NOT _larecomp_xxhash_include)
    foreach(_candidate "${LARECOMP_XENOSRECOMP_DIR}/thirdparty/xxHash")
        if(EXISTS "${_candidate}/xxhash.h")
            set(_larecomp_xxhash_include "${_candidate}")
            break()
        endif()
    endforeach()
endif()
if(NOT _larecomp_xxhash_include)
    message(FATAL_ERROR
        "larecomp_shaderpack: xxhash.h not found. Set LARECOMP_XXHASH_INCLUDE_DIR.")
endif()

# ---- the library -----------------------------------------------------------------------------
add_library(larecomp_shaderpack STATIC
    # shared
    hash.cpp
    log.cpp
    # game files
    image_scan.cpp
    xex_image.cpp
    rpf_shaders.cpp
    # the compiler
    dxc.cpp
    contract_checks.cpp
    # the builder
    containers.cpp
    hlsl_source.cpp
    pack_writer.cpp
    stamp.cpp
    builder.cpp
    # the pixel shader variant pack (builder.h VariantStep): the framework, then one file per variant
    variant_pack.cpp
    gbuffer_material_pack.cpp
    variants/hdr_scene.cpp
    variants/bloom_gauss.cpp
    variants/env_map.cpp
    variants/shadow_filter.cpp
    variants/glow_soft.cpp
    variants/car_selfrefl.cpp
    variants/car_chrome.cpp
    variants/car_chrome_city.cpp
    variants/luminance_scale.cpp
    variants/ui_sharp.cpp
    variants/headlight.cpp
    variants/gbuffer.cpp
    variants/gbuffer_night.cpp
    variants/gbuffer_day.cpp
    variants/gbuffer_cars.cpp
    # self tests, used by tools/shaderpack_tool
    input_selftest.cpp
    dxc_selftest.cpp
)
target_compile_features(larecomp_shaderpack PUBLIC cxx_std_23)
# Consumers include "shaderpack/builder.h" (and the others) from src/.
target_include_directories(larecomp_shaderpack PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_include_directories(larecomp_shaderpack PRIVATE
    "${_larecomp_dxc_include}"
    "${_larecomp_xxhash_include}")
# PRIVATE: the translator's headers are only used inside the library (consumers get the archive,
# not LAR-XenosRecomp's include path).
target_link_libraries(larecomp_shaderpack PRIVATE XenosRecompLib)
# The packs and the stamp are written with src/os/file_replace.h (parallel processes share the shader folder); the tools and tests that link this library get it with it.
include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/LarecompFileReplace.cmake")
target_link_libraries(larecomp_shaderpack PUBLIC larecomp_file_replace)

# ---- the HDR scene variant's curve -------------------------------------------------------------
# variants/hdr_scene.cpp pastes the HDR scene's curve into the game's tone map shader. The curve is a file of
# the HDR feature (src/gpu/hlsl/hdr_tonemap_shoulder.hlsli), not of this code: when it exists in the
# source tree (or where LARECOMP_HDR_HLSLI points) its bytes are turned into an array that is compiled
# into the library, so the HDR pack always matches the curve of the build that wrote it and the stamp
# can carry its hash. When it does not exist, BuiltInVariants() leaves the HDR scene variant out: no
# failure, the variant pack holds the other variants.
# The file is read when CMake configures (and CMake configures again when it changes). A build
# directory that was configured before the file existed has to be configured again.
set(_larecomp_hdr_hlsli "${LARECOMP_HDR_HLSLI}")
if(NOT _larecomp_hdr_hlsli)
    set(_larecomp_hdr_hlsli "${LARECOMP_SHADERPACK_REPO_ROOT}/src/gpu/hlsl/hdr_tonemap_shoulder.hlsli")
endif()
if(EXISTS "${_larecomp_hdr_hlsli}")
    set(_larecomp_hdr_generated "${CMAKE_CURRENT_BINARY_DIR}/generated")
    file(READ "${_larecomp_hdr_hlsli}" _larecomp_hdr_hex HEX)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," _larecomp_hdr_bytes "${_larecomp_hdr_hex}")
    # A closing 0 (not part of the file) keeps an empty file a valid array. file(GENERATE) leaves the
    # file alone when its text did not change, so configuring again does not rebuild the library.
    file(GENERATE OUTPUT "${_larecomp_hdr_generated}/hdr_hlsli_bytes.inc"
         CONTENT "${_larecomp_hdr_bytes}0x00\n")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_larecomp_hdr_hlsli}")
    target_include_directories(larecomp_shaderpack PRIVATE "${_larecomp_hdr_generated}")
    target_compile_definitions(larecomp_shaderpack PRIVATE LARECOMP_HDR_HLSLI_EMBEDDED=1)
    message(STATUS "larecomp_shaderpack: the HDR scene variant embeds ${_larecomp_hdr_hlsli}")
else()
    message(STATUS "larecomp_shaderpack: ${_larecomp_hdr_hlsli} does not exist: no HDR scene variant, "
                   "the variant pack is built without it")
endif()
# ---- the G-buffer variant's helper -------------------------------------------------------------
# variants/gbuffer.cpp pastes src/gpu/hlsl/gbuffer_common.hlsli (the interface's HLSL, shared with the host passes) into the game's lit shaders. Its
# bytes are turned into an array that is compiled into the library, the same way as the HDR curve above, so the pack always matches the file of the
# build that wrote it and the stamp carries its hash (variants_gbuffer_input). When the file does not exist BuiltInVariants() leaves the variant out.
set(_larecomp_gb_hlsli "${LARECOMP_SHADERPACK_REPO_ROOT}/src/gpu/hlsl/gbuffer_common.hlsli")
if(EXISTS "${_larecomp_gb_hlsli}")
    set(_larecomp_gb_generated "${CMAKE_CURRENT_BINARY_DIR}/generated")
    file(READ "${_larecomp_gb_hlsli}" _larecomp_gb_hex HEX)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," _larecomp_gb_bytes "${_larecomp_gb_hex}")
    file(GENERATE OUTPUT "${_larecomp_gb_generated}/gbuffer_hlsli_bytes.inc"
         CONTENT "${_larecomp_gb_bytes}0x00\n")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_larecomp_gb_hlsli}")
    target_include_directories(larecomp_shaderpack PRIVATE "${_larecomp_gb_generated}")
    target_compile_definitions(larecomp_shaderpack PRIVATE LARECOMP_GBUFFER_HLSLI_EMBEDDED=1)
    message(STATUS "larecomp_shaderpack: the G-buffer variant embeds ${_larecomp_gb_hlsli}")
else()
    message(STATUS "larecomp_shaderpack: ${_larecomp_gb_hlsli} does not exist: no G-buffer variant")
endif()
if(WIN32)
    target_compile_definitions(larecomp_shaderpack PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(larecomp_shaderpack PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
endif()
if(COMMAND rexglue_apply_target_settings)
    # The same flags the SDK applies to the rest of larecomp.
    rexglue_apply_target_settings(larecomp_shaderpack)
endif()

# The shadow filter variant's text transform has a check that needs no compiler, renderer or game files:
#   cmake --build <dir> --target larecomp_shadow_filter_test, then run src/shaderpack/larecomp_shadow_filter_test
#   [<folder of translated HLSL> [<folder for the results>]] (exit 0 = passed; variants/shadow_filter_test.cpp).
add_executable(larecomp_shadow_filter_test EXCLUDE_FROM_ALL
    variants/shadow_filter_test.cpp
    variants/shadow_filter.cpp)
target_compile_features(larecomp_shadow_filter_test PRIVATE cxx_std_23)
target_include_directories(larecomp_shadow_filter_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")

# The car self-reflection variant's text transform (variants/car_selfrefl.h) likewise:
#   cmake --build <dir> --target larecomp_car_selfrefl_test, then run src/shaderpack/larecomp_car_selfrefl_test
#   [<folder of translated HLSL> [<folder for the result>]] (exit 0 = passed; variants/car_selfrefl_test.cpp).
add_executable(larecomp_car_selfrefl_test EXCLUDE_FROM_ALL
    variants/car_selfrefl_test.cpp
    variants/car_selfrefl.cpp)
target_compile_features(larecomp_car_selfrefl_test PRIVATE cxx_std_23)
target_include_directories(larecomp_car_selfrefl_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")

# The car chrome variant's text transform (variants/car_chrome.h) likewise:
#   cmake --build <dir> --target larecomp_car_chrome_test, then run src/shaderpack/larecomp_car_chrome_test
#   [<folder of translated HLSL> [<folder for the result>]] (exit 0 = passed; variants/car_chrome_test.cpp).
add_executable(larecomp_car_chrome_test EXCLUDE_FROM_ALL
    variants/car_chrome_test.cpp
    variants/car_chrome.cpp
    variants/car_selfrefl.cpp)
target_compile_features(larecomp_car_chrome_test PRIVATE cxx_std_23)
target_include_directories(larecomp_car_chrome_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")

# The chrome's city stage (variants/car_chrome_city.h, CITY-RT M2 R3a: the chrome reads the colour of the ray traced image) on top of the chrome's, composed through the chain:
#   cmake --build <dir> --target larecomp_car_chrome_city_test, then run src/shaderpack/larecomp_car_chrome_city_test
#   [<folder of translated HLSL> [<folder for the result>]] (exit 0 = passed; variants/car_chrome_city_test.cpp).
add_executable(larecomp_car_chrome_city_test EXCLUDE_FROM_ALL
    variants/car_chrome_city_test.cpp
    variants/car_chrome_city.cpp
    variants/car_chrome.cpp
    variants/car_selfrefl.cpp)
target_compile_features(larecomp_car_chrome_city_test PRIVATE cxx_std_23)
target_include_directories(larecomp_car_chrome_city_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")

# The glow variant's text transform has a check that needs no compiler, renderer or game files:
#   cmake --build <dir> --target larecomp_glow_soft_test, then run src/shaderpack/larecomp_glow_soft_test
#   [<folder of translated HLSL> [<folder for the results>]] (exit 0 = passed; variants/glow_soft_test.cpp).
add_executable(larecomp_glow_soft_test EXCLUDE_FROM_ALL
    variants/glow_soft_test.cpp
    variants/glow_soft.cpp)
target_compile_features(larecomp_glow_soft_test PRIVATE cxx_std_23)
target_include_directories(larecomp_glow_soft_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")

# The rule that a pixel shader is in one shader variant only (variant_overlap.h), and the car paint's case (the car self-reflection
# variant edits it, the shadow filter's leaves it out), need no compiler, renderer or game files either:
#   cmake --build <dir> --target larecomp_variant_overlap_test, then run src/shaderpack/larecomp_variant_overlap_test
#   [<folder of translated HLSL>] (exit 0 = passed; variants/variant_overlap_test.cpp).
add_executable(larecomp_variant_overlap_test EXCLUDE_FROM_ALL
    variants/variant_overlap_test.cpp
    variants/car_selfrefl.cpp
    variants/shadow_filter.cpp)
target_compile_features(larecomp_variant_overlap_test PRIVATE cxx_std_23)
target_include_directories(larecomp_variant_overlap_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")

# The transform chain (variant_chain.h, variant_compose.h, variants/stage_constants.h, variant_pack_format.h): the variant word, the table, the
# constants' layout of every permutation, which permutations are built, the renderer's choice, composing synthetic stages, a vertex stage and
# the variant pack's file round trip with the refusal of other versions. Header only code, no compiler, renderer or game files:
#   cmake --build <dir> --target larecomp_variant_chain_test, then run src/shaderpack/larecomp_variant_chain_test (exit 0 = passed;
#   variants/variant_chain_test.cpp).
add_executable(larecomp_variant_chain_test EXCLUDE_FROM_ALL
    variants/variant_chain_test.cpp)
target_compile_features(larecomp_variant_chain_test PRIVATE cxx_std_23)
target_include_directories(larecomp_variant_chain_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")

# The chain against the retail text (variants/variant_golden_test.cpp): every built-in variant alone gives byte for byte the text it gave before the
# chain (against a golden file of hashes made by the pre-chain code), and each stage composed with a test stage compiles with dxc:
#   cmake --build <dir> --target larecomp_variant_golden_test, then run src/shaderpack/larecomp_variant_golden_test --hlsl <hlsl-rpf> --hlsl <hlsl-xex>
#   --hlsli src/gpu/hlsl/hdr_tonemap_shoulder.hlsli [--golden FILE] [--dxc <folder of dxc.exe>] (exit 0 = passed).
add_executable(larecomp_variant_golden_test EXCLUDE_FROM_ALL
    variants/variant_golden_test.cpp
    variants/hdr_scene.cpp
    variants/bloom_gauss.cpp
    variants/env_map.cpp
    variants/shadow_filter.cpp
    variants/glow_soft.cpp
    variants/car_selfrefl.cpp
    variants/car_chrome.cpp
    variants/luminance_scale.cpp
    variants/ui_sharp.cpp)
target_compile_features(larecomp_variant_golden_test PRIVATE cxx_std_23)
target_include_directories(larecomp_variant_golden_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
# The variants of the branches that are not in every tree (feat/headlights, feat/gbuffer-spike) are checked too when their files are there (variant_golden_test.cpp
# takes them with __has_include; the G-buffer's helper is `--gbuffer-hlsli src/gpu/hlsl/gbuffer_common.hlsli`): with both, the real pair is composed and compiled.
foreach(_larecomp_branch_variant IN ITEMS headlight gbuffer gbuffer_night gbuffer_day gbuffer_cars)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/variants/${_larecomp_branch_variant}.cpp")
        target_sources(larecomp_variant_golden_test PRIVATE variants/${_larecomp_branch_variant}.cpp)
    endif()
endforeach()

# The same for the luminance scale variant (variants/luminance_scale_test.cpp): cmake --build <dir> --target
# larecomp_luminance_scale_test, then run it [<translated PSSampleAverageLuminance.hlsl> [<folder for the result>]].
add_executable(larecomp_luminance_scale_test EXCLUDE_FROM_ALL
    variants/luminance_scale_test.cpp
    variants/luminance_scale.cpp)
target_compile_features(larecomp_luminance_scale_test PRIVATE cxx_std_23)
target_include_directories(larecomp_luminance_scale_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")

# The UI sharpening variant's text transform (variants/ui_sharp_test.cpp): cmake --build <dir> --target larecomp_ui_sharp_variant_test,
# then run it [<translated xrage_im__PS_Textured.hlsl> [<folder for the transformed text>]] (exit 0 = passed).
add_executable(larecomp_ui_sharp_variant_test EXCLUDE_FROM_ALL
    variants/ui_sharp_test.cpp
    variants/ui_sharp.cpp)
target_compile_features(larecomp_ui_sharp_variant_test PRIVATE cxx_std_23)
target_include_directories(larecomp_ui_sharp_variant_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
# The headlight variant's text transform (variants/headlight_test.cpp; the pixel shaders of the city's lit family): cmake --build <dir> --target
# larecomp_headlight_variant_test, then run it [<folder of translated HLSL> [<folder for the transformed texts>]] (exit 0 = passed).
add_executable(larecomp_headlight_variant_test EXCLUDE_FROM_ALL
    variants/headlight_test.cpp
    variants/headlight.cpp)
target_compile_features(larecomp_headlight_variant_test PRIVATE cxx_std_23)
target_include_directories(larecomp_headlight_variant_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
# The G-buffer variant's text transform (variants/gbuffer_test.cpp; xCityDefault's night shader, and the dispatch to the night and day stages): cmake --build <dir> --target
# larecomp_gbuffer_variant_test, then run it [<src/gpu/hlsl/gbuffer_common.hlsli> [<folder of translated HLSL> [<folder for the transformed texts>]]] (exit 0 = passed).
add_executable(larecomp_gbuffer_variant_test EXCLUDE_FROM_ALL
    variants/gbuffer_test.cpp
    variants/gbuffer.cpp
    variants/gbuffer_night.cpp
    variants/gbuffer_day.cpp
    variants/gbuffer_cars.cpp
    hash.cpp)
target_compile_features(larecomp_gbuffer_variant_test PRIVATE cxx_std_23)
target_include_directories(larecomp_gbuffer_variant_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/.." "${_larecomp_xxhash_include}")
# The G-buffer variant's night stage (variants/gbuffer_night_test.cpp; package GB-C: the PSMultiLight, ...Env and ...Interior shaders): its anchors on small texts, and with a folder of translated HLSL
# the census of the retail shaders (edited, or left out and why), the colour identity of every edited text, and the composition with the headlight's stage. cmake --build <dir> --target
# larecomp_gbuffer_night_test, then run it [<src/gpu/hlsl/gbuffer_common.hlsli> [<folder of translated HLSL> [<folder for the transformed texts>]]] (exit 0 = passed).
add_executable(larecomp_gbuffer_night_test EXCLUDE_FROM_ALL
    variants/gbuffer_night_test.cpp
    variants/gbuffer_night.cpp
    variants/gbuffer_day.cpp
    variants/gbuffer_cars.cpp
    variants/gbuffer.cpp
    variants/headlight.cpp
    hash.cpp)
target_compile_features(larecomp_gbuffer_night_test PRIVATE cxx_std_23)
target_include_directories(larecomp_gbuffer_night_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/.." "${_larecomp_xxhash_include}")
# The G-buffer's day families (variants/gbuffer_day.h: 42 pixel shaders with their normal sources, and the vertex stage's three shaders): cmake --build <dir> --target
# larecomp_gbuffer_day_test, then run it [<src/gpu/hlsl/gbuffer_common.hlsli> [<folder of translated HLSL> [<folder for the transformed texts>]]] (exit 0 = passed).
add_executable(larecomp_gbuffer_day_test EXCLUDE_FROM_ALL
    variants/gbuffer_day_test.cpp
    variants/gbuffer_day.cpp
    hash.cpp)
target_compile_features(larecomp_gbuffer_day_test PRIVATE cxx_std_23)
target_include_directories(larecomp_gbuffer_day_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/.." "${_larecomp_xxhash_include}")
# The G-buffer's car, traffic and character families (variants/gbuffer_cars.h, package GB-G1: 73 shaders, the table, the transform at 1 to 3 planes, the colour identity, the composition with the car
# variants, the host's word for each class): cmake --build <dir> --target
# larecomp_gbuffer_cars_test, then run it [<src/gpu/hlsl/gbuffer_common.hlsli> [<folder of translated HLSL> [<folder for the transformed texts>]]] (exit 0 = passed).
add_executable(larecomp_gbuffer_cars_test EXCLUDE_FROM_ALL
    variants/gbuffer_cars_test.cpp
    variants/gbuffer_cars.cpp
    variants/gbuffer_day.cpp
    variants/car_selfrefl.cpp
    variants/car_chrome.cpp
    variants/car_chrome_city.cpp
    hash.cpp)
target_compile_features(larecomp_gbuffer_cars_test PRIVATE cxx_std_23)
target_include_directories(larecomp_gbuffer_cars_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/.." "${_larecomp_xxhash_include}")
# The bloom fix variant's text transform, and the proof that its two bilinear fetches at 3x give the three sub-taps' sum, likewise:
#   cmake --build <dir> --target larecomp_bloom_gauss_test, then run src/shaderpack/larecomp_bloom_gauss_test
#   [<folder of translated HLSL> [<folder for the results>]] (exit 0 = passed; variants/bloom_gauss_test.cpp).
add_executable(larecomp_bloom_gauss_test EXCLUDE_FROM_ALL
    variants/bloom_gauss_test.cpp
    variants/bloom_gauss.cpp)
target_compile_features(larecomp_bloom_gauss_test PRIVATE cxx_std_23)
target_include_directories(larecomp_bloom_gauss_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")

# The DXIL feature flags reader (dxil_features.h): a variant's blob must not use double precision (an Intel iGPU on D3D12 cannot create the pipeline).
#   cmake --build <dir> --target larecomp_dxil_features_test, then run src/shaderpack/larecomp_dxil_features_test (exit 0 = passed; dxil_features_test.cpp).
add_executable(larecomp_dxil_features_test EXCLUDE_FROM_ALL
    dxil_features_test.cpp)
target_compile_features(larecomp_dxil_features_test PRIVATE cxx_std_23)
target_include_directories(larecomp_dxil_features_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
