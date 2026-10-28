/* SPDX-License-Identifier: BSD-3-Clause-Clear
 * Explicit admin-only MT7922 diagnostic and radio-session interfaces.
 * Nothing starts from DriverEntry, AddDevice or a status request.
 */
#include "windows_platform.h"
#include "device_abi.h"
#include "radio_abi.h"
#include <initguid.h>
#include <wdmguid.h>
typedef struct mt2_device_context {
    mt2_windows_platform platform;
    WDFWAITLOCK lock;
    NTSTATUS last_status;
    uint32_t generation;
} mt2_device_context;
typedef struct mt2_file_context {BOOLEAN boot_owner;} mt2_file_context;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(mt2_device_context,DeviceContext)
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(mt2_file_context,FileContext)
DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD Mt2AddDevice;
EVT_WDF_DEVICE_PREPARE_HARDWARE Mt2Prepare;
EVT_WDF_DEVICE_RELEASE_HARDWARE Mt2Release;
EVT_WDF_DEVICE_D0_ENTRY Mt2D0Entry;
EVT_WDF_DEVICE_D0_EXIT Mt2D0Exit;
EVT_WDF_DEVICE_SURPRISE_REMOVAL Mt2Removed;
EVT_WDF_DEVICE_FILE_CREATE Mt2Create;
EVT_WDF_FILE_CLEANUP Mt2Cleanup;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL Mt2Control;
C_ASSERT(MT2_IOCTL_BOOT==CTL_CODE(FILE_DEVICE_UNKNOWN,0x901,METHOD_BUFFERED,FILE_READ_ACCESS|FILE_WRITE_ACCESS));
C_ASSERT(MT2_IOCTL_RADIO_START==CTL_CODE(FILE_DEVICE_UNKNOWN,0x903,METHOD_BUFFERED,FILE_READ_ACCESS|FILE_WRITE_ACCESS));
C_ASSERT(MT2_IOCTL_RADIO_RECEIVE==CTL_CODE(FILE_DEVICE_UNKNOWN,0x905,METHOD_BUFFERED,FILE_READ_ACCESS));
static void put32(uint8_t *p,uint32_t x){p[0]=(uint8_t)x;p[1]=(uint8_t)(x>>8);p[2]=(uint8_t)(x>>16);p[3]=(uint8_t)(x>>24);}
static void status_bytes(mt2_device_context *c,uint8_t *out) {
    mt2_windows_platform *p=&c->platform;uint32_t f=0;
    if(p->prepared)f|=MT2_STATUS_PREPARED;if(p->powered)f|=MT2_STATUS_POWERED;
    if(p->removed)f|=MT2_STATUS_REMOVED;if(p->boot_attempted)f|=MT2_STATUS_BOOT_ATTEMPTED;
    if(p->firmware.ready)f|=MT2_STATUS_FIRMWARE_READY;if(p->caps_valid)f|=MT2_STATUS_CAPS_VALID;
    if(p->chip.published)f|=MT2_STATUS_DMA_PUBLISHED;if(p->chip.quiescent)f|=MT2_STATUS_DMA_QUIESCENT;
    RtlZeroMemory(out,MT2_STATUS_BYTES);put32(out,MT2_STATUS_MAGIC);put32(out+4,MT2_ABI_VERSION);
    put32(out+8,MT2_STATUS_BYTES);put32(out+12,f);put32(out+16,(uint32_t)c->last_status);
    put32(out+20,(uint32_t)p->core_error);put32(out+24,(uint32_t)p->stop_error);put32(out+28,c->generation);
    put32(out+32,p->firmware.bytes_sent);put32(out+36,p->firmware.regions_sent);
    put32(out+40,(uint32_t)p->mcu.fault);put32(out+44,(uint32_t)p->chip.start_error);put32(out+48,(uint32_t)p->chip.stop_error);
    if(p->caps_valid) {
        put32(out+56,p->caps.present);put32(out+60,p->caps.bands);put32(out+64,p->caps.nss);put32(out+68,p->caps.chainmask);
        put32(out+72,p->caps.chip_low);put32(out+76,p->caps.chip_high);RtlCopyMemory(out+80,p->caps.mac,6);
    }
    if(p->prepared){put32(out+88,0x061614c3);put32(out+92,0x223c14c3);put32(out+96,2);}
    /* Bytes 100 and 104 are radio_ready and airdrop_ready: always zero here. */
}
NTSTATUS Mt2Prepare(WDFDEVICE device,WDFCMRESLIST raw,WDFCMRESLIST translated) {
    mt2_device_context *c=DeviceContext(device);NTSTATUS s;
    WdfWaitLockAcquire(c->lock,NULL);s=mt2_windows_prepare(&c->platform,device,raw,translated);
    c->last_status=s;if(NT_SUCCESS(s))++c->generation;WdfWaitLockRelease(c->lock);return s;
}
NTSTATUS Mt2Release(WDFDEVICE device,WDFCMRESLIST translated) {
    mt2_device_context *c=DeviceContext(device);UNREFERENCED_PARAMETER(translated);
    WdfWaitLockAcquire(c->lock,NULL);mt2_windows_release(&c->platform);WdfWaitLockRelease(c->lock);return STATUS_SUCCESS;
}
NTSTATUS Mt2D0Entry(WDFDEVICE device,WDF_POWER_DEVICE_STATE previous) {
    mt2_device_context *c=DeviceContext(device);NTSTATUS s;UNREFERENCED_PARAMETER(previous);
    WdfWaitLockAcquire(c->lock,NULL);s=mt2_windows_enter(&c->platform);c->last_status=s;WdfWaitLockRelease(c->lock);return s;
}
NTSTATUS Mt2D0Exit(WDFDEVICE device,WDF_POWER_DEVICE_STATE target) {
    mt2_device_context *c=DeviceContext(device);NTSTATUS s;UNREFERENCED_PARAMETER(target);
    WdfWaitLockAcquire(c->lock,NULL);s=mt2_windows_leave(&c->platform);c->last_status=s;WdfWaitLockRelease(c->lock);return s;
}
VOID Mt2Removed(WDFDEVICE device) {
    mt2_device_context *c=DeviceContext(device);
    mt2_windows_revoke(&c->platform); /* Revoke first, even while a boot owns the lock. */
    WdfWaitLockAcquire(c->lock,NULL);(void)mt2_windows_stop(&c->platform);WdfWaitLockRelease(c->lock);
}
VOID Mt2Create(WDFDEVICE device,WDFREQUEST request,WDFFILEOBJECT file) {
    PCUNICODE_STRING name=WdfFileObjectGetFileName(file);UNREFERENCED_PARAMETER(device);
    WdfRequestComplete(request,name && name->Length?STATUS_OBJECT_NAME_INVALID:STATUS_SUCCESS);
}
VOID Mt2Cleanup(WDFFILEOBJECT file) {
    mt2_device_context *c=DeviceContext(WdfFileObjectGetDevice(file));
    if(!FileContext(file)->boot_owner)return;
    WdfWaitLockAcquire(c->lock,NULL);c->last_status=mt2_windows_stop(&c->platform);WdfWaitLockRelease(c->lock);
}
static void put64(uint8_t *p,uint64_t x){put32(p,(uint32_t)x);put32(p+4,(uint32_t)(x>>32));}
static void radio_status_bytes(mt2_device_context *c,uint8_t *out,uint32_t token,uint32_t token_valid) {
    mt2_windows_platform *p=&c->platform;mt2_radio_session *s=&p->session;mt2_rx_bridge *rx=&p->receive_bridge;
    uint32_t flags=0,i;
    if(s->prepared)flags|=1;if(s->attempted)flags|=2;
    if(s->ready && !s->stopped && p->mcu.active && rx->active)flags|=4;
    if(s->stopped)flags|=8;if(p->tx.active)flags|=16;if(rx->active)flags|=32;
    if(p->firmware.ready)flags|=64;if(p->chip.quiescent)flags|=128;if(p->removed)flags|=256;
    if(p->chip.published)flags|=512;if(p->boot_attempted)flags|=1024;
    RtlZeroMemory(out,256);put32(out,MT2_RADIO_STATUS_MAGIC);put32(out+4,1);put32(out+8,256);put32(out+12,flags);
    put32(out+16,s->stage);put32(out+20,(uint32_t)p->core_error);put32(out+24,(uint32_t)p->stop_error);
    put32(out+28,(uint32_t)c->last_status);put32(out+32,c->generation);
    put32(out+36,s->channel.primary);put32(out+40,s->channel.center);put32(out+44,s->channel.width);
    out[48]=s->options.alpha2[0];out[49]=s->options.alpha2[1];RtlCopyMemory(out+50,s->channel.context_address,6);
    put32(out+56,(uint32_t)s->options.cap_half_dbm);put32(out+60,s->options.fcs_present);
    put32(out+64,s->options.environment);put32(out+68,s->options.power_type);put32(out+72,p->firmware_pair);
    put32(out+76,p->tx.queued);put32(out+80,p->tx.inflight);put32(out+84,rx->queued);
    put64(out+88,rx->accepted);put64(out+96,rx->taken);put64(out+104,rx->overflow);put64(out+112,rx->inactive);
    for(i=0;i<9;++i)put64(out+120+i*8,rx->discarded[i]);
    put64(out+192,p->mcu.ignored_records);put64(out+200,p->mcu.matched_responses);
    if(token_valid){put32(out+208,token);put32(out+212,1);}
    /* 216..255 reserved: never infer iOS reception/AirDrop readiness. */
}
static NTSTATUS radio_error(int e) {
    if(!e)return STATUS_SUCCESS;
    if(e==MT2_INVALID)return STATUS_INVALID_PARAMETER;
    if(e==MT2_FULL || e==MT2_PENDING)return STATUS_DEVICE_BUSY;
    if(e==MT2_FW_TIMEOUT)return STATUS_IO_TIMEOUT;
    return STATUS_DEVICE_DATA_ERROR;
}
static VOID radio_control(mt2_device_context *c,WDFREQUEST request,size_t input_size,ULONG code) {
    mt2_windows_platform *p=&c->platform;mt2_radio_view view;uint8_t *in=NULL,*out;size_t actual;
    WDFFILEOBJECT file;NTSTATUS status;uint32_t bytes=0,token=0,token_valid=0;int e=0;
    if(input_size>MT2_RADIO_START_BYTES+2*MT2_FW_MAX_SIZE){WdfRequestComplete(request,STATUS_INVALID_PARAMETER);return;}
    if(input_size) {
        status=WdfRequestRetrieveInputBuffer(request,input_size,(PVOID *)&in,&actual);
        if(!NT_SUCCESS(status)){WdfRequestComplete(request,status);return;}
        if(actual!=input_size){WdfRequestComplete(request,STATUS_INVALID_PARAMETER);return;}
    }
    if(mt2_radio_request(code,in,(uint32_t)input_size,&view)){WdfRequestComplete(request,STATUS_INVALID_PARAMETER);return;}
    status=WdfRequestRetrieveOutputBuffer(request,code==MT2_IOCTL_RADIO_RECEIVE?MT2_RX_MESSAGE_MAX:MT2_RADIO_STATUS_BYTES,(PVOID *)&out,NULL);
    if(!NT_SUCCESS(status)){WdfRequestComplete(request,status);return;}
    file=WdfRequestGetFileObject(request);if(!file){WdfRequestComplete(request,STATUS_INVALID_HANDLE);return;}
    WdfWaitLockAcquire(c->lock,NULL);
    if(code==MT2_IOCTL_RADIO_START) {
        c->last_status=mt2_windows_radio_start(p,view.firmware.patch,view.firmware.patch_bytes,view.firmware.ram,view.firmware.ram_bytes,&view.options);
        if(p->boot_attempted)FileContext(file)->boot_owner=TRUE;
    } else if(code!=MT2_IOCTL_RADIO_STATUS) {
        if(!FileContext(file)->boot_owner){WdfWaitLockRelease(c->lock);WdfRequestComplete(request,STATUS_INVALID_DEVICE_STATE);return;}
        if(code==MT2_IOCTL_RADIO_SERVICE)e=mt2_radio_session_poll(&p->session);
        else if(code==MT2_IOCTL_RADIO_RECEIVE)e=mt2_radio_session_receive(&p->session,out,MT2_RX_MESSAGE_MAX,&bytes);
        else if(code==MT2_IOCTL_RADIO_TRANSMIT){e=mt2_radio_session_transmit_rate(&p->session,view.frame,view.frame_bytes,view.rate,view.no_ack,&token);token_valid=!e;}
        else if(code==MT2_IOCTL_RADIO_CHANNEL)e=mt2_radio_session_channel(&p->session,view.primary,view.center,view.width);
        else if(code==MT2_IOCTL_RADIO_ADDRESS)e=mt2_radio_session_address(&p->session,view.address);
        p->core_error=e;c->last_status=radio_error(e);
        if(p->session.stopped)(void)mt2_windows_stop(p); /* Preserve operation and chip-stop errors separately. */
    }
    if(code==MT2_IOCTL_RADIO_RECEIVE) {
        status=e==MT2_PENDING?STATUS_NO_MORE_ENTRIES:radio_error(e);
        WdfWaitLockRelease(c->lock);WdfRequestCompleteWithInformation(request,status,NT_SUCCESS(status)?bytes:0);return;
    }
    radio_status_bytes(c,out,token,token_valid);WdfWaitLockRelease(c->lock);
    WdfRequestCompleteWithInformation(request,STATUS_SUCCESS,MT2_RADIO_STATUS_BYTES);
}
VOID Mt2Control(WDFQUEUE queue,WDFREQUEST request,size_t output_size,size_t input_size,ULONG code) {
    mt2_device_context *c=DeviceContext(WdfIoQueueGetDevice(queue));
    uint8_t *out,*in;mt2_boot_view view={0};NTSTATUS s;WDFFILEOBJECT file;size_t actual;
    UNREFERENCED_PARAMETER(output_size);
    if(mt2_radio_ioctl(code)){radio_control(c,request,input_size,code);return;}
    if(code!=MT2_IOCTL_STATUS && code!=MT2_IOCTL_BOOT && code!=MT2_IOCTL_STOP) {
        WdfRequestComplete(request,STATUS_INVALID_DEVICE_REQUEST);return;
    }
    if((code!=MT2_IOCTL_BOOT && input_size) || input_size>MT2_BOOT_HEADER+2*MT2_FW_MAX_SIZE) {
        WdfRequestComplete(request,STATUS_INVALID_PARAMETER);return;
    }
    s=WdfRequestRetrieveOutputBuffer(request,MT2_STATUS_BYTES,(PVOID *)&out,NULL);
    if(!NT_SUCCESS(s)){WdfRequestComplete(request,s);return;}
    file=WdfRequestGetFileObject(request);
    if(!file){WdfRequestComplete(request,STATUS_INVALID_HANDLE);return;}
    if(code==MT2_IOCTL_BOOT) {
        s=WdfRequestRetrieveInputBuffer(request,MT2_BOOT_HEADER,(PVOID *)&in,&actual);
        if(!NT_SUCCESS(s)){WdfRequestComplete(request,s);return;}
        if(actual!=input_size || mt2_boot_request(in,(uint32_t)input_size,&view)) {
            WdfRequestComplete(request,STATUS_INVALID_PARAMETER);return;
        }
    }
    WdfWaitLockAcquire(c->lock,NULL);
    if(code==MT2_IOCTL_BOOT) {
        /* Buffered input/output share storage: consume immutable images fully
         * before serializing any response over that buffer. */
        c->last_status=mt2_windows_boot(&c->platform,view.patch,view.patch_bytes,view.ram,view.ram_bytes);
        if(c->platform.boot_attempted)FileContext(file)->boot_owner=TRUE;
    } else if(code==MT2_IOCTL_STOP)c->last_status=mt2_windows_stop(&c->platform);
    status_bytes(c,out);
    WdfWaitLockRelease(c->lock);
    /* Transport success means a complete status record; operation NTSTATUS is
     * inside it, so failed boot diagnostics are available without another IOCTL. */
    WdfRequestCompleteWithInformation(request,STATUS_SUCCESS,MT2_STATUS_BYTES);
}
NTSTATUS Mt2AddDevice(WDFDRIVER driver,PWDFDEVICE_INIT init) {
    WDFDEVICE device;WDF_OBJECT_ATTRIBUTES attributes,file_attributes;
    WDF_PNPPOWER_EVENT_CALLBACKS pnp;WDF_FILEOBJECT_CONFIG files;WDF_IO_QUEUE_CONFIG queue;
    NTSTATUS s;mt2_device_context *c;UNREFERENCED_PARAMETER(driver);
    DECLARE_CONST_UNICODE_STRING(name,L"\\Device\\AirWindowsMt7922Bringup");
    DECLARE_CONST_UNICODE_STRING(link,L"\\DosDevices\\AirWindowsMt7922Bringup");
    DECLARE_CONST_UNICODE_STRING(acl,L"D:P(A;;GA;;;SY)(A;;GA;;;BA)");
    WdfDeviceInitSetDeviceType(init,FILE_DEVICE_UNKNOWN);
    WdfDeviceInitSetCharacteristics(init,FILE_DEVICE_SECURE_OPEN,FALSE);
    WdfDeviceInitSetExclusive(init,TRUE);
    s=WdfDeviceInitAssignName(init,&name);if(!NT_SUCCESS(s))return s;
    s=WdfDeviceInitAssignSDDLString(init,&acl);if(!NT_SUCCESS(s))return s;
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
    pnp.EvtDevicePrepareHardware=Mt2Prepare;pnp.EvtDeviceReleaseHardware=Mt2Release;
    pnp.EvtDeviceD0Entry=Mt2D0Entry;pnp.EvtDeviceD0Exit=Mt2D0Exit;pnp.EvtDeviceSurpriseRemoval=Mt2Removed;
    WdfDeviceInitSetPnpPowerEventCallbacks(init,&pnp);
    WDF_FILEOBJECT_CONFIG_INIT(&files,Mt2Create,WDF_NO_EVENT_CALLBACK,Mt2Cleanup);
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&file_attributes,mt2_file_context);
    file_attributes.ExecutionLevel=WdfExecutionLevelPassive;
    WdfDeviceInitSetFileObjectConfig(init,&files,&file_attributes);
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes,mt2_device_context);
    attributes.ExecutionLevel=WdfExecutionLevelPassive;attributes.SynchronizationScope=WdfSynchronizationScopeNone;
    s=WdfDeviceCreate(&init,&attributes,&device);if(!NT_SUCCESS(s))return s;
    c=DeviceContext(device);WDF_OBJECT_ATTRIBUTES_INIT(&attributes);attributes.ParentObject=device;
    s=WdfWaitLockCreate(&attributes,&c->lock);if(!NT_SUCCESS(s))return s;
    s=WdfDeviceCreateSymbolicLink(device,&link);if(!NT_SUCCESS(s))return s;
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queue,WdfIoQueueDispatchSequential);
    queue.PowerManaged=WdfTrue;queue.EvtIoDeviceControl=Mt2Control;
    return WdfIoQueueCreate(device,&queue,WDF_NO_OBJECT_ATTRIBUTES,NULL);
}
NTSTATUS DriverEntry(PDRIVER_OBJECT driver,PUNICODE_STRING path) {
    WDF_DRIVER_CONFIG config;WDF_DRIVER_CONFIG_INIT(&config,Mt2AddDevice);
    return WdfDriverCreate(driver,path,WDF_NO_OBJECT_ATTRIBUTES,&config,WDF_NO_HANDLE);
}
