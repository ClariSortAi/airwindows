/* SPDX-License-Identifier: BSD-3-Clause-Clear
 * Copyright (C) 2020, 2021 MediaTek Inc.
 * Initial DEV/BSS requests: pinned mt76_connac_mcu_uni_add_dev and
 * mt7921_add_interface. Bounded lifecycle/failure ownership are port additions.
 */
#include "monitor_context.h"
static int address_valid(const uint8_t *p) {
    uint32_t i,any=0;if(!p || (p[0]&1u))return 0;
    for(i=0;i<6;++i)any|=p[i];return any!=0;
}
int mt2_monitor_context_request(uint32_t cmd,int enabled,const uint8_t *mac,uint8_t *out,uint32_t capacity) {
    uint32_t i,n;
    if(!out || !address_valid(mac) || (enabled!=0 && enabled!=1))return MT2_INVALID;
    if(cmd==MT2_MCU_DEVICE)n=16;else if(cmd==MT2_MCU_BSS)n=36;else return MT2_INVALID;
    if(capacity<n)return MT2_INVALID;
    for(i=0;i<n;++i)out[i]=0;
    out[6]=(uint8_t)(n-4);out[8]=(uint8_t)enabled;
    if(cmd==MT2_MCU_DEVICE){for(i=0;i<6;++i)out[10+i]=mac[i];}
    else {
        out[12]=2;out[14]=1;out[16]=1; /* INFRA_AP = BIT(1)|BIT(16). */
        if(!enabled){out[24]=MT2_MONITOR_WCID;out[30]=MT2_MONITOR_WCID;}
    }
    return (int)n;
}
static int usable(const mt2_channel_control *c) {
    return c && c->prepared && !c->fault && c->configured && !c->monitor &&
        c->mcu && c->mcu->active && !c->mcu->fault && !c->mcu->busy && c->mcu->clc_completed &&
        c->mac && c->mac->initialized && c->tx && c->tx->prepared && !c->tx->fault &&
        !c->tx->active && !c->tx->queued && !c->tx->inflight;
}
static int change(mt2_channel_control *c,const uint8_t *mac,int enabled) {
    uint8_t device[16],bss[36];int e;
    if(mt2_monitor_context_request(MT2_MCU_DEVICE,enabled,mac,device,sizeof(device))!=16 ||
       mt2_monitor_context_request(MT2_MCU_BSS,enabled,mac,bss,sizeof(bss))!=36)return MT2_INVALID;
    if(enabled)c->context_attempted=1;
    e=mt2_pcie_context_command(c->mcu,enabled?MT2_MCU_DEVICE:MT2_MCU_BSS,
                              enabled?device:bss,enabled?16u:36u);
    if(!e)e=mt2_pcie_context_command(c->mcu,enabled?MT2_MCU_BSS:MT2_MCU_DEVICE,
                                     enabled?bss:device,enabled?36u:16u);
    if(!e){c->context_configured=(uint32_t)enabled;return 0;}
    c->fault=e;c->context_configured=0;c->configured=0;c->power_configured=0;
    c->monitor=0;c->primary=0;c->center=0;c->width=0;
    if(!c->mcu->fault)c->mcu->fault=e;
    mt2_tx_disarm(c->tx);mt2_pcie_disarm(c->mcu);return e;
}
int mt2_monitor_context_start(mt2_channel_control *c,const uint8_t *mac) {
    uint32_t i;int e;
    if(!usable(c) || c->context_attempted || !address_valid(mac))return MT2_INVALID;
    /* Copy before publication; caller's input remains disjoint and stable. */
    for(i=0;i<6;++i)c->context_address[i]=mac[i];
    e=change(c,c->context_address,1);return e;
}
int mt2_monitor_context_stop(mt2_channel_control *c) {
    if(!usable(c) || !c->context_configured)return MT2_INVALID;
    return change(c,c->context_address,0);
}
int mt2_monitor_context_address(mt2_channel_control *c,const uint8_t *mac) {
    uint8_t request[16];uint32_t i;int e;
    if(!c || !c->prepared || c->fault || !c->context_configured || !c->mcu || !c->mac || !c->tx ||
       !c->mcu->active || c->mcu->fault || c->mcu->busy || !c->mcu->clc_completed || !c->mac->initialized ||
       !c->tx->prepared || c->tx->fault || !address_valid(mac))return MT2_INVALID;
    if(c->tx->queued || c->tx->inflight)return MT2_PENDING;
    if(mt2_monitor_context_request(MT2_MCU_DEVICE,1,mac,request,sizeof(request))!=16)return MT2_INVALID;
    e=mt2_pcie_context_command(c->mcu,MT2_MCU_DEVICE,request,sizeof(request));
    if(!e){for(i=0;i<6;++i)c->context_address[i]=request[10+i];return 0;}
    c->fault=e;c->context_configured=0;c->configured=0;c->power_configured=0;
    c->monitor=0;c->primary=0;c->center=0;c->width=0;
    if(!c->mcu->fault)c->mcu->fault=e;
    mt2_tx_disarm(c->tx);mt2_pcie_disarm(c->mcu);return e;
}
