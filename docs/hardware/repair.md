# Repair

Helgatchi is built to be opened. Every part is replaceable, and the CAD for
the printed parts is in the repo.

## Opening the device

<!-- TODO: screw locations/sizes, opening order, what NOT to yank (battery
     leads, u.FL pigtail, screen FPC) -->

## Replacing a broken screen

The most common repair. The screen is a Waveshare 1.69" rounded IPS (240x280,
ST7789) - a standard, cheap module.

<!-- TODO: exact part/link, adhesive removal, FPC connector handling,
     reassembly. Also: print a screen bumper (see 3D printing) to prevent the
     next one. -->

**Prevention:** print the [screen bumper](3d-printing.md) - it exists because
of exactly this failure.

## Replacing the battery

Any 3.7 V LiPo that fits the spacer cavity works (400-1200 mAh).

<!-- TODO: connector type + polarity warning (off-the-shelf LiPo pigtails are
     frequently reversed!), which spacer height fits which battery thickness -->

## Buttons

<!-- TODO: switch part number, replacement notes -->

## Antenna / no range

If range collapses, work the RF chain from the outside in: antenna snug on the
RP-SMA -> bulkhead tight -> u.FL pigtail seated on the back PCB -> u.FL seated on
the XIAO. u.FL connectors are rated for very few mating cycles; if one is
loose, replace the pigtail.

## Vibration motor

<!-- TODO: part, replacement -->

## Nothing physical is wrong but it acts haunted

That's a firmware/filesystem state problem, not hardware - see
[Troubleshooting](../troubleshooting.md) for the erase-and-reflash procedure.
