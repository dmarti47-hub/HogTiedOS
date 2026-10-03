// SPDX-License-Identifier: MIT
/*
 * Shared touch widgets for the refreshed UI: cards, bar gauges and tap
 * buttons. Kept small and style-token driven (ui.h) so the look is one edit.
 */
#include "ui.h"

#include <stdio.h>

lv_obj_t *ui_card(lv_obj_t *parent, int x, int y, int w, int h)
{
	lv_obj_t *c = lv_obj_create(parent);

	lv_obj_remove_style_all(c);
	lv_obj_set_pos(c, x, y);
	lv_obj_set_size(c, w, h);
	lv_obj_set_style_bg_color(c, COL_SURFACE, 0);
	lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(c, 10, 0);
	lv_obj_set_style_border_color(c, COL_LINE, 0);
	lv_obj_set_style_border_width(c, 1, 0);
	lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
	return c;
}

/* ---- bar gauge ---------------------------------------------------------- */

static lv_color_t gauge_color(const struct ui_gauge *g, int v)
{
	if (g->alarm > g->min && v >= g->alarm)
		return COL_ALARM;
	if (g->warn > g->min && v >= g->warn)
		return COL_WARN;
	return COL_ACCENT;
}

void ui_gauge_init(struct ui_gauge *g, lv_obj_t *parent, int x, int y, int w, int h,
		   const char *caption)
{
	g->vertical = h > w;
	g->min = 0;
	g->max = 100;
	g->warn = g->alarm = 0;

	g->lbl_cap = ui_label(parent, FONT_TINY, COL_DIM, caption ? caption : "");
	g->lbl_val = ui_label(parent, FONT_RG, COL_TEXT, "--");

	/* the track */
	g->bar = lv_obj_create(parent);
	lv_obj_remove_style_all(g->bar);
	lv_obj_set_style_bg_color(g->bar, COL_BG, 0);
	lv_obj_set_style_bg_opa(g->bar, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(g->bar, LV_RADIUS_CIRCLE, 0);
	lv_obj_remove_flag(g->bar, LV_OBJ_FLAG_SCROLLABLE);
	/* the fill */
	g->fill = lv_obj_create(g->bar);
	lv_obj_remove_style_all(g->fill);
	lv_obj_set_style_bg_color(g->fill, COL_ACCENT, 0);
	lv_obj_set_style_bg_opa(g->fill, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(g->fill, LV_RADIUS_CIRCLE, 0);

	if (g->vertical) {
		int bw = 8;

		lv_obj_set_size(g->bar, bw, h - 38);
		lv_obj_set_pos(g->bar, x + (w - bw) / 2, y + 16);
		lv_obj_set_width(g->fill, bw);
		lv_obj_align(g->fill, LV_ALIGN_BOTTOM_MID, 0, 0);
		lv_obj_align_to(g->lbl_cap, g->bar, LV_ALIGN_OUT_TOP_MID, 0, -2);
		lv_obj_align(g->lbl_val, LV_ALIGN_TOP_LEFT, 0, 0);
		lv_obj_align_to(g->lbl_val, g->bar, LV_ALIGN_OUT_BOTTOM_MID, 0, 4);
	} else {
		int bh = 8;

		lv_obj_set_size(g->bar, w, bh);
		lv_obj_set_pos(g->bar, x, y + 20);
		lv_obj_set_height(g->fill, bh);
		lv_obj_align(g->fill, LV_ALIGN_LEFT_MID, 0, 0);
		lv_obj_set_pos(g->lbl_cap, x, y);
		lv_obj_align(g->lbl_val, LV_ALIGN_TOP_RIGHT, 0, 0);
		lv_obj_set_pos(g->lbl_val, x + w - 48, y - 2);
	}
	ui_gauge_set_unknown(g);
}

void ui_gauge_range(struct ui_gauge *g, int min, int max, int warn, int alarm)
{
	g->min = min;
	g->max = max > min ? max : min + 1;
	g->warn = warn;
	g->alarm = alarm;
}

void ui_gauge_set(struct ui_gauge *g, int value, const char *txt)
{
	int span = g->max - g->min;
	int v = value < g->min ? g->min : value > g->max ? g->max : value;
	int pct = (v - g->min) * 100 / span;

	lv_obj_set_style_bg_color(g->fill, gauge_color(g, value), 0);
	if (g->vertical) {
		int full = lv_obj_get_height(g->bar);

		lv_obj_set_height(g->fill, full * pct / 100);
	} else {
		int full = lv_obj_get_width(g->bar);

		lv_obj_set_width(g->fill, full * pct / 100);
	}
	lv_obj_set_style_text_color(g->lbl_val, value >= g->alarm && g->alarm > g->min ? COL_ALARM
				    : COL_TEXT, 0);
	if (txt) {
		lv_label_set_text(g->lbl_val, txt);
		lv_obj_remove_flag(g->lbl_val, LV_OBJ_FLAG_HIDDEN);
	} else {
		lv_obj_add_flag(g->lbl_val, LV_OBJ_FLAG_HIDDEN);
	}
}

void ui_gauge_set_unknown(struct ui_gauge *g)
{
	if (g->vertical)
		lv_obj_set_height(g->fill, 0);
	else
		lv_obj_set_width(g->fill, 0);
	lv_label_set_text(g->lbl_val, "--");
	lv_obj_set_style_text_color(g->lbl_val, COL_DIM, 0);
	lv_obj_remove_flag(g->lbl_val, LV_OBJ_FLAG_HIDDEN);
}

/* ---- tap button --------------------------------------------------------- */

struct btn_cb { void (*cb)(void *); void *user; };

static void btn_event(lv_event_t *e)
{
	struct btn_cb *b = lv_event_get_user_data(e);

	if (b->cb)
		b->cb(b->user);
}

static void btn_delete(lv_event_t *e)
{
	lv_free(lv_event_get_user_data(e));
}

lv_obj_t *ui_button(lv_obj_t *parent, int x, int y, int w, int h, const char *label,
		    void (*cb)(void *), void *user)
{
	lv_obj_t *c = ui_card(parent, x, y, w, h);
	lv_obj_t *l = ui_label(c, FONT_SM, COL_TEXT, label);
	struct btn_cb *b = lv_malloc(sizeof(*b));

	b->cb = cb;
	b->user = user;
	lv_obj_center(l);
	lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_style_bg_color(c, COL_SURFACE_HI, LV_STATE_PRESSED);
	lv_obj_set_style_border_color(c, COL_ACCENT, LV_STATE_PRESSED);
	lv_obj_add_event_cb(c, btn_event, LV_EVENT_CLICKED, b);
	lv_obj_add_event_cb(c, btn_delete, LV_EVENT_DELETE, b);
	return c;
}
