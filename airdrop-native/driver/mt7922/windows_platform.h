/* SPDX-License-Identifier: BSD-3-Clause-Clear */
#ifndef AIRWINDOWS_MT7922_WINDOWS_PLATFORM_H
#define AIRWINDOWS_MT7922_WINDOWS_PLATFORM_H
#include <ntddk.h>
#include <wdf.h>
#include "mac_init.h"
#include "platform_acpi.h"
#include "radio_session.h"
#define MT2_WINDOWS_RX_SLOTS 16u

typedef struct mt2_windows_block {
    WDFCOMMONBUFFER handle;
    void *virtual_address;
    uint64_t address;
    uint32_t bytes;
} mt2_windows_block;
typedef struct mt2_windows_platform {
    WDFDEVICE device;
    WDFDMAENABLER dma;
    WDFMEMORY assembly_memory[3];
    WDFMEMORY bridge_memory;
    mt2_windows_block blocks[12]; /* descriptor/payload pairs, chip ring order */
    mt2_mcu_bank banks[MT2_MCU_BANKS];
    mt2_ring_binding bindings[MT2_RING_COUNT];
    PUCHAR bar;
    ULONG bar_bytes;
    volatile LONG removed;
    BOOLEAN prepared,powered,writes_enabled,boot_attempted;
    mt2_chip chip;
    mt2_pcie_mcu mcu;
    mt2_fw_transport transport;
    mt2_fw_result firmware;
    mt2_nic_caps caps;
    mt2_mac_state mac;
    mt2_platform_data platform_data;
    mt2_rx_bridge receive_bridge;
    mt2_radio_session session;
    uint32_t receive_lengths[MT2_WINDOWS_RX_SLOTS];
    uint8_t acpi_wire[MT2_ACPI_WIRE_BYTES];
    uint8_t session_scratch[MT2_SESSION_SCRATCH];
    uint32_t firmware_pair;
    mt2_tx_queue tx;
    uint32_t tx_states[64],tx_slots[64],caps_valid;
    int core_error,stop_error;
    uint64_t boot_deadline_ms;
} mt2_windows_platform;
/* Internal platform API, NOT an IOCTL interface. Owner must provide a passive
 * device-wide lock and exact KMDF lifecycle routing. Zero-initialize context.
 * The bring-up entry point wires these; the identity harness does not.
 * prepare maps resources and allocates memory but performs no MMIO accesses. */
NTSTATUS mt2_windows_prepare(mt2_windows_platform *,WDFDEVICE,WDFCMRESLIST,WDFCMRESLIST);
NTSTATUS mt2_windows_enter(mt2_windows_platform *);
/* Caller provides immutable kernel buffers for the duration of this call.
 * Both complete images must match the pinned installed-image SHA-256 hashes
 * before any ACPI evaluation, register access or address publication. Acquires
 * fixed OEM power tables before MMIO. Boots firmware, queries
 * capabilities and initializes common MAC registers. Raw TX stays disarmed;
 * station/channel/power/AWDL initialization is still required. */
NTSTATUS mt2_windows_boot(mt2_windows_platform *,const uint8_t *,uint32_t,const uint8_t *,uint32_t);
/* Explicit radio entry: validates the separate pinned upstream pair, country
 * record and options before ACPI/MMIO, then boots and starts the radio session.
 * 60-second aggregate register/command deadline; ACPI cancellation still waits
 * for lower-stack completion. No install/rebind/security-setting changes. */
NTSTATUS mt2_windows_radio_start(mt2_windows_platform *,const uint8_t *,uint32_t,const uint8_t *,uint32_t,const mt2_radio_options *);
/* Stop a boot session while remaining in D0. Memory remains allocated. */
NTSTATUS mt2_windows_stop(mt2_windows_platform *);
NTSTATUS mt2_windows_leave(mt2_windows_platform *);
/* Surprise-removal callback first calls revoke WITHOUT taking the lock, then
 * takes the lock before any other state work. No further MMIO is attempted. */
void mt2_windows_revoke(mt2_windows_platform *);
/* Call only from EvtDeviceReleaseHardware, after the framework power-down
 * boundary, or a prepare failure before any addresses were published.
 * Never call merely because boot/stop timed out. No MMIO in this function. */
void mt2_windows_release(mt2_windows_platform *);
#endif
