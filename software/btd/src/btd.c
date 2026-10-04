// SPDX-License-Identifier: MIT
/*
 * hbas-btd: phone connection and music controls for hogtied-ui.
 *
 * Talks to BlueZ over D-Bus and gives the UI a one-line text protocol on a
 * Unix socket (libhbas btproto.h):
 *  - which phone is connected, and the adapter's powered/pairable state
 *  - the phone's media player (AVRCP via BlueZ MediaPlayer1): track
 *    metadata, play status, position; play/pause/next/previous/stop
 *  - with --agent, pairing: BlueZ asks to confirm a passkey, the UI shows it
 *    and the rider accepts or rejects; paired phones are trusted so they
 *    reconnect on their own
 * The audio itself (A2DP) is bluez-alsa's job on the unit, PipeWire's on a PC.
 *
 * Options:
 *   --socket PATH  default /run/hbas/bt.sock
 *   --session      use the session bus (tests with a fake BlueZ)
 *   --agent        be the pairing agent (on the unit; a PC has its own)
 *   --on-paired CMD  run CMD a few seconds after a phone pairs (the unit
 *                  saves BlueZ's pairing keys to the eMMC with it)
 *   -v             log D-Bus traffic decisions to stderr
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <dbus/dbus.h>

#include "hbas/btproto.h"

#define BLUEZ          "org.bluez"
#define IF_ADAPTER     "org.bluez.Adapter1"
#define IF_DEVICE      "org.bluez.Device1"
#define IF_PLAYER      "org.bluez.MediaPlayer1"
#define IF_CONTROL     "org.bluez.MediaControl1"
#define IF_AGENT       "org.bluez.Agent1"
#define IF_AGENT_MGR   "org.bluez.AgentManager1"
#define IF_PROPS       "org.freedesktop.DBus.Properties"
#define IF_OBJMGR      "org.freedesktop.DBus.ObjectManager"
#define AGENT_PATH     "/org/hogtied/agent"
#define MAX_OBJS       48
#define MAX_CLIENTS    4
#define PAIRABLE_SECS  180

static int verbose;
static bool agent_enabled;              /* --agent: we confirm pairing */
static volatile sig_atomic_t stop;

static void logmsg(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	fputs("hbas-btd: ", stderr);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
}

/* ---- BlueZ object model ------------------------------------------------- */

struct obj {
	char path[128];
	bool adapter, device, player;
	/* adapter */
	bool powered, pairable, discoverable;
	/* device */
	bool connected, paired;
	bool control;                    /* MediaControl1 connected (AVRCP up) */
	char name[HBAS_BT_TEXT_MAX];
	/* player */
	char status[24];
	uint32_t position;
	char title[HBAS_BT_TEXT_MAX], artist[HBAS_BT_TEXT_MAX], album[HBAS_BT_TEXT_MAX];
	uint32_t duration;
};

static struct obj objs[MAX_OBJS];
static DBusConnection *bus;

static struct obj *find(const char *path, bool create)
{
	struct obj *free_slot = NULL;

	for (int i = 0; i < MAX_OBJS; i++) {
		if (objs[i].path[0] && !strcmp(objs[i].path, path))
			return &objs[i];
		if (!objs[i].path[0] && !free_slot)
			free_slot = &objs[i];
	}
	if (!create || !free_slot)
		return NULL;
	memset(free_slot, 0, sizeof(*free_slot));
	snprintf(free_slot->path, sizeof(free_slot->path), "%s", path);
	return free_slot;
}

static bool under(const char *child, const char *parent)
{
	size_t n = strlen(parent);

	return !strncmp(child, parent, n) && child[n] == '/';
}

static struct obj *first(bool (*pred)(const struct obj *))
{
	for (int i = 0; i < MAX_OBJS; i++)
		if (objs[i].path[0] && pred(&objs[i]))
			return &objs[i];
	return NULL;
}

static bool known_iface(const char *iface)
{
	return !strcmp(iface, IF_ADAPTER) || !strcmp(iface, IF_DEVICE) ||
	       !strcmp(iface, IF_PLAYER) || !strcmp(iface, IF_CONTROL);
}

static bool is_adapter(const struct obj *o) { return o->adapter; }
static bool is_connected(const struct obj *o) { return o->device && o->connected; }

static struct obj *current_device(void) { return first(is_connected); }

static struct obj *current_player(void)
{
	struct obj *d = current_device();

