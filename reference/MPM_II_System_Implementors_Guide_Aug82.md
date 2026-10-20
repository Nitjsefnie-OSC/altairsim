# Digital Research MP/M II — System Implementor's Guide (XIOS, LDRBIOS, GENSYS)

Source: [MPM_II_System_Implementors_Guide_Aug82.pdf](#) (*MP/M II Operating System, System
Implementor's Guide*, © 1981 Digital Research), 173 PDF pages, **OCR text layer** (Acrobat Paper
Capture, 2010). The OCR has errors in prose and listings ("Digi tal", `0`/`O` swaps), so every table
and number below was read from the page image. Page numbers are the **printed** ones. Printed
page N is PDF page N+9.

MP/M II is DRI's multi-user, multi-tasking operating system for the 8080, 8085 and Z80. DRI ships
the logical parts finished: the XDOS nucleus, the BDOS (resident and banked parts), the TMP, the
resident system processes and the utilities. A site writes three hardware parts:

- the **XIOS**, which is the BIOS plus the MP/M extensions
- the **LDRBIOS**, a small CP/M 2 BIOS that the loader uses
- the **cold-start loader**

The guide is the specification for those three parts. **Not
emulated.** MP/M II is software that an emulated machine runs. The DRI MP/M II 2.1 distribution
disks were found, and they boot on no machine altairsim models. The plan for an Altair XIOS is in
issue #820. MP/M II is also the server for CP/NET, which [Digital Research CP/NET 1.2](dri-cpnet.md) covers.

This file keeps what an XIOS writer needs: the build chain, the boot and disk layout, the BIOS and
XIOS entry points, the interrupt and clock rules, the system data page, the GENSYS questions and
the loader. It leaves out the DISKDEF macro source (App. A), most of the deblocking listing
(App. B) and the full sample listings (App. D, E). Section 7 gives each sample's hardware.

---

## 1. Quick reference for an XIOS writer

| What | Value | Where |
|---|---|---|
| Parts the site writes | `BNKXIOS.SPR` (banked memory) or `RESXIOS.SPR` (no banks), `LDRBIOS`, the cold-start loader | §1, p.1 |
| Precondition | A working CP/M 2 BIOS on the target machine. MPMLDR loads MP/M II through a CP/M 2 BIOS (the LDRBIOS). | §1.1, p.1 |
| Build the XIOS | `RMAC BNKXIOS`, then `LINK BNKXIOS[OS]` gives `BNKXIOS.SPR` | §1.3, p.3 |
| Build the system | `GENSYS` (under CP/M or MP/M II) writes `MPM.SYS`. `GENSYS $A` takes every default. | §5, p.51–57 |
| Load the system | `MPMLDR` reads `MPM.SYS` (user 0) and jumps to it | §6, p.59 |
| LDRBIOS origin | `ORG 1700H`. Needs only console output, `SELDSK` and disk reads from one drive. | §1.2, p.2 |
| LDRBIOS top | Below `1A00H` if MPMLDR must fit on two floppy system tracks (⚠ see Quirks) | §1.2, p.2 |
| First LDRBIOS call | `SELDSK`. Put device initialization there. | §1.2, p.2 |
| BIOS vector | 17 jumps, `BIOS+00H`–`BIOS+30H` (§3 below) | §2.1, p.15 |
| XIOS vector | 8 jumps right after it, `BIOS+33H`–`BIOS+48H` (§4 below) | §3.1, p.39 |
| First development step | Polled I/O and no interrupts. The system runs without a clock interrupt. Add interrupts last. | §1.3, p.3 |
| System tick | Flag #1 at each tick while `STARTCLOCK` is in effect. 60 Hz (16.67 ms) is recommended. At 50 Hz, use 20 ms. | §3.4, p.44 |
| One-second flag | Flag #2, once each second, always | §3.4, p.44 |
| Only call from an interrupt handler | XDOS function 133, Flag Set. End the handler with a jump to `PDISP`. | §3.3, p.44 |
| Consoles | Up to 16 character devices. Up to 8 are system consoles with a TMP. | §2.2, p.16; §5.2.3, p.53 |
| Printers | Up to 16 list devices | §2.2, p.16 |
| Disks | Up to 16 drives, A–P | Table 2-1, p.19 |
| Flags | 32 under MP/M II | §3.3, p.44 |
| Disk retries | 10 before an error is returned | §2.2, p.16 |

