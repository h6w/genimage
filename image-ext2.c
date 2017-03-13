/*
 * Copyright (c) 2011 Sascha Hauer <s.hauer@pengutronix.de>, Pengutronix
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <confuse.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <ftw.h>
#include <stdbool.h>

/* POSIX.1 says each process has at least 20 file descriptors.
 * Three of those belong to the standard streams.
 * Here, we use a conservative estimate of 15 available;
 * assuming we use at most two for other uses in this program,
 * we should never run into any problems.
 * Most trees are shallower than that, so it is efficient.
 * Deeper trees are traversed fine, just a bit slower.
 * (Linux allows typically hundreds to thousands of open files,
 *  so you'll probably never see any issues even if you used
 *  a much higher value, say a couple of hundred, but
 *  15 is a safe, reasonable value.)
*/
#ifndef USE_FDS
#define USE_FDS 15
#endif

#include "genimage.h"

#define DEBUGFS_PROMPT "debugfs: "

static int ext2_generate(struct image *image)
{
	int ret;
    struct partition *part;
	char *extraargs = cfg_getstr(image->imagesec, "extraargs");
	char *features = cfg_getstr(image->imagesec, "features");
	char *label = cfg_getstr(image->imagesec, "label");

    list_for_each_entry(part, &image->partitions, list) {
        image_log(image, 1, "Entry start:\n");
        struct image *child = image_get(part->image);
        const char *file = imageoutfile(child);
        const char *target = part->name;
        char *path = strdupa(target);
        char *next = path;

        image_log(image, 1, "Entry: Mountpath:%s File:%s Target:%s Path:%s Next:%s\n",
		mountpath(image), file, target, path, next);

        char target_filepath[1024];

        strcpy(target_filepath,mountpath(image));
        strcat(target_filepath,part->name);

        ret = systemp(image, "%s %s %s",
			get_opt("rsync"),
			imageoutfile(image),
			target_filepath);

    }

	image_log(image, 1, "Generating ext2 image...\n");
	ret = systemp(image, "%s -d %s --size-in-blocks=%lld -i 16384 %s %s",
			get_opt("genext2fs"),
			mountpath(image), image->size / 1024, imageoutfile(image),
			extraargs);

	if (ret)
		return ret;

	if (features && features[0] != '\0') {
		image_log(image, 1, "%s -O \"%s\" %s\n", get_opt("tune2fs"),
				features, imageoutfile(image));
		ret = systemp(image, "%s -O \"%s\" %s", get_opt("tune2fs"),
				features, imageoutfile(image));
		if (ret)
			return ret;
	}
	if (label && label[0] != '\0') {
		image_log(image, 1, "%s -L \"%s\" %s\n", get_opt("tune2fs"),
				label, imageoutfile(image));
		ret = systemp(image, "%s -L \"%s\" %s", get_opt("tune2fs"),
				label, imageoutfile(image));
		if (ret)
			return ret;
	}

    if (!list_empty(&image->partitions))
            return 0;

	ret = systemp(image, "%s -pvfD %s", get_opt("e2fsck"),
			imageoutfile(image));

	/* e2fsck return 1 when the filesystem was successfully modified */
	return ret > 2;
}

static int ext2_parse(struct image *image, cfg_t *cfg)
{
	unsigned int i, j;
	struct partition *part;

	for (i = 0; i < cfg_size(cfg, "files"); i++) {
	    cfg_t *filesec = cfg_getnsec(cfg, "files", i);
        if (cfg_getstr(filesec, "image") != NULL) {
		    part = xzalloc(sizeof *part);
		    part->name = cfg_title(filesec);
		    part->image = cfg_getstr(filesec, "image");
		    list_add_tail(&part->list, &image->partitions);
        }

        if (cfg_getstr(filesec, "source") != NULL) {
            part = xzalloc(sizeof *part);
            part->name = cfg_title(filesec);
            part->image = cfg_getstr(filesec, "source");
            list_add_tail(&part->list, &image->partitions);
        }

        unsigned int num_sources = 0;
		num_sources = cfg_size(filesec,"sources");
        for(j = 0; j < num_sources; j++) {
            part = xzalloc(sizeof *part);
            part->name = cfg_title(filesec);
            part->image = cfg_getnstr(filesec, "sources", j);
            list_add_tail(&part->list, &image->partitions);
        }

	}

	return 0;
}


static cfg_opt_t files_opts[] = {
	CFG_STR("image", NULL, CFGF_NONE),
	CFG_STR("source", NULL, CFGF_NONE),
	CFG_STR_LIST("sources", 0, CFGF_NONE),
	CFG_END()
};

static cfg_opt_t ext2_opts[] = {
	CFG_STR("extraargs", "", CFGF_NONE),
	CFG_STR("features", 0, CFGF_NONE),
	CFG_STR("label", 0, CFGF_NONE),
	CFG_SEC("files", files_opts, CFGF_MULTI | CFGF_TITLE),
	CFG_END()
};

struct image_handler ext2_handler = {
	.type = "ext2",
	.generate = ext2_generate,
    .parse = ext2_parse,
	.opts = ext2_opts,
};

static cfg_opt_t ext3_opts[] = {
	CFG_STR("extraargs", "", CFGF_NONE),
	CFG_STR("features", "has_journal", CFGF_NONE),
	CFG_STR("label", 0, CFGF_NONE),
	CFG_SEC("files", files_opts, CFGF_MULTI | CFGF_TITLE),
	CFG_END()
};

struct image_handler ext3_handler = {
	.type = "ext3",
	.generate = ext2_generate,
    .parse = ext2_parse,
	.opts = ext3_opts,
};

static cfg_opt_t ext4_opts[] = {
	CFG_STR("extraargs", "", CFGF_NONE),
	CFG_STR("features", "extents,uninit_bg,dir_index,has_journal", CFGF_NONE),
	CFG_STR("label", 0, CFGF_NONE),
	CFG_SEC("files", files_opts, CFGF_MULTI | CFGF_TITLE),
	CFG_END()
};

struct image_handler ext4_handler = {
	.type = "ext4",
	.generate = ext2_generate,
    .parse = ext2_parse,
	.opts = ext4_opts,
};

