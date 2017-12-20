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

#include <ntddk.h>
#include <procgrp.h>
#include <ntstrsafe.h>
#include <stdlib.h>
#include <xen.h>

#include "driver.h"
#include "registry.h"
#include "fdo.h"
#include "pdo.h"
#include "thread.h"
#include "frontend.h"
#include "names.h"
#include "ring.h"
#include "dbg_print.h"
#include "assert.h"
#include "util.h"

#define DOMID_INVALID   (0x7FF4U)

struct _XENVKBD_FRONTEND {
    PXENVKBD_PDO                Pdo;
    PCHAR                       Path;
    XENVKBD_FRONTEND_STATE      State;
    BOOLEAN                     Online;
    KSPIN_LOCK                  Lock;
    PXENVKBD_THREAD             EjectThread;
    KEVENT                      EjectEvent;

    PCHAR                       BackendPath;
    USHORT                      BackendDomain;

    PXENVKBD_RING               Ring;

    XENBUS_SUSPEND_INTERFACE    SuspendInterface;
    XENBUS_STORE_INTERFACE      StoreInterface;

    PXENBUS_SUSPEND_CALLBACK    SuspendCallbackEarly;
    PXENBUS_SUSPEND_CALLBACK    SuspendCallbackLate;
    PXENBUS_STORE_WATCH         Watch;
};

static const PCHAR
FrontendStateName(
    IN  XENVKBD_FRONTEND_STATE  State
    )
{
#define _STATE_NAME(_State)     \
    case  FRONTEND_ ## _State:  \
        return #_State;

    switch (State) {
    _STATE_NAME(UNKNOWN);
    _STATE_NAME(CLOSED);
    _STATE_NAME(PREPARED);
    _STATE_NAME(CONNECTED);
    _STATE_NAME(ENABLED);
    default:
        break;
    }

    return "INVALID";

#undef  _STATE_NAME
}

#define FRONTEND_POOL    'NORF'

static FORCEINLINE PVOID
__FrontendAllocate(
    IN  ULONG   Length
    )
{
    return __AllocatePoolWithTag(NonPagedPool, Length, FRONTEND_POOL);
}

static FORCEINLINE VOID
__FrontendFree(
    IN  PVOID   Buffer
    )
{
    __FreePoolWithTag(Buffer, FRONTEND_POOL);
}

static FORCEINLINE PXENVKBD_PDO
__FrontendGetPdo(
    IN  PXENVKBD_FRONTEND   Frontend
    )
{
    return Frontend->Pdo;
}

PXENVKBD_PDO
FrontendGetPdo(
    IN  PXENVKBD_FRONTEND   Frontend
    )
{
    return __FrontendGetPdo(Frontend);
}

static FORCEINLINE PCHAR
__FrontendGetPath(
    IN  PXENVKBD_FRONTEND   Frontend
    )
{
    return Frontend->Path;
}

PCHAR
FrontendGetPath(
    IN  PXENVKBD_FRONTEND   Frontend
    )
{
    return __FrontendGetPath(Frontend);
}

static FORCEINLINE PCHAR
__FrontendGetBackendPath(
    IN  PXENVKBD_FRONTEND   Frontend
    )
{
    return Frontend->BackendPath;
}

PCHAR
FrontendGetBackendPath(
    IN  PXENVKBD_FRONTEND   Frontend
    )
{
    return __FrontendGetBackendPath(Frontend);
}

static FORCEINLINE USHORT
__FrontendGetBackendDomain(
    IN  PXENVKBD_FRONTEND   Frontend
    )
{
    return Frontend->BackendDomain;
}

USHORT
FrontendGetBackendDomain(
    IN  PXENVKBD_FRONTEND   Frontend
    )
{
    return __FrontendGetBackendDomain(Frontend);
}

#define DEFINE_FRONTEND_GET_FUNCTION(_Function, _Type)  \
static FORCEINLINE _Type                                \
__FrontendGet ## _Function(                             \
    IN  PXENVKBD_FRONTEND   Frontend                    \
    )                                                   \
{                                                       \
    return Frontend-> ## _Function;                     \
}                                                       \
                                                        \
