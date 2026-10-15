#include "boards/compupro-disk1a.h"

#include "core/bus.h"
#include "core/clock.h"
#include "core/hex.h"
#include "core/roms.h"
#include "core/statefile.h"
#include "core/value.h"
#include "host/media.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>

namespace altair {

namespace {

constexpr uint32_t kRomBytes  = 0x2000;  // a 2764: 16 routines of 512 bytes
constexpr uint32_t kRomWindow = 0x0200;  // one routine, at the Z80/8080 reset address
constexpr int      kMotorSecs = 15;      // the Motor Control Register's timeout (§2.3)

// A DMA byte is one memory cycle the board drives: three bus states, no wait states.
constexpr uint32_t kDmaCycle = 3;

// Drive Select Register bits (§2.3).
constexpr uint8_t kDsrForce2s = 0x08, kDsrMini = 0x20;

// Board status register bits (§2.1). Bits 3-6 are "not used" and read 0 here.
constexpr uint8_t kStReady = 0x01, kStIndex = 0x02, kStSense = 0x04, kStInt = 0x80;

int driveIndex(const std::string& unit) {
    if (unit.size() != 6 || unit.rfind("drive", 0) != 0) return -1;
    const char ch = unit[5];
    return (ch >= '0' && ch < '0' + CompuProDisk1aBoard::kDrives) ? ch - '0' : -1;
}

// ---------------------------------------------------------------------------
// THE SIZE PROBE. A raw image carries no geometry, so its byte count decides it, from the
// formats CompuPro's own BIOS knows (HMXBIOS MAKEDPB, ACTIVE.LIB TRK5 = 80):
//
//   8"    77 cylinders. Cylinder 0 head 0 is always SD 26 x 128 (the IBM 3740 index
//         track, and what the boot ROM reads). Every other track is the disk's format:
//         SD 26 x 128, or DD 26 x 256, 15 x 512, 8 x 1024.
//   5.25" 80 cylinders, one format throughout: SD 16 x 128, or DD 16 x 256, 8 x 512,
//         5 x 1024. (The BIOS blocks every 5.25" track; there is no SD track 0.)
//
// Either size, one or two sides. TWO SIDES ARE CYLINDER-INTERLEAVED (C0H0, C0H1, C1H0...):
// the BIOS turns a logical track into cylinder t/2, head t&1, and the CP/M directory of
// the double-sided CCPM22Q disk is at slot 4 of the file, which is cylinder 2 head 0 only
// in that order.
//
// Three 5.25" formats share 327,680 bytes and two share 655,360, so those sizes are
// refused rather than guessed. A smaller image of no known size is a BLANK disk: READY,
// steppable, and formattable track by track with Format a Track. A blank has the reach of the
// largest disk, 80 cylinders and two sides, so a 5.25" two-sided FORMAT can write every track.
// A slot that is never formatted takes no bytes (DiskImage::rebuild), so a one-sided or
// 77-cylinder format still lays out end to end, and the size probe reads it back.
// ---------------------------------------------------------------------------
struct Format {
    bool    eightInch;
    Density density;
    int     sectors, size;
};

constexpr Format kFormats[] = {
    {true,  Density::SD, 26, 128},  {true,  Density::DD, 26, 256},
    {true,  Density::DD, 15, 512},  {true,  Density::DD,  8, 1024},
    {false, Density::SD, 16, 128},  {false, Density::DD, 16, 256},
    {false, Density::DD,  8, 512},  {false, Density::DD,  5, 1024},
};

uint64_t formatBytes(const Format& f, int heads) {
    const uint64_t track = (uint64_t)f.sectors * (uint64_t)f.size;
    if (!f.eightInch) return 80ull * (uint64_t)heads * track;
    return 26ull * 128 + (77ull * (uint64_t)heads - 1) * track;
}

uint64_t largestImage() {
    uint64_t m = 0;
    for (const Format& f : kFormats) m = std::max(m, formatBytes(f, 2));
    return m;
}

std::string describe(const Format& f, int heads) {
    return std::string(f.eightInch ? "8\" " : "5.25\" ") + (heads == 2 ? "DS" : "SS") +
           (f.density == Density::SD ? "SD " : "DD ") + std::to_string(f.sectors) + " x " +
           std::to_string(f.size);
}

// Set up `img` from its size; false (and `err`) when the size is no disk this board reads.
bool probe(DiskImage& img, bool& eightInch, std::string& err) {
    const uint64_t bytes = img.size();
    const Format*  hit   = nullptr;
    int            heads = 0;
    std::string    all;
    int            count = 0;
    for (const Format& f : kFormats)
        for (int h = 1; h <= 2; ++h)
            if (sizeMatches(bytes, formatBytes(f, h))) {
                if (!hit) { hit = &f; heads = h; }
                all += (count++ ? ", " : "") + describe(f, h);
            }
    if (count > 1) {
        err = std::to_string(bytes) + " bytes fits more than one 5.25\" format (" + all +
              "), and a raw image cannot say which.";
        return false;
    }
    if (hit) {
        eightInch = hit->eightInch;
        const int cyls = eightInch ? 77 : 80;
        img.init(cyls, heads, true);
        if (eightInch) {
            img.initFormat(0, 0, 0, 0, Density::SD, 26, 128, 1);
            if (heads == 2)
                img.initFormat(0, 0, 1, 1, hit->density, hit->sectors, hit->size, 1);
            img.initFormat(1, cyls - 1, 0, heads - 1, hit->density, hit->sectors, hit->size, 1);
        } else {
            img.initFormat(0, cyls - 1, 0, heads - 1, hit->density, hit->sectors, hit->size, 1);
        }
        return true;
    }
    if (bytes < largestImage()) {
        eightInch = true;
        img.init(80, 2, true);  // blank: no tracks formatted yet
        return true;
    }
    err = std::to_string(bytes) + " bytes is larger than any disk the Disk 1A reads (" +
          std::to_string(largestImage()) + " = 8\" DSDD 8 x 1024).";
    return false;
}

} // namespace

// ---------------------------------------------------------------------------
// Construction: what the 765's pins reach (Upd765::Wiring), through the Drive Select
// Register.
// ---------------------------------------------------------------------------
CompuProDisk1aBoard::CompuProDisk1aBoard() {
    mover_.d = this;
    chip_.wiring.drive = [this](int us) -> FloppyDrive* { return &drive_[(size_t)cableUnit(us)].drv; };
    chip_.wiring.head  = [this](int us, int hd) { drive_[(size_t)cableUnit(us)].drv.setSide(hd); };
    // TS is the disk's: two-sided when side 1 has a formatted track. A blank reaches both
    // sides, but reads one-sided until a FORMAT writes side 1, as a new 8" SS disk does.
    chip_.wiring.twoSided = [this](int us) {
        if (dsr_ & kDsrForce2s) return true;
        const DiskImage* img = drive_[(size_t)cableUnit(us)].img.get();
        TrackFormat      f;
        for (int t = 0; img && img->heads() > 1 && t < img->tracks(); ++t)
            if (img->trackFormat(t, 1, f)) return true;
        return false;
    };
    rom_.assign(kRomBytes, 0xFF);
}

CompuProDisk1aBoard::~CompuProDisk1aBoard() {
    // The Clock holds lambdas with `this` in them; the board can be pulled from a live machine.
    if (clock_) {
        clock_->cancel(wake_);
        clock_->cancel(motorWake_);
    }
}

void CompuProDisk1aBoard::clockAttached() {
    chip_.attachClock(clock_);
    for (Drive& d : drive_) d.drv.setClock(clock_);
}

void CompuProDisk1aBoard::applyDataRate() {
    chip_.fdcClockHz = (dsr_ & kDsrMini) ? 4000000 : 8000000;
}

// Motor bits 4-6 are 8" drives 0-2; bit 7 is 8" drive 3 AND every 5.25" drive (§2.3).
// With the timeout strapped out, the motors run all the time.
bool CompuProDisk1aBoard::motorOn(int unit) const {
    if (!timeout_) return true;
    const Drive& d = drive_[(size_t)unit];
    return (motor_ & (d.eightInch ? (0x10 << unit) : 0x80)) != 0;
}

// ---------------------------------------------------------------------------
// The bus.
// ---------------------------------------------------------------------------
bool CompuProDisk1aBoard::decodes(const BusCycle& c) const {
    if (!enabled_) return false;
    if (c.type == Cycle::IoRead || c.type == Cycle::IoWrite) {
        const uint8_t p = c.port();
        if (p < port_) return false;
        const uint8_t off = (uint8_t)(p - port_);
        // Port 3 is write-only: nothing on the board drives the bus for an IN there.
        return c.type == Cycle::IoWrite ? off < 4 : off < 3;
    }
    // The ROM answers reads only; a write under it lands in the RAM (honors_phantom = read).
    if (c.type == Cycle::MemRead) return assertsPhantom(c);
    return false;
}

// PHANTOM* is a level over the ROM's window while the ROM is on (board.h).
bool CompuProDisk1aBoard::assertsPhantom(const BusCycle& c) const {
    if (!romOn_ || !boot_) return false;
    if (c.type != Cycle::MemRead && c.type != Cycle::MemWrite) return false;
    return c.addr < kRomWindow;
}

// DISASM and EXAMINE see the ROM while it is on. The ports are never peeked: an IN from the
// data register takes a result byte.
bool CompuProDisk1aBoard::peek(uint16_t addr, uint8_t& out) const {
    if (!enabled_ || !romOn_ || !boot_ || addr >= kRomWindow) return false;
    out = rom_[(size_t)routine_ * kRomWindow + addr];
    return true;
}

uint8_t CompuProDisk1aBoard::read(const BusCycle& c) {
    if (c.type == Cycle::MemRead)
        return rom_[(size_t)routine_ * kRomWindow + (c.addr & (kRomWindow - 1))];

    chip_.poll();
    uint8_t v = 0xFF;
    switch ((uint8_t)(c.port() - port_)) {
        case 0: v = chip_.readMsr(); break;
        case 1: v = chip_.readData(); break;
        case 2: {
            // Ready and index are the lines of the drive on the cable now (§2.1).
            const int    unit = cableUnit(chip_.unitSelect());
            const Drive& d    = drive_[(size_t)unit];
            v = 0;
            if (d.drv.ready()) v |= kStReady;
            if (motorOn(unit) && d.drv.index()) v |= kStIndex;
            if (!sense_) v |= kStSense;  // S3-1: ON reads 0
            if (chip_.intPin()) v |= kStInt;
            break;
        }
        default: break;
    }
    touch();
    refresh();
    return v;
}

void CompuProDisk1aBoard::write(const BusCycle& c) {
    if (c.type != Cycle::IoWrite) return;
    chip_.poll();
    switch ((uint8_t)(c.port() - port_)) {
        case 0:
            dsr_ = c.data;
            applyDataRate();
            break;
        case 1: chip_.writeData(c.data); break;
        case 2:
            // A three-deep push-down stack, most significant byte first (§2).
            dma_ = ((dma_ << 8) | c.data) & 0xFFFFFFu;
            break;
        case 3:
            motor_ = c.data & 0xF0;
            if (!(c.data & 0x01) && romOn_) {
                romOn_ = false;
                decodeChanged();
            }
            break;
        default: break;
    }
    touch();
    refresh();
}

// ---------------------------------------------------------------------------
// The motor timer: any access restarts it, and ~15 s after the last one every motor stops
// (§2.3). The deadline is armed once and re-armed only when it fires early, so a guest
// polling the status register does not touch the clock's queue on every IN.
// ---------------------------------------------------------------------------
void CompuProDisk1aBoard::touch() {
    if (!clock_) return;
    motorOffAt_ = clock_->now() + clock_->tStatesPer(1) * kMotorSecs;
    if (timeout_ && motor_ && !clock_->pending(motorWake_))
        motorWake_ = clock_->at(motorOffAt_, [this] { motorCheck(); });
}

void CompuProDisk1aBoard::motorCheck() {
    motorWake_ = Clock::kNone;
    if (!clock_ || !timeout_ || !motor_) return;
    if (clock_->now() >= motorOffAt_) {
        motor_ = 0;
        return;
    }
    motorWake_ = clock_->at(motorOffAt_, [this] { motorCheck(); });
}

// ---------------------------------------------------------------------------
// The chip's clock discipline: advance it, re-drive INT and pHOLD, re-arm the one deadline.
// ---------------------------------------------------------------------------
void CompuProDisk1aBoard::refresh() {
    chip_.poll();
    intChanged();
    holdChanged();
    if (!clock_) return;
    clock_->cancel(wake_);
    wake_ = Clock::kNone;
    if (uint64_t e = chip_.nextEdge()) wake_ = clock_->at(e, [this] { refresh(); });
}

bool CompuProDisk1aBoard::assertsInt() const {
    return irq_ == IrqJumper::Int && chip_.intPin();
}

uint8_t CompuProDisk1aBoard::assertsVi() const {
    return chip_.intPin() ? viBit(irq_) : 0;
}

// ---------------------------------------------------------------------------
// DMA. DRQ asks for the bus (in DMA mode only: the chip never raises DRQ with ND set), and
// each granted step moves one byte at the counter, which then counts up. The command ends
// at its EOT, DRQ drops, and the run loop gives the bus back.
// ---------------------------------------------------------------------------
bool CompuProDisk1aBoard::requestsBus() const {
    return chip_.drq();
}

StepResult CompuProDisk1aBoard::transferOne(Bus& bus) {
    if (!chip_.drq()) return {0, RunStatus::Ok};
    const uint16_t addr = (uint16_t)(dma_ & 0xFFFFu);  // A16-A23 are not on this bus
    if (chip_.toMemory()) {
        uint8_t v = 0;
        if (!chip_.dackRead(v)) {
            refresh();
            return {0, RunStatus::Ok};
        }
        bus.memWrite(addr, v);
    } else {
        chip_.dackWrite(bus.memRead(addr));
    }
    dma_ = (dma_ + 1) & 0xFFFFFFu;
    refresh();
    return {kDmaCycle, RunStatus::Ok};
}

// ---------------------------------------------------------------------------
// Reset and power. RESET* reaches the 765's RST pin and turns the boot ROM back on (§3).
// ---------------------------------------------------------------------------
void CompuProDisk1aBoard::reset(Reset r) {
    if (!romOn_) {
        romOn_ = true;
        decodeChanged();
    }
    dsr_   = 0;
    motor_ = 0;
    applyDataRate();
    if (r == Reset::PowerOn) chip_.powerOn();
    else                     chip_.reset();
    if (clock_) {
        clock_->cancel(motorWake_);
        motorWake_ = Clock::kNone;
    }
    refresh();
}

void CompuProDisk1aBoard::power() {
    std::string err;
    if (!loadRom(romSpec_, err)) log_.push_back(err);
    reset(Reset::PowerOn);
}

void CompuProDisk1aBoard::pump() { refresh(); }

void CompuProDisk1aBoard::configChanged() {
    decodeChanged();
    refresh();
}

// ---------------------------------------------------------------------------
// The EPROM. `builtin:<name>` travels the ROM registry; a file is Intel HEX, S-records or a
// flat binary, placed at 0. The whole 2764 is kept and `routine` picks the page, so moving
// the switch needs no reload.
// ---------------------------------------------------------------------------
bool CompuProDisk1aBoard::loadRom(const std::string& spec, std::string& err) {
    Image img;
    if (spec.rfind("builtin:", 0) == 0) {
        const BuiltinRom* rom = findRom(spec.substr(8));
        if (!rom) {
            err = "no built-in ROM named '" + spec.substr(8) + "'. SHOW ROMS lists them.";
            return false;
        }
        if (!decodeRom(*rom, 0, img, err)) return false;
    } else {
        std::ifstream f(resolvePath(spec), std::ios::binary);
        if (!f) {
            err = "cannot open '" + spec + "'" + pathNote(spec);
            return false;
        }
        std::vector<uint8_t> raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (looksLikeHex(raw)) {
            if (!loadHex(raw, img, err)) { err = spec + ": " + err; return false; }
        } else if (looksLikeSrec(raw)) {
            if (!loadSrec(raw, img, err)) { err = spec + ": " + err; return false; }
        } else {
            loadBin(raw, 0, img);
        }
    }
    if (img.empty()) {
        err = spec + ": no bytes";
        return false;
    }
    if (img.bytes.rbegin()->first >= kRomBytes) {
        err = spec + ": runs past 1FFF. The Disk 1A's EPROM is a 2764 (8 KB, 16 routines of 512 bytes).";
        return false;
    }
    rom_.assign(kRomBytes, 0xFF);
    for (const auto& [a, b] : img.bytes) rom_[a] = b;
    return true;
}

// ---------------------------------------------------------------------------
// Properties.
// ---------------------------------------------------------------------------
std::vector<Property> CompuProDisk1aBoard::properties() {
    std::vector<Property> p;
    {
        Property x;
        x.name  = "port";
        x.help  = "Base address (S3-2..7). The board decodes four ports: BASE+0 .. BASE+3 (default C0)";
        x.kind  = Kind::Int;
        x.radix = 16;
        x.min   = 0;
        x.max   = 0xFC;
        x.get   = [this] { return Value::ofInt(port_); };
        x.set   = [this](const Value& v, std::string& err) {
            if (v.i() % 4) {
                err = "the base is a multiple of 4: the switches set A7-A2 only";
                return false;
            }
            port_ = (uint8_t)v.i();
            return true;
        };
        p.push_back(std::move(x));
    }
    {
        Property x;
        x.name = "rom";
        x.help = "The boot EPROM: builtin:DISK1A (default), or a 2764 image file (Intel HEX, "
                 "S-records or binary)";
        x.kind = Kind::Str;
        x.get  = [this] { return Value::ofStr(romSpec_); };
        x.set  = [this](const Value& v, std::string& err) {
            if (!loadRom(v.s(), err)) return false;
            romSpec_ = v.s();
            return true;
        };
        p.push_back(std::move(x));
    }
    {
        Property x;
        x.name  = "routine";
        x.help  = "Boot routine (S1-2..5): the 512-byte page of the EPROM at 0000. 0-3 are 8080/Z80 code";
        x.kind  = Kind::Int;
        x.radix = 10;
        x.min   = 0;
        x.max   = 15;
        x.get   = [this] { return Value::ofInt(routine_); };
        x.set   = [this](const Value& v, std::string&) {
            routine_ = (int)v.i();
            return true;
        };
        p.push_back(std::move(x));
    }
    {
        Property x;
        x.name = "boot";
        x.help = "Boot EPROM enable (S3-8). On (default): the ROM answers reads at 0000-01FF over "
                 "PHANTOM* after a reset. Off: a plain disk controller";
        x.kind = Kind::Bool;
        x.get  = [this] { return Value::ofBool(boot_); };
        x.set  = [this](const Value& v, std::string&) {
            boot_ = v.b();
            return true;
        };
        p.push_back(std::move(x));
    }
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
        x.name = "motor_timeout";
        x.help = "J11. On (default, J11 installed): the drive motors stop about 15 seconds after "
                 "the last access to the board. Off: the motors run all the time";
        x.kind = Kind::Bool;
        x.get  = [this] { return Value::ofBool(timeout_); };
        x.set  = [this](const Value& v, std::string&) {
            timeout_ = v.b();
            return true;
        };
        p.push_back(std::move(x));
    }
    p.push_back(irqJumperProperty("interrupt", "Where the 765's INT goes (J10; CompuPro used vi4)", irq_));
    return p;
}

