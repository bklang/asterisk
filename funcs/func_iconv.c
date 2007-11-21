/*
 * Asterisk -- An open source telephony toolkit.
 *
 * Copyright (c) 2005,2006,2007 Sven Slezak <sunny@mezzo.net>
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

/*!
 * \file
 *
 * \brief Charset conversions
 *
 * \author Sven Slezak <sunny@mezzo.net>
 *
 * \ingroup functions
 */

/*** MODULEINFO
 ***/

#include "asterisk.h"

ASTERISK_FILE_VERSION(__FILE__, "$Revision$")

#include <ctype.h>
#include <iconv.h>

#include "asterisk/module.h"
#include "asterisk/channel.h"
#include "asterisk/pbx.h"
#include "asterisk/utils.h"
#include "asterisk/app.h"

/*! 
 * Some systems define the second arg to iconv() as (const char *),
 * while others define it as (char *).  Cast it to a (void *) to 
 * suppress compiler warnings about it. 
 */
#define AST_ICONV_CAST void *

static int iconv_read(struct ast_channel *chan, const char *cmd, char *arguments, char *buf, size_t len)
{
	AST_DECLARE_APP_ARGS(args,
		AST_APP_ARG(in_charset);
		AST_APP_ARG(out_charset);
		AST_APP_ARG(text);
	);
	iconv_t cd;
	size_t incount, outcount = len;
	char *parse;

	if (ast_strlen_zero(arguments)) {
		ast_log(LOG_WARNING, "Syntax: ICONV(<in-charset>,<out-charset>,<text>) - missing arguments!\n");
		return -1;
	}

	parse = ast_strdupa(arguments);
	AST_STANDARD_APP_ARGS(args, parse);

	if (args.argc < 3) {
		ast_log(LOG_WARNING, "Syntax: ICONV(<in-charset>,<out-charset>,<text>) %d\n", args.argc);
		return -1;
	}

	incount = strlen(args.text);

	ast_debug(1, "Iconv: \"%s\" %s -> %s\n", args.text, args.in_charset, args.out_charset);

	cd = iconv_open(args.out_charset, args.in_charset);

	if (cd == (iconv_t) -1) {
		ast_log(LOG_ERROR, "conversion from '%s' to '%s' not available. type 'iconv -l' in a shell to list the supported charsets.\n", args.in_charset, args.out_charset);
		return -1;
	}

	if (iconv(cd, (AST_ICONV_CAST) &args.text, &incount, &buf, &outcount) == (size_t) -1) {
		if (errno == E2BIG)
			ast_log(LOG_WARNING, "Iconv: output buffer too small.\n");
		else if (errno == EILSEQ)
			ast_log(LOG_WARNING,  "Iconv: illegal character.\n");
		else if (errno == EINVAL)
			ast_log(LOG_WARNING,  "Iconv: incomplete character sequence.\n");
		else
			ast_log(LOG_WARNING,  "Iconv: error %d: %s.\n", errno, strerror(errno));
	}
	iconv_close(cd);

	return 0;
}


static struct ast_custom_function iconv_function = {
	.name = "ICONV",
	.synopsis = "Converts charsets of strings.",
	.desc =
"Converts string from in-charset into out-charset.  For available charsets,\n"
"use 'iconv -l' on your shell command line.\n"
"Note: due to limitations within the API, ICONV will not currently work with\n"
"charsets with embedded NULLs.  If found, the string will terminate.\n",
	.syntax = "ICONV(in-charset,out-charset,string)",
	.read = iconv_read,
};

static int unload_module(void)
{
	return ast_custom_function_unregister(&iconv_function);
}

static int load_module(void)
{
	return ast_custom_function_register(&iconv_function);
}

AST_MODULE_INFO_STANDARD(ASTERISK_GPL_KEY, "Charset conversions");

