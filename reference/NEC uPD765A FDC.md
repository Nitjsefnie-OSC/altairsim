# NEC µPD765A — Single/Double Density Floppy Disk Controller

Source: [765.pdf](#)

NEC Electronics U.S.A., Microcomputer Division, data sheet Rev/3 (footer
`765ADS-REV3-1-82-CAT`, January 1982), 19 pages, printed pages 459–477 of an NEC catalog
(PDF page *n* = printed page *n* + 458). The µPD765A is a 40-pin floppy disk controller for up to
four drives, IBM 3740 single density (FM) and IBM System 34 double density (MFM), single or
double sided, 128 to 8192 bytes per sector, with DMA or non-DMA data transfer. The **Intel
8272** is the second source of the same chip; the CompuPro Disk 1 manual reprints Intel's 8272
data sheet (see [CompuPro Disk 1 / Disk 1A](CompuPro%20Disk%201%20%26%201A%20Floppy%20Controllers.md)).

This file holds what an emulator needs: the two registers, the 15 commands with their byte
layouts, the four status registers, the multi-sector and multi-track rules, the interrupt
rules and the timer encodings. It omits the DC/AC electrical tables, the FDD-side waveforms,
the data separator and write precompensation, and the package outlines.

Emulated as `Upd765` (`src/chips/upd765.{h,cpp}`), the chip of the CompuPro Disk 1A board `disk1a`.

---

## 1. Quick reference for emulation

| Item | Value |
|------|-------|
| Registers | Two, selected by **A0**: A0=0 Main Status Register (read only), A0=1 Data Register (read and write) |
| Data Register | A stack; only one byte is on the bus at a time. All command bytes, parameters, data (non-DMA) and result bytes pass through it |
| Command flow | Command phase (processor writes bytes) → Execution phase (FDC works) → Result phase (processor reads bytes) |
| Handshake | Before each command or result byte, read MSR and wait for **RQM=1** with **DIO** in the right direction (DIO=0 write, DIO=1 read) |
| Clock | 8 MHz single-phase (125 ns) for 8″. **4 MHz for minifloppy: every timer interval doubles** |
| Drives | 4, selected by US0/US1. Head by HD |
| Formats | FM: 128/256/512/1024/2048/4096 B sectors (N=00–05). MFM: 256–8192 B (N=01–06). **MFM with N=00 is not possible** |
| Data path | DMA (DRQ/DACK/TC) or non-DMA (ND=1 in Specify: an interrupt per byte) |
| Byte service time | **27 µs FM, 13 µs MFM** (8″). Late service sets OR (overrun) and ends the command |
| Interrupt | INT pin, active high. Cleared by reading the first result byte, or by Sense Interrupt Status |
| Invalid command | One result byte: **ST0 = 80H**. No interrupt |
| Reset | Idle state, FDD outputs to 0. **SRT, HUT and HLT are kept** |

---

## 2. Registers (pp. 464–465)

**Register select** (p. 464):

| A0 | RD | WR | Function |
|----|----|----|----------|
| 0 | 0 | 1 | Read Main Status Register |
| 0 | 1 | 0 | Illegal |
| 0 | 0 | 0 | Illegal |
| 1 | 0 | 0 | Illegal |
| 1 | 0 | 1 | Read from Data Register |
| 1 | 1 | 0 | Write into Data Register |

**Main Status Register** (p. 465):

| Bit | Name | Symbol | Meaning |
|-----|------|--------|---------|
| DB0 | FDD 0 Busy | D0B | Drive 0 is in Seek mode. If any of D0B–D3B is set, the FDC will not accept a read or write command |
| DB1 | FDD 1 Busy | D1B | Drive 1 is in Seek mode |
| DB2 | FDD 2 Busy | D2B | Drive 2 is in Seek mode |
| DB3 | FDD 3 Busy | D3B | Drive 3 is in Seek mode |
| DB4 | FDC Busy | CB | A read or write command is in process. The FDC will not accept any other command |
| DB5 | Execution Mode | EXM | Set only during the execution phase in **non-DMA** mode. Goes low when the execution phase ends and the result phase starts |
| DB6 | Data Input/Output | DIO | 1 = Data Register to processor. 0 = processor to Data Register |
| DB7 | Request for Master | RQM | The Data Register is ready to send or receive a byte |

DIO and RQM together give the handshake. The data sheet says to wait 12 µs after a data byte
before reading MSR again. The last RD of the result phase drops CB within 12 µs. An emulator
can make both changes at once.

---

## 3. Command set (pp. 466–467)

Byte 1 is the command code. MT = multi-track, MF = MFM (1) / FM (0), SK = skip deleted data
address marks. X = don't care, "usually 0". Byte 2 is `X X X X X HD US1 US0` unless the table
says otherwise. A0 = 1 for all of it.

| Command | Byte 1 (D7…D0) | Code with flags clear | Command bytes after byte 1 | Result bytes |
|---------|----------------|-----------------------|----------------------------|--------------|
| Read Data | MT MF SK 0 0 1 1 0 | 06H | HD/US, C, H, R, N, EOT, GPL, DTL | ST0 ST1 ST2 C H R N |
| Read Deleted Data | MT MF SK 0 1 1 0 0 | 0CH | HD/US, C, H, R, N, EOT, GPL, DTL | ST0 ST1 ST2 C H R N |
| Write Data | MT MF 0 0 0 1 0 1 | 05H | HD/US, C, H, R, N, EOT, GPL, DTL | ST0 ST1 ST2 C H R N |
| Write Deleted Data | MT MF 0 0 1 0 0 1 | 09H | HD/US, C, H, R, N, EOT, GPL, DTL | ST0 ST1 ST2 C H R N |
| Read a Track | 0 MF SK 0 0 0 1 0 | 02H | HD/US, C, H, R, N, EOT, GPL, DTL | ST0 ST1 ST2 C H R N |
| Read ID | 0 MF 0 0 1 0 1 0 | 0AH | HD/US | ST0 ST1 ST2 C H R N |
| Format a Track | 0 MF 0 0 1 1 0 1 | 0DH | HD/US, N, SC, GPL, D | ST0 ST1 ST2 C H R N (C/H/R/N "have no meaning") |
| Scan Equal | MT MF SK 1 0 0 0 1 | 11H | HD/US, C, H, R, N, EOT, GPL, STP | ST0 ST1 ST2 C H R N |
| Scan Low or Equal | MT MF SK 1 1 0 0 1 | 19H | HD/US, C, H, R, N, EOT, GPL, STP | ST0 ST1 ST2 C H R N |
| Scan High or Equal | MT MF SK 1 1 1 0 1 | 1DH | HD/US, C, H, R, N, EOT, GPL, STP | ST0 ST1 ST2 C H R N |
| Recalibrate | 0 0 0 0 0 1 1 1 | 07H | `X X X X X 0 US1 US0` | none |
| Sense Interrupt Status | 0 0 0 0 1 0 0 0 | 08H | none | ST0 PCN |
| Specify | 0 0 0 0 0 0 1 1 | 03H | `SRT(4) HUT(4)`, `HLT(7) ND(1)` | none |
| Sense Drive Status | 0 0 0 0 0 1 0 0 | 04H | HD/US | ST3 |
| Seek | 0 0 0 0 1 1 1 1 | 0FH | HD/US, NCN | none |
| Invalid | any other code | — | none | ST0 = 80H |

**Symbols** (pp. 467–468):

| Symbol | Meaning |
|--------|---------|
| C | Cylinder number, 0–76 on the medium |
| H | Head address (0 or 1) as in the ID field |
| HD | Selected head; drives the HD pin. "H = HD in all command words" |
| R | Record: the sector number to read or write |
| N | Bytes per sector code (128 × 2^N) |
| EOT | End of track: the final sector number on the cylinder |
| GPL | Gap 3 length. In read/write, how long the VCO stays low after the two CRC bytes; in Format, the gap written |
| DTL | Data length. Used **only when N = 0**: the number of bytes to transfer from each sector. When N ≠ 0, DTL "has no meaning and should be set to FF" |
| SC | Sectors per cylinder (Format) |
| D | Filler byte written to every data field (Format) |
| STP | Scan step: 1 = contiguous sectors, 2 = alternate sectors |
| NCN | New cylinder number (Seek) |
| PCN | Present cylinder number, returned by Sense Interrupt Status |
| ND | Non-DMA mode (Specify) |
| SRT, HUT, HLT | Step rate, head unload and head load times (Specify) |

**Marginalia:** a previous owner wrote `DF` and `28` beside the Specify entry (p. 467). `DF` is
the second Specify byte that CompuPro's Disk 1A boot ROM source (`ROM1.ASM`) sends.

---

## 4. Specify: timer encodings (p. 474, p. 468)

| Field | Bits | Range at 8 MHz | Encoding |
|-------|------|----------------|----------|
| SRT | byte 2, D7–D4 | 1–16 ms in 1 ms steps | F = 1 ms, E = 2 ms, D = 3 ms … (time = 16 − SRT ms). Applies to all drives |
| HUT | byte 2, D3–D0 | 16–240 ms in 16 ms steps | 01 = 16 ms, 02 = 32 ms … 0F = 240 ms |
| HLT | byte 3, D7–D1 | 2–254 ms in 2 ms steps | 01 = 2 ms, 02 = 4 ms, 03 = 6 ms … 7F = 254 ms |
| ND | byte 3, D0 | — | 1 = non-DMA mode, 0 = DMA mode |

All intervals are proportional to CLK. At 4 MHz (minifloppy) every interval doubles.

- ⚠ The data sheet does not give the meaning of SRT = 0, HUT = 0 or HLT = 0. The formula for
  SRT gives 16 ms; nothing is stated for the other two.
- The Seek section adds a footnote: if writing the three Seek bytes takes more than 150 µs,
  the first two step pulses can be up to 1 ms closer than SRT.

---

## 5. Status registers (pp. 475–476)

**ST0**

| Bit | Symbol | Meaning |
|-----|--------|---------|
| D7–D6 | IC | Interrupt code: **00** normal termination (NT). **01** abnormal termination (AT): the command started but did not complete. **10** invalid command (IC): never started. **11** abnormal termination because the Ready signal changed state during execution |
| D5 | SE | Seek End: set when the FDC completes a Seek (and Recalibrate) |
| D4 | EC | Equipment Check: a Fault signal from the FDD, or Track 0 not found after 77 step pulses (Recalibrate) |
| D3 | NR | Not Ready: the FDD is not ready and a read or write command is issued. **Also set when a read or write command is issued to side 1 of a single-sided drive** |
| D2 | HD | State of the head at interrupt |
| D1–D0 | US1, US0 | Drive unit at interrupt |

**ST1**

| Bit | Symbol | Meaning |
|-----|--------|---------|
| D7 | EN | End of Cylinder: the FDC tried to access a sector beyond the final sector of a cylinder |
| D6 | — | Not used, always 0 |
| D5 | DE | Data Error: CRC error in the ID field or the data field |
| D4 | OR | Over Run: the processor did not service a data transfer in time |
| D3 | — | Not used, always 0 |
| D2 | ND | No Data: (a) Read Data, Write Deleted Data or Scan cannot find the sector given in the IDR; (b) Read ID cannot read an ID field without error; (c) Read a Track cannot find the starting sector |
| D1 | NW | Not Writable: write protect signal during Write Data, Write Deleted Data or Format |
| D0 | MA | Missing Address Mark: no ID address mark after the index hole twice. Also set, with ST2 MD, when no data address mark or deleted data address mark is found |

**ST2**

| Bit | Symbol | Meaning |
|-----|--------|---------|
| D7 | — | Not used, always 0 |
| D6 | CM | Control Mark: Read Data or Scan met a sector with a deleted data address mark (and, by §7, Read Deleted Data met a normal one) |
| D5 | DD | Data Error in Data Field: CRC error in the data field |
| D4 | WC | Wrong Cylinder: with ND, the C on the medium differs from the IDR |
| D3 | SH | Scan Equal Hit: the "equal" condition was met during a Scan |
| D2 | SN | Scan Not Satisfied: no sector on the cylinder met the Scan condition |
| D1 | BC | Bad Cylinder: with ND, the C on the medium differs from the IDR **and is FF** |
| D0 | MD | Missing Address Mark in Data Field: no data address mark or deleted data address mark |

**ST3** (Sense Drive Status)

| Bit | Symbol | Meaning |
|-----|--------|---------|
| D7 | FT | Fault signal from the FDD |
| D6 | WP | Write Protected signal from the FDD |
| D5 | RY | Ready signal from the FDD |
| D4 | T0 | Track 0 signal from the FDD |
| D3 | TS | Two Side signal from the FDD |
| D2 | HD | Side select signal to the FDD |
| D1 | US1 | Unit select 1 to the FDD |
| D0 | US0 | Unit select 0 to the FDD |

---

## 6. Data transfer and interrupts (p. 469, p. 474)

- **Command and result phases:** the processor must read MSR before each byte (RQM and DIO).
  It need not read MSR during the execution phase.
- **Non-DMA mode** (ND=1): each data byte raises INT (and RQM). The RD or WR of the Data
  Register clears it. A processor that cannot take interrupts that fast may poll MSR, and RQM
  acts as the interrupt signal. EXM is 1 during execution.
- **DMA mode** (ND=0): no interrupts during execution. The FDC raises **DRQ** for each byte.
  The DMA controller answers with **DACK=0** and **RD=0** (read from FDC) or **WR=0** (write
  to FDC). DACK=0 clears DRQ.
- **End of execution:** when TC arrives or the EOT sector has been read or written, INT=1
  and the result phase starts. **Reading the first result byte clears INT.**
- **All result bytes must be read.** The FDC does not accept a new command until it has
  given the last one (Read Data has seven).
- **No short cuts:** the command bytes must all be sent, in table order; nothing can be left
  out. The execution phase starts after the last command byte. The command ends after the
  last result byte.

**Interrupt sources** (p. 474):

1. The start of the result phase of Read Data, Read a Track, Read ID, Read Deleted Data,
   Write Data, Format a Cylinder, Write Deleted Data and the Scan commands.
2. The Ready line of an FDD changes state.
3. The end of a Seek or Recalibrate.
4. Each byte during the execution phase in non-DMA mode.

Sources 1 and 4 need no Sense Interrupt Status. Sources 2 and 3 are told apart by **Sense
Interrupt Status**, which also clears INT. Its ST0 bits 5–7 (Table 5):

| SE (bit 5) | IC bit 6 | IC bit 7 | Cause |
|------------|----------|----------|-------|
| 0 | 1 | 1 | Ready line changed state, either polarity |
| 1 | 0 | 0 | Normal termination of Seek or Recalibrate |
| 1 | 1 | 0 | Abnormal termination of Seek or Recalibrate |

- Seek and Recalibrate have **no result phase**. Sense Interrupt Status must be used after
  them to end the command and get the PCN.
- **Sense Interrupt Status with no interrupt pending is an invalid command** (ST0 = 80H).
- **A Sense Interrupt Status must follow a Seek or Recalibrate interrupt. If it does not, the
  FDC treats the next command as invalid** (p. 474, Invalid).

**Reset pin** (p. 461): "If RDY pin is held high during Reset, FDC will generate an interrupt
within 1.024 ms. To clear this interrupt use Sense Interrupt Status command."

**Polling** (p. 469): after Specify, the FDC polls all four drives through US0/US1 between
commands (and between step pulses of a Seek) for a change of the Ready line. A change raises
an interrupt; ST0 then shows NR. Each drive is polled every 1.024 ms, except during
read/write commands.

---

## 7. The commands (pp. 470–474)

### Read Data

Nine command bytes. The FDC loads the head (if it is unloaded), waits the head load time,
then reads ID fields. When the R in the IDR matches the sector number read, it sends the
data field byte by byte.

- **Multi-sector:** after each sector, R is incremented by one and the next sector is read,
  until TC. On TC the FDC stops sending data but finishes reading the current sector and
  checks its CRC, then ends.
- **Multi-track (MT=1):** the transfer runs from sector 1 of side 0 to sector L (the last
  sector) of side 1. It stays on one cylinder.
- **N = 0:** DTL bytes are sent from each sector; the FDC still reads the whole sector and
  checks its CRC. N ≠ 0: DTL is ignored.
- The head is unloaded only after the head unload time. A new command inside that time skips
  the head load wait.
- **Sector not found:** index hole twice without finding R → ST1 **ND**, ST0 = 40H|…
  (abnormal termination).
- **CRC error:** in an ID field → ST1 DE. In a data field → ST1 DE and ST2 DD. The command
  ends (abnormal).
- **Deleted data address mark** with SK=0: ST2 **CM** is set, all data of the sector is
  read, then the command ends. With SK=1: the sector is skipped and the next one read. The
  CRC of the skipped data field is not checked.
- **Overrun:** a byte not serviced within 27 µs (FM) / 13 µs (MFM) → ST1 OR, abnormal end.

**Table 1, transfer capacity** (p. 470):

| MT | MF | N | Maximum transfer (bytes/sector × sectors) | Final sector read |
|----|----|---|------------------------------------------|-------------------|
| 0 | 0 | 00 | 128 × 26 = 3,328 | 26 at side 0 or 26 at side 1 |
| 0 | 1 | 01 | 256 × 26 = 6,656 | 26 at side 0 or 26 at side 1 |
| 1 | 0 | 00 | 128 × 52 = 6,656 | 26 at side 1 |
| 1 | 1 | 01 | 256 × 52 = 13,312 | 26 at side 1 |
| 0 | 0 | 01 | 256 × 15 = 3,840 | 15 at side 0 or 15 at side 1 |
| 0 | 1 | 02 | 512 × 15 = 7,680 | 15 at side 0 or 15 at side 1 |
| 1 | 0 | 01 | 256 × 30 = 7,680 | 15 at side 1 |
| 1 | 1 | 02 | 512 × 30 = 15,360 | 15 at side 1 |
| 0 | 0 | 02 | 512 × 8 = 4,096 | 8 at side 0 or 8 at side 1 |
| 0 | 1 | 03 | 1024 × 8 = 8,192 | 8 at side 0 or 8 at side 1 |
| 1 | 0 | 02 | 512 × 16 = 8,192 | 8 at side 1 |
| 1 | 1 | 03 | 1024 × 16 = 16,384 | 8 at side 1 |

**Table 2, ID information in the result phase** (p. 471), when the processor ends the command
(with TC):

| MT | HD | Final sector transferred | C | H | R | N |
|----|----|--------------------------|---|---|---|---|
| 0 | 0 | Less than EOT | NC | NC | R + 1 | NC |
| 0 | 0 | Equal to EOT | C + 1 | NC | R = 01 | NC |
| 0 | 1 | Less than EOT | NC | NC | R + 1 | NC |
| 0 | 1 | Equal to EOT | C + 1 | NC | R = 01 | NC |
| 1 | 0 | Less than EOT | NC | NC | R + 1 | NC |
| 1 | 0 | Equal to EOT | NC | LSB | R = 01 | NC |
| 1 | 1 | Less than EOT | NC | NC | R + 1 | NC |
| 1 | 1 | Equal to EOT | C + 1 | LSB | R = 01 | NC |

NC = no change from the start of execution. LSB = the least significant bit of H is
complemented.

⚠ The data sheet does not state the result when the FDC itself runs past EOT without a TC.
ST1 **EN** ("tried to access a sector beyond the final sector of a cylinder") is the flag for
it. CompuPro's boot ROM treats ST0 = 40H with ST1 EN as a successful read (it reads a whole
track without TC), which is the period evidence for that case.

