# Digital Research CP/NET 1.2 — network operating system for CP/M and MP/M II

Source: [dri-cpnet.pdf](#) (*CP/NET Network Operating System Reference Manual*, © 1980–1982
Digital Research, Fifth Edition November 1982), 219 pp, **real text layer**. This copy is the
"Unofficial CP/NET Documentation", re-typeset in OpenOffice (2016-12-24), with the note "CP/M3
extensions added April, 2016" on the title page. The re-typeset has **no printed page numbers**,
so this file cites **PDF pages** and section numbers. Figures 3-1 and 4-2 are text in this
edition. Figures E-1 to E-3 are images and were read as page images.

CP/NET connects CP/M 2.2 machines (**requesters**) to an MP/M II machine (the **server**). A
requester can use the server's disks, printers and consoles as if they were its own. DRI ships the
logical parts finished: `NDOS.SPR` and `CCP.SPR` on the requester, and `SERVER.RSP` on the
server. A site writes only the network driver: the **SNIOS** on the requester and the
**NETWRKIF** on the server. **Not emulated.** CP/NET is software that an emulated machine runs.
The plan for running it on altairsim, and the disk images that are still needed, are in issue #820.

This file keeps what an SNIOS or a server writer needs: the module layout, the message format, the
SNIOS entry points and tables, the Appendix E serial protocol (with the sample SNIOS code that
settles its details), the logical message list and the server rules. It leaves out the MAIL
utility, the tutorial on network topologies, the NETWRKIF process design for MP/M II (§4.3.2–4.3.7,
except the tables), and the ULCnet (App. F) and Corvus OMNINET (App. G) examples.

---

## 1. Quick reference for an implementer

| Item | Value | Source |
|---|---|---|
| Requester OS | Unmodified CP/M 2.x. CP/NET loads under it. | §1.3, p.11 |
| Requester processor / memory | 8080, 8085 or Z80, 16K minimum | Foreword, p.3 |
| Server OS | MP/M II: 8080, 8085 or Z80, 32K minimum, 1–16 consoles, a clock/timer interrupt, a network interface | Foreword, p.3 |
| Requester modules | `NDOS.SPR` (DRI, never changed), `SNIOS.SPR` (site-written), `CCP.SPR` (DRI, replaces the CCP) | §1.3, p.11–12 |
| Loader | `CPNETLDR.COM`: SNIOS directly below the BDOS, NDOS directly below the SNIOS | §1.3, p.12; §2.7, p.17 |
| Server modules | `SERVER.RSP` (DRI), `NETWRKIF.RSP` (site-written), built in with GENSYS | §1.4, p.13–14 |
| Requesters per MP/M II server | 16 | §4.3.3 Table 4-2, p.67 |
| Node IDs | `00`–`FE`. `FF` is illegal. The usual server is `00`. | §3.1.2–3.1.3, p.24 |
| Message header | `FMT DID SID FNC SIZ`, then `SIZ+1` data bytes (1–256) | §3.1.5, Fig 3-1, p.25 |
| Formats used | Requester → server `00`, server → requester `01` | §3.1.7, p.25 |
| Serial protocol | Appendix E: ENQ/ACK, framed header with HCS, framed data with CKS | App. E, p.96–97 |
| Version number | Function 12 returns `022x` under CP/NET, not `002x` | §3.2, p.26 |

## 2. How the requester loads and runs (§1.3, p.11–13)

An SPR file has its logical origin at `0000`. It has a 256-byte header. Header bytes 1–2 hold the
code length. A bit map after the code marks each byte to relocate when the code loads on a
256-byte page boundary.

`CPNETLDR` relocates `SNIOS.SPR` to directly below the BDOS, and `NDOS.SPR` to directly below the
SNIOS. The NDOS initialization then does these steps:

1. Saves the jump at location `0005` (the BDOS entry).
2. Writes a jump to the NDOS at `0005`, so that every BDOS call goes to the NDOS first.
3. Replaces the BIOS jump vector entries for console status, console in, console out, list
   status, list out and warm boot with jumps into the NDOS.
4. Calls the SNIOS `NTWRKINIT`.
5. Jumps to its own warm boot: a disk system reset, then it loads `CCP.SPR` and starts the CCP.

