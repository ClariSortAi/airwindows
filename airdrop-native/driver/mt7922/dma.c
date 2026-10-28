/* SPDX-License-Identifier: BSD-3-Clause-Clear
 * Copyright (C) 2016 Felix Fietkau <nbd@nbd.name>
 * DMA fields and ring convention: pinned mt76 dma.c/dma.h.
 * Fixed storage and ownership validation are native-port additions.
 */
#include "dma.h"
#define LAST0 0x40000000u
static int span(uint64_t address,uint64_t bytes) {
    return bytes && address<=0xffffffffULL && bytes-1<=0xffffffffULL-address;
}
static int memory_valid(const mt2_dma_memory *m) {
    uint64_t descriptors,bytes;
    if(!m || !m->descriptors || !m->pool || m->count<4 || m->count>2048 ||
       !m->slot_bytes || m->slot_bytes>MT2_MCU_MAX_PACKET || (m->slot_bytes&3u) ||
       (m->descriptor_address&15u) || (m->pool_address&3u))return 0;
    descriptors=(uint64_t)m->count*16;bytes=(uint64_t)m->count*m->slot_bytes;
    return m->pool_bytes>=bytes && span(m->descriptor_address,descriptors) &&
        span(m->pool_address,bytes) && !(m->descriptor_address<m->pool_address+bytes &&
        m->pool_address<m->descriptor_address+descriptors);
}
static int command_fault(mt2_command_queue *q,int e){q->fault=e;q->active=0;return e;}
int mt2_command_prepare(mt2_command_queue *q,const mt2_dma_memory *m,const mt2_tx_io *io,int download) {
    uint32_t i;
    if(!q || q->prepared || !memory_valid(m) || !io || !io->barrier || !io->publish ||
       (download!=0 && download!=1))return MT2_INVALID;
    q->memory=*m;q->io=*io;q->download=(uint32_t)download;
    q->head=0;q->tail=0;q->queued=0;q->active=0;q->used=0;q->fault=0;q->prepared=1;
    for(i=0;i<m->count;++i) {
        m->descriptors[i].address=0;m->descriptors[i].address1=0;
        m->descriptors[i].info=0;m->descriptors[i].control=MT2_DMA_DONE;
    }
    return 0;
}
int mt2_command_activate(mt2_command_queue *q) {
    if(!q || !q->prepared || q->used || q->fault)return MT2_INVALID;
    q->active=1;q->used=1;return 0;
}
void mt2_command_disarm(mt2_command_queue *q){if(q){q->active=0;q->used=1;}}
int mt2_command_submit(mt2_command_queue *q,uint32_t command,uint8_t *sequence,const uint8_t *p,uint32_t bytes) {
    mt2_descriptor *d;uint32_t index;uint8_t next_sequence;int size,e;
    if(!q || !q->active || q->fault || !sequence ||
       q->download!=(uint32_t)(command==MT2_FW_SCATTER))return MT2_INVALID;
    if(q->queued>=q->memory.count-2)return MT2_FULL;
    index=q->head;d=&q->memory.descriptors[index];
    if(!(((volatile mt2_descriptor *)d)->control&MT2_DMA_DONE))return MT2_PENDING;
    next_sequence=*sequence;
    size=mt2_mcu_encode(command,&next_sequence,p,bytes,
        q->memory.pool+index*q->memory.slot_bytes,q->memory.slot_bytes);
    if(size<0)return size;
    d->address=(uint32_t)(q->memory.pool_address+(uint64_t)index*q->memory.slot_bytes);
    d->address1=0;d->info=0;d->control=((uint32_t)size<<16)|LAST0;
    q->head=(index+1)%q->memory.count;++q->queued;*sequence=next_sequence;
    q->io.barrier(q->io.context);
    e=q->io.publish(q->io.context,q->head);
    return e?command_fault(q,e):0;
}
int mt2_command_complete(mt2_command_queue *q,uint32_t next) {
    uint32_t completed,i,index;
    if(!q || !q->active || q->fault)return MT2_INVALID;
    if(next>=q->memory.count)return command_fault(q,MT2_PROTOCOL);
    completed=(next+q->memory.count-q->tail)%q->memory.count;
    if(completed>q->queued)return command_fault(q,MT2_PROTOCOL);
    if(!completed)return MT2_PENDING;
    q->io.barrier(q->io.context);
    for(i=0;i<completed;++i) {
        index=q->tail;q->memory.descriptors[index].control=MT2_DMA_DONE;
        q->tail=(index+1)%q->memory.count;--q->queued;
    }
    return 0;
}
static void refill(mt2_rx_queue *q,uint32_t index) {
    mt2_descriptor *d=&q->memory.descriptors[index];
    d->address=(uint32_t)(q->memory.pool_address+(uint64_t)index*q->memory.slot_bytes);
    d->address1=0;d->info=0;d->control=q->memory.slot_bytes<<16;
}
static int rx_fault(mt2_rx_queue *q,int e){q->fault=e;q->active=0;return e;}
int mt2_rx_prepare(mt2_rx_queue *q,const mt2_dma_memory *m,uint8_t *assembly,uint32_t cap,const mt2_rx_io *io) {
    uint32_t i;
    if(!q || q->prepared || !memory_valid(m) || !assembly || !cap || cap>MT2_RX_RECORD_MAX ||
       !io || !io->barrier || !io->publish || !io->record)return MT2_INVALID;
    q->memory=*m;q->io=*io;q->assembly=assembly;q->capacity=cap;
    q->length=0;q->tail=0;q->dropping=0;q->active=0;q->used=0;q->fault=0;
    q->records=0;q->dropped=0;q->prepared=1;
    for(i=0;i<m->count;++i)refill(q,i);
    return 0;
}
int mt2_rx_activate(mt2_rx_queue *q) {
    if(!q || !q->prepared || q->used || q->fault)return MT2_INVALID;
    q->active=1;q->used=1;return 0;
}
void mt2_rx_disarm(mt2_rx_queue *q){if(q){q->active=0;q->used=1;}}
int mt2_rx_poll(mt2_rx_queue *q,uint32_t budget) {
    uint32_t consumed,index,ctrl,size,i,final;volatile mt2_descriptor *d;int e;
    if(!q || !q->active || q->fault || !budget || budget>q->memory.count)return MT2_INVALID;
    for(consumed=0;consumed<budget;++consumed) {
        index=q->tail;d=(volatile mt2_descriptor *)&q->memory.descriptors[index];
        ctrl=d->control;if(!(ctrl&MT2_DMA_DONE))break;
        q->io.barrier(q->io.context); /* Acquire payload after observing DMA_DONE. */
        size=(ctrl>>16)&0x3fffu;final=ctrl&LAST0;
        if(!size || size>q->memory.slot_bytes ||
           d->address!=(uint32_t)(q->memory.pool_address+(uint64_t)index*q->memory.slot_bytes) ||
           d->address1 || (ctrl&0x80u))return rx_fault(q,MT2_PROTOCOL); /* no WED */
        if((ctrl&0x5000u) || size>q->capacity-q->length)q->dropping=1;
        if(!q->dropping) {
            for(i=0;i<size;++i)q->assembly[q->length+i]=q->memory.pool[index*q->memory.slot_bytes+i];
            q->length+=size;
        }
        refill(q,index);q->tail=(index+1)%q->memory.count;
        q->io.barrier(q->io.context); /* Release rebuilt descriptor before doorbell. */
        e=q->io.publish(q->io.context,index);
        if(e)return rx_fault(q,e); /* No retry/replay; device may have seen it. */
        if(final) {
            if(q->dropping)++q->dropped;
            else {
                e=q->io.record(q->io.context,q->assembly,q->length);
                if(e)return rx_fault(q,e);
                ++q->records;
            }
            q->length=0;q->dropping=0;
        }
    }
    return (int)consumed;
}
