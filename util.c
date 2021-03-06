/*
 * Copyright (c) 2011 Sascha Hauer <s.hauer@pengutronix.de>, Pengutronix
 *           (c) 2011 Michael Olbrich <m.olbrich@pengutronix.de>
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
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include "genimage.h"

static int skip_log(int level)
{
	static int loglevel = -1;

	if (loglevel < 0) {
		const char *l = get_opt("loglevel");
		if (l)
			loglevel = atoi(l);
		else
			loglevel = 1;
	}

	return (level > loglevel);
}

void image_error(struct image *image, const char *fmt, ...)
{
	va_list args;
	char *buf;

	va_start (args, fmt);

	vasprintf(&buf, fmt, args);

	va_end (args);

	fprintf(stderr, "%s(%s): %s", image->handler ?
		image->handler->type : "unknown", image->file, buf);

	free(buf);
}

void image_log(struct image *image, int level,  const char *fmt, ...)
{
	va_list args;
	char *buf;

	if (skip_log(level))
		return;

	va_start (args, fmt);

	vasprintf(&buf, fmt, args);

	va_end (args);

	fprintf(stderr, "%s(%s): %s", image->handler->type, image->file, buf);

	free(buf);
}

void error(const char *fmt, ...)
{
	va_list args;

	va_start (args, fmt);

	vfprintf(stderr, fmt, args);

	va_end (args);
}

void logmsg(int level, const char *fmt, ...)
{
	va_list args;

	if (skip_log(level))
		return;

	va_start (args, fmt);

	vfprintf(stderr, fmt, args);

	va_end (args);
}

/*
 * printf wrapper around 'system'
 */
int systemp(struct image *image, const char *fmt, ...)
{
	va_list args;
	char *buf;
	int ret;

	va_start (args, fmt);

	vasprintf(&buf, fmt, args);

	va_end (args);

	if (!buf)
		return -ENOMEM;

	if (image)
		image_log(image, 2, "cmd: %s\n", buf);
	else
		logmsg(2, "cmd: %s\n", buf);

	ret = system(buf);

	if (ret > 0)
		ret = WEXITSTATUS(ret);

	free(buf);

	return ret;
}



/*
 * bidirectional process communication
 */
int popenbd(const char *cmdline, struct bdpipe *pipeinfo) {
    pid_t p;
    int parent_child[2], child_parent[2];  // 0 - read end of pipe, 1 - write end of pipe


    if (pipe(parent_child))
        return -1;
    if (pipe(child_parent))
        return -1;

    p = fork();

    if(p < 0) return p;

    if(p == 0) {
        close(parent_child[1]);     //Close the write end of the parent->child pipe, because that's not us.
        close(child_parent[0]);     //Close the read end of the child->parent pipe, because we're not listening to the parent.
        dup2(parent_child[0], 0);   //Replace our stdin (0) with the readable end of the parent->child pipe
        dup2(child_parent[1], 1);   //Replace our stdout (1) to the writable end of the child->parent pipe
        dup2(child_parent[1], 2);   //Replace our stderr (1) to the writable end of the child->parent pipe
        execl("/bin/sh", "sh", "-c", cmdline, (char *)0);
        perror("execl"); exit(99);
    }

    pipeinfo->pid = p;
    pipeinfo->write = parent_child[1];
    pipeinfo->read = child_parent[0];
    return 0;
}

/*
 * printf wrapper around 'popen'
 */
FILE *popenp(struct image *image, const char *mode, const char *fmt, ...)
{
	va_list args;
	char *buf;
	FILE *ret;

	va_start (args, fmt);

	vasprintf(&buf, fmt, args);

	va_end (args);

	if (!buf)
		return NULL;

	if (image)
		image_log(image, 2, "cmd: %s\n", buf);
	else
		logmsg(2, "cmd: %s\n", buf);

	ret = popen(buf, mode);

	if (ret == NULL)
		image_log(image, 1, "PROCESS OPEN FAILED!! cmd: %s\n", buf);
//		ret = errno;

	return ret;
}

/*
 * printf wrapper around 'popenbd'
 */
struct bdpipe *popenbdp(struct image *image, const char *mode, const char *fmt, ...)
{
	va_list args;
	char *buf;
	struct bdpipe *p = malloc(sizeof *p);
    int ret = 0;

