// CompuPro Disk 1A (src/boards/compupro-disk1a.h). The µPD765A itself is pinned in
// test_upd765; this file pins the board around it, through real bus cycles: the port map,
// the board status bits, the Drive Select Register, the DMA engine and its 24-bit counter,
// the boot ROM over PHANTOM*, the motor timeout, the size probe, the snapshot and the
// property checks.
//
// The command sequences are the ones CompuPro's own ROM1.ASM and HMXBIOS.ASM send: Specify
// DF, Recalibrate, a poll of status bit 7 for INT, Sense Interrupt Status, and a Read Data
// with no TC that ends at EOT.

#include "boards/compupro-disk1a.h"
#include "boards/s100-memory.h"
#include "core/bus.h"
#include "core/clock.h"
#include "core/debug.h"
#include "core/machine.h"
#include "core/statefile.h"
#include "host/media.h"
#include "test.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace altair;

namespace {

constexpr uint8_t MSR = 0xC0, DSR = 0xC0, DATA = 0xC1, STAT = 0xC2, DMA = 0xC2, MOTOR = 0xC3;

// Every byte of a disk is stamped with its own file offset, (i + i/128) & 0xFF, so a sector
// read from the wrong place in the file never matches by accident.
uint8_t stamp(uint64_t i) { return (uint8_t)((i + i / 128) & 0xFF); }

void withRampDisk(uint64_t bytes) {
    setMediaResolver([bytes](const std::string& path, bool ro, std::string&) {
        std::vector<uint8_t> d((size_t)bytes);
        for (size_t i = 0; i < d.size(); ++i) d[i] = stamp(i);
        return std::make_unique<MemoryMedia>(path, std::move(d), ro);
    });
}

// An 8 KB EPROM whose byte at a is (routine << 4) | (a & 0x0F): the page shows which
// routine answered.
std::string writeTestRom(size_t bytes = 0x2000) {
    std::vector<char> rom(bytes);
    for (size_t a = 0; a < bytes; ++a) rom[a] = (char)(((a >> 9) << 4) | (a & 0x0F));
    const auto path = (std::filesystem::temp_directory_path() /
                       ("disk1a-test-" + std::to_string(bytes) + ".bin")).string();
    std::ofstream(path, std::ios::binary).write(rom.data(), (std::streamsize)rom.size());
    return path;
}

// A machine: 64 K of RAM that stands down for reads under PHANTOM*, the Disk 1A, an 8080.
struct Rig {
    Machine              m;
    CompuProDisk1aBoard* fdc = nullptr;

    explicit Rig(bool boot = false) {
        std::string err;
        auto* mem = dynamic_cast<MemoryBoard*>(m.add("memory", "mem0", err));
        Region r;
        r.kind = RegionKind::Ram;
        r.at   = 0x0000;
        r.size = 0x10000;
        mem->addRegion(r, err);
        setProperty(*mem, "fill", "zero", err);
        setProperty(*mem, "honors_phantom", "read", err);
        mem->power();
        fdc = dynamic_cast<CompuProDisk1aBoard*>(m.add("disk1a", "fd0", err));
        setProperty(*fdc, "rom", writeTestRom(), err);
        setProperty(*fdc, "boot", boot ? "on" : "off", err);
        m.add("8080", "cpu0", err);
        m.cpu()->reset(Reset::PowerOn);
        fdc->power();
    }

    uint8_t in(uint8_t p) { return m.bus.ioRead(p); }
    void    out(uint8_t p, uint8_t v) { m.bus.ioWrite(p, v); }

    void dmaAt(uint32_t a) {
        out(DMA, (uint8_t)(a >> 16));
        out(DMA, (uint8_t)(a >> 8));
        out(DMA, (uint8_t)a);
    }

    // All but the last command byte from the test; the last from an 8080 that then polls
    // status bit 7 for INT and halts -- ROM1.ASM's RCAL2 and READ2 loops. The DMA bytes move
    // between its instructions, as the run loop grants the bus.
    void command(std::vector<uint8_t> bytes) {
        for (size_t i = 0; i + 1 < bytes.size(); ++i) out(DATA, bytes[i]);
        uint16_t pc = 0x8000;
        auto emit = [&](std::initializer_list<uint8_t> b) {
            for (uint8_t x : b) m.bus.memWrite(pc++, x);
        };
        emit({0x3E, bytes.back(), 0xD3, DATA});  // MVI A,last ; OUT C1
        emit({0xDB, STAT, 0xB7, 0xF2, 0x04, 0x80});  // IN C2 ; ORA A ; JP 8004
        emit({0x76});                                // HLT
        m.cpu()->reset(Reset::Bus);  // out of the last command's HLT
        m.cpu()->setPc(0x8000);
        m.debug.run(0);
    }

