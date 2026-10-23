#include "test.h"

#include "chips/am9511a.h"
#include "core/statefile.h"

#include <cmath>
#include <cstdint>

using namespace altair;

// The Am9511A (chips/am9511a.h) driven through its two ports, with no board and no bus.
// Every expected value comes from reference/AMD 9511A.md: the data sheet's command pages,
// the worked encodings, the SS-1 manual's test program, and the ch.9 error limits.

namespace {

// A chip and its CLK count. Each command runs to its end before the next access, as PAUSE
// makes it on a real bus.
struct Apu {
    Am9511a  c{"apu"};
    uint64_t now = 1000;

    void push16(uint16_t v) { c.writeData((uint8_t)v, now); c.writeData((uint8_t)(v >> 8), now); }
    void push32(uint32_t v) { for (int i = 0; i < 4; ++i) c.writeData((uint8_t)(v >> 8 * i), now); }
    void pushF(double x) { push32(Am9511a::toFloat(x)); }
    uint16_t pop16() {
        uint16_t v = (uint16_t)(c.readData(now) << 8);
        return (uint16_t)(v | c.readData(now));
    }
    uint32_t pop32() {
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v = v << 8 | c.readData(now);
        return v;
    }
    double popF() { return Am9511a::fromFloat(pop32()); }
    // Write a command, run it to its end, and read the status.
    uint8_t run(uint8_t cmd) {
        c.writeCommand(cmd, now);
        now = c.busyUntil();
        return c.readStatus(now);
    }
    unsigned cycles(uint8_t cmd) {
        uint64_t t0 = now;
        c.writeCommand(cmd, now);
        now = c.busyUntil();
        return (unsigned)(now - t0);
    }
};

double rel(double got, double want) { return std::fabs(got - want) / std::fabs(want); }

// The worst error of a derived function over a range, against host math at the operand the
// chip actually received (the 24-bit rounded input).
double worst(uint8_t cmd, double lo, double hi, int n, double (*f)(double), bool relative) {
    double w = 0;
    for (int i = 0; i <= n; ++i) {
        Apu a;
        double x = Am9511a::fromFloat(Am9511a::toFloat(lo + (hi - lo) * i / n));
        a.pushF(x);
        a.run(cmd);
        double want = f(x), got = a.popF();
        double e = relative ? (want == 0 ? std::fabs(got) : rel(got, want)) : std::fabs(got - want);
        if (e > w) w = e;
    }
    return w;
}

}  // namespace

