/*
 * (C) Copyright 2013
 * Stefano Babic, DENX Software Engineering, sbabic@denx.de.
 * 	on behalf of ifm electronic GmbH
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston,
 * MA 02111-1307 USA
 */

#include <stdio.h>
#include <sys/mount.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <errno.h>

#include "swupdate.h"
#include "handler.h"
#include "util.h"

void remove_handler(void);

static int remove_path(struct img_type *img,
	void __attribute__ ((__unused__)) *data)
{
	char path[255];
	char key[] = "REMOVE.KEY";
	int ret = 0;
	int use_mount = (strlen(img->device) && strlen(img->filesystem)) ? 1 : 0;

	if (strlen(img->path) == 0) {
		ERROR("Missing path attribute");
		return -1;
	}

	if (strcmp(img->fname, key) != 0) {
		ERROR("Missing %s to allow remove", key);
		return -1;
	}

	if (use_mount) {
		ret = mount(img->device, DATADST_DIR, img->filesystem, 0, NULL);
		if (ret) {
			ERROR("Device %s with filesystem %s cannot be mounted",
				img->device, img->filesystem);
			return -1;
		}

		if (snprintf(path, sizeof(path), "%s%s",
					 DATADST_DIR, img->path) >= (int)sizeof(path)) {
			ERROR("Path too long: %s%s", DATADST_DIR, img->path);
			return -1;
		}
	} else {
		if (snprintf(path, sizeof(path), "%s", img->path) >= (int)sizeof(path)) {
			ERROR("Path too long: %s", img->path);
			return -1;
		}
	}

	TRACE("Removing target %s\n", path);
	ret = remove(path);
	if (ret < 0) {
		if(errno == 2) {
			TRACE("No such target %s", path);
			ret = 0;
		}
		else {
			ERROR("Error removing target %s: %s\n", path, strerror(errno));
		}
	}

	if (use_mount) {
		umount(DATADST_DIR);
	}

	return ret;
}

__attribute__((constructor))
void remove_handler(void)
{
	register_handler("remove", remove_path, NULL);
}
