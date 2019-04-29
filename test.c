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

#include <xf86drm.h>
#include <xf86drmMode.h>

const char *kms_driver_name = "sun4i-drm";
int kms_fd;

static void
kms_object_properties_print(int fd, int id, uint32_t type)
{
	drmModeObjectProperties *props;
	int i;

	props = drmModeObjectGetProperties(fd, id, type);
	if (!props) {
		/* yes, no properties returns EINVAL */
		if (errno != EINVAL)
			fprintf(stderr,
				"Failed to get object %d properties: %s\n",
				id, strerror(errno));
		return;
	}

	printf("\t\t   Properties:\n");
	for (i = 0; i < (int) props->count_props; i++) {
		drmModePropertyRes *property;

		property = drmModeGetProperty(fd, props->props[i]);
		if (!property) {
			fprintf(stderr,
				"Failed to get object %d property %d: %s\n",
				id, props->props[i], strerror(errno));
			continue;
		}

		printf("\t\t\t%02d: %s\n", property->prop_id, property->name);

		drmModeFreeProperty(property);
	}

	drmModeFreeObjectProperties(props);
}

static void
kms_fb_print(int fd, int id)
{
	drmModeFB *fb;

	fb = drmModeGetFB(fd, id);
	if (!fb) {
		fprintf(stderr, "Failed to get FB %d: %s\n", id,
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
		fprintf(stderr, "Failed to get Plane %d: %s\n", id,
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
kms_crtc_print(int fd, int id)
{
	drmModeCrtc *crtc;

	crtc = drmModeGetCrtc(fd, id);
	if (!crtc) {
		fprintf(stderr, "Failed to get CRTC %d: %s\n", id,
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
kms_encoder_print(int fd, int id)
{
	drmModeEncoder *encoder;

	encoder = drmModeGetEncoder(fd, id);
	if (!encoder) {
		fprintf(stderr, "Failed to get Encoder %d: %s\n", id,
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
kms_connector_print(int fd, int id)
{
	drmModeConnector *connector = drmModeGetConnector(fd, id);
	int i;

	if (!connector) {
		fprintf(stderr, "Failed to get Connector %d: %s\n", id,
			strerror(errno));
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
	drmModePlaneRes *planes;
	int i;

	resources = drmModeGetResources(fd);
	if (!resources) {
		fprintf(stderr, "Failed to get KMS resources: %s\n",
			strerror(errno));
		return -EINVAL;
	}

	planes = drmModeGetPlaneResources(fd);
	if (!planes) {
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
	for (i = 0; i < (int) planes->count_planes; i++)
		kms_plane_print(fd, planes->planes[i]);

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
	drmModeFreePlaneResources(planes);

	return 0;
}

int
main(int argc, char *argv[])
{
	int ret;

	kms_fd = drmOpen(kms_driver_name, NULL);
	if (kms_fd == -1) {
		fprintf(stderr, "Error: Failed to open KMS driver %s: %s\n",
			kms_driver_name, strerror(errno));
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

	ret = kms_resources_list(kms_fd);
	if (ret)
		return ret;

	return 0;
}
