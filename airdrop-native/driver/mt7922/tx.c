/* SPDX-License-Identifier: BSD-3-Clause-Clear
 * Copyright (C) 2020, 2021, 2022 MediaTek Inc.
 * Connac2 wire formats adapted from mt76_connac_mac.c, mt76_connac2_mac.h,
 * mt76_connac.h and mt7921/mac.c. Queue lifetime follows the two-completion
 * discipline in the referenced Windows MT7925 port. See SOURCES.md.
 */
#include "tx.h"
static uint32_t le16(const uint8_t *p) { return p[0] | (uint32_t)p[1]<<8; }
static uint32_t le32(const uint8_t *p) { return le16(p) | le16(p+2)<<16; }
static void put16(uint8_t *p,uint32_t v) { p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8); }
static void put32(uint8_t *p,uint32_t v) { put16(p,v);put16(p+2,v>>16); }
static int span32(uint64_t address,uint32_t size) {
    return size && address<=0xffffffffULL && (uint64_t)(size-1)<=0xffffffffULL-address;
}
static int fault(mt2_tx_queue *q,int result) { q->fault=result;q->active=0;return result; }
static int live(const mt2_tx_queue *q) { return q && q->prepared && q->active && !q->fault; }

int mt2_tx_encode(const uint8_t *frame,uint32_t size,uint64_t address,
                  uint32_t token,const mt2_tx_config *c,uint8_t *out,uint32_t capacity) {
    uint32_t fc,type,sub,header=24,tid=0,multicast,seq,i;
    if (!frame || !c || !out || size<24 || size>MT2_PAYLOAD_MAX || capacity<64 ||
        !span32(address,size) || token>=MT2_TOKEN_MAX || c->wcid>1023 || c->own_mac>63 ||
        c->spe_index>31 || c->rate<0x48 || c->rate>0x4f || c->no_ack>1) return MT2_INVALID;
    fc=le16(frame);type=(fc>>2)&3u;sub=(fc>>4)&15u;seq=le16(frame+22);
    if ((fc&0xc703u) || (seq&15u) ||
        !((type==0 && sub==13) || (type==2 && (sub==0 || sub==8)))) return MT2_INVALID;
    if (type==0 && size<25) return MT2_INVALID;
    if (type==2 && sub==8) {
        if (size<26 || (frame[24]&0xf8u)) return MT2_INVALID;
        header=26;tid=frame[24]&7u;
    }
    if (size<header) return MT2_INVALID;
    multicast=frame[4]&1u;
    for (i=0;i<64;++i) out[i]=0;
    put32(out,((type==2?1u:16u)<<25)|(size+32u));
    put32(out+4,0x80000000u|c->own_mac<<24|tid<<20|2u<<16|(header/2u)<<11|c->wcid);
    put32(out+8,0x80002000u|multicast<<10|type<<4|sub);
    put32(out+12,0x90007800u|(seq>>4)<<16|((c->no_ack||multicast)?1u:0u));
    put32(out+20,1u); /* No host TXS requested; firmware release owns lifetime. */
    put32(out+24,c->rate<<16|4u);
    put32(out+28,type<<20|sub<<16|c->spe_index<<11);
    put16(out+32,token|0x8000u);
    put32(out+40,(uint32_t)address);put16(out+44,size|0x8000u);
    return 64;
}