## 2. Alteration procedure (Section 1)

### 2.1 Put a LDRBIOS into MPMLDR.COM (§1.2, p.2–3)

1. Assemble `LDRBIOS.HEX` at `ORG 1700H`.
2. `DDT MPMLDR.COM`. DDT shows `NEXT 1780`, `PC 0100`.
3. `ILDRBIOS.HEX`, then `R`. The LDRBIOS part of the loader is overlaid. DDT shows `NEXT 1A00`.
4. `G0` returns to the CCP.
5. `SAVE 26 XMPMLDR.COM`. Test it, then rename it to `MPMLDR.COM`.

Make sure that the LDRBIOS never writes to the disk during a read.

### 2.2 Debug an XIOS under CP/M (§1.4, p.4–6)

1. Load DDT and list location `0005`. The jump goes to the base of DDT (`C800` in the example).
2. Run GENSYS under CP/M. Set the top page below DDT, with at least 256 bytes for patches (`C6` in
   the example). Give MP/M II a breakpoint RST other than DDT's, which is RST 7. The guide
   suggests RST 6.
3. `DDT MPMLDR.COM`. Put `$B` at `005D`–`005E` (in the default FCB at `005C`) with `I$B`. MPMLDR
   then breaks to DDT after it loads `MPM.SYS`. A fourth character gives the CP/M debugger's
   RST: `I$B6`. The default is 7.
4. `G`. The loader prints the memory map. Note the addresses of the XIOS and of `SYSTEM.DAT`.
5. `X` shows the PC at the RST. MP/M II starts at the next byte (`09F3` in the example).
6. Start it with `G09F3,<breakpoint>`. The example breaks on the XIOS list entry, `C30F`, when
   `XIOSJMP TBL` is at `C300H`.

DDT runs with interrupts enabled. Keep console #0 polled while you debug, because DDT gets no
input through an interrupt-driven console #0 handler. The guide recommends a debug build of the XIOS
with console #0 polled.

### 2.3 Boot MP/M II directly (§1.5, p.6–10)

The cold-start loader and MPMLDR go on the first two tracks of an 8" disk. Change the CP/M 2
cold-start loader to load at `0100H` and run at `0100H`. The examples are for the Intel MDS-800,
whose boot ROM loads track 0 sector 1 at `3000H`.

- **With SYSGEN:** in DDT, `IMPMLDR.HEX`, `R880`, then `IBOOT.HEX`, `RD900` (the boot lands at
  `0900H`). `G0`, then `SYSGEN` with no source and a destination drive.
- **Without SYSGEN:** write a GETSYS at `0100H` that reads the boot to `3300H` and MPMLDR to
  `3380H`. Write a PUTSYS at `0200H` that writes memory from `3380H` back to tracks 0 and 1.
  The guide prints skeletons of both (§1.5.3, p.9–10). They read sectors 1–26 of tracks 0 and 1,
  128 bytes each.

### 2.4 Disk organization (§1.8, Table 1-2, p.12–13)

Two tracks of 26 sectors of 128 bytes, IBM 3740 single density:

| Track | Sectors | Memory | Contents |
|---|---|---|---|
| 0 | 1 | boot address | Cold-start loader |
| 0 | 2–25 | `0100H`–`0CFFH` | MPMLDR |
| 0–1 | T0 26, T1 1–19 | `0D00H`–`16FFH` | LDRBDOS |
| 1 | 20–26 | `1700H`–`1A7FH` | LDRBIOS |

If the controller has no built-in load of track 0 sector 1, start the load at track 0 sector 2.

### 2.5 MPM.SYS file format (§1.6, Table 1-1, p.11)

| Record | Contents |
|---|---|
| 1 | First 128 bytes of the system data page |
| 2 | Second 128 bytes of the system data page |
| 3–n | The operating system, in reverse order, top down |

Byte `000` of the system data page gives its page in memory. The rest of the system goes directly
below it. Bytes `120`–`121` hold n. Byte `011` (XDOS base page) gives the execution address. A
custom loader can place `MPM.SYS` without MPMLDR.

