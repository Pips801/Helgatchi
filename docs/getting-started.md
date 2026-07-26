# Getting started

From box to first alert in a few minutes.

## What's in the box

<!-- TODO: photo + list — device, antenna, USB-C cable?, lanyard? -->

- Helgatchi device
- 2.4 GHz RP-SMA antenna

## 1. Attach the antenna

Screw the antenna onto the RP-SMA connector. The device works without it, but
range drops dramatically.

## 2. Charge it

USB-C, any 5 V charger.

<!-- TODO: charge LED behavior, time-to-full for common battery sizes, how the
     battery icon reads while charging -->

## 3. Power on

<!-- TODO: exact button behavior — which button wakes/powers on, hold time,
     what shipping mode is and how a fresh device leaves it -->

The device may arrive in **shipping mode** (deep storage). Plugging in USB
wakes it.

## 4. Meet the buttons

| Button | Short press | Long press |
|---|---|---|
| Left | <!-- TODO --> | <!-- TODO --> |
| Center | wake / select | <!-- TODO --> |
| Right | <!-- TODO --> | <!-- TODO --> |

## 5. Your first alert

Out of the box, factory rulesets are already active. Walk around — when a
matching device (a Tile tracker, a Ring doorbell, a Flock camera…) comes into
range the Helgatchi vibrates, lights up, and shows an alert card.

<!-- TODO: screenshot of an alert card; how to acknowledge/dismiss an alert -->

To test indoors without waiting for the real thing:

<!-- TODO: easiest self-test for a new user, e.g. enable a common-device
     ruleset (Sonos, Philips Hue) or use `scan inject` over the serial console -->

## 6. Update the firmware

New devices may ship with old firmware. Open the
[web flasher](https://pips801.github.io/Helgatchi/) in Chrome or Edge, plug in
USB-C, and click **Connect** — it shows your version and offers the latest
release. Settings survive firmware updates.

## Next steps

- [User guide](user-guide.md) — every screen and setting
- [Detection rules](rules.md) — tune what it alerts on
- [Troubleshooting](troubleshooting.md) — if something's off
