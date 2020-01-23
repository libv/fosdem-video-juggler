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
 * This file has all the code supporting the projector, currently on HDMI,
 * but in future we might also get VGA.
 *
 */

#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

#include <pthread.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>

#include "juggler.h"
#include "kms.h"
#include "projector.h"
#include "capture.h"

static pthread_t kms_projector_thread[1];

struct kms_projector {
	bool connected;
	bool mode_ok;

	uint32_t connector_id;
	uint32_t encoder_id;
	uint32_t crtc_id;
	int crtc_width;
	int crtc_height;
	int crtc_index;

	struct kms_plane *capture_scaling;
	struct kms_plane *capture_yuv;

	/*
	 * it could be that the primary plane is not used by us, and
	 * should be disabled
	 */
	struct kms_plane *plane_disable;

	pthread_mutex_t capture_buffer_mutex[1];
	/*
	 * This is the buffer that is currently being shown. It will be
	 * released as soon as the next buffer is shown using atomic modeset
	 * commit.
	 */
	struct capture_buffer *capture_buffer_current;
	/*
	 * This is the buffer that our blocking atomic modeset is about to
	 * show.
	 */
	struct capture_buffer *capture_buffer_next;
	/*
	 * This is the upcoming buffer that was last queued by capture.
	 */
	struct capture_buffer *capture_buffer_new;

	/*
	 * Count the number of frames not updated, so we can implement
	 * a poor mans "No signal".
	 */
	uint32_t capture_stall_count;
};
static struct kms_projector *kms_projector;

/*
 * Get all the desired planes in one go.
 */
static int
kms_projector_planes_get(struct kms_projector *projector)
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
		bool frontend = false, yuv = false, used = false;
		int j;

		plane = drmModeGetPlane(kms_fd, plane_id);
		if (!plane) {
			fprintf(stderr, "%s: failed to get Plane %u: %s\n",
				__func__, plane_id, strerror(errno));
			ret = 0;
			goto error;
		}

		if (!(plane->possible_crtcs & (1 << projector->crtc_index)))
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
			default:
				break;
			}
		}

		if (frontend) {
			projector->capture_scaling =
				kms_plane_create(plane->plane_id);
			if (!projector->capture_scaling) {
				ret = -1;
				goto plane_error;
			}
			used = true;
		} else if (yuv) {
			projector->capture_yuv =
				kms_plane_create(plane->plane_id);
			if (!projector->capture_yuv) {
				ret = -1;
				goto plane_error;
			}
			used = true;
		}

		if (plane->fb_id && !used) {
			if (!projector->plane_disable) {
				projector->plane_disable =
					kms_plane_create(plane->plane_id);
				/* if this fails, continue */
			} else
				fprintf(stderr, "%s: multiple planes need to "
					"be disabled (%d, %d)!\n", __func__,
					projector->plane_disable->plane_id,
					plane->plane_id);
		}

	plane_next:
		drmModeFreePlane(plane);
		continue;
	plane_error:
		drmModeFreePlane(plane);
		break;
	}

	if (projector->plane_disable)
		projector->plane_disable->active = true;

 error:
	drmModeFreePlaneResources(resources_plane);
	return ret;
}

/*
 * Show input buffer on projector, scaled, with borders.
 */
static void
kms_projector_capture_set(struct kms_projector *projector,
			  struct capture_buffer *buffer,
			  drmModeAtomicReqPtr request)
{
	struct kms_plane *plane = projector->capture_scaling;

	if (!buffer)
		return;

	if (!plane->active) {
		int x, y, w, h;

		drmModeAtomicAddProperty(request, plane->plane_id,
					 plane->property_crtc_id,
					 projector->crtc_id);

		/* Scale, with borders, and center */
		if ((buffer->width == projector->crtc_width) &&
		    (buffer->height == projector->crtc_height)) {
			x = 0;
			y = 0;
			w = projector->crtc_width;
			h = projector->crtc_height;
		} else {
			/* first, try to fit horizontally. */
			w = projector->crtc_width;
			h = buffer->height * projector->crtc_width /
				buffer->width;

			/* if height does not fit, inverse the logic */
			if (h > projector->crtc_height) {
				h = projector->crtc_height;
				w = buffer->width * projector->crtc_height /
					buffer->height;
			}

			/* center */
			x = (projector->crtc_width - w) / 2;
			y = (projector->crtc_height -h) / 2;
		}

		drmModeAtomicAddProperty(request, plane->plane_id,
					 plane->property_crtc_x, x);
		drmModeAtomicAddProperty(request, plane->plane_id,
					 plane->property_crtc_y, y);
		drmModeAtomicAddProperty(request, plane->plane_id,
					 plane->property_crtc_w, w);
		drmModeAtomicAddProperty(request, plane->plane_id,
					 plane->property_crtc_h, h);

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
	}

	/* actual flip. */
	drmModeAtomicAddProperty(request, plane->plane_id,
				 plane->property_fb_id,
				 buffer->kms_fb_id);
}

