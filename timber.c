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
#include <xcb/xcb_icccm.h>
#include <xcb/randr.h>
#undef inline

#define TMBR_UNUSED __attribute__((unused))

#define TMBR_ARG_NONE      { 0, 0, 0 }
#define TMBR_ARG_SEL(s)    { s, 0, 0 }
#define TMBR_ARG_DIR(d, i) { 0, d, i }

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
	tmbr_select_t sel;
	tmbr_dir_t dir;
	int i;
};

struct tmbr_client {
	tmbr_desktop_t *desktop;
	tmbr_tree_t *tree;
	xcb_window_t window;
	uint16_t w, h;
	int16_t x, y;
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
	uint8_t fullscreen;
};

struct tmbr_screen {
	tmbr_screen_t *next;
	tmbr_desktop_t *desktops;
	tmbr_desktop_t *focus;
	xcb_randr_output_t output;
	xcb_window_t root;
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

static void tmbr_cmd_client_kill(const tmbr_command_args_t *args);
static void tmbr_cmd_client_focus(const tmbr_command_args_t *args);
static void tmbr_cmd_client_move(const tmbr_command_args_t *args);
static void tmbr_cmd_client_resize(const tmbr_command_args_t *args);
static void tmbr_cmd_client_send(const tmbr_command_args_t *args);
static void tmbr_cmd_client_swap(const tmbr_command_args_t *args);
static void tmbr_cmd_desktop_new(const tmbr_command_args_t *args);
static void tmbr_cmd_desktop_kill(const tmbr_command_args_t *args);
static void tmbr_cmd_desktop_focus(const tmbr_command_args_t *args);
static void tmbr_cmd_screen_focus(const tmbr_command_args_t *args);
static void tmbr_cmd_tree_rotate(const tmbr_command_args_t *args);

#include "config.h"

static void tmbr_handle_events(uint8_t ignored_events);

static struct {
	tmbr_screen_t *screens;
	tmbr_screen_t *screen;
	xcb_connection_t *conn;
	const xcb_query_extension_reply_t *randr;
	xcb_ewmh_connection_t ewmh;
	struct {
		xcb_atom_t wm_delete_window;
		xcb_atom_t wm_protocols;
		xcb_atom_t net_wm_state;
		xcb_atom_t net_wm_state_fullscreen;
	} atoms;
	int fifofd;
} state = { NULL, NULL, NULL, NULL, { 0 }, { 0 }, -1 };

static void __attribute__((noreturn, format(printf, 1, 2))) die(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);

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
		} else if (t != tmbr_tree_get_child(t->parent, upwards)) {
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
	xcb_change_window_attributes(state.conn, client->window, XCB_CW_BORDER_PIXEL, &color);
	return 0;
}

static int tmbr_client_focus(tmbr_client_t *client)
{
	if (!client)
		return 0;
	tmbr_client_draw_border(client, TMBR_COLOR_ACTIVE);
	xcb_set_input_focus(state.conn, XCB_INPUT_FOCUS_PARENT, client->window, XCB_CURRENT_TIME);
	return 0;
}

static int tmbr_client_unfocus(tmbr_client_t *client)
{
	if (!client)
		return 0;
	tmbr_client_draw_border(client, TMBR_COLOR_INACTIVE);
	xcb_set_input_focus(state.conn, XCB_INPUT_FOCUS_PARENT, client->desktop->screen->root, XCB_CURRENT_TIME);
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

	cookie = xcb_change_window_attributes_checked(state.conn, window, XCB_CW_EVENT_MASK, values);
	if ((xcb_request_check(state.conn, cookie)) != NULL)
		die("Could not subscribe to window events");

	*out = client;
	return 0;
}

static void tmbr_client_free(tmbr_client_t *client)
{
	free(client);
}

