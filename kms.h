/*
 * Copyright (c) 2011-2013, 2019 Luc Verhaegen <libv@skynet.be>
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

#ifndef _HAVE_KMS_H_
#define _HAVE_KMS_H_ 1

struct capture_buffer;

int kms_buffer_import(struct capture_buffer *buffer);

void kms_projector_capture_display(struct capture_buffer *buffer);
void kms_status_capture_display(struct capture_buffer *buffer);

int kms_init(int width, int height, int bpp, uint32_t format,
	     unsigned long count);

#endif /* _HAVE_KMS_H_ */
