/*
 * Asterisk -- A telephony toolkit for Linux.
 *
 * Silly application to play an NBScat file -- uses nbscat8k
 * 
 * Copyright (C) 1999, Mark Spencer
 *
 * Mark Spencer <markster@linux-support.net>
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License
 */
 
#include <asterisk/lock.h>
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
#include <sys/time.h>
#include <sys/socket.h>

#define LOCAL_NBSCAT "/usr/local/bin/nbscat8k"
#define NBSCAT "/usr/bin/nbscat8k"

#ifndef AF_LOCAL
#define AF_LOCAL AF_UNIX
#endif

static char *tdesc = "Silly NBS Stream Application";

static char *app = "NBScat";

static char *synopsis = "Play an NBS local stream";

static char *descrip = 
"  NBScat: Executes nbscat to listen to the local NBS stream.\n"
"Returns  -1  on\n hangup or 0 otherwise. User can exit by \n"
"pressing any key\n.";

STANDARD_LOCAL_USER;

LOCAL_USER_DECL;

static int NBScatplay(int fd)
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
	/* Most commonly installed in /usr/local/bin */
	execl(NBSCAT, "nbscat8k", "-d", (char *)NULL);
	execl(LOCAL_NBSCAT, "nbscat8k", "-d", (char *)NULL);
	ast_log(LOG_WARNING, "Execute of nbscat8k failed\n");
	return -1;
}

static int timed_read(int fd, void *data, int datalen)
{
	int res;
	struct pollfd fds[1];
	fds[0].fd = fd;
	fds[0].events = POLLIN;
	res = poll(fds, 1, 2000);
	if (res < 1) {
		ast_log(LOG_NOTICE, "Selected timed out/errored out with %d\n", res);
		return -1;
	}
	return read(fd, data, datalen);
	
}

static int NBScat_exec(struct ast_channel *chan, void *data)
{
	int res=0;
	struct localuser *u;
	int fds[2];
	int ms = -1;
	int pid = -1;
	int owriteformat;
	struct timeval now, next;
	struct ast_frame *f;
	struct myframe {
		struct ast_frame f;
		char offset[AST_FRIENDLY_OFFSET];
		short frdata[160];
	} myf;
	if (socketpair(AF_LOCAL, SOCK_STREAM, 0, fds)) {
		ast_log(LOG_WARNING, "Unable to create socketpair\n");
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
	
	res = NBScatplay(fds[1]);
	/* Wait 1000 ms first */
	next = now;
	next.tv_sec += 1;
	if (res >= 0) {
		pid = res;
		/* Order is important -- there's almost always going to be mp3...  we want to prioritize the
		   user */
		for (;;) {
			gettimeofday(&now, NULL);
			ms = (next.tv_sec - now.tv_sec) * 1000;
			ms += (next.tv_usec - now.tv_usec) / 1000;
#if 0
			printf("ms: %d\n", ms);
#endif			
			if (ms <= 0) {
#if 0
				{
					static struct timeval last;
					struct timeval tv;
					gettimeofday(&tv, NULL);
					printf("Since last: %ld\n", (tv.tv_sec - last.tv_sec) * 1000 + (tv.tv_usec - last.tv_usec) / 1000);
					last = tv;
				}
#endif
				res = timed_read(fds[0], myf.frdata, sizeof(myf.frdata));
				if (res > 0) {
					myf.f.frametype = AST_FRAME_VOICE;
					myf.f.subclass = AST_FORMAT_SLINEAR;
					myf.f.datalen = res;
					myf.f.samples = res / 2;
					myf.f.mallocd = 0;
					myf.f.offset = AST_FRIENDLY_OFFSET;
					myf.f.src = __PRETTY_FUNCTION__;
					myf.f.delivery.tv_sec = 0;
					myf.f.delivery.tv_usec = 0;
					myf.f.data = myf.frdata;
					if (ast_write(chan, &myf.f) < 0) {
						res = -1;
						break;
					}
				} else {
					ast_log(LOG_DEBUG, "No more mp3\n");
					res = 0;
					break;
				}
				next.tv_usec += res / 2 * 125;
				if (next.tv_usec >= 1000000) {
					next.tv_usec -= 1000000;
					next.tv_sec++;
				}
#if 0
				printf("Next: %d\n", ms);
#endif				
			} else {
				ms = ast_waitfor(chan, ms);
				if (ms < 0) {
					ast_log(LOG_DEBUG, "Hangup detected\n");
					res = -1;
					break;
				}
				if (ms) {
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
				} 
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
	return ast_register_application(app, NBScat_exec, synopsis, descrip);
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
