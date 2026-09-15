# GeyserSwitch — firmware

ESP-IDF firmware for the GeyserSwitch smart geyser (water-heater)
controller, targeting the **ESP32-C6**. Provides BLE GATT services for
local control and provisioning, and a Firebase Realtime Database client
for remote control, telemetry and alerts over WiFi.

- **Firmware** (this repo): ESP-IDF v5.3, sources under `main/`.
- **App**: separate repo `gs_rework` (Flutter).

The device switches the geyser at the **mains**, upstream of the geyser's
own mechanical thermostat — it is an energy/scheduling controller, **not a
safety device**, and must not be relied upon as one.

## ⚠️ Safety

This firmware drives a relay on a live mains circuit supplying a water
heater. Incorrect wiring, assembly, configuration or use can cause
electric shock, fire, burns, scalding, water damage, property damage,
serious injury or death.

It carries no safety integrity rating and has not been certified against
any electrical safety standard. Electrical work must be carried out by a
suitably qualified and, where required, licensed person under the wiring
rules applicable where you are. **Isolate the supply before working on any
circuit.** Full terms in Section 5 of [LICENSE](LICENSE).

## ⚠️ Licence — this is not open source

The source is published so people can read it and try it out. That is the
whole of the permission granted.

**You may** read the code, and build and flash it privately, on hardware
you own, to evaluate it.

**You may not** use it commercially, redistribute it in source or binary
form, publish a firmware image for download or OTA, offer it as a service,
distribute modified versions, manufacture or sell any device based on it,
or use it as machine-learning training data.

[LICENSE](LICENSE) governs; the summary above is not a substitute for
reading it.

The Espressif components vendored under `components/` and
`managed_components/` are **Apache-2.0** and keep their own terms — see
Section 6 of [LICENSE](LICENSE). ESP-IDF itself is not redistributed here.

## Building

Requires ESP-IDF v5.3 and its toolchain on `PATH`:

```bash
. $IDF_PATH/export.sh
idf.py set-target esp32c6
idf.py build
idf.py -p /dev/cu.usbmodemXXXX flash monitor
```

`sdkconfig` is generated and git-ignored; project configuration lives in
`sdkconfig.defaults`. Change settings there, not in the generated file, or
they will be lost.

## Layout

    main/               application sources
      ble_init.c        NimBLE host, GAP, advertising, pairing
      gatt_server.c     control service (temperature, relay, limits, timers)
      wifi_prov.c       provisioning service, WiFi join, credentials
      firebase_rtdb.c   RTDB client: SSE settings stream, live/telemetry push
      device_state.c    the single source of truth for relay and setpoints
    components/         vendored third-party (Apache-2.0)
    tools/              helper scripts
    partitions.csv      OTA-capable layout with rollback

Further docs: [`SECURE_BOOT.md`](SECURE_BOOT.md) for the opt-in Secure Boot
v2 / flash-encryption path, [`LED_STATUS_SPEC.md`](LED_STATUS_SPEC.md) for
the status-LED grammar. The app ↔ firmware contract is documented in the
`gs_rework` repo under `documentation/`.

## Contributing

Not accepting contributions. Please do not open pull requests — see
[CONTRIBUTING.md](CONTRIBUTING.md).