## 3. The BIOS part (Section 2)

### 3.1 Jump vector (§2.1, p.15)

| Offset | Entry | MP/M II use |
|---|---|---|
| `00H` | `COMMONBASE` | Start of the part that must be in common memory. Calling it ends the process. |
| `03H` | `WBOOT` | XDOS terminate process (function 0) |
| `06H` | `CONST` | Console status. Console number in **D**. `0FFH` = ready. |
| `09H` | `CONIN` | Console input from console **D**. Parity bit 0. |
| `0CH` | `CONOUT` | Character in **C** to console **D** |
| `0FH` | `LIST` | Character in **C** to list device **D** |
| `12H` | `PUNCH` | Not used |
| `15H` | `READER` | Not used |
| `18H` | `HOME` | Track 0 |
| `1BH` | `SELDSK` | Drive in **C** (0–15). **E** bit 0 = 0 on the first select. Returns the DPH address in HL, or `0000H`. |
| `1EH` | `SETTRK` | Track in **BC** |
| `21H` | `SETSEC` | Translated sector in **BC** |
| `24H` | `SETDMA` | DMA address in **BC**. **`0FFFFH` means flush the blocking buffer.** |
| `27H` | `READ` | Returns **A** = 0 or 1 |
| `2AH` | `WRITE` | Write type in **C**. Returns **A** = 0, 1, or 2 (read-only). |
| `2DH` | `LISTST` | List status of device **D**. Not used by the standard utilities. |
| `30H` | `SECTRAN` | Logical sector in **BC**, table in **DE**, physical sector in HL |

Rules that differ from CP/M 2.2 (Table 2-1, p.17–23):

- Every character entry takes a **device number in D** and must be re-entrant.
- Every entry except the disk entries must be above `COMMONBASE`, in common memory.
- `READ` and `WRITE` also get the **absolute record number**: the high byte in B and the low two
  bytes in DE.
- A flush (`SETDMA 0FFFFH`) that fails pops its return address and returns an error in A:
  1 = non-recoverable, 2 = read-only.
- On an error, the BDOS prints `BDOS ERR ON x: BAD SECTOR`. The process's error mode decides
  whether it ends or gets an error code.
- The first `SELDSK` only gets the `DIRBUF` address. It needs no disk I/O.
- The standard skew is 6. Keep a single-density IBM version with skew 6 for exchanging disks.

### 3.2 Disk tables (§2.3, p.24–33)

The Disk Parameter Header (16 bytes, one for each drive) is `XLT, 0000, 0000, 0000, DIRBUF, DPB,
CSV, ALV`. The Disk Parameter Block is `SPT(16) BSH(8) BLM(8) EXM(8) DSM(16) DRM(16) AL0(8)
AL1(8) CKS(16) OFF(16)`. These are the CP/M 2.2 tables. Table 2-4 (BSH/BLM) and Table 2-5 (EXM)
were read from the page image:

| BLS | BSH | BLM | EXM, DSM < 256 | EXM, DSM > 255 | Directory entries per block |
|---|---|---|---|---|---|
| 1,024 | 3 | 7 | 0 | N/A | 32 |
| 2,048 | 4 | 15 | 1 | 0 | 64 |
| 4,096 | 5 | 31 | 3 | 1 | 128 |
| 8,192 | 6 | 63 | 7 | 3 | 256 |
| 16,384 | 7 | 127 | 15 | 7 | 512 |

- `CKS` = (DRM+1)/4 for removable media. **`CKS = 8000H` marks a drive as permanent**, with no
  directory check. That is an MP/M II addition.
- The ALV area is (DSM/8)+1 bytes.
- `DISKDEF dn,fsc,lsc,[skf],bls,dks,dir,cks,ofs,[k16],[prm]`. The optional `prm` marks a
  permanent drive. The standard 8" drive is `DISKDEF 0,1,26,6,1024,243,64,64,2`. Its skew-6
  table is `1,7,13,19,25,5,11,17,23,3,9,15,21,2,8,14,20,26,6,12,18,24,4,10,16,22` (p.32).

### 3.3 Write types and deblocking (§2.5, p.36–37; App. B)

