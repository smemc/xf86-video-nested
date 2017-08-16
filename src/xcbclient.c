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
 * Laércio de Sousa <laerciosousa@sme-mogidascruzes.sp.gov.br>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdlib.h>

#include <sys/shm.h>

#include <xorg-server.h>
#include <xf86.h>
#include <xf86Priv.h>

#include <xcb/xcb.h>
#include <xcb/xcb_aux.h>
#include <xcb/xcb_event.h>
#include <xcb/xcb_icccm.h>
#include <xcb/xcb_image.h>
#include <xcb/shm.h>
#include <xcb/randr.h>

#include "client.h"

#define BUF_LEN 256

extern char *display;

static xcb_atom_t atom_WM_DELETE_WINDOW;

struct NestedClientPrivate {
    /* Host X server data */
    int screenNumber;
    xcb_connection_t *conn;
    xcb_visualtype_t *visual;
    xcb_window_t rootWindow;
    xcb_gcontext_t gc;
    Bool usingShm;

    /* Nested X server window data */
    xcb_window_t window;
    int scrnIndex;
    int x;
    int y;
    unsigned int width;
    unsigned int height;
    Bool usingFullscreen;
    xcb_image_t *img;
    xcb_shm_segment_info_t shminfo;

    /* Common data */
    uint32_t attrs[2];
    uint32_t attr_mask;
};

/*
 * ----------------------------------------------------------------------------------------
 * INTERNAL FUNCTIONS (needed for NestedPreInit)
 * ----------------------------------------------------------------------------------------
 */

static Bool
XCBClientConnectionHasError(int scrnIndex,
                            xcb_connection_t *conn) {
    const char *displayName = getenv("DISPLAY");

    switch (xcb_connection_has_error(conn)) {
    case XCB_CONN_ERROR:
        xf86DrvMsg(scrnIndex,
                   X_ERROR,
                   "Failed to connect to host X server at display %s.\n", displayName);
        return TRUE;
    case XCB_CONN_CLOSED_EXT_NOTSUPPORTED:
        xf86DrvMsg(scrnIndex,
                   X_ERROR,
                   "Connection to host X server closed: unsupported extension.\n");
        return TRUE;
    case XCB_CONN_CLOSED_MEM_INSUFFICIENT:
        xf86DrvMsg(scrnIndex,
                   X_ERROR,
                   "Connection to host X server closed: out of memory.\n");
        return TRUE;
    case XCB_CONN_CLOSED_REQ_LEN_EXCEED:
        xf86DrvMsg(scrnIndex,
                   X_ERROR,
                   "Connection to host X server closed: exceeding request length that server accepts.\n");
        return TRUE;
    case XCB_CONN_CLOSED_PARSE_ERR:
        xf86DrvMsg(scrnIndex,
                   X_ERROR,
                   "Invalid display for host X server: %s\n", displayName);
        return TRUE;
    case XCB_CONN_CLOSED_INVALID_SCREEN:
        xf86DrvMsg(scrnIndex,
                   X_ERROR,
                   "Host X server does not have a screen matching display %s.\n", displayName);
        return TRUE;
    default:
        return FALSE;
    }
}

static inline Bool
XCBClientCheckExtension(xcb_connection_t *connection,
                        xcb_extension_t *extension) {
    const xcb_query_extension_reply_t *rep = xcb_get_extension_data(connection, extension);
    return rep && rep->present;
}

static Bool
XCBClientCheckRandRVersion(int scrnIndex,
                           xcb_connection_t *conn,
                           int major, int minor) {
    xcb_randr_query_version_cookie_t cookie;
    xcb_randr_query_version_reply_t *reply;
    xcb_generic_error_t *error;

    if (!XCBClientCheckExtension(conn, &xcb_randr_id)) {
        xf86DrvMsg(scrnIndex,
                   X_ERROR,
                   "Host X server does not support RANDR extension (or it's disabled).\n");
        return FALSE;
    }

    /* Check RandR version */
    cookie = xcb_randr_query_version(conn, major, minor);
    reply = xcb_randr_query_version_reply(conn, cookie, &error);

    if (!reply) {
        xf86DrvMsg(scrnIndex,
                   X_ERROR,
                   "Failed to get RandR version supported by host X server. Error code = %d.\n",
                   error->error_code);
        free(error);
        return FALSE;
    } else if (reply->major_version < major ||
               (reply->major_version == major && reply->minor_version < minor)) {
        xf86DrvMsg(scrnIndex,
                   X_ERROR,
                   "Host X server doesn't support RandR %d.%d, needed for Option \"Output\" usage.\n",
                   major, minor);
        free(reply);
        return FALSE;
    } else {
        free(reply);
        return TRUE;
    }
}

