/* SPDX-License-Identifier: BSD-3-Clause-Clear */
#ifndef AIRWINDOWS_MT7922_TX_POWER_H
#define AIRWINDOWS_MT7922_TX_POWER_H
#include "channel_control.h"
#include "platform_acpi.h"
#define MT2_SKU_HEADER 44u
#define MT2_SKU_ENTRY 162u
#define MT2_SKU_BATCH_BYTES (MT2_SKU_HEADER+8u*MT2_SKU_ENTRY)
typedef struct mt2_tx_power_plan {
    uint8_t alpha2[2];
    uint32_t prepared,bands,batches,environment,dfs_region;
    int32_t requested_half_dbm,limits[11];
} mt2_tx_power_plan;
typedef struct mt2_tx_power_result {
    uint32_t attempted,completed,batches_drained,pse_reads,last_pse_value;
    int error;
} mt2_tx_power_result;
/* Finite requested cap in half-dBm, -127..44 inclusive. 44 matches upstream's
 * 22-dBm host ceiling for firmware-derived domains; it is NOT an interpretation
 * of opaque eirp_raw or proof of legal/physical RF power. Zero means zero dBm,
 * never an unlimited default. Intersect with the acquired SAR range limits.
 * Firmware CLC/EFUSE must separately enforce device/country calibration limits.
 * Programs all reported bands, including 6 GHz, so firmware receives its full
 * batch sequence even though radio-link v3 only selects 2.4/5 GHz channels.
 * Input/output disjoint. Failure invalidates plan. No device access. */
int mt2_tx_power_prepare(const mt2_platform_policy *,uint32_t bands,int32_t requested_half_dbm,mt2_tx_power_plan *);
/* Immutable prepared plan; returns byte count or negative error. Failure leaves
 * output unchanged. The Connac2 header's version/len fields remain zero exactly
 * as upstream. VHT reserved bytes and unused 5/6GHz CCK slots remain 127. */
int mt2_tx_power_request(const mt2_tx_power_plan *,uint32_t ordinal,uint8_t *,uint32_t);
/* One attempt per channel generation, after initial monitor/channel setup and
 * before raw TX activation. Serialized caller-owned nonpaged scratch. Requires
 * matching country/environment/DFS/bands; no queued or firmware-owned raw TX.
 * Drain each no-ACK SKU batch, then query fixed PSE register via firmware before
 * sending another. On failure disarm MCU/raw TX and retain DMA; no rollback. */
int mt2_tx_power_apply(mt2_channel_control *,const mt2_tx_power_plan *,uint8_t *,uint32_t,mt2_tx_power_result *);
#endif
