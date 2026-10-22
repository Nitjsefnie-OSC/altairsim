#pragma once
//
// SciTronics RTC-100 -- an S-100 battery-backed real-time clock/calendar.
// See docs/boards/scitronics-rtc100.md and
// reference/SciTronics RTC-100 Real-Time Clock.md (SciTronics Inc., Bethlehem PA,
// software (c) 1980, schematic dated 9-7-80).
//
// THE CLOCK CHIP IS NOT THIS CARD'S WORK. The RTC-100 carries an OKI MSM5832 --
// the same chip the CompuPro System Support 1 carries -- and chips/msm5832.h
// already models it, down to the battery-backed offset that survives RESET. That
// header says "the next card with an MSM5832 gets this for free"; this is that
// card, and it adds no clock logic of its own.
//
// WHAT THIS CARD IS, THEN, IS A 6821 PIA WITH A CLOCK BEHIND IT. Where the SS-1
// wires the MSM5832's command and data lines straight to two I/O ports, the
// RTC-100 puts a Motorola 6821 in between, and the guest drives the clock by
// manipulating PIA pins. That translation is this board's whole job:
//
//   base+0   PIA A -- data or direction register (CRA bit 2 picks which)
//              data: digit ADDRESS in the low nibble, digit DATA in the HIGH nibble
//              direction: 0FH = low nibble out / high nibble in (the read set-up)
//                         FFH = all eight out          (the write set-up)
//   base+1   PIA A control -- CA2 drives the clock's HOLD line
//              CA2 LOW  = Hold asserted = clock stopped, display frozen
//              CA2 HIGH = run
//   base+2   PIA B -- the clock's WRITE strobe: 0 = pulse on, 1 = pulse off
//   base+3   PIA B control -- CB2 drives the clock's READ line
//
// A READ THEREFORE COMES BACK IN THE HIGH NIBBLE, with the digit code the guest
// wrote still sitting in the low nibble -- the two nibbles face opposite ways
// through one 8-bit port. That is why every driver ever written for this card ends
// with `ANI 0F0H` and four `RRC`, and it is the single most characteristic thing
// about the board.
//
// ---------------------------------------------------------------------------
// ONLY AS MUCH PIA AS THE CLOCK NEEDS, AND SAID OUT LOUD.
//
// This is NOT a general 6821 (the 88-4PIO's 6820 file is closer to that). Modeled:
// the two control registers' DDR/data select (bit 2) and CA2/CB2 output level (bit
// 3, under the bits-5:4 = output-mode the card always uses), the two direction
// registers, and the two data registers. NOT modeled: CA1/CB1 inputs, the IRQA/IRQB
// flags and their read-clears, the pulse/handshake CA2 modes, and any input path on
// port B. Nothing on this card is wired to them -- the clock's Hold/Read/Write are
// the only loads -- so a guest that programs them observes nothing, which is what
// the docs/boards Limitations section says.
//
// ---------------------------------------------------------------------------
// THE INTERRUPT IS THE CARD'S OWN, AND IT IS NOT A VI STRAP.
//
// The RTC-100 requests an interrupt once per second and then VECTORS ITSELF: it
// claims the acknowledge cycle and jams `RST n` onto the data bus, where n comes
// from the 3-position INT dip switch (manual p8-9, Table II -- the switch is
// NEGATIVE logic, ON = 0). There is no VI connector on the card, so there is no
// vi0..vi7 strap here: `interrupt` is none|int, and `restart` is the switch. A
// board that offered eight VI levels would be a board that never existed.
//
// The second is REAL time -- the chip's own crystal, not the CPU's (issue #827) -- so
// the tick counts host time through a HostTicker (core/hostticker.h), the same way the
// 88-VI's RTC does it. The host clock is injected (setHostNs), and a test sets it.

#include "chips/msm5832.h"
#include "core/board.h"
#include "core/clock.h"
#include "core/hostticker.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace altair {

class Rtc100Board : public Board {
public:
    std::string type() const override { return "rtc100"; }

    bool    decodes(const BusCycle& c) const override;
    uint8_t read(const BusCycle& c) override;
    void    write(const BusCycle& c) override;

    // Pin 73 only. The card has no VI wire, so assertsVi() is left at the base's 0.
    bool assertsInt() const override { return intWired_ && intReq_; }

    void reset(Reset) override;
    void power() override;

    // Once a slice: is the next second due in host time?
    void pump() override { tick_.pump(clock_); }

    // The HOST clock the seconds count in, in nanoseconds. Defaults to steady_clock;
    // a test drives it.
    void setHostNs(std::function<uint64_t()> f) { tick_.setHostNs(std::move(f)); }

    void serialize(StateWriter& w) const override;
    void deserialize(StateReader& r) override;

    std::vector<Property> properties() override;
    std::vector<MapEntry> ioMap() const override;

    // ---- for tests: the live clock without going through the bus ----
    const Msm5832& clockChip() const { return rtc_; }

private:
    uint8_t portA() const { return (uint8_t)(base_ + 0); }
    uint8_t ctrlA() const { return (uint8_t)(base_ + 1); }
    uint8_t portB() const { return (uint8_t)(base_ + 2); }
    uint8_t ctrlB() const { return (uint8_t)(base_ + 3); }

    // Push the current PIA pin state at the clock chip. Called after anything that
    // could move Hold, the digit select, or the write strobe -- the chip's command
    // register is edge-sensitive on Hold, so it must see every transition exactly once.
    void driveClock(bool writeStrobe);

    void scheduleTick();       // restart the seconds at host-now, or stop them if unwired
    void onTick();             // the clock chip's seconds output
    void dropRequest();        // clear the request and settle pin 73

    Msm5832 rtc_;

    uint8_t base_ = 0xF0;  // multiple of 4; A0/A1 are the card's own function select

    // ---- the modeled slice of the 6821 ----
    uint8_t cra_  = 0;     // control A: bit 2 = data/DDR select, bit 3 = CA2 level
    uint8_t crb_  = 0;     // control B: same shape, bit 3 = CB2 level
    uint8_t ddra_ = 0;     // direction A (0FH in the read set-up, FFH in the write one)
    uint8_t ddrb_ = 0;     // direction B
    uint8_t oraa_ = 0;     // output register A -- digit address low, digit data high
    uint8_t orb_  = 1;     // output register B -- bit 0 is the write strobe, idle high

    // ---- the once-a-second interrupt ----
    bool          intWired_ = false;        // `interrupt` strap: none | int (pin 73)
    uint8_t       restart_  = 7;            // INT switch: RST 0..7, vector 8n
    bool          intReq_   = false;        // the request, pending until acknowledged
    HostTicker    tick_;                    // the seconds, in host time
};

}  // namespace altair
