# User guide

Everything the device does, screen by screen.

<!-- TODO: this page is the full manual. Each section below is a stub — expand
     with a screenshot + the actual button behavior for that screen. -->

## Screens

<!-- TODO: navigation model — how you move between screens (left/right?), what
     center does per screen -->

### Overview (Helga)

The home screen. Helga reacts to what's around you.

<!-- TODO: what Helga's states mean, top-bar icons (battery / USB / BT / WiFi /
     bell), what the title shows -->

### Devices

Live list of everything seen nearby, deduplicated by MAC, with vendor names
resolved from the on-device IEEE OUI / BT SIG table.

<!-- TODO: what a card shows (name, vendor, RSSI, beacon vs probe), sort order,
     how to open a device and start a foxhunt from it -->

### Alerts

Active alert cards — one per (ruleset, device) pair. Alerts persist across
sleep and only clear when acknowledged.

<!-- TODO: acknowledging, what UPDATED vs new alerts look like, alert types -->

### Foxhunting

Lock onto one device and walk it down using live signal strength. While
hunting, the scan/sleep duty cycle is suspended: BLE targets use continuous
active scanning, Wi-Fi targets use a promiscuous sniffer.

<!-- TODO: how to start/stop a hunt, reading the RSSI display, tips (body
     shielding, antenna orientation, walking pattern) -->

### Settings

<!-- TODO: walk each setting group — alert outputs (vibe/LED/screen wake),
     scan timing (sleep duration / scan duration), display (brightness, dim,
     color scheme), rules enable/disable -->

## Alerts & rulesets

Each factory ruleset controls its own alert: title, LED pattern, vibration
pattern. Enable or disable rulesets per your threat model — see
[Detection rules](rules.md).

## Power & battery

- Between scans the device deep-sleeps; center button wakes it.
- <!-- TODO: expected battery life at default duty cycle per battery size -->
- **Shipping mode** powers the device fully down for storage.
  <!-- TODO: how to enter/exit shipping mode -->

## The USB console

Plug into USB and open a serial terminal (or the console tab of the
[web flasher](https://pips801.github.io/Helgatchi/)) for the full command
interface — see [Serial console](serial-console.md).
