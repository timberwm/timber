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

void __attribute__((noreturn, format(printf, 1, 2))) die(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);

	exit(-1);
}

void *tmbr_alloc(size_t bytes, const char *msg)
{
	void *ptr;
	if ((ptr = calloc(1, bytes)) == NULL)
		die("%s", msg);
	return ptr;
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
	if (read_bytes(fd, (char *) out, sizeof(*out)) < 0)
		return -1;
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

int tmbr_ctrl_write(int fd, tmbr_pkt_t *pkt)
{
	if (write_bytes(fd, (char *)pkt, sizeof(*pkt)) < 0)
		return -1;
	return 0;
}

int tmbr_ctrl_write_data(int fd, const char *fmt, ...)
{
	tmbr_pkt_t pkt = {0};
	int messagelen;
	va_list ap;

	va_start(ap, fmt);
	messagelen = vsnprintf(pkt.u.data, sizeof(pkt.u.data), fmt, ap);
	va_end(ap);

	pkt.type = TMBR_PKT_DATA;
	if (messagelen < 0 || (unsigned) messagelen >= sizeof(pkt.u.data) - 1)
		return -1;

	return tmbr_ctrl_write(fd, &pkt);
}
