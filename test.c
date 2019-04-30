/*
 * Copyright (c) 2012, 2019 Luc Verhaegen <libv@skynet.be>
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

struct buffer {
	uint32_t handle; /* dumb buffer handle */

	int pitch;
	size_t size;

	uint64_t map_offset;
	void *map;

	uint32_t fb_id;
};

struct test {
	int kms_fd;

	uint32_t crtc_id;
	int crtc_width;
	int crtc_height;

	uint32_t plane_id;

	/* buffer info */
	int width;
	int height;
	int bpp;
	uint32_t format;

	/* actual buffers */
	int buffer_count;
	struct buffer buffers[2][1];
};

static int
kms_init(struct test *test, const char *driver_name)
{
	int ret;

	test->kms_fd = drmOpen(driver_name, NULL);
	if (test->kms_fd == -1) {
		fprintf(stderr, "Error: Failed to open KMS driver %s: %s\n",
			driver_name, strerror(errno));
		return errno;
	}

	ret = drmSetClientCap(test->kms_fd, DRM_CLIENT_CAP_ATOMIC, 1);
	if (ret < 0) {
		fprintf(stderr, "Error: Unable to set DRM_CLIENT_CAP_ATOMIC:"
			" %s\n", strerror(errno));
		return ret;
	}

	ret = drmSetClientCap(test->kms_fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
	if (ret < 0) {
		fprintf(stderr, "Error: Unable to set "
			"DRM_CLIENT_CAP_UNIVERSAL_PLANES: %s\n",
			strerror(errno));
		return ret;
	}

return 0;
}

static void
kms_object_properties_print(int fd, uint32_t id, uint32_t type)
{
	drmModeObjectProperties *props;
	int i;

	props = drmModeObjectGetProperties(fd, id, type);
	if (!props) {
		/* yes, no properties returns EINVAL */
		if (errno != EINVAL)
			fprintf(stderr,
				"Failed to get object %u properties: %s\n",
				id, strerror(errno));
		return;
	}

	printf("\t\t   Properties:\n");
	for (i = 0; i < (int) props->count_props; i++) {
		drmModePropertyRes *property;

		property = drmModeGetProperty(fd, props->props[i]);
		if (!property) {
			fprintf(stderr,
				"Failed to get object %u property %u: %s\n",
				id, props->props[i], strerror(errno));
			continue;
		}

		printf("\t\t\t%02d: %s\n", property->prop_id, property->name);

		drmModeFreeProperty(property);
	}

	drmModeFreeObjectProperties(props);
}

static void
kms_fb_print(int fd, uint32_t id)
{
	drmModeFB *fb;

	fb = drmModeGetFB(fd, id);
	if (!fb) {
		fprintf(stderr, "Failed to get FB %u: %s\n", id,
			strerror(errno));
		return;
	}

	printf("\t\t%02d: %4dx%4d(%4d)@%d/%d\n", fb->fb_id,
	       fb->width, fb->height, fb->pitch, fb->depth, fb->bpp);

	drmModeFreeFB(fb);

	kms_object_properties_print(fd, id, DRM_MODE_OBJECT_FB);
}

static void
kms_plane_print(int fd, uint32_t id)
{
	drmModePlane *plane;
	int i;

	plane = drmModeGetPlane(fd, id);
	if (!plane) {
		fprintf(stderr, "Failed to get Plane %u: %s\n", id,
			strerror(errno));
		return;
	}

	printf("\t\t%02d: FB %02d (%4dx%4d), CRTC %02d (%4dx%4d),"
	       " Possible CRTCs 0x%02X\n", plane->plane_id, plane->fb_id,
	       plane->crtc_x, plane->crtc_y, plane->crtc_id, plane->x, plane->y,
	       plane->possible_crtcs);
	printf("\t\t   Supported Formats:");
	for (i = 0; i < (int) plane->count_formats; i++) {
		if (((i - 7) % 9) == 0)
			printf("\n\t\t\t");
		printf(" %C%C%C%C,", plane->formats[i] & 0xFF,
		       (plane->formats[i] >> 8) & 0xFF,
		       (plane->formats[i] >> 16) & 0xFF,
		       (plane->formats[i] >> 24) & 0xFF);
	}
	printf("\n");

	drmModeFreePlane(plane);

	kms_object_properties_print(fd, id, DRM_MODE_OBJECT_PLANE);
}

static void
kms_crtc_print(int fd, uint32_t id)
{
	drmModeCrtc *crtc;

	crtc = drmModeGetCrtc(fd, id);
	if (!crtc) {
		fprintf(stderr, "Failed to get CRTC %u: %s\n", id,
			strerror(errno));
		return;
	}

	printf("\t\t%02d: FB %02d: %4d,%4d / %4dx%4d; Mode \"%s\"%s\n",
	       crtc->crtc_id, crtc->buffer_id, crtc->x, crtc->y,
	       crtc->width, crtc->height, crtc->mode.name,
	       crtc->mode_valid ? " (valid)" : "");

#if 0
	if (crtc->buffer_id) {
		/* For a laugh, try to get the FB connected to the CRTC,
		   even if it isn't listed */
		printf("Attached FB:\n");
		kms_fb_print(fd, crtc->buffer_id);
	}
#endif

	drmModeFreeCrtc(crtc);

	kms_object_properties_print(fd, id, DRM_MODE_OBJECT_CRTC);
}

static char *
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

static void
kms_encoder_print(int fd, uint32_t id)
{
	drmModeEncoder *encoder;

	encoder = drmModeGetEncoder(fd, id);
	if (!encoder) {
		fprintf(stderr, "Failed to get Encoder %u: %s\n", id,
			strerror(errno));
		return;
	}

	printf("\t\t%02d: %s, Crtc %02d. Possible Crtcs: 0x%02X, "
	       "Possible Clones: 0x%02X\n", encoder->encoder_id,
	       kms_encoder_string(encoder->encoder_type), encoder->crtc_id,
	       encoder->possible_crtcs, encoder->possible_clones);

	drmModeFreeEncoder(encoder);

	kms_object_properties_print(fd, id, DRM_MODE_OBJECT_ENCODER);
}

static char *
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

static char *
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

static void
kms_connector_print(int fd, uint32_t id)
{
	drmModeConnector *connector;
	int i;

	connector = drmModeGetConnector(fd, id);
	if (!connector) {
		fprintf(stderr, "%s: failed to get Connector %u: %s\n",
			__func__, id, strerror(errno));
		return;
	}

	printf("\t\t%02d: %s %1d, %s, Encoder %02d\n",
	       connector->connector_id,
	       kms_connector_string(connector->connector_type),
	       connector->connector_type_id,
	       kms_connection_string(connector->connection),
	       connector->encoder_id);
	printf("\t\t   Possible encoders:");
	for (i = 0; i < connector->count_encoders; i++)
		printf(" %02d,", connector->encoders[i]);
	printf("\n");

	drmModeFreeConnector(connector);
}

int
kms_resources_list(int fd)
{
	drmModeRes *resources;
	drmModePlaneRes *resources_plane;
	int i;

	resources = drmModeGetResources(fd);
	if (!resources) {
		fprintf(stderr, "%s: Failed to get KMS resources: %s\n",
			__func__, strerror(errno));
		return -EINVAL;
	}

	resources_plane = drmModeGetPlaneResources(fd);
	if (!resources_plane) {
		fprintf(stderr, "Failed to get KMS plane resources\n");
		drmModeFreeResources(resources);
		return -EINVAL;
	}

	printf("KMS resources:\n");
	printf("\tDimensions: (%d, %d) -> (%d, %d)\n",
	     resources->min_width, resources->min_height,
	     resources->max_width, resources->max_height);

	printf("\tFBs:\n");
	for (i = 0; i < resources->count_fbs; i++)
		kms_fb_print(fd, resources->fbs[i]);

	printf("\tPlanes:\n");
	for (i = 0; i < (int) resources_plane->count_planes; i++)
		kms_plane_print(fd, resources_plane->planes[i]);

	printf("\tCRTCs:\n");
	for (i = 0; i < resources->count_crtcs; i++)
		kms_crtc_print(fd, resources->crtcs[i]);

	printf("\tEncoders:\n");
	for (i = 0; i < resources->count_encoders; i++)
		kms_encoder_print(fd, resources->encoders[i]);

	printf("\tConnectors:\n");
	for (i = 0; i < resources->count_connectors; i++)
		kms_connector_print(fd, resources->connectors[i]);

	drmModeFreeResources(resources);
	drmModeFreePlaneResources(resources_plane);

	return 0;
}

static int
kms_plane_get(struct test *test)
{
	drmModeRes *resources;
	drmModeConnector *connector;
	drmModeEncoder *encoder;
	drmModeCrtc *crtc;
	drmModePlaneRes *resources_plane;
	drmModePlane *plane;
	uint32_t crtc_id, plane_id, encoder_id, connector_id;
	int i, j, ret, crtc_index;

	resources = drmModeGetResources(test->kms_fd);
	if (!resources) {
		fprintf(stderr, "%s: Failed to get KMS resources: %s\n",
			__func__, strerror(errno));
		return -EINVAL;
	}

	/* First, scan through our connectors. */
        for (i = 0; i < resources->count_connectors; i++) {
		connector_id = resources->connectors[i];

		connector = drmModeGetConnector(test->kms_fd, connector_id);
		if (!connector) {
			fprintf(stderr,
				"%s: failed to get Connector %u: %s\n",
				__func__, connector_id, strerror(errno));
			ret = -errno;
			goto error;
		}

		if ((connector->connector_type == DRM_MODE_CONNECTOR_HDMIA) &&
		    (connector->connection == DRM_MODE_CONNECTED))
			break;

		drmModeFreeConnector(connector);
	}

	if (i == resources->count_connectors) {
		fprintf(stderr, "%s: no active HDMI connector found.\n",
			__func__);
		ret = -ENODEV;
		goto error;
	}

	printf("Using HDMI Connector %02u\n", connector->connector_id);

	encoder_id = connector->encoder_id;

	drmModeFreeConnector(connector);

	/* Now look for our encoder */
	encoder = drmModeGetEncoder(test->kms_fd, encoder_id);
	if (!encoder) {
		fprintf(stderr, "%s: failed to get Encoder %u: %s\n",
			__func__, encoder_id, strerror(errno));
		ret = -errno;
		goto error;
	}

	printf("Using Encoder %02u\n", encoder_id);

	crtc_id = encoder->crtc_id;

	drmModeFreeEncoder(encoder);

	/* Now look for our CRTC */
	crtc = drmModeGetCrtc(test->kms_fd, crtc_id);
	if (!crtc) {
		fprintf(stderr, "%s: failed to get CRTC %u: %s\n",
			__func__, crtc_id, strerror(errno));
		ret = -errno;
		goto error;
	}

	if (!crtc->mode_valid) {
		fprintf(stderr, "%s: CRTC %u does not have a valid mode\n",
			__func__, crtc_id);
		ret = -EINVAL;
		goto error;
	}

	test->crtc_id = crtc_id;
	test->crtc_width = crtc->width;
	test->crtc_height = crtc->height;
	printf("Using CRTC %02u\n", crtc_id);

	drmModeFreeCrtc(crtc);

	/* Now get the crtc index, so we can see which planes work */
	for (i = 0; i < resources->count_crtcs; i++)
		if (resources->crtcs[i] == crtc_id)
			break;
	crtc_index = i;

	printf("CRTC has index %d\n", crtc_index);

	/* Get plane resources so we can start sifting through the planes */
	resources_plane = drmModeGetPlaneResources(test->kms_fd);
	if (!resources_plane) {
		fprintf(stderr, "%s: Failed to get KMS plane resources\n",
			__func__);
		ret = -EINVAL;
		goto error;
	}

	/* now cycle through the planes to find one for our crtc */
	for (i = 0; i < (int) resources_plane->count_planes; i++) {
		plane_id = resources_plane->planes[i];

		plane = drmModeGetPlane(test->kms_fd, plane_id);
		if (!plane) {
			fprintf(stderr, "%s: failed to get Plane %u: %s\n",
				__func__, plane_id, strerror(errno));
			ret = -errno;
			goto error;
		}

		if (!(plane->possible_crtcs & (1 << crtc_index)))
			goto plane_next;

		if (plane->crtc_id != 0)
			goto plane_next;

		if (plane->fb_id != 0)
			goto plane_next;

		for (j = 0; j < (int) plane->count_formats; j++)
			if (plane->formats[j] == DRM_FORMAT_ARGB8888)
				break;

		if (j == (int) plane->count_formats)
			goto plane_next;

		drmModeFreePlane(plane);
		break;

	plane_next:
		drmModeFreePlane(plane);
		continue;
	}

	if (i == (int) resources_plane->count_planes) {
		fprintf(stderr, "%s: failed to get a Plane for our needs.\n",
			__func__);
		ret = -ENODEV;
		goto error;
	}

	test->plane_id = plane_id;
	printf("Using Plane %02u\n", plane_id);

	drmModeFreePlaneResources(resources_plane);
	drmModeFreeResources(resources);

	return 0;
 error:
	drmModeFreePlaneResources(resources_plane);
	drmModeFreeResources(resources);

	return ret;
}

static int
kms_buffer_get(struct test *test, struct buffer *buffer)
{
	struct drm_mode_create_dumb buffer_create = { 0 };
	struct drm_mode_map_dumb buffer_map = { 0 };
	uint32_t handles[4] = { 0 };
	uint32_t pitches[4] = { 0 };
	uint32_t offsets[4] = { 0 };
	int ret;

	buffer_create.width = test->width;
	buffer_create.height = test->height;
	buffer_create.bpp = test->bpp;
	ret = drmIoctl(test->kms_fd, DRM_IOCTL_MODE_CREATE_DUMB,
		       &buffer_create);
	if (ret) {
		fprintf(stderr, "%s: failed to create buffer: %s\n",
			__func__, strerror(errno));
		return ret;
	}

	buffer->handle = buffer_create.handle;
	buffer->size = buffer_create.size;
	buffer->pitch = buffer_create.pitch;
	printf("Created buffer %dx%d@%dbpp: %02u (%dbytes)\n",
	       test->width, test->height, test->bpp,
	       buffer->handle, buffer->size);

	buffer_map.handle = buffer->handle;
	ret = drmIoctl(test->kms_fd, DRM_IOCTL_MODE_MAP_DUMB, &buffer_map);
	if (ret) {
		fprintf(stderr, "%s: failed to map buffer: %s\n",
			__func__, strerror(errno));
		return -errno;
	}

	buffer->map_offset = buffer_map.offset;
	printf("Mapped buffer %02u at offset 0x%llX\n", buffer->handle,
		buffer->map_offset);

	buffer->map = mmap(0, buffer->size, PROT_READ | PROT_WRITE, MAP_SHARED,
			   test->kms_fd, buffer->map_offset);
	if (buffer->map == MAP_FAILED) {
		fprintf(stderr, "%s: failed to mmap buffer: %s\n",
			__func__, strerror(errno));
		return -errno;
	}

	printf("MMapped buffer %02u to %p\n", buffer->handle, buffer->map);

	handles[0] = buffer->handle;
	pitches[0] = buffer->pitch;

	ret = drmModeAddFB2(test->kms_fd, test->width, test->height,
			    test->format, handles, pitches, offsets,
			    &buffer->fb_id, 0);
	if (ret) {
		fprintf(stderr, "%s: failed to create fb for buffer %02u: %s\n",
			__func__, buffer->handle, strerror(errno));
		return -errno;
	}

	printf("Created FB %02u from buffer %02u.\n",
	       buffer->fb_id, buffer->handle);

	return 0;
}

static void buffer_fill(struct test *test, struct buffer *buffer, int frame)
{
	uint32_t *data = buffer->map;
	int offset = 0;
	int x, y;

	for (y = 0; y < test->height; y++) {
		for (x = 0; x < test->width; x++) {
			data[offset] =
				((frame & 0xFF) << 0) |
				((x & 0xFF) << 8) |
				((y & 0xFF) << 16);
			offset++;
		}
	}
}

static int
kms_plane_display(struct test *test, struct buffer *buffer, int frame)
{
	int ret;

	buffer_fill(test, buffer, frame);

	/*
	 * << 16? when did that bs happen?
	 * Also, since when do we fail with just "einval"?
	 */
	ret = drmModeSetPlane(test->kms_fd, test->plane_id, test->crtc_id,
			      buffer->fb_id, 0,
			      0, 0, test->width, test->height,
			      0, 0, test->width << 16, test->height << 16);
	if (ret) {
		fprintf(stderr,
			"%s: failed to show plane %02u with fb %02u: %s\n",
			__func__, test->plane_id, buffer->fb_id,
			strerror(errno));
		return -errno;
	}

	printf("Showing plane %02u with fb %02u for frame %d\n",
	       test->plane_id, buffer->fb_id, frame);

	return 0;
}

int
main(int argc, char *argv[])
{
	struct test test[1] = {{ 0 }};
	int ret;

	ret = kms_init(test, "sun4i-drm");
	if (ret)
		return ret;

	ret = kms_resources_list(test->kms_fd);
	if (ret)
		return ret;

	ret = kms_plane_get(test);
	if (ret)
		return ret;

	printf("Using Plane %02d attached to Crtc %02d\n",
	       test->plane_id, test->crtc_id);

	test->width = 1280;
	test->height = 720;
	test->bpp = 32;
	test->format = DRM_FORMAT_ARGB8888;

	ret = kms_buffer_get(test, test->buffers[0]);
	if (ret)
		return ret;

	buffer_fill(test, test->buffers[0], 0x0F);

	ret = kms_buffer_get(test, test->buffers[1]);
	if (ret)
		return ret;

	ret = kms_plane_display(test, test->buffers[0], 0x0F);
	if (ret)
		return ret;

	sleep(30);

	return 0;
}
