/*
 * Asterisk -- An open source telephony toolkit.
 *
 * Copyright (C) 1999 - 2005, Digium, Inc.
 *
 * Martin Pycko <martinp@digium.com>
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
 * \brief Applications connected with CDR engine
 *
 * Martin Pycko <martinp@digium.com>
 *
 * \ingroup applications
 */

#include <sys/types.h>
#include <stdlib.h>

#define STATIC_MODULE

#include "asterisk.h"

ASTERISK_FILE_VERSION(__FILE__, "$Revision$")

#include "asterisk/channel.h"
#include "asterisk/module.h"
#include "asterisk/pbx.h"

static char *nocdr_descrip = 
"  NoCDR(): This application will tell Asterisk not to maintain a CDR for the\n"
"current call.\n";

static char *nocdr_app = "NoCDR";
static char *nocdr_synopsis = "Tell Asterisk to not maintain a CDR for the current call";

LOCAL_USER_DECL;

static int nocdr_exec(struct ast_channel *chan, void *data)
{
	struct localuser *u;
	
	LOCAL_USER_ADD(u);

	if (chan->cdr) {
		ast_cdr_free(chan->cdr);
		chan->cdr = NULL;
	}

	LOCAL_USER_REMOVE(u);

	return 0;
}

STATIC_MODULE int unload_module(void)
{
	int res;

	res = ast_unregister_application(nocdr_app);

	STANDARD_HANGUP_LOCALUSERS;

	return res;
}

STATIC_MODULE int load_module(void)
{
	return ast_register_application(nocdr_app, nocdr_exec, nocdr_synopsis, nocdr_descrip);
}

STATIC_MODULE char *description(void)
{
	return "Tell Asterisk to not maintain a CDR for the current call";
}

STATIC_MODULE int usecount(void)
{
	int res;
	STANDARD_USECOUNT(res);
	return res;
}

STATIC_MODULE char *key(void)
{
	return ASTERISK_GPL_KEY;
}

STD_MOD(MOD_1, NULL, NULL, NULL);
