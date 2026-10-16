#pragma once
//
// CompuPro Disk 1A -- a floppy disk controller board (board 203, 1984). See
// reference/CompuPro Disk 1 & 1A Floppy Controllers.md; the section numbers below are that
// file's. What it shares with the Disk 1 -- the 765, the DMA engine, the boot ROM window,
// the drives -- is in boards/compupro-disk.h.
//
// THE PORTS (§2), at a base that is a multiple of four (default C0):
//       C0  IN  the 765's main status register     OUT  the Drive Select Register
//       C1  the 765's data register
//       C2  IN  the board status register          OUT  the DMA address, 3 bytes, MSB first
//       C3  OUT the Motor Control Register (D0 = 0 also switches the boot ROM off)
//
// THE BOOT ROM is 512 bytes of a 2764, picked by S1-2..5 (`routine`), at 0000-01FF.
//
// THE DRIVE SELECT REGISTER (§2.3) sits between the 765's US0/US1/HD pins and the drives,
// which is why the chip reaches its drives through Upd765::Wiring and not an array:
//   bit 2 (alternate select) puts bits 0-1 on the unit lines instead of the 765's;
//   bit 3 (Force Two Sided) holds the 765's TS input high;
//   bit 5 (5.25" data rate) runs the 765 at 4 MHz instead of 8.
//
// WHAT IS NOT MODELED HERE, AND WHY EACH IS SAFE TO LEAVE OUT (and see compupro-disk.h):
//   - ALTERNATE HEAD LOAD (bit 4). The drives here have no head-load solenoid to load.
//   - THE MOTOR AND READY. A motor that is off stops the index pulse in the status register;
//     it does not take READY away. An 8" drive keeps READY with its motor stopped (the
//     HMXBIOS SOFTRDY comment says so); a 5.25" drive would lose it, and is not modeled.
//   - J5 AND J12, both out as shipped. J5 holds the 5.25" READY on for unit 3; J12 lets the
//     motor timer's first stage clear the Drive Select Register after about 2 s.

#include "boards/compupro-disk.h"

#include <cstdint>
#include <string>
#include <vector>

namespace altair {

class CompuProDisk1aBoard : public CompuProDiskBoard {
public:
    CompuProDisk1aBoard();
    ~CompuProDisk1aBoard() override;

    std::string type() const override { return "disk1a"; }

    std::vector<MapEntry> ioMap() const override;

    void serialize(StateWriter& w) const override;
    void deserialize(StateReader& r) override;

    // For tests, without going through the bus.
    uint8_t motors() const { return motor_; }

protected:
    bool      decodesPort(uint8_t off, bool write) const override;
    uint8_t   readPort(uint8_t off) override;
    void      writePort(uint8_t off, uint8_t v) override;
    int       cableUnit(int us) const override { return (dsr_ & 0x04) ? (dsr_ & 0x03) : (us & 0x03); }
    bool      forceTwoSided() const override;
    long long fdcClockHz() const override;
    void      touched() override;
    void      resetRegisters() override;
    void      boardProperties(std::vector<Property>& p) override;

private:
    bool motorOn(int unit) const;
    void motorCheck();

    // ---- straps ----
    bool sense_   = true;   // S3-1 ON = System Support 1 console
    bool j11_     = true;   // J11 installed: bit 7 runs the 5.25" motors

    // ---- runtime (snapshot) ----
    uint8_t  dsr_   = 0;      // Drive Select Register
    uint8_t  motor_ = 0;      // Motor Control Register, bits 4-7
    uint64_t motorOffAt_ = 0; // when the motors stop, if nothing touches the board

    Clock::Handle motorWake_ = Clock::kNone;
};

} // namespace altair