_Type                                                   \
FrontendGet ## _Function(                               \
    IN  PXENVKBD_FRONTEND   Frontend                    \
    )                                                   \
{                                                       \
    return __FrontendGet ## _Function ## (Frontend);    \
}

DEFINE_FRONTEND_GET_FUNCTION(Ring, PXENVKBD_RING)

static BOOLEAN
FrontendIsOnline(
    IN  PXENVKBD_FRONTEND   Frontend
    )
{
    return Frontend->Online;
}

static BOOLEAN
FrontendIsBackendOnline(
    IN  PXENVKBD_FRONTEND   Frontend
    )
{
    PCHAR                   Buffer;
    BOOLEAN                 Online;
    NTSTATUS                status;

    status = XENBUS_STORE(Read,
                          &Frontend->StoreInterface,
                          NULL,
                          __FrontendGetBackendPath(Frontend),
                          "online",
                          &Buffer);
    if (!NT_SUCCESS(status)) {
        Online = FALSE;
    } else {
        Online = (BOOLEAN)strtol(Buffer, NULL, 2);

        XENBUS_STORE(Free,
                     &Frontend->StoreInterface,
                     Buffer);
    }

    return Online;
}

static DECLSPEC_NOINLINE NTSTATUS
FrontendEject(
    IN  PXENVKBD_THREAD Self,
    IN  PVOID           Context
    )
{
    PXENVKBD_FRONTEND    Frontend = Context;
    PKEVENT             Event;

    Trace("%s: ====>\n", __FrontendGetPath(Frontend));

    Event = ThreadGetEvent(Self);

    for (;;) {
        KIRQL       Irql;

        KeWaitForSingleObject(Event,
                              Executive,
                              KernelMode,
                              FALSE,
                              NULL);
        KeClearEvent(Event);

        if (ThreadIsAlerted(Self))
            break;

        KeAcquireSpinLock(&Frontend->Lock, &Irql);

        // It is not safe to use interfaces before this point
        if (Frontend->State == FRONTEND_UNKNOWN ||
            Frontend->State == FRONTEND_CLOSED)
            goto loop;

        if (!FrontendIsOnline(Frontend))
            goto loop;

        if (!FrontendIsBackendOnline(Frontend))
            PdoRequestEject(__FrontendGetPdo(Frontend));

loop:
        KeReleaseSpinLock(&Frontend->Lock, Irql);

        KeSetEvent(&Frontend->EjectEvent, IO_NO_INCREMENT, FALSE);
    }

    KeSetEvent(&Frontend->EjectEvent, IO_NO_INCREMENT, FALSE);

    Trace("%s: <====\n", __FrontendGetPath(Frontend));

    return STATUS_SUCCESS;
}

VOID
FrontendEjectFailed(
    IN PXENVKBD_FRONTEND    Frontend
    )
{
    KIRQL                   Irql;
    ULONG                   Length;
    PCHAR                   Path;
    NTSTATUS                status;

    KeAcquireSpinLock(&Frontend->Lock, &Irql);

    Info("%s: device eject failed\n", __FrontendGetPath(Frontend));

    Length = sizeof ("error/") + (ULONG)strlen(__FrontendGetPath(Frontend));
    Path = __FrontendAllocate(Length);

    status = STATUS_NO_MEMORY;
    if (Path == NULL)
        goto fail1;

    status = RtlStringCbPrintfA(Path, 
                                Length,
                                "error/%s", 
                                __FrontendGetPath(Frontend));
    if (!NT_SUCCESS(status))
        goto fail2;

    (VOID) XENBUS_STORE(Printf,
                        &Frontend->StoreInterface,
                        NULL,
                        Path,
                        "error",
                        "UNPLUG FAILED: device is still in use");

    __FrontendFree(Path);

    KeReleaseSpinLock(&Frontend->Lock, Irql);
    return;

fail2:
    Error("fail2\n");

    __FrontendFree(Path);

fail1:
    Error("fail1 (%08x)\n", status);

    KeReleaseSpinLock(&Frontend->Lock, Irql);
}

static VOID
FrontendSetOnline(
    IN  PXENVKBD_FRONTEND   Frontend
    )
{
    Trace("====>\n");

    Frontend->Online = TRUE;

    Trace("<====\n");
}

