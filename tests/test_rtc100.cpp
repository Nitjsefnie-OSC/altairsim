// The SciTronics RTC-100 -- an MSM5832 behind a 6821 PIA.
//
// THE MANUAL'S SHIPPED DRIVERS ARE THE ORACLE HERE, NOT ITS PROSE. The "sequence to
// enable clock read" printed on p5 writes 248 to base+3 and orders the last two writes
// the other way round; both programs SciTronics shipped -- READ.ASM (Appendix V) and
// the North Star RTCREAD (Appendix VI) -- write 252 and then 244, and so does the
// pencil correction in the margin of the scan. These tests drive the sequence the
// LISTINGS use, byte for byte, because that is the one that ran on the hardware.
// See reference/SciTronics RTC-100 Real-Time Clock.md sections 4 and 9.
//
// Everything goes through real bus cycles, as a guest program would.

#include "test.h"

#include "boards/scitronics-rtc100.h"
#include "core/clock.h"
#include "core/machine.h"
#include "core/statefile.h"
#include "platform/localtime.h"

#include <cstdint>
#include <cstdlib>
#include <ctime>

using namespace altair;

namespace {

constexpr uint8_t kBase = 0xF0;

uint8_t pA(uint8_t b) { return (uint8_t)(b + 0); }
uint8_t cA(uint8_t b) { return (uint8_t)(b + 1); }
uint8_t pB(uint8_t b) { return (uint8_t)(b + 2); }
uint8_t cB(uint8_t b) { return (uint8_t)(b + 3); }

// READ.ASM's TSTART (manual p34), byte for byte:
//   F0H -> base+1 ; 0FH -> base+0 ; delay ; FCH -> base+3 ; F4H -> base+1
void enableRead(Machine& m, uint8_t b) {
    m.bus.ioWrite(cA(b), 0xF0);
    m.bus.ioWrite(pA(b), 0x0F);  // DDRA: low nibble out (address), high nibble in (data)
    m.bus.ioWrite(cB(b), 0xFC);
    m.bus.ioWrite(cA(b), 0xF4);
}

// READ.ASM's TIMED (p34): back to run mode.
void runMode(Machine& m, uint8_t b) {
    m.bus.ioWrite(cA(b), 0xF8);
    m.bus.ioWrite(pA(b), 0x0F);
    m.bus.ioWrite(cB(b), 0xF8);
    m.bus.ioWrite(cA(b), 0xFC);
    m.bus.ioWrite(pA(b), 0x0F);
}

// GETDIG1 (p35): OUT the digit code, IN the port, drop the code, rotate the data down.
uint8_t readDigit(Machine& m, uint8_t b, uint8_t sel) {
    m.bus.ioWrite(pA(b), sel);
    return (uint8_t)((m.bus.ioRead(pA(b)) & 0xF0) >> 4);
}

// The enable-set sequence (manual p6): both ports to output, Hold up, read off.
void enableSet(Machine& m, uint8_t b) {
    m.bus.ioWrite(cA(b), 0xF4);  // select the data registers first...
    m.bus.ioWrite(cB(b), 0xF4);
    m.bus.ioWrite(cA(b), 0xF0);  // ...then the direction registers, to preset them
    m.bus.ioWrite(cB(b), 0xF0);
    m.bus.ioWrite(pA(b), 0xFF);  // DDRA: all eight out
    m.bus.ioWrite(pB(b), 0xFF);  // DDRB: all eight out
    m.bus.ioWrite(cA(b), 0xF4);  // data registers, CA2 low = Hold asserted
    m.bus.ioWrite(cB(b), 0xF4);
}

// One digit: address in the low nibble, data in the high nibble, then strobe PB0 low
// and back high (manual p7).
void writeDigit(Machine& m, uint8_t b, uint8_t sel, uint8_t val) {
    m.bus.ioWrite(pA(b), (uint8_t)((val << 4) | (sel & 0x0F)));
    m.bus.ioWrite(pB(b), 0x00);
    m.bus.ioWrite(pB(b), 0x01);
}

// Program a whole date/time, the way RTCSET does. Seconds are attempted and must be
// ignored by the chip.
void setClock(Machine& m, uint8_t b, int y2, int mo, int d, int h, int mi) {
    enableSet(m, b);
    writeDigit(m, b, 0, 9);  // seconds units -- write-ignored
    writeDigit(m, b, 1, 5);  // seconds tens  -- write-ignored
    writeDigit(m, b, 2, (uint8_t)(mi % 10));
    writeDigit(m, b, 3, (uint8_t)(mi / 10));
    writeDigit(m, b, 4, (uint8_t)(h % 10));
    writeDigit(m, b, 5, (uint8_t)(h / 10));
    writeDigit(m, b, 7, (uint8_t)(d % 10));
    writeDigit(m, b, 8, (uint8_t)(d / 10));
    writeDigit(m, b, 9, (uint8_t)(mo % 10));
    writeDigit(m, b, 10, (uint8_t)(mo / 10));
    writeDigit(m, b, 11, (uint8_t)(y2 % 10));
    writeDigit(m, b, 12, (uint8_t)(y2 / 10));
    m.bus.ioWrite(cA(b), 0xF8);  // CA2 high = drop Hold = the set is applied
}

int secOf(Machine& m, uint8_t b) {
    return (readDigit(m, b, 1) & 0x0F) * 10 + (readDigit(m, b, 0) & 0x0F);
}
int minOf(Machine& m, uint8_t b) {
    return (readDigit(m, b, 3) & 0x0F) * 10 + (readDigit(m, b, 2) & 0x0F);
}
int hourOf(Machine& m, uint8_t b) {
    return (readDigit(m, b, 5) & 0x03) * 10 + (readDigit(m, b, 4) & 0x0F);
}
int dayOf(Machine& m, uint8_t b) {
    return (readDigit(m, b, 8) & 0x03) * 10 + (readDigit(m, b, 7) & 0x0F);
}
int monOf(Machine& m, uint8_t b) {
    return (readDigit(m, b, 10) & 0x0F) * 10 + (readDigit(m, b, 9) & 0x0F);
}
int year2Of(Machine& m, uint8_t b) {
    return (readDigit(m, b, 12) & 0x0F) * 10 + (readDigit(m, b, 11) & 0x0F);
}

Rtc100Board* attach(Machine& m, uint8_t base = kBase) {
    auto* b = new Rtc100Board();
    b->id   = "rtc100";
    m.bus.attach(b);
    if (base != kBase) {
        std::string err;
        for (Property& p : b->properties())
            if (p.name == "port") CHECK(p.set(Value::ofInt(base), err), "the base moved");
    }
    return b;
}

void setProp(Rtc100Board* b, const std::string& name, const Value& v) {
    std::string err;
    for (Property& p : b->properties())
        if (p.name == name) CHECK(p.set(v, err), "the property was accepted");
}

}  // namespace

