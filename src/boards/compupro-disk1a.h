#pragma once
//
// CompuPro Disk 1A -- a floppy disk controller board (board 203, 1984). See
// reference/CompuPro Disk 1 & 1A Floppy Controllers.md; the section numbers below are that
// file's.
//
// THREE THINGS ON ONE BOARD, all around an NEC µPD765A (chips/upd765.h):
//
//   - FOUR PORTS at a base that is a multiple of four (default C0), §2:
//       C0  IN  the 765's main status register     OUT  the Drive Select Register
//       C1  the 765's data register
//       C2  IN  the board status register          OUT  the DMA address, 3 bytes, MSB first
//       C3  OUT the Motor Control Register (D0 = 0 also switches the boot ROM off)
//   - A DMA ENGINE. The 765's DRQ asks for the bus; the board takes it and moves one byte
//     per granted cycle to or from the address in a 24-bit counter. There is no byte count
//     and no TC: a read runs to the EOT the guest gave, which is why the CompuPro ROM takes
//     ST0 = 40H with ST1 = EN as a good read.
//   - A BOOT ROM. 512 bytes of a 2764, picked by S1-2..5 (`routine`), answer memory reads at
//     0000-01FF over PHANTOM* until the guest writes D0 = 0 to the Motor Control Register.
//     Any reset turns it on again (§3).
//
// THE DRIVE SELECT REGISTER (§2.3) sits between the 765's US0/US1/HD pins and the drives,
// which is why the chip reaches its drives through Upd765::Wiring and not an array:
//   bit 2 (alternate select) puts bits 0-1 on the unit lines instead of the 765's;
//   bit 3 (Force Two Sided) holds the 765's TS input high;
//   bit 5 (5.25" data rate) runs the 765 at 4 MHz instead of 8.
//
// WHAT IS NOT MODELED, AND WHY EACH IS SAFE TO LEAVE OUT:
//   - A16-A23. The counter counts all 24 bits, but this is a 64 K bus (DESIGN.md 4.0), so
//     only A0-A15 reach memory.
//   - WAIT STATES AND DMA PRIORITY (S2). One bus master and no slow memory: nothing to wait
//     for and nobody to arbitrate with. A DMA byte costs one 3-state bus cycle.
//   - ALTERNATE HEAD LOAD (bit 4). The drives here have no head-load solenoid to load.
//   - THE MOTOR AND READY. A motor that is off stops the index pulse in the status register;
//     it does not take READY away. An 8" drive keeps READY with its motor stopped (the
//     HMXBIOS SOFTRDY comment says so); a 5.25" drive would lose it, and is not modeled.
//   - THE DATA RATE AGAINST THE MEDIA. A 5.25" disk read at the 8" rate fails on the board;
//     here the rate sets only the 765's Specify timing.

#include "boards/floppy-drive.h"
#include "chips/upd765.h"
#include "core/board.h"
#include "host/disk.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace altair {

class CompuProDisk1aBoard : public Board {
public:
    CompuProDisk1aBoard();
    ~CompuProDisk1aBoard() override;

    std::string type() const override { return "disk1a"; }

    // ---- the bus: the four ports, and the boot ROM's memory reads ----
    bool    decodes(const BusCycle& c) const override;
    uint8_t read(const BusCycle& c) override;
    void    write(const BusCycle& c) override;
    bool    assertsPhantom(const BusCycle& c) const override;
    bool    peek(uint16_t addr, uint8_t& out) const override;

    bool    assertsInt() const override;
    uint8_t assertsVi() const override;

    // ---- pHOLD: the 765's DRQ, in DMA mode (HAS-A Mover, as the Tarbell DD) ----
    bool       requestsBus() const override;
    BusMaster* busMaster() override { return &mover_; }

    void reset(Reset r) override;
    void power() override;
    void pump() override;
    void configChanged() override;
    void clockAttached() override;

    std::vector<Property> properties() override;
    std::vector<MapEntry> ioMap() const override;
    std::vector<MapEntry> memMap() const override;

    std::vector<UnitDef> units() const override;
    bool mount(const std::string& unit, const std::string& path, bool ro, std::string& err) override;
    bool unmount(const std::string& unit, std::string& err) override;

    std::vector<std::string> subUnitTables() const override { return {"drive"}; }
    std::vector<Property>    subUnitProperties(const std::string& table) const override;
    std::vector<SubUnit>     subUnits() const override;

    std::vector<std::string> drainLog() override;

    void serialize(StateWriter& w) const override;
    void deserialize(StateReader& r) override;

    // For tests, without going through the bus.
    Upd765&  chip() { return chip_; }
    bool     romEnabled() const { return romOn_; }
    uint32_t dmaAddress() const { return dma_; }
    uint8_t  motors() const { return motor_; }

    static constexpr int kDrives = 4;

protected:
    bool addSubUnit(const std::string& table, const KeyValues& kv, std::string& err) override;

private:
    struct Drive {
        std::unique_ptr<DiskImage> img;
        std::string    path;           // as WRITTEN (round-trips through SAVE)
        bool           eightInch = true;
        DiskImageDrive drv;
    };

    // The unit the drive cable carries: the Drive Select Register's when bit 2 is set,
    // else the 765's own US1/US0.
    int  cableUnit(int us) const { return (dsr_ & 0x04) ? (dsr_ & 0x03) : (us & 0x03); }
    bool motorOn(int unit) const;
    void applyDataRate();

    // A byte the board answers on any port: the motor timer restarts (§2.3).
    void touch();
    void motorCheck();

    void refresh();  // the chip up to now, the wires re-driven, the next deadline armed
    bool loadRom(const std::string& spec, std::string& err);

    StepResult transferOne(Bus& bus);

    // HAS-A, NOT IS-A: Machine::master()'s dynamic_cast must not take the board for a CPU.
    struct Mover : public BusMaster {
        CompuProDisk1aBoard* d = nullptr;
        StepResult step(Bus& bus) override { return d->transferOne(bus); }
    } mover_;

    Upd765 chip_{"fdc"};
    std::array<Drive, kDrives> drive_;

    // ---- straps ----
    uint8_t     port_     = 0xC0;
    std::string romSpec_  = "builtin:DISK1A";
    int         routine_  = 0;      // S1-2..5
    bool        boot_     = true;   // S3-8 ON
    bool        sense_    = true;   // S3-1 ON = System Support 1 console
    bool        timeout_  = true;   // J11 installed
    IrqJumper   irq_      = IrqJumper::Vi4;  // J10 position 4, as shipped

    // ---- runtime (snapshot) ----
    uint8_t  dsr_   = 0;      // Drive Select Register
    uint8_t  motor_ = 0;      // Motor Control Register, bits 4-7
    uint32_t dma_   = 0;      // the 24-bit DMA address counter
    bool     romOn_ = true;   // the boot ROM flip-flop
    uint64_t motorOffAt_ = 0; // when the motors stop, if nothing touches the board

    std::vector<uint8_t>     rom_;  // the whole EPROM; `routine` picks a 512-byte page
    std::vector<std::string> log_;
    Clock::Handle wake_  = Clock::kNone;
    Clock::Handle motorWake_ = Clock::kNone;
};

} // namespace altair
