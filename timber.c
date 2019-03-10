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
#define TMBR_UNUSED(x) (void)(x)
#define TMBR_BORDER_WIDTH 3
#define TMBR_COLOR_ACTIVE 0xFF005577
#define TMBR_COLOR_INACTIVE 0xFF222222

typedef struct tmbr_client tmbr_client_t;
typedef struct tmbr_command_args tmbr_command_args_t;
typedef struct tmbr_command tmbr_command_t;
typedef struct tmbr_screen tmbr_screen_t;
typedef struct tmbr_tree tmbr_tree_t;

typedef enum {
    TMBR_DIR_LEFT,
    TMBR_DIR_RIGHT,
    TMBR_DIR_MAX
} tmbr_dir_t;

typedef enum {
    TMBR_ORIENTATION_VERTICAL,
    TMBR_ORIENTATION_HORIZONTAL
} tmbr_orientation_t;

struct tmbr_command_args {
    int i;
};

struct tmbr_client {
    tmbr_screen_t *screen;
    tmbr_tree_t *tree;
    xcb_window_t window;
    char focussed;
};

struct tmbr_command {
    const char *cmd;
    void (*fn)(const tmbr_command_args_t *);
    tmbr_command_args_t args;
};

struct tmbr_screen {
    tmbr_screen_t *next;
    tmbr_tree_t *tree;
    xcb_screen_t *screen;
    uint16_t width;
    uint16_t height;
    char focussed;
};

struct tmbr_tree {
    tmbr_tree_t *children[TMBR_DIR_MAX];
    tmbr_tree_t *parent;
    tmbr_client_t *client;
    tmbr_orientation_t orientation;
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
        client->tree = t;

        *tree = t;

        return 0;
    } else if ((*tree)->client) {
        tmbr_tree_t *l, *r;

        if ((l = calloc(1, sizeof(*l))) == NULL ||
                (r = calloc(1, sizeof(*r))) == NULL)
            die("Unable to allocate trees");

        l->client = (*tree)->client;
        l->client->tree = l;
        l->parent = (*tree);

        r->client = client;
        r->client->tree = r;
        r->parent = (*tree);

        (*tree)->client = NULL;
        (*tree)->children[TMBR_DIR_LEFT] = l;
        (*tree)->children[TMBR_DIR_RIGHT] = r;

        return 0;
    } else if (!(*tree)->children[TMBR_DIR_LEFT]) {
        return tmbr_tree_insert(&(*tree)->children[TMBR_DIR_LEFT], client);
    } else {
        return tmbr_tree_insert(&(*tree)->children[TMBR_DIR_RIGHT], client);
    }
}

static int tmbr_tree_find_by_window(tmbr_tree_t **node, tmbr_tree_t *tree,
        xcb_window_t window)
{
    if (!tree)
        return -1;

    if (tree->client && tree->client->window == window) {
        *node = tree;
        return 0;
    } else if (tmbr_tree_find_by_window(node, tree->children[TMBR_DIR_LEFT], window) == 0) {
        return 0;
    } else if (tmbr_tree_find_by_window(node, tree->children[TMBR_DIR_RIGHT], window) == 0) {
        return 0;
    }

    return -1;
}

static int tmbr_tree_find_by_focus(tmbr_tree_t **node, tmbr_tree_t *tree)
{
    if (!tree)
        return -1;

    if (tree->client && tree->client->focussed) {
        *node = tree;
        return 0;
    } else if (tmbr_tree_find_by_focus(node, tree->children[TMBR_DIR_LEFT]) == 0) {
        return 0;
    } else if (tmbr_tree_find_by_focus(node, tree->children[TMBR_DIR_RIGHT]) == 0) {
        return 0;
    }

    return -1;
}

static int tmbr_tree_find_sibling(tmbr_tree_t **node, tmbr_tree_t *tree, tmbr_dir_t dir)
{
    unsigned char upwards_dir = dir, downwards_dir = !dir;

    while (tree) {
        if (!tree->parent) {
            /* We want to wrap to the leftmost node */
            break;
        } else if (tree != (tree->parent->children[upwards_dir])) {
            /* Go to the leftmost node of the right parent node */
            tree = tree->parent->children[upwards_dir];
            break;
        }
        tree = tree->parent;
    }

    if (!tree)
        return -1;

    while (tree->children[downwards_dir])
        tree = tree->children[downwards_dir];

    *node = tree;

    return 0;
}