static int tmbr_client_send_message(tmbr_client_t *client, xcb_atom_t value)
{
	xcb_icccm_get_wm_protocols_reply_t protos;
	xcb_client_message_event_t msg = { 0 };
	size_t i;

	if (xcb_icccm_get_wm_protocols_reply(state.conn,
					     xcb_icccm_get_wm_protocols(state.conn,
									client->window,
									state.atoms.wm_protocols),
									&protos, NULL) != 1)
		return -1;
	for (i = 0; i < protos.atoms_len; i++)
		if (protos.atoms[i] == value)
			break;
	xcb_icccm_get_wm_protocols_reply_wipe(&protos);

	if (i == protos.atoms_len)
		return -1;

	msg.response_type = XCB_CLIENT_MESSAGE;
	msg.window = client->window;
	msg.type = state.atoms.wm_protocols;
	msg.format = 32;
	msg.data.data32[0] = value;
	msg.data.data32[1] = XCB_CURRENT_TIME;

	xcb_send_event(state.conn, 0, client->window, XCB_EVENT_MASK_NO_EVENT, (char *) &msg);

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
	uint32_t values[] = { XCB_STACK_MODE_ABOVE };
	xcb_ewmh_set_wm_state(&state.ewmh, client->window, fs, &state.atoms.net_wm_state_fullscreen);
	xcb_configure_window(state.conn, client->window, XCB_CONFIG_WINDOW_STACK_MODE, values);
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
	if ((*out = calloc(1, sizeof(**out))) == NULL)
		die("Unable to allocate desktop");
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
	tmbr_handle_events(XCB_ENTER_NOTIFY);
	return 0;
}

static int tmbr_desktop_hide(tmbr_desktop_t *d)
{
	tmbr_tree_t *it, *t;
	tmbr_tree_foreach_leaf(d->clients, it, t)
		tmbr_client_hide(t->client);
	return 0;
}

static int tmbr_desktop_focus_client(tmbr_desktop_t *desktop, tmbr_client_t *client, int inputfocus)
{
	if (inputfocus && (tmbr_client_unfocus(desktop->focus) < 0 || tmbr_client_focus(client) < 0))
		return -1;
	if (desktop->focus == client)
		return 0;

	if (desktop->fullscreen)
		tmbr_client_set_fullscreen(desktop->focus, 0);
	desktop->fullscreen = 0;
	desktop->focus = client;

	return tmbr_desktop_layout(desktop);
}

static int tmbr_desktop_focus(tmbr_desktop_t *desktop)
{
	return tmbr_desktop_focus_client(desktop, desktop->focus, 1);
}

static int tmbr_desktop_unfocus(tmbr_desktop_t *desktop)
{
	if (!desktop)
		return 0;
	return tmbr_client_unfocus(desktop->focus);
}

static int tmbr_desktop_set_fullscreen(tmbr_desktop_t *desktop, tmbr_client_t *client, uint8_t fs)
{
	desktop->fullscreen = fs;
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
	if (tmbr_tree_insert(desktop->focus ? &desktop->focus->tree : &desktop->clients, client) < 0)
		die("Unable to insert client into tree");
	client->desktop = desktop;
	return tmbr_desktop_layout(desktop);
}

static int tmbr_desktop_remove_client(tmbr_desktop_t *desktop, tmbr_client_t *client)
{
	if (desktop->focus == client) {
		tmbr_tree_t *sibling;
		if (tmbr_tree_find_sibling(&sibling, client->tree, TMBR_SELECT_NEAREST) < 0)
			sibling = NULL;
		tmbr_desktop_focus_client(desktop, sibling ? sibling->client : NULL,
					  desktop->screen == state.screen && desktop->screen->focus == desktop);
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
	    tmbr_desktop_focus(screen->focus) < 0)
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
	    tmbr_desktop_focus(desktop) < 0)
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

	if ((tree = xcb_query_tree_reply(state.conn, xcb_query_tree(state.conn, screen->root), NULL)) == NULL)
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
		    tmbr_desktop_focus_client(screen->focus, client, 1) < 0)
			die("Unable to adopt client");
next:
		free(attrs);
	}

	free(tree);

	return 0;
}

