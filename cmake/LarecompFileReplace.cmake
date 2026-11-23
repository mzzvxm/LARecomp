# The file replacing helper (src/os/file_replace.h) as a static library, larecomp_file_replace: writing a cache, pack or list file that parallel
# processes share (a unique temporary name, one step over the old file, tried again while it is busy, the same bytes already there are a success).
# Every library or program that writes such a file links it: larecomp (src/gpu, src/native_gfx, src/mc_engine), larecomp_citypack and
# larecomp_shaderpack (which pass it on to their tools and tests). Included from more than one CMakeLists.txt (the top level one, and src/shaderpack's
# for tools/shaderpack_tool built on its own): the second include does nothing.
if(NOT TARGET larecomp_file_replace)
    get_filename_component(_larecomp_file_replace_src "${CMAKE_CURRENT_LIST_DIR}/../src" ABSOLUTE)
    add_library(larecomp_file_replace STATIC "${_larecomp_file_replace_src}/os/file_replace.cpp")
    target_compile_features(larecomp_file_replace PUBLIC cxx_std_23)
    # Consumers include "os/file_replace.h" from src/.
    target_include_directories(larecomp_file_replace PUBLIC "${_larecomp_file_replace_src}")
    if(WIN32)
        target_compile_definitions(larecomp_file_replace PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
    else()
        find_package(Threads REQUIRED)
        target_link_libraries(larecomp_file_replace PUBLIC Threads::Threads)
    endif()
    if(COMMAND rexglue_apply_target_settings)
        # The same flags the SDK applies to the rest of larecomp.
        rexglue_apply_target_settings(larecomp_file_replace)
    endif()
    unset(_larecomp_file_replace_src)
endif()
