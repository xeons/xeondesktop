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
XD_EFFECT_IMPL(magic_lamp)
XD_EFFECT_IMPL(burn)
XD_EFFECT_IMPL(explode)
XD_EFFECT_IMPL(water)
#endif
XD_EFFECT_IMPL(scale)
XD_EFFECT_IMPL(animations)
XD_EFFECT_IMPL(window_opacity)
XD_EFFECT_IMPL(ring_switcher)
XD_EFFECT_IMPL(enhanced_zoom)
