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

#include <fcntl.h>
#include <sys/stat.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "client.h"
#include "common.h"
#include "config.h"

typedef int (*tmbr_cmd_t)(int argc, const char *argv[]);

static int tmbr_ctrl_connect(const char *path)
{
	int fd = open(path, O_WRONLY);
	return fd;
}

static int tmbr_dispatch_command(tmbr_command_t cmd, const tmbr_command_args_t *args, int fd)
{
	char params[3][32] = { "", "", "" };

	if (commands[cmd].args & TMBR_ARG_SEL)
		snprintf(params[0], sizeof(params[0]), " %s", selections[args->sel]);
	if (commands[cmd].args & TMBR_ARG_DIR)
		snprintf(params[1], sizeof(params[1]), " %s", directions[args->dir]);
	if (commands[cmd].args & TMBR_ARG_INT)
		snprintf(params[2], sizeof(params[2]), " %i", args->i);

	dprintf(fd, "%s %s%s%s%s\n", commands[cmd].cmd, commands[cmd].subcmd, params[0], params[1], params[2]);

	return 0;
}

int tmbr_client(int argc, const char *argv[])
{
	tmbr_command_args_t args;
	tmbr_command_t cmd;
	int fd;

	if ((tmbr_command_parse(&cmd, &args, argc - 1, argv + 1)) < 0)
		usage(argv[0]);

	if ((fd = tmbr_ctrl_connect(TMBR_CTRL_PATH)) < 0)
		die("Unable to connect to control socket");

	if (tmbr_dispatch_command(cmd, &args, fd) < 0)
		printf("Failed to dispatch command\n");

	close(fd);

	return 0;
}

/* vim: set tabstop=8 noexpandtab : */
