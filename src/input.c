/*
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 *
 * Paulo Zanoni <pzanoni@mandriva.com>
 * Tuan Bui <tuanbui918@gmail.com>
 * Colin Cornaby <colin.cornaby@mac.com>
 * Timothy Fleck <tim.cs.pdx@gmail.com>
 * Colin Hill <colin.james.hill@gmail.com>
 * Weseung Hwang <weseung@gmail.com>
 * Nathaniel Way <nathanielcw@hotmail.com>
 */

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <xorg-server.h>
#include <fb.h>
#include <micmap.h>
#include <mipointer.h>
#include <shadow.h>
#include <xf86.h>
#include <xf86Module.h>
#include <xf86str.h>
#include <xf86_OSproc.h>

#include "config.h"

#include "client.h"
#include "input.h"

#define SYSCALL(call) while (((call) == -1) && (errno == EINTR))

#define NUM_MOUSE_BUTTONS 6
#define NUM_MOUSE_AXES 2

static pointer
NestedInputPlug(pointer module, pointer options, int *errmaj, int  *errmin);
static void
NestedInputUnplug(pointer p);

static void
NestedInputReadInput(InputInfoPtr pInfo);
static int
NestedInputControl(DeviceIntPtr    device,int what);

static int
_nested_input_init_keyboard(DeviceIntPtr device);
static int
_nested_input_init_buttons(DeviceIntPtr device);
static int
_nested_input_init_axes(DeviceIntPtr device);

typedef struct _NestedInputDeviceRec {
    NestedClientPrivatePtr clientData;
    int   version;
} NestedInputDeviceRec, *NestedInputDevicePtr;

static XF86ModuleVersionInfo NestedInputVersionRec = {
    "nestedinput",
    MODULEVENDORSTRING,
    MODINFOSTRING1,
    MODINFOSTRING2,
    XORG_VERSION_CURRENT,
    PACKAGE_VERSION_MAJOR, PACKAGE_VERSION_MINOR,
    PACKAGE_VERSION_PATCHLEVEL,
    ABI_CLASS_XINPUT,
    ABI_XINPUT_VERSION,
    MOD_CLASS_XINPUT,
    {0, 0, 0, 0}
};

_X_EXPORT XF86ModuleData nestedInputModuleData = {
    &NestedInputVersionRec,
    &NestedInputPlug,
    &NestedInputUnplug
};

int
NestedInputPreInit(InputDriverPtr drv, InputInfoPtr pInfo, int flags) {
    NestedInputDevicePtr pNestedInput;

    pNestedInput = calloc(1, sizeof(NestedInputDeviceRec));

    if (!pNestedInput)
        return BadAlloc;

    pInfo->private = pNestedInput;
    pInfo->type_name = XI_MOUSE; // This is really both XI_MOUSE and XI_KEYBOARD... but oh well.
    pInfo->read_input = NestedInputReadInput; // new data available.
    pInfo->switch_mode = NULL; // Toggle absolute/relative mode.
    pInfo->device_control = NestedInputControl; // Enable/disable device.

    return Success;
}

void
NestedInputUnInit(InputDriverPtr drv, InputInfoPtr pInfo, int flags) {
}

static pointer
NestedInputPlug(pointer module, pointer options, int *errmaj, int  *errmin) {
    return NULL;
}

static void
NestedInputUnplug(pointer p) {
}

static int
_nested_input_init_keyboard(DeviceIntPtr device) {
    InputInfoPtr pInfo = device->public.devicePrivate;
    
    if (!InitKeyboardDeviceStruct(device, NULL, NULL, NULL)) {
        xf86Msg(X_ERROR, "%s: Failed to register keyboard.\n", pInfo->name);
        return BadAlloc;
    }

    return Success;
}
static int
_nested_input_init_buttons(DeviceIntPtr device) {
    InputInfoPtr pInfo = device->public.devicePrivate;
    CARD8       *map;
    Atom         buttonLabels[NUM_MOUSE_BUTTONS] = {0};

    map = calloc(NUM_MOUSE_BUTTONS, sizeof(CARD8));

    int i;
    for (i = 0; i < NUM_MOUSE_BUTTONS; i++)
        map[i] = i;

    if (!InitButtonClassDeviceStruct(device, NUM_MOUSE_BUTTONS, buttonLabels, map)) {
        xf86Msg(X_ERROR, "%s: Failed to register buttons.\n", pInfo->name);
        
        free(map);
        return BadAlloc;
    }

    return Success;
}

