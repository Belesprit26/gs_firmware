#!/usr/bin/env bash
#
# Publish a GeyserSwitch (gs_rework) firmware build for OTA.
#
# Uploads the built app image to the gs_rework Firebase Storage path
# (/firmware_gs/, non-public + download-token) and updates the RTDB
# manifest at /firmware/latest that devices poll (see main/ota.c).
#
# Prerequisites:
#   - `idf.py build` has produced build/gs_firmware.bin
#   - firebase-tools + gsutil installed and authenticated as a principal
#     with Storage write + RTDB write on the project
#
# Usage:  tools/publish_firmware.sh [version]
#   version defaults to the contents of version.txt (the value baked
#   into the image), so normally just run it with no arguments.
#
# NOTE: this uploads to the path devices download from. Publishing a
# version <= what a device already runs is a no-op (ota.c rejects
# non-newer images), so you cannot accidentally force a downgrade.

set -euo pipefail

# ── Config — override via env if your project differs ────────────
PROJECT="${GS_FIREBASE_PROJECT:-geyserswitch-bloc}"
BUCKET="${GS_STORAGE_BUCKET:-geyserswitch-bloc.appspot.com}"
INSTANCE="${GS_RTDB_INSTANCE:-geyserswitch-bloc-default-rtdb}"

# ── Locate the build ─────────────────────────────────────────────
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/build/gs_firmware.bin"
VERSION="${1:-$(tr -d '[:space:]' < "$ROOT/version.txt")}"

[ -f "$BIN" ] || { echo "error: $BIN not found — run 'idf.py build' first" >&2; exit 1; }
[ -n "$VERSION" ] || { echo "error: empty version" >&2; exit 1; }

# gs_rework firmware lives under its OWN Storage path, /firmware_gs/, which
# storage.rules keeps non-public (read: if false). Devices download it via a
# Firebase Storage *download token* baked into the manifest URL below; the
# token bypasses the rule for legitimate devices while an unauthenticated
# party (who can't read the auth-gated RTDB manifest) can't fetch the binary.
# The legacy Orange fleet's public /firmware/ path is untouched.
OBJECT="firmware_gs/gs_rework-${VERSION}.bin"
ENCODED="$(python3 -c "import urllib.parse,sys; print(urllib.parse.quote(sys.argv[1], safe=''))" "$OBJECT")"
TOKEN="$(python3 -c "import uuid; print(uuid.uuid4())")"
DOWNLOAD_URL="https://firebasestorage.googleapis.com/v0/b/${BUCKET}/o/${ENCODED}?alt=media&token=${TOKEN}"
SHA256="$(shasum -a 256 "$BIN" | awk '{print $1}')"

echo "Publishing gs_rework ${VERSION}"
echo "  bin:    ${BIN}"
echo "  sha256: ${SHA256}"
echo "  object: gs://${BUCKET}/${OBJECT}"

# ── Upload the binary ────────────────────────────────────────────
gsutil -h "Content-Type:application/octet-stream" \
       -h "x-goog-meta-firebaseStorageDownloadTokens:${TOKEN}" \
       cp "$BIN" "gs://${BUCKET}/${OBJECT}"

# ── Update the manifest (last, so devices never see a URL 404) ───
MANIFEST="$(cat <<JSON
{
  "version": "${VERSION}",
  "url": "${DOWNLOAD_URL}",
  "sha256": "${SHA256}",
  "mandatory": false
}
JSON
)"

printf '%s' "$MANIFEST" | firebase database:set /firmware/latest \
  --project "$PROJECT" --instance "$INSTANCE" --confirm -

echo "Done — devices will pick up ${VERSION} on their next check (~within the check interval)."
