#pragma once
//
// framedump -- get a video board's frame OUT of the simulator, for a person to look at
// and for a test to diff (DESIGN.md 7.4).
//
// A board paints an Indexed8 Surface and hands the host a palette -- or paints an Rgb32
// Surface of finished colors; that is all a Display ever sees, and it is all a test can read
// back through a NullDisplay (display_null.h). But a Surface is a pile of palette indices, and "is pixel (32,0)
// index 1" is a poor way to say "the picture is right". These are the two forms a
// frame takes once it leaves the seam:
//
//   THE TEXT GRID (frameText)  -- one character per pixel, from a legend. This is what
//        an assertion is written against: it is diffable, it prints legibly in a
//        FAIL line, and for a small frame the EXPECTED picture is a raw string
//        literal sitting in the test where a reader can see it.
//
//   THE IMAGE (framePpm)       -- the frame resolved through its palette into 24-bit
//        RGB and wrapped as a binary PPM (P6). Any image viewer opens it and it needs
//        no library, no compression and no endianness. This is what you OPEN when the
//        grid disagrees and you need to see what the board actually drew.
//
// Plus frameCrc, a whole frame as one number over the RESOLVED RGB (so a palette-only
// change moves it) -- the one-line escape for a frame too big to keep either way.
//
// Resolving index -> Color here is the same operation the SDL back end performs when it
// uploads a frame, and for a board with a RAMDAC it is literally the DAC in software:
// what these functions hand back is what the wire carries.
//
// NOTHING ON THE SEAM CHANGES for this. Surface::pixels() is public and NullDisplay
// keeps each owner's surface and palette; these are pure functions over the two.

#include "host/display.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace altair {

// The frame as packed 24-bit RGB, row-major, top-left origin, 3 bytes per pixel. An index
// past the palette's end resolves to black -- the same answer the SDL back end gives for an
// entry a board never set. An Rgb32 frame is already colors: `pal` is not used.
std::vector<uint8_t> frameRgb(const Surface& s, std::span<const Color> pal);

// The frame as a complete binary PPM (P6) file image: "P6\n<w> <h>\n255\n" then frameRgb().
std::vector<uint8_t> framePpm(const Surface& s, std::span<const Color> pal);

// framePpm() written to `path`. False, with `err` set, if the file cannot be written.
bool writePpm(const std::string& path, const Surface& s, std::span<const Color> pal,
              std::string& err);

// How frameText() draws a pixel.
//
//   legend  -- index i prints as legend[i]; an index past the legend's end prints '?', so a
//              stray value is visible rather than aliased onto a real color. The default
//              names the sixteen entries a Dazzler or a 4-bpp board uses; a test with a
//              256-entry palette picks a legend that names the entries it loaded. An
//              Rgb32 frame has no indices: black prints legend[0] and any other color
//              '#', so the grid shows where the board drew, and frameCrc() what.
//   xStep,  -- sample every Nth column / row. A 64x64 frame reads 1:1; a 512x208 VDM-1
//   yStep      frame does not, and stepping by the glyph cell makes each character one
//              character cell. Must be >= 1.
struct TextGridOpts {
    std::string legend = ".123456789ABCDEF";
    int         xStep  = 1;
    int         yStep  = 1;
};

// The frame as text: ceil(h/yStep) lines of ceil(w/xStep) characters, each line ending in
// '\n'. Row 0 is the top of the picture.
std::string frameText(const Surface& s, const TextGridOpts& o = {});

// CRC-32 of frameRgb(). Two frames that look the same have the same CRC; a palette change
// with no repaint changes it, which is the point of hashing the resolved bytes.
uint32_t frameCrc(const Surface& s, std::span<const Color> pal);

} // namespace altair
