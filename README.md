<div align="center">

<!-- TODO: hero photo of the device in hand, ~700px wide -->
<!-- <img src="docs/images/hero.jpg" width="600" alt="Helgatchi in hand"> -->

# Helgatchi V2 🐑

**A pocket-sized BLE + Wi-Fi hunter that tells you when surveillance tech is nearby.**

[![Latest release](https://img.shields.io/github/v/release/Pips801/Helgatchi)](https://github.com/Pips801/Helgatchi/releases/latest)
[![Web Flasher](https://img.shields.io/badge/flash-web%20flasher-2ea043)](https://pips801.github.io/Helgatchi/)
<!-- TODO: license badge once a license is chosen -->

[Getting started](docs/getting-started.md) | [Documentation](docs/README.md) | [Flash firmware](https://pips801.github.io/Helgatchi/) | [Build your own](docs/hardware/assembly.md)

</div>

---

## What is it?

Helgatchi is a handheld, rechargeable scanner built around the Seeed XIAO
ESP32-S3. It passively listens to BLE advertisements and Wi-Fi management
traffic, matches what it hears against a library of detection rulesets, and
alerts you - LEDs, vibration, screen wake - when something interesting is in
range. It lives in your pocket and runs a scan/sleep duty cycle to stretch
battery life.

It ships with **55+ factory rulesets** covering things like:

- **Surveillance infrastructure** - Flock Safety ALPRs, ShotSpotter, Genetec, Neology readers, Motorolla solutions, and more
- **Tactical equipment** - Axon equipment, WatchGuard, Digital Ally, VieVu, Motorola Solutions, and more
- **Hidden cameras** - Wyze / Blink / Tuya covert cams, generic P2P camera modules
- **Trackers & tags** - Tile, Chipolo, Samsung SmartTag, Moto Tag, Pebblebee, eufy
- **Wearables** - Fitbit, Garmin, Whoop, Oura, Polar

Every detection is a JSON ruleset, and nothing is hardcoded. You can [write your
own](docs/rules.md) to hunt any device by MAC/OUI, manufacturer, name/SSID,
BLE service, or Wi-Fi probe fingerprint.

## Detection and alerting capabilites

|    🐑  	| MAC address 	| OUI (explicit) 	| OUI organization 	| MFG (explicit) 	| MFG organization 	| 802.11 SIG 	| Device name 	| SSID 	|
|------	|-------------	|----------------	|------------------	|----------------	|------------------	|------------	|-------------	|------	|
| WiFi 	|      ✅      	|        ✅       	|         ✅        	|        ❌       	|         ❌        	|      ✅     	|      ❌      	|   ✅  	|
| BLE  	|      ✅      	|        ✅       	|         ✅        	|        ✅       	|         ✅        	|      ❌     	|      ✅      	|   ❌  	|

<!-- TODO: photo strip - alert screen / devices list / foxhunting screen -->

## How it works

- **Passive scanning.** BLE and Wi-Fi are time-multiplexed; Wi-Fi discovery is
  a promiscuous management-frame sniffer, so the device transmits nothing
  while it hunts.
- **Rules engine.** Scan results stream through user-editable JSON rulesets
  with pattern/regex matching backed by a ~700 KB on-flash IEEE OUI + BT SIG
  vendor table.
- **Alerts.** A match raises an alert card and fires the ruleset's configured
  LED pattern, vibration pattern, and screen wake. Alerts persist across sleep.
- **Foxhunt mode.** Lock onto a single device and use live RSSI to walk it
  down - continuous scanning with the power manager's duty cycle suspended.

## Quick start

1. **Get a device** - [buy one or build one](#where-to-get-one).
2. **Flash the latest firmware** - plug in USB-C and use the
   [web flasher](https://pips801.github.io/Helgatchi/) (Chrome/Edge). No
   toolchain needed.
3. **Read [Getting started](docs/getting-started.md)** - first boot, buttons,
   screens, and your first alert.

## Documentation

| I want to... | Read this |
|---|---|
| Frequently asked questions | [FAQ](docs/faq.md) |
| Set up my new device | [Getting started](docs/getting-started.md) |
| Learn the screens, buttons, and settings | [User guide](docs/user-guide.md) |
| Update or recover firmware | [Flashing & updating](docs/flashing.md) |
| Write my own detection rules | [Detection rules](docs/rules.md) |
| Use the serial console | [Serial console](docs/serial-console.md) |
| Fix a problem | [Troubleshooting](docs/troubleshooting.md) |
| Build one from scratch | [Hardware guide](docs/hardware/README.md) |
| Print a spacer or screen bumper | [3D printing](docs/hardware/3d-printing.md) |
| Repair a broken screen / battery | [Repair guide](docs/hardware/repair.md) |
| Hack on the firmware | [Developer guide](docs/developers.md) |


## Where to get one

<!-- TODO: where devices/kits are sold - DC801 / 801 Labs, cons, kit availability, price -->

Helgatchi is open hardware. If you can't buy one, you can
[build one](docs/hardware/assembly.md) - the KiCad sources, fab outputs, and
printable parts are all in this repo.

## Repository layout

| Path | Contents |
|---|---|
| [`Software/Helgatchi-Firmware/`](Software/Helgatchi-Firmware/) | PlatformIO firmware (ESP32-S3, LVGL 9.5, NimBLE) + internal dev docs |
| [`Software/UI/`](Software/UI/) | EEZ Studio project that generates the UI sources |
| [`Hardware/`](Hardware/) | KiCad PCBs (front, back, test jig), spacer & bumper CAD, drawings |
| [`WebFlasher/`](WebFlasher/) | Web Serial flasher + console, deployed to GitHub Pages |
| [`docs/`](docs/README.md) | User, builder, and repair documentation |

## Specifications

| Spec | Info |
|---|---|
| MCU | Seeed XIAO ESP32-S3 (8 MB flash, 8 MB PSRAM) |
| Screen | Waveshare 1.69" rounded IPS, 240x280, ST7789 @ 80 MHz SPI |
| Radios | 2.4 GHz BLE + Wi-Fi (time-multiplexed scanning) |
| Antenna | XIAO u.FL -> back PCB -> RP-SMA -> external 2.4 GHz antenna |
| Alerts | 6x RGB LEDs, vibration motor, screen wake |
| Input | 3 reverse-mount buttons |
| Battery | 400 mAh 3.7 V LiPo, USB-C charging, deep-sleep duty cycle |
| Firmware | PlatformIO + Arduino-ESP32 | LVGL 9.5 + EEZ Studio | LovyanGFX | NimBLE | FastLED |
| Size | 50 mm x 80 mm  |

## Community & contributing

<!-- TODO: where to find the community - Discord/Matrix, DC801, issue tracker etiquette -->

Bug reports and pull requests welcome. Firmware contributors should start with
the [developer guide](docs/developers.md).

## License

In short: this project is free for non-commercial use. You can build one for yourself, fork the code, modify the design/firmware, and learn from all of it. What you cannot not do is profit from this work: you may not sell any product running this firmware, and you may not sell hardware based on this design - whether built from the design files or reproduced from the board itself.

Hardware license: PolyForm Noncommercial 1.0.0
Software license: CC BY-NC-SA 4.0

*License not yet chosen.*
