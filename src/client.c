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

#include <wlr/types/wlr_keyboard.h>

#include "client.h"
#include "common.h"
#include "config.h"

#define ARRAY_FIND(array, i, cmp) \
	for (i = 0; i < (ssize_t)ARRAY_SIZE(array); i++) \
		if (cmp) \
			break; \
	if (i == ARRAY_SIZE(array)) \
		i = -1;

#define TMBR_ARG_SCREEN (1 << 0)
#define TMBR_ARG_SEL    (1 << 1)
#define TMBR_ARG_DIR    (1 << 2)
#define TMBR_ARG_INT    (1 << 3)
#define TMBR_ARG_KEY    (1 << 4)
#define TMBR_ARG_CMD    (1 << 5)
#define TMBR_ARG_MODE   (1 << 6)

static const struct {
	const char *cmd;
	const char *subcmd;
	int args;
} commands[] = {
	{ "client", "focus",      TMBR_ARG_SEL                  },
	{ "client", "fullscreen", 0                             },
	{ "client", "kill",       0                             },
	{ "client", "resize",     TMBR_ARG_DIR|TMBR_ARG_INT     },
	{ "client", "swap",       TMBR_ARG_SEL                  },
	{ "client", "to_desktop", TMBR_ARG_SEL                  },
	{ "client", "to_screen",  TMBR_ARG_SEL                  },
	{ "desktop", "focus",     TMBR_ARG_SEL                  },
	{ "desktop", "kill",      0                             },
	{ "desktop", "new",       0                             },
	{ "desktop", "swap",      TMBR_ARG_SEL                  },
	{ "screen", "focus",      TMBR_ARG_SEL                  },
	{ "screen", "scale",      TMBR_ARG_SCREEN|TMBR_ARG_INT  },
	{ "screen", "mode",       TMBR_ARG_SCREEN|TMBR_ARG_MODE },
	{ "tree", "rotate",       0                             },
	{ "state", "subscribe",   0                             },
	{ "state", "query",       0                             },
	{ "state", "stop",        0                             },
	{ "binding", "add",       TMBR_ARG_KEY|TMBR_ARG_CMD     }
};

static const struct {
	const char *name;
	enum wlr_keyboard_modifier modifier;
} modmasks[] = {
	{ "shift", WLR_MODIFIER_SHIFT },
	{ "caps",  WLR_MODIFIER_CAPS  },
	{ "ctrl",  WLR_MODIFIER_CTRL  },
	{ "alt",   WLR_MODIFIER_ALT   },
	{ "mod2",  WLR_MODIFIER_MOD2  },
	{ "mod3",  WLR_MODIFIER_MOD3  },
	{ "logo",  WLR_MODIFIER_LOGO  },
	{ "mod5",  WLR_MODIFIER_MOD5  }
};

static const char *directions[] = { "north", "south", "east", "west" };
static const char *selections[] = { "prev", "next" };

typedef int (*tmbr_cmd_t)(int argc, const char *argv[]);

static int tmbr_execute(const tmbr_command_t *cmd, int fd)
{
	tmbr_pkt_t pkt;
	int error;

	memset(&pkt, 0, sizeof(pkt));
	pkt.type = TMBR_PKT_COMMAND;
	pkt.u.command = *cmd;

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
		die("Error executing command: %s", strerror(pkt.u.error));

	return 0;
}