	for (int i = 0; d && i < MAX_OBJS; i++)
		if (objs[i].path[0] && objs[i].player && under(objs[i].path, d->path))
			return &objs[i];
	return NULL;
}

/* ---- variant helpers ------------------------------------------------------ */

static void get_string(DBusMessageIter *v, char *dst, size_t cap)
{
	const char *s = "";

	if (dbus_message_iter_get_arg_type(v) == DBUS_TYPE_STRING ||
	    dbus_message_iter_get_arg_type(v) == DBUS_TYPE_OBJECT_PATH)
		dbus_message_iter_get_basic(v, &s);
	snprintf(dst, cap, "%s", s);
}

static bool get_bool(DBusMessageIter *v)
{
	dbus_bool_t b = FALSE;

	if (dbus_message_iter_get_arg_type(v) == DBUS_TYPE_BOOLEAN)
		dbus_message_iter_get_basic(v, &b);
	return b;
}

static uint32_t get_u32(DBusMessageIter *v)
{
	dbus_uint32_t u = 0;

	if (dbus_message_iter_get_arg_type(v) == DBUS_TYPE_UINT32)
		dbus_message_iter_get_basic(v, &u);
	return u;
}

/* Track is a{sv}: Title, Artist, Album (s), Duration (u, ms) */
static void parse_track(struct obj *o, DBusMessageIter *v)
{
	DBusMessageIter arr, ent, val;

	o->title[0] = o->artist[0] = o->album[0] = '\0';
	o->duration = 0;
	if (dbus_message_iter_get_arg_type(v) != DBUS_TYPE_ARRAY)
		return;
	for (dbus_message_iter_recurse(v, &arr);
	     dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_DICT_ENTRY;
	     dbus_message_iter_next(&arr)) {
		const char *k;

		dbus_message_iter_recurse(&arr, &ent);
		dbus_message_iter_get_basic(&ent, &k);
		dbus_message_iter_next(&ent);
		dbus_message_iter_recurse(&ent, &val);
		if (!strcmp(k, "Title"))
			get_string(&val, o->title, sizeof(o->title));
		else if (!strcmp(k, "Artist"))
			get_string(&val, o->artist, sizeof(o->artist));
		else if (!strcmp(k, "Album"))
			get_string(&val, o->album, sizeof(o->album));
		else if (!strcmp(k, "Duration"))
			o->duration = get_u32(&val);
	}
}

/* Apply a{sv} properties of one interface to an object. */
static void apply_props(struct obj *o, const char *iface, DBusMessageIter *dict)
{
	DBusMessageIter ent, val;

	if (!strcmp(iface, IF_ADAPTER))
		o->adapter = true;
	else if (!strcmp(iface, IF_DEVICE))
		o->device = true;
	else if (!strcmp(iface, IF_PLAYER))
		o->player = true;
	else if (strcmp(iface, IF_CONTROL))
		return;
	for (; dbus_message_iter_get_arg_type(dict) == DBUS_TYPE_DICT_ENTRY;
	     dbus_message_iter_next(dict)) {
		const char *k;

		dbus_message_iter_recurse(dict, &ent);
		dbus_message_iter_get_basic(&ent, &k);
		dbus_message_iter_next(&ent);
		dbus_message_iter_recurse(&ent, &val);
		if (!strcmp(iface, IF_ADAPTER)) {
			if (!strcmp(k, "Powered")) o->powered = get_bool(&val);
			else if (!strcmp(k, "Pairable")) o->pairable = get_bool(&val);
			else if (!strcmp(k, "Discoverable")) o->discoverable = get_bool(&val);
		} else if (!strcmp(iface, IF_CONTROL)) {
			if (!strcmp(k, "Connected")) o->control = get_bool(&val);
		} else if (!strcmp(iface, IF_DEVICE)) {
			if (!strcmp(k, "Connected")) o->connected = get_bool(&val);
			else if (!strcmp(k, "Paired")) o->paired = get_bool(&val);
			else if (!strcmp(k, "Alias")) get_string(&val, o->name, sizeof(o->name));
		} else {
			if (!strcmp(k, "Status")) get_string(&val, o->status, sizeof(o->status));
			else if (!strcmp(k, "Position")) o->position = get_u32(&val);
			else if (!strcmp(k, "Track")) parse_track(o, &val);
		}
	}
}