std::vector<MapEntry> CompuProDisk1aBoard::ioMap() const {
    return {
        {(uint32_t)port_ + 0, (uint32_t)port_ + 0, "read/write", "765 main status (in) / drive select (out)"},
        {(uint32_t)port_ + 1, (uint32_t)port_ + 1, "read/write", "765 data"},
        {(uint32_t)port_ + 2, (uint32_t)port_ + 2, "read/write", "board status (in) / DMA address, 3 bytes MSB first (out)"},
        {(uint32_t)port_ + 3, (uint32_t)port_ + 3, "write", "motor control; D0 = 0 switches the boot ROM off"},
    };
}

std::vector<MapEntry> CompuProDisk1aBoard::memMap() const {
    if (!boot_) return {};
    return {{0x0000, kRomWindow - 1, "boot ROM",
             "one 512-byte routine of the EPROM, over PHANTOM* until OUT BASE+3 with D0 = 0"}};
}

// ---------------------------------------------------------------------------
// Units, MOUNT, UNMOUNT, and [[board.drive]].
// ---------------------------------------------------------------------------
std::vector<UnitDef> CompuProDisk1aBoard::units() const {
    std::vector<UnitDef> u;
    for (int i = 0; i < kDrives; ++i) {
        const Drive& d = drive_[(size_t)i];
        UnitDef x;
        x.name  = "drive" + std::to_string(i);
        x.kind  = UnitKind::Disk;
        x.state = d.img ? d.path : "(empty)";
        if (d.img) {
            x.readOnly       = d.img->readOnly();
            x.readOnlyForced = d.img->readOnlyForced();
        }
        u.push_back(std::move(x));
    }
    return u;
}