### Write Data

Nine command bytes. The FDC loads the head, waits the head settling time, and reads ID fields.
When C, H, R and N all match the ID field, it takes data from the processor byte by byte and
writes the data field.

- R increments after each sector, and the next data field is written ("Multi-Sector Write"),
  until TC.
- **TC in the middle of a sector:** the FDC finishes the data field and **fills the rest of
  it with 00**.
- The FDC reads each ID field's CRC. A CRC error there → ST1 DE, abnormal end.
- The same as Read Data: transfer capacity, EN, ND, head unload, the result ID (Table 2), DTL
  when N = 0.
- A byte not received within 27 µs (FM) / 13 µs (MFM) → ST1 OR, abnormal end.
- Write protect → ST1 NW (§5).

### Write Deleted Data

The same as Write Data, except that it writes a deleted data address mark at the start of the
data field.

### Read Deleted Data

The same as Read Data, except that a **normal** data address mark with SK=0 reads all of that
sector, sets ST2 CM and ends the command. SK=1 skips the sector with the normal data address
mark and reads the next one.

### Read a Track

Like Read Data, but a continuous read: after the index hole, all data fields on the track are
read as continuous blocks of data. ID and data CRC errors do not stop it.

- The FDC compares the ID read from each sector with the IDR, and sets ST1 ND if there is no
  match. It goes on reading.
