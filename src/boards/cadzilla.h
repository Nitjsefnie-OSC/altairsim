#pragma once
//
// CADzilla -- an HD63484 ACRTC graphics board with a Bt453 RAMDAC. A new custom product
// based on period components (2026); see docs/boards/cadzilla.md.
//
// TWO CHIPS ON SIX I/O PORTS, AND A FRAME MEMORY THE CPU NEVER SEES.
//
//   I/O     -- ONE 8-port block from BASE (default 0x70; BASE must be a multiple of 8):
//                BASE+0  ACRTC RS=0 (OUT: address register, IN: status)
//                BASE+1  MODE register (write-only) -- the board's own glue-logic strap,
//                        not a chip register; see MODE REGISTER below
//                BASE+2  ACRTC RS=1 (the 16-bit register the address names, one byte per
//                        cycle, or the command FIFOs)
//                BASE+3  the high byte of a 16-bit Command/FIFO transfer on the real
//                        board (SW1-7) -- an 8080/Z80 never makes one, so nothing answers
//                BASE+4..+7  Bt453 by C1C0 -- address register, color palette RAM, address
//                        register again, overlay registers -- all read/write
//              The ACRTC's own RS=0/RS=1 pair is therefore NOT adjacent on this board --
//              MODE sits between them. That is a real decode choice (SW1 selects the
//              8-port BASE; within the block, address bit A1 is the ACRTC's RS pin and
//              A0 picks MODE vs. the ACRTC at A1=0), not an accident.
//   MEMORY  -- NONE decoded. The frame memory is 2 MB of SRAM on the board, private to the
//              ACRTC and fixed (no refresh cycles, no `vram` strap -- the reference design
//              is exactly this much and no other); the CPU draws by issuing commands
//              through the FIFO, never by poking a byte. The opposite reason from the
//              Dazzler's for decoding no address.
//   DISPLAY -- once per pump() the board builds the MONITOR'S frame -- a fixed VESA raster
//              chosen by the `mode` strap, 1024x768 by default -- by running its own shift
//              registers over the addresses the ACRTC puts out, and has the Bt453 turn each
//              pixel into the color the host shows. Never touches SDL: the Display is
//              injected at the composition root (setDisplay), a headless build gets a
//              NullDisplay, and the board runs and is tested with no window (DESIGN.md 7.4).
//
// THE PIPELINE IS THE POINT. Frame memory -> P7..0, and the overlay SRAM -> OL1..0, both
// into the Bt453, which drives 24-bit RGB. The board keeps the RAMDAC's input bus as it is
// (busPixel: P, OL and BLANK for every pixel of the frame) and resolves it through the chip
// (Bt453::lookup, Table 3) into an Rgb32 Surface. The palette RAM and the overlay registers
// stay two separate things, as they are in the part: the host is handed colors, not a
// look-up table it would have to know the RAMDAC's rules for.
//
// ---------------------------------------------------------------------------
// WHAT THE BOARD DECIDED (a chip cannot), and every one of them is visible from the bus:
//
//   THE SHIFT REGISTER IS WIRED FOR 8 BITS PER PIXEL AND 8 WORDS PER FETCH. The frame
//   memory is EIGHT banks of SRAM side by side: drawing word W is bank W & 7, row W >> 3.
//   Each display cycle the ACRTC puts one address A on MAD; the board ignores MA2..0 and
//   reads row A >> 3 of all eight banks at once -- words (A & ~7) + 0..7, bank 0 first, so
//   a start address that is not a multiple of 8 has its low three bits ignored -- and
//   shifts them out as SIXTEEN 8-bit pixels, low byte of the low word first (so the
//   ACRTC's +X is rightward -- chips/hd63484.h). That is a hardware fact,
//   not a register: a program must set CCR GBM = 011 (8 bpp) so the drawing engine's
//   pixel arithmetic agrees with the wire, and OMR GAI = 011 (+8 words) so the ACRTC's
//   address steps over exactly the words the board fetched. A program that sets either
//   differently gets what the hardware would give -- a scrambled picture, not an error --
//   and SHOW's `wiring` line says which register is off.
//
//   THE MONITOR IS A FIXED-FREQUENCY VESA DISPLAY, chosen by `mode`: the three primary VESA
//   resolutions -- 640x480, 800x600 or 1024x768 (default), settable in the machine file or
//   with SET. The frame the window shows and the tests capture is
//   ALWAYS the mode's size. The ACRTC's programmed display area is placed in it exactly as
//   a monitor would place it: its horizontal display start (HDS) is counted in memory
//   cycles from HSYNC's rising edge and the mode's back porch is a fixed number of those,
//   so a picture programmed to start where the porch ends fills the frame from the left
//   edge, and one that starts earlier or later is shifted and clipped. Vertically the same
//   with VDS and the vertical back porch. The register values for each mode are in
//   docs/boards/cadzilla.md.
//
//   ACCESS MODES: single (ACM = 0x) and interleaved (ACM = 10). In interleaved mode the
//   ACRTC takes two memory cycles per display cycle, so every horizontal register value
//   (HC, HSW, HDS, HDW, HWS, HWW) is doubled for the same picture and each memory cycle is
//   worth 8 pixels of the frame instead of 16. Superimposed mode (ACM = 11) is not wired:
//   the window's second phase would need its own fetch path.
//
//   THE OVERLAY IS A ONE-BIT PLANE IN ITS OWN SRAM: 128 K words (U19), one bit per frame
//   pixel, shifted out beside the frame memory into the Bt453's OL1. OL0 is tied low, so a
//   set bit shows OVERLAY COLOR 2 and a clear bit the palette; overlay colors 1 and 3 are
//   never selected. It has no display cycles of its own: every display fetch at A also
//   reads overlay word A >> 3, the bits of exactly those 16 pixels, bit 0 leftmost. So
//   frame pixel p (p = 2 x word address + byte) is overlay word p >> 4, bit p & 15.
//   MODE OLEN gates the display only -- off, the overlay is transparent.
//   The ACRTC DRAWS the overlay through a screen whose MWR CHR bit is set: the board
//   decodes the chip's CHR pin, enables no frame-memory bank, and sends the access to U19
//   at MAD0-15, with MODE OLSEL as the seventeenth address bit (a character screen has only
//   64 K words, and its MA16-19 carry a raster address). So overlay word O is drawing
//   address O & FFFF with OLSEL = O >> 16. A CHR screen must never be DISPLAY-enabled: the
//   board ignores CHR on display cycles and would show frame memory there -- `wiring`
//   says so. The board decides nothing more than that: which screen, and the GBM a program
//   draws the overlay with, are the program's.
//
//   Also: IRQ* wired to the `interrupt` strap (SW1-8 on the board enables it; off, the
//   default, disconnects it -- see below).
//
//   2CLK IS THE BOARD'S. The ACRTC's clock comes from the monitor's pixel clock (PCLK, the
//   VESA rate of `mode`): PCLK/8 in single access mode, PCLK/4 in interleaved -- selected
//   by MODE AMODE, the board's glue, not the chip's OMR ACM. A memory cycle is then 16
//   pixels single and 8 interleaved, which is why the horizontal registers double.
//
// DRAWING TIME -- the `draw_rate` strap. `full` (the default): the ACRTC draws in no time --
// the fastest bench for developing a program. `real`: each command
// costs its datasheet Table 3 time in 2CLK (chips/hd63484.h, DRAWING TIME), so the write
// FIFO backs up and CED comes late exactly as a timing-sensitive program -- a game -- would
// see on the board. The board turns T-states into 2CLK (sync) and arms one Clock deadline
// at the command's end (arm), so an interrupt on CED lands on time with the CPU halted.
// It is not the CPU's `clock_hz`: that only decides whether the host waits, and the guest
// cannot see it; the drawing time is emulated time, the same number of instructions long
// at any `clock_hz`.
//
// THE MODE REGISTER (BASE+1, write-only) is the board's own glue logic, not a register on
// either chip -- the host programs it in tandem with the ACRTC's own OMR when it sets up
// the picture, and it is what the board's OWN fetch logic (programmedWidth/X, paintFrame)
// reads for single vs. interleaved access, not the ACRTC's OMR bit:
//
//   bit 0  HSPOL  horizontal sync polarity: 0 = positive sync pulse, 1 = negative
//   bit 1  VSPOL  vertical sync polarity:   0 = positive sync pulse, 1 = negative
//   bit 2  AMODE  access mode the board's glue expects: 0 = single, 1 = interleaved --
//                 must agree with the ACRTC's own OMR ACM bit, or `wiring` says so
//   bit 3  OLEN   overlay display enable: 0 = the overlay shift register is never loaded
//                 and OL1 stays low (transparent); drawing into the overlay is unaffected
//   bit 4  OLSEL  the overlay SRAM's A16 on drawing cycles: which 64 K-word half a CHR
//                 screen draws into (1 = the half behind the upper 1 MB of frame memory)
//   bits 5-7      not connected
//
// The positions of bits 0-3 were this implementation's own choice and the board's
// schematic agrees with them; OLSEL is the schematic's. The register is a 74LS273 that bus
// RESET* clears, and it cannot be read back, so a driver keeps its own copy.
//
// HSPOL/VSPOL are recorded and reported (`SHOW`'s hspol/vspol) but drive nothing: this
// model has no separate sync-pulse signal for a polarity to invert. See Limitations.
//
// THE 8-POSITION DIP SWITCH SW1, for reference (docs/boards/cadzilla.md has the full row):
// SW1-1..5 pick the I/O BASE (bits 7-3, i.e. the `port` strap); SW1-6 (16-bit host mode)
// and SW1-7 (16-bit I/O decode) have no expressible effect on an 8080/Z80 S-100 bus, which
// only ever runs 8-bit IN/OUT cycles -- both are fixed Off in this machine, not straps;
// SW1-8 is the `interrupt` strap below: Off (`none`, the default) disconnects IRQ*, On
// means the strap names which S-100 line it reaches (`int` or `vi0`..`vi7`).
// ---------------------------------------------------------------------------

