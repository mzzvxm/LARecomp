# The city pack (src/citypack): the city's geometry for ray tracing, built on the player's PC from their own game files (CITY-RT M1).
#
# One static library, larecomp_citypack: the pack format (pack_format.h), the reader of the game's city resources (sector_parse.h), the class of a
# shader (material_class.h), the welder (mesh_builder.h), the builder (builder.h) and the reader the streamer uses (pack_reader.h), and the lamps: the
# static light table with its grid and the game's laws (lamp_table.h), the reader of lights.xlm (lamp_parse.h) and the merge with the running game's lists
# (lamp_merge.h; CITY-RT M3 package G2b). It has no
# dependency on the ReXGlue runtime, Plume or libmspack: the builder is given the LZX function, and it expects, from whoever links it
# (larecomp already has them in LARECOMP_SOURCES; the tool and the test compile them themselves): src/mc_engine/modloader/{rpf3,aes256,texture}.cpp.

add_library(larecomp_citypack STATIC
    builder.cpp
    lamp_merge.cpp
    lamp_parse.cpp
    lamp_table.cpp
    material_class.cpp
    mesh_builder.cpp
    pack_format.cpp
    pack_reader.cpp
    rsc_image.cpp
    sector_parse.cpp
    stamp.cpp
)
target_compile_features(larecomp_citypack PUBLIC cxx_std_23)
# Consumers include "citypack/builder.h" (and the others) from src/.
target_include_directories(larecomp_citypack PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
if(WIN32)
    target_compile_definitions(larecomp_citypack PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(larecomp_citypack PUBLIC Threads::Threads)
endif()
if(COMMAND rexglue_apply_target_settings)
    # The same flags the SDK applies to the rest of larecomp.
    rexglue_apply_target_settings(larecomp_citypack)
endif()

# The pack's checks need no game files and no renderer: the byte helpers, the index codec, the mesh blob, the weld, the shader classes, the reader of the
# game's resources on synthetic resources, and the whole builder on a synthetic RPF3 archive (the same bytes with one thread and four):
#   cmake --build <dir> --target larecomp_citypack_test, then run src/citypack/larecomp_citypack_test [<scratch folder>] (exit 0 = passed).
add_executable(larecomp_citypack_test EXCLUDE_FROM_ALL
    citypack_test.cpp
    "${CMAKE_CURRENT_SOURCE_DIR}/../mc_engine/modloader/aes256.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../mc_engine/modloader/rpf3.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../mc_engine/modloader/texture.cpp")
target_compile_features(larecomp_citypack_test PRIVATE cxx_std_23)
target_link_libraries(larecomp_citypack_test PRIVATE larecomp_citypack)
if(WIN32)
    target_compile_definitions(larecomp_citypack_test PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
endif()

# The lamps' checks (no game files, no renderer, no archive): the section's codec and its damage, the grid's queries against brute force, the game's laws held to the volumetric
# light's (it includes ../gpu/volumetrics_lamps.h: header only), the on hours, the reader of lights.xlm on synthetic resources, the merge on a simulated live read, SelectLamps.
# (The lamps inside a pack built from a synthetic archive are citypack_test.cpp's.)
#   cmake --build <dir> --target larecomp_lamp_table_test, then run src/citypack/larecomp_lamp_table_test (exit 0 = passed).
add_executable(larecomp_lamp_table_test EXCLUDE_FROM_ALL lamp_table_test.cpp)
target_compile_features(larecomp_lamp_table_test PRIVATE cxx_std_23)
target_link_libraries(larecomp_lamp_table_test PRIVATE larecomp_citypack)
target_include_directories(larecomp_lamp_table_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../gpu")
if(WIN32)
    target_compile_definitions(larecomp_lamp_table_test PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
endif()
