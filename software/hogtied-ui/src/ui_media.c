// SPDX-License-Identifier: MIT
/*
 * Media page: music from the paired phone over Bluetooth (hbas-btd, which
 * talks to BlueZ; the audio itself is A2DP via bluez-alsa). Shows the
 * phone, track and progress, with previous / play-pause / next / pair.
 *
 * Keys: Up/Down pick a button, OK presses it (play/pause is selected by
 * default, so OK alone toggles playback). Left/Right switch pages (ui.c).
 *
 * Pairing: when a phone asks to pair, a box with its 6-digit code appears
 * over any page: OK accepts (if the phone shows the same code), Back
 * rejects.
 */
#include "ui.h"

#include <stdio.h>
#include <string.h>

enum { BTN_PREV, BTN_PLAY, BTN_NEXT, BTN_PAIR, BTN_COUNT };

static struct hbas_bt_state bt;
static void (*send_cmd)(const char *line);
static int sel = BTN_PLAY;
static uint32_t pos_ms_at, pos_base_ms;    /* local clock for the progress bar */

static lv_obj_t *lbl_phone, *lbl_title, *lbl_artist, *lbl_album, *lbl_time, *bar_pos;
static lv_obj_t *btn[BTN_COUNT], *btn_lbl[BTN_COUNT], *lbl_hint;
static lv_obj_t *pair_box, *lbl_pair, *lbl_toast;
static uint32_t toast_until;

static void cmd(const char *line)
{
	if (send_cmd)
		send_cmd(line);
}

static void fmt_time(char *buf, size_t n, uint32_t ms)
{
	unsigned s = ms / 1000;

	snprintf(buf, n, "%u:%02u", s / 60, s % 60);
}

static uint32_t position_now(void)
{
	uint32_t p = pos_base_ms;

	if (bt.status == HBAS_BT_PLAYING)
		p += lv_tick_elaps(pos_ms_at);      /* phones only report on changes */
	if (bt.duration_ms && p > bt.duration_ms)
		p = bt.duration_ms;
	return p;
}

static void refresh_progress(void)
{
	char a[12], b[12];
	uint32_t p = position_now();

	if (!bt.player || !bt.duration_ms) {
		lv_obj_add_flag(bar_pos, LV_OBJ_FLAG_HIDDEN);
		lv_label_set_text(lbl_time, "");
		return;
	}
	lv_obj_remove_flag(bar_pos, LV_OBJ_FLAG_HIDDEN);
	lv_bar_set_range(bar_pos, 0, (int32_t)(bt.duration_ms / 1000));
	lv_bar_set_value(bar_pos, (int32_t)(p / 1000), LV_ANIM_OFF);
	fmt_time(a, sizeof(a), p);
	fmt_time(b, sizeof(b), bt.duration_ms);
	lv_label_set_text_fmt(lbl_time, "%s / %s", a, b);
}

