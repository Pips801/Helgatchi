#pragma once
#include "admin_types.h"
#include <stdint.h>
#include <stddef.h>

// Build a signed admin command frame into `out` (ADMIN_MSD_LEN bytes: company
// id + header + params + HMAC). Returns the length written.
size_t adminBuildFrame(AdminCmd cmd, uint8_t param1, uint16_t param2_secs,
                       uint8_t* out);

// Parse + authenticate a received MSD frame. Returns true and fills the out
// params only if length / company id / magic / version match AND the HMAC
// validates. Any failure returns false and leaves the out params untouched.
bool adminParseFrame(const uint8_t* msd, size_t len,
                     AdminCmd* cmd, uint8_t* param1, uint16_t* param2_secs);
