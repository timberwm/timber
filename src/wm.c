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
#include "timber-server.h"
#include "wm.h"

#define TMBR_UNUSED __attribute__((unused))

#define tmbr_return_error(resource, code, msg) \
	do { wl_resource_post_error((resource), (code), (msg)); return; } while (0)

#define wlr_box_scaled(vx, vy, vw, vh, s) (struct wlr_box){ .x = (vx)*(s), .y = (vy)*(s), .width = (vw)*(s), .height = (vh)*(s) }

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

	struct wl_listener destroy;
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
	struct wlr_output_damage *damage;
	struct wl_list desktops;
	tmbr_desktop_t *focus;

	struct wl_listener destroy;
	struct wl_listener frame;
	struct wl_listener scale;
	struct wl_listener mode;
};

struct tmbr_server {
	struct wl_display *display;
	struct wlr_backend *backend;
	struct wlr_cursor *cursor;
	struct wlr_idle *idle;
	struct wlr_idle_inhibit_manager_v1 *idle_inhibit;
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
	struct wl_listener inhibitor_new;
	struct wl_listener inhibitor_destroy;

	int inhibitors;

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
		sigset_t set;

		if (setsid() < 0 || sigemptyset(&set) < 0 || sigprocmask(SIG_SETMASK, &set, NULL) < 0)
			die("Could not prepare child process: %s", strerror(errno));
		for (i = 0; i < 1024; i++)
			close(i);
		if (fork() == 0 && execv(path, argv) < 0)
			die("Could not execute '%s': %s", path, strerror(errno));

		_exit(0);
	}

	waitpid(pid, NULL, 0);
}

static tmbr_client_t *tmbr_server_find_focus(tmbr_server_t *server)
{
	return server->screen->focus->focus;
}

static struct wl_list *tmbr_list_get(struct wl_list *head, struct wl_list *link, enum tmbr_ctrl_selection which)
{
	struct wl_list *sibling = (which == TMBR_CTRL_SELECTION_PREV) ? link->prev : link->next;
	if (sibling == head)
		sibling = (which == TMBR_CTRL_SELECTION_PREV) ? head->prev : head->next;
	return (sibling == link) ? NULL : sibling;
}

static void tmbr_register(struct wl_signal *signal, struct wl_listener *listener, wl_notify_func_t callback)
{
	listener->notify = callback;
	wl_signal_add(signal, listener);
}

static void tmbr_client_damage(tmbr_client_t *c)
{
	struct wlr_box box = wlr_box_scaled(c->x, c->y, c->w, c->h, c->desktop->screen->output->scale);
	wlr_output_damage_add_box(c->desktop->screen->damage, &box);
}

static void tmbr_client_kill(tmbr_client_t *client)
{
	wlr_xdg_toplevel_send_close(client->surface);
}

static void tmbr_client_send_frame_done(struct wlr_surface *surface, TMBR_UNUSED int sx, TMBR_UNUSED int sy, TMBR_UNUSED void *payload)
{
	wlr_surface_send_frame_done(surface, (struct timespec *)payload);
}

