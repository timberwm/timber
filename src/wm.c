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
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <sys/socket.h>

#include <xcb/xcb.h>
#include <xcb/xcb_aux.h>
#include <xcb/xcb_event.h>
#include <xcb/randr.h>

#include "common.h"
#include "config.h"
#include "wm.h"

#define TMBR_UNUSED __attribute__((unused))

typedef struct tmbr_client tmbr_client_t;
typedef struct tmbr_desktop tmbr_desktop_t;
typedef struct tmbr_screen tmbr_screen_t;
typedef struct tmbr_tree tmbr_tree_t;

typedef enum {
	TMBR_SPLIT_VERTICAL,
	TMBR_SPLIT_HORIZONTAL
} tmbr_split_t;

typedef enum {
	TMBR_WM_STATE_WITHDRAWN = 0,
	TMBR_WM_STATE_NORMAL = 1
} tmbr_wm_state_t;

struct tmbr_client {
	tmbr_desktop_t *desktop;
	tmbr_tree_t *tree;
	xcb_window_t window;
	uint16_t w, h;
	int16_t x, y;
};

struct tmbr_desktop {
	tmbr_desktop_t *prev;
	tmbr_desktop_t *next;
	tmbr_screen_t *screen;
	tmbr_tree_t *clients;
	tmbr_client_t *focus;
	uint8_t fullscreen;
};

struct tmbr_screen {
	tmbr_screen_t *next;
	tmbr_desktop_t *desktops;
	tmbr_desktop_t *focus;
	xcb_randr_output_t output;
	uint16_t w, h;
	int16_t x, y;
};

struct tmbr_tree {
	tmbr_tree_t *parent;
	tmbr_tree_t *left;
	tmbr_tree_t *right;
	tmbr_client_t *client;
	tmbr_split_t split;
	uint8_t ratio;
};

static struct {
	tmbr_screen_t *screens;
	tmbr_screen_t *screen;
	xcb_connection_t *conn;
	xcb_window_t root, meta;
	const xcb_query_extension_reply_t *randr;
	const char *ctrl_path;
	struct {
		xcb_atom_t wm_delete_window;
		xcb_atom_t wm_take_focus;
		xcb_atom_t wm_protocols;
		xcb_atom_t wm_state;
		xcb_atom_t net_supported;
		xcb_atom_t net_wm_state;
		xcb_atom_t net_wm_state_fullscreen;
	} atoms;
	int ctrlfd;
	int subfds[10];
	uint8_t ignored_events;
} state = { NULL, NULL, NULL, 0, 0, NULL, NULL, { 0, 0, 0, 0, 0, 0, 0 }, -1, { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 }, 0 };

static __attribute__((format(printf, 1, 2))) void tmbr_notify(const char *fmt, ...)
{
	char buf[4096];
	unsigned i;
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	for (i = 0; i < ARRAY_SIZE(state.subfds); i++) {
		if (state.subfds[i] < 0)
			continue;
		if (tmbr_ctrl_write(state.subfds[i], TMBR_PKT_DATA, "%s", buf) < 0) {
			close(state.subfds[i]);
			state.subfds[i] = -1;
		}
	}
}

