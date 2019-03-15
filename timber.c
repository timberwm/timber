/*
 * Copyright (C) Patrick Steinhardt, 2019
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define inline
#include <xcb/xcb.h>
#include <xcb/xcb_aux.h>
#include <xcb/xcb_event.h>
#include <xcb/xcb_ewmh.h>
#include <xcb/randr.h>
#undef inline

#define TMBR_UNUSED(x) (void)(x)

#define TMBR_ARG_NONE      { 0, 0 }
#define TMBR_ARG_SEL(i)    { i, 0 }
#define TMBR_ARG_DIR(d, i) { i, d }

typedef struct tmbr_client tmbr_client_t;
typedef struct tmbr_command_args tmbr_command_args_t;
typedef struct tmbr_command tmbr_command_t;
typedef struct tmbr_desktop tmbr_desktop_t;
typedef struct tmbr_screen tmbr_screen_t;
typedef struct tmbr_tree tmbr_tree_t;

typedef enum {
	TMBR_DIR_NORTH,
	TMBR_DIR_SOUTH,
	TMBR_DIR_EAST,
	TMBR_DIR_WEST
} tmbr_dir_t;

typedef enum {
	TMBR_SELECT_PREV,
	TMBR_SELECT_NEXT,
	TMBR_SELECT_NEAREST
} tmbr_select_t;

typedef enum {
	TMBR_SPLIT_VERTICAL,
	TMBR_SPLIT_HORIZONTAL
} tmbr_split_t;

struct tmbr_command_args {
	int i;
	tmbr_dir_t dir;
};

struct tmbr_client {
	tmbr_desktop_t *desktop;
	tmbr_tree_t *tree;
	xcb_window_t window;
};

struct tmbr_command {
	const char *cmd;
	void (*fn)(const tmbr_command_args_t *);
	tmbr_command_args_t args;
};

struct tmbr_desktop {
	tmbr_desktop_t *prev;
	tmbr_desktop_t *next;
	tmbr_screen_t *screen;
	tmbr_tree_t *clients;
	tmbr_client_t *focus;
};

struct tmbr_screen {
	tmbr_screen_t *next;
	tmbr_desktop_t *desktops;
	tmbr_desktop_t *focus;
	xcb_window_t root;
	uint16_t x;
	uint16_t y;
	uint16_t width;
	uint16_t height;
	char focussed;
};

struct tmbr_tree {
	tmbr_tree_t *parent;
	tmbr_tree_t *left;
	tmbr_tree_t *right;
	tmbr_client_t *client;
	tmbr_split_t split;
	uint8_t ratio;
};

static void tmbr_cmd_client_kill(const tmbr_command_args_t *args);
static void tmbr_cmd_client_focus(const tmbr_command_args_t *args);
static void tmbr_cmd_client_move(const tmbr_command_args_t *args);
static void tmbr_cmd_client_resize(const tmbr_command_args_t *args);
static void tmbr_cmd_client_swap(const tmbr_command_args_t *args);
static void tmbr_cmd_desktop_new(const tmbr_command_args_t *args);
static void tmbr_cmd_desktop_kill(const tmbr_command_args_t *args);
static void tmbr_cmd_desktop_focus(const tmbr_command_args_t *args);
static void tmbr_cmd_tree_rotate(const tmbr_command_args_t *args);

#include "config.h"

static void tmbr_discard_events(uint8_t type);

static tmbr_screen_t *screens;
static xcb_connection_t *conn;
static xcb_ewmh_connection_t ewmh;
static int fifofd = -1;

static void die(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);

	exit(-1);
}

static int tmbr_tree_new(tmbr_tree_t **out, tmbr_client_t *client)
{
	tmbr_tree_t *t;

	if ((t = calloc(1, sizeof(*t))) == NULL)
		return -1;
	t->ratio = 50;
	t->split = TMBR_SPLIT_VERTICAL;
	t->client = client;
	if (client)
		client->tree = t;

	*out = t;
	return 0;
}

static int tmbr_tree_insert(tmbr_tree_t **tree, tmbr_client_t *client)
{
	tmbr_tree_t *l, *r, *t = *tree;

	if (!t)
		return tmbr_tree_new(tree, client);

	if (tmbr_tree_new(&l, t->client) < 0 || tmbr_tree_new(&r, client) < 0)
		die("Unable to allocate right tree");

	l->left = t->left;
	l->right = t->right;
	l->parent = t;
	r->parent = t;

	t->client = NULL;
	t->left = l;
	t->right = r;

	return 0;
}

static tmbr_tree_t *tmbr_tree_get_child(tmbr_tree_t *tree, tmbr_select_t which)
{
	return (which == TMBR_SELECT_PREV) ? tree->left : tree->right;
}

static int tmbr_tree_find_sibling(tmbr_tree_t **node, tmbr_tree_t *tree, tmbr_select_t which)
{
	tmbr_select_t upwards, downwards;
	tmbr_tree_t *t = tree;

	if (which == TMBR_SELECT_NEAREST)
		which = (t && t->parent && t->parent->left == t) ? TMBR_SELECT_NEXT : TMBR_SELECT_PREV;

	upwards = which;
	downwards = !which;

	while (t) {
		if (!t->parent) {
			/* We want to wrap to the leftmost node */
			break;
		} else if (t != (tmbr_tree_get_child(t->parent, upwards))) {
			/* Go to the leftmost node of the right parent node */
			t = tmbr_tree_get_child(t->parent, upwards);
			break;
		}
		t = t->parent;
	}

	if (!t)
		return -1;

	while (tmbr_tree_get_child(t, downwards))
		t = tmbr_tree_get_child(t, downwards);

	if (t == tree)
		return -1;

	*node = t;

	return 0;
}

