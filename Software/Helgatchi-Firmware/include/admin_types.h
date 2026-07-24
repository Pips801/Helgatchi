#pragma once
#include <stdint.h>

// ---------------------------------------------------------------------------
// Admin-mode BLE wire protocol — shared contract.
//
// Transport: BLE legacy advertising, Manufacturer-Specific Data, non-connectable.
// The sender (AdminService) builds the frame; the receiver path fills the raw
// bytes on the NimBLE host task (ScanEngine) and AdminService interprets them on
// the main loop. Both sides share these constants so they can never drift.
//
// No replay protection by design: commands are idempotent, self-expiring
// state-sets, so a re-received (or replayed) frame is benign. The receive-side
// duration clamp (ADMIN_MAX_EFFECT_SECS) bounds how long any single frame can
// pin a state — sustained states are re-broadcast, not truly infinite.
// ---------------------------------------------------------------------------

// BT SIG "no company / internal-test" id. We share it with other testers, so a
// magic byte + HMAC distinguish real admin frames.
static constexpr uint16_t ADMIN_COMPANY_ID = 0xFFFF;
static constexpr uint8_t  ADMIN_MAGIC      = 0x48;   // 'H'
static constexpr uint8_t  ADMIN_PROTO_VER  = 1;

// MSD layout (little-endian multi-byte fields). Total = ADMIN_MSD_LEN.
//   [0..1]  company id (LE)        routing only, NOT signed
//   [2]     magic                  signed
//   [3]     version                signed
//   [4]     command (AdminCmd)     signed
//   [5]     param1                 signed  (message index / LedPatternId)
//   [6..7]  param2 = duration secs signed  (LE u16; ADMIN_DUR_FOREVER sentinel)
//   [8..23] HMAC-SHA256 over bytes [2..7], truncated to 16 bytes
static constexpr uint8_t ADMIN_MSD_LEN    = 24;
static constexpr uint8_t ADMIN_SIGNED_OFF = 2;   // first HMAC-covered byte
static constexpr uint8_t ADMIN_SIGNED_LEN = 6;   // bytes [2..7]
static constexpr uint8_t ADMIN_HMAC_OFF   = 8;
static constexpr uint8_t ADMIN_HMAC_LEN   = 16;

// Duration handling: FOREVER and anything over the cap collapse to the cap on
// receive; 0 also collapses to the cap (avoids colliding with party's "0 =
// default 20 s" and LED's "0 = until cleared").
static constexpr uint16_t ADMIN_DUR_FOREVER    = 0xFFFF;
static constexpr uint16_t ADMIN_MAX_EFFECT_SECS = 300;   // 5 min

enum AdminCmd : uint8_t {
    ADMIN_CMD_PARTY_START = 0,   // param2 = duration secs
    ADMIN_CMD_PARTY_STOP  = 1,
    ADMIN_CMD_MESSAGE     = 2,   // param1 = message index, param2 = duration
    ADMIN_CMD_LED         = 3,   // param1 = LedPatternId,  param2 = duration
    ADMIN_CMD_BEACON      = 4,   // param2 = duration
    ADMIN_CMD_STOP_ALL    = 5,
    ADMIN_CMD_COUNT
};
// Order MUST match the EEZ admin_command_dropdown options (screens.c):
//   "Start party mode / Stop party mode / Message / LED pattern / Beacon / Stop all"
// so a dropdown index casts straight to AdminCmd.

// Raw frame handed from the NimBLE host task to the main loop — no
// interpretation, just the bytes (POD, trivially copyable for the FreeRTOS queue).
struct AdminFrame {
    uint8_t bytes[ADMIN_MSD_LEN];
    int8_t  rssi;
};
