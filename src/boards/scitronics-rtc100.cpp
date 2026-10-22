#include "boards/scitronics-rtc100.h"

#include "core/statefile.h"

namespace altair {
namespace {

// The 6821 control register, in the only configuration this card uses. Bits 5:4 = 11
// puts CA2/CB2 in output mode; bit 3 is then the level on the pin.
constexpr uint8_t kCrDataSelect = 0x04;  // bit 2: 1 = data register, 0 = direction register
constexpr uint8_t kCrC2Level    = 0x08;  // bit 3: the CA2 / CB2 output level

// The clock chip's command byte (chips/msm5832.h): Hold(6) / Write(5) / digit(3-0).
constexpr uint8_t kCmdHold  = 0x40;
constexpr uint8_t kCmdWrite = 0x20;

}  // namespace

// ---------------------------------------------------------------------------
// The bus interface. Four consecutive ports, plus the acknowledge cycle when we are
// the one interrupting -- the card vectors itself (see the header).
// ---------------------------------------------------------------------------
bool Rtc100Board::decodes(const BusCycle& c) const {
    if (!enabled_) return false;
    if (c.type == Cycle::IoRead || c.type == Cycle::IoWrite) {
        uint8_t p = c.port();
        return p == portA() || p == ctrlA() || p == portB() || p == ctrlB();
    }
    // We claim the acknowledge only while we are actually requesting; a card that
    // grabbed every IntAck would be answering for somebody else's interrupt.
    if (c.type == Cycle::IntAck) return intWired_ && intReq_;
    return false;
}

uint8_t Rtc100Board::read(const BusCycle& c) {
    if (c.type == Cycle::IntAck) {
        // Jam RST n. The acknowledge IS the dismissal on this card -- there is no
        // status register to clear and no port write in any driver that drops it.
        dropRequest();
        return rstOpcode(restart_);
    }
    if (c.type != Cycle::IoRead) return 0xFF;

    uint8_t p = c.port();
    if (p == portA()) {
        // Direction register, if the guest has CRA bit 2 clear -- a 6821 read there
        // reads back the DDR, which is what it wrote.
        if (!(cra_ & kCrDataSelect)) return ddra_;

        // The data register. INPUT pins come from the clock, OUTPUT pins read back
        // the output latch -- so in the standard 0FH direction set-up the guest gets
        // the digit in the HIGH nibble and its own digit code still in the low one.
        uint8_t in = (uint8_t)(rtc_.readData() << 4);
        return (uint8_t)((in & ~ddra_) | (oraa_ & ddra_));
    }
    if (p == ctrlA()) return cra_;
    if (p == portB()) {
        if (!(crb_ & kCrDataSelect)) return ddrb_;
        return orb_;  // nothing drives port B's pins; an input bit reads its latch
    }
    if (p == ctrlB()) return crb_;
    return 0xFF;
}

void Rtc100Board::write(const BusCycle& c) {
    uint8_t p = c.port();

    if (p == portA()) {
        if (cra_ & kCrDataSelect) {
            oraa_ = c.data;
            // The high nibble is the digit data, staged for the next write strobe;
            // the low nibble is the digit address, which takes effect immediately.
            rtc_.writeData((uint8_t)(c.data >> 4));
            driveClock(false);
        } else {
            ddra_ = c.data;
        }
        return;
    }
    if (p == ctrlA()) {
        cra_ = c.data;
        driveClock(false);  // CA2 may just have moved Hold
        return;
    }
    if (p == portB()) {
        if (crb_ & kCrDataSelect) {
            uint8_t prev = orb_;
            orb_ = c.data;
            // The write strobe is a LOW pulse on PB0: the set sequence writes 0 then
            // 1 per digit. Commit on the falling edge, once per pulse.
            bool falling = (prev & 1) && !(c.data & 1);
            driveClock(falling);
        } else {
            ddrb_ = c.data;
        }
        return;
    }
    if (p == ctrlB()) {
        crb_ = c.data;
        driveClock(false);
        return;
    }
}

// ---------------------------------------------------------------------------
// The PIA pins, translated into the clock chip's command register. The chip is
// edge-sensitive on Hold -- a rising edge freezes the display, a falling edge
// commits whatever digits were written -- so every transition must be pushed
// through exactly once, and a call that changes nothing must be harmless.
//
// CA2 LOW is Hold ASSERTED (manual: "CA2 = low = hold high = start of stop clock
// pulse"), which is the inversion it is easiest to get backwards.
//
// CB2 -- the clock's READ line -- is deliberately not consulted: chips/msm5832.h
// answers a data read with the selected digit regardless, because that is what every
// real read sequence relies on. We model the pin so the guest can read it back, and
// it gates nothing. See the board doc's Limitations.
// ---------------------------------------------------------------------------
void Rtc100Board::driveClock(bool writeStrobe) {
    uint8_t cmd = (uint8_t)(oraa_ & 0x0F);       // digit select
    if (!(cra_ & kCrC2Level)) cmd |= kCmdHold;   // CA2 low = Hold
    if (writeStrobe) cmd |= kCmdWrite;
    rtc_.writeCommand(cmd);
}

// ---------------------------------------------------------------------------
// The once-a-second interrupt. The seconds output is REAL time, not CPU time: the
// MSM5832 divides its own 32.768 kHz crystal, so the second is kept in host time and
// does not depend on how fast the guest is running (issue #827).
// ---------------------------------------------------------------------------
void Rtc100Board::scheduleTick() {
    if (!clock_) return;
    if (!intWired_) {  // an unsoldered strap is not a deadline -- keep the queue
        tick_.stop(clock_);  // empty so an idle machine can still stand down
        return;
    }
    tick_.start(clock_, 1, 1, [this] { onTick(); });
}

// The ticker repeats by itself: the chip ticks whether or not anyone listened.
void Rtc100Board::onTick() {
    if (!intReq_) {
        intReq_ = true;
        intChanged();
    }
}

void Rtc100Board::dropRequest() {
    if (!intReq_) return;
    intReq_ = false;
    intChanged();
}

// ---------------------------------------------------------------------------
// RESET does NOT touch the time. The MSM5832 is battery-backed: it keeps time across
// a front-panel RESET and across a power cycle alike, which is the whole point of the
// lithium cell on the card. Msm5832::reset() preserves the offset by design; what we
// clear here is the PIA, which is a 6821 and does come up cleared.
// ---------------------------------------------------------------------------
void Rtc100Board::reset(Reset) {
    cra_ = crb_ = ddra_ = ddrb_ = oraa_ = 0;
    orb_ = 1;
    rtc_.reset();
    dropRequest();
    scheduleTick();
}

// POWER-ON. The PIA comes up cleared and the seconds tick starts running; the TIME
// does not change, because the cell kept it while the machine was off.
void Rtc100Board::power() {
    reset(Reset::PowerOn);
}

void Rtc100Board::serialize(StateWriter& w) const {
    Board::serialize(w);
    w.u8(cra_);
    w.u8(crb_);
    w.u8(ddra_);
    w.u8(ddrb_);
    w.u8(oraa_);
    w.u8(orb_);
    w.boolean(intReq_);
    rtc_.serialize(w);
}

void Rtc100Board::deserialize(StateReader& r) {
    Board::deserialize(r);
    cra_  = r.u8();
    crb_  = r.u8();
    ddra_ = r.u8();
    ddrb_ = r.u8();
    oraa_ = r.u8();
    orb_  = r.u8();
    intReq_ = r.boolean();
    rtc_.deserialize(r);
    // The strap and the INT switch are config, already right in a matching machine.
    intChanged();
    scheduleTick();
}

// ---------------------------------------------------------------------------
// Properties: the two dip switches, and the clock itself as a live read-only.
// ---------------------------------------------------------------------------
std::vector<Property> Rtc100Board::properties() {
    std::vector<Property> p;
    {
        Property x;
        x.name  = "port";
        x.help  = "Base address -- MUST BE A MULTIPLE OF 4. Four consecutive ports";
        x.kind  = Kind::Int;
        x.radix = 16;  // ON THE WIRE -> HEX (DESIGN.md 10.0.1)
        x.min   = 0;
        x.max   = 0xFC;
        x.get   = [this] { return Value::ofInt(base_); };
        x.set   = [this](const Value& v, std::string& err) {
            // The PORT switch decodes A2-A7 only: A0 and A1 pick which of the card's
            // four functions is addressed, so a base that is not a multiple of 4 is
            // not a card you could build ("The only valid base addresses are those
            // which are multiples of 4", manual p3).
            if (v.i() & 3) {
                err = "the RTC-100 decodes A2-A7 -- the base must be a multiple of 4";
                return false;
            }
            base_ = (uint8_t)v.i();
            return true;
        };
        p.push_back(std::move(x));
    }
    {
        // NOT irqJumperProperty(). That helper's vocabulary includes vi0..vi7, and
        // this card has no VI connector: it drives pin 73 and supplies its own RST.
        // Offering eight levels it never had would be inventing hardware.
        Property x;
        x.name    = "interrupt";
        x.help    = "The once-a-second interrupt: none | int (pin 73, vectored by `restart`)";
        x.kind    = Kind::Enum;
        x.choices = {"none", "int"};
        x.get     = [this] { return Value::ofStr(intWired_ ? "int" : "none"); };
        x.set     = [this](const Value& v, std::string&) {
            intWired_ = v.s() == "int";
            if (!intWired_) dropRequest();
            scheduleTick();
            return true;
        };
        p.push_back(std::move(x));
    }
    {
        // The INT dip switch (Table II). The manual advises against 0 and 7 -- both
        // are commonly taken -- but they are legal settings of a real switch, so they
        // are legal here; the help says so rather than the validator.
        Property x;
        x.name = "restart";
        x.help = "INT switch: which RST the card jams on acknowledge, 0-7 (vector 8n). "
                 "0 and 7 are legal but commonly taken by other devices";
        x.kind = Kind::Int;
        x.min  = 0;
        x.max  = 7;
        x.get  = [this] { return Value::ofInt(restart_); };
        x.set  = [this](const Value& v, std::string&) {
            restart_ = (uint8_t)v.i();
            return true;
        };
        p.push_back(std::move(x));
    }
    {
        // LIVE, read-only: no setter, so CONFIG SAVE skips it. The guest sets this
        // clock by programming the chip, not by a monitor SET.
        Property x;
        x.name = "time";
        x.help = "LIVE: the date/time the MSM5832 is showing, and its offset from host time";
        x.kind = Kind::Str;
        x.get  = [this] { return Value::ofStr(rtc_.describe()); };
        p.push_back(std::move(x));
    }
    return p;
}

std::vector<MapEntry> Rtc100Board::ioMap() const {
    return {
        {(uint32_t)portA(), (uint32_t)portA(), "read/write",
         "RTC-100 -- PIA A data/direction: digit address (low nibble) / digit data (high)"},
        {(uint32_t)ctrlA(), (uint32_t)ctrlA(), "read/write",
         "RTC-100 -- PIA A control: CA2 = clock Hold (low = stopped)"},
        {(uint32_t)portB(), (uint32_t)portB(), "read/write",
         "RTC-100 -- PIA B data/direction: bit 0 = clock Write strobe (low pulse)"},
        {(uint32_t)ctrlB(), (uint32_t)ctrlB(), "read/write",
         "RTC-100 -- PIA B control: CB2 = clock Read line"},
    };
}

}  // namespace altair
