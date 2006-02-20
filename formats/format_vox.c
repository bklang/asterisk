/*
 * Asterisk -- An open source telephony toolkit.
 *
 * Copyright (C) 1999 - 2005, Digium, Inc.
 *
 * Mark Spencer <markster@digium.com>
 *
 * See http://www.asterisk.org for more information about
 * the Asterisk project. Please do not directly contact
 * any of the maintainers of this project for assistance;
 * the project provides a web site, mailing lists and IRC
 * channels for your use.
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License Version 2. See the LICENSE file
 * at the top of the source tree.
 */

/*! \file
 *
 * \brief Flat, binary, ADPCM vox file format.
 * \arg File name extensions: vox
 * 
 * \ingroup formats
 */
 
#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <sys/time.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>

#include "asterisk.h"

ASTERISK_FILE_VERSION(__FILE__, "$Revision$")

#include "asterisk/lock.h"
#include "asterisk/channel.h"
#include "asterisk/file.h"
#include "asterisk/logger.h"
#include "asterisk/sched.h"
#include "asterisk/module.h"
#include "asterisk/endian.h"

#define BUF_SIZE 80		/* 160 samples */

struct ast_filestream {
	void *reserved[AST_RESERVED_POINTERS];
	/* This is what a filestream means to us */
	FILE *f; /* Descriptor */
	struct ast_frame fr;				/* Frame information */
	char waste[AST_FRIENDLY_OFFSET];	/* Buffer for sending frames, etc */
	char empty;							/* Empty character */
	unsigned char buf[BUF_SIZE];	/* Output Buffer */
	int lasttimeout;
	struct timeval last;
	short signal;						/* Signal level (file side) */
	short ssindex;						/* Signal ssindex (file side) */
	unsigned char zero_count;				/* counter of consecutive zero samples */
	unsigned char next_flag;
};


AST_MUTEX_DEFINE_STATIC(vox_lock);
static int glistcnt = 0;

static char *name = "vox";
static char *desc = "Dialogic VOX (ADPCM) File Format";
static char *exts = "vox";

static struct ast_filestream *vox_open(FILE *f)
{
	/* We don't have any header to read or anything really, but
	   if we did, it would go here.  We also might want to check
	   and be sure it's a valid file.  */
	struct ast_filestream *tmp;
	if ((tmp = malloc(sizeof(struct ast_filestream)))) {
		memset(tmp, 0, sizeof(struct ast_filestream));
		if (ast_mutex_lock(&vox_lock)) {
			ast_log(LOG_WARNING, "Unable to lock vox list\n");
			free(tmp);
			return NULL;
		}
		tmp->f = f;
		tmp->fr.data = tmp->buf;
		tmp->fr.frametype = AST_FRAME_VOICE;
		tmp->fr.subclass = AST_FORMAT_ADPCM;
		/* datalen will vary for each frame */
		tmp->fr.src = name;
		tmp->fr.mallocd = 0;
		tmp->lasttimeout = -1;
		glistcnt++;
		ast_mutex_unlock(&vox_lock);
		ast_update_use_count();
	}
	return tmp;
}

static struct ast_filestream *vox_rewrite(FILE *f, const char *comment)
{
	/* We don't have any header to read or anything really, but
	   if we did, it would go here.  We also might want to check
	   and be sure it's a valid file.  */
	struct ast_filestream *tmp;
	if ((tmp = malloc(sizeof(struct ast_filestream)))) {
		memset(tmp, 0, sizeof(struct ast_filestream));
		if (ast_mutex_lock(&vox_lock)) {
			ast_log(LOG_WARNING, "Unable to lock vox list\n");
			free(tmp);
			return NULL;
		}
		tmp->f = f;
		glistcnt++;
		ast_mutex_unlock(&vox_lock);
		ast_update_use_count();
	} else
		ast_log(LOG_WARNING, "Out of memory\n");
	return tmp;
}

static void vox_close(struct ast_filestream *s)
{
	if (ast_mutex_lock(&vox_lock)) {
		ast_log(LOG_WARNING, "Unable to lock vox list\n");
		return;
	}
	glistcnt--;
	ast_mutex_unlock(&vox_lock);
	ast_update_use_count();
	fclose(s->f);
	free(s);
	s = NULL;
}

static struct ast_frame *vox_read(struct ast_filestream *s, int *whennext)
{
	int res;
	/* Send a frame from the file to the appropriate channel */
	s->fr.frametype = AST_FRAME_VOICE;
	s->fr.subclass = AST_FORMAT_ADPCM;
	s->fr.offset = AST_FRIENDLY_OFFSET;
	s->fr.mallocd = 0;
	s->fr.data = s->buf;
	if ((res = fread(s->buf, 1, BUF_SIZE, s->f)) < 1) {
		if (res)
			ast_log(LOG_WARNING, "Short read (%d) (%s)!\n", res, strerror(errno));
		return NULL;
	}
	s->fr.samples = res * 2;
	s->fr.datalen = res;
	*whennext = s->fr.samples;
	return &s->fr;
}

static int vox_write(struct ast_filestream *fs, struct ast_frame *f)
{
	int res;
	if (f->frametype != AST_FRAME_VOICE) {
		ast_log(LOG_WARNING, "Asked to write non-voice frame!\n");
		return -1;
	}
	if (f->subclass != AST_FORMAT_ADPCM) {
		ast_log(LOG_WARNING, "Asked to write non-ADPCM frame (%d)!\n", f->subclass);
		return -1;
	}
	if ((res = fwrite(f->data, 1, f->datalen, fs->f)) != f->datalen) {
			ast_log(LOG_WARNING, "Bad write (%d/%d): %s\n", res, f->datalen, strerror(errno));
			return -1;
	}
	return 0;
}

static char *vox_getcomment(struct ast_filestream *s)
{
	return NULL;
}

static int vox_seek(struct ast_filestream *fs, off_t sample_offset, int whence)
{
     off_t offset=0,min,cur,max,distance;
	
     min = 0;
     cur = ftello(fs->f);
     fseeko(fs->f, 0, SEEK_END);
	 max = ftello(fs->f);
	 
     /* have to fudge to frame here, so not fully to sample */
     distance = sample_offset/2;
     if(whence == SEEK_SET)
	  offset = distance;
     else if(whence == SEEK_CUR || whence == SEEK_FORCECUR)
	  offset = distance + cur;
     else if(whence == SEEK_END)
	  offset = max - distance;
     if (whence != SEEK_FORCECUR) {
	  offset = (offset > max)?max:offset;
	  offset = (offset < min)?min:offset;
     }
     fseeko(fs->f, offset, SEEK_SET);
	 return ftello(fs->f);
}

static int vox_trunc(struct ast_filestream *fs)
{
     return ftruncate(fileno(fs->f), ftello(fs->f));
}

static off_t vox_tell(struct ast_filestream *fs)
{
     off_t offset;
     offset = ftello(fs->f) << 1;
     return offset; 
}

int load_module()
{
	return ast_format_register(name, exts, AST_FORMAT_ADPCM,
								vox_open,
								vox_rewrite,
								vox_write,
								vox_seek,
								vox_trunc,
								vox_tell,
								vox_read,
								vox_close,
								vox_getcomment);
								
								
}

int unload_module()
{
	return ast_format_unregister(name);
}	

int usecount()
{
	return glistcnt;
}

char *description()
{
	return desc;
}


char *key()
{
	return ASTERISK_GPL_KEY;
}
