/*    Copyright (C) 2013 GP Orcullo
 *
 *    This file is part of PiCnc.
 *
 *    This program is free software; you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation; either version 3 of the License, or
 *    (at your option) any later version.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with this program; if not, write to the Free Software
 *    Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 */

#ifndef __STEPGEN_H__
#define __STEPGEN_H__

#define MAXGEN	4

#define disable_int()								\
	do {									\
		asm volatile("di");						\
		asm volatile("ehb");						\
	} while (0)

#define enable_int()								\
	do {									\
		asm volatile("ei");						\
	} while (0)

typedef struct {
	int32_t velocity[MAXGEN];
} stepgen_input_struct;

void stepgen(void);
void stepgen_reset(void);
int stepgen_get_position(void *buf);
void stepgen_update_input(const void *buf);

#endif				/* __STEPGEN_H__ */
