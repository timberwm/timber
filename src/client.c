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
#include <unistd.h>

#include "client.h"
#include "common.h"
#include "config.h"

typedef int (*tmbr_cmd_t)(int argc, const char *argv[]);

static int tmbr_execute(tmbr_command_t cmd, const tmbr_command_args_t *args, int fd)
{
	char params[3][32] = { "", "", "" };
	int error = EINVAL;
	tmbr_pkt_t pkt;

	if (commands[cmd].args & TMBR_ARG_SEL)
		snprintf(params[0], sizeof(params[0]), " %s", selections[args->sel]);
	if (commands[cmd].args & TMBR_ARG_DIR)
		snprintf(params[1], sizeof(params[1]), " %s", directions[args->dir]);
	if (commands[cmd].args & TMBR_ARG_INT)
		snprintf(params[2], sizeof(params[2]), " %i", args->i);

	if (tmbr_ctrl_write(fd, TMBR_PKT_COMMAND, "%s %s%s%s%s", commands[cmd].cmd,
			    commands[cmd].subcmd, params[0], params[1], params[2]) < 0 ||
	    tmbr_ctrl_read(fd, &pkt) < 0)
		return -1;

	if (pkt.type != TMBR_PKT_ERROR || (error = atoi(pkt.message)) != 0)
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

	if ((fd = tmbr_ctrl_connect(NULL, 0)) < 0)
		die("Unable to connect to control socket");

	if ((error = tmbr_execute(cmd, &args, fd)) < 0)
		die("Failed to dispatch command\n");

	close(fd);

	return error;
}
