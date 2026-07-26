# Serial console

Plug into USB-C and open a serial terminal - or use the console built into the
[web flasher](https://pips801.github.io/Helgatchi/). Baud: 115200.

The command grammar is consistent: **singular** verbs mutate one thing
(`setting`, `alert`, `rule`), **plural** verbs list or manage many
(`settings`, `alerts`, `rules`). Multi-word values use underscores
(`title=Axon_device_nearby`).

## Command families

| Family | Examples | Purpose |
|---|---|---|
| `help` | `help` | list everything |
| `stats` | `stats` | uptime, memory, FPS, event-bus drops |
| `settings` / `setting` | `settings`, `setting <key>=<value>` | view / change settings |
| `scan` | `scan`, `scan inject domain=ble mac=... name=...` | list seen devices; synthesize a result to test rules |
| `rules` / `rule` | `rules`, `rules show <name>`, `rule create ...`, `rule add <name> mfg=0x05D2`, `rule enable/disable <name>` | manage rulesets |
| `alerts` / `alert` | `alerts`, `alert ack ...` | view / manage alerts |
| `vendor` | `vendor oui AA:BB:CC`, `vendor mfg 0x004C`, `vendor search flock` | query the on-device vendor table |
| `leds` / `led` | `led list` | LED patterns |
| `vibe` | `vibe list` | haptic patterns |
| `selftest` | `selftest` | GPIO short/load detection |
| `admin` | `admin unlock ...` | admin (crowd-control) mode - see below |

<!-- TODO: full per-command reference with arguments and examples. Consider
     generating this from the console parser rather than maintaining by hand. -->

## Testing rules without real devices

```
scan inject domain=ble mac=B4:1E:52:00:00:01 name=Flock_Test
```

injects a synthetic scan result straight into the rules engine - the fastest
way to verify a new ruleset fires. Full workflow in
[Writing rulesets](../Software/Helgatchi-Firmware/docs/WRITING_RULES.md).

## Admin mode

HMAC-signed BLE crowd control for fleets of devices (e.g. every Helgatchi in a
room reacting together). All devices obey valid frames; *sending* requires
`admin unlock` with the build's admin password.

<!-- TODO: user-facing description of what admin mode can do, and a pointer to
     the build-time secret requirements in the developer guide -->
