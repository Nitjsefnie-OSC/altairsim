#pragma once
//
// BusClock -- the S-100 bus CLOCK, pin 49 (DESIGN.md 7.5; issues #825 and #836).
//
// Every CPU board supplies CLOCK at 2 MHz, whatever its processor runs at. The CPU's
// crystal does not leave the CPU board; this does. So CLOCK is REAL time, and a board
// that counts it -- the CompuPro SS-1's 8253, the 88-VI/RTC's 10 kHz divider -- must
// not count it in T-states. Flat out (clock_hz = 0, the default) T-states come as fast
// as the host runs the guest, and a counter clocked by them runs fast with it.
//
// ONE PER MACHINE, ON THE BUS. Pin 49 is one wire that every board sees, so it is
// modelled once, and every board that divides it reads the same count. A board that
// takes its clock from somewhere else -- its own crystal, the mains, a J4 jumper --
// does not use this.
//
// The count is in 2 MHz ticks, and it comes from one of two places:
//   * PACED (clock_hz set): T-states. The run loop holds them to real time, so
//     `kHz / hz` ticks per T-state is real time, exactly, at every processor speed --
//     and deterministic, because it is a function of the instruction stream.
//   * FLAT OUT: host time, read through an injected function (a test sets it).
// A switch between the two, or a new hz, re-anchors the count where it stands, so it
// never jumps and never runs backwards. Paced, pump() also moves the count forward to
// host time when the host fell well behind its crystal or the machine sat stopped at
// the monitor, as HostTicker::pump() does for a tick. That happens only in pump(), so
// two reads in one slice always agree.
//
// Nothing is scheduled here. A board turns the tick it wants into a T-state with
// tStateOf() and arms its own Clock deadline, and re-samples in its pump().

#include "core/clock.h"

#include <cstdint>
#include <functional>

namespace altair {

class BusClock {
public:
    static constexpr long long kHz = 2000000;  // pin 49

    // The HOST clock, in nanoseconds. Defaults to steady_clock; a test drives it.
    void     setHostNs(std::function<uint64_t()> f) { hostNs_ = std::move(f); }
    uint64_t hostNs() const;

    // Ticks of CLOCK since power. Monotonic. Const because reading the wire changes
    // nothing a guest can see; the re-anchoring it may do is bookkeeping.
    uint64_t now(const Clock& clk) const;

    // The T-state at which tick `tick` arrives. Paced, exact. Flat out, an estimate at
    // the clock's hz: a deadline that keeps a HLT waiting for the tick from being taken
    // for the end of the run (see HostTicker::start), not the moment the tick lands --
    // the board's pump() sees that. Always later than clk.now() for a future tick.
    uint64_t tStateOf(uint64_t tick, const Clock& clk) const;

    // Once a slice. Paced, move the count up to host time when host time is well ahead.
    void pump(const Clock& clk);

    // Power applied: the count restarts at 0.
    void power(const Clock& clk);

    // The clock's time was just set (RESTORE). Keep the count, anchor it to the new
    // T-state and host time. Host time does not travel in a snapshot.
    void rebase(const Clock& clk);

private:
    uint64_t hostNow() const { return hostNs(); }
    uint64_t counted(const Clock& clk) const;               // under the current anchor
    void     anchor(const Clock& clk, uint64_t tick) const;  // re-anchor at `tick`

    std::function<uint64_t()> hostNs_;

    // The anchor: at T-state anchorT_ and host time anchorNs_, the count was anchorTick_,
    // and it was counted in host time (free_) or in T-states at hz_.
    mutable bool      primed_     = false;
    mutable bool      free_       = true;
    mutable long long hz_         = kHz;
    mutable uint64_t  anchorT_    = 0;
    mutable uint64_t  anchorNs_   = 0;
    mutable uint64_t  anchorTick_ = 0;
    mutable uint64_t  last_       = 0;  // the highest count handed out: never go back
};

}  // namespace altair
