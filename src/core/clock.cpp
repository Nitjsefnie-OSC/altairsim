#include "core/clock.h"

#include "core/statefile.h"

#include <algorithm>
#include <atomic>

namespace altair {

// One counter for every clock in the process -- see Handle in the header. Pre-incremented,
// so kNone (0) is never issued. The machine is single-threaded; the atomic only makes that
// not a precondition of a number's uniqueness.
static std::atomic<Clock::Handle> nextHandle{0};

Clock::Handle Clock::at(uint64_t when, std::function<void()> fn) {
    Handle h = nextHandle.fetch_add(1, std::memory_order_relaxed) + 1;
    live_.emplace(h, std::move(fn));
    heap_.push_back(Item{when, h});
    std::push_heap(heap_.begin(), heap_.end(), std::greater<Item>());
    return h;
}

Clock::~Clock() {
    for (Clock** p : watchers_) *p = nullptr;
}

void Clock::watch(Clock** p) {
    if (std::find(watchers_.begin(), watchers_.end(), p) == watchers_.end())
        watchers_.push_back(p);
}

void Clock::unwatch(Clock** p) {
    watchers_.erase(std::remove(watchers_.begin(), watchers_.end(), p), watchers_.end());
}

void Clock::cancel(Handle h) {
    // The heap entry stays as a tombstone. It is skipped when it surfaces, which
    // costs one hash lookup at a moment we were about to do one anyway -- cheaper
    // than a linear search of the heap right now, and this is called on every
    // character a UART sends.
    live_.erase(h);
}

bool Clock::pending(Handle h) const { return live_.find(h) != live_.end(); }

// One instruction retired. Run time forward to where the CPU left it, stopping at
// each thing that comes due along the way.
//
// TIME IS THE EVENT'S TIME WHILE THE EVENT RUNS. now() inside a callback reports
// WHEN THE THING ACTUALLY HAPPENED, not where the instruction that carried us past
// it happened to end. An instruction is up to 17 T-states long and a board's
// deadline lands wherever it lands inside that; a board that asks the time and is
// told the wrong one -- or that schedules `now() + charTime` and quietly drifts by
// up to 17 T-states per character -- is a bug that would take a very long time to
// see. The clock is the one thing in this simulator that must never lie about the
// time.
//
// A callback MAY schedule more work -- a UART re-arming for the next character does
// exactly that -- and if it schedules something already due, that fires in this same
// drain. Which is correct, and is also precisely how you would write an infinite
// loop: a board that re-arms at now() every time never lets go. So boards schedule
// STRICTLY IN THE FUTURE, and it is the board's job to guarantee it (see
// Mc6850::nextEdge, which is written to).
void Clock::advance(uint64_t dt) {
    const uint64_t target = t_ + dt;

    while (!heap_.empty() && heap_.front().when <= target) {
        Item top = heap_.front();
        std::pop_heap(heap_.begin(), heap_.end(), std::greater<Item>());
        heap_.pop_back();

        auto it = live_.find(top.h);
        if (it == live_.end()) continue;  // cancelled; the handle outlived the job

        // Move the function out and erase BEFORE calling it. The callback is
        // entitled to schedule, cancel, or destroy things -- including, in the
        // ordinary case, to re-arm itself -- and it must not find its own corpse
        // still in the table when it does.
        auto fn = std::move(it->second);
        live_.erase(it);

        if (top.when > t_) t_ = top.when;  // ...and an overdue event does not rewind it
        fn();
    }

    t_ = target;  // and the instruction ends where the CPU said it ended
}

void Clock::power() {
    t_      = 0;
    rateAt_ = 0;
    heap_.clear();
    live_.clear();
}

void Clock::serialize(StateWriter& w) const {
    w.u64(t_);
    w.u64(nextHandle.load(std::memory_order_relaxed));
}

void Clock::deserialize(StateReader& r) {
    // CLEAR THE QUEUE FIRST, and this is load-bearing. RESTORE applies onto a live
    // machine whose queue holds deadlines scheduled against the OLD time. Once t_
    // jumps to the snapshot's value those are all in the past and would fire in a
    // storm on the next advance(); worse, a handle re-armed by a board could collide
    // with a stale one still sitting here. So the queue is emptied and every board
    // that had a deadline re-arms it in its own deserialize() -- into a clean queue,
    // against the restored time.
    heap_.clear();
    live_.clear();
    t_    = r.u64();
    // Forward only. Moving it back would re-issue numbers handed out since the snapshot,
    // which a board may still hold and is about to cancel.
    Handle saved = r.u64();
    Handle cur   = nextHandle.load(std::memory_order_relaxed);
    while (saved > cur && !nextHandle.compare_exchange_weak(cur, saved)) {}
}

} // namespace altair
