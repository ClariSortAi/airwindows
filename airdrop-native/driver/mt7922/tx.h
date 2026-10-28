/* SPDX-License-Identifier: BSD-3-Clause-Clear */
#ifndef AIRWINDOWS_MT7922_TX_H
#define AIRWINDOWS_MT7922_TX_H
#include <stdint.h>

#define MT2_TXWI_SIZE 64u
#define MT2_PAYLOAD_MAX 4095u
#define MT2_SLOT_SIZE 4160u
#define MT2_TOKEN_MAX 8192u
#define MT2_DMA_DONE 0x80000000u
enum mt2_result { MT2_OK=0, MT2_INVALID=-1, MT2_FULL=-2, MT2_PENDING=-3, MT2_PROTOCOL=-4 };
typedef struct mt2_descriptor { uint32_t address, control, address1, info; } mt2_descriptor;
typedef struct mt2_tx_config {
    uint32_t wcid;      /* WCID selected by the caller's qualified interface path. */
    uint32_t own_mac;   /* Initialized OMAC/BSS slot, 0..63. */
    uint32_t spe_index; /* Initialized antenna mapping; do not infer from MAC. */
    uint32_t rate;      /* Connac2 encoded legacy OFDM rate, 0x48..0x4f. */
    uint32_t no_ack;    /* Multicast also disables ACK automatically. */
} mt2_tx_config;
typedef struct mt2_tx_io {
    void *context;
    void (*barrier)(void *);
    int (*publish)(void *, uint32_t next_index);
} mt2_tx_io;
typedef struct mt2_tx_queue {
    mt2_descriptor *descriptors;
    uint8_t *pool;
    uint32_t *states, *slots;
    uint64_t descriptor_address,pool_address;
    uint32_t count, head, tail, queued, tokens, inflight, cursor;
    uint32_t prepared, active, used;
    int fault;
    mt2_tx_io io;
} mt2_tx_queue;

/* Software only. Input and output must be disjoint. Raw action/data MPDU,
 * no radiotap/FCS, no encryption, fragments, DS translation or aggregation.
 * Rejection leaves output unchanged. Returns 64 or a negative result. */
int mt2_tx_encode(const uint8_t *, uint32_t, uint64_t payload_address,
                  uint32_t token, const mt2_tx_config *, uint8_t *, uint32_t);
/* Caller serializes all queue operations. Memory must be coherent, nonpaged,
 * disjoint from every other DMA allocation, and live until proven quiescent.
 * No function maps memory, calls Windows APIs, or accesses hardware directly. */
int mt2_tx_prepare(mt2_tx_queue *, mt2_descriptor *, uint64_t descriptor_address,
                   uint32_t count, uint8_t *pool, uint64_t pool_address,
                   uint32_t pool_bytes, uint32_t *states, uint32_t tokens,
                   uint32_t *slots, const mt2_tx_io *);
/* Platform must establish firmware, queue, WCID, MAC, channel, regulatory and
 * power readiness before activation. Activation alone proves none of them. */
int mt2_tx_activate(mt2_tx_queue *);
void mt2_tx_disarm(mt2_tx_queue *);
int mt2_tx_submit(mt2_tx_queue *, const uint8_t *, uint32_t,
                  const mt2_tx_config *, uint32_t *token);
int mt2_tx_dma_complete(mt2_tx_queue *, uint32_t hardware_next_index);
int mt2_tx_firmware_complete(mt2_tx_queue *, const uint8_t *, uint32_t);
/* Payload is reusable only after both DMA completion and firmware release.
 * A fault retains ownership. Disarm does NOT establish DMA quiescence.
 * Firmware token release is not proof of delivery to an iOS peer. */
#endif
