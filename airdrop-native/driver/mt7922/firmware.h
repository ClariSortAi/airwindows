/* SPDX-License-Identifier: BSD-3-Clause-Clear */
#ifndef AIRWINDOWS_MT7922_FIRMWARE_H
#define AIRWINDOWS_MT7922_FIRMWARE_H
#include "mcu.h"
#define MT2_FW_PATCH 1u
#define MT2_FW_RAM 2u
#define MT2_FW_MAX_SIZE (8u*1024u*1024u)
#define MT2_FW_MAX_REGIONS 32u
#define MT2_FW_CHUNK 4096u
#define MT2_FW_PROTOCOL MT2_PROTOCOL
#define MT2_FW_TIMEOUT (-5)
typedef struct mt2_fw_region {
    uint32_t offset,length,address,mode,skip,override_address;
} mt2_fw_region;
typedef struct mt2_fw_plan {
    uint32_t kind,count,entry;
    mt2_fw_region regions[MT2_FW_MAX_REGIONS];
} mt2_fw_plan;
typedef struct mt2_fw_result {
    uint32_t patch_present,regions_sent,bytes_sent,start_acked,ready;
    int32_t primary_error,release_error;
} mt2_fw_result;
typedef struct mt2_fw_transport {
    void *context;
    int (*send)(void *,uint32_t,const uint8_t *,uint32_t,int,uint32_t *);
    int (*drain)(void *);
    int (*wait_state)(void *,uint32_t);
} mt2_fw_transport;
/* Validates layout/bounds, not origin or cryptographic integrity. The platform
 * must verify a trusted image manifest and keep both input images immutable.
 * Firmware decryption remains in the device's established download mechanism;
 * this module never attempts to recover encryption keys. */
int mt2_fw_validate(const uint8_t *,uint32_t,uint32_t,mt2_fw_plan *);
/* Preflight both images before any callback. send must use mt2_mcu_encode,
 * route scatter to FWDL, and await correctly matched responses when requested.
 * drain must prove DMA completion. wait_state must observe actual ROM/RAM
 * state with a bounded deadline. They must never report assumed success.
 * ready means firmware-ready only, not MAC/radio/AWDL readiness. */
int mt2_fw_boot(const uint8_t *patch,uint32_t patch_size,const uint8_t *ram,
                uint32_t ram_size,const mt2_fw_transport *,mt2_fw_result *);
#endif
