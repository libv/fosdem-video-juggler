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
struct _drmModeAtomicReq;

extern int kms_fd;

struct kms_buffer {
	int width;
	int height;
	uint32_t format;

	uint32_t handle; /* dumb buffer handle */

	int pitch;
	size_t size;

	uint64_t map_offset;
	void *map;

	uint32_t fb_id;
};

struct kms_plane {
	uint32_t plane_id;
	bool active;

	/* property ids -- how clunky is this? */
	uint32_t property_crtc_id;
	uint32_t property_fb_id;
	uint32_t property_crtc_x;
	uint32_t property_crtc_y;
	uint32_t property_crtc_w;
	uint32_t property_crtc_h;
	uint32_t property_src_x;
	uint32_t property_src_y;
	uint32_t property_src_w;
	uint32_t property_src_h;
	uint32_t property_src_formats;
	uint32_t property_alpha;
	uint32_t property_zpos;
	uint32_t property_type;
	uint32_t property_in_fence_id;
};

int kms_connector_id_get(uint32_t type, uint32_t *id_ret);
const char *kms_connector_string(uint32_t connector);
int kms_connection_check(uint32_t connector_id, bool *connected,
			 uint32_t *encoder_id);
int kms_crtc_id_get(uint32_t encoder_id, uint32_t *crtc_id, bool *ok,
		    int *width, int *height);
int kms_crtc_modeline_print(uint32_t crtc_id);
int kms_crtc_index_get(uint32_t id);

struct kms_plane *kms_plane_create(uint32_t plane_id);
void kms_plane_disable(struct kms_plane *kms_plane,
		       struct _drmModeAtomicReq *request);

struct kms_buffer *kms_png_read(const char *filename);

int kms_buffer_import(struct capture_buffer *buffer);

int kms_init(void);

#endif /* _HAVE_KMS_H_ */
