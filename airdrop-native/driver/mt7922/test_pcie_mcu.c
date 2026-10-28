/* SPDX-License-Identifier: BSD-3-Clause-Clear */
#include "pcie_mcu.h"
#include "raw_rx.h"
#include "mac_init.h"
#include "channel_control.h"
#include "tx_power.h"
#include "monitor_context.h"
#include "radio_tx.h"
#include "radio_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"PCIE FAIL line %d: %s\n",__LINE__,#x);exit(1);}}while(0)
typedef struct device_model {
    uint32_t bar[0x100000/4],reset,top_own;
    mt2_chip chip;mt2_pcie_mcu mcu;mt2_fw_transport transport;
    mt2_mcu_bank banks[MT2_MCU_BANKS];
    mt2_descriptor descriptors[MT2_MCU_BANKS][4];
    uint8_t pools[MT2_MCU_BANKS][4*8192],assemblies[3][512];
    uint32_t reads,writes,barriers,commands,scatter_bytes,remaining,releases,rx_index,rx_credit;
    uint32_t running,hold_dma,omit_reply,wrong_sequence,unsolicited,short_reply;
    uint32_t wrong_eid,duplicate_reply,hold_rom,hold_ready,freeze_time,write_fail,all_ones_read;
    uint32_t extra_records,reentrant,reply_early;
    uint32_t io_count,fail_io,ignore_write,wtbl_writes,hold_wtbl,wtbl_stall_after;
    uint32_t volatile_counters,early_mac_ack,early_regd;
    uint32_t hw_invalid,hw_wrong_address,clc_packets,omit_clc_at,early_clc,no_clc_events;
    uint32_t radio_count,radio_id[32],radio_len[32],omit_radio_at,hold_radio_at,radio_ack_status;
    uint8_t radio_payload[32][348];
    uint32_t sku_packets,sku_items[3],sku_last,pse_reads,pse_wrong_address,omit_pse_at,hold_sku_at;
    uint32_t context_count,context_id[4],context_status,context_fail_at,context_omit_at,context_hold_at;
    uint8_t context_payload[4][36];
    uint64_t time,scatter_hash;
    uint8_t radio_record[256];uint32_t radio_bytes,radio_outcome;
} device_model;
static device_model v;
uint32_t test_nic_payload(uint8_t *);
uint32_t test_raw_packet(uint8_t *);
uint32_t test_regd_image(uint8_t *);
uint32_t test_regd_event(uint8_t *,uint8_t);
uint32_t test_clc_image(uint8_t *);
uint32_t test_hw_event(uint8_t *,uint8_t);
uint32_t test_clc_event(uint8_t *,uint8_t);
static uint32_t get32(const uint8_t *p){return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static void put16(uint8_t *p,uint32_t x){p[0]=(uint8_t)x;p[1]=(uint8_t)(x>>8);}
static void put32(uint8_t *p,uint32_t x){put16(p,x);put16(p+2,x>>16);}
static void be32(uint8_t *p,uint32_t x){p[3]=(uint8_t)x;p[2]=(uint8_t)(x>>8);p[1]=(uint8_t)(x>>16);p[0]=(uint8_t)(x>>24);}
static void barrier(void *ctx){++((device_model *)ctx)->barriers;}
static void response(device_model *s,uint32_t cmd,uint8_t seq,uint32_t status) {
    uint32_t index=s->rx_index,bytes=cmd==7 || cmd==0x10?33u:36u;
    uint8_t *p=s->pools[2]+index*8192;mt2_descriptor *d=&s->descriptors[2][index];
    CHECK(s->rx_credit && !(d->control&MT2_DMA_DONE));--s->rx_credit;
    CHECK(d->address==0x220000+index*8192 && d->control==0x20000000);
    memset(p,0,128);
    if(cmd==0x8a)bytes=36+test_nic_payload(p+36);
    if(cmd==0x8a && s->no_clc_events)p[91]&=~1u;
    if(cmd==0xd1)bytes=test_regd_event(p,seq);
    if(cmd==MT2_HW_TYPE_COMMAND) {
        bytes=test_hw_event(p,seq);
        if(s->hw_invalid)put32(p+40,0);
        if(s->hw_wrong_address)put32(p+36,0x560);
    }
    if(cmd==MT2_CLC_COMMAND)bytes=test_clc_event(p,seq);
    if(cmd==MT2_MCU_PSE_READ){bytes=44;put32(p+36,s->pse_wrong_address?0x820c8004u:0x820c8000u);put32(p+40,0xf1234567u);}
    if((cmd&MT2_MCU_UNI) && cmd!=MT2_MCU_SNIFFER)bytes=44;
    put32(p,0x38000000u|bytes);put16(p+24,bytes-24);put16(p+26,0xe000);
    p[28]=(uint8_t)(cmd==0x10?4:cmd==0x8a?0xec:1);p[29]=seq;
    if(cmd==0x10 || cmd==7)p[32]=(uint8_t)status;
    else if((cmd&MT2_MCU_UNI) && cmd!=MT2_MCU_SNIFFER){p[36]=(uint8_t)cmd;put32(p+40,status);}
    if(s->wrong_sequence){p[29]=(uint8_t)(seq==15?1:seq+1);s->wrong_sequence=0;}
    if(s->unsolicited){p[30]=4;s->unsolicited=0;}
    if(s->wrong_eid){p[28]=0xef;s->wrong_eid=0;}
    if(s->short_reply){bytes=31;put32(p,0x38000000u|bytes);}
    d->control=0xc0000000u|(bytes<<16);s->rx_index=(index+1)%4;
}
static void consume(device_model *s,uint32_t bank,uint32_t cpu) {
    uint32_t reg=bank?0xd4400u:0xd4410u,index=s->bar[(reg+12)/4],length,cmd,status,i;
    uint8_t *p,seq;mt2_descriptor *d;
    if(s->hold_dma)return;
    while(index!=cpu) {
        d=&s->descriptors[bank][index];p=s->pools[bank]+index*8192;
        length=(d->control>>16)&0x3fff;
        CHECK(d->address==0x200000+bank*0x10000+index*8192);
        CHECK(!(d->control&MT2_DMA_DONE) && (d->control&0x40000000) && length && !d->address1);
        ++s->commands;
        if(bank) {
            CHECK(length<=4096 && length<=s->remaining);s->remaining-=length;s->scatter_bytes+=length;
            for(i=0;i<length;++i)s->scatter_hash=(s->scatter_hash^p[i])*1099511628211ULL;
        } else {
            CHECK(length>=48 && get32(p)==(0x41000000u|length) && get32(p+4)==0x80010000u);
            CHECK((uint32_t)(p[32]|p[33]<<8)==length-32 && p[37]==0xa0);
            cmd=p[36];seq=p[39];status=0;CHECK(seq && seq<=15);
            if((p[34]|p[35]<<8)!=0x8000){cmd=MT2_MCU_UNI|p[34];CHECK(length>=48);}
            else {
                CHECK(length>=64);
                switch(cmd) {
                case 4: CHECK(length==68 && get32(p+64)==1);s->bar[0xe00f0/4]=s->hold_rom?0:1;break;
                case 0x10:
                    CHECK(length==68);
                    if(get32(p+64))status=2;
                    else{status=3;++s->releases;}break;
                case 1:case 5:CHECK(length==76 && !s->remaining);s->remaining=get32(p+68);break;
                case 7:CHECK(length==68 && !s->remaining);break;
                case 2:CHECK(length==72 && !s->remaining);s->bar[0xe00f0/4]=s->hold_ready?1:3;break;
                case 0x8a:CHECK(length==64 && p[38]==1 && !p[41]);break;
                case 0xd1:
                    CHECK(length==196 && p[38]==1 && !p[41] && !p[43]);
                    CHECK(p[64]==1 && p[65]==1 && get32(p+68)==52 && get32(p+76)==1);
                    CHECK(p[72]=='U' && p[73]=='S' && p[144]=='U' && p[145]=='S');break;
                case 0xed:
                    CHECK(p[43]==1 && !p[42]);
                    cmd|=(uint32_t)p[41]<<8;
                    if(cmd==0x1ed) {
                        CHECK(p[38]==0 && length==88 && get32(p+64)==0x550);
                        for(i=68;i<88;++i)CHECK(!p[i]);cmd|=MT2_MCU_QUERY;
                    }
                    else if(cmd==MT2_MCU_EFUSE_MODE)CHECK(p[38]==1 && length==68 && get32(p+64)==0x100);
                    else if(cmd==MT2_MCU_PROTECT){CHECK(p[38]==1 && length==76);
                        CHECK(get32(p+64)==1 && get32(p+68)==0x92b && get32(p+72)==2);}
                    else if(cmd==MT2_MCU_MAC_ENABLE)CHECK(p[38]==1 && length==68 && get32(p+64)==1);
                    else CHECK(p[38]==1 && (cmd==MT2_MCU_RX_PATH || cmd==MT2_MCU_CHANNEL) && length==140);
                    break;
                case 0x5c:
                    CHECK(p[38]==1 && !p[41] && !p[43] && length>=140 && get32(p+64)==((length-64)<<16|1));
                    ++s->clc_packets;cmd=MT2_CLC_COMMAND;break;
                case 0x0f:CHECK(p[38]==1 && !p[41] && !p[43] && length>=84);cmd=MT2_MCU_DOMAIN;break;
                case 0x0a:CHECK(p[38]==1 && !p[41] && !p[43] && length==132);cmd=MT2_MCU_RX_FILTER;break;
                case 0x5d:
                    CHECK(p[38]==1 && !p[41] && !p[43] && length>=270 && get32(p+64)==0);
                    CHECK(p[68]>=1 && p[68]<=8 && length==108+162u*p[68] && p[69]>=1 && p[69]<=3);
                    CHECK(p[72]=='U' && p[73]=='S' && s->sku_packets==s->pse_reads && !s->sku_last);
                    ++s->sku_packets;s->sku_items[p[69]-1]+=p[68];if(p[70])++s->sku_last;cmd=MT2_MCU_SKU;break;
                case 0xc0:
                    CHECK(p[38]==0 && !p[41] && !p[43] && length==72 && get32(p+64)==0x820c8000u && !get32(p+68));
                    ++s->pse_reads;cmd=MT2_MCU_PSE_READ;break;
                default:CHECK(0);
                }
            }
            if(cmd==MT2_MCU_MAC_ENABLE || cmd==MT2_MCU_DOMAIN || cmd==MT2_MCU_RX_PATH ||
               cmd==MT2_MCU_CHANNEL || cmd==MT2_MCU_SNIFFER || cmd==MT2_MCU_RX_FILTER) {
                uint32_t n=s->radio_count++,h=cmd==MT2_MCU_SNIFFER?48u:64u;
                CHECK(n<32 && length-h<=348);s->radio_id[n]=cmd;s->radio_len[n]=length-h;
                memcpy(s->radio_payload[n],p+h,length-h);status=s->radio_ack_status;
            }
            if(cmd==MT2_MCU_DEVICE || cmd==MT2_MCU_BSS) {
                uint32_t n=s->context_count++,bytes=cmd==MT2_MCU_DEVICE?16u:36u;
                CHECK(n<4 && length==48+bytes && p[43]==7 && !p[38]);
                s->context_id[n]=cmd;memcpy(s->context_payload[n],p+48,bytes);
                if(s->context_fail_at==s->context_count)status=s->context_status;
            }
            if(cmd!=4 && cmd!=MT2_MCU_DOMAIN && cmd!=MT2_MCU_RX_FILTER && cmd!=MT2_MCU_SKU &&
               (!s->context_omit_at || s->context_count!=s->context_omit_at) &&
               !(cmd==MT2_MCU_PSE_READ && s->omit_pse_at==s->pse_reads) &&
               (!s->omit_radio_at || s->radio_count!=s->omit_radio_at) && !s->omit_reply && !(cmd==MT2_CLC_COMMAND &&
                (!p[71] || s->omit_clc_at==s->clc_packets))) {
                response(s,cmd,seq,status);
                if(s->duplicate_reply)response(s,cmd,seq,status);
            }
            if(s->hold_radio_at && s->radio_count==s->hold_radio_at){s->hold_dma=1;return;}
            if(cmd==MT2_MCU_SKU && s->hold_sku_at && s->sku_packets==s->hold_sku_at){s->hold_dma=1;return;}
            if(s->context_hold_at && s->context_count==s->context_hold_at){s->hold_dma=1;return;}
        }
        d->control|=MT2_DMA_DONE;index=(index+1)%4;s->bar[(reg+12)/4]=index;
    }
}
static int read_register(void *ctx,uint32_t offset,uint32_t *value) {
    device_model *s=(device_model *)ctx;uint32_t physical;
    CHECK(offset<0x100000 && !(offset&3u));++s->reads;
    if(++s->io_count==s->fail_io)return -89;
    if(offset==s->all_ones_read){*value=0xffffffffu;return 0;}
    if(s->running && s->reply_early && offset==0xd441c && s->mcu.command[0].queued) {
        s->reply_early=0;response(s,0x10,s->mcu.sequence,2);
    }
    if(s->running && s->early_mac_ack && offset==0xd441c && s->mcu.command[0].queued) {
        s->early_mac_ack=0;response(s,MT2_MCU_EFUSE_MODE,s->mcu.sequence,0);
    }
    if(s->running && s->early_regd && offset==0xd441c && s->mcu.command[0].queued) {
        s->early_regd=0;response(s,0xd1,s->mcu.sequence,0);
    }
    if(s->running && s->early_clc && offset==0xd441c && s->mcu.command[0].queued) {
        s->early_clc=0;response(s,MT2_CLC_COMMAND,s->mcu.sequence,0);
    }
    if(s->running && (offset==0xd441c || offset==0xd440c))
        consume(s,offset==0xd440c,s->bar[(offset-4)/4]);
    if(offset>=0x40000 && offset<0x50000) {
        physical=(s->bar[0xfe24c/4]&0xffffu)<<16|(offset-0x40000);
        if(physical==0x18000140){*value=s->reset;return 0;}
        if(physical==0x18060010){*value=s->top_own;return 0;}
    }
    *value=s->bar[offset/4];
    if(offset==0x34230 && (s->hold_wtbl || (s->wtbl_stall_after && s->wtbl_writes>=s->wtbl_stall_after)))
        *value|=0x80000000u;
    return 0;
}
static int write_register(void *ctx,uint32_t offset,uint32_t value) {
    device_model *s=(device_model *)ctx;uint32_t physical;
    CHECK(offset<0x100000 && !(offset&3u));++s->writes;CHECK(s->barriers>=s->writes);
    ++s->io_count;
    if(offset==s->ignore_write)return 0;
    s->bar[offset/4]=value;
    if(offset==0x34230) {
        CHECK((value&0x3ffu)==s->wtbl_writes && (value&0x1000u) && !(value&0x4000u));
        ++s->wtbl_writes;
    }
    if(s->volatile_counters && (offset==0x217c4 || offset==0x21780 || offset==0xa17c4 || offset==0xa1780))
        s->bar[offset/4]&=~0x40000000u;
    if(offset==0xd4208)s->bar[offset/4]&=~10u;
    if(offset==0xe0010)s->bar[offset/4]=value==1?4u:0u;
    if(offset>=0x40000 && offset<0x50000) {
        physical=(s->bar[0xfe24c/4]&0xffffu)<<16|(offset-0x40000);
        if(physical==0x18000140)s->reset=(value&1u)?value|0x10u:value&~0x10u;
        if(physical==0x18060010)s->top_own=value==2?0u:1u;
    }
    if(s->running && offset==0xd4508){++s->rx_credit;CHECK(s->rx_credit<=3);}
    if(s->running && (offset==0xd4418 || offset==0xd4408))consume(s,offset==0xd4408,value);
    if(s->io_count==s->fail_io)return -89;
    return offset==s->write_fail?-88:0; /* Deliberate post-write error. */
}
static uint64_t milliseconds(void *ctx){return ((device_model *)ctx)->time;}
static int delay(void *ctx,uint32_t ms){device_model *s=(device_model *)ctx;if(!s->freeze_time)s->time+=ms;return 0;}
static int other_record(void *ctx,const uint8_t *p,uint32_t bytes) {
    device_model *s=(device_model *)ctx;uint32_t reply=99;
    CHECK(bytes>=4 && p);++s->extra_records;
    if((get32(p)>>27)==2) {
        /* Test platform explicitly configures FCS absent. Frame-level parse
         * failures are drops, not fatal MCU transport errors. */
        (void)mt2_raw_rx_record(p,bytes,0,s->radio_record,sizeof(s->radio_record),
                               &s->radio_bytes,&s->radio_outcome);
    }
    if(s->reentrant) {
        CHECK(mt2_pcie_poll(&s->mcu)==MT2_INVALID);
        CHECK(s->transport.send(&s->mcu,0x10,(const uint8_t *)"\1\0\0\0",4,1,&reply)==MT2_INVALID);
        CHECK(reply==99);
    }
    return 0;
}
static void setup(int activate) {
    mt2_ring_binding bindings[MT2_RING_COUNT];uint32_t i;
    mt2_register_io io={&v,0x100000,read_register,write_register,barrier,milliseconds,delay};
    memset(&v,0,sizeof(v));v.reset=0x11;v.write_fail=0xffffffffu;v.all_ones_read=0xffffffffu;v.ignore_write=0xffffffffu;
    v.scatter_hash=14695981039346656037ULL;
    for(i=0;i<MT2_MCU_BANKS;++i) {
        mt2_dma_memory *b=&v.banks[i].memory;
        b->descriptors=v.descriptors[i];b->descriptor_address=0x100000+i*0x1000;
        b->pool=v.pools[i];b->pool_address=0x200000+i*0x10000;
        b->count=4;b->slot_bytes=8192;b->pool_bytes=4*8192;
        if(i>=2){v.banks[i].assembly=v.assemblies[i-2];v.banks[i].capacity=512;}
        bindings[i+1].address=b->descriptor_address;bindings[i+1].count=4;
    }
    bindings[0].address=0x300000;bindings[0].count=4;
    if(!activate)return;
    CHECK(mt2_pcie_prepare(&v.mcu,&v.chip,v.banks)==0 && !v.reads && !v.writes);
    CHECK(mt2_pcie_activate(&v.mcu,&v.transport)==MT2_INVALID);
    CHECK(mt2_chip_start(&v.chip,&io,bindings)==0);
    CHECK(mt2_pcie_activate(&v.mcu,&v.transport)==0);
    v.mcu.other_record=other_record;v.mcu.record_context=&v;v.running=1;v.rx_credit=3;
}
static void stop(void) {
    mt2_pcie_disarm(&v.mcu);v.running=0;
    CHECK(mt2_chip_stop(&v.chip)==0 && v.chip.quiescent);
    CHECK(mt2_pcie_activate(&v.mcu,&v.transport)==MT2_INVALID);
}
static int semaphore(uint32_t *reply) {
    static const uint8_t request[4]={1,0,0,0};
    return v.transport.send(v.transport.context,0x10,request,4,1,reply);
}
static void images(uint8_t *patch,uint8_t *ram) {
    memset(patch,0,164);be32(patch+44,1);be32(patch+96,2);be32(patch+100,160);
    be32(patch+104,4);be32(patch+108,0x900000);be32(patch+112,4);be32(patch+116,0xffffffffu);
    memset(ram,0,80);put32(ram+20,0x91b000);put32(ram+24,4);ram[28]=0x21;ram[46]=1;
}
static const uint32_t mac_regs[14]={0xf004,0xf000,0x210f4,0x217c4,0x21780,0x24804,0x21e00,0x23408,
                                  0xa10f4,0xa17c4,0xa1780,0xa4804,0xa1e00,0xa3408};
static void mac_setup(void) {
    uint32_t i;setup(1);v.bar[0xe00f0/4]=3;
    CHECK(v.transport.wait_state(&v.mcu,1)==0);
    for(i=0;i<14;++i)v.bar[mac_regs[i]/4]=0xa5a5a5a5;
    v.io_count=0;
}
static void test_mac(void) {
    mt2_mac_state s;uint32_t i,j,total,before;uint64_t start;
    static const uint32_t mask[14]={0xfff8,0x88000,0x6003f,0x40000000,0x40000000,0x300,0x80fff8,0xc3000000,
                                   0x6003f,0x40000000,0x40000000,0x300,0x80fff8,0xc3000000};
    static const uint32_t set[14]={0x3000,0x8000,0x6003f,0x40000000,0x40000000,0x300,0x3000,0x03000000,
                                  0x6003f,0x40000000,0x40000000,0x300,0x3000,0x03000000};
    setup(1);memset(&s,0,sizeof(s));before=v.io_count;
    CHECK(mt2_mac_initialize(&v.mcu,&s)==MT2_INVALID && !s.attempted && v.io_count==before);
    CHECK(mt2_pcie_mac_command(&v.mcu,MT2_MCU_EFUSE_MODE)==MT2_INVALID && v.io_count==before);stop();
    mac_setup();memset(&s,0,sizeof(s));
    CHECK(mt2_pcie_mac_command(&v.mcu,0x22ed)==MT2_INVALID && !v.io_count);
    v.mcu.busy=1;CHECK(mt2_mac_initialize(&v.mcu,&s)==MT2_INVALID && !s.attempted && !v.io_count);v.mcu.busy=0;
    CHECK(mt2_mac_initialize(&v.mcu,&s)==0 && s.initialized && s.step==6 && !s.error);
    CHECK(v.commands==2 && v.mcu.matched_responses==2 && v.wtbl_writes==20);
    CHECK(!v.mcu.command[0].queued && !v.mcu.command[1].queued && !v.bar[0xd4308/4]);
    for(i=0;i<14;++i)CHECK(v.bar[mac_regs[i]/4]==((0xa5a5a5a5u&~mask[i])|set[i]));
    CHECK(v.bar[0x34230/4]==0x1013 && !v.bar[0x34380/4] && !v.bar[0xe800/4]);
    total=v.io_count;
    CHECK(mt2_mac_initialize(&v.mcu,&s)==MT2_INVALID && v.io_count==total);stop();
    /* Every register I/O in the successful sequence is independently faulted.
     * Writes fail AFTER publication; no cleanup is allowed to free DMA memory. */
    for(i=1;i<=total;++i) {
        mac_setup();memset(&s,0,sizeof(s));v.fail_io=i;
        CHECK(mt2_mac_initialize(&v.mcu,&s)==-89 && s.error==-89 && !s.initialized);
        CHECK(v.mcu.fault==-89 && !v.mcu.active && v.chip.published && !v.chip.quiescent);
        before=v.io_count;CHECK(mt2_mac_initialize(&v.mcu,&s)==MT2_INVALID && v.io_count==before);
        v.fail_io=0;stop();
    }
    /* Stable settings must read back. Mutable counter-enable bits need only
     * successful posted-write flush; hardware may clear them asynchronously. */
    for(j=0;j<14;++j) {
        if(j==3 || j==4 || j==9 || j==10)continue;
        mac_setup();memset(&s,0,sizeof(s));v.ignore_write=mac_regs[j];
        v.bar[mac_regs[j]/4]=(0xa5a5a5a5u&~mask[j])|(~set[j]&mask[j]);
        CHECK(mt2_mac_initialize(&v.mcu,&s)==MT2_PROTOCOL && !s.initialized && !v.mcu.active);
        v.ignore_write=0xffffffffu;stop();
    }
    mac_setup();memset(&s,0,sizeof(s));v.volatile_counters=1;
    CHECK(mt2_mac_initialize(&v.mcu,&s)==0 && s.initialized);stop();
    mac_setup();memset(&s,0,sizeof(s));v.hold_wtbl=1;start=v.time;
    CHECK(mt2_mac_initialize(&v.mcu,&s)==MT2_FW_TIMEOUT && s.step==3 && !v.wtbl_writes);
    CHECK(v.time-start==5 && !v.mcu.active);stop();
    mac_setup();memset(&s,0,sizeof(s));v.wtbl_stall_after=1;v.freeze_time=1;
    CHECK(mt2_mac_initialize(&v.mcu,&s)==MT2_FW_TIMEOUT && v.wtbl_writes==1 && !s.initialized);stop();
    mac_setup();memset(&s,0,sizeof(s));v.all_ones_read=0xf004;
    CHECK(mt2_mac_initialize(&v.mcu,&s)==MT2_PROTOCOL && s.step==2 && !v.wtbl_writes);
    v.all_ones_read=0xffffffffu;stop();
    /* An ACK without command-DMA completion is insufficient even at stage 1. */
    mac_setup();memset(&s,0,sizeof(s));v.hold_dma=1;v.early_mac_ack=1;
    CHECK(mt2_mac_initialize(&v.mcu,&s)==MT2_FW_TIMEOUT && s.step==1 && !s.initialized);
    CHECK(v.mcu.command[0].queued==1 && !v.wtbl_writes);v.hold_dma=0;stop();
    mac_setup();memset(&s,0,sizeof(s));v.omit_reply=1;
    CHECK(mt2_mac_initialize(&v.mcu,&s)==MT2_FW_TIMEOUT && s.step==1 && !s.initialized);stop();
    printf("{\"mac_initialization_tests\":\"passed\",\"injected_register_failures\":%u,\"device_access\":false}\n",total);
}
static void test_regd_transport(void) {
    uint8_t ram[244],scratch[MT2_REGD_REQUEST_MAX];mt2_regdomain d,before;mt2_nic_caps caps;uint32_t prior;
    test_regd_image(ram);memset(&before,0xa5,sizeof(before));d=before;
    mac_setup();prior=v.io_count;
    CHECK(mt2_pcie_query_regdomain(&v.mcu,ram,244,(const uint8_t *)"US",scratch,sizeof(scratch),&d)==MT2_INVALID);
    CHECK(v.io_count==prior && !memcmp(&d,&before,sizeof(d)));
    CHECK(mt2_pcie_query_nic(&v.mcu,&caps)==0 && v.mcu.nic_valid && (v.mcu.nic_chip_caps&0x20));
    CHECK(mt2_pcie_query_regdomain(&v.mcu,ram,244,(const uint8_t *)"US",scratch,sizeof(scratch),&d)==0);
    CHECK(d.count==1 && d.rules[0].start_mhz==2402 && !v.mcu.regd_target && !v.mcu.command[0].queued);
    prior=v.io_count;v.mcu.nic_chip_caps&=~0x20u;d=before;
    CHECK(mt2_pcie_query_regdomain(&v.mcu,ram,244,(const uint8_t *)"US",scratch,sizeof(scratch),&d)==MT2_INVALID);
    CHECK(v.io_count==prior && !memcmp(&d,&before,sizeof(d)));stop();
    mac_setup();CHECK(mt2_pcie_query_nic(&v.mcu,&caps)==0);prior=v.io_count;ram[4]=4;
    CHECK(mt2_pcie_query_regdomain(&v.mcu,ram,244,(const uint8_t *)"US",scratch,sizeof(scratch),&d)==MT2_INVALID);
    CHECK(v.io_count==prior && !v.mcu.fault && !memcmp(&d,&before,sizeof(d)));test_regd_image(ram);stop();
    mac_setup();CHECK(mt2_pcie_query_nic(&v.mcu,&caps)==0);v.wrong_sequence=1;
    CHECK(mt2_pcie_query_regdomain(&v.mcu,ram,244,(const uint8_t *)"US",scratch,sizeof(scratch),&d)==MT2_FW_TIMEOUT);
    CHECK(!v.mcu.regd_target && !v.mcu.nic_valid && !memcmp(&d,&before,sizeof(d)));stop();
    mac_setup();CHECK(mt2_pcie_query_nic(&v.mcu,&caps)==0);v.short_reply=1;
    CHECK(mt2_pcie_query_regdomain(&v.mcu,ram,244,(const uint8_t *)"US",scratch,sizeof(scratch),&d)==MT2_PROTOCOL);
    CHECK(!v.mcu.regd_target && !memcmp(&d,&before,sizeof(d)));stop();
    mac_setup();CHECK(mt2_pcie_query_nic(&v.mcu,&caps)==0);v.hold_dma=1;v.early_regd=1;
    CHECK(mt2_pcie_query_regdomain(&v.mcu,ram,244,(const uint8_t *)"US",scratch,sizeof(scratch),&d)==MT2_FW_TIMEOUT);
    CHECK(!v.mcu.regd_target && !memcmp(&d,&before,sizeof(d)) && v.mcu.command[0].queued==1);v.hold_dma=0;stop();
    puts("{\"regulatory_transport_tests\":\"passed\",\"transport\":\"register_and_dma_simulation\",\"device_access\":false}");
}
static void power_setup(int events) {
    mt2_nic_caps caps;mt2_hw_type hw;mac_setup();v.no_clc_events=!events;
    CHECK(mt2_pcie_query_nic(&v.mcu,&caps)==0);
    CHECK(mt2_pcie_query_hw_type(&v.mcu,&hw)==0 && hw.encapsulation==1 && v.mcu.hw_type_valid);
}
static void test_clc_transport(void) {
    uint8_t ram[240],scratch[1024];mt2_clc_policy p={{'U','S'},0,1,0,255,0,255};
    mt2_clc_result result;mt2_nic_caps caps;mt2_hw_type hw,before;uint32_t count;
    test_clc_image(ram);memset(&before,0xa5,sizeof(before));hw=before;
    mac_setup();count=v.io_count;CHECK(mt2_pcie_query_hw_type(&v.mcu,&hw)==MT2_INVALID && v.io_count==count);stop();
    mac_setup();CHECK(mt2_pcie_query_nic(&v.mcu,&caps)==0);v.hw_invalid=1;
    CHECK(mt2_pcie_query_hw_type(&v.mcu,&hw)==MT2_PROTOCOL && !memcmp(&hw,&before,sizeof(hw)) && !v.mcu.hw_type_valid);stop();
    mac_setup();CHECK(mt2_pcie_query_nic(&v.mcu,&caps)==0);v.hw_wrong_address=1;
    CHECK(mt2_pcie_query_hw_type(&v.mcu,&hw)==MT2_FW_TIMEOUT && !memcmp(&hw,&before,sizeof(hw)) && !v.mcu.hw_target);stop();
    power_setup(1);p.acpi_known=0;count=v.io_count;
    CHECK(mt2_pcie_apply_clc(&v.mcu,ram,240,&p,scratch,sizeof(scratch),&result)==MT2_INVALID && !result.attempted && v.io_count==count);
    p.acpi_known=1;
    CHECK(mt2_pcie_apply_clc(&v.mcu,ram,240,&p,scratch,sizeof(scratch),&result)==0);
    CHECK(result.completed && result.requests_drained==2 && result.requests_replied==2 && result.channel_known && result.channel_config==31);
    CHECK(v.mcu.clc_completed && v.mcu.clc_channel_known && !v.mcu.clc_target && !v.mcu.command[0].queued);
    count=v.io_count;CHECK(mt2_pcie_apply_clc(&v.mcu,ram,240,&p,scratch,sizeof(scratch),&result)==MT2_INVALID && v.io_count==count);stop();
    power_setup(0);CHECK(!(v.mcu.nic_chip_caps&1u));
    CHECK(mt2_pcie_apply_clc(&v.mcu,ram,240,&p,scratch,sizeof(scratch),&result)==0);
    CHECK(result.completed && result.requests_drained==2 && !result.requests_replied && !result.channel_known && !v.mcu.clc_channel_known);stop();
    power_setup(1);v.omit_clc_at=2;
    CHECK(mt2_pcie_apply_clc(&v.mcu,ram,240,&p,scratch,sizeof(scratch),&result)==MT2_FW_TIMEOUT);
    CHECK(result.attempted && !result.completed && result.requests_drained==1 && !result.channel_known);
    CHECK(!v.mcu.active && v.mcu.clc_attempted && !v.mcu.clc_completed && v.chip.published && !v.chip.quiescent);stop();
    power_setup(1);ram[86]=4;count=v.io_count;
    CHECK(mt2_pcie_apply_clc(&v.mcu,ram,240,&p,scratch,sizeof(scratch),&result)==MT2_INVALID && !result.attempted && v.io_count==count);
    test_clc_image(ram);stop();
    power_setup(1);v.hold_dma=1;v.early_clc=1;
    CHECK(mt2_pcie_apply_clc(&v.mcu,ram,240,&p,scratch,sizeof(scratch),&result)==MT2_FW_TIMEOUT);
    CHECK(result.requests_replied==1 && !result.requests_drained && !result.completed && !result.channel_known && v.mcu.command[0].queued==1);
    v.hold_dma=0;stop();
    puts("{\"clc_transport_tests\":\"passed\",\"device_access\":false}");
}
typedef struct channel_fixture {
    mt2_channel_control control;mt2_mac_state mac;mt2_tx_queue tx;
    mt2_regdomain domain;mt2_nic_caps caps;mt2_clc_policy policy;
} channel_fixture;
static channel_fixture cf;
static void channel_reprepare(void) {
    memset(&cf.control,0,sizeof(cf.control));
    CHECK(mt2_channel_prepare(&cf.control,&v.mcu,&cf.mac,&cf.tx,&cf.domain,&cf.caps,&cf.policy)==0);
}
static void channel_setup_environment(uint32_t flags,uint32_t environment) {
    uint8_t ram[240],scratch[1024];mt2_clc_result result;
    power_setup(1);memset(&cf,0,sizeof(cf));cf.policy.alpha2[0]='U';cf.policy.alpha2[1]='S';
    cf.policy.acpi_known=1;cf.policy.mtcl_config=255;cf.policy.world_mtcl_config=255;
    cf.policy.environment=environment;
    CHECK(mt2_pcie_query_nic(&v.mcu,&cf.caps)==0 && mt2_mac_initialize(&v.mcu,&cf.mac)==0);
    test_clc_image(ram);CHECK(mt2_pcie_apply_clc(&v.mcu,ram,240,&cf.policy,scratch,sizeof(scratch),&result)==0);
    cf.domain.count=3;cf.domain.dfs_region=1;
    cf.domain.rules[0].start_mhz=2402;cf.domain.rules[0].end_mhz=2482;cf.domain.rules[0].bandwidth_mhz=40;
    cf.domain.rules[1].start_mhz=5170;cf.domain.rules[1].end_mhz=5330;cf.domain.rules[1].bandwidth_mhz=80;
    cf.domain.rules[2].start_mhz=5735;cf.domain.rules[2].end_mhz=5895;cf.domain.rules[2].bandwidth_mhz=80;cf.domain.rules[2].flags=flags;
    cf.tx.prepared=1; /* Synthetic idle raw-TX owner; MCU registers/DMA are modeled. */
    channel_reprepare();
}
static void channel_setup(uint32_t flags){channel_setup_environment(flags,0);}
static void test_channel_transport(void) {
    uint8_t expected[76]={0},request[348];uint32_t before,i;
    static const uint32_t denies[]={1,4,8,16,32,64,128,256,0x100000,0x4000000};
    channel_setup(0);before=v.io_count;memset(request,0xa5,sizeof(request));
    CHECK(mt2_channel_domain_request(&cf.control,request,20)==MT2_INVALID && request[0]==0xa5 && v.io_count==before);
    CHECK(mt2_channel_apply(&cf.control)==0 && cf.control.configured && !mt2_channel_permits_tx(&cf.control));
    CHECK(v.radio_count==2 && v.radio_id[0]==MT2_MCU_MAC_ENABLE && v.radio_id[1]==MT2_MCU_DOMAIN);
    CHECK(v.radio_len[1]==244 && !memcmp(v.radio_payload[1],"US\0\0\0\2\3\0\15\20\0\0",12));
    before=v.io_count;CHECK(mt2_channel_apply(&cf.control)==MT2_INVALID && before==v.io_count);
    CHECK(mt2_channel_monitor(&cf.control,149,155,80)==0 && cf.control.monitor && mt2_channel_permits_tx(&cf.control));
    CHECK(v.radio_count==7 && v.radio_id[2]==MT2_MCU_RX_PATH && v.radio_id[3]==MT2_MCU_CHANNEL && v.radio_id[6]==MT2_MCU_RX_FILTER);
    expected[0]=149;expected[1]=155;expected[2]=2;expected[3]=2;expected[4]=3;expected[10]=1;
    CHECK(v.radio_len[2]==76 && !memcmp(v.radio_payload[2],expected,76));
    expected[4]=2;CHECK(!memcmp(v.radio_payload[3],expected,76));
    memset(expected,0,sizeof(expected));expected[6]=8;expected[8]=1;
    CHECK(v.radio_len[4]==12 && !memcmp(v.radio_payload[4],expected,12));
    memset(expected,0,sizeof(expected));expected[4]=1;expected[6]=16;expected[10]=2;expected[11]=1;
    expected[12]=149;expected[13]=1;expected[14]=155;expected[16]=1;
    CHECK(v.radio_len[5]==20 && !memcmp(v.radio_payload[5],expected,20));
    memset(expected,0,sizeof(expected));expected[4]=1;put32(expected+8,0x80000060u);
    CHECK(v.radio_len[6]==68 && !memcmp(v.radio_payload[6],expected,68));
    before=v.io_count;cf.tx.inflight=1;
    CHECK(mt2_channel_monitor(&cf.control,6,6,20)==MT2_INVALID && before==v.io_count && cf.control.primary==149);
    cf.tx.inflight=0;CHECK(mt2_channel_monitor(&cf.control,6,6,20)==0 && cf.control.primary==6 && v.radio_count==9);
    CHECK(v.radio_id[7]==MT2_MCU_CHANNEL && v.radio_id[8]==MT2_MCU_SNIFFER);
    before=v.io_count;CHECK(mt2_channel_monitor(&cf.control,149,42,80)==MT2_INVALID && before==v.io_count && cf.control.primary==6);
    CHECK(mt2_channel_monitor_stop(&cf.control)==0 && !cf.control.monitor && !cf.control.primary && !mt2_channel_permits_tx(&cf.control));stop();
    for(i=0;i<sizeof(denies)/sizeof(denies[0]);++i) {
        channel_setup(denies[i]);CHECK(mt2_channel_apply(&cf.control)==0 && mt2_channel_monitor(&cf.control,149,149,20)==0);
        CHECK(!mt2_channel_permits_tx(&cf.control));stop();
    }
    channel_setup(0);cf.control.clc_known=0;
    CHECK(mt2_channel_apply(&cf.control)==0);before=v.io_count;
    CHECK(mt2_channel_monitor(&cf.control,169,169,20)==MT2_INVALID && before==v.io_count);
    CHECK(mt2_channel_monitor(&cf.control,149,149,20)==0);stop();
    channel_setup(0x8000);CHECK(mt2_channel_apply(&cf.control)==0);before=v.io_count;
    CHECK(mt2_channel_monitor(&cf.control,149,155,80)==MT2_INVALID && before==v.io_count);stop();
    channel_setup(0);cf.domain.rules[2].end_mhz=5770;cf.domain.count=4;cf.domain.rules[3]=cf.domain.rules[2];
    cf.domain.rules[3].start_mhz=5780;cf.domain.rules[3].end_mhz=5895;
    channel_reprepare();
    CHECK(mt2_channel_apply(&cf.control)==0);before=v.io_count;
    CHECK(mt2_channel_monitor(&cf.control,149,155,80)==MT2_INVALID && before==v.io_count);stop();
    channel_setup(0x800);cf.domain.rules[2].end_mhz=5775;cf.domain.rules[2].bandwidth_mhz=40;
    cf.domain.count=4;cf.domain.rules[3]=cf.domain.rules[2];cf.domain.rules[3].start_mhz=5775;cf.domain.rules[3].end_mhz=5815;
    channel_reprepare();CHECK(mt2_channel_apply(&cf.control)==0 && mt2_channel_monitor(&cf.control,149,155,80)==0 && mt2_channel_permits_tx(&cf.control));stop();
    channel_setup(0);cf.domain.count=4;cf.domain.rules[3]=cf.domain.rules[2];cf.domain.rules[3].start_mhz=5780;
    cf.domain.rules[3].end_mhz=5790;cf.domain.rules[3].bandwidth_mhz=20;channel_reprepare();
    CHECK(mt2_channel_apply(&cf.control)==0);before=v.io_count;
    CHECK(mt2_channel_monitor(&cf.control,149,155,80)==MT2_INVALID && before==v.io_count);stop();
    channel_setup_environment(8,1);CHECK(mt2_channel_apply(&cf.control)==0 && mt2_channel_monitor(&cf.control,149,151,40)==0 && mt2_channel_permits_tx(&cf.control));
    CHECK(mt2_channel_monitor(&cf.control,153,151,40)==0 && v.radio_payload[v.radio_count-1][13]==3);stop();
    channel_setup_environment(4,2);CHECK(mt2_channel_apply(&cf.control)==0 && mt2_channel_monitor(&cf.control,149,149,20)==0 && mt2_channel_permits_tx(&cf.control));stop();
    /* Fault at a solicited sniffer reply after earlier setters completed. */
    channel_setup(0);v.omit_radio_at=5;CHECK(mt2_channel_apply(&cf.control)==0);
    CHECK(mt2_channel_monitor(&cf.control,149,149,20)==MT2_FW_TIMEOUT && cf.control.fault && !cf.control.monitor && !cf.control.configured);
    CHECK(!v.mcu.active && cf.tx.used && !cf.tx.active && v.chip.published && !v.chip.quiescent);stop();
    /* The final no-ACK RX-filter command must also complete DMA. */
    channel_setup(0);v.hold_radio_at=7;CHECK(mt2_channel_apply(&cf.control)==0);
    CHECK(mt2_channel_monitor(&cf.control,149,149,20)==MT2_FW_TIMEOUT && !cf.control.monitor && v.mcu.command[0].queued==1);
    v.hold_dma=0;stop();
    channel_setup(0);CHECK(mt2_channel_apply(&cf.control)==0);v.short_reply=1;
    CHECK(mt2_channel_monitor(&cf.control,149,149,20)==MT2_PROTOCOL && cf.control.fault && !cf.control.monitor);stop();
    puts("{\"channel_control_tests\":\"passed\",\"transport\":\"register_and_dma_simulation\",\"device_access\":false}");
}
void test_power_policy(mt2_platform_policy *);
static void test_monitor_context(void) {
    static const uint8_t mac[6]={2,0x13,0x24,0x35,0x46,0x57};
    static const uint8_t device[16]={0,0,0,0,0,0,12,0,1,0,2,0x13,0x24,0x35,0x46,0x57};
    static const uint8_t bss[36]={0,0,0,0,0,0,32,0,1,0,0,0,2,0,1,0,1};
    uint8_t output[36],original[36],invalid[6]={0};uint32_t i,j,cmd,n,before;
    for(i=0;i<2;++i) {
        cmd=i?MT2_MCU_BSS:MT2_MCU_DEVICE;n=i?36:16;
        memset(output,0xa5,sizeof(output));memcpy(original,output,sizeof(output));
        for(j=0;j<n;++j)CHECK(mt2_monitor_context_request(cmd,1,mac,output,j)==MT2_INVALID && !memcmp(output,original,36));
        CHECK(mt2_monitor_context_request(cmd,1,mac,output,36)==(int)n && !memcmp(output,i?bss:device,n));
        CHECK(mt2_monitor_context_request(cmd,0,mac,output,36)==(int)n && !output[8]);output[8]=1;
        if(i){CHECK(output[24]==19 && output[30]==19);output[24]=0;output[30]=0;}
        CHECK(!memcmp(output,i?bss:device,n));
    }
    memcpy(original,output,36);
    CHECK(mt2_monitor_context_request(MT2_MCU_BSS,2,mac,output,36)==MT2_INVALID && !memcmp(output,original,36));
    CHECK(mt2_monitor_context_request(MT2_MCU_SNIFFER,1,mac,output,36)==MT2_INVALID && !memcmp(output,original,36));
    CHECK(mt2_monitor_context_request(MT2_MCU_BSS,1,invalid,output,36)==MT2_INVALID && !memcmp(output,original,36));
    invalid[0]=1;CHECK(mt2_monitor_context_request(MT2_MCU_DEVICE,1,invalid,output,36)==MT2_INVALID && !memcmp(output,original,36));
    channel_setup(0);before=v.io_count;
    CHECK(mt2_monitor_context_start(&cf.control,mac)==MT2_INVALID && before==v.io_count && !cf.control.context_attempted);
    CHECK(mt2_channel_apply(&cf.control)==0);before=v.io_count;
    CHECK(mt2_monitor_context_start(&cf.control,invalid)==MT2_INVALID && before==v.io_count && !cf.control.context_attempted);
    cf.tx.active=1;CHECK(mt2_monitor_context_start(&cf.control,mac)==MT2_INVALID && before==v.io_count);cf.tx.active=0;
    cf.tx.queued=1;CHECK(mt2_monitor_context_start(&cf.control,mac)==MT2_INVALID && before==v.io_count);cf.tx.queued=0;
    cf.tx.inflight=1;CHECK(mt2_monitor_context_start(&cf.control,mac)==MT2_INVALID && before==v.io_count);cf.tx.inflight=0;
    CHECK(mt2_monitor_context_start(&cf.control,mac)==0 && cf.control.context_configured && cf.control.context_attempted);
    CHECK(v.context_count==2 && v.context_id[0]==MT2_MCU_DEVICE && v.context_id[1]==MT2_MCU_BSS);
    CHECK(!memcmp(v.context_payload[0],device,16) && !memcmp(v.context_payload[1],bss,36));
    CHECK(!v.mcu.command[0].queued && !cf.tx.active && !cf.control.power_configured);
    before=v.io_count;CHECK(mt2_monitor_context_start(&cf.control,mac)==MT2_INVALID && before==v.io_count);
    CHECK(mt2_channel_monitor(&cf.control,149,149,20)==0);before=v.io_count;
    CHECK(mt2_monitor_context_stop(&cf.control)==MT2_INVALID && before==v.io_count && cf.control.context_configured);
    CHECK(mt2_channel_monitor_stop(&cf.control)==0 && mt2_monitor_context_stop(&cf.control)==0);
    CHECK(!cf.control.context_configured && cf.control.context_attempted && v.context_count==4);
    CHECK(v.context_id[2]==MT2_MCU_BSS && v.context_id[3]==MT2_MCU_DEVICE && !v.context_payload[2][8] && !v.context_payload[3][8]);
    v.context_payload[2][8]=1;v.context_payload[3][8]=1;
    CHECK(v.context_payload[2][24]==19 && v.context_payload[2][30]==19);
    v.context_payload[2][24]=0;v.context_payload[2][30]=0;
    CHECK(!memcmp(v.context_payload[2],bss,36) && !memcmp(v.context_payload[3],device,16));
    before=v.io_count;CHECK(mt2_monitor_context_start(&cf.control,mac)==MT2_INVALID && before==v.io_count);
    CHECK(mt2_monitor_context_stop(&cf.control)==MT2_INVALID && before==v.io_count);stop();
    /* Every stage can return nonzero UNI status. No next command or rollback. */
    for(i=1;i<=4;++i) {
        channel_setup(0);CHECK(mt2_channel_apply(&cf.control)==0);
        v.context_fail_at=i;v.context_status=i&1?7u:0x80000000u;
        if(i>2)CHECK(mt2_monitor_context_start(&cf.control,mac)==0);
        CHECK((i>2?mt2_monitor_context_stop(&cf.control):mt2_monitor_context_start(&cf.control,mac))==MT2_PROTOCOL);
        CHECK(v.context_count==i && cf.control.context_attempted && !cf.control.context_configured && cf.control.fault);
        CHECK(!cf.control.configured && !v.mcu.active && !cf.tx.active && cf.tx.used && !v.chip.quiescent);
        before=v.io_count;CHECK(mt2_monitor_context_start(&cf.control,mac)==MT2_INVALID && v.io_count==before);stop();
    }
    for(i=1;i<=2;++i) {
        channel_setup(0);CHECK(mt2_channel_apply(&cf.control)==0);v.context_omit_at=i;
        CHECK(mt2_monitor_context_start(&cf.control,mac)==MT2_FW_TIMEOUT && v.context_count==i && !cf.control.context_configured);stop();
        channel_setup(0);CHECK(mt2_channel_apply(&cf.control)==0);v.context_hold_at=i;
        CHECK(mt2_monitor_context_start(&cf.control,mac)==MT2_FW_TIMEOUT && v.context_count==i && !cf.control.context_configured);
        CHECK(v.mcu.command[0].queued==1 && !v.chip.quiescent);v.hold_dma=0;stop();
    }
    channel_setup(0);CHECK(mt2_channel_apply(&cf.control)==0);v.short_reply=1;
    CHECK(mt2_monitor_context_start(&cf.control,mac)==MT2_PROTOCOL && v.context_count==1 && !cf.control.context_configured);stop();
    channel_setup(0);CHECK(mt2_channel_apply(&cf.control)==0);v.wrong_sequence=1;
    CHECK(mt2_monitor_context_start(&cf.control,mac)==MT2_FW_TIMEOUT && !cf.control.context_configured);stop();
    channel_setup(0);CHECK(mt2_channel_apply(&cf.control)==0 && mt2_monitor_context_start(&cf.control,mac)==0);
    v.omit_radio_at=5;
    CHECK(mt2_channel_monitor(&cf.control,149,149,20)==MT2_FW_TIMEOUT && !cf.control.context_configured);stop();
    puts("{\"monitor_context_tests\":\"passed\",\"status_failure_stages\":4,\"device_access\":false}");
}
static void test_power_transport(void) {
    uint8_t scratch[MT2_SKU_BATCH_BYTES];mt2_platform_policy policy;mt2_tx_power_plan plan;mt2_tx_power_result result;
    uint32_t before,value;
    test_power_policy(&policy);CHECK(mt2_tx_power_prepare(&policy,7,20,&plan)==0);
    channel_setup(0);before=v.io_count;
    CHECK(mt2_tx_power_apply(&cf.control,&plan,scratch,sizeof(scratch),&result)==MT2_INVALID && !result.attempted && v.io_count==before);
    CHECK(mt2_channel_apply(&cf.control)==0 && mt2_channel_monitor(&cf.control,149,149,20)==0);
    before=v.io_count;CHECK(mt2_tx_power_apply(&cf.control,&plan,scratch,1339,&result)==MT2_INVALID && !result.attempted && v.io_count==before);
    cf.tx.active=1;CHECK(mt2_tx_power_apply(&cf.control,&plan,scratch,sizeof(scratch),&result)==MT2_INVALID && v.io_count==before);cf.tx.active=0;
    CHECK(mt2_tx_power_apply(&cf.control,&plan,scratch,sizeof(scratch),&result)==0);
    CHECK(result.completed && result.batches_drained==22 && result.pse_reads==22 && result.last_pse_value==0xf1234567u);
    CHECK(cf.control.power_configured && v.sku_packets==22 && v.pse_reads==22 && v.sku_items[0]==14 && v.sku_items[1]==48 && v.sku_items[2]==109 && v.sku_last==1);
    CHECK(!v.mcu.pse_target && !v.mcu.command[0].queued);
    before=v.io_count;CHECK(mt2_tx_power_apply(&cf.control,&plan,scratch,sizeof(scratch),&result)==MT2_INVALID && v.io_count==before);stop();
    channel_setup(0);CHECK(mt2_channel_apply(&cf.control)==0 && mt2_channel_monitor(&cf.control,149,149,20)==0);
    v.omit_pse_at=2;CHECK(mt2_tx_power_apply(&cf.control,&plan,scratch,sizeof(scratch),&result)==MT2_FW_TIMEOUT);
    CHECK(result.attempted && !result.completed && result.batches_drained==2 && result.pse_reads==1 && v.sku_packets==2);
    CHECK(cf.control.fault && !cf.control.power_configured && !cf.control.monitor && !v.mcu.active && cf.tx.used);stop();
    channel_setup(0);CHECK(mt2_channel_apply(&cf.control)==0 && mt2_channel_monitor(&cf.control,149,149,20)==0);
    v.hold_sku_at=2;CHECK(mt2_tx_power_apply(&cf.control,&plan,scratch,sizeof(scratch),&result)==MT2_FW_TIMEOUT);
    CHECK(!result.completed && result.batches_drained==1 && result.pse_reads==1 && v.mcu.command[0].queued==1 && !v.chip.quiescent);
    v.hold_dma=0;stop();
    channel_setup(0);value=0x1234;v.pse_wrong_address=1;
    CHECK(mt2_pcie_query_pse(&v.mcu,&value)==MT2_FW_TIMEOUT && value==0x1234 && !v.mcu.pse_target);stop();
    channel_setup(0);value=0x1234;v.short_reply=1;
    CHECK(mt2_pcie_query_pse(&v.mcu,&value)==MT2_PROTOCOL && value==0x1234 && !v.mcu.pse_target);stop();
    puts("{\"tx_power_transport_tests\":\"passed\",\"batches\":22,\"pse_reads\":22,\"device_access\":false}");
}
typedef struct raw_fixture {
    mt2_descriptor descriptors[4];uint8_t pool[4*MT2_SLOT_SIZE];
    uint32_t states[4],slots[4];
} raw_fixture;
static raw_fixture rf;
static const uint8_t raw_mac[6]={2,0x13,0x24,0x35,0x46,0x57};
static int raw_publish(void *ctx,uint32_t index) {
    uint32_t slot=(index+3)%4,id=cf.tx.slots[slot];(void)ctx;
    CHECK(index==cf.tx.head && id<4 && rf.states[id]==1 && rf.descriptors[slot].control==0x40400000);
    CHECK(rf.descriptors[slot].address==0x310000+id*MT2_SLOT_SIZE);
    return mt2_reg_write(&v.chip.io,0xd4308,index);
}
static void raw_prepare(void) {
    mt2_tx_io io={&v,barrier,raw_publish};memset(&rf,0,sizeof(rf));memset(&cf.tx,0,sizeof(cf.tx));
    CHECK(mt2_tx_prepare(&cf.tx,rf.descriptors,0x300000,4,rf.pool,0x310000,sizeof(rf.pool),rf.states,4,rf.slots,&io)==0);
}
static void raw_setup(void) {
    uint8_t scratch[MT2_SKU_BATCH_BYTES];mt2_platform_policy policy;mt2_tx_power_plan plan;mt2_tx_power_result result;
    channel_setup(0);cf.domain.rules[1].flags=16;channel_reprepare();
    raw_prepare();CHECK(mt2_pcie_bind_tx(&v.mcu,&cf.tx)==0);
    CHECK(mt2_channel_apply(&cf.control)==0 && mt2_monitor_context_start(&cf.control,raw_mac)==0);
    CHECK(mt2_channel_monitor(&cf.control,149,149,20)==0);
    test_power_policy(&policy);CHECK(mt2_tx_power_prepare(&policy,7,20,&plan)==0);
    CHECK(mt2_tx_power_apply(&cf.control,&plan,scratch,sizeof(scratch),&result)==0);
}
static void raw_frame(uint8_t *p) {
    memset(p,0,40);p[0]=0xd0;p[4]=1;memcpy(p+10,raw_mac,6);p[22]=0x30;p[24]=0x7f;
}
static void raw_release(const uint32_t *ids,uint32_t count) {
    uint32_t i,index=v.mcu.receive[2].tail,bytes=8+4*count;uint8_t *p=v.pools[4]+index*8192;
    CHECK(count<=4 && !(v.descriptors[4][index].control&MT2_DMA_DONE));
    memset(p,0,bytes);put32(p,0x30000000u|count<<16|bytes);
    for(i=0;i<count;++i)put32(p+8+i*4,ids[i]<<16);
    v.descriptors[4][index].control=0xc0000000u|bytes<<16;
}
static void test_raw_binding(void) {
    mt2_ring_binding bindings[MT2_RING_COUNT];uint32_t i,before;uint64_t address;uint8_t *pool;uint32_t *states;
    mt2_register_io io={&v,0x100000,read_register,write_register,barrier,milliseconds,delay};
    setup(0);raw_prepare();CHECK(mt2_pcie_prepare(&v.mcu,&v.chip,v.banks)==0);before=v.io_count;
    address=cf.tx.pool_address;cf.tx.pool_address=v.banks[0].memory.pool_address;
    CHECK(mt2_pcie_bind_tx(&v.mcu,&cf.tx)==MT2_INVALID && !v.mcu.raw_tx);cf.tx.pool_address=address;
    pool=cf.tx.pool;cf.tx.pool=v.banks[0].memory.pool;
    CHECK(mt2_pcie_bind_tx(&v.mcu,&cf.tx)==MT2_INVALID && !v.mcu.raw_tx);cf.tx.pool=pool;
    states=cf.tx.states;cf.tx.states=(uint32_t *)cf.tx.pool;
    CHECK(mt2_pcie_bind_tx(&v.mcu,&cf.tx)==MT2_INVALID && !v.mcu.raw_tx);cf.tx.states=states;
    CHECK(mt2_pcie_bind_tx(&v.mcu,&cf.tx)==0 && !cf.tx.active && before==v.io_count);
    CHECK(mt2_pcie_bind_tx(&v.mcu,&cf.tx)==MT2_INVALID && before==v.io_count);
    bindings[0].address=cf.tx.descriptor_address+0x100;bindings[0].count=4;
    for(i=0;i<5;++i){bindings[i+1].address=v.banks[i].memory.descriptor_address;bindings[i+1].count=4;}
    CHECK(mt2_chip_start(&v.chip,&io,bindings)==0);
    CHECK(mt2_pcie_activate(&v.mcu,&v.transport)==MT2_INVALID && !v.mcu.active);stop();
    setup(0);raw_prepare();CHECK(mt2_pcie_prepare(&v.mcu,&v.chip,v.banks)==0 && mt2_pcie_bind_tx(&v.mcu,&cf.tx)==0);
    bindings[0].address=cf.tx.descriptor_address;
    CHECK(mt2_chip_start(&v.chip,&io,bindings)==0 && mt2_pcie_activate(&v.mcu,&v.transport)==0);
    CHECK(!cf.tx.active && !cf.tx.used);stop();CHECK(cf.tx.used && !cf.tx.active);
    channel_setup(0);raw_prepare();cf.tx.descriptor_address+=0x100;before=v.io_count;
    CHECK(mt2_pcie_bind_tx(&v.mcu,&cf.tx)==MT2_INVALID && !v.mcu.raw_tx && before==v.io_count);stop();
}
static void test_raw_transport(void) {
    uint8_t frame[40];uint32_t id=99,ids[4],before,i,extra;raw_frame(frame);test_raw_binding();
    raw_setup();before=v.io_count;
    cf.control.power_configured=0;CHECK(mt2_radio_tx_start(&cf.control)==MT2_INVALID && v.io_count==before);cf.control.power_configured=1;
    cf.control.context_configured=0;CHECK(mt2_radio_tx_start(&cf.control)==MT2_INVALID && v.io_count==before);cf.control.context_configured=1;
    cf.control.chainmask=2;CHECK(mt2_radio_tx_start(&cf.control)==MT2_INVALID && v.io_count==before);cf.control.chainmask=3;
    CHECK(mt2_radio_tx_start(&cf.control)==0 && cf.tx.active);before=v.io_count;
    CHECK(mt2_radio_tx_start(&cf.control)==MT2_INVALID && v.io_count==before);
    frame[10]^=2;CHECK(mt2_radio_tx_submit(&cf.control,frame,29,0,&id)==MT2_INVALID && id==99 && v.io_count==before);frame[10]^=2;
    CHECK(mt2_radio_tx_submit(&cf.control,frame,29,2,&id)==MT2_INVALID && id==99 && v.io_count==before);
    CHECK(mt2_radio_tx_submit(&cf.control,frame,29,0,&id)==0 && id==0 && cf.tx.inflight==1);
    CHECK((get32(rf.pool+4)&0x3ffu)==19 && (get32(rf.pool+4)>>24&0x3fu)==0);
    CHECK(get32(rf.pool+24)==0x004b0004 && (get32(rf.pool+28)>>11&31u)==24 && !memcmp(rf.pool+64,frame,29));
    CHECK(mt2_radio_tx_stop(&cf.control)==MT2_PENDING && cf.tx.active);
    before=v.io_count;CHECK(mt2_channel_monitor(&cf.control,6,6,20)==MT2_INVALID && v.io_count==before);
    /* Firmware release first: token stays owned until TX0 consumer advances. */
    extra=v.extra_records;raw_release(&id,1);CHECK(mt2_radio_tx_poll(&cf.control)==0);
    CHECK(rf.states[id]==5 && cf.tx.inflight==1 && cf.tx.queued==1 && extra==v.extra_records);
    v.bar[0xd430c/4]=cf.tx.head;CHECK(mt2_radio_tx_poll(&cf.control)==0 && !cf.tx.queued && !cf.tx.inflight);
    /* MCU exchanges poll the same raw queue; no separate completion loop. */
    CHECK(mt2_radio_tx_submit(&cf.control,frame,29,0,&id)==0);v.bar[0xd430c/4]=cf.tx.head;
    {uint32_t reply;CHECK(semaphore(&reply)==0 && rf.states[id]==3 && cf.tx.inflight==1);}
    raw_release(&id,1);CHECK(mt2_radio_tx_poll(&cf.control)==0 && !cf.tx.inflight);
    for(i=0;i<20;++i) {
        CHECK(mt2_radio_tx_submit(&cf.control,frame,29,0,&id)==0);v.bar[0xd430c/4]=cf.tx.head;
        raw_release(&id,1);CHECK(mt2_radio_tx_poll(&cf.control)==0 && !cf.tx.inflight && !cf.tx.queued);
    }
    /* Empty active queues may retune, but a new TX rechecks permission. */
    CHECK(mt2_channel_monitor(&cf.control,36,36,20)==0);before=v.io_count;
    CHECK(mt2_radio_tx_submit(&cf.control,frame,29,0,&id)==MT2_INVALID && v.io_count==before && cf.tx.active);
    CHECK(mt2_radio_tx_poll(&cf.control)==0);
    CHECK(mt2_channel_monitor(&cf.control,6,6,20)==0 && mt2_radio_tx_submit(&cf.control,frame,29,0,&id)==0);
    v.bar[0xd430c/4]=cf.tx.head;raw_release(&id,1);CHECK(mt2_radio_tx_poll(&cf.control)==0);
    CHECK(mt2_radio_tx_stop(&cf.control)==0 && !cf.tx.active && cf.tx.used);
    CHECK(mt2_radio_tx_start(&cf.control)==MT2_INVALID);
    CHECK(mt2_channel_monitor_stop(&cf.control)==0 && mt2_monitor_context_stop(&cf.control)==0);stop();
    {
        static const uint32_t rates[8]={12,18,24,36,48,72,96,108};
        static const uint32_t codes[8]={0x4b,0x4f,0x4a,0x4e,0x49,0x4d,0x48,0x4c};
        raw_setup();CHECK(mt2_radio_tx_start(&cf.control)==0);before=v.io_count;
        CHECK(mt2_radio_tx_submit_rate(&cf.control,frame,29,0x4b,0,&id)==MT2_INVALID && before==v.io_count);
        for(i=0;i<8;++i) {
            CHECK(mt2_radio_tx_submit_rate(&cf.control,frame,29,rates[i],0,&id)==0);
            CHECK(get32(rf.pool+id*MT2_SLOT_SIZE+24)==(codes[i]<<16|4u));
            v.bar[0xd430c/4]=cf.tx.head;raw_release(&id,1);CHECK(mt2_radio_tx_poll(&cf.control)==0);
        }
        stop();
    }
    /* Single observed chain selects WF0, not the two-chain duplicate index. */
    raw_setup();cf.control.nss=1;cf.control.chainmask=1;CHECK(mt2_radio_tx_start(&cf.control)==0);
    CHECK(mt2_radio_tx_submit(&cf.control,frame,29,0,&id)==0 && !(get32(rf.pool+28)&0xf800u));stop();
    raw_setup();CHECK(mt2_radio_tx_start(&cf.control)==0);
    CHECK(mt2_radio_tx_submit(&cf.control,frame,29,0,&ids[0])==0 && mt2_radio_tx_submit(&cf.control,frame,29,0,&ids[1])==0);
    before=v.io_count;id=99;CHECK(mt2_radio_tx_submit(&cf.control,frame,29,0,&id)==MT2_FULL && id==99 && v.io_count==before && cf.tx.active);
    v.bar[0xd430c/4]=cf.tx.head;CHECK(mt2_radio_tx_poll(&cf.control)==0 && cf.tx.inflight==2 && !cf.tx.queued);
    CHECK(mt2_radio_tx_submit(&cf.control,frame,29,0,&ids[2])==0 && mt2_radio_tx_submit(&cf.control,frame,29,0,&ids[3])==0);
    v.bar[0xd430c/4]=cf.tx.head;CHECK(mt2_radio_tx_poll(&cf.control)==0 && cf.tx.inflight==4 && !cf.tx.queued);
    CHECK(mt2_radio_tx_submit(&cf.control,frame,29,0,&id)==MT2_FULL);raw_release(ids,4);
    CHECK(mt2_radio_tx_poll(&cf.control)==0 && !cf.tx.inflight);stop();
    /* A bad release batch changes no token ownership, then disarms everything. */
    raw_setup();CHECK(mt2_radio_tx_start(&cf.control)==0);
    CHECK(mt2_radio_tx_submit(&cf.control,frame,29,0,&ids[0])==0 && mt2_radio_tx_submit(&cf.control,frame,29,0,&ids[1])==0);
    ids[1]=99;raw_release(ids,2);CHECK(mt2_radio_tx_poll(&cf.control)==MT2_PROTOCOL);
    CHECK(rf.states[ids[0]]==1 && cf.tx.queued==2 && cf.tx.inflight==2 && !cf.tx.active && !v.mcu.active && !cf.control.context_configured && !v.chip.quiescent);stop();
    /* Publication may have reached hardware before its reported error. */
    raw_setup();CHECK(mt2_radio_tx_start(&cf.control)==0);v.write_fail=0xd4308;id=99;
    CHECK(mt2_radio_tx_submit(&cf.control,frame,29,0,&id)==-88 && id==99);
    CHECK(cf.tx.queued==1 && cf.tx.inflight==1 && rf.states[0]==1 && !v.mcu.active && !cf.control.power_configured && !v.chip.quiescent);
    v.write_fail=0xffffffffu;stop();
    raw_setup();CHECK(mt2_radio_tx_start(&cf.control)==0);v.all_ones_read=0xd430c;
    CHECK(mt2_radio_tx_poll(&cf.control)==MT2_PROTOCOL && !cf.tx.active && !v.mcu.active);v.all_ones_read=0xffffffffu;stop();
    raw_setup();v.bar[0xd430c/4]=1;
    CHECK(mt2_radio_tx_start(&cf.control)==MT2_PROTOCOL && !cf.tx.active && !v.mcu.active);stop();
    raw_setup();CHECK(mt2_radio_tx_start(&cf.control)==0);v.all_ones_read=0xd441c;
    CHECK(mt2_radio_tx_poll(&cf.control)==MT2_PROTOCOL && !cf.tx.active && cf.tx.used);v.all_ones_read=0xffffffffu;stop();
    puts("{\"raw_tx_transport_tests\":\"passed\",\"completion_orders\":2,\"transport\":\"register_and_dma_simulation\",\"device_access\":false}");
}
static mt2_radio_session session;
static mt2_platform_data session_platform;
static mt2_rx_bridge session_receive;
static uint8_t session_storage[2*MT2_RX_MESSAGE_MAX],session_image[408],session_scratch[MT2_SESSION_SCRATCH];
static uint32_t session_lengths[2];
static mt2_radio_options session_options;
static void session_setup(void) {
    uint8_t regd[244],clc[240];uint32_t i;
    mac_setup();memset(&cf,0,sizeof(cf));
    CHECK(mt2_pcie_query_nic(&v.mcu,&cf.caps)==0 && mt2_mac_initialize(&v.mcu,&cf.mac)==0);
    raw_prepare();CHECK(mt2_pcie_bind_tx(&v.mcu,&cf.tx)==0);
    memset(&session,0,sizeof(session));memset(&session_receive,0,sizeof(session_receive));memset(&session_platform,0,sizeof(session_platform));
    CHECK(mt2_rx_bridge_prepare(&session_receive,session_storage,sizeof(session_storage),session_lengths,2)==0);
    for(i=0;i<4;++i)CHECK(mt2_acpi_decode(MT2_ACPI_NAME_NOT_FOUND,0,0,&session_platform.tables[i])==0);
    session_platform.attempted=1;session_platform.completed=1;session_platform.failed_index=4;
    CHECK(mt2_radio_session_prepare(&session,&v.mcu,&cf.mac,&cf.tx,&cf.caps,&session_platform,&session_receive)==MT2_INVALID); /* Existing callback belongs to fixture. */
    v.mcu.other_record=0;v.mcu.record_context=0;
    CHECK(mt2_radio_session_prepare(&session,&v.mcu,&cf.mac,&cf.tx,&cf.caps,&session_platform,&session_receive)==0);
    test_regd_image(regd);test_clc_image(clc);
    memcpy(session_image,regd,168);memcpy(session_image+168,clc,164);memcpy(session_image+332,clc+164,76);put32(session_image+352,332);
    memset(&session_options,0,sizeof(session_options));session_options.alpha2[0]='U';session_options.alpha2[1]='S';
    memcpy(session_options.address,raw_mac,6);session_options.primary=6;session_options.center=6;session_options.width=20;
    session_options.cap_half_dbm=20;session_options.enable_tx=1;
}
static int session_start(void) {
    return mt2_radio_session_start(&session,session_image,sizeof(session_image),&session_options,session_scratch,sizeof(session_scratch));
}
static void session_rx(const uint8_t *packet,uint32_t bytes) {
    uint32_t index=v.mcu.receive[1].tail;
    CHECK(bytes<512 && !(v.descriptors[3][index].control&MT2_DMA_DONE));
    memcpy(v.pools[3]+index*8192,packet,bytes);v.descriptors[3][index].control=0xc0000000u|bytes<<16;
}
static void test_session(void) {
    uint8_t frame[40],packet[128],out[128];uint32_t id,before,bytes,n=99,i;int e;
    static const uint32_t failed_stages[]={MT2_SESSION_REGDOMAIN,MT2_SESSION_HW_TYPE,MT2_SESSION_CLC,
        MT2_SESSION_CHANNEL_PREPARE,MT2_SESSION_CONTEXT,MT2_SESSION_MONITOR,MT2_SESSION_SKU,MT2_SESSION_TRANSMIT};
    session_setup();before=v.io_count;
    session_options.fcs_present=2;CHECK(session_start()==MT2_INVALID && !session.attempted && before==v.io_count);session_options.fcs_present=0;
    session_options.cap_half_dbm=45;CHECK(session_start()==MT2_INVALID && !session.attempted && before==v.io_count);session_options.cap_half_dbm=20;
    session_options.alpha2[0]='Z';CHECK(session_start()==MT2_INVALID && !session.attempted && before==v.io_count);session_options.alpha2[0]='U';
    session_platform.completed=0;CHECK(session_start()==MT2_INVALID && !session.attempted && before==v.io_count);session_platform.completed=1;
    CHECK(mt2_radio_session_start(&session,session_image,sizeof(session_image),&session_options,session_scratch,sizeof(session_scratch)-1)==MT2_INVALID && before==v.io_count);
    CHECK(session_start()==0 && session.ready && session.stage==MT2_SESSION_READY && cf.tx.active && session_receive.active);
    CHECK(session.policy.dfs_region==1 && session.power.dfs_region==1 && session.channel.primary==6 && session.sku.batches_drained==22 && session.clc.requests_drained==2);
    before=v.io_count;CHECK(session_start()==MT2_INVALID && before==v.io_count);
    CHECK(mt2_radio_session_receive(&session,out,sizeof(out),&n)==MT2_PENDING && n==99);
    bytes=test_raw_packet(packet);session_rx(packet,bytes);CHECK(mt2_radio_session_poll(&session)==0 && session_receive.queued==1);
    memset(v.pools[3],0,8192); /* Committed output must outlive the recycled DMA buffer. */
    CHECK(mt2_radio_session_receive(&session,out,50,&n)==MT2_FULL && n==99 && session_receive.queued==1);
    CHECK(mt2_radio_session_receive(&session,out,sizeof(out),&n)==0 && n==51 && out[0]==2 && !memcmp(out+22,packet+24,29));
    for(i=0;i<3;++i){session_rx(packet,bytes);CHECK(mt2_radio_session_poll(&session)==0);}
    CHECK(session_receive.queued==2 && session_receive.overflow==1 && session.ready);
    CHECK(mt2_radio_session_channel(&session,11,11,20)==0 && session.ready && session_receive.queued==2);
    raw_frame(frame);CHECK(mt2_radio_session_transmit(&session,frame,29,0,&id)==0);before=v.io_count;
    CHECK(mt2_radio_session_channel(&session,6,6,20)==MT2_PENDING && before==v.io_count && session.channel.primary==11);
    CHECK(mt2_radio_session_channel(&session,6,7,20)==MT2_INVALID && before==v.io_count && session.ready);
    v.bar[0xd430c/4]=cf.tx.head;raw_release(&id,1);CHECK(mt2_radio_session_poll(&session)==0);
    CHECK(mt2_radio_session_channel(&session,6,6,20)==0);
    mt2_radio_session_stop(&session);mt2_radio_session_stop(&session);
    CHECK(session.stopped && !session.ready && !session_receive.queued && !session_receive.active && !cf.tx.active && !v.mcu.active && !v.chip.quiescent);
    CHECK(mt2_radio_session_receive(&session,out,sizeof(out),&n)==MT2_INVALID && session_start()==MT2_INVALID);stop();
    session_setup();session_options.enable_tx=0;session_options.fcs_present=1;
    CHECK(session_start()==0 && session.ready && !cf.tx.active && !cf.tx.used);
    session_rx(packet,bytes);CHECK(mt2_radio_session_poll(&session)==0 && mt2_radio_session_receive(&session,out,sizeof(out),&n)==0 && out[16]==0x10);
    before=v.io_count;CHECK(mt2_radio_session_transmit(&session,frame,29,0,&id)==MT2_INVALID && before==v.io_count);
    packet[24]=3;session_rx(packet,bytes);CHECK(mt2_radio_session_poll(&session)==0 && session.ready && session_receive.discarded[MT2_RX_MALFORMED]==1);
    /* A malformed outer RXD is a transport fault, unlike a bad on-air MPDU. */
    packet[0]=1;session_rx(packet,bytes);CHECK(mt2_radio_session_poll(&session)==MT2_PROTOCOL && !session.ready && session.stopped);stop();
    for(i=0;i<8;++i) {
        session_setup();
        if(i==0)v.omit_reply=1;
        else if(i==1)v.hw_invalid=1;
        else if(i==2)v.omit_clc_at=2;
        else if(i==3){session_options.primary=149;session_options.center=149;}
        else if(i==4){v.context_fail_at=2;v.context_status=7;}
        else if(i==5)v.omit_radio_at=5;
        else if(i==6)v.omit_pse_at=2;
        else v.bar[0xd430c/4]=1;
        e=session_start();CHECK(e<0 && session.error==e && session.stage==failed_stages[i]);
        CHECK(session.attempted && session.stopped && !session.ready && !session_receive.active && !cf.tx.active && !v.mcu.active && !v.chip.quiescent);
        before=v.io_count;CHECK(session_start()==MT2_INVALID && before==v.io_count);stop();
    }
    session_setup();CHECK(session_start()==0);v.all_ones_read=0xd441c;
    CHECK(mt2_radio_session_poll(&session)==MT2_PROTOCOL && session.stopped && !session.ready && !session_receive.active);
    v.all_ones_read=0xffffffffu;stop();
    {
        static const uint8_t address[6]={2,0xa1,0xb2,0xc3,0xd4,0xe5};
        session_setup();CHECK(session_start()==0);raw_frame(frame);
        CHECK(mt2_radio_session_transmit(&session,frame,29,0,&id)==0);before=v.io_count;
        CHECK(mt2_radio_session_address(&session,address)==MT2_PENDING && v.io_count==before && !memcmp(session.channel.context_address,raw_mac,6));
        v.bar[0xd430c/4]=cf.tx.head;raw_release(&id,1);CHECK(mt2_radio_session_poll(&session)==0);
        CHECK(mt2_radio_session_address(&session,address)==0 && !memcmp(session.channel.context_address,address,6) && !memcmp(session.options.address,address,6));
        CHECK(v.context_count==3 && v.context_id[2]==MT2_MCU_DEVICE && !memcmp(v.context_payload[2]+10,address,6));
        before=v.io_count;CHECK(mt2_radio_session_transmit(&session,frame,29,0,&id)==MT2_INVALID && v.io_count==before && session.ready);
        memcpy(frame+10,address,6);CHECK(mt2_radio_session_transmit(&session,frame,29,0,&id)==0);
        v.bar[0xd430c/4]=cf.tx.head;raw_release(&id,1);CHECK(mt2_radio_session_poll(&session)==0);
        v.context_fail_at=4;v.context_status=7;
        CHECK(mt2_radio_session_address(&session,raw_mac)==MT2_PROTOCOL && session.stopped && !session.ready && !v.mcu.active);
        CHECK(!memcmp(session.channel.context_address,address,6));stop();
    }
    puts("{\"radio_session_tests\":\"passed\",\"injected_start_failures\":8,\"transport\":\"register_and_dma_simulation\",\"device_access\":false}");
}
void test_pcie_mcu(void) {
    uint8_t patch[164],ram[80],raw[64];mt2_fw_result result;uint32_t reply,i,before;uint64_t start;
    mt2_nic_caps caps,original;mt2_mac_state mac={0};
    setup(0);v.banks[1].memory.pool_address=v.banks[0].memory.pool_address;
    CHECK(mt2_pcie_prepare(&v.mcu,&v.chip,v.banks)==MT2_INVALID && !v.reads && !v.writes);
    setup(0);v.banks[1].memory.pool=v.banks[0].memory.pool;
    CHECK(mt2_pcie_prepare(&v.mcu,&v.chip,v.banks)==MT2_INVALID);
    setup(0);v.banks[2].assembly=(uint8_t *)&v.mcu;
    CHECK(mt2_pcie_prepare(&v.mcu,&v.chip,v.banks)==MT2_INVALID);
    setup(0);v.banks[2].memory.pool_address=0xffffff00ULL;
    CHECK(mt2_pcie_prepare(&v.mcu,&v.chip,v.banks)==MT2_INVALID);
    setup(1);images(patch,ram);
    CHECK(mt2_fw_boot(patch,sizeof(patch),ram,sizeof(ram),&v.transport,&result)==0);
    CHECK(result.ready && v.mcu.last_state==3 && v.commands==9 && v.scatter_bytes==8 && v.releases==1);
    CHECK(v.mcu.matched_responses==6 && !v.mcu.fault && v.rx_credit==3);
    CHECK(mt2_pcie_query_nic(&v.mcu,&caps)==0 && caps.present==15 && caps.bands==7 && caps.nss==2);
    CHECK(v.commands==10 && v.mcu.matched_responses==7 && !v.mcu.nic_target);
    CHECK(mt2_mac_initialize(&v.mcu,&mac)==0 && mac.initialized && v.commands==12);
    CHECK(v.transport.drain(&v.mcu)==0 && !v.mcu.command[0].queued && !v.mcu.command[1].queued);stop();
    /* DMA -> complete record -> Connac2 decoder -> radio_link RX, including a
     * descriptor boundary inside the 802.11 frame control field. */
    setup(1);CHECK(test_raw_packet(raw)==53);
    memcpy(v.pools[3],raw,25);memcpy(v.pools[3]+8192,raw+25,28);
    v.descriptors[3][0].control=0x80190000;v.descriptors[3][1].control=0xc01c0000;
    CHECK(mt2_pcie_poll(&v.mcu)==0 && v.mcu.receive[1].records==1 && v.bar[0xd4528/4]==1);
    CHECK(v.radio_bytes==51 && v.radio_outcome==MT2_RX_ACCEPT && !memcmp(v.radio_record+22,raw+24,29));stop();
    /* Sequence wrap and receive-ring wrap using actual encode/parse paths. */
    setup(1);for(i=0;i<40;++i){reply=99;CHECK(semaphore(&reply)==0 && reply==2);}
    CHECK(v.mcu.matched_responses==40 && v.mcu.sequence==10);stop();
    setup(1);reply=99;before=v.commands;
    CHECK(v.transport.send(&v.mcu,MT2_MCU_CE|0x8a,NULL,0,1,&reply)==MT2_INVALID);
    CHECK(reply==99 && v.commands==before && !v.mcu.fault);stop();
    /* A pre-existing matching reply must be discarded before publication. */
    setup(1);response(&v,0x10,1,99);v.reentrant=1;
    CHECK(semaphore(&reply)==0 && reply==2 && v.extra_records==1);stop();
    setup(1);v.duplicate_reply=1;
    CHECK(semaphore(&reply)==0 && reply==2 && v.extra_records==1 && v.mcu.matched_responses==1);stop();
    setup(1);v.wrong_sequence=1;reply=99;start=v.time;
    CHECK(semaphore(&reply)==MT2_FW_TIMEOUT && reply==99 && v.time-start==3000 && v.mcu.fault);
    before=v.commands;CHECK(semaphore(&reply)==MT2_FW_TIMEOUT && v.commands==before);stop();
    setup(1);v.unsolicited=1;reply=99;
    CHECK(semaphore(&reply)==MT2_FW_TIMEOUT && reply==99 && v.extra_records==1);stop();
    setup(1);v.wrong_eid=1;reply=99;
    CHECK(semaphore(&reply)==MT2_FW_TIMEOUT && reply==99 && v.extra_records==1);stop();
    setup(1);v.short_reply=1;reply=99;
    CHECK(semaphore(&reply)==MT2_PROTOCOL && reply==99 && !v.mcu.active);stop();
    setup(1);v.omit_reply=1;v.freeze_time=1;reply=99;start=v.time;
    CHECK(semaphore(&reply)==MT2_FW_TIMEOUT && reply==99 && v.time==start);stop();
    setup(1);v.hold_dma=1;reply=99;
    CHECK(semaphore(&reply)==MT2_FW_TIMEOUT && v.mcu.command[0].queued==1 && !v.chip.quiescent);stop();
    setup(1);v.hold_dma=1;v.reply_early=1;reply=99;
    CHECK(semaphore(&reply)==0 && reply==2 && v.mcu.command[0].queued==1);
    CHECK(v.transport.drain(&v.mcu)==MT2_FW_TIMEOUT && v.mcu.command[0].queued==1 && !v.chip.quiescent);stop();
    setup(1);v.write_fail=0xd4418;reply=99;
    CHECK(semaphore(&reply)==-88 && reply==99 && v.mcu.command[0].queued==1 && !v.chip.quiescent);
    v.write_fail=0xffffffffu;stop();
    setup(1);v.all_ones_read=0xd441c;reply=99;
    CHECK(semaphore(&reply)==MT2_PROTOCOL && reply==99 && !v.commands);
    v.all_ones_read=0xffffffffu;stop();
    setup(1);v.hold_rom=1;
    CHECK(mt2_fw_boot(patch,164,ram,80,&v.transport,&result)==MT2_FW_TIMEOUT && !result.ready && v.commands==1);stop();
    setup(1);v.hold_ready=1;
    CHECK(mt2_fw_boot(patch,164,ram,80,&v.transport,&result)==MT2_FW_TIMEOUT && result.start_acked && !result.ready);stop();
    setup(1);ram[46]=255;before=v.writes;
    CHECK(mt2_fw_boot(patch,164,ram,80,&v.transport,&result)==MT2_INVALID && !v.commands && v.writes==before);stop();
    setup(1);memset(&caps,0xa5,sizeof(caps));original=caps;
    CHECK(mt2_pcie_query_nic(&v.mcu,&caps)==MT2_INVALID && !v.commands && !memcmp(&caps,&original,sizeof(caps)));
    v.bar[0xe00f0/4]=3;CHECK(v.transport.wait_state(&v.mcu,1)==0);
    v.short_reply=1;
    CHECK(mt2_pcie_query_nic(&v.mcu,&caps)==MT2_PROTOCOL && !memcmp(&caps,&original,sizeof(caps)) && !v.mcu.nic_target);stop();
    setup(1);v.bar[0xe00f0/4]=3;CHECK(v.transport.wait_state(&v.mcu,1)==0);
    v.wrong_sequence=1;caps=original;
    CHECK(mt2_pcie_query_nic(&v.mcu,&caps)==MT2_FW_TIMEOUT && !memcmp(&caps,&original,sizeof(caps)) && !v.mcu.nic_target);stop();
    test_mac();
    test_regd_transport();
    test_clc_transport();
    test_channel_transport();
    test_power_transport();
    test_monitor_context();
    test_raw_transport();
    test_session();
    puts("{\"pcie_transport_tests\":\"passed\",\"transport\":\"register_and_dma_simulation\",\"device_access\":false}");
}
void test_pcie_image_buffers(const uint8_t *patch,uint32_t ps,const uint8_t *ram,uint32_t rs) {
    mt2_fw_result result;mt2_fw_plan plans[2];uint32_t i,j,k;
    const uint8_t *images[2]={patch,ram};uint64_t expected_hash=14695981039346656037ULL;
    CHECK(mt2_fw_validate(patch,ps,MT2_FW_PATCH,&plans[0])==0);
    CHECK(mt2_fw_validate(ram,rs,MT2_FW_RAM,&plans[1])==0);
    for(i=0;i<2;++i)for(j=0;j<plans[i].count;++j) {
        const mt2_fw_region *r=&plans[i].regions[j];if(r->skip)continue;
        for(k=0;k<r->length;++k)expected_hash=(expected_hash^images[i][r->offset+k])*1099511628211ULL;
    }
    setup(1);
    CHECK(mt2_fw_boot(patch,ps,ram,rs,&v.transport,&result)==0 && result.ready);
    CHECK(v.transport.drain(&v.mcu)==0 && v.scatter_bytes==result.bytes_sent && v.rx_credit==3);
    CHECK(v.scatter_hash==expected_hash); /* Every scatter byte, in order. */
    printf("{\"installed_image_pcie_boot\":\"passed\",\"transport\":\"register_and_dma_simulation\",\"bytes\":%u,\"commands\":%u,\"matched_responses\":%llu,\"device_access\":false}\n",v.scatter_bytes,v.commands,(unsigned long long)v.mcu.matched_responses);
    stop();
}
