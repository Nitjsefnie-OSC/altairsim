#pragma once
//
// HostTicker -- a periodic tick that keeps REAL time, delivered into the T-state stream
// (DESIGN.md, the Clock section; issues #825 and #827).
//
// A board whose tick comes from something other than the CPU's crystal -- the 60 Hz
// mains, the bus's 2 MHz CLOCK, a clock chip's own 32.768 kHz crystal -- must not count
// that tick in T-states. Flat out (clock_hz = 0, the default) T-states come as fast as
// the host runs the guest, and a clock counted in them runs fast with it. So the
// schedule is kept in HOST nanoseconds, read through an injected function, and a test
// sets the time.
//
// The tick still has to land at some T-state, and it lands by one of two routes:
//   * PACED (clock_hz set): the run loop holds T-states to real time, so a Clock
//     deadline `period x hz` T-states on fires it at the exact instruction, with no
//     jitter. hz is only the exchange rate the throttle promises.
//   * FLAT OUT, OR LATE: pump() reads host time once a slice and fires the tick when
//     the schedule is due. Flat out, a slice is microseconds of host time. Paced, this
//     only fires when the host fell well behind its crystal, or the machine sat
//     stopped at the monitor while the real source kept counting.
// Flat out, the Clock deadline is still armed -- only so a guest that HALTs to wait
// for the tick is not taken for finished (see start()).
//
// TWO BOARDS NEED THIS: the 88-VI/RTC's divider chain and the RTC-100's seconds
// output. Same arithmetic, so it is written once. The ticker knows about Clock and
// nothing else; what a tick DOES is the board's `fire` callback.
//
// The Clock is passed in on every call rather than kept, because a board's clock_ is
// the pointer Clock::watch() clears when the clock goes away.

#include "core/clock.h"

#include <cstdint>
#include <functional>

namespace altair {

class HostTicker {
public:
    // The HOST clock, in nanoseconds. Defaults to steady_clock; a test drives it.
    void setHostNs(std::function<uint64_t()> f) { hostNs_ = std::move(f); }

    // Restart at host-now: one tick every `multiple` periods of `rateHz`, the first one
    // interval from now. Cancels any deadline already armed, then arms a new one.
    //
    // A rate and a multiple, not one period in hertz, because the 88-VI/RTC's divider
    // makes fractional rates (60 Hz / 1000 = 0.06 Hz) that would round to nothing.
    // Clock::tStatesPer(rateHz) gives the base tick exactly and the multiple is then
    // just multiplication.
    void start(Clock* clock, long long rateHz, uint64_t multiple, std::function<void()> fire);

    // Cancel the deadline. A board stops the ticker whenever its tick cannot be
    // observed: an always-armed periodic timer leaves Clock::queued() permanently
    // non-zero, which is one of the two things the run loop uses to decide a HLT has
    // finished (core/debug.cpp). A machine with an unwired tick would never stop on a
    // HLT again.
    void stop(Clock* clock);

    bool running() const { return wake_ != Clock::kNone; }

    // Once a slice: is the host-time schedule due? See the header note.
    void pump(Clock* clock);

private:
    void     onDeadline(Clock* clock);
    void     arm(Clock* clock, uint64_t tStates);
    uint64_t period(const Clock* clock) const;  // one interval, in T-states at the clock's hz
    uint64_t hostNow() const;
    void     setDue();               // hostDueNs_ from anchorNs_ and ticks_
    void     passed(uint64_t now);   // move the schedule to the first tick after now

    // One interval is num() / den() nanoseconds: multiple x 1e9 over the rate.
    uint64_t num() const { return multiple_ * 1000000000ull; }
    uint64_t den() const { return (uint64_t)rateHz_; }

    std::function<uint64_t()> hostNs_;
    std::function<void()>     fire_;
    long long                 rateHz_   = 1;
    uint64_t                  multiple_ = 1;

    Clock::Handle wake_ = Clock::kNone;

    // The schedule, in host nanoseconds. Tick k is at anchorNs_ + k * num() / den(),
    // computed fresh each time so it cannot drift; setDue() folds whole nanoseconds
    // into the anchor to keep k small. Host time does not travel in a snapshot: a board
    // restores by starting the ticker again.
    uint64_t anchorNs_  = 0;
    uint64_t ticks_     = 0;
    uint64_t hostDueNs_ = 0;
};

}  // namespace altair
