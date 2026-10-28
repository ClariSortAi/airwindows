/* SPDX-License-Identifier: BSD-3-Clause-Clear
 * Copyright (C) 2020, 2021 MediaTek Inc.
 * Adapted from mt76_connac2_mcu_fill_message and mt7921_mcu_parse_response.
 * Bounds checks and explicit unsupported-command rejection are port additions.
 */
#include "mcu.h"
static uint32_t get16(const uint8_t *p) {return p[0]|(uint32_t)p[1]<<8;}
static uint32_t get32(const uint8_t *p) {return get16(p)|get16(p+2)<<16;}
static void put16(uint8_t *p,uint32_t v) {p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);}
static void put32(uint8_t *p,uint32_t v) {put16(p,v);put16(p+2,v>>16);}
int mt2_mcu_encode(uint32_t cmd,uint8_t *sequence,const uint8_t *payload,
                    uint32_t length,uint8_t *out,uint32_t capacity) {
    uint32_t header,total,i,id=cmd&255u,ext=(cmd>>8)&255u;uint8_t seq;
    if (!sequence || !out || (length&&!payload) || (cmd&~0x1fffffu) ||
        ((cmd&MT2_MCU_UNI) && (cmd&(0xff00u|MT2_MCU_QUERY|MT2_MCU_CE|MT2_MCU_WA)))) return MT2_INVALID;
    header=cmd==MT2_FW_SCATTER?0u:((cmd&MT2_MCU_UNI)?48u:64u);
    if (length>MT2_MCU_MAX_PACKET-header || capacity<length+header ||
        (!header&&!length)) return MT2_INVALID;
    total=length+header;seq=(uint8_t)((*sequence+1u)&15u);if (!seq) seq=1;
    for(i=0;i<header;++i) out[i]=0;
    if(header) {
        put32(out,0x41000000u|total);
        put32(out+4,0x80010000u); /* Connac2 LONG_FORMAT | HDR_FORMAT_CMD. */
        put16(out+32,total-32);out[37]=0xa0;out[39]=seq;
        if(cmd&MT2_MCU_UNI) {put16(out+34,id);out[43]=7;}
        else {
            put16(out+34,0x8000);out[36]=(uint8_t)id;
            out[38]=(ext||(cmd&MT2_MCU_CE))?((cmd&MT2_MCU_QUERY)?0:1):3;
            out[41]=(uint8_t)ext;out[42]=(cmd&MT2_MCU_WA)?2:0;out[43]=ext?1:0;
        }
    }
    for(i=0;i<length;++i) out[header+i]=payload[i];
    *sequence=seq;return (int)total;
}
int mt2_mcu_response(uint32_t cmd,uint8_t sequence,const uint8_t *p,
                      uint32_t size,uint32_t *status) {
    uint32_t declared,id=cmd&255u;int unified=0,patch=0,extended=0;
    if (!p || !status || !sequence || sequence>15 || size<32) return MT2_INVALID;
    if (cmd==(MT2_MCU_UNI|1u) || cmd==(MT2_MCU_UNI|2u) || cmd==(MT2_MCU_UNI|3u) ||
        cmd==(MT2_MCU_UNI|5u) || cmd==(MT2_MCU_UNI|6u) || cmd==(MT2_MCU_UNI|7u)) unified=1;
    else if(cmd==0x10 || cmd==7) patch=1;
    else if(cmd==MT2_MCU_EFUSE_MODE || cmd==MT2_MCU_PROTECT || cmd==MT2_MCU_MAC_ENABLE ||
            cmd==MT2_MCU_RX_PATH || cmd==MT2_MCU_CHANNEL || cmd==MT2_MCU_SNIFFER) extended=1;
    else if(cmd!=1 && cmd!=2 && cmd!=5) return MT2_INVALID;
    declared=get16(p+24);
    if(declared<8 || declared>size-24) return MT2_INVALID;
    if(p[29]!=sequence || (p[30]&4u)) return MT2_PENDING;
    if(extended) {
        /* mt7921_mcu_parse_response consumes the full Connac2 header and
         * returns zero for these commands; no result payload/EID is specified.
         * Do not invent a status field or mistake this ACK for RF readiness. */
        if(declared<12) return MT2_INVALID;
        *status=0;return MT2_OK;
    }
    if(p[28]!=(cmd==0x10?4u:1u)) return MT2_PENDING;
    if(patch) {
        if(declared<9) return MT2_INVALID;
        *status=p[32];return MT2_OK;
    }
    if(unified) {
        if(declared<20) return MT2_INVALID;
        if(p[36]!=id) return MT2_PENDING;
        *status=get32(p+40);return MT2_OK;
    }
    if(declared<12) return MT2_INVALID;
    /* Upstream defines no status payload for these legacy download ACKs.
     * A matched ACK does not establish RAM-ready state; poll that separately. */
    *status=0;return MT2_OK;
}