static VOID
FrontendSetOffline(
    IN  PXENVKBD_FRONTEND   Frontend
    )
{
    Trace("====>\n");

    Frontend->Online = FALSE;
    PdoRequestEject(__FrontendGetPdo(Frontend));

    Trace("<====\n");
}

static VOID
FrontendSetXenbusState(
    IN  PXENVKBD_FRONTEND   Frontend,
    IN  XenbusState         State
    )
{
    BOOLEAN                 Online;

    Trace("%s: ====> %s\n",
          __FrontendGetPath(Frontend),
          XenbusStateName(State));

    ASSERT(FrontendIsOnline(Frontend));

    Online = FrontendIsBackendOnline(Frontend);

    (VOID) XENBUS_STORE(Printf,
                        &Frontend->StoreInterface,
                        NULL,
                        __FrontendGetPath(Frontend),
                        "state",
                        "%u",
                        State);

    if (State == XenbusStateClosed && !Online)
        FrontendSetOffline(Frontend);

    Trace("%s: <==== %s\n",
          __FrontendGetPath(Frontend),
          XenbusStateName(State));
}

static VOID
FrontendWaitForBackendXenbusStateChange(
    IN      PXENVKBD_FRONTEND   Frontend,
    IN OUT  XenbusState         *State
    )
{
    KEVENT                      Event;
    PXENBUS_STORE_WATCH         Watch;
    LARGE_INTEGER               Start;
    ULONGLONG                   TimeDelta;
    LARGE_INTEGER               Timeout;
    XenbusState                 Old = *State;
    NTSTATUS                    status;

    Trace("%s: ====> %s\n",
          __FrontendGetBackendPath(Frontend),
          XenbusStateName(*State));

    ASSERT(FrontendIsOnline(Frontend));

    KeInitializeEvent(&Event, NotificationEvent, FALSE);

    status = XENBUS_STORE(WatchAdd,
                          &Frontend->StoreInterface,
                          __FrontendGetBackendPath(Frontend),
                          "state",
                          &Event,
                          &Watch);
    if (!NT_SUCCESS(status))
        Watch = NULL;

    KeQuerySystemTime(&Start);
    TimeDelta = 0;

    Timeout.QuadPart = 0;

    while (*State == Old && TimeDelta < 120000) {
        PCHAR           Buffer;
        LARGE_INTEGER   Now;

        if (Watch != NULL) {
            ULONG   Attempt = 0;

            while (++Attempt < 1000) {
                status = KeWaitForSingleObject(&Event,
                                               Executive,
                                               KernelMode,
                                               FALSE,
                                               &Timeout);
                if (status != STATUS_TIMEOUT)
                    break;

                // We are waiting for a watch event at DISPATCH_LEVEL so
                // it is our responsibility to poll the store ring.
                XENBUS_STORE(Poll,
                             &Frontend->StoreInterface);

                KeStallExecutionProcessor(1000);   // 1ms
            }

            KeClearEvent(&Event);
        }

        status = XENBUS_STORE(Read,
                              &Frontend->StoreInterface,
                              NULL,
                              __FrontendGetBackendPath(Frontend),
                              "state",
                              &Buffer);
        if (!NT_SUCCESS(status)) {
            *State = XenbusStateUnknown;
        } else {
            *State = (XenbusState)strtol(Buffer, NULL, 10);

            XENBUS_STORE(Free,
                         &Frontend->StoreInterface,
                         Buffer);
        }

        KeQuerySystemTime(&Now);

        TimeDelta = (Now.QuadPart - Start.QuadPart) / 10000ull;
    }

    if (Watch != NULL)
        (VOID) XENBUS_STORE(WatchRemove,
                            &Frontend->StoreInterface,
                            Watch);

    Trace("%s: <==== (%s)\n",
          __FrontendGetBackendPath(Frontend),
          XenbusStateName(*State));
}

