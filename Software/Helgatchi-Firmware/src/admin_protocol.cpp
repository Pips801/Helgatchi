#include "admin_protocol.h"
#include "admin_crypto.h"

size_t adminBuildFrame(AdminCmd cmd, uint8_t param1, uint16_t param2_secs,
                       uint8_t* out) {
    out[0] = (uint8_t)(ADMIN_COMPANY_ID & 0xFF);
    out[1] = (uint8_t)(ADMIN_COMPANY_ID >> 8);
    out[2] = ADMIN_MAGIC;
    out[3] = ADMIN_PROTO_VER;
    out[4] = (uint8_t)cmd;
    out[5] = param1;
    out[6] = (uint8_t)(param2_secs & 0xFF);
    out[7] = (uint8_t)(param2_secs >> 8);
    adminComputeHmac(out + ADMIN_SIGNED_OFF, ADMIN_SIGNED_LEN, out + ADMIN_HMAC_OFF);
    return ADMIN_MSD_LEN;
}

bool adminParseFrame(const uint8_t* msd, size_t len,
                     AdminCmd* cmd, uint8_t* param1, uint16_t* param2_secs) {
    if (!msd || len < ADMIN_MSD_LEN) return false;
    if (msd[0] != (uint8_t)(ADMIN_COMPANY_ID & 0xFF) ||
        msd[1] != (uint8_t)(ADMIN_COMPANY_ID >> 8))   return false;
    if (msd[2] != ADMIN_MAGIC)                         return false;
    if (msd[3] != ADMIN_PROTO_VER)                     return false;
    if (!adminVerifyHmac(msd + ADMIN_SIGNED_OFF, ADMIN_SIGNED_LEN,
                         msd + ADMIN_HMAC_OFF))         return false;

    if (cmd)         *cmd         = (AdminCmd)msd[4];
    if (param1)      *param1      = msd[5];
    if (param2_secs) *param2_secs = (uint16_t)msd[6] | ((uint16_t)msd[7] << 8);
    return true;
}
