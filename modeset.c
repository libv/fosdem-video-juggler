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

	struct _drmModeModeInfo mode[1];
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

static int
modeline_parse(struct _drmModeModeInfo *mode, int argc, char *argv[])
{
	float dotclock, refresh;
	int ret, val;

	if (argc != 11) {
		fprintf(stderr, "Error: not enough arguments.\n");
		return -1;
	}

	ret = sscanf(argv[0], "%f", &dotclock);
	if (ret != 1) {
		fprintf(stderr, "Failed to read dotclock from %s.\n",
			argv[0]);
		return -1;
	}
	mode->clock = dotclock * 1000;


	ret = sscanf(argv[1], "%d", &val);
	if (ret != 1) {
		fprintf(stderr, "Failed to read hdisplay from %s.\n",
			argv[1]);
		return -1;
	}
	mode->hdisplay = val;

	ret = sscanf(argv[2], "%d", &val);
	if (ret != 1) {
		fprintf(stderr, "Failed to read hsync_start from %s.\n",
			argv[2]);
		return -1;
	}
	mode->hsync_start = val;

	ret = sscanf(argv[3], "%d", &val);
	if (ret != 1) {
		fprintf(stderr, "Failed to read hsync_end from %s.\n",
			argv[3]);
		return -1;
	}
	mode->hsync_end = val;

	ret = sscanf(argv[4], "%d", &val);
	if (ret != 1) {
		fprintf(stderr, "Failed to read htotal from %s.\n",
			argv[4]);
		return -1;
	}
	mode->htotal = val;

	ret = sscanf(argv[5], "%d", &val);
	if (ret != 1) {
		fprintf(stderr, "Failed to read vdisplay from %s.\n",
			argv[5]);
		return -1;
	}
	mode->vdisplay = val;

	ret = sscanf(argv[6], "%d", &val);
	if (ret != 1) {
		fprintf(stderr, "Failed to read vsync_start from %s.\n",
			argv[6]);
		return -1;
	}
	mode->vsync_start = val;

	ret = sscanf(argv[7], "%d", &val);
	if (ret != 1) {
		fprintf(stderr, "Failed to read vsync_end from %s.\n",
			argv[7]);
		return -1;
	}
	mode->vsync_end = val;

	ret = sscanf(argv[8], "%d", &val);
	if (ret != 1) {
		fprintf(stderr, "Failed to read vtotal from %s.\n",
			argv[8]);
		return -1;
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
		return -1;
	}

	mode->flags &= ~0x0C;
	if (!strcmp(argv[10], "+vsync"))
		mode->flags |= DRM_MODE_FLAG_PVSYNC;
	else if (!strcmp(argv[10], "-vsync"))
		mode->flags |= DRM_MODE_FLAG_NVSYNC;
	else {
		fprintf(stderr, "Failed to read vsync polarity from %s.\n",
			argv[10]);
		return -1;
	}

	refresh = (mode->clock * 1000.0) /
		(mode->htotal * mode->vtotal);

	snprintf(mode->name, DRM_DISPLAY_MODE_LEN, "%dx%d@%2.2fHz",
		 mode->hdisplay, mode->vdisplay, refresh);
	mode->vrefresh = refresh;

	return 0;
}

static int
modeline_verify(struct _drmModeModeInfo *mode)
{
	float refresh;

	if (mode->clock < 1000.0) {
		fprintf(stderr, "Error: clock %2.2f is too low.\n",
			mode->clock / 1000.0);
		return -1;
	}

	if (mode->clock > 500000.0) {
		fprintf(stderr, "Error: clock %2.2f is too low.\n",
			mode->clock / 1000.0);
		return -1;
	}

	if ((mode->hdisplay <= 0) || (mode->hdisplay > 4096)) {
		fprintf(stderr, "Error: Invalid HDisplay %d\n",
			mode->hdisplay);
		return -1;
	}

	if ((mode->hsync_start <= 0) || (mode->hsync_start > 4096)) {
		fprintf(stderr, "Error: Invalid HSync Start %d\n",
			mode->hsync_start);
		return -1;
	}

	if ((mode->hsync_end <= 0) || (mode->hsync_end > 4096)) {
		fprintf(stderr, "Error: Invalid HSync End %d\n",
			mode->hsync_end);
		return -1;
	}

	if ((mode->htotal <= 0) || (mode->htotal > 4096)) {
		fprintf(stderr, "Error: Invalid HTotal %d\n",
			mode->htotal);
		return -1;
	}

	if ((mode->vdisplay <= 0) || (mode->vdisplay > 4096)) {
		fprintf(stderr, "Error: Invalid VDisplay %d\n",
			mode->vdisplay);
		return -1;
	}

	if ((mode->vsync_start <= 0) || (mode->vsync_start > 4096)) {
		fprintf(stderr, "Error: Invalid VSync Start %d\n",
			mode->vsync_start);
		return -1;
	}

	if ((mode->vsync_end <= 0) || (mode->vsync_end > 4096)) {
		fprintf(stderr, "Error: Invalid VSync End %d\n",
			mode->vsync_end);
		return -1;
	}

	if ((mode->vtotal <= 0) || (mode->vtotal > 4096)) {
		fprintf(stderr, "Error: Invalid VTotal %d\n",
			mode->vtotal);
		return -1;
	}

	if (mode->hdisplay > mode->hsync_start) {
		fprintf(stderr, "Error: HDisplay %d is above HSync Start %d\n",
			mode->hdisplay, mode->hsync_start);
		return -1;
	}

	if (mode->hsync_start > mode->hsync_end) {
		fprintf(stderr, "Error: HSync Start %d is above HSync End %d\n",
			mode->hsync_start, mode->hsync_end);
		return -1;
	}

	if (mode->hsync_end > mode->htotal) {
		fprintf(stderr, "Error: HSync End %d is above HTotal %d\n",
			mode->hsync_end, mode->htotal);
		return -1;
	}

	if (mode->vdisplay > mode->vsync_start) {
		fprintf(stderr, "Error: VDisplay %d is above VSync Start %d\n",
			mode->vdisplay, mode->vsync_start);
		return -1;
	}

	if (mode->vsync_start > mode->vsync_end) {
		fprintf(stderr, "Error: VSync Start %d is above VSync End %d\n",
			mode->vsync_start, mode->vsync_end);
		return -1;
	}

	if (mode->vsync_end > mode->vtotal) {
		fprintf(stderr, "Error: VSync End %d is above VTotal %d\n",
			mode->vsync_end, mode->vtotal);
		return -1;
	}

	/*
	 * Here we lock down the vertical refresh to around 60Hz, as we
	 * do not want to run our displays too far from 60Hz, even when
	 * playing with the timing.
	 *
	 */
	refresh = (mode->clock * 1000.0) /
		(mode->htotal * mode->vtotal);
	if (refresh < 55.0) {
		fprintf(stderr, "Error: refresh rate too low: %2.2f\n",
			refresh);
		return -1;
	}

	if (refresh > 65.0) {
		fprintf(stderr, "Error: refresh rate too high: %2.2f\n",
			refresh);
		return -1;
	}

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

	ret = modeline_parse(modeset->mode, argc - 1, &argv[1]);
	if (ret) {
		usage(argv[0]);
		return ret;
	}

	ret = modeline_verify(modeset->mode);
	if (ret)
		return ret;

	printf("Mode parsed from the arguments list:\n  ");
	kms_modeline_print(modeset->mode);

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
