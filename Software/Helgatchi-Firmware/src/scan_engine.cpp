#include "scan_engine.h"
#include "scan_service.h"
#include "settings_service.h"
#include "settings_keys.h"
#include "vendor_lookup.h"
#include "power_manager.h"      // g_power.scanDurationS() drives the phase length
#include <Arduino.h>
#include <NimBLEDevice.h>
#include <WiFi.h>
#include <esp_wifi.h>           // promiscuous mode APIs for WiFi lock-on
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <string.h>

// Per-channel dwell for the passive WiFi sweep. ≥ one ~102 ms beacon interval
// so each AP's beacon is reliably caught on every channel. Hardcoded to match
// the BLE side (interval/window also fixed constants, not settings).
static constexpr uint32_t WIFI_DWELL_MS = 120;

ScanEngine g_scan_engine;

// ---------------------------------------------------------------------------
// NimBLE callback bridge
//
// onResult fires on the BLE host task (NimBLE's internal FreeRTOS task), so
// it must NOT call ScanService::publish() directly — publish is single-
// threaded by contract. The callback instead builds a ScanResult and pushes
// it through a FreeRTOS queue; ScanEngine::tick() drains the queue on the
// main loop and calls publish() from there.
// ---------------------------------------------------------------------------

namespace {

QueueHandle_t s_queue       = nullptr;
QueueHandle_t s_admin_queue = nullptr;

// Counter pointers — set in ScanEngine::begin so the callback can update
// stats without needing g_scan_engine private accessors.
uint32_t* s_cb_count   = nullptr;
uint32_t* s_q_overflow = nullptr;

// Lock-on shared state — read by the radio-task callbacks (BLE onResult, WiFi
// promiscuous rx), written by the main loop. `s_lockon_active` gates the target
// filter; the three pointers alias ScanEngine's _lockon_* store so callbacks
// can update RSSI/last-seen without a back-reference. Set once in begin();
// s_lockon_active + s_lockon_mac are (re)set when a lock-on starts/stops.
volatile bool s_lockon_active = false;
uint8_t       s_lockon_mac[6] = {0};
volatile int8_t*   s_lockon_rssi    = nullptr;
volatile uint32_t* s_lockon_last_ms = nullptr;
volatile bool*     s_lockon_have    = nullptr;

// Record a target sighting from either radio callback. RSSI/last-seen are
// aligned scalars → atomic single writes on Xtensa; `have` publishes last.
inline void s_lockonHit(int8_t rssi) {
    if (s_lockon_rssi)    *s_lockon_rssi    = rssi;
    if (s_lockon_last_ms) *s_lockon_last_ms = millis();
    if (s_lockon_have)    *s_lockon_have    = true;
}

// WiFi promiscuous RX callback (runs on the WiFi task). Pulls RSSI straight from
// the radio metadata for any 802.11 frame whose transmitter/BSSID/receiver
// matches the target — no scan_start, so it sidesteps the back-to-back-scan
// fault that limits normal WiFi scanning to one sweep per window. Beacons alone
// give a ~100 ms refresh; associated traffic makes it faster.
void wifiPromiscRxCb(void* buf, wifi_promiscuous_pkt_type_t /*type*/) {
    if (!s_lockon_active || !buf) return;
    const wifi_promiscuous_pkt_t* p = (const wifi_promiscuous_pkt_t*)buf;
    if (p->rx_ctrl.sig_len < 22) return;                 // too short to hold addr3
    const uint8_t* h = p->payload;                        // 802.11 MAC header
    // addr1 @4 (receiver), addr2 @10 (transmitter), addr3 @16 (BSSID). Match the
    // target in any of the three so beacons, AP→client and client→AP all count.
    if (memcmp(h + 10, s_lockon_mac, 6) == 0 ||
        memcmp(h + 16, s_lockon_mac, 6) == 0 ||
        memcmp(h + 4,  s_lockon_mac, 6) == 0) {
        s_lockonHit((int8_t)p->rx_ctrl.rssi);
    }
}

// Classify a BLE address into a MacAddrType. `ble_type` is ble_addr_t.type
// (0=public, 1=random, 2=public_id, 3=random_id — the _id variants are
// controller-resolved RPAs, so even => public identity, odd => random). For
// random addresses the sub-type lives in the top two bits of the MSB.
uint8_t classifyBleMac(uint8_t ble_type, uint8_t msb) {
    if ((ble_type & 0x01) == 0) return MAC_TYPE_PUBLIC;
    switch (msb >> 6) {
        case 0b11: return MAC_TYPE_RANDOM_STATIC;
        case 0b01: return MAC_TYPE_RPA;
        case 0b00: return MAC_TYPE_NRPA;
        default:   return MAC_TYPE_RANDOM_OTHER;   // 0b10 reserved
    }
}

class HelgatchiScanCallbacks : public NimBLEScanCallbacks {
public:
    void onResult(const NimBLEAdvertisedDevice* dev) override {
        if (!s_queue || !dev) return;
        if (s_cb_count) (*s_cb_count)++;

        // Manufacturer-Specific Data — fetched once and shared by the admin
        // command channel (below) and the mfg_id extraction further down.
        const std::string mfg = dev->haveManufacturerData()
                                ? dev->getManufacturerData() : std::string();

        // Admin command channel: a signed manufacturer-data advert on the
        // internal-test company id. Cheap-prefilter here (length + company id +
        // magic), copy the raw frame to the admin queue, and let AdminService
        // authenticate + execute it on the main loop. Never falls through to a
        // ScanResult — an admin advert must not surface as a phantom device or
        // feed the rules engine.
        if (s_admin_queue && mfg.size() >= ADMIN_MSD_LEN &&
            (uint8_t)mfg[0] == (uint8_t)(ADMIN_COMPANY_ID & 0xFF) &&
            (uint8_t)mfg[1] == (uint8_t)(ADMIN_COMPANY_ID >> 8) &&
            (uint8_t)mfg[2] == ADMIN_MAGIC) {
            AdminFrame f{};
            memcpy(f.bytes, mfg.data(), ADMIN_MSD_LEN);
            f.rssi = (int8_t)dev->getRSSI();
            xQueueSend(s_admin_queue, &f, 0);   // non-blocking; drop-on-full is fine
            return;
        }

        ScanResult r{};
        r.domain        = SCAN_BLE;
        r.timestamp_ms  = millis();

        // MAC — NimBLEAddress::getBase() returns ble_addr_t with `val` in
        // little-endian (radio wire order). We store display order (MSB first).
        const NimBLEAddress addr = dev->getAddress();
        const uint8_t* native = addr.getBase()->val;
        for (int i = 0; i < 6; i++) r.mac[i] = native[5 - i];

        // Classify from the advertised address type + the MSB (r.mac[0]).
        r.mac_type = classifyBleMac(addr.getBase()->type, r.mac[0]);

        r.rssi = (int8_t)dev->getRSSI();

        // Lock-on: while hunting a BLE target, refresh its RSSI and discard
        // everything else — no queue push, so the ring / seen-map / rules engine
        // stay idle. Placed before the name/UUID parse so non-target adverts bail
        // cheaply. (wantDuplicates is enabled during lock-on so every one of the
        // target's advertisements lands here, not just the first.)
        if (s_lockon_active) {
            if (memcmp(r.mac, s_lockon_mac, 6) == 0) s_lockonHit(r.rssi);
            return;
        }

        // Adv name. NimBLE returns std::string; empty if none.
        if (dev->haveName()) {
            const std::string& nm = dev->getName();
            const size_t n = nm.size() < (sizeof(r.name) - 1)
                             ? nm.size() : (sizeof(r.name) - 1);
            memcpy(r.name, nm.data(), n);
            r.name[n] = '\0';
        }

        // Manufacturer ID — first 2 bytes of mfg-specific data, little-endian.
        if (mfg.size() >= 2) {
            r.mfg_id = (uint16_t)((uint8_t)mfg[0]) |
                       ((uint16_t)((uint8_t)mfg[1]) << 8);
        }

        // Service UUIDs — stash up to 4, normalized to 128-bit wire order.
        // NimBLEUUID::to128() mutates the UUID in place, so we need a
        // non-const copy. getValue() returns a raw byte pointer (16 bytes
        // after promotion).
        r.service_count = 0;
        const size_t n_svc = dev->getServiceUUIDCount();
        for (size_t i = 0; i < n_svc && r.service_count < 4; i++) {
            NimBLEUUID big(dev->getServiceUUID(i));
            big.to128();
            const uint8_t* bytes = big.getValue();
            if (bytes) {
                memcpy(r.service_uuids[r.service_count], bytes, 16);
                r.service_count++;
            }
        }

        // Non-blocking push. If the main loop is starved and we run out of
        // queue space, count the loss and drop. Re-firings on the same MAC
        // are common at active advertisement rates, so a missed one is rarely
        // the only chance we get.
        if (xQueueSend(s_queue, &r, 0) != pdTRUE) {
            if (s_q_overflow) (*s_q_overflow)++;
        }
    }

