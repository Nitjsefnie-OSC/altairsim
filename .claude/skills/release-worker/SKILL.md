---
name: release-worker
description: Build, test, package and deliver ONE altairsim release archive on THIS machine — for a tag someone else created. Use when asked to build the release package, build vX.Y.Z, run the release build on this box, or do DISTRIBUTION.md §4.2. A build machine never tags, never publishes, never picks a version.
---

# Build one release archive on this machine

You are a **build machine**. You build one archive for this machine's platform, prove it, and
hand it to the coordinator. You **never** tag, push, create or publish a release, run `gh`, or
decide a version number. If you are asked to do any of that, that is the `release-coordinator`
skill, and it runs on the coordinator only.

**The steps are a script: `tools/release-worker.sh`. Run it. Do not write your own from this
page.** A worker script that was written again for each release had new bugs each release
(issue #643). This page gives the command, then the reason for each check, so that you can
read a STOP when the script reports one.

**If the script stops, report it — the report block and the output above it. Do not work
around it, do not decide it is probably fine, and do not deliver.** Nothing checks the package
after you hand it over; the next stop is a user's download.

## What you need to be told

- **The version**, `X.Y.Z`. The tag is `vX.Y.Z`. If you were not given it, STOP and ask. Never
  guess it from `git tag`, `CMakeLists.txt` or the latest release.
- **Site values**, from `distribution.conf` in the repo root if it exists (see
  `distribution.conf.example` for every key), otherwise from whoever asked you. Keys are
  prefixed by this machine's target in capitals (`LINUX_X86_64_PARALLEL`):
  - `<TARGET>_PARALLEL` — `no` means `--no-parallel`. If unknown, build in parallel only on a
    machine with 16 GB or more.
  - `<TARGET>_PATH` — becomes `--path`.
  - `<TARGET>_SERIAL_A` / `_SERIAL_B` — the null-modem pair, if this box has one.
  - `COORDINATOR_USER`, `COORDINATOR_HOST`, `COORDINATOR_DIST` — where step 8 delivers. If you
    do not have them, leave `--deliver` off and report that you did not deliver.

The script finds the target itself:

| this machine | target | archive |
|---|---|---|
| macOS, `uname -m` = `arm64` | `macos-arm64` | `.tar.gz` |
| macOS, `uname -m` = `x86_64` | `macos-x86_64` | `.tar.gz` |
| Windows (Git Bash) | `windows-x86_64` | `.zip` |
| Linux, `x86_64` | `linux-x86_64` | `.tar.gz` |

## Run it

**Run a copy that is outside the checkout.** Step 1 is `git checkout -f vX.Y.Z`, which can
replace the script while the shell reads it. The script refuses to start from inside the
checkout.

macOS and Linux:

```sh
cp tools/release-worker.sh /tmp/altairsim-release-worker.sh
sh /tmp/altairsim-release-worker.sh --version X.Y.Z --repo "$PWD" \
   --deliver <COORDINATOR_USER>@<COORDINATOR_HOST>:<COORDINATOR_DIST>
```

Windows, in **Git Bash** (no Developer shell is needed):

```sh
cp tools/release-worker.sh ~/altairsim-release-worker.sh
sh ~/altairsim-release-worker.sh --version X.Y.Z --repo "$PWD" \
   --deliver <COORDINATOR_USER>@<COORDINATOR_HOST>:<COORDINATOR_DIST>
```

Add what the site values say:

| Argument | When |
|---|---|
| `--no-parallel` | `<TARGET>_PARALLEL=no`. The compiler gets OOM-killed on a machine short of RAM. |
| `--path DIRS` | `cmake` is not found in a non-login ssh shell. Give the directory that the login profile adds. |
| `--serial-a PORT --serial-b PORT` | This box has the null-modem pair. Give both or neither. |
| no `--deliver` | You are the coordinator (the archive is in `dist/` already), or you have no delivery values. |
| `--skip-slow` | A dry run only. A release runs the CPU exercisers. |

The coordinator starts this for you over ssh with `tools/release-drive.sh`; then you do
nothing here.

**The last line of the output is the result:** `release-worker: OK`, or
`release-worker: FAIL at step N`. The report block is above it.

## What each step checks, and why

Every check reads the **full** output of its command. In 1.3.0 a hand-written check read the
last 6 lines of the `ctest` output, the label summary had pushed the pass line out of them, and
two machines built again for nothing.

| Step | Does | CHECK, and the STOP |
|---|---|---|
| 0 | Finds the target and the checkout. | STOP on a machine that no release is built on, and when the script is inside the checkout. |
| 1 | `git fetch --tags --force`, `git checkout -f vX.Y.Z`. | The source is **the tag, never a branch**. `--force` matters: a re-spin force-moves the tag, and a plain fetch keeps the old one and builds the wrong commit. STOP when a tracked file has a change, because `-f` would discard it. |
| 2 | Configures from a **clean** `build/`. | CHECK `-- SDL3 found -- video boards enabled (windowed)`. STOP without it: a headless binary runs and draws nothing, and that is how v0.2.0 shipped. A reused `build/` keeps its cached `SDL3_DIR`, often Homebrew's dylib. |
| 3 | Builds. | STOP on a failed build. |
| 4 | `ctest -LE slow`, without the two hardware tests. | CHECK the line `100% tests passed … out of N`. The absence of the word "error" proves nothing. |
| 4b | `serial-hw` and `tnfs-hw`. | **A warning, never a STOP.** See below. |
| 4c | `ctest -L slow`: the CPU exercisers. | CHECK the pass line. These run here, not in CI on the tag: the four build machines are the four compilers that ship, and the Intel Mac has no CI runner. |
| 5 | `altairsim --version`. | CHECK the one line `AltairSim X.Y.Z`. STOP on a `-N-gsha` suffix (not on the tag) or `(modified)` (dirty tree): the binary cannot be traced to the release. |
| 6 | `tools/build-package.sh --pdf docs/altairsim-manual.pdf`. | The manual is the one CI built; the tag's tree holds it, and pandoc never runs. STOP if it refuses (headless binary, SDL linked by absolute path — it says which). |
| 7 | `tools/verify-package.sh <archive>`. | CHECK `verify-package: PASS -- N/N manual commands reached their documented prompt`. It extracts the archive outside the repo and runs the manual's own commands. |
| 8 | `scp` to the coordinator's `dist/`. | The worker's only credential is the delivery key; it never touches GitHub. |

**The hardware tests (4b).** Each of `serial-hw` and `tnfs-hw` is reported Passed, Skipped or
Failed. If serial ports are configured for this box and `serial-hw` is not Passed, the script
warns loudly (the cable, the ports, or the serial code is wrong) and continues. `tnfs-hw`
runs when `de-tnfsd` is on `PATH` and skips otherwise. With no ports, the script leaves the
two variables **unset**: an empty value makes the test open an empty path and fail, where an
unset one makes it skip.

**On macOS** the script never passes `-DCMAKE_OSX_ARCHITECTURES`: each Mac builds native for
itself.

**On Windows:**

- **The generator is named**, `-G "Visual Studio 18 2026"`. Without it CMake picks the newest
  Visual Studio *it* knows, and an old CMake builds with 2022 silently. CI builds with 2026.
  MSBuild finds the toolchain, so no `vcvars` is needed. Ninja is not used for a release.
  **MSVC is the only supported Windows toolchain.**
- **`MultiThreaded` is load-bearing.** It links the C runtime statically, so the `.exe` does
  not need the VC++ redistributable. SDL3 was built the same way; mixing the two fails to link.
- **`--config Release` is load-bearing** — the generator is multi-config. The binary lands in
  `build\Release\`; `build-package.sh` finds it there.
- **`verify-package` skips the pty checks on Windows** (there is no `expect`). A PASS line
  with those SKIPs is the expected result, not a failure.
- **The `.zip` is made by Windows' own `tar.exe`.** If it ever fails, do not fall back to
  `Compress-Archive`: PowerShell 5.1 writes backslashes that Unix `unzip` cannot extract.

## Report back

The script prints this block, whether it finished or stopped. Pass it on as it is:

```
target        linux-x86_64
version       AltairSim X.Y.Z                (the --version line, verbatim)
tests         100% tests passed out of N     (verbatim)
exercisers    100% tests passed out of N in <time>
serial-hw     Passed | Skipped | Failed      (+ ports used)
tnfs-hw       Passed | Skipped | Failed
SDL3          3.4.12                          (~/opt/sdl3-static/.altairsim-sdl3-version)
verify        verify-package: PASS -- N/N ... (verbatim)
archive       altairsim-X.Y.Z-<target>.tar.gz  sha256 <hash>
delivered     yes | no -- <why>
stopped at    <step and what is wrong>        (only if it stopped)
release-worker: OK | FAIL at step N
```

The SDL3 version matters: nothing makes the four machines agree, so this report is the only
record, and it is the first thing to check if a video bug shows on one platform only.

## First time on this machine

Done once, not at release time. If any is missing, STOP and report which.

- **A C++20 compiler, CMake ≥ 3.20, git.** No pandoc, no Chrome — the PDFs come from CI.
- **A checkout of the public repo over anonymous https**, no login and no token:
  `git clone https://github.com/deltecent/altairsim.git`, or on an existing clone
  `git remote set-url origin https://github.com/deltecent/altairsim.git`.
- **Static SDL3**, which the configure expects at `~/opt/sdl3-static`:
  `tools/build-sdl3-static.sh` (macOS, Linux) or `tools\build-sdl3-static.bat` (Windows).
  Idempotent. On Linux install the X11/Wayland headers first — `docs/building-linux.md` has the
  list, and `libxtst-dev` is required.
- **Windows:** `tools\windows\RUN-ME-setup-windows-worker.bat` installs and checks all of it
  (Visual Studio 2026 Build Tools, Git + Git Bash, OpenSSH Server, static SDL3).
- **Linux:** build on the oldest glibc you can — the binary will not start on a distro older
  than the one it was built on.
- **The delivery key.** A passphrase-less ed25519 key, `~/.ssh/altairsim_deploy`, used for
  nothing else, its public half in the coordinator's `~/.ssh/authorized_keys`, and in
  `~/.ssh/config`:

  ```
  Host <COORDINATOR_HOST>
      IdentityFile ~/.ssh/altairsim_deploy
      IdentitiesOnly yes
  ```

  It must have no passphrase because step 8 runs unattended. It writes to `dist/` and grants
  no GitHub access at all.
