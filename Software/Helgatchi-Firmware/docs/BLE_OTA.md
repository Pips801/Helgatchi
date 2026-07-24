# BLE OTA — passive peer-to-peer firmware updates

**Status: design only. Nothing here is implemented yet.** This doc captures the
target architecture so the partition, transport, and security decisions are
settled before code lands.

Goal: an admin-mode Helgatchi passively broadcasts "I can update you." Nearby
devices notice, check the offer against their own version + policy, ask the user,
then pull the image over BLE, verify it, and (with a second confirmation) swap to
it and reboot. No servers, no cables, no central app — one seeded device
propagates to the swarm in the field.

The offerer distributes **its own running firmware** — see [Self-clone
constraint](#self-clone-constraint). It is not a general file server.

---

## Security decision — settle this first

BLE OTA from any sender is a wireless brick/backdoor vector. Two independent
layers, and the doc assumes both:

1. **Offer authenticity (who is allowed to offer).** The discovery advert is
   HMAC-signed exactly like admin frames (`AdminService` / `admin_protocol`,
   shared `HELGATCHI_HMAC_SECRET`). A receiver won't even *prompt* on an
   unsigned/forged offer. This reuses existing infra but is **symmetric** — the
   secret is on every device, so it authenticates "a legit unit is offering,"
   not "this exact image is trustworthy." Extract the secret from one unit (no
   flash encryption yet) and offers can be forged.

2. **Image authenticity (what is allowed to boot).** The real gate. The image
   carries an **asymmetric signature** (private key never on any device); the
   receiver verifies it before install, and ideally the bootloader re-verifies
   at boot. This is **Secure Boot v2** (RSA-3072 or ECDSA over SHA-256) +
   flash encryption. With it, a tampered/unsigned image is rejected regardless
   of who sent it, and peer-to-peer cloning is safe by construction. It also
   retroactively hardens the admin HMAC secret against extraction.

> **Open decision:** ship BLE OTA *only* with Secure Boot v2 enabled? Strongly
> recommended. Until then, the app-level signature check (below) is integrity +
> weak authenticity, not a real trust anchor. Enabling secure boot is a one-way
> eFuse burn and complicates dev flashing — decide before the first signed
> release, because you can't retrofit it onto already-fielded units.

MD5 (per the flow below) covers **integrity** — it detects corruption in
transit. It is not a security control. Keep it for cheap chunk/whole-image
validation; rely on the signature for trust.

---

## Partition layout change (prerequisite)

Today: single `factory` app (5 MB) + oversized `spiffs` (~3 MB, ~empty — the
vendor tables live in the app image, not the FS). OTA needs two equal app slots.
The wasted FS space pays for the second slot.

```
# OTA-capable layout (8 MB). A/B app slots + small FS.
# Name,   Type, SubType, Offset,    Size,      Flags
nvs,      data, nvs,     0x9000,    0x5000,
otadata,  data, ota,     0xe000,    0x2000,
ota_0,    app,  ota_0,   0x10000,   0x3A0000,   # 3.625 MB
ota_1,    app,  ota_1,   0x3B0000,  0x3A0000,   # 3.625 MB
spiffs,   data, spiffs,  0x750000,  0xB0000,    # 704 KB
```

- Each slot **3.625 MB** vs the current ~3.32 MB image → ~480 KB headroom.
  Watch it: the app is actively growing, and an image that exceeds slot size
  can't OTA (fails safe — no brick — but forces a serial reflash + repartition).
- No `factory` partition: pure A/B. Patient-zero's first image is serial-flashed
  to `ota_0`.
- `nvs` shrinks 28→20 KB to fit `otadata` before the 64 KB app boundary (20 KB
  is ample). To keep 28 KB instead, push `ota_0` to `0x20000` and eat a 56 KB
  alignment gap.
- **Switching tables wipes NVS** — one-time settings reset (same warning already
  in `partitions.csv`).
- Update the layout note in `docs/ARCHITECTURE.md` when this lands.

### Self-clone constraint

The offerer streams a copy of its **own active app partition**
(`esp_partition_read` from the running `ota_X`; hash via
`esp_partition_get_sha256`). It cannot store and forward an arbitrary image —
the 704 KB FS can't hold a 3.3 MB payload. Consequence: an offerer only ever
distributes the firmware it is itself running. To seed a new version, one device
first gets it via serial / Web-Bluetooth, then becomes an offerer.

---

## Roles

| Role         | Who                          | BLE posture                                              |
|--------------|------------------------------|---------------------------------------------------------|
| **Offerer**  | a device in **admin mode**   | broadcasts a signed OTA-offer advert; runs a connectable GATT server that serves its own active partition |
| **Receiver** | any device, OTA-enabled      | observes offers via the scan engine; connects as GATT client and pulls the image |

Multiple offerers may broadcast at once. A receiver picks one (highest offered
version; tie-break on RSSI) and ignores the rest for the duration of a transfer.

---

## End-to-end flow

```
OFFERER (admin)                         RECEIVER (OTA enabled)
──────────────                          ──────────────────────
broadcast OTA-offer advert  ───────▶    scan engine filters offer into a queue
  {type, fw_version, HMAC}              authenticate HMAC ─ forged? drop, no prompt
                                        offered_version > my_version?      no ─▶ ignore
                                        SKEY_OTA_ALLOWED enabled?          no ─▶ ignore
                                        battery ok (or charging)?          no ─▶ ignore (see note)
                                        ▼
                                        PROMPT: "Update available <version>. Accept?"
                                        user declines ─▶ back to normal, cooldown this offer
                                        user accepts
                                        ▼
                            ◀───────    connect (GATT client) to offerer address
read META char              ───────▶    {size, chunk_size, md5[16], signature, fw_version}
                                        ▼
                                        ENTER OTA-RECEIVE screen — LOCK OUT all scans /
                                        power cycle / other behavior (see PowerManager)
                                        esp_ota_begin(inactive_slot)
request chunks (windowed)   ◀──────▶    stream chunks; per-chunk CRC; esp_ota_write
  data notifications        ───────▶    progress bar advances 0 → 100%
                                        ▼
                                        whole-image MD5 == META.md5 ?   fail ─▶ abort, erase, unlock
                                        signature valid over image ?    fail ─▶ abort, erase, unlock
                                        esp_ota_end (framework validation)
                                        ▼
                                        PROMPT: "Update ready. Install & reboot?  [Install] [Discard]"
                                        discard ─▶ leave boot partition unchanged, unlock, resume
                                        install
                                        ▼
                                        esp_ota_set_boot_partition(inactive_slot)  ("swap partitions")
                                        esp_restart()
                                        ▼ (next boot)
                                        first successful boot ─▶ esp_ota_mark_app_valid_cancel_rollback()
                                        boot fails / panics    ─▶ bootloader auto-rolls back to old slot
```

**Battery note.** Gate acceptance on a safe level so a mid-flash brownout can't
corrupt the write. Proposed: **≥ 50% or charging**. The prompt is suppressed (or
shown greyed with a "needs charge" reason) below the threshold. Constant lives
with `PowerManager`.

---

## BLE transport

### 1. Discovery — the passive offer advert

Reuses the admin receive path. The offerer advertises **connectable**
manufacturer-data frames; the scan engine's NimBLE callback filters OTA frames
into a queue alongside admin frames, and an `OtaService::tick()` drains +
authenticates them (mirrors `AdminService`).

Advert payload is kept tiny (legacy 31-byte adv; use BLE 5 extended adv only if
needed):

| field        | bytes | notes                                             |
|--------------|-------|---------------------------------------------------|
| company id   | 2     | manufacturer-data header                          |
| type         | 1     | `OTA_OFFER`                                        |
| fw_version   | 3     | packed major.minor.patch                          |
| flags        | 1     | reserved (e.g. mandatory-update bit)              |
| HMAC (trunc) | 8–10  | over the above, admin secret                      |

Everything heavy (size, MD5, signature) is fetched from GATT **after** connect,
not crammed into the advert.

Offer broadcasts for a bounded window then auto-stops, exactly like admin
commands (`BROADCAST_MS`). The operator arms it from the admin menu.

### 2. Pull — GATT service on the offerer

The receiver connects as central and drives the transfer:

| characteristic | op            | payload                                                        |
|----------------|---------------|---------------------------------------------------------------|
| `META`         | read          | `{ size, chunk_size, md5[16], sig[...], fw_version }`          |
| `CONTROL`      | write         | `START`, `REQUEST(window_start, count)`, `ABORT`              |
| `DATA`         | notify        | `{ index, bytes[] }` chunks, streamed for the requested window |

Windowed request/notify (receiver ACKs each window before the next) gives flow
control without a round-trip per chunk. Per-chunk CRC catches corruption early;
whole-image MD5 + signature are the final gates. Negotiate 2M PHY + DLE + max
MTU + a short connection interval — two ESP32-S3s aren't subject to phone-stack
caps, so ~100+ KB/s (~30 s for 3.3 MB) is realistic.

The offerer serves chunks straight from its active partition
(`esp_partition_read`); no buffering the whole image in RAM.

---

## Version comparison

`FW_VERSION_STR` comes from the `v*` git release tag (`scripts/build_info.py` →
`build_info.h`, e.g. `v2.8.2`). For OTA, embed the same semver into the image's
`esp_app_desc_t.version` (set the project version at build) so it's:

- readable from any partition on-device (`esp_app_get_description` /
  `esp_ota_get_partition_description`), and
- packable into the offer advert.

The receiver compares offered semver against its own running
`esp_app_desc_t.version`; only **strictly newer** offers pass. (Ignore `-dirty`
/ build-metadata suffixes in the compare.) Optional `flags` bit for a
force/mandatory offer that bypasses the newer-than check but never the user
prompt.

---

## Settings

One new key, append-only per the NVS convention in `settings_keys.h` (bump
`SCHEMA_VERSION`):

| key                 | type | default | semantics                                             |
|---------------------|------|---------|-------------------------------------------------------|
| `SKEY_OTA_ALLOWED`  | bool | **off** | receiver participates in BLE OTA (sees offers, prompts) |

Default off: OTA is opt-in. UI toggle added in EEZ (Scanning/Power section),
wired in `settings_screen.cpp` like the other switches. Offer authenticity and
image signature are *not* user-toggleable.

---

## UI

- **Prompt 1 — offer.** Modal: offered version, current version, Accept /
  Decline. Decline arms a per-offer cooldown (like admin message dismissal) so
  the same advert doesn't re-nag every scan window.
- **OTA-receive screen.** Progress bar + spinner + version label. Shown for the
  whole connected phase; **locks out all other UI/behavior** (see below). Cancel
  button aborts the transfer.
- **Prompt 2 — install.** After download + verify: "Update ready. Install &
  reboot?" Install / Discard. Discard leaves the boot partition untouched.
- **Post-reboot.** Brief "updated to <version>" confirmation on first boot, then
  `esp_ota_mark_app_valid_cancel_rollback()`.

EEZ owns the screens/widgets; C owns dynamic labels + progress. Do not hand-edit
generated `src/UI/*`.

---

## PowerManager / scan lockout

The connected transfer must own the device — no scan windows, no duty cycle, no
deep sleep, no competing radio use. Reuse the **foxhunt lock-on** mechanism
verbatim:

- An `_ota_active` flag mirrors `_hunting`: it inhibits sleep (`_isInhibited`)
  and short-circuits the whole duty-cycle state machine in `tick()` (the same
  early-return that hunting uses).
- Entering the OTA-receive screen posts a lock-on-style command; leaving /
  finishing / aborting releases it and resumes a fresh scan window.
- The battery gate keeps the device from starting a transfer it can't finish;
  the sleep inhibit keeps it awake through the transfer.

The offerer, being in admin mode, is already broadcasting; serving GATT is
additive. Its own scanning pauses while a client is connected.

---

## Failure handling

| failure                          | behavior                                                        |
|----------------------------------|----------------------------------------------------------------|
| forged / unsigned offer          | dropped at HMAC check; never prompts                            |
| offered ≤ installed version       | ignored silently                                               |
| `SKEY_OTA_ALLOWED` off            | offers ignored                                                 |
| battery too low                   | prompt suppressed / greyed with reason                         |
| connection drops mid-transfer     | abort, `esp_ota_abort`, erase slot, unlock, resume scanning    |
| chunk CRC mismatch                | re-request window; N retries then abort                        |
| whole-image MD5 mismatch          | abort + erase (corruption)                                     |
| signature invalid                 | abort + erase (**do not install**)                             |
| user discards after verify        | boot partition unchanged; slot left for next attempt           |
| new image panics on first boot    | bootloader rolls back to the previous slot (never marked valid)|

---

## Implementation phases

1. **Partitions + plumbing.** Swap to the A/B table; add `OtaService` skeleton
   (`begin`/`tick`/`onEvent`); confirm `esp_ota_*` works with a serial-delivered
   image before any BLE.
2. **Secure boot decision + signing.** Decide Secure Boot v2; wire image signing
   into the build; implement receiver-side signature verify.
3. **Discovery.** Offer advert (offerer) + scan-engine filter + authenticate +
   version/policy/battery gate + Prompt 1 (receiver).
4. **Transport.** GATT server (offerer, serves active partition) + client pull
   (receiver) + windowed chunk protocol + CRC.
5. **Receive + verify + install.** OTA-receive screen, progress, MD5 +
   signature gates, Prompt 2, `set_boot_partition` + reboot + rollback-arm.
6. **Lockout + power.** `_ota_active` inhibit (mirror foxhunt), battery gate
   constant, `SKEY_OTA_ALLOWED` + UI toggle.

---

## Open questions

- **Secure Boot v2**: enable before first signed release? (Recommended; one-way,
  can't retrofit fielded units.)
- **Battery threshold**: 50%? charging-only? charging-OR-≥X%?
- **MD5 vs SHA-256** for the integrity field — MD5 per the flow, but SHA-256 is
  already used by `esp_partition_get_sha256`; consider reusing it and dropping
  MD5 entirely.
- **Legacy vs extended advertising** for the offer frame — start legacy (31 B)
  with meta-over-GATT; only move to BLE 5 ext-adv if the advert must self-carry
  more.
- **Mandatory updates**: honor a force flag (bypass newer-than) for security
  fixes, or always require strictly-newer?