static int tmbr_screen_manage(xcb_randr_output_t output, xcb_window_t root, int16_t x, int16_t y, uint16_t width, uint16_t height)
{
	tmbr_desktop_t *d;
	tmbr_screen_t *s;

	if (tmbr_screen_find_by_output(&s, output) < 0) {
		if ((s = calloc(1, sizeof(*s))) == NULL)
			die("Cannot allocate screen");
		if (tmbr_desktop_new(&d) < 0 || tmbr_screen_add_desktop(s, d) < 0)
			die("Cannot set up desktop");
		s->output = output;
		s->root = root;
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

			tmbr_screen_manage(outputs[i], screen->root, crtc->x, crtc->y, crtc->width, crtc->height);
next:
			free(output);
			free(crtc);
		}

		free(screens);
		return 0;
	} else {
		return tmbr_screen_manage(0, screen->root, 0, 0, screen->width_in_pixels, screen->height_in_pixels);
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

static void tmbr_handle_focus_in(xcb_focus_in_event_t *ev)
{
	tmbr_client_t *client;
	if (tmbr_client_find_by_focus(&client) == 0 && client->window != ev->event)
		tmbr_desktop_focus_client(client->desktop, client, 1);
}

static void tmbr_handle_enter_notify(xcb_enter_notify_event_t *ev)
{
	tmbr_client_t *client;

	if (ev->mode != XCB_NOTIFY_MODE_NORMAL ||
	    tmbr_client_find_by_window(&client, ev->event) < 0 ||
	    tmbr_desktop_focus_client(client->desktop, client, 1) < 0 ||
	    tmbr_screen_focus(client->desktop->screen) < 0)
		return;
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
	    tmbr_desktop_add_client(state.screen->focus, client) < 0)
		die("Unable to manage client");

	xcb_map_window(state.conn, ev->window);
	tmbr_desktop_focus_client(state.screen->focus, client, 1);
}

static void tmbr_handle_destroy_notify(xcb_destroy_notify_event_t *ev)
{
	tmbr_client_t *client;
	if (tmbr_client_find_by_window(&client, ev->window) < 0)
		return;
	if (tmbr_desktop_remove_client(client->desktop, client) < 0)
		die("Unable to remove client from tree");
	tmbr_client_free(client);
}

static void tmbr_handle_client_message(xcb_client_message_event_t * ev)
{
	tmbr_client_t *client;

	if (ev->type != state.atoms.net_wm_state || tmbr_client_find_by_window(&client, ev->window) < 0)
		return;
	if (ev->data.data32[1] == state.atoms.net_wm_state_fullscreen)
		tmbr_desktop_set_fullscreen(client->desktop, client, ev->data.data32[0] == XCB_EWMH_WM_STATE_ADD);
}

static void tmbr_handle_error(xcb_request_error_t *ev)
{
	if (ev->error_code != 3 /* BAD_WINDOW */)
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
}

static void tmbr_handle_event(xcb_generic_event_t *ev)
{
	uint8_t type = XCB_EVENT_RESPONSE_TYPE(ev);
	if (type == XCB_FOCUS_IN)
		tmbr_handle_focus_in((xcb_focus_in_event_t *) ev);
	else if (type == XCB_ENTER_NOTIFY)
		tmbr_handle_enter_notify((xcb_enter_notify_event_t *) ev);
	else if (type == XCB_MAP_REQUEST)
		tmbr_handle_map_request((xcb_map_request_event_t *) ev);
	else if (type == XCB_DESTROY_NOTIFY)
		tmbr_handle_destroy_notify((xcb_destroy_notify_event_t *) ev);
	else if (type == XCB_CLIENT_MESSAGE)
		tmbr_handle_client_message((xcb_client_message_event_t *) ev);
	else if (state.randr->present && type == state.randr->first_event + XCB_RANDR_SCREEN_CHANGE_NOTIFY)
		tmbr_handle_screen_change_notify();
	else if (!type)
		tmbr_handle_error((xcb_request_error_t *) ev);
}

