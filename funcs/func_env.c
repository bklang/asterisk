/*
 * Asterisk -- An open source telephony toolkit.
 *
 * Copyright (C) 1999 - 2005, Digium, Inc.
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
 * \brief Environment related dialplan functions
 * 
 */

#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include "asterisk.h"

/* ASTERISK_FILE_VERSION(__FILE__, "$Revision$") */

#include "asterisk/channel.h"
#include "asterisk/pbx.h"
#include "asterisk/logger.h"
#include "asterisk/utils.h"
#include "asterisk/app.h"

static char *builtin_function_env_read(struct ast_channel *chan, char *cmd, char *data, char *buf, size_t len) 
{
	char *ret = "";

	if (data) {
		ret = getenv(data);
		if (!ret)
			ret = "";
	}
	ast_copy_string(buf, ret, len);

	return buf;
}

static void builtin_function_env_write(struct ast_channel *chan, char *cmd, char *data, const char *value) 
{
	if (!ast_strlen_zero(data)) {
		if (!ast_strlen_zero(value)) {
			setenv(data, value, 1);
		} else {
			unsetenv(data);
		}
	}
}

#ifndef BUILTIN_FUNC
static
#endif
struct ast_custom_function env_function = {
	.name = "ENV",
	.synopsis = "Gets or sets the environment variable specified",
	.syntax = "ENV(<envname>)",
	.read = builtin_function_env_read,
	.write = builtin_function_env_write,
};
