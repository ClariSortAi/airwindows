/* SPDX-License-Identifier: BSD-3-Clause-Clear */
#ifndef AIRWINDOWS_MT7922_RX_BRIDGE_H
#define AIRWINDOWS_MT7922_RX_BRIDGE_H
#include "raw_rx.h"
#define MT2_RX_MESSAGE_MAX 16392u /* radio_link header + maximum payload */
typedef struct mt2_rx_bridge {
    uint8_t *storage;uint32_t *lengths;
    uint32_t count,head,tail,queued,prepared,active,used,fcs_present;
    uint64_t accepted,taken,overflow,inactive,discarded[9];
} mt2_rx_bridge;
/* Caller owns disjoint nonpaged storage/lengths/zeroed context, serialized by the
 * device lock. No DMA addresses are published for this output queue. */
int mt2_rx_bridge_prepare(mt2_rx_bridge *,uint8_t *,uint32_t,uint32_t *,uint32_t);
/* Explicit FCS policy from the platform's qualified receive configuration.
 * One activation per generation. No inference from frame contents. */
int mt2_rx_bridge_start(mt2_rx_bridge *,uint32_t fcs_present);
/* MCU record callback. Copies before RX assembly is recycled. Radio parser
 * errors/full queue are counted drops, never fatal MCU transport errors.
 * Drops newest on overflow; non-frame events are not counted as overflow. */
int mt2_rx_bridge_record(void *,const uint8_t *,uint32_t);
/* Complete radio_link v3 RX message. Empty -> PENDING; short output -> FULL.
 * Rejection leaves output/length and queue untouched. Successful take wipes
 * the copied queue slot. Input/output storage must remain disjoint. */
int mt2_rx_bridge_take(mt2_rx_bridge *,uint8_t *,uint32_t,uint32_t *);
/* Terminal, software only. Wipes queued bytes; no DMA/quiescence claim. */
void mt2_rx_bridge_stop(mt2_rx_bridge *);
#endif
