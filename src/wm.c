/*
 * Copyright (C) Patrick Steinhardt, 2019-2023
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
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <string.h>
#include <unistd.h>

#include <linux/input-event-codes.h>

#include <wlr/backend.h>
#include <wlr/backend/session.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_data_control_v1.h>
#include <wlr/types/wlr_export_dmabuf_v1.h>
#include <wlr/types/wlr_fractional_scale_v1.h>
#include <wlr/types/wlr_gamma_control_v1.h>
#include <wlr/types/wlr_idle_inhibit_v1.h>
#include <wlr/types/wlr_idle_notify_v1.h>
#include <wlr/types/wlr_input_inhibitor.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_output_management_v1.h>
#include <wlr/types/wlr_output_power_management_v1.h>
#include <wlr/types/wlr_presentation_time.h>
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wlr/types/wlr_relative_pointer_v1.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_server_decoration.h>
#include <wlr/types/wlr_single_pixel_buffer_v1.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_touch.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/types/wlr_virtual_keyboard_v1.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/types/wlr_xdg_shell.h>

#include "timber.h"
#include "timber-protocol.h"

#define tmbr_return_error(resource, code, msg) \
	do { wl_resource_post_error((resource), (code), (msg)); return; } while (0)

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
	struct wlr_keyboard *keyboard;

	struct wl_listener destroy;
	struct wl_listener key;
	struct wl_listener modifiers;
};

enum tmbr_client_type {
	TMBR_CLIENT_XDG_SURFACE,
	TMBR_CLIENT_LAYER_SURFACE,
};

struct tmbr_client {
	enum tmbr_client_type type;

	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener destroy;
	struct wl_listener commit;
};

struct tmbr_xdg_client {
	struct tmbr_client base;

	struct tmbr_server *server;
	struct tmbr_desktop *desktop;
	struct tmbr_tree *tree;
	struct wlr_xdg_surface *surface;
	struct wlr_scene_tree *scene_client;
	struct wlr_scene_tree *scene_xdg_surface;
	struct wlr_scene_rect *scene_borders;
	int h, w, border;
	uint32_t pending_serial;

	struct wl_event_source *configure_timer;
	struct wl_listener request_fullscreen;
	struct wl_listener request_maximize;
};

struct tmbr_tree {
	struct tmbr_tree *parent;
	struct tmbr_tree *left;
	struct tmbr_tree *right;
	struct tmbr_xdg_client *client;
	enum tmbr_split split;
	uint8_t ratio;
};

struct tmbr_xdg_popup {
	struct wlr_xdg_popup *popup;
	struct tmbr_xdg_client *xdg_parent;
	struct tmbr_layer_client *layer_parent;

	bool has_committed;

	struct wl_listener commit;
	struct wl_listener destroy;
};

struct tmbr_desktop {
	struct wl_list link;
	struct tmbr_output *output;
	struct tmbr_tree *clients;
	struct tmbr_xdg_client *focus;
	struct wlr_scene_tree *scene_desktop;
	struct wlr_scene_tree *scene_clients;
	struct wlr_scene_tree *scene_fullscreen;
	bool fullscreen;
};

enum tmbr_scene_layer {
	TMBR_SCENE_LAYER_BACKGROUND,
	TMBR_SCENE_LAYER_BOTTOM,
	TMBR_SCENE_LAYER_DESKTOP,
	TMBR_SCENE_LAYER_TOP,
	TMBR_SCENE_LAYER_FULLSCREEN,
	TMBR_SCENE_LAYER_OVERLAY,
	TMBR_SCENE_LAYER_MAX,
};

struct tmbr_output {
	struct wlr_box full_area, usable_area;
	struct wl_list link;
	struct tmbr_server *server;

	struct wlr_output *output;
	struct wlr_scene_tree *scene_output;
	struct wlr_scene_tree *scene_layers[TMBR_SCENE_LAYER_MAX];
	struct wl_list desktops;
	struct wl_list layer_clients;
	struct tmbr_desktop *focus;

	struct wl_listener destroy;
	struct wl_listener frame;
	struct wl_listener commit;
	struct wl_listener request_state;
};

struct tmbr_layer_client {
	struct tmbr_client base;

	struct tmbr_output *output;
	struct wlr_scene_layer_surface_v1 *scene_layer_surface;
	struct wl_list link;
};

struct tmbr_server {
	struct wl_display *display;
	struct wlr_renderer *renderer;
	struct wlr_allocator *allocator;
	struct wlr_backend *backend;
	struct wlr_cursor *cursor;
	struct wlr_gamma_control_manager_v1 *gamma_control_manager;
	struct wlr_idle_inhibit_manager_v1 *idle_inhibit;
	struct wlr_idle_notifier_v1 *idle_notifier;
	struct wlr_input_inhibit_manager *input_inhibit;
	struct wlr_layer_shell_v1 *layer_shell;
	struct wlr_output_layout *output_layout;
	struct wlr_output_manager_v1 *output_manager;
	struct wlr_output_power_manager_v1 *output_power_manager;
	struct wlr_presentation *presentation;
	struct wlr_relative_pointer_manager_v1 *relative_pointer_manager;
	struct wlr_scene *scene;
	struct wlr_scene_output_layout *scene_layout;
	struct wlr_scene_tree *scene_unowned_clients;
	struct wlr_scene_tree *scene_drag;
	struct wlr_seat *seat;
	struct wlr_session *session;
	struct wlr_server_decoration_manager *decoration;
	struct wlr_virtual_keyboard_manager_v1 *virtual_keyboard_manager;
	struct wlr_xcursor_manager *xcursor_manager;
	struct wlr_xdg_shell *xdg_shell;

	struct wl_listener new_input;
	struct wl_listener new_virtual_keyboard;
	struct wl_listener new_output;
	struct wl_listener new_surface;
	struct wl_listener new_layer_shell_surface;
	struct wl_listener cursor_axis;
	struct wl_listener cursor_button;
	struct wl_listener cursor_motion_relative;
	struct wl_listener cursor_motion_absolute;
	struct wl_listener cursor_touch_down;
	struct wl_listener cursor_touch_up;
	struct wl_listener cursor_touch_motion;
	struct wl_listener cursor_frame;
	struct wl_listener request_set_cursor;
	struct wl_listener request_set_selection;
	struct wl_listener request_set_primary_selection;
	struct wl_listener request_start_drag;
	struct wl_listener start_drag;
	struct wl_listener destroy_drag_icon;
	struct wl_listener idle_inhibitor_new;
	struct wl_listener idle_inhibitor_destroy;
	struct wl_listener apply_layout;
	struct wl_listener set_gamma;
	struct wl_listener output_power_set_mode;
	int touch_emulation_id;

	struct wl_list bindings;
	struct wl_list outputs;
	struct tmbr_output *focussed_output;
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
		if (open("/dev/null", O_RDONLY) != STDIN_FILENO ||
		    open("/dev/null", O_WRONLY) != STDOUT_FILENO ||
		    open("/dev/null", O_WRONLY) != STDERR_FILENO)
			die("Could not redirect standard streams");

		if (fork() == 0 && execv(path, argv) < 0)
			die("Could not execute '%s': %s", path, strerror(errno));

		_exit(0);
	}

	waitpid(pid, NULL, 0);
}

static struct tmbr_xdg_client *tmbr_server_find_focus(struct tmbr_server *server)
{
	return server->input_inhibit->active_client ? NULL : server->focussed_output ? server->focussed_output->focus->focus : NULL;
}

static struct tmbr_client *tmbr_server_find_client_at(struct tmbr_server *server, double x, double y, struct wlr_surface **subsurface, double *sx, double *sy)
{
	struct wlr_scene_node *node = wlr_scene_node_at(&server->scene->tree.node, x, y, sx, sy);
	struct wlr_scene_surface *scene_surface;

	if (!node || node->type != WLR_SCENE_NODE_BUFFER)
		return NULL;

	scene_surface = wlr_scene_surface_try_from_buffer(wlr_scene_buffer_from_node(node));
	if (!scene_surface)
		return NULL;
	*subsurface = scene_surface->surface;

	for (struct wlr_scene_tree *p = node->parent; p; p = p->node.parent)
		if (p->node.data)
			return p->node.data;
	return NULL;
}

static void tmbr_server_update_output_layout(struct tmbr_server *server)
{
	struct wlr_output_configuration_v1 *cfg = wlr_output_configuration_v1_create();
	struct tmbr_output *o;

	wl_list_for_each(o, &server->outputs, link) {
		struct wlr_output_configuration_head_v1 *head = wlr_output_configuration_head_v1_create(cfg, o->output);
		struct wlr_box geom;

		wlr_output_layout_get_box(server->output_layout, o->output, &geom);
		head->state.enabled = o->output->enabled;
		head->state.mode = o->output->current_mode;
		head->state.x = geom.x;
		head->state.y = geom.y;
		head->state.adaptive_sync_enabled = o->output->adaptive_sync_status;
	}

	wlr_output_manager_v1_set_configuration(server->output_manager, cfg);
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

static void tmbr_surface_notify_focus(struct wlr_surface *surface, struct wlr_surface *subsurface, struct tmbr_server *server, double x, double y, bool adjust_keyboard_focus)
{
	if (server->input_inhibit->active_client &&
	    wl_resource_get_client(surface->resource) != server->input_inhibit->active_client)
		return;

	if (adjust_keyboard_focus) {
		struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(server->seat);
		if (surface && keyboard)
			wlr_seat_keyboard_notify_enter(server->seat, surface, keyboard->keycodes, keyboard->num_keycodes, &keyboard->modifiers);
		else
			wlr_seat_keyboard_notify_clear_focus(server->seat);
	}

	if (subsurface) {
		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);
		wlr_seat_pointer_notify_enter(server->seat, subsurface, x, y);
		wlr_seat_pointer_notify_motion(server->seat, now.tv_sec * 1000 + now.tv_nsec / 1000000, x, y);
	} else {
		wlr_seat_pointer_notify_clear_focus(server->seat);
		wlr_cursor_set_xcursor(server->cursor, server->xcursor_manager, "default");
	}
}

static void tmbr_xdg_client_kill(struct tmbr_xdg_client *client)
{
	wlr_xdg_toplevel_send_close(client->surface->toplevel);
}

static void tmbr_xdg_client_notify_focus(struct tmbr_xdg_client *client)
{
	double x = client->server->cursor->x, y = client->server->cursor->y;
	struct wlr_surface *subsurface;
	struct tmbr_client *found;

	if (!client->desktop)
		die("Focus notification for client without desktop");

	if ((found = tmbr_server_find_client_at(client->server, x, y, &subsurface, &x, &y)) != NULL &&
	    found->type == TMBR_CLIENT_XDG_SURFACE && wl_container_of(found, (struct tmbr_xdg_client *) NULL, base) == client)
		tmbr_surface_notify_focus(client->surface->surface, subsurface, client->server, x, y, true);
	else
		tmbr_surface_notify_focus(client->surface->surface, NULL, client->server, 0, 0, true);
}

static int tmbr_xdg_client_handle_configure_timer(void *payload)
{
	((struct tmbr_xdg_client *) payload)->pending_serial = 0;
	return 0;
}

static void tmbr_xdg_client_set_box(struct tmbr_xdg_client *client, int x, int y, int w, int h, int border)
{
	if (client->w != w || client->h != h || client->border != border) {
		client->pending_serial = wlr_xdg_toplevel_set_size(client->surface->toplevel, w - 2 * border, h - 2 * border);
		wl_event_source_timer_update(client->configure_timer, 50);
		client->w = w; client->h = h; client->border = border;
	}

	wlr_scene_node_set_position(&client->scene_xdg_surface->node, x + border, y + border);
	wlr_scene_rect_set_size(client->scene_borders, w, h);
	wlr_scene_node_set_position(&client->scene_borders->node, x, y);

	wlr_scene_subsurface_tree_set_clip(&client->scene_xdg_surface->node, &(struct wlr_box){
		.x = client->surface->pending.geometry.x,
		.y = client->surface->pending.geometry.y,
		.width = w - 2 * border,
		.height = h - 2 * border,
	});
}

static void tmbr_xdg_client_focus(struct tmbr_xdg_client *client, bool focus)
{
	wlr_xdg_toplevel_set_activated(client->surface->toplevel, focus);
	wlr_scene_rect_set_color(client->scene_borders, focus ? TMBR_COLOR_ACTIVE : TMBR_COLOR_INACTIVE);
	if (focus)
		tmbr_xdg_client_notify_focus(client);
}

static void tmbr_xdg_client_on_destroy(struct wl_listener *listener, TMBR_UNUSED void *payload)
{
	struct tmbr_xdg_client *client = wl_container_of(listener, client, base.destroy);
	tmbr_unregister(&client->base.destroy, &client->base.commit, &client->base.map, &client->base.unmap, &client->request_fullscreen, &client->request_maximize, NULL);
	wlr_scene_node_destroy(&client->scene_client->node);
	wl_event_source_remove(client->configure_timer);
	free(client);
}

static void tmbr_xdg_client_on_commit(struct wl_listener *listener, TMBR_UNUSED void *payload)
{
	struct tmbr_xdg_client *client = wl_container_of(listener, client, base.commit);

	/*
	 * Some clients may shift around their surfaces on commit, which thus
	 * requires us to re-calculate the clipping area. Most commonly this
	 * seems to happen for clients that normally render Client-Side Decorations.
	 */
	 wlr_scene_subsurface_tree_set_clip(&client->scene_xdg_surface->node,
	 &(struct wlr_box){
		.x = client->surface->current.geometry.x,
		.y = client->surface->current.geometry.y,
		.width = client->w - 2 * client->border,
		.height = client->h - 2 * client->border,
	});

	 /*
	  * If the client is currently focussed then we re-focus it here. A
	  * commit may indicate that the client has reconfigured its geometry,
	  * and thus the previous cursor's position as known by the client may
	  * not be accurate anymore.
	  */
	if (client == tmbr_server_find_focus(client->server))
		tmbr_xdg_client_notify_focus(client);

	/*
	 * The client has finished a resize. We thus unset its pending serial
	 * and disarm the timer so that we start emitting new frames again.
	 */
	if (client->pending_serial && client->pending_serial == client->surface->current.configure_serial) {
		tmbr_xdg_client_handle_configure_timer(client);
		wl_event_source_timer_update(client->configure_timer, 0);
	}
}

