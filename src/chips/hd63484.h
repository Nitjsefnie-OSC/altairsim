#pragma once
//
// Hitachi HD63484 ACRTC -- Advanced CRT Controller. A CHIP, NOT A CARD, exactly as the
// FD1771 and the 8257 next door are. The CADzilla video board has one; the next board
// that turns up with an ACRTC gets it for free.
//
// A CRT controller with a DRAWING PROCESSOR and its OWN frame memory: up to 1 M words
// of 16 bits that the host never addresses directly. The host talks to it through two
// locations and a pair of FIFOs -- it sets the raster timing in ~30 control registers,
// then draws by issuing COMMANDS ("line to (x, y)", "clear this rectangle") that the
// chip executes against the frame memory in logical X-Y pixel coordinates at 1, 2, 4,
// 8 or 16 bits per pixel. The picture is whatever is in that memory, scanned out through
// the display-control registers.
//
// SOURCES (docs/sources.md). Modeled from the Hitachi datasheet (reference/Hitachi
// HD63484 ACRTC.md, register LAYOUT) and the Hitachi HD63484 User's Manual
// (reference/Hitachi HD63484 ACRTC User's Manual.md, register and command SEMANTICS --
// #U75, Nov 1984) -- every register field, every status rule and every opcode below cites
// one of them. MAME's hd63484.cpp (BSD-3-Clause,
// (c) Angelo Salese, Sandro Ronco; LICENSE-MAME-HD63484) was read as the STRUCTURAL
// basis -- the shape of the register file, the FIFO, the command dispatch and the
// scan-out -- and where it and the datasheet disagree the datasheet won (the FIFO is 8
// words deep, not 16; a line's pattern scan starts at PPX, not PSX). Nothing here was
// derived by running it.
//
// It knows nothing about S-100 and nothing about the board's ports. The BOARD decodes the
// two host locations somewhere in its I/O map and forwards each as RS (0 or 1); the board
// also decided how much DRAM it fitted (the constructor) and how the 16-bit frame-memory
// word reaches its shift register (see scanline()).
//
// ---------------------------------------------------------------------------
// THE HOST INTERFACE (datasheet "Hardware Access"; manual 5.1-5.4)
//
//   RS=0  write -> ADDRESS REGISTER (AR), 8 bits: which control register RS=1 reaches.
//         read  -> STATUS REGISTER (SR):  CER ARD CED LPD RFF RFR WFR WFE  (D7..D0)
//   RS=1  the 16-bit control register AR names, or the FIFOs when AR = 0.
//
// 8-BIT MPU MODE (manual 5.2, 5.4, 6.2), which is the only mode an 8-bit S-100 host can
// use and the only one modeled: every control register is 16 bits, so each is TWO byte
// locations -- an EVEN AR reaches the high byte, an ODD AR the low byte. Accesses to
// r80-rFF (the timing and display-control RAM) auto-increment AR by one so a block load
// needs one AR write; accesses to r00-r7F do not. The FIFOs sit at AR = 0 and pair bytes
// HIGH THEN LOW into 16-bit words with AR left alone.
//
// THE FIFOs (manual 5.3, 5.4): a write FIFO and a read FIFO, EACH 8 WORDS (16 bytes).
// Commands and their parameters go in through the write FIFO; RPR/RPTN/RD results come
// back through the read FIFO. The status bits are the flow control a driver polls:
//   WFE  write FIFO empty (safe to write up to 8 words)     WFR  not full (1 word ok)
//   RFR  read FIFO has a word                               RFF  read FIFO full
//   CED  the chip can take a NEW command (cleared when one is written, set when it ends)
//   CER  an undefined command or a bad parameter; cleared only by ABT
//   ARD  a drawing crossed the defined area (AREA modes); cleared by RPR or ABT
// RES* and ABT (CCR bit 15) both put SR at $23 = CED|WFR|WFE with both FIFOs cleared.
//
// ---------------------------------------------------------------------------
// COORDINATES AND THE FRAME MEMORY (manual 5.10, 6.6-6.8, ORG)
//
// The frame memory is sequential 16-bit words. A logical pixel of bpp bits lives in one
// word at DOT ADDRESS d, occupying bits [d*bpp .. d*bpp+bpp-1] -- pixel 0 is the LOW
// bits (manual ORG-3: "bit position 4-7 at word $25" is DPD=4 at 4 bpp). X increases
// through the dot addresses of a word and then into the next word; a raster is MW words
// wide (the screen's Memory Width register), and +Y is the raster ABOVE, i.e. MW words
// LOWER in memory -- ORG sets the origin's physical word address and the axes point
// right and UP from it. Scan-out reads rasters from the Start Address upward in memory,
// so the top raster on the screen is the lowest address.
//
// WHICH PIXEL IS LEFTMOST is not the chip's decision: it puts a word on its MAD bus and
// the BOARD's shift register serializes it. For the drawing engine's +X to be "to the
// right", the board must shift the LOW dot address out first, and that is the only
// wiring under which the ACRTC's own manual examples make sense; scanline() assumes it.
//
// ---------------------------------------------------------------------------
// WHAT IS MODELED, AND WHAT IS NOT (docs/boards/cadzilla.md has the honest list)
//
//   Register file: all of it, with the auto-increment rule.       FIFOs and SR: all.
//   Commands: ORG WPR RPR WPTN RPTN / RD WT MOD DRD DWT DMOD CLR SCLR CPY SCPY / AMOVE RMOVE
//             ALINE RLINE ARCT RRCT APLL RPLL APLG RPLG CRCL ELPS AARC RARC AEARC REARC
//             AFRCT RFRCT PAINT DOT PTN AGCPY RGCPY, with every
//             OPM, every COL and every AREA mode, the pattern pointer and its zoom
//             counters stepping live (RPR reads them back); CRCL ELPS AARC RARC AEARC
//             REARC traced pixel by pixel in their C direction; PAINT as a scan-line
//             seed fill with no stack limit; PTN in all sixteen SL/SD directions; AGCPY
//             RGCPY with every S/DSD rotation and mirror. Every opcode is executed; an
//             undefined one sets CER.
//   Reads:    a read that does not fit the read FIFO waits for room, and the command
//             stream waits behind it (manual RD-1).
//   Scan-out: the three background screens (upper/base/lower) stacked by SP0/SP1/SP2 and
//             the window over them, graphic screens only, non-interlaced, GAI +1/+2/+4/+8.
//   Timing:   TWO MODES, the board's choice (setTimed). UNTIMED (the default): drawing is
//             instantaneous at the moment the last parameter lands -- WFE and CED are the
//             steady state and a guest that times a command sees an infinitely fast ACRTC.
//             TIMED: every command costs its datasheet Table 3 2CLK count, paid in the
//             memory cycles the display leaves free for drawing (DRAWING TIME, below);
//             until it is paid, the words behind it wait in the write FIFO and CED, the
//             SR flags it raised and any words it answers stay unseen. Either way the
//             raster counter is not modeled and DTACK/wait states do not exist.
//   Character space: DRAWING only. A drawing access to a screen whose MWR CHR bit is set
//             goes to the board's character memory (CHARACTER ADDRESS SPACE, below).
//   Not modeled: zoom, character screen DISPLAY (raster addressing, RA0-RA4, block
//             cursors), graphic cursors, light pen, blink attributes, DMA handshaking
//             (DRD/DWT/DMOD run in the manual's "under program control" mode), interlace,
//             master/slave sync.
//
// ---------------------------------------------------------------------------
// THE CHARACTER ADDRESS SPACE (manual 3.1.1, 4.2.3.13)
//
// "The ACRTC controls two separate logical address spaces": each of the four screens is
// graphic (MWR CHR = 0, a 20-bit address into the 1 M-word frame memory) or character
// (MWR CHR = 1, a 64 K-word space, with the CHR pin high so the board can decode a
// separate memory). Which memory answers a CHR access is the BOARD's wiring, so a board
// that decodes CHR attaches its memory with setCharSpace(); a drawing access to a CHR
// screen then reaches it at the low 16 bits of the address. A drawing access belongs to
// the screen its pointer names: ORG's DN for the pixel commands, RWP's DN for the word
// commands. With nothing attached, a CHR access goes to the frame memory as a graphic one
// would -- a board that does not decode CHR.
//
// ---------------------------------------------------------------------------
// DRAWING TIME (timed mode; datasheet Table 3, manual 2.2 and OMR ACP/RAM/ACM)
//
// The chip knows time only as a count of 2CLK cycles, which the BOARD supplies (advance):
// the board has the oscillator and knows how 2CLK relates to the CPU's T-states. The chip
// never learns what a second is.
//
// COST. Table 3 gives each command in 2CLK cycles, from its parameters and from what it
// drew: L and d are the logical pixel positions the command visited (counted in drawPixel
// -- COL-suppressed and AREA-clipped positions included, as they step the pattern too), B
// the rows of a plane command, x and y the words of a block command. P = 4 for OPM
// 000-011, 6 for 100-111. PAINT's formula is exact only for a rectangle (Table 3 note 2);
// it is applied with A x B = the pixels painted and B = the spans. The program-controlled
// transfers are charged word by word as the words move: 4 per word, 8 per row, 16 (DRD:
// 12) per started group of 8 words, and the table's constant when the command starts.
//
// WHERE IT IS PAID. A cost of N 2CLK needs ceil(N/2) drawing SLOTS; a slot is one memory
// cycle (two 2CLK). A raster is HC+1 slots from HSYNC falling, a frame VC rasters from
// VSYNC falling, and a slot is free for drawing by these rules:
//   - the last slot of HSYNC low (the attribute output cycle): never;
//   - the other HSYNC-low slots (the DRAM refresh period): only with OMR RAM = 1;
//   - the display period -- the background's HDS..HDS+HDW on a lit raster, the window's
//     HWS..HWS+HWW on a window raster: none in single access with ACP = 0 (display
//     priority), all with ACP = 1, and every second slot (the drawing phase) in the dual
//     access modes;
//   - every other slot of the retrace: always.
// A chip that is not started (OMR STR = 0), or whose HC/VC describe no frame, has every
// slot free. The frame's phase is simply 2CLK time / 2 modulo the frame, from power-on.
// ASSUMPTIONS the manual does not settle: Table 3's counts are what the drawing processor
// takes when it gets every slot it asks for; in the dual modes the retrace slots are all
// free, as in single access; a command's end is fixed when it starts, so a timing
// register written while it runs moves the NEXT command, not this one.
// ---------------------------------------------------------------------------

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace altair {

class StateWriter;  // core/statefile.h -- SNAPSHOT/RESTORE
class StateReader;

class Hd63484 {
public:
    // The board's character memory (CHARACTER ADDRESS SPACE above): one 16-bit word at a
    // 16-bit address, read and written by the drawing processor with CHR high.
    class CharSpace {
    public:
        virtual ~CharSpace() = default;
        virtual uint16_t charRead(uint16_t addr) = 0;
        virtual void     charWrite(uint16_t addr, uint16_t v) = 0;
    };

    // `vramWords` is how much frame memory the BOARD fitted, in 16-bit words. The chip
    // emits a 20-bit word address; a smaller memory aliases (the DRAM does not decode
    // the high bits), which is what the silicon on a real board does too.
    explicit Hd63484(size_t vramWords = 1u << 20);

    // ---- THE TWO HOST LOCATIONS, 8-bit MPU mode. rs is the RS pin. ----
    uint8_t read(bool rs);
    void    write(bool rs, uint8_t v);

    // RES*: SR = $23, CCR ABT = 1 and every other CCR bit 0, OMR M/S and STR = 0, both
    // FIFOs cleared. Every other register keeps its value (manual 5.5, 5.6; App Note
    // Table 7-1) -- "after power-up neither the registers nor the RAM hold defined
    // values", so power() below is what zeroes them.
    void reset();
    // Power-on: everything zero, then reset(). The frame memory comes up zero.
    void power();

    // Wire the CHR pin to a memory of the board's (nullptr = not decoded). Not state: the
    // board attaches it once, and a snapshot carries the memory with the board.
    void setCharSpace(CharSpace* cs) { charSpace_ = cs; }

    // ---- DRAWING TIME (see DRAWING TIME above) ----

    // Timed or instantaneous drawing -- the board's strap. Turning it off finishes the
    // command in flight at once, and everything queued behind it.
    void setTimed(bool on);
    bool timed() const { return timed_; }

    // The time is `now2clk` 2CLK cycles since power-on: finish every command whose end has
    // come -- each at its own end, so the next starts there, not at `now2clk` -- and start
    // what the write FIFO holds behind it. Time never runs backwards; an earlier value is
    // ignored. A no-op untimed.
    void advance(uint64_t now2clk);

    // Is a command still being paid for, and until when (2CLK)? The board arms its
    // deadline on it, so an interrupt on CED or WFE lands on time with no bus cycle.
    bool     busy() const { return busy_; }
    uint64_t busyUntil() const { return busyUntil_; }

    // Table 3: the 2CLK cost of command `op` with parameters `params`, having visited
    // `dots` pixel positions in `rows` rows. Public and static so a test can check the
    // table itself. Transfers (DRD/DWT/DMOD) return only their constant.
    static uint64_t opCycles(uint16_t op, const std::vector<uint16_t>& params, uint64_t dots, uint64_t rows);

    // ---- THE SCREEN, for the board's render loop ----

    // Is the chip displaying? OMR STR (start) and DCR SE1 (the base screen, which "must
    // always be defined"). Off, the board paints black.
    bool displayOn() const;

    // The visible raster in pixels: (HDW+1) memory cycles wide, each cycle one word (or
    // 2/4/8 with GAI) at the current bits per pixel; SP0+SP1+SP2 rasters tall for the
    // screens DCR enables. Zero if the timing registers describe nothing.
    int displayWidth() const;
    int displayHeight() const;
    int bitsPerPixel() const;  // 1, 2, 4, 8 or 16 from CCR GBM

    // Visible raster `y` (0 = top) as pixel values, LOW dot address first, at most
    // out.size() of them; pixels past displayWidth() are left untouched. A pixel is the
    // bpp-bit field, so at 16 bpp it is the whole word. THIS IS THE CHIP'S OWN VIEW --
    // pixels unpacked at GBM, one fetch per memory cycle -- and it is what a test of the
    // chip reads. A real board's shift register decides the pixel width and the words per
    // fetch for itself; CADzilla uses the address-level API below and ignores GBM.
    void scanline(int y, std::span<uint16_t> out) const;

    // ---- SCAN-OUT AT THE ADDRESS LEVEL: what the MAD bus carries (manual 5.6-5.9) ----
    //
    // The ACRTC does not serialize pixels. Once per display cycle it puts a frame-memory
    // word address on MAD, external logic fetches one or more words there and shifts them
    // out, and the ACRTC advances the address by GAI words for the next cycle. A board
    // that models its own shift register asks for the addresses and does the rest.

    // The start address of background raster `raster` (0 = the first raster of the
    // upper/base/lower stack, in display order). False if no enabled screen owns it or
    // the screen that does is blanked (SE bits): the raster shows black.
    bool backgroundRaster(int raster, uint32_t& startAddr) const;

    // The start address of the window's raster on VSYNC-relative raster `vsyncRaster`
    // (0 = the raster after VSYNC's rising edge -- the same reference VDS and VWS use).
    // False if the window is not displayed (DCR SE3 != 11) or the raster is outside
    // VWS+1 .. VWS+VWW.
    bool windowRaster(int vsyncRaster, uint32_t& startAddr) const;

    // Words the display address advances per display cycle: OMR GAI 000-011 -> 1, 2, 4, 8;
    // the no-increment modes -> 0; the increment-every-two-cycles mode -> 1 (approximate).
    int gaiWords() const;

    // Memory cycles per display cycle: 1 in single access mode, 2 in interleaved and
    // superimposed (OMR ACM bit 3 -- "the frame buffer is accessed twice every display
    // cycle"; manual 5.6).
    int accessMode() const;

    // The timing registers, decoded to the manual's units (memory cycles / rasters, the
    // "+1" applied where the manual says "set to N-1").
    int hds() const { return ((reg(0x84) >> 8) & 0xFF) + 1; }   // display start, from HSYNC rise
    int hdw() const { return (reg(0x84) & 0xFF) + 1; }          // display width
    int hws() const { return ((reg(0x92) >> 8) & 0xFF) + 1; }   // window start
    int hww() const { return (reg(0x92) & 0xFF) + 1; }          // window width
    int vds() const { return ((reg(0x88) >> 8) & 0xFF) + 1; }   // display start, from VSYNC rise
    int vws() const { return (reg(0x94) & 0x0FFF) + 1; }        // window start
    int vww() const { return reg(0x96) & 0x0FFF; }              // window width

    // Has anything the picture depends on changed since last asked? Consuming.
    bool takeDirty() {
        bool d = dirty_;
        dirty_ = false;
        return d;
    }

    // IRQ* as the chip would drive it: any SR flag whose CCR enable bit is set. The
    // board may wire it to a bus interrupt; nothing here does.
    bool irq() const { return (sr_ & (ccr() & 0xFF)) != 0; }

    // ---- For tests: registers, memory and the drawing state, straight ----
    uint8_t  status() const { return sr_; }
    uint8_t  ar() const { return ar_; }
    uint16_t reg(uint8_t even) const;      // control register `even` (r02, r84, ...)
    uint16_t ccr() const { return reg(0x02); }
    uint16_t omr() const { return reg(0x04); }
    uint16_t dcr() const { return reg(0x06); }
    uint16_t param(uint8_t rn) const;      // drawing parameter register Pr00..Pr13
    uint16_t pattern(uint8_t pra) const { return pram_[pra & 0x0F]; }
    int16_t  cpx() const { return cpx_; }
    int16_t  cpy() const { return cpy_; }
    std::span<const uint16_t> vram() const { return vram_; }
    uint16_t peekWord(uint32_t addr) const { return vram_[addr & vmask_]; }
    void     pokeWord(uint32_t addr, uint16_t v) { vram_[addr & vmask_] = v; dirty_ = true; }
    int      writeFifoWords() const { return wfifoN_ / 2; }
    int      readFifoWords() const { return rfifoN_ / 2; }

    // SNAPSHOT/RESTORE (DESIGN.md 13): registers, FIFOs, the command in flight, the
    // drawing state, the pattern RAM and the whole frame memory (nothing else holds it).
    void serialize(StateWriter& w) const;
    void deserialize(StateReader& r);

private:
    // ---- register file ----
    uint16_t regWord(uint8_t even) const { return (uint16_t)((reg_[even & 0xFE] << 8) | reg_[(even & 0xFE) | 1]); }
    void     regByteWritten(uint8_t addr);   // side effects of a control-register byte write
    uint8_t  regByteRead(uint8_t addr) const;

    // ---- FIFOs (bytes; a word is two, high then low) ----
    void pushWrite(uint8_t b);
    bool popWrite(uint8_t& b);
    void pushRead(uint8_t b);
    bool popRead(uint8_t& b);
    void updateFifoStatus();
    void abort();

    // Words a command owes the read FIFO that did not fit (RD, RPR, RPTN): the chip "enters
    // a wait state until space becomes available" (manual RD-1), and so does the command
    // stream behind it. feedRead() moves them in as the host drains.
    void queueRead(uint16_t w);
    void feedRead();
    bool readStalled() const { return !rpending_.empty() || xferRead_; }

    // ---- the command engine ----
    void processFifo();                      // consume words from the write FIFO
    void commandWord(uint16_t w);            // one word of command or parameter
    void run();                              // execute(), paid for when timed
    int  paramsFor(uint16_t opcode) const;   // how many parameter words (-1 = undefined)
    void execute();                          // all parameters are in
    void commandEnd() { sr_ |= kCED; }
    void commandError() { sr_ |= kCER; }

    // ---- drawing (manual 6.6-6.8) ----
    void     drawLine(int x0, int y0, int x1, int y1);          // excludes (x1, y1)
    void     drawPixel(int x, int y);                           // through COL/OPM/AREA, at the pattern pointer
    bool     areaAllows(int x, int y);                          // false = suppressed/stopped
    bool     stopped_ = false;                                  // AREA 001/101 fired...
    int16_t  stopX_ = 0, stopY_ = 0;                            // ...at this point, where CP ends
    void     wordAddress(int x, int y, uint32_t& addr, int& shift) const;
    uint16_t colorFor(bool& draw) const;
    uint16_t applyOpm(uint16_t data, uint16_t color, uint32_t addr, int shift, int bpp) const;
    void     fillRect(int x1, int y1);                          // AFRCT/RFRCT body
    void     traceConic(int xc, int yc, int64_t a, int64_t b,   // CRCL/ELPS/arcs body
                        int x0, int y0, int xe, int ye, bool cw);
    void     paint(bool e);                                     // PAINT body
    void     drawPattern(int szx, int szy, int sd, bool sl);    // PTN body
    void     graphicCopy(int xs, int ys, int dx, int dy, bool s, int dsd);   // AGCPY/RGCPY body
    void     patternAt(int dx, int dy, uint8_t ppx0, uint8_t pzcx0, uint8_t ppy0, uint8_t pzcy0);
    void     clearBlock(uint16_t d, int16_t ax, int16_t ay, bool masked, int mm);
    void     copyBlock(uint32_t src, int16_t ax, int16_t ay, bool s, int dsd, bool masked, int mm);
    uint16_t modify(uint16_t data, uint16_t d, int mm) const;   // MM under MASK

    // THE PATTERN POINTER (manual 5.10.2.6, 6.8.3-6.8.4) is live state: PPX/PPY name the
    // pattern-RAM bit in use, PZCX/PZCY count the zoom repeats of it. Every logical pixel
    // position a drawing visits steps X once -- drawn, suppressed by COL, or clipped by
    // AREA alike -- and a plane command steps Y once per row. What is left is what RPR Pr05
    // reads and where the next command's pattern picks up ("pattern continuity", 5.10.1).
    void     stepPatternX();
    void     stepPatternY();

    // DRD/DWT/DMOD: the word the transfer cursor is on (signed AX/AY walk, like CLR).
    uint32_t xferAddr() const {
        return xferBase_ + (uint32_t)(xferX_ * xferSx_) - (uint32_t)(xferY_ * xferSy_) * mw(rwpDn_);
    }
    void     xferAdvance();

    // RWP / DP as 20-bit word addresses plus the screen number they carry.
    uint32_t rwp() const { return rwp_; }
    void     setRwp(uint32_t a) { rwp_ = a & 0xFFFFF; }
    uint32_t mw(int dn) const { return regWord((uint8_t)(0xC2 + dn * 8)) & 0x0FFF; }
    uint32_t sar(int dn) const;

    // ---- drawing time (timed mode) ----
    // The engine is busy for `cycles` 2CLK from now_: the SR flags raised since `before`
    // are held back until the end, when finish() raises them.
    void     occupy(uint64_t cycles, uint8_t before);
    void     finish();                                  // the command's end has come
    uint64_t drawEnd(uint64_t start, uint64_t cycles);  // when `cycles` of drawing started at `start` end
    void     buildTimeline();                           // slotFree_/rasterFree_ from the registers
    uint64_t xferCost() const;                          // the transfer word about to move

    // A drawing access to word `addr` of screen `dn`: the frame memory, or the board's
    // character memory when that screen's MWR CHR bit is set and the board decodes CHR.
    //
    // KEEP THIS SHAPE. Written as one `charSpace_ && (regWord(..) & 0x8000)` expression, MSVC
    // 14.51 (Visual Studio 18) compiles the function to `xor al, al; ret` -- always false -- so a
    // CHR screen drew into the frame memory on Windows alone. The early return and the separate
    // bit test compile correctly there; the CHR tests in test_hd63484 and test_cadzilla guard it.
    bool     charScreen(int dn) const {
        if (charSpace_ == nullptr) return false;
        const uint16_t mwr = regWord((uint8_t)(0xC2 + dn * 8));
        return (mwr & 0x8000) != 0;
    }
    void     vramWrite(int dn, uint32_t addr, uint16_t v) {
        if (charScreen(dn)) {
            charSpace_->charWrite((uint16_t)addr, v);
            return;
        }
        vram_[addr & vmask_] = v;
        dirty_ = true;
    }
    uint16_t vramRead(int dn, uint32_t addr) const {
        return charScreen(dn) ? charSpace_->charRead((uint16_t)addr) : vram_[addr & vmask_];
    }

    // Status bits.
    static constexpr uint8_t kCER = 0x80, kARD = 0x40, kCED = 0x20, kLPD = 0x10;
    static constexpr uint8_t kRFF = 0x08, kRFR = 0x04, kWFR = 0x02, kWFE = 0x01;
    static constexpr int     kFifoBytes = 16;  // 8 words

    // ---- state ----
    uint8_t  reg_[256] = {};        // the control registers, byte-addressed, big-endian pairs
    uint8_t  ar_ = 0;
    uint8_t  sr_ = 0;

    uint8_t  wfifo_[kFifoBytes] = {};
    int      wfifoN_ = 0;
    uint8_t  rfifo_[kFifoBytes] = {};
    int      rfifoN_ = 0;
    uint8_t  fifoHi_ = 0;           // the high byte of a word in flight (8-bit pairing)
    bool     fifoHalf_ = false;     // true = a high byte has been written, low is next

    // The command in flight: its opcode, its parameters so far, how many it wants.
    bool     inCommand_ = false;
    uint16_t cmd_ = 0;
    std::vector<uint16_t> params_;
    int      paramsWanted_ = 0;
    // DWT/DMOD/DRD under program control: words still to move after the parameters.
    int      xferLeft_ = 0;
    int      xferAx_ = 0, xferAy_ = 0, xferX_ = 0, xferY_ = 0;
    int      xferSx_ = 1, xferSy_ = 1;  // the signs of AX and AY: which way the block runs
    uint32_t xferBase_ = 0;
    bool     xferRead_ = false;
    std::vector<uint16_t> rpending_;  // read-FIFO words waiting for space (queueRead)

    // Drawing parameter registers (Pr00..Pr13) and the pattern RAM.
    uint16_t cl0_ = 0, cl1_ = 0, ccmp_ = 0, edg_ = 0, mask_ = 0xFFFF;
    uint8_t  ppx_ = 0, ppy_ = 0, pzcx_ = 0, pzcy_ = 0;
    uint8_t  psx_ = 0, psy_ = 0, pex_ = 0, pey_ = 0, pzx_ = 0, pzy_ = 0;
    int16_t  xmin_ = 0, ymin_ = 0, xmax_ = 0, ymax_ = 0;
    uint32_t rwp_ = 0;              // ONE register; DN rides in its high word (manual 5.10.2.8)
    uint8_t  rwpDn_ = 0;
    uint32_t orgDpa_ = 0;           // ORG: the origin's word address...
    uint8_t  orgDpd_ = 0;           // ...and dot address
    uint8_t  orgDn_ = 0;            // ...on which screen
    int16_t  cpx_ = 0, cpy_ = 0;    // the current pointer
    uint16_t pram_[16] = {};

    std::vector<uint16_t> vram_;
    uint32_t vmask_;
    bool     dirty_ = true;
    CharSpace* charSpace_ = nullptr;  // the board's CHR decode, or none

    // Drawing time. timed_ is the board's strap and does not travel in a snapshot.
    bool     timed_ = false;
    uint64_t now_ = 0;              // 2CLK since power-on, as last told (advance)
    bool     busy_ = false;         // a command is being paid for...
    uint64_t busyUntil_ = 0;        // ...until this 2CLK
    uint8_t  pendingSr_ = 0;        // SR flags it raised, shown when it ends
    uint64_t dots_ = 0, rows_ = 0;  // what the command in execute() has visited
    // The frame's free slots: one flag per slot, and free slots before each raster (the
    // last entry is the frame's total). Rebuilt lazily when a timing register moves.
    std::vector<uint8_t>  slotFree_;
    std::vector<uint32_t> rasterFree_;
    int      slotsPerRaster_ = 0;
    bool     timelineAll_ = true;   // every slot free: stopped, or no frame described
    bool     timelineStale_ = true;
};

} // namespace altair