The NDOS gives a local drive's call to the BDOS unchanged. For a networked drive it builds a
message, calls the SNIOS `SENDMSG`, and then calls `RECEIVEMSG` at once for the response. A
request always has one response, so the NDOS never has to sort a stream of messages. Functions
12 (version), 26 (set DMA) and 32 (user number) always go to the local BDOS. The NDOS keeps a copy
of their values for its own use.

**Network drive A:.** When A: is a network drive, `CCP.SPR` must be on that network drive, or a
warm boot fails (§2.3, p.16).

## 3. The message (§3.1, p.24–25; App. B, p.85)

```
+-----+-----+-----+-----+-----+-----      ------+
| FMT | DID | SID | FNC | SIZ | MSG ...         |
+-----+-----+-----+-----+-----+-----      ------+
```

| Field | Size | Meaning |
|---|---|---|
| FMT | 1 | Format code. Even = request, odd = response. DRI reserves `00`–`7F`. |
| DID | 1 | Destination node ID |
| SID | 1 | Source node ID. A node always writes its own ID here. |
| FNC | 1 | BDOS function number (Appendix C) |
| SIZ | 1 | Data length **minus 1**. `00` = 1 byte, `FF` = 256 bytes. |
| MSG | SIZ+1 | The data |

The NDOS always sends format `00`, and the server always expects `00` and answers with `01`. An
SNIOS or a NETWRKIF that uses another format on the wire must convert back to `00`/`01` before it
passes a message on (§3.1.7, p.25). The base message has no error check. The SNIOS adds one if the
medium needs it.

Table B-1 (p.85), field lengths in bytes:

| FMT | DID | SID | FNC | SIZ | MSG | Comment |
|---|---|---|---|---|---|---|
| `00` | 1 | 1 | 1 | 1 | 1–256 | Preferred format |
| `01` | 1 | 1 | 1 | 1 | 1–256 | Returned result |
| `02` | 1 | 1 | 1 | 2 | 1–65536 | |
| `03` | 1 | 1 | 1 | 2 | 1–65536 | Returned result |
| `04` | 2 | 2 | 1 | 1 | 1–256 | |
| `05` | 2 | 2 | 1 | 1 | 1–256 | Returned result |
| `06` | 2 | 2 | 1 | 2 | 1–65536 | |
| `07` | 2 | 2 | 1 | 2 | 1–65536 | Returned result |

**Buffer size.** A server message buffer is at least **262 bytes**: 5 header bytes and 257 data
bytes. The extra data byte is scratch space for the server process (§4.3.3, p.67).

## 4. The SNIOS (§4.2, p.50–58)

The SNIOS works in sequence. It never has to answer a message that the NDOS did not ask for,
unless the network topology needs it to forward messages (a loop network, §4.2.4, p.53).

### 4.1 Jump vector (Listing 4-1, p.50)

| Offset | Entry | Input | Output |
|---|---|---|---|
| `+00` | `NTWRKINIT` | — | Sets up the interface and puts the requester ID in the config table. Can send messages at start. |
| `+03` | `NTWRKSTS` | — | A = status byte (§4.2). Clears `rcverr` and `snderr`. |
| `+06` | `CNFGTBLADR` | — | HL = address of the config table (§4.3) |
| `+09` | `SENDMSG` | BC = message | A = `00` sent, `FF` failed. Returns after the message is sent, so the caller can use the buffer again at once. |
| `+0C` | `RECEIVEMSG` | BC = buffer | A = `00` received, `FF` failed. Returns after a whole message is in the buffer. |
| `+0F` | `NTWRKERROR` | — | Called on a network error. Re-initialize the device. A return gives a retry. For no retry, print a message and warm boot. |
| `+12` | `NTWRKWBOOT` | — | Called each time the NDOS loads the CCP. The sample prints `<Warm Boot>`. |

### 4.2 Network status byte (Fig 4-2, p.51)

| Bit | Name | Meaning |
|---|---|---|
| 4 | `active` | 1 = requester logged in |
| 2 | `ctrlps` | 1 = Ctrl-P is on |
| 1 | `rcverr` | 1 = error in a received message |
| 0 | `snderr` | 1 = error when sending a message |

