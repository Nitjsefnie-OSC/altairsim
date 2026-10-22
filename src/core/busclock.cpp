#include "core/busclock.h"

#include <chrono>

namespace altair {
namespace {

constexpr uint64_t kNsPerTick = 1000000000ull / (uint64_t)BusClock::kHz;  // 500

// Paced, pump() moves the count to host time only when host time is this far ahead.
// The same slack as HostTicker's: a host's sleep can overshoot by a scheduler quantum,
// and that is not lateness worth moving the count for.
constexpr uint64_t kPacedSlackTicks = (uint64_t)BusClock::kHz / 10;  // 100 ms

uint64_t steadyNs() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// a * num / den without overflowing a * num: whole dens first, then the remainder.
uint64_t scale(uint64_t a, uint64_t num, uint64_t den) {
    return (a / den) * num + (a % den) * num / den;
}

}  // namespace

uint64_t BusClock::hostNs() const { return hostNs_ ? hostNs_() : steadyNs(); }

void BusClock::anchor(const Clock& clk, uint64_t tick) const {
    primed_     = true;
    free_       = clk.free();
    hz_         = clk.hz();
    anchorT_    = clk.now();
    anchorNs_   = hostNow();
    anchorTick_ = tick;
}

uint64_t BusClock::counted(const Clock& clk) const {
    if (free_) {
        uint64_t ns = hostNow();
        return anchorTick_ + (ns > anchorNs_ ? (ns - anchorNs_) / kNsPerTick : 0);
    }
    uint64_t t = clk.now();
    return anchorTick_ + (t > anchorT_ ? scale(t - anchorT_, (uint64_t)kHz, (uint64_t)hz_) : 0);
}

uint64_t BusClock::now(const Clock& clk) const {
    if (!primed_) anchor(clk, 0);
    // The CPU board changed its pacing or its crystal at T-state `at`: count up to
    // there the old way, and on from there the new way.
    if (clk.free() != free_ || clk.hz() != hz_) {
        uint64_t at = clk.rateChangedAt();
        if (at < anchorT_) at = anchorT_;
        if (at > clk.now()) at = clk.now();
        uint64_t tick = free_ ? counted(clk)
                              : anchorTick_ + scale(at - anchorT_, (uint64_t)kHz, (uint64_t)hz_);
        anchor(clk, tick);
        anchorT_ = at;
    }
    uint64_t n = counted(clk);
    if (n < last_) n = last_;
    last_ = n;
    return n;
}

uint64_t BusClock::tStateOf(uint64_t tick, const Clock& clk) const {
    uint64_t n  = now(clk);
    uint64_t t0 = clk.now();
    if (tick <= n) return t0;
    if (free_) {
        uint64_t dt = scale(tick - n, (uint64_t)hz_, (uint64_t)kHz);
        return t0 + (dt ? dt : 1);
    }
    // Paced: the first T-state at which counted() reaches `tick`. The division rounds
    // down, so step up the at most one T-state it can be short.
    uint64_t d  = tick - anchorTick_;
    uint64_t at = anchorT_ + scale(d, (uint64_t)hz_, (uint64_t)kHz);
    while (scale(at - anchorT_, (uint64_t)kHz, (uint64_t)hz_) < d) ++at;
    return at > t0 ? at : t0 + 1;
}

void BusClock::pump(const Clock& clk) {
    if (!primed_ || clk.free()) return;
    uint64_t n    = now(clk);
    uint64_t ns   = hostNow();
    uint64_t host = anchorTick_ + (ns > anchorNs_ ? (ns - anchorNs_) / kNsPerTick : 0);
    if (host > n + kPacedSlackTicks) {
        anchor(clk, host);
        last_ = host;
    }
}

void BusClock::power(const Clock& clk) {
    last_ = 0;
    anchor(clk, 0);
}

void BusClock::rebase(const Clock& clk) {
    anchor(clk, last_);
}

}  // namespace altair
