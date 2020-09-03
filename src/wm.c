/*
 * Copyright (C) Patrick Steinhardt, 2019-2020
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

#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <wlr/backend.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_matrix.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_server_decoration.h>
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>

#include "common.h"
#include "config.h"
#include "wm.h"

#define TMBR_UNUSED __attribute__((unused))

typedef struct tmbr_binding tmbr_binding_t;
typedef struct tmbr_client tmbr_client_t;
typedef struct tmbr_keyboard tmbr_keyboard_t;
typedef struct tmbr_desktop tmbr_desktop_t;
typedef struct tmbr_screen tmbr_screen_t;
typedef struct tmbr_server tmbr_server_t;
typedef struct tmbr_tree tmbr_tree_t;

typedef enum {
	TMBR_SPLIT_VERTICAL,
	TMBR_SPLIT_HORIZONTAL
} tmbr_split_t;

struct tmbr_binding {
	struct wl_list link;

	uint32_t modifiers;
	xkb_keysym_t keycode;
	char *command;
};

struct tmbr_keyboard {
	tmbr_server_t *server;
	struct wlr_input_device *device;

	struct wl_listener key;
	struct wl_listener modifiers;
};

struct tmbr_client {
	tmbr_server_t *server;
	tmbr_desktop_t *desktop;
	tmbr_tree_t *tree;
	struct wlr_xdg_surface *surface;
	int h, w, x, y, border;

	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener destroy;
	struct wl_listener commit;
};

struct tmbr_tree {
	tmbr_tree_t *parent;
	tmbr_tree_t *left;
	tmbr_tree_t *right;
	tmbr_client_t *client;
	tmbr_split_t split;
	uint8_t ratio;
};

struct tmbr_desktop {
	struct wl_list link;
	tmbr_screen_t *screen;
	tmbr_tree_t *clients;
	tmbr_client_t *focus;
	char fullscreen;
};

struct tmbr_screen {
	struct wl_list link;
	tmbr_server_t *server;

	struct wlr_output *output;
	struct wl_list desktops;
	tmbr_desktop_t *focus;

	struct timespec render_time;
	char damaged;

	struct wl_listener destroy;
	struct wl_listener frame;
	struct wl_listener mode;
};

struct tmbr_server {
	struct wl_display *display;
	struct wlr_backend *backend;
	struct wlr_compositor *compositor;
	struct wlr_server_decoration_manager *decoration;
	struct wlr_output_layout *output_layout;
	struct wlr_seat *seat;
	struct wlr_xdg_shell *xdg_shell;
	struct wlr_cursor *cursor;
	struct wlr_xcursor_manager *xcursor;

	struct wl_listener new_input;
	struct wl_listener new_output;
	struct wl_listener new_surface;

	struct wl_listener cursor_axis;
	struct wl_listener cursor_button;
	struct wl_listener cursor_motion;
	struct wl_listener cursor_motion_absolute;
	struct wl_listener cursor_frame;
	struct wl_listener request_set_cursor;
	struct wl_listener request_set_selection;
	struct wl_listener request_set_primary_selection;

	int ctrlfd;
	int subfds[16];

	struct wl_list bindings;
	tmbr_keyboard_t *keyboards;
	struct wl_list screens;
	tmbr_screen_t *screen;
};

static tmbr_server_t server;

static void tmbr_spawn(const char *path, char * const argv[])
{
	pid_t pid;
	int i;

	if ((pid = fork()) < 0) {
		die("Could not fork: %s", strerror(errno));
	} else if (pid == 0) {
		close(0);
		for (i = 3; i < 1024; i++)
			close(i);
		if (execv(path, argv) < 0)
			die("Could not execute '%s': %s", path, strerror(errno));
	}
}

static struct wl_list *tmbr_list_get(struct wl_list *head, struct wl_list *link, tmbr_select_t which)
{
	struct wl_list *sibling = (which == TMBR_SELECT_PREV) ? link->prev : link->next;
	if (sibling == head)
		sibling = (which == TMBR_SELECT_PREV) ? head->prev : head->next;
	return (sibling == link) ? NULL : sibling;
}

static void tmbr_register(struct wl_signal *signal, struct wl_listener *listener, wl_notify_func_t callback)
{
	listener->notify = callback;
	wl_signal_add(signal, listener);
}

static __attribute__((format(printf, 2, 3))) void tmbr_notify(tmbr_server_t *server, const char *fmt, ...)
{
	char buf[TMBR_PKT_MESSAGELEN];
	unsigned i;
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	for (i = 0; i < ARRAY_SIZE(server->subfds); i++) {
		if (server->subfds[i] <= 0)
			continue;
		if (tmbr_ctrl_write_data(server->subfds[i], "%s", buf) < 0) {
			close(server->subfds[i]);
			server->subfds[i] = 0;
		}
	}
}

static void tmbr_client_on_destroy(struct wl_listener *listener, TMBR_UNUSED void *payload)
{
	tmbr_client_t *client = wl_container_of(listener, client, destroy);
	free(client);
}

static void tmbr_client_on_commit(struct wl_listener *listener, TMBR_UNUSED void *payload)
{
	tmbr_client_t *client = wl_container_of(listener, client, commit);
	if (client->desktop)
		client->desktop->screen->damaged = true;
}

static int tmbr_client_new(tmbr_client_t **out, tmbr_server_t *server, struct wlr_xdg_surface *surface)
{
	tmbr_client_t *client;

	client = tmbr_alloc(sizeof(*client), "Could not allocate client");
	client->server = server;
	client->surface = surface;

	tmbr_register(&surface->events.destroy, &client->destroy, tmbr_client_on_destroy);
	tmbr_register(&surface->surface->events.commit, &client->commit, tmbr_client_on_commit);

	*out = client;
	return 0;
}

static int tmbr_client_kill(tmbr_client_t *client)
{
	wlr_xdg_toplevel_send_close(client->surface);
	return 0;
}

static void tmbr_client_render_surface(struct wlr_surface *surface, int sx, int sy, void *payload)
{
	tmbr_client_t *client = payload;
	struct wlr_output *output = client->desktop->screen->output;
	struct wlr_texture *texture;
	struct wlr_box box;
	float matrix[9];

	if ((texture = wlr_surface_get_texture(surface)) == NULL)
		return;

	box.x = (client->x + client->border + sx) * output->scale;
	box.y = (client->y + client->border + sy) * output->scale;
	box.width = surface->current.width * output->scale;
	box.height = surface->current.height * output->scale;

	wlr_matrix_project_box(matrix, &box, wlr_output_transform_invert(surface->current.transform), 0, output->transform_matrix);
	wlr_render_texture_with_matrix(wlr_backend_get_renderer(output->backend), texture, matrix, 1);
	wlr_surface_send_frame_done(surface, &client->desktop->screen->render_time);
}

static void tmbr_client_render(tmbr_client_t *c)
{
	if (c->border) {
		float *color = (c->desktop->focus == c) ? (float[4])TMBR_COLOR_ACTIVE : (float[4])TMBR_COLOR_INACTIVE;
		struct wlr_output *output = c->desktop->screen->output;
		struct wlr_box borders[4];
		size_t i;

		borders[0] = (struct wlr_box){ c->x, c->y, c->w, c->border };
		borders[1] = (struct wlr_box){ c->x, c->y, c->border, c->h };
		borders[2] = (struct wlr_box){ c->x + c->w - c->border, c->y, c->border, c->h };
		borders[3] = (struct wlr_box){ c->x, c->y + c->h - c->border, c->w, c->border };

		for (i = 0; i < ARRAY_SIZE(borders); i++)
			wlr_render_rect(wlr_backend_get_renderer(output->backend), &borders[i], color, output->transform_matrix);
	}
	wlr_xdg_surface_for_each_surface(c->surface, tmbr_client_render_surface, c);
}

static void tmbr_client_set_box(tmbr_client_t *client, int x, int y, int w, int h, int border)
{
	if (client->w != w || client->h != h || client->border != border)
		wlr_xdg_toplevel_set_size(client->surface, w - 2 * border, h - 2 * border);
	client->w = w; client->h = h; client->x = x; client->y = y; client->border = border;
}

static void tmbr_tree_recalculate(tmbr_tree_t *tree, int x, int y, int w, int h)
{
	int xoff, yoff, lw, rw, lh, rh;

	if (!tree)
		return;

	if (tree->client) {
		tmbr_client_set_box(tree->client, x, y, w, h, TMBR_BORDER_WIDTH);
		return;
	}

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

	tmbr_tree_recalculate(tree->left, x, y, lw, lh);
	tmbr_tree_recalculate(tree->right, x + xoff, y + yoff, rw, rh);
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

#define tmbr_tree_foreach_leaf(t, i, n) \
	i = NULL, n = t; \
	while ((!i && n && n->client && (i = n)) || (tmbr_tree_find_sibling(&n, n, TMBR_SELECT_NEXT) == 0 && ((!i && (i = n)) || i != n)))

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

static int tmbr_desktop_new(tmbr_desktop_t **out)
{
	*out = tmbr_alloc(sizeof(**out), "Could not allocate desktop");
	return 0;
}

static void tmbr_desktop_free(tmbr_desktop_t *desktop)
{
	free(desktop);
}

static int tmbr_desktop_find_sibling(tmbr_desktop_t **out, tmbr_desktop_t *desktop, tmbr_select_t which)
{
	struct wl_list *sibling;
	if ((sibling = tmbr_list_get(&desktop->screen->desktops, &desktop->link, which)) == NULL)
		return -1;
	*out = wl_container_of(sibling, *out, link);
	return 0;
}

static void tmbr_desktop_focus(tmbr_desktop_t *desktop, tmbr_client_t *client, int inputfocus)
{
	struct wlr_seat *seat = desktop->screen->server->seat;
	struct wlr_surface *current = seat->keyboard_state.focused_surface;

	if (!client) {
		desktop->focus = NULL;
		return;
	}
	if (current == client->surface->surface)
		return;

	if (inputfocus) {
		struct wlr_keyboard *keyboard;
		if (desktop->focus)
			wlr_xdg_toplevel_set_activated(desktop->focus->surface, false);
		wlr_xdg_toplevel_set_activated(client->surface, true);
		if ((keyboard = wlr_seat_get_keyboard(seat)) != NULL)
			wlr_seat_keyboard_notify_enter(seat, client->surface->surface, keyboard->keycodes, keyboard->num_keycodes, &keyboard->modifiers);
		wlr_seat_pointer_notify_enter(seat, client->surface->surface, 0, 0);
	}

	desktop->focus = client;
	desktop->screen->focus = desktop;
	desktop->screen->server->screen = desktop->screen;
	desktop->screen->damaged = true;
	desktop->fullscreen = 0;
}

static void tmbr_desktop_recalculate(tmbr_desktop_t *desktop)
{
	int width, height;
	wlr_output_effective_resolution(desktop->screen->output, &width, &height);
	if (desktop->fullscreen)
		tmbr_client_set_box(desktop->focus, 0, 0, width, height, 0);
	else
		tmbr_tree_recalculate(desktop->clients, 0, 0, width, height);
	desktop->screen->damaged = true;
}

static void tmbr_desktop_add_client(tmbr_desktop_t *desktop, tmbr_client_t *client)
{
	if (tmbr_tree_insert(desktop->focus ? &desktop->focus->tree : &desktop->clients, client) < 0)
		die("Unable to insert client into tree");
	client->desktop = desktop;
	desktop->fullscreen = 0;
	tmbr_desktop_recalculate(desktop);
}

static void tmbr_desktop_remove_client(tmbr_desktop_t *desktop, tmbr_client_t *client)
{
	if (desktop->focus == client) {
		tmbr_tree_t *sibling;
		if (tmbr_tree_find_sibling(&sibling, client->tree, TMBR_SELECT_NEAREST) < 0)
			sibling = NULL;
		tmbr_desktop_focus(desktop, sibling ? sibling->client : NULL, 1);
	}

	if (tmbr_tree_remove(&desktop->clients, client->tree) < 0)
		die("Unable to remove client from tree");

	desktop->fullscreen = 0;
	client->desktop = NULL;
	client->tree = NULL;
	tmbr_desktop_recalculate(desktop);
}

static int tmbr_desktop_swap(tmbr_desktop_t *_a, tmbr_desktop_t *_b)
{
	struct wl_list *a = &_a->link, *b = &_b->link, *pos = b->prev;

	if (_a->screen != _b->screen)
		return -1;

	wl_list_remove(b);
	wl_list_insert(a->prev, b);
	if (pos == a)
		pos = b;
	wl_list_remove(a);
	wl_list_insert(pos, a);

	return 0;
}

static void tmbr_desktop_set_fullscreen(tmbr_desktop_t *desktop, char fs)
{
	desktop->fullscreen = fs;
	tmbr_desktop_recalculate(desktop);
}

static void tmbr_screen_on_destroy(struct wl_listener *listener, TMBR_UNUSED void *payload)
{
	tmbr_screen_t *screen = wl_container_of(listener, screen, destroy);
	free(screen);
}

static void tmbr_screen_on_frame(struct wl_listener *listener, TMBR_UNUSED void *payload)
{
	tmbr_screen_t *screen = wl_container_of(listener, screen, frame);
	struct wlr_renderer *renderer = wlr_backend_get_renderer(screen->output->backend);
	int width, height;

	if (!screen->damaged || !wlr_output_attach_render(screen->output, NULL))
		return;

	clock_gettime(CLOCK_MONOTONIC, &screen->render_time);
	wlr_output_effective_resolution(screen->output, &width, &height);
	wlr_renderer_begin(renderer, width, height);

	wlr_renderer_clear(renderer, (float[4]){0.3, 0.3, 0.3, 1.0});
	if (screen->focus->fullscreen) {
		tmbr_client_render(screen->focus->focus);
	} else {
		tmbr_tree_t *it, *t;
		tmbr_tree_foreach_leaf(screen->focus->clients, it, t)
			tmbr_client_render(t->client);
	}
	wlr_output_render_software_cursors(screen->output, NULL);

	wlr_renderer_end(renderer);
	wlr_output_commit(screen->output);
}

static void tmbr_screen_on_mode(struct wl_listener *listener, TMBR_UNUSED void *payload)
{
	tmbr_screen_t *screen = wl_container_of(listener, screen, mode);
	tmbr_desktop_recalculate(screen->focus);
}

static int tmbr_screen_focus_desktop(tmbr_screen_t *screen, tmbr_desktop_t *desktop)
{
	if (screen->focus == desktop)
		return 0;
	if (desktop->screen != screen)
		return -1;
	tmbr_desktop_focus(desktop, desktop->focus, 1);
	screen->focus = desktop;
	return 0;
}

static int tmbr_screen_remove_desktop(tmbr_screen_t *screen, tmbr_desktop_t *desktop)
{
	if (desktop->clients)
		return -1;

	if (screen->focus == desktop) {
		tmbr_desktop_t *sibling;
		if (tmbr_desktop_find_sibling(&sibling, desktop, TMBR_SELECT_NEXT) < 0 ||
		    tmbr_screen_focus_desktop(screen, sibling) < 0)
			return -1;
	}
	wl_list_remove(&desktop->link);

	return 0;
}

static int tmbr_screen_add_desktop(tmbr_screen_t *screen, tmbr_desktop_t *desktop)
{
	wl_list_insert(&screen->desktops, &desktop->link);
	desktop->screen = screen;
	if (tmbr_screen_focus_desktop(screen, desktop) < 0)
		return EIO;
	return 0;
}

static int tmbr_screen_find_sibling(tmbr_screen_t **out, tmbr_screen_t *screen, tmbr_select_t which)
{
	struct wl_list *sibling;
	if ((sibling = tmbr_list_get(&screen->server->screens, &screen->link, which)) == NULL)
		return -1;
	*out = wl_container_of(sibling, *out, link);
	return 0;
}

static int tmbr_screen_focus(tmbr_screen_t *screen)
{
	if (screen->server->screen == screen)
		return 0;
	tmbr_desktop_focus(screen->focus, screen->focus->focus, 1);
	screen->server->screen = screen;
	return 0;
}

static int tmbr_screen_new(tmbr_screen_t **out, tmbr_server_t *server, struct wlr_output *output)
{
	tmbr_screen_t *screen;
	tmbr_desktop_t *desktop;

	screen = tmbr_alloc(sizeof(*screen), "Could not allocate screen");
	screen->output = output;
	screen->server = server;
	wl_list_init(&screen->desktops);

	if (tmbr_desktop_new(&desktop) < 0 ||
	    tmbr_screen_add_desktop(screen, desktop) < 0)
		die("Could not create desktop for screen");

	tmbr_register(&output->events.destroy, &screen->destroy, tmbr_screen_on_destroy);
	tmbr_register(&output->events.frame, &screen->frame, tmbr_screen_on_frame);
	tmbr_register(&output->events.mode, &screen->mode, tmbr_screen_on_mode);

	*out = screen;
	return 0;
}

static void tmbr_server_on_new_output(struct wl_listener *listener, void *payload)
{
	struct wlr_output *output = payload;
	tmbr_server_t *server = wl_container_of(listener, server, new_output);
	tmbr_screen_t *screen;

	if (!wl_list_empty(&output->modes)) {
		struct wlr_output_mode *mode = wlr_output_preferred_mode(output);
		wlr_output_set_mode(output, mode);
		wlr_output_enable(output, true);
		if (!wlr_output_commit(output))
			return;
	}

	if (tmbr_screen_new(&screen, server, output) < 0)
		die("Could not create new screen");
	wl_list_insert(&server->screens, &screen->link);
	if (!server->screen)
		server->screen = screen;

	wlr_output_layout_add_auto(server->output_layout, output);
}

static void tmbr_server_on_map(struct wl_listener *listener, TMBR_UNUSED void *payload)
{
	tmbr_client_t *client = wl_container_of(listener, client, map);
	tmbr_desktop_add_client(client->server->screen->focus, client);
	tmbr_desktop_focus(client->server->screen->focus, client, 1);
}

static void tmbr_server_on_unmap(struct wl_listener *listener, TMBR_UNUSED void *payload)
{
	tmbr_client_t *client = wl_container_of(listener, client, unmap);
	tmbr_desktop_remove_client(client->desktop, client);
}

static void tmbr_server_on_new_surface(struct wl_listener *listener, void *payload)
{
	tmbr_server_t *server = wl_container_of(listener, server, new_surface);
	struct wlr_xdg_surface *surface = payload;
	tmbr_client_t *client;

	if (surface->role != WLR_XDG_SURFACE_ROLE_TOPLEVEL)
		return;

	if (tmbr_client_new(&client, server, surface) < 0)
		die("Could not create client");
	tmbr_register(&surface->events.map, &client->map, tmbr_server_on_map);
	tmbr_register(&surface->events.unmap, &client->unmap, tmbr_server_on_unmap);
}

static void tmbr_keyboard_on_key(struct wl_listener *listener, void *payload)
{
	tmbr_keyboard_t *keyboard = wl_container_of(listener, keyboard, key);
	uint32_t modifiers = wlr_keyboard_get_modifiers(keyboard->device->keyboard);
	struct wlr_event_keyboard_key *event = payload;
	xkb_keysym_t keysym;
	tmbr_binding_t *binding;

	keysym = xkb_state_key_get_one_sym(keyboard->device->keyboard->xkb_state, event->keycode + 8);
	keysym = xkb_keysym_to_lower(keysym);
	if (event->state != WLR_KEY_PRESSED || keysym == XKB_KEY_NoSymbol)
		goto unhandled;

	wl_list_for_each(binding, &keyboard->server->bindings, link) {
		if (binding->keycode != keysym || binding->modifiers != modifiers)
			continue;
		tmbr_spawn("/bin/sh", (char * const[]){ "/bin/sh", "-c", binding->command, NULL });
		return;
	}

unhandled:
	wlr_seat_set_keyboard(keyboard->server->seat, keyboard->device);
	wlr_seat_keyboard_notify_key(keyboard->server->seat, event->time_msec, event->keycode, event->state);
}

static void tmbr_keyboard_on_modifiers(struct wl_listener *listener, TMBR_UNUSED void *payload)
{
	tmbr_keyboard_t *keyboard = wl_container_of(listener, keyboard, modifiers);
	wlr_seat_set_keyboard(keyboard->server->seat, keyboard->device);
	wlr_seat_keyboard_notify_modifiers(keyboard->server->seat, &keyboard->device->keyboard->modifiers);
}

static int tmbr_keyboard_new(tmbr_keyboard_t **out, tmbr_server_t *server, struct wlr_input_device *device)
{
	struct xkb_rule_names rules = {0};
	struct xkb_context *context;
	struct xkb_keymap *keymap;
	tmbr_keyboard_t *keyboard;

	if ((context = xkb_context_new(XKB_CONTEXT_NO_FLAGS)) == NULL)
		die("Could not create XKB context");
	if ((keymap = xkb_map_new_from_names(context, &rules, XKB_KEYMAP_COMPILE_NO_FLAGS)) == NULL)
		die("Could not create XKB map");

	keyboard = tmbr_alloc(sizeof(*keyboard), "Could not allocate keyboard");
	keyboard->server = server;
	keyboard->device = device;

	wlr_keyboard_set_keymap(device->keyboard, keymap);
	wlr_keyboard_set_repeat_info(device->keyboard, 25, 600);

	tmbr_register(&device->keyboard->events.key, &keyboard->key, tmbr_keyboard_on_key);
	tmbr_register(&device->keyboard->events.modifiers, &keyboard->modifiers, tmbr_keyboard_on_modifiers);

	*out = keyboard;

	xkb_keymap_unref(keymap);
	xkb_context_unref(context);
	return 0;
}

static void tmbr_server_on_new_input(struct wl_listener *listener, void *payload)
{
	tmbr_server_t *server = wl_container_of(listener, server, new_input);
	struct wlr_input_device *device = payload;

	switch (device->type) {
		case WLR_INPUT_DEVICE_POINTER:
			wlr_cursor_attach_input_device(server->cursor, device);
			break;
		case WLR_INPUT_DEVICE_KEYBOARD:
			if (tmbr_keyboard_new(&server->keyboards, server, device) < 0)
				die("Could not create new keyboard");
			break;
		default:
			break;
	}

	wlr_seat_set_capabilities(server->seat, WL_SEAT_CAPABILITY_POINTER|WL_SEAT_CAPABILITY_KEYBOARD);
}

static void tmbr_server_on_cursor_axis(struct wl_listener *listener, void *payload)
{
	tmbr_server_t *server = wl_container_of(listener, server, cursor_axis);
	struct wlr_event_pointer_axis *event = payload;
	wlr_seat_pointer_notify_axis(server->seat, event->time_msec, event->orientation,
				     event->delta, event->delta_discrete, event->source);
}

static void tmbr_server_on_cursor_button(struct wl_listener *listener, void *payload)
{
	tmbr_server_t *server = wl_container_of(listener, server, cursor_button);
	struct wlr_event_pointer_button *event = payload;
	wlr_seat_pointer_notify_button(server->seat, event->time_msec, event->button, event->state);
}

static void tmbr_server_on_cursor_frame(struct wl_listener *listener, TMBR_UNUSED void *payload)
{
	tmbr_server_t *server = wl_container_of(listener, server, cursor_frame);
	wlr_seat_pointer_notify_frame(server->seat);
}

static void tmbr_server_handle_cursor_motion(tmbr_server_t *server, uint32_t time)
{
	double x = server->cursor->x, y = server->cursor->y;
	struct wlr_output *output;
	tmbr_client_t *client = NULL;
	tmbr_screen_t *screen;
	tmbr_tree_t *it, *t;

	if ((output = wlr_output_layout_output_at(server->output_layout, x, y)) == NULL)
		return;

	wl_list_for_each(screen, &server->screens, link)
		if (screen->output == output)
			break;
	if (!screen)
		return;

	if (tmbr_screen_focus(screen) < 0)
		return;
	screen->damaged = true;

	if (screen->focus->fullscreen) {
		client = screen->focus->focus;
	} else {
		tmbr_tree_foreach_leaf(screen->focus->clients, it, t) {
			if (t->client->x > x || t->client->x + t->client->w < x ||
			    t->client->y > y || t->client->y + t->client->h < y)
				continue;
			client = t->client;
			break;
		}
	}

	if (client) {
		struct wlr_surface *surface;
		double sx, sy;

		tmbr_desktop_focus(screen->focus, client, 1);
		if ((surface = wlr_xdg_surface_surface_at(client->surface, x - client->x, y - client->y, &sx, &sy)) != NULL) {
			wlr_seat_pointer_notify_enter(server->seat, surface, sx, sy);
			wlr_seat_pointer_notify_motion(server->seat, time, sx, sy);
		}

		return;
	}

	wlr_xcursor_manager_set_cursor_image(server->xcursor, "left_ptr", server->cursor);
}

static void tmbr_server_on_cursor_motion(struct wl_listener *listener, void *payload)
{
	tmbr_server_t *server = wl_container_of(listener, server, cursor_motion);
	struct wlr_event_pointer_motion *event = payload;
	wlr_cursor_move(server->cursor, event->device, event->delta_x, event->delta_y);
	tmbr_server_handle_cursor_motion(server, event->time_msec);
}

static void tmbr_server_on_cursor_motion_absolute(struct wl_listener *listener, void *payload)
{
	tmbr_server_t *server = wl_container_of(listener, server, cursor_motion_absolute);
	struct wlr_event_pointer_motion_absolute *event = payload;
	wlr_cursor_warp_absolute(server->cursor, event->device, event->x, event->y);
	tmbr_server_handle_cursor_motion(server, event->time_msec);
}

static void tmbr_server_on_request_set_cursor(struct wl_listener *listener, void *payload)
{
	tmbr_server_t *server = wl_container_of(listener, server, request_set_cursor);
	struct wlr_seat_pointer_request_set_cursor_event *event = payload;
	struct wlr_seat_client *focused_client = server->seat->pointer_state.focused_client;
	if (focused_client == event->seat_client)
		wlr_cursor_set_surface(server->cursor, event->surface, event->hotspot_x, event->hotspot_y);
}

static void tmbr_server_on_request_set_selection(struct wl_listener *listener, void *payload)
{
	tmbr_server_t *server = wl_container_of(listener, server, request_set_selection);
	struct wlr_seat_request_set_selection_event *event = payload;
	wlr_seat_set_selection(server->seat, event->source, event->serial);
}

static void tmbr_server_on_request_set_primary_selection(struct wl_listener *listener, void *payload)
{
	tmbr_server_t *server = wl_container_of(listener, server, request_set_primary_selection);
	struct wlr_seat_request_set_primary_selection_event *event = payload;
	wlr_seat_set_primary_selection(server->seat, event->source, event->serial);
}

static int tmbr_server_focussed_client(tmbr_client_t **out, tmbr_server_t *server)
{
	if ((*out = server->screen->focus->focus) == NULL)
		return -1;
	return 0;
}

static void tmbr_server_stop(tmbr_server_t *server)
{
	wl_display_terminate(server->display);
}

static int tmbr_cmd_client_focus(tmbr_server_t *server, const tmbr_command_t *cmd)
{
	tmbr_client_t *focus;
	tmbr_tree_t *next;

	if (tmbr_server_focussed_client(&focus, server) < 0 ||
	    tmbr_tree_find_sibling(&next, focus->tree, cmd->sel) < 0)
		return ENOENT;
	tmbr_desktop_focus(focus->desktop, next->client, 1);

	return 0;
}

static int tmbr_cmd_client_kill(tmbr_server_t *server, TMBR_UNUSED const tmbr_command_t *cmd)
{
	tmbr_client_t *focus;
	if (tmbr_server_focussed_client(&focus, server) < 0)
		return ENOENT;
	tmbr_client_kill(focus);
	return 0;
}

static int tmbr_cmd_client_resize(tmbr_server_t *server, const tmbr_command_t *cmd)
{
	tmbr_client_t *client;
	tmbr_select_t select;
	tmbr_split_t split;
	tmbr_tree_t *tree;
	int i;

	if (tmbr_server_focussed_client(&client, server) < 0)
		return ENOENT;

	switch (cmd->dir) {
	    case TMBR_DIR_NORTH:
		split = TMBR_SPLIT_HORIZONTAL; select = TMBR_SELECT_NEXT; i = cmd->i * -1; break;
	    case TMBR_DIR_SOUTH:
		split = TMBR_SPLIT_HORIZONTAL; select = TMBR_SELECT_PREV; i = cmd->i; break;
	    case TMBR_DIR_EAST:
		split = TMBR_SPLIT_VERTICAL; select = TMBR_SELECT_PREV; i = cmd->i; break;
	    case TMBR_DIR_WEST:
		split = TMBR_SPLIT_VERTICAL; select = TMBR_SELECT_NEXT; i = cmd->i * -1; break;
	    case TMBR_DIR_LAST:
		assert(0); break;
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
	tmbr_desktop_recalculate(client->desktop);

	return 0;
}

static int tmbr_cmd_client_fullscreen(tmbr_server_t *server, TMBR_UNUSED const tmbr_command_t *cmd)
{
	tmbr_client_t *focus;
	if (tmbr_server_focussed_client(&focus, server) < 0)
		return ENOENT;
	tmbr_desktop_set_fullscreen(focus->desktop, !focus->desktop->fullscreen);
	return 0;
}

static int tmbr_cmd_client_to_desktop(tmbr_server_t *server, const tmbr_command_t *cmd)
{
	tmbr_desktop_t *target;
	tmbr_client_t *focus;

	if (tmbr_server_focussed_client(&focus, server) < 0 ||
	    tmbr_desktop_find_sibling(&target, focus->desktop, cmd->sel) < 0)
		return ENOENT;

	tmbr_desktop_remove_client(focus->desktop, focus);
	tmbr_desktop_add_client(target, focus);
	tmbr_desktop_focus(target, focus, 0);
	return 0;
}

static int tmbr_cmd_client_to_screen(tmbr_server_t *server, const tmbr_command_t *cmd)
{
	tmbr_screen_t *screen;
	tmbr_client_t *client;

	if (tmbr_server_focussed_client(&client, server) < 0 ||
	    tmbr_screen_find_sibling(&screen, client->desktop->screen, cmd->sel) < 0)
		return ENOENT;

	tmbr_desktop_remove_client(client->desktop, client);
	tmbr_desktop_add_client(screen->focus, client);
	tmbr_desktop_focus(screen->focus, client, 0);
	return 0;
}

static int tmbr_cmd_client_swap(tmbr_server_t *server, const tmbr_command_t *cmd)
{
	tmbr_client_t *focus;
	tmbr_tree_t *next;

	if (tmbr_server_focussed_client(&focus, server) < 0 ||
	    tmbr_tree_find_sibling(&next, focus->tree, cmd->sel) < 0)
		return ENOENT;
	if (tmbr_tree_swap(focus->tree, next) < 0)
		return EIO;
	tmbr_desktop_recalculate(focus->desktop);

	return 0;
}

static int tmbr_cmd_desktop_focus(tmbr_server_t *server, const tmbr_command_t *cmd)
{
	tmbr_desktop_t *sibling;
	if (tmbr_desktop_find_sibling(&sibling, server->screen->focus, cmd->sel) < 0)
		return ENOENT;
	if (tmbr_screen_focus_desktop(server->screen, sibling) < 0)
		return EIO;
	return 0;
}

static int tmbr_cmd_desktop_swap(tmbr_server_t *server, const tmbr_command_t *cmd)
{
	tmbr_desktop_t *sibling;
	if (tmbr_desktop_find_sibling(&sibling, server->screen->focus, cmd->sel) < 0)
		return ENOENT;
	if (tmbr_desktop_swap(server->screen->focus, sibling) < 0)
		return EIO;
	return 0;
}

static int tmbr_cmd_desktop_kill(tmbr_server_t *server, TMBR_UNUSED const tmbr_command_t *cmd)
{
	tmbr_desktop_t *desktop = server->screen->focus;
	if (desktop->clients)
		return EEXIST;
	if (!desktop->link.prev && !desktop->link.next)
		return ENOENT;
	if (tmbr_screen_remove_desktop(server->screen, desktop) < 0)
		return EIO;
	tmbr_desktop_free(desktop);
	return 0;
}

static int tmbr_cmd_desktop_new(tmbr_server_t *server, TMBR_UNUSED const tmbr_command_t *cmd)
{
	tmbr_desktop_t *desktop;
	if (tmbr_desktop_new(&desktop) < 0 ||
	    tmbr_screen_add_desktop(server->screen, desktop) < 0)
		return EIO;
	return 0;
}

static int tmbr_cmd_screen_focus(tmbr_server_t *server, const tmbr_command_t *cmd)
{
	tmbr_screen_t *sibling;
	if (tmbr_screen_find_sibling(&sibling, server->screen, cmd->sel) < 0)
		return ENOENT;
	if (tmbr_screen_focus(sibling) < 0)
		return EIO;
	return 0;
}

static int tmbr_cmd_tree_rotate(tmbr_server_t *server, TMBR_UNUSED const tmbr_command_t *cmd)
{
	tmbr_client_t *focus;
	tmbr_tree_t *p;

	if (tmbr_server_focussed_client(&focus, server) < 0 ||
	    (p = focus->tree->parent) == NULL)
		return ENOENT;

	if (p->split == TMBR_SPLIT_HORIZONTAL) {
		tmbr_tree_t *l = p->left;
		p->left = p->right;
		p->right = l;
	}
	p->split ^= 1;
	tmbr_desktop_recalculate(focus->desktop);

	return 0;
}

static int tmbr_cmd_state_subscribe(tmbr_server_t *server, int fd)
{
	unsigned i;
	for (i = 0; i < ARRAY_SIZE(server->subfds); i++) {
		if (server->subfds[i] != 0)
			continue;
		server->subfds[i] = dup(fd);
		return 0;
	}
	return ENOSPC;
}

static int tmbr_cmd_state_query(tmbr_server_t *server, int fd)
{
	tmbr_screen_t *s;

	tmbr_ctrl_write_data(fd, "screens:");
	wl_list_for_each(s, &server->screens, link) {
		tmbr_desktop_t *d;
		double x = 0, y = 0;
		int w, h;

		wlr_output_layout_output_coords(s->server->output_layout, s->output, &x, &y);
		wlr_output_effective_resolution(s->output, &w, &h);
		tmbr_ctrl_write_data(fd, "- name: %s", s->output->name);
		tmbr_ctrl_write_data(fd, "  x: %lf", x);
		tmbr_ctrl_write_data(fd, "  y: %lf", y);
		tmbr_ctrl_write_data(fd, "  width: %u", w);
		tmbr_ctrl_write_data(fd, "  height: %u", h);
		tmbr_ctrl_write_data(fd, "  selected: %s", s == server->screen ? "true" : "false");
		tmbr_ctrl_write_data(fd, "  desktops:");

		wl_list_for_each(d, &s->desktops, link) {
			tmbr_tree_t *it, *tree;

			tmbr_ctrl_write_data(fd, "  - selected: %s", d == s->focus ? "true" : "false");
			tmbr_ctrl_write_data(fd, "    clients:");

			tmbr_tree_foreach_leaf(d->clients, it, tree) {
				tmbr_client_t *c = tree->client;
				tmbr_ctrl_write_data(fd, "    - title: %s", c->surface->toplevel->title);
				tmbr_ctrl_write_data(fd, "      x: %u", c->x);
				tmbr_ctrl_write_data(fd, "      y: %u", c->y);
				tmbr_ctrl_write_data(fd, "      width: %u", c->w);
				tmbr_ctrl_write_data(fd, "      height: %u", c->h);
				tmbr_ctrl_write_data(fd, "      selected: %s", c == d->focus ? "true" : "false");
			}
		}
	}

	return 0;
}

static int tmbr_cmd_state_stop(tmbr_server_t *server)
{
	tmbr_server_stop(server);
	return 0;
}

static int tmbr_cmd_binding_add(tmbr_server_t *server, const tmbr_command_t *cmd)
{
	tmbr_binding_t *binding;

	if (!cmd->key.keycode)
		return EINVAL;

	wl_list_for_each(binding, &server->bindings, link)
		if (binding->keycode == cmd->key.keycode && binding->modifiers == cmd->key.modifiers)
			break;

	if (&binding->link == &server->bindings) {
		binding = tmbr_alloc(sizeof(*binding), "Could not allocate binding");
		wl_list_insert(&server->bindings, &binding->link);
	} else {
		free(binding->command);
	}

	binding->modifiers = cmd->key.modifiers;
	binding->keycode = cmd->key.keycode;
	if ((binding->command = strndup(cmd->command, sizeof(cmd->command))) == NULL)
		die("Could not allocate binding command");

	return 0;
}

static int tmbr_server_on_command(int fd, TMBR_UNUSED uint32_t mask, void *payload)
{
	tmbr_server_t *server = payload;
	tmbr_command_t *cmd;
	tmbr_pkt_t pkt;
	int error, cfd, persistent = 0;

	if ((cfd = accept(fd, NULL, NULL)) < 0)
		return 0;
	if (tmbr_ctrl_read(cfd, &pkt) < 0 || pkt.type != TMBR_PKT_COMMAND)
		goto out;
	cmd = &pkt.u.command;

	if (cmd->type >= TMBR_COMMAND_LAST || cmd->sel >= TMBR_SELECT_LAST || cmd->dir >= TMBR_DIR_LAST) {
		error = EINVAL;
		goto out;
	}

	switch (cmd->type) {
		case TMBR_COMMAND_CLIENT_FOCUS: error = tmbr_cmd_client_focus(server, cmd); break;
		case TMBR_COMMAND_CLIENT_KILL: error = tmbr_cmd_client_kill(server, cmd); break;
		case TMBR_COMMAND_CLIENT_RESIZE: error = tmbr_cmd_client_resize(server, cmd); break;
		case TMBR_COMMAND_CLIENT_SWAP: error = tmbr_cmd_client_swap(server, cmd); break;
		case TMBR_COMMAND_CLIENT_FULLSCREEN: error = tmbr_cmd_client_fullscreen(server, cmd); break;
		case TMBR_COMMAND_CLIENT_TO_DESKTOP: error = tmbr_cmd_client_to_desktop(server, cmd); break;
		case TMBR_COMMAND_CLIENT_TO_SCREEN: error = tmbr_cmd_client_to_screen(server, cmd); break;
		case TMBR_COMMAND_DESKTOP_FOCUS: error = tmbr_cmd_desktop_focus(server, cmd); break;
		case TMBR_COMMAND_DESKTOP_SWAP: error = tmbr_cmd_desktop_swap(server, cmd); break;
		case TMBR_COMMAND_DESKTOP_KILL: error = tmbr_cmd_desktop_kill(server, cmd); break;
		case TMBR_COMMAND_DESKTOP_NEW: error = tmbr_cmd_desktop_new(server, cmd); break;
		case TMBR_COMMAND_SCREEN_FOCUS: error = tmbr_cmd_screen_focus(server, cmd); break;
		case TMBR_COMMAND_TREE_ROTATE: error = tmbr_cmd_tree_rotate(server, cmd); break;
		case TMBR_COMMAND_STATE_SUBSCRIBE: error = tmbr_cmd_state_subscribe(server, cfd); persistent = 1; break;
		case TMBR_COMMAND_STATE_QUERY: error = tmbr_cmd_state_query(server, cfd); break;
		case TMBR_COMMAND_STATE_STOP: error = tmbr_cmd_state_stop(server); break;
		case TMBR_COMMAND_BINDING_ADD: error = tmbr_cmd_binding_add(server, cmd); break;
		case TMBR_COMMAND_LAST: error = ENOTSUP; break;
	}

	if (!error && persistent)
		return 0;

	tmbr_notify(server, "{type: command, error: %d}", error);
	memset(&pkt, 0, sizeof(pkt));
	pkt.type = TMBR_PKT_ERROR;
	pkt.u.error = error;
	tmbr_ctrl_write(cfd, &pkt);
out:
	close(cfd);
	return 0;
}

static void tmbr_on_signal(int signal)
{
	switch (signal) {
		case SIGCHLD: waitpid(-1, &signal, WNOHANG); break;
		case SIGTERM: tmbr_server_stop(&server); break;
	}
}

int tmbr_wm(void)
{
	struct sigaction sa;
	struct stat st;
	const char *socket;
	char *cfg;

	sa.sa_handler = tmbr_on_signal;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;
	if (sigaction(SIGCHLD, &sa, NULL) < 0 ||
	    sigaction(SIGTERM, &sa, NULL) < 0)
		die("Could not setup signal handler: %s", strerror(errno));

	wl_list_init(&server.bindings);
	wl_list_init(&server.screens);
	if ((server.display = wl_display_create()) == NULL)
		die("Could not create display");
	if ((server.backend = wlr_backend_autocreate(server.display, NULL)) == NULL)
		die("Could not create backend");
	wlr_renderer_init_wl_display(wlr_backend_get_renderer(server.backend), server.display);

	if (wlr_compositor_create(server.display, wlr_backend_get_renderer(server.backend)) == NULL ||
	    wlr_data_device_manager_create(server.display) == NULL ||
	    wlr_primary_selection_v1_device_manager_create(server.display) == NULL ||
	    wlr_xdg_decoration_manager_v1_create(server.display) == NULL ||
	    (server.decoration = wlr_server_decoration_manager_create(server.display)) == NULL ||
	    (server.cursor = wlr_cursor_create()) == NULL ||
	    (server.output_layout = wlr_output_layout_create()) == NULL ||
	    (server.seat = wlr_seat_create(server.display, "seat0")) == NULL ||
	    (server.xcursor = wlr_xcursor_manager_create(NULL, 24)) == NULL ||
	    (server.xdg_shell = wlr_xdg_shell_create(server.display)) == NULL)
		die("Could not create backends");

	wlr_server_decoration_manager_set_default_mode(server.decoration, WLR_SERVER_DECORATION_MANAGER_MODE_SERVER);
	wlr_cursor_attach_output_layout(server.cursor, server.output_layout);
	wlr_xcursor_manager_load(server.xcursor, 1);

	tmbr_register(&server.backend->events.new_input, &server.new_input, tmbr_server_on_new_input);
	tmbr_register(&server.backend->events.new_output, &server.new_output, tmbr_server_on_new_output);
	tmbr_register(&server.xdg_shell->events.new_surface, &server.new_surface, tmbr_server_on_new_surface);
	tmbr_register(&server.seat->events.request_set_cursor, &server.request_set_cursor, tmbr_server_on_request_set_cursor);
	tmbr_register(&server.seat->events.request_set_selection, &server.request_set_selection, tmbr_server_on_request_set_selection);
	tmbr_register(&server.seat->events.request_set_primary_selection, &server.request_set_primary_selection, tmbr_server_on_request_set_primary_selection);
	tmbr_register(&server.cursor->events.axis, &server.cursor_axis, tmbr_server_on_cursor_axis);
	tmbr_register(&server.cursor->events.button, &server.cursor_button, tmbr_server_on_cursor_button);
	tmbr_register(&server.cursor->events.motion, &server.cursor_motion, tmbr_server_on_cursor_motion);
	tmbr_register(&server.cursor->events.motion_absolute, &server.cursor_motion_absolute, tmbr_server_on_cursor_motion_absolute);
	tmbr_register(&server.cursor->events.frame, &server.cursor_frame, tmbr_server_on_cursor_frame);

	if ((socket = wl_display_add_socket_auto(server.display)) == NULL)
		die("Could not create Wayland socket");
	setenv("WAYLAND_DISPLAY", socket, 1);
	if ((server.ctrlfd = tmbr_ctrl_connect(&socket, 1)) < 0)
		die("Unable to setup control socket");
	wl_event_loop_add_fd(wl_display_get_event_loop(server.display), server.ctrlfd,
			     WL_EVENT_READABLE, tmbr_server_on_command, &server);

	if (!wlr_backend_start(server.backend))
		die("Could not start backend");

	if ((cfg = getenv("TMBR_CONFIG_PATH")) == NULL)
		cfg = TMBR_CONFIG_PATH;
	if (stat(cfg, &st) == 0)
		tmbr_spawn(cfg, (char * const[]){ cfg, NULL });
	else if (errno != ENOENT)
		die("Could not execute config file: %s", strerror(errno));

	wl_display_run(server.display);

	wl_display_destroy_clients(server.display);
	wl_display_destroy(server.display);
	return 0;
}
