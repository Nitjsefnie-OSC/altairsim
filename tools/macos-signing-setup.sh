#!/bin/sh
#
# SET UP A MAC TO SIGN AND NOTARIZE RELEASE PACKAGES. Run it once per Mac, on that Mac (over
# ssh is fine). It makes the keychain that tools/macos-sign.sh uses -- DISTRIBUTION.md 4.1 has
# the reasoning -- and ends by proving that the keychain holds the identity and that Apple
# answers with the stored notary credentials.
#
#   usage: tools/macos-signing-setup.sh --p12 <file> --p8 <file> --key-id <id> --issuer <uuid>
#
#     --p12      The Developer ID Application certificate and its private key, exported as .p12
#                (Keychain Access: right-click the identity, Export). Its password is read from
#                MACOS_P12_PASSWORD, or asked for when there is a terminal.
#     --p8       The App Store Connect API key (AuthKey_XXXXXXXXXX.p8).
#     --key-id   That key's ID.
#     --issuer   Its issuer ID.
#
# It writes:
#   ~/Library/Keychains/altairsim-signing.keychain-db   the keychain
#   ~/.altairsim-signing-pass                            its password, mode 600
# and stores the notary profile `altairsim-notary` in that keychain. Put those three names in
# distribution.conf (<TARGET>_KEYCHAIN, _KEYCHAIN_PASS_FILE, _NOTARY_PROFILE) with the identity's
# SHA-1, which it prints. The SHA-1, not the name: the name has spaces, and sh sources that file.
#
# Run it again to replace both: it deletes the old keychain first.

set -eu

p12=""; p8=""; keyid=""; issuer=""
need() { [ $# -ge 2 ] || { echo "macos-signing-setup: $1 needs a value" >&2; exit 1; }; }
while [ $# -gt 0 ]; do
  case $1 in
    --p12)    need "$@"; p12=$2; shift 2 ;;
    --p8)     need "$@"; p8=$2; shift 2 ;;
    --key-id) need "$@"; keyid=$2; shift 2 ;;
    --issuer) need "$@"; issuer=$2; shift 2 ;;
    -h|--help) sed -n '/^#   usage:/,/^# Run it again/p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "macos-signing-setup: unknown argument $1" >&2; exit 1 ;;
  esac
done
[ -n "$p12" ] && [ -n "$p8" ] && [ -n "$keyid" ] && [ -n "$issuer" ] || {
  echo "macos-signing-setup: --p12, --p8, --key-id and --issuer are all required (--help)" >&2; exit 1; }
[ -f "$p12" ] || { echo "macos-signing-setup: $p12 does not exist" >&2; exit 1; }
[ -f "$p8" ]  || { echo "macos-signing-setup: $p8 does not exist" >&2; exit 1; }

p12pass=${MACOS_P12_PASSWORD:-}
if [ -z "$p12pass" ]; then
  [ -t 0 ] || { echo "macos-signing-setup: set MACOS_P12_PASSWORD (no terminal to ask on)" >&2; exit 1; }
  printf 'Password of %s: ' "$p12"; stty -echo; read -r p12pass; stty echo; echo
fi

keychain=$HOME/Library/Keychains/altairsim-signing.keychain-db
passfile=$HOME/.altairsim-signing-pass
profile=altairsim-notary

security delete-keychain "$keychain" 2>/dev/null || true
( umask 077; uuidgen > "$passfile" )
kcpass=$(cat "$passfile")

security create-keychain -p "$kcpass" "$keychain"
# No auto-lock timeout: macos-sign.sh unlocks it before every use anyway.
security set-keychain-settings "$keychain"
security unlock-keychain -p "$kcpass" "$keychain"
security import "$p12" -k "$keychain" -P "$p12pass" -T /usr/bin/codesign
# Without this, the first codesign over ssh waits for an "allow access" dialog nobody sees.
security set-key-partition-list -S apple-tool:,apple:,codesign: -s -k "$kcpass" "$keychain" > /dev/null
# codesign finds the identity only in a keychain on the search list. Add ours, keep the rest.
# shellcheck disable=SC2046
security list-keychains -d user -s $(security list-keychains -d user | tr -d '"' | grep -v altairsim-signing) "$keychain"

xcrun notarytool store-credentials "$profile" --key "$p8" --key-id "$keyid" --issuer "$issuer" \
  --keychain "$keychain" > /dev/null

echo
security find-identity -v -p codesigning "$keychain"
identity=$(security find-identity -v -p codesigning "$keychain" |
  sed -n 's/.*\([0-9A-F]\{40\}\) "Developer ID Application:.*/\1/p' | head -n 1)
[ -n "$identity" ] || { echo "macos-signing-setup: FAIL -- $p12 has no Developer ID Application identity" >&2; exit 1; }
xcrun notarytool history --keychain-profile "$profile" --keychain "$keychain" > /dev/null \
  || { echo "macos-signing-setup: FAIL -- Apple did not accept the notary credentials" >&2; exit 1; }

cat <<EOF

macos-signing-setup: OK. For distribution.conf, on this Mac's target:
  <TARGET>_SIGN_IDENTITY=$identity
  <TARGET>_NOTARY_PROFILE=$profile
  <TARGET>_KEYCHAIN=$keychain
  <TARGET>_KEYCHAIN_PASS_FILE=$passfile
EOF
