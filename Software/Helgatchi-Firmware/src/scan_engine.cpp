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

// WiFi promiscuous sniffer channel hopping. We dwell on each of the three
// non-overlapping 2.4 GHz channels long enough to catch a beacon (~102 ms
// interval) and several of a probe-requesting station's bursts (Flock STAs emit
// at ~125 ms intervals), then hop. 300 ms per channel = a full 1/6/11 cycle
// under a second.
static constexpr uint8_t  WIFI_HOP_CHANNELS[] = {1, 6, 11};
static constexpr uint8_t  WIFI_HOP_COUNT      = sizeof(WIFI_HOP_CHANNELS);
static constexpr uint32_t WIFI_HOP_DWELL_MS   = 300;

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

// WiFi promiscuous SNIFFER state (normal discovery, distinct from lock-on). While
// s_sniff_active the sniffer callback parses management frames and enqueues a
// ScanResult per beacon / probe request. s_sniff_channel is the channel the hop
// scheduler last pinned (main loop writes, callback reads — aligned byte, atomic).
// Sniffer and lock-on are mutually exclusive: only one installs its rx callback
// and flips its flag at a time.
volatile bool    s_sniff_active  = false;
volatile uint8_t s_sniff_channel = 0;

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

// Append one hex byte (lowercase) to buf[cap], advancing *sp. Silently stops at
// capacity so the signature truncates rather than overflowing.
inline void s_sigHexByte(char* buf, int cap, int* sp, uint8_t b) {
    static const char hx[] = "0123456789abcdef";
    if (*sp + 2 > cap - 1) { *sp = cap - 1; return; }
    buf[(*sp)++] = hx[b >> 4];
    buf[(*sp)++] = hx[b & 0xF];
}