- It ends when the number of sectors read equals EOT.
- No ID address mark before the index hole comes round a second time → ST1 MA, abnormal end.
- ⚠ **SK: the text and the command table disagree.** The text says "multi-track or skip
  operations are not allowed with this command". The command table (p. 466) shows byte 1 as
  `0 MF SK 0 0 0 1 0`, with an SK bit. MT is 0 in both. Unresolved by this data sheet; the
  Intel 8272 reprint in the CompuPro Disk 1 manual is the next source to check.

### Read ID

Returns the present head position. The FDC stores the values from the first ID field it can
read. If it finds no proper ID address mark before the index hole comes round a second time,
it sets ST1 MA, and if it can read no data, ST1 ND; ST0 = 40H|… (abnormal). There is no data
transfer during the execution phase.

### Format a Track

After the index hole, the FDC writes the whole track: gaps, address marks, ID fields and data
fields, in IBM System 34 (MFM) or System 3740 (FM) format.

- N, SC, GPL and D come from the command bytes. D fills every data field.
- **The ID fields come from the processor:** for each sector it supplies **C, H, R and N**
  (four bytes). In DMA mode that is **4 DMA requests per sector**; in non-DMA mode, 4
  interrupts per sector. This allows non-sequential sector numbers.
- R is incremented by one after each sector is formatted, so in the result phase R holds the
  last sector number + 1. The ID result bytes "have no meaning".
