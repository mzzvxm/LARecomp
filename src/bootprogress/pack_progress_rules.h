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

}  // namespace larecomp::pack_progress
