/* SPDX-License-Identifier: BSD-3-Clause-Clear */
#ifndef AIRWINDOWS_MT7922_RADIO_ABI_H
#define AIRWINDOWS_MT7922_RADIO_ABI_H
#include "device_abi.h"
#include "radio_session.h"
#define MT2_IOCTL_RADIO_START ((0x22u<<16)|(3u<<14)|(0x903u<<2))
#define MT2_IOCTL_RADIO_SERVICE ((0x22u<<16)|(3u<<14)|(0x904u<<2))
#define MT2_IOCTL_RADIO_RECEIVE ((0x22u<<16)|(1u<<14)|(0x905u<<2))
#define MT2_IOCTL_RADIO_TRANSMIT ((0x22u<<16)|(3u<<14)|(0x906u<<2))
#define MT2_IOCTL_RADIO_CHANNEL ((0x22u<<16)|(3u<<14)|(0x907u<<2))
#define MT2_IOCTL_RADIO_STATUS ((0x22u<<16)|(1u<<14)|(0x908u<<2))
#define MT2_IOCTL_RADIO_ADDRESS ((0x22u<<16)|(3u<<14)|(0x909u<<2))
#define MT2_RADIO_START_MAGIC 0x3252544du /* MTR2 */
#define MT2_RADIO_STATUS_MAGIC 0x3253524du /* MRS2 */
#define MT2_RADIO_START_BYTES 64u
#define MT2_RADIO_STATUS_BYTES 256u
#define MT2_RADIO_FIRMWARE_PAIR 1u /* pinned linux-firmware 20260724 pair */
typedef struct mt2_radio_view {
    mt2_boot_view firmware;mt2_radio_options options;
    const uint8_t *frame;uint32_t frame_bytes,no_ack,rate,primary,center,width;
    uint8_t address[6];
} mt2_radio_view;
/* All are METHOD_BUFFERED, no user pointers. Parse into a temporary view so
 * shared input/output storage cannot be overwritten before use. No I/O.
 * Failure leaves out unchanged. Start fields: magic/version/patch/ram lengths,
 * alpha2+MAC (8 bytes), env/type/primary/center/width/FCS/TX/cap/pair/reserved;
 * exact images follow the 64-byte header. Pair 1 is explicit, never a fallback. */
int mt2_radio_request(uint32_t,const uint8_t *,uint32_t,mt2_radio_view *);
/* Recognizes exact IOCTL values, including encoded access bits. */
int mt2_radio_ioctl(uint32_t);
#endif