bool CompuProDisk1aBoard::mount(const std::string& unit, const std::string& path, bool ro,
                                std::string& err) {
    const int i = driveIndex(unit);
    if (i < 0) {
        err = "no unit `" + unit + "` on " + id + " (it has drive0..drive3)";
        return false;
    }
    auto media = openMedia(resolvePath(path), ro, err);
    if (!media) {
        err += pathNote(path);
        return false;
    }
    auto img = std::make_unique<DiskImage>(std::move(media));
    bool eight = true;
    if (!probe(*img, eight, err)) return false;  // the old disk stays in
    img->setExtendsOnWrite(true);  // a blank grows as the guest formats it

    Drive& d = drive_[(size_t)i];
    d.img       = std::move(img);
    d.path      = path;
    d.eightInch = eight;
    d.drv.mount(d.img.get(), ro);
    d.drv.setHeadTrack(0);
    d.drv.setFormatting(true);
    d.drv.setRevsPerSecond(eight ? 6 : 5);
    if (d.img->readOnlyForced()) {
        char m[192];
        std::snprintf(m, sizeof m, "drive%d mounted WRITE-PROTECTED -- the host will not let us write %s",
                      i, path.c_str());
        log_.push_back(m);
    }
    return true;
}

bool CompuProDisk1aBoard::unmount(const std::string& unit, std::string& err) {
    const int i = driveIndex(unit);
    if (i < 0) {
        err = "no unit `" + unit + "` on " + id;
        return false;
    }
    Drive& d = drive_[(size_t)i];
    if (!d.img) {
        err = id + ":" + unit + " is empty";
        return false;
    }
    d.img->sync();
    d.drv.eject();
    d.img.reset();
    d.path.clear();
    d.eightInch = true;
    return true;
}