static int tmbr_tree_remove(tmbr_tree_t **tree, tmbr_tree_t *node)
{
    tmbr_tree_t *parent = node->parent;

    if (node == *tree) {
        free(node);
        *tree = NULL;
        return 0;
    }

    if (parent->children[TMBR_DIR_LEFT] == node)
        parent->client = parent->children[TMBR_DIR_RIGHT]->client;
    else if (parent->children[TMBR_DIR_RIGHT] == node)
        parent->client = parent->children[TMBR_DIR_LEFT]->client;

    parent->client->tree = parent;
    free(parent->children[TMBR_DIR_LEFT]);
    parent->children[TMBR_DIR_LEFT] = NULL;
    free(parent->children[TMBR_DIR_RIGHT]);
    parent->children[TMBR_DIR_RIGHT] = NULL;

    return 0;
}

static int tmbr_client_manage(tmbr_screen_t *screen, xcb_window_t window)
{
    const uint32_t values[] = { XCB_EVENT_MASK_ENTER_WINDOW };
    tmbr_tree_t *focussed = NULL;
    xcb_void_cookie_t cookie;
    tmbr_client_t *client;
    int error = 0;

    cookie = xcb_change_window_attributes_checked(conn, window, XCB_CW_EVENT_MASK, values);
    if ((xcb_request_check(conn, cookie)) != NULL)
        die("Could not subscribe to window events");

    if ((client = calloc(1, sizeof(*client))) == NULL)
        die("Unable to allocate client");

    client->window = window;
    client->screen = screen;

    tmbr_tree_find_by_focus(&focussed, screen->tree);

    if (tmbr_tree_insert(focussed ? &focussed : &screen->tree, client) < 0)
        die("Unable to remove client from tree");

    return error;
}

static int tmbr_client_unmanage(tmbr_client_t *client)
{
    if (tmbr_tree_remove(&client->screen->tree, client->tree) < 0)
        die("Unable to remove client from tree");

    free(client);
    return 0;
}

static int tmbr_client_draw_border(tmbr_client_t *client, uint32_t color)
{
    xcb_change_window_attributes(conn, client->window, XCB_CW_BORDER_PIXEL, &color);
    return 0;
}

static int tmbr_client_focus(tmbr_client_t *client)
{
    xcb_void_cookie_t cookie;

    cookie = xcb_set_input_focus(conn, XCB_INPUT_FOCUS_NONE, client->window, XCB_CURRENT_TIME);
    if ((xcb_request_check(conn, cookie)) != NULL)
        die("Could not focus client");

    tmbr_client_draw_border(client, TMBR_COLOR_ACTIVE);
    client->focussed = 1;
    client->screen->focussed = 1;
    return 0;
}

static int tmbr_client_unfocus(tmbr_client_t *client)
{
    client->focussed = 0;
    client->screen->focussed = 0;
    tmbr_client_draw_border(client, TMBR_COLOR_INACTIVE);
    return 0;
}

static int tmbr_client_layout(tmbr_client_t *client, uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    uint32_t values[5];
    uint16_t mask =
        XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
        XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT |
        XCB_CONFIG_WINDOW_BORDER_WIDTH;

    values[0] = x;
    values[1] = y;
    values[2] = w - 2 * TMBR_BORDER_WIDTH;
    values[3] = h - 2 * TMBR_BORDER_WIDTH;
    values[4] = TMBR_BORDER_WIDTH;

    xcb_configure_window(conn, client->window, mask, values);
    return 0;
}