static Bool
XCBClientGetOutputGeometry(int scrnIndex,
                           xcb_connection_t *conn,
                           int screenNumber,
                           OutputPtr output) {
    xcb_generic_error_t *error;
    xcb_screen_t *screen;
    xcb_randr_get_screen_resources_cookie_t screen_resources_c;
    xcb_randr_get_screen_resources_reply_t *screen_resources_r;
    xcb_randr_output_t *outputs;
    xcb_randr_mode_info_t *available_modes;
    int available_modes_len, i, j;

    if (!XCBClientCheckRandRVersion(scrnIndex, conn, 1, 2))
        return FALSE;

    screen = xcb_aux_get_screen(conn, screenNumber);

    /* Get list of outputs from screen resources */
    screen_resources_c = xcb_randr_get_screen_resources(conn,
                                                        screen->root);
    screen_resources_r = xcb_randr_get_screen_resources_reply(conn,
                                                              screen_resources_c,
                                                              &error);

    if (!screen_resources_r) {
        xf86DrvMsg(scrnIndex,
                   X_ERROR,
                   "Failed to get host X server screen resources. Error code = %d.\n",
                   error->error_code);
        free(error);
        return FALSE;
    }

    outputs = xcb_randr_get_screen_resources_outputs(screen_resources_r);
    available_modes = xcb_randr_get_screen_resources_modes(screen_resources_r);
    available_modes_len = xcb_randr_get_screen_resources_modes_length(screen_resources_r);

    for (i = 0; i < screen_resources_r->num_outputs; i++) {
        char *name;
        int name_len;
        xcb_randr_get_output_info_cookie_t output_info_c;
        xcb_randr_get_output_info_reply_t *output_info_r;

        /* Get info from the output */
        output_info_c = xcb_randr_get_output_info(conn,
                                                  outputs[i],
                                                  XCB_TIME_CURRENT_TIME);
        output_info_r = xcb_randr_get_output_info_reply(conn,
                                                        output_info_c,
                                                        &error);

        if (!output_info_r) {
            xf86DrvMsg(scrnIndex,
                       X_ERROR,
                       "Failed to get info for output %d. Error code = %d.\n",
                       outputs[i], error->error_code);
            free(error);
            continue;
        }

        /* Get output name */
        name_len = xcb_randr_get_output_info_name_length(output_info_r);
        name = malloc(name_len + 1);
        strncpy(name, (char *)xcb_randr_get_output_info_name(output_info_r), name_len);
        name[name_len] = '\0';

        if (!strcmp(name, output->name)) {
            /* Output found! */
            if (output_info_r->crtc != XCB_NONE) {
                /* Output is enabled! Get its CRTC geometry */
                xcb_randr_get_crtc_info_cookie_t crtc_info_c;
                xcb_randr_get_crtc_info_reply_t *crtc_info_r;

                crtc_info_c = xcb_randr_get_crtc_info(conn,
                                                      output_info_r->crtc,
                                                      XCB_TIME_CURRENT_TIME);
                crtc_info_r = xcb_randr_get_crtc_info_reply(conn,
                                                            crtc_info_c,
                                                            &error);

                if (!crtc_info_r) {
                    xf86DrvMsg(scrnIndex,
                               X_ERROR,
                               "Failed to get CRTC info for output %s. Error code = %d.\n",
                               name, error->error_code);
                    free(error);
                    free(output_info_r);
                    free(screen_resources_r);
                    return FALSE;
                } else {
                    output->width = crtc_info_r->width;
                    output->height = crtc_info_r->height;
                    output->x = crtc_info_r->x;
                    output->y = crtc_info_r->y;
                    free(crtc_info_r);
                }
            } else {
                xf86DrvMsg(scrnIndex,
                           X_ERROR,
                           "Output %s is currently disabled or disconnected.\n",
                           output->name);
                free(error);
                free(name);
                free(output_info_r);
                free(screen_resources_r);
                return FALSE;
            }

            free(name);
            free(output_info_r);
            free(screen_resources_r);
            return TRUE;
        }

        free(output_info_r);
    }

    free(screen_resources_r);
    return FALSE;
}

