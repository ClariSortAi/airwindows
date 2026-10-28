/* SPDX-License-Identifier: BSD-3-Clause-Clear */
#include "tx_power.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"TX POWER FAIL line %d: %s\n",__LINE__,#x);exit(1);}}while(0)
void test_power_policy(mt2_platform_policy *p) {
    uint32_t i;memset(p,0,sizeof(*p));p->prepared=1;p->dfs_region=1;
    p->clc.alpha2[0]='U';p->clc.alpha2[1]='S';p->clc.acpi_known=1;p->clc.mtcl_config=255;p->clc.world_mtcl_config=255;
    for(i=0;i<11;++i)p->sar_half_dbm[i]=(int32_t)(10+i);
}
void test_tx_power(void) {
    mt2_platform_policy p;mt2_tx_power_plan plan;uint8_t out[MT2_SKU_BATCH_BYTES],expected[MT2_SKU_BATCH_BYTES];
    uint32_t i,j,k,band,records[3],last;int n;
    test_power_policy(&p);CHECK(mt2_tx_power_prepare(&p,7,20,&plan)==0 && plan.batches==22);
    memset(expected,0,sizeof(expected));expected[4]=8;expected[5]=1;expected[8]='U';expected[9]='S';
    for(i=0;i<8;++i) {
        uint8_t *s=expected+45+162*i;expected[44+162*i]=(uint8_t)(i+1);memset(s,10,161);
        for(j=0;j<4;++j){s[39+j*12]=127;s[40+j*12]=127;}
    }
    CHECK(mt2_tx_power_request(&plan,0,out,sizeof(out))==1340 && !memcmp(out,expected,sizeof(out)));
    CHECK(mt2_tx_power_request(&plan,2,out,sizeof(out))==1340 && out[5]==2 && out[44]==36 && out[45]==127 && out[49]==11);
    CHECK(mt2_tx_power_request(&plan,8,out,sizeof(out))==1340 && out[5]==3 && out[44]==1 && out[45]==127 && out[49]==15);
    CHECK(mt2_tx_power_request(&plan,21,out,sizeof(out))==854 && out[4]==5 && out[5]==3 && out[6]==1 && out[44+4*162]==233);
    for(band=1;band<8;++band) {
        memset(records,0,sizeof(records));last=0;CHECK(mt2_tx_power_prepare(&p,band,20,&plan)==0);
        for(i=0;i<plan.batches;++i) {
            n=mt2_tx_power_request(&plan,i,out,sizeof(out));CHECK(n==(int)(44+out[4]*162u));
            CHECK(out[5]>=1 && out[5]<=3 && (band&(1u<<(out[5]-1))));records[out[5]-1]+=out[4];
            CHECK(out[6]==(i+1==plan.batches));last+=out[6];
            for(j=0;j<44;++j)if(j!=4 && j!=5 && j!=6 && j!=8 && j!=9)CHECK(out[j]==0);
            for(j=0;j<out[4];++j) {
                const uint8_t *s=out+45+j*162;
                for(k=0;k<161;++k) {
                    int reserved=(out[5]!=1 && k<4) || (k>=29 && k<77 && (k-29)%12>=10);
                    CHECK(reserved?s[k]==127:s[k]>=10 && s[k]<=20);
                }
            }
        }
        CHECK(last==1 && records[0]==(band&1?14u:0u) && records[1]==(band&2?48u:0u) && records[2]==(band&4?109u:0u));
    }
    CHECK(mt2_tx_power_prepare(&p,1,0,&plan)==0 && mt2_tx_power_request(&plan,0,out,sizeof(out))==1340 && out[49]==0);
    CHECK(mt2_tx_power_prepare(&p,1,-127,&plan)==0 && mt2_tx_power_request(&plan,0,out,sizeof(out))==1340 && out[49]==129);
    memset(out,0xa5,sizeof(out));CHECK(mt2_tx_power_request(&plan,plan.batches,out,sizeof(out))==MT2_INVALID && out[0]==0xa5);
    for(i=0;i<1340;++i)CHECK(mt2_tx_power_request(&plan,0,out,i)==MT2_INVALID && out[0]==0xa5);
    CHECK(mt2_tx_power_prepare(&p,7,45,&plan)==MT2_INVALID && !plan.prepared);
    CHECK(mt2_tx_power_prepare(&p,7,-128,&plan)==MT2_INVALID && !plan.prepared);
    p.sar_half_dbm[10]=128;CHECK(mt2_tx_power_prepare(&p,7,20,&plan)==MT2_INVALID && !plan.prepared);
    test_power_policy(&p);p.clc.acpi_known=0;CHECK(mt2_tx_power_prepare(&p,7,20,&plan)==MT2_INVALID && !plan.prepared);
    test_power_policy(&p);CHECK(mt2_tx_power_prepare(&p,8,20,&plan)==MT2_INVALID && !plan.prepared);
    puts("{\"tx_power_codec_tests\":\"passed\",\"sku_channel_records\":171,\"device_access\":false}");
}