`WRITE` gets a write type in C:

| C | Meaning |
|---|---|
| 0 | Deferred write |
| 1 | Non-deferred write |
| 2 | Deferred write to the first sector of a new block |
| 3 | Non-deferred write to the first sector of a new block |

Types 0 and 2 occur only on permanent drives. Type 1 is also used for directory writes on a
permanent drive. The Appendix B algorithm skips the pre-read when bit 1 is set. Its sample host
has 512-byte sectors, 20 to a track, and 2,048-byte blocks.

### 3.4 What the BNKXIOS must keep in common memory (§2.6, p.37)

- Every BIOS and XIOS entry, except the disk entries
- The DPHs, the DPBs and `DIRBUF`
- All disk polling code and all interrupt handlers
- The code that moves data into the user's DMA buffer

A deblocking buffer can be in banked memory. That needs a 128-byte buffer in common memory and one
more move. The bank must not change while a controller does DMA into banked memory.

### 3.5 External procedures (§2.4, Table 2-7, p.33–35)

GENSYS fills in a jump vector at `COMMONBASE`:

```
COMMONBASE: JMP COLDSTART
SWTUSER:    JMP $-$     ; switch to the calling process's bank
SWTSYS:     JMP $-$     ; switch back to the BNKBDOS bank
PDISP:      JMP $-$     ; force a dispatch (end of an interrupt handler)
XDOS:       JMP $-$     ; XDOS functions
SYSDAT:     DW  $-$     ; address of the system data page (not a jump)
```

Call `SWTUSER` and `SWTSYS` only from common memory and only from the disk functions. Both disable
interrupts and then **enable** them again, even if interrupts were disabled before the call.

## 4. The XIOS part (Section 3)

### 4.1 Jump vector (§3.1, p.39)

| Offset | Entry | Rule |
|---|---|---|
| `33H` | `SELMEMORY` | BC points to a memory descriptor: base, size, attributes and bank, one byte each. Called from the dispatcher with interrupts disabled. Must not enable them. |
| `36H` | `POLLDEVICE` | Device number in C. Returns `0FFH` if ready, `00H` if not. Called in a critical region. Must not enable interrupts. |
| `39H` | `STARTCLOCK` | A process is on the delay list. Start setting flag #1 at each tick. |
| `3CH` | `STOPCLOCK` | The delay list is empty. Stop setting flag #1. |
| `3FH` | `EXITREGION` | If `PREEMPT` is true, leave interrupts disabled. If it is false, enable them. |
| `42H` | `MAXCONSOLE` | Number of physical consoles in A |
| `45H` | `SYSTEMINIT` | Cold-start initialization (below) |
| `48H` | `IDLE` | Optional. If it is not supplied, put three zero bytes here. |

Every entry must be above `COMMONBASE` (Table 3-1, p.39–43).

- **One clock for both time bases.** When a single timer gives the tick and the second,
  `STARTCLOCK` and `STOPCLOCK` set a flag byte. The interrupt handler tests it before it sets
  flag #1.
- **`SYSTEMINIT`.** MP/M II disables interrupts and calls `SYSTEMINIT` before any other XIOS call.
  It enables interrupts when `SYSTEMINIT` returns. Bank 0 is in place, and MP/M II has already
  set its base page. On entry:
  - **C** holds the MP/M debugger RST number.
  - **DE** holds the debugger entry. Put a jump to DE at that RST in each bank.
  - **HL** holds the BIOS direct jump table. Put a jump to HL at `0000H` in each bank.

  `SYSTEMINIT` also sets the interrupt vectors and masks. MPMLDR and MP/M II both assume bank 0
  is selected when MPMLDR runs.
- **`IDLE`.** The normal idle is a call to the dispatcher, which serves the polled devices. A
  system with no polled devices can use `HLT` then `RET`.

### 4.2 Interrupt service (§3.3, p.43–44)

1. Save the registers with as little stack as possible. A CP/M program gives no spare stack. The
   samples switch to a local stack and add no levels beyond the RST itself.
2. Set `PREEMPT` true.
3. Find the cause, clear it, and set a flag with XDOS function 133. Any other XDOS or BDOS call
   dispatches and enables interrupts too early.
