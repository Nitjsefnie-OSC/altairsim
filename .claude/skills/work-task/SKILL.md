---
name: work-task
description: How any change to altairsim is done, from plan to review — plan in plan mode (for a feature, first prove it is needed — not already documented, not a docs gap), branch, build, fix related bugs and file unrelated ones as issues, update the docs, test, then STOP for the maintainer's review. Use when starting or doing any task — fix a bug, fix issue #N, add a feature, change docs, refactor. Ends before any commit; committing is the ship-change skill.
---

# Do a task: plan, build, stop for review

This skill ends at a review. **It never commits and never pushes.** After the maintainer
approves the change, the `ship-change` skill takes over.

## 1. Plan, in plan mode

If you are not in plan mode, call `EnterPlanMode` first. **No edits before the plan is
approved** — not a "quick" one, not a probe left in the tree.

Research economy: read the memory index and run one targeted `grep` before you reach for an
agent. Read the relevant `DESIGN.md` section before you design anything.

**For a feature, do 1a first, inside plan mode** — it decides whether there is anything to
design. The plan names:

- the files to change
- the tests to add or run
- the docs to update
- how you will prove it works
- the `/compact` points, when the work has more than one task

### `/compact` points

Divide a plan with a lot of work into small tasks. Each task has its own check, such as a test
that passes or a guest that boots. Put a **⏸ COMPACT** mark after each task. At that mark, the
maintainer can run `/compact` or `/clear` before the next task starts.

The plan gives two facts for each mark:

1. **What is done.** The check that shows the task is complete.
2. **Where the state is.** Write every fact the next task needs to a file before the mark: the
   plan file, a progress file in the working folder, the tree, or a memory note. Do not keep
   such a fact only in the conversation, because `/compact` can lose it.

At each mark, stop. Report that the task is done, where its state is, and which task is next.
The maintainer runs `/compact` or tells you to continue.

Ask with `AskUserQuestion` only when different answers would lead to different work. Then
`ExitPlanMode`.

## 1a. A feature must earn its place

**When the task starts from an incoming issue, `review-issue` settles the verdict first** —
this step then builds on it rather than repeating it.

For a feature — anything that is not fixing a bug — settle this **before** designing anything:

1. **The need, in the requester's terms.** What were they trying to do, what did they try,
   and where did it fall short? A request that names a solution ("add a `--foo` flag") has a
   problem behind it; find the problem.
2. **Whether altairsim can already do it.** Search the manual, `HELP`, `docs/manual/ref/`,
   the machine files, and the existing commands and properties. If it can, there is no code
   to write, and one of two things is true:
   - **It is documented, and the requester did not find it.** Nothing changes: point them to
     the section or `HELP` entry, on the issue if there is one.
   - **It is not documented, or is too hard to find.** That is a real documentation gap; the
     fix is a docs change (a manual section, a `HELP` line, an example).
3. **Whether it was declined before.** Search closed issues and `DESIGN.md` for an earlier
   decision and its reason.
4. **Whether it fits.** Read the relevant `DESIGN.md` section. A feature that gives hardware a
   behavior it never had is a no.

**The plan opens with the verdict** — needed, already documented, a documentation gap, or
already declined — and the evidence for it. If the need is unclear, ask before planning. A
documentation gap makes the plan a docs change; already documented means no change at all.

## 2. Branch off an up-to-date master

```sh
git switch master && git pull --ff-only && git switch -c <type>/<slug>
```

`<type>` is `fix`, `feat`, `docs`, `test` or `refactor`, as in the commit messages.

## 3. Build it

### Tests prove the code; they never bend around it

When a test exposes a bug, the fix goes in the code. None of these is allowed:

- loosening an assertion, or widening a tolerance until it passes
- skipping, disabling or deleting the test
- changing the test's input so it no longer reaches the bug
- adding a retry or a sleep to hide a race
- recording known-bad output as the expected result

Prove a new test catches what it claims: break the fix, see the test fail, restore the fix.

### Bugs you find on the way

- **Related** — the task touches it, or it stands in the task's way: fix it in this change
  and name it in the review.
- **Unrelated** — file it now and carry on with the task. Do not fix it here.

  ```sh
  gh issue create --title "<symptom, plainly>" --body-file <file>
  ```

  The body gives the symptom, the repro, `file:line` where known, and where it was found
  (the branch). Never quote the maintainer. End it with `--AltairSim Claude`.

**A hazard is never documented as a warning.** If you are about to write "be careful to…",
fix the code instead, or file the issue.

## 4. Docs, before the review

The docs are part of the change under review, not a follow-up. Update whichever apply:

- `DESIGN.md`, when the design or its reasoning moved
- `docs/devguide/`, for how the code is built
- the manual in `docs/manual/` — and `docs/manual/ORDER` for a new chapter
- `docs/manual/ref/` is generated: edit the emitter and run
  `cmake --build build --target docs-reference`
- `docs/changelog/changelog.md` `## Unreleased` — **whenever what a package holder gets
  changes**. Not only code: a new document in the archive, an example that now ships, a chapter
  that answers something it did not, all count. The trap is filing a change as "only
  documentation" when the document IS the thing that shipped. A fix for a bug nobody reported
  still gets no entry. **An entry for a fix says what the simulator did wrong** — the register,
  bit or command, and why the guest failed — then what it does now, with the guest as the
  example. "X now works" is not an entry. Check it against the commit message. CI enforces this
  (the `Changelog entry` job in `changelog.yml`), so a missing entry reds the PR rather than reaching
  a release unannounced. **A release removes the
  `## Unreleased` header.** If it is not there, add it: below the `---` rule, above the newest
  `## X.Y.Z`.

Never commit a locally built PDF; CI builds them.

**Load `simplified-english` before you write the first sentence.** It is the standard for
every text that a person reads: the manual, a README, the changelog, HELP text, and a paragraph
in `DESIGN.md` or the Developer Guide. Write to its rules while you draft (its §6). Do not
write first and fix later. Before the review report, run its §6 checks on the text that you
added or changed, and only on that text. A sample's output is copied from a real run, or it is
left out.

**Scope the doc checks to the change.** Edit the docs your diff affects, and replay samples
(`check-doc-samples`) only for a doc whose transcripts the diff could change. Do not run a
sense pass, a whole-file `simplified-english` review or an audit of every TOML example unless
the task is itself a doc rewrite. The whole-tree sweep is a release step
(`release-coordinator`, step 1a).

## 5. Test

The cadence `CLAUDE.md` sets: the unit suites for what you touched
(`./build/altair_tests <names>`), and `cmake -B build -DWERROR=on && cmake --build build -j`
for a code change. Run the full `ctest --test-dir build -LE slow` for a wide change. **Quote
the pass line** (`100% tests passed out of N`); never paraphrase it.

## 6. STOP for review

Report, then wait:

- `git status --short` and `git diff --stat`
- what changed and why, file by file
- the tests run, with their pass lines
- the docs touched, and that the `simplified-english` §6 checks were run on the new text
- the issues filed, by number
- anything not verified, and why
- **last**, one line saying what is ready and where: *"Ready for your review on branch
  `<branch>`. Nothing is committed or pushed."* The maintainer reads to the bottom, so the
  line that says the next move is theirs goes there, not above the detail.

**Do not `git commit`. Do not push.** Changes the maintainer asks for go back to step 3, and
come back here for review again.
