# Microsoft's DirectX Shader Compiler for Linux (libdxcompiler.so): what LARecomp builds its SPIR-V shader
# packs with on Linux, as dxcompiler.dll is on Windows (src/shaderpack/dxc.h, bootprogress/
# larecomp_shader_build.cpp: the compiler is looked for in <exe folder>/D3D12/). Same release as the
# Windows DLL in assets/D3D12 (v1.9.2609, SOURCE-dxc.txt). Nothing is committed: the release archive is
# fetched once into the build folder and checked against its SHA-256, and the two files the game folder
# needs are taken out of it. libdxil.so (the DXIL signer, Microsoft's licence "solely for use on
# Windows") is not used and not copied: Linux builds SPIR-V only.
#
# Sets LARECOMP_DXC_LINUX_LIB (the libdxcompiler.so to copy next to the executable) and
# LARECOMP_DXC_LINUX_LICENCE (its LICENSE-LLVM.txt); both empty when nothing could be found.
# LARECOMP_DXC_LINUX_DIR names an unpacked archive (a folder holding lib/libdxcompiler.so) and
# LARECOMP_DXC_LINUX_ARCHIVE a local copy of the .tar.gz, for a build without network. When neither
# works, the older dxc of the thirdparty/dxc-bin submodule (v1.8.2407) is used with a warning.
#
# CHECK: not run (written on Windows, 2026-11-07). The archive name is the release's own, spelled
# "x86_x64" there (checked against the GitHub release API); the SHA-256 values were taken from the
# archive downloaded that day.

set(LARECOMP_DXC_LINUX_LIB "")
set(LARECOMP_DXC_LINUX_LICENCE "")

set(LARECOMP_DXC_LINUX_DIR "" CACHE PATH
    "An unpacked DXC Linux release (the folder holding lib/libdxcompiler.so); empty fetches the pinned one")
set(LARECOMP_DXC_LINUX_ARCHIVE "" CACHE FILEPATH
    "A local copy of linux_dxc_2026_09_28.x86_x64.tar.gz, used instead of the download")
option(LARECOMP_DXC_LINUX_FETCH "Fetch the pinned DXC Linux release when LARECOMP_DXC_LINUX_DIR is unset" ON)

set(_dxcl_version "1.9.2609")
set(_dxcl_url "https://github.com/microsoft/DirectXShaderCompiler/releases/download/v${_dxcl_version}/linux_dxc_2026_09_28.x86_x64.tar.gz")
set(_dxcl_archive_sha256 "96faadc7f5c282d2ffda49804beb4c3ee38127bc252b723234e3c5cdf7aa39a1")
set(_dxcl_lib_sha256 "d40e4a7b3b46490313f4bfd08101946483a673831b3ea65c93d746de1c2f9216")
set(_dxcl_licence_sha256 "729615317e28dd03907e46f0fc3b5e88f7853cee61d1a1471d2749335516b46f")
set(_dxcl_root "${CMAKE_BINARY_DIR}/_deps/dxc-linux-${_dxcl_version}")

if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    message(STATUS "larecomp: no DXC Linux release for ${CMAKE_SYSTEM_PROCESSOR}: shader packs need "
                   "LARECOMP_DXC_LINUX_DIR (a folder with lib/libdxcompiler.so)")
endif()