/* a{sa{sv}} interfaces-and-properties for one object */
static void apply_ifaces(const char *path, DBusMessageIter *arr)
{
	DBusMessageIter ent, props;

	for (; dbus_message_iter_get_arg_type(arr) == DBUS_TYPE_DICT_ENTRY;
	     dbus_message_iter_next(arr)) {
		const char *iface;
		struct obj *o;

		dbus_message_iter_recurse(arr, &ent);
		dbus_message_iter_get_basic(&ent, &iface);
		if (!known_iface(iface))
			continue;
		if (!(o = find(path, true)))
			return;
		dbus_message_iter_next(&ent);
		dbus_message_iter_recurse(&ent, &props);
		apply_props(o, iface, &props);
	}
}

/* ---- UI clients --------------------------------------------------------- */

static int clients[MAX_CLIENTS] = { -1, -1, -1, -1 };
static char inbuf[MAX_CLIENTS][HBAS_BT_LINE_MAX];
static size_t inlen[MAX_CLIENTS];

static void send_line(int fd, const char *line)
{
	size_t n = strlen(line);

	/* one short line; a client that can't keep up is dropped */
	if (fd >= 0 && send(fd, line, n, MSG_NOSIGNAL | MSG_DONTWAIT) != (ssize_t)n) {
		for (int i = 0; i < MAX_CLIENTS; i++)
			if (clients[i] == fd) {
				close(fd);
				clients[i] = -1;
			}
	}
}

static void broadcast(const char *line)
{
	if (verbose)
		fprintf(stderr, "-> %s", line);
	for (int i = 0; i < MAX_CLIENTS; i++)
		if (clients[i] >= 0)
			send_line(clients[i], line);
}

/* last lines sent, so only real changes go out */
static char last_bt[HBAS_BT_LINE_MAX], last_track[HBAS_BT_LINE_MAX], last_play[HBAS_BT_LINE_MAX];
static char last_devs[HBAS_BT_LINE_MAX * HBAS_BT_MAX_DEVICES];

static void build_state(char *bt, char *track, char *play)
{
	struct obj *a = first(is_adapter), *d = current_device(), *p = current_player();
	char dur[16], pos[16];

	hbas_bt_format(bt, HBAS_BT_LINE_MAX, "bt",
		       "powered", a && a->powered ? "1" : "0",
		       "pairable", a && a->discoverable && a->pairable ? "1" : "0",
		       "agent", agent_enabled ? "1" : "0",
		       "connected", d ? "1" : "0",
		       "name", d ? d->name : "",
		       "player", p || (d && d->control) ? "1" : "0", NULL);
	snprintf(dur, sizeof(dur), "%u", p ? p->duration : 0);
	hbas_bt_format(track, HBAS_BT_LINE_MAX, "track",
		       "title", p ? p->title : "", "artist", p ? p->artist : "",
		       "album", p ? p->album : "", "duration", dur, NULL);
	snprintf(pos, sizeof(pos), "%u", p ? p->position : 0);
	hbas_bt_format(play, HBAS_BT_LINE_MAX, "play",
		       "status", p && p->status[0] ? p->status : "stopped", "position", pos, NULL);
}

/* The paired phones, as "devices n=K\n" then one "device ..." line each,
 * concatenated into out. */
static void build_devices(char *out, size_t cap)
{
	int k = 0;
	size_t off;

	for (int i = 0; i < MAX_OBJS; i++)
		if (objs[i].path[0] && objs[i].device && (objs[i].paired || objs[i].connected))
			k++;
	off = (size_t)snprintf(out, cap, "devices n=%d\n", k);
	for (int i = 0; i < MAX_OBJS && off < cap; i++) {
		struct obj *o = &objs[i];

		if (!o->path[0] || !o->device || !(o->paired || o->connected))
			continue;
		off += hbas_bt_format(out + off, cap - off, "device",
				      "id", o->path,
				      "name", o->name[0] ? o->name : "phone",
				      "paired", o->paired ? "1" : "0",
				      "connected", o->connected ? "1" : "0", NULL);
	}
}

static void publish(void)
{
	char devs[sizeof(last_devs)];

	char bt[HBAS_BT_LINE_MAX], track[HBAS_BT_LINE_MAX], play[HBAS_BT_LINE_MAX];

	build_state(bt, track, play);
	if (strcmp(bt, last_bt)) {
		strcpy(last_bt, bt);
		broadcast(bt);
	}
	if (strcmp(track, last_track)) {
		strcpy(last_track, track);
		broadcast(track);
	}
	if (strcmp(play, last_play)) {
		strcpy(last_play, play);
		broadcast(play);
	}
	build_devices(devs, sizeof(devs));
	if (strcmp(devs, last_devs)) {
		strcpy(last_devs, devs);
		broadcast(devs);
	}
}