static struct tmbr_xdg_client *tmbr_xdg_client_new(struct tmbr_server *server, struct wlr_xdg_surface *surface)
{
	struct tmbr_xdg_client *client = tmbr_alloc(sizeof(*client), "Could not allocate client");

	client->base.type = TMBR_CLIENT_XDG_SURFACE;
	client->server = server;
	client->surface = surface;
	client->configure_timer = wl_event_loop_add_timer(wl_display_get_event_loop(server->display), tmbr_xdg_client_handle_configure_timer, client);
	client->scene_client = wlr_scene_tree_create(server->scene_unowned_clients);
	client->scene_xdg_surface = wlr_scene_xdg_surface_create(client->scene_client, surface);
	client->scene_xdg_surface->node.data = client;
	client->scene_borders = wlr_scene_rect_create(client->scene_client, 0, 0, TMBR_COLOR_INACTIVE);
	wlr_scene_node_place_below(&client->scene_borders->node, &client->scene_xdg_surface->node);
	if (wl_resource_get_version(surface->toplevel->resource) >= XDG_TOPLEVEL_STATE_TILED_LEFT_SINCE_VERSION)
		wlr_xdg_toplevel_set_tiled(surface->toplevel, WLR_EDGE_LEFT|WLR_EDGE_RIGHT|WLR_EDGE_TOP|WLR_EDGE_BOTTOM);
	else
		wlr_xdg_toplevel_set_maximized(surface->toplevel, true);
	wlr_xdg_toplevel_set_wm_capabilities(surface->toplevel, WLR_XDG_TOPLEVEL_WM_CAPABILITIES_FULLSCREEN);

	surface->data = client;

	tmbr_register(&surface->events.destroy, &client->base.destroy, tmbr_xdg_client_on_destroy);
	tmbr_register(&surface->surface->events.commit, &client->base.commit, tmbr_xdg_client_on_commit);
	return client;
}

static void tmbr_xdg_popup_on_destroy(struct wl_listener *listener, TMBR_UNUSED void *payload)
{
	struct tmbr_xdg_popup *popup = wl_container_of(listener, popup, destroy);
	tmbr_unregister(&popup->destroy, &popup->commit, NULL);
	free(popup);
}