    std::vector<uint8_t> results(int n) {
        std::vector<uint8_t> v;
        for (int i = 0; i < n; ++i) v.push_back(in(DATA));
        return v;
    }

    // Specify (SRT 3 ms, HUT 240 ms, HLT 2 ms, DMA), then Recalibrate drive 0 and take its
    // Sense Interrupt Status. Leaves the head at track 0 and INT clear.
    void home(uint8_t us = 0) {
        out(DATA, 0x03); out(DATA, 0xDF); out(DATA, 0x02);
        command({0x07, us});
        out(DATA, 0x08);
        results(2);
    }
};

} // namespace

void test_disk1a() {
    SECTION("disk1a: the port map");
    // ---- the port map, and the base moves all of it ----
    {
        Rig k;
        CHECK(k.in(MSR) == 0x80, "C0 reads the 765's MSR: RQM, idle");
        BusCycle c;
        c.type = Cycle::IoRead;
        c.addr = 0xC3;
        CHECK(!k.fdc->decodes(c), "C3 is write-only: an IN there is not the board's");
        c.type = Cycle::IoWrite;
        CHECK(k.fdc->decodes(c), "...and an OUT there is");
        std::string err;
        CHECK(setProperty(*k.fdc, "port", "40", err), "the base moves to 40");
        CHECK(k.in(0x40) == 0x80, "40 now reads the MSR");
        c.addr = 0xC0;
        c.type = Cycle::IoRead;
        CHECK(!k.fdc->decodes(c), "...and C0 is no longer the board's");
        k.out(0x42, 0x12); k.out(0x42, 0x34); k.out(0x42, 0x56);
        CHECK(k.fdc->dmaAddress() == 0x123456, "the DMA stack moved with the base");
        CHECK(!setProperty(*k.fdc, "port", "41", err), "a base that is not a multiple of 4 is refused");
        CHECK(setProperty(*k.fdc, "port", "FC", err), "FC is the top base");
    }

    SECTION("disk1a: the DMA address stack");
    // ---- the DMA address register: a 3-deep push-down stack, MSB first ----
    {
        Rig k;
        k.out(DMA, 0x12); k.out(DMA, 0x34); k.out(DMA, 0x56);
        CHECK(k.fdc->dmaAddress() == 0x123456, "three writes load A23-A16, A15-A8, A7-A0");
        k.out(DMA, 0x78);
        CHECK(k.fdc->dmaAddress() == 0x345678, "a fourth pushes the oldest byte out");
    }

    SECTION("disk1a: board status");
    // ---- board status: ready, sense switch, INT ----
    {
        withRampDisk(256256);
        Rig k;
        CHECK((k.in(STAT) & 0x01) == 0, "D0: no disk in drive 0, not ready");
        CHECK((k.in(STAT) & 0x04) == 0, "D2: S3-1 ON (the default) reads 0");
        std::string err;
        setProperty(*k.fdc, "sense_switch", "off", err);
        CHECK((k.in(STAT) & 0x04) == 0x04, "D2: S3-1 OFF reads 1");
        CHECK((k.in(STAT) & 0x7B) == 0, "bits 3-6 read 0, as do index and ready with no disk");

        CHECK(k.fdc->mount("drive0", "a.dsk", false, err), "an 8\" SSSD disk mounts");
        CHECK((k.in(STAT) & 0x01) == 0x01, "D0: drive 0 is ready");
        k.fdc->reset(Reset::Bus);
        CHECK((k.in(STAT) & 0x80) == 0x80, "D7: reset with drive 0 ready raises INT (the 765's ready change)");
        CHECK(k.fdc->assertsVi() == 0x10, "...on VI4, CompuPro's strap");
        setProperty(*k.fdc, "interrupt", "none", err);
        CHECK(k.fdc->assertsVi() == 0, "with the jumper out, no VI line");
        CHECK((k.in(STAT) & 0x80) == 0x80, "...but status bit 7 still shows INT");
        k.out(DATA, 0x08);
        auto r = k.results(2);
        CHECK(r[0] == 0xC0, "SIS reports the ready change on drive 0");
        CHECK((k.in(STAT) & 0x80) == 0, "D7 clears once SIS has read it");
    }

    SECTION("disk1a: the Drive Select Register");
    // ---- the Drive Select Register: alternate select, Force Two Sided, data rate ----
    {
        withRampDisk(256256);
        Rig k;
        std::string err;
        k.fdc->mount("drive2", "c.dsk", false, err);
        CHECK((k.in(STAT) & 0x01) == 0, "the 765 selects unit 0, which is empty");
        k.out(DSR, 0x04 | 0x02);
        CHECK((k.in(STAT) & 0x01) == 0x01, "alternate select puts unit 2 on the cable: ready");
        k.out(DATA, 0x04); k.out(DATA, 0x00);  // Sense Drive Status, US = 0
        CHECK((k.in(DATA) & 0x20) == 0x20, "...and the 765's unit 0 reaches drive 2 (ST3 RY)");
        k.out(DSR, 0x00);
        k.out(DATA, 0x04); k.out(DATA, 0x02);
        CHECK((k.in(DATA) & 0x08) == 0, "an SS disk: ST3 TS is 0");
        k.out(DSR, 0x08);
        k.out(DATA, 0x04); k.out(DATA, 0x02);
        CHECK((k.in(DATA) & 0x08) == 0x08, "Force Two Sided holds TS high");
        CHECK(k.fdc->chip().fdcClockHz == 8000000, "8\" rate: the 765 runs at 8 MHz");
        k.out(DSR, 0x20);
        CHECK(k.fdc->chip().fdcClockHz == 4000000, "bit 5 halves the 765's clock for 5.25\"");
    }

    SECTION("disk1a: a DMA read");
    // ---- a DMA read, as the CompuPro ROM does it: EOT, no TC, ST0 40 + ST1 EN ----
    {
        withRampDisk(256256);
        Rig k;
        std::string err;
        k.fdc->mount("drive0", "a.dsk", false, err);
        k.home();
        k.dmaAt(0x002000);
        k.command({0x06, 0x00, 0, 0, 1, 0, 2, 7, 128});  // Read Data, SD, sectors 1-2
        auto r = k.results(7);
        CHECK(r[0] == 0x40 && r[1] == 0x80 && r[2] == 0, "ends at EOT: ST0 40, ST1 EN, ST2 0");
        bool ok = true;
        for (int i = 0; i < 256; ++i)
            if (k.m.bus.memRead((uint16_t)(0x2000 + i)) != stamp((uint64_t)i)) ok = false;
        CHECK(ok, "sectors 1 and 2 landed at 2000 by DMA");
        CHECK(k.m.bus.memRead(0x2100) == 0, "...and nothing past them");
        CHECK(k.fdc->dmaAddress() == 0x002100, "the counter stands one past the last byte");
        CHECK(!k.fdc->requestsBus(), "pHOLD is down once the command ends");
    }

    SECTION("disk1a: the 24-bit counter");
    // ---- the counter is 24 bits: it carries into A16, and the bus sees A0-A15 ----
    {
        withRampDisk(256256);
        Rig k;
        std::string err;
        k.fdc->mount("drive0", "a.dsk", false, err);
        k.home();
        k.dmaAt(0x00FFC0);
        k.command({0x06, 0x00, 0, 0, 1, 0, 1, 7, 128});
        k.results(7);
        bool ok = true;
        for (int i = 0; i < 128; ++i)
            if (k.m.bus.memRead((uint16_t)(0xFFC0 + i)) != stamp((uint64_t)i)) ok = false;
        CHECK(ok, "the sector wraps from FFFF to 0000 on this 64 K bus");
        CHECK(k.fdc->dmaAddress() == 0x010040, "...while the counter carries into A16");
    }

    SECTION("disk1a: a DMA write");
    // ---- a DMA write, read back ----
    {
        withRampDisk(256256);
        Rig k;
        std::string err;
        k.fdc->mount("drive0", "a.dsk", false, err);
        k.home();
        for (int i = 0; i < 128; ++i) k.m.bus.memWrite((uint16_t)(0x3000 + i), (uint8_t)(0xA0 ^ i));
        k.dmaAt(0x003000);
        k.command({0x05, 0x00, 0, 0, 3, 0, 3, 7, 128});  // Write Data, sector 3
        auto r = k.results(7);
        CHECK(r[0] == 0x40 && r[1] == 0x80, "the write ends at EOT");
        k.dmaAt(0x004000);
        k.command({0x06, 0x00, 0, 0, 3, 0, 3, 7, 128});
        k.results(7);
        bool ok = true;
        for (int i = 0; i < 128; ++i)
            if (k.m.bus.memRead((uint16_t)(0x4000 + i)) != (uint8_t)(0xA0 ^ i)) ok = false;
        CHECK(ok, "the sector reads back as written");
    }

    SECTION("disk1a: the size probe");
    // ---- the size probe: 8" DS is cylinder-interleaved ----
    {
        // 8" DSDD 8 x 1024: SD track 0 head 0, then 153 tracks of 8 KB (CCPM22Q.DSK's size).
        withRampDisk(1256704);
        Rig k;
        std::string err;
        CHECK(k.fdc->mount("drive0", "q.dsk", false, err), "1,256,704 bytes is 8\" DSDD 8 x 1024");
        k.home();
        k.out(DATA, 0x04); k.out(DATA, 0x00);
        CHECK((k.in(DATA) & 0x08) == 0x08, "a DS image raises TS");
        k.dmaAt(0x2000);
        k.command({0x46, 0x04, 0, 1, 1, 3, 1, 0x1B, 0xFF});  // cylinder 0 head 1
        auto r = k.results(7);
        CHECK(r[1] == 0x80, "cylinder 0 head 1 reads at MFM");
        CHECK(k.m.bus.memRead(0x2000) == stamp(3328) && k.m.bus.memRead(0x23FF) == stamp(3328 + 1023),
              "cylinder 0 head 1 is the file's second track (3328): interleaved");
        k.command({0x0F, 0x00, 2});  // Seek to cylinder 2
        k.out(DATA, 0x08);
        k.results(2);
        k.dmaAt(0x2000);
        k.command({0x46, 0x00, 2, 0, 1, 3, 1, 0x1B, 0xFF});
        k.results(7);
        CHECK(k.m.bus.memRead(0x2000) == stamp(27904),
              "cylinder 2 head 0 is at 27,904 -- where CCPM22Q.DSK keeps its directory");
    }
    {
        Rig k;
        std::string err;
        const struct { uint64_t bytes; bool ok; const char* what; } sizes[] = {
            {256256, true, "8\" SSSD 26 x 128"},     {512512, true, "8\" DSSD"},
            {625920, true, "8\" SSDD 8 x 1024"},     {587008, true, "8\" SSDD 15 x 512"},
            {509184, true, "8\" SSDD 26 x 256"},     {163840, true, "5.25\" SSSD 16 x 128"},
            {409600, true, "5.25\" SSDD 5 x 1024"},  {0, true, "a blank (0-byte) disk"},
            {327680, false, "327,680 (three 5.25\" formats)"},
            {655360, false, "655,360 (two 5.25\" formats)"},
            {2000000, false, "an image larger than any disk"},
        };
        for (const auto& s : sizes) {
            withRampDisk(s.bytes);
            err.clear();
            CHECK(k.fdc->mount("drive1", "x.dsk", false, err) == s.ok,
                  (std::string(s.ok ? "mounts: " : "refused: ") + s.what).c_str());
        }
        CHECK(!k.fdc->mount("drive4", "x.dsk", false, err), "there is no drive4");
    }

    SECTION("disk1a: the boot ROM");
    // ---- the boot ROM over PHANTOM*: reads only, the routine, D0 = 0, reset ----
    {
        Rig k(true);
        CHECK(k.fdc->romEnabled(), "the ROM is on after power-up");
        CHECK(k.m.bus.memRead(0x0000) == 0x00 && k.m.bus.memRead(0x01FF) == 0x0F,
              "0000-01FF read routine 0");
        CHECK(k.m.bus.memRead(0x0201) == 0x00, "0200 up is RAM (zero fill; the ROM would read 01)");
        k.m.bus.memWrite(0x005D, 0xAA);
        CHECK(k.m.bus.memRead(0x005D) == 0x0D, "a write under the ROM does not show through");
        std::string err;
        setProperty(*k.fdc, "routine", "3", err);
        CHECK(k.m.bus.memRead(0x0001) == 0x31, "routine 3 is the EPROM's fourth 512-byte page");
        k.out(MOTOR, 0xFF);
        CHECK(k.fdc->romEnabled(), "OUT C3,FF (ROM1.ASM's INIT) leaves the ROM on");
        k.out(MOTOR, 0xFE);
        CHECK(!k.fdc->romEnabled(), "OUT C3,FE switches it off");
        CHECK(k.m.bus.memRead(0x005D) == 0xAA, "...and the write made under it is in the RAM");
        k.out(MOTOR, 0x01);
        CHECK(!k.fdc->romEnabled(), "D0 = 1 does not turn it back on");
        k.m.reset(Reset::Bus);
        CHECK(k.fdc->romEnabled() && k.m.bus.memRead(0x0001) == 0x31, "RESET turns it back on");
        setProperty(*k.fdc, "boot", "off", err);
        CHECK(k.m.bus.memRead(0x0001) == 0x00, "boot = off: no ROM, RAM answers");
    }

    SECTION("disk1a: the motors");
    // ---- the motors: bits 4-7, the index pulse, and the ~15 s timeout ----
    {
        withRampDisk(256256);
        Rig k;
        std::string err;
        k.fdc->mount("drive0", "a.dsk", false, err);
        auto sawIndex = [&] {
            const uint64_t rev = k.m.clock.tStatesPer(6);  // one 360 RPM revolution
            bool seen = false;
            for (uint64_t t = 0; t < rev; t += 2000) {
                k.m.clock.advance(2000);
                if (k.in(STAT) & 0x02) seen = true;
            }
            return seen;
        };
        CHECK(!sawIndex(), "motor off: no index pulse");
        k.out(MOTOR, 0x11);
        CHECK(k.fdc->motors() == 0x10, "OUT C3 sets the drive 0 motor bit");
        CHECK(sawIndex(), "motor on: the index pulse shows in D1");
        const uint64_t sec = k.m.clock.tStatesPer(1);
        k.m.clock.advance(14 * sec);
        CHECK(k.fdc->motors() == 0x10, "14 s on, the motor still runs");
        k.in(MSR);  // any access restarts the timer
        k.m.clock.advance(14 * sec);
        CHECK(k.fdc->motors() == 0x10, "an access restarted the 15 s");
        k.m.clock.advance(2 * sec);
        CHECK(k.fdc->motors() == 0, "15 s after the last access, the motors stop");
        CHECK(!k.m.clock.queued(), "...and the board leaves no deadline behind");
        setProperty(*k.fdc, "motor_timeout", "off", err);
        CHECK(sawIndex(), "motor_timeout = off: the motors always run");
    }

    SECTION("disk1a: the snapshot");
    // ---- the snapshot ----
    {
        withRampDisk(256256);
        Rig a(true);
        std::string err;
        a.fdc->mount("drive0", "a.dsk", false, err);
        a.home();
        a.command({0x0F, 0x00, 5});
        a.out(DSR, 0x20);
        a.dmaAt(0xABCDEF);
        a.out(MOTOR, 0x30);
        StateWriter w;
        a.fdc->serialize(w);

        Rig b(true);
        b.fdc->mount("drive0", "a.dsk", false, err);
        StateReader r(w.data());
        b.fdc->deserialize(r);
        CHECK(r.ok() && r.remaining() == 0, "the snapshot reads back to the last byte");
        CHECK(b.fdc->dmaAddress() == 0xABCDEF, "the DMA counter travels");
        CHECK(!b.fdc->romEnabled(), "the ROM flip-flop travels");
        CHECK(b.fdc->motors() == 0x30, "the motor register travels");
        CHECK(b.fdc->chip().fdcClockHz == 4000000, "the Drive Select Register travels");
        CHECK(b.fdc->chip().pcn(0) == 5, "the 765 travels");
        CHECK((b.in(STAT) & 0x80) == 0x80, "...with its seek-end INT still up");
    }

    SECTION("disk1a: property checks");
    // ---- property checks ----
    {
        Rig k;
        std::string err;
        CHECK(!setProperty(*k.fdc, "routine", "16", err), "routine is 0-15");
        CHECK(!setProperty(*k.fdc, "rom", "/no/such/disk1a.rom", err), "a missing ROM file is refused");
        std::string rom;
        for (auto& p : k.fdc->properties())
            if (p.name == "rom") rom = p.get().s();
        CHECK(rom == writeTestRom(), "...and the old ROM stays");
        CHECK(!setProperty(*k.fdc, "rom", writeTestRom(0x4000), err) &&
                  err.find("2764") != std::string::npos,
              "a ROM larger than a 2764 is refused");
        CHECK(!setProperty(*k.fdc, "rom", "builtin:no-such-rom", err), "an unknown built-in is refused");
    }
}
