#pragma once
//
// FD1771 -- Western Digital's Floppy Disk Formatter/Controller. The first of the
// family; the one the Tarbell soft-sector card is built around.
//
// A CHIP, NOT A CARD. The Tarbell Floppy Disk Interface has one of these on it, and
// the next card to turn up with one gets it for free. Modeled from the FD1771 data
// sheet (reference/Western Digital FD1771 - Datasheet.pdf), cross-checked against
// the copy Tarbell reprinted as section 7-2 of their own manual -- NOT from the one
// BIOS that happens to drive it, which is how you end up implementing the subset
// that BIOS uses and quietly getting the rest wrong.
//
// It knows nothing about S-100, and it knows nothing about the Tarbell's ports.
//
// ---------------------------------------------------------------------------
// WHY THE FILE IS `wd17xx` AND THE CLASS IS `Wd1771`.
//
// The family shares a register file and a command set, and a later card will want
// the FD1791/93. It will NOT want this class: the 179x is a different chip in the
// ways that bite (MFM, side select, a one-bit record type). So the FILE is the
// family and the CLASS is the part, and the 1793 lands next to this one rather than
// inside it, behind a `#ifdef` that would be a lie either way.
//
// AND IT IS NOT THE WD1770/72/73. `reference/Western Digital WD177X-00 - Datasheet.pdf`
// is in the tree and it is THE WRONG CHIP for this card -- a 28-pin part with an
// on-board data separator, MFM, and motor control. Its step-rate table reads
// 6/12/20/30 ms. THE FD1771's READS 6/6/10/20 (Table 1, at CLK=2 MHz). A chip built
// from that sheet would look right, compile, boot nothing, and step at the wrong
// speed forever. Do not "fix" this file against it.
// ---------------------------------------------------------------------------

#include "chips/floppy.h"
#include "core/clock.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace altair {

// ---------------------------------------------------------------------------
// THE FD177x FAMILY -- the base, and the parts.
//
// `Wd17xx` is the whole register file and state machine, which every part in the
// family shares byte-for-byte (that is what "the family shares a register file and a
// command set" means, and it is why it lives in ONE class and not three copies). The
// PARTS -- Wd1771, Wd1791 -- are the leaves, and each supplies only the handful of
// behaviors that genuinely differ between the silicon:
//
//   - the STEP-RATE table (Table 1 is not the same on the 1771 and the 1791);
//   - the READ ADDRESS side byte (the 1771 has no side pin and hard-zeros it; the
//     179x carries the head/side number the board selected);
//   - the RECORD TYPE (the 1771 has FOUR data address marks and reports two status
//     bits; the 179x has TWO and reports one);
//   - which DATA ADDRESS MARK a Write command writes (the a1a0 field on the 1771; a
//     single a0 bit on the 179x).
//
// So the base is abstract: it cannot answer those four questions itself, and it must
// not guess. A board constructs the PART it has on it (Wd1771 for a VersaFloppy I or a
// Tarbell SD card; Wd1791 for a VersaFloppy II or a Tarbell DD card), and the base
// runs the command set through the part's answers. New part, new leaf -- next to these,
// never inside one, exactly as the file-level note above lays out.
// ---------------------------------------------------------------------------
class Wd17xx {
public:
    explicit Wd17xx(std::string name) : name_(std::move(name)) {}
    virtual ~Wd17xx() = default;

    const std::string& name() const { return name_; }

    // ---- THE REGISTER FILE (A1,A0 under RE/WE) ----
    //
    //      A1 A0     READ            WRITE
    //       0  0     Status Reg      Command Reg
    //       0  1     Track Reg       Track Reg
    //       1  0     Sector Reg      Sector Reg
    //       1  1     Data Reg        Data Reg
    //
    // The DAL is an INVERTED bus on this chip (pins 7-14, "eight bit inverted
    // bidirectional bus"). The inversion IS NOT DONE HERE: the Tarbell runs the bus
    // through inverting buffers on its way to S-100, so the two inversions cancel and
    // the guest sees true data. Putting it in the chip and un-doing it on the card
    // would be two lies that happen to agree. THE CHIP'S REGISTERS ARE TRUE SENSE.
    // (Same rule as the COM2502's status bits -- see uart1602.h.)
    uint8_t readStatus(const Clock& clk);
    uint8_t readTrackReg() const { return track_; }
    uint8_t readSectorReg() const { return sector_; }
    uint8_t readData(const Clock& clk);

