/* SPDX-License-Identifier: BSD-3-Clause-Clear */
#include "nic_caps.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"NIC FAIL line %d: %s\n",__LINE__,#x);exit(1);}}while(0)
static void put16(uint8_t *p,uint32_t x){p[0]=(uint8_t)x;p[1]=(uint8_t)(x>>8);}
static void put32(uint8_t *p,uint32_t x){put16(p,x);put16(p+2,x>>16);}
/* Deliberately unaligned entries, including a tag whose low 16 bits equal MAC.
 * This is a synthetic fixture, not a captured response from the user's adapter. */
uint32_t test_nic_payload(uint8_t *p) {
    static const uint8_t mac[6]={2,0x11,0x22,0x33,0x44,0x55};
    memset(p,0,74);put16(p,5);
    put32(p+4,7);put32(p+8,6);memcpy(p+12,mac,6);
    put32(p+18,8);put32(p+22,12);
    p[26]=1;p[27]=1;p[29]=3;p[30]=2;p[36]=3;p[37]=1;
    put32(p+38,0x18);put32(p+42,1);p[46]=1;
    put32(p+47,0x20);put32(p+51,8);put32(p+55,0x89abcdefu);put32(p+59,0x01234567);
    put32(p+63,0x10000007);put32(p+67,3);p[71]=7;p[72]=8;p[73]=9;
    return 74;
}
static void unchanged_parse(const uint8_t *p,uint32_t bytes) {
    mt2_nic_caps c,before;memset(&c,0xa5,sizeof(c));before=c;
    CHECK(mt2_nic_parse(p,bytes,&c)==MT2_INVALID && !memcmp(&c,&before,sizeof(c)));
}
static void unchanged_response(uint8_t seq,const uint8_t *p,uint32_t bytes,int expected) {
    mt2_nic_caps c,before;memset(&c,0xa5,sizeof(c));before=c;
    CHECK(mt2_nic_response(seq,p,bytes,&c)==expected && !memcmp(&c,&before,sizeof(c)));
}
void test_nic_caps(void) {
    uint8_t p[128],event[256],command[64],seq=0;uint32_t size,i;mt2_nic_caps c;
    size=test_nic_payload(p);CHECK(mt2_nic_parse(p,size,&c)==0);
    CHECK(c.present==15 && c.bands==7 && c.nss==2 && c.chainmask==3 && c.max_bandwidth==3);
    CHECK(c.mac[0]==2 && c.mac[5]==0x55 && c.ht==1 && c.vht==1 && c.he==1);
    CHECK(c.chip_low==0x89abcdefu && c.chip_high==0x01234567);
    for(i=0;i<size;++i)unchanged_parse(p,i);
    p[size]=0;unchanged_parse(p,size+1);
    put16(p,0xffff);unchanged_parse(p,size);test_nic_payload(p);
    put32(p+8,0xffffffffu);unchanged_parse(p,size);test_nic_payload(p);
    put32(p+22,11);unchanged_parse(p,size);test_nic_payload(p);
    p[30]=0;unchanged_parse(p,size);p[30]=3;unchanged_parse(p,size);test_nic_payload(p);
    p[12]=1;unchanged_parse(p,size);test_nic_payload(p);
    memset(p+12,0,6);unchanged_parse(p,size);test_nic_payload(p);
    memcpy(p+size,p+4,14);put16(p,6);unchanged_parse(p,size+14);test_nic_payload(p);
    /* Optional fields stay absent, rather than being inferred from model name. */
    put16(p,2);CHECK(mt2_nic_parse(p,38,&c)==0 && c.present==3 && c.bands==3 && !c.chip_low);
    p[30]=1;CHECK(mt2_nic_parse(p,38,&c)==0 && c.chainmask==1);
    p[36]=0;unchanged_parse(p,38);test_nic_payload(p);
    /* 6 GHz capability before PHY must survive PHY parsing. */
    memcpy(event,p,4);memcpy(event+4,p+38,9);memcpy(event+13,p+4,34);put16(event,3);
    CHECK(mt2_nic_parse(event,47,&c)==0 && c.bands==7 && c.present==7);
    /* Connac3's 16-bit tag/total-length header is not accepted. */
    memset(event,0,40);put16(event,2);put16(event+4,7);put16(event+6,10);
    memcpy(event+8,p+12,6);put16(event+14,8);put16(event+16,17);
    memcpy(event+18,p+26,12);unchanged_parse(event,31);
    CHECK(mt2_mcu_encode(MT2_NIC_COMMAND,&seq,NULL,0,command,sizeof(command))==64);
    CHECK(seq==1 && command[36]==0x8a && command[38]==1 && command[35]==0x80);
    memset(event,0,sizeof(event));size=test_nic_payload(event+36)+36;
    put32(event,0x38000000u|size);put16(event+24,size-24);put16(event+26,0xe000);
    event[28]=0xec;event[29]=7; /* Synthetic EID; parser follows upstream sequence routing. */
    CHECK(mt2_nic_response(7,event,size,&c)==0 && c.bands==7);
    for(i=0;i<size;++i)unchanged_response(7,event,i,MT2_INVALID);
    unchanged_response(6,event,size,MT2_PENDING);
    event[30]=4;unchanged_response(7,event,size,MT2_PENDING);event[30]=0;
    event[28]=0x27;unchanged_response(7,event,size,MT2_PENDING);event[28]=0xec;
    event[32]=0x87;unchanged_response(7,event,size,MT2_PENDING);
    event[28]=6;CHECK(mt2_nic_response(7,event,size,&c)==0);event[32]=0;event[28]=0xec;
    event[2]|=1;unchanged_response(7,event,size,MT2_PENDING);event[2]&=0xfe;
    put16(event+24,size-25);unchanged_response(7,event,size,MT2_INVALID);
    puts("{\"nic_capability_tests\":\"passed\",\"responses\":\"synthetic_connac2\",\"device_access\":false}");
}
