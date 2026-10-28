/* SPDX-License-Identifier: BSD-3-Clause-Clear */
#ifndef AIRWINDOWS_MT7922_MAC_INIT_H
#define AIRWINDOWS_MT7922_MAC_INIT_H
#include "pcie_mcu.h"
typedef struct mt2_mac_state {
    uint32_t attempted,initialized,step,index,reg,mask,expected,observed;
    int error;
} mt2_mac_state;
/* One attempt per zero-initialized state/prepared generation, serialized by
 * the same passive device lock as firmware/PnP. Requires active DMA and observed
 * RAM readiness. Partial failure faults/disarms the MCU; no retry or rollback
 * claim. Leaves raw TX disarmed. This initializes common MAC registers and
 * clears admission counters, NOT station entries, channel, power or AWDL.
 * Success means the sequence completed, not verified RF operation. */
int mt2_mac_initialize(mt2_pcie_mcu *,mt2_mac_state *);
#endif
