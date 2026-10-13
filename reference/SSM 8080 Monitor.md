# SSM 8080 Monitor V1.0 — the operator's manual and example listing

Source: [SSM_8080_Monitor_1977.pdf](#)

The SSM (Solid State Music) 8080 Monitor V1.0 by C. E. Ohme, copyright 1977: 48 pages (printed
pages are the PDF page minus one). It is a 2 KB ROM monitor at `F000`–`F7FF`: a command
interpreter, a table-driven I/O configuration (the "System Configuration Package", `F600`), and
a VB-1 video driver (`F700`). The manual has the operator's commands (§1), a full source listing
**with object bytes** (pp. 10–42), a loading and example-session section (§2) and the I/O
configuration (§3). This file keeps the command set, the entry points, the I/O tables and the
points that decide emulation. It leaves out the source listing: the shipped ROM sources are in
`roms/SSM-8080MON/` and are the better copy of it.

**Used as a ROM, not emulated.** It ships as the built-in ROM `ssm-8080mon` and boots on the `io4`
example machine. Nothing in the simulator is specific to it.

## 1. Quick reference

| Item | Value |
|---|---|
| Origin | `F000` (jump table), `F600` (System Configuration Package), `F700` (VB-1 video driver) |
| Banner | `CR LF MONITOR V1.0`, then the prompt `.` on every command line |
| Error | `LF` + `*`, then a new prompt. The only error message |
| RAM | at least 256 bytes from `0000`; a 64-byte scratchpad is made at the **top of contiguous RAM** on cold start |
| Breakpoints | `RST 1` (`CF`). A `JMP` to the monitor is stored at `0008H` |
| Console (TTY version) | status port `0`, data port `1`, status bit 0 = DAV (active low), bit 7 = DAK (active low) |
| Console (video version) | keyboard data port `3`, status port `2`, DAV = D0 (active low); output memory mapped to `EC00` (1702 version) or `B000` (2708 version) |
| Stack at a breakpoint | the monitor uses 12 bytes of the **user's** stack to save the registers |

## 2. Commands (§1.0, pp. 2–6)

One letter, then parameters separated by a comma or a space. A parameter is the last four (or two)
hex digits typed before the terminator; leading zeros are assumed. A carriage return ends the line.
Any other character aborts the command with `*`.

| Cmd | Form | What it does |
|---|---|---|
| `D` | `D low,high` | Display memory, 16 bytes a line, each line led by its 4-digit address. `high` ≤ `low` shows one byte |
| `S` | `S addr` | Show the byte then `-`. Hex digits replace it, space or comma goes on to the next address, CR ends |
| `F` | `F low,high,data` | Fill. `high` ≤ `low` fills one byte |
| `M` | `M low,high,dest` | Move. If `dest` is inside the source the bytes repeat to fill the destination |
| `B` | `B low,high` | Binary dump to the punch device, MITS paper-tape format. `high` = 0 writes the end-of-file record with `low` as the entry address |
| `L` | `L bias` | Binary load from the reader, plus `bias`. An entry address of 0 leaves the monitor in control |
| `W` | `W low,high` | Hex dump to the punch device, Intel format. `high` = 0 writes the end-of-file record |
| `R` | `R bias` | Hex load from the reader, plus `bias`. Non-zero entry address = jump |
| `N` | `N` | Sixty `00` bytes to the punch device |
| `K` | `K` | Copy reader to punch until reset |
| `G` | `G addr,bp1,bp2` | Go. No `addr` = resume at the last breakpoint, registers restored. Breakpoints write `RST 1` into RAM (not ROM) |
| `X` | `X` or `X reg` | Registers (§3) |
| `H` | `H a,b` | Print the sum and the difference of two hex numbers |
| `A` | `A c=n` | Assign physical device `n` (0–3) to logical device `c` (`C` console, `R` reader, `P` punch, `L` list). First letter only |

## 3. The `X` command (p. 5–6 and the listing, pp. 27–28)

* `X` + CR prints **all** registers with no operator action, in table order
  `A B C D E F H L M P S`, each as `<name>=<value> `. 8-bit registers print 2 hex digits,
  `M` (HL), `P` (PC) and `S` (SP) print 4.
* `X reg` shows `reg`, a `-`, and lets the operator type a new value. Space or comma keeps or
  changes it and goes to the next register in table order. CR ends. The command also ends after `S`.
* The registers shown are those saved at the **last breakpoint** (`RST 1`), not the live CPU.

The dump loop in the listing (label `X7`, printed page 28) is:

```
F5A2 CDCFF0   X7:   CALL  BLK
F5A5 7E             MOV   A,M        ; register letter, 0 = end of table
F5A6 23             INX   H
F5A7 B7             ORA   A
F5A8 F8             RM
F5A9 4F             MOV   C,A
F5AA CDD1F0         CALL  CO         ; letter
F5AD 0E3D           MVI   C,'='
F5AF CDD1F0         CALL  CO
F5B2 7E             MOV   A,M        ; offset from SP
F5B3 23             INX   H
F5B4 EB             XCHG
F5B5 6F             MOV   L,A
F5B6 2600           MVI   H,0
F5B8 39             DAD   SP
F5B9 EB             XCHG             ; DE -> saved register, HL -> ACTBL
F5BA 46             MOV   B,M        ; size, 1 or 2
F5BB 23             INX   H
F5BC 1A             LDAX  D          ; the byte (high byte of a pair)
F5BD CD95F1         CALL  LBYTE
F5C0 05             DCR   B
F5C1 CAA2F5         JZ    X7
F5C4 1B             DCX   D
F5C5 1A             LDAX  D          ; low byte of a pair
F5C6 CD95F1         CALL  LBYTE
F5C9 C3A2F5         JMP   X7
```

The first read is `LDAX D` (`1A`), the same as the edit path `X2` at `F565`. ⚠ An OCR'd copy of the
listing has `LDAX B` (`0A`) at `F5BC`: BC is size:letter there, so every register read the same
constant byte. Issue #727. The printed page, not the OCR, is the authority.

`ACTBL` at `F5CC` has 3 bytes a row: letter, offset from SP (`xLOC+2`), size.

| Letter | Offset symbol | Size |
|---|---|---|
| `A B C D E F H L` | `ALOC+2` … `LLOC+2` | 1 |
| `M` | `HLOC+2` | 2 |
| `P` | `PLOC+2` | 2 |
| `S` | `SLOC+2` | 2 |

## 4. Entry points (§1.0, pp. 6–9)

| Address | Name | Contract |
|---|---|---|
| `F000` | `MONTR` | Cold start. Sets every logical device to physical 0 |
| `F003` | `CI` | Console in → A. Keeps all but A, F |
| `F006` | `RI` | Reader in → A. Carry set if nothing is there |
| `F009` | `CO` | Console out, byte in C |
| `F00C` | `PO` | Punch out, byte in C |
| `F00F` | `LO` | List out, byte in C |
| `F012` | `CSTS` | Console status. A = 0 and Z set if no character |
| `F015` | `IOCHK` | A = `IOBYT` |
| `F018` | `IOSET` | `IOBYT` = C |
| `F01B` | `MEMCK` | BA = address of the first byte above the user's RAM |
| `F01E` | `STRNG` | Print the string at HL, ends before a null or after a byte with bit 7 set. Keeps B, D, E |
| `F021` | `REENT` | Warm entry. The I/O configuration is not touched |

## 5. System Configuration Package at `F600` (§1.0 p. 8–9, §3)

`IOTAB` at `F600`: six sub-tables, four 2-byte driver addresses each (console status, console in,
console out, reader, punch, list). Three entry stubs reached by `CALL`:

| Address | Name | Result |
|---|---|---|
| `F630` | `ADSCS` | DE = top of the monitor scratchpad. Called **without the stack** (`LXI D,$+6 / JMP`) |
| `F633` | `ADSCR` | HL = top of the scratchpad |
| `F636` | `ADIOB` | HL = address of `IOBYT` |
| `F639` | `ADUST` | HL = default user stack |

`IOBYT` is the last byte of contiguous RAM: bits 1–0 console, 3–2 reader, 5–4 punch, 7–6 list.

## 6. Preset devices (§3.1, p. 47)

| Logical | Status port | Status bits (active low) | Data port | What |
|---|---|---|---|---|
| `AC=0` | 0 | DAV=D0, DAK=D7 | 1 | Main console |
| `AC=1` | 2 / 0 | DAV=D0 / DAK=D7 | 3 / 1 | Keyboard in, console out |
| `AC=2` | 2 / 2 | DAV=D0 / DAK=D7 | 3 / 3 | Keyboard in, output device |
| `AC=3` | 2 / memory mapped | DAV=D0 | 3 | Keyboard in, VB-1 out |
| `AR=0` | 0 | DAV=D0 | 1 | Console |
| `AR=1` | 4 | DAV=D0 | 5 | Reader |
| `AR=2` | 2 | DAV=D0 | 3 | Keyboard |
| `AR=3` | | | | Vector to a disk read (iCOM Microfloppy) |
| `AP=0` | 0 | DAK=D7 | 1 | Console |
| `AP=1` | 4 | DAK=D7 | 5 | Punch |
| `AP=2` | memory mapped | | | VB-1 |
| `AP=3` | | | | Vector to a disk write |
| `AL=0` | 0 | DAK=D7 | 1 | Console |
| `AL=1` | memory mapped | | | VB-1 |
| `AL=2` | 2 | DAK=D7 | 3 | Listing device |
| `AL=3` | 0 | DAK=D7 | 1 | Console |

## 7. Variants (§2.2, p. 44)

| Item | 1702 build | 2708 build |
|---|---|---|
| Video RAM `VID` | `EC00` | `B000` (VB-1B card, so a 16K ROM card does not clash) |
| Bytes that differ (`F70C`, `F729`, `F72E`, `F72F`, `F737`, `F74C`, `F74D`, `F754`, `F767`, `F77B`, `F782`, `F783`, `F786`, `F78F`, `F7E2`) | `EC`/`F0`/`EF` | `B0`/`B4`/`B3` |

Console choice is six bytes at `F600`:

| Address | Video version | TTY version |
|---|---|---|
| `F600` | `7F` | `A0` |
| `F606` | `A0` | `7F` |
| `F608` | `66` | `A8` |
| `F60E` | `A8` | `66` |
| `F610` | `59` | `B7` |
| `F616` | `B7` | `59` |

In the video version `AC=0` is the parallel keyboard and CRT and `AC=3` is the teletype.
The shipped ROM here has the TTY column.

## 8. Quirks worth carrying

* ⚠ **The listing is the authority for the bytes.** The printed text layer of this PDF is poor
  (`B`↔`8`, `D`↔`0`, `S`↔`5`); the page images are clean. Of the 476 object bytes whose addresses
  parse cleanly, every one matches `roms/SSM-8080MON/SSM-8080MON.HEX` except `F5BC`.
* `X` shows the register state saved at the last **breakpoint** (or `RST 1`), not the live CPU.
* Breakpoints cannot be set in ROM (`RST 1` is written to memory).
* Video version: the sense switch at port `FF` bit 0 only starts and stops a long listing. With no
  such port, A8 reads as 1 and that is fine.
* The manual calls the version "V1.0" in the banner, `MONITOR V1.0`.
