# NVIDIA Streamline SDK 2.14.16, for DLSS Super Resolution (src/gpu/temporal_dlss.h): the headers the
# DLSS backend is compiled with (Streamline's own, MIT) and the production DLLs the game folder carries
# (sl.interposer.dll, sl.common.dll, sl.dlss.dll and nvngx_dlss.dll, NVIDIA-signed, with their licence
# files). Nothing from NVIDIA is committed to this repository: everything is fetched once into the build
# folder from NVIDIA's release (one zip, the same as `streamline-sdk-v2.14.16.zip` on
# github.com/NVIDIA-RTX/Streamline/releases), and every file that is used is checked against its SHA-256.
# F4-DESIGN.md section 15 (the NVIDIA RTX SDKs licence allows redistributing the binaries; fetching them
# on the builder's PC keeps this repository, and anyone who builds it, out of the flow-down terms).
#
# Sets LARECOMP_STREAMLINE_DIR to the fetched copy (a folder with include/ and bin/x64/).
# No network, or a file that is not pinned yet: DLSS is left out of the build (the game runs with it
# unavailable). A file that does not match its hash stops the configure.
# LARECOMP_STREAMLINE_DIR set by hand (a Streamline SDK folder, the zip unpacked) skips all of this;
# LARECOMP_STREAMLINE_ZIP names a local copy of the zip to use instead of the 276 MB download.
#
# The first configure with a hash still marked CHECK prints the hash of that file: fill it in.

set(_sl_version "2.14.16")
set(_sl_zip_url "https://github.com/NVIDIA-RTX/Streamline/releases/download/v${_sl_version}/streamline-sdk-v${_sl_version}.zip")
# The SHA-256 of the whole zip, as GitHub's release API lists it for the asset (its "digest" field).
# CHECK: verify with `gh api repos/NVIDIA-RTX/Streamline/releases/tags/v2.14.16 --jq '.assets[] | [.name, .digest]'`
set(_sl_zip_sha256 "32041e51db0cab709a271b72a352281b690c2abbd669a827809401a5dd23956b")
set(_sl_root "${CMAKE_BINARY_DIR}/_deps/streamline-${_sl_version}")

# "<sha256> <path in the SDK>", the path under the zip's SDK root (package.bat of Streamline lays out
# include/, bin/x64/ and the licence files at the root). The same path is used under _sl_root.
# CHECK: every hash below (the first configure prints the real ones), and that the zip has these
# paths (the headers sl.h pulls in, the production DLLs in bin/x64 and not bin/x64/development, the
# licence files).
set(_sl_files
    "CHECK include/sl.h"
    "CHECK include/sl_appidentity.h"
    "CHECK include/sl_consts.h"
    "CHECK include/sl_core_api.h"
    "CHECK include/sl_core_types.h"
    "CHECK include/sl_device_wrappers.h"
    "CHECK include/sl_dlss.h"
    "CHECK include/sl_result.h"
    "CHECK include/sl_security.h"
    "CHECK include/sl_struct.h"
    "CHECK include/sl_version.h"
    "CHECK bin/x64/sl.interposer.dll"
    "CHECK bin/x64/sl.common.dll"
    "CHECK bin/x64/sl.dlss.dll"
    "CHECK bin/x64/nvngx_dlss.dll"
    "CHECK bin/x64/nvngx_dlss.license.txt"
    "CHECK license.txt"
    "CHECK 3rd-party-licenses.md")

# The file of the extracted zip whose path ends with /<path> (or is <path>), by exact trailing
# components so that bin/x64/development/x.dll is never taken for bin/x64/x.dll; the shallowest
# when several do.
function(_larecomp_sl_find extract_dir path out_var)
    file(GLOB_RECURSE _all "${extract_dir}/*")
    string(LENGTH "${path}" _plen)
    set(_found "")
    set(_found_len 0)
    foreach(_f IN LISTS _all)
        string(LENGTH "${_f}" _flen)
        if(_flen LESS _plen)
            continue()
        endif()
        math(EXPR _start "${_flen} - ${_plen}")
        string(SUBSTRING "${_f}" ${_start} -1 _tail)
        if(NOT _tail STREQUAL path)
            continue()
        endif()
        if(_start GREATER 0)
            math(EXPR _before "${_start} - 1")
            string(SUBSTRING "${_f}" ${_before} 1 _sep)
            if(NOT _sep STREQUAL "/")
                continue()
            endif()
        endif()
        # The shallowest one: the SDK root's license.txt, not one inside some folder of it.
        if(NOT _found OR _flen LESS _found_len)
            set(_found "${_f}")
            set(_found_len ${_flen})
        endif()
    endforeach()
    set(${out_var} "${_found}" PARENT_SCOPE)
endfunction()

set(_sl_ok TRUE)
set(_sl_missing "")
foreach(_sl_entry IN LISTS _sl_files)
    string(FIND "${_sl_entry}" " " _sl_space)
    string(SUBSTRING "${_sl_entry}" 0 ${_sl_space} _sl_hash)
    math(EXPR _sl_path_at "${_sl_space} + 1")
    string(SUBSTRING "${_sl_entry}" ${_sl_path_at} -1 _sl_path)
    set(_sl_local "${_sl_root}/${_sl_path}")
    if(EXISTS "${_sl_local}")
        file(SHA256 "${_sl_local}" _sl_have)
        if(_sl_hash STREQUAL "CHECK")
            message(WARNING "Streamline: ${_sl_path} has no SHA-256 pinned yet (cmake/FetchStreamline.cmake); "
                            "it hashes to ${_sl_have}. Building without DLSS until it is.")
            set(_sl_ok FALSE)
            continue()
        endif()
        if(_sl_have STREQUAL _sl_hash)
            continue()
        endif()
        file(REMOVE "${_sl_local}")
    endif()
    list(APPEND _sl_missing "${_sl_entry}")
