/* SPDX-License-Identifier: BSD-3-Clause-Clear */
#ifndef AIRWINDOWS_MT7922_PCIE_MCU_H
#define AIRWINDOWS_MT7922_PCIE_MCU_H
#include "chip.h"
#include "dma.h"
#include "nic_caps.h"
#include "regulatory.h"
#include "clc_power.h"

#define MT2_MCU_BANKS 5u
/* Bank order: command TX17, download TX16, event RX0, data RX2, done RX4.
 * Each bank owns disjoint coherent descriptor/payload storage. RX banks also
 * need non-DMA assembly storage. Firmware identity and PCI identity are checked
 * by the platform before starting; this object allocates or frees nothing. */
typedef struct mt2_mcu_bank {
    mt2_dma_memory memory;
    uint8_t *assembly;
    uint32_t capacity;
} mt2_mcu_bank;
struct mt2_pcie_mcu;
typedef struct mt2_pcie_port {struct mt2_pcie_mcu *owner;uint32_t registers;} mt2_pcie_port;
typedef struct mt2_pcie_mcu {
    mt2_chip *chip;
    mt2_tx_queue *raw_tx; /* Borrowed until teardown; binding never activates it. */
    mt2_command_queue command[2];
    mt2_rx_queue receive[3];
    mt2_pcie_port ports[MT2_MCU_BANKS];
    uint32_t prepared,active,used,busy,waiting,matched,command_id,status,last_state;
    uint8_t sequence;
    int fault;
    uint64_t ignored_records,matched_responses;
    mt2_nic_caps *nic_target; /* Private, valid only during a serialized query. */
    mt2_regdomain *regd_target;
    mt2_hw_type *hw_target;
    mt2_clc_reply *clc_target;
    uint32_t *pse_target;
    uint32_t nic_valid,nic_chip_caps;
    uint32_t hw_type_valid,hw_encapsulation,clc_attempted,clc_completed,clc_channel_known,clc_channel_config;
    /* Optional record consumer for data, TX status/releases and unsolicited
     * events. Borrowed buffer; must copy before returning, never reenter.
     * Return 0 or a negative error. Absence means count and discard. */
    void *record_context;
    int (*other_record)(void *,const uint8_t *,uint32_t);
} mt2_pcie_mcu;
/* Prepare BEFORE chip_start, with zero-initialized m and chip. No I/O.
 * Cross-validates physical/virtual spans of these five banks and metadata.
 * The platform must additionally validate the independent raw TX data pool.
 * Configure optional record consumer before activation. */
int mt2_pcie_prepare(mt2_pcie_mcu *,mt2_chip *,const mt2_mcu_bank banks[MT2_MCU_BANKS]);
/* Bind one prepared, unused raw queue. Validates its used virtual/physical
 * spans against all MCU banks, assembly and metadata. If chip is started its
 * TX0 binding must already match. Serialize with all MCU/PnP work. No I/O.
 * Subsequent MCU polling reclaims TX0 DMA and routes Connac2 release records;
 * any MCU fault disarms the borrowed queue without freeing its allocations. */
int mt2_pcie_bind_tx(mt2_pcie_mcu *,mt2_tx_queue *);
/* After chip_start: verifies exact ring bindings before activating queues and
 * filling firmware callbacks. No assumed firmware readiness. */
int mt2_pcie_activate(mt2_pcie_mcu *,mt2_fw_transport *);
/* Poll completions and RX using chip register callbacks. All entry points need
 * the same PASSIVE_LEVEL device lock as chip_start/stop. No concurrent callers.
 * Timeout/error permanently faults the transport; no command replay/restart.
 * Disarm does not establish DMA quiescence or release any allocation. */
int mt2_pcie_poll(mt2_pcie_mcu *);
/* Fixed Connac2 GET_NIC_CAPAB, zero payload, only after observed RAM readiness.
 * Output is committed only after the entire exchange succeeds. This does not
 * configure a MAC, WTBL, channel or transmit power. */
int mt2_pcie_query_nic(mt2_pcie_mcu *,mt2_nic_caps *);
/* Fixed, no caller payload: select on-device EFUSE calibration (not an EFUSE
 * read/write operation), or set RTS length 0x92b / packet threshold 2, band 0.
 * Requires observed RAM readiness and drains command DMA after the ACK. */
int mt2_pcie_mac_command(mt2_pcie_mcu *,uint32_t command);
/* Internal typed CE regulatory exchange. Requires an observed capability reply
 * with REGD_EN and observed RAM readiness. Caller pins the complete trusted RAM
 * image and supplies disjoint, nonpaged scratch/output. Preflights before I/O;
 * output commits only after full response validation and command-DMA drain.
 * No power/channel/TX activation; not exposed by the diagnostic IOCTL ABI. */
int mt2_pcie_query_regdomain(mt2_pcie_mcu *,const uint8_t *,uint32_t,const uint8_t alpha2[2],
                           uint8_t *scratch,uint32_t capacity,mt2_regdomain *);
/* Fixed read-only hardware-type query, only after RAM and NIC capability proof.
 * No arbitrary EFUSE address or programming operation is exposed. */
int mt2_pcie_query_hw_type(mt2_pcie_mcu *,mt2_hw_type *);
/* One-shot CLC application per prepared generation, no retry after partial
 * publication. Complete layout/selection preflight before first send. Acquires
 * no ACPI data itself: policy must come from verified platform acquisition.
 * RAM/policy/scratch/result must be disjoint, immutable where inputs, and remain
 * alive/nonpaged throughout. Completion means selected commands drained and,
 * when firmware advertises CLC events, validated replies were received. It does
 * NOT mean SKU/SAR/channel/raw TX is configured. Not exposed in diagnostic ABI. */
int mt2_pcie_apply_clc(mt2_pcie_mcu *,const uint8_t *,uint32_t,const mt2_clc_policy *,uint8_t *,uint32_t,mt2_clc_result *);
/* Internal channel/power transport, seven fixed command IDs/shapes only.
 * Requires completed CLC. MAC/domain/context/geometry and idle raw-TX gates are
 * owned by channel_control.c. Drain every command, including no-ACK domain and
 * RX filter. No diagnostic IOCTL exposes this or accepts arbitrary payloads. */
int mt2_pcie_radio_command(mt2_pcie_mcu *,uint32_t,const uint8_t *,uint32_t);
/* Internal DEV/BSS initial basic TLVs only. Requires an actual zero UNI status
 * and completed DMA; unlike SNIFFER these commands carry firmware status. */
int mt2_pcie_context_command(mt2_pcie_mcu *,uint32_t,const uint8_t *,uint32_t);
/* Fixed CE query of PSE register 0x820c8000 after each SKU batch, as upstream's
 * firmware/PSE pacing step. No arbitrary register address or write is exposed.
 * Returned bits are data, not signed firmware status. Output commits after DMA. */
int mt2_pcie_query_pse(mt2_pcie_mcu *,uint32_t *);
void mt2_pcie_disarm(mt2_pcie_mcu *);
#endif
