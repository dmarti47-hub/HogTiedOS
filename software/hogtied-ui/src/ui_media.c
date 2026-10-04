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
#include "persist.h"

#include "hbas/settings.h"
#include "hbas/tunerproto.h"

#include <stdio.h>
#include <string.h>

enum { BTN_PREV, BTN_PLAY, BTN_NEXT, BTN_PAIR, BTN_COUNT };

static struct hbas_bt_state bt;
static void (*send_cmd)(const char *line);
static int sel = BTN_PLAY;
static void btn_clicked(lv_event_t *e);
static void bt_refresh(void);
static uint32_t pos_ms_at, pos_base_ms;    /* local clock for the progress bar */

static lv_obj_t *lbl_phone, *lbl_title, *lbl_artist, *lbl_album, *lbl_time, *bar_pos;
static lv_obj_t *btn[BTN_COUNT], *btn_lbl[BTN_COUNT], *lbl_hint;
static lv_obj_t *bt_view, *radio_view, *chip_bt, *chip_radio;
enum { SRC_BT, SRC_RADIO };
static int source = SRC_BT;
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
		ui_home_now_playing(bt.title, bt.artist, bt.connected);
	} else {
		ui_home_now_playing(NULL, NULL, bt.connected);
		lv_label_set_text(lbl_title, bt.connected ? "Nothing playing" : "");
		lv_label_set_text(lbl_artist, bt.connected && !bt.player
					      ? "Phone has no music controls" : "");
		lv_label_set_text(lbl_album, "");
	}
	refresh_progress();
	bt_refresh();

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

	lv_obj_set_style_bg_color(chip_bt, source == SRC_BT ? COL_ACCENT : COL_SURFACE, 0);
	lv_obj_set_style_text_color(lv_obj_get_child(chip_bt, 0),
				    source == SRC_BT ? COL_BG : COL_DIM, 0);
	lv_obj_set_style_bg_color(chip_radio, source == SRC_RADIO ? COL_ACCENT : COL_SURFACE, 0);
	lv_obj_set_style_text_color(lv_obj_get_child(chip_radio, 0),
				    source == SRC_RADIO ? COL_BG : COL_DIM, 0);

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

static void src_cb(lv_event_t *e);

static lv_obj_t *source_chip(lv_obj_t *p, int x, const char *label, int which)
{
	lv_obj_t *c = ui_card(p, x, 6, 86, 26);
	lv_obj_t *l = ui_label(c, FONT_XS, COL_DIM, label);

	lv_obj_center(l);
	lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_event_cb(c, src_cb, LV_EVENT_CLICKED, (void *)(intptr_t)which);
	return c;
}

static void set_source(int s)
{
	source = s;
	(s == SRC_BT ? lv_obj_remove_flag : lv_obj_add_flag)(bt_view, LV_OBJ_FLAG_HIDDEN);
	(s == SRC_RADIO ? lv_obj_remove_flag : lv_obj_add_flag)(radio_view, LV_OBJ_FLAG_HIDDEN);
	refresh();
}

static void src_cb(lv_event_t *e)
{
	set_source((int)(intptr_t)lv_event_get_user_data(e));
}

/* ---- radio (AM/FM/WB via hbas-tunerd) ---------------------------------- */

#define N_PRESET 6
static struct hbas_tuner_state tuner;
static void (*tuner_send)(const char *line);
static lv_obj_t *band_chip[HBAS_BAND_COUNT], *lbl_freq, *lbl_unit, *lbl_rds_ps, *lbl_rds_rt;
static lv_obj_t *lbl_sig, *preset_btn[N_PRESET], *preset_lbl[N_PRESET];
static int presets[HBAS_BAND_COUNT][N_PRESET];   /* saved freqs, 0 = empty */
static const char *presets_path;

static void tuner_cmd_raw(const char *line)
{
	if (tuner_send)
		tuner_send(line);
}

static void save_presets(void)
{
	char buf[512];
	int off = snprintf(buf, sizeof(buf), "# HogTiedOS radio 1\n");

	for (int b = 0; b < HBAS_BAND_COUNT; b++)
		for (int i = 0; i < N_PRESET; i++)
			if (presets[b][i])
				off += snprintf(buf + off, sizeof(buf) - off, "preset %s %d %d\n",
						hbas_band_name(b), i, presets[b][i]);
	if (presets_path)
		persist_write_file(presets_path, buf);
}

