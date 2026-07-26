# Troubleshooting

Symptoms first, theory later. If a fix here doesn't work, open an issue with
your firmware version (`stats` on the [serial console](serial-console.md)).

## It won't turn on

- Charge it for 30 minutes — a deeply drained LiPo can take a while to show life.
- It may be in **shipping mode**: plug in USB to wake it.
- <!-- TODO: hard-reset procedure for this hardware -->

## The web flasher can't see it

- Use Chrome or Edge — Firefox and Safari don't support Web Serial.
- Try another cable; charge-only USB-C cables are everywhere.
- Close anything else holding the serial port (PlatformIO monitor, Arduino IDE).
- Force the bootloader: <!-- TODO: BOOT+RESET procedure --> then reconnect.

## It's in a weird state (settings/rules corrupted, boot loops)

The escape hatch is a full erase and reflash — there is deliberately no
auto-repair logic:

1. `pio run -t erase` (or the flasher's full-erase option <!-- TODO: confirm the web flasher exposes this -->)
2. Reflash firmware + filesystem.

This resets all settings and deletes user rulesets.

## It never alerts

- Check the antenna is attached and snug.
- Check the ruleset you expect is enabled (`rules` on the console).
- Test the pipeline without radio: `scan inject …` — see
  [Serial console](serial-console.md). If injection alerts but the real device
  doesn't, the rule's criteria don't match what the device actually broadcasts.

## It alerts constantly

That's usually the factory set being broader than your threat model — disable
the consumer-gadget rulesets (Sonos, Hue, JBL, …). See
[Detection rules](rules.md).

## Battery drains fast

- Foxhunt mode suspends the sleep cycle — don't leave a hunt running.
- Increase sleep duration / decrease scan duration in Settings.
- <!-- TODO: expected battery life table by duty cycle + battery size -->

## Boot log says "No core dump partition found"

Harmless ESP-IDF noise — there is intentionally no coredump partition. Ignore it.

## Screen is cracked / dim / dead

Screen replacement is a repair, not a loss —
see the [repair guide](hardware/repair.md).

## Charging issues

<!-- TODO: charge LED states, what "USB attached but not charging" means,
     battery connector check -->
