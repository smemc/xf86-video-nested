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
 * Laércio de Sousa <laerciosousa@sme-mogidascruzes.sp.gov.br>
 */

#include <stdlib.h>

#include <sys/ipc.h>
#include <sys/shm.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/XKBlib.h>
#include <X11/extensions/XShm.h>

#include <xorg-server.h>
#include <xf86.h>

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "client.h"

struct NestedClientPrivate {
    Display *display;
    int screenNumber;
    Screen *screen;
    Window rootWindow;
    Window window;
    XImage *img;
    GC gc;
    Bool usingShm;
    XShmSegmentInfo shminfo;
    int scrnIndex; /* stored only for xf86DrvMsg usage */
    Cursor mycursor; /* Test cursor */
    Pixmap bitmapNoData;
    XColor color1;

    struct {
        int op;
        int event;
        int error;
        int major;
        int minor;
    } xkb;
};

/* Checks if a display is open */
Bool
NestedClientCheckDisplay(int scrnIndex, OutputPtr output) {
    Display *d;

    d = XOpenDisplay(NULL);
    if (!d) {
        return FALSE;
    } else {
      XCloseDisplay(d);
        return TRUE;
    }
}

Bool
NestedClientValidDepth(int depth) {
    /* XXX: implement! */
    return TRUE;
}

static Bool
NestedClientTryXShm(NestedClientPrivatePtr pPriv, int scrnIndex, int width, int height, int depth) {
    int shmMajor, shmMinor;
    Bool hasSharedPixmaps;

    if (!XShmQueryExtension(pPriv->display)) {
        xf86DrvMsg(scrnIndex, X_INFO, "XShmQueryExtension failed.  Dropping XShm support.\n");

        return FALSE;
    }

    if (XShmQueryVersion(pPriv->display, &shmMajor, &shmMinor,
                         &hasSharedPixmaps)) {
        xf86DrvMsg(scrnIndex, X_INFO,
                   "XShm extension version %d.%d %s shared pixmaps\n",
                   shmMajor, shmMinor, (hasSharedPixmaps) ? "with" : "without");
    }

    pPriv->img = XShmCreateImage(pPriv->display,
                                 DefaultVisualOfScreen(pPriv->screen),
                                 depth,
                                 ZPixmap,
                                 NULL, /* data */
                                 &pPriv->shminfo,
                                 width,
                                 height);

    if (!pPriv->img) {
        xf86DrvMsg(scrnIndex, X_ERROR, "XShmCreateImage failed.  Dropping XShm support.\n");
        return FALSE;
    }

    /* XXX: change the 0777 mask? */
    pPriv->shminfo.shmid = shmget(IPC_PRIVATE,
                                  pPriv->img->bytes_per_line *
                                  pPriv->img->height,
                                  IPC_CREAT | 0777);

    if (pPriv->shminfo.shmid == -1) {
        xf86DrvMsg(scrnIndex, X_ERROR, "shmget failed.  Dropping XShm support.\n");
        XDestroyImage(pPriv->img);
        return FALSE;
    }

    pPriv->shminfo.shmaddr = (char *)shmat(pPriv->shminfo.shmid, NULL, 0);

    if (pPriv->shminfo.shmaddr == (char *) -1) {
        xf86DrvMsg(scrnIndex, X_ERROR, "shmaddr failed.  Dropping XShm support.\n");
        XDestroyImage(pPriv->img);
        return FALSE;
    }

    pPriv->img->data = pPriv->shminfo.shmaddr;
    pPriv->shminfo.readOnly = FALSE;
    XShmAttach(pPriv->display, &pPriv->shminfo);
    pPriv->usingShm = TRUE;

    return TRUE;
}


