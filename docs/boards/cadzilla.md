# CADzilla — an HD63484 ACRTC graphics board with a Bt453 RAMDAC

**Status:** done — every ACRTC command in every operation, color and area mode; scan-out at 8 bpp
through the Bt453 onto a fixed VESA monitor; data-sheet drawing time with `draw_rate = real`.
What is not modeled is under *Limitations*.

## The real hardware

CADzilla is **a new custom product based on period components**, not a period product. It is
based on two vintage chips of the mid 1980s the way a CAD-station graphics board of that era was.

- **Hitachi HD63484 ACRTC** (Advanced CRT Controller, 1984). A CRT controller with a
  microcoded *drawing processor* and its own frame-buffer interface: up to 2 MB of memory on the
  board that the host never addresses — CADzilla fits 2 MB of **SRAM**. The host sets ~30
  timing and display registers, then draws by writing 16-bit *commands* into an 8-word FIFO — move, line, rectangle,
  polyline, polygon, circle, ellipse, arc, paint, pattern, copy — in logical X-Y pixel coordinates
  at 1, 2, 4, 8 or 16 bits per pixel. Four logical screens (upper, base, lower, window), zoom,
  cursors, a light pen, DMA to the host. Interfaces as an 8- or 16-bit peripheral occupying **two I/O
  locations**, selected by its RS pin.
- **Brooktree Bt453** (1986). A 256 × 24 color-palette RAM with three 8-bit video DACs. Eight
  pixel inputs (P0–P7) select one of its 256 entries, each set to any of 16.7M colors; two
  overlay inputs (OL0–OL1) select one of three overlay colors instead; an 8-bit MPU port with
  two command inputs, so it occupies **four I/O locations**.

Board design decisions:

