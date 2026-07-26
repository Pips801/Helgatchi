# Helgatchi firmware

PlatformIO + Arduino-ESP32 firmware for the Seeed XIAO ESP32-S3. LVGL 9.5
(generated from the EEZ Studio project in [`../UI/`](../UI/)) + LovyanGFX for
the display, NimBLE for BLE, promiscuous mode Wi-Fi sniffing, FastLED for the
LEDs.

```
pio run                  # build
pio run -t upload        # flash firmware (NVS settings preserved)
pio run -t buildfs && pio run -t uploadfs   # build + flash LittleFS from data/
pio run -t erase         # full chip erase - the escape hatch for stale state
```

The build requires `HELGATCHI_HMAC_SECRET` and `HELGATCHI_ADMIN_PASSWORD` in
the environment (or `ALLOW_DEV_ADMIN_SECRET=1` for local dev builds).

## Docs

- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) - services, event bus, storage tiers, footguns
- [docs/WRITING_RULES.md](docs/WRITING_RULES.md) - the rules engine reference
- [docs/PHASE_6_SCAN_ENGINE.md](docs/PHASE_6_SCAN_ENGINE.md) - the scan engine
- [GPIO.md](GPIO.md) - pinout | [SCREENS.md](SCREENS.md) - UI wiring
- User-facing docs (getting started, flashing, troubleshooting): [`/docs/`](../../docs/README.md)