static int tmbr_clients_enumerate(tmbr_screen_t *screen)
{
    xcb_query_tree_reply_t *tree;
    xcb_window_t *children;
    int i;

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

static int tmbr_layout_tree(tmbr_screen_t *screen, tmbr_tree_t *tree,
        uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    uint16_t xoff = 0, yoff = 0;

    if (tree->client)
        return tmbr_client_layout(tree->client, x, y, w, h);

    if (tree->orientation == TMBR_ORIENTATION_VERTICAL) {
        w /= 2;
        xoff = w;
    } else {
        h /= 2;
        yoff = h;
    }

    if (tmbr_layout_tree(screen, tree->children[TMBR_DIR_LEFT], x, y, w, h) < 0)
        die("Unable to layout left tree");

    if (tmbr_layout_tree(screen, tree->children[TMBR_DIR_RIGHT], x + xoff, y + yoff, w, h) < 0)
        die("Unable to layout right tree");

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

static int tmbr_screens_find_by_focus(tmbr_screen_t **out)
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

static void tmbr_screens_free(tmbr_screen_t *s)
{
    tmbr_screen_t *n;

    for (; s; s = n) {
        n = s->next;
        free(s);
    }

    screens = NULL;
}

static int tmbr_handle_enter_notify(xcb_enter_notify_event_t *ev)
{
    tmbr_screen_t *screen;

    if (ev->mode != XCB_NOTIFY_MODE_NORMAL)
        return 0;

    for (screen = screens; screen; screen = screen->next) {
        tmbr_tree_t *focussed = NULL, *entered;

        tmbr_tree_find_by_focus(&focussed, screen->tree);

        if ((tmbr_tree_find_by_window(&entered, screen->tree, ev->event)) < 0)
            continue;

        if (tmbr_client_focus(entered->client) < 0)
            return -1;

        if (focussed && tmbr_client_unfocus(focussed->client) < 0)
            return -1;

        return 0;
    }

    return -1;
}

static int tmbr_handle_map_request(xcb_map_request_event_t *ev)
{
    tmbr_screen_t *screen;

    if (tmbr_screens_find_by_root(&screen, ev->parent) < 0)
        return -1;

    if (tmbr_client_manage(screen, ev->window) < 0)
        return -1;

    if (tmbr_layout(screen) < 0)
        return -1;

    xcb_map_window(conn, ev->window);

    return 0;
}

static int tmbr_handle_destroy_notify(xcb_destroy_notify_event_t *ev)
{
    tmbr_screen_t *screen;

    for (screen = screens; screen; screen = screen->next) {
        tmbr_tree_t *node;

        if ((tmbr_tree_find_by_window(&node, screen->tree, ev->window)) < 0)
            continue;

        if (tmbr_client_unmanage(node->client) < 0)
            die("Unable to unmanage client");

        return tmbr_layout(screen);
    }

    return -1;
}

static int tmbr_handle_event(xcb_generic_event_t *ev)
{
    switch (XCB_EVENT_RESPONSE_TYPE(ev)) {
        case XCB_ENTER_NOTIFY:
            return tmbr_handle_enter_notify((xcb_enter_notify_event_t *) ev);
        case XCB_MAP_REQUEST:
            return tmbr_handle_map_request((xcb_map_request_event_t *) ev);
        case XCB_DESTROY_NOTIFY:
            return tmbr_handle_destroy_notify((xcb_destroy_notify_event_t *) ev);
        default:
            return -1;
    }
}

static void tmbr_cmd_focus_sibling(const tmbr_command_args_t *args)
{
    tmbr_tree_t *focussed, *next;
    tmbr_screen_t *screen;

    if (tmbr_screens_find_by_focus(&screen) < 0 ||
            tmbr_tree_find_by_focus(&focussed, screen->tree) < 0 ||
            tmbr_tree_find_sibling(&next, focussed, args->i) < 0)
        return;

    tmbr_client_unfocus(focussed->client);
    tmbr_client_focus(next->client);
}

static void tmbr_cmd_swap_sibling(const tmbr_command_args_t *args)
{
    tmbr_tree_t *focussed, *next;
    tmbr_screen_t *screen;
    tmbr_client_t *c;

    if (tmbr_screens_find_by_focus(&screen) < 0 ||
            tmbr_tree_find_by_focus(&focussed, screen->tree) < 0 ||
            tmbr_tree_find_sibling(&next, focussed, args->i) < 0)
        return;

    c = focussed->client;
    focussed->client = next->client;
    next->client = c;

    tmbr_layout(screen);
}

static void tmbr_cmd_toggle_orientation(const tmbr_command_args_t *args)
{
    tmbr_screen_t *screen;
    tmbr_tree_t *focussed;

    TMBR_UNUSED(args);

    if (tmbr_screens_find_by_focus(&screen) < 0 ||
            tmbr_tree_find_by_focus(&focussed, screen->tree) < 0 ||
            !focussed->parent)
        return;

    focussed->parent->orientation = !focussed->parent->orientation;

    tmbr_layout(screen);
}

static void tmbr_handle_command(int fd)
{
    const tmbr_command_t cmds[] = {
        { "client_focus_prev",       tmbr_cmd_focus_sibling,      { TMBR_DIR_LEFT }  },
        { "client_focus_next",       tmbr_cmd_focus_sibling,      { TMBR_DIR_RIGHT } },
        { "client_swap_prev",        tmbr_cmd_swap_sibling,       { TMBR_DIR_LEFT }  },
        { "client_swap_next",        tmbr_cmd_swap_sibling,       { TMBR_DIR_RIGHT } },
        { "tree_toggle_orientation", tmbr_cmd_toggle_orientation, { 0 }              },
    };
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
    unlink(FIFO_PATH);

    tmbr_screens_free(screens);
    xcb_disconnect(conn);
}

static int tmbr_setup(void)
{
    if (mkfifo(FIFO_PATH, 0644) < 0)
        die("Unable to create fifo");

    if ((fifofd = open(FIFO_PATH, O_RDWR|O_NONBLOCK)) < 0)
        die("Unable to open fifo");

    if ((conn = xcb_connect(NULL, NULL)) == NULL)
        die("Unable to connect to X server");

    if (tmbr_screens_enumerate(conn) < 0)
        die("Unable to enumerate screens");

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