    void writeCommand(uint8_t v, const Clock& clk);
    void writeTrackReg(uint8_t v) { track_ = v; }
    void writeSectorReg(uint8_t v) { sector_ = v; }
    void writeData(uint8_t v, const Clock& clk);

    // ---- MASTER RESET -- pin 19, AND THIS CHIP REALLY HAS ONE ----
    //
    // Unlike the 6850 (which has no reset pin at all -- see mc6850.h), the FD1771 has
    // MR, and on the Tarbell the S-100 RESET* reaches it. What it does is NOT "clear
    // the registers", and if you assume it is you will be hunting the difference for
    // a day:
    //
    //   "A logic low on this input resets the device and loads '03' into the command
    //    register. The Not Ready (Status bit 7) is reset during MR ACTIVE. When MR is
    //    brought to a logic high, A RESTORE COMMAND IS EXECUTED, regardless of the
    //    state of the Ready signal from the drive."
    //
    // So a front-panel RESET does not merely idle the controller -- IT DRIVES THE HEAD
    // BACK TO TRACK 0, by itself, with no help from any software. 0x03 is Restore with
    // h=0, V=0 and r1r0=11: the 20 ms step rate, the slowest one, which is what you
    // want when you have no idea where the head is.
    void masterReset(const Clock& clk);

    // POWER APPLIED. A known-good state at once, so the card is usable the instant the
    // machine comes up (DESIGN.md 6.1). Not the same thing as MR: powering on does not
    // restore, because at power-on there is not yet a drive to restore.
    void powerOn(const Clock& clk);

    // ---- THE TWO OUTPUT PINS THE CARD CARES ABOUT ----
    //
    // INTRQ (pin 39): set at the completion OR termination of any operation. "The
    // INTRQ signal remains active until reset by reading the Status Register to the
    // processor or by the loading of the Command Register." Note that it is NOT reset
    // by reading the Data Register -- a driver that polls DRQ forever and never reads
    // status leaves INTRQ asserted, and on a card that jumpers it to pin 73 that is an
    // interrupt storm. It is meant to be that way.
    bool intrq() const { return intrq_; }

    // DRQ (pin 38): the Data Register has a byte in it (read) or wants one (write).
    // Also visible as status bit S1 on every command except Type I.
    bool drq() const { return drq_; }

    // ---- THE DRIVE ----
    //
    // The CARD's drive-select latch is what points this. A null drive is not an error
    // and not a crash: it is a card with nothing plugged into it, which reads NOT
    // READY, exactly as the bare connector does.
    void         attach(FloppyDrive* d) { drive_ = d; }
    FloppyDrive* drive() const { return drive_; }

    // ---- THE STRAPS ----
    //
    // The FDC's own crystal, which IS NOT THE CPU's. The Tarbell hangs a 4 MHz can off
    // the board and halves it; the chip wants 2 MHz. It selects the column of Table 1
    // -- and therefore how long a step takes -- and NOTHING else here, because every
    // other deadline in this chip is set by how fast the DISK turns, not by how fast
    // the chip's clock runs.
    long long fdcClockHz = 2000000;

    // The bit rate on the MEDIA. 250 kbit/s is the 8" single-density standard, which
    // is what the Tarbell manual quotes ("the standard speed of 250,000 bits per
    // second") and what an 8" drive turning at 360 RPM produces. It is a strap and not
    // a constant because a 5.25" drive runs at half of it, and the same chip drives
    // both. A double-density card doubles it -- the board writes it when it decodes its
    // density bit.
    long long dataRateBits = 250000;

    // The DDEN pin: FM (single density, false) or MFM (double). It is a pin of its own and
    // not a reading of dataRateBits, because the rate does not give the density -- a 5.25"
    // double-density disk runs at 250 kbit/s, the 8" single-density rate. The board drives
    // it from its density bit. Write Track records a track at it, and with the density
    // check on (setDensityChecked) a read sees only the ID fields recorded at it. A
    // single-density part (the FD1771) has no such pin and leaves it false.
    bool doubleDensity = false;

