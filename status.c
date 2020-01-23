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
#include "status.h"
#include "capture.h"

static pthread_t kms_status_thread[1];

struct kms_status {
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

	struct kms_plane *text;
	struct kms_buffer *text_buffer;

	struct kms_plane *logo;
	struct kms_buffer *logo_buffer;

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
static struct kms_status *kms_status;

/*
 * Get all the desired planes in one go.
 */
static int
kms_status_planes_get(struct kms_status *status)
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

		if (!(plane->possible_crtcs & (1 << status->crtc_index)))
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
			status->capture_scaling =
				kms_plane_create(plane->plane_id);
			if (!status->capture_scaling) {
				ret = -1;
				goto plane_error;
			}
			used = true;
		} else if (yuv) {
			status->capture_yuv =
				kms_plane_create(plane->plane_id);
			if (!status->capture_yuv) {
				ret = -1;
				goto plane_error;
			}
			used = true;
		} else if (!layer) {
			if (!status->text) {
				status->text =
					kms_plane_create(plane->plane_id);
				if (!status->text) {
					ret = -1;
					goto plane_error;
				}
				used = true;
			} else if (!status->logo) {
				status->logo =
					kms_plane_create(plane->plane_id);
				if (!status->logo) {
					ret = -1;
					goto plane_error;
				}
				used = true;
			}
		}

		if (plane->fb_id && !used) {
			if (!status->plane_disable) {
				status->plane_disable =
					kms_plane_create(plane->plane_id);
				/* if this fails, continue */
			} else
				fprintf(stderr, "%s: multiple planes need to "
					"be disabled (%d, %d)!\n", __func__,
					status->plane_disable->plane_id,
					plane->plane_id);
		}

	plane_next:
		drmModeFreePlane(plane);
		continue;
	plane_error:
		drmModeFreePlane(plane);
		break;
	}

	if (status->plane_disable)
		status->plane_disable->active = true;

 error:
	drmModeFreePlaneResources(resources_plane);
	return ret;
}

/*
 * Show input buffer on the status lcd, in the top right corner.
 */
static void
kms_status_capture_set(struct kms_status *status,
		       struct capture_buffer *buffer,
		       drmModeAtomicReqPtr request)
{
	struct kms_plane *plane = status->capture_scaling;

	if (!buffer)
		return;

	if (!plane->active) {
		int x, y, w, h;

		drmModeAtomicAddProperty(request, plane->plane_id,
					 plane->property_crtc_id,
					 status->crtc_id);

#if 0
		/* top right corner */
		x = status->crtc_width / 2;
		y = 0;
		w = status->crtc_width / 2;
		h = buffer->height * w / buffer->width;
#else
				/* Scale, with borders, and center */
		if ((buffer->width == status->crtc_width) &&
		    (buffer->height == status->crtc_height)) {
			x = 0;
			y = 0;
			w = status->crtc_width;
			h = status->crtc_height;
		} else {
			/* first, try to fit horizontally. */
			w = status->crtc_width;
			h = buffer->height * status->crtc_width /
				buffer->width;

			/* if height does not fit, inverse the logic */
			if (h > status->crtc_height) {
				h = status->crtc_height;
				w = buffer->width * status->crtc_height /
					buffer->height;
			}

			/* center */
			x = (status->crtc_width - w) / 2;
			y = (status->crtc_height -h) / 2;
		}
#endif

		printf("%s(): %4dx%4d -> %4dx%4d\n", __func__, x, y, w, h);

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
		drmModeAtomicAddProperty(request, plane->plane_id,
					 plane->property_alpha,
					 0x4000);

		plane->active = true;
	}

	/* actual flip. */
	drmModeAtomicAddProperty(request, plane->plane_id,
				 plane->property_fb_id,
				 buffer->kms_fb_id);
}

/*
 * Show input buffer on the status lcd, in the top right corner.
 */
static void
kms_status_capture_disable(struct kms_status *status,
			   drmModeAtomicReqPtr request)
{
	struct kms_plane *plane = status->capture_scaling;

	if (plane->active) {
		drmModeAtomicAddProperty(request, plane->plane_id,
					 plane->property_fb_id, 0);
		plane->active = false;
	}
}

/*
 * Show status text on bottom of the status lcd.
 */
static void
kms_status_text_set(struct kms_status *status, drmModeAtomicReqPtr request)
{
	struct kms_plane *plane = status->text;
	struct kms_buffer *buffer = status->text_buffer;

	if (!plane->active) {
		int x, y, w, h;

		drmModeAtomicAddProperty(request, plane->plane_id,
					 plane->property_crtc_id,
					 status->crtc_id);

		/* bottom, with a bit of space remaining */
		x = 8;
		y = status->crtc_height - 8 - buffer->height;
		w = buffer->width;
		h = buffer->height;

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
				 buffer->fb_id);
}

/*
 * Show logo on the top right of status lcd.
 */