	va_start (args, fmt);

	vasprintf(&buf, fmt, args);

	va_end (args);

	if (!buf)
		return NULL;

	if (image)
		image_log(image, 2, "cmd: %s\n", buf);
	else
		logmsg(2, "cmd: %s\n", buf);

	ret = popenbd(buf, p);

	if (ret < 0)
		image_log(image, 1, "PROCESS OPEN FAILED!! cmd: %s\n", buf);
//		ret = errno;

	return p;
}

/*
 * xzalloc - safely allocate zeroed memory
 */
void *xzalloc(size_t n)
{
	void *m = malloc(n);

	if (!m) {
		error("out of memory\n");
		exit(1);
	}

	memset(m, 0, n);

	return m;
}

/*
 * Like simple_strtoul() but handles an optional G, M, K or k
 * suffix for Gigabyte, Megabyte or Kilobyte
 */
unsigned long long strtoul_suffix(const char *str, char **endp, int base)
{
	unsigned long long val;
	char *end;

	val = strtoull(str, &end, base);

	switch (*end) {
	case 'G':
		val *= 1024;
	case 'M':
		val *= 1024;
	case 'k':
	case 'K':
		val *= 1024;
		end++;
	default:
		break;
	}

	if (endp)
		*endp = (char *)end;

	return val;
}

static size_t min(size_t a, size_t b)
{
	return a < b ? a : b;
}

int pad_file(struct image *image, const char *infile, const char *outfile,
		size_t size, unsigned char fillpattern, enum pad_mode mode)
{
	FILE *f = NULL, *outf = NULL;
	void *buf = NULL;
	int now, r, w;
	int ret = 0;

	if (infile) {
		f = fopen(infile, "r");
		if (!f) {
			image_error(image, "open %s: %s\n", infile, strerror(errno));
			ret = -errno;
			goto err_out;
		}
	}

	outf = fopen(outfile, mode == MODE_OVERWRITE ? "w" : "a");
	if (!outf) {
		image_error(image, "open %s: %s\n", outfile, strerror(errno));
		ret = -errno;
		goto err_out;
	}

	buf = xzalloc(4096);

	if (!infile) {
		struct stat s;
		ret = stat(outfile, &s);
		if (ret)
			goto err_out;
		if ((unsigned long long)s.st_size > size) {
			ret = -EINVAL;
			goto err_out;
		}
		size = size - s.st_size;
		goto fill;
	}

	while (size) {
		now = min(size, 4096);

		r = fread(buf, 1, now, f);
		w = fwrite(buf, 1, r, outf);
		if (w < r) {
			ret = -errno;
			goto err_out;
		}
		size -= r;

		if (r < now)
			goto fill;
	}

	now = fread(buf, 1, 1, f);
	if (now == 1) {
		image_error(image, "input file '%s' too large\n", infile);
		ret = -EINVAL;
		goto err_out;
	}

fill:
	memset(buf, fillpattern, 4096);

	while (size) {
		now = min(size, 4096);

		r = fwrite(buf, 1, now, outf);
		if (r < now) {
			ret = -errno;
			goto err_out;
		}
		size -= now;
	}
err_out:
	free(buf);
	if (f)
		fclose(f);
	if (outf)
		fclose(outf);

	return ret;
}

int insert_data(struct image *image, const char *data, const char *outfile,
		size_t size, long offset)
{
	FILE *outf = NULL;
	int now, r;
	int ret = 0;

	outf = fopen(outfile, "r+");
	if (!outf && errno == ENOENT)
		outf = fopen(outfile, "w");
	if (!outf) {
		image_error(image, "open %s: %s\n", outfile, strerror(errno));
		ret = -errno;
		goto err_out;
	}
	ret = fseek(outf, offset, SEEK_SET);
	if (ret) {
		image_error(image, "seek %s: %s\n", outfile, strerror(errno));
		ret = -errno;
		goto err_out;
	}
	while (size) {
		now = min(size, 4096);

		r = fwrite(data, 1, now, outf);
		if (r < now) {
			ret = -errno;
			goto err_out;
		}
		size -= now;
		data += now;
	}
err_out:
	if (outf)
		fclose(outf);

	return ret;
}
