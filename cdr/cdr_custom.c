/*
 * Asterisk -- An open source telephony toolkit.
 *
 * Copyright (C) 1999 - 2005, Digium, Inc.
 *
 * Mark Spencer <markster@digium.com>
 *
 * Includes code and algorithms from the Zapata library.
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
 * \brief Custom Comma Separated Value CDR records.
 *
 * \author Mark Spencer <markster@digium.com>
 *
 * \arg See also \ref AstCDR
 *
 * Logs in LOG_DIR/cdr_custom
 * \ingroup cdr_drivers
 */

#include <sys/types.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

#include <stdlib.h>
#include <unistd.h>
#include <time.h>

#include "asterisk.h"

ASTERISK_FILE_VERSION(__FILE__, "$Revision$")

#include "asterisk/channel.h"
#include "asterisk/cdr.h"
#include "asterisk/module.h"
#include "asterisk/config.h"
#include "asterisk/pbx.h"
#include "asterisk/logger.h"
#include "asterisk/utils.h"

#define CUSTOM_LOG_DIR "/cdr_custom"

#define DATE_FORMAT "%Y-%m-%d %T"

AST_MUTEX_DEFINE_STATIC(lock);

static char *desc = "Customizable Comma Separated Values CDR Backend";

static char *name = "cdr-custom";

static FILE *mf = NULL;

static char master[AST_CONFIG_MAX_PATH];
static char format[1024]="";

static int load_config(int reload) 
{
	struct ast_config *cfg;
	struct ast_variable *var;
	int res = -1;

	strcpy(format, "");
	strcpy(master, "");
	if((cfg = ast_config_load("cdr_custom.conf"))) {
		var = ast_variable_browse(cfg, "mappings");
		while(var) {
			ast_mutex_lock(&lock);
			if (!ast_strlen_zero(var->name) && !ast_strlen_zero(var->value)) {
				if (strlen(var->value) > (sizeof(format) - 2))
					ast_log(LOG_WARNING, "Format string too long, will be truncated, at line %d\n", var->lineno);
				strncpy(format, var->value, sizeof(format) - 2);
				strcat(format,"\n");
				snprintf(master, sizeof(master),"%s/%s/%s", ast_config_AST_LOG_DIR, name, var->name);
				ast_mutex_unlock(&lock);
			} else
				ast_log(LOG_NOTICE, "Mapping must have both filename and format at line %d\n", var->lineno);
			if (var->next)
				ast_log(LOG_NOTICE, "Sorry, only one mapping is supported at this time, mapping '%s' will be ignored at line %d.\n", var->next->name, var->next->lineno); 
			var = var->next;
		}
		ast_config_destroy(cfg);
		res = 0;
	} else {
		if (reload)
			ast_log(LOG_WARNING, "Failed to reload configuration file.\n");
		else
			ast_log(LOG_WARNING, "Failed to load configuration file. Module not activated.\n");
	}
	
	return res;
}



static int custom_log(struct ast_cdr *cdr)
{
	/* Make sure we have a big enough buf */
	char buf[2048];
	struct ast_channel dummy;

	/* Abort if no master file is specified */
	if (ast_strlen_zero(master))
		return 0;

	memset(buf, 0 , sizeof(buf));
	/* Quite possibly the first use of a static struct ast_channel, we need it so the var funcs will work */
	memset(&dummy, 0, sizeof(dummy));
	dummy.cdr = cdr;
	pbx_substitute_variables_helper(&dummy, format, buf, sizeof(buf) - 1);

	/* because of the absolutely unconditional need for the
	   highest reliability possible in writing billing records,
	   we open write and close the log file each time */
	mf = fopen(master, "a");
	if (!mf) {
		ast_log(LOG_ERROR, "Unable to re-open master file %s : %s\n", master, strerror(errno));
	}
	if (mf) {
		fputs(buf, mf);
		fflush(mf); /* be particularly anal here */
		fclose(mf);
		mf = NULL;
	}
	return 0;
}

static const char *description(void)
{
	return desc;
}

static int unload_module(void *mod)
{
	if (mf)
		fclose(mf);
	ast_cdr_unregister(name);
	return 0;
}

static int load_module(void *mod)
{
	int res = 0;

	if (!load_config(0)) {
		res = ast_cdr_register(name, desc, custom_log);
		if (res)
			ast_log(LOG_ERROR, "Unable to register custom CDR handling\n");
		if (mf)
			fclose(mf);
	}
	return res;
}

static int reload(void *mod)
{
	return load_config(1);
}

static const char *key(void)
{
	return ASTERISK_GPL_KEY;
}

STD_MOD(MOD_1 | NO_USECOUNT, reload, NULL, NULL);