    // ---- WAIT-STATE (PRDY) SYNCHRONIZATION -- a board strap ----
    //
    // Some cards do not let the guest poll DRQ at all: the DRQ line drives a wait-state
    // generator that stalls the CPU (S-100 PRDY) until the byte -- or the whole command
    // -- is ready. The VersaFloppy is one (reference/SD Systems VersaFloppy.md 4): its
    // sector-transfer loop is a bare `IN A,(67H)` with NO software DRQ polling, and a
    // status read after a command blocks until the FDC is done.
    //
    // This is the board's `timing = full` model (the default). The clock does not advance
    // inside a bus cycle, so a wait-synced card sets this, and then EVERY register access resolves all
    // pending TIME-BASED progress first: poll() treats every deadline as already due and
    // runs the state machine forward, stopping only where it genuinely needs the guest --
    // the next byte of a transfer, which the data register serves on demand. A status read
    // therefore returns the finished result and a data read the next byte, each exactly as
    // if the CPU had waited on PRDY. It is FAITHFUL, not a shortcut: a wait-stalled CPU
    // cannot observe seek time, byte timing, or reach Lost Data -- so the collapsed command
    // time is invisible, and Lost Data (which the wait-state hardware exists to prevent) is
    // correctly unreachable. Off by default: a DRQ-polling card leaves it alone and gets the
    // byte-timed model above, Lost Data and all.
    //
    // One thing the wait states cannot hold back is the disk. A Write Track still ends at the
    // index pulse: when the guest has left a DRQ unserviced for one revolution, the rest of
    // the track is zeros and the command ends with Lost Data (with nothing written, if it
    // was the first DRQ). That is the only deadline a wait-synced chip keeps.
    //
    // `timing = real` (issue #637) leaves this OFF and charges the wait instead: the chip
    // runs its byte-timed model, and the board calls holdUntilReady() at the access the
    // hardware stalls on, then holds READY (Board::holdReady()) for what it returns.
    void setWaitSynced(bool on) { waitSynced_ = on; }
    bool waitSynced() const { return waitSynced_; }

    // A wait-synced Type I command (Restore, Seek, Step) that has not stepped yet.
    //
    // The step pulses go to the drive that is selected WHEN THEY HAPPEN, and they happen
    // milliseconds after the command is loaded. A driver may load the command and select
    // another drive in the next few instructions without waiting -- the Tarbell CBIOS HOME
    // does exactly that -- and it is the new drive that moves. So loading the command moves
    // nothing, and poll() leaves it alone: the stepping happens on the guest's next access
    // to the board, which the board reports with touch(), or one step time on (nextEdge())
    // if the guest never comes back. The register accesses below touch() by themselves; the
    // board calls it for the ports that never reach the chip -- its drive-select latch, and
    // the track and sector registers. A real part may get its first pulse out before the
    // select lands; here the new drive takes them all.
    bool stepPending() const { return waitSynced_ && phase_ == Phase::Settle; }
    void touch(const Clock& clk);

    // ---- THE DENSITY CHECK -- a board strap ----
    //
    // A real part clocked for FM cannot see an MFM address mark, and the reverse: Read
    // Address at the wrong density finds no ID field and ends in Record Not Found. Software
    // LEANS on that. The SD Systems DDBIOS finds what kind of disk is in the drive by trying
    // each density in turn and keeping the first one whose Read Address succeeds
    // (DDB200.ASM, USL1) -- answer at every density and it keeps its first guess, double
    // density, for a single-density disk, and the boot dies on a sector that is not there.
    //
    // ON: an ID field recorded at the other density does not exist for this chip -- Read
    // Address, the Type II search and the Type I verify all pass it by. The chip's density
    // is doubleDensity (the DDEN pin), the same value Write Track records a track with, so
    // what a card formats at a density it reads back at that density.
    //
    // OFF (the default): the medium decides. The VersaFloppy (issue #691), the Tarbell
    // double-density card and the Cromemco FDCs (issue #692) turn it on.
    void setDensityChecked(bool on) { densityChecked_ = on; }
    bool densityChecked() const { return densityChecked_; }