static void tmbr_xdg_popup_on_commit(struct wl_listener *listener, TMBR_UNUSED void *payload)
{
	struct tmbr_xdg_popup *popup = wl_container_of(listener, popup, commit);

	if (!popup->has_committed) {
		if (popup->xdg_parent) {
			/*
			 * Constrain XDG client popups to the window of their parent.
			 */
			wlr_xdg_popup_unconstrain_from_box(popup->popup, &(struct wlr_box){
				.width = popup->xdg_parent->w,
				.height = popup->xdg_parent->h,
			});
		} else if (popup->layer_parent) {
			int x, y;

			wlr_scene_node_coords(&popup->layer_parent->scene_layer_surface->tree->node, &x, &y);

			/*
			 * Layer shell clients are unconstrained to the complete output.
			 */
			wlr_xdg_popup_unconstrain_from_box(popup->popup, &(struct wlr_box) {
				.width = popup->layer_parent->output->full_area.width,
				.height = popup->layer_parent->output->full_area.height,
				.x = popup->layer_parent->output->full_area.x - x,
				.y = popup->layer_parent->output->full_area.y - y,
			});
		}

		popup->has_committed = true;
	}
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

static struct tmbr_desktop *tmbr_desktop_new(struct tmbr_output *parent)
{
	struct tmbr_desktop *desktop = tmbr_alloc(sizeof(struct tmbr_desktop), "Could not allocate desktop");
	desktop->scene_desktop = wlr_scene_tree_create(parent->scene_layers[TMBR_SCENE_LAYER_DESKTOP]);
	desktop->scene_clients = wlr_scene_tree_create(desktop->scene_desktop);
	desktop->scene_fullscreen = wlr_scene_tree_create(parent->scene_layers[TMBR_SCENE_LAYER_FULLSCREEN]);
	wlr_scene_node_set_enabled(&desktop->scene_fullscreen->node, false);
	return desktop;
}

static void tmbr_desktop_free(struct tmbr_desktop *desktop)
{
	wlr_scene_node_destroy(&desktop->scene_desktop->node);
	wlr_scene_node_destroy(&desktop->scene_fullscreen->node);
	free(desktop);
}

static struct tmbr_desktop *tmbr_desktop_find_sibling(struct tmbr_desktop *desktop, enum tmbr_ctrl_selection which)
{
	struct wl_list *sibling;
	if ((sibling = tmbr_list_get(&desktop->output->desktops, &desktop->link, which)) == NULL)
		return NULL;
	return wl_container_of(sibling, desktop, link);
}

static void tmbr_desktop_recalculate(struct tmbr_desktop *desktop)
{
	struct tmbr_xdg_client *focus;

	if (desktop->fullscreen && desktop->focus)
		tmbr_xdg_client_set_box(desktop->focus, desktop->output->full_area.x, desktop->output->full_area.y,
					desktop->output->full_area.width, desktop->output->full_area.height, 0);
	else
		tmbr_tree_recalculate(desktop->clients, desktop->output->usable_area.x, desktop->output->usable_area.y,
				      desktop->output->usable_area.width, desktop->output->usable_area.height);

	/*
	 * After recalculating the layout it may happen that the pointer's
	 * position changes relative to the currently-focussed window. In that
	 * case we want to send the client a new focus notification to tell it
	 * about the updated cursor position.
	 */
	if ((focus = tmbr_server_find_focus(desktop->output->server)) != NULL)
		tmbr_xdg_client_notify_focus(focus);
}

static void tmbr_desktop_set_fullscreen(struct tmbr_desktop *desktop, bool fullscreen)
{
	if (desktop->fullscreen == fullscreen)
		return;
	desktop->fullscreen = fullscreen;
	wlr_scene_node_set_enabled(&desktop->scene_clients->node, !fullscreen);
	wlr_scene_node_set_enabled(&desktop->scene_fullscreen->node, fullscreen);
	if (desktop->focus) {
		wlr_xdg_toplevel_set_fullscreen(desktop->focus->surface->toplevel, fullscreen);
		wlr_scene_node_reparent(&desktop->focus->scene_client->node, fullscreen ? desktop->scene_fullscreen : desktop->scene_clients);
	}
	tmbr_desktop_recalculate(desktop);
}

static void tmbr_desktop_focus_client(struct tmbr_desktop *desktop, struct tmbr_xdg_client *client, bool inputfocus)
{
	if (desktop->focus != client)
		tmbr_desktop_set_fullscreen(desktop, false);
	if (inputfocus) {
		struct tmbr_xdg_client *current_focus = tmbr_server_find_focus(desktop->output->server);
		if (current_focus && current_focus != client)
			tmbr_xdg_client_focus(current_focus, false);
		if (client) {
			tmbr_xdg_client_focus(client, true);
		} else {
			wlr_seat_keyboard_notify_clear_focus(desktop->output->server->seat);
			wlr_seat_pointer_notify_clear_focus(desktop->output->server->seat);
		}
	}
	desktop->focus = client;
}

static void tmbr_desktop_add_client(struct tmbr_desktop *desktop, struct tmbr_xdg_client *client)
{
	tmbr_tree_insert(desktop->focus ? &desktop->focus->tree : &desktop->clients, client);
	wlr_scene_node_reparent(&client->scene_client->node, desktop->scene_clients);
	client->desktop = desktop;
	tmbr_desktop_set_fullscreen(desktop, false);
	tmbr_desktop_recalculate(desktop);
}

static void tmbr_desktop_remove_client(struct tmbr_desktop *desktop, struct tmbr_xdg_client *client)
{
	if (client->desktop != desktop || !client->tree)
		die("Trying to remove client not assigned to the desktop");

	if (desktop->focus == client) {
		enum tmbr_ctrl_selection sel = (client->tree->parent && client->tree->parent->left == client->tree)
			? TMBR_CTRL_SELECTION_NEXT : TMBR_CTRL_SELECTION_PREV;
		struct tmbr_tree *sibling = tmbr_tree_find_sibling(client->tree, sel);
		tmbr_desktop_focus_client(desktop, sibling ? sibling->client : NULL,
					  tmbr_server_find_focus(desktop->output->server) == client);
	}

	wlr_scene_node_reparent(&client->scene_client->node, client->server->scene_unowned_clients);
	tmbr_tree_remove(&desktop->clients, client->tree);
	tmbr_desktop_set_fullscreen(desktop, false);
	tmbr_desktop_recalculate(desktop);

	client->desktop = NULL;
	client->tree = NULL;
}

static void tmbr_desktop_swap(struct tmbr_desktop *_a, struct tmbr_desktop *_b)
{
	struct wl_list *a = &_a->link, *b = &_b->link, *pos = b->prev;

	if (_a->output != _b->output)
		die("Cannot swap desktops of different outputs");

	wl_list_remove(b);
	wl_list_insert(a->prev, b);
	if (pos == a)
		pos = b;
	wl_list_remove(a);
	wl_list_insert(pos, a);
}

static void tmbr_output_focus_desktop(struct tmbr_output *output, struct tmbr_desktop *desktop)
{
	if (desktop->output != output)
		die("Cannot focus desktop for different output");
	if (output->focus && output->focus != desktop) {
		wlr_scene_node_set_enabled(&output->focus->scene_desktop->node, false);
		wlr_scene_node_set_enabled(&output->focus->scene_fullscreen->node, false);
	}
	tmbr_desktop_focus_client(desktop, desktop->focus, true);
	wlr_scene_node_set_enabled(&desktop->scene_desktop->node, true);
	wlr_scene_node_set_enabled(&desktop->scene_fullscreen->node, true);
	output->focus = desktop;
	output->server->focussed_output = output;
}

static void tmbr_output_remove_desktop(struct tmbr_output *output, struct tmbr_desktop *desktop)
{
	if (desktop->clients)
		die("Cannot remove non-empty desktop");

	if (output->focus == desktop) {
		struct tmbr_desktop *sibling;
		if ((sibling = tmbr_desktop_find_sibling(desktop, TMBR_CTRL_SELECTION_NEXT)) == NULL)
			die("Cannot remove output's last desktop");
		tmbr_output_focus_desktop(output, sibling);
	}
	wl_list_remove(&desktop->link);
}

static void tmbr_output_add_desktop(struct tmbr_output *output, struct tmbr_desktop *desktop)
{
	wl_list_insert(output->focus ? &output->focus->link : &output->desktops, &desktop->link);
	wlr_scene_node_reparent(&desktop->scene_desktop->node, output->scene_layers[TMBR_SCENE_LAYER_DESKTOP]);
	wlr_scene_node_reparent(&desktop->scene_fullscreen->node, output->scene_layers[TMBR_SCENE_LAYER_FULLSCREEN]);
	desktop->output = output;
	tmbr_desktop_recalculate(desktop);
	tmbr_output_focus_desktop(output, desktop);
}

static struct tmbr_output *tmbr_output_find_sibling(struct tmbr_output *output, enum tmbr_ctrl_selection which)
{
	struct wl_list *sibling;
	if ((sibling = tmbr_list_get(&output->server->outputs, &output->link, which)) == NULL)
		return NULL;
	return wl_container_of(sibling, output, link);;
}

static void tmbr_output_on_destroy(struct wl_listener *listener, TMBR_UNUSED void *payload)
{
	struct tmbr_output *output = wl_container_of(listener, output, destroy), *sibling = tmbr_output_find_sibling(output, TMBR_CTRL_SELECTION_NEXT);
	struct tmbr_desktop *desktop, *tmp;
	struct tmbr_layer_client *c, *ctmp;

	wl_list_for_each_safe(c, ctmp, &output->layer_clients, link) {
		wlr_layer_surface_v1_destroy(c->scene_layer_surface->layer_surface);
	}
	if (sibling) {
		wl_list_for_each_safe(desktop, tmp, &output->desktops, link)
			tmbr_output_add_desktop(sibling, desktop);
		wl_list_init(&output->desktops);
		if (output->server->focussed_output == output)
			tmbr_output_focus_desktop(sibling, sibling->focus);
	} else {
		wl_list_for_each_safe(desktop, tmp, &output->desktops, link) {
			struct tmbr_tree *t;
			while ((t = tmbr_tree_find_sibling(desktop->clients, TMBR_CTRL_SELECTION_NEXT)) != NULL)
				tmbr_desktop_remove_client(desktop, t->client);
			free(desktop);
		}
		wl_display_terminate(output->server->display);
	}

	wlr_scene_node_destroy(&output->scene_output->node);
	tmbr_server_update_output_layout(output->server);
	tmbr_unregister(&output->destroy, &output->frame, &output->commit, &output->request_state, NULL);
	wl_list_remove(&output->link);
	free(output);
}

static void tmbr_output_on_frame(struct wl_listener *listener, TMBR_UNUSED void *payload)
{
	struct tmbr_output *output = wl_container_of(listener, output, frame);
	struct wlr_scene_output *scene_output = wlr_scene_get_scene_output(output->server->scene, output->output);
	struct timespec time;

	tmbr_tree_for_each(output->focus->clients, tree)
		if (tree->client->pending_serial)
			goto out;
	wlr_scene_output_commit(scene_output, NULL);

out:
	clock_gettime(CLOCK_MONOTONIC, &time);
	wlr_scene_output_send_frame_done(scene_output, &time);
}

static void tmbr_output_on_request_state(struct wl_listener *listener, void *payload)
{
	struct tmbr_output *output = wl_container_of(listener, output, request_state);
	struct wlr_output_event_request_state *event = payload;
	wlr_output_commit_state(output->output, event->state);
}

static void tmbr_output_recalculate_layers(struct tmbr_output *o, bool exclusive)
{
	for (int l = ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY; l >= ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND; l--) {
		struct tmbr_layer_client *c;

		wl_list_for_each(c, &o->layer_clients, link) {
			struct wlr_layer_surface_v1_state *state = &c->scene_layer_surface->layer_surface->current;
			struct wlr_scene_tree *parent_scene;

			if ((int)state->layer != l || exclusive != (state->exclusive_zone > 0))
				continue;

			switch (state->layer) {
			case ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND: parent_scene = o->scene_layers[TMBR_SCENE_LAYER_BACKGROUND]; break;
			case ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM: parent_scene = o->scene_layers[TMBR_SCENE_LAYER_BOTTOM]; break;
			case ZWLR_LAYER_SHELL_V1_LAYER_TOP: parent_scene = o->scene_layers[TMBR_SCENE_LAYER_TOP]; break;
			case ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY: parent_scene = o->scene_layers[TMBR_SCENE_LAYER_OVERLAY]; break;
			default:
				die("Unexpected layer");
			}
			wlr_scene_node_reparent(&c->scene_layer_surface->tree->node, parent_scene);
			wlr_scene_layer_surface_v1_configure(c->scene_layer_surface, &o->full_area, &o->usable_area);
		}
	}
}

static void tmbr_output_recalculate(struct tmbr_output *o)
{
	struct tmbr_desktop *d;

	wlr_output_layout_get_box(o->server->output_layout, o->output, &o->full_area);
	wlr_output_layout_get_box(o->server->output_layout, o->output, &o->usable_area);

	tmbr_output_recalculate_layers(o, true);
	tmbr_output_recalculate_layers(o, false);
	wl_list_for_each(d, &o->desktops, link)
		tmbr_desktop_recalculate(d);
}

static void tmbr_output_on_commit(struct wl_listener *listener, TMBR_UNUSED void *payload)
{
	struct tmbr_output *output = wl_container_of(listener, output, commit);
	struct wlr_output_event_commit *event = payload;
	if (event->state->committed & WLR_OUTPUT_STATE_SCALE)
		wlr_xcursor_manager_load(output->server->xcursor_manager, output->output->scale);
	if (event->state->committed & (WLR_OUTPUT_STATE_TRANSFORM|WLR_OUTPUT_STATE_SCALE|WLR_OUTPUT_STATE_MODE))
		tmbr_output_recalculate(output);
	tmbr_server_update_output_layout(output->server);
}

static struct tmbr_output *tmbr_output_new(struct tmbr_server *server, struct wlr_output *wlr_output)
{
	struct tmbr_output *output = tmbr_alloc(sizeof(*output), "Could not allocate output");
	output->output = wlr_output;
	output->server = server;
	output->scene_output = wlr_scene_tree_create(&server->scene->tree);
	for (unsigned i = 0; i < ARRAY_SIZE(output->scene_layers); i++) {
		output->scene_layers[i] = wlr_scene_tree_create(output->scene_output);
		if (i > 0)
			wlr_scene_node_place_above(&output->scene_layers[i]->node, &output->scene_layers[i - 1]->node);
	}
	wl_list_init(&output->desktops);
	wl_list_init(&output->layer_clients);

	tmbr_output_add_desktop(output, tmbr_desktop_new(output));
	tmbr_register(&wlr_output->events.destroy, &output->destroy, tmbr_output_on_destroy);
	tmbr_register(&wlr_output->events.commit, &output->commit, tmbr_output_on_commit);
	tmbr_register(&wlr_output->events.frame, &output->frame, tmbr_output_on_frame);
	tmbr_register(&wlr_output->events.request_state, &output->request_state, tmbr_output_on_request_state);

	return output;
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
	uint32_t modifiers = wlr_keyboard_get_modifiers(keyboard->keyboard);
	struct wlr_keyboard_key_event *event = payload;
	const xkb_keysym_t *keysyms;
	xkb_layout_index_t layout;
	struct tmbr_binding *binding;
	int i, n;

	wlr_idle_notifier_v1_notify_activity(keyboard->server->idle_notifier, keyboard->server->seat);
	if (event->state != WL_KEYBOARD_KEY_STATE_PRESSED || keyboard->server->input_inhibit->active_client)
		goto unhandled;

	layout = xkb_state_key_get_layout(keyboard->keyboard->xkb_state, event->keycode + 8);
	n = xkb_keymap_key_get_syms_by_level(keyboard->keyboard->keymap, event->keycode + 8, layout, 0, &keysyms);

	for (i = 0; i < n; i++) {
		xkb_keysym_t keysym = keysyms[i];

		if (modifiers == (WLR_MODIFIER_CTRL|WLR_MODIFIER_ALT) &&
		    keysym >= XKB_KEY_F1 && keysym <= XKB_KEY_F12) {
			wlr_session_change_vt(keyboard->server->session, keysym - XKB_KEY_F1 + 1);
			return;
		}

		wl_list_for_each(binding, &keyboard->server->bindings, link) {
			if (binding->keycode != keysym || binding->modifiers != modifiers)
				continue;
			tmbr_spawn("/bin/sh", (char * const[]){ "/bin/sh", "-c", binding->command, NULL });
			return;
		}
	}

unhandled:
	wlr_seat_set_keyboard(keyboard->server->seat, keyboard->keyboard);
	wlr_seat_keyboard_notify_key(keyboard->server->seat, event->time_msec, event->keycode, event->state);
}

static void tmbr_keyboard_on_modifiers(struct wl_listener *listener, TMBR_UNUSED void *payload)
{
	struct tmbr_keyboard *keyboard = wl_container_of(listener, keyboard, modifiers);
	wlr_seat_set_keyboard(keyboard->server->seat, keyboard->keyboard);
	wlr_seat_keyboard_notify_modifiers(keyboard->server->seat, &keyboard->keyboard->modifiers);
}

static void tmbr_keyboard_new(struct tmbr_server *server, struct wlr_keyboard *wlr_keyboard)
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
	keyboard->keyboard = wlr_keyboard;

	wlr_keyboard_set_keymap(wlr_keyboard, keymap);
	wlr_keyboard_set_repeat_info(wlr_keyboard, 25, 600);

	tmbr_register(&wlr_keyboard->base.events.destroy, &keyboard->destroy, tmbr_keyboard_on_destroy);
	tmbr_register(&wlr_keyboard->events.key, &keyboard->key, tmbr_keyboard_on_key);
	tmbr_register(&wlr_keyboard->events.modifiers, &keyboard->modifiers, tmbr_keyboard_on_modifiers);

	xkb_keymap_unref(keymap);
	xkb_context_unref(context);
}

