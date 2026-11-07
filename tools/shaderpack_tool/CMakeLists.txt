# larecomp_shaderpack_tool: the shader pack builder from the command line (dev tool).
#
#   larecomp_shaderpack_tool build        --game <dir> --image <flat image> --out <dir> --dxc <dir>
#                                         [--formats dxil|spirv|both] [--threads N] [--force]
#   larecomp_shaderpack_tool check-inputs --game <dir> [--image <flat image>] [--rpf-ref <dir>] [--xex-ref <dir>]
#   larecomp_shaderpack_tool dxc-selftest --dxc <dir> --pack <pack> --hlsl <dir> [--sample N]
#   larecomp_shaderpack_tool validate     <pack> [--formats dxil|spirv|both] [--no-blobs]
#   larecomp_shaderpack_tool stamp        --out <dir> --game <dir> --dxc <dir> [--formats ...]
#
# Two ways to build it:
#   * Inside the larecomp project: the top-level CMakeLists.txt adds this directory with
#     EXCLUDE_FROM_ALL, so   cmake --build <dir> --target larecomp_shaderpack_tool   builds it.
#   * Standalone, from a LAR-XenosRecomp checkout alone (no ReXGlue SDK, no Plume):
#       cmake -S tools/shaderpack_tool -B <build dir> -G Ninja -DCMAKE_BUILD_TYPE=Release
#             -DCMAKE_CXX_COMPILER=<clang++> -DLARECOMP_XENOSRECOMP_DIR=<LAR-XenosRecomp>
#       cmake --build <build dir>
#     LAR-XenosRecomp's own thirdparty/ then supplies fmt, the dxc headers and xxhash (its
#     submodules have to be checked out).

cmake_minimum_required(VERSION 3.25)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(larecomp_shaderpack_tool LANGUAGES CXX)
    set(CMAKE_CXX_STANDARD 23)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    if(NOT LARECOMP_XENOSRECOMP_DIR)
        message(FATAL_ERROR "Pass -DLARECOMP_XENOSRECOMP_DIR=<LAR-XenosRecomp checkout>")
    endif()
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../src/shaderpack" shaderpack)
endif()

get_filename_component(_larecomp_repo "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

# What larecomp.exe already has in LARECOMP_SOURCES, and the shader pack code calls into.
add_executable(larecomp_shaderpack_tool
    main.cpp
    "${_larecomp_repo}/src/mc_engine/modloader/aes256.cpp"
    "${_larecomp_repo}/src/mc_engine/modloader/objmesh.cpp"
    "${_larecomp_repo}/src/mc_engine/modloader/rpf3.cpp"
    "${_larecomp_repo}/src/mc_engine/modloader/texture.cpp"
    "${_larecomp_repo}/src/native_gfx/shader_identity.cpp"
)
target_link_libraries(larecomp_shaderpack_tool PRIVATE larecomp_shaderpack)
if(WIN32)
    target_compile_definitions(larecomp_shaderpack_tool PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
endif()