static void send_snapshot(int fd)
{
	char bt[HBAS_BT_LINE_MAX], track[HBAS_BT_LINE_MAX], play[HBAS_BT_LINE_MAX];

	build_state(bt, track, play);
	char devs[sizeof(last_devs)];

	send_line(fd, bt);
	send_line(fd, track);
	send_line(fd, play);
	build_devices(devs, sizeof(devs));
	send_line(fd, devs);
}

/* ---- D-Bus calls ---------------------------------------------------------- */

static void call_noreply(const char *path, const char *iface, const char *method)
{
	DBusMessage *m = dbus_message_new_method_call(BLUEZ, path, iface, method);

	if (!m)
		return;
	dbus_message_set_no_reply(m, TRUE);
	dbus_connection_send(bus, m, NULL);
	dbus_message_unref(m);
}

static void set_prop(const char *path, const char *iface, const char *name, int type,
		     const void *value)
{
	DBusMessage *m = dbus_message_new_method_call(BLUEZ, path, IF_PROPS, "Set");
	DBusMessageIter it, var;
	char sig[2] = { (char)type, 0 };

	if (!m)
		return;
	dbus_message_iter_init_append(m, &it);
	dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &iface);
	dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &name);
	dbus_message_iter_open_container(&it, DBUS_TYPE_VARIANT, sig, &var);
	dbus_message_iter_append_basic(&var, type, value);
	dbus_message_iter_close_container(&it, &var);
	dbus_message_set_no_reply(m, TRUE);
	dbus_connection_send(bus, m, NULL);
	dbus_message_unref(m);
}

static void set_pairable(bool on)
{
	struct obj *a = first(is_adapter);
	dbus_bool_t b = on;
	dbus_uint32_t t = PAIRABLE_SECS;

	if (!a) {
		logmsg("no Bluetooth adapter");
		return;
	}
	if (on) {
		dbus_bool_t yes = TRUE;

		set_prop(a->path, IF_ADAPTER, "Powered", DBUS_TYPE_BOOLEAN, &yes);
		set_prop(a->path, IF_ADAPTER, "DiscoverableTimeout", DBUS_TYPE_UINT32, &t);
		set_prop(a->path, IF_ADAPTER, "PairableTimeout", DBUS_TYPE_UINT32, &t);
	}
	set_prop(a->path, IF_ADAPTER, "Pairable", DBUS_TYPE_BOOLEAN, &b);
	set_prop(a->path, IF_ADAPTER, "Discoverable", DBUS_TYPE_BOOLEAN, &b);
}

static void set_powered(bool on)
{
	struct obj *a = first(is_adapter);
	dbus_bool_t b = on;

	if (a)
		set_prop(a->path, IF_ADAPTER, "Powered", DBUS_TYPE_BOOLEAN, &b);
}

/* Forget a paired phone: RemoveDevice(objpath) on the adapter. */
static void remove_device(const char *dev_path)
{
	struct obj *a = first(is_adapter);
	DBusMessage *m;

	if (!a || !dev_path)
		return;
	m = dbus_message_new_method_call(BLUEZ, a->path, IF_ADAPTER, "RemoveDevice");
	if (!m)
		return;
	dbus_message_append_args(m, DBUS_TYPE_OBJECT_PATH, &dev_path, DBUS_TYPE_INVALID);
	dbus_message_set_no_reply(m, TRUE);
	dbus_connection_send(bus, m, NULL);
	dbus_message_unref(m);
}