static NTSTATUS
FrontendUpdatePath(
    IN  PXENVKBD_FRONTEND   Frontend
    )
{
    ULONG                   Length;
    PCHAR                   Buffer;
    NTSTATUS                status;

    status = XENBUS_STORE(Read,
                          &Frontend->StoreInterface,
                          NULL,
                          Frontend->Path,
                          "backend-id",
                          &Buffer);
    if (NT_SUCCESS(status)) {
        Frontend->BackendDomain = (USHORT)strtoul(Buffer, NULL, 10);

        XENBUS_STORE(Free,
                     &Frontend->StoreInterface,
                     Buffer);
    } else {
        Frontend->BackendDomain = 0;
    }

    status = XENBUS_STORE(Read,
                          &Frontend->StoreInterface,
                          NULL,
                          Frontend->Path,
                          "backend",
                          &Buffer);
    if (!NT_SUCCESS(status))
        goto fail1;

    Length = (ULONG)strlen(Buffer);

    status = STATUS_NO_MEMORY;
    if (Frontend->BackendPath)
        __FrontendFree(Frontend->BackendPath);

    Frontend->BackendPath = __FrontendAllocate((Length + 1) * sizeof(CHAR));
    if (Frontend->BackendPath == NULL)
        goto fail2;

    RtlCopyMemory(Frontend->BackendPath,
                  Buffer,
                  Length * sizeof(CHAR));

    XENBUS_STORE(Free,
                 &Frontend->StoreInterface,
                 Buffer);

    return STATUS_SUCCESS;

fail2:
    Error("fail2\n");

    XENBUS_STORE(Free,
                 &Frontend->StoreInterface,
                 Buffer);

fail1:
    Error("fail1 %08x\n", status);
    return status;
}

static NTSTATUS
FrontendClose(
    IN  PXENVKBD_FRONTEND   Frontend
    )
{
    XenbusState             State;
    NTSTATUS                status;

    Trace("=====>\n");
    if (Frontend->Watch)
        XENBUS_STORE(WatchRemove,
                     &Frontend->StoreInterface,
                     Frontend->Watch);
    Frontend->Watch = NULL;

    status = FrontendUpdatePath(Frontend);
    if (!NT_SUCCESS(status))
        goto fail1;

    State = XenbusStateUnknown;
    do {
        FrontendWaitForBackendXenbusStateChange(Frontend,
                                                &State);
        if (State == XenbusStateUnknown)
            goto fail2;
    } while (State == XenbusStateInitialising);

    FrontendSetXenbusState(Frontend, XenbusStateClosing);

    status = STATUS_UNSUCCESSFUL;
    do {
        FrontendWaitForBackendXenbusStateChange(Frontend,
                                                &State);
        if (State == XenbusStateUnknown)
            goto fail3;
    } while (State != XenbusStateClosing &&
             State != XenbusStateClosed);

    FrontendSetXenbusState(Frontend, XenbusStateClosed);

    do {
        FrontendWaitForBackendXenbusStateChange(Frontend,
                                                &State);
        if (State == XenbusStateUnknown)
            goto fail4;
    } while (State != XenbusStateClosed);

    __FrontendFree(Frontend->BackendPath);
    Frontend->BackendPath = NULL;
    Frontend->BackendDomain = DOMID_INVALID;

    Trace("<=====\n");
    return STATUS_SUCCESS;

fail4:
    Error("fail4\n");
fail3:
    Error("fail3\n");
fail2:
    Error("fail2\n");
fail1:
    Error("fail1 %08x\n", status);
    return status;
}

static NTSTATUS
FrontendPrepare(
    IN  PXENVKBD_FRONTEND   Frontend
    )
{
    XenbusState             State;
    NTSTATUS                status;

    Trace("=====>\n");

    status = FrontendUpdatePath(Frontend);
    if (!NT_SUCCESS(status))
        goto fail1;

    status = XENBUS_STORE(WatchAdd,
                          &Frontend->StoreInterface,
                          NULL,
                          Frontend->BackendPath,
                          ThreadGetEvent(Frontend->EjectThread),
                          &Frontend->Watch);
    if (!NT_SUCCESS(status))
        goto fail2;

    FrontendSetXenbusState(Frontend, XenbusStateInitialising);

    status = STATUS_UNSUCCESSFUL;
    do {
        FrontendWaitForBackendXenbusStateChange(Frontend,
                                                &State);
        if (State == XenbusStateUnknown)
            goto fail3;
    } while (State != XenbusStateInitWait);

    Trace("<=====\n");
    return STATUS_SUCCESS;

fail3:
    Error("fail3\n");
    XENBUS_STORE(WatchRemove,
                 &Frontend->StoreInterface,
                 Frontend->Watch);
    Frontend->Watch = NULL;
fail2:
    Error("fail2\n");
fail1:
    Error("fail1 %08x\n", status);
    return status;
}

