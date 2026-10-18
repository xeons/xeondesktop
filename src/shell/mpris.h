/* SPDX-License-Identifier: MIT
 * The session's media players, over MPRIS (org.mpris.MediaPlayer2 on the
 * session bus, through GDBus), for the Previous, Play/Pause and Next
 * buttons under a player's taskbar thumbnail: the closest there is to
 * Windows 7's thumbnail toolbars, which programs added through a Windows
 * API that has no Linux counterpart.
 *
 * Players are followed as they come and go (NameOwnerChanged); their
 * properties come cached and kept current by GDBusProxy. Nothing blocks:
 * a player shows once its properties are in.
 */
#ifndef XD_SHELL_MPRIS_H
#define XD_SHELL_MPRIS_H

typedef struct xd_mpris xd_mpris;

typedef struct xd_mpris_state {
    bool can_previous, can_play_pause, can_next;
    bool playing; /* PlaybackStatus is Playing: the middle button pauses */
} xd_mpris_state;

/* Starts following the players; `changed` runs when one comes, goes or
 * changes. */
xd_mpris *xd_mpris_new(void (*changed)(void *data), void *data);
void xd_mpris_free(xd_mpris *mp);
/* The bus name of the program's player (see xd_jumplist_player_of), or
 * nullptr if it has none, or several (there's no telling which window is
 * whose then). */
const char *xd_mpris_player_of(xd_mpris *mp, const char *desktop_id, const char *app_id);
bool xd_mpris_state_of(xd_mpris *mp, const char *name, xd_mpris_state *out);
/* Calls Previous, PlayPause or Next on the player, not waiting. */
void xd_mpris_send(xd_mpris *mp, const char *name, const char *method);

#endif
