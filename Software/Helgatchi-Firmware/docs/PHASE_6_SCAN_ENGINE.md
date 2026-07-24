# Phase 6 — Scan engine

Goal: produce real BLE + WiFi scan results into the existing `ScanService`
ring so `RulesService` can fire on them. Everything downstream is already
plumbed and tested via `scan inject` — Phase 6 just replaces the synthetic
producer with real radios.

## Interface contract (do not change)

```cpp
// include/scan_service.h — the only producer entry point
void ScanService::publish(const ScanResult& r);
```

```cpp
// include/scan_types.h — what to fill in
struct ScanResult {
    uint8_t  domain;                // SCAN_BLE or SCAN_WIFI
    uint8_t  mac[6];
    int8_t   rssi;
    char     name[32];              // BLE adv name OR WiFi SSID, NUL-terminated, truncated
    uint16_t mfg_id;                // BT SIG company ID (BLE); 0 = none. Unused for WiFi.
    uint8_t  service_count;
    uint8_t  service_uuids[4][16];  // 128-bit, BLE wire order (LSB first); promote 16/32-bit via base UUID at publish time
    uint32_t timestamp_ms;          // millis() at the scan callback
};
```

Validation path: `scan inject domain=... mac=... ...` builds the same struct
and pushes it through the same ring. If your real-radio output matches what
`scan inject` produces for the same device, you're good.

## What already exists

- **Ring + seen map**: `ScanService` (PSRAM-backed). `publish()` is thread-
  safe-enough for the main loop (no locking yet — see "concurrency" below).
- **RulesService consumer**: drains the ring on every `tick()` via
  `g_scan_service.drain(&_ring_read_pos, ...)`. Match → alert.
- **Vendor resolution**: `vendor_lookup.h` exposes `vendor_oui_lookup(prefix)`,
  `vendor_mfg_lookup(id)`. Already used by `scan` list and rule pretty-print.
  Scan engine does NOT need to resolve names — just fill in raw MAC + mfg_id
  and downstream code does the rest.
- **PowerManager scan window orchestration**: exists, drives the wake / scan /
  sleep cycle. Today nothing emits `CMD_SCAN_START` / `CMD_SCAN_STOP` because
  there's no real scanner; PowerManager just toggles the timer cadence. When
  Phase 6 lands, those commands become live triggers.

## Settings the engine must honor

All in `include/settings_keys.h`:

| key                       | semantics                                                   |
|---------------------------|-------------------------------------------------------------|
| `SKEY_SCAN_MODE`          | bitmask: bit 0 = BLE, bit 1 = WiFi. 0 = disabled (idle)     |
| `SKEY_SCAN_DURATION_S`    | how long a scan window lasts before PowerManager stops it   |
| `SKEY_BLE_SCAN_WINDOW_MS` | NimBLE `setWindow()` parameter                              |
| `SKEY_BLE_SCAN_INTERVAL_MS`| NimBLE `setInterval()` parameter                           |
| `SKEY_WIFI_DWELL_MS`      | per-channel dwell during WiFi scan                          |
| `SKEY_WIFI_HOP_INTERVAL_MS`| time between WiFi channel hops                             |
| `SKEY_PERF_MODE`          | perf-vs-battery hint; engine can pick scan defaults from it |

Subscribe to `EV_SETTINGS_CHANGED` with mask `SMASK_SCAN` to re-apply on the
fly (or just on next scan start — whichever is simpler).

## Existing reserved event IDs to use

In `include/event_ids.h`:

- `CMD_SCAN_START` — PowerManager posts this when a scan window opens
- `CMD_SCAN_STOP`  — PowerManager posts this to end a window
- `CMD_SCAN_LOCKON_START` / `CMD_SCAN_LOCKON_STOP` — future "focus on one
  device" mode (Phase 7+); leave unhandled for now
- `EV_SCAN_STATE_CHANGED` — emit with payload
  `ScanStatePayload { uint8_t domain; uint8_t active }` on every radio
  start/stop. `domain` is `ScanDomain` (`SCAN_BLE` / `SCAN_WIFI`), `active` is
  0/1. ScanEngine emits this for BLE today (`_emitScanState`); WiFi will emit
  the same event with `SCAN_WIFI` once its radio path lands. The top-bar icons
  already consume it (DisplayService) to color the BT/WiFi glyphs per domain.

The existing `EV_OBS_*` and `EV_ENTITY_*` reservations are vestigial from
an earlier design pass. Don't use them; we may delete them in cleanup.

## Implementation outline

New files:
- `include/scan_engine.h`
- `src/scan_engine.cpp`

Skeleton:

```cpp
class ScanEngine : public IEventHandler {
public:
    void begin(EventBus& bus);
    void tick();                          // poll WiFi async results; BLE is callback-driven
    void onEvent(const Event& e) override;
private:
    EventBus* _bus = nullptr;
    bool      _ble_running  = false;
    bool      _wifi_running = false;
    uint32_t  _last_state_emit_ms = 0;

    void _startBle();
    void _stopBle();
    void _startWifi();
    void _stopWifi();
    void _applyMode(uint32_t mode_bitmask);
    void _onBleAdv(/* NimBLE callback args */);    // formats + publishes
    void _onWifiScanDone();                         // walks WiFi.SSID(i) etc.
};
extern ScanEngine g_scan_engine;
```

