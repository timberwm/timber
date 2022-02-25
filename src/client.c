/*
 * Copyright (C) Patrick Steinhardt, 2019-2021
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

#include <wayland-client.h>
#include <wlr/types/wlr_keyboard.h>

#include "timber.h"
#include "timber-client-protocol.h"

#define ARRAY_FIND(array, i, cmp) \
	for (i = 0; i < (ssize_t)ARRAY_SIZE(array); i++) \
		if (cmp) \
			break; \
	if (i == ARRAY_SIZE(array)) \
		i = -1;

#define TMBR_ARG_SEL    (1 << 0)
#define TMBR_ARG_DIR    (1 << 1)
#define TMBR_ARG_INT    (1 << 2)
#define TMBR_ARG_KEY    (1 << 3)
#define TMBR_ARG_CMD    (1 << 4)

static const struct {
	const char *cmd;
	const char *subcmd;
	int function;
	int args;
} commands[] = {
	{ "client", "focus",      TMBR_CTRL_CLIENT_FOCUS,      TMBR_ARG_SEL                  },
	{ "client", "fullscreen", TMBR_CTRL_CLIENT_FULLSCREEN, 0                             },
	{ "client", "kill",       TMBR_CTRL_CLIENT_KILL,       0                             },
	{ "client", "resize",     TMBR_CTRL_CLIENT_RESIZE,     TMBR_ARG_DIR|TMBR_ARG_INT     },
	{ "client", "swap",       TMBR_CTRL_CLIENT_SWAP,       TMBR_ARG_SEL                  },
	{ "client", "to_desktop", TMBR_CTRL_CLIENT_TO_DESKTOP, TMBR_ARG_SEL                  },
	{ "client", "to_screen",  TMBR_CTRL_CLIENT_TO_SCREEN,  TMBR_ARG_SEL                  },
	{ "desktop", "focus",     TMBR_CTRL_DESKTOP_FOCUS,     TMBR_ARG_SEL                  },
	{ "desktop", "kill",      TMBR_CTRL_DESKTOP_KILL,      0                             },
	{ "desktop", "new",       TMBR_CTRL_DESKTOP_NEW,       0                             },
	{ "desktop", "swap",      TMBR_CTRL_DESKTOP_SWAP,      TMBR_ARG_SEL                  },
	{ "screen", "focus",      TMBR_CTRL_SCREEN_FOCUS,      TMBR_ARG_SEL                  },
	{ "tree", "rotate",       TMBR_CTRL_TREE_ROTATE,       0                             },
	{ "state", "query",       TMBR_CTRL_STATE_QUERY,       0                             },
	{ "state", "quit",        TMBR_CTRL_STATE_QUIT,        0                             },
	{ "binding", "add",       TMBR_CTRL_BINDING_ADD,       TMBR_ARG_KEY|TMBR_ARG_CMD     }
};

struct tmbr_arg {
	int function;
	enum tmbr_ctrl_selection sel;
	enum tmbr_ctrl_direction dir;
	int i;
	struct { uint32_t modifiers; xkb_keysym_t keycode; } key;
	const char *command;
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

static const char * const directions[] = { "north", "south", "east", "west" };
static const char * const selections[] = { "prev", "next" };

static void tmbr_parse(struct tmbr_arg *out, int argc, char **argv)
{
	ssize_t c, i;

	if (argc < 1)
		die("Missing command");
	if (argc < 2)
		die("Missing subcommand");

	ARRAY_FIND(commands, c, !strcmp(commands[c].cmd, argv[0]) && !strcmp(commands[c].subcmd, argv[1]));
	if (c < 0)
		die("Unknown command '%s %s'", argv[0], argv[1]);
	out->function = commands[c].function;

	argc -= 2;
	argv += 2;

	if (commands[c].args & TMBR_ARG_SEL) {
		if (!argc)
			die("Command is missing selection");
		ARRAY_FIND(selections, i, !strcmp(argv[0], selections[i]));
		if (i < 0)
			die("Unknown selection '%s'", argv[0]);
		out->sel = (enum tmbr_ctrl_selection) i;
		argc--;
		argv++;
	}

	if (commands[c].args & TMBR_ARG_DIR) {
		if (!argc)
			die("Command is missing direction");
		ARRAY_FIND(directions, i, !strcmp(argv[0], directions[i]));
		if (i < 0)
			die("Unknown direction '%s'", argv[0]);
		out->dir = (enum tmbr_ctrl_direction) i;
		argc--;
		argv++;
	}

	if (commands[c].args & TMBR_ARG_INT) {
		if (!argc)
			die("Command is missing integer");
		out->i = atoi(argv[0]);
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
				out->key.modifiers |= modmasks[i].modifier;
				continue;
			}

			if ((out->key.keycode = xkb_keysym_from_name(key, 0)) == 0)
				die("Unable to parse key '%s'", key);
		}
		if (!out->key.keycode)
			die("Binding requires a key");

		argc--;
		argv++;
	}

	if (commands[c].args & TMBR_ARG_CMD) {
		if (!argc)
			die("Command is missing command line");
		out->command = argv[0];
		argc--;
		argv++;
	}

	if (argc)
		die("Command has trailing arguments");
}

static void tmbr_client_on_global(void *data, struct wl_registry *registry, uint32_t id, const char *interface, uint32_t version)
{
	if (!strcmp(interface, tmbr_ctrl_interface.name)) {
		struct tmbr_ctrl **cmd = data;
		if ((int)version != tmbr_ctrl_interface.version)
			die("Incompatible control protocol versions (server: v%d, client: v%d)", version, tmbr_ctrl_interface.version);
		if ((*cmd = wl_registry_bind(registry, id, &tmbr_ctrl_interface, version)) == NULL)
			die("Could not bind timber control");
	}
}

static void __attribute__((noreturn)) usage(const char *executable)
{
	size_t i;
	printf("USAGE: %s [--help] [--version] <command> [<args>]\n\n", executable);

	puts("These are the availabe commands:\n");

	printf("   %s run\n", executable);
	for (i = 0; i < ARRAY_SIZE(commands); i++)
		printf("   %s %s %s%s%s%s%s%s\n", executable, commands[i].cmd, commands[i].subcmd,
			commands[i].args & TMBR_ARG_SEL ? " (next|prev)" : "",
			commands[i].args & TMBR_ARG_DIR ? " (north|south|east|west)" : "",
			commands[i].args & TMBR_ARG_INT ? " <NUMBER>" : "",
			commands[i].args & TMBR_ARG_KEY ? " <KEY>" : "",
			commands[i].args & TMBR_ARG_CMD ? " <COMMAND>" : "");

	exit(0);
}

static void __attribute__((noreturn)) version(void)
{
	puts("timber version " TMBR_VERSION);
	exit(0);
}

int tmbr_client(int argc, char *argv[])
{
	const struct wl_registry_listener listener = {
		.global = tmbr_client_on_global,
	};
	struct wl_display *display;
	struct tmbr_ctrl *ctrl = NULL;
	struct tmbr_arg args = { 0 };
	uint32_t error = 0;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--help"))
			usage(argv[0]);
		else if (!strcmp(argv[i], "--version"))
			version();
	}

	tmbr_parse(&args, argc - 1, argv + 1);

	if ((display = wl_display_connect(NULL)) == NULL)
		die("Could not connect to display");

	wl_registry_add_listener(wl_display_get_registry(display), &listener, &ctrl);
	if (wl_display_roundtrip(display) < 0 || !ctrl)
		die("Could not discover timber control");

	switch (args.function) {
		case TMBR_CTRL_CLIENT_FOCUS: tmbr_ctrl_client_focus(ctrl, args.sel); break;
		case TMBR_CTRL_CLIENT_FULLSCREEN: tmbr_ctrl_client_fullscreen(ctrl); break;
		case TMBR_CTRL_CLIENT_KILL: tmbr_ctrl_client_kill(ctrl); break;
		case TMBR_CTRL_CLIENT_RESIZE: tmbr_ctrl_client_resize(ctrl, args.dir, args.i); break;
		case TMBR_CTRL_CLIENT_SWAP: tmbr_ctrl_client_swap(ctrl, args.sel); break;
		case TMBR_CTRL_CLIENT_TO_DESKTOP: tmbr_ctrl_client_to_desktop(ctrl, args.sel); break;
		case TMBR_CTRL_CLIENT_TO_SCREEN: tmbr_ctrl_client_to_screen(ctrl, args.sel); break;
		case TMBR_CTRL_DESKTOP_FOCUS: tmbr_ctrl_desktop_focus(ctrl, args.sel); break;
		case TMBR_CTRL_DESKTOP_KILL: tmbr_ctrl_desktop_kill(ctrl); break;
		case TMBR_CTRL_DESKTOP_NEW: tmbr_ctrl_desktop_new(ctrl); break;
		case TMBR_CTRL_DESKTOP_SWAP: tmbr_ctrl_desktop_swap(ctrl, args.sel); break;
		case TMBR_CTRL_SCREEN_FOCUS: tmbr_ctrl_screen_focus(ctrl, args.sel); break;
		case TMBR_CTRL_TREE_ROTATE: tmbr_ctrl_tree_rotate(ctrl); break;
		case TMBR_CTRL_STATE_QUERY: tmbr_ctrl_state_query(ctrl, STDOUT_FILENO); break;
		case TMBR_CTRL_STATE_QUIT: tmbr_ctrl_state_quit(ctrl); break;
		case TMBR_CTRL_BINDING_ADD: tmbr_ctrl_binding_add(ctrl, args.key.keycode, args.key.modifiers, args.command); break;
	}

	if (wl_display_roundtrip(display) < 0) {
		if (errno != EPROTO)
			die("Could not send request: %s", strerror(errno));
		error = wl_display_get_protocol_error(display, NULL, NULL);
	}

	wl_display_disconnect(display);

	return error;
}
