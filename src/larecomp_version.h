#pragma once

// The release, in one place for the code (the save picker's corner) and for
// larecomp.rc's version resource, which the resource compiler reads with
// RC_INVOKED defined: plain #defines above that guard only.
#define LARECOMP_PRODUCT_NAME "LARecomp"
#define LARECOMP_VERSION_LABEL "PATCH 10 - PREVIEW 2"

#ifndef RC_INVOKED
namespace larecomp {

// Shown in the save picker's bottom-right corner.
inline constexpr char kVersionLabel[] = LARECOMP_VERSION_LABEL;

}  // namespace larecomp
#endif
