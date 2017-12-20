/* Copyright (c) Citrix Systems Inc.
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, 
 * with or without modification, are permitted provided 
 * that the following conditions are met:
 * 
 * *   Redistributions of source code must retain the above 
 *     copyright notice, this list of conditions and the 
 *     following disclaimer.
 * *   Redistributions in binary form must reproduce the above 
 *     copyright notice, this list of conditions and the 
 *     following disclaimer in the documentation and/or other 
 *     materials provided with the distribution.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND 
 * CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, 
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF 
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE 
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR 
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, 
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, 
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR 
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS 
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, 
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING 
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE 
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF 
 * SUCH DAMAGE.
 */

#define INITGUID 1

#include <ntddk.h>
#include <wdmguid.h>
#include <devguid.h>
#include <ntstrsafe.h>
#include <stdlib.h>
#include <xen.h>

#include <suspend_interface.h>
#include <unplug_interface.h>
#include <debug_interface.h>

#include "names.h"
#include "fdo.h"
#include "pdo.h"
#include "bus.h"
#include "frontend.h"
#include "hid.h"
#include "driver.h"
#include "registry.h"
#include "thread.h"
#include "dbg_print.h"
#include "assert.h"
#include "util.h"
#include "version.h"
#include "revision.h"

#define PDO_POOL 'ODP'

#define MAXNAMELEN  128

struct _XENVKBD_PDO {
    PXENVKBD_DX                 Dx;

    PXENVKBD_THREAD             SystemPowerThread;
    PIRP                        SystemPowerIrp;
    PXENVKBD_THREAD             DevicePowerThread;
    PIRP                        DevicePowerIrp;

    PXENVKBD_FDO                Fdo;
    BOOLEAN                     Missing;
    const CHAR                  *Reason;
    LONG                        Eject;

    BUS_INTERFACE_STANDARD      BusInterface;

    XENBUS_SUSPEND_INTERFACE    SuspendInterface;
    PXENBUS_SUSPEND_CALLBACK    SuspendCallbackLate;

    XENBUS_UNPLUG_INTERFACE     UnplugInterface;
    BOOLEAN                     UnplugRequested;

    PXENVKBD_FRONTEND           Frontend;

    PXENVKBD_HID_CONTEXT        HidContext;
    XENHID_HID_INTERFACE        HidInterface;
};

static FORCEINLINE PVOID
__PdoAllocate(
    IN  ULONG   Length
    )
{
    return __AllocatePoolWithTag(NonPagedPool, Length, PDO_POOL);
}

static FORCEINLINE VOID
__PdoFree(
    IN  PVOID   Buffer
    )
{
    __FreePoolWithTag(Buffer, PDO_POOL);
}

static FORCEINLINE VOID
__PdoSetDevicePnpState(
    IN  PXENVKBD_PDO        Pdo,
    IN  DEVICE_PNP_STATE    State
    )
{
    PXENVKBD_DX             Dx = Pdo->Dx;

    // We can never transition out of the deleted state
    ASSERT(Dx->DevicePnpState != Deleted || State == Deleted);

    Dx->PreviousDevicePnpState = Dx->DevicePnpState;
    Dx->DevicePnpState = State;
}

VOID
PdoSetDevicePnpState(
    IN  PXENVKBD_PDO        Pdo,
    IN  DEVICE_PNP_STATE    State
    )
{
    __PdoSetDevicePnpState(Pdo, State);
}

static FORCEINLINE VOID
__PdoRestoreDevicePnpState(
    IN  PXENVKBD_PDO        Pdo,
    IN  DEVICE_PNP_STATE    State
    )
{
    PXENVKBD_DX             Dx = Pdo->Dx;

    if (Dx->DevicePnpState == State)
        Dx->DevicePnpState = Dx->PreviousDevicePnpState;
}

static FORCEINLINE DEVICE_PNP_STATE
__PdoGetDevicePnpState(
    IN  PXENVKBD_PDO    Pdo
    )
{
    PXENVKBD_DX         Dx = Pdo->Dx;

    return Dx->DevicePnpState;
}

DEVICE_PNP_STATE
PdoGetDevicePnpState(
    IN  PXENVKBD_PDO    Pdo
    )
{
    return __PdoGetDevicePnpState(Pdo);
}

static FORCEINLINE VOID
__PdoSetSystemPowerState(
    IN  PXENVKBD_PDO        Pdo,
    IN  SYSTEM_POWER_STATE  State
    )
{
    PXENVKBD_DX             Dx = Pdo->Dx;

    Dx->SystemPowerState = State;
}

static FORCEINLINE SYSTEM_POWER_STATE
__PdoGetSystemPowerState(
    IN  PXENVKBD_PDO    Pdo
    )
{
    PXENVKBD_DX         Dx = Pdo->Dx;

    return Dx->SystemPowerState;
}

static FORCEINLINE VOID
__PdoSetDevicePowerState(
    IN  PXENVKBD_PDO        Pdo,
    IN  DEVICE_POWER_STATE  State
    )
{
    PXENVKBD_DX             Dx = Pdo->Dx;

    Dx->DevicePowerState = State;
}

static FORCEINLINE DEVICE_POWER_STATE
__PdoGetDevicePowerState(
    IN  PXENVKBD_PDO    Pdo
    )
{
    PXENVKBD_DX         Dx = Pdo->Dx;

    return Dx->DevicePowerState;
}

static FORCEINLINE VOID
__PdoSetMissing(
    IN  PXENVKBD_PDO    Pdo,
    IN  const CHAR      *Reason
    )
{
    Pdo->Reason = Reason;
    Pdo->Missing = TRUE;
}

VOID
PdoSetMissing(
    IN  PXENVKBD_PDO    Pdo,
    IN  const CHAR      *Reason
    )
{
    __PdoSetMissing(Pdo, Reason);
}

static FORCEINLINE BOOLEAN
__PdoIsMissing(
    IN  PXENVKBD_PDO    Pdo
    )
{
    return Pdo->Missing;
}

BOOLEAN
PdoIsMissing(
    IN  PXENVKBD_PDO    Pdo
    )
{
    return __PdoIsMissing(Pdo);
}

static FORCEINLINE PXENVKBD_FDO
__PdoGetFdo(
    IN  PXENVKBD_PDO    Pdo
    )
{
    return Pdo->Fdo;
}

PXENVKBD_FDO
PdoGetFdo(
    IN  PXENVKBD_PDO    Pdo
    )
{
    return __PdoGetFdo(Pdo);
}

static FORCEINLINE VOID
__PdoSetName(
    IN  PXENVKBD_PDO    Pdo,
    IN  PCHAR           Path
    )
{
    PXENVKBD_DX         Dx = Pdo->Dx;
    NTSTATUS            status;

    status = RtlStringCbPrintfA(Dx->Name, 
                                MAX_DEVICE_ID_LEN, 
                                "%s", 
                                Path);
    ASSERT(NT_SUCCESS(status));
}

static FORCEINLINE PCHAR
__PdoGetName(
    IN  PXENVKBD_PDO    Pdo
    )
{
    PXENVKBD_DX         Dx = Pdo->Dx;

    return Dx->Name;
}

PCHAR
PdoGetName(
    IN  PXENVKBD_PDO    Pdo
    )
{
    return __PdoGetName(Pdo);
}

static FORCEINLINE BOOLEAN
__PdoSetEjectRequested(
    IN  PXENVKBD_PDO    Pdo
    )
{
    return (InterlockedBitTestAndSet(&Pdo->Eject, 0) == 0) ? TRUE : FALSE;
}