Bits 7, 6, 5 and 3 are not used. The sample SNIOS equates agree for bits 4, 1 and 0 (`active` =
`0001$0000b`, `rcverr` = `0000$0010b`, `senderr` = `0000$0001b`, PDF p.102).

### 4.3 Requester configuration table (Table 4-1, p.52)

Offsets are **decimal**. The sample SNIOS's `DSEG` layout (PDF p.101) agrees byte for byte.

| Offset | Bytes | Contents |
|---|---|---|
| 0 | 1 | Requester status byte (§4.2) |
| 1 | 1 | Requester node ID |
| 2–33 | 16 × 2 | Drives A:–P:. Byte 1: bit 7 = 1 for a network drive, bits 3–0 = the server's drive. Byte 2: server ID. |
| 34–35 | 2 | Console. Byte 1: bit 7 = on the network, bits 3–0 = server console number. Byte 2: server ID. |
| 36–37 | 2 | List device, in the same form as the console |
| 38 | 1 | List buffer index |
| 39–43 | 5 | List message header: FMT, DID, SID, FNC, SIZ. The sample sets FMT = 0, DID = 0, SID = its own ID, FNC = 5. |
| 44 | 1 | Server list device number (MSG byte 0) |
| 45–172 | 128 | List buffer |

`NETWORK` and `LOCAL` change this table at run time. An SNIOS can also set it up at assembly time
(§4.2.3, p.52). Set bit 7 and the server drive in byte 1 and the server ID in byte 2. **Do not map
a device to a server that is off line.** Some functions (Free Drive, for one) go to every server in
the table, and an off-line server makes them stop early. Open files on the other servers then stay
open (p.52–53).

### 4.4 Building `SNIOS.SPR` (§4.2.5, p.57)

With RMAC and LINK-80:

```
A>RMAC SNIOS
A>LINK SNIOS[OS]
```

Without them, with ASM, PIP and `GENMOD` (GENMOD makes the relocation bit map from two origins):

```
A>ASM SNIOS                  (with ORG 0000H)
A>REN SNIOS0.HEX=SNIOS.HEX
A>ASM SNIOS                  (edit the source to ORG 0100H first)
A>REN SNIOS1.HEX=SNIOS.HEX
A>PIP SNIOS.HEX=SNIOS0.HEX,SNIOS1.HEX
A>GENMOD SNIOS.HEX SNIOS.SPR
```

The requester disk holds `CPNETLDR.COM`, `CPNETSTS.COM`, `NETWORK.COM`, `LOCAL.COM`,
`DSKRESET.COM`, `LOGIN.COM`, `LOGOFF.COM`, `MAIL.COM`, `NDOS.SPR`, `SNIOS.SPR` and `CCP.SPR`.

To debug under DDT (p.57–58): `DDT CPNETLDR.COM`, `IB`, `S103` and set the byte to the debugger's
restart number (usually `07`), then `G`. CPNETLDR loads, prints its memory map and stops at the
restart. Set breakpoints, then `G` to the address after the restart.

The manual's debugging advice: turn off every timeout in the SNIOS and the NETWRKIF first (the
sample's `ALWAYS$RETRY` switch). Never set a breakpoint in one side without one in the other side.

## 5. The Appendix E RS-232 protocol (App. E, p.96–121)

DRI's own point-to-point serial protocol, written "to encourage compatibility among hardware
vendors" (p.96). It works between any requester and server that have a spare serial port.

### 5.1 Handshake (Fig E-1, p.96, read as an image)

| Step | Sender → | ← Receiver |
|---|---|---|
| 1 | `ENQ` (`05`) | |
| 2 | | `ACK` (`06`) |
| 3 | `SOH` (`01`) FMT DID SID FNC SIZ `HCS` | |
| 4 | | `ACK` |
| 5 | `STX` (`02`) DB0 … DBn `ETX` (`03`) `CKS` `EOT` (`04`) | |
| 6 | | `ACK` |

Frame on the wire (Fig E-2, p.97):
`ENQ SOH FMT DID SID FNC SIZ HCS STX MSG… ETX CKS EOT`.

### 5.2 Checksums

- **HCS:** the two's complement of the sum, modulo 256, of `SOH` through `SIZ`. The sum of `SOH`
  through `HCS` is then zero (p.97).
