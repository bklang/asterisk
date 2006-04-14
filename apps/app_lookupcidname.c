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
 * \brief App to set callerid name from database, based on directory number
 *
 * \author Mark Spencer <markster@digium.com>
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
#include "asterisk/options.h"
#include "asterisk/channel.h"
#include "asterisk/pbx.h"
#include "asterisk/module.h"
#include "asterisk/translate.h"
#include "asterisk/image.h"
#include "asterisk/callerid.h"
#include "asterisk/astdb.h"

static char *tdesc = "Look up CallerID Name from local database";

static char *app = "LookupCIDName";

static char *synopsis = "Look up CallerID Name from local database";

static char *descrip =
  "  LookupCIDName: Looks up the Caller*ID number on the active\n"
  "channel in the Asterisk database (family 'cidname') and sets the\n"
  "Caller*ID name.  Does nothing if no Caller*ID was received on the\n"
  "channel.  This is useful if you do not subscribe to Caller*ID\n"
  "name delivery, or if you want to change the names on some incoming\n"
  "calls.\n";

LOCAL_USER_DECL;

static int
lookupcidname_exec (struct ast_channel *chan, void *data)
{
  char dbname[64];
  struct localuser *u;

  LOCAL_USER_ADD (u);
  if (chan->cid.cid_num) {
	if (!ast_db_get ("cidname", chan->cid.cid_num, dbname, sizeof (dbname))) {
		ast_set_callerid (chan, NULL, dbname, NULL);
		  if (option_verbose > 2)
		    ast_verbose (VERBOSE_PREFIX_3 "Changed Caller*ID name to %s\n",
				 dbname);
	}
  }
  LOCAL_USER_REMOVE (u);
  return 0;
}

static int unload_module(void *mod)
{
	int res;

	res = ast_unregister_application (app);

	STANDARD_HANGUP_LOCALUSERS;

	return res;
}

static int load_module(void *mod)
{
	return ast_register_application (app, lookupcidname_exec, synopsis, descrip);
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
