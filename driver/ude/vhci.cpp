/*
 * Copyright (C) 2022 Vadym Hrynchyshyn <vadimgrn@gmail.com>
 */

#include "vhci.h"
#include "trace.h"
#include "vhci.tmh"

#include <ntstrsafe.h>

#include <usb.h>
#include <usbdlib.h>
#include <usbiodef.h>

#include <wdfusb.h>
#include <Udecx.h>

#include "driver.h"
#include "usbdevice.h"
#include "vhci_queue.h"

namespace
{

using namespace usbip;

_Function_class_(EVT_WDF_OBJECT_CONTEXT_CLEANUP)
_IRQL_requires_same_
_IRQL_requires_max_(DISPATCH_LEVEL)
void vhci_cleanup(_In_ WDFOBJECT DeviceObject)
{
        auto vhci = static_cast<WDFDEVICE>(DeviceObject);
        Trace(TRACE_LEVEL_INFORMATION, "vhci %04x", ptr04x(vhci));
}

_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
auto create_interfaces(_In_ WDFDEVICE vhci)
{
        PAGED_CODE();

        const GUID* v[] = {
                &GUID_DEVINTERFACE_USB_HOST_CONTROLLER,
                &vhci::GUID_DEVINTERFACE_USB_HOST_CONTROLLER
        };

        for (auto guid: v) {
                if (auto err = WdfDeviceCreateDeviceInterface(vhci, guid, nullptr)) {
                        Trace(TRACE_LEVEL_ERROR, "WdfDeviceCreateDeviceInterface(%!GUID!) %!STATUS!", guid, err);
                        return err;
                }
        }

        return STATUS_SUCCESS;
}

_Function_class_(EVT_UDECX_WDF_DEVICE_QUERY_USB_CAPABILITY)
_IRQL_requires_same_
NTSTATUS query_usb_capability(
        _In_ WDFDEVICE /*UdecxWdfDevice*/,
        _In_ GUID *CapabilityType,
        _In_ ULONG /*OutputBufferLength*/,
        _Out_writes_to_opt_(OutputBufferLength, *ResultLength) PVOID /*OutputBuffer*/,
        _Out_ ULONG *ResultLength)
{
        const GUID* supported[] = {
                &GUID_USB_CAPABILITY_CHAINED_MDLS, 
                &GUID_USB_CAPABILITY_SELECTIVE_SUSPEND, // class extension reports it as supported without invoking the callback
                &GUID_USB_CAPABILITY_FUNCTION_SUSPEND,
                &GUID_USB_CAPABILITY_DEVICE_CONNECTION_HIGH_SPEED_COMPATIBLE, 
                &GUID_USB_CAPABILITY_DEVICE_CONNECTION_SUPER_SPEED_COMPATIBLE 
        };

        auto st = STATUS_NOT_SUPPORTED;

        for (auto i: supported) {
                if (*i == *CapabilityType) {
                        st = STATUS_SUCCESS;
                        break;
                }
        }

        *ResultLength = 0;
        return st;
}

_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGEABLE auto initialize(_Inout_ WDFDEVICE_INIT *DeviceInit)
{
        PAGED_CODE();

        WDF_PNPPOWER_EVENT_CALLBACKS pnp_power;
        WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp_power);
        WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnp_power);

        WDF_DEVICE_POWER_POLICY_IDLE_SETTINGS idle_settings;
        WDF_DEVICE_POWER_POLICY_IDLE_SETTINGS_INIT(&idle_settings, IdleUsbSelectiveSuspend); // IdleCanWakeFromS0

        WDF_OBJECT_ATTRIBUTES request_attrs;
        WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&request_attrs, request_context);
        WdfDeviceInitSetRequestAttributes(DeviceInit, &request_attrs);

        WDF_FILEOBJECT_CONFIG fobj_cfg;
        WDF_FILEOBJECT_CONFIG_INIT(&fobj_cfg, WDF_NO_EVENT_CALLBACK, WDF_NO_EVENT_CALLBACK, WDF_NO_EVENT_CALLBACK);
        fobj_cfg.FileObjectClass = WdfFileObjectNotRequired;
        WdfDeviceInitSetFileObjectConfig(DeviceInit, &fobj_cfg, WDF_NO_OBJECT_ATTRIBUTES);

        WdfDeviceInitSetCharacteristics(DeviceInit, FILE_AUTOGENERATED_DEVICE_NAME, true);

        if (auto err = WdfDeviceInitAssignSDDLString(DeviceInit, &SDDL_DEVOBJ_SYS_ALL_ADM_RWX_WORLD_RW_RES_R)) {
                Trace(TRACE_LEVEL_ERROR, "WdfDeviceInitAssignSDDLString %!STATUS!", err);
                return err;
        }

        if (auto err = UdecxInitializeWdfDeviceInit(DeviceInit)) {
                Trace(TRACE_LEVEL_ERROR, "UdecxInitializeWdfDeviceInit %!STATUS!", err);
                return err;
        }

        return STATUS_SUCCESS;
}

_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGEABLE auto initialize(_In_ WDFDEVICE vhci)
{
        PAGED_CODE();
        auto &ctx = *get_vhci_context(vhci);

        WDF_OBJECT_ATTRIBUTES attrs;
        WDF_OBJECT_ATTRIBUTES_INIT(&attrs);
        attrs.ParentObject = vhci;

        if (auto err = WdfSpinLockCreate(&attrs, &ctx.devices_lock)) {
                Trace(TRACE_LEVEL_ERROR, "WdfSpinLockCreate %!STATUS!", err);
                return err;
        }

        return STATUS_SUCCESS;
}

