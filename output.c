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

#include "kms.h"

int main(int argc, char *argv[])
{
	unsigned long count = 1000;
	int ret;

	if (argc > 1) {
		ret = sscanf(argv[1], "%lu", &count);
		if (ret != 1) {
			fprintf(stderr, "%s: failed to fscanf(%s): %s\n",
				__func__, argv[1], strerror(errno));
			return -1;
		}

		if (count < 0)
			count = 1000;
	}

	printf("Running for %lu frames.\n", count);

	ret = kms_init();
	if (ret)
		return ret;


	while (1)
		sleep(1);

	return 0;
}