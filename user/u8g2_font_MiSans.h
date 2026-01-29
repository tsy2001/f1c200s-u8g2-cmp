#ifndef _U8G2_MISANS_H
#define _U8G2_MISANS_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef U8G2_USE_LARGE_FONTS
#define U8G2_USE_LARGE_FONTS
#endif

#ifndef U8X8_FONT_SECTION

#ifdef __GNUC__
#  define U8X8_SECTION(name) __attribute__ ((section (name)))
#else
#  define U8X8_SECTION(name)
#endif

#ifndef U8X8_FONT_SECTION
#  define U8X8_FONT_SECTION(name) 
#endif

#endif

#ifndef U8G2_FONT_SECTION
#define U8G2_FONT_SECTION(name) U8X8_FONT_SECTION(name) 
#endif

extern const uint8_t u8g2_font_misans_thin_14_ascii[] U8G2_FONT_SECTION("u8g2_font_misans_thin_14_ascii");
extern const uint8_t u8g2_font_misans_light_16_cjk[] U8G2_FONT_SECTION("u8g2_font_misans_light_16_cjk");

#ifdef __cplusplus
}
#endif

#endif
