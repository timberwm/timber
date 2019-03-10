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

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include <xcb/xcb.h>
#include <xcb/xcb_event.h>

typedef struct tmbr_client tmbr_client_t;
typedef struct tmbr_screen tmbr_screen_t;
typedef struct tmbr_tree tmbr_tree_t;

struct tmbr_client {
    tmbr_client_t *next;
    tmbr_screen_t *screen;
    xcb_window_t window;
};

struct tmbr_screen {
    tmbr_screen_t *next;
    tmbr_client_t *clients;
    tmbr_tree_t *tree;
    xcb_screen_t *screen;
    uint16_t width;
    uint16_t height;
};

struct tmbr_tree {
    tmbr_tree_t *left;
    tmbr_tree_t *right;
    tmbr_client_t *client;
};

static tmbr_screen_t *screens;
static xcb_connection_t *conn;

static void die(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);

    exit(-1);
}

static int tmbr_tree_insert(tmbr_tree_t **tree, tmbr_client_t *client)
{
    if (!*tree) {
        tmbr_tree_t *t;

        if ((t = calloc(1, sizeof(*t))) == NULL)
            die("Unable to allocate tree");
        t->client = client;

        *tree = t;

        return 0;
    } else if ((*tree)->client) {
        tmbr_tree_t *l, *r;

        if ((l = calloc(1, sizeof(*l))) == NULL ||
                (r = calloc(1, sizeof(*r))) == NULL)
            die("Unable to allocate trees");

        l->client = (*tree)->client;
        r->client = client;
        (*tree)->client = NULL;
        (*tree)->left = l;
        (*tree)->right = r;

        return 0;
    } else if (!(*tree)->left) {
        return tmbr_tree_insert(&(*tree)->left, client);
    } else {
        return tmbr_tree_insert(&(*tree)->right, client);
    }
}

static int tmbr_tree_remove(tmbr_tree_t **tree, tmbr_client_t *client)
{
    if (!*tree)
        return -1;

    if ((*tree)->client == client) {
        free(*tree);
        *tree = NULL;
        return 0;
    } else if (tmbr_tree_remove(&(*tree)->left, client) == 0) {
        return 0;
    } else if (tmbr_tree_remove(&(*tree)->right, client) == 0) {
        return 0;
    }

    return -1;
}

static int tmbr_client_manage(tmbr_screen_t *screen, xcb_window_t window)
{
    const uint32_t values[] = { XCB_EVENT_MASK_ENTER_WINDOW };
    xcb_get_window_attributes_reply_t *attrs;
    xcb_void_cookie_t cookie;
    tmbr_client_t *client;
    int error = 0;

    if ((attrs = xcb_get_window_attributes_reply(conn,
                                                 xcb_get_window_attributes(conn, window),
                                                 NULL)) == NULL) {
        error = -1;
        goto out;
    }

    if (attrs->map_state == XCB_MAP_STATE_UNVIEWABLE)
        goto out;

    cookie = xcb_change_window_attributes_checked(conn, window, XCB_CW_EVENT_MASK, values);
    if ((xcb_request_check(conn, cookie)) != NULL)
        die("Could not subscribe to window events");

    if ((client = calloc(1, sizeof(*client))) == NULL)
        die("Unable to allocate client");

    client->window = window;
    client->screen = screen;
    client->next = screen->clients;
    screen->clients = client;

    if (tmbr_tree_insert(&screen->tree, client) < 0)
        die("Unable to remove client from tree");

out:
    free(attrs);
    return error;
}

static int tmbr_client_unmanage(tmbr_client_t *client)
{
    tmbr_client_t **c = &client->screen->clients;

    while (*c && *c != client)
        c = &(*c)->next;

    if (!*c)
        return -1;

    *c = client->next;

    if (tmbr_tree_remove(&client->screen->tree, client) < 0)
        die("Unable to insert client into tree");

    free(client);
    return 0;
}

static int tmbr_client_focus(tmbr_client_t *client)
{
    xcb_void_cookie_t cookie;

    cookie = xcb_set_input_focus(conn, XCB_INPUT_FOCUS_NONE, client->window, XCB_CURRENT_TIME);
    if ((xcb_request_check(conn, cookie)) != NULL)
        die("Could not focus client");

    xcb_flush(conn);
    return 0;
}

static int tmbr_clients_enumerate(tmbr_screen_t *screen)
{
    xcb_query_tree_reply_t *tree;
    xcb_window_t *children;
    unsigned i;

    if ((tree = xcb_query_tree_reply(conn, xcb_query_tree(conn, screen->screen->root), NULL)) == NULL)
        die("Unable to query tree");

    children = xcb_query_tree_children(tree);

    for (i = 0; i < xcb_query_tree_children_length(tree); i++)
        tmbr_client_manage(screen, children[i]);

    free(tree);

    return 0;
}

