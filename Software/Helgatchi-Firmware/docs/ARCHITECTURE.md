# Architecture

Quick map of the firmware. For per-phase implementation history see the
other docs in this directory; for current next-up work see `PHASE_6_SCAN_ENGINE.md`.

## Hardware

- **MCU**: ESP32-S3 (Seeed XIAO ESP32-S3 module)
- **Flash**: 8 MB
- **PSRAM**: 8 MB (LVGL framebuffers + LVGL allocator pool + scan ring + seen-devices map)
- **Display**: ST7789 SPI panel @ 80 MHz, 280×240 (no TE pin — frame tearing
  inherent above ~30 fps animation)
- **Radios**: BLE + WiFi (NimBLE for BLE, ESP32 WiFi for scan)
- **LEDs**: addressable WS2812 chain via FastLED
- **Motor**: PWM-driven haptic
- **Buttons**: left / right / center (debounced in `HalService`)
- **Battery**: VSENSE via ADC, USB-attach detect via SOF, charge-state inferred
- **Power**: deep-sleep wake via EXT1 on center button + RTC timer for scheduled scan windows; shipping mode for cold storage

## Build system

PlatformIO, Arduino-ESP32 framework. Three pre-scripts run before each
build (see `platformio.ini`):

1. `scripts/auto_montserrat.py` — scans source for referenced LVGL Montserrat
   font sizes and `-D`s them into the build.
2. `scripts/build_info.py` — emits `include/build_info.h` with build date and
   UI version (mtime of `Helgatchi UI.eez-project`).
3. `scripts/build_vendor_tables.py` — generates `include/vendor_tables_data.h`
   (~700 KB of flash data) from `scripts/vendor_sources/oui.csv.gz` and
   `scripts/vendor_sources/bt_companies.yaml`. Run
   `scripts/refresh_vendor_sources.py` manually to update the upstream cache.

Partition layout (`partitions.csv`): NVS 28 KB / factory 5 MB / spiffs ~3 MB
(LittleFS-formatted; partition subtype is `spiffs` because that's what
esptool/PIO look for, but the on-disk format is LittleFS via
`board_build.filesystem = littlefs`).

## Service model

Every subsystem is a `class FooService { void begin(EventBus&); void tick(); ... };`
with a single global instance (`extern FooService g_foo;`). `setup()` in
`main.cpp` initializes them in a specific order (some have prerequisites —
e.g. `g_rules` depends on `g_scan_service` and `g_alerts` being ready, and on LittleFS
being mounted).

Service catalog (`src/main.cpp` shows current init order):

| service             | role                                                                  |
|---------------------|-----------------------------------------------------------------------|
| `g_bus`             | Pub/sub event bus (queued; drained per main-loop tick)                |
| `g_settings`        | NVS-backed settings store (`SettingsKey` enum + `SKEY_*` macros)      |
| `g_hal`             | Display, LEDs, motor, buttons, ADC                                    |
| `g_logger`          | LVGL perf overlay gating, serial debug-level filtering                |
| `g_console`         | Serial command parser                                                 |
| `g_power`           | Scan/sleep cycle, battery sampling, shipping mode                     |
| `g_alerts`          | Active alert store (RTC slow memory persistence across deep sleep)    |
| `g_scan_service`            | Scan-result ring buffer (256) + seen-devices map (128). Both in PSRAM |
| `g_rules`           | Rules engine — loads `/rules/{factory,user}/*.json`, drains `g_scan_service`  |
| `g_leds`            | LED pattern renderer (~30 fps), pattern name registry                 |
| `g_vibe`            | Haptic step-machine, pattern name registry                            |
| `g_ui`              | LVGL `lv_timer_handler` wrapper, render gating when screen off        |
| `g_display`         | Top-bar icon strings (battery / USB / BT / WiFi / bell)               |
| `g_settings_screen` | Settings widget wiring (EEZ-generated UI)                             |
| `g_alerts_screen`   | Alert card UI (lives in its own file mirroring settings_service split)|

## Event bus

