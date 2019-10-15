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
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <stdbool.h>
#include <sysexits.h>

#include "juggler.h"
#include "capture.h"
#include "kms.h"
#include "status.h"
#include "projector.h"

void
usage(const char *name)
{
	printf("%s: the central FOSDEM video capture hardware tool.\n", name);
	printf("\n");
	printf("usage: %s [-t] [frames] [hoffset] [voffset]\n", name);
	printf("  -t\t\tTest frames for position markers to validate "
	       "integrity.\n");
	printf("  frames\tThe number of frames to capture and display.\n");
	printf("  hoffset\tCSI capture starts hoffset pixels after HSync.\n");
	printf("  voffset\tCSI capture starts voffset lines after VSync.\n");
	printf("\n");
}

int main(int argc, char *argv[])
{
	unsigned long count = 1000;
	unsigned int hoffset = -1, voffset = -1;
	int ret;

	if (argc > 1) {
		ret = sscanf(argv[1], "%lu", &count);
		if (ret != 1) {
			fprintf(stderr, "%s: failed to fscanf(%s) to "
				"frame count: %s\n",
				__func__, argv[1], strerror(errno));
			usage(argv[0]);
			return EX_USAGE;
		}

		if (count < 0)
			count = 1000;
	}

	if (argc > 2) {
		ret = sscanf(argv[2], "%i", &hoffset);
		if (ret != 1) {
			fprintf(stderr, "%s: failed to fscanf(%s) to "
				"h offset: %s\n",
				__func__, argv[2], strerror(errno));
			usage(argv[0]);
			return EX_USAGE;
		}
	}

	if (argc > 3) {
		ret = sscanf(argv[3], "%i", &voffset);
		if (ret != 1) {
			fprintf(stderr, "%s: failed to fscanf(%s) to "
				"v offset: %s\n",
				__func__, argv[3], strerror(errno));
			usage(argv[0]);
			return EX_USAGE;
		}
	}

	printf("Running for %lu frames.\n", count);

	ret = kms_init();
	if (ret)
		return ret;

	ret = kms_status_init(count);
	if (ret)
		return ret;

	ret = kms_projector_init(count);
	if (ret)
		return ret;

	ret = capture_init(count, hoffset, voffset);
	if (ret)
		return ret;

	/* todo: properly wait for threads to return */
	while (1)
		sleep(1);

	return 0;
}
