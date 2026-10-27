/* SPDX-License-Identifier: MIT
 * The compositor's effects: one XD_EFFECT_IMPL(name) line each, its
 * xd_effect_impl being xd_effect_impl_<name> in src/compositor/effects/
 * <name>.c. OpenGL effects inside XD_SCENEFX: only xd-compositor-fx has
 * them (meson.build's effects_fx_src).
 *
 * Included with XD_EFFECT_IMPL defined, more than once, so no include
 * guard.
 */
#ifdef XD_SCENEFX
XD_EFFECT_IMPL(wobbly_windows)
#endif
XD_EFFECT_IMPL(general_options)
XD_EFFECT_IMPL(desktop_wall)
#ifdef XD_SCENEFX
XD_EFFECT_IMPL(desktop_cube)
XD_EFFECT_IMPL(rotate_cube)
XD_EFFECT_IMPL(expo)
XD_EFFECT_IMPL(shift_switcher)
#endif
