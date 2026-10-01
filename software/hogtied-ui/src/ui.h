/* SPDX-License-Identifier: MIT */
#ifndef HOGTIED_UI_H
#define HOGTIED_UI_H

#include "hbas/audio.h"
#include "hbas/vehicle.h"

#define UI_WIDTH  400     /* premium panel, CROSS_CHECKS.md sec. 7 */
#define UI_HEIGHT 240

/* Navigation keys: handlebars (via hbas-iocd), PC arrow keys, or stdin. */
enum ui_key { UI_KEY_LEFT, UI_KEY_RIGHT, UI_KEY_UP, UI_KEY_DOWN, UI_KEY_ENTER, UI_KEY_BACK };

enum ui_page { UI_PAGE_DASH, UI_PAGE_AUDIO, UI_PAGE_TIRES, UI_PAGE_SYSTEM, UI_PAGE_COUNT };

void ui_create(void);
void ui_update(const struct hbas_vehicle *v);
void ui_key(enum ui_key key);
enum ui_page ui_current_page(void);

/* Audio: where settings go (NULL = nowhere), and bike facts that shape it. */
void ui_set_audio_backend(const struct hbas_audio_backend *b);
void ui_set_speaker_count(uint8_t speakers);   /* 2 or 4; fade needs 4 */
void ui_set_bike_config(int cfg);               /* HD_Configuration_Options, -1 unknown */

/* ---- shared between ui.c and ui_audio.c --------------------------------- */
#include "lvgl.h"

#define COL_BG      lv_color_hex(0x0B0B0D)
#define COL_PANEL   lv_color_hex(0x1A1B1F)
#define COL_PANEL_HI lv_color_hex(0x2A2C33)
#define COL_TEXT    lv_color_hex(0xECECEC)
#define COL_DIM     lv_color_hex(0x7C7F87)
#define COL_ACCENT  lv_color_hex(0xFF7A1A)
#define COL_WARN    lv_color_hex(0xFFC21A)
#define COL_ALARM   lv_color_hex(0xFF3B30)
#define COL_OK      lv_color_hex(0x34C759)

lv_obj_t *ui_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t col, const char *txt);
lv_obj_t *ui_panel(lv_obj_t *parent, int x, int y, int w, int h);

void ui_audio_build(lv_obj_t *page);
void ui_audio_update(const struct hbas_vehicle *v);
/* Returns true if the audio page consumed the key (e.g. while editing). */
bool ui_audio_key(enum ui_key key);

#endif
