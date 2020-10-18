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

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>

#include <png.h>

#include "juggler.h"
#include "kms.h"
#include "capture.h"

/* so that our capture side can use this separately. */
int kms_fd = -1;

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

static __maybe_unused const char *
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

const char *
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

static __maybe_unused const char *
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

int
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

int
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
int
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

int
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

struct _drmModeModeInfo *
kms_modeline_arguments_parse(int argc, char *argv[])
{
	struct _drmModeModeInfo *mode;
	float dotclock, refresh;
	int ret, val;

	if (argc != 11) {
		fprintf(stderr, "Error: not enough arguments.\n");
		return NULL;
	}

	mode = calloc(1, sizeof(struct _drmModeModeInfo));
	if (!mode) {
		fprintf(stderr, "%s(): failed to allocated mode.\n",
			__func__);
		return NULL;
	}

	ret = sscanf(argv[0], "%f", &dotclock);
	if (ret != 1) {
		fprintf(stderr, "Failed to read dotclock from %s.\n",
			argv[0]);
		goto error;
	}
	mode->clock = dotclock * 1000;


	ret = sscanf(argv[1], "%d", &val);
	if (ret != 1) {
		fprintf(stderr, "Failed to read hdisplay from %s.\n",
			argv[1]);
		goto error;
	}
	mode->hdisplay = val;

	ret = sscanf(argv[2], "%d", &val);
	if (ret != 1) {
		fprintf(stderr, "Failed to read hsync_start from %s.\n",
			argv[2]);
		goto error;
	}
	mode->hsync_start = val;

	ret = sscanf(argv[3], "%d", &val);
	if (ret != 1) {
		fprintf(stderr, "Failed to read hsync_end from %s.\n",
			argv[3]);
		goto error;
	}
	mode->hsync_end = val;

	ret = sscanf(argv[4], "%d", &val);
	if (ret != 1) {
		fprintf(stderr, "Failed to read htotal from %s.\n",
			argv[4]);
		goto error;
	}
	mode->htotal = val;

	ret = sscanf(argv[5], "%d", &val);
	if (ret != 1) {
		fprintf(stderr, "Failed to read vdisplay from %s.\n",
			argv[5]);
		goto error;
	}
	mode->vdisplay = val;

	ret = sscanf(argv[6], "%d", &val);
	if (ret != 1) {
		fprintf(stderr, "Failed to read vsync_start from %s.\n",
			argv[6]);
		goto error;
	}
	mode->vsync_start = val;

	ret = sscanf(argv[7], "%d", &val);
	if (ret != 1) {
		fprintf(stderr, "Failed to read vsync_end from %s.\n",
			argv[7]);
		goto error;
	}
	mode->vsync_end = val;

	ret = sscanf(argv[8], "%d", &val);
	if (ret != 1) {
		fprintf(stderr, "Failed to read vtotal from %s.\n",
			argv[8]);
		goto error;
	}
	mode->vtotal = val;

	mode->flags &= ~0x03;
	if (!strcmp(argv[9], "+hsync"))
		mode->flags |= DRM_MODE_FLAG_PHSYNC;
	else if (!strcmp(argv[9], "-hsync"))
		mode->flags |= DRM_MODE_FLAG_NHSYNC;
	else {
		fprintf(stderr, "Failed to read hsync polarity from %s.\n",
			argv[9]);
		goto error;
	}

	mode->flags &= ~0x0C;
	if (!strcmp(argv[10], "+vsync"))
		mode->flags |= DRM_MODE_FLAG_PVSYNC;
	else if (!strcmp(argv[10], "-vsync"))
		mode->flags |= DRM_MODE_FLAG_NVSYNC;
	else {
		fprintf(stderr, "Failed to read vsync polarity from %s.\n",
			argv[10]);
		goto error;
	}

	refresh = (mode->clock * 1000.0) /
		(mode->htotal * mode->vtotal);

	snprintf(mode->name, DRM_DISPLAY_MODE_LEN, "%dx%d@%2.2fHz",
		 mode->hdisplay, mode->vdisplay, refresh);
	mode->vrefresh = refresh;

	if (mode->clock < 1000.0) {
		fprintf(stderr, "Error: clock %2.2f is too low.\n",
			mode->clock / 1000.0);
		goto error;
	}

	if (mode->clock > 500000.0) {
		fprintf(stderr, "Error: clock %2.2f is too low.\n",
			mode->clock / 1000.0);
		goto error;
	}

	if ((mode->hdisplay <= 0) || (mode->hdisplay > 4096)) {
		fprintf(stderr, "Error: Invalid HDisplay %d\n",
			mode->hdisplay);
		goto error;
	}

	if ((mode->hsync_start <= 0) || (mode->hsync_start > 4096)) {
		fprintf(stderr, "Error: Invalid HSync Start %d\n",
			mode->hsync_start);
		goto error;
	}

	if ((mode->hsync_end <= 0) || (mode->hsync_end > 4096)) {
		fprintf(stderr, "Error: Invalid HSync End %d\n",
			mode->hsync_end);
		goto error;
	}

	if ((mode->htotal <= 0) || (mode->htotal > 4096)) {
		fprintf(stderr, "Error: Invalid HTotal %d\n",
			mode->htotal);
		goto error;
	}

	if ((mode->vdisplay <= 0) || (mode->vdisplay > 4096)) {
		fprintf(stderr, "Error: Invalid VDisplay %d\n",
			mode->vdisplay);
		goto error;
	}

	if ((mode->vsync_start <= 0) || (mode->vsync_start > 4096)) {
		fprintf(stderr, "Error: Invalid VSync Start %d\n",
			mode->vsync_start);
		goto error;
	}

	if ((mode->vsync_end <= 0) || (mode->vsync_end > 4096)) {
		fprintf(stderr, "Error: Invalid VSync End %d\n",
			mode->vsync_end);
		goto error;
	}

	if ((mode->vtotal <= 0) || (mode->vtotal > 4096)) {
		fprintf(stderr, "Error: Invalid VTotal %d\n",
			mode->vtotal);
		goto error;
	}

	if (mode->hdisplay > mode->hsync_start) {
		fprintf(stderr, "Error: HDisplay %d is above HSync Start %d\n",
			mode->hdisplay, mode->hsync_start);
		goto error;
	}

	if (mode->hsync_start > mode->hsync_end) {
		fprintf(stderr, "Error: HSync Start %d is above HSync End %d\n",
			mode->hsync_start, mode->hsync_end);
		goto error;
	}

	if (mode->hsync_end > mode->htotal) {
		fprintf(stderr, "Error: HSync End %d is above HTotal %d\n",
			mode->hsync_end, mode->htotal);
		goto error;
	}

	if (mode->vdisplay > mode->vsync_start) {
		fprintf(stderr, "Error: VDisplay %d is above VSync Start %d\n",
			mode->vdisplay, mode->vsync_start);
		goto error;
	}

	if (mode->vsync_start > mode->vsync_end) {
		fprintf(stderr, "Error: VSync Start %d is above VSync End %d\n",
			mode->vsync_start, mode->vsync_end);
		goto error;
	}

	if (mode->vsync_end > mode->vtotal) {
		fprintf(stderr, "Error: VSync End %d is above VTotal %d\n",
			mode->vsync_end, mode->vtotal);
		goto error;
	}

	/*
	 * Here we lock down the vertical refresh to around 60Hz, as we
	 * do not want to run our displays too far from 60Hz, even when
	 * playing with the timing.
	 *
	 */
	if (refresh < 55.0) {
		fprintf(stderr, "Error: refresh rate too low: %2.2f\n",
			refresh);
		goto error;
	}

	if (refresh > 65.0) {
		fprintf(stderr, "Error: refresh rate too high: %2.2f\n",
			refresh);
		goto error;
	}

	return mode;

 error:
	free(mode);
	return NULL;
}

