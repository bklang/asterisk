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
 * \brief SoftHangup application
 *
 * \author Mark Spencer <markster@digium.com>
 * 
 * \ingroup applications
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>

#include "asterisk.h"

ASTERISK_FILE_VERSION(__FILE__, "$Revision$")

#include "asterisk/file.h"
#include "asterisk/logger.h"
#include "asterisk/channel.h"
#include "asterisk/pbx.h"
#include "asterisk/module.h"
#include "asterisk/lock.h"

static char *synopsis = "Soft Hangup Application";

static char *tdesc = "Hangs up the requested channel";

static char *desc = "  SoftHangup(Technology/resource|options)\n"
"Hangs up the requested channel.  If there are no channels to hangup,\n"
"the application will report it.\n"
"- 'options' may contain the following letter:\n"
"     'a' : hang up all channels on a specified device instead of a single resource\n";

static char *app = "SoftHangup";

LOCAL_USER_DECL;

static int softhangup_exec(struct ast_channel *chan, void *data)
{
	struct localuser *u;
	struct ast_channel *c=NULL;
	char *options, *cut, *cdata, *match;
	char name[AST_CHANNEL_NAME] = "";
	int all = 0;
	
	if (ast_strlen_zero(data)) {
                ast_log(LOG_WARNING, "SoftHangup requires an argument (Technology/resource)\n");
		return 0;
	}
	
	LOCAL_USER_ADD(u);

	cdata = ast_strdupa(data);
	match = strsep(&cdata, "|");
	options = strsep(&cdata, "|");
	all = options && strchr(options,'a');
	c = ast_channel_walk_locked(NULL);
	while (c) {
		strncpy(name, c->name, sizeof(name)-1);
		ast_mutex_unlock(&c->lock);
		/* XXX watch out, i think it is wrong to access c-> after unlocking! */
		if (all) {
			/* CAPI is set up like CAPI[foo/bar]/clcnt */ 
			if (!strcmp(c->tech->type, "CAPI")) 
				cut = strrchr(name,'/');
			/* Basically everything else is Foo/Bar-Z */
			else
				cut = strchr(name,'-');
			/* Get rid of what we've cut */
			if (cut)
				*cut = 0;
		}
		if (!strcasecmp(name, match)) {
			ast_log(LOG_WARNING, "Soft hanging %s up.\n",c->name);
			ast_softhangup(c, AST_SOFTHANGUP_EXPLICIT);
			if(!all)
				break;
		}
		c = ast_channel_walk_locked(c);
	}
	
	LOCAL_USER_REMOVE(u);

	return 0;
}

static int unload_module(void *mod)
{
	int res;

	res = ast_unregister_application(app);

	STANDARD_HANGUP_LOCALUSERS;

	return res;	
}

static int load_module(void *mod)
{
	return ast_register_application(app, softhangup_exec, synopsis, desc);
}

static const char *description(void)
{
	return tdesc;
}

static const char *key(void)
{
	return ASTERISK_GPL_KEY;
}

STD_MOD1;
