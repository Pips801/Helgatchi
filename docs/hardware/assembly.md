# Assembly

Build a Helgatchi from the sources in [`Hardware/`](../../Hardware/).

<!-- TODO: this is the big one - a photographed step-by-step build. Stub order
     below; expand each step with photos and gotchas. -->

## Bill of materials

<!-- TODO: full BOM table - PCBs, XIAO ESP32-S3, screen, battery (+ connector
     polarity warning), antenna + u.FL pigtail, RP-SMA bulkhead, vibration
     motor, buttons, screws/standoffs (M2? 2.5mm vs 3mm hole spacer variants
     exist), spacer print -->

## Tools

<!-- TODO: soldering iron, hot air (if any SMD is hand-placed), driver sizes -->

## Steps

1. **Order the PCBs** - fab outputs are in each board's `production/` folder.
   <!-- TODO: fab settings -->
2. **Print the spacer** - pick the right variant for your battery and screw
   size; see [3D printing](3d-printing.md).
3. **Populate the back PCB** - XIAO, battery connector, RP-SMA, motor.
   <!-- TODO: order of operations, u.FL routing -->
4. **Populate the front PCB** - screen, LEDs, buttons.
   <!-- TODO: screen FPC handling, adhesive -->
5. **Route the antenna** - XIAO u.FL -> back PCB -> RP-SMA. Torque the bulkhead
   before final assembly.
6. **First flash** - the XIAO ships blank; flash firmware + filesystem before
   closing up ([web flasher](https://pips801.github.io/Helgatchi/) or
   [from source](../developers.md)).
7. **Test** - run `selftest` on the [serial console](../serial-console.md);
   with the test jig, <!-- TODO: jig procedure -->.
8. **Sandwich** - battery in the spacer cavity, boards on, screws in.
   <!-- TODO: screw torque / thread-into-plastic warning -->

## First boot checklist

<!-- TODO: what a healthy first boot looks like - boot screen, battery
     reading sane, `stats` output, BLE + WiFi both discovering -->
