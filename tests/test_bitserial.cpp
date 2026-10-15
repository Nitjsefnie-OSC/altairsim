// BitSerialLine (src/chips/bitserial.h): an RS-232 line with no UART on the board. These
// tests drive the two wires directly, on a bench Clock, the way a guest's delay loop would:
// one byte, a burst, a break, a framing error, a glitch, a guest that runs a little slow,
// the receive side, and a snapshot taken in the middle of a frame. Each runs at two crystals
// and at `clock_hz = 0`, where hz() still gives the T-states a bit takes.

#include "chips/bitserial.h"
#include "core/clock.h"
#include "core/statefile.h"
#include "host/stream.h"
#include "test.h"

#include <cstdint>
#include <memory>
#include <string>

using namespace altair;

namespace {

struct Bench {
    Clock           clk;
    BitSerialLine   line;
    ScriptedStream* s = nullptr;

    explicit Bench(long long hz, long long baud = 9600) {
        clk.setHz(hz);
        line.baud = baud;
        auto st   = std::make_unique<ScriptedStream>();
        s         = st.get();
        line.connect(std::move(st));
        line.attachClock(&clk);
    }

    // One bit time, in T-states, scaled by `stretch` percent (100 = exact).
    uint64_t bit(int stretch = 100) const {
        return (uint64_t)clk.hz() * (uint64_t)stretch / (100 * (uint64_t)line.baud);
    }

    // A guest sending one frame: start bit, 8 data bits LSB first, then `stop`.
    void send(uint8_t b, bool stop = true, int stretch = 100) {
        line.setTx(false);
        clk.advance(bit(stretch));
        for (int i = 0; i < 8; ++i) {
            line.setTx((b >> i) & 1);
            clk.advance(bit(stretch));
        }
        line.setTx(stop);
        clk.advance(bit(stretch));
    }

    // A guest receiving one frame: wait for the start bit, go to its middle, sample 8 bits.
    // Returns -1 if no start bit came within `wait` T-states.
    int receive(uint64_t wait = 100000) {
        uint64_t spent = 0;
        while (line.rx()) {
            if (spent >= wait) return -1;
            clk.advance(10);
            spent += 10;
        }
        clk.advance(bit() / 2);
        if (line.rx()) return -2;  // the start bit was gone by its middle
        int b = 0;
        for (int i = 0; i < 8; ++i) {
            clk.advance(bit());
            if (line.rx()) b |= 1 << i;
        }
        clk.advance(bit());
        if (!line.rx()) return -3;  // no stop bit
        return b;
    }

    std::string log() {
        std::string out;
        for (auto& m : line.drainLog()) out += m + "\n";
        return out;
    }
};

void atRate(long long hz) {
    const std::string at = hz ? " at " + std::to_string(hz) + " Hz" : " at clock_hz=0";

    {
        Bench b(hz);
        CHECK(b.line.tx(), ("the line idles at MARKING" + at).c_str());
        b.send('A');
        CHECK(b.s->out() == "A", ("one byte goes out" + at).c_str());
        CHECK(b.clk.queued() == 0, ("no deadline is left after a frame" + at).c_str());
        CHECK(b.log().empty(), ("one good byte logs nothing" + at).c_str());
    }
    {
        // Back to back, one stop bit each. The last byte needs no write after it: the
        // deadline at 9.5 bit times sends it.
        Bench b(hz);
        for (char c : std::string("HELLO")) b.send((uint8_t)c);
        CHECK(b.s->out() == "HELLO", ("a burst goes out in order" + at).c_str());
        Bench c(hz);
        c.line.setTx(false);
        c.clk.advance(c.bit());
        for (int i = 0; i < 8; ++i) {
            c.line.setTx(("Z"[0] >> i) & 1);
            c.clk.advance(c.bit());
        }
        c.line.setTx(true);
        CHECK(c.s->out().empty(), ("the byte waits for the stop bit's middle" + at).c_str());
        c.clk.advance(c.bit());
        CHECK(c.s->out() == "Z", ("the deadline sends the last byte" + at).c_str());
    }
    {
        // A slow guest: every bit 4% long. The far end samples the middle and still reads it.
        Bench b(hz);
        b.send(0xA5, true, 104);
        b.send(0x5A, true, 96);
        CHECK(b.s->out() == "\xA5\x5A", ("a guest 4% off still gets through" + at).c_str());
    }
    {
        Bench b(hz);
        b.send(0x55, false);  // SPACE where the stop bit belongs
        b.line.setTx(true);
        b.clk.advance(b.bit() * 2);
        const std::string l = b.log();
        CHECK(b.s->out().empty(), ("a framing error sends nothing" + at).c_str());
        CHECK(l.find("framing error") != std::string::npos && l.find("55h") != std::string::npos &&
                  l.find('\n') == l.size() - 1,
              ("a framing error is logged once, with the byte" + at).c_str());
        b.send('k');
        CHECK(b.s->out() == "k", ("the next good byte goes out" + at).c_str());
    }
    {
        // A break: SPACE for three frames. One message, not one per frame, and the line
        // recovers on the next start bit after MARK.
        Bench b(hz);
        b.line.setTx(false);
        b.clk.advance(b.bit() * 30);
        b.line.setTx(true);
        b.clk.advance(b.bit() * 2);
        const std::string l = b.log();
        CHECK(b.s->out().empty(), ("a break sends nothing" + at).c_str());
        CHECK(l.find("break") != std::string::npos &&
                  l.find('\n') == l.size() - 1,
              ("a break is logged once" + at).c_str());
        b.send('!');
        CHECK(b.s->out() == "!", ("the line works after a break" + at).c_str());
    }
    {
        // A glitch: SPACE for a quarter of a bit. No start bit at its middle: nothing.
        Bench b(hz);
        b.line.setTx(false);
        b.clk.advance(b.bit() / 4);
        b.line.setTx(true);
        b.clk.advance(b.bit() * 12);
        CHECK(b.s->out().empty() && b.log().empty(),
              ("a glitch is not a byte, and not an error" + at).c_str());
    }
    {
        // Receive. Nothing waiting: MARK, and the frame does not start until the guest looks.
        Bench b(hz);
        CHECK(b.line.rx(), ("an empty line reads MARKING" + at).c_str());
        b.s->feed("Hi\x80\x01");
        b.clk.advance(1000000);
        CHECK(b.line.rxBytes() == 0, ("a byte waits for the guest to look" + at).c_str());
        CHECK(b.receive() == 'H', ("the first byte arrives" + at).c_str());
        CHECK(b.receive() == 'i', ("the second byte arrives" + at).c_str());
        CHECK(b.receive() == 0x80, ("bit 7 travels" + at).c_str());
        CHECK(b.receive() == 0x01, ("bit 0 travels" + at).c_str());
        CHECK(b.line.rxBytes() == 4, ("rxBytes counts what crossed" + at).c_str());
        CHECK(b.receive(1000) == -1, ("then the line is quiet" + at).c_str());
        CHECK(b.clk.queued() == 0, ("receive runs no clock events" + at).c_str());
    }
}

} // namespace