4. If the processor needs `RETI`, execute it before the jump to `PDISP`.
5. Jump to `PDISP`. The dispatcher runs the highest-priority ready process and enables interrupts.

Use interrupts for asynchronous events, such as console input and disk completion. Leave console
output polled, because a dispatch for each character costs more, especially at high baud rates.

### 4.3 Time base (§3.4, p.44–45)

- Flag #2: once each second of real time. MP/M II keeps the time of day with it.
- Flag #1: at each tick, from `STARTCLOCK` to `STOPCLOCK`. The delay list runs on it.
- The tick rate is also the dispatch rate for compute-bound processes. A high rate costs
  overhead. A low rate lets one process keep the processor for longer.
- GENSYS records the rate in the system data page, byte 122, for programs to read. It does not
  set the hardware.

## 5. MPM.SYS components and the system data page (Section 4)

`MPM.SYS` holds the system data page, the XIOS, RESBDOS and BNKBDOS, XDOS and BNKXDOS, the TMP
and the resident system processes. It is in user 0, usually read-only.

Table 4-1, system data page (p.47–48, read from the page image):

| Byte | Contents |
|---|---|
| 000 | `Mem$top`, top page of memory |
| 001 | `Nmb$cns`, number of system consoles (TMPs) |
| 002 | `Brkpt$RST`, breakpoint RST number |
| 003 | Add system call user stacks, boolean |
| 004 | Bank switched, boolean |
| 005 | Z80 version, boolean |
| 006 | Banked BDOS, boolean |
| 007 | XIOS jump table page |
| 008 | RESBDOS base page |
| 009–010 | **CP/NET master configuration table address** |
| 011 | XDOS base page |
| 012 | RSPs (BNKXIOS top+1) base page |
| 013 | BNKXIOS base page |
| 014 | BNKBDOS base page |
| 015 | `Max$mem$seg`, maximum memory segment number |
| 016–047 | Initial memory segment table |
| 048–063 | Breakpoint vector table, filled in by debuggers |
| 064–079 | Reserved |
| 080–095 | System call user stack pointer table |
| 096–119 | Reserved |
| 120–121 | Number of records in `MPM.SYS` |
| 122 | Ticks per second |
| 123 | System drive |
| 124 | Common memory base page |
| 125 | Number of RSPs |
| 126–127 | `Listcp` array address |
| 128–143 | `Subflg`, submit flag array |
| 144–186 | Reserved |
| 187 | Maximum locked records per process |
| 188 | Maximum open files per process |
| 189–190 | Number of list items |
| 191–192 | Pointer to the base of lock table free space |
| 193 | Total system locked records |
| 194 | Total system open files |
| 195 | Dayfile logging, boolean |
| 196 | Temporary file drive |
| 197 | Number of printers |
| 197–241 | Reserved (⚠ printed as 197, see Quirks) |
| 242 | Banked XDOS base page |
| 243 | TMP process descriptor base |
| 244 | `Console.dat` base |
| 245–246 | BDOS/XDOS entry point |
| 247 | `TMP.spr` base |
| 248 | `Nmbrsps`, number of banked RSPs |
| 249 | BRSP base address |
| 250–251 | `Brspl`, non-resident RSP process link |
| 252–253 | `Sysdatadr`, XDOS internal data segment address |
| 254–255 | `Rspl`, resident system process link |

Bytes 009–010 are where the CP/NET server's NETWRKIF writes its configuration table address
([dri-cpnet.md](dri-cpnet.md) §6.1).

**Resident system processes** (`.RSP`, §4.5, p.49) are page-relocatable. Bytes 0–1 hold the
BDOS/XDOS address, and the process descriptor starts at byte 2. A **banked RSP** (§4.6, p.49–50)
has a resident part with the descriptor, whose memory segment index is 0 and not `0FFH`. It also
has a `.BRS` code part:

| BRS bytes | Contents |
|---|---|
| 0–1 | Address of the resident part |
| 2–3 | Initial stack pointer. The top of the stack holds the entry address. |
| 4–11 | ASCII process name |

## 6. GENSYS and MPMLDR (Sections 5 and 6)

GENSYS questions, with the defaults from the sample run (p.51–57). A number is hex. A `#` before it
means decimal.