static int
kms_projector_frame_update(struct kms_projector *projector,
			   struct capture_buffer *buffer, int frame)
{
	drmModeAtomicReqPtr request;
	int ret;

	request = drmModeAtomicAlloc();

	kms_projector_capture_set(projector, buffer, request);

	if (projector->plane_disable && projector->plane_disable->active)
		kms_plane_disable(projector->plane_disable, request);

	ret = drmModeAtomicCommit(kms_fd, request,
				  DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);

	drmModeAtomicFree(request);

	if (ret) {
		fprintf(stderr, "%s: failed to show frame %d: %s\n",
			__func__, frame, strerror(errno));
		ret = -errno;
	}

	return ret;
}

static void *
kms_projector_thread_handler(void *arg)
{
	struct kms_projector *projector = (struct kms_projector *) arg;
	int ret, i;

	for (i = 0; true; i++) {
		struct capture_buffer *new, *old;

		pthread_mutex_lock(projector->capture_buffer_mutex);

		new = projector->capture_buffer_new;
		projector->capture_buffer_new = NULL;

		pthread_mutex_unlock(projector->capture_buffer_mutex);

		if (new) {
			ret = kms_projector_frame_update(projector, new, i);
			if (ret)
				return NULL;

			old = projector->capture_buffer_current;
			projector->capture_buffer_current = new;

			if (old)
				capture_buffer_display_release(old);

			if (projector->capture_stall_count) {
				if (projector->capture_stall_count > 2)
					printf("Projector: Capture stalled for"
					       " %d frames.\n",
					       projector->capture_stall_count);
				projector->capture_stall_count = 0;
			}
		} else {
			projector->capture_stall_count++;
			if (projector->capture_stall_count == 5) {
				printf("Projector: No input!\n");
			}
			usleep(16667);
		}
	}

	printf("%s: done!\n", __func__);

	return NULL;
}

void
kms_projector_capture_display(struct capture_buffer *buffer)
{
	struct kms_projector *projector = kms_projector;
	struct capture_buffer *old;

	if (!projector) {
		capture_buffer_display_release(buffer);
		return;
	}

	pthread_mutex_lock(projector->capture_buffer_mutex);

	old = projector->capture_buffer_new;
	projector->capture_buffer_new = buffer;

	pthread_mutex_unlock(projector->capture_buffer_mutex);

	if (old)
		capture_buffer_display_release(old);
}

int
kms_projector_init(void)
{
	struct kms_projector *projector;
	int ret;

	projector = calloc(1, sizeof(struct kms_projector));
	if (!projector)
		return -ENOMEM;

	kms_projector = projector;
	pthread_mutex_init(projector->capture_buffer_mutex, NULL);

	ret = kms_connector_id_get(DRM_MODE_CONNECTOR_HDMIA,
				   &projector->connector_id);
	if (ret)
		return ret;

	ret = kms_connection_check(projector->connector_id,
				   &projector->connected,
				   &projector->encoder_id);
	if (ret)
		return ret;

	ret = kms_crtc_id_get(projector->encoder_id,
			      &projector->crtc_id, &projector->mode_ok,
			      &projector->crtc_width, &projector->crtc_height);
	if (ret)
		return ret;

	ret = kms_crtc_index_get(projector->crtc_id);
	if (ret < 0)
		return ret;

	projector->crtc_index = ret;

	printf("Projector is CRTC %d, %4dx%4d\n", projector->crtc_index,
	       projector->crtc_width, projector->crtc_height);

	ret = kms_projector_planes_get(projector);
	if (ret)
		return ret;

	ret = pthread_create(kms_projector_thread, NULL,
			     kms_projector_thread_handler,
			     (void *) kms_projector);
	if (ret) {
		fprintf(stderr, "%s() projector thread creation failed: %s\n",
			__func__, strerror(ret));
		return ret;
	}

	return 0;
}
