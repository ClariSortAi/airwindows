/* SPDX-License-Identifier: BSD-3-Clause-Clear */
#ifndef AIRWINDOWS_MT7922_RADIO_SESSION_H
#define AIRWINDOWS_MT7922_RADIO_SESSION_H
#include "radio_tx.h"
#include "tx_power.h"
#include "rx_bridge.h"
#define MT2_SESSION_SCRATCH MT2_CLC_MAX_PAYLOAD
typedef struct mt2_radio_options {
    uint8_t alpha2[2],address[6];
    uint32_t environment,power_type,primary,center,width,fcs_present,enable_tx;
    int32_t cap_half_dbm;
} mt2_radio_options;
int mt2_radio_options_valid(const mt2_radio_options *);
enum mt2_session_stage {MT2_SESSION_PREFLIGHT,MT2_SESSION_REGDOMAIN,MT2_SESSION_HW_TYPE,
    MT2_SESSION_POLICY,MT2_SESSION_CLC,MT2_SESSION_CHANNEL_PREPARE,MT2_SESSION_DOMAIN,
    MT2_SESSION_CONTEXT,MT2_SESSION_MONITOR,MT2_SESSION_SKU,MT2_SESSION_RECEIVE,
    MT2_SESSION_TRANSMIT,MT2_SESSION_READY};
typedef struct mt2_radio_session {
    mt2_pcie_mcu *mcu;const mt2_mac_state *mac;mt2_tx_queue *tx;
    const mt2_nic_caps *caps;const mt2_platform_data *platform;mt2_rx_bridge *receive;
    mt2_channel_control channel;mt2_regdomain domain;mt2_hw_type hardware;
    mt2_platform_policy policy;mt2_tx_power_plan power;
    mt2_clc_result clc;mt2_tx_power_result sku;mt2_radio_options options;
    uint32_t prepared,attempted,ready,stopped,stage;int error;
} mt2_radio_session;
/* Stable zeroed nonpaged session/dependencies, one device-wide passive lock.
 * Prepare after MCU/raw queue binding, preferably before boot. Installs the
 * receive bridge as the sole optional MCU-record consumer; no hardware I/O.
 * Capabilities/MAC/platform objects are filled by the same generation's BOOT. */
int mt2_radio_session_prepare(mt2_radio_session *,mt2_pcie_mcu *,const mt2_mac_state *,
    mt2_tx_queue *,const mt2_nic_caps *,const mt2_platform_data *,mt2_rx_bridge *);
/* Owner verifies this complete immutable RAM image matches the pair booted on
 * the device. Explicit options, disjoint caller-owned scratch >= constant.
 * No firmware/image pointers are retained after return. FCS policy must come
 * from qualified platform configuration. One hardware attempt; partial failure
 * revokes the whole session without replay/rollback or freeing DMA storage.
 * ready means software sequence completed, NOT proof of RF/iOS operation. */
int mt2_radio_session_start(mt2_radio_session *,const uint8_t *,uint32_t,const mt2_radio_options *,uint8_t *,uint32_t);
int mt2_radio_session_poll(mt2_radio_session *);
int mt2_radio_session_channel(mt2_radio_session *,uint32_t,uint32_t,uint32_t);
int mt2_radio_session_transmit(mt2_radio_session *,const uint8_t *,uint32_t,uint32_t,uint32_t *);
int mt2_radio_session_transmit_rate(mt2_radio_session *,const uint8_t *,uint32_t,uint32_t,uint32_t,uint32_t *);
int mt2_radio_session_receive(mt2_radio_session *,uint8_t *,uint32_t,uint32_t *);
int mt2_radio_session_address(mt2_radio_session *,const uint8_t *);
/* Idempotent software revocation, including queued receive data. Owner must
 * still stop the chip and prove DMA quiescence before releasing allocations. */
void mt2_radio_session_stop(mt2_radio_session *);
#endif
