# Writing rulesets

The authoritative guide to Helgatchi's rules engine: how rulesets and rules
work, every field, the pattern/regex language, worked examples, how to get a
ruleset onto the device, and how to test it. If you're creating or editing a
ruleset, this is the page to read.

**Terminology.** A **ruleset** is a single JSON file. It holds a collection of
**rules** plus the alert behaviour (title, LED, vibration, type, …) to use when
the ruleset fires. Each **rule** is one match entry — an object in the file's
`criteria` array. A ruleset fires when **any** of its rules matches a nearby
device; it then raises a single alert configured by that ruleset. Because
behaviour is per-ruleset, **don't duplicate the same rule across multiple
rulesets** — a device that matches two rulesets raises two alerts.

## Contents

1. [Quick reference](#1-quick-reference)
2. [How rulesets & rules work](#2-how-rulesets--rules-work)
3. [Ruleset anatomy — the top-level fields](#3-ruleset-anatomy--the-top-level-fields)
4. [Rules — the match entries](#4-rules--the-match-entries)
5. [Patterns & regex](#5-patterns--regex)
6. [Vendor-name matching](#6-vendor-name-matching)
7. [Worked examples](#7-worked-examples)
8. [A full annotated ruleset](#8-a-full-annotated-ruleset)
9. [Deploying rulesets](#9-deploying-rulesets)
10. [Console command reference](#10-console-command-reference)
11. [Testing & validating your ruleset](#11-testing--validating-your-ruleset)
12. [Migration note](#12-migration-note)
13. [Gotchas checklist](#13-gotchas-checklist)
14. [Internals (for engine contributors)](#14-internals-for-engine-contributors)

---

## 1. Quick reference

A ruleset is one JSON file. Minimal shape:

```json
{
  "name": "flock_safety",
  "title": "Flock nearby",
  "type": "ble",
  "criteria": [
    { "oui": ["B4:1E:52"] },
    { "name": [".*flock.*", ".*alpr.*"] }
  ]
}
```

**Ruleset fields** (top level)

| Field      | Required | Value |
|------------|----------|-------|
| `name`     | yes | unique id, ≤ 55 chars. For **user** rulesets it is also the on-disk filename (`/rules/user/<name>.json`), so keep it path-safe — `[a-z0-9_]`. Factory rulesets are read-only (never written back) and may use display-style names, including spaces (e.g. `"Flock Safety"`). |
| `title`    | no  | text on the alert card (defaults to `name`) |
| `type`     | no  | `ble` \| `wifi` \| `sys` \| `batt` \| `auto` (default: infer) |
| `action`   | no  | `alert` (default) \| `party` |
| `vibe`     | no  | haptic pattern name (`vibe list` on the console) |
| `led`      | no  | LED pattern name (`led list` on the console) |
| `tag`      | yes | array of category tags (e.g. `["tactical", "camera"]`) |
| `criteria` | yes | the ruleset's rules — an array of match entries (below) |

**Rule fields** (each entry in `criteria`)

| Field     | Matches                          | Value |
|-----------|----------------------------------|-------|
| `oui`     | leading MAC bytes (24–48 bit)    | `"B4:1E:52"`, `"70:B3:D5:1A:2"` |
| `mac`     | full 6-byte MAC                  | `"12:34:56:78:9A:BC"` |
| `mfg`     | BT SIG company id                | `"0x05D2"` |
| `service` | advertised BLE service UUID      | `"180F"`, `"0x180F"`, or full 128-bit |
| `name`    | BLE name / Wi-Fi SSID            | **pattern** |
| `ssid`    | name, Wi-Fi only                 | **pattern** |
| `oui_org` | IEEE vendor name for the OUI     | **pattern** (matched at runtime) |
| `mfg_org` | BT SIG company name              | **pattern** (matched at runtime) |
| `ie_sig`  | Wi-Fi 802.11 IE fingerprint, Wi-Fi only | **pattern** (see below) |

`ie_sig` matches the ordered information-element fingerprint the WiFi sniffer
builds for each captured beacon / probe request: the IE tag numbers in order,
SSID (tag 0) omitted, joined by `;`, with vendor IEs (tag 221) expanded to
`221:<first up-to-8 payload bytes in lowercase hex>`. Example (a Flock camera's
station-mode probe request):
`2;12;127;221:506f9a16030103;45;191;221:0050f208000000`. Use `;` — not `,` —
between tags (comma is the criterion value separator). This fingerprint identifies
a device by *how* it probes, independent of its (often shared or randomized) MAC —
it's how the Flock ruleset tells a real camera from any other Espressif/Liteon
device. To capture one from a real device, enable WiFi scanning and read the `ie=`
field off the `scan` debug log (`debug level scan`).

**Pattern cheat sheet** (case-insensitive, matches the *whole* value)

| Intent      | Pattern     |
|-------------|-------------|
| equals      | `flock`     |
| contains    | `.*flock.*` |
| starts with | `flock.*`   |
| ends with   | `.*cam`     |
| regex       | `BWL\d-\d*` (in JSON: `"BWL\\d-\\d*"`) |

---

## 2. How rulesets & rules work

The scan engine publishes every BLE/Wi-Fi sighting into a ring buffer. The rules
service drains that ring and tests each sighting against the rules of every
**enabled** ruleset. When any rule in a ruleset matches, the ruleset fires its
action (raise an alert, or start party mode) using its configured behaviour.

Rulesets load **disabled** — shipped factory rulesets and anything flashed later
arrive silent until the user enables them (rules screen, `rule enable <name>`,
or the upcoming tag bulk-toggles). The exception: rulesets deliberately created
on-device (`rule create` / `rule save`) start enabled. Enabled-state lives in an
NVS overlay, not in the ruleset files, so it survives an FS reflash; a factory
reset or full erase returns every ruleset to disabled.

- **Behaviour is per-ruleset.** `title`, `led`, `vibe`, `type` and `action` are
  set once on the ruleset and apply to whatever its rules match.
- **Within a ruleset, everything is OR.** Any one rule matching fires the
  ruleset, and any one value inside a rule counts as a hit. There is **no AND**
  across the rules of a ruleset today — every rule is OR'd.
- **Don't duplicate a rule across rulesets.** Each ruleset that matches a device
  fires independently, so overlapping match logic in two rulesets means two
  alerts for the same device. Put a given match in exactly one ruleset.
- **One fire per (ruleset, sighting).** Matching stops at the first rule that
  hits in a ruleset.
- **Dedup is per (ruleset, MAC).** A device that keeps advertising refreshes the
  ruleset's existing alert ("last seen") instead of stacking duplicates.
- **`type` categorises the alert; it does not gate matching.** A `name` rule is
  tested against both BLE names and Wi-Fi SSIDs. Only `ssid` is Wi-Fi-gated.
- **Factory beats user.** On a `name` collision the read-only factory ruleset
  wins.
- **Enable/disable is sticky.** Disabling a ruleset is stored in NVS and survives
  a reboot and even a filesystem reflash.

---

## 3. Ruleset anatomy — the top-level fields

These configure the ruleset's identity and the alert it raises.

### `name` (required)

The unique id and the filename (`<name>.json`). Lowercase letters, digits and
underscores only; ≤ 55 characters. Cannot be changed after creation via the
editor (it's the identity).

### `title`

The human string shown on the alert card. Defaults to `name`. May contain
spaces and punctuation. (Over the serial console an underscore becomes a
space — see §10.)

### `type`

The alert category, one of:

| Value | Meaning |
|-------|---------|
| `ble`  | Bluetooth device |
| `wifi` | Wi-Fi device |
| `sys`  | system alert |
| `batt` | battery alert |
| `auto` | infer from the sighting's radio (default if omitted) |

This only labels the resulting alert; it does **not** restrict which sightings
the ruleset can match.

### `action`

- `alert` (default) — raise/refresh an alert card with the ruleset's `title`,
  `vibe` and `led`.
- `party` — trigger party mode (rainbow LEDs, haptics, animation); no alert
  card. Used by `party.json`.

### `vibe` / `led`

Names of registered haptic / LED patterns. Omit for the service default. List
the available names on the console with `vibe list` and `led list`.

---

## 4. Rules — the match entries

`criteria` is the ruleset's list of rules. Each rule is a single-key object that
names one field and gives an array of values (the values within a rule are OR'd).
There are two kinds of field.

### Literal fields

| Field     | Format | Notes |
|-----------|--------|-------|
| `oui`     | `"B4:1E:52"`, `"8C:1F:64:F"`, `"70:B3:D5:1A:2"` | a leading MAC prefix, 24–48 bits: MA-L (3 octets), MA-M (28-bit / 7 nibbles), MA-S (36-bit / 9 nibbles), up to a full MAC. Colons optional; an odd trailing nibble is allowed. Use `mac` for an exact address. |
| `mac`     | `"12:34:56:78:9A:BC"` | a single exact device |
| `mfg`     | `"0x05D2"` | 16-bit BT SIG company id from the advert |
| `service` | `"180F"` / `"0x180F"` / `"0000180f-0000-1000-8000-00805f9b34fb"` | a service UUID advertised by the device; short forms are promoted onto the BLE base UUID |

### Pattern fields

| Field     | Matched against | When |
|-----------|-----------------|------|
| `name`    | BLE adv name or Wi-Fi SSID | runtime, per sighting |
| `ssid`    | the name, **only for Wi-Fi** sightings | runtime |
| `oui_org` | the IEEE vendor name of the sighting's OUI | resolved per sighting |
| `mfg_org` | the BT SIG company name of the mfg id | resolved per sighting |

`oui_org` / `mfg_org` resolve the sighting's vendor name at match time (one
cached vendor lookup per sighting) and test the pattern against it — there's no
vendor-table scan at load, so they don't slow boot. See §6.

---

## 5. Patterns & regex

### The key principle

> A pattern is **case-insensitive** and must match the **whole** value.

Full-match (anchored) is the thing people trip on: `flock` means the value *is
exactly* `flock`, not "contains flock". To match part of a value, add `.*`
wildcards yourself.

### The four everyday shapes

| Intent      | Pattern     | Matches                     | Does **not** match |
|-------------|-------------|-----------------------------|--------------------|
| equals      | `flock`     | `flock`, `FLOCK`            | `flock-cam`, `myflock` |
| contains    | `.*flock.*` | `Flock-Cam`, `myFLOCKing`   | `flo`, `lock` |
| starts with | `flock.*`   | `flock`, `flock-cam`        | `myflock` |
| ends with   | `.*cam`     | `body-cam`, `CAM`           | `camera` |

These cost nothing at runtime (they run as plain string compares — §14). Reach
for real regex only when a value has *structure* (a serial number, a suffix).

### Regex tokens

Patterns below are shown **as the matcher sees them**. In a JSON file every
backslash is doubled — `\d` is written `"\\d"` (see [JSON escaping](#writing-patterns-in-json)).

**`.` — any single character**

| Pattern | Matches            | Not |
|---------|--------------------|-----|
| `a.c`   | `abc`, `a5c`, `a-c` | `ac`, `abbc` |

**`* + ?` — repeat the preceding element** (zero-or-more, one-or-more, zero-or-one)

| Pattern   | Matches              | Not |
|-----------|----------------------|-----|
| `ab*c`    | `ac`, `abc`, `abbbc` | `abx` |
| `ab+c`    | `abc`, `abbc`        | `ac` |
| `colou?r` | `color`, `colour`    | `colouur` |

**`\d \D` — digit / non-digit**

| Pattern  | Matches         | Not |
|----------|-----------------|-----|
| `\d\d\d` | `192`, `007`    | `19`, `1a2` |
| `cam\d+` | `cam1`, `cam42` | `cam`, `camx` |
| `\D`     | `x`, `-`        | `5` |

**`\w \W` — word `[A-Za-z0-9_]` / non-word**

| Pattern | Matches          | Not |
|---------|------------------|-----|
| `\w+`   | `node_7`, `AB12` | `a-b`, `a b` |

**`\s \S` — whitespace / non-whitespace**

| Pattern    | Matches   | Not |
|------------|-----------|-----|
| `ap\sname` | `ap name` | `apname` |

**`[...]` `[^...]` — character sets & ranges** (ranges fold case: `[a-z]` also matches `B`)

| Pattern     | Matches      | Not |
|-------------|--------------|-----|
| `[bc]at`    | `bat`, `cat` | `hat` |
| `[0-9a-f]+` | `3f`, `a0b1` | `3g`, `xyz` |
| `[^_]+`     | `abc`        | `a_b` |

**`\` — escape a metacharacter to a literal** (`. * + ? [ ] ( ) { } | ^ $ \`)

| Pattern         | Matches    | Not |
|-----------------|------------|-----|
| `v\d\.\d`       | `v3.1`     | `v3x1`, `v31` |
| `10\.0\.0\.\d+` | `10.0.0.5` | `10x0x0x5` |
| `\*`            | `*`        | `x` |

**`^` `$` — anchors** — redundant (patterns are already fully anchored) but
tolerated. `^flock$` behaves exactly like `flock`.

### What's not supported (and what to do instead)

| Missing            | Instead |
|--------------------|---------|
| alternation `a\|b` | use separate array values: `["a", "b"]` |
| bounded repeat `\d{2,4}` | spell it: `\d\d`, `\d\d\d?\d?`, or loosen to `\d+` |
| capture groups `(ab)+` | flatten the pattern |
| backreferences     | not available |

Use **arrays** rather than clever single patterns — a single `name` rule can
carry several alternatives:

```json
{ "name": [".*flock.*", ".*alpr.*", "cam\\d+"] }
```

matches a name containing "flock", **or** "alpr", **or** the regex `cam\d+`.

### Writing patterns in JSON

JSON uses `\` for its own escapes, so a regex backslash must be **doubled**:

| Matcher sees | JSON file |
|--------------|-----------|
| `\d`         | `"\\d"` |
| `\w+`        | `"\\w+"` |
| `BWL\d-\d*`  | `"BWL\\d-\\d*"` |
| `v\.\d`      | `"v\\.\\d"` |

A single `\d` in a file is invalid JSON — the whole file is rejected at load
with a parse error on the console. (The console `rule add` command is *not* JSON;
there you type a single backslash.)

### Limits

Patterns are capped at **64 characters**.

---

## 6. Vendor-name matching

`oui_org` and `mfg_org` match a **pattern against the vendor/company name** — so
you can flag a manufacturer without hand-listing their prefixes. At match time
the device's vendor name is looked up (from its OUI, or its BT SIG company id)
and your pattern is tested against it.

```json
{ "oui_org": [".*flock.*"] }       // fires when the OUI's IEEE vendor name contains "flock"
{ "mfg_org": [".*motorola.*"] }    // fires when the BT SIG company name contains "motorola"
{ "oui_org": ["sierra wireless"] } // exact vendor-name match (names are suffix-stripped)
```

Keep patterns reasonably specific — an overly broad one (`.*a.*`) will match a
huge range of vendors. Vendor names have corporate suffixes stripped
("Apple, Inc." → "Apple"), so match on the brand.

---

## 7. Worked examples

Each block below is a single rule you drop into a ruleset's `criteria` array.

**Regional serial scheme** (the WatchGuard case) — `BWL9-089912`, `BWL5-012241`:

```json
{ "name": ["BWL\\d-\\d*"] }
```

**Body cam labelled `AX` + exactly six digits** (no `{6}`, so spell it):

```json
{ "name": ["AX\\d\\d\\d\\d\\d\\d"] }
```

**Printer with a random hex suffix**, e.g. `HP-3f9a`:

```json
{ "name": ["hp-[0-9a-f]+"] }
```

**Access point on the 5 GHz band** whose SSID ends `_5G`:

```json
{ "ssid": [".*_5g"] }
```

**A device family by several keywords** (contains-any):

```json
{ "name": [".*dashcam.*", ".*bodycam.*", ".*evidence.*"] }
```

**A whole vendor, by name, without listing OUIs:**

```json
{ "oui_org": [".*axon.*"], "mfg_org": [".*axon.*"] }
```

**One exact device (allowlist/party target):**

```json
{ "mac": ["12:34:56:78:9A:BC"] }
```

**A longer OUI block** — an MA-M (28-bit) or MA-S (36-bit) assignment, more
specific than the shared 24-bit prefix it sits under:

```json
{ "oui": ["70:B3:D5:1A:2"] }
```

---

## 8. A full annotated ruleset

JSON has **no comments** — this block is annotated for learning only. A valid,
copy-pasteable version follows.

```jsonc
{
  "name": "acme_surveillance",   // ruleset id — [a-z0-9_], <= 55 chars, unique
  "title": "ACME gear nearby",   // text shown on the alert card
  "vibe": "double_tap",          // haptic pattern (omit for the default; `vibe list`)
  "led": "red_blue",             // LED pattern (omit for the default; `led list`)
  "type": "ble",                 // alert category: ble | wifi | sys | batt | auto
  "action": "alert",             // alert | party
  "criteria": [                  // the ruleset's rules — ANY one matching fires it (all OR'd)

    // --- literal rules ---
    { "oui": ["00:1D:96", "B4:1E:52"] },          // match either 24-bit MAC prefix
    { "mac": ["12:34:56:78:9A:BC"] },             // one exact device
    { "mfg": ["0x05D2", "0x0008"] },              // BT SIG company id(s)
    { "service": ["180F", "0x180A",               // advertised service UUID: short...
                  "0000fe2c-0000-1000-8000-00805f9b34fb"] }, // ...or full 128-bit

    // --- pattern rules (case-insensitive, whole-value) ---
    { "name": ["ACME.*",                          // name/SSID starts with "acme"
               ".*bodycam.*",                     // ...or contains "bodycam"
               "AX\\d\\d\\d\\d\\d\\d"] },          // ...or "AX" + exactly six digits
    { "ssid": [".*_5g", "ACME-[0-9]+"] },         // Wi-Fi ONLY: ends "_5g", or "ACME-"+digits
    { "oui_org": [".*acme.*"] },                  // any OUI whose vendor name contains acme
    { "mfg_org": [".*acme.*"] }                   // any BT SIG company containing acme
  ]
}
```

The same ruleset as a valid file (comments removed, backslashes doubled):

```json
{
  "name": "acme_surveillance",
  "title": "ACME gear nearby",
  "vibe": "double_tap",
  "led": "red_blue",
  "type": "ble",
  "action": "alert",
  "criteria": [
    { "oui": ["00:1D:96", "B4:1E:52"] },
    { "mac": ["12:34:56:78:9A:BC"] },
    { "mfg": ["0x05D2", "0x0008"] },
    { "service": ["180F", "0x180A", "0000fe2c-0000-1000-8000-00805f9b34fb"] },
    { "name": ["ACME.*", ".*bodycam.*", "AX\\d\\d\\d\\d\\d\\d"] },
    { "ssid": [".*_5g", "ACME-[0-9]+"] },
    { "oui_org": [".*acme.*"] },
    { "mfg_org": [".*acme.*"] }
  ]
}
```

---

## 9. Deploying rulesets

There are two homes for rulesets, with different install paths.

### Factory rulesets (shipped in the image)

Read-only seeds under `data/rules/factory/`. To add or change one:

1. Create/edit the `.json` file in `data/rules/factory/`.
2. Syntax-check it before flashing: `python -m json.tool data/rules/factory/your_ruleset.json`
   (prints the file if valid, or points at the first syntax error). Full
   behavioural testing comes after flashing — see §11.
3. Build and flash the filesystem image:
   ```
   pio run -t buildfs      # build the LittleFS image from data/
   pio run -t uploadfs     # flash it
   ```

Factory rulesets can't be edited on-device; the firmware never rewrites them.

### User rulesets (created at runtime)

Writable, stored under `/rules/user/`, auto-persisted on every change and kept
across a firmware-only reflash. Create them either way:

- **WebFlasher** — connect, open the rules card, add a ruleset with the field +
  match-mode dropdowns, Save. It sends `rule save <json>` for you.
- **Serial console** — `rule save <json>` for a whole ruleset, or build one up
  with `rule create <name>` then `rule add <name> <field>=<values>` (§10).

A user ruleset with the same `name` as a factory ruleset is ignored (factory
wins).

### Lifecycle

- `rule reload` — wipe in-memory rulesets and re-read both directories (also
  zeroes match counters).
- `rule enable <name>` / `rule disable <name>` — sticky, stored in NVS.
- `rule delete <name>` — removes a **user** ruleset and its file.
- `pio run -t erase` — nuke everything (NVS + filesystem). Only needed to escape
  stale state; not part of normal ruleset work.

---

## 10. Console command reference

The `rule` commands manage **rulesets** (the command name predates the
ruleset/rule split); `rule add` and `rule rm` add and remove individual rules
inside a ruleset.

```
rule                          show this help
rule list                     list every ruleset with its rules
rule show <name>              ruleset details + live match count
rule create <name> [k=v ...]  new user ruleset (k=v: title= vibe= led= type= action=)
rule add <name> <f>=<v>[,<v>] add one or more rules to a ruleset (space-separated)
rule rm <name> <idx>          remove the Nth rule from a ruleset
rule delete <name>            delete a user ruleset
rule enable  <name>           enable  a ruleset (NVS overlay)
rule disable <name>           disable a ruleset (NVS overlay)
rule reload                   re-read /rules/{factory,user}; zero counters
rule stats                    match / ring-drain counters
rule dump                     all rulesets as one JSON line (used by the WebFlasher)
rule save <json>              create/replace a user ruleset from a JSON object
```

Support commands:

```
led list                      list LED pattern names for `led`
vibe list                     list haptic pattern names for `vibe`
scan inject <k=v>             push a synthetic sighting (test the match path)
scan list                     show the seen-devices map
scan clear                    wipe the seen map
```

**Console input rules** (they differ from JSON):

- `_` becomes a space in string values, so `rule add r oui_org=sierra_wireless`
  matches "Sierra Wireless". A literal underscore can't be typed — use `\w` or
  author via JSON.
- No spaces or commas *inside* a value (they separate tokens / values).
- Backslashes are single here (`name=BWL\d-\d*`), doubled in JSON files.

Example, building a ruleset by hand:

```
rule create bwl_cams type=ble title=BWL_camera
rule add   bwl_cams name=BWL\d-\d*
rule add   bwl_cams oui_org=watchguard
rule show  bwl_cams
```

---

## 11. Testing & validating your ruleset

You don't need the real target device present to test a ruleset. The console can
inject synthetic sightings and report whether the ruleset fired. Run these in the
serial console or the WebFlasher's console view.

The goal is two checks: your ruleset **loads clean**, and it **fires on what you
intend — and nothing else**.

### Step 1 — Confirm it loaded

- **Saved via WebFlasher / `rule save`:** a bad ruleset is rejected on the spot
  (the editor shows an error; `rule save` prints `{"ok":false}`). Fix and resave.
- **Flashed as a factory file:** watch the boot log. You want
  `[rules] loaded N rules` (that count is rulesets). A malformed file logs a
  parse error naming the file; an unknown/removed field logs `bad criterion
  field`. Fix and reflash.
- Either way, run `rule show <name>` and read its rules back. Patterns print as
  `name ~ "..."`. **If a rule you wrote is missing, it was rejected** — usually
  an invalid pattern or a typo'd field name.

### Step 2 — Zero the counters

```
rule reload
```

This re-reads both directories and resets every ruleset's match counter to 0, so
you can measure from a clean slate. `rule show <name>` now reports `matches: 0`.

### Step 3 — Inject a device that SHOULD match

`scan inject` pushes a fake sighting through the real engine:

```
scan inject domain=bt mac=B4:1E:52:00:00:01 name=Flock-Cam
rule show flock_safety        # expect: matches: 1
```

Two constraints on injected sightings: the `name=` value **cannot contain
spaces**, and avoid `mac=AA:BB:CC:DD:EE:FF` (it triggers the factory party
ruleset). Set the fields your rules key on — `mac`/`oui` via `mac=`, `mfg` via
`mfg=`, `name`/`ssid` via `name=`, and `oui_org`/`mfg_org` by injecting a MAC
whose vendor your pattern matches.

### Step 4 — Inject a device that should NOT match

Just as important: prove the ruleset isn't too broad.

```
scan inject domain=bt mac=12:34:56:78:9A:B0 name=unrelated
rule show flock_safety        # expect: matches: 1  (unchanged)
```

This is where anchoring bugs show up. If you wrote `flock` (equals) but expected
"contains", a name like `Flock-Cam` won't match; if you wrote `.*a.*` you'll see
it match nearly everything.

### Step 5 — See the alert fire (optional)

`scan inject` a matching device, then check the device's alert screen (or the
`alerts` console command) for a card with your `title`, `vibe` and `led`.

### Step 6 — Clean up

```
rule delete <scratch_ruleset>   # remove any throwaway rulesets you created
scan clear                      # wipe the injected sightings
```

### Worked example — validating a regex rule end to end

```
rule reload
rule create wgtest
rule add wgtest name=BWL\d-\d*
rule show wgtest                                   # the rule prints: name ~ "BWL\d-\d*"
scan inject domain=bt mac=12:34:56:78:9A:C0 name=BWL9-089912
rule show wgtest                                   # matches: 1
scan inject domain=bt mac=12:34:56:78:9A:C1 name=BWLx-1
rule show wgtest                                   # matches: 1  (no change — \d needs a digit)
scan inject domain=bt mac=12:34:56:78:9A:C2 name=xBWL9-1
rule show wgtest                                   # matches: 1  (no change — anchored)
rule delete wgtest
scan clear
```

### When it doesn't behave

| Symptom | Likely cause |
|---------|--------------|
| Ruleset missing from `rule list` | JSON parse error at load (check the boot log), or its `name` collides with a factory ruleset (factory wins) |
| A rule missing from `rule show` | invalid pattern or unknown field — that rule was rejected while the rest of the ruleset loaded |
| Never matches | bare literal used where you meant contains (add `.*…*`); or `ssid` used for a BLE device; or wrong field |
| Matches far too much | pattern too broad (e.g. `.*a.*`), or an `oui_org`/`mfg_org` pattern that expanded to a huge vendor set |
| Two alerts for one device | the same match lives in two rulesets — consolidate it into one |
| `rule add … name=…` returns `ERR` | the pattern failed validation (e.g. a leading `*`, an unterminated `[`, a trailing `\`) |

---

## 12. Migration note

The pre-regex rule fields are **removed**:

`name_equals`, `name_contains`, `ssid_equals`, `ssid_contains`,
`oui_org_equals`, `oui_org_contains`, `mfg_org_equals`, `mfg_org_contains`.

Migrate a `_contains` value to `.*value.*` and an `_equals` value to `value`.
Old field names in a stale user file are rejected at load with a
`[rules] ... bad criterion field` log line; the rest of the ruleset still loads.
Re-author the file or reflash the filesystem image. All factory rulesets already
use the new fields.

---

## 13. Gotchas checklist

- Bare literal = **equals**, not contains. Add `.*…*` for substring.
- Double backslashes in JSON (`"\\d"`); single on the console (`\d`).
- No `|` / `{m,n}` / groups — use array values or spell repeats out.
- No literal spaces or commas in console values; `_` → space on the console.
- `type` categorises the alert; it doesn't gate matching. `ssid` is Wi-Fi-only.
- Patterns ≤ 64 chars; keep `oui_org`/`mfg_org` reasonably specific (a broad one matches many vendors).
- Don't repeat a match across rulesets — you'll get one alert per matching ruleset.
- Factory rulesets are read-only on-device; edit the file and reflash the FS.

---

## 14. Internals (for engine contributors)

- **Storage.** Rulesets live in PSRAM (`RulesService`, `src/rules_service.cpp`).
  In the code a ruleset is a `Rule` and a rule is a `Criterion`; the caps are
  `MAX_RULES = 64` (rulesets) and `MAX_CRITERIA = 256` (rules per ruleset). The
  pattern kinds (`name`/`ssid`/`oui_org`/`mfg_org`) store a verbatim string plus
  a classified shape. `oui_org`/`mfg_org` are resolved at match time against the
  sighting's vendor name (one cached bsearch per sighting), not expanded at load.
- **Pattern classification.** Each pattern is classified once (`_classifyPattern`)
  into `PAT_EXACT` / `PAT_CONTAINS` / `PAT_PREFIX` / `PAT_SUFFIX` (plain
  case-insensitive string compares) or `PAT_REGEX`. Every shipped factory pattern
  is a fast-path shape, so the regex engine rarely runs and a busy area stays
  under ~1% CPU.
- **Regex engine.** `lib/re_lite/` — a small case-insensitive, full-match matcher
  (`. * + ? \d \w \s [ranges]`, anchors). Purpose-built, not a vendored library.
  Recursion/backtracking is bounded by the 64-char pattern cap.
- **Persistence.** User rulesets serialize to `/rules/user/<name>.json` on
  mutation; enabled-state is a single NVS blob listing the *enabled* ruleset
  names (rulesets load disabled — an absent blob means nothing is enabled), and
  survives an FS reflash.
- **Round-trip.** Pattern strings — including `oui_org`/`mfg_org` — are stored
  and re-serialized verbatim, so save/dump/reload preserve exactly what was
  written. (Before match-time resolution, `oui_org`/`mfg_org` used to persist as
  their expanded `oui`/`mfg` lists; they now round-trip as the pattern.)

> **Naming note.** The code, the serial `rule` commands, the JSON `criteria` key
> and the `/rules/` directories predate the ruleset/rule vocabulary and still say
> "rule" for the file. This doc uses ruleset (file) / rule (match entry); the API
> names are unchanged.

## 15. Roadmap — rules-engine v2

The engine is evolving under a locked v2 design. **Shipped so far:**

- **Regex + pattern fields** — `lib/re_lite/`, the `name`/`ssid`/`oui_org`/`mfg_org`
  pattern kinds and their fast-path shapes (§14).
- **Variable-length OUI prefixes** — 24/28/36/48-bit (MA-L / MA-M / MA-S / full
  MAC), nibble-wise (§1, `oui`).
- **Match-time vendor resolution** for `oui_org` / `mfg_org`.
- **Wi-Fi IE fingerprint** matching (`ie_sig`, §1) for probe / beacon frames.

**Not yet built:**

- **AND groups.** Every rule in a ruleset is OR'd (§2); there is no way to require
  "OUI X *and* SSID Y" as one condition. No workaround today.
- **Cross-ruleset alert de-duplication (rule priority)** — see below.

### 15.1 Cross-ruleset double-alerting (planned: rule priority / suppression)

**Problem.** Alert dedup is keyed on `<ruleset-name>:<MAC>` (built in
`RulesService::_fire`, `src/rules_service.cpp`), so it only coalesces re-fires
*within a single ruleset*. When two **different** rulesets match the same device,
each raises its own alert card. `_matchScan` evaluates every enabled ruleset
independently (first matching criterion fires that ruleset, then `break`), so
there is no cross-ruleset arbitration.

This is most visible for Flock. A Flock camera's probe request matches
`flock_safety.json` via `ie_sig` **and** `liteon_tech.json` (or
`motorola_solutions.json`) via the camera's module-vendor OUI — two or three cards
for one physical device. Those supplier-OUI rulesets are also inherently broad:
Liteon/Motorola OUIs (and `mfg 0x0008`) fire on *any* Liteon/Motorola gear, not
just Flock, and they carry the same `Tactical`/`Camera` tags as `flock_safety`, so
a tag bulk-enable switches all of them on together.

**Planned v2 fix.** Add a per-ruleset confidence/priority field. During the
`_matchScan` drain, once a high-confidence ruleset (e.g. one matching on `ie_sig`)
fires for a MAC, suppress lower-confidence OUI-only rulesets for that same MAC
within the batch. This keeps broad OUI rules as a genuine fallback — they fire
only when the precise rule doesn't — without the duplicate card.

**Interim mitigations (data-only, no code change):**

- **Retire / disable** the `(Flock) Liteon` and `(Flock) Motorola` supplier
  rulesets. `ie_sig` plus Flock's own registered OUI `B4:1E:52` (already in
  `flock_safety.json`) cover Flock cleanly; the supplier OUIs add mostly noise.
- **Or de-scope them:** rename to plain vendor rulesets and drop the
  `Tactical`/`Camera` tags, so a bulk-enable can't turn them on alongside
  `flock_safety`.

**Heavier alternative (not planned).** Re-key dedup to one card per **MAC**,
listing every ruleset that matched. This removes all cross-ruleset duplication
structurally, but reworks the alert model and the existing per-(rule, MAC) +
RTC-persistence dedup, so it is out of scope for v2.