void ui_tuner_load_presets(const char *path)
{
	char buf[512], band[4];
	int i, freq;
	const char *line;

	presets_path = path;
	memset(presets, 0, sizeof(presets));
	if (!path || hbas_settings_load(path, buf, sizeof(buf)))
		return;
	for (line = buf; *line; ) {
		const char *nl = strchr(line, '\n');

		if (sscanf(line, "preset %3s %d %d", band, &i, &freq) == 3 && i >= 0 && i < N_PRESET)
			for (int b = 0; b < HBAS_BAND_COUNT; b++)
				if (!strcmp(band, hbas_band_name(b)))
					presets[b][i] = freq;
		line = nl ? nl + 1 : line + strlen(line);
	}
}

static void radio_refresh(void)
{
	char fs[16], sig[48];
	const char *unit;

	for (int b = 0; b < HBAS_BAND_COUNT; b++) {
		bool on = (int)tuner.band == b;

		lv_obj_set_style_bg_color(band_chip[b], on ? COL_ACCENT : COL_SURFACE, 0);
		lv_obj_set_style_text_color(lv_obj_get_child(band_chip[b], 0), on ? COL_BG : COL_DIM, 0);
	}
	hbas_tuner_freq_str(&tuner, fs, sizeof(fs), &unit);
	lv_label_set_text(lbl_freq, fs);
	lv_label_set_text(lbl_unit, unit);
	lv_obj_update_layout(lbl_freq);
	lv_obj_align_to(lbl_unit, lbl_freq, LV_ALIGN_OUT_RIGHT_BOTTOM, 6, -10);
	lv_label_set_text(lbl_rds_ps, tuner.ps[0] ? tuner.ps : hbas_band_name(tuner.band));
	lv_label_set_text(lbl_rds_rt, tuner.rt);
	if (!tuner.daemon)
		snprintf(sig, sizeof(sig), "radio service not running");
	else
		snprintf(sig, sizeof(sig), "%s   signal %d", tuner.stereo ? "STEREO" : "mono",
			 tuner.rssi);
	lv_label_set_text(lbl_sig, sig);

	for (int i = 0; i < N_PRESET; i++) {
		int f = presets[tuner.band][i];

		if (f) {
			struct hbas_tuner_state tmp = tuner;
			char ps[16];

			tmp.freq = f;
			hbas_tuner_freq_str(&tmp, ps, sizeof(ps), NULL);
			lv_label_set_text(preset_lbl[i], ps);
			lv_obj_set_style_text_color(preset_lbl[i], f == tuner.freq ? COL_ACCENT
						    : COL_TEXT, 0);
		} else {
			lv_label_set_text_fmt(preset_lbl[i], "P%d", i + 1);
			lv_obj_set_style_text_color(preset_lbl[i], COL_DIM, 0);
		}
	}
}

void ui_tuner_apply(const char *line)
{
	if (hbas_tuner_apply(&tuner, line))
		radio_refresh();
}

void ui_tuner_set_sender(void (*send)(const char *line))
{
	tuner_send = send;
}

static void set_band(enum hbas_band b)
{
	char cmd[32];

	hbas_tuner_cmd_band(cmd, sizeof(cmd), b);
	tuner_cmd_raw(cmd);
}

static void do_seek(bool up)
{
	char cmd[32];

	hbas_tuner_cmd_seek(cmd, sizeof(cmd), up);
	tuner_cmd_raw(cmd);
}

static void do_tune(int freq)
{
	char cmd[32];

	hbas_tuner_cmd_tune(cmd, sizeof(cmd), freq);
	tuner_cmd_raw(cmd);
}

static void band_cb(lv_event_t *e)
{
	set_band((enum hbas_band)(intptr_t)lv_event_get_user_data(e));
}

static void preset_cb(lv_event_t *e)
{
	int i = (int)(intptr_t)lv_event_get_user_data(e);
	bool long_press = lv_event_get_code(e) == LV_EVENT_LONG_PRESSED;

	if (long_press) {
		presets[tuner.band][i] = tuner.freq;   /* save current station */
		save_presets();
		radio_refresh();
	} else if (presets[tuner.band][i]) {
		do_tune(presets[tuner.band][i]);       /* recall */
	}
}

static void step_cb(lv_event_t *e)
{
	int dir = (int)(intptr_t)lv_event_get_user_data(e);

	do_tune(hbas_tuner_step(tuner.band, tuner.freq, dir));
}

static void seek_cb(lv_event_t *e)
{
	do_seek((int)(intptr_t)lv_event_get_user_data(e) > 0);
}