- It continues until the FDC meets the index hole a second time.
- A Fault signal at the end of a write → ST0 EC, abnormal end. Loss of READY at the start of
  the execution phase → ST0 NR, abnormal end.
- Write protect → ST1 NW (§5).

**Table 3, N, SC and GPL** (p. 472, all values hexadecimal). GPL ① = suggested gap for
read/write commands; GPL ② = suggested gap for Format.

8″ standard floppy:

| Mode | Sector size | N | SC | GPL ① | GPL ② |
|------|-------------|---|----|-------|-------|
| FM | 128 | 00 | 1A | 07 | 1B |
| FM | 256 | 01 | 0F | 0E | 2A |
| FM | 512 | 02 | 08 | 1B | 3A |
| FM | 1024 | 03 | 04 | 47 | 8A |
| FM | 2048 | 04 | 02 | C8 | FF |
| FM | 4096 | 05 | 01 | C8 | FF |
| MFM | 256 | 01 | 1A | 0E | 36 |
| MFM | 512 | 02 | 0F | 1B | 54 |
| MFM | 1024 | 03 | 08 | 35 | 74 |
| MFM | 2048 | 04 | 04 | 99 | FF |
| MFM | 4096 | 05 | 02 | C8 | FF |
| MFM | 8192 | 06 | 01 | C8 | FF |

