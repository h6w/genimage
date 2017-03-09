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

void split_path_file(char** p, char** f, const char *pf) {
    const char *slash = pf, *next;
    while ((next = strpbrk(slash + 1, "\\/"))) slash = next;
    if (pf != slash) slash++;
    if (*slash == '/') slash++;
    *p = strndup(pf, slash - pf);
    *f = strdup(slash);
}

static int readuntil(struct image *image, int stream, char *expected, char *response) {
/*
    char buf[1024];
    int numchars;
    int totalchars = 0, prevtotalchars = 0;
    response[0] = '\0';
    do {
        prevtotalchars = totalchars;
        usleep(100);
        do {
            numchars = read(stream, buf, 1024);
            buf[numchars] = '\0';
            strcat(response,buf);
            totalchars += numchars;
        } while (strstr(response,expected) == NULL);
    } while (prevtotalchars != totalchars);
    image_log(image, 1, "from debugfs[%d]: %s\n", totalchars, response);
*/

    char *p;
    char ch;
    char buf[2];
    p = expected;
    char *bufp;
    bufp = response;
    int i = 0;

    while(*p != '\0') {
        read(stream, buf, 1);
        ch = buf[0];
        //fprintf(stderr,"[%d]%c**%d\n",i,ch,(int)ch);
        if (bufp-response > 1 && (int)ch == 13 && (int)(*(bufp-1)) == 32) {
            if (p > expected && *p == *(bufp-2)) p--;
            bufp -= 1;
            //fprintf(stderr,"Back 2\n");
            continue;
        }
        //fprintf(stderr,"%c",ch);
        //fflush(stderr);
        *bufp = ch;
        //fprintf(stderr,"[%c%c] [%c%c]\n", *p, *(p+1), *(bufp-2), *(bufp-1));
        if (bufp-response > 1 && *p == *(bufp-2)) p++;
        else p = expected;
        if (*p == *(bufp-1) && *(p+1) == '\0') {
            *bufp='\0';
            image_log(image, 1, " <-- debugfs[%ld]: %s\n", (bufp-response), response);
            return 0;
        }
        bufp++;
        i++;
    }
    if (*p == '\0') {
        *bufp = '\n';
        bufp++;
    }
    *bufp='\0';
    image_log(image, 1, " <-- debugfs[%ld]: %s\n", (bufp-response), response);

    return 0;
}

static int verify_directory_exists(struct bdpipe *debugfspipe, const char *dirpath, struct image *image) {
    //image_log(image, 1, " dirpath: %s\n", dirpath);
    const char *p;
    char *tmp;
    int ret = 0;
    p = dirpath+strlen(dirpath)-1;
    bool pathok = false;
    char response[1024];
    char action[1024];
    //image_log(image, 1, " dirpath: %s\n", dirpath);
    //image_log(image, 1, " dirpath: %s p: %s diff: %d\n", dirpath, p, p-dirpath);
    while (p >= dirpath && !pathok) {
        //image_log(image, 1, " dirpath: %s p: %s diff: %d\n", dirpath, p, p-dirpath);
        if (*p == '/' && p-dirpath >= 0) {
            tmp = strndup(dirpath, p-dirpath+1);
            sprintf(action,"cd %s", tmp);
            image_log(image, 1, " --> debugfs[%s]: %s\n",
                            imageoutfile(image), action);
            ret = write(debugfspipe->write, action, strlen(action));
            ret = write(debugfspipe->write, "\n", 1);
            readuntil(image, debugfspipe->read, action, response);
            readuntil(image, debugfspipe->read, DEBUGFS_PROMPT,response);
            if (strstr(response,"not found") == NULL) pathok = true;
            free(tmp);
        }
        if (!pathok) p--;
    }
    while (*p != '\0' && !(*p == '/' && *(p+1) == '\0')) {
        p++;
        if ((*p == '/' && p-dirpath > 0)) {
            tmp = strndup(dirpath, p-dirpath);
            sprintf(action,"mkdir %s", tmp);
            image_log(image, 1, " --> debugfs[%s]: %s\n",
                             imageoutfile(image), action);
            ret = write(debugfspipe->write, action, strlen(action));
            ret = write(debugfspipe->write, "\n", 1);
            readuntil(image, debugfspipe->read, action, response);
            readuntil(image, debugfspipe->read, DEBUGFS_PROMPT,response);
            free(tmp);
        }
    }
    return ret;
}

