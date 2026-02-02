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

extern const uint8_t u8g2_font_misans_thin_9_ascii[] U8G2_FONT_SECTION("u8g2_font_misans_thin_9_ascii");
extern const uint8_t u8g2_font_misans_thin_12_ascii[] U8G2_FONT_SECTION("u8g2_font_misans_thin_12_ascii");
extern const uint8_t u8g2_font_misans_light_16_cjk[] U8G2_FONT_SECTION("u8g2_font_misans_light_16_cjk");

extern const uint8_t u8g2_font_streamline_all_t[] U8G2_FONT_SECTION("u8g2_font_streamline_all_t");
extern const uint8_t u8g2_font_spleen12x24_mf[] U8G2_FONT_SECTION("u8g2_font_spleen12x24_mf");
extern const uint8_t u8g2_font_open_iconic_all_2x_t[] U8G2_FONT_SECTION("u8g2_font_open_iconic_all_2x_t");
extern const uint8_t u8g2_font_spleen8x16_mf[] U8G2_FONT_SECTION("u8g2_font_spleen8x16_mf");



#ifdef __cplusplus
}
#endif

#endif