5¼″ minifloppy:

| Mode | Sector size | N | SC | GPL ① | GPL ② |
|------|-------------|---|----|-------|-------|
| FM | 128 | 00 | 12 | 07 | 09 |
| FM | 128 | 00 | 10 | 10 | 19 |
| FM | 256 | 01 | 08 | 18 | 30 |
| FM | 512 | 02 | 04 | 46 | 87 |
| FM | 1024 | 03 | 02 | C8 | FF |
| FM | 2048 | 04 | 01 | C8 | FF |
| MFM | 256 | 01 | 12 | 0A | 0C |
| MFM | 256 | 01 | 10 | 20 | 32 |
| MFM | 512 | 02 | 08 | 2A | 50 |
| MFM | 1024 | 03 | 04 | 80 | F0 |
| MFM | 2048 | 04 | 02 | C8 | FF |
| MFM | 4096 | 05 | 01 | C8 | FF |

Note ③: "In MFM mode FDC can not perform a read/write/format operation with 128 bytes/sector
(N = 00)."

### Scan Equal, Scan Low or Equal, Scan High or Equal

The data read from the disk is compared, byte by byte, with data from the processor (or DMA),
looking for a sector that meets D_FDD = D_Processor, D_FDD ≤ D_Processor or D_FDD ≥
D_Processor. **A processor byte of FF is a mask**: it matches any disk byte. The comparison
uses "ones complement arithmetic … (FF = largest number, 00 = smallest number)".

