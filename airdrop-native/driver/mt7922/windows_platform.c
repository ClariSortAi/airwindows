/* SPDX-License-Identifier: BSD-3-Clause-Clear
 * Native KMDF adapters for the Connac2 core. BAR matching and common-buffer
 * lifetime approach adapted from the pinned Windows MT7925 device.c reference.
 * The separate bring-up entry point attaches this to PnP callbacks.
 */
#include "windows_platform.h"
#include <wdmguid.h>
#include <bcrypt.h>
#include <acpiioct.h>
C_ASSERT(sizeof(mt2_descriptor)==16);
C_ASSERT(ACPI_EVAL_OUTPUT_BUFFER_SIGNATURE_V1==MT2_ACPI_OUTPUT_SIGNATURE);
C_ASSERT(FIELD_OFFSET(ACPI_EVAL_OUTPUT_BUFFER_V1,Argument)==12);
C_ASSERT(sizeof(ACPI_METHOD_ARGUMENT_V1)==8);
C_ASSERT(FIELD_OFFSET(ACPI_METHOD_ARGUMENT_V1,Data)==4);
C_ASSERT(ACPI_METHOD_ARGUMENT_INTEGER==0);
C_ASSERT((ULONG)STATUS_OBJECT_NAME_NOT_FOUND==MT2_ACPI_NAME_NOT_FOUND);
static const uint32_t ring_counts[6]={64,16,16,32,64,32};
static const uint32_t slot_bytes[6]={MT2_SLOT_SIZE,8192,8192,8192,8192,8192};
static int span32(uint64_t address,uint32_t size) {
    return size && address<=0xffffffffULL && size-1<=0xffffffffULL-address;
}
static int overlap(uint64_t a,uint32_t as,uint64_t b,uint32_t bs) {
    return a<=b?b-a<as:a-b<bs;
}
static int read_register(void *context,uint32_t offset,uint32_t *value) {
    mt2_windows_platform *p=(mt2_windows_platform *)context;
    if(p->boot_deadline_ms && KeQueryInterruptTime()/10000u>=p->boot_deadline_ms)return MT2_FW_TIMEOUT;
    if(!p->prepared || !p->powered || p->removed || !p->bar || !value ||
       (offset&3u) || offset>p->bar_bytes-4)return MT2_INVALID;
    *value=READ_REGISTER_ULONG((PULONG)(p->bar+offset));return 0;
}
static int write_register(void *context,uint32_t offset,uint32_t value) {
    mt2_windows_platform *p=(mt2_windows_platform *)context;
    if(p->boot_deadline_ms && KeQueryInterruptTime()/10000u>=p->boot_deadline_ms)return MT2_FW_TIMEOUT;
    if(!p->prepared || !p->powered || !p->writes_enabled || p->removed || !p->bar ||
       (offset&3u) || offset>p->bar_bytes-4)return MT2_INVALID;
    WRITE_REGISTER_ULONG((PULONG)(p->bar+offset),value);return 0;
}
static void barrier(void *context){UNREFERENCED_PARAMETER(context);KeMemoryBarrier();}
static uint64_t milliseconds(void *context){UNREFERENCED_PARAMETER(context);return KeQueryInterruptTime()/10000u;}
static int delay_ms(void *context,uint32_t ms) {
    LARGE_INTEGER delay;NTSTATUS s;mt2_windows_platform *p=(mt2_windows_platform *)context;
    if(p->removed || KeGetCurrentIrql()!=PASSIVE_LEVEL || ms>1000)return MT2_INVALID;
    delay.QuadPart=-(LONGLONG)ms*10000;s=KeDelayExecutionThread(KernelMode,FALSE,&delay);
    return NT_SUCCESS(s)?0:MT2_PROTOCOL;
}
static int tx_publish(void *context,uint32_t index) {
    mt2_windows_platform *p=(mt2_windows_platform *)context;
    return mt2_reg_write(&p->chip.io,0xd4308,index);
}
static NTSTATUS identity(WDFDEVICE device,PCI_COMMON_CONFIG *pci) {
    BUS_INTERFACE_STANDARD bus;NTSTATUS s;ULONG bytes;
    RtlZeroMemory(&bus,sizeof(bus));RtlZeroMemory(pci,sizeof(*pci));
    s=WdfFdoQueryForInterface(device,&GUID_BUS_INTERFACE_STANDARD,(PINTERFACE)&bus,sizeof(bus),1,NULL);
    if(!NT_SUCCESS(s))return s;
    if(!bus.InterfaceDereference)return STATUS_INVALID_DEVICE_STATE;
    if(!bus.GetBusData){bus.InterfaceDereference(bus.Context);return STATUS_INVALID_DEVICE_STATE;}
    bytes=bus.GetBusData(bus.Context,PCI_WHICHSPACE_CONFIG,pci,0,PCI_COMMON_HDR_LENGTH);
    bus.InterfaceDereference(bus.Context);
    if(bytes!=PCI_COMMON_HDR_LENGTH)return STATUS_DEVICE_DATA_ERROR;
    if(pci->VendorID!=0x14c3 || pci->DeviceID!=0x0616 || pci->RevisionID!=2 ||
       (pci->HeaderType&0x7f)!=PCI_DEVICE_TYPE || pci->u.type0.SubVendorID!=0x14c3 ||
       pci->u.type0.SubSystemID!=0x223c)return STATUS_NOT_SUPPORTED;
    return STATUS_SUCCESS;
}
static NTSTATUS allocate(mt2_windows_platform *p,mt2_windows_block *b,uint32_t bytes) {
    NTSTATUS s=WdfCommonBufferCreate(p->dma,bytes,WDF_NO_OBJECT_ATTRIBUTES,&b->handle);
    if(!NT_SUCCESS(s))return s;
    b->virtual_address=WdfCommonBufferGetAlignedVirtualAddress(b->handle);
    b->address=(uint64_t)WdfCommonBufferGetAlignedLogicalAddress(b->handle).QuadPart;b->bytes=bytes;
    if(!b->virtual_address || !span32(b->address,bytes) || (b->address&15u))return STATUS_CONFLICTING_ADDRESSES;
    return STATUS_SUCCESS;
}
void mt2_windows_release(mt2_windows_platform *p) {
    uint32_t i;
    p->writes_enabled=FALSE;p->powered=FALSE;p->prepared=FALSE;
    mt2_radio_session_stop(&p->session);mt2_pcie_disarm(&p->mcu);mt2_tx_disarm(&p->tx);
    if(p->bridge_memory){WdfObjectDelete(p->bridge_memory);p->bridge_memory=NULL;}
    for(i=0;i<3;++i)if(p->assembly_memory[i]){WdfObjectDelete(p->assembly_memory[i]);p->assembly_memory[i]=NULL;}
    for(i=0;i<12;++i)if(p->blocks[i].handle){WdfObjectDelete(p->blocks[i].handle);p->blocks[i].handle=NULL;}
    if(p->dma){WdfObjectDelete(p->dma);p->dma=NULL;}
    if(p->bar){MmUnmapIoSpace(p->bar,p->bar_bytes);p->bar=NULL;}
    RtlZeroMemory(p,sizeof(*p)); /* A later PrepareHardware starts a fresh generation. */
}
NTSTATUS mt2_windows_prepare(mt2_windows_platform *p,WDFDEVICE device,WDFCMRESLIST raw,WDFCMRESLIST translated) {
    PCI_COMMON_CONFIG pci;NTSTATUS s;ULONGLONG bar0;ULONG low,i,j;
    PCM_PARTIAL_RESOURCE_DESCRIPTOR r,t,selected=NULL;WDF_DMA_ENABLER_CONFIG dma;
    WDF_OBJECT_ATTRIBUTES attributes;mt2_tx_io txio;
    if(KeGetCurrentIrql()!=PASSIVE_LEVEL || !p || p->bar || p->dma || p->boot_attempted)return STATUS_INVALID_DEVICE_STATE;
    s=identity(device,&pci);if(!NT_SUCCESS(s))return s;
    low=pci.u.type0.BaseAddresses[0];
    if((low&PCI_ADDRESS_IO_SPACE) || (low&6u)==2u || (low&6u)==6u)return STATUS_NOT_SUPPORTED;
    bar0=low&0xfffffff0u;if((low&6u)==4u)bar0|=(ULONGLONG)pci.u.type0.BaseAddresses[1]<<32;
    if(!bar0 || WdfCmResourceListGetCount(raw)!=WdfCmResourceListGetCount(translated))return STATUS_DEVICE_CONFIGURATION_ERROR;
    for(i=0;i<WdfCmResourceListGetCount(raw);++i) {
        r=WdfCmResourceListGetDescriptor(raw,i);t=WdfCmResourceListGetDescriptor(translated,i);
        if(!r || !t)return STATUS_DEVICE_CONFIGURATION_ERROR;
        if(r->Type!=CmResourceTypeMemory || t->Type!=CmResourceTypeMemory || (ULONGLONG)r->u.Memory.Start.QuadPart!=bar0)continue;
        if(selected || t->u.Memory.Length<0x100000 || (t->Flags&CM_RESOURCE_MEMORY_READ_ONLY))return STATUS_DEVICE_CONFIGURATION_ERROR;
        selected=t;
    }
    if(!selected)return STATUS_DEVICE_CONFIGURATION_ERROR;
    p->device=device;p->bar_bytes=0x100000;
    p->bar=MmMapIoSpace(selected->u.Memory.Start,p->bar_bytes,MmNonCached);
    if(!p->bar)return STATUS_INSUFFICIENT_RESOURCES;
    WdfDeviceSetAlignmentRequirement(device,15);
    WDF_DMA_ENABLER_CONFIG_INIT(&dma,WdfDmaProfileScatterGather,8192);
    s=WdfDmaEnablerCreate(device,&dma,WDF_NO_OBJECT_ATTRIBUTES,&p->dma);if(!NT_SUCCESS(s))goto fail;
    for(i=0;i<6;++i) {
        s=allocate(p,&p->blocks[2*i],ring_counts[i]*16);if(!NT_SUCCESS(s))goto fail;
        s=allocate(p,&p->blocks[2*i+1],ring_counts[i]*slot_bytes[i]);if(!NT_SUCCESS(s))goto fail;
    }
    /* Includes the independent raw-TX pool, before zeroing or publishing any
     * descriptor. All DMA buffers must be disjoint virtually and logically. */
    for(i=0;i<12;++i) {
        const mt2_windows_block *a=&p->blocks[i];
        if(overlap((uintptr_t)a->virtual_address,a->bytes,(uintptr_t)p,sizeof(*p))){s=STATUS_CONFLICTING_ADDRESSES;goto fail;}
        for(j=0;j<i;++j) {
            const mt2_windows_block *b=&p->blocks[j];
            if(overlap(a->address,a->bytes,b->address,b->bytes) ||
               overlap((uintptr_t)a->virtual_address,a->bytes,(uintptr_t)b->virtual_address,b->bytes)) {
                s=STATUS_CONFLICTING_ADDRESSES;goto fail;
            }
        }
    }
    for(i=0;i<12;++i)RtlZeroMemory(p->blocks[i].virtual_address,p->blocks[i].bytes);
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);attributes.ParentObject=device;
    for(i=0;i<3;++i) {
        PVOID buffer;
        s=WdfMemoryCreate(&attributes,NonPagedPoolNx,'22tM',MT2_RX_RECORD_MAX,&p->assembly_memory[i],&buffer);
        if(!NT_SUCCESS(s))goto fail;
        p->banks[i+2].assembly=(uint8_t *)buffer;p->banks[i+2].capacity=MT2_RX_RECORD_MAX;
    }
    {
        PVOID buffer;
        s=WdfMemoryCreate(&attributes,NonPagedPoolNx,'22tM',MT2_WINDOWS_RX_SLOTS*MT2_RX_MESSAGE_MAX,&p->bridge_memory,&buffer);
        if(!NT_SUCCESS(s))goto fail;
        p->core_error=mt2_rx_bridge_prepare(&p->receive_bridge,(uint8_t *)buffer,
            MT2_WINDOWS_RX_SLOTS*MT2_RX_MESSAGE_MAX,p->receive_lengths,MT2_WINDOWS_RX_SLOTS);
        if(p->core_error){s=STATUS_INVALID_PARAMETER;goto fail;}
    }
    for(i=0;i<6;++i) {
        p->bindings[i].address=p->blocks[2*i].address;p->bindings[i].count=ring_counts[i];
        if(i) {
            mt2_dma_memory *b=&p->banks[i-1].memory;
            b->descriptors=(mt2_descriptor *)p->blocks[2*i].virtual_address;b->descriptor_address=p->blocks[2*i].address;
            b->pool=(uint8_t *)p->blocks[2*i+1].virtual_address;b->pool_address=p->blocks[2*i+1].address;
            b->pool_bytes=p->blocks[2*i+1].bytes;b->count=ring_counts[i];b->slot_bytes=slot_bytes[i];
        }
    }
    txio.context=p;txio.barrier=barrier;txio.publish=tx_publish;
    p->core_error=mt2_tx_prepare(&p->tx,(mt2_descriptor *)p->blocks[0].virtual_address,p->blocks[0].address,64,
        (uint8_t *)p->blocks[1].virtual_address,p->blocks[1].address,p->blocks[1].bytes,p->tx_states,64,p->tx_slots,&txio);
    if(!p->core_error)p->core_error=mt2_pcie_prepare(&p->mcu,&p->chip,p->banks);
    if(!p->core_error)p->core_error=mt2_pcie_bind_tx(&p->mcu,&p->tx);
    if(!p->core_error)p->core_error=mt2_radio_session_prepare(&p->session,&p->mcu,&p->mac,&p->tx,
        &p->caps,&p->platform_data,&p->receive_bridge);
    if(p->core_error){s=STATUS_INVALID_PARAMETER;goto fail;}
    p->prepared=TRUE;return STATUS_SUCCESS;
