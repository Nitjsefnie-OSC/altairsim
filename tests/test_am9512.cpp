#include "test.h"

#include "chips/am9512.h"
#include "core/statefile.h"

#include <cmath>
#include <cstdint>
#include <cstring>

using namespace altair;

// The Am9512 (chips/am9512.h) driven through its two ports, with no board and no bus. Every
// expected value comes from reference/AMD 9512.md: the data sheet's formats, command pages,
// Tables 2 and 3, the Manual's worked encodings and the SS-1 manual's test program.

namespace {

struct Fpu {
    Am9512   c{"fpu"};
    uint64_t now = 1000;

    void push32(uint32_t v) { for (int i = 0; i < 4; ++i) c.writeData((uint8_t)(v >> 8 * i), now); }
    void push64(uint64_t v) { for (int i = 0; i < 8; ++i) c.writeData((uint8_t)(v >> 8 * i), now); }
    uint32_t pop32() {
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v = v << 8 | c.readData(now);
        return v;
    }
    uint64_t pop64() {
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i) v = v << 8 | c.readData(now);
        return v;
    }
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
    // B op A: push B (NOS), then A (TOS), run, pop R.
    uint32_t s(uint8_t cmd, uint32_t b, uint32_t a, uint8_t* st = nullptr) {
        push32(b); push32(a);
        uint8_t x = run(cmd);
        if (st) *st = x;
        return pop32();
    }
    uint64_t d(uint8_t cmd, uint64_t b, uint64_t a, uint8_t* st = nullptr) {
        push64(b); push64(a);
        uint8_t x = run(cmd);
        if (st) *st = x;
        return pop64();
    }
};

uint32_t bitsOf(float f) { uint32_t v; std::memcpy(&v, &f, 4); return v; }
uint64_t bitsOf(double f) { uint64_t v; std::memcpy(&v, &f, 8); return v; }
float    floatOf(uint32_t v) { float f; std::memcpy(&f, &v, 4); return f; }
double   doubleOf(uint64_t v) { double f; std::memcpy(&f, &v, 8); return f; }

// A normal operand with its exponent field in [lo, hi], from a fixed LCG.
struct Lcg {
    uint64_t x = 0x9512;
    uint64_t next() { x = x * 6364136223846793005ull + 1442695040888963407ull; return x; }
    uint32_t single(unsigned lo, unsigned hi) {
        uint64_t r = next();
        uint32_t e = lo + (uint32_t)((r >> 40) % (hi - lo + 1));
        return (uint32_t)(r >> 63) << 31 | e << 23 | (uint32_t)(r & 0x7FFFFF);
    }
    uint64_t dbl(unsigned lo, unsigned hi) {
        uint64_t r = next(), m = next();
        uint64_t e = lo + (r >> 20) % (hi - lo + 1);
        return (r >> 63) << 63 | e << 52 | (m & 0xFFFFFFFFFFFFFull);
    }
};

enum : uint8_t {
    CLR = 0x00, SADD = 0x01, SSUB = 0x02, SMUL = 0x03, SDIV = 0x04,
    CHSS = 0x05, PTOS = 0x06, POPS = 0x07, XCHS = 0x08,
    DADD = 0x29, DSUB = 0x2A, DMUL = 0x2B, DDIV = 0x2C, CHSD = 0x2D, PTOD = 0x2E, POPD = 0x2F,
    SRE = 0x80,
};

}  // namespace