static lv_obj_t *radio_chip(lv_obj_t *p, int x, int y, int w, const char *lbl,
			    lv_event_cb_t cb, int data, lv_event_code_t extra)
{
	lv_obj_t *c = ui_card(p, x, y, w, 30);
	lv_obj_t *l = ui_label(c, FONT_SM, COL_TEXT, lbl);

	lv_obj_center(l);
	lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_style_bg_color(c, COL_SURFACE_HI, LV_STATE_PRESSED);
	lv_obj_add_event_cb(c, cb, LV_EVENT_CLICKED, (void *)(intptr_t)data);
	if (extra)
		lv_obj_add_event_cb(c, cb, extra, (void *)(intptr_t)data);
	return c;
}

static void build_radio(lv_obj_t *parent)
{
	static const char *const bn[HBAS_BAND_COUNT] = { "FM", "AM", "WB" };

	hbas_tuner_state_init(&tuner);

	/* band selector */
	for (int b = 0; b < HBAS_BAND_COUNT; b++)
		band_chip[b] = radio_chip(parent, 12 + b * 56, 40, 52, bn[b], band_cb, b, 0);

	/* frequency, big */
	lbl_freq = ui_label(parent, FONT_HERO, COL_TEXT, "--");
	lv_obj_align(lbl_freq, LV_ALIGN_TOP_LEFT, 16, 60);
	lbl_unit = ui_label(parent, FONT_SM, COL_DIM, "MHz");

	lbl_rds_ps = ui_label(parent, FONT_RG, COL_ACCENT, "");
	lv_obj_align(lbl_rds_ps, LV_ALIGN_TOP_RIGHT, -16, 44);
	lbl_sig = ui_label(parent, FONT_XS, COL_DIM, "");
	lv_obj_align(lbl_sig, LV_ALIGN_TOP_RIGHT, -16, 74);
	lbl_rds_rt = ui_label(parent, FONT_XS, COL_DIM, "");
	lv_obj_set_width(lbl_rds_rt, 376);
	lv_label_set_long_mode(lbl_rds_rt, LV_LABEL_LONG_DOT);
	lv_obj_align(lbl_rds_rt, LV_ALIGN_TOP_LEFT, 12, 118);

	/* tune / seek */
	radio_chip(parent, 12, 140, 60, LV_SYMBOL_PREV, seek_cb, -1, 0);
	radio_chip(parent, 76, 140, 52, LV_SYMBOL_LEFT, step_cb, -1, 0);
	radio_chip(parent, 132, 140, 52, LV_SYMBOL_RIGHT, step_cb, 1, 0);
	radio_chip(parent, 188, 140, 60, LV_SYMBOL_NEXT, seek_cb, 1, 0);

	/* presets (short tap recall, long press save) */
	for (int i = 0; i < N_PRESET; i++) {
		preset_btn[i] = radio_chip(parent, 12 + i * 63, 180, 58, "", preset_cb, i,
					   LV_EVENT_LONG_PRESSED);
		preset_lbl[i] = lv_obj_get_child(preset_btn[i], 0);
	}
	lv_obj_t *hint = ui_label(parent, FONT_TINY, COL_DIM, "hold a preset to save");
	lv_obj_align(hint, LV_ALIGN_BOTTOM_RIGHT, -12, -4);
	radio_refresh();
}

bool ui_tuner_key(enum ui_key key)
{
	switch (key) {
	case UI_KEY_UP: do_seek(true); return true;
	case UI_KEY_DOWN: do_seek(false); return true;
	case UI_KEY_ENTER: set_band((tuner.band + 1) % HBAS_BAND_COUNT); return true;
	default: return false;
	}
}

