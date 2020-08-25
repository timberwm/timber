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

#include <errno.h>
#include <libgen.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>

#include <xcb/xcb.h>

#include "common.h"
#include "config.h"

const tmbr_commands_t commands[] = {
	{ "client", "focus",      TMBR_ARG_SEL              },
	{ "client", "fullscreen", 0                         },
	{ "client", "kill",       0                         },
	{ "client", "resize",     TMBR_ARG_DIR|TMBR_ARG_INT },
	{ "client", "swap",       TMBR_ARG_SEL              },
	{ "client", "to_desktop", TMBR_ARG_SEL              },
	{ "client", "to_screen",  TMBR_ARG_SEL              },
	{ "desktop", "focus",     TMBR_ARG_SEL              },
	{ "desktop", "kill",      0                         },
	{ "desktop", "new",       0                         },
	{ "desktop", "swap",      TMBR_ARG_SEL              },
	{ "screen", "focus",      TMBR_ARG_SEL              },
	{ "tree", "rotate",       0                         },
	{ "state", "subscribe",   0                         },
	{ "state", "query",       0                         }
};

const char *directions[] = { "north", "south", "east", "west" };
const char *selections[] = { "prev", "next" };

void __attribute__((noreturn, format(printf, 1, 2))) die(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);

	exit(-1);
}

void __attribute__((noreturn)) usage(const char *executable)
{
	size_t i;
	printf("USAGE: %s\n", executable);

	for (i = 0; i < ARRAY_SIZE(commands); i++)
		printf("   or: %s %s %s%s%s%s\n", executable, commands[i].cmd, commands[i].subcmd,
			commands[i].args & TMBR_ARG_SEL ? " (next|prev)" : "",
			commands[i].args & TMBR_ARG_DIR ? " (north|south|east|west)" : "",
			commands[i].args & TMBR_ARG_INT ? " <NUMBER>" : "");

	exit(-1);
}

void *tmbr_alloc(size_t bytes, const char *msg)
{
	void *ptr;
	if ((ptr = calloc(1, bytes)) == NULL)
		die("%s", msg);
	return ptr;
}

int tmbr_command_parse(tmbr_command_args_t *args, int argc, const char **argv)
{
	ssize_t c, i;

	if (argc < 2)
		return -1;

	ARRAY_FIND(commands, c, !strcmp(commands[c].cmd, argv[0]) && !strcmp(commands[c].subcmd, argv[1]));
	if (c < 0)
		return -1;
	args->cmd = (tmbr_command_t) c;

	argc -= 2;
	argv += 2;

	if (commands[c].args & TMBR_ARG_SEL) {
		if (!argc)
			return -1;

		ARRAY_FIND(commands, i, !strcmp(argv[0], selections[i]));
		if (i < 0)
			return -1;
		args->sel = (tmbr_select_t) i;
		argc--;
		argv++;
	}

	if (commands[c].args & TMBR_ARG_DIR) {
		if (!argc)
			return -1;

		ARRAY_FIND(commands, i, !strcmp(argv[0], directions[i]));
		if (i < 0)
			return -1;
		args->dir = (tmbr_dir_t) i;
		argc--;
		argv++;
	}

	if (commands[c].args & TMBR_ARG_INT) {
		if (!argc)
			return -1;
		args->i = atoi(argv[0]);
		argc--;
		argv++;
	}

	if (argc)
		return -1;

	return 0;
}

int tmbr_ctrl_connect(const char **out_path, char create)
{
	static char path[PATH_MAX] = { 0 };
	struct sockaddr_un addr;
	char *host = NULL, *env;
	int fd, display;

	if (!xcb_parse_display(NULL, &host, &display, NULL))
		display = 0;
	free(host);

	if ((env = getenv("TMBR_CTRL_PATH")) != NULL)
		strncpy(path, env, sizeof(path) - 1);
	else
		snprintf(path, sizeof(path), TMBR_CTRL_PATH, display);

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

	if ((fd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0)
		die("Unable to create control socket");

	if (create) {
		char *dir;

		if ((dir = strdup(path)) == NULL || (dir = dirname(dir)) == NULL)
			die("Unable to compute control directory name");

		if ((mkdir(dir, 0700) < 0 && errno != EEXIST) ||
		    (unlink(path) < 0 && errno != ENOENT))
			die("Unable to prepare control socket directory: %s", strerror(errno));

		if (bind(fd, (struct sockaddr *) &addr, sizeof(addr)) < 0 || listen(fd, 10) < 0)
			die("Unable to set up control socket: %s", strerror(errno));

		free(dir);
	} else if (connect(fd, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
		die("Unable to connect to control socket: %s", strerror(errno));
	}

	if (out_path)
		*out_path = path;
	return fd;
}

static int read_bytes(int fd, char *buf, size_t bufsize)
{
	size_t total = 0;
	while (total < bufsize) {
		ssize_t bytes = read(fd, buf + total, bufsize - total);
		if (bytes < 0 && (errno == EAGAIN || errno == EINTR))
			continue;
		if (bytes <= 0)
			return -1;
		total += (size_t) bytes;
	}
	return 0;
}

int tmbr_ctrl_read(int fd, tmbr_pkt_t *out)
{
	char prefix[TMBR_PKT_PREFIXLEN + 1] = {0}, type;
	unsigned messagelen;

	if (read_bytes(fd, prefix, sizeof(prefix) - 1) < 0 ||
	    sscanf(prefix, "%c%04u", &type, &messagelen) != 2 ||
	    type < '0' || type >= '0' + TMBR_PKT_LAST ||
	    messagelen > sizeof(out->message) - 1 ||
	    read_bytes(fd, out->message, messagelen) < 0)
		return -1;

	out->message[messagelen] = '\0';
	out->type = (tmbr_pkt_type_t) type - '0';

	return 0;
}

static int write_bytes(int fd, const char *buf, size_t bufsize)
{
	size_t total = 0;
	while (total < bufsize) {
		ssize_t bytes = write(fd, buf + total, bufsize - total);
		if (bytes < 0 && (errno == EAGAIN || errno == EINTR))
			continue;
		if (bytes <= 0)
			return -1;
		total += (size_t) bytes;
	}
	return 0;
}

int tmbr_ctrl_write(int fd, tmbr_pkt_type_t type, const char *fmt, ...)
{
	char prefix[TMBR_PKT_PREFIXLEN + 1], message[TMBR_PKT_MESSAGELEN];
	int messagelen;
	va_list ap;

	va_start(ap, fmt);
	messagelen = vsnprintf(message, sizeof(message), fmt, ap);
	va_end(ap);

	if (messagelen < 0 || (unsigned) messagelen >= sizeof(message) - 1)
		return -1;
	snprintf(prefix, sizeof(prefix), "%c%04u", type + '0', messagelen);

	if (write_bytes(fd, prefix, sizeof(prefix) - 1) < 0 ||
	    write_bytes(fd, message, (size_t) messagelen) < 0)
		return -1;

	return 0;
}
