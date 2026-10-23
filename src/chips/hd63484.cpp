#include "chips/hd63484.h"

#include "core/statefile.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <utility>

namespace altair {
namespace {

// Floor division and modulo for the signed pixel -> word arithmetic (a pixel left of
// the origin's word is in the word BELOW it, not truncated toward it).
int floorDiv(int a, int b) {
    int q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) --q;
    return q;
}
int floorMod(int a, int b) { return a - floorDiv(a, b) * b; }

// THE COPY SCANS (manual CPY-3/4, Tables C14-1 and C14-2), shared by CPY/SCPY in words
// and AGCPY/RGCPY in pixels. A scan is a FAST axis (the order within a line) and a SLOW
// axis (the order of the lines), each a unit step in logical X and Y, +Y up.
struct Scan {
    int fx = 1, fy = 0;    // one step along a line
    int sx = 0, sy = 1;    // one line to the next
};

// The source (S, bit 11): S = 0 scans rows, S = 1 scans columns; the signs of the extents
// decide which corner it starts from ("decided by the relation between bit 11 ... and the
// Pss and the Pse").
Scan sourceScan(bool s, int dx, int dy) {
    const int ux = dx < 0 ? -1 : 1, uy = dy < 0 ? -1 : 1;
    return s ? Scan{0, uy, ux, 0} : Scan{ux, 0, 0, uy};
}

// The destination (DSD, bits 10-8), read off Table C14-2: bit 2 scans columns instead of
// rows, bit 1 runs X leftward, bit 0 runs Y downward.
Scan destScan(int dsd) {
    const int ux = (dsd & 2) ? -1 : 1, uy = (dsd & 1) ? -1 : 1;
    return (dsd & 4) ? Scan{0, uy, ux, 0} : Scan{ux, 0, 0, uy};
}

// The curve commands' parameters are 16 bits "but only the low order 13 bits are
// effective" (manual CRCL-1, AARC-2): a coordinate is 13-bit two's complement, a size
// (r, a, b, dX) 13 bits unsigned.
int sext13(uint16_t v) { return (int)((v & 0x1FFF) ^ 0x1000) - 0x1000; }
int mag13(uint16_t v) { return v & 0x1FFF; }

// log2 of a bits-per-pixel value 1..16.
int log2bpp(int bpp) {
    int n = 0;
    while ((1 << n) < bpp) ++n;
    return n;
}

} // namespace

Hd63484::Hd63484(size_t vramWords) {
    // Power-of-two only: the address is masked, as a DRAM array that decodes only its own
    // address lines does.
    size_t n = 1;
    while (n < vramWords) n <<= 1;
    if (n > (1u << 20)) n = 1u << 20;
    vram_.assign(n, 0);
    vmask_ = (uint32_t)(n - 1);
    power();
}

// ---------------------------------------------------------------------------
// Reset and power (manual 5.3, 5.5, 5.6; App Note Table 7-1).
// ---------------------------------------------------------------------------
void Hd63484::reset() {
    abort();                                   // SR = $23, FIFOs cleared
    reg_[0x02] = 0x80;                         // CCR: ABT = 1, everything else 0
    reg_[0x03] = 0x00;
    reg_[0x04] &= 0x3F;                        // OMR: M/S = 0, STR = 0; the rest keeps
    inCommand_ = false;
    params_.clear();
    xferLeft_  = 0;
    xferRead_  = false;
    rpending_.clear();
    stopped_   = false;
    fifoHalf_  = false;
    dirty_     = true;
}

void Hd63484::power() {
    for (auto& b : reg_) b = 0;
    for (auto& w : vram_) w = 0;
    for (auto& w : pram_) w = 0;
    ar_ = 0;
    cl0_ = cl1_ = ccmp_ = edg_ = 0;
    mask_ = 0xFFFF;
    ppx_ = ppy_ = pzcx_ = pzcy_ = psx_ = psy_ = pex_ = pey_ = pzx_ = pzy_ = 0;
    xmin_ = ymin_ = xmax_ = ymax_ = 0;
    rwp_   = 0;
    rwpDn_ = 0;
    orgDpa_ = 0;
    orgDpd_ = 0;
    orgDn_  = 0;
    cpx_ = cpy_ = 0;
    now_ = 0;
    timelineStale_ = true;
    reset();
}

// ABT (CCR bit 15): "command execution is aborted and the read/write FIFOs are cleared.
// The status register is set to $23."
void Hd63484::abort() {
    wfifoN_ = 0;
    rfifoN_ = 0;
    fifoHalf_ = false;
    inCommand_ = false;
    params_.clear();
    xferLeft_ = 0;
    xferRead_ = false;
    rpending_.clear();
    stopped_  = false;
    busy_      = false;               // the command being paid for is gone with the rest
    pendingSr_ = 0;
    sr_ = kCED | kWFR | kWFE;
}

// ---------------------------------------------------------------------------
// The two host locations.
// ---------------------------------------------------------------------------
uint8_t Hd63484::read(bool rs) {
    if (!rs) return sr_;                       // RS=0 read: the status register

    if (ar_ < 2) {                             // the read FIFO, high byte then low
        uint8_t b = 0xFF;
        popRead(b);
        return b;
    }
    uint8_t v = regByteRead(ar_);
    if (ar_ & 0x80) ar_ = (uint8_t)(ar_ + 1);  // r80-rFF auto-increment (manual 5.2)
    return v;
}

void Hd63484::write(bool rs, uint8_t v) {
    if (!rs) {                                 // RS=0 write: the address register
        ar_ = v;
        return;
    }
    if (ar_ < 2) {                             // the write FIFO: bytes pair high, low
        if (!fifoHalf_) {
            fifoHi_   = v;
            fifoHalf_ = true;
            return;
        }
        fifoHalf_ = false;
        pushWrite(fifoHi_);
        pushWrite(v);
        processFifo();
        return;
    }
    reg_[ar_] = v;
    regByteWritten(ar_);
    if (ar_ & 0x80) ar_ = (uint8_t)(ar_ + 1);
}

uint16_t Hd63484::reg(uint8_t even) const { return regWord(even); }

uint8_t Hd63484::regByteRead(uint8_t addr) const {
    // The raster counter (r80) is not modeled: a guest that polls it sees raster 0.
    if ((addr & 0xFE) == 0x80) return 0;
    return reg_[addr];
}

void Hd63484::regByteWritten(uint8_t addr) {
    switch (addr) {
    case 0x02:                                 // CCR high: ABT, PSE, DMA bits, GBM
        if (reg_[0x02] & 0x80) abort();
        dirty_ = true;                         // GBM changes the picture
        return;
    case 0x03:                                 // CCR low: interrupt enables
        return;
    case 0x04: case 0x05:                      // OMR: STR, GAI, ACM
    case 0x06: case 0x07:                      // DCR: screen enables
        dirty_ = true;
        timelineStale_ = true;                 // STR, ACP, RAM, ACM and the screens set drawing time
        return;
    default:
        if (addr >= 0x80) dirty_ = true;       // timing and display-control RAM
        if (addr >= 0x82 && addr <= 0x97) timelineStale_ = true;   // HSR..VWR: the frame's shape
        return;
    }
}

// ---------------------------------------------------------------------------
// The FIFOs (manual 5.3, 5.4): 8 words each, status kept in step.
// ---------------------------------------------------------------------------
void Hd63484::pushWrite(uint8_t b) {
    if (wfifoN_ < 0 || wfifoN_ >= kFifoBytes) return;   // full: the write is lost (a driver polls WFR)
    wfifo_[wfifoN_++] = b;
    updateFifoStatus();
}

bool Hd63484::popWrite(uint8_t& b) {
    if (wfifoN_ == 0) return false;
    b = wfifo_[0];
    for (int i = 1; i < wfifoN_; ++i) wfifo_[i - 1] = wfifo_[i];
    --wfifoN_;
    updateFifoStatus();
    return true;
}

void Hd63484::pushRead(uint8_t b) {
    if (rfifoN_ < 0 || rfifoN_ >= kFifoBytes) return;
    rfifo_[rfifoN_++] = b;
    updateFifoStatus();
}

bool Hd63484::popRead(uint8_t& b) {
    if (rfifoN_ == 0) return false;
    b = rfifo_[0];
    for (int i = 1; i < rfifoN_; ++i) rfifo_[i - 1] = rfifo_[i];
    --rfifoN_;
    updateFifoStatus();
    // A command waiting on read-FIFO space goes on; when it has delivered its last word it
    // ends, and the command stream stalled behind it runs again.
    const bool waiting = !rpending_.empty();
    feedRead();
    if (waiting && rpending_.empty()) commandEnd();
    if (!readStalled()) processFifo();
    return true;
}

void Hd63484::queueRead(uint16_t w) { rpending_.push_back(w); }

// Move owed words into the read FIFO while there is room: first a command's queued
// words, then a DRD's block, fetched as it goes. After a DRD's LAST word the chip "goes
// into an indefinite wait state ... the command should be aborted" (manual 6.5): CED
// stays clear, and the stream stays stalled, until ABT.
void Hd63484::feedRead() {
    if (busy_) return;                         // timed: a command's answer waits for its end
    size_t n = 0;
    while (n < rpending_.size() && rfifoN_ + 2 <= kFifoBytes) {
        pushRead((uint8_t)(rpending_[n] >> 8));
        pushRead((uint8_t)rpending_[n]);
        ++n;
    }
    rpending_.erase(rpending_.begin(), rpending_.begin() + (std::ptrdiff_t)n);
    while (xferRead_ && xferLeft_ > 0 && rfifoN_ + 2 <= kFifoBytes) {
        const uint64_t cost = timed_ ? xferCost() : 0;
        uint16_t w = vramRead(rwpDn_, xferAddr());
        pushRead((uint8_t)(w >> 8));
        pushRead((uint8_t)w);
        xferAdvance();
        if (timed_) {                          // one word per paid stretch: the next comes at its end
            occupy(cost, sr_);
            return;
        }
    }
}

void Hd63484::xferAdvance() {
    if (++xferX_ == xferAx_) {
        xferX_ = 0;
        ++xferY_;
    }
    --xferLeft_;
}

