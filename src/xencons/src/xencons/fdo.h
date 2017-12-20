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

#ifndef _XENCONS_FDO_H
#define _XENCONS_FDO_H

#include <ntddk.h>
#include <debug_interface.h>
#include <suspend_interface.h>
#include <store_interface.h>
#include <console_interface.h>

#include "driver.h"

extern NTSTATUS
FdoDispatch(
    IN  PXENCONS_FDO    Fdo,
    IN  PIRP            Irp
    );

#define DECLARE_FDO_GET_INTERFACE(_Interface, _Type)    \
extern VOID                                             \
FdoGet ## _Interface ## Interface(                      \
    IN  PXENCONS_FDO Fdo,                               \
    OUT _Type        _Interface ## Interface            \
    );

DECLARE_FDO_GET_INTERFACE(Debug, PXENBUS_DEBUG_INTERFACE)
DECLARE_FDO_GET_INTERFACE(Suspend, PXENBUS_SUSPEND_INTERFACE)
DECLARE_FDO_GET_INTERFACE(Store, PXENBUS_STORE_INTERFACE)
DECLARE_FDO_GET_INTERFACE(Console, PXENBUS_CONSOLE_INTERFACE)

extern NTSTATUS
FdoCreate(
    IN  PDEVICE_OBJECT  PhysicalDeviceObject
    );

extern VOID
FdoDestroy(
    IN  PXENCONS_FDO    Fdo
    );

#endif  // _XENCONS_FDO_H