void test_am9512() {
    SECTION("Am9512 -- the formats, from the data sheet and the Manual's encodings");
    {
        CHECK(Am9512::toSingle(28) == 0x41E00000, "28 = 0 10000011 1100...");
        CHECK(Am9512::toSingle(-0.04296875) == 0xBD300000, "-0.04296875 = 1 01111010 0110...");
        CHECK(Am9512::toSingle(5) == 0x40A00000 && Am9512::toSingle(6) == 0x40C00000, "5, 6");
        CHECK(Am9512::toSingle(0.6) == 0x3F19999A, "0.6 = 3F19999A");
        CHECK(Am9512::toSingle(123) == 0x42F60000 && Am9512::toSingle(-678) == 0xC4298000,
              "123, -678");
        CHECK(Am9512::toSingle(-1.234) == 0xBF9DF3B6, "-1.234 = BF9DF3B6");
        CHECK(Am9512::toDouble(5) == 0x4014000000000000ull, "double 5 = 4014000000000000");
        CHECK(Am9512::toDouble(-1.234) == 0xBFF3BE76C8B43958ull, "double -1.234");
        CHECK(Am9512::toSingle(0) == 0, "zero is all zeros");
        CHECK(Am9512::fromSingle(0x00400000) == 0 && Am9512::fromSingle(0x80000001) == 0,
              "an exponent field of 0 is zero, whatever the mantissa");
        CHECK(Am9512::fromDouble(0x000FFFFFFFFFFFFFull) == 0, "for double too");
    }

    SECTION("Am9512 -- the SS-1 manual's test program: 1.0 + 1.0 by SADD");
    {
        Fpu f;
        for (uint8_t b : {0x00, 0x00, 0x80, 0x3F, 0x00, 0x00, 0x80, 0x3F}) f.c.writeData(b, f.now);
        f.run(SADD);
        uint8_t r[4];
        for (uint8_t& b : r) b = f.c.readData(f.now);
        CHECK(r[0] == 0x40 && r[1] == 0 && r[2] == 0 && r[3] == 0,
              "pops 40H 00H 00H 00H, MS byte first (TBL4)");
    }

    SECTION("Am9512 -- the data sheet's execution examples (Table 3)");
    {
        Fpu f;
        CHECK(f.s(SADD, 0x3F800000, 0x3F800000) == 0x40000000, "SADD 1 + 1 = 2");
        uint8_t st;
        CHECK(f.s(SSUB, 0x3F800000, 0x3F800000, &st) == 0, "SSUB 1 - 1 = 00000000");
        CHECK(st == Am9512::kZero, "with ZERO");
        CHECK(f.s(SMUL, 0x3FC00000, 0x40400000) == 0x40900000, "SMUL 1.5 x 3 = 4.5");
        CHECK(f.s(SDIV, 0x3F800000, 0x40000000) == 0x3F000000, "SDIV 1 / 2 = 0.5 (B / A)");
        f.push32(0x3F800000);
        f.run(CHSS);
        CHECK(f.pop32() == 0xBF800000, "CHSS 1 = -1");
        f.push64(0x3FF0000000000000ull);
        st = f.run(CHSD);
        CHECK(f.pop64() == 0xBFF0000000000000ull && st == Am9512::kSign, "CHSD 1 = -1, SIGN");
        CHECK(f.d(DADD, 0x8000000000000000ull, 0x3FF00000A0000000ull) == 0x3FF00000A0000000ull,
              "DADD -0 + 1.00000A = 1.00000A");
        CHECK(f.d(DSUB, 0x8000000000000000ull, 0x3FF00000A0000000ull) == 0xBFF00000A0000000ull,
              "DSUB -0 - 1.00000A is negative (Table 3 prints the DADD row: a misprint)");
        CHECK(f.d(DMUL, 0x3FF8000000000000ull, 0xBFF8000000000000ull) == 0xC002000000000000ull,
              "DMUL 1.5 x -1.5 = -2.25");
        CHECK(f.d(DDIV, 0x3FF8000000000000ull, 0xBFF8000000000000ull) == 0xBFF0000000000000ull,
              "DDIV 1.5 / -1.5 = -1");
    }

    SECTION("Am9512 -- the Manual's measured operands give the right sums");
    {
        Fpu f;
        CHECK(f.s(SADD, 0x40A00000, 0x40C00000) == Am9512::toSingle(11), "5 + 6 = 11");
        CHECK(f.s(SSUB, 0x42F60000, 0x43E40000) == Am9512::toSingle(-333), "123 - 456 = -333");
        CHECK(f.s(SMUL, 0x436A0000, 0xC4298000) == Am9512::toSingle(-158652),
              "234 x -678 = -158652");
        CHECK(f.d(DADD, Am9512::toDouble(12345), Am9512::toDouble(67890)) == Am9512::toDouble(80235),
              "double 12345 + 67890 = 80235");
    }

    // Round to nearest, ties to even (p.7-98). For a normal result that is IEEE 754's rule,
    // so the host's own float and double arithmetic is the oracle.
    SECTION("Am9512 -- rounding: nearest, ties to even, with a sticky bit");
    {
        Fpu f;
        CHECK(f.s(SADD, 0x3F800000, 0x33800000) == 0x3F800000, "1 + 2^-24: a tie, to even (down)");
        CHECK(f.s(SADD, 0x3F800000, 0x34400000) == 0x3F800002, "1 + 3 x 2^-24: a tie, to even (up)");
        CHECK(f.s(SADD, 0x3F800000, 0x33800001) == 0x3F800001, "just past the tie rounds up");
        CHECK(f.s(SADD, 0x3FFFFFFF, 0x33800000) == 0x40000000,
              "(2 - 2^-23) + 2^-24 rounds up into the next exponent: 2.0");
        CHECK(f.d(DADD, bitsOf(1.0), bitsOf(std::ldexp(1.0, -53))) == bitsOf(1.0),
              "double 1 + 2^-53: a tie, to even");
        CHECK(f.d(DADD, bitsOf(1.0), bitsOf(std::ldexp(1.0, -53) * (1 + std::ldexp(1.0, -52)))) ==
                  bitsOf(1.0 + std::ldexp(1.0, -52)),
              "double: just past the tie rounds up");

        Lcg g;
        int bad = 0;
        for (int i = 0; i < 4000; ++i) {
            // Exponents within +-56: no product or quotient leaves the normal range.
            uint32_t a = g.single(71, 183), b = g.single(71, 183);
            float fa = floatOf(a), fb = floatOf(b);
            if (f.s(SADD, b, a) != bitsOf(fb + fa)) ++bad;
            if (f.s(SSUB, b, a) != bitsOf(fb - fa)) ++bad;
            if (f.s(SMUL, b, a) != bitsOf(fb * fa)) ++bad;
            if (f.s(SDIV, b, a) != bitsOf(fb / fa)) ++bad;
        }
        CHECK(bad == 0, "16000 single results match IEEE round-to-nearest-even");
        bad = 0;
        for (int i = 0; i < 4000; ++i) {
            uint64_t a = g.dbl(600, 1446), b = g.dbl(600, 1446);
            double da = doubleOf(a), db = doubleOf(b);
            if (f.d(DADD, b, a) != bitsOf(db + da)) ++bad;
            if (f.d(DSUB, b, a) != bitsOf(db - da)) ++bad;
            if (f.d(DMUL, b, a) != bitsOf(db * da)) ++bad;
            if (f.d(DDIV, b, a) != bitsOf(db / da)) ++bad;
        }
        CHECK(bad == 0, "16000 double results match IEEE round-to-nearest-even");
    }

    SECTION("Am9512 -- divide by zero, overflow and underflow");
    {
        Fpu f;
        uint8_t st;
        CHECK(f.s(SDIV, 0x40A00000, 0, &st) == 0x40A00000, "SDIV by 0: R = B");
        CHECK(st == Am9512::kDivide, "with D, the divide exception");
        CHECK(f.s(SDIV, 0x40A00000, 0x00654321, &st) == 0x40A00000 && st == Am9512::kDivide,
              "an exponent field of 0 is a zero divisor");
        CHECK(f.d(DDIV, Am9512::toDouble(-3), 0, &st) == Am9512::toDouble(-3) &&
                  st == (Am9512::kDivide | Am9512::kSign),
              "DDIV by 0: R = B, D");
        CHECK(f.s(SDIV, 0, 0x40A00000, &st) == 0 && st == Am9512::kZero, "0 / 5 = 0, no error");

        // ⚠ The result after overflow or underflow is not documented: the model keeps the
        // mantissa and wraps the exponent field, as the Am9511A data sheet says it does.
        uint32_t big = Am9512::toSingle(std::ldexp(1.0, 100));
        CHECK(f.s(SMUL, big, big, &st) == 0x23800000, "SMUL 2^100 x 2^100: exponent 327 wraps to 71");
        CHECK(st == Am9512::kOverflow, "with V");
        uint32_t tiny = Am9512::toSingle(std::ldexp(1.0, -100));
        CHECK(f.s(SMUL, tiny, tiny, &st) == 0x5B800000 && st == Am9512::kUnderflow,
              "SMUL 2^-100 x 2^-100: U, exponent -73 wraps to 183");
        CHECK(f.s(SADD, 0x7F7FFFFF, 0x7F7FFFFF, &st) == 0x7FFFFFFF && st == Am9512::kOverflow,
              "SADD of the largest single: V; the exponent field reaches all ones");
        uint64_t dbig = Am9512::toDouble(std::ldexp(1.0, 1000));
        f.d(DMUL, dbig, dbig, &st);
        CHECK(st == Am9512::kOverflow, "DMUL 2^1000 x 2^1000: V");
        uint64_t dtiny = Am9512::toDouble(std::ldexp(1.0, -1000));
        f.d(DMUL, dtiny, dtiny, &st);
        CHECK(st == Am9512::kUnderflow, "DMUL 2^-1000 x 2^-1000: U");
    }

    SECTION("Am9512 -- the stack: 4 singles or 2 doubles, and the manipulation commands");
    {
        Fpu f;
        f.push32(1); f.push32(2); f.push32(3); f.push32(4); f.push32(5);
        CHECK(f.pop32() == 5 && f.pop32() == 4 && f.pop32() == 3 && f.pop32() == 2, "LIFO, 4 deep");
        CHECK(f.pop32() == 5, "a pop past empty wraps to old data: the first push is gone");

        Fpu p;
        p.push32(0x3F800000); p.push32(0x40000000); p.push32(0x40400000); p.push32(0x40800000);
        p.run(POPS);  // A B C D -> B C D A
        CHECK(p.pop32() == 0x40400000 && p.pop32() == 0x40000000 && p.pop32() == 0x3F800000 &&
                  p.pop32() == 0x40800000,
              "POPS writes A at the bottom: B C D A");

        Fpu x;
        x.push32(0x3F800000); x.push32(0x40000000);
        uint8_t st = x.run(XCHS);
        CHECK(x.pop32() == 0x3F800000 && x.pop32() == 0x40000000 && st == 0, "XCHS swaps TOS and NOS");

        Fpu t;
        t.push32(0x00123456);
        st = t.run(PTOS);
        CHECK(t.pop32() == 0 && t.pop32() == 0x00123456, "PTOS: A* is 0 when A's exponent is 0; NOS is A");
        CHECK(st == Am9512::kZero, "and ZERO is set");
        t.push32(0x00123456);
        t.run(CHSS);
        CHECK(t.pop32() == 0, "CHSS of a zero exponent: all bits zero");

        Fpu d;
        d.push64(0x1111111111111111ull); d.push64(0x4000000000000000ull);
        d.run(POPD);
        CHECK(d.pop64() == 0x1111111111111111ull && d.pop64() == 0x4000000000000000ull,
              "POPD is an exchange of the two doubles");
        d.push64(0x4000000000000000ull);
        d.run(PTOD);
        CHECK(d.pop64() == 0x4000000000000000ull && d.pop64() == 0x4000000000000000ull, "PTOD: A A");
        d.push64(0x800FFFFFFFFFFFFFull);
        d.run(CHSD);
        CHECK(d.pop64() == 0x800FFFFFFFFFFFFFull, "CHSD of zero: the sign is not affected");

        Fpu z;
        z.push32(0x00000001); z.push32(0x40000000);
        st = z.run(POPS);
        CHECK(st == Am9512::kZero, "POPS: Z when the new TOS has a zero exponent");
    }

    SECTION("Am9512 -- cycle counts: Table 2's maximum, Table 3 for the rest");
    {
        Fpu f;
        const struct { uint8_t op; unsigned n; const char* what; } kTab[] = {
            {SADD, 512, "SADD 512"}, {SSUB, 512, "SSUB 512"}, {SMUL, 254, "SMUL 254"},
            {SDIV, 264, "SDIV 264"}, {DADD, 3100, "DADD 3100"}, {DSUB, 3100, "DSUB 3100"},
            {DMUL, 1860, "DMUL 1860"}, {DDIV, 5120, "DDIV 5120"}, {CHSS, 10, "CHSS 10"},
            {PTOS, 16, "PTOS 16"}, {POPS, 14, "POPS 14"}, {XCHS, 26, "XCHS 26"},
            {CHSD, 24, "CHSD 24"}, {PTOD, 40, "PTOD 40"}, {POPD, 26, "POPD 26"}, {CLR, 4, "CLR 4"},
        };
        for (const auto& k : kTab) CHECK(f.cycles(k.op) == k.n, k.what);
        CHECK(f.cycles(SADD | SRE) == 512, "the SRE bit does not change the operation");
    }

    SECTION("Am9512 -- status: BUSY while it runs, CLR clears");
    {
        Fpu f;
        f.push32(0x40A00000); f.push32(0);
        f.c.writeCommand(SDIV, f.now);
        CHECK(f.c.readStatus(f.now + 263) == Am9512::kBusy, "BUSY alone until the last cycle");
        f.now += 264;
        CHECK(f.c.readStatus(f.now) == Am9512::kDivide, "then the result's status");
        CHECK(f.run(CLR) == 0, "CLR clears every bit");
    }

    SECTION("Am9512 -- END: set at the end, cleared by any access, EACK or RESET");
    {
        Fpu f;
        f.push32(0x3F800000); f.push32(0x3F800000);
        f.c.writeCommand(SADD, f.now);
        CHECK(!f.c.endOut(f.now + 511) && f.c.endOut(f.now + 512), "END comes up at the end");
        f.now += 512;
        f.c.readData(f.now);
        CHECK(!f.c.endOut(f.now), "a data read clears it");
        f.run(CLR);
        CHECK(!f.c.endOut(f.now), "a status read clears it");
        f.c.writeCommand(CLR, f.now);
        f.now += 4;
        f.c.writeData(0, f.now);
        CHECK(!f.c.endOut(f.now), "a data write clears it");
        f.c.writeCommand(CLR, f.now);
        f.now += 4;
        f.c.eack(f.now);
        CHECK(!f.c.endOut(f.now), "EACK clears it");
        f.c.writeCommand(CLR, f.now);
        f.now += 4;
        f.c.reset();
        CHECK(!f.c.endOut(f.now), "RESET clears it");
    }

    SECTION("Am9512 -- SVREQ follows the SRE bit");
    {
        Fpu f;
        f.run(CLR | SRE);
        CHECK(f.c.svreqOut(f.now), "set at the end of an SRE = 1 command");
        f.c.writeCommand(CLR, f.now);
        CHECK(f.c.svreqOut(f.now + 3) && !f.c.svreqOut(f.now + 4),
              "an SRE = 0 command clears it at its end");
        f.now += 4;
        f.run(CLR | SRE);
        f.c.svack();
        CHECK(!f.c.svreqOut(f.now), "SVACK clears it");
    }

    SECTION("Am9512 -- ERR: up at the end of an error, down at a status read or RESET");
    {
        Fpu f;
        f.push32(0x40A00000); f.push32(0);
        f.c.writeCommand(SDIV, f.now);
        CHECK(!f.c.errOut(f.now + 263) && f.c.errOut(f.now + 264), "ERR comes up at the end");
        f.now += 264;
        f.c.readData(f.now);
        CHECK(f.c.errOut(f.now), "a data read does not clear it");
        f.c.writeCommand(CLR, f.now);
        f.now += 4;
        CHECK(f.c.errOut(f.now), "nor does a command without an error");
        CHECK(f.c.readStatus(f.now) == 0 && !f.c.errOut(f.now), "a status read clears it");

        f.push32(0x40A00000); f.push32(0); f.push32(0);
        f.c.writeCommand(SDIV, f.now);        // 0 / 0: R = 0, D
        f.c.writeCommand(SDIV, f.now + 264);  // 5 / 0: D again; no status read between
        f.now += 264;
        CHECK(f.c.readStatus(f.now) == Am9512::kBusy && !f.c.errOut(f.now),
              "a status read while BUSY clears the first error's ERR");
        CHECK(f.c.errOut(f.now + 264), "the second error's ERR comes up at its end");
        f.c.reset();
        CHECK(!f.c.errOut(f.now + 264), "RESET clears it");
    }

    SECTION("Am9512 -- a snapshot keeps the stack, the status and the time left");
    {
        Fpu f;
        f.push64(Am9512::toDouble(2)); f.push64(Am9512::toDouble(0));
        f.c.writeCommand(DDIV | SRE, f.now);
        StateWriter w;
        f.c.serialize(w, f.now + 120);
        Am9512 b("fpu");
        StateReader r(w.data());
        b.deserialize(r, 9000);
        CHECK(b.busy(9000 + 4999) && !b.busy(9000 + 5000), "5000 CLKs were left, and 5000 are left");
        CHECK(b.svreqOut(14000) && b.endOut(14000) && b.errOut(14000),
              "SVREQ, END and ERR come at the new end");
        CHECK(b.readStatus(14000) == Am9512::kDivide, "the status travels");
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i) v = v << 8 | b.readData(14000);
        CHECK(Am9512::fromDouble(v) == 2, "and so does the stack");
    }
}
