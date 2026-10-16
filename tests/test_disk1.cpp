// CompuPro Disk 1 (src/boards/compupro-disk1.h). What it shares with the Disk 1A -- the 765,
// the DMA engine, the size probe -- is pinned in test_disk1a; this file pins what the Disk 1
// does differently, through real bus cycles: the port map (no port 0 OUT, a port 3 IN), the
// one-bit status register, the 2716's 256-byte routines, the port 3 latch, the 765's clock
// and the 8"-only size probe, the factory EPROM, the snapshot and the property checks.
//
// The command sequence is the one CompuPro's HMXFPROM.ASM sends: Specify, Recalibrate, a
// poll of status bit 7 for INT, Sense Interrupt Status, then a Read Data of all of track 0
// to 0100 with no TC, which ends at EOT.

#include "boards/compupro-disk1.h"
#include "boards/s100-memory.h"
#include "core/bus.h"
#include "core/clock.h"
#include "core/debug.h"
#include "core/machine.h"
#include "core/statefile.h"
#include "host/cardimg.h"
#include "host/media.h"
#include "host/stream.h"
#include "test.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace altair;

namespace {

constexpr uint8_t MSR = 0xC0, DATA = 0xC1, STAT = 0xC2, DMA = 0xC2, SER = 0xC3;

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

// A 2 KB EPROM whose byte at a is (routine << 4) | (a & 0x0F): the page shows which routine
// answered.
std::string writeTestRom(size_t bytes = 0x0800) {
    std::vector<char> rom(bytes);
    for (size_t a = 0; a < bytes; ++a) rom[a] = (char)(((a >> 8) << 4) | (a & 0x0F));
    const auto path = (std::filesystem::temp_directory_path() /
                       ("disk1-test-" + std::to_string(bytes) + ".bin")).string();
    std::ofstream(path, std::ios::binary).write(rom.data(), (std::streamsize)rom.size());
    return path;
}

// A guest that bit-bangs port 3 at 1200 baud on a 2 MHz 8080, with counted delay loops: it
// sends "HELLO", then echoes three bytes it receives. D7 = 1 is SPACING on both wires.
//   8000  LXI SP,9000 / LXI H,MSG / loop: MOV A,M, ORA A, JZ ECHO, MOV C,A, CALL TXCHR, INX H
//   8013  ECHO: MVI E,3 / CALL RXCHR / CALL TXCHR / DCR E / JNZ / HLT
//   8020  TXCHR: OUT C3 80h (start), then 8 x (RRC the byte in C, CMA, ANI 80h, OUT C3),
//         then OUT C3 00h (stop), each followed by BITDLY
//   803F  RXCHR: wait for IN C3 bit 7 = 1 (start), HALFDLY, then 8 x (BITDLY, IN C3, CMA,
//         RAL, shift CY into C from the top), BITDLY into the stop bit
//   805E  BITDLY: MVI B,105 / DCR B / JNZ / RET -- 1659 T-states a bit with the loop's own
//         instructions; a 1200-baud bit at 2 MHz is 1667
//   8065  HALFDLY: MVI B,53, the same loop
//   806C  MSG: "HELLO",0
const std::vector<uint8_t> kBitBang = {
    0x31, 0x00, 0x90, 0x21, 0x6C, 0x80, 0x7E, 0xB7, 0xCA, 0x13, 0x80, 0x4F, 0xCD, 0x20, 0x80,
    0x23, 0xC3, 0x06, 0x80, 0x1E, 0x03, 0xCD, 0x3F, 0x80, 0xCD, 0x20, 0x80, 0x1D, 0xC2, 0x15,
    0x80, 0x76, 0x3E, 0x80, 0xD3, 0xC3, 0xCD, 0x5E, 0x80, 0x16, 0x08, 0x79, 0x0F, 0x4F, 0x2F,
    0xE6, 0x80, 0xD3, 0xC3, 0xCD, 0x5E, 0x80, 0x15, 0xC2, 0x29, 0x80, 0xAF, 0xD3, 0xC3, 0xCD,
    0x5E, 0x80, 0xC9, 0xDB, 0xC3, 0xB7, 0xF2, 0x3F, 0x80, 0xCD, 0x65, 0x80, 0x16, 0x08, 0x0E,
    0x00, 0xCD, 0x5E, 0x80, 0xDB, 0xC3, 0x2F, 0x17, 0x79, 0x1F, 0x4F, 0x15, 0xC2, 0x4C, 0x80,
    0xCD, 0x5E, 0x80, 0xC9, 0x06, 0x69, 0x05, 0xC2, 0x60, 0x80, 0xC9, 0x06, 0x35, 0x05, 0xC2,
    0x67, 0x80, 0xC9, 0x48, 0x45, 0x4C, 0x4C, 0x4F, 0x00,
};

// A machine: 64 K of RAM that stands down for reads under PHANTOM*, the Disk 1, an 8080.
struct Rig {
    Machine             m;
    CompuProDisk1Board* fdc = nullptr;

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
        fdc = dynamic_cast<CompuProDisk1Board*>(m.add("disk1", "fd0", err));
        setProperty(*fdc, "rom", writeTestRom(), err);
        setProperty(*fdc, "boot", boot ? "on" : "off", err);
        m.add("8080", "cpu0", err);
        m.cpu()->reset(Reset::PowerOn);
        fdc->power();
    }

