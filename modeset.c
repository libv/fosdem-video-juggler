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

	float dotclock;
	int hdisplay, hsync_start, hsync_end, htotal;
	int vdisplay, vsync_start, vsync_end, vtotal;
	bool polarity_hsync;
	bool polarity_vsync;
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
modeline_parse(struct kms_modeset *modeset, int argc, char *argv[])
{
	int ret;

	if (argc != 11) {
		fprintf(stderr, "Error: not enough arguments.\n");
		return -1;
	}

	ret = sscanf(argv[0], "%f", &modeset->dotclock);
	if (ret != 1) {
		fprintf(stderr, "Failed to read dotclock from %s.\n",
			argv[0]);
		return -1;
	}

	ret = sscanf(argv[1], "%d", &modeset->hdisplay);
	if (ret != 1) {
		fprintf(stderr, "Failed to read hdisplay from %s.\n",
			argv[1]);
		return -1;
	}

	ret = sscanf(argv[2], "%d", &modeset->hsync_start);
	if (ret != 1) {
		fprintf(stderr, "Failed to read hsync_start from %s.\n",
			argv[2]);
		return -1;
	}

	ret = sscanf(argv[3], "%d", &modeset->hsync_end);
	if (ret != 1) {
		fprintf(stderr, "Failed to read hsync_end from %s.\n",
			argv[3]);
		return -1;
	}

	ret = sscanf(argv[4], "%d", &modeset->htotal);
	if (ret != 1) {
		fprintf(stderr, "Failed to read htotal from %s.\n",
			argv[4]);
		return -1;
	}

	ret = sscanf(argv[5], "%d", &modeset->vdisplay);
	if (ret != 1) {
		fprintf(stderr, "Failed to read vdisplay from %s.\n",
			argv[5]);
		return -1;
	}

	ret = sscanf(argv[6], "%d", &modeset->vsync_start);
	if (ret != 1) {
		fprintf(stderr, "Failed to read vsync_start from %s.\n",
			argv[6]);
		return -1;
	}

	ret = sscanf(argv[7], "%d", &modeset->vsync_end);
	if (ret != 1) {
		fprintf(stderr, "Failed to read vsync_end from %s.\n",
			argv[7]);
		return -1;
	}

	ret = sscanf(argv[8], "%d", &modeset->vtotal);
	if (ret != 1) {
		fprintf(stderr, "Failed to read vtotal from %s.\n",
			argv[8]);
		return -1;
	}

	if (!strcmp(argv[9], "+hsync"))
		modeset->polarity_hsync = true;
	else if (!strcmp(argv[9], "-hsync"))
		modeset->polarity_hsync = false;
	else {
		fprintf(stderr, "Failed to read hsync polarity from %s.\n",
			argv[9]);
		return -1;
	}

	if (!strcmp(argv[10], "+vsync"))
		modeset->polarity_vsync = true;
	else if (!strcmp(argv[10], "-vsync"))
		modeset->polarity_vsync = false;
	else {
		fprintf(stderr, "Failed to read vsync polarity from %s.\n",
			argv[10]);
		return -1;
	}

	printf("Parsed modeline: %2.2f  %d %d %d %d  %d %d %d %d "
	       "%chsync %cvsync\n", modeset->dotclock,
	       modeset->hdisplay, modeset->hsync_start,
	       modeset->hsync_end, modeset->htotal,
	       modeset->vdisplay, modeset->vsync_start,
	       modeset->vsync_end, modeset->vtotal,
	       modeset->polarity_hsync ? '+' : '-',
	       modeset->polarity_vsync ? '+' : '-');

	return 0;
}

static int
modeline_verify(struct kms_modeset *modeset)
{
	float refresh;

	if (modeset->dotclock < 1.0) {
		fprintf(stderr, "Error: clock %2.2f is too low.\n",
			modeset->dotclock);
		return -1;
	}

	if (modeset->dotclock > 500.0) {
		fprintf(stderr, "Error: clock %2.2f is too low.\n",
			modeset->dotclock);
		return -1;
	}

	if ((modeset->hdisplay <= 0) || (modeset->hdisplay > 4096)) {
		fprintf(stderr, "Error: Invalid HDisplay %d\n",
			modeset->hdisplay);
		return -1;
	}

	if ((modeset->hsync_start <= 0) || (modeset->hsync_start > 4096)) {
		fprintf(stderr, "Error: Invalid HSync Start %d\n",
			modeset->hsync_start);
		return -1;
	}

	if ((modeset->hsync_end <= 0) || (modeset->hsync_end > 4096)) {
		fprintf(stderr, "Error: Invalid HSync End %d\n",
			modeset->hsync_end);
		return -1;
	}

	if ((modeset->htotal <= 0) || (modeset->htotal > 4096)) {
		fprintf(stderr, "Error: Invalid HTotal %d\n",
			modeset->htotal);
		return -1;
	}

	if ((modeset->vdisplay <= 0) || (modeset->vdisplay > 4096)) {
		fprintf(stderr, "Error: Invalid VDisplay %d\n",
			modeset->vdisplay);
		return -1;
	}

	if ((modeset->vsync_start <= 0) || (modeset->vsync_start > 4096)) {
		fprintf(stderr, "Error: Invalid VSync Start %d\n",
			modeset->vsync_start);
		return -1;
	}

	if ((modeset->vsync_end <= 0) || (modeset->vsync_end > 4096)) {
		fprintf(stderr, "Error: Invalid VSync End %d\n",
			modeset->vsync_end);
		return -1;
	}

	if ((modeset->vtotal <= 0) || (modeset->vtotal > 4096)) {
		fprintf(stderr, "Error: Invalid VTotal %d\n",
			modeset->vtotal);
		return -1;
	}

	if (modeset->hdisplay > modeset->hsync_start) {
		fprintf(stderr, "Error: HDisplay %d is above HSync Start %d\n",
			modeset->hdisplay, modeset->hsync_start);
		return -1;
	}

	if (modeset->hsync_start > modeset->hsync_end) {
		fprintf(stderr, "Error: HSync Start %d is above HSync End %d\n",
			modeset->hsync_start, modeset->hsync_end);
		return -1;
	}

	if (modeset->hsync_end > modeset->htotal) {
		fprintf(stderr, "Error: HSync End %d is above HTotal %d\n",
			modeset->hsync_end, modeset->htotal);
		return -1;
	}

	if (modeset->vdisplay > modeset->vsync_start) {
		fprintf(stderr, "Error: VDisplay %d is above VSync Start %d\n",
			modeset->vdisplay, modeset->vsync_start);
		return -1;
	}

	if (modeset->vsync_start > modeset->vsync_end) {
		fprintf(stderr, "Error: VSync Start %d is above VSync End %d\n",
			modeset->vsync_start, modeset->vsync_end);
		return -1;
	}

	if (modeset->vsync_end > modeset->vtotal) {
		fprintf(stderr, "Error: VSync End %d is above VTotal %d\n",
			modeset->vsync_end, modeset->vtotal);
		return -1;
	}

	/*
	 * Here we lock down the vertical refresh to around 60Hz, as we
	 * do not want to run our displays too far from 60Hz, even when
	 * playing with the timing.
	 *
	 */
	refresh = (modeset->dotclock * 1000000.0) /
		(modeset->htotal * modeset->vtotal);
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

	ret = modeline_parse(modeset, argc - 1, &argv[1]);
	if (ret) {
		usage(argv[0]);
		return ret;
	}

	ret = modeline_verify(modeset);
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
