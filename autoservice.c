/*
 * Asterisk -- A telephony toolkit for Linux.
 *
 * Automatic channel service routines
 * 
 * Copyright (C) 1999, Mark Spencer
 *
 * Mark Spencer <markster@linux-support.net>
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License
 */

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <string.h>
#include <sys/time.h>
#include <signal.h>
#include <errno.h>
#include <unistd.h>
#include <math.h>			/* For PI */
#include <asterisk/pbx.h>
#include <asterisk/frame.h>
#include <asterisk/sched.h>
#include <asterisk/options.h>
#include <asterisk/channel.h>
#include <asterisk/channel_pvt.h>
#include <asterisk/logger.h>
#include <asterisk/file.h>
#include <asterisk/translate.h>
#include <asterisk/manager.h>
#include <asterisk/chanvars.h>
#include <asterisk/linkedlists.h>
#include <asterisk/indications.h>

#define MAX_AUTOMONS 256

static ast_mutex_t autolock = AST_MUTEX_INITIALIZER;

struct asent {
	struct ast_channel *chan;
	struct asent *next;
};

static struct asent *aslist = NULL;
static pthread_t asthread = (pthread_t) -1;

static void *autoservice_run(void *ign)
{
	struct ast_channel *mons[MAX_AUTOMONS];
	int x;
	int ms;
	struct ast_channel *chan;
	struct asent *as;
	struct ast_frame *f;
	for(;;) {
		x = 0;
		ast_mutex_lock(&autolock);
		as = aslist;
		while(as) {
			if (!as->chan->_softhangup) {
				if (x < MAX_AUTOMONS)
					mons[x++] = as->chan;
				else
					ast_log(LOG_WARNING, "Exceeded maximum number of automatic monitoring events.  Fix autoservice.c\n");
			}
			as = as->next;
		}
		ast_mutex_unlock(&autolock);

/* 		if (!aslist)
			break; */
		ms = 500;
		chan = ast_waitfor_n(mons, x, &ms);
		if (chan) {
			/* Read and ignore anything that occurs */
			f = ast_read(chan);
			if (f)
				ast_frfree(f);
		}
	}
	asthread = (pthread_t) -1;
	return NULL;
}

int ast_autoservice_start(struct ast_channel *chan)
{
	int res = -1;
	struct asent *as;
	int needstart;
	ast_mutex_lock(&autolock);
	needstart = (asthread == (pthread_t) -1) ? 1 : 0 /* aslist ? 0 : 1 */;
	as = aslist;
	while(as) {
		if (as->chan == chan)
			break;
		as = as->next;
	}
	if (!as) {
		as = malloc(sizeof(struct asent));
		if (as) {
			memset(as, 0, sizeof(struct asent));
			as->chan = chan;
			as->next = aslist;
			aslist = as;
			res = 0;
			if (needstart) {
				if (pthread_create(&asthread, NULL, autoservice_run, NULL)) {
					ast_log(LOG_WARNING, "Unable to create autoservice thread :(\n");
					free(aslist);
					aslist = NULL;
					res = -1;
				} else
					pthread_kill(asthread, SIGURG);
			}
		}
	}
	ast_mutex_unlock(&autolock);
	return res;
}

int ast_autoservice_stop(struct ast_channel *chan)
{
	int res = -1;
	struct asent *as, *prev;
	ast_mutex_lock(&autolock);
	as = aslist;
	prev = NULL;
	while(as) {
		if (as->chan == chan)
			break;
		prev = as;
		as = as->next;
	}
	if (as) {
		if (prev)
			prev->next = as->next;
		else
			aslist = as->next;
		free(as);
		if (!chan->_softhangup)
			res = 0;
	}
	if (asthread != (pthread_t) -1) 
		pthread_kill(asthread, SIGURG);
	ast_mutex_unlock(&autolock);
	/* Wait for it to un-block */
	while(chan->blocking)
		usleep(1000);
	return res;
}