    // THE CPU WAITS ON READY UNTIL DRQ OR THE END OF THE COMMAND. Runs the state machine
    // forward, deadline by deadline, until DRQ is up or the chip is idle, and returns the
    // T-states that took -- the wait states the board holds READY for. Zero if it is
    // already so, and zero on a wait-synced chip (that model charges no time at all).
    //
    // THE CHIP RUNS AHEAD OF THE CLOCK. The clock does not move inside an instruction; it
    // is charged the hold at the boundary (DESIGN.md 7.5). Until then the chip has lived
    // through time the clock has not reached yet, and ahead_ remembers how far, so every
    // time the chip reads is now() = max(clock, ahead_). After the instruction the clock
    // catches up and ahead_ is behind it again.
    uint32_t holdUntilReady(const Clock& clk);

    // The SIDE the board's drive-select latch has chosen (the 179x has a side-select
    // input pin; the 1771 does not). The board writes it; only a part that has the pin
    // reports it (Read Address byte 2). Kept here because it is a chip input, not state.
    void setSide(int s) { side_ = s; }

    // ADVANCE THE STATE MACHINE. Public because the CARD must be able to call it: a
    // seek takes 20 ms per track and the guest is not touching a register while it
    // happens, so a chip that only moved when a port was read would never finish one.
    void poll(const Clock& clk);

    // THE NEXT MOMENT THIS CHIP'S PINS COULD MOVE WITH NOBODY TOUCHING IT. Zero means
    // never. Always strictly in the future -- see Mc6850::nextEdge(), the reasoning is
    // identical and it is the difference between a deadline and a spin.
    uint64_t nextEdge(const Clock& clk) const;

    // Drain what the chip has to say (a drive that could not take a format). Cleared
    // by draining. Board::drainLog() is what surfaces it.
    std::vector<std::string> drainLog();

    // For SHOW and for tests: is a command running, and which.
    bool busy() const { return phase_ != Phase::Idle; }

    // SNAPSHOT/RESTORE (DESIGN.md 13). The whole register file and state machine --
    // including any command in flight (phase_, due_, the transfer buffer) and the
    // Force-Interrupt arming. The drive_ pointer and the format straps do not
    // travel; the owning card re-arms the Clock from due_ after restore.
    void serialize(StateWriter& w) const;
    void deserialize(StateReader& r);

protected:
    // ---- THE FOUR THINGS A PART ANSWERS FOR ITSELF (see the class note) ----
    //
    // The base runs the whole command set; these are the only places it has to ask the
    // specific chip what it does. Pure, because the base genuinely does not know and must
    // not guess -- a default here would silently be "the 1771", which is the bug the
    // family split exists to prevent.

    // Table 1, the step-rate column for this part, four rates in ms indexed by r1r0.
    // `twoMHz` selects by the FDC's own crystal (fdcClockHz), never the CPU's.
    virtual const int* stepRatesMs(bool twoMHz) const = 0;

    // Read Address byte 2: the side/head number the board selected, or a hard 0 on a
    // part with no side-select pin (the 1771).
    virtual uint8_t readAddressSide() const = 0;

    // The status bit(s) a READ sets when the data field carries a Deleted Data Mark:
    // S6|S5 on the 1771 (four marks, two bits), S5 alone on the 179x (two marks, one).
    virtual uint8_t deletedRecordBits() const = 0;

    // Does this Write Sector command's data-address-mark field select a DELETED mark?
    // The 1771 reads a two-bit a1a0 field (11 = F8); the 179x reads a single a0 bit.
    virtual bool cmdWritesDeletedMark(uint8_t cmd) const = 0;

    // ---- THE STATUS REGISTER IS SIX DIFFERENT REGISTERS (Table 6) ----
    //
    // "Status varies according to the type of command executed." Not according to the
    // command in the command register -- according to the one that WAS EXECUTED, which
    // is a LATCH, and it is the single easiest thing to get wrong in this chip:
    //
    //   - S6 is WRITE PROTECT after a Type I, and RECORD TYPE after a Read.
    //   - S4 is SEEK ERROR after a Type I, ID NOT FOUND after a Read Address, and
    //     RECORD NOT FOUND after a Read.
    //   - S2 is TRACK 0 after a Type I and LOST DATA after everything else.
    //   - S1 is INDEX after a Type I and DRQ after everything else.
    //
    // So a driver that seeks, then reads a sector, then polls status is looking at
    // three different meanings for bit 2 depending on where it is -- and a model that
    // reports one fixed layout will hand a BIOS "track 0" when it asked "did I lose
    // data". Latch the type; report through it.
    //
    // The Force Interrupt rule falls straight out of this, and it is in the data sheet
    // verbatim: FI *while a command runs* resets BUSY and LEAVES THE OTHER BITS ALONE
    // (so the context stays whatever it was). FI *while idle* updates them and "Status
    // reflects the Type I commands" -- i.e. it RESETS the context to Type I.
    enum class Ctx { TypeI, ReadAddress, Read, ReadTrack, Write, WriteTrack };

