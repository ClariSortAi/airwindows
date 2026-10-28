/* SPDX-License-Identifier: BSD-3-Clause-Clear
 * Copyright (C) 2020, 2021, 2023 MediaTek Inc.
 * Connac2 routing/readiness from mt7921/pci_mcu.c and mt792x_core.c.
 * Bounded polling and error ownership follow the referenced Windows port;
 * see SOURCES.md. This implementation uses MT7922 queues and response layout.
 */
#include "pcie_mcu.h"
#define COMMAND_TIMEOUT 3000u
static const uint32_t registers[MT2_MCU_BANKS]={0xd4410,0xd4400,0xd4500,0xd4520,0xd4540};
typedef struct memory_span {uint64_t start,size;} memory_span;
static uint32_t get32(const uint8_t *p){return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static int pse_response(uint8_t sequence,const uint8_t *p,uint32_t size,uint32_t *value) {
    uint32_t declared;
    if(!p || !value || size<32)return MT2_INVALID;
    declared=p[24]|(uint32_t)p[25]<<8;if(declared<8 || declared!=size-24)return MT2_INVALID;
    if(p[29]!=sequence || (p[30]&4u))return MT2_PENDING;
    if(size!=44)return MT2_INVALID;
    if(get32(p+36)!=0x820c8000u)return MT2_PENDING;
    *value=get32(p+40);return 0;
}
static int add_span(memory_span *spans,uint32_t *count,uint64_t start,uint64_t size,uint64_t max) {
    uint32_t i;
    if(!start || !size || start>max || size-1>max-start)return 0;
    for(i=0;i<*count;++i) {
        uint64_t a=spans[i].start,b=spans[i].size;
        if(start<=a ? a-start<size : start-a<b)return 0;
    }
    spans[*count].start=start;spans[*count].size=size;++*count;return 1;
}
static int banks_valid(mt2_pcie_mcu *m,mt2_chip *c,const mt2_mcu_bank *banks) {
    memory_span va[16],pa[10];uint32_t vn=0,pn=0,i;
    if(!add_span(va,&vn,(uintptr_t)m,sizeof(*m),UINTPTR_MAX) ||
       !add_span(va,&vn,(uintptr_t)c,sizeof(*c),UINTPTR_MAX) ||
       !add_span(va,&vn,(uintptr_t)banks,sizeof(*banks)*MT2_MCU_BANKS,UINTPTR_MAX))return 0;
    for(i=0;i<MT2_MCU_BANKS;++i) {
        const mt2_dma_memory *b=&banks[i].memory;
        uint64_t ds=(uint64_t)b->count*16,ps=(uint64_t)b->count*b->slot_bytes;
        if(b->count<4 || b->count>2048 || b->slot_bytes<(i<2?4160u:64u) ||
           b->slot_bytes>MT2_MCU_MAX_PACKET || (b->slot_bytes&3u) || b->pool_bytes<ps ||
           (b->descriptor_address&15u) || (b->pool_address&3u) ||
           !add_span(va,&vn,(uintptr_t)b->descriptors,ds,UINTPTR_MAX) ||
           !add_span(va,&vn,(uintptr_t)b->pool,b->pool_bytes,UINTPTR_MAX) ||
           !add_span(pa,&pn,b->descriptor_address,ds,0xffffffffULL) ||
           !add_span(pa,&pn,b->pool_address,b->pool_bytes,0xffffffffULL))return 0;
        if(i>=2) {
            if(banks[i].capacity<44 || banks[i].capacity>MT2_RX_RECORD_MAX ||
               !add_span(va,&vn,(uintptr_t)banks[i].assembly,banks[i].capacity,UINTPTR_MAX))return 0;
        } else if(banks[i].assembly || banks[i].capacity)return 0;
    }
    return 1;
}
static void barrier(void *context) {
    mt2_pcie_mcu *m=((mt2_pcie_port *)context)->owner;
    m->chip->io.barrier(m->chip->io.context);
}
int mt2_pcie_bind_tx(mt2_pcie_mcu *m,mt2_tx_queue *q) {
    memory_span va[24],pa[12];uint32_t vn=0,pn=0,i;
    if(!m || !m->prepared || m->fault || m->busy || m->raw_tx || (!m->active && m->used) ||
       !q || !q->prepared || q->active || q->used || q->fault || q->queued || q->inflight ||
       q->count<4 || q->count>2048 || !q->tokens || q->tokens>MT2_TOKEN_MAX)return MT2_INVALID;
    if(m->chip->attempted && (!m->chip->active || m->chip->rings[0].address!=q->descriptor_address ||
       m->chip->rings[0].count!=q->count))return MT2_INVALID;
    if(!add_span(va,&vn,(uintptr_t)m,sizeof(*m),UINTPTR_MAX) ||
       !add_span(va,&vn,(uintptr_t)m->chip,sizeof(*m->chip),UINTPTR_MAX) ||
       !add_span(va,&vn,(uintptr_t)q,sizeof(*q),UINTPTR_MAX) ||
       !add_span(va,&vn,(uintptr_t)q->descriptors,(uint64_t)q->count*16,UINTPTR_MAX) ||
       !add_span(va,&vn,(uintptr_t)q->pool,(uint64_t)q->tokens*MT2_SLOT_SIZE,UINTPTR_MAX) ||
       !add_span(va,&vn,(uintptr_t)q->states,(uint64_t)q->tokens*4,UINTPTR_MAX) ||
       !add_span(va,&vn,(uintptr_t)q->slots,(uint64_t)q->count*4,UINTPTR_MAX) ||
       !add_span(pa,&pn,q->descriptor_address,(uint64_t)q->count*16,0xffffffffULL) ||
       !add_span(pa,&pn,q->pool_address,(uint64_t)q->tokens*MT2_SLOT_SIZE,0xffffffffULL))return MT2_INVALID;
    for(i=0;i<MT2_MCU_BANKS;++i) {
        const mt2_dma_memory *b=i<2?&m->command[i].memory:&m->receive[i-2].memory;
        if(!add_span(va,&vn,(uintptr_t)b->descriptors,(uint64_t)b->count*16,UINTPTR_MAX) ||
           !add_span(va,&vn,(uintptr_t)b->pool,b->pool_bytes,UINTPTR_MAX) ||
           !add_span(pa,&pn,b->descriptor_address,(uint64_t)b->count*16,0xffffffffULL) ||
           !add_span(pa,&pn,b->pool_address,b->pool_bytes,0xffffffffULL))return MT2_INVALID;
        if(i>=2 && !add_span(va,&vn,(uintptr_t)m->receive[i-2].assembly,m->receive[i-2].capacity,UINTPTR_MAX))return MT2_INVALID;
    }
    m->raw_tx=q;return 0;
}
static int publish(void *context,uint32_t index) {
    mt2_pcie_port *p=(mt2_pcie_port *)context;
    return mt2_reg_write(&p->owner->chip->io,p->registers+8,index);
}
static int record(void *context,const uint8_t *data,uint32_t size) {
    mt2_pcie_mcu *m=((mt2_pcie_port *)context)->owner;
    uint32_t word;int e;
    if(size<4)return MT2_PROTOCOL;
    word=get32(data);
    if((word&0xffffu)>size || (word&0xffffu)<4)return MT2_PROTOCOL;
    size=word&0xffffu;
    if((word>>27)==6 && m->raw_tx && m->raw_tx->active)
        return mt2_tx_firmware_complete(m->raw_tx,data,size);
    if((word>>27)==7 && ((word>>16)&15u)!=1 && m->waiting && !m->matched) {
        e=m->pse_target?pse_response(m->sequence,data,size,m->pse_target):
            m->hw_target?mt2_hw_type_response(m->sequence,data,size,m->hw_target):
            m->clc_target?mt2_clc_response(m->sequence,data,size,m->clc_target):
            m->regd_target?mt2_regd_response(m->sequence,data,size,m->regd_target):
            m->nic_target?mt2_nic_response(m->sequence,data,size,m->nic_target):
            mt2_mcu_response(m->command_id,m->sequence,data,size,&m->status);
        if(!e){m->matched=1;++m->matched_responses;return 0;}
        if(e!=MT2_PENDING)return MT2_PROTOCOL;
    }
    ++m->ignored_records;
    return m->other_record?m->other_record(m->record_context,data,size):0;
}
int mt2_pcie_prepare(mt2_pcie_mcu *m,mt2_chip *c,const mt2_mcu_bank banks[MT2_MCU_BANKS]) {
    uint32_t i;int e;
    if(!m || !c || !banks || m->prepared || m->used || c->attempted || !banks_valid(m,c,banks))return MT2_INVALID;
    m->chip=c;
    for(i=0;i<MT2_MCU_BANKS;++i) {
        m->ports[i].owner=m;m->ports[i].registers=registers[i];
        if(i<2) {
            mt2_tx_io io={&m->ports[i],barrier,publish};
            e=mt2_command_prepare(&m->command[i],&banks[i].memory,&io,i==1);
        } else {
            mt2_rx_io io={&m->ports[i],barrier,publish,record};
            e=mt2_rx_prepare(&m->receive[i-2],&banks[i].memory,banks[i].assembly,banks[i].capacity,&io);
        }
        if(e)return e;
    }
    m->prepared=1;return 0;
}
void mt2_pcie_disarm(mt2_pcie_mcu *m) {
    uint32_t i;if(!m)return;
    m->active=0;m->used=1;m->waiting=0;m->nic_target=0;m->regd_target=0;m->nic_valid=0;m->nic_chip_caps=0;
    m->hw_target=0;m->clc_target=0;m->pse_target=0;m->hw_type_valid=0;m->clc_completed=0;m->clc_channel_known=0;m->clc_channel_config=0;
    if(m->raw_tx)mt2_tx_disarm(m->raw_tx);
    for(i=0;i<2;++i)mt2_command_disarm(&m->command[i]);
    for(i=0;i<3;++i)mt2_rx_disarm(&m->receive[i]);
}
static int fail(mt2_pcie_mcu *m,int e) {
    if(!m->fault)m->fault=e;
    mt2_pcie_disarm(m);return m->fault;
}
static int poll_queues(mt2_pcie_mcu *m) {
    uint32_t i,index;int e;
    if(m->raw_tx) {
        if(m->raw_tx->fault)return fail(m,m->raw_tx->fault);
        if(m->raw_tx->active) {
            e=mt2_reg_read(&m->chip->io,0xd430c,&index);if(e)return fail(m,e);
            e=mt2_tx_dma_complete(m->raw_tx,index);
            if(e && e!=MT2_PENDING)return fail(m,e);
        }
    }
    for(i=0;i<2;++i) {
        e=mt2_reg_read(&m->chip->io,registers[i]+12,&index);if(e)return fail(m,e);
        e=mt2_command_complete(&m->command[i],index);
        if(e && e!=MT2_PENDING)return fail(m,e);
    }
    for(i=0;i<3;++i) {
        e=mt2_rx_poll(&m->receive[i],m->receive[i].memory.count);
        if(e<0)return fail(m,e);
    }
    return 0;
}
static int usable(mt2_pcie_mcu *m) {
    return m && m->prepared && m->active && m->chip->active && !m->fault;
}
int mt2_pcie_poll(mt2_pcie_mcu *m) {
    int e;
    if(!usable(m) || m->busy)return m && m->fault?m->fault:MT2_INVALID;
    m->busy=1;e=poll_queues(m);m->busy=0;return e;
}
static int status_command(uint32_t c) {
    return c==1 || c==2 || c==5 || c==7 || c==0x10 || c==(MT2_MCU_UNI|1u) ||
        c==(MT2_MCU_UNI|2u) || c==(MT2_MCU_UNI|3u) || c==(MT2_MCU_UNI|5u) ||
        c==(MT2_MCU_UNI|6u) || c==(MT2_MCU_UNI|7u) ||
        c==MT2_MCU_EFUSE_MODE || c==MT2_MCU_PROTECT || c==MT2_MCU_MAC_ENABLE ||
        c==MT2_MCU_RX_PATH || c==MT2_MCU_CHANNEL || c==MT2_MCU_SNIFFER;
}
static int pause_until(mt2_pcie_mcu *m,uint64_t start,uint32_t attempt,uint32_t timeout) {
    int e;
    if(attempt>=timeout || m->chip->io.milliseconds(m->chip->io.context)-start>=timeout)
        return fail(m,MT2_FW_TIMEOUT);
    e=m->chip->io.delay_ms(m->chip->io.context,1);return e?fail(m,e):0;
}
static int exchange(mt2_pcie_mcu *m,uint32_t cmd,const uint8_t *p,uint32_t size,int wait,
                    uint32_t *reply,mt2_nic_caps *caps,mt2_regdomain *domain,mt2_hw_type *hw,mt2_clc_reply *clc,uint32_t *pse) {
    mt2_command_queue *q;
    uint64_t start;uint32_t attempt;int e;
    if(!usable(m) || m->busy)return m && m->fault?m->fault:MT2_INVALID;
    if(!reply || (wait!=0 && wait!=1) || (wait && !status_command(cmd) &&
        !(caps && cmd==MT2_NIC_COMMAND) && !(domain && cmd==MT2_REGD_COMMAND) &&
        !(hw && cmd==MT2_HW_TYPE_COMMAND) && !(clc && cmd==MT2_CLC_COMMAND) && !(pse && cmd==MT2_MCU_PSE_READ)))return MT2_INVALID;
    m->busy=1;m->waiting=0;m->matched=0;
    q=&m->command[cmd==MT2_FW_SCATTER?1:0];
    start=m->chip->io.milliseconds(m->chip->io.context);
    for(attempt=0;;++attempt) {
        e=poll_queues(m);if(e)goto done;
        /* No pending record from before publication may satisfy this command. */
        e=mt2_command_submit(q,cmd,&m->sequence,p,size);
        if(!e)break;
        if(e!=MT2_FULL && e!=MT2_PENDING){if(q->fault)e=fail(m,e);goto done;}
        e=pause_until(m,start,attempt,COMMAND_TIMEOUT);if(e)goto done;
    }
    if(!wait){*reply=0;goto done;}
    m->waiting=1;m->command_id=cmd;m->status=0;m->nic_target=caps;m->regd_target=domain;
    m->hw_target=hw;m->clc_target=clc;m->pse_target=pse;
    start=m->chip->io.milliseconds(m->chip->io.context);
    for(attempt=0;;++attempt) {
        e=poll_queues(m);if(e)goto done;
        if(m->matched){*reply=m->status;break;}
        e=pause_until(m,start,attempt,COMMAND_TIMEOUT);if(e)goto done;
    }
done:
    m->waiting=0;m->busy=0;m->nic_target=0;m->regd_target=0;m->hw_target=0;m->clc_target=0;m->pse_target=0;return e;
}
static int send(void *ctx,uint32_t cmd,const uint8_t *p,uint32_t size,int wait,uint32_t *reply) {
    return exchange((mt2_pcie_mcu *)ctx,cmd,p,size,wait,reply,0,0,0,0,0);
}
int mt2_pcie_query_nic(mt2_pcie_mcu *m,mt2_nic_caps *out) {
    mt2_nic_caps caps={0};uint32_t reply;int e;
    if(!usable(m) || !out || (m->last_state&3u)!=3u)return m && m->fault?m->fault:MT2_INVALID;
    e=exchange(m,MT2_NIC_COMMAND,0,0,1,&reply,&caps,0,0,0,0);
    if(!e){*out=caps;m->nic_valid=1;m->nic_chip_caps=(caps.present&MT2_CAP_CHIP)?caps.chip_low:0;}
    return e;
}
static int drain(void *ctx) {
    mt2_pcie_mcu *m=(mt2_pcie_mcu *)ctx;uint64_t start;uint32_t attempt;int e;
    if(!usable(m) || m->busy)return m && m->fault?m->fault:MT2_INVALID;
    m->busy=1;start=m->chip->io.milliseconds(m->chip->io.context);
    for(attempt=0;;++attempt) {
        e=poll_queues(m);if(e)break;
        if(!m->command[0].queued && !m->command[1].queued)break;
        e=pause_until(m,start,attempt,COMMAND_TIMEOUT);if(e)break;
    }
    m->busy=0;return e;
}
int mt2_pcie_query_regdomain(mt2_pcie_mcu *m,const uint8_t *ram,uint32_t size,const uint8_t alpha[2],
                           uint8_t *scratch,uint32_t cap,mt2_regdomain *out) {
    mt2_regdomain domain;uint32_t reply;int length,e;
    if(!usable(m) || !out || m->busy || (m->last_state&3u)!=3u ||
       !m->nic_valid || !(m->nic_chip_caps&0x20u))return m && m->fault?m->fault:MT2_INVALID;
    length=mt2_regd_request(ram,size,alpha,scratch,cap);
    if(length<0)return length;
    e=exchange(m,MT2_REGD_COMMAND,scratch,(uint32_t)length,1,&reply,0,&domain,0,0,0);
    if(!e)e=drain(m);
    if(!e)*out=domain;
    return e;
}
int mt2_pcie_query_hw_type(mt2_pcie_mcu *m,mt2_hw_type *out) {
    static const uint8_t request[24]={0x50,5,0,0};
    mt2_hw_type hw;uint32_t reply;int e;
    if(!usable(m) || !out || !m->nic_valid || (m->last_state&3u)!=3u)return m && m->fault?m->fault:MT2_INVALID;
    e=exchange(m,MT2_HW_TYPE_COMMAND,request,sizeof(request),1,&reply,0,0,&hw,0,0);
    if(!e)e=drain(m);
    if(!e){*out=hw;m->hw_type_valid=1;m->hw_encapsulation=hw.encapsulation;}
    return e;
}
int mt2_pcie_apply_clc(mt2_pcie_mcu *m,const uint8_t *ram,uint32_t size,const mt2_clc_policy *policy,
                      uint8_t *scratch,uint32_t capacity,mt2_clc_result *out) {
    mt2_clc_plan plan;mt2_clc_reply reply_data={0};mt2_clc_policy copy;
    mt2_clc_result result={0};uint32_t ordinal,status,events,cap;int e=MT2_INVALID,length;
    if(!out)return MT2_INVALID;
    if(!usable(m) || !policy || !scratch || m->busy || m->clc_attempted || !m->nic_valid ||
       !m->hw_type_valid || (m->last_state&3u)!=3u)goto done;
    copy=*policy;cap=m->command[0].memory.slot_bytes-64;if(cap>capacity)cap=capacity;
    e=mt2_clc_plan_build(ram,size,m->hw_encapsulation,&copy,cap,&plan);if(e)goto done;
    result.selected_mask=plan.selected_mask;result.fallback_mask=plan.fallback_mask;
    m->clc_attempted=1;result.attempted=1;events=m->nic_chip_caps&1u;
    for(ordinal=0;ordinal<plan.requests;++ordinal) {
        length=mt2_clc_request(&plan,&copy,ordinal,events,scratch,cap);
        if(length<0){e=length;goto fault;}
        e=exchange(m,MT2_CLC_COMMAND,scratch,(uint32_t)length,(int)events,&status,0,0,0,events?&reply_data:0,0);
        if(e)goto fault;
        if(events){++result.requests_replied;result.channel_known=1;result.channel_config=reply_data.channel_config;}
        e=drain(m);if(e)goto fault;++result.requests_drained;
    }
    result.completed=1;m->clc_completed=1;m->clc_channel_known=result.channel_known;m->clc_channel_config=result.channel_config;
    goto done;
fault:
    result.channel_known=0;result.channel_config=0;e=fail(m,e);
done:
    result.error=e;*out=result;return e;
}
int mt2_pcie_radio_command(mt2_pcie_mcu *m,uint32_t cmd,const uint8_t *p,uint32_t size) {
    uint32_t reply;int wait=1,e;
    if(!usable(m) || !p || m->busy || !m->nic_valid || !m->clc_completed ||
       (m->last_state&3u)!=3u)return m && m->fault?m->fault:MT2_INVALID;
    if(cmd==MT2_MCU_MAC_ENABLE){if(size!=4)return MT2_INVALID;}
    else if(cmd==MT2_MCU_RX_PATH || cmd==MT2_MCU_CHANNEL){if(size!=76)return MT2_INVALID;}
    else if(cmd==MT2_MCU_SNIFFER){if(size!=12 && size!=20)return MT2_INVALID;}
    else if(cmd==MT2_MCU_RX_FILTER){if(size!=68)return MT2_INVALID;wait=0;}
    else if(cmd==MT2_MCU_DOMAIN){if(size<20 || size>348 || (size-12)%8)return MT2_INVALID;wait=0;}
    else if(cmd==MT2_MCU_SKU){if(size<206 || size>1340 || p[4]<1 || p[4]>8 || size!=44+162u*p[4] || p[5]<1 || p[5]>3)return MT2_INVALID;wait=0;}
    else return MT2_INVALID;
    e=exchange(m,cmd,p,size,wait,&reply,0,0,0,0,0);
    if(!e && reply)e=fail(m,MT2_PROTOCOL);
    if(!e)e=drain(m);
    return e;
}
int mt2_pcie_context_command(mt2_pcie_mcu *m,uint32_t cmd,const uint8_t *p,uint32_t size) {
    uint32_t reply;int e;
    if(!usable(m) || !p || m->busy || !m->nic_valid || !m->clc_completed ||
       (m->last_state&3u)!=3u)return m && m->fault?m->fault:MT2_INVALID;
    if(cmd==MT2_MCU_DEVICE){if(size!=16)return MT2_INVALID;}
    else if(cmd==MT2_MCU_BSS){if(size!=36)return MT2_INVALID;}
    else return MT2_INVALID;
    if(p[4] || p[5] || p[6]!=size-4 || p[7] || p[8]>1)return MT2_INVALID;
    e=exchange(m,cmd,p,size,1,&reply,0,0,0,0,0);
    if(!e && reply)e=fail(m,MT2_PROTOCOL);
    if(!e)e=drain(m);return e;
}
int mt2_pcie_query_pse(mt2_pcie_mcu *m,uint32_t *out) {
    static const uint8_t request[8]={0,0x80,0x0c,0x82,0,0,0,0};uint32_t value=0,reply;int e;
    if(!usable(m) || !out || m->busy || !m->nic_valid || !m->clc_completed || (m->last_state&3u)!=3u)return m && m->fault?m->fault:MT2_INVALID;
    e=exchange(m,MT2_MCU_PSE_READ,request,8,1,&reply,0,0,0,0,&value);
    if(!e)e=drain(m);if(!e)*out=value;return e;
}
int mt2_pcie_mac_command(mt2_pcie_mcu *m,uint32_t cmd) {
    static const uint8_t efuse[4]={0,1,0,0};
    static const uint8_t rts[12]={1,0,0,0,0x2b,9,0,0,2,0,0,0};
    const uint8_t *p;uint32_t size,reply;int e;
    if(!usable(m) || (m->last_state&3u)!=3u)return m && m->fault?m->fault:MT2_INVALID;
    if(cmd==MT2_MCU_EFUSE_MODE){p=efuse;size=sizeof(efuse);}
    else if(cmd==MT2_MCU_PROTECT){p=rts;size=sizeof(rts);}
    else return MT2_INVALID;
    e=exchange(m,cmd,p,size,1,&reply,0,0,0,0,0);
    if(!e && reply)e=fail(m,MT2_PROTOCOL);
    if(!e)e=drain(m);
    return e;
}
static int wait_state(void *ctx,uint32_t ready) {
    mt2_pcie_mcu *m=(mt2_pcie_mcu *)ctx;uint64_t start;uint32_t attempt,state;
    uint32_t expected=ready?3u:1u,mask=ready?3u:7u,timeout=ready?1500u:1000u;int e;
    if(!usable(m) || m->busy || ready>1)return m && m->fault?m->fault:MT2_INVALID;
    m->busy=1;start=m->chip->io.milliseconds(m->chip->io.context);
    for(attempt=0;;++attempt) {
        e=poll_queues(m);if(e)break;
        e=mt2_reg_read(&m->chip->io,0x7c0600f0u,&state);if(e){e=fail(m,e);break;}
        m->last_state=state;
        if((state&mask)==expected)break;
        e=pause_until(m,start,attempt,timeout);if(e)break;
    }
    m->busy=0;return e;
}
int mt2_pcie_activate(mt2_pcie_mcu *m,mt2_fw_transport *t) {
    uint32_t i;int e;
    if(!m || !t || !m->prepared || m->used || m->fault || !m->chip->active)return MT2_INVALID;
    if(m->raw_tx && (m->chip->rings[0].address!=m->raw_tx->descriptor_address ||
       m->chip->rings[0].count!=m->raw_tx->count))return MT2_INVALID;
    for(i=0;i<MT2_MCU_BANKS;++i) {
        const mt2_dma_memory *b=i<2?&m->command[i].memory:&m->receive[i-2].memory;
        const mt2_ring_binding *r=&m->chip->rings[i+1];
        const mt2_ring_binding *raw=&m->chip->rings[0];
        uint64_t start=b->pool_address,bytes=b->pool_bytes;
        if(r->address!=b->descriptor_address || r->count!=b->count ||
           (start<raw->address+(uint64_t)raw->count*16 && raw->address<start+bytes))return MT2_INVALID;
    }
    m->used=1;
    for(i=0;i<2;++i){e=mt2_command_activate(&m->command[i]);if(e)return fail(m,e);}
    for(i=0;i<3;++i){e=mt2_rx_activate(&m->receive[i]);if(e)return fail(m,e);}
    m->active=1;t->context=m;t->send=send;t->drain=drain;t->wait_state=wait_state;return 0;
}