endforeach()

if(_sl_missing)
    set(_sl_zip "")
    set(_sl_zip_ours FALSE)
    if(LARECOMP_STREAMLINE_ZIP)
        set(_sl_zip "${LARECOMP_STREAMLINE_ZIP}")
    else()
        set(_sl_zip "${_sl_root}/streamline-sdk-v${_sl_version}.zip")
        set(_sl_zip_ours TRUE)
        if(NOT EXISTS "${_sl_zip}")
            message(STATUS "larecomp: fetching Streamline ${_sl_version} (about 276 MB, once) from ${_sl_zip_url}")
            file(DOWNLOAD "${_sl_zip_url}" "${_sl_zip}.part" SHOW_PROGRESS INACTIVITY_TIMEOUT 120
                 STATUS _sl_status TLS_VERIFY ON)
            list(GET _sl_status 0 _sl_code)
            if(NOT _sl_code EQUAL 0)
                list(GET _sl_status 1 _sl_message)
                file(REMOVE "${_sl_zip}.part")
                message(WARNING "Streamline: could not fetch ${_sl_zip_url} (${_sl_message}); building without DLSS. "
                                "Set LARECOMP_STREAMLINE_ZIP to a local streamline-sdk-v${_sl_version}.zip, or "
                                "LARECOMP_STREAMLINE_DIR to an unpacked SDK, to build it offline.")
                set(_sl_ok FALSE)
            else()
                file(RENAME "${_sl_zip}.part" "${_sl_zip}")
            endif()
        endif()
    endif()
    if(_sl_ok AND NOT EXISTS "${_sl_zip}")
        message(WARNING "Streamline: ${_sl_zip} does not exist; building without DLSS.")
        set(_sl_ok FALSE)
    endif()
    if(_sl_ok)
        file(SHA256 "${_sl_zip}" _sl_zip_have)
        if(NOT _sl_zip_have STREQUAL _sl_zip_sha256)
            if(_sl_zip_ours)
                file(REMOVE "${_sl_zip}")
            endif()
            message(FATAL_ERROR "Streamline: ${_sl_zip} does not match its SHA-256 (got ${_sl_zip_have}, "
                                "expected ${_sl_zip_sha256})")
        endif()
        set(_sl_extract "${_sl_root}/_extract")
        file(REMOVE_RECURSE "${_sl_extract}")
        set(_sl_patterns "")
        foreach(_sl_entry IN LISTS _sl_missing)
            string(FIND "${_sl_entry}" " " _sl_space)
            math(EXPR _sl_path_at "${_sl_space} + 1")
            string(SUBSTRING "${_sl_entry}" ${_sl_path_at} -1 _sl_path)
            list(APPEND _sl_patterns "*${_sl_path}")
        endforeach()
        # CHECK: libarchive's patterns let `*` match `/`, so a zip with or without a top folder works.
        file(ARCHIVE_EXTRACT INPUT "${_sl_zip}" DESTINATION "${_sl_extract}" PATTERNS ${_sl_patterns})
        foreach(_sl_entry IN LISTS _sl_missing)
            string(FIND "${_sl_entry}" " " _sl_space)
            string(SUBSTRING "${_sl_entry}" 0 ${_sl_space} _sl_hash)
            math(EXPR _sl_path_at "${_sl_space} + 1")
            string(SUBSTRING "${_sl_entry}" ${_sl_path_at} -1 _sl_path)
            _larecomp_sl_find("${_sl_extract}" "${_sl_path}" _sl_src)
            if(NOT _sl_src)
                message(WARNING "Streamline: ${_sl_path} is not in ${_sl_zip}; building without DLSS.")
                set(_sl_ok FALSE)
                continue()
            endif()
            set(_sl_local "${_sl_root}/${_sl_path}")
            get_filename_component(_sl_dir "${_sl_local}" DIRECTORY)
            file(MAKE_DIRECTORY "${_sl_dir}")
            file(COPY_FILE "${_sl_src}" "${_sl_local}" ONLY_IF_DIFFERENT)
            file(SHA256 "${_sl_local}" _sl_have)
            if(_sl_hash STREQUAL "CHECK")
                message(WARNING "Streamline: ${_sl_path} has no SHA-256 pinned yet (cmake/FetchStreamline.cmake); "
                                "it hashes to ${_sl_have}. Building without DLSS until it is.")
                set(_sl_ok FALSE)
            elseif(NOT _sl_have STREQUAL _sl_hash)
                file(REMOVE "${_sl_local}")
                file(REMOVE_RECURSE "${_sl_extract}")
                message(FATAL_ERROR "Streamline: ${_sl_path} does not match its SHA-256 (got ${_sl_have}, "
                                    "expected ${_sl_hash})")
            endif()
        endforeach()
        file(REMOVE_RECURSE "${_sl_extract}")
        if(_sl_zip_ours)
            file(REMOVE "${_sl_zip}")  # the files that are used are kept, checked, beside it
        endif()
    endif()
endif()

if(_sl_ok)
    set(LARECOMP_STREAMLINE_DIR "${_sl_root}" CACHE PATH
        "NVIDIA Streamline SDK (the folder holding include/ and bin/x64/); empty builds without DLSS" FORCE)
    message(STATUS "larecomp: Streamline ${_sl_version} files fetched and checked in ${_sl_root}")
endif()