    // onScanEnd fires when a finite duration scan completes. We use
    // duration=0 (forever) so this normally doesn't fire — but if NimBLE
    // ever stops on its own (e.g. due to a stack error), restart so we
    // don't go silent.
    void onScanEnd(const NimBLEScanResults& /*results*/, int /*reason*/) override {
        if (g_scan_engine.bleActive()) {
            NimBLEScan* scan = NimBLEDevice::getScan();
            if (scan) scan->start(0, false);
        }
    }
};

HelgatchiScanCallbacks s_callbacks;

}  // namespace

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void ScanEngine::begin(EventBus& bus) {
    _bus = &bus;

    // Queue lives in DRAM (FreeRTOS internal). ~64 entries * sizeof(ScanResult)
    // ~= 7 KB. Acceptable; if we ever need to shrink, move ScanResult to a
    // smaller "raw" payload and reconstruct in tick().
    if (!s_queue) {
        s_queue = xQueueCreate(QUEUE_DEPTH, sizeof(ScanResult));
        if (!s_queue) return;
    }
    if (!s_admin_queue) {
        s_admin_queue = xQueueCreate(ADMIN_QUEUE_DEPTH, sizeof(AdminFrame));
    }
    _queue       = s_queue;
    _admin_queue = s_admin_queue;
    s_cb_count   = &_cb_count;
    s_q_overflow = &_q_overflow;

    // Alias the lock-on store so the radio-task callbacks can update it.
    s_lockon_rssi    = &_lockon_rssi;
    s_lockon_last_ms = &_lockon_last_ms;
    s_lockon_have    = &_lockon_have;

    bus.subscribe(CMD_SCAN_START,        this);
    bus.subscribe(CMD_SCAN_STOP,         this);
    bus.subscribe(CMD_SCAN_LOCKON_START, this);
    bus.subscribe(CMD_SCAN_LOCKON_STOP,  this);
    bus.subscribe(EV_SETTINGS_CHANGED,   this);

    // Bring the WiFi radio up now (before the first lazy NimBLE init) when WiFi
    // scanning is enabled — ARCHITECTURE.md: NimBLE must init after WiFi under
    // radio coex. NimBLE is initialized lazily at the first scan window, which
    // is dispatched after every begin() has run, so this stays ordered first.
    if (g_settings.get(SKEY_SCAN_MODE) & 2u) ensureWifi();
}