    // There is deliberately NO `Verify` phase. A verify is not a state the chip sits in
    // -- it is what HeadSettle hands off to, and it completes within the one call. The
    // 10 ms wait in front of it is the only part that takes time, and that is HeadSettle.
    enum class Phase {
        Idle,
        Settle,      // stepping: one step per rate interval
        HeadSettle,  // the 10 ms HLD->HLT delay, when E (or h) asked for it
        Read,        // streaming a data field (or an ID field, or a track) out to the guest
        WriteWait,   // Write: the FIRST DRQ. If the guest misses it, the command dies.
        Write,       // ...and the rest of them, where a miss writes a zero and carries on
        ReadDrain,   // the LAST byte is in the DR; hold a byte-time so the guest takes it
                     // before INTRQ (on silicon INTRQ trails the final DRQ, past the CRC).
                     // Appended LAST on purpose: phase_ serializes as its ordinal.
    };

    // What to do when the byte stream in `buf_` runs out. Read Sector, Read Address and
    // Read Track all STREAM IDENTICALLY -- a byte every byteTStates(), DRQ each time,
    // Lost Data if the guest was late -- and differ only in where the bytes came from
    // and what happens at the end. One streamer, three endings.
    enum class Ending { Plain, NextRecord, CommitSector, CommitTrack };

    // The bits. Named, because `1 << 4` in six different meanings is how the above
    // goes wrong quietly.
    static constexpr uint8_t kBusy      = 0x01;  // S0, every context
    static constexpr uint8_t kIndex     = 0x02;  // S1, Type I
    static constexpr uint8_t kDrq       = 0x02;  // S1, everything else
    static constexpr uint8_t kTrack0    = 0x04;  // S2, Type I
    static constexpr uint8_t kLostData  = 0x04;  // S2, everything else
    static constexpr uint8_t kCrcError  = 0x08;  // S3
    static constexpr uint8_t kSeekError = 0x10;  // S4, Type I
    static constexpr uint8_t kNotFound  = 0x10;  // S4, everything else
    static constexpr uint8_t kHeadLoad  = 0x20;  // S5, Type I
    static constexpr uint8_t kWriteFault= 0x20;  // S5, Write
    static constexpr uint8_t kRecType5  = 0x20;  // S5, Read -- LSB of the record type
    static constexpr uint8_t kProtected = 0x40;  // S6, Type I and Write
    static constexpr uint8_t kRecType6  = 0x40;  // S6, Read -- MSB of the record type
    static constexpr uint8_t kNotReady  = 0x80;  // S7, every context

    // Timing, all of it derived and none of it invented.
    uint64_t stepTStates(const Clock& clk) const;        // Table 1, via fdcClockHz and r1r0
    uint64_t byteTStates(const Clock& clk) const;        // dataRateBits / 8
    void requestWriteByte(const Clock& clk);             // DRQ for a write, and its deadline
    bool indexPassed(const Clock& clk) const;            // wait-synced Write Track: index came
    uint64_t headSettleTStates(const Clock& clk) const;  // 10 ms, or 20 at CLK=1 MHz
    uint64_t msTStates(const Clock& clk, int ms) const;

    void loadHead(bool on, const Clock& clk);  // drive HLD, and remember when
    bool headEngaged(const Clock& clk) const;  // HLD *and* HLT -- what S5 actually reports

    void startCommand(uint8_t cmd, const Clock& clk);
    void startTypeI(uint8_t cmd, const Clock& clk);
    void startTypeII(const Clock& clk);
    void startTypeIII(const Clock& clk);
    void forceInterrupt(uint8_t cmd, const Clock& clk);

