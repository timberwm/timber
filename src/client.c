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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "client.h"
#include "common.h"
#include "config.h"

typedef int (*tmbr_cmd_t)(int argc, const char *argv[]);

static int tmbr_ctrl_connect(const char *path)
{
	struct sockaddr_un addr;
	int fd;

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

	if ((fd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0 ||
	    connect(fd, (struct sockaddr *) &addr, sizeof(addr)) < 0)
		die("Unable to connect to control socket: %s", strerror(errno));

	return fd;
}

static int tmbr_execute(tmbr_command_t cmd, const tmbr_command_args_t *args, int fd)
{
	char params[3][32] = { "", "", "" };
	char buf[TMBR_CTRL_BUFSIZE];
	int error;

	if (commands[cmd].args & TMBR_ARG_SEL)
		snprintf(params[0], sizeof(params[0]), " %s", selections[args->sel]);
	if (commands[cmd].args & TMBR_ARG_DIR)
		snprintf(params[1], sizeof(params[1]), " %s", directions[args->dir]);
	if (commands[cmd].args & TMBR_ARG_INT)
		snprintf(params[2], sizeof(params[2]), " %i", args->i);

	if (tmbr_ctrl_writef(fd, "%s %s%s%s%s", commands[cmd].cmd,
			     commands[cmd].subcmd, params[0], params[1], params[2]) < 0 ||
	    tmbr_ctrl_read(fd, buf, sizeof(buf)) < 0)
		return -1;

	if ((error = atoi(buf)) != 0)
		printf("Error executing command: %s\n", strerror(error));

	return error;
}

int tmbr_client(int argc, const char *argv[])
{
	tmbr_command_args_t args;
	tmbr_command_t cmd;
	int error, fd;

	if ((tmbr_command_parse(&cmd, &args, argc - 1, argv + 1)) < 0)
		usage(argv[0]);

	if ((fd = tmbr_ctrl_connect(TMBR_CTRL_PATH)) < 0)
		die("Unable to connect to control socket");

	if ((error = tmbr_execute(cmd, &args, fd)) < 0)
		die("Failed to dispatch command\n");

	close(fd);

	return error;
}

/* vim: set tabstop=8 noexpandtab : */
