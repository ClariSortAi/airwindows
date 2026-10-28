/* SPDX-License-Identifier: BSD-3-Clause-Clear
 * Copyright (C) 2023 MediaTek Inc.
 * MTCL/MTDS/MTGS/MTFG layouts and mapping: mt792x_acpi_sar.c/h, mac80211.c.
 * Windows V1 decoder/acquisition approach: pinned Windows platform_acpi.c.
 * Strict lengths, failure invalidation and fixed-method acquisition are port
 * additions. This file does not evaluate ACPI or operate a radio by itself.
 */
#include "platform_acpi.h"
static uint32_t le16(const uint8_t *p){return p[0]|(uint32_t)p[1]<<8;}
static uint32_t le32(const uint8_t *p){return le16(p)|le16(p+2)<<16;}
static void zero(void *p,uint32_t n){uint32_t i;for(i=0;i<n;++i)((uint8_t *)p)[i]=0;}
int mt2_acpi_decode(uint32_t status,const uint8_t *p,uint32_t size,mt2_acpi_table *out) {
    uint32_t count,i;
    if(!out)return MT2_INVALID;zero(out,sizeof(*out));out->ntstatus=status;
    if(status==MT2_ACPI_NAME_NOT_FOUND) {
        if(size)return MT2_INVALID;out->state=MT2_ACPI_ABSENT;return 0;
    }
    if(status || !p || size<12 || size>MT2_ACPI_WIRE_BYTES ||
       le32(p)!=MT2_ACPI_OUTPUT_SIGNATURE || le32(p+4)!=size)return MT2_INVALID;
    count=le32(p+8);
    if(count<4 || count>MT2_ACPI_TABLE_BYTES || size!=12+count*8)return MT2_INVALID;
    for(i=0;i<count;++i) {
        const uint8_t *a=p+12+i*8;
        if(le16(a)!=0 || le16(a+2)!=4 || le32(a+4)>255)return MT2_INVALID;
    }
    for(i=0;i<count;++i)out->data[i]=p[16+i*8];
    out->size=count;out->state=MT2_ACPI_DATA;return 0;
}
static int present(const mt2_acpi_table *t) {
    if(t->state==MT2_ACPI_ABSENT)return t->ntstatus==MT2_ACPI_NAME_NOT_FOUND && !t->size?0:MT2_INVALID;
    return t->state==MT2_ACPI_DATA && !t->ntstatus && t->size>=4 && t->size<=MT2_ACPI_TABLE_BYTES?1:MT2_INVALID;
}
static int fits(const mt2_acpi_table *t,uint32_t h,uint32_t row,uint32_t min) {
    uint32_t n;
    if(t->size<h || (t->size-h)%row)return 0;
    n=(t->size-h)/row;return n>=min && n<=8 && t->data[h-1]==n;
}
static int sar_layout(const mt2_acpi_table *t,uint32_t geo,uint32_t *h,uint32_t *stride) {
    uint32_t at,j,bands;int exists=present(t);
    *h=0;*stride=0;if(exists<=0)return exists;
    if(fits(t,6,geo?5u:6u,geo?3u:1u)){*h=6;*stride=geo?5u:6u;}
    else if(fits(t,7,geo?7u:12u,geo?3u:1u)){*h=7;*stride=geo?7u:12u;}
    else return MT2_INVALID;
    bands=geo?(*stride-1)/2:*stride-1;
    for(at=*h;at<t->size;at+=*stride)for(j=0;j<bands;++j) {
        uint8_t v=t->data[at+1+j*(geo?2u:1u)];
        /* Preserve 0xff as no limit. Do not guess signed OEM encodings. */
        if(v>127 && v!=255)return MT2_INVALID;
    }
    return 0;
}
static int table_valid(uint32_t index,const mt2_acpi_table *t) {
    uint32_t h,stride;int exists=present(t);if(exists<=0)return exists;
    if(index==MT2_ACPI_MTCL) {
        return ((t->size==19 && (t->data[4]==1 || t->data[4]==2)) ||
                (t->size==26 && t->data[4]==3))?0:MT2_INVALID;
    }
    if(index==MT2_ACPI_MTFG)return t->size>=8 && t->data[6]<=t->size-8?0:MT2_INVALID;
    return sar_layout(t,index==MT2_ACPI_MTGS,&h,&stride);
}
int mt2_platform_validate(const mt2_platform_data *p) {
    uint32_t i;
    if(!p || p->attempted!=1 || p->completed!=1 || p->error || p->failed_index!=4)return MT2_INVALID;
    for(i=0;i<4;++i)if(table_valid(i,&p->tables[i]))return MT2_INVALID;
    return 0;
}
int mt2_platform_read(const mt2_acpi_io *io,uint8_t *wire,uint32_t capacity,mt2_platform_data *out) {
    static const uint8_t names[4][4]={{'M','T','C','L'},{'M','T','D','S'},{'M','T','G','S'},{'M','T','F','G'}};
    uint32_t i,size,status;int error;
    if(!out)return MT2_INVALID;zero(out,sizeof(*out));out->error=MT2_INVALID;out->failed_index=4;
    if(!io || !io->evaluate || !wire || capacity<MT2_ACPI_WIRE_BYTES)return MT2_INVALID;
    out->attempted=1;
    for(i=0;i<4;++i) {
        zero(wire,MT2_ACPI_WIRE_BYTES);size=0;
        status=(uint32_t)io->evaluate(io->context,names[i],wire,MT2_ACPI_WIRE_BYTES,&size);
        error=mt2_acpi_decode(status,wire,size,&out->tables[i]);
        if(!error)error=table_valid(i,&out->tables[i]);
        if(error){out->failed_index=i;out->error=error;return error;}
    }
    out->completed=1;out->error=0;return 0;
}
static const char countries[][3]={"00","EU","AR","AU","AZ","BY","BO","BR","CA","CL","CN","ID","JP","MY","MX","ME",
    "MA","NZ","NG","PH","RU","RS","SG","KR","TW","TH","UA","GB","US","VN","KH","PY"};
