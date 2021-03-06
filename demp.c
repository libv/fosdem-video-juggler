/*
 * Copyright (c) 2019-2020 Luc Verhaegen <libv@skynet.be>
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
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <stdbool.h>
#include <inttypes.h>
#include <stdlib.h>
#include <sysexits.h>
#include <sys/mman.h>

#include <png.h>

#include <linux/videodev2.h>

#if defined(__LINUX_VIDEODEV2_H) && !defined(V4L2_PIX_FMT_R8_G8_B8)
/*
 * This definition might be out of sync with v4l2 though.
 */
#warning "V4L2_PIX_FMT_R8_G8_B8 undefined. Working around it."
#define V4L2_PIX_FMT_R8_G8_B8 v4l2_fourcc('P', 'R', 'G', 'B') /* 24bit planar RGB */
#endif

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>

#include "kms.h"

#define DRIVER_NAME "sun4i_demp"

static int demp_fd;

static struct demp_buffer {
	int width;
	int height;

	uint32_t *png_rgba;

	struct {
		uint8_t *map;
		size_t size;
	} inputs[3];
	int input_count;

	struct {
		uint8_t *map;
		size_t size;
		int export_fd;
		uint32_t prime_handle;
	} outputs[3];
	int output_count;
} demp_buffer[1];

static int
demp_device_open_and_verify(int number)
{
	struct v4l2_capability capability[1] = {{{ 0 }}};
	char filename[128] = "";
	bool has_prgb = false, has_nv12 = false;
	int fd, ret, i;

	ret = snprintf(filename, sizeof(filename), "/dev/video%d", number);
	if (ret < 0) {
		fprintf(stderr, "Error: %s(%d):snprintf() failed: %s",
			__func__, number, strerror(-ret));
		return ret;
	}

	fd = open(filename, O_RDWR);
	if (fd < 0) {
		if ((errno == ENODEV) || (errno == ENOENT)) {
			return 0; /* next! */
		} else {
			fprintf(stderr, "Error: %s():open(%s): %s\n",
				__func__, filename, strerror(errno));
			return fd;
		}
	}

	ret = ioctl(fd, VIDIOC_QUERYCAP, capability);
	if (ret < 0) {
		fprintf(stderr, "Error: %s():ioctl(%s, QUERYCAP): %s\n",
			__func__, filename, strerror(errno));
		close(fd);
		return ret;
	}

	if (strcmp(DRIVER_NAME, (const char *) capability->driver)) {
		/* not our driver */
		close(fd);
		return 0; /* next! */
	}

	if (!(capability->device_caps & V4L2_CAP_VIDEO_M2M_MPLANE)) {
		fprintf(stderr, "Error: %s(): %s is not VIDEO_M2M_MPLANE.\n",
			__func__, filename);
		close(fd);
		return -EINVAL;
	}

	printf("Input Formats (aka VIDEO_OUTPUT):\n");
	for (i = 0; ; i++) {
		struct v4l2_fmtdesc descriptor[1] = {{
			.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
			.index = i,
		}};

		ret = ioctl(fd, VIDIOC_ENUM_FMT, descriptor);
		if (ret) {
			if (errno == EINVAL)
				break;

			fprintf(stderr,
				"Error: %s():ioctl(ENUM_FMT(input)): %s\n",
				__func__, strerror(errno));
			close(fd);
			return errno;
		}

		if (descriptor->pixelformat == V4L2_PIX_FMT_R8_G8_B8)
			has_prgb = true;

		printf("  %C%C%C%C.\n",
		       (descriptor->pixelformat >> 0) & 0xFF,
		       (descriptor->pixelformat >> 8) & 0xFF,
		       (descriptor->pixelformat >> 16) & 0xFF,
		       (descriptor->pixelformat >> 24) & 0xFF);
	}

	if (!has_prgb) {
		fprintf(stderr, "Error: %s(): missing R8_G8_B8 format.\n",
			__func__);
		close(fd);
		return -EINVAL;
	}

	printf("Output Formats (aka VIDEO_CAPTURE):\n");
	for (i = 0; ; i++) {
		struct v4l2_fmtdesc descriptor[1] = {{
			.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
			.index = i,
		}};

		ret = ioctl(fd, VIDIOC_ENUM_FMT, descriptor);
		if (ret) {
			if (errno == EINVAL)
				break;

			fprintf(stderr,
				"Error: %s():ioctl(ENUM_FMT(output)): %s\n",
				__func__, strerror(errno));
			close(fd);
			return errno;
		}

		if (descriptor->pixelformat == V4L2_PIX_FMT_NV12)
			has_nv12 = true;

		printf("  %C%C%C%C.\n",
		       (descriptor->pixelformat >> 0) & 0xFF,
		       (descriptor->pixelformat >> 8) & 0xFF,
		       (descriptor->pixelformat >> 16) & 0xFF,
		       (descriptor->pixelformat >> 24) & 0xFF);
	}

	if (!has_nv12) {
		fprintf(stderr, "Error: %s(): missing NV12 format.\n",
			__func__);
		close(fd);
		return -EINVAL;
	}

	printf("Found %s driver as %s.\n", DRIVER_NAME, filename);

	return fd;
}