void Hd63484::updateFifoStatus() {
    sr_ = (uint8_t)(sr_ & ~(kWFE | kWFR | kRFR | kRFF));
    if (wfifoN_ == 0) sr_ |= kWFE;
    if (wfifoN_ < kFifoBytes) sr_ |= kWFR;
    if (rfifoN_ > 0) sr_ |= kRFR;
    if (rfifoN_ >= kFifoBytes) sr_ |= kRFF;
}

// ---------------------------------------------------------------------------
// The command engine. UNTIMED, drawing is instantaneous, so the write FIFO drains the
// moment a word lands -- WFE is the steady state and a driver's polling loop never spins.
// TIMED, the engine stops taking words while a command is being paid for (busy_), so the
// words behind it back up in the write FIFO as they do on the chip: WFE clears, and at 8
// words WFR. Either way a command waiting on the read FIFO (readStalled) holds the
// words behind it in the write FIFO until the host reads, exactly as the chip does.
// ---------------------------------------------------------------------------
void Hd63484::processFifo() {
    while (wfifoN_ >= 2 && !readStalled() && !busy_) {
        uint8_t hi = 0, lo = 0;
        popWrite(hi);
        popWrite(lo);
        commandWord((uint16_t)((hi << 8) | lo));
    }
}

// How many parameter words an opcode takes (manual Figure 6.1). -1 is an undefined
// opcode; the variable-length ones (WPTN, the polylines/polygons) are settled once
// their first parameter, n, has arrived (commandWord).
int Hd63484::paramsFor(uint16_t op) const {
    switch (op) {
    case 0x0400: return 2;                     // ORG DPH DPL
    case 0x2400: return 2;                     // DRD AX AY
    case 0x2800: return 2;                     // DWT AX AY
    case 0x4400: return 0;                     // RD
    case 0x4800: return 1;                     // WT D
    case 0x5800: return 3;                     // CLR D AX AY
    case 0x8000: return 2;                     // AMOVE X Y
    case 0x8400: return 2;                     // RMOVE dX dY
    default: break;
    }
    switch (op & ~0x0003) {
    case 0x2C00: return 2;                     // DMOD (MM) AX AY
    case 0x4C00: return 1;                     // MOD (MM) D
    case 0x5C00: return 3;                     // SCLR (MM) D AX AY
    default: break;
    }
    switch (op & ~0x000F) {
    case 0x1800: return 1;                     // WPTN (PRA) n, D1..Dn  -- n settles the rest
    case 0x1C00: return 1;                     // RPTN (PRA) n
    default: break;
    }
    switch (op & ~0x001F) {
    case 0x0800: return 1;                     // WPR (RN) D
    case 0x0C00: return 0;                     // RPR (RN)
    default: break;
    }
    if ((op & ~0x0F00) == 0x6000) return 4;    // CPY  (S DSD) SAH SAL AX AY
    if ((op & ~0x0F03) == 0x7000) return 4;    // SCPY (S DSD MM) SAH SAL AX AY
    switch (op & ~0x00FF) {                    // AREA COL OPM in the low byte
    case 0x8800: case 0x8C00: return 2;        // ALINE / RLINE
    case 0x9000: case 0x9400: return 2;        // ARCT / RRCT
    case 0x9800: case 0x9C00: return 1;        // APLL / RPLL  n, then 2n
    case 0xA000: case 0xA400: return 1;        // APLG / RPLG  n, then 2n
    case 0xC000: case 0xC400: return 2;        // AFRCT / RFRCT
    case 0xCC00: return 0;                     // DOT
    default: break;
    }
    switch (op & ~0x01FF) {                    // C or E in bit 8 as well
    case 0xA800: return 1;                     // CRCL r
    case 0xAC00: return 3;                     // ELPS a b dX
    case 0xB000: case 0xB400: return 4;        // AARC / RARC
    case 0xB800: case 0xBC00: return 6;        // AEARC / REARC
    case 0xC800: return 0;                     // PAINT
    default: break;
    }
    if ((op & ~0x0FFF) == 0xD000) return 1;    // PTN (SL SD) SZ
    switch (op & ~0x0FE7) {                    // COL must be 00: the source IS the color
    case 0xE000: case 0xF000: return 4;        // AGCPY / RGCPY (S DSD) Xs Ys DX DY
    default: break;
    }
    return -1;
}

void Hd63484::commandWord(uint16_t w) {
    // A DWT/DMOD in progress under program control: the words are frame-memory data.
    if (xferLeft_ > 0 && !xferRead_) {
        const uint8_t  before = sr_;
        const uint64_t cost   = timed_ ? xferCost() : 0;
        uint32_t addr = xferAddr();
        int      mm   = cmd_ & 3;
        bool     mod  = (cmd_ & 0xFF00) == 0x2C00;
        vramWrite(rwpDn_, addr, mod ? modify(vramRead(rwpDn_, addr), w, mm) : w);
        xferAdvance();
        if (xferLeft_ == 0) commandEnd();
        if (timed_) occupy(cost, before);
        return;
    }

    if (!inCommand_) {
        cmd_ = w;
        params_.clear();
        sr_ = (uint8_t)(sr_ & ~kCED);          // "CED is cleared by writing a command"
        stopped_ = false;
        paramsWanted_ = paramsFor(w);
        if (paramsWanted_ < 0) {               // undefined: CER, and the chip is free again
            commandError();
            commandEnd();
            return;
        }
        inCommand_ = true;
        if (paramsWanted_ == 0) run();
        return;
    }

    params_.push_back(w);
    if (params_.size() == 1) {
        uint16_t top = (uint16_t)(cmd_ & 0xFF00);
        if ((cmd_ & ~0x000F) == 0x1800)        // WPTN: in 8-bit mode n counts BYTES (manual WPTN-1)
            paramsWanted_ = 1 + (int)(w / 2);
        else if (top == 0x9800 || top == 0x9C00 || top == 0xA000 || top == 0xA400)
            paramsWanted_ = 1 + 2 * (int)w;    // polyline/polygon: n points
    }
    if ((int)params_.size() >= paramsWanted_) run();
}

// Execute the command whose parameters are all in. Timed, it is then paid for: the drawing
// lands in the frame memory now (nothing the guest can read sees it before the end -- the
// commands that could are queued behind this one), and everything the guest CAN see waits
// for the end: the SR flags it raised (CED above all), and any words it answers (busy_ is
// set first, so feedRead() holds them).
void Hd63484::run() {
    if (!timed_) {
        execute();
        return;
    }
    const uint8_t before = sr_;
    dots_ = rows_ = 0;
    busy_ = true;
    execute();
    occupy(opCycles(cmd_, params_, dots_, rows_), before);
}

