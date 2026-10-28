/* SPDX-License-Identifier: BSD-3-Clause-Clear */
#include "rx_bridge.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"RX BRIDGE FAIL %d: %s\n",__LINE__,#x);exit(1);}}while(0)
uint32_t test_raw_packet(uint8_t *);
static mt2_rx_bridge q;
static uint8_t storage[2*MT2_RX_MESSAGE_MAX];static uint32_t lengths[2];
static void setup(uint32_t fcs) {
    memset(&q,0,sizeof(q));memset(storage,0xa5,sizeof(storage));
    CHECK(mt2_rx_bridge_prepare(&q,storage,sizeof(storage),lengths,2)==0);
    CHECK(mt2_rx_bridge_start(&q,fcs)==0);
}
void test_rx_bridge(void) {
    uint8_t packet[128],output[128],expected[128],before[128];uint32_t bytes,n,expected_size,outcome,i;
    memset(&q,0,sizeof(q));
    CHECK(mt2_rx_bridge_prepare(&q,storage,sizeof(storage)-1,lengths,2)==MT2_INVALID && !q.prepared);
    CHECK(mt2_rx_bridge_prepare(&q,storage,sizeof(storage),(uint32_t *)storage,2)==MT2_INVALID && !q.prepared);
    CHECK(mt2_rx_bridge_prepare(&q,storage,sizeof(storage),lengths,2)==0);
    bytes=test_raw_packet(packet);CHECK(mt2_rx_bridge_record(&q,packet,bytes)==0 && q.inactive==1 && !q.queued);
    CHECK(mt2_rx_bridge_start(&q,2)==MT2_INVALID && !q.used);
    CHECK(mt2_rx_bridge_start(&q,0)==0);
    CHECK(mt2_raw_rx_record(packet,bytes,0,expected,sizeof(expected),&expected_size,&outcome)==0);
    CHECK(mt2_rx_bridge_record(&q,packet,bytes)==0 && q.queued==1);
    memset(output,0xcc,sizeof(output));memcpy(before,output,sizeof(output));n=99;
    CHECK(mt2_rx_bridge_take(&q,output,expected_size-1,&n)==MT2_FULL && n==99 && !memcmp(output,before,sizeof(output)) && q.queued==1);
    CHECK(mt2_rx_bridge_take(&q,output,sizeof(output),&n)==0 && n==expected_size && !memcmp(output,expected,n));
    for(i=0;i<n;++i)CHECK(!storage[i]);
    n=99;CHECK(mt2_rx_bridge_take(&q,output,sizeof(output),&n)==MT2_PENDING && n==99);
    for(i=0;i<30;++i){CHECK(mt2_rx_bridge_record(&q,packet,bytes)==0);CHECK(mt2_rx_bridge_take(&q,output,sizeof(output),&n)==0 && !memcmp(output,expected,n));}
    CHECK(mt2_rx_bridge_record(&q,packet,bytes)==0 && mt2_rx_bridge_record(&q,packet,bytes)==0 && q.queued==2);
    CHECK(mt2_rx_bridge_record(&q,packet,bytes)==0 && q.overflow==1 && q.queued==2);
    packet[3]=0x38;CHECK(mt2_rx_bridge_record(&q,packet,bytes)==0 && q.overflow==1 && q.discarded[MT2_RX_OTHER]==1);
    bytes=test_raw_packet(packet);packet[0]=1;CHECK(mt2_rx_bridge_record(&q,packet,bytes)==0 && q.discarded[MT2_RX_MALFORMED]==1 && q.active);
    CHECK(mt2_rx_bridge_take(&q,output,sizeof(output),&n)==0 && !memcmp(output,expected,n));
    mt2_rx_bridge_stop(&q);CHECK(!q.active && !q.queued && mt2_rx_bridge_start(&q,0)==MT2_INVALID);
    CHECK(!lengths[0] && !lengths[1]);
    for(i=0;i<expected_size;++i)CHECK(!storage[i] && !storage[MT2_RX_MESSAGE_MAX+i]);
    setup(1);bytes=test_raw_packet(packet);
    CHECK(mt2_rx_bridge_record(&q,packet,bytes)==0 && mt2_rx_bridge_take(&q,output,sizeof(output),&n)==0 && output[16]==0x10);
    mt2_rx_bridge_stop(&q);
    puts("{\"rx_bridge_tests\":\"passed\",\"overflow\":\"drop_newest\",\"device_access\":false}");
}