static int
demp_device_find(void)
{
	int i;

	for (i = 0; i < 16; i++) {
		int fd;

		fd = demp_device_open_and_verify(i);
		if (fd)
			return fd;
	}

	fprintf(stderr, "Error: %s: unable to find /dev/videoX node for "
		"\"%s\"\n", __func__, DRIVER_NAME);
	return -ENODEV;
}

static int
demp_png_load(const char *filename)
{
	png_image image[1] = {{
		.version = PNG_IMAGE_VERSION,
	}};
	int ret;

	ret = png_image_begin_read_from_file(image, filename);
	if (ret != 1) {
		fprintf(stderr, "Error: %s():begin_read(): %s\n",
			__func__, image->message);
		return ret;
	}

	image->format = PNG_FORMAT_RGBA;

	printf("Reading from %s: %dx%d (%dbytes)\n", filename,
	       image->width, image->height, PNG_IMAGE_SIZE(*image));

	demp_buffer->width = image->width;
	demp_buffer->height = image->height;

	demp_buffer->png_rgba = calloc(demp_buffer->width * demp_buffer->height,
				      sizeof(uint32_t));
	if (!demp_buffer->png_rgba) {
		fprintf(stderr, "Error: %s(): calloc(): %s\n",
			__func__, strerror(errno));
		png_image_free(image);
		return errno;
	}

	ret = png_image_finish_read(image, NULL, demp_buffer->png_rgba,
				    0, NULL);
	if (ret != 1) {
		fprintf(stderr, "Error: %s():finish_read(): %s\n",
			__func__, image->message);
		free(demp_buffer->png_rgba);
		png_image_free(image);
		return ret;
	}

	png_image_free(image);

	return 0;
}

static void
demp_format_print(struct v4l2_pix_format_mplane *format)
{
	int i;

	printf("  %4d x %4d %C%C%C%C.\n",
	       format->width, format->height,
	       (format->pixelformat >> 0) & 0xFF,
	       (format->pixelformat >> 8) & 0xFF,
	       (format->pixelformat >> 16) & 0xFF,
	       (format->pixelformat >> 24) & 0xFF);

	printf("  %d planes:\n", format->num_planes);
	for (i = 0; i < format->num_planes; i++)
		printf("    pitch %4d bytes, size %6d bytes\n",
		       format->plane_fmt[i].bytesperline,
		       format->plane_fmt[i].sizeimage);
}

