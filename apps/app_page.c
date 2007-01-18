/*
 * Asterisk -- An open source telephony toolkit.
 *
 * Copyright (c) 2004 - 2006 Digium, Inc.  All rights reserved.
 *
 * Mark Spencer <markster@digium.com>
 *
 * This code is released under the GNU General Public License
 * version 2.0.  See LICENSE for more information.
 *
 * See http://www.asterisk.org for more information about
 * the Asterisk project. Please do not directly contact
 * any of the maintainers of this project for assistance;
 * the project provides a web site, mailing lists and IRC
 * channels for your use.
 *
 */

/*! \file
 *
 * \brief page() - Paging application
 *
 * \author Mark Spencer <markster@digium.com>
 *
 * \ingroup applications
 */

/*** MODULEINFO
	<depend>zaptel</depend>
 ***/

#include "asterisk.h"

ASTERISK_FILE_VERSION(__FILE__, "$Revision$")

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

#include "asterisk/options.h"
#include "asterisk/logger.h"
#include "asterisk/channel.h"
#include "asterisk/pbx.h"
#include "asterisk/module.h"
#include "asterisk/file.h"
#include "asterisk/app.h"
#include "asterisk/chanvars.h"
#include "asterisk/utils.h"
#include "asterisk/devicestate.h"

static const char *app_page= "Page";

static const char *page_synopsis = "Pages phones";

static const char *page_descrip =
"Page(Technology/Resource&Technology2/Resource2[|options])\n"
"  Places outbound calls to the given technology / resource and dumps\n"
"them into a conference bridge as muted participants.  The original\n"
"caller is dumped into the conference as a speaker and the room is\n"
"destroyed when the original caller leaves.  Valid options are:\n"
"        d - full duplex audio\n"
"        q - quiet, do not play beep to caller\n"
"        r - record the page into a file (see 'r' for app_meetme)\n"
"        s - only dial channel if devicestate says it is not in use\n";

enum {
	PAGE_DUPLEX = (1 << 0),
	PAGE_QUIET = (1 << 1),
	PAGE_RECORD = (1 << 2),
	PAGE_SKIP = (1 << 3),
} page_opt_flags;

AST_APP_OPTIONS(page_opts, {
	AST_APP_OPTION('d', PAGE_DUPLEX),
	AST_APP_OPTION('q', PAGE_QUIET),
	AST_APP_OPTION('r', PAGE_RECORD),
	AST_APP_OPTION('s', PAGE_SKIP),
});

struct calloutdata {
	char cidnum[64];
	char cidname[64];
	char tech[64];
	char resource[256];
	char meetmeopts[64];
	struct ast_variable *variables;
};

static void *page_thread(void *data)
{
	struct calloutdata *cd = data;

	ast_pbx_outgoing_app(cd->tech, AST_FORMAT_SLINEAR, cd->resource, 30000,
		"MeetMe", cd->meetmeopts, NULL, 0, cd->cidnum, cd->cidname, cd->variables, NULL, NULL);

	free(cd);

	return NULL;
}

static void launch_page(struct ast_channel *chan, const char *meetmeopts, const char *tech, const char *resource)
{
	struct calloutdata *cd;
	const char *varname;
	struct ast_variable *lastvar = NULL;
	struct ast_var_t *varptr;
	pthread_t t;
	pthread_attr_t attr;

	if (!(cd = ast_calloc(1, sizeof(*cd))))
		return;

	/* Copy data from our page over */
	ast_copy_string(cd->cidnum, chan->cid.cid_num ? chan->cid.cid_num : "", sizeof(cd->cidnum));
	ast_copy_string(cd->cidname, chan->cid.cid_name ? chan->cid.cid_name : "", sizeof(cd->cidname));
	ast_copy_string(cd->tech, tech, sizeof(cd->tech));
	ast_copy_string(cd->resource, resource, sizeof(cd->resource));
	ast_copy_string(cd->meetmeopts, meetmeopts, sizeof(cd->meetmeopts));
	
	AST_LIST_TRAVERSE(&chan->varshead, varptr, entries) {
		struct ast_variable *newvar = NULL;

		if (!(varname = ast_var_full_name(varptr)) || (varname[0] != '_'))
			continue;
			
		if (varname[1] == '_')
			newvar = ast_variable_new(varname, ast_var_value(varptr));
		else
			newvar = ast_variable_new(&varname[1], ast_var_value(varptr));
		
		if (newvar) {
			if (lastvar)
				lastvar->next = newvar;
			else
				cd->variables = newvar;
			lastvar = newvar;
		}
	}

	/* Spawn thread to handle this page */
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	if (ast_pthread_create(&t, &attr, page_thread, cd)) {
		ast_log(LOG_WARNING, "Unable to create paging thread: %s\n", strerror(errno));
		free(cd);
	}

	return;
}

