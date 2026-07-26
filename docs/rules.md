# Detection rules

What the Helgatchi hunts is defined entirely by JSON **rulesets** — nothing is
hardcoded in firmware.

## Concepts

- A **ruleset** is one JSON file: a list of match entries (**rules**) plus the
  alert behavior (title, LED pattern, vibration pattern) used when any of them
  fires.
- Rules can match on MAC/OUI, manufacturer ID, vendor organization name,
  device name / SSID, BLE service UUID, or Wi-Fi probe IE fingerprint.
  Name/SSID/vendor fields support a small regex dialect.
- **Factory rulesets** ship with the firmware (`/rules/factory/`) and are
  read-only — you can enable/disable them, and that choice survives updates.
- **User rulesets** (`/rules/user/`) are yours: created over the serial
  console, saved automatically.

## Tuning the factory set

Not every ruleset fits every threat model — with all of them on, a walk
through a big-box store is a vibration festival (Sonos, Hue, JBL…).

<!-- TODO: recommended starting profiles, e.g. "surveillance only" vs
     "trackers only" vs "everything"; how to toggle from the Settings screen
     vs `rule enable/disable` on the console -->

## Writing your own

Minimal example — alert on any device advertising a name containing "flock":

```json
{
  "name": "my_flock_rule",
  "title": "Flock nearby",
  "type": "ble",
  "criteria": [
    { "name": [".*flock.*"] }
  ]
}
```

The **full reference** — every field, the pattern/regex language, vendor-name
matching, worked examples, deployment, and testing with `scan inject` — lives
with the firmware:

➡️ **[Writing rulesets — full reference](../Software/Helgatchi-Firmware/docs/WRITING_RULES.md)**

## Contributing rulesets

Found a device worth detecting? Factory rulesets live in
[`Software/Helgatchi-Firmware/data/rules/factory/`](../Software/Helgatchi-Firmware/data/rules/factory/) —
PRs adding well-tested rulesets are welcome.

<!-- TODO: contribution bar — what evidence a new factory ruleset needs
     (captures, OUI sources, false-positive testing) -->
