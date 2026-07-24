# Generates include/admin_secret.h with the admin-mode secrets baked in from
# environment variables, so they never live in source control.
#
#   HELGATCHI_HMAC_SECRET    raw frame-signing key (>= 32 bytes; must be a
#                            CSPRNG-generated random value, e.g. `openssl rand
#                            -hex 32`, NOT a memorable passphrase — a single
#                            sniffed advert is a known message/tag pair, so a
#                            weak key is brute-forceable offline).
#   HELGATCHI_ADMIN_PASSWORD the unlock password (plaintext, build machine only).
#                            The device never sees it — only its PBKDF2 digest is
#                            baked; the operator re-derives it from the password
#                            typed over serial.
#
# Missing either var HARD-FAILS the build (so a release never silently ships a
# publicly-known key), unless ALLOW_DEV_ADMIN_SECRET=1 is set — then documented
# dev defaults are baked, ADMIN_SECRET_IS_DEFAULT=1, and a compile-time #warning
# fires. Never ship a build made with the dev defaults.
#
# Password encoding contract (MUST match SerialConsole::_cmdAdmin's trim): UTF-8,
# surrounding whitespace stripped, internal whitespace preserved. A one-byte
# mismatch breaks unlock silently.
#
# Header is only rewritten when its content changes (deterministic salt), so it
# doesn't gratuitously trigger a recompile.

import os
import hashlib

Import("env")  # noqa: F821

PROJECT_DIR = env["PROJECT_DIR"]
OUT_PATH    = os.path.join(PROJECT_DIR, "include", "admin_secret.h")


def _load_dotenv(path):
    """Load KEY=VALUE lines from a .env into os.environ WITHOUT overriding
    values already set (so an explicit shell env / CI secret wins over the
    file). Supports `#` comments, optional `export `, and surrounding quotes."""
    if not os.path.isfile(path):
        return
    with open(path, "r", encoding="utf-8") as f:
        for raw in f:
            line = raw.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            if line.startswith("export "):
                line = line[len("export "):]
            key, _, val = line.partition("=")
            key = key.strip()
            val = val.strip()
            if len(val) >= 2 and val[0] == val[-1] and val[0] in ("'", '"'):
                val = val[1:-1]
            if key and key not in os.environ:
                os.environ[key] = val


# Pick up a project-root .env (gitignored) so secrets don't have to be exported
# into the shell each build. Real env vars set by the shell / CI take precedence.
_load_dotenv(os.path.join(PROJECT_DIR, ".env"))

# PBKDF2 parameters. iters is defense-in-depth ONLY: the digest AND the HMAC key
# both ship in the firmware image, so anyone who extracts one has the other —
# this just adds a token speed bump to an offline dictionary attack on the
# password. adminCheckPassword() runs synchronously on the main loop during
# `admin unlock`, so keep it low enough to stay imperceptible: 100k stalled this
# ESP32-S3 for several seconds (froze LEDs/LVGL/serial). ~2k is well under 100 ms.
# dklen = full SHA-256 output.
PBKDF2_ITERS = 2000
PBKDF2_DKLEN = 32
# Fixed per-project pepper. The salt is derived from the pepper + the (high-
# entropy) HMAC key — NOT the password — so it stays deterministic (reproducible
# header, no needless recompiles) without becoming a fast offline password
# oracle: a password-derived salt shipped in the image would let an attacker
# recover the password with one hash per guess, nullifying the iteration count.
SALT_PEPPER  = b"helgatchi-admin-v1"

# Dev defaults (only used with ALLOW_DEV_ADMIN_SECRET=1). The HMAC key is a
# fixed 32-byte value; dev-built devices form their own admin domain and cannot
# talk to release devices (different key) — which is exactly what we want.
DEV_HMAC_SECRET = "helgatchi-dev-hmac-key-do-not-ship-0001"   # 39 bytes
DEV_ADMIN_PW    = "helgatchi"


def normalize_pw(s):
    return s.strip()


def normalize_secret(s):
    return s.strip()


def c_bytes(b):
    return ", ".join("0x%02x" % x for x in b)


hmac_secret = os.environ.get("HELGATCHI_HMAC_SECRET")
admin_pw    = os.environ.get("HELGATCHI_ADMIN_PASSWORD")
allow_dev   = os.environ.get("ALLOW_DEV_ADMIN_SECRET") == "1"

is_default = 0

if hmac_secret is None or admin_pw is None:
    if not allow_dev:
        raise SystemExit(
            "[admin_secret] FATAL: HELGATCHI_HMAC_SECRET and "
            "HELGATCHI_ADMIN_PASSWORD must both be set.\n"
            "  Set them (e.g. in a .env / CI secret), or pass "
            "ALLOW_DEV_ADMIN_SECRET=1 to build with insecure dev defaults."
        )
    hmac_secret = DEV_HMAC_SECRET
    admin_pw    = DEV_ADMIN_PW
    is_default  = 1
    print("[admin_secret] WARNING: using DEV DEFAULT secrets — do NOT ship this build")

hmac_bytes = normalize_secret(hmac_secret).encode("utf-8")
pw_bytes   = normalize_pw(admin_pw).encode("utf-8")

if len(hmac_bytes) < 32:
    raise SystemExit(
        "[admin_secret] FATAL: HELGATCHI_HMAC_SECRET is %d bytes; need >= 32.\n"
        "  Generate one with `openssl rand -hex 32` (a random value, not a "
        "passphrase)." % len(hmac_bytes)
    )
if not pw_bytes:
    raise SystemExit("[admin_secret] FATAL: HELGATCHI_ADMIN_PASSWORD is empty after trimming")

salt = hashlib.sha256(SALT_PEPPER + hmac_bytes).digest()[:16]
digest = hashlib.pbkdf2_hmac("sha256", pw_bytes, salt, PBKDF2_ITERS, PBKDF2_DKLEN)

warn = ""
if is_default:
    warn = ('#warning "Admin secrets are DEV DEFAULTS '
            '(ALLOW_DEV_ADMIN_SECRET=1) — do not ship this build"\n')

contents = (
    "#pragma once\n"
    "// Auto-generated by scripts/build_admin_secret.py — do not edit, do not commit.\n"
    "// Included ONLY by src/admin_crypto.cpp; no other TU sees this key material.\n"
    f"#define ADMIN_SECRET_IS_DEFAULT {is_default}\n"
    f"{warn}"
    f"static const unsigned char ADMIN_HMAC_KEY[] = {{ {c_bytes(hmac_bytes)} }};\n"
    f"#define ADMIN_HMAC_KEY_LEN {len(hmac_bytes)}\n"
    f"static const unsigned char ADMIN_PW_SALT[] = {{ {c_bytes(salt)} }};\n"
    f"#define ADMIN_PW_SALT_LEN {len(salt)}\n"
    f"#define ADMIN_PW_ITERS {PBKDF2_ITERS}\n"
    f"#define ADMIN_PW_HASH_LEN {PBKDF2_DKLEN}\n"
    f"static const unsigned char ADMIN_PW_HASH[] = {{ {c_bytes(digest)} }};\n"
)

existing = ""
if os.path.isfile(OUT_PATH):
    try:
        with open(OUT_PATH, "r", encoding="utf-8") as f:
            existing = f.read()
    except OSError:
        pass

if existing != contents:
    os.makedirs(os.path.dirname(OUT_PATH), exist_ok=True)
    with open(OUT_PATH, "w", encoding="utf-8") as f:
        f.write(contents)
    print(f"[admin_secret] regenerated (default={is_default}, hmac_len={len(hmac_bytes)})")
else:
    print(f"[admin_secret] up to date (default={is_default})")
