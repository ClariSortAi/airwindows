/* SPDX-License-Identifier: BSD-3-Clause-Clear
 * Copyright (C) 2020, 2021 MediaTek Inc.
 * Connac2 SKU layouts/channel lists/pacing: pinned mt76_connac_mcu.c/h.
 * Finite caps, atomic preflight and failure ownership are Windows additions.
 */
#include "tx_power.h"
static void zero(void *p,uint32_t n){uint32_t i;for(i=0;i<n;++i)((uint8_t *)p)[i]=0;}
static int alpha_valid(const uint8_t *a){return (a[0]=='0' && a[1]=='0') || (a[0]>='A' && a[0]<='Z' && a[1]>='A' && a[1]<='Z');}
static const uint8_t channels2[]={1,2,3,4,5,6,7,8,9,10,11,12,13,14};
static const uint8_t channels5[]={36,38,40,42,44,46,48,50,52,54,56,58,60,62,64,100,102,104,106,108,110,112,114,116,118,120,122,124,126,128,132,134,136,138,140,142,144,149,151,153,155,157,159,161,165,169,173,177};
static const uint8_t channels6[]={1,3,5,7,9,11,13,15,17,19,21,23,25,27,29,33,35,37,39,41,43,45,47,49,51,53,55,57,59,61,65,67,69,71,73,75,77,79,81,83,85,87,89,91,93,97,99,101,103,105,107,109,111,113,115,117,119,121,123,125,129,131,133,135,137,139,141,143,145,147,149,151,153,155,157,161,163,165,167,169,171,173,175,177,179,181,183,185,187,189,193,195,197,199,201,203,205,207,209,211,213,215,217,219,221,225,227,229,233};
static const uint32_t counts[3]={sizeof(channels2),sizeof(channels5),sizeof(channels6)};
static uint32_t batches(uint32_t bands) {
    uint32_t i,n=0;for(i=0;i<3;++i)if(bands&(1u<<i))n+=(counts[i]+7)/8;return n;
}
static int valid(const mt2_tx_power_plan *p) {
    uint32_t i;
    if(!p || p->prepared!=1 || !p->bands || (p->bands&~7u) || p->batches!=batches(p->bands) ||
       !alpha_valid(p->alpha2) || p->environment>2 || p->dfs_region>3 || p->requested_half_dbm< -127 || p->requested_half_dbm>44)return 0;
    for(i=0;i<11;++i)if(p->limits[i]< -127 || p->limits[i]>p->requested_half_dbm)return 0;
    return 1;
}
int mt2_tx_power_prepare(const mt2_platform_policy *p,uint32_t bands,int32_t requested,mt2_tx_power_plan *out) {
    uint32_t i;
    if(!out)return MT2_INVALID;zero(out,sizeof(*out));
    if(!p || p->prepared!=1 || p->clc.acpi_known!=1 || !bands || (bands&~7u) ||
       !alpha_valid(p->clc.alpha2) || p->clc.environment>2 || p->dfs_region>3 || requested< -127 || requested>44)return MT2_INVALID;
    for(i=0;i<11;++i)if(p->sar_half_dbm[i]<0 || p->sar_half_dbm[i]>127)return MT2_INVALID;
    out->alpha2[0]=p->clc.alpha2[0];out->alpha2[1]=p->clc.alpha2[1];out->bands=bands;
    out->environment=p->clc.environment;out->dfs_region=p->dfs_region;out->requested_half_dbm=requested;
    for(i=0;i<11;++i)out->limits[i]=p->sar_half_dbm[i]<requested?p->sar_half_dbm[i]:requested;
    out->batches=batches(bands);out->prepared=1;return 0;
}
static int32_t limit(const mt2_tx_power_plan *p,uint32_t band,uint32_t channel) {
    static const uint32_t starts[11]={2402,5150,5350,5470,5725,5945,6165,6405,6525,6705,6865};
    static const uint32_t ends[11]={2494,5350,5470,5725,5950,6165,6405,6525,6705,6865,7125};
    uint32_t i,freq=band==0?(channel==14?2484:2407+channel*5):band==1?5000+channel*5:5950+channel*5;
    for(i=0;i<11;++i)if(freq>=starts[i] && freq<ends[i])return p->limits[i];
    return p->requested_half_dbm;
}
static void sku(uint8_t *p,uint32_t band,int32_t cap) {
    uint32_t i,row;uint8_t value=(uint8_t)cap;
    for(i=0;i<161;++i)p[i]=127;
    if(!band)for(i=0;i<4;++i)p[i]=value;
    for(i=4;i<29;++i)p[i]=value;
    for(row=0;row<4;++row)for(i=0;i<10;++i)p[29+row*12+i]=value;
    for(i=77;i<161;++i)p[i]=value;
}
int mt2_tx_power_request(const mt2_tx_power_plan *p,uint32_t ordinal,uint8_t *out,uint32_t capacity) {
    uint32_t band,left=ordinal,start,n,bytes,i;const uint8_t *channels;
    if(!valid(p) || !out || ordinal>=p->batches)return MT2_INVALID;
    for(band=0;band<3;++band) {
        if(!(p->bands&(1u<<band)))continue;
        n=(counts[band]+7)/8;if(left<n)break;left-=n;
    }
    if(band==3)return MT2_INVALID;start=left*8;n=counts[band]-start;if(n>8)n=8;
    bytes=44+162*n;if(capacity<bytes)return MT2_INVALID;
    channels=band==0?channels2:band==1?channels5:channels6;
    zero(out,44);out[4]=(uint8_t)n;out[5]=(uint8_t)(band+1);out[6]=(uint8_t)(ordinal+1==p->batches);
    out[8]=p->alpha2[0];out[9]=p->alpha2[1];
    for(i=0;i<n;++i){out[44+162*i]=channels[start+i];sku(out+45+162*i,band,limit(p,band,channels[start+i]));}
    return (int)bytes;
}
int mt2_tx_power_apply(mt2_channel_control *c,const mt2_tx_power_plan *p,uint8_t *scratch,uint32_t capacity,mt2_tx_power_result *out) {
    mt2_tx_power_result result={0};uint32_t i,value;int n,e=MT2_INVALID;
    if(!out)return MT2_INVALID;
    if(!c || !c->prepared || c->fault || !c->configured || !c->monitor || c->power_attempted || !valid(p) || !scratch ||
       !c->mcu->active || c->mcu->fault || c->mcu->busy || !c->mcu->clc_completed || !c->mac->initialized ||
       !c->tx->prepared || c->tx->fault || c->tx->active || c->tx->queued || c->tx->inflight ||
       p->bands!=c->bands || p->dfs_region!=c->domain->dfs_region || p->environment!=c->environment ||
       p->alpha2[0]!=c->alpha2[0] || p->alpha2[1]!=c->alpha2[1])goto done;
    /* Validate every encoded batch before first hardware publication. */
    for(i=0;i<p->batches;++i){n=mt2_tx_power_request(p,i,scratch,capacity);if(n<0)goto done;}
    c->power_attempted=1;c->power_configured=0;result.attempted=1;
    for(i=0;i<p->batches;++i) {
        n=mt2_tx_power_request(p,i,scratch,capacity);if(n<0){e=n;goto fault;}
        e=mt2_pcie_radio_command(c->mcu,MT2_MCU_SKU,scratch,(uint32_t)n);if(e)goto fault;
        ++result.batches_drained;
        e=mt2_pcie_query_pse(c->mcu,&value);if(e)goto fault;
        ++result.pse_reads;result.last_pse_value=value;
    }
    result.completed=1;c->power_configured=1;goto done;
fault:
    c->fault=e;c->configured=0;c->monitor=0;c->primary=0;c->center=0;c->width=0;c->power_configured=0;c->context_configured=0;
    if(!c->mcu->fault)c->mcu->fault=e;
    mt2_tx_disarm(c->tx);mt2_pcie_disarm(c->mcu);
done:
    result.error=e;*out=result;return e;
}