static void refresh(void)
{
	static const char *const sym[BTN_COUNT] = {
		LV_SYMBOL_PREV, LV_SYMBOL_PLAY, LV_SYMBOL_NEXT, LV_SYMBOL_BLUETOOTH " Pair",
	};

	if (!bt.daemon)
		lv_label_set_text(lbl_phone, LV_SYMBOL_BLUETOOTH " Bluetooth service not running");
	else if (!bt.powered)
		lv_label_set_text(lbl_phone, LV_SYMBOL_BLUETOOTH " Bluetooth is off");
	else if (bt.connected)
		lv_label_set_text_fmt(lbl_phone, LV_SYMBOL_BLUETOOTH " %s", bt.device[0] ? bt.device
											: "Phone");
	else if (bt.pairable)
		lv_label_set_text(lbl_phone, LV_SYMBOL_BLUETOOTH " Visible: pair from your phone");
	else if (!bt.agent)
		lv_label_set_text(lbl_phone, LV_SYMBOL_BLUETOOTH " No phone: pair it in this PC's settings");
	else
		lv_label_set_text(lbl_phone, LV_SYMBOL_BLUETOOTH " No phone connected");

	if (bt.player && bt.title[0]) {
		lv_label_set_text(lbl_title, bt.title);
		lv_label_set_text(lbl_artist, bt.artist);
		lv_label_set_text(lbl_album, bt.album);
	} else {
		lv_label_set_text(lbl_title, bt.connected ? "Nothing playing" : "");
		lv_label_set_text(lbl_artist, bt.connected && !bt.player
					      ? "Phone has no music controls" : "");
		lv_label_set_text(lbl_album, "");
	}
	refresh_progress();

	for (int i = 0; i < BTN_COUNT; i++) {
		bool hi = i == sel;
		bool enabled = i == BTN_PAIR ? bt.daemon && bt.agent : bt.player;

		lv_label_set_text(btn_lbl[i], i == BTN_PLAY && bt.status == HBAS_BT_PLAYING
					      ? LV_SYMBOL_PAUSE : sym[i]);
		lv_obj_set_style_bg_color(btn[i], hi ? COL_PANEL_HI : COL_PANEL, 0);
		lv_obj_set_style_border_width(btn[i], hi ? 2 : 0, 0);
		lv_obj_set_style_text_color(btn_lbl[i], !enabled ? COL_DIM : hi ? COL_ACCENT
										: COL_TEXT, 0);
	}

	if (bt.pair_request) {
		lv_label_set_text_fmt(lbl_pair, "Pair with %s?\n\n%03u %03u\n\n"
				      "Check the phone shows the same code.\n"
				      "OK = pair    Back = cancel",
				      bt.pair_device[0] ? bt.pair_device : "phone",
				      bt.pair_passkey / 1000, bt.pair_passkey % 1000);
		lv_obj_remove_flag(pair_box, LV_OBJ_FLAG_HIDDEN);
	} else {
		lv_obj_add_flag(pair_box, LV_OBJ_FLAG_HIDDEN);
	}
}