#include "chips/bt453.h"
#include "chips/hd63484.h"
#include "core/board.h"

#include <cstdint>
#include <string>
#include <vector>

namespace altair {

class Display;  // host/display.h -- injected; the board never learns it is SDL

class CadzillaBoard : public Board, private Hd63484::CharSpace {
public:
    CadzillaBoard();
    ~CadzillaBoard() override;  // cancels the drawing deadline (a fired stale alarm is a UAF)

    std::string type() const override { return "cadzilla"; }

    bool    decodes(const BusCycle& c) const override;
    uint8_t read(const BusCycle& c) override;
    void    write(const BusCycle& c) override;

    void reset(Reset r) override;
    void power() override;
    void pump() override;

    // ---- Interrupts: IRQ* strapped to a bus line, or disconnected (SW1-8) ----
    //
    // acrtc_.irq() is already pure and combinational (SR & CCR's enable bits) -- exactly
    // the shape assertsInt()/assertsVi() want. `irq_ == IrqJumper::None` (the default) is
    // SW1-8 off: IRQ* is asked for but nothing is soldered to it, same as the front
    // panel's own unclaimed lines. See docs/devguide/adding-a-board.md's interrupt section
    // for why intChanged() must be called from every place assertsInt()/assertsVi() could
    // move -- here, that is every ACRTC register or FIFO write and read.
    bool    assertsInt() const override { return irq_ == IrqJumper::Int && acrtc_.irq(); }
    uint8_t assertsVi() const override { return acrtc_.irq() ? viBit(irq_) : 0; }

