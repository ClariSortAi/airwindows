/* SPDX-License-Identifier: BSD-3-Clause-Clear */
#ifndef AIRWINDOWS_MT7922_PLATFORM_ACPI_H
#define AIRWINDOWS_MT7922_PLATFORM_ACPI_H
#include "clc_power.h"
#define MT2_ACPI_TABLE_BYTES 263u /* MTFG header + at most 255 flag indices. */
#define MT2_ACPI_WIRE_BYTES (12u+8u*MT2_ACPI_TABLE_BYTES)
#define MT2_ACPI_OUTPUT_SIGNATURE 0x426f6541u
#define MT2_ACPI_NAME_NOT_FOUND 0xc0000034u
enum mt2_acpi_state {MT2_ACPI_UNKNOWN=0,MT2_ACPI_ABSENT=1,MT2_ACPI_DATA=2};
enum mt2_acpi_index {MT2_ACPI_MTCL=0,MT2_ACPI_MTDS=1,MT2_ACPI_MTGS=2,MT2_ACPI_MTFG=3};
typedef struct mt2_acpi_table {uint32_t state,ntstatus,size;uint8_t data[MT2_ACPI_TABLE_BYTES];} mt2_acpi_table;
typedef struct mt2_platform_data {
    mt2_acpi_table tables[4];
    uint32_t attempted,completed,failed_index;
    int error;
} mt2_platform_data;
typedef struct mt2_acpi_io {
    void *context;
    int32_t (*evaluate)(void *,const uint8_t name[4],uint8_t *,uint32_t,uint32_t *);
} mt2_acpi_io;
typedef struct mt2_platform_policy {
    mt2_clc_policy clc;
    int32_t sar_half_dbm[11]; /* mt76 frequency-range order; 127 means no SAR limit. */
    uint32_t prepared,dfs_region,dynamic_active,dynamic_version,geographic_version;
} mt2_platform_policy;
/* Decode explicit ACPI V1 integer-array output. Reject truncation, alternate
 * types, nested packages, >byte values, unknown status and partial output.
 * Only NAME_NOT_FOUND with zero output proves absence. Invalidates out first.
 * Buffers must be disjoint; no user-provided ACPI data is accepted by the driver. */
int mt2_acpi_decode(uint32_t,const uint8_t *,uint32_t,mt2_acpi_table *);
/* Fixed MTCL/MTDS/MTGS/MTFG methods only. Scratch is caller-owned/nonpaged in the
 * kernel. No retries or namespace fallback. Stops at first error; completed is
 * set only after all four reads and table validation. Serialized by caller. */
int mt2_platform_read(const mt2_acpi_io *,uint8_t *,uint32_t,mt2_platform_data *);
int mt2_platform_validate(const mt2_platform_data *);
/* Explicit country/environment/power type/DFS inputs. No location inference.
 * First MTDS row and FCC/ETSI/world MTGS rows follow pinned mt792x semantics.
 * Does not apply power, imply legal channels or supply a default TX power cap.
 * Caller must later intersect SAR with channel/antenna/hardware/user limits.
 * Failure clears prepared and all policy values. Input/output must be disjoint. */
int mt2_platform_policy_build(const mt2_platform_data *,const uint8_t alpha2[2],
    uint32_t environment,uint32_t power_type,uint32_t dfs_region,mt2_platform_policy *);
#endif