static void
kms_status_logo_set(struct kms_status *status, drmModeAtomicReqPtr request)
{
	struct kms_plane *plane = status->logo;
	struct kms_buffer *buffer = status->logo_buffer;

	if (!plane->active) {
		int x, y, w, h;

		drmModeAtomicAddProperty(request, plane->plane_id,
					 plane->property_crtc_id,
					 status->crtc_id);

		/* top right, with a bit of space remaining */
		x = status->crtc_width - 8 - buffer->width;
		y = 8;
		w = buffer->width;
		h = buffer->height;

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

		drmModeAtomicAddProperty(request, plane->plane_id,
					 plane->property_zpos,
					 4);

		plane->active = true;
	}

	/* actual flip. */
	drmModeAtomicAddProperty(request, plane->plane_id,
				 plane->property_fb_id,
				 buffer->fb_id);
}

static int
kms_status_frame_update(struct kms_status *status,
			struct capture_buffer *buffer, int frame)
{
	drmModeAtomicReqPtr request;
	int ret;

	request = drmModeAtomicAlloc();

	kms_status_capture_set(status, buffer, request);
	kms_status_text_set(status, request);
	kms_status_logo_set(status, request);

	if (status->plane_disable && status->plane_disable->active)
		kms_plane_disable(status->plane_disable, request);

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

static int
kms_status_frame_noinput(struct kms_status *status, int frame)
{
	drmModeAtomicReqPtr request;
	int ret;

	request = drmModeAtomicAlloc();

	kms_plane_disable(status->capture_scaling, request);
	kms_status_text_set(status, request);
	kms_status_logo_set(status, request);

	if (status->plane_disable && status->plane_disable->active)
		kms_plane_disable(status->plane_disable, request);

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
kms_status_thread_handler(void *arg)
{
	struct kms_status *status = (struct kms_status *) arg;
	int ret, i;

	for (i = 0; true; i++) {
		struct capture_buffer *new, *old;

		pthread_mutex_lock(status->capture_buffer_mutex);

		new = status->capture_buffer_new;
		status->capture_buffer_new = NULL;

		pthread_mutex_unlock(status->capture_buffer_mutex);

		if (new) {
			ret = kms_status_frame_update(status, new, i);
			if (ret)
				return NULL;

			old = status->capture_buffer_current;
			status->capture_buffer_current = new;

			if (old)
				capture_buffer_display_release(old);

			if (status->capture_stall_count) {
				if (status->capture_stall_count > 2)
					printf("Status: Capture stalled for"
					       " %d frames.\n",
					       status->capture_stall_count);
				status->capture_stall_count = 0;
			}
		} else {
			status->capture_stall_count++;
			if (status->capture_stall_count == 5) {
				printf("Status: No input!\n");

				ret = kms_status_frame_noinput(status, i);
				if (ret)
					return NULL;

				old = status->capture_buffer_current;
				status->capture_buffer_current = NULL;

				if (old)
					capture_buffer_display_release(old);
			}

			usleep(16667);
		}
	}

	printf("%s: done!\n", __func__);

	return NULL;
}

void
kms_status_capture_display(struct capture_buffer *buffer)
{
	struct kms_status *status = kms_status;
	struct capture_buffer *old;

	if (!status) {
		capture_buffer_display_release(buffer);
		return;
	}

	pthread_mutex_lock(status->capture_buffer_mutex);

	old = status->capture_buffer_new;

	status->capture_buffer_new = buffer;

	pthread_mutex_unlock(status->capture_buffer_mutex);

	if (old)
		capture_buffer_display_release(old);
}

int
kms_status_init(void)
{
	struct kms_status *status;
	int ret;

	status = calloc(1, sizeof(struct kms_status));
	if (!status)
		return -ENOMEM;
	kms_status = status;

	pthread_mutex_init(status->capture_buffer_mutex, NULL);

	ret = kms_connector_id_get(DRM_MODE_CONNECTOR_DPI,
				   &status->connector_id);
	if (ret)
		return ret;

	ret = kms_connection_check(status->connector_id,
				   &status->connected, &status->encoder_id);
	if (ret)
		return ret;

	ret = kms_crtc_id_get(status->encoder_id,
			      &status->crtc_id, &status->mode_ok,
			      &status->crtc_width, &status->crtc_height);
	if (ret)
		return ret;

	ret = kms_crtc_index_get(status->crtc_id);
	if (ret < 0)
		return ret;

	status->crtc_index = ret;

	ret = kms_status_planes_get(status);
	if (ret)
		return ret;

	status->text_buffer = kms_png_read("status_text.png");
	if (!status->text_buffer)
		return -1;

	status->logo_buffer = kms_png_read("fosdem_logo.png");
	if (!status->logo_buffer)
		return -1;

	ret = pthread_create(kms_status_thread, NULL,
			     kms_status_thread_handler,
			     (void *) kms_status);
	if (ret) {
		fprintf(stderr, "%s() status thread creation failed: %s\n",
			__func__, strerror(ret));
		return ret;
	}

	return 0;
}
