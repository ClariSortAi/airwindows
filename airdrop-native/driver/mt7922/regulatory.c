/* SPDX-License-Identifier: BSD-3-Clause-Clear
 * Copyright (C) 2025 MediaTek Inc.
 * MT7921 regd.c/h and mcu.c from pinned mt76; bounded parsing and atomic outputs
 * adapted from the referenced Windows regulatory module. No device calls.
 */
#include "regulatory.h"
static uint32_t le16(const uint8_t *p){return p[0]|(uint32_t)p[1]<<8;}
static uint32_t le32(const uint8_t *p){return le16(p)|le16(p+2)<<16;}
static void put32(uint8_t *p,uint32_t x){p[0]=(uint8_t)x;p[1]=(uint8_t)(x>>8);p[2]=(uint8_t)(x>>16);p[3]=(uint8_t)(x>>24);}
static int alpha_valid(const uint8_t *a) {
    return a && ((a[0]=='0' && a[1]=='0') || (a[0]>='A' && a[0]<='Z' && a[1]>='A' && a[1]<='Z'));
}
int mt2_regd_request(const uint8_t *ram,uint32_t size,const uint8_t alpha[2],uint8_t *out,uint32_t cap) {
    mt2_fw_plan plan;const uint8_t *selected=0,*country=0;
    uint32_t table,i,regions=0,offset,total,count=0,length=0;
    if(!out || !alpha_valid(alpha) || mt2_fw_validate(ram,size,MT2_FW_RAM,&plan))return MT2_INVALID;
    table=size-36-plan.count*40;
    for(i=0;i<plan.count;++i) {
        const mt2_fw_region *r=&plan.regions[i];
        if(!r->skip || ram[table+i*40+25]!=2)continue;
        if(regions++ || (ram[table+i*40+24]&1u))return MT2_INVALID;
        offset=0;
        while(offset<r->length) {
            const uint8_t *b=ram+r->offset+offset;uint32_t n;
            if(r->length-offset<16)return MT2_INVALID;
            n=le32(b);
            if(n<16 || n>r->length-offset || b[4]>=4)return MT2_INVALID;
            /* mt7921_load_clc retains the first index-3 block. Its hardware
             * encapsulation filter applies only to index-0 power blocks. */
            if(b[4]==3 && !selected)selected=b;
            offset+=n;
        }
    }
    if(!selected || selected[5]!=1)return MT2_INVALID;
    offset=16;total=le32(selected);
    while(offset<total) {
        const uint8_t *cc=selected+offset;uint32_t n,bytes;
        if(total-offset<16)return MT2_INVALID;
        n=le32(cc+4);
        if(n>MT2_REGD_MAX_RULES)return MT2_INVALID;
        bytes=36+n*32; /* country header + rule header + rules + signature */
        if(bytes>total-offset)return MT2_INVALID;
        if(cc[0]==alpha[0] && cc[1]==alpha[1]) {
            if(country || !n || cc[2]!=1 || cc[8]!=1 || cc[16]!=alpha[0] || cc[17]!=alpha[1])return MT2_INVALID;
            country=cc;count=n;length=bytes-16;
        }
        offset+=bytes;
    }
    if(!country || cap<MT2_REGD_REQUEST_HEADER+length)return MT2_INVALID;
    for(i=0;i<MT2_REGD_REQUEST_HEADER;++i)out[i]=0;
    out[0]=country[2];out[1]=country[8];put32(out+4,length);
    out[8]=alpha[0];out[9]=alpha[1];put32(out+12,count);
    for(i=0;i<length;++i)out[MT2_REGD_REQUEST_HEADER+i]=country[16+i];
    return (int)(MT2_REGD_REQUEST_HEADER+length);
}
int mt2_regd_response(uint8_t sequence,const uint8_t *p,uint32_t size,mt2_regdomain *out) {
    uint32_t declared,count,i,offset;
    if(!p || !out || !sequence || sequence>15 || size<32)return MT2_INVALID;
    declared=le16(p+24);
    if(declared<8 || declared!=size-24)return MT2_INVALID;
    if(p[29]!=sequence || (p[30]&4u))return MT2_PENDING;
    if(size<64)return MT2_INVALID;
    count=le32(p+44);
    if(!count || count>MT2_REGD_MAX_RULES || p[48]>3 || size!=64+count*32)return MT2_INVALID;
    for(i=0;i<count;++i) {
        uint32_t start,end,bw;offset=64+i*32;
        start=le32(p+offset);end=le32(p+offset+4);bw=le32(p+offset+8);
        if(!start || start>=end || end>10000 || !bw || bw>320)return MT2_INVALID;
    }
    out->count=count;out->dfs_region=p[48];out->event_tag=le16(p+40);out->event_length=le16(p+42);
    for(i=0;i<MT2_REGD_MAX_RULES;++i) {
        mt2_reg_rule *r=&out->rules[i];offset=64+i*32;
        r->start_mhz=i<count?le32(p+offset):0;r->end_mhz=i<count?le32(p+offset+4):0;
        r->bandwidth_mhz=i<count?le32(p+offset+8):0;r->eirp_raw=i<count?le32(p+offset+12):0;
        r->flags=i<count?le32(p+offset+16):0;
    }
    return MT2_OK;
}
