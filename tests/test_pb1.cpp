#include "test.h"

#include "boards/io4.h"
#include "boards/mits-88cpu.h"
#include "boards/s100-memory.h"
#include "boards/ssm-pb1.h"
#include "core/debug.h"
#include "core/hex.h"
#include "core/machine.h"
#include "core/statefile.h"
#include "host/stream.h"

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

using namespace altair;

namespace {

// A machine with a PB1 in it: 52K of RAM (0000-CFFF), the programming window at D000, and the
// control port at 10 -- the PB1 half of tests/media/pb1/pb1.toml. The RAM stops below the window
// so nothing contends with the board. No monitor and no console: MonRig below adds those.
struct Rig {
    Machine   m;
    Pb1Board* pb = nullptr;

    Rig() {
        std::string err;
        m.bus.setVerify(true);

        auto* mem = dynamic_cast<MemoryBoard*>(m.add("memory", "mem0", err));
        Region r;
        r.kind = RegionKind::Ram;
        r.at   = 0;
        r.size = 0xD000;  // 0000-CFFF -- below the window
        mem->addRegion(r, err);
        setProperty(*mem, "fill", "zero", err);

        pb = dynamic_cast<Pb1Board*>(m.add("pb1", "pb1", err));
        setProperty(*pb, "port", "10", err);
        setProperty(*pb, "window", "D000", err);

        m.add("8080", "cpu0", err);
        m.power();
    }

    void    out(uint8_t p, uint8_t v) { m.bus.ioWrite(p, v); }   // OUT p,v
    void    wr(uint16_t a, uint8_t v) { m.bus.memWrite(a, v); }  // a bus write (may burn)
    uint8_t rd(uint16_t a) { return m.bus.memRead(a); }          // a bus read (disarms in-window)
    uint8_t look(uint16_t a) { return m.bus.peek(a); }           // a peek -- no side effects
};

// THE WHOLE MACHINE of tests/media/pb1/pb1.toml: the PB1, plus the SSM 8080 System Monitor as a
// ROM at F000 and an SSM IO-4 whose Serial A is the console. This is what the PB1 manual's
// software was written for -- every program exits with JMP F021 (the monitor's re-entry) and
// the two check programs print through CALL F009 (its console-out). The console is a
// ScriptedStream, so the test types at the monitor's prompt and reads what it prints.
struct MonRig {
    Machine         m;
    Pb1Board*       pb  = nullptr;
    Io4Board*       io  = nullptr;
    ScriptedStream* tty = nullptr;

    MonRig() {
        std::string err;

        auto*  mem = dynamic_cast<MemoryBoard*>(m.add("memory", "mem0", err));
        Region ram;
        ram.kind = RegionKind::Ram;
        ram.at   = 0;
        ram.size = 0xD000;  // 0000-CFFF -- below the PB1 window
        mem->addRegion(ram, err);
        Region rom;
        rom.kind  = RegionKind::Rom;
        rom.at    = 0xF000;
        rom.mount = "builtin:ssm-8080mon";
        CHECK(mem->addRegion(rom, err), "the SSM 8080 monitor ROM mounts at F000");
        setProperty(*mem, "fill", "zero", err);

        pb = dynamic_cast<Pb1Board*>(m.add("pb1", "pb1", err));
        setProperty(*pb, "port", "10", err);
        setProperty(*pb, "window", "D000", err);

        io     = dynamic_cast<Io4Board*>(m.add("io4", "io40", err));
        auto s = std::make_unique<ScriptedStream>();
        tty    = s.get();
        CHECK(io->connectStream("a", std::move(s), err), "the IO-4's Serial A gets the console");

        m.add("8080", "cpu0", err);
        m.power();
        m.cpu()->setPc(0xF000);  // cold-start the monitor at its ORG
    }

    // Run until `text` has reached the console. The console is baud-paced, so the clock must
    // advance with the CPU. The step limit is far above the longest run here (the 2708 burn).
    bool until(const std::string& text) {
        for (int i = 0; i < 40000000; ++i) {
            if (tty->out().find(text) != std::string::npos) return true;
            StepResult st = m.master()->step(m.bus);
            m.clock.advance(st.tStates);
            io->pump();
        }
        return false;
    }

    // The monitor's banner and its '.' prompt. It prints both at cold start AND each time a
    // program comes back through F021, because REENT falls into the cold-start code.
    bool prompt() { return until("MONITOR V1.0\r\n."); }

