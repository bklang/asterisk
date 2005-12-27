/*
 * Asterisk -- An open source telephony toolkit.
 *
 * Copyright (C) 2005, Digium, Inc.
 * Portions Copyright (C) 2005, Tilghman Lesher.  All rights reserved.
 * Portions Copyright (C) 2005, Anthony Minessale II
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
 * \brief String manipulation dialplan functions
 * 
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <regex.h>

#include "asterisk.h"

/* ASTERISK_FILE_VERSION(__FILE__, "$Revision$") */

#include "asterisk/channel.h"
#include "asterisk/pbx.h"
#include "asterisk/logger.h"
#include "asterisk/utils.h"
#include "asterisk/app.h"
#include "asterisk/localtime.h"

static char *function_fieldqty(struct ast_channel *chan, char *cmd, char *data, char *buf, size_t len)
{
	char *varname, *varval, workspace[256];
	char *delim = ast_strdupa(data);
	int fieldcount = 0;

	if (delim) {
		varname = strsep(&delim, "|");
		pbx_retrieve_variable(chan, varname, &varval, workspace, sizeof(workspace), NULL);
		while (strsep(&varval, delim))
			fieldcount++;
		snprintf(buf, len, "%d", fieldcount);
	} else {
		ast_log(LOG_ERROR, "Out of memory\n");
		strncpy(buf, "1", len);
	}
	return buf;
}

#ifndef BUILTIN_FUNC
static
#endif
struct ast_custom_function fieldqty_function = {
	.name = "FIELDQTY",
	.synopsis = "Count the fields, with an arbitrary delimiter",
	.syntax = "FIELDQTY(<varname>,<delim>)",
	.read = function_fieldqty,
};

static char *builtin_function_filter(struct ast_channel *chan, char *cmd, char *data, char *buf, size_t len)
{
	char *allowed, *string, *outbuf=buf;

	string = ast_strdupa(data);
	if (!string) {
		ast_log(LOG_ERROR, "Out of memory");
		return "";
	}

	allowed = strsep(&string, "|");

	if (!string) {
		ast_log(LOG_ERROR, "Usage: FILTER(<allowed-chars>,<string>)\n");
		return "";
	}

	for ( ; *string && (buf + len - 1 > outbuf); string++) {
		if (strchr(allowed, *string)) {
			*outbuf = *string;
			outbuf++;
		}
	}
	*outbuf = '\0';
	
	return buf;
}

#ifndef BUILTIN_FUNC
static
#endif
struct ast_custom_function filter_function = {
	.name = "FILTER",
	.synopsis = "Filter the string to include only the allowed characters",
	.syntax = "FILTER(<allowed-chars>,<string>)",
	.read = builtin_function_filter,
};

static char *builtin_function_regex(struct ast_channel *chan, char *cmd, char *data, char *buf, size_t len) 
{
	char *arg, *earg = NULL, *tmp, errstr[256] = "";
	int errcode;
	regex_t regexbuf;

	ast_copy_string(buf, "0", len);
	
	tmp = ast_strdupa(data);
	if (!tmp) {
		ast_log(LOG_ERROR, "Out of memory in %s(%s)\n", cmd, data);
		return buf;
	}

	/* Regex in quotes */
	arg = strchr(tmp, '"');
	if (arg) {
		arg++;
		earg = strrchr(arg, '"');
		if (earg) {
			*earg++ = '\0';
			/* Skip over any spaces before the data we are checking */
			while (*earg == ' ')
				earg++;
		}
	} else {
		arg = tmp;
	}

	if ((errcode = regcomp(&regexbuf, arg, REG_EXTENDED | REG_NOSUB))) {
		regerror(errcode, &regexbuf, errstr, sizeof(errstr));
		ast_log(LOG_WARNING, "Malformed input %s(%s): %s\n", cmd, data, errstr);
	} else {
		if (!regexec(&regexbuf, earg ? earg : "", 0, NULL, 0))
			ast_copy_string(buf, "1", len); 
	}
	regfree(&regexbuf);

	return buf;
}

#ifndef BUILTIN_FUNC
static
#endif
struct ast_custom_function regex_function = {
	.name = "REGEX",
	.synopsis = "Regular Expression: Returns 1 if data matches regular expression.",
	.syntax = "REGEX(\"<regular expression>\" <data>)",
	.read = builtin_function_regex,
};