| Decision | CADzilla |
|---|---|
| ACRTC bus mode | 8-bit (DACK\* strapped at reset via SW1-6 held Off), the only mode an S-100 8080/Z80 can use |
| **I/O map** | ONE 8-port block from `port` (default `0x70`, a multiple of 8): `+0` the ACRTC's address/status register RS=0, `+1` the board's own **MODE register** (write-only), `+2` the ACRTC's Command/FIFO low data lane RS=1 (in 8-bit mode both bytes of a word pass through it, high byte first), `+3` upper byte of 16 bit Command/FIFO in 16 bit mode, `+4`..`+7` the Bt453. The ACRTC's address/status port utilizes only the lower byte, and will never support a 16 bit transfer. This allows the MODE register to occupy `+1`, leaving `+2` for 8 bit Data/FIFO access, and `+2` and `+3` for 16 bit access. An 8080 or Z80 makes only 8-bit I/O cycles, so here `+3` is never used and nothing answers it |
| **MODE register** (`port+1`, write-only) | the board's own glue-logic strap, not a register on either chip: `HSPOL`/`VSPOL` sync polarity, `AMODE` the access mode the board's *own* fetch logic runs (single/interleaved — must agree with the ACRTC's own OMR ACM bit, or `wiring` says so), `OLEN` overlay display enable, `OLSEL` which half of the overlay SRAM the ACRTC draws. Bit layout below |
| **Shift register** | wired for **8 bits per pixel and 8 words per display fetch**: the frame memory is eight SRAM banks side by side (drawing word *W* is bank *W* & 7, row *W* >> 3), and each display cycle the board reads row *A* >> 3 of all eight banks at once — words (*A* & ~7) + 0..7, MA2..0 ignored — and shifts them out as sixteen 8-bit pixels, low byte of the low word first. This is a hardware fact, not a register — see *Programming model* below |
| **Overlay** | a **one-bit plane in its own 128 K-word SRAM**, one bit per frame pixel, shifted into the Bt453's OL1 beside each frame fetch; OL0 is tied low, so a set bit shows **overlay color 2**. The ACRTC draws it through a **CHR screen** — see *The overlay* below |
| **Monitor** | a fixed-frequency VESA display chosen by the `mode` strap: the three primary VESA resolutions -- **640x480, 800x600, 1024x768 (default)** -- settable in the machine file or with `SET`. The frame is always the mode's size; the ACRTC's picture is placed in it by HDS/VDS against the mode's porches |
| **Access modes** | single and interleaved, selected by **MODE AMODE** (not read from the ACRTC's own OMR ACM bit — the two are independent straps a driver must set in agreement). Superimposed is not wired |
| **Frame memory** | **fixed at 2 MB of SRAM** — the reference design's own fit, the full 1 M words the ACRTC addresses. |
| **IRQ\*** | strapped by `interrupt` (SW1-8): `none` (default, SW1-8 Off) disconnects it; `int` or `vi0`..`vi7` (SW1-8 On) raises that S-100 line whenever `acrtc_.irq()` — an enabled SR flag — is true |
| **2CLK** | the ACRTC's clock comes from the monitor's pixel clock (PCLK, the VESA rate of `mode`): **PCLK/8 in single access mode, PCLK/4 in interleaved**, selected by MODE `AMODE`. A memory cycle is then 16 pixels single and 8 interleaved |
| **Drawing time** | the `draw_rate` strap: `full` (default) draws in no time; `real` gives each command its data-sheet time — see *Drawing time* below |

### The MODE register (`port+1`, write-only)

| Bit | Field | Meaning |
|---|---|---|
| 0 | `HSPOL` | horizontal sync polarity: 0 = positive sync pulse, 1 = negative |
| 1 | `VSPOL` | vertical sync polarity: 0 = positive sync pulse, 1 = negative |
| 2 | `AMODE` | access mode the board's own fetch logic runs: 0 = single, 1 = interleaved |
| 3 | `OLEN` | overlay display enable: 0 = the overlay is not shown (drawing into it still works) |
| 4 | `OLSEL` | the overlay SRAM's A16 on drawing cycles: which 64 K-word half a CHR screen draws |
| 5–7 | — | not connected |

The register is a 74LS273 that bus RESET\* clears to `00`. Bits 0–3 were first placed by this
implementation; the board's schematic agrees with them, and adds `OLSEL` at bit 4. The
register is write-only on the wire, like the Dazzler's format port, so a driver keeps its own
copy; `SHOW <id>` decodes it back into five read-only status lines (`hspol`, `vspol`, `amode`,
`olen`, `olsel`) the same way the Dazzler turns its write-only control/format bytes into
`video`/`resolution`/`color`.

`HSPOL`/`VSPOL` are recorded and reported but drive nothing else — this model has no separate
sync-pulse signal for a polarity to invert (see Limitations).

### The overlay

The board has a second, one-bit plane on top of the 8-bit picture: **the overlay SRAM**,
128 K words of 16 bits, **one bit for every pixel of the 2 MB frame memory**. A set bit shows
the Bt453's **overlay color 2**, whatever the pixel's own value is; a clear bit lets the
palette show through.

**On the screen**, the overlay has no display cycles of its own. Each display fetch at
address *A* also reads overlay word *A* >> 3 — the sixteen bits of exactly the sixteen pixels
that fetch shows, bit 0 the leftmost. The bits go through their own shift register into the
Bt453's OL1; OL0 is tied low, so the overlay can select only overlay color 2 (colors 1 and 3
are never used). So frame pixel *p* — *p* = 2 × word address + byte — is overlay word
*p* >> 4, bit *p* & 15. **MODE `OLEN`** loads the overlay shift register: with `OLEN` = 0 the
overlay is transparent.

**Drawing into it**, the ACRTC uses a screen whose memory-width register has **CHR = 1**
(MWR bit 15). The ACRTC drives its CHR pin high for every drawing access to that screen; the
board then enables no frame-memory bank and sends the access to the overlay SRAM at MAD0–15.
A character screen has only 64 K words (its MA16–19 carry a raster address), so **MODE
`OLSEL`** is the overlay's seventeenth address bit: overlay word *O* is drawing address
*O* & `FFFF` with `OLSEL` = *O* >> 16. `OLSEL` = 1 reaches the half of the overlay for the
upper 1 MB of the frame memory.

What a program does, as far as the board decides it:

1. Load overlay color 2 into the Bt453: `02` to `port+4`, then R, G, B to `port+7`.
2. Set MODE `OLEN` (bit 3), and `OLSEL` (bit 4) for the half it will draw.
3. Draw into a screen defined with MWR CHR = 1. **Never enable that screen for display.**

Which screen to use, and how to set it up, is the program's choice. A layout that makes
overlay (X, Y) the same as frame (X, Y): one bit per pixel, so the overlay screen's memory
width is the frame's width / 16 words, CCR GBM = 000 (1 bpp) while drawing it — GBM is one
setting for all screens, so set it back to 011 (8 bpp) before the next frame drawing — and the
overlay's ORG on the overlay word of the frame's own ORG pixel.

| If you get it wrong | You see |
|---|---|
| `OLEN` = 0 | no overlay |
| the CHR screen enabled for display | the board ignores CHR on a display cycle, so that screen shows frame memory at its address — `wiring` names it |
| drawing with CHR = 0 | the drawing lands in the frame memory |
| the wrong `OLSEL` | the overlay appears for the other half of the frame memory |

### The programmed picture uses the board's own AMODE, not the chip's

`programmedWidth()`, `programmedX()` and the paint loop all read **MODE AMODE**, not the
ACRTC's OMR ACM bit — the board's external fetch logic cannot see inside the chip's register
file, so it needs its own strap, exactly as `wiring()` already treats GBM and GAI as things a
driver must set in agreement with fixed board behavior. If OMR ACM and MODE AMODE disagree,
`wiring` names it (`"OMR ACM is interleaved, MODE AMODE says single"`) and the picture follows
MODE, not OMR — that is what "the board's own fetch logic" means concretely.

### The monitor modes

VESA timings in the board's units — memory cycles of 16 pixels (single access mode) and
rasters. Where a VESA porch is not a whole number of cycles the board's timing rounds it and
keeps the line total, as a timing PROM on the board would.

| `mode` | Pixel clock | H: sync / back porch / active / front porch (cycles) | V: sync / back porch / active / front porch (rasters) |
|---|---|---|---|
| `640x480` | 25.175 MHz | 6 / 3 / 40 / 1 = 50 | 2 / 33 / 480 / 10 = 525 |
| `800x600` | 40.000 MHz | 8 / 6 / 50 / 2 = 66 | 4 / 23 / 600 / 1 = 628 |
| `1024x768` | 65.000 MHz | 8 / 10 / 64 / 2 = 84 | 6 / 29 / 768 / 3 = 806 |

The register values that put a full picture in each frame (single access mode; the manual's
"set to N−1" already applied). For interleaved mode double `HC`, `HSW`, `HDS` and `HDW`
(`HC = 2·total − 1`, `HSW = 2·sync`, `HDS = 2·back porch − 1`, `HDW = 2·active − 1`) and set
ACM = `10`:

| `mode` | `HSR` r82 (HC, HSW) | `HDR` r84 (HDS, HDW) | `VSR` r86 (VC) | `VDR` r88 (VDS, VSW) | `SSW` r8A (SP1) | `MWR1` rCA |
|---|---|---|---|---|---|---|
| `640x480` | `$3106` (49, 6) | `$0227` (2, 39) | `$020D` (525) | `$2002` (32, 2) | `$01E0` (480) | `$0140` (320) |
| `800x600` | `$4108` (65, 8) | `$0531` (5, 49) | `$0274` (628) | `$1604` (22, 4) | `$0258` (600) | `$0190` (400) |
| `1024x768` | `$5308` (83, 8) | `$093F` (9, 63) | `$0326` (806) | `$1C06` (28, 6) | `$0300` (768) | `$0200` (512) |

Then `CCR = $0300` (8 bpp), `OMR = $4030` (STR, GAI +8; `$4038` interleaved), `DCR = $4000`
(SE1), `SAR1 = 0`, and an `ORG` wherever the program wants its origin — on the bottom raster
(`DPA = (height−1)·MW`) if it wants +Y to be up on the screen.

**Placement.** The ACRTC's display starts `HDS` memory cycles after HSYNC's rising edge; the
monitor's picture starts a fixed number of cycles after the same edge — the mode's back porch.
A picture programmed with `HDS = back porch − 1` fills the frame from the left edge; one cycle
later it is 16 pixels to the right (8 in interleaved mode) with its last cycle off the edge, one
cycle earlier it is clipped on the left. Vertically the same with `VDS` and the vertical back
porch. `SHOW`'s `picture` line reports the programmed size and where its top-left corner lands.

### Drawing time (`draw_rate`)

`draw_rate = "full"` (the default) is the fastest bench for writing a program: every command is
done the moment its last parameter lands, `WFE` and `CED` are always set, and the CPU never
waits on the chip. `draw_rate = "real"` gives each command the time the HD63484 data sheet
gives it (Table 3, in 2CLK cycles), so a timing-sensitive program — a game — sees the chip it
was written for: the words behind a command wait in the write FIFO (`WFE` clears; at 8 words
`WFR` clears too), and `CED`, any SR flag the command raised and any word it answers
(RD, RPR, RPTN, DRD) appear only when its time is up. Set it in the machine file or with
`SET cad0 draw_rate=real`.

**Where the time goes.** A command of N 2CLK needs N/2 memory cycles of drawing, and the
display takes its own share first. The ACRTC's own timing registers and `OMR` decide which
memory cycles are free:

| Memory cycle | Free for drawing |
|---|---|
| the last cycle of HSYNC low (the attribute output cycle) | never |
| the rest of HSYNC low (the DRAM refresh period) | only with `OMR` RAM = 1 (static RAM mode — right for this board's SRAM) |
| the display period (the background's HDS..HDS+HDW, the window's HWS..HWS+HWW) | single access, ACP = 0 (display priority): none. ACP = 1 (drawing priority): all. Interleaved: every second cycle |
| the rest of the retrace | always |

So the same command is fastest in interleaved mode, or in single access mode with drawing
priority, and slowest in single access mode with display priority, where it draws only in the
retrace. A chip that is not started (`OMR` STR = 0) draws in every cycle.

**2CLK per mode**:

| `mode` | Pixel clock | 2CLK, single (PCLK/8) | 2CLK, interleaved (PCLK/4) |
|---|---|---|---|
| `640x480` | 25.175 MHz | 3.147 MHz | 6.294 MHz |
| `800x600` | 40.000 MHz | 5.000 MHz | 10.000 MHz |
| `1024x768` | 65.000 MHz | 8.125 MHz | 16.250 MHz |

The design intends interleaved access at 640x480, and single access at 800x600 and 1024x768.
Nothing enforces that: the board times what it is programmed to do.

**It is not the CPU's `clock_hz`.** `clock_hz = 0` (flat out) only stops the host from waiting;
the guest cannot see it. The drawing time is emulated time, turned into T-states at the CPU's
rate (2 MHz when flat out), so a command costs the same number of instructions at any
`clock_hz`. A flat-out CPU runs the whole program — drawing waits included — faster in wall-clock
time, and the program's own timing stays right.

**What the data sheet leaves open, and what this model assumes:** Table 3's counts are taken as
the drawing processor's time when it gets every memory cycle it asks for. L and d (the dots of a
line or curve) and A × B (a filled area) are the logical pixel positions the command actually
visited, clipped and suppressed ones included. PAINT's formula (exact only for a rectangle,
Table 3 note 2) is applied with its painted pixels and spans. In interleaved mode every
retrace cycle is free, as in single access mode. A command's end is fixed when it starts, so a
timing register written while it runs changes the next command, not that one. The raster
counter (`r80`) still reads 0.