| Question | Sample default | Notes |
|---|---|---|
| Use SYSTEM.DAT for defaults | Y | Or the built-in defaults |
| Top page of operating system | FF | The highest address used is `xxFFH` |
| Number of TMPs (system consoles) | #2 | Each needs 256 bytes of `CONSOLE.DAT`. Four TMP descriptors fit in each page of `TMPD.DAT`. `MAXCONSOLE` lowers it at start-up. |
| Number of printers | #1 | Used by MPMSTAT |
| Breakpoint RST | 06 | RST 1 to RST 6 are recommended |
| Add system call user stacks | Y | Needed to run CP/M `.COM` files. `USERSYS.STK` is 100H for 0–3 segments, 200H for 4–7. |
| Z80 CPU | Y | The dispatcher saves the alternate registers. Answer Y only on a Z80. |
| Number of ticks/second | #60 | For programs to read |
| System disk | E: | Second search drive for the CLI |
| Temporary file drive | E: | SUBMIT's `$n$.SUB` |
| Maximum locked records/process | #16 | 0–255, not more than the system total |
| Total locked records/system | #32 | 0–255 |
| Maximum open files/process | #16 | 0–255, not more than the system total |
| Total open files/system | #32 | 0–255 |
| Bank switched memory | Y | |
| Number of user memory segments | #3 | 1–7, at least the number of system consoles |
| Common memory base page | C0 | GENSYS checks that common modules are above it |
| Dayfile logging at console | Y | Prints time, file and user for each command |

After the RSPs are selected, GENSYS asks for the memory segment table as `base,size,attrib,bank`.
- Entry 0 is the operating system (bank 0). It is not counted in the user segment count.
- List the segments based at `0000H` from the largest to the smallest. The memory manager takes the
  first free one for a `.COM` program.
- An attribute of `FFH` pre-allocates a segment.
- The bank byte is an index. The XIOS turns it into the value for the bank hardware.

The sample run gives this map (p.51–52, read from the page image):

| Module | Base | Size |
|---|---|---|
| `SYSTEM DAT` | `FF00H` | `0100H` |
| `TMPD DAT` | `FE00H` | `0100H` |
| `USERSYS STK` | `FD00H` | `0100H` |
| `XIOSJMP TBL` | `FC00H` | `0100H` |
| `RESBDOS SPR` | `F000H` | `0C00H` |
| `XDOS SPR` | `CE00H` | `2200H` |
| `ABORT`, `SPOOL`, `MPMSTAT RSP` | `CD00H`, `CC00H`, `CB00H` | `0100H` each |
| `BNKXIOS SPR` | `B800H` | `1300H` |
| `BNKBDOS SPR` | `9500H` | `2300H` |
| `BNKXDOS SPR` | `9200H` | `0300H` |
| `TMP SPR` | `8F00H` | `0300H` |
| `SPOOL BRS`, `MPMSTAT BRS` | `8700H`, `7900H` | `0800H`, `0E00H` |
| `LCKLSTS DAT`, `CONSOLE DAT` | `7700H`, `7500H` | `0200H` each |
| Segments | System: `7500H`, `8B00H`, bank 0. User: `0000H`, `C000H`, banks 1 and 2. User: `0000H`, `7500H`, bank 0. | |

**MPMLDR** (§6, p.59–60) needs no operator input. It prints the console count, the breakpoint RST,
the Z80 line and the memory map, then starts MP/M II. Its parameters are:

- `$Bn` in the default FCB name field from `005DH`, to break to a CP/M debugger. The default n is 7.
- Another `.SYS` file name in the FCB at `005CH`, or at `006CH` with `$Bn`. A LDRBIOS can ask for a
  name at `SELDSK` and put it there.

## 7. The sample listings (Appendixes C, D, E)

