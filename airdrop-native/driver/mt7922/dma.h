/* SPDX-License-Identifier: BSD-3-Clause-Clear */
#ifndef AIRWINDOWS_MT7922_DMA_H
#define AIRWINDOWS_MT7922_DMA_H
#include "mcu.h"

/* Native little-endian, coherent DMA only. All calls are serialized by the
 * platform; callbacks must not reenter these queues. Allocate before chip_start
 * and retain every allocation until chip_stop proves DMA quiescence.
 * Each queue owns disjoint descriptor, payload and (RX) assembly allocations.
 * Prepare checks this queue's physical spans, not cross-queue/virtual aliases. */
typedef struct mt2_dma_memory {
    mt2_descriptor *descriptors;
    uint64_t descriptor_address;
    uint8_t *pool;
    uint64_t pool_address;
    uint32_t pool_bytes,count,slot_bytes;
} mt2_dma_memory;

typedef struct mt2_command_queue {
    mt2_dma_memory memory;
    mt2_tx_io io;
    uint32_t head,tail,queued,prepared,active,used,download;
    int fault;
} mt2_command_queue;

/* One queue for command TX17, another for FWDL TX16. No raw-frame TX here.
 * publish writes the next producer index. Sequence/output change only when
 * publication is attempted; a failed write may already have reached hardware.
 * DMA completion releases command storage, not the awaited firmware reply. */
int mt2_command_prepare(mt2_command_queue *,const mt2_dma_memory *,const mt2_tx_io *,int download);
int mt2_command_activate(mt2_command_queue *);
void mt2_command_disarm(mt2_command_queue *);
int mt2_command_submit(mt2_command_queue *,uint32_t command,uint8_t *sequence,
                       const uint8_t *payload,uint32_t bytes);
int mt2_command_complete(mt2_command_queue *,uint32_t hardware_next_index);

#define MT2_RX_RECORD_MAX 65535u
typedef struct mt2_rx_io {
    void *context;
    /* Full acquire/release DMA fence, not just a compiler barrier. */
    void (*barrier)(void *);
    int (*publish)(void *,uint32_t recycled_index);
    /* Borrowed assembly buffer; copy before returning. Receives the complete
     * DMA record (including Connac2 RXD), never a claimed decoded 802.11 frame.
     * Called after successful descriptor recycling; failure faults the queue. */
    int (*record)(void *,const uint8_t *,uint32_t);
} mt2_rx_io;
typedef struct mt2_rx_queue {
    mt2_dma_memory memory;
    mt2_rx_io io;
    uint8_t *assembly;
    uint32_t capacity,length,tail,dropping,prepared,active,used;
    uint64_t records,dropped;
    int fault;
} mt2_rx_queue;
/* All slots prepared; chip_start initially advertises count-1. Each returned
 * descriptor is rebuilt before publishing its index, preserving one reserve.
 * Non-WED Connac2 RX; fragmented records are assembled up to capacity.
 * Hardware-drop or oversized records are discarded through their final segment.
 * Impossible DMA lengths/pointers fault instead of exposing untrusted memory. */
int mt2_rx_prepare(mt2_rx_queue *,const mt2_dma_memory *,uint8_t *assembly,
                    uint32_t capacity,const mt2_rx_io *);
int mt2_rx_activate(mt2_rx_queue *);
void mt2_rx_disarm(mt2_rx_queue *);
/* Returns consumed descriptor count, 0 when pending, or a negative fault.
 * budget must be 1..count. No device registers are read directly. */
int mt2_rx_poll(mt2_rx_queue *,uint32_t budget);
#endif
