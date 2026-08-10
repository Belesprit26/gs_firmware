# RGB status LED — behaviour spec

Status: **spec (not yet implemented).** There is no LED code in the firmware
today; this defines the behaviour to build. Pairs with the HARDWARE_ROADMAP
item "RGB LED shows correct status colours".

---

## Hardware (from the board schematic)

**D9 — a WS2812B addressable RGB LED** (Worldsemi WS2812B-V5, LCSC C2874885),
one pixel, data line **`LED_1` on GPIO1** (XIAO pin A1/D1), with a 0.1 µF
bypass cap (C9) across its supply. Driven by the ESP-IDF `led_strip` component
over a **dedicated** RMT channel — the DS18B20 already uses RMT for OneWire
(`temperature.c`), so allocate a *separate* channel. One data wire, one GPIO.
GPIO1 is a normal, non-strapping pin — safe: at boot, before `led_init`, the
line is idle and the pixel stays dark.

Keep default brightness **low (~20–30 %)** — it's a utility indicator, often in
a cupboard or ceiling, and a bright pixel at night is a nuisance.

> ⚠️ **Power rail (current vs next board rev):** the **current** board powers D9
> from **+3.3 V**, a hair under the WS2812B's typical 3.5 V VDD minimum — on
> this rev expect possibly dim or colour-shifted output (green/blue lose
> headroom first). That's a hardware margin, **not a firmware bug**. The **next
> board rev moves D9 to +5 V**, which fixes brightness/colour but shifts the
> concern to the **data line**: a WS2812B at 5 V needs V_IH ≈ 3.5 V, above the
> ESP32-C6's 3.3 V GPIO swing. Simplest fix on the 5 V rev — one series diode on
> D9's VDD (~0.65 V drop → VDD ≈ 4.3 V → V_IH ≈ 3.0 V < 3.3 V) — or a
> single-gate level shifter, or a 3.3 V-native pixel (SK6812 / WS2812C). The
> firmware (`led_strip` over RMT on GPIO1) is identical in every case.

## Prerequisites (small firmware additions)

The LED task polls existing state, but two **live** signals are not exposed
yet — add tiny getters:

- `wifi_prov_is_link_up()` → returns `s_wifi_connected` (`wifi_prov.c`) — the
  live WiFi link, distinct from "has stored creds" (`wifi_prov_has_wifi()`).
- `firebase_rtdb_is_online()` → the SSE `connected` flag (`firebase_rtdb.c`) —
  true cloud reachability. (Fallback if you'd rather not expose the stream
  flag: `firebase_auth_is_ready()`, which only means a valid token exists.)

Everything else is already readable: `wifi_prov_is_provisioned()`,
`ble_get_conn_handle()`, `owner_auth_is_unlocked()`, `device_state_get_relay()`.

Drive all button animations from the button's own constants so the LED always
tracks them: `SHORT_MAX_MS` (5 s toggle window) and `RESET_HOLD_MS` (10 s
wipe), both in `button.c`.

## Priority (highest wins)

1. Factory-wipe warning / commit (button held long)
2. Button-press feedback (blackout + toggle confirm)
3. OTA in progress *(optional overlay)*
4. Provisioning / connecting feedback (install time)
5. Resting state (connectivity colour + relay motion)

---

## Resting state — colour = connectivity, motion = relay

One pixel answers both "where am I" (hue) and "is the geyser on" (breathe vs
steady) at a glance.

| Connectivity | Colour |
|---|---|
| Unprovisioned / setup mode | white |
| Provisioned, BLE client connected (no cloud) | blue |
| Provisioned, cloud/WiFi online | green |
| Provisioned, fully offline (no BLE, no cloud) | amber |

| Relay | Motion applied to the colour above |
|---|---|
| ON (heating allowed) | slow **breathe** (~2.5 s sine fade) |
| OFF (idle) | **steady** dim glow |

Setup mode is the exception: while unprovisioned, show white **breathing**
regardless of relay — the priority is "set me up", and the breathe reads as
"alive, waiting".