static const char europe[][3]={"AD","AT","BE","BG","CY","CZ","HR","DK","EE","FI","FR","DE","GR","HU","IS","IE",
    "IT","LV","LI","LT","LU","MC","MT","NL","NO","PL","PT","RO","SK","SI","ES","SE","CH"};
static const char be_countries[][3]={"AR","BR","BY","CL","IQ","MX","OM","RU","RW","VN","KR","UA","","","","",
    "EU","AT","CN","CA","TW","NZ","PH","UK","US"};
static int eq(const uint8_t *a,const char *b){return a[0]==(uint8_t)b[0] && a[1]==(uint8_t)b[1];}
static uint32_t mtcl(const mt2_acpi_table *t,const uint8_t *alpha) {
    const uint8_t *d=t->data;uint32_t i,row=0,bit=0x80,config=0,be=0x20,mode;
    if(t->state==MT2_ACPI_ABSENT)return 255;
    for(i=0;i<sizeof(countries)/sizeof(countries[0]);++i)if(eq(alpha,countries[i]))break;
    if(i<sizeof(countries)/sizeof(countries[0])){row=i/8;bit=1u<<(7-i%8);}
    else for(i=0;i<sizeof(europe)/sizeof(europe[0]);++i)if(eq(alpha,europe[i])){bit=0x40;break;}
    mode=d[5]>2?0:d[5];if((d[6+row]&bit) || d[5]==2)config|=mode<<2;
    mode=d[12]>1?0:d[12];if(d[4]>1 && (d[13+row]&bit))config|=mode;
    if(d[4]==3) {
        row=3;bit=0x80;
        for(i=0;i<sizeof(be_countries)/sizeof(be_countries[0]);++i)if(eq(alpha,be_countries[i]))break;
        if(i<sizeof(be_countries)/sizeof(be_countries[0])){row=i/8;bit=1u<<(7-i%8);}
        else {
            for(i=0;i<sizeof(europe)/sizeof(europe[0]);++i)if(eq(alpha,europe[i]))break;
            if(i==sizeof(europe)/sizeof(europe[0]))return config|be;
        }
        mode=d[19]>2?0:d[19];be=(d[20+row]&bit)?mode<<4:0;
    }
    return config|be;
}
int mt2_platform_policy_build(const mt2_platform_data *p,const uint8_t alpha[2],uint32_t env,uint32_t power,
                              uint32_t region,mt2_platform_policy *out) {
    const mt2_acpi_table *d,*g,*f;uint32_t dh,dr,gh,gr,i,band,at,row;
    if(!out)return MT2_INVALID;zero(out,sizeof(*out));
    if(mt2_platform_validate(p) || !alpha || env>2 || power>4 || region>3 ||
       !((alpha[0]=='0' && alpha[1]=='0') || (alpha[0]>='A' && alpha[0]<='Z' && alpha[1]>='A' && alpha[1]<='Z')))return MT2_INVALID;
    d=&p->tables[MT2_ACPI_MTDS];g=&p->tables[MT2_ACPI_MTGS];f=&p->tables[MT2_ACPI_MTFG];
    if(sar_layout(d,0,&dh,&dr) || sar_layout(g,1,&gh,&gr))return MT2_INVALID;
    out->clc.alpha2[0]=alpha[0];out->clc.alpha2[1]=alpha[1];out->clc.environment=env;out->clc.power_type=power;
    out->clc.mtcl_config=mtcl(&p->tables[0],alpha);out->clc.world_mtcl_config=mtcl(&p->tables[0],(const uint8_t *)"00");
    /* Completed acquisition corresponds to allocated upstream acpisar, even
     * when every optional method is absent. Unknown acquisition never does. */
    out->clc.acpi_flags=1;
    if(f->state==MT2_ACPI_DATA)for(i=0;i<f->data[6];++i) {
        if(f->data[8+i]==1)out->clc.acpi_flags|=2;
        if(f->data[8+i]==4)out->clc.acpi_flags|=4;
    }
    out->dynamic_version=dh==6?1u:dh==7?2u:0u;out->geographic_version=gh==6?1u:gh==7?2u:0u;
    out->dynamic_active=dh && d->data[4];row=region==1?0u:region==2?1u:2u;
    for(i=0;i<11;++i) {
        int32_t value=127;
        if(out->dynamic_active && i<dr-1) {
            value=d->data[dh+1+i]==255?127:d->data[dh+1+i];band=i==0?0u:i<5?1u:2u;
            if(gh && band<(gr-1)/2) {
                at=gh+row*gr+1+band*2;
                if(g->data[at]!=255) {
                    value+=g->data[at+1];if(value>127)value=127;
                    if(value>g->data[at])value=g->data[at];
                }
            }
        }
        out->sar_half_dbm[i]=value;
    }
    out->dfs_region=region;out->clc.acpi_known=1;out->prepared=1;return 0;
}