static int page_exec(struct ast_channel *chan, void *data)
{
	struct ast_module_user *u;
	char *options;
	char *tech, *resource;
	char meetmeopts[80];
	struct ast_flags flags = { 0 };
	unsigned int confid = ast_random();
	struct ast_app *app;
	char *tmp;
	int res=0;
	char originator[AST_CHANNEL_NAME];

	if (ast_strlen_zero(data)) {
		ast_log(LOG_WARNING, "This application requires at least one argument (destination(s) to page)\n");
		return -1;
	}

	u = ast_module_user_add(chan);

	if (!(app = pbx_findapp("MeetMe"))) {
		ast_log(LOG_WARNING, "There is no MeetMe application available!\n");
		ast_module_user_remove(u);
		return -1;
	};

	options = ast_strdupa(data);

	ast_copy_string(originator, chan->name, sizeof(originator));
	if ((tmp = strchr(originator, '-')))
		*tmp = '\0';

	tmp = strsep(&options, "|");
	if (options)
		ast_app_parse_options(page_opts, &flags, NULL, options);

	snprintf(meetmeopts, sizeof(meetmeopts), "%ud|%s%sqxdw(5)", confid, (ast_test_flag(&flags, PAGE_DUPLEX) ? "" : "m"),
		(ast_test_flag(&flags, PAGE_RECORD) ? "r" : "") );

	while ((tech = strsep(&tmp, "&"))) {
		int state = 0;

		/* don't call the originating device */
		if (!strcasecmp(tech, originator))
			continue;

		if (!(resource = strchr(tech, '/'))) {
			ast_log(LOG_WARNING, "Incomplete destination '%s' supplied.\n", tech);
			continue;
		}

		/* Ensure device is not in use if skip option is enabled */
		if (ast_test_flag(&flags, PAGE_SKIP) && (state = ast_device_state(tech)) != AST_DEVICE_NOT_INUSE) {
			ast_log(LOG_WARNING, "Destination '%s' has device state '%s'.\n", tech, devstate2str(state));
			continue;
		}
		
		*resource++ = '\0';
		launch_page(chan, meetmeopts, tech, resource);
	}

	if (!ast_test_flag(&flags, PAGE_QUIET)) {
		res = ast_streamfile(chan, "beep", chan->language);
		if (!res)
			res = ast_waitstream(chan, "");
	}

	if (!res) {
		snprintf(meetmeopts, sizeof(meetmeopts), "%ud|A%s%sqxd", confid, (ast_test_flag(&flags, PAGE_DUPLEX) ? "" : "t"), 
			(ast_test_flag(&flags, PAGE_RECORD) ? "r" : "") );
		pbx_exec(chan, app, meetmeopts);
	}

	ast_module_user_remove(u);

	return -1;
}

static int unload_module(void)
{
	int res;

	res =  ast_unregister_application(app_page);

	ast_module_user_hangup_all();

	return res;
}

static int load_module(void)
{
	return ast_register_application(app_page, page_exec, page_synopsis, page_descrip);
}

AST_MODULE_INFO_STANDARD(ASTERISK_GPL_KEY, "Page Multiple Phones");

