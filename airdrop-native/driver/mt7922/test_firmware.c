/* SPDX-License-Identifier: BSD-3-Clause-Clear */
#define _CRT_SECURE_NO_WARNINGS
#include "firmware.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FW FAIL line %d: %s\n",__LINE__,#x);exit(1);}}while(0)
static void le32(uint8_t *p,uint32_t v){p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);p[2]=(uint8_t)(v>>16);p[3]=(uint8_t)(v>>24);}
static uint32_t read32(const uint8_t *p){return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static void be32(uint8_t *p,uint32_t v){p[3]=(uint8_t)v;p[2]=(uint8_t)(v>>8);p[1]=(uint8_t)(v>>16);p[0]=(uint8_t)(v>>24);}
typedef struct simulation {
    uint32_t sends,drains,waits,releases,bytes,semaphore,regions;
    uint32_t remaining;int scatter_error,release_error,drain_error,ready_error,rom_error,ack_error;
} simulation;
static int send(void *ctx,uint32_t cmd,const uint8_t *p,uint32_t size,int wait,uint32_t *reply) {
    simulation *s=(simulation *)ctx;++s->sends;*reply=0;
    CHECK(p && size);
    if(cmd==4) {CHECK(size==4 && !wait && read32(p)==1);return 0;}
    if(cmd==0x10) {
        CHECK(size==4 && wait);
        if(read32(p)==1) *reply=s->semaphore;
        else {CHECK(read32(p)==0);++s->releases;*reply=3;return s->release_error;}
    } else if(cmd==1 || cmd==5) {
        CHECK(wait && size==12 && !s->remaining);
        s->remaining=read32(p+4);++s->regions;*reply=(uint32_t)s->ack_error;
    } else if(cmd==MT2_FW_SCATTER) {
        CHECK(!wait && size<=4096 && size<=s->remaining);
        if(s->scatter_error) return s->scatter_error;
        s->remaining-=size;s->bytes+=size;
    } else if(cmd==7) {CHECK(wait && size==4 && !s->remaining);}
    else if(cmd==2) {CHECK(wait && size==8 && !s->remaining);}
    else CHECK(0);
    return 0;
}
static int drain(void *ctx){simulation *s=(simulation *)ctx;CHECK(!s->remaining);++s->drains;return s->drain_error;}
static int wait_state(void *ctx,uint32_t ready){simulation *s=(simulation *)ctx;++s->waits;return ready?s->ready_error:s->rom_error;}
static mt2_fw_transport transport(simulation *s){mt2_fw_transport t={s,send,drain,wait_state};return t;}
static void images(uint8_t *patch,uint8_t *ram) {
    memset(patch,0,164);be32(patch+44,1);be32(patch+96,2);be32(patch+100,160);
    be32(patch+104,4);be32(patch+108,0x900000);be32(patch+112,4);be32(patch+116,0xffffffffu);
    memset(ram,0,80);le32(ram+20,0x91b000);le32(ram+24,4);ram[28]=0x21;ram[46]=1;
}
static void reset(simulation *s){memset(s,0,sizeof(*s));s->semaphore=2;}
void test_firmware(void) {
    uint8_t patch[164],ram[80];mt2_fw_plan plan;mt2_fw_result result;
    simulation s;mt2_fw_transport t=transport(&s);
    images(patch,ram);
    CHECK(mt2_fw_validate(ram,80,MT2_FW_RAM,&plan)==0 && plan.count==1 && plan.entry==0x91b000);
    CHECK(plan.regions[0].mode==0x80000009 && plan.regions[0].length==4);
    reset(&s);CHECK(mt2_fw_boot(patch,164,ram,80,&t,&result)==0);
    CHECK(result.ready && result.start_acked && result.bytes_sent==8 && result.regions_sent==2);
    CHECK(s.sends==9 && s.drains==2 && s.releases==1 && s.waits==2);
    reset(&s);s.semaphore=1;CHECK(mt2_fw_boot(patch,164,ram,80,&t,&result)==0);
    CHECK(result.patch_present && !s.releases && s.bytes==4 && s.drains==1);
    reset(&s);ram[46]=255;CHECK(mt2_fw_boot(patch,164,ram,80,&t,&result)==MT2_INVALID);
    CHECK(!s.sends && !s.waits);images(patch,ram);
    reset(&s);le32(ram+24,5);CHECK(mt2_fw_boot(patch,164,ram,80,&t,&result)==MT2_INVALID);
    CHECK(!s.sends);images(patch,ram);
    reset(&s);be32(patch+100,159);CHECK(mt2_fw_boot(patch,164,ram,80,&t,&result)==MT2_INVALID);
    CHECK(!s.sends);images(patch,ram);
    reset(&s);s.scatter_error=-77;CHECK(mt2_fw_boot(patch,164,ram,80,&t,&result)==-77);
    CHECK(s.releases==1 && !result.start_acked && !result.ready);
    reset(&s);s.scatter_error=-77;s.release_error=-88;
    CHECK(mt2_fw_boot(patch,164,ram,80,&t,&result)==-77 && result.release_error==-88 && s.releases==1);
    reset(&s);s.drain_error=-5;CHECK(mt2_fw_boot(patch,164,ram,80,&t,&result)==-5 && s.releases==1);
    reset(&s);s.ack_error=4;CHECK(mt2_fw_boot(patch,164,ram,80,&t,&result)==MT2_PROTOCOL && s.releases==1);
    reset(&s);s.rom_error=-5;CHECK(mt2_fw_boot(patch,164,ram,80,&t,&result)==-5 && !s.releases && s.sends==1);
    reset(&s);s.ready_error=-5;CHECK(mt2_fw_boot(patch,164,ram,80,&t,&result)==-5);
    CHECK(result.start_acked && !result.ready);
    reset(&s);s.semaphore=0;CHECK(mt2_fw_boot(patch,164,ram,80,&t,&result)==MT2_PROTOCOL && !s.releases);
    puts("{\"firmware_lifecycle_tests\":\"passed\",\"transport\":\"simulation\",\"device_access\":false}");
}
static uint8_t *read_image(const char *path,uint32_t *size) {
    FILE *f=fopen(path,"rb");long length;uint8_t *p;
    CHECK(f!=NULL);CHECK(!fseek(f,0,SEEK_END));length=ftell(f);
    CHECK(length>0 && (unsigned long)length<=MT2_FW_MAX_SIZE);CHECK(!fseek(f,0,SEEK_SET));
    *size=(uint32_t)length;p=(uint8_t *)malloc(*size);CHECK(p!=NULL);
    CHECK(fread(p,1,*size,f)==*size);CHECK(!fclose(f));return p;
}
void test_pcie_image_buffers(const uint8_t *,uint32_t,const uint8_t *,uint32_t);
void test_regulatory_file(const uint8_t *,uint32_t);
void test_clc_file(const uint8_t *,uint32_t);
void test_installed_image_files(const char *patch_path,const char *ram_path) {
    uint32_t ps,rs;uint8_t *patch=read_image(patch_path,&ps),*ram=read_image(ram_path,&rs);
    mt2_fw_plan pp,rp;mt2_fw_result result;simulation s;mt2_fw_transport t=transport(&s);
    CHECK(mt2_fw_validate(patch,ps,MT2_FW_PATCH,&pp)==0);
    CHECK(mt2_fw_validate(ram,rs,MT2_FW_RAM,&rp)==0);reset(&s);
    test_regulatory_file(ram,rs);
    test_clc_file(ram,rs);
    CHECK(mt2_fw_boot(patch,ps,ram,rs,&t,&result)==0 && result.ready);
    printf("{\"installed_image_file_test\":\"passed\",\"transport\":\"simulation\",\"patch_regions\":%u,\"ram_regions\":%u,\"entry\":%u,\"simulated_bytes\":%u,\"simulated_commands\":%u,\"device_access\":false}\n",pp.count,rp.count,rp.entry,s.bytes,s.sends);
    test_pcie_image_buffers(patch,ps,ram,rs);
    free(patch);free(ram);
}
