# Secure Boot v2 + Flash Encryption — Runbook

> **This procedure burns eFuses and is IRREVERSIBLE on the chip it runs on.**
> A mistake bricks the board (best case) or ships an unpatchable/insecure
> unit (worst case). Read this entire document before flashing a secure
> build, and only ever bring it up on **sacrificial dev boards** until the
> whole flow is proven.

Target: **ESP32-C6**, ESP-IDF v5.x. This is "Part B" of the OTA work; **do not
start it until OTA (Part A) is validated on dev boards**, because once flash
encryption is on, OTA is the only way to update a unit.

---

## What this gives you

| Feature | Protects against |
|---------|------------------|
| **Secure Boot v2** (RSA-3072) | Running unsigned/tampered firmware. Also signs OTA images, so the OTA channel can't be used to push a forged binary. |
| **Flash Encryption** (AES-XTS) | Reading secrets out of a dumped flash chip. |
| **NVS encryption** | The **Firebase refresh token** and **WiFi PSK** in NVS being readable at rest. |

All three are configured in the opt-in `sdkconfig.secure` fragment. The
`nvs_key` partition is already in `partitions.csv` (inert until flash
encryption is on), so enabling security never repartitions / wipes NVS.

---

## One-time: generate the signing key

```sh
. $IDF_PATH/export.sh
./tools/gen_secure_boot_key.sh          # → secure_boot_signing_key.pem
```

- The key is **gitignored**. Move it to **offline storage** (secret manager /
  HSM) and have CI pull it from there.
- **Lose it → you can never update secured units. Leak it → attacker can sign
  malicious firmware.**
- Production hardening: burn a **second** key digest (Secure Boot v2 supports
  up to 3) so a compromised key can be revoked. See the IDF Secure Boot v2 doc
  ("Multiple keys / revocation").

---

## Phase 1 — Development-mode bring-up (sacrificial board)

`sdkconfig.secure` defaults to **development** flash-encryption mode, which
keeps the board reflashable. Build with the fragment layered on the defaults:

```sh
idf.py fullclean
idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.secure" build
idf.py -p <PORT> flash monitor
```

On **first boot** the device burns the flash-encryption key + secure-boot key
digest and encrypts flash in place, then reboots. Watch the log for the
flash-encryption and "Secure Boot V2 enabled" messages.

> **Exact reflash commands differ by mode and IDF version** (e.g.
> `idf.py encrypted-flash`). Follow the official docs rather than guessing —
> a wrong command here is expensive:
> - Flash Encryption: https://docs.espressif.com/projects/esp-idf/en/latest/esp32c6/security/flash-encryption.html
> - Secure Boot v2: https://docs.espressif.com/projects/esp-idf/en/latest/esp32c6/security/secure-boot-v2.html

**Verify before moving on:**
- Device boots and runs normally (relay, BLE, WiFi, Firebase all work).
- Dump flash (`esptool.py read_flash`) and confirm the refresh token / WiFi
  password are **not** readable as plaintext.
- Re-flash a rebuilt image succeeds (dev mode).

### Partition-table offset caveat
A signed Secure Boot v2 bootloader is larger and may overflow the default
`0x8000` partition-table offset on the C6. If the build fails with a
bootloader-size / overlap error:
1. Uncomment `CONFIG_PARTITION_TABLE_OFFSET=0x10000` in `sdkconfig.secure`.
2. Shift the `nvs`, `phy_init`, `otadata` offsets in `partitions.csv` by
   `+0x8000` (i.e. `0x11000`, `0x17000`, `0x18000`). `ota_0` (0x20000),
   `ota_1` (0x1E0000) and `nvs_key` (0x3A0000) are already past 0x10000 and
   stay put.

---

## Phase 2 — Validate OTA under Secure Boot

With a secure dev board from Phase 1:
1. Bump `version.txt`, rebuild (signed), and publish with
   `tools/publish_firmware.sh`.
2. Confirm the device downloads, **signature-verifies**, applies, and reboots
   into the new image (the OTA image is signed automatically by the secure
   build).
3. Confirm rollback still works: publish a deliberately-broken build and
   confirm the device reverts.

If OTA does not work under secure boot, **stop** — do not go to Phase 3.

---

## Phase 3 — Release mode (production units, permanent)

Only after Phases 1–2 pass. In `sdkconfig.secure`:
- Comment `CONFIG_SECURE_FLASH_ENCRYPTION_MODE_DEVELOPMENT=y`
- Set `CONFIG_SECURE_FLASH_ENCRYPTION_MODE_RELEASE=y`

Release mode **permanently disables** plaintext UART download — the unit can
only be updated via signed OTA from then on. Make the eFuse burn a documented
step in manufacturing/provisioning, done per unit, and keep a record of which
key/version each unit shipped with.

---

## Gotchas checklist

- [ ] OTA (Part A) validated **before** enabling any of this.
- [ ] Signing key generated, backed up offline, removed from dev machines.
- [ ] Second key digest planned for revocation.
- [ ] Bench-tested in **development** mode on sacrificial boards first.
- [ ] Flash dump confirmed to hide secrets.
- [ ] OTA re-verified under secure boot (signed images).
- [ ] Partition-offset bump handled if the bootloader overflowed 0x8000.
- [ ] Release mode enabled **only** for production silicon.
- [ ] `nvs_flash_init()` path verified (auto NVS encryption); if you see
      `ESP_ERR_NVS_KEYS_NOT_INITIALIZED`, the `nvs_key` partition wasn't
      flash-encrypted.
