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

/*
 * Tool that shows a fullscreen trackable, testable image on hdmi out,
 * allowing us to test the integrity of our capture setup at the other end.
 */

#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <errno.h>
#include <unistd.h>
#include <stdbool.h>

#include <xf86drmMode.h>
#include <drm_fourcc.h>

#include "kms.h"

struct kms_output {
	bool connected;
	bool mode_ok;

	uint32_t connector_id;
	uint32_t encoder_id;
	uint32_t crtc_id;
	int crtc_width;
	int crtc_height;
	int crtc_index;

	struct kms_plane *plane_scaling;

	struct kms_plane *plane_topleft;
	struct kms_plane *plane_topright;
	struct kms_plane *plane_middle;
	struct kms_plane *plane_bottomleft;
	struct kms_plane *plane_bottomright;

	/*
	 * it could be that the primary plane is not used by us, and
	 * should be disabled
	 */
	struct kms_plane *plane_disable;
};

/*
 * Get all the desired planes in one go.
 */
static int
kms_output_planes_get(struct kms_output *output)
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

		if (!(plane->possible_crtcs & (1 << output->crtc_index)))
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
			output->plane_scaling =
				kms_plane_create(plane->plane_id);
			if (!output->plane_scaling) {
				ret = -1;
				goto plane_error;
			}
			used = true;
		} else if (!yuv && !layer) {
			if (!output->plane_topleft) {
				output->plane_topleft =
					kms_plane_create(plane->plane_id);
				if (!output->plane_topleft) {
					ret = -1;
					goto plane_error;
				}
				used = true;
			} else if (!output->plane_topright) {
				output->plane_topright =
					kms_plane_create(plane->plane_id);
				if (!output->plane_topright) {
					ret = -1;
					goto plane_error;
				}
				used = true;
			} else if (!output->plane_middle) {
				output->plane_middle =
					kms_plane_create(plane->plane_id);
				if (!output->plane_middle) {
					ret = -1;
					goto plane_error;
				}
				used = true;
			} else if (!output->plane_bottomleft) {
				output->plane_bottomleft =
					kms_plane_create(plane->plane_id);
				if (!output->plane_bottomleft) {
					ret = -1;
					goto plane_error;
				}
				used = true;
			} else if (!output->plane_bottomright) {
				output->plane_bottomright =
					kms_plane_create(plane->plane_id);
				if (!output->plane_bottomright) {
					ret = -1;
					goto plane_error;
				}
				used = true;
			}
		}

		if (plane->fb_id && !used) {
			if (!output->plane_disable) {
				output->plane_disable =
					kms_plane_create(plane->plane_id);
				/* if this fails, continue */
			} else
				fprintf(stderr, "%s: multiple planes need to "
					"be disabled (%d, %d)!\n", __func__,
					output->plane_disable->plane_id,
					plane->plane_id);
		}

	plane_next:
		drmModeFreePlane(plane);
		continue;
	plane_error:
		drmModeFreePlane(plane);
		break;
	}

	if (output->plane_disable)
		output->plane_disable->active = true;

 error:
	drmModeFreePlaneResources(resources_plane);
	return ret;
}

int main(int argc, char *argv[])
{
	struct kms_output *output;
	unsigned long count = 1000;
	int ret;

	if (argc > 1) {
		ret = sscanf(argv[1], "%lu", &count);
		if (ret != 1) {
			fprintf(stderr, "%s: failed to fscanf(%s): %s\n",
				__func__, argv[1], strerror(errno));
			return -1;
		}

		if (count < 0)
			count = 1000;
	}

	printf("Running for %lu frames.\n", count);

	ret = kms_init();
	if (ret)
		return ret;

	output = calloc(1, sizeof(struct kms_output));
	if (!output)
		return -ENOMEM;

	ret = kms_connector_id_get(DRM_MODE_CONNECTOR_HDMIA,
				   &output->connector_id);
	if (ret)
		return ret;

	ret = kms_connection_check(output->connector_id,
				   &output->connected, &output->encoder_id);
	if (ret)
		return ret;

	ret = kms_crtc_id_get(output->encoder_id,
			      &output->crtc_id, &output->mode_ok,
			      &output->crtc_width, &output->crtc_height);
	if (ret)
		return ret;

	ret = kms_crtc_index_get(output->crtc_id);
	if (ret < 0)
		return ret;

	output->crtc_index = ret;

	ret = kms_output_planes_get(output);
	if (ret)
		return ret;

	while (1)
		sleep(1);

	return 0;
}
