/* SPDX-License-Identifier: BSD-3-Clause-Clear */
#ifndef AIRWINDOWS_MT7922_CHANNEL_CONTROL_H
#define AIRWINDOWS_MT7922_CHANNEL_CONTROL_H
#include "mac_init.h"
typedef struct mt2_channel_control {
    mt2_pcie_mcu *mcu;
    const mt2_mac_state *mac;
    const mt2_regdomain *domain;
    mt2_tx_queue *tx;
    uint32_t bands,clc_known,clc_config,environment,nss,chainmask;
    uint32_t prepared,attempted,configured,monitor,primary,center,width;
    uint32_t power_attempted,power_configured;
    uint32_t context_attempted,context_configured;
    uint8_t context_address[6];
    uint8_t alpha2[2];
    int fault;
} mt2_channel_control;
/* One zero-initialized control per transport generation. Inputs must originate
 * from that generation's actual NIC/regdomain/CLC/MAC results; policy must be
 * the same one applied by CLC. All objects stay nonpaged, immutable where input,
 * disjoint and alive. Caller serializes these APIs with raw TX and MCU polling.
 * Numeric radio-link v3 channels distinguish 2.4/5 GHz only, not 6 GHz. */
int mt2_channel_prepare(mt2_channel_control *,mt2_pcie_mcu *,const mt2_mac_state *,mt2_tx_queue *,
    const mt2_regdomain *,const mt2_nic_caps *,const mt2_clc_policy *);
/* Pure request encoder after prepare: length or error, no partial output. */
int mt2_channel_domain_request(const mt2_channel_control *,uint8_t *,uint32_t);
/* One initial MAC-enable + domain sequence. Does not install SKU/SAR/BSS/WTBL,
 * activate raw TX or assert RF readiness. No driver IOCTL calls this yet. */
int mt2_channel_apply(mt2_channel_control *);
/* Explicit 20/40/80 MHz primary/center. All occupied spectrum must be covered.
 * No raw TX may remain queued/inflight during a command sequence. First call
 * sets RX path, switches channel and enables/configures sniffer plus RX filter;
 * subsequent calls switch/reconfigure. Commits state only after command drain.
 * Partial failure faults/disarms, retains DMA memory, requires chip teardown. */
int mt2_channel_monitor(mt2_channel_control *,uint32_t,uint32_t,uint32_t);
int mt2_channel_monitor_stop(mt2_channel_control *);
/* Eligibility only: still requires SKU/SAR, WCID/BSS, queue activation, actual
 * tuning and runtime integration. DFS, no-IR, PSD and unknown flags deny TX. */
int mt2_channel_permits_tx(const mt2_channel_control *);
/* Pure preflight after prepare, before MAC/channel publication. Commits the
 * permission output only for supported geometry; permission is policy only. */
int mt2_channel_validate(const mt2_channel_control *,uint32_t,uint32_t,uint32_t,uint32_t *);
#endif
