#pragma once
#include <stdint.h>
#include <stddef.h>

// ---------------------------------------------------------------------------
// Admin-mode crypto helpers (mbedTLS, bundled with the ESP32 framework).
//
// The baked secrets live in the build-generated include/admin_secret.h, which
// is included ONLY by admin_crypto.cpp — no other translation unit ever sees
// the raw key material.
// ---------------------------------------------------------------------------

// Recompute HMAC-SHA256 over `signed_region` and constant-time compare its
// first ADMIN_HMAC_LEN bytes against `tag`. Fails closed on any mbedTLS error.
bool adminVerifyHmac(const uint8_t* signed_region, size_t len, const uint8_t* tag);

// Compute the truncated HMAC tag for a frame we're about to send. On any
// mbedTLS error the output is zeroed (which will fail verification downstream).
void adminComputeHmac(const uint8_t* signed_region, size_t len, uint8_t* out_tag);

// PBKDF2-verify a serial-entered password against the baked digest. `pw` must
// already be trimmed of surrounding whitespace (SerialConsole does this) to
// match the build-time normalization. Fails closed.
bool adminCheckPassword(const char* pw);

// True if this firmware was built with dev-default secrets (ALLOW_DEV_ADMIN_SECRET).
bool adminSecretIsDefault();

// A stable, non-secret-leaking fingerprint of the baked HMAC key (first 4 bytes
// of its SHA-256). Used to invalidate a persisted unlock when the firmware is
// reflashed with a different secret (NVS survives a firmware-only flash).
uint32_t adminSecretFingerprint();