Wiring in `main.cpp`: `g_scan_engine.begin(g_bus);` AFTER `g_scan_service.begin()` and
`g_power.begin()` (so subscriptions land in the right order). Tick goes in
`loop()`.

### BLE (NimBLE)

Add `h2zero/NimBLE-Arduino` to `lib_deps`. Use passive scanning — less power,
no responses needed, doesn't betray our presence.

```cpp
NimBLEScan* scan = NimBLEDevice::getScan();
scan->setActiveScan(false);
scan->setInterval(g_settings.get(SKEY_BLE_SCAN_INTERVAL_MS));
scan->setWindow  (g_settings.get(SKEY_BLE_SCAN_WINDOW_MS));
scan->setCallbacks(this, /* wantDuplicates */ true);
scan->start(0, /* is_continue */ false);  // 0 duration = until stop()
```

In the callback, build `ScanResult`:
- `domain = SCAN_BLE`
- `mac` from `device.getAddress().getNative()` (note byte order — NimBLE
  returns big-endian; we want the MAC in display order, so check)
- `rssi = device.getRSSI()`
- `name` from `device.getName()` (truncate to 31 chars + null)
- `mfg_id` from `device.getManufacturerData()[0..1]` (LE) if mfg data present
- `service_uuids[]` from `device.getServiceUUIDCount()` / `device.getServiceUUID(i)`,
  promoted to 128-bit BLE wire order. **Don't publish > 4 UUIDs** — the
  `service_uuids` array is fixed at 4.
- `timestamp_ms = millis()`

Then `g_scan_service.publish(r)`.

### WiFi

ESP32 WiFi scan is "async" via `WiFi.scanNetworks(true /* async */, true /* show hidden */)`.
Poll on tick with `WiFi.scanComplete()`:

```cpp
int n = WiFi.scanComplete();
if (n >= 0) {
    for (int i = 0; i < n; i++) {
        ScanResult r{};
        r.domain = SCAN_WIFI;
        memcpy(r.mac, WiFi.BSSID(i), 6);
        r.rssi = WiFi.RSSI(i);
        strncpy(r.name, WiFi.SSID(i).c_str(), sizeof(r.name) - 1);
        r.timestamp_ms = millis();
        g_scan_service.publish(r);
    }
    WiFi.scanDelete();
    // optionally trigger next scan or wait
}
```

For channel hop control beyond what `scanNetworks` does internally, you may
need `esp_wifi_scan_start()` with explicit `wifi_scan_config_t`. Start with
`WiFi.scanNetworks` — only drop to esp_wifi APIs if dwell/hop settings need
tighter control.

### Concurrency

NimBLE callbacks fire on the BLE host task (separate from `loop()`). `publish()`
is currently not thread-safe — it writes `_write_pos` and the ring entry
without locking. For Phase 6, options:

1. **Easiest, do this first**: have the NimBLE callback enqueue minimal data
   into a FreeRTOS queue, drain it on `tick()` and publish from the main
   thread. Single-threaded publisher → no lock needed.
2. Make `ScanService::publish()` use `portMUX`/`taskENTER_CRITICAL` to serialize.

Pick (1) — keeps `ScanService` clean and is the standard ESP32 pattern.

## Test plan

**Bench**:
1. Build, flash, confirm `scan inject` still works (sanity).
2. Create a rule that matches a known nearby device (e.g. `rule create apple
   oui_org_contains=apple` — should match any AirTag/iPhone OUI).
3. Power on, walk around with an iPhone. `scan` should list iPhones with
   `Apple` in the vendor column. `alerts` should fire the rule.
4. Verify settings honored: change `SCAN_MODE` to BLE-only (1), reboot, verify
   WiFi entries don't appear.

**Edge cases to verify**:
- BLE flood (lots of advertisements/sec) doesn't overrun ring — check
  `rules stats` for `lost: N scan results dropped`.
- WiFi scan with no networks → empty result list, no crash.
- Both radios enabled simultaneously → no coex issues (NimBLE + WiFi share
  the radio, ESP32 handles arbitration but timing tightens).
- Power cycle during a scan window → next boot resumes cleanly.

## Out of scope for Phase 6

- "Lockon" mode (track one device, ignore everything else) — Phase 7+
- Mesh-bridge events (`EV_MESH_RULE_FIRED_RX`) — future networking work
- Device list screen on the UI — UI work, Phase 8+
- Scan-result enrichment beyond what `vendor_lookup` already does

## Quick links

- Producer interface: `include/scan_service.h`
- ScanResult struct: `include/scan_types.h`
- Existing test-injection path: `_cmdScan` in `src/serial_console.cpp`
  (`scan inject`)
- Rules consumer: `src/rules_service.cpp` (`tick()` → `_matchScan()`)
- Settings keys: `include/settings_keys.h`
- Existing power orchestration: `src/power_manager.cpp`