/*
 * ----------------------------------------------------------------------------------------
 * INTERNAL FUNCTIONS (needed for NestedScreenInit)
 * ----------------------------------------------------------------------------------------
 */

static void
XCBClientTryXShm(NestedClientPrivatePtr pPriv) {
    int shmMajor = 0, shmMinor = 0;
    Bool hasSharedPixmaps = FALSE;

    /* Try to get share memory ximages for a little bit more speed */
    if (!XCBClientCheckExtension(pPriv->conn, &xcb_shm_id))
        pPriv->usingShm = FALSE;
    else {
        /* Really really check we have shm - better way ?*/
        xcb_shm_segment_info_t shminfo;
        xcb_generic_error_t *e;
        xcb_void_cookie_t cookie;
        xcb_shm_seg_t shmseg;

        pPriv->usingShm = TRUE;

        shminfo.shmid = shmget(IPC_PRIVATE, 1, IPC_CREAT | 0777);
        shminfo.shmaddr = shmat(shminfo.shmid, 0, 0);

        shmseg = xcb_generate_id(pPriv->conn);
        cookie = xcb_shm_attach_checked(pPriv->conn,
                                        shmseg,
                                        shminfo.shmid,
                                        TRUE);
        e = xcb_request_check(pPriv->conn, cookie);

        if (e) {
            pPriv->usingShm = FALSE;
            free(e);
        }

        shmdt(shminfo.shmaddr);
        shmctl(shminfo.shmid, IPC_RMID, 0);
    }

    if (!pPriv->usingShm)
        xf86DrvMsg(pPriv->scrnIndex,
                   X_INFO,
                   "XShm extension query failed. Dropping XShm support.\n");
    else {
        xf86DrvMsg(pPriv->scrnIndex,
                   X_INFO,
                   "XShm extension version %d.%d %s shared pixmaps\n",
                   shmMajor, shmMinor, hasSharedPixmaps ? "with" : "without");
    }
}

static void
XCBClientCreateXImage(NestedClientPrivatePtr pPriv, int depth) {
    if (pPriv->img != NULL) {
        /* Free up the image data if previously used
         * i.e. called by server reset */
        if (pPriv->usingShm) {
            xcb_shm_detach(pPriv->conn, pPriv->shminfo.shmseg);
            xcb_image_destroy(pPriv->img);
            shmdt(pPriv->shminfo.shmaddr);
            shmctl(pPriv->shminfo.shmid, IPC_RMID, 0);
        } else {
            free(pPriv->img->data);
            pPriv->img->data = NULL;
            xcb_image_destroy(pPriv->img);
        }
    }

    if (pPriv->usingShm) {
        pPriv->img = xcb_image_create_native(pPriv->conn,
                                             pPriv->width,
                                             pPriv->height,
                                             XCB_IMAGE_FORMAT_Z_PIXMAP,
                                             depth,
                                             NULL,
                                             ~0,
                                             NULL);

        /* XXX: change the 0777 mask? */
        pPriv->shminfo.shmid = shmget(IPC_PRIVATE,
                                      pPriv->img->stride * pPriv->height,
                                      IPC_CREAT | 0777);
        pPriv->img->data = shmat(pPriv->shminfo.shmid, 0, 0);
        pPriv->shminfo.shmaddr = pPriv->img->data;

        if (pPriv->img->data == (uint8_t *)-1) {
            xf86DrvMsg(pPriv->scrnIndex,
                       X_INFO,
                       "Can't attach SHM Segment, falling back to plain XImages.\n");
            pPriv->usingShm = FALSE;
            xcb_image_destroy(pPriv->img);
            shmctl(pPriv->shminfo.shmid, IPC_RMID, 0);
        } else {
            xf86DrvMsg(pPriv->scrnIndex,
                       X_INFO,
                       "SHM segment attached %p\n",
                       pPriv->shminfo.shmaddr);
            pPriv->shminfo.shmseg = xcb_generate_id(pPriv->conn);
            xcb_shm_attach(pPriv->conn,
                           pPriv->shminfo.shmseg,
                           pPriv->shminfo.shmid,
                           FALSE);
        }
    }

    if (!pPriv->usingShm) {
        xf86DrvMsg(pPriv->scrnIndex,
                   X_INFO,
                   "Creating image %dx%d for screen pPriv=%p\n",
                   pPriv->width, pPriv->height, pPriv);
        pPriv->img = xcb_image_create_native(pPriv->conn,
                                             pPriv->width,
                                             pPriv->height,
                                             XCB_IMAGE_FORMAT_Z_PIXMAP,
                                             depth,
                                             NULL,
                                             ~0,
                                             NULL);

        pPriv->img->data =
            malloc(pPriv->img->stride * pPriv->height);
    }
}

