/* SPDX-License-Identifier: BSD-3-Clause-Clear */
#ifndef AIRWINDOWS_MT7922_CHIP_H
#define AIRWINDOWS_MT7922_CHIP_H
#include "firmware.h"
typedef struct mt2_register_io {
    void *context;
    uint32_t bar_bytes;
    int (*read)(void *,uint32_t bar_offset,uint32_t *value);
    int (*write)(void *,uint32_t bar_offset,uint32_t value);
    void (*barrier)(void *);
    uint64_t (*milliseconds)(void *);
    int (*delay_ms)(void *,uint32_t);
} mt2_register_io;
/* Caller holds a device-wide lock across each transaction and all lifecycle
 * operations. BAR is powered, mapped and at least 1 MiB. No PCI config access.
 * Remapped access restores the previous selector, even after an access error. */
int mt2_reg_read(const mt2_register_io *,uint32_t physical_address,uint32_t *);
int mt2_reg_write(const mt2_register_io *,uint32_t physical_address,uint32_t);
enum mt2_ring_id {MT2_RING_TX_DATA,MT2_RING_TX_COMMAND,MT2_RING_TX_DOWNLOAD,
                  MT2_RING_RX_EVENT,MT2_RING_RX_DATA,MT2_RING_RX_DONE,MT2_RING_COUNT};
typedef struct mt2_ring_binding {uint64_t address;uint32_t count;} mt2_ring_binding;
typedef struct mt2_chip {
    mt2_register_io io;
    mt2_ring_binding rings[MT2_RING_COUNT];
    uint32_t attempted,published,quiescent,active;
    int start_error,stop_error;
} mt2_chip;
/* Zero-initialized chip object; one start only. PnP identity must already match
 * the intended MT7922. All ring descriptors and their referenced payloads must
 * be prepared in coherent nonpaged memory and mutually disjoint. This validates
 * descriptor spans; the Windows platform must validate payload allocations too.
 * This is a polling-only bring-up: interrupt masks stay disabled.
 * active means DMA configured, not firmware/radio/AirDrop readiness. */
int mt2_chip_start(mt2_chip *,const mt2_register_io *,const mt2_ring_binding *);
/* Stop never frees memory. A failed stop keeps quiescent false after any address
 * publication. No shared PCI bus reset, adapter binding or OS network changes. */
int mt2_chip_stop(mt2_chip *);
#endif
