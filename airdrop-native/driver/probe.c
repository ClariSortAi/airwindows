/* Native MT7922 bring-up probe. No MMIO, DMA, firmware or radio writes.
 * This is not an AirDrop receiver and does not provide networking. */
#include <ntddk.h>
#include <wdf.h>
#include <initguid.h>
#include <wdmguid.h>

#define IOCTL_IDENTITY CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_READ_ACCESS)
typedef struct { ULONG version; USHORT vendor, device; USHORT subvendor, subsystem; ULONG ready; } IDENTITY;
typedef struct { IDENTITY identity; } MT7922_CONTEXT;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(MT7922_CONTEXT, Context)
DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD AddDevice;
EVT_WDF_DEVICE_PREPARE_HARDWARE Prepare;
EVT_WDF_DEVICE_RELEASE_HARDWARE Release;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL Control;

NTSTATUS Prepare(WDFDEVICE device, WDFCMRESLIST raw, WDFCMRESLIST translated) {
    BUS_INTERFACE_STANDARD bus;
    PCI_COMMON_CONFIG config;
    NTSTATUS status;
    ULONG bytes;
    MT7922_CONTEXT *context = Context(device);
    UNREFERENCED_PARAMETER(raw); UNREFERENCED_PARAMETER(translated);
    RtlZeroMemory(&context->identity, sizeof(context->identity));
    RtlZeroMemory(&bus, sizeof(bus));
    RtlZeroMemory(&config, sizeof(config));
    status = WdfFdoQueryForInterface(device, &GUID_BUS_INTERFACE_STANDARD,
        (PINTERFACE)&bus, sizeof(bus), 1, NULL);
    if (!NT_SUCCESS(status)) return status;
    bytes = bus.GetBusData(bus.Context, PCI_WHICHSPACE_CONFIG, &config, 0, PCI_COMMON_HDR_LENGTH);
    bus.InterfaceDereference(bus.Context);
    if (bytes != PCI_COMMON_HDR_LENGTH) return STATUS_DEVICE_DATA_ERROR;
    if (config.VendorID != 0x14c3 || config.DeviceID != 0x0616 ||
        (config.HeaderType & 0x7f) != PCI_DEVICE_TYPE ||
        config.u.type0.SubVendorID != 0x14c3 || config.u.type0.SubSystemID != 0x223c)
        return STATUS_NOT_SUPPORTED;
    context->identity.version = 1;
    context->identity.vendor = config.VendorID;
    context->identity.device = config.DeviceID;
    context->identity.subvendor = config.u.type0.SubVendorID;
    context->identity.subsystem = config.u.type0.SubSystemID;
    context->identity.ready = 1;
    return STATUS_SUCCESS;
}

NTSTATUS Release(WDFDEVICE device, WDFCMRESLIST translated) {
    UNREFERENCED_PARAMETER(translated);
    RtlZeroMemory(&Context(device)->identity, sizeof(IDENTITY));
    return STATUS_SUCCESS;
}

VOID Control(WDFQUEUE queue, WDFREQUEST request, size_t outSize, size_t inSize, ULONG code) {
    IDENTITY *out;
    NTSTATUS status;
    UNREFERENCED_PARAMETER(outSize);
    if (code != IOCTL_IDENTITY || inSize != 0) {
        WdfRequestComplete(request, STATUS_INVALID_DEVICE_REQUEST); return;
    }
    status = WdfRequestRetrieveOutputBuffer(request, sizeof(*out), (PVOID *)&out, NULL);
    if (NT_SUCCESS(status)) {
        *out = Context(WdfIoQueueGetDevice(queue))->identity;
        WdfRequestCompleteWithInformation(request, STATUS_SUCCESS, sizeof(*out));
    } else WdfRequestComplete(request, status);
}

NTSTATUS AddDevice(WDFDRIVER driver, PWDFDEVICE_INIT init) {
    WDFDEVICE device;
    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_PNPPOWER_EVENT_CALLBACKS pnp;
    WDF_IO_QUEUE_CONFIG queue;
    NTSTATUS status;
    DECLARE_CONST_UNICODE_STRING(name, L"\\Device\\AirWindowsMt7922Probe");
    DECLARE_CONST_UNICODE_STRING(link, L"\\DosDevices\\AirWindowsMt7922Probe");
    DECLARE_CONST_UNICODE_STRING(acl, L"D:P(A;;GA;;;SY)(A;;GA;;;BA)");
    UNREFERENCED_PARAMETER(driver);
    WdfDeviceInitSetDeviceType(init, FILE_DEVICE_UNKNOWN);
    WdfDeviceInitSetCharacteristics(init, FILE_DEVICE_SECURE_OPEN, FALSE);
    status = WdfDeviceInitAssignName(init, &name);
    if (!NT_SUCCESS(status)) return status;
    status = WdfDeviceInitAssignSDDLString(init, &acl);
    if (!NT_SUCCESS(status)) return status;
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
    pnp.EvtDevicePrepareHardware = Prepare;
    pnp.EvtDeviceReleaseHardware = Release;
    WdfDeviceInitSetPnpPowerEventCallbacks(init, &pnp);
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, MT7922_CONTEXT);
    attributes.ExecutionLevel = WdfExecutionLevelPassive;
    status = WdfDeviceCreate(&init, &attributes, &device);
    if (!NT_SUCCESS(status)) return status;
    status = WdfDeviceCreateSymbolicLink(device, &link);
    if (!NT_SUCCESS(status)) return status;
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queue, WdfIoQueueDispatchSequential);
    queue.EvtIoDeviceControl = Control;
    return WdfIoQueueCreate(device, &queue, WDF_NO_OBJECT_ATTRIBUTES, NULL);
}

NTSTATUS DriverEntry(PDRIVER_OBJECT driver, PUNICODE_STRING path) {
    WDF_DRIVER_CONFIG config;
    WDF_DRIVER_CONFIG_INIT(&config, AddDevice);
    return WdfDriverCreate(driver, path, WDF_NO_OBJECT_ATTRIBUTES, &config, WDF_NO_HANDLE);
}