static void
XCBClientWindowSetTitle(NestedClientPrivatePtr pPriv,
                        const char *extra_text) {
    char buf[BUF_LEN + 1];

    memset(buf, 0, BUF_LEN + 1);
    snprintf(buf, BUF_LEN, "Xorg at :%s.%d nested on %s%s%s",
             display,
             pPriv->scrnIndex,
             getenv("DISPLAY"),
             extra_text ? " " : "",
             extra_text ? extra_text : "");
    xcb_icccm_set_wm_name(pPriv->conn,
                          pPriv->window,
                          XCB_ATOM_STRING,
                          8,
                          strlen(buf),
                          buf);
}

static void
XCBClientWindowSetWMClass(NestedClientPrivatePtr pPriv,
                    const char *wm_class) {
    const char *resource_name = getenv("RESOURCE_NAME");
    size_t class_len;
    char *class_hint;

    if (!resource_name)
        resource_name = xf86ServerName;

    class_len = strlen(resource_name) + strlen(wm_class) + 2;
    class_hint = malloc(class_len);

    if (class_hint) {
        strcpy(class_hint, resource_name);
        strcpy(class_hint + strlen(resource_name) + 1, wm_class);
        xcb_change_property(pPriv->conn,
                            XCB_PROP_MODE_REPLACE,
                            pPriv->window,
                            XCB_ATOM_WM_CLASS,
                            XCB_ATOM_STRING,
                            8,
                            class_len,
                            class_hint);
        free(class_hint);
    }
}

static Bool
XCBClientConnectToServer(NestedClientPrivatePtr pPriv) {
    uint16_t red, green, blue;
    uint32_t pixel;
    xcb_screen_t *screen;

    pPriv->attrs[0] = XCB_EVENT_MASK_EXPOSURE;
    pPriv->attr_mask = XCB_CW_EVENT_MASK;
    pPriv->conn = xcb_connect(NULL, &pPriv->screenNumber);

    if (XCBClientConnectionHasError(pPriv->scrnIndex, pPriv->conn))
        return FALSE;
    else {
        screen = xcb_aux_get_screen(pPriv->conn, pPriv->screenNumber);
        pPriv->rootWindow = screen->root;
        pPriv->gc = xcb_generate_id(pPriv->conn);
        pPriv->visual = xcb_aux_find_visual_by_id(screen,
                                                  screen->root_visual);

        xcb_create_gc(pPriv->conn, pPriv->gc, pPriv->rootWindow, 0, NULL);

        if (!xcb_aux_parse_color("red", &red, &green, &blue)) {
            xcb_lookup_color_cookie_t c =
                xcb_lookup_color(pPriv->conn, screen->default_colormap, 3, "red");
            xcb_lookup_color_reply_t *r =
                xcb_lookup_color_reply(pPriv->conn, c, NULL);
            red = r->exact_red;
            green = r->exact_green;
            blue = r->exact_blue;
            free(r);
        }

        {
            xcb_alloc_color_cookie_t c = xcb_alloc_color(pPriv->conn,
                                                         screen->default_colormap,
                                                         red, green, blue);
            xcb_alloc_color_reply_t *r = xcb_alloc_color_reply(pPriv->conn, c, NULL);
            pixel = r->pixel;
            free(r);
        }

        xcb_change_gc(pPriv->conn, pPriv->gc, XCB_GC_FOREGROUND, &pixel);
        return TRUE;
    }
}

