# larecomp_citypack_tool: the city pack builder from the command line (dev tool; main.cpp has the commands). Built inside the larecomp project
# (the top-level CMakeLists adds this directory with EXCLUDE_FROM_ALL):
#   cmake --build <dir> --target larecomp_citypack_tool
# It reads the player's own game files; the pack it writes is game-derived and is never shipped or committed.

get_filename_component(_larecomp_repo "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

add_executable(larecomp_citypack_tool
    main.cpp
    "${_larecomp_repo}/src/mc_engine/modloader/aes256.cpp"
    "${_larecomp_repo}/src/mc_engine/modloader/rpf3.cpp"
    "${_larecomp_repo}/src/mc_engine/modloader/texture.cpp"
    "${_larecomp_repo}/src/mc_engine/modloader/xcompress.cpp"
)
# xcompress.cpp alone sees libmspack's headers (the same property the larecomp target sets), and the decoder is larecomp_mspack.
if(LARECOMP_MSPACK_DIR)
    set_source_files_properties("${_larecomp_repo}/src/mc_engine/modloader/xcompress.cpp" PROPERTIES INCLUDE_DIRECTORIES "${LARECOMP_MSPACK_DIR}")
endif()
target_link_libraries(larecomp_citypack_tool PRIVATE larecomp_citypack)
if(TARGET larecomp_mspack)
    target_link_libraries(larecomp_citypack_tool PRIVATE larecomp_mspack)
endif()
if(WIN32)
    target_compile_definitions(larecomp_citypack_tool PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
endif()