static int tmbr_clients_find_by_window(tmbr_client_t **out, xcb_window_t w)
{
    tmbr_screen_t *s;

    for (s = screens; s; s = s->next) {
        tmbr_client_t *c;

        for (c = s->clients; c; c = c->next) {
            if (c->window == w) {
                *out = c;
                return 0;
            }
        }
    }

    return -1;
}

static void tmbr_clients_free(tmbr_client_t *c)
{
    tmbr_client_t *n;

    for (; c; c = n) {
        n = c->next;
        free(c);
    }
}

static int tmbr_screen_manage(xcb_screen_t *screen)
{
    const uint32_t values[] = {
        XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT |
        XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY
    };
    xcb_generic_error_t *error;
    xcb_void_cookie_t cookie;
    tmbr_screen_t *s;

    if ((s = calloc(1, sizeof(*s))) == NULL)
        die("Cannot allocate screen");

    s->screen = screen;
    s->width = screen->width_in_pixels;
    s->height = screen->height_in_pixels;
    s->next = screens;
    screens = s;

    cookie = xcb_change_window_attributes_checked(conn, screen->root,
                                                  XCB_CW_EVENT_MASK, values);

    if ((error = xcb_request_check(conn, cookie)) != NULL)
        die("Another window manager is running already.");

    tmbr_clients_enumerate(s);

    return 0;
}

static int tmbr_screens_enumerate(xcb_connection_t *conn)
{
    xcb_screen_iterator_t iter;
    const xcb_setup_t *setup;

    if ((setup = xcb_get_setup(conn)) == NULL)
        die("Unable to get X setup");

    iter = xcb_setup_roots_iterator(setup);
    while (iter.rem) {
        tmbr_screen_manage(iter.data);
        xcb_screen_next(&iter);
    }

    return 0;
}

static int tmbr_screens_find_by_root(tmbr_screen_t **out, xcb_window_t root)
{
    tmbr_screen_t *s;

    for (s = screens; s; s = s->next) {
        if (s->screen->root == root) {
            *out = s;
            return 0;
        }
    }

    return -1;
}

static void tmbr_screens_free(void)
{
    tmbr_screen_t *s, *n;

    for (s = screens; s; s = n) {
        n = s->next;
        tmbr_clients_free(s->clients);
        free(s);
    }

    screens = NULL;
}

static void tmbr_handle_enter_notify(xcb_enter_notify_event_t *ev)
{
    tmbr_client_t *client;

    if (ev->mode != XCB_NOTIFY_MODE_NORMAL)
        return;

    if ((tmbr_clients_find_by_window(&client, ev->event)) < 0)
        return;

    puts("adjusting focus");
    tmbr_client_focus(client);
}

static void tmbr_handle_create_notify(xcb_create_notify_event_t *ev)
{
    tmbr_screen_t *screen;

    if (tmbr_screens_find_by_root(&screen, ev->parent) < 0)
        return;

    puts("adopting new client");
    tmbr_client_manage(screen, ev->window);
}

static void tmbr_handle_map_request(xcb_map_request_event_t *ev)
{
    xcb_map_window(conn, ev->window);
    xcb_flush(conn);
    puts("map request");
}

static void tmbr_handle_unmap_notify(xcb_unmap_notify_event_t *ev)
{
    puts("unmap notify");
}

static void tmbr_handle_destroy_notify(xcb_destroy_notify_event_t *ev)
{
    tmbr_client_t *client;

    if ((tmbr_clients_find_by_window(&client, ev->window)) < 0)
        return;

    puts("unmanage client");
    tmbr_client_unmanage(client);
}

int main(int argc, const char *argv[])
{
    xcb_generic_event_t *ev;

    if (argc > 1)
        die("USAGE: %s\n", argv[0]);

    if ((conn = xcb_connect(NULL, NULL)) == NULL)
        die("Unable to connect to X server");

    if (tmbr_screens_enumerate(conn) < 0)
        die("Unable to enumerate screens");

    xcb_flush(conn);

    while ((ev = xcb_wait_for_event(conn)) != NULL) {
        switch (XCB_EVENT_RESPONSE_TYPE(ev)) {
            case XCB_ENTER_NOTIFY:
                tmbr_handle_enter_notify((xcb_enter_notify_event_t *) ev);
                break;
            case XCB_CREATE_NOTIFY:
                tmbr_handle_create_notify((xcb_create_notify_event_t *) ev);
                break;
            case XCB_MAP_REQUEST:
                tmbr_handle_map_request((xcb_map_request_event_t *) ev);
                break;
            case XCB_UNMAP_NOTIFY:
                tmbr_handle_unmap_notify((xcb_unmap_notify_event_t *) ev);
                break;
            case XCB_DESTROY_NOTIFY:
                tmbr_handle_destroy_notify((xcb_destroy_notify_event_t *) ev);
                break;
        }

        free(ev);
    }

    tmbr_screens_free();
    xcb_disconnect(conn);

    return 0;
}
