/*
 * Asterisk -- A telephony toolkit for Linux.
 *
 * Silly application to play an MP3 file -- uses mpg123
 * 
 * Copyright (C) 1999, Mark Spencer
 *
 * Mark Spencer <markster@linux-support.net>
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License
 */
 
#include <asterisk/file.h>
#include <asterisk/logger.h>
#include <asterisk/channel.h>
#include <asterisk/frame.h>
#include <asterisk/pbx.h>
#include <asterisk/module.h>
#include <asterisk/translate.h>
#include <string.h>
#include <stdio.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/time.h>

#define MPG_123 "/usr/bin/mpg123"

static char *tdesc = "Silly MP3 Application";

static char *app = "MP3Player";

static char *synopsis = "Play an MP3 file or stream";

static char *descrip = 
"  MP3Player(location) Executes mpg123 to play the given location\n"
"which typically would be a  filename  or  a URL. Returns  -1  on\n"
"hangup or 0 otherwise. User can exit by pressing any key\n.";

STANDARD_LOCAL_USER;

LOCAL_USER_DECL;

static int mp3play(char *filename, int fd)
{
	int res;
	int x;
	res = fork();
	if (res < 0) 
		ast_log(LOG_WARNING, "Fork failed\n");
	if (res)
		return res;
	dup2(fd, STDOUT_FILENO);
	for (x=0;x<256;x++) {
		if (x != STDOUT_FILENO)
			close(x);
	}
	/* Execute mpg123, but buffer if it's a net connection */
	if (strncmp(filename, "http://", 7)) 
	    execl(MPG_123, MPG_123, "-q", "-s", "-b", "1024", "--mono", "-r", "8000", filename, NULL);
	else
	    execl(MPG_123, MPG_123, "-q", "-s", "--mono", "-r", "8000", filename, NULL);
	ast_log(LOG_WARNING, "Execute of mpg123 failed\n");
	return -1;
}

static int mp3_exec(struct ast_channel *chan, void *data)
{
	int res=0;
	struct localuser *u;
	int fds[2];
	int rfds[1 + AST_MAX_FDS];
	int ms = -1;
	int pid = -1;
	int us;
	int exception;
	int owriteformat;
	struct timeval tv;
	struct timeval last;
	struct ast_frame *f;
	int x;
	struct myframe {
		struct ast_frame f;
		char offset[AST_FRIENDLY_OFFSET];
		char frdata[160];
	} myf;
	last.tv_usec = 0;
	last.tv_sec = 0;
	if (!data) {
		ast_log(LOG_WARNING, "MP3 Playback requires an argument (filename)\n");
		return -1;
	}
	if (pipe(fds)) {
		ast_log(LOG_WARNING, "Unable to create pipe\n");
		return -1;
	}
	LOCAL_USER_ADD(u);
	ast_stopstream(chan);

	owriteformat = chan->writeformat;
	res = ast_set_write_format(chan, AST_FORMAT_SLINEAR);
	if (res < 0) {
		ast_log(LOG_WARNING, "Unable to set write format to signed linear\n");
		return -1;
	}
	
	res = mp3play((char *)data, fds[1]);
	if (res >= 0) {
		pid = res;
		/* Order is important -- there's almost always going to be mp3...  we want to prioritize the
		   user */
		rfds[AST_MAX_FDS] = fds[0];
		for (;;) {
			CHECK_BLOCKING(chan);
			for (x=0;x<AST_MAX_FDS;x++) 
				rfds[x] = chan->fds[x];
			res = ast_waitfor_n_fd(rfds, AST_MAX_FDS+1, &ms, &exception);
			chan->blocking = 0;
			if (res < 1) {
				ast_log(LOG_DEBUG, "Hangup detected\n");
				res = -1;
				break;
			}
			for(x=0;x<AST_MAX_FDS;x++) 
				if (res == chan->fds[x])
					break;

			if (x < AST_MAX_FDS) {
				if (exception)
					chan->exception = 1;
				f = ast_read(chan);
				if (!f) {
					ast_log(LOG_DEBUG, "Null frame == hangup() detected\n");
					res = -1;
					break;
				}
				if (f->frametype == AST_FRAME_DTMF) {
					ast_log(LOG_DEBUG, "User pressed a key\n");
					ast_frfree(f);
					res = 0;
					break;
				}
				ast_frfree(f);
			} else if (res == fds[0]) {
				gettimeofday(&tv, NULL);
				if (last.tv_sec || last.tv_usec) {
					/* We should wait at least a frame length */
					us = sizeof(myf.frdata) / 16 * 1000;
					/* Subtract 1,000,000 us for each second late we've passed */
					us -= (tv.tv_sec - last.tv_sec) * 1000000;
					/* And one for each us late we've passed */
					us -= (tv.tv_usec - last.tv_usec);
					/* Sleep that long if needed */
					if (us > 0)
						usleep(us);
				}
				last = tv;
				res = read(fds[0], myf.frdata, sizeof(myf.frdata));
				if (res > 0) {
					myf.f.frametype = AST_FRAME_VOICE;
					myf.f.subclass = AST_FORMAT_SLINEAR;
					myf.f.datalen = res;
					myf.f.timelen = res / 16;
					myf.f.mallocd = 0;
					myf.f.offset = AST_FRIENDLY_OFFSET;
					myf.f.src = __PRETTY_FUNCTION__;
					myf.f.data = myf.frdata;
					if (ast_write(chan, &myf.f) < 0) {
						res = -1;
						break;
					}
				} else {
					ast_log(LOG_DEBUG, "No more mp3\n");
					res = 0;
				}
			} else {
				ast_log(LOG_DEBUG, "HuhHHH?\n");
				res = -1;
				break;
			}
		}
	}
	close(fds[0]);
	close(fds[1]);
	LOCAL_USER_REMOVE(u);
	if (pid > -1)
		kill(pid, SIGKILL);
	if (!res && owriteformat)
		ast_set_write_format(chan, owriteformat);
	return res;
}

int unload_module(void)
{
	STANDARD_HANGUP_LOCALUSERS;
	return ast_unregister_application(app);
}

int load_module(void)
{
	return ast_register_application(app, mp3_exec, synopsis, descrip);
}

char *description(void)
{
	return tdesc;
}

int usecount(void)
{
	int res;
	STANDARD_USECOUNT(res);
	return res;
}

char *key()
{
	return ASTERISK_GPL_KEY;
}
