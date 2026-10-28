/* SPDX-License-Identifier: BSD-3-Clause-Clear */
#include "regulatory.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"REGD FAIL line %d: %s\n",__LINE__,#x);exit(1);}}while(0)
static uint32_t le32(const uint8_t *p){return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static void put16(uint8_t *p,uint32_t x){p[0]=(uint8_t)x;p[1]=(uint8_t)(x>>8);}
static void put32(uint8_t *p,uint32_t x){put16(p,x);put16(p+2,x>>16);}
uint32_t test_regd_image(uint8_t *p) {
    uint32_t i;memset(p,0,244);
    put32(p,84);p[4]=3;p[5]=1;p[6]=0xfe;
    p[16]='U';p[17]='S';p[18]=1;put32(p+20,1);p[24]=1;
    p[32]='U';p[33]='S';p[34]=1;
    for(i=48;i<84;++i)p[i]=(uint8_t)i;
    /* A second hardware variant must not silently replace the first block. */
    memcpy(p+84,p,84);p[91]=1;p[132]^=255;
    put32(p+188,168);p[192]=0x40;p[193]=2;p[210]=1;return 244;
}
uint32_t test_regd_event(uint8_t *p,uint8_t seq) {
    memset(p,0,96);put32(p,0x38000060);put16(p+24,72);put16(p+26,0xe000);p[28]=0xef;p[29]=seq;
    put16(p+40,7);put16(p+42,32);put32(p+44,1);p[48]=1;
    put32(p+64,2402);put32(p+68,2482);put32(p+72,40);put32(p+76,0x98765432);put32(p+80,0x8000);
    return 96;
}
static void bad_request(const uint8_t *p,uint32_t n,const uint8_t *alpha,uint32_t capacity) {
    uint8_t out[MT2_REGD_REQUEST_MAX],before[MT2_REGD_REQUEST_MAX];
    memset(out,0xa5,sizeof(out));memcpy(before,out,sizeof(out));
    CHECK(mt2_regd_request(p,n,alpha,out,capacity)==MT2_INVALID && !memcmp(out,before,sizeof(out)));
}
static void response_result(const uint8_t *p,uint32_t n,int expected) {
    mt2_regdomain out,before;memset(&out,0xa5,sizeof(out));before=out;
    CHECK(mt2_regd_response(3,p,n,&out)==expected && !memcmp(&out,&before,sizeof(out)));
}
void test_regulatory(void) {
    uint8_t ram[244],out[MT2_REGD_REQUEST_MAX],expected[132]={0},event[128];
    const uint8_t alpha[2]={'U','S'};mt2_regdomain d;uint32_t i;
    CHECK(test_regd_image(ram)==sizeof(ram));
    expected[0]=1;expected[1]=1;put32(expected+4,52);expected[8]='U';expected[9]='S';put32(expected+12,1);
    memcpy(expected+80,ram+32,52);
    CHECK(mt2_regd_request(ram,244,alpha,out,sizeof(out))==132 && !memcmp(out,expected,132));
    for(i=0;i<244;++i)bad_request(ram,i,alpha,sizeof(out));
    for(i=0;i<132;++i)bad_request(ram,244,alpha,i);
    bad_request(ram,244,(const uint8_t *)"us",sizeof(out));bad_request(ram,244,(const uint8_t *)"CA",sizeof(out));
    put32(ram,0xffffffffu);bad_request(ram,244,alpha,sizeof(out));test_regd_image(ram);
    put32(ram+84,15);bad_request(ram,244,alpha,sizeof(out));test_regd_image(ram);
    ram[88]=4;bad_request(ram,244,alpha,sizeof(out));test_regd_image(ram);
    ram[5]=2;bad_request(ram,244,alpha,sizeof(out));test_regd_image(ram);
    put32(ram+20,129);bad_request(ram,244,alpha,sizeof(out));test_regd_image(ram);
    ram[24]=2;bad_request(ram,244,alpha,sizeof(out));test_regd_image(ram);
    ram[32]='C';bad_request(ram,244,alpha,sizeof(out));test_regd_image(ram);
    ram[192]|=1;bad_request(ram,244,alpha,sizeof(out));test_regd_image(ram);
    /* A later valid country cannot substitute for a missing first-block country. */
    ram[16]='C';ram[17]='A';ram[32]='C';ram[33]='A';bad_request(ram,244,alpha,sizeof(out));
    test_regd_event(event,3);CHECK(mt2_regd_response(3,event,96,&d)==0);
    CHECK(d.count==1 && d.dfs_region==1 && d.event_tag==7 && d.event_length==32);
    CHECK(d.rules[0].start_mhz==2402 && d.rules[0].end_mhz==2482 && d.rules[0].bandwidth_mhz==40);
    CHECK(d.rules[0].eirp_raw==0x98765432 && d.rules[0].flags==0x8000 && !d.rules[127].start_mhz);
    for(i=0;i<96;++i)response_result(event,i,MT2_INVALID);
    response_result(event,97,MT2_INVALID);
    event[29]=4;response_result(event,96,MT2_PENDING);event[29]=3;event[30]=4;response_result(event,96,MT2_PENDING);event[30]=0;
    put32(event+44,0xffffffffu);response_result(event,96,MT2_INVALID);test_regd_event(event,3);
    event[48]=4;response_result(event,96,MT2_INVALID);test_regd_event(event,3);
    put32(event+68,2402);response_result(event,96,MT2_INVALID);test_regd_event(event,3);
    put32(event+72,321);response_result(event,96,MT2_INVALID);test_regd_event(event,3);
    event[24]=71;response_result(event,96,MT2_INVALID);
    puts("{\"regulatory_codec_tests\":\"passed\",\"firmware_responses\":\"synthetic_only\",\"device_access\":false}");
}
void test_regulatory_file(const uint8_t *ram,uint32_t size) {
    mt2_fw_plan plan;const uint8_t *selected=0;uint32_t i,offset,table,total,countries=0,empty=0;
    uint8_t out[MT2_REGD_REQUEST_MAX];
    CHECK(mt2_fw_validate(ram,size,MT2_FW_RAM,&plan)==0);table=size-36-plan.count*40;
    for(i=0;i<plan.count;++i) {
        const mt2_fw_region *r=&plan.regions[i];
        if(!r->skip || ram[table+i*40+25]!=2)continue;
        offset=0;
        while(offset<r->length) {
            const uint8_t *p=ram+r->offset+offset;uint32_t n;
            CHECK(r->length-offset>=16);n=le32(p);CHECK(n>=16 && n<=r->length-offset);
            if(p[4]==3 && !selected)selected=p;
            offset+=n;
        }
    }
    if(!selected) {
        CHECK(mt2_regd_request(ram,size,(const uint8_t *)"US",out,sizeof(out))==MT2_INVALID);
        puts("{\"firmware_regulatory_records\":\"absent_rejected\",\"device_access\":false}");return;
    }
    offset=16;total=le32(selected);
    while(offset<total) {
        const uint8_t *cc=selected+offset;uint32_t n,bytes;int length;
        CHECK(total-offset>=36);n=le32(cc+4);CHECK(n<=128);bytes=36+32*n;
        CHECK(bytes<=total-offset);length=mt2_regd_request(ram,size,cc,out,sizeof(out));
        if(!n){CHECK(length==MT2_INVALID);offset+=bytes;++empty;continue;}
        CHECK(length==(int)(80+bytes-16) && !memcmp(out+80,cc+16,bytes-16));
        CHECK(out[0]==cc[2] && out[1]==cc[8] && out[8]==cc[0] && out[9]==cc[1] && le32(out+12)==n);
        offset+=bytes;++countries;
    }
    printf("{\"firmware_regulatory_records\":\"nonempty_forwarded_byte_exact\",\"countries\":%u,\"empty_records_rejected\":%u,\"signature_validation\":false,\"device_access\":false}\n",countries,empty);
}
