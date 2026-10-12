/* The preview's LVGL configuration: what firmware/hearth-sink/sdkconfig.panel asks of LVGL on the
 * board, and nothing else; every other option is lv_conf_internal.h's default, as it is there. */
#if 1

#define LV_COLOR_DEPTH 16
#define LV_USE_STDLIB_MALLOC LV_STDLIB_CLIB
#define LV_USE_OS LV_OS_NONE
#define LV_USE_LOG 0
#define LV_USE_TINY_TTF 1
#define LV_TINY_TTF_FILE_SUPPORT 0
#define LV_TINY_TTF_CACHE_GLYPH_CNT 96
#define LV_TINY_TTF_CACHE_KERNING_CNT 64
#define LV_USE_BIDI 1
#define LV_FONT_MONTSERRAT_14 1

#endif
