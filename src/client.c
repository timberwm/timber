/*
 * Copyright (C) Patrick Steinhardt, 2019-2024
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

struct tmbr_key {
	uint32_t modifiers;
	xkb_keysym_t keycode;
};

static const char * const directions[] = { "north", "south", "east", "west" };
static const char * const selections[] = { "prev", "next" };

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

static enum tmbr_ctrl_selection tmbr_parse_selection(const char *arg)
{
	ssize_t i;

	if (!arg)
		die("Command is missing selection");

	ARRAY_FIND(selections, i, !strcmp(arg, selections[i]));
	if (i < 0)
		die("Unknown selection '%s'", arg);

	return (enum tmbr_ctrl_selection) i;
}

static enum tmbr_ctrl_direction tmbr_parse_direction(const char *arg)
{
	ssize_t i;

	if (!arg)
		die("Command is missing direction");

	ARRAY_FIND(directions, i, !strcmp(arg, directions[i]));
	if (i < 0)
		die("Unknown direction '%s'", arg);

	return (enum tmbr_ctrl_direction) i;
}

static int32_t tmbr_parse_i32(const char *arg)
{
	long value;
	char *end;

	if (!arg)
		die("Command is missing integer");

	value = strtol(arg, &end, 10);
	if (*end)
		die("Integer is not a number");
	if (value < INT32_MIN)
		die("Integer exceeds minimum range");
	if (value > INT32_MAX)
		die("Integer exceeds maximum range");

	return value;
}

static uint32_t tmbr_parse_u32(const char *arg)
{
	unsigned long value;
	char *end;

	if (!arg)
		die("Command is missing integer");

	value = strtoul(arg, &end, 10);
	if (*end)
		die("Integer is not a number");
	if (value > UINT32_MAX)
		die("Integer exceeds maximum range");

	return value;
}

static uint32_t tmbr_parse_bool(const char *arg)
{
	if (!arg)
		die("Command is missing boolean");

	if (!strcmp(arg, "enabled"))
		return 1;
	else if (!strcmp(arg, "disabled"))
		return 0;
	else
		die("Boolean is not one of enabled or disabled");
}

static struct tmbr_key tmbr_parse_key(char *arg)
{
	struct tmbr_key key = { 0 };
	char *keysym;

	if (!arg)
		die("Command is missing key");

	for (keysym = strtok(arg, "+"); keysym; keysym = strtok(NULL, "+")) {
		ssize_t i;

		ARRAY_FIND(modmasks, i, !strcmp(keysym, modmasks[i].name));
		if (i >= 0) {
			key.modifiers |= modmasks[i].modifier;
			continue;
		}

		if ((key.keycode = xkb_keysym_from_name(keysym, 0)) == 0)
			die("Unable to parse key '%s'", keysym);
	}
	if (!key.keycode)
		die("Binding requires a key");

	return key;
}

static uint32_t tmbr_parse_color(const char *arg)
{
	uint32_t color;
	char *endptr;

	if (!arg)
		die("Command is missing color");
	if (strlen(arg) != 8)
		die("Color is expected to be in RGBA8888 format");

	color = strtoul(arg, &endptr, 16);
	if (*endptr)
		die("Argument is not a base-16 number");

	return color;
}

static void tmbr_require_args(char **argv, size_t length)
{
	size_t i;
	for (i = 0; argv[i]; i++);
	if (i != length)
		die("Expected %"PRIuMAX" arguments, got %"PRIuMAX,
		    (uintmax_t) length, (uintmax_t) i);
}

static void tmbr_client_focus(struct wl_display *display TMBR_UNUSED,
			      struct tmbr_ctrl *ctrl, char **argv)
{
	tmbr_require_args(argv, 1);
	tmbr_ctrl_client_focus(ctrl, tmbr_parse_selection(argv[0]));
}

static void tmbr_client_fullscreen(struct wl_display *display TMBR_UNUSED,
				   struct tmbr_ctrl *ctrl, char **argv)
{
	tmbr_require_args(argv, 0);
	tmbr_ctrl_client_fullscreen(ctrl);
}

static void tmbr_client_kill(struct wl_display *display TMBR_UNUSED,
			     struct tmbr_ctrl *ctrl, char **argv)
{
	tmbr_require_args(argv, 0);
	tmbr_ctrl_client_kill(ctrl);
}

static void tmbr_client_resize(struct wl_display *display TMBR_UNUSED,
			       struct tmbr_ctrl *ctrl, char **argv)
{
	tmbr_require_args(argv, 2);
	tmbr_ctrl_client_resize(ctrl, tmbr_parse_direction(argv[0]), tmbr_parse_i32(argv[1]));
}

static void tmbr_client_swap(struct wl_display *display TMBR_UNUSED,
			     struct tmbr_ctrl *ctrl, char **argv)
{
	tmbr_require_args(argv, 1);
	tmbr_ctrl_client_swap(ctrl, tmbr_parse_selection(argv[0]));
}

static void tmbr_client_to_desktop(struct wl_display *display TMBR_UNUSED,
				   struct tmbr_ctrl *ctrl, char **argv)
{
	tmbr_require_args(argv, 1);
	tmbr_ctrl_client_to_desktop(ctrl, tmbr_parse_selection(argv[0]));
}

static void tmbr_client_to_output(struct wl_display *display TMBR_UNUSED,
				  struct tmbr_ctrl *ctrl, char **argv)
{
	tmbr_require_args(argv, 1);
	tmbr_ctrl_client_to_output(ctrl, tmbr_parse_selection(argv[0]));
}

static void tmbr_desktop_focus(struct wl_display *display TMBR_UNUSED,
			       struct tmbr_ctrl *ctrl, char **argv)
{
	tmbr_require_args(argv, 1);
	tmbr_ctrl_desktop_focus(ctrl, tmbr_parse_selection(argv[0]));
}

static void tmbr_desktop_kill(struct wl_display *display TMBR_UNUSED,
			      struct tmbr_ctrl *ctrl, char **argv)
{
	tmbr_require_args(argv, 0);
	tmbr_ctrl_desktop_kill(ctrl);
}

static void tmbr_desktop_new(struct wl_display *display TMBR_UNUSED,
			     struct tmbr_ctrl *ctrl, char **argv)
{
	tmbr_require_args(argv, 0);
	tmbr_ctrl_desktop_new(ctrl);
}

static void tmbr_desktop_swap(struct wl_display *display TMBR_UNUSED,
			      struct tmbr_ctrl *ctrl, char **argv)
{
	tmbr_require_args(argv, 1);
	tmbr_ctrl_desktop_swap(ctrl, tmbr_parse_selection(argv[0]));
}

static void tmbr_output_focus(struct wl_display *display TMBR_UNUSED,
			      struct tmbr_ctrl *ctrl, char **argv)
{
	tmbr_require_args(argv, 1);
	tmbr_ctrl_output_focus(ctrl, tmbr_parse_selection(argv[0]));
}

static void tmbr_tree_rotate(struct wl_display *display TMBR_UNUSED,
			     struct tmbr_ctrl *ctrl, char **argv)
{
	tmbr_require_args(argv, 0);
	tmbr_ctrl_tree_rotate(ctrl);
}

static void tmbr_state_query(struct wl_display *display TMBR_UNUSED,
			     struct tmbr_ctrl *ctrl, char **argv)
{
	tmbr_require_args(argv, 0);
	tmbr_ctrl_state_query(ctrl, STDOUT_FILENO);
}

static void tmbr_state_quit(struct wl_display *display TMBR_UNUSED,
			    struct tmbr_ctrl *ctrl, char **argv)
{
	tmbr_require_args(argv, 0);
	tmbr_ctrl_state_quit(ctrl);
}

static void tmbr_binding_add(struct wl_display *display TMBR_UNUSED,
			     struct tmbr_ctrl *ctrl, char **argv)
{
	struct tmbr_key key;
	tmbr_require_args(argv, 2);
	key = tmbr_parse_key(argv[0]);
	tmbr_ctrl_binding_add(ctrl, key.keycode, key.modifiers, argv[1]);
}

static void tmbr_client_on_config(void *payload,
				  struct tmbr_ctrl *ctrl TMBR_UNUSED,
				  uint32_t border_width,
				  uint32_t border_color_active,
				  uint32_t border_color_inactive,
				  uint32_t gap,
				  uint32_t tap_to_click,
				  uint32_t natural_scroll,
				  uint32_t dwt)
{
	struct tmbr_config cfg = {
		.border_width = border_width,
		.border_color_active = border_color_active,
		.border_color_inactive = border_color_inactive,
		.gap = gap,
		.tap_to_click = tap_to_click,
		.natural_scroll = natural_scroll,
		.dwt = dwt,
	};
	struct tmbr_config **out = payload;
	*out = tmbr_alloc(sizeof(**out), "Could not allocate config");
	**out = cfg;
}

static struct tmbr_config *tmbr_client_receive_config(struct wl_display *display,
						      struct tmbr_ctrl *ctrl)
{
	struct tmbr_ctrl_listener listener = {
		.config = tmbr_client_on_config,
	};
	struct tmbr_config *cfg = NULL;

	tmbr_ctrl_add_listener(ctrl, &listener, &cfg);
	tmbr_ctrl_config_get(ctrl);

	while (!cfg)
		wl_display_dispatch(display);

	return cfg;
}

static const char *tmbr_config_to_bool(uint32_t enabled)
{
	return enabled ? "enabled" : "disabled";
}

static void tmbr_config_get(struct wl_display *display TMBR_UNUSED,
			    struct tmbr_ctrl *ctrl, char **argv)
{
	struct tmbr_config *cfg;

	tmbr_require_args(argv, 0);

	cfg = tmbr_client_receive_config(display, ctrl);
	printf("border_width: %" PRIu32 "\n", cfg->border_width);
	printf("border_color_active: %" PRIx32 "\n", cfg->border_color_active);
	printf("border_color_inactive: %" PRIx32 "\n", cfg->border_color_inactive);
	printf("gap: %" PRIu32 "\n", cfg->gap);
	printf("tap_to_click: %s\n", tmbr_config_to_bool(cfg->tap_to_click));
	printf("natural_scroll: %s\n", tmbr_config_to_bool(cfg->natural_scroll));
	printf("dwt: %s\n", tmbr_config_to_bool(cfg->dwt));

	free(cfg);
}

static bool skip_prefix(char *arg, const char *prefix, char **out)
{
	size_t prefixlen = strlen(prefix);
	if (!strncmp(arg, prefix, prefixlen)) {
		*out = arg + prefixlen;
		return true;
	}
	return false;
}

static void tmbr_config_set(struct wl_display *display TMBR_UNUSED,
			    struct tmbr_ctrl *ctrl, char **argv)
{
	struct tmbr_config *cfg = tmbr_client_receive_config(display, ctrl);

	for (size_t i = 0; argv[i]; i++) {
		char *arg = argv[i], *value;

		if (skip_prefix(arg, "border_width=", &value))
			cfg->border_width = tmbr_parse_u32(value);
		else if (skip_prefix(arg, "border_color_active=", &value))
			cfg->border_color_active = tmbr_parse_color(value);
		else if (skip_prefix(arg, "border_color_inactive=", &value))
			cfg->border_color_inactive = tmbr_parse_color(value);
		else if (skip_prefix(arg, "gap=", &value))
			cfg->gap = tmbr_parse_u32(value);
		else if (skip_prefix(arg, "tap_to_click=", &value))
			cfg->tap_to_click = tmbr_parse_bool(value);
		else if (skip_prefix(arg, "natural_scroll=", &value))
			cfg->natural_scroll = tmbr_parse_bool(value);
		else if (skip_prefix(arg, "dwt=", &value))
			cfg->dwt = tmbr_parse_bool(value);
		else
			die("Unknown config key '%s'", arg);
	}

	tmbr_ctrl_config_set(ctrl,
			     cfg->border_width,
			     cfg->border_color_active,
			     cfg->border_color_inactive,
			     cfg->gap,
			     cfg->tap_to_click,
			     cfg->natural_scroll,
			     cfg->dwt);

	free(cfg);
}

static const struct {
	const char *cmd;
	const char *subcmd;
	const char *argh;
	void (*fn)(struct wl_display *display, struct tmbr_ctrl *ctrl, char **argv);
} commands[] = {
	{ "client", "focus",      "(next|prev)",      tmbr_client_focus },
	{ "client", "fullscreen", NULL,               tmbr_client_fullscreen },
	{ "client", "kill",       NULL,               tmbr_client_kill },
	{ "client", "resize",     "(next|prev) <px>", tmbr_client_resize },
	{ "client", "swap",       "(next|prev)",      tmbr_client_swap, },
	{ "client", "to_desktop", "(next|prev)",      tmbr_client_to_desktop },
	{ "client", "to_output",  "(next|prev)",      tmbr_client_to_output },
	{ "desktop", "focus",     "(next|prev)",      tmbr_desktop_focus },
	{ "desktop", "kill",      NULL,               tmbr_desktop_kill },
	{ "desktop", "new",       NULL,               tmbr_desktop_new },
	{ "desktop", "swap",      "(next|prev)",      tmbr_desktop_swap },
	{ "output", "focus",      "(next|prev)",      tmbr_output_focus },
	{ "tree", "rotate",       NULL,               tmbr_tree_rotate },
	{ "state", "query",       NULL,               tmbr_state_query },
	{ "state", "quit",        NULL,               tmbr_state_quit },
	{ "binding", "add",       "<key> <command>",  tmbr_binding_add },
	{ "config", "get",        NULL,               tmbr_config_get },
	{ "config", "set",        "<key=value...>",   tmbr_config_set },
};

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
	for (i = 0; i < ARRAY_SIZE(commands); i++) {
		printf("   %s %s %s %s\n", executable, commands[i].cmd, commands[i].subcmd,
		       commands[i].argh ? commands[i].argh : "");
	}

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
	uint32_t error = 0;
	ssize_t command;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--help"))
			usage(argv[0]);
		else if (!strcmp(argv[i], "--version"))
			version();
	}

	if (!argv[1])
		die("Missing command");
	if (!argv[2])
		die("Missing subcommand");

	ARRAY_FIND(commands, command, !strcmp(commands[command].cmd, argv[1]) && !strcmp(commands[command].subcmd, argv[2]));
	if (command < 0)
		die("Unknown command '%s %s'", argv[0], argv[1]);

	if ((display = wl_display_connect(NULL)) == NULL)
		die("Could not connect to display");

	wl_registry_add_listener(wl_display_get_registry(display), &listener, &ctrl);
	if (wl_display_roundtrip(display) < 0 || !ctrl)
		die("Could not discover timber control");

	commands[command].fn(display, ctrl, argv + 3);

	if (wl_display_roundtrip(display) < 0) {
		if (errno != EPROTO)
			die("Could not send request: %s", strerror(errno));
		error = wl_display_get_protocol_error(display, NULL, NULL);
	}

	wl_display_disconnect(display);

	return error;
}
