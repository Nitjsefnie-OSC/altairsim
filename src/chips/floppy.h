#pragma once
//
// FloppyDrive -- the drive on the far end of a floppy controller CHIP's disk pins.
//
// The seam is the chip's, not one chip's: the FD177x family (chips/wd17xx.h) was the
// first to use it, and the pin numbers cited below are the FD1771's because that is
// the part it was written against. A boards/floppy-drive.h DiskImageDrive implements
// it for a mounted DiskImage, and any controller chip that talks to a drive through
// STEP/DIRC/TR00/READY/IP and whole sectors can take one without the WD chip.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace altair {

// ---------------------------------------------------------------------------
// WHAT IS ON THE OTHER END OF THE DISK PINS.
//
// The same seam ByteStream is for the 6850: the chip owns no image, no file and no
// geometry. It has pins -- STEP, DIRC, TR00, WPRT, READY, IP -- and a drive on the
// far end of them, and this is that drive.
//
// NOTE WHAT IS *NOT* HERE, because it is the whole chip/board split in one absence:
//
//   - NO DRIVE SELECT. The FD1771 has no drive-select pin. It talks to ONE drive,
//     and the CARD's select latch decides which. So the board calls attach() and
//     the chip is none the wiser -- which is exactly what the silicon does.
//   - NO SIDE SELECT. Same reason: the 1771 has no such pin either.
//   - NO MOTOR CONTROL. That is a WD1770 thing, and this is not one.
//
// THE HEAD POSITION LIVES HERE, NOT IN THE TRACK REGISTER. This is the distinction
// the whole seek/verify mechanism is built on and it is worth being blunt about: the
// chip's Track Register is SOFTWARE'S BOOKKEEPING -- a number the guest wrote, or
// that the chip counted up as it pulsed STEP. Where the head physically IS is the
// drive's business and only the drive's. They are allowed to disagree, and when they
// do, a verify sets Seek Error (S4). Fold them into one variable and Seek Error can
// never happen, which means a driver's error recovery is never exercised, and the
// first real disk with a bad ID field silently reads the wrong track.
// ---------------------------------------------------------------------------
class FloppyDrive {
public:
    virtual ~FloppyDrive() = default;

    // ---- The input pins, straight ----
    virtual bool ready() const = 0;           // READY (pin 32): is there a disk, is it spinning
    virtual bool writeProtected() const = 0;  // WRPT* (pin 36)
    virtual bool trackZero() const = 0;       // TR00* (pin 34): the head is over track 0

    // IP (pin 35): the index hole is under the sensor RIGHT NOW. A pulse, not a level
    // -- it is true for a sliver of each revolution. The chip needs it for two things
    // and cannot synthesize either: Type I status bit S1 IS this pin, and a Force
    // Interrupt on I2 fires on its edge. It belongs to the DRIVE because it is the
    // disk that is turning; the chip has no idea how fast, and no pin that tells it.
    virtual bool index() const = 0;

    // WF (pin 33): the drive's own write electronics have faulted -- no write current
    // when the gate is open, that sort of thing. "When WG=1 and WF goes low, the current
    // Write command is terminated and the Write Fault status bit is set."
    //
    // NOT PURE. A drive backed by a host file has no write electronics TO fault, so the
    // honest default is "no fault" -- but the pin exists, S5 WRITE FAULT is a documented
    // status bit, and without the pin there is no way to ever produce it except by the
    // medium refusing the write. A real drive model can override it.
    virtual bool writeFault() const { return false; }

    // DINT (pin 37): "sampled whenever a Write Track command is received. If DINT=0, the
    // operation is terminated and the Write Protect status bit is set." A second, separate
    // interlock on formatting -- the board could strap it to forbid a format outright,
    // independently of the disk's own write-protect notch. Default: formatting is permitted.
    virtual bool diskInit() const { return true; }

    // ---- STEP (pin 15) and DIRC (pin 16), as one call, because they arrive together ----
    // Moves the HEAD. A step outward at track 0 goes nowhere -- the head is against
    // its stop, and the real drive just buzzes.
    virtual void step(bool inward) = 0;

    // Where the head actually is. For the drive's own use and for tests; the chip
    // reads it only to build an ID field. See the note above.
    virtual int headTrack() const = 0;

    // ---- WHAT IS WRITTEN ON THE TRACK UNDER THE HEAD ----
    //
    // The chip searches these for an ID field matching (Track Register, Sector
    // Register). It is a LIST AND NOT A LOOKUP on purpose: Read Address returns "the
    // next ID field encountered", which is a question about rotation, and a map keyed
    // by sector number could not answer it.
    // ---- ONE DELIBERATE DEVIATION, AND IT IS HERE ----
    //
    // On the real chip, how many bytes a Read or Write moves is decided by the COMMAND's
    // `b` flag together with the ID field's length code -- the Type II flow chart has a box
    // for it: "COMPUTE LENGTH FROM b FLAG". b=1 gives IBM sizes (128 << n); b=0 gives
    // non-IBM ones (16 x N, where a code of 0 means 256 groups, i.e. 4096 bytes).
    //
    // The two readings COLLIDE: length code 00 is 128 bytes under b=1 and 4096 under b=0.
    // The chip decides, and the disk cannot.
    //
    // THIS CHIP DOES NOT DO THAT ARITHMETIC. It moves `size` bytes, and the DRIVE is what
    // decoded them. Why: the drive is the only thing here that knows its own format, and a
    // raw sector image physically cannot serve a length the format did not record. Ask a
    // real controller for 4096 bytes out of a 128-byte sector and it reads on through the
    // gaps and the next eleven sectors and hands back the lot with a CRC error. There are
    // no gaps in a .DSK to read through -- so honouring `b` would mean INVENTING the
    // garbage, which is precisely what DESIGN.md 0.1 forbids.
    //
    // To be observable you need a driver whose `b` flag disagrees with the disk in its own
    // drive -- a driver that is already broken on real hardware. Written down rather than
    // left to be discovered. If a disk ever turns up that needs it, start here.
    struct SectorId {
        int  track      = 0;  // as RECORDED in the ID field -- which is the point of a verify
        int  sector     = 0;
        int  size       = 0;  // decoded length of the data field, in bytes