static void tmbr_layer_client_notify_focus(struct tmbr_layer_client *c)
{
	bool adjust_keyboard_focus = c->scene_layer_surface->layer_surface->current.keyboard_interactive != ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE;
	double x = c->output->server->cursor->x, y = c->output->server->cursor->y;
	struct wlr_surface *subsurface;
	struct tmbr_client *found;

	if (adjust_keyboard_focus && tmbr_server_find_focus(c->output->server))
		tmbr_xdg_client_focus(tmbr_server_find_focus(c->output->server), false);

	if ((found = tmbr_server_find_client_at(c->output->server, x, y, &subsurface, &x, &y)) != NULL &&
	    found->type == TMBR_CLIENT_LAYER_SURFACE && wl_container_of(found, (struct tmbr_layer_client *) NULL, base) == c)
		tmbr_surface_notify_focus(c->scene_layer_surface->layer_surface->surface, subsurface, c->output->server, x, y, adjust_keyboard_focus);
	else
		tmbr_surface_notify_focus(c->scene_layer_surface->layer_surface->surface, NULL, c->output->server, 0, 0, adjust_keyboard_focus);
}

static void tmbr_layer_client_on_map(struct wl_listener *listener, TMBR_UNUSED void *payload)
{
	struct tmbr_layer_client *client = wl_container_of(listener, client, base.map);
	struct wlr_layer_surface_v1_state *state = &client->scene_layer_surface->layer_surface->current;
	if ((state->layer == ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY || state->layer == ZWLR_LAYER_SHELL_V1_LAYER_TOP) &&
	    state->keyboard_interactive)
		tmbr_layer_client_notify_focus(client);
	wlr_scene_node_set_enabled(&client->scene_layer_surface->tree->node, true);
	tmbr_output_recalculate(client->output);
}