#define tmbr_tree_foreach_leaf(t, i, n) \
	i = NULL, n = t; \
	while ((!i && n && n->client && (i = n)) || (tmbr_tree_find_sibling(&n, n, TMBR_SELECT_NEXT) == 0 && ((!i && (i = n)) || i != n)))

static int tmbr_tree_swap(tmbr_tree_t *a, tmbr_tree_t *b)
{
	tmbr_tree_t *l = a->left, *r = a->right;
	tmbr_client_t *c = a->client;

	if ((a->client = b->client) != NULL)
		a->client->tree = a;
	if ((a->left = b->left) != NULL)
		a->left->parent = a;
	if ((a->right = b->right) != NULL)
		a->right->parent = a;

	if ((b->client = c) != NULL)
		b->client->tree = b;
	if ((b->left = l) != NULL)
		b->left->parent = b;
	if ((b->right = r) != NULL)
		b->right->parent = b;

	return 0;
}

static int tmbr_tree_remove(tmbr_tree_t **tree, tmbr_tree_t *node)
{
	tmbr_tree_t *parent = node->parent, *uplift;

	if (node == *tree) {
		free(node);
		*tree = NULL;
		return 0;
	}

	uplift = (parent->left == node) ? parent->right : parent->left;

	tmbr_tree_swap(uplift, node->parent);

	free(uplift);
	free(node);
	return 0;
}

static int tmbr_client_draw_border(tmbr_client_t *client, uint32_t color)
{
	xcb_change_window_attributes(conn, client->window, XCB_CW_BORDER_PIXEL, &color);
	return 0;
}

static int tmbr_client_show(tmbr_client_t *client)
{
	xcb_map_window(conn, client->window);
	return 0;
}

static int tmbr_client_hide(tmbr_client_t *client)
{
	xcb_unmap_window(conn, client->window);
	return 0;
}

static int tmbr_client_new(tmbr_client_t **out, xcb_window_t window)
{
	const uint32_t values[] = { XCB_EVENT_MASK_ENTER_WINDOW };
	xcb_void_cookie_t cookie;
	tmbr_client_t *client;

	if ((client = calloc(1, sizeof(*client))) == NULL)
		die("Unable to allocate client");
	client->window = window;

	cookie = xcb_change_window_attributes_checked(conn, window, XCB_CW_EVENT_MASK, values);
	if ((xcb_request_check(conn, cookie)) != NULL)
		die("Could not subscribe to window events");

	*out = client;
	return 0;
}

static void tmbr_client_free(tmbr_client_t *client)
{
	free(client);
}