- **CKS:** the two's complement of the sum of `STX`, the data **and `ETX`**. The sum of `STX`
  through `CKS`, `ETX` included, is then zero. ⚠ The Appendix E text says "from the STX, to the
  last byte of the MSG field" and leaves out ETX. The sample SNIOS adds ETX on both sides: the
  sender sends ETX through `Pre$Char$out`, which adds it to the checksum (PDF p.104, lines
  260–263; p.110, line 544). The receiver adds it with `add d` after the ETX check (p.113, lines
  651–652). **The listing wins**, because it is the shipped code.

### 5.3 ASCII variant (Fig E-3, p.97)

For a 7-bit line, each header byte, each data byte and each checksum byte is sent as two hex ASCII
characters, high nibble first. `ENQ`, `SOH`, `STX`, `ETX`, `EOT`, `ACK` and `NAK` stay one raw
byte. The checksums are on the byte values, not on the ASCII characters (sample `Net$out` /
`Net$in`, p.106–107). The text says ASCII mode is for debugging and that a link with 8-bit data
should use binary. ⚠ The text's CKS sentence for the ASCII form says the sum runs "from the FMT to
and including the CKS". That contradicts its own description of the binary form, and the shared
code. The code (§5.2) wins.

### 5.4 The sample SNIOS (Listing E-1, PDF p.99–121)

Assembled for an Altos ACS 8000 (equates also for a Dynabyte DB8/2 and a Digital Microsystems
DSC-2). To port it, change only `CHAROUT`, `CHARIN` and `DELAY` (§E.4, p.98).

| Item | Value | Listing |
|---|---|---|
| Requester ID | `slave$ID`: `56h` Altos, `12h` DB8/2, `34h` DSC-2 | p.100, lines 56–64 |
| Default server | `defaultmaster equ 00h` | p.100, line 97 |
| Send retries | `max$retries` = 10 | p.101, line 155 |
| Wait for the first reply | `timeout$retries` = 100 tries of `Char$in` (256 with `modem`) | p.101, lines 150–154 |
| `Char$in` timeout | 50 ms ("50 chars @ 9600 baud"). Altos 3 ms. `modem` 256 ms. | p.105–106, lines 333–343 |
| `xChar$in` (first byte of a reply) | 100 ms | p.105, line 330 |
| Delay after each byte sent | `DELAY`: 3 ms (non-Altos). Altos: 12 `XTHL` = 54 µs. | p.104–105, lines 236–245, 294–303 |
| ACK test | `ani 7fh`, compare with `06`. Any other byte is a retry. | p.110, lines 554–560 |
| Receive errors | Bad HCS or CKS: send `NAK` (`15`). Wrong byte or timeout: retry quietly. | p.113, lines 677–680 |
| Wrong DID | ACK is sent, `RECEIVEMSG` returns `FF` | p.113, lines 663–669 |
| `ALWAYS$RETRY` true | On failure, set the status bit, call `NTWRKERROR`, and start the send or receive again, without end | p.111–112, lines 566–574, 593–601 |

**The timeouts are instruction loops.** `Char$in` counts `5Ah` passes of a status poll for each
"millisecond", and `DELAY` counts `6 × 86h` passes of `DCR`/`JNZ`. At 8080 T-states those are
about 3,800 and 12,200 states, so the stated 1 ms and 3 ms are true at about **4 MHz** (this
figure is arithmetic from the listing, not a number the manual gives). At 2 MHz every timeout and
delay is twice as long. On a faster processor they are shorter. The 3 ms delay after each byte
also limits a non-Altos port to about 330 bytes a second, whatever the baud rate.

### 5.5 The sample NETWRKIF (§E.5, p.121)

Written for an Altos ACS 8000-10. It finds the first byte of a message through an XIOS `CONIN`
routine, then **turns off interrupts** and reads the rest of the message under program control.
A port with no XIOS `CONIN` routine needs one. Settings to change: `NMB$SLVS` (requesters, four
without more process descriptors), `MUTEXIN`/`MUTEXOUT`, `Z80`, the `BINARYASCII` table (per
port), the console and printer status ports and `CHARIOTBL`, and the I/O ports in `CHAR$OUT` and
`CHAR$IN`.

