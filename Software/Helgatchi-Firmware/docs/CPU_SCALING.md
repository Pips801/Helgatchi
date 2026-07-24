# CPU frequency scaling — investigated and ABANDONED (2026-07-10)

Goal was to underclock the CPU during autonomous (screen-off) scan cycles to save
battery while keeping 240 MHz when interactive. **Dropped from the code entirely.**
This is the record so it isn't re-attempted from scratch.

## Why it doesn't work on this board

Runtime `setCpuFrequencyMhz()` **hangs** this hardware (Seeed XIAO ESP32-S3, 8 MB
octal PSRAM, LovyanGFX SPI display). Confirmed on battery with the BLE scan stopped
and no USB attached — so it is **not** the radio and **not** USB CDC (both were
suspected and ruled out). The cause is the runtime clock switch itself once the
driver stack is up: the **MSPI bus (flash execute-in-place + PSRAM) timing is
calibrated per-frequency at boot and is not re-tuned by a raw `setCpuFrequencyMhz()`**.
Booting at a fixed 80 *or* 240 works fine; *transitioning* between them at runtime
corrupts memory-bus timing and hangs.

- Near-identical documented case: [arduino-esp32 #6872](https://github.com/espressif/arduino-esp32/issues/6872)
  — SPI LCD + PSRAM + `setCpuFrequencyMhz()` → crash during the call.
- Contrast [#6032](https://github.com/espressif/arduino-esp32/issues/6032), which
  *works* — but it's a bare classic ESP32 with **PSRAM off and no SPI display**.

Supporting facts:
- On the S3, APB is fixed at 80 MHz across CPU 240/160/80, so APB-clocked peripherals
  (the display SPI is `freq_write = 80 MHz` in `lgfx_config.h`, plus esp_timers, LEDC,
  UART) are unaffected. The display SPI is *not* the blocker — it was idle when we hung.
- The memory bus (flash + PSRAM) is the blocker, and it can't be quiesced — it's what
  runs the code doing the switch.
- Below 80 MHz breaks the radios and garbles UART; 80 is the floor regardless.

## What was tried (all reverted)

| # | Approach | Result |
|---|----------|--------|
| 1 | Adaptive runtime scaling in `loop()` (240/80, queue-pressure escalation) | Froze on `power sleepscreen` (USB); bricked on battery (retune after driver init) |
| 2 | Force 240 whenever USB/serial attached | Still bricked on battery TIMER-wake |
| 3 | Per-boot clock from wake cause + upward-only runtime upclock on screen-on | Froze on a button press *during* a scan window |
| 4 | Approach A: stop BLE → `setCpuFrequencyMhz(240)` → resume BLE on wake | **Froze on battery, radio-stopped, no USB** — proved it's the runtime switch (MSPI), not the radio |
| 5 | Per-boot only (boot scans at 80, no runtime change) | Worked & no freeze, but a mid-scan wake ran the UI at 80 → unacceptably laggy menus |

The only survivor from the effort was a LED idle-off gate (skip the 30 Hz all-black
RMT push when nothing's displayed) — also reverted with the rest for a clean slate;
trivially re-addable in `LedService::tick()` if ever wanted.

## The only path that could actually work (not pursued)

`esp_pm` (ESP-IDF Dynamic Frequency Scaling) is the only mechanism that re-tunes the
MSPI on the fly, via power-management locks. It needs `CONFIG_PM_ENABLE` (a custom
sdkconfig / core rebuild under PlatformIO) **and** a main loop that yields/blocks so
tickless idle engages — ours busy-polls and never blocks. That's a large rework, and
the payoff is secondary: during an awake scan the **radio dominates power, not the
CPU**, and the deep-sleep duty cycle between scans already does the real battery work.

## Verdict

Not worth it on this hardware. If battery life needs improvement, tune the
deep-sleep duty cycle (`scan_duration_s` / `sleep_duration_s`) or the BLE scan duty
cycle — not CPU frequency.
