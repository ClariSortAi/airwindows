/* SPDX-License-Identifier: BSD-3-Clause-Clear */
#include "radio_abi.h"
static uint32_t get32(const uint8_t *p){return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static int32_t signed32(uint32_t n){return n<=0x7fffffffu?(int32_t)n:-1-(int32_t)(~n);}
int mt2_radio_ioctl(uint32_t c) {
    return c==MT2_IOCTL_RADIO_START || c==MT2_IOCTL_RADIO_SERVICE || c==MT2_IOCTL_RADIO_RECEIVE ||
        c==MT2_IOCTL_RADIO_TRANSMIT || c==MT2_IOCTL_RADIO_CHANNEL || c==MT2_IOCTL_RADIO_STATUS || c==MT2_IOCTL_RADIO_ADDRESS;
}
int mt2_radio_request(uint32_t code,const uint8_t *p,uint32_t bytes,mt2_radio_view *out) {
    mt2_radio_view v={0};uint32_t i,ps,rs,any=0;
    if(!out || !mt2_radio_ioctl(code) || (bytes && !p))return MT2_INVALID;
    if(code==MT2_IOCTL_RADIO_START) {
        if(bytes<64 || bytes>64+2*MT2_FW_MAX_SIZE || get32(p)!=MT2_RADIO_START_MAGIC || get32(p+4)!=1 ||
           get32(p+56)!=MT2_RADIO_FIRMWARE_PAIR || get32(p+60))return MT2_INVALID;
        ps=get32(p+8);rs=get32(p+12);
        if(!ps || !rs || ps>MT2_FW_MAX_SIZE || rs>MT2_FW_MAX_SIZE || ps>bytes-64 || rs!=bytes-64-ps)return MT2_INVALID;
        v.firmware.patch=p+64;v.firmware.ram=p+64+ps;v.firmware.patch_bytes=ps;v.firmware.ram_bytes=rs;
        v.options.alpha2[0]=p[16];v.options.alpha2[1]=p[17];
        for(i=0;i<6;++i)v.options.address[i]=p[18+i];
        v.options.environment=get32(p+24);v.options.power_type=get32(p+28);
        v.options.primary=get32(p+32);v.options.center=get32(p+36);v.options.width=get32(p+40);
        v.options.fcs_present=get32(p+44);v.options.enable_tx=get32(p+48);v.options.cap_half_dbm=signed32(get32(p+52));
        if(!mt2_radio_options_valid(&v.options))return MT2_INVALID;
    } else if(code==MT2_IOCTL_RADIO_TRANSMIT) {
        static const uint32_t rates[8]={12,18,24,36,48,72,96,108};
        if(bytes<12+24 || bytes>12+MT2_PAYLOAD_MAX || get32(p)>1 || get32(p+8)!=bytes-12)return MT2_INVALID;
        v.rate=get32(p+4);for(i=0;i<8;++i)if(v.rate==rates[i])break;if(i==8)return MT2_INVALID;
        v.no_ack=get32(p);v.frame_bytes=bytes-12;v.frame=p+12;
    } else if(code==MT2_IOCTL_RADIO_CHANNEL) {
        if(bytes!=12)return MT2_INVALID;v.primary=get32(p);v.center=get32(p+4);v.width=get32(p+8);
        if(!v.primary || v.primary>177 || !v.center || v.center>177 || (v.width!=20 && v.width!=40 && v.width!=80))return MT2_INVALID;
    } else if(code==MT2_IOCTL_RADIO_ADDRESS) {
        if(bytes!=6 || (p[0]&1u))return MT2_INVALID;
        for(i=0;i<6;++i){v.address[i]=p[i];any|=p[i];}if(!any)return MT2_INVALID;
    } else if(bytes)return MT2_INVALID;
    *out=v;return 0;
}
