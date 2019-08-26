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

#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/mman.h>
#include <time.h>
#include <inttypes.h>

#include <pthread.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>

#include <png.h>

#include "juggler.h"
#include "kms.h"
#include "capture.h"

/* so that our capture side can use this separately. */
static int kms_fd = -1;

static unsigned long kms_frame_count;

pthread_t kms_projector_thread[1];
pthread_t kms_status_thread[1];

struct buffer {
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

struct kms_status {
	bool connected;
	bool mode_ok;

	uint32_t connector_id;
	uint32_t encoder_id;
	uint32_t crtc_id;
	int crtc_width;
	int crtc_height;
	int crtc_index;

	struct kms_plane *capture_scaling;
	struct kms_plane *capture_yuv;

	struct kms_plane *text;
	struct buffer *text_buffer;

	struct kms_plane *logo;
	struct buffer *logo_buffer;

	/*
	 * it could be that the primary plane is not used by us, and
	 * should be disabled
	 */
	struct kms_plane *plane_disable;

	pthread_mutex_t capture_buffer_mutex[1];
	/*
	 * This is the buffer that is currently being shown. It will be
	 * released as soon as the next buffer is shown using atomic modeset
	 * commit.
	 */
	struct capture_buffer *capture_buffer_current;
	/*
	 * This is the buffer that our blocking atomic modeset is about to
	 * show.
	 */
	struct capture_buffer *capture_buffer_next;
	/*
	 * This is the upcoming buffer that was last queued by capture.
	 */
	struct capture_buffer *capture_buffer_new;
};
static struct kms_status *kms_status;

struct kms_projector {
	bool connected;
	bool mode_ok;

	uint32_t connector_id;
	uint32_t encoder_id;
	uint32_t crtc_id;
	int crtc_width;
	int crtc_height;
	int crtc_index;

	struct kms_plane *capture_scaling;
	struct kms_plane *capture_yuv;

	/*
	 * it could be that the primary plane is not used by us, and
	 * should be disabled
	 */
	struct kms_plane *plane_disable;