// ---------------------------------------------------------------------------
// Execution, once every parameter is in.
// ---------------------------------------------------------------------------
void Hd63484::execute() {
    inCommand_ = false;
    const uint16_t op = cmd_;
    const auto     p  = [&](size_t i) -> int16_t { return (int16_t)params_[i]; };
    const auto     u  = [&](size_t i) -> uint16_t { return params_[i]; };
    // A command that answers through the read FIFO ends when its last word is IN it --
    // now, or when the host has made room (popRead).
    const auto     endRead = [&] {
        feedRead();
        if (rpending_.empty()) commandEnd();
    };

    // ---- Register access (manual 6.4) ----
    if (op == 0x0400) {                        // ORG DPH DPL
        orgDn_  = (uint8_t)(u(0) >> 14);
        orgDpa_ = ((uint32_t)(u(0) & 0xFF) << 12) | (uint32_t)(u(1) >> 4);
        orgDpd_ = (uint8_t)(u(1) & 0x0F);
        cpx_ = cpy_ = 0;
        commandEnd();
        return;
    }
    if ((op & ~0x001F) == 0x0800) {            // WPR (RN) D
        uint16_t d = u(0);
        switch (op & 0x1F) {
        case 0x00: cl0_ = d; break;
        case 0x01: cl1_ = d; break;
        case 0x02: ccmp_ = d; break;
        case 0x03: edg_ = d; break;
        case 0x04: mask_ = d; break;
        case 0x05:
            ppy_ = (uint8_t)(d >> 12); pzcy_ = (uint8_t)((d >> 8) & 0xF);
            ppx_ = (uint8_t)((d >> 4) & 0xF); pzcx_ = (uint8_t)(d & 0xF);
            break;
        case 0x06:
            psy_ = (uint8_t)(d >> 12); psx_ = (uint8_t)((d >> 4) & 0xF);
            break;
        case 0x07:
            pey_ = (uint8_t)(d >> 12); pzy_ = (uint8_t)((d >> 8) & 0xF);
            pex_ = (uint8_t)((d >> 4) & 0xF); pzx_ = (uint8_t)(d & 0xF);
            break;
        case 0x08: xmin_ = (int16_t)d; break;
        case 0x09: ymin_ = (int16_t)d; break;
        case 0x0A: xmax_ = (int16_t)d; break;
        case 0x0B: ymax_ = (int16_t)d; break;
        case 0x0C:                             // RWP high: DN and the top 8 address bits
            rwpDn_ = (uint8_t)(d >> 14);
            rwp_   = (rwp_ & 0x00FFF) | ((uint32_t)(d & 0xFF) << 12);
            break;
        case 0x0D:                             // RWP low: the bottom 12 address bits
            rwp_ = (rwp_ & 0xFF000) | (uint32_t)(d >> 4);
            break;
        default:                               // Pr0E/0F undefined, Pr10-13 read-only
            commandError();
            break;
        }
        commandEnd();
        return;
    }
    if ((op & ~0x001F) == 0x0C00) {            // RPR (RN) -> read FIFO; clears ARD
        uint16_t v = 0;
        bool     ok = true;
        switch (op & 0x1F) {
        case 0x00: v = cl0_; break;
        case 0x01: v = cl1_; break;
        case 0x02: v = ccmp_; break;
        case 0x03: v = edg_; break;
        case 0x04: v = mask_; break;
        case 0x05: v = (uint16_t)((ppy_ << 12) | (pzcy_ << 8) | (ppx_ << 4) | pzcx_); break;
        case 0x06: v = (uint16_t)((psy_ << 12) | (psx_ << 4)); break;
        case 0x07: v = (uint16_t)((pey_ << 12) | (pzy_ << 8) | (pex_ << 4) | pzx_); break;
        case 0x08: v = (uint16_t)xmin_; break;
        case 0x09: v = (uint16_t)ymin_; break;
        case 0x0A: v = (uint16_t)xmax_; break;
        case 0x0B: v = (uint16_t)ymax_; break;
        case 0x0C: v = (uint16_t)((rwpDn_ << 14) | ((rwp() >> 12) & 0xFF)); break;
        case 0x0D: v = (uint16_t)((rwp() & 0xFFF) << 4); break;
        case 0x10: case 0x11: {                // DP: where CP is, physically
            uint32_t addr = 0;
            int      shift = 0;
            wordAddress(cpx_, cpy_, addr, shift);
            int dpd = (shift / bitsPerPixel()) << log2bpp(bitsPerPixel());
            v = (op & 1) ? (uint16_t)(((addr & 0xFFF) << 4) | (dpd & 0xF))
                         : (uint16_t)((orgDn_ << 14) | ((addr >> 12) & 0xFF));
            break;
        }
        case 0x12: v = (uint16_t)cpx_; break;
        case 0x13: v = (uint16_t)cpy_; break;
        default: ok = false; break;
        }
        if (!ok) {
            commandError();
            commandEnd();
            return;
        }
        sr_ = (uint8_t)(sr_ & ~kARD);
        queueRead(v);
        endRead();
        return;
    }
    if ((op & ~0x000F) == 0x1800) {            // WPTN (PRA) n, D1..Dn
        int pra = op & 0x0F;
        for (size_t i = 1; i < params_.size(); ++i) pram_[(pra + i - 1) & 0x0F] = params_[i];
        commandEnd();
        return;
    }
    if ((op & ~0x000F) == 0x1C00) {            // RPTN (PRA) n -> read FIFO
        int pra = op & 0x0F;
        int n   = u(0);
        for (int i = 0; i < n; ++i) queueRead(pram_[(pra + i) & 0x0F]);
        endRead();
        return;
    }

    // ---- Data transfer (manual 6.5) ----
    if (op == 0x4400) {                        // RD: one word at RWP -> read FIFO; RWP++
        queueRead(vramRead(rwpDn_, rwp()));
        setRwp(rwp() + 1);
        endRead();
        return;
    }
    if (op == 0x4800) {                        // WT D: one word at RWP; RWP++
        vramWrite(rwpDn_, rwp(), u(0));
        setRwp(rwp() + 1);
        commandEnd();
        return;
    }
    if ((op & ~0x0003) == 0x4C00) {            // MOD (MM) D under MASK; RWP++
        vramWrite(rwpDn_, rwp(), modify(vramRead(rwpDn_, rwp()), u(0), op & 3));
        setRwp(rwp() + 1);
        commandEnd();
        return;
    }
    if (op == 0x5800 || (op & ~0x0003) == 0x5C00) {   // CLR / SCLR D AX AY
        clearBlock(u(0), p(1), p(2), (op & ~0x0003) == 0x5C00, op & 3);
        commandEnd();
        return;
    }
    if (op == 0x2400 || op == 0x2800 || (op & ~0x0003) == 0x2C00) {   // DRD / DWT / DMOD AX AY
        // Under program control (manual 6.5, no DMAC): the words follow through the FIFOs.
        // The block is (|AX|+1) x (|AY|+1) words from RWP, walked as CLR walks it: "if
        // minus values are set in AX and AY, the read direction becomes negative" (DRD-2),
        // a negative AY running DOWN in Y, i.e. up in memory.
        int16_t ax = p(0), ay = p(1);
        xferSx_   = ax < 0 ? -1 : 1;
        xferSy_   = ay < 0 ? -1 : 1;
        xferAx_   = std::abs(ax) + 1;
        xferAy_   = std::abs(ay) + 1;
        xferX_    = 0;
        xferY_    = 0;
        xferBase_ = rwp();
        xferLeft_ = xferAx_ * xferAy_;
        xferRead_ = (op == 0x2400);
        // RWPe: RWP's column on the block's LAST raster (manual CLR-4: RWP $56, AY = -6,
        // MW $10 -> RWPe $B6).
        setRwp(rwp() - (uint32_t)ay * mw(rwpDn_));
        if (xferRead_) feedRead();             // prime; popRead() keeps it fed. CED never -- ABT ends a DRD
        return;                                // CED comes with the last data word (DWT)
    }
    if ((op & ~0x0F00) == 0x6000 || (op & ~0x0F03) == 0x7000) {   // CPY / SCPY SAH SAL AX AY
        // The source is a 20-bit word address, SAH (8 bits) then SAL (12 bits, left-
        // justified), on the screen RWP names; RWP is the destination (manual CPY-1/2).
        const uint32_t src = ((uint32_t)(u(0) & 0xFF) << 12) | (uint32_t)(u(1) >> 4);
        const bool     sel = (op & 0xF000) == 0x7000;
        copyBlock(src, p(2), p(3), (op & 0x0800) != 0, (op >> 8) & 7, sel, op & 3);
        commandEnd();
        return;
    }

    // ---- Graphic drawing (manual 6.6) ----
    // AREA 001/101 end a drawing where the pointer crossed: "drawing is executed as long
    // as the CP resides in the defined area" (6.6.3), so that is where CP is left.
    const auto endDraw = [&] {
        if (stopped_) {
            cpx_ = stopX_;
            cpy_ = stopY_;
        }
        commandEnd();
    };
    const uint16_t top = (uint16_t)(op & 0xFF00);
    const bool     rel = (op & 0x0400) != 0;   // bit 10 is the A/R distinction in every pair
    switch (top) {
    case 0x8000: case 0x8400: {                // AMOVE / RMOVE
        cpx_ = (int16_t)(rel ? cpx_ + p(0) : p(0));
        cpy_ = (int16_t)(rel ? cpy_ + p(1) : p(1));
        commandEnd();
        return;
    }
    case 0x8800: case 0x8C00: {                // ALINE / RLINE: draw to Pe (excluded), CP = Pe
        int x = rel ? cpx_ + p(0) : p(0);
        int y = rel ? cpy_ + p(1) : p(1);
        drawLine(cpx_, cpy_, x, y);
        cpx_ = (int16_t)x;
        cpy_ = (int16_t)y;
        endDraw();
        return;
    }
    case 0x9000: case 0x9400: {                // ARCT / RRCT: X first, around, back to CP
        int x = rel ? cpx_ + p(0) : p(0);
        int y = rel ? cpy_ + p(1) : p(1);
        drawLine(cpx_, cpy_, x, cpy_);
        drawLine(x, cpy_, x, y);
        drawLine(x, y, cpx_, y);
        drawLine(cpx_, y, cpx_, cpy_);
        endDraw();
        return;
    }
    case 0x9800: case 0x9C00:                  // APLL / RPLL
    case 0xA000: case 0xA400: {                // APLG / RPLG
        int  n   = u(0);
        int  sx  = cpx_, sy = cpy_;
        int  ex  = sx, ey = sy;
        for (int i = 0; i < n; ++i) {
            ex = rel ? sx + p(1 + 2 * i) : p(1 + 2 * i);
            ey = rel ? sy + p(2 + 2 * i) : p(2 + 2 * i);
            drawLine(sx, sy, ex, ey);
            sx = ex;
            sy = ey;
        }
        if (top == 0xA000 || top == 0xA400) {
            drawLine(sx, sy, cpx_, cpy_);      // a polygon closes on CP and leaves it there
        } else {
            cpx_ = (int16_t)ex;
            cpy_ = (int16_t)ey;
        }
        endDraw();
        return;
    }
    case 0xC000: case 0xC400: {                // AFRCT / RFRCT
        int x = rel ? cpx_ + p(0) : p(0);
        int y = rel ? cpy_ + p(1) : p(1);
        fillRect(x, y);
        endDraw();
        return;
    }
    case 0xCC00: {                             // DOT: the pixel at CP; CP stays
        drawPixel(cpx_, cpy_);
        endDraw();
        return;
    }
    case 0xC800: case 0xC900: {                // PAINT (E in bit 8)
        paint((op & 0x0100) != 0);
        endDraw();
        return;
    }
    default:
        if ((op & 0xE000) == 0xE000) {         // AGCPY / RGCPY (S DSD) Xs Ys DX DY
            int xs = p(0), ys = p(1);
            if (op & 0x1000) {                 // RGCPY: the source corner relative to CP
                xs += cpx_;
                ys += cpy_;
            }
            graphicCopy(xs, ys, p(2), p(3), (op & 0x0800) != 0, (op >> 8) & 7);
            endDraw();
            return;
        }
        if ((op & 0xF000) == 0xD000) {         // PTN (SL SD) SZ: SL bit 11, SD bits 10-8
            // (the PTN-8/PTN-9 examples, $D8XX SL = 1 and $D1XX SD = 1, settle the fields)
            drawPattern(u(0) & 0xFF, u(0) >> 8, (op >> 8) & 7, (op & 0x0800) != 0);
            endDraw();
            return;
        }
        break;
    }

    // ---- The curves (manual CRCL, ELPS, AARC, RARC, AEARC, REARC): C (bit 8) = 1 is
    // clockwise, 0 counter-clockwise, in the logical frame (+Y up). ----
    const bool cw = (op & 0x0100) != 0;
    switch (op & 0xFE00) {
    case 0xA800: {                             // CRCL r: from (A+r, B) all the way round
        const int r = mag13(u(0));
        traceConic(cpx_, cpy_, 1, 1, cpx_ + r, cpy_, cpx_ + r, cpy_, cw);
        endDraw();                             // "the CP moves back to the center" -- it never left
        return;
    }
    case 0xAC00: {                             // ELPS a b dX
        // a and b are NOT the semi-axes: "a : b = dX^2 : dY^2", the curve
        // (X-A)^2/a + (Y-B)^2/b = dX^2/a (ELPS-2). dX is the X semi-axis.
        const int a = mag13(u(0)), b = mag13(u(1)), dx = mag13(u(2));
        if (a == 0 || b == 0) {
            commandError();
            commandEnd();
            return;
        }
        traceConic(cpx_, cpy_, a, b, cpx_ + dx, cpy_, cpx_ + dx, cpy_, cw);
        endDraw();
        return;
    }
    case 0xB000: case 0xB400:                  // AARC / RARC  Xc Yc Xe Ye
    case 0xB800: case 0xBC00: {                // AEARC / REARC  a b Xc Yc Xe Ye
        // From CP round the center to Pe; the radius is CP's own distance from the center
        // (appendix: R = sqrt((CPx-Xc)^2 + (CPy-Yc)^2)). Relative forms are offsets from
        // CP, both the center and the end (RARC-1: "CC (A+dXc, B+dYc)").
        const bool   ell = (op & 0x0800) != 0;
        const size_t o   = ell ? 2 : 0;
        const int    a   = ell ? mag13(u(0)) : 1, b = ell ? mag13(u(1)) : 1;
        if (a == 0 || b == 0) {
            commandError();
            commandEnd();
            return;
        }
        int xc = sext13(u(o)), yc = sext13(u(o + 1)), xe = sext13(u(o + 2)), ye = sext13(u(o + 3));
        if (rel) {
            xc += cpx_;
            yc += cpy_;
            xe += cpx_;
            ye += cpy_;
        }
        traceConic(xc, yc, a, b, cpx_, cpy_, xe, ye, cw);
        cpx_ = (int16_t)xe;                    // "CP moves to Pe. However a dot is not drawn at Pe"
        cpy_ = (int16_t)ye;
        endDraw();
        return;
    }
    default:
        break;
    }

    // Nothing reaches here: paramsFor() let through only opcodes executed above.
    commandError();
    commandEnd();
}

