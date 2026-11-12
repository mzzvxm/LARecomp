#pragma once

// The CPU level this build was compiled for (CMake option LARECOMP_CPU_LEVEL) and what the CPU it
// runs on reports. cpu_level.cpp is compiled without the level's -march flag (it has to run on any
// x86-64 CPU), so these can be called from anywhere.

#include <string>

namespace larecomp::cpu {

// "baseline" (what the SDK builds for: x86-64 with SSE4.1) or "x86-64-v3" (AVX2, FMA, BMI1/2,
// MOVBE, LZCNT, F16C).
const char* BuiltLevel();

// The features the CPU reports, as one line of text for the log: "<vendor>; avx2 fma ...; ...".
// "AVX" is only listed when the OS has enabled the AVX register state too.
std::string DescribeFeatures();

}  // namespace larecomp::cpu
