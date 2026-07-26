# Flashing & updating

## Web flasher (recommended)

The [web flasher](https://pips801.github.io/Helgatchi/) runs entirely in the
browser via Web Serial — no toolchain, no drivers on most systems.

Requirements: Chrome or Edge (Firefox/Safari don't support Web Serial), a
data-capable USB-C cable.

1. Open the flasher and click **Connect**; pick the device's serial port.
2. It reads your current firmware version and lists available releases.
3. Click install. Don't unplug until it reboots.

<!-- TODO: screenshot of the flasher; note about the pre-flash "updating"
     screen once implemented -->

## What survives an update?

| Data | Firmware update | Filesystem flash | Full erase |
|---|---|---|---|
| Settings (NVS) | ✅ kept | ✅ kept | ❌ reset |
| Factory rulesets | ✅ kept | 🔄 replaced with shipped set | 🔄 replaced |
| User rulesets (`/rules/user/`) | ✅ kept | ❌ erased | ❌ erased |
| Factory-rule enable/disable choices | ✅ kept | ✅ kept | ❌ reset |

## Recovery / manual bootloader

If the device won't enumerate or a flash fails partway:

<!-- TODO: exact BOOT+RESET button procedure to force the ESP32-S3 ROM
     bootloader on this hardware, and how to reach the buttons -->

## Flashing from source

Building and flashing with PlatformIO is covered in the
[developer guide](developers.md). The nuclear option for a device in a weird
state is `pio run -t erase` followed by a full firmware + filesystem flash —
this resets *everything*.
