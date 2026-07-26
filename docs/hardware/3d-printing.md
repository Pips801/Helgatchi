# 3D printing

All printable parts live in
[`Hardware/Helgatchi Spacer/`](../../Hardware/Helgatchi%20Spacer/) as STEP,
3MF, and FreeCAD sources (plus DXF/PDF drawings for reference).

## Parts

### Spacer

The frame between the front and back PCBs - it sets device thickness and holds
the battery. Pick your variant:

| Choice | Options | Pick based on |
|---|---|---|
| Height | **7.6 mm** or **8.6 mm** | battery thickness |
| Antenna cutout | with / without | whether you're fitting the RP-SMA bulkhead |
| Screw holes | **2.5 mm** or **3 mm** | your screws/standoffs |

Files: `7.6mm Spacer*.step`, `8.6mm Spacer*.step`. The `Conjoined Spacer`
variants gang multiple spacers into one plate for batch printing
(`Conjoined Spacer 3x4.3mf` is a print-ready plate).

### Screen bumper

A protective lip around the screen - print this; screens are the #1 casualty.
Files: `screen bumper.step`, `screen bumper stacked.3mf` (pre-stacked plate).

## Print settings

<!-- TODO: material (PETG vs PLA?), layer height, perimeters, orientation,
     supports (none?), tolerance notes for the PCB fit and screw holes -->

## Remixing

`Spacer.FCStd` is the FreeCAD source - modify it rather than the exported
STEPs if you're changing dimensions (new battery size, different bulkhead...).
PRs with useful variants welcome.

<!-- TODO: link Printables/Thingiverse mirrors if published -->
