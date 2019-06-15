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
#include <string.h>

#include "common.h"

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
	{ "screen", "focus",      TMBR_ARG_SEL              },
	{ "tree", "rotate",       0                         }
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

int tmbr_command_parse(tmbr_command_t *cmd, tmbr_command_args_t *args, int argc, const char **argv)
{
	ssize_t c, i;

	if (argc < 2)
		return -1;

	ARRAY_FIND(commands, c, !strcmp(commands[c].cmd, argv[0]) && !strcmp(commands[c].subcmd, argv[1]));
	if (c < 0)
		return -1;
	*cmd = (tmbr_command_t) c;

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

/* vim: set tabstop=8 noexpandtab : */