- After each sector that does not meet the condition, R = R + STP and the scan goes on.
- The scan ends when the condition is met, when the last sector on the track (EOT) is
  reached, or on TC.
- Condition met → ST2 **SH**, end. No sector met it up to EOT → ST2 **SN**, end.
- TC during a scan: the FDC completes the comparison of the byte in process, then ends.

**Table 4** (p. 473):

| Command | SN (ST2 bit 2) | SH (ST2 bit 3) | Meaning |
|---------|----------------|----------------|---------|
| Scan Equal | 0 | 1 | D_FDD = D_Processor |
| Scan Equal | 1 | 0 | D_FDD ≠ D_Processor |
| Scan Low or Equal | 0 | 1 | D_FDD = D_Processor |
| Scan Low or Equal | 0 | 0 | D_FDD < D_Processor |
| Scan Low or Equal | 1 | 0 | D_FDD > D_Processor |
| Scan High or Equal | 0 | 1 | D_FDD = D_Processor |
| Scan High or Equal | 0 | 0 | D_FDD > D_Processor |
| Scan High or Equal | 1 | 0 | D_FDD < D_Processor |

- **Deleted data address mark:** with SK=0, the sector is taken as the last sector on the
  cylinder: ST2 CM is set and the command ends. With SK=1 the sector is skipped, ST2 CM is set
  to show a deleted sector was met, and the scan goes on.
