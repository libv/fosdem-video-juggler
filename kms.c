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

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>

#include "juggler.h"
#include "kms.h"

struct kms;

struct buffer {
	int width;
	int height;
	uint32_t format;

	struct buffer_plane {
		uint32_t handle; /* dumb buffer handle */

		int pitch;
		size_t size;

		uint64_t map_offset;
		void *map;
	} planes[3];

	uint32_t fb_id;
};

struct kms_plane {
	struct kms *kms;

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
};

struct kms_display {
	struct kms *kms;

	bool connected;
	bool mode_ok;

	uint32_t connector_id;
	uint32_t encoder_id;
	uint32_t crtc_id;
	int crtc_width;
	int crtc_height;
};

struct kms_status {
	struct kms *kms;

	struct kms_display display[1];

	struct kms_plane capture[1];

	struct kms_plane text[1];
	struct buffer text_buffer[1];

	struct kms_plane logo[1];
	struct buffer logo_buffer[1];
};

struct kms_projector {
	struct kms *kms;

	struct kms_display display[1];

	struct kms_plane capture[1];
};

struct kms {
	int kms_fd;

	/* buffer info */
	int width;
	int height;
	int bpp;
	uint32_t format;

	/* actual buffers */
	int buffer_count;
	struct buffer buffers[3][1];

	struct kms_status status[1];
	struct kms_projector projector[1];

	/*
	 * kms is soo clunky, here we track the index position of the
	 * actual crtc ids.
	 */
#define CRTC_INDEX_COUNT_MAX 2
	uint32_t crtc_index[CRTC_INDEX_COUNT_MAX];
	int crtc_index_count;
};