// ---------------------------------------------------------------------------
// Drawing time (timed mode; the DRAWING TIME note in the header).
// ---------------------------------------------------------------------------
void Hd63484::setTimed(bool on) {
    if (!on && timed_) {
        timed_ = false;
        while (busy_) finish();                // untimed, nothing it starts is paid for
    }
    timed_ = on;
}

void Hd63484::advance(uint64_t now2clk) {
    if (now2clk < now_) return;
    while (busy_ && busyUntil_ <= now2clk) {
        now_ = busyUntil_;                     // the next command starts HERE, not at now2clk
        finish();
    }
    now_ = now2clk;
}

void Hd63484::occupy(uint64_t cycles, uint8_t before) {
    pendingSr_ |= (uint8_t)(sr_ & ~before & (kCER | kARD | kCED));
    sr_         = (uint8_t)(sr_ & ~pendingSr_);
    busy_       = true;
    busyUntil_  = drawEnd(now_, cycles ? cycles : 1);   // nothing the chip does is free
}

void Hd63484::finish() {
    busy_ = false;
    sr_ |= pendingSr_;
    pendingSr_ = 0;
    // The words it answered go into the read FIFO now; a command still owed room ends
    // when the host has made it (popRead), as untimed.
    const bool waiting = !rpending_.empty();
    feedRead();
    if (waiting && rpending_.empty()) commandEnd();
    if (!busy_ && !readStalled()) processFifo();
}

// The transfer word about to move (DRD/DWT/DMOD under program control): Table 3's
// (4x+8)y + 16[xy/8] (DRD: 12[xy/8]), spread over the words -- 4 each, 8 more on the
// last word of a row, 16 (12) more on the first word of every group of 8.
uint64_t Hd63484::xferCost() const {
    const int k    = xferAx_ * xferAy_ - xferLeft_;   // this word's index in the block
    uint64_t  cost = 4;
    if (k % 8 == 0) cost += xferRead_ ? 12 : 16;
    if (xferX_ == xferAx_ - 1) cost += 8;
    return cost;
}

// Table 3 (datasheet "ACRTC Command Table"), in 2CLK cycles.
uint64_t Hd63484::opCycles(uint16_t op, const std::vector<uint16_t>& prm, uint64_t dots, uint64_t rows) {
    const auto     u     = [&](size_t i) -> uint64_t { return i < prm.size() ? prm[i] : 0; };
    const auto     words = [&](size_t i) -> uint64_t { return (uint64_t)std::abs((int)(int16_t)u(i)) + 1; };
    const uint64_t P     = (op & 0x0004) ? 6 : 4;     // OPM 100-111 read before they write

    // ---- register access and data transfer ----
    if (op == 0x0400) return 8;                                   // ORG
    if ((op & ~0x001F) == 0x0800) return 6;                       // WPR
    if ((op & ~0x001F) == 0x0C00) return 6;                       // RPR
    if ((op & ~0x000F) == 0x1800)                                 // WPTN: n words
        return 4 * (prm.empty() ? 0 : prm.size() - 1) + 8;
    if ((op & ~0x000F) == 0x1C00) return 4 * u(0) + 10;           // RPTN n
    if (op == 0x2400) return 62;                                  // DRD: + each word as it moves
    if (op == 0x2800 || (op & ~0x0003) == 0x2C00) return 34;      // DWT / DMOD: likewise
    if (op == 0x4400) return 12;                                  // RD
    if (op == 0x4800 || (op & ~0x0003) == 0x4C00) return 8;       // WT / MOD
    if (op == 0x5800) return (2 * words(1) + 8) * words(2) + 12;  // CLR D AX AY
    if ((op & ~0x0003) == 0x5C00) return (4 * words(1) + 6) * words(2) + 12;   // SCLR
    if ((op & ~0x0F00) == 0x6000 || (op & ~0x0F03) == 0x7000)     // CPY / SCPY SAH SAL AX AY
        return (6 * words(2) + 10) * words(3) + 12;

    // ---- graphic drawing ----
    switch (op & 0xFF00) {
    case 0x8000: case 0x8400: return 56;                          // AMOVE / RMOVE
    case 0x8800: case 0x8C00: return P * dots + 18;               // ALINE / RLINE: P.L+18
    case 0x9000: case 0x9400: return P * dots + 54;               // ARCT / RRCT: 2P(A+B)+54
    case 0x9800: case 0x9C00: return P * dots + 16 * u(0) + 8;    // APLL / RPLL: sum[P.L+16]+8
    case 0xA000: case 0xA400: return P * dots + 16 * u(0) + 20;   // APLG / RPLG: ...+P.Lo+20
    case 0xC000: case 0xC400: return P * dots + 8 * rows + 18;    // AFRCT / RFRCT: (P.A+8)B+18
    case 0xCC00: return 8;                                        // DOT
    case 0xC800: case 0xC900: {                                   // PAINT: (18A+102)B-58
        const int64_t c = 18 * (int64_t)dots + 102 * (int64_t)rows - 58;
        return c > 0 ? (uint64_t)c : 0;
    }
    default: break;
    }
    if ((op & 0xE000) == 0xE000) return (P + 2) * dots + 10 * rows + 70;   // AGCPY / RGCPY
    if ((op & 0xF000) == 0xD000) return P * dots + 10 * rows + 20;         // PTN
    switch (op & 0xFE00) {
    case 0xA800: return 8 * dots + 66;                            // CRCL
    case 0xAC00: return 10 * dots + 90;                           // ELPS
    case 0xB000: case 0xB400: return 8 * dots + 18;               // AARC / RARC
    case 0xB800: case 0xBC00: return 10 * dots + 96;              // AEARC / REARC
    default: return 0;
    }
}

// Which slots of the frame the display leaves free for drawing (the rules in the header).
void Hd63484::buildTimeline() {
    timelineStale_ = false;
    const uint16_t omr = regWord(0x04);
    const int      H   = ((regWord(0x82) >> 8) & 0xFF) + 1;
    const int      V   = regWord(0x86) & 0x0FFF;
    timelineAll_ = !(omr & 0x4000) || V == 0;           // not started, or no frame
    slotFree_.clear();
    rasterFree_.clear();
    if (timelineAll_) return;

    int hsw = regWord(0x82) & 0x1F;
    if (hsw < 1) hsw = 1;
    if (hsw > H) hsw = H;
    const int  vsw  = regWord(0x88) & 0x1F;
    const bool ram  = (omr & 0x0080) != 0;              // static RAM: the refresh period draws
    const bool acp  = (omr & 0x2000) != 0;              // drawing priority
    const bool dual = (omr & 0x0008) != 0;              // interleaved (or superimposed)

    slotsPerRaster_ = H;
    slotFree_.assign((size_t)H * (size_t)V, 0);
    rasterFree_.assign((size_t)V + 1, 0);
    for (int v = 0; v < V; ++v) {
        uint32_t   a   = 0;
        const bool bg  = backgroundRaster(v - vsw - vds(), a);
        const bool win = windowRaster(v - vsw, a);
        uint32_t   n   = 0;
        for (int h = 0; h < H; ++h) {
            bool f;
            if (h < hsw - 1) {
                f = ram;                                // DRAM refresh period
            } else if (h == hsw - 1) {
                f = false;                              // the attribute output cycle
            } else {
                const int k    = h - hsw;               // memory cycles from HSYNC's rise
                const bool inB = bg && k >= hds() && k < hds() + hdw();
                const bool inW = win && k >= hws() && k < hws() + hww();
                if ((!inB && !inW) || acp) {
                    f = true;
                } else if (dual) {                      // the drawing phase of each display cycle
                    f = (!inB || ((k - hds()) & 1)) && (!inW || ((k - hws()) & 1));
                } else {
                    f = false;
                }
            }
            slotFree_[(size_t)v * (size_t)H + (size_t)h] = f ? 1 : 0;
            n += f ? 1 : 0;
        }
        rasterFree_[(size_t)v + 1] = rasterFree_[(size_t)v] + n;
    }
    // A frame with no free slot at all is no real timing (every retrace slot is display);
    // drawing at full rate beats a chip that never finishes anything.
    if (rasterFree_.back() == 0) timelineAll_ = true;
}