/* Full resync: GetManagedObjects (at start and when bluetoothd restarts). */
static void resync(void)
{
	DBusMessage *m = dbus_message_new_method_call(BLUEZ, "/", IF_OBJMGR, "GetManagedObjects");
	DBusMessage *r;
	DBusMessageIter it, arr, ent, ifaces;
	DBusError err;

	memset(objs, 0, sizeof(objs));
	dbus_error_init(&err);
	r = dbus_connection_send_with_reply_and_block(bus, m, 3000, &err);
	dbus_message_unref(m);
	if (!r) {
		logmsg("BlueZ not available yet (%s)", err.message ? err.message : "?");
		dbus_error_free(&err);
		publish();
		return;
	}
	if (dbus_message_iter_init(r, &it) &&
	    dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_ARRAY) {
		for (dbus_message_iter_recurse(&it, &arr);
		     dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_DICT_ENTRY;
		     dbus_message_iter_next(&arr)) {
			const char *path;

			dbus_message_iter_recurse(&arr, &ent);
			dbus_message_iter_get_basic(&ent, &path);
			dbus_message_iter_next(&ent);
			dbus_message_iter_recurse(&ent, &ifaces);
			apply_ifaces(path, &ifaces);
		}
	}
	dbus_message_unref(r);
	publish();
}

/* ---- pairing agent ---------------------------------------------------- */

static DBusMessage *pending;            /* RequestConfirmation awaiting the rider */
static char pending_device[128];

static void reply_error(DBusMessage *msg, const char *name, const char *text)
{
	DBusMessage *r = dbus_message_new_error(msg, name, text);

	if (r) {
		dbus_connection_send(bus, r, NULL);
		dbus_message_unref(r);
	}
}

static void reply_ok(DBusMessage *msg)
{
	DBusMessage *r = dbus_message_new_method_return(msg);

	if (r) {
		dbus_connection_send(bus, r, NULL);
		dbus_message_unref(r);
	}
}

static const char *on_paired;
static time_t save_due;                     /* run on_paired at this time, 0 = no */

static void pair_end(const char *result)
{
	char line[HBAS_BT_LINE_MAX];

	hbas_bt_format(line, sizeof(line), "pair-end", "result", result, NULL);
	broadcast(line);
	/* bluetoothd writes the keys just after Paired goes true: wait a bit */
	if (on_paired && !strcmp(result, "ok"))
		save_due = time(NULL) + 3;
}

static void run_on_paired(void)
{
	pid_t pid;

	save_due = 0;
	if ((pid = fork()) == 0) {
		execl("/bin/sh", "sh", "-c", on_paired, (char *)NULL);
		_exit(127);
	}
	if (pid < 0)
		logmsg("can't run %s: %s", on_paired, strerror(errno));
}

static void answer_pending(bool yes)
{
	if (!pending)
		return;
	if (yes) {
		dbus_bool_t t = TRUE;

		reply_ok(pending);
		/* trusted: it may reconnect and use A2DP/AVRCP without asking again */
		set_prop(pending_device, IF_DEVICE, "Trusted", DBUS_TYPE_BOOLEAN, &t);
	} else {
		reply_error(pending, "org.bluez.Error.Rejected", "rejected by the rider");
		pair_end("rejected");
	}
	dbus_message_unref(pending);
	pending = NULL;
}

static const char *device_name(const char *path)
{
	struct obj *o = find(path, false);

	return o && o->name[0] ? o->name : path;
}

static DBusHandlerResult agent_handler(DBusConnection *c, DBusMessage *msg, void *data)
{
	const char *member = dbus_message_get_member(msg);
	const char *dev = NULL;
	dbus_uint32_t passkey = 0;
	struct obj *a = first(is_adapter);
	char line[HBAS_BT_LINE_MAX], pk[16];

	(void)c;
	(void)data;
	if (!dbus_message_is_method_call(msg, IF_AGENT, member))
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	if (verbose)
		logmsg("agent: %s", member);
	if (!strcmp(member, "RequestConfirmation")) {
		if (!dbus_message_get_args(msg, NULL, DBUS_TYPE_OBJECT_PATH, &dev,
					   DBUS_TYPE_UINT32, &passkey, DBUS_TYPE_INVALID)) {
			reply_error(msg, "org.bluez.Error.Rejected", "bad arguments");
			return DBUS_HANDLER_RESULT_HANDLED;
		}
		if (pending)
			answer_pending(false);          /* only one at a time */
		pending = dbus_message_ref(msg);
		snprintf(pending_device, sizeof(pending_device), "%s", dev);
		snprintf(pk, sizeof(pk), "%06u", passkey);
		hbas_bt_format(line, sizeof(line), "pair", "device", device_name(dev),
			       "passkey", pk, NULL);
		broadcast(line);
	} else if (!strcmp(member, "RequestAuthorization")) {
		/* "just works" pairing: only while the rider has made us pairable */
		if (a && a->pairable && a->discoverable)
			reply_ok(msg);
		else
			reply_error(msg, "org.bluez.Error.Rejected", "not pairable");
	} else if (!strcmp(member, "AuthorizeService")) {
		/* only bonded (paired) phones get here; allow their audio profiles */
		reply_ok(msg);
	} else if (!strcmp(member, "Cancel")) {
		if (pending) {
			dbus_message_unref(pending);
			pending = NULL;
		}
		pair_end("cancelled");
		reply_ok(msg);
	} else if (!strcmp(member, "Release")) {
		reply_ok(msg);
	} else {
		/* PIN codes and passkey entry need a keypad we don't have */
		reply_error(msg, "org.bluez.Error.Rejected", "not supported");
	}
	return DBUS_HANDLER_RESULT_HANDLED;
}

