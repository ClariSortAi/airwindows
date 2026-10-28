/* SPDX-License-Identifier: BSD-3-Clause-Clear */
#ifndef AIRWINDOWS_MT7922_RADIO_TX_H
#define AIRWINDOWS_MT7922_RADIO_TX_H
#include "monitor_context.h"
/* Internal fixed-rate monitor TX. Requires same-generation NIC/MAC/CLC,
 * channel permission, completed finite SKU delivery, initial DEV/BSS context,
 * and a raw queue bound to MCU completion polling. Common MAC already cleared
 * admission counters for all 20 slots; the upstream monitor path uses reserved
 * WCID 19 without adding an associated peer. These checks establish software
 * prerequisites only; initial hardware qualification remains necessary.
 * All calls use the MCU/PnP passive lock. No IOCTL invokes these APIs yet. */
int mt2_radio_tx_start(mt2_channel_control *);
/* Raw action/plain or QoS data MPDU, no FCS/radiotap/encryption. Source address
 * must match this monitor context. Fixed 6 Mbps OFDM, OMAC 0, WCID 19, antenna
 * selection from observed chain count. No caller-controlled hardware fields.
 * Token means queued ownership, NOT an ACK or received frame at an iOS peer. */
int mt2_radio_tx_submit(mt2_channel_control *,const uint8_t *,uint32_t,uint32_t no_ack,uint32_t *token);
/* Explicit legacy OFDM rate in 500 kbps units: 12,18,24,36,48,72,96,108.
 * Maps to Connac2 hardware encoding; no raw rate-table index is accepted. */
int mt2_radio_tx_submit_rate(mt2_channel_control *,const uint8_t *,uint32_t,uint32_t rate,uint32_t no_ack,uint32_t *token);
/* Bounded nonblocking poll. Completion remains allowed on RX-only channels;
 * new submissions recheck channel permission after every channel switch. */
int mt2_radio_tx_poll(mt2_channel_control *);
/* Terminal for this generation. Pending ownership returns MT2_PENDING; caller
 * polls before retrying or uses chip shutdown. No freeing, reset or replay. */
int mt2_radio_tx_stop(mt2_channel_control *);
#endif