void
kms_modeline_print(struct _drmModeModeInfo *mode)
{
	printf("Modeline  \"%s\"  %.2f  %d %d %d %d  %d %d %d %d  "
	       "%chsync %cvsync\n", mode->name, mode->clock / 1000.0,
	       mode->hdisplay, mode->hsync_start, mode->hsync_end, mode->htotal,
	       mode->vdisplay, mode->vsync_start, mode->vsync_end, mode->vtotal,
	       (mode->flags & DRM_MODE_FLAG_PHSYNC) ? '+' : '-',
	       (mode->flags & DRM_MODE_FLAG_PVSYNC) ? '+' : '-');
}

struct _drmModeModeInfo *
kms_crtc_modeline_get(uint32_t crtc_id)
{
	drmModePropertyBlobRes *blob;
	drmModeObjectProperties *properties;
	struct _drmModeModeInfo *mode;
	uint32_t blob_id;
	int i;

	properties = drmModeObjectGetProperties(kms_fd, crtc_id,
						DRM_MODE_OBJECT_CRTC);
	if (!properties) {
		/* yes, if there are no properties, this returns EINVAL */
		if (errno != EINVAL) {
			fprintf(stderr,
				"%s(0x%02X): Failed to get properties: %s\n",
				__func__, crtc_id, strerror(errno));
			return NULL;
		}

		return NULL;
	}

	for (i = 0; i < (int) properties->count_props; i++) {
		drmModePropertyRes *property;

		property = drmModeGetProperty(kms_fd,
					      properties->props[i]);
		if (!property) {
			fprintf(stderr, "%s(0x%02X): Failed to get property"
				" %u: %s\n", __func__, crtc_id,
				properties->props[i], strerror(errno));
			continue;
		}

		if (!strcmp(property->name, "MODE_ID")) {
			/*
			 * So, wait, a blob id value comes from the list of
			 * properties, and is not separately present in the
			 * actual property? WTF?
			 */
			blob_id = (uint32_t) properties->prop_values[i];
			drmModeFreeProperty(property);
			break;
		}

		drmModeFreeProperty(property);
	}

	if (i == (int) properties->count_props) {
		fprintf(stderr, "%s(0x%02X): Failed to get MODE_ID property\n",
			__func__, crtc_id);
		drmModeFreeObjectProperties(properties);
		return NULL;
	}

	drmModeFreeObjectProperties(properties);

	blob = drmModeGetPropertyBlob(kms_fd, blob_id);
	if (!blob) {
		fprintf(stderr, "%s(0x%02X): Failed to get property blob "
			"%X: %s\n", __func__, crtc_id, blob_id,
			strerror(errno));
		return NULL;
	}

	if (blob->length != sizeof(drmModeModeInfo)) {
		fprintf(stderr, "%s(0x%02X): wrong blob size: "
			"%d should be %d\n", __func__, crtc_id,
			blob->length, (int) sizeof(drmModeModeInfo));
		drmModeFreePropertyBlob(blob);
		return NULL;
	}

	mode = calloc(1, sizeof(struct _drmModeModeInfo));
	memcpy(mode, blob->data, sizeof(struct _drmModeModeInfo));

	drmModeFreePropertyBlob(blob);

	return mode;
}