        // The length byte AS RECORDED, which is NOT derivable from `size` -- the same
        // 128-byte sector is code 0 on an IBM-format disk (128 << n) and code 8 on a
        // non-IBM one (16 x N). Read Address hands the raw byte to the guest, so the
        // raw byte has to survive the trip; a chip that re-encoded it from `size`
        // would quietly change what a formatter reads back.
        int  lengthCode = 0;

        bool deleted    = false;  // the data field carries an F8, not an FB

        // How the ID field is RECORDED: FM (single density) or MFM (double). A chip that
        // is clocked for the other encoding finds no address mark here at all -- see
        // Wd17xx::setDensityChecked().
        bool doubleDensity = false;
        bool idCrcOk    = true;   // a bad ID-field CRC. Real disks have them; images do not
        bool dataCrcOk  = true;   // ...and a bad data-field CRC
    };

    virtual int  sectorCount() const = 0;                        // ID fields on this track
    virtual bool sectorIdAt(int index, SectorId& out) const = 0;  // in ROTATIONAL order

    // The data field. `n` is IN/OUT, exactly as DiskImage has it: on entry the
    // capacity, on exit the bytes moved. A short read is a failure, not a truncation.
    virtual bool readData(const SectorId& id, uint8_t* buf, size_t* n) = 0;
    virtual bool writeData(const SectorId& id, const uint8_t* buf, size_t n) = 0;

    // ---- THE WHOLE TRACK, GAPS AND ADDRESS MARKS AND ALL (Read Track / Write Track) ----
    //
    // Everything above is a SECTOR interface, because that is what a Type II command
    // addresses. These two are not: they run index-to-index and they see the bytes the
    // gaps are made of. That is a real difference in kind, and it is why they are
    // separate calls rather than a `size` that happens to be big.
    //
    // NOT PURE, AND FALSE BY DEFAULT, because a drive backed by a raw .DSK genuinely
    // CANNOT produce one: the gaps and the address marks were never in the file. That
    // is not a limitation to paper over -- it is a fact about the medium, and the right
    // answer is to say so (Write Track sets WRITE FAULT, which is the bit the hardware
    // would set) rather than to hand a formatter 5,208 invented bytes that round-trip
    // into a disk nothing can read.
    //
    // WRITE TRACK IS THE ONE PLACE GEOMETRY IS ESTABLISHED, so the chip hands the format
    // virtuals what it is configured at: the DATA RATE (dataRateBits) to trackImageBytes, for
    // the revolution byte budget, and the DENSITY (doubleDensity, the DDEN pin) to
    // writeTrackImage, for the encoding the track is recorded in. Both differ per track (an
    // 8" SD track is ~5208 bytes / single density; a DD track ~10416 / double), and the chip
    // is the single source of truth for which. They are two values and not one because the
    // rate does not give the density: a 5.25" double-density track runs at the 8"
    // single-density rate. The drive stores neither. (readTrackImage does NOT format, so it
    // takes neither.)
    virtual int  trackImageBytes(long long dataRateBits) const { (void)dataRateBits; return 0; }
    virtual bool readTrackImage(std::vector<uint8_t>& out) { (void)out; return false; }
    virtual bool writeTrackImage(const std::vector<uint8_t>& in, bool doubleDensity) {
        (void)in; (void)doubleDensity; return false;
    }

    // ---- TWO SIDE (the µPD765A's TS input, pin 34 in seek mode) ----
    //
    // The 8" drive drives this line when a two-sided diskette is in it. The FD177x has
    // no such input, so only a chip that reads ST3 asks. False by default: a drive that
    // says nothing about its sides is single sided, and the 765 answers a read or write
    // to its side 1 with NOT READY, as the data sheet says it does.
    virtual bool twoSided() const { return false; }

    // ---- FORMAT FROM AN ID LIST (the µPD765A's Format a Track) ----
    //
    // The 765 does not take a raw track the way the FD177x's Write Track does. It writes
    // the gaps and the address marks itself, and takes only C/H/R/N for each sector from
    // the processor, plus one fill byte D for every data field. So the drive gets what the
    // chip got: the ID fields in the order they go round the track (`size` and
    // `doubleDensity` filled in by the chip from N and MF), and the fill byte.
    //
    // False by default, for the same reason as the whole-track calls above: a drive that
    // cannot record a format must say so, and the chip reports a fault.
    virtual bool formatTrack(const std::vector<SectorId>& ids, uint8_t fill) {
        (void)ids; (void)fill; return false;
    }
};

} // namespace altair
