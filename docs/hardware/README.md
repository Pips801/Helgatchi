# Hardware overview

Helgatchi is open hardware: a two-PCB sandwich around a 3D-printed spacer,
with the PCBs themselves forming the enclosure.

<!-- TODO: exploded-view render or photo -->

## The stack

| Layer | Source | Notes |
|---|---|---|
| Front PCB | [`Hardware/Helgatchi front PCB/`](../../Hardware/Helgatchi%20front%20PCB/) (KiCad) | Screen, 6× side-firing SK68xx LEDs, buttons |
| Spacer | [`Hardware/Helgatchi Spacer/`](../../Hardware/Helgatchi%20Spacer/) (FreeCAD / STEP / 3MF) | 3D-printed frame between the boards — see [3D printing](3d-printing.md) |
| Back PCB | [`Hardware/Helgatchi back PCB/`](../../Hardware/Helgatchi%20back%20PCB/) (KiCad) | XIAO ESP32-S3 carrier, battery, RP-SMA, vibration motor |
| Test jig | [`Hardware/Helgatchi test jig/`](../../Hardware/Helgatchi%20test%20jig/) | Production testing fixture |

Dimensional drawings (edge cuts, silkscreen) are in
[`Hardware/Drawings/`](../../Hardware/Drawings/).

## Key components

<!-- TODO: proper BOM with part numbers and sources; the list below is the
     orientation version -->

- **MCU**: Seeed XIAO ESP32-S3 (8 MB flash / 8 MB PSRAM variant)
- **Display**: Waveshare 1.69" rounded IPS, 240×280, ST7789, SPI
- **LEDs**: 6× SK68xx side-firing addressable RGB
- **Haptics**: PWM-driven vibration motor
- **Battery**: 3.7 V LiPo, 400–1200 mAh (size constrained by the spacer height — 7.6 mm and 8.6 mm variants exist)
- **Antenna**: XIAO u.FL → back PCB → RP-SMA bulkhead → external 2.4 GHz antenna

## GPIO map

The authoritative pin assignment lives with the firmware:
[GPIO.md](../../Software/Helgatchi-Firmware/GPIO.md).

## Ordering PCBs

Fab outputs (gerbers/BOM/placement) are generated with the fabrication
toolkit into each PCB's `production/` folder.

<!-- TODO: exact ordering walkthrough — fab house used, layer count, thickness,
     finish, and any assembly notes (which side is hand-soldered) -->

## Building one

See [Assembly](assembly.md). Broke something? See [Repair](repair.md).
