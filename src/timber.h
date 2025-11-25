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

#include <stdint.h>
#include <sys/types.h>

#define TMBR_UNUSED __attribute__((unused))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof(*a))

struct tmbr_config {
	unsigned border_width;
	uint32_t border_color_active;
	uint32_t border_color_inactive;
	uint32_t gap;
};

void __attribute__((noreturn, format(printf, 1, 2))) die(const char *fmt, ...);
void *tmbr_alloc(size_t bytes, const char *msg);
int tmbr_client(int argc, char *argv[]);
int tmbr_wm(void);
