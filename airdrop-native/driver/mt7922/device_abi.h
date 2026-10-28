/* SPDX-License-Identifier: BSD-3-Clause-Clear */
#ifndef AIRWINDOWS_MT7922_DEVICE_ABI_H
#define AIRWINDOWS_MT7922_DEVICE_ABI_H
#include "firmware.h"
#define MT2_IOCTL_STATUS ((0x22u<<16)|(1u<<14)|(0x900u<<2))
#define MT2_IOCTL_BOOT ((0x22u<<16)|(3u<<14)|(0x901u<<2))
#define MT2_IOCTL_STOP ((0x22u<<16)|(2u<<14)|(0x902u<<2))
#define MT2_BOOT_MAGIC 0x3242544du
#define MT2_STATUS_MAGIC 0x3253544du
#define MT2_ABI_VERSION 1u
#define MT2_BOOT_HEADER 16u
#define MT2_STATUS_BYTES 128u
enum mt2_status_flags {MT2_STATUS_PREPARED=1,MT2_STATUS_POWERED=2,MT2_STATUS_REMOVED=4,
    MT2_STATUS_BOOT_ATTEMPTED=8,MT2_STATUS_FIRMWARE_READY=16,MT2_STATUS_CAPS_VALID=32,
    MT2_STATUS_DMA_PUBLISHED=64,MT2_STATUS_DMA_QUIESCENT=128};
typedef struct mt2_boot_view {const uint8_t *patch,*ram;uint32_t patch_bytes,ram_bytes;} mt2_boot_view;
/* METHOD_BUFFERED request: magic, version, patch_bytes, ram_bytes, then both
 * exact images. Bounds validation only; kernel platform verifies SHA-256 before
 * MMIO. Reject trailing bytes, empty images, oversize and length overflow.
 * On failure out is unchanged. No pointer is accepted from a user packet. */
int mt2_boot_request(const uint8_t *,uint32_t,mt2_boot_view *);
#endif