void test_bitserial() {
    SECTION("BitSerialLine: transmit and receive at 2 MHz, 4 MHz and clock_hz=0");
    atRate(2000000);
    atRate(4000000);
    atRate(0);

    SECTION("BitSerialLine: 1200 baud at 4 MHz is 3333 T-states a bit");
    {
        Bench b(4000000, 1200);
        CHECK(b.bit() == 3333, "the bit time is hz / baud");
        b.send('Q');
        CHECK(b.s->out() == "Q", "a 1200-baud byte goes out");
    }

    SECTION("BitSerialLine: unclocked, the line is dead");
    {
        BitSerialLine l;
        auto          st = std::make_unique<ScriptedStream>();
        ScriptedStream* s = st.get();
        l.connect(std::move(st));
        s->feed("x");
        CHECK(l.rx(), "an unclocked line reads MARKING");
        l.setTx(false);
        l.setTx(true);
        CHECK(s->out().empty(), "an unclocked line sends nothing");
    }

    SECTION("BitSerialLine: power on drops a frame in flight");
    {
        Bench b(2000000);
        b.clk.advance(b.bit() * 100);  // late in the run, so the frame outlives the clock reset
        b.line.setTx(false);  // a start bit, then data bits of 1: left alone, this is FFh
        b.clk.advance(b.bit());
        b.line.setTx(true);
        b.clk.advance(b.bit() * 3);
        b.clk.power();
        b.line.powerOn();
        CHECK(b.line.tx() && b.clk.queued() == 0, "power on: MARK, no deadline");
        b.clk.advance(b.bit() * 20);
        CHECK(b.s->out().empty(), "power on: the frame in flight is gone");
        b.send('p');
        CHECK(b.s->out() == "p", "power on: the line works after");
        CHECK(b.log().empty(), "power on: nothing about the old frame is logged");

        // The same on the receive side: a frame that started late must not outlive power.
        Bench c(2000000);
        c.clk.advance(c.bit() * 100);
        c.s->feed("r");
        CHECK(!c.line.rx(), "power on: a receive frame is on the wire");
        c.clk.power();
        c.line.powerOn();
        c.s->feed("s");
        CHECK(c.receive(2000) == 's', "power on: the next byte is received from its start bit");
    }

    SECTION("BitSerialLine: snapshot in the middle of a frame, both ways");
    {
        Bench b(2000000);
        b.s->feed("R");
        CHECK(!b.line.rx(), "receive: the start bit is on the wire");
        b.clk.advance(b.bit() * 2 + b.bit() / 2);
        b.line.setTx(false);                        // and a transmit frame starts
        b.clk.advance(b.bit());
        b.line.setTx(true);                         // data bit 0 = 1
        b.clk.advance(b.bit());  // receive is now 4.5 bits in: data bit 3

        StateWriter w;
        b.clk.serialize(w);
        b.line.serialize(w);

        Bench c(2000000);
        StateReader r(w.data());
        c.clk.deserialize(r);
        c.line.deserialize(r);
        CHECK(c.clk.queued() == 1, "restore: the transmit deadline is re-armed");
        CHECK(c.line.rx() == ((('R' >> 3) & 1) != 0), "restore: receive is in data bit 3");
        // Finish the transmit frame as 0x01 (data bit 0 = 1, the rest 0), stop bit MARK.
        c.line.setTx(false);
        c.clk.advance(c.bit() * 7);
        c.line.setTx(true);
        c.clk.advance(c.bit() * 2);
        CHECK(c.s->out() == "\x01", "restore: the transmit frame finishes");
        CHECK(c.line.rxBytes() == 1, "restore: the receive count travels");
    }
}
