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
# tool compiles them itself): src/mc_engine/modloader/{rpf3,aes256,texture,objmesh}.cpp and
# src/native_gfx/shader_identity.cpp.
#
# Cache variables:
#   LARECOMP_XENOSRECOMP_DIR     the LAR-XenosRecomp checkout (default thirdparty/LAR-XenosRecomp;
#                                pass a clone's path for a local build)
#   LARECOMP_DXC_INCLUDE_DIR     the folder with dxcapi.h (default thirdparty/dxc-bin/inc, else the
#                                one inside LAR-XenosRecomp's own thirdparty/dxc-bin)
#   LARECOMP_XXHASH_INCLUDE_DIR  the folder with xxhash.h (default: the SDK's, else the one inside
#                                LAR-XenosRecomp's thirdparty/xxHash)

get_filename_component(LARECOMP_SHADERPACK_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

set(LARECOMP_XENOSRECOMP_DIR "${LARECOMP_SHADERPACK_REPO_ROOT}/thirdparty/LAR-XenosRecomp"
    CACHE PATH "LAR-XenosRecomp checkout (the shader translator, library target XenosRecompLib)")
set(LARECOMP_DXC_INCLUDE_DIR "" CACHE PATH "Folder with dxcapi.h (empty: look in thirdparty/dxc-bin/inc)")
set(LARECOMP_XXHASH_INCLUDE_DIR "" CACHE PATH "Folder with xxhash.h (empty: the SDK's, or LAR-XenosRecomp's)")

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
if(NOT _larecomp_xxhash_include AND TARGET rex::xxhash)
    get_target_property(_larecomp_xxhash_include rex::xxhash INTERFACE_INCLUDE_DIRECTORIES)
endif()
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