static void tmbr_layer_client_on_unmap(struct wl_listener *listener, TMBR_UNUSED void *payload)
{
	struct tmbr_layer_client *client = wl_container_of(listener, client, base.unmap);
	tmbr_output_focus_desktop(client->output, client->output->focus);
	wlr_scene_node_set_enabled(&client->scene_layer_surface->tree->node, false);
	tmbr_output_recalculate(client->output);
}

static void tmbr_layer_client_on_destroy(struct wl_listener *listener, TMBR_UNUSED void *payload)
{
	struct tmbr_layer_client *client = wl_container_of(listener, client, base.destroy);
	wl_list_remove(&client->link);
	tmbr_unregister(&client->base.map, &client->base.unmap, &client->base.destroy, &client->base.commit, NULL);
	tmbr_output_recalculate(client->output);
	free(client);
}

static void tmbr_layer_client_on_commit(struct wl_listener *listener, TMBR_UNUSED void *payload)
{
	struct tmbr_layer_client *client = wl_container_of(listener, client, base.commit);
	struct wlr_layer_surface_v1_state *c = &client->scene_layer_surface->layer_surface->current,
					  *p = &client->scene_layer_surface->layer_surface->pending;
	if (c->anchor != p->anchor || c->exclusive_zone != p->exclusive_zone || c->desired_width != p->desired_width ||
	    c->desired_height != p->desired_height || c->layer != p->layer || memcmp(&c->margin, &p->margin, sizeof(c->margin)))
		tmbr_output_recalculate(client->output);
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
		tmbr_keyboard_new(server, wlr_keyboard_from_input_device(device));
		break;
	default:
		break;
	}

	wlr_seat_set_capabilities(server->seat, WL_SEAT_CAPABILITY_POINTER|WL_SEAT_CAPABILITY_KEYBOARD);
}

static void tmbr_server_on_new_virtual_keyboard(struct wl_listener *listener, void *payload)
{
	struct tmbr_server *server = wl_container_of(listener, server, new_virtual_keyboard);
	struct wlr_virtual_keyboard_v1 *keyboard = payload;
	tmbr_keyboard_new(server, &keyboard->keyboard);
}

static void tmbr_server_on_new_output(struct wl_listener *listener, void *payload)
{
	struct tmbr_server *server = wl_container_of(listener, server, new_output);
	struct wlr_output *wlr_output = payload;
	struct wlr_output_layout_output *layout_output;
	struct wlr_output_mode *preferred_mode;
	struct wlr_scene_output *scene_output;
	struct wlr_output_state state;
	struct tmbr_output *output;

	wlr_output_init_render(wlr_output, server->allocator, server->renderer);

	wlr_output_state_init(&state);
	wlr_output_state_set_enabled(&state, true);
	if ((preferred_mode = wlr_output_preferred_mode(wlr_output)) != NULL)
		wlr_output_state_set_mode(&state, preferred_mode);
	if (!wlr_output_commit_state(wlr_output, &state))
		return;

	output = tmbr_output_new(server, wlr_output);
	wl_list_insert(&server->outputs, &output->link);
	if (!server->focussed_output)
		server->focussed_output = output;
	wlr_output->data = output;

	layout_output = wlr_output_layout_add_auto(server->output_layout, wlr_output);
	scene_output = wlr_scene_output_create(server->scene, wlr_output);
	wlr_scene_output_layout_add_output(server->scene_layout, layout_output, scene_output);

	tmbr_output_recalculate(output);
	tmbr_server_update_output_layout(output->server);
}

static void tmbr_server_on_request_maximize(struct wl_listener *listener, TMBR_UNUSED void *payload)
{
	struct tmbr_xdg_client *client = wl_container_of(listener, client, request_maximize);
	wlr_xdg_surface_schedule_configure(client->surface);
}

static void tmbr_server_on_request_fullscreen(struct wl_listener *listener, TMBR_UNUSED void *payload)
{
	struct tmbr_xdg_client *client = wl_container_of(listener, client, request_fullscreen);
	if (client->desktop && client == tmbr_server_find_focus(client->server))
		tmbr_desktop_set_fullscreen(client->desktop, client->surface->toplevel->requested.fullscreen);
	else
		wlr_xdg_surface_schedule_configure(client->surface);
}

static void tmbr_server_on_map(struct wl_listener *listener, TMBR_UNUSED void *payload)
{
	struct tmbr_xdg_client *client = wl_container_of(listener, client, base.map);
	tmbr_desktop_add_client(client->server->focussed_output->focus, client);
	tmbr_desktop_focus_client(client->server->focussed_output->focus, client, true);
}

static void tmbr_server_on_unmap(struct wl_listener *listener, TMBR_UNUSED void *payload)
{
	struct tmbr_xdg_client *client = wl_container_of(listener, client, base.unmap);
	if (client->desktop)
		tmbr_desktop_remove_client(client->desktop, client);
}

static void tmbr_server_on_new_xdg_surface(struct wl_listener *listener, void *payload)
{
	struct tmbr_server *server = wl_container_of(listener, server, new_surface);
	struct wlr_xdg_surface *surface = payload;

	if (surface->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL) {
		struct tmbr_xdg_client *client = tmbr_xdg_client_new(server, surface);
		tmbr_register(&surface->surface->events.map, &client->base.map, tmbr_server_on_map);
		tmbr_register(&surface->surface->events.unmap, &client->base.unmap, tmbr_server_on_unmap);
		tmbr_register(&surface->toplevel->events.request_fullscreen, &client->request_fullscreen, tmbr_server_on_request_fullscreen);
		tmbr_register(&surface->toplevel->events.request_maximize, &client->request_maximize, tmbr_server_on_request_maximize);
	} else if (surface->role == WLR_XDG_SURFACE_ROLE_POPUP) {
		struct wlr_xdg_surface *xdg_surface, *root = surface;
		struct wlr_layer_surface_v1 *layer_surface;
		struct tmbr_xdg_popup *popup;

		popup = tmbr_alloc(sizeof(*popup), "Could not allocate popup");
		popup->popup = surface->popup;

		/*
		 * The popup may itself have a popup as parent, so we need to
		 * walk up the chain of surfaces until we find the root of the
		 * hierarchy.
		 */
		while (1) {
			struct wlr_xdg_surface *parent = wlr_xdg_surface_try_from_wlr_surface(root->popup->parent);
			if (!parent || parent->role != WLR_XDG_SURFACE_ROLE_POPUP)
				break;
			root = parent;
		}

		if ((xdg_surface = wlr_xdg_surface_try_from_wlr_surface(root->popup->parent))) {
			struct tmbr_xdg_client *xdg_client = xdg_surface->data;
			surface->data = wlr_scene_xdg_surface_create(xdg_client->scene_xdg_surface, surface);
			popup->xdg_parent = xdg_client;
		} else if ((layer_surface = wlr_layer_surface_v1_try_from_wlr_surface(root->popup->parent))) {
			struct tmbr_layer_client *layer_client = layer_surface->data;
			surface->data = wlr_scene_xdg_surface_create(layer_client->scene_layer_surface->tree, surface);
			popup->layer_parent = layer_client;
		}

		tmbr_register(&surface->popup->base->surface->events.commit, &popup->commit, tmbr_xdg_popup_on_commit);
		tmbr_register(&surface->popup->base->events.destroy, &popup->destroy, tmbr_xdg_popup_on_destroy);
	}
}

