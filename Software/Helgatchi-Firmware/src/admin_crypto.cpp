#include "admin_crypto.h"
#include "admin_types.h"    // ADMIN_HMAC_LEN
#include "admin_secret.h"   // GENERATED — the ONLY includer of the key material
#include <string.h>
#include "mbedtls/md.h"
#include "mbedtls/pkcs5.h"

namespace {

// Constant-time equality. Defense-in-depth only — there is no timing oracle
// over a one-way BLE broadcast — but cheap and correct. The volatile
// accumulator stops the compiler short-circuiting the loop.
bool ctEqual(const uint8_t* a, const uint8_t* b, size_t n) {
    volatile uint8_t diff = 0;
    for (size_t i = 0; i < n; i++) diff |= (uint8_t)(a[i] ^ b[i]);
    return diff == 0;
}

}  // namespace

void adminComputeHmac(const uint8_t* signed_region, size_t len, uint8_t* out_tag) {
    uint8_t full[32];
    const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!info ||
        mbedtls_md_hmac(info, ADMIN_HMAC_KEY, ADMIN_HMAC_KEY_LEN,
                        signed_region, len, full) != 0) {
        memset(out_tag, 0, ADMIN_HMAC_LEN);   // fail closed
        return;
    }
    memcpy(out_tag, full, ADMIN_HMAC_LEN);
}

bool adminVerifyHmac(const uint8_t* signed_region, size_t len, const uint8_t* tag) {
    uint8_t full[32];
    const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!info) return false;
    if (mbedtls_md_hmac(info, ADMIN_HMAC_KEY, ADMIN_HMAC_KEY_LEN,
                        signed_region, len, full) != 0) return false;
    return ctEqual(full, tag, ADMIN_HMAC_LEN);
}

bool adminCheckPassword(const char* pw) {
    if (!pw) return false;

    const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!info) return false;

    uint8_t derived[ADMIN_PW_HASH_LEN];
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    // hmac=1 is REQUIRED: PBKDF2 uses HMAC-SHA256 as its PRF. Omitting it
    // silently produces plain-hash output that won't match hashlib.pbkdf2_hmac.
    int rc = mbedtls_md_setup(&ctx, info, 1 /* hmac */);
    if (rc == 0) {
        rc = mbedtls_pkcs5_pbkdf2_hmac(&ctx,
                (const unsigned char*)pw, strlen(pw),
                ADMIN_PW_SALT, ADMIN_PW_SALT_LEN,
                ADMIN_PW_ITERS, ADMIN_PW_HASH_LEN, derived);
    }
    mbedtls_md_free(&ctx);
    if (rc != 0) return false;   // fail closed
    return ctEqual(derived, ADMIN_PW_HASH, ADMIN_PW_HASH_LEN);
}

bool adminSecretIsDefault() { return ADMIN_SECRET_IS_DEFAULT != 0; }

uint32_t adminSecretFingerprint() {
    uint8_t h[32];
    // Use the generic md API (returns int in this mbedTLS 2.28) rather than
    // mbedtls_sha256(), whose non-_ret form here is a deprecated void.
    const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!info || mbedtls_md(info, ADMIN_HMAC_KEY, ADMIN_HMAC_KEY_LEN, h) != 0)
        return 0;
    return ((uint32_t)h[0] << 24) | ((uint32_t)h[1] << 16) |
           ((uint32_t)h[2] << 8)  |  (uint32_t)h[3];
}