## 6. The server

### 6.1 MP/M II structure (§1.4, p.13–14; §4.3, p.58–80)

`SERVER.RSP` and `NETWRKIF.RSP` are resident system processes, built in with GENSYS. For each
requester there are two processes and two queues: `NtwrkIP<x>` (transport, in the NETWRKIF)
writes the message address to queue `NtwrkQI<x>`. `SERVR<x>PR` (in SERVER.RSP) reads it, does the
MP/M II calls, and writes the response to `NtwrkQO<x>`. `<x>` is a hex digit `0`–`F`. The server
answers in the buffer it was given, so the transport must not change a buffer between the two
queue operations (p.67).

The NETWRKIF writes the address of the server configuration table to **offset `0009` of the MP/M
II system data page**. SERVER.RSP reads the maximum requester count there and makes that many
copies of itself (p.68).

### 6.2 Server configuration table (Table 4-2, p.67–68)

Offsets are decimal.

| Offset | Bytes | Contents |
|---|---|---|
| 0 | 1 | Server status. No fixed use. |
| 1 | 1 | Server ID. Every incoming DID must match it, or the server returns an error. |
| 2 | 1 | Maximum requesters, up to 16 |
| 3 | 1 | Requesters logged in now. Login fails when it equals the maximum. |
| 4–5 | 2 | Login vector: one bit for each entry of the requester ID table |
| 6–21 | 16 | Requester ID table |
| 22–29 | 8 | Login password |

The manual's preloaded example (p.68): five requesters, IDs 1, 2 and 5 already logged in, at
table positions 0, 3 and 15, login vector `8009h`, password `'WUGGA'`.

### 6.3 Requester control block (Table 4-3, p.73)

For a server that reuses its processes for more than 16 requester nodes: byte 0 = requester ID
(`FF` = free), bytes 1–3 = pointer to that server's input queue, bytes 4–5 = pointer to byte 6,
bytes 6–7 = address of the message. A message from an unknown node that is not a login gets
extended error 12, "requester not logged in" (p.73).

### 6.4 A server that is not MP/M II (§4.4, p.80–81)

A server can run on any computer and operating system. `SERVER.RSP` cannot run there, so the
server must also do the logical work: a communications interface, a function interpreter
(Appendix C), a translator from CP/M FCBs to the host's files, and an interface to the host
operating system. A server that serves all requesters from one process must keep each requester's
state itself, for example the Search First / Search Next position and the shared files. The manual
says that CP/NET servers "have actually been implemented under CP/M" (p.80). A server can leave
out functions it cannot support.

## 7. Logical messages (App. C, p.86–94)

All values are hex. `ss` = server ID, `rr` = requester ID. The request is format `00` from `rr` to
`ss`, and the response is format `01` from `ss` to `rr`.

**Extended error.** A response with `SIZ = 01` is an error, whatever MSG(0) holds. The form is
MSG(0) = `FF`, MSG(1) = the extended error code. Functions marked *EE* can return one. Any
function can return "not logged in" or "function not implemented on server" (p.86).

**"Not implemented at server"** (functions 0–2, 6–10, 12, 13, 25, 26, 32, 45, 66–69): the request
has `SIZ 00`, and the response is `SIZ 01`, MSG = `FF 0C`. The NDOS handles these locally.

**FCB rules.** In a response FCB, byte 0 (the drive) is never copied back to the caller, and the
file ID bytes 22–24 only when F5' is set and F6' is not (open, close, make). For a search, the
32-byte directory entry is copied into the DMA buffer at the position that the directory code
gives. So the DMA buffer holds collected entries, not a real directory sector. A function that
returns an address (27, 31, 71) returns it into an NDOS buffer that the next network call
overwrites (p.86; §3.2, p.26).

