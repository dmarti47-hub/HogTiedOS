/* LVGL 9.2 configuration for hogtied-ui. Unlisted options use LVGL defaults. */
#ifndef LV_CONF_H
#define LV_CONF_H

/* Panel is RGB565 (premium omap3530.conf lcdfmt=0x1 / display.conf rgb565) */
#define LV_COLOR_DEPTH 16

#define LV_USE_STDLIB_MALLOC  LV_STDLIB_BUILTIN
#define LV_MEM_SIZE           (512 * 1024)

#define LV_USE_OS             LV_OS_NONE
#define LV_DEF_REFR_PERIOD    33          /* ~30 fps is plenty for gauges */
#define LV_USE_LOG            0

#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_28 1
#define LV_FONT_MONTSERRAT_48 1
#define LV_FONT_DEFAULT       &lv_font_montserrat_14

/* Target framebuffer backend (-DHOGTIED_FBDEV=ON) */
#ifdef HOGTIED_FBDEV
#define LV_USE_LINUX_FBDEV    1
#endif

/* PC desktop window backend (-DHOGTIED_SDL=ON) */
#ifdef HOGTIED_SDL
#define LV_USE_SDL            1
#define LV_SDL_INCLUDE_PATH   <SDL2/SDL.h>
#define LV_SDL_RENDER_MODE    LV_DISPLAY_RENDER_MODE_DIRECT
#define LV_SDL_BUF_COUNT      1
#endif

#define LV_BUILD_EXAMPLES     0
#define LV_USE_DEMO_WIDGETS   0

#endif