static int
demp_input_create(void)
{
	struct v4l2_format format[1] = {{
		.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
	}};
	struct v4l2_requestbuffers request[1] = {{
		.count = 1,
		.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
		.memory = V4L2_MEMORY_MMAP,
	}};
	struct v4l2_plane planes_query[3] = {{ 0 }};
	struct v4l2_buffer query[1] = {{
		.index = 0, /* we only have 1 buffer, so index is 0 */
		.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
		.memory = V4L2_MEMORY_MMAP,
		.length = 3,
		.m.planes = planes_query,
	}};
	int i, ret;

	ret = ioctl(demp_fd, VIDIOC_G_FMT, format);
	if (ret) {
		fprintf(stderr, "Error: %s():ioctl(G_FMT(input)): %s\n",
			__func__, strerror(errno));
		return errno;
	}

	format->fmt.pix_mp.width = demp_buffer->width;
	format->fmt.pix_mp.height = demp_buffer->height;
	format->fmt.pix_mp.pixelformat = V4L2_PIX_FMT_R8_G8_B8;

	ret = ioctl(demp_fd, VIDIOC_S_FMT, format);
	if (ret) {
		fprintf(stderr, "Error: %s():ioctl(S_FMT(input)): %s\n",
			__func__, strerror(errno));
		return errno;
	}

	printf("Input format:\n");
	demp_format_print(&format->fmt.pix_mp);

	ret = ioctl(demp_fd, VIDIOC_REQBUFS, request);
	if (ret) {
		fprintf(stderr, "Error: %s():ioctl(REQBUFS): %s\n",
			__func__, strerror(errno));
		return errno;
	}

	if (request->count < 1) {
		fprintf(stderr, "Error: %s(): Not enough buffers available.\n",
			__func__);
		return -ENOMEM;
	}

	ret = ioctl(demp_fd, VIDIOC_QUERYBUF, query);
	if (ret) {
		fprintf(stderr, "Error: %s():ioctl(QUERYBUF): %s\n",
			__func__, strerror(errno));
		return errno;
	}

	for (i = 0; i < format->fmt.pix_mp.num_planes; i++) {
		off_t offset = query->m.planes[i].m.mem_offset;
		size_t size = query->m.planes[i].length;
		void *map;

		printf("%s: plane %d: 0x%08lX (%dbytes)\n",
		       __func__, i, offset, (int) size);

		map = mmap(NULL, size, PROT_WRITE, MAP_SHARED, demp_fd, offset);
		if (map == MAP_FAILED) {
			fprintf(stderr, "Error: %s():mmap(%d): %s\n",
				__func__, 0, strerror(errno));
			return errno;
		}
		demp_buffer->inputs[i].map = map;
		demp_buffer->inputs[i].size = size;
	}
	demp_buffer->input_count = i;

	return 0;
}

static int
demp_output_create(void)
{
	struct v4l2_format format[1] = {{
		.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
	}};
	struct v4l2_requestbuffers request[1] = {{
		.count = 1,
		.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
		.memory = V4L2_MEMORY_MMAP,
	}};
	struct v4l2_plane planes_query[3] = {{ 0 }};
	struct v4l2_buffer query[1] = {{
		.index = 0, /* we only have 1 buffer, so index is 0 */
		.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
		.memory = V4L2_MEMORY_MMAP,
		.length = 3,
		.m.planes = planes_query,
	}};
	int i, ret;

	ret = ioctl(demp_fd, VIDIOC_G_FMT, format);
	if (ret) {
		fprintf(stderr, "Error: %s():ioctl(G_FMT(format)): %s\n",
			__func__, strerror(errno));
		return errno;
	}

	format->fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;

	ret = ioctl(demp_fd, VIDIOC_S_FMT, format);
	if (ret) {
		fprintf(stderr, "Error: %s():ioctl(S_FMT(input)): %s\n",
			__func__, strerror(errno));
		return errno;
	}

	printf("Output format:\n");
	demp_format_print(&format->fmt.pix_mp);

	ret = ioctl(demp_fd, VIDIOC_REQBUFS, request);
	if (ret) {
		fprintf(stderr, "Error: %s():ioctl(REQBUFS): %s\n",
			__func__, strerror(errno));
		return errno;
	}

	if (request->count < 1) {
		fprintf(stderr, "Error: %s(): Not enough buffers available.\n",
			__func__);
		return -ENOMEM;
	}

	ret = ioctl(demp_fd, VIDIOC_QUERYBUF, query);
	if (ret) {
		fprintf(stderr, "Error: %s():ioctl(QUERYBUF): %s\n",
			__func__, strerror(errno));
		return errno;
	}

	for (i = 0; i < format->fmt.pix_mp.num_planes; i++) {
		off_t offset = query->m.planes[i].m.mem_offset;
		size_t size = query->m.planes[i].length;
		void *map;

		printf("%s: plane %d: 0x%08lX (%dbytes)\n",
		       __func__, i, offset, (int) size);

		map = mmap(NULL, size, PROT_READ, MAP_SHARED, demp_fd, offset);
		if (map == MAP_FAILED) {
			fprintf(stderr, "Error: %s():mmap(%d): %s\n",
				__func__, 0, strerror(errno));
			return errno;
		}
		demp_buffer->outputs[i].map = map;
		demp_buffer->outputs[i].size = size;
	}
	demp_buffer->output_count = i;

	return 0;
}

