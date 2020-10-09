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
#include <stdlib.h>
#include <sysexits.h>

#include <xf86drmMode.h>
#include <drm_fourcc.h>

#include "kms.h"

struct output_test {
	struct kms_plane *plane;

	struct kms_buffer *buffers[2];

	int x;
	int y;
	int w;
	int h;
};
#define OUTPUT_TEST_COUNT 5
#define OUTPUT_TEST_WIDTH 16
#define OUTPUT_TEST_HEIGHT 16

struct kms_output {
	bool connected;
	bool mode_ok;

	uint32_t connector_id;
	uint32_t encoder_id;
	uint32_t crtc_id;
	int crtc_width;
	int crtc_height;
	int crtc_index;

	struct kms_plane *plane_background;
	struct kms_buffer *buffer_background;

	struct output_test tests[OUTPUT_TEST_COUNT][1];

	/*
	 * it could be that the primary plane is not used by us, and
	 * should be disabled
	 */
	struct kms_plane *plane_disable;
};

static void
usage(const char *name)
{
	printf("Usage:\n");
	printf("%s\n", name);
	printf("Or:\n");
	printf("%s  <framecount>\n", name);
	printf("Or:\n");
	printf("%s  <framecount>  <dotclock>  "
	       "<hdisplay> <hsync_start> <hsync_end> <htotal>  "
	       "<vdisplay> <vsync_start> <vsync_end> <vtotal> "
	       "[+-]hsync [+-]vsync\n", name);
	printf("The arguments are formated as an xfree86 modeline:\n");
	printf("\t* dotclock is a float for MHz.\n");
	printf("\t* The sync polarities are written out as '+vsync'.\n");
	printf("\t* All other values are pixels positions, as integers.\n");
}

/*
 * Get all the desired planes in one go.
 */