static NTSTATUS
FrontendConnect(
    IN  PXENVKBD_FRONTEND   Frontend
    )
{
    XenbusState             State;
    NTSTATUS                status;

    Trace("=====>\n");

    status = RingConnect(Frontend->Ring);
    if (!NT_SUCCESS(status))
        goto fail1;

    for (;;) {
        PXENBUS_STORE_TRANSACTION   Transaction;

        status = XENBUS_STORE(TransactionStart,
                              &Frontend->StoreInterface,
                              &Transaction);
        if (!NT_SUCCESS(status))
            break;

        status = RingStoreWrite(Frontend->Ring,
                                Transaction);
        if (!NT_SUCCESS(status))
            goto abort;

        status = XENBUS_STORE(TransactionEnd,
                              &Frontend->StoreInterface,
                              Transaction,
                              TRUE);
        if (status == STATUS_RETRY)
            continue;
        break;

abort:
        (VOID) XENBUS_STORE(TransactionEnd,
                            &Frontend->StoreInterface,
                            Transaction,
                            FALSE);
        break;
    }
    if (!NT_SUCCESS(status))
        goto fail2;

    FrontendSetXenbusState(Frontend, XenbusStateInitialised);

    status = STATUS_UNSUCCESSFUL;
    do {
        FrontendWaitForBackendXenbusStateChange(Frontend,
                                                &State);
        if (State == XenbusStateUnknown)
            goto fail3;
    } while (State == XenbusStateInitWait ||
             State == XenbusStateInitialising ||
             State == XenbusStateInitialised);

    if (State != XenbusStateConnected)
        goto fail4;

    FrontendSetXenbusState(Frontend, XenbusStateConnected);

    Trace("<=====\n");
    return STATUS_SUCCESS;

fail4:
    Error("fail4\n");
fail3:
    Error("fail3\n");
fail2:
    Error("fail2\n");
fail1:
    Error("fail1 %08x\n", status);
    return status;
}

static VOID
FrontendDisconnect(
    IN  PXENVKBD_FRONTEND   Frontend
    )
{
    Trace("====>\n");

    RingDisconnect(__FrontendGetRing(Frontend));

    Trace("<====\n");
}