    // Type one command at the '.' prompt, as an operator does: a key at a time, each one sent
    // only after the monitor has echoed the one before. Leaves the console holding only what
    // the monitor printed after the command line.
    bool type(const std::string& cmd) {
        for (char ch : cmd) {
            tty->clearOut();
            tty->feed(std::string(1, ch));
            if (!until(std::string(1, ch))) return false;
        }
        tty->clearOut();
        tty->feed("\r");
        return true;
    }

    void    load(uint16_t at, const std::vector<uint8_t>& prog) {
        for (uint8_t b : prog) m.bus.memWrite(at++, b);
    }
    void    wr(uint16_t a, uint8_t v) { m.bus.memWrite(a, v); }
    uint8_t look(uint16_t a) { return m.bus.peek(a); }
};

// THE PB1 MANUAL'S OBJECT CODE, VERBATIM -- nothing patched, the exits still JMP F021.
// Section 4.2, the 2708 programmer, ORG 0100 (tests/media/pb1/PROG2708.HEX).
const std::vector<uint8_t> kProg2708 = {
    0x3E, 0x01, 0xD3, 0x10, 0x06, 0xFF, 0x0E, 0x03, 0x11, 0x00, 0xD0, 0x21, 0x00, 0x40, 0x7E, 0x12,
    0x13, 0x23, 0x7A, 0xA1, 0xB3, 0xC2, 0x0E, 0x01, 0x05, 0xC2, 0x08, 0x01, 0x1B, 0x1A, 0xC3, 0x21,
    0xF0,
};
// Section 4.4, the erase check, ORG 0140 (tests/media/pb1/ERACHK.HEX). Prints P or F.
const std::vector<uint8_t> kEraChk = {
    0x0E, 0x03, 0x11, 0x00, 0xD0, 0x1A, 0x13, 0xFE, 0xFF, 0x3E, 0x46, 0xC2, 0x56, 0x01, 0x7A, 0xA1,
    0xB3, 0xC2, 0x45, 0x01, 0x3E, 0x50, 0x4F, 0xCD, 0x09, 0xF0, 0xC3, 0x21, 0xF0,
};
// Section 4.5, the copy verify, ORG 0180 (tests/media/pb1/CPYVFY.HEX). Prints P or F.
const std::vector<uint8_t> kCpyVfy = {
    0x0E, 0x03, 0x11, 0x00, 0xD0, 0x21, 0x00, 0x40, 0x46, 0x1A, 0x13, 0x23, 0xB8, 0x3E, 0x46, 0xC2,
    0x9A, 0x01, 0x7A, 0xA1, 0xB3, 0xC2, 0x88, 0x01, 0x3E, 0x50, 0x4F, 0xCD, 0x09, 0xF0, 0xC3, 0x21,
    0xF0,
};

} // namespace

