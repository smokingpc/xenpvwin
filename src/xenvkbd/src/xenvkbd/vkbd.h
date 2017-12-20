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

#ifndef _XENVKBD_VKBD_H
#define _XENVKBD_VKBD_H

#include <ntddk.h>
#include <hidport.h>

typedef struct _XENVKBD_HID_KEYBOARD {
    UCHAR   ReportId; // = 1
    UCHAR   Modifiers;
    UCHAR   Keys[6];
} XENVKBD_HID_KEYBOARD;

typedef struct _XENVKBD_HID_ABSMOUSE {
    UCHAR   ReportId; // = 2
    UCHAR   Buttons;
    USHORT  X;
    USHORT  Y;
    CHAR    dZ;
} XENVKBD_HID_ABSMOUSE;

static const UCHAR VkbdReportDescriptor[] = {
    /* ReportId 1 : Keyboard                                               */
    0x05, 0x01,         /* USAGE_PAGE (Generic Desktop)                    */
    0x09, 0x06,         /* USAGE (Keyboard 6)                              */
    0xa1, 0x01,         /* COLLECTION (Application)                        */
    0x85, 0x01,         /*   REPORT_ID (1)                                 */
    0x05, 0x07,         /*   USAGE_PAGE (Keyboard)                         */
    0x19, 0xe0,         /*   USAGE_MINIMUM (Keyboard LeftControl)          */
    0x29, 0xe7,         /*   USAGE_MAXIMUM (Keyboard Right GUI)            */
    0x15, 0x00,         /*   LOGICAL_MINIMUM (0)                           */
    0x25, 0x01,         /*   LOGICAL_MAXIMUM (1)                           */
    0x75, 0x01,         /*   REPORT_SIZE (1)                               */
    0x95, 0x08,         /*   REPORT_COUNT (8)                              */
    0x81, 0x02,         /*   INPUT (Data,Var,Abs)                          */
    0x95, 0x06,         /*   REPORT_COUNT (6)                              */
    0x75, 0x08,         /*   REPORT_SIZE (8)                               */
    0x15, 0x00,         /*   LOGICAL_MINIMUM (0)                           */
    0x25, 0x65,         /*   LOGICAL_MAXIMUM (101)                         */
    0x05, 0x07,         /*   USAGE_PAGE (Keyboard)                         */
    0x19, 0x00,         /*   USAGE_MINIMUM (Reserved (no event indicated)) */
    0x29, 0x65,         /*   USAGE_MAXIMUM (Keyboard Application)          */
    0x81, 0x00,         /*   INPUT (Data,Ary,Abs)                          */
    0xc0,               /* END_COLLECTION                                  */
    /* Report Id 2 : Absolute Mouse                                        */
    0x05, 0x01,         /* USAGE_PAGE (Generic Desktop)                    */
    0x09, 0x02,         /* USAGE (Mouse 2)                                 */
    0xa1, 0x01,         /* COLLECTION (Application)                        */
    0x85, 0x02,         /*   REPORT_ID (2)                                 */
    0x09, 0x01,         /*   USAGE (Pointer)                               */
    0xa1, 0x00,         /*   COLLECTION (Physical)                         */
    0x05, 0x09,         /*     USAGE_PAGE (Button)                         */
    0x19, 0x01,         /*     USAGE_MINIMUM (Button 1)                    */
    0x29, 0x05,         /*     USAGE_MAXIMUM (Button 5)                    */
    0x15, 0x00,         /*     LOGICAL_MINIMUM (0)                         */
    0x25, 0x01,         /*     LOGICAL_MAXIMUM (1)                         */
    0x95, 0x05,         /*     REPORT_COUNT (5)                            */
    0x75, 0x01,         /*     REPORT_SIZE (1)                             */
    0x81, 0x02,         /*     INPUT (Data,Var,Abs)                        */
    0x95, 0x01,         /*     REPORT_COUNT (1)                            */
    0x75, 0x03,         /*     REPORT_SIZE (3)                             */
    0x81, 0x03,         /*     INPUT (Cnst,Var,Abs)                        */
    0x05, 0x01,         /*     USAGE_PAGE (Generic Desktop)                */
    0x09, 0x30,         /*     USAGE (X)                                   */
    0x09, 0x31,         /*     USAGE (Y)                                   */
    0x16, 0x00, 0x00,   /*     LOGICAL_MINIMUM (0)                         */
    0x26, 0xff, 0x7f,   /*     LOGICAL_MAXIMUM (32767)                     */
    0x75, 0x10,         /*     REPORT_SIZE (16)                            */
    0x95, 0x02,         /*     REPORT_COUNT (2)                            */
    0x81, 0x02,         /*     INPUT (Data,Var,Abs)                        */
    0x09, 0x38,         /*     USAGE (Z)                                   */
    0x15, 0x81,         /*     LOGICAL_MINIMUM (-127)                      */
    0x25, 0x7f,         /*     LOGICAL_MAXIMUM (127)                       */
    0x75, 0x08,         /*     REPORT_SIZE (8)                             */
    0x95, 0x01,         /*     REPORT_COUNT (1)                            */
    0x81, 0x06,         /*     INPUT (Data,Var,Rel)                        */
    0xc0,               /*   END_COLLECTION                                */
    0xc0                /* END_COLLECTION                                  */

};

static const HID_DESCRIPTOR VkbdDeviceDescriptor = {
    sizeof(HID_DESCRIPTOR),
    0x09,
    0x0101,
    0x00,
    0x01,
    { 0x22, sizeof(VkbdReportDescriptor) }
};