VOID
PdoRequestEject(
    IN  PXENVKBD_PDO    Pdo
    )
{
    PXENVKBD_DX         Dx = Pdo->Dx;
    PDEVICE_OBJECT      PhysicalDeviceObject = Dx->DeviceObject;
    PXENVKBD_FDO        Fdo = __PdoGetFdo(Pdo);

    if (!__PdoSetEjectRequested(Pdo))
        return;

    Info("%p (%s)\n",
         PhysicalDeviceObject,
         __PdoGetName(Pdo));

    IoInvalidateDeviceRelations(FdoGetPhysicalDeviceObject(Fdo),
                                BusRelations);
}

static FORCEINLINE BOOLEAN
__PdoClearEjectRequested(
    IN  PXENVKBD_PDO    Pdo
    )
{
    return (InterlockedBitTestAndReset(&Pdo->Eject, 0) != 0) ? TRUE : FALSE;
}

static FORCEINLINE BOOLEAN
__PdoIsEjectRequested(
    IN  PXENVKBD_PDO    Pdo
    )
{
    KeMemoryBarrier();
    return (Pdo->Eject & 1) ? TRUE : FALSE;
}

BOOLEAN
PdoIsEjectRequested(
    IN  PXENVKBD_PDO    Pdo
    )
{
    return __PdoIsEjectRequested(Pdo);
}

#define MAXTEXTLEN  1024

typedef struct _XENVKBD_PDO_REVISION {
    ULONG   Number;
    ULONG   HidInterfaceVersion;
} XENVKBD_PDO_REVISION, *PXENVKBD_PDO_REVISION;

#define DEFINE_REVISION(_N, _H) \
    { (_N), (_H) }

static XENVKBD_PDO_REVISION PdoRevision[] = {
    DEFINE_REVISION_TABLE
};

#undef DEFINE_REVISION

static VOID
PdoDumpRevisions(
    IN  PXENVKBD_PDO    Pdo
    )
{
    ULONG               Index;

    UNREFERENCED_PARAMETER(Pdo);

    for (Index = 0; Index < ARRAYSIZE(PdoRevision); Index++) {
        PXENVKBD_PDO_REVISION Revision = &PdoRevision[Index];

        ASSERT3U(Revision->HidInterfaceVersion, >=, XENHID_HID_INTERFACE_VERSION_MIN);
        ASSERT3U(Revision->HidInterfaceVersion, <=, XENHID_HID_INTERFACE_VERSION_MAX);
        ASSERT(IMPLY(Index == ARRAYSIZE(PdoRevision) - 1,
                     Revision->HidInterfaceVersion == XENHID_HID_INTERFACE_VERSION_MAX));

        Info("%08X -> "
             "HID v%u\n",
             Revision->Number,
             Revision->HidInterfaceVersion);
    }
}

static FORCEINLINE PDEVICE_OBJECT
__PdoGetDeviceObject(
    IN  PXENVKBD_PDO    Pdo
    )
{
    PXENVKBD_DX         Dx = Pdo->Dx;

    return (Dx->DeviceObject);
}
    
PDEVICE_OBJECT
PdoGetDeviceObject(
    IN  PXENVKBD_PDO    Pdo
    )
{
    return __PdoGetDeviceObject(Pdo);
}

static FORCEINLINE PCHAR
__PdoGetVendorName(
    IN  PXENVKBD_PDO    Pdo
    )
{
    return FdoGetVendorName(__PdoGetFdo(Pdo));
}

static FORCEINLINE PXENVKBD_FRONTEND
__PdoGetFrontend(
    IN  PXENVKBD_PDO    Pdo
    )
{
    return Pdo->Frontend;
}

PXENVKBD_FRONTEND
PdoGetFrontend(
    IN  PXENVKBD_PDO    Pdo
    )
{
    return __PdoGetFrontend(Pdo);
}

static FORCEINLINE PXENVKBD_HID_CONTEXT
__PdoGetHidContext(
    IN  PXENVKBD_PDO Pdo
    )
{
    return Pdo->HidContext;
}

PXENVKBD_HID_CONTEXT
PdoGetHidContext(
    IN  PXENVKBD_PDO    Pdo
    )
{
    return __PdoGetHidContext(Pdo);
}

PDMA_ADAPTER
PdoGetDmaAdapter(
    IN  PXENVKBD_PDO        Pdo,
    IN  PDEVICE_DESCRIPTION DeviceDescriptor,
    OUT PULONG              NumberOfMapRegisters
    )
{
    Trace("<===>\n");

    return FdoGetDmaAdapter(__PdoGetFdo(Pdo),
                            DeviceDescriptor,
                            NumberOfMapRegisters);
}

BOOLEAN
PdoTranslateBusAddress(
    IN      PXENVKBD_PDO        Pdo,
    IN      PHYSICAL_ADDRESS    BusAddress,
    IN      ULONG               Length,
    IN OUT  PULONG              AddressSpace,
    OUT     PPHYSICAL_ADDRESS   TranslatedAddress
    )
{
    Trace("<===>\n");

    return FdoTranslateBusAddress(__PdoGetFdo(Pdo),
                                  BusAddress,
                                  Length,
                                  AddressSpace,
                                  TranslatedAddress);
}

ULONG
PdoSetBusData(
    IN  PXENVKBD_PDO    Pdo,
    IN  ULONG           DataType,
    IN  PVOID           Buffer,
    IN  ULONG           Offset,
    IN  ULONG           Length
    )
{
    Trace("<===>\n");

    return FdoSetBusData(__PdoGetFdo(Pdo),
                         DataType,
                         Buffer,
                         Offset,
                         Length);
}

ULONG
PdoGetBusData(
    IN  PXENVKBD_PDO    Pdo,
    IN  ULONG           DataType,
    IN  PVOID           Buffer,
    IN  ULONG           Offset,
    IN  ULONG           Length
    )
{
    Trace("<===>\n");

    return FdoGetBusData(__PdoGetFdo(Pdo),
                         DataType,
                         Buffer,
                         Offset,
                         Length);
}

