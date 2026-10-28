/* SPDX-License-Identifier: BSD-3-Clause-Clear */
#include "device_abi.h"
static uint32_t le32(const uint8_t *p){return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
int mt2_boot_request(const uint8_t *p,uint32_t bytes,mt2_boot_view *out) {
    uint32_t ps,rs;
    if(!p || !out || bytes<MT2_BOOT_HEADER || bytes>MT2_BOOT_HEADER+2*MT2_FW_MAX_SIZE ||
       le32(p)!=MT2_BOOT_MAGIC || le32(p+4)!=MT2_ABI_VERSION)return MT2_INVALID;
    ps=le32(p+8);rs=le32(p+12);
    if(!ps || !rs || ps>MT2_FW_MAX_SIZE || rs>MT2_FW_MAX_SIZE || ps>bytes-MT2_BOOT_HEADER ||
       rs!=bytes-MT2_BOOT_HEADER-ps)return MT2_INVALID;
    out->patch=p+MT2_BOOT_HEADER;out->ram=out->patch+ps;
    out->patch_bytes=ps;out->ram_bytes=rs;return 0;
}
