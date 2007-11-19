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
 * \brief Playback the special information tone to get rid of telemarketers
 *
 * \author Mark Spencer <markster@digium.com>
 * 
 * \ingroup applications
 */
 
#include "asterisk.h"

ASTERISK_FILE_VERSION(__FILE__, "$Revision$")

#include "asterisk/lock.h"
#include "asterisk/file.h"
#include "asterisk/channel.h"
#include "asterisk/pbx.h"
#include "asterisk/module.h"
#include "asterisk/translate.h"
#include "asterisk/app.h"

static char *app = "Zapateller";

static char *synopsis = "Block telemarketers with SIT";

static char *descrip = 
"  Zapateller(options):  Generates special information tone to block\n"
"telemarketers from calling you.  Options is a pipe-delimited list of\n" 
"options.  The following options are available:\n"
"    'answer'     - causes the line to be answered before playing the tone,\n" 
"    'nocallerid' - causes Zapateller to only play the tone if there is no\n"
"                   callerid information available.  Options should be\n"
"                   separated by , characters\n\n"
"  This application will set the following channel variable upon completion:\n"
"    ZAPATELLERSTATUS - This will contain the last action accomplished by the\n"
"                        Zapateller application. Possible values include:\n"
"                        NOTHING | ANSWERED | ZAPPED\n\n";


static int zapateller_exec(struct ast_channel *chan, void *data)
{
	int res = 0;
	int i, answer = 0, nocallerid = 0;
	char *parse = ast_strdupa((char *)data);
	AST_DECLARE_APP_ARGS(args,
		AST_APP_ARG(options)[2];
	);

	AST_STANDARD_APP_ARGS(args, parse);

	for (i = 0; i < args.argc; i++) {
		if (!strcasecmp(args.options[i], "answer"))
			answer = 1;
		else if (!strcasecmp(args.options[i], "nocallerid"))
			nocallerid = 1;
	}

	pbx_builtin_setvar_helper(chan, "ZAPATELLERSTATUS", "NOTHING");
	ast_stopstream(chan);
	if (chan->_state != AST_STATE_UP) {
		if (answer) {
			res = ast_answer(chan);
			pbx_builtin_setvar_helper(chan, "ZAPATELLERSTATUS", "ANSWERED");
		}
		if (!res)
			res = ast_safe_sleep(chan, 500);
	}

	if (!ast_strlen_zero(chan->cid.cid_num) && nocallerid)
		return res;

	if (!res) 
		res = ast_tonepair(chan, 950, 0, 330, 0);
	if (!res) 
		res = ast_tonepair(chan, 1400, 0, 330, 0);
	if (!res) 
		res = ast_tonepair(chan, 1800, 0, 330, 0);
	if (!res) 
		res = ast_tonepair(chan, 0, 0, 1000, 0);
	
	pbx_builtin_setvar_helper(chan, "ZAPATELLERSTATUS", "ZAPPED");
	return res;
}

static int unload_module(void)
{
	return ast_unregister_application(app);
}

static int load_module(void)
{
	return ast_register_application(app, zapateller_exec, synopsis, descrip);
}

AST_MODULE_INFO_STANDARD(ASTERISK_GPL_KEY, "Block Telemarketers with Special Information Tone");