static int tmbr_client_layout(tmbr_client_t *client, uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t border)
{
	uint32_t values[5];
	uint16_t mask =
		XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
		XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT |
		XCB_CONFIG_WINDOW_BORDER_WIDTH;

	values[0] = x;
	values[1] = y;
	values[2] = w - 2 * border;
	values[3] = h - 2 * border;
	values[4] = border;

	xcb_configure_window(conn, client->window, mask, values);
	return 0;
}

static int tmbr_client_set_fullscreen(tmbr_client_t *client, char fs)
{
	uint32_t values[] = { XCB_STACK_MODE_ABOVE };
	xcb_ewmh_set_wm_state(&ewmh, client->window, fs, &ewmh._NET_WM_STATE_FULLSCREEN);
	xcb_configure_window(conn, client->window, XCB_CONFIG_WINDOW_STACK_MODE, values);
	return 0;
}

static int tmbr_layout_tree(tmbr_tree_t *tree, uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
	uint16_t xoff, yoff, lw, rw, lh, rh;

	if (tree->client)
		return tmbr_client_layout(tree->client, x, y, w, h, TMBR_BORDER_WIDTH);

	if (tree->split == TMBR_SPLIT_VERTICAL) {
		lw = w * (tree->ratio / 100.0);
		rw = w - lw;
		lh = rh = h;
		xoff = lw;
		yoff = 0;
	} else {
		lh = h * (tree->ratio / 100.0);
		rh = h - lh;
		lw = rw = w;
		yoff = lh;
		xoff = 0;
	}

	if (tmbr_layout_tree(tree->left, x, y, lw, lh) < 0 ||
	    tmbr_layout_tree(tree->right, x + xoff, y + yoff, rw, rh) < 0)
		die("Unable to layout subtrees");

	return 0;
}

static int tmbr_desktop_new(tmbr_desktop_t **out)
{
	tmbr_desktop_t *d;

	if ((d = calloc(1, sizeof(*d))) == NULL)
		die("Unable to allocate desktop");

	*out = d;
	return 0;
}

static void tmbr_desktop_free(tmbr_desktop_t *desktop)
{
	free(desktop);
}

static int tmbr_desktop_get_focussed_client(tmbr_client_t **out, tmbr_desktop_t *desktop)
{
	if ((*out = desktop->focus) == NULL)
		return -1;
	return 0;
}

static int tmbr_desktop_set_focussed_client(tmbr_desktop_t *desktop, tmbr_client_t *client)
{
	tmbr_client_t *focus;

	if (!client)
		return 0;

	if (tmbr_desktop_get_focussed_client(&focus, desktop) == 0)
		tmbr_client_draw_border(focus, TMBR_COLOR_INACTIVE);
	tmbr_client_draw_border(client, TMBR_COLOR_ACTIVE);

	xcb_set_input_focus(conn, XCB_INPUT_FOCUS_PARENT, client->window, XCB_CURRENT_TIME);
	desktop->focus = client;

	return 0;
}

static int tmbr_desktop_layout(tmbr_desktop_t *desktop)
{
	if (!desktop->clients)
		return 0;

	if (tmbr_layout_tree(desktop->clients,
			     desktop->screen->x,
			     desktop->screen->y,
			     desktop->screen->width,
			     desktop->screen->height) < 0)
		die("Unable to layout tree");

	tmbr_discard_events(XCB_ENTER_NOTIFY);
	return 0;
}

static int tmbr_desktop_set_focussed(tmbr_desktop_t *desktop)
{
	tmbr_tree_t *it, *t;

	tmbr_tree_foreach_leaf(desktop->screen->focus->clients, it, t)
		tmbr_client_hide(t->client);
	tmbr_tree_foreach_leaf(desktop->clients, it, t)
		tmbr_client_show(t->client);

	tmbr_desktop_set_focussed_client(desktop, desktop->focus);
	return tmbr_desktop_layout(desktop);
}

static int tmbr_desktop_set_fullscreen(tmbr_desktop_t *desktop, tmbr_client_t *client, char fs)
{
	if (fs)
		tmbr_client_layout(client,
				   desktop->screen->x,
				   desktop->screen->y,
				   desktop->screen->width,
				   desktop->screen->height, 0);
	else
		tmbr_desktop_layout(desktop);

	return tmbr_client_set_fullscreen(client, fs);
}

