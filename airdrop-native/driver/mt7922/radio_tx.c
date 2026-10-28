/* SPDX-License-Identifier: BSD-3-Clause-Clear
 * Copyright (C) 2020, 2021, 2022 MediaTek Inc.
 * Fixed-rate/antenna/WCID mapping: pinned mt76_connac_mac.c, mt76_connac.h,
 * mt7921/main.c and mt792x_core.c. Readiness/lifetime gates are port additions.
 */
#include "radio_tx.h"
static int live(const mt2_channel_control *c) {
    const mt2_pcie_mcu *m;
    if(!c || !c->prepared || c->fault || !c->mcu || !c->mac || !c->tx)return 0;
    m=c->mcu;
    return m->active && !m->fault && !m->busy && m->chip->active && m->raw_tx==c->tx &&
        (m->last_state&3u)==3 && m->nic_valid && m->clc_completed && c->mac->initialized &&
        c->tx->prepared && !c->tx->fault;
}
static int ready(const mt2_channel_control *c) {
    return live(c) && c->power_configured && c->context_configured &&
        ((c->nss==1 && c->chainmask==1) || (c->nss==2 && c->chainmask==3)) && mt2_channel_permits_tx(c);
}
static int fail(mt2_channel_control *c,int e) {
    c->fault=e;c->configured=0;c->monitor=0;c->power_configured=0;c->context_configured=0;
    c->primary=0;c->center=0;c->width=0;
    if(!c->mcu->fault)c->mcu->fault=e;
    mt2_tx_disarm(c->tx);mt2_pcie_disarm(c->mcu);return e;
}
int mt2_radio_tx_start(mt2_channel_control *c) {
    uint32_t producer,consumer;int e;
    if(!ready(c) || c->tx->active || c->tx->used || c->tx->queued || c->tx->inflight)return MT2_INVALID;
    /* A new software queue must match the ring reset by this chip generation. */
    e=mt2_reg_read(&c->mcu->chip->io,0xd4308,&producer);if(e)return fail(c,e);
    e=mt2_reg_read(&c->mcu->chip->io,0xd430c,&consumer);if(e)return fail(c,e);
    if(producer || consumer)return fail(c,MT2_PROTOCOL);
    e=mt2_tx_activate(c->tx);return e?fail(c,e):0;
}
int mt2_radio_tx_submit_rate(mt2_channel_control *c,const uint8_t *frame,uint32_t bytes,uint32_t rate,uint32_t no_ack,uint32_t *token) {
    static const uint32_t rates[8]={12,18,24,36,48,72,96,108};
    static const uint32_t codes[8]={0x4b,0x4f,0x4a,0x4e,0x49,0x4d,0x48,0x4c};
    mt2_tx_config config={MT2_MONITOR_WCID,0,0,0x4b,0};uint32_t i;int e;
    if(!ready(c) || !c->tx->active || !frame || bytes<24 || no_ack>1 || !token)return MT2_INVALID;
    for(i=0;i<6;++i)if(frame[10+i]!=c->context_address[i])return MT2_INVALID;
    for(i=0;i<8;++i)if(rate==rates[i])break;if(i==8)return MT2_INVALID;config.rate=codes[i];
    config.spe_index=c->chainmask==3?24u:0u;config.no_ack=no_ack;
    e=mt2_tx_submit(c->tx,frame,bytes,&config,token);
    return c->tx->fault?fail(c,e):e;
}
int mt2_radio_tx_submit(mt2_channel_control *c,const uint8_t *frame,uint32_t bytes,uint32_t no_ack,uint32_t *token) {
    return mt2_radio_tx_submit_rate(c,frame,bytes,12,no_ack,token);
}
int mt2_radio_tx_poll(mt2_channel_control *c) {
    int e;
    if(!live(c) || !c->tx->active)return MT2_INVALID;
    e=mt2_pcie_poll(c->mcu);return e?fail(c,e):0;
}
int mt2_radio_tx_stop(mt2_channel_control *c) {
    if(!live(c) || !c->tx->active)return MT2_INVALID;
    if(c->tx->queued || c->tx->inflight)return MT2_PENDING;
    mt2_tx_disarm(c->tx);return 0;
}
