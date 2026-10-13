#pragma once
//
// Newtech Model 6 Music Board -- a 6-bit D/A, an amplifier and a speaker on one S-100
// card (1977). See reference/Newtech Music Board.md and docs/boards/newtech-music.md.
//
// ONE OUTPUT PORT, and nothing else. No input port, no status, no interrupt. A program
// makes sound by writing the latch in a timed loop; the board has no oscillator and no
// timer. It holds the last value written.
//
// THE PORT ANSWERS AT FOUR ADDRESSES. The decode is A7..A4 by jumper, A3 = 0 and A2 = 1
// fixed, and A1 and A0 are not connected to the board. As supplied that is 24H, and so
// 25H, 26H and 27H too.
//
// SIX BITS ARE LATCHED: DO7..DO2, DO7 the most significant. DO1 and DO0 go nowhere, so a
// program that toggles only a low bit is silent, as it was on the real board.
//
// NOTHING CLEARS THE LATCH. Its clear pin is tied to +5 V (R13), so a bus reset leaves
// the value where it was.
//
// THE D/A IS UNSIGNED, 0 V TO ALMOST +5 V, AND AC-COUPLED. C7 (0.1 uF) into R14 + R15
// (2.025 megohms) stands between the ladder and the LM380, so a level that only sits
// there is 0 V at the amplifier. The shared Speaker (host/speaker.h) carries that
// capacitor, and the rules for sound: write() records the change with its T-state,
// pump() renders it, and a machine with no crystal (clock_hz = 0) plays nothing.

#include "core/board.h"
#include "host/speaker.h"

#include <cstdint>
#include <string>
#include <vector>

namespace altair {

class NewtechMusicBoard : public Board {
public:
    NewtechMusicBoard();

    std::string type() const override { return "music6"; }

    bool decodes(const BusCycle& c) const override;
    void write(const BusCycle& c) override;

    void power() override;
    void pump() override;

    // SNAPSHOT/RESTORE (DESIGN.md 13): the latch. The port strap is config, and the sound
    // not yet played is the host's.
    void serialize(StateWriter& w) const override;
    void deserialize(StateReader& r) override;

    std::vector<Property> properties() override;
    std::vector<std::string> statusLines() const override;
    std::vector<MapEntry> ioMap() const override;

    // For tests: the six latched bits, in DO7..DO2 (the low two bits read 0).
    uint8_t latch() const { return latch_; }

private:
    // The latch as the level the Speaker takes: 0 V..5 V around its middle.
    int8_t level() const { return (int8_t)(uint8_t)(latch_ ^ 0x80); }

    uint8_t base_  = 0x24;   // the first of the four addresses (J1..J8 as supplied)
    uint8_t latch_ = 0;      // IC3, a 74LS174: DO7..DO2
    Speaker spk_;
};

} // namespace altair