static void
XCBClientWindowSetFullscreenHint(NestedClientPrivatePtr pPriv) {
    xcb_intern_atom_cookie_t cookie_WINDOW_STATE,
        cookie_WINDOW_STATE_FULLSCREEN;
    xcb_atom_t atom_WINDOW_STATE, atom_WINDOW_STATE_FULLSCREEN;
    xcb_intern_atom_reply_t *reply;

    cookie_WINDOW_STATE = xcb_intern_atom(pPriv->conn, FALSE,
                                          strlen("_NET_WM_STATE"),
                                          "_NET_WM_STATE");
    cookie_WINDOW_STATE_FULLSCREEN =
        xcb_intern_atom(pPriv->conn, FALSE,
                        strlen("_NET_WM_STATE_FULLSCREEN"),
                        "_NET_WM_STATE_FULLSCREEN");

    reply = xcb_intern_atom_reply(pPriv->conn, cookie_WINDOW_STATE, NULL);
    atom_WINDOW_STATE = reply->atom;
    free(reply);

    reply = xcb_intern_atom_reply(pPriv->conn, cookie_WINDOW_STATE_FULLSCREEN,
                                  NULL);
    atom_WINDOW_STATE_FULLSCREEN = reply->atom;
    free(reply);

    xcb_change_property(pPriv->conn,
                        XCB_PROP_MODE_REPLACE,
                        pPriv->window,
                        atom_WINDOW_STATE,
                        XCB_ATOM_ATOM,
                        32,
                        1,
                        &atom_WINDOW_STATE_FULLSCREEN);
}

static void
XCBClientWindowSetDeleteWindowHint(NestedClientPrivatePtr pPriv) {
    xcb_intern_atom_cookie_t cookie_WM_PROTOCOLS,
        cookie_WM_DELETE_WINDOW;
    xcb_atom_t atom_WM_PROTOCOLS;
    xcb_intern_atom_reply_t *reply;

    cookie_WM_PROTOCOLS = xcb_intern_atom(pPriv->conn, FALSE,
                                          strlen("WM_PROTOCOLS"),
                                          "WM_PROTOCOLS");
    cookie_WM_DELETE_WINDOW =
        xcb_intern_atom(pPriv->conn, FALSE,
                        strlen("WM_DELETE_WINDOW"),
                        "WM_DELETE_WINDOW");

    reply = xcb_intern_atom_reply(pPriv->conn, cookie_WM_PROTOCOLS, NULL);
    atom_WM_PROTOCOLS = reply->atom;
    free(reply);

    reply = xcb_intern_atom_reply(pPriv->conn, cookie_WM_DELETE_WINDOW,
                                  NULL);
    atom_WM_DELETE_WINDOW = reply->atom;
    free(reply);

    xcb_change_property(pPriv->conn,
                        XCB_PROP_MODE_REPLACE,
                        pPriv->window,
                        atom_WM_PROTOCOLS,
                        XCB_ATOM_ATOM,
                        32,
                        1,
                        &atom_WM_DELETE_WINDOW);
}

static void
XCBClientWindowCreate(NestedClientPrivatePtr pPriv) {
    xcb_size_hints_t sizeHints;
    uint32_t mask = XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y;
    uint32_t values[2] = {pPriv->x, pPriv->y};

    sizeHints.flags = XCB_ICCCM_SIZE_HINT_P_POSITION
                      | XCB_ICCCM_SIZE_HINT_P_SIZE
                      | XCB_ICCCM_SIZE_HINT_P_MIN_SIZE
                      | XCB_ICCCM_SIZE_HINT_P_MAX_SIZE;
    sizeHints.min_width = pPriv->width;
    sizeHints.max_width = pPriv->width;
    sizeHints.min_height = pPriv->height;
    sizeHints.max_height = pPriv->height;

    pPriv->window = xcb_generate_id(pPriv->conn);

    xcb_create_window(pPriv->conn,
                      XCB_COPY_FROM_PARENT,
                      pPriv->window,
                      pPriv->rootWindow,
                      0, 0, pPriv->width, pPriv->height,
                      0,
                      XCB_WINDOW_CLASS_COPY_FROM_PARENT,
                      pPriv->visual->visual_id,
                      pPriv->attr_mask,
                      pPriv->attrs);

    xcb_icccm_set_wm_normal_hints(pPriv->conn,
                                  pPriv->window,
                                  &sizeHints);

    if (pPriv->usingFullscreen)
        XCBClientWindowSetFullscreenHint(pPriv);

    XCBClientWindowSetDeleteWindowHint(pPriv);
    XCBClientWindowSetTitle(pPriv, NULL);
    XCBClientWindowSetWMClass(pPriv, "Xorg");

    xcb_map_window(pPriv->conn, pPriv->window);

    /* Put this code after xcb_map_window() call, so that
     * our window position values won't be overriden by WM. */
    xcb_configure_window(pPriv->conn, pPriv->window, mask, values);
}

