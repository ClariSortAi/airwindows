/* SPDX-License-Identifier: BSD-3-Clause-Clear */
#include "device_abi.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"ABI FAIL line %d: %s\n",__LINE__,#x);exit(1);}}while(0)
typedef char ioctl_values_match[(MT2_IOCTL_STATUS==0x226400 && MT2_IOCTL_BOOT==0x22e404 && MT2_IOCTL_STOP==0x22a408)?1:-1];
static void put32(uint8_t *p,uint32_t x){p[0]=(uint8_t)x;p[1]=(uint8_t)(x>>8);p[2]=(uint8_t)(x>>16);p[3]=(uint8_t)(x>>24);}
static void reject(const uint8_t *p,uint32_t bytes) {
    mt2_boot_view out,old;memset(&out,0xa5,sizeof(out));old=out;
    CHECK(mt2_boot_request(p,bytes,&out)==MT2_INVALID && !memcmp(&out,&old,sizeof(out)));
}
void test_device_abi(void) {
    uint8_t p[32]={0};mt2_boot_view out;uint32_t i;
    put32(p,MT2_BOOT_MAGIC);put32(p+4,MT2_ABI_VERSION);put32(p+8,1);put32(p+12,1);p[16]=7;p[17]=9;
    CHECK(mt2_boot_request(p,18,&out)==0 && out.patch==p+16 && out.ram==p+17 && out.patch_bytes==1 && out.ram_bytes==1);
    for(i=0;i<18;++i)reject(p,i);
    reject(p,19);reject(NULL,18);reject(p,0xffffffffu);
    put32(p,0);reject(p,18);put32(p,MT2_BOOT_MAGIC);
    put32(p+4,2);reject(p,18);put32(p+4,1);
    put32(p+8,0);reject(p,18);put32(p+8,0xffffffffu);reject(p,18);put32(p+8,1);
    put32(p+12,0);reject(p,18);put32(p+12,0xffffffffu);reject(p,18);
    put32(p+8,MT2_FW_MAX_SIZE+1);put32(p+12,1);reject(p,MT2_BOOT_HEADER+MT2_FW_MAX_SIZE+2);
    puts("{\"device_abi_tests\":\"passed\",\"input\":\"synthetic_buffers\",\"device_access\":false}");
}
