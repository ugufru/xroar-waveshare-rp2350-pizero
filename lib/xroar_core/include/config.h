/* Hand-written config.h for the RP2350 port of XRoar.
 * Replaces the autotools-generated config.h. Keep this minimal — every
 * HAVE_* flipped on here pulls more host code into the build.
 */
#ifndef XROAR_PORT_CONFIG_H
#define XROAR_PORT_CONFIG_H

#define HAVE___BUILTIN_EXPECT 1
#define HAVE_FUNC_ATTRIBUTE_CONST 1
#define HAVE_FUNC_ATTRIBUTE_FORMAT 1
#define HAVE_FUNC_ATTRIBUTE_MALLOC 1
#define HAVE_FUNC_ATTRIBUTE_NONNULL 1
#define HAVE_FUNC_ATTRIBUTE_NORETURN 1
#define HAVE_FUNC_ATTRIBUTE_RETURNS_NONNULL 1
#define HAVE_FUNC_ATTRIBUTE_PURE 1

/* Intentionally NOT defined:
 *   LOGGING         — logging.h falls back to no-op macros
 *   HAVE_SDL2 etc.  — no host stack
 */

#endif