    // SNAPSHOT/RESTORE (DESIGN.md 13): both chips, frame memory and overlay SRAM included --
    // no memory board holds them, so they must travel here.
    void serialize(StateWriter& w) const override;
    void deserialize(StateReader& r) override;

    std::vector<Property> properties() override;
    std::vector<MapEntry> ioMap() const override;

    // The host video service, wired once in main.cpp / tests/main.cpp (like
    // DazzlerBoard::setDisplay). Borrowed; the composition root owns it.
    static void setDisplay(Display* d);

    // ---- The monitor: one VESA mode, in the board's own memory-cycle units ----
    //
    // Horizontal figures are memory cycles of 16 pixels (single access mode); vertical
    // are rasters. Where the VESA pixel counts are not multiples of 16 the board rounds
    // its porches to whole cycles and keeps the total, as a timing PROM on the board
    // would; the pixel clocks are the VESA ones.
    struct Mode {
        const char* name;
        int width, height;         // the active picture, in pixels
        int hsw, hbp, hfp;         // sync, back porch, front porch: memory cycles
        int vsw, vbp, vfp;         // sync, back porch, front porch: rasters
        long long pclk;            // the pixel clock, Hz (2CLK is PCLK/8 or PCLK/4)
        int hc() const { return hsw + hbp + width / 16 + hfp; }   // total cycles per line
        int vc() const { return vsw + vbp + height + vfp; }       // total rasters per frame
    };
    static const Mode& mode(int i);
    static int         modeCount();
    const Mode&        currentMode() const { return mode(mode_); }

    // The programmed picture as the board sees it: pixels wide (HDW memory cycles at 16 or
    // 8 pixels each by access mode), rasters tall (the enabled SP0/SP1/SP2), and where its
    // top-left corner lands in the monitor's frame (may be negative or beyond the frame).
    int programmedWidth() const;
    int programmedHeight() const;
    int programmedX() const;
    int programmedY() const;

    // What SHOW's `wiring` says: "ok", or which of GBM / GAI / ACM disagrees with the board,
    // or that a CHR screen is displayed.
    std::string wiring() const;

