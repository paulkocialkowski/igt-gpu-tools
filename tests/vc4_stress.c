/*
 * Copyright © 2017 Broadcom
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "igt.h"
#include "igt_sysfs.h"
#include "igt_vc4.h"
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <inttypes.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <poll.h>
#include "vc4_drm.h"

igt_output_t *select_output(igt_display_t *display);
void bandwidth_limit_check(int drm_fd, igt_display_t *display,
			   unsigned int tries);

igt_output_t *select_output(igt_display_t *display)
{
	igt_output_t *output;
	enum pipe pipe;

	for_each_pipe(display, pipe) {
		for_each_valid_output_on_pipe(display, pipe, output) {
			drmModeConnector *connector = output->config.connector;

			if (connector->connection != DRM_MODE_CONNECTED)
				continue;

			igt_output_set_pipe(output, pipe);

			return output;
		}
	}

	return NULL;
}

void bandwidth_limit_check(int drm_fd, igt_display_t *display,
			   unsigned int tries)
{
	igt_output_t *output;
	drmModeModeInfo *mode;
	struct igt_fb primary_fb;
	igt_pipe_t *pipe;
	igt_plane_t *primary_plane;
	struct igt_fb *overlay_fbs;
	unsigned int overlay_planes_max = 0;
	unsigned int overlay_planes_count;
	unsigned int overlay_planes_index;
	bool bandwidth_exceeded = false;
	bool underrun_detected = false;
	uint32_t overlay_width, overlay_height;
	unsigned int fb_id;
	unsigned int i;
	char *underrun;
	int debugfs;
	int ret;

	debugfs = igt_debugfs_dir(drm_fd);
	igt_assert(debugfs >= 0);

	output = select_output(display);
	igt_assert(output);

	igt_debug("Selected connector %s\n",
		  kmstest_connector_type_str(output->config.connector->connector_type));

	pipe = &display->pipes[output->pending_pipe];

	mode = igt_output_get_mode(output);
	igt_assert(mode);

	primary_plane = igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY);
	igt_assert(primary_plane);

	fb_id = igt_create_pattern_fb(drm_fd, mode->hdisplay, mode->vdisplay,
				      DRM_FORMAT_XRGB8888,
				      LOCAL_DRM_FORMAT_MOD_NONE,
				      &primary_fb);
	igt_assert(fb_id > 0);

	igt_plane_set_fb(primary_plane, &primary_fb);

	for (i = 0; i < pipe->n_planes; i++) {
		igt_plane_t *plane = &pipe->planes[i];

		if (plane->type != DRM_PLANE_TYPE_OVERLAY)
			overlay_planes_max++;
	}

	overlay_planes_count = 1;

	overlay_width = mode->hdisplay / 2;
	overlay_height = mode->vdisplay / 2;

	do {
		igt_debug("Using %d overlay planes with resolution %dx%d\n",
			  overlay_planes_count, overlay_width, overlay_height);
		igt_debug("%d tries remaining\n", tries);

		overlay_fbs = calloc(sizeof(struct igt_fb), overlay_planes_count);

		for (i = 0, overlay_planes_index = 0;
		     i < pipe->n_planes &&
		     overlay_planes_index < overlay_planes_count;
		     i++) {
			struct igt_fb *overlay_fb =
				&overlay_fbs[overlay_planes_index];
			igt_plane_t *plane = &pipe->planes[i];

			if (plane->type != DRM_PLANE_TYPE_OVERLAY)
				continue;

			fb_id = igt_create_pattern_fb(drm_fd, overlay_width,
						      overlay_height,
						      DRM_FORMAT_XRGB8888,
						      LOCAL_DRM_FORMAT_MOD_NONE,
						      overlay_fb);
			igt_assert(fb_id > 0);

			igt_plane_set_fb(plane, overlay_fb);

			overlay_planes_index++;
		}

		igt_sysfs_set(debugfs, "load_tracker", "Y");

		ret = igt_display_try_commit2(display, COMMIT_ATOMIC);
		bandwidth_exceeded = (ret < 0 && errno == ENOSPC);

		igt_debug("Bandwidth limitation exeeded: %s\n",
			  bandwidth_exceeded ? "Yes" : "No");

		igt_sysfs_set(debugfs, "load_tracker", "N");

		igt_display_commit2(display, COMMIT_ATOMIC);

		igt_wait_for_vblank(drm_fd, pipe->pipe);

		underrun = igt_sysfs_get(debugfs, "underrun");
		igt_assert(underrun);

		underrun_detected = (underrun[0] == 'Y');
		free(underrun);

		igt_debug("Underrun detected: %s\n",
			  underrun_detected ? "Yes" : "No");

		igt_assert(bandwidth_exceeded == underrun_detected);

		if (bandwidth_exceeded && underrun) {
			if (overlay_width > mode->hdisplay / 16) {
				overlay_width /= 2;
				overlay_height /= 2;
			} else if (overlay_planes_count > 0) {
				overlay_planes_count--;
			}
		} else if (overlay_planes_count < overlay_planes_max) {
			overlay_planes_count++;
		} else {
			overlay_width *= 2;
			overlay_height *= 2;
		}
/*
		for (i = 0, overlay_planes_index = 0;
		     i < pipe->n_planes && overlay_planes_index < overlay_planes_count;
		     i++) {
			igt_plane_t *plane = &pipe->planes[i];
			struct igt_fb *overlay_fb = &overlay_fbs[overlay_planes_index];

			if (plane->type != DRM_PLANE_TYPE_OVERLAY)
				continue;

			igt_remove_fb(drm_fd, overlay_fb);

			overlay_planes_index++;
		}
*/
		free(overlay_fbs);
	} while (tries-- > 0);

	igt_remove_fb(drm_fd, &primary_fb);

	close(debugfs);
}

igt_main
{
	igt_display_t display;
	int drm_fd;

	igt_fixture {
		drm_fd = drm_open_driver(DRIVER_VC4);

		kmstest_set_vt_graphics_mode();

		igt_display_require(&display, drm_fd);
		igt_require(display.is_atomic);
	}

	igt_subtest("bandwidth-limit-check")
		bandwidth_limit_check(drm_fd, &display, 10);

	igt_fixture {
		igt_display_fini(&display);
		kmstest_restore_vt_mode();
		close(drm_fd);
	}
}