int mt2_tx_prepare(mt2_tx_queue *q,mt2_descriptor *d,uint64_t da,uint32_t count,
                   uint8_t *pool,uint64_t pa,uint32_t bytes,uint32_t *states,
                   uint32_t tokens,uint32_t *slots,const mt2_tx_io *io) {
    uint32_t i,needed;
    if (!q || q->prepared || !d || !pool || !states || !slots || !io ||
        !io->barrier || !io->publish || count<4 || count>2048 || !tokens ||
        tokens>MT2_TOKEN_MAX || (da&15u) || (pa&3u)) return MT2_INVALID;
    needed=tokens*MT2_SLOT_SIZE;
    if (bytes<needed || !span32(da,count*16u) || !span32(pa,needed) ||
        (da<pa+needed && pa<da+count*16u)) return MT2_INVALID;
    q->descriptors=d;q->descriptor_address=da;q->pool=pool;q->pool_address=pa;q->states=states;q->slots=slots;
    q->count=count;q->tokens=tokens;q->io=*io;
    q->head=0;q->tail=0;q->queued=0;q->inflight=0;q->cursor=0;
    q->active=0;q->used=0;q->fault=0;q->prepared=1;
    for (i=0;i<tokens;++i) states[i]=0;
    for (i=0;i<count;++i) {
        d[i].address=0;d[i].address1=0;d[i].info=0;d[i].control=MT2_DMA_DONE;
        slots[i]=0xffffffffu;
    }
    return MT2_OK;
}
int mt2_tx_activate(mt2_tx_queue *q) {
    if (!q || !q->prepared || q->used || q->fault) return MT2_INVALID;
    q->used=1;q->active=1;return MT2_OK;
}
void mt2_tx_disarm(mt2_tx_queue *q) { if(q) {q->active=0;q->used=1;} }
int mt2_tx_submit(mt2_tx_queue *q,const uint8_t *frame,uint32_t size,
                  const mt2_tx_config *c,uint32_t *token) {
    uint32_t i,id,index;uint64_t address;uint8_t header[64],*buffer;int result;
    if (!live(q) || !token) return MT2_INVALID;
    if (q->queued>=q->count-2u || q->inflight>=q->tokens) return MT2_FULL;
    for (i=0;i<q->tokens;++i) if (!q->states[(q->cursor+i)%q->tokens]) break;
    if (i==q->tokens) return fault(q,MT2_PROTOCOL);
    index=q->head;
    if (!(((volatile mt2_descriptor *)q->descriptors)[index].control&MT2_DMA_DONE)) return MT2_PENDING;
    id=(q->cursor+i)%q->tokens;address=q->pool_address+(uint64_t)id*MT2_SLOT_SIZE;
    result=mt2_tx_encode(frame,size,address+64,id,c,header,sizeof(header));
    if (result<0) return result;
    buffer=q->pool+id*MT2_SLOT_SIZE;
    for (i=0;i<64;++i) buffer[i]=header[i];
    for (i=0;i<size;++i) buffer[64+i]=frame[i];
    q->descriptors[index].address=(uint32_t)address;
    q->descriptors[index].address1=0;q->descriptors[index].info=0;
    q->descriptors[index].control=(64u<<16)|0x40000000u;
    q->slots[index]=id;q->states[id]=1;++q->inflight;++q->queued;
    q->head=(index+1u)%q->count;q->cursor=(id+1u)%q->tokens;
    q->io.barrier(q->io.context);
    result=q->io.publish(q->io.context,q->head);
    if (result) return fault(q,result); /* DMA may already own this descriptor. */
    *token=id;return MT2_OK;
}
static void release(mt2_tx_queue *q,uint32_t id) {
    if (q->states[id]==7u) {q->states[id]=0;--q->inflight;}
}
int mt2_tx_dma_complete(mt2_tx_queue *q,uint32_t next) {
    uint32_t count,i,index,id;
    if (!live(q)) return MT2_INVALID;
    if (next>=q->count) return fault(q,MT2_PROTOCOL);
    q->io.barrier(q->io.context);
    count=(next+q->count-q->tail)%q->count;
    if (count>q->queued) return fault(q,MT2_PROTOCOL);
    for (i=0;i<count;++i) {
        id=q->slots[(q->tail+i)%q->count];
        if (id>=q->tokens || !(q->states[id]&1u) || (q->states[id]&2u)) return fault(q,MT2_PROTOCOL);
    }
    for (i=0;i<count;++i) {
        index=q->tail;id=q->slots[index];q->slots[index]=0xffffffffu;
        q->descriptors[index].control=MT2_DMA_DONE;
        q->tail=(q->tail+1u)%q->count;--q->queued;q->states[id]|=2u;release(q,id);
    }
    return count?MT2_OK:MT2_PENDING;
}
int mt2_tx_firmware_complete(mt2_tx_queue *q,const uint8_t *data,uint32_t size) {
    uint16_t ids[1023];uint32_t declared,total,n=0,offset,word,id,i;
    if (!live(q)) return MT2_INVALID;
    if (!data || size<8) return fault(q,MT2_PROTOCOL);
    word=le32(data);declared=word&0xffffu;total=(word>>16)&0x3ffu;
    if ((word>>27)!=6 || declared<8 || declared>size || (declared&3u) || total>q->tokens)
        return fault(q,MT2_PROTOCOL);
    for (offset=8;offset<declared;offset+=4) {
        word=le32(data+offset);
        if (word&0x80000000u) continue; /* Connac2 WCID pair, not token. */
        id=(word>>16)&0x7fffu;
        if (n>=total || id>=q->tokens || !(q->states[id]&1u) || (q->states[id]&4u))
            return fault(q,MT2_PROTOCOL);
        for (i=0;i<n;++i) if (ids[i]==id) return fault(q,MT2_PROTOCOL);
        ids[n++]=(uint16_t)id;
    }
    if (n!=total) return fault(q,MT2_PROTOCOL);
    /* Entire batch validates before changing any ownership. */
    for (i=0;i<n;++i) {q->states[ids[i]]|=4u;release(q,ids[i]);}
    return MT2_OK;
}
