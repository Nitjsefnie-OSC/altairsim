# Writing a video board

The lamp chapter built a board that latches a byte. This one builds a board that **paints** —
and it is the same board with one more connection: a host video service it draws into. The
worked example is **CADzilla** (`src/boards/cadzilla.{h,cpp}`, `docs/boards/cadzilla.md`), an
HD63484 ACRTC with a Bt453 RAMDAC, chosen because it exercises every part of the seam: a chip
with its own frame memory, a chip that *is* the palette, and a picture you can only reach by
issuing commands. The Dazzler and the VDM-1 are the two simpler shapes and are cited where
they differ.

Everything here is tested with **no window**. That is not a limitation to work around; it is
the design, and the last section is how you prove a picture without one.

## 1. The nine questions

A video board is an ordinary `Board` that happens to paint, and nine decisions turn into code.
Answer them in the board's `docs/boards/*.md` **before** writing the class; seven become a
method each, and the other two are the Limitations section.

| # | Question | CADzilla's answer | Where it lands |
|---|---|---|---|
| 1 | **Name.** The chip or the common name, never a catalog number. "board", never "card" (`DESIGN.md` §0.3) | `cadzilla` | `type()`, the registry, the filename |
| 2 | **I/O footprint** — which ports, and *separately* which answer `IN` vs `OUT`: the bus decodes by cycle type | one 8-port block from `port`: `+0`/`+2` the ACRTC (both directions, different registers each way), `+1` the board's own MODE register (write-only), `+4`..`+7` the Bt453 (both directions); `+3`, the high byte of a 16-bit transfer an 8-bit host never makes, answers nothing | `decodes()`, `ioMap()` |
| 3 | **Memory footprint.** On-board screen RAM the CPU addresses (VDM-1 decodes a 1 KB window), a framebuffer in main RAM (Dazzler decodes none, reads RAM), or private memory (CADzilla decodes none, and the CPU never sees it) | none | `decodes()`, `memMap()` |
| 4 | **Where pixels come from and how you know they moved.** A write to on-board RAM latches a dirty flag; main RAM has to be polled against a shadow; a chip with its own memory tells you | `Hd63484::takeDirty()`, `Bt453::takeDirty()` | `pump()` |
| 5 | **Geometry.** Native w×h — fixed, decoded from registers each frame, or **the monitor's**: a board with a real CRT controller can carry a fixed-frequency display and place the chip's picture in it. `PixelFormat::Indexed8` (an index into the palette) or `PixelFormat::Rgb32` (the color itself) — §3 says when a board needs the second | the `mode` strap's VESA frame (one of the three primary VESA resolutions, 1024x768 by default); the ACRTC's picture placed by HDS/VDS against the mode's porches | `render()`, `acquire()` |
| 6 | **Palette.** How many entries, from where. Dazzler: 16 from an RGBI nibble. VDM-1: 2. CADzilla: **256 plus the overlay colors, and they are a chip** — so the chip, not the host, resolves them | `Bt453::lookup(p, ol)` | `render()` |
| 7 | **Observable timing.** A status bit a guest can *time* (vblank, scan parity) comes off `clock_->now()` — never a poll counter. An oscillator the guest cannot observe (a cursor blink) comes off `Display::hostSeconds()` | the drawing time at `draw_rate=real`: T-states carried into 2CLK (`sync()`, remainder kept), one Clock deadline at the command's end (`arm()`) so an interrupt lands with no bus cycle; the raster counter is not modeled | `sync()`, `arm()` |
| 8 | **Straps vs live status.** A strap has a setter and round-trips through `CONFIG SAVE`; live status has **no setter**, and that absence is the whole signal. A register that is write-only *on the wire* (a chip's format byte, or a board's own glue register) is still reflected as read-only live status — the board keeps the shadow the wire cannot give back. A fixed hardware fact with no jumper on the board (CADzilla's 2 MB of SRAM) is neither a strap nor live status — it is a constant. Every video board pushes `Display::widthProperty(videoWidth_)` | `port`, `mode`, `draw_rate`, `width`, `interrupt`; live `video`, `picture`, `wiring`, `hspol`/`vspol`/`amode`/`olen`/`olsel`, `status`, `irq` | `properties()` |
| 9 | **Snapshot.** Runtime state only — never a strap, never the `Display*`. If no memory board holds your pixels, **they travel with you** | both chips, frame memory and overlay SRAM as `blob`s | `serialize()` |

Two things that are *not* the board's business, and each is the classic mistake: a **keyboard**
is a separate board or an endpoint, never the window (`host/display.h`'s header note); and the
**frame rate** is the host's (`wantsFrame()`), never yours.

## 2. Chips first, if the board has any

CADzilla's two parts are chips before they are a board, in `src/chips/`, and that split is
the one the FD1771 and the 8257 already made (`chips/wd17xx.h`, `chips/i8257.h`): a chip
knows nothing about S-100 or about the board's ports. It has *pins* — the ACRTC's RS, the
Bt453's C1C0 — and the board decodes an address onto them:

```cpp
uint8_t CadzillaBoard::read(const BusCycle& c) {
    uint8_t off = (uint8_t)(c.port() - port_);
    if (off == 0 || off == 2) return acrtc_.read(off == 2);   // RS: +0 status, +2 data/FIFO
    return dac_.read(off & 3);                                // C1C0 = A1A0
}
```

The pay-off is that a chip is tested on its own, from its data sheet, with no bus at all —
`tests/test_hd63484.cpp` and `tests/test_bt453.cpp` — and the board test is left with only the
board's own decisions to prove. The **decisions a chip cannot make** are the board's, and they
belong in the board header where CADzilla's are: the shift register (8 bits per pixel, 8 words
per fetch, low byte first), the monitor (a fixed VESA frame), how much frame memory was fitted,
what the overlay inputs are tied to, where IRQ\* is strapped. Draw the line where the hardware draws it:
the ACRTC only ever puts an *address* on its bus, so `Hd63484` answers address-level questions
(`backgroundRaster()`, `windowRaster()`, `gaiWords()`) and the *board* fetches words and makes
pixels of them — which is also why a program that sets the chip to 4 bpp gets a scrambled
picture here, as it would on the board, instead of a helpfully re-unpacked one.

## 3. The board: three gates and one call

The whole host side of a video board is `pump()`, and it is three gates in a fixed order:

```cpp
void CadzillaBoard::pump() {
    if (!g_display) return;                      // no service at all (the bench)
    bool a = acrtc_.takeDirty();                 // consume BOTH flags every time, so one
    bool d = dac_.takeDirty();                   //   is never lost behind the other
    dirty_ = dirty_ || a || d;
    if (!dirty_) return;                         // deterministic: would it look different?
    if (!g_display->wantsFrame()) return;        // host-side rate limit; 0 = always, in tests
    render();
    dirty_ = false;
}
```

The order matters. Change detection is **deterministic** and always on: it is what makes a
headless test reproducible. The frame limit is **wall-clock** and off in tests: it is a host
economy that changes what is *drawn*, never what is *computed*. Put them the other way round
and a test's frame count depends on the machine it ran on.

`render()` is one call that is the window, and its arguments are not decoration:

```cpp
Surface* s = g_display->acquire(this, id, m.width, m.height, PixelFormat::Rgb32, videoWidth_);
```

`this` keys **this board's own window** (issue #234: two boards of the same resolution would
otherwise land on one Surface), `id` **titles** it, `videoWidth_` **sizes** it — and `m` is the
monitor mode, so the frame is the monitor's, not the chip's. Get that line right and the SDL
back end's windowing, integer scaling, CRT look, focus policy and close box all arrive with no
further code — and a `NullDisplay` keeps the same Surface per owner for a test to read. Then
paint every pixel (the Surface may be last frame's buffer) and present:

```cpp
paintFrame(m.width, m.height);                   // the shift registers: P7..0, OL1..0, BLANK per pixel
// ...each bus value through the Bt453 -- dac_.lookup(p, ol), black for BLANK -- into s...
g_display->present(this, s);
```

**Indexed8 or Rgb32.** The Dazzler and the VDM-1 paint `Indexed8`: one byte per pixel, an index
the host resolves through `setPalette()`, so a color change is a palette away with no repaint.
That is the right format whenever the board's color logic has **one input of at most eight
bits**. CADzilla's has two: the Bt453 takes the frame memory's P7..0 *and* the overlay's
OL1..0, and an overlay select replaces the palette entry rather than indexing it. Folding that
into one host-side table would teach the host the RAMDAC's rules. So the board keeps the
RAMDAC's input bus itself (`busPixel`: what a test reads to see pixel *values*), runs the chip
(`Bt453::lookup`, Table 3), and paints `Rgb32`: the colors, which the host shows as they are.
The palette RAM and the overlay registers stay separate, as they are in the part.

## 4. Wiring — every file a video board touches

| File | What goes in |
|---|---|
| `CMakeLists.txt` | the `.cpp`s in `altair_core`; the test in `altair_tests` |
| `src/boards/registry.cpp` | `boardTypes()` and `makeBoard()` |
| `src/main.cpp` **and** `tests/main.cpp` | `YourBoard::setDisplay(&g_display)` — miss the second and the board draws into nothing in every test |
| `tests/test.h`, `tests/main.cpp` | the suite's declaration and its table row |
| `machines/<name>.toml` | a sample machine |
| `docs/boards/<name>.md` | from `_TEMPLATE.md`; Limitations and Quirks are the load-bearing sections |
| `docs/manual/boards.md`, `docs/changelog/changelog.md` | the prose list and an `Unreleased` line (add the `## Unreleased` header if it is not there) — **no test guards either** |
| `docs/manual/ref/` | regenerated: `cmake --build build --target docs-reference` |

Nothing in the bus, the monitor, the TOML loader or the MCP server changes. `properties()` is
the entire configuration layer.

## 5. Proving a picture with no window

`test_dazzler.cpp` used to check a picture one pixel at a time — `px(32, 0) == 1` — which is
fine for "is this element red" and useless for "is this the picture", and prints nothing you
can look at when it fails. Two pieces fix that.

**`host/framedump.h`** takes a Surface and its palette out of the seam in the forms a person
and a test can use: `frameText()` renders it as a **text grid**, one character per pixel from a
legend; `framePpm()` / `writePpm()` resolve it through the palette into a **PPM image** any
viewer opens; `frameCrc()` hashes the resolved RGB, so a palette-only change moves it. An
`Rgb32` Surface is already resolved: `frameRgb()` passes its colors through, and `frameText()`
prints black as the legend's first character and any other color as `#`.

**`tests/framecheck.h`** builds the assertion on top. The expected picture is a raw string
literal *in the test*, readable by a person; on a mismatch the failure prints the first row that
differs with a caret under the column **and writes what the board drew as a `.ppm`**, naming the
path. A 640x480 frame is not readable one character per pixel, so CADzilla's end-to-end test
draws its rectangle, line and dot on a 32-pixel grid and samples the frame at the same pitch —
20 x 15 characters that a person can check against the commands that drew them. CADzilla
paints `Rgb32`, so its test checks pixel *values* on the RAMDAC's input instead: `busView()`
copies the board's `busPixel` P7..0 into an `Indexed8` frame on a second `NullDisplay`, and the
same checks run on that:

```cpp
TextGridOpts every32;
every32.xStep = 32;
every32.yStep = 32;
CHECK_FRAME_OPTS(g.busView(), g.cad, R"(
11111111111111111111
1..................1
1..................1
1..................1
1..................1
1..................1
1..................1
1.3333333333333333.1
1..................1
1..................1
1..................1
1.........2........1
1..................1
1..................1
11111111111111111111
)", every32, "sampled every 32nd pixel: the rectangle, the line and the dot, right way up");
```

Every character is a pixel's palette index. A rectangle one row too high, a mirrored X axis, a
picture placed one memory cycle late — each is visible in the diff, and the `.ppm` beside it
shows the full-resolution picture through the colors the guest loaded.

**Be clear about what a sampled grid proves.** It looks at one pixel in a thousand: it proves
the *geometry* in a form a person can read, and nothing about the pixels between the samples.
So pair it with **`CHECK_FRAME_PIXELS`**, which compares **every** pixel against an oracle the
test writes — a function from `(x, y)` to the palette index the commands must have left there.
For a rectangle, a line and a dot that is four comparisons:

```cpp
auto oracle = [](int x, int y) -> uint8_t {
    if (x == 320 && y == 352) return 2;                                  // the dot
    if (y == 224 && x >= 64 && x <= 544) return 3;                       // the line, end excluded
    bool onRect = (y == 0 || y == 448) ? (x <= 608)                      // top and bottom edges
                                        : ((x == 0 || x == 608) && y < 448);   // the sides
    return onRect ? 1 : 0;
};
CHECK_FRAME_PIXELS(g.busView(), g.cad, oracle, "all 307,200 pixels match the oracle");
```

A single wrong pixel anywhere fails it, and the failure names the first differing pixel, how
many differ in all, and the `.ppm`. The grid is for the reader; the oracle is for the proof.
Use both. For a frame you cannot describe in a function, keep a golden file under
`tests/golden/` and `CHECK_FRAME_GOLDEN` it; the golden is rewritten only under
`ALTAIR_TEST_WRITE_GOLDEN=1`, because a golden that rewrites itself asserts nothing.

Then prove the two things a picture alone does not: that a **palette-only change** reaches the
host (`frames()` advanced, `frameCrc()` moved, the pixel bytes unchanged), and that a
**snapshot** repaints the same grid on a fresh board. Both are in `tests/test_cadzilla.cpp`.

## What CADzilla left for next time

Its own `docs/boards/cadzilla.md` says exactly what is not modeled. **Interrupts** are wired —
`IrqJumper` and `irqJumperProperty()`, `assertsInt()`/`assertsVi()` reading the chip's own
`irq()`, and `intChanged()` called from every place that could move, per the lamp chapter's
closing section — so that recipe step is already done here and is the worked example for the
next board that needs it. **Drawing time** is done too, at `draw_rate=real`: the chip counts
only 2CLK and is told the time (`Hd63484::advance()`), and the board owns the conversion from
T-states and the one Clock deadline — the pattern for a chip whose speed depends on a clock the
board supplies. One piece still remains: **the raster counter** (`r80` reads 0). A guest reads it
to sync to the beam, so it must come off the `Clock`, like the Dazzler's vblank bit; the frame's
slot timeline that `Hd63484::buildTimeline()` already builds for drawing time is where its
position would come from.