static void tmbr_server_on_new_layer_shell_surface(struct wl_listener *listener, void *payload)
{
	struct tmbr_server *server = wl_container_of(listener, server, new_layer_shell_surface);
	struct wlr_layer_surface_v1 *surface = payload;
	struct wlr_layer_surface_v1_state current_state = surface->current;
	struct tmbr_layer_client *client;

	if (!surface->output)
		surface->output = server->focussed_output->output;
	client = tmbr_alloc(sizeof(*client), "Could not allocate layer shell client");
	client->base.type = TMBR_CLIENT_LAYER_SURFACE;
	client->output = surface->output->data;
	client->scene_layer_surface = wlr_scene_layer_surface_v1_create(server->scene_unowned_clients, surface);
	client->scene_layer_surface->tree->node.data = client;
	wlr_scene_node_set_enabled(&client->scene_layer_surface->tree->node, false);

	surface->data = client;

	wl_list_insert(&client->output->layer_clients, &client->link);

	tmbr_register(&surface->surface->events.map, &client->base.map, tmbr_layer_client_on_map);
	tmbr_register(&surface->surface->events.unmap, &client->base.unmap, tmbr_layer_client_on_unmap);
	tmbr_register(&surface->events.destroy, &client->base.destroy, tmbr_layer_client_on_destroy);
	tmbr_register(&surface->surface->events.commit, &client->base.commit, tmbr_layer_client_on_commit);

	surface->current = surface->pending;
	tmbr_output_recalculate(client->output);
	surface->current = current_state;
}

static void tmbr_cursor_on_axis(struct wl_listener *listener, void *payload)
{
	struct tmbr_server *server = wl_container_of(listener, server, cursor_axis);
	struct wlr_pointer_axis_event *event = payload;
	wlr_idle_notifier_v1_notify_activity(server->idle_notifier, server->seat);
	wlr_seat_pointer_notify_axis(server->seat, event->time_msec, event->orientation,
				     event->delta, event->delta_discrete, event->source);
}

static void tmbr_cursor_on_button(struct wl_listener *listener, void *payload)
{
	struct tmbr_server *server = wl_container_of(listener, server, cursor_button);
	struct wlr_pointer_button_event *event = payload;
	wlr_idle_notifier_v1_notify_activity(server->idle_notifier, server->seat);
	wlr_seat_pointer_notify_button(server->seat, event->time_msec, event->button, event->state);
}

static void tmbr_cursor_on_frame(struct wl_listener *listener, TMBR_UNUSED void *payload)
{
	struct tmbr_server *server = wl_container_of(listener, server, cursor_frame);
	wlr_seat_pointer_notify_frame(server->seat);
}

static void tmbr_cursor_handle_motion(struct tmbr_server *server, struct wlr_input_device *device, uint32_t time_msec,
				      double dx, double dy, double dx_unaccel, double dy_unaccel)
{
	struct wlr_surface *surface = NULL;
	struct wlr_drag_icon *drag_icon;
	struct tmbr_client *client;
	double sx, sy;

	wlr_relative_pointer_manager_v1_send_relative_motion(server->relative_pointer_manager, server->seat,
							     (uint64_t) time_msec * 1000, dx, dy, dx_unaccel, dy_unaccel);
	wlr_cursor_move(server->cursor, device, dx, dy);
	wlr_idle_notifier_v1_notify_activity(server->idle_notifier, server->seat);
	if (server->input_inhibit->active_client)
		return;

	if (server->seat->drag && (drag_icon = server->seat->drag->icon))
		wlr_scene_node_set_position(drag_icon->data,
					    server->cursor->x + drag_icon->surface->current.dx,
					    server->cursor->y + drag_icon->surface->current.dy);

	if ((client = tmbr_server_find_client_at(server, server->cursor->x, server->cursor->y, &surface, &sx, &sy)) == NULL)
		return;
	if (client->type == TMBR_CLIENT_LAYER_SURFACE) {
		struct tmbr_layer_client *layer_client = wl_container_of(client, layer_client, base);
		tmbr_layer_client_notify_focus(layer_client);
		server->focussed_output = layer_client->output;
	} else if (client->type == TMBR_CLIENT_XDG_SURFACE) {
		struct tmbr_xdg_client *xdg_client = wl_container_of(client, xdg_client, base);
		tmbr_desktop_focus_client(xdg_client->desktop->output->focus, xdg_client, true);
		server->focussed_output = xdg_client->desktop->output;
	}
}

static void tmbr_cursor_on_motion_relative(struct wl_listener *listener, void *payload)
{
	struct tmbr_server *server = wl_container_of(listener, server, cursor_motion_relative);
	struct wlr_pointer_motion_event *event = payload;
	tmbr_cursor_handle_motion(server, &event->pointer->base, event->time_msec,
				  event->delta_x, event->delta_y, event->unaccel_dx, event->unaccel_dy);
}

static void tmbr_cursor_on_motion_absolute(struct wl_listener *listener, void *payload)
{
	struct tmbr_server *server = wl_container_of(listener, server, cursor_motion_absolute);
	struct wlr_pointer_motion_absolute_event *event = payload;
	double lx, ly;
	wlr_cursor_absolute_to_layout_coords(server->cursor, &event->pointer->base, event->x, event->y, &lx, &ly);
	tmbr_cursor_handle_motion(server, &event->pointer->base, event->time_msec, lx - server->cursor->x,
				  ly - server->cursor->y, lx - server->cursor->x, ly - server->cursor->y);
}

static void tmbr_cursor_on_touch_down(struct wl_listener *listener, void *payload)
{
	struct tmbr_server *server = wl_container_of(listener, server, cursor_touch_down);
	struct wlr_touch_down_event *event = payload;
	struct wlr_surface *surface;
	double lx, ly, sx, sy;

	wlr_idle_notifier_v1_notify_activity(server->idle_notifier, server->seat);
	if (server->input_inhibit->active_client)
		return;

	wlr_cursor_absolute_to_layout_coords(server->cursor, &event->touch->base, event->x, event->y, &lx, &ly);
	if (tmbr_server_find_client_at(server, lx, ly, &surface, &sx, &sy) && wlr_surface_accepts_touch(server->seat, surface)) {
		wlr_seat_touch_notify_down(server->seat, surface, event->time_msec, event->touch_id, sx, sy);
	} else if (!server->touch_emulation_id) {
		server->touch_emulation_id = event->touch_id;
		tmbr_cursor_handle_motion(server, &event->touch->base, event->time_msec, lx - server->cursor->x,
					  ly - server->cursor->y, lx - server->cursor->x, ly - server->cursor->y);
		wlr_seat_pointer_notify_button(server->seat, event->time_msec, BTN_LEFT, WLR_BUTTON_PRESSED);
		wlr_seat_pointer_notify_frame(server->seat);
	}
}

static void tmbr_cursor_on_touch_up(struct wl_listener *listener, void *payload)
{
	struct tmbr_server *server = wl_container_of(listener, server, cursor_touch_up);
	struct wlr_touch_up_event *event = payload;

	wlr_idle_notifier_v1_notify_activity(server->idle_notifier, server->seat);
	if (server->touch_emulation_id == event->touch_id) {
		server->touch_emulation_id = 0;
		wlr_seat_pointer_notify_button(server->seat, event->time_msec, BTN_LEFT, WLR_BUTTON_RELEASED);
		wlr_seat_pointer_notify_frame(server->seat);
	} else if (!server->touch_emulation_id) {
		wlr_seat_touch_notify_up(server->seat, event->time_msec, event->touch_id);
	}
}

