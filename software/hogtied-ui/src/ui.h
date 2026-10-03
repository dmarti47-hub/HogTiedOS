/* SPDX-License-Identifier: MIT */
#ifndef HOGTIED_UI_H
#define HOGTIED_UI_H

#include "hbas/audio.h"
#include "hbas/btproto.h"
#include "hbas/gpsproto.h"
#include "hbas/vehicle.h"

#define UI_WIDTH  400     /* premium panel, CROSS_CHECKS.md sec. 7 */
#define UI_HEIGHT 240

/* Navigation keys: handlebars (via hbas-iocd), PC arrow keys, or stdin. */
enum ui_key { UI_KEY_LEFT, UI_KEY_RIGHT, UI_KEY_UP, UI_KEY_DOWN, UI_KEY_ENTER, UI_KEY_BACK };

/*
 * Pages. HOME is the audio home (where the bezel/handlebar Home button
 * returns to); the others are reached by on-screen buttons. Speed and rpm
 * live on the bike's own gauges, not here.
 */
enum ui_page { UI_PAGE_HOME, UI_PAGE_NAV, UI_PAGE_MEDIA, UI_PAGE_INFO, UI_PAGE_SETTINGS,
	       UI_PAGE_AUDIO, UI_PAGE_EQ, UI_PAGE_COUNT };

void ui_create(void);
void ui_update(const struct hbas_vehicle *v);
void ui_key(enum ui_key key);
enum ui_page ui_current_page(void);
void ui_goto(enum ui_page page);              /* direct navigation (tests/snapshots) */

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

/*
 * Design system (refreshed): a high-contrast dark theme for a bike in
 * sunlight. Surfaces step up in lightness (BG < SURFACE < SURFACE_HI);
 * ACCENT is the Harley orange. Colours are tokens so a restyle is one edit.
 */
#define COL_BG       lv_color_hex(0x0A0B0E)   /* screen */
#define COL_SURFACE  lv_color_hex(0x16181D)   /* cards */
#define COL_SURFACE_HI lv_color_hex(0x242831) /* raised / selected */
#define COL_LINE     lv_color_hex(0x2C313B)   /* hairline borders */
#define COL_TEXT     lv_color_hex(0xF2F4F7)
#define COL_DIM      lv_color_hex(0x8A909B)   /* secondary text */
#define COL_ACCENT   lv_color_hex(0xFF7A1A)   /* Harley orange */
#define COL_ACCENT_DK lv_color_hex(0x7A3A0C)  /* dim orange fill */
#define COL_WARN     lv_color_hex(0xFFC21A)
#define COL_ALARM    lv_color_hex(0xFF453A)
#define COL_OK       lv_color_hex(0x32D74B)
#define COL_COOL     lv_color_hex(0x3B82F6)   /* cold end of temp gauges */
#define COL_HOT      lv_color_hex(0xFF453A)   /* hot end */
/* back-compat alias: old pages said COL_PANEL / COL_PANEL_HI */
#define COL_PANEL    COL_SURFACE
#define COL_PANEL_HI COL_SURFACE_HI

/* Type scale */
#define FONT_HERO   &lv_font_montserrat_48    /* speed */
#define FONT_XL     &lv_font_montserrat_36
#define FONT_LG     &lv_font_montserrat_28
#define FONT_MD     &lv_font_montserrat_24
#define FONT_RG     &lv_font_montserrat_20
#define FONT_SM     &lv_font_montserrat_16
#define FONT_XS     &lv_font_montserrat_14
#define FONT_TINY   &lv_font_montserrat_12

lv_obj_t *ui_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t col, const char *txt);
lv_obj_t *ui_panel(lv_obj_t *parent, int x, int y, int w, int h);

/* ---- touch widget toolkit (ui_widgets.c) -------------------------------- */

/* A rounded surface card. */
lv_obj_t *ui_card(lv_obj_t *parent, int x, int y, int w, int h);

/*
 * A bar gauge with coloured zones, for temperature / rpm and the like.
 * Vertical if h > w, else horizontal. Set the value and optional warn/alarm
 * thresholds; the fill turns amber then red past them.
 */
