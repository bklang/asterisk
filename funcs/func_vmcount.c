/*
 * Asterisk -- An open source telephony toolkit.
 *
 * Copyright (c) 2006 Tilghman Lesher.  All rights reserved.
 * 
 * Tilghman Lesher <asterisk-vmcount-func@the-tilghman.com>
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
 * \brief VMCOUNT dialplan function
 *
 * \author Tilghman Lesher <asterisk-vmcount-func@the-tilghman.com>
 *
 * \ingroup functions
 */

#include "asterisk.h"

ASTERISK_FILE_VERSION(__FILE__, "$Revision$")

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/types.h>

#include "asterisk/file.h"
#include "asterisk/channel.h"
#include "asterisk/pbx.h"
#include "asterisk/module.h"
#include "asterisk/lock.h"
#include "asterisk/utils.h"
#include "asterisk/app.h"
#include "asterisk/options.h"

static int acf_vmcount_exec(struct ast_channel *chan, const char *cmd, char *argsstr, char *buf, size_t len)
{
	char *context;
	AST_DECLARE_APP_ARGS(args,
		AST_APP_ARG(vmbox);
		AST_APP_ARG(folder);
	);

	buf[0] = '\0';

	AST_STANDARD_APP_ARGS(args, argsstr);

	if (strchr(args.vmbox, '@')) {
		context = args.vmbox;
		args.vmbox = strsep(&context, "@");
	} else {
		context = "default";
	}

	if (ast_strlen_zero(args.folder)) {
		args.folder = "INBOX";
	}

	snprintf(buf, len, "%d", ast_app_messagecount(context, args.vmbox, args.folder));
	
	return 0;
}

struct ast_custom_function acf_vmcount = {
	.name = "VMCOUNT",
	.synopsis = "Counts the voicemail in a specified mailbox",
	.syntax = "VMCOUNT(vmbox[@context][,folder])",
	.desc =
	"  context - defaults to \"default\"\n"
	"  folder  - defaults to \"INBOX\"\n",
	.read = acf_vmcount_exec,
};

static int unload_module(void)
{
	return ast_custom_function_unregister(&acf_vmcount);
}

static int load_module(void)
{
	return ast_custom_function_register(&acf_vmcount);
}

AST_MODULE_INFO_STANDARD(ASTERISK_GPL_KEY, "Indicator for whether a voice mailbox has messages in a given folder.");
