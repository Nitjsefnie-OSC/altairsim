#include "test.h"
#include "framecheck.h"

#include "core/crc32.h"
#include "host/display.h"
#include "host/display_null.h"
#include "host/framedump.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <system_error>
#include <fstream>
#include <string>
#include <vector>

using namespace altair;

namespace {

// Read a whole file and CLOSE IT before returning. The stream must not outlive the read:
// the sections below delete the file next, and on Windows deleting a file that a stream
// still holds open is a sharing violation -- std::filesystem::remove() throws, and an
// uncaught exception aborts the whole test binary (CI, 2026-09-18: exit 0xC0000409).
std::vector<uint8_t> slurp(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
}

} // namespace

// The frame-capture helpers (host/framedump.h) proved on hand-built Surfaces -- no board,
// no window. A board's test then uses CHECK_FRAME and trusts these.

void test_framedump() {
    SECTION("framedump -- an Rgb32 Surface is colors: 4 bytes a pixel, no palette, frameText by black");
    {
        Surface s(3, 2, PixelFormat::Rgb32);
        CHECK(s.pitch() == 12 && s.pixels().size() == 24, "four bytes per pixel, pitch in bytes");
        s.clear(7);
        CHECK(s.pixels()[0] == 0 && s.pixels()[2] == 0 && s.pixels()[3] == 255,
              "clear() is opaque black whatever the index");
        s.put(0, 0, 9);
        CHECK(s.pixels()[0] == 0, "put() takes an index, which an Rgb32 Surface has none of: a no-op");
        auto px = s.pixels();
        px[4] = 0x11; px[5] = 0x22; px[6] = 0x33;            // pixel (1,0)
        px[12 + 8] = 0xFF;                                   // pixel (2,1): red
        std::vector<Color> pal = {{9, 9, 9, 255}, {8, 8, 8, 255}};
        std::vector<uint8_t> rgb = frameRgb(s, pal);
        CHECK(rgb.size() == 18, "three bytes per pixel out, the alpha dropped");
        CHECK(rgb[0] == 0 && rgb[3] == 0x11 && rgb[4] == 0x22 && rgb[5] == 0x33 && rgb[15] == 0xFF,
              "the colors as painted -- the palette is not consulted");
        CHECK(frameText(s) == ".#.\n..#\n", "frameText: black is legend[0], any other color '#'");
        const uint32_t before = frameCrc(s, {});
        CHECK(frameCrc(s, pal) == before, "frameCrc does not move with a palette it does not use");
        px[4] = 0x12;
        CHECK(frameCrc(s, {}) != before, "and does move with a color");
    }

    SECTION("framedump -- frameRgb resolves indices through the palette; strays go black");
    {
        Surface s(2, 2, PixelFormat::Indexed8);
        s.put(0, 0, 0);
        s.put(1, 0, 1);
        s.put(0, 1, 2);
        s.put(1, 1, 7);  // past the palette's end
        std::vector<Color> pal = {{10, 20, 30, 255}, {255, 0, 0, 255}, {0, 255, 0, 255}};

        std::vector<uint8_t> rgb = frameRgb(s, pal);
        CHECK(rgb.size() == 12, "three bytes per pixel, row-major");
        CHECK(rgb[0] == 10 && rgb[1] == 20 && rgb[2] == 30, "index 0 -> palette[0]");
        CHECK(rgb[3] == 255 && rgb[4] == 0 && rgb[5] == 0, "index 1 -> palette[1], red");
        CHECK(rgb[6] == 0 && rgb[7] == 255 && rgb[8] == 0, "row 1 starts at byte 6: green");
        CHECK(rgb[9] == 0 && rgb[10] == 0 && rgb[11] == 0, "an index past the palette is black");
    }

    SECTION("framedump -- framePpm is a P6 header and the raster, and writePpm lands it");
    {
        Surface s(3, 2, PixelFormat::Indexed8);
        s.clear(1);
        std::vector<Color> pal = {{0, 0, 0, 255}, {0x11, 0x22, 0x33, 255}};

        std::vector<uint8_t> ppm = framePpm(s, pal);
        const std::string    hdr = "P6\n3 2\n255\n";
        CHECK(ppm.size() == hdr.size() + 18, "header plus 3x2x3 raster bytes");
        CHECK(std::string(ppm.begin(), ppm.begin() + (long)hdr.size()) == hdr,
              "the header names the width, height and maxval");
        CHECK(ppm[hdr.size()] == 0x11 && ppm[hdr.size() + 1] == 0x22 && ppm[hdr.size() + 2] == 0x33,
              "the raster follows immediately, RGB");

        std::string path =
            (std::filesystem::temp_directory_path() / "altair_framedump_test.ppm").string();
        std::string err;
        CHECK(writePpm(path, s, pal, err), "writePpm succeeds in the temp directory");
        std::vector<uint8_t> back = slurp(path);
        CHECK(back == ppm, "and the file is byte-for-byte framePpm()");
        std::error_code ec;
        std::filesystem::remove(path, ec);

        CHECK(!writePpm("/nonexistent-dir/x/y.ppm", s, pal, err) && !err.empty(),
              "an unwritable path fails with a message");
    }

    SECTION("framedump -- frameText maps the legend, marks strays, and steps");
    {
        Surface s(4, 2, PixelFormat::Indexed8);
        s.put(0, 0, 0);
        s.put(1, 0, 1);
        s.put(2, 0, 15);
        s.put(3, 0, 16);  // past the default 16-entry legend
        s.put(0, 1, 2);
        s.put(1, 1, 2);
        s.put(2, 1, 2);
        s.put(3, 1, 2);
        CHECK(frameText(s) == ".1F?\n2222\n", "default legend: '.' for 0, hex digits, '?' past it");

        TextGridOpts o;
        o.legend = " #";
        s.put(2, 0, 1);
        s.put(3, 0, 1);
        CHECK(frameText(s, o) == " ###\n????\n", "a custom legend, and index 2 is off its end");

        // Stepping: an 8x4 frame sampled every 2 columns and rows is 4x2 characters, and
        // it is the pixel AT (2i, 2j) that is sampled -- not an average.
        Surface big(8, 4, PixelFormat::Indexed8);
        big.put(0, 0, 1);
        big.put(1, 0, 2);   // skipped by xStep=2
        big.put(6, 2, 3);
        big.put(7, 3, 4);   // skipped: odd row and column
        TextGridOpts st;
        st.xStep = 2;
        st.yStep = 2;
        CHECK(frameText(big, st) == "1...\n...3\n", "xStep/yStep sample every Nth pixel");

        // A frame whose size is not a multiple of the step still shows its last column/row.
        Surface odd(5, 3, PixelFormat::Indexed8);
        odd.put(4, 2, 1);
        CHECK(frameText(odd, st) == "...\n..1\n", "ceil() columns and rows -- the edge pixel appears");
    }

    SECTION("framedump -- frameCrc hashes the RESOLVED frame, so a palette change moves it");
    {
        Surface s(4, 4, PixelFormat::Indexed8);
        s.put(1, 1, 1);
        std::vector<Color> red  = {{0, 0, 0, 255}, {255, 0, 0, 255}};
        std::vector<Color> blue = {{0, 0, 0, 255}, {0, 0, 255, 255}};

        uint32_t a = frameCrc(s, red);
        CHECK(a == frameCrc(s, red), "the same picture hashes the same");
        CHECK(a == crc32(frameRgb(s, red)), "and it is crc32 over frameRgb()");
        CHECK(a != frameCrc(s, blue), "the same indices under a different palette differ");
        s.put(1, 1, 0);
        CHECK(a != frameCrc(s, red), "and a repainted pixel differs");
    }

    SECTION("framedump -- CHECK_FRAME reads a board's frame off the NullDisplay per owner");
    {
        NullDisplay disp;
        char        tagA = 0, tagB = 0;
        Display::Owner a = &tagA, b = &tagB;
        std::vector<Color> pal = {{0, 0, 0, 255}, {255, 255, 255, 255}};

        Surface* sa = disp.acquire(a, "a", 4, 2, PixelFormat::Indexed8, 0);
        Surface* sb = disp.acquire(b, "b", 4, 2, PixelFormat::Indexed8, 0);
        disp.setPalette(a, pal);
        disp.setPalette(b, pal);
        sa->clear(0);
        sa->put(0, 0, 1);
        sa->put(3, 1, 1);
        sb->clear(1);
        disp.present(a, sa);
        disp.present(b, sb);

        CHECK_FRAME(disp, a, R"(
1...
...1
)", "owner A's frame, expected as a raw string literal");
        CHECK_FRAME(disp, b, "1111\n1111\n", "owner B's frame, expected inline");
        CHECK_FRAME(disp, a, "1...\n...1", "a missing final newline is supplied");

        TextGridOpts o;
        o.legend = " *";
        CHECK_FRAME_OPTS(disp, a, "*   \n   *\n", o, "and options carry a legend");

        // THE FAILURE PATH, called directly so it does not count as a failure here: a
        // mismatch prints its diagnosis (the lines below are expected) and writes the
        // actual frame as a .ppm we can find.
        std::printf("  (the next 'frame:' lines are the failure path being exercised, not a failure)\n");
        std::string ppm =
            (std::filesystem::temp_directory_path() / "altair_frame_framedump_negative.ppm").string();
        std::error_code ec;
        std::filesystem::remove(ppm, ec);
        CHECK(!checkFrame(disp, a, "1...\n....\n", TextGridOpts{}, "framedump negative"),
              "a wrong expectation is reported false");
        CHECK(std::filesystem::exists(ppm, ec), "and the actual frame was written as a .ppm");
        CHECK(slurp(ppm) == framePpm(*disp.surface(a), disp.palette(a)),
              "...and it is the frame the board drew, through its own palette");
        std::filesystem::remove(ppm, ec);

        char unknown = 0;
        CHECK(!checkFrame(disp, &unknown, "", TextGridOpts{}, "framedump no owner"),
              "an owner that never drew fails rather than matching an empty grid");
    }

    SECTION("framedump -- CHECK_FRAME_PIXELS compares every pixel against an oracle");
    {
        NullDisplay disp;
        char        tag = 0;
        Display::Owner a = &tag;
        std::vector<Color> pal = {{0, 0, 0, 255}, {255, 255, 255, 255}};
        Surface* s = disp.acquire(a, "a", 64, 48, PixelFormat::Indexed8, 0);
        disp.setPalette(a, pal);
        s->clear(0);
        for (int x = 0; x < 64; ++x) s->put(x, 10, 1);      // one full row
        s->put(63, 47, 1);                                   // and the far corner
        disp.present(a, s);

        auto oracle = [](int x, int y) -> uint8_t { return (y == 10 || (x == 63 && y == 47)) ? 1 : 0; };
        CHECK_FRAME_PIXELS(disp, a, oracle, "3072 pixels, all as the oracle says");

        std::printf("  (the next 'frame:' lines are the failure path being exercised, not a failure)\n");
        s->put(5, 20, 1);                                    // one stray pixel the oracle does not know
        CHECK(!checkFramePixels(disp, a, oracle, "framedump pixels negative"),
              "a single wrong pixel anywhere in the frame fails the check -- nothing is sampled");
        char unknown = 0;
        CHECK(!checkFramePixels(disp, &unknown, oracle, "framedump pixels no owner"),
              "an owner that never drew fails");
    }

    SECTION("framedump -- a golden file is compared, and only rewritten on request");
    {
        NullDisplay disp;
        char        tag = 0;
        Display::Owner a = &tag;
        std::vector<Color> pal = {{0, 0, 0, 255}, {255, 255, 255, 255}};
        Surface* s = disp.acquire(a, "a", 3, 1, PixelFormat::Indexed8, 0);
        disp.setPalette(a, pal);
        s->put(1, 0, 1);
        disp.present(a, s);

        // The committed golden for this exact frame.
        CHECK_FRAME_GOLDEN(disp, a, "framedump-3x1", TextGridOpts{}, "tests/golden/framedump-3x1.txt matches");

        // Not under ALTAIR_TEST_WRITE_GOLDEN=1 -- that mode writes every golden it is
        // asked about, which is the point of it, and would leave this stray one behind.
        if (const char* w = std::getenv("ALTAIR_TEST_WRITE_GOLDEN"); !(w && *w && *w != '0')) {
            std::printf("  (the next 'golden missing' line is the failure path being exercised)\n");
            CHECK(!checkFrameGolden(disp, a, "framedump-does-not-exist", TextGridOpts{},
                                    "missing golden"),
                  "a golden that does not exist is a failure, not a silent pass");
        }
    }
}