struct ui_gauge {
	lv_obj_t *bar, *fill, *lbl_val, *lbl_cap;
	int min, max, warn, alarm;
	bool vertical;
};
void ui_gauge_init(struct ui_gauge *g, lv_obj_t *parent, int x, int y, int w, int h,
		   const char *caption);
void ui_gauge_range(struct ui_gauge *g, int min, int max, int warn, int alarm);
/* value in gauge units; txt is what the number cell shows (NULL hides it). */
void ui_gauge_set(struct ui_gauge *g, int value, const char *txt);
void ui_gauge_set_unknown(struct ui_gauge *g);

/* A tap target that calls cb(user) when pressed; returns the card. */
lv_obj_t *ui_button(lv_obj_t *parent, int x, int y, int w, int h, const char *label,
		    void (*cb)(void *), void *user);

void ui_audio_build(lv_obj_t *page);
void ui_audio_update(const struct hbas_vehicle *v);
/* Returns true if the audio page consumed the key (e.g. while editing). */
bool ui_audio_key(enum ui_key key);
void ui_audio_eq_changed(void);             /* EQ moved: custom-system headroom */
void ui_set_bike_config(int cfg);           /* audio part of ui_set_bike() */
void ui_settings_touch(void);               /* the rider changed a setting */

/* Media page: phone music over Bluetooth via hbas-btd (btproto.h). */
/* Home page shows the Bluetooth now-playing at a glance (called from media). */
void ui_home_now_playing(const char *track, const char *artist, bool connected);
void ui_media_build(lv_obj_t *page);
void ui_media_set_sender(void (*send)(const char *line));   /* commands to hbas-btd */
void ui_media_update(const struct hbas_bt_state *s);
void ui_media_tick(void);                    /* progress bar, toasts */
bool ui_media_key(enum ui_key key);
bool ui_media_pairing_key(enum ui_key key);  /* pairing box takes keys on any page */

/* Map page: offline map (hbas-map) centred on the GPS fix. */
void ui_map_build(lv_obj_t *page);
void ui_map_open(const char *db, const char *style, const char *font);   /* NULL db: none */
void ui_map_close(void);
void ui_map_tick(void);                      /* swap in finished frames */
bool ui_map_has_frame(void);
void ui_map_gps(const struct hbas_gps_view *v);
bool ui_map_key(enum ui_key key);
void ui_map_places(const char *path);         /* saved places file; NULL: none */
void ui_map_add_place(const char *name, double lat, double lon);   /* not saved */
bool ui_map_idle(void);                       /* no route or frame in progress */
void ui_map_set_metric(bool metric);          /* follows the bike's unit setting */
bool ui_map_next_turn(char *buf, size_t len); /* "300 m: Turn left onto ..." */

/* On-screen keyboard (touch and handlebar buttons), with autofill rows. */
#define UI_KBD_SUGGESTIONS 3
enum ui_kbd_result { UI_KBD_CANCEL, UI_KBD_OK, UI_KBD_PICK };
typedef void (*ui_kbd_done_cb)(enum ui_kbd_result r, const char *text, int pick);
void ui_kbd_build(void);
/* changed (may be NULL) is called after every edit, e.g. to search */
void ui_kbd_open(const char *title, const char *initial, void (*changed)(const char *),
		 ui_kbd_done_cb done);
void ui_kbd_suggest(const char *const *labels, int n);
bool ui_kbd_active(void);
void ui_kbd_type(const char *s);              /* as if typed (scripts, tests) */
bool ui_kbd_key(enum ui_key key);

/* GPS page: hbas-gpsd's view of the u-blox receiver (gpsproto.h). */
void ui_gps_build(lv_obj_t *page);
void ui_gps_update(const struct hbas_gps_view *v);
void ui_gps_set_metric(bool metric);         /* follows the bike's unit setting */

void ui_eq_build(lv_obj_t *page);
void ui_eq_set_backend(const struct hbas_audio_backend *b);
bool ui_eq_key(enum ui_key key);
const struct hbas_eq *ui_eq_current(void);
void ui_eq_set(const struct hbas_eq *eq);
/* What the Harley preset follows (from the audio page). */
void ui_eq_set_harley_context(int bike_cfg, bool headset, bool engine_on, unsigned vol_step);

#endif
