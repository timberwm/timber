/*
 * Copyright (C) Patrick Steinhardt, 2019-2021
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

#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

#include <wlr/version.h>
#include <wlr/backend.h>
#if WLR_VERSION_MAJOR > 0 || WLR_VERSION_MINOR > 14
# include <wlr/render/allocator.h>
#endif
#include <wlr/render/gles2.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_export_dmabuf_v1.h>
#include <wlr/types/wlr_gamma_control_v1.h>
#if WLR_VERSION_MAJOR == 0 && WLR_VERSION_MINOR < 14
# include <wlr/types/wlr_gtk_primary_selection.h>
#endif
#include <wlr/types/wlr_idle.h>
#include <wlr/types/wlr_idle_inhibit_v1.h>
#include <wlr/types/wlr_input_inhibitor.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_matrix.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_damage.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_output_management_v1.h>
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wlr/types/wlr_server_decoration.h>
#include <wlr/types/wlr_touch.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
#include <wlr/util/region.h>

#include "timber.h"
#include "timber-protocol.h"

#define tmbr_return_error(resource, code, msg) \
	do { wl_resource_post_error((resource), (code), (msg)); return; } while (0)
#define tmbr_box_scaled(vx, vy, vw, vh, s) (struct wlr_box){ .x = (vx)*(s), .y = (vy)*(s), .width = (vw)*(s), .height = (vh)*(s) }
#define tmbr_box_from_pixman(b) (struct wlr_box) { .x = (b).x1, .y = (b).y1, .width = (b).x2 - (b).x1, .height = (b).y2 - (b).y1 }
#define tmbr_box_to_pixman(b) (struct pixman_box32) { .x1 = (b).x, .x2 = (b).x + (b).width, .y1 = (b).y, .y2 = (b).y + (b).height }

#if WLR_VERSION_MAJOR == 0 && WLR_VERSION_MINOR < 13
# define WL_KEYBOARD_KEY_STATE_PRESSED WLR_KEY_PRESSED
# define wlr_backend_autocreate(backend) wlr_backend_autocreate((backend), NULL)
#endif

enum tmbr_split {
	TMBR_SPLIT_VERTICAL,
	TMBR_SPLIT_HORIZONTAL
};

struct tmbr_binding {
	struct wl_list link;

	uint32_t modifiers;
	xkb_keysym_t keycode;
	char *command;
};

struct tmbr_keyboard {
	struct tmbr_server *server;
	struct wlr_input_device *device;

	struct wl_listener destroy;
	struct wl_listener key;
	struct wl_listener modifiers;
};

struct tmbr_surface_render_data {
	struct wlr_renderer *renderer;
	struct pixman_region32 *damage;
	struct wlr_output *output;
	struct wlr_box box;
};

struct tmbr_surface_damage_data {
	struct tmbr_screen *screen;
	int x, y;
};

struct tmbr_xdg_client {
	struct tmbr_server *server;
	struct tmbr_desktop *desktop;
	struct tmbr_tree *tree;
	struct wlr_xdg_surface *surface;
	int h, w, x, y, border;
	uint32_t pending_serial;

	struct wl_event_source *configure_timer;
	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener destroy;
	struct wl_listener commit;
	struct wl_listener request_fullscreen;
	struct wl_listener new_popup;
};

struct tmbr_xdg_popup {
	struct wlr_xdg_surface *surface;
	struct tmbr_xdg_client *client;
	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener destroy;
};

struct tmbr_tree {
	struct tmbr_tree *parent;
	struct tmbr_tree *left;
	struct tmbr_tree *right;
	struct tmbr_xdg_client *client;
	enum tmbr_split split;
	uint8_t ratio;
};

struct tmbr_desktop {
	struct wl_list link;
	struct tmbr_screen *screen;
	struct tmbr_tree *clients;
	struct tmbr_xdg_client *focus;
	bool fullscreen;
};

struct tmbr_screen {
	struct wlr_box box;
	struct wl_list link;
	struct tmbr_server *server;

	struct wlr_output *output;
	struct wlr_output_damage *damage;
	struct wl_list desktops;
	struct wl_list layer_clients;
	struct tmbr_desktop *focus;

	struct wl_listener destroy;
	struct wl_listener frame;
	struct wl_listener commit;
	struct wl_listener mode;
};

struct tmbr_layer_client {
	struct wlr_layer_surface_v1 *surface;
	struct tmbr_screen *screen;
	int h, w, x, y;
	struct wl_list link;
	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener destroy;
	struct wl_listener commit;
};

struct tmbr_server {
	struct wl_display *display;
	struct wlr_renderer *renderer;
#if WLR_VERSION_MAJOR > 0 || WLR_VERSION_MINOR > 14
	struct wlr_allocator *allocator;
#endif
	struct wlr_backend *backend;
	struct wlr_cursor *cursor;
	struct wlr_idle *idle;
	struct wlr_idle_inhibit_manager_v1 *idle_inhibit;
	struct wlr_input_inhibit_manager *input_inhibit;
	struct wlr_layer_shell_v1 *layer_shell;
	struct wlr_output_layout *output_layout;
	struct wlr_output_manager_v1 *output_manager;
	struct wlr_seat *seat;
	struct wlr_server_decoration_manager *decoration;
	struct wlr_xcursor_manager *xcursor;
	struct wlr_xdg_shell *xdg_shell;

	struct wl_listener new_input;
	struct wl_listener new_output;
	struct wl_listener new_surface;
	struct wl_listener new_layer_shell_surface;
	struct wl_listener cursor_axis;
	struct wl_listener cursor_button;
	struct wl_listener cursor_motion;
	struct wl_listener cursor_motion_absolute;
	struct wl_listener cursor_touch_down;
	struct wl_listener cursor_touch_up;
	struct wl_listener cursor_frame;
	struct wl_listener request_set_cursor;
	struct wl_listener request_set_selection;
	struct wl_listener request_set_primary_selection;
	struct wl_listener seat_idle;
	struct wl_listener seat_resume;
	struct wl_listener idle_inhibitor_new;
	struct wl_listener idle_inhibitor_destroy;
	struct wl_listener apply_layout;
	int idle_inhibitors;

	struct wl_list bindings;
	struct wl_list screens;
	struct tmbr_screen *focussed_screen;
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

static struct tmbr_xdg_client *tmbr_server_find_focus(struct tmbr_server *server)
{
	return server->input_inhibit->active_client ? NULL : server->focussed_screen ? server->focussed_screen->focus->focus : NULL;
}

static void tmbr_server_update_output_layout(struct tmbr_server *server)
{
	struct wlr_output_configuration_v1 *cfg = wlr_output_configuration_v1_create();
	struct tmbr_screen *s;

	wl_list_for_each(s, &server->screens, link) {
		struct wlr_output_configuration_head_v1 *head = wlr_output_configuration_head_v1_create(cfg, s->output);
		struct wlr_box *geom = wlr_output_layout_get_box(server->output_layout, s->output);
		head->state.enabled = s->output->enabled;
		head->state.mode = s->output->current_mode;
		if (geom) {
			head->state.x = geom->x;
			head->state.y = geom->y;
		}
	}

	wlr_output_manager_v1_set_configuration(server->output_manager, cfg);
}

static struct tmbr_screen *tmbr_server_find_screen_at(struct tmbr_server *server, double x, double y)
{
	struct wlr_output *output = wlr_output_layout_output_at(server->output_layout, x, y);
	return output ? output->data : NULL;
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

static void tmbr_unregister(struct wl_listener *listener, ...)
{
	va_list ap;
	va_start(ap, listener);
	while (listener) {
		wl_list_remove(&listener->link);
		listener = va_arg(ap, struct wl_listener *);
	}
	va_end(ap);
}

static void tmbr_surface_send_frame_done(struct wlr_surface *surface, TMBR_UNUSED int sx, TMBR_UNUSED int sy, void *payload)
{
	wlr_surface_send_frame_done(surface, payload);
}

static void tmbr_surface_render(struct wlr_surface *surface, int sx, int sy, void *payload)
{
	struct tmbr_surface_render_data *data = payload;
	struct wlr_box bounds = data->box, extents = {
		.x = bounds.x + sx * data->output->scale, .y = bounds.y + sy * data->output->scale,
		.width = surface->current.width * data->output->scale, .height = surface->current.height * data->output->scale,
	};
	struct wlr_texture *texture;
	struct pixman_region32 damage;
	struct pixman_box32 *rects;
	float matrix[9];
	int i, nrects;

	pixman_region32_init(&damage);
	pixman_region32_union_rect(&damage, &damage, extents.x, extents.y, extents.width, extents.height);
	pixman_region32_intersect_rect(&damage, &damage, bounds.x, bounds.y, bounds.width, bounds.height);
	pixman_region32_intersect(&damage, &damage, data->damage);
	if (!pixman_region32_not_empty(&damage) || (texture = wlr_surface_get_texture(surface)) == NULL)
		goto out;
	if (wlr_texture_is_gles2(texture)) {
		struct wlr_gles2_texture_attribs attribs;
		wlr_gles2_texture_get_attribs(texture, &attribs);
		glBindTexture(attribs.target, attribs.tex);
		glTexParameteri(attribs.target, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	}
	wlr_matrix_project_box(matrix, &extents, wlr_output_transform_invert(surface->current.transform), 0, data->output->transform_matrix);

	for (i = 0, rects = pixman_region32_rectangles(&damage, &nrects); i < nrects; i++) {
		wlr_renderer_scissor(data->renderer, &tmbr_box_from_pixman(rects[i]));
		wlr_render_texture_with_matrix(data->renderer, texture, matrix, 1);
	}

out:
	pixman_region32_fini(&damage);
}

static void tmbr_surface_damage_surface(struct wlr_surface *surface, int sx, int sy, void *payload)
{
	struct tmbr_surface_damage_data *data = payload;

	if (pixman_region32_not_empty(&surface->buffer_damage)) {
		struct pixman_region32 damage;
		pixman_region32_init(&damage);
		wlr_surface_get_effective_damage(surface, &damage);
		pixman_region32_translate(&damage, data->x + sx, data->y + sy);
		wlr_region_scale(&damage, &damage, data->screen->output->scale);
		wlr_output_damage_add(data->screen->damage, &damage);
		pixman_region32_fini(&damage);
	}

	if (!wl_list_empty(&surface->current.frame_callback_list))
		wlr_output_schedule_frame(data->screen->output);
}

static void tmbr_surface_notify_focus(struct wlr_surface *surface, struct wlr_surface *subsurface, struct tmbr_server *server, double x, double y)
{
	struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(server->seat);
	if (server->input_inhibit->active_client &&
	    wl_resource_get_client(surface->resource) != server->input_inhibit->active_client)
		return;
	if (surface && keyboard)
		wlr_seat_keyboard_notify_enter(server->seat, surface, keyboard->keycodes, keyboard->num_keycodes, &keyboard->modifiers);
	else
		wlr_seat_keyboard_notify_clear_focus(server->seat);

	if (subsurface) {
		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);
		wlr_seat_pointer_notify_enter(server->seat, subsurface, x, y);
		wlr_seat_pointer_notify_motion(server->seat, now.tv_sec * 1000 + now.tv_nsec / 1000000, x, y);
	} else {
		wlr_seat_pointer_notify_clear_focus(server->seat);
		wlr_xcursor_manager_set_cursor_image(server->xcursor, "left_ptr", server->cursor);
	}
}

static void tmbr_xdg_popup_damage_whole(struct tmbr_xdg_popup *p)
{
	struct tmbr_xdg_client *c = p->client;
	int popup_x = p->surface->popup->geometry.x, popup_y = p->surface->popup->geometry.y,
	    popup_w = p->surface->popup->geometry.width, popup_h = p->surface->popup->geometry.height,
#if WLR_VERSION_MAJOR > 0 || WLR_VERSION_MINOR > 14
	    surface_x = p->surface->current.geometry.x, surface_y = p->surface->current.geometry.y;
#else
	    surface_x = p->surface->geometry.x, surface_y = p->surface->geometry.y;
#endif

	if (c->desktop && c->desktop == c->desktop->screen->focus)
		wlr_output_damage_add_box(c->desktop->screen->damage, &tmbr_box_scaled(
			c->x + c->border + popup_x - surface_x, c->y + c->border + popup_y - surface_y,
			popup_w + popup_x, popup_h + popup_y, c->desktop->screen->output->scale));
}

static void tmbr_xdg_popup_on_map(struct wl_listener *listener, TMBR_UNUSED void *payload)
{
	struct tmbr_xdg_popup *popup = wl_container_of(listener, popup, map);
	tmbr_xdg_popup_damage_whole(popup);
}

static void tmbr_xdg_popup_on_unmap(struct wl_listener *listener, TMBR_UNUSED void *payload)
{
	struct tmbr_xdg_popup *popup = wl_container_of(listener, popup, unmap);
	tmbr_xdg_popup_damage_whole(popup);
}

static void tmbr_xdg_popup_on_destroy(struct wl_listener *listener, TMBR_UNUSED void *payload)
{
	struct tmbr_xdg_popup *popup = wl_container_of(listener, popup, destroy);
	tmbr_unregister(&popup->map, &popup->unmap, NULL);
	free(popup);
}

static void tmbr_xdg_client_on_new_popup(struct wl_listener *listener, void *payload)
{
	struct tmbr_xdg_client *client = wl_container_of(listener, client, new_popup);
	struct tmbr_xdg_popup *popup = tmbr_alloc(sizeof(*popup), "could not allocate XDG popup");
	popup->surface = ((struct wlr_xdg_popup *)payload)->base;
	popup->client = client;
	tmbr_register(&popup->surface->events.map, &popup->map, tmbr_xdg_popup_on_map);
	tmbr_register(&popup->surface->events.unmap, &popup->unmap, tmbr_xdg_popup_on_unmap);
	tmbr_register(&popup->surface->events.destroy, &popup->destroy, tmbr_xdg_popup_on_destroy);
}

static void tmbr_xdg_client_damage_whole(struct tmbr_xdg_client *c)
{
	if (c->desktop && c->desktop == c->desktop->screen->focus) {
		struct wlr_box box = tmbr_box_scaled(c->x, c->y, c->w, c->h, c->desktop->screen->output->scale);
		wlr_output_damage_add_box(c->desktop->screen->damage, &box);
	}
}

static void tmbr_xdg_client_kill(struct tmbr_xdg_client *client)
{
	wlr_xdg_toplevel_send_close(client->surface);
}

static void tmbr_xdg_client_render(struct tmbr_xdg_client *c, struct pixman_region32 *output_damage)
{
	struct wlr_output *output = c->desktop->screen->output;
	struct tmbr_surface_render_data payload = {
		c->server->renderer, output_damage, output, tmbr_box_scaled(c->x + c->border, c->y + c->border, c->w - 2 * c->border, c->h - 2 * c->border, output->scale),
	};
	struct pixman_box32 extents = tmbr_box_to_pixman(tmbr_box_scaled(c->x, c->y, c->w, c->h, output->scale));

	if (c->border && pixman_region32_contains_rectangle(output_damage, &extents)) {
		const float *color = (c == tmbr_server_find_focus(c->server)) ? TMBR_COLOR_ACTIVE : TMBR_COLOR_INACTIVE;
		struct pixman_region32 borders;
		struct pixman_box32 *rects;
		int i, nrects;

		pixman_region32_init_with_extents(&borders, &tmbr_box_to_pixman(payload.box));
		pixman_region32_inverse(&borders, &borders, &extents);
		pixman_region32_intersect(&borders, &borders, output_damage);

		for (i = 0, rects = pixman_region32_rectangles(&borders, &nrects); i < nrects; i++) {
			wlr_renderer_scissor(c->server->renderer, &tmbr_box_from_pixman(rects[i]));
			wlr_renderer_clear(c->server->renderer, color);
		}
		pixman_region32_fini(&borders);
	}
	if (pixman_region32_contains_rectangle(output_damage, &tmbr_box_to_pixman(payload.box)))
		wlr_xdg_surface_for_each_surface(c->surface, tmbr_surface_render, &payload);
}

static void tmbr_xdg_client_notify_focus(struct tmbr_xdg_client *client)
{
	double x = client->server->cursor->x, y = client->server->cursor->y;
	struct wlr_surface *subsurface;

	wlr_output_layout_output_coords(client->server->output_layout, client->desktop->screen->output, &x, &y);
	subsurface = wlr_xdg_surface_surface_at(client->surface, x - client->x, y - client->y, &x, &y);
	tmbr_surface_notify_focus(client->surface->surface, subsurface, client->server, x, y);
}

static int tmbr_xdg_client_handle_configure_timer(void *payload)
{
	struct tmbr_xdg_client *client = payload;
	if (client == tmbr_server_find_focus(client->server))
		tmbr_xdg_client_notify_focus(client);
	return client->pending_serial = 0;
}

static void tmbr_xdg_client_set_box(struct tmbr_xdg_client *client, int x, int y, int w, int h, int border)
{
	if (client->w != w || client->h != h || client->border != border) {
		client->pending_serial = wlr_xdg_toplevel_set_size(client->surface, w - 2 * border, h - 2 * border);
		wl_event_source_timer_update(client->configure_timer, 50);
	}
	if (client->w != w || client->h != h || client->border != border || client->x != x || client->y != y) {
		tmbr_xdg_client_damage_whole(client);
		client->w = w; client->h = h; client->x = x; client->y = y; client->border = border;
		tmbr_xdg_client_damage_whole(client);
		if (tmbr_server_find_focus(client->server) == client)
			tmbr_xdg_client_notify_focus(client);
	}
}

static void tmbr_xdg_client_focus(struct tmbr_xdg_client *client, bool focus)
{
	struct wlr_output_damage *damage = client->desktop->screen->damage;
	float scale = client->desktop->screen->output->scale;

	wlr_xdg_toplevel_set_activated(client->surface, focus);
	if (focus)
		tmbr_xdg_client_notify_focus(client);
	if ((focus && tmbr_server_find_focus(client->server) != client) ||
	    (!focus && tmbr_server_find_focus(client->server) == client)) {
		wlr_output_damage_add_box(damage, &tmbr_box_scaled(client->x, client->y, client->w, client->border, scale));
		wlr_output_damage_add_box(damage, &tmbr_box_scaled(client->x, client->y, client->border, client->h, scale));
		wlr_output_damage_add_box(damage, &tmbr_box_scaled(client->x + client->w - client->border, client->y, client->border, client->h, scale));
		wlr_output_damage_add_box(damage, &tmbr_box_scaled(client->x, client->y + client->h - client->border, client->w, client->border, scale));
	}
}

static void tmbr_xdg_client_on_destroy(struct wl_listener *listener, TMBR_UNUSED void *payload)
{
	struct tmbr_xdg_client *client = wl_container_of(listener, client, destroy);
	tmbr_unregister(&client->destroy, &client->commit, &client->map, &client->unmap, &client->new_popup, &client->request_fullscreen, NULL);
	wl_event_source_remove(client->configure_timer);
	free(client);
}

static void tmbr_xdg_client_on_commit(struct wl_listener *listener, TMBR_UNUSED void *payload)
{
	struct tmbr_xdg_client *client = wl_container_of(listener, client, commit);
#if WLR_VERSION_MAJOR > 0 || WLR_VERSION_MINOR > 14
	uint32_t serial = client->surface->current.configure_serial;
#else
	uint32_t serial = client->surface->configure_serial;
#endif

	if (client->desktop && client->desktop == client->desktop->screen->focus) {
		struct tmbr_surface_damage_data damage_data = { client->desktop->screen, client->x + client->border, client->y + client->border };
		wlr_xdg_surface_for_each_surface(client->surface, tmbr_surface_damage_surface, &damage_data);
	}
	if (client->pending_serial && client->pending_serial == serial) {
		tmbr_xdg_client_handle_configure_timer(client);
		wl_event_source_timer_update(client->configure_timer, 0);
	}
}

static struct tmbr_xdg_client *tmbr_xdg_client_new(struct tmbr_server *server, struct wlr_xdg_surface *surface)
{
	struct tmbr_xdg_client *client = tmbr_alloc(sizeof(*client), "Could not allocate client");
	client->server = server;
	client->surface = surface;
	client->configure_timer = wl_event_loop_add_timer(wl_display_get_event_loop(server->display), tmbr_xdg_client_handle_configure_timer, client);
	tmbr_register(&surface->events.destroy, &client->destroy, tmbr_xdg_client_on_destroy);
	tmbr_register(&surface->events.new_popup, &client->new_popup, tmbr_xdg_client_on_new_popup);
	tmbr_register(&surface->surface->events.commit, &client->commit, tmbr_xdg_client_on_commit);
	return client;
}

static void tmbr_tree_recalculate(struct tmbr_tree *tree, int x, int y, int w, int h)
{
	int xoff, yoff, lw, rw, lh, rh;

	if (!tree)
		return;

	if (tree->client) {
		tmbr_xdg_client_set_box(tree->client, x, y, w, h, TMBR_BORDER_WIDTH);
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

static void tmbr_tree_insert(struct tmbr_tree **tree, struct tmbr_xdg_client *client)
{
	struct tmbr_tree *l, *r, *p = *tree;

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

static struct tmbr_tree *tmbr_tree_get_child(struct tmbr_tree *tree, enum tmbr_ctrl_selection which)
{
	return (which == TMBR_CTRL_SELECTION_PREV) ? tree->left : tree->right;
}

static struct tmbr_tree *tmbr_tree_find_sibling(struct tmbr_tree *tree, enum tmbr_ctrl_selection which)
{
	enum tmbr_ctrl_selection upwards = which, downwards = !which;
	struct tmbr_tree *t = tree;

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

static void tmbr_tree_swap(struct tmbr_tree *a, struct tmbr_tree *b)
{
	struct tmbr_tree tmp = *a;
	if ((a->client = b->client)  != NULL) a->client->tree = a;
	if ((a->left = b->left)      != NULL) a->left->parent = a;
	if ((a->right = b->right)    != NULL) a->right->parent = a;
	if ((b->client = tmp.client) != NULL) b->client->tree = b;
	if ((b->left = tmp.left)     != NULL) b->left->parent = b;
	if ((b->right = tmp.right)   != NULL) b->right->parent = b;
}

#define tmbr_tree_for_each(t, n) \
	for (struct tmbr_tree *i = NULL, *n = t; (!i && n && n->client && (i = n)) || ((n = tmbr_tree_find_sibling(n, TMBR_CTRL_SELECTION_NEXT)) != NULL && ((!i && (i = n)) || i != n));)

static void tmbr_tree_remove(struct tmbr_tree **tree, struct tmbr_tree *node)
{
	if (node != *tree) {
		struct tmbr_tree *uplift = (node->parent->left == node) ?
					node->parent->right : node->parent->left;
		tmbr_tree_swap(uplift, node->parent);
		free(uplift);
	} else {
		*tree = NULL;
	}
	free(node);
}

static struct tmbr_desktop *tmbr_desktop_new(void)
{
	return tmbr_alloc(sizeof(struct tmbr_desktop), "Could not allocate desktop");
}

static void tmbr_desktop_free(struct tmbr_desktop *desktop)
{
	free(desktop);
}

static struct tmbr_desktop *tmbr_desktop_find_sibling(struct tmbr_desktop *desktop, enum tmbr_ctrl_selection which)
{
	struct wl_list *sibling;
	if ((sibling = tmbr_list_get(&desktop->screen->desktops, &desktop->link, which)) == NULL)
		return NULL;
	return wl_container_of(sibling, desktop, link);
}

static void tmbr_desktop_recalculate(struct tmbr_desktop *desktop)
{
	if (desktop->fullscreen && desktop->focus)
		tmbr_xdg_client_set_box(desktop->focus, desktop->screen->box.x, desktop->screen->box.y,
					desktop->screen->box.width, desktop->screen->box.height, 0);
	else
		tmbr_tree_recalculate(desktop->clients, desktop->screen->box.x, desktop->screen->box.y,
				      desktop->screen->box.width, desktop->screen->box.height);
}

static void tmbr_desktop_set_fullscreen(struct tmbr_desktop *desktop, bool fullscreen)
{
	if (desktop->fullscreen == fullscreen)
		return;
	desktop->fullscreen = fullscreen;
	if (desktop->focus)
		wlr_xdg_toplevel_set_fullscreen(desktop->focus->surface, fullscreen);
	tmbr_desktop_recalculate(desktop);
	wlr_output_damage_add_whole(desktop->screen->damage);
}

static void tmbr_desktop_focus_client(struct tmbr_desktop *desktop, struct tmbr_xdg_client *client, bool inputfocus)
{
	if (inputfocus) {
		struct tmbr_xdg_client *current_focus = tmbr_server_find_focus(desktop->screen->server);
		if (current_focus && current_focus != client)
			tmbr_xdg_client_focus(current_focus, false);
		if (client) {
			tmbr_xdg_client_focus(client, true);
		} else {
			wlr_seat_keyboard_notify_clear_focus(desktop->screen->server->seat);
			wlr_seat_pointer_notify_clear_focus(desktop->screen->server->seat);
		}
	}
	if (desktop->focus != client)
		tmbr_desktop_set_fullscreen(desktop, false);
	desktop->focus = client;
}

static void tmbr_desktop_add_client(struct tmbr_desktop *desktop, struct tmbr_xdg_client *client)
{
	tmbr_tree_insert(desktop->focus ? &desktop->focus->tree : &desktop->clients, client);
	client->desktop = desktop;
	tmbr_desktop_set_fullscreen(desktop, false);
	tmbr_desktop_recalculate(desktop);
}

static void tmbr_desktop_remove_client(struct tmbr_desktop *desktop, struct tmbr_xdg_client *client)
{
	if (desktop->focus == client) {
		enum tmbr_ctrl_selection sel = (client->tree->parent && client->tree->parent->left == client->tree)
			? TMBR_CTRL_SELECTION_NEXT : TMBR_CTRL_SELECTION_PREV;
		struct tmbr_tree *sibling = tmbr_tree_find_sibling(client->tree, sel);
		tmbr_desktop_focus_client(desktop, sibling ? sibling->client : NULL, true);
	}
	tmbr_tree_remove(&desktop->clients, client->tree);
	tmbr_desktop_set_fullscreen(desktop, false);
	tmbr_desktop_recalculate(desktop);
	client->desktop = NULL;
	client->tree = NULL;
}

static void tmbr_desktop_swap(struct tmbr_desktop *_a, struct tmbr_desktop *_b)
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

static void tmbr_screen_focus_desktop(struct tmbr_screen *screen, struct tmbr_desktop *desktop)
{
	if (desktop->screen != screen)
		die("Cannot focus desktop for different screen");
	if (screen->focus != desktop)
		wlr_output_damage_add_whole(screen->damage);
	if (screen->server->focussed_screen && screen->server->focussed_screen != screen)
		wlr_output_damage_add_whole(screen->server->focussed_screen->damage);
	tmbr_desktop_focus_client(desktop, desktop->focus, true);
	screen->focus = desktop;
	screen->server->focussed_screen = screen;
}

static void tmbr_screen_remove_desktop(struct tmbr_screen *screen, struct tmbr_desktop *desktop)
{
	if (desktop->clients)
		die("Cannot remove non-empty desktop");

	if (screen->focus == desktop) {
		struct tmbr_desktop *sibling;
		if ((sibling = tmbr_desktop_find_sibling(desktop, TMBR_CTRL_SELECTION_NEXT)) == NULL)
			die("Cannot remove screen's last desktop");
		tmbr_screen_focus_desktop(screen, sibling);
	}
	wl_list_remove(&desktop->link);
}

static void tmbr_screen_add_desktop(struct tmbr_screen *screen, struct tmbr_desktop *desktop)
{
	wl_list_insert(screen->focus ? &screen->focus->link : &screen->desktops, &desktop->link);
	desktop->screen = screen;
	tmbr_desktop_recalculate(desktop);
	tmbr_screen_focus_desktop(screen, desktop);
}

static struct tmbr_screen *tmbr_screen_find_sibling(struct tmbr_screen *screen, enum tmbr_ctrl_selection which)
{
	struct wl_list *sibling;
	if ((sibling = tmbr_list_get(&screen->server->screens, &screen->link, which)) == NULL)
		return NULL;
	return wl_container_of(sibling, screen, link);;
}

static struct tmbr_layer_client *tmbr_screen_find_layer_client_at(struct tmbr_screen *screen, double x, double y)
{
	struct tmbr_layer_client *c;
	for (int l = ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY; l >= ZWLR_LAYER_SHELL_V1_LAYER_TOP; l--)
		wl_list_for_each_reverse(c, &screen->layer_clients, link)
			if (c->surface->mapped && (int)c->surface->current.layer == l && c->surface->current.keyboard_interactive &&
			    c->x <= x && c->x + c->w >= c->x && c->y <= y && c->y + c->h >= y)
				return c;
	return NULL;
}

static struct tmbr_xdg_client *tmbr_screen_find_xdg_client_at(struct tmbr_screen *screen, double x, double y)
{
	if (screen->focus->fullscreen)
		return screen->focus->focus;
	tmbr_tree_for_each(screen->focus->clients, t) {
		if (t->client->x <= x && t->client->x + t->client->w >= x &&
		    t->client->y <= y && t->client->y + t->client->h >= y)
			return t->client;
	}
	return NULL;
}

static void tmbr_screen_on_destroy(struct wl_listener *listener, TMBR_UNUSED void *payload)
{
	struct tmbr_screen *screen = wl_container_of(listener, screen, destroy), *sibling = tmbr_screen_find_sibling(screen, TMBR_CTRL_SELECTION_NEXT);
	struct tmbr_desktop *desktop, *tmp;
	struct tmbr_layer_client *c, *ctmp;

	wl_list_for_each_safe(c, ctmp, &screen->layer_clients, link) {
#if WLR_VERSION_MAJOR > 0 || WLR_VERSION_MINOR > 14
		wlr_layer_surface_v1_destroy(c->surface);
#else
		wlr_layer_surface_v1_close(c->surface);
		c->screen = NULL;
#endif
	}
	if (sibling) {
		wl_list_for_each_safe(desktop, tmp, &screen->desktops, link)
			tmbr_screen_add_desktop(sibling, desktop);
		wl_list_init(&screen->desktops);
		if (screen->server->focussed_screen == screen)
			tmbr_screen_focus_desktop(sibling, sibling->focus);
	} else {
		wl_list_for_each(desktop, &screen->desktops, link) {
			tmbr_tree_for_each(desktop->clients, t)
				tmbr_desktop_remove_client(desktop, t->client);
		}
		wl_display_terminate(screen->server->display);
	}

	tmbr_server_update_output_layout(screen->server);
	tmbr_unregister(&screen->destroy, &screen->frame, &screen->mode, &screen->commit, NULL);
	wl_list_remove(&screen->link);
	free(screen);
}

static void tmbr_screen_render_layer(struct tmbr_screen *screen, struct pixman_region32 *output_damage, enum zwlr_layer_shell_v1_layer layer)
{
		struct tmbr_layer_client *c;
		wl_list_for_each(c, &screen->layer_clients, link) {
			struct tmbr_surface_render_data data = {
				c->screen->server->renderer, output_damage, screen->output, tmbr_box_scaled(c->x, c->y, c->w, c->h, screen->output->scale),
			};
			if (c->surface->current.layer == layer)
				wlr_layer_surface_v1_for_each_surface(c->surface, tmbr_surface_render, &data);
		}
}

static void tmbr_screen_on_frame(struct wl_listener *listener, TMBR_UNUSED void *payload)
{
	struct tmbr_screen *screen = wl_container_of(listener, screen, frame);
	struct wlr_renderer *renderer = screen->server->renderer;
	struct tmbr_layer_client *layer_client;
	struct pixman_region32 damage;
	struct timespec time;
	bool needs_frame;

	clock_gettime(CLOCK_MONOTONIC, &time);
	pixman_region32_init(&damage);

	tmbr_tree_for_each(screen->focus->clients, tree)
		if (tree->client->pending_serial)
			goto out;

	if (!wlr_output_damage_attach_render(screen->damage, &needs_frame, &damage))
		goto out;
	if (needs_frame) {
		wlr_renderer_begin(renderer, screen->output->width, screen->output->height);

		if (!screen->focus->focus && wl_list_empty(&screen->layer_clients)) {
			wlr_renderer_clear(renderer, (float[4]){0.3, 0.3, 0.3, 1.0});
		} else if (pixman_region32_not_empty(&damage)) {
			struct pixman_box32 *rects;
			int i, nrects;

			for (i = 0, rects = pixman_region32_rectangles(&damage, &nrects); i < nrects; i++) {
				wlr_renderer_scissor(renderer, &tmbr_box_from_pixman(rects[i]));
				wlr_renderer_clear(renderer, (float[4]){0.3, 0.3, 0.3, 1.0});
			}

			if (screen->focus->fullscreen) {
				tmbr_xdg_client_render(screen->focus->focus, &damage);
			} else {
				tmbr_screen_render_layer(screen, &damage, ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND);
				tmbr_screen_render_layer(screen, &damage, ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM);
				tmbr_tree_for_each(screen->focus->clients, tree)
					tmbr_xdg_client_render(tree->client, &damage);
				tmbr_screen_render_layer(screen, &damage, ZWLR_LAYER_SHELL_V1_LAYER_TOP);
			}
			tmbr_screen_render_layer(screen, &damage, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY);
		}

		wlr_renderer_scissor(renderer, NULL);
		wlr_output_render_software_cursors(screen->output, &damage);
		wlr_renderer_end(renderer);
		wlr_output_set_damage(screen->output, &screen->damage->current);
		wlr_output_commit(screen->output);
	} else {
		wlr_output_rollback(screen->output);
	}

out:
	tmbr_tree_for_each(screen->focus->clients, tree)
		wlr_xdg_surface_for_each_surface(tree->client->surface, tmbr_surface_send_frame_done, &time);
	wl_list_for_each(layer_client, &screen->layer_clients, link)
		wlr_layer_surface_v1_for_each_surface(layer_client->surface, tmbr_surface_send_frame_done, &time);
	pixman_region32_fini(&damage);
}

static void tmbr_layer_client_damage_whole(struct tmbr_layer_client *c)
{
	wlr_output_damage_add_box(c->screen->damage, &tmbr_box_scaled(c->x, c->y, c->w, c->h, c->screen->output->scale));
}

static void tmbr_screen_recalculate_layers(struct tmbr_screen *s, bool exclusive)
{
	for (int l = ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY; l >= ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND; l--) {
		struct tmbr_layer_client *c;

		wl_list_for_each(c, &s->layer_clients, link) {
			struct wlr_layer_surface_v1_state *state = &c->surface->current;
			struct wlr_box box = { .x = 0, .y = 0, .width = state->desired_width, .height = state->desired_height };
			struct {
				uint32_t singular_anchor, anchor_triplet;
				int *positive_axis, *negative_axis, margin;
			} edges[] = {
				{
					.singular_anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP,
					.anchor_triplet = ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT|ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT|ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP,
					.positive_axis = &s->box.y,
					.negative_axis = &s->box.height,
					.margin = state->margin.top,
				},
				{
					.singular_anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM,
					.anchor_triplet = ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT|ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT|ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM,
					.negative_axis = &s->box.height,
					.margin = state->margin.bottom,
				},
				{
					.singular_anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT,
					.anchor_triplet = ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT|ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP|ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM,
					.positive_axis = &s->box.x,
					.negative_axis = &s->box.width,
					.margin = state->margin.left,
				},
				{
					.singular_anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT,
					.anchor_triplet = ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT|ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP|ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM,
					.negative_axis = &s->box.width,
					.margin = state->margin.right,
				},
			};

			if ((int)state->layer != l || exclusive == !state->exclusive_zone)
				continue;

			switch (state->anchor & (ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT|ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT)) {
			case ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT|ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT: box.x = s->box.x, box.width = s->box.width; break;
			case ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT: box.x = s->box.x; break;
			case ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT: box.x = s->box.x + s->box.width - box.width; break;
			default: box.x = s->box.x + s->box.width / 2 - box.width / 2; break;
			}

			switch (state->anchor & (ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP|ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM)) {
			case (ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP|ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM): box.y = s->box.y, box.height = s->box.height; break;
			case ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP: box.y = s->box.y; break;
			case ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM: box.y = s->box.y + s->box.height - box.height; break;
			default: box.y = s->box.y + s->box.height / 2 - box.height / 2; break;
			}

			switch (state->anchor & (ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT|ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT)) {
			case ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT|ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT: box.x += state->margin.left; box.width -= state->margin.left + state->margin.right; break;
			case ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT: box.x += state->margin.left; break;
			case ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT: box.x += state->margin.right; break;
			}

			switch (state->anchor & (ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP|ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM)) {
			case ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP|ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM: box.y += state->margin.top; box.height -= state->margin.top + state->margin.bottom; break;
			case ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP: box.y += state->margin.top; break;
			case ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM: box.y += state->margin.bottom; break;
			}

			if (box.width < 0 || box.height < 0) {
#if WLR_VERSION_MAJOR > 0 || WLR_VERSION_MINOR > 14
				wlr_layer_surface_v1_destroy(c->surface);
#else
				wlr_layer_surface_v1_close(c->surface);
#endif
				continue;
			}

			if (c->w != box.width || c->h != box.height)
				wlr_layer_surface_v1_configure(c->surface, box.width, box.height);
			if (c->w != box.width || c->h != box.height || c->x != box.x || c->y != box.y) {
				tmbr_layer_client_damage_whole(c);
				c->w = box.width; c->h = box.height; c->x = box.x; c->y = box.y;
				tmbr_layer_client_damage_whole(c);
			}

			for (size_t i = 0; exclusive && i < sizeof(edges) / sizeof(edges[0]); i++) {
				if ((state->anchor  == edges[i].singular_anchor || state->anchor == edges[i].anchor_triplet) &&
				    state->exclusive_zone + edges[i].margin > 0) {
					if (edges[i].positive_axis)
						*edges[i].positive_axis += state->exclusive_zone + edges[i].margin;
					if (edges[i].negative_axis)
						*edges[i].negative_axis -= state->exclusive_zone + edges[i].margin;
					break;
				}
			}
		}
	}
}

static void tmbr_screen_recalculate(struct tmbr_screen *s)
{
	struct tmbr_desktop *d;

	s->box.x = s->box.y = 0;
	wlr_output_effective_resolution(s->output, &s->box.width, &s->box.height);

	tmbr_screen_recalculate_layers(s, true);
	tmbr_screen_recalculate_layers(s, false);
	wl_list_for_each(d, &s->desktops, link)
		tmbr_desktop_recalculate(d);
}

static void tmbr_screen_on_mode(struct wl_listener *listener, TMBR_UNUSED void *payload)
{
	struct tmbr_screen *screen = wl_container_of(listener, screen, mode);
	tmbr_screen_recalculate(screen);
	tmbr_server_update_output_layout(screen->server);
}

static void tmbr_screen_on_commit(struct wl_listener *listener, TMBR_UNUSED void *payload)
{
	struct tmbr_screen *screen = wl_container_of(listener, screen, commit);
#if WLR_VERSION_MAJOR > 0 || WLR_VERSION_MINOR >= 13
	struct wlr_output_event_commit *event = payload;
	if (event->committed & WLR_OUTPUT_STATE_SCALE)
		wlr_xcursor_manager_load(screen->server->xcursor, screen->output->scale);
	if (event->committed & (WLR_OUTPUT_STATE_TRANSFORM|WLR_OUTPUT_STATE_SCALE))
		tmbr_screen_recalculate(screen);
#else
	wlr_xcursor_manager_load(screen->server->xcursor, screen->output->scale);
	tmbr_screen_recalculate(screen);
#endif
	tmbr_server_update_output_layout(screen->server);
}

static struct tmbr_screen *tmbr_screen_new(struct tmbr_server *server, struct wlr_output *output)
{
	struct tmbr_screen *screen = tmbr_alloc(sizeof(*screen), "Could not allocate screen");
	screen->output = output;
	screen->server = server;
	screen->damage = wlr_output_damage_create(output);
	wl_list_init(&screen->desktops);
	wl_list_init(&screen->layer_clients);
	tmbr_screen_recalculate(screen);

	tmbr_screen_add_desktop(screen, tmbr_desktop_new());
	tmbr_register(&output->events.destroy, &screen->destroy, tmbr_screen_on_destroy);
	tmbr_register(&output->events.mode, &screen->mode, tmbr_screen_on_mode);
#if WLR_VERSION_MAJOR == 0 && WLR_VERSION_MINOR < 13
	tmbr_register(&output->events.scale, &screen->commit, tmbr_screen_on_commit);
#else
	tmbr_register(&output->events.commit, &screen->commit, tmbr_screen_on_commit);
#endif
	tmbr_register(&screen->damage->events.frame, &screen->frame, tmbr_screen_on_frame);

	return screen;
}

static void tmbr_keyboard_on_destroy(struct wl_listener *listener, TMBR_UNUSED void *payload)
{
	struct tmbr_keyboard *keyboard = wl_container_of(listener, keyboard, destroy);
	tmbr_unregister(&keyboard->destroy, &keyboard->key, &keyboard->modifiers, NULL);
	free(keyboard);
}

static void tmbr_keyboard_on_key(struct wl_listener *listener, void *payload)
{
	struct tmbr_keyboard *keyboard = wl_container_of(listener, keyboard, key);
	uint32_t modifiers = wlr_keyboard_get_modifiers(keyboard->device->keyboard);
	struct wlr_event_keyboard_key *event = payload;
	const xkb_keysym_t *keysyms;
	xkb_layout_index_t layout;
	struct tmbr_binding *binding;
	int i, n;

	wlr_idle_notify_activity(keyboard->server->idle, keyboard->server->seat);
	if (event->state != WL_KEYBOARD_KEY_STATE_PRESSED || keyboard->server->input_inhibit->active_client)
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
	struct tmbr_keyboard *keyboard = wl_container_of(listener, keyboard, modifiers);
	wlr_seat_set_keyboard(keyboard->server->seat, keyboard->device);
	wlr_seat_keyboard_notify_modifiers(keyboard->server->seat, &keyboard->device->keyboard->modifiers);
}

static void tmbr_keyboard_new(struct tmbr_server *server, struct wlr_input_device *device)
{
	struct xkb_rule_names rules = {0};
	struct xkb_context *context;
	struct xkb_keymap *keymap;
	struct tmbr_keyboard *keyboard;

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

static void tmbr_layer_client_notify_focus(struct tmbr_layer_client *c)
{
	double x = c->screen->server->cursor->x, y = c->screen->server->cursor->y;
	struct wlr_surface *surface;

	if (tmbr_server_find_focus(c->screen->server))
		tmbr_xdg_client_focus(tmbr_server_find_focus(c->screen->server), false);

	wlr_output_layout_output_coords(c->screen->server->output_layout, c->screen->output, &x, &y);
	surface = wlr_layer_surface_v1_surface_at(c->surface, x - c->x, y - c->y, &x, &y);
	tmbr_surface_notify_focus(c->surface->surface, surface, c->screen->server, x, y);
}

static void tmbr_layer_client_on_map(struct wl_listener *listener, TMBR_UNUSED void *payload)
{
	struct tmbr_layer_client *client = wl_container_of(listener, client, map);
	if ((client->surface->current.layer == ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY || client->surface->current.layer == ZWLR_LAYER_SHELL_V1_LAYER_TOP) &&
	    client->surface->current.keyboard_interactive)
		tmbr_layer_client_notify_focus(client);
}

static void tmbr_layer_client_on_unmap(struct wl_listener *listener, TMBR_UNUSED void *payload)
{
	struct tmbr_layer_client *client = wl_container_of(listener, client, unmap);
	tmbr_screen_focus_desktop(client->screen, client->screen->focus);
	tmbr_layer_client_damage_whole(client);
}

static void tmbr_layer_client_on_destroy(struct wl_listener *listener, TMBR_UNUSED void *payload)
{
	struct tmbr_layer_client *client = wl_container_of(listener, client, destroy);
	wl_list_remove(&client->link);
	tmbr_unregister(&client->map, &client->unmap, &client->destroy, &client->commit, NULL);
	if (client->screen)
		tmbr_screen_recalculate(client->screen);
	free(client);
}

static void tmbr_layer_client_on_commit(struct wl_listener *listener, TMBR_UNUSED void *payload)
{
	struct tmbr_layer_client *client = wl_container_of(listener, client, commit);
	struct wlr_layer_surface_v1_state *c = &client->surface->current,
#if WLR_VERSION_MAJOR > 0 || WLR_VERSION_MINOR > 14
					  *p = &client->surface->pending;
#else
					  *p = &client->surface->client_pending;
#endif
	if (c->anchor != p->anchor || c->exclusive_zone != p->exclusive_zone || c->desired_width != p->desired_width ||
	    c->desired_height != p->desired_height || c->layer != p->layer || memcmp(&c->margin, &p->margin, sizeof(c->margin)))
		tmbr_screen_recalculate(client->screen);
	wlr_layer_surface_v1_for_each_surface(client->surface, tmbr_surface_damage_surface,
					      &(struct tmbr_surface_damage_data){ client->screen, client->x, client->y });
}

static void tmbr_server_on_new_input(struct wl_listener *listener, void *payload)
{
	struct tmbr_server *server = wl_container_of(listener, server, new_input);
	struct wlr_input_device *device = payload;

	switch (device->type) {
	case WLR_INPUT_DEVICE_POINTER:
	case WLR_INPUT_DEVICE_TOUCH:
	case WLR_INPUT_DEVICE_TABLET_TOOL:
	case WLR_INPUT_DEVICE_TABLET_PAD:
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
	struct tmbr_server *server = wl_container_of(listener, server, new_output);
	struct wlr_output *output = payload;
	struct wlr_output_mode *mode;
	struct tmbr_screen *screen;

#if WLR_VERSION_MAJOR > 0 || WLR_VERSION_MINOR > 14
	wlr_output_init_render(output, server->allocator, server->renderer);
#endif

	wlr_output_enable(output, true);
	if ((mode = wlr_output_preferred_mode(output)) != NULL)
		wlr_output_set_mode(output, mode);
	wlr_output_layout_add_auto(server->output_layout, output);
	if (!wlr_output_commit(output))
		return;

	screen = tmbr_screen_new(server, output);
	wl_list_insert(&server->screens, &screen->link);
	if (!server->focussed_screen)
		server->focussed_screen = screen;
	output->data = screen;
	tmbr_server_update_output_layout(screen->server);
}

static void tmbr_server_on_request_fullscreen(struct wl_listener *listener, void *payload)
{
	struct wlr_xdg_toplevel_set_fullscreen_event *event = payload;
	struct tmbr_xdg_client *client = wl_container_of(listener, client, request_fullscreen);
	if (client->desktop && client == tmbr_server_find_focus(client->server))
		tmbr_desktop_set_fullscreen(client->desktop, event->fullscreen);
}

static void tmbr_server_on_map(struct wl_listener *listener, TMBR_UNUSED void *payload)
{
	struct tmbr_xdg_client *client = wl_container_of(listener, client, map);
	tmbr_desktop_add_client(client->server->focussed_screen->focus, client);
	tmbr_desktop_focus_client(client->server->focussed_screen->focus, client, true);
}

static void tmbr_server_on_unmap(struct wl_listener *listener, TMBR_UNUSED void *payload)
{
	struct tmbr_xdg_client *client = wl_container_of(listener, client, unmap);
	if (client->desktop)
		tmbr_desktop_remove_client(client->desktop, client);
}

static void tmbr_server_on_new_surface(struct wl_listener *listener, void *payload)
{
	struct tmbr_server *server = wl_container_of(listener, server, new_surface);
	struct wlr_xdg_surface *surface = payload;

	if (surface->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL) {
		struct tmbr_xdg_client *client = tmbr_xdg_client_new(server, surface);
		tmbr_register(&surface->events.map, &client->map, tmbr_server_on_map);
		tmbr_register(&surface->events.unmap, &client->unmap, tmbr_server_on_unmap);
		tmbr_register(&surface->toplevel->events.request_fullscreen, &client->request_fullscreen, tmbr_server_on_request_fullscreen);
	}
}

static void tmbr_server_on_new_layer_shell_surface(struct wl_listener *listener, void *payload)
{
	struct tmbr_server *server = wl_container_of(listener, server, new_layer_shell_surface);
	struct wlr_layer_surface_v1 *surface = payload;
	struct wlr_layer_surface_v1_state current_state = surface->current;
	struct tmbr_layer_client *client;

	if (!surface->output)
		surface->output = server->focussed_screen->output;
	client = tmbr_alloc(sizeof(*client), "Could not allocate layer shell client");
	client->surface = surface;
	client->screen = surface->output->data;

	wl_list_insert(&client->screen->layer_clients, &client->link);

	tmbr_register(&surface->events.map, &client->map, tmbr_layer_client_on_map);
	tmbr_register(&surface->events.unmap, &client->unmap, tmbr_layer_client_on_unmap);
	tmbr_register(&surface->events.destroy, &client->destroy, tmbr_layer_client_on_destroy);
	tmbr_register(&surface->surface->events.commit, &client->commit, tmbr_layer_client_on_commit);

#if WLR_VERSION_MAJOR > 0 || WLR_VERSION_MINOR > 14
	surface->current = surface->pending;
#else
	surface->current = surface->client_pending;
#endif
	tmbr_screen_recalculate(client->screen);
	surface->current = current_state;
}

static void tmbr_cursor_on_axis(struct wl_listener *listener, void *payload)
{
	struct tmbr_server *server = wl_container_of(listener, server, cursor_axis);
	struct wlr_event_pointer_axis *event = payload;
	wlr_idle_notify_activity(server->idle, server->seat);
	wlr_seat_pointer_notify_axis(server->seat, event->time_msec, event->orientation,
				     event->delta, event->delta_discrete, event->source);
}

static void tmbr_cursor_on_button(struct wl_listener *listener, void *payload)
{
	struct tmbr_server *server = wl_container_of(listener, server, cursor_button);
	struct wlr_event_pointer_button *event = payload;
	wlr_idle_notify_activity(server->idle, server->seat);
	wlr_seat_pointer_notify_button(server->seat, event->time_msec, event->button, event->state);
}

static void tmbr_cursor_on_frame(struct wl_listener *listener, TMBR_UNUSED void *payload)
{
	struct tmbr_server *server = wl_container_of(listener, server, cursor_frame);
	wlr_seat_pointer_notify_frame(server->seat);
}

static void tmbr_cursor_handle_motion(struct tmbr_server *server)
{
	struct tmbr_layer_client *layer_client = NULL;
	struct tmbr_xdg_client *xdg_client = NULL;
	struct tmbr_screen *screen = NULL;
	double x = server->cursor->x, y = server->cursor->y;

	wlr_idle_notify_activity(server->idle, server->seat);
	if (server->input_inhibit->active_client)
		return;

	if ((screen = tmbr_server_find_screen_at(server, x, y)) == NULL)
		return;
	server->focussed_screen = screen;
	wlr_output_layout_output_coords(server->output_layout, screen->output, &x, &y);

	if ((layer_client = tmbr_screen_find_layer_client_at(screen, x, y)) != NULL)
		tmbr_layer_client_notify_focus(layer_client);
	else if ((xdg_client = tmbr_screen_find_xdg_client_at(screen, x, y)) != NULL)
		tmbr_desktop_focus_client(screen->focus, xdg_client, true);
}

static void tmbr_cursor_on_motion(struct wl_listener *listener, void *payload)
{
	struct tmbr_server *server = wl_container_of(listener, server, cursor_motion);
	struct wlr_event_pointer_motion *event = payload;
	wlr_cursor_move(server->cursor, event->device, event->delta_x, event->delta_y);
	tmbr_cursor_handle_motion(server);
}

static void tmbr_cursor_on_motion_absolute(struct wl_listener *listener, void *payload)
{
	struct tmbr_server *server = wl_container_of(listener, server, cursor_motion_absolute);
	struct wlr_event_pointer_motion_absolute *event = payload;
	wlr_cursor_warp_absolute(server->cursor, event->device, event->x, event->y);
	tmbr_cursor_handle_motion(server);
}

static void tmbr_cursor_on_touch_down(struct wl_listener *listener, void *payload)
{
	struct tmbr_server *server = wl_container_of(listener, server, cursor_touch_down);
	struct wlr_event_touch_down *event = payload;
	struct tmbr_layer_client *layer_client = NULL;
	struct tmbr_xdg_client *xdg_client = NULL;
	struct wlr_surface *surface = NULL;
	struct tmbr_screen *screen;
	double x, y, sx, sy;

	wlr_idle_notify_activity(server->idle, server->seat);
	if (server->input_inhibit->active_client)
		return;

	wlr_cursor_absolute_to_layout_coords(server->cursor, event->device, event->x, event->y, &x, &y);
	if ((screen = tmbr_server_find_screen_at(server, server->cursor->x, server->cursor->y)) == NULL)
		return;

	if ((layer_client = tmbr_screen_find_layer_client_at(screen, x, y)) != NULL)
		surface = wlr_layer_surface_v1_surface_at(layer_client->surface, x - layer_client->x, y - layer_client->y, &sx, &sy);
	else if ((xdg_client = tmbr_screen_find_xdg_client_at(screen, x, y)) != NULL)
		surface = wlr_xdg_surface_surface_at(xdg_client->surface, x - xdg_client->x, y - xdg_client->y, &sx, &sy);
	if (surface)
		wlr_seat_touch_notify_down(server->seat, surface, event->time_msec, event->touch_id, sx, sy);
}

static void tmbr_cursor_on_touch_up(struct wl_listener *listener, void *payload)
{
	struct tmbr_server *server = wl_container_of(listener, server, cursor_touch_up);
	struct wlr_event_touch_up *event = payload;
	wlr_idle_notify_activity(server->idle, server->seat);
	wlr_seat_touch_notify_up(server->seat, event->time_msec, event->touch_id);
}

static void tmbr_server_on_request_set_cursor(struct wl_listener *listener, void *payload)
{
	struct tmbr_server *server = wl_container_of(listener, server, request_set_cursor);
	struct wlr_seat_pointer_request_set_cursor_event *event = payload;
	struct wlr_seat_client *focused_client = server->seat->pointer_state.focused_client;
	if (focused_client == event->seat_client)
		wlr_cursor_set_surface(server->cursor, event->surface, event->hotspot_x, event->hotspot_y);
}

static void tmbr_server_on_request_set_selection(struct wl_listener *listener, void *payload)
{
	struct tmbr_server *server = wl_container_of(listener, server, request_set_selection);
	struct wlr_seat_request_set_selection_event *event = payload;
	wlr_seat_set_selection(server->seat, event->source, event->serial);
}

static void tmbr_server_on_request_set_primary_selection(struct wl_listener *listener, void *payload)
{
	struct tmbr_server *server = wl_container_of(listener, server, request_set_primary_selection);
	struct wlr_seat_request_set_primary_selection_event *event = payload;
	wlr_seat_set_primary_selection(server->seat, event->source, event->serial);
}

static void tmbr_server_on_destroy_idle_inhibitor(struct wl_listener *listener, TMBR_UNUSED void *payload)
{
	struct tmbr_server *server = wl_container_of(listener, server, idle_inhibitor_destroy);
	wlr_idle_set_enabled(server->idle, server->seat, !--server->idle_inhibitors);
}

static void tmbr_server_on_new_idle_inhibitor(struct wl_listener *listener, void *payload)
{
	struct tmbr_server *server = wl_container_of(listener, server, idle_inhibitor_new);
	struct wlr_idle_inhibitor_v1 *inhibitor = payload;
	tmbr_register(&inhibitor->events.destroy, &server->idle_inhibitor_destroy, tmbr_server_on_destroy_idle_inhibitor);
	wlr_idle_set_enabled(server->idle, server->seat, !++server->idle_inhibitors);
}

static void tmbr_server_on_apply_layout(struct wl_listener *listener, void *payload)
{
	struct tmbr_server *server = wl_container_of(listener, server, apply_layout);
	struct wlr_output_configuration_v1 *cfg = payload;
	struct wlr_output_configuration_head_v1 *head;
	bool successful = true;

	wl_list_for_each(head, &cfg->heads, link) {
		struct tmbr_screen *s = head->state.output->data;
		wlr_output_enable(s->output, head->state.enabled);
		if (head->state.enabled) {
			if (head->state.mode != NULL)
				wlr_output_set_mode(s->output, head->state.mode);
			else
				wlr_output_set_custom_mode(s->output, head->state.custom_mode.width, head->state.custom_mode.height,
							   head->state.custom_mode.refresh / 1000.0);
			wlr_output_set_transform(s->output, head->state.transform);
			wlr_output_set_scale(s->output, head->state.scale);
			wlr_output_layout_add(s->server->output_layout, s->output, head->state.x, head->state.y);
		}

		successful &= wlr_output_commit(s->output);
	}

	if (successful)
		wlr_output_configuration_v1_send_succeeded(cfg);
	else
		wlr_output_configuration_v1_send_failed(cfg);
	wlr_output_configuration_v1_destroy(cfg);
}

static void tmbr_cmd_client_focus(TMBR_UNUSED struct wl_client *client, struct wl_resource *resource, unsigned selection)
{
	struct tmbr_server *server = wl_resource_get_user_data(resource);
	struct tmbr_xdg_client *focus;
	struct tmbr_tree *next;

	if ((focus = tmbr_server_find_focus(server)) == NULL ||
	    (next = tmbr_tree_find_sibling(focus->tree, selection)) == NULL)
		tmbr_return_error(resource, TMBR_CTRL_ERROR_CLIENT_NOT_FOUND, "client not found");
	tmbr_desktop_focus_client(focus->desktop, next->client, true);
}

static void tmbr_cmd_client_fullscreen(TMBR_UNUSED struct wl_client *client, struct wl_resource *resource)
{
	struct tmbr_server *server = wl_resource_get_user_data(resource);
	struct tmbr_xdg_client *focus;
	if ((focus = tmbr_server_find_focus(server)) == NULL)
		tmbr_return_error(resource, TMBR_CTRL_ERROR_CLIENT_NOT_FOUND, "client not found");
	tmbr_desktop_set_fullscreen(focus->desktop, !focus->desktop->fullscreen);
}

static void tmbr_cmd_client_kill(TMBR_UNUSED struct wl_client *client, struct wl_resource *resource)
{
	struct tmbr_server *server = wl_resource_get_user_data(resource);
	struct tmbr_xdg_client *focus;
	if ((focus = tmbr_server_find_focus(server)) == NULL)
		tmbr_return_error(resource, TMBR_CTRL_ERROR_CLIENT_NOT_FOUND, "client not found");
	tmbr_xdg_client_kill(focus);
}

static void tmbr_cmd_client_resize(TMBR_UNUSED struct wl_client *client, struct wl_resource *resource, uint32_t dir, uint32_t ratio)
{
	struct tmbr_server *server = wl_resource_get_user_data(resource);
	struct tmbr_xdg_client *focus;
	enum tmbr_ctrl_selection select;
	enum tmbr_split split;
	struct tmbr_tree *tree;
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
	struct tmbr_server *server = wl_resource_get_user_data(resource);
	struct tmbr_xdg_client *focus;
	struct tmbr_tree *next;

	if ((focus = tmbr_server_find_focus(server)) == NULL ||
	    (next = tmbr_tree_find_sibling(focus->tree, selection)) == NULL)
		tmbr_return_error(resource, TMBR_CTRL_ERROR_CLIENT_NOT_FOUND, "client not found");
	tmbr_tree_swap(focus->tree, next);
	tmbr_desktop_recalculate(focus->desktop);
}

static void tmbr_cmd_client_to_desktop(TMBR_UNUSED struct wl_client *client, struct wl_resource *resource, uint32_t selection)
{
	struct tmbr_server *server = wl_resource_get_user_data(resource);
	struct tmbr_desktop *target;
	struct tmbr_xdg_client *focus;

	if ((focus = tmbr_server_find_focus(server)) == NULL ||
	    (target = tmbr_desktop_find_sibling(focus->desktop, selection)) == NULL)
		tmbr_return_error(resource, TMBR_CTRL_ERROR_CLIENT_NOT_FOUND, "client not found");
	tmbr_desktop_remove_client(focus->desktop, focus);
	tmbr_desktop_add_client(target, focus);
	tmbr_desktop_focus_client(target, focus, false);
}

static void tmbr_cmd_client_to_screen(TMBR_UNUSED struct wl_client *client, struct wl_resource *resource, uint32_t selection)
{
	struct tmbr_server *server = wl_resource_get_user_data(resource);
	struct tmbr_screen *screen;
	struct tmbr_xdg_client *focus;

	if ((focus = tmbr_server_find_focus(server)) == NULL ||
	    (screen = tmbr_screen_find_sibling(focus->desktop->screen, selection)) == NULL)
		tmbr_return_error(resource, TMBR_CTRL_ERROR_SCREEN_NOT_FOUND, "screen not found");
	tmbr_desktop_remove_client(focus->desktop, focus);
	tmbr_desktop_add_client(screen->focus, focus);
	tmbr_desktop_focus_client(screen->focus, focus, false);
}

static void tmbr_cmd_desktop_focus(TMBR_UNUSED struct wl_client *client, struct wl_resource *resource, uint32_t selection)
{
	struct tmbr_server *server = wl_resource_get_user_data(resource);
	struct tmbr_desktop *sibling;
	if ((sibling = tmbr_desktop_find_sibling(server->focussed_screen->focus, selection)) == NULL)
		tmbr_return_error(resource, TMBR_CTRL_ERROR_DESKTOP_NOT_FOUND, "desktop not found");
	tmbr_screen_focus_desktop(server->focussed_screen, sibling);
}

static void tmbr_cmd_desktop_kill(TMBR_UNUSED struct wl_client *client, struct wl_resource *resource)
{
	struct tmbr_server *server = wl_resource_get_user_data(resource);
	struct tmbr_desktop *desktop = server->focussed_screen->focus;
	if (desktop->clients)
		tmbr_return_error(resource, TMBR_CTRL_ERROR_DESKTOP_NOT_EMPTY, "desktop not empty");
	if (tmbr_desktop_find_sibling(desktop, TMBR_CTRL_SELECTION_NEXT) == NULL)
		tmbr_return_error(resource, TMBR_CTRL_ERROR_DESKTOP_NOT_FOUND, "desktop not found");
	tmbr_screen_remove_desktop(server->focussed_screen, desktop);
	tmbr_desktop_free(desktop);
}

static void tmbr_cmd_desktop_new(TMBR_UNUSED struct wl_client *client, struct wl_resource *resource)
{
	struct tmbr_server *server = wl_resource_get_user_data(resource);
	tmbr_screen_add_desktop(server->focussed_screen, tmbr_desktop_new());
}

static void tmbr_cmd_desktop_swap(TMBR_UNUSED struct wl_client *client, struct wl_resource *resource, uint32_t selection)
{
	struct tmbr_server *server = wl_resource_get_user_data(resource);
	struct tmbr_desktop *sibling;
	if ((sibling = tmbr_desktop_find_sibling(server->focussed_screen->focus, selection)) == NULL)
		tmbr_return_error(resource, TMBR_CTRL_ERROR_DESKTOP_NOT_FOUND, "desktop not found");
	tmbr_desktop_swap(server->focussed_screen->focus, sibling);
}

static void tmbr_cmd_screen_focus(TMBR_UNUSED struct wl_client *client, struct wl_resource *resource, uint32_t selection)
{
	struct tmbr_server *server = wl_resource_get_user_data(resource);
	struct tmbr_screen *sibling;
	if ((sibling = tmbr_screen_find_sibling(server->focussed_screen, selection)) == NULL)
		tmbr_return_error(resource, TMBR_CTRL_ERROR_SCREEN_NOT_FOUND, "screen not found");
	tmbr_screen_focus_desktop(sibling, sibling->focus);
}

static void tmbr_cmd_tree_rotate(TMBR_UNUSED struct wl_client *client, struct wl_resource *resource)
{
	struct tmbr_server *server = wl_resource_get_user_data(resource);
	struct tmbr_xdg_client *focus;
	struct tmbr_tree *p;

	if ((focus = tmbr_server_find_focus(server)) == NULL ||
	    (p = focus->tree->parent) == NULL)
		tmbr_return_error(resource, TMBR_CTRL_ERROR_CLIENT_NOT_FOUND, "client not found");

	if (p->split == TMBR_SPLIT_HORIZONTAL) {
		struct tmbr_tree *l = p->left;
		p->left = p->right;
		p->right = l;
	}
	p->split ^= 1;
	tmbr_desktop_recalculate(focus->desktop);
}

static void tmbr_cmd_state_query(TMBR_UNUSED struct wl_client *client, TMBR_UNUSED struct wl_resource *resource, int fd)
{
	struct tmbr_server *server = wl_resource_get_user_data(resource);
	struct tmbr_screen *s;
	FILE *f;

	if ((f = fdopen(fd, "w")) == NULL)
		return;

	fprintf(f, "screens:\n");
	wl_list_for_each(s, &server->screens, link) {
		struct wlr_output_mode *mode;
		struct tmbr_desktop *d;
		double x = 0, y = 0;

		wlr_output_layout_output_coords(s->server->output_layout, s->output, &x, &y);
		fprintf(f, "- name: %s\n", s->output->name);
		fprintf(f, "  geom: {x: %u, y: %u, width: %u, height: %u}\n", (int)x, (int)y, s->box.width, s->box.height);
		fprintf(f, "  selected: %s\n", s == server->focussed_screen ? "true" : "false");
		fprintf(f, "  modes:\n");
		wl_list_for_each(mode, &s->output->modes, link)
			fprintf(f, "  - %dx%d@%d\n", mode->width, mode->height, mode->refresh);
		fprintf(f, "  desktops:\n");

		wl_list_for_each(d, &s->desktops, link) {
			fprintf(f, "  - selected: %s\n", d == s->focus ? "true" : "false");
			fprintf(f, "    clients:\n");
			tmbr_tree_for_each(d->clients, tree) {
				struct tmbr_xdg_client *c = tree->client;
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
	struct tmbr_server *server = wl_resource_get_user_data(resource);
	wl_display_terminate(server->display);
}

static void tmbr_cmd_binding_add(TMBR_UNUSED struct wl_client *client, struct wl_resource *resource, uint32_t keycode, uint32_t modifiers, const char *command)
{
	struct tmbr_server *server = wl_resource_get_user_data(resource);
	struct tmbr_binding *binding;

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
	struct tmbr_server *server = payload;
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
	struct tmbr_server server = { 0 };
	const char *socket;
	char *cfg;

	wl_list_init(&server.bindings);
	wl_list_init(&server.screens);
	if ((server.display = wl_display_create()) == NULL)
		die("Could not create display");
	if ((server.backend = wlr_backend_autocreate(server.display)) == NULL)
		die("Could not create backend");
#if WLR_VERSION_MAJOR == 0 && WLR_VERSION_MINOR > 14
	if ((server.renderer = wlr_renderer_autocreate(server.backend)) == NULL)
		die("Could not create renderer");
	if ((server.allocator = wlr_allocator_autocreate(server.backend, server.renderer)) == NULL)
		die("Could not create allocator");
#else
	if ((server.renderer = wlr_backend_get_renderer(server.backend)) == NULL)
		die("Could not create renderer");
#endif
	wlr_renderer_init_wl_display(server.renderer, server.display);

	if (wl_global_create(server.display, &tmbr_ctrl_interface, tmbr_ctrl_interface.version, &server, tmbr_server_on_bind) == NULL ||
	    wlr_compositor_create(server.display, server.renderer) == NULL ||
	    wlr_data_device_manager_create(server.display) == NULL ||
	    wlr_export_dmabuf_manager_v1_create(server.display) == NULL ||
	    wlr_gamma_control_manager_v1_create(server.display) == NULL ||
#if WLR_VERSION_MAJOR == 0 && WLR_VERSION_MINOR < 14
	    wlr_gtk_primary_selection_device_manager_create(server.display) == NULL ||
#endif
	    wlr_primary_selection_v1_device_manager_create(server.display) == NULL ||
	    wlr_xdg_decoration_manager_v1_create(server.display) == NULL ||
	    (server.decoration = wlr_server_decoration_manager_create(server.display)) == NULL ||
	    (server.cursor = wlr_cursor_create()) == NULL ||
	    (server.seat = wlr_seat_create(server.display, "seat0")) == NULL ||
	    (server.idle = wlr_idle_create(server.display)) == NULL ||
	    (server.idle_inhibit = wlr_idle_inhibit_v1_create(server.display)) == NULL ||
	    (server.input_inhibit = wlr_input_inhibit_manager_create(server.display)) == NULL ||
	    (server.layer_shell = wlr_layer_shell_v1_create(server.display)) == NULL ||
	    (server.output_layout = wlr_output_layout_create()) == NULL ||
	    (server.output_manager = wlr_output_manager_v1_create(server.display)) == NULL ||
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
	tmbr_register(&server.layer_shell->events.new_surface, &server.new_layer_shell_surface, tmbr_server_on_new_layer_shell_surface);
	tmbr_register(&server.seat->events.request_set_cursor, &server.request_set_cursor, tmbr_server_on_request_set_cursor);
	tmbr_register(&server.seat->events.request_set_selection, &server.request_set_selection, tmbr_server_on_request_set_selection);
	tmbr_register(&server.seat->events.request_set_primary_selection, &server.request_set_primary_selection, tmbr_server_on_request_set_primary_selection);
	tmbr_register(&server.cursor->events.axis, &server.cursor_axis, tmbr_cursor_on_axis);
	tmbr_register(&server.cursor->events.button, &server.cursor_button, tmbr_cursor_on_button);
	tmbr_register(&server.cursor->events.motion, &server.cursor_motion, tmbr_cursor_on_motion);
	tmbr_register(&server.cursor->events.motion_absolute, &server.cursor_motion_absolute, tmbr_cursor_on_motion_absolute);
	tmbr_register(&server.cursor->events.touch_down, &server.cursor_touch_down, tmbr_cursor_on_touch_down);
	tmbr_register(&server.cursor->events.touch_up, &server.cursor_touch_up, tmbr_cursor_on_touch_up);
	tmbr_register(&server.cursor->events.frame, &server.cursor_frame, tmbr_cursor_on_frame);
	tmbr_register(&server.idle_inhibit->events.new_inhibitor, &server.idle_inhibitor_new, tmbr_server_on_new_idle_inhibitor);
	tmbr_register(&server.output_manager->events.apply, &server.apply_layout, tmbr_server_on_apply_layout);

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

	signal(SIGPIPE, SIG_IGN);

	wl_display_run(server.display);
	wl_display_destroy_clients(server.display);
	wl_display_destroy(server.display);
	return 0;
}
