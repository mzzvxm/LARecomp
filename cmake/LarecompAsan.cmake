execute_process(
    COMMAND "${CMAKE_CXX_COMPILER}" --print-resource-dir
    COMMAND_ERROR_IS_FATAL ANY OUTPUT_VARIABLE _asan_resource
    OUTPUT_STRIP_TRAILING_WHITESPACE)
get_filename_component(_asan_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
set(_asan_lib "${_asan_resource}/lib/windows")
set(_asan_import "${_asan_lib}/clang_rt.asan_dynamic-x86_64.lib")
set(_asan_thunk "${_asan_lib}/clang_rt.asan_dynamic_runtime_thunk-x86_64.lib")
set(_asan_dll "${_asan_lib}/clang_rt.asan_dynamic-x86_64.dll")
set(_asan_symbolizer "${_asan_bin}/llvm-symbolizer.exe")
foreach(_asan_file IN ITEMS "${_asan_import}" "${_asan_thunk}" "${_asan_dll}" "${_asan_symbolizer}")
    if(NOT EXISTS "${_asan_file}")
        message(FATAL_ERROR "LARECOMP_ASAN: missing ${_asan_file} from the selected LLVM toolchain")
    endif()
endforeach()

# CMake's clang-cl link rule calls the linker directly. Mirror clang-cl's /MD
# ASan link flags, including the thunk's initialization and SEH hooks.
add_library(larecomp_asan_runtime INTERFACE)
target_link_libraries(larecomp_asan_runtime INTERFACE "${_asan_import}")
target_link_options(larecomp_asan_runtime INTERFACE
    /INCREMENTAL:NO /DEBUG:FULL /INFERASANLIBS:NO /INCLUDE:__asan_seh_interceptor
    "/WHOLEARCHIVE:${_asan_thunk}")

# The generated PPC translation units live on larecomp_recompiled. Apply the
# compile flags only to these named host targets so the guest and SDK stay out.
set(_asan_compile_options
    /fsanitize=address
    /clang:-fsanitize-recover=address
    /clang:-fno-omit-frame-pointer
    /clang:-fno-optimize-sibling-calls
    /Zi)
foreach(_asan_target IN ITEMS larecomp larecomp_gpu plume larecomp_cpuguard
        larecomp_file_replace larecomp_shaderpack larecomp_citypack)
    if(TARGET ${_asan_target})
        target_compile_options(${_asan_target} PRIVATE
            "$<$<COMPILE_LANGUAGE:C,CXX>:${_asan_compile_options}>")
        # The uninstrumented recompiled code links into the same exe: the MSVC STL container annotations must
        # agree across objects (/failifmismatch annotate_string / annotate_vector), so they stay off here.
        target_compile_definitions(${_asan_target} PRIVATE _DISABLE_STRING_ANNOTATION _DISABLE_VECTOR_ANNOTATION)
        target_link_libraries(${_asan_target} PRIVATE larecomp_asan_runtime)
    endif()
endforeach()

target_sources(larecomp PRIVATE src/asan_options.cpp)
add_custom_command(TARGET larecomp POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_asan_dll}" "${_asan_symbolizer}"
            "$<TARGET_FILE_DIR:larecomp>"
    VERBATIM)
