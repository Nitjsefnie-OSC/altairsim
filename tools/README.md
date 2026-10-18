# tools/

This directory holds the scripts and small programs that build the documents, the release
package and the media, and that CI uses. Nothing here is part of the simulator. Each file
starts with a long comment that gives its usage and the reason that it exists.

## For the user

| File | What it does |
|---|---|
| `user/` | The tools that a person with the release package uses. This is the only part of `tools/` that ships: it is `tools/` in the package. It has the two `mirror-watch` scripts, which show a `--mirror` in a second window, and the three LBR tools (`lbr-extract.py`, `uncrunch.py`, `unsqueeze.py`), which open CP/M library files and decode crunched and squeezed files. |

## Release

| File | What it does |
|---|---|
| `release-worker.sh` | Builds, tests, packages and delivers the archive of one release on one build machine, with a check after each step. Runs on all four targets (on Windows, in Git Bash). |
| `release-drive.sh` | On the coordinator: starts `release-worker.sh` on a build machine from `distribution.conf`, proves that it started, and reads its result. |
| `build-package.sh` | Assembles the release archive for this platform from [`docs/package.map`](../docs/package.map). |
| `verify-package.sh` | Unpacks a finished archive outside the repository and runs the commands that the *User Manual* gives. |
| `build-checksums.sh` | Writes `SHA256SUMS` when all the archives of one version are present. |
| `build-sdl3-static.sh`, `build-sdl3-static.bat` | Build a static SDL3 for packaging. Run one of them once on each build machine. |
| `windows/` | The setup of a Windows build machine. Start with `RUN-ME-setup-windows-worker.bat`. |

Do a release with the release skills in `.claude/skills/`. [`DISTRIBUTION.md`](../DISTRIBUTION.md)
gives the reasons.

## Documents

| File | What it does |
|---|---|
| `build-docs.sh` | Builds the PDFs from [`docs/`](../docs/README.md). It needs pandoc and Chrome, so it is separate from the program build. |
| `chrome-print.py` | Prints an HTML file to PDF after Paged.js completes its layout. `build-docs.sh` calls it. |
| `otf2ttf.py` | Converts an OpenType font to TrueType. It made the body fonts in `docs/fonts/`. |
| `gen-reference.cpp` | Builds to `altair_genref`, which writes the reference chapters in `docs/manual/ref/` and `docs/monitor/ref/` from the board properties and the command table. |

CI builds the PDFs and commits them. If you run `build-docs.sh`, restore the PDFs with
`git checkout --` before you commit.

## CI

| File | What it does |
|---|---|
| `ci-changed-code.sh` | Tells CI if a change is documentation, code or core. A PR runs all three platforms only for a core change. |
| `ci-merge-already-tested.sh` | Tells CI if the tree of a merge was already tested. A documentation merge is then skipped; a core merge builds without the tests. |
| `ci-apt-install.sh` | Installs packages on a Linux runner with a timeout. |
| `fetch-ci-binaries.sh` | Waits for a CI run, then downloads the binaries it built into `artifacts/`. |

## Media

| File | What it does |
|---|---|
| `fetch-disk-images.sh` | Downloads the disk images that are too large to track, and checks them. |
| `install-hostbridge-utils.sh` | Puts `R.COM`, `W.COM` and `HDIR.COM` on a CP/M disk image. |
| `make-drawdemo-disk.sh` | Builds the CP/M disk that the CADzilla test boots. |
| `tapetool.cpp` | Builds to `altair_tapetool`, which reads and writes cassette audio with no simulator attached. |

## Build helper

| File | What it does |
|---|---|
| `embed.cpp` | Writes the generated source files for the plain `Makefile` build. Its output must match the scripts in [`cmake/`](../cmake/README.md). |
