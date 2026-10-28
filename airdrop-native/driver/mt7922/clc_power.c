/* SPDX-License-Identifier: BSD-3-Clause-Clear
 * Copyright (C) 2020, 2023 MediaTek Inc.
 * Fixed EFUSE read and flat CLC format: mt7921/mcu.c, mt7921.h, mcu.h.
 * Bounded preflight and atomic outputs are native Windows port additions.
 */
#include "clc_power.h"
static uint32_t le16(const uint8_t *p){return p[0]|(uint32_t)p[1]<<8;}
static uint32_t le32(const uint8_t *p){return le16(p)|le16(p+2)<<16;}
static void put16(uint8_t *p,uint32_t x){p[0]=(uint8_t)x;p[1]=(uint8_t)(x>>8);}
static int frame(uint8_t sequence,const uint8_t *p,uint32_t size) {
    if(!p || !sequence || sequence>15 || size<32 || le16(p+24)<8 || le16(p+24)!=size-24)return MT2_INVALID;
    return p[29]!=sequence || (p[30]&4u)?MT2_PENDING:MT2_OK;
}
int mt2_hw_type_response(uint8_t seq,const uint8_t *p,uint32_t size,mt2_hw_type *out) {
    int e;if(!out)return MT2_INVALID;e=frame(seq,p,size);if(e)return e;
    if(size!=60)return MT2_INVALID;
    if(le32(p+36)!=0x550)return MT2_PENDING;
    if(!le32(p+40))return MT2_PROTOCOL;
    out->encapsulation=p[55]&1u;out->valid_raw=le32(p+40);out->event_id=p[28];return 0;
}
int mt2_clc_response(uint8_t seq,const uint8_t *p,uint32_t size,mt2_clc_reply *out) {
    int e;if(!out)return MT2_INVALID;e=frame(seq,p,size);if(e)return e;
    if(size!=108)return MT2_INVALID;
    out->channel_config=p[44];out->event_tag=le16(p+40);out->event_length=le16(p+42);return 0;
}
static int valid_policy(const mt2_clc_policy *p) {
    return p && p->environment<=2 && p->acpi_known==1 && p->acpi_flags<=7 && p->mtcl_config<=255 &&
        p->world_mtcl_config<=255 && p->power_type<=4 &&
        ((p->alpha2[0]=='0' && p->alpha2[1]=='0') ||
         (p->alpha2[0]>='A' && p->alpha2[0]<='Z' && p->alpha2[1]>='A' && p->alpha2[1]<='Z'));
}
static int match(const uint8_t *r,const uint8_t *alpha){return r[0]==alpha[0] && r[1]==alpha[1];}
static int validate_view(mt2_clc_view *v,const mt2_clc_policy *p,uint32_t capacity) {
    uint32_t at=16,exact=0,world=0;const uint8_t *b=v->block;
    if(b[5]!=1)return MT2_INVALID;
    while(at<v->length) {
        const uint8_t *r=b+at;uint32_t n;
        /* Pinned MT7921 processes while remaining >16; this implementation
         * rejects unexplained tails instead of silently ignoring them. */
        if(v->length-at<=16)return MT2_INVALID;
        n=le16(r+4);
        if(!n || n>v->length-at-6)return MT2_INVALID;
        if(match(r,p->alpha2))++exact;
        if(r[0]=='0' && r[1]=='0')++world;
        at+=6+n;
    }
    if(!exact && !world)return MT2_INVALID;
    v->fallback=!exact;v->matches=exact?exact:world;
    for(at=16;at<v->length;) {
        const uint8_t *r=b+at;uint32_t n=le16(r+4);
        if((v->fallback?(r[0]=='0' && r[1]=='0'):match(r,p->alpha2)) &&
           (n>MT2_CLC_MAX_PAYLOAD-MT2_CLC_HEADER || n>capacity-MT2_CLC_HEADER))return MT2_INVALID;
        at+=6+n;
    }
    return 0;
}
int mt2_clc_plan_build(const uint8_t *ram,uint32_t size,uint32_t hw,const mt2_clc_policy *p,uint32_t cap,mt2_clc_plan *out) {
    mt2_clc_plan plan={0};mt2_fw_plan fw;uint32_t i,table,regions=0;
    if(!out || hw>1 || !valid_policy(p) || cap<MT2_CLC_HEADER || mt2_fw_validate(ram,size,MT2_FW_RAM,&fw))return MT2_INVALID;
    table=size-36-fw.count*40;
    for(i=0;i<fw.count;++i) {
        const mt2_fw_region *r=&fw.regions[i];uint32_t at=0;
        if(!r->skip || ram[table+i*40+25]!=2)continue;
        if(regions++ || (ram[table+i*40+24]&1u))return MT2_INVALID;
        while(at<r->length) {
            const uint8_t *b=ram+r->offset+at;uint32_t n,index;
            if(r->length-at<16)return MT2_INVALID;n=le32(b);index=b[4];
            if(n<16 || n>r->length-at || index>=4)return MT2_INVALID;
            if(index<3 && !plan.views[index].block && (index || (b[7]&1u)==hw)) {
                plan.views[index].block=b;plan.views[index].length=n;
            }
            at+=n;
        }
    }
    if(!plan.views[0].block)return MT2_INVALID;
    for(i=0;i<3;++i) {
        mt2_clc_view *v=&plan.views[i];int e;
        if(!v->block)continue;e=validate_view(v,p,cap);if(e)return e;
        plan.selected_mask|=1u<<i;if(v->fallback)plan.fallback_mask|=1u<<i;
        plan.requests+=v->matches;
    }
    *out=plan;return 0;
}
int mt2_clc_request(const mt2_clc_plan *plan,const mt2_clc_policy *p,uint32_t ordinal,uint32_t events,uint8_t *out,uint32_t cap) {
    uint32_t i,at,j;
    if(!plan || !out || !valid_policy(p) || ordinal>=plan->requests || events>1 || cap<MT2_CLC_HEADER)return MT2_INVALID;
    for(i=0;i<3;++i) {
        const mt2_clc_view *v=&plan->views[i];
        if(!v->block)continue;
        for(at=16;at<v->length;) {
            const uint8_t *r=v->block+at;uint32_t n=le16(r+4);
            if(v->fallback?(r[0]=='0' && r[1]=='0'):match(r,p->alpha2)) {
                if(!ordinal) {
                    if(n>MT2_CLC_MAX_PAYLOAD-MT2_CLC_HEADER || n>cap-MT2_CLC_HEADER)return MT2_INVALID;
                    for(j=0;j<MT2_CLC_HEADER;++j)out[j]=0;
                    out[0]=1;put16(out+2,MT2_CLC_HEADER+n);out[4]=(uint8_t)i;
                    out[5]=(uint8_t)(v->fallback?1:p->environment);out[6]=(uint8_t)p->acpi_flags;out[7]=(uint8_t)events;
                    out[8]=r[0];out[9]=r[1];out[10]=r[2];out[11]=r[3];out[12]=(uint8_t)p->power_type;
                    out[13]=(uint8_t)(v->fallback?p->world_mtcl_config:p->mtcl_config);
                    for(j=0;j<n;++j)out[MT2_CLC_HEADER+j]=r[6+j];
                    return (int)(MT2_CLC_HEADER+n);
                }
                --ordinal;
            }
            at+=6+n;
        }
    }
    return MT2_INVALID;
}