static void tmbr_parse(tmbr_command_t *cmd, int argc, char **argv)
{
	ssize_t c, i;

	if (argc < 1)
		die("Missing command");
	if (argc < 2)
		die("Missing subcommand");

	ARRAY_FIND(commands, c, !strcmp(commands[c].cmd, argv[0]) && !strcmp(commands[c].subcmd, argv[1]));
	if (c < 0)
		die("Unknown command '%s %s'", argv[0], argv[1]);
	cmd->type = (tmbr_command_type_t) c;

	argc -= 2;
	argv += 2;

	if (commands[c].args & TMBR_ARG_SCREEN) {
		size_t len;
		if (!argc)
			die("Command is missing screen");
		if ((len = strlen(argv[0])) >= sizeof(cmd->screen))
			die("Screen length exceeds maximum");
		memcpy(cmd->screen, argv[0], len + 1);
		argc--;
		argv++;
	}

	if (commands[c].args & TMBR_ARG_SEL) {
		if (!argc)
			die("Command is missing selection");
		ARRAY_FIND(selections, i, !strcmp(argv[0], selections[i]));
		if (i < 0)
			die("Unknown selection '%s'", argv[0]);
		cmd->sel = (tmbr_select_t) i;
		argc--;
		argv++;
	}

	if (commands[c].args & TMBR_ARG_DIR) {
		if (!argc)
			die("Command is missing direction");
		ARRAY_FIND(directions, i, !strcmp(argv[0], directions[i]));
		if (i < 0)
			die("Unknown direction '%s'", argv[0]);
		cmd->dir = (tmbr_dir_t) i;
		argc--;
		argv++;
	}

	if (commands[c].args & TMBR_ARG_INT) {
		if (!argc)
			die("Command is missing integer");
		cmd->i = atoi(argv[0]);
		argc--;
		argv++;
	}

	if (commands[c].args & TMBR_ARG_KEY) {
		char *key;

		if (!argc)
			die("Command is missing key");

		for (key = strtok(argv[0], "+"); key; key = strtok(NULL, "+")) {
			ARRAY_FIND(modmasks, i, !strcmp(key, modmasks[i].name));
			if (i >= 0) {
				cmd->key.modifiers |= modmasks[i].modifier;
				continue;
			}

			if ((cmd->key.keycode = xkb_keysym_from_name(key, 0)) == 0)
				die("Unable to parse key '%s'", key);
		}
		if (!cmd->key.keycode)
			die("Binding requires a key");

		argc--;
		argv++;
	}

	if (commands[c].args & TMBR_ARG_CMD) {
		size_t len;
		if (!argc)
			die("Command is missing command line");
		if ((len = strlen(argv[0])) >= sizeof(cmd->command))
			die("Command length exceeds maximum");
		memcpy(cmd->command, argv[0], len + 1);
		argc--;
		argv++;
	}

	if (commands[c].args & TMBR_ARG_MODE) {
		if (!argc)
			die("Command is missing mode");
		if (sscanf(argv[0], "%dx%d@%d", &cmd->mode.width, &cmd->mode.height, &cmd->mode.refresh) != 3)
			die("Invalid mode '%s'", argv[0]);
		argc--;
		argv++;
	}

	if (argc)
		die("Command has trailing arguments");
}

static void __attribute__((noreturn)) usage(const char *executable)
{
	size_t i;
	printf("USAGE: %s\n", executable);

	for (i = 0; i < ARRAY_SIZE(commands); i++)
		printf("   or: %s %s %s%s%s%s%s%s%s%s\n", executable, commands[i].cmd, commands[i].subcmd,
			commands[i].args & TMBR_ARG_SCREEN ? " <SCREEN>" : "",
			commands[i].args & TMBR_ARG_SEL ? " (next|prev)" : "",
			commands[i].args & TMBR_ARG_DIR ? " (north|south|east|west)" : "",
			commands[i].args & TMBR_ARG_INT ? " <NUMBER>" : "",
			commands[i].args & TMBR_ARG_KEY ? " <KEY>" : "",
			commands[i].args & TMBR_ARG_CMD ? " <COMMAND>" : "",
			commands[i].args & TMBR_ARG_MODE ? " <WIDTH>x<HEIGHT>@<REFRESH>" : "");

	exit(-1);
}

int tmbr_client(int argc, char *argv[])
{
	tmbr_command_t cmd;
	int error, fd, i;

	for (i = 1; i < argc; i++)
		if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help"))
			usage(argv[0]);

	memset(&cmd, 0, sizeof(cmd));
	tmbr_parse(&cmd, argc - 1, argv + 1);

	if ((fd = tmbr_ctrl_connect(0)) < 0)
		die("Unable to connect to control socket");

	if ((error = tmbr_execute(&cmd, fd)) < 0)
		die("Failed to dispatch command");

	close(fd);

	return error;
}
