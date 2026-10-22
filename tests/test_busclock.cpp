// The bus CLOCK, pin 49 (core/busclock.h, issue #836).
//
// CLOCK is 2 MHz of real time from every CPU board. What is tested here is the one
// property every board that divides it rests on: the count is real time in BOTH run
// modes. Paced, it is the T-states (deterministic, exact); flat out, it is host time,
// and a million T-states do not move it.

#include "core/busclock.h"
#include "core/clock.h"
#include "test.h"

#include <cstdint>

using namespace altair;

namespace {

// A BusClock on a Clock, with the host's time in a variable the test moves.
struct Rig {
    Clock    clk;
    BusClock bc;
    uint64_t ns = 1000000000;  // an arbitrary host time; only differences matter

    explicit Rig(long long hz) {
        clk.setHz(hz);  // 0 = flat out
        bc.setHostNs([this] { return ns; });
        bc.power(clk);
    }
};

}  // namespace

void test_busclock() {
    SECTION("BusClock -- pin 49 is 2 MHz of real time, paced or flat out");

    // ---- Flat out: host time, not T-states ----
    {
        Rig r(0);
        CHECK(r.bc.now(r.clk) == 0, "flat out: the count starts at zero at power");
        r.clk.advance(1000000);
        CHECK(r.bc.now(r.clk) == 0, "flat out: a million T-states do not move CLOCK");
        r.ns += 1000000;  // 1 ms
        CHECK(r.bc.now(r.clk) == 2000, "flat out: 1 ms of host time is 2000 ticks");
        CHECK(r.clk.now() < r.bc.tStateOf(2001, r.clk),
              "flat out: the deadline for a future tick is after now (the HLT keep-alive)");
    }

    // ---- Paced: T-states, exactly, at any processor speed ----
    {
        Rig r(4000000);
        r.clk.advance(4000);
        CHECK(r.bc.now(r.clk) == 2000, "paced at 4 MHz: 4000 T-states are 2000 ticks");
        r.ns += 5000000000ull;  // the host says 5 s went by; between slices, nobody asks it
        CHECK(r.bc.now(r.clk) == 2000, "paced: a read does not look at host time");
        CHECK(r.bc.tStateOf(2001, r.clk) == 4002, "paced at 4 MHz: tick 2001 is T-state 4002");
    }
    {
        Rig r(1000000);  // slower than CLOCK: half a T-state per tick, never 0
        r.clk.advance(1000);
        CHECK(r.bc.now(r.clk) == 2000, "paced at 1 MHz: 1000 T-states are 2000 ticks");
        CHECK(r.bc.tStateOf(2001, r.clk) == 1001, "paced at 1 MHz: tick 2001 needs T-state 1001");
    }
    {
        Rig r(3000000);  // not a whole number of T-states per tick
        r.clk.advance(3);
        CHECK(r.bc.now(r.clk) == 2, "paced at 3 MHz: 3 T-states are 2 ticks");
        uint64_t at = r.bc.tStateOf(3, r.clk);
        CHECK(at == 5, "paced at 3 MHz: tick 3 arrives at T-state 5 (4.5 rounded up)");
    }

    // ---- Paced, the machine sat at the monitor: pump() catches up to host time ----
    {
        Rig r(2000000);
        r.clk.advance(2000);
        r.ns += 50000000;  // 50 ms: inside the slack, a host sleep overshooting
        r.bc.pump(r.clk);
        CHECK(r.bc.now(r.clk) == 2000, "paced: pump leaves the count alone 50 ms behind");
        r.ns += 950000000;  // 1 s since power, 1 ms of T-states
        r.bc.pump(r.clk);
        CHECK(r.bc.now(r.clk) == 2000000, "paced: pump moves the count to host time (1 s)");
        r.clk.advance(2000);
        CHECK(r.bc.now(r.clk) == 2002000, "paced: and counts T-states on from there");
    }

    // ---- A mode or crystal change keeps the count where it stands ----
    {
        Rig r(2000000);
        r.clk.advance(1000);
        CHECK(r.bc.now(r.clk) == 1000, "paced at 2 MHz: 1000 ticks");
        r.clk.setHz(0);
        r.clk.advance(1000000);
        CHECK(r.bc.now(r.clk) == 1000, "to flat out: the count holds, and T-states stop counting");
        r.ns += 500;
        CHECK(r.bc.now(r.clk) == 1001, "flat out: host time counts on from it");
        r.clk.setHz(4000000);
        CHECK(r.bc.now(r.clk) == 1001, "back to paced: the count holds");
        r.clk.advance(8);
        CHECK(r.bc.now(r.clk) == 1005, "paced at the new 4 MHz: 8 T-states are 4 ticks");
    }

    // ---- Power and restore ----
    {
        Rig r(2000000);
        r.clk.advance(500);
        CHECK(r.bc.now(r.clk) == 500, "500 ticks");
        r.clk.power();
        r.bc.power(r.clk);
        CHECK(r.bc.now(r.clk) == 0, "power restarts the count at zero");
        r.clk.advance(300);
        CHECK(r.bc.now(r.clk) == 300, "300 ticks");
        r.clk.power();
        r.clk.advance(7000);  // a RESTORE sets the clock to the snapshot's time
        r.bc.rebase(r.clk);
        r.clk.advance(100);
        CHECK(r.bc.now(r.clk) == 400, "rebase keeps the count and goes on from the new T-state");
    }
}