int
kms_crtc_modeline_set(uint32_t crtc_id, struct _drmModeModeInfo *mode)
{
	drmModeObjectProperties *properties;
	drmModeAtomicReqPtr request;
	uint32_t prop_id, blob_id;
	int i, ret;

	properties = drmModeObjectGetProperties(kms_fd, crtc_id,
						DRM_MODE_OBJECT_CRTC);
	if (!properties) {
		/* yes, if there are no properties, this returns EINVAL */
		if (errno != EINVAL) {
			fprintf(stderr,
				"%s(0x%02X): Failed to get properties: %s\n",
				__func__, crtc_id, strerror(errno));
			return -errno;
		}

		return -1;
	}

	for (i = 0; i < (int) properties->count_props; i++) {
		drmModePropertyRes *property;

		property = drmModeGetProperty(kms_fd,
					      properties->props[i]);
		if (!property) {
			fprintf(stderr, "%s(0x%02X): Failed to get property"
				" %u: %s\n", __func__, crtc_id,
				properties->props[i], strerror(errno));
			continue;
		}

		if (!strcmp(property->name, "MODE_ID")) {
			prop_id = property->prop_id;
			drmModeFreeProperty(property);
			break;
		}

		drmModeFreeProperty(property);
	}

	if (i == (int) properties->count_props) {
		fprintf(stderr, "%s(0x%02X): Failed to get MODE_ID property\n",
			__func__, crtc_id);
		drmModeFreeObjectProperties(properties);
		return -1;
	}

	drmModeFreeObjectProperties(properties);

	ret = drmModeCreatePropertyBlob(kms_fd, mode,
					sizeof(struct _drmModeModeInfo),
					&blob_id);
	if (ret) {
		fprintf(stderr,  "%s(0x%02X): Failed to get PropertyBlob: %s\n",
			__func__, crtc_id, strerror(errno));
		return ret;
	}

	request = drmModeAtomicAlloc();

	drmModeAtomicAddProperty(request, crtc_id, prop_id, blob_id);

	ret = drmModeAtomicCommit(kms_fd, request,
				  DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);

	drmModeAtomicFree(request);
	drmModeDestroyPropertyBlob(kms_fd, blob_id);

	if (ret) {
		fprintf(stderr, "%s(0x%02X): failed to set mode blob: %s\n",
			__func__, crtc_id, strerror(errno));
		return ret;
	}

	return 0;
}