static int tmbr_desktop_find_window(tmbr_client_t **out, tmbr_desktop_t *desktop,
		xcb_window_t window)
{
	tmbr_tree_t *n, *it;

	tmbr_tree_foreach_leaf(desktop->clients, it, n) {
		if (n->client->window == window) {
			*out = n->client;
			return 0;
		}
	}

	return -1;
}

static int tmbr_desktop_add_client(tmbr_desktop_t *desktop, tmbr_client_t *client)
{
	tmbr_client_t *focus = NULL;

	tmbr_desktop_get_focussed_client(&focus, desktop);
	if (tmbr_tree_insert(focus ? &focus->tree : &desktop->clients, client) < 0)
		die("Unable to insert client into tree");

	if (tmbr_desktop_set_focussed_client(desktop, client) < 0)
		die("Unable to focus client");
	client->desktop = desktop;

	return 0;
}

static int tmbr_desktop_remove_client(tmbr_desktop_t *desktop, tmbr_client_t *client)
{
	if (desktop->focus == client) {
		tmbr_tree_t *sibling = NULL;
		if (tmbr_tree_find_sibling(&sibling, client->tree, TMBR_SELECT_NEAREST) == 0)
			tmbr_desktop_set_focussed_client(desktop, sibling->client);
		else
			desktop->focus = NULL;
	}

	if (tmbr_tree_remove(&desktop->clients, client->tree) < 0)
		die("Unable to remove client from tree");

	client->desktop = NULL;
	client->tree = NULL;

	return 0;
}

static int tmbr_screen_manage_windows(tmbr_screen_t *screen)
{
	xcb_query_tree_reply_t *tree;
	xcb_window_t *children;
	int i;

	if ((tree = xcb_query_tree_reply(conn, xcb_query_tree(conn, screen->root), NULL)) == NULL)
		die("Unable to query tree");

	children = xcb_query_tree_children(tree);

	for (i = 0; i < xcb_query_tree_children_length(tree); i++) {
		xcb_get_window_attributes_reply_t *attrs = NULL;
		tmbr_client_t *client;

		if ((attrs = xcb_get_window_attributes_reply(conn,
							     xcb_get_window_attributes(conn, children[i]),
							     NULL)) == NULL)
			goto next;

		if (attrs->map_state == XCB_MAP_STATE_UNVIEWABLE)
			goto next;

		if (tmbr_client_new(&client, children[i]) < 0)
			die("Unable to create new client");
		if (tmbr_desktop_add_client(screen->focus, client) < 0)
			die("Unable to add client to desktop");
		if (attrs->map_state == XCB_MAP_STATE_UNMAPPED)
			tmbr_client_show(client);
next:
		free(attrs);
	}

	free(tree);

	return 0;
}

static int tmbr_screen_get_focussed(tmbr_screen_t **out)
{
	tmbr_screen_t *s;

	for (s = screens; s; s = s->next) {
		if (!s->focussed)
			continue;

		*out = s;
		return 0;
	}

	return -1;
}

static int tmbr_screen_set_focussed(tmbr_screen_t *screen)
{
	tmbr_screen_t *focus;
	if (tmbr_screen_get_focussed(&focus) == 0)
		focus->focussed = 0;
	screen->focussed = 1;
	return 0;
}

static int tmbr_screen_set_focussed_desktop(tmbr_screen_t *screen, tmbr_desktop_t *desktop)
{
	if (screen->focus == desktop)
		return 0;
	if (desktop->screen != screen)
		return -1;
	if (tmbr_desktop_set_focussed(desktop) < 0)
		return -1;
	screen->focus = desktop;
	return 0;
}

static int tmbr_screen_add_desktop(tmbr_screen_t *screen, tmbr_desktop_t *desktop)
{
	desktop->screen = screen;

	if (screen->focus) {
		desktop->prev = screen->focus;
		desktop->next = screen->focus->next;
		screen->focus->next = desktop;
	} else {
		desktop->prev = desktop->next = NULL;
		screen->desktops = screen->focus = desktop;
	}

	return 0;
}