size_t ScanEngine::queueDepth() const {
    if (!_queue) return 0;
    return (size_t)uxQueueMessagesWaiting((QueueHandle_t)_queue);
}

void ScanEngine::tick() {
    // Radio phase sequencing (BLE→WiFi handoff) and WiFi result polling run
    // every tick, independent of the BLE callback queue below. Skipped while
    // hunting: lock-on owns the radio outright (continuous BLE scan, or WiFi
    // promiscuous — never esp_wifi_scan_start, which _pollWifi would call).
    if (!_lockon_active) {
        _advancePhaseIfDue();
        _pollWifi();
    }

    if (!_queue) return;
    // Drain whatever's in the queue. Cap per-tick so a flood doesn't starve
    // the rest of the loop; 32 is well above the steady-state inflow rate
    // and matches a couple of advertisements per millisecond.

    // Raw-dump mode is opted into via DEBUG_SCANNING_PERF + serial enabled.
    // Settings reads are cheap (in-memory uint32 fetch); checking once per
    // tick avoids interleaving the lookup with the queue drain.
    const bool log_raw = g_settings.getBool(SKEY_DEBUG_SERIAL_ENABLED) &&
                         g_settings.get(SKEY_DEBUG_LEVEL) == DEBUG_SCANNING_PERF;

    ScanResult r;
    for (int drained = 0; drained < 32; drained++) {
        if (xQueueReceive((QueueHandle_t)_queue, &r, 0) != pdTRUE) break;
        g_scan_service.publish(r);
        _pub_count++;

        if (log_raw) {
            const char* oui_org = vendor_for_mac(r.mac);
            const char* mfg_org = r.mfg_id ? vendor_mfg_lookup(r.mfg_id) : nullptr;
            Serial.printf("[scan] %02X:%02X:%02X:%02X:%02X:%02X "
                          "type=%-8s rssi=%-4d mfg=0x%04X svc=%u "
                          "oui=%-16.16s mfg_org=%-16.16s name=\"%s\"\n",
                          r.mac[0], r.mac[1], r.mac[2],
                          r.mac[3], r.mac[4], r.mac[5],
                          macTypeName(r.mac_type),
                          (int)r.rssi, (unsigned)r.mfg_id,
                          (unsigned)r.service_count,
                          oui_org ? oui_org : "----",
                          mfg_org ? mfg_org : "----",
                          r.name);
        }
    }
}