fail:
    mt2_windows_release(p);return s;
}
NTSTATUS mt2_windows_enter(mt2_windows_platform *p) {
    if(!p || !p->prepared || p->removed || p->stop_error || KeGetCurrentIrql()!=PASSIVE_LEVEL)return STATUS_INVALID_DEVICE_STATE;
    p->powered=TRUE;return STATUS_SUCCESS; /* No automatic radio/firmware start. */
}
void mt2_windows_revoke(mt2_windows_platform *p){InterlockedExchange(&p->removed,1);}
NTSTATUS mt2_windows_stop(mt2_windows_platform *p) {
    if(!p || KeGetCurrentIrql()!=PASSIVE_LEVEL)return STATUS_INVALID_DEVICE_STATE;
    p->boot_deadline_ms=0;
    if(!p->boot_attempted)return STATUS_SUCCESS; /* Status-only handles do not disarm preparation. */
    mt2_radio_session_stop(&p->session);mt2_pcie_disarm(&p->mcu);mt2_tx_disarm(&p->tx);p->caps_valid=0;p->firmware.ready=0;
    p->mac.initialized=0;
    if(p->chip.attempted && !p->removed && p->powered) {
        p->writes_enabled=TRUE;p->stop_error=mt2_chip_stop(&p->chip);
    } else if(p->chip.published && !p->chip.quiescent)p->stop_error=MT2_PROTOCOL;
    p->writes_enabled=FALSE;
    return p->stop_error?STATUS_DEVICE_HARDWARE_ERROR:STATUS_SUCCESS; /* Retain all buffers. */
}
NTSTATUS mt2_windows_leave(mt2_windows_platform *p) {
    NTSTATUS s=mt2_windows_stop(p);
    if(p)p->powered=FALSE;
    return s;
}
static NTSTATUS verify_image(const uint8_t *data,uint32_t bytes,const uint8_t expected[32]) {
    BCRYPT_ALG_HANDLE algorithm=NULL;UCHAR digest[32];NTSTATUS s;ULONG i,diff=0;
    if(!data || !bytes || bytes>MT2_FW_MAX_SIZE)return STATUS_INVALID_PARAMETER;
    s=BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,NULL,0);
    if(!NT_SUCCESS(s))return s;
    s=BCryptHash(algorithm,NULL,0,(PUCHAR)data,bytes,digest,sizeof(digest));
    BCryptCloseAlgorithmProvider(algorithm,0);
    if(!NT_SUCCESS(s))return s;
    for(i=0;i<32;++i)diff|=digest[i]^expected[i];
    return diff?STATUS_IMAGE_CHECKSUM_MISMATCH:STATUS_SUCCESS;
}
/* Fixed table-return methods on this adapter's lower device stack. No generic
 * ACPI IOCTL, external table input, namespace search or retry is exposed.
 * ACPI evaluates OEM AML; this is not a claim that arbitrary AML is side-effect
 * free. WDF waits for completion/cancellation before stack input goes away. */