static int add_directory(struct bdpipe * debugfspipe, const char *dirpath, struct image *image, struct image *child, const char *target, const char *file)
{
    int result;

    int add_file(const char *filepath, const struct stat *info,
                    const int typeflag, struct FTW *pathinfo)
    {
        int ret = 0;
        char response[1024];
        char action[1024];

        if (typeflag == FTW_SL) {
            char   *file_target;
            size_t  maxlen = 1023;
            ssize_t len;

            while (1) {

                file_target = malloc(maxlen + 1);
                if (file_target == NULL)
                    return ENOMEM;

                len = readlink(filepath, file_target, maxlen);
                if (len == (ssize_t)-1) {
                    const int saved_errno = errno;
                    free(file_target);
                    return saved_errno;
                }
                if (len >= (ssize_t)maxlen) {
                    free(file_target);
                    maxlen += 1024;
                    continue;
                }

                file_target[len] = '\0';
                break;
            }

            printf(" %s -> %s\n", filepath, file_target);
            free(file_target);

        } else
        if (typeflag == FTW_SLN)
            printf("WARNING: NOT adding %s (dangling symlink)\n", filepath);
        else
        if (typeflag == FTW_F) {
            struct stat sb;
            stat(filepath, &sb);
            if (!S_ISREG(sb.st_mode)) {
                image_log(image, 1, " !!! debugfs[%s]: NONREGULAR FILE UNHANDLED %s\n",
                                imageoutfile(image), filepath);
                return 0;
            }

            char target_filepath[1024];
            char *target_path;
            char *target_file;

            strcpy(target_filepath,target);
            strcat(target_filepath,filepath+strlen(dirpath));

            image_log(image, 1, "Adding file '%s' as '%s' ...\n",
                            filepath, target_filepath);

            split_path_file(&target_path, &target_file, target_filepath);

            image_log(image, 1, "Verifying parent directory %s...\n", target_path);

            verify_directory_exists(debugfspipe, target_path, image);

/*
            image_log(image, 1, "debugfs[%s]:Parent directory %s exists\n",
                                imageoutfile(image), target_path);

            image_log(image, 1, "debugfs[%s]: cd %s\n",
                            imageoutfile(image), target_path);
            ret = dprintf(debugfspipe->write, "cd %s\n", target_path);
            readuntil(image,debugfspipe->read,DEBUGFS_PROMPT);
*/
            sprintf(action,"write %s %s", filepath, target_file);
            image_log(image, 1, " --> debugfs[%s]: %s\n",
                            imageoutfile(image), action);
            ret = write(debugfspipe->write, action, strlen(action));
            ret = write(debugfspipe->write, "\n", 1);
            readuntil(image, debugfspipe->read, action, response);
            readuntil(image, debugfspipe->read, DEBUGFS_PROMPT,response);
//            printf(" %s\n", filepath);
            ret = 0;
        } else if (typeflag == FTW_D || typeflag == FTW_DP) {
            char new_filepath[1024];
            strcpy(new_filepath,filepath);
            strcat(new_filepath,"/");

            char target_filepath[1024];

            strcpy(target_filepath,target);
            strcat(target_filepath,new_filepath+strlen(dirpath));

            image_log(image, 1, " --- debugfs[%s]: Adding directory from file structure %s to %s\n",
                            imageoutfile(image), new_filepath, target_filepath);

            verify_directory_exists(debugfspipe, target_filepath, image);

            ret = 0;
        } else if (typeflag == FTW_DNR) {
            printf("WARNING: NOT adding %s/ (unreadable)\n", filepath);
            ret = 0;
        } else {
            printf("WARNING: NOT adding %s (unknown)\n", filepath);
            ret = 0;
        }

        return ret;
    }



    /* Invalid directory path? */
    if (dirpath == NULL || *dirpath == '\0')
        return errno = EINVAL;

    result = nftw(dirpath, add_file, USE_FDS, FTW_PHYS);
    if (result >= 0)
        errno = result;

    return errno;
}

