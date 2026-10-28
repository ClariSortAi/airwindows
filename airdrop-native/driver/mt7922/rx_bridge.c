/* SPDX-License-Identifier: BSD-3-Clause-Clear */
#include "rx_bridge.h"
static int overlap(uintptr_t a,uint64_t an,uintptr_t b,uint64_t bn) {
    return a<=b?(uint64_t)(b-a)<an:(uint64_t)(a-b)<bn;
}
static int span(const void *p,uint64_t bytes) {return p && bytes && bytes-1<=UINTPTR_MAX-(uintptr_t)p;}
int mt2_rx_bridge_prepare(mt2_rx_bridge *q,uint8_t *storage,uint32_t bytes,uint32_t *lengths,uint32_t count) {
    uint32_t i;uint64_t needed=(uint64_t)count*MT2_RX_MESSAGE_MAX,meta=(uint64_t)count*4;
    if(!q || q->prepared || q->used || count<2 || count>256 || bytes<needed ||
       !span(q,sizeof(*q)) || !span(storage,needed) || !span(lengths,meta) ||
       overlap((uintptr_t)q,sizeof(*q),(uintptr_t)storage,needed) ||
       overlap((uintptr_t)q,sizeof(*q),(uintptr_t)lengths,meta) ||
       overlap((uintptr_t)storage,needed,(uintptr_t)lengths,meta))return MT2_INVALID;
    q->storage=storage;q->lengths=lengths;q->count=count;
    for(i=0;i<count;++i)lengths[i]=0;
    q->prepared=1;return 0;
}
int mt2_rx_bridge_start(mt2_rx_bridge *q,uint32_t fcs) {
    if(!q || !q->prepared || q->used || q->active || fcs>1)return MT2_INVALID;
    q->fcs_present=fcs;q->active=1;q->used=1;return 0;
}
int mt2_rx_bridge_record(void *context,const uint8_t *data,uint32_t size) {
    mt2_rx_bridge *q=(mt2_rx_bridge *)context;uint32_t length=0,outcome=MT2_RX_MALFORMED;uint8_t unused=0;int e;
    if(!q || !q->prepared)return 0;
    if(!q->active){++q->inactive;return 0;}
    e=mt2_raw_rx_record(data,size,q->fcs_present,q->queued==q->count?&unused:q->storage+q->head*MT2_RX_MESSAGE_MAX,
                        q->queued==q->count?0:MT2_RX_MESSAGE_MAX,&length,&outcome);
    if(e) {
        if(e==MT2_FULL && outcome==MT2_RX_CAPACITY)++q->overflow;
        else if(outcome<9)++q->discarded[outcome];
        return 0;
    }
    q->lengths[q->head]=length;q->head=(q->head+1)%q->count;++q->queued;++q->accepted;return 0;
}
int mt2_rx_bridge_take(mt2_rx_bridge *q,uint8_t *out,uint32_t capacity,uint32_t *length) {
    uint32_t i,n;uint8_t *source;
    if(!q || !q->prepared || !q->active || !out || !length)return MT2_INVALID;
    if(!q->queued)return MT2_PENDING;n=q->lengths[q->tail];
    if(!n || n>MT2_RX_MESSAGE_MAX)return MT2_PROTOCOL;
    if(capacity<n)return MT2_FULL;
    source=q->storage+q->tail*MT2_RX_MESSAGE_MAX;
    for(i=0;i<n;++i){out[i]=source[i];source[i]=0;}
    *length=n;q->lengths[q->tail]=0;q->tail=(q->tail+1)%q->count;--q->queued;++q->taken;return 0;
}
void mt2_rx_bridge_stop(mt2_rx_bridge *q) {
    uint32_t i,j;if(!q || !q->prepared)return;
    q->active=0;q->used=1;
    for(i=0;i<q->count;++i) {
        uint32_t n=q->lengths[i];if(n>MT2_RX_MESSAGE_MAX)n=MT2_RX_MESSAGE_MAX;
        for(j=0;j<n;++j)q->storage[i*MT2_RX_MESSAGE_MAX+j]=0;
        q->lengths[i]=0;
    }
    q->head=0;q->tail=0;q->queued=0;
}
