#pragma once
//
// AMD Am9511A Arithmetic Processing Unit (Intel 8231A) -- a CHIP, NOT A CARD.
//
// 16- and 32-bit fixed point, the chip's own 32-bit floating-point format, eleven derived
// functions (SQRT, SIN ... PWR) and the stack commands, all worked on a 16-byte LIFO stack
// reached through two ports. The CompuPro System Support 1 carries one in its math socket.
// Modeled from the Am9511A data sheet and AMD's Am9511A/Am9512 Floating Point Processor
// Manual (reference/AMD 9511A.md), NOT from any one program that drives it.
//
// It knows nothing about S-100. The ports, the stack, END/SVREQ and BUSY are the bus side it
// shares with the Am9512 (chips/am95xx.h); this file is the command set. On the SS-1 CLK is
// the 2 MHz bus clock (pin 49).
//
// ---------------------------------------------------------------------------
//
// The data sheet gives most cycle counts as a range ("FADD 54-368"). The model takes the TOP
// of each range: it is the time a program that times the chip by a delay loop had to allow.
// A short path the data sheet names (FADD with A = 0, SIN of a tiny angle) takes its own count.
//
// ---------------------------------------------------------------------------
// ARITHMETIC
//
//   * Fixed point is exact two's complement, with the data sheet's overflow rules.
//   * The four float operations are rounded to the 24-bit mantissa, nearest-even. The 9511A
//     data sheet does not say how it rounds (the 9512's says nearest-even); see the ⚠ in
//     reference/AMD 9511A.md. A result too big or too small keeps its mantissa and its
//     exponent wraps by 128, with the overflow or underflow error, as the data sheet says.
//   * The derived functions follow the Manual's ch.9 algorithms -- range reduction, the
//     Chebyshev series with its coefficients, then post-processing -- with every step rounded
//     to 24 bits as the chip's own float operations round it. Their low bits are as close to
//     silicon as the documents allow; no real chip was available to compare against.
//   * FIXS/FIXD take "the integer portion" of A: the fraction is dropped (toward zero).
//   * A float whose mantissa MSB is 0 is zero (the data sheet's test for CHSF).
//
// WHAT IS NOT MODELED: the scratch values a command leaves in its "lost" stack cells (the
// data sheet does not give them; the ring simply keeps what was there), and the bits of an
// undefined command byte (it does nothing and takes 4 cycles, like NOP, but leaves status).

#include "chips/am95xx.h"

#include <cstdint>
#include <string>

namespace altair {

class Am9511a : public Am95xx {
public:
    explicit Am9511a(std::string name) : Am95xx(std::move(name), 0) {}  // no ERR pin

    // The chip's float format <-> a host double. toFloat rounds to nearest-even and wraps
    // an out-of-range exponent by 128; `err` gets the overflow (0x02) or underflow (0x04)
    // bit when it does.
    static uint32_t toFloat(double x, uint8_t* err = nullptr);
    static double   fromFloat(uint32_t v);

    // The error bits of the status register (data sheet p.7-71); BUSY, SIGN and ZERO are
    // Am95xx's.
    static constexpr uint8_t kDivZero   = 0x10;  // error code 1000
    static constexpr uint8_t kNegArg    = 0x08;  // error code 0100
    static constexpr uint8_t kArgRange  = 0x18;  // error code 1100
    static constexpr uint8_t kUnderflow = 0x04;  // error code XX10
    static constexpr uint8_t kOverflow  = 0x02;  // error code XX01
    static constexpr uint8_t kCarry     = 0x01;

private:
    unsigned execute(uint8_t op) override;
};

}  // namespace altair
