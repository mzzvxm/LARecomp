# AMD FidelityFX SDK 2.4.0 (MIT; its signed DLLs may be redistributed with the licence notice): the headers the temporal upscaler is
# compiled with and the DLLs the game folder ships. Fetched once into the build folder from AMD's repository at a fixed commit, each
# file checked against its SHA-256, instead of committing ~94 MB of binaries. Sets LARECOMP_FFX_SDK_DIR to the fetched copy.
# A file that does not match its hash stops the configure; no network leaves FSR out of the build (the game runs with Upscaling Off).
# LARECOMP_FFX_SDK_DIR set by hand (a local SDK checkout) skips all of this.

set(_ffx_commit "7539ae8b6568ad29ed78a753404351daf3b5cca7")  # tag v2.4.0, "AMD FSR SDK 2.4.0"
set(_ffx_base "https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/${_ffx_commit}")
set(_ffx_root "${CMAKE_BINARY_DIR}/_deps/fidelityfx-sdk-2.4.0")
set(_ffx_files
    "e2191d3c0828be45eb8affd4e3ec0378f2293adc345b772a7170edcf3b021ecb Kits/FidelityFX/signedbin/amd_fidelityfx_loader_dx12.dll"
    "38a56a01fb3565c0757d9f5c1e4e69de4b65e5f2e5769b323b200dc1f142bab9 Kits/FidelityFX/signedbin/amd_fidelityfx_loader_vk.dll"
    "aad4fccccb29084a655f9a4f9f613bd22c8c03fb4a0f8840148cc6543b4ccace Kits/FidelityFX/signedbin/amd_fidelityfx_upscaler_dx12.dll"
    "4a7e895af6e0b615d40a84d59eb8c2b1054ad91cdd0c739567940270d51bdab7 Kits/FidelityFX/signedbin/amd_fidelityfx_upscaler_vk.dll"
    "32357259b965d6a8001e9e508ba60eda6034027c5923a7d226ffbaff813c2bfb Kits/FidelityFX/docs/license.md"
    "c9afbc3c4c673723e9e5369a666f0ff54560543b5a93537f8ff453bf1c3bf530 Kits/FidelityFX/api/include/dx12/ffx_api_dx12.h"
    "a4f02dcf86ed5c05f76b3cb83d8b3410f0b8c96f5d6042e48cca30dd25bac7f2 Kits/FidelityFX/api/include/dx12/ffx_api_dx12.hpp"
    "4d9dcf243ef5efbf03340a02f16b51a22053243a22e8a9a0b3ce91c26982b4bf Kits/FidelityFX/api/include/ffx_api.h"
    "af441486342abfeb189c5672f3216a28446b582ad314ec4e754b81e20bb8a840 Kits/FidelityFX/api/include/ffx_api.hpp"
    "18d1901dbbc881e8131b91c65ad44bb3b4a1de6c31435d5b75236f16727ba80e Kits/FidelityFX/api/include/ffx_api_loader.h"
    "f37055b1980562b75295ac0ee3f4b60dd314c8c2f21a41b7d71d9f9070858bd5 Kits/FidelityFX/api/include/ffx_api_matrix_helpers.h"
    "8f671dadf9e1267dd8a51d224e11b27656ddb9da51ef9524a177b89114548737 Kits/FidelityFX/api/include/ffx_api_matrix_helpers.hpp"
    "53f07c113a9ba493f297656e4481b6539aed0afc9f3cc8da682aa477d270c46c Kits/FidelityFX/api/include/ffx_api_types.h"
    "09ac1855bc4c2f711243d6982909385072ca3f8d30db44d1a82ee5bd637832ad Kits/FidelityFX/api/include/vk/ffx_api_vk.h"
    "7757c3ffa6b1e189bca5eec3b8c93aa8f802386642648a0dfeb8c2c52502e5ba Kits/FidelityFX/api/include/vk/ffx_api_vk.hpp"
    "13091e7fe5fd9c82b1fb7c044b9f08f664d9653f599d2ea500738709236fca4d Kits/FidelityFX/upscalers/include/ffx_upscale.h"
    "52066379fd629080ed5c9a2821627cd7a3b972e6592565c7f3fa54a95404bac0 Kits/FidelityFX/upscalers/include/ffx_upscale.hpp")

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
                        "Set LARECOMP_FFX_SDK_DIR to a local FidelityFX SDK 2.4.0 checkout to build it offline.")
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
    set(LARECOMP_FFX_SDK_DIR "${_ffx_root}" CACHE PATH
        "AMD FidelityFX SDK (the folder holding Kits/FidelityFX); empty builds without FSR" FORCE)
    message(STATUS "larecomp: FidelityFX SDK 2.4.0 files fetched and checked in ${_ffx_root}")
endif()
