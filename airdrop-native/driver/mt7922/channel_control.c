/* SPDX-License-Identifier: BSD-3-Clause-Clear
 * Copyright (C) 2020, 2023 MediaTek Inc.
 * MT7922 commands: pinned mt7921/mcu.c and mt76_connac_mcu.c/h.
 * Geometry/overlap and rule-to-channel flags adapted from pinned Windows
 * channel_control.c; flag definitions match Linux v6.18 headers. See SOURCES.
 * Channel availability is not proof of actual tuning or TX power eligibility.
 */
#include "channel_control.h"
static const uint8_t channels[42]={1,2,3,4,5,6,7,8,9,10,11,12,13,14,
    36,40,44,48,52,56,60,64,100,104,108,112,116,120,124,128,132,136,140,144,
    149,153,157,161,165,169,173,177};
static void put16(uint8_t *p,uint32_t v) {p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);}
static void put32(uint8_t *p,uint32_t v) {put16(p,v);put16(p+2,v>>16);}
static uint32_t frequency(uint32_t c) {return c==14?2484:c<14?2407+5*c:5000+5*c;}
static int supported(const mt2_channel_control *c,uint32_t ch) {
    uint32_t i;
    if (!(c->bands&(ch<=14?1u:2u))) return 0;
    if (ch>=169 && (!c->clc_known || !(c->clc_config&1u))) return 0;
    for (i=0;i<42;++i) if (channels[i]==ch) return 1;
    return 0;
}
static int coverage(const mt2_channel_control *c,uint32_t center,uint32_t width,uint32_t *flags) {
    uint32_t low=center-width/2,high=center+width/2,cursor=low,i,round,combined=0;
    const mt2_regdomain *d=c->domain;
    for (i=0;i<d->count;++i) {
        const mt2_reg_rule *r=&d->rules[i];
        if (r->start_mhz<=low && r->end_mhz>=high && r->bandwidth_mhz>=width) cursor=high;
    }
    for (round=0;round<d->count && cursor<high;++round) {
        uint32_t end=cursor;
        for (i=0;i<d->count;++i) {
            const mt2_reg_rule *r=&d->rules[i];
            if (r->start_mhz<=cursor && r->end_mhz>end && (r->flags&0x800u)) end=r->end_mhz;
        }
        if (end==cursor) return 0;
        cursor=end;
    }
    if (cursor<high) return 0;
    /* All overlapping restrictions apply, even when another rule covers it. */
    for (i=0;i<d->count;++i) {
        const mt2_reg_rule *r=&d->rules[i];
        if (r->start_mhz<high && r->end_mhz>low) {
            if(r->bandwidth_mhz<width && !(r->flags&0x800u))return 0;
            combined|=r->flags;
        }
    }
    *flags=combined;return 1;
}
static int geometry(const mt2_channel_control *c,uint32_t primary,uint32_t center,uint32_t width,uint32_t *flags) {
    uint32_t i,base,sub;
    if (!supported(c,primary) || (width!=20 && width!=40 && width!=80)) return 0;
    if (width==20) {if (center!=primary) return 0;}
    else if (width==40) {
        if (primary==14 || (primary+2!=center && center+2!=primary) || center<3) return 0;
        if (!supported(c,center-2) || !supported(c,center+2) ||
            (primary<=14 && center+2>13)) return 0;
        if (primary>14) {
            for (i=14;i<42;i+=2) if (channels[i]+2u==center) break;
            if (i==42) return 0;
        }
    } else {
        if (primary<=14) return 0;
        for (i=14;i<42;i+=4) if (channels[i]+6u==center) break;
        if (i==42) return 0;
        base=channels[i];
        if (primary<base || primary>base+12 || (primary-base)%4) return 0;
        for (sub=0;sub<4;++sub) if (!supported(c,base+4*sub)) return 0;
    }
    if (!coverage(c,frequency(center),width,flags)) return 0;
    if (width==80 && (*flags&0x8000u)) return 0;
    if (width==40 && (*flags&(primary<center?0x4000u:0x2000u))) return 0;
    return 1;
}
static uint32_t channel_flags(uint32_t raw) {
    static const uint8_t from[19]={0,3,4,7,8,12,13,14,15,16,17,18,19,20,21,22,23,24,25};
    static const uint8_t to[19]={6,9,3,1,1,10,5,4,7,8,13,19,20,2,21,22,23,25,26};
    uint32_t i,result=0;
    for (i=0;i<19;++i) if (raw&(1u<<from[i])) result|=1u<<to[i];
    /* Unknown rule flags and PTP/PTMP-only modes have no supported TX policy. */
    if ((raw&~0x03fff9ffu) || (raw&0x100060u)) result|=2u;
    return result;
}
static int live(const mt2_channel_control *c) {
    const mt2_pcie_mcu *m;
    if(!c || !c->prepared || c->fault || !c->mcu || !c->mac || !c->tx)return 0;
    m=c->mcu;
    return m->active && !m->fault && !m->busy && m->chip->active && (m->last_state&3u)==3u &&
        m->nic_valid && m->clc_completed && c->mac->initialized && c->tx->prepared && !c->tx->fault;
}
static int idle(const mt2_channel_control *c){return live(c) && !c->tx->queued && !c->tx->inflight;}
static int command(mt2_channel_control *c,uint32_t cmd,const uint8_t *data,uint32_t size) {
    int e=mt2_pcie_radio_command(c->mcu,cmd,data,size);
    if(e) {
        c->fault=e;c->configured=0;c->monitor=0;c->primary=0;c->center=0;c->width=0;c->power_configured=0;c->context_configured=0;
        if(!c->mcu->fault)c->mcu->fault=e;
        mt2_tx_disarm(c->tx);
        mt2_pcie_disarm(c->mcu);
    }
    return e;
}
int mt2_channel_prepare(mt2_channel_control *c,mt2_pcie_mcu *m,const mt2_mac_state *mac,mt2_tx_queue *tx,
    const mt2_regdomain *d,const mt2_nic_caps *caps,const mt2_clc_policy *p) {
    uint32_t i;
    if(!c || c->prepared || !m || !mac || !tx || !d || !caps || !p || p->environment>2 || p->acpi_known!=1 ||
       !m->clc_completed || m->clc_channel_known>1 || m->clc_channel_config>255 ||
       (caps->present&3u)!=3u || !(caps->bands&3u) || (caps->bands&~7u) || !caps->nss || caps->nss>2 ||
       caps->chainmask!=((1u<<caps->nss)-1) || !d->count || d->count>MT2_REGD_MAX_RULES || d->dfs_region>3 ||
       !((p->alpha2[0]=='0' && p->alpha2[1]=='0') ||
         (p->alpha2[0]>='A' && p->alpha2[0]<='Z' && p->alpha2[1]>='A' && p->alpha2[1]<='Z')))return MT2_INVALID;
    for(i=0;i<d->count;++i) {
        const mt2_reg_rule *r=&d->rules[i];
        if(!r->start_mhz || r->start_mhz>=r->end_mhz || r->end_mhz>10000 || !r->bandwidth_mhz || r->bandwidth_mhz>320)return MT2_INVALID;
    }
    c->mcu=m;c->mac=mac;c->tx=tx;c->domain=d;c->bands=caps->bands;c->nss=caps->nss;c->chainmask=caps->chainmask;
    c->clc_known=m->clc_channel_known;c->clc_config=m->clc_channel_config;c->environment=p->environment;
    c->alpha2[0]=p->alpha2[0];c->alpha2[1]=p->alpha2[1];c->prepared=1;return 0;
}
int mt2_channel_domain_request(const mt2_channel_control *c,uint8_t *out,uint32_t capacity) {
    uint8_t request[348]={0};uint32_t i,j,n=0,n2=0,flags,raw,temp,bytes;
    if(!c || !c->prepared || !out)return MT2_INVALID;
    request[0]=c->alpha2[0];request[1]=c->alpha2[1];request[5]=2;request[6]=3;
    for(i=0;i<42;++i) {
        uint32_t ch=channels[i];
        if(!geometry(c,ch,ch,20,&raw))continue;
        flags=channel_flags(raw);
        if((raw&4u) && c->environment!=2)flags|=2u;
        if((raw&8u) && c->environment!=1)flags|=2u;
        if(!geometry(c,ch,ch+2,40,&temp))flags|=16u;
        if(ch<3 || !geometry(c,ch,ch-2,40,&temp))flags|=32u;
        j=i<14?0:14+(i-14)/4*4;
        if(i<14 || !geometry(c,ch,channels[j]+6,80,&temp))flags|=128u;
        flags|=0x00182100u; /* no160/no320/noHE/noEHT on this v3 radio-link path */
        put16(request+12+8*n,ch);put32(request+16+8*n,flags);++n;if(i<14)++n2;
    }
    bytes=12+8*n;if(!n || capacity<bytes)return MT2_INVALID;
    request[8]=(uint8_t)n2;request[9]=(uint8_t)(n-n2);
    for(i=0;i<bytes;++i)out[i]=request[i];return (int)bytes;
}
int mt2_channel_apply(mt2_channel_control *c) {
    static const uint8_t enable[4]={1,0,0,0};uint8_t request[348];int e,n;
    if(!idle(c) || c->attempted)return MT2_INVALID;
    n=mt2_channel_domain_request(c,request,sizeof(request));if(n<0)return n;
    c->attempted=1;e=command(c,MT2_MCU_MAC_ENABLE,enable,4);
    if(!e)e=command(c,MT2_MCU_DOMAIN,request,(uint32_t)n);
    if(!e)c->configured=1;return e;
}
int mt2_channel_monitor(mt2_channel_control *c,uint32_t primary,uint32_t center,uint32_t width) {
    static const uint8_t enable[12]={0,0,0,0,0,0,8,0,1,0,0,0};
    uint8_t channel[76]={0},config[20]={0},filter[68]={0};uint32_t flags;int e=0;
    if(!idle(c) || !c->configured || !geometry(c,primary,center,width,&flags))return MT2_INVALID;
    channel[0]=(uint8_t)primary;channel[1]=(uint8_t)center;channel[2]=(uint8_t)(width==80?2:width==40?1:0);
    channel[3]=(uint8_t)c->nss;channel[4]=(uint8_t)c->chainmask;channel[10]=(uint8_t)(primary<=14?0:1);
    put16(config+4,1);put16(config+6,16);config[10]=(uint8_t)(primary<=14?1:2);
    config[11]=(uint8_t)(width==80);config[12]=(uint8_t)primary;
    config[13]=(uint8_t)(primary<center?1:primary>center?3:0);config[14]=(uint8_t)center;config[16]=1;
    filter[4]=1;put32(filter+8,0x80000060u); /* control + other BSS, drop FCS errors */
    if(!c->monitor)e=command(c,MT2_MCU_RX_PATH,channel,sizeof(channel));
    channel[4]=(uint8_t)c->nss;
    if(!e)e=command(c,MT2_MCU_CHANNEL,channel,sizeof(channel));
    if(!e && !c->monitor)e=command(c,MT2_MCU_SNIFFER,enable,sizeof(enable));
    if(!e)e=command(c,MT2_MCU_SNIFFER,config,sizeof(config));
    if(!e && !c->monitor)e=command(c,MT2_MCU_RX_FILTER,filter,sizeof(filter));
    if(!e){c->monitor=1;c->primary=primary;c->center=center;c->width=width;}
    return e;
}
int mt2_channel_monitor_stop(mt2_channel_control *c) {
    static const uint8_t disable[12]={0,0,0,0,0,0,8,0,0,0,0,0};int e;
    if(!idle(c) || !c->monitor)return MT2_INVALID;
    e=command(c,MT2_MCU_SNIFFER,disable,sizeof(disable));
    if(!e){c->monitor=0;c->primary=0;c->center=0;c->width=0;}return e;
}
static int permitted(const mt2_channel_control *c,uint32_t flags) {
    if((flags&~0x03fff9ffu) || (flags&0x1001f1u))return 0; /* includes PSD without a derived PSD power cap */
    if((flags&4u) && c->environment!=2)return 0;
    if((flags&8u) && c->environment!=1)return 0;
    return 1;
}
int mt2_channel_validate(const mt2_channel_control *c,uint32_t primary,uint32_t center,uint32_t width,uint32_t *allowed) {
    uint32_t flags;
    if(!c || !c->prepared || c->fault || !allowed || !geometry(c,primary,center,width,&flags))return MT2_INVALID;
    *allowed=(uint32_t)permitted(c,flags);return 0;
}
int mt2_channel_permits_tx(const mt2_channel_control *c) {
    uint32_t flags;
    if(!live(c) || !c->configured || !c->monitor || !geometry(c,c->primary,c->center,c->width,&flags))return 0;
    return permitted(c,flags);
}