static void tmbr_client_render_surface(struct wlr_surface *surface, int sx, int sy, void *payload)
{
	tmbr_client_t *client = payload;
	struct wlr_output *output = client->desktop->screen->output;
	struct wlr_box box = wlr_box_scaled(
		client->x + client->border + sx, client->y + client->border + sy,
		surface->current.width, surface->current.height, output->scale
	);
	struct wlr_texture *texture;
	float matrix[9];

	if ((texture = wlr_surface_get_texture(surface)) == NULL)
		return;
	if (wlr_texture_is_gles2(texture)) {
		struct wlr_gles2_texture_attribs attribs;
		wlr_gles2_texture_get_attribs(texture, &attribs);
		glBindTexture(attribs.target, attribs.tex);
		glTexParameteri(attribs.target, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	}

	wlr_matrix_project_box(matrix, &box, wlr_output_transform_invert(surface->current.transform), 0, output->transform_matrix);
	wlr_render_texture_with_matrix(wlr_backend_get_renderer(output->backend), texture, matrix, 1);
}

static void tmbr_client_render(tmbr_client_t *c, pixman_region32_t *damage)
{
	struct wlr_output *output = c->desktop->screen->output;
	struct wlr_box box = wlr_box_scaled(c->x, c->y, c->w, c->h, output->scale);
	pixman_box32_t rect = { .x1 = box.x, .x2 = box.x + box.width, .y1 = box.y, .y2 = box.y + box.height };

	if (!pixman_region32_contains_rectangle(damage, &rect))
		return;

	wlr_renderer_scissor(wlr_backend_get_renderer(output->backend), &box);
	if (c->surface->geometry.width < c->w - 2 * c->border || c->surface->geometry.height < c->h - 2 * c->border)
		wlr_renderer_clear(wlr_backend_get_renderer(output->backend), (float[4]){0.0, 0.0, 0.0, 1.0});
	if (c->border) {
		const float *color = TMBR_COLOR_INACTIVE;
		struct wlr_box borders[4] = {
			wlr_box_scaled(c->x, c->y, c->w, c->border, output->scale),
			wlr_box_scaled(c->x, c->y, c->border, c->h, output->scale),
			wlr_box_scaled(c->x + c->w - c->border, c->y, c->border, c->h, output->scale),
			wlr_box_scaled(c->x, c->y + c->h - c->border, c->w, c->border, output->scale),
		};
		size_t i;

		if (c->desktop->focus == c && c->desktop->screen == c->server->screen)
			color = TMBR_COLOR_ACTIVE;
		for (i = 0; i < ARRAY_SIZE(borders); i++)
			wlr_render_rect(wlr_backend_get_renderer(output->backend), &borders[i], color, output->transform_matrix);
	}
	wlr_xdg_surface_for_each_surface(c->surface, tmbr_client_render_surface, c);
	wlr_renderer_scissor(wlr_backend_get_renderer(output->backend), NULL);
}

static void tmbr_client_set_box(tmbr_client_t *client, int x, int y, int w, int h, int border)
{
	if (client->w != w || client->h != h || client->border != border)
		wlr_xdg_toplevel_set_size(client->surface, w - 2 * border, h - 2 * border);
	if (client->w != w || client->h != h || client->border != border || client->x != x || client->y != y) {
		client->w = w; client->h = h; client->x = x; client->y = y; client->border = border;
		tmbr_client_damage(client);
	}
}

static void tmbr_client_notify_pointer(tmbr_client_t *client, uint32_t time)
{
	tmbr_server_t *server = client->desktop->screen->server;
	double x = server->cursor->x, y = server->cursor->y;
	struct wlr_surface *surface;

	if ((surface = wlr_xdg_surface_surface_at(client->surface, x - client->x, y - client->y, &x, &y)) != NULL) {
		wlr_seat_pointer_notify_enter(server->seat, surface, x, y);
		if (time)
			wlr_seat_pointer_notify_motion(server->seat, time, x, y);
	} else {
		wlr_seat_pointer_notify_clear_focus(server->seat);
	}
}

static void tmbr_client_focus(tmbr_client_t *client, bool focus)
{
	wlr_xdg_toplevel_set_activated(client->surface, focus);
	if (focus) {
		tmbr_server_t *server = client->desktop->screen->server;
		struct wlr_keyboard *keyboard;

		if ((keyboard = wlr_seat_get_keyboard(server->seat)) != NULL)
			wlr_seat_keyboard_notify_enter(server->seat, client->surface->surface, keyboard->keycodes, keyboard->num_keycodes, &keyboard->modifiers);
		tmbr_client_notify_pointer(client, 0);
	}
	tmbr_client_damage(client);
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
	if (client->desktop && client->desktop->screen->focus == client->desktop) {
		tmbr_client_damage(client);
		if (client == client->desktop->focus)
			tmbr_client_notify_pointer(client, 0);
	}
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

static tmbr_tree_t *tmbr_tree_get_child(tmbr_tree_t *tree, enum tmbr_ctrl_selection which)
{
	return (which == TMBR_CTRL_SELECTION_PREV) ? tree->left : tree->right;
}

static tmbr_tree_t *tmbr_tree_find_sibling(tmbr_tree_t *tree, enum tmbr_ctrl_selection which)
{
	enum tmbr_ctrl_selection upwards = which, downwards = !which;
	tmbr_tree_t *t = tree;

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
	while ((!i && n && n->client && (i = n)) || ((n = tmbr_tree_find_sibling(n, TMBR_CTRL_SELECTION_NEXT)) != NULL && ((!i && (i = n)) || i != n)))

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

static tmbr_desktop_t *tmbr_desktop_find_sibling(tmbr_desktop_t *desktop, enum tmbr_ctrl_selection which)
{
	struct wl_list *sibling;
	if ((sibling = tmbr_list_get(&desktop->screen->desktops, &desktop->link, which)) == NULL)
		return NULL;
	return wl_container_of(sibling, desktop, link);
}

static void tmbr_desktop_recalculate(tmbr_desktop_t *desktop)
{
	int width, height;
	wlr_output_effective_resolution(desktop->screen->output, &width, &height);
	if (desktop->fullscreen && desktop->focus)
		tmbr_client_set_box(desktop->focus, 0, 0, width, height, 0);
	else
		tmbr_tree_recalculate(desktop->clients, 0, 0, width, height);
}

static void tmbr_desktop_set_fullscreen(tmbr_desktop_t *desktop, bool fullscreen)
{
	if (desktop->fullscreen == fullscreen)
		return;
	desktop->fullscreen = fullscreen;
	if (desktop->focus)
		wlr_xdg_toplevel_set_fullscreen(desktop->focus->surface, fullscreen);
	tmbr_desktop_recalculate(desktop);
	wlr_output_damage_add_whole(desktop->screen->damage);
}

static void tmbr_desktop_focus_client(tmbr_desktop_t *desktop, tmbr_client_t *client, bool inputfocus)
{
	if (inputfocus) {
		tmbr_client_t *current_focus = tmbr_server_find_focus(desktop->screen->server);
		if (current_focus && current_focus != client)
			tmbr_client_focus(current_focus, false);
		if (client)
			tmbr_client_focus(client, true);
	}
	if (desktop->focus == client)
		return;
	desktop->focus = client;
	tmbr_desktop_set_fullscreen(desktop, false);
	tmbr_desktop_recalculate(desktop);
}

static void tmbr_desktop_add_client(tmbr_desktop_t *desktop, tmbr_client_t *client)
{
	tmbr_tree_insert(desktop->focus ? &desktop->focus->tree : &desktop->clients, client);
	client->desktop = desktop;
	tmbr_desktop_set_fullscreen(desktop, false);
	tmbr_desktop_recalculate(desktop);
}

static void tmbr_desktop_remove_client(tmbr_desktop_t *desktop, tmbr_client_t *client)
{
	if (desktop->focus == client) {
		enum tmbr_ctrl_selection sel = (client->tree->parent && client->tree->parent->left == client->tree)
			? TMBR_CTRL_SELECTION_NEXT : TMBR_CTRL_SELECTION_PREV;
		tmbr_tree_t *sibling = tmbr_tree_find_sibling(client->tree, sel);
		tmbr_desktop_focus_client(desktop, sibling ? sibling->client : NULL, true);
	}
	tmbr_tree_remove(&desktop->clients, client->tree);
	tmbr_desktop_set_fullscreen(desktop, false);
	tmbr_desktop_recalculate(desktop);
	client->desktop = NULL;
	client->tree = NULL;
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

static void tmbr_screen_focus_desktop(tmbr_screen_t *screen, tmbr_desktop_t *desktop)
{
	if (desktop->screen != screen)
		die("Cannot focus desktop for different screen");
	if (screen->focus != desktop)
		wlr_output_damage_add_whole(screen->damage);
	screen->focus = desktop;
	screen->server->screen = screen;
	tmbr_desktop_focus_client(desktop, desktop->focus, true);
}

static void tmbr_screen_remove_desktop(tmbr_screen_t *screen, tmbr_desktop_t *desktop)
{
	if (desktop->clients)
		die("Cannot remove non-empty desktop");

	if (screen->focus == desktop) {
		tmbr_desktop_t *sibling;
		if ((sibling = tmbr_desktop_find_sibling(desktop, TMBR_CTRL_SELECTION_NEXT)) == NULL)
			die("Cannot remove screen's last desktop");
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

static tmbr_screen_t *tmbr_screen_find_sibling(tmbr_screen_t *screen, enum tmbr_ctrl_selection which)
{
	struct wl_list *sibling;
	if ((sibling = tmbr_list_get(&screen->server->screens, &screen->link, which)) == NULL)
		return NULL;
	return wl_container_of(sibling, screen, link);;
}

static void tmbr_screen_on_destroy(struct wl_listener *listener, TMBR_UNUSED void *payload)
{
	tmbr_screen_t *screen = wl_container_of(listener, screen, destroy), *sibling = tmbr_screen_find_sibling(screen, TMBR_CTRL_SELECTION_NEXT);
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
	tmbr_tree_t *it, *tree;
	struct timespec time;
	bool needs_frame;

	pixman_region32_init(&damage);
	if (!wlr_output_damage_attach_render(screen->damage, &needs_frame, &damage))
		goto out;
	if (needs_frame) {
		struct wlr_renderer *renderer = wlr_backend_get_renderer(screen->output->backend);

		wlr_renderer_begin(renderer, screen->output->width, screen->output->height);

		if (!screen->focus->focus) {
			wlr_renderer_clear(renderer, (float[4]){0.3, 0.3, 0.3, 1.0});
		} else if (pixman_region32_not_empty(&damage) && !screen->focus->fullscreen) {
			tmbr_tree_foreach_leaf(screen->focus->clients, it, tree)
				tmbr_client_render(tree->client, &damage);
		} else if (pixman_region32_not_empty(&damage)) {
			tmbr_client_render(screen->focus->focus, &damage);
		}

		wlr_output_render_software_cursors(screen->output, &damage);
		wlr_renderer_end(renderer);
		wlr_output_set_damage(screen->output, &screen->damage->current);
		wlr_output_commit(screen->output);
	} else {
		wlr_output_rollback(screen->output);
	}

out:
	pixman_region32_fini(&damage);

	clock_gettime(CLOCK_MONOTONIC, &time);
	tmbr_tree_foreach_leaf(screen->focus->clients, it, tree) {
		wlr_xdg_surface_for_each_surface(tree->client->surface,
						 tmbr_client_send_frame_done, &time);
	}
}

static void tmbr_screen_on_mode(struct wl_listener *listener, TMBR_UNUSED void *payload)
{
	tmbr_screen_t *screen = wl_container_of(listener, screen, mode);
	tmbr_desktop_recalculate(screen->focus);
}

static void tmbr_screen_on_scale(struct wl_listener *listener, TMBR_UNUSED void *payload)
{
	tmbr_screen_t *screen = wl_container_of(listener, screen, scale);
	wlr_xcursor_manager_load(screen->server->xcursor, screen->output->scale);
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

static void tmbr_keyboard_on_destroy(struct wl_listener *listener, TMBR_UNUSED void *payload)
{
	tmbr_keyboard_t *keyboard = wl_container_of(listener, keyboard, destroy);
	wl_list_remove(&keyboard->destroy.link);
	wl_list_remove(&keyboard->key.link);
	wl_list_remove(&keyboard->modifiers.link);
	free(keyboard);
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

static void tmbr_keyboard_new(tmbr_server_t *server, struct wlr_input_device *device)
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

	tmbr_register(&device->keyboard->events.destroy, &keyboard->destroy, tmbr_keyboard_on_destroy);
	tmbr_register(&device->keyboard->events.key, &keyboard->key, tmbr_keyboard_on_key);
	tmbr_register(&device->keyboard->events.modifiers, &keyboard->modifiers, tmbr_keyboard_on_modifiers);

	xkb_keymap_unref(keymap);
	xkb_context_unref(context);
}

static tmbr_screen_t *tmbr_server_find_output(tmbr_server_t *server, const char *output)
{
	tmbr_screen_t *s;
	wl_list_for_each(s, &server->screens, link)
		if (!strcmp(s->output->name, output))
			return s;
	return NULL;
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
			tmbr_keyboard_new(server, device);
			break;
		default:
			break;
	}

	wlr_seat_set_capabilities(server->seat, WL_SEAT_CAPABILITY_POINTER|WL_SEAT_CAPABILITY_KEYBOARD);
}

static void tmbr_server_on_new_output(struct wl_listener *listener, void *payload)
{
	struct wlr_output *output = payload;
	struct wlr_output_mode *mode;
	tmbr_server_t *server = wl_container_of(listener, server, new_output);
	tmbr_screen_t *screen;

	wlr_output_enable(output, true);
	if ((mode = wlr_output_preferred_mode(output)) != NULL)
		wlr_output_set_mode(output, mode);
	wlr_output_layout_add_auto(server->output_layout, output);
	if (!wlr_output_commit(output))
		return;

	screen = tmbr_screen_new(server, output);
	wl_list_insert(&server->screens, &screen->link);
	if (!server->screen)
		server->screen = screen;
}

static void tmbr_server_on_request_fullscreen(struct wl_listener *listener, void *payload)
{
	struct wlr_xdg_toplevel_set_fullscreen_event *event = payload;
	tmbr_client_t *client = wl_container_of(listener, client, request_fullscreen);
	if (client->desktop && client == tmbr_server_find_focus(client->server))
		tmbr_desktop_set_fullscreen(client->desktop, event->fullscreen);
}

static void tmbr_server_on_map(struct wl_listener *listener, TMBR_UNUSED void *payload)
{
	tmbr_client_t *client = wl_container_of(listener, client, map);
	tmbr_desktop_add_client(client->server->screen->focus, client);
	tmbr_desktop_focus_client(client->server->screen->focus, client, true);
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

	wlr_idle_notify_activity(server->idle, server->seat);

	if ((output = wlr_output_layout_output_at(server->output_layout, x, y)) == NULL)
		return;

	wl_list_for_each(screen, &server->screens, link)
		if (screen->output == output)
			break;
	if (&screen->link == &server->screens)
		return;

	tmbr_screen_focus_desktop(screen, screen->focus);

	if (screen->focus->fullscreen) {
		client = screen->focus->focus;
	} else {
		tmbr_tree_t *it, *t;
		tmbr_tree_foreach_leaf(screen->focus->clients, it, t) {
			if (t->client->x > x || t->client->x + t->client->w < x ||
			    t->client->y > y || t->client->y + t->client->h < y)
				continue;
			client = t->client;
			break;
		}
	}

	if (client) {
		tmbr_desktop_focus_client(screen->focus, client, true);
		tmbr_client_notify_pointer(client, time);
	} else {
		wlr_xcursor_manager_set_cursor_image(server->xcursor, "left_ptr", server->cursor);
	}
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

static void tmbr_server_on_new_inhibitor(struct wl_listener *listener, TMBR_UNUSED void *payload)
{
	tmbr_server_t *server = wl_container_of(listener, server, inhibitor_new);
	server->inhibitors++;
	wlr_idle_set_enabled(server->idle, server->seat, !!server->inhibitors);
}

static void tmbr_server_on_destroy_inhibitor(struct wl_listener *listener, TMBR_UNUSED void *payload)
{
	tmbr_server_t *server = wl_container_of(listener, server, inhibitor_destroy);
	server->inhibitors--;
	wlr_idle_set_enabled(server->idle, server->seat, !!server->inhibitors);
}

static void tmbr_cmd_client_focus(TMBR_UNUSED struct wl_client *client, struct wl_resource *resource, unsigned selection)
{
	tmbr_server_t *server = wl_resource_get_user_data(resource);
	tmbr_client_t *focus;
	tmbr_tree_t *next;

	if ((focus = tmbr_server_find_focus(server)) == NULL ||
	    (next = tmbr_tree_find_sibling(focus->tree, selection)) == NULL)
		tmbr_return_error(resource, TMBR_CTRL_ERROR_CLIENT_NOT_FOUND, "client not found");
	tmbr_desktop_focus_client(focus->desktop, next->client, true);
}

static void tmbr_cmd_client_fullscreen(TMBR_UNUSED struct wl_client *client, struct wl_resource *resource)
{
	tmbr_server_t *server = wl_resource_get_user_data(resource);
	tmbr_client_t *focus;
	if ((focus = tmbr_server_find_focus(server)) == NULL)
		tmbr_return_error(resource, TMBR_CTRL_ERROR_CLIENT_NOT_FOUND, "client not found");
	tmbr_desktop_set_fullscreen(focus->desktop, !focus->desktop->fullscreen);
}

static void tmbr_cmd_client_kill(TMBR_UNUSED struct wl_client *client, struct wl_resource *resource)
{
	tmbr_server_t *server = wl_resource_get_user_data(resource);
	tmbr_client_t *focus;
	if ((focus = tmbr_server_find_focus(server)) == NULL)
		tmbr_return_error(resource, TMBR_CTRL_ERROR_CLIENT_NOT_FOUND, "client not found");
	tmbr_client_kill(focus);
}

static void tmbr_cmd_client_resize(TMBR_UNUSED struct wl_client *client, struct wl_resource *resource, uint32_t dir, uint32_t ratio)
{
	tmbr_server_t *server = wl_resource_get_user_data(resource);
	tmbr_client_t *focus;
	enum tmbr_ctrl_selection select;
	tmbr_split_t split;
	tmbr_tree_t *tree;
	int i;

	if ((focus = tmbr_server_find_focus(server)) == NULL)
		tmbr_return_error(resource, TMBR_CTRL_ERROR_CLIENT_NOT_FOUND, "client not found");

	switch (dir) {
	    case TMBR_CTRL_DIRECTION_NORTH:
		split = TMBR_SPLIT_HORIZONTAL; select = TMBR_CTRL_SELECTION_NEXT; i = ratio * -1; break;
	    case TMBR_CTRL_DIRECTION_SOUTH:
		split = TMBR_SPLIT_HORIZONTAL; select = TMBR_CTRL_SELECTION_PREV; i = ratio; break;
	    case TMBR_CTRL_DIRECTION_EAST:
		split = TMBR_SPLIT_VERTICAL; select = TMBR_CTRL_SELECTION_PREV; i = ratio; break;
	    case TMBR_CTRL_DIRECTION_WEST:
		split = TMBR_SPLIT_VERTICAL; select = TMBR_CTRL_SELECTION_NEXT; i = ratio * -1; break;
	    default:
		tmbr_return_error(resource, TMBR_CTRL_ERROR_INVALID_PARAM, "invalid direction");
	}

	for (tree = focus->tree; tree; tree = tree->parent) {
		if (!tree->parent)
			tmbr_return_error(resource, TMBR_CTRL_ERROR_CLIENT_NOT_FOUND, "client has no parent");
		if (tmbr_tree_get_child(tree->parent, select) != tree ||
		    tree->parent->split != split)
			continue;
		tree = tree->parent;
		break;
	}

	if ((i < 0 && i >= tree->ratio) || (i > 0 && i + tree->ratio >= 100))
		tmbr_return_error(resource, TMBR_CTRL_ERROR_INVALID_PARAM, "invalid ratio");
	tree->ratio += i;
	tmbr_desktop_recalculate(focus->desktop);
}

static void tmbr_cmd_client_swap(TMBR_UNUSED struct wl_client *client, struct wl_resource *resource, uint32_t selection)
{
	tmbr_server_t *server = wl_resource_get_user_data(resource);
	tmbr_client_t *focus;
	tmbr_tree_t *next;

	if ((focus = tmbr_server_find_focus(server)) == NULL ||
	    (next = tmbr_tree_find_sibling(focus->tree, selection)) == NULL)
		tmbr_return_error(resource, TMBR_CTRL_ERROR_CLIENT_NOT_FOUND, "client not found");
	tmbr_tree_swap(focus->tree, next);
	tmbr_desktop_recalculate(focus->desktop);
}

static void tmbr_cmd_client_to_desktop(TMBR_UNUSED struct wl_client *client, struct wl_resource *resource, uint32_t selection)
{
	tmbr_server_t *server = wl_resource_get_user_data(resource);
	tmbr_desktop_t *target;
	tmbr_client_t *focus;

	if ((focus = tmbr_server_find_focus(server)) == NULL ||
	    (target = tmbr_desktop_find_sibling(focus->desktop, selection)) == NULL)
		tmbr_return_error(resource, TMBR_CTRL_ERROR_CLIENT_NOT_FOUND, "client not found");
	tmbr_desktop_remove_client(focus->desktop, focus);
	tmbr_desktop_add_client(target, focus);
	tmbr_desktop_focus_client(target, focus, false);
}

static void tmbr_cmd_client_to_screen(TMBR_UNUSED struct wl_client *client, struct wl_resource *resource, uint32_t selection)
{
	tmbr_server_t *server = wl_resource_get_user_data(resource);
	tmbr_screen_t *screen;
	tmbr_client_t *focus;

	if ((focus = tmbr_server_find_focus(server)) == NULL ||
	    (screen = tmbr_screen_find_sibling(focus->desktop->screen, selection)) == NULL)
		tmbr_return_error(resource, TMBR_CTRL_ERROR_SCREEN_NOT_FOUND, "screen not found");
	tmbr_desktop_remove_client(focus->desktop, focus);
	tmbr_desktop_add_client(screen->focus, focus);
	tmbr_desktop_focus_client(screen->focus, focus, false);
}

static void tmbr_cmd_desktop_focus(TMBR_UNUSED struct wl_client *client, struct wl_resource *resource, uint32_t selection)
{
	tmbr_server_t *server = wl_resource_get_user_data(resource);
	tmbr_desktop_t *sibling;
	if ((sibling = tmbr_desktop_find_sibling(server->screen->focus, selection)) == NULL)
		tmbr_return_error(resource, TMBR_CTRL_ERROR_DESKTOP_NOT_FOUND, "desktop not found");
	tmbr_screen_focus_desktop(server->screen, sibling);
}

static void tmbr_cmd_desktop_kill(TMBR_UNUSED struct wl_client *client, struct wl_resource *resource)
{
	tmbr_server_t *server = wl_resource_get_user_data(resource);
	tmbr_desktop_t *desktop = server->screen->focus;
	if (desktop->clients)
		tmbr_return_error(resource, TMBR_CTRL_ERROR_DESKTOP_NOT_EMPTY, "desktop not empty");
	if (tmbr_desktop_find_sibling(desktop, TMBR_CTRL_SELECTION_NEXT) == NULL)
		tmbr_return_error(resource, TMBR_CTRL_ERROR_DESKTOP_NOT_FOUND, "desktop not found");
	tmbr_screen_remove_desktop(server->screen, desktop);
	tmbr_desktop_free(desktop);
}

static void tmbr_cmd_desktop_new(TMBR_UNUSED struct wl_client *client, struct wl_resource *resource)
{
	tmbr_server_t *server = wl_resource_get_user_data(resource);
	tmbr_screen_add_desktop(server->screen, tmbr_desktop_new());
}

static void tmbr_cmd_desktop_swap(TMBR_UNUSED struct wl_client *client, struct wl_resource *resource, uint32_t selection)
{
	tmbr_server_t *server = wl_resource_get_user_data(resource);
	tmbr_desktop_t *sibling;
	if ((sibling = tmbr_desktop_find_sibling(server->screen->focus, selection)) == NULL)
		tmbr_return_error(resource, TMBR_CTRL_ERROR_DESKTOP_NOT_FOUND, "desktop not found");
	tmbr_desktop_swap(server->screen->focus, sibling);
}

static void tmbr_cmd_screen_focus(TMBR_UNUSED struct wl_client *client, struct wl_resource *resource, uint32_t selection)
{
	tmbr_server_t *server = wl_resource_get_user_data(resource);
	tmbr_screen_t *sibling;
	if ((sibling = tmbr_screen_find_sibling(server->screen, selection)) == NULL)
		tmbr_return_error(resource, TMBR_CTRL_ERROR_SCREEN_NOT_FOUND, "screen not found");
	tmbr_screen_focus_desktop(sibling, sibling->focus);
}

static void tmbr_cmd_screen_mode(TMBR_UNUSED struct wl_client *client, struct wl_resource *resource, const char *screen, int32_t height, int32_t width, int32_t refresh)
{
	tmbr_server_t *server = wl_resource_get_user_data(resource);
	struct wlr_output_mode *mode;
	tmbr_screen_t *s;

	if ((s = tmbr_server_find_output(server, screen)) == NULL)
		tmbr_return_error(resource, TMBR_CTRL_ERROR_SCREEN_NOT_FOUND, "screen not found");
	wl_list_for_each(mode, &s->output->modes, link) {
		if (width != mode->width || height != mode->height || refresh != mode->refresh)
			continue;
		wlr_output_set_mode(s->output, mode);
		return;
	}

	tmbr_return_error(resource, TMBR_CTRL_ERROR_INVALID_PARAM, "invalid mode");
}

static void tmbr_cmd_screen_scale(TMBR_UNUSED struct wl_client *client, struct wl_resource *resource, const char *screen, uint32_t scale)
{
	tmbr_server_t *server = wl_resource_get_user_data(resource);
	tmbr_screen_t *s;
	if (scale <= 0 || scale >= 10000)
		tmbr_return_error(resource, TMBR_CTRL_ERROR_INVALID_PARAM, "invalid scale");
	if ((s = tmbr_server_find_output(server, screen)) == NULL)
		tmbr_return_error(resource, TMBR_CTRL_ERROR_SCREEN_NOT_FOUND, "screen not found");
	wlr_output_set_scale(s->output, scale / 100.0);
}

static void tmbr_cmd_tree_rotate(TMBR_UNUSED struct wl_client *client, struct wl_resource *resource)
{
	tmbr_server_t *server = wl_resource_get_user_data(resource);
	tmbr_client_t *focus;
	tmbr_tree_t *p;

	if ((focus = tmbr_server_find_focus(server)) == NULL ||
	    (p = focus->tree->parent) == NULL)
		tmbr_return_error(resource, TMBR_CTRL_ERROR_CLIENT_NOT_FOUND, "client not found");

	if (p->split == TMBR_SPLIT_HORIZONTAL) {
		tmbr_tree_t *l = p->left;
		p->left = p->right;
		p->right = l;
	}
	p->split ^= 1;
	tmbr_desktop_recalculate(focus->desktop);
}

static void tmbr_cmd_state_query(TMBR_UNUSED struct wl_client *client, TMBR_UNUSED struct wl_resource *resource, int fd)
{
	tmbr_server_t *server = wl_resource_get_user_data(resource);
	tmbr_screen_t *s;
	FILE *f;

	if ((f = fdopen(fd, "w")) == NULL)
		return;

	fprintf(f, "screens:\n");
	wl_list_for_each(s, &server->screens, link) {
		struct wlr_output_mode *mode;
		tmbr_desktop_t *d;
		double x = 0, y = 0;
		int w, h;

		wlr_output_layout_output_coords(s->server->output_layout, s->output, &x, &y);
		wlr_output_effective_resolution(s->output, &w, &h);
		fprintf(f, "- name: %s\n", s->output->name);
		fprintf(f, "  geom: {x: %u, y: %u, width: %u, height: %u}\n", (int)x, (int)y, w, h);
		fprintf(f, "  selected: %s\n", s == server->screen ? "true" : "false");
		fprintf(f, "  modes:\n");
		wl_list_for_each(mode, &s->output->modes, link)
			fprintf(f, "  - %dx%d@%d\n", mode->width, mode->height, mode->refresh);
		fprintf(f, "  desktops:\n");

		wl_list_for_each(d, &s->desktops, link) {
			tmbr_tree_t *it, *tree;

			fprintf(f, "  - selected: %s\n", d == s->focus ? "true" : "false");
			fprintf(f, "    clients:\n");

			tmbr_tree_foreach_leaf(d->clients, it, tree) {
				tmbr_client_t *c = tree->client;
				fprintf(f, "    - title: %s\n", c->surface->toplevel->title);
				fprintf(f, "      geom: {x: %u, y: %u, width: %u, height: %u}\n", c->x, c->y, c->w, c->h);
				fprintf(f, "      selected: %s\n", c == d->focus ? "true" : "false");
			}
		}
	}

	fclose(f);
}

static void tmbr_cmd_state_quit(TMBR_UNUSED struct wl_client *client, struct wl_resource *resource)
{
	tmbr_server_t *server = wl_resource_get_user_data(resource);
	wl_display_terminate(server->display);
}

static void tmbr_cmd_binding_add(TMBR_UNUSED struct wl_client *client, struct wl_resource *resource, uint32_t keycode, uint32_t modifiers, const char *command)
{
	tmbr_server_t *server = wl_resource_get_user_data(resource);
	tmbr_binding_t *binding;

	if (!keycode)
		tmbr_return_error(resource, TMBR_CTRL_ERROR_INVALID_PARAM, "invalid keycode");

	wl_list_for_each(binding, &server->bindings, link)
		if (binding->keycode == keycode && binding->modifiers == modifiers)
			break;

	if (&binding->link == &server->bindings) {
		binding = tmbr_alloc(sizeof(*binding), "Could not allocate binding");
		wl_list_insert(&server->bindings, &binding->link);
	} else {
		free(binding->command);
	}

	binding->modifiers = modifiers;
	binding->keycode = keycode;
	if ((binding->command = strdup(command)) == NULL)
		die("Could not allocate binding command");
}

static int tmbr_server_on_term(TMBR_UNUSED int signal, void *payload)
{
	tmbr_server_t *server = payload;
	wl_display_terminate(server->display);
	return 0;
}

static void tmbr_server_on_bind(struct wl_client *client, void *payload, uint32_t version, uint32_t id)
{
	static const struct tmbr_ctrl_interface impl = {
		.client_focus = tmbr_cmd_client_focus,
		.client_fullscreen = tmbr_cmd_client_fullscreen,
		.client_kill = tmbr_cmd_client_kill,
		.client_resize = tmbr_cmd_client_resize,
		.client_swap = tmbr_cmd_client_swap,
		.client_to_desktop = tmbr_cmd_client_to_desktop,
		.client_to_screen = tmbr_cmd_client_to_screen,
		.desktop_focus = tmbr_cmd_desktop_focus,
		.desktop_kill = tmbr_cmd_desktop_kill,
		.desktop_new = tmbr_cmd_desktop_new,
		.desktop_swap = tmbr_cmd_desktop_swap,
		.screen_focus = tmbr_cmd_screen_focus,
		.screen_mode = tmbr_cmd_screen_mode,
		.screen_scale = tmbr_cmd_screen_scale,
		.tree_rotate = tmbr_cmd_tree_rotate,
		.state_query = tmbr_cmd_state_query,
		.state_quit = tmbr_cmd_state_quit,
		.binding_add = tmbr_cmd_binding_add,
	};
	struct wl_resource *resource;

	if ((resource = wl_resource_create(client, &tmbr_ctrl_interface, version, id)) == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &impl, payload, NULL);
}

int tmbr_wm(void)
{
	tmbr_server_t server = { 0 };
	const char *socket;
	char *cfg;

	wl_list_init(&server.bindings);
	wl_list_init(&server.screens);
	if ((server.display = wl_display_create()) == NULL)
		die("Could not create display");
	if ((server.backend = wlr_backend_autocreate(server.display, NULL)) == NULL)
		die("Could not create backend");
	wlr_renderer_init_wl_display(wlr_backend_get_renderer(server.backend), server.display);

	if (wl_global_create(server.display, &tmbr_ctrl_interface, 1, &server, tmbr_server_on_bind) == NULL ||
	    wlr_compositor_create(server.display, wlr_backend_get_renderer(server.backend)) == NULL ||
	    wlr_data_device_manager_create(server.display) == NULL ||
	    wlr_export_dmabuf_manager_v1_create(server.display) == NULL ||
	    wlr_gamma_control_manager_v1_create(server.display) == NULL ||
	    wlr_gtk_primary_selection_device_manager_create(server.display) == NULL ||
	    wlr_primary_selection_v1_device_manager_create(server.display) == NULL ||
	    wlr_xdg_decoration_manager_v1_create(server.display) == NULL ||
	    (server.decoration = wlr_server_decoration_manager_create(server.display)) == NULL ||
	    (server.cursor = wlr_cursor_create()) == NULL ||
	    (server.seat = wlr_seat_create(server.display, "seat0")) == NULL ||
	    (server.idle = wlr_idle_create(server.display)) == NULL ||
	    (server.idle_inhibit = wlr_idle_inhibit_v1_create(server.display)) == NULL ||
	    (server.idle_timeout = wlr_idle_timeout_create(server.idle, server.seat, TMBR_SCREEN_DPMS_TIMEOUT)) == NULL ||
	    (server.output_layout = wlr_output_layout_create()) == NULL ||
	    (server.xcursor = wlr_xcursor_manager_create(getenv("XCURSOR_THEME"), 24)) == NULL ||
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
	tmbr_register(&server.idle_inhibit->events.new_inhibitor, &server.inhibitor_new, tmbr_server_on_new_inhibitor);
	tmbr_register(&server.idle_inhibit->events.destroy, &server.inhibitor_destroy, tmbr_server_on_destroy_inhibitor);

	if ((socket = wl_display_add_socket_auto(server.display)) == NULL)
		die("Could not create Wayland socket");
	setenv("WAYLAND_DISPLAY", socket, 1);

	wl_event_loop_add_signal(wl_display_get_event_loop(server.display), SIGTERM, tmbr_server_on_term, &server);

	if (!wlr_backend_start(server.backend))
		die("Could not start backend");

	if ((cfg = getenv("TMBR_CONFIG_PATH")) == NULL)
		cfg = TMBR_CONFIG_PATH;
	if (access(cfg, X_OK) == 0)
		tmbr_spawn(cfg, (char * const[]){ cfg, NULL });
	else if (errno != ENOENT)
		die("Could not execute config file: %s", strerror(errno));

	wl_display_run(server.display);
	wl_display_destroy_clients(server.display);
	wl_display_destroy(server.display);
	return 0;
}
