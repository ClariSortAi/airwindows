/* SPDX-License-Identifier: BSD-3-Clause-Clear */
#include "platform_acpi.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"ACPI FAIL line %d: %s\n",__LINE__,#x);exit(1);}}while(0)
static void put32(uint8_t *p,uint32_t n){p[0]=(uint8_t)n;p[1]=(uint8_t)(n>>8);p[2]=(uint8_t)(n>>16);p[3]=(uint8_t)(n>>24);}
static uint32_t encode(uint8_t *wire,const uint8_t *p,uint32_t n) {
    uint32_t i;memset(wire,0,MT2_ACPI_WIRE_BYTES);put32(wire,MT2_ACPI_OUTPUT_SIGNATURE);put32(wire+4,12+8*n);put32(wire+8,n);
    for(i=0;i<n;++i){wire[14+i*8]=4;wire[16+i*8]=p[i];}return 12+8*n;
}
typedef struct fixture {uint8_t tables[4][MT2_ACPI_TABLE_BYTES];uint32_t sizes[4],statuses[4],calls;} fixture;
static fixture v;
static int32_t evaluate(void *context,const uint8_t name[4],uint8_t *wire,uint32_t cap,uint32_t *size) {
    static const char names[4][5]={"MTCL","MTDS","MTGS","MTFG"};fixture *f=(fixture *)context;uint32_t i=f->calls++;
    CHECK(i<4 && !memcmp(name,names[i],4) && cap==MT2_ACPI_WIRE_BYTES);
    *size=f->sizes[i]?encode(wire,f->tables[i],f->sizes[i]):0;return (int32_t)f->statuses[i];
}
static void setup(void) {
    uint32_t i;memset(&v,0,sizeof(v));for(i=0;i<4;++i)v.statuses[i]=MT2_ACPI_NAME_NOT_FOUND;
}
static void data(uint32_t index,uint32_t n) {v.statuses[index]=0;v.sizes[index]=n;}
static int acquire(mt2_platform_data *out) {
    uint8_t wire[MT2_ACPI_WIRE_BYTES];mt2_acpi_io io={&v,evaluate};v.calls=0;
    return mt2_platform_read(&io,wire,sizeof(wire),out);
}
static void decode_tests(void) {
    uint8_t wire[MT2_ACPI_WIRE_BYTES],bytes[MT2_ACPI_TABLE_BYTES];mt2_acpi_table table;uint32_t n,i;
    memset(bytes,77,sizeof(bytes));n=encode(wire,bytes,19);
    CHECK(mt2_acpi_decode(0,wire,n,&table)==0 && table.state==MT2_ACPI_DATA && table.size==19 && table.data[18]==77);
    for(i=0;i<n;++i){CHECK(mt2_acpi_decode(0,wire,i,&table)==MT2_INVALID && table.state==MT2_ACPI_UNKNOWN && !table.size);}
    CHECK(mt2_acpi_decode(0,wire,n+1,&table)==MT2_INVALID);
    CHECK(mt2_acpi_decode(0xc00000b5u,wire,0,&table)==MT2_INVALID && table.ntstatus==0xc00000b5u);
    CHECK(mt2_acpi_decode(0xc0000010u,wire,0,&table)==MT2_INVALID); /* unsupported is not absent */
    CHECK(mt2_acpi_decode(MT2_ACPI_NAME_NOT_FOUND,wire,1,&table)==MT2_INVALID);
    CHECK(mt2_acpi_decode(MT2_ACPI_NAME_NOT_FOUND,0,0,&table)==0 && table.state==MT2_ACPI_ABSENT);
    wire[0]^=1;CHECK(mt2_acpi_decode(0,wire,n,&table)==MT2_INVALID);wire[0]^=1;
    put32(wire+8,0xffffffffu);CHECK(mt2_acpi_decode(0,wire,n,&table)==MT2_INVALID);encode(wire,bytes,19);
    wire[12]=3;CHECK(mt2_acpi_decode(0,wire,n,&table)==MT2_INVALID);wire[12]=0;
    wire[14]=8;CHECK(mt2_acpi_decode(0,wire,n,&table)==MT2_INVALID);wire[14]=4;
    wire[n-3]=1;CHECK(mt2_acpi_decode(0,wire,n,&table)==MT2_INVALID && !table.state && !table.data[0]);
    n=encode(wire,bytes,MT2_ACPI_TABLE_BYTES);CHECK(mt2_acpi_decode(0,wire,n,&table)==0 && table.size==263);
}
static void policy_tests(void) {
    const uint8_t us[2]={'U','S'},de[2]={'D','E'},eu[2]={'E','U'};mt2_platform_data p;mt2_platform_policy q;uint32_t i,j;
    setup();CHECK(acquire(&p)==0 && v.calls==4 && mt2_platform_validate(&p)==0);
    CHECK(mt2_platform_policy_build(&p,us,0,0,1,&q)==0 && q.prepared && q.clc.acpi_known && q.clc.acpi_flags==1 && q.clc.mtcl_config==255);
    CHECK(q.clc.world_mtcl_config==255 && !q.dynamic_active);
    for(i=0;i<11;++i)CHECK(q.sar_half_dbm[i]==127);
    p.completed=0;CHECK(mt2_platform_policy_build(&p,us,0,0,1,&q)==MT2_INVALID && !q.prepared && !q.clc.acpi_known);
    setup();data(0,19);v.tables[0][4]=2;v.tables[0][5]=1;v.tables[0][9]=8;v.tables[0][12]=1;v.tables[0][16]=8;
    data(3,11);v.tables[3][6]=3;v.tables[3][8]=1;v.tables[3][9]=4;v.tables[3][10]=99;
    CHECK(acquire(&p)==0 && mt2_platform_policy_build(&p,us,2,1,2,&q)==0);
    CHECK(q.clc.mtcl_config==0x25 && q.clc.world_mtcl_config==0x20 && q.clc.acpi_flags==7 && q.clc.environment==2 && q.clc.power_type==1);
    data(0,26);v.tables[0][4]=3;v.tables[0][19]=1;v.tables[0][23]=0x80;v.tables[0][6]=0x40;v.tables[0][13]=0x40;
    CHECK(acquire(&p)==0 && mt2_platform_policy_build(&p,us,0,0,0,&q)==0 && q.clc.mtcl_config==0x15);
    CHECK(mt2_platform_policy_build(&p,de,0,0,0,&q)==0 && q.clc.mtcl_config==0x15);
    CHECK(mt2_platform_policy_build(&p,eu,0,0,0,&q)==0 && q.clc.mtcl_config==5); /* upstream v3 map distinction */
    v.tables[0][5]=2;v.tables[0][19]=255;CHECK(acquire(&p)==0 && mt2_platform_policy_build(&p,us,0,0,0,&q)==0 && q.clc.mtcl_config==9);
    /* V1 dynamic/geo layout is selected from length/count, independent of MTCL. */
    setup();data(1,12);v.tables[1][4]=1;v.tables[1][5]=1;
    for(i=0;i<5;++i)v.tables[1][7+i]=(uint8_t)(20+i*10);v.tables[1][11]=255;
    data(2,21);v.tables[2][5]=3;
    for(i=0;i<3;++i){v.tables[2][7+i*5]=25;v.tables[2][8+i*5]=3;v.tables[2][9+i*5]=35;v.tables[2][10+i*5]=5;}
    CHECK(acquire(&p)==0 && mt2_platform_policy_build(&p,us,0,0,1,&q)==0 && q.dynamic_version==1 && q.geographic_version==1);
    CHECK(q.sar_half_dbm[0]==23);for(i=1;i<5;++i)CHECK(q.sar_half_dbm[i]==35);for(i=5;i<11;++i)CHECK(q.sar_half_dbm[i]==127);
    v.tables[1][4]=0;CHECK(acquire(&p)==0 && mt2_platform_policy_build(&p,us,0,0,1,&q)==0 && !q.dynamic_active);
    for(i=0;i<11;++i)CHECK(q.sar_half_dbm[i]==127);
    /* Full V2 layouts; geographic rows differ for FCC/ETSI/world. */
    setup();data(1,31);v.tables[1][4]=1;v.tables[1][6]=2;
    for(i=0;i<11;++i){v.tables[1][8+i]=40;v.tables[1][20+i]=1;} /* first row only */
    data(2,28);v.tables[2][6]=3;
    for(i=0;i<3;++i)for(j=0;j<3;++j){v.tables[2][8+i*7+j*2]=(uint8_t)(30+i*10);v.tables[2][9+i*7+j*2]=5;}
    CHECK(acquire(&p)==0 && mt2_platform_policy_build(&p,us,0,0,1,&q)==0 && q.dynamic_version==2 && q.geographic_version==2);
    for(i=0;i<11;++i)CHECK(q.sar_half_dbm[i]==30);
    CHECK(mt2_platform_policy_build(&p,us,0,0,2,&q)==0 && q.sar_half_dbm[10]==40);
    CHECK(mt2_platform_policy_build(&p,us,0,0,3,&q)==0 && q.sar_half_dbm[10]==45);
    v.tables[2][26]=255;v.tables[2][27]=255;CHECK(acquire(&p)==0 && mt2_platform_policy_build(&p,us,0,0,0,&q)==0 && q.sar_half_dbm[10]==40);
    v.tables[2][26]=127;v.tables[2][27]=255;CHECK(acquire(&p)==0 && mt2_platform_policy_build(&p,us,0,0,0,&q)==0 && q.sar_half_dbm[10]==127);
    CHECK(mt2_platform_policy_build(&p,us,0,0,4,&q)==MT2_INVALID && !q.prepared);
    v.tables[1][20]=128;CHECK(acquire(&p)==MT2_INVALID && p.failed_index==1 && !p.completed && v.calls==2); /* later unused row validated */
}
static void acquisition_failures(void) {
    mt2_platform_data p;uint32_t i,n;mt2_platform_policy q;const uint8_t us[2]={'U','S'};
    for(i=0;i<4;++i) {
        setup();v.statuses[i]=0xc00000b5u;
        CHECK(acquire(&p)==MT2_INVALID && p.failed_index==i && v.calls==i+1 && !p.completed);
        CHECK(mt2_platform_policy_build(&p,us,0,0,1,&q)==MT2_INVALID && !q.prepared);
        CHECK(p.tables[i].state==MT2_ACPI_UNKNOWN && p.tables[i].ntstatus==0xc00000b5u);
    }
    setup();data(0,19);v.tables[0][4]=3;CHECK(acquire(&p)==MT2_INVALID && p.failed_index==0);
    setup();data(3,8);v.tables[3][6]=1;CHECK(acquire(&p)==MT2_INVALID && p.failed_index==3);
    setup();data(1,12);v.tables[1][4]=1;v.tables[1][5]=2;CHECK(acquire(&p)==MT2_INVALID && p.failed_index==1);
    /* Every incomplete fixed-layout MTCL response fails before subsequent calls. */
    for(n=4;n<19;++n){setup();data(0,n);v.tables[0][4]=2;CHECK(acquire(&p)==MT2_INVALID && v.calls==1);}
    setup();data(3,263);v.tables[3][6]=255;memset(v.tables[3]+8,1,255);
    CHECK(acquire(&p)==0 && mt2_platform_policy_build(&p,us,0,0,1,&q)==0 && q.clc.acpi_flags==3);
}
void test_platform_acpi(void) {
    decode_tests();policy_tests();acquisition_failures();
    puts("{\"platform_acpi_tests\":\"passed\",\"acpi_responses\":\"synthetic_only\",\"device_access\":false}");
}
