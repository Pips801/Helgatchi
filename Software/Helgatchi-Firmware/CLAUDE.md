# Helgatchi Firmware — orientation

ESP32-S3 foxhunting handheld (Seeed XIAO ESP32-S3, 8 MB flash, 8 MB PSRAM).
LVGL 9.5 + EEZ Flow Studio drives the UI; LovyanGFX for the ST7789 panel.

**Current state**: Phase 5 complete (rules engine + LittleFS persistence). Next
work is the scan engine — see `docs/PHASE_6_SCAN_ENGINE.md`.

For the bigger picture (services, event bus, partitions, build flow), see
`docs/ARCHITECTURE.md`.

## Conventions to follow

- **Serial commands**: singular verb mutates one (`setting`, `alert`, `rule`,
  `ruleset` deprecated → `rule`), plural verb lists/manages (`settings`,
  `alerts`, `rules`).
- **Services**: live in `src/`, headers in `include/`. Follow the
  `begin(EventBus&)` / `tick()` / `onEvent(const Event&)` pattern.
- **Rules**: defined as JSON files in `/rules/factory/` (read-only, shipped in
  LittleFS image) and `/rules/user/` (writable, auto-saved on mutation). Never
  hardcoded in C++. `name`/`ssid`/`oui_org`/`mfg_org` are case-insensitive
  full-match patterns (equals `x`, contains `.*x.*`, plus `\d`/`[...]` regex);
  see `docs/WRITING_RULES.md`. The regex engine is `lib/re_lite/`.
- **Vendor data**: ~700 KB of IEEE OUI + BT SIG name table in flash, generated
  at build time from `scripts/vendor_sources/*` via `scripts/build_vendor_tables.py`.
  Forward lookup only via `vendor_lookup.h`.
- **LED / vibe patterns**: name registry lives with the service that owns
  them (`led_service.cpp`, `vibe_service.cpp`). Rules and serial console
  resolve by name via `ledPatternByName()` / `vibePatternByName()`.

## Things not to do

- **Don't add band-aid code** for one-off device states. If user files end up
  orphaned, the answer is `pio run -t erase` + reflash, not auto-cleanup logic.
- **Don't duplicate data** between code and files. If something belongs as
  JSON in `data/`, it stays there — don't shadow it in C++.
- **Don't be verbose**. Direct sentences, no "let me explain" preambles. The
  user is technical and gets frustrated by over-explanation or speculative
  complexity.
- **Don't suppress logs or warnings to hide problems**. Fix the root cause
  or accept the noise — don't silence diagnostics.

## Build / flash

```
pio run                  # firmware
pio run -t upload        # flash firmware (NVS preserved across firmware-only flashes)
pio run -t buildfs       # build LittleFS image from data/
pio run -t uploadfs      # flash LittleFS image
pio run -t erase         # nuke entire chip (settings + FS reset; needed only to escape stale state)
```

Partition table: 28 KB NVS + 5 MB app + ~3 MB LittleFS (`partitions.csv`).
No OTA, no coredump partition (the boot log line about "No core dump partition
found" is harmless ESP-IDF noise and can't be suppressed from C++).
