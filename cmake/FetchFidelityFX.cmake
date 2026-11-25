# AMD FidelityFX SDK 2.4.0 (MIT; its signed DLLs may be redistributed with the licence notice): the headers the temporal upscaler is
# compiled with and the DLLs the game folder ships. Fetched once into the build folder from AMD's repository at a fixed commit, each
# file checked against its SHA-256, instead of committing ~94 MB of binaries. Sets LARECOMP_FFX_SDK_DIR to the fetched copy.
# A file that does not match its hash stops the configure; no network leaves FSR out of the build (the game runs with Upscaling Off).
# LARECOMP_FFX_SDK_DIR set by hand (a local SDK checkout) skips all of this.

include("${CMAKE_CURRENT_LIST_DIR}/ReadDeps.cmake")
larecomp_read_kit(fidelityfx _ffx_kit)
string(JSON _ffx_version GET "${_ffx_kit}" version)
string(JSON _ffx_base GET "${_ffx_kit}" url)
set(_ffx_root "${CMAKE_BINARY_DIR}/_deps/fidelityfx-sdk-${_ffx_version}")
larecomp_read_kit_pins("${_ffx_kit}" _ffx_files)

set(_ffx_ok TRUE)
foreach(_ffx_entry IN LISTS _ffx_files)
    string(SUBSTRING "${_ffx_entry}" 0 64 _ffx_hash)
    string(SUBSTRING "${_ffx_entry}" 65 -1 _ffx_path)
    set(_ffx_local "${_ffx_root}/${_ffx_path}")
    if(EXISTS "${_ffx_local}")
        file(SHA256 "${_ffx_local}" _ffx_have)
        if(_ffx_have STREQUAL _ffx_hash)
            continue()
        endif()
        file(REMOVE "${_ffx_local}")
    endif()
    file(DOWNLOAD "${_ffx_base}/${_ffx_path}" "${_ffx_local}.part" STATUS _ffx_status TLS_VERIFY ON)
    list(GET _ffx_status 0 _ffx_code)
    if(NOT _ffx_code EQUAL 0)
        list(GET _ffx_status 1 _ffx_message)
        file(REMOVE "${_ffx_local}.part")
        message(WARNING "FidelityFX: could not fetch ${_ffx_path} (${_ffx_message}); building without FSR. "
                        "Set LARECOMP_FFX_SDK_DIR to a local FidelityFX SDK ${_ffx_version} checkout to build it offline.")
        set(_ffx_ok FALSE)
        break()
    endif()
    file(SHA256 "${_ffx_local}.part" _ffx_have)
    if(NOT _ffx_have STREQUAL _ffx_hash)
        file(REMOVE "${_ffx_local}.part")
        message(FATAL_ERROR "FidelityFX: ${_ffx_path} does not match its SHA-256 (got ${_ffx_have}, expected ${_ffx_hash})")
    endif()
    file(RENAME "${_ffx_local}.part" "${_ffx_local}")
endforeach()
if(_ffx_ok)
    # A plain variable, not the cache: the next configure checks the files again (CMakeLists.txt).
    set(LARECOMP_FFX_SDK_DIR "${_ffx_root}")
    message(STATUS "larecomp: FidelityFX SDK ${_ffx_version} files fetched and checked in ${_ffx_root}")
endif()
