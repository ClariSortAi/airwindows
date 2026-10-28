/* SPDX-License-Identifier: BSD-3-Clause-Clear */
#include "clc_power.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"CLC FAIL line %d: %s\n",__LINE__,#x);exit(1);}}while(0)
static uint32_t le16(const uint8_t *p){return p[0]|(uint32_t)p[1]<<8;}
static void put16(uint8_t *p,uint32_t x){p[0]=(uint8_t)x;p[1]=(uint8_t)(x>>8);}
static void put32(uint8_t *p,uint32_t x){put16(p,x);put16(p+2,x>>16);}
uint32_t test_clc_image(uint8_t *p) {
    uint32_t variant,i,j;memset(p,0,240);
    for(variant=0;variant<2;++variant) {
        uint8_t *b=p+variant*82;put32(b,82);b[4]=0;b[5]=1;b[6]=255;b[7]=(uint8_t)variant;
        for(i=0;i<3;++i) {
            uint8_t *r=b+16+i*22;r[0]=(uint8_t)(i==2?'0':'U');r[1]=(uint8_t)(i==2?'0':'S');
            r[2]=(uint8_t)(0x30+i);r[3]=0x2d;put16(r+4,16);
            for(j=0;j<16;++j)r[6+j]=(uint8_t)(variant*128+i*32+j);
        }
    }
    put32(p+184,164);p[188]=0x40;p[189]=2;p[206]=1;return 240;
}
uint32_t test_hw_event(uint8_t *p,uint8_t seq) {
    memset(p,0,60);put32(p,0x3800003c);put16(p+24,36);put16(p+26,0xe000);p[28]=0xe1;p[29]=seq;
    put32(p+36,0x550);put32(p+40,1);p[55]=0xa5;return 60;
}
uint32_t test_clc_event(uint8_t *p,uint8_t seq) {
    memset(p,0,108);put32(p,0x3800006c);put16(p+24,84);put16(p+26,0xe000);p[28]=0xe2;p[29]=seq;
    put16(p+40,1);put16(p+42,68);p[44]=0x1f;return 108;
}
static mt2_clc_policy policy(void) {
    mt2_clc_policy p={{'U','S'},2,1,3,255,0,77};return p;
}
static void bad_plan(const uint8_t *ram,uint32_t size,uint32_t hw,const mt2_clc_policy *p,uint32_t cap) {
    mt2_clc_plan plan,before;memset(&plan,0xa5,sizeof(plan));before=plan;
    CHECK(mt2_clc_plan_build(ram,size,hw,p,cap,&plan)==MT2_INVALID && !memcmp(&plan,&before,sizeof(plan)));
}
void test_clc_power(void) {
    uint8_t ram[240],request[MT2_CLC_MAX_PAYLOAD],expected[92]={0},event[128];
    mt2_clc_policy p=policy();mt2_clc_plan plan;mt2_hw_type hw,before;mt2_clc_reply reply,prior;uint32_t i;
    test_clc_image(ram);CHECK(mt2_clc_plan_build(ram,240,1,&p,sizeof(request),&plan)==0);
    CHECK(plan.selected_mask==1 && !plan.fallback_mask && plan.requests==2);
    expected[0]=1;put16(expected+2,92);expected[5]=2;expected[6]=3;expected[7]=1;expected[8]='U';expected[9]='S';
    expected[10]=0x30;expected[11]=0x2d;expected[13]=255;memcpy(expected+76,ram+104,16);
    CHECK(mt2_clc_request(&plan,&p,0,1,request,sizeof(request))==92 && !memcmp(request,expected,92));
    CHECK(mt2_clc_request(&plan,&p,1,1,request,sizeof(request))==92 && request[10]==0x31 && request[76]==160);
    memset(request,0xa5,sizeof(request));CHECK(mt2_clc_request(&plan,&p,2,1,request,sizeof(request))==MT2_INVALID && request[0]==0xa5);
    CHECK(mt2_clc_request(&plan,&p,0,1,request,91)==MT2_INVALID && request[0]==0xa5);
    p.alpha2[0]='Z';p.alpha2[1]='Z';CHECK(mt2_clc_plan_build(ram,240,0,&p,sizeof(request),&plan)==0);
    CHECK(plan.requests==1 && plan.fallback_mask==1);
    CHECK(mt2_clc_request(&plan,&p,0,0,request,sizeof(request))==92 && request[5]==1 && !request[7] && request[8]=='0' && request[9]=='0' && request[13]==77 && request[76]==64);
    p=policy();for(i=0;i<240;++i)bad_plan(ram,i,0,&p,sizeof(request));
    for(i=0;i<92;++i)bad_plan(ram,240,0,&p,i);
    bad_plan(ram,240,2,&p,sizeof(request));p.acpi_known=0;bad_plan(ram,240,0,&p,sizeof(request));p=policy();
    p.mtcl_config=256;bad_plan(ram,240,0,&p,sizeof(request));p=policy();
    p.world_mtcl_config=256;bad_plan(ram,240,0,&p,sizeof(request));p=policy();
    /* Optional indices use their first block; each resolves country/fallback
     * independently. Bad data in a later selected block rejects the whole plan. */
    ram[86]=1;ram[98]='C';ram[99]='A';ram[120]='C';ram[121]='A';
    CHECK(mt2_clc_plan_build(ram,240,0,&p,sizeof(request),&plan)==0 && plan.selected_mask==3 && plan.fallback_mask==2 && plan.requests==3);
    CHECK(mt2_clc_request(&plan,&p,2,1,request,sizeof(request))==92 && request[4]==1 && request[5]==1 && request[13]==77 && request[76]==192);
    put16(ram+146,0xffff);bad_plan(ram,240,0,&p,sizeof(request));test_clc_image(ram);
    put32(ram+82,0xffffffffu);bad_plan(ram,240,0,&p,sizeof(request));test_clc_image(ram);
    ram[5]=2;bad_plan(ram,240,0,&p,sizeof(request));test_clc_image(ram);
    put16(ram+20,0xffff);bad_plan(ram,240,0,&p,sizeof(request));test_clc_image(ram);
    ram[7]=1;bad_plan(ram,240,0,&p,sizeof(request));test_clc_image(ram);
    ram[188]|=1;bad_plan(ram,240,0,&p,sizeof(request));
    test_hw_event(event,3);CHECK(mt2_hw_type_response(3,event,60,&hw)==0 && hw.encapsulation==1 && hw.valid_raw==1);
    memset(&before,0xa5,sizeof(before));
    for(i=0;i<60;++i){hw=before;CHECK(mt2_hw_type_response(3,event,i,&hw)==MT2_INVALID && !memcmp(&hw,&before,sizeof(hw)));}
    hw=before;event[29]=4;CHECK(mt2_hw_type_response(3,event,60,&hw)==MT2_PENDING && !memcmp(&hw,&before,sizeof(hw)));event[29]=3;
    event[30]=4;CHECK(mt2_hw_type_response(3,event,60,&hw)==MT2_PENDING);event[30]=0;
    put32(event+36,0x560);CHECK(mt2_hw_type_response(3,event,60,&hw)==MT2_PENDING);put32(event+36,0x550);
    put32(event+40,0);CHECK(mt2_hw_type_response(3,event,60,&hw)==MT2_PROTOCOL && !memcmp(&hw,&before,sizeof(hw)));
    test_clc_event(event,3);CHECK(mt2_clc_response(3,event,108,&reply)==0 && reply.channel_config==31 && reply.event_tag==1 && reply.event_length==68);
    memset(&prior,0xa5,sizeof(prior));
    for(i=0;i<108;++i){reply=prior;CHECK(mt2_clc_response(3,event,i,&reply)==MT2_INVALID && !memcmp(&reply,&prior,sizeof(reply)));}
    reply=prior;event[29]=4;CHECK(mt2_clc_response(3,event,108,&reply)==MT2_PENDING && !memcmp(&reply,&prior,sizeof(reply)));event[29]=3;
    event[30]=4;CHECK(mt2_clc_response(3,event,108,&reply)==MT2_PENDING);event[30]=0;
    event[24]=83;CHECK(mt2_clc_response(3,event,108,&reply)==MT2_INVALID);
    puts("{\"clc_and_hardware_type_codecs\":\"passed\",\"device_access\":false}");
}
void test_clc_file(const uint8_t *ram,uint32_t size) {
    mt2_clc_policy p=policy();mt2_clc_plan plan;uint8_t request[MT2_CLC_MAX_PAYLOAD];uint32_t hw,i,at,ordinal,n,total=0;
    /* Compare all matching US and world records for both real power variants.
     * No host-side interpretation of the opaque power data is performed. */
    for(hw=0;hw<2;++hw)for(i=0;i<2;++i) {
        p.alpha2[0]=(uint8_t)(i?'0':'U');p.alpha2[1]=(uint8_t)(i?'0':'S');
        if(mt2_clc_plan_build(ram,size,hw,&p,sizeof(request),&plan)) {
            mt2_fw_plan fw;uint32_t k,found=0,table;
            CHECK(mt2_fw_validate(ram,size,MT2_FW_RAM,&fw)==0);table=size-36-fw.count*40;
            for(k=0;k<fw.count;++k)if(fw.regions[k].skip && ram[table+k*40+25]==2)found=1;
            CHECK(!found);puts("{\"firmware_clc_power\":\"absent_rejected\",\"device_access\":false}");return;
        }
        CHECK(plan.selected_mask==1 && !plan.fallback_mask);ordinal=0;
        for(at=16;at<plan.views[0].length;at+=6+n) {
            const uint8_t *r=plan.views[0].block+at;n=le16(r+4);
            if(r[0]!=p.alpha2[0] || r[1]!=p.alpha2[1])continue;
            CHECK(mt2_clc_request(&plan,&p,ordinal,1,request,sizeof(request))==(int)(76+n));
            CHECK(!memcmp(request+76,r+6,n) && !memcmp(request+8,r,4));++ordinal;++total;
        }
        CHECK(ordinal==plan.requests);
    }
    printf("{\"firmware_clc_power\":\"US_and_world_both_variants_byte_exact\",\"records\":%u,\"device_access\":false}\n",total);
}
