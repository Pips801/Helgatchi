# Developer guide

Firmware lives in
[`Software/Helgatchi-Firmware/`](../Software/Helgatchi-Firmware/); its
internal docs are the source of truth for architecture. This page is the
on-ramp.

## Building

PlatformIO (VS Code extension or CLI), Arduino-ESP32 framework.

```
pio run                  # build firmware
pio run -t upload        # flash firmware (settings/NVS preserved)
pio run -t buildfs       # build LittleFS image from data/
pio run -t uploadfs      # flash LittleFS image (wipes user rulesets)
pio run -t erase         # nuke the chip - full reset
```

**Admin-mode secrets:** the build **hard-fails** without
`HELGATCHI_HMAC_SECRET` and `HELGATCHI_ADMIN_PASSWORD` in the environment. For
local development set `ALLOW_DEV_ADMIN_SECRET=1` instead. You can also put `ALLOW_DEV_ADMIN_SECRET=1` in a `.env` file in the root of the `Software` directory.

## Read these next

| Doc | What it covers |
|---|---|
| [ARCHITECTURE.md](../Software/Helgatchi-Firmware/docs/ARCHITECTURE.md) | Services, event bus, storage tiers, build scripts, past footguns |
| [WRITING_RULES.md](../Software/Helgatchi-Firmware/docs/WRITING_RULES.md) | The rules engine, end to end |
| [PHASE_6_SCAN_ENGINE.md](../Software/Helgatchi-Firmware/docs/PHASE_6_SCAN_ENGINE.md) | The BLE/Wi-Fi scan engine |
| [LVGL_PERFORMANCE.md](../Software/Helgatchi-Firmware/docs/LVGL_PERFORMANCE.md) | Rendering constraints on this hardware |
| [GPIO.md](../Software/Helgatchi-Firmware/GPIO.md) / [SCREENS.md](../Software/Helgatchi-Firmware/SCREENS.md) | Pinout / UI wiring |

## The UI

The LVGL UI is generated from the EEZ Studio project in
[`Software/UI/`](../Software/UI/). **Never hand-edit generated UI sources** -
change the `.eez-project` and regenerate.

## The web flasher

[`WebFlasher/`](../WebFlasher/) is a static esptool-js site deployed to GitHub
Pages by `.github/workflows/pages.yml`. It pulls firmware binaries from GitHub
Releases at deploy time, so publishing a release updates the flasher without
touching the site.

## Releases

Firmware version releases are github version tags. A Github actions job (`.github/workflows/release.yml`) will run when a new tag is pushed, building the firmware using stored secrets for `HELGATCHI_ADMIN_PASSWORD` and `HELGATCHI_HMAC_SECRET`, which gate Admin mode behind a password and signs admin commands. 

Release major and minor versions are as follows:
- Major changes (new features, new settings keys, performance fixes, new UI screens, revamps, etc) change the major version tag (eg. 2.X.0)
- Minor changes (bug fixes, grammatical fixes, small filesystem changes, behavior tweaks, etc) are minor version tag bumps (eg. 2.0.X)
- The overall version is not expected to change, aside from a major hardware and software revision or update.

## Contributing

Good first contributions: new factory rulesets (see
[Detection rules](rules.md)), documentation TODOs, and spacer/bumper variants.