    uint8_t in(uint8_t p) { return m.bus.ioRead(p); }
    void    out(uint8_t p, uint8_t v) { m.bus.ioWrite(p, v); }

    bool decodes(Cycle type, uint8_t port) const {
        BusCycle c;
        c.type = type;
        c.addr = port;
        return fdc->decodes(c);
    }

    void dmaAt(uint32_t a) {
        out(DMA, (uint8_t)(a >> 16));
        out(DMA, (uint8_t)(a >> 8));
        out(DMA, (uint8_t)a);
    }

    // All but the last command byte from the test; the last from an 8080 that then polls
    // status bit 7 for INT and halts, as HMXFPROM does. The DMA bytes move between its
    // instructions, as the run loop grants the bus.
    void command(std::vector<uint8_t> bytes) {
        for (size_t i = 0; i + 1 < bytes.size(); ++i) out(DATA, bytes[i]);
        uint16_t pc = 0x8000;
        auto emit = [&](std::initializer_list<uint8_t> b) {
            for (uint8_t x : b) m.bus.memWrite(pc++, x);
        };
        emit({0x3E, bytes.back(), 0xD3, DATA});      // MVI A,last ; OUT C1
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

    // Specify, then Recalibrate drive 0 and take its Sense Interrupt Status.
    void home(uint8_t us = 0) {
        out(DATA, 0x03); out(DATA, 0xDF); out(DATA, 0x02);
        command({0x07, us});
        out(DATA, 0x08);
        results(2);
    }
};

} // namespace

