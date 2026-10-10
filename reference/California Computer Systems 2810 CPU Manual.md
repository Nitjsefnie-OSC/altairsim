# CCS Model 2810 — Z-80 CPU with 8250 serial port and MOSS 2.2 monitor

Source: [California Computer Systems 2810 CPU Manual.pdf](#)

California Computer Systems (CCS), Sunnyvale CA. *Owner's Manual, Model 2810 Z-80 CPU*, manual
no. 89000-02810 (the comment sheet says `89000-02810A`), © 1980, 105 PDF pages. One S-100 board
with a **Z-80** (2 or 4 MHz, toggle switch), one **asynchronous serial port** built on a
**National 8250** Asynchronous Communications Element, a **2716 monitor EPROM** at `F000h`
(the **MOSS 2.2** monitor, © Robert B. Mason, 20 June 1980) and a **power-on jump** circuit.
The PC board is `2810 CPU, rev A` (part `02810-00002`, p. D-4).

**Not emulated.** This file omits the warranty (Appendix E), the generic Z-80 register
description (B.1; see [Zilog Z80 CPU](Zilog%20Z80.md)) and the S-100 history (A.1.1). The 8250
is recorded here **as this manual documents it** — CCS renamed its registers and bits (§5). It
is not a chip reference: the manual leaves out the Interrupt Identification register and says
nothing about interrupt behavior. **The chip itself is in
[National INS8250A / NS16450](INS8250.md)**, with National's names. How the 8250 family's parts differ is in
[National AN-493](NS16450.md); the parts list here says only `8250` (U5), so which part the
board carried is not settled by this manual.

**Scan notes.** No text layer — every page was read as an image. Pages carry chapter-page
numbers (`2-2`, `B-6`, `C-19`); this file cites those. Printed `C-4` and `C-14` (firmware) and
`D-5` (board layout) are bound upside down. The schematic (`D-7`) is too coarse to read a single
signal name; **nothing here comes from it**. In Appendix D the layout and schematic come before
the parts list in the PDF. No handwritten marks.

## 1. Quick reference for emulation

| Item | Value |
|---|---|
| CPU | Z-80 (layout marks U10 `Z-80A MK3880N-4`), 2 or 4 MHz by toggle switch S1, 16 MHz crystal |
| Bus | S-100, 8080-style signals rebuilt from the Z-80's (§8) |
| Serial chip | **8250** (U5), 1.8432 MHz crystal (Y1), 75150 drivers / 75154 receiver |
| Serial ports | **8 addresses decoded** (A3–A7 compared, A0–A2 to the chip). Base = any multiple of 8. **MOSS needs base `20h`** |
| Registers used | `base+0` data / divisor low, `+1` interrupt enable / divisor high, `+3` line control, `+4` modem control, `+5` line status, `+6` modem status |
| Serial interrupts | **Not used.** MOSS writes 0 to the interrupt enable register. Whether the 8250's interrupt pin reaches the bus is not stated |
| Console format | 8 data bits, no parity, 1 stop bit (`LCR = 03h`) |
| Baud | **Auto-baud**: MOSS times the start bit of a carriage return through **modem status bit 6** (§6) |
| ROM | 2716, 2K, `F000h`–`F7FFh`; MOSS 2.2 occupies `F000h`–`F714h`. PHANTOM* turns it off byte by byte |
| ROM wait | One wait state on every ROM access **when at 4 MHz** (p. 4-9) |
| Reset | Power-on jump forces `JMP` to a jumper-set address; `F000h` for MOSS. With the jump disabled the Z-80 starts at `0000h` |
| RAM MOSS needs | `0000h`–`003Eh` (it writes all of it), plus about 88 bytes at the top of contiguous RAM |
| Power | +8 V 0.650 A, +16 V 0.030 A, −16 V 0.025 A; 6.2 W (p. D-9) |

## 2. Jumpers and the switch (§2.1, pp. 2-1 to 2-5; layout p. D-5)

The `W` numbers are from the board layout; the manual's text uses the names only.

| Jumper | Name | What it does |
|---|---|---|
| W1–W5 | SERIAL ADDRESS SELECT A7…A3 | The five high bits of the serial port base. Each is set to 1 or 0 |
| W6 | SER EN | ON enables the serial port |
| W7 | ADD MIR | ON puts the I/O port number on A8–A15 too, as an 8080 does (§8) |
| W8 | ROM EN | ON enables the on-board ROM. OFF frees `F000h`–`F7FFh` |
| W9 | WAIT | ON adds one wait state to every M1 cycle (and so to interrupt acknowledge) |
| W10–W25 | JMP ADDR SEL JA15…JA0 | The power-on jump address, one jumper per bit. The layout shows two blocks, W10–17 and W18–25; which block is the high byte could not be read |
| W26 | JMP EN | ON enables the power-on jump |
| W27 | 2/4 MHZ | ON drives bus pin 98 high at 4 MHz, low at 2 MHz |
| W28 | PHANTOM | ON lets bus pin 67 disable the on-board ROM |
| W29 | NMI | ON connects bus pin 12 to the Z-80's NMI (restart at `0066h`) |
| W30 | REFRESH | ON drives bus pin 66 with the Z-80's refresh signal |
| S1 | 2/4 MHz toggle | Sets the CPU clock. Set it before power-on or reset, not while running |
| pads `65` | MREQ | A soldered wire between two pads puts the Z-80's MREQ* on bus pin 65 |

**Settings MOSS requires** (pp. 2-2, 2-4, 3-1): SER EN on, A7–A3 = `00100` (base `20h`); ROM EN
on; JMP EN on with JA15–JA12 = 1 and JA11–JA0 = 0 (`F000h`).

Three LEDs: ROM (the ROM is enabled and selected), HALT (`sHLTA`), INTE (`pINTE`).

## 3. Serial port: address decode and bus interface (§4.2.1, p. 4-11)

- The 8250 has two chip selects. **CS1** is high when the Z-80's IOREQ* is active and M1* is
  not — this keeps an interrupt acknowledge cycle from selecting the chip. **CS0** is high when
  A3–A7 match the SERIAL ADDRESS SELECT jumpers.
- A0–A2 go straight to the chip's register select pins.
- `pDBIN` is the chip's read strobe and `pWR*` its write strobe.
- On a read, the 8250's `DDIS*` output turns off the board's Data In bus buffer. The byte goes
  to the Z-80 on the board's internal data lines. It is not driven onto the S-100 bus.
- Address mirroring does not matter: only A0–A7 are decoded.

⚠ The setup text says the registers are "seven I/O ports, either as X0h through X6h or as X8h
through XEh" (p. 2-2). The decode compares A3–A7 only, so the chip is selected at all **eight**
addresses. `base+7` is a location the manual never mentions — the original INS8250 has no
register there ([AN-493](NS16450.md) §3) — and `base+2` is one it never names (§5).

## 4. Serial port: the RS-232 side (§4.2.2 pp. 4-11 to 4-12; A.2 pp. A-9 to A-10)

The connector is wired as **DCE** (a modem), so a terminal plugs in with a straight cable. The
8250's pins are named for a DTE, so every handshake line changes its name between the chip and
the connector. This is Table 4-3, with the register bit added from the firmware's equates and
Table B-2:

| 8250 pin | Direction | Connector signal | DB-25 pin | Register bit |
|---|---|---|---|---|
| SOUT | out | RxD (Receive Data, BB) | 3 | — |
| SIN | in | TxD (Transmit Data, BA) | 2 | — |
| DTR | out | DSR (CC) | 6 | modem control bit 0 |
| RTS | out | CTS (CB) | 5 | modem control bit 1 |
| OUT 1 | out | RLSD (CF) | 8 | modem control bit 2 |
| CTS | in | RTS (CA) | 4 | modem status bit 4 (delta: bit 0) |
| DSR | in | DTR (CD) | 20 | modem status bit 5 (delta: bit 1) |
| **RI** | in | **TxD again** — the peripheral's serial data | 2 | **modem status bit 6** |

- Pin 1 is protective ground (AA), pin 7 signal ground (AB).
- **The serial input is also wired to the 8250's Ring Indicator input "to support the auto-baud
  feature of the 2810's firmware"** (p. 4-12). The firmware reads it as modem status bit 6 with
  mask `40h`, and treats **1 as a start bit** (line spacing) and 0 as idle (§6).
- J2 is a 2 × 13 right-angle header; the user supplies a ribbon cable to a DB-25S (p. 2-6).
- The drivers and receivers translate TTL to "nominal +5 to −5 volt" levels (p. 4-11).

⚠ The pin drawing (p. A-10) also shows **pin 24, `DA`, "TRANSMIT SIG ELE CLK (DTE)"**, with an
arrow in the same direction as DTR's. No text explains it, and A.2.1 does not list it.

⚠ The manual never says what the 8250's `OUT 2`, `DCD` (RLSD input) or interrupt pins connect
to. Table B-2 says only to write modem control bit 3 (`OUT 2`) as 1.

## 5. The 8250 registers as CCS documents them (Appendix B.2, pp. B-5 to B-9)

### 5.1 Addressing (Table B-1)

`DLAB` is bit 7 of the line control register.

| Offset | DLAB | Read | Write | CCS name (Table B-2 mnemonic) | Firmware equate at base `20h` |
|---|---|---|---|---|---|
| 0 | 0 | Receiver Buffer | Transmitter Holding Register | RBR / THR | `SDATA` = `20h` |
| 0 | 1 | Divisor Latch, low byte | same | DLL | `SDATA` |
| 1 | 0 | Interrupt Enable | same | IER | `SINTEN` = `21h` |
| 1 | 1 | Divisor Latch, high byte | same | "MSR" ⚠ | `SINTEN` |
| 2 | x | *not in Table B-1 or B-2* | | — | `SIDENT` = `22h`, "interrupt identification"; never used |
| 3 | x | Line Control | same | LCR | `SLCTRL` = `23h` |
| 4 | x | **Peripheral Control** | same | PCR | `SMDMCT` = `24h`, "modem control" |
| 5 | x | Line Status | (MOSS writes 0 here) | LSR | `SLSTAT` = `25h` |
| 6 | x | **Peripheral Status** | | PSR | `SMDMST` = `26h`, "modem status" |
| 7 | x | *not mentioned* | | — | — |

The manual counts "nine accessible registers of concern". **CCS's "Peripheral Control" and
"Peripheral Status" are the 8250's modem control and modem status registers**; the firmware
listing uses the usual names.

⚠ Table B-2 heads the divisor high-byte column `MSR`. It is the divisor latch (bits 8–15), not
a status register.

### 5.2 Bit summary (Table B-2, p. B-6)

Bit names are CCS's, which are **connector** names (§4). The last column of each pair gives the
8250 pin behind the bit.

| Bit | IER | LCR | Peripheral Control (`+4`) | 8250 pin | LSR | Peripheral Status (`+6`) | 8250 pin |
|---|---|---|---|---|---|---|---|
| 0 | "Set to 0" | Word length select bit 0 | DSR | DTR | Data Ready | Delta CTS ⚠ | change on CTS input |
| 1 | "Set to 0" | Word length select bit 1 | CTS | RTS | Overrun Error | Delta DSR ⚠ | change on DSR input |
| 2 | "Set to 0" | Number of stop bits | RLSD | OUT 1 | Parity Error | 0 | |
| 3 | 0 | Parity enable | "Set to 1" | (OUT 2) | Framing Error | 0 | |
| 4 | 0 | Even parity select | Loop | | Break Interrupt | RTS | CTS input |
| 5 | 0 | Stick parity | 0 | | Transmitter Holding Register Empty | DTR | DSR input |
| 6 | 0 | Set break | 0 | | Transmitter Shift Register Empty | 0 ⚠ | **RI input = serial data in** |
| 7 | 0 | Divisor Latch Access Bit | 0 | | 0 | 0 | |

- **IER.** "Since we do not use the 8250's interrupt capabilities, the first four bits of the
  Interrupt Enable Register should be set to 0" (p. B-5). The table marks only bits 0–2 "Set
  to 0" and prints bit 3 as 0. MOSS writes `00h`.
- **Peripheral Control** (B.2.1). Bits 0–2 drive the DSR, CTS and RLSD connector outputs; "to
  set one of these signals active high, write a 1 to its bit". Bit 4 = 1 is loopback: transmit
  data goes to the receiver and is not sent out.
- **Peripheral Status** (B.2.3). Bits 0 and 1 are "set to a logic 1 whenever the state of the
  control line has changed since the peripheral status register was last read".

⚠ **Peripheral Status bit 6 is not 0.** The data sheet makes it the complement of the RI* pin
([INS8250](INS8250.md) §8). Table B-2 prints 0, but p. 4-12 wires the serial input
to the 8250's RI pin and the firmware's auto-baud loop reads this bit (`IN 26h`, `ANA` with
`40h`, pp. C-18 to C-19). **The firmware won**: the board cannot find its baud rate if the bit
is 0.

⚠ **The delta bits keep the 8250's names; the level bits get CCS's.** Bits 4 and 5 are
renamed to the connector's RTS and DTR, but bits 0 and 1 are printed "Delta CTS" and "Delta
DSR". By CCS's own rule they are the change flags for the connector's RTS and DTR. Not
resolved from the manual; the firmware never reads them.

### 5.3 Line Control Register (Table B-3, p. B-7)

| Bit | Name | Meaning |
|---|---|---|
| 1, 0 | Word Length Select | `00` 5 bits, `01` 6 bits, `10` 7 bits, `11` 8 bits |
| 2 | Stop Bits Select | 0 = 1 stop bit. 1 = 1½ stop bits with a 5-bit word, 2 stop bits with 6, 7 or 8 |
| 3 | Parity Enable | 0 = no parity bit, 1 = parity bit between the last data bit and the stop bit |
| 4 | Even Parity Select | 0 = odd, 1 = even |
| 5 | Stick Parity | With bit 3 = 1: bit 4 = 0 sends the parity bit as 1, bit 4 = 1 sends it as 0. Bit 5 = 0: no stick |
| 6 | Set Break | 1 forces the serial output to spacing (logic 0) |
| 7 | Divisor Latch (DLAB) | 0 = receiver buffer / holding register / interrupt enable; 1 = divisor latches |

The register can be read back at any time.

⚠ Table B-3 heads the word-length columns "Bit 0  Bit 2". The field is bits 1 and 0 (Table B-2
agrees). The firmware writes `03h` for 8 bits. The table also names the break output `sOUT`,
which on this bus is a status line; it means the 8250's serial output.

### 5.4 Line Status Register (Table B-4, p. B-8)

"Except where otherwise noted, the bits are reset when the CPU reads the line status register."

| Bit | Name | Definition |
|---|---|---|
| 0 | Data Ready (DR) | 1 when the receiver buffer is full. "Reset by CPU reading buffer or writing a 0 to it" |
| 1 | Overrun Error (OE) | 1 if the CPU did not read the receiver buffer before the next character was moved into it |
| 2 | Parity Error (PE) | 1 when a parity error is detected |
| 3 | Framing Error (FE) | 1 if the incoming character has no valid stop bit |
| 4 | Break Interrupt (BI) | 1 when the received data input is held spacing for longer than a full word time |
| 5 | Transmitter Holding Register Empty (THRE) | 1 when the holding register has moved its data to the shift register. Reset when the CPU loads the THR |
| 6 | Transmitter Shift Register Empty (TSRE) | 1 when the shift register is idle. Reset on a transfer from the THR. Read-only |
| 7 | — | Permanently 0 |

**The line status register is writable on this chip, and MOSS writes it**: `OUT 25h` with 0,
commented "AND RESET ERROR FLAGS" (p. C-19). The table's "writing a 0 to it" for Data Ready is
the same fact.

### 5.5 Divisor latches and baud rates (B.2.5, Table B-5, pp. B-8 to B-9)

Clock in = **1.8432 MHz**. The baud generator's output is 16 × the baud rate:
`divisor = 1 843 200 / (baud × 16) = 115 200 / baud`. Any divisor from 1 to 2¹⁶ − 1.

| Baud | Divisor | Error % | | Baud | Divisor | Error % |
|---|---|---|---|---|---|---|
| 50 | 2304 | – | | 2000 | 58 | 0.69 |
| 75 | 1536 | – | | 2400 | 48 | – |
| 110 | 1047 | 0.026 | | 3600 | 32 | – |
| 134.5 | 857 | 0.058 | | 4800 | 24 | – |
| 150 | 768 | – | | 7200 | 16 | – |
| 300 | 384 | – | | 9600 | 12 | – |
| 600 | 192 | – | | 19200 | 6 | – |
| 1200 | 96 | – | | 38400 | 3 | – |
| 1800 | 64 | – | | 56000 | 2 | 2.86 |

Every row closes against `115 200 / divisor`. Divisor 2 is really 57 600 baud; the manual's
"56K" is that rate with a 2.86 % error.

## 6. What MOSS does to the 8250 (Appendix C, pp. C-18, C-19, C-24, C-25)

The listing is the authority for this section.

### 6.1 Initialization and auto-baud — `I8250`, `F49Fh`

Run once at cold start and again by the `Y` command.

1. `OUT 24h` ← `0Fh`. Modem control: DTR, RTS, OUT 1 and OUT 2 all on — the connector's DSR,
   CTS and RLSD are asserted.
2. **Wait for a start bit:** loop on `IN 26h` until bit 6 (`40h`) is 1.
3. **Time it:** count loop passes (`IN 26h` / `INX H` / `ANA E` / `ANA E` / `JNZ`) while bit 6
   stays 1. A carriage return (`0Dh`) has bit 0 = 1, so the spacing run is the start bit alone:
   the count `c` is one bit time.
4. Make the "2 MHz divisor": `d = 2c + 2 × (high byte of 2c)`.
5. Waste about 8 bit times in a loop of `4d` passes that reads `IN 20h` — the rest of the
   character goes by.
6. `OUT 23h` ← `83h` (DLAB = 1, 8 bits, 1 stop, no parity). `OUT 20h` ← divisor low,
   `OUT 21h` ← divisor high. `OUT 23h` ← `03h`. `OUT 21h` ← `00h` (interrupts off).
   `OUT 25h` ← `00h` (clear the line status flags).
7. Read one character with `TTYIN` and mask it to 7 bits. If it is `0Dh`, done.
8. If not, "must be 4 MHz system": the divisor becomes `c × 5/4` and step 6 runs again, then
   step 7.

So the operator types **two carriage returns at 2 MHz and three at 4 MHz** (listing comment
p. C-18: "either 2 or 3"; §3.4: "the maximum … is three"). If the character in step 7 is never
a carriage return, the routine loops, multiplying by 5/4 each time.

After it returns, cold start prints `MOSS VERS 2.2` and the `-` prompt. The manual gives the
range as "any baud rate between 2 and 56K" (pp. 3-3, 3-17).

**Arithmetic check (ours, not the manual's).** The timing loop is 35 T-states. At 2 MHz that
is 17.5 µs a pass, so `c ≈ 57 143 / baud` and the right divisor `115 200 / baud` is `2.016 c`
— step 4 gives `2.008 c`. At 4 MHz the eight ROM fetches in the loop each take one wait state
(p. 4-9), so a pass is 43 T-states = 10.75 µs, `c ≈ 93 023 / baud`, and the right divisor is
`1.238 c` — step 8 gives `1.25 c`. Both close, and the second confirms the ROM wait state at
4 MHz.

### 6.2 Console drivers (p. C-24, C-25)

| Routine | Address | Code | Result |
|---|---|---|---|
| `TTST` (input status) | `F6C6h` | `IN 25h`; `ANI 01h`; `RZ`; `ADI 0FEh` | A = `00h` no data, `FFh` data ready |
| `TTYIN` | `F6CEh` | loop `IN 25h` until bit 0 = 1; `IN 20h` | A = the byte, **all 8 bits** |
| `TTOST` (output status) | `F6D6h` | `IN 25h`; `ANI 20h`; `RZ`; `ADI 0BFh` | A = `00h` busy, non-zero ready |
| `TTYOUT` | `F6DEh` | loop `IN 25h` until bit 5 = 1; `OUT 20h` ← C | sends C |

MOSS polls **Data Ready (bit 0)** and **THRE (bit 5)** only. It never reads the error bits, the
modem status levels or the delta bits after start-up, and never changes the handshake outputs.

## 7. The MOSS 2.2 monitor (Chapter 3; listing Appendix C)

### 7.1 Entry points (p. 3-2, C-3, C-4)

| Address | Name | Use |
|---|---|---|
| `F000h` | `CBOOT` | Cold start |
| `F003h` | `CONIN` | Console input → A (8 bits; **not** stripped) |
| `F006h` | `READER` | Reader input → A |
| `F009h` | `CONOUT` | Console output ← C |
| `F00Ch` | `PUNCH` | Punch output ← C |
| `F00Fh` | `LIST` | List output ← C |
| `F012h` | `CONST` | Console status → A (`00h` / `FFh`) |
| `F015h` | `IOCHK` | A ← IOBYTE |
| `F018h` | `IOSET` | IOBYTE ← C |
| `F01Bh` | `MEMCK` | Top of contiguous RAM less the work space → B (high), A (low) |
| `F01Eh`, `F021h` | | Jump to a bare `RET` — places for user I/O definition and control |
| `F024h` | `REST` | Breakpoint entry: saves all registers |
| `F10Fh` | `WINIT` | Warm start: resets the stack and the vector at `0000h` |
| `F68Fh` | `CONI` | Console input with bit 7 stripped — the CP/M-style input |

### 7.2 Memory use

- **Cold start writes `0000h`–`003Eh`**: sixteen copies of `C3 B2 F6 00` — `JMP F6B2h`
  (`RSTER`, prints `RST ERR`) at every restart address, with a `00h` after each. The `00h` at
  `0003h` **is the IOBYTE default**. Warm start then puts `JMP F10Fh` at `0000h`.
- Breakpoints use `RST 1`: the monitor puts `CFh` at the breakpoint and `JMP REST` at `0008h`.
  Two breakpoints at most; they work only in RAM and only on an opcode byte.
- `MEMSIZ` (`F06Fh`) finds the top of RAM by testing one byte in each 256-byte page from page 0
  up (complement, compare, restore). It stops at the first page that fails or at page `F0h`.
  RAM must be contiguous from `0000h`.
- The work area sits at the top of that RAM: 56 bytes for breakpoints, saved registers and the
  register-restore routine (copied down from the ROM), with the stack below. The manual says to
  reserve 88 bytes (p. 3-1).

### 7.3 IOBYTE (`0003h`) — fields from the listing (pp. C-8, C-23)

| Bits | Device | 0 | 1 | 2 | 3 |
|---|---|---|---|---|---|
| 1–0 | Console (`C`) | `T` teletype | `C` CRT | `B` batch (reader in, list out) | `1` user |
| 3–2 | Reader (`R`) | `T` teletype | `P` paper tape reader | `1` user 1 | `2` user 2 |
| 5–4 | Punch (`P`) | `T` teletype | `P` high-speed punch | `1` user 1 | `2` user 2 |
| 7–6 | List (`L`) | `T` teletype | `L` (CRT in CP/M) | `1` (line printer in CP/M) | `2` (user list 1 in CP/M) |

**Only value 0, "teletype", has a driver, and it is the 8250 port** for all four devices.
Every other value is equated to `IOER` (`F462h`): it sets IOBYTE to 0, prints `I/O ERR` and
warm-starts. The one exception is console = batch, which falls through to the list and reader
routines. Drivers are added by overlaying the ROM through PHANTOM* (the CCS 2422 floppy
controller does this) or by reprogramming the jump operands in the ROM.

### 7.4 Commands (§3.5–3.7; dispatch table `F027h`, p. C-4)

Prompt `-`. One letter, then hex values with no space after the letter; space or comma
separates values, carriage return ends. Only the last four (address) or two (data) digits typed
count. A bad entry prints `????` and warm-starts.

| Cmd | Form | Action |
|---|---|---|
| `A` | `-AX` then `-Y` | Assign: logical device `C`/`R`/`P`/`L`, then a physical code from §7.3 |
| `D` | `-DA1 A2` | Display memory in hex and ASCII, 16 bytes a line |
| `E` | `-EA L` | Punch an Intel hex end-of-file record, entry address A, then L tenths of an inch of nulls (default 60) |
| `F` | `-FA1 A2 C` | Fill |
| `G` | `-GA B1 B2` | Go, with up to two breakpoints. `-G` alone resumes at the saved PC. A breakpoint prints `*` and the address |
| `H` | `-HA1 A2` | Print the sum and the difference |
| `I` | `-IA` | Read port A, print it in binary |
| `L` | `-LH` | Punch H tenths of an inch of nulls (default 60) |
| `M` | `-MA1 A2 AD` | Move, lowest address first |
| `O` | `-OA D` | Write D to port A |
| `Q` | `-Q` | Show the assignments as `C-X R-X P-X L-X` |
| `R` | `-RB` | Read an Intel hex tape, adding bias B; verifies each byte written; jumps to the entry address if it is not 0 |
| `S` | `-SA,` | Examine and change memory. Space or comma = next, carriage return = stop, line feed = previous |
| `T` | `-TA1 A2` | Memory test (complement and restore); prints the address and the bad bits |
| `V` | `-VA1 A2 AD` | Compare two blocks |
| `W` | `-WA1 A2 R` | Punch Intel hex, R bytes a record (default 16) |
| `X` | `-X` / `-Xr` / `-X'r` | Show or change the saved Z-80 registers; `'` selects the alternate set and X, Y, R |
| `Y` | `-Y` | Run the auto-baud routine again (§6.1) |
| `Z` | `-Z` | "Zleep": ignore everything until two bell characters (`07h`) in a row |

In `D`, `T` and `V`, Control-S (`13h`) freezes the output and any other key stops the command.
The letters B, C, J, K, N, P and U are not commands.

## 8. CPU theory an emulator needs (§4.1, pp. 4-1 to 4-10)

- **Clocks.** 16 MHz crystal divided to 8, 4 and 2 MHz. Bus `CLOCK` (pin 49) is **always
  2 MHz**. φ1 and φ2 follow the CPU speed.
- **Reset.** `POC*` goes low about 50 ms after power-on and pulls `pRESET*` and `EXT CLR*` low.
- **Power-on jump** (§4.1.9). After reset the board itself feeds the Z-80 `C3h` on the first M1
  cycle and the low then high jumper bytes on the next two reads, with the Data In bus off. The
  fourth read (the first fetch at the target) turns the circuit off until the next reset.
- **ROM enable** (§4.1.8). A 256 × 4 decode ROM (U9, 5623) selects the 2716 when A15–A11 are
  `F0h`–`F7h`, MREQ* is active and PHANTOM* is not.
- **Wait** (§4.1.7). The Z-80 waits when XRDY is low, when pRDY is low, on M1 with the WAIT
  jumper on, or on a ROM access at 4 MHz. The last two give exactly one wait state.
- **Address mirror** (§4.1.3). With ADD MIR on, A0–A7 are copied to A8–A15 while IOREQ* is
  active and M1* is not. Without it the Z-80's own high byte is on A8–A15 during I/O.
- **Status byte** on the Data Out bus during `pSYNC` (Table 4-1, p. 4-7):

| Cycle | D7 MEMR | D6 INP | D5 M1 | D4 OUT | D3 HLTA | D2 | D1 WO* | D0 INTA |
|---|---|---|---|---|---|---|---|---|
| Instruction fetch | 1 | 0 | 1 | 0 | 0 | x | 1 | 0 |
| Memory read | 1 | 0 | 0 | 0 | 0 | x | 1 | 0 |
| Memory write | 0 | 0 | 0 | 0 | 0 | x | 0 | 0 |
| Input read | 0 | 1 | 0 | 0 | 0 | x | 1 | 0 |
| Output write | 0 | 0 | 0 | 1 | 0 | x | 0 | 0 |
| Interrupt acknowledge | 0 | 0 | 1 | 0 | 0 | x | 1 | 1 |
| Halt acknowledge | 1 | 0 | 0 | 0 | 1 | x | 1 | 0 |

  D2 is the 8080's STACK bit; **the 2810 does not generate `sSTACK`**.
- **`pINTE`** is rebuilt by a flip-flop that watches for the `EI` (`FBh`) and `DI` (`F3h`)
  opcodes on an M1 cycle and is cleared by reset or interrupt acknowledge.
- **`pSYNC`** comes in T2, not T1, on an I/O cycle. `pDBIN` is RD* or `sINTA`. `pHLDA` is BUSAK*
  inverted.
- **Bus pins that differ from an 8080 CPU board** (p. A-8): 12 `NMI*`, 65 `MREQ*`, 66
  `REFRESH*`, 67 `PHANTOM*`, 98 `2*/4 MHZ` (in place of `sSTACK`). All five are jumper- or
  wire-enabled.
- J3 is the front-panel data socket (Altair 8800 and IMSAI, §2.3).

## 9. Parts (pp. D-3, D-4)

| Ref | Part |
|---|---|
| U5 | 8250 |
| U8 | 2716, 2048 × 8 EPROM (MOSS 2.2) |
| U9 | 5623, 256 × 4 ROM (address decode) |
| U10 | Z-80 |
| U2, U3 | 75150 line drivers |
| U4 | 75154 line receiver |
| U24 | 74LS161 (clock divider) |
| U19, U20, U46 | 78L12, 79L12, 7805 regulators |
| U47, U49 | 75453 |
| U48 | 555 |
| Y1 | 1.8432 MHz crystal |
| Y2 | 16.000 MHz crystal |

## Quirks worth carrying

- ⚠ **The console does not come up unless modem status bit 6 follows the receive line.** MOSS
  finds the baud rate by timing a start bit on the 8250's RI input, where the board wires the
  serial data. An emulator that keeps bit 6 at 0 hangs at `F4A8h` forever. Table B-2 says the
  bit is 0; the firmware and p. 4-12 say otherwise.
- ⚠ **Every modem bit has two names.** CCS's tables use the connector's (DCE) names, the
  firmware and any 8250 data sheet use the chip's. Modem control bit 0 is the chip's DTR and the
  connector's DSR; modem status bit 4 is the chip's CTS and the connector's RTS. Table 4-3 is
  the key.
- ⚠ **"Peripheral Control" and "Peripheral Status" are modem control and modem status.**
- ⚠ **The line status register is written**, with 0, to clear it.
- ⚠ **`CONIN` (`F003h`) returns 8 bits**; `CONI` (`F68Fh`) strips bit 7. The manual points
  CP/M at `F68Fh` (p. 3-3).
- ⚠ **Cold start overwrites `0000h`–`003Eh`** and depends on the `00h` it leaves at `0003h` for
  the IOBYTE.
- ⚠ **Seven ports in the text, eight in the decode** (§3).
- ⚠ **The 4 MHz ROM wait state is built into the auto-baud constant** (§6.1). A model that
  runs the ROM with no wait state at 4 MHz gets a count that the `5/4` factor does not fit.
- ⚠ Cross-references in the manual are wrong in three places: Chapter 2's opening sends the
  reader to "section 4.2" and "4.3" for the serial and front-panel setup (they are 2.2 and 2.3)
  and to "Appendix C" for the board layout (it is Appendix D); §3.7.1 cites PHANTOM as
  "section 2.1.8" (it is 2.1.7).
- ⚠ §2.2.2 describes the console format as "8 data bits, no parity bit, a 0 stick bit and one
  stop bit". The firmware's `03h` is plain 8-N-1, stick parity off.
- ⚠ Not in this manual at all: the Interrupt Identification register and the 8250's reset state
  (both are in [the data sheet](INS8250.md)), what its interrupt pin and `OUT 2` connect to, and which jumper block is the high byte of the
  power-on jump address.
