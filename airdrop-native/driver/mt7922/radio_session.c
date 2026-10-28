/* SPDX-License-Identifier: BSD-3-Clause-Clear
 * Lifecycle composition uses the pinned Windows port's one-attempt/retained-DMA
 * approach, with MT7922-specific stages and an owned receive-message queue.
 */
#include "radio_session.h"
int mt2_radio_options_valid(const mt2_radio_options *p) {
    uint32_t i,any=0;if(!p || p->enable_tx>1 || p->fcs_present>1 || p->environment>2 || p->power_type>4 ||
        p->cap_half_dbm< -127 || p->cap_half_dbm>44 || !p->primary || p->primary>177 ||
        !p->center || p->center>177 || (p->width!=20 && p->width!=40 && p->width!=80) || (p->address[0]&1u))return 0;
    if(!((p->alpha2[0]=='0' && p->alpha2[1]=='0') ||
       (p->alpha2[0]>='A' && p->alpha2[0]<='Z' && p->alpha2[1]>='A' && p->alpha2[1]<='Z')))return 0;
    for(i=0;i<6;++i)any|=p->address[i];return any!=0;
}
int mt2_radio_session_prepare(mt2_radio_session *s,mt2_pcie_mcu *m,const mt2_mac_state *mac,
    mt2_tx_queue *tx,const mt2_nic_caps *caps,const mt2_platform_data *platform,mt2_rx_bridge *rx) {
    if(!s || s->prepared || s->attempted || s->stopped || !m || !m->prepared || m->fault || m->busy ||
       m->raw_tx!=tx || m->other_record || !mac || !tx || !tx->prepared || tx->used || tx->active || tx->fault ||
       !caps || !platform || !rx || !rx->prepared || rx->used || rx->active)return MT2_INVALID;
    s->mcu=m;s->mac=mac;s->tx=tx;s->caps=caps;s->platform=platform;s->receive=rx;s->prepared=1;
    m->other_record=mt2_rx_bridge_record;m->record_context=rx;return 0;
}
void mt2_radio_session_stop(mt2_radio_session *s) {
    if(!s || !s->prepared)return;
    s->ready=0;s->stopped=1;s->channel.configured=0;s->channel.monitor=0;
    s->channel.power_configured=0;s->channel.context_configured=0;
    s->channel.primary=0;s->channel.center=0;s->channel.width=0;
    mt2_rx_bridge_stop(s->receive);mt2_tx_disarm(s->tx);mt2_pcie_disarm(s->mcu);
}
static int fail(mt2_radio_session *s,int e) {
    s->error=e;if(!s->mcu->fault)s->mcu->fault=e;
    s->channel.fault=e;mt2_radio_session_stop(s);return e;
}
int mt2_radio_session_start(mt2_radio_session *s,const uint8_t *ram,uint32_t size,const mt2_radio_options *options,uint8_t *scratch,uint32_t capacity) {
    mt2_radio_options config;uint32_t allowed;int e;
    if(!s || !s->prepared || s->attempted || s->stopped || !mt2_radio_options_valid(options) || !ram ||
       !scratch || capacity<MT2_SESSION_SCRATCH || !s->mcu->active || s->mcu->fault || s->mcu->busy ||
       !s->mcu->nic_valid || !s->mcu->chip->active || (s->mcu->last_state&3u)!=3 ||
       s->mcu->clc_attempted || !s->mac->initialized || s->tx->used || s->tx->active || s->tx->fault ||
       s->receive->used || s->mcu->other_record!=mt2_rx_bridge_record || s->mcu->record_context!=s->receive)return MT2_INVALID;
    config=*options;
    /* Preflight image/country, actual platform tables and finite SKU plan
     * before the first command. DFS=0 is validation-only and is replaced by
     * the firmware response before any power-policy command is sent. */
    e=mt2_regd_request(ram,size,config.alpha2,scratch,capacity);if(e<0)return e;
    e=mt2_platform_policy_build(s->platform,config.alpha2,config.environment,config.power_type,0,&s->policy);if(e)return e;
    e=mt2_tx_power_prepare(&s->policy,s->caps->bands,config.cap_half_dbm,&s->power);if(e)return e;
    s->options=config;s->attempted=1;s->stage=MT2_SESSION_REGDOMAIN;
    e=mt2_pcie_query_regdomain(s->mcu,ram,size,config.alpha2,scratch,capacity,&s->domain);if(e)return fail(s,e);
    s->stage=MT2_SESSION_HW_TYPE;e=mt2_pcie_query_hw_type(s->mcu,&s->hardware);if(e)return fail(s,e);
    s->stage=MT2_SESSION_POLICY;
    e=mt2_platform_policy_build(s->platform,config.alpha2,config.environment,config.power_type,s->domain.dfs_region,&s->policy);if(e)return fail(s,e);
    e=mt2_tx_power_prepare(&s->policy,s->caps->bands,config.cap_half_dbm,&s->power);if(e)return fail(s,e);
    s->stage=MT2_SESSION_CLC;e=mt2_pcie_apply_clc(s->mcu,ram,size,&s->policy.clc,scratch,capacity,&s->clc);if(e)return fail(s,e);
    s->stage=MT2_SESSION_CHANNEL_PREPARE;
    e=mt2_channel_prepare(&s->channel,s->mcu,s->mac,s->tx,&s->domain,s->caps,&s->policy.clc);if(e)return fail(s,e);
    e=mt2_channel_validate(&s->channel,config.primary,config.center,config.width,&allowed);if(e)return fail(s,e);
    if(config.enable_tx && !allowed)return fail(s,MT2_INVALID);
    s->stage=MT2_SESSION_DOMAIN;e=mt2_channel_apply(&s->channel);if(e)return fail(s,e);
    s->stage=MT2_SESSION_CONTEXT;e=mt2_monitor_context_start(&s->channel,config.address);if(e)return fail(s,e);
    s->stage=MT2_SESSION_MONITOR;e=mt2_channel_monitor(&s->channel,config.primary,config.center,config.width);if(e)return fail(s,e);
    s->stage=MT2_SESSION_SKU;e=mt2_tx_power_apply(&s->channel,&s->power,scratch,capacity,&s->sku);if(e)return fail(s,e);
    s->stage=MT2_SESSION_RECEIVE;e=mt2_rx_bridge_start(s->receive,config.fcs_present);if(e)return fail(s,e);
    if(config.enable_tx){s->stage=MT2_SESSION_TRANSMIT;e=mt2_radio_tx_start(&s->channel);if(e)return fail(s,e);}
    s->stage=MT2_SESSION_READY;s->ready=1;return 0;
}
int mt2_radio_session_poll(mt2_radio_session *s) {
    int e;if(!s || !s->ready || s->stopped)return MT2_INVALID;
    e=mt2_pcie_poll(s->mcu);return e?fail(s,e):0;
}
int mt2_radio_session_channel(mt2_radio_session *s,uint32_t primary,uint32_t center,uint32_t width) {
    uint32_t allowed;int e;if(!s || !s->ready || s->stopped)return MT2_INVALID;
    e=mt2_channel_validate(&s->channel,primary,center,width,&allowed);if(e)return e;
    if(s->tx->queued || s->tx->inflight)return MT2_PENDING;
    e=mt2_channel_monitor(&s->channel,primary,center,width);
    return s->channel.fault?fail(s,e):e;
}
int mt2_radio_session_transmit_rate(mt2_radio_session *s,const uint8_t *frame,uint32_t bytes,uint32_t rate,uint32_t no_ack,uint32_t *token) {
    int e;if(!s || !s->ready || s->stopped || !s->options.enable_tx)return MT2_INVALID;
    e=mt2_radio_tx_submit_rate(&s->channel,frame,bytes,rate,no_ack,token);
    return s->channel.fault?fail(s,e):e;
}
int mt2_radio_session_transmit(mt2_radio_session *s,const uint8_t *frame,uint32_t bytes,uint32_t no_ack,uint32_t *token) {
    return mt2_radio_session_transmit_rate(s,frame,bytes,12,no_ack,token);
}
int mt2_radio_session_receive(mt2_radio_session *s,uint8_t *out,uint32_t capacity,uint32_t *length) {
    if(!s || !s->ready || s->stopped)return MT2_INVALID;
    return mt2_rx_bridge_take(s->receive,out,capacity,length);
}
int mt2_radio_session_address(mt2_radio_session *s,const uint8_t *address) {
    uint32_t i;int e;if(!s || !s->ready || s->stopped)return MT2_INVALID;
    e=mt2_monitor_context_address(&s->channel,address);
    if(s->channel.fault)return fail(s,e);
    if(!e)for(i=0;i<6;++i)s->options.address[i]=s->channel.context_address[i];
    return e;
}
