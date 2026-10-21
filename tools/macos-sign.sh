#!/bin/sh
#
# SIGN, AND NOTARIZE, A STAGED macOS PACKAGE. tools/build-package.sh calls it on a macos-*
# target, after the package directory is complete and before it is archived.
#
# WHY. macOS marks a file that a browser downloads, and it will not start a marked program that
# Apple has not notarized. Until this script, the manual told every Mac user to clear the mark
# with `xattr` by hand. A program signed with a Developer ID certificate and accepted by Apple's
# notary service starts with no dialog.
#
# A PLAIN EXECUTABLE CANNOT BE STAPLED. Only an app bundle, a .dmg or a .pkg can carry the
# notary ticket inside it. A bare `altairsim` in a tar.gz cannot, so Gatekeeper fetches the
# ticket from Apple the first time the program starts. That first start needs the internet;
# the manual's Troubleshooting chapter says what to do without it.
#
# WITH NO --sign, the program gets an AD HOC signature (`codesign -s -`). That is what the
# linker gives an arm64 binary anyway, and what the CI package leg uses: it has no certificate.
# A Mac user still has to clear the mark on such a package. tools/release-worker.sh refuses to
# ship one; see DISTRIBUTION.md 4.3.
#
# THE KEYCHAIN. A release build runs over ssh on the Intel Mac, where the login keychain can be
# locked and `codesign` can wait for an "allow access" dialog that nobody sees. So each Mac
# keeps the Developer ID identity and the notary credentials in a keychain of their own, which
# this script unlocks from a password file. tools/macos-signing-setup.sh makes it.
#
#   usage: tools/macos-sign.sh [--sign <identity> --notary-profile <name>
#                               --keychain <path> --keychain-pass-file <file>] <package dir>
#
#     --sign                The Developer ID Application identity: its name or its SHA-1.
#     --notary-profile      The `notarytool store-credentials` profile, in that keychain.
#     --keychain            The signing keychain (altairsim-signing.keychain-db).
#     --keychain-pass-file  A file holding that keychain's password, readable only by you.
#
# All four, or none. It prints `macos-sign: SIGNED ...` and exits 0, or names what failed.

set -eu

identity=""
profile=""
keychain=""
passfile=""
dir=""

need() { [ $# -ge 2 ] || { echo "macos-sign: $1 needs a value" >&2; exit 1; }; }

while [ $# -gt 0 ]; do
  case $1 in
    --sign)               need "$@"; identity=$2; shift 2 ;;
    --notary-profile)     need "$@"; profile=$2; shift 2 ;;
    --keychain)           need "$@"; keychain=$2; shift 2 ;;
    --keychain-pass-file) need "$@"; passfile=$2; shift 2 ;;
    -h|--help) sed -n '/^#   usage:/,/^# All four/p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    -*) echo "macos-sign: unknown option $1" >&2; exit 1 ;;
    *)  [ -z "$dir" ] || { echo "macos-sign: only one package dir, got '$dir' and '$1'" >&2; exit 1; }
        dir=$1; shift ;;
  esac
done

[ -n "$dir" ] && [ -d "$dir" ] || { echo "macos-sign: no package directory given, or it does not exist: '$dir'" >&2; exit 1; }
[ "$(uname -s)" = Darwin ] || { echo "macos-sign: this runs on macOS only" >&2; exit 1; }

set_count=0
for v in "$identity" "$profile" "$keychain" "$passfile"; do [ -z "$v" ] || set_count=$((set_count + 1)); done
case $set_count in
  0|4) ;;
  *) echo "macos-sign: give all of --sign, --notary-profile, --keychain and --keychain-pass-file, or none" >&2
     exit 1 ;;
esac

# EVERY Mach-O file in the package, not only altairsim. Notarization rejects the whole
# submission for one unsigned executable, so a second binary added to the package one day is
# signed here without anyone remembering this script.
machos=$(find "$dir" -type f -exec sh -c 'file -b "$1" | grep -q "^Mach-O" && printf "%s\n" "$1"' _ {} \;)
[ -n "$machos" ] || { echo "macos-sign: no Mach-O file in $dir" >&2; exit 1; }

if [ "$set_count" -eq 0 ]; then
  echo "$machos" | while IFS= read -r f; do codesign --force --sign - "$f"; done
  echo "$machos" | while IFS= read -r f; do codesign --verify --strict "$f"; done
  echo "macos-sign: SIGNED ad hoc -- not notarized. A Mac user must clear the quarantine mark."
  exit 0
fi

[ -f "$keychain" ] || { echo "macos-sign: keychain $keychain does not exist. Run tools/macos-signing-setup.sh" >&2; exit 1; }
[ -r "$passfile" ] || { echo "macos-sign: cannot read $passfile" >&2; exit 1; }
kcpass=$(cat "$passfile")
security unlock-keychain -p "$kcpass" "$keychain" \
  || { echo "macos-sign: could not unlock $keychain with the password in $passfile" >&2; exit 1; }

# Hardened runtime (--options runtime) and a secure timestamp are what notarization requires.
echo "$machos" | while IFS= read -r f; do
  codesign --force --options runtime --timestamp --keychain "$keychain" --sign "$identity" "$f"
  codesign --verify --strict --verbose=2 "$f"
done

first=$(echo "$machos" | head -n 1)
authority=$(codesign --display --verbose=2 "$first" 2>&1 | sed -n 's/^Authority=//p' | head -n 1)
case $authority in
  "Developer ID Application:"*) ;;
  *) echo "macos-sign: $first is signed by '$authority', not a Developer ID Application identity" >&2
     exit 1 ;;
esac

# Notarize the whole package directory, zipped. ditto -c -k --keepParent is the form Apple
# documents; the zip is only for the upload and is deleted after.
work=$(mktemp -d "${TMPDIR:-/tmp}/macos-sign.XXXXXX")
trap 'rm -rf "$work"' EXIT
ditto -c -k --keepParent "$dir" "$work/notarize.zip"
echo "macos-sign: submitting to Apple's notary service (this waits; usually a few minutes)"
xcrun notarytool submit "$work/notarize.zip" \
    --keychain-profile "$profile" --keychain "$keychain" \
    --wait --timeout 30m --output-format json > "$work/notary.json" || true
cat "$work/notary.json"; echo
status=$(plutil -extract status raw "$work/notary.json" 2>/dev/null || echo "no answer")
if [ "$status" != Accepted ]; then
  id=$(plutil -extract id raw "$work/notary.json" 2>/dev/null || true)
  [ -z "$id" ] || xcrun notarytool log "$id" --keychain-profile "$profile" --keychain "$keychain" || true
  echo "macos-sign: notarization finished with status '$status'" >&2
  exit 1
fi

echo "macos-sign: SIGNED $authority -- notarized ($status)"