static void
XCBClientWindowHideCursor(NestedClientPrivatePtr pPriv) {
    uint32_t pixel = 0;
    xcb_cursor_t empty_cursor = xcb_generate_id(pPriv->conn);
    xcb_pixmap_t cursor_pxm = xcb_generate_id(pPriv->conn);
    xcb_gcontext_t cursor_gc = xcb_generate_id(pPriv->conn);
    xcb_rectangle_t rect = {0, 0, 1, 1};

    xcb_create_pixmap(pPriv->conn, 1, cursor_pxm, pPriv->rootWindow, 1, 1);
    xcb_create_gc(pPriv->conn, cursor_gc, cursor_pxm,
                  XCB_GC_FOREGROUND, &pixel);
    xcb_poly_fill_rectangle(pPriv->conn, cursor_pxm, cursor_gc, 1, &rect);
    xcb_free_gc(pPriv->conn, cursor_gc);

    xcb_create_cursor(pPriv->conn,
                      empty_cursor,
                      cursor_pxm, cursor_pxm,
                      0, 0, 0,
                      0, 0, 0,
                      1, 1);
    xcb_free_pixmap(pPriv->conn, cursor_pxm);

    xcb_change_window_attributes(pPriv->conn,
                                 pPriv->window,
                                 XCB_CW_CURSOR,
                                 &empty_cursor);
}

static void
XCBClientHandleEventExpose(NestedClientPrivatePtr pPriv,
                           xcb_expose_event_t *event) {
    NestedClientUpdateScreen(pPriv,
                             event->x,
                             event->y,
                             event->x + event->width,
                             event->y + event->height);
}

static void
XCBClientHandleEventClientMessage(NestedClientPrivatePtr pPriv,
                                  xcb_client_message_event_t *event) {
    if (event->data.data32[0] == atom_WM_DELETE_WINDOW) {
        /* XXX: Is there a better way to terminate nested Xorg
         *      on window deletion, avoiding memory leaks? */
        xf86DrvMsg(pPriv->scrnIndex,
                   X_INFO,
                   "Nested client window closed.\n");
        CloseWellKnownConnections();
        OsCleanup(0);
        exit(0);
    }
}

static void
XCBClientPoll(NestedClientPrivatePtr pPriv) {
    while (TRUE) {
        xcb_generic_event_t *event = xcb_poll_for_event(pPriv->conn);
        
        if (!event) {
            /* If our XCB connection has died (for example, our window was
             * closed), exit now.
             */
             if (XCBClientConnectionHasError(pPriv->scrnIndex, pPriv->conn)) {
                CloseWellKnownConnections();
                OsCleanup(1);
                exit(1);
            }

            break;
        }

        switch (XCB_EVENT_RESPONSE_TYPE(event)) {
        case XCB_EXPOSE:
            XCBClientHandleEventExpose(pPriv, (xcb_expose_event_t *)event);
            break;
        case XCB_CLIENT_MESSAGE:
            XCBClientHandleEventClientMessage(pPriv, (xcb_client_message_event_t *)event);
        }

        free(event);
    }
}

/*
 * ----------------------------------------------------------------------------------------
 * PUBLIC API IMPLEMENTATION
 * ----------------------------------------------------------------------------------------
 */

Bool
NestedClientCheckDisplay(int scrnIndex, OutputPtr output) {
    int n;
    xcb_connection_t *conn = xcb_connect(NULL, &n);

    if (XCBClientConnectionHasError(scrnIndex, conn))
        return FALSE;
    else if (output->name != NULL) {
        output->width = 0;
        output->height = 0;
        output->x = 0;
        output->y = 0;

        if (!XCBClientGetOutputGeometry(scrnIndex, conn, n, output))
            return FALSE;
        else
            xf86DrvMsg(scrnIndex,
                       X_INFO,
                       "Got CRTC geometry from output %s: %dx%d+%d+%d\n",
                       output->name,
                       output->width,
                       output->height,
                       output->x,
                       output->y);
    } else {
        xcb_screen_t *s = xcb_aux_get_screen(conn, n);
        output->width = s->width_in_pixels;
        output->height = s->height_in_pixels;
    }

    xcb_disconnect(conn);
    return TRUE;
}

