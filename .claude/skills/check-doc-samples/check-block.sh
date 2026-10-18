#!/usr/bin/env bash
#
# Replay one monitor transcript from a doc and diff it against the binary.
#
#   check-block.sh --list <doc>
#       Print the line of every fenced block that holds an `altairsim>` prompt.
#
#   check-block.sh <doc> <fence-line> <machine> [setup-command ...]
#       Take the fenced block that opens at <fence-line>, run its `altairsim>` commands on
#       <machine> with --exec, and diff the output. Setup commands run first; their output is
#       not compared. Prints "OK <doc>:<line>" on a match, else the diff. Exit 0 on a match.
#
#   DIR=<folder> check-block.sh ...
#       Replay as if from <folder>, for a block that names a file by a relative path
#       (`SYMBOLS LOAD ALTMON.PRN` needs DIR=roms/ALTMON).
#
# THE REPLAY RUNS IN A NEW TEMPORARY FOLDER, never in the tree (issue #815). It holds a copy of
# each file of <folder> (default: the repository root) and a link to each of its sub-folders,
# so a relative read finds its file and a file the block writes (`SAVE`, `--log`) lands in the
# temporary folder. A write into a sub-folder would go through the link into the tree, so the
# script compares `git status` before and after, and fails with exit 3 if the tree changed.
#
# Paths are relative to the repository root, and the binary is its build/altairsim. Stdin is
# always /dev/null: a pipe or a tty on stdin becomes the guest's keyboard, and a `startup`
# RUN reads the commands as keys. macOS has no `timeout`, so perl's alarm stops a RUN that
# never returns.

set -u
root=$(cd "$(dirname "$0")/../../.." && pwd)
bin=$root/build/altairsim
cd "$root" || exit 2

if [ "${1:-}" = --list ]; then
    awk '/^```/ { if (c) { if (b ~ /altairsim> /) print FILENAME ":" s; c = 0 }
                  else    { c = 1; s = NR; b = "" }
                  next }
         c { b = b $0 "\n" }' "$2"
    exit 0
fi

if [ $# -lt 3 ]; then
    echo "usage: $0 --list <doc> | <doc> <fence-line> <machine> [setup-command ...]" >&2
    exit 2
fi
doc=$1 line=$2 machine=$3
shift 3

# The block, with the doc's notes removed from its prompt lines. A note is what follows two
# or more spaces: `altairsim> REGS            (on a Z80 machine)`. A command that needs two
# spaces in a row cannot be checked by this script.
want=$(awk -v s="$line" 'NR > s && /^```/ { exit } NR > s' "$doc" |
       perl -pe 's/^(altairsim> \S.*?\S)  .*$/$1/; s/ +$//' | grep -v '^$')
first=$(printf '%s\n' "$want" | grep -m1 '^altairsim> ')
if [ -z "$first" ]; then
    echo "$doc:$line: no altairsim> prompt in this block" >&2
    exit 2
fi

args=()
for c in "$@"; do args+=(-x "$c"); done
while IFS= read -r c; do args+=(-x "$c"); done < <(printf '%s\n' "$want" | sed -n 's/^altairsim> //p')

# A machine file named by a path from the root must still be found from the temporary folder.
[ -f "$machine" ] && machine=$root/$machine

src=$root/${DIR:-.}
[ -d "$src" ] || { echo "DIR=$DIR is not a folder" >&2; exit 2; }
tmp=$(mktemp -d "${TMPDIR:-/tmp}/check-block.XXXXXX") || exit 2
trap 'rm -rf "$tmp"' EXIT
for f in "$src"/*; do
    if [ -d "$f" ]; then ln -s "$f" "$tmp/"; elif [ -f "$f" ]; then cp "$f" "$tmp/"; fi
done
before=$(git status --porcelain --untracked-files=all)

got=$(cd "$tmp" && perl -e 'alarm 20; exec @ARGV' "$bin" "$machine" "${args[@]}" </dev/null 2>&1 |
      awk -v f="$first" '$0 == f { p = 1 } p' | sed 's/ *$//' | grep -v -e '^\[console' -e '^$')

if [ "$(git status --porcelain --untracked-files=all)" != "$before" ]; then
    echo "$doc:$line: the replay changed the tree. git status now shows:" >&2
    git status --short --untracked-files=all >&2
    exit 3
fi

# A block that ends in "..." shows only the start of the output. Compare that start.
if [ "$(printf '%s\n' "$want" | tail -1)" = "..." ]; then
    n=$(printf '%s\n' "$want" | wc -l)
    want=$(printf '%s\n' "$want" | sed '$d')
    got=$(printf '%s\n' "$got" | head -n $((n - 1)))
fi

if diff <(printf '%s\n' "$want") <(printf '%s\n' "$got"); then
    echo "OK $doc:$line"
else
    exit 1
fi
