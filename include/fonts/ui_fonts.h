#ifndef UI_FONTS_H
#define UI_FONTS_H

// DejaVu9/12 extended with Latin-1 glyphs (£, accented letters, etc.) so news
// feed text renders those instead of blank boxes. Defined in src/fonts/
// dejavu_ext.cpp; requires LovyanGFX types (pulled in via ui_common.h before
// this header, exactly as the stock &lgfx::fonts::DejaVu* macros below require).
extern const lgfx::GFXfont DejaVu9_ext;
extern const lgfx::GFXfont DejaVu12_ext;
extern const lgfx::GFXfont DejaVu18_ext;

#define UI_FONT_9   &DejaVu9_ext
#define UI_FONT_12  &DejaVu12_ext
#define UI_FONT_18  &DejaVu18_ext
#define UI_FONT_24  &lgfx::fonts::DejaVu24
#define UI_FONT_40  &lgfx::fonts::DejaVu40
#define UI_FONT_56  &lgfx::fonts::DejaVu56
#define UI_FONT_72  &lgfx::fonts::DejaVu72

#endif // UI_FONTS_H
