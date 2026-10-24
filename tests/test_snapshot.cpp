#include "test.h"

#include "boards/mits-88acr.h"
#include "boards/mits-frontpanel.h"
#include "boards/s100-memory.h"
#include "core/crc32.h"
#include "core/machine.h"
#include "core/statefile.h"
#include "cpu/cpu.h"
#include "host/cardimg.h"
#include "host/media.h"
#include "host/tape.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <span>
#include <sstream>
#include <string>
#include <vector>

using namespace altair;

namespace {

// Set/read a CPU register by NAME through the reflection layer -- the same door
// SNAPSHOT itself would use, so the test does not depend on the 8080's field order.
void setReg(Machine& m, const std::string& name, uint32_t v) {
    for (auto& rd : m.cpu()->registers())
        if (rd.name == name) { rd.set(v); return; }
}
uint32_t getReg(Machine& m, const std::string& name) {
    for (auto& rd : m.cpu()->registers())
        if (rd.name == name) return rd.get();
    return 0xFFFFFFFF;
}

MemoryBoard* addRam(Machine& m, const std::string& id, uint16_t at, uint32_t size) {
    std::string err;
    auto* mb = dynamic_cast<MemoryBoard*>(m.add("memory", id, err));
    Region r;
    r.kind = RegionKind::Ram;
    r.at   = at;
    r.size = size;
    mb->addRegion(r, err);
    return mb;
}

// A same-build round trip restores every board: true only with no notes.
bool restoreWhole(Machine& m, const std::string& path, std::string& err) {
    std::vector<std::string> notes;
    return m.restore(path, err, notes) && notes.empty();
}

bool hasNote(const std::vector<std::string>& notes, const std::string& part) {
    for (const auto& n : notes)
        if (n.find(part) != std::string::npos) return true;
    return false;
}

// Read a snapshot file, let `edit` change its format version or its board sections, and
// write it back with a fresh checksum -- an older or damaged file, made to order.
struct Section {
    std::string          id, type;
    std::vector<uint8_t> bytes;
};
template <class Edit>
bool rewriteSnap(const std::string& path, Edit edit) {
    std::vector<uint8_t> buf;
    {
        std::ifstream f(path, std::ios::binary);
        buf.assign(std::istreambuf_iterator<char>(f), {});
    }
    if (buf.size() < 16) return false;
    StateReader r(buf.data(), buf.size() - 4);
    uint8_t magic[8];
    r.raw(magic, sizeof magic);
    uint32_t    ver  = r.u32();
    std::string name = r.str();
    uint32_t    n    = r.u32();
    std::vector<uint8_t> clockBytes = r.blob();
    std::vector<Section> secs(n);
    for (auto& s : secs) {
        s.id    = r.str();
        s.type  = r.str();
        s.bytes = r.blob();
    }
    if (!r.ok()) return false;
    edit(ver, secs);

    StateWriter w;
    w.raw(magic, sizeof magic);
    w.u32(ver);
    w.str(name);
    w.u32((uint32_t)secs.size());
    w.blob(clockBytes);
    for (const auto& s : secs) {
        w.str(s.id);
        w.str(s.type);
        w.blob(s.bytes);
    }
    w.u32(crc32(std::span<const uint8_t>(w.data().data(), w.data().size())));
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write((const char*)w.data().data(), (std::streamsize)w.data().size());
    return (bool)f;
}

std::string tmpSnap(const char* leaf) {
    return (std::filesystem::temp_directory_path() / leaf).string();
}

}  // namespace