void test_pb1() {
    SECTION("pb1 -- the decode: a write-only control port, and the 4K memory window");
    {
        Rig g;
        BusCycle c;
        c.type = Cycle::IoWrite; c.addr = 0x10;
        CHECK(g.pb->decodes(c), "decodes OUT 10 -- the arm/type control port");
        c.type = Cycle::IoRead;
        CHECK(!g.pb->decodes(c), "does NOT decode IN 10 -- the control port is write-only");
        c.type = Cycle::MemWrite; c.addr = 0xD000;
        CHECK(g.pb->decodes(c), "claims a write anywhere in the window (armed or not)");
        c.type = Cycle::MemRead;
        CHECK(g.pb->decodes(c), "and a read there");
        c.addr = 0xCFFF;
        CHECK(!g.pb->decodes(c), "but not the byte just below the window");
        c.addr = 0xE000;
        CHECK(!g.pb->decodes(c), "and nothing above it (no on-board PROM mounted)");
    }

    SECTION("pb1 -- arm, burn (AND-only), and a window read disarms the board");
    {
        Rig g;
        // Un-armed, a window write cannot burn.
        g.wr(0xD000, 0x3C);
        CHECK(g.look(0xD000) == 0xFF, "a write with the board un-armed does nothing -- still erased");

        // Arm for the 2708 and burn a byte.
        g.out(0x10, 0x01);
        g.wr(0xD000, 0x3C);
        CHECK(g.look(0xD000) == 0x3C, "armed for 2708: the byte is programmed");

        // Programming an EPROM cell only clears bits -- a second write ANDs.
        g.wr(0xD000, 0xC3);
        CHECK(g.look(0xD000) == (uint8_t)(0x3C & 0xC3), "a second burn ANDs; a 1 cannot come back");

        // A read of the window resets the flip-flop (the LED goes out) -- writes stop burning.
        (void)g.rd(0xD000);
        g.wr(0xD001, 0x55);
        CHECK(g.look(0xD001) == 0xFF, "after a window read disarms it, a write no longer burns");
    }

    SECTION("pb1 -- the D0/D1 latch picks the socket; the 2716 reaches 2K, the 2708 does not");
    {
        Rig g;
        g.out(0x10, 0x01); g.wr(0xD010, 0xAA);  // a 2708 byte in U22
        g.out(0x10, 0x02); g.wr(0xD010, 0x55);  // a 2716 byte in U23 -- a different socket
        CHECK(g.look(0xD010) == 0x55, "with 2716 latched the window maps to U23");
        g.out(0x10, 0x01);
        CHECK(g.look(0xD010) == 0xAA, "and 2708 maps back to U22 -- two independent sockets");

        // The 2708 is 1K; the 2716 is 2K. Above 1K only the 2716 socket answers.
        g.out(0x10, 0x02); g.wr(0xD500, 0x11);
        CHECK(g.look(0xD500) == 0x11, "the 2716 burns above 1K");
        g.out(0x10, 0x01);
        CHECK(g.look(0xD500) == 0xFF, "the 2708 does not -- past 1K its socket floats");
    }

    SECTION("pb1 -- SNAPSHOT carries the burn, the arm flip-flop, and the type latch");
    {
        Rig g;
        g.out(0x10, 0x02);       // arm, 2716
        g.wr(0xD000, 0x7E);      // burn one byte -- and leave the board ARMED (no read)
        StateWriter w;
        g.pb->serialize(w);

        Rig g2;
        StateReader r(w.data());
        g2.pb->deserialize(r);
        CHECK(g2.look(0xD000) == 0x7E, "the burned 2716 byte survived the snapshot");
        g2.wr(0xD001, 0x22);
        CHECK(g2.look(0xD001) == 0x22, "the arm flip-flop and 2716 type latch travelled too");
    }

    SECTION("pb1 -- the real SSM 2708 burner copies RAM to the socket, end to end");
    {
        Rig g;

        // The SSM PB1 manual's 2708 programmer (section 4.2), object code verbatim, exit HLT.
        // Arms the 2708, copies 1K from 4000 into the window at D000, then reads the socket once
        // to disarm. This is the same program as tests/media/pb1/PROG2708.HEX, but its exit
        // is HLT, so this section tests the board with no monitor in the machine. The next
        // section runs the unpatched program, which returns to the SSM monitor at F021.
        const uint8_t prog[] = {
            0x3E, 0x01,        // MVI A,01     ; 2708 mode
            0xD3, 0x10,        // OUT 10       ; arm + latch type
            0x06, 0xFF,        // MVI B,FF     ; 256 passes
            0x0E, 0x03,        // MVI C,03     ; 1024 bytes
            0x11, 0x00, 0xD0,  // LXI D,D000   ; -> socket window
            0x21, 0x00, 0x40,  // LXI H,4000   ; -> source
            0x7E,              // MOV A,M
            0x12,              // STAX D       ; burn a byte
            0x13,              // INX D
            0x23,              // INX H
            0x7A,              // MOV A,D
            0xA1,              // ANA C
            0xB3,              // ORA E
            0xC2, 0x0E, 0x01,  // JNZ 010E
            0x05,              // DCR B
            0xC2, 0x08, 0x01,  // JNZ 0108
            0x1B,              // DCX D
            0x1A,              // LDAX D       ; read the socket -> disarm
            0x76,              // HLT
        };
        uint16_t a = 0x0100;
        for (uint8_t b : prog) g.wr(a++, b);
        for (int i = 0; i < 1024; ++i) g.wr((uint16_t)(0x4000 + i), (uint8_t)(i & 0xFF));

        g.m.cpu()->setPc(0x0100);
        RunResult res = g.m.debug.run(50000000);
        CHECK(res.why == StopReason::Halted, "the burner ran to its HLT");

        bool ok = true;
        for (int i = 0; i < 1024; ++i)
            if (g.look((uint16_t)(0xD000 + i)) != (uint8_t)(i & 0xFF)) ok = false;
        CHECK(ok, "every one of the 1024 source bytes landed in the 2708 socket");

        // The final LDAX D disarmed the board: a stray write can no longer burn.
        g.wr(0xD3FF, 0x00);
        CHECK(g.look(0xD3FF) == 0xFF, "the burner's closing read left the board disarmed");
    }

    SECTION("pb1 -- the manual's programs run under the SSM 8080 monitor, unpatched");
    {
        // What an operator did in 1978: at the monitor's '.' prompt, G<addr> starts a program,
        // and the program comes back to the monitor by itself. Nothing is patched and no
        // breakpoint catches the exit.
        MonRig g;
        g.load(0x0100, kProg2708);
        g.load(0x0140, kEraChk);
        g.load(0x0180, kCpyVfy);
        for (int i = 0; i < 1024; ++i) g.wr((uint16_t)(0x4000 + i), (uint8_t)(i * 7 + 1));

        CHECK(g.prompt(), "the monitor cold-starts to its banner and prompt");
        // The monitor sized memory on the way: its stack must sit in RAM below the window, and
        // the probe must not have burned anything into the unarmed socket.
        CHECK(g.look(0xD000) == 0xFF && g.look(0xD3FF) == 0xFF,
              "the monitor's memory sizing left the erased socket alone");

        CHECK(g.type("G140") && g.prompt(), "ERACHK runs and returns to the monitor");
        CHECK(g.tty->out().find("P\r\nMONITOR") != std::string::npos,
              "the erase check prints P for an erased chip, through the monitor's F009");

        CHECK(g.type("G100") && g.prompt(), "the 2708 burner returns to the monitor at F021");
        bool ok = true;
        for (int i = 0; i < 1024; ++i)
            if (g.look((uint16_t)(0xD000 + i)) != (uint8_t)(i * 7 + 1)) ok = false;
        CHECK(ok, "every one of the 1024 source bytes landed in the 2708 socket");
        CHECK(g.tty->out().find('P') == std::string::npos &&
                  g.tty->out().find('F') == std::string::npos,
              "the burner itself prints nothing");

        CHECK(g.type("G180") && g.prompt(), "CPYVFY runs and returns to the monitor");
        CHECK(g.tty->out().find("P\r\nMONITOR") != std::string::npos,
              "the verify prints P when the chip equals the source");

        g.wr(0x4123, (uint8_t)~g.look(0x4123));  // the source no longer equals the chip
        CHECK(g.type("G180") && g.prompt(), "CPYVFY runs again");
        CHECK(g.tty->out().find("F\r\nMONITOR") != std::string::npos,
              "the verify prints F when one byte is different");

        CHECK(g.type("G140") && g.prompt(), "ERACHK runs again");
        CHECK(g.tty->out().find("F\r\nMONITOR") != std::string::npos,
              "the erase check prints F for the burned chip");
    }

    SECTION("pb1 -- the burned socket saves to an Intel HEX file and round-trips (issue #382)");
    {
        Rig g;
        g.out(0x10, 0x01);
        for (int i = 0; i < 16; ++i) g.wr((uint16_t)(0xD000 + i), (uint8_t)(0xF0 + i));

        // `SAVE file window` builds this exact Image from the bus and writes saveHex().
        Image img;
        for (int i = 0; i < 16; ++i) img.bytes[0xD000 + i] = g.look((uint16_t)(0xD000 + i));
        std::string hex = saveHex(img);

        Image           back;
        std::string     e;
        std::vector<uint8_t> raw(hex.begin(), hex.end());
        CHECK(loadHex(raw, back, e), "the hex we wrote loads back");
        CHECK(back.bytes == img.bytes, "the burned bytes round-trip through Intel HEX unchanged");
        CHECK(back.bytes[0xD000] == 0xF0, "and they are the burned values, at the socket address");
    }

    SECTION("pb1 -- an on-board [[board.prom]] area answers reads and refuses writes");
    {
        Rig g;

        // A tiny read-only image on the host, mounted into an on-board socket above 8000.
        const std::string path = "pb1_promtest.tmp";
        {
            std::ofstream f(path, std::ios::binary);
            const uint8_t rom[] = {0xDE, 0xAD, 0xBE, 0xEF};
            f.write(reinterpret_cast<const char*>(rom), sizeof rom);
        }
        std::string err;
        CHECK(g.pb->loadSubUnit("prom", {{"at", "E000"}, {"mount", path}}, err),
              "a [[board.prom]] socket loads its image");

        BusCycle c;
        c.type = Cycle::MemRead; c.addr = 0xE000;
        CHECK(g.pb->decodes(c), "the on-board PROM now decodes reads");
        CHECK(g.rd(0xE000) == 0xDE, "and returns the firmware byte");
        CHECK(g.rd(0xE003) == 0xEF, "across the image");

        c.type = Cycle::MemWrite;
        CHECK(!g.pb->decodes(c), "it does NOT claim a write -- the on-board area is read-only");
        g.wr(0xE000, 0x00);  // lands nowhere
        CHECK(g.look(0xE000) == 0xDE, "so a write cannot change it");

        std::remove(path.c_str());
    }
}