static void
demp_input_load(void)
{
	struct rgba {
		uint8_t red;
		uint8_t green;
		uint8_t blue;
		uint8_t alpha;
	} *pixels = (struct rgba *) demp_buffer->png_rgba;
	int i;

	for (i = 0; i < demp_buffer->inputs[0].size; i++) {
		demp_buffer->inputs[0].map[i] = pixels[i].red;
		demp_buffer->inputs[1].map[i] = pixels[i].green;
		demp_buffer->inputs[2].map[i] = pixels[i].blue;
	}
}

static int
demp_streaming_start(void)
{
	struct v4l2_plane planes_input[3] = {{ 0 }};
	struct v4l2_buffer queue_input[1] = {{
		.index = 0, /* we only have 1 buffer, so index is 0 */
		.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
		.memory = V4L2_MEMORY_MMAP,
		.m.planes = planes_input,
		.length = 3,
	}};
	struct v4l2_plane planes_output[3] = {{ 0 }};
	struct v4l2_buffer queue_output[1] = {{
		.index = 0, /* we only have 1 buffer, so index is 0 */
		.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
		.memory = V4L2_MEMORY_MMAP,
		.m.planes = planes_output,
		.length = 3,
	}};
	int type_input = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	int type_output = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	int ret;

	ret = ioctl(demp_fd, VIDIOC_QBUF, queue_input);
	if (ret) {
		fprintf(stderr, "Error: %s()ioctl(QBUF(input)): %s\n",
			__func__, strerror(errno));
		return errno;
	}

	ret = ioctl(demp_fd, VIDIOC_STREAMON, &type_input);
	if (ret) {
		fprintf(stderr, "Error: %s(): ioctl(STREAMON(input)): %s\n",
			__func__, strerror(errno));
		return errno;
	}
	printf("Input stream started!\n");

	ret = ioctl(demp_fd, VIDIOC_QBUF, queue_output);
	if (ret) {
		fprintf(stderr, "Error: %s()ioctl(QBUF(output)): %s\n",
			__func__, strerror(errno));
		return errno;
	}

	ret = ioctl(demp_fd, VIDIOC_STREAMON, &type_output);
	if (ret) {
		fprintf(stderr, "Error: %s(): ioctl(STREAMON(output)): %s\n",
			__func__, strerror(errno));
		return errno;
	}
	printf("Output stream started!\n");

	return 0;
}

static int
demp_buffers_dequeue(void)
{
	struct v4l2_plane planes_input[3] = {{ 0 }};
	struct v4l2_buffer dequeue_input[1] = {{
		.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
		.memory = V4L2_MEMORY_MMAP,
		.m.planes = planes_input,
		.length = 3,
	}};
	struct v4l2_plane planes_output[3] = {{ 0 }};
	struct v4l2_buffer dequeue_output[1] = {{
		.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
		.memory = V4L2_MEMORY_MMAP,
		.m.planes = planes_output,
		.length = 3,
	}};
	int ret;

	ret = ioctl(demp_fd, VIDIOC_DQBUF, dequeue_input);
	if (ret) {
		fprintf(stderr, "Error: %s():ioctl(DQBUF(input): %s\n",
			__func__, strerror(errno));
		return ret;
	}
	printf("Input buffer %d dequeued.\n", dequeue_input->index);

	ret = ioctl(demp_fd, VIDIOC_DQBUF, dequeue_output);
	if (ret) {
		fprintf(stderr, "Error: %s():ioctl(DQBUF(output): %s\n",
			__func__, strerror(errno));
		return ret;
	}
	printf("Output buffer %d dequeued.\n", dequeue_output->index);

	return 0;
}