static void register_agent(void)
{
	static const DBusObjectPathVTable vt = { .message_function = agent_handler };
	const char *path = AGENT_PATH, *cap = "DisplayYesNo";
	DBusMessage *m;

	if (!agent_enabled)
		return;
	dbus_connection_register_object_path(bus, AGENT_PATH, &vt, NULL);
	m = dbus_message_new_method_call(BLUEZ, "/org/bluez", IF_AGENT_MGR, "RegisterAgent");
	dbus_message_append_args(m, DBUS_TYPE_OBJECT_PATH, &path, DBUS_TYPE_STRING, &cap,
				 DBUS_TYPE_INVALID);
	dbus_message_set_no_reply(m, TRUE);
	dbus_connection_send(bus, m, NULL);
	dbus_message_unref(m);
	m = dbus_message_new_method_call(BLUEZ, "/org/bluez", IF_AGENT_MGR, "RequestDefaultAgent");
	dbus_message_append_args(m, DBUS_TYPE_OBJECT_PATH, &path, DBUS_TYPE_INVALID);
	dbus_message_set_no_reply(m, TRUE);
	dbus_connection_send(bus, m, NULL);
	dbus_message_unref(m);
}

/* ---- signals ------------------------------------------------------------ */

static DBusHandlerResult filter(DBusConnection *c, DBusMessage *msg, void *data)
{
	DBusMessageIter it, sub;
	const char *path = dbus_message_get_path(msg);

	(void)c;
	(void)data;
	if (dbus_message_is_signal(msg, IF_PROPS, "PropertiesChanged") && path) {
		const char *iface;
		struct obj *o;
		bool was_paired;

		if (!dbus_message_iter_init(msg, &it) ||
		    dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_STRING)
			return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
		dbus_message_iter_get_basic(&it, &iface);
		if (!known_iface(iface))
			return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
		if (!(o = find(path, true)))
			return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
		was_paired = o->paired;
		dbus_message_iter_next(&it);
		dbus_message_iter_recurse(&it, &sub);
		apply_props(o, iface, &sub);
		if (o->device && o->paired && !was_paired)
			pair_end("ok");
		publish();
	} else if (dbus_message_is_signal(msg, IF_OBJMGR, "InterfacesAdded")) {
		const char *p;

		if (dbus_message_iter_init(msg, &it) &&
		    dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_OBJECT_PATH) {
			dbus_message_iter_get_basic(&it, &p);
			dbus_message_iter_next(&it);
			dbus_message_iter_recurse(&it, &sub);
			apply_ifaces(p, &sub);
			publish();
		}
	} else if (dbus_message_is_signal(msg, IF_OBJMGR, "InterfacesRemoved")) {
		const char *p;
		struct obj *o;

		if (dbus_message_iter_init(msg, &it) &&
		    dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_OBJECT_PATH) {
			dbus_message_iter_get_basic(&it, &p);
			dbus_message_iter_next(&it);
			for (dbus_message_iter_recurse(&it, &sub);
			     dbus_message_iter_get_arg_type(&sub) == DBUS_TYPE_STRING;
			     dbus_message_iter_next(&sub)) {
				const char *iface;

				dbus_message_iter_get_basic(&sub, &iface);
				if (!(o = find(p, false)))
					break;
				if (!strcmp(iface, IF_PLAYER)) o->player = false;
				if (!strcmp(iface, IF_CONTROL)) o->control = false;
				if (!strcmp(iface, IF_DEVICE)) o->device = o->connected = false;
				if (!strcmp(iface, IF_ADAPTER)) o->adapter = false;
				if (!o->player && !o->device && !o->adapter)
					o->path[0] = '\0';
			}
			publish();
		}
	} else if (dbus_message_is_signal(msg, DBUS_INTERFACE_DBUS, "NameOwnerChanged")) {
		const char *name, *old, *new;

		if (dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &name, DBUS_TYPE_STRING, &old,
					  DBUS_TYPE_STRING, &new, DBUS_TYPE_INVALID) &&
		    !strcmp(name, BLUEZ)) {
			logmsg("bluetoothd %s", new[0] ? "(re)started" : "went away");
			if (pending) {
				dbus_message_unref(pending);
				pending = NULL;
			}
			resync();
			if (new[0])
				register_agent();
		}
	}
	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