`include/event_ids.h` is the source of truth for event IDs. Two flavors:

- `CMD_*` — imperative requests (`CMD_SCAN_START`, `CMD_ALERT_ACK`, etc.)
- `EV_*` — immutable facts (`EV_ALERT_RAISED`, `EV_BATTERY_UPDATED`, etc.)

Payloads are an 8-byte tagged union (`include/event_payload.h`). Adding a new
event ID *to send a custom payload* means extending the union.

**Hot path note**: scan results do NOT go through the bus. `g_scan_service.publish()`
writes to a PSRAM ring buffer; `g_rules.tick()` drains it directly via
`g_scan_service.drain()`. Only the alert path that fires *after* matching emits bus
events (downstream of the hot loop).

## Storage tiers

| tier             | what lives here                                                                          |
|------------------|------------------------------------------------------------------------------------------|
| Flash (app)      | Firmware, generated vendor tables, factory rule JSONs (via LittleFS image at build time) |
| NVS              | Settings (`g_settings`), rule enable/disable overlay                                     |
| RTC slow memory  | Active alerts (`g_alerts`) — survives deep sleep, cleared on cold boot                   |
| LittleFS         | `/rules/factory/*.json` (read-only at runtime), `/rules/user/*.json` (writable)          |
| PSRAM            | LVGL framebuffers + LVGL allocator pool, scan ring + seen-devices map                    |
| Internal SRAM    | Stack, FreeRTOS, hot in-flight data only                                                 |

## Conventions

- **Singular/plural serial verbs**: `setting`/`settings`, `alert`/`alerts`,
  `led`/`leds`, `rule`/`rules`. Singular mutates one thing; plural lists or
  manages many.
- **Underscore-as-space in serial values**: multi-word titles in `alert` and
  `rule create` use underscores (`title=Axon_device_nearby`). Other fields
  (vibe, led, type, action, etc.) take registry names verbatim.
- **Comma-separated arrays in rule criteria**: `rule add foo mfg=0x05D2,0x004C`
  adds two atomic criteria. `oui_org_contains` and `mfg_org_contains` expand
  at parse time into many atomic criteria by walking the vendor table.
- **Per-(rule, MAC) dedup**: alerts use identifier format
  `<rule_name>:<MAC_hex>` so re-firing on the same device updates last_seen
  rather than stacking new alerts.
- **Factory rule immutability**: `_rules[i].is_factory == true` blocks all
  content mutations. Only `enable`/`disable` works on factory rules, persisted
  via NVS overlay (survives FS reflash).

## Debugging / introspection commands

- `stats` — uptime, chip, memory, display fps, bus drops
- `scan` — list seen devices (dedup'd by MAC)
- `scan inject domain=... mac=... mfg=... name=...` — synthesize a scan
  result for testing the rules engine without real radio
- `vendor oui <AA:BB:CC>` / `vendor mfg <0xNNNN>` — forward lookup
- `vendor search <substr>` — list every IEEE org + BT SIG name containing the
  substring (same code path RulesService takes at rule load for `*_org_contains`)
- `rules` — list all rules with their full criteria (so `rule rm <name> <idx>`
  is unambiguous)
- `rules show <name>` / `rules stats` / `rules reload`
- `selftest` — GPIO short/load detection

## Things that have already burned us

- **DMA + PSRAM**: cache writeback needed before LovyanGFX flushes from PSRAM.
  See the `Cache_WriteBack_Addr` call in `ui_controller.cpp` flush_cb.
- **NimBLE timing**: not in the codebase yet (Phase 6), but historically
  `NimBLEDevice::init` on Arduino-ESP32 must come after WiFi init if both
  share radio coex.
- **LittleFS unlink while iterating**: the directory iterator holds FDs on
  entries — collect paths first, close the dir, then unlink.
- **Partition table changes** wipe NVS (and reformat LittleFS if the
  partition moves). Avoid changing offsets once stable.
- **ArduinoJson 7** allocates dynamically; on big rule files use stack-scoped
  `JsonDocument` to avoid leaks.
