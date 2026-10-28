/* SPDX-License-Identifier: BSD-3-Clause-Clear */
#ifndef AIRWINDOWS_MT7922_NIC_CAPS_H
#define AIRWINDOWS_MT7922_NIC_CAPS_H
#include "mcu.h"
#define MT2_NIC_COMMAND (MT2_MCU_CE|0x8au)
#define MT2_CAP_MAC 1u
#define MT2_CAP_PHY 2u
#define MT2_CAP_6GHZ 4u
#define MT2_CAP_CHIP 8u
typedef struct mt2_nic_caps {
    uint32_t present,bands,chainmask,chip_low,chip_high;
    uint8_t mac[6],nss,max_bandwidth,ht,vht,he,hw_path;
} mt2_nic_caps;
/* Connac2 payload: element count LE16 + reserved[2], then LE32 type,
 * LE32 payload length (EXCLUDING the 8-byte header). Unknown types are skipped
 * with bounds checks. MAC and PHY are mandatory; known duplicates, invalid MAC,
 * NSS outside 1..2 and trailing/incomplete entries are rejected atomically.
 * bands: 1=2.4 GHz, 2=5 GHz, 4=6 GHz capability; this does NOT authorize channels
 * or transmit power. max_bandwidth and PHY flags are retained as raw fields. */
int mt2_nic_parse(const uint8_t *,uint32_t,mt2_nic_caps *);
/* Full Connac2 MCU event, with 36-byte header. Follows the pinned MT7921 path's
 * sequence and unsolicited-event routing; no unverified fixed capability EID
 * is assumed. Only complete mandatory capability TLVs establish success.
 * MT2_PENDING means unrelated/stale; errors and pending leave out unchanged. */
int mt2_nic_response(uint8_t sequence,const uint8_t *,uint32_t,mt2_nic_caps *);
#endif
