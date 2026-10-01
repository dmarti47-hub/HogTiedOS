/* SPDX-License-Identifier: MIT */
#ifndef HOGTIED_UI_H
#define HOGTIED_UI_H

#include "hbas/audio.h"
#include "hbas/btproto.h"
#include "hbas/vehicle.h"

#define UI_WIDTH  400     /* premium panel, CROSS_CHECKS.md sec. 7 */
#define UI_HEIGHT 240

/* Navigation keys: handlebars (via hbas-iocd), PC arrow keys, or stdin. */
enum ui_key { UI_KEY_LEFT, UI_KEY_RIGHT, UI_KEY_UP, UI_KEY_DOWN, UI_KEY_ENTER, UI_KEY_BACK };

enum ui_page { UI_PAGE_DASH, UI_PAGE_MEDIA, UI_PAGE_AUDIO, UI_PAGE_EQ, UI_PAGE_TIRES, UI_PAGE_SYSTEM, UI_PAGE_COUNT };

void ui_create(void);
void ui_update(const struct hbas_vehicle *v);
void ui_key(enum ui_key key);
enum ui_page ui_current_page(void);

/* Audio: where settings go (NULL = nowhere), and bike facts that shape it. */
void ui_set_audio_backend(const struct hbas_audio_backend *b);
void ui_set_speaker_count(uint8_t speakers);   /* 2 or 4; fade needs 4 */
/* The bike's configuration (HD_Configuration_Options byte 0 from the IOC,
 * -1 unknown): model name, stock speaker count, trike, factory EQ. */
void ui_set_bike(int cfg);

/*
 * Settings the rider changes (audio and EQ pages), for saving across power
 * cycles. ui_settings_set() restores them (call after ui_create); the stock
 * speaker count and mute are not part of it. ui_settings_changes() goes up
 * on every change the rider makes, so the caller knows when to save.
 */
void ui_settings_get(struct hbas_audio_settings *a, struct hbas_eq *eq);
void ui_settings_set(const struct hbas_audio_settings *a, const struct hbas_eq *eq);
unsigned ui_settings_changes(void);

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
void ui_audio_eq_changed(void);             /* EQ moved: custom-system headroom */
void ui_set_bike_config(int cfg);           /* audio part of ui_set_bike() */
void ui_settings_touch(void);               /* the rider changed a setting */

/* Media page: phone music over Bluetooth via hbas-btd (btproto.h). */
void ui_media_build(lv_obj_t *page);
void ui_media_set_sender(void (*send)(const char *line));   /* commands to hbas-btd */
void ui_media_update(const struct hbas_bt_state *s);
void ui_media_tick(void);                    /* progress bar, toasts */
bool ui_media_key(enum ui_key key);
bool ui_media_pairing_key(enum ui_key key);  /* pairing box takes keys on any page */

void ui_eq_build(lv_obj_t *page);
void ui_eq_set_backend(const struct hbas_audio_backend *b);
bool ui_eq_key(enum ui_key key);
const struct hbas_eq *ui_eq_current(void);
void ui_eq_set(const struct hbas_eq *eq);
/* What the Harley preset follows (from the audio page). */
void ui_eq_set_harley_context(int bike_cfg, bool headset, bool engine_on, unsigned vol_step);

#endif