/* ---- UI commands ---------------------------------------------------------- */

static void handle_command(const char *line)
{
	struct hbas_bt_msg m;
	struct obj *p = current_player(), *d = current_device();
	const char *arg;
	static const struct { const char *cmd, *method; } media[] = {
		{ "play", "Play" }, { "pause", "Pause" }, { "next", "Next" },
		{ "previous", "Previous" }, { "stop", "Stop" },
	};

	if (hbas_bt_parse(line, &m))
		return;
	arg = m.nargs ? m.arg[0].val : "";
	if (verbose)
		logmsg("<- %s %s", m.verb, arg);
	for (unsigned i = 0; i < sizeof(media) / sizeof(media[0]); i++) {
		if (!strcmp(m.verb, media[i].cmd)) {
			/* the phone's player if it published one, else the basic
			 * AVRCP remote control BlueZ offers once AVRCP is up */
			if (p)
				call_noreply(p->path, IF_PLAYER, media[i].method);
			else if (d && d->control)
				call_noreply(d->path, IF_CONTROL, media[i].method);
			else
				logmsg("%s: phone has no media controls", m.verb);
			return;
		}
	}
	if (!strcmp(m.verb, "pairable")) {
		/* only as the pairing agent (the unit): on a PC the desktop pairs,
		 * and we must not change the PC's own adapter settings */
		if (agent_enabled)
			set_pairable(!strcmp(arg, "on"));
		else
			logmsg("pairable: ignored, not the pairing agent (pair from the PC's settings)");
	}
	else if (!strcmp(m.verb, "power")) {
		if (agent_enabled)
			set_powered(!strcmp(arg, "on"));
		else
			logmsg("power: ignored, not the pairing agent (use the PC's settings)");
	}
	else if (!strcmp(m.verb, "confirm"))
		answer_pending(!strcmp(arg, "yes"));
	else if (!strcmp(m.verb, "connect")) {
		const char *id = hbas_bt_get(&m, "id");

		if (id && find(id, false))
			call_noreply(id, IF_DEVICE, "Connect");
	}
	else if (!strcmp(m.verb, "disconnect")) {
		const char *id = hbas_bt_get(&m, "id");

		if (id && find(id, false))
			call_noreply(id, IF_DEVICE, "Disconnect");
		else if (d)
			call_noreply(d->path, IF_DEVICE, "Disconnect");
	}
	else if (!strcmp(m.verb, "forget")) {
		const char *id = hbas_bt_get(&m, "id");

		if (!agent_enabled)
			logmsg("forget: ignored, not the pairing agent");
		else if (id && find(id, false))
			remove_device(id);
		else if (d)
			remove_device(d->path);
	}
}

static int listen_socket(const char *path)
{
	struct sockaddr_un a = { .sun_family = AF_UNIX };
	int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	char dir[sizeof(a.sun_path)], *slash;

	if (s < 0 || strlen(path) >= sizeof(a.sun_path))
		return -1;
	snprintf(dir, sizeof(dir), "%s", path);
	if ((slash = strrchr(dir, '/')) && slash != dir) {
		*slash = '\0';
		mkdir(dir, 0755);
	}
	strcpy(a.sun_path, path);
	unlink(path);
	if (bind(s, (struct sockaddr *)&a, sizeof(a)) || listen(s, 4)) {
		close(s);
		return -1;
	}
	return s;
}

