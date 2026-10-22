# Zenith NET-100-1 Local Area Network Interface Card — S-100 ARCNET board

Source: [Z-100 NET100 Ethernet Interface card.pdf](#) (the manual) and
[Z-100 NET1 schematic.pdf](#) (the card schematic, a separate one-page scan; section 12)

Zenith Data Systems service manual for the **NET-100-1 Local Area Network Interface Card** (ZLAN), for the
Z-100 series. © 1982, 1983 Standard Microsystems Corporation, © 1984 Zenith Data Systems; 116 PDF
pages. Chapters 1–11 are Zenith's own text (introduction, S-100 pins, configuration, tests, theory,
service, parts, PAL); chapter 12 reprints the SMC **COM9026 LANC** and **COM9032 LANT** data sheet and
tech note TN5-2. This file keeps what an implementer needs: the address decode, the switch and jumper
map, the COM9026 register and command set, the packet buffer layout, interrupts, wait states, and the
hub. It omits installation, disassembly, assembly, the parts list, and the transceiver analog circuits.

**Not emulated.** **The file name says "Ethernet" but this is not Ethernet.** The card is a
**Datapoint ARCNET** node: a token-passing network at **2.5 Mbit/s** on **RG-62A 93 Ω coax**, with up to
255 nodes. It has an on-board **4-port active hub** (three external BNC ports, one internal to the
controller). It is an ordinary S-100 (IEEE 696) card, so it is not tied to the Z-100, and the manual
gives all switch settings for non-Zenith S-100 machines too.

**The manual has no schematic**: chapter 9 says "refer to the schematic diagrams", but the PDF ends at
Appendix A (page A-8). The separate schematic scan (Zenith part of 585-49-2, 1984, one page, image only)
was read as a zoomed image; section 12 gives what it settles.

Printed page numbers are used below (`n-m`). The PDF page is the printed page plus 20 in chapter 4,
plus 72 in chapter 12 (e.g. printed 12-10 is PDF page 82). The scan is an Acrobat Paper Capture OCR
with a text layer; it mis-reads digits (see *Quirks*), so every number here was checked on the page image.

---

## 1. Quick reference for emulation

| Item | Value | Page |
|---|---|---|
| Network controller | SMC **COM9026** (LANC), U116; transceiver COM9032 (LANT), U123 | Spec, 9-10 |
| Network | ARCNET-compatible, token passing, 2.5 Mbit/s, node IDs 1–255 (0 = broadcast) | 1-2, 12-17 |
| Buffer RAM | **2 K × 8** (6116), U115, shared by the processor and the COM9026 | Spec, 8-3 |
| ROM | Optional, **not fitted**: 2732 (4 K), 2764 (8 K) or 27128 (16 K); boot without a disk | Spec, 8-3 |
| I/O ports | **2 consecutive ports** from a switch-set base (factory **0A0h**): base = status read / mask write, base+1 = command write | 12-23, 6-3 |
| Buffer RAM address | 2 KB, switch-set on a 2 KB boundary, 24-bit (factory **0F0000h**) | 4-6 |
| ROM address | switch-set 24-bit (factory **0F4000h**, 8 K 2764) | 4-6 |
| Node ID | 8-position DIP SW105, **inverted** (OFF = 1), position 7 = bit 0 | 4-7, App. A |
| Interrupt | one jumper (J101): VI0–VI7, NMI or INT; factory **VI2** | 4-5 |
| Waits | the card holds **RDY** low on every RAM or I/O access to the COM9026 until its WAIT drops | 9-5 |
| Bus | S-100 IEEE 696 pinout. 8-bit or 16-bit I/O address (J105). Memory compares **A23–A16 and A15–A11**; there is **no** 16/24-bit memory jumper, only the switches (section 12) | Spec, schematic |
| Access time | RAM/I-O 880 ns max, ROM 220 ns max | Spec |
| Power | 8–11 V DC (typ 1.6 A, max 2.0 A), −12 V at 0.03 A; on-board +5 V regulators U113/U114 (LM323), −5 V U143 (79L05) | Spec, 11-9 |

---

## 2. Address decode

The decode is done by comparators U105/U134 (memory) and U110/U135 (I/O), set by the switches below,
and by the PAL16L8 U107 (section 8). OFF = logic 1, ON = logic 0.

### 2.1 Buffer RAM — SW101 and SW106 (2 K, on a 2 K boundary)

| Switch | Pos 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 |
|---|---|---|---|---|---|---|---|---|
| SW101 | A23 | A22 | A21 | A20 | A19 | A18 | A17 | A16 |
| SW106 | A15 | A14 | A13 | A12 | A11 | NC | NC | NC |

Factory (**0F0000h**): SW101 = ON ON ON ON OFF OFF OFF OFF; SW106 = all ON. A10–A0 select the byte in
the 2 KB window. The Z-100 monitor shows this as `F000:0000`.

### 2.2 ROM — SW102 and SW104

| Switch | Pos 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 |
|---|---|---|---|---|---|---|---|---|
| SW102 | A16 | A17 | A18 | A19 | A20 | A21 | A22 | A23 |
| SW104 | NC | NC | NC | NC | NC | A15 | A14 | A13 |

Factory (**0F4000h**): SW102 = OFF OFF OFF OFF ON ON ON ON; SW104 = pos 5 ON, pos 6 OFF, pos 7 ON,
positions 0–4 do not matter. ROMSEL goes low when both comparators match; it enables the ROM's CE.

ROM jumpers (Table 9-1, agrees with the text on 4-6):

| ROM | J112 | J113 | J115 |
|---|---|---|---|
| 2732-2 (4 K) | 1-2 | 1-2 | 2-3 |
| 2764-2 (8 K) | 1-2 | 2-3 | none |
| 27128-2 (16 K) | 2-3 | 2-3 | 1-2 |

**J114**: pins 2-3 = ROM does **not** drive PHANTOM*; pins 1-2 = PHANTOM* is driven while the ROM is
selected, so the ROM maps over main-system memory. The Z-100 does not use J114.

### 2.3 I/O — SW103 and SW107

| Switch | Pos 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 |
|---|---|---|---|---|---|---|---|---|
| SW103 | A7 | A6 | A5 | A4 | A3 | A2 | A1 | NC |
| SW107 | A8 | A9 | A10 | A11 | A12 | A13 | A14 | A15 |

Factory **0A0h**: SW103 = OFF ON OFF ON ON ON ON (x); SW107 = all ON.
**J105**: pins 1-2 = 16-bit I/O address (compare SW103 and SW107); pins 2-3 = 8-bit I/O address
(compare SW103 only; SW107 is ignored). Factory is 2-3 (8-bit).

**SW103 position 7.** Chapter 4 (4-6) labels it **NC**; chapter 9 (9-3) labels it **A0**. **NC wins.**
Two things settle it: the manual's own I/O test reads `IN AL,0A0h` and writes `OUT 0A1h,03h`, so A0
cannot be part of the compare; and the schematic shows comparator U110 comparing bus A7–A1 only, with
its last input tied to ground. A0 goes on to the COM9026 as its AD0 command-decode input (section 4).
Leave position 7 **ON**: the schematic ties the comparator input low, so OFF would not match.

---

## 3. Node ID — SW105

Eight positions, **position 0 = bit 7 … position 7 = bit 0**, **OFF = 1, ON = 0**. All ON is ID 0,
which is the broadcast address, so it is not a valid node ID. Manual examples:

| ID | Settings (positions 0–7) |
|---|---|
| 1 | ON ON ON ON ON ON ON OFF |
| 2 | ON ON ON ON ON ON OFF ON |
| 100 (64h) | ON OFF OFF ON ON OFF ON ON |
| 114 (72h) | ON OFF OFF OFF ON ON OFF ON |

Appendix A (from PDF page 109) lists all 255. The COM9026 reads the switches once at power-on, through
shift register U132 (74LS166), but the data sheet says the value is "continuously sampled". The ID is
copied into buffer RAM address 01h (section 5).

---

## 4. COM9026 programming interface

The card gives the COM9026 two I/O ports. The data sheet decodes IOREQ with **AD0** (the address bit
A0 that the multiplexers pass to the chip) and **R/W** (12-23):

| IOREQ | AD0 | R/W | Function | Card port |
|---|---|---|---|---|
| low | low | low | **write interrupt mask** | base |
| low | low | high | **read status** | base |
| low | high | low | **write command** | base+1 |
| low | high | high | reserved | base+1 read |

### 4.1 Status register (read, port base)

| Bit | Name | Meaning |
|---|---|---|
| 7 | **RI** | Receiver inhibited. 1 = a packet is in the RAM page of the last ENABLE RECEIVE. Set automatically after any message; no message is accepted until the next ENABLE RECEIVE. Can interrupt. |
| 6 | ETS2 | Current level of the ET2 input pin. |
| 5 | ETS1 | Current level of the ET1 input pin. |
| 4 | **POR** | Power-on reset flag. **Non-maskable interrupt** via INTR. Cleared by CLEAR FLAGS. |
| 3 | TEST | Diagnostic; 0 in normal use. |
| 2 | **RECON** | Reconfiguration timer expired (RX idle for 78.2 µs). Can interrupt. Cleared by CLEAR FLAGS. |
| 1 | **TMA** | Transmit message acknowledged. Valid only after TA = 1. Never set for a broadcast. Never interrupts. |
| 0 | **TA** | Transmitter available (a transmit finished, or DISABLE TRANSMITTER ran). Can interrupt. |

State during POR (12-22): RI = 1, POR = 1, TEST = 0, RECON = 0, TMA = 0, TA = 1, ETS2 and ETS1 not
affected. The interrupt mask is reset (no maskable interrupt enabled) and page 00 is selected for both
receive and transmit. When POR is released, the chip raises the non-maskable POR interrupt.

### 4.2 Interrupt mask (write, port base)

Same bit positions as the status register: **bit 7 = RI mask, bit 2 = RECON mask, bit 0 = TA mask**; the
other bits are don't-care. TMA, ETS1 and ETS2 never interrupt. INTR = (RI·M7) + (RECON·M2) + (TA·M0) + POR.
INTR is active high at the chip. On the card it goes through U107 and J101, and lights LED D103.
To clear a TA or RI interrupt, either issue a new transmit/receive command or clear its mask bit; to
clear a POR or RECON interrupt, issue CLEAR FLAGS.

### 4.3 Commands (write, port base+1)

`nn` = buffer page (0–3). All other codes are not permitted and can corrupt the chip or the network.

| Code | Command | Effect |
|---|---|---|
| `0000 0000` | reserved | |
| `0000 0001` | DISABLE TRANSMITTER | Cancels a pending (not yet started) transmit; sets TA at the next token. |
| `0000 0010` | DISABLE RECEIVER | Cancels a pending receive; RI is set at the next token (a receive in progress finishes). |
| `000n n011` | **ENABLE TRANSMIT FROM PAGE nn** | Sends page nn at the next token. Sets TA = 0, TMA = 0. TA goes to 1 when done; TMA = 1 if the destination acknowledged. |
| `b00n n100` | **ENABLE RECEIVE TO PAGE nn** | Sets RI = 0 and accepts a packet into page nn. `b` = 1 also accepts broadcasts (DID 0). RI = 1 when a packet arrives. |
| `0000 c101` | DEFINE CONFIGURATION | `c` = 1: long packets (up to 508 bytes) are handled too; `c` = 0: short packets only (under 254 bytes). |
| `000r p110` | CLEAR FLAGS | `p` = 1 clears POR; `r` = 1 clears RECON. |

The manual's I/O test waits for TA (`IN AL,base; AND AL,01`), then writes `03h` to base+1 — ENABLE
TRANSMIT FROM PAGE 0 — and loops (6-3).

---

## 5. Buffer RAM and packet format

The processor sees the buffer as **2 KB of memory** and the COM9026 as 2 I/O ports. The buffer is
**four pages of 512 bytes** (or four of 256 with a 1 KB RAM; this card has 2 KB, so the long-packet
format is available). Page nn selects the top address bits, so page nn starts at buffer offset nn × 200h.
Four pages let transmit and receive be double-buffered. The COM9026 and the processor share the RAM
by arbitration; the processor is held in a wait state when they collide.

Power-on self-test: after reading its ID, the chip writes **`D1h` to address 00** and the ID to
**address 01** of page 0. The processor can read address 01 for the node ID and check that `D1h` at 00
survives (12-22; the manual's memory test uses this, 6-3/6-4).

### 5.1 Short packet (offsets in the page; COUNT is a 2's complement)

| Offset | Transmit buffer | Receive buffer |
|---|---|---|
| 00 | SID (the chip ignores this and sends its own switch ID) | SID (written by the chip) |
| 01 | DID (destination; 00 = broadcast) | DID |
| 02 | COUNT = **256 − N** | COUNT = 256 − N |
| COUNT … FF | N data bytes — the last data byte is always at **offset FFh** | N data bytes |

Examples (12-10, 12-11): transmit N = 120 (78h) from ID 4Ch to B2h: `4C B2 88 …` with data from 88h to FFh
(88h = 100h − 78h). Receive N = 201 (C9h) from F3h to 91h: `F3 91 37 …`, data 37h to FFh.

### 5.2 Long packet (up to 508 bytes; needs DEFINE CONFIGURATION c = 1)

| Offset | Contents |
|---|---|
| 00 | SID |
| 01 | DID |
| 02 | **00** (this marks a long packet) |
| 03 | COUNT = **512 − N** |
| COUNT … 1FF | N data bytes — the last byte is at offset **1FFh** |

Example: N = 300 (12Ch) from 2Fh to D8h: `2F D8 00 D4 …`, data D4h to 1FFh. The COUNT byte always
points inside the first 256 bytes of the page. Message lengths of **254 to 256** bytes must be padded
to 257 or more so they use the long format. Nodes with and without long-packet support can coexist; a
long packet sent to a node without the long-packet flag is ignored by the receiver, the sender sets
TA but not TMA.

### 5.3 Notes

- On **transmit** the chip ignores the SID in the buffer and inserts the ID from the switches.
- On **receive** the chip stores the SID of **every** message on the wire in the receive page, even
  those not for this node. It stores the body only if the DID is 0 (broadcast, if enabled) or this
  node's ID. A node can use this to watch traffic.
- A received packet has the same layout as a transmit buffer, so it can be sent on without rearranging.
- The page numbers `nn` pick the page; the card's 2 KB RAM gives pages at buffer offsets 000h, 200h,
  400h and 600h. This is inferred: the data sheet says `nn` drives the top RAM address lines (12-17).

⚠ **Where the received SID lands.** The text on 12-12 says the chip stores the SID "into RAM buffer
location 02 of the current page". Figure 8 (12-18) and figures 15 and 16 (12-11) show the SID at
offset **00** and offset 02 as COUNT (or 00 for a long packet). The figures win here: three figures
and two worked examples agree, and offset 02 holds COUNT in all of them. The manual ships no driver
to settle it.

---

## 6. Network behavior (from the data sheet)

| Item | Value |
|---|---|
| Line protocol | Isochronous. Each byte: 2 mark bits + 1 space + 8 data bits = 11 bit times of 400 ns (4.4 µs per byte). 1 = 200 ns negative pulse (dipulse on the coax), 0 = no pulse. |
| Transmission types | **Invitation to transmit (ITT)**: ALERT BURST (8 marks) + EOT (04h) + 2 × DID. **Free buffer enquiry (FBE)**: ALERT + ENQ (05h) + 2 × DID. **Packet**: ALERT + SOH (01h) + SID + 2 × DID + COUNT (+ 00 + COUNT for long) + data + 2 CRC (CRC-16, X¹⁶+X¹⁵+X²+1). **ACK**: ALERT + 06h. **NAK**: ALERT + 15h. |
| Token passing | A node with the token and a packet sends an FBE to the DID; on ACK it sends the packet, and the destination ACKs again if the CRC is good. On NAK or no reply within **74.7 µs**, it passes the token with an ITT to NID. A DID of 0 skips the FBE (broadcast). |
| NID | Each node keeps the Next ID. Powered-off nodes are skipped by incrementing NID until one answers. |
| Reconfiguration | Triggered when a node powers on, or when a node has had no ITT for **840 ms** (shortest ET setting, table below). It sends a RECONFIGURE BURST (8 marks and 1 space, repeated **765** times) that destroys the token and ends all activity. During reconfiguration ITTs go to all 256 possible IDs; each node ends up with NID = the ID of the node it received the token from. Takes 24–61 ms depending on node count and highest ID (12-18). |
| Per-node timeout | A node that sees an idle line for more than 78.2 µs starts a timer, and sets NID = its own ID. The data sheet gives the timer as "146 microseconds minus the quantity 255 minus ID" so that the highest ID times out first and starts sending ITTs. ⚠ The exact formula is not settled here; see Quirks. |
| Timing | Token pass with nothing to send ≈ 28 µs (Tb = 28 N µs for N nodes). Packet time ≈ 33.2 µs + 4.4 µs × bytes. A worst-case round trip is Tw = Tb + (113 + 4.4 B) N µs. |

Timeouts set by J110 and J111 (ET2, ET1). The chip's ET pins read the same jumpers, so the host can
see the setting in status bits 6 and 5. **All nodes on one network must use the same setting.**

| ET2 | ET1 | Response time | Reconfiguration time |
|---|---|---|---|
| 1 | 1 | 74.7 µs | 840 ms |
| 1 | 0 | 283.4 µs | 1680 ms |
| 0 | 1 | 561.8 µs | 1680 ms |
| 0 | 0 | 1118.6 µs | 1680 ms |

(Table 1 on 12-23. The table on 4-5 prints the reconfiguration column in "ns" — an error; the data
sheet's "ms" is right.) The idle time is always the response time plus 3.5 µs. The manual does not say
which jumper position is logic 1; the factory J110/J111 position is pins 1-2.

**Jumpers T1 (J108), T2 (J107):** pins 2-3 for normal operation (1-2 = chip-level test). **J109 (ECHO):**
pins 1-2 for normal operation; pins 2-3 makes the chip retransmit every message shorter than 254 bytes
(a test mode). Factory: J107 and J108 on 2-3, J109, J110 and J111 on 1-2.

---

## 7. Interrupt

The interrupt request is **INTR9026** from the COM9026. U107 (PAL) passes it to **INTR\*** (active low),
which also drives LED D103 through U138, and J101 routes it to one S-100 line. **Install one jumper only.**

| J101 option | S-100 pin |
|---|---|
| VI0\* … VI7\* | 4 … 11 |
| NMI\* | 12 |
| INT\* | 73 |

Factory: jumper across **header pin 5** of J101 = **VI2\***. The OCR text reads "V12*"; it is VI2. The
schematic confirms it: J101 has ten rows in the order NMI\*, INT\*, VI0\* … VI7\* wired to S-100
pins 12, 73, 4 … 11, and the factory jumper is drawn on the VI2 row (the fifth).

All interrupt lines are open-collector (O.C.) low-active (S-100 pin table, 2-1/2-2).

---

## 8. Bus interface logic (PAL16L8 U107, 11-15)

Inputs: /IOADRS, /MEMADR2, /MEMADR1 (from the comparators), INTR9026, BSYNC, BOUT, BINP, BMEMR,
/BWO. Outputs: /IOREQ, /MREQ, AS, /INTR, /PHANTOM1, /PHANTOM2 (PHANTOM1 and 2 tied together; both go
to S-100 PHANTOM\*, pin 67). Equations as printed:

```
IF (MEMADR2 * MEMADR1 * /BOUT * BWO) PHANTOM2 = PGMGND
IF (INTR9026) INTR = INTR9026
IF (MEMADR2 * MEMADR1 * BMEMR) PHANTOM1 = PGMGND
/AS = /BSYNC
MREQ  = MEMADR2 * MEMADR1 * MEMR + MEMADR2 * MEMADR1 * /BOUT * BWO
IOREQ = IOADRS * BINP + IOADRS * BOUT
```

Meaning (9-4, 9-5): a memory cycle that hits the RAM window raises MREQ; an I/O read or write that hits
the port pair raises IOREQ; the **buffer RAM window drives PHANTOM\* on both read and write** (so the
card overlays system ROM at the same address). No jumper is listed for the RAM's PHANTOM\*, unlike
the ROM's (J114). `AS` is BSYNC inverted and tells the COM9026 to sample MREQ/IOREQ.
LED D105 = MREQ, D102 = IOREQ, D104 = ROM read.

**Wait states (9-5, 9-6):** BSYNC presets flip-flop U139. When a cycle hits the RAM window or the I/O
ports, the card forces **RDY (S-100 pin 72) low** until the COM9026 drops WAIT; WAIT is synchronized with
φ2 through U139 and ends the wait. The ROM path is **independent**: it generates no waits and does not
use the COM9026's WAIT.

---

## 9. Factory configuration (Z-100)

Typical settings (4-3, Figure 4-1):

| Part | Setting |
|---|---|
| J101 | pin 5 (VI2\*) |
| J105 | pins 2-3 (8-bit I/O address) |
| J107, J108 | pins 2-3 (normal) |
| J109, J110, J111 | pins 1-2 |
| J112 / J113 | pins 1-2 / pins 2-3 (2764 ROM) |
| SW101 | pos 0–3 ON, 4–7 OFF → RAM 0F0000h |
| SW102 | pos 0–3 OFF, 4–7 ON → ROM 0F4000h |
| SW103 | pos 0 and 2 OFF, rest ON → I/O 0A0h |
| SW104 | pos 6 OFF, rest ON |
| SW105 | node ID (user) |
| SW106, SW107 | all ON |

The Z-100 uses no J114 and no ROM (the ROM socket is empty).

---

## 10. Hub, cabling, and LEDs

- Three external **BNC** ports (interface modules IM101–IM103, via the chassis adapter) and one internal
  port to the controller. Cable: **RG-62A**, 93 Ω. Maximum run between units: **2000 ft**.
- The hub is **active**: every unit in a path or tree must be powered for traffic to pass. Use a daisy
  chain or a tree; a loop is not allowed (Figure 1-4).
- An unused port is fine (the hub tolerates an unterminated stub).
- Power-on RC (R101, C134) holds the hub's reset for 0.22 s. A retriggerable one-shot (U137) returns the hub to idle
  4.9 µs after the last pulse; EOT\* (end of time) then resets the port flip-flops.
- LEDs: **D102** I/O request, **D103** interrupt, **D104** ROM read, **D105** RAM (memory request),
  **D106 / D107 / D108** activity on ports 1 / 2 / 3 (IM101 / 102 / 103), **D109** on-board port (lit while idle).

---

## 11. Shipped software

The manual ships no driver and no ROM listing. Chapter 6 has two 8086 test programs (RAM.ASM, IO.ASM)
and a monitor memory test; the I/O test is the only one that touches the COM9026 (section 4.3).
The RAM test writes `0` in a loop to `F000:0` to light D105. The ROM socket is empty when shipped.

---

## 12. What the schematic settles

Read from `Z-100 NET1 schematic.pdf` (Zenith, part of 585-49-2, 1984), rendered at 216 dpi and read in tiles.

- **There is no 16-bit/24-bit memory jumper.** The specification page says "jumper selectable 16-bit or
  24-bit memory addressing", but the only jumper that touches addressing is **J105, for I/O**. A16–A23
  go through buffer **U101** (74LS244, both enables grounded, so always on) straight into comparator
  **U105**, against **SW101**. ROM does the same: A16–A23 into **U106**, against **SW102**. A11–A15
  go through **U111** into **U134** against **SW106**. The comparator switch inputs have 10 K pull-ups
  (RP101, RP102, RP103, RP106, RP108, RP109); ON = grounded = 0. So "16-bit memory" means only: set
  SW101 and SW102 to what A16–A23 read on your bus.
- **I/O compares seven bits.** U110 compares bus A7…A1 with SW103 and has its eighth input grounded.
  A0 is never compared. A0 reaches the COM9026 through U102 and the address multiplexers U112/U118 as
  its AD0, which picks status/mask (A0 = 0) or command (A0 = 1). U135 and SW107 (A15…A8) are only in
  the path when J105 is on 1-2.
- **Bus signals used.** In: A0–A15 (through U102, U108, U111), DI0–DI7 and DO0–DO7 (separate buses),
  pSYNC (76), pDBIN (78), pWR\* (77), sOUT (45), sINP (46), sMEMR (47), sWO\* (97), RESET\* (75). Out:
  RDY (72), PHANTOM\* (67), and one of NMI\* (12), INT\* (73), VI0–VI7\* (4–11). The chapter 2 pin
  table lists no pHOLD or other DMA signal, so the card is a plain bus slave.
- **PHANTOM\*.** U107 pins 15 and 16 are tied to one open-collector line, pin 67. The ROM adds its own
  term through U131 and J114 (J114 pin 3 is not connected, so 2-3 = ROM does not drive it).
- **RDY.** Two U131 open-collector gates drive pin 72: one for I/O cycles (with U121, U136), one for
  RAM cycles (with U142), both released when U139 sees the COM9026's WAIT end. ROM drives neither.
- **The LEDs** are fed as the manual says: D105 from MREQ\*, D102 from IOREQ\*, D103 from INTR\*, D104
  from ROMSEL and BMEMR.
- ⚠ **Hardware caution, not checked on the schematic:** the chapter 2 pin table (2-1, 2-2) lists S-100
  pins **20, 53 and 70** as ground. On a real Altair, pins 70 and 20 are memory PROTECT and UNPROTECT and
  pin 53 is the sense-switch disable (DESIGN.md section 4). A card that grounds them can assert those
  signals on a real Altair backplane. Check the card's edge connector before plugging it into real
  hardware. AltairSim does not model a board driving those pins, so this has no effect in the simulator.

---

## 13. Adding it to AltairSim — 16-bit memory address, 8-bit I/O address

This is a design note for a future plan, not a plan. It was checked against `DESIGN.md` section 4 and
`src/core/board.h`. The card is a late S-100 board that behaves within the Altair bus, which
`DESIGN.md` allows; only the 24-bit addressing is out of scope, and this card does not need it.

### 13.1 What fits without change

| Need | AltairSim |
|---|---|
| 16-bit memory | `BusCycle.addr` is 16 bits. Compare A15…A11 only; treat A23…A16 as 0. |
| 8-bit I/O | The port is `addr & 0xFF`. Ignore A15…A8, so J105 on 2-3 is the only case. Two ports: base and base+1. |
| Interrupt | `IrqJumper` is `none | int | vi0 … vi7` (`board.h` 114–163); `assertsInt()` / `assertsVi()`. |
| PHANTOM\* | `assertsPhantom(cycle)`. |
| Wait states | `Board::holdReady(tStates)` from `read()` / `write()`. |
| Separate data buses | `BusCycle.data` is already one byte per direction. |

### 13.2 Findings to carry into a plan

1. **Memory window.** Settings that mean "16-bit": SW101 all ON (A23…A16 = 0) and, if a ROM is
   used, SW102 all ON. Then RAM and ROM are 16-bit addresses (2 KB aligned for RAM; 4/8/16 KB for
   ROM, set by SW104/J112/J113/J115). Recommend config in **addresses** (`ram = 0xF000`), not switch
   strings. The factory `0F0000h` becomes `0xF000` in 16 bits, but **a 64 KB RAM machine already
   has memory there**; choose a window where no RAM answers, or set `honors_phantom` on the RAM.
   ⚠ On a **real** Altair, A16–A23 are unconnected, and a floating 74LS244 input reads high, so SW101
   would have to be all **OFF**. That depends on the backplane and was not checked.
2. **Ports.** The factory base `0A0h` **collides with the 88-HDSK**, whose default is eight ports at
   `0xA0` (`src/boards/mits-88hdsk.cpp:16`). The base is switch-set, so choose another default and
   check with `WHO IO`. Port map: **base read = status**, **base write = interrupt mask**,
   **base+1 write = command**. A read of base+1 is "reserved".
3. **Interrupt.** Offer `none | int | vi0 … vi7`. The card's NMI\* option has no wire in the model, so
   leave it out. The factory jumper is VI2, which only works if an 88-VI (`virtc`) is in the machine;
   a machine without one needs `int`. `INTR` = (RI·M7) + (RECON·M2) + (TA·M0) + POR (section 4.2),
   so the board must raise `intChanged()` when any status bit or mask bit moves.
4. **PHANTOM\*.** The RAM window drives PHANTOM\* on every cycle that hits it, **read and write**,
   with no jumper (section 8). The ROM does so only with J114 on 1-2; make it a setting that defaults
   to off, as shipped.
5. **Wait states.** Every cycle to the RAM window or the two ports is held by RDY until the COM9026
   answers; ROM cycles are not. The manual gives **no count in T-states**. It gives times: RAM/I-O
   access 880 ns max, ROM 220 ns max (Spec); a processor cycle takes 4 CLK of 200 ns, and the chip
   starts it 200–500 ns after the request, 1300 ns at worst (12-22). Model this as time, converted
   at the CPU's `clock_hz`, and do not pick a T-state count that merely boots (see "A wrong number that
   boots is dangerous"). At `clock_hz = 0` (flat out) decide what a wait means before coding.
6. **Reset and power-on.** At power-on and keyboard reset the chip reads the node ID, writes `D1h` to
   buffer offset 0 and the ID to offset 1 of page 0, and selects page 0 for both directions
   (12-22, 6-3). Derived status after reset: RI = 1, POR = 1, TA = 1, RECON = 0, TMA = 0, so the
   status byte is `91h` plus bits 6 and 5 from the ET jumpers. This gives an immediate test:
   read the window at offsets 0 and 1.
7. **Where the code goes.** Split as other boards do: a COM9026 chip model under `src/chips/`
   (registers, commands, packet engine, token and timers) and a board under `src/boards/` (decode,
   switches as settings, window, wait states, PHANTOM\*, interrupt). Follow
   `docs/devguide/adding-a-board.md` and the `add-a-board` skill. The board name follows the house
   style ("board", not "card").
8. **The network medium is the big open question.** Real nodes share one coax cable through a hub. A
   simulated node needs a host transport to reach other simulated nodes (two processes, or one process
   with several nodes). The protocol the chip speaks is documented (section 6), but the manual has
   nothing about a host link. Decide it in the plan, with the endpoint layer in mind
   (`serial-io-architecture`: board/chip → ByteStream → `resolveEndpoint`), and ask the maintainer
   before inventing a medium. The "never invent hardware" rule applies: no behavior the real
   hardware lacked.
9. **Guest software.** None ships; the manual's tests are 8086 code. A test needs an 8080 program
   (use the `altairsim-cpm-build` skill, M80/L80 in the simulator). Useful first tests: the power-on
   bytes (item 6); `OUT base+1,03h` and a TA poll (the manual's I/O test); two nodes passing a
   short packet; a long packet; broadcast.
10. **Sources.** The COM9026 data sheet in this manual is the authority for the chip. Do not read another
    emulator's source (`docs/sources.md`).

---

## 14. Quirks worth carrying

- ⚠ **OCR digits.** The text layer reads the self-test byte at `F000:0000` as "01"; the page image
  (6-3, 6-4) and the data sheet (12-22) both say **D1h**. The text layer also gives "640" for the
  reconfiguration timeout (12-18); the image says **840 ms**, matching Table 1. "VI2" is read "V12".
- ⚠ **Specification page** (viii) prints ROM size "4 K × 1" for the 2732; this is a typo for 4 K × 8.
- **SW103 position 7** is NC in ch. 4 and A0 in ch. 9; NC is right (section 2.3, schematic).
- **S-100 pin 97** is sWO\*. The chapter 2 pin table (2-2) reads "96" in the text layer; the schematic
  says 97, which is also the Altair pin.
- ⚠ **Per-node timeout formula.** The data sheet (12-18) says each node starts a timer equal to
  "146 microseconds minus the quantity 255 minus ID" when it sees an idle line. The print is ambiguous
  (probably 146 µs × (255 − ID)), and this was not settled. Check 12-18 and the COM9026 behavior
  before coding it.
- ⚠ **Received SID location**: offset 00 (figures) or 02 (text on 12-12); see 5.3.
- **Node ID 0 is the broadcast address**, so all-ON on SW105 is not a valid ID.
- **TMA is not a delivery guarantee.** The ACK is hardware level, sent before the receiver's processor
  knows about the packet, and it can be lost to line errors.
- **A target that is not servicing its COM9026 never ACKs.** Its receiver stays inhibited (RI = 1), so
  the sender's TA bit never sets. Software needs its own timeout, then DISABLE TRANSMITTER, which makes
  the chip set TA at the next token (12-10). A packet to a **nonexistent** node gets TA = 1 and TMA = 0.
- A transmit of a long packet from a node whose long-packet flag is not set is not sent, and TA stays 0
  until DISABLE TRANSMITTER (12-12).
- **Slow processor hazard.** The COM9026 needs the RAM periodically. A slow CPU, or holding off the
  delayed-write input (DWR), can overflow its data. The data sheet says to ground DWR when the write
  setup time is met; the card does **not** ground it. The schematic feeds DWR from buffered pWR\*
  (signal BWR) and R/W from buffered sWO\* (BWO).
- **POR flag interrupt** is non-maskable and stays until CLEAR FLAGS; do this first after reset.
- The buffer RAM overlays main memory by driving PHANTOM\*; the system ROM at the same address is hidden.
- All nodes must share the same ET setting (J110/J111), and the idle time is response time + 3.5 µs.