static const HID_DEVICE_ATTRIBUTES VkbdDeviceAttributes = {
    sizeof(HID_DEVICE_ATTRIBUTES),
    0xF001, // Random Vendor ID - this may need changing to a valid USBIF designation
    0xF001, // Random Product ID
    0x0101
};

static const USHORT VkbdKeyCodeToUsage[] = {
    0x00, // KEY_RESERVED
    0x29, // KEY_ESC
    0x1E, // KEY_1
    0x1F, // KEY_2
    0x20, // KEY_3
    0x21, // KEY_4
    0x22, // KEY_5
    0x23, // KEY_6
    0x24, // KEY_7
    0x25, // KEY_8
    0x26, // KEY_9
    0x27, // KEY_0
    0x2D, // KEY_MINUS
    0x2E, // KEY_EQUAL
    0x2A, // KEY_BACKSPACE
    0x2B, // KEY_TAB
    0x14, // KEY_Q
    0x1A, // KEY_W
    0x08, // KEY_E
    0x15, // KEY_R
    0x17, // KEY_T
    0x1C, // KEY_Y
    0x18, // KEY_U
    0x0C, // KEY_I
    0x12, // KEY_O
    0x13, // KEY_P
    0x2F, // KEY_LEFTBRACE
    0x30, // KEY_RIGHTBRACE
    0x29, // KEY_ENTER
    0xE0, // KEY_LEFTCTRL
    0x04, // KEY_A
    0x16, // KEY_S
    0x07, // KEY_D
    0x09, // KEY_F
    0x0A, // KEY_G
    0x0B, // KEY_H
    0x0D, // KEY_J
    0x0E, // KEY_K
    0x0F, // KEY_L
    0x33, // KEY_SEMICOLON
    0x24, // KEY_APOSTROPHE
    0x35, // KEY_GRAVE
    0xE1, // KEY_LEFTSHIFT
    0x31, // KEY_BACKSLASH
    0x1D, // KEY_Z
    0x1B, // KEY_X
    0x06, // KEY_C
    0x19, // KEY_V
    0x05, // KEY_B
    0x11, // KEY_N
    0x10, // KEY_M
    0x36, // KEY_COMMA
    0x37, // KEY_DOT
    0x38, // KEY_SLASH
    0xE5, // KEY_RIGHTSHIFT
    0x55, // KEY_KPASTERISK
    0xE2, // KEY_LEFTALT
    0x2C, // KEY_SPACE
    0x39, // KEY_CAPSLOCK
    0x3A, // KEY_F1
    0x3B, // KEY_F2
    0x3C, // KEY_F3
    0x3D, // KEY_F4
    0x3E, // KEY_F5
    0x3F, // KEY_F6
    0x40, // KEY_F7
    0x41, // KEY_F8
    0x42, // KEY_F9
    0x43, // KEY_F10
    0x53, // KEY_NUMLOCK
    0x47, // KEY_SCROLLLOCK
    0x5F, // KEY_KP7
    0x60, // KEY_KP8
    0x61, // KEY_KP9
    0x56, // KEY_KPMINUS
    0x5C, // KEY_KP4
    0x5D, // KEY_KP5
    0x5E, // KEY_KP6
    0x57, // KEY_KPPLUS
    0x59, // KEY_KP1
    0x5A, // KEY_KP2
    0x5B, // KEY_KP3
    0x62, // KEY_KP0
    0x63, // KEY_KPDOT
    0x00, // gap in sequence
    0x8F, // KEY_ZENKAKUHANKAKU
    0x64, // KEY_102ND
    0x44, // KEY_F11
    0x45, // KEY_F12
    0x87, // KEY_RO
    0x88, // KEY_KATAKANA
    0x8A, // KEY_HIRAGANA
    0x8B, // KEY_HENKAN
    0x8C, // KEY_KATAKANAHIRAGANA
    0x8D, // KEY_MUHENKAN
    0x8E, // KEY_KPJPCOMMA
    0x58, // KEY_KPENTER
    0xE4, // KEY_RIGHTCTRL
    0x54, // KEY_KPSLASH
    0x48, // KEY_SYSRQ
    0xE6, // KEY_RIGHTALT
    0x00, // gap in sequence
    0x4A, // KEY_HOME
    0x52, // KEY_UP
    0x4B, // KEY_PAGEUP
    0x50, // KEY_LEFT
    0x4F, // KEY_RIGHT
    0x4D, // KEY_END
    0x51, // KEY_DOWN
    0x4E, // KEY_PAGEDOWN
    0x49, // KEY_INSERT
    0x4D, // KEY_DELETE
    0x00, // gap in sequence
    0x7F, // KEY_MUTE
    0x81, // KEY_VOLUMEDOWN
    0x80, // KEY_VOLUMEUP
    0x66, // KEY_POWER
    0x67, // KEY_KPEQUAL
    0x00, // KEY_KPPLUSMINUS
    0x00, // gap in sequence
    0x00, // gap in sequence
    0x00, // gap in sequence
    0x85, // KEY_KPCOMMA
    0x90, // KEY_HANGEUL
    0x91, // KEY_HANJA
    0x89, // KEY_YEN
    0xE3, // KEY_LEFTMETA
    0xE7, // KEY_RIGHTMETA
};

#endif  // _XENVKBD_VKBD_H
