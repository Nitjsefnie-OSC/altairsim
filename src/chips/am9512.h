#pragma once
//
// AMD Am9512 Floating Point Processor (Intel 8232) -- a CHIP, NOT A CARD.
//
// Single (32-bit) and double (64-bit) precision add, subtract, multiply and divide, plus the
// change-sign, push, pop and exchange commands, on the same 16-byte stack and two ports as
// the Am9511A (chips/am95xx.h): 4 single or 2 double operands. It has no fixed point and no
// derived functions. The CompuPro System Support 1's math socket takes one in place of a
// 9511A. Modeled from the Am9512 data sheet and AMD's Am9511A/Am9512 Floating Point
// Processor Manual (reference/AMD 9512.md), NOT from any one program that drives it.
//
// ---------------------------------------------------------------------------
// THE FORMATS are the IEEE-proposal ones: sign, a biased exponent (8 bits, bias 127; 11
// bits, bias 1023) and the mantissa with an implied 1. An exponent field of 0 is zero,
// whatever the mantissa holds. The documents define no denormals, infinities or NaNs, so
// there are none here: an all-ones exponent is a number like any other.
//
// THE ARITHMETIC is rounded to nearest, ties to even, as the data sheet says (p.7-98). A zero
// result is all zero bits (Table 3: SSUB of 1.0 from 1.0 gives 00000000).
//
// ⚠ What the result holds after an exponent overflow or underflow is NOT DOCUMENTED. The
// model keeps the rounded mantissa and wraps the exponent field (by 256 or 2048), with the
// V or U status bit -- what the Am9511A data sheet says its own float operations do. No real
// chip was available to compare against.
//
// THE CYCLE COUNTS are the top of each data-sheet range (Table 2), as the 9511A model takes
// them; the commands Table 2 does not list take Table 3's counts.
//
// WHAT IS NOT MODELED: the "undefined" stack cells a command leaves (the ring keeps what was
// there), and the bits of an undefined command byte (it does nothing and takes 4 cycles).

#include "chips/am95xx.h"

#include <cstdint>
#include <string>

namespace altair {

class Am9512 : public Am95xx {
public:
    explicit Am9512(std::string name) : Am95xx(std::move(name), kDivide | kUnderflow | kOverflow) {}

    // The two formats <-> a host double, for tests and the monitor. The encoders round to
    // nearest-even and wrap an out-of-range exponent; `err` gets kOverflow or kUnderflow.
    static uint32_t toSingle(double x, uint8_t* err = nullptr);
    static uint64_t toDouble(double x, uint8_t* err = nullptr);
    static double   fromSingle(uint32_t v);
    static double   fromDouble(uint64_t v);

    // The error bits of the status register (p.7-96); BUSY, SIGN and ZERO are Am95xx's.
    // Each one drives ERR.
    static constexpr uint8_t kDivide    = 0x08;  // D: divide by zero, the only divide exception
    static constexpr uint8_t kUnderflow = 0x04;  // U
    static constexpr uint8_t kOverflow  = 0x02;  // V

private:
    unsigned execute(uint8_t op) override;
};

}  // namespace altair