static int tmbr_tree_insert(tmbr_tree_t **tree, tmbr_client_t *client)
{
	tmbr_tree_t *l, *r, *p = *tree;

	r = tmbr_alloc(sizeof(*r), "Unable to allocate right tree node");
	r->client = client;
	r->client->tree = r;
	r->parent = p;

	if (p) {
		l = tmbr_alloc(sizeof(*l), "Unable to allocate left tree node");
		l->client = p->client;
		l->client->tree = l;
		l->left = p->left;
		l->right = p->right;
		l->parent = p;

		p->client = NULL;
		p->left = l;
		p->right = r;
		p->ratio = 50;
		p->split = (l->client->w < l->client->h) ? TMBR_SPLIT_HORIZONTAL : TMBR_SPLIT_VERTICAL;
	} else {
		*tree = r;
	}

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

	while (t && t->parent) {
		if (t != tmbr_tree_get_child(t->parent, upwards)) {
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
	if (node != *tree) {
		tmbr_tree_t *uplift = (node->parent->left == node) ?
					node->parent->right : node->parent->left;
		if (tmbr_tree_swap(uplift, node->parent) < 0)
			return -1;
		free(uplift);
	} else {
		*tree = NULL;
	}

	free(node);
	return 0;
}

static int tmbr_client_draw_border(tmbr_client_t *client, uint32_t color)
{
	xcb_change_window_attributes(state.conn, client->window, XCB_CW_BORDER_PIXEL, &color);
	return 0;
}

static int tmbr_client_set_wm_state(tmbr_client_t *client, tmbr_wm_state_t wmstate)
{
	uint32_t data[2] = { 0, XCB_NONE };
	data[0] = wmstate;
	xcb_change_property(state.conn, XCB_PROP_MODE_REPLACE, client->window,
			    state.atoms.wm_state, state.atoms.wm_state, 32, 2, data);
	return 0;
}

static int tmbr_client_new(tmbr_client_t **out, xcb_window_t window)
{
	const uint32_t values[] = { XCB_EVENT_MASK_ENTER_WINDOW };
	xcb_void_cookie_t cookie;
	tmbr_client_t *client;

	client = tmbr_alloc(sizeof(*client), "Unable to allocate client");
	client->window = window;

	cookie = xcb_change_window_attributes_checked(state.conn, window, XCB_CW_EVENT_MASK, values);
	if ((xcb_request_check(state.conn, cookie)) != NULL)
		die("Could not subscribe to window events");

	if (tmbr_client_set_wm_state(client, TMBR_WM_STATE_NORMAL) < 0)
		die("Unable to set WM state to 'normal'");

	*out = client;
	return 0;
}

static void tmbr_client_free(tmbr_client_t *client)
{
	free(client);
}

static int tmbr_client_send_message(tmbr_client_t *client, xcb_atom_t value)
{
	xcb_client_message_event_t msg;
	xcb_get_property_reply_t *prop;
	xcb_atom_t *atoms;
	size_t i, len;

	memset(&msg, 0, sizeof(msg));

	if ((prop = xcb_get_property_reply(state.conn, xcb_get_property(state.conn, 0, client->window,
									state.atoms.wm_protocols, XCB_ATOM_ATOM,
									0, UINT_MAX), NULL)) == NULL)
		return -1;
	atoms = (xcb_atom_t *) xcb_get_property_value(prop);
	len = (unsigned) xcb_get_property_value_length(prop) / sizeof(*atoms);

	for (i = 0; i < len; i++) {
		if (atoms[i] != value)
			continue;
		msg.response_type = XCB_CLIENT_MESSAGE;
		msg.window = client->window;
		msg.type = state.atoms.wm_protocols;
		msg.format = 32;
		msg.data.data32[0] = value;
		msg.data.data32[1] = XCB_CURRENT_TIME;
		xcb_send_event(state.conn, 0, client->window, XCB_EVENT_MASK_NO_EVENT, (char *) &msg);
		break;
	}

	free(prop);
	return (i == len) ? -1 : 0;
}

static int tmbr_client_focus(tmbr_client_t *client)
{
	if (!client)
		return 0;
	tmbr_client_draw_border(client, TMBR_COLOR_ACTIVE);
	if (tmbr_client_send_message(client, state.atoms.wm_take_focus) < 0)
		xcb_set_input_focus(state.conn, XCB_INPUT_FOCUS_PARENT, client->window, XCB_CURRENT_TIME);
	return 0;
}

static int tmbr_client_unfocus(tmbr_client_t *client)
{
	if (!client)
		return 0;
	tmbr_client_draw_border(client, TMBR_COLOR_INACTIVE);
	xcb_set_input_focus(state.conn, XCB_INPUT_FOCUS_PARENT, state.meta, XCB_CURRENT_TIME);
	return 0;
}

static void tmbr_client_kill(tmbr_client_t *client)
{
	if (tmbr_client_send_message(client, state.atoms.wm_delete_window) < 0)
		xcb_kill_client(state.conn, client->window);
}

static int tmbr_client_move(tmbr_client_t *client, int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t border)
{
	uint32_t values[5];
	uint16_t mask =
		XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
		XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT |
		XCB_CONFIG_WINDOW_BORDER_WIDTH;

	((int32_t *) values)[0] = client->x = x;
	((int32_t *) values)[1] = client->y = y;
	values[2] = client->w = w - 2 * border;
	values[3] = client->h = h - 2 * border;
	values[4] = border;

	xcb_configure_window(state.conn, client->window, mask, values);
	return 0;
}

static int tmbr_client_hide(tmbr_client_t *c)
{
	return tmbr_client_move(c, c->w * -1, c->y, c->w, c->h, 0);
}

static int tmbr_client_set_fullscreen(tmbr_client_t *client, uint8_t fs)
{
	uint32_t stacking;
	xcb_change_property(state.conn, XCB_PROP_MODE_REPLACE,
			    client->window, state.atoms.net_wm_state, XCB_ATOM_ATOM, 32,
			    fs, &state.atoms.net_wm_state_fullscreen);
	stacking = fs ? XCB_STACK_MODE_ABOVE : XCB_STACK_MODE_BELOW;
	xcb_configure_window(state.conn, client->window, XCB_CONFIG_WINDOW_STACK_MODE, &stacking);
	return 0;
}

static int tmbr_layout_tree(tmbr_tree_t *tree, int16_t x, int16_t y, uint16_t w, uint16_t h)
{
	uint16_t xoff, yoff, lw, rw, lh, rh;

	if (tree->client)
		return tmbr_client_move(tree->client, x, y, w, h, TMBR_BORDER_WIDTH);

	if (tree->split == TMBR_SPLIT_VERTICAL) {
		lw = (uint16_t) (w * (tree->ratio / 100.0));
		rw = w - lw;
		lh = rh = h;
		xoff = lw;
		yoff = 0;
	} else {
		lh = (uint16_t) (h * (tree->ratio / 100.0));
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
	*out = tmbr_alloc(sizeof(**out), "Unable to allocate desktop");
	return 0;
}

static void tmbr_desktop_free(tmbr_desktop_t *desktop)
{
	free(desktop);
}

static int tmbr_desktop_find_sibling(tmbr_desktop_t **out, tmbr_desktop_t *desktop, tmbr_select_t which)
{
	if (!desktop || (!desktop->prev && !desktop->next))
		return -1;
	if ((*out = (which == TMBR_SELECT_PREV) ? desktop->prev : desktop->next) != NULL)
		return 0;

	if (which == TMBR_SELECT_PREV)
		for (*out = desktop->screen->desktops; *out && (*out)->next; *out = (*out)->next);
	else if  (which == TMBR_SELECT_NEXT)
		*out = desktop->screen->desktops;
	return 0;
}

static int tmbr_desktop_layout(tmbr_desktop_t *desktop)
{
	int error;

	if (!desktop->clients || desktop->screen->focus != desktop)
		return 0;

	if (desktop->fullscreen)
		error = tmbr_client_move(desktop->focus,
					 desktop->screen->x, desktop->screen->y,
					 desktop->screen->w, desktop->screen->h, 0);
	else
		error = tmbr_layout_tree(desktop->clients,
					 desktop->screen->x, desktop->screen->y,
					 desktop->screen->w, desktop->screen->h);

	if (error < 0)
		die("Unable to layout desktop");

	xcb_aux_sync(state.conn);
	state.ignored_events = XCB_ENTER_NOTIFY;
	return 0;
}

static int tmbr_desktop_hide(tmbr_desktop_t *d)
{
	tmbr_tree_t *it, *t;
	tmbr_tree_foreach_leaf(d->clients, it, t)
		tmbr_client_hide(t->client);
	return 0;
}

static int tmbr_desktop_focus(tmbr_desktop_t *desktop, tmbr_client_t *client, int inputfocus)
{
	if (inputfocus && (tmbr_client_unfocus(desktop->focus) < 0 || tmbr_client_focus(client) < 0))
		return -1;
	if (desktop->focus == client)
		return 0;
	if (desktop->fullscreen && tmbr_client_set_fullscreen(desktop->focus, 0) < 0)
		return -1;

	desktop->fullscreen = 0;
	desktop->focus = client;

	return tmbr_desktop_layout(desktop);
}

static int tmbr_desktop_unfocus(tmbr_desktop_t *desktop)
{
	if (!desktop)
		return 0;
	return tmbr_client_unfocus(desktop->focus);
}

static int tmbr_desktop_swap(tmbr_desktop_t *a, tmbr_desktop_t *b)
{
	tmbr_desktop_t *tmp;

	if (a->screen != b->screen)
		return -1;

	tmp = a->next;
	a->next = b->next;
	b->next = tmp;
	if (a->next) a->next->prev = a;
	if (b->next) b->next->prev = b;

	tmp = a->prev;
	a->prev = b->prev;
	b->prev = tmp;
	if (a->prev) a->prev->next = a;
	if (b->prev) b->prev->next = b;

	if (!a->prev) a->screen->desktops = a;
	if (!b->prev) b->screen->desktops = b;

	return 0;
}

static int tmbr_desktop_set_fullscreen(tmbr_desktop_t *desktop, tmbr_client_t *client, uint8_t fs)
{
	if (tmbr_desktop_focus(desktop, client, 1) < 0)
		return -1;

	desktop->fullscreen = fs;

	if (tmbr_desktop_layout(desktop) < 0 ||
	    tmbr_client_set_fullscreen(client, fs) < 0)
		return -1;

	return 0;
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
	if (tmbr_tree_insert(desktop->focus ? &desktop->focus->tree : &desktop->clients, client) < 0)
		die("Unable to insert client into tree");
	client->desktop = desktop;
	return 0;
}

static int tmbr_desktop_remove_client(tmbr_desktop_t *desktop, tmbr_client_t *client)
{
	if (desktop->focus == client) {
		int setfocus = (desktop->screen == state.screen && desktop->screen->focus == desktop);
		tmbr_tree_t *sibling;

		if (tmbr_tree_find_sibling(&sibling, client->tree, TMBR_SELECT_NEAREST) < 0)
			sibling = NULL;
		if (tmbr_desktop_focus(desktop, sibling ? sibling->client : NULL, setfocus) < 0)
			return -1;
	}

	if (tmbr_tree_remove(&desktop->clients, client->tree) < 0)
		die("Unable to remove client from tree");

	desktop->fullscreen = 0;
	client->desktop = NULL;
	client->tree = NULL;

	return tmbr_desktop_layout(desktop);
}

static int tmbr_screen_find_by_output(tmbr_screen_t **out, xcb_randr_output_t output)
{
	for (*out = state.screens; *out; *out = (*out)->next)
		if ((*out)->output == output)
			return 0;
	return -1;
}

static int tmbr_screen_find_sibling(tmbr_screen_t **out, tmbr_screen_t *screen, tmbr_select_t which)
{
	tmbr_screen_t *p;

	if (which == TMBR_SELECT_PREV)
		for (p = state.screens; p && p->next != screen; p = p->next);
	else
		p = screen->next;

	if (!p && which == TMBR_SELECT_PREV)
		for (p = state.screens; p && p->next; p = p->next);
	else if (!p && which == TMBR_SELECT_NEXT)
		p = state.screens;

	return (*out = p) ? 0 : -1;
}

static int tmbr_screen_focus(tmbr_screen_t *screen)
{
	if (state.screen == screen)
		return 0;
	if ((state.screen && tmbr_desktop_unfocus(state.screen->focus) < 0) ||
	    tmbr_desktop_focus(screen->focus, screen->focus->focus, 1) < 0)
		return -1;
	state.screen = screen;
	return 0;
}

static int tmbr_screen_focus_desktop(tmbr_screen_t *screen, tmbr_desktop_t *desktop)
{
	if (screen->focus == desktop)
		return 0;
	if (desktop->screen != screen)
		return -1;

	if (tmbr_desktop_unfocus(screen->focus) < 0 ||
	    tmbr_desktop_hide(screen->focus) < 0 ||
	    tmbr_desktop_focus(desktop, desktop->focus, 1) < 0)
		return -1;

	screen->focus = desktop;
	return tmbr_desktop_layout(desktop);
}

static int tmbr_screen_add_desktop(tmbr_screen_t *screen, tmbr_desktop_t *desktop)
{
	tmbr_desktop_t *prev = screen->focus, *next = prev ? prev->next : NULL;

	desktop->screen = screen;
	desktop->prev = prev;
	desktop->next = next;

	if (prev)
		prev->next = desktop;
	if (next)
		next->prev = desktop;
	if (!screen->focus)
		screen->desktops = screen->focus = desktop;

	return 0;
}

static int tmbr_screen_remove_desktop(tmbr_screen_t *screen, tmbr_desktop_t *desktop)
{
	if (desktop->clients || (!desktop->prev && !desktop->next))
		return -1;

	if (desktop->prev)
		desktop->prev->next = desktop->next;
	if (desktop->next)
		desktop->next->prev = desktop->prev;

	if (screen->desktops == desktop)
		screen->desktops = desktop->next;
	if (screen->focus == desktop)
		tmbr_screen_focus_desktop(screen, desktop->next ? desktop->next : desktop->prev);

	return 0;
}

static int tmbr_screen_manage_windows(tmbr_screen_t *screen)
{
	xcb_query_tree_reply_t *tree;
	xcb_window_t *children;
	int i;

	if ((tree = xcb_query_tree_reply(state.conn, xcb_query_tree(state.conn, state.root), NULL)) == NULL)
		die("Unable to query tree");

	children = xcb_query_tree_children(tree);

	for (i = 0; i < xcb_query_tree_children_length(tree); i++) {
		xcb_get_window_attributes_reply_t *attrs = NULL;
		tmbr_client_t *client;

		if ((attrs = xcb_get_window_attributes_reply(state.conn,
							     xcb_get_window_attributes(state.conn, children[i]),
							     NULL)) == NULL)
			goto next;

		if (attrs->map_state != XCB_MAP_STATE_VIEWABLE || attrs->override_redirect)
			goto next;

		if (tmbr_client_new(&client, children[i]) < 0 ||
		    tmbr_desktop_add_client(screen->focus, client) < 0 ||
		    tmbr_desktop_focus(screen->focus, client, 1) < 0)
			die("Unable to adopt client");
next:
		free(attrs);
	}

	free(tree);

	return 0;
}

static int tmbr_screen_manage(xcb_randr_output_t output, int16_t x, int16_t y, uint16_t width, uint16_t height)
{
	tmbr_desktop_t *d;
	tmbr_screen_t *s;

	if (tmbr_screen_find_by_output(&s, output) < 0) {
		s = tmbr_alloc(sizeof(*s), "Unable to allocate screen");
		if (tmbr_desktop_new(&d) < 0 || tmbr_screen_add_desktop(s, d) < 0)
			die("Cannot set up desktop");
		s->output = output;
		s->next = state.screens;
		state.screens = s;
	}

	s->x = x;
	s->y = y;
	s->w = width;
	s->h = height;

	return tmbr_desktop_layout(s->focus);
}

static int tmbr_screens_update(xcb_screen_t *screen)
{
	if (state.randr->present) {
		xcb_randr_get_screen_resources_reply_t *screens;
		xcb_randr_output_t *outputs;
		int i;

		if ((screens = xcb_randr_get_screen_resources_reply(state.conn,
								    xcb_randr_get_screen_resources(state.conn, screen->root),
								    NULL)) == NULL)
			die("Unable to get screen resources");

		outputs = xcb_randr_get_screen_resources_outputs(screens);

		for (i = 0; i < xcb_randr_get_screen_resources_outputs_length(screens); i++) {
			xcb_randr_get_output_info_reply_t *output = NULL;
			xcb_randr_get_crtc_info_reply_t *crtc = NULL;

			output = xcb_randr_get_output_info_reply(state.conn,
								 xcb_randr_get_output_info(state.conn, outputs[i], XCB_CURRENT_TIME),
								 NULL);
			if (output == NULL || output->crtc == XCB_NONE)
				goto next;

			crtc = xcb_randr_get_crtc_info_reply(state.conn,
							     xcb_randr_get_crtc_info(state.conn, output->crtc, XCB_CURRENT_TIME),
							     NULL);
			if (crtc == NULL)
				goto next;

			tmbr_screen_manage(outputs[i], crtc->x, crtc->y, crtc->width, crtc->height);
next:
			free(output);
			free(crtc);
		}

		free(screens);
		return 0;
	} else {
		return tmbr_screen_manage(0, 0, 0, screen->width_in_pixels, screen->height_in_pixels);
	}
}

static void tmbr_screens_free(tmbr_screen_t *s)
{
	tmbr_screen_t *n;

	for (; s; s = n) {
		n = s->next;
		free(s);
	}

	state.screens = NULL;
}

static int tmbr_client_find_by_focus(tmbr_client_t **out)
{
	if ((*out = state.screen->focus->focus) == NULL)
		return -1;
	return 0;
}

static int tmbr_client_find_by_window(tmbr_client_t **out, xcb_window_t window)
{
	tmbr_screen_t *screen;
	tmbr_desktop_t *desktop;

	for (screen = state.screens; screen; screen = screen->next)
		for (desktop = screen->desktops; desktop; desktop = desktop->next)
			if ((tmbr_desktop_find_window(out, desktop, window)) == 0)
				return 0;

	return -1;
}

static void tmbr_handle_enter_notify(xcb_enter_notify_event_t *ev)
{
	tmbr_client_t *client;

	if (ev->mode != XCB_NOTIFY_MODE_NORMAL ||
	    tmbr_client_find_by_window(&client, ev->event) < 0 ||
	    tmbr_desktop_focus(client->desktop, client, 1) < 0 ||
	    tmbr_screen_focus(client->desktop->screen) < 0)
		return;

	tmbr_notify("event: enter-notify(window=%d)", ev->event);
}

static void tmbr_handle_map_request(xcb_map_request_event_t *ev)
{
	xcb_get_window_attributes_reply_t *attrs;
	tmbr_client_t *client;
	uint8_t override;

	attrs = xcb_get_window_attributes_reply(state.conn, xcb_get_window_attributes(state.conn, ev->window), NULL);
	override = attrs ? attrs->override_redirect : 0;
	free(attrs);

	if (override || tmbr_client_find_by_window(&client, ev->window) == 0)
		return;

	if (tmbr_client_new(&client, ev->window) < 0 ||
	    tmbr_desktop_add_client(state.screen->focus, client) < 0 ||
	    tmbr_desktop_layout(state.screen->focus) < 0)
		die("Unable to manage client");

	xcb_map_window(state.conn, ev->window);

	if (tmbr_desktop_focus(state.screen->focus, client, 1) < 0)
		die("Unable to focus new client");

	tmbr_notify("event: map-request(window=%d)", ev->window);
}

static void tmbr_handle_unmap_notify(xcb_unmap_notify_event_t *ev)
{
	tmbr_client_t *client;
	if (tmbr_client_find_by_window(&client, ev->window) == 0 &&
	    tmbr_client_set_wm_state(client, TMBR_WM_STATE_WITHDRAWN) < 0)
		die("Unable to set WM state to 'withdrawn'");
	xcb_aux_sync(state.conn);
	state.ignored_events = XCB_ENTER_NOTIFY;
	tmbr_notify("event: unmap-notify(window=%d)", ev->window);
}

static void tmbr_handle_destroy_notify(xcb_destroy_notify_event_t *ev)
{
	tmbr_client_t *client;
	if (tmbr_client_find_by_window(&client, ev->window) < 0)
		return;
	if (tmbr_desktop_remove_client(client->desktop, client) < 0)
		die("Unable to remove client from tree");
	tmbr_client_free(client);
	tmbr_notify("event: destroy-notify(window=%d)", ev->window);
}

static void tmbr_handle_client_message(xcb_client_message_event_t * ev)
{
	tmbr_client_t *client;

	if (ev->type != state.atoms.net_wm_state || tmbr_client_find_by_window(&client, ev->window) < 0)
		return;
	if (ev->data.data32[1] == state.atoms.net_wm_state_fullscreen)
		tmbr_desktop_set_fullscreen(client->desktop, client, ev->data.data32[0] == 1);

	tmbr_notify("event: client-message(window=%d)", ev->window);
}

static void tmbr_handle_error(xcb_request_error_t *ev)
{
	if (ev->error_code == 3 /* BAD_WINDOW */)
		return;

	tmbr_notify("event: error(code=%d)", ev->error_code);
	die("X11 error when handling request '%s': %s",
	    xcb_event_get_request_label(ev->major_opcode),
	    xcb_event_get_error_label(ev->error_code));
}

static void tmbr_handle_screen_change_notify(void)
{
	xcb_screen_t *screen;
	if ((screen = xcb_setup_roots_iterator(xcb_get_setup(state.conn)).data) == NULL)
		die("Unable to get root screen");
	tmbr_screens_update(screen);
	tmbr_notify("event: screen-change-notify");
}

static void tmbr_handle_event(xcb_generic_event_t *ev)
{
	uint8_t type = XCB_EVENT_RESPONSE_TYPE(ev);

	if (type == XCB_ENTER_NOTIFY)
		tmbr_handle_enter_notify((xcb_enter_notify_event_t *) ev);
	else if (type == XCB_MAP_REQUEST)
		tmbr_handle_map_request((xcb_map_request_event_t *) ev);
	else if (type == XCB_UNMAP_NOTIFY)
		tmbr_handle_unmap_notify((xcb_unmap_notify_event_t *) ev);
	else if (type == XCB_DESTROY_NOTIFY)
		tmbr_handle_destroy_notify((xcb_destroy_notify_event_t *) ev);
	else if (type == XCB_CLIENT_MESSAGE)
		tmbr_handle_client_message((xcb_client_message_event_t *) ev);
	else if (state.randr->present && type == state.randr->first_event + XCB_RANDR_SCREEN_CHANGE_NOTIFY)
		tmbr_handle_screen_change_notify();
	else if (!type)
		tmbr_handle_error((xcb_request_error_t *) ev);
}

static int tmbr_cmd_client_kill(TMBR_UNUSED const tmbr_command_args_t *args)
{
	tmbr_client_t *focus;
	if (tmbr_client_find_by_focus(&focus) < 0)
		return ENOENT;
	tmbr_client_kill(focus);
	return 0;
}

static int tmbr_cmd_client_focus(const tmbr_command_args_t *args)
{
	tmbr_client_t *focus;
	tmbr_tree_t *next;

	if (tmbr_client_find_by_focus(&focus) < 0 ||
	    tmbr_tree_find_sibling(&next, focus->tree, args->sel) < 0)
		return ENOENT;
	if (tmbr_desktop_focus(focus->desktop, next->client, 1) < 0)
		return EFAULT;

	return 0;
}

static int tmbr_cmd_client_fullscreen(TMBR_UNUSED const tmbr_command_args_t *args)
{
	tmbr_desktop_t *d = state.screen->focus;
	if (!d->focus)
		return ENOENT;
	if (tmbr_desktop_set_fullscreen(d, d->focus, !d->fullscreen) < 0)
		return EIO;
	return 0;
}

static int tmbr_cmd_client_to_desktop(const tmbr_command_args_t *args)
{
	tmbr_desktop_t *target;
	tmbr_client_t *focus;

	if (tmbr_client_find_by_focus(&focus) < 0 ||
	    tmbr_desktop_find_sibling(&target, focus->desktop, args->sel) < 0)
		return ENOENT;

	if (tmbr_desktop_remove_client(focus->desktop, focus) < 0 ||
	    tmbr_client_hide(focus) < 0 ||
	    tmbr_desktop_add_client(target, focus) < 0 ||
	    tmbr_desktop_focus(target, focus, 0))
		return EIO;

	return 0;
}

static int tmbr_cmd_client_resize(const tmbr_command_args_t *args)
{
	tmbr_client_t *client;
	tmbr_select_t select;
	tmbr_split_t split;
	tmbr_tree_t *tree;
	int i;

	if (tmbr_client_find_by_focus(&client) < 0)
		return ENOENT;

	switch (args->dir) {
	    case TMBR_DIR_NORTH:
		split = TMBR_SPLIT_HORIZONTAL; select = TMBR_SELECT_NEXT; i = args->i * -1; break;
	    case TMBR_DIR_SOUTH:
		split = TMBR_SPLIT_HORIZONTAL; select = TMBR_SELECT_PREV; i = args->i; break;
	    case TMBR_DIR_EAST:
		split = TMBR_SPLIT_VERTICAL; select = TMBR_SELECT_PREV; i = args->i; break;
	    case TMBR_DIR_WEST:
		split = TMBR_SPLIT_VERTICAL; select = TMBR_SELECT_NEXT; i = args->i * -1; break;
	}

	for (tree = client->tree; tree; tree = tree->parent) {
		if (!tree->parent)
			return ENOENT;
		if (tmbr_tree_get_child(tree->parent, select) != tree ||
		    tree->parent->split != split)
			continue;
		tree = tree->parent;
		break;
	}

	if ((i < 0 && i >= tree->ratio) || (i > 0 && i + tree->ratio >= 100))
		return EINVAL;
	tree->ratio += i;
	if (tmbr_desktop_layout(client->desktop) < 0)
		return EIO;

	return 0;
}

static int tmbr_cmd_client_to_screen(const tmbr_command_args_t *args)
{
	tmbr_screen_t *screen;
	tmbr_client_t *client;

	if (tmbr_client_find_by_focus(&client) < 0 ||
	    tmbr_screen_find_sibling(&screen, client->desktop->screen, args->sel) < 0)
		return ENOENT;

	if (tmbr_desktop_remove_client(client->desktop, client) < 0 ||
	    tmbr_desktop_add_client(screen->focus, client) < 0 ||
	    tmbr_desktop_focus(screen->focus, client, 0) < 0)
		return EIO;

	return 0;
}

static int tmbr_cmd_client_swap(const tmbr_command_args_t *args)
{
	tmbr_client_t *focus;
	tmbr_tree_t *next;

	if (tmbr_client_find_by_focus(&focus) < 0 ||
	    tmbr_tree_find_sibling(&next, focus->tree, args->sel) < 0)
		return ENOENT;

	if (tmbr_tree_swap(focus->tree, next) < 0 ||
	    tmbr_desktop_layout(focus->desktop) < 0)
		return EIO;

	return 0;
}

static int tmbr_cmd_desktop_new(TMBR_UNUSED const tmbr_command_args_t *args)
{
	tmbr_desktop_t *desktop;
	if (tmbr_desktop_new(&desktop) < 0 ||
	    tmbr_screen_add_desktop(state.screen, desktop) < 0)
		return EIO;
	return tmbr_screen_focus_desktop(state.screen, desktop);
}

static int tmbr_cmd_desktop_kill(TMBR_UNUSED const tmbr_command_args_t *args)
{
	tmbr_desktop_t *desktop = state.screen->focus;
	if (desktop->clients)
		return EEXIST;
	if (!desktop->prev && !desktop->next)
		return ENOENT;
	if (tmbr_screen_remove_desktop(state.screen, state.screen->focus) < 0)
		return EIO;
	tmbr_desktop_free(desktop);
	return 0;
}

static int tmbr_cmd_desktop_focus(const tmbr_command_args_t *args)
{
	tmbr_desktop_t *sibling;
	if (tmbr_desktop_find_sibling(&sibling, state.screen->focus, args->sel) < 0)
		return ENOENT;
	if (tmbr_screen_focus_desktop(state.screen, sibling) < 0)
		return EIO;
	return 0;
}

static int tmbr_cmd_desktop_swap(const tmbr_command_args_t *args)
{
	tmbr_desktop_t *sibling;
	if (tmbr_desktop_find_sibling(&sibling, state.screen->focus, args->sel) < 0)
		return ENOENT;
	if (tmbr_desktop_swap(state.screen->focus, sibling) < 0)
		return EIO;
	return 0;
}

static int tmbr_cmd_screen_focus(const tmbr_command_args_t *args)
{
	tmbr_screen_t *sibling;
	if (tmbr_screen_find_sibling(&sibling, state.screen, args->sel) < 0)
		return ENOENT;
	if (tmbr_screen_focus(sibling) < 0)
		return EIO;
	return 0;
}

static int tmbr_cmd_tree_rotate(TMBR_UNUSED const tmbr_command_args_t *args)
{
	tmbr_client_t *focus;
	tmbr_tree_t *p;

	if (tmbr_client_find_by_focus(&focus) < 0 ||
	    (p = focus->tree->parent) == NULL)
		return ENOENT;

	if (p->split == TMBR_SPLIT_HORIZONTAL) {
		tmbr_tree_t *l = p->left;
		p->left = p->right;
		p->right = l;
	}
	p->split ^= 1;

	if (tmbr_desktop_layout(focus->desktop) < 0)
		return EIO;

	return 0;
}

static int tmbr_cmd_state_subscribe(int fd)
{
	unsigned i;
	for (i = 0; i < ARRAY_SIZE(state.subfds); i++) {
		if (state.subfds[i] >= 0)
			continue;
		state.subfds[i] = dup(fd);
		return 0;
	}
	return ENOSPC;
}

static void tmbr_handle_command(int fd)
{
	tmbr_command_args_t args;
	tmbr_command_t command;
	tmbr_pkt_t pkt;
	const char *argv[10];
	int error, argc;

	if (tmbr_ctrl_read(fd, &pkt) < 0 || pkt.type != TMBR_PKT_COMMAND)
		return;
	tmbr_notify("command: %s", pkt.message);

	if ((argv[0] = strtok(pkt.message, " ")) == NULL)
		return;
	for (argc = 1; argc < (int) ARRAY_SIZE(argv); argc++)
		if ((argv[argc] = strtok(NULL, " ")) == NULL)
			break;
	if (argc == (int) ARRAY_SIZE(argv))
		return;

	if (tmbr_command_parse(&command, &args, argc, argv) < 0)
		return;

	switch (command) {
		case TMBR_COMMAND_CLIENT_FOCUS: error = tmbr_cmd_client_focus(&args); break;
		case TMBR_COMMAND_CLIENT_FULLSCREEN: error = tmbr_cmd_client_fullscreen(&args); break;
		case TMBR_COMMAND_CLIENT_KILL: error = tmbr_cmd_client_kill(&args); break;
		case TMBR_COMMAND_CLIENT_RESIZE: error = tmbr_cmd_client_resize(&args); break;
		case TMBR_COMMAND_CLIENT_SWAP: error = tmbr_cmd_client_swap(&args); break;
		case TMBR_COMMAND_CLIENT_TO_DESKTOP: error = tmbr_cmd_client_to_desktop(&args); break;
		case TMBR_COMMAND_CLIENT_TO_SCREEN: error = tmbr_cmd_client_to_screen(&args); break;
		case TMBR_COMMAND_DESKTOP_FOCUS: error = tmbr_cmd_desktop_focus(&args); break;
		case TMBR_COMMAND_DESKTOP_KILL: error = tmbr_cmd_desktop_kill(&args); break;
		case TMBR_COMMAND_DESKTOP_NEW: error = tmbr_cmd_desktop_new(&args); break;
		case TMBR_COMMAND_DESKTOP_SWAP: error = tmbr_cmd_desktop_swap(&args); break;
		case TMBR_COMMAND_SCREEN_FOCUS: error = tmbr_cmd_screen_focus(&args); break;
		case TMBR_COMMAND_TREE_ROTATE: error = tmbr_cmd_tree_rotate(&args); break;
		case TMBR_COMMAND_STATE_SUBSCRIBE: error = tmbr_cmd_state_subscribe(fd); break;
	}

	tmbr_ctrl_write(fd, TMBR_PKT_ERROR, "%d", error);
}

static void tmbr_cleanup(TMBR_UNUSED int signal)
{
	if (state.ctrlfd >= 0)
		close(state.ctrlfd);
	unlink(state.ctrl_path);

	tmbr_screens_free(state.screens);
	xcb_destroy_window(state.conn, state.meta);
	xcb_disconnect(state.conn);
}

static int tmbr_setup_atom(xcb_atom_t *out, char *name)
{
	xcb_intern_atom_cookie_t cookie;
	xcb_intern_atom_reply_t *reply;

	cookie = xcb_intern_atom(state.conn, 0, (uint16_t) strlen(name), name);
	if ((reply = xcb_intern_atom_reply(state.conn, cookie, NULL)) == NULL)
		return -1;

	*out = reply->atom;
	free(reply);
	return 0;
}

static int tmbr_setup_x11(void)
{
	uint32_t mask = XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY;
	uint32_t override_redirect = 1;
	xcb_connection_t *conn;
	xcb_screen_t *screen;

	if ((state.conn = conn = xcb_connect(NULL, NULL)) == NULL ||
	    xcb_connection_has_error(conn) != 0)
		die("Unable to connect to X server");

	if ((screen = xcb_setup_roots_iterator(xcb_get_setup(conn)).data) == NULL)
		die("Unable to get root screen");

	state.root = screen->root;

	state.meta = xcb_generate_id(conn);
	xcb_create_window(conn, XCB_COPY_FROM_PARENT, state.meta, state.root, -1, -1, 1, 1, 0, XCB_WINDOW_CLASS_INPUT_ONLY, XCB_COPY_FROM_PARENT, XCB_NONE, NULL);
	xcb_change_window_attributes(conn, state.meta, XCB_CW_OVERRIDE_REDIRECT, &override_redirect);
	xcb_map_window(conn, state.meta);

	if ((state.randr = xcb_get_extension_data(state.conn, &xcb_randr_id))->present)
		xcb_randr_select_input(state.conn, screen->root, XCB_RANDR_NOTIFY_MASK_SCREEN_CHANGE);

	if (xcb_request_check(conn, xcb_change_window_attributes_checked(conn, screen->root,
									 XCB_CW_EVENT_MASK, &mask)) != NULL)
		die("Another window manager is running already.");

	if (tmbr_setup_atom(&state.atoms.wm_delete_window, "WM_DELETE_WINDOW") < 0 ||
	    tmbr_setup_atom(&state.atoms.wm_take_focus, "WM_TAKE_FOCUS") < 0 ||
	    tmbr_setup_atom(&state.atoms.wm_protocols, "WM_PROTOCOLS") < 0 ||
	    tmbr_setup_atom(&state.atoms.wm_state, "WM_STATE") < 0 ||
	    tmbr_setup_atom(&state.atoms.net_supported, "_NET_SUPPORTED") < 0 ||
	    tmbr_setup_atom(&state.atoms.net_wm_state, "_NET_WM_STATE") < 0 ||
	    tmbr_setup_atom(&state.atoms.net_wm_state_fullscreen, "_NET_WM_STATE_FULLSCREEN") < 0)
		die("Unable to setup atoms");

	xcb_change_property(state.conn, XCB_PROP_MODE_REPLACE, screen->root, state.atoms.net_supported,
			    XCB_ATOM_ATOM, 32, 2, &state.atoms.net_wm_state);

	if (tmbr_screens_update(screen) < 0 ||
	    tmbr_screen_manage_windows(state.screens) < 0 ||
	    tmbr_screen_focus(state.screens) < 0 ||
	    tmbr_desktop_layout(state.screens->focus) < 0)
		die("Unable to set up initial screens");

	return 0;
}

static int tmbr_setup(void)
{
	if (tmbr_setup_x11() < 0)
		die("Unable to setup X server");
	if ((state.ctrlfd = tmbr_ctrl_connect(&state.ctrl_path, 1)) < 0)
		die("Unable to setup control socket");

	signal(SIGPIPE, SIG_IGN);
	signal(SIGINT, tmbr_cleanup);
	signal(SIGHUP, tmbr_cleanup);
	signal(SIGTERM, tmbr_cleanup);
	signal(SIGCHLD, tmbr_cleanup);

	return 0;
}

int tmbr_wm(void)
{
	xcb_generic_event_t *ev;
	struct pollfd fds[2];

	if (tmbr_setup() < 0)
		die("Unable to setup timber");

	fds[0].fd = xcb_get_file_descriptor(state.conn);
	fds[0].events = POLLIN;
	fds[1].fd = state.ctrlfd;
	fds[1].events = POLLIN;

	tmbr_notify("status: running");
	while (xcb_flush(state.conn) > 0) {
		if (poll(fds, 2, -1) < 0)
			die("timber: unable to poll for events");

		if (fds[1].revents & POLLIN) {
			int cfd = accept(fds[1].fd, NULL, NULL);
			if (cfd < 0)
				continue;
			tmbr_handle_command(cfd);
			close(cfd);
		}

		while ((ev = xcb_poll_for_event(state.conn)) != NULL) {
			if (!state.ignored_events ||
			    XCB_EVENT_RESPONSE_TYPE(ev) != state.ignored_events)
				tmbr_handle_event(ev);
			free(ev);
		}

		state.ignored_events = 0;
	}
	tmbr_notify("status: shutdown");

	return 0;
}