static int
kms_fd_init(struct kms *kms, const char *driver_name)
{
	int ret;

	kms->kms_fd = drmOpen(driver_name, NULL);
	if (kms->kms_fd == -1) {
		fprintf(stderr, "Error: Failed to open KMS driver %s: %s\n",
			driver_name, strerror(errno));
		return errno;
	}

	ret = drmSetClientCap(kms->kms_fd, DRM_CLIENT_CAP_ATOMIC, 1);
	if (ret < 0) {
		fprintf(stderr, "Error: Unable to set DRM_CLIENT_CAP_ATOMIC:"
			" %s\n", strerror(errno));
		return ret;
	}

	ret = drmSetClientCap(kms->kms_fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
	if (ret < 0) {
		fprintf(stderr, "Error: Unable to set "
			"DRM_CLIENT_CAP_UNIVERSAL_PLANES: %s\n",
			strerror(errno));
		return ret;
	}

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
kms_connector_id_get(struct kms_display *display, uint32_t type)
{
	struct kms *kms = display->kms;
	drmModeRes *resources;
	drmModeConnector *connector = NULL;
	uint32_t connector_id = 0;
	int i, ret;

	resources = drmModeGetResources(kms->kms_fd);
	if (!resources) {
		fprintf(stderr, "%s: Failed to get KMS resources: %s\n",
			__func__, strerror(errno));
		return -EINVAL;
	}

	/* First, scan through our connectors. */
        for (i = 0; i < resources->count_connectors; i++) {
		connector_id = resources->connectors[i];

		connector = drmModeGetConnector(kms->kms_fd, connector_id);
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

	display->connector_id = connector_id;
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
static int
kms_crtc_indices_get(struct kms *kms)
{
	drmModeRes *resources;
	int i;

	resources = drmModeGetResources(kms->kms_fd);
	if (!resources) {
		fprintf(stderr, "%s: Failed to get KMS resources: %s\n",
			__func__, strerror(errno));
		return -EINVAL;
	}

	if (resources->count_crtcs > CRTC_INDEX_COUNT_MAX)
		kms->crtc_index_count = CRTC_INDEX_COUNT_MAX;
	else
		kms->crtc_index_count = resources->count_crtcs;

	for (i = 0; i < kms->crtc_index_count; i++)
		kms->crtc_index[i] = resources->crtcs[i];

	drmModeFreeResources(resources);
	return 0;
}

static int
kms_crtc_index_get(struct kms *kms, uint32_t id)
{
	int i;

	for (i = 0; i < kms->crtc_index_count; i++)
		if (kms->crtc_index[i] == id)
			return i;

	fprintf(stderr, "%s: failed to find crtc %u\n", __func__, id);
	return -EINVAL;
}

/*
 * DRM/KMS clunk galore.
 */
static int
kms_connection_check(struct kms_display *display)
{
	struct kms *kms = display->kms;
	drmModeConnector *connector = NULL;

	/* Check whether our connector is connected. */
	connector = drmModeGetConnector(kms->kms_fd, display->connector_id);
	if (!connector) {
		fprintf(stderr, "%s: failed to get Connector %u: %s\n",
			__func__, display->connector_id, strerror(errno));
		return -errno;
	}

	if (connector->connection != DRM_MODE_CONNECTED) {
		display->connected = false;
	} else {
		display->connected = true;
		display->encoder_id = connector->encoder_id;
	}

	drmModeFreeConnector(connector);
	return 0;
}

static int
kms_crtc_id_get(struct kms_display *display)
{
	struct kms *kms = display->kms;
	drmModeEncoder *encoder;
	drmModeCrtc *crtc;

	encoder = drmModeGetEncoder(kms->kms_fd, display->encoder_id);
	if (!encoder) {
		fprintf(stderr, "%s: failed to get Encoder %u: %s\n",
			__func__, display->encoder_id, strerror(errno));
		return -errno;
	}

	display->crtc_id = encoder->crtc_id;
	drmModeFreeEncoder(encoder);

	crtc = drmModeGetCrtc(kms->kms_fd, display->crtc_id);
	if (!crtc) {
		fprintf(stderr, "%s: failed to get CRTC %u: %s\n",
			__func__, display->crtc_id, strerror(errno));
		return -errno;
	}

	if (!crtc->mode_valid) {
		fprintf(stderr, "%s: CRTC %u does not have a valid mode\n",
			__func__, display->crtc_id);

		display->mode_ok = false;
		drmModeFreeCrtc(crtc);

		return -EINVAL;
	}

	display->mode_ok = true;
	display->crtc_width = crtc->width;
	display->crtc_height = crtc->height;

	drmModeFreeCrtc(crtc);

	return 0;
}

/*
 * Todo, make sure that we use a plane only once.
 */
static int
kms_plane_id_get(struct kms_display *display, uint32_t format, int index)
{
	struct kms *kms = display->kms;
	drmModePlaneRes *resources_plane = NULL;
	drmModePlane *plane;
	uint32_t plane_id = 0;
	int i, j, ret, crtc_index, count = 0;

	crtc_index = kms_crtc_index_get(kms, display->crtc_id);

	/* Get plane resources so we can start sifting through the planes */
	resources_plane = drmModeGetPlaneResources(kms->kms_fd);
	if (!resources_plane) {
		fprintf(stderr, "%s: Failed to get KMS plane resources\n",
			__func__);
		ret = 0;
		goto error;
	}

	/* now cycle through the planes to find one for our crtc */
	for (i = 0; i < (int) resources_plane->count_planes; i++) {
		plane_id = resources_plane->planes[i];

		plane = drmModeGetPlane(kms->kms_fd, plane_id);
		if (!plane) {
			fprintf(stderr, "%s: failed to get Plane %u: %s\n",
				__func__, plane_id, strerror(errno));
			ret = 0;
			goto error;
		}

		if (!(plane->possible_crtcs & (1 << crtc_index)))
			goto plane_next;

		for (j = 0; j < (int) plane->count_formats; j++)
			if (plane->formats[j] == kms->format)
				break;

		if (j == (int) plane->count_formats)
			goto plane_next;

		if (count != index) {
			count++;
			goto plane_next;
		}

		drmModeFreePlane(plane);
		break;

	plane_next:
		drmModeFreePlane(plane);
		continue;
	}

	if (i == (int) resources_plane->count_planes) {
		fprintf(stderr, "%s: failed to get a Plane for our needs.\n",
			__func__);
		ret = 0;
		goto error;
	}

	ret = plane_id;

 error:
	drmModeFreePlaneResources(resources_plane);
	return ret;
}

static int
kms_plane_properties_get(struct kms_plane *plane)
{
	drmModeObjectProperties *properties;
	int i;

	/* now that we have our plane, get the relevant property ids */
	properties = drmModeObjectGetProperties(plane->kms->kms_fd,
						plane->plane_id,
						DRM_MODE_OBJECT_PLANE);
	if (!properties) {
		/* yes, no properties returns EINVAL */
		if (errno != EINVAL) {
			fprintf(stderr,
				"Failed to get object %u properties: %s\n",
				plane->plane_id, strerror(errno));
			return -errno;
		}
		return 0;
	}

	for (i = 0; i < (int) properties->count_props; i++) {
		drmModePropertyRes *property;

		property = drmModeGetProperty(plane->kms->kms_fd,
					      properties->props[i]);
		if (!property) {
			fprintf(stderr, "Failed to get object %u "
				"property %u: %s\n", plane->plane_id,
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
		else
			printf("Unhandled property: %s\n", property->name);

		drmModeFreeProperty(property);
	}

	drmModeFreeObjectProperties(properties);

	return 0;
}

static void
kms_layout_show(struct kms_display *display, struct kms_plane *plane,
		const char *name)
{
	printf("%s: %s, mode: %s\n", name,
	       display->connected ? "connected" : "disconnected",
	       display->mode_ok ? "set" : "disabled");
	printf("\tFB -> Plane(0x%02X) -> CRTC(0x%02X) -> Encoder(0x%02X) ->"
	       " Connector(0x%02X);\n", plane->plane_id,
	       display->crtc_id, display->encoder_id, display->connector_id);
}

static int
kms_buffer_planar_get(int kms_fd, struct buffer *buffer,
		      int width, int height, uint32_t format)
{
	uint32_t handles[4] = { 0 };
	uint32_t pitches[4] = { 0 };
	uint32_t offsets[4] = { 0 };
	int ret, i;

	buffer->width = width;
	buffer->height = height;
	buffer->format = format;

	for (i = 0; i < 3; i++) {
		struct drm_mode_create_dumb buffer_create = { 0 };
		struct drm_mode_map_dumb buffer_map = { 0 };
		struct buffer_plane *plane = &buffer->planes[i];

		buffer_create.width = width;
		buffer_create.height = height;
		buffer_create.bpp = 8;
		ret = drmIoctl(kms_fd, DRM_IOCTL_MODE_CREATE_DUMB,
			       &buffer_create);
		if (ret) {
			fprintf(stderr, "%s: failed to create buffer: %s\n",
			__func__, strerror(errno));
			return ret;
		}

		plane->handle = buffer_create.handle;
		plane->size = buffer_create.size;
		plane->pitch = buffer_create.pitch;
		printf("buffer_plane %d: Created buffer %dx%d@%dbpp: "
		       "%02u (%tdbytes)\n", i, buffer->width, buffer->height,
		       buffer_create.bpp, plane->handle, plane->size);

		buffer_map.handle = plane->handle;
		ret = drmIoctl(kms_fd, DRM_IOCTL_MODE_MAP_DUMB,
			       &buffer_map);
		if (ret) {
			fprintf(stderr, "%s: failed to map buffer: %s\n",
				__func__, strerror(errno));
			return -errno;
		}

		plane->map_offset = buffer_map.offset;
		printf("buffer_plane %d: Mapped buffer %02u at offset "
		       "0x%jX\n", i, plane->handle, plane->map_offset);

		plane->map = mmap(0, plane->size, PROT_READ | PROT_WRITE,
				   MAP_SHARED, kms_fd, plane->map_offset);
		if (plane->map == MAP_FAILED) {
			fprintf(stderr, "%s: failed to mmap buffer: %s\n",
				__func__, strerror(errno));
			return -errno;
		}

		printf("buffer_plane %d: MMapped buffer %02u to %p\n",
		       i, plane->handle, plane->map);

		handles[i] = plane->handle;
		pitches[i] = plane->pitch;
	}

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

static int
kms_buffer_argb8888_get(int kms_fd, struct buffer *buffer,
			int width, int height, uint32_t format)
{
	struct drm_mode_create_dumb buffer_create = { 0 };
	struct drm_mode_map_dumb buffer_map = { 0 };
	struct buffer_plane *plane = &buffer->planes[0];
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

	plane->handle = buffer_create.handle;
	plane->size = buffer_create.size;
	plane->pitch = buffer_create.pitch;
	printf("buffer_plane: Created buffer %dx%d@%dbpp: "
	       "%02u (%tdbytes)\n", buffer->width, buffer->height,
	       buffer_create.bpp, plane->handle, plane->size);

	buffer_map.handle = plane->handle;
	ret = drmIoctl(kms_fd, DRM_IOCTL_MODE_MAP_DUMB, &buffer_map);
	if (ret) {
		fprintf(stderr, "%s: failed to map buffer: %s\n",
			__func__, strerror(errno));
		return -errno;
	}

	plane->map_offset = buffer_map.offset;
	printf("buffer_plane: Mapped buffer %02u at offset 0x%jX\n",
	       plane->handle, plane->map_offset);

	plane->map = mmap(0, plane->size, PROT_READ | PROT_WRITE,
			  MAP_SHARED, kms_fd, plane->map_offset);
	if (plane->map == MAP_FAILED) {
		fprintf(stderr, "%s: failed to mmap buffer: %s\n",
			__func__, strerror(errno));
		return -errno;
	}

	printf("buffer_plane: MMapped buffer %02u to %p\n",
	       plane->handle, plane->map);

	handles[0] = plane->handle;
	pitches[0] = plane->pitch;

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
 * Show input buffer on projector, scaled, with borders.
 */
static void
kms_projector_capture_set(struct kms_projector *projector,
			  struct buffer *buffer,
			  drmModeAtomicReqPtr request)
{
	struct kms_display *display = projector->display;
	struct kms_plane *plane = projector->capture;

	if (!plane->active) {
		int x, y, w, h;

		drmModeAtomicAddProperty(request, plane->plane_id,
					 plane->property_crtc_id,
					 display->crtc_id);

		/* Scale, with borders, and center */
		if ((buffer->width == display->crtc_width) &&
		    (buffer->height == display->crtc_height)) {
			x = 0;
			y = 0;
			w = display->crtc_width;
			h = display->crtc_height;
		} else {
			/* first, try to fit horizontally. */
			w = display->crtc_width;
			h = buffer->height * display->crtc_width /
				buffer->width;

			/* if height does not fit, inverse the logic */
			if (h > display->crtc_height) {
				h = display->crtc_height;
				w = buffer->width * display->crtc_height /
					buffer->height;
			}

			/* center */
			x = (display->crtc_width - w) / 2;
			y = (display->crtc_height -h) / 2;
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
				 buffer->fb_id);
}

/*
 * HDMI or VGA used for the projector.
 */
static int
kms_projector_init(struct kms *kms)
{
	struct kms_projector *projector = kms->projector;
	struct kms_display *display = projector->display;
	struct kms_plane *plane = projector->capture;
	int ret;

	projector->kms = kms;
	display->kms = kms;

	ret = kms_connector_id_get(display, DRM_MODE_CONNECTOR_HDMIA);
	if (ret)
		return ret;

	ret = kms_connection_check(display);
	if (ret)
		return ret;

	ret = kms_crtc_id_get(display);
	if (ret)
		return ret;

	plane->kms = kms;
	plane->plane_id = kms_plane_id_get(display, kms->format, 0);
	if (!plane->plane_id)
		return -ENODEV;

	ret = kms_plane_properties_get(plane);
	if (ret)
		return ret;

	kms_layout_show(display, plane, "Projector");

	return 0;
}

/*
 * Show input buffer on the status lcd, in the top right corner.
 */
static void
kms_status_capture_set(struct kms_status *status, struct buffer *buffer,
		       drmModeAtomicReqPtr request)
{
	struct kms_display *display = status->display;
	struct kms_plane *plane = status->capture;

	if (!plane->active) {
		int x, y, w, h;

		drmModeAtomicAddProperty(request, plane->plane_id,
					 plane->property_crtc_id,
					 display->crtc_id);

#if 0
		/* top right corner */
		x = display->crtc_width / 2;
		y = 0;
		w = display->crtc_width / 2;
		h = buffer->height * w / buffer->width;
#else
				/* Scale, with borders, and center */
		if ((buffer->width == display->crtc_width) &&
		    (buffer->height == display->crtc_height)) {
			x = 0;
			y = 0;
			w = display->crtc_width;
			h = display->crtc_height;
		} else {
			/* first, try to fit horizontally. */
			w = display->crtc_width;
			h = buffer->height * display->crtc_width /
				buffer->width;

			/* if height does not fit, inverse the logic */
			if (h > display->crtc_height) {
				h = display->crtc_height;
				w = buffer->width * display->crtc_height /
					buffer->height;
			}

			/* center */
			x = (display->crtc_width - w) / 2;
			y = (display->crtc_height -h) / 2;
		}
#endif

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
					 0x6000);
		//0xFFFF);
		plane->active = true;
	}

	/* actual flip. */
	drmModeAtomicAddProperty(request, plane->plane_id,
				 plane->property_fb_id,
				 buffer->fb_id);
}

/*
 * Show status text on bottom of the status lcd.
 */
static void
kms_status_text_set(struct kms_status *status, drmModeAtomicReqPtr request)
{
	struct kms_display *display = status->display;
	struct kms_plane *plane = status->text;
	struct buffer *buffer = status->text_buffer;

	if (!plane->active) {
		int x, y, w, h;

		drmModeAtomicAddProperty(request, plane->plane_id,
					 plane->property_crtc_id,
					 display->crtc_id);

		/* bottom, with a bit of space remaining */
		x = 8;
		y = display->crtc_height - 8 - buffer->height;
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
	struct kms_display *display = status->display;
	struct kms_plane *plane = status->logo;
	struct buffer *buffer = status->logo_buffer;

	if (!plane->active) {
		int x, y, w, h;

		drmModeAtomicAddProperty(request, plane->plane_id,
					 plane->property_crtc_id,
					 display->crtc_id);

		/* top right, with a bit of space remaining */
		x = display->crtc_width - 8 - buffer->width;
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
#include "fosdem_status_text.c"
#include "fosdem_logo.c"

static int
kms_status_init(struct kms *kms)
{
	struct kms_status *status = kms->status;
	struct kms_display *display = status->display;
	struct kms_plane *capture = status->capture;
	struct kms_plane *text = status->text;
	struct kms_plane *logo = status->logo;
	int ret;

	status->kms = kms;
	display->kms = kms;

	ret = kms_connector_id_get(display, DRM_MODE_CONNECTOR_DPI);
	if (ret)
		return ret;

	ret = kms_connection_check(display);
	if (ret)
		return ret;

	ret = kms_crtc_id_get(display);
	if (ret)
		return ret;

	capture->kms = kms;
	capture->plane_id = kms_plane_id_get(display, kms->format, 0);
	if (!capture->plane_id)
		return -ENODEV;

	ret = kms_plane_properties_get(capture);
	if (ret)
		return ret;

	kms_layout_show(display, capture, "Status:Capture");

	text->kms = kms;
	text->plane_id = kms_plane_id_get(display,  STATUS_TEXT_FORMAT, 1);
	if (!text->plane_id)
		return -ENODEV;

	ret = kms_plane_properties_get(text);
	if (ret)
		return ret;

	kms_layout_show(display, text, "Status:Text");

	ret = kms_buffer_argb8888_get(kms->kms_fd, status->text_buffer,
				      STATUS_TEXT_WIDTH, STATUS_TEXT_HEIGHT,
				      STATUS_TEXT_FORMAT);
	if (ret)
		return ret;

	memcpy(status->text_buffer->planes[0].map, status_text_bitmap,
	       status->text_buffer->planes[0].size);

	logo->kms = kms;
	logo->plane_id = kms_plane_id_get(display, LOGO_FORMAT, 2);
	if (!logo->plane_id)
		return -ENODEV;

	ret = kms_plane_properties_get(logo);
	if (ret)
		return ret;

	kms_layout_show(display, logo, "Status:Logo");

	ret = kms_buffer_argb8888_get(kms->kms_fd, status->logo_buffer,
				      LOGO_WIDTH, LOGO_HEIGHT, LOGO_FORMAT);
	if (ret)
		return ret;

	memcpy(status->logo_buffer->planes[0].map, logo_bitmap,
	       status->logo_buffer->planes[0].size);

	return 0;
}

static int
kms_buffer_show(struct kms *kms, struct buffer *buffer, int frame)
{
	drmModeAtomicReqPtr request;
	int ret;

	request = drmModeAtomicAlloc();

	kms_status_capture_set(kms->status, buffer, request);
	kms_status_text_set(kms->status, request);
	kms_status_logo_set(kms->status, request);

	kms_projector_capture_set(kms->projector, buffer, request);

	ret = drmModeAtomicCommit(kms->kms_fd, request,
				  DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);

	drmModeAtomicFree(request);

	if (ret) {
		fprintf(stderr, "%s: failed to show fb %02u: %s\n",
			__func__, buffer->fb_id, strerror(errno));
		ret = -errno;
	} else {
		printf("\rShowing fb %02u for frame %d", buffer->fb_id, frame);
		fflush(stdout);
	}

	return ret;

}

static int
kms_buffers_test_create(struct kms *kms)
{
	int ret;

	ret = kms_buffer_planar_get(kms->kms_fd, kms->buffers[0],
				    kms->width, kms->height, kms->format);
	if (ret)
		return ret;

	memset(kms->buffers[0]->planes[0].map, 0xFF,
	       kms->buffers[0]->planes[0].size);

	ret = kms_buffer_planar_get(kms->kms_fd, kms->buffers[1],
				    kms->width, kms->height, kms->format);
	if (ret)
		return ret;

	memset(kms->buffers[1]->planes[1].map, 0xFF,
	       kms->buffers[1]->planes[1].size);

	ret = kms_buffer_planar_get(kms->kms_fd, kms->buffers[2],
				    kms->width, kms->height, kms->format);
	if (ret)
		return ret;

	memset(kms->buffers[2]->planes[2].map, 0xFF,
	       kms->buffers[2]->planes[2].size);

	return 0;
}

static int
kms_buffers_test_show(struct kms *kms, unsigned long count)
{
	int ret, i;

	for (i = 0; i < count;) {
		ret = kms_buffer_show(kms, kms->buffers[0], i);
		if (ret)
			return ret;
		i++;

		//sleep(1);

		ret = kms_buffer_show(kms, kms->buffers[1], i);
		if (ret)
			return ret;
		i++;

		//sleep(1);

		ret = kms_buffer_show(kms, kms->buffers[2], i);
		if (ret)
			return ret;
		i++;

		//sleep(1);
	}

	printf("\n");

	return 0;
}

int
kms_init(int width, int height, int bpp, uint32_t format, unsigned long count)
{
	struct kms kms[1] = {{ 0 }};
	int ret;

	kms->width = width;
	kms->height = height;
	kms->bpp = bpp;
	kms->format = format;

	ret = kms_fd_init(kms, "sun4i-drm");
	if (ret)
		return ret;

	ret = kms_crtc_indices_get(kms);
	if (ret)
		return ret;

	ret = kms_status_init(kms);
	if (ret)
		return ret;

	ret = kms_projector_init(kms);
	if (ret)
		return ret;

	ret = kms_buffers_test_create(kms);
	if (ret)
		return ret;

	ret = kms_buffers_test_show(kms, count);
	if (ret)
		return ret;

	return 0;
}