## Provisioning / connecting (install-time feedback)

Driven by `prov_status_t` (`wifi_prov`):

- Setup / waiting → white breathe.
- WiFi connecting → green **fast blink** (~2 Hz) — progress.
- WiFi OK / cloud coming up → blink slows, settles to resting green.
- WiFi FAIL / error (wrong password, etc.) → **red blink** (~2 Hz) for a few
  seconds, then back to setup white — the installer sees the failure.
- BLE-only provisioned OK → settle to resting blue.

## Button timeline (real 5 s / 10 s thresholds)

The instant the button goes **down**, the LED **blacks out** — an immediate
"input received" cue and a blank canvas for what follows.

- **Release ≤ 5 s (short press → relay toggle):** ~1 s confirmation, then
  return to resting:
  - turned ON  → white → **green** → white
  - turned OFF → white → **red** → white
- **Still held, 5–10 s (past the toggle window — today a dead zone):** a **red
  strobe** (fast red/off blink) — "keep holding and I will factory-reset."
  Releasing here does nothing (matches firmware) and returns to resting.
- **8–10 s (final ~2 s before the wipe):** escalate to a **solid bright red** —
  "release NOW or I wipe."
- **≥ 10 s (wipe commits):** three rapid red flashes, then the device erases
  NVS and reboots — it comes back unprovisioned, so the LED naturally lands on
  setup **white**. The reboot + white *is* the confirmation; no separate
  "wiped" state needed.

## Optional overlays (say the word and I fold them in)

- **OTA updating** — a **cyan** slow pulse while the image downloads/applies,
  so an installer can see an update happening (closes the "no OTA indicator"
  gap). The post-update reboot returns to resting.
- **Sensor offline (attention)** — a brief **red triple-blink every ~10 s**
  over the resting state: non-intrusive "check the app" without hijacking the
  colour.
- **Leak detected (critical)** — the roadmap has a leak sensor; if/when it's
  wired, this should be the **highest-priority** overlay: a fast, unmistakable
  **red/off strobe** that overrides everything until cleared. Full app +
  firmware + LED path: `gs_rework/documentation/LEAK_ALERT_SPEC.md`.

## Notes

- Keep the palette small and distinct (white / blue / green / amber / red, plus
  cyan for OTA). Don't lean on **red-vs-green hue alone** for anything critical
  — that's why relay ON/OFF is breathe-vs-steady (motion) and events use
  distinct patterns, so it stays legible for colour-blind users. The green/red
  pair is only used for the button *confirmation* flashes, where it's transient
  and unambiguous.
- `led_task`: `led_init(GPIO_NUM_1)` then
  `xTaskCreate(led_task, "led", 2048, NULL, 2, NULL)`, a `for(;;)` loop on a
  ~20–30 ms tick (smooth breathe/blink), registered with the task WDT
  (`esp_task_wdt_add(NULL)` + `esp_task_wdt_reset()` each iteration) exactly
  like `button_task`. It reads the state getters each tick; no blocking calls.
- Add the `led_strip` dependency to `idf_component.yml`.

## Open decisions for you

1. **Thresholds.** This spec follows the firmware's real **5 s** toggle /
   **10 s** wipe. You mentioned 3 s / 9 s — if you want those, I'll change the
   button constants and the LED tracks them automatically.
2. **Relay display.** Recommended: colour = connectivity + breathe = relay
   (always shows both). Your alternative — green = ON / red = OFF — is very
   legible but collides with green = cloud-online, so I kept green/red only for
   the button *confirm* flashes. Happy to switch the resting model to
   relay-as-colour if you prefer.
3. **Overlays.** Include the OTA cyan pulse, the sensor-offline blink, and/or
   the leak strobe now, or defer?
4. **WS2812B at 3.3 V.** Pin (GPIO1) and part (WS2812B D9) are fixed on the
   board; the only open hardware question is confirming reliable 3.3 V
   operation (see the Hardware note) or choosing a 3.3 V-native pixel.
