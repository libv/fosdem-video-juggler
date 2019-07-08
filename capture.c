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
 * Small tool to capture the output of hdmi_output and qiuckly verify
 * some pixels for signal integrity and frame sequentiality.
 */

#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <inttypes.h>

#include <linux/videodev2.h>

int capture_fd = -1;

char *v4l2_device_card_name = "sun4i_csi1";

enum v4l2_buf_type capture_type = -1;
int capture_width;
int capture_height;
int capture_stride;
size_t capture_size;

static int
v4l2_device_find(void)
{
	struct v4l2_capability capability[1];
	char filename[128];
	int fd, ret, i;

	for (i = 0; i < 16; i++) {
		ret = snprintf(filename, sizeof(filename), "/dev/video%d", i);
		if (ret <= 10) {
			fprintf(stderr,
				"failed to create v4l2 device filename: %d",
				ret);
			return ret;
		}

		fd = open(filename, O_RDWR);
		if (fd < 0) {
			if ((errno == ENODEV) || (errno == ENOENT)) {
				continue;
			} else {
				fprintf(stderr, "Error: failed to open %s: "
					"%s\n", filename, strerror(errno));
				return fd;
			}
		}

		memset(capability, 0, sizeof(struct v4l2_capability));

		ret = ioctl(fd, VIDIOC_QUERYCAP, capability);
		if (ret < 0) {
			fprintf(stderr, "Error: ioctl(VIDIOC_QUERYCAP) on %s"
				" failed: %s\n", filename, strerror(errno));
			return ret;
		}

		if (!strcmp("sun4i_csi1", (const char *) capability->driver) &&
		    (capability->device_caps & V4L2_CAP_VIDEO_CAPTURE)) {
			printf("Found sun4i_csi1 driver as %s.\n",
			       filename);
			return fd;
		}

		close(fd);
	}

	fprintf(stderr, "Error: unable to find /dev/videoX node for "
		"\"sun4i_csi1\"\n");
	return -ENODEV;
}

static int
v4l2_format_get(void)
{
	struct v4l2_format format[1] = {{
			.type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
		}};
	int ret;
	uint32_t fourcc;

	ret = ioctl(capture_fd, VIDIOC_G_FMT, format);
	if (ret) {
		fprintf(stderr, "Error: ioctl(VIDIOC_G_FMT) failed: %s\n",
			strerror(errno));
		return ret;
	}

	capture_width = format->fmt.pix.width;
	capture_height = format->fmt.pix.height;
	capture_stride = format->fmt.pix.bytesperline;
	capture_size = format->fmt.pix.sizeimage;
	fourcc = format->fmt.pix.pixelformat;

	printf("Format is %dx%d (%dbytes, %dkB) %C%C%C%C\n",
	       capture_width, capture_height, capture_stride,
	       (int) (capture_size >> 10),
	       (fourcc >> 0) & 0xFF, (fourcc >> 8) & 0xFF,
	       (fourcc >> 16) & 0xFF, (fourcc >> 24) & 0xFF);

	return 0;
}

int main(int argc, char *argv[])
{
	int ret;

	capture_fd = v4l2_device_find();
	if (capture_fd < 0)
		return -capture_fd;

	ret = v4l2_format_get();
	if (ret)
		return ret;

	return 0;
}
