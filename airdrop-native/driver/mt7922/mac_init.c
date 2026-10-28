/* SPDX-License-Identifier: BSD-3-Clause-Clear
 * Copyright (C) 2020, 2023 MediaTek Inc.
 * MT7921/MT7922 mt7921_mac_init, mt792x_mac_init_band and command definitions
 * from pinned mt76. Bounds, error propagation and raw-header policy are port
 * additions. Uses physical addresses through the MT7922 register map.
 */
#include "mac_init.h"
#define WTBL_UPDATE 0x820d4230u
static int read_reg(mt2_pcie_mcu *m,mt2_mac_state *s,uint32_t reg,uint32_t *out) {
    int e;
    if(!m->active || !m->chip->active || m->fault)return MT2_INVALID;
    s->reg=reg;e=mt2_reg_read(&m->chip->io,reg,out);
    if(!e)s->observed=*out;
    return e;
}
static int write_reg(mt2_pcie_mcu *m,mt2_mac_state *s,uint32_t reg,uint32_t value) {
    if(!m->active || !m->chip->active || m->fault)return MT2_INVALID;
    s->reg=reg;s->expected=value;return mt2_reg_write(&m->chip->io,reg,value);
}
static int rmw(mt2_pcie_mcu *m,mt2_mac_state *s,uint32_t reg,uint32_t clear,uint32_t set,int stable) {
    uint32_t value,expected;int e;
    s->mask=clear|set;e=read_reg(m,s,reg,&value);if(e)return e;
    expected=(value&~clear)|set;e=write_reg(m,s,reg,expected);
    if(!e)e=read_reg(m,s,reg,&value);
    if(!e && stable && (value&s->mask)!=(expected&s->mask))e=MT2_PROTOCOL;
    return e;
}
static int wtbl_idle(mt2_pcie_mcu *m,mt2_mac_state *s) {
    const mt2_register_io *io=&m->chip->io;
    uint64_t start=io->milliseconds(io->context);uint32_t value,attempt;int e;
    for(attempt=0;;++attempt) {
        e=read_reg(m,s,WTBL_UPDATE,&value);if(e)return e;
        if(!(value&0x80000000u))return 0;
        /* mt76_poll timeout is microseconds: 5000 us, not 5000 ms.
         * Iteration bound also covers a stalled platform clock. */
        if(attempt>=5 || io->milliseconds(io->context)-start>=5)return MT2_FW_TIMEOUT;
        e=io->delay_ms(io->context,1);if(e)return e;
    }
}
static int init_band(mt2_pcie_mcu *m,mt2_mac_state *s,uint32_t band) {
    uint32_t offset=band*0x10000u;int e;
    e=rmw(m,s,0x820e40f4u+offset,0x3fu,0x6003fu,1);
    /* Counter enables are not stable configuration fields. Flush writes and
     * reject failed/all-ones reads without asserting their value is retained. */
    if(!e)e=rmw(m,s,0x820e53c4u+offset,0,0x40000000u,0);
    if(!e)e=rmw(m,s,0x820e5380u+offset,0,0x40000000u,0);
    if(!e)e=rmw(m,s,0x820ed004u+offset,0,0x300u,1);
    if(!e)e=rmw(m,s,0x820e7000u+offset,0x80fff8u,0x3000u,1);
    if(!e)e=rmw(m,s,0x820e9008u+offset,0xc3000000u,0x03000000u,1);
    return e;
}
int mt2_mac_initialize(mt2_pcie_mcu *m,mt2_mac_state *s) {
    uint32_t i,value;int e;
    if(!m || !s || s->attempted || !m->prepared || !m->active || m->busy ||
       m->fault || !m->chip || !m->chip->active || (m->last_state&3u)!=3u)return MT2_INVALID;
    s->attempted=1;s->initialized=0;s->step=1;s->index=0;
    e=mt2_pcie_mac_command(m,MT2_MCU_EFUSE_MODE);if(e)goto done;
    s->step=2;
    e=rmw(m,s,0x820cd004u,0xfff8u,0x3000u,1);
    /* AWDL consumes raw 802.11. Clear header translation while retaining
     * upstream hardware deaggregation, handled by raw_rx.c. */
    if(!e)e=rmw(m,s,0x820cd000u,0x80000u,0x8000u,1);
    if(e)goto done;
    s->step=3;e=wtbl_idle(m,s);
    for(i=0;!e && i<20;++i) {
        s->index=i;s->mask=0x3ffu;
        e=read_reg(m,s,WTBL_UPDATE,&value);
        if(!e)e=write_reg(m,s,WTBL_UPDATE,(value&~0x3ffu)|i|0x1000u);
        if(!e)e=wtbl_idle(m,s);
    }
    if(e)goto done;
    s->step=4;
    for(i=0;!e && i<2;++i){s->index=i;e=init_band(m,s,i);}
    if(e)goto done;
    s->step=5;e=mt2_pcie_mac_command(m,MT2_MCU_PROTECT);
    if(!e){s->initialized=1;s->step=6;}
done:
    s->error=e;
    if(e){if(!m->fault)m->fault=e;mt2_pcie_disarm(m);}
    return e;
}