// When `cycles` 2CLK of drawing that start at `start` are done: the end of the
// ceil(cycles/2)-th free slot at or after `start`.
uint64_t Hd63484::drawEnd(uint64_t start, uint64_t cycles) {
    if (timelineStale_) buildTimeline();
    const uint64_t n = (cycles + 1) / 2;
    const uint64_t s = (start + 1) / 2;                 // the first whole slot
    if (timelineAll_) return 2 * (s + n);

    const uint64_t F        = slotFree_.size();
    const uint64_t perFrame = rasterFree_.back();
    const uint64_t H        = (uint64_t)slotsPerRaster_;
    uint64_t       frame    = s / F;
    const uint64_t pos      = s % F;
    // Free slots of this frame before `pos`; the target is the n-th free one from there.
    uint64_t r      = pos / H;
    uint64_t before = rasterFree_[(size_t)r];
    for (uint64_t i = r * H; i < pos; ++i) before += slotFree_[(size_t)i];
    uint64_t target = before + n;                       // 1-based, counted from this frame's start
    frame += (target - 1) / perFrame;
    target = (target - 1) % perFrame + 1;
    // The raster holding it: rasterFree_[r] < target <= rasterFree_[r + 1].
    r = (uint64_t)(std::lower_bound(rasterFree_.begin(), rasterFree_.end(), (uint32_t)target) -
                   rasterFree_.begin()) - 1;
    uint64_t cnt = rasterFree_[(size_t)r];
    uint64_t i   = r * H;
    for (;; ++i) {
        cnt += slotFree_[(size_t)i];
        if (cnt == target) break;
    }
    return 2 * (frame * F + i + 1);
}

// ---------------------------------------------------------------------------
// Frame-memory arithmetic (manual 6.7, Figure 6.14(b)).
// ---------------------------------------------------------------------------
int Hd63484::bitsPerPixel() const {
    int gbm = (reg_[0x02] >> 0) & 0x07;        // CCR bits 10-8 live in the high byte's low bits
    return gbm <= 4 ? (1 << gbm) : 1;          // 101-111 are invalid; treat as 1 bpp
}

uint32_t Hd63484::sar(int dn) const {
    uint8_t base = (uint8_t)(0xC4 + dn * 8);
    return ((uint32_t)(regWord(base) & 0x0F) << 16) | regWord((uint8_t)(base + 2));
}

// The word holding logical pixel (x, y) and the bit position of its field in it.
void Hd63484::wordAddress(int x, int y, uint32_t& addr, int& shift) const {
    const int bpp = bitsPerPixel();
    const int ppw = 16 / bpp;
    const int dot = x + (orgDpd_ >> log2bpp(bpp));    // the origin's own dot offset
    addr  = orgDpa_ + (uint32_t)floorDiv(dot, ppw) - (uint32_t)y * mw(orgDn_);
    shift = floorMod(dot, ppw) * bpp;
}

uint16_t Hd63484::modify(uint16_t data, uint16_t d, int mm) const {
    uint16_t r = data;
    switch (mm & 3) {
    case 0: r = d; break;
    case 1: r = data | d; break;
    case 2: r = data & d; break;
    default: r = data ^ d; break;
    }
    return (uint16_t)((data & ~mask_) | (r & mask_));
}

void Hd63484::clearBlock(uint16_t d, int16_t ax, int16_t ay, bool masked, int mm) {
    const int      dx  = ax < 0 ? -1 : 1;
    const int      dy  = ay < 0 ? -1 : 1;
    const uint32_t w   = mw(rwpDn_);
    const uint32_t org = rwp();
    for (int y = 0; y != ay + dy; y += dy) {
        for (int x = 0; x != ax + dx; x += dx) {
            uint32_t addr = org + (uint32_t)x - (uint32_t)y * w;
            vramWrite(rwpDn_, addr, masked ? modify(vramRead(rwpDn_, addr), d, mm) : d);
        }
    }
    setRwp(org - (uint32_t)ay * w);               // RWPe: RWP's column on the last raster (manual CLR-4)
}

// CPY/SCPY (manual CPY-1..6, SCPY-1..8): the (|AX|+1) x (|AY|+1)-word block from `src`,
// scanned by S, written from RWP in DSD's order -- so a copy can transpose, mirror, and be
// ordered to survive an overlap ("the host chooses the direction"). Words move one at a
// time, read then written, as the chip does. SCPY applies MM under MASK; CPY does not
// use MASK at all (5.10.2.5). RWP is left at RWPe: one line past the last on the slow axis,
// at the fast axis's start (Table C14-2; CPY-6: RWP $B0, four lines up -> RWPe $70).
void Hd63484::copyBlock(uint32_t src, int16_t ax, int16_t ay, bool s, int dsd, bool masked, int mm) {
    const int64_t w     = mw(rwpDn_);
    const Scan    ss    = sourceScan(s, ax, ay);
    const Scan    ds    = destScan(dsd);
    const int     nFast = (s ? std::abs(ay) : std::abs(ax)) + 1;
    const int     nSlow = (s ? std::abs(ax) : std::abs(ay)) + 1;
    const auto    step  = [&](int dx, int dy) -> int64_t { return dx - dy * w; };   // +Y is MW words lower
    const int64_t dst   = rwp();
    for (int j = 0; j < nSlow; ++j) {
        for (int i = 0; i < nFast; ++i) {
            const uint32_t sa = (uint32_t)((int64_t)src + i * step(ss.fx, ss.fy) + j * step(ss.sx, ss.sy));
            const uint32_t da = (uint32_t)(dst + i * step(ds.fx, ds.fy) + j * step(ds.sx, ds.sy));
            const uint16_t v  = vramRead(rwpDn_, sa);
            vramWrite(rwpDn_, da, masked ? modify(vramRead(rwpDn_, da), v, mm) : v);
        }
    }
    setRwp((uint32_t)(dst + nSlow * step(ds.sx, ds.sy)));
}

// ---------------------------------------------------------------------------
// Drawing one logical pixel (manual 6.6.1-6.6.3, 6.8.2).
// ---------------------------------------------------------------------------
bool Hd63484::areaAllows(int x, int y) {
    const int  area   = (cmd_ >> 5) & 0x07;
    if ((area & 3) == 0) return true;             // X00: no checking
    const bool inside = x >= xmin_ && x <= xmax_ && y >= ymin_ && y <= ymax_;
    const bool watchOutside = !(area & 4);        // 0xx: the area is where drawing may go
    const bool crossed = watchOutside ? !inside : inside;
    switch (area & 3) {
    case 1:                                       // stop: ARD and CED, no more pixels
        if (crossed) {
            sr_ |= kARD;
            stopped_ = true;
            stopX_   = (int16_t)x;
            stopY_   = (int16_t)y;
            return false;
        }
        return true;
    case 2:                                       // suppress, silently
        return !crossed;
    default:                                      // suppress, and say so
        if (crossed) sr_ |= kARD;
        return !crossed;
    }
}

uint16_t Hd63484::colorFor(bool& draw) const {
    const int col = (cmd_ >> 3) & 0x03;
    if (col == 3) {                               // Pattern RAM direct: a 4x4 color tile
        draw = true;
        return pram_[((ppy_ & 3) * 4 + (ppx_ & 3)) & 0x0F];
    }
    int bit = (pram_[ppy_ & 0x0F] >> (ppx_ & 0x0F)) & 1;
    switch (col) {
    case 0: draw = true; return bit ? cl1_ : cl0_;
    case 1: draw = bit != 0; return cl1_;
    default: draw = bit == 0; return cl0_;
    }
}

// One step of the pattern scan (manual 6.8.3): each bit is used PZ+1 times (the zoom
// counter PZC starts wherever the host left it -- "the initial magnification counter
// value", 5.10.2.6), then the pointer advances "until PPX = PEX. Then, PPX is reset to
// PSX" -- through 15 -> 0 when PEX < PSX. Pattern RAM direct (COL 11) scans a 4x4 tile,
// so its pointer runs in two bits.
void Hd63484::stepPatternX() {
    if (pzcx_ < pzx_) {
        ++pzcx_;
        return;
    }
    pzcx_ = 0;
    const int m = ((cmd_ >> 3) & 3) == 3 ? 3 : 15;
    ppx_ = (uint8_t)((ppx_ & m) == (pex_ & m) ? (psx_ & m) : ((ppx_ + 1) & m));
}

void Hd63484::stepPatternY() {
    ++rows_;                                      // one row of a plane command: Table 3's B
    if (pzcy_ < pzy_) {
        ++pzcy_;
        return;
    }
    pzcy_ = 0;
    const int m = ((cmd_ >> 3) & 3) == 3 ? 3 : 15;
    ppy_ = (uint8_t)((ppy_ & m) == (pey_ & m) ? (psy_ & m) : ((ppy_ + 1) & m));
}

uint16_t Hd63484::applyOpm(uint16_t data, uint16_t color, uint32_t /*addr*/, int shift, int bpp) const {
    const uint16_t fm   = (uint16_t)((1u << bpp) - 1);
    const uint16_t mask = (uint16_t)(fm << shift);
    const uint16_t cur  = (uint16_t)((data >> shift) & fm);
    const uint16_t c    = (uint16_t)((color >> shift) & fm);   // the color register's field at this dot
    uint16_t       res  = cur;
    switch (cmd_ & 0x07) {
    case 0: res = c; break;
    case 1: res = cur | c; break;
    case 2: res = cur & c; break;
    case 3: res = cur ^ c; break;
    case 4: if (cur == ((ccmp_ >> shift) & fm)) res = c; break;
    case 5: if (cur != ((ccmp_ >> shift) & fm)) res = c; break;
    case 6: if (cur < c) res = c; break;
    case 7: if (cur > c) res = c; break;
    }
    return (uint16_t)((data & ~mask) | ((res << shift) & mask));
}