## Sources

| Source | Path | Authority |
|---|---|---|
| Hitachi *HD63484 ACRTC* datasheet, 47 pp. | `reference/Hitachi HD63484 ACRTC.md` | **Authoritative** for the register file (Figure 5), the drawing parameter registers (Table 1), the opcode table (Table 4) and the status register |
| Hitachi *HD63484 ACRTC User's Manual*, #U75, Nov 1984 | `reference/Hitachi HD63484 ACRTC User's Manual.md` | **Authoritative for semantics**: 8-bit mode byte order (§5.2, §6.2), the FIFO and status rules (§5.3), GBM/OMR/DCR field meanings (§5.5–5.7), timing and display-control RAM (§5.8–5.9), the operation / color / area modes (§6.6) and every command's worked example (ORG-3, CLR-3/4, WPR-2, RPR-2, WPTN-2, RPTN-2, ALINE-2, ARCT-2, AFRCT-1, DRD/DWT-2). Transcription stops at source page 308 — Sections 1–5 and all 38 commands are covered; the Electrical Specification section (timing, not needed here) is not |
| Hitachi *HD63484 ACRTC Application Note* #U90, April 1986 | bitsavers | The reset state (Table 7-1) and the initialization sequence (§7.2) |
| Brooktree *Bt453*, L453001 Rev. J | `reference/Brooktree Bt453 RAMDAC.md` | **Authoritative** for the four locations (Table 1), the address/phase logic (Table 2) and the pixel/overlay truth table (Table 3) |
| MAME `src/devices/video/hd63484.cpp` | `LICENSE-MAME-HD63484` | **Structural basis only** of the ACRTC model, per the terms in `docs/sources.md`. Where it and the Hitachi documents disagreed, the documents won — see Quirks |

## Register reference

One 8-port I/O block, no memory: everything at `port` (a multiple of 8, default `0x70`).
The 2 MB frame memory is not in this table — it is fixed, private to the ACRTC, and the CPU
never addresses it directly; see *The real hardware* above.

| Addr | OUT (write) | IN (read) |
|---|---|---|
| `BASE+0` | **Address register (AR)**: which control register `BASE+2` reaches (`r00`–`rFF`) | **Status register (SR)**: `CER ARD CED LPD RFF RFR WFR WFE` (D7..D0) |
| `BASE+1` | **MODE register**: `HSPOL`(0) `VSPOL`(1) `AMODE`(2) `OLEN`(3) `OLSEL`(4), bits 5–7 not connected. Write-only | floats — MODE cannot be read back |
| `BASE+2` | the control register AR names, **one byte**: even AR = high byte, odd AR = low byte. AR = 0/1: a **command word into the write FIFO**, high byte then low | the same register's byte; AR = 0/1: the next byte **out of the read FIFO**, high then low |
| `BASE+3` | High byte of the Command/FIFO word write if 16 bit I/O is requested and enabled, AR = 0. Never on an 8080/Z80: not decoded here | High byte of the Command/FIFO word read if 16 bit I/O is requested and enabled, AR = 0. Never on an 8080/Z80: not decoded here |
| `BASE+4` | Bt453 address register (which LUT entry the next R,G,B cycles touch); resets the color phase to red | the address register |
| `BASE+5` | color palette RAM: red, then green, then blue; the entry is written on the blue cycle and the address increments | R, G, B of the entry, one per read; increments after blue |
| `BASE+6` | the address register again (an alias; there is no separate read-mode register on this part) | the address register |
| `BASE+7` | overlay registers 1–3 (ADDR1..0 = 01, 10, 11), same R,G,B protocol; the board shows only overlay color 2 | the same |

The ACRTC's own RS=0/RS=1 pair is deliberately **not adjacent**: MODE sits between them at
`BASE+1`, reflecting a real address-bit decode (A1 is the ACRTC's RS pin, A0 within the low
half of the block picks MODE vs. the ACRTC), not an accident of layout. The ACRTC's
address/status port utilizes only the lower byte, and will never support a 16 bit transfer. This
allows the MODE register to occupy `+1`, leaving `+2` for 8 bit Data/FIFO access, and `+2` and
`+3` for 16 bit access.

**Status register.** `WFE` write FIFO empty (up to 8 words may be written), `WFR` write FIFO
not full (one word may), `RFR` read FIFO has a word, `RFF` read FIFO full, `CED` command end —
the chip will take a new command (cleared when one is written), `ARD` area detect (set by the
AREA modes, cleared by RPR or ABT), `CER` command error — an undefined opcode or a bad parameter
(cleared only by ABT). After RESET\* and
after ABT: `$23`.

**Control registers** (through AR). `r02` CCR — bit 15 ABT, 14 PSE, 13 DDM, 12 CDM, 11 DRC,
10–8 **GBM** (000 = 1 bpp, 001 = 2, 010 = 4, 011 = 8, 100 = 16), 7–0 interrupt enables matching
the SR bits. `r04` OMR — bit 15 M/S, **14 STR** (start), 13 ACP, 12 WSS, 11–10 CSK, 9–8 DSK,
7 RAM, **6–4 GAI** (words per display cycle: 000 = 1, 001 = 2, 010 = 4, 011 = 8), 3–2 ACM,
1–0 RSM. `r06` DCR — bit 15 DSP, **14 SE1** (base screen shown), 13–12 SE0 (upper: 1x
enabled, 11 shown), 11–10 SE2 (lower), 9–8 SE3 (window), 7–0 ATR. Timing RAM `r80`–`r9F`:
`r84` HDR (HDS `<<8` | HDW, both in memory cycles, value = cycles − 1), `r88` VDR (VDS `<<8` |
VSW), `r8A`/`r8C`/`r8E` SP1/SP0/SP2 (rasters of the base/upper/lower screens), `r92` HWR,
`r94`/`r96` VWS/VWW. Display-control RAM `rC0`–`rEF`: per screen *n* (0 upper, 1 base, 2 lower,
3 window) `rC2+8n` MWR (bit 15 CHR, 11–0 memory width in words), `rC4+8n`/`rC6+8n` SAR (start
address high nibble / low word). Accesses to `r80`–`rFF` auto-increment AR by one; `r00`–`r7F`
do not.

**Commands** (16-bit words into the FIFO, then their parameters; manual Figure 6.1):

| Opcode | Mnemonic | Parameters | What it does |
|---|---|---|---|
| `$0400` | ORG | DPH, DPL | set the origin: screen number, word address, dot address; CP = (0,0) |
| `$0800+RN` | WPR | D | write drawing-parameter register RN (CL0 `00`, CL1 `01`, CCMP `02`, EDG `03`, MASK `04`, PRC `05`–`07`, area `08`–`0B`, RWP `0C`–`0D`) |
| `$0C00+RN` | RPR | — | read it into the read FIFO (DP `10`–`11` and CP `12`–`13` too); clears ARD |
| `$1800+PRA` | WPTN | n, D1..Dn | write the pattern RAM from word PRA; **in 8-bit mode n counts bytes** |
| `$1C00+PRA` | RPTN | n | read n pattern words into the read FIFO |
| `$4400` / `$4800` / `$4C00+MM` | RD / WT / MOD | — / D / D | one word at RWP; RWP++ (MOD under MASK) |
| `$5800` / `$5C00+MM` | CLR / SCLR | D, AX, AY | fill (AX+1)×(AY+1) words from RWP; RWPe = RWP's column on the last raster |
| `$2400` / `$2800` / `$2C00+MM` | DRD / DWT / DMOD | AX, AY | a block through the FIFOs under program control |
| `$8000` / `$8400` | AMOVE / RMOVE | X, Y | move CP |
| `$8800` / `$8C00` + fields | ALINE / RLINE | X, Y | line from CP to Pe, **Pe not drawn**, CP = Pe |
| `$9000` / `$9400` + fields | ARCT / RRCT | X, Y | the rectangle CP–(X,Y), X direction first, CP unchanged |
| `$9800` / `$9C00`, `$A000` / `$A400` + fields | APLL / RPLL, APLG / RPLG | n, points | polyline (CP = end) / polygon (closes on CP) |
| `$C000` / `$C400` + fields | AFRCT / RFRCT | X, Y | fill the rectangle with the pattern; CP.y = one past the far edge |
| `$CC00` + fields | DOT | — | the pixel at CP |

The low byte of a graphic command is `AREA` (bits 7–5), `COL` (4–3), `OPM` (2–0): eight
operation modes (replace, OR, AND, EOR, and replace if P = CCMP / P ≠ CCMP / P < CL / P > CL),
four color modes (pattern bit selects CL0/CL1; CL1 or suppress; CL0 or suppress; pattern RAM
direct), eight area modes (none; stop on exit with ARD+CED; suppress outside without / with ARD;
and the same three for *inside*).

## How it is simulated

- **Bus cycles decoded**: `IN`/`OUT` on `port` and `port+2` (the ACRTC's RS=0/RS=1); `OUT`
  only on `port+1` (MODE); `IN`/`OUT` on `port+4`..`port+7` (the Bt453). `port+3`, the high
  byte of a 16-bit transfer, answers nothing: an 8080 or Z80 makes only 8-bit I/O cycles. No
  memory.
- **Two chips, no bus-level timing**: `Hd63484` (`src/chips/hd63484.h`) and `Bt453`
  (`src/chips/bt453.h`) hold all the state; the board forwards RS = A0 to one and C1C0 = A1A0
  to the other. A drawing command executes **at the instant its last parameter lands** — at
  `draw_rate=full` the FIFO drains immediately, so `WFE` is the steady state.
- **The CHR decode**: the board gives the ACRTC its overlay SRAM as the chip's *character
  address space* (`Hd63484::setCharSpace`). A drawing access to a screen whose MWR CHR bit is
  set — ORG's screen for a pixel command, RWP's for a word command — goes there, at the low 16
  address bits with MODE `OLSEL` as bit 16, and never to the frame memory.
- **Drawing time** (`draw_rate=real`): the chip counts time only in 2CLK cycles and is told
  it (`Hd63484::advance`); it keeps the command it is paying for (`busy`/`busyUntil`), holds
  back the SR flags and read-FIFO words it owes, and stops taking FIFO words until the end.
  The board turns the Clock's T-states into 2CLK at PCLK/8 or PCLK/4 (`sync`, with the
  remainder carried so no fraction is lost however often a guest polls), syncs before any
  change of rate (MODE `AMODE`, `mode`), and arms **one** Clock deadline at the command's end
  (`arm`), so an interrupt on CED lands on time with the CPU halted and no bus cycle.
- **`pump()`**: the three gates every video board here uses — did either chip change anything
  (a drawing, a register, a LUT entry, an overlay word that shows)? does the host want a
  frame? — then the board builds the **monitor's** frame in two steps, as the hardware does.
  First its shift registers fill **the RAMDAC's input** for every pixel of the `mode`'s frame
  (`busPixel`: P7..0, OL1..0 and BLANK), all BLANK to start. For every raster it asks the
  ACRTC which background raster (if any) and which window raster fall there
  (`Hd63484::backgroundRaster` / `windowRaster`, VDS and VWS against the mode's vertical back
  porch) and, for each display cycle of `HDW`/`HWW`, reads the aligned eight-word row at the
  ACRTC's address — advancing by `gaiWords()` per display cycle — and the overlay word that goes
  with it when `OLEN` is set, and places sixteen pixels at the cycle's frame x (HDS/HWS against
  the horizontal back porch), clipped to the frame. Then **the Bt453** turns each into a color
  (`Bt453::lookup`, Table 3; BLANK is black) in an `Rgb32` Surface. The palette RAM and the
  overlay registers stay separate, as in the part; the host is handed colors. Off (STR or SE1
  clear) the frame is all BLANK, black -- the monitor is there from power-on, signal or not,
  so the window opens at the prompt like the Dazzler's and shows the picture once a program
  starts the chip.
- **`properties()`**: straps `port` (the whole 8-port block's base), `mode` (the monitor),
  `draw_rate` (`full|real`), `width` (the window), `interrupt` (`none|int|vi0`..`vi7` — SW1-8); live, read-only `video`,
  `picture` (programmed size and position in the frame), `wiring` (GBM/GAI against the board,
  OMR ACM against MODE AMODE, a displayed CHR screen), the MODE register decoded into
  `hspol`/`vspol`/`amode`/`olen`/`olsel`,
  `status` (the SR), and `irq` (whether IRQ\* is asserted right now). Frame memory is not a
  property: it is fixed at 2 MB, with no strap to refit it.
- **Interrupts**: `assertsInt()`/`assertsVi()` read `Hd63484::irq()` — already a pure,
  combinational function of SR against CCR's own enable bits — and gate it on `interrupt`
  (SW1-8): `none`, the default (SW1-8 off), asserts nothing, exactly as an unstrapped IRQ\*
  on the board reaches nothing. `intChanged()` is called after every ACRTC register and
  FIFO access, and on reset, power-on and snapshot restore, so the S-100 interrupt line
  tracks the chip's own request the instant it can move.
- **Snapshot**: both chips — registers, FIFOs, a command in flight, the drawing state, the
  pattern RAM, **the whole frame memory** (no memory board holds it) and the LUT — plus the
  board's own MODE register and **overlay SRAM**, since nothing else holds them either.

### Reset

- `Reset::PowerOn`: the ACRTC's registers, pattern RAM and frame memory zero, then RES\*; the
  overlay SRAM zero; the Bt453's table black, address 0, phase red.
- `Reset::Bus`: RES\* to the ACRTC — SR `$23`, ABT set, STR and M/S clear, FIFOs cleared,
  everything else kept, so the picture blanks (nothing is displaying) and a driver that restarts
  the chip finds its timing intact. The MODE register is glue on the same RESET\* line and is
  cleared too — a driver must reprogram both the ACRTC's timing and MODE after a reset.
  **The Bt453 has no reset pin**: the LUT survives.

## Quirks reproduced

| Quirk | If you get it wrong |
|---|---|
| 8-bit mode: an even AR is the high byte, an odd AR the low byte; `r80`–`rFF` auto-increment, `r00`–`r7F` do not (manual 5.2) | a driver's one-address block load of the timing RAM writes one register 30 times, or its two-write CCR load lands both bytes in one |
| The FIFOs are **8 words**, and WFE/WFR/RFR/RFF track them (manual 5.3) — MAME's model has 16 | a driver that writes eight words on WFE and then polls WFR sees the wrong flag |
| CED clears the moment a command word is written and sets when it ends; CER only ever clears on ABT | a driver polling CED between commands hangs or runs ahead |
| The end point of a line is **not drawn** (ALINE-1); a rectangle's four edges therefore draw each corner exactly once | an EOR-drawn frame leaves its corners lit after being undrawn |
| Dot address *n* is bits `n·bpp` upward from the LSB (ORG-3: DPD=4 at 4 bpp is "bit position 4–7"), and +Y is MW words **lower** in memory | the picture is mirrored, or upside down, or both |
| A line's pattern scan starts at **PPX** (manual 6.8.3) — MAME starts at PSX | a dashed line drawn with a phase offset comes out in the wrong phase |
| The **pattern pointer is live**: PPX/PZCX step once per pixel position a drawing visits (drawn, COL-suppressed or AREA-clipped alike) and are left where they stopped, so the edges of ARCT/APLL/APLG continue one dash pattern and RPR Pr05 reads the advanced pointer; PZCX/PZCY are the zoom counters' *starting* values (manual 5.10.2.6, 6.8.3) | a dashed polyline restarts its dash at every vertex, and a driver that reads Pr05 back to chain patterns gets its own last write |
| A plane fill (AFRCT/RFRCT) starts **every row** from the PPX/PZCX it found and steps PPY once per row; it leaves PPY on the row after its last, so a fill drawn at the new CP continues the tiling. The row restart is inferred from PTN-4, not stated | stacked fills show a seam, or a tile skewed one bit per row |
| AREA 001/101 leave **CP where the drawing crossed** the area boundary, not at Pe ("drawing is executed as long as the CP resides in the defined area", 6.6.3) | a driver that resumes from CP after a hit starts from the wrong place |
| RWPe after CLR/DRD/DWT is RWP's column on the **last raster** (CLR-4: `$56`, AY = −6, MW `$10` → `$B6`) — MAME leaves it one raster further | a driver that clears in strips by chaining CLRs skips or overlaps a raster |
| In 8-bit mode WPTN's *n* counts **bytes**, so it is twice the word count (WPTN-1) | half the pattern loads and the other half is parsed as the next command |
| A DRD "goes into an indefinite wait state after the last transfer" — CED never sets; ABT ends it (manual 6.5) | a driver waiting on CED after a DRD hangs on real silicon and would not here |
| A read that does not fit the 8-word read FIFO (RPTN *n* > 8, RD or RPR onto a full FIFO) **waits for room** (RD-1), and the commands behind it wait in the write FIFO; CED sets when its last word is in | an RPTN of the whole pattern RAM returns half of it, and the command after it runs early |
| **RWP is one register**; DN rides in its high word, so rewriting Pr0C to change screens keeps RWPL (5.10.2.8) | a driver that selects the screen after setting the address transfers to address 0 |
| ELPS's *a* and *b* are **not the semi-axes**: they are the ratio *a* : *b* = dX² : dY² of the squared semi-axes, dX the X one (ELPS-2/3: 9 : 4 with dX = 9 is dY = 6). An arc's radius is CP's own distance from the center; RARC/REARC give both the center and Pe relative to CP | an ellipse comes out the wrong shape, or an arc round the wrong center |
| A curve is drawn **in order** from its start — CCW, or CW when C = 1 — so the pattern runs along it and AREA 001/101 stop where it crosses; CRCL/ELPS start at (A + r, B), go once round, and leave CP at the center; arcs leave CP at Pe, **undrawn** (CRCL-1, AARC-1). Which pixels: the nearest to the curve in each column where it runs mostly horizontally and each row where it runs mostly vertically, so a circle is 8-way symmetric and C changes the order, never the pixels. The manual gives no rule for detecting Pe; an arc ends where Pe's ray crosses it, so a Pe slightly off the curve still ends it | a dashed circle's dashes run backwards, or an EOR-drawn circle leaves its start pixel lit |
| PAINT's edge is EDG (E = 0) or everything but EDG (E = 1) — and **also any pixel already in CL0 or CL1** (PAINT-1), compared at each pixel's own field. It paints 4-connected spans left to right from CP's line and leaves CP just past the last span (Figure C34-4). Its pattern is anchored at CP, and the pointer is left as PAINT found it | a fill color that matches the background paints nothing; a pattern fill shows seams between spans |
| PTN's rows run along **SD × 45°** counter-clockwise from +X; successive rows step 90° further round, or 45° when **SL = 1** (Table C36-1, read from the scan). SL is bit 11 and SD bits 10-8 — the PTN-1 bit diagram is misdrawn, its examples (`$D8XX`, `$D1XX`) are not. A diagonal step moves one pixel in X and Y, so a diagonal PTN is gapped (PTN-9) | a rotated pattern comes out mirrored, or a slanted one sheared the wrong way |
| AGCPY/RGCPY copy in **logical pixels** with CPY's S and DSD scans (Tables C37-1/C37-2), from (Xs, Ys) — relative to CP for RGCPY — to CP, so they turn a block 90° or mirror it; CP ends one line past the last (AGCPY-5: CP (4,2), S = 1, DX = 13 → (4,16)). COL is fixed at 00: the source pixel is the color data, OPM combines it with the destination, AREA judges the destination, and the pattern RAM is not used | a rotated copy comes out transposed the wrong way, or a driver chaining copies from CP lands them on top of each other |
| CPY/SCPY scan the source by **S** (rows or columns, from the corner the signs of AX/AY name) and write the destination from RWP in **DSD**'s order — bit 2 columns, bit 1 leftward, bit 0 downward (Tables C14-1/C14-2, read from the scan) — so S ≠ DSD bit 2 transposes the block; RWPe ends **one line past** the last on the slow axis (CPY-6: `$B0` → `$70`), unlike CLR's | a rotated or mirrored copy comes out in the wrong orientation, or a chained copy overlaps its predecessor by a line |
| DRD/DWT/DMOD with **negative AX/AY** walk the block the way CLR does: leftward, and down in Y (up in memory) (DRD-2, DWT-2) | a bottom-up block transfer is rejected, or lands mirrored |
| The shift register is 8 bpp × 8 words whatever CCR/OMR say: a wrong GBM packs pixels the board will not unpack, a wrong GAI fetches the same aligned row more than once (GAI +1: eight fetches of one row) | an off-spec program shows a coherent picture here and garbage on the board, or the reverse |
| A display fetch reads the **aligned** eight-word row: a start address's low three bits select nothing | a program that scrolls by one word sees the picture move by sixteen pixels every eighth step, and not at all in between |
| BLANK\* overrides the look-up table: every pixel no display fetch reaches is black, whatever palette entry 0 holds | a program that loads a background color into entry 0 sees it only where the ACRTC displays |
| The overlay shows **only overlay color 2** (OL0 is tied low), and only with MODE `OLEN`; it is drawn only through a CHR screen, and `OLSEL` picks its half | a driver that loads overlay color 1 sees nothing; one that draws the overlay on a graphic screen draws into the picture |
| **MODE AMODE, not OMR ACM, governs the board's own fetch pattern** — the two are independent straps a driver must agree, and only `wiring` compares them | a driver that sets the ACRTC to interleaved but forgets `port+1` gets a SINGLE-access picture out of doubled registers: half the frame, or a picture that never fills |
| MODE (`port+1`) is write-only and the ACRTC's own two ports are not adjacent — `port+3` is only the high byte of a 16-bit transfer, which an 8-bit host never makes | a driver probing the block with `IN` for a live register at `+1` or `+3` finds nothing, correctly; one that assumes RS=0/RS=1 are back to back writes its FIFO data to MODE instead |
| In interleaved mode every horizontal register is in doubled units and a memory cycle is 8 frame pixels | the picture is half as wide as intended, or the timing is off by half a line |
| The picture is placed by HDS/VDS against the mode's porches; the frame does not grow to fit | a program that lands one cycle early is clipped, not shifted |
| A Bt453 address-register **write** resets the R/G/B phase; a **read** does not; both `DAC+0` and `DAC+2` are the same register; the address increments after the blue cycle and wraps `$FF` → `$00` | colors land one channel out of phase, or a Bt458 driver's "read mask" write moves the address |

## Limitations and deliberate departures

- **Drawing time is Table 3's, and nothing finer.** At `draw_rate=real` a command takes its
  data-sheet time in the memory cycles the display leaves free (*Drawing time*, above, lists
  what that model assumes); at `full`, the default, it takes none. Either way the raster
  counter reads 0, and there is no DTACK and no wait state on a register access, so a guest
  that syncs to the raster to avoid flicker sees an unmoving beam.
- **Every command executes.** DRD/DWT/DMOD run only in the manual's "under program control"
  mode (no DMAC on the board). An undefined opcode sets **CER** and is dropped alone, so the
  words after it are read as commands.
- **PAINT has no seed-stack limit.** The chip keeps four pending seeds and hands any more to the
  host through the read FIFO, to be painted by re-issuing PAINT (PAINT-4); here one PAINT fills
  the whole area and never sets RFR, so a driver following the manual's re-issue loop simply
  finds nothing left to do.
- **Scan-out**: graphic screens only (a CHR = 1 screen enabled for display is scanned as
  graphic, frame memory at its address — the board ignores CHR on a display cycle, as the
  hardware does, but the raster address the chip would put on MA16–19 is not modeled), the
  three background screens stacked and the window over them, non-interlaced; single and
  interleaved access only (superimposed mode's second phase is not fetched). The monitor is the
  three listed VESA modes, each at the one refresh rate in the table; the ACRTC's own HC/VC and
  the sync widths are accepted but not checked against the mode — a timing a real monitor would
  lose lock on shows here as a picture in the wrong place. No zoom, no cursors, no light pen,
  no blink, no smooth scroll (SDA is ignored), no DISP/CUD skew.
- **The frame memory and the overlay SRAM are zero at power-on**; real SRAM is not
  necessarily zero at power-on — this is a simulator convenience, not a hardware fact.
- **The overlay follows the picture a frame at a time.** On the board, clearing `OLEN` lets
  the overlay shift register run empty within sixteen pixels; here the next frame simply has
  no overlay.
- **Bt453**: the analog side (sync, blank, the CS\*-blanks-video artifact) is not modeled and
  cannot be observed from the bus.
- **MODE HSPOL/VSPOL are recorded, not modeled.** This board has no separate sync-signal
  representation for a polarity bit to invert — the picture is painted directly into a
  Surface, not generated as a raster with distinct sync pulses — so the two bits are decoded
  into `SHOW`'s `hspol`/`vspol` and drive nothing further.

## Verification

`tests/test_bt453.cpp` drives the DAC through Tables 1–2 of its data sheet.
`tests/test_hd63484.cpp` (57 sections) drives the ACRTC through its two locations: reset state,
8-bit register access, FIFO/status transitions, every worked example the manual gives (WPR-2,
RPR-2, WPTN-2, RPTN-2, ORG-3, CLR-3/4, ALINE-2, ARCT-2, AFRCT-1, DRD/DWT-2), the operation,
color and area modes, scan-out geometry, the window overlay, a snapshot round trip, and the
character address space (a CHR screen's drawing reaching the board's memory, by ORG's or RWP's
screen, at the low 16 bits). Its
**drawing-time** sections check every Table 3 formula, the FIFO backing up behind a busy
command (WFE, then WFR at 8 words), RD's word and a DWT's per-word cost, ABT and turning timing
off mid-command, the free-cycle rules on a small frame counted by hand (display priority,
drawing priority, static RAM, interleaved, a command wrapping whole frames, a stopped chip), and
a snapshot taken mid-command.
`tests/test_cadzilla.cpp` proves the board: the one-8-port-block decode (both directions on
the ACRTC's RS=0/RS=1 pair at `+0`/`+2` and the Bt453 at `+4`..`+7`, write-only on MODE at
`+1`, nothing at `+3`, the 16-bit high byte an 8-bit host never uses), the MODE register's five fields decoding independently into
live status and RESET\* clearing it, the ports reaching the chips, and an **end-to-end 640x480 picture** — LUT loaded
through the four DAC ports, a rectangle, a line and a dot drawn through the two ACRTC ports,
the frame read back with `CHECK_FRAME_OPTS` sampled every 32nd pixel for a reader and
`CHECK_FRAME_PIXELS` against an oracle for **every one of the 307,200 pixels**
(`tests/framecheck.h`, on the RAMDAC's input), its colors resolved by the Bt453; the picture's placement by
HDS/VDS (one cycle right, one cycle clipped, one raster down, a narrower picture); the wiring
— GAI +1 fetching one aligned row eight times, a start address's low bits selecting nothing,
BLANK black whatever entry 0 holds, a 4 bpp dot landing in the wrong nibble, and MODE AMODE
disagreeing with OMR ACM (the picture follows MODE, not OMR), as the hardware would, with
`wiring` naming each; **1024x768 in interleaved mode** with the doubled registers, sampled
every 64th pixel and checked pixel by pixel; every mode's frame filled by one `AFRCT` and
every pixel of it checked; the window at HWS/VWS; the `mode` strap re-opening the frame at
800x600; a palette-only change moving the frame, a snapshot repainting it, and RESET\*
leaving a black frame while the LUT survives; **the overlay** — drawn through a CHR screen
into its own SRAM at `(OLSEL << 16) | address` with the frame memory untouched, shown as
overlay color 2 only with `OLEN` (bit 0 leftmost, single and interleaved), word *A* >> 3 going
with its fetch and `OLSEL`'s half behind the upper 1 MB, `wiring` naming a displayed CHR
screen, and a snapshot carrying the SRAM and `OLSEL`; and a dedicated **interrupts** section proving
`interrupt=none` (the default, SW1-8 off) asserts nothing regardless of the ACRTC's own
pending state, `interrupt=int`/`vi0`..`vi7` (SW1-8 on) raises that line exactly when
`Hd63484::irq()` is true, and disabling CCR's enable bits drops the request even while SR's
own CED bit is still pending. Its **drawing-time** sections prove `draw_rate` set both by `SET`
and from a machine file (and saved back), the T-state length of a DOT and a CLR at 1024x768
single and 640x480 interleaved, an `AMODE` change mid-command, a guest polling every T-state
losing no time, the same T-states at `clock_hz = 0`, 2 MHz and (doubled) 4 MHz, the CED
interrupt raised by the deadline alone, and a restored board re-arming its command's end.

No period software exists for this board; the register table above is what a program would
load, and `machines/cadzilla.toml`'s header walks the default 1024x768 case from the monitor
prompt. **`tests/media/cadzilla/`** holds a whole
program: drawdemo, under CP/M 2.2 on an 8" floppy, steps through every drawing command at 1024x768, one screen each (`drawdemo.toml` at
`draw_rate = "full"`, `drawdemo-real.toml` at a 2 MHz 8080 and `draw_rate = "real"`).
`acceptance-media` boots those machine files and that floppy from a copy of the folder,
runs DRAWDEMO, and checks through
`SHOW` that it started the board at 1024x768 with the wiring right.

## References

- `reference/Hitachi HD63484 ACRTC.md`, `reference/Brooktree Bt453 RAMDAC.md`
- `docs/sources.md` — the sourcing terms, including MAME's role
- `docs/devguide/video-board.md` — how a video board is written, with this one as the example
