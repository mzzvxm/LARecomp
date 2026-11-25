#pragma once

namespace larecomp::pack_progress {

// Weights approximate the relative times of a full build. Reading 407 files and writing one
// pack are quick. Preparing roughly 435–505 variant targets takes longer than compiling
// 2056 base shaders because the variant passes are largely serial. The 110–116
// composition jobs run in parallel. See the 11-24 integ4 and pbrint shader_build.txt logs.
inline constexpr char kShaderPhasesLine[] =
    "@shaderpack|phases|Reading game files:1:Read game files;Compiling shaders:25:Compile shaders;"
    "Writing the shader pack:1:Write shader pack;Preparing the shader variants:69:Prepare shader variants;"
    "Composing the shader variants:4:Combine shader variants";

// There is one far geometry file and one lamps file. The 439 sectors take most of the time
// (about 1.3–1.6 s versus 0.1 s for far geometry in the 11-21 CITY-RT-M3 builds).
inline constexpr char kCityPhasesLine[] =
    "@citypack|phases|far level of detail:6:Build distant geometry;lamps:1:Build lamps;city sectors:93:Build city sectors";

// CITY-RT-M3 act-hit-first-261121-123528/logs/city_hit_build.txt: phases start at
// 0.1, 1.2, 1.5 and 1.7 s and finish at 6.7 s. Round the intervals to shares of 100.
inline constexpr char kCityHitPhasesLine[] =
    "@citypack|phases|city textures:17:Read city textures;city texture files:4:Write city textures;"
    "far level of detail:3:Build distant geometry;city hit sectors:76:Build city hit sectors";

// CITY-RT-M3 act-hit-deep-261121-123536/logs/city_hit_check.txt: phases start at
// 0.0, 0.6 and 1.3 s and finish at 1.6 s. The stamp-only check has no phase events.
inline constexpr char kCityHitCheckPhasesLine[] =
    "@citypack|phases|checking the city pack:37:Check city pack;checking the city hit file:44:Check city hit file;"
    "checking the city textures:19:Check city textures";

}  // namespace larecomp::pack_progress
