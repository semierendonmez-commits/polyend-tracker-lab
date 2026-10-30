// KONSOL flash diet (MANUAL.md "Building", Makefile "KONSOL flash diet"):
// the device image leaves out OpenTracker's packed UI font image
// (src/core/ui/fonts/fonts.cpp, 31 KB). A lab firmware draws only with the
// FT81x ROM fonts (lab::font); the OpenTracker screens that use the custom
// fonts are not linked into a lab image.
//
// The FT81x driver still uploads "the font image" at init: here it is a
// valid zlib stream of 4 zero bytes, so CMD_INFLATE finishes at once, the
// CRC check that follows fails as designed ("MISMATCH (ROM fonts)" in the
// diag log) and the driver keeps the ROM fonts (fontsOk_ false, no custom
// font handles are set up). The selftest fallback draws with ROM fonts too.
//
// Built only into the device image (the Makefile defines KONSOL_FONT_STUB
// for this one object and filters fonts.cpp out); the simulator and the host
// tests keep the real fonts.cpp.
#if defined(KONSOL_FONT_STUB) && KONSOL_FONT_STUB
#include "ui/fonts/fonts.h"

namespace ot {
namespace fontdata {

#if OT_FONTS_COMPRESSED
alignas(4) const uint8_t kBlobZ[] = {0x78, 0xDA, 0x63, 0x60, 0x60, 0x60, 0x00, 0x00, 0x00, 0x04, 0x00, 0x01};
const uint32_t kBlobZSize = sizeof(kBlobZ);
const uint8_t kAdvance[10][128] = {};
#else
#error "KONSOL_FONT_STUB needs OT_FONTS_COMPRESSED=1 (the device build)"
#endif

}  // namespace fontdata
}  // namespace ot
#endif
