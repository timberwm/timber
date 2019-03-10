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

#include <xcb/xcb.h>
#include <xcb/xcb_event.h>

#define FIFO_PATH "/tmp/timber.s"

typedef struct tmbr_client tmbr_client_t;
typedef struct tmbr_screen tmbr_screen_t;
typedef struct tmbr_tree tmbr_tree_t;

struct tmbr_client {
    tmbr_client_t *next;
    tmbr_screen_t *screen;
    tmbr_tree_t *tree;
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
    tmbr_tree_t *parent;
    tmbr_tree_t *left;
    tmbr_tree_t *right;
    tmbr_client_t *client;
};

static tmbr_screen_t *screens;
static xcb_connection_t *conn;
static int fifofd = -1;

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
        l->parent = (*tree);
        r->client = client;
        r->parent = (*tree);
        (*tree)->client = NULL;
        (*tree)->left = l;
        (*tree)->right = r;

        client->tree = r;

        return 0;
    } else if (!(*tree)->left) {
        return tmbr_tree_insert(&(*tree)->left, client);
    } else {
        return tmbr_tree_insert(&(*tree)->right, client);
    }
}

static int tmbr_tree_remove(tmbr_tree_t **tree, tmbr_tree_t *node)
{
    tmbr_tree_t *parent;

    if (node == *tree) {
        free(node);
        *tree = NULL;
        return 0;
    }

    while ((parent = node->parent) != NULL) {
        if (parent->left == node)
            parent->left = NULL;
        else if (parent->right == node)
            parent->right = NULL;
        free(node);
        if (parent->left || parent->right)
            break;
        node = parent;
    }

    return 0;
}

static int tmbr_client_manage(tmbr_screen_t *screen, xcb_window_t window)
{
    const uint32_t values[] = { XCB_EVENT_MASK_ENTER_WINDOW };
    xcb_void_cookie_t cookie;
    tmbr_client_t *client;
    int error = 0;

    puts("managing window");

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

    if (tmbr_tree_remove(&client->screen->tree, client->tree) < 0)
        die("Unable to remove client from tree");

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

static int tmbr_client_layout(tmbr_client_t *client, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    const uint32_t values[] = { x, y, w, h };
    uint16_t mask = XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
        XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT;
    xcb_configure_window(conn, client->window, mask, values);
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

    for (i = 0; i < xcb_query_tree_children_length(tree); i++) {
        xcb_get_window_attributes_reply_t *attrs = NULL;

        if ((attrs = xcb_get_window_attributes_reply(conn,
                                                     xcb_get_window_attributes(conn, children[i]),
                                                     NULL)) == NULL)
            goto next;

        if (attrs->map_state != XCB_MAP_STATE_VIEWABLE)
            goto next;

        tmbr_client_manage(screen, children[i]);
next:
        free(attrs);
    }

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

static int tmbr_layout_tree(tmbr_screen_t *screen, tmbr_tree_t *tree,
        int x, int y, int w, int h)
{
    if (tree->client)
        return tmbr_client_layout(tree->client, x, y, w, h);

    if (tree->left) {
        int lw = tree->right ? w / 2 : w;
        if (tmbr_layout_tree(screen, tree->left, x, y, lw, h) < 0)
            die("Unable to layout left tree");
    }

    if (tree->right) {
        int rx = tree->left ? x + w / 2 : x, rw = tree->left ? w / 2 : w;
        if (tmbr_layout_tree(screen, tree->right, rx, y, rw, h) < 0)
            die("Unable to layout right tree");
    }

    return 0;
}

static int tmbr_layout(tmbr_screen_t *screen)
{
    if (!screen || !screen->tree)
        return 0;

    return tmbr_layout_tree(screen, screen->tree, 0, 0,
            screen->screen->width_in_pixels,
            screen->screen->height_in_pixels);
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

    if (tmbr_clients_enumerate(s) < 0)
        die("Unable to enumerate clients");

    if (tmbr_layout(s) < 0)
        die("Unable to layout screen");

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

static void tmbr_screens_free(tmbr_screen_t *s)
{
    tmbr_screen_t *n;

    for (; s; s = n) {
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

static void tmbr_handle_map_request(xcb_map_request_event_t *ev)
{
    tmbr_screen_t *s;

    /* Only handle windows for managed screens */
    if (tmbr_screens_find_by_root(&s, ev->parent) < 0)
        return;

    tmbr_client_manage(s, ev->window);

    for (s = screens; s; s = s->next)
        tmbr_layout(s);

    xcb_map_window(conn, ev->window);
    xcb_flush(conn);
}

static void tmbr_handle_unmap_notify(xcb_unmap_notify_event_t *ev)
{
    puts("unmap notify");
}

static void tmbr_handle_destroy_notify(xcb_destroy_notify_event_t *ev)
{
    tmbr_client_t *client;
    tmbr_screen_t *s;

    if ((tmbr_clients_find_by_window(&client, ev->window)) < 0)
        return;

    if (tmbr_client_unmanage(client) < 0)
        die("Unable to unmanage client");

    for (s = screens; s; s = s->next)
        tmbr_layout(s);
}

static void tmbr_handle_event(xcb_generic_event_t *ev)
{
    switch (XCB_EVENT_RESPONSE_TYPE(ev)) {
        case XCB_ENTER_NOTIFY:
            tmbr_handle_enter_notify((xcb_enter_notify_event_t *) ev);
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
}

static void tmbr_handle_command(int fd)
{
    char buf[BUFSIZ];
    ssize_t n;

    if ((n = read(fd, buf, sizeof(buf) - 1)) <= 0)
        return;
    buf[n] = '\0';

    printf("command: %s\n", buf);
}

static void tmbr_cleanup(int signal)
{
    if (fifofd >= 0)
        close(fifofd);
    unlink(FIFO_PATH);

    tmbr_screens_free(screens);
    xcb_disconnect(conn);
}

static int tmbr_setup(void)
{
    if (mkfifo(FIFO_PATH, 0644) < 0)
        die("Unable to create fifo");

    if ((fifofd = open(FIFO_PATH, O_RDONLY|O_NONBLOCK)) < 0)
        die("Unable to open fifo");

    if ((conn = xcb_connect(NULL, NULL)) == NULL)
        die("Unable to connect to X server");

    if (tmbr_screens_enumerate(conn) < 0)
        die("Unable to enumerate screens");

    xcb_flush(conn);

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

    while (poll(fds, 2, -1) > 0) {
        xcb_generic_event_t *ev;

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
