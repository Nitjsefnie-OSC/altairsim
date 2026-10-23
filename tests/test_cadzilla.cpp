#include "test.h"
#include "framecheck.h"

#include "boards/cadzilla.h"
#include "config/toml.h"
#include "core/machine.h"
#include "core/statefile.h"
#include "host/display_null.h"
#include "host/framedump.h"

#include <cstdint>
#include <string>

using namespace altair;

namespace {

// A machine with a cadzilla and a NullDisplay wired to it -- the SAME injection main()
// does (CadzillaBoard::setDisplay), one backend down. The board builds the monitor's frame
// in memory and hands the host colors (an Rgb32 Surface, through the Bt453). A test reads
// the colors from `disp`, and reads what the shift registers put on the RAMDAC's input --
// the pixel VALUES, P7..0 -- from busView(), with CHECK_FRAME.
struct Rig {
    Machine        m;
    NullDisplay    disp;
    NullDisplay    bus;    // busView()'s: the RAMDAC's P7..0 input as an Indexed8 frame
    CadzillaBoard* cad = nullptr;

    static constexpr uint8_t kAcrtc     = 0x70;   // BASE+0: ACRTC RS=0 (address/status)
    static constexpr uint8_t kMode      = 0x71;   // BASE+1: MODE register, write-only
    static constexpr uint8_t kAcrtcData = 0x72;   // BASE+2: ACRTC RS=1 (data/FIFO)
    static constexpr uint8_t kDac       = 0x74;   // BASE+4..+7: Bt453

    Rig() {
        std::string err;
        m.bus.setVerify(true);
        cad = dynamic_cast<CadzillaBoard*>(m.add("cadzilla", "cad0", err));
        CadzillaBoard::setDisplay(&disp);
        m.power();
    }
    // The Display is injected statically; a test that builds a second Rig must point the
    // first back at its own before asking it to draw.
    void adopt() { CadzillaBoard::setDisplay(&disp); }

    // ---- the ACRTC through its two ports, 8-bit MPU mode ----
    void    ar(uint8_t a) { m.bus.ioWrite(kAcrtc, a); }
    uint8_t sr() { return m.bus.ioRead(kAcrtc); }
    void    data(uint8_t v) { m.bus.ioWrite(kAcrtcData, v); }
    uint8_t data() { return m.bus.ioRead(kAcrtcData); }
    void reg(uint8_t even, uint16_t v) {
        ar(even);
        data((uint8_t)(v >> 8));
        if (even < 0x80) ar((uint8_t)(even | 1));
        data((uint8_t)v);
    }
    void word(uint16_t w) {
        ar(0);
        data((uint8_t)(w >> 8));
        data((uint8_t)w);
    }
    void cmd(uint16_t op, std::initializer_list<uint16_t> params = {}) {
        word(op);
        for (uint16_t p : params) word(p);
    }

    // ---- the Bt453 through its four ports: address, then R, G, B ----
    void lut(uint8_t index, uint8_t r, uint8_t g, uint8_t b) {
        m.bus.ioWrite(kDac + 0, index);
        m.bus.ioWrite(kDac + 1, r);
        m.bus.ioWrite(kDac + 1, g);
        m.bus.ioWrite(kDac + 1, b);
    }

    // ---- the MODE register: one write-only byte at BASE+1 ----
    void mode_reg(uint8_t v) { m.bus.ioWrite(kMode, v); }

    // Program the ACRTC for the monitor mode the board is strapped to, the way the board's
    // documentation says to: 8 bpp, GAI +8, the mode's timing in memory cycles (doubled in
    // interleaved mode), the picture starting exactly where the porch ends, one raster per
    // MW words, SAR1 = 0. The origin goes on the BOTTOM raster so +Y is up on the screen.
    void programMode(bool interleaved = false) {
        const auto& md  = cad->currentMode();
        const int   acm = interleaved ? 2 : 1;
        const int   mw  = md.width / 2;                       // words per raster at 8 bpp
        reg(0x02, 0x0300);                                    // CCR: GBM = 011 (8 bpp), ABT clear
        reg(0x82, (uint16_t)(((md.hc() * acm - 1) << 8) | (md.hsw * acm)));           // HC, HSW
        reg(0x84, (uint16_t)(((md.hbp * acm - 1) << 8) | (md.width / 16 * acm - 1)));  // HDS, HDW
        reg(0x86, (uint16_t)md.vc());                                                  // VC
        reg(0x88, (uint16_t)(((md.vbp - 1) << 8) | md.vsw));                           // VDS, VSW
        reg(0x8A, (uint16_t)md.height);                                                // SP1
        reg(0xCA, (uint16_t)mw);                                                       // MWR1
        reg(0xCC, 0x0000);
        reg(0xCE, 0x0000);                                                             // SAR1 = 0
        reg(0x04, (uint16_t)(0x4030 | (interleaved ? 0x08 : 0)));   // OMR: STR, GAI = 011, ACM
        reg(0x06, 0x4000);                                          // DCR: SE1
        // The board's OWN glue reads MODE.AMODE for single/interleaved, not the ACRTC's OMR
        // ACM bit -- both must be set in agreement, or `wiring()` says so.
        mode_reg(interleaved ? 0x04 : 0x00);
        const uint32_t org = (uint32_t)(md.height - 1) * (uint32_t)mw;
        cmd(0x0400, {(uint16_t)(0x4000 | ((org >> 12) & 0xFF)), (uint16_t)((org & 0xFFF) << 4)});
        cmd(0x1800, {2, 0xFFFF});                             // pattern row 0 all ones
        cmd(0x0806, {0x0000});                                // PRC: PSX = PSY = 0
        cmd(0x0807, {0x00F0});                                // PEX = 15, PEY = 0
        cmd(0x0805, {0x0000});                                // PPX = PPY = 0
    }
    void color(uint8_t idx) { cmd(0x0801, {(uint16_t)((idx << 8) | idx)}); }   // CL1, both bytes

    // P7..0 at frame pixel (x, y) in the last frame; a BLANK pixel reads 0.
    uint8_t px(int x, int y) const { return (uint8_t)cad->busPixel(x, y); }

    // The last frame's P7..0 as an Indexed8 frame under the Bt453's palette RAM, for the
    // CHECK_FRAME family: the frame the board drew, as pixel values. BLANK reads as 0.
    NullDisplay& busView() {
        const auto& md = cad->currentMode();
        Surface*    s  = bus.acquire(cad, "bus", md.width, md.height, PixelFormat::Indexed8, 0);
        for (int y = 0; y < md.height; ++y)
            for (int x = 0; x < md.width; ++x) s->put(x, y, px(x, y));
        const auto pal = cad->dac().palette();
        bus.setPalette(cad, pal);
        bus.present(cad, s);
        return bus;
    }