void test_am9511a() {
    SECTION("Am9511A -- the float format, from the Manual's worked encodings");
    {
        CHECK(Am9511a::toFloat(5) == 0x03A00000, "5 = 03A00000");
        CHECK(Am9511a::toFloat(6) == 0x03C00000, "6 = 03C00000");
        CHECK(Am9511a::toFloat(123) == 0x07F60000, "123 = 07F60000");
        CHECK(Am9511a::toFloat(456) == 0x09E40000, "456 = 09E40000");
        CHECK(Am9511a::toFloat(12345) == 0x0EC0E400, "12345 = 0EC0E400");
        CHECK(Am9511a::toFloat(-678) == 0x8AA98000, "-678 = 8AA98000");
        CHECK(Am9511a::toFloat(100.5) == 0x07C90000, "100.5 = 0.11001001 x 2^7");
        CHECK(Am9511a::toFloat(0) == 0, "zero is all zeros");
        CHECK(Am9511a::toFloat(0.25) == 0x7F800000, "a negative exponent is two's complement");
        CHECK(Am9511a::fromFloat(0x0EC0E400) == 12345, "and back");
        CHECK(Am9511a::fromFloat(0x05400000) == 0, "a mantissa MSB of 0 reads as zero");
    }

    SECTION("Am9511A -- the SS-1 manual's test program: 3000H + 4000H by SADD");
    {
        Apu a;
        for (uint8_t b : {0x00, 0x30, 0x00, 0x40}) a.c.writeData(b, a.now);  // TBL1
        a.run(0x6C);
        uint8_t hi = a.c.readData(a.now), lo = a.c.readData(a.now);
        CHECK(hi == 0x70 && lo == 0x00, "pops 70H then 00H, MS byte first (TBL2)");
    }

    SECTION("Am9511A -- the stack is a 16-byte ring");
    {
        Apu a;
        a.push32(0x11111111); a.push32(0x22222222); a.push32(0x33333333);
        a.push32(0x44444444); a.push32(0x55555555);  // a fifth overwrites the oldest
        CHECK(a.pop32() == 0x55555555 && a.pop32() == 0x44444444, "LIFO");
        CHECK(a.pop32() == 0x33333333 && a.pop32() == 0x22222222, "four 32-bit entries");
        CHECK(a.pop32() == 0x55555555, "a pop past empty re-reads old data: the first is gone");

        Apu b;
        b.push16(0x1234);
        b.c.writeData(0xAB, b.now);  // one stray byte: the pointer is now misaligned
        CHECK(b.pop16() == 0xAB12, "a misaligned pointer takes the wrong bytes");
        b.c.reset();
        b.push16(0x5678);
        CHECK(b.pop16() == 0x5678, "RESET re-inits the pointer");
    }

    SECTION("Am9511A -- 16-bit fixed point");
    {
        Apu a;
        a.push16(0x7FFF); a.push16(1);
        uint8_t st = a.run(0x6C);  // SADD
        CHECK(a.pop16() == 0x8000, "SADD 7FFF + 1 = 8000 (low 16 bits)");
        CHECK(st == (Am9511a::kOverflow | Am9511a::kSign), "with overflow and sign");

        a.push16(0xFFFF); a.push16(1);
        st = a.run(0x6C);
        CHECK(a.pop16() == 0 && st == (Am9511a::kCarry | Am9511a::kZero), "FFFF + 1 carries to zero");

        a.push16(5); a.push16(7);
        st = a.run(0x6D);  // SSUB: B - A
        CHECK(a.pop16() == 0xFFFE && (st & Am9511a::kCarry) && (st & Am9511a::kSign), "SSUB 5 - 7 = -2, borrow");

        a.push16(0xFFFF); a.push16(0x8000);
        st = a.run(0x6D);
        CHECK(a.pop16() == 0x7FFF && (st & Am9511a::kOverflow), "SSUB with A = 8000 sets overflow even when R fits");

        a.push16(300); a.push16((uint16_t)-7);
        st = a.run(0x6E);  // SMUL
        CHECK(a.pop16() == (uint16_t)-2100 && !(st & 0x1E), "SMUL 300 x -7 = -2100");
        a.push16(200); a.push16(200);
        st = a.run(0x6E);
        CHECK(a.pop16() == (uint16_t)40000 && (st & Am9511a::kOverflow), "SMUL 200 x 200 overflows; low bits returned");
        a.push16(0x8000); a.push16(1);
        st = a.run(0x6E);
        CHECK(a.pop16() == 0x8000 && (st & Am9511a::kOverflow), "SMUL with the most negative value returns it");

        a.push16(0x4000); a.push16(0x4000);
        a.run(0x76);  // SMUU
        CHECK(a.pop16() == 0x1000, "SMUU 4000 x 4000: upper half 1000");

        a.push16(7); a.push16((uint16_t)-2);
        a.run(0x6F);  // SDIV
        CHECK(a.pop16() == (uint16_t)-3, "SDIV 7 / -2 = -3, no remainder");
        a.push16(1234); a.push16(0);
        st = a.run(0x6F);
        CHECK(a.pop16() == 1234 && (st & 0x1E) == Am9511a::kDivZero, "SDIV by 0: R = B, divide-by-zero");
    }

    SECTION("Am9511A -- 32-bit fixed point");
    {
        Apu a;
        a.push32(0x7FFFFFFF); a.push32(1);
        uint8_t st = a.run(0x2C);  // DADD
        CHECK(a.pop32() == 0x80000000u && (st & Am9511a::kOverflow), "DADD 7FFFFFFF + 1 overflows");
        a.push32(100000); a.push32(300000);
        st = a.run(0x2D);  // DSUB
        CHECK(a.pop32() == (uint32_t)-200000 && (st & Am9511a::kCarry), "DSUB 100000 - 300000");
        a.push32(100000); a.push32((uint32_t)-30000);
        st = a.run(0x2E);  // DMUL
        CHECK(a.pop32() == (uint32_t)(int32_t)-3000000000LL && (st & Am9511a::kOverflow),
              "DMUL 100000 x -30000 overflows; low 32 bits returned");
        a.push32(0x40000000); a.push32(0x10);
        a.run(0x36);  // DMUU
        CHECK(a.pop32() == 4, "DMUU 40000000 x 10: upper half 4");
        a.push32(1000000); a.push32(7);
        a.run(0x2F);  // DDIV
        CHECK(a.pop32() == 142857, "DDIV 1000000 / 7");
        a.push32(0x80000000u); a.push32(2);
        st = a.run(0x2F);
        CHECK(st & Am9511a::kOverflow, "DDIV with the most negative value sets overflow");
    }

    SECTION("Am9511A -- float add, subtract, multiply, divide");
    {
        Apu a;
        a.pushF(5); a.pushF(6);
        a.run(0x10);  // FADD
        CHECK(a.pop32() == 0x04B00000, "FADD 5 + 6 = 11 = 04B00000");
        a.pushF(5); a.pushF(6);
        uint8_t st = a.run(0x11);  // FSUB: B - A
        CHECK(a.pop32() == 0x81800000 && (st & Am9511a::kSign), "FSUB 5 - 6 = -1");
        a.pushF(5); a.pushF(5);
        st = a.run(0x11);
        CHECK(a.pop32() == 0 && st == Am9511a::kZero, "FSUB 5 - 5 = 0, all zeros");
        a.pushF(123); a.pushF(456);
        a.run(0x12);  // FMUL
        CHECK(a.popF() == 56088, "FMUL 123 x 456");
        a.pushF(1); a.pushF(3);
        a.run(0x13);  // FDIV
        CHECK(a.pop32() == 0x7FAAAAAB, "FDIV 1 / 3 = 7FAAAAAB (rounded to nearest)");
        a.pushF(7); a.pushF(0);
        st = a.run(0x13);
        CHECK(a.popF() == 7 && (st & 0x1E) == Am9511a::kDivZero, "FDIV by 0: R = B, divide-by-zero");
    }

    SECTION("Am9511A -- float results round to nearest, ties to even");
    {
        // 1 + 2^-23 has an odd last mantissa bit; adding half an LSB is a tie.
        Apu a;
        a.pushF(1 + std::ldexp(1.0, -23)); a.pushF(std::ldexp(1.0, -24));
        a.run(0x10);
        CHECK(a.pop32() == 0x01800002, "an odd tie rounds up to even");
        a.pushF(1); a.pushF(std::ldexp(1.0, -24));
        a.run(0x10);
        CHECK(a.pop32() == 0x01800000, "an even tie stays");
        a.pushF(1); a.pushF(std::ldexp(3.0, -25));  // 0.75 LSB
        a.run(0x10);
        CHECK(a.pop32() == 0x01800001, "more than half an LSB rounds up");
    }

    SECTION("Am9511A -- overflow and underflow keep the mantissa, exponent off by 128");
    {
        Apu a;
        a.push32(0x3F800000); a.push32(0x3F800000);  // 0.5 x 2^63, twice
        uint8_t st = a.run(0x12);
        CHECK(a.pop32() == 0x7D800000, "2^62 x 2^62 = 0.5 x 2^125: exponent 125 - 128 = -3");
        CHECK((st & 0x1E) == Am9511a::kOverflow, "with overflow");
        a.push32(0x40800000); a.push32(0x40800000);  // 0.5 x 2^-64, twice
        st = a.run(0x12);
        CHECK(a.pop32() == 0x7F800000, "0.5 x 2^-129 -> exponent -129 + 128 = -1");
        CHECK((st & 0x1E) == Am9511a::kUnderflow, "with underflow");
    }

    SECTION("Am9511A -- conversions");
    {
        Apu a;
        a.pushF(-3.75);
        uint8_t st = a.run(0x1F);  // FIXS
        CHECK(a.pop16() == (uint16_t)-3 && (st & Am9511a::kSign), "FIXS -3.75 = -3 (the integer portion)");
        a.pushF(40000);
        st = a.run(0x1F);
        CHECK((st & 0x1E) == Am9511a::kOverflow && a.popF() == 40000, "FIXS 40000 overflows; A not changed");
        a.pushF(123456789.0);
        a.run(0x1E);  // FIXD
        CHECK(a.pop32() == 123456792, "FIXD of 123456789 as a float (24 bits)");
        a.push16((uint16_t)-1);
        a.run(0x1D);  // FLTS
        CHECK(a.pop32() == 0x81800000, "FLTS -1 = 81800000");
        a.push32(16777217);  // 2^24 + 1: a tie at 24 bits
        a.run(0x1C);  // FLTD
        CHECK(a.pop32() == 0x19800000, "FLTD 2^24 + 1 rounds to even, 2^24");
    }

    SECTION("Am9511A -- change sign");
    {
        Apu a;
        a.push16(5);
        a.run(0x74);
        CHECK(a.pop16() == (uint16_t)-5, "CHSS 5 = -5");
        a.push16(0x8000);
        uint8_t st = a.run(0x74);
        CHECK(a.pop16() == 0x8000 && (st & Am9511a::kOverflow), "CHSS 8000 overflows, unchanged");
        a.push32(0x80000000u);
        st = a.run(0x34);
        CHECK(a.pop32() == 0x80000000u && (st & Am9511a::kOverflow), "CHSD 80000000 overflows, unchanged");
        a.pushF(5);
        a.run(0x15);
        CHECK(a.pop32() == 0x83A00000, "CHSF 5 = -5");
        a.push32(0);
        a.run(0x15);
        CHECK(a.pop32() == 0, "CHSF of zero makes no change");
    }

    SECTION("Am9511A -- stack commands");
    {
        Apu a;
        a.push16(0x1111); a.push16(0x2222);
        a.run(0x79);  // XCHS
        CHECK(a.pop16() == 0x1111 && a.pop16() == 0x2222, "XCHS swaps TOS and NOS");
        a.push32(0xAABBCCDD);
        a.run(0x37);  // PTOD
        CHECK(a.pop32() == 0xAABBCCDD && a.pop32() == 0xAABBCCDD, "PTOD pushes a copy");
        a.push16(1); a.push16(2);
        a.run(0x78);  // POPS
        CHECK(a.pop16() == 1, "POPS drops TOS");
        a.run(0x1A);  // PUPI
        CHECK(a.pop32() == 0x02C90FDB, "PUPI pushes pi = 02C90FDB");
        a.pushF(-5); a.pushF(0);
        uint8_t pst = a.run(0x18);  // POPF
        CHECK(pst == Am9511a::kSign && a.popF() == -5, "POPF drops TOS; status is the new TOS");
        a.push32(0x80000000u); a.push16(0);
        uint8_t st = a.run(0x00);  // NOP
        CHECK(st == 0, "NOP clears the status byte");
    }

    SECTION("Am9511A -- BUSY lasts the top of each data sheet range");
    {
        struct { uint8_t cmd; unsigned cyc; const char* what; } t[] = {
            {0x6C, 18, "SADD 16-18"},   {0x6D, 32, "SSUB 30-32"},  {0x6E, 94, "SMUL 84-94"},
            {0x2C, 22, "DADD 20-22"},   {0x2E, 210, "DMUL 194-210"},
            {0x12, 168, "FMUL 146-168"}, {0x01, 870, "SQRT 782-870"}, {0x03, 4878, "COS 3840-4878"},
            {0x0B, 12032, "PWR 8290-12032"}, {0x00, 4, "NOP 4"}, {0x1A, 16, "PUPI 16"},
            {0x77, 16, "PTOS 16"}, {0x19, 26, "XCHF 26"},
        };
        for (auto& e : t) {
            Apu a;
            a.pushF(2); a.pushF(3);
            CHECK(a.cycles(e.cmd) == e.cyc, e.what);
        }
        Apu a;
        a.pushF(2); a.pushF(3);
        CHECK(a.cycles(0x10) == 368, "FADD 54-368");
        a.pushF(2); a.pushF(0);
        CHECK(a.cycles(0x10) == 24, "FADD with A = 0: 24");
        a.pushF(2); a.pushF(0);
        CHECK(a.cycles(0x13) == 22, "FDIV with A = 0: 22");
        a.push16(2); a.push16(0);
        CHECK(a.cycles(0x6F) == 14, "SDIV with A = 0: 14");
        a.pushF(1e-5);
        CHECK(a.cycles(0x02) == 30, "SIN of |A| <= 2^-12: 30");
        a.pushF(-1);
        CHECK(a.cycles(0x09) == 20, "LN of A <= 0: 20");
        a.pushF(40);
        CHECK(a.cycles(0x0A) == 34, "EXP of |A| > 32: 34");

        Apu b;
        b.pushF(2); b.pushF(3);
        b.c.writeCommand(0x12, b.now);
        CHECK(b.c.busy(b.now + 167) && !b.c.busy(b.now + 168), "busy for 168 CLKs, then idle");
        CHECK(b.c.readStatus(b.now + 100) == Am9511a::kBusy, "status shows BUSY while it runs");
        CHECK(b.c.readStatus(b.now + 168) == 0, "and the result's status after");
    }

    SECTION("Am9511A -- END: set at the end, cleared by any access or EACK");
    {
        Apu a;
        CHECK(!a.c.endOut(a.now), "END is off after power-on");
        a.pushF(2); a.pushF(3);
        a.c.writeCommand(0x10, a.now);
        uint64_t end = a.c.busyUntil();
        CHECK(!a.c.endOut(end - 1), "not while BUSY");
        a.c.readStatus(end - 1);
        CHECK(a.c.endOut(end), "a status read during BUSY does not stop it setting");
        a.c.readStatus(end);
        CHECK(!a.c.endOut(end), "a status read after the end clears it");

        a.c.writeCommand(0x00, end);
        CHECK(a.c.endOut(end + 4), "the next command sets it again");
        a.c.readData(end + 4);
        CHECK(!a.c.endOut(end + 4), "a data read clears it");
        a.c.writeCommand(0x00, end + 4);
        a.c.eack(end + 8);
        CHECK(!a.c.endOut(end + 8), "EACK clears it");
        a.c.writeCommand(0x00, end + 8);
        a.c.reset();
        CHECK(!a.c.endOut(end + 20) && !a.c.busy(end + 8), "RESET clears it and stops the command");
    }

    SECTION("Am9511A -- SVREQ follows the SR bit");
    {
        Apu a;
        a.c.writeCommand(0x80, a.now);  // NOP with SR = 1
        CHECK(!a.c.svreqOut(a.now), "not while the command runs");
        a.now = a.c.busyUntil();
        CHECK(a.c.svreqOut(a.now), "set at its end");
        a.c.readStatus(a.now);
        a.c.readData(a.now);
        CHECK(a.c.svreqOut(a.now), "an access does not clear it");
        a.c.writeCommand(0x00, a.now);  // SR = 0
        CHECK(a.c.svreqOut(a.now), "it stays while the next command runs");
        a.now = a.c.busyUntil();
        CHECK(!a.c.svreqOut(a.now), "and clears at the end of an SR = 0 command");
        a.run(0x80);
        a.c.svack();
        CHECK(!a.c.svreqOut(a.now), "SVACK clears it");
        a.run(0x80);
        a.c.reset();
        CHECK(!a.c.svreqOut(a.now), "RESET clears it");
    }

    // The data sheet gives one error figure per function; AMD's graphs of the real chip
    // (Algorithm Details pp.6-7) show where it holds. A relative error near a zero of SIN or
    // COS, or near a pole of TAN, is not finite on the chip either: the 24-bit 2/PI leaves
    // about 1e-7 in the reduced angle. There the check is absolute.
    SECTION("Am9511A -- derived functions inside the data sheet's error figures");
    {
        const double kTwoPi = 6.283185307179586, kHalfPi = 1.5707963267948966;
        double w;
        w = worst(0x02, -kHalfPi, kHalfPi, 2000, [](double x) { return std::sin(x); }, true);
        CHECK(w < 5.0e-7, "SIN: 5.0e-7 relative over -pi/2 ... +pi/2");
        w = worst(0x02, -kTwoPi, kTwoPi, 4000, [](double x) { return std::sin(x); }, false);
        CHECK(w < 5.0e-7, "SIN: 5.0e-7 absolute over -2pi ... +2pi");
        w = worst(0x03, -1, 1, 2000, [](double x) { return std::cos(x); }, true);
        CHECK(w < 5.0e-7, "COS: 5.0e-7 relative over -1 ... +1");
        w = worst(0x03, -kTwoPi, kTwoPi, 4000, [](double x) { return std::cos(x); }, false);
        CHECK(w < 5.0e-7, "COS: 5.0e-7 absolute over -2pi ... +2pi");
        w = worst(0x04, -0.78, 0.78, 2000, [](double x) { return std::tan(x); }, true);
        CHECK(w < 5.0e-7, "TAN: 5.0e-7 relative inside -pi/4 ... +pi/4");
        w = worst(0x05, -1, 1, 4000, [](double x) { return std::asin(x); }, true);
        CHECK(w < 4.0e-7, "ASIN: 4.0e-7 relative over -1 ... +1");
        w = worst(0x06, -1, 0.9, 4000, [](double x) { return std::acos(x); }, true);
        CHECK(w < 2.5e-7, "ACOS: about 2.0e-7 relative up to 0.9 (2.07e-7 at 0.706)");
        w = worst(0x06, -1, 1, 4000, [](double x) { return std::acos(x); }, false);
        CHECK(w < 5.0e-7, "ACOS: 5.0e-7 absolute over -1 ... +1");
        w = worst(0x07, -1e4, 1e4, 8000, [](double x) { return std::atan(x); }, true);
        CHECK(w < 3.0e-7, "ATAN: 3.0e-7 relative");
        w = worst(0x09, 1 / 2.718281828, 2.718281828, 2000, [](double x) { return std::log(x); }, false);
        CHECK(w < 2.0e-7, "LN: 2e-7 absolute over 1/e ... e");
        w = worst(0x08, 0.1, 10, 2000, [](double x) { return std::log10(x); }, false);
        CHECK(w < 2.0e-7, "LOG: 2.0e-7 absolute over 0.1 ... 10");
        w = worst(0x0A, -16, 16, 4000, [](double x) { return std::exp(x); }, true);
        CHECK(w < 5.0e-7, "EXP: 5.0e-7 relative over -16 ... +16");
        w = worst(0x0A, -32, 32, 8000, [](double x) { return std::exp(x); }, true);
        CHECK(w < 7.0e-7, "EXP: under the graph's 7e-7 peak over -32 ... +32");
        w = worst(0x01, 1e-6, 1e6, 8000, [](double x) { return std::sqrt(x); }, true);
        CHECK(w < 2.0e-7, "SQRT: 2.0e-7 relative (the graph peaks near 1.3e-7)");

        Apu a;
        a.pushF(2); a.pushF(10);
        a.run(0x0B);  // PWR: B ^ A
        CHECK(rel(a.popF(), 1024) < 7.0e-7, "PWR 2 ^ 10 = 1024 within 7.0e-7");
        a.pushF(1e-5);
        a.run(0x02);
        CHECK(a.popF() == Am9511a::fromFloat(Am9511a::toFloat(1e-5)), "SIN of a tiny angle returns A");
    }

    SECTION("Am9511A -- derived-function argument errors");
    {
        Apu a;
        a.pushF(-4);
        uint8_t st = a.run(0x01);
        CHECK((st & 0x1E) == Am9511a::kNegArg && a.popF() == -4, "SQRT of a negative: error 0100");
        a.pushF(0);
        st = a.run(0x09);
        CHECK((st & 0x1E) == Am9511a::kNegArg, "LN of 0: error 0100");
        a.pushF(1.5);
        st = a.run(0x05);
        CHECK((st & 0x1E) == Am9511a::kArgRange, "ASIN of 1.5: error 1100");
        a.pushF(33);
        st = a.run(0x0A);
        CHECK((st & 0x1E) == Am9511a::kArgRange, "EXP of 33: error 1100");
        a.pushF(-2); a.pushF(2);
        st = a.run(0x0B);
        CHECK((st & 0x1E) == Am9511a::kNegArg, "PWR with B < 0: error 0100");
        a.pushF(0); a.pushF(2);
        st = a.run(0x0B);
        CHECK((st & 0x1E) == Am9511a::kNegArg, "PWR with B = 0: error 0100 (B must be positive)");
        a.pushF(10); a.pushF(20);
        st = a.run(0x0B);
        CHECK((st & 0x1E) == Am9511a::kArgRange, "PWR 10 ^ 20: A x LN B > 32, error 1100");
    }

    SECTION("Am9511A -- a snapshot keeps the stack, the status and the time left");
    {
        Apu a;
        a.pushF(2); a.pushF(3);
        a.c.writeCommand(0x92, a.now);  // FMUL, SR = 1
        StateWriter w;
        a.c.serialize(w, a.now + 100);
        Am9511a b("apu");
        StateReader r(w.data());
        b.deserialize(r, 5000);
        CHECK(b.busy(5067) && !b.busy(5068), "68 CLKs were left, and 68 are left");
        CHECK(b.svreqOut(5068) && b.endOut(5068), "SVREQ and END come at the new end");
        CHECK(b.readStatus(5068) == 0, "the status travels");
        CHECK(Am9511a::fromFloat(b.readData(5068) << 24 | b.readData(5068) << 16 |
                                 b.readData(5068) << 8 | b.readData(5068)) == 6,
              "and so does the stack");
    }
}
