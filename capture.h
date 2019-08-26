/*
 * Copyright (c) 2019 Luc Verhaegen <libv@skynet.be>
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

#ifndef _HAVE_CAPTURE_H_
#define _HAVE_CAPTURE_H_ 1

struct capture_buffer {
	int index;

	int width;
	int height;

	/* we assume that all sizes and strides are the same for all planes. */
	size_t stride;
	size_t plane_size;

	uint32_t v4l2_fourcc;
	uint32_t drm_format;

	struct plane {
		off_t offset;
		void *map;
		int export_fd;
	} planes[3];
};

int capture_init(unsigned long count);
void capture_destroy(void);

#endif /* _HAVE_CAPTURE_H_ */
