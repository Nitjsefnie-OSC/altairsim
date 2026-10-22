#include "core/hostticker.h"

#include <algorithm>
#include <chrono>
#include <numeric>

namespace altair {
namespace {

// Paced, pump() leaves a tick to the Clock deadline unless host time is this far past
// the schedule. A host's sleep can overshoot by a whole scheduler quantum (15.6 ms on
// Windows), and that is not lateness worth moving a tick off its T-state for.
constexpr uint64_t kPacedSlackNs = 100000000;  // 100 ms

uint64_t steadyNs() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

}  // namespace

uint64_t HostTicker::hostNow() const { return hostNs_ ? hostNs_() : steadyNs(); }

uint64_t HostTicker::period(const Clock* clock) const {
    return clock->tStatesPer(rateHz_) * multiple_;
}

// hostDueNs_ = tick number ticks_. Every den/g ticks are a whole number of nanoseconds
// (num/g), so those fold into the anchor: the schedule is exact forever and the
// products below stay far from overflow.
void HostTicker::setDue() {
    const uint64_t n    = num();
    const uint64_t d    = den();
    const uint64_t g    = std::gcd(n, d);
    const uint64_t step = d / g;
    anchorNs_ += (ticks_ / step) * (n / g);
    ticks_ %= step;
    hostDueNs_ = anchorNs_ + ticks_ * n / d;
}

// Ticks between the last one we delivered and `now` are gone: what a tick sets on a
// board is a latch, and it was set once. The schedule moves to the first tick AFTER now.
void HostTicker::passed(uint64_t now) {
    const uint64_t n       = num();
    const uint64_t d       = den();
    const uint64_t elapsed = now - anchorNs_;
    ticks_ = (elapsed / n) * d + (elapsed % n) * d / n + 1;
    setDue();
}

void HostTicker::arm(Clock* clock, uint64_t tStates) {
    if (tStates) wake_ = clock->after(tStates, [this, clock] { onDeadline(clock); });
}

// Cancel and re-arm -- the idiom every clocked board here uses (see Sio2Board::refresh).
//
// The deadline is armed flat out too, where it does not decide when the tick lands. A
// guest that HALTs to wait for its clock needs Clock::queued() non-zero, or the run
// loop takes the HLT for the end of the run.
void HostTicker::start(Clock* clock, long long rateHz, uint64_t multiple,
                       std::function<void()> fire) {
    if (!clock) return;
    stop(clock);
    rateHz_   = rateHz;
    multiple_ = multiple;
    fire_     = std::move(fire);
    anchorNs_ = hostNow();
    ticks_    = 1;
    setDue();
    arm(clock, period(clock));
}

void HostTicker::stop(Clock* clock) {
    if (clock) clock->cancel(wake_);
    wake_ = Clock::kNone;
}

// PACED, the deadline IS the tick: T-states are real time here, and the tick it stands
// for is the one the schedule has next. FLAT OUT it only keeps the machine alive
// (start), so it looks at the host's time and goes back to sleep.
void HostTicker::onDeadline(Clock* clock) {
    wake_ = Clock::kNone;
    if (!clock->free()) {
        fire_();
        ++ticks_;
        setDue();
    } else {
        uint64_t now = hostNow();
        if (now >= hostDueNs_) {
            fire_();
            passed(now);
        }
    }
    arm(clock, period(clock));
}

// Once a slice. Flat out, this is where the tick lands: the first slice that finds the
// schedule due. Paced, the Clock deadline delivers it, and this fires only when host
// time got well ahead of the T-states -- a host that cannot hold its crystal, or a
// machine that sat stopped at the monitor -- and then puts the deadline where host
// time says the next tick is.
void HostTicker::pump(Clock* clock) {
    if (wake_ == Clock::kNone || !clock) return;
    const bool     paced = !clock->free();
    const uint64_t now   = hostNow();
    const uint64_t slack = paced ? std::max<uint64_t>(num() / den(), kPacedSlackNs) : 0;
    if (now < hostDueNs_ + slack) return;
    fire_();
    passed(now);
    if (!paced) return;  // the keep-alive deadline stays where it is
    clock->cancel(wake_);
    wake_ = Clock::kNone;
    const double ahead = (double)(hostDueNs_ - now) * 1e-9 * (double)clock->hz();
    arm(clock, std::max<uint64_t>(1, (uint64_t)ahead));
}

}  // namespace altair