    // ---- the overlay ----
    // ORG at word `addr` of screen `dn`, dot 0.
    void org(int dn, uint32_t addr) {
        cmd(0x0400, {(uint16_t)((dn << 14) | ((addr >> 12) & 0xFF)), (uint16_t)((addr & 0xFFF) << 4)});
    }
    // Draw the overlay: the Upper screen (0) made a CHR screen one bit per frame pixel wide
    // (MW = width / 16 words), 1 bpp, its origin on the overlay word of the frame's bottom-left
    // pixel -- so overlay (X, Y) is frame (X, Y), the same coordinates programMode() gives
    // the frame memory. Never display-enabled.
    void overlayScreen() {
        const auto& md = cad->currentMode();
        reg(0xC2, (uint16_t)(0x8000 | (md.width / 16)));     // MWR0: CHR, MW
        reg(0x02, 0x0000);                                    // CCR: GBM = 000 (1 bpp)
        org(0, (uint32_t)(md.height - 1) * (uint32_t)(md.width / 16));
        cmd(0x0801, {0xFFFF});                                // CL1: the bit set
    }
    // Back to drawing the frame memory: 8 bpp, ORG on screen 1 as programMode() left it.
    void frameScreen() {
        const auto& md = cad->currentMode();
        reg(0x02, 0x0300);
        org(1, (uint32_t)(md.height - 1) * (uint32_t)(md.width / 2));
    }
    void dot(int x, int y) {
        cmd(0x8000, {(uint16_t)x, (uint16_t)y});
        cmd(0xCC00);
    }
    // Bt453 overlay color `n` (1-3): its address through C1C0 = 00, R,G,B at C1C0 = 11.
    void ovlColor(uint8_t n, uint8_t r, uint8_t g, uint8_t b) {
        m.bus.ioWrite(kDac + 0, n);
        m.bus.ioWrite(kDac + 3, r);
        m.bus.ioWrite(kDac + 3, g);
        m.bus.ioWrite(kDac + 3, b);
    }
    // The color the host was handed at frame pixel (x, y).
    Color rgb(int x, int y) const {
        const Surface* s = disp.surface(cad);
        const size_t   i = (size_t)y * (size_t)s->pitch() + (size_t)x * 4;
        return Color{s->pixels()[i], s->pixels()[i + 1], s->pixels()[i + 2], s->pixels()[i + 3]};
    }

    // Is the last frame black wherever it is sampled (every 97th pixel)? Colors, not values.
    bool frameBlack(int w, int h) const {
        const Surface* s = disp.surface(cad);
        if (!s || s->width() != w || s->height() != h) return false;
        const std::vector<uint8_t> rgb = frameRgb(*s, {});
        for (size_t i = 0; i < rgb.size(); i += 97)
            if (rgb[i] != 0) return false;
        return true;
    }
};

} // namespace