// One logical pixel position visited: drawn unless AREA or COL says not, and the pattern
// pointer steps on either way -- a clipped dash is still a dash's worth of pattern.
void Hd63484::drawPixel(int x, int y) {
    if (stopped_) return;
    ++dots_;                                      // a visited position: Table 3's L and d
    if (areaAllows(x, y)) {
        bool     draw = false;
        uint16_t color = colorFor(draw);
        if (draw) {
            uint32_t addr = 0;
            int      shift = 0;
            wordAddress(x, y, addr, shift);
            vramWrite(orgDn_, addr, applyOpm(vramRead(orgDn_, addr), color, addr, shift, bitsPerPixel()));
        }
    }
    if (!stopped_) stepPatternX();
}

// Bresenham from (x0, y0) toward (x1, y1), EXCLUDING the end point (manual ALINE-1:
// "the logical pixel at position Pe is not drawn"). The pattern scans one step per
// pixel along the line, from wherever the pointer is (manual 6.8.3) -- so the segments of
// a rectangle, polyline or polygon continue one pattern rather than restarting it.
void Hd63484::drawLine(int x0, int y0, int x1, int y1) {
    int dx = std::abs(x1 - x0), dy = std::abs(y1 - y0);
    int sx = x1 > x0 ? 1 : (x1 < x0 ? -1 : 0);
    int sy = y1 > y0 ? 1 : (y1 < y0 ? -1 : 0);
    if (dx >= dy) {
        int err = dy * 2 - dx;
        while (x0 != x1) {
            drawPixel(x0, y0);
            if (stopped_) return;
            if (err >= 0) {
                y0 += sy;
                err -= dx * 2;
            }
            x0 += sx;
            err += dy * 2;
        }
    } else {
        int err = dx * 2 - dy;
        while (y0 != y1) {
            drawPixel(x0, y0);
            if (stopped_) return;
            if (err >= 0) {
                x0 += sx;
                err -= dy * 2;
            }
            y0 += sy;
            err += dx * 2;
        }
    }
}

// THE CURVE TRACER, for every circle, ellipse and arc. The curve is
//     b (x - xc)^2 + a (y - yc)^2 = K,  K fixed by the start point,
// a circle when a = b.
//
// WHICH pixels: the nearest pixel to the curve in every column where the curve runs
// mostly horizontally, and in every row where it runs mostly vertically -- the classic
// raster rule, in exact integer arithmetic, mirrored into all four quadrants. So the set
// depends on the curve alone, never on the direction drawn, and is symmetric as the curve
// is (and 8-way for a circle).
//
// IN WHAT ORDER: by angle round the center from the start point, counter-clockwise, or
// clockwise for C = 1 (the mirror image). Drawing order is what the chip's own is too, and
// it matters: the pattern scans along it and an AREA stop ends at the first pixel that
// crossed. The start point is drawn first, then every pixel of the set strictly past its
// angle, up to -- not including -- the end point's angle: "a dot is not drawn at Pe". The
// manual gives no rule for detecting the end (its appendix only says to put Pe on the
// curve, truncating toward the center), so a Pe off the curve ends the arc where its ray
// crosses it. A closed curve (CRCL, ELPS) is an arc whose Pe is its own start: once round.
// (Pixels at the start's own angle -- a thin ellipse's tip -- follow it, nearest first.)
void Hd63484::traceConic(int xc, int yc, int64_t a, int64_t b, int x0, int y0, int xe, int ye, bool cw) {
    using Pt = std::pair<int64_t, int64_t>;
    const int64_t m  = cw ? -1 : 1;                      // C = 1: walk the mirror image
    const Pt      p0 = {x0 - xc, m * (y0 - yc)};
    const Pt      pe = {xe - xc, m * (ye - yc)};
    const int64_t K  = b * p0.first * p0.first + a * p0.second * p0.second;
    drawPixel(x0, y0);
    if (stopped_ || K == 0) return;

    // round(sqrt(rem / d)), exactly: the floor, then up if rem/d >= (n + 1/2)^2.
    const auto nearestRoot = [](int64_t rem, int64_t d) {
        int64_t n = (int64_t)std::sqrt((double)rem / (double)d);
        while (n > 0 && d * n * n > rem) --n;
        while (d * (n + 1) * (n + 1) <= rem) ++n;
        return 4 * rem >= d * (2 * n + 1) * (2 * n + 1) ? n + 1 : n;
    };
    std::vector<Pt> pts;
    const auto add = [&](int64_t u, int64_t v) {
        pts.push_back({u, v});
        pts.push_back({-u, v});
        pts.push_back({u, -v});
        pts.push_back({-u, -v});
    };
    // Columns while the curve is x-major (a v >= b u), then rows while it is y-major
    // (b u > a v) -- judged on the curve itself OR at the chosen pixel, whichever runs
    // longer, so the two runs overlap at the 45-degree point and at a thin ellipse's tip
    // instead of leaving a gap between them.
    for (int64_t u = 0;; ++u) {
        const int64_t rem = K - b * u * u;
        if (rem < 0) break;
        const int64_t v = nearestRoot(rem, a);
        if (a * rem < b * b * u * u && a * v < b * u) break;
        add(u, v);
    }
    for (int64_t v = 0;; ++v) {
        const int64_t rem = K - a * v * v;
        if (rem < 0) break;
        const int64_t u = nearestRoot(rem, b);
        if (b * rem <= a * a * v * v && b * u <= a * v) break;
        add(u, v);
    }
    std::sort(pts.begin(), pts.end());
    pts.erase(std::unique(pts.begin(), pts.end()), pts.end());

    // Angle order counter-clockwise from p0, exactly: the half-turn a point is in, then the
    // cross product; points on one ray nearest first.
    const auto cross = [](const Pt& p, const Pt& q) { return p.first * q.second - p.second * q.first; };
    const auto dot   = [](const Pt& p, const Pt& q) { return p.first * q.first + p.second * q.second; };
    const auto half  = [&](const Pt& p) {
        const int64_t c = cross(p0, p);
        return (c < 0 || (c == 0 && dot(p0, p) < 0)) ? 1 : 0;
    };
    const auto before = [&](const Pt& p, const Pt& q) {
        if (half(p) != half(q)) return half(p) < half(q);
        const int64_t c = cross(p, q);
        return c != 0 ? c > 0 : dot(p, p) < dot(q, q);
    };
    std::sort(pts.begin(), pts.end(), before);
    const bool once = cross(p0, pe) == 0 && dot(p0, pe) > 0;   // Pe on the start's own ray
    for (const Pt& p : pts) {
        if (p == p0) continue;                                // the start: drawn already
        if (!once && !(half(p) < half(pe) || (half(p) == half(pe) && cross(p, pe) > 0))) break;
        drawPixel((int)(xc + p.first), (int)(yc + m * p.second));
        if (stopped_) return;
    }
}

// The pattern pointer as a plane command anchored at CP would have it at the pixel
// (dx, dy) away: the scan from the pointer the command found, dx steps in X and dy in Y,
// either sign, round the PS..PE window and the zoom. PAINT fills in no fixed order, so it
// places the pointer by position -- a seamless tiling whichever way a span runs.
void Hd63484::patternAt(int dx, int dy, uint8_t ppx0, uint8_t pzcx0, uint8_t ppy0, uint8_t pzcy0) {
    const int  m    = ((cmd_ >> 3) & 3) == 3 ? 3 : 15;
    const auto axis = [m](uint8_t pp, uint8_t pzc, int ps, int pe, int pz, int steps, uint8_t& opp, uint8_t& opzc) {
        const int64_t width = ((pe - ps) & m) + 1;
        const int64_t rep   = pz + 1;
        int64_t       t     = (((pp - ps) & m) % width) * rep + (pzc < pz ? pzc : pz) + steps;
        t                   = ((t % (width * rep)) + width * rep) % (width * rep);
        opp                 = (uint8_t)((ps + t / rep) & m);
        opzc                = (uint8_t)(t % rep);
    };
    axis(ppx0, pzcx0, psx_, pex_, pzx_, dx, ppx_, pzcx_);
    axis(ppy0, pzcy0, psy_, pey_, pzy_, dy, ppy_, pzcy_);
}

// PAINT (manual PAINT-1..11): fill the closed area round CP, bounded by the EDGE color --
// E = 0: pixels equal to EDG; E = 1: every pixel NOT equal to EDG -- and, as the chip
// does, by pixels already in CL0 or CL1: "color code stored in color registers (CL0 or
// CL1) are also considered to be an edge during PAINT execution" (PAINT-1), which is what
// stops it filling what it has just filled. Each color is compared at the pixel's own
// field, as a drawing color is.
//
// It paints spans "parallel to the X axis (left to right)" from CP's line out to the lines
// above and below (PAINT-3), 4-connected. The chip's four-entry seed stack overflows into
// the read FIFO for the host to re-issue; THIS ONE HAS NO LIMIT, so one PAINT fills the
// whole area and RFR is never set -- a driver that follows the manual's re-issue loop
// (Figure C34-6) simply finds nothing to re-issue. Visited pixels are kept by their
// PHYSICAL location, so a fill that escapes a leaking outline wraps round the frame memory
// as the chip's would and still ends. AREA suppress modes (x1x) bound the fill like an
// edge; the stop modes (x01) end it at the first crossing. CP ends at the end point
// (Figure C34-4): just past the last span painted.
void Hd63484::paint(bool e) {
    const int      bpp = bitsPerPixel();
    const uint16_t fm  = (uint16_t)((1u << bpp) - 1);
    const int      am  = (cmd_ >> 5) & 7;
    std::vector<bool> seen((size_t)vram_.size() * 16);
    const auto key = [&](int x, int y) {
        uint32_t addr = 0;
        int      shift = 0;
        wordAddress(x, y, addr, shift);
        return (size_t)(addr & vmask_) * 16 + (size_t)shift;
    };
    const auto fillable = [&](int x, int y) {
        if (seen[key(x, y)]) return false;
        if ((am & 3) >= 2) {                    // suppress outside (0x1x) / inside (1x1x)
            const bool inside = x >= xmin_ && x <= xmax_ && y >= ymin_ && y <= ymax_;
            if ((am & 4) ? inside : !inside) {
                if ((am & 3) == 3) sr_ |= kARD;
                return false;
            }
        }
        uint32_t addr = 0;
        int      shift = 0;
        wordAddress(x, y, addr, shift);
        const uint16_t v = (uint16_t)((vramRead(orgDn_, addr) >> shift) & fm);
        const bool     edge = e ? v != ((edg_ >> shift) & fm) : v == ((edg_ >> shift) & fm);
        return !edge && v != ((cl0_ >> shift) & fm) && v != ((cl1_ >> shift) & fm);
    };

    const int     x0 = cpx_, y0 = cpy_;
    const uint8_t ppx0 = ppx_, pzcx0 = pzcx_, ppy0 = ppy_, pzcy0 = pzcy_;
    std::vector<std::pair<int, int>> seeds{{x0, y0}};
    while (!seeds.empty()) {
        auto [x, y] = seeds.back();
        seeds.pop_back();
        if (!fillable(x, y)) continue;
        // Claim the span as it is found, so one that wraps all the way round the frame
        // memory meets its own start and stops.
        seen[key(x, y)] = true;
        int xl = x, xr = x;
        while (fillable(xl - 1, y)) seen[key(--xl, y)] = true;
        while (fillable(xr + 1, y)) seen[key(++xr, y)] = true;
        ++rows_;                                // a span: PAINT's B (Table 3)
        for (int xi = xl; xi <= xr; ++xi) {
            patternAt(xi - x0, y - y0, ppx0, pzcx0, ppy0, pzcy0);
            drawPixel(xi, y);
            if (stopped_) break;
        }
        if (stopped_) break;
        cpx_ = (int16_t)(xr + 1);
        cpy_ = (int16_t)y;
        for (int ny : {y + 1, y - 1}) {
            bool run = false;
            for (int xi = xl; xi <= xr; ++xi) {
                const bool f = fillable(xi, ny);
                if (f && !run) seeds.push_back({xi, ny});
                run = f;
            }
        }
    }
    ppx_ = ppx0; pzcx_ = pzcx0; ppy_ = ppy0; pzcy_ = pzcy0;
}

