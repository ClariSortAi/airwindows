/* SPDX-License-Identifier: BSD-3-Clause-Clear */
#include "raw_rx.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"RAW RX FAIL line %d: %s\n",__LINE__,#x);exit(1);}}while(0)
static uint8_t packet[17000],output[17000],saved[17000];
static void put16(uint8_t *p,uint32_t v){p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);}
static void put32(uint8_t *p,uint32_t v){put16(p,v);put16(p+2,v>>16);}
static uint32_t get16(const uint8_t *p){return p[0]|(uint32_t)p[1]<<8;}
/* Minimal action frame with Apple's AWDL OUI, all values synthetic. */
static const uint8_t frame[29]={0xd0,0,0,0,0xff,0xff,0xff,0xff,0xff,0xff,
    2,0x11,0x22,0x33,0x44,0x55,0,0x25,0,0xff,0x94,0x73,0x10,0,127,0,0x17,0xf2,8};
uint32_t test_raw_packet(uint8_t *p) {
    memset(p,0,53);put32(p,0x10000035);put32(p+12,6u<<8);memcpy(p+24,frame,29);return 53;
}
static void reject(uint32_t size,uint32_t fcs,int result,uint32_t expected) {
    uint32_t length=0x12345678,outcome=99;int e;
    memset(output,0x55,sizeof(output));memcpy(saved,output,sizeof(output));
    e=mt2_raw_rx_record(packet,size,fcs,output,sizeof(output),&length,&outcome);
    CHECK(e==result && outcome==expected && length==0x12345678 && !memcmp(saved,output,sizeof(output)));
}
void test_raw_rx(void) {
    uint32_t size,length,outcome,i,j,groups,offset,pad,d1,fc,header;
    static const uint32_t bits[5]={14,11,12,13,15},bytes[5]={16,16,8,8,72};
    static const uint8_t prefix[22]={2,0,0,0,43,0,0,0,0,0,14,0,10,0,0,0,0,0,0x85,9,0x80,0};
    size=test_raw_packet(packet);
    CHECK(mt2_raw_rx_record(packet,size,0,output,sizeof(output),&length,&outcome)==0);
    CHECK(length==51 && outcome==MT2_RX_ACCEPT && !memcmp(output,prefix,22) && !memcmp(output+22,frame,29));
    for(i=0;i<size;++i)reject(i,0,MT2_INVALID,MT2_RX_MALFORMED);
    length=99;memset(output,0x55,sizeof(output));
    CHECK(mt2_raw_rx_record(packet,size,0,output,50,&length,&outcome)==MT2_FULL && length==99 && outcome==MT2_RX_CAPACITY && output[0]==0x55);
    /* Connac2's complete group set occupies 144 bytes before header padding,
     * not Connac3's 192-byte all-group layout. Exercise every legal combination. */
    for(groups=0;groups<32;++groups)for(pad=0;pad<4;++pad) {
        memset(packet,0,sizeof(packet));offset=24;d1=0;
        for(j=0;j<5;++j)if(groups&(1u<<j)){d1|=1u<<bits[j];offset+=bytes[j];}
        offset+=2*pad;size=offset+29;
        put32(packet,0x10000000|size);put32(packet+4,d1);put32(packet+8,pad<<14);put32(packet+12,6u<<8);
        memcpy(packet+offset,frame,29);
        if((groups&16u) && !(groups&8u)){reject(size,0,MT2_INVALID,MT2_RX_MALFORMED);continue;}
        CHECK(mt2_raw_rx_record(packet,size,0,output,sizeof(output),&length,&outcome)==0);
        CHECK(length==51 && !memcmp(output+22,frame,29));
        if(groups==31 && pad==3)CHECK(offset==150);
    }
    size=test_raw_packet(packet);put32(packet+12,44u<<8);
    CHECK(mt2_raw_rx_record(packet,size,0,output,sizeof(output),&length,&outcome)==0 && get16(output+18)==5220 && get16(output+20)==0x100);
    put32(packet+12,181u<<8);
    CHECK(mt2_raw_rx_record(packet,size,0,output,sizeof(output),&length,&outcome)==0 && get16(output+18)==5955 && !get16(output+20));
    put32(packet+12,239u<<8);
    CHECK(mt2_raw_rx_record(packet,size,0,output,sizeof(output),&length,&outcome)==0 && get16(output+18)==7115);
    put32(packet+12,240u<<8);reject(size,0,MT2_INVALID,MT2_RX_MALFORMED);
    put32(packet+12,0);reject(size,0,MT2_INVALID,MT2_RX_MALFORMED);
    size=test_raw_packet(packet);put32(packet,0x40000000|size);
    CHECK(mt2_raw_rx_record(packet,size,0,output,sizeof(output),&length,&outcome)==0);
    put32(packet,0x38010000|size);
    CHECK(mt2_raw_rx_record(packet,size,0,output,sizeof(output),&length,&outcome)==0);
    put32(packet,0x38000000|size);reject(size,0,MT2_PENDING,MT2_RX_OTHER);
    size=test_raw_packet(packet);put32(packet+8,0x2000);reject(size,0,MT2_PENDING,MT2_RX_TRANSLATED);
    size=test_raw_packet(packet);put32(packet+4,1u<<16);reject(size,0,MT2_PENDING,MT2_RX_CIPHER);
    size=test_raw_packet(packet);packet[25]|=0x40;reject(size,0,MT2_PENDING,MT2_RX_PROTECTED);
    size=test_raw_packet(packet);packet[24]=0xd4;reject(size,0,MT2_PENDING,MT2_RX_CONTROL);
    for(i=25;i<=28;++i){size=test_raw_packet(packet);put32(packet+4,1u<<i);reject(size,0,MT2_PENDING,MT2_RX_AIR_ERROR);}
    size=test_raw_packet(packet);put32(packet+8,0x01800000);reject(size,0,MT2_PENDING,MT2_RX_AIR_ERROR);
    /* Explicit FCS presence is preserved in the radiotap flag and payload. */
    size=test_raw_packet(packet)+4;put32(packet,0x10000000|size);put32(packet+size-4,0x12345678);
    CHECK(mt2_raw_rx_record(packet,size,1,output,sizeof(output),&length,&outcome)==0);
    CHECK(length==55 && output[16]==0x10 && !memcmp(output+length-4,packet+size-4,4));
    reject(size,2,MT2_INVALID,MT2_RX_MALFORMED);
    /* Aggregation adds two bytes after the true MAC header, including 4-address
     * QoS frames with HT control. Only that padding is removed. */
    for(i=0;i<4;++i) {
        memset(packet,0,sizeof(packet));fc=0x88;header=26;
        if(i&1u){fc|=0x300;header+=6;}if(i&2u){fc|=0x8000;header+=4;}
        size=24+header+2+5;put32(packet,0x10000000|size);put32(packet+12,6u<<8);put32(packet+16,3);
        put16(packet+24,fc);memset(packet+24+header,0xee,2);memcpy(packet+26+header,"HELLO",5);
        CHECK(mt2_raw_rx_record(packet,size,0,output,sizeof(output),&length,&outcome)==0);
        CHECK(length==22+header+5 && !memcmp(output+22+header,"HELLO",5));
    }
    size=test_raw_packet(packet);put32(packet+16,1);reject(size,0,MT2_INVALID,MT2_RX_MALFORMED);
    /* Large forged receive lengths cannot exceed radio_link's payload bound. */
    size=test_raw_packet(packet);put32(packet,0x10000000|16999u);reject(16999,0,MT2_INVALID,MT2_RX_MALFORMED);
    puts("{\"raw_receive_tests\":\"passed\",\"records\":\"synthetic_connac2\",\"device_access\":false}");
}