_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGEABLE auto create_device(_Out_ WDFDEVICE &vhci, _In_ WDFDEVICE_INIT *DeviceInit)
{
        PAGED_CODE();

        WDF_OBJECT_ATTRIBUTES attrs;
        WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attrs, vhci_context);
        attrs.EvtCleanupCallback = vhci_cleanup;

        if (auto err = WdfDeviceCreate(&DeviceInit, &attrs, &vhci)) {
                Trace(TRACE_LEVEL_ERROR, "WdfDeviceCreate %!STATUS!", err);
                return err;
        }

        if (auto err = initialize(vhci)) {
                return err;
        }

        return create_interfaces(vhci);
}

_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGEABLE auto add_usb_device_emulation(_In_ WDFDEVICE vhci)
{
        PAGED_CODE();

        UDECX_WDF_DEVICE_CONFIG cfg;
        UDECX_WDF_DEVICE_CONFIG_INIT(&cfg, query_usb_capability);

        cfg.NumberOfUsb20Ports = vhci::USB2_PORTS;
        cfg.NumberOfUsb30Ports = vhci::USB3_PORTS;

        if (auto err = UdecxWdfDeviceAddUsbDeviceEmulation(vhci, &cfg)) {
                Trace(TRACE_LEVEL_ERROR, "UdecxWdfDeviceAddUsbDeviceEmulation %!STATUS!", err);
                return err;
        }

        return create_default_queue(vhci);
}

} // namespace


_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGEABLE int usbip::claim_roothub_port(_In_ UDECXUSBDEVICE udev, _In_ UDECX_USB_DEVICE_SPEED speed)
{
        PAGED_CODE();

        auto &udev_ctx = *get_usbdevice_context(udev);
        auto &vhci_ctx = *get_vhci_context(udev_ctx.vhci); 

        auto &port = udev_ctx.port;
        NT_ASSERT(!port);

        auto hci_ver = to_hci_version(speed);
        auto from = make_port(hci_ver, 0);

        WdfSpinLockAcquire(vhci_ctx.devices_lock);

        for (auto i = from; i < from + get_num_ports(hci_ver); ++i) {

                NT_ASSERT(i < ARRAYSIZE(vhci_ctx.devices));
                auto &handle = vhci_ctx.devices[i];

                if (!handle) {
                        WdfObjectReference(handle = udev);
                        port = i + 1;
                        NT_ASSERT(vhci::is_valid_port(port));
                        break;
                }
        }

        WdfSpinLockRelease(vhci_ctx.devices_lock);

        if (port) {
                TraceDbg("udev %04x, port %d", ptr04x(udev), port);
        }

        return port;
}

_IRQL_requires_same_
_IRQL_requires_max_(DISPATCH_LEVEL)
void usbip::reclaim_roothub_port(_In_ UDECXUSBDEVICE udev)
{
        auto &udev_ctx = *get_usbdevice_context(udev);
        static_assert(sizeof(udev_ctx.port) == sizeof(LONG));

        auto port = InterlockedExchange(reinterpret_cast<LONG*>(&udev_ctx.port), 0);
        if (!port) {
                return;
        }

        TraceDbg("udev %04x, port %ld", ptr04x(udev), port);
        NT_ASSERT(vhci::is_valid_port(port));

        auto &vhci_ctx = *get_vhci_context(udev_ctx.vhci); 

        WdfSpinLockAcquire(vhci_ctx.devices_lock);
        {
                auto &handle = vhci_ctx.devices[port - 1];
                NT_ASSERT(handle == udev);
                handle = WDF_NO_HANDLE;
        }
        WdfSpinLockRelease(vhci_ctx.devices_lock);

        WdfObjectDereference(udev);
}

_IRQL_requires_same_
_IRQL_requires_max_(DISPATCH_LEVEL)
wdf::WdfObjectRef usbip::get_usbdevice(_In_ WDFDEVICE vhci, _In_ int port)
{
        wdf::WdfObjectRef udev;
        if (!vhci::is_valid_port(port)) {
                return udev;
        }

        auto &ctx = *get_vhci_context(vhci);
        WdfSpinLockAcquire(ctx.devices_lock);

        if (auto handle = ctx.devices[port - 1]) {
                udev.reset(handle); // adds reference
                NT_ASSERT(get_usbdevice_context(udev.get())->port == port);
        }

        WdfSpinLockRelease(ctx.devices_lock);
        return udev;
}

_Function_class_(EVT_WDF_DRIVER_DEVICE_ADD)
_IRQL_requires_same_
_IRQL_requires_(PASSIVE_LEVEL)
PAGEABLE NTSTATUS usbip::DriverDeviceAdd(_In_ WDFDRIVER, _Inout_ WDFDEVICE_INIT *DeviceInit)
{
        PAGED_CODE();

        if (auto err = initialize(DeviceInit)) {
                return err;
        }

        WDFDEVICE vhci{};
        static_assert(!WDF_NO_HANDLE);

        if (auto err = create_device(vhci, DeviceInit)) {
                if (vhci) {
                        WdfObjectDelete(vhci);
                }
                return err;
        }

        if (auto err = add_usb_device_emulation(vhci)) {
                return err;
        }

        Trace(TRACE_LEVEL_INFORMATION, "vhci %04x", ptr04x(vhci));
        return STATUS_SUCCESS;
}