void ui_media_build(lv_obj_t *p)
{
	lbl_phone = ui_label(p, &lv_font_montserrat_14, COL_DIM, "");
	lv_obj_set_pos(lbl_phone, 12, 34);

	lbl_title = ui_label(p, &lv_font_montserrat_20, COL_TEXT, "");
	lv_obj_set_pos(lbl_title, 12, 62);
	lv_obj_set_width(lbl_title, 376);
	lv_label_set_long_mode(lbl_title, LV_LABEL_LONG_SCROLL_CIRCULAR);
	lbl_artist = ui_label(p, &lv_font_montserrat_14, COL_TEXT, "");
	lv_obj_set_pos(lbl_artist, 12, 92);
	lv_obj_set_width(lbl_artist, 376);
	lv_label_set_long_mode(lbl_artist, LV_LABEL_LONG_DOT);
	lbl_album = ui_label(p, &lv_font_montserrat_14, COL_DIM, "");
	lv_obj_set_pos(lbl_album, 12, 112);
	lv_obj_set_width(lbl_album, 376);
	lv_label_set_long_mode(lbl_album, LV_LABEL_LONG_DOT);

	bar_pos = lv_bar_create(p);
	lv_obj_set_size(bar_pos, 280, 6);
	lv_obj_set_pos(bar_pos, 12, 146);
	lv_obj_set_style_bg_color(bar_pos, COL_PANEL_HI, LV_PART_MAIN);
	lv_obj_set_style_bg_color(bar_pos, COL_ACCENT, LV_PART_INDICATOR);
	lbl_time = ui_label(p, &lv_font_montserrat_14, COL_DIM, "");
	lv_obj_align(lbl_time, LV_ALIGN_TOP_RIGHT, -12, 140);

	for (int i = 0; i < BTN_COUNT; i++) {
		int w = i == BTN_PAIR ? 100 : 72;

		btn[i] = ui_panel(p, 12 + i * 80, 168, w, 40);
		lv_obj_set_style_border_color(btn[i], COL_ACCENT, 0);
		btn_lbl[i] = ui_label(btn[i], &lv_font_montserrat_20, COL_TEXT, "");
		lv_obj_center(btn_lbl[i]);
	}
	lv_obj_set_style_text_font(btn_lbl[BTN_PAIR], &lv_font_montserrat_14, 0);
	lbl_hint = ui_label(p, &lv_font_montserrat_14, COL_DIM,
			    LV_SYMBOL_UP LV_SYMBOL_DOWN " select  OK press");
	lv_obj_align(lbl_hint, LV_ALIGN_BOTTOM_RIGHT, -12, -6);

	/* pairing box and result toast float over every page */
	pair_box = ui_panel(lv_layer_top(), 30, 30, 340, 180);
	lv_obj_set_style_border_width(pair_box, 2, 0);
	lv_obj_set_style_border_color(pair_box, COL_ACCENT, 0);
	lbl_pair = ui_label(pair_box, &lv_font_montserrat_14, COL_TEXT, "");
	lv_obj_set_style_text_align(lbl_pair, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_center(lbl_pair);
	lv_obj_add_flag(pair_box, LV_OBJ_FLAG_HIDDEN);
	lbl_toast = ui_label(lv_layer_top(), &lv_font_montserrat_14, COL_TEXT, "");
	lv_obj_set_style_bg_color(lbl_toast, COL_PANEL_HI, 0);
	lv_obj_set_style_bg_opa(lbl_toast, LV_OPA_COVER, 0);
	lv_obj_set_style_pad_all(lbl_toast, 8, 0);
	lv_obj_set_style_radius(lbl_toast, 6, 0);
	lv_obj_align(lbl_toast, LV_ALIGN_BOTTOM_MID, 0, -30);
	lv_obj_add_flag(lbl_toast, LV_OBJ_FLAG_HIDDEN);

	hbas_bt_state_init(&bt);
	refresh();
}

void ui_media_set_sender(void (*send)(const char *line))
{
	send_cmd = send;
}

static void toast(const char *text)
{
	lv_label_set_text(lbl_toast, text);
	lv_obj_remove_flag(lbl_toast, LV_OBJ_FLAG_HIDDEN);
	toast_until = lv_tick_get() + 3000;
}

void ui_media_update(const struct hbas_bt_state *s)
{
	bool was_request = bt.pair_request;
	char result[sizeof(bt.pair_result)];

	snprintf(result, sizeof(result), "%s", bt.pair_result);
	if (s->position_ms != bt.position_ms || s->status != bt.status) {
		pos_base_ms = s->position_ms;
		pos_ms_at = lv_tick_get();
	}
	bt = *s;
	if (bt.pair_result[0] && (strcmp(result, bt.pair_result) || was_request)) {
		if (!strcmp(bt.pair_result, "ok"))
			toast(LV_SYMBOL_OK " Phone paired");
		else if (strcmp(bt.pair_result, "rejected"))
			toast("Pairing did not complete");
	}
	refresh();
}

void ui_media_tick(void)
{
	static uint32_t last;

	if (toast_until && lv_tick_get() >= toast_until) {
		lv_obj_add_flag(lbl_toast, LV_OBJ_FLAG_HIDDEN);
		toast_until = 0;
	}
	if (bt.status == HBAS_BT_PLAYING && lv_tick_elaps(last) >= 500) {
		last = lv_tick_get();
		refresh_progress();
	}
}

bool ui_media_pairing_key(enum ui_key key)
{
	if (!bt.pair_request)
		return false;
	if (key == UI_KEY_ENTER || key == UI_KEY_BACK) {
		cmd(key == UI_KEY_ENTER ? "confirm yes\n" : "confirm no\n");
		bt.pair_request = false;               /* hide now; daemon confirms */
		refresh();
	}
	return true;                                /* the box has the keys */
}

bool ui_media_key(enum ui_key key)
{
	switch (key) {
	case UI_KEY_UP: sel = (sel + BTN_COUNT - 1) % BTN_COUNT; break;
	case UI_KEY_DOWN: sel = (sel + 1) % BTN_COUNT; break;
	case UI_KEY_ENTER:
		if (sel != BTN_PAIR && !bt.player)
			break;                      /* greyed out: no phone controls */
		if (sel == BTN_PAIR && !(bt.daemon && bt.agent))
			break;                      /* PC: pair from its own settings */
		switch (sel) {
		case BTN_PREV: cmd("previous\n"); break;
		case BTN_NEXT: cmd("next\n"); break;
		case BTN_PLAY: cmd(bt.status == HBAS_BT_PLAYING ? "pause\n" : "play\n"); break;
		case BTN_PAIR:
			cmd(bt.pairable ? "pairable off\n" : "pairable on\n");
			break;
		}
		break;
	default:
		return false;                       /* Left/Right/Back: page navigation */
	}
	refresh();
	return true;
}
