/*
 * Asterisk -- An open source telephony toolkit.
 *
 * Copyright (C) 2005, Joshua Colp
 *
 * Joshua Colp <jcolp@digium.com>
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
 * \brief Directed Call Pickup Support
 *
 * \author Joshua Colp <jcolp@asterlink.com>
 *
 * \ingroup applications
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "asterisk.h"

ASTERISK_FILE_VERSION(__FILE__, "$Revision$")

#include "asterisk/file.h"
#include "asterisk/logger.h"
#include "asterisk/channel.h"
#include "asterisk/pbx.h"
#include "asterisk/module.h"
#include "asterisk/lock.h"
#include "asterisk/app.h"

static const char *app = "Pickup";
static const char *synopsis = "Directed Call Pickup";
static const char *descrip =
"  Pickup(extension[@context][&extension2@context...]): This application can pickup any ringing channel\n"
"that is calling the specified extension. If no context is specified, the current\n"
"context will be used.\n";

LOCAL_USER_DECL;

static int pickup_exec(struct ast_channel *chan, void *data)
{
	int res = 0;
	struct localuser *u = NULL;
	struct ast_channel *origin = NULL, *target = NULL;
	char *tmp = NULL, *exten = NULL, *context = NULL, *rest=data;
	char workspace[256] = "";

	if (ast_strlen_zero(data)) {
		ast_log(LOG_WARNING, "Pickup requires an argument (extension) !\n");
		return -1;	
	}

	LOCAL_USER_ADD(u);
	
	while (!target && (exten = rest) ) {
		res = 0;
		rest = strchr(exten, '&');
		if (rest)
			*rest++ = 0;

		/* Get the extension and context if present */
		context = strchr(exten, '@');
		if (context)
			*context++ = '\0';

		/* Find a channel to pickup */
		origin = ast_get_channel_by_exten_locked(exten, context);
		if (origin) {
			ast_cdr_getvar(origin->cdr, "dstchannel", &tmp, workspace,
					sizeof(workspace), 0, 0);
			if (tmp) {
				/* We have a possible channel... now we need to find it! */
				target = ast_get_channel_by_name_locked(tmp);
			} else {
				ast_log(LOG_NOTICE, "No target channel found for %s.\n", exten);
				res = -1;
			}
			ast_mutex_unlock(&origin->lock);

		} else {
			ast_log(LOG_DEBUG, "No originating channel found.\n");
		}

		if (res)
			continue;

		if (target && (!target->pbx) && ((target->_state == AST_STATE_RINGING) || (target->_state == AST_STATE_RING) ) ) {
			ast_log(LOG_DEBUG, "Call pickup on chan '%s' by '%s'\n", target->name,
					chan->name);
			res = ast_answer(chan);
			if (res) {
				ast_log(LOG_WARNING, "Unable to answer '%s'\n", chan->name);
				res = -1;
				break;
			}
			res = ast_queue_control(chan, AST_CONTROL_ANSWER);
			if (res) {
				ast_log(LOG_WARNING, "Unable to queue answer on '%s'\n",
						chan->name);
				res = -1;
				break;
			}
			res = ast_channel_masquerade(target, chan);
			if (res) {
				ast_log(LOG_WARNING, "Unable to masquerade '%s' into '%s'\n", chan->name, target->name);
				res = -1;
				break;
			}
		} else {
			ast_log(LOG_NOTICE, "No call pickup possible for %s...\n", exten);
			res = -1;
		}
	}
	if (target) 
		ast_mutex_unlock(&target->lock);
	
	LOCAL_USER_REMOVE(u);

	return res;
}

static int unload_module(void *mod)
{
	int res;

	res = ast_unregister_application(app);
	
	return res;
}

static int load_module(void *mod)
{
	__mod_desc = mod;
	return ast_register_application(app, pickup_exec, synopsis, descrip);
}

static const char *description(void)
{
	return "Directed Call Pickup Application";
}

static const char *key(void)
{
	return ASTERISK_GPL_KEY;
}

STD_MOD(MOD_1, NULL, NULL, NULL);
