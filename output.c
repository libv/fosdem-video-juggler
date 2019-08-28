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
};

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

	while (1)
		sleep(1);

	return 0;
}