struct kms_plane *
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

	printf("%s(): Created Plane 0x%02X\n", __func__, plane->plane_id);

	return plane;
}

struct kms_buffer *
kms_buffer_get(int width, int height, uint32_t format)
{
	struct kms_buffer *buffer;
	struct drm_mode_create_dumb buffer_create = { 0 };
	struct drm_mode_map_dumb buffer_map = { 0 };
	uint32_t handles[4] = { 0 };
	uint32_t pitches[4] = { 0 };
	uint32_t offsets[4] = { 0 };
	int ret;

	buffer_create.width = width;
	buffer_create.height = height;
	buffer_create.bpp = 32;
	ret = drmIoctl(kms_fd, DRM_IOCTL_MODE_CREATE_DUMB, &buffer_create);
	if (ret) {
		fprintf(stderr, "%s: failed to create buffer: %s\n",
			__func__, strerror(errno));
		return NULL;
	}

	buffer = calloc(1, sizeof(struct kms_buffer));
	buffer->width = width;
	buffer->height = height;
	buffer->format = format;

	buffer->handle = buffer_create.handle;
	buffer->size = buffer_create.size;
	buffer->pitch = buffer_create.pitch;

	buffer_map.handle = buffer->handle;
	ret = drmIoctl(kms_fd, DRM_IOCTL_MODE_MAP_DUMB, &buffer_map);
	if (ret) {
		fprintf(stderr, "%s: failed to map buffer: %s\n",
			__func__, strerror(errno));
		return NULL;
	}

	buffer->map_offset = buffer_map.offset;

	buffer->map = mmap(0, buffer->size, PROT_READ | PROT_WRITE,
			  MAP_SHARED, kms_fd, buffer->map_offset);
	if (buffer->map == MAP_FAILED) {
		fprintf(stderr, "%s: failed to mmap buffer: %s\n",
			__func__, strerror(errno));
		return NULL;
	}

	handles[0] = buffer->handle;
	pitches[0] = buffer->pitch;

	ret = drmModeAddFB2(kms_fd, buffer->width, buffer->height,
			    buffer->format, handles, pitches, offsets,
			    &buffer->fb_id, 0);
	if (ret) {
		fprintf(stderr, "%s: failed to create fb: %s\n",
			__func__, strerror(errno));
		return NULL;
	}

	printf("%s(): Created FB 0x%02X (%dx%d, %tdbytes).\n", __func__,
	       buffer->fb_id, buffer->width, buffer->height, buffer->size);

	return buffer;
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
int
kms_buffer_release(struct capture_buffer *buffer)
{
	int ret, i;

	printf("%s(%d, %d);\n", __func__, buffer->index, buffer->kms_fb_id);

	ret = drmModeRmFB(kms_fd, buffer->kms_fb_id);
	if (ret) {
		fprintf(stderr, "%s(%d, %d) failed: %s.\n", __func__,
			buffer->index, buffer->kms_fb_id, strerror(errno));
		return ret;
	}

	for (i = 0; i < 3; i++) {
		struct drm_gem_close gem_close[1] = {{
			.handle = buffer->planes[i].prime_handle,
		}};

		ret = drmIoctl(kms_fd, DRM_IOCTL_GEM_CLOSE, gem_close);
		if (ret) {
			fprintf(stderr, "%s: drmIoctl(GEM_CLOSE), %d) "
				"failed: %s\n", __func__,
				buffer->planes[i].prime_handle,
				strerror(errno));
			return ret;
		}
	}

	return 0;
}

/*
 *
 */
struct kms_buffer *
kms_png_read(const char *filename)
{
	struct kms_buffer *buffer;
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

	buffer = kms_buffer_get(image->width, image->height,
				DRM_FORMAT_ARGB8888);
	if (!buffer) {
		fprintf(stderr, "%s(): failed to create buffer for %s\n",
			__func__, filename);
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
 * Yes, you really need all this to disable a plane.
 */
void
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

int
kms_init(void)
{
	int ret;

	ret = kms_fd_init("sun4i-drm");
	if (ret)
		return ret;

	ret = kms_crtc_indices_get();
	if (ret)
		return ret;

	return 0;
}
