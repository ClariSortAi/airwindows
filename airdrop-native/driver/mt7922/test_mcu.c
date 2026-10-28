/* SPDX-License-Identifier: BSD-3-Clause-Clear */
#include "mcu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"MCU FAIL line %d: %s\n",__LINE__,#x);exit(1);}}while(0)
static void put32(uint8_t *p,uint32_t v){p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);p[2]=(uint8_t)(v>>16);p[3]=(uint8_t)(v>>24);}
static uint8_t out[MT2_MCU_MAX_PACKET],payload[MT2_MCU_MAX_PACKET];
void test_mcu(void) {
    uint8_t expected[68]={0},seq=0,event[64]={0},before[68];uint32_t status,i;
    const uint8_t body[4]={1,2,3,4};
    /* Golden legacy packet, including the LONG_FORMAT bit missing from the
     * MT7925 header, fixed command routing and no USB prefix. */
    put32(expected,0x41000044);put32(expected+4,0x80010000);
    expected[32]=36;expected[35]=0x80;expected[36]=0x10;
    expected[37]=0xa0;expected[38]=3;expected[39]=1;
    memcpy(expected+64,body,4);
    CHECK(mt2_mcu_encode(0x10,&seq,body,4,out,sizeof(out))==68 && seq==1);
    CHECK(!memcmp(out,expected,68));
    memset(expected,0,sizeof(expected));put32(expected,0x41000034);put32(expected+4,0x80010000);
    expected[32]=20;expected[34]=2;expected[37]=0xa0;expected[39]=2;expected[43]=7;
    memcpy(expected+48,body,4);
    CHECK(mt2_mcu_encode(MT2_MCU_UNI|2,&seq,body,4,out,sizeof(out))==52 && seq==2);
    CHECK(!memcmp(out,expected,52));
    CHECK(mt2_mcu_encode(MT2_MCU_QUERY|MT2_MCU_CE|0xc0,&seq,body,4,out,sizeof(out))==68);
    CHECK(out[36]==0xc0 && out[38]==0 && out[41]==0 && out[43]==0);
    CHECK(mt2_mcu_encode(MT2_MCU_WA|0x2ced,&seq,body,4,out,sizeof(out))==68);
    CHECK(out[36]==0xed && out[38]==1 && out[41]==0x2c && out[42]==2 && out[43]==1);
    seq=15;CHECK(mt2_mcu_encode(MT2_FW_SCATTER,&seq,body,4,out,sizeof(out))==4 && seq==1);
    CHECK(!memcmp(out,body,4));
    memset(out,0xa5,68);memcpy(before,out,68);seq=9;
    CHECK(mt2_mcu_encode(MT2_MCU_UNI|MT2_MCU_QUERY|2,&seq,body,4,out,sizeof(out))==MT2_INVALID);
    CHECK(mt2_mcu_encode(1,&seq,body,4,out,67)==MT2_INVALID);
    CHECK(mt2_mcu_encode(1,&seq,NULL,4,out,sizeof(out))==MT2_INVALID);
    CHECK(mt2_mcu_encode(MT2_FW_SCATTER,&seq,NULL,0,out,sizeof(out))==MT2_INVALID);
    CHECK(mt2_mcu_encode(0x80000001,&seq,body,4,out,sizeof(out))==MT2_INVALID);
    CHECK(seq==9 && !memcmp(out,before,68));
    CHECK(mt2_mcu_encode(1,&seq,payload,MT2_MCU_MAX_PACKET-64,out,sizeof(out))==MT2_MCU_MAX_PACKET);
    CHECK(mt2_mcu_encode(1,&seq,payload,MT2_MCU_MAX_PACKET-63,out,sizeof(out))==MT2_INVALID);
    /* Patch semaphore status lives at 32, not the Connac3 offset 40. */
    event[24]=9;event[28]=4;event[29]=3;event[32]=2;status=99;
    CHECK(mt2_mcu_response(0x10,3,event,33,&status)==0 && status==2);
    event[24]=20;event[28]=1;event[36]=2;put32(event+40,0x12345678);status=99;
    CHECK(mt2_mcu_response(MT2_MCU_UNI|2,3,event,44,&status)==0 && status==0x12345678);
    status=99;event[36]=1;CHECK(mt2_mcu_response(MT2_MCU_UNI|2,3,event,44,&status)==MT2_PENDING);
    event[36]=2;event[29]=4;CHECK(mt2_mcu_response(MT2_MCU_UNI|2,3,event,44,&status)==MT2_PENDING);
    event[29]=3;event[30]=4;CHECK(mt2_mcu_response(MT2_MCU_UNI|2,3,event,44,&status)==MT2_PENDING);
    event[30]=0;event[28]=0x27;CHECK(mt2_mcu_response(MT2_MCU_UNI|2,3,event,44,&status)==MT2_PENDING);
    CHECK(status==99);event[28]=1;
    for(i=0;i<44;++i) CHECK(mt2_mcu_response(MT2_MCU_UNI|2,3,event,i,&status)==MT2_INVALID);
    CHECK(status==99);
    event[24]=12;CHECK(mt2_mcu_response(1,3,event,36,&status)==0 && status==0);
    status=99;CHECK(mt2_mcu_response(0x8a,3,event,36,&status)==MT2_INVALID && status==99);
    event[24]=255;CHECK(mt2_mcu_response(1,3,event,36,&status)==MT2_INVALID && status==99);
    /* Fixed extended command ACKs have no established status/EID semantics.
     * Synthetic EIDs below exercise matching, not captured firmware behavior. */
    memset(event,0,sizeof(event));event[24]=12;event[28]=0xed;event[29]=3;
    CHECK(mt2_mcu_response(MT2_MCU_EFUSE_MODE,3,event,36,&status)==0 && !status);
    event[28]=1;CHECK(mt2_mcu_response(MT2_MCU_PROTECT,3,event,36,&status)==0);
    status=99;event[29]=4;
    CHECK(mt2_mcu_response(MT2_MCU_EFUSE_MODE,3,event,36,&status)==MT2_PENDING && status==99);
    event[29]=3;event[30]=4;
    CHECK(mt2_mcu_response(MT2_MCU_PROTECT,3,event,36,&status)==MT2_PENDING && status==99);
    event[30]=0;
    for(i=0;i<36;++i)CHECK(mt2_mcu_response(MT2_MCU_PROTECT,3,event,i,&status)==MT2_INVALID && status==99);
    event[24]=11;CHECK(mt2_mcu_response(MT2_MCU_EFUSE_MODE,3,event,36,&status)==MT2_INVALID);
    event[24]=12;CHECK(mt2_mcu_response(0x22ed,3,event,36,&status)==MT2_INVALID);
    /* SNIFFER is UNI on transmit but falls through to header-only ACK in
     * mt7921_mcu_parse_response. It must accept 36 bytes, not require 44. */
    event[28]=0xec;CHECK(mt2_mcu_response(MT2_MCU_SNIFFER,3,event,36,&status)==0 && !status);
    status=99;event[29]=4;CHECK(mt2_mcu_response(MT2_MCU_SNIFFER,3,event,36,&status)==MT2_PENDING && status==99);event[29]=3;
    for(i=0;i<36;++i)CHECK(mt2_mcu_response(MT2_MCU_SNIFFER,3,event,i,&status)==MT2_INVALID && status==99);
    event[24]=20;put32(event+40,7);CHECK(mt2_mcu_response(MT2_MCU_SNIFFER,3,event,44,&status)==0 && !status);
    puts("{\"mcu_tests\":\"passed\",\"device_access\":false}");
}