    void headLoadThen(bool wanted, const Clock& clk);
    void afterHeadSettle(const Clock& clk);  // where HeadSettle hands off to
    void beginTypeII(const Clock& clk);      // find the ID field, then read or write
    void doVerify(const Clock& clk);
    void commitSector(const Clock& clk);
    void commitTrack(const Clock& clk);
    void finish(const Clock& clk);  // INTRQ, drop BUSY, go Idle
    void advance(const Clock& clk, bool access);  // poll() and touch(): see stepPending()

    bool findSector(FloppyDrive::SectorId& out);  // an ID matching TR + SR

    // Can the chip, clocked as it is, see this ID field at all? See setDensityChecked().
    bool legible(const FloppyDrive::SectorId& id) const {
        return !densityChecked_ || id.doubleDensity == doubleDensity;
    }
    void stepOnce();
    bool ready() const { return drive_ && drive_->ready(); }

    std::string  name_;
    FloppyDrive* drive_ = nullptr;

    // The register file.
    uint8_t command_ = 0;
    uint8_t status_  = 0;
    uint8_t track_   = 0;
    uint8_t sector_  = 0;
    uint8_t data_    = 0;

    Ctx   ctx_   = Ctx::TypeI;
    Phase phase_ = Phase::Idle;

    bool intrq_ = false;
    bool drq_   = false;

    // Board straps (see setWaitSynced/setSide). Not part of the register file: a strap
    // set by the CARD, so they do not travel in serialize() -- the owning card re-applies
    // them after a restore, just as it does the format straps.
    bool waitSynced_     = false;
    bool densityChecked_ = false;
    int  side_           = 0;

    // Where the state machine is going next, and when.
    uint64_t due_ = 0;

    // The latest time the chip has lived through inside a held bus cycle (see
    // holdUntilReady()). Never serialized: at an instruction boundary the clock has already
    // been charged the hold, so ahead_ is never past it there.
    uint64_t ahead_ = 0;
    uint64_t now(const Clock& clk) const { return clk.now() > ahead_ ? clk.now() : ahead_; }

    // ---- THE STEPPING DIRECTION IS A LATCH ON THE REAL CHIP (the DIRC pin) ----
    // A bare Step command (not Step In / Step Out) repeats the LAST direction, so it
    // has to be remembered across commands. Default inward: after a Restore the head
    // is at track 0 and the only way it can go is in.
    bool dirIn_ = true;

    int stepsLeft_ = 0;  // Restore counts down from 255 -- past that it is a Seek Error

    // ---- HLD AND HLT ARE TWO DIFFERENT PINS, AND S5 IS THE *AND* OF THEM ----
    //
    // "S5 HEAD LOADED: when set, it indicates the head is loaded and engaged. This bit is
    // a logical 'and' of HLD and HLT signals."  HLD (pin 28) is the chip's output -- it
    // asks for the head. HLT (pin 23) is the drive's answer, and it is "sampled after
    // 10 msec". So there is a window, right after a head load, in which the head is
    // COMMANDED DOWN BUT NOT YET ENGAGED, and S5 reports 0 for the whole of it.
    //
    // A driver that waits on S5 to know the head has settled is reading a bit that must
    // be able to be FALSE, or it waits on nothing. So we keep the output pin (headLoaded_
    // = HLD) and the MOMENT IT WENT ACTIVE (hldAt_), and derive HLT from the clock -- no
    // extra phase, no extra deadline, and it stays right even if nobody polls us.
    //
    // ...and the settle is 10 ms at 2 MHz and TWENTY at 1 MHz ("these times doubled when
    // CLK = 1 MHz"), which is why it goes through headSettleTStates() and not a literal.
    bool     headLoaded_ = false;  // HLD, the output pin
    uint64_t hldAt_      = 0;      // ...and when it last went active, for HLT

    // WE DO NOT MODEL THE TWO-REVOLUTION AUTO-UNLOAD ("if the FD1771 does not receive
    // any commands after two revolutions of the disk, the head will be automatically
    // disengaged"). It is a power-saving detail with exactly one observable: S5 goes
    // low a third of a second after the drive goes quiet, and nothing reads S5 to find
    // that out. Modeling it would mean giving the chip a rotational clock it otherwise
    // has no use for. Said out loud rather than left as a silent divergence.