static int tmbr_screen_remove_desktop(tmbr_screen_t *screen, tmbr_desktop_t *desktop)
{
	if (desktop->clients || (screen->desktops == desktop && !desktop->next))
		return -1;

	if (desktop->prev)
		desktop->prev->next = desktop->next;
	if (desktop->next)
		desktop->next->prev = desktop->prev;

	if (screen->desktops == desktop)
		screen->desktops = desktop->next;
	if (screen->focus == desktop)
		tmbr_screen_set_focussed_desktop(screen, desktop->next ? desktop->next : desktop->prev);

	return 0;
}

static int tmbr_screen_manage(xcb_window_t root, uint16_t x, uint16_t y, uint16_t width, uint16_t height)
{
	tmbr_desktop_t *d;
	tmbr_screen_t *s;

	if ((s = calloc(1, sizeof(*s))) == NULL)
		die("Cannot allocate screen");
	if (tmbr_desktop_new(&d) < 0 || tmbr_screen_add_desktop(s, d) < 0)
		die("Cannot set up desktop");

	s->root = root;
	s->x = x;
	s->y = y;
	s->width = width;
	s->height = height;
	s->next = screens;
	screens = s;

	if (tmbr_screen_set_focussed(s) < 0)
		die("Unable to focus screen");

	return 0;
}

static void tmbr_screens_free(tmbr_screen_t *s)
{
	tmbr_screen_t *n;

	for (; s; s = n) {
		n = s->next;
		free(s);
	}

	screens = NULL;
}

static int tmbr_client_find_by_focus(tmbr_client_t **out)
{
	tmbr_screen_t *screen;
	if (tmbr_screen_get_focussed(&screen) < 0 ||
	    tmbr_desktop_get_focussed_client(out, screen->focus) < 0)
		return -1;
	return 0;
}

static int tmbr_client_find_by_window(tmbr_client_t **out, xcb_window_t window)
{
	tmbr_screen_t *screen;
	tmbr_desktop_t *desktop;

	for (screen = screens; screen; screen = screen->next)
		for (desktop = screen->desktops; desktop; desktop = desktop->next)
			if ((tmbr_desktop_find_window(out, desktop, window)) == 0)
				return 0;

	return -1;
}

static int tmbr_handle_focus_in(xcb_focus_in_event_t *ev)
{
	tmbr_client_t *client;

	if (tmbr_client_find_by_focus(&client) < 0 ||
	    client->window == ev->event)
		return 0;

	return tmbr_desktop_set_focussed_client(client->desktop, client);
}

static int tmbr_handle_enter_notify(xcb_enter_notify_event_t *ev)
{
	tmbr_client_t *client;

	if (ev->mode != XCB_NOTIFY_MODE_NORMAL ||
	    tmbr_client_find_by_window(&client, ev->event) < 0)
		return 0;

	if (tmbr_desktop_set_focussed_client(client->desktop, client) < 0 ||
	    tmbr_screen_set_focussed(client->desktop->screen) < 0)
		return -1;

	return 0;
}

static int tmbr_handle_map_request(xcb_map_request_event_t *ev)
{
	tmbr_client_t *client;
	tmbr_screen_t *screen;

	if (tmbr_screen_get_focussed(&screen) < 0)
		return -1;
	if (tmbr_client_find_by_window(&client, ev->window) == 0)
		return 0;

	xcb_map_window(conn, ev->window);

	if (tmbr_client_new(&client, ev->window) < 0)
		die("Unable to create new client");

	if (tmbr_desktop_add_client(screen->focus, client) < 0)
		return -1;

	if (tmbr_desktop_layout(screen->focus) < 0)
		return -1;

	return 0;
}

static int tmbr_handle_destroy_notify(xcb_destroy_notify_event_t *ev)
{
	tmbr_desktop_t *desktop;
	tmbr_client_t *client;

	if (tmbr_client_find_by_window(&client, ev->window) < 0)
		return 0;
	desktop = client->desktop;

	if (tmbr_desktop_remove_client(desktop, client) < 0)
		die("Unable to remove client from tree");
	tmbr_client_free(client);

	return tmbr_desktop_layout(desktop);
}