void ScanEngine::onEvent(const Event& e) {
    switch (e.id) {
        case CMD_SCAN_START: {
            if (_lockon_active) break;   // hunting owns the radio — ignore duty-cycle starts
            _in_scan_window = true;
            // Build the phase sequence from the enabled radios (BLE then WiFi).
            // Each phase owns the radio for the full scan duration in turn, so
            // the two radios never share airtime. PowerManager sizes the total
            // window to duration × this count.
            _seq_len = 0;
            const uint32_t mode = g_settings.get(SKEY_SCAN_MODE);
            if (mode & 1u) _scan_seq[_seq_len++] = SCAN_BLE;
            if (mode & 2u) _scan_seq[_seq_len++] = SCAN_WIFI;
            _seq_idx      = 0;
            _phase_dur_ms = (uint32_t)g_power.scanDurationS() * 1000u;
            if (_seq_len > 0) {
                _phase_start_ms = millis();
                _startDomain(_scan_seq[0]);
            }
            break;
        }
        case CMD_SCAN_STOP:
            if (_lockon_active) break;   // hunting owns the radio — ignore duty-cycle stops
            _in_scan_window = false;
            _stopBle();     // idempotent guards — stop whichever radio is live
            _stopWifi();
            _seq_len = 0;
            _seq_idx = 0;
            break;
        case CMD_SCAN_LOCKON_START:
            _lockon_domain  = e.data.lockon.domain;
            memcpy(_lockon_mac, e.data.lockon.mac, 6);
            _lockon_channel = e.data.lockon.channel;
            _startLockon();
            break;
        case CMD_SCAN_LOCKON_STOP:
            _stopLockon();
            // Normal scanning resumes when PowerManager re-opens a window
            // (it also handles CMD_SCAN_LOCKON_STOP and posts a fresh
            // CMD_SCAN_START), so nothing to restart here.
            break;
        case EV_SETTINGS_CHANGED:
            if (e.data.settings.mask & SMASK_SCAN) _applyScanSettingsChange();
            break;
        default:
            break;
    }
}

// ---------------------------------------------------------------------------
// Lock-on / foxhunt
// ---------------------------------------------------------------------------

void ScanEngine::_startLockon() {
    // Tear the normal scan down first — the two radios are still time-multiplexed
    // here, hunting just pins one of them to a single target.
    _stopBle();
    _stopWifi();
    _in_scan_window = false;
    _seq_len = 0;
    _seq_idx = 0;

    // Reset the store BEFORE arming the filter so a stale sighting can't leak in.
    _lockon_have    = false;
    _lockon_rssi    = 0;
    _lockon_last_ms = 0;
    memcpy(s_lockon_mac, _lockon_mac, 6);
    s_lockon_active = true;
    _lockon_active  = true;

    if (_lockon_domain == SCAN_WIFI) _startWifiLockon();
    else                             _startBleLockon();
}

void ScanEngine::_stopLockon() {
    if (!_lockon_active) return;
    s_lockon_active = false;              // stop the callbacks recording first
    if (_lockon_domain == SCAN_WIFI) _stopWifiLockon();
    else                             _stopBle();   // tears down the continuous scan
    _lockon_active = false;
}

// Continuous BLE scan tuned for the fastest possible RSSI refresh on one target:
// active (solicits scan responses → more packets), duplicates enabled (every
// advertisement fires onResult, not just the first), 100% duty. onResult filters
// to the target and drops everything else.
void ScanEngine::_startBleLockon() {
    if (_scan_inhibited) return;   // admin broadcast owns the radio (rare here)
    ensureBle();
    NimBLEScan* scan = NimBLEDevice::getScan();
    if (!scan) return;
    scan->setMaxResults(0);
    scan->setScanCallbacks(&s_callbacks, /*wantDuplicates*/ true);
    scan->setActiveScan(true);
    scan->setInterval(100);
    scan->setWindow(100);
    if (!scan->start(0, false)) return;
    _ble_scanning = true;
    _emitScanState(SCAN_BLE, true);
}

