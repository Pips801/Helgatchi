# Troubleshooting

Symptoms first, theory later. If a fix here doesn't work, open an issue with
your firmware version (`stats` on the [serial console](serial-console.md)).

## It won't turn on

- Charge it for 30 minutes - a deeply drained LiPo can take a while to show life.
- It may be in **shipping mode**: Press and hold (BAAA) for a few seconds.

## The web flasher can't see it

- Use Chrome, edge, Firefox, or Opera. Safari and iOS devices don't support Web Serial.
- Try another cable; charge-only USB-C cables are everywhere.
- Close anything else holding the serial port (PlatformIO monitor, Arduino IDE).
- Force re-install of the firmware: read below.

## It's in a weird state (settings/rules corrupted, boot loops)

The escape hatch is a full erase and reflash - there is deliberately no
auto-repair logic:

### Force-reboot the chip
- press the RST button on the ESP. You do not have to disassemble the device to access this button - you can access the boot button on the ESP through the bottom of the device. Examine the photo below to determine the boot and reset button locations.

<img width="500" alt="image" src="https://github.com/user-attachments/assets/c09f99f5-8a3c-4ab9-b2dc-50e68d33e02e" />

### WebFlasher
1. Connect device to [WebFlasher](https://helga.pet)
2. If it does not connect, the bootloader may be stuck. You will need to follow standard ESP bootloader manual flashing mode.
  - Hold the BOOT button down.
  - Press and release the EN/RST button.
  - Release the BOOT button once the uploader or console indicates connection or starts flashing.

### Developer erase and flash
1. `pio run -t erase`
2. Reflash firmware + filesystem.

This resets all settings, installs fresh firmware, and deletes user rulesets.


## It never alerts

- Check the antenna is attached and snug.
- Check the ruleset you expect is enabled (`rules` on the console, or connect to the WeBflasher and scroll down to the rules area).
- If no factory rules are present, bring up the WebFlasher.
  - Under "What to flash", deselect the App firmware (unless you want to also re-install firmware)
  - Select "Rules"
  - Click Install and wait for the rules (stored as a filesystem image) to be installed on the device.
- Test the pipeline without radio: `scan inject ...` - see
  [Serial console](serial-console.md). If injection alerts but the real device
  doesn't, the rule's criteria don't match what the device actually broadcasts.

## It alerts constantly

That's usually the factory set being broader than your threat model - disable
the consumer-gadget rulesets (Sonos, Hue, JBL, ...). See
[Detection rules](rules.md).

## Battery drains fast

Battery life for this device is sub-optimal, due to design and spacial constraints. The device comes with a 400mAh LiPo battery. Below are some tips to extend your battery life.
- Foxhunt mode suspends the sleep cycle - don't leave a hunt running.
- In your Settings, under `Scanning & Performance`, set your `Scan mode` to `Power Saver`. This will extend the battery life of your device, by adjusting the sleep/scanwake windows. Your device will sleep for longer, and scan for less time. This will bring the duty cycle down and provide a longer overall runtime.
- 

## Boot log says "No core dump partition found"

Harmless ESP-IDF noise - there is intentionally no coredump partition. Ignore it.

## Screen is cracked / dim / dead

Screen replacement is an easy repair with off the shelf parts you can order from amazon, not a total loss -
see the [repair guide](hardware/repair.md).

## Charging issues

### Charging indicator

When plugged in to charge, regardless if your device is powered off/dead/asleep or if firmware is broken and the device cannot boot, a small red LED will blink and be visible towards the bottom of the device.

This is your charging indicator LED, and it will function regardless of firmware. When it stops blinking, 
| **Charging LED behavior** 	| **What it means**                                                         	|
|---------------------------	|---------------------------------------------------------------------------	|
| Blinking                  	| The battery is charging. When it turns off, the battery is fully charged. 	|
| Solid red                 	| Battery charge issue - cell may be missing or too depleted to recharge.   	|

Additionally, if the device is functional/awake, the RGB LEDs around the device will provide some charge information.

| **RGB LED behavior** 	|                           **What it means**                          	|
|:--------------------:	|:--------------------------------------------------------------------:	|
| Green pulse          	| The device is connected to power and is charging up.                 	|
| Solid green          	| The device is fully charged (red charging LED may still be flashing) 	|
| Red flash            	| The battery is dangerously low and needs recharged.                  	|
| Blue ring            	| The device has an active serial connection.                          	|

Beyond that, there are a rich set of icons and colors that will be displayed in the top right of the screen, on the status bar.

| **Status bar icon (right side)** 	|                                    **What it means**                                   	|
|:--------------------------------:	|:--------------------------------------------------------------------------------------:	|
| Yellow power icon                	| The device is connected to a dumb charger (no data) and is charging.                   	|
| Green USB icon                   	| The device is connected and has enumerated as a USB device.                            	|
| Blue keyboard icon               	| The device has an active serial connection.                                            	|
| Red exclamation icon             	| The device's power is dangerously low, or an issue with the battery has been detected. 	|
| Battery icon                     	| The icon and its color will changed based on the devices charge state.                 	|
