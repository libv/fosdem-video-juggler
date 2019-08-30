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
};

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