// Promiscuous sniff pinned to the target's channel. No esp_wifi_scan_start, so
// it avoids the back-to-back-scan fault (see _pollWifi) entirely — the rx
// callback just reads RSSI off every matching frame.
void ScanEngine::_startWifiLockon() {
    if (_scan_inhibited) return;
    ensureWifi();
    wifi_promiscuous_filter_t filt = {};
    filt.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA;
    esp_wifi_set_promiscuous_filter(&filt);
    esp_wifi_set_promiscuous_rx_cb(&wifiPromiscRxCb);
    esp_wifi_set_promiscuous(true);
    if (_lockon_channel >= 1 && _lockon_channel <= 14) {
        esp_wifi_set_channel(_lockon_channel, WIFI_SECOND_CHAN_NONE);
    }
    _wifi_promisc  = true;
    _wifi_scanning = true;            // reuse the flag so state/telemetry read "WiFi active"
    _emitScanState(SCAN_WIFI, true);
}

void ScanEngine::_stopWifiLockon() {
    if (!_wifi_promisc) return;
    esp_wifi_set_promiscuous(false);
    _wifi_promisc  = false;
    _wifi_scanning = false;
    _emitScanState(SCAN_WIFI, false);
}

// ---------------------------------------------------------------------------
// Phase sequencing
// ---------------------------------------------------------------------------

void ScanEngine::_startDomain(uint8_t domain) {
    if      (domain == SCAN_BLE)  _startBle();
    else if (domain == SCAN_WIFI) _startWifi();
}

void ScanEngine::_stopDomain(uint8_t domain) {
    if      (domain == SCAN_BLE)  _stopBle();
    else if (domain == SCAN_WIFI) _stopWifi();
}

void ScanEngine::_advancePhaseIfDue() {
    if (!_in_scan_window || _seq_len < 2) return;       // single radio: no handoff
    if (_seq_idx + 1 >= _seq_len)         return;       // already on the last phase
    if (_scan_inhibited)                  return;       // frozen while admin owns radio
    if ((millis() - _phase_start_ms) < _phase_dur_ms) return;

    // Hand the radio from the current domain to the next. Fully stopping the
    // outgoing radio before starting the next is the whole point of the
    // time-multiplex: they never contend.
    _stopDomain(_scan_seq[_seq_idx]);
    _seq_idx++;
    _phase_start_ms = millis();
    _startDomain(_scan_seq[_seq_idx]);
}

void ScanEngine::_applyScanSettingsChange() {
    // Only act mid-window — between windows the radios are already dark and the
    // next CMD_SCAN_START rebuilds the sequence fresh. A radio enabled mid-
    // window takes effect on the next window (we don't splice in a new phase).
    if (!_in_scan_window || _seq_len == 0) return;

    const uint32_t mode = g_settings.get(SKEY_SCAN_MODE);
    const uint8_t  cur  = _scan_seq[_seq_idx];
    const bool cur_enabled = (cur == SCAN_BLE) ? (mode & 1u) : (mode & 2u);
    const bool cur_running = (cur == SCAN_BLE) ? _ble_scanning : _wifi_scanning;

    if (!cur_enabled) {
        _stopDomain(cur);                       // active radio disabled — go dark
    } else if (cur_running) {
        _stopDomain(cur); _startDomain(cur);    // re-apply a changed param (active/passive)
    } else if (!_scan_inhibited) {
        _startDomain(cur);                      // re-enabled current phase — bring it back
    }
}

// ---------------------------------------------------------------------------
// BLE control
// ---------------------------------------------------------------------------

void ScanEngine::ensureBle() {
    if (_ble_initialized) return;
    // Empty device name — scanning needs none, and the admin name-beacon sets
    // its own advertised name at advertise time.
    NimBLEDevice::init("");
    // Maximum sensitivity (the receiver) — also helps admin-beacon TX range.
    NimBLEDevice::setPower(ESP_PWR_LVL_P9);
    _ble_initialized = true;
}

bool ScanEngine::popAdminFrame(AdminFrame& out) {
    if (!_admin_queue) return false;
    return xQueueReceive((QueueHandle_t)_admin_queue, &out, 0) == pdTRUE;
}

