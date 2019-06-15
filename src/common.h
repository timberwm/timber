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

#define TMBR_UNUSED __attribute__((unused))

#define ARRAY_SIZE(a) (sizeof(a) / sizeof(*a))
#define ARRAY_FIND(array, i, cmp) \
	for (i = 0; i < (ssize_t)ARRAY_SIZE(array); i++) \
		if (!cmp) \
			break; \
	if (i == ARRAY_SIZE(array)) \
		i = -1;

#define TMBR_ARG_SEL (1 << 1)
#define TMBR_ARG_DIR (1 << 2)
#define TMBR_ARG_INT (1 << 3)

typedef enum {
	TMBR_COMMAND_CLIENT_FOCUS,
	TMBR_COMMAND_CLIENT_FULLSCREEN,
	TMBR_COMMAND_CLIENT_KILL,
	TMBR_COMMAND_CLIENT_RESIZE,
	TMBR_COMMAND_CLIENT_SWAP,
	TMBR_COMMAND_CLIENT_TO_DESKTOP,
	TMBR_COMMAND_CLIENT_TO_SCREEN,
	TMBR_COMMAND_DESKTOP_FOCUS,
	TMBR_COMMAND_DESKTOP_KILL,
	TMBR_COMMAND_DESKTOP_NEW,
	TMBR_COMMAND_SCREEN_FOCUS,
	TMBR_COMMAND_TREE_ROTATE
} tmbr_command_t;

typedef enum {
	TMBR_DIR_NORTH,
	TMBR_DIR_SOUTH,
	TMBR_DIR_EAST,
	TMBR_DIR_WEST
} tmbr_dir_t;

typedef enum {
	TMBR_SELECT_PREV,
	TMBR_SELECT_NEXT,
	TMBR_SELECT_NEAREST
} tmbr_select_t;

typedef struct {
	tmbr_select_t sel;
	tmbr_dir_t dir;
	int i;
} tmbr_command_args_t;

typedef struct {
	const char *command;
	int args;
} tmbr_commands_t;

extern const tmbr_commands_t commands[];
extern const char *directions[];
extern const char *selections[];

void __attribute__((noreturn, format(printf, 1, 2))) die(const char *fmt, ...);
void __attribute__((noreturn)) usage(const char *executable);

int tmbr_command_parse(tmbr_command_t *cmd, tmbr_command_args_t *args, int argc, const char *argv[]);

/* vim: set tabstop=8 noexpandtab : */