| Fn | Name | Request SIZ / MSG | Response SIZ / MSG |
|---|---|---|---|
| 3 | Raw console input | `00` / server console | `00` / character |
| 4 | Raw console output | `01` / console, character | `00` / `00` |
| 5 | List output | `nn` / list number, 1–`80` characters | `00` / `00` |
| 11 | Console status | `00` / console | `00` / status |
| 14 | Select disk *EE* | `00` / disk | `00` / return code |
| 15 | Open file *EE* | `2C` / user, FCB (01–24), password (25–2C) | `24` / directory code, FCB |
| 16 | Close file *EE* | `2C` / user, FCB, password (ignored) | `24` / directory code, FCB |
| 17 | Search first *EE* | `25` / current disk if FCB(0) = `?`, user, FCB | `20` / code, directory entry. Full-search mode: `80` (sector) or `0`. |
| 18 | Search next *EE* | `01` / xx, user | as 17 |
| 19 | Delete file *EE* | `24` / user, FCB | `00` / directory code |
| 20 | Read sequential *EE* | `24` / user, FCB | `A4` / return code, FCB, 128-byte record |
| 21 | Write sequential *EE* | `A4` / user, FCB, record | `24` / return code, FCB |
| 22 | Make file *EE* | `24` / user, FCB | `24` / directory code, FCB |
| 23 | Rename file *EE* | `24` / user, FCB in rename form | `00` / directory code |
| 24 | Login vector | `00` / xx, sent to each remote drive's server | `01` / vector |
| 27 | Allocation vector *EE* | `00` / disk | `02`–`FF` / the vector |
| 28 | Write protect disk *EE* | `00` / disk | `00` / `00` |
| 29 | R/O vector | `00` / xx | `01` / vector |
| 30 | Set file attributes *EE* | `24` / user, FCB | `00` / directory code |
| 31 | Disk parameter block *EE* | `00` / disk | `0F` / DPB (16 bytes) |
| 33 | Read random *EE* | `24` / user, FCB | `A4` / return code, FCB, record |
| 34 | Write random *EE* | `A4` / user, FCB, record | `24` / return code, FCB |
| 35 | Compute file size *EE* | `24` / user, FCB | `24` / return code, FCB |
| 36 | Set random record *EE* | `24` / user, FCB | `24` / return code, FCB |
| 37 | Reset drive *EE* | `01` / drive vector | `00` / return code |
| 38 | Access drive *EE* | `01` / drive vector | `00` / return code |
| 39 | Free drive *EE* | `01` / drive vector | `00` / return code |
| 40 | Write random, zero fill *EE* | `A4` / user, FCB, record | `24` / return code, FCB |
| 42 | Lock record *EE* | `26` / user, FCB, file ID (25–26) | `24` / return code, FCB |
| 43 | Unlock record *EE* | `26` / user, FCB, file ID | `24` / return code, FCB |
| 64 | Login *EE* | `07` / 8-character password | `00` / return code |
| 65 | Logoff *EE* | `00` / xx | `00` / return code |
| 70 | Set compatibility attributes | `00` / attributes, sent to all known servers | `00` / xx |
| 71 | Return server configuration *EE* | `00` / xx | `16` / temp-file drive, status, ID, max, logged in, login vector (05–06), requester IDs (07–16) |
| 106 | Set default password | `07` / password, sent to all known servers | `00` / return code |

