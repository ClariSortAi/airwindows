/* SPDX-License-Identifier: BSD-3-Clause-Clear */
#ifndef AIRWINDOWS_MT7922_RAW_RX_H
#define AIRWINDOWS_MT7922_RAW_RX_H
#include "tx.h"
enum mt2_rx_outcome {MT2_RX_ACCEPT,MT2_RX_OTHER,MT2_RX_AIR_ERROR,MT2_RX_TRANSLATED,
    MT2_RX_CIPHER,MT2_RX_PROTECTED,MT2_RX_CONTROL,MT2_RX_MALFORMED,MT2_RX_CAPACITY};
/* Complete Connac2 RXD record -> radio_link v3 RX message:
 * 8-byte message header, 14-byte radiotap (FLAGS + CHANNEL), raw 802.11 MPDU.
 * fcs_present MUST come from verified platform configuration, never guessing.
 * No invented rate, signal, TSFT, successful ACK or authenticated peer claim.
 * Input/output must be disjoint. Output and length are unchanged on failure;
 * outcome is set. MT2_PENDING means deliberate frame/event discard; malformed
 * records return MT2_INVALID, insufficient output capacity returns MT2_FULL.
 * Decoding channel metadata does not authorize transmission on that channel. */
int mt2_raw_rx_record(const uint8_t *,uint32_t,uint32_t fcs_present,uint8_t *,uint32_t,
                       uint32_t *length,uint32_t *outcome);
#endif
