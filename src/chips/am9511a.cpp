#include "chips/am9511a.h"

#include <cmath>

namespace altair {

namespace {

// ---------------------------------------------------------------------------
// THE CHIP'S FLOAT ARITHMETIC. A value is held as a host double and rounded to a 24-bit
// mantissa, nearest-even, after every operation, as the chip rounds each of its own float
// operations. A double holds the exact sum, product or quotient of two 24-bit values to
// 53 bits, and 53 >= 2 x 24 + 2, so rounding it a second time to 24 bits gives the
// correctly rounded result. The exponent is not bounded here; toFloat wraps it.
//
// Two hand-offs inside the derived functions are NOT rounded: the range reduction (the
// argument times 2/PI, 4/PI or LOG2 e, less its whole part) and the ASIN that ACOS subtracts
// from PI/2. AMD's error graphs for the real chip (Algorithm Details pp.6-7) show e^x at
// about 5e-7 near |x| = 32 and ACOS at about 1e-7; rounding those steps to 24 bits gives
// 1.8e-6 and 5e-7, and keeping them whole gives what the graphs show.
// ---------------------------------------------------------------------------
double r24(double x) {
    if (x == 0) return 0;
    int e;
    double m = std::frexp(x, &e);                         // 0.5 <= |m| < 1
    return std::ldexp(std::nearbyint(std::ldexp(m, 24)), e - 24);  // ties to even
}

double add(double a, double b) { return r24(a + b); }
double sub(double a, double b) { return r24(a - b); }
double mul(double a, double b) { return r24(a * b); }
double dvd(double a, double b) { return r24(a / b); }

// The constants, as the chip's ROM holds them: rounded to 24 bits.
const double kPi       = r24(3.14159265358979323846);
const double kHalfPi   = r24(1.57079632679489661923);
const double kTwoOverPi  = r24(0.63661977236758134308);
const double kFourOverPi = r24(1.27323954473516268615);
const double kSqrt2    = r24(1.41421356237309504880);
const double kLog2e    = r24(1.44269504088896340736);
const double kLn2      = r24(0.69314718055994530942);
const double kLog10e   = r24(0.43429448190325182765);

// The Chebyshev coefficients, AMD Floating Point Processor Manual ch.9 (reference/AMD
// 9511A.md sec. 6).
const double kCsin[]  = {2.5525579, -0.2852616, 9.118016e-3, -1.365875e-4, 1.184962e-6,
                         -6.702792e-9};
const double kCtan[]  = {1.7701474, 1.0675393e-1, 7.5861016e-3, 5.4417038e-4,
                         3.9066370e-5, 2.8048161e-6, 2.0137658e-7, 1.4458187e-8,
                         1.0380510e-9};
const double kCasin[] = {1.4866665, 3.8853034e-2, 2.8854414e-3, 2.8842183e-4,
                         3.3223672e-5, 4.1584779e-6, 5.4965045e-7, 7.5500784e-8,
                         1.0671938e-8, 1.5421800e-9};
const double kCatan[] = {1.7627472, -1.0589292e-1, 1.1135842e-2, -1.3811950e-3,
                         1.8574297e-4, -2.6215196e-5, 3.8210366e-6, -5.6991862e-7,
                         8.6488779e-8, -1.3303384e-8, 2.0685060e-9, -3.2448600e-10};
const double kCexp[]  = {1.4569999, -2.4876243e-1, 2.1446556e-2, -1.2357141e-3,
                         5.3453058e-5, -1.8506907e-6, 5.3411877e-8, -1.3215160e-9};
const double kCln[]   = {7.5290563e-1, 3.4314575e-1, -2.9437253e-2, 3.3670893e-3,
                         -4.3327589e-4, 5.9470712e-5, -8.5029675e-6, 1.2504674e-6,
                         -1.8772800e-7, 2.8630251e-8, -4.4209570e-9};

// CHEBY (ch.9.3), the Clenshaw form the Manual gives in FORTRAN: C(1..N) there is c[0..n-1].
template <size_t N>
double cheby(double x, const double (&c)[N]) {
    double a = 0, b = 0, d = r24(c[N - 1]);
    double x2 = 2 * x;  // exact
    for (size_t i = N - 1; i >= 1; --i) {
        a = b;
        b = d;
        d = add(sub(mul(x2, b), a), r24(c[i - 1]));
    }
    return sub(d, a) / 2;  // exact
}

double entier(double x) { return std::floor(x); }  // rounds down: ENTIER(-2.5) = -3

// SIN and COS (ch.9.4, 9.5); both use the sine coefficients.
double fsin(double x) {
    x = x * kTwoOverPi;
    x = x - 4 * entier((x + 1) / 4);
    if (x > 1) x = 2 - x;
    x = r24(x);
    return mul(x, cheby(sub(2 * mul(x, x), 1), kCsin));  // ⚠ printed "2X^2 - 0.1"
}
double fcos(double x) {
    x = x * kTwoOverPi;
    x = 4 * entier((x + 2) / 4) - x + 1;
    if (x > 1) x = 2 - x;
    x = r24(x);
    return mul(x, cheby(sub(2 * mul(x, x), 1), kCsin));
}

// TAN (ch.9.6). ⚠ printed X*2/PI; X*4/PI is what closes, and what the command page's
// -PI/4 ... +PI/4 reduction says. Sets *pole when the cotangent branch would divide by 0.
double ftan(double x, bool* pole) {
    x = x * kFourOverPi;
    x = x - 4 * entier((x + 1) / 4);
    double y = x;
    if (y > 1) x = 2 - x;
    x = r24(x);
    double t = mul(x, cheby(sub(2 * mul(x, x), 1), kCtan));
    if (y > 1) {
        if (t == 0) { *pole = true; return 0; }
        t = dvd(1, t);
    }
    return t;
}

// SQRT (ch.9.14): split off an even exponent, a minimax first guess on the mantissa, then
// two Newton-Raphson steps. ⚠ the printed FORTRAN divides X, not the mantissa, in the
// Newton steps; the prose (and arithmetic) says the mantissa.
double fsqrt(double x) {
    if (x == 0) return 0;
    int e;
    double m = std::frexp(x, &e);  // the chip's own mantissa and exponent
    if (e & 1) { ++e; m /= 2; }
    double x0 = add(mul(r24(0.68629150), m), r24(0.34314575));
    double x1 = dvd(add(dvd(m, x0), x0), 2);
    double x2 = dvd(add(dvd(m, x1), x1), 2);
    return std::ldexp(x2, e / 2);
}

// ASIN (ch.9.7). ⚠ both printed branches drop a symbol; these are the forms that close.
// The last step is left unrounded: ACOS (ch.9.8) subtracts it from PI/2 before rounding.
double fasinWhole(double x) {
    double xx = mul(x, x);
    if (xx <= 0.5) return mul(x, kSqrt2) * cheby(sub(4 * xx, 1), kCasin);
    double s = mul(fsqrt(sub(2, 2 * xx)), cheby(sub(3, 4 * xx), kCasin));
    double r = kHalfPi - s;
    return x < 0 ? -r : r;
}
double fasin(double x) { return r24(fasinWhole(x)); }
double facos(double x) { return r24(kHalfPi - fasinWhole(x)); }

// ATAN (ch.9.9).
double fatan(double x) {
    double u = x;
    if (std::fabs(u) > 1) x = dvd(1, x);
    double a = mul(x, cheby(sub(2 * mul(x, x), 1), kCatan));
    if (u > 1) a = sub(kHalfPi, a);
    if (u < -1) a = sub(-kHalfPi, a);
    return a;
}

// EXP (ch.9.10): 2^N times a series on the fraction; "no postprocessing is required".
double fexp(double x) {
    x = x * kLog2e;
    double n = 1 + entier(x);
    return std::ldexp(cheby(r24(2 * (n - x) - 1), kCexp), (int)n);
}

// LN (ch.9.11): a series on the mantissa plus the exponent times LN 2.
double fln(double x) {
    int e;
    double m = std::frexp(x, &e);
    return add(cheby(sub(4 * m, 3), kCln), mul(e - 1, kLn2));
}

// The derived-function command list, by op code.
enum : uint8_t {
    SQRT = 0x01, SIN = 0x02, COS = 0x03, TAN = 0x04, ASIN = 0x05, ACOS = 0x06, ATAN = 0x07,
    LOG = 0x08, LN = 0x09, EXP = 0x0A, PWR = 0x0B,
    FADD = 0x10, FSUB = 0x11, FMUL = 0x12, FDIV = 0x13, CHSF = 0x15, PTOF = 0x17,
    POPF = 0x18, XCHF = 0x19, PUPI = 0x1A, FLTD = 0x1C, FLTS = 0x1D, FIXD = 0x1E, FIXS = 0x1F,
    DADD = 0x2C, DSUB = 0x2D, DMUL = 0x2E, DDIV = 0x2F, CHSD = 0x34, DMUU = 0x36, PTOD = 0x37,
    POPD = 0x38, XCHD = 0x39,
    SADD = 0x6C, SSUB = 0x6D, SMUL = 0x6E, SDIV = 0x6F, CHSS = 0x74, SMUU = 0x76, PTOS = 0x77,
    POPS = 0x78, XCHS = 0x79,
    NOP = 0x00,
};

// SIGN and ZERO for a value of each width.
uint8_t sz16(uint16_t v) { return (v & 0x8000 ? Am9511a::kSign : 0) | (v == 0 ? Am9511a::kZero : 0); }
uint8_t sz32(uint32_t v) { return (v & 0x80000000u ? Am9511a::kSign : 0) | (v == 0 ? Am9511a::kZero : 0); }
uint8_t szF(uint32_t v) {
    return (v & 0x80000000u ? Am9511a::kSign : 0) | (v & 0x800000 ? 0 : Am9511a::kZero);
}

}  // namespace

// ---------------------------------------------------------------------------
// THE FLOAT FORMAT (p.7-71): sign in bit 31, a 7-bit two's complement exponent in bits
// 30-24, a 24-bit mantissa 0.5 <= m < 1 with no hidden bit. Zero is all zeros.
// ---------------------------------------------------------------------------
uint32_t Am9511a::toFloat(double x, uint8_t* err) {
    if (err) *err = 0;
    if (x == 0) return 0;
    int e;
    double m = std::frexp(std::fabs(x), &e);
    uint32_t mant = (uint32_t)std::nearbyint(std::ldexp(m, 24));
    if (mant == 0x1000000) { mant = 0x800000; ++e; }  // rounding carried into a new bit
    if (err && e > 63) *err = kOverflow;
    if (err && e < -64) *err = kUnderflow;
    // "The mantissa is correct and the exponent is offset by 128": the field wraps.
    uint32_t e7 = (uint32_t)(e + 64) & 0x7F;
    e7 = (e7 - 64) & 0x7F;
    return (x < 0 ? 0x80000000u : 0) | (e7 << 24) | mant;
}

double Am9511a::fromFloat(uint32_t v) {
    if (!(v & 0x800000)) return 0;  // mantissa MSB clear: zero
    int e = (int)((v >> 24) & 0x7F);
    if (e & 0x40) e -= 128;
    double m = std::ldexp((double)(v & 0xFFFFFF), e - 24);
    return (v & 0x80000000u) ? -m : m;
}

// ---------------------------------------------------------------------------
// THE COMMANDS (pp.7-72 ... 7-86). Each one sets the whole status register; its cycle count
// is the top of the data sheet's range.
// ---------------------------------------------------------------------------
unsigned Am9511a::execute(uint8_t op) {
    switch (op) {
    case NOP:
        status_ = 0;  // NOP clears the status byte
        return 4;

    // ---- 16-bit fixed point: B op A ----
    case SADD: case SSUB: case SMUL: case SMUU: case SDIV: {
        int32_t a = (int16_t)get16(0), b = (int16_t)get16(1);
        uint16_t ua = (uint16_t)a, ub = (uint16_t)b;
        uint8_t st = 0;
        int32_t r = 0;
        unsigned cyc = 0;
        switch (op) {
        case SADD:
            r = b + a;
            if (ub + ua > 0xFFFF) st |= kCarry;
            if (r != (int16_t)r) st |= kOverflow;
            cyc = 18;
            break;
        case SSUB:
            r = b - a;
            if (ub < ua) st |= kCarry;  // borrow
            if (r != (int16_t)r || a == -32768) st |= kOverflow;
            cyc = 32;
            break;
        case SMUL:
            if (a == -32768 || b == -32768) { r = -32768; st |= kOverflow; }
            else { r = b * a; if (r != (int16_t)r) st |= kOverflow; }
            cyc = 94;
            break;
        case SMUU:
            if (a == -32768 || b == -32768) { r = -32768; st |= kOverflow; }
            else r = (b * a) >> 16;
            cyc = 98;
            break;
        default:  // SDIV
            if (a == 0) { r = b; st |= kDivZero; cyc = 14; break; }
            r = b / a;  // no remainder; the quotient is truncated toward zero
            if (r != (int16_t)r) st |= kOverflow;
            cyc = 94;
            break;
        }
        drop(2);
        set16(0, (uint16_t)r);
        status_ = st | sz16((uint16_t)r);
        return cyc;
    }

    // ---- 32-bit fixed point: B op A ----
    case DADD: case DSUB: case DMUL: case DMUU: case DDIV: {
        const int64_t kMin = INT32_MIN;
        int64_t a = (int32_t)get32(0), b = (int32_t)get32(1);
        uint32_t ua = (uint32_t)a, ub = (uint32_t)b;
        uint8_t st = 0;
        int64_t r = 0;
        unsigned cyc = 0;
        switch (op) {
        case DADD:
            r = b + a;
            if ((uint64_t)ub + ua > 0xFFFFFFFFu) st |= kCarry;
            if (r != (int32_t)r) st |= kOverflow;
            cyc = 22;
            break;
        case DSUB:
            r = b - a;
            if (ub < ua) st |= kCarry;
            if (r != (int32_t)r || a == kMin) st |= kOverflow;
            cyc = 40;
            break;
        case DMUL:
            if (a == kMin || b == kMin) { r = kMin; st |= kOverflow; }
            else { r = b * a; if (r != (int32_t)r) st |= kOverflow; }
            cyc = 210;
            break;
        case DMUU:
            if (a == kMin || b == kMin) { r = kMin; st |= kOverflow; }  // "R is meaningless"
            else r = (b * a) >> 32;
            cyc = 218;
            break;
        default:  // DDIV
            if (a == 0) { r = b; st |= kDivZero; cyc = 18; break; }
            if (a == kMin || b == kMin) st |= kOverflow;  // "R will be meaningless"
            r = b / a;
            cyc = 210;
            break;
        }
        drop(4);
        set32(0, (uint32_t)r);
        status_ = st | sz32((uint32_t)r);
        return cyc;
    }

    // ---- 32-bit floating point: B op A ----
    case FADD: case FSUB: case FMUL: case FDIV: {
        double a = fromFloat(get32(0)), b = fromFloat(get32(1));
        uint8_t st = 0;
        uint32_t r;
        unsigned cyc;
        if (op == FDIV && a == 0) {
            r = get32(1);  // "R is set equal to B"
            st = kDivZero;
            cyc = 22;
        } else {
            double x = op == FADD ? b + a : op == FSUB ? b - a : op == FMUL ? b * a : b / a;
            r = toFloat(x, &st);
            static const unsigned kTop[] = {368, 370, 168, 184};
            static const unsigned kZeroA[] = {24, 26, 168, 184};
            cyc = a == 0 ? kZeroA[op - FADD] : kTop[op - FADD];
        }
        drop(4);
        set32(0, r);
        status_ = st | szF(r);
        return cyc;
    }

    // ---- the derived functions: TOS = f(TOS); PWR: TOS = B ^ A ----
    case SQRT: case SIN: case COS: case TAN: case ASIN: case ACOS: case ATAN:
    case LOG: case LN: case EXP: case PWR: {
        double a = fromFloat(get32(0));
        uint8_t err = 0;
        bool keep = false;  // an argument error leaves the operand in place
        double r = 0;
        unsigned cyc = 0;
        switch (op) {
        case SQRT:
            cyc = 870;
            if (a < 0) { err = kNegArg; keep = true; } else r = fsqrt(a);
            break;
        case SIN:
            if (std::fabs(a) <= std::ldexp(1.0, -12)) { r = a; cyc = 30; }  // sin A = A
            else { r = fsin(a); cyc = 4808; }
            break;
        case COS:
            r = fcos(a);
            cyc = 4878;
            break;
        case TAN:
            if (std::fabs(a) <= std::ldexp(1.0, -12)) { r = a; cyc = 30; break; }
            {
                bool pole = false;
                r = ftan(a, &pole);
                // At an exact pole the result is not documented; A is left with overflow.
                if (pole) { err = kOverflow; keep = true; }
            }
            cyc = 5886;
            break;
        case ASIN:
        case ACOS:
            cyc = op == ASIN ? 7938 : 8284;
            if (std::fabs(a) > 1) { err = kArgRange; keep = true; break; }
            r = op == ASIN ? fasin(a) : facos(a);
            break;
        case ATAN:
            r = fatan(a);
            cyc = 6536;
            break;
        case LN:
        case LOG:
            if (a <= 0) { err = kNegArg; keep = true; cyc = 20; break; }
            r = fln(a);
            if (op == LOG) r = mul(r, kLog10e);  // ch.9.12
            cyc = op == LN ? 6956 : 7132;
            break;
        case EXP:
            if (std::fabs(a) > 32) { err = kArgRange; keep = true; cyc = 34; break; }
            r = fexp(a);
            cyc = 4878;
            break;
        default: {  // PWR: B ^ A = EXP(A * LN B) (ch.9.13)
            double b = fromFloat(get32(1));
            cyc = 12032;
            uint32_t rb = get32(1);
            drop(4);  // A is consumed either way; R replaces B
            if (b <= 0) { err = kNegArg; set32(0, rb); status_ = err | szF(rb); return cyc; }
            double y = mul(a, fln(b));
            if (std::fabs(y) > 32) { err = kArgRange; set32(0, rb); status_ = err | szF(rb); return cyc; }
            uint32_t rv = toFloat(fexp(y), &err);
            set32(0, rv);
            status_ = err | szF(rv);
            return cyc;
        }
        }
        uint32_t rv = get32(0);
        if (!keep) {
            uint8_t range = 0;
            rv = toFloat(r, &range);
            err |= range;
            set32(0, rv);
        }
        status_ = err | szF(rv);
        return cyc;
    }

    // ---- conversions ----
    case FIXS: case FIXD: {
        uint32_t a = get32(0);
        double t = std::trunc(fromFloat(a));  // the integer portion of A
        double lim = op == FIXS ? 32768.0 : 2147483648.0;
        if (std::fabs(t) >= lim) {  // larger than 15 [31] bits: A is not changed
            status_ = kOverflow | szF(a);
            return op == FIXS ? 214 : 336;
        }
        drop(4);
        if (op == FIXS) {
            lift(2);
            set16(0, (uint16_t)(int16_t)t);
            status_ = sz16(get16(0));
            return 214;
        }
        lift(4);
        set32(0, (uint32_t)(int32_t)t);
        status_ = sz32(get32(0));
        return 336;
    }
    case FLTS: {
        double a = (int16_t)get16(0);
        drop(2);
        lift(4);
        set32(0, toFloat(a));
        status_ = szF(get32(0));
        return 156;
    }
    case FLTD: {
        set32(0, toFloat((int32_t)get32(0)));  // rounded when it has more than 24 bits
        status_ = szF(get32(0));
        return 342;
    }

    // ---- change sign ----
    case CHSS: {
        uint16_t a = get16(0);
        if (a == 0x8000) status_ = kOverflow | sz16(a);  // no positive equivalent
        else { set16(0, (uint16_t)-a); status_ = sz16(get16(0)); }
        return 24;
    }
    case CHSD: {
        uint32_t a = get32(0);
        if (a == 0x80000000u) status_ = kOverflow | sz32(a);
        else { set32(0, 0u - a); status_ = sz32(get32(0)); }
        return 28;
    }
    case CHSF: {
        uint32_t a = get32(0);
        if (a & 0x800000) set32(0, a ^ 0x80000000u);  // zero (mantissa MSB 0): no change
        status_ = szF(get32(0));
        return 20;
    }

    // ---- stack manipulation ----
    case PTOS: { uint16_t a = get16(0); lift(2); set16(0, a); status_ = sz16(a); return 16; }
    case PTOD: { uint32_t a = get32(0); lift(4); set32(0, a); status_ = sz32(a); return 20; }
    case PTOF: { uint32_t a = get32(0); lift(4); set32(0, a); status_ = szF(a); return 20; }
    case POPS: drop(2); status_ = sz16(get16(0)); return 10;
    case POPD: drop(4); status_ = sz32(get32(0)); return 12;
    case POPF: drop(4); status_ = szF(get32(0)); return 12;
    case XCHS: { uint16_t a = get16(0); set16(0, get16(1)); set16(1, a); status_ = sz16(get16(0)); return 18; }
    case XCHD: { uint32_t a = get32(0); set32(0, get32(1)); set32(1, a); status_ = sz32(get32(0)); return 26; }
    case XCHF: { uint32_t a = get32(0); set32(0, get32(1)); set32(1, a); status_ = szF(get32(0)); return 26; }
    case PUPI: lift(4); set32(0, toFloat(kPi)); status_ = szF(get32(0)); return 16;

    default:
        return 4;  // "results are undefined": nothing is done
    }
}

}  // namespace altair
