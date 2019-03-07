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

typedef struct tmbr_screen {
    struct tmbr_screen *next;
    uint16_t width;
    uint16_t height;
    uint8_t num;
} tmbr_screen_t;

static tmbr_screen_t *screens;

static void die(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);

    exit(-1);
}

static int tmbr_screens_enumerate(xcb_connection_t *conn)
{
    xcb_screen_iterator_t iter;
    const xcb_setup_t *setup;
    int i = 0;;

    if ((setup = xcb_get_setup(conn)) == NULL)
        die("Unable to get X setup");

    iter = xcb_setup_roots_iterator(setup);
    while (iter.rem) {
        tmbr_screen_t *s;

        if ((s = calloc(1, sizeof(*s))) == NULL)
            die("Cannot allocate screen");

        s->width = iter.data->width_in_pixels;
        s->height = iter.data->height_in_pixels;
        s->num = i++;
        s->next = screens;
        screens = s;

        xcb_screen_next(&iter);
    }

    return 0;
}

static void tmbr_screens_free(void)
{
    tmbr_screen_t *s, *n;

    for (s = screens; s; s = n) {
        n = s->next;
        free(s);
    }

    screens = NULL;
}

int main(int argc, const char *argv[])
{
    xcb_connection_t *conn;

    if (argc > 1)
        die("USAGE: %s\n", argv[0]);

    if ((conn = xcb_connect(NULL, NULL)) == NULL)
        die("Unable to connect to X server");

    if (tmbr_screens_enumerate(conn) < 0)
        die("Unable to enumerate screens");

    tmbr_screens_free();
    xcb_disconnect(conn);

    return 0;
}
