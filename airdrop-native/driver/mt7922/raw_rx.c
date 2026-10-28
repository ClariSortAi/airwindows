/* SPDX-License-Identifier: BSD-3-Clause-Clear
 * Copyright (C) 2020, 2021, 2023 MediaTek Inc.
 * Connac2 RXD layout: pinned mt7921/mac.c, mt76_connac2_mac.h, mt792x.h.
 * Radio-link serialization adapted from the Windows MT7925 raw_rx.c reference.
 */
#include "raw_rx.h"
static uint32_t le16(const uint8_t *p){return p[0]|(uint32_t)p[1]<<8;}
static uint32_t le32(const uint8_t *p){return le16(p)|le16(p+2)<<16;}
static void put16(uint8_t *p,uint32_t v){p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);}
static void put32(uint8_t *p,uint32_t v){put16(p,v);put16(p+2,v>>16);}
int mt2_raw_rx_record(const uint8_t *p,uint32_t size,uint32_t fcs,uint8_t *out,uint32_t cap,
                       uint32_t *length,uint32_t *outcome) {
    static const uint32_t bits[5]={14,11,12,13,15},group_bytes[5]={16,16,8,8,72};
    uint32_t d0,d1,d2,d3,total,type,offset=24,i,pad,frame,fc,header=24,amsdu;
    uint32_t channel,frequency,flags;
    if(!outcome)return MT2_INVALID;
    *outcome=MT2_RX_MALFORMED;
    if(!p || !out || !length || fcs>1 || size<4)return MT2_INVALID;
    d0=le32(p);total=d0&0xffffu;type=d0>>27;
    if(total<4 || total>size)return MT2_INVALID;
    if(type!=2 && type!=8 && !(type==7 && ((d0>>16)&15u)==1)) {
        *outcome=MT2_RX_OTHER;return MT2_PENDING;
    }
    if(total<24)return MT2_INVALID;
    d1=le32(p+4);d2=le32(p+8);d3=le32(p+12);amsdu=le32(p+16)&3u;
    if((d1&0x1e000000u) || (d2&0x01800000u) || (d3&0x40000000u)) {
        *outcome=MT2_RX_AIR_ERROR;return MT2_PENDING;
    }
    if(d2&0x2000u){*outcome=MT2_RX_TRANSLATED;return MT2_PENDING;}
    if(d1&0x001f0000u){*outcome=MT2_RX_CIPHER;return MT2_PENDING;}
    if((d1&0x8000u) && !(d1&0x2000u))return MT2_INVALID;
    for(i=0;i<5;++i)if(d1&(1u<<bits[i])) {
        if(group_bytes[i]>total-offset)return MT2_INVALID;
        offset+=group_bytes[i];
    }
    pad=2*((d2>>14)&3u);if(pad>total-offset)return MT2_INVALID;offset+=pad;
    frame=total-offset;
    if(frame<2+4*fcs)return MT2_INVALID;
    fc=le16(p+offset);
    if(fc&3u)return MT2_INVALID;
    if(fc&0x4000u){*outcome=MT2_RX_PROTECTED;return MT2_PENDING;}
    type=(fc>>2)&3u;
    if(type==1 || type==3){*outcome=MT2_RX_CONTROL;return MT2_PENDING;}
    if(type==2) {
        if((fc&0x300u)==0x300u)header+=6;
        if(fc&0x80u){header+=2;if(fc&0x8000u)header+=4;}
    }
    if(frame<header+4*fcs+(amsdu?2u:0u))return MT2_INVALID;
    if(amsdu && (type!=2 || !(fc&0x80u)))return MT2_INVALID;
    channel=(d3>>8)&255u;
    if(!channel || channel>239)return MT2_INVALID;
    if(channel>180){frequency=5950+5*((channel-181)*4+1);flags=0;}
    else if(channel>14){frequency=5000+5*channel;flags=0x100;}
    else{frequency=channel==14?2484:2407+5*channel;flags=0x80;}
    if(amsdu)frame-=2;
    if(frame>16384u-14)return MT2_INVALID;
    if(cap<frame+22){*outcome=MT2_RX_CAPACITY;return MT2_FULL;}
    for(i=0;i<22;++i)out[i]=0;
    put16(out,2);put32(out+4,frame+14);put16(out+10,14);put32(out+12,10);
    out[16]=(uint8_t)(fcs?0x10:0);put16(out+18,frequency);put16(out+20,flags);
    for(i=0;i<frame;++i)out[22+i]=p[offset+i+(amsdu && i>=header?2u:0u)];
    *length=frame+22;*outcome=MT2_RX_ACCEPT;return 0;
}
