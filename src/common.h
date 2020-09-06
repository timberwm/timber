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

#include <sys/types.h>
#include <xkbcommon/xkbcommon.h>

#define TMBR_UNUSED __attribute__((unused))

#define ARRAY_SIZE(a) (sizeof(a) / sizeof(*a))

#define TMBR_PKT_MESSAGELEN 1024

typedef enum {
	TMBR_DIR_NORTH,
	TMBR_DIR_SOUTH,
	TMBR_DIR_EAST,
	TMBR_DIR_WEST,
	TMBR_DIR_LAST
} tmbr_dir_t;

typedef enum {
	TMBR_SELECT_PREV,
	TMBR_SELECT_NEXT,
	TMBR_SELECT_NEAREST,
	TMBR_SELECT_LAST
} tmbr_select_t;

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
	TMBR_COMMAND_DESKTOP_SWAP,
	TMBR_COMMAND_SCREEN_FOCUS,
	TMBR_COMMAND_SCREEN_SCALE,
	TMBR_COMMAND_TREE_ROTATE,
	TMBR_COMMAND_STATE_SUBSCRIBE,
	TMBR_COMMAND_STATE_QUERY,
	TMBR_COMMAND_STATE_STOP,
	TMBR_COMMAND_BINDING_ADD,
	TMBR_COMMAND_LAST
} tmbr_command_type_t;

typedef struct {
	tmbr_command_type_t type;
	tmbr_select_t sel;
	tmbr_dir_t dir;
	int i;
	struct { uint32_t modifiers; xkb_keysym_t keycode; } key;
	char command[128];
	char screen[16];
} tmbr_command_t;

typedef enum {
	TMBR_PKT_COMMAND,
	TMBR_PKT_ERROR,
	TMBR_PKT_DATA,
	TMBR_PKT_LAST
} tmbr_pkt_type_t;

typedef struct {
	tmbr_pkt_type_t type;
	union {
		tmbr_command_t command;
		int error;
		char data[TMBR_PKT_MESSAGELEN];
	} u;
} tmbr_pkt_t;

void __attribute__((noreturn, format(printf, 1, 2))) die(const char *fmt, ...);

void *tmbr_alloc(size_t bytes, const char *msg);

int tmbr_ctrl_connect(char create);
int tmbr_ctrl_read(int fd, tmbr_pkt_t *out);
int tmbr_ctrl_write(int fd, tmbr_pkt_t *pkt);
int __attribute__((format(printf, 2, 3))) tmbr_ctrl_write_data(int fd, const char *fmt, ...);
