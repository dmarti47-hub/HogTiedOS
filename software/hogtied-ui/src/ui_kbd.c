// SPDX-License-Identifier: MIT
/*
 * On-screen keyboard, for naming places and entering addresses. Works by
 * touch (tap a key or a suggestion) and with the handlebar buttons: the
 * arrows move a highlight over the keys, OK presses the highlighted key,
 * Back cancels. Up from the top row of keys goes into the suggestions
 * (autofill), OK there picks one.
 *
 * Keys are plain LVGL objects rather than lv_keyboard, so the highlight
 * and the handlebar navigation are ours.
 */
#include "ui.h"

#include <stdio.h>
#include <string.h>

#define ROWS 4
#define COLS 10
#define TEXT_MAX 48
#define KEY_H 27
#define KB_Y 128

enum { K_CHAR, K_BKSP, K_SHIFT, K_SPACE, K_OK, K_CANCEL };

struct key {
	const char *label;
	int kind;
	int span;                         /* width in key units */
};

/* letters and symbols; row 3 is the same in both */
static const struct key letters[ROWS][COLS] = {
	{ { "Q", K_CHAR, 1 }, { "W", K_CHAR, 1 }, { "E", K_CHAR, 1 }, { "R", K_CHAR, 1 }, { "T", K_CHAR, 1 }, { "Y", K_CHAR, 1 }, { "U", K_CHAR, 1 }, { "I", K_CHAR, 1 }, { "O", K_CHAR, 1 }, { "P", K_CHAR, 1 } },
	{ { "A", K_CHAR, 1 }, { "S", K_CHAR, 1 }, { "D", K_CHAR, 1 }, { "F", K_CHAR, 1 }, { "G", K_CHAR, 1 }, { "H", K_CHAR, 1 }, { "J", K_CHAR, 1 }, { "K", K_CHAR, 1 }, { "L", K_CHAR, 1 },
	  { LV_SYMBOL_BACKSPACE, K_BKSP, 1 } },
	{ { "Z", K_CHAR, 1 }, { "X", K_CHAR, 1 }, { "C", K_CHAR, 1 }, { "V", K_CHAR, 1 }, { "B", K_CHAR, 1 }, { "N", K_CHAR, 1 }, { "M", K_CHAR, 1 }, { "'", K_CHAR, 1 }, { "-", K_CHAR, 1 }, { ".", K_CHAR, 1 } },
	{ {"123", K_SHIFT, 2}, {"Space", K_SPACE, 4}, {"OK", K_OK, 2}, {"Cancel", K_CANCEL, 2} },
};
static const struct key symbols[ROWS][COLS] = {
	{ { "1", K_CHAR, 1 }, { "2", K_CHAR, 1 }, { "3", K_CHAR, 1 }, { "4", K_CHAR, 1 }, { "5", K_CHAR, 1 }, { "6", K_CHAR, 1 }, { "7", K_CHAR, 1 }, { "8", K_CHAR, 1 }, { "9", K_CHAR, 1 }, { "0", K_CHAR, 1 } },
	{ { "#", K_CHAR, 1 }, { "&", K_CHAR, 1 }, { "/", K_CHAR, 1 }, { "(", K_CHAR, 1 }, { ")", K_CHAR, 1 }, { ",", K_CHAR, 1 }, { ":", K_CHAR, 1 }, { "@", K_CHAR, 1 }, { "!", K_CHAR, 1 },
	  { LV_SYMBOL_BACKSPACE, K_BKSP, 1 } },
	{ { "+", K_CHAR, 1 }, { "=", K_CHAR, 1 }, { "?", K_CHAR, 1 }, { "*", K_CHAR, 1 }, { "_", K_CHAR, 1 }, { "$", K_CHAR, 1 }, { "%", K_CHAR, 1 }, { "'", K_CHAR, 1 }, { "-", K_CHAR, 1 }, { ".", K_CHAR, 1 } },
	{ {"ABC", K_SHIFT, 2}, {"Space", K_SPACE, 4}, {"OK", K_OK, 2}, {"Cancel", K_CANCEL, 2} },
};

static lv_obj_t *box, *lbl_title, *lbl_text, *sugg[UI_KBD_SUGGESTIONS];
static lv_obj_t *keys[ROWS][COLS];
static int ncols[ROWS];
static bool open_, sym;
static int row, col;                    /* highlight; row -1 = suggestions */
static int sugg_sel, n_sugg;
static char text[TEXT_MAX + 1];
static void (*changed_cb)(const char *);
static ui_kbd_done_cb done_cb;

static const struct key *key_at(int r, int c)
{
	return sym ? &symbols[r][c] : &letters[r][c];
}

