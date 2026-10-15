# CompuPro CP/M 2.2 — Technical Manual & Installation Procedures

Source: [CPM_2.2_Technical_Manual_and_Installation_Oct84.pdf](#) (October 1984 printing),
[CompuPro CPM-22 Technical Manual.pdf](#) (October 1983 printing),
[Software_Alteration_Guide_May84.pdf](#) (May 1984 alteration guides)

CompuPro (Hayward, CA), Document #15299, file name `CPM8022.MAN`. Two printings of one
document:

| Printing | © | PDF | Numbered pages | Text layer |
|---|---|---|---|---|
| October 1984 (back cover `15299/4`) | 1984 | 45 pp., PDF page N = printed page N + 4 | 1–40 | OCR (Acrobat Paper Capture); values read from the page images |
| October 1983 | 1983 | 40 pp., PDF page N = printed page N + 4 | 1–33 | none |

Both say "First Edition / First Printed: August 1982". **This file follows the 1984 printing**
and cites its page numbers. §7 lists what changed from 1983.

The third source is the software section of the *CompuPro Technical Training Manual* (45 PDF
pages, OCR text layer, values read from the page images). It holds four parts, each numbered
from page 1. This file cites them as:

| Cite | Part | Date | PDF pages |
|---|---|---|---|
| SAG appendix | I/O port list of all CompuPro products | — | 2 |
| SAG 2.2N | *Software Alteration Guide for CP/M 2.2 Version N* | 5/1/84 | 3–18 (pp.1–16) |
| SAG 86R | *Software Alteration Guide for CP/M 86 or 816 Version R* | 5/3/84 | 19–30 (pp.1–12) |
| SAG 68K | *CP/M-68K BIOS and Loader Modification Guide*, software rev 1.1K | 29 May 84 | 31–45 (pp.1–15; pp.5 and 6 are in the wrong order) |

A page number with no prefix is the October 1984 manual.

The manual is the CP/M side of the **Disk 1 / Disk 1A** floppy controllers
([CompuPro Disk 1 / Disk 1A](CompuPro%20Disk%201%20%26%201A%20Floppy%20Controllers.md)): the
system requirements, the floppy formats, the layout of a system disk, the boot chain, the CBIOS
console and list assumptions, how to rebuild the system, and a "Hardware Settings for CP/M"
section of switch and jumper tables for every CompuPro board in such a machine. The 1984
printing adds the **Disk 1A** settings. The board registers are not in this manual — they are in
the board reference. The troubleshooting tables (pp.29–31) are omitted.

**Not emulated** — it is software documentation. The `disk1a` board boots CompuPro CP/M 2.2LD,
a release of the same family (see §8).

---

## 1. Quick reference

| Item | Value | Page |
|---|---|---|
| CPU | CPU 8085/88 **with the swap port at `FDH`**, or CPU Z | 1 |
| Console board | System Support 1; or Interfacer 3 / 4 at `10H`; or Interfacer 1 / 2 at `0` | 1 |
| Floppy controller | Disk 1 **or Disk 1A** at **`C0H`**, **J17** jumpered | 1 |
| 5.25″ drives | on the Disk 1A, or on a second ("special") Disk 1 at **`CCH`** | 5 |
| Standard Disk 1A drives | 8″ = **A, B**; 5.25″ = **C, D** | 35 |
| Memory | at least 64 K of 24-bit-address RAM in the first 64 K page | 1 |
| Track 0, side 0 | always **FM, 26 × 128** | 20 |
| Data tracks | one of four formats (§2) | 14, 20 |
| System disk | must be one of the **MFM** formats (the CBIOS does not fit in format 0) | 20 |
| CP/M build (`HMX1BIOS`) | CCP `D200H`, BDOS `DD00H`, BIOS `E800H` | 10 |
| Boot program | read by the ROM to **`0100H`** | 20 |
| CBIOS console (System Support 1) | ports `5CH` / `5DH`, **19200 baud, 7 bits, odd parity, 1 stop** | 7, 22 |
| 8″ step / unload / head settle | 3 ms / 240 ms / 35 ms (`ACTIVE.LIB`) | 10 |
| Step-rate byte in a `CPMxxxx.COM` | `09A1`: `8F` 8 ms, `AF` 6 ms, `DF` 3 ms (§3.5) | SAG 2.2N 12 |

### 1.1 CompuPro I/O ports (SAG appendix)

"A list of the I/O ports used by all COMPUPRO products":

| Ports | Board |
|---|---|
| `00`–`03` | Interfacer 1 and 2 |
| `10`–`17` | Interfacer 3 and 4 |
| `50`–`5F` | System Support |
| `90` | Disk 3 |
| `C0`–`C3` | Disk 1 |
| `C4`, `C5` | reserved |
| `C6`, `C7` | MDrive/H |
| `C8` | Disk 2 |
| `F0` | Selector Channel |
| `F1` | MPX |
| `F2`–`F5` | reserved |
| `FD` | Memory Manager and the swap port on the dual processor |
| `FFF0`–`FFFF` | 80130 on the CPU 8086/87 |

The 5.25″ Disk 1 at `CCH` (p.5) is not in the list.

---

## 2. Floppy formats (FORMAT.COM)

Track 0 side 0 is always single density, 26 sectors of 128 bytes. The rest of the disk takes one
of four formats (pp.14, 20):

| Type | Other tracks | Density | Track 0 |
|---|---|---|---|
| 0 | 26 × 128 | single (FM) | 128 / SD |
| 1 | 26 × 256 | double (MFM) | 128 / SD |
| 2 | 15 × 512 | double (MFM) | 128 / SD |
| 3 | 8 × 1024 | double (MFM) | 128 / SD |

Type 0 is "IBM 1 (3740)". An 8″ disk is 77 tracks. The FORMAT banner reads
`8" disk: 77 tracks, 1 sided, format #0, 128 byte sectors.` (p.14). The manual gives no 5.25″
geometry.

FORMAT (pp.14–15, 23–24):

- `FORMAT` or `FORMAT d` — prompts `Specify drive (A: - P:)` if no drive is given, then shows
  the disk's present format and asks `Select Disk format type (0-3)`.
- It prints `F` for each formatted track (77 of them), then verifies: `V` read verified, `S`
  error but a retry worked, `E` hard failure.
- `FORMAT d: #` formats drive d with type # (batch use). `FORMAT d?` shows the format of d.
  `FORMAT d*` rewrites the directory of d with all entries "not in use". `FORMAT d!` is for
  memory drives: it formats only if the drive is not formatted already.
- It finds the number of sides itself and adjusts the format for a two-sided disk. It also
  formats MDrive memory disks.

COPY (pp.16, 24) copies track by track and compares: `A` all tracks, `D` data tracks, `S` system
tracks, `X` exit. It uses the 765A/8272 multi-sector commands. Source and destination must be
**exactly** the same format and number of sides, NEC-compatible FM/MFM.

SYSGEN (pp.17, 24) writes the BOOT (8080 code) and the CP/M system onto tracks 0 and 1, from a
`.COM` file (`SYSGEN CPM.COM`) or from a source drive. It handles one- and two-sided disks and
any of the MFM formats, "since the system tracks are always FM" (p.24). Only cylinder 0 side 0
is FM: §3.1 puts CCP and BDOS on cylinder 1 in MFM.

The backup procedure uses **format 3** (1024-byte sectors), the master's format (p.1). SYSGEN is
needed to move the system onto a 256- or 512-byte disk (p.2).

---

## 3. System-disk layout and the boot chain

### 3.1 Layout (p.25)

The manual numbers sectors **from 0** here.

| Cylinder / head | Recording | Contents |
|---|---|---|
| 0 / 0 | FM, 128-byte sectors | 0–1 BOOT; 2–23 CBIOS; 25 the RESTART code, loaded at `FFF8:0070` (`0FFFF0H`, the 8086/8088 reset address) |
| 0 / 1 | MFM, 256 / 512 / 1024 | sectors 1–n not used |
| 1 / 0, 512-byte sectors | MFM | 0–3 CCP; 4–11 BDOS; 12–15 reserved |
| 1 / 0, 1024-byte sectors | MFM | 0–1 CCP; 2–5 BDOS; 6, 7 reserved |
| 1 / 1 | MFM, 256 / 512 / 1024 | sectors 1–n not used |

n is 26 for 256-byte, 15 for 512-byte and 8 for 1024-byte sectors. ⚠ This table and p.20
disagree on the size of BOOT and the CBIOS sectors (§8).

### 3.2 Boot chain (pp.20–21, 25)

1. **Disk 1 ROM** (256 bytes, straight-line code, no memory data references): reads the first
   sectors (BOOT) into **`0100H`**, disables the ROM, jumps to BOOT. Side 0 head 0 is always FM.
2. **BOOT**: reads the CP/M LOADER, sends SPECIFY to every floppy controller in the system,
   initializes all serial ports, jumps to the LOADER's cold-start entry.
3. **LOADER**: initializes hardware that needs it, loads the CBIOS from cylinder 0 and jumps to
   its cold start.
4. **Cold start**: sets IOBYTE (location 3) and the current disk (location 4), sizes both kinds
   of MDrive, loads CCP and BDOS from the system disk, moves a copy of the CCP and BIOS to very
   high RAM for warm boots (if the 8088 switch is on), prints the sign-on, enters the CCP.
5. **Warm boot**: reloads CCP and BDOS and enters the CCP.

At cold boot the BIOS uses the CP/M auto-command feature to run **MFORM**, which formats an
MDrive/H only if it is not formatted yet, so a reset keeps the memory disk's data (pp.12, 19).

### 3.3 Pre-built systems on the master (pp.3–4, 6)

The master disk boots a very simple configuration. Pre-built `CPM*.COM` files are on it, put on
a disk with `SYSGEN CPMxxx.COM`:

| File | For |
|---|---|
| `CPMPLAIN.COM` | the master's own system: any 8080-type processor, 8 ms floppy step |
| `CPMSYS.COM` | CPU 8085/88 and CompuPro floppies (3 ms step Qume drives), floppy only |
| `CPM210.COM`, `CPM220.COM`, `CPM240.COM` | CPU 8085/88 with a Disk 2 and a 10, 20 or 40 MB hard disk |
| `CPMQ540.COM` | CPU 8085/88 with a Disk 3 and the 40 MB Quantum (H40) |
| `CPMZ80.COM` | CPU Z and CompuPro floppies |
| `CPMHMX2.COM` | CPU 8085/88 and the extended I/O system (`HMX2BIOS`) |

On a Disk 3 the logical drive partitioning differs between CP/M 2.2 and MP/M 8-16, CCP/M 8-16,
CP/M-86 and CP/M 8-16; only the first and last logical drives are the same (p.6).

SAG 2.2N (pp.4–5) gives each file's settings for **version 2.2N**:

| File | Processor | Disks | Console | List |
|---|---|---|---|---|
| `CPMPLAIN.COM` | — | four 8″ floppies at 8 ms, no hard disk | 9600, no handshaking | 9600, no handshaking |
| `CPM210.COM` | 8085/88 | Disk 2 + 10 MB; four 8″ at 3 ms | 9600, no handshaking | 9600, no handshaking |
| `CPM220.COM` | 8085/88 | Disk 2 + 20 MB; four 8″ at 3 ms | 9600, no handshaking | 9600, no handshaking |
| `CPMQ540.COM` | 8085/88 | Disk 3 + Quantum Q540 40 MB; four 8″ at 3 ms | 9600, no handshaking | 9600, no handshaking |
| `CPMSYS.COM` | 8085/88 | four 8″ at 3 ms | 9600, 7 data bits, 1 stop | 9600, DTR handshaking |
| `CPMHMX2.COM` | — | — | loads `HMX2IO.COM` (interrupts and more) | |

All support the MDrive/H and none the "soft" MDrive. The 2.2N features (SAG 2.2N p.1): Disk 2
and Disk 3 support, MDrive with RAM and the CPU 8085/88, MDrive/H, **minifloppies "using DISK 1 -
5"**, and interrupts. Changes since 2.2M: XON/XOFF and DTR protocols, wider IOBYTE support, Disk
3 support.

Master-disk files the manual does not list (SAG 2.2N pp.2–3): **`HMXFPROM.ASM`**, "floppy disk
(8 & 5 1/4) initializing program loader, similar to the routine found in Boot Rom on the Disk 1
Controller Board"; `DSBLINTR.ASM`, which disables the System Support 1 interrupts except the
console and printer; `STARTUP.SUB`, which loads `HMX2IO.COM` and `MFORM.COM`.

### 3.4 Rebuilding the system (pp.5–12)

The CBIOS is assembled with Digital Research **RMAC** and **LINK** (not supplied). The library
files are `ASCII.LIB`, `COMPUPRO.LIB` (hardware equates), `CPMDISK.LIB` (BDOS equates),
`ACTIVE.LIB` (the configuration) and `BOOTSCPM.LIB` (cold-boot routines). In `ACTIVE.LIB`,
`BDISK` is the warm-boot drive: the first floppy, so `A` without a hard disk and `I` with one.
`TRK5` is the track count of 5.25″ drives (usually 40, 77 or 80).

Two BIOS sources: **`HMX1BIOS.ASM`** has console and list drivers for all CompuPro I/O boards,
but space is tight (only two floppies, one hard disk and one memory drive fit on the boot
tracks). **`HMX2BIOS.ASM`** has only a minimal console driver; `HMX2IO.COM`, run at cold boot,
relocates a larger I/O system (IOBYTE, interrupts, XON/XOFF) into reserved space. With HMX2IO,
set `XLOADZ` to at least `400H` and move CCP and BDOS down by 1 K.

`ACTIVE.LIB` 8″ floppy constants as shipped (p.10):

| Equate | Value | Meaning |
|---|---|---|
| `FLOPPY8` | `TRUE` | 8″ floppy system (Disk 1) present |
| `FPY8X4` | `FALSE` | 4 drives present (2 is the default) |
| `STEPR8` | `3` | step rate, ms |
| `ULOAD8` | `240` | head unload delay, ms |
| `HDLT8` | `35` | head load settling time, ms |

Hardware handshake on an Interfacer 3/4 or System Support 1: remove the `;` before `or IF3DSR`
/ `or SS1DSR` in the transmit-ready masks `IF3TMSK`, `IF3FMSK`, `SS1TMSK`, `SS1FMSK` (p.10).

The build for `HMX1BIOS` (BIOS at `E800H`, so BDOS `DD00H` and CCP `D200H`; the CCP is always
`1600H` below the BIOS) (pp.10–12):

```
A>DDT MOVCPM.COM          MOVCPM.COM must be 14K
-S806
806 14 18                 hex count of 256-byte pages from the top of memory to the BIOS
807 00 .
A>SAVE 50 MOVCPM.COM
A>MOVCPM 64 *
A>SAVE 43 CPM64.COM
A>RMAC HMX1BIOS           (or RMAC HMX2BIOS)
A>LINK HMX1BIOS [LE800]   (or LINK HMX2BIOS [CLE800])    ABSOLUTE 0BAF (E800-F3AE)
A>RMAC HMXFBOOT
A>LINK HMXFBOOT           ABSOLUTE 0100 (0100-01FF)
A>DDT CPM64.COM           (CPMHMX2.COM for HMX2BIOS)
-IHMXFBOOT.COM
-R800
-IHMX1BIOS.COM            (or IHMX2BIOS.COM)
-R900
A>SAVE 43 CPM.COM
A>SYSGEN CPM.COM          SYSGEN Version 2.2D
```

The manual marks the sizes "may change".

### 3.5 Patching a `CPMxxxx.COM` with DDT (SAG 2.2N pp.12–13)

A `CPMxxxx.COM` image loaded by DDT (`NEXT 2C00`, `PC 0100`) can be changed without
reassembly, then saved with `SAVE 43 CPMNEW.COM` and put on a disk with SYSGEN.

**Step rate** — the byte at **`09A1`**: `8F` for 8 ms, `AF` for 6 ms, `DF` for 3 ms. It is the
first data byte of the µPD765 **Specify** command: SRT in the high nibble, HUT in the low one,
and on 8″ drives the step time is 16 − SRT ms ([NEC µPD765A FDC](NEC%20uPD765A%20FDC.md)).
`8`, `A` and `D` give 8, 6 and 3 ms, so the three values agree with the chip. The Disk 1A boot ROM
`291B_1.HEX` and the boot sector of `tests/media/compupro/CCPM22LD.DSK` both send
`03 8F 24` — 8 ms. CompuPro's `ROM1.ASM` sends `03 DF 12` — 3 ms.

**Serial setup** — the `INISEQ` table is stored as port, value pairs from `09BE` (the dump
starts at `09BF`, the value of the first pair) to the `FF` end marker at `09E4`:

| Port | Register | Values in the example |
|---|---|---|
| `17` | Interfacer 3/4 user select | `04`, `05`, `06`, `07` |
| `12` | Interfacer 3/4 mode registers 1 and 2 (two writes) | `EE`, then `7E` |
| `13` | Interfacer 3/4 command | `27` |
| `5E` | System Support 1 mode registers 1 and 2 | `EE`, then `7E` |
| `5F` | System Support 1 command | `27` |

Mode register 2: `7E` = 9600, `7F` = 19200, `79` = 1200. Mode register 1 `EE` is async 16×, 8
bits, no parity, 2 stop bits. The 2651 bit tables are in
[Signetics 2651 USART](Signetics%202651%20USART.md); SAG 86R p.6 reprints them.

---

## 4. CBIOS

### 4.1 Console, list, reader, punch (p.22)

The CBIOS assumes an Interfacer 1, 2, 3, 4 or a System Support 1:

| Board | Console | List | Reader, punch | UL1 |
|---|---|---|---|---|
| Interfacer 1 or 2 | ports 0 & 1 | ports 2 & 3 | ports 2 & 3 | not supported |
| System Support 1 | ports `05CH` & `05DH` | port 10, user 4 | not supported | port 10, user 5 |
| Interfacer 3 or 4 | port 10, user 7 | port 10, user 4 | not supported | port 10, user 5 |

"Port 10, user n" is the Interfacer 3/4 user-select scheme at base `10H`.

### 4.2 Serial setup in `HMXFBOOT.ASM` (pp.7–9)

The **I/O device initialization sequence table** (`INISEQ`) is a list of port, value pairs, ended
by port `0FFH`. As shipped:

| Device | Mode register 1 | Mode register 2 | Command |
|---|---|---|---|
| Interfacer 3/4, users 4, 5, 6, 7 (select with `IF3UX`) | `01011010b` — async 16×, 7 bits, odd parity, 1 stop | `01111110b` — 9600 baud | `00100111b` |
| System Support 1 UART | `01011010b` — async 16×, 7 bits, odd parity, 1 stop | `01111111b` — **19200 baud** | `00100111b` |

These are 2651 registers. The mode-register bit tables (p.8) are the chip's own — see
[Signetics 2651 USART](Signetics%202651%20USART.md). The low four bits of mode register 2 are
the baud rate: `1110` = 9600, `1111` = 19200.

The **I/O device initial select table** (`BIOTBL`) gives, for each switch value 0–3, an IOBYTE
and an auxiliary I/O control byte (`IOCNTL`):

| Switch | Assignment (comment in the source) |
|---|---|
| 0 | LPT = Interfacer I UART 1, CRT = Interfacer I UART 0 |
| 1 | LPT = Interfacer 3 user 4 (XON/XOFF), CRT = user 0 |
| 2 | LPT = Interfacer 3,4 user 4, CRT = System Support |
| 3 | LPT = Interfacer 3,4 user 4, CRT = System Support (CON = UC1:, LST = TTY:) |

`IOCNTL` = `ww xx yy zz`: `zz` picks the CRT (00 Interfacer 3 user 0, 01 System Support 1,
10 Interfacer 1,2 UART 0, 11 Interfacer 1,2 UART 1 custom); `ww` picks the LPT (00 Interfacer 3,4
user 4, 01 Interfacer 1,2 UART 1, 10 Interfacer 1,2 UART 2 custom); `xx` and `yy` are the
software protocol for the Interfacer 3,4 user 5 and user 4 list routines (00 none, 01 XON/XOFF,
10 ETX/ACK). XON/XOFF is the default as shipped.

SAG 2.2N (pp.10–12) decodes both bytes and ties the switch value to the Disk 1:

| Disk 1 S2-1 | S2-2 | Switch value |
|---|---|---|
| ON | ON | 0 |
| OFF | ON | 1 |
| ON | OFF | 2 |
| OFF | OFF | 3 |

The first byte is the IOBYTE, `AABBCCDD`: AA = LST (`00` TTY, `01` CRT, `10` LPT, `11` UL1),
BB = PUN (`00` TTY, `01` PTP, `10` UP1, `11` UP2), CC = RDR (`00` TTY, `01` PTP, `10` UP1,
`11` UP2), DD = CON (`00` TTY, `01` CRT, `10` BAT, `11` UC1). The physical devices are fixed:
**UC1 = Interfacer 3,4 user 7, TTY = user 6, UL1 = user 5**. With CON = BAT, the reader and punch
devices map to Interfacer 3,4 users: UR2/UP2 → user 3, UR1/UP2 (sic) → user 2, PTR/PTP → user 1,
TTY → user 0 (user 6 for the reader and punch vectors alone). ⚠ The switch-value mapping does
not agree with the consoles the manuals give for S2-1/2 (§8).

### 4.3 Drives (pp.5–6, 12)

- Without a hard disk the floppies are `A` and `B`. With a Disk 2, the hard disk is `A`, `B`,
  `C` and the floppies are `I` and `J`.
- With the Disk 1A standard settings, 8″ drives are `A`, `B` and 5.25″ drives `C`, `D` (p.35).
- Two RAM disks: **XMDRIVE**, the "soft" MDrive that uses the 8088 to move data in and out of
  memory above 64 K, and **HMDRIVE**, the MDrive/H (up to 4 MB). Either alone is `M`; with both,
  HMDRIVE is `H` and XMDRIVE is `M`. **XMBOOT** warm-boots from system memory instead of floppy.
- 5.25″ drives run on the Disk 1A or on a 5.25″ Disk 1 at `CCH`. With a **Disk 1**, a two-sided
  minifloppy needs the two-sided line (pin 10 of the 34-pin cable) grounded (p.5).

### 4.4 Disk routines (pp.21–28)

- The CBIOS uses a modified DEBLOCK (blocker / deblocker). **Before a disk is changed** the
  buffer must be flushed by a warm start, by closing all files and a disk reset, or by a call to
  **HOME** (which flushes and sets track 0). Otherwise data can be written to the new disk.
- **SELDSK** computes the DPH address and tests the least significant bit of `E`: non-zero, the
  saved disk type is used; zero (first select), it calls `TREAD` to find the density and sides,
  then sets the translation table and DPB address in the DPH.
- **SECTRAN** does no translation when `DE` = 0. Single density uses a 6-sector interlace.
- **READ** / **WRITE**: `CHKBKD` checks for single density; single density goes straight to the
  physical routine (`FINAL`), otherwise through the host buffer (`FILL`). WRITE's `C`: 0 = an
  allocated block, 1 = the directory, 2 = first write to an unallocated block.
- Two extra entries: **Set Extended Address** (the high byte of the 24-bit DMA address, for all
  IEEE 696 memory) and **Set Number of Sectors** (multi-sector transfers for direct callers).
- Variables: `ACTDSK` `ACTTYP` `ACTTRK` `ACTSEC` (active request), `HSTDSK` `HSTTYP` `HSTTRK`
  `HSTSEC` (host buffer), `SEKxxx` (the CP/M request).

The disk type definition block (p.26), one per mode:

| Size | Field |
|---|---|
| 8 bit | disk type code |
| 16 bit | sectors per track |
| 8 bit | block shift |
| 8 bit | block mask |
| 8 bit | extent mask |
| 16 bit | disk size / 1024 − 1 |
| 16 bit | directory size |
| 16 bit | allocation for directory |
| 16 bit | check area size |
| 16 bit | offset to first track |

It is a standard DPB with a type code byte in front.

---

## 5. Hardware settings for CP/M (pp.32–40)

`ON` / `OFF` as printed. Position 1 first.

### 5.1 Disk 1A (p.35, 1984 only)

"The standard switch settings for running 8″ floppies as drives A and B, and 5.25″ floppies as
drives C and D":

| Switch | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 |
|---|---|---|---|---|---|---|---|---|
| S1 | ON | ON | ON | \* | \* | \* | OFF | OFF |
| S2 | OFF | OFF | OFF | OFF | OFF | OFF | OFF | ON |
| S3 | OFF | ON | ON | ON | ON | OFF | OFF | ON |

S1 positions 4–6 (\*) select the processor and the console:

| S1-4 | S1-5 | S1-6 | CPU | Console |
|---|---|---|---|---|
| ON | ON | ON | CPU 86/87 | Interfacer 1/2 |
| ON | ON | OFF | CPU 68K | System Support / Interfacer 3/4 |
| ON | OFF | ON | CPU 86/87 | System Support |
| ON | OFF | OFF | CPU 86/87 | Interfacer 3/4 |
| OFF | ON | ON | CPU 85/88 - Z | Interfacer 1/2 |
| OFF | ON | OFF | not supported | |
| OFF | OFF | ON | CPU 85/88 - Z | System Support |
| OFF | OFF | OFF | CPU 85/88 - Z | Interfacer 3/4 |

Jumpers: J1, J2 position "5"; J3, J4 position "8"; J5 removed; J6 A–C for minifloppies that
generate READY, otherwise B–C; J7 B–C (two wait states); J8, J9 as shipped; J10 shunt on "4";
J11 shunt installed.

The manual: "These settings select DMA arbiter priority 15, port C0H–C3H, wait states enabled,
and the BOOT routine as selected." That agrees with the board reference §4: S3 2–7 (A2…A7, `ON`
= 0) = `C0H`, S3-8 ON = boot enabled, S2 4–7 all OFF = priority 15, S2-8 ON = wait states. J10
on 4 is VI4\*, the CompuPro floppy interrupt. ⚠ The S1 4–6 table does **not** agree with the
board reference (§8).

### 5.2 Disk 1 (p.36)

| | S1 1–8 | S2 1–8 | Jumpers |
|---|---|---|---|
| 8″ ("Disk 1-8") | ON OFF OFF OFF OFF OFF OFF OFF | ON\* ON\* OFF OFF ON ON ON ON | J16 B–C, J17 A–C |
| 5.25″ ("Disk 1-5") | ON OFF OFF ON OFF OFF OFF OFF | ON\* ON\* OFF OFF ON ON OFF OFF | J15 below |

- **S1-4**: OFF = boot from a floppy disk, ON = boot from a hard disk. The 5.25″ board has it
  ON — it is not the boot board.
- **S2-1, S2-2** (\*) choose the console: both ON = Interfacer 1 & 2; 1 OFF, 2 ON = System
  Support 1; both OFF = Interfacer 3 & 4.
- S2-3…8 decode to **`C0H`** (8″) and **`CCH`** (5.25″) with the board's inverted sense
  (`ON` = 0), which agrees with the board reference and with p.5.
- J15 (5.25″): A–B if the minifloppy does not drive `READY*`, A–C if it does. Ground the
  "TWO-SIDED" line for two-sided minifloppies.

### 5.3 CPU and System Support 1 (pp.32–33)

| Board | S1 1–8 | S2 1–8 | S3 1–8 |
|---|---|---|---|
| CPU 8085/88 | ON ON ON OFF OFF OFF ON OFF | all OFF | OFF ON OFF OFF OFF OFF OFF OFF |
| CPU Z | all OFF | all OFF | OFF OFF OFF OFF ON OFF OFF OFF |
| System Support 1 | OFF OFF OFF ON OFF OFF ON OFF | ON ON ON ON OFF OFF OFF OFF | OFF OFF OFF OFF ON OFF ON OFF |

CPU 8085/88 S3 (pos 2 ON only) is port **`FDH`**, as in
[CompuPro CPU 8085/88](CompuPro%20CPU%208085-88.md) §3. Its S1 by that file: 8085 and 8088
Reset-On-Swap (pos 4, 5) OFF — resume in place — and power-on jump (pos 8) OFF.

System Support 1 jumpers (p.33): J1 serial port connection; J2 a DIP shunt on all 8 lines; J3
the auxiliary battery cable, red wire to the left; **J13** on 8 and C for a CPU 8085/88, on C
and Z for a CPU Z; the rest open. Socket U16 takes a 6116 RAM or the "GO 86" EPROM (CPU
8085/88).

### 5.4 Other boards

The manual also gives switch and jumper tables for RAM 22, RAM 21, RAM 17 and RAM 16 (one per
64 K / 128 K / 256 K bank; the RAM 22 cannot disable any of its four 64 K blocks), Disk 2 and the
Selector Channel (Selector Channel S1-1: OFF = boot from floppy), Disk 3, Interfacer 1 (9600 baud
example), Interfacer 3 and Interfacer 4. They are not transcribed here; read pp.33–40.

MDrive/H (p.40): S1 1–7 are the same on every board (OFF OFF ON ON ON OFF OFF). Positions
8, 9, 10 number the board:

| Board | 8 | 9 | 10 |
|---|---|---|---|
| 1st | ON | ON | ON |
| 2nd | ON | ON | OFF |
| 3rd | ON | OFF | ON |
| 4th | ON | OFF | OFF |
| 5th | OFF | ON | ON |
| 6th | OFF | ON | OFF |
| 7th | OFF | OFF | ON |
| 8th | OFF | OFF | OFF |

---

## 6. Hard disk utilities (pp.2–3, 17–19)

`DISK2 [type] all` / `DISK3 [type] all` format, verify and test a hard disk and map out bad
sectors (the 1984 printing says the run takes 17 hours or more). Disk 2 types: `M10`, `M20`
(default), `M20BE`, `M40BE` (Fujitsu; BE = Pragmatic modification), `f40`. Disk 3 types: `Q540`
(default), `ST506`, `st412`, `cmi5619`. DISK3 defaults: step rate 0 × 100 µs, head settle
0 ms, 1024-byte sectors, 9 sectors per track, 8 heads, 512 cylinders, precomp from cylinder 256,
reduced current from cylinder 512, interleave 1, 8 retries, **21 reserved tracks**, drive 0.
After an `all` run, **do not** reformat a Disk 3 drive: it destroys the bad-sector map. `dummy`
writes an empty bad map to recover data from a damaged file system.

---

## 6A. CP/M-86 R and CP/M-68K 1.1K on the same hardware (SAG 86R, SAG 68K)

Only what bears on the Disk 1 / Disk 1A boot path.

**CP/M-86 / CP/M 8-16 version R** (5/3/84):
- New in R: the ASM86 assembler, reallocated Disk 2 sectors (reformat needed), interrupts, and
  "**a skeleton for 5 1/4″ floppy support**" (SAG 86R p.1).
- Every pre-built `CPMxxxx.SYS` (`CPM.SYS`, `CPM210`, `CPM220`, `CPM340`, `CPM35`) is **four 8″
  floppies at 3 ms**, 9600-baud console and list, no handshaking (SAG 86R p.4).
- The system tracks carry a loader, not the system: `SYSGEN LDRxx.CMD` (`88` for the CPU 85/88,
  `86` for the CPU 86/87), then `CPM.SYS` is a file. `MAKLDR88.SUB` / `MAKLDR86.SUB` build it
  with `GENCMD LOADER 8080 code[A800]` and `PIP LDR88.CMD=TMXFBOOT.88,LOADER.CMD` (SAG 86R
  pp.5, 11–12).
- `TMXDEVIO.INI` as shown: Interfacer users 4, 5 at 9600 8N2, user 6 at 1200 8N1, user 7 at 9600
  7O1; System Support 1 at 9600 8N2 (SAG 86R pp.6–7). The `BIOTBL` and S2-1/2 table are the same
  as 2.2N.
- `ACTIVE.EQU` titles its constants "DISK 1 or DISK 1A"; `FPY8X4` is `TRUE` (four drives) and
  `STEPR8` "(3-8ms.)" (SAG 86R p.10).

**CP/M-68K 1.1K** (29 May 84):
- Two loaders ship: `CPMLDRSS.SYS` (System Support console, 3 ms step, on the system tracks of
  diskette #1) and `CPMLDRIO.SYS` (Interfacer 3/4 user 7 console, 8 ms step, on diskette #2).
  `PUTBOOT` writes a loader to the system tracks; `CPM.SYS` is a file (SAG 68K p.7).
- Loader constants in `LBIOS.S`: `SYSSUP` (0 = System Support, 1 = Interfacer user 7), `STEPR8`
  (step rate in ms, new in 1.1K). BIOS constants in `BIOS.S`: `HIMEM` = (top of memory + 1) −
  `B000H` (`35000` for 256 K), `ORDER` (0 = floppies A–D, 1 = hard disk first), `XD8` (0 = four
  floppies, 1 = two), `FDBASE` (SAG 68K pp.2, 7–9).
- "CP/M-68K cannot load the BIOS from the hard disk at boot time" (SAG 68K p.12).
- Changing the System Support baud rate to 19200: replace `3e` with `3f` on line 43 of
  `LBIOS.S` (SAG 68K p.10).

None of the three May 1984 systems describes a 5.25″ boot disk.

---

## 7. What changed from the 1983 printing

| Topic | 1983 | 1984 |
|---|---|---|
| Requirement | Disk 1 at `C0H` (Disk 1A named only in the intro) | Disk 1 **or Disk 1A** at `C0H` |
| 5.25″ | "on the special 5¼″ DISK1 addressed at 0CCh" | "on the DISK 1A or the special 5-1/4″ DISK 1 at 0CCh"; pin-10 grounding only "if you are using a DISK 1" |
| Disk 1A switch table | none | p.35 (§5.1) |
| Pre-built `CPM*.COM` systems | none | pp.3–4, 6 (§3.3) |
| `INISEQ`, `BIOTBL`, handshake, `ACTIVE.LIB` step rate | none | pp.7–10 (§3.4, §4.2) |
| MOVCPM `806` | `18` ("use 19 for HMX2BIOS") | `18`, explained as pages from top of memory to the BIOS; HMX2BIOS links with `[CLE800]` |
| COPY | "uses the standard CBIOS call formats" | "uses the 765A/8272 multi-sector commands" |
| System Support 1 J2 | shunt lines 1, 2, 3 (3-wire) | shunt all 8 lines |
| Disk 2 / Disk 3 types | M10, M20, f40 / q540, st506 | adds M20BE, M40BE / st412, cmi5619, `dummy`, the DISK3 defaults |
| Format table, disk layout, boot chain, Disk 1 switches | | unchanged |

---

## 8. Quirks worth carrying

- ⚠ **Disk 1 S2-1/2: two meanings for the console.** The October 1984 manual (p.36) and the Disk 1
  board manual say 1 OFF 2 ON selects the System Support 1, and both OFF the Interfacer 3/4. SAG
  2.2N p.10 and SAG 86R p.7 say 1 OFF 2 ON is switch value 1, and their `BIOTBL` gives value 1 a
  CRT of Interfacer 3 user 0 and value 2 (1 ON 2 OFF) the System Support. **Unresolved.** A
  boot ROM passes its own value to the BIOS, so the ROM's code decides it, not the switch
  pages.
- ⚠ **The shipped serial format: three readings.** The `INISEQ` source listing gives the System
  Support 1 19200 baud, 7 bits, odd parity, 1 stop (p.7; SAG 2.2N p.10). The DDT patch example
  shows `EE` / `7E` — 9600 baud, 8 bits, no parity, 2 stop (SAG 2.2N pp.12–13), as does CP/M-86
  R's `TMXDEVIO.INI` (SAG 86R p.7). The `CPMSYS.COM` spec says 9600 baud, 7 data bits, 1 stop
  (SAG 2.2N p.5). **Unresolved**; set a terminal from the image's own bytes.
- **Interfacer user 6** is 8 bits, odd parity in SAG 2.2N (p.10) and 7 bits, odd parity in the
  October 1984 manual (p.7).

- ⚠ **Disk 1A S1 positions: two EPROM maps.** This manual (p.35) uses **S1 4–6** to pick the
  processor and the console, with console-specific routines (like the Disk 1's). The Disk 1A
  board manual (EPROM 1.4) and the `291B_1.HEX` dump use **S1 2–5** as a 4-bit routine number:
  four routines per processor family that differ in **drive search**, with the console chosen by
  **S3-1**. The same switches mean different things under the two maps. For example, the CP/M
  manual's 85/88 + System Support row (S1 2–5 = ON ON OFF OFF) is **routine 3** under the 1.4
  map, which tries a 5.25″ drive first. **The 1.4 map wins for the `disk1a` board**, because the
  board ships the `291B_1.HEX` image and that image follows it. The CP/M manual most likely
  describes an earlier EPROM. Unconfirmed.
- ⚠ **BOOT size: two readings.** p.20: the ROM reads "the first four sectors (BOOT program)",
  and BOOT "resides in the first four sectors of the disk (512 bytes)". p.25: sectors 0–1 (two
  128-byte sectors) are BOOT. Both printings say the same. **Unresolved.** In
  `tests/media/compupro/CCPM22LD.DSK` (CP/M 2.2LD) sectors 1–2 hold code and sectors 3–4 are
  zero, which fits both readings. The Disk 1A boot ROM does neither: it reads **all 26 sectors**
  of track 0 to `0100H` (Read Data with R = 1, EOT = 26; `docs/sources.md`, `291B_1.HEX`).
- ⚠ **CBIOS sectors: two readings.** p.20: "the 3rd thru 26th sectors". p.25: sectors 2–23
  (0-based, so the 3rd–24th) and sector 25 (the 26th) is the 8086 RESTART code. In the 2.2LD
  image the BIOS jump table starts at sector 5 and sector 26 holds 8080 BIOS code, not 8086
  code — but 2.2LD is a different BIOS, so it settles nothing here. **Unresolved.**
- ⚠ **Sector numbers.** p.25 counts sectors from 0; the µPD765A counts from 1 (the Disk 1A ROM
  reads from R = 1). Add one before you look for a p.25 sector in a disk image.
- **Neither printing can boot from a 5.25″ disk.** The 1984 printing is written for the Disk 1A
  and still makes the 5.25″ drives `C` and `D`, with the system on 8″ `A` (p.35). The system-disk
  layout needs track 0 side 0 in FM with 26 × 128 bytes (pp.20, 25). That is 3,328 data bytes. A
  5.25″ FM track at 125 kbit/s holds about 3,125 bytes in all, gaps and headers included, so the
  layout does not fit. CompuPro's `HMXBIOS.ASM` gives 16 × 128 as its 5.25″ single-density
  format. This does **not** show that no 5.25″ boot disk existed. The 1.4 boot EPROM has two
  routines (2 and 3) that try a 5.25″ drive, so a later CompuPro system (CP/M 2.2LD, CP/M-86,
  CCP/M-86, MP/M 8-16) could have a 5.25″ system disk with a different track 0. A second Disk 1
  must have its ROM off whatever its media, because two boot ROMs cannot both answer at the
  reset address. The May 1984 alteration guides narrow it further: CP/M 2.2N supports
  minifloppies only "using DISK 1 - 5", CP/M-86 R has only "a skeleton" for 5.25″ support, and
  every pre-built CP/M 2.2N and CP/M-86 R system is for 8″ floppies; the CP/M-68K loaders set
  only the 8″ step rate `STEPR8` (§3.3, §6A). As of
  May 1984 no CompuPro operating system documents a 5.25″ boot disk. Issue #746 tracks the search
  for a 5.25″ image.
- **Format 0 does not make a system disk.** The CBIOS needs an MFM format on cylinder 1.
- **"System tracks are always FM" (p.24) is only true of cylinder 0 side 0**; cylinder 1 is in
  the data format (p.25).
- **The console is 7 bits, odd parity** as shipped (`INISEQ`, p.7), at 19200 baud on a System
  Support 1. A terminal set to 8N1 sees the parity bit.
- **The drive letters move with a hard disk:** floppies are `I`/`J`, and `BDISK` must be set to
  `I`.
- **The CBIOS's sign-on counts the MDrive/H boards it found.** A wrong count means a wrong
  switch setting (p.12).
- **"Disk 1" in the software sections means both boards.** The Disk 1 switch tables (p.36) are
  the **Disk 1's** — the Disk 1A moves every switch (board reference §4 and §8).
