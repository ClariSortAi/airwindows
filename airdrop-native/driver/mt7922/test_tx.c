/* SPDX-License-Identifier: BSD-3-Clause-Clear */
#include "tx.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) {fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);exit(1);} } while(0)
static uint32_t get32(const uint8_t *p) {return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static void put32(uint8_t *p,uint32_t v) {p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);p[2]=(uint8_t)(v>>16);p[3]=(uint8_t)(v>>24);}
static mt2_tx_config config={0x155,2,24,0x4b,0};
static void action(uint8_t *f) {
    memset(f,0,64);f[0]=0xd0;f[4]=1;f[22]=0xc0;f[23]=0xab;
    f[24]=0x7f;f[26]=0x17;f[27]=0xf2;f[28]=8;
}
static void reject(const uint8_t *f,uint32_t size,uint64_t address,uint32_t token,
                   const mt2_tx_config *c,uint32_t capacity) {
    uint8_t out[64],before[64];memset(out,0xa5,sizeof(out));memcpy(before,out,sizeof(out));
    CHECK(mt2_tx_encode(f,size,address,token,c,out,capacity)==MT2_INVALID);
    CHECK(!memcmp(out,before,sizeof(out)));
}
static void descriptors(void) {
    uint8_t f[64],out[64],expected[64]={0};uint32_t i;
    const uint32_t golden[]={0x2000003d,0x82026155,0x8000240d,0x9abc7801,0,1,0x004b0004,0x000dc000};
    mt2_tx_config c=config;
    action(f);
    for (i=0;i<8;++i) put32(expected+i*4,golden[i]);
    expected[32]=7;expected[33]=0x80;put32(expected+40,0x11223344);expected[44]=29;expected[45]=0x80;
    CHECK(mt2_tx_encode(f,29,0x11223344,7,&c,out,sizeof(out))==64);
    CHECK(!memcmp(out,expected,64));
    /* QoS unicast: Connac2 TID/header fields, ACK on, AC_BE queue. */
    f[0]=0x88;f[4]=2;f[22]=0x30;f[23]=0x12;f[24]=5;
    CHECK(mt2_tx_encode(f,40,0x11223344,7,&c,out,sizeof(out))==64);
    CHECK(get32(out)==0x02000048 && get32(out+4)==0x82526955);
    CHECK(get32(out+8)==0x80002028 && get32(out+12)==0x91237800);
    CHECK(get32(out+28)==0x0028c000);
    c.no_ack=1;CHECK(mt2_tx_encode(f,40,0x11223344,7,&c,out,sizeof(out))==64);
    CHECK(get32(out+12)==0x91237801);c.no_ack=0;
    reject(f,25,0x1000,0,&c,64);f[24]=8;reject(f,40,0x1000,0,&c,64);
    f[24]=0x80;reject(f,40,0x1000,0,&c,64);
    action(f);reject(f,29,0x100000000ULL,0,&c,64);
    reject(f,29,0xfffffff0ULL,0,&c,64);reject(f,29,0x1000,8192,&c,64);
    reject(f,29,0x1000,0,&c,63);reject(f,24,0x1000,0,&c,64);
    reject(f,4096,0x1000,0,&c,64);reject(NULL,29,0x1000,0,&c,64);
    f[1]=0x40;reject(f,29,0x1000,0,&c,64);f[1]=4;reject(f,29,0x1000,0,&c,64);
    f[1]=1;reject(f,29,0x1000,0,&c,64);f[1]=0x80;reject(f,29,0x1000,0,&c,64);
    f[1]=0;f[22]|=1;reject(f,29,0x1000,0,&c,64);action(f);
    c.wcid=1024;reject(f,29,0x1000,0,&c,64);c=config;
    c.rate=18;reject(f,29,0x1000,0,&c,64);c=config; /* MT7925 table index is wrong here. */
    c.own_mac=64;reject(f,29,0x1000,0,&c,64);c=config;
    c.spe_index=32;reject(f,29,0x1000,0,&c,64);
}
typedef struct fixture {
    mt2_tx_queue q;mt2_descriptor d[8];uint8_t pool[4*MT2_SLOT_SIZE];
    uint32_t states[4],slots[8],barriers,publishes,last;int publish_result;
} fixture;
static fixture v;
static void barrier(void *p) {++((fixture *)p)->barriers;}
static int publish(void *p,uint32_t next) {
    fixture *f=(fixture *)p;uint32_t previous=(next+7u)%8;
    CHECK(f->barriers>f->publishes);CHECK(f->q.head==next);
    CHECK(f->q.slots[previous]<4 && f->q.states[f->q.slots[previous]]==1);
    CHECK(f->d[previous].control==0x40400000 && f->pool[f->q.slots[previous]*MT2_SLOT_SIZE+64]==0xd0);
    ++f->publishes;f->last=next;return f->publish_result;
}
static void setup(void) {
    mt2_tx_io io;memset(&v,0,sizeof(v));io.context=&v;io.barrier=barrier;io.publish=publish;
    CHECK(mt2_tx_prepare(&v.q,v.d,0x10000,8,v.pool,0x20000,sizeof(v.pool),v.states,4,v.slots,&io)==0);
    CHECK(mt2_tx_activate(&v.q)==0);
}
static uint32_t submit(void) {
    uint8_t f[64];uint32_t token=0xffffffffu;action(f);
    CHECK(mt2_tx_submit(&v.q,f,29,&config,&token)==0);CHECK(token<4);return token;
}
static void release_packet(uint8_t *p,uint32_t id) {
    memset(p,0,16);put32(p,0x30010010);put32(p+8,0x80000000u|(config.wcid<<14));
    put32(p+12,id<<16);
}
static void ownership(void) {
    uint8_t p[24];uint32_t id,i,next;
    setup();id=submit();release_packet(p,id);
    CHECK(mt2_tx_dma_complete(&v.q,1)==0);CHECK(v.q.inflight==1 && v.states[id]==3);
    CHECK(mt2_tx_firmware_complete(&v.q,p,16)==0);CHECK(!v.q.inflight && !v.states[id]);
    setup();id=submit();release_packet(p,id);
    CHECK(mt2_tx_firmware_complete(&v.q,p,16)==0);CHECK(v.q.inflight==1 && v.states[id]==5);
    CHECK(mt2_tx_dma_complete(&v.q,1)==0);CHECK(!v.q.inflight && !v.states[id]);
    /* Exercise ring wrap and token reuse after both completions. */
    for(i=0;i<40;++i) {
        id=submit();next=v.q.head;release_packet(p,id);
        CHECK(mt2_tx_firmware_complete(&v.q,p,16)==0);CHECK(mt2_tx_dma_complete(&v.q,next)==0);
        CHECK(!v.q.inflight && !v.q.queued);
    }
    /* DMA reclaim alone does not permit reusing the payload tokens. */
    setup();for(i=0;i<4;++i) {submit();CHECK(mt2_tx_dma_complete(&v.q,v.q.head)==0);}
    {uint8_t f[64];action(f);CHECK(mt2_tx_submit(&v.q,f,29,&config,&id)==MT2_FULL);}
    CHECK(v.q.inflight==4);
    /* Bad second token must not release the valid first token. */
    setup();id=submit();submit();memset(p,0,sizeof(p));put32(p,0x30020010);
    put32(p+8,id<<16);put32(p+12,99u<<16);
    CHECK(mt2_tx_firmware_complete(&v.q,p,16)==MT2_PROTOCOL);
    CHECK(v.q.inflight==2 && v.states[id]==1 && !v.q.active);
    /* Duplicate token in one batch is rejected atomically. */
    setup();id=submit();put32(p,0x30020010);put32(p+8,id<<16);put32(p+12,id<<16);
    CHECK(mt2_tx_firmware_complete(&v.q,p,16)==MT2_PROTOCOL);CHECK(v.states[id]==1);
    /* Connac3's two-IDs-per-word notification must not free Connac2 tokens. */
    setup();id=submit();put32(p,0x3002000c);put32(p+4,0x40000);put32(p+8,id|(1u<<15));
    CHECK(mt2_tx_firmware_complete(&v.q,p,12)==MT2_PROTOCOL);CHECK(v.states[id]==1);
    setup();id=submit();release_packet(p,id);put32(p,0x30010014);
    CHECK(mt2_tx_firmware_complete(&v.q,p,16)==MT2_PROTOCOL);CHECK(v.states[id]==1);
    setup();id=submit();CHECK(mt2_tx_dma_complete(&v.q,2)==MT2_PROTOCOL);CHECK(v.states[id]==1);
    /* Publish error can occur after the doorbell write: retain both allocations. */
    setup();v.publish_result=-99;
    {uint8_t f[64];action(f);CHECK(mt2_tx_submit(&v.q,f,29,&config,&id)==-99);}
    CHECK(v.q.inflight==1 && v.q.queued==1 && v.states[0]==1 && !v.q.active);
    setup();id=submit();mt2_tx_disarm(&v.q);
    CHECK(v.q.inflight==1 && v.states[id]==1 && mt2_tx_activate(&v.q)==MT2_INVALID);
}
void test_mcu(void);
void test_firmware(void);
void test_chip(void);
void test_dma(void);
void test_pcie_mcu(void);
void test_nic_caps(void);
void test_raw_rx(void);
void test_device_abi(void);
void test_regulatory(void);
void test_clc_power(void);
void test_platform_acpi(void);
void test_tx_power(void);
void test_rx_bridge(void);
void test_radio_abi(void);
void test_installed_image_files(const char *,const char *);
int main(int argc,char **argv) {
    CHECK(argc==1 || argc==3);
    descriptors();ownership();test_mcu();test_firmware();test_chip();test_dma();test_nic_caps();test_raw_rx();test_rx_bridge();test_regulatory();test_clc_power();test_platform_acpi();test_tx_power();test_pcie_mcu();test_device_abi();test_radio_abi();
    if(argc==3) test_installed_image_files(argv[1],argv[2]);
    puts("{\"result\":\"passed\",\"suites\":[\"connac2_golden_descriptors\",\"invalid_frames\",\"dma_firmware_ownership\"],\"device_access\":false}");
    return 0;
}