static NTSTATUS
FrontendEnable(
    IN  PXENVKBD_FRONTEND   Frontend
    )
{
    NTSTATUS                status;

    Trace("====>\n");

    status = RingEnable(__FrontendGetRing(Frontend));
    if (!NT_SUCCESS(status))
        goto fail1;

    Trace("<====\n");
    return STATUS_SUCCESS;

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

static VOID
FrontendDisable(
    IN  PXENVKBD_FRONTEND    Frontend
    )
{
    Trace("====>\n");

    RingDisable(__FrontendGetRing(Frontend));

    Trace("<====\n");
}

NTSTATUS
FrontendSetState(
    IN  PXENVKBD_FRONTEND       Frontend,
    IN  XENVKBD_FRONTEND_STATE  State
    )
{
    NTSTATUS                    status;
    KIRQL                       Irql;

    KeAcquireSpinLock(&Frontend->Lock, &Irql);

    Info("%s: ====> '%s' -> '%s'\n",
         __FrontendGetPath(Frontend),
         FrontendStateName(Frontend->State),
         FrontendStateName(State));

    status = STATUS_SUCCESS;
    while (Frontend->State != State && NT_SUCCESS(status)) {
        switch (Frontend->State) {
        case FRONTEND_UNKNOWN:
            switch (State) {
            case FRONTEND_CLOSING:
            case FRONTEND_CLOSED:
            case FRONTEND_PREPARED:
            case FRONTEND_CONNECTED:
            case FRONTEND_ENABLED:
                status = FrontendClose(Frontend);
                if (NT_SUCCESS(status)) {
                    Frontend->State = FRONTEND_CLOSED;
                }
                break;

            default:
                ASSERT(FALSE);
                break;
            }
            break;

        case FRONTEND_CLOSING:
            switch (State) {
            case FRONTEND_UNKNOWN:
            case FRONTEND_CLOSED:
            case FRONTEND_PREPARED:
            case FRONTEND_CONNECTED:
            case FRONTEND_ENABLED:
                FrontendDisconnect(Frontend);
                Frontend->State = FRONTEND_CLOSED;
                break;
            default:
                ASSERT(FALSE);
                break;
            }
            break;

        case FRONTEND_CLOSED:
            switch (State) {
            case FRONTEND_PREPARED:
            case FRONTEND_CONNECTED:
            case FRONTEND_ENABLED:
                status = FrontendPrepare(Frontend);
                if (NT_SUCCESS(status)) {
                    Frontend->State = FRONTEND_PREPARED;
                } else {
                    FrontendClose(Frontend);
                    Frontend->State = FRONTEND_CLOSED;
                }
                break;
            case FRONTEND_UNKNOWN:
                Frontend->State = FRONTEND_UNKNOWN;
                break;
            default:
                ASSERT(FALSE);
                break;
            }
            break;

        case FRONTEND_PREPARED:
            switch (State) {
            case FRONTEND_CLOSING:
            case FRONTEND_CLOSED:
                status = FrontendClose(Frontend);
                if (NT_SUCCESS(status))
                    Frontend->State = FRONTEND_CLOSED;
                break;
            case FRONTEND_CONNECTED:
            case FRONTEND_ENABLED:
                status = FrontendConnect(Frontend);
                if (NT_SUCCESS(status)) {
                    Frontend->State = FRONTEND_CONNECTED;
                } else {
                    FrontendClose(Frontend);
                    Frontend->State = FRONTEND_CLOSED;
                }
                break;
            default:
                ASSERT(FALSE);
                break;
            }
            break;

        case FRONTEND_CONNECTED:
            switch (State) {
            case FRONTEND_ENABLED:
                FrontendEnable(Frontend);
                Frontend->State = FRONTEND_ENABLED;
                break;
            case FRONTEND_CLOSING:
            case FRONTEND_CLOSED:
                FrontendClose(Frontend);
                Frontend->State = FRONTEND_CLOSING;
                break;
            default:
                ASSERT(FALSE);
                break;
            }
            break;

        case FRONTEND_ENABLED:
            switch (State) {
            case FRONTEND_CLOSING:
            case FRONTEND_CLOSED:
            case FRONTEND_CONNECTED:
                FrontendDisable(Frontend);
                Frontend->State = FRONTEND_CONNECTED;
                break;
            default:
                ASSERT(FALSE);
                break;
            }
            break;

        default:
            ASSERT(FALSE);
            break;
        }

        Info("%s in state '%s'\n",
             __FrontendGetPath(Frontend),
             FrontendStateName(Frontend->State));
    }

    KeReleaseSpinLock(&Frontend->Lock, Irql);

    Info("%s: <=====\n", __FrontendGetPath(Frontend));

    return status;
}

static FORCEINLINE VOID
__FrontendResume(
    IN  PXENVKBD_FRONTEND   Frontend
    )
{
    ASSERT3U(KeGetCurrentIrql(), ==, DISPATCH_LEVEL);

    ASSERT3U(Frontend->State, ==, FRONTEND_UNKNOWN);
    (VOID) FrontendSetState(Frontend, FRONTEND_CLOSED);
}

static FORCEINLINE VOID
__FrontendSuspend(
    IN  PXENVKBD_FRONTEND   Frontend
    )
{
    ASSERT3U(KeGetCurrentIrql(), ==, DISPATCH_LEVEL);

    (VOID) FrontendSetState(Frontend, FRONTEND_UNKNOWN);
}

static DECLSPEC_NOINLINE VOID
FrontendSuspendCallbackEarly(
    IN  PVOID           Argument
    )
{
    PXENVKBD_FRONTEND   Frontend = Argument;

    Frontend->Online = FALSE;
}

static DECLSPEC_NOINLINE VOID
FrontendSuspendCallbackLate(
    IN  PVOID           Argument
    )
{
    PXENVKBD_FRONTEND   Frontend = Argument;

    __FrontendSuspend(Frontend);
    __FrontendResume(Frontend);
}

NTSTATUS
FrontendResume(
    IN  PXENVKBD_FRONTEND   Frontend
    )
{
    KIRQL                   Irql;
    NTSTATUS                status;

    Trace("====>\n");

    KeRaiseIrql(DISPATCH_LEVEL, &Irql);

    status = XENBUS_SUSPEND(Acquire, &Frontend->SuspendInterface);
    if (!NT_SUCCESS(status))
        goto fail1;

    __FrontendResume(Frontend);

    status = XENBUS_SUSPEND(Register,
                            &Frontend->SuspendInterface,
                            SUSPEND_CALLBACK_EARLY,
                            FrontendSuspendCallbackEarly,
                            Frontend,
                            &Frontend->SuspendCallbackEarly);
    if (!NT_SUCCESS(status))
        goto fail2;

    status = XENBUS_SUSPEND(Register,
                            &Frontend->SuspendInterface,
                            SUSPEND_CALLBACK_LATE,
                            FrontendSuspendCallbackLate,
                            Frontend,
                            &Frontend->SuspendCallbackLate);
    if (!NT_SUCCESS(status))
        goto fail3;

    KeLowerIrql(Irql);

    KeClearEvent(&Frontend->EjectEvent);
    ThreadWake(Frontend->EjectThread);

    Trace("waiting for eject thread\n");

    (VOID) KeWaitForSingleObject(&Frontend->EjectEvent,
                                 Executive,
                                 KernelMode,
                                 FALSE,
                                 NULL);

    Trace("<====\n");

    return STATUS_SUCCESS;
    
fail3:
    Error("fail3\n");

    XENBUS_SUSPEND(Deregister,
                   &Frontend->SuspendInterface,
                   Frontend->SuspendCallbackEarly);
    Frontend->SuspendCallbackEarly = NULL;

fail2:
    Error("fail2\n");

    __FrontendSuspend(Frontend);

    XENBUS_SUSPEND(Release, &Frontend->SuspendInterface);

fail1:
    Error("fail1 (%08x)\n", status);

    KeLowerIrql(Irql);

    return status;
}

VOID
FrontendSuspend(
    IN  PXENVKBD_FRONTEND   Frontend
    )
{
    KIRQL                   Irql;

    Trace("====>\n");

    KeRaiseIrql(DISPATCH_LEVEL, &Irql);

    XENBUS_SUSPEND(Deregister,
                   &Frontend->SuspendInterface,
                   Frontend->SuspendCallbackLate);
    Frontend->SuspendCallbackLate = NULL;

    XENBUS_SUSPEND(Deregister,
                   &Frontend->SuspendInterface,
                   Frontend->SuspendCallbackEarly);
    Frontend->SuspendCallbackEarly = NULL;

    __FrontendSuspend(Frontend);

    XENBUS_SUSPEND(Release, &Frontend->SuspendInterface);

    KeLowerIrql(Irql);

    KeClearEvent(&Frontend->EjectEvent);
    ThreadWake(Frontend->EjectThread);

    Trace("waiting for eject thread\n");

    (VOID) KeWaitForSingleObject(&Frontend->EjectEvent,
                                 Executive,
                                 KernelMode,
                                 FALSE,
                                 NULL);

    Trace("<====\n");
}

__drv_requiresIRQL(PASSIVE_LEVEL)
NTSTATUS
FrontendInitialize(
    IN  PXENVKBD_PDO        Pdo,
    OUT PXENVKBD_FRONTEND   *Frontend
    )
{
    PCHAR                   Name;
    ULONG                   Length;
    PCHAR                   Path;
    NTSTATUS                status;

    Trace("====>\n");

    ASSERT3U(KeGetCurrentIrql(), ==, PASSIVE_LEVEL);

    Name = PdoGetName(Pdo);

    Length = sizeof ("devices/vkbd/") + (ULONG)strlen(Name);
    Path = __FrontendAllocate(Length);

    status = STATUS_NO_MEMORY;
    if (Path == NULL)
        goto fail1;

    status = RtlStringCbPrintfA(Path, 
                                Length,
                                "device/vkbd/%s", 
                                Name);
    if (!NT_SUCCESS(status))
        goto fail2;

    *Frontend = __FrontendAllocate(sizeof (XENVKBD_FRONTEND));

    status = STATUS_NO_MEMORY;
    if (*Frontend == NULL)
        goto fail3;

    (*Frontend)->Pdo = Pdo;
    (*Frontend)->Path = Path;
    (*Frontend)->BackendDomain = DOMID_INVALID;

    KeInitializeSpinLock(&(*Frontend)->Lock);

    (*Frontend)->Online = TRUE;

    FdoGetSuspendInterface(PdoGetFdo(Pdo), &(*Frontend)->SuspendInterface);
    FdoGetStoreInterface(PdoGetFdo(Pdo), &(*Frontend)->StoreInterface);

    status = RingInitialize(*Frontend, &(*Frontend)->Ring);
    if (!NT_SUCCESS(status))
        goto fail4;

    KeInitializeEvent(&(*Frontend)->EjectEvent, NotificationEvent, FALSE);

    status = ThreadCreate(FrontendEject, *Frontend, &(*Frontend)->EjectThread);
    if (!NT_SUCCESS(status))
        goto fail5;

    Trace("<====\n");

    return STATUS_SUCCESS;

fail5:
    Error("fail5\n");

    RtlZeroMemory(&(*Frontend)->EjectEvent, sizeof (KEVENT));

    RingTeardown(__FrontendGetRing(*Frontend));
    (*Frontend)->Ring = NULL;

fail4:
    Error("fail4\n");

    RtlZeroMemory(&(*Frontend)->StoreInterface,
                  sizeof (XENBUS_STORE_INTERFACE));

    RtlZeroMemory(&(*Frontend)->SuspendInterface,
                  sizeof (XENBUS_SUSPEND_INTERFACE));

    (*Frontend)->Online = FALSE;

    RtlZeroMemory(&(*Frontend)->Lock, sizeof (KSPIN_LOCK));

    (*Frontend)->BackendDomain = 0;
    (*Frontend)->Path = NULL;
    (*Frontend)->Pdo = NULL;

    ASSERT(IsZeroMemory(*Frontend, sizeof (XENVKBD_FRONTEND)));

    __FrontendFree(*Frontend);
    *Frontend = NULL;

fail3:
    Error("fail3\n");

fail2:
    Error("fail2\n");

    __FrontendFree(Path);

fail1:
    Error("fail1 (%08x)\n", status);

    return status;
}

VOID
FrontendTeardown(
    IN  PXENVKBD_FRONTEND   Frontend
    )
{
    Trace("====>\n");

    ASSERT3U(KeGetCurrentIrql(), ==, PASSIVE_LEVEL);

    ASSERT(Frontend->State == FRONTEND_UNKNOWN);

    ThreadAlert(Frontend->EjectThread);
    ThreadJoin(Frontend->EjectThread);
    Frontend->EjectThread = NULL;

    RtlZeroMemory(&Frontend->EjectEvent, sizeof (KEVENT));

    RingTeardown(__FrontendGetRing(Frontend));
    Frontend->Ring = NULL;

    RtlZeroMemory(&Frontend->StoreInterface,
                  sizeof (XENBUS_STORE_INTERFACE));

    RtlZeroMemory(&Frontend->SuspendInterface,
                  sizeof (XENBUS_SUSPEND_INTERFACE));

    Frontend->Online = FALSE;

    RtlZeroMemory(&Frontend->Lock, sizeof (KSPIN_LOCK));

    Frontend->BackendDomain = 0;

    __FrontendFree(Frontend->Path);
    Frontend->Path = NULL;

    Frontend->Pdo = NULL;

    ASSERT(IsZeroMemory(Frontend, sizeof (XENVKBD_FRONTEND)));

    __FrontendFree(Frontend);

    Trace("<====\n");
}