	pthread_mutex_t capture_buffer_mutex[1];
	/*
	 * This is the buffer that is currently being shown. It will be
	 * released as soon as the next buffer is shown using atomic modeset
	 * commit.
	 */
	struct capture_buffer *capture_buffer_current;
	/*
	 * This is the buffer that our blocking atomic modeset is about to
	 * show.
	 */
	struct capture_buffer *capture_buffer_next;
	/*
	 * This is the upcoming buffer that was last queued by capture.
	 */
	struct capture_buffer *capture_buffer_new;
};
static struct kms_projector *kms_projector;

static int
kms_fd_init(const char *driver_name)
{
	int ret;

	kms_fd = drmOpen(driver_name, NULL);
	if (kms_fd == -1) {
		fprintf(stderr, "Error: Failed to open KMS driver %s: %s\n",
			driver_name, strerror(errno));
		return errno;
	}

	ret = drmSetClientCap(kms_fd, DRM_CLIENT_CAP_ATOMIC, 1);
	if (ret < 0) {
		fprintf(stderr, "Error: Unable to set DRM_CLIENT_CAP_ATOMIC:"
			" %s\n", strerror(errno));
		return ret;
	}

	ret = drmSetClientCap(kms_fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
	if (ret < 0) {
		fprintf(stderr, "Error: Unable to set "
			"DRM_CLIENT_CAP_UNIVERSAL_PLANES: %s\n",
			strerror(errno));
		return ret;
	}

	kms_fd = kms_fd;

	return 0;
}

static __maybe_unused char *
kms_encoder_string(uint32_t encoder)
{
	struct {
		uint32_t encoder;
		char *name;
	} table[] = {
		{DRM_MODE_ENCODER_NONE, "None"},
		{DRM_MODE_ENCODER_DAC, "DAC"},
		{DRM_MODE_ENCODER_TMDS, "TMDS"},
		{DRM_MODE_ENCODER_LVDS, "LVDS"},
		{DRM_MODE_ENCODER_TVDAC, "TVDAC"},
		{DRM_MODE_ENCODER_VIRTUAL, "VIRTUAL"},
		{DRM_MODE_ENCODER_DSI, "DSI"},
		{DRM_MODE_ENCODER_DPMST, "DPMST"},
		{DRM_MODE_ENCODER_DPI, "DPI"},
	};
	int i;

	for (i = 0; table[i].name; i++)
		if (table[i].encoder == encoder)
			return table[i].name;

	return table[0].name;
}

static __maybe_unused char *
kms_connector_string(uint32_t connector)
{
	struct {
		uint32_t connector;
		char *name;
	} table[] = {
		{DRM_MODE_CONNECTOR_Unknown, "Unknown"},
		{DRM_MODE_CONNECTOR_VGA, "VGA"},
		{DRM_MODE_CONNECTOR_DVII, "DVI-I"},
		{DRM_MODE_CONNECTOR_DVID, "DVI-D"},
		{DRM_MODE_CONNECTOR_DVIA, "DVI-A"},
		{DRM_MODE_CONNECTOR_Composite, "Composite"},
		{DRM_MODE_CONNECTOR_SVIDEO, "S-Video"},
		{DRM_MODE_CONNECTOR_LVDS, "LVDS"},
		{DRM_MODE_CONNECTOR_Component, "Component"},
		{DRM_MODE_CONNECTOR_9PinDIN, "DIN 9pin"},
		{DRM_MODE_CONNECTOR_DisplayPort, "DisplayPort"},
		{DRM_MODE_CONNECTOR_HDMIA, "HDMI A"},
		{DRM_MODE_CONNECTOR_HDMIB, "HDMI B"},
		{DRM_MODE_CONNECTOR_TV, "TV"},
		{DRM_MODE_CONNECTOR_eDP, "eDP"},
		{DRM_MODE_CONNECTOR_VIRTUAL, "Virtual"},
		{DRM_MODE_CONNECTOR_DSI, "DSI"},
		{DRM_MODE_CONNECTOR_DPI, "DPI"},
	};
	int i;

	for (i = 0; table[i].name; i++)
		if (table[i].connector == connector)
			return table[i].name;

	return table[0].name;
}

static __maybe_unused char *
kms_connection_string(drmModeConnection connection)
{
	struct {
		drmModeConnection connection;
		char *name;
	} table[] = {
		{DRM_MODE_CONNECTED, "connected"},
		{DRM_MODE_DISCONNECTED, "disconnected"},
		{DRM_MODE_UNKNOWNCONNECTION, "connection unknown"},
		{0, NULL}
	};
	int i;

	for (i = 0; table[i].name; i++)
		if (table[i].connection == connection)
			return table[i].name;

	return "connection unknown";
}

static int
kms_connector_id_get(uint32_t type, uint32_t *id_ret)
{
	drmModeRes *resources;
	drmModeConnector *connector = NULL;
	uint32_t connector_id = 0;
	int i, ret;

	resources = drmModeGetResources(kms_fd);
	if (!resources) {
		fprintf(stderr, "%s: Failed to get KMS resources: %s\n",
			__func__, strerror(errno));
		return -EINVAL;
	}

	/* First, scan through our connectors. */
	for (i = 0; i < resources->count_connectors; i++) {
		connector_id = resources->connectors[i];

		connector = drmModeGetConnector(kms_fd, connector_id);
		if (!connector) {
			fprintf(stderr,
				"%s: failed to get Connector %u: %s\n",
				__func__, connector_id, strerror(errno));
			ret = -errno;
			goto error;
		}

		if (connector->connector_type == type)
			break;

		drmModeFreeConnector(connector);
	}

	if (i == resources->count_connectors) {
		fprintf(stderr, "%s: no connector found for %s.\n",
			__func__, kms_connector_string(type));
		ret = -ENODEV;
		goto error;
	}

	*id_ret = connector_id;
	drmModeFreeConnector(connector);
	ret = 0;

 error:
	drmModeFreeResources(resources);
	return ret;
}

/*
 * KMS planes come with a bitmask, flagging which crtcs they can be
 * connected to. But our handles to crtcs are ids, not an index. So
 * we need to harvest the order of the crtcs from the main kms
 * resources structure. WTF?
 */
#define CRTC_INDEX_COUNT_MAX 2
static uint32_t kms_crtc_index[CRTC_INDEX_COUNT_MAX];
static int kms_crtc_index_count;

static int
kms_crtc_indices_get(void)
{
	drmModeRes *resources;
	int i;

	resources = drmModeGetResources(kms_fd);
	if (!resources) {
		fprintf(stderr, "%s: Failed to get KMS resources: %s\n",
			__func__, strerror(errno));
		return -EINVAL;
	}

	if (resources->count_crtcs > CRTC_INDEX_COUNT_MAX)
		kms_crtc_index_count = CRTC_INDEX_COUNT_MAX;
	else
		kms_crtc_index_count = resources->count_crtcs;

	for (i = 0; i < kms_crtc_index_count; i++)
		kms_crtc_index[i] = resources->crtcs[i];

	drmModeFreeResources(resources);
	return 0;
}

static int
kms_crtc_index_get(uint32_t id)
{
	int i;

	for (i = 0; i < kms_crtc_index_count; i++)
		if (kms_crtc_index[i] == id)
			return i;

	fprintf(stderr, "%s: failed to find crtc %u\n", __func__, id);
	return -EINVAL;
}

/*
 * DRM/KMS clunk galore.
 */
static int
kms_connection_check(uint32_t connector_id, bool *connected,
		     uint32_t *encoder_id)
{
	drmModeConnector *connector = NULL;

	/* Check whether our connector is connected. */
	connector = drmModeGetConnector(kms_fd, connector_id);
	if (!connector) {
		fprintf(stderr, "%s: failed to get Connector %u: %s\n",
			__func__, connector_id, strerror(errno));
		return -errno;
	}

	if (connector->connection != DRM_MODE_CONNECTED) {
		*connected = false;
	} else {
		*connected = true;
		*encoder_id = connector->encoder_id;
	}

	drmModeFreeConnector(connector);
	return 0;
}

static int
kms_crtc_id_get(uint32_t encoder_id, uint32_t *crtc_id, bool *ok,
		int *width, int *height)
{
	drmModeEncoder *encoder;
	drmModeCrtc *crtc;

	encoder = drmModeGetEncoder(kms_fd, encoder_id);
	if (!encoder) {
		fprintf(stderr, "%s: failed to get Encoder %u: %s\n",
			__func__, encoder_id, strerror(errno));
		return -errno;
	}

	*crtc_id = encoder->crtc_id;
	drmModeFreeEncoder(encoder);

	crtc = drmModeGetCrtc(kms_fd, *crtc_id);
	if (!crtc) {
		fprintf(stderr, "%s: failed to get CRTC %u: %s\n",
			__func__, *crtc_id, strerror(errno));
		return -errno;
	}

	if (!crtc->mode_valid) {
		fprintf(stderr, "%s: CRTC %u does not have a valid mode\n",
			__func__, *crtc_id);

		*ok = false;
		drmModeFreeCrtc(crtc);

		return -EINVAL;
	}

	*ok = true;
	*width = crtc->width;
	*height = crtc->height;

	drmModeFreeCrtc(crtc);

	return 0;
}

static struct kms_plane *
kms_plane_create(uint32_t plane_id)
{
	struct kms_plane *plane;
	drmModeObjectProperties *properties;
	int i;

	properties = drmModeObjectGetProperties(kms_fd, plane_id,
						DRM_MODE_OBJECT_PLANE);
	if (!properties) {
		/* yes, if there are no properties, this returns EINVAL */
		if (errno != EINVAL) {
			fprintf(stderr,
				"%s(0x%02X): Failed to get properties: %s\n",
				__func__, plane_id, strerror(errno));
			return NULL;
		}

		return NULL;
	}


	plane = calloc(1, sizeof(struct kms_plane));

	plane->plane_id = plane_id;

	for (i = 0; i < (int) properties->count_props; i++) {
		drmModePropertyRes *property;

		property = drmModeGetProperty(kms_fd,
					      properties->props[i]);
		if (!property) {
			fprintf(stderr, "Failed to get object %u "
				"property %u: %s\n", plane_id,
				properties->props[i], strerror(errno));
			continue;
		}

		if (!strcmp(property->name, "CRTC_ID"))
			plane->property_crtc_id = property->prop_id;
		else if (!strcmp(property->name, "FB_ID"))
			plane->property_fb_id = property->prop_id;
		else if (!strcmp(property->name, "CRTC_X"))
			plane->property_crtc_x = property->prop_id;
		else if (!strcmp(property->name, "CRTC_Y"))
			plane->property_crtc_y = property->prop_id;
		else if (!strcmp(property->name, "CRTC_W"))
			plane->property_crtc_w = property->prop_id;
		else if (!strcmp(property->name, "CRTC_H"))
			plane->property_crtc_h = property->prop_id;
		else if (!strcmp(property->name, "SRC_X"))
			plane->property_src_x = property->prop_id;
		else if (!strcmp(property->name, "SRC_Y"))
			plane->property_src_y = property->prop_id;
		else if (!strcmp(property->name, "SRC_W"))
			plane->property_src_w = property->prop_id;
		else if (!strcmp(property->name, "SRC_H"))
			plane->property_src_h = property->prop_id;
		else if (!strcmp(property->name, "IN_FORMATS"))
			plane->property_src_formats = property->prop_id;
		else if (!strcmp(property->name, "alpha"))
			plane->property_alpha = property->prop_id;
		else if (!strcmp(property->name, "zpos"))
			plane->property_zpos = property->prop_id;
		else if (!strcmp(property->name, "type"))
			plane->property_type = property->prop_id;
		else if (!strcmp(property->name, "IN_FENCE_FD"))
			plane->property_in_fence_id = property->prop_id;
		else
			printf("Unhandled property: %s\n", property->name);

		drmModeFreeProperty(property);
	}

	drmModeFreeObjectProperties(properties);

	return plane;
}

/*
 * Get all the desired planes in one go.
 */
static int
kms_status_planes_get(struct kms_status *status)
{
	drmModePlaneRes *resources_plane = NULL;
	int ret = 0, i;

	/* Get plane resources so we can start sifting through the planes */
	resources_plane = drmModeGetPlaneResources(kms_fd);
	if (!resources_plane) {
		fprintf(stderr, "%s: Failed to get KMS plane resources\n",
			__func__);
		ret = 0;
		goto error;
	}

	/* now cycle through the planes to find one for our crtc */
	for (i = 0; i < (int) resources_plane->count_planes; i++) {
		drmModePlane *plane;
		uint32_t plane_id = resources_plane->planes[i];
		bool frontend = false, yuv = false, layer = false;
		bool used = false;
		int j;

		plane = drmModeGetPlane(kms_fd, plane_id);
		if (!plane) {
			fprintf(stderr, "%s: failed to get Plane %u: %s\n",
				__func__, plane_id, strerror(errno));
			ret = 0;
			goto error;
		}

		if (!(plane->possible_crtcs & (1 << status->crtc_index)))
			goto plane_next;

		for (j = 0; j < (int) plane->count_formats; j++) {
			switch (plane->formats[j]) {
			/* only supported by frontend */
			case DRM_FORMAT_NV12:
				frontend = true;
				break;
			/* supported by the frontend and yuv layers */
			case DRM_FORMAT_R8_G8_B8:
				yuv = true;
				break;
			/* not supported by the sprites */
			case DRM_FORMAT_RGB565:
				layer = true;
				break;
			default:
				break;
			}
		}

		if (frontend) {
			status->capture_scaling =
				kms_plane_create(plane->plane_id);
			if (!status->capture_scaling) {
				ret = -1;
				goto plane_error;
			}
			used = true;
		} else if (yuv) {
			status->capture_yuv =
				kms_plane_create(plane->plane_id);
			if (!status->capture_yuv) {
				ret = -1;
				goto plane_error;
			}
			used = true;
		} else if (!layer) {
			if (!status->text) {
				status->text =
					kms_plane_create(plane->plane_id);
				if (!status->text) {
					ret = -1;
					goto plane_error;
				}
				used = true;
			} else if (!status->logo) {
				status->logo =
					kms_plane_create(plane->plane_id);
				if (!status->logo) {
					ret = -1;
					goto plane_error;
				}
				used = true;
			}
		}

		if (plane->fb_id && !used) {
			if (!status->plane_disable) {
				status->plane_disable =
					kms_plane_create(plane->plane_id);
				/* if this fails, continue */
			} else
				fprintf(stderr, "%s: multiple planes need to "
					"be disabled (%d, %d)!\n", __func__,
					status->plane_disable->plane_id,
					plane->plane_id);
		}

	plane_next:
		drmModeFreePlane(plane);
		continue;
	plane_error:
		drmModeFreePlane(plane);
		break;
	}

	if (status->plane_disable)
		status->plane_disable->active = true;

 error:
	drmModeFreePlaneResources(resources_plane);
	return ret;
}

/*
 * Get all the desired planes in one go.
 */
static int
kms_projector_planes_get(struct kms_projector *projector)
{
	drmModePlaneRes *resources_plane = NULL;
	int ret = 0, i;

	/* Get plane resources so we can start sifting through the planes */
	resources_plane = drmModeGetPlaneResources(kms_fd);
	if (!resources_plane) {
		fprintf(stderr, "%s: Failed to get KMS plane resources\n",
			__func__);
		ret = 0;
		goto error;
	}

	/* now cycle through the planes to find one for our crtc */
	for (i = 0; i < (int) resources_plane->count_planes; i++) {
		drmModePlane *plane;
		uint32_t plane_id = resources_plane->planes[i];
		bool frontend = false, yuv = false, used = false;
		int j;

		plane = drmModeGetPlane(kms_fd, plane_id);
		if (!plane) {
			fprintf(stderr, "%s: failed to get Plane %u: %s\n",
				__func__, plane_id, strerror(errno));
			ret = 0;
			goto error;
		}

		if (!(plane->possible_crtcs & (1 << projector->crtc_index)))
			goto plane_next;

		for (j = 0; j < (int) plane->count_formats; j++) {
			switch (plane->formats[j]) {
			/* only supported by frontend */
			case DRM_FORMAT_NV12:
				frontend = true;
				break;
			/* supported by the frontend and yuv layers */
			case DRM_FORMAT_R8_G8_B8:
				yuv = true;
				break;
			default:
				break;
			}
		}

		if (frontend) {
			projector->capture_scaling =
				kms_plane_create(plane->plane_id);
			if (!projector->capture_scaling) {
				ret = -1;
				goto plane_error;
			}
			used = true;
		} else if (yuv) {
			projector->capture_yuv =
				kms_plane_create(plane->plane_id);
			if (!projector->capture_yuv) {
				ret = -1;
				goto plane_error;
			}
			used = true;
		}

		if (plane->fb_id && !used) {
			if (!projector->plane_disable) {
				projector->plane_disable =
					kms_plane_create(plane->plane_id);
				/* if this fails, continue */
			} else
				fprintf(stderr, "%s: multiple planes need to "
					"be disabled (%d, %d)!\n", __func__,
					projector->plane_disable->plane_id,
					plane->plane_id);
		}

	plane_next:
		drmModeFreePlane(plane);
		continue;
	plane_error:
		drmModeFreePlane(plane);
		break;
	}

	if (projector->plane_disable)
		projector->plane_disable->active = true;

 error:
	drmModeFreePlaneResources(resources_plane);
	return ret;
}

static int
kms_buffer_argb8888_get(struct buffer *buffer,
			int width, int height, uint32_t format)
{
	struct drm_mode_create_dumb buffer_create = { 0 };
	struct drm_mode_map_dumb buffer_map = { 0 };
	uint32_t handles[4] = { 0 };
	uint32_t pitches[4] = { 0 };
	uint32_t offsets[4] = { 0 };
	int ret;

	buffer->width = width;
	buffer->height = height;
	buffer->format = format;

	buffer_create.width = width;
	buffer_create.height = height;
	buffer_create.bpp = 32;
	ret = drmIoctl(kms_fd, DRM_IOCTL_MODE_CREATE_DUMB, &buffer_create);
	if (ret) {
		fprintf(stderr, "%s: failed to create buffer: %s\n",
			__func__, strerror(errno));
		return ret;
	}

	buffer->handle = buffer_create.handle;
	buffer->size = buffer_create.size;
	buffer->pitch = buffer_create.pitch;
	printf("%s(): Created buffer %dx%d@%dbpp: %02u (%tdbytes)\n",
	       __func__, buffer->width, buffer->height,
	       buffer_create.bpp, buffer->handle, buffer->size);

	buffer_map.handle = buffer->handle;
	ret = drmIoctl(kms_fd, DRM_IOCTL_MODE_MAP_DUMB, &buffer_map);
	if (ret) {
		fprintf(stderr, "%s: failed to map buffer: %s\n",
			__func__, strerror(errno));
		return -errno;
	}

	buffer->map_offset = buffer_map.offset;
	printf("%s(): Mapped buffer %02u at offset 0x%jX\n",
	       __func__, buffer->handle, buffer->map_offset);

	buffer->map = mmap(0, buffer->size, PROT_READ | PROT_WRITE,
			  MAP_SHARED, kms_fd, buffer->map_offset);
	if (buffer->map == MAP_FAILED) {
		fprintf(stderr, "%s: failed to mmap buffer: %s\n",
			__func__, strerror(errno));
		return -errno;
	}

	printf("%s(): MMapped buffer %02u to %p\n",
	       __func__, buffer->handle, buffer->map);

	handles[0] = buffer->handle;
	pitches[0] = buffer->pitch;

	ret = drmModeAddFB2(kms_fd, buffer->width, buffer->height,
			    buffer->format, handles, pitches, offsets,
			    &buffer->fb_id, 0);
	if (ret) {
		fprintf(stderr, "%s: failed to create fb: %s\n",
			__func__, strerror(errno));
		return -errno;
	}

	printf("Created FB %02u.\n", buffer->fb_id);

	return 0;
}

/*
 *
 */
int
kms_buffer_import(struct capture_buffer *buffer)
{
	uint32_t handles[4] = { 0 };
	uint32_t pitches[4] = { 0 };
	uint32_t offsets[4] = { 0 };
	int ret, i;

	for (i = 0; i < 3; i++) {
		struct drm_prime_handle prime[1] = {{
				.fd = buffer->planes[i].export_fd,
			}};

		ret = drmIoctl(kms_fd, DRM_IOCTL_PRIME_FD_TO_HANDLE, prime);
		if (ret) {
			fprintf(stderr, "%s: drmIoctl(PRIME_FD_TO_HANDLE, %d) "
				"failed: %s\n", __func__,
				buffer->planes[i].export_fd, strerror(errno));
			return ret;
		}

		buffer->planes[i].prime_handle = prime->handle;
		handles[i] = prime->handle;
		pitches[i] = buffer->pitch;
	}

	printf("%s(%d): prime handles: %02X, %02X, %02X\n",
	       __func__, buffer->index, buffer->planes[0].prime_handle,
	       buffer->planes[1].prime_handle, buffer->planes[2].prime_handle);

	ret = drmModeAddFB2(kms_fd, buffer->width, buffer->height,
			    buffer->drm_format, handles, pitches, offsets,
			    &buffer->kms_fb_id, 0);
	if (ret) {
		fprintf(stderr, "%s(%d): failed to create fb: %s\n",
			__func__, buffer->index, strerror(errno));
		return -errno;
	}

	printf("%s(%d): FB %02u.\n",
	       __func__, buffer->index, buffer->kms_fb_id);

	return 0;
}

/*
 *
 */
static struct buffer *
kms_png_read(const char *filename)
{
	struct buffer *buffer;
	png_image image[1] = {{
		.version = PNG_IMAGE_VERSION,
	}};
	int ret;

	ret = png_image_begin_read_from_file(image, filename);
	if (ret != 1) {
		fprintf(stderr, "%s(): read_from_file() failed: %s\n",
			__func__, image->message);
		return NULL;
	}

	image->format = PNG_FORMAT_BGRA;

	printf("Reading from %s: %dx%d (%dbytes)\n", filename,
	       image->width, image->height, PNG_IMAGE_SIZE(*image));

	buffer = calloc(1, sizeof(struct buffer));

	ret = kms_buffer_argb8888_get(buffer, image->width, image->height,
				      DRM_FORMAT_ARGB8888);
	if (ret) {
		fprintf(stderr, "%s(): failed to create buffer for %s\n",
			__func__, filename);
		free(buffer);
		png_image_free(image);
		return NULL;
	}

	ret = png_image_finish_read(image, NULL, buffer->map, 0, NULL);
	if (ret != 1) {
		fprintf(stderr, "%s(): failed to read png for %s: %s\n",
			__func__, filename, image->message);
		/* we need to cleanly destroy buffers. */
		free(buffer);
		png_image_free(image);
		return NULL;
	}

	png_image_free(image);
	return buffer;
}

/*
 * Show input buffer on projector, scaled, with borders.
 */
static void
kms_projector_capture_set(struct kms_projector *projector,
			  struct capture_buffer *buffer,
			  drmModeAtomicReqPtr request)
{
	struct kms_plane *plane = projector->capture_scaling;

	if (!buffer)
		return;

	if (!plane->active) {
		int x, y, w, h;

		drmModeAtomicAddProperty(request, plane->plane_id,
					 plane->property_crtc_id,
					 projector->crtc_id);

		/* Scale, with borders, and center */
		if ((buffer->width == projector->crtc_width) &&
		    (buffer->height == projector->crtc_height)) {
			x = 0;
			y = 0;
			w = projector->crtc_width;
			h = projector->crtc_height;
		} else {
			/* first, try to fit horizontally. */
			w = projector->crtc_width;
			h = buffer->height * projector->crtc_width /
				buffer->width;

			/* if height does not fit, inverse the logic */
			if (h > projector->crtc_height) {
				h = projector->crtc_height;
				w = buffer->width * projector->crtc_height /
					buffer->height;
			}

			/* center */
			x = (projector->crtc_width - w) / 2;
			y = (projector->crtc_height -h) / 2;
		}

		drmModeAtomicAddProperty(request, plane->plane_id,
					 plane->property_crtc_x, x);
		drmModeAtomicAddProperty(request, plane->plane_id,
					 plane->property_crtc_y, y);
		drmModeAtomicAddProperty(request, plane->plane_id,
					 plane->property_crtc_w, w);
		drmModeAtomicAddProperty(request, plane->plane_id,
					 plane->property_crtc_h, h);

		/* read in full size image */
		drmModeAtomicAddProperty(request, plane->plane_id,
					 plane->property_src_x, 0);
		drmModeAtomicAddProperty(request, plane->plane_id,
					 plane->property_src_y, 0);
		drmModeAtomicAddProperty(request, plane->plane_id,
					 plane->property_src_w,
					 buffer->width << 16);
		drmModeAtomicAddProperty(request, plane->plane_id,
					 plane->property_src_h,
					 buffer->height << 16);
		plane->active = true;
	}

	/* actual flip. */
	drmModeAtomicAddProperty(request, plane->plane_id,
				 plane->property_fb_id,
				 buffer->kms_fb_id);
}

/*
 * HDMI or VGA used for the projector.
 */
static struct kms_projector *
kms_projector_init(void)
{
	struct kms_projector *projector;
	int ret;

	projector = calloc(1, sizeof(struct kms_projector));
	if (!projector)
		return NULL;

	pthread_mutex_init(projector->capture_buffer_mutex, NULL);

	ret = kms_connector_id_get(DRM_MODE_CONNECTOR_HDMIA,
				   &projector->connector_id);
	if (ret)
		return NULL;

	ret = kms_connection_check(projector->connector_id,
				   &projector->connected,
				   &projector->encoder_id);
	if (ret)
		return NULL;

	ret = kms_crtc_id_get(projector->encoder_id,
			      &projector->crtc_id, &projector->mode_ok,
			      &projector->crtc_width, &projector->crtc_height);
	if (ret)
		return NULL;

	ret = kms_crtc_index_get(projector->crtc_id);
	if (ret < 0)
		return NULL;

	projector->crtc_index = ret;

	ret = kms_projector_planes_get(projector);
	if (ret)
		return NULL;

	return projector;
}

/*
 * Show input buffer on the status lcd, in the top right corner.
 */
static void
kms_status_capture_set(struct kms_status *status,
		       struct capture_buffer *buffer,
		       drmModeAtomicReqPtr request)
{
	struct kms_plane *plane = status->capture_scaling;

	if (!buffer)
		return;

	if (!plane->active) {
		int x, y, w, h;

		drmModeAtomicAddProperty(request, plane->plane_id,
					 plane->property_crtc_id,
					 status->crtc_id);

#if 0
		/* top right corner */
		x = status->crtc_width / 2;
		y = 0;
		w = status->crtc_width / 2;
		h = buffer->height * w / buffer->width;
#else
				/* Scale, with borders, and center */
		if ((buffer->width == status->crtc_width) &&
		    (buffer->height == status->crtc_height)) {
			x = 0;
			y = 0;
			w = status->crtc_width;
			h = status->crtc_height;
		} else {
			/* first, try to fit horizontally. */
			w = status->crtc_width;
			h = buffer->height * status->crtc_width /
				buffer->width;

			/* if height does not fit, inverse the logic */
			if (h > status->crtc_height) {
				h = status->crtc_height;
				w = buffer->width * status->crtc_height /
					buffer->height;
			}

			/* center */
			x = (status->crtc_width - w) / 2;
			y = (status->crtc_height -h) / 2;
		}
#endif

		printf("%s(): %4dx%4d -> %4dx%4d\n", __func__, x, y, w, h);

		drmModeAtomicAddProperty(request, plane->plane_id,
					 plane->property_crtc_x, x);
		drmModeAtomicAddProperty(request, plane->plane_id,
					 plane->property_crtc_y, y);
		drmModeAtomicAddProperty(request, plane->plane_id,
					 plane->property_crtc_w, w);
		drmModeAtomicAddProperty(request, plane->plane_id,
					 plane->property_crtc_h, h);

		/* read in full size image */
		drmModeAtomicAddProperty(request, plane->plane_id,
					 plane->property_src_x, 0);
		drmModeAtomicAddProperty(request, plane->plane_id,
					 plane->property_src_y, 0);
		drmModeAtomicAddProperty(request, plane->plane_id,
					 plane->property_src_w,
					 buffer->width << 16);
		drmModeAtomicAddProperty(request, plane->plane_id,
					 plane->property_src_h,
					 buffer->height << 16);
		drmModeAtomicAddProperty(request, plane->plane_id,
					 plane->property_alpha,
					 0x4000);

		plane->active = true;
	}

	/* actual flip. */
	drmModeAtomicAddProperty(request, plane->plane_id,
				 plane->property_fb_id,
				 buffer->kms_fb_id);
}

/*
 * Show status text on bottom of the status lcd.
 */
static void
kms_status_text_set(struct kms_status *status, drmModeAtomicReqPtr request)
{
	struct kms_plane *plane = status->text;
	struct buffer *buffer = status->text_buffer;

	if (!plane->active) {
		int x, y, w, h;

		drmModeAtomicAddProperty(request, plane->plane_id,
					 plane->property_crtc_id,
					 status->crtc_id);

		/* bottom, with a bit of space remaining */
		x = 8;
		y = status->crtc_height - 8 - buffer->height;
		w = buffer->width;
		h = buffer->height;

		drmModeAtomicAddProperty(request, plane->plane_id,
					 plane->property_crtc_x, x);
		drmModeAtomicAddProperty(request, plane->plane_id,
					 plane->property_crtc_y, y);
		drmModeAtomicAddProperty(request, plane->plane_id,
					 plane->property_crtc_w, w);
		drmModeAtomicAddProperty(request, plane->plane_id,
					 plane->property_crtc_h, h);

		/* read in full size image */
		drmModeAtomicAddProperty(request, plane->plane_id,
					 plane->property_src_x, 0);
		drmModeAtomicAddProperty(request, plane->plane_id,
					 plane->property_src_y, 0);
		drmModeAtomicAddProperty(request, plane->plane_id,
					 plane->property_src_w,
					 buffer->width << 16);
		drmModeAtomicAddProperty(request, plane->plane_id,
					 plane->property_src_h,
					 buffer->height << 16);

		plane->active = true;
	}

	/* actual flip. */
	drmModeAtomicAddProperty(request, plane->plane_id,
				 plane->property_fb_id,
				 buffer->fb_id);
}

/*
 * Show logo on the top right of status lcd.
 */
static void
kms_status_logo_set(struct kms_status *status, drmModeAtomicReqPtr request)
{
	struct kms_plane *plane = status->logo;
	struct buffer *buffer = status->logo_buffer;

	if (!plane->active) {
		int x, y, w, h;

		drmModeAtomicAddProperty(request, plane->plane_id,
					 plane->property_crtc_id,
					 status->crtc_id);

		/* top right, with a bit of space remaining */
		x = status->crtc_width - 8 - buffer->width;
		y = 8;
		w = buffer->width;
		h = buffer->height;

		drmModeAtomicAddProperty(request, plane->plane_id,
					 plane->property_crtc_x, x);
		drmModeAtomicAddProperty(request, plane->plane_id,
					 plane->property_crtc_y, y);
		drmModeAtomicAddProperty(request, plane->plane_id,
					 plane->property_crtc_w, w);
		drmModeAtomicAddProperty(request, plane->plane_id,
					 plane->property_crtc_h, h);

		/* read in full size image */
		drmModeAtomicAddProperty(request, plane->plane_id,
					 plane->property_src_x, 0);
		drmModeAtomicAddProperty(request, plane->plane_id,
					 plane->property_src_y, 0);
		drmModeAtomicAddProperty(request, plane->plane_id,
					 plane->property_src_w,
					 buffer->width << 16);
		drmModeAtomicAddProperty(request, plane->plane_id,
					 plane->property_src_h,
					 buffer->height << 16);

		drmModeAtomicAddProperty(request, plane->plane_id,
					 plane->property_zpos,
					 4);

		plane->active = true;
	}

	/* actual flip. */
	drmModeAtomicAddProperty(request, plane->plane_id,
				 plane->property_fb_id,
				 buffer->fb_id);
}

/*
 * Status LCD.
 */
static struct kms_status *
kms_status_init(void)
{
	struct kms_status *status;
	int ret;

	status = calloc(1, sizeof(struct kms_status));
	if (!status)
		return NULL;

	pthread_mutex_init(status->capture_buffer_mutex, NULL);

	ret = kms_connector_id_get(DRM_MODE_CONNECTOR_DPI,
				   &status->connector_id);
	if (ret)
		return NULL;

	ret = kms_connection_check(status->connector_id,
				   &status->connected, &status->encoder_id);
	if (ret)
		return NULL;

	ret = kms_crtc_id_get(status->encoder_id,
			      &status->crtc_id, &status->mode_ok,
			      &status->crtc_width, &status->crtc_height);
	if (ret)
		return NULL;

	ret = kms_crtc_index_get(status->crtc_id);
	if (ret < 0)
		return NULL;

	status->crtc_index = ret;

	ret = kms_status_planes_get(status);
	if (ret)
		return NULL;

	status->text_buffer = kms_png_read("status_text.png");
	if (!status->text_buffer)
		return NULL;

	status->logo_buffer = kms_png_read("fosdem_logo.png");
	if (!status->logo_buffer)
		return NULL;

	return status;
}

/*
 * Yes, you really need all this to disable a plane.
 */
static void
kms_plane_disable(struct kms_plane *kms_plane, drmModeAtomicReqPtr request)
{
	drmModeAtomicAddProperty(request, kms_plane->plane_id,
				 kms_plane->property_crtc_id, 0);

	drmModeAtomicAddProperty(request, kms_plane->plane_id,
				 kms_plane->property_crtc_x, 0);
	drmModeAtomicAddProperty(request, kms_plane->plane_id,
				 kms_plane->property_crtc_y, 0);
	drmModeAtomicAddProperty(request, kms_plane->plane_id,
				 kms_plane->property_crtc_w, 0);
	drmModeAtomicAddProperty(request, kms_plane->plane_id,
				 kms_plane->property_crtc_h, 0);

	drmModeAtomicAddProperty(request, kms_plane->plane_id,
				 kms_plane->property_src_x, 0);
	drmModeAtomicAddProperty(request, kms_plane->plane_id,
				 kms_plane->property_src_y, 0);
	drmModeAtomicAddProperty(request, kms_plane->plane_id,
				 kms_plane->property_src_w, 0);
	drmModeAtomicAddProperty(request, kms_plane->plane_id,
				 kms_plane->property_src_h, 0);

	drmModeAtomicAddProperty(request, kms_plane->plane_id,
				 kms_plane->property_fb_id, 0);

	kms_plane->active = false;
}

static int
kms_projector_frame_update(struct kms_projector *projector,
			   struct capture_buffer *buffer, int frame)
{
	drmModeAtomicReqPtr request;
	int ret;

	request = drmModeAtomicAlloc();

	kms_projector_capture_set(projector, buffer, request);

	if (projector->plane_disable && projector->plane_disable->active)
		kms_plane_disable(projector->plane_disable, request);

	ret = drmModeAtomicCommit(kms_fd, request,
				  DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);

	drmModeAtomicFree(request);

	if (ret) {
		fprintf(stderr, "%s: failed to show frame %d: %s\n",
			__func__, frame, strerror(errno));
		ret = -errno;
	}

	return ret;
}

static int
kms_status_frame_update(struct kms_status *status,
			struct capture_buffer *buffer, int frame)
{
	drmModeAtomicReqPtr request;
	int ret;

	request = drmModeAtomicAlloc();

	kms_status_capture_set(status, buffer, request);
	kms_status_text_set(status, request);
	kms_status_logo_set(status, request);

	if (status->plane_disable && status->plane_disable->active)
		kms_plane_disable(status->plane_disable, request);

	ret = drmModeAtomicCommit(kms_fd, request,
				  DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);

	drmModeAtomicFree(request);

	if (ret) {
		fprintf(stderr, "%s: failed to show frame %d: %s\n",
			__func__, frame, strerror(errno));
		ret = -errno;
	}

	return ret;
}

static void *
kms_projector_thread_handler(void *arg)
{
	struct kms_projector *projector = (struct kms_projector *) arg;
	int ret, i;

	for (i = 0; i < kms_frame_count; i++) {
		ret = kms_projector_frame_update(projector,
						 projector->capture_buffer_next,
						 i);
		if (ret)
			return NULL;
	}

	printf("%s: done!\n", __func__);

	return NULL;
}

void
kms_projector_capture_display(struct capture_buffer *buffer)
{
	struct kms_projector *projector = kms_projector;
	struct capture_buffer *old;

	pthread_mutex_lock(projector->capture_buffer_mutex);

	old = projector->capture_buffer_new;
	projector->capture_buffer_new = buffer;

	pthread_mutex_unlock(projector->capture_buffer_mutex);

	if (old)
		capture_buffer_display_release(old);
}

static void *
kms_status_thread_handler(void *arg)
{
	struct kms_status *status = (struct kms_status *) arg;
	int ret, i;

	for (i = 0; i < kms_frame_count; i++) {
		ret = kms_status_frame_update(status,
					      status->capture_buffer_next,
					      i);
		if (ret)
			return NULL;
	}

	printf("%s: done!\n", __func__);

	return NULL;
}

void
kms_status_capture_display(struct capture_buffer *buffer)
{
	struct kms_status *status = kms_status;
	struct capture_buffer *old;

	pthread_mutex_lock(status->capture_buffer_mutex);

	old = status->capture_buffer_new;

	status->capture_buffer_new = buffer;

	pthread_mutex_unlock(status->capture_buffer_mutex);

	if (old)
		capture_buffer_display_release(old);
}

int
kms_init(int width, int height, int bpp, uint32_t format, unsigned long count)
{
	int ret;

	kms_frame_count = count;

	ret = kms_fd_init("sun4i-drm");
	if (ret)
		return ret;

	ret = kms_crtc_indices_get();
	if (ret)
		return ret;

	kms_status = kms_status_init();
	if (!kms_status)
		return -1;

	ret = pthread_create(kms_status_thread, NULL,
			     kms_status_thread_handler,
			     (void *) kms_status);
	if (ret) {
		fprintf(stderr, "%s() status thread creation failed: %s\n",
			__func__, strerror(ret));
		return ret;
	}

	kms_projector = kms_projector_init();
	if (!kms_projector)
		return -1;

	ret = pthread_create(kms_projector_thread, NULL,
			     kms_projector_thread_handler,
			     (void *) kms_projector);
	if (ret) {
		fprintf(stderr, "%s() projector thread creation failed: %s\n",
			__func__, strerror(ret));
		return ret;
	}

	return 0;
}