void ScanEngine::_startBle() {
    if (_ble_scanning) return;
    if (_scan_inhibited) return;   // an admin broadcast is using the radio

    ensureBle();

    NimBLEScan* scan = NimBLEDevice::getScan();
    if (!scan) return;
    // Callback-only: we consume every result via onResult→queue and keep our
    // own seen-map in PSRAM, so NimBLE's internal results vector is dead weight.
    // Its default (maxResults=0xFF) never frees it — it accumulates a heap-
    // allocated device per unique MAC on the internal heap and never clears
    // between windows. 0 = erase each device right after onResult, store nothing.
    scan->setMaxResults(0);
    scan->setScanCallbacks(&s_callbacks, /*wantDuplicates*/ false);
    // Active scan sends scan requests to solicit scan responses (more names /
    // data) at the cost of TX power and being observable; passive only listens.
    // User-controlled via SKEY_SCAN_ACTIVE.
    scan->setActiveScan(g_settings.getBool(SKEY_SCAN_ACTIVE));
    // Radio always-on within the scan window — duty cycle is governed by the
    // outer SCAN_DURATION_S / SLEEP_DURATION_S pair, not by BLE-level params.
    scan->setInterval(100);
    scan->setWindow(100);

    // duration=0 → run forever (until stop()); is_continue=false → start fresh.
    if (!scan->start(0, false)) return;
    _ble_scanning = true;
    _emitScanState(SCAN_BLE, true);
}

void ScanEngine::_stopBle() {
    if (!_ble_scanning) return;
    NimBLEScan* scan = NimBLEDevice::getScan();
    if (scan) scan->stop();
    _ble_scanning = false;
    _emitScanState(SCAN_BLE, false);
}

// ---------------------------------------------------------------------------
// WiFi control
//
// WiFi scanning runs on the main loop: an async sweep is kicked and its
// completion is polled in tick() (unlike BLE, which is callback-driven off the
// host task). Because we publish from tick(), no marshalling queue is needed —
// ScanService::publish() stays single-threaded. WiFi and BLE never run at the
// same time (see the phase sequence), so there's no radio coexistence to
// manage: each fully owns the radio for its phase.
//
// The WiFi driver is initialized once (STA, never associates) and reused across
// scan windows, one sweep per phase (we don't deinit between windows or restart
// back-to-back). WiFi's buffers live in PSRAM (the build ships
// CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y) — that's fine, there's ample PSRAM.
//
// EXPERIMENTAL / KNOWN-UNSTABLE: enabling WiFi scanning (SCAN_MODE bit 1) has
// caused intermittent memory-corruption crashes on this board that we have not
// yet root-caused (needs a source-built toolchain with heap poisoning to trace
// the wild write — see docs / the phase notes). The default SCAN_MODE is
// BLE-only, which is stable; treat WiFi as opt-in until the corruption is fixed.
// ---------------------------------------------------------------------------

void ScanEngine::ensureWifi() {
    if (_wifi_initialized) return;
    WiFi.mode(WIFI_STA);            // esp_wifi_init + start — radio up, never associates
    WiFi.disconnect(false, false);  // ensure no stray connect attempt
    _wifi_initialized = true;
}

bool ScanEngine::_kickWifiScan() {
    // Passive by default: listen for beacons only, never transmit probe
    // requests (doesn't betray our presence). SKEY_SCAN_ACTIVE flips to active
    // probing — the same toggle that governs BLE active/passive scan.
    const bool passive = !g_settings.getBool(SKEY_SCAN_ACTIVE);
    const int16_t rc = WiFi.scanNetworks(/*async*/true, /*show_hidden*/true,
                                         passive, WIFI_DWELL_MS);
    _wifi_scan_inflight = (rc == WIFI_SCAN_RUNNING);
    return _wifi_scan_inflight;
}

void ScanEngine::_startWifi() {
    if (_wifi_scanning) return;
    if (_scan_inhibited) return;   // an admin broadcast is using the radio
    ensureWifi();
    _wifi_scanning = true;
    _emitScanState(SCAN_WIFI, true);
    _kickWifiScan();
}