void ui_media_build(lv_obj_t *top)
{
	lv_obj_t *p;

	/* source selector (Bluetooth / Radio) across the top */
	chip_bt = source_chip(top, 214, LV_SYMBOL_BLUETOOTH " BT", SRC_BT);
	chip_radio = source_chip(top, 304, "Radio", SRC_RADIO);

	radio_view = lv_obj_create(top);
	lv_obj_remove_style_all(radio_view);
	lv_obj_set_size(radio_view, UI_WIDTH, UI_HEIGHT);
	lv_obj_remove_flag(radio_view, LV_OBJ_FLAG_SCROLLABLE);
	build_radio(radio_view);
	lv_obj_add_flag(radio_view, LV_OBJ_FLAG_HIDDEN);

	bt_view = lv_obj_create(top);
	lv_obj_remove_style_all(bt_view);
	lv_obj_set_size(bt_view, UI_WIDTH, UI_HEIGHT);
	lv_obj_remove_flag(bt_view, LV_OBJ_FLAG_SCROLLABLE);
	p = bt_view;                        /* the Bluetooth widgets live here */

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
		lv_obj_add_flag(btn[i], LV_OBJ_FLAG_CLICKABLE);
		lv_obj_add_event_cb(btn[i], btn_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);
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

/* ---- Bluetooth settings page (phone connection) ------------------------ */

#define BT_TOP 2                         /* Power, Pair */
static lv_obj_t *bt_status, *bt_note, *bt_list;
static lv_obj_t *bt_top[BT_TOP], *bt_top_lbl[BT_TOP];
static lv_obj_t *bt_dev_row[HBAS_BT_MAX_DEVICES], *bt_dev_lbl[HBAS_BT_MAX_DEVICES];
static int bt_sel;                       /* 0=power, 1=pair, 2+ = device index */

static int bt_nsel(void) { return BT_TOP + (bt.ndev < HBAS_BT_MAX_DEVICES ? bt.ndev
							    : HBAS_BT_MAX_DEVICES); }

static void bt_send_id(const char *verb, const char *id)
{
	char line[128];

	if (hbas_bt_format(line, sizeof(line), verb, "id", id, NULL) > 0)
		cmd(line);
}

static void bt_refresh(void)
{
	char buf[160];
	int nsel = bt_nsel();

	if (!bt_status)
		return;
	if (bt_sel >= nsel)
		bt_sel = nsel - 1;

	lv_label_set_text(bt_top_lbl[0], bt.powered ? "Bluetooth: On" : "Bluetooth: Off");
	lv_label_set_text(bt_top_lbl[1], bt.pairable ? "Visible - pairing..." : "Pair a phone");
	for (int i = 0; i < BT_TOP; i++) {
		bool on = i == bt_sel;

		lv_obj_set_style_border_width(bt_top[i], on ? 2 : 1, 0);
		lv_obj_set_style_border_color(bt_top[i], on ? COL_ACCENT : COL_LINE, 0);
		lv_obj_set_style_bg_color(bt_top[i], on ? COL_SURFACE_HI : COL_SURFACE, 0);
	}

	for (int i = 0; i < HBAS_BT_MAX_DEVICES; i++) {
		if (i < bt.ndev) {
			bool on = bt_sel == BT_TOP + i;

			snprintf(buf, sizeof(buf), "%s%s", bt.dev[i].name,
				 bt.dev[i].connected ? "   " LV_SYMBOL_OK " connected" : "");
			lv_label_set_text(bt_dev_lbl[i], buf);
			lv_obj_set_style_text_color(bt_dev_lbl[i],
						    bt.dev[i].connected ? COL_ACCENT : COL_TEXT, 0);
			lv_obj_set_style_border_width(bt_dev_row[i], on ? 2 : 1, 0);
			lv_obj_set_style_border_color(bt_dev_row[i], on ? COL_ACCENT : COL_LINE, 0);
			lv_obj_remove_flag(bt_dev_row[i], LV_OBJ_FLAG_HIDDEN);
		} else {
			lv_obj_add_flag(bt_dev_row[i], LV_OBJ_FLAG_HIDDEN);
		}
	}

	if (!bt.daemon)
		snprintf(buf, sizeof(buf), LV_SYMBOL_BLUETOOTH " Bluetooth service not running");
	else if (!bt.powered)
		snprintf(buf, sizeof(buf), LV_SYMBOL_BLUETOOTH " Bluetooth is off");
	else if (bt.connected)
		snprintf(buf, sizeof(buf), LV_SYMBOL_BLUETOOTH " Connected: %s",
			 bt.device[0] ? bt.device : "phone");
	else if (bt.pairable)
		snprintf(buf, sizeof(buf), LV_SYMBOL_BLUETOOTH " Visible - pair from your phone now");
	else if (bt.ndev)
		snprintf(buf, sizeof(buf), LV_SYMBOL_BLUETOOTH " Tap a phone to connect");
	else
		snprintf(buf, sizeof(buf), LV_SYMBOL_BLUETOOTH " No paired phones");
	lv_label_set_text(bt_status, buf);

	(bt.daemon && !bt.agent ? lv_obj_remove_flag : lv_obj_add_flag)(bt_note,
									LV_OBJ_FLAG_HIDDEN);
}

static void bt_activate(int sel)
{
	if (sel == 0)
		cmd(bt.powered ? "power off\n" : "power on\n");
	else if (sel == 1)
		cmd("pairable on\n");
	else {
		int i = sel - BT_TOP;

		if (i >= 0 && i < bt.ndev)
			bt_send_id(bt.dev[i].connected ? "disconnect" : "connect", bt.dev[i].id);
	}
}

static void bt_top_clicked(lv_event_t *e)
{
	bt_sel = (int)(intptr_t)lv_event_get_user_data(e);
	bt_activate(bt_sel);
	bt_refresh();
}

static void bt_dev_clicked(lv_event_t *e)
{
	int i = (int)(intptr_t)lv_event_get_user_data(e);

	if (i >= bt.ndev)
		return;
	bt_sel = BT_TOP + i;
	if (lv_event_get_code(e) == LV_EVENT_LONG_PRESSED)
		bt_send_id("forget", bt.dev[i].id);     /* hold to forget */
	else
		bt_send_id(bt.dev[i].connected ? "disconnect" : "connect", bt.dev[i].id);
	bt_refresh();
}

void ui_bt_build(lv_obj_t *p)
{
	static const char *const top[BT_TOP] = { "Bluetooth", "Pair a phone" };

	bt_status = ui_label(p, FONT_SM, COL_DIM, "");
	lv_obj_set_pos(bt_status, 12, 40);

	for (int i = 0; i < BT_TOP; i++) {
		bt_top[i] = ui_card(p, 12 + i * 194, 64, 182, 32);
		lv_obj_add_flag(bt_top[i], LV_OBJ_FLAG_CLICKABLE);
		lv_obj_add_event_cb(bt_top[i], bt_top_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);
		bt_top_lbl[i] = ui_label(bt_top[i], FONT_SM, COL_TEXT, top[i]);
		lv_obj_center(bt_top_lbl[i]);
	}

	/* scrollable list of paired phones */
	bt_list = lv_obj_create(p);
	lv_obj_remove_style_all(bt_list);
	lv_obj_set_pos(bt_list, 8, 104);
	lv_obj_set_size(bt_list, 384, 116);
	lv_obj_set_flex_flow(bt_list, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(bt_list, 6, 0);
	lv_obj_set_scroll_dir(bt_list, LV_DIR_VER);
	for (int i = 0; i < HBAS_BT_MAX_DEVICES; i++) {
		bt_dev_row[i] = ui_card(bt_list, 0, 0, 368, 32);
		lv_obj_add_flag(bt_dev_row[i], LV_OBJ_FLAG_CLICKABLE);
		lv_obj_add_event_cb(bt_dev_row[i], bt_dev_clicked, LV_EVENT_CLICKED,
				    (void *)(intptr_t)i);
		lv_obj_add_event_cb(bt_dev_row[i], bt_dev_clicked, LV_EVENT_LONG_PRESSED,
				    (void *)(intptr_t)i);
		bt_dev_lbl[i] = ui_label(bt_dev_row[i], FONT_SM, COL_TEXT, "");
		lv_obj_align(bt_dev_lbl[i], LV_ALIGN_LEFT_MID, 12, 0);
		lv_obj_add_flag(bt_dev_row[i], LV_OBJ_FLAG_HIDDEN);
	}

	bt_note = ui_label(p, FONT_TINY, COL_DIM,
			   "hold a phone to forget it");
	lv_obj_align(bt_note, LV_ALIGN_BOTTOM_LEFT, 12, -6);
	bt_refresh();
}

bool ui_bt_key(enum ui_key key)
{
	int nsel = bt_nsel();

	switch (key) {
	case UI_KEY_UP: bt_sel = (bt_sel + nsel - 1) % nsel; break;
	case UI_KEY_DOWN: bt_sel = (bt_sel + 1) % nsel; break;
	case UI_KEY_ENTER: bt_activate(bt_sel); break;
	default: return false;
	}
	bt_refresh();
	return true;
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

static void btn_clicked(lv_event_t *e)
{
	sel = (int)(intptr_t)lv_event_get_user_data(e);
	ui_media_key(UI_KEY_ENTER);
}

bool ui_media_key(enum ui_key key)
{
	if (key == UI_KEY_LEFT) {
		set_source(SRC_BT);
		return true;
	}
	if (key == UI_KEY_RIGHT) {
		set_source(SRC_RADIO);
		return true;
	}
	if (source != SRC_BT)
		return ui_tuner_key(key);
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
