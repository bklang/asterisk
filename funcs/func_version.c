/*
 * Asterisk -- An open source telephony toolkit.
 *
 * Copyright (C) 2006, Digium, Inc.
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
 * \brief Return the current Version strings
 * 
 * \author Steve Murphy (murf@digium.com)
 * \ingroup functions
 */

#include "asterisk.h"

ASTERISK_FILE_VERSION(__FILE__, "$Revision$")

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/types.h>

#include "asterisk/module.h"
#include "asterisk/channel.h"
#include "asterisk/pbx.h"
#include "asterisk/logger.h"
#include "asterisk/utils.h"
#include "asterisk/app.h"
#include "asterisk/options.h"
#include "asterisk/version.h"
#include "asterisk/build.h"

#define STRING_IT(vernum) STRING_IT2(vernum)
#define STRING_IT2(vernum) #vernum

static int acf_version_exec(struct ast_channel *chan, const char *cmd,
			 char *parse, char *buffer, size_t buflen)
{
	struct ast_module_user *u;
	char *response_char = ASTERISK_VERSION;
	AST_DECLARE_APP_ARGS(args,
			     AST_APP_ARG(info);
	);

	u = ast_module_user_add(chan);

	AST_STANDARD_APP_ARGS(args, parse);

	if (!ast_strlen_zero(args.info) ) {
		if (strcasecmp(args.info,"ASTERISK_VERSION_NUM") == 0)
			response_char = STRING_IT(ASTERISK_VERSION_NUM);
		else if (strcasecmp(args.info,"BUILD_USER") == 0)
			response_char = BUILD_USER;
		else if (strcasecmp(args.info,"BUILD_HOSTNAME") == 0)
			response_char = BUILD_HOSTNAME;
		else if (strcasecmp(args.info,"BUILD_MACHINE") == 0)
			response_char = BUILD_MACHINE;
		else if (strcasecmp(args.info,"BUILD_KERNEL") == 0)
			response_char = BUILD_KERNEL;
		else if (strcasecmp(args.info,"BUILD_OS") == 0)
			response_char = BUILD_OS;
		else if (strcasecmp(args.info,"BUILD_DATE") == 0)
			response_char = BUILD_DATE;

	}


	if (option_debug)
		ast_log(LOG_DEBUG, "VERSION returns %s result, given %s argument\n",
			response_char, args.info);
	snprintf(buffer, buflen, "%s", response_char);

	ast_module_user_remove(u);

	return 0;
}

static struct ast_custom_function acf_version = {
	.name = "VERSION",
	.synopsis = "Return the Version info for this Asterisk",
	.syntax = "VERSION([info])",
	.desc =
		"If there are no arguments, return the version of Asterisk in this format: SVN-branch-1.4-r44830M\n"
		"If the argument is 'ASTERISK_VERSION_NUM', a string of digits is returned (right now fixed at 999999).\n"
		"If the argument is 'BUILD_USER', the string representing the user's name whose account was used to configure Asterisk, is returned.\n"
		"If the argument is 'BUILD_HOSTNAME', the string representing the name of the host on which Asterisk was configured, is returned.\n"
		"If the argument is 'BUILD_MACHINE', the string representing the type of machine on which Asterisk was configured, is returned.\n"
		"If the argument is 'BUILD_OS', the string representing the OS of the machine on which Asterisk was configured, is returned.\n"
		"If the argument is 'BUILD_DATE', the string representing the date on which Asterisk was configured, is returned.\n"
		"If the argument is 'BUILD_KERNEL', the string representing the kernel version of the machine on which Asterisk was configured, is returned .\n"
		"  Example:  Set(junky=${VERSION()}; \n"
		"  Sets junky to the string 'SVN-branch-1.6-r74830M', or possibly, 'SVN-trunk-r45126M'.\n",
	.read = acf_version_exec,
};

static int unload_module(void)
{
	ast_custom_function_unregister(&acf_version);

	return 0;
}

static int load_module(void)
{
	return ast_custom_function_register(&acf_version);
}

AST_MODULE_INFO_STANDARD(ASTERISK_GPL_KEY, "Get Asterisk Version/Build Info");