void ScanEngine::_stopWifi() {
    if (!_wifi_scanning) return;
    // Lightweight stop: free the result buffer and mark the radio idle. The WiFi
    // driver stays initialized (no deinit — esp_wifi_deinit faulted freeing a
    // PSRAM buffer, and with buffers now internal there's no need to tear down).
    // A mid-sweep scan simply finishes into a buffer we ignore; the next phase
    // starts fresh via _startWifi.
    WiFi.scanDelete();
    _wifi_scanning      = false;
    _wifi_scan_inflight = false;
    _emitScanState(SCAN_WIFI, false);
}

void ScanEngine::_pollWifi() {
    if (!_wifi_scanning || _scan_inhibited) return;

    // ONE sweep per WiFi phase — we never re-kick within a live WiFi session.
    // Restarting a scan back-to-back corrupts memory on this S3+PSRAM build
    // (the first sweep always works; the second esp_wifi_scan_start faults —
    // spinlock assert or scheduler TCB corruption), and adding an inter-scan
    // delay made it worse, not better. So the phase does a single all-channel
    // sweep; the next fresh sweep is the next scan window's WiFi phase, after a
    // full stop + the between-window gap (never a rapid restart).
    if (!_wifi_scan_inflight) return;   // sweep already consumed this phase

    const int n = WiFi.scanComplete();
    if (n == WIFI_SCAN_RUNNING) return;   // -1: still sweeping this channel set

    if (n >= 0) {
        const bool log_raw = g_settings.getBool(SKEY_DEBUG_SERIAL_ENABLED) &&
                             g_settings.get(SKEY_DEBUG_LEVEL) == DEBUG_SCANNING_PERF;
        for (int i = 0; i < n; i++) {
            ScanResult r{};
            r.domain       = SCAN_WIFI;
            r.mac_type     = MAC_TYPE_UNKNOWN;   // WiFi BSSIDs aren't BLE-classified
            r.timestamp_ms = millis();

            const uint8_t* bssid = WiFi.BSSID(i);   // 6 bytes, display order (MSB first)
            if (bssid) memcpy(r.mac, bssid, 6);
            r.rssi    = (int8_t)WiFi.RSSI(i);
            r.channel = (uint8_t)WiFi.channel(i);   // so WiFi lock-on can pin promiscuous to this channel

            const String ssid = WiFi.SSID(i);       // empty string for hidden APs
            strncpy(r.name, ssid.c_str(), sizeof(r.name) - 1);
            r.name[sizeof(r.name) - 1] = '\0';

            // On the main loop → publish() is single-threaded, no queue needed.
            g_scan_service.publish(r);
            _pub_count++;
            _wifi_result_count++;

            if (log_raw) {
                const char* oui_org = vendor_for_mac(r.mac);
                Serial.printf("[scan] %02X:%02X:%02X:%02X:%02X:%02X "
                              "wifi     rssi=%-4d ch=%-2ld "
                              "oui=%-16.16s ssid=\"%s\"\n",
                              r.mac[0], r.mac[1], r.mac[2],
                              r.mac[3], r.mac[4], r.mac[5],
                              (int)r.rssi, (long)WiFi.channel(i),
                              oui_org ? oui_org : "----", r.name);
            }
        }
        WiFi.scanDelete();
        _wifi_scan_count++;
    } else {
        WiFi.scanDelete();   // WIFI_SCAN_FAILED — free any partial result buffer
    }

    // Sweep consumed. Do NOT re-kick — the radio idle-listens until the phase
    // ends (CMD_SCAN_STOP → _stopWifi). wifiBusy() now reads false so a sleep
    // isn't blocked.
    _wifi_scan_inflight = false;
}

void ScanEngine::setScanInhibited(bool inhibit) {
    if (_scan_inhibited == inhibit) return;
    _scan_inhibited = inhibit;
    if (inhibit) {
        // Free the radio for admin advertising — stop whichever radio is live,
        // including a lock-on scan.
        _stopBle();
        _stopWifi();
        _stopWifiLockon();
    } else if (_lockon_active) {
        // Resume the hunt's dedicated scan (the phase machine is idle here).
        if (_lockon_domain == SCAN_WIFI) _startWifiLockon();
        else                             _startBleLockon();
    } else if (_in_scan_window && _seq_len > 0) {
        // Resume the radio for the phase we're currently in.
        _startDomain(_scan_seq[_seq_idx]);
    }
}

void ScanEngine::_emitScanState(uint8_t domain, bool active) {
    if (!_bus) return;
    EventPayload p{};
    p.scan_state.domain = domain;
    p.scan_state.active = active ? 1 : 0;
    _bus->post(EV_SCAN_STATE_CHANGED, p);
}
