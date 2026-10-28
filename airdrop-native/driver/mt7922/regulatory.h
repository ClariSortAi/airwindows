/* SPDX-License-Identifier: BSD-3-Clause-Clear */
#ifndef AIRWINDOWS_MT7922_REGULATORY_H
#define AIRWINDOWS_MT7922_REGULATORY_H
#include "firmware.h"
#define MT2_REGD_COMMAND (MT2_MCU_CE|0xd1u)
#define MT2_REGD_MAX_RULES 128u
#define MT2_REGD_REQUEST_HEADER 80u
#define MT2_REGD_REQUEST_MAX (MT2_REGD_REQUEST_HEADER+20u+32u*MT2_REGD_MAX_RULES)
typedef struct mt2_reg_rule {
    uint32_t start_mhz,end_mhz,bandwidth_mhz,eirp_raw,flags;
} mt2_reg_rule;
typedef struct mt2_regdomain {
    uint32_t count,dfs_region,event_tag,event_length;
    mt2_reg_rule rules[MT2_REGD_MAX_RULES];
} mt2_regdomain;
/* Builds MT7921/MT7922 CE SET_REGD_CH, not the MT7925 UNI/TLV format.
 * RAM must be trusted/immutable, out disjoint. Validates image layout and all
 * CLC block bounds, then chooses the FIRST index-3 block exactly as mt7921's
 * load_clc does (MT7925's type-at-byte-6 selection must not be substituted).
 * Forwards selected country's opaque rule/signature bytes unchanged. This is
 * NOT signature validation, country/location evidence or TX authorization.
 * Returns payload length, or a negative error without modifying output.
 * Missing CLC/country is an error; no permissive default is manufactured. */
int mt2_regd_request(const uint8_t *,uint32_t,const uint8_t alpha2[2],uint8_t *,uint32_t);
/* Solicited Connac2 response with 36-byte header + 4-byte event prefix +
 * 24-byte rule header + n*32 bytes. Tag/length are preserved as raw diagnostics;
 * upstream supplies no validation semantics for those two fields. Outer length,
 * count, DFS enum, rule geometry and complete records must agree. Unsolicited
 * or wrong-sequence events are pending; failure/pending never changes output.
 * EIRP is kept opaque, not converted into an invented host power limit. */
int mt2_regd_response(uint8_t,const uint8_t *,uint32_t,mt2_regdomain *);
#endif
