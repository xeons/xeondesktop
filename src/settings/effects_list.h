/* SPDX-License-Identifier: MIT
 * Every effect Desktop Effects knows: one XD_EFFECT(name) line each, whose
 * xd_effect_info is xd_effect_<name>, defined in src/settings/effects/
 * <name>.c (and that file listed in meson.build's settings_lib). Order
 * doesn't matter: the panel sorts each category by name.
 *
 * Included with XD_EFFECT defined, more than once, so no include guard.
 */
XD_EFFECT(wobbly_windows)
XD_EFFECT(general_options)
XD_EFFECT(desktop_wall)
XD_EFFECT(desktop_cube)
XD_EFFECT(rotate_cube)