static struct kms_plane *
demp_kms_plane_get(int crtc_index)
{
	drmModePlaneRes *resources_plane = NULL;
	struct kms_plane *kms_plane = NULL;
	int i;

	/* Get plane resources so we can start sifting through the planes */
	resources_plane = drmModeGetPlaneResources(kms_fd);
	if (!resources_plane) {
		fprintf(stderr, "%s: Failed to get KMS plane resources\n",
			__func__);
		goto error;
	}

	/* now cycle through the planes to find one for our crtc */
	for (i = 0; i < (int) resources_plane->count_planes; i++) {
		drmModePlane *plane;
		uint32_t plane_id = resources_plane->planes[i];
		int j;

		plane = drmModeGetPlane(kms_fd, plane_id);
		if (!plane) {
			fprintf(stderr, "%s: failed to get Plane %u: %s\n",
				__func__, plane_id, strerror(errno));
			goto error;
		}

		if (!(plane->possible_crtcs & (1 << crtc_index)))
			goto plane_next;

		for (j = 0; j < (int) plane->count_formats; j++)
			if (plane->formats[j] == DRM_FORMAT_NV12)
				break;

		if (j == (int) plane->count_formats)
			goto plane_next;

		printf("NV12 Plane: ");
		kms_plane = kms_plane_create(plane->plane_id);
		if (!kms_plane)
			goto plane_error;

		break;

	plane_next:
		drmModeFreePlane(plane);
		continue;
	plane_error:
		drmModeFreePlane(plane);
		break;
	}

 error:
	drmModeFreePlaneResources(resources_plane);
	return kms_plane;
}

static int
demp_v4l2_buffers_export(void)
{
	struct v4l2_exportbuffer export[1] = {{
		.index = 0, /* single buffer anyway */
		.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
		.flags = O_RDONLY,
	}};
	int i, ret;

	for (i = 0; i < demp_buffer->output_count; i++) {
		export->plane = i;

		ret = ioctl(demp_fd, VIDIOC_EXPBUF, export);
		if (ret) {
			fprintf(stderr, "Error: %s: ioctl(EXPBUF(%d)): %s\n",
				__func__, i, strerror(errno));
			return ret;
		}

		demp_buffer->outputs[i].export_fd = export->fd;

		printf("Exported output buffer %d to %d.\n", i, export->fd);
	}

	return 0;
}

static int
demp_kms_buffer_import(uint32_t *fb_id)
{
	uint32_t handles[4] = { 0 };
	uint32_t pitches[4] = { 0 };
	uint32_t offsets[4] = { 0 };
	int ret, i;

	for (i = 0; i < demp_buffer->output_count; i++) {
		struct drm_prime_handle prime[1] = {{
				.fd = demp_buffer->outputs[i].export_fd,
			}};

		ret = drmIoctl(kms_fd, DRM_IOCTL_PRIME_FD_TO_HANDLE, prime);
		if (ret) {
			fprintf(stderr, "%s: drmIoctl(PRIME_FD_TO_HANDLE, %d) "
				"failed: %s\n", __func__,
				demp_buffer->outputs[i].export_fd,
				strerror(errno));
			return ret;
		}

		demp_buffer->outputs[i].prime_handle = prime->handle;
		handles[i] = prime->handle;
		pitches[i] = demp_buffer->width;
	}

	printf("%s(): prime handles: %02X, %02X, %02X\n",
	       __func__, demp_buffer->outputs[0].prime_handle,
	       demp_buffer->outputs[1].prime_handle,
	       demp_buffer->outputs[2].prime_handle);

	ret = drmModeAddFB2(kms_fd, demp_buffer->width, demp_buffer->height,
			    DRM_FORMAT_NV12, handles, pitches, offsets,
			    fb_id, 0);
	if (ret) {
		fprintf(stderr, "%s(): failed to create fb: %s\n",
			__func__, strerror(errno));
		return -errno;
	}

	printf("%s(): FB %02u.\n", __func__, *fb_id);

	return 0;
}