NestedClientPrivatePtr
NestedClientCreateScreen(int scrnIndex,
                         Bool wantFullscreenHint,
                         int width,
                         int height,
                         int originX,
                         int originY,
                         int depth,
                         int bitsPerPixel,
                         Pixel *retRedMask,
                         Pixel *retGreenMask,
                         Pixel *retBlueMask) {
    NestedClientPrivatePtr pPriv;
    XSizeHints sizeHints;
    Bool supported;
    char windowTitle[32];

    pPriv = malloc(sizeof(struct NestedClientPrivate));
    pPriv->scrnIndex = scrnIndex;

    pPriv->display = XOpenDisplay(NULL);
    if (!pPriv->display)
        return NULL;

    supported = XkbQueryExtension(pPriv->display, &pPriv->xkb.op, &pPriv->xkb.event,
                                  &pPriv->xkb.error, &pPriv->xkb.major, &pPriv->xkb.minor);
    if (!supported) {
        xf86DrvMsg(pPriv->scrnIndex, X_ERROR, "The remote server does not support the XKEYBOARD extension.\n");
        XCloseDisplay(pPriv->display);
        return NULL;
    }

    pPriv->screenNumber = DefaultScreen(pPriv->display);
    pPriv->screen = ScreenOfDisplay(pPriv->display, pPriv->screenNumber);
    pPriv->rootWindow = RootWindow(pPriv->display, pPriv->screenNumber);
    pPriv->gc = DefaultGC(pPriv->display, pPriv->screenNumber);

    pPriv->window = XCreateSimpleWindow(pPriv->display, pPriv->rootWindow,
    originX, originY, width, height, 0, 0, 0);

    sizeHints.flags = PPosition | PSize | PMinSize | PMaxSize;
    sizeHints.min_width = width;
    sizeHints.max_width = width;
    sizeHints.min_height = height;
    sizeHints.max_height = height;
    XSetWMNormalHints(pPriv->display, pPriv->window, &sizeHints);

    snprintf(windowTitle, sizeof(windowTitle), "Screen %d", scrnIndex);

    XStoreName(pPriv->display, pPriv->window, windowTitle);
    
    XMapWindow(pPriv->display, pPriv->window);

    XSelectInput(pPriv->display, pPriv->window, ExposureMask);

    if (!NestedClientTryXShm(pPriv, scrnIndex, width, height, depth)) {
        pPriv->img = XCreateImage(pPriv->display,
        DefaultVisualOfScreen(pPriv->screen),
                              depth,
                              ZPixmap,
                              0, /* offset */
                              NULL, /* data */
                              width,
                              height,
                              32, /* XXX: bitmap_pad */
                              0 /* XXX: bytes_per_line */);

        if (!pPriv->img)
            return NULL;

        pPriv->img->data = malloc(pPriv->img->bytes_per_line * pPriv->img->height);
        pPriv->usingShm = FALSE;
    }

    if (!pPriv->img->data)
        return NULL;

    NestedClientHideCursor(pPriv); /* Hide cursor */

#if 0
xf86DrvMsg(scrnIndex, X_INFO, "width: %d\n", pPriv->img->width);
xf86DrvMsg(scrnIndex, X_INFO, "height: %d\n", pPriv->img->height);
xf86DrvMsg(scrnIndex, X_INFO, "xoffset: %d\n", pPriv->img->xoffset);
xf86DrvMsg(scrnIndex, X_INFO, "depth: %d\n", pPriv->img->depth);
xf86DrvMsg(scrnIndex, X_INFO, "bpp: %d\n", pPriv->img->bits_per_pixel);
xf86DrvMsg(scrnIndex, X_INFO, "red_mask: 0x%lx\n", pPriv->img->red_mask);
xf86DrvMsg(scrnIndex, X_INFO, "gre_mask: 0x%lx\n", pPriv->img->green_mask);
xf86DrvMsg(scrnIndex, X_INFO, "blu_mask: 0x%lx\n", pPriv->img->blue_mask);
#endif

    *retRedMask = pPriv->img->red_mask;
    *retGreenMask = pPriv->img->green_mask;
    *retBlueMask = pPriv->img->blue_mask;

    XEvent ev;
    while (1) {
        XNextEvent(pPriv->display, &ev);
        if (ev.type == Expose) {
            break;
        }
    }
 
    return pPriv;
}

void NestedClientHideCursor(NestedClientPrivatePtr pPriv) {
    char noData[]= {0,0,0,0,0,0,0,0};
    pPriv->color1.red = pPriv->color1.green = pPriv->color1.blue = 0;

    pPriv->bitmapNoData = XCreateBitmapFromData(pPriv->display,
                                                pPriv->window, noData, 7, 7);

    pPriv->mycursor = XCreatePixmapCursor(pPriv->display,
                                          pPriv->bitmapNoData, pPriv->bitmapNoData,
                                          &pPriv->color1, &pPriv->color1, 0, 0);

    XDefineCursor(pPriv->display, pPriv->window, pPriv->mycursor);
    XFreeCursor(pPriv->display, pPriv->mycursor);
}

char *
NestedClientGetFrameBuffer(NestedClientPrivatePtr pPriv) {
    return pPriv->img->data;
}

void
NestedClientUpdateScreen(NestedClientPrivatePtr pPriv, int16_t x1,
                          int16_t y1, int16_t x2, int16_t y2) {
    if (pPriv->usingShm) {
        XShmPutImage(pPriv->display, pPriv->window, pPriv->gc, pPriv->img,
                     x1, y1, x1, y1, x2 - x1, y2 - y1, FALSE);
        /* Without this sync we get some freezes, probably due to some lock
         * in the shm usage */
        XSync(pPriv->display, FALSE);
    } else {
        XPutImage(pPriv->display, pPriv->window, pPriv->gc, pPriv->img,
                  x1, y1, x1, y1, x2 - x1, y2 - y1);
    }
}

void
NestedClientCheckEvents(NestedClientPrivatePtr pPriv) {
    XEvent ev;

    while(XCheckMaskEvent(pPriv->display, ~0, &ev)) {
        switch (ev.type) {
        case Expose:
            NestedClientUpdateScreen(pPriv,
                                     ((XExposeEvent*)&ev)->x,
                                     ((XExposeEvent*)&ev)->y,
                                     ((XExposeEvent*)&ev)->x + 
                                     ((XExposeEvent*)&ev)->width,
                                     ((XExposeEvent*)&ev)->y + 
                                     ((XExposeEvent*)&ev)->height);
            break;
        }
    }
}

void
NestedClientCloseScreen(NestedClientPrivatePtr pPriv) {
    if (pPriv->usingShm) {
        XShmDetach(pPriv->display, &pPriv->shminfo);
        shmdt(pPriv->shminfo.shmaddr);
    }

    XDestroyImage(pPriv->img);
    XCloseDisplay(pPriv->display);
}

int
NestedClientGetFileDescriptor(NestedClientPrivatePtr pPriv) {
    return ConnectionNumber(pPriv->display);
}
