/* SPDX-License-Identifier: BSD-3-Clause-Clear */
#include "chip.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"CHIP FAIL line %d: %s\n",__LINE__,#x);exit(1);}}while(0)
typedef struct model {
    uint32_t bar[0x100000/4],reset,top_own,reads,writes,barriers;
    uint32_t fail_write,fail_count,fail_nonzero,ignore_write,all_ones_read,hold_busy;
    uint32_t fail_restore,remap_written;
    uint64_t time;
} model;
static model m;
static void barrier(void *ctx){++((model *)ctx)->barriers;}
static int read_register(void *ctx,uint32_t offset,uint32_t *value) {
    model *s=(model *)ctx;uint32_t physical;
    CHECK(offset<0x100000 && !(offset&3u));++s->reads;
    if(offset==s->all_ones_read) {*value=0xffffffffu;return 0;}
    if(offset>=0x40000 && offset<0x50000) {
        physical=(s->bar[0xfe24c/4]&0xffffu)<<16|(offset-0x40000);
        if(physical==0x18000140) {*value=s->reset;return 0;}
        if(physical==0x18060010) {*value=s->top_own;return 0;}
    }
    *value=s->bar[offset/4];
    if(offset==0xd4208 && s->hold_busy)*value|=10;
    return 0;
}
static int write_register(void *ctx,uint32_t offset,uint32_t value) {
    model *s=(model *)ctx;uint32_t physical;
    CHECK(offset<0x100000 && !(offset&3u));++s->writes;
    CHECK(s->barriers>=s->writes);
    if(offset==0xfe24c && s->fail_restore && s->remap_written++)return -42;
    if(offset==s->ignore_write)return 0;
    /* Simulate a reported write failure AFTER it reached the device. */
    s->bar[offset/4]=value;
    /* TX/RX BUSY are device-owned, read-only status bits. A read-modify-write
     * must not latch the injected busy status into the register model. */
    if(offset==0xd4208)s->bar[offset/4]&=~10u;
    if(offset==0xe0010)s->bar[offset/4]=(value==1)?4u:0u;
    if(offset>=0x40000 && offset<0x50000) {
        physical=(s->bar[0xfe24c/4]&0xffffu)<<16|(offset-0x40000);
        if(physical==0x18000140)s->reset=(value&1u)?value|0x10u:value&~0x10u;
        if(physical==0x18060010)s->top_own=(value==2)?0u:1u;
    }
    if(offset==s->fail_write && s->fail_count && (!s->fail_nonzero || value)){--s->fail_count;return -41;}
    return 0;
}
static uint64_t milliseconds(void *ctx){return ((model *)ctx)->time;}
static int delay(void *ctx,uint32_t value){((model *)ctx)->time+=value;return 0;}
static mt2_register_io reset_model(void) {
    mt2_register_io io={&m,0x100000,read_register,write_register,barrier,milliseconds,delay};
    memset(&m,0,sizeof(m));m.fail_write=0xffffffffu;m.ignore_write=0xffffffffu;
    m.all_ones_read=0xffffffffu;m.reset=0x11;return io;
}
static void bindings(mt2_ring_binding *r) {
    uint32_t i;for(i=0;i<MT2_RING_COUNT;++i){r[i].address=0x100000u+i*0x1000u;r[i].count=16;}
}
void test_chip(void) {
    mt2_register_io io;mt2_ring_binding r[MT2_RING_COUNT];mt2_chip c;
    uint32_t value,before,i;
    io=reset_model();m.bar[0x21000/4]=0x12345678;value=0;
    CHECK(mt2_reg_read(&io,0x820e4000,&value)==0 && value==0x12345678 && !m.writes);
    CHECK(mt2_reg_write(&io,0x820e43fc,0x55)==0 && m.bar[0x213fc/4]==0x55);
    before=m.reads+m.writes;value=99;
    CHECK(mt2_reg_read(&io,0x820e4400,&value)==MT2_INVALID && value==99);
    CHECK(mt2_reg_write(&io,0x820e4001,0)==MT2_INVALID);
    CHECK(mt2_reg_write(&io,0xffffffffu,0)==MT2_INVALID && m.reads+m.writes==before);
    io.bar_bytes=0x80000;CHECK(mt2_reg_read(&io,0,&value)==MT2_INVALID);io.bar_bytes=0x100000;
    m.bar[0xfe24c/4]=0xabcd0000;
    CHECK(mt2_reg_read(&io,0x18000140,&value)==0 && value==0x11);
    CHECK(m.bar[0xfe24c/4]==0xabcd0000);
    CHECK(mt2_reg_write(&io,0x18060010,2)==0 && !m.top_own && m.bar[0xfe24c/4]==0xabcd0000);
    io=reset_model();m.fail_write=0x40140;m.fail_count=1;
    CHECK(mt2_reg_write(&io,0x18000140,0)==-41 && m.bar[0xfe24c/4]==0);
    io=reset_model();m.fail_write=0xfe24c;m.fail_count=1;value=99;
    CHECK(mt2_reg_read(&io,0x18000140,&value)==-41 && value==99 && m.bar[0xfe24c/4]==0);
    io=reset_model();m.fail_restore=1;value=99;
    CHECK(mt2_reg_read(&io,0x18000140,&value)==-42 && value==99);
    io=reset_model();m.all_ones_read=0x21000;value=99;
    CHECK(mt2_reg_read(&io,0x820e4000,&value)==MT2_PROTOCOL && value==99);

    io=reset_model();bindings(r);memset(&c,0,sizeof(c));
    r[1].address=r[0].address;CHECK(mt2_chip_start(&c,&io,r)==MT2_INVALID && !m.writes && !m.reads);
    bindings(r);r[0].address=0xfffffff0ULL;CHECK(mt2_chip_start(&c,&io,r)==MT2_INVALID && !m.writes);
    bindings(r);r[0].count=3;CHECK(mt2_chip_start(&c,&io,r)==MT2_INVALID && !m.writes);
    bindings(r);m.bar[0x9f23c/4]=2;
    CHECK(mt2_chip_start(&c,&io,r)==0 && c.active && c.published && !c.quiescent);
    CHECK(!m.bar[0x9f23c/4]); /* SWDEF_NORMAL_MODE before firmware download. */
    CHECK(m.bar[0xd4410/4]==r[1].address && m.bar[0xd4414/4]==16); /* command TX17 */
    CHECK(m.bar[0xd43f0/4]==0 && m.bar[0xd43f4/4]==0); /* not MT7925 TX15 */
    CHECK(m.bar[0xd4400/4]==r[2].address && m.bar[0xd4540/4]==r[5].address);
    CHECK(m.bar[0xd4548/4]==15 && m.bar[0xd4644/4]==0x03800004);
    CHECK((m.bar[0xd4208/4]&0x0c000005u)==5 && m.bar[0xfe24c/4]==0);
    CHECK(!m.bar[0xd4204/4] && !m.bar[0x10188/4] && !m.bar[0xd41f4/4]);
    CHECK(mt2_chip_stop(&c)==0 && c.quiescent && !c.active);
    for(i=0;i<18;++i)CHECK(!m.bar[(0xd4300+i*16)/4] && !m.bar[(0xd4304+i*16)/4]);
    CHECK(mt2_chip_start(&c,&io,r)==MT2_INVALID);

    io=reset_model();memset(&c,0,sizeof(c));CHECK(mt2_chip_start(&c,&io,r)==0);
    m.ignore_write=0xd4208;before=m.writes;
    CHECK(mt2_chip_stop(&c)==MT2_FW_TIMEOUT && !c.quiescent && !c.active);
    CHECK(m.bar[0xd4410/4]==r[1].address && m.writes>before);
    m.ignore_write=0xffffffffu;CHECK(mt2_chip_stop(&c)==0 && c.quiescent);
    io=reset_model();memset(&c,0,sizeof(c));CHECK(mt2_chip_start(&c,&io,r)==0);
    m.hold_busy=1;CHECK(mt2_chip_stop(&c)==MT2_FW_TIMEOUT && !c.quiescent);
    m.hold_busy=0;CHECK(mt2_chip_stop(&c)==0 && c.quiescent);

    /* Fail after a descriptor address has become visible. Cleanup must still
     * fence and clear DMA before those allocations can be released. */
    io=reset_model();memset(&c,0,sizeof(c));m.fail_write=0xd4410;m.fail_count=1;m.fail_nonzero=1;
    CHECK(mt2_chip_start(&c,&io,r)==-41 && c.published && !c.quiescent && !c.active);
    CHECK(m.bar[0xd4410/4]==r[1].address);
    CHECK(mt2_chip_stop(&c)==0 && c.quiescent);
    io=reset_model();memset(&c,0,sizeof(c));CHECK(mt2_chip_start(&c,&io,r)==0);
    m.all_ones_read=0xd4208;CHECK(mt2_chip_stop(&c)==MT2_PROTOCOL && !c.quiescent);
    m.all_ones_read=0xffffffffu;CHECK(mt2_chip_stop(&c)==0 && c.quiescent);
    puts("{\"chip_sequence_tests\":\"passed\",\"registers\":\"simulation\",\"device_access\":false}");
}