static int tmbr_handle_client_message(xcb_client_message_event_t * ev)
{
	xcb_atom_t state = ev->data.data32[1];
	uint32_t action = ev->data.data32[0];
	tmbr_client_t *client;

	if (ev->type != ewmh._NET_WM_STATE ||
	    tmbr_client_find_by_window(&client, ev->window) < 0)
		return 0;

	if (state == ewmh._NET_WM_STATE_FULLSCREEN &&
	    tmbr_desktop_set_fullscreen(client->desktop, client, action == XCB_EWMH_WM_STATE_ADD) < 0)
		return -1;

	return 0;
}

static int tmbr_handle_event(xcb_generic_event_t *ev)
{
	switch (XCB_EVENT_RESPONSE_TYPE(ev)) {
		case XCB_FOCUS_IN:
			return tmbr_handle_focus_in((xcb_focus_in_event_t *) ev);
		case XCB_ENTER_NOTIFY:
			return tmbr_handle_enter_notify((xcb_enter_notify_event_t *) ev);
		case XCB_MAP_REQUEST:
			return tmbr_handle_map_request((xcb_map_request_event_t *) ev);
		case XCB_DESTROY_NOTIFY:
			return tmbr_handle_destroy_notify((xcb_destroy_notify_event_t *) ev);
		case XCB_CLIENT_MESSAGE:
			return tmbr_handle_client_message((xcb_client_message_event_t *) ev);
		default:
			return -1;
	}
}

static void tmbr_discard_events(uint8_t type)
{
	xcb_generic_event_t *ev;

	xcb_aux_sync(conn);

	while ((ev = xcb_poll_for_event(conn)) != NULL) {
		if (XCB_EVENT_RESPONSE_TYPE(ev) != type)
			tmbr_handle_event(ev);
		free(ev);
	}
}

static void tmbr_cmd_client_kill(const tmbr_command_args_t *args)
{
	tmbr_client_t *focus;

	TMBR_UNUSED(args);

	if (tmbr_client_find_by_focus(&focus) < 0)
		return;

	xcb_kill_client(conn, focus->window);
}

static void tmbr_cmd_client_focus(const tmbr_command_args_t *args)
{
	tmbr_client_t *focus;
	tmbr_tree_t *next;

	if (tmbr_client_find_by_focus(&focus) < 0 ||
	    tmbr_tree_find_sibling(&next, focus->tree, args->i) < 0)
		return;

	tmbr_desktop_set_focussed_client(focus->desktop, next->client);
}

static void tmbr_cmd_client_move(const tmbr_command_args_t *args)
{
	tmbr_desktop_t *source, *target;
	tmbr_client_t *focus;

	if (tmbr_client_find_by_focus(&focus) < 0)
		return;
	source = focus->desktop;

	if ((target = (args->i == TMBR_SELECT_PREV) ? focus->desktop->prev : focus->desktop->next) == NULL)
		return;

	tmbr_desktop_remove_client(focus->desktop, focus);
	tmbr_client_hide(focus);
	tmbr_desktop_add_client(target, focus);
	tmbr_desktop_layout(source);
}

static void tmbr_cmd_client_resize(const tmbr_command_args_t *args)
{
	tmbr_client_t *client;
	tmbr_select_t select;
	tmbr_split_t split;
	tmbr_tree_t *tree;

	if (tmbr_client_find_by_focus(&client) < 0)
		return;

	switch (args->dir) {
	    case TMBR_DIR_NORTH:
		split = TMBR_SPLIT_HORIZONTAL; select = TMBR_SELECT_NEXT; break;
	    case TMBR_DIR_SOUTH:
		split = TMBR_SPLIT_HORIZONTAL; select = TMBR_SELECT_PREV; break;
	    case TMBR_DIR_EAST:
		split = TMBR_SPLIT_VERTICAL; select = TMBR_SELECT_PREV; break;
	    case TMBR_DIR_WEST:
		split = TMBR_SPLIT_VERTICAL; select = TMBR_SELECT_NEXT; break;
	}

	for (tree = client->tree; tree; tree = tree->parent) {
		if (!tree->parent)
			return;
		if (tmbr_tree_get_child(tree->parent, select) != tree ||
		    tree->parent->split != split)
			continue;
		tree = tree->parent;
		break;
	}

	if ((args->i < 0 && args->i >= tree->ratio) ||
	    (args->i > 0 && args->i + tree->ratio >= 100))
		return;
	tree->ratio += args->i;
	tmbr_desktop_layout(client->desktop);
}

