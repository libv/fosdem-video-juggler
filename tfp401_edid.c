// SPDX-License-Identifier: GPL-2.0+

/*
 * Copyright (c) 2019 Luc Verhaegen <libv@skynet.be>
 *
 * EDID block for a mode close to standard 720, but adjusted so a tfp401
 * module can reliably capture it.
 */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include <linux/i2c-dev.h>

#define I2CDEV_NAME "/dev/i2c-1"
#define EDID_ADDRESS 0x50
#define EDID_SIZE 0x80

char edid[EDID_SIZE] = {
#if 0 /* hacked edid */
	0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x31, 0xd8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* "........1......." */
	0x05, 0x16, 0x01, 0x03, 0x6d, 0x2c, 0x19, 0x78, 0xea, 0x5e, 0xc0, 0xa4, 0x59, 0x4a, 0x98, 0x25, /* "....m,.x.^..YJ.%" */
	0x20, 0x50, 0x54, 0x00, 0x00, 0x00, 0x81, 0xc0, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, /* " PT............." */
	0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x1a, 0x1d, 0x00, 0x74, 0x51, 0xd0, 0x20, 0x20, 0x6e, 0x28, /* ".........tQ.  n(" */
	0x55, 0x00, 0xbc, 0xfa, 0x10, 0x00, 0x00, 0x1e, 0x00, 0x00, 0x00, 0xff, 0x00, 0x4c, 0x69, 0x6e, /* "U............Lin" */
	0x75, 0x78, 0x20, 0x23, 0x30, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x00, 0x00, 0x00, 0xfd, 0x00, 0x3b, /* "ux, #0.   .....;" */
	0x3d, 0x2c, 0x2e, 0x08, 0x00, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x00, 0x00, 0x00, 0xfc, /* "=,....      ...." */
	0x00, 0x37, 0x32, 0x30, 0x70, 0x20, 0x54, 0x46, 0x50, 0x34, 0x30, 0x31, 0x0a, 0x20, 0x00, 0x3f, /* ".720p TFP401. .?" */
#else /* standard edid */
	0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x18, 0x8d, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* "................" */
	0x05, 0x1e, 0x01, 0x03, 0x6d, 0x2c, 0x19, 0x78, 0xea, 0x5e, 0xc0, 0xa4, 0x59, 0x4a, 0x98, 0x25, /* "....m,.x.^..YJ.%" */
	0x20, 0x50, 0x54, 0x00, 0x00, 0x00, 0x81, 0xc0, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, /* " PT............." */
	0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x1d, 0x00, 0x72, 0x51, 0xd0, 0x1e, 0x20, 0x6e, 0x28, /* ".........rQ.. n(" */
	0x55, 0x00, 0xbc, 0xfa, 0x10, 0x00, 0x00, 0x1e, 0x00, 0x00, 0x00, 0xff, 0x00, 0x4c, 0x69, 0x6e, /* "U............Lin" */
	0x75, 0x78, 0x20, 0x23, 0x30, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x00, 0x00, 0x00, 0xfd, 0x00, 0x3b, /* "ux #0.    .....;" */
	0x3d, 0x2c, 0x2e, 0x08, 0x00, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x00, 0x00, 0x00, 0xfc, /* "=,....      ...." */
	0x00, 0x56, 0x69, 0x64, 0x65, 0x6f, 0x62, 0x6f, 0x78, 0x0a, 0x20, 0x20, 0x20, 0x20, 0x00, 0xc0, /* ".Videobox.    .." */

#endif
};

int
main(int argc, char *argv[])
{
	char buffer[7];
	struct i2c_msg msg = {
		.addr = EDID_ADDRESS,
		.flags = 0,
		.len = 2,
		.buf = buffer,
	};
	struct i2c_rdwr_ioctl_data data = {
		.msgs = &msg,
		.nmsgs = 1,
	};
	int fd, ret, i;

	fd = open(I2CDEV_NAME, O_RDWR);
	if (fd == -1) {
		fprintf(stderr, "Error: Failed to open %s: %s\n",
			I2CDEV_NAME, strerror(errno));
		return errno;
	}

	ret = ioctl(fd, I2C_SLAVE, EDID_ADDRESS);
	if (ret < 0) {
		fprintf(stderr, "Error: Failed to initialize slave 0x%02X: "
			"%s\n", EDID_ADDRESS, strerror(errno));
		return errno;
	}

	for (i = 0; i < EDID_SIZE; i++) {
		buffer[0] = i;
		buffer[1] = edid[i];

		ret = ioctl(fd, I2C_RDWR, &data);
		if (ret < 0) {
			fprintf(stderr, "Error: Failed to write edid at 0x%02X: %s\n",
				i, strerror(errno));
			return errno;
		}

		usleep(5000);
	}

	printf("%s:0x%02X: %d bytes written.\n", I2CDEV_NAME, EDID_ADDRESS, i);

	return 0;
}
