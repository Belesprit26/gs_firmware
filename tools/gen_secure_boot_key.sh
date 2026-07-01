#!/usr/bin/env bash
#
# Generate the Secure Boot v2 signing key (RSA-3072).
#
# This key signs the bootloader and every firmware image (including OTA
# updates). Its public-key digest is burned into eFuse on the first boot
# of a secure build; after that, only images signed with this key will
# boot. Treat it like a production secret:
#
#   • Store it OFFLINE — a secret manager / HSM, not a laptop.
#   • Lose it  → you can never ship another update to secured units.
#   • Leak it  → an attacker can sign malicious firmware.
#
# For production, also plan to burn a SECOND key digest (Secure Boot v2
# allows up to 3) so a compromised key can be revoked — see SECURE_BOOT.md.
#
# Requires the ESP-IDF environment (`. $IDF_PATH/export.sh`).

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
KEY="$ROOT/secure_boot_signing_key.pem"

if [ -f "$KEY" ]; then
  echo "error: $KEY already exists — refusing to overwrite it." >&2
  echo "       (Overwriting would orphan every device already signed with the old key.)" >&2
  exit 1
fi

command -v espsecure.py >/dev/null 2>&1 || {
  echo "error: espsecure.py not found — source the IDF env first: . \$IDF_PATH/export.sh" >&2
  exit 1
}

espsecure.py generate_signing_key --version 2 --scheme rsa3072 "$KEY"

echo
echo "Generated: $KEY"
echo "→ It is gitignored. Move it to secure offline storage and delete the local copy"
echo "  once your build/CI pipeline can retrieve it from there."
