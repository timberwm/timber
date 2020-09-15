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
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <wlr/backend.h>
#include <wlr/render/gles2.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_export_dmabuf_v1.h>
#include <wlr/types/wlr_gamma_control_v1.h>
#include <wlr/types/wlr_gtk_primary_selection.h>
#include <wlr/types/wlr_idle.h>
#include <wlr/types/wlr_idle_inhibit_v1.h>
#include <wlr/types/wlr_matrix.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_damage.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wlr/types/wlr_server_decoration.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_output_v1.h>
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
	struct wl_listener request_fullscreen;
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
	bool fullscreen;
};

struct tmbr_screen {
	struct wl_list link;
	tmbr_server_t *server;

	struct wlr_output *output;
	struct wl_list desktops;
	tmbr_desktop_t *focus;

	struct timespec render_time;
	struct wlr_output_damage *damage;

	struct wl_listener destroy;
	struct wl_listener frame;
	struct wl_listener scale;
	struct wl_listener mode;
};

struct tmbr_server {
	struct wl_display *display;
	struct wlr_backend *backend;
	struct wlr_compositor *compositor;
	struct wlr_cursor *cursor;
	struct wlr_idle *idle;
	struct wlr_idle_timeout *idle_timeout;
	struct wlr_output_layout *output_layout;
	struct wlr_seat *seat;
	struct wlr_server_decoration_manager *decoration;
	struct wlr_xcursor_manager *xcursor;
	struct wlr_xdg_shell *xdg_shell;

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
	struct wl_listener seat_idle;
	struct wl_listener seat_resume;

	int ctrlfd;
	int subfds[16];

	struct wl_list bindings;
	struct wl_list screens;
	tmbr_screen_t *screen;
};