- **STP and EOT:** the last sector on the track must be read. With STP=02 and MT=0 on sectors
  1–26, a scan that starts at sector 21 reads 21, 23 and 25; sector 27 is never found, the
  index hole comes round, and the command ends abnormally. Started at sector 20 it ends
  normally.
- The data for the comparison must arrive in under 27 µs (FM) / 13 µs (MFM), or ST1 OR is
  set and the command ends (ST0 bits 7–6 = 01).

### Seek

- The FDC has **four independent PCN registers**, one per drive. They are cleared only by
  Recalibrate.
- It compares PCN with NCN. PCN < NCN: Direction = 1 (step in). PCN > NCN: Direction = 0
  (step out). Step pulses are issued at the SRT rate. After each pulse NCN is compared with
  PCN; when equal, ST0 **SE** is set and the FDC raises INT.
- **DB0–DB3 of the MSR** are set for the drive during the seek execution phase and cleared by
  Sense Interrupt Status.
- During the command phase the FDC is BUSY. **During the execution phase it is NOT BUSY**, and
  another Seek may be issued to another drive: parallel seeks on up to 4 drives. No other
  command may be issued while the FDC is sending step pulses to any drive.
- If the FDD is not ready at the start of the execution phase, or during the seek, ST0 NR is
  set and the command ends (ST0 bits 7–6 = 01).

### Recalibrate

Retracts the head to track 0. The FDC clears the PCN counter and checks the Track 0 signal. While
Track 0 is low, Direction stays 0 and step pulses are issued. When Track 0 goes high, ST0 SE is
set and the command ends. **If Track 0 is still low after 77 step pulses**, ST0 SE and **EC**
are both set and the command ends abnormally (bits 7–6 = 01). Overlap and not-ready behave as
for Seek.

⚠ Arithmetic, not stated in the data sheet: a head beyond cylinder 77 (an 80-track drive)
needs a second Recalibrate.

### Sense Interrupt Status

See §6. Result: ST0, then PCN.

### Specify

See §4. No result phase.

### Sense Drive Status

Returns ST3 for the selected drive. No interrupt.

### Invalid

Any code not in the table. The FDC ends the command with ST0 bits 7–6 = 10. No interrupt.
DIO and RQM are both 1, so the processor knows to read; ST0 reads **80H**. "In some
applications the user may wish to use this command as a No-Op command, to place the FDC in a
standby or no operation state."

---

## 8. Pins (pp. 459, 461)

