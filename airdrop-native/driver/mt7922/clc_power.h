/* SPDX-License-Identifier: BSD-3-Clause-Clear */
#ifndef AIRWINDOWS_MT7922_CLC_POWER_H
#define AIRWINDOWS_MT7922_CLC_POWER_H
#include "firmware.h"
#define MT2_HW_TYPE_COMMAND (MT2_MCU_QUERY|0x01edu)
#define MT2_CLC_COMMAND (MT2_MCU_CE|0x5cu)
#define MT2_CLC_HEADER 76u
#define MT2_CLC_MAX_PAYLOAD (8192u-64u)
typedef struct mt2_hw_type {uint32_t encapsulation,valid_raw,event_id;} mt2_hw_type;
typedef struct mt2_clc_reply {uint32_t channel_config,event_tag,event_length;} mt2_clc_reply;
typedef struct mt2_clc_policy {
    uint8_t alpha2[2];
    uint32_t environment,acpi_known,acpi_flags,mtcl_config,power_type,world_mtcl_config;
} mt2_clc_policy;
typedef struct mt2_clc_view {const uint8_t *block;uint32_t length,matches,fallback;} mt2_clc_view;
typedef struct mt2_clc_plan {mt2_clc_view views[3];uint32_t selected_mask,requests,fallback_mask;} mt2_clc_plan;
typedef struct mt2_clc_result {uint32_t attempted,completed,requests_drained,requests_replied,selected_mask,fallback_mask,channel_known,channel_config;int error;} mt2_clc_result;
/* Fixed read: 16-byte EFUSE block 0x550, byte 11, bit0. No arbitrary address,
 * write, OTP programming or bulk data output. Full Connac2 framing and returned
 * address must match; valid_raw==0 is conservatively rejected pending hardware
 * qualification. That additional gate is not claimed as MT7921's own check. */
int mt2_hw_type_response(uint8_t,const uint8_t *,uint32_t,mt2_hw_type *);
int mt2_clc_response(uint8_t,const uint8_t *,uint32_t,mt2_clc_reply *);
/* Trusted immutable RAM and disjoint output. Preflight all selected flat-table
 * records before sending any. Index0 selects EFUSE bit0; other indices use first
 * match, as upstream. Index3 is handled separately by regulatory.c. Missing
 * power variant, malformed records and unknown ACPI acquisition state fail.
 * Country fallback is exactly upstream's 00/INDOOR, recorded explicitly.
 * acpi_known means platform acquisition/translation completed, not guessed.
 * mtcl_config is for alpha2; world_mtcl_config is independently resolved for
 * "00", because upstream recomputes MTCL when falling back to the world table.
 * Policy must remain immutable with plan. No RF operation or TX authorization. */
int mt2_clc_plan_build(const uint8_t *,uint32_t,uint32_t,const mt2_clc_policy *,uint32_t,mt2_clc_plan *);
/* Requires a successful plan_build result and the same immutable policy/RAM.
 * Encode nth selected record (0-based), preserving every opaque data byte.
 * Returns length or negative error; rejection leaves output unchanged. */
int mt2_clc_request(const mt2_clc_plan *,const mt2_clc_policy *,uint32_t ordinal,uint32_t event_enabled,uint8_t *,uint32_t);
#endif