static void tmbr_handle_events(uint8_t ignored_events)
{
	xcb_generic_event_t *ev;
	while ((ev = xcb_poll_for_event(state.conn)) != NULL) {
		if (!ignored_events || XCB_EVENT_RESPONSE_TYPE(ev) != ignored_events)
			tmbr_handle_event(ev);
		free(ev);
	}
}

static void tmbr_cmd_client_kill(TMBR_UNUSED const tmbr_command_args_t *args)
{
	tmbr_client_t *focus;

	if (tmbr_client_find_by_focus(&focus) < 0)
		return;

	tmbr_client_kill(focus);
}

static void tmbr_cmd_client_focus(const tmbr_command_args_t *args)
{
	tmbr_client_t *focus;
	tmbr_tree_t *next;

	if (tmbr_client_find_by_focus(&focus) < 0 ||
	    tmbr_tree_find_sibling(&next, focus->tree, args->sel) < 0)
		return;

	tmbr_desktop_focus_client(focus->desktop, next->client, 1);
}

static void tmbr_cmd_client_move(const tmbr_command_args_t *args)
{
	tmbr_desktop_t *target;
	tmbr_client_t *focus;

	if (tmbr_client_find_by_focus(&focus) < 0 ||
	    tmbr_desktop_find_sibling(&target, focus->desktop, args->sel) < 0)
		return;

	tmbr_desktop_remove_client(focus->desktop, focus);
	tmbr_client_hide(focus);
	tmbr_desktop_add_client(target, focus);
	tmbr_desktop_focus_client(target, focus, 0);
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

static void tmbr_cmd_client_send(const tmbr_command_args_t *args)
{
	tmbr_screen_t *screen;
	tmbr_client_t *client;

	if (tmbr_client_find_by_focus(&client) < 0 ||
	    tmbr_screen_find_sibling(&screen, client->desktop->screen, args->sel) < 0)
		return;

	tmbr_desktop_remove_client(client->desktop, client);
	tmbr_desktop_add_client(screen->focus, client);
	tmbr_desktop_focus_client(screen->focus, client, 0);
}

static void tmbr_cmd_client_swap(const tmbr_command_args_t *args)
{
	tmbr_client_t *focus;
	tmbr_tree_t *next;

	if (tmbr_client_find_by_focus(&focus) < 0 ||
	    tmbr_tree_find_sibling(&next, focus->tree, args->sel) < 0 ||
	    tmbr_tree_swap(focus->tree, next) < 0)
		return;

	tmbr_desktop_layout(focus->desktop);
}

static void tmbr_cmd_desktop_new(TMBR_UNUSED const tmbr_command_args_t *args)
{
	tmbr_desktop_t *desktop;

	if (tmbr_desktop_new(&desktop) < 0 ||
	    tmbr_screen_add_desktop(state.screen, desktop) < 0)
		return;

	tmbr_screen_focus_desktop(state.screen, desktop);
}

static void tmbr_cmd_desktop_kill(TMBR_UNUSED const tmbr_command_args_t *args)
{
	tmbr_desktop_t *desktop;

	desktop = state.screen->focus;
	if (tmbr_screen_remove_desktop(state.screen, state.screen->focus) < 0)
		return;
	tmbr_desktop_free(desktop);
}

static void tmbr_cmd_desktop_focus(const tmbr_command_args_t *args)
{
	tmbr_desktop_t *sibling;

	if (tmbr_desktop_find_sibling(&sibling, state.screen->focus, args->sel) < 0)
		return;

	tmbr_screen_focus_desktop(state.screen, sibling);
}

static void tmbr_cmd_screen_focus(const tmbr_command_args_t *args)
{
	tmbr_screen_t *sibling;

	if (tmbr_screen_find_sibling(&sibling, state.screen, args->sel) < 0)
		return;

	tmbr_screen_focus(sibling);
}

static void tmbr_cmd_tree_rotate(TMBR_UNUSED const tmbr_command_args_t *args)
{
	tmbr_client_t *focus;

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

	if ((n = read(fd, cmd, sizeof(cmd) - 1)) <= 0) {
		if (errno == EAGAIN || errno == EINTR)
			return;
		die("Unable to read from control pipe: %s", strerror(errno));
	}
	n = (cmd[n - 1] == '\n') ? (n - 1) : n;

	for (i = 0; i < sizeof(cmds) / sizeof(*cmds); i++) {
		if (strncmp(cmds[i].cmd, cmd, (size_t) n))
			continue;
		cmds[i].fn(&cmds[i].args);
	}
}

static void tmbr_cleanup(TMBR_UNUSED int signal)
{
	if (state.fifofd >= 0)
		close(state.fifofd);
	unlink(TMBR_CTRL_PATH);

	tmbr_screens_free(state.screens);
	xcb_ewmh_connection_wipe(&state.ewmh);
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

static int tmbr_setup_x11(xcb_connection_t *conn)
{
	uint32_t mask = XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY;
	xcb_atom_t netatoms[2];
	xcb_screen_t *screen;

	if ((screen = xcb_setup_roots_iterator(xcb_get_setup(conn)).data) == NULL)
		die("Unable to get root screen");

	if ((state.randr = xcb_get_extension_data(state.conn, &xcb_randr_id))->present)
		xcb_randr_select_input(state.conn, screen->root, XCB_RANDR_NOTIFY_MASK_SCREEN_CHANGE);

	if (xcb_request_check(conn, xcb_change_window_attributes_checked(conn, screen->root,
									 XCB_CW_EVENT_MASK, &mask)) != NULL)
		die("Another window manager is running already.");

	if (tmbr_setup_atom(&state.atoms.wm_delete_window, "WM_DELETE_WINDOW") < 0 ||
	    tmbr_setup_atom(&state.atoms.wm_protocols, "WM_PROTOCOLS") < 0 ||
	    tmbr_setup_atom(&state.atoms.net_wm_state, "_NET_WM_STATE") < 0 ||
	    tmbr_setup_atom(&state.atoms.net_wm_state_fullscreen, "_NET_WM_STATE_FULLSCREEN") < 0)
		die("Unable to setup atoms");

	if (xcb_ewmh_init_atoms_replies(&state.ewmh, xcb_ewmh_init_atoms(conn, &state.ewmh), NULL) == 0)
		die("Unable to initialize EWMH atoms");
	netatoms[0] = state.atoms.net_wm_state;
	netatoms[1] = state.atoms.net_wm_state_fullscreen;
	xcb_ewmh_set_supported(&state.ewmh, 0, sizeof(netatoms) / sizeof(*netatoms), netatoms);

	if (tmbr_screens_update(screen) < 0 ||
	    tmbr_screen_manage_windows(state.screens) < 0 ||
	    tmbr_screen_focus(state.screens) < 0 ||
	    tmbr_desktop_layout(state.screens->focus) < 0)
		die("Unable to set up initial screens");

	return 0;
}

static int tmbr_setup(void)
{
	if ((mkdir(TMBR_CTRL_DIR, 0700) < 0 && errno != EEXIST) ||
	    (mkfifo(TMBR_CTRL_PATH, 0600) < 0 && errno != EEXIST))
		die("Unable to create fifo");

	if ((state.fifofd = open(TMBR_CTRL_PATH, O_RDWR|O_NONBLOCK)) < 0)
		die("Unable to open fifo");

	if ((state.conn = xcb_connect(NULL, NULL)) == NULL ||
	    xcb_connection_has_error(state.conn) != 0)
		die("Unable to connect to X server");

	if (tmbr_setup_x11(state.conn) < 0)
		die("Unable to setup X server");

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

	fds[0].fd = xcb_get_file_descriptor(state.conn);
	fds[0].events = POLLIN;
	fds[1].fd = state.fifofd;
	fds[1].events = POLLIN;

	while (xcb_flush(state.conn) > 0) {
		if (poll(fds, 2, -1) < 0)
			die("timber: unable to poll for events");
		if (fds[0].revents & POLLIN)
			tmbr_handle_events(0);
		if (fds[1].revents & POLLIN)
			tmbr_handle_command(fds[1].fd);
	}

	return 0;
}

/* vim: set tabstop=8 noexpandtab : */
