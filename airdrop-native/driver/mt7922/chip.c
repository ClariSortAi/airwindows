/* SPDX-License-Identifier: BSD-3-Clause-Clear
 * Copyright (C) 2020, 2021, 2023 MediaTek Inc.
 * MT7922 register/sequence definitions: mt7921/pci.c, mt7921/regs.h,
 * mt7921/pci_mcu.c, mt792x_core.c and mt792x_dma.c at the pinned mt76 commit.
 * Validation, selector restoration and explicit DMA ownership are port additions.
 */
#include "chip.h"
typedef struct reg_map {uint32_t physical,offset,size;} reg_map;
#include "register_map.inc"
#define REMAP 0xfe24cu
#define WINDOW 0x40000u
#define DMA_CFG 0xd4208u
static const uint32_t ring_registers[MT2_RING_COUNT]={0xd4300u,0xd4410u,0xd4400u,0xd4500u,0xd4520u,0xd4540u};
static int valid_io(const mt2_register_io *io) {
    return io && io->bar_bytes>=0x100000 && io->read && io->write && io->barrier;
}
static int raw_read(const mt2_register_io *io,uint32_t offset,uint32_t *value) {
    uint32_t v;int e=io->read(io->context,offset,&v);
    if(e) return e;
    if(v==0xffffffffu) return MT2_PROTOCOL;
    *value=v;return 0;
}
static int raw_write(const mt2_register_io *io,uint32_t offset,uint32_t value) {
    io->barrier(io->context);return io->write(io->context,offset,value);
}
static int access_register(const mt2_register_io *io,uint32_t address,uint32_t *value,int write) {
    uint32_t i,offset=0,saved,mapped,readback=0,temporary=0;int found=0,e,restore;
    if(!valid_io(io) || !value || (address&3u)) return MT2_INVALID;
    if(address<0x100000) {offset=address;found=1;}
    else for(i=0;i<sizeof(fixed_map)/sizeof(fixed_map[0]);++i) {
        const reg_map *m=&fixed_map[i];
        if(address>=m->physical && address-m->physical<=m->size-4u) {
            offset=m->offset+(address-m->physical);found=1;break;
        }
    }
    if(found) {
        if(offset>io->bar_bytes-4u) return MT2_INVALID;
        return write?raw_write(io,offset,*value):raw_read(io,offset,value);
    }
    if(!((address>=0x18000000 && address<0x18c00000) ||
         (address>=0x70000000 && address<0x78000000) ||
         (address>=0x7c000000 && address<0x7c400000))) return MT2_INVALID;
    e=raw_read(io,REMAP,&saved);if(e) return e;
    mapped=(saved&0xffff0000u)|(address>>16);
    e=raw_write(io,REMAP,mapped);
    if(!e) e=raw_read(io,REMAP,&readback);
    if(!e && readback!=mapped) e=MT2_PROTOCOL;
    if(!e) e=write?raw_write(io,WINDOW+(address&0xffffu),*value):
                   raw_read(io,WINDOW+(address&0xffffu),&temporary);
    /* Failed MMIO writes may still have reached the device. Restore even zero. */
    restore=raw_write(io,REMAP,saved);
    if(!restore) restore=raw_read(io,REMAP,&readback);
    if(!restore && readback!=saved) restore=MT2_PROTOCOL;
    if(e) return e;
    if(restore) return restore;
    if(!write) *value=temporary;
    return 0;
}
int mt2_reg_read(const mt2_register_io *io,uint32_t a,uint32_t *v){return access_register(io,a,v,0);}
int mt2_reg_write(const mt2_register_io *io,uint32_t a,uint32_t v){return access_register(io,a,&v,1);}
static int rd(mt2_chip *c,uint32_t a,uint32_t *v){return mt2_reg_read(&c->io,a,v);}
static int wr(mt2_chip *c,uint32_t a,uint32_t v){return mt2_reg_write(&c->io,a,v);}
static int rmw(mt2_chip *c,uint32_t a,uint32_t clear,uint32_t set) {
    uint32_t v;int e=rd(c,a,&v);return e?e:wr(c,a,(v&~clear)|set);
}
static int equal(mt2_chip *c,uint32_t a,uint32_t mask,uint32_t expected) {
    uint32_t v;int e=rd(c,a,&v);return e?e:((v&mask)==expected?0:MT2_PROTOCOL);
}
static int poll(mt2_chip *c,uint32_t a,uint32_t mask,uint32_t expected,uint32_t limit) {
    uint64_t start=c->io.milliseconds(c->io.context);uint32_t i,v;int e;
    for(i=0;i<=limit;++i) {
        e=rd(c,a,&v);if(e) return e;
        if((v&mask)==expected) return 0;
        if(c->io.milliseconds(c->io.context)-start>=limit) break;
        e=c->io.delay_ms(c->io.context,1);if(e) return e;
    }
    return MT2_FW_TIMEOUT;
}
static int ownership(mt2_chip *c,int firmware) {
    uint32_t i;int e;
    for(i=0;i<10;++i) {
        e=wr(c,0x7c060010u,firmware?1u:2u);if(e) return e;
        if(!firmware) {e=c->io.delay_ms(c->io.context,3);if(e) return e;}
        e=poll(c,0x7c060010u,4,firmware?4u:0u,50);
        if(e!=MT2_FW_TIMEOUT) return e;
    }
    return MT2_FW_TIMEOUT;
}
static int mask_interrupts(mt2_chip *c) {
    static const uint32_t addresses[]={0xd4204u,0x10188u,0xd41f4u};uint32_t i;int e;
    for(i=0;i<3;++i){e=wr(c,addresses[i],0);if(e)return e;e=equal(c,addresses[i],0xffffffffu,0);if(e)return e;}
    return 0;
}
static int disable_dma(mt2_chip *c) {
    int e=rmw(c,DMA_CFG,0x18208005u,0);
    if(!e)e=poll(c,DMA_CFG,15,0,100); /* Require enables AND busy bits clear. */
    if(!e)e=rmw(c,0xd42b0u,0x40,0);
    if(!e)e=rmw(c,0xd6004u,0,0x10000000);
    if(!e)e=rmw(c,0xd4100u,0x30,0);
    if(!e)e=rmw(c,0xd4100u,0,0x30);
    return e;
}
static int ring(mt2_chip *c,uint32_t reg,uint32_t address,uint32_t count,uint32_t cpu) {
    int e=wr(c,reg+4,0);
    if(!e)e=wr(c,reg,address);
    if(!e)e=wr(c,reg+8,cpu);
    if(!e)e=wr(c,reg+4,count);
    if(!e)e=equal(c,reg,0xffffffffu,address);
    if(!e)e=equal(c,reg+4,0xffffffffu,count);
    if(!e)e=equal(c,reg+8,0xffffffffu,cpu);
    return e;
}
static int clear_rings(mt2_chip *c) {
    uint32_t i;int e;
    for(i=0;i<18;++i){e=ring(c,0xd4300u+i*16,0,0,0);if(e)return e;}
    for(i=0;i<6;++i){e=ring(c,0xd4500u+i*16,0,0,0);if(e)return e;}
    return 0;
}
static int setup_dma(mt2_chip *c) {
    static const uint32_t prefetch[][2]={
        {0xd4680,0x00000004},{0xd4688,0x00400004},{0xd468c,0x00800004},
        {0xd4690,0x00c00004},{0xd4694,0x01000004},
        {0xd4600,0x01400004},{0xd4604,0x01800004},{0xd4608,0x01c00004},
        {0xd460c,0x02000004},{0xd4610,0x02400004},{0xd4614,0x02800004},
        {0xd4618,0x02c00004},{0xd4640,0x03400004},{0xd4644,0x03800004}};
    uint32_t i;int e=clear_rings(c);
    for(i=0;!e && i<sizeof(prefetch)/sizeof(prefetch[0]);++i)e=wr(c,prefetch[i][0],prefetch[i][1]);
    if(!e)e=wr(c,0xd420cu,0xffffffffu);
    if(!e)e=wr(c,0xd42f0u,0);
    if(e)return e;
    c->published=1;c->quiescent=0; /* Before the first potentially visible address. */
    for(i=0;!e && i<MT2_RING_COUNT;++i) e=ring(c,ring_registers[i],(uint32_t)c->rings[i].address,
        c->rings[i].count,i>=MT2_RING_RX_EVENT?c->rings[i].count-1:0);
    if(!e)e=rmw(c,DMA_CFG,0x0c000035u,0x5020b870u);
    if(!e)e=rmw(c,DMA_CFG,0,5);
    if(!e)e=equal(c,DMA_CFG,5,5);
    if(!e)e=rmw(c,0x54000120u,0,2);
    return e;
}
int mt2_chip_start(mt2_chip *c,const mt2_register_io *io,const mt2_ring_binding *rings) {
    uint32_t i,j;int e;
    if(!c || c->attempted || !valid_io(io) || !io->milliseconds || !io->delay_ms || !rings) return MT2_INVALID;
    for(i=0;i<MT2_RING_COUNT;++i) {
        uint64_t address=rings[i].address,bytes=(uint64_t)rings[i].count*16;
        if(rings[i].count<4 || rings[i].count>2048 || (address&15u) || address>0xffffffffULL ||
           bytes-1>0xffffffffULL-address) return MT2_INVALID;
        for(j=0;j<i;++j) if(address<rings[j].address+(uint64_t)rings[j].count*16 && rings[j].address<address+bytes)
            return MT2_INVALID;
    }
    c->io=*io;for(i=0;i<MT2_RING_COUNT;++i)c->rings[i]=rings[i];
    c->attempted=1;c->published=0;c->quiescent=1;c->active=0;c->stop_error=0;
    e=ownership(c,1);if(!e)e=ownership(c,0);
    if(!e)e=rmw(c,0x18000140u,1,0);
    if(!e)e=c->io.delay_ms(c->io.context,50);
    if(!e)e=rmw(c,0x18000140u,0,1);
    if(!e)e=poll(c,0x18000140u,0x11,0x11,500);
    if(!e)e=mask_interrupts(c);
    if(!e)e=disable_dma(c);
    if(!e)e=setup_dma(c);
    /* MT7921/MT7922 MCU ownership uses TOP host bit0, not PCIe sync bit2. */
    if(!e)e=wr(c,0x18060010u,2);
    if(!e)e=poll(c,0x18060010u,1,0,500);
    if(!e)e=rmw(c,0x10194u,0,0x100);
    if(!e)e=equal(c,0x10194u,0x100,0x100);
    /* mt7921 __init_hardware selects normal operation before downloading
     * firmware. Never inherit ICAP/spectrum mode from earlier diagnostics. */
    if(!e)e=wr(c,0x0041f23cu,0);
    if(!e)e=equal(c,0x0041f23cu,0xffffffffu,0);
    c->start_error=e;c->active=!e;return e;
}
int mt2_chip_stop(mt2_chip *c) {
    int e;
    if(!c || !c->attempted)return MT2_INVALID;
    c->active=0;
    if(!c->published || c->quiescent)return 0;
    e=ownership(c,0);
    if(!e)e=mask_interrupts(c);
    if(!e)e=disable_dma(c);
    if(!e)e=clear_rings(c);
    if(!e)e=equal(c,DMA_CFG,15,0);
    if(!e)c->quiescent=1;
    c->stop_error=e;return e;
}