void test_disk1() {
    SECTION("disk1: the port map");
    // ---- the port map: no port 0 OUT, a port 3 IN; the base moves all of it ----
    {
        Rig k;
        CHECK(k.in(MSR) == 0x80, "C0 reads the 765's MSR: RQM, idle");
        CHECK(!k.decodes(Cycle::IoWrite, 0xC0), "an OUT to C0 is not the board's: no Drive Select Register");
        CHECK(k.decodes(Cycle::IoRead, 0xC2) && k.decodes(Cycle::IoWrite, 0xC2), "C2 is both ways");
        CHECK(k.decodes(Cycle::IoRead, 0xC3), "an IN from C3 is the board's: the serial input");
        CHECK(k.decodes(Cycle::IoWrite, 0xC3), "...and an OUT to C3, the serial output");
        CHECK(!k.decodes(Cycle::IoRead, 0xC4) && !k.decodes(Cycle::IoRead, 0xBF), "four ports and no more");
        std::string err;
        CHECK(setProperty(*k.fdc, "port", "40", err), "the base moves to 40");
        CHECK(k.in(0x40) == 0x80, "40 now reads the MSR");
        CHECK(k.decodes(Cycle::IoRead, 0x43) && !k.decodes(Cycle::IoRead, 0xC3), "...and C3 moved to 43");
        k.out(0x42, 0x12); k.out(0x42, 0x34); k.out(0x42, 0x56);
        CHECK(k.fdc->dmaAddress() == 0x123456, "the DMA stack moved with the base");
        CHECK(!setProperty(*k.fdc, "port", "41", err), "a base that is not a multiple of 4 is refused");
    }

    SECTION("disk1: board status");
    // ---- board status: D7 = INT, and nothing else ----
    {
        withRampDisk(256256);
        Rig k;
        std::string err;
        CHECK(k.in(STAT) == 0x00, "no INT: the status reads 00");
        CHECK(k.fdc->mount("drive0", "a.dsk", false, err), "an 8\" SSSD disk mounts");
        CHECK(k.in(STAT) == 0x00, "a ready drive sets no status bit: there is no ready bit");
        k.fdc->reset(Reset::Bus);
        CHECK(k.in(STAT) == 0x80, "reset with drive 0 ready raises INT: D7 alone");
        CHECK(k.fdc->assertsVi() == 0 && !k.fdc->assertsInt(), "the default jumper is none: polled");
        setProperty(*k.fdc, "interrupt", "vi3", err);
        CHECK(k.fdc->assertsVi() == 0x08, "J3: INT on VI3");
        setProperty(*k.fdc, "interrupt", "int", err);
        CHECK(k.fdc->assertsInt() && k.fdc->assertsVi() == 0, "J8: INT on the S-100 INT* line");
        k.out(DATA, 0x08);
        auto r = k.results(2);
        CHECK(r[0] == 0xC0, "SIS reports the ready change on drive 0");
        CHECK(k.in(STAT) == 0x00 && !k.fdc->assertsInt(), "D7 clears once SIS has read it");
    }

    SECTION("disk1: no Force Two Sided");
    // ---- TS is the disk's: an OUT to C0 reaches nothing ----
    {
        withRampDisk(256256);
        Rig k;
        std::string err;
        k.fdc->mount("drive0", "a.dsk", false, err);
        k.out(MSR, 0x2C);  // the Disk 1A's Force Two Sided + 5.25" + alternate select
        k.out(DATA, 0x04); k.out(DATA, 0x00);
        CHECK((k.in(DATA) & 0x28) == 0x20, "ST3: ready, and TS 0 for an SS disk");
        CHECK(k.fdc->chip().fdcClockHz == 8000000, "...and the 765 still runs at 8 MHz");
    }

    SECTION("disk1: media");
    // ---- `media` is the diskette: a blank 8ds reads two-sided before any format (#755) ----
    {
        auto ts = [](Rig& k, uint8_t unit) {
            k.out(DATA, 0x04); k.out(DATA, unit);
            return (k.in(DATA) & 0x08) != 0;
        };
        auto drive = [](Rig& k, const char* unit, const char* media, std::string& err) {
            KeyValues kv{{"unit", unit}, {"mount", "x.dsk"}};
            if (media) kv.push_back({"media", media});
            return k.fdc->loadSubUnit("drive", kv, err);
        };
        withRampDisk(0);
        Rig k;
        std::string err;
        CHECK(drive(k, "0", nullptr, err) && !ts(k, 0), "a blank with no media reads one-sided");
        CHECK(drive(k, "1", "8ds", err) && ts(k, 1), "media = 8ds: a blank reads two-sided at once");
        CHECK(drive(k, "2", "8SS", err) && !ts(k, 2), "media = 8ss (any case): a blank reads one-sided");
        CHECK(!drive(k, "3", "5ds", err) && err.find("8ds") != std::string::npos,
              "a 5.25\" media is refused, and the choices are the 8\" ones");

        withRampDisk(256256);
        err.clear();
        CHECK(!drive(k, "3", "8ds", err) && err.find("not media = 8ds") != std::string::npos,
              "an 8\" SSSD image is refused as media = 8ds, not taken for a blank");
        CHECK(drive(k, "3", "8ss-sd", err) && !ts(k, 3), "...and mounts as 8ss-sd");
        withRampDisk(1256704);
        CHECK(!drive(k, "3", "8ds-sd", err), "an 8\" DSDD 8 x 1024 image is too large for 8ds-sd");
        CHECK(!ts(k, 3), "...and a refused drive table leaves the old disk and its media");

        auto subs = k.fdc->subUnits();
        CHECK(subs.size() == 4, "every drive with a disk is saved");
        bool saved = false;
        for (const auto& su : subs)
            for (const auto& f : su.fields)
                if (f.key == "media" && f.text == "8ss") saved = true;
        CHECK(saved, "media is saved as its own name, in lower case");
        k.fdc->unmount("drive1", err);
        CHECK(drive(k, "1", "8ds", err), "an 8\" DSDD image mounts as 8ds");
        k.fdc->unmount("drive1", err);
        subs = k.fdc->subUnits();
        CHECK(subs.size() == 4, "an empty drive with a media is still saved");
        withRampDisk(0);
        CHECK(k.fdc->mount("drive1", "y.dsk", false, err) && ts(k, 1),
              "the media stays with the drive: MOUNT a blank, and it reads two-sided");
    }
    {
        // `create` makes the file when it is not there, and never writes over one that is.
        setMediaResolver(openHostMedia);
        const auto dir = std::filesystem::temp_directory_path() / "disk1-create-test";
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
        const auto fresh = (dir / "fresh.dsk").string();
        const auto old   = (dir / "old.dsk").string();
        std::ofstream(old, std::ios::binary).write("x", 1);
        Rig k;
        std::string err;
        CHECK(k.fdc->loadSubUnit("drive", {{"unit", "0"}, {"mount", fresh}, {"media", "8ds"},
                                           {"create", "true"}}, err) &&
                  std::filesystem::exists(fresh),
              ("create makes an empty file and mounts it: " + err).c_str());
        CHECK(k.fdc->loadSubUnit("drive", {{"unit", "1"}, {"mount", old}, {"create", "on"}}, err) &&
                  std::filesystem::file_size(old) == 1,
              "create does not write over a file that is there");
        CHECK(!k.fdc->loadSubUnit("drive", {{"unit", "2"}, {"mount", (dir / "none.dsk").string()}}, err),
              "without create, a missing file is refused");
        k.fdc->unmount("drive0", err);
        k.fdc->unmount("drive1", err);
        std::filesystem::remove_all(dir);
    }

    SECTION("disk1: the boot ROM");
    // ---- the boot ROM: 256-byte routines of a 2716 over PHANTOM*, D0 = 0 off, reset on ----
    {
        Rig k(true);
        CHECK(k.m.bus.memRead(0x0000) == 0x40 && k.m.bus.memRead(0x00FF) == 0x4F,
              "0000-00FF read routine 4, the default");
        CHECK(k.m.bus.memRead(0x0101) == 0x00, "0100 up is RAM (zero fill; the ROM would read 11)");
        std::string err;
        setProperty(*k.fdc, "routine", "5", err);
        CHECK(k.m.bus.memRead(0x0003) == 0x53, "routine 5 is the EPROM's sixth 256-byte page");
        CHECK(k.m.bus.peek(0x00FF) == 0x5F, "DISASM and EXAMINE see the ROM (peek)");
        k.out(SER, 0x81);
        CHECK(k.fdc->romEnabled(), "OUT C3,81 (D0 = 1) leaves the ROM on");
        k.out(SER, 0x80);
        CHECK(!k.fdc->romEnabled(), "OUT C3,80 switches it off");
        CHECK(k.m.bus.memRead(0x0003) == 0x00, "...and RAM answers at 0000");
        k.out(SER, 0x01);
        CHECK(!k.fdc->romEnabled(), "D0 = 1 does not turn it back on");
        k.m.reset(Reset::Bus);
        CHECK(k.fdc->romEnabled() && k.m.bus.memRead(0x0003) == 0x53, "RESET turns it back on");
        setProperty(*k.fdc, "boot", "off", err);
        CHECK(k.m.bus.memRead(0x0003) == 0x00, "boot = off: no ROM, RAM answers");
    }

    SECTION("disk1: the port 3 latch");
    // ---- port 3 D7: the output latch, MARKING at reset; the input idles at MARKING ----
    {
        Rig k;
        CHECK(!k.fdc->latch(), "power on: the latch is MARKING (D7 = 0)");
        k.out(SER, 0x81);
        CHECK(k.fdc->latch(), "OUT C3 with D7 = 1 latches SPACING");
        k.out(SER, 0x01);
        CHECK(!k.fdc->latch(), "...and D7 = 0 latches MARKING");
        k.out(SER, 0x81);
        k.m.reset(Reset::Bus);
        CHECK(!k.fdc->latch(), "RESET clears the latch to MARKING");
        CHECK(k.in(SER) == 0x00, "IN C3: the idle input line is MARKING, D7 = 0");
        std::string err;
        CHECK(setProperty(*k.fdc, "port3", "motor", err), "S1-3 makes the latch MOTOR ON");
        k.out(SER, 0x01);
        CHECK(!k.fdc->latch(), "...and D7 = 0 is MOTOR ON (1982 manual: ON by a 0)");
        CHECK(!setProperty(*k.fdc, "port3", "parallel", err), "port3 is serial or motor");
    }

    SECTION("disk1: the serial line, from a guest");
    // ---- the 8080 bit-bangs HELLO out and echoes three bytes back, through port 3 ----
    {
        // `cut`: first power the machine off in the middle of a frame started late in a run,
        // so the frame would still be in the future after the clock restarts.
        auto run = [](const std::string& port3, const std::string& typed, bool cut = false) {
            Rig         k;
            std::string err;
            setProperty(*k.fdc, "port3", port3, err);
            setUnitProperty(*k.fdc, "serial", "baud", "1200", err);
            auto            st = std::make_unique<ScriptedStream>();
            ScriptedStream* s  = st.get();
            k.fdc->connectStream("serial", std::move(st), err);
            if (cut) {
                k.m.clock.advance(5000000);
                k.out(SER, 0x80);
                k.m.power();
            }
            s->feed(typed);
            for (size_t i = 0; i < kBitBang.size(); ++i)
                k.m.bus.memWrite((uint16_t)(0x8000 + i), kBitBang[i]);
            k.m.cpu()->setPc(0x8000);
            k.m.debug.run(5000000);
            return std::make_pair(s->out(), k.fdc->drainLog());
        };
        auto r = run("serial", "Hi\x80");
        CHECK(r.first == std::string("HELLOHi\x80"), "the guest's HELLO and its echo come out");
        CHECK(r.second.empty(), "no framing errors");
        auto c = run("serial", "Hi\x80", true);
        CHECK(c.first == std::string("HELLOHi\x80") && c.second.empty(),
              "power on in the middle of a frame: the guest's next frames are clean");
        auto m = run("motor", "Hi\x80");
        CHECK(m.first.empty(), "port3 = motor: the latch drives the motor, and nothing is sent");

        Rig         k;
        std::string err;
        UnitDef     u;
        CHECK(k.fdc->findUnit("serial", u) && u.kind == UnitKind::Serial && u.state == "null",
              "the serial unit is listed, and nothing is connected");
        CHECK(k.fdc->connect("serial", "null", err), "CONNECT fd0:serial goes through the resolver");
        CHECK(!k.fdc->connect("drive0", "null", err), "a drive is not a serial unit");
        CHECK(!setUnitProperty(*k.fdc, "serial", "baud", "10", err), "baud is checked");
        k.out(SER, 0x80);  // a start bit: the frame's deadline is queued
        CHECK(k.m.clock.queued() == 1, "a start bit arms the line's deadline");
    }

    SECTION("disk1: a track 0 read to 0100");
    // ---- HMXFPROM's read: all 26 sectors of track 0 by DMA to 0100, ending at EOT ----
    {
        withRampDisk(256256);
        Rig k(true);
        std::string err;
        k.fdc->mount("drive0", "a.dsk", false, err);
        k.out(SER, 0x80);  // the ROM off, so 0100 is RAM either way
        k.home();
        k.dmaAt(0x000100);
        k.command({0x06, 0x00, 0, 0, 1, 0, 26, 7, 128});
        auto r = k.results(7);
        CHECK(r[0] == 0x40 && r[1] == 0x80 && r[2] == 0, "ends at EOT: ST0 40, ST1 EN, ST2 0");
        bool ok = true;
        for (int i = 0; i < 26 * 128; ++i)
            if (k.m.bus.memRead((uint16_t)(0x0100 + i)) != stamp((uint64_t)i)) ok = false;
        CHECK(ok, "track 0 landed at 0100-0DFF by DMA");
        CHECK(k.fdc->dmaAddress() == 0x000E00, "the counter stands one past the last byte");
        CHECK(!k.fdc->requestsBus(), "pHOLD is down once the command ends");
    }

    SECTION("disk1: builtin:DISK1, the factory EPROM");
    // ---- the built-in EPROM, a factory dump: routines 0-3 are 8086 code and 4-7 8080 code.
    // 4-7 differ in one byte, the console code each passes in C. Each 8080 routine reads
    // sectors 1-4 of track 0 to 0100 and falls into it with the ROM off and D7 = 0 ----
    {
        auto boot = [](Rig& k) {
            k.m.cpu()->reset(Reset::PowerOn);
            k.fdc->power();
            return k.m.debug.run(20000000);
        };
        auto regC = [](Rig& k) {
            uint8_t c = 0xFF;
            for (const RegDef& x : k.m.cpu()->registers())
                if (x.name == "C") c = (uint8_t)x.get();
            return c;
        };
        auto filled = [](Rig& k, uint16_t from, uint16_t to, uint8_t v) {
            for (uint32_t a = from; a < to; ++a)
                if (k.m.bus.memRead((uint16_t)a) != v) return false;
            return k.m.bus.memRead(to) == 0;  // and not a byte further: zero-filled RAM
        };
        {
            Machine     m;
            std::string err;
            auto* fdc = m.add("disk1", "fd0", err);
            fdc->power();
            CHECK(fdc->drainLog().empty(), "the default ROM, builtin:DISK1, loads");
            long long routine = -1;
            for (const Property& x : fdc->properties())
                if (x.name == "routine") routine = x.get().i();
            CHECK(routine == 4, "the default routine is 4, the first 8080 one");
            const uint8_t head[] = {0, 0, 0, 0, 0, 0, 0, 0xC3, 0x0A, 0x00};
            bool ok = true;
            for (uint16_t a = 0; a < sizeof head; ++a) ok = ok && m.bus.peek(a) == head[a];
            CHECK(ok, "routine 4: NOP x7, JMP 000A");
            uint8_t page4[256];
            for (uint16_t a = 0; a < 256; ++a) page4[a] = m.bus.peek(a);
            CHECK(page4[0xFC] == 0x0E && page4[0xFE] == 0xD3 && page4[0xFF] == 0xC3,
                  "...ends MVI C,n / OUT C3");
            for (int r = 5; r <= 7; ++r) {
                setProperty(*fdc, "routine", std::to_string(r), err);
                ok = true;
                for (uint16_t a = 0; a < 256; ++a)
                    ok = ok && m.bus.peek(a) == (a == 0xFD ? (uint8_t)(r - 4) : page4[a]);
                CHECK(ok, ("routine " + std::to_string(r) + " is routine 4 with C = " +
                           std::to_string(r - 4)).c_str());
            }
            for (int r : {0, 2, 3}) {
                setProperty(*fdc, "routine", std::to_string(r), err);
                CHECK(m.bus.peek(0x0000) == 0x90 && m.bus.peek(0x0004) == 0xDB,
                      ("routine " + std::to_string(r) + " is 8086 code: NOP, IN AL").c_str());
            }
        }
        // 8" SSDD 8 x 1024, CP/M 2.2N's format. Track 0 (26 x 128 FM) is all HLT.
        setMediaResolver([](const std::string& path, bool ro, std::string&) {
            std::vector<uint8_t> d(625920);
            for (size_t i = 0; i < d.size(); ++i) d[i] = i < 26 * 128 ? 0x76 : stamp(i);
            return std::make_unique<MemoryMedia>(path, std::move(d), ro);
        });
        for (int r = 4; r <= 7; ++r)
            for (const bool motor : {false, true}) {
                const std::string tag =
                    " (routine " + std::to_string(r) + (motor ? ", port3 = motor)" : ")");
                Rig         k(true);
                std::string err;
                setProperty(*k.fdc, "rom", "builtin:DISK1", err);
                setProperty(*k.fdc, "routine", std::to_string(r), err);
                if (motor) setProperty(*k.fdc, "port3", "motor", err);
                k.fdc->mount("drive0", "n.dsk", false, err);
                const RunResult rr = boot(k);
                CHECK(rr.why == StopReason::Halted && rr.pc == 0x0101,
                      ("the routine jumps to 0100, where track 0's first byte is" + tag).c_str());
                CHECK(filled(k, 0x0100, 0x0300, 0x76),
                      ("...after it reads sectors 1-4 to 0100-02FF" + tag).c_str());
                CHECK(regC(k) == r - 4 && !k.fdc->romEnabled(),
                      ("...with C = routine - 4 and the ROM off (OUT C3,00)" + tag).c_str());
                CHECK(!k.fdc->latch() && k.fdc->line().tx() && k.fdc->drainLog().empty(),
                      ("D7 is left at 0: the line rests at MARKING" + tag).c_str());
            }
    }

    SECTION("disk1: the drive size");
    // ---- the board runs 8" drives, with the 765 at 8 MHz. A 5.25" image is refused ----
    {
        Rig k;
        std::string err;
        CHECK(k.fdc->chip().fdcClockHz == 8000000, "the 765 runs at 8 MHz");
        withRampDisk(256256);
        CHECK(k.fdc->mount("drive0", "a.dsk", false, err), "an 8\" SSSD disk mounts");
        withRampDisk(409600);
        CHECK(!k.fdc->mount("drive1", "b.dsk", false, err) && err.find("8\"") != std::string::npos,
              "a 5.25\" SSDD 5 x 1024 disk is refused: the board is built for 8\" drives");
        withRampDisk(0);
        CHECK(k.fdc->mount("drive2", "blank.dsk", false, err), "a blank mounts");
        k.fdc->reset(Reset::Bus);
        CHECK(k.fdc->chip().fdcClockHz == 8000000, "...and a reset keeps the 8 MHz clock");
        CHECK(!setProperty(*k.fdc, "size", "5.25", err), "there is no size property");
    }

    SECTION("disk1: the snapshot");
    // ---- the snapshot ----
    {
        withRampDisk(256256);
        Rig a(true);
        std::string err;
        a.fdc->mount("drive0", "a.dsk", false, err);
        a.home();
        a.command({0x0F, 0x00, 5});
        a.dmaAt(0xABCDEF);
        a.out(SER, 0x80);
        StateWriter w;
        a.fdc->serialize(w);

        Rig b(true);
        b.fdc->mount("drive0", "a.dsk", false, err);
        StateReader r(w.data());
        b.fdc->deserialize(r);
        CHECK(r.ok() && r.remaining() == 0, "the snapshot reads back to the last byte");
        CHECK(b.fdc->dmaAddress() == 0xABCDEF, "the DMA counter travels");
        CHECK(!b.fdc->romEnabled(), "the ROM flip-flop travels");
        CHECK(b.fdc->latch(), "the port 3 latch travels");
        CHECK(b.fdc->chip().pcn(0) == 5, "the 765 travels");
        CHECK(b.in(STAT) == 0x80, "...with its seek-end INT still up");
    }

    SECTION("disk1: property checks");
    // ---- property checks ----
    {
        Rig k;
        std::string err;
        CHECK(setProperty(*k.fdc, "routine", "7", err), "routine 7 is the top of a 2716");
        CHECK(!setProperty(*k.fdc, "routine", "8", err), "routine is 0-7");
        CHECK(!setProperty(*k.fdc, "rom", writeTestRom(0x1000), err) &&
                  err.find("2716") != std::string::npos,
              "a ROM larger than a 2716 is refused");
        CHECK(!setProperty(*k.fdc, "rom", "/no/such/disk1.rom", err), "a missing ROM file is refused");
    }
}