    // The ACRTC's 2CLK in Hz: the mode's pixel clock over 8 (MODE AMODE single) or 4
    // (interleaved).
    long long twoClkHz() const;

    // ---- For tests: the chips themselves, the overlay SRAM and the RAMDAC's input ----
    Hd63484& acrtc() { return acrtc_; }
    Bt453&   dac() { return dac_; }
    uint16_t overlayWord(uint32_t o) const { return ovl_[o & kOvlMask]; }
    // The last frame's RAMDAC input at frame pixel (x, y): P7..0 in bits 7-0, OL1..0 in
    // bits 9-8, and kBusBlank where no display fetch reached (BLANK*: black whatever the
    // look-up table says). 0 off the frame or before the first frame.
    uint16_t busPixel(int x, int y) const;
    static constexpr uint16_t kBusBlank = 0x8000;

private:
    void render();
    void paintFrame(int w, int h);   // the shift registers, into bus_

    // The CHR decode (Hd63484::CharSpace): a drawing access to a CHR screen reaches the
    // overlay SRAM at MAD0-15, with MODE OLSEL as A16.
    uint16_t charRead(uint16_t addr) override;
    void     charWrite(uint16_t addr, uint16_t v) override;
    uint32_t overlayAddr(uint16_t addr) const { return ((modeReg_ & kModeOlsel) ? 0x10000u : 0u) | addr; }

    // Drawing time: bring the ACRTC's 2CLK count up to the Clock, at the rate in force
    // since the last sync; then (re)arm the deadline at the end of whatever it is drawing.
    // Anything that changes the 2CLK rate (MODE AMODE, `mode`) syncs BEFORE it changes it.
    void sync();
    void arm();
    void clockAttached() override;

    // The board's own notion of access mode -- from the MODE register (BASE+1), not the
    // ACRTC's OMR. This is what the shift register (paintFrame, programmedWidth/X) runs on;
    // `wiring()` is what compares it against the chip's own OMR ACM bit.
    int glueAccessMode() const { return (modeReg_ & kModeAmode) ? 2 : 1; }

    Hd63484 acrtc_;
    Bt453   dac_;

    // MODE register bits (BASE+1) -- see the header comment for why these positions.
    static constexpr uint8_t kModeHspol = 0x01;
    static constexpr uint8_t kModeVspol = 0x02;
    static constexpr uint8_t kModeAmode = 0x04;
    static constexpr uint8_t kModeOlen  = 0x08;
    static constexpr uint8_t kModeOlsel = 0x10;

    // 2 MB (1 M sixteen-bit words) -- the reference design's fixed SRAM fit; the ACRTC's
    // own 20-bit address space, exactly. Not a strap: the board has no jumper for it.
    static constexpr size_t kVramWords = 1u << 20;
    // The overlay SRAM: 128 K sixteen-bit words, one bit per frame-memory pixel.
    static constexpr size_t   kOvlWords = 1u << 17;
    static constexpr uint32_t kOvlMask  = kOvlWords - 1;

    // ---- Straps ----
    uint8_t   port_ = 0x70;       // the 8-port block's BASE; a multiple of 8
    int       mode_ = 2;          // index into the VESA mode table: 1024x768
    int       videoWidth_ = 0;    // host window width in px, 0 = auto
    IrqJumper irq_ = IrqJumper::None;   // SW1-8: where IRQ* lands, if anywhere
    bool      drawReal_ = false;  // draw_rate: false = full (instant), true = real (Table 3)

    // ---- Runtime state written by the guest, not a strap ----
    uint8_t modeReg_ = 0;         // MODE register (BASE+1): HSPOL/VSPOL/AMODE/OLEN/OLSEL
    std::vector<uint16_t> ovl_ = std::vector<uint16_t>(kOvlWords);   // the overlay SRAM

    // ---- Render bookkeeping ----
    bool dirty_ = true;           // something in the picture moved since the last frame
    std::vector<uint16_t> bus_;   // the last frame's RAMDAC input, busPixel's layout
    int  busW_ = 0, busH_ = 0;

    // ---- Drawing time: the ACRTC's 2CLK count, kept in step with the Clock ----
    uint64_t      lastT_ = 0;     // the Clock's T-state at the last sync
    uint64_t      acc2clk_ = 0;   // 2CLK cycles since power-on
    uint64_t      rem_ = 0;       // the fraction carried: (T-states x 2CLK Hz) mod CPU Hz
    Clock::Handle wake_ = Clock::kNone;   // the end of the command being drawn
};

} // namespace altair
