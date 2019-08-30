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
 * Sadly, changes to the mode are not persistent, and get reverted the instant
 * this application finishes.
 */
#include <stdlib.h>
#include <stdbool.h>
#include <inttypes.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

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
};

static void
usage(const char *name)
{
	printf("Usage:\n");
	printf("%s  <dotclock>  "
	       "<hdisplay> <hsync_start> <hsync_end> <htotal>  "
	       "<vdisplay> <vsync_start> <vsync_end> <vtotal> "
	       "[+-]hsync [+-]vsync\n", name);
	printf("The arguments are formated as an xfree86 modeline:\n");
	printf("\t* dotclock is a float for MHz.\n");
	printf("\t* The sync polarities are written out as '+vsync'.\n");
	printf("\t* All other values are pixels positions, as integers.\n");
}

int main(int argc, char *argv[])
{
	struct kms_modeset *modeset;
	struct _drmModeModeInfo *mode, *old;
	int ret;

	ret = kms_init();
	if (ret)
		return ret;

	modeset = calloc(1, sizeof(struct kms_modeset));
	if (!modeset)
		return -ENOMEM;

	mode = kms_modeline_arguments_parse(argc - 1, &argv[1]);
	if (!mode) {
		usage(argv[0]);
		return ret;
	}

	printf("Mode parsed from the arguments list:\n  ");
	kms_modeline_print(mode);

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

	old = kms_crtc_modeline_get(modeset->crtc_id);
	if (!old)
		return -1;

	printf("Current mode:\n  ");
	kms_modeline_print(old);
	free(old);

	ret = kms_crtc_modeline_set(modeset->crtc_id, mode);
	if (ret)
		return ret;

	old = kms_crtc_modeline_get(modeset->crtc_id);
	if (!old)
		return -1;

	printf("New/updated mode:\n  ");
	kms_modeline_print(old);

	return 0;
}
