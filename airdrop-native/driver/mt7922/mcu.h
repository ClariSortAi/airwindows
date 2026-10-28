/* SPDX-License-Identifier: BSD-3-Clause-Clear */
#ifndef AIRWINDOWS_MT7922_MCU_H
#define AIRWINDOWS_MT7922_MCU_H
#include "tx.h"
#define MT2_MCU_QUERY (1u<<16)
#define MT2_MCU_UNI (1u<<17)
#define MT2_MCU_CE (1u<<18)
#define MT2_MCU_WA (1u<<19)
#define MT2_MCU_WM (1u<<20)
#define MT2_FW_SCATTER 0xeeu
#define MT2_MCU_MAX_PACKET 0x3fffu
#define MT2_MCU_EFUSE_MODE 0x21edu
#define MT2_MCU_PROTECT 0x3eedu
#define MT2_MCU_MAC_ENABLE 0x46edu
#define MT2_MCU_RX_PATH 0x4eedu
#define MT2_MCU_CHANNEL 0x08edu
#define MT2_MCU_SNIFFER (MT2_MCU_UNI|0x24u)
#define MT2_MCU_DOMAIN (MT2_MCU_CE|0x0fu)
#define MT2_MCU_RX_FILTER (MT2_MCU_CE|0x0au)
#define MT2_MCU_SKU (MT2_MCU_CE|0x5du)
#define MT2_MCU_PSE_READ (MT2_MCU_CE|MT2_MCU_QUERY|0xc0u)
#define MT2_MCU_DEVICE (MT2_MCU_UNI|1u)
#define MT2_MCU_BSS (MT2_MCU_UNI|2u)

/* Disjoint buffers. Successful encoding advances sequence 1..15; rejection
 * changes neither sequence nor output. Scatter has no header and uses FWDL,
 * never the normal command ring. UNI queries are deliberately unsupported:
 * the pinned Connac2 sender emits SET|ACK, unlike Connac3's query handling. */
int mt2_mcu_encode(uint32_t command,uint8_t *sequence,const uint8_t *payload,
                    uint32_t length,uint8_t *out,uint32_t capacity);
/* Strict decoder for boot replies, six UNI status commands, and fixed
 * MAC/channel/sniffer acknowledgments. Sniffer uses a UNI request but is not
 * one of MT7921's six UNI status responses. These ACK-only commands follow upstream's
 * sequence-only solicited matching and expose no verified firmware status.
 * Caller serializes commands and routes only MCU response packets here.
 * Unsolicited/wrong-sequence/wrong-command replies return MT2_PENDING without
 * changing status. Unknown command semantics fail, never imply success.
 * Firmware status is separate from this function's parse result. */
int mt2_mcu_response(uint32_t command,uint8_t sequence,const uint8_t *packet,
                      uint32_t size,uint32_t *firmware_status);
#endif