static int
_nested_input_init_axes(DeviceIntPtr device) {
    if (!InitValuatorClassDeviceStruct(device,
                                       NUM_MOUSE_AXES,
                                       (Atom*)GetMotionHistory, // Not sure about this.
                                       GetMotionHistorySize(),
                                       (Atom)0)) {
        return BadAlloc;
    }

    int i;
    for (i = 0; i < NUM_MOUSE_AXES; i++) {
        xf86InitValuatorAxisStruct(device, i, (Atom)0, -1, -1, 1, 1, 1, Absolute);
        xf86InitValuatorDefaults(device, i);
    }

    return Success; 
}

static CARD32
nested_input_on(OsTimerPtr timer, CARD32 time, pointer arg) {
    DeviceIntPtr device = arg;
    InputInfoPtr pInfo = device->public.devicePrivate;
    NestedInputDevicePtr pNestedInput = pInfo->private;

    if(device->public.on)
    {
        pInfo->fd = NestedClientGetFileDescriptor(pNestedInput->clientData);
        xf86FlushInput(pInfo->fd);
        xf86AddEnabledDevice(pInfo);
    }
    return 0;
}

static int 
NestedInputControl(DeviceIntPtr device, int what) {
    InputInfoPtr pInfo = device->public.devicePrivate;

    switch (what) {
        case DEVICE_INIT:
            _nested_input_init_keyboard(device);
            _nested_input_init_buttons(device);
            _nested_input_init_axes(device);
            break;
        case DEVICE_ON:
            xf86Msg(X_INFO, "%s: On.\n", pInfo->name);
            
            if (device->public.on)
                break;

            device->public.on = TRUE;
            TimerSet(NULL, 0, 1, nested_input_on, device);
            break;
        case DEVICE_OFF:
            xf86Msg(X_INFO, "%s: Off.\n", pInfo->name);
            
            if (!device->public.on)
                break;
            
            xf86RemoveEnabledDevice(pInfo);
            
            pInfo->fd = -1;
            device->public.on = FALSE;
            break;
        case DEVICE_CLOSE:
            break;
    }

    return Success;
}

static CARD32
nested_input_ready(OsTimerPtr timer, CARD32 time, pointer arg) {
    NestedClientPrivatePtr clientData = arg;
    NestedClientCheckEvents(clientData);
    return 0;
}

static void 
NestedInputReadInput(InputInfoPtr pInfo) {
    NestedInputDevicePtr pNestedInput = pInfo->private;
    TimerSet(NULL, 0, 1, nested_input_ready, pNestedInput->clientData);
}

void
NestedInputLoadDriver(NestedClientPrivatePtr clientData) {
    
    // Create input options for our invocation to NewInputDeviceRequest.   
    InputOption* options = (InputOption*)malloc(sizeof(InputOption));
    
    options->key = "driver";
    options->value = "nestedinput";

    options->next = (InputOption*)malloc(sizeof(InputOption));
    
    options->next->key = "identifier";
    options->next->value = "nestedinput";
    options->next->next = NULL;

    // Invoke NewInputDeviceRequest to call the PreInit function of
    // the driver.
    DeviceIntPtr dev;
    int ret = NewInputDeviceRequest(options, NULL, &dev);
    
    if (ret != Success) {
        xf86Msg(X_ERROR, "Failed to load input driver.\n");
    }

    // Send the device to the client so that the client can send the
    // device back to the input driver when events are being posted.
    NestedClientSetDevicePtr(clientData, dev);

    InputInfoPtr pInfo = dev->public.devicePrivate;
    NestedInputDevicePtr pNestedInput = pInfo->private;
    pNestedInput->clientData = clientData;
}
    
void
NestedInputPostMouseMotionEvent(DeviceIntPtr dev, int x, int y) {
    xf86PostMotionEvent(dev, TRUE, 0, 2, x, y);
}

void
NestedInputPostButtonEvent(DeviceIntPtr dev, int button, int isDown) {
    xf86PostButtonEvent(dev, 0, button, isDown, 0, 0);
}

void
NestedInputPostKeyboardEvent(DeviceIntPtr dev, unsigned int keycode, int isDown) {
    xf86PostKeyboardEvent(dev, keycode, isDown);
}