void test_rtc100() {
    SECTION("SciTronics RTC-100 -- MSM5832 behind a 6821 PIA");

    // ---- READ.ASM's own sequence reads the host wall clock ----
    {
        Machine m;
        attach(m);

        enableRead(m, kBase);
        int sec = secOf(m, kBase), mi = minOf(m, kBase), h = hourOf(m, kBase);
        int d = dayOf(m, kBase), mo = monOf(m, kBase), y2 = year2Of(m, kBase);
        runMode(m, kBase);

        platform::CalendarTime host = platform::localCalendar(std::time(nullptr));
        std::tm tm{};
        tm.tm_sec   = sec;
        tm.tm_min   = mi;
        tm.tm_hour  = h;
        tm.tm_mday  = d;
        tm.tm_mon   = mo - 1;
        tm.tm_year  = ((host.year / 100) * 100 + y2) - 1900;
        tm.tm_isdst = -1;
        long long chip = (long long)std::mktime(&tm);
        long long now  = (long long)std::time(nullptr);
        CHECK(std::llabs(chip - now) <= 2,
              "READ.ASM's TSTART + GETDIG1 sequence reads the host wall time");
    }

    // ---- the digit comes back in the HIGH nibble, the code stays in the low one ----
    {
        Machine m;
        attach(m);
        enableRead(m, kBase);

        m.bus.ioWrite(pA(kBase), 0x06);  // digit 6 = day of the week (0-6)
        uint8_t raw = m.bus.ioRead(pA(kBase));
        CHECK((raw & 0x0F) == 0x06,
              "the low nibble reads back the digit code the guest wrote (output pins)");
        CHECK((raw >> 4) <= 6, "the high nibble carries the clock digit (input pins)");

        platform::CalendarTime host = platform::localCalendar(std::time(nullptr));
        CHECK((raw >> 4) == (uint8_t)host.weekday, "and it is the host's day of the week");
    }

    // ---- the direction register reads back, and gates which nibble is which ----
    {
        Machine m;
        attach(m);
        m.bus.ioWrite(cA(kBase), 0xF0);  // CRA bit 2 clear -> base+0 is the DDR
        m.bus.ioWrite(pA(kBase), 0x0F);
        CHECK(m.bus.ioRead(pA(kBase)) == 0x0F, "DDRA reads back what was written");
        CHECK(m.bus.ioRead(cA(kBase)) == 0xF0, "the control register reads back");
    }

    // ---- setting the clock: exact readback, seconds forced to zero ----
    {
        Machine m;
        attach(m);

        // 2032 is a leap year, so Feb 29 exercises the leap bit and the compose path.
        setClock(m, kBase, /*y2*/ 32, /*mo*/ 2, /*d*/ 29, /*h*/ 13, /*mi*/ 45);
        enableRead(m, kBase);

        CHECK(year2Of(m, kBase) == 32, "the year reads back what RTCSET wrote");
        CHECK(monOf(m, kBase) == 2, "the month reads back");
        CHECK(dayOf(m, kBase) == 29, "the day reads back (Feb 29 in a leap year)");
        CHECK(hourOf(m, kBase) == 13, "the hour reads back");
        CHECK(minOf(m, kBase) == 45, "the minute reads back");
        CHECK(secOf(m, kBase) <= 2,
              "the seconds are forced to 0 on a set -- the written 59 is ignored");

        // The two digits every driver has to mask (the reference's section 3).
        CHECK((readDigit(m, kBase, 5) & 0x08) != 0,
              "Hours-10 carries the 24-hour flag above the digit -- hence ANI 3");
        CHECK((readDigit(m, kBase, 8) & 0x04) != 0,
              "Days-10 carries the leap-year flag above the digit -- hence ANI 3");
        runMode(m, kBase);
    }

    // ---- battery backup: the time survives RESET and a power cycle ----
    {
        Machine m;
        auto*   b = attach(m);

        setClock(m, kBase, 32, 2, 29, 13, 45);
        b->reset(Reset::Bus);
        enableRead(m, kBase);
        CHECK(year2Of(m, kBase) == 32, "a front-panel RESET does not lose the time");
        b->power();
        enableRead(m, kBase);
        CHECK(year2Of(m, kBase) == 32,
              "power-on does not lose the time -- the lithium cell kept it");
    }

    // ---- the once-a-second interrupt, vectored by the card itself ----
    {
        Machine m;
        auto*   b = attach(m);
        m.clock.setHz(2000000);
        b->attachClock(&m.clock);

        setProp(b, "interrupt", Value::ofStr("int"));
        setProp(b, "restart", Value::ofInt(2));

        CHECK(!b->assertsInt(), "no request before the first second has elapsed");
        m.clock.advance(m.clock.tStatesPer(1) + 1);
        CHECK(b->assertsInt(), "the clock requests an interrupt once a second");
        CHECK(b->assertsVi() == 0, "the card has no VI wire -- it drives pin 73 only");

        // The acknowledge: the card claims the cycle and jams RST 2 (0xD7 -> vector 16).
        uint8_t op = m.bus.intAck();
        CHECK(op == 0xD7, "the card jams RST 2 on acknowledge (restart = 2)");
        CHECK(!b->assertsInt(), "the acknowledge dismisses the request");

        m.clock.advance(m.clock.tStatesPer(1) + 1);
        CHECK(b->assertsInt(), "and the next second raises it again");
    }

    // ---- `interrupt = none` is an unsoldered wire, and timekeeping is unaffected ----
    {
        Machine m;
        auto*   b = attach(m);
        m.clock.setHz(2000000);
        b->attachClock(&m.clock);

        setProp(b, "interrupt", Value::ofStr("none"));
        m.clock.advance(m.clock.tStatesPer(1) * 3);
        CHECK(!b->assertsInt(), "an unsoldered strap never requests");

        enableRead(m, kBase);
        int y2 = year2Of(m, kBase);
        platform::CalendarTime host = platform::localCalendar(std::time(nullptr));
        CHECK(y2 == (host.year / 10) % 10 * 10 + host.year % 10,
              "STOPS stops the interrupt, not the timekeeping");
    }

    // ---- THE SECOND IS REAL TIME, NOT CPU TIME (issue #827). ----
    //
    // The MSM5832 divides its own 32.768 kHz crystal. Flat out (the default) T-states
    // come as fast as the host runs the guest, so they must move the tick not at all
    // -- and host time must move it. Both directions are asserted: the first alone
    // would pass against a dead tick, the second against the old one.
    struct HostRig {
        Machine      m;
        Rtc100Board* b      = nullptr;
        uint64_t     hostNs = 1000000000;
        explicit HostRig(long long hz) {
            b = attach(m);
            m.clock.setHz(hz);
            b->setHostNs([this] { return hostNs; });
            b->attachClock(&m.clock);
            setProp(b, "interrupt", Value::ofStr("int"));
        }
    };

    {
        HostRig g(0);
        for (int i = 0; i < 2000; ++i) {
            g.m.clock.advance(10000);
            if (i % 2 == 0) g.b->pump();
        }
        CHECK(!g.b->assertsInt(), "flat out, 20 million T-states move the seconds not at all");
        CHECK(g.m.clock.queued() != 0,
              "...but the deadline stays armed, so a guest that HALTs for its tick waits");

        g.hostNs += 999000000;
        g.b->pump();
        CHECK(!g.b->assertsInt(), "999 ms of real time: not yet");
        g.hostNs += 1000000;
        g.b->pump();
        CHECK(g.b->assertsInt(), "1000 ms of real time: one tick");

        // Ten seconds, slice by slice, at a fast host's speed. No drift.
        g.m.bus.intAck();
        int ticks = 1;
        for (int ms = 1000; ms < 10000; ++ms) {
            g.hostNs += 1000000;
            g.m.clock.advance(1000000);  // a millisecond is a million T-states here
            g.b->pump();
            if (g.b->assertsInt()) {
                ++ticks;
                g.m.bus.intAck();
            }
        }
        CHECK(ticks == 10, "ten ticks in ten seconds of real time, at any host speed");
    }

    // ---- PACED, the tick lands on its exact T-state. ----
    //
    // With a crystal set, the run loop holds T-states to real time, so the deadline
    // delivers the tick at the instruction it belongs to. Host time is held still to
    // prove it is the deadline doing it.
    for (long long hz : {2000000LL, 10000000LL}) {
        HostRig g(hz);
        g.m.clock.advance((uint64_t)hz - 1);
        g.b->pump();
        CHECK(!g.b->assertsInt(), "paced: one T-state short of a second, no tick");
        g.m.clock.advance(1);
        CHECK(g.b->assertsInt(), "paced: the tick lands on exactly hz T-states");
    }

    // ---- PACED, but the host fell behind its crystal: host time is the truth. ----
    //
    // pump() leaves a tick to the deadline until host time is a whole period past it
    // (or 100 ms, for a fast tick) -- here, one second.
    {
        HostRig g(2000000);
        g.hostNs += 1900000000;  // the first second was due at +1 s
        g.b->pump();
        CHECK(!g.b->assertsInt(), "paced, 0.9 s late: still the deadline's to deliver");
        g.hostNs += 200000000;
        g.b->pump();
        CHECK(g.b->assertsInt(), "paced, 1.1 s late: pump() delivers the tick");

        // Host-now is +2.1 s; the next real second is at +3 s -- 0.9 s of crystal.
        g.m.bus.intAck();
        g.m.clock.advance(1800000 - 1);
        CHECK(!g.b->assertsInt(), "...and re-aims the deadline at the next real second");
        g.m.clock.advance(1);
        CHECK(g.b->assertsInt(), "...which arrives 0.9 s of crystal later, not 1 s");
    }

    // ---- an unwired strap arms nothing, flat out or paced ----
    for (long long hz : {0LL, 2000000LL}) {
        HostRig g(hz);
        setProp(g.b, "interrupt", Value::ofStr("none"));
        CHECK(g.m.clock.queued() == 0, "interrupt = none leaves the Clock queue empty");
        g.hostNs += 5000000000ull;
        g.b->pump();
        CHECK(!g.b->assertsInt(), "...and five real seconds raise nothing");
    }

    // ---- the PORT switch decodes A2-A7: the base must be a multiple of 4 ----
    {
        Machine m;
        auto*   b = attach(m);
        std::string err;
        for (Property& p : b->properties()) {
            if (p.name != "port") continue;
            CHECK(!p.set(Value::ofInt(0x92), err),
                  "a base that is not a multiple of 4 is rejected");
            CHECK(err.find("multiple of 4") != std::string::npos,
                  "and the message says why");
            CHECK(p.set(Value::ofInt(0x90), err), "144 decimal (the manual's example) is fine");
        }
    }

    // ---- moving the base moves all four ports and nothing else answers ----
    {
        Machine m;
        attach(m, 0x90);
        enableRead(m, 0x90);
        m.bus.ioWrite(pA(0x90), 0x06);
        CHECK((m.bus.ioRead(pA(0x90)) & 0x0F) == 0x06, "the card answers at its new base");
        CHECK(m.bus.ioRead(0xF0) == 0xFF, "and no longer at the old one -- the bus floats it");
    }

    // ---- SNAPSHOT / RESTORE carries the set time and the PIA state ----
    {
        Machine m;
        auto*   b = attach(m);
        setClock(m, kBase, 32, 2, 29, 13, 45);

        StateWriter w;
        b->serialize(w);

        Machine m2;
        auto*   b2 = attach(m2);
        StateReader r(w.data());
        b2->deserialize(r);
        CHECK(r.ok(), "the snapshot reads back without underrun");

        enableRead(m2, kBase);
        CHECK(year2Of(m2, kBase) == 32 && monOf(m2, kBase) == 2 && dayOf(m2, kBase) == 29,
              "the restored board shows the time the snapshot captured");
    }
}
