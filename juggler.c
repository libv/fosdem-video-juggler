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

static bool capture_test = false;
static int capture_hoffset = -1;
static int capture_voffset = -1;

void
usage(const char *name)
{
	printf("%s: the central FOSDEM video capture hardware tool.\n", name);
	printf("usage: %s [-t] [hoffset] [voffset]\n", name);
	printf("  -t\t\tTest frames for position markers to validate "
	       "integrity.\n");
	printf("  hoffset\tCSI capture starts hoffset pixels after HSync.\n");
	printf("  voffset\tCSI capture starts voffset lines after VSync.\n");
	printf("\n");
}

int
args_parse(int argc, char *argv[])
{
	int i = 1; /* skip program name */
	int ret;

	if (i == argc) /* no args */
		return 0;

	if (!strcmp(argv[i], "-t")) {
		capture_test = true;
		i++;
		if (i == argc)
			return 0;
	}

	ret = sscanf(argv[i], "%i", &capture_hoffset);
	if (ret != 1) {
		fprintf(stderr, "\n%s: failed to sscanf(%s) to capture "
			"hoffset.\n\n", __func__, argv[i]);
		goto error;
	}
	i++;
	if (i == argc)
		return 0;

	ret = sscanf(argv[i], "%i", &capture_voffset);
	if (ret != 1) {
		fprintf(stderr, "\n%s: failed to sscanf(%s) to capture "
			"voffset.\n\n", __func__, argv[i]);
		goto error;
	}
	i++;
	if (i == argc)
		return 0;

	fprintf(stderr, "\n%s: too many arguments: \"%s\" is not "
		"handled\n\n", __func__, argv[i]);
 error:
	usage(argv[0]);
	return EX_USAGE;
}

int main(int argc, char *argv[])
{
	int ret;

	ret = args_parse(argc, argv);
	if (ret)
		return ret;

	ret = kms_init();
	if (ret)
		return ret;

	ret = kms_status_init();
	if (ret)
		return ret;

	ret = kms_projector_init();
	if (ret)
		return ret;

	ret = capture_init(capture_test, capture_hoffset, capture_voffset);
	if (ret)
		return ret;

	/* todo: properly wait for threads to return */
	while (1)
		sleep(1);

	return 0;
}
