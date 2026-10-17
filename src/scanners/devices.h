/* SPDX-License-Identifier: MIT
 * The imaging devices: scanners through SANE, cameras through
 * libgphoto2. Every call here blocks (USB, the network) and belongs in a
 * worker thread; SANE calls are serialized, since backends needn't be
 * thread-safe, and so are a camera's.
 */
#ifndef XD_SCANNERS_DEVICES_H
#define XD_SCANNERS_DEVICES_H

#include <gdk-pixbuf/gdk-pixbuf.h>

#include "scanlib.h"

typedef struct xd_imaging_device {
    bool camera;
    char *id;     /* SANE's device name, or the camera's model */
    char *port;   /* the camera's port ("usb:001,004"); SANE's name again */
    char *vendor; /* "Canon" */
    char *model;  /* "CanoScan LiDE 220" */
    char *type;   /* "flatbed scanner", "Digital camera" */
} xd_imaging_device;

void xd_imaging_device_free(xd_imaging_device *d);
xd_imaging_device *xd_imaging_device_copy(const xd_imaging_device *d);

/* The scanners and cameras connected now. Free with g_ptr_array_unref. */
GPtrArray *xd_imaging_devices_list(void);

/* ---- scanners ---------------------------------------------------------------------- */

/* What the scanner offers: its modes (strings) and resolutions (ints). */
typedef struct xd_scanner_info {
    GPtrArray *modes;
    GArray *resolutions;
    int min_dpi, max_dpi;
} xd_scanner_info;

xd_scanner_info *xd_scanner_info_get(const char *device, GError **error);
void xd_scanner_info_free(xd_scanner_info *info);

/* Opens the scanner and reads its options: Test Scanner. */
bool xd_scanner_test(const char *device, GError **error);

/* Scans a picture. `progress` gets thousandths done (written from the
 * scanning thread; read it atomically); setting `*cancel` stops the
 * scan, which then fails with G_IO_ERROR_CANCELLED. */
GdkPixbuf *xd_scanner_scan(const char *device, xd_picture_type type, int dpi, bool preview,
                           gint *progress, gint *cancel, GError **error);

/* ---- cameras ----------------------------------------------------------------------- */

typedef struct xd_camera xd_camera;

xd_camera *xd_camera_open(const char *model, const char *port, GError **error);
void xd_camera_close(xd_camera *cam);
bool xd_camera_test(const char *model, const char *port, GError **error);

typedef struct xd_camera_photo {
    char *folder, *name;
    GdkPixbuf *thumbnail; /* nullptr when the camera has none */
} xd_camera_photo;

void xd_camera_photo_free(xd_camera_photo *p);

/* The camera's pictures, every folder, with their thumbnails. */
GPtrArray *xd_camera_list_photos(xd_camera *cam, gint *cancel, GError **error);
/* Copies one picture to `dest`. */
bool xd_camera_download(xd_camera *cam, const char *folder, const char *name, const char *dest,
                        GError **error);

#endif
