#include "boards/compupro-disk1a.h"

#include "core/clock.h"
#include "core/statefile.h"
#include "core/value.h"

namespace altair {

namespace {

constexpr int kMotorSecs = 15;  // the Motor Control Register's timeout (§2.3)

// Drive Select Register bits (§2.3).
constexpr uint8_t kDsrForce2s = 0x08, kDsrMini = 0x20;

// Board status register bits (§2.1). Bits 3-6 are "not used" and read 0 here.
constexpr uint8_t kStReady = 0x01, kStIndex = 0x02, kStSense = 0x04, kStInt = 0x80;

const CompuProDiskBoard::Traits kTraits = {
    "Disk 1A",
    0x2000,  // a 2764: 16 routines of 512 bytes
    0x0200,
    "builtin:DISK1A",
    "a 2764 (8 KB, 16 routines of 512 bytes)",
    "The boot EPROM: builtin:DISK1A (default), or a 2764 image file (Intel HEX, S-records or "
    "binary)",
    "S3-2..7",
    "Boot routine (S1-2..5): the 512-byte page of the EPROM at 0000. 0-3 are 8080/Z80 code",
    "Boot EPROM enable (S3-8). On (default): the ROM answers reads at 0000-01FF over PHANTOM* "
    "after a reset. Off: a plain disk controller",
    0,
};

} // namespace

CompuProDisk1aBoard::CompuProDisk1aBoard() : CompuProDiskBoard(kTraits) {
    irq_ = IrqJumper::Vi4;  // J10 position 4, as shipped
}

CompuProDisk1aBoard::~CompuProDisk1aBoard() {
    if (clock_) clock_->cancel(motorWake_);
}

// ---------------------------------------------------------------------------
// The board's own registers (§2.1, §2.3).
// ---------------------------------------------------------------------------
// Port 3 is write-only: nothing on the board drives the bus for an IN there.
bool CompuProDisk1aBoard::decodesPort(uint8_t off, bool write) const {
    return write || off != 3;
}

uint8_t CompuProDisk1aBoard::readPort(uint8_t off) {
    if (off != 2) return 0xFF;
    // Ready and index are the lines of the drive on the cable now (§2.1).
    const int    unit = cableUnit(chip_.unitSelect());
    const Drive& d    = drive_[(size_t)unit];
    uint8_t      v    = 0;
    if (d.drv.ready()) v |= kStReady;
    if (motorOn(unit) && d.drv.index()) v |= kStIndex;
    if (!sense_) v |= kStSense;  // S3-1: ON reads 0
    if (chip_.intPin()) v |= kStInt;
    return v;
}

void CompuProDisk1aBoard::writePort(uint8_t off, uint8_t v) {
    if (off == 0) {
        dsr_ = v;
        applyDataRate();
    } else if (off == 3) {
        motor_ = v & 0xF0;
    }
}

bool CompuProDisk1aBoard::forceTwoSided() const {
    return (dsr_ & kDsrForce2s) != 0;
}

long long CompuProDisk1aBoard::fdcClockHz() const {
    return (dsr_ & kDsrMini) ? 4000000 : 8000000;
}

// Motor bits 4-6 are 8" drives 0-2; bit 7 is 8" drive 3 AND every 5.25" drive (§2.3). J11 is
// only in the 5.25" MTR ON line: with it out, the 5.25" motors run all the time, and the 8"
// motors still follow their bits and the timer.
bool CompuProDisk1aBoard::motorOn(int unit) const {
    const Drive& d = drive_[(size_t)unit];
    if (d.eightInch) return (motor_ & (0x10 << unit)) != 0;
    return !j11_ || (motor_ & 0x80) != 0;
}

// ---------------------------------------------------------------------------
// The motor timer: any access restarts it, and ~15 s after the last one every motor stops
// (§2.3). The deadline is armed once and re-armed only when it fires early, so a guest
// polling the status register does not touch the clock's queue on every IN.
// ---------------------------------------------------------------------------
void CompuProDisk1aBoard::touched() {
    if (!clock_) return;
    motorOffAt_ = clock_->now() + clock_->tStatesPer(1) * kMotorSecs;
    if (motor_ && !clock_->pending(motorWake_))
        motorWake_ = clock_->at(motorOffAt_, [this] { motorCheck(); });
}

void CompuProDisk1aBoard::motorCheck() {
    motorWake_ = Clock::kNone;
    if (!clock_ || !motor_) return;
    if (clock_->now() >= motorOffAt_) {
        motor_ = 0;
        return;
    }
    motorWake_ = clock_->at(motorOffAt_, [this] { motorCheck(); });
}

// Any reset clears both registers: INIT clears U6 and U29 (§2.3, from the 1985 schematic).
void CompuProDisk1aBoard::resetRegisters() {
    dsr_   = 0;
    motor_ = 0;
    if (clock_) {
        clock_->cancel(motorWake_);
        motorWake_ = Clock::kNone;
    }
}

// ---------------------------------------------------------------------------
// Properties.
// ---------------------------------------------------------------------------
void CompuProDisk1aBoard::boardProperties(std::vector<Property>& p) {
    {
        Property x;
        x.name = "sense_switch";
        x.help = "S3-1, read in status bit 2. On (default, reads 0): the CompuPro ROM tells the "
                 "OS to use a System Support 1 console. Off: an Interfacer 3/4";
        x.kind = Kind::Bool;
        x.get  = [this] { return Value::ofBool(sense_); };
        x.set  = [this](const Value& v, std::string&) {
            sense_ = v.b();
            return true;
        };
        p.push_back(std::move(x));
    }
    {
        Property x;
        x.name = "mini_motor_control";
        x.help = "Jumper J11, in the 5.25\" MTR ON line. On (default, J11 installed): bit 7 of the "
                 "Motor Control Register runs the 5.25\" motors, and they stop about 15 seconds "
                 "after the last access. Off: the 5.25\" motors run all the time. The 8\" motors "
                 "always follow bits 4-7";
        x.kind = Kind::Bool;
        x.get  = [this] { return Value::ofBool(j11_); };
        x.set  = [this](const Value& v, std::string&) {
            j11_ = v.b();
            return true;
        };
        p.push_back(std::move(x));
    }
    p.push_back(irqJumperProperty("interrupt", "Where the 765's INT goes (J10; CompuPro used vi4)", irq_));
}

std::vector<MapEntry> CompuProDisk1aBoard::ioMap() const {
    return {
        {(uint32_t)port_ + 0, (uint32_t)port_ + 0, "read/write", "765 main status (in) / drive select (out)"},
        {(uint32_t)port_ + 1, (uint32_t)port_ + 1, "read/write", "765 data"},
        {(uint32_t)port_ + 2, (uint32_t)port_ + 2, "read/write", "board status (in) / DMA address, 3 bytes MSB first (out)"},
        {(uint32_t)port_ + 3, (uint32_t)port_ + 3, "write", "motor control; D0 = 0 switches the boot ROM off"},
    };
}

// ---------------------------------------------------------------------------
// SNAPSHOT/RESTORE (DESIGN.md 13): the registers, the counter, the ROM flip-flop, the
// motor deadline, each head's position and the chip. The images are host-backed and the
// straps come from the config.
// ---------------------------------------------------------------------------
void CompuProDisk1aBoard::serialize(StateWriter& w) const {
    Board::serialize(w);
    w.u8(dsr_);
    w.u8(motor_);
    w.u32(dma_);
    w.boolean(romOn_);
    w.u64(motorOffAt_);
    serializeHeads(w);
    chip_.serialize(w);
}

void CompuProDisk1aBoard::deserialize(StateReader& r) {
    Board::deserialize(r);
    dsr_        = r.u8();
    motor_      = r.u8();
    dma_        = r.u32() & 0xFFFFFFu;
    romOn_      = r.boolean();
    motorOffAt_ = r.u64();
    deserializeHeads(r);
    chip_.deserialize(r);
    applyDataRate();
    if (clock_) {
        clock_->cancel(motorWake_);
        motorWake_ = Clock::kNone;
        if (motor_) motorWake_ = clock_->at(motorOffAt_, [this] { motorCheck(); });
    }
    refresh();
    decodeChanged();
}

} // namespace altair