static void tmbr_cursor_on_touch_motion(struct wl_listener *listener, void *payload)
{
	struct tmbr_server *server = wl_container_of(listener, server, cursor_touch_motion);
	struct wlr_touch_motion_event *event = payload;
	struct wlr_surface *surface;
	double lx, ly, sx, sy;

	wlr_idle_notifier_v1_notify_activity(server->idle_notifier, server->seat);
	if (server->input_inhibit->active_client)
		return;

	wlr_cursor_absolute_to_layout_coords(server->cursor, &event->touch->base, event->x, event->y, &lx, &ly);
	if (server->touch_emulation_id == event->touch_id)
		tmbr_cursor_handle_motion(server, &event->touch->base, event->time_msec, lx - server->cursor->x,
					  ly - server->cursor->y, lx - server->cursor->x, ly - server->cursor->y);
	else if (!server->touch_emulation_id && tmbr_server_find_client_at(server, lx, ly, &surface, &sx, &sy))
		wlr_seat_touch_notify_motion(server->seat, event->time_msec, event->touch_id, sx, sy);
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

static void tmbr_server_on_destroy_drag_icon(TMBR_UNUSED struct wl_listener *listener, void *payload)
{
	struct wlr_drag_icon *icon = payload;
	wlr_scene_node_destroy(icon->data);
}

static void tmbr_server_on_start_drag(struct wl_listener *listener, void *payload)
{
	struct tmbr_server *server = wl_container_of(listener, server, start_drag);
	struct wlr_drag *drag = payload;

	if (drag->icon) {
		drag->icon->data = wlr_scene_subsurface_tree_create(server->scene_drag, drag->icon->surface);
		tmbr_register(&drag->icon->events.destroy, &server->destroy_drag_icon, tmbr_server_on_destroy_drag_icon);
	}
}

static void tmbr_server_on_request_start_drag(struct wl_listener *listener, void *payload)
{
	struct tmbr_server *server = wl_container_of(listener, server, request_start_drag);
	struct wlr_seat_request_start_drag_event *event = payload;

	if (wlr_seat_validate_pointer_grab_serial(server->seat, event->origin, event->serial))
		wlr_seat_start_pointer_drag(server->seat, event->drag, event->serial);
	else
		wlr_data_source_destroy(event->drag->source);
}

static void tmbr_server_on_destroy_idle_inhibitor(struct wl_listener *listener, TMBR_UNUSED void *payload)
{
	struct tmbr_server *server = wl_container_of(listener, server, idle_inhibitor_destroy);
	wlr_idle_notifier_v1_set_inhibited(server->idle_notifier, wl_list_length(&server->idle_inhibit->inhibitors) > 1);
}

static void tmbr_server_on_new_idle_inhibitor(struct wl_listener *listener, void *payload)
{
	struct tmbr_server *server = wl_container_of(listener, server, idle_inhibitor_new);
	struct wlr_idle_inhibitor_v1 *inhibitor = payload;
	tmbr_register(&inhibitor->events.destroy, &server->idle_inhibitor_destroy, tmbr_server_on_destroy_idle_inhibitor);
	wlr_idle_notifier_v1_set_inhibited(server->idle_notifier, true);
}

static void tmbr_server_on_apply_layout(TMBR_UNUSED struct wl_listener *listener, void *payload)
{
	struct wlr_output_configuration_v1 *cfg = payload;
	struct wlr_output_configuration_head_v1 *head;
	bool successful = true;

	wl_list_for_each(head, &cfg->heads, link) {
		struct tmbr_output *o = head->state.output->data;
		struct wlr_output_state state;

		wlr_output_state_init(&state);
		wlr_output_state_set_enabled(&state, head->state.enabled);
		if (head->state.enabled) {
			if (head->state.mode != NULL)
				wlr_output_state_set_mode(&state, head->state.mode);
			else
				wlr_output_state_set_custom_mode(&state, head->state.custom_mode.width, head->state.custom_mode.height,
								 head->state.custom_mode.refresh / 1000.0);
			wlr_output_state_set_adaptive_sync_enabled(&state, head->state.adaptive_sync_enabled);
			wlr_output_state_set_transform(&state, head->state.transform);
			wlr_output_state_set_scale(&state, head->state.scale);
		}

		if (!wlr_output_commit_state(o->output, &state)) {
			successful = false;
			continue;
		}

		wlr_output_layout_add(o->server->output_layout, o->output, head->state.x, head->state.y);

		/*
		 * We need to explicitly recalculate the output. While mode
		 * changes et cetera would lead to a recalculation already via
		 * the `commit` event, changing the output's position in the
		 * layout goes undetected.
		 */
		tmbr_output_recalculate(o);
	}

	if (successful)
		wlr_output_configuration_v1_send_succeeded(cfg);
	else
		wlr_output_configuration_v1_send_failed(cfg);
	wlr_output_configuration_v1_destroy(cfg);
}

static void tmbr_server_on_set_gamma(struct wl_listener *listener, void *payload)
{
	struct tmbr_server *server = wl_container_of(listener, server, set_gamma);
	struct wlr_gamma_control_manager_v1_set_gamma_event *event = payload;
	struct wlr_gamma_control_v1 *control = wlr_gamma_control_manager_v1_get_control(server->gamma_control_manager, event->output);
	wlr_gamma_control_v1_apply(control, &event->output->pending);
	wlr_output_commit(event->output);
	wlr_output_schedule_frame(event->output);
}

static void tmbr_server_on_output_power_set_mode(TMBR_UNUSED struct wl_listener *listener, void *payload)
{
	struct wlr_output_power_v1_set_mode_event *event = payload;
	struct tmbr_output *output = event->output->data;
	wlr_output_enable(event->output, event->mode == ZWLR_OUTPUT_POWER_V1_MODE_ON);
	wlr_output_commit(event->output);
	wlr_scene_node_set_enabled(&output->scene_output->node, event->mode == ZWLR_OUTPUT_POWER_V1_MODE_ON);
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

	for (tree = focus->tree; ; tree = tree->parent) {
		if (!tree || !tree->parent)
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

static void tmbr_cmd_client_to_output(TMBR_UNUSED struct wl_client *client, struct wl_resource *resource, uint32_t selection)
{
	struct tmbr_server *server = wl_resource_get_user_data(resource);
	struct tmbr_output *output;
	struct tmbr_xdg_client *focus;

	if ((focus = tmbr_server_find_focus(server)) == NULL ||
	    (output = tmbr_output_find_sibling(focus->desktop->output, selection)) == NULL)
		tmbr_return_error(resource, TMBR_CTRL_ERROR_OUTPUT_NOT_FOUND, "output not found");
	tmbr_desktop_remove_client(focus->desktop, focus);
	tmbr_desktop_add_client(output->focus, focus);
	tmbr_desktop_focus_client(output->focus, focus, false);
}

static void tmbr_cmd_desktop_focus(TMBR_UNUSED struct wl_client *client, struct wl_resource *resource, uint32_t selection)
{
	struct tmbr_server *server = wl_resource_get_user_data(resource);
	struct tmbr_desktop *sibling;
	if ((sibling = tmbr_desktop_find_sibling(server->focussed_output->focus, selection)) == NULL)
		tmbr_return_error(resource, TMBR_CTRL_ERROR_DESKTOP_NOT_FOUND, "desktop not found");
	tmbr_output_focus_desktop(server->focussed_output, sibling);
}

static void tmbr_cmd_desktop_kill(TMBR_UNUSED struct wl_client *client, struct wl_resource *resource)
{
	struct tmbr_server *server = wl_resource_get_user_data(resource);
	struct tmbr_desktop *desktop = server->focussed_output->focus;
	if (desktop->clients)
		tmbr_return_error(resource, TMBR_CTRL_ERROR_DESKTOP_NOT_EMPTY, "desktop not empty");
	if (tmbr_desktop_find_sibling(desktop, TMBR_CTRL_SELECTION_NEXT) == NULL)
		tmbr_return_error(resource, TMBR_CTRL_ERROR_DESKTOP_NOT_FOUND, "desktop not found");
	tmbr_output_remove_desktop(server->focussed_output, desktop);
	tmbr_desktop_free(desktop);
}

static void tmbr_cmd_desktop_new(TMBR_UNUSED struct wl_client *client, struct wl_resource *resource)
{
	struct tmbr_server *server = wl_resource_get_user_data(resource);
	tmbr_output_add_desktop(server->focussed_output, tmbr_desktop_new(server->focussed_output));
}

static void tmbr_cmd_desktop_swap(TMBR_UNUSED struct wl_client *client, struct wl_resource *resource, uint32_t selection)
{
	struct tmbr_server *server = wl_resource_get_user_data(resource);
	struct tmbr_desktop *sibling;
	if ((sibling = tmbr_desktop_find_sibling(server->focussed_output->focus, selection)) == NULL)
		tmbr_return_error(resource, TMBR_CTRL_ERROR_DESKTOP_NOT_FOUND, "desktop not found");
	tmbr_desktop_swap(server->focussed_output->focus, sibling);
}

static void tmbr_cmd_output_focus(TMBR_UNUSED struct wl_client *client, struct wl_resource *resource, uint32_t selection)
{
	struct tmbr_server *server = wl_resource_get_user_data(resource);
	struct tmbr_output *sibling;
	if ((sibling = tmbr_output_find_sibling(server->focussed_output, selection)) == NULL)
		tmbr_return_error(resource, TMBR_CTRL_ERROR_OUTPUT_NOT_FOUND, "output not found");
	tmbr_output_focus_desktop(sibling, sibling->focus);
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
	struct tmbr_output *o;
	FILE *f;

	if ((f = fdopen(fd, "w")) == NULL)
		return;

	fprintf(f, "outputs:\n");
	wl_list_for_each(o, &server->outputs, link) {
		struct wlr_output_mode *mode;
		struct tmbr_desktop *d;

		fprintf(f, "- name: %s\n", o->output->name);
		fprintf(f, "  geom: {x: %u, y: %u, width: %u, height: %u}\n", o->full_area.x, o->full_area.y, o->full_area.width, o->full_area.height);
		fprintf(f, "  selected: %s\n", o == server->focussed_output ? "true" : "false");
		fprintf(f, "  modes:\n");
		wl_list_for_each(mode, &o->output->modes, link)
			fprintf(f, "  - %dx%d@%d\n", mode->width, mode->height, mode->refresh);
		fprintf(f, "  desktops:\n");

		wl_list_for_each(d, &o->desktops, link) {
			fprintf(f, "  - selected: %s\n", d == o->focus ? "true" : "false");
			fprintf(f, "    clients:\n");
			tmbr_tree_for_each(d->clients, tree) {
				struct tmbr_xdg_client *c = tree->client;

				fprintf(f, "    - title: '%s'\n", c->surface->toplevel->title);
				fprintf(f, "      geom: {x: %u, y: %u, width: %u, height: %u}\n",
					c->scene_client->node.x, c->scene_client->node.y, c->w, c->h);
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
		.client_to_output = tmbr_cmd_client_to_output,
		.desktop_focus = tmbr_cmd_desktop_focus,
		.desktop_kill = tmbr_cmd_desktop_kill,
		.desktop_new = tmbr_cmd_desktop_new,
		.desktop_swap = tmbr_cmd_desktop_swap,
		.output_focus = tmbr_cmd_output_focus,
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
	struct tmbr_binding *binding, *binding_tmp;
	struct wl_event_source *source;
	const char *socket;
	char *cfg;

	wl_list_init(&server.bindings);
	wl_list_init(&server.outputs);
	if ((server.display = wl_display_create()) == NULL)
		die("Could not create display");
	if ((server.backend = wlr_backend_autocreate(server.display, &server.session)) == NULL)
		die("Could not create backend");
	if ((server.renderer = wlr_renderer_autocreate(server.backend)) == NULL)
		die("Could not create renderer");
	if ((server.allocator = wlr_allocator_autocreate(server.backend, server.renderer)) == NULL)
		die("Could not create allocator");
	wlr_renderer_init_wl_display(server.renderer, server.display);

	if (wl_global_create(server.display, &tmbr_ctrl_interface, tmbr_ctrl_interface.version, &server, tmbr_server_on_bind) == NULL ||
	    wlr_compositor_create(server.display, 5, server.renderer) == NULL ||
	    wlr_subcompositor_create(server.display) == NULL ||
	    wlr_data_device_manager_create(server.display) == NULL ||
	    wlr_data_control_manager_v1_create(server.display) == NULL ||
	    wlr_export_dmabuf_manager_v1_create(server.display) == NULL ||
	    wlr_fractional_scale_manager_v1_create(server.display, 1) == NULL ||
	    wlr_primary_selection_v1_device_manager_create(server.display) == NULL ||
	    wlr_screencopy_manager_v1_create(server.display) == NULL ||
	    wlr_single_pixel_buffer_manager_v1_create(server.display) == NULL ||
	    wlr_viewporter_create(server.display) == NULL ||
	    wlr_xdg_decoration_manager_v1_create(server.display) == NULL ||
	    (server.decoration = wlr_server_decoration_manager_create(server.display)) == NULL ||
	    (server.cursor = wlr_cursor_create()) == NULL ||
	    (server.output_layout = wlr_output_layout_create()) == NULL ||
	    (server.scene = wlr_scene_create()) == NULL ||
	    (server.scene_drag = wlr_scene_tree_create(&server.scene->tree)) == NULL ||
	    (server.scene_unowned_clients = wlr_scene_tree_create(&server.scene->tree)) == NULL ||
	    (server.scene_layout = wlr_scene_attach_output_layout(server.scene, server.output_layout)) == NULL ||
	    (server.seat = wlr_seat_create(server.display, "seat0")) == NULL ||
	    (server.gamma_control_manager = wlr_gamma_control_manager_v1_create(server.display)) == NULL ||
	    (server.idle_inhibit = wlr_idle_inhibit_v1_create(server.display)) == NULL ||
	    (server.idle_notifier = wlr_idle_notifier_v1_create(server.display)) == NULL ||
	    (server.input_inhibit = wlr_input_inhibit_manager_create(server.display)) == NULL ||
	    (server.layer_shell = wlr_layer_shell_v1_create(server.display, 4)) == NULL ||
	    (server.output_manager = wlr_output_manager_v1_create(server.display)) == NULL ||
	    (server.output_power_manager = wlr_output_power_manager_v1_create(server.display)) == NULL ||
	    (server.presentation = wlr_presentation_create(server.display, server.backend)) == NULL ||
	    (server.relative_pointer_manager = wlr_relative_pointer_manager_v1_create(server.display)) == NULL ||
	    (server.virtual_keyboard_manager = wlr_virtual_keyboard_manager_v1_create(server.display)) == NULL ||
	    (server.xcursor_manager = wlr_xcursor_manager_create(getenv("XCURSOR_THEME"), 24)) == NULL ||
	    (server.xdg_shell = wlr_xdg_shell_create(server.display, 5)) == NULL ||
	    wlr_xdg_output_manager_v1_create(server.display, server.output_layout) == NULL)
		die("Could not create backends");

	wlr_server_decoration_manager_set_default_mode(server.decoration, WLR_SERVER_DECORATION_MANAGER_MODE_SERVER);
	wlr_cursor_attach_output_layout(server.cursor, server.output_layout);
	wlr_scene_set_presentation(server.scene, server.presentation);
	wlr_scene_node_set_enabled(&server.scene_unowned_clients->node, false);
	wlr_xcursor_manager_load(server.xcursor_manager, 1);

	tmbr_register(&server.backend->events.new_input, &server.new_input, tmbr_server_on_new_input);
	tmbr_register(&server.virtual_keyboard_manager->events.new_virtual_keyboard, &server.new_virtual_keyboard, tmbr_server_on_new_virtual_keyboard);
	tmbr_register(&server.backend->events.new_output, &server.new_output, tmbr_server_on_new_output);
	tmbr_register(&server.xdg_shell->events.new_surface, &server.new_surface, tmbr_server_on_new_xdg_surface);
	tmbr_register(&server.layer_shell->events.new_surface, &server.new_layer_shell_surface, tmbr_server_on_new_layer_shell_surface);
	tmbr_register(&server.seat->events.request_set_cursor, &server.request_set_cursor, tmbr_server_on_request_set_cursor);
	tmbr_register(&server.seat->events.request_set_selection, &server.request_set_selection, tmbr_server_on_request_set_selection);
	tmbr_register(&server.seat->events.request_set_primary_selection, &server.request_set_primary_selection, tmbr_server_on_request_set_primary_selection);
	tmbr_register(&server.seat->events.request_start_drag, &server.request_start_drag, tmbr_server_on_request_start_drag);
	tmbr_register(&server.seat->events.start_drag, &server.start_drag, tmbr_server_on_start_drag);
	tmbr_register(&server.cursor->events.axis, &server.cursor_axis, tmbr_cursor_on_axis);
	tmbr_register(&server.cursor->events.button, &server.cursor_button, tmbr_cursor_on_button);
	tmbr_register(&server.cursor->events.motion, &server.cursor_motion_relative, tmbr_cursor_on_motion_relative);
	tmbr_register(&server.cursor->events.motion_absolute, &server.cursor_motion_absolute, tmbr_cursor_on_motion_absolute);
	tmbr_register(&server.cursor->events.touch_down, &server.cursor_touch_down, tmbr_cursor_on_touch_down);
	tmbr_register(&server.cursor->events.touch_up, &server.cursor_touch_up, tmbr_cursor_on_touch_up);
	tmbr_register(&server.cursor->events.touch_motion, &server.cursor_touch_motion, tmbr_cursor_on_touch_motion);
	tmbr_register(&server.cursor->events.frame, &server.cursor_frame, tmbr_cursor_on_frame);
	tmbr_register(&server.idle_inhibit->events.new_inhibitor, &server.idle_inhibitor_new, tmbr_server_on_new_idle_inhibitor);
	tmbr_register(&server.gamma_control_manager->events.set_gamma, &server.set_gamma, tmbr_server_on_set_gamma);
	tmbr_register(&server.output_manager->events.apply, &server.apply_layout, tmbr_server_on_apply_layout);
	tmbr_register(&server.output_power_manager->events.set_mode, &server.output_power_set_mode, tmbr_server_on_output_power_set_mode);

	if ((socket = wl_display_add_socket_auto(server.display)) == NULL)
		die("Could not create Wayland socket");
	setenv("WAYLAND_DISPLAY", socket, 1);

	source = wl_event_loop_add_signal(wl_display_get_event_loop(server.display), SIGTERM, tmbr_server_on_term, &server);

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
	wlr_output_layout_destroy(server.output_layout);
	wlr_scene_node_destroy(&server.scene->tree.node);
	wlr_xcursor_manager_destroy(server.xcursor_manager);
	wlr_cursor_destroy(server.cursor);
	wlr_renderer_destroy(server.renderer);
	wlr_allocator_destroy(server.allocator);
	wl_list_for_each_safe(binding, binding_tmp, &server.bindings, link) {
		free(binding->command);
		free(binding);
	}
	free(source);

	return 0;
}
