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
 * \brief App to transmit a text message
 *
 * \author Mark Spencer <markster@digium.com>
 * 
 * \note Requires support of sending text messages from channel driver
 *
 * \ingroup applications
 */
 
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "asterisk.h"

ASTERISK_FILE_VERSION(__FILE__, "$Revision$")

#include "asterisk/lock.h"
#include "asterisk/file.h"
#include "asterisk/logger.h"
#include "asterisk/channel.h"
#include "asterisk/pbx.h"
#include "asterisk/module.h"
#include "asterisk/translate.h"
#include "asterisk/image.h"
#include "asterisk/options.h"
#include "asterisk/app.h"

static const char *app = "SendText";

static const char *synopsis = "Send a Text Message";

static const char *descrip = 
"  SendText(text[|options]): Sends text to current channel (callee).\n"
"Result of transmission will be stored in the SENDTEXTSTATUS\n"
"channel variable:\n"
"      SUCCESS      Transmission succeeded\n"
"      FAILURE      Transmission failed\n"
"      UNSUPPORTED  Text transmission not supported by channel\n"
"\n"
"At this moment, text is supposed to be 7 bit ASCII in most channels.\n"
"The option string many contain the following character:\n"
"'j' -- jump to n+101 priority if the channel doesn't support\n"
"       text transport\n";

LOCAL_USER_DECL;

static int sendtext_exec(struct ast_channel *chan, void *data)
{
	int res = 0;
	struct localuser *u;
	char *status = "UNSUPPORTED";
	char *parse = NULL;
	int priority_jump = 0;
	AST_DECLARE_APP_ARGS(args,
		AST_APP_ARG(text);
		AST_APP_ARG(options);
	);
		
	LOCAL_USER_ADD(u);	

	if (ast_strlen_zero(data)) {
		ast_log(LOG_WARNING, "SendText requires an argument (text[|options])\n");
		LOCAL_USER_REMOVE(u);
		return -1;
	} else {
		if (!(parse = ast_strdupa(data))) {
			LOCAL_USER_REMOVE(u);
			return -1;
		}
	}
	
	AST_STANDARD_APP_ARGS(args, parse);

	if (args.options) {
		if (strchr(args.options, 'j'))
			priority_jump = 1;
	}

	ast_mutex_lock(&chan->lock);
	if (!chan->tech->send_text) {
		ast_mutex_unlock(&chan->lock);
		/* Does not support transport */
		if (priority_jump || ast_opt_priority_jumping)
			ast_goto_if_exists(chan, chan->context, chan->exten, chan->priority + 101);
		LOCAL_USER_REMOVE(u);
		return 0;
	}
	status = "FAILURE";
	ast_mutex_unlock(&chan->lock);
	res = ast_sendtext(chan, args.text);
	if (!res)
		status = "SUCCESS";
	pbx_builtin_setvar_helper(chan, "SENDTEXTSTATUS", status);
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
	return ast_register_application(app, sendtext_exec, synopsis, descrip);
}

static const char *description(void)
{
	return "Send Text Applications";
}

static const char *key(void)
{
	return ASTERISK_GPL_KEY;
}

STD_MOD1;