static FORCEINLINE NTSTATUS
__PdoD3ToD0(
    IN  PXENVKBD_PDO            Pdo
    )
{
    POWER_STATE                 PowerState;
    NTSTATUS                    status;

    Trace("(%s) ====>\n", __PdoGetName(Pdo));

    ASSERT3U(KeGetCurrentIrql(), ==, DISPATCH_LEVEL);
    ASSERT3U(__PdoGetDevicePowerState(Pdo), ==, PowerDeviceD3);

    // Dont take over keyboard/mouse until XenHid starts
    // XenHid calling XENHID_HID(Enable...) sets state to ENABLED
    status = FrontendSetState(__PdoGetFrontend(Pdo), FRONTEND_CLOSED);
    if (!NT_SUCCESS(status))
        goto fail1;

    __PdoSetDevicePowerState(Pdo, PowerDeviceD0);

    PowerState.DeviceState = PowerDeviceD0;
    PoSetPowerState(__PdoGetDeviceObject(Pdo),
                    DevicePowerState,
                    PowerState);

    Trace("(%s) <====\n", __PdoGetName(Pdo));

    return STATUS_SUCCESS;

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

static FORCEINLINE VOID
__PdoD0ToD3(
    IN  PXENVKBD_PDO    Pdo
    )
{
    POWER_STATE         PowerState;

    Trace("(%s) ====>\n", __PdoGetName(Pdo));

    ASSERT3U(KeGetCurrentIrql(), ==, DISPATCH_LEVEL);
    ASSERT3U(__PdoGetDevicePowerState(Pdo), ==, PowerDeviceD0);

    PowerState.DeviceState = PowerDeviceD3;
    PoSetPowerState(__PdoGetDeviceObject(Pdo),
                    DevicePowerState,
                    PowerState);

    __PdoSetDevicePowerState(Pdo, PowerDeviceD3);

    (VOID) FrontendSetState(__PdoGetFrontend(Pdo), FRONTEND_CLOSED);

    Trace("(%s) <====\n", __PdoGetName(Pdo));
}

static DECLSPEC_NOINLINE VOID
PdoSuspendCallbackLate(
    IN  PVOID           Argument
    )
{
    PXENVKBD_PDO        Pdo = Argument;
    NTSTATUS            status;

    __PdoD0ToD3(Pdo);

    status = __PdoD3ToD0(Pdo);
    ASSERT(NT_SUCCESS(status));
}

// This function must not touch pageable code or data
static DECLSPEC_NOINLINE NTSTATUS
PdoD3ToD0(
    IN  PXENVKBD_PDO    Pdo
    )
{
    KIRQL               Irql;
    NTSTATUS            status;

    ASSERT3U(KeGetCurrentIrql(), ==, PASSIVE_LEVEL);

    KeRaiseIrql(DISPATCH_LEVEL, &Irql);

    status = XENBUS_SUSPEND(Acquire, &Pdo->SuspendInterface);
    if (!NT_SUCCESS(status))
        goto fail1;

    status = __PdoD3ToD0(Pdo);
    if (!NT_SUCCESS(status))
        goto fail2;

    status = XENBUS_SUSPEND(Register,
                            &Pdo->SuspendInterface,
                            SUSPEND_CALLBACK_LATE,
                            PdoSuspendCallbackLate,
                            Pdo,
                            &Pdo->SuspendCallbackLate);
    if (!NT_SUCCESS(status))
        goto fail3;

    KeLowerIrql(Irql);

    return STATUS_SUCCESS;

fail3:
    Error("fail3\n");

    __PdoD0ToD3(Pdo);

fail2:
    Error("fail2\n");

    XENBUS_SUSPEND(Release, &Pdo->SuspendInterface);

fail1:
    Error("fail1 (%08x)\n", status);

    KeLowerIrql(Irql);

    return status;
}

// This function must not touch pageable code or data
static DECLSPEC_NOINLINE VOID
PdoD0ToD3(
    IN  PXENVKBD_PDO    Pdo
    )
{
    KIRQL               Irql;

    ASSERT3U(KeGetCurrentIrql(), ==, PASSIVE_LEVEL);

    KeRaiseIrql(DISPATCH_LEVEL, &Irql);

    XENBUS_SUSPEND(Deregister,
                   &Pdo->SuspendInterface,
                   Pdo->SuspendCallbackLate);
    Pdo->SuspendCallbackLate = NULL;

    __PdoD0ToD3(Pdo);

    XENBUS_SUSPEND(Release, &Pdo->SuspendInterface);

    KeLowerIrql(Irql);
}

// This function must not touch pageable code or data
static DECLSPEC_NOINLINE VOID
PdoS4ToS3(
    IN  PXENVKBD_PDO    Pdo
    )
{
    Trace("(%s) ====>\n", __PdoGetName(Pdo));

    ASSERT3U(KeGetCurrentIrql(), ==, PASSIVE_LEVEL);
    ASSERT3U(__PdoGetSystemPowerState(Pdo), ==, PowerSystemHibernate);

    __PdoSetSystemPowerState(Pdo, PowerSystemSleeping3);

    Trace("(%s) <====\n", __PdoGetName(Pdo));
}

// This function must not touch pageable code or data
static DECLSPEC_NOINLINE VOID
PdoS3ToS4(
    IN  PXENVKBD_PDO    Pdo
    )
{
    Trace("(%s) ====>\n", __PdoGetName(Pdo));

    ASSERT3U(KeGetCurrentIrql(), ==, PASSIVE_LEVEL);
    ASSERT3U(__PdoGetSystemPowerState(Pdo), ==, PowerSystemSleeping3);

    __PdoSetSystemPowerState(Pdo, PowerSystemHibernate);

    Trace("(%s) <====\n", __PdoGetName(Pdo));
}

static DECLSPEC_NOINLINE NTSTATUS
PdoStartDevice(
    IN  PXENVKBD_PDO    Pdo,
    IN  PIRP            Irp
    )
{
    NTSTATUS            status;

    status = PdoD3ToD0(Pdo);
    if (!NT_SUCCESS(status))
        goto fail1;

    __PdoSetDevicePnpState(Pdo, Started);

    Irp->IoStatus.Status = STATUS_SUCCESS;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return STATUS_SUCCESS;

fail1:
    Error("fail1 (%08x)\n", status);

    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static DECLSPEC_NOINLINE NTSTATUS
PdoQueryStopDevice(
    IN  PXENVKBD_PDO    Pdo,
    IN  PIRP            Irp
    )
{
    NTSTATUS            status;

    __PdoSetDevicePnpState(Pdo, StopPending);
    status = STATUS_SUCCESS;

    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static DECLSPEC_NOINLINE NTSTATUS
PdoCancelStopDevice(
    IN  PXENVKBD_PDO    Pdo,
    IN  PIRP            Irp
    )
{
    NTSTATUS            status;

    __PdoRestoreDevicePnpState(Pdo, StopPending);
    status = STATUS_SUCCESS;

    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static DECLSPEC_NOINLINE NTSTATUS
PdoStopDevice(
    IN  PXENVKBD_PDO    Pdo,
    IN  PIRP            Irp
    )
{
    NTSTATUS            status;

    if (__PdoGetDevicePowerState(Pdo) != PowerDeviceD0)
        goto done;

    PdoD0ToD3(Pdo);

done:
    __PdoSetDevicePnpState(Pdo, Stopped);
    status = STATUS_SUCCESS;

    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static DECLSPEC_NOINLINE NTSTATUS
PdoQueryRemoveDevice(
    IN  PXENVKBD_PDO    Pdo,
    IN  PIRP            Irp
    )
{
    NTSTATUS            status;

    __PdoSetDevicePnpState(Pdo, RemovePending);
    status = STATUS_SUCCESS;

    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static DECLSPEC_NOINLINE NTSTATUS
PdoCancelRemoveDevice(
    IN  PXENVKBD_PDO    Pdo,
    IN  PIRP            Irp
    )
{
    NTSTATUS            status;

    if (__PdoClearEjectRequested(Pdo))
        FrontendEjectFailed(__PdoGetFrontend(Pdo));

    __PdoRestoreDevicePnpState(Pdo, RemovePending);
    status = STATUS_SUCCESS;

    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static DECLSPEC_NOINLINE NTSTATUS
PdoSurpriseRemoval(
    IN  PXENVKBD_PDO    Pdo,
    IN  PIRP            Irp
    )
{
    NTSTATUS            status;

    Warning("%s\n", __PdoGetName(Pdo));

    __PdoSetDevicePnpState(Pdo, SurpriseRemovePending);
    status = STATUS_SUCCESS;

    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static DECLSPEC_NOINLINE NTSTATUS
PdoRemoveDevice(
    IN  PXENVKBD_PDO    Pdo,
    IN  PIRP            Irp
    )
{
    PXENVKBD_FDO        Fdo = __PdoGetFdo(Pdo);
    BOOLEAN             NeedInvalidate;
    NTSTATUS            status;

    if (__PdoGetDevicePowerState(Pdo) != PowerDeviceD0)
        goto done;

    PdoD0ToD3(Pdo);

done:
    NeedInvalidate = FALSE;

    FdoAcquireMutex(Fdo);

    if (__PdoIsMissing(Pdo)) {
        DEVICE_PNP_STATE    State = __PdoGetDevicePnpState(Pdo);

        __PdoSetDevicePnpState(Pdo, Deleted);

        if (State == SurpriseRemovePending)
            PdoDestroy(Pdo);
        else
            NeedInvalidate = TRUE;
    } else {
        __PdoSetDevicePnpState(Pdo, Enumerated);
    }

    FdoReleaseMutex(Fdo);

    if (NeedInvalidate)
        IoInvalidateDeviceRelations(FdoGetPhysicalDeviceObject(Fdo), 
                                    BusRelations);

    status = STATUS_SUCCESS;

    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static DECLSPEC_NOINLINE NTSTATUS
PdoQueryDeviceRelations(
    IN  PXENVKBD_PDO    Pdo,
    IN  PIRP            Irp
    )
{
    PIO_STACK_LOCATION  StackLocation;
    PDEVICE_RELATIONS   Relations;
    NTSTATUS            status;

    StackLocation = IoGetCurrentIrpStackLocation(Irp);

    status = Irp->IoStatus.Status;

    if (StackLocation->Parameters.QueryDeviceRelations.Type != TargetDeviceRelation)
        goto done;

    Relations = ExAllocatePoolWithTag(PagedPool, sizeof (DEVICE_RELATIONS), 'FIV');

    status = STATUS_NO_MEMORY;
    if (Relations == NULL)
        goto done;

    RtlZeroMemory(Relations, sizeof (DEVICE_RELATIONS));

    Relations->Count = 1;
    ObReferenceObject(__PdoGetDeviceObject(Pdo));
    Relations->Objects[0] = __PdoGetDeviceObject(Pdo);

    Irp->IoStatus.Information = (ULONG_PTR)Relations;
    status = STATUS_SUCCESS;

done:
    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static NTSTATUS
PdoDelegateIrp(
    IN  PXENVKBD_PDO    Pdo,
    IN  PIRP            Irp
    )
{
    return FdoDelegateIrp(Pdo->Fdo, Irp);
}

static NTSTATUS
PdoQueryBusInterface(
    IN  PXENVKBD_PDO        Pdo,
    IN  PIRP                Irp
    )
{
    PIO_STACK_LOCATION      StackLocation;
    USHORT                  Size;
    USHORT                  Version;
    PBUS_INTERFACE_STANDARD BusInterface;
    NTSTATUS                status;

    status = Irp->IoStatus.Status;        

    StackLocation = IoGetCurrentIrpStackLocation(Irp);
    Size = StackLocation->Parameters.QueryInterface.Size;
    Version = StackLocation->Parameters.QueryInterface.Version;
    BusInterface = (PBUS_INTERFACE_STANDARD)StackLocation->Parameters.QueryInterface.Interface;

    if (Version != 1)
        goto done;

    status = STATUS_BUFFER_TOO_SMALL;        
    if (Size < sizeof (BUS_INTERFACE_STANDARD))
        goto done;

    *BusInterface = Pdo->BusInterface;
    BusInterface->InterfaceReference(BusInterface->Context);

    Irp->IoStatus.Information = 0;
    status = STATUS_SUCCESS;

done:
    return status;
}

static NTSTATUS
PdoQueryHidInterface(
    IN  PXENVKBD_PDO        Pdo,
    IN  PIRP                Irp
    )
{
    PIO_STACK_LOCATION      StackLocation;
    USHORT                  Size;
    USHORT                  Version;
    PINTERFACE              Interface;
    PVOID                   Context;
    NTSTATUS                status;

    status = Irp->IoStatus.Status;        

    StackLocation = IoGetCurrentIrpStackLocation(Irp);
    Size = StackLocation->Parameters.QueryInterface.Size;
    Version = StackLocation->Parameters.QueryInterface.Version;
    Interface = StackLocation->Parameters.QueryInterface.Interface;

    Context = __PdoGetHidContext(Pdo);

    status = HidGetInterface(Context,
                             Version,
                             Interface,
                             Size);
    if (!NT_SUCCESS(status))
        goto done;

    Irp->IoStatus.Information = 0;
    status = STATUS_SUCCESS;

done:
    return status;
}

struct _INTERFACE_ENTRY {
    const GUID  *Guid;
    const CHAR  *Name;
    NTSTATUS    (*Query)(PXENVKBD_PDO, PIRP);
};

struct _INTERFACE_ENTRY PdoInterfaceTable[] = {
    { &GUID_BUS_INTERFACE_STANDARD, "BUS_INTERFACE", PdoQueryBusInterface },
    { &GUID_XENHID_HID_INTERFACE, "HID_INTERFACE", PdoQueryHidInterface },
    { &GUID_XENBUS_CACHE_INTERFACE, "CACHE_INTERFACE", PdoDelegateIrp },
    { &GUID_XENBUS_STORE_INTERFACE, "STORE_INTERFACE", PdoDelegateIrp },
    { &GUID_XENBUS_SUSPEND_INTERFACE, "SUSPEND_INTERFACE", PdoDelegateIrp },
    { NULL, NULL, NULL }
};

static DECLSPEC_NOINLINE NTSTATUS
PdoQueryInterface(
    IN  PXENVKBD_PDO        Pdo,
    IN  PIRP                Irp
    )
{
    PIO_STACK_LOCATION      StackLocation;
    const GUID              *InterfaceType;
    struct _INTERFACE_ENTRY *Entry;
    USHORT                  Version;
    NTSTATUS                status;

    status = Irp->IoStatus.Status;        

    if (status != STATUS_NOT_SUPPORTED)
        goto done;

    StackLocation = IoGetCurrentIrpStackLocation(Irp);
    InterfaceType = StackLocation->Parameters.QueryInterface.InterfaceType;
    Version = StackLocation->Parameters.QueryInterface.Version;

    for (Entry = PdoInterfaceTable; Entry->Guid != NULL; Entry++) {
        if (IsEqualGUID(InterfaceType, Entry->Guid)) {
            Info("%s: %s (VERSION %d)\n",
                 __PdoGetName(Pdo),
                 Entry->Name,
                 Version);
            status = Entry->Query(Pdo, Irp);
            goto done;
        }
    }

done:
    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static DECLSPEC_NOINLINE NTSTATUS
PdoQueryCapabilities(
    IN  PXENVKBD_PDO        Pdo,
    IN  PIRP                Irp
    )
{
    PIO_STACK_LOCATION      StackLocation;
    PDEVICE_CAPABILITIES    Capabilities;
    SYSTEM_POWER_STATE      SystemPowerState;
    NTSTATUS                status;

    UNREFERENCED_PARAMETER(Pdo);

    StackLocation = IoGetCurrentIrpStackLocation(Irp);
    Capabilities = StackLocation->Parameters.DeviceCapabilities.Capabilities;

    status = STATUS_INVALID_PARAMETER;
    if (Capabilities->Version != 1)
        goto done;

    Capabilities->DeviceD1 = 0;
    Capabilities->DeviceD2 = 0;
    Capabilities->LockSupported = 0;
    Capabilities->EjectSupported = 1;
    Capabilities->Removable = 1;
    Capabilities->DockDevice = 0;
    Capabilities->UniqueID = 1;
    Capabilities->SilentInstall = 1;
    Capabilities->RawDeviceOK = 0;
    Capabilities->SurpriseRemovalOK = 1;
    Capabilities->HardwareDisabled = 0;
    Capabilities->NoDisplayInUI = 0;

    Capabilities->Address = 0xffffffff;
    Capabilities->UINumber = 0xffffffff;

    for (SystemPowerState = 0; SystemPowerState < PowerSystemMaximum; SystemPowerState++) {
        switch (SystemPowerState) {
        case PowerSystemUnspecified:
        case PowerSystemSleeping1:
        case PowerSystemSleeping2:
            break;

        case PowerSystemWorking:
            Capabilities->DeviceState[SystemPowerState] = PowerDeviceD0;
            break;

        default:
            Capabilities->DeviceState[SystemPowerState] = PowerDeviceD3;
            break;
        }
    }

    Capabilities->SystemWake = PowerSystemUnspecified;
    Capabilities->DeviceWake = PowerDeviceUnspecified;
    Capabilities->D1Latency = 0;
    Capabilities->D2Latency = 0;
    Capabilities->D3Latency = 0;

    status = STATUS_SUCCESS;

done:
    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static DECLSPEC_NOINLINE NTSTATUS
PdoQueryDeviceText(
    IN  PXENVKBD_PDO    Pdo,
    IN  PIRP            Irp
    )
{
    PIO_STACK_LOCATION  StackLocation;
    PWCHAR              Buffer;
    UNICODE_STRING      Text;
    NTSTATUS            status;

    StackLocation = IoGetCurrentIrpStackLocation(Irp);

    switch (StackLocation->Parameters.QueryDeviceText.DeviceTextType) {
    case DeviceTextDescription:
        Trace("DeviceTextDescription\n");
        break;

    case DeviceTextLocationInformation:
        Trace("DeviceTextLocationInformation\n");
        break;

    default:
        Irp->IoStatus.Information = 0;
        status = STATUS_NOT_SUPPORTED;
        goto done;
    }

    Buffer = ExAllocatePoolWithTag(PagedPool, MAXTEXTLEN, 'FIV');

    status = STATUS_NO_MEMORY;
    if (Buffer == NULL)
        goto done;

    RtlZeroMemory(Buffer, MAXTEXTLEN);

    Text.Buffer = Buffer;
    Text.MaximumLength = MAXTEXTLEN;
    Text.Length = 0;

    switch (StackLocation->Parameters.QueryDeviceText.DeviceTextType) {
    case DeviceTextDescription:
        status = RtlStringCbPrintfW(Buffer,
                                    MAXTEXTLEN,
                                    L"%hs %hs",
                                    FdoGetName(__PdoGetFdo(Pdo)),
                                    __PdoGetName(Pdo));
        ASSERT(NT_SUCCESS(status));

        Buffer += wcslen(Buffer);

        break;

    case DeviceTextLocationInformation:
        status = RtlStringCbPrintfW(Buffer,
                                    MAXTEXTLEN,
                                    L"%hs",
                                    __PdoGetName(Pdo));
        ASSERT(NT_SUCCESS(status));

        Buffer += wcslen(Buffer);

        break;

    default:
        ASSERT(FALSE);
        break;
    }

    Text.Length = (USHORT)((ULONG_PTR)Buffer - (ULONG_PTR)Text.Buffer);

    ASSERT3U(KeGetCurrentIrql(), ==, PASSIVE_LEVEL);

    Trace("%s: %wZ\n", __PdoGetName(Pdo), &Text);

    Irp->IoStatus.Information = (ULONG_PTR)Text.Buffer;
    status = STATUS_SUCCESS;

done:
    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static DECLSPEC_NOINLINE NTSTATUS
PdoReadConfig(
    IN  PXENVKBD_PDO    Pdo,
    IN  PIRP            Irp
    )
{
    UNREFERENCED_PARAMETER(Pdo);

    Irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return STATUS_NOT_SUPPORTED;
}

static DECLSPEC_NOINLINE NTSTATUS
PdoWriteConfig(
    IN  PXENVKBD_PDO    Pdo,
    IN  PIRP            Irp
    )
{
    UNREFERENCED_PARAMETER(Pdo);

    Irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return STATUS_NOT_SUPPORTED;
}

#define REGSTR_VAL_MAX_HCID_LEN 1024

static DECLSPEC_NOINLINE NTSTATUS
PdoQueryId(
    IN  PXENVKBD_PDO    Pdo,
    IN  PIRP            Irp
    )
{
    PIO_STACK_LOCATION  StackLocation;
    PWCHAR              Buffer;
    UNICODE_STRING      Id;
    ULONG               Type;
    NTSTATUS            status;

    StackLocation = IoGetCurrentIrpStackLocation(Irp);

    switch (StackLocation->Parameters.QueryId.IdType) {
    case BusQueryInstanceID:
        Trace("BusQueryInstanceID\n");
        Id.MaximumLength = (USHORT)(strlen(__PdoGetName(Pdo)) + 1) * sizeof (WCHAR);
        break;

    case BusQueryDeviceID:
        Trace("BusQueryDeviceID\n");
        Id.MaximumLength = (MAX_DEVICE_ID_LEN - 2) * sizeof (WCHAR);
        break;

    case BusQueryHardwareIDs:
        Trace("BusQueryHardwareIDs\n");
        Id.MaximumLength = (USHORT)(MAX_DEVICE_ID_LEN * ARRAYSIZE(PdoRevision)) * sizeof (WCHAR);
        break;

    case BusQueryCompatibleIDs:
        Trace("BusQueryCompatibleIDs\n");
        Id.MaximumLength = (USHORT)(MAX_DEVICE_ID_LEN * ARRAYSIZE(PdoRevision)) * sizeof (WCHAR);
        break;

    default:
        Irp->IoStatus.Information = 0;
        status = STATUS_NOT_SUPPORTED;
        goto done;
    }

    Buffer = ExAllocatePoolWithTag(PagedPool, Id.MaximumLength, 'FIV');

    status = STATUS_NO_MEMORY;
    if (Buffer == NULL)
        goto done;

    RtlZeroMemory(Buffer, Id.MaximumLength);

    Id.Buffer = Buffer;
    Id.Length = 0;

    switch (StackLocation->Parameters.QueryId.IdType) {
    case BusQueryInstanceID:
        Type = REG_SZ;

        status = RtlStringCbPrintfW(Buffer,
                                    Id.MaximumLength,
                                    L"%hs",
                                    __PdoGetName(Pdo));
        ASSERT(NT_SUCCESS(status));

        Buffer += wcslen(Buffer);

        break;

    case BusQueryDeviceID: {
        ULONG                   Index;
        PXENVKBD_PDO_REVISION    Revision;

        Type = REG_SZ;
        Index = ARRAYSIZE(PdoRevision) - 1;
        Revision = &PdoRevision[Index];

        status = RtlStringCbPrintfW(Buffer,
                                    Id.MaximumLength,
                                    L"XENVKBD\\VEN_%hs&DEV_HID&REV_%08X",
                                    __PdoGetVendorName(Pdo),
                                    Revision->Number);
        ASSERT(NT_SUCCESS(status));

        Buffer += wcslen(Buffer);

        break;
    }
    case BusQueryHardwareIDs:
    case BusQueryCompatibleIDs: {
        LONG    Index;
        ULONG   Length;

        Type = REG_MULTI_SZ;
        Index = ARRAYSIZE(PdoRevision) - 1;

        Length = Id.MaximumLength;

        while (Index >= 0) {
            PXENVKBD_PDO_REVISION    Revision = &PdoRevision[Index];

            status = RtlStringCbPrintfW(Buffer,
                                        Length,
                                        L"XENVKBD\\VEN_%hs&DEV_HID&REV_%08X",
                                        __PdoGetVendorName(Pdo),
                                        Revision->Number);
            ASSERT(NT_SUCCESS(status));

            Buffer += wcslen(Buffer);
            Length -= (ULONG)(wcslen(Buffer) * sizeof (WCHAR));

            Buffer++;
            Length -= sizeof (WCHAR);

            --Index;
        }

        status = RtlStringCbPrintfW(Buffer,
                                    Length,
                                    L"XENDEVICE");
        ASSERT(NT_SUCCESS(status));

        Buffer += wcslen(Buffer);
        Buffer++;

        ASSERT3U((ULONG_PTR)Buffer - (ULONG_PTR)Id.Buffer, <,
                 REGSTR_VAL_MAX_HCID_LEN);
        break;
    }
    default:
        Type = REG_NONE;

        ASSERT(FALSE);
        break;
    }

    ASSERT3U(KeGetCurrentIrql(), ==, PASSIVE_LEVEL);

    Id.Length = (USHORT)((ULONG_PTR)Buffer - (ULONG_PTR)Id.Buffer);
    Buffer = Id.Buffer;

    switch (Type) {
    case REG_SZ:
        Trace("- %ws\n", Buffer);
        break;

    case REG_MULTI_SZ:
        do {
            Trace("- %ws\n", Buffer);
            Buffer += wcslen(Buffer);
            Buffer++;
        } while (*Buffer != L'\0');
        break;

    default:
        ASSERT(FALSE);
        break;
    }

    Irp->IoStatus.Information = (ULONG_PTR)Id.Buffer;
    status = STATUS_SUCCESS;

done:
    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static DECLSPEC_NOINLINE NTSTATUS
PdoQueryBusInformation(
    IN  PXENVKBD_PDO        Pdo,
    IN  PIRP                Irp
    )
{
    PPNP_BUS_INFORMATION    Info;
    NTSTATUS                status;

    UNREFERENCED_PARAMETER(Pdo);

    Info = ExAllocatePoolWithTag(PagedPool, sizeof (PNP_BUS_INFORMATION), 'FIV');

    status = STATUS_NO_MEMORY;
    if (Info == NULL)
        goto done;

    RtlZeroMemory(Info, sizeof (PNP_BUS_INFORMATION));

    Info->BusTypeGuid = GUID_BUS_TYPE_INTERNAL;
    Info->LegacyBusType = PNPBus;
    Info->BusNumber = 0;

    Irp->IoStatus.Information = (ULONG_PTR)Info;
    status = STATUS_SUCCESS;

done:
    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static DECLSPEC_NOINLINE NTSTATUS
PdoDeviceUsageNotification(
    IN  PXENVKBD_PDO    Pdo,
    IN  PIRP            Irp
    )
{
    NTSTATUS            status;

    status = PdoDelegateIrp(Pdo, Irp);

    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static DECLSPEC_NOINLINE NTSTATUS
PdoEject(
    IN  PXENVKBD_PDO    Pdo,
    IN  PIRP            Irp
    )
{
    PXENVKBD_FDO        Fdo = __PdoGetFdo(Pdo);
    NTSTATUS            status;

    Trace("%s\n", __PdoGetName(Pdo));

    FdoAcquireMutex(Fdo);

    __PdoSetDevicePnpState(Pdo, Deleted);
    __PdoSetMissing(Pdo, "device ejected");

    FdoReleaseMutex(Fdo);

    IoInvalidateDeviceRelations(FdoGetPhysicalDeviceObject(Fdo),
                                BusRelations);

    status = STATUS_SUCCESS;

    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

static DECLSPEC_NOINLINE NTSTATUS
PdoDispatchPnp(
    IN  PXENVKBD_PDO    Pdo,
    IN  PIRP            Irp
    )
{
    PIO_STACK_LOCATION  StackLocation;
    UCHAR               MinorFunction;
    NTSTATUS            status;

    StackLocation = IoGetCurrentIrpStackLocation(Irp);
    MinorFunction = StackLocation->MinorFunction;

    Trace("====> (%s) (%02x:%s)\n",
          __PdoGetName(Pdo),
          MinorFunction, 
          PnpMinorFunctionName(MinorFunction));

    switch (StackLocation->MinorFunction) {
    case IRP_MN_START_DEVICE:
        status = PdoStartDevice(Pdo, Irp);
        break;

    case IRP_MN_QUERY_STOP_DEVICE:
        status = PdoQueryStopDevice(Pdo, Irp);
        break;

    case IRP_MN_CANCEL_STOP_DEVICE:
        status = PdoCancelStopDevice(Pdo, Irp);
        break;

    case IRP_MN_STOP_DEVICE:
        status = PdoStopDevice(Pdo, Irp);
        break;

    case IRP_MN_QUERY_REMOVE_DEVICE:
        status = PdoQueryRemoveDevice(Pdo, Irp);
        break;

    case IRP_MN_CANCEL_REMOVE_DEVICE:
        status = PdoCancelRemoveDevice(Pdo, Irp);
        break;

    case IRP_MN_SURPRISE_REMOVAL:
        status = PdoSurpriseRemoval(Pdo, Irp);
        break;

    case IRP_MN_REMOVE_DEVICE:
        status = PdoRemoveDevice(Pdo, Irp);
        break;

    case IRP_MN_QUERY_DEVICE_RELATIONS:
        status = PdoQueryDeviceRelations(Pdo, Irp);
        break;

    case IRP_MN_QUERY_INTERFACE:
        status = PdoQueryInterface(Pdo, Irp);
        break;

    case IRP_MN_QUERY_CAPABILITIES:
        status = PdoQueryCapabilities(Pdo, Irp);
        break;

    case IRP_MN_QUERY_DEVICE_TEXT:
        status = PdoQueryDeviceText(Pdo, Irp);
        break;

    case IRP_MN_READ_CONFIG:
        status = PdoReadConfig(Pdo, Irp);
        break;

    case IRP_MN_WRITE_CONFIG:
        status = PdoWriteConfig(Pdo, Irp);
        break;

    case IRP_MN_QUERY_ID:
        status = PdoQueryId(Pdo, Irp);
        break;

    case IRP_MN_QUERY_BUS_INFORMATION:
        status = PdoQueryBusInformation(Pdo, Irp);
        break;

    case IRP_MN_DEVICE_USAGE_NOTIFICATION:
        status = PdoDeviceUsageNotification(Pdo, Irp);
        break;

    case IRP_MN_EJECT:
        status = PdoEject(Pdo, Irp);
        break;

    default:
        status = Irp->IoStatus.Status;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        break;
    }

    Trace("<==== (%02x:%s)(%08x)\n",
          MinorFunction, 
          PnpMinorFunctionName(MinorFunction),
          status);

    return status;
}

static FORCEINLINE NTSTATUS
__PdoSetDevicePower(
    IN  PXENVKBD_PDO    Pdo,
    IN  PIRP            Irp
    )
{
    PIO_STACK_LOCATION  StackLocation;
    DEVICE_POWER_STATE  DeviceState;
    POWER_ACTION        PowerAction;
    NTSTATUS            status;

    StackLocation = IoGetCurrentIrpStackLocation(Irp);
    DeviceState = StackLocation->Parameters.Power.State.DeviceState;
    PowerAction = StackLocation->Parameters.Power.ShutdownType;

    Trace("====> (%s) (%s:%s)\n",
          __PdoGetName(Pdo),
          PowerDeviceStateName(DeviceState), 
          PowerActionName(PowerAction));

    ASSERT3U(PowerAction, <, PowerActionShutdown);

    if (__PdoGetDevicePowerState(Pdo) > DeviceState) {
        Trace("%s: POWERING UP: %s -> %s\n",
              __PdoGetName(Pdo),
              PowerDeviceStateName(__PdoGetDevicePowerState(Pdo)),
              PowerDeviceStateName(DeviceState));

        ASSERT3U(DeviceState, ==, PowerDeviceD0);
        status = PdoD3ToD0(Pdo);
        ASSERT(NT_SUCCESS(status));
    } else if (__PdoGetDevicePowerState(Pdo) < DeviceState) {
        Trace("%s: POWERING DOWN: %s -> %s\n",
              __PdoGetName(Pdo),
              PowerDeviceStateName(__PdoGetDevicePowerState(Pdo)),
              PowerDeviceStateName(DeviceState));

        ASSERT3U(DeviceState, ==, PowerDeviceD3);
        PdoD0ToD3(Pdo);
    }

    Irp->IoStatus.Status = STATUS_SUCCESS;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    Trace("<==== (%s:%s)\n",
          PowerDeviceStateName(DeviceState), 
          PowerActionName(PowerAction));

    return STATUS_SUCCESS;
}

static NTSTATUS
PdoDevicePower(
    IN  PXENVKBD_THREAD Self,
    IN  PVOID           Context
    )
{
    PXENVKBD_PDO        Pdo = Context;
    PKEVENT             Event;

    Event = ThreadGetEvent(Self);

    for (;;) {
        PIRP    Irp;

        if (Pdo->DevicePowerIrp == NULL) {
            (VOID) KeWaitForSingleObject(Event,
                                         Executive,
                                         KernelMode,
                                         FALSE,
                                         NULL);
            KeClearEvent(Event);
        }

        if (ThreadIsAlerted(Self))
            break;

        Irp = Pdo->DevicePowerIrp;

        if (Irp == NULL)
            continue;

        Pdo->DevicePowerIrp = NULL;
        KeMemoryBarrier();

        (VOID) __PdoSetDevicePower(Pdo, Irp);
    }

    return STATUS_SUCCESS;
}

static FORCEINLINE NTSTATUS
__PdoSetSystemPower(
    IN  PXENVKBD_PDO        Pdo,
    IN  PIRP                Irp
    )
{
    PIO_STACK_LOCATION      StackLocation;
    SYSTEM_POWER_STATE      SystemState;
    POWER_ACTION            PowerAction;

    StackLocation = IoGetCurrentIrpStackLocation(Irp);
    SystemState = StackLocation->Parameters.Power.State.SystemState;
    PowerAction = StackLocation->Parameters.Power.ShutdownType;

    Trace("====> (%s) (%s:%s)\n",
          __PdoGetName(Pdo),
          PowerSystemStateName(SystemState), 
          PowerActionName(PowerAction));

    ASSERT3U(PowerAction, <, PowerActionShutdown);

    if (__PdoGetSystemPowerState(Pdo) > SystemState) {
        if (SystemState < PowerSystemHibernate &&
            __PdoGetSystemPowerState(Pdo) >= PowerSystemHibernate) {
            __PdoSetSystemPowerState(Pdo, PowerSystemHibernate);
            PdoS4ToS3(Pdo);
        }

        Trace("%s: POWERING UP: %s -> %s\n",
              __PdoGetName(Pdo),
              PowerSystemStateName(__PdoGetSystemPowerState(Pdo)),
              PowerSystemStateName(SystemState));
    } else if (__PdoGetSystemPowerState(Pdo) < SystemState) {
        Trace("%s: POWERING DOWN: %s -> %s\n",
              __PdoGetName(Pdo),
              PowerSystemStateName(__PdoGetSystemPowerState(Pdo)),
              PowerSystemStateName(SystemState));

        if (SystemState >= PowerSystemHibernate &&
            __PdoGetSystemPowerState(Pdo) < PowerSystemHibernate) {
            __PdoSetSystemPowerState(Pdo, PowerSystemSleeping3);
            PdoS3ToS4(Pdo);
        }
    }

    __PdoSetSystemPowerState(Pdo, SystemState);

    Irp->IoStatus.Status = STATUS_SUCCESS;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    Trace("<==== (%s:%s)\n",
          PowerSystemStateName(SystemState), 
          PowerActionName(PowerAction));

    return STATUS_SUCCESS;
}

static NTSTATUS
PdoSystemPower(
    IN  PXENVKBD_THREAD Self,
    IN  PVOID           Context
    )
{
    PXENVKBD_PDO        Pdo = Context;
    PKEVENT             Event;

    Event = ThreadGetEvent(Self);

    for (;;) {
        PIRP    Irp;

        if (Pdo->SystemPowerIrp == NULL) {
            (VOID) KeWaitForSingleObject(Event,
                                         Executive,
                                         KernelMode,
                                         FALSE,
                                         NULL);
            KeClearEvent(Event);
        }

        if (ThreadIsAlerted(Self))
            break;

        Irp = Pdo->SystemPowerIrp;

        if (Irp == NULL)
            continue;

        Pdo->SystemPowerIrp = NULL;
        KeMemoryBarrier();

        (VOID) __PdoSetSystemPower(Pdo, Irp);
    }

    return STATUS_SUCCESS;
}

static DECLSPEC_NOINLINE NTSTATUS
PdoSetPower(
    IN  PXENVKBD_PDO    Pdo,
    IN  PIRP            Irp
    )
{
    PIO_STACK_LOCATION  StackLocation;
    POWER_STATE_TYPE    PowerType;
    POWER_ACTION        PowerAction;
    NTSTATUS            status;
    
    StackLocation = IoGetCurrentIrpStackLocation(Irp);
    PowerType = StackLocation->Parameters.Power.Type;
    PowerAction = StackLocation->Parameters.Power.ShutdownType;

    if (PowerAction >= PowerActionShutdown) {
        Irp->IoStatus.Status = STATUS_SUCCESS;

        status = Irp->IoStatus.Status;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);

        goto done;
    }

    switch (PowerType) {
    case DevicePowerState:
        IoMarkIrpPending(Irp);

        ASSERT3P(Pdo->DevicePowerIrp, ==, NULL);
        Pdo->DevicePowerIrp = Irp;
        KeMemoryBarrier();

        ThreadWake(Pdo->DevicePowerThread);

        status = STATUS_PENDING;
        break;

    case SystemPowerState:
        IoMarkIrpPending(Irp);

        ASSERT3P(Pdo->SystemPowerIrp, ==, NULL);
        Pdo->SystemPowerIrp = Irp;
        KeMemoryBarrier();

        ThreadWake(Pdo->SystemPowerThread);

        status = STATUS_PENDING;
        break;

    default:
        status = Irp->IoStatus.Status;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        break;
    }

done:    
    return status;
}

static DECLSPEC_NOINLINE NTSTATUS
PdoQueryPower(
    IN  PXENVKBD_PDO    Pdo,
    IN  PIRP            Irp
    )
{
    NTSTATUS            status;
    
    UNREFERENCED_PARAMETER(Pdo);

    Irp->IoStatus.Status = STATUS_SUCCESS;

    status = Irp->IoStatus.Status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    
    return status;
}

static DECLSPEC_NOINLINE NTSTATUS
PdoDispatchPower(
    IN  PXENVKBD_PDO    Pdo,
    IN  PIRP            Irp
    )
{
    PIO_STACK_LOCATION  StackLocation;
    UCHAR               MinorFunction;
    NTSTATUS            status;

    StackLocation = IoGetCurrentIrpStackLocation(Irp);
    MinorFunction = StackLocation->MinorFunction;

    switch (StackLocation->MinorFunction) {
    case IRP_MN_SET_POWER:
        status = PdoSetPower(Pdo, Irp);
        break;

    case IRP_MN_QUERY_POWER:
        status = PdoQueryPower(Pdo, Irp);
        break;

    default:
        status = Irp->IoStatus.Status;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        break;
    }

    return status;
}

static DECLSPEC_NOINLINE NTSTATUS
PdoDispatchDefault(
    IN  PXENVKBD_PDO    Pdo,
    IN  PIRP            Irp
    )
{
    NTSTATUS            status;

    UNREFERENCED_PARAMETER(Pdo);

    status = Irp->IoStatus.Status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    return status;
}

NTSTATUS
PdoDispatch(
    IN  PXENVKBD_PDO    Pdo,
    IN  PIRP            Irp
    )
{
    PIO_STACK_LOCATION  StackLocation;
    NTSTATUS            status;

    StackLocation = IoGetCurrentIrpStackLocation(Irp);

    switch (StackLocation->MajorFunction) {
    case IRP_MJ_PNP:
        status = PdoDispatchPnp(Pdo, Irp);
        break;

    case IRP_MJ_POWER:
        status = PdoDispatchPower(Pdo, Irp);
        break;

    default:
        status = PdoDispatchDefault(Pdo, Irp);
        break;
    }

    return status;
}

NTSTATUS
PdoResume(
    IN  PXENVKBD_PDO    Pdo
    )
{
    return FrontendResume(__PdoGetFrontend(Pdo));
}

VOID
PdoSuspend(
    IN  PXENVKBD_PDO    Pdo
    )
{
    FrontendSuspend(__PdoGetFrontend(Pdo));
}

NTSTATUS
PdoCreate(
    IN  PXENVKBD_FDO    Fdo,
    IN  PANSI_STRING    Path
    )
{
    PDEVICE_OBJECT      PhysicalDeviceObject;
    PXENVKBD_DX         Dx;
    PXENVKBD_PDO        Pdo;
    NTSTATUS            status;

#pragma prefast(suppress:28197) // Possibly leaking memory 'PhysicalDeviceObject'
    status = IoCreateDevice(DriverGetDriverObject(),
                            sizeof(XENVKBD_DX),
                            NULL,
                            FILE_DEVICE_UNKNOWN,
                            FILE_DEVICE_SECURE_OPEN | FILE_AUTOGENERATED_DEVICE_NAME,
                            FALSE,
                            &PhysicalDeviceObject);
    if (!NT_SUCCESS(status))
        goto fail1;

    Dx = (PXENVKBD_DX)PhysicalDeviceObject->DeviceExtension;
    RtlZeroMemory(Dx, sizeof (XENVKBD_DX));

    Dx->Type = PHYSICAL_DEVICE_OBJECT;
    Dx->DeviceObject = PhysicalDeviceObject;
    Dx->DevicePnpState = Present;
    Dx->SystemPowerState = PowerSystemWorking;
    Dx->DevicePowerState = PowerDeviceD3;

    Pdo = __PdoAllocate(sizeof (XENVKBD_PDO));

    status = STATUS_NO_MEMORY;
    if (Pdo == NULL)
        goto fail2;

    Pdo->Dx = Dx;
    Pdo->Fdo = Fdo;

    status = ThreadCreate(PdoSystemPower, Pdo, &Pdo->SystemPowerThread);
    if (!NT_SUCCESS(status))
        goto fail3;

    status = ThreadCreate(PdoDevicePower, Pdo, &Pdo->DevicePowerThread);
    if (!NT_SUCCESS(status))
        goto fail4;

    __PdoSetName(Pdo, Path->Buffer);

    status = BusInitialize(Pdo, &Pdo->BusInterface);
    if (!NT_SUCCESS(status))
        goto fail5;

    status = HidInitialize(Pdo, &Pdo->HidContext);
    if (!NT_SUCCESS(status))
        goto fail6;

    status = FrontendInitialize(Pdo, &Pdo->Frontend);
    if (!NT_SUCCESS(status))
        goto fail7;

    FdoGetSuspendInterface(Fdo, &Pdo->SuspendInterface);
    FdoGetUnplugInterface(Fdo, &Pdo->UnplugInterface);

    Dx->Pdo = Pdo;

    status = FdoAddPhysicalDeviceObject(Fdo, Pdo);
    if (!NT_SUCCESS(status))
        goto fail8;

    status = STATUS_UNSUCCESSFUL;
    if (__PdoIsEjectRequested(Pdo))
        goto fail9;

    Info("%p (%s)\n",
         PhysicalDeviceObject,
         __PdoGetName(Pdo));

    PdoDumpRevisions(Pdo);

    PhysicalDeviceObject->Flags &= ~DO_DEVICE_INITIALIZING;
    return STATUS_SUCCESS;

fail9:
    Error("fail9\n");

    FdoRemovePhysicalDeviceObject(Fdo, Pdo);

fail8:
    Error("fail8\n");

    (VOID) __PdoClearEjectRequested(Pdo);

    Dx->Pdo = NULL;

    RtlZeroMemory(&Pdo->UnplugInterface,
                  sizeof (XENBUS_UNPLUG_INTERFACE));

    RtlZeroMemory(&Pdo->SuspendInterface,
                  sizeof (XENBUS_SUSPEND_INTERFACE));

    FrontendTeardown(__PdoGetFrontend(Pdo));
    Pdo->Frontend = NULL;

fail7:
    Error("fail7\n");

    HidTeardown(Pdo->HidContext);
    Pdo->HidContext = NULL;

fail6:
    Error("fail6\n");

    BusTeardown(&Pdo->BusInterface);

fail5:
    Error("fail5\n");

    ThreadAlert(Pdo->DevicePowerThread);
    ThreadJoin(Pdo->DevicePowerThread);
    Pdo->DevicePowerThread = NULL;

fail4:
    Error("fail4\n");

    ThreadAlert(Pdo->SystemPowerThread);
    ThreadJoin(Pdo->SystemPowerThread);
    Pdo->SystemPowerThread = NULL;

fail3:
    Error("fail3\n");

    Pdo->Fdo = NULL;
    Pdo->Dx = NULL;

    ASSERT(IsZeroMemory(Pdo, sizeof (XENVKBD_PDO)));
    __PdoFree(Pdo);

fail2:
    Error("fail2\n");

    IoDeleteDevice(PhysicalDeviceObject);

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

VOID
PdoDestroy(
    IN  PXENVKBD_PDO    Pdo
    )
{
    PXENVKBD_DX         Dx = Pdo->Dx;
    PDEVICE_OBJECT      PhysicalDeviceObject = Dx->DeviceObject;
    PXENVKBD_FDO        Fdo = __PdoGetFdo(Pdo);

    Pdo->UnplugRequested = FALSE;

    ASSERT3U(__PdoGetDevicePnpState(Pdo), ==, Deleted);

    ASSERT(__PdoIsMissing(Pdo));
    Pdo->Missing = FALSE;

    Info("%p (%s) (%s)\n",
         PhysicalDeviceObject,
         __PdoGetName(Pdo),
         Pdo->Reason);

    Pdo->Reason = NULL;

    FdoRemovePhysicalDeviceObject(Fdo, Pdo);

    (VOID) __PdoClearEjectRequested(Pdo);

    Dx->Pdo = NULL;

    RtlZeroMemory(&Pdo->UnplugInterface,
                  sizeof (XENBUS_UNPLUG_INTERFACE));

    RtlZeroMemory(&Pdo->SuspendInterface,
                  sizeof (XENBUS_SUSPEND_INTERFACE));

    FrontendTeardown(__PdoGetFrontend(Pdo));
    Pdo->Frontend = NULL;    

    HidTeardown(Pdo->HidContext);
    Pdo->HidContext = NULL;

    BusTeardown(&Pdo->BusInterface);

    ThreadAlert(Pdo->DevicePowerThread);
    ThreadJoin(Pdo->DevicePowerThread);
    Pdo->DevicePowerThread = NULL;

    ThreadAlert(Pdo->SystemPowerThread);
    ThreadJoin(Pdo->SystemPowerThread);
    Pdo->SystemPowerThread = NULL;

    Pdo->Fdo = NULL;
    Pdo->Dx = NULL;

    ASSERT(IsZeroMemory(Pdo, sizeof (XENVKBD_PDO)));
    __PdoFree(Pdo);

    IoDeleteDevice(PhysicalDeviceObject);
}