static int
kms_output_planes_get(struct kms_output *output)
{
	drmModePlaneRes *resources_plane = NULL;
	int ret = 0, i, test = 0;

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
			printf("Background Plane: ");
			output->plane_background =
				kms_plane_create(plane->plane_id);
			if (!output->plane_background) {
				ret = -1;
				goto plane_error;
			}
			used = true;
		} else if (!yuv && !layer) {
			if (test < OUTPUT_TEST_COUNT) {
				printf("Test Plane %d: ", test);
				output->tests[test]->plane =
					kms_plane_create(plane->plane_id);
				if (!output->tests[test]->plane) {
					ret = -1;
					goto plane_error;
				}
				test++;
				used = true;
			}
		}

		if (plane->fb_id && !used) {
			if (!output->plane_disable) {
				printf("Disable Plane: ");
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

static void
kms_output_background_set(struct kms_output *output,
			  drmModeAtomicReqPtr request)
{
	struct kms_plane *plane = output->plane_background;
	struct kms_buffer *buffer = output->buffer_background;

	drmModeAtomicAddProperty(request, plane->plane_id,
				 plane->property_crtc_id,
				 output->crtc_id);

	/* Full crtc size */
	drmModeAtomicAddProperty(request, plane->plane_id,
				 plane->property_crtc_x, 0);
	drmModeAtomicAddProperty(request, plane->plane_id,
				 plane->property_crtc_y, 0);
	drmModeAtomicAddProperty(request, plane->plane_id,
				 plane->property_crtc_w,
				 buffer->width);
	drmModeAtomicAddProperty(request, plane->plane_id,
				 plane->property_crtc_h,
				 buffer->height);

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

	/* actual flip. */
	drmModeAtomicAddProperty(request, plane->plane_id,
				 plane->property_fb_id,
				 buffer->fb_id);
}

static void
output_test_buffer_fill(void *buffer, int x, int y, int w, int h, int pitch)
{
	uint8_t *line;
	uint8_t green;
	int i, j;

	line = buffer;
	green = y;
	for (j = 0; j < h; j++) {
		uint32_t *p = (uint32_t *) line;
		uint8_t blue = x;

		for (i = 0; i < w; i++) {
			*p = 0xFF000000 | (green << 8) | blue;
			p++;
			blue++;
		}

		line += pitch;
		green++;
	}

#if 0
	/* some quick self-test */
	uint32_t *p = buffer;
	printf(" 0, 0: 0x%08X\n", p[0]);
	printf("15, 0: 0x%08X\n", p[15]);
	printf(" 0,15: 0x%08X\n", p[15 * 16]);
	printf("15,15: 0x%08X\n", p[15 * 16 + 15]);
#endif
}

static int
output_test_init(struct output_test *test, int x, int y, int w, int h)
{
	printf("%s(plane 0x%02X) = %4dx%4d (%4dx%4d)\n", __func__,
	       test->plane->plane_id, x, y, w, h);

	test->x = x;
	test->y = y;
	test->w = w;
	test->h = h;

	test->buffers[0] = kms_buffer_get(w, h, DRM_FORMAT_ARGB8888);
	if (!test->buffers[0])
		return -1;

	test->buffers[1] = kms_buffer_get(w, h, DRM_FORMAT_ARGB8888);
	if (!test->buffers[1])
		return -1;

	output_test_buffer_fill(test->buffers[0]->map, x, y, w, h,
				test->buffers[0]->pitch);

	memcpy(test->buffers[1]->map, test->buffers[0]->map,
	       test->buffers[0]->size);

	return 0;
}

static int
kms_output_tests_init(struct kms_output *output)
{
	int w = OUTPUT_TEST_WIDTH;
	int h = OUTPUT_TEST_HEIGHT;
	int right = output->crtc_width - w;
	int bottom = output->crtc_height - h;
	int middle_x = (output->crtc_width - w) / 2;
	int middle_y = (output->crtc_height - h) / 2;
	int ret;

	/* Top left. */
	ret = output_test_init(output->tests[0], 0, 0, w, h);
	if (ret)
		return ret;

	/* Top right. */
	ret = output_test_init(output->tests[1], right, 0, w, h);
	if (ret)
		return ret;

	/* Middle. */
	ret = output_test_init(output->tests[2], middle_x, middle_y, w, h);
	if (ret)
		return ret;

	/* Bottom left. */
	ret = output_test_init(output->tests[3], 0, bottom, w, h);
	if (ret)
		return ret;

	/* Bottom right */
	ret = output_test_init(output->tests[4], right, bottom, w, h);
	if (ret)
		return ret;

	return 0;
}

static void
output_test_frame_update(struct output_test *test, int frame)
{
	struct kms_buffer *buffer = test->buffers[frame & 0x01];
	uint8_t *line;
	uint8_t red = frame;
	int i, j;

	line = buffer->map + 2;
	for (j = 0; j < test->h; j++) {
		uint8_t *p = line;

		for (i = 0; i < test->w; i++) {
			*p = red;
			p += 4;
		}

		line += buffer->pitch;
	}
}

static void
output_test_frame_set(struct kms_output *output, struct output_test *test,
		      drmModeAtomicReqPtr request, int frame)
{
	struct kms_plane *plane = test->plane;
	struct kms_buffer *buffer = test->buffers[frame & 0x01];

	if (!plane->active) {
		printf("test: 0x%02X (%dx%d) -> %4dx%4d (%dx%d), "
		       "plane 0x%02X, crtc 0x%02X\n",
		       buffer->fb_id, buffer->width, buffer->height,
		       test->x, test->y, test->w, test->h,
		       test->plane->plane_id, output->crtc_id);

		drmModeAtomicAddProperty(request, plane->plane_id,
					 plane->property_crtc_id,
					 output->crtc_id);

		drmModeAtomicAddProperty(request, plane->plane_id,
					 plane->property_crtc_x,
					 test->x);
		drmModeAtomicAddProperty(request, plane->plane_id,
					 plane->property_crtc_y,
					 test->y);
		drmModeAtomicAddProperty(request, plane->plane_id,
					 plane->property_crtc_w,
					 test->w);
		drmModeAtomicAddProperty(request, plane->plane_id,
					 plane->property_crtc_h,
					 test->h);

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

		/*
		 * we are using sprites, so zpos needs to be between 4 and
		 * 36 (if only kms supported that many planes).
		 * Instead of trying to be smart here, just keep the default.
		 */

		plane->active = true;
	}

	/* actual flip. */
	drmModeAtomicAddProperty(request, plane->plane_id,
				 plane->property_fb_id,
				 buffer->fb_id);
}

int main(int argc, char *argv[])
{
	struct kms_output *output;
	struct _drmModeModeInfo *mode = NULL, *mode_old;
	unsigned long count = 1000;
	int ret, i, j;

	if ((argc != 1) && (argc != 2) && (argc != 13)) {
		usage(argv[0]);
		return EX_USAGE;
	}

	if (argc > 1) {
		ret = sscanf(argv[1], "%lu", &count);
		if (ret != 1) {
			fprintf(stderr, "%s: failed to fscanf(%s): %s\n",
				__func__, argv[1], strerror(errno));
			usage(argv[0]);
			return EX_USAGE;
		}

		if (count < 0)
			count = 1000;
	}
	printf("Running for %lu frames.\n", count);

	if (argc > 2) {
		mode = kms_modeline_arguments_parse(argc - 2, &argv[2]);
		if (!mode) {
			usage(argv[0]);
			return EX_USAGE;
		}

		printf("Mode parsed from the arguments list:\n  ");
		kms_modeline_print(mode);
	}

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

	printf("Using CRTC %X (%dx%d), connector %X (%s).\n",
	       output->crtc_id, output->crtc_width, output->crtc_height,
	       output->connector_id,
	       kms_connector_string(DRM_MODE_CONNECTOR_HDMIA));

	mode_old = kms_crtc_modeline_get(output->crtc_id);
	if (!mode_old)
		return -1;
	printf("Current mode:\n  ");
	kms_modeline_print(mode_old);
	free(mode_old);

	if (mode) {
		ret = kms_crtc_modeline_set(output->crtc_id, mode);
		if (ret)
			return ret;

		mode_old = kms_crtc_modeline_get(output->crtc_id);
		if (!mode_old)
			return -1;

		printf("New/updated mode:\n  ");
		kms_modeline_print(mode_old);
		free(mode_old);

		printf("Waiting for monitor to catch up with the new mode...");
		sleep(2);
		printf(" Done.\n");
	}

	ret = kms_output_planes_get(output);
	if (ret)
		return ret;

	output->buffer_background =
		kms_png_read("PM5644_test_card_FOSDEM.1280x720.png");
	if (!output->buffer_background)
		return -1;

	ret = kms_output_tests_init(output);
	if (ret)
		return ret;

	for (i = 0 ; i < count; i++) {
		drmModeAtomicReqPtr request;

		printf("\rShowing frame %8d/%ld,", i, count);

		request = drmModeAtomicAlloc();

		if (!output->plane_background->active)
			kms_output_background_set(output, request);

		if (output->plane_disable && output->plane_disable->active)
			kms_plane_disable(output->plane_disable, request);

		for (j = 0; j < OUTPUT_TEST_COUNT; j++) {
			output_test_frame_update(output->tests[j], i);
			output_test_frame_set(output, output->tests[j],
					      request, i);
		}

		ret = drmModeAtomicCommit(kms_fd, request,
					  DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);

		drmModeAtomicFree(request);

		if (ret) {
			fprintf(stderr, "%s: failed to show frame %d: %s\n",
				__func__, i, strerror(errno));
			return ret;
		}
	}

	printf("\nDone!\n");

	return 0;
}