static void show(void)
{
	lv_label_set_text_fmt(lbl_text, "%s_", text);
	for (int r = 0; r < ROWS; r++)
		for (int c = 0; c < ncols[r]; c++) {
			bool hi = row == r && col == c;

			lv_obj_t *l = lv_obj_get_child(keys[r][c], 0);

			lv_label_set_text(l, key_at(r, c)->label);
			lv_obj_set_style_text_color(l, hi ? COL_BG : COL_TEXT, 0);
			lv_obj_set_style_bg_color(keys[r][c], hi ? COL_ACCENT : COL_PANEL_HI, 0);
		}
	for (int i = 0; i < UI_KBD_SUGGESTIONS; i++) {
		bool hi = row < 0 && sugg_sel == i;

		lv_obj_set_style_bg_opa(sugg[i], hi ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
		lv_obj_set_style_text_color(sugg[i], hi ? COL_BG : COL_TEXT, 0);
		if (i < n_sugg)
			lv_obj_remove_flag(sugg[i], LV_OBJ_FLAG_HIDDEN);
		else
			lv_obj_add_flag(sugg[i], LV_OBJ_FLAG_HIDDEN);
	}
}

static void finish(enum ui_kbd_result r, int pick)
{
	ui_kbd_done_cb cb = done_cb;

	open_ = false;
	lv_obj_add_flag(box, LV_OBJ_FLAG_HIDDEN);
	if (cb)
		cb(r, text, pick);
}

static void edited(void)
{
	show();
	if (changed_cb)
		changed_cb(text);
}

static void press(int r, int c)
{
	const struct key *k = key_at(r, c);
	size_t n = strlen(text);

	switch (k->kind) {
	case K_CHAR:
		if (n < TEXT_MAX) {
			char ch = k->label[0];

			/* capitals at the start of words, small letters after */
			if (ch >= 'A' && ch <= 'Z' && n > 0 && text[n - 1] != ' ')
				ch = (char)(ch - 'A' + 'a');
			text[n] = ch;
			text[n + 1] = '\0';
			edited();
		}
		break;
	case K_BKSP:
		if (n) {
			text[n - 1] = '\0';
			edited();
		}
		break;
	case K_SPACE:
		if (n && n < TEXT_MAX && text[n - 1] != ' ') {
			text[n] = ' ';
			text[n + 1] = '\0';
			edited();
		}
		break;
	case K_SHIFT:
		sym = !sym;
		show();
		break;
	case K_OK:
		finish(UI_KBD_OK, -1);
		break;
	case K_CANCEL:
		finish(UI_KBD_CANCEL, -1);
		break;
	}
}

static void key_clicked(lv_event_t *e)
{
	int id = (int)(intptr_t)lv_event_get_user_data(e);

	if (!open_)
		return;
	row = id / COLS;
	col = id % COLS;
	press(row, col);
	if (open_)
		show();
}

static void sugg_clicked(lv_event_t *e)
{
	int i = (int)(intptr_t)lv_event_get_user_data(e);

	if (open_ && i < n_sugg)
		finish(UI_KBD_PICK, i);
}

/* the key under the same spot on another row */
static int col_for(int from_row, int from_col, int to_row)
{
	int x = 0, mid;

	for (int c = 0; c < from_col; c++)
		x += key_at(from_row, c)->span ? key_at(from_row, c)->span : 1;
	mid = x * 2 + (key_at(from_row, from_col)->span ? key_at(from_row, from_col)->span : 1);
	x = 0;
	for (int c = 0; c < ncols[to_row]; c++) {
		int w = key_at(to_row, c)->span ? key_at(to_row, c)->span : 1;

		if (mid < (x + w) * 2)
			return c;
		x += w;
	}
	return ncols[to_row] - 1;
}

bool ui_kbd_key(enum ui_key k)
{
	if (!open_)
		return false;
	switch (k) {
	case UI_KEY_LEFT:
		if (row >= 0)
			col = (col + ncols[row] - 1) % ncols[row];
		break;
	case UI_KEY_RIGHT:
		if (row >= 0)
			col = (col + 1) % ncols[row];
		break;
	case UI_KEY_UP:
		if (row < 0) {
			if (sugg_sel > 0)
				sugg_sel--;
		} else if (row == 0) {
			if (n_sugg) {
				row = -1;
				sugg_sel = n_sugg - 1;
			}
		} else {
			col = col_for(row, col, row - 1);
			row--;
		}
		break;
	case UI_KEY_DOWN:
		if (row < 0) {
			if (sugg_sel < n_sugg - 1)
				sugg_sel++;
			else
				row = 0;
		} else if (row < ROWS - 1) {
			col = col_for(row, col, row + 1);
			row++;
		}
		break;
	case UI_KEY_ENTER:
		if (row < 0)
			finish(UI_KBD_PICK, sugg_sel);
		else
			press(row, col);
		break;
	case UI_KEY_BACK:
		finish(UI_KBD_CANCEL, -1);
		break;
	}
	if (open_)
		show();
	return true;
}

void ui_kbd_suggest(const char *const *labels, int n)
{
	if (n > UI_KBD_SUGGESTIONS)
		n = UI_KBD_SUGGESTIONS;
	n_sugg = n < 0 ? 0 : n;
	for (int i = 0; i < n_sugg; i++)
		lv_label_set_text(sugg[i], labels[i]);
	if (row < 0 && sugg_sel >= n_sugg) {
		if (n_sugg)
			sugg_sel = n_sugg - 1;
		else
			row = 0;
	}
	if (open_)
		show();
}

void ui_kbd_type(const char *s)
{
	size_t n = strlen(text);

	if (!open_)
		return;
	while (*s && n < TEXT_MAX)
		text[n++] = *s++;
	text[n] = '\0';
	edited();
}

bool ui_kbd_active(void)
{
	return open_;
}

void ui_kbd_open(const char *title, const char *initial, void (*changed)(const char *),
		 ui_kbd_done_cb done)
{
	if (!box)
		ui_kbd_build();
	snprintf(text, sizeof(text), "%s", initial ? initial : "");
	changed_cb = changed;
	done_cb = done;
	sym = false;
	row = col = 0;
	n_sugg = sugg_sel = 0;
	lv_label_set_text(lbl_title, title);
	open_ = true;
	lv_obj_remove_flag(box, LV_OBJ_FLAG_HIDDEN);
	lv_obj_move_foreground(box);
	show();
}

void ui_kbd_build(void)
{
	lv_obj_t *field;

	box = lv_obj_create(lv_layer_top());
	lv_obj_remove_style_all(box);
	lv_obj_set_size(box, UI_WIDTH, UI_HEIGHT);
	lv_obj_set_style_bg_color(box, COL_BG, 0);
	lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
	lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);

	lbl_title = ui_label(box, &lv_font_montserrat_14, COL_DIM, "");
	lv_obj_set_pos(lbl_title, 8, 3);
	field = lv_obj_create(box);
	lv_obj_remove_style_all(field);
	lv_obj_set_size(field, UI_WIDTH - 12, 30);
	lv_obj_set_pos(field, 6, 21);
	lv_obj_set_style_bg_color(field, COL_PANEL, 0);
	lv_obj_set_style_bg_opa(field, LV_OPA_COVER, 0);
	lv_obj_set_style_border_side(field, LV_BORDER_SIDE_BOTTOM, 0);
	lv_obj_set_style_border_width(field, 2, 0);
	lv_obj_set_style_border_color(field, COL_ACCENT, 0);
	lbl_text = ui_label(field, &lv_font_montserrat_20, COL_TEXT, "");
	lv_obj_set_pos(lbl_text, 6, 4);
	lv_label_set_long_mode(lbl_text, LV_LABEL_LONG_CLIP);
	lv_obj_set_width(lbl_text, UI_WIDTH - 24);

	for (int i = 0; i < UI_KBD_SUGGESTIONS; i++) {
		sugg[i] = ui_label(box, &lv_font_montserrat_14, COL_TEXT, "");
		lv_obj_set_size(sugg[i], UI_WIDTH - 12, 22);
		lv_obj_set_pos(sugg[i], 6, 54 + i * 24);
		lv_obj_set_style_pad_left(sugg[i], 6, 0);
		lv_obj_set_style_pad_top(sugg[i], 3, 0);
		lv_obj_set_style_radius(sugg[i], 4, 0);
		lv_obj_set_style_bg_color(sugg[i], COL_ACCENT, 0);
		lv_label_set_long_mode(sugg[i], LV_LABEL_LONG_DOT);
		lv_obj_add_flag(sugg[i], LV_OBJ_FLAG_CLICKABLE);
		lv_obj_add_event_cb(sugg[i], sugg_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);
	}

	for (int r = 0; r < ROWS; r++) {
		int x = 0;

		ncols[r] = 0;
		for (int c = 0; c < COLS && letters[r][c].label; c++) {
			int span = letters[r][c].span ? letters[r][c].span : 1;
			lv_obj_t *k = lv_obj_create(box), *l;

			lv_obj_remove_style_all(k);
			lv_obj_set_pos(k, 2 + x * 40, KB_Y + r * (KEY_H + 1));
			lv_obj_set_size(k, span * 40 - 2, KEY_H);
			lv_obj_set_style_bg_opa(k, LV_OPA_COVER, 0);
			lv_obj_set_style_radius(k, 4, 0);
			lv_obj_add_flag(k, LV_OBJ_FLAG_CLICKABLE);
			lv_obj_add_event_cb(k, key_clicked, LV_EVENT_CLICKED,
					    (void *)(intptr_t)(r * COLS + c));
			l = ui_label(k, &lv_font_montserrat_14, COL_TEXT, "");
			lv_obj_center(l);
			keys[r][c] = k;
			ncols[r]++;
			x += span;
		}
	}
	lv_obj_add_flag(box, LV_OBJ_FLAG_HIDDEN);
}