    // The transfer in flight.
    std::vector<uint8_t>  buf_;
    size_t                idx_  = 0;
    bool                  lost_ = false;
    Ending                end_  = Ending::Plain;
    FloppyDrive::SectorId id_{};

    // ---- FORCE INTERRUPT'S ARMED CONDITIONS (I0, I1, I2) ----
    // I3 is immediate and needs no state. The other three arm the chip to interrupt on
    // something that has not happened yet -- a drive coming ready, going not-ready, or
    // an index pulse -- and stay armed until the next command. Which means the pins
    // have to be SAMPLED for edges, and that is what the prev* pair is for.
    uint8_t fiConds_  = 0;
    bool    prevReady_ = false;
    bool    prevIndex_ = false;

    // Read Address returns "the next encountered ID field", which is a question about
    // where the disk has got to. WE DO NOT MODEL ANGULAR POSITION -- so successive Read
    // Addresses walk the track in rotational order and wrap, which is what a real one
    // does and what every formatter that uses the command actually depends on. It does
    // not pretend to tell you WHICH sector is under the head at T=now, because nothing
    // in this program knows that.
    int raCursor_ = 0;

    std::vector<std::string> log_;
};

// ---------------------------------------------------------------------------
// FD1771 -- single density (FM), the first of the family. Four data address marks;
// no side-select pin. This is the VersaFloppy I's chip, and the Tarbell SD card's.
// ---------------------------------------------------------------------------
class Wd1771 : public Wd17xx {
public:
    explicit Wd1771(std::string name) : Wd17xx(std::move(name)) {}

protected:
    // Table 1: 6/6/10/20 ms at 2 MHz, doubled at 1 MHz. NOT the WD1770's 6/12/20/30 --
    // see the file-level note; that is a different chip and copying its table seeks at
    // the wrong speed while looking correct.
    const int* stepRatesMs(bool twoMHz) const override {
        static const int k2[4] = {6, 6, 10, 20};
        static const int k1[4] = {12, 12, 20, 40};
        return twoMHz ? k2 : k1;
    }

    // No side-select pin: Read Address byte 2 is a hard zero (see the ID-field note in
    // afterHeadSettle). The 1771 is single-sided as far as the silicon is concerned.
    uint8_t readAddressSide() const override { return 0; }

    // FOUR data address marks, so the record type is TWO bits: FB (normal) = 00,
    // F8 (deleted) = 11. This is the deepest difference from a 179x.
    uint8_t deletedRecordBits() const override { return kRecType6 | kRecType5; }

    // a1a0 = 11 selects the Deleted Data Mark (Table 4).
    bool cmdWritesDeletedMark(uint8_t cmd) const override { return (cmd & 0x03) == 0x03; }
};

// ---------------------------------------------------------------------------
// FD1791 -- single AND double density (FM/MFM). Two data address marks; a side-select
// pin. This is the VersaFloppy II's chip, and the Tarbell DD card's. The register file
// and command set are the 1771's; only the four hooks below differ.
// ---------------------------------------------------------------------------
class Wd1791 : public Wd17xx {
public:
    explicit Wd1791(std::string name) : Wd17xx(std::move(name)) {}

protected:
    // The 179x step-rate column (Table 1, 179x data sheet): 3/6/10/15 ms at 2 MHz,
    // doubled at 1 MHz. Faster at the top of the range than the 1771's 6/6/10/20.
    const int* stepRatesMs(bool twoMHz) const override {
        static const int k2[4] = {3, 6, 10, 15};
        static const int k1[4] = {6, 12, 20, 30};
        return twoMHz ? k2 : k1;
    }

    // The 179x has a side-select input; Read Address byte 2 carries the head number the
    // board selected (setSide()).
    uint8_t readAddressSide() const override { return (uint8_t)side_; }

    // TWO data address marks: the record type is ONE bit, S5 (F8 deleted = 1).
    uint8_t deletedRecordBits() const override { return kRecType5; }

    // A single a0 bit selects the Deleted Data Mark on a Write.
    bool cmdWritesDeletedMark(uint8_t cmd) const override { return (cmd & 0x01) != 0; }
};

} // namespace altair