// PTN (manual PTN-1..9): the pattern RAM as a (SZx + 1) x (SZy + 1) pixel picture at CP.
// Its rows run along SD x 45 degrees, counter-clockwise from +X; successive rows step 90
// degrees further round -- or only 45, slanting it into a parallelogram, when SL = 1
// (Table C36-1, read from the scan). A diagonal step is one pixel in X AND Y, so a
// diagonal PTN leaves a pixel's gap between its neighbours (PTN-9). The pattern scans as
// AFRCT's does -- every row from the pointer PTN found, PPY stepping per row -- and CP
// ends one row past the last, at its start (Pe in Table C36-1).
void Hd63484::drawPattern(int szx, int szy, int sd, bool sl) {
    static constexpr int kDir[8][2] = {{1, 0}, {1, 1}, {0, 1}, {-1, 1}, {-1, 0}, {-1, -1}, {0, -1}, {1, -1}};
    const int     fx = kDir[sd][0], fy = kDir[sd][1];
    const int     sx = kDir[(sd + (sl ? 1 : 2)) & 7][0], sy = kDir[(sd + (sl ? 1 : 2)) & 7][1];
    const uint8_t ppx0 = ppx_, pzcx0 = pzcx_;
    for (int j = 0; j <= szy; ++j) {
        ppx_  = ppx0;
        pzcx_ = pzcx0;
        for (int i = 0; i <= szx; ++i) {
            drawPixel(cpx_ + i * fx + j * sx, cpy_ + i * fy + j * sy);
            if (stopped_) return;
        }
        stepPatternY();
    }
    ppx_  = ppx0;
    pzcx_ = pzcx0;
    cpx_  = (int16_t)(cpx_ + (szy + 1) * sx);
    cpy_  = (int16_t)(cpy_ + (szy + 1) * sy);
}

// AGCPY/RGCPY (manual AGCPY-1..5, RGCPY-1..3): copy the (|DX|+1) x (|DY|+1)-pixel
// rectangle from (xs, ys) to CP, in logical pixels -- CPY's scans in pixel units, S for
// the source and DSD for the destination (Tables C37-1/C37-2 are C14-1/C14-2 again), so it
// rotates by 90 degrees and mirrors. The source pixel IS the color data: COL is fixed at
// 00 and the pattern RAM is not used, while OPM combines it with the destination pixel and
// AREA judges the destination. One pixel at a time, read then written. CP ends one line
// past the last on the slow axis (AGCPY-4: S = 1, DSD = 000, CP (4,2), DX = 13 -> (4,16)).
void Hd63484::graphicCopy(int xs, int ys, int dx, int dy, bool s, int dsd) {
    const int      bpp   = bitsPerPixel();
    const uint16_t fm    = (uint16_t)((1u << bpp) - 1);
    const Scan     ss    = sourceScan(s, dx, dy);
    const Scan     ds    = destScan(dsd);
    const int      nFast = (s ? std::abs(dy) : std::abs(dx)) + 1;
    const int      nSlow = (s ? std::abs(dx) : std::abs(dy)) + 1;
    const int      x0 = cpx_, y0 = cpy_;
    for (int j = 0; j < nSlow; ++j) {
        ++rows_;                                // Table 3's B...
        for (int i = 0; i < nFast; ++i) {
            ++dots_;                            // ...and A, clipped pixels included
            uint32_t sa = 0;
            int      sh = 0;
            wordAddress(xs + i * ss.fx + j * ss.sx, ys + i * ss.fy + j * ss.sy, sa, sh);
            const uint16_t v  = (uint16_t)((vramRead(orgDn_, sa) >> sh) & fm);
            const int      tx = x0 + i * ds.fx + j * ds.sx, ty = y0 + i * ds.fy + j * ds.sy;
            if (!areaAllows(tx, ty)) {
                if (stopped_) return;
                continue;
            }
            uint32_t da = 0;
            int      dsh = 0;
            wordAddress(tx, ty, da, dsh);
            vramWrite(orgDn_, da, applyOpm(vramRead(orgDn_, da), (uint16_t)(v << dsh), da, dsh, bpp));
        }
    }
    cpx_ = (int16_t)(x0 + nSlow * ds.sx);
    cpy_ = (int16_t)(y0 + nSlow * ds.sy);
}

// AFRCT/RFRCT: the rectangle with CP and (x1, y1) as opposite corners, both included,
// tiled with the pattern; CP ends one raster past the far edge in Y (manual AFRCT-1).
// A plane drawing (6.8.4): every row starts the X scan from the pointer as the command
// found it, and PPY steps once per row -- "incremented independent of pixel drawing
// direction". PPY is left on the row after the last, so a rectangle drawn at the new CP
// continues the tiling where this one stopped.
void Hd63484::fillRect(int x1, int y1) {
    int           X = x1 - cpx_, Y = y1 - cpy_;
    int           dx = X < 0 ? -1 : 1, dy = Y < 0 ? -1 : 1;
    const uint8_t ppx0 = ppx_, pzcx0 = pzcx_;
    for (int j = 0; j != Y + dy; j += dy) {
        ppx_  = ppx0;
        pzcx_ = pzcx0;
        for (int k = 0; k != X + dx; k += dx) {
            drawPixel(cpx_ + k, cpy_ + j);
            if (stopped_) return;
        }
        stepPatternY();
    }
    ppx_  = ppx0;
    pzcx_ = pzcx0;
    cpy_  = (int16_t)(cpy_ + Y + dy);
}

// ---------------------------------------------------------------------------
// Scan-out (manual 5.6-5.9): the screens DCR enables, stacked, the window over them.
// ---------------------------------------------------------------------------
bool Hd63484::displayOn() const {
    return (regWord(0x04) & 0x4000) != 0 && (regWord(0x06) & 0x4000) != 0;   // STR, SE1
}

int Hd63484::displayWidth() const {
    const int hdw  = (regWord(0x84) & 0xFF) + 1;                  // memory cycles
    const int bpp  = bitsPerPixel();
    const int gai  = (regWord(0x04) >> 4) & 0x07;
    const int dual = (regWord(0x04) & 0x08) ? 2 : 1;             // ACM: interleaved/superimposed
    const int words = gai <= 3 ? (1 << gai) : 1;                  // words per display cycle
    return hdw * (16 / bpp) * words / dual;
}

int Hd63484::displayHeight() const {
    const uint16_t dcr = regWord(0x06);
    int h = regWord(0x8A) & 0x0FFF;                               // SP1, the base screen
    if (dcr & 0x2000) h += regWord(0x8C) & 0x0FFF;                // SE0 set: the upper
    if (dcr & 0x0800) h += regWord(0x8E) & 0x0FFF;                // SE2 set: the lower
    return h;
}

int Hd63484::gaiWords() const {
    int gai = (regWord(0x04) >> 4) & 0x07;
    if (gai <= 3) return 1 << gai;
    return gai == 7 ? 1 : 0;
}

int Hd63484::accessMode() const { return (regWord(0x04) & 0x08) ? 2 : 1; }

bool Hd63484::backgroundRaster(int raster, uint32_t& startAddr) const {
    const uint16_t dcr = regWord(0x06);
    if (raster < 0) return false;
    const int sp0 = (dcr & 0x2000) ? (regWord(0x8C) & 0x0FFF) : 0;
    const int sp1 = regWord(0x8A) & 0x0FFF;
    const int sp2 = (dcr & 0x0800) ? (regWord(0x8E) & 0x0FFF) : 0;
    int  dn, top;
    bool lit;
    if (raster < sp0)                  { dn = 0; top = 0;         lit = (dcr & 0x1000) != 0; }
    else if (raster < sp0 + sp1)       { dn = 1; top = sp0;       lit = (dcr & 0x4000) != 0; }
    else if (raster < sp0 + sp1 + sp2) { dn = 2; top = sp0 + sp1; lit = (dcr & 0x0400) != 0; }
    else return false;
    if (!lit) return false;
    startAddr = sar(dn) + (uint32_t)(raster - top) * mw(dn);
    return true;
}

bool Hd63484::windowRaster(int vsyncRaster, uint32_t& startAddr) const {
    if ((regWord(0x06) & 0x0300) != 0x0300) return false;
    const int wy = vsyncRaster - vws();
    if (wy < 0 || wy >= vww()) return false;
    startAddr = sar(3) + (uint32_t)wy * mw(3);
    return true;
}