Bool
NestedClientValidDepth(int depth) {
    /* XXX: implement! */
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
    NestedClientPrivatePtr pPriv = malloc(sizeof(struct NestedClientPrivate));

    if (!pPriv)
        return NULL;
    else {
        pPriv->scrnIndex = scrnIndex;
        pPriv->usingFullscreen = wantFullscreenHint;
        pPriv->width = width;
        pPriv->height = height;
        pPriv->x = originX;
        pPriv->y = originY;
	pPriv->img = NULL;

        if (!XCBClientConnectToServer(pPriv)) {
            xcb_disconnect(pPriv->conn);
            free(pPriv);
            return NULL;
        } else {
            XCBClientTryXShm(pPriv);
            XCBClientCreateXImage(pPriv, depth);
            XCBClientWindowCreate(pPriv);
            XCBClientWindowHideCursor(pPriv);
            xcb_flush(pPriv->conn);

#if 0
            xf86DrvMsg(pPriv->scrnIndex, X_INFO, "width: %d\n", pPriv->img->width);
            xf86DrvMsg(pPriv->scrnIndex, X_INFO, "height: %d\n", pPriv->img->height);
            xf86DrvMsg(pPriv->scrnIndex, X_INFO, "depth: %d\n", pPriv->img->depth);
            xf86DrvMsg(pPriv->scrnIndex, X_INFO, "bpp: %d\n", pPriv->img->bpp);
            xf86DrvMsg(pPriv->scrnIndex, X_INFO, "red_mask: 0x%x\n", pPriv->visual->red_mask);
            xf86DrvMsg(pPriv->scrnIndex, X_INFO, "gre_mask: 0x%x\n", pPriv->visual->green_mask);
            xf86DrvMsg(pPriv->scrnIndex, X_INFO, "blu_mask: 0x%x\n", pPriv->visual->blue_mask);
#endif

            *retRedMask = pPriv->visual->red_mask;
            *retGreenMask = pPriv->visual->green_mask;
            *retBlueMask = pPriv->visual->blue_mask;

            return pPriv;
        }
    }
}

void
NestedClientHideCursor(NestedClientPrivatePtr pPriv) {
    XCBClientWindowHideCursor(pPriv);
}

char *
NestedClientGetFrameBuffer(NestedClientPrivatePtr pPriv) {
    return (char *)pPriv->img->data;
}

void
NestedClientUpdateScreen(NestedClientPrivatePtr pPriv,
                         int16_t x1, int16_t y1,
                         int16_t x2, int16_t y2) {
    if (pPriv->usingShm)
        xcb_image_shm_put(pPriv->conn, pPriv->window,
                          pPriv->gc, pPriv->img,
                          pPriv->shminfo,
                          x1, y1, x1, y1, x2 - x1, y2 - y1, FALSE);
    else {
        xcb_image_t *subimg = xcb_image_subimage(pPriv->img, x1, y1,
                                                 x2 - x1, y2 - y1, 0, 0, 0);
        xcb_image_t *img = xcb_image_native(pPriv->conn, subimg, 1);
        xcb_image_put(pPriv->conn, pPriv->window, pPriv->gc, img, x1, y1, 0);

        if (subimg != img)
            xcb_image_destroy(img);

        xcb_image_destroy(subimg);
    }

    xcb_flush(pPriv->conn);
}

void
NestedClientCheckEvents(NestedClientPrivatePtr pPriv) {
    XCBClientPoll(pPriv);
}

void
NestedClientCloseScreen(NestedClientPrivatePtr pPriv) {
    if (pPriv->usingShm) {
        xcb_shm_detach(pPriv->conn, pPriv->shminfo.shmseg);
        shmdt(pPriv->shminfo.shmaddr);
    }

    xcb_image_destroy(pPriv->img);
    xcb_disconnect(pPriv->conn);
    free(pPriv);
}

int
NestedClientGetFileDescriptor(NestedClientPrivatePtr pPriv) {
    return xcb_get_file_descriptor(pPriv->conn);
}