static void tmbr_cmd_client_swap(const tmbr_command_args_t *args)
{
	tmbr_client_t *focus;
	tmbr_tree_t *next;

	if (tmbr_client_find_by_focus(&focus) < 0 ||
	    tmbr_tree_find_sibling(&next, focus->tree, args->i) < 0 ||
	    tmbr_tree_swap(focus->tree, next) < 0)
		return;

	tmbr_desktop_layout(focus->desktop);
	tmbr_desktop_set_focussed_client(focus->desktop, next->client);
}

static void tmbr_cmd_desktop_new(const tmbr_command_args_t *args)
{
	tmbr_desktop_t *desktop;
	tmbr_screen_t *screen;

	TMBR_UNUSED(args);

	if (tmbr_desktop_new(&desktop) < 0 ||
	    tmbr_screen_get_focussed(&screen) < 0 ||
	    tmbr_screen_add_desktop(screen, desktop) < 0)
		return;

	tmbr_screen_set_focussed_desktop(screen, desktop);
}

static void tmbr_cmd_desktop_kill(const tmbr_command_args_t *args)
{
	tmbr_screen_t *screen;
	tmbr_desktop_t *desktop;

	TMBR_UNUSED(args);

	if (tmbr_screen_get_focussed(&screen) < 0)
		return;
	desktop = screen->focus;
	if (tmbr_screen_remove_desktop(screen, screen->focus) < 0)
		return;
	tmbr_desktop_free(desktop);
}

static void tmbr_cmd_desktop_focus(const tmbr_command_args_t *args)
{
	tmbr_desktop_t *c, *p;
	tmbr_screen_t *screen;

	if (tmbr_screen_get_focussed(&screen) < 0)
		return;

	for (p = NULL, c = screen->desktops; c; p = c, c = c->next)
		if (c == screen->focus)
			break;

	if (args->i == TMBR_SELECT_PREV && p)
		tmbr_screen_set_focussed_desktop(screen, p);
	else if (args->i == TMBR_SELECT_NEXT && c && c->next)
		tmbr_screen_set_focussed_desktop(screen, c->next);
}

static void tmbr_cmd_tree_rotate(const tmbr_command_args_t *args)
{
	tmbr_client_t *focus;

	TMBR_UNUSED(args);

	if (tmbr_client_find_by_focus(&focus) < 0 ||
	    !focus->tree->parent)
		return;

	focus->tree->parent->split = !focus->tree->parent->split;

	tmbr_desktop_layout(focus->desktop);
}

static void tmbr_handle_command(int fd)
{
	char cmd[BUFSIZ];
	ssize_t n;
	size_t i;

	if ((n = read(fd, cmd, sizeof(cmd) - 1)) <= 0)
		return;
	if (cmd[n - 1] == '\n')
		cmd[--n] = '\0';
	else
		cmd[n] = '\0';

	for (i = 0; i < sizeof(cmds) / sizeof(*cmds); i++) {
		if (strcmp(cmds[i].cmd, cmd))
			continue;
		cmds[i].fn(&cmds[i].args);
	}
}

static void tmbr_cleanup(int signal)
{
	TMBR_UNUSED(signal);

	if (fifofd >= 0)
		close(fifofd);
	unlink(TMBR_CTRL_PATH);

	tmbr_screens_free(screens);
	xcb_ewmh_connection_wipe(&ewmh);
	xcb_disconnect(conn);
}

static int tmbr_ewmh_setup(xcb_connection_t *conn)
{
	xcb_atom_t atoms[2];

	if (xcb_ewmh_init_atoms_replies(&ewmh, xcb_ewmh_init_atoms(conn, &ewmh), NULL) == 0)
		die("Unable to initialize EWMH atoms");

	atoms[0] = ewmh._NET_WM_STATE;
	atoms[1] = ewmh._NET_WM_STATE_FULLSCREEN;

	xcb_ewmh_set_supported(&ewmh, 0, sizeof(atoms) / sizeof(*atoms), atoms);

	return 0;
}