void Hd63484::scanline(int y, std::span<uint16_t> out) const {
    const uint16_t dcr = regWord(0x06);
    const int      bpp = bitsPerPixel();
    const int      ppw = 16 / bpp;
    const uint16_t fm  = (uint16_t)((1u << bpp) - 1);
    const int      w   = displayWidth();
    const int      n   = (int)out.size() < w ? (int)out.size() : w;
    if (n <= 0 || y < 0) return;

    // Which background screen owns raster y, and its first raster.
    int      dn = 1, top = 0;
    int      sp0 = (dcr & 0x2000) ? (regWord(0x8C) & 0x0FFF) : 0;
    int      sp1 = regWord(0x8A) & 0x0FFF;
    int      sp2 = (dcr & 0x0800) ? (regWord(0x8E) & 0x0FFF) : 0;
    bool     lit;
    if (y < sp0)              { dn = 0; top = 0;         lit = (dcr & 0x1000) != 0; }   // SE0 = 11
    else if (y < sp0 + sp1)   { dn = 1; top = sp0;       lit = (dcr & 0x4000) != 0; }   // SE1
    else if (y < sp0 + sp1 + sp2) { dn = 2; top = sp0 + sp1; lit = (dcr & 0x0400) != 0; } // SE2 = 11
    else return;

    uint32_t addr = sar(dn) + (uint32_t)(y - top) * mw(dn);
    for (int x = 0; x < n; x += ppw) {
        uint16_t word = lit ? peekWord(addr++) : 0;   // scan-out: graphic screens only
        for (int b = 0; b < ppw && x + b < n; ++b) {
            out[(size_t)(x + b)] = (uint16_t)(word & fm);
            word >>= bpp;
        }
    }

    // The window (SE3 = 11) over it: HWS/HWW in memory cycles from HSYNC, VWS/VWW in
    // rasters from VSYNC; both relative to where the base display starts (HDS, VDS).
    if ((dcr & 0x0300) == 0x0300) {
        const int vds = (regWord(0x88) >> 8) + 1;
        const int vws = (regWord(0x94) & 0x0FFF) + 1;
        const int vww = regWord(0x96) & 0x0FFF;
        const int wy  = y - (vws - vds);
        if (wy < 0 || wy >= vww) return;
        const int cyc = w / ((regWord(0x84) & 0xFF) + 1);        // pixels per memory cycle
        const int hds = (regWord(0x84) >> 8) + 1;
        const int hws = (regWord(0x92) >> 8) + 1;
        const int hww = (regWord(0x92) & 0xFF) + 1;
        const int x0  = (hws - hds) * cyc;
        uint32_t  wa  = sar(3) + (uint32_t)wy * mw(3);
        for (int x = x0; x < x0 + hww * cyc; x += ppw) {
            uint16_t word = peekWord(wa++);
            for (int b = 0; b < ppw; ++b) {
                int px = x + b;
                if (px >= 0 && px < n) out[(size_t)px] = (uint16_t)(word & fm);
                word >>= bpp;
            }
        }
    }
}

uint16_t Hd63484::param(uint8_t rn) const {
    switch (rn) {
    case 0x00: return cl0_;
    case 0x01: return cl1_;
    case 0x02: return ccmp_;
    case 0x03: return edg_;
    case 0x04: return mask_;
    case 0x05: return (uint16_t)((ppy_ << 12) | (pzcy_ << 8) | (ppx_ << 4) | pzcx_);
    case 0x06: return (uint16_t)((psy_ << 12) | (psx_ << 4));
    case 0x07: return (uint16_t)((pey_ << 12) | (pzy_ << 8) | (pex_ << 4) | pzx_);
    case 0x08: return (uint16_t)xmin_;
    case 0x09: return (uint16_t)ymin_;
    case 0x0A: return (uint16_t)xmax_;
    case 0x0B: return (uint16_t)ymax_;
    case 0x0C: return (uint16_t)((rwpDn_ << 14) | ((rwp() >> 12) & 0xFF));
    case 0x0D: return (uint16_t)((rwp() & 0xFFF) << 4);
    case 0x12: return (uint16_t)cpx_;
    case 0x13: return (uint16_t)cpy_;
    default: return 0;
    }
}

// ---------------------------------------------------------------------------
// State.
// ---------------------------------------------------------------------------
void Hd63484::serialize(StateWriter& w) const {
    w.raw(reg_, sizeof reg_);
    w.u8(ar_);
    w.u8(sr_);
    w.raw(wfifo_, sizeof wfifo_);
    w.u32((uint32_t)wfifoN_);
    w.raw(rfifo_, sizeof rfifo_);
    w.u32((uint32_t)rfifoN_);
    w.u8(fifoHi_);
    w.boolean(fifoHalf_);
    w.boolean(inCommand_);
    w.u16(cmd_);
    w.u32((uint32_t)params_.size());
    for (uint16_t p : params_) w.u16(p);
    w.u32((uint32_t)paramsWanted_);
    w.u32((uint32_t)xferLeft_);
    w.u32((uint32_t)xferAx_);
    w.u32((uint32_t)xferAy_);
    w.u32((uint32_t)xferX_);
    w.u32((uint32_t)xferY_);
    w.u32(xferBase_);
    w.boolean(xferRead_);
    w.boolean(stopped_);
    w.u16(cl0_); w.u16(cl1_); w.u16(ccmp_); w.u16(edg_); w.u16(mask_);
    w.u8(ppx_); w.u8(ppy_); w.u8(pzcx_); w.u8(pzcy_);
    w.u8(psx_); w.u8(psy_); w.u8(pex_); w.u8(pey_); w.u8(pzx_); w.u8(pzy_);
    w.u16((uint16_t)xmin_); w.u16((uint16_t)ymin_); w.u16((uint16_t)xmax_); w.u16((uint16_t)ymax_);
    w.u32(rwp_);
    w.u8(rwpDn_);
    w.u32(orgDpa_);
    w.u8(orgDpd_);
    w.u8(orgDn_);
    w.u16((uint16_t)cpx_);
    w.u16((uint16_t)cpy_);
    for (uint16_t p : pram_) w.u16(p);
    // The frame memory, little-endian words, length-prefixed so a different fit is caught.
    std::vector<uint8_t> bytes;
    bytes.reserve(vram_.size() * 2);
    for (uint16_t v : vram_) {
        bytes.push_back((uint8_t)v);
        bytes.push_back((uint8_t)(v >> 8));
    }
    w.blob(bytes);
    w.boolean(xferSx_ < 0);
    w.boolean(xferSy_ < 0);
    w.u16((uint16_t)stopX_);
    w.u16((uint16_t)stopY_);
    w.u32((uint32_t)rpending_.size());
    for (uint16_t v : rpending_) w.u16(v);
    w.u64(now_);
    w.boolean(busy_);
    w.u64(busyUntil_);
    w.u8(pendingSr_);
}

void Hd63484::deserialize(StateReader& r) {
    r.raw(reg_, sizeof reg_);
    ar_ = r.u8();
    sr_ = r.u8();
    r.raw(wfifo_, sizeof wfifo_);
    wfifoN_ = (int)r.u32();
    r.raw(rfifo_, sizeof rfifo_);
    rfifoN_ = (int)r.u32();
    fifoHi_   = r.u8();
    fifoHalf_ = r.boolean();
    inCommand_ = r.boolean();
    cmd_ = r.u16();
    uint32_t np = r.u32();
    params_.clear();
    for (uint32_t i = 0; i < np && i < 65536; ++i) params_.push_back(r.u16());
    paramsWanted_ = (int)r.u32();
    xferLeft_ = (int)r.u32();
    xferAx_   = (int)r.u32();
    xferAy_   = (int)r.u32();
    xferX_    = (int)r.u32();
    xferY_    = (int)r.u32();
    xferBase_ = r.u32();
    xferRead_ = r.boolean();
    stopped_  = r.boolean();
    cl0_ = r.u16(); cl1_ = r.u16(); ccmp_ = r.u16(); edg_ = r.u16(); mask_ = r.u16();
    ppx_ = r.u8(); ppy_ = r.u8(); pzcx_ = r.u8(); pzcy_ = r.u8();
    psx_ = r.u8(); psy_ = r.u8(); pex_ = r.u8(); pey_ = r.u8(); pzx_ = r.u8(); pzy_ = r.u8();
    xmin_ = (int16_t)r.u16(); ymin_ = (int16_t)r.u16(); xmax_ = (int16_t)r.u16(); ymax_ = (int16_t)r.u16();
    rwp_    = r.u32() & 0xFFFFF;
    rwpDn_  = (uint8_t)(r.u8() & 3);
    orgDpa_ = r.u32();
    orgDpd_ = r.u8();
    orgDn_  = (uint8_t)(r.u8() & 3);
    cpx_ = (int16_t)r.u16();
    cpy_ = (int16_t)r.u16();
    for (auto& p : pram_) p = r.u16();
    std::vector<uint8_t> bytes = r.blob();
    if (bytes.size() == vram_.size() * 2) {
        for (size_t i = 0; i < vram_.size(); ++i)
            vram_[i] = (uint16_t)(bytes[2 * i] | (bytes[2 * i + 1] << 8));
    }
    xferSx_ = r.boolean() ? -1 : 1;
    xferSy_ = r.boolean() ? -1 : 1;
    stopX_  = (int16_t)r.u16();
    stopY_  = (int16_t)r.u16();
    uint32_t nr = r.u32();
    rpending_.clear();
    for (uint32_t i = 0; i < nr && i < 65536; ++i) rpending_.push_back(r.u16());
    now_       = r.u64();
    busy_      = r.boolean();
    busyUntil_ = r.u64();
    pendingSr_ = r.u8();
    timelineStale_ = true;
    if (wfifoN_ < 0 || wfifoN_ > kFifoBytes) wfifoN_ = 0;
    if (rfifoN_ < 0 || rfifoN_ > kFifoBytes) rfifoN_ = 0;
    dirty_ = true;
}

} // namespace altair