**2016 additions (not in DRI's 1982 text).** The re-typeset marks these "Extension for CP/M 3":
46 Get disk free space (response `02`, 3 bytes little-endian), 48 Flush buffers, 98 Free blocks,
99 Truncate file *EE*, 101 Get directory label byte, 102 Get file time stamps *EE*, 105 Get date
and time ("Not issued by NDOS": days since 12/31/1977, then BCD hours, minutes, seconds), and the
"full search" responses of 17 and 18 (p.89, p.92–94). A period server need not answer them.

## 8. NDOS functions beyond CP/M 2.2 (App. D, p.95)

| Fn | Name | Input | Output |
|---|---|---|---|
| 38 | Access drive | DE = drive vector | — |
| 39 | Free drive | DE = drive vector | — |
| 42 | Lock record | DE = FCB | A = error code |
| 43 | Unlock record | DE = FCB | A = error code |
| 45 | Set BDOS error mode | E = mode | — |
| 64 | Login | see §3.3 of the manual | A = error code |
| 65 | Logoff | E = server ID | — |
| 66 | Send message on network | DE = message | A = error code |
| 67 | Receive message from network | DE = message | A = error code |
| 68 | Get network status | — | A = status byte (§4.2) |
| 69 | Get config table address | — | HL = table |
| 70 | Set compatibility attributes | E = attributes | — |
| 71 | Get server configuration | E = server ID | HL = table |
| 106 | Set default password | see §3.3 of the manual | — |

Functions 66 and 67 are outside the normal request/response rule. A NETWRKIF must recognize a
special format code and keep that message away from the server processes (§4.3.4, p.74).

## 9. The user commands (§2, p.16–18)

| Command | Form | Notes |
|---|---|---|
| `LOGIN` | `LOGIN {password}{[id]}` | Default password `PASSWORD`, default server `[00]`. A login survives a power cycle of the requester. |
| `LOGOFF` | `LOGOFF {[id]}` | |
| `NETWORK` | `NETWORK local{=server-dev{[id]}}` | e.g. `NETWORK B:=D:[F]`, `NETWORK LST:=3[07]`, `NETWORK CON:=2` |
| `LOCAL` | `LOCAL dev` | Back to local |
| `ENDLIST` | `ENDLIST` | Sends `FF` to the list device to end a network print job. The CCP does this when a program ends, unless Ctrl-P is on. |
| `DSKRESET` | `DSKRESET {d:,...}` | Prints `***Reset Failed***` on failure |
| `CPNETLDR` | `CPNETLDR` | Prints `CP/NET 1.2 Loader` and the memory map |
| `CPNETSTS` | `CPNETSTS` | Shows the configuration table |

## 10. CP/NOS (App. A, p.82–84)

CP/NOS is a diskless requester. It has only a console and a printer of its own, and every disk is
on the network. It is about 4K and can be in ROM, on an 8080, 8085 or Z80. It needs an MP/M II
server, the same one as CP/NET uses. The site writes `CPBIOS` (only `BOOT`, `CONST`, `CONIN`,
`CONOUT`, `LIST`, `LISTST`, with the full BIOS jump vector) and `CPNIOS` (the SNIOS with
`cpnos equ true`). The CPNIOS must set up the whole configuration table at cold start and ask for
the login, because CP/NOS has no disk to load `NETWORK` or `LOGIN` from. The sample's CP/NOS table
maps all 16 drives to server 0 (A: = server drive E:, E: = server drive A:, PDF p.100). Link
order: `CPNOS, CPNDOS, CPNIOS, CPBDOS, CPBIOS`, for example
`LINK CPNOS,CPNDOS,CPNIOS,CPBDOS,CPBIOS[LF000,DEC00]`, with at least 1K of RAM for the data
segments.

---

## Quirks worth carrying

- ⚠ **CKS covers ETX.** The text says the data checksum runs from STX to the last data byte. The
  sample SNIOS adds ETX on both the send side and the receive side. Follow the code (§5.2).
- ⚠ **The ASCII-mode CKS text starts the sum at FMT**, and the binary-mode text starts it at STX.
  The shared code starts at STX (§5.3).
- ⚠ **"As shown in Appendix A"** (§3.1.1, p.24) and "as defined in Appendix A" (E.2 and E.3, p.97)
  mean **Appendix B**. Appendix A is CP/NOS.
- ⚠ **The version number is `022x`**, not `002x`. A program that checks for CP/M 2.2 can refuse
  to run (§3.2, p.26).
- ⚠ **Temporary file names change** on a network drive (`$$$.SUB`, `*.$$$`), so that two
  requesters do not use the same file. This can confuse a post-mortem of a failed program (§3.2,
  p.26; §3.2.3, p.30).
- ⚠ **The allocation vector (fn 27) and the DPB (fn 31) of a network drive are in an NDOS
  buffer** that the next network message overwrites. Copy them out first (p.26, p.86).
- ⚠ **A login vector or R/O vector cannot carry an error.** The NDOS combines what all servers and
  the local BDOS return (p.86).
- ⚠ **The sample timeouts are instruction loops for about 4 MHz** (§5.4). A different processor
  speed changes every timeout and the delay after each byte.
- ⚠ **Never map a device to an off-line server** (§4.3). A Free Drive that stops early leaves
  files open on the servers that answered.
- ⚠ **The 2016 re-typeset adds CP/M 3 functions** (§7) that DRI's 1982 CP/NET 1.2 does not have.
  A period NDOS never sends them, except the full-search forms, which a period server will not
  know.