static int tmbr_display_setup(xcb_connection_t *conn)
{
	const uint32_t values[] = {
		XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT |
		XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY
	};
	xcb_screen_t *screen;

	if ((screen = xcb_setup_roots_iterator(xcb_get_setup(conn)).data) == NULL)
		die("Unable to get root screen");

	if (xcb_request_check(conn, xcb_change_window_attributes_checked(conn, screen->root,
									 XCB_CW_EVENT_MASK, values)) != NULL)
		die("Another window manager is running already.");

	if (xcb_get_extension_data(conn, &xcb_randr_id)->present) {
		xcb_randr_get_screen_resources_reply_t *screens;
		xcb_randr_output_t *outputs;
		int i;

		if ((screens = xcb_randr_get_screen_resources_reply(conn,
								    xcb_randr_get_screen_resources(conn, screen->root),
								    NULL)) == NULL)
			die("Unable to get screen resources");

		outputs = xcb_randr_get_screen_resources_outputs(screens);

		for (i = 0; i < xcb_randr_get_screen_resources_outputs_length(screens); i++) {
			xcb_randr_get_output_info_reply_t *output = NULL;
			xcb_randr_get_crtc_info_reply_t *crtc = NULL;

			output = xcb_randr_get_output_info_reply(conn,
								 xcb_randr_get_output_info(conn, outputs[i], XCB_CURRENT_TIME),
								 NULL);
			if (output == NULL || output->crtc == XCB_NONE)
				goto next;

			crtc = xcb_randr_get_crtc_info_reply(conn,
							     xcb_randr_get_crtc_info(conn, output->crtc, XCB_CURRENT_TIME),
							     NULL);
			if (crtc == NULL)
				goto next;

			tmbr_screen_manage(screen->root, crtc->x, crtc->y, crtc->width, crtc->height);
next:
			free(output);
			free(crtc);
		}

		free(screens);
	} else {
		tmbr_screen_manage(screen->root, 0, 0, screen->width_in_pixels, screen->height_in_pixels);
	}

	if (tmbr_screen_manage_windows(screens) < 0)
		die("Unable to manage clients");

	if (tmbr_desktop_layout(screens->focus) < 0)
		die("Unable to layout screen");

	return 0;
}

static int tmbr_setup(void)
{
	if ((mkdir(TMBR_CTRL_DIR, 0700) < 0 && errno != EEXIST) ||
	    (mkfifo(TMBR_CTRL_PATH, 0600) < 0 && errno != EEXIST))
		die("Unable to create fifo");

	if ((fifofd = open(TMBR_CTRL_PATH, O_RDWR|O_NONBLOCK)) < 0)
		die("Unable to open fifo");

	if ((conn = xcb_connect(NULL, NULL)) == NULL ||
	    xcb_connection_has_error(conn) != 0)
		die("Unable to connect to X server");

	if (tmbr_ewmh_setup(conn) < 0)
		die("Unable to setup EWMH");

	if (tmbr_display_setup(conn) < 0)
		die("Unable to setup display");

	signal(SIGINT, tmbr_cleanup);
	signal(SIGHUP, tmbr_cleanup);
	signal(SIGTERM, tmbr_cleanup);
	signal(SIGCHLD, tmbr_cleanup);

	return 0;
}

int main(int argc, const char *argv[])
{
	struct pollfd fds[2];

	if (argc > 1)
		die("USAGE: %s\n", argv[0]);

	if (tmbr_setup() < 0)
		die("Unable to setup timber");

	fds[0].fd = xcb_get_file_descriptor(conn);
	fds[0].events = POLLIN;
	fds[1].fd = fifofd;
	fds[1].events = POLLIN;

	while (1) {
		xcb_generic_event_t *ev;

		xcb_flush(conn);

		if (poll(fds, 2, -1) < 0)
			die("timber: unable to poll for events");

		if (fds[0].revents & POLLIN) {
			while ((ev = xcb_poll_for_event(conn)) != NULL) {
				tmbr_handle_event(ev);
				free(ev);
			}
		}

		if (fds[1].revents & POLLIN)
			tmbr_handle_command(fds[1].fd);
	}

	return 0;
}

/* vim: set tabstop=8 noexpandtab : */
