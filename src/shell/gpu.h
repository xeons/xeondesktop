/* SPDX-License-Identifier: MIT
 * Hybrid graphics: which graphics card a program starts on. Laptops with
 * an integrated and a dedicated GPU run everything on the default one;
 * switcheroo-control (net.hadess.SwitcherooControl on the system bus)
 * names the cards and the environment variables (DRI_PRIME, NVIDIA's
 * PRIME render offload...) that put a program on another. A desktop entry
 * with PrefersNonDefaultGPU=true starts on the other card by itself, and
 * the user can choose per program (~/.config/xeondesktop/gpu.ini).
 */
#ifndef XD_SHELL_GPU_H
#define XD_SHELL_GPU_H

#include <stdint.h>

typedef struct _GVariant GVariant;
typedef struct _GDesktopAppInfo GDesktopAppInfo;
typedef struct _GAppLaunchContext GAppLaunchContext;

typedef struct xd_gpu {
    char *name;
    char **env; /* KEY, VALUE, KEY, VALUE, ..., nullptr */
    bool is_default;
} xd_gpu;

typedef struct xd_gpu_list {
    xd_gpu *items;
    size_t count;
} xd_gpu_list;

/* switcheroo-control's GPUs property (aa{sv}: Name, Environment, Default). */
void xd_gpu_list_parse(GVariant *gpus, xd_gpu_list *out);
void xd_gpu_list_free(xd_gpu_list *list);
/* The default card, or the first that isn't; -1 if none. */
int xd_gpu_list_default(const xd_gpu_list *list);
int xd_gpu_list_other(const xd_gpu_list *list);

/* What the user chose for a program: nothing (its desktop entry decides),
 * the default card, or the other (dedicated) one. */
typedef enum xd_gpu_pref : uint8_t {
    XD_GPU_PREF_AUTO,
    XD_GPU_PREF_DEFAULT,
    XD_GPU_PREF_OTHER,
} xd_gpu_pref;

/* The card a program starts on: an index into `list`, or -1 for the
 * default one (nothing to set). Fewer than two cards: always -1. */
int xd_gpu_choose(const xd_gpu_list *list, bool prefers_non_default, xd_gpu_pref pref);

/* Per program, by desktop file id (e.g. "firefox.desktop"), in `file`
 * (nullptr: ~/.config/xeondesktop/gpu.ini, [Programs] id=dedicated or
 * default). */
xd_gpu_pref xd_gpu_pref_get(const char *file, const char *app_id);
bool xd_gpu_pref_set(const char *file, const char *app_id, xd_gpu_pref pref);
/* The id a program's choice is kept under: its desktop file id, or for a
 * shortcut loaded from a file (Quick Launch, the desktop) the file's name.
 * Free with g_free; nullptr if it has neither. */
char *xd_gpu_app_key(GDesktopAppInfo *info);

/* The shell follows switcheroo-control from xd_gpu_watch on. */
void xd_gpu_watch(void);
void xd_gpu_unwatch(void);
/* The cards while switcheroo-control reports more than one (HasDualGpu);
 * empty otherwise (and in programs that never watch). */
const xd_gpu_list *xd_gpu_current(void);
/* Whether `info` starts on another card than the default now: by its
 * desktop entry (PrefersNonDefaultGPU) or the user's choice. */
bool xd_gpu_app_uses_other(GDesktopAppInfo *info);
/* Sets the variables of the card a launch of `info` uses in `ctx`: card
 * `gpu` (an index into xd_gpu_current), or with gpu < 0 the program's own
 * (xd_gpu_app_uses_other). */
void xd_gpu_setup_launch(GAppLaunchContext *ctx, GDesktopAppInfo *info, int gpu);

#endif