// The machine-level round trip (DESIGN.md 13): snapshot the state, scribble over
// every part of it, restore, and prove the machine is bit-for-bit where it was.
void test_snapshot() {
    SECTION("snapshot: CPU + RAM + a board latch round trip");
    {
        Machine m;
        std::string err;
        m.add("8080", "cpu0", err);
        MemoryBoard* mem = addRam(m, "mem0", 0x0000, 0x10000);
        m.add("2sio", "sio0", err);
        auto* fp = dynamic_cast<FrontPanelBoard*>(m.add("fp", "panel", err));
        m.power();

        // Put a fingerprint on every layer.
        setReg(m, "A", 0x42);
        setReg(m, "B", 0x11);
        setReg(m, "SP", 0xABCD);
        m.cpu()->setPc(0x1234);
        m.bus.memWrite(0x2000, 0x37);
        m.bus.memWrite(0xFFFF, 0x9E);
        fp->setSwitches(0xC3A5);

        std::string path = tmpSnap("altairsim-snap-test.snap");
        CHECK(m.snapshot(path, err), "snapshot writes");

        // Now scribble over all of it.
        setReg(m, "A", 0x00);
        setReg(m, "B", 0x00);
        setReg(m, "SP", 0x0000);
        m.cpu()->setPc(0x0000);
        m.bus.memWrite(0x2000, 0x00);
        m.bus.memWrite(0xFFFF, 0x00);
        fp->setSwitches(0x0000);

        CHECK(restoreWhole(m, path, err), "restore reads, every board");

        CHECK(getReg(m, "A") == 0x42, "A restored");
        CHECK(getReg(m, "B") == 0x11, "B restored");
        CHECK(getReg(m, "SP") == 0xABCD, "SP restored");
        CHECK(m.cpu()->pc() == 0x1234, "PC restored");
        CHECK(m.bus.memRead(0x2000) == 0x37, "RAM at 2000 restored");
        CHECK(m.bus.memRead(0xFFFF) == 0x9E, "RAM at FFFF restored");
        CHECK(fp->switches() == 0xC3A5, "front-panel switches restored");
        (void)mem;

        std::filesystem::remove(path);
    }

    SECTION("snapshot: a tape's head position and auto-stop mark travel");
    {
        // A cassette's bytes are host-backed and do not travel; its head position and its
        // auto-stop mark are the runtime state that must (DESIGN.md 13). This is what the
        // v2 format added, and the reason the format version was bumped.
        setMediaResolver([](const std::string& p, bool ro, std::string&) {
            return std::make_unique<MemoryMedia>(p, std::vector<uint8_t>(2000, 0x5A), ro);
        });
        Machine     m;
        std::string err;
        m.add("8080", "cpu0", err);
        addRam(m, "mem0", 0x0000, 0x10000);
        auto* acr = dynamic_cast<AcrBoard*>(m.add("acr", "acr0", err));
        m.power();
        CHECK(acr->mount("tape", "t.tap", false, err), "a tape goes in");

        // Arm a stop and move the head, then remember where both are.
        CHECK(setUnitProperty(*acr, "tape", "stop", "0:20", err), "arm a stop");
        std::ostringstream o;
        CHECK(acr->runCommand("WIND", {"WI", "acr0:tape", "0:10"}, o, err), "wind the head in");
        const uint64_t p0 = acr->tape()->pos();
        const uint64_t s0 = acr->tape()->stopAt();
        CHECK(p0 > 0 && s0 != TapeImage::kNoStop, "the head moved and the stop is armed");

        std::string path = tmpSnap("altairsim-snap-tape.snap");
        CHECK(m.snapshot(path, err), "snapshot writes");

        // Scribble over both -- wind to the end and clear the stop.
        CHECK(acr->runCommand("WIND", {"WI", "acr0:tape", "END"}, o, err), "wind to the end");
        CHECK(setUnitProperty(*acr, "tape", "stop", "off", err), "clear the stop");
        CHECK(acr->tape()->pos() != p0 || acr->tape()->stopAt() != s0, "scribbled");

        CHECK(restoreWhole(m, path, err), "restore reads, every board");
        CHECK(acr->tape()->pos() == p0, "the head position came back");
        CHECK(acr->tape()->stopAt() == s0, "the auto-stop mark came back");

        std::filesystem::remove(path);
        // Put back the resolver tests/main.cpp installs. An EMPTY one leaks: every MOUNT in a
        // later suite fails (`altair_tests snapshot mcp` failed the MCP tape test).
        setMediaResolver(openHostMedia);
    }

    SECTION("snapshot: enabled_ travels (a board that switched itself off)");
    {
        Machine m;
        std::string err;
        m.add("8080", "cpu0", err);
        MemoryBoard* mem = addRam(m, "mem0", 0x0000, 0x1000);
        m.power();

        mem->setEnabled(false);
        std::string path = tmpSnap("altairsim-snap-enabled.snap");
        CHECK(m.snapshot(path, err), "snapshot writes");
        mem->setEnabled(true);
        CHECK(restoreWhole(m, path, err), "restore reads, every board");
        CHECK(!mem->enabled(), "a disabled board comes back disabled");
        std::filesystem::remove(path);
    }

    SECTION("snapshot: a snapshot is deterministic (byte-for-byte)");
    {
        Machine m;
        std::string err;
        m.add("8080", "cpu0", err);
        addRam(m, "mem0", 0x0000, 0x1000);
        m.power();
        setReg(m, "A", 0x7E);
        m.bus.memWrite(0x0100, 0x5A);

        std::string p1 = tmpSnap("altairsim-snap-det1.snap");
        std::string p2 = tmpSnap("altairsim-snap-det2.snap");
        CHECK(m.snapshot(p1, err), "first snapshot writes");
        CHECK(m.snapshot(p2, err), "second snapshot writes");

        std::vector<uint8_t> b1, b2;
        {
            // Scope the streams so they CLOSE before the remove() below. On Windows a
            // file that is still open cannot be deleted, and the throwing
            // std::filesystem::remove then throws filesystem_error -- which on Unix it
            // never does, because unlinking an open file is legal there. Reading the
            // bytes in a nested scope closes the handles first and keeps the cleanup
            // portable. (This is why the Windows unit binary faulted with 0xC0000409.)
            std::ifstream f1(p1, std::ios::binary), f2(p2, std::ios::binary);
            b1.assign(std::istreambuf_iterator<char>(f1), {});
            b2.assign(std::istreambuf_iterator<char>(f2), {});
        }
        CHECK(!b1.empty() && b1 == b2, "two snapshots of one state are identical bytes");
        std::filesystem::remove(p1);
        std::filesystem::remove(p2);
    }

    SECTION("snapshot: boards are matched by id; the rest start at power-on");
    {
        Machine a;
        std::string err;
        a.add("8080", "cpu0", err);
        addRam(a, "mem0", 0x0000, 0x1000);
        a.power();
        setReg(a, "A", 0x5C);
        a.bus.memWrite(0x0000, 0xEE);
        std::string path = tmpSnap("altairsim-snap-topo.snap");
        CHECK(a.snapshot(path, err), "snapshot writes");

        // A DIFFERENT machine: mem0 is gone, and a board the file does not hold is new.
        Machine b;
        b.add("8080", "cpu0", err);
        MemoryBoard* fresh = addRam(b, "mem9", 0x8000, 0x1000);
        CHECK(setProperty(*fresh, "fill", "zero", err), "mem9 powers up zeroed");
        b.power();
        setReg(b, "A", 0x00);
        b.bus.memWrite(0x8000, 0x11);
        std::vector<std::string> notes;
        CHECK(b.restore(path, err, notes), "restore imports what the machines share");
        CHECK(getReg(b, "A") == 0x5C, "cpu0 came back");
        CHECK(b.bus.memRead(0x8000) == 0x00, "mem9 is not in the file: it starts at power-on");
        CHECK(notes.size() == 2 && hasNote(notes, "'mem0' (memory) is in the snapshot but not") &&
                  hasNote(notes, "'mem9' (memory) is not in the snapshot"),
              "and each board not restored is named");

        // The right id, but another type of board.
        Machine c;
        c.add("8080", "cpu0", err);
        c.add("2sio", "mem0", err);
        c.power();
        CHECK(c.restore(path, err, notes), "restore imports cpu0");
        CHECK(notes.size() == 1 && hasNote(notes, "'mem0' is a memory in the snapshot and a 2sio"),
              "and names the board whose type differs");

        // Nothing in common: refused, and the machine is untouched.
        Machine d;
        addRam(d, "mem7", 0x0000, 0x1000);
        d.power();
        d.bus.memWrite(0x0000, 0x22);
        CHECK(!d.restore(path, err, notes), "a file with no board in common is refused");
        CHECK(!err.empty() && notes.empty(), "and it says why");
        CHECK(d.bus.memRead(0x0000) == 0x22, "and the machine is untouched");

        std::filesystem::remove(path);
    }

    SECTION("snapshot: an older format restores the boards whose state is unchanged");
    {
        Machine m;
        std::string err;
        m.add("8080", "cpu0", err);
        addRam(m, "mem0", 0x0000, 0x1000);
        m.add("ss1", "ss1", err);
        m.power();
        setReg(m, "A", 0x3D);
        m.bus.memWrite(0x0100, 0x77);
        std::string path = tmpSnap("altairsim-snap-old.snap");
        CHECK(m.snapshot(path, err), "snapshot writes");

        // The same file, labeled format 3 (1.3.1 to 1.3.4): the ss1 timer's state changed
        // meaning in format 4 and keeps its length, so only the table can catch it.
        CHECK(rewriteSnap(path, [](uint32_t& ver, std::vector<Section>&) { ver = 3; }),
              "relabel it format 3");
        setReg(m, "A", 0x00);
        m.bus.memWrite(0x0100, 0x00);
        std::vector<std::string> notes;
        CHECK(m.restore(path, err, notes), "a format 3 file is read");
        CHECK(getReg(m, "A") == 0x3D && m.bus.memRead(0x0100) == 0x77, "cpu0 and mem0 came back");
        CHECK(notes.size() == 1 && hasNote(notes, "'ss1' (ss1): its state is in snapshot format 3"),
              "and the ss1 is named");

        // A NEWER format than this build: refused whole.
        CHECK(rewriteSnap(path, [](uint32_t& ver, std::vector<Section>&) { ver = 99; }),
              "relabel it format 99");
        m.bus.memWrite(0x0100, 0x00);
        CHECK(!m.restore(path, err, notes), "a file from a newer build is refused");
        CHECK(err.find("newer") != std::string::npos, "and it says so");
        CHECK(m.bus.memRead(0x0100) == 0x00, "and the machine is untouched");

        std::filesystem::remove(path);
    }

    SECTION("snapshot: a board section of the wrong length is not misread");
    {
        Machine m;
        std::string err;
        m.add("8080", "cpu0", err);
        MemoryBoard* mem = addRam(m, "mem0", 0x0000, 0x1000);
        CHECK(setProperty(*mem, "fill", "zero", err), "mem0 powers up zeroed");
        m.power();
        setReg(m, "A", 0x6B);
        m.bus.memWrite(0x0100, 0x77);
        std::string path = tmpSnap("altairsim-snap-len.snap");
        CHECK(m.snapshot(path, err), "snapshot writes");

        for (int grow : {+1, -1}) {
            CHECK(m.snapshot(path, err), "snapshot writes");
            CHECK(rewriteSnap(path,
                              [grow](uint32_t&, std::vector<Section>& secs) {
                                  for (auto& s : secs)
                                      if (s.id == "mem0") {
                                          if (grow > 0) s.bytes.push_back(0);
                                          else s.bytes.pop_back();
                                      }
                              }),
                  "change mem0's section length");
            setReg(m, "A", 0x00);
            m.bus.memWrite(0x0100, 0x55);
            std::vector<std::string> notes;
            CHECK(m.restore(path, err, notes), "the file is read");
            CHECK(getReg(m, "A") == 0x6B, "cpu0 came back");
            CHECK(m.bus.memRead(0x0100) == 0x00, "mem0 starts at power-on, not half-read");
            CHECK(notes.size() == 1 && hasNote(notes, "'mem0' (memory): its saved state does not fit"),
                  grow > 0 ? "a section one byte long is named" : "a section one byte short is named");
            m.bus.memWrite(0x0100, 0x77);
            setReg(m, "A", 0x6B);
        }

        std::filesystem::remove(path);
    }

    SECTION("snapshot: a corrupt file is refused");
    {
        Machine m;
        std::string err;
        m.add("8080", "cpu0", err);
        addRam(m, "mem0", 0x0000, 0x1000);
        m.power();
        std::string path = tmpSnap("altairsim-snap-corrupt.snap");
        CHECK(m.snapshot(path, err), "snapshot writes");

        // Flip a byte in the middle of the file.
        {
            std::fstream f(path, std::ios::in | std::ios::out | std::ios::binary);
            f.seekg(0, std::ios::end);
            auto len = f.tellg();
            f.seekp(len / 2);
            char c;
            f.seekg(len / 2);
            f.get(c);
            c ^= 0x01;
            f.seekp(len / 2);
            f.put(c);
        }
        std::vector<std::string> notes;
        CHECK(!m.restore(path, err, notes), "a tampered snapshot is refused");
        std::filesystem::remove(path);
    }
}
