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
 *
 * Tool to adjust and fine-tune a mode on the secondary CRTC.
 *
 */
#include <stdlib.h>
#include <stdbool.h>
#include <inttypes.h>
#include <errno.h>
#include <stdio.h>

#include <xf86drmMode.h>

#include "kms.h"

struct kms_modeset {
	bool connected;
	bool mode_ok;

	uint32_t connector_id;
	uint32_t encoder_id;
	uint32_t crtc_id;
	int crtc_width;
	int crtc_height;

	float dotclock;
	int hdisplay, hsync_start, hsync_end, htotal;
	int vdisplay, vsync_start, vsync_end, vtotal;
};

static int
modeline_parse(struct kms_modeset *modeset, int argc, char *argv[])
{
	int ret;

	if (argc != 10) {
		fprintf(stderr, "Error: not enough arguments.\n");
		printf("Usage:\n");
		printf("%s dotclock hdisplay hsync_start hsync_end htotal "
		       "vdisplay vsync_start vsync_end vtotal\n", argv[0]);
		return -1;
	}

	ret = sscanf(argv[1], "%f", &modeset->dotclock);
	if (ret != 1) {
		fprintf(stderr, "Failed to read dotclock from %s.\n",
			argv[1]);
		return -1;
	}

	ret = sscanf(argv[2], "%d", &modeset->hdisplay);
	if (ret != 1) {
		fprintf(stderr, "Failed to read hdisplay from %s.\n",
			argv[2]);
		return -1;
	}

	ret = sscanf(argv[3], "%d", &modeset->hsync_start);
	if (ret != 1) {
		fprintf(stderr, "Failed to read hsync_start from %s.\n",
			argv[3]);
		return -1;
	}

	ret = sscanf(argv[4], "%d", &modeset->hsync_end);
	if (ret != 1) {
		fprintf(stderr, "Failed to read hsync_end from %s.\n",
			argv[4]);
		return -1;
	}

	ret = sscanf(argv[5], "%d", &modeset->htotal);
	if (ret != 1) {
		fprintf(stderr, "Failed to read htotal from %s.\n",
			argv[5]);
		return -1;
	}

	ret = sscanf(argv[6], "%d", &modeset->vdisplay);
	if (ret != 1) {
		fprintf(stderr, "Failed to read vdisplay from %s.\n",
			argv[6]);
		return -1;
	}

	ret = sscanf(argv[7], "%d", &modeset->vsync_start);
	if (ret != 1) {
		fprintf(stderr, "Failed to read vsync_start from %s.\n",
			argv[7]);
		return -1;
	}

	ret = sscanf(argv[8], "%d", &modeset->vsync_end);
	if (ret != 1) {
		fprintf(stderr, "Failed to read vsync_end from %s.\n",
			argv[8]);
		return -1;
	}

	ret = sscanf(argv[9], "%d", &modeset->vtotal);
	if (ret != 1) {
		fprintf(stderr, "Failed to read vtotal from %s.\n",
			argv[9]);
		return -1;
	}

	printf("Modeline: %2.2f  %d %d %d %d  %d %d %d %d\n",
	       modeset->dotclock,
	       modeset->hdisplay, modeset->hsync_start,
	       modeset->hsync_end, modeset->htotal,
	       modeset->vdisplay, modeset->vsync_start,
	       modeset->vsync_end, modeset->vtotal);

	return 0;
}

int main(int argc, char *argv[])
{
	struct kms_modeset *modeset;
	int ret;

	ret = kms_init();
	if (ret)
		return ret;

	modeset = calloc(1, sizeof(struct kms_modeset));
	if (!modeset)
		return -ENOMEM;

	ret = modeline_parse(modeset, argc, argv);
	if (ret)
		return ret;

	ret = kms_connector_id_get(DRM_MODE_CONNECTOR_HDMIA,
				   &modeset->connector_id);
	if (ret)
		return ret;

	ret = kms_connection_check(modeset->connector_id,
				   &modeset->connected, &modeset->encoder_id);
	if (ret)
		return ret;

	ret = kms_crtc_id_get(modeset->encoder_id,
			      &modeset->crtc_id, &modeset->mode_ok,
			      &modeset->crtc_width, &modeset->crtc_height);
	if (ret)
		return ret;

	return 0;
}
