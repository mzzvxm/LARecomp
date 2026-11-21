#pragma once
// ===========================================================================
// LARecomp city pack -- what a geometry is, from its shader's names
// ===========================================================================
// A city sector's shader group names each geometry's shader twice: the group ("CityBumpSpec", "CityRoad", "CityWindowUpper", ...) and the preset
// ("CityBumpSpec_Cloth", "CityNeonAlpha_Metal", ...). Nothing else in the sources says whether a geometry is a wall or a layer laid over one, so
// the class comes from the names: ClassifyShader is the ONE place (kBuilderVersion changes with it). The rules are written from the groups
// the survey saw in the first sectors (25 groups in a 14 sector sample, CITY-RT-SURVEY C7) and are completed by the builder's census
// (`citypack_tool census`: every group and preset of all 439 sectors with its triangles, the class each got): a name the rules do not know is
// a solid caster, which is the safe side for shadows (a layer left in shadows its surface, a wall left out leaks light), and the census lists
// the classes by name so the main session can see what each rule caught.
// ===========================================================================

#include <cstdint>
#include <string_view>

#include "citypack/pack_format.h"

namespace larecomp::citypack {

// The rage string hash (lowercases A-Z, folds '\' to '/'), the game's name hash: the same function as mc::modloader::RageHash, repeated here so the
// pack's own code needs nothing of the mod loader.
uint32_t RageHashText(std::string_view text);

// The class of a geometry whose shader has this group and preset (either may be empty: a source without a name is solid).
MaterialClass ClassifyShader(std::string_view group, std::string_view preset);

// The hash a run keeps for a shader: RageHashText("<group>/<preset>").
uint32_t ShaderHash(std::string_view group, std::string_view preset);

}  // namespace larecomp::citypack
