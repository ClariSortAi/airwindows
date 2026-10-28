/* SPDX-License-Identifier: BSD-3-Clause-Clear */
#ifndef AIRWINDOWS_MT7922_MONITOR_CONTEXT_H
#define AIRWINDOWS_MT7922_MONITOR_CONTEXT_H
#include "channel_control.h"
#define MT2_MONITOR_WCID 19u
/* Initial single monitor interface only: BSS/OMAC/band/WMM/link index zero.
 * Initial BSS WCIDs are zero, matching mt7921_add_interface's add_dev call
 * before assigning the interface's reserved WCID 19. Deactivation carries 19,
 * matching removal after that assignment. This creates neither a
 * station record nor a usable transmit WCID. Never activate raw TX from this
 * result alone. Caller owns a valid unicast address and serialized generation.
 * Start after channel-domain setup, before enabling sniffer; stop sniffer before
 * stopping this context. Country/power/RF readiness are separate requirements.
 * Pure encoder returns length or error; rejected requests leave output intact. */
int mt2_monitor_context_request(uint32_t,int,const uint8_t *,uint8_t *,uint32_t);
/* One start attempt per controller generation; success requires both UNI
 * statuses and both DMA drains. No IOCTL calls this. All buffers are internal.
 * Stop disables BSS then device, but does not establish RF/DMA quiescence.
 * Partial failure clears readiness, disarms queues and retains allocations. */
int mt2_monitor_context_start(mt2_channel_control *,const uint8_t *);
int mt2_monitor_context_stop(mt2_channel_control *);
/* Change only OMAC0 address, after context setup. Active raw TX is allowed only
 * when neither DMA nor firmware owns a frame. Pending does not change identity.
 * Commit the new source address after zero UNI status and command-DMA drain. */
int mt2_monitor_context_address(mt2_channel_control *,const uint8_t *);
#endif