| App. | What | Hardware it assumes |
|---|---|---|
| C, p.75–76 | Skeleton LDRBIOS, `ORG 1700H` | None. Every routine is a `RET`. |
| D, p.77–97 | Simple RESXIOS, `MP/M II V2.0 DSC-2 Basic & Extended I/O Systems` ("bios for micro-2 computer"). It is `RESXIOS.ASM` on the distribution disk. | Z80 (`IM 1`) at 4 MHz (`dlycnst 086h`). No banks and no protection: `SELMEMORY` is a `RET`. Three consoles, with data/status at `40h/41h`, `50h/51h` and `58h/59h`. The printer (a TTY Model 40) is at `48h/49h`. Status bit 1 = input ready, bit 0 = output ready. Disk controller at `80h`–`83h`. Interrupt mask at port `60h`, bit 6 = the 60 Hz clock. Every interrupt is an RST 7 (`0038h`). One 8" drive type: `DISKDEF 0,1,58,,2048,256,128,128,2`. |
| E, p.99–159 | Banked XIOS `XIOS200` by Altos Computer Systems, version 1.12 and later, for the Altos Sun Series 8000. Printed by permission "as a sample". | Z80 with a CTC. The vector `60H` goes to port `30H`. Channel 3 (port `33H`) gets control `0A7H` and time constant 250. Bank select at port `09H` (`MEMPORT`): the bank number shifted left 3, masked with `18H`. WD1791 floppy and hard disk. Virtual disk in banks 1–3. Tick 62.5 per second. |

Flags used by the Appendix D handlers (p.88–90):

| Flag | Set by |
|---|---|
| 1 | The 60 Hz clock, while `tickn` is true |
| 2 | The 60 Hz clock, every 60th tick |
| 5 | Disk interrupt (status bit 3) |
| 6 | Console #0 input (only when `debug` is false) |
| 8 | Console #2 input |

## Quirks worth carrying

- ⚠ **Byte 197 is listed twice in Table 4-1** (p.48). The table has "197-197 Number of printers"
  and then "197-241 Reserved", and the page image confirms both. Nothing in the guide settles
  it. The likely reading is that the reserved range starts at 198. (Unresolved.)
- ⚠ **The LDRBIOS top.** The text says the upper limit is `1A00H` (p.2). Table 1-2 (p.13) gives
  the LDRBIOS seven sectors, `1700H`–`1A7FH`, and 2 × 26 − 1 sectors from `0100H` also end at
  `1A7FH`. Both readings fit if the LDRBIOS stays below `1A00H`, as the text says.
- ⚠ **The sample XIOS puts every interrupt on RST 7**, and its `SYSTEMINIT` writes a jump at
  `0038H` (App. D, p.87). §1.4 says that DDT and SID use RST 7. Running the sample XIOS under DDT
  breaks DDT's breakpoints. On a machine with a vectored interrupt board, use a level other than 7.
- ⚠ **The Appendix E comments contradict its code** (p.148–149). The handler is labeled `T20MS`,
  and a comment says the dispatch is forced "each 1/32nd of a second". The code counts 62 and 63 in
  turn ("ACTUAL #/SEC = 62.5"). The code wins: 62.5 ticks a second, 16 ms. The CTC setup agrees. Control `0A7H` selects the ÷256 prescaler, so 256 × 250 = 64,000 clocks is 16 ms with a 4 MHz clock (p.157). The listing does not state the clock rate. The heading "MP/M 1.0
  INTERRUPT HANDLERS" is also left over from MP/M 1.0.
- ⚠ **The version is 2.0 in the guide and 2.1 on the disks.** The guide and its samples say V2.0.
  The GENSYS sample prints `MP/M-80 V2.0 System Generation`. The DRI distribution disks are labeled
  Version 2.1. Expect banners that differ from the guide.
- ⚠ **Write types use CP/M 2.2 names in Appendix B.** The listing names C = 0, 1, 2 `wrall`,
  `wrdir`, `wrual` (p.67). §2.5 defines them as deferred or non-deferred, with bit 1 meaning a new
  block. Bit 1, which the algorithm tests, means the same in both.
- ⚠ **`SETDMA 0FFFFH` is a flush, not an address** (p.20–21). A BIOS copied from CP/M 2.2 stores
  `0FFFFH` as the DMA address and then writes to it.
- ⚠ **`SWTUSER` and `SWTSYS` always enable interrupts on return** (p.35), even when the caller had
  them disabled.
- ⚠ **A wrong cross-reference.** §5.2.15 (p.55) says the bank questions are "as shown in Section
  5.2.2". They are §5.2.16 and §5.2.17.
