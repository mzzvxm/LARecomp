# The city pack (src/citypack): the city's geometry for ray tracing, built on the player's PC from their own game files (CITY-RT M1).
#
# One static library, larecomp_citypack: the pack format (pack_format.h), the hit data's format (city_hit_format.h: CITY-RT M2) and its builder (hit_builder.h, with hit_params.h, hit_collect.h,
# hit_material.h), the reader of the game's city resources (sector_parse.h), the class of a
# shader (material_class.h), the welder (mesh_builder.h), the builder (builder.h) and the reader the streamer uses (pack_reader.h). It has no
# dependency on the ReXGlue runtime, Plume or libmspack: the builder is given the LZX function, and it expects, from whoever links it
# (larecomp already has them in LARECOMP_SOURCES; the tool and the test compile them themselves): src/mc_engine/modloader/{rpf3,aes256,texture}.cpp.

add_library(larecomp_citypack STATIC
    builder.cpp
    city_hit_format.cpp
    hit_reader.cpp
    hit_builder.cpp
    hit_collect.cpp
    hit_corner_codec.cpp
    hit_material.cpp
    hit_params.cpp
    city_texture.cpp
    city_zstd.cpp
    guest_hash.cpp
    material_class.cpp
    mesh_builder.cpp
    pack_format.cpp
    pack_reader.cpp
    rsc_image.cpp
    sector_parse.cpp
    stamp.cpp
    texture_residency.cpp
    texture_set.cpp
    xenos_texture.cpp
    xtd_reader.cpp
)
target_compile_features(larecomp_citypack PUBLIC cxx_std_23)
# Consumers include "citypack/builder.h" (and the others) from src/.
target_include_directories(larecomp_citypack PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
# xxhash.h for guest_hash.cpp alone (the texture key, XXH3-64: the number the renderer's texture dumps and GB-F2's rows name a texture by). Header only (XXH_INLINE_ALL): an include folder,
# nothing to link. The same folder src/gpu and src/shaderpack use: LARECOMP_XXHASH_INCLUDE_DIR, else LAR-XenosRecomp's thirdparty/xxHash (they define these cache variables with the same
# text; whichever runs first wins, and the value is the same).
set(LARECOMP_XXHASH_INCLUDE_DIR "" CACHE PATH "Folder with xxhash.h (empty: LAR-XenosRecomp's)")
get_filename_component(_larecomp_citypack_root "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(LARECOMP_XENOSRECOMP_DIR "${_larecomp_citypack_root}/thirdparty/LAR-XenosRecomp"
    CACHE PATH "LAR-XenosRecomp checkout (the shader translator, library target XenosRecompLib)")
set(_larecomp_citypack_xxhash_include "${LARECOMP_XXHASH_INCLUDE_DIR}")
if(NOT _larecomp_citypack_xxhash_include)
    foreach(_candidate "${LARECOMP_XENOSRECOMP_DIR}/thirdparty/xxHash")
        if(EXISTS "${_candidate}/xxhash.h")
            set(_larecomp_citypack_xxhash_include "${_candidate}")
            break()
        endif()
    endforeach()
endif()
if(NOT _larecomp_citypack_xxhash_include)
    message(FATAL_ERROR
        "larecomp_citypack: xxhash.h not found (looked in ${LARECOMP_XENOSRECOMP_DIR}/thirdparty/xxHash: "
        "git submodule update --init --recursive). Set LARECOMP_XXHASH_INCLUDE_DIR.")
endif()
target_include_directories(larecomp_citypack SYSTEM PRIVATE "${_larecomp_citypack_xxhash_include}")

# zstd (BSD-3-Clause, Meta Platforms; https://github.com/facebook/zstd) for city_zstd.cpp: the hit pack's corner streams and the texture file's chains are stored as zstd frames (CITY-HIT-SIZE:
# 419 MB -> about 250 MB on the retail game). The sources are LAR-XenosRecomp's thirdparty/zstd (a nested submodule that is checked out already, as xxHash above: no new submodule); the
# static library has the library's three folders only (common, compress, decompress): no assembly, no threads (the builders run their own workers), no legacy formats, no dictionary
# builder; third-party C, its warnings off. LARECOMP_ZSTD_DIR (the folder with lib/zstd.h) names another checkout.
set(LARECOMP_ZSTD_DIR "" CACHE PATH "Folder with zstd's lib/ (empty: LAR-XenosRecomp's thirdparty/zstd)")
set(_larecomp_citypack_zstd_root "${LARECOMP_ZSTD_DIR}")
if(NOT _larecomp_citypack_zstd_root)
    set(_larecomp_citypack_zstd_root "${LARECOMP_XENOSRECOMP_DIR}/thirdparty/zstd")
endif()
if(NOT EXISTS "${_larecomp_citypack_zstd_root}/lib/zstd.h")
    message(FATAL_ERROR
        "larecomp_citypack: zstd not found (looked for ${_larecomp_citypack_zstd_root}/lib/zstd.h: "
        "git submodule update --init --recursive). Set LARECOMP_ZSTD_DIR.")
endif()
if(NOT TARGET larecomp_zstd)
    file(GLOB _larecomp_zstd_sources CONFIGURE_DEPENDS
        "${_larecomp_citypack_zstd_root}/lib/common/*.c"
        "${_larecomp_citypack_zstd_root}/lib/compress/*.c"
        "${_larecomp_citypack_zstd_root}/lib/decompress/*.c")
    add_library(larecomp_zstd STATIC ${_larecomp_zstd_sources})
    target_include_directories(larecomp_zstd PUBLIC "${_larecomp_citypack_zstd_root}/lib")
    target_compile_definitions(larecomp_zstd PRIVATE ZSTD_DISABLE_ASM=1 ZSTD_LEGACY_SUPPORT=0)
    target_compile_options(larecomp_zstd PRIVATE -w)
    set_target_properties(larecomp_zstd PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()
target_link_libraries(larecomp_citypack PRIVATE larecomp_zstd)
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

# The city hit data's interface (city_hit_format.h, CITY-RT M2 package H0): the records, the UV transform, the corner streams, the hit blob, the file's headers, the pool block, and the
# HLSL decode (src/gpu/hlsl/city_hit_common.hlsli) compiled as C++ and checked against the twin and against the data that went in. It reads two headers of src/gpu (the G-buffer's material word and
# the instance id) so that the interface cannot drift from them:
#   cmake --build <dir> --target larecomp_city_hit_format_test, then run src/citypack/larecomp_city_hit_format_test [<hlsl/city_hit_common.hlsli>] (exit 0 = passed).
add_executable(larecomp_city_hit_format_test EXCLUDE_FROM_ALL city_hit_format_test.cpp)
target_compile_features(larecomp_city_hit_format_test PRIVATE cxx_std_23)
target_link_libraries(larecomp_city_hit_format_test PRIVATE larecomp_citypack)
target_include_directories(larecomp_city_hit_format_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../gpu")
if(WIN32)
    target_compile_definitions(larecomp_city_hit_format_test PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
endif()

# The runtime's reader of the city hit file (hit_reader.h, CITY-RT M2 package H3): a small hit file written with city_hit_format.h's writers, opened beside a made-up base pack identity;
# what it reads back and what it refuses (another pack, another name, a cut file, a damaged table, a damaged blob):
#   cmake --build <dir> --target larecomp_hit_reader_test, then run src/citypack/larecomp_hit_reader_test [<scratch folder>] (exit 0 = passed).
add_executable(larecomp_hit_reader_test EXCLUDE_FROM_ALL hit_reader_test.cpp)
target_compile_features(larecomp_hit_reader_test PRIVATE cxx_std_23)
target_link_libraries(larecomp_hit_reader_test PRIVATE larecomp_citypack)
if(WIN32)
    target_compile_definitions(larecomp_hit_reader_test PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
endif()

# The hit data's builder (hit_builder.h, CITY-RT M2 package H1) on a SYNTHETIC archive: the shader instance parameters, the vertex layouts, the corners and the runs, the material table through the G-buffer's rules,
# the file's bytes (the same with one thread and four), the reader and the validator, the stamp. It needs the same archive writer as the pack's test:
#   cmake --build <dir> --target larecomp_city_hit_builder_test, then run src/citypack/larecomp_city_hit_builder_test [<scratch folder>] (exit 0 = passed).
add_executable(larecomp_city_hit_builder_test EXCLUDE_FROM_ALL
    city_hit_builder_test.cpp
    "${CMAKE_CURRENT_SOURCE_DIR}/../mc_engine/modloader/aes256.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../mc_engine/modloader/rpf3.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../mc_engine/modloader/texture.cpp")
target_compile_features(larecomp_city_hit_builder_test PRIVATE cxx_std_23)
target_link_libraries(larecomp_city_hit_builder_test PRIVATE larecomp_citypack)
if(WIN32)
    target_compile_definitions(larecomp_city_hit_builder_test PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
endif()

# The city texture set (src/citypack/texture_set.h, CITY-RT M2 package H2): the Xenos layout and untiling (checked against the mod loader's own tiler, which writes the synthetic textures), the
# dictionaries' reader, the BC decoders and means, the set builder on a synthetic archive (one thread and four give the same file), the texture file and the residency rules. With a game folder as
# the second argument it also builds the retail archive's set and checks the golden counts and the sizes at each class's cap (that part needs the LZX decoder: libmspack, as the tool):
#   cmake --build <dir> --target larecomp_texture_set_test, then run src/citypack/larecomp_texture_set_test [<scratch folder> [<game folder>]] (exit 0 = passed).
add_executable(larecomp_texture_set_test EXCLUDE_FROM_ALL
    texture_set_test.cpp
    "${CMAKE_CURRENT_SOURCE_DIR}/../mc_engine/modloader/aes256.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../mc_engine/modloader/rpf3.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../mc_engine/modloader/texture.cpp")
target_compile_features(larecomp_texture_set_test PRIVATE cxx_std_23)
target_link_libraries(larecomp_texture_set_test PRIVATE larecomp_citypack)
if(LARECOMP_MSPACK_DIR AND TARGET larecomp_mspack)
    target_sources(larecomp_texture_set_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../mc_engine/modloader/xcompress.cpp")
    set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/../mc_engine/modloader/xcompress.cpp" PROPERTIES INCLUDE_DIRECTORIES "${LARECOMP_MSPACK_DIR}")
    target_link_libraries(larecomp_texture_set_test PRIVATE larecomp_mspack)
    target_compile_definitions(larecomp_texture_set_test PRIVATE LARECOMP_TEXTURE_SET_TEST_REAL_LZX=1)
endif()
if(WIN32)
    target_compile_definitions(larecomp_texture_set_test PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
endif()
