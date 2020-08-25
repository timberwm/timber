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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "client.h"
#include "common.h"
#include "config.h"

typedef int (*tmbr_cmd_t)(int argc, const char *argv[]);

static int tmbr_execute(const tmbr_command_args_t *args, int fd)
{
	tmbr_pkt_t pkt = {0};
	int error;

	pkt.type = TMBR_PKT_COMMAND;
	pkt.u.command = *args;

	if (tmbr_ctrl_write(fd, &pkt) < 0)
		return -1;

	while ((error = tmbr_ctrl_read(fd, &pkt)) == 0) {
		if (pkt.type != TMBR_PKT_DATA)
			break;
		puts(pkt.u.data);
	}
	if (error < 0)
		die("Could not read control packet");
	if (pkt.type != TMBR_PKT_ERROR)
		die("Received unexpected control packet from server");
	if (pkt.u.error != 0)
		die("Error executing command: %s", strerror(error));

	return 0;
}

int tmbr_client(int argc, const char *argv[])
{
	tmbr_command_args_t args;
	int error, fd;

	if ((tmbr_command_parse(&args, argc - 1, argv + 1)) < 0)
		usage(argv[0]);

	if ((fd = tmbr_ctrl_connect(NULL, 0)) < 0)
		die("Unable to connect to control socket");

	if ((error = tmbr_execute(&args, fd)) < 0)
		die("Failed to dispatch command");

	close(fd);

	return error;
}