std::vector<Property> CompuProDisk1aBoard::subUnitProperties(const std::string& table) const {
    if (table != "drive") return {};
    std::vector<Property> p;
    {
        Property x;
        x.name  = "unit";
        x.help  = "Which drive (0..3)";
        x.kind  = Kind::Int;
        x.radix = 10;
        x.min   = 0;
        x.max   = kDrives - 1;
        p.push_back(std::move(x));
    }
    {
        Property x;
        x.name = "mount";
        x.help = "The disk image to put in it. Relative to THIS FILE.";
        x.kind = Kind::Str;
        p.push_back(std::move(x));
    }
    {
        Property x;
        x.name    = "readonly";
        x.help    = "Write-protect the disk. The drive senses it, so the guest is never told";
        x.kind    = Kind::Bool;
        x.aliases = {"writeprotect"};
        p.push_back(std::move(x));
    }
    return p;
}

bool CompuProDisk1aBoard::addSubUnit(const std::string& table, const KeyValues& kv, std::string& err) {
    if (table != "drive") {
        err = type() + " has no [[board." + table + "]] table";
        return false;
    }
    int         unit = -1;
    std::string path;
    bool        ro = false;
    for (const auto& [k, v] : kv) {
        if (k == "unit") unit = std::stoi(v);
        else if (k == "mount") path = v;
        else if (k == "readonly") {
            Value       bv;
            std::string e;
            if (parseValue(v, Kind::Bool, bv, e)) ro = bv.b();
        }
    }
    if (unit < 0) {
        err = "[[board.drive]] needs a `unit`";
        return false;
    }
    if (path.empty()) return true;
    return mount("drive" + std::to_string(unit), path, ro, err);
}