| Pin | Symbol | Dir | Function |
|-----|--------|-----|----------|
| 1 | RST | In | Reset: idle state, FDD outputs to 0. Does not affect SRT, HUT, HLT |
| 2 | RD | In | Read (Data Register → bus). Disabled when CS = 1 |
| 3 | WR | In | Write. Disabled when CS = 1 |
| 4 | CS | In | Chip select (active low) |
| 5 | A0 | In | 0 = Status Register, 1 = Data Register |
| 6–13 | DB0–DB7 | I/O | Data bus |
| 14 | DRQ | Out | DMA request |
| 15 | DACK | In | DMA acknowledge (active low) |
| 16 | TC | In | Terminal count: ends a Read/Write/Scan command |
| 17 | IDX | In | Index |
| 18 | INT | Out | Interrupt request |
| 19 | CLK | In | Single-phase 8 MHz square wave |
| 21 | WCK | In | Write clock: 500 kHz FM, 1 MHz MFM (8″) |
| 22 | RDW | In | Read data window, from the PLL |
| 23 | RDD | In | Read data from the FDD |
| 24 | VCO | Out | VCO sync |
| 25 | WE | Out | Write enable |
| 26 | MFM | Out | MFM mode (1 = MFM, 0 = FM) |
| 27 | HD | Out | Head select (1 = head 1) |
| 28, 29 | US1, US0 | Out | Unit select |
| 30 | WDA | Out | Write data |
| 31, 32 | PS1, PS0 | Out | Precompensation (pre-shift) |
| 33 | FLT/TR0 | In | Fault (read/write mode), Track 0 (seek mode) |
| 34 | WP/TS | In | Write protect (read/write mode), Two Side (seek mode) |
| 35 | RDY | In | Ready |
| 36 | HDL | Out | Head load |
| 37 | FR/STP | Out | Fault reset (read/write mode), step (seek mode) |
| 38 | LCT/DIR | Out | Low current (read/write mode), direction (seek mode) |
| 39 | RW/SEEK | Out | 1 = Seek mode, 0 = Read/Write mode |
| 40 | Vcc | — | +5 V |

**Timing worth knowing:** clock period 120–500 ns (125 typ). **DRQ cycle time ≥ 13 µs.** TC
width 1 clock. Reset width 14 clocks. Index pulse width 10 clocks. Step pulse 6–7 µs; step
cycle 1–16 ms at 8 MHz (2–32 ms at 4 MHz) "under software control". "For mini-floppy
applications, ΦCY must be 4 MHz." (p. 462)

---

## Quirks worth carrying

- ⚠ **A Sense Interrupt Status must follow every Seek/Recalibrate interrupt**, or the next
  command is treated as invalid. **Sense Interrupt Status with nothing pending is itself
  invalid** (ST0 = 80H).
- ⚠ **Seek and Recalibrate have no result phase.** The drive's MSR busy bit (DB0–DB3) stays
  set until Sense Interrupt Status.
- ⚠ **Parallel seeks:** the FDC is not busy during a seek's execution phase. A read or write
  is still refused while any DB0–DB3 bit is set.
- ⚠ **ST0 NR on side 1 of a single-sided drive.** A read or write with HD=1 to a drive whose
  Two Side signal is low is "not ready", not "no data".
- ⚠ **MT stays on one cylinder.** At the end of side 1 the result C is incremented and the
  command ends; it never steps.
- ⚠ **TC mid-sector on a write fills the rest of the sector with 00.** On a read, the sector
  is finished and CRC-checked, but no more bytes go to the processor.
- ⚠ **MFM cannot do N = 00.** 128-byte sectors are FM only.
- ⚠ **DTL matters only when N = 0**; otherwise set it to FF.
- ⚠ **Format takes C/H/R/N per sector through the data path** (4 DMA requests per sector).
- ⚠ **Reset keeps SRT/HUT/HLT.** The data sheet is silent on ND across a reset.
- ⚠ **Reset with RDY high raises an interrupt** within 1.024 ms (the ready-change poll),
  cleared by Sense Interrupt Status. Software after a reset commonly issues Sense Interrupt
  Status once per drive.
- ⚠ **Read a Track and SK:** the text forbids skip, the command table encodes an SK bit
  (§7, unresolved).
- ⚠ **Running past EOT without TC** is reported with ST1 EN and ST0 abnormal (40H); CompuPro's
  boot ROM counts that as success.
- ⚠ **4 MHz clock doubles every timer** (minifloppy). The data rate (WCK) is set outside the
  chip by the board.