static int
demp_kms_fb_show(uint32_t crtc_id, struct kms_plane *plane, uint32_t fb_id)
{
	drmModeAtomicReqPtr request;
	int ret;

	request = drmModeAtomicAlloc();

	drmModeAtomicAddProperty(request, plane->plane_id,
				 plane->property_crtc_id, crtc_id);

	/* Full crtc size */
	drmModeAtomicAddProperty(request, plane->plane_id,
				 plane->property_crtc_x, 0);
	drmModeAtomicAddProperty(request, plane->plane_id,
				 plane->property_crtc_y, 0);
	drmModeAtomicAddProperty(request, plane->plane_id,
				 plane->property_crtc_w,
				 demp_buffer->width);
	drmModeAtomicAddProperty(request, plane->plane_id,
				 plane->property_crtc_h,
				 demp_buffer->height);

	/* read in full size image */
	drmModeAtomicAddProperty(request, plane->plane_id,
				 plane->property_src_x, 0);
	drmModeAtomicAddProperty(request, plane->plane_id,
				 plane->property_src_y, 0);
	drmModeAtomicAddProperty(request, plane->plane_id,
				 plane->property_src_w,
				 demp_buffer->width << 16);
	drmModeAtomicAddProperty(request, plane->plane_id,
				 plane->property_src_h,
				 demp_buffer->height << 16);

	plane->active = true;

	/* actual flip. */
	drmModeAtomicAddProperty(request, plane->plane_id,
				 plane->property_fb_id, fb_id);

	ret = drmModeAtomicCommit(kms_fd, request,
				  DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);

	drmModeAtomicFree(request);

	if (ret) {
		fprintf(stderr, "Error: %s(): drmModeAtomicCommit(): %s\n",
			__func__, strerror(errno));
		return errno;
	}

	return 0;
}

static int
demp_kms_show(void)
{
	struct kms_plane *plane;
	bool connected, mode_ok;
	uint32_t connector_id, encoder_id, crtc_id, fb_id;
	int crtc_width, crtc_height, crtc_index;
	int ret;

	ret = kms_init();
	if (ret)
		return ret;

	ret = kms_connector_id_get(DRM_MODE_CONNECTOR_HDMIA, &connector_id);
	if (ret)
		return ret;

	ret = kms_connection_check(connector_id, &connected, &encoder_id);
	if (ret)
		return ret;

	ret = kms_crtc_id_get(encoder_id, &crtc_id,
			      &mode_ok, &crtc_width, &crtc_height);
	if (ret)
		return ret;

	printf("Using CRTC %X (%dx%d), connector %X (%s).\n",
	       crtc_id, crtc_width, crtc_height, connector_id,
	       kms_connector_string(DRM_MODE_CONNECTOR_HDMIA));

	ret = kms_crtc_index_get(crtc_id);
	if (ret < 0)
		return ret;

	crtc_index = ret;

	plane = demp_kms_plane_get(crtc_index);
	if (!plane)
		return -1;

	ret = demp_v4l2_buffers_export();
	if (ret)
		return ret;

	ret = demp_kms_buffer_import(&fb_id);
	if (ret)
		return ret;

	ret = demp_kms_fb_show(crtc_id, plane, fb_id);
	if (ret)
		return ret;

	printf("Displaying converted NV12 buffer.\n");

	sleep(600);

	return 0;
}

int main(int argc, char *argv[])
{
	int ret;

	if (argc < 2) {
		fprintf(stderr, "Error: missing .png argument.\n");
		return EX_USAGE;
	}

	demp_fd = demp_device_find();
	if (demp_fd < 0)
		return -demp_fd;

	ret = demp_png_load(argv[1]);
	if (ret) {
		fprintf(stderr, "Error: demp_png_load(): %s\n",
			strerror(ret));
		return ret;
	}

	ret = demp_input_create();
	if (ret) {
		fprintf(stderr, "Error: demp_input_create(): %s\n",
			strerror(ret));
		return ret;
	}

	ret = demp_output_create();
	if (ret) {
		fprintf(stderr, "Error: demp_output_create(): %s\n",
			strerror(ret));
		return ret;
	}

	demp_input_load();

	ret = demp_streaming_start();
	if (ret) {
		fprintf(stderr, "Error: demp_streaming_start(): %s\n",
			strerror(ret));
		return ret;
	}

	ret = demp_buffers_dequeue();
	if (ret) {
		fprintf(stderr, "Error: demp_buffers_dequeue(): %s\n",
			strerror(ret));
		return ret;
	}

	ret = demp_kms_show();
	if (ret) {
		fprintf(stderr, "Error: demp_kms_show(): %s\n",
			strerror(ret));
		return ret;
	}

	return 0;
}
