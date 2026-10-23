/* SPDX-License-Identifier: MIT
 * A scripted VirtualBox host for tests (fakehost.c has the script's
 * steps); what the guest answers goes to SCRIPT.log.
 */
#ifndef XD_VBOXDND_FAKEHOST_H
#define XD_VBOXDND_FAKEHOST_H

#include <glib.h>

#include "hgcm.h"

xd_hgcm *xd_fake_host_open(const char *script, GError **error);

#endif