static void tmbr_spawn(const char *path, char * const argv[])
{
	pid_t pid;
	int i;

	if ((pid = fork()) < 0) {
		die("Could not fork: %s", strerror(errno));
	} else if (pid == 0) {
		for (i = 0; i < 1024; i++)
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
	wl_list_remove(&client->destroy.link);
	wl_list_remove(&client->commit.link);
	wl_list_remove(&client->map.link);
	wl_list_remove(&client->unmap.link);
	wl_list_remove(&client->request_fullscreen.link);
	free(client);
}

static void tmbr_client_on_commit(struct wl_listener *listener, TMBR_UNUSED void *payload)
{
	tmbr_client_t *client = wl_container_of(listener, client, commit);
	if (client->desktop)
		wlr_output_damage_add_whole(client->desktop->screen->damage);
}

static tmbr_client_t *tmbr_client_new(tmbr_server_t *server, struct wlr_xdg_surface *surface)
{
	tmbr_client_t *client = tmbr_alloc(sizeof(*client), "Could not allocate client");
	client->server = server;
	client->surface = surface;

	tmbr_register(&surface->events.destroy, &client->destroy, tmbr_client_on_destroy);
	tmbr_register(&surface->surface->events.commit, &client->commit, tmbr_client_on_commit);

	return client;
}

static void tmbr_client_kill(tmbr_client_t *client)
{
	wlr_xdg_toplevel_send_close(client->surface);
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
	if (wlr_texture_is_gles2(texture)) {
		struct wlr_gles2_texture_attribs attribs;
		wlr_gles2_texture_get_attribs(texture, &attribs);
		glBindTexture(attribs.target, attribs.tex);
		glTexParameteri(attribs.target, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	}

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
		struct wlr_output *output = c->desktop->screen->output;
		const float *color = TMBR_COLOR_INACTIVE, s = output->scale;
		struct wlr_box borders[4] = {
			{ c->x * s, c->y * s, c->w * s, c->border * s },
			{ c->x * s, c->y * s, c->border * s, c->h * s },
			{ (c->x + c->w - c->border) * s, c->y * s, c->border * s, c->h * s },
			{ c->x * s, (c->y + c->h - c->border) * s, c->w * s, c->border * s },
		};
		size_t i;

		if (c->desktop->focus == c && c->desktop->screen == c->server->screen)
			color = TMBR_COLOR_ACTIVE;
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

static void tmbr_client_focus(tmbr_client_t *client, bool focus)
{
	wlr_xdg_toplevel_set_activated(client->surface, focus);
	if (focus) {
		struct wlr_seat *seat = client->desktop->screen->server->seat;
		struct wlr_keyboard *keyboard;
		if ((keyboard = wlr_seat_get_keyboard(seat)) != NULL)
			wlr_seat_keyboard_notify_enter(seat, client->surface->surface, keyboard->keycodes, keyboard->num_keycodes, &keyboard->modifiers);
		wlr_seat_pointer_notify_enter(seat, client->surface->surface, 0, 0);
	}
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

static void tmbr_tree_insert(tmbr_tree_t **tree, tmbr_client_t *client)
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
}

static tmbr_tree_t *tmbr_tree_get_child(tmbr_tree_t *tree, tmbr_select_t which)
{
	return (which == TMBR_SELECT_PREV) ? tree->left : tree->right;
}

static tmbr_tree_t *tmbr_tree_find_sibling(tmbr_tree_t *tree, tmbr_select_t which)
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
		return NULL;

	while (tmbr_tree_get_child(t, downwards))
		t = tmbr_tree_get_child(t, downwards);

	if (t == tree)
		return NULL;

	return t;
}

static void tmbr_tree_swap(tmbr_tree_t *a, tmbr_tree_t *b)
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
}

#define tmbr_tree_foreach_leaf(t, i, n) \
	i = NULL, n = t; \
	while ((!i && n && n->client && (i = n)) || ((n = tmbr_tree_find_sibling(n, TMBR_SELECT_NEXT)) != NULL && ((!i && (i = n)) || i != n)))

static void tmbr_tree_remove(tmbr_tree_t **tree, tmbr_tree_t *node)
{
	if (node != *tree) {
		tmbr_tree_t *uplift = (node->parent->left == node) ?
					node->parent->right : node->parent->left;
		tmbr_tree_swap(uplift, node->parent);
		free(uplift);
	} else {
		*tree = NULL;
	}
	free(node);
}

static tmbr_desktop_t *tmbr_desktop_new(void)
{
	return tmbr_alloc(sizeof(tmbr_desktop_t), "Could not allocate desktop");
}

static void tmbr_desktop_free(tmbr_desktop_t *desktop)
{
	free(desktop);
}

static tmbr_desktop_t *tmbr_desktop_find_sibling(tmbr_desktop_t *desktop, tmbr_select_t which)
{
	struct wl_list *sibling;
	if ((sibling = tmbr_list_get(&desktop->screen->desktops, &desktop->link, which)) == NULL)
		return NULL;
	return wl_container_of(sibling, desktop, link);
}

static void tmbr_desktop_focus_client(tmbr_desktop_t *desktop, tmbr_client_t *client, int inputfocus)
{
	struct wlr_surface *current = desktop->screen->server->seat->keyboard_state.focused_surface;

	if (!client) {
		desktop->focus = NULL;
		return;
	}
	if (current == client->surface->surface)
		return;

	if (inputfocus) {
		if (desktop->focus)
			tmbr_client_focus(desktop->focus, false);
		tmbr_client_focus(client, true);
	}

	desktop->focus = client;
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
	wlr_output_damage_add_whole(desktop->screen->damage);
}

static void tmbr_desktop_add_client(tmbr_desktop_t *desktop, tmbr_client_t *client)
{
	tmbr_tree_insert(desktop->focus ? &desktop->focus->tree : &desktop->clients, client);
	client->desktop = desktop;
	desktop->fullscreen = 0;
	tmbr_desktop_recalculate(desktop);
}

static void tmbr_desktop_remove_client(tmbr_desktop_t *desktop, tmbr_client_t *client)
{
	if (desktop->focus == client) {
		tmbr_tree_t *sibling = tmbr_tree_find_sibling(client->tree, TMBR_SELECT_NEAREST);
		tmbr_desktop_focus_client(desktop, sibling ? sibling->client : NULL, 1);
	}
	tmbr_tree_remove(&desktop->clients, client->tree);

	desktop->fullscreen = 0;
	client->desktop = NULL;
	client->tree = NULL;
	tmbr_desktop_recalculate(desktop);
}

static void tmbr_desktop_swap(tmbr_desktop_t *_a, tmbr_desktop_t *_b)
{
	struct wl_list *a = &_a->link, *b = &_b->link, *pos = b->prev;

	if (_a->screen != _b->screen)
		die("Cannot swap desktops of different screens");

	wl_list_remove(b);
	wl_list_insert(a->prev, b);
	if (pos == a)
		pos = b;
	wl_list_remove(a);
	wl_list_insert(pos, a);
}

static void tmbr_desktop_set_fullscreen(tmbr_desktop_t *desktop, bool fullscreen)
{
	if (desktop->fullscreen != fullscreen) {
		desktop->fullscreen = fullscreen;
		wlr_xdg_toplevel_set_fullscreen(desktop->focus->surface, fullscreen);
		tmbr_desktop_recalculate(desktop);
	}
}

static void tmbr_screen_focus_desktop(tmbr_screen_t *screen, tmbr_desktop_t *desktop)
{
	if (desktop->screen != screen)
		die("Cannot focus desktop for different screen");
	tmbr_desktop_focus_client(desktop, desktop->focus, 1);
	screen->focus = desktop;
	screen->server->screen = screen;
	wlr_output_damage_add_whole(screen->damage);
}

static void tmbr_screen_remove_desktop(tmbr_screen_t *screen, tmbr_desktop_t *desktop)
{
	if (desktop->clients)
		die("Cannot remove desktop from screen which has clients");

	if (screen->focus == desktop) {
		tmbr_desktop_t *sibling;
		if ((sibling = tmbr_desktop_find_sibling(desktop, TMBR_SELECT_NEXT)) == NULL)
			die("Cannot remove last screen's desktop");
		tmbr_screen_focus_desktop(screen, sibling);
	}
	wl_list_remove(&desktop->link);
}

static void tmbr_screen_add_desktop(tmbr_screen_t *screen, tmbr_desktop_t *desktop)
{
	wl_list_insert(screen->focus ? &screen->focus->link : &screen->desktops, &desktop->link);
	desktop->screen = screen;
	tmbr_screen_focus_desktop(screen, desktop);
}

static tmbr_screen_t *tmbr_screen_find_sibling(tmbr_screen_t *screen, tmbr_select_t which)
{
	struct wl_list *sibling;
	if ((sibling = tmbr_list_get(&screen->server->screens, &screen->link, which)) == NULL)
		return NULL;
	return wl_container_of(sibling, screen, link);;
}

static void tmbr_screen_on_destroy(struct wl_listener *listener, TMBR_UNUSED void *payload)
{
	tmbr_screen_t *screen = wl_container_of(listener, screen, destroy), *sibling = tmbr_screen_find_sibling(screen, TMBR_SELECT_NEXT);
	tmbr_desktop_t *desktop, *tmp;

	if (sibling) {
		wl_list_for_each_safe(desktop, tmp, &screen->desktops, link)
			tmbr_screen_add_desktop(sibling, desktop);
		wl_list_init(&screen->desktops);
	} else {
		tmbr_tree_t *it, *t;
		wl_list_for_each(desktop, &screen->desktops, link) {
			tmbr_tree_foreach_leaf(desktop->clients, it, t)
				tmbr_desktop_remove_client(desktop, t->client);
		}
		wl_display_terminate(screen->server->display);
	}

	wl_list_remove(&screen->destroy.link);
	wl_list_remove(&screen->frame.link);
	wl_list_remove(&screen->mode.link);
	wl_list_remove(&screen->scale.link);
	wl_list_remove(&screen->link);
	free(screen);
}

static void tmbr_screen_on_frame(struct wl_listener *listener, TMBR_UNUSED void *payload)
{
	tmbr_screen_t *screen = wl_container_of(listener, screen, frame);
	pixman_region32_t damage;
	bool needs_frame;

	pixman_region32_init(&damage);
	if (!wlr_output_damage_attach_render(screen->damage, &needs_frame, &damage))
		return;
	if (needs_frame) {
		struct wlr_renderer *renderer = wlr_backend_get_renderer(screen->output->backend);

		clock_gettime(CLOCK_MONOTONIC, &screen->render_time);
		wlr_renderer_begin(renderer, screen->output->width, screen->output->height);

		if (!screen->focus->focus) {
			wlr_renderer_clear(renderer, (float[4]){0.3, 0.3, 0.3, 1.0});
		} else if (screen->focus->fullscreen) {
			tmbr_client_render(screen->focus->focus);
		} else {
			tmbr_tree_t *it, *t;
			tmbr_tree_foreach_leaf(screen->focus->clients, it, t)
				tmbr_client_render(t->client);
		}
		wlr_output_render_software_cursors(screen->output, NULL);

		wlr_renderer_end(renderer);
		wlr_output_commit(screen->output);
	} else {
		wlr_output_rollback(screen->output);
	}

	pixman_region32_fini(&damage);
}

static void tmbr_screen_on_mode(struct wl_listener *listener, TMBR_UNUSED void *payload)
{
	tmbr_screen_t *screen = wl_container_of(listener, screen, mode);
	tmbr_desktop_recalculate(screen->focus);
}

static void tmbr_screen_on_scale(struct wl_listener *listener, TMBR_UNUSED void *payload)
{
	tmbr_screen_t *screen = wl_container_of(listener, screen, scale);
	tmbr_desktop_recalculate(screen->focus);
}

static tmbr_screen_t *tmbr_screen_new(tmbr_server_t *server, struct wlr_output *output)
{
	tmbr_screen_t *screen = tmbr_alloc(sizeof(*screen), "Could not allocate screen");
	screen->output = output;
	screen->server = server;
	screen->damage = wlr_output_damage_create(output);
	wl_list_init(&screen->desktops);

	tmbr_screen_add_desktop(screen, tmbr_desktop_new());
	tmbr_register(&output->events.destroy, &screen->destroy, tmbr_screen_on_destroy);
	tmbr_register(&output->events.mode, &screen->mode, tmbr_screen_on_mode);
	tmbr_register(&output->events.scale, &screen->scale, tmbr_screen_on_scale);
	tmbr_register(&screen->damage->events.frame, &screen->frame, tmbr_screen_on_frame);

	return screen;
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

	screen = tmbr_screen_new(server, output);
	wl_list_insert(&server->screens, &screen->link);
	if (!server->screen)
		server->screen = screen;

	wlr_output_layout_add_auto(server->output_layout, output);
}

static void tmbr_server_on_request_fullscreen(struct wl_listener *listener, void *payload)
{
	struct wlr_xdg_toplevel_set_fullscreen_event *event = payload;
	tmbr_client_t *client = wl_container_of(listener, client, request_fullscreen);
	if (client->desktop) {
		tmbr_desktop_focus_client(client->server->screen->focus, client, 0);
		tmbr_desktop_set_fullscreen(client->desktop, event->fullscreen);
	}
}

static void tmbr_server_on_map(struct wl_listener *listener, TMBR_UNUSED void *payload)
{
	tmbr_client_t *client = wl_container_of(listener, client, map);
	tmbr_desktop_add_client(client->server->screen->focus, client);
	tmbr_desktop_focus_client(client->server->screen->focus, client, 1);
}

static void tmbr_server_on_unmap(struct wl_listener *listener, TMBR_UNUSED void *payload)
{
	tmbr_client_t *client = wl_container_of(listener, client, unmap);
	if (client->desktop)
		tmbr_desktop_remove_client(client->desktop, client);
}

static void tmbr_server_on_new_surface(struct wl_listener *listener, void *payload)
{
	tmbr_server_t *server = wl_container_of(listener, server, new_surface);
	struct wlr_xdg_surface *surface = payload;
	tmbr_client_t *client;

	if (surface->role != WLR_XDG_SURFACE_ROLE_TOPLEVEL)
		return;

	client = tmbr_client_new(server, surface);
	tmbr_register(&surface->events.map, &client->map, tmbr_server_on_map);
	tmbr_register(&surface->events.unmap, &client->unmap, tmbr_server_on_unmap);
	tmbr_register(&surface->toplevel->events.request_fullscreen, &client->request_fullscreen, tmbr_server_on_request_fullscreen);
}

static void tmbr_keyboard_on_key(struct wl_listener *listener, void *payload)
{
	tmbr_keyboard_t *keyboard = wl_container_of(listener, keyboard, key);
	uint32_t modifiers = wlr_keyboard_get_modifiers(keyboard->device->keyboard);
	struct wlr_event_keyboard_key *event = payload;
	const xkb_keysym_t *keysyms;
	xkb_layout_index_t layout;
	tmbr_binding_t *binding;
	int i, n;

	wlr_idle_notify_activity(keyboard->server->idle, keyboard->server->seat);
	if (event->state != WLR_KEY_PRESSED)
		goto unhandled;

	layout = xkb_state_key_get_layout(keyboard->device->keyboard->xkb_state, event->keycode + 8);
	n = xkb_keymap_key_get_syms_by_level(keyboard->device->keyboard->keymap, event->keycode + 8, layout, 0, &keysyms);

	wl_list_for_each(binding, &keyboard->server->bindings, link) {
		for (i = 0; i < n; i++) {
			if (binding->keycode != keysyms[i] || binding->modifiers != modifiers)
				continue;
			tmbr_spawn("/bin/sh", (char * const[]){ "/bin/sh", "-c", binding->command, NULL });
			return;
		}
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

static void tmbr_keyboard_setup(tmbr_server_t *server, struct wlr_input_device *device)
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

	xkb_keymap_unref(keymap);
	xkb_context_unref(context);
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
			tmbr_keyboard_setup(server, device);
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
	wlr_idle_notify_activity(server->idle, server->seat);
	wlr_seat_pointer_notify_axis(server->seat, event->time_msec, event->orientation,
				     event->delta, event->delta_discrete, event->source);
}

static void tmbr_server_on_cursor_button(struct wl_listener *listener, void *payload)
{
	tmbr_server_t *server = wl_container_of(listener, server, cursor_button);
	struct wlr_event_pointer_button *event = payload;
	wlr_idle_notify_activity(server->idle, server->seat);
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

	wlr_idle_notify_activity(server->idle, server->seat);

	if ((output = wlr_output_layout_output_at(server->output_layout, x, y)) == NULL)
		return;

	wl_list_for_each(screen, &server->screens, link)
		if (screen->output == output)
			break;
	if (!screen)
		return;

	tmbr_screen_focus_desktop(screen, screen->focus);

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

		wlr_output_damage_add_whole(screen->damage);
		tmbr_desktop_focus_client(screen->focus, client, 1);
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

static void tmbr_server_on_idle(struct wl_listener *listener, TMBR_UNUSED void *payload)
{
	tmbr_server_t *server = wl_container_of(listener, server, seat_idle);
	tmbr_screen_t *s;
	wl_list_for_each(s, &server->screens, link) {
		wlr_output_enable(s->output, false);
		wlr_output_commit(s->output);
	}
}

static void tmbr_server_on_resume(struct wl_listener *listener, TMBR_UNUSED void *payload)
{
	tmbr_server_t *server = wl_container_of(listener, server, seat_resume);
	tmbr_screen_t *s;
	wl_list_for_each(s, &server->screens, link) {
		wlr_output_enable(s->output, true);
		wlr_output_commit(s->output);
	}
}

static tmbr_client_t *tmbr_server_focussed_client(tmbr_server_t *server)
{
	return server->screen->focus->focus;
}

static tmbr_screen_t *tmbr_server_find_output(tmbr_server_t *server, const char *output)
{
	tmbr_screen_t *s;
	wl_list_for_each(s, &server->screens, link)
		if (!strcmp(s->output->name, output))
			return s;
	return NULL;
}

static void tmbr_server_stop(tmbr_server_t *server)
{
	wl_display_terminate(server->display);
}

static int tmbr_cmd_client_focus(tmbr_server_t *server, const tmbr_command_t *cmd)
{
	tmbr_client_t *focus;
	tmbr_tree_t *next;

	if ((focus = tmbr_server_focussed_client(server)) == NULL ||
	    (next = tmbr_tree_find_sibling(focus->tree, cmd->sel)) == NULL)
		return ENOENT;
	tmbr_desktop_focus_client(focus->desktop, next->client, 1);

	return 0;
}

static int tmbr_cmd_client_kill(tmbr_server_t *server, TMBR_UNUSED const tmbr_command_t *cmd)
{
	tmbr_client_t *focus;
	if ((focus = tmbr_server_focussed_client(server)) == NULL)
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

	if ((client = tmbr_server_focussed_client(server)) == NULL)
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
	if ((focus = tmbr_server_focussed_client(server)) == NULL)
		return ENOENT;
	tmbr_desktop_set_fullscreen(focus->desktop, !focus->desktop->fullscreen);
	return 0;
}

static int tmbr_cmd_client_to_desktop(tmbr_server_t *server, const tmbr_command_t *cmd)
{
	tmbr_desktop_t *target;
	tmbr_client_t *focus;

	if ((focus = tmbr_server_focussed_client(server)) == NULL ||
	    (target = tmbr_desktop_find_sibling(focus->desktop, cmd->sel)) == NULL)
		return ENOENT;
	tmbr_desktop_remove_client(focus->desktop, focus);
	tmbr_desktop_add_client(target, focus);
	tmbr_desktop_focus_client(target, focus, 0);

	return 0;
}

static int tmbr_cmd_client_to_screen(tmbr_server_t *server, const tmbr_command_t *cmd)
{
	tmbr_screen_t *screen;
	tmbr_client_t *client;

	if ((client = tmbr_server_focussed_client(server)) == NULL ||
	    (screen = tmbr_screen_find_sibling(client->desktop->screen, cmd->sel)) == NULL)
		return ENOENT;
	tmbr_desktop_remove_client(client->desktop, client);
	tmbr_desktop_add_client(screen->focus, client);
	tmbr_desktop_focus_client(screen->focus, client, 0);

	return 0;
}

static int tmbr_cmd_client_swap(tmbr_server_t *server, const tmbr_command_t *cmd)
{
	tmbr_client_t *focus;
	tmbr_tree_t *next;

	if ((focus = tmbr_server_focussed_client(server)) == NULL ||
	    (next = tmbr_tree_find_sibling(focus->tree, cmd->sel)) == NULL)
		return ENOENT;
	tmbr_tree_swap(focus->tree, next);
	tmbr_desktop_recalculate(focus->desktop);

	return 0;
}

static int tmbr_cmd_desktop_focus(tmbr_server_t *server, const tmbr_command_t *cmd)
{
	tmbr_desktop_t *sibling;
	if ((sibling = tmbr_desktop_find_sibling(server->screen->focus, cmd->sel)) == NULL)
		return ENOENT;
	tmbr_screen_focus_desktop(server->screen, sibling);
	return 0;
}

static int tmbr_cmd_desktop_swap(tmbr_server_t *server, const tmbr_command_t *cmd)
{
	tmbr_desktop_t *sibling;
	if ((sibling = tmbr_desktop_find_sibling(server->screen->focus, cmd->sel)) == NULL)
		return ENOENT;
	tmbr_desktop_swap(server->screen->focus, sibling);
	return 0;
}

static int tmbr_cmd_desktop_kill(tmbr_server_t *server, TMBR_UNUSED const tmbr_command_t *cmd)
{
	tmbr_desktop_t *desktop = server->screen->focus;
	if (desktop->clients)
		return EEXIST;
	if (!desktop->link.prev && !desktop->link.next)
		return ENOENT;
	tmbr_screen_remove_desktop(server->screen, desktop);
	tmbr_desktop_free(desktop);
	return 0;
}

static int tmbr_cmd_desktop_new(tmbr_server_t *server, TMBR_UNUSED const tmbr_command_t *cmd)
{
	tmbr_screen_add_desktop(server->screen, tmbr_desktop_new());
	return 0;
}

static int tmbr_cmd_screen_focus(tmbr_server_t *server, const tmbr_command_t *cmd)
{
	tmbr_screen_t *sibling;
	if ((sibling = tmbr_screen_find_sibling(server->screen, cmd->sel)) == NULL)
		return ENOENT;
	tmbr_screen_focus_desktop(sibling, sibling->focus);
	return 0;
}

static int tmbr_cmd_screen_scale(tmbr_server_t *server, const tmbr_command_t *cmd)
{
	tmbr_screen_t *s;
	if (cmd->i <= 0 || cmd->i >= 10000 || cmd->screen[sizeof(cmd->screen) - 1])
		return EINVAL;
	if ((s = tmbr_server_find_output(server, cmd->screen)) == NULL)
		return ENOENT;
	wlr_output_set_scale(s->output, cmd->i / 100.0);
	return 0;
}

static int tmbr_cmd_screen_mode(tmbr_server_t *server, const tmbr_command_t *cmd)
{
	struct wlr_output_mode *mode;
	tmbr_screen_t *s;

	if ((s = tmbr_server_find_output(server, cmd->screen)) == NULL)
		return ENOENT;
	wl_list_for_each(mode, &s->output->modes, link) {
		if (cmd->mode.width !=  mode->width || cmd->mode.height != mode->height || cmd->mode.refresh != mode->refresh)
			continue;
		wlr_output_set_mode(s->output, mode);
		return 0;
	}

	return ENOENT;
}

static int tmbr_cmd_tree_rotate(tmbr_server_t *server, TMBR_UNUSED const tmbr_command_t *cmd)
{
	tmbr_client_t *focus;
	tmbr_tree_t *p;

	if ((focus = tmbr_server_focussed_client(server)) == NULL ||
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
		struct wlr_output_mode *mode;
		tmbr_desktop_t *d;
		double x = 0, y = 0;
		int w, h;

		wlr_output_layout_output_coords(s->server->output_layout, s->output, &x, &y);
		wlr_output_effective_resolution(s->output, &w, &h);
		tmbr_ctrl_write_data(fd, "- name: %s", s->output->name);
		tmbr_ctrl_write_data(fd, "  geom: {x: %u, y: %u, width: %u, height: %u}", (int)x, (int)y, w, h);
		tmbr_ctrl_write_data(fd, "  selected: %s", s == server->screen ? "true" : "false");
		tmbr_ctrl_write_data(fd, "  modes:");
		wl_list_for_each(mode, &s->output->modes, link)
			tmbr_ctrl_write_data(fd, "  - %dx%d@%d", mode->width, mode->height, mode->refresh);
		tmbr_ctrl_write_data(fd, "  desktops:");

		wl_list_for_each(d, &s->desktops, link) {
			tmbr_tree_t *it, *tree;

			tmbr_ctrl_write_data(fd, "  - selected: %s", d == s->focus ? "true" : "false");
			tmbr_ctrl_write_data(fd, "    clients:");

			tmbr_tree_foreach_leaf(d->clients, it, tree) {
				tmbr_client_t *c = tree->client;
				tmbr_ctrl_write_data(fd, "    - title: %s", c->surface->toplevel->title);
				tmbr_ctrl_write_data(fd, "      geom: {x: %u, y: %u, width: %u, height: %u}", c->x, c->y, c->w, c->h);
				tmbr_ctrl_write_data(fd, "      selected: %s", c == d->focus ? "true" : "false");
			}
		}
	}

	return 0;
}

static int tmbr_cmd_state_quit(tmbr_server_t *server)
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
		case TMBR_COMMAND_SCREEN_SCALE: error = tmbr_cmd_screen_scale(server, cmd); break;
		case TMBR_COMMAND_SCREEN_MODE: error = tmbr_cmd_screen_mode(server, cmd); break;
		case TMBR_COMMAND_TREE_ROTATE: error = tmbr_cmd_tree_rotate(server, cmd); break;
		case TMBR_COMMAND_STATE_SUBSCRIBE: error = tmbr_cmd_state_subscribe(server, cfd); persistent = 1; break;
		case TMBR_COMMAND_STATE_QUERY: error = tmbr_cmd_state_query(server, cfd); break;
		case TMBR_COMMAND_STATE_QUIT: error = tmbr_cmd_state_quit(server); break;
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

static int tmbr_server_on_signal(int signal, void *payload)
{
	tmbr_server_t *server = payload;
	switch (signal) {
		case SIGCHLD: waitpid(-1, &signal, WNOHANG); break;
		case SIGTERM: tmbr_server_stop(server); break;
	}
	return 0;
}

int tmbr_wm(void)
{
	struct wl_event_loop *loop;
	tmbr_server_t server;
	const char *socket;
	struct stat st;
	char *cfg;

	wl_list_init(&server.bindings);
	wl_list_init(&server.screens);
	if ((server.display = wl_display_create()) == NULL)
		die("Could not create display");
	if ((server.backend = wlr_backend_autocreate(server.display, NULL)) == NULL)
		die("Could not create backend");
	wlr_renderer_init_wl_display(wlr_backend_get_renderer(server.backend), server.display);

	if (wlr_compositor_create(server.display, wlr_backend_get_renderer(server.backend)) == NULL ||
	    wlr_data_device_manager_create(server.display) == NULL ||
	    wlr_export_dmabuf_manager_v1_create(server.display) == NULL ||
	    wlr_gamma_control_manager_v1_create(server.display) == NULL ||
	    wlr_gtk_primary_selection_device_manager_create(server.display) == NULL ||
	    wlr_idle_inhibit_v1_create(server.display) == NULL ||
	    wlr_primary_selection_v1_device_manager_create(server.display) == NULL ||
	    wlr_xdg_decoration_manager_v1_create(server.display) == NULL ||
	    (server.decoration = wlr_server_decoration_manager_create(server.display)) == NULL ||
	    (server.cursor = wlr_cursor_create()) == NULL ||
	    (server.seat = wlr_seat_create(server.display, "seat0")) == NULL ||
	    (server.idle = wlr_idle_create(server.display)) == NULL ||
	    (server.idle_timeout = wlr_idle_timeout_create(server.idle, server.seat, TMBR_SCREEN_DPMS_TIMEOUT)) == NULL ||
	    (server.output_layout = wlr_output_layout_create()) == NULL ||
	    (server.xcursor = wlr_xcursor_manager_create(NULL, 24)) == NULL ||
	    (server.xdg_shell = wlr_xdg_shell_create(server.display)) == NULL ||
	    wlr_xdg_output_manager_v1_create(server.display, server.output_layout) == NULL)
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
	tmbr_register(&server.idle_timeout->events.idle, &server.seat_idle, tmbr_server_on_idle);
	tmbr_register(&server.idle_timeout->events.resume, &server.seat_resume, tmbr_server_on_resume);

	if ((socket = wl_display_add_socket_auto(server.display)) == NULL)
		die("Could not create Wayland socket");
	setenv("WAYLAND_DISPLAY", socket, 1);
	if ((server.ctrlfd = tmbr_ctrl_connect(1)) < 0)
		die("Unable to setup control socket");

	loop = wl_display_get_event_loop(server.display);
	wl_event_loop_add_fd(loop, server.ctrlfd, WL_EVENT_READABLE, tmbr_server_on_command, &server);
	wl_event_loop_add_signal(loop, SIGCHLD, tmbr_server_on_signal, &server);
	wl_event_loop_add_signal(loop, SIGTERM, tmbr_server_on_signal, &server);

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