static void client_input(int i)
{
	char *nl;
	ssize_t r = recv(clients[i], inbuf[i] + inlen[i], sizeof(inbuf[i]) - 1 - inlen[i], 0);

	if (r <= 0) {
		close(clients[i]);
		clients[i] = -1;
		inlen[i] = 0;
		return;
	}
	inlen[i] += (size_t)r;
	inbuf[i][inlen[i]] = '\0';
	while ((nl = strchr(inbuf[i], '\n'))) {
		*nl = '\0';
		handle_command(inbuf[i]);
		inlen[i] -= (size_t)(nl + 1 - inbuf[i]);
		memmove(inbuf[i], nl + 1, inlen[i] + 1);
	}
	if (inlen[i] >= sizeof(inbuf[i]) - 1)
		inlen[i] = 0;                       /* overlong line: drop it */
}

static void on_sig(int s) { (void)s; stop = 1; }

int main(int argc, char **argv)
{
	const char *sock_path = "/run/hbas/bt.sock";
	bool session = false;
	DBusError err;
	int ls, dfd;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--socket") && i + 1 < argc)
			sock_path = argv[++i];
		else if (!strcmp(argv[i], "--session"))
			session = true;
		else if (!strcmp(argv[i], "--agent"))
			agent_enabled = true;
		else if (!strcmp(argv[i], "--on-paired") && i + 1 < argc)
			on_paired = argv[++i];
		else if (!strcmp(argv[i], "-v"))
			verbose++;
		else {
			fprintf(stderr, "usage: %s [--socket PATH] [--session] [--agent] "
				"[--on-paired CMD] [-v]\n", argv[0]);
			return 2;
		}
	}
	signal(SIGINT, on_sig);
	signal(SIGTERM, on_sig);
	signal(SIGPIPE, SIG_IGN);
	signal(SIGCHLD, SIG_IGN);               /* on-paired children reap themselves */

	dbus_error_init(&err);
	bus = dbus_bus_get(session ? DBUS_BUS_SESSION : DBUS_BUS_SYSTEM, &err);
	if (!bus) {
		logmsg("no D-Bus: %s", err.message);
		return 1;
	}
	dbus_connection_set_exit_on_disconnect(bus, FALSE);
	dbus_bus_add_match(bus, "type='signal',sender='" BLUEZ "',interface='" IF_PROPS
			   "',member='PropertiesChanged'", NULL);
	dbus_bus_add_match(bus, "type='signal',sender='" BLUEZ "',interface='" IF_OBJMGR "'", NULL);
	dbus_bus_add_match(bus, "type='signal',interface='" DBUS_INTERFACE_DBUS
			   "',member='NameOwnerChanged',arg0='" BLUEZ "'", NULL);
	dbus_connection_add_filter(bus, filter, NULL, NULL);
	if ((ls = listen_socket(sock_path)) < 0) {
		logmsg("%s: %s", sock_path, strerror(errno));
		return 1;
	}
	if (!dbus_connection_get_unix_fd(bus, &dfd)) {
		logmsg("can't get the D-Bus fd");
		return 1;
	}
	resync();
	register_agent();
	logmsg("ready on %s%s", sock_path, agent_enabled ? " (pairing agent)" : "");

	while (!stop && dbus_connection_get_is_connected(bus)) {
		struct pollfd p[2 + MAX_CLIENTS];
		int n = 0;

		p[n++] = (struct pollfd){ .fd = dfd, .events = POLLIN };
		p[n++] = (struct pollfd){ .fd = ls, .events = POLLIN };
		for (int i = 0; i < MAX_CLIENTS; i++)
			p[n++] = (struct pollfd){ .fd = clients[i], .events = POLLIN };
		if (poll(p, (nfds_t)n, 1000) < 0 && errno != EINTR)
			break;
		dbus_connection_read_write(bus, 0);
		while (dbus_connection_dispatch(bus) == DBUS_DISPATCH_DATA_REMAINS)
			;
		if (p[1].revents & POLLIN) {
			int c = accept4(ls, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);

			for (int i = 0; c >= 0 && i < MAX_CLIENTS; i++) {
				if (clients[i] < 0) {
					clients[i] = c;
					inlen[i] = 0;
					send_snapshot(c);
					c = -1;
				}
			}
			if (c >= 0)
				close(c);                   /* full */
		}
		for (int i = 0; i < MAX_CLIENTS; i++)
			if (clients[i] >= 0 && (p[2 + i].revents & (POLLIN | POLLHUP | POLLERR)))
				client_input(i);
		dbus_connection_flush(bus);
		if (save_due && time(NULL) >= save_due)
			run_on_paired();
	}
	if (pending)
		answer_pending(false);
	unlink(sock_path);
	return 0;
}