if(LARECOMP_DXC_LINUX_DIR)
    if(EXISTS "${LARECOMP_DXC_LINUX_DIR}/lib/libdxcompiler.so")
        set(LARECOMP_DXC_LINUX_LIB "${LARECOMP_DXC_LINUX_DIR}/lib/libdxcompiler.so")
        if(EXISTS "${LARECOMP_DXC_LINUX_DIR}/LICENSE-LLVM.txt")
            set(LARECOMP_DXC_LINUX_LICENCE "${LARECOMP_DXC_LINUX_DIR}/LICENSE-LLVM.txt")
        endif()
    else()
        message(WARNING "LARECOMP_DXC_LINUX_DIR=${LARECOMP_DXC_LINUX_DIR} holds no lib/libdxcompiler.so")
    endif()
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    # Already unpacked and still matching its hash: nothing to do.
    set(_dxcl_have_lib FALSE)
    if(EXISTS "${_dxcl_root}/lib/libdxcompiler.so")
        file(SHA256 "${_dxcl_root}/lib/libdxcompiler.so" _dxcl_have)
        if(_dxcl_have STREQUAL _dxcl_lib_sha256)
            set(_dxcl_have_lib TRUE)
        endif()
    endif()
    if(NOT _dxcl_have_lib AND LARECOMP_DXC_LINUX_FETCH)
        set(_dxcl_archive "${LARECOMP_DXC_LINUX_ARCHIVE}")
        if(NOT _dxcl_archive)
            set(_dxcl_archive "${CMAKE_BINARY_DIR}/_deps/linux_dxc_${_dxcl_version}.tar.gz")
            if(NOT EXISTS "${_dxcl_archive}")
                file(DOWNLOAD "${_dxcl_url}" "${_dxcl_archive}.part" STATUS _dxcl_status TLS_VERIFY ON)
                list(GET _dxcl_status 0 _dxcl_code)
                if(_dxcl_code EQUAL 0)
                    file(RENAME "${_dxcl_archive}.part" "${_dxcl_archive}")
                else()
                    list(GET _dxcl_status 1 _dxcl_message)
                    file(REMOVE "${_dxcl_archive}.part")
                    message(WARNING "DXC: could not fetch ${_dxcl_url} (${_dxcl_message})")
                    set(_dxcl_archive "")
                endif()
            endif()
        endif()
        if(_dxcl_archive AND EXISTS "${_dxcl_archive}")
            file(SHA256 "${_dxcl_archive}" _dxcl_archive_have)
            if(NOT _dxcl_archive_have STREQUAL _dxcl_archive_sha256)
                message(FATAL_ERROR "DXC: ${_dxcl_archive} does not match its SHA-256 "
                                    "(got ${_dxcl_archive_have}, expected ${_dxcl_archive_sha256})")
            endif()
            file(REMOVE_RECURSE "${_dxcl_root}")
            file(MAKE_DIRECTORY "${_dxcl_root}")
            file(ARCHIVE_EXTRACT INPUT "${_dxcl_archive}" DESTINATION "${_dxcl_root}")
            if(EXISTS "${_dxcl_root}/lib/libdxcompiler.so")
                file(SHA256 "${_dxcl_root}/lib/libdxcompiler.so" _dxcl_have)
                if(_dxcl_have STREQUAL _dxcl_lib_sha256)
                    set(_dxcl_have_lib TRUE)
                else()
                    message(FATAL_ERROR "DXC: lib/libdxcompiler.so of the archive does not match its "
                                        "SHA-256 (got ${_dxcl_have}, expected ${_dxcl_lib_sha256})")
                endif()
            else()
                message(WARNING "DXC: the archive holds no lib/libdxcompiler.so")
            endif()
        endif()
    endif()
    if(_dxcl_have_lib)
        set(LARECOMP_DXC_LINUX_LIB "${_dxcl_root}/lib/libdxcompiler.so")
        if(EXISTS "${_dxcl_root}/LICENSE-LLVM.txt")
            file(SHA256 "${_dxcl_root}/LICENSE-LLVM.txt" _dxcl_have)
            if(_dxcl_have STREQUAL _dxcl_licence_sha256)
                set(LARECOMP_DXC_LINUX_LICENCE "${_dxcl_root}/LICENSE-LLVM.txt")
            endif()
        endif()
        message(STATUS "larecomp: DXC ${_dxcl_version} for Linux fetched and checked in ${_dxcl_root}")
    endif()
endif()

# Nothing found: the submodule's older dxc, which builds SPIR-V too.
if(NOT LARECOMP_DXC_LINUX_LIB AND
   EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/dxc-bin/lib/x64/libdxcompiler.so")
    set(LARECOMP_DXC_LINUX_LIB "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/dxc-bin/lib/x64/libdxcompiler.so")
    message(WARNING "larecomp: no DXC ${_dxcl_version} for Linux: the shader packs will be built with the "
                    "older dxc of thirdparty/dxc-bin (v1.8.2407). Set LARECOMP_DXC_LINUX_ARCHIVE to a "
                    "local copy of the release archive to build offline.")
endif()
if(NOT LARECOMP_DXC_LINUX_LIB)
    message(WARNING "larecomp: no libdxcompiler.so for Linux: the game cannot build its shader packs "
                    "(the first start stops with the shader compiler missing)")
endif()