static int open_and_add_directory(const char *dirpath, struct image *image, struct image *child, const char *target, const char *file)
{
    struct bdpipe *debugfspipe;
    char response[1024];

    image_log(image, 1, "Opening connection to debugfs[%s]...",
                            imageoutfile(image));
    debugfspipe = popenbdp(image, "w", "%s -w %s",
                      get_opt("debugfs"), imageoutfile(image));
    image_log(image, 1, "open\n");
    readuntil(image,debugfspipe->read,DEBUGFS_PROMPT,response);

    add_directory(debugfspipe, dirpath, image, child, target, file);

    dprintf(debugfspipe->write, "quit\n");
    //readuntil(image,debugfspipe->read,DEBUGFS_PROMPT);

    return 0;
}


static int ext2_generate(struct image *image)
{
	int ret;
        struct partition *part;
	char *extraargs = cfg_getstr(image->imagesec, "extraargs");
	char *features = cfg_getstr(image->imagesec, "features");
	char *label = cfg_getstr(image->imagesec, "label");

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

    list_for_each_entry(part, &image->partitions, list) {
	        image_log(image, 1, "Entry start:\n");
            struct image *child = image_get(part->image);
            const char *file = imageoutfile(child);
            const char *target = part->name;
            char *path = strdupa(target);
            char *next = path;

	        image_log(image, 1, "Entry: File:%s Target:%s Path:%s Next:%s\n",
			file, target, path, next);

            struct stat fileinfo;
            if (stat(file, &fileinfo) != 0)
	            image_log(image, 1, "Stat failed.\n");
            if (S_ISDIR(fileinfo.st_mode)) {
	            image_log(image, 1, "It's a directory.\n");
                open_and_add_directory(file,image,child,target,file);
            } else {
	            image_log(image, 1, "It's a file.\n");
            }
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

	for(i = 0; i < cfg_size(cfg, "files"); i++) {

		cfg_t *filessec = cfg_getnsec(cfg, "files", i);
        if (cfg_getstr(filessec, "source") != NULL) {
	            part = xzalloc(sizeof *part);
	            part->name = cfg_title(filessec);
	            part->image = cfg_getstr(filessec, "source");
	            list_add_tail(&part->list, &image->partitions);
        }

        unsigned int num_sources = 0;
		num_sources = cfg_size(filessec,"sources");
        for(j = 0; j < num_sources; j++) {
            part = xzalloc(sizeof *part);
            part->name = cfg_title(filessec);
            part->image = cfg_getnstr(filessec, "sources", j);
            list_add_tail(&part->list, &image->partitions);
        }
	}

	for (i = 0; i < cfg_size(cfg, "file"); i++) {
		cfg_t *filesec = cfg_getnsec(cfg, "file", i);
		part = xzalloc(sizeof *part);
		part->name = cfg_title(filesec);
		part->image = cfg_getstr(filesec, "image");
		list_add_tail(&part->list, &image->partitions);
	}

	return 0;
}


static cfg_opt_t file_opts[] = {
	CFG_STR("image", NULL, CFGF_NONE),
	CFG_END()
};

static cfg_opt_t files_opts[] = {
	CFG_STR("source", NULL, CFGF_NONE),
	CFG_STR_LIST("sources", 0, CFGF_NONE),
	CFG_END()
};


static cfg_opt_t ext2_opts[] = {
	CFG_STR("extraargs", "", CFGF_NONE),
	CFG_STR("features", 0, CFGF_NONE),
	CFG_STR("label", 0, CFGF_NONE),
	CFG_SEC("files", files_opts, CFGF_MULTI | CFGF_TITLE),
	CFG_SEC("file", file_opts, CFGF_MULTI | CFGF_TITLE),
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
	CFG_SEC("file", file_opts, CFGF_MULTI | CFGF_TITLE),
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
	CFG_SEC("file", file_opts, CFGF_MULTI | CFGF_TITLE),
	CFG_END()
};

struct image_handler ext4_handler = {
	.type = "ext4",
	.generate = ext2_generate,
  .parse = ext2_parse,
	.opts = ext4_opts,
};

