/*
 * Asterisk -- A telephony toolkit for Linux.
 *
 * Exec application
 *
 * Copyright (c) 2004 - 2005, Tilghman Lesher.  All rights reserved.
 *
 * Tilghman Lesher <app_exec__v001@the-tilghman.com>
 *
 * $Id$
 *
 * This code is released by the author with no restrictions on usage.
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <asterisk/file.h>
#include <asterisk/logger.h>
#include <asterisk/options.h>
#include <asterisk/channel.h>
#include <asterisk/pbx.h>
#include <asterisk/module.h>

/* Maximum length of any variable */
#define MAXRESULT	1024

static char *tdesc = "Executes applications";

static char *app_exec = "Exec";

static char *exec_synopsis = "Exec(Appname(arguments))";

static char *exec_descrip =
"Exec(appname(arguments))\n"
"  Allows an arbitrary application to be invoked even when not\n"
"hardcoded into the dialplan.  Returns whatever value the\n"
"app returns or -2 when the app cannot be found.\n";

STANDARD_LOCAL_USER;

LOCAL_USER_DECL;

static int exec_exec(struct ast_channel *chan, void *data)
{
	int res=0;
	struct localuser *u;
	char *s, *appname, *endargs, args[MAXRESULT];
	struct ast_app *app;

	LOCAL_USER_ADD(u);

	memset(args, 0, MAXRESULT);

	/* Check and parse arguments */
	if (data) {
		s = ast_strdupa((char *)data);
		if (s) {
			appname = strsep(&s, "(");
			if (s) {
				endargs = strrchr(s, ')');
				if (endargs)
					*endargs = '\0';
				pbx_substitute_variables_helper(chan, s, args, MAXRESULT - 1);
			}
			if (appname) {
				app = pbx_findapp(appname);
				if (app) {
					res = pbx_exec(chan, app, args, 1);
				} else {
					ast_log(LOG_WARNING, "Could not find application (%s)\n", appname);
					res = -2;
				}
			}
		} else {
			ast_log(LOG_ERROR, "Out of memory\n");
			res = -1;
		}
	}

	LOCAL_USER_REMOVE(u);
	return res;
}

int unload_module(void)
{
	STANDARD_HANGUP_LOCALUSERS;
	return ast_unregister_application(app_exec);
}

int load_module(void)
{
	return ast_register_application(app_exec, exec_exec, exec_synopsis, exec_descrip);
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
