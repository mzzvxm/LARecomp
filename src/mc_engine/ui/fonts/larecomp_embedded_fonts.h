#pragma once
#include <cstddef>

// Byte copies of assets/fonts/*.ttf, the fallback when the loose files are absent.
namespace mcla::ui::fonts {

extern const unsigned char kFontLarecomp[];
extern const size_t kFontLarecompSize;

extern const unsigned char kFontLarecompLogo[];
extern const size_t kFontLarecompLogoSize;

extern const unsigned char kFontLarecompLogoAlt[];
extern const size_t kFontLarecompLogoAltSize;

extern const unsigned char kFontLarecompWide[];
extern const size_t kFontLarecompWideSize;

} // namespace mcla::ui::fonts