static void builtin_function_array(struct ast_channel *chan, char *cmd, char *data, const char *value)
{
	char *varv[100];
	char *valuev[100];
	char *var, *value2;
	int varcount, valuecount, i;

	var = ast_strdupa(data);
	value2 = ast_strdupa(value);
	if (!var || !value2) {
		ast_log(LOG_ERROR, "Out of memory\n");
		return;
	}

	/* The functions this will generally be used with are SORT and ODBC_*, which
	 * both return comma-delimited lists.  However, if somebody uses literal lists,
	 * their commas will be translated to vertical bars by the load, and I don't
	 * want them to be surprised by the result.  Hence, we prefer commas as the
	 * delimiter, but we'll fall back to vertical bars if commas aren't found.
	 */
	if (strchr(var, ',')) {
		varcount = ast_app_separate_args(var, ',', varv, 100);
	} else {
		varcount = ast_app_separate_args(var, '|', varv, 100);
	}

	if (strchr(value2, ',')) {
		valuecount = ast_app_separate_args(value2, ',', valuev, 100);
	} else {
		valuecount = ast_app_separate_args(value2, '|', valuev, 100);
	}

	for (i = 0; i < varcount; i++) {
		if (i < valuecount) {
			pbx_builtin_setvar_helper(chan, varv[i], valuev[i]);
		} else {
			/* We could unset the variable, by passing a NULL, but due to
			 * pushvar semantics, that could create some undesired behavior. */
			pbx_builtin_setvar_helper(chan, varv[i], "");
		}
	}
}

#ifndef BUILTIN_FUNC
static
#endif
struct ast_custom_function array_function = {
	.name = "ARRAY",
	.synopsis = "Allows setting multiple variables at once",
	.syntax = "ARRAY(var1[,var2[...][,varN]])",
	.write = builtin_function_array,
	.desc =
"The comma-separated list passed as a value to which the function is set\n"
"will be interpreted as a set of values to which the comma-separated list\n"
"of variable names in the argument should be set.\n"
"Hence, Set(ARRAY(var1,var2)=1,2) will set var1 to 1 and var2 to 2\n",
};

static char *builtin_function_len(struct ast_channel *chan, char *cmd, char *data, char *buf, size_t len) 
{
	int length = 0;
	if (data) {
		length = strlen(data);
	}
	snprintf(buf, len, "%d", length);
	return buf;
}

#ifndef BUILTIN_FUNC
static
#endif
struct ast_custom_function len_function = {
	.name = "LEN",
	.synopsis = "Returns the length of the argument given",
	.syntax = "LEN(<string>)",
	.read = builtin_function_len,
};

static char *acf_strftime(struct ast_channel *chan, char *cmd, char *data, char *buf, size_t len) 
{
	char *format, *epoch, *timezone = NULL;
	long epochi;
	struct tm time;

	buf[0] = '\0';

	if (!data) {
		ast_log(LOG_ERROR, "Asterisk function STRFTIME() requires an argument.\n");
		return buf;
	}
	
	format = ast_strdupa(data);
	if (!format) {
		ast_log(LOG_ERROR, "Out of memory\n");
		return buf;
	}
	
	epoch = strsep(&format, "|");
	timezone = strsep(&format, "|");

	if (ast_strlen_zero(epoch) || !sscanf(epoch, "%ld", &epochi)) {
		struct timeval tv = ast_tvnow();
		epochi = tv.tv_sec;
	}

	ast_localtime(&epochi, &time, timezone);

	if (!format) {
		format = "%c";
	}

	if (!strftime(buf, len, format, &time)) {
		ast_log(LOG_WARNING, "C function strftime() output nothing?!!\n");
	}
	buf[len - 1] = '\0';

	return buf;
}

#ifndef BUILTIN_FUNC
static
#endif
struct ast_custom_function strftime_function = {
	.name = "STRFTIME",
	.synopsis = "Returns the current date/time in a specified format.",
	.syntax = "STRFTIME([<epoch>][,[timezone][,format]])",
	.read = acf_strftime,
};

static char *function_eval(struct ast_channel *chan, char *cmd, char *data, char *buf, size_t len) 
{
	memset(buf, 0, len);

	if (ast_strlen_zero(data)) {
		ast_log(LOG_WARNING, "EVAL requires an argument: EVAL(<string>)\n");
		return buf;
	}

	pbx_substitute_variables_helper(chan, data, buf, len - 1);

	return buf;
}

#ifndef BUILTIN_FUNC
static
#endif
struct ast_custom_function eval_function = {
	.name = "EVAL",
	.synopsis = "Evaluate stored variables.",
	.syntax = "EVAL(<variable>)",
	.desc = "Using EVAL basically causes a string to be evaluated twice.\n"
		"When a variable or expression is in the dialplan, it will be\n"
		"evaluated at runtime. However, if the result of the evaluation\n"
		"is in fact a variable or expression, using EVAL will have it\n"
		"evaluated a second time. For example, if the variable ${MYVAR}\n"
		"contains \"${OTHERVAR}\", then the result of putting ${EVAL(${MYVAR})}\n"
		"in the dialplan will be the contents of the variable, OTHERVAR.\n"
		"Normally, by just putting ${MYVAR} in the dialplan, you would be\n"
		"left with \"${OTHERVAR}\".\n", 
	.read = function_eval,
};