static int32_t evaluate_acpi(void *context,const uint8_t name[4],uint8_t *output,uint32_t capacity,uint32_t *size) {
    mt2_windows_platform *p=(mt2_windows_platform *)context;
    ACPI_EVAL_INPUT_BUFFER_V1 input;WDF_MEMORY_DESCRIPTOR in_desc,out_desc;
    WDF_REQUEST_SEND_OPTIONS options;ULONG_PTR received=0;NTSTATUS s;uint64_t now,timeout;
    *size=0;
    if(!p->prepared || !p->powered || p->removed || !p->boot_attempted || p->chip.attempted ||
       KeGetCurrentIrql()!=PASSIVE_LEVEL || capacity!=MT2_ACPI_WIRE_BYTES)return STATUS_INVALID_DEVICE_STATE;
    now=milliseconds(p);
    if(!p->boot_deadline_ms || now>=p->boot_deadline_ms)return STATUS_IO_TIMEOUT;
    timeout=p->boot_deadline_ms-now;if(timeout>2000)timeout=2000;
    RtlZeroMemory(&input,sizeof(input));input.Signature=ACPI_EVAL_INPUT_BUFFER_SIGNATURE_V1;
    RtlCopyMemory(input.MethodName,name,4);
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&in_desc,&input,sizeof(input));
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&out_desc,output,capacity);
    WDF_REQUEST_SEND_OPTIONS_INIT(&options,WDF_REQUEST_SEND_OPTION_TIMEOUT);
    WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(&options,WDF_REL_TIMEOUT_IN_MS((LONGLONG)timeout));
    s=WdfIoTargetSendIoctlSynchronously(WdfDeviceGetIoTarget(p->device),NULL,
        IOCTL_ACPI_EVAL_METHOD_V1,&in_desc,&out_desc,&options,&received);
    if(p->removed)return STATUS_DEVICE_REMOVED;
    if(received>capacity)return STATUS_DEVICE_DATA_ERROR;
    *size=(uint32_t)received;return s;
}
static NTSTATUS boot_common(mt2_windows_platform *p,const uint8_t *patch,uint32_t ps,const uint8_t *ram,uint32_t rs,const mt2_radio_options *options) {
    static const uint8_t patch_sha[32]={0x6c,0x22,0xdc,0x4b,0xdf,0x97,0xc0,0xff,0x67,0x88,0x41,0xf1,0x83,0xe5,0x43,0xb1,0x4d,0x72,0x44,0xd8,0x34,0x34,0x3a,0x1c,0xb1,0x5d,0x1f,0x09,0xfa,0x17,0x8a,0x72};
    static const uint8_t ram_sha[32]={0x3e,0x3a,0x04,0xe3,0x30,0x71,0x37,0x06,0xfe,0x11,0x14,0x56,0xcc,0x80,0x7b,0x6c,0x39,0x8f,0xa7,0x50,0xca,0x18,0xf7,0x07,0xe0,0xeb,0x3e,0x68,0x2c,0xe0,0xcf,0xed};
    static const uint8_t radio_patch_sha[32]={0x7f,0xc9,0x07,0x5d,0x6d,0x54,0xb3,0x1e,0x05,0x39,0x73,0x46,0x14,0xca,0x97,0x1f,0xa5,0x2c,0xaf,0x41,0xb6,0x42,0xa3,0x00,0x0c,0x8a,0x45,0x01,0x59,0x1a,0x4d,0x98};
    static const uint8_t radio_ram_sha[32]={0xd1,0x00,0xe5,0xe0,0x4c,0x32,0x4f,0xc8,0xda,0x63,0x02,0x11,0x2d,0x1e,0x0b,0x4f,0xac,0x95,0x22,0x86,0x44,0x7d,0x24,0x19,0xea,0xbc,0xd8,0xfb,0x0a,0x1f,0x5b,0x93};
    mt2_register_io io;mt2_acpi_io acpi;NTSTATUS s;mt2_fw_plan plan;
    if(!p || !p->prepared || !p->powered || p->removed || p->boot_attempted || KeGetCurrentIrql()!=PASSIVE_LEVEL)return STATUS_INVALID_DEVICE_STATE;
    if(options && !mt2_radio_options_valid(options))return STATUS_INVALID_PARAMETER;
    s=verify_image(patch,ps,options?radio_patch_sha:patch_sha);if(!NT_SUCCESS(s))return s;
    s=verify_image(ram,rs,options?radio_ram_sha:ram_sha);if(!NT_SUCCESS(s))return s;
    if(mt2_fw_validate(patch,ps,MT2_FW_PATCH,&plan) || mt2_fw_validate(ram,rs,MT2_FW_RAM,&plan))return STATUS_INVALID_IMAGE_FORMAT;
    if(options && mt2_regd_request(ram,rs,options->alpha2,p->session_scratch,sizeof(p->session_scratch))<0)return STATUS_INVALID_PARAMETER;
    p->boot_attempted=TRUE;p->firmware_pair=options?1u:0u;p->writes_enabled=FALSE;
    p->boot_deadline_ms=milliseconds(p)+(options?60000u:30000u);
    acpi.context=p;acpi.evaluate=evaluate_acpi;
    p->core_error=mt2_platform_read(&acpi,p->acpi_wire,sizeof(p->acpi_wire),&p->platform_data);
    if(p->core_error) {
        s=STATUS_DEVICE_DATA_ERROR;
        if(p->platform_data.failed_index<4 && !NT_SUCCESS((NTSTATUS)p->platform_data.tables[p->platform_data.failed_index].ntstatus))
            s=(NTSTATUS)p->platform_data.tables[p->platform_data.failed_index].ntstatus;
        (void)mt2_windows_stop(p);return s;
    }
    p->writes_enabled=TRUE;
    io.context=p;io.bar_bytes=p->bar_bytes;io.read=read_register;io.write=write_register;
    io.barrier=barrier;io.milliseconds=milliseconds;io.delay_ms=delay_ms;
    p->core_error=mt2_chip_start(&p->chip,&io,p->bindings);
    if(!p->core_error)p->core_error=mt2_pcie_activate(&p->mcu,&p->transport);
    if(!p->core_error)p->core_error=mt2_fw_boot(patch,ps,ram,rs,&p->transport,&p->firmware);
    if(!p->core_error)p->core_error=mt2_pcie_query_nic(&p->mcu,&p->caps);
    if(!p->core_error)p->core_error=mt2_mac_initialize(&p->mcu,&p->mac);
    if(!p->core_error){
        p->caps_valid=1;
        if(options)p->core_error=mt2_radio_session_start(&p->session,ram,rs,options,p->session_scratch,sizeof(p->session_scratch));
    }
    p->boot_deadline_ms=0;
    if(!p->core_error)return STATUS_SUCCESS;
    s=p->core_error==MT2_FW_TIMEOUT?STATUS_IO_TIMEOUT:STATUS_DEVICE_DATA_ERROR;
    (void)mt2_windows_stop(p);return s;
}

NTSTATUS mt2_windows_boot(mt2_windows_platform *p,const uint8_t *patch,uint32_t ps,const uint8_t *ram,uint32_t rs) {
    return boot_common(p,patch,ps,ram,rs,NULL);
}
NTSTATUS mt2_windows_radio_start(mt2_windows_platform *p,const uint8_t *patch,uint32_t ps,const uint8_t *ram,uint32_t rs,const mt2_radio_options *options) {
    if(!options)return STATUS_INVALID_PARAMETER;
    return boot_common(p,patch,ps,ram,rs,options);
}
