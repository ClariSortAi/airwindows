/* SPDX-License-Identifier: BSD-3-Clause-Clear
 * Copyright (C) 2020, 2021 MediaTek Inc.
 * Connac2 capability layout/routing: pinned mt7921/mcu.c, mcu.h and
 * mt76_connac_mcu.h. Atomic bounds validation is a native-port addition.
 */
#include "nic_caps.h"
static uint32_t le16(const uint8_t *p){return p[0]|(uint32_t)p[1]<<8;}
static uint32_t le32(const uint8_t *p){return le16(p)|le16(p+2)<<16;}
int mt2_nic_parse(const uint8_t *p,uint32_t size,mt2_nic_caps *out) {
    mt2_nic_caps c={0};uint32_t count,offset=4,i,j,tag,bytes,bit,any;
    const uint8_t *v;
    if(!p || !out || size<4)return MT2_INVALID;
    count=le16(p);if(count>(size-4)/8)return MT2_INVALID;
    for(i=0;i<count;++i) {
        if(size-offset<8)return MT2_INVALID;
        tag=le32(p+offset);bytes=le32(p+offset+4);offset+=8;
        if(bytes>size-offset)return MT2_INVALID;
        v=p+offset;
        bit=tag==7?MT2_CAP_MAC:tag==8?MT2_CAP_PHY:tag==0x18?MT2_CAP_6GHZ:tag==0x20?MT2_CAP_CHIP:0;
        if(c.present&bit)return MT2_INVALID;
        if(tag==7) {
            if(bytes<6 || (v[0]&1u))return MT2_INVALID;
            any=0;for(j=0;j<6;++j){c.mac[j]=v[j];any|=v[j];}
            if(!any)return MT2_INVALID;
        } else if(tag==8) {
            if(bytes<12 || !v[4] || v[4]>2)return MT2_INVALID;
            c.nss=v[4];c.chainmask=(1u<<c.nss)-1;
            c.max_bandwidth=v[3];c.ht=v[0];c.vht=v[1];c.he=v[11];c.hw_path=v[10];
            c.bands|=c.hw_path&3u;
        } else if(tag==0x18) {
            if(!bytes)return MT2_INVALID;
            if(v[0])c.bands|=4;
        } else if(tag==0x20) {
            if(bytes<8)return MT2_INVALID;
            c.chip_low=le32(v);c.chip_high=le32(v+4);
        }
        c.present|=bit;offset+=bytes;
    }
    if(offset!=size || (c.present&3u)!=3u || !c.bands)return MT2_INVALID;
    *out=c;return 0;
}
static int unsolicited(uint8_t eid,uint8_t ext) {
    if(eid==6)return 0; /* Explicit solicited exception in mt7921_mcu_rx_event. */
    return ext==0x87 || eid==0x13 || eid==0x23 || eid==0x96 || eid==0x0d ||
        eid==0x0f || eid==0x27 || eid==0xf0 || eid==7;
}
int mt2_nic_response(uint8_t sequence,const uint8_t *p,uint32_t size,mt2_nic_caps *out) {
    uint32_t word,declared,total;
    if(!p || !out || !sequence || sequence>15 || size<36)return MT2_INVALID;
    word=le32(p);total=word&0xffffu;
    if((word>>27)!=7 || ((word>>16)&15u)==1)return MT2_PENDING;
    if(total<36 || total>size)return MT2_INVALID;
    declared=le16(p+24);
    if(declared<12 || declared!=total-24)return MT2_INVALID;
    if(p[29]!=sequence || (p[30]&4u) || unsolicited(p[28],p[32]))return MT2_PENDING;
    return mt2_nic_parse(p+36,declared-12,out);
}