// WiFi promiscuous SNIFFER callback (runs on the WiFi task). Parses 802.11
// management frames — beacons (subtype 8) and probe requests (subtype 4) — into a
// ScanResult and marshals it through the same queue the BLE path uses, so
// ScanService::publish() stays single-threaded (drained in tick()). Builds the IE
// fingerprint string (r.ie_sig) that the CRIT_IE_SIG rule criterion matches; this
// is how a probe-only STA (e.g. a Flock camera in station mode) becomes visible —
// WiFi.scanNetworks() only ever surfaced APs. Bounds-checked throughout: sig_len
// includes the 4-byte FCS tail, which we strip before walking IEs.
void wifiSniffRxCb(void* buf, wifi_promiscuous_pkt_type_t /*type*/) {
    if (!s_sniff_active || s_lockon_active || !buf || !s_queue) return;
    const wifi_promiscuous_pkt_t* p = (const wifi_promiscuous_pkt_t*)buf;
    const int sig_len = (int)p->rx_ctrl.sig_len;
    if (sig_len < 24) return;                              // too short for a full MAC header
    const uint8_t* h = p->payload;

    // Frame control: [version:2][type:2][subtype:4]. Management = type 0.
    const uint8_t fc0     = h[0];
    const uint8_t ftype   = (uint8_t)((fc0 >> 2) & 0x3);
    const uint8_t subtype = (uint8_t)((fc0 >> 4) & 0xF);
    if (ftype != 0) return;
    uint8_t frame_kind;
    int     ie_start;                                     // offset of the first IE
    if (subtype == 8)      { frame_kind = FRAME_BEACON;    ie_start = 36; }  // 24 hdr + 12 fixed params
    else if (subtype == 4) { frame_kind = FRAME_PROBE_REQ; ie_start = 24; }  // IEs start right after hdr
    else return;

    ScanResult r{};
    r.domain       = SCAN_WIFI;
    r.mac_type     = MAC_TYPE_UNKNOWN;
    r.frame_kind   = frame_kind;
    r.rssi         = (int8_t)p->rx_ctrl.rssi;
    r.channel      = s_sniff_channel;
    r.timestamp_ms = millis();
    memcpy(r.mac, h + 10, 6);                             // addr2 = transmitter (the STA/AP itself)

    // IE body ends before the 4-byte FCS. Walk TLVs (tag, len, value), extracting
    // the SSID (tag 0) into r.name and building the tag-order fingerprint into
    // r.ie_sig. Vendor IEs (tag 221) expand to "221:<first ≤8 payload bytes hex>";
    // SSID is skipped from the signature. Entries joined by ';' so commas stay free
    // as the rule criterion-value separator.
    const int frame_end = sig_len - 4;                    // strip FCS
    const int sig_cap   = (int)sizeof(r.ie_sig);
    int sp = 0;
    for (int i = ie_start; i + 2 <= frame_end; ) {
        const uint8_t tag  = h[i];
        const uint8_t elen = h[i + 1];
        if (i + 2 + (int)elen > frame_end) break;         // declared length overruns the buffer
        const uint8_t* val = h + i + 2;
        if (tag == 0) {
            const uint8_t n = elen < (uint8_t)(sizeof(r.name) - 1) ? elen
                                                                   : (uint8_t)(sizeof(r.name) - 1);
            memcpy(r.name, val, n);
            r.name[n] = '\0';
        } else if (sp < sig_cap - 1) {
            if (sp > 0) r.ie_sig[sp++] = ';';
            if (tag == 221) {
                sp += snprintf(r.ie_sig + sp, sig_cap - sp, "221:");
                if (sp > sig_cap - 1) sp = sig_cap - 1;
                const uint8_t nb = elen < 8 ? elen : 8;
                for (uint8_t b = 0; b < nb; b++) s_sigHexByte(r.ie_sig, sig_cap, &sp, val[b]);
            } else {
                sp += snprintf(r.ie_sig + sp, sig_cap - sp, "%u", (unsigned)tag);
                if (sp > sig_cap - 1) sp = sig_cap - 1;
            }
        }
        i += 2 + (int)elen;
    }
    r.ie_sig[sp] = '\0';

    xQueueSend(s_queue, &r, 0);                           // non-blocking; drop-on-full is fine
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
    // Radio phase sequencing (BLE→WiFi handoff) and WiFi sniffer channel hopping
    // run every tick, independent of the BLE callback queue below. Skipped while
    // hunting: lock-on owns the radio outright (continuous BLE scan, or WiFi
    // promiscuous pinned to one channel — never the hopping discovery sniffer).
    if (!_lockon_active) {
        _advancePhaseIfDue();
        _pollWifiSniff();
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
        if (r.domain == SCAN_WIFI) _wifi_result_count++;

        if (log_raw && r.domain == SCAN_WIFI) {
            // WiFi promiscuous sniffer frame — show frame class + IE fingerprint
            // so a real Flock probe's ie_sig can be read off serial and compared.
            const char* oui_org = vendor_for_mac(r.mac);
            Serial.printf("[scan] %02X:%02X:%02X:%02X:%02X:%02X "
                          "wifi     rssi=%-4d ch=%-2u %-6s "
                          "oui=%-16.16s ssid=\"%s\" ie=\"%s\"\n",
                          r.mac[0], r.mac[1], r.mac[2],
                          r.mac[3], r.mac[4], r.mac[5],
                          (int)r.rssi, (unsigned)r.channel,
                          r.frame_kind == FRAME_PROBE_REQ ? "probe" :
                          r.frame_kind == FRAME_BEACON    ? "beacon" : "-",
                          oui_org ? oui_org : "----", r.name, r.ie_sig);
        } else if (log_raw) {
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

// Promiscuous sniff pinned to the target's channel — like the discovery sniffer
// but single-channel and target-filtered: the rx callback (wifiPromiscRxCb) just
// reads RSSI off every frame matching the locked-on MAC.
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
// WiFi discovery is a promiscuous management-frame sniffer, not an AP scan. The
// radio is put in promiscuous mode with a MGMT-only filter; the sniffer callback
// (wifiSniffRxCb, on the WiFi task) parses each beacon and probe request into a
// ScanResult and marshals it through the same queue the BLE path uses, so
// ScanService::publish() stays single-threaded (drained in tick()). tick() also
// hops the channel across 1/6/11.
//
// Why promiscuous instead of WiFi.scanNetworks(): scanNetworks only enumerates
// APs (beacons / probe responses), so a probe-requesting STA — e.g. a Flock
// camera running in station mode — was completely invisible. It was also flagged
// KNOWN-UNSTABLE (back-to-back esp_wifi_scan_start faulted on this S3+PSRAM
// build). Promiscuous capture is the same mechanism the lock-on path has used
// reliably; it sees beacons AND probe requests and never calls scan_start.
//
// WiFi and BLE never run at the same time (see the phase sequence), so there's no
// radio coexistence to manage. The WiFi driver is initialized once (STA, never
// associates) and reused across windows.
// ---------------------------------------------------------------------------

void ScanEngine::ensureWifi() {
    if (_wifi_initialized) return;
    WiFi.mode(WIFI_STA);            // esp_wifi_init + start — radio up, never associates
    WiFi.disconnect(false, false);  // ensure no stray connect attempt
    _wifi_initialized = true;
}

void ScanEngine::_startWifi() {
    if (_wifi_scanning) return;
    if (_scan_inhibited) return;   // an admin broadcast is using the radio
    ensureWifi();

    // Promiscuous MGMT sniffer: passive by nature (we only listen), so
    // SKEY_SCAN_ACTIVE doesn't apply to WiFi. Pin the first hop channel, then
    // tick() rotates 1/6/11.
    wifi_promiscuous_filter_t filt = {};
    filt.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT;
    esp_wifi_set_promiscuous_filter(&filt);
    esp_wifi_set_promiscuous_rx_cb(&wifiSniffRxCb);
    esp_wifi_set_promiscuous(true);
    _sniff_hop_idx = 0;
    s_sniff_channel = WIFI_HOP_CHANNELS[0];
    esp_wifi_set_channel(s_sniff_channel, WIFI_SECOND_CHAN_NONE);
    _sniff_hop_ms  = millis();
    s_sniff_active = true;
    _wifi_promisc  = true;
    _wifi_scanning = true;
    _emitScanState(SCAN_WIFI, true);
}

void ScanEngine::_stopWifi() {
    if (!_wifi_scanning) return;
    s_sniff_active = false;         // stop the callback enqueuing first
    if (_wifi_promisc) {
        esp_wifi_set_promiscuous(false);
        _wifi_promisc = false;
    }
    _wifi_scanning = false;
    _emitScanState(SCAN_WIFI, false);
}

// Channel-hop the sniffer across 1/6/11. The callback captures frames on the
// WiFi task as they arrive; this just advances the pinned channel each dwell.
void ScanEngine::_pollWifiSniff() {
    if (!_wifi_scanning || !_wifi_promisc || _scan_inhibited) return;
    if ((millis() - _sniff_hop_ms) < WIFI_HOP_DWELL_MS) return;
    _sniff_hop_idx = (uint8_t)((_sniff_hop_idx + 1) % WIFI_HOP_COUNT);
    if (_sniff_hop_idx == 0) _wifi_scan_count++;   // one full 1/6/11 cycle == a "sweep"
    s_sniff_channel = WIFI_HOP_CHANNELS[_sniff_hop_idx];
    esp_wifi_set_channel(s_sniff_channel, WIFI_SECOND_CHAN_NONE);
    _sniff_hop_ms  = millis();
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