void test_cadzilla() {
    SECTION("cadzilla -- one 8-port block: ACRTC RS=0/RS=1 split by MODE, the Bt453, the BASE+3 gap");
    {
        Rig g;
        BusCycle c;
        for (int dir = 0; dir < 2; ++dir) {
            c.type = dir ? Cycle::IoRead : Cycle::IoWrite;
            c.addr = 0x70;
            CHECK(g.cad->decodes(c), "ACRTC RS=0 at BASE+0, both directions (AR out, SR in)");
            c.addr = 0x72;
            CHECK(g.cad->decodes(c), "ACRTC RS=1 at BASE+2, both directions -- NOT adjacent to BASE+0");
            c.addr = 0x6F;
            CHECK(!g.cad->decodes(c), "not the port below BASE");
            c.addr = 0x73;
            CHECK(!g.cad->decodes(c), "BASE+3 is not decoded at all -- nobody answers it");
            for (uint16_t p = 0x74; p <= 0x77; ++p) {
                c.addr = p;
                CHECK(g.cad->decodes(c), "Bt453: all four of BASE+4..+7, both directions");
            }
            c.addr = 0x78;
            CHECK(!g.cad->decodes(c), "and not BASE+8 -- the block is eight ports");
        }
        // BASE+1, MODE, is write-only -- like the Dazzler's format port floats on a read.
        c.addr = 0x71;
        c.type = Cycle::IoWrite;
        CHECK(g.cad->decodes(c), "BASE+1 (MODE) decodes OUT");
        c.type = Cycle::IoRead;
        CHECK(!g.cad->decodes(c), "...but not IN");

        c.type = Cycle::MemRead;
        c.addr = 0x0000;
        CHECK(!g.cad->decodes(c), "no memory: the frame memory is the ACRTC's own");
        c.addr = 0xC000;
        CHECK(!g.cad->decodes(c), "nowhere");
    }

    SECTION("cadzilla -- the MODE register (BASE+1): write-only glue, decoded into live status");
    {
        Rig g;
        auto prop = [&](const char* name) -> Property {
            for (auto& p : g.cad->properties())
                if (p.name == name) return p;
            return Property{};
        };
        auto val = [&](const char* name) -> std::string {
            Property p = prop(name);
            return p.get ? p.get().text(p.radix) : std::string("<missing>");
        };
        CHECK(val("hspol") == "positive" && val("vspol") == "positive" && val("amode") == "single",
              "the register comes up all zero");
        CHECK(prop("olen").get().b() == false && val("olsel") == "0", "OLEN off, OLSEL 0");
        CHECK(!prop("hspol").set && !prop("vspol").set && !prop("amode").set && !prop("olen").set &&
                  !prop("olsel").set,
              "all five report read-only -- the register itself is write-only on the wire");

        g.mode_reg(0x01);                          // HSPOL alone
        CHECK(val("hspol") == "negative" && val("vspol") == "positive" && val("amode") == "single",
              "HSPOL decodes on its own");
        g.mode_reg(0x1F);                           // every bit the board connects
        CHECK(val("hspol") == "negative" && val("vspol") == "negative" && val("amode") == "interleaved",
              "all five bits decode independently");
        CHECK(prop("olen").get().b() == true && val("olsel") == "1", "OLEN on, OLSEL 1");
        g.mode_reg(0x10);
        CHECK(prop("olen").get().b() == false && val("olsel") == "1", "OLSEL is its own bit");
        g.mode_reg(0x1F);
        g.m.reset(Reset::Bus);                      // the 74LS273's ~MR is on bus RESET*
        CHECK(val("amode") == "single" && prop("olen").get().b() == false && val("olsel") == "0",
              "RESET* clears the whole register");

        BusCycle c;
        c.type = Cycle::IoRead;
        c.addr = Rig::kMode;
        CHECK(!g.cad->decodes(c), "and it cannot be read back on the bus");
    }

    SECTION("cadzilla -- the ports reach the chips: ACRTC status and registers, DAC palette");
    {
        Rig g;
        CHECK(g.sr() == 0x23, "IN 70 is the ACRTC status register, $23 after power");
        g.reg(0x84, 0x1234);
        CHECK(g.cad->acrtc().reg(0x84) == 0x1234, "OUT 70/71 wrote HDR through AR");
        g.lut(1, 0x10, 0x20, 0x30);
        Color c = g.cad->dac().lookup(1);
        CHECK(c.r == 0x10 && c.g == 0x20 && c.b == 0x30, "OUT 74 then 75 x3 loaded LUT entry 1");
        g.m.bus.ioWrite(0x76, 1);                  // the address register's alias
        CHECK(g.m.bus.ioRead(0x75) == 0x10 && g.m.bus.ioRead(0x75) == 0x20 && g.m.bus.ioRead(0x75) == 0x30,
              "and IN 75 x3 reads it back R, G, B");
    }

    SECTION("cadzilla -- the monitor is there from power-on: a black frame before the ACRTC starts");
    {
        Rig g;
        CHECK(std::string(g.cad->currentMode().name) == "1024x768", "the default mode");
        g.cad->pump();
        CHECK(g.disp.frames(g.cad) == 1, "the first pump presents a frame -- the window opens at the prompt");
        const Surface* s = g.disp.surface(g.cad);
        CHECK(s && s->width() == 1024 && s->height() == 768, "at the mode's size");
        CHECK(g.frameBlack(1024, 768), "and black: no signal");
        g.cad->pump();
        CHECK(g.disp.frames(g.cad) == 1, "and nothing repaints it until something changes");
    }

    SECTION("cadzilla -- 640x480: LUT, ORG, a rectangle, a line, a dot");
    {
        Rig g;
        std::string err;
        CHECK(setProperty(*g.cad, "mode", "640x480", err), "the mode strap takes 640x480");
        g.programMode();
        g.lut(0, 0, 0, 0);
        g.lut(1, 0xFF, 0, 0);                      // 1 = red
        g.lut(2, 0, 0xFF, 0);                      // 2 = green
        g.lut(3, 0, 0, 0xFF);                      // 3 = blue

        // Everything on the 32-pixel sampling grid the frame check reads: frame y = 479 - Y.
        g.color(1);
        g.cmd(0x8000, {0, 31});
        g.cmd(0x9000, {608, 479});                 // ARCT: corners (0,31) and (608,479)
        g.color(3);
        g.cmd(0x8000, {64, 255});
        g.cmd(0x8800, {545, 255});                 // ALINE: x 64..544 on Y = 255 (frame row 224)
        g.color(2);
        g.cmd(0x8000, {320, 127});
        g.cmd(0xCC00);                             // DOT at (320,127): frame (320, 352)

        g.cad->pump();
        CHECK(g.disp.frames(g.cad) == 1, "one frame presented");
        const Surface* s = g.disp.surface(g.cad);
        CHECK(s && s->width() == 640 && s->height() == 480, "the frame is the MONITOR's: 640x480");
        CHECK(g.cad->wiring() == "ok", "GBM 8 bpp, GAI +8, single access: wired as the board is");
        CHECK(g.cad->programmedWidth() == 640 && g.cad->programmedHeight() == 480 &&
                  g.cad->programmedX() == 0 && g.cad->programmedY() == 0,
              "the programmed picture fills the frame from (0,0)");

        TextGridOpts every32;
        every32.xStep = 32;
        every32.yStep = 32;
        CHECK_FRAME_OPTS(g.busView(), g.cad, R"(
11111111111111111111
1..................1
1..................1
1..................1
1..................1
1..................1
1..................1
1.3333333333333333.1
1..................1
1..................1
1..................1
1.........2........1
1..................1
1..................1
11111111111111111111
)", every32, "sampled every 32nd pixel: the rectangle, the line and the dot, right way up");

        // EVERY PIXEL, against what the three commands must have drawn (frame y = 479 - Y).
        // The rectangle's perimeter: x 0..608 on rows 0 and 448, x = 0 and 608 on rows 0..448.
        // The line: x 64..544 on row 224 (its end point 545 not drawn). The dot: (320, 352).
        auto oracle = [](int x, int y) -> uint8_t {
            if (x == 320 && y == 352) return 2;
            if (y == 224 && x >= 64 && x <= 544) return 3;
            bool onRect = (y == 0 || y == 448) ? (x >= 0 && x <= 608)
                                                : ((x == 0 || x == 608) && y > 0 && y < 448);
            return onRect ? 1 : 0;
        };
        CHECK_FRAME_PIXELS(g.busView(), g.cad, oracle, "all 307,200 pixels match the oracle for the three commands");

        // THE RAMDAC: what the wire carries is the LUT's color, not the index.
        const auto pal = g.cad->dac().palette();
        CHECK(pal[1].r == 0xFF && pal[3].b == 0xFF, "the 256 LUT entries, as loaded");
        CHECK(s->format() == PixelFormat::Rgb32, "the host is handed colors");
        std::vector<uint8_t> rgb = frameRgb(*s, {});
        size_t dot = ((size_t)352 * 640 + 320) * 3;
        CHECK(rgb[dot] == 0 && rgb[dot + 1] == 0xFF && rgb[dot + 2] == 0, "the dot resolves to green");

        // A palette-only change: no drawing command, yet the picture on the wire moves.
        uint32_t before = frameCrc(*s, {});
        g.cad->pump();
        CHECK(g.disp.frames(g.cad) == 1, "nothing changed: no new frame");
        g.lut(1, 0xFF, 0xFF, 0xFF);
        g.cad->pump();
        CHECK(g.disp.frames(g.cad) == 2, "a LUT write is a change the host must see");
        CHECK(frameCrc(*g.disp.surface(g.cad), {}) != before, "and the resolved frame differs");
        CHECK(g.px(0, 0) == 1, "while the pixel values are what they were");
    }

    SECTION("cadzilla -- the monitor places the picture by HDS/VDS against its porches");
    {
        Rig g;
        std::string err;
        CHECK(setProperty(*g.cad, "mode", "640x480", err), "the mode strap takes 640x480");
        g.programMode();
        g.color(1);
        g.cmd(0x8000, {0, 479});
        g.cmd(0x8800, {640, 479});                 // the whole top raster in 1
        g.cad->pump();
        CHECK(g.px(0, 0) == 1 && g.px(639, 0) == 1 && g.px(0, 1) == 0, "flush with the frame");

        // HDS one memory cycle later than the back porch: the picture shifts 16 pixels right
        // and its last cycle falls off the right edge -- the blanking on the left is black.
        const uint16_t hdr = g.cad->acrtc().reg(0x84);
        g.reg(0x84, (uint16_t)(hdr + 0x0100));
        g.cad->pump();
        CHECK(g.px(15, 0) == 0 && g.px(16, 0) == 1 && g.px(639, 0) == 1, "shifted right by one cycle");
        CHECK(g.cad->programmedX() == 16, "and SHOW says so");
        g.reg(0x84, (uint16_t)(hdr - 0x0100));     // one cycle EARLY: the first cycle is clipped
        g.cad->pump();
        CHECK(g.px(0, 0) == 1 && g.cad->programmedX() == -16, "shifted left, clipped, reported negative");
        g.reg(0x84, hdr);

        // VDS one raster later: frame row 0 is blanking, the raster is on row 1.
        const uint16_t vdr = g.cad->acrtc().reg(0x88);
        g.reg(0x88, (uint16_t)(vdr + 0x0100));
        g.cad->pump();
        CHECK(g.px(0, 0) == 0 && g.px(0, 1) == 1 && g.cad->programmedY() == 1, "shifted down one raster");
        g.reg(0x88, vdr);

        // A picture narrower than the monitor: HDW halved leaves the right half black.
        g.reg(0x84, (uint16_t)((hdr & 0xFF00) | 19));   // 20 cycles = 320 px
        g.cad->pump();
        CHECK(g.px(319, 0) == 1 && g.px(320, 0) == 0 && g.cad->programmedWidth() == 320, "320 wide, then blanking");
    }

    SECTION("cadzilla -- the board is wired for 8 bpp and GAI +8; anything else is what the hardware would show");
    {
        Rig g;
        std::string err;
        CHECK(setProperty(*g.cad, "mode", "640x480", err), "the mode strap takes 640x480");
        g.programMode();
        // Distinct bytes in the top raster's first 16 words (SAR1 = 0: memory raster 0 is the
        // top of the screen; the ORG sits on the bottom one).
        const uint32_t top = 0;
        for (uint32_t i = 0; i < 16; ++i) g.cad->acrtc().pokeWord(top + i, (uint16_t)(0x2010 + i * 0x0101));
        g.cad->pump();
        CHECK(g.px(0, 0) == 0x10 && g.px(1, 0) == 0x20 && g.px(2, 0) == 0x11 && g.px(16, 0) == 0x18,
              "GAI +8: the second fetch starts eight words on -- a continuous raster");

        g.reg(0x04, 0x4000);                       // GAI = 000: the ACRTC steps ONE word per cycle
        g.cad->pump();
        CHECK(g.cad->wiring() == "OMR GAI is +1 words, the board fetches 8", "SHOW names the mismatch");
        // A fetch reads row A >> 3 of all eight banks (MA2..0 select no bank on a display
        // cycle), so fetches at A = 0..7 all show words 0..7 and A = 8 is the next row.
        CHECK(g.px(0, 0) == 0x10 && g.px(16, 0) == 0x10 && g.px(17, 0) == 0x20 && g.px(112, 0) == 0x10 &&
                  g.px(128, 0) == 0x18,
              "the board still fetches the aligned eight-word row, so the picture repeats itself, as the hardware would");
        g.reg(0x04, 0x4030);

        g.reg(0x02, 0x0200);                       // GBM = 010: 4 bpp
        CHECK(g.cad->wiring() == "CCR GBM is 4 bpp, the board is wired for 8", "a wrong GBM is named too");
        g.cmd(0x0801, {0xFFFF});                   // CL1 = $FFFF: F in every 4-bit field
        g.cmd(0x8000, {1, 479});
        g.cmd(0xCC00);                             // the drawing engine believes 4 bpp: dot 1 is bits 4-7
        g.cad->pump();
        CHECK(g.px(0, 0) == 0xF0, "so the byte the shift register sees has the high nibble lit: scrambled, faithfully");
        g.reg(0x02, 0x0300);
        CHECK(g.cad->wiring() == "ok", "put right, the wiring line clears");

        // The board's OWN glue (MODE AMODE) is what the picture actually uses -- not the
        // ACRTC's OMR ACM bit, which must simply agree with it or `wiring` says so.
        g.reg(0x04, 0x4038);                        // OMR ACM = interleaved; MODE AMODE still single
        CHECK(g.cad->wiring() == "OMR ACM is interleaved, MODE AMODE says single",
              "the two straps disagreeing is named");
        CHECK(g.cad->programmedWidth() == 640, "and the picture still runs on MODE's single, unmoved");
        g.mode_reg(0x04);                           // bring MODE AMODE into agreement: interleaved
        CHECK(g.cad->wiring() == "ok", "agreeing again clears it");
        CHECK(g.cad->programmedWidth() == 320, "and now the SAME HDW register reads as half the pixels");
        g.mode_reg(0x00);
        g.reg(0x04, 0x4030);

        g.reg(0x04, 0x403C);                       // ACM = 11: superimposed
        CHECK(g.cad->wiring() == "OMR ACM is superimposed, the board wires single and interleaved only",
              "superimposed mode is not wired");
    }

    SECTION("cadzilla -- a circle, PAINTed, and AGCPY'd across the screen: the whole picture");
    {
        Rig g;
        std::string err;
        CHECK(setProperty(*g.cad, "mode", "640x480", err), "the mode strap takes 640x480");
        g.programMode();
        g.lut(1, 0xFF, 0, 0);
        g.lut(2, 0, 0xFF, 0);

        // The center on the 32-pixel sampling grid (frame y = 479 - Y), the radius 98 so no
        // grid point is within a pixel of the outline: the grid sees the PAINTed inside only.
        g.color(1);
        g.cmd(0x8000, {160, 223});
        g.cmd(0xA800, {98});                       // CRCL r = 98, outline in 1
        g.cmd(0x0803, {0x0101});                   // EDG = 1
        g.cmd(0x0800, {0x0202});                   // CL0 = CL1 = 2
        g.cmd(0x0801, {0x0202});
        g.cmd(0xC800);                             // PAINT from the center
        g.cmd(0x8000, {286, 125});                 // the copy's bottom-left corner: 224 to the right
        g.cmd(0xE000, {62, 125, 196, 196});        // AGCPY S = 0 DSD = 000: the disk's bounding box
        CHECK((g.sr() & 0x80) == 0, "no command error on the way");

        g.cad->pump();
        TextGridOpts every32;
        every32.xStep = 32;
        every32.yStep = 32;
        CHECK_FRAME_OPTS(g.busView(), g.cad, R"(
....................
....................
....................
....................
....................
.....2......2.......
...22222..22222.....
...22222..22222.....
..22222222222222....
...22222..22222.....
...22222..22222.....
.....2......2.......
....................
....................
....................
)", every32, "sampled every 32nd pixel: the painted disk, and its copy 224 pixels to the right");
    }

    SECTION("cadzilla -- 1024x768 in interleaved mode: doubled horizontal registers, 8 pixels a cycle");
    {
        Rig g;
        std::string err;
        CHECK(setProperty(*g.cad, "mode", "1024x768", err), "the mode strap takes 1024x768");
        g.programMode(true);
        CHECK((g.cad->acrtc().reg(0x84) & 0xFF) == 127 && g.cad->acrtc().accessMode() == 2,
              "HDW = 127: 128 memory cycles of 8 pixels, ACM interleaved");
        g.color(1);
        g.cmd(0x8000, {0, 63});
        g.cmd(0x9000, {960, 767});                 // corners on the 64-pixel sampling grid
        g.cad->pump();
        const Surface* s = g.disp.surface(g.cad);
        CHECK(s && s->width() == 1024 && s->height() == 768, "the frame is 1024x768");
        CHECK(g.cad->wiring() == "ok" && g.cad->programmedWidth() == 1024 &&
                  g.cad->programmedHeight() == 768 && g.cad->programmedX() == 0 && g.cad->programmedY() == 0,
              "the doubled registers describe the same full-frame picture");
        TextGridOpts every64;
        every64.xStep = 64;
        every64.yStep = 64;
        CHECK_FRAME_OPTS(g.busView(), g.cad, R"(
1111111111111111
1..............1
1..............1
1..............1
1..............1
1..............1
1..............1
1..............1
1..............1
1..............1
1..............1
1111111111111111
)", every64, "sampled every 64th pixel: the border, at 1024x768 through the interleaved fetch");
        // Every pixel: the perimeter of (0,63)-(960,767) is rows 0 and 704, columns 0 and 960.
        CHECK_FRAME_PIXELS(g.busView(), g.cad, [](int x, int y) -> uint8_t {
            bool on = (y == 0 || y == 704) ? (x <= 960) : ((x == 0 || x == 960) && y < 704);
            return on ? 1 : 0;
        }, "all 786,432 pixels of the interleaved 1024x768 frame match");

        // In interleaved mode HDS is in doubled units: +2 memory cycles is one display cycle.
        const uint16_t hdr = g.cad->acrtc().reg(0x84);
        g.reg(0x84, (uint16_t)(hdr + 0x0200));
        g.cad->pump();
        CHECK(g.px(15, 0) == 0 && g.px(16, 0) == 1 && g.cad->programmedX() == 16, "shifted one display cycle: 16 pixels");
    }

    SECTION("cadzilla -- every mode: the frame is the strap's size and a full picture fills it");
    {
        for (int i = 0; i < CadzillaBoard::modeCount(); ++i) {
            Rig g;
            std::string err;
            const auto& md = CadzillaBoard::mode(i);
            CHECK(setProperty(*g.cad, "mode", md.name, err), "the strap takes every listed mode");
            g.programMode();
            g.color(1);
            g.cmd(0x8000, {0, 0});
            g.cmd(0xC000, {(uint16_t)(md.width - 1), (uint16_t)(md.height - 1)});   // AFRCT: fill it all
            g.cad->pump();
            const Surface* s = g.disp.surface(g.cad);
            CHECK(s && s->width() == md.width && s->height() == md.height, "the frame is the mode's size");
            std::string what = std::string(md.name) + ": every pixel of the frame is the fill";
            CHECK_FRAME_PIXELS(g.busView(), g.cad, [](int, int) -> uint8_t { return 1; }, what.c_str());
        }
    }

    SECTION("cadzilla -- the window screen lands in the frame at HWS/VWS");
    {
        Rig g;
        g.programMode();
        // A 16 x 2 window one display cycle in and three rasters down, on screen 3 at $70000.
        const auto& md = g.cad->currentMode();
        g.reg(0x92, (uint16_t)((md.hbp << 8) | 0));              // HWS = hbp: one cycle after the display start; HWW = 1 cycle
        g.reg(0x94, (uint16_t)(md.vbp + 3 - 1));                  // VWS: raster 3 of the picture
        g.reg(0x96, 2);                                           // VWW
        g.reg(0xDA, 8);                                           // MWR3 = 8 words
        g.reg(0xDC, 0x0007);
        g.reg(0xDE, 0x0000);                                      // SAR3 = $70000
        g.reg(0x06, 0x4300);                                      // SE1 + SE3 = 11
        for (uint32_t i = 0; i < 16; ++i) g.cad->acrtc().pokeWord(0x70000 + i, 0x0505);
        g.cad->pump();
        CHECK(g.px(15, 3) == 0 && g.px(16, 3) == 5 && g.px(31, 3) == 5 && g.px(32, 3) == 0,
              "raster 3: the window's 16 pixels at x = 16..31");
        CHECK(g.px(16, 4) == 5 && g.px(16, 2) == 0 && g.px(16, 5) == 0, "two rasters tall, from raster 3");
    }

    // ---- THE OVERLAY: a one-bit plane in its own SRAM, shifted into the Bt453's OL1 ----

    SECTION("cadzilla -- the overlay is drawn through a CHR screen, into its own SRAM");
    {
        Rig g;
        std::string err;
        CHECK(setProperty(*g.cad, "mode", "640x480", err), "the mode strap takes 640x480");
        g.programMode();
        g.overlayScreen();
        g.dot(320, 127);                           // frame (320, 352)
        g.dot(335, 127);                           // (335, 352): the same 16 pixels, the last one
        const uint32_t o = 352u * 40u + 20u;       // frame pixel p = 352 x 640 + 320 -> word p >> 4
        CHECK(g.cad->overlayWord(o) == 0x8001, "pixels 320 and 335 are bits 0 and 15 of overlay word p >> 4");
        bool clean = true;
        for (uint16_t v : g.cad->acrtc().vram())
            if (v) clean = false;
        CHECK(clean, "no frame-memory bank is enabled on a CHR access: the frame memory is untouched");
        CHECK(g.cad->overlayWord(0x10000 + o) == 0, "OLSEL 0: the lower half only");

        g.mode_reg(0x10);                          // OLSEL: A16 on drawing cycles
        g.dot(321, 127);
        CHECK(g.cad->overlayWord(0x10000 + o) == 0x0002 && g.cad->overlayWord(o) == 0x8001,
              "OLSEL 1: the same drawing address reaches the upper half");
        g.mode_reg(0x00);

        g.frameScreen();
        g.color(1);
        g.dot(0, 0);                               // frame (0, 479): word 479 x 320
        CHECK(g.cad->acrtc().peekWord(479u * 320u) == 0x0001 && g.cad->overlayWord(479u * 40u) == 0,
              "a graphic screen (CHR = 0) draws the frame memory again");
        CHECK(g.cad->wiring() == "ok", "an undisplayed CHR screen is what the board is wired for");
    }

    SECTION("cadzilla -- OLEN shows the overlay as overlay color 2, single and interleaved");
    for (bool il : {false, true}) {
        Rig g;
        std::string err;
        const std::string how = il ? " (interleaved)" : " (single)";
        std::string       msg;
        auto              say = [&](const char* t) { return (msg = t + how).c_str(); };
        CHECK(setProperty(*g.cad, "mode", "640x480", err), "the mode strap takes 640x480");
        g.programMode(il);
        const uint8_t amode = il ? 0x04 : 0x00;
        g.lut(1, 0xFF, 0, 0);                      // 1 = red
        g.ovlColor(1, 0xFF, 0, 0xFF);              // the overlay colors the board never selects...
        g.ovlColor(3, 0xFF, 0xFF, 0);
        g.ovlColor(2, 0, 0xFF, 0xFF);              // ...and the one it does: cyan
        g.color(1);
        g.cmd(0x8000, {304, 127});
        g.cmd(0x8800, {352, 127});                 // a red line under the overlay, x 304..351
        g.overlayScreen();
        g.dot(320, 127);
        g.dot(335, 127);
        g.frameScreen();

        g.cad->pump();
        CHECK(g.px(320, 352) == 1 && g.cad->busPixel(320, 352) == 0x0001, say("OLEN 0: OL1..0 stay 0"));
        CHECK(g.rgb(320, 352).r == 0xFF && g.rgb(320, 352).g == 0, say("and the palette shows: red"));

        g.mode_reg((uint8_t)(amode | 0x08));       // OLEN
        g.cad->pump();
        CHECK(g.cad->busPixel(320, 352) == 0x0201 && g.cad->busPixel(335, 352) == 0x0201,
              say("OLEN 1: the set bits drive OL1 -- OL1..0 = 10, P7..0 still the frame's"));
        CHECK(g.cad->busPixel(319, 352) == 0x0001 && g.cad->busPixel(321, 352) == 0x0001 &&
                  g.cad->busPixel(336, 352) == 0x0001,
              say("bit 0 is the fetch's leftmost pixel, bit 15 its rightmost, and nothing else is lit"));
        const Color c = g.rgb(320, 352);
        CHECK(c.r == 0 && c.g == 0xFF && c.b == 0xFF, say("the Bt453 shows overlay color 2 over the palette"));
        CHECK(g.rgb(321, 352).r == 0xFF && g.rgb(321, 352).b == 0, say("its neighbor is the palette's red"));
        CHECK(g.cad->wiring() == "ok", say("wired as the board is"));
    }

    SECTION("cadzilla -- the overlay word goes with its fetch: A >> 3, OLSEL's half behind the upper 1 MB");
    {
        Rig g;
        std::string err;
        CHECK(setProperty(*g.cad, "mode", "640x480", err), "the mode strap takes 640x480");
        g.programMode();
        g.ovlColor(2, 0xFF, 0xFF, 0xFF);
        g.overlayScreen();
        g.org(0, 0);                               // overlay word 0, dot 0: Y counts down from here
        g.mode_reg(0x10);                          // OLSEL 1: draw word $10000
        g.dot(0, 0);
        g.mode_reg(0x08);                          // OLEN, OLSEL 0
        g.dot(1, 0);                               // word 0, bit 1
        CHECK(g.cad->overlayWord(0x10000) == 0x0001 && g.cad->overlayWord(0) == 0x0002, "both halves drawn");
        g.frameScreen();

        g.cad->pump();                             // SAR1 = 0: the top raster fetches A = 0
        CHECK(g.cad->busPixel(1, 0) == 0x0200 && g.cad->busPixel(0, 0) == 0,
              "SAR 0: the lower half's word 0 is the top-left 16 pixels");
        g.reg(0xCC, 0x0008);
        g.reg(0xCE, 0x0000);                       // SAR1 = $80000: the upper 1 MB
        g.cad->pump();
        CHECK(g.cad->busPixel(0, 0) == 0x0200 && g.cad->busPixel(1, 0) == 0,
              "SAR $80000: fetch A >> 3 = $10000, the half OLSEL 1 drew");
        g.reg(0xCC, 0x0000);
        g.reg(0xCE, 0x0003);                       // SAR1 = 3: not a multiple of 8
        g.cad->pump();
        CHECK(g.cad->busPixel(1, 0) == 0x0200, "SAR 3: the same row, so the same overlay word");
    }

    SECTION("cadzilla -- a display fetch reads the aligned row: a start address's low 3 bits select nothing");
    {
        Rig g;
        std::string err;
        CHECK(setProperty(*g.cad, "mode", "640x480", err), "the mode strap takes 640x480");
        g.programMode();
        for (uint32_t i = 0; i < 16; ++i) g.cad->acrtc().pokeWord(i, (uint16_t)(0x2010 + i * 0x0101));
        g.reg(0xCE, 0x0003);                       // SAR1 = 3
        g.cad->pump();
        CHECK(g.px(0, 0) == 0x10 && g.px(1, 0) == 0x20 && g.px(15, 0) == 0x27,
              "the first fetch is words 0-7, bank 0 first, as at SAR 0");
        CHECK(g.px(16, 0) == 0x18, "the second, at A = 11, is the next row: words 8-15");
    }

    SECTION("cadzilla -- BLANK* is black, whatever the palette's entry 0 is");
    {
        Rig g;
        std::string err;
        CHECK(setProperty(*g.cad, "mode", "640x480", err), "the mode strap takes 640x480");
        g.lut(0, 0x40, 0x40, 0x40);                // entry 0: grey
        g.cad->pump();
        CHECK(g.frameBlack(640, 480), "before the ACRTC starts: no fetch anywhere, black");
        g.programMode();
        const uint16_t hdr = g.cad->acrtc().reg(0x84);
        g.reg(0x84, (uint16_t)((hdr & 0xFF00) | 19));   // 20 cycles = 320 px
        g.cad->pump();
        CHECK(g.rgb(100, 0).r == 0x40 && g.px(100, 0) == 0, "a fetched pixel of value 0 is entry 0: grey");
        CHECK(g.cad->busPixel(400, 0) == CadzillaBoard::kBusBlank && g.rgb(400, 0).r == 0 &&
                  g.rgb(400, 0).g == 0 && g.rgb(400, 0).b == 0,
              "past the display width the pixel is BLANK: black, not entry 0");
    }

    SECTION("cadzilla -- wiring names a displayed CHR screen; a snapshot carries the overlay");
    {
        Rig g;
        std::string err;
        CHECK(setProperty(*g.cad, "mode", "640x480", err), "the mode strap takes 640x480");
        g.programMode();
        g.ovlColor(2, 0, 0xFF, 0);
        g.overlayScreen();
        g.dot(32, 479);                            // the top raster, x 32: overlay word 2, bit 0
        g.frameScreen();
        g.mode_reg(0x18);                          // OLEN, OLSEL
        CHECK(g.cad->wiring() == "ok", "screen 0 is CHR but not displayed: ok");
        g.reg(0x06, 0x7000);                       // DCR: SE1, and the upper screen enabled too
        CHECK(g.cad->wiring() == "MWR0 CHR is set on a displayed screen, the board has no character display path",
              "a displayed CHR screen is named");
        g.reg(0x06, 0x4000);

        StateWriter w;
        g.cad->serialize(w);
        Rig h;
        CHECK(setProperty(*h.cad, "mode", "640x480", err), "the mode strap takes 640x480");
        StateReader r(w.data());
        h.cad->deserialize(r);
        CHECK(r.ok(), "the state reads back");
        CHECK(h.cad->overlayWord(2) == 0x0001, "the overlay SRAM travelled");
        h.cad->pump();
        CHECK(h.cad->busPixel(32, 0) == 0x0200 && h.rgb(32, 0).g == 0xFF, "and OLEN with it: the overlay shows");
        h.overlayScreen();
        h.dot(48, 479);                            // OLSEL travelled too: it draws the upper half
        CHECK(h.cad->overlayWord(0x10000 + 3) == 0x0001, "and OLSEL with it");
        g.adopt();
    }

    SECTION("cadzilla -- interrupts (SW1-8): disconnected by default, strapped to pin 73 or a VI line");
    {
        Rig g;
        std::string err;
        CHECK(!g.cad->assertsInt() && g.cad->assertsVi() == 0,
              "SW1-8 off by default: nothing pulled, whatever the chip wants");

        // Arm CED's interrupt enable (CCR bit 5, CEE) so the reset-time CED=1 becomes a
        // live, enabled request -- acrtc_.irq() is SR & CCR's enables, and CED is one of
        // the eight bits both share.
        g.reg(0x02, 0x0020);
        CHECK(g.cad->acrtc().irq(), "the chip itself wants an interrupt: CED is set and CEE enables it");
        CHECK(!g.cad->assertsInt() && g.cad->assertsVi() == 0,
              "but SW1-8 is still off -- the chip's own IRQ* pin does not reach the bus");

        CHECK(setProperty(*g.cad, "interrupt", "vi3", err), "strap SW1-8 on, to VI3");
        CHECK(g.cad->assertsVi() == 0x08 && !g.cad->assertsInt(),
              "VI3 (bit 3) is pulled; pin 73 is not -- the strap chose a VI line, not int");

        auto prop = [&](const char* name) -> Property {
            for (auto& p : g.cad->properties())
                if (p.name == name) return p;
            return Property{};
        };
        CHECK(prop("irq").get().b() == true, "SHOW's irq line agrees: asserted right now");

        CHECK(setProperty(*g.cad, "interrupt", "int", err), "restrap to pin 73 instead");
        CHECK(g.cad->assertsInt() && g.cad->assertsVi() == 0,
              "now pin 73 is pulled and no VI line is");

        // Clearing CEE drops the enabled condition -- acrtc_.irq() is pure (SR & CCR's
        // enables), so it and both assertsInt()/assertsVi() follow the register write
        // immediately, with no separate "clear" step of their own.
        g.reg(0x02, 0x0000);   // CCR low byte back to 0: CEE disabled: CED is still SET in
                                // SR (nothing read it), but no longer an ENABLED request
        CHECK(!g.cad->acrtc().irq() && !g.cad->assertsInt() && g.cad->assertsVi() == 0,
              "disabling CEE drops the request even though CED itself is still pending");
        CHECK(prop("irq").get().b() == false, "and SHOW's irq line drops with it");
    }

    SECTION("cadzilla -- SHOW: the straps validate, the live lines are read-only");
    {
        Rig g;
        auto prop = [&](const char* name) -> Property {
            for (auto& p : g.cad->properties())
                if (p.name == name) return p;
            return Property{};
        };
        auto val = [&](const char* name) -> std::string {
            Property p = prop(name);
            return p.get ? p.get().text(p.radix) : std::string("<missing>");
        };
        CHECK(val("mode") == "1024x768", "defaults: 1024x768");
        CHECK(g.cad->acrtc().vram().size() == 1u << 20, "2 MB of SRAM -- the ACRTC's whole 1 M-word "
              "address space, fixed, not a strap");
        CHECK(!prop("vram").get, "there is no vram property at all any more");
        CHECK(val("video") == "off" && val("status") == "0x23", "comes up stopped");
        CHECK(val("interrupt") == "none" && val("irq") == "false", "SW1-8 off by default: IRQ* disconnected");
        CHECK(!prop("video").set && !prop("picture").set && !prop("wiring").set && !prop("status").set &&
                  !prop("irq").set,
              "live status is read-only");
        CHECK((bool)prop("mode").set && (bool)prop("port").set && (bool)prop("interrupt").set,
              "the straps are settable");
        CHECK(!prop("hspol").set && !prop("vspol").set && !prop("amode").set && !prop("olen").set,
              "the MODE register's four decoded fields are read-only, like the rest of live status");
        g.programMode();
        CHECK(val("video") == "on" && val("picture") == "1024x768 at (0,0)" && val("wiring") == "ok",
              "live values follow the registers");

        std::string err;
        CHECK(!setProperty(*g.cad, "mode", "320x200", err), "an unlisted mode is refused");
        CHECK(!setProperty(*g.cad, "port", "71", err), "a BASE that is not a multiple of 8 is refused");
        CHECK(!setProperty(*g.cad, "port", "74", err), "not even a multiple of 4 is enough now -- the whole block moves together");
        CHECK(!setProperty(*g.cad, "interrupt", "vi9", err), "an interrupt strap outside vi0..vi7 is refused");
        CHECK(setProperty(*g.cad, "port", "E0", err), "a multiple of 8 is taken");
        BusCycle c;
        c.type = Cycle::IoWrite;
        c.addr = 0xE1;
        CHECK(g.cad->decodes(c), "and the MODE register now decodes at the new BASE+1");
        c.type = Cycle::IoRead;
        c.addr = 0xE2;
        CHECK(g.cad->decodes(c), "the ACRTC's data/FIFO port now decodes at BASE+2");
        c.addr = 0xE4;
        CHECK(g.cad->decodes(c), "with the Bt453 four ports along, still at BASE+4 -- one strap moves both chips");
        c.addr = 0x74;
        CHECK(!g.cad->decodes(c), "and nothing answers at the old Bt453 address any more");

        // Changing the mode re-opens the window at the new size on the next frame.
        Rig h;
        CHECK(setProperty(*h.cad, "mode", "640x480", err), "the mode strap takes 640x480");
        h.programMode();
        h.cad->pump();
        CHECK(setProperty(*h.cad, "mode", "800x600", err), "SET mode=800x600");
        h.cad->pump();
        CHECK(h.disp.surface(h.cad)->width() == 800 && h.disp.surface(h.cad)->height() == 600,
              "the frame is 800x600 now -- the 640x480 picture sits in its top-left corner");
        CHECK(h.px(639, 479) == 0 && h.cad->programmedWidth() == 640, "the programmed picture did not change");
    }

    SECTION("cadzilla -- RESET* resets the ACRTC and keeps the palette; a snapshot restores the picture");
    {
        Rig g;
        std::string err;
        CHECK(setProperty(*g.cad, "mode", "640x480", err), "the mode strap takes 640x480");
        g.programMode();
        g.lut(1, 0xFF, 0, 0);
        g.color(1);
        g.cmd(0x8000, {0, 31});
        g.cmd(0x9000, {608, 479});
        g.cad->pump();

        StateWriter w;
        g.cad->serialize(w);

        Rig h;
        CHECK(setProperty(*h.cad, "mode", "640x480", err), "the mode strap takes 640x480");
        StateReader r(w.data());
        h.cad->deserialize(r);
        CHECK(r.ok(), "the state reads back");
        h.cad->pump();
        TextGridOpts every32;
        every32.xStep = 32;
        every32.yStep = 32;
        CHECK_FRAME_OPTS(h.busView(), h.cad, R"(
11111111111111111111
1..................1
1..................1
1..................1
1..................1
1..................1
1..................1
1..................1
1..................1
1..................1
1..................1
1..................1
1..................1
1..................1
11111111111111111111
)", every32, "the restored board repaints the same picture from its own frame memory");
        CHECK(h.cad->dac().lookup(1).r == 0xFF, "with the same palette");

        g.adopt();
        g.m.reset(Reset::Bus);                     // RESET*: the ACRTC stops (STR clears)
        CHECK(!g.cad->acrtc().displayOn(), "RESET* stops the display");
        CHECK(g.cad->dac().lookup(1).r == 0xFF, "the Bt453 has no reset pin: the LUT survives");
        g.cad->pump();
        CHECK(g.frameBlack(640, 480), "and the monitor shows a black 640x480 frame -- the window stays, the picture is gone");
    }

    // ---- DRAWING TIME: the draw_rate strap, 2CLK from the pixel clock, one deadline ----

    SECTION("cadzilla -- draw_rate: full by default, real by SET or from the machine file");
    {
        Rig g;
        std::string err;
        CHECK(!g.cad->acrtc().timed(), "full (instant drawing) is the default");
        g.cmd(0x5800, {0, 15, 15});
        CHECK((g.sr() & 0x23) == 0x23, "full: a CLR is done the moment it lands");
        CHECK(setProperty(*g.cad, "draw_rate", "real", err) && g.cad->acrtc().timed(), "SET draw_rate=real");
        CHECK(!setProperty(*g.cad, "draw_rate", "fast", err), "anything but full or real is refused");

        // The file, the way an operator sets it -- not only SET at the prompt (DESIGN.md 8).
        const char* kText = R"(
[machine]
name = "drawtime"

[[board]]
type      = "cadzilla"
id        = "cad0"
port      = 0x70
draw_rate = "real"
)";
        Machine fm;
        CHECK(loadTomlText(kText, "drawtime", fm, err), "a machine file with draw_rate loads");
        auto* fc = dynamic_cast<CadzillaBoard*>(fm.find("cad0"));
        CHECK(fc && fc->acrtc().timed(), "draw_rate = \"real\" in the file times the ACRTC");
        CHECK(saveTomlText(fm).find("draw_rate = \"real\"") != std::string::npos, "and CONFIG SAVE writes it back");
    }

    SECTION("cadzilla -- draw_rate=real: 2CLK is the pixel clock / 8 single, / 4 interleaved");
    {
        std::string err;
        {
            Rig g;                                // 1024x768: 65 MHz / 8 = 8.125 MHz; CPU Hz 2 MHz
            CHECK(setProperty(*g.cad, "draw_rate", "real", err), "real");
            CHECK(g.cad->twoClkHz() == 8125000, "1024x768 single access: 8.125 MHz");
            g.cmd(0xCC00);                        // DOT: 8 2CLK = 1.97 T-states -> the 2nd
            g.m.clock.advance(1);
            CHECK((g.sr() & 0x20) == 0, "a DOT is not done one T-state later");
            g.m.clock.advance(1);
            CHECK((g.sr() & 0x20) != 0, "and done two T-states later");

            g.cmd(0x5800, {0, 15, 15});           // CLR: 652 2CLK = 160.5 T-states
            g.m.clock.advance(160);
            CHECK((g.cad->acrtc().status() & 0x20) == 0, "a 16 x 16 CLR is not done after 160 T-states");
            g.m.clock.advance(1);
            CHECK((g.cad->acrtc().status() & 0x20) != 0,
                  "and done at 161 -- with no bus cycle: the deadline brought it in");

            // A guest polls SR every few T-states, and each poll carries T-states into 2CLK:
            // the fraction (4.0625 2CLK a T-state here) must travel, or every poll loses some.
            g.cmd(0x5800, {0, 15, 15});
            const uint64_t t0 = g.m.clock.now();
            while ((g.sr() & 0x20) == 0 && g.m.clock.now() - t0 < 1000) g.m.clock.advance(1);
            CHECK(g.m.clock.now() - t0 == 161, "polled every T-state, the CLR still takes 161");
        }
        {
            Rig g;
            CHECK(setProperty(*g.cad, "mode", "640x480", err), "640x480");
            CHECK(setProperty(*g.cad, "draw_rate", "real", err), "real");
            g.mode_reg(0x04);                     // AMODE: interleaved
            CHECK(g.cad->twoClkHz() == 6293750, "640x480 interleaved: 25.175 MHz / 4");
            g.cmd(0x5800, {0, 15, 15});           // 652 2CLK = 207.2 T-states
            g.m.clock.advance(207);
            CHECK((g.cad->acrtc().status() & 0x20) == 0, "not done at 207");
            g.m.clock.advance(1);
            CHECK((g.cad->acrtc().status() & 0x20) != 0, "done at 208");
        }
        {
            Rig g;                                // AMODE moves mid-command: the rest at the new rate
            CHECK(setProperty(*g.cad, "draw_rate", "real", err), "real");
            g.cmd(0x5800, {0, 15, 15});
            g.m.clock.advance(80);                // 80 T x 8.125 MHz / 2 MHz = 325 2CLK paid
            g.mode_reg(0x04);                     // now 16.25 MHz: 327 left = 40.2 T
            g.m.clock.advance(40);
            CHECK((g.cad->acrtc().status() & 0x20) == 0, "not done 40 T-states after the switch");
            g.m.clock.advance(1);
            CHECK((g.cad->acrtc().status() & 0x20) != 0, "done at 41: the time before it counted at the old rate");
        }
    }

    SECTION("cadzilla -- draw_rate=real: the drawing time is emulated time, whatever clock_hz is");
    {
        std::string err;
        const auto clrDone = [&](long long hz) {
            Rig g;
            g.m.clock.setHz(hz);
            setProperty(*g.cad, "draw_rate", "real", err);
            g.cmd(0x5800, {0, 15, 15});
            uint64_t t = 0;
            while ((g.cad->acrtc().status() & 0x20) == 0 && t < 100000) {
                g.m.clock.advance(1);
                ++t;
            }
            return t;
        };
        CHECK(clrDone(0) == 161 && clrDone(2000000) == 161,
              "flat out (clock_hz = 0) and 2 MHz: the same 161 T-states -- the guest cannot tell them apart");
        CHECK(clrDone(4000000) == 321, "a 4 MHz CPU runs twice the instructions in the ACRTC's same 80 us");
    }

    SECTION("cadzilla -- draw_rate=real: the end raises IRQ* on its own, and survives a snapshot");
    {
        Rig g;
        std::string err;
        CHECK(setProperty(*g.cad, "draw_rate", "real", err), "real");
        CHECK(setProperty(*g.cad, "interrupt", "int", err), "IRQ* to pin 73");
        g.reg(0x02, 0x0020);                      // CCR: CED interrupt enabled
        g.cmd(0x5800, {0, 15, 15});
        CHECK(!g.cad->assertsInt(), "no interrupt while the CLR is drawn");
        g.m.clock.advance(161);                   // the CPU halted: not one bus cycle
        CHECK(g.cad->assertsInt(), "the command's end raises IRQ* with nobody touching the board");

        g.cmd(0x5800, {0, 15, 15});               // (reading SR is no acknowledge: CED is simply clear)
        g.m.clock.advance(100);
        StateWriter w;
        g.cad->serialize(w);
        Rig h;
        CHECK(setProperty(*h.cad, "draw_rate", "real", err), "real, on the restoring board too");
        h.m.clock.advance(g.m.clock.now());
        StateReader r(w.data());
        h.cad->deserialize(r);
        CHECK(r.ok() && h.cad->acrtc().busy(), "the CLR in flight travelled");
        h.m.clock.advance(60);
        CHECK((h.cad->acrtc().status() & 0x20) == 0, "not done 160 T-states in");
        h.m.clock.advance(1);
        CHECK((h.cad->acrtc().status() & 0x20) != 0, "the restored board re-armed its end: done at 161");
        g.adopt();
    }
}
