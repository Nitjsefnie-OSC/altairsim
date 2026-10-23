#include "chips/am9512.h"

#include <algorithm>
#include <cmath>

namespace altair {

namespace {

// ---------------------------------------------------------------------------
// THE TWO FORMATS (p.7-95): sign, biased exponent, mantissa with an implied 1 (`p` bits in
// all, the implied one included).
// ---------------------------------------------------------------------------
struct Fmt {
    int p, ebits, bias;
    int      fracBits() const { return p - 1; }
    uint64_t fracMask() const { return (1ull << fracBits()) - 1; }
    uint64_t expMask() const { return (1ull << ebits) - 1; }
    uint64_t signBit() const { return 1ull << (fracBits() + ebits); }
    uint64_t expField(uint64_t v) const { return (v >> fracBits()) & expMask(); }
};
constexpr Fmt kSingle{24, 8, 127};
constexpr Fmt kDouble{53, 11, 1023};

// A value as m x 2^e, |m| in [1, 2) or m = 0. The exponent is kept apart from the host
// double so that no exponent the chip can hold, or reach, overflows the host.
struct Num {
    double m;
    int    e;
};

Num decode(uint64_t v, const Fmt& f) {
    uint64_t ex = f.expField(v);
    if (ex == 0) return {0, 0};  // an exponent field of 0 is zero
    double m = std::ldexp((double)((1ull << f.fracBits()) | (v & f.fracMask())), -f.fracBits());
    return {(v & f.signBit()) ? -m : m, (int)ex - f.bias};
}

// s x 2^e, rounded to the format (nearest, ties to even). An exponent out of range wraps,
// with the overflow or underflow bit in `err`. A zero is all zero bits.
uint64_t encode(double s, int e, const Fmt& f, uint8_t& err) {
    err = 0;
    if (s == 0) return 0;
    int fe;
    double fm = std::frexp(std::fabs(s), &fe);  // 0.5 <= fm < 1
    uint64_t mant = (uint64_t)std::nearbyint(std::ldexp(fm, f.p));
    long ex = (long)e + fe - 1;
    if (mant == 1ull << f.p) { mant >>= 1; ++ex; }  // rounding carried into a new bit
    long biased = ex + f.bias;
    if (biased >= (long)f.expMask()) err = Am9512::kOverflow;  // all ones is not a number
    else if (biased <= 0) err = Am9512::kUnderflow;
    return (s < 0 ? f.signBit() : 0) | ((uint64_t)biased & f.expMask()) << f.fracBits() |
           (mant & f.fracMask());
}

enum : uint8_t {
    CLR = 0x00,
    SADD = 0x01, SSUB = 0x02, SMUL = 0x03, SDIV = 0x04,
    CHSS = 0x05, PTOS = 0x06, POPS = 0x07, XCHS = 0x08,
    DADD = 0x29, DSUB = 0x2A, DMUL = 0x2B, DDIV = 0x2C,
    CHSD = 0x2D, PTOD = 0x2E, POPD = 0x2F,
};

// B op A in format `f`; `op` is the single-precision code (SADD ... SDIV).
uint64_t arith(uint8_t op, uint64_t a, uint64_t b, const Fmt& f, uint8_t& st) {
    Num A = decode(a, f), B = decode(b, f);
    if (op == SDIV && A.m == 0) {  // "If exponent field of A is zero then R = B"
        st = Am9512::kDivide;
        return b;
    }
    double s = 0;
    int    e = 0;
    switch (op) {
    case SADD:
    case SSUB: {
        double am = op == SSUB ? -A.m : A.m;
        if (A.m == 0) { s = B.m; e = B.e; }
        else if (B.m == 0) { s = am; e = A.e; }
        else {
            // Scale both by the larger exponent: the host sum is then rounded once to 53
            // bits, which rounds again to 24 correctly (53 >= 2 x 24 + 2).
            e = std::max(A.e, B.e);
            s = std::ldexp(B.m, B.e - e) + std::ldexp(am, A.e - e);
        }
        break;
    }
    case SMUL:
        if (A.m != 0 && B.m != 0) { s = B.m * A.m; e = B.e + A.e; }
        break;
    default:  // SDIV
        if (B.m != 0) { s = B.m / A.m; e = B.e - A.e; }
        break;
    }
    return encode(s, e, f, st);
}

// SIGN and ZERO: Z tests the exponent field only, as the data sheet says for POPS.
uint8_t szS(uint32_t v) {
    return (v & 0x80000000u ? Am9512::kSign : 0) | (kSingle.expField(v) == 0 ? Am9512::kZero : 0);
}
uint8_t szD(uint64_t v) {
    return (v >> 63 ? Am9512::kSign : 0) | (kDouble.expField(v) == 0 ? Am9512::kZero : 0);
}

}  // namespace

uint32_t Am9512::toSingle(double x, uint8_t* err) {
    uint8_t st;
    uint32_t v = (uint32_t)encode(x, 0, kSingle, st);
    if (err) *err = st;
    return v;
}

uint64_t Am9512::toDouble(double x, uint8_t* err) {
    uint8_t st;
    uint64_t v = encode(x, 0, kDouble, st);
    if (err) *err = st;
    return v;
}

double Am9512::fromSingle(uint32_t v) {
    Num n = decode(v, kSingle);
    return std::ldexp(n.m, n.e);
}

double Am9512::fromDouble(uint64_t v) {
    Num n = decode(v, kDouble);
    return std::ldexp(n.m, n.e);
}

// ---------------------------------------------------------------------------
// THE COMMANDS (pp.7-94, 7-100 ... 7-104). A = TOS, B = NOS. Each one sets the whole
// status register. Cycle counts: Table 2's maximum for the arithmetic, Table 3 for the rest.
// ---------------------------------------------------------------------------
unsigned Am9512::execute(uint8_t op) {
    switch (op) {
    case CLR:
        status_ = 0;
        return 4;

    case SADD: case SSUB: case SMUL: case SDIV: {
        uint8_t st = 0;
        uint32_t r = (uint32_t)arith(op, get32(0), get32(1), kSingle, st);
        drop(4);
        set32(0, r);
        status_ = st | szS(r);
        static const unsigned kMax[] = {512, 512, 254, 264};
        return kMax[op - SADD];
    }
    case DADD: case DSUB: case DMUL: case DDIV: {
        uint8_t st = 0;
        uint64_t r = arith((uint8_t)(op - DADD + SADD), get64(0), get64(1), kDouble, st);
        drop(8);
        set64(0, r);
        status_ = st | szD(r);
        static const unsigned kMax[] = {3100, 3100, 1860, 5120};
        return kMax[op - DADD];
    }

    case CHSS: {
        uint32_t a = get32(0);
        // "If the exponent field of A is zero, all bits of R will be zeros."
        set32(0, kSingle.expField(a) == 0 ? 0 : a ^ 0x80000000u);
        status_ = szS(get32(0));
        return 10;
    }
    case CHSD: {
        uint64_t a = get64(0);
        // "If the double precision operand A is zero, then the sign is not affected."
        if (kDouble.expField(a) != 0) set64(0, a ^ (1ull << 63));
        status_ = szD(get64(0));
        return 24;
    }

    case PTOS: {
        // A* A B C: the new TOS is zeroed when A's exponent is; the NOS is A as it was.
        uint32_t a = get32(0);
        lift(4);
        set32(0, kSingle.expField(a) == 0 ? 0 : a);
        status_ = szS(get32(0));
        return 16;
    }
    case PTOD: {
        uint64_t a = get64(0);
        lift(8);
        set64(0, a);
        status_ = szD(a);
        return 40;
    }
    case POPS: drop(4); status_ = szS(get32(0)); return 14;  // A goes to the bottom
    case POPD: drop(8); status_ = szD(get64(0)); return 26;  // = exchange TOS and NOS
    case XCHS: {
        uint32_t a = get32(0);
        set32(0, get32(1));
        set32(1, a);
        status_ = szS(get32(0));
        return 26;
    }

    default:
        return 4;  // "Operation for bit combinations not listed above is undefined."
    }
}

}  // namespace altair
