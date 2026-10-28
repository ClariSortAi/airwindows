/* SPDX-License-Identifier: BSD-3-Clause-Clear */
#include "dma.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"DMA FAIL line %d: %s\n",__LINE__,#x);exit(1);}}while(0)
typedef struct fixture {
    mt2_descriptor d[4];uint8_t pool[4*128],assembly[256],received[256];
    mt2_command_queue command;mt2_rx_queue rx;
    uint32_t barriers,publishes,index,records,received_bytes;
    int publish_error,record_error;
} fixture;
static fixture v;
static void barrier(void *ctx){++((fixture *)ctx)->barriers;}
static int publish(void *ctx,uint32_t index) {
    fixture *f=(fixture *)ctx;
    CHECK(f->barriers>f->publishes);++f->publishes;f->index=index;return f->publish_error;
}
static int record(void *ctx,const uint8_t *p,uint32_t bytes) {
    fixture *f=(fixture *)ctx;
    CHECK(bytes<=sizeof(f->received) && f->publishes);
    ++f->records;f->received_bytes=bytes;memcpy(f->received,p,bytes);return f->record_error;
}
static mt2_dma_memory memory(void) {
    mt2_dma_memory m={v.d,0x100000,v.pool,0x200000,sizeof(v.pool),4,128};return m;
}
static void command_setup(int download) {
    mt2_dma_memory m;mt2_tx_io io={&v,barrier,publish};
    memset(&v,0,sizeof(v));m=memory();
    CHECK(mt2_command_prepare(&v.command,&m,&io,download)==0);
    CHECK(mt2_command_activate(&v.command)==0);
}
static void receive_setup(uint32_t cap) {
    mt2_dma_memory m;mt2_rx_io io={&v,barrier,publish,record};uint32_t i;
    memset(&v,0,sizeof(v));m=memory();
    CHECK(mt2_rx_prepare(&v.rx,&m,v.assembly,cap,&io)==0);
    for(i=0;i<4;++i)CHECK(v.d[i].address==0x200000+i*128 && v.d[i].control==0x00800000);
    CHECK(mt2_rx_activate(&v.rx)==0);
}
/* Simulated device writes payload first and DMA_DONE last. */
static void received(uint32_t index,const char *p,uint32_t bytes,uint32_t extra) {
    CHECK(index<4 && bytes<=128);memcpy(v.pool+index*128,p,bytes);
    v.d[index].control=MT2_DMA_DONE|(bytes<<16)|extra;
}
void test_dma(void) {
    uint8_t seq,expected[128],es;uint32_t i;int size;mt2_dma_memory m;mt2_tx_io io={&v,barrier,publish};
    memset(&v,0,sizeof(v));m=memory();m.pool_address=0x100010;
    CHECK(mt2_command_prepare(&v.command,&m,&io,0)==MT2_INVALID && !v.command.prepared);
    m=memory();m.pool_address=0xffffff00ULL;
    CHECK(mt2_command_prepare(&v.command,&m,&io,0)==MT2_INVALID);
    m=memory();m.pool_bytes=511;CHECK(mt2_command_prepare(&v.command,&m,&io,0)==MT2_INVALID);
    m=memory();m.count=3;CHECK(mt2_command_prepare(&v.command,&m,&io,0)==MT2_INVALID);

    command_setup(0);seq=0;es=0;
    size=mt2_mcu_encode(1,&es,(const uint8_t *)"test",4,expected,sizeof(expected));
    CHECK(size==68 && mt2_command_submit(&v.command,1,&seq,(const uint8_t *)"test",4)==0);
    CHECK(seq==es && !memcmp(expected,v.pool,68));
    CHECK(v.d[0].address==0x200000 && v.d[0].control==0x40440000 && v.index==1);
    CHECK(mt2_command_submit(&v.command,1,&seq,NULL,0)==0);
    CHECK(mt2_command_submit(&v.command,1,&seq,NULL,0)==MT2_FULL && seq==2);
    CHECK(mt2_command_complete(&v.command,0)==MT2_PENDING && v.command.queued==2);
    CHECK(mt2_command_complete(&v.command,2)==0 && !v.command.queued);
    for(i=0;i<40;++i) {
        CHECK(mt2_command_submit(&v.command,1,&seq,NULL,0)==0);
        CHECK(mt2_command_complete(&v.command,v.command.head)==0);
    }
    CHECK(v.command.head==2 && v.command.tail==2);
    command_setup(0);seq=7;memset(v.pool,0x55,sizeof(v.pool));
    CHECK(mt2_command_submit(&v.command,MT2_FW_SCATTER,&seq,(const uint8_t *)"x",1)==MT2_INVALID);
    CHECK(mt2_command_submit(&v.command,1,&seq,v.received,65)==MT2_INVALID);
    CHECK(seq==7 && v.pool[0]==0x55 && !v.publishes && !v.command.queued);
    v.d[0].control=0;CHECK(mt2_command_submit(&v.command,1,&seq,NULL,0)==MT2_PENDING);
    CHECK(seq==7 && !v.publishes);
    command_setup(1);seq=0;
    CHECK(mt2_command_submit(&v.command,1,&seq,NULL,0)==MT2_INVALID);
    CHECK(mt2_command_submit(&v.command,MT2_FW_SCATTER,&seq,(const uint8_t *)"abcd",4)==0);
    CHECK(v.d[0].control==0x40040000 && !memcmp(v.pool,"abcd",4));
    CHECK(mt2_command_complete(&v.command,2)==MT2_PROTOCOL && v.command.queued==1 && !v.command.active);
    command_setup(0);seq=0;v.publish_error=-99;
    CHECK(mt2_command_submit(&v.command,1,&seq,NULL,0)==-99);
    CHECK(seq==1 && v.command.queued==1 && !v.command.active && !(v.d[0].control&MT2_DMA_DONE));
    CHECK(mt2_command_activate(&v.command)==MT2_INVALID);
    command_setup(0);mt2_command_disarm(&v.command);CHECK(mt2_command_activate(&v.command)==MT2_INVALID);

    receive_setup(256);CHECK(mt2_rx_poll(&v.rx,4)==0 && !v.publishes);
    received(0,"abc",3,0x40000000);CHECK(mt2_rx_poll(&v.rx,4)==1);
    CHECK(v.records==1 && v.received_bytes==3 && !memcmp(v.received,"abc",3));
    CHECK(v.index==0 && v.d[0].control==0x00800000 && v.rx.tail==1);
    received(1,"123",3,0);CHECK(mt2_rx_poll(&v.rx,4)==1 && v.records==1 && v.rx.length==3);
    received(2,"456",3,0x40000000);CHECK(mt2_rx_poll(&v.rx,4)==1);
    CHECK(v.records==2 && v.received_bytes==6 && !memcmp(v.received,"123456",6));
    for(i=0;i<40;++i) {
        received(v.rx.tail,"W",1,0x40000000);CHECK(mt2_rx_poll(&v.rx,1)==1);
    }
    CHECK(v.rx.records==42 && v.rx.tail==3);
    /* A fragmented record can span both a poll budget and ring wrap. */
    received(3,"tail",4,0);received(0,"head",4,0x40000000);
    CHECK(mt2_rx_poll(&v.rx,1)==1 && v.rx.length==4 && v.records==42);
    CHECK(mt2_rx_poll(&v.rx,1)==1 && v.received_bytes==8 && !memcmp(v.received,"tailhead",8));
    receive_setup(4);received(0,"abcd",4,0);received(1,"e",1,0x40000000);
    CHECK(mt2_rx_poll(&v.rx,4)==2 && !v.records && v.rx.dropped==1 && !v.rx.dropping);
    received(2,"ok",2,0x40000000);CHECK(mt2_rx_poll(&v.rx,4)==1 && v.records==1);
    receive_setup(256);received(0,"bad",3,0x1000);received(1,"end",3,0x40000000);
    CHECK(mt2_rx_poll(&v.rx,4)==2 && !v.records && v.rx.dropped==1);
    receive_setup(256);received(0,"bad",3,0x40004000);
    CHECK(mt2_rx_poll(&v.rx,1)==1 && !v.records && v.rx.dropped==1);
    /* Descriptor corruption must not turn into a host out-of-bounds copy. */
    receive_setup(256);v.d[0].control=0xc0810000;
    CHECK(mt2_rx_poll(&v.rx,1)==MT2_PROTOCOL && !v.publishes && !v.records);
    receive_setup(256);v.d[0].control=0xc0000000;
    CHECK(mt2_rx_poll(&v.rx,1)==MT2_PROTOCOL && !v.publishes);
    receive_setup(256);received(0,"x",1,0x40000000);v.d[0].address+=4;
    CHECK(mt2_rx_poll(&v.rx,1)==MT2_PROTOCOL && !v.publishes);
    receive_setup(256);received(0,"x",1,0x40000000);v.d[0].address1=1;
    CHECK(mt2_rx_poll(&v.rx,1)==MT2_PROTOCOL && !v.publishes);
    receive_setup(256);received(0,"x",1,0x40000080);
    CHECK(mt2_rx_poll(&v.rx,1)==MT2_PROTOCOL && !v.publishes);
    receive_setup(256);received(0,"x",1,0x40000000);v.publish_error=-99;
    CHECK(mt2_rx_poll(&v.rx,1)==-99 && !v.records && v.rx.tail==1 && !v.rx.active);
    CHECK(mt2_rx_poll(&v.rx,1)==MT2_INVALID && mt2_rx_activate(&v.rx)==MT2_INVALID);
    receive_setup(256);received(0,"x",1,0x40000000);v.record_error=-98;
    CHECK(mt2_rx_poll(&v.rx,1)==-98 && v.records==1 && !v.rx.active);
    CHECK(mt2_rx_poll(&v.rx,1)==MT2_INVALID && v.records==1);
    receive_setup(256);mt2_rx_disarm(&v.rx);CHECK(mt2_rx_activate(&v.rx)==MT2_INVALID);
    puts("{\"dma_queue_tests\":\"passed\",\"transport\":\"simulation\",\"device_access\":false}");
}
