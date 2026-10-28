/* SPDX-License-Identifier: BSD-3-Clause-Clear */
#include "radio_abi.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"RADIO ABI FAIL %d: %s\n",__LINE__,#x);exit(1);}}while(0)
static void put32(uint8_t *p,uint32_t n){p[0]=(uint8_t)n;p[1]=(uint8_t)(n>>8);p[2]=(uint8_t)(n>>16);p[3]=(uint8_t)(n>>24);}
static void start(uint8_t *p) {
    memset(p,0,80);put32(p,MT2_RADIO_START_MAGIC);put32(p+4,1);put32(p+8,1);put32(p+12,2);
    p[16]='U';p[17]='S';p[18]=2;p[23]=1;put32(p+32,6);put32(p+36,6);put32(p+40,20);
    put32(p+48,1);put32(p+52,20);put32(p+56,1);p[64]='P';p[65]='R';p[66]='R';
}
static void reject(uint32_t code,const uint8_t *p,uint32_t bytes) {
    mt2_radio_view v,b;memset(&v,0xa5,sizeof(v));b=v;
    CHECK(mt2_radio_request(code,p,bytes,&v)==MT2_INVALID && !memcmp(&v,&b,sizeof(v)));
}
void test_radio_abi(void) {
    uint8_t p[80];uint32_t i;mt2_radio_view v;
    const uint32_t empty[]={MT2_IOCTL_RADIO_STATUS,MT2_IOCTL_RADIO_SERVICE,MT2_IOCTL_RADIO_RECEIVE};
    start(p);CHECK(mt2_radio_request(MT2_IOCTL_RADIO_START,p,67,&v)==0);
    CHECK(v.firmware.patch==p+64 && v.firmware.ram==p+65 && v.firmware.patch_bytes==1 && v.firmware.ram_bytes==2);
    CHECK(v.options.alpha2[0]=='U' && v.options.address[0]==2 && v.options.address[5]==1 && v.options.cap_half_dbm==20 && v.options.enable_tx==1);
    for(i=0;i<67;++i)reject(MT2_IOCTL_RADIO_START,p,i);reject(MT2_IOCTL_RADIO_START,p,68);
    put32(p+52,0xffffff81u);CHECK(mt2_radio_request(MT2_IOCTL_RADIO_START,p,67,&v)==0 && v.options.cap_half_dbm== -127);
    put32(p+52,0);CHECK(mt2_radio_request(MT2_IOCTL_RADIO_START,p,67,&v)==0 && !v.options.cap_half_dbm);
    put32(p+52,0xffffff80u);reject(MT2_IOCTL_RADIO_START,p,67);
    start(p);put32(p+56,0);reject(MT2_IOCTL_RADIO_START,p,67);
    start(p);put32(p+60,1);reject(MT2_IOCTL_RADIO_START,p,67);
    start(p);put32(p+8,MT2_FW_MAX_SIZE+1);reject(MT2_IOCTL_RADIO_START,p,67);
    start(p);put32(p+12,0xffffffffu);reject(MT2_IOCTL_RADIO_START,p,67);
    start(p);p[16]='u';reject(MT2_IOCTL_RADIO_START,p,67);
    start(p);p[18]=1;reject(MT2_IOCTL_RADIO_START,p,67);
    start(p);put32(p+44,2);reject(MT2_IOCTL_RADIO_START,p,67);
    start(p);put32(p+48,2);reject(MT2_IOCTL_RADIO_START,p,67);
    start(p);put32(p+52,45);reject(MT2_IOCTL_RADIO_START,p,67);
    reject(MT2_IOCTL_RADIO_START^0x4000u,p,67);reject(MT2_IOCTL_BOOT,p,67);
    for(i=0;i<3;++i){CHECK(mt2_radio_request(empty[i],0,0,&v)==0);reject(empty[i],p,1);}
    memset(p,0,sizeof(p));put32(p+4,12);put32(p+8,24);p[12]=8;
    CHECK(mt2_radio_request(MT2_IOCTL_RADIO_TRANSMIT,p,36,&v)==0 && v.frame==p+12 && v.frame_bytes==24 && v.rate==12 && !v.no_ack);
    for(i=0;i<36;++i)reject(MT2_IOCTL_RADIO_TRANSMIT,p,i);
    put32(p,2);reject(MT2_IOCTL_RADIO_TRANSMIT,p,36);put32(p,1);put32(p+4,25);reject(MT2_IOCTL_RADIO_TRANSMIT,p,36);
    put32(p,6);put32(p+4,6);put32(p+8,20);
    CHECK(mt2_radio_request(MT2_IOCTL_RADIO_CHANNEL,p,12,&v)==0 && v.primary==6 && v.center==6 && v.width==20);
    reject(MT2_IOCTL_RADIO_CHANNEL,p,11);reject(MT2_IOCTL_RADIO_CHANNEL,p,13);put32(p+8,160);reject(MT2_IOCTL_RADIO_CHANNEL,p,12);
    memset(p,0,6);p[0]=2;CHECK(mt2_radio_request(MT2_IOCTL_RADIO_ADDRESS,p,6,&v)==0 && v.address[0]==2);
    reject(MT2_IOCTL_RADIO_ADDRESS,p,5);reject(MT2_IOCTL_RADIO_ADDRESS,p,7);p[0]=0;reject(MT2_IOCTL_RADIO_ADDRESS,p,6);
    p[0]=1;reject(MT2_IOCTL_RADIO_ADDRESS,p,6);
    puts("{\"radio_abi_tests\":\"passed\",\"input\":\"synthetic_buffers\",\"device_access\":false}");
}