std::vector<Board::SubUnit> CompuProDisk1aBoard::subUnits() const {
    std::vector<SubUnit> out;
    for (int i = 0; i < kDrives; ++i) {
        const Drive& d = drive_[(size_t)i];
        if (!d.img) continue;
        SubUnit su;
        su.table = "drive";
        su.fields.push_back({"unit", std::to_string(i), false});
        su.fields.push_back({"mount", d.path, true});
        if (d.img->readOnly()) su.fields.push_back({"readonly", "true", false});
        out.push_back(std::move(su));
    }
    return out;
}

std::vector<std::string> CompuProDisk1aBoard::drainLog() {
    std::vector<std::string> out = std::move(log_);
    log_.clear();
    for (auto& s : out) s = id + ": " + s;
    return out;
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
    for (const Drive& d : drive_) {
        w.u32((uint32_t)d.drv.headTrackRaw());
        w.u8((uint8_t)d.drv.side());
    }
    chip_.serialize(w);
}

void CompuProDisk1aBoard::deserialize(StateReader& r) {
    Board::deserialize(r);
    dsr_        = r.u8();
    motor_      = r.u8();
    dma_        = r.u32() & 0xFFFFFFu;
    romOn_      = r.boolean();
    motorOffAt_ = r.u64();
    for (Drive& d : drive_) {
        d.drv.setHeadTrack((int)r.u32());
        d.drv.setSide(r.u8() & 1);
    }
    chip_.deserialize(r);
    applyDataRate();
    if (clock_) {
        clock_->cancel(motorWake_);
        motorWake_ = Clock::kNone;
        if (timeout_ && motor_) motorWake_ = clock_->at(motorOffAt_, [this] { motorCheck(); });
    }
    refresh();
    decodeChanged();
}

} // namespace altair
