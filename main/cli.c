/*
 * Asterisk -- An open source telephony toolkit.
 *
 * Copyright (C) 1999 - 2006, Digium, Inc.
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
 * \brief Standard Command Line Interface
 *
 * \author Mark Spencer <markster@digium.com> 
 */

#include "asterisk.h"

ASTERISK_FILE_VERSION(__FILE__, "$Revision$")

#include <unistd.h>
#include <stdlib.h>
#include <sys/signal.h>
#include <stdio.h>
#include <signal.h>
#include <string.h>
#include <ctype.h>
#include <regex.h>

#include "asterisk/logger.h"
#include "asterisk/options.h"
#include "asterisk/cli.h"
#include "asterisk/linkedlists.h"
#include "asterisk/module.h"
#include "asterisk/pbx.h"
#include "asterisk/channel.h"
#include "asterisk/utils.h"
#include "asterisk/app.h"
#include "asterisk/lock.h"
#include "editline/readline/readline.h"
#include "asterisk/threadstorage.h"

extern unsigned long global_fin, global_fout;

AST_THREADSTORAGE(ast_cli_buf);

/*! \brief Initial buffer size for resulting strings in ast_cli() */
#define AST_CLI_INITLEN   256

void ast_cli(int fd, char *fmt, ...)
{
	int res;
	struct ast_dynamic_str *buf;
	va_list ap;

	if (!(buf = ast_dynamic_str_thread_get(&ast_cli_buf, AST_CLI_INITLEN)))
		return;

	va_start(ap, fmt);
	res = ast_dynamic_str_thread_set_va(&buf, 0, &ast_cli_buf, fmt, ap);
	va_end(ap);

	if (res != AST_DYNSTR_BUILD_FAILED)
		ast_carefulwrite(fd, buf->str, strlen(buf->str), 100);
}

static AST_LIST_HEAD_STATIC(helpers, ast_cli_entry);

static char help_help[] =
"Usage: help [topic]\n"
"       When called with a topic as an argument, displays usage\n"
"       information on the given command. If called without a\n"
"       topic, it provides a list of commands.\n";

static char chanlist_help[] = 
"Usage: core show channels [concise|verbose]\n"
"       Lists currently defined channels and some information about them. If\n"
"       'concise' is specified, the format is abridged and in a more easily\n"
"       machine parsable format. If 'verbose' is specified, the output includes\n"
"       more and longer fields.\n";

static char logger_mute_help[] = 
"Usage: logger mute\n"
"       Disables logging output to the current console, making it possible to\n"
"       gather information without being disturbed by scrolling lines.\n";

static char softhangup_help[] =
"Usage: soft hangup <channel>\n"
"       Request that a channel be hung up. The hangup takes effect\n"
"       the next time the driver reads or writes from the channel\n";

static char group_show_channels_help[] = 
"Usage: group show channels [pattern]\n"
"       Lists all currently active channels with channel group(s) specified.\n"
"       Optional regular expression pattern is matched to group names for each\n"
"       channel.\n";

static char *complete_fn(const char *word, int state)
{
	char *c;
	char filename[256];

	if (word[0] == '/')
		ast_copy_string(filename, word, sizeof(filename));
	else
		snprintf(filename, sizeof(filename), "%s/%s", ast_config_AST_MODULE_DIR, word);

	/* XXX the following function is not reentrant, so we better not use it */
	c = filename_completion_function(filename, state);
	
	if (c && word[0] != '/')
		c += (strlen(ast_config_AST_MODULE_DIR) + 1);
	
	return c ? strdup(c) : c;
}

static char *handle_load(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	/* "module load <mod>" */
	switch (cmd) {
	case CLI_INIT:
		e->command = "module load";
		e->usage =
			"Usage: module load <module name>\n"
			"       Loads the specified module into Asterisk.\n";
		return NULL;

	case CLI_GENERATE:
		if (a->argc != e->args + 1)
			return NULL;
		return complete_fn(a->word, a->n);
	}
	if (a->argc != e->args + 1)
		return CLI_SHOWUSAGE;
	if (ast_load_resource(a->argv[e->args])) {
		ast_cli(a->fd, "Unable to load module %s\n", a->argv[e->args]);
		return CLI_FAILURE;
	}
	return CLI_SUCCESS;
}

static char *handle_load_deprecated(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	char *res = handle_load(e, cmd, a);
	if (cmd == CLI_INIT)
		e->command = "load";
	return res;
}

static char *handle_reload(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	int x;

	switch (cmd) {
	case CLI_INIT:
		e->command = "module reload";
		e->usage =
			"Usage: module reload [module ...]\n"
			"       Reloads configuration files for all listed modules which support\n"
			"       reloading, or for all supported modules if none are listed.\n";
		return NULL;

	case CLI_GENERATE:
		return ast_module_helper(a->line, a->word, a->pos, a->n, a->pos, 1);
	}
	if (a->argc == e->args) {
		ast_module_reload(NULL);
		return CLI_SUCCESS;
	}
	for (x = e->args; x < a->argc; x++) {
		int res = ast_module_reload(a->argv[x]);
		/* XXX reload has multiple error returns, including -1 on error and 2 on success */
		switch(res) {
		case 0:
			ast_cli(a->fd, "No such module '%s'\n", a->argv[x]);
			break;
		case 1:
			ast_cli(a->fd, "Module '%s' does not support reload\n", a->argv[x]);
			break;
		}
	}
	return CLI_SUCCESS;
}

static char *handle_reload_deprecated(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	char *s = handle_reload(e, cmd, a);
	if (cmd == CLI_INIT)		/* override command name */
		e->command = "reload";
	return s;
}

static char *handle_verbose(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	int oldval = option_verbose;
	int newlevel;
	int atleast = 0;
	static char *choices[] = { "off", "atleast", NULL };
	int fd = a->fd;
	int argc = a->argc;
	char **argv = a->argv;

	switch (cmd) {
	case CLI_INIT:
		e->command = "core set verbose";
		e->usage =
			"Usage: core set verbose [atleast] <level>\n"
			"       core set verbose off\n"
			"       Sets level of verbose messages to be displayed.  0 or off means\n"
			"       no messages should be displayed. Equivalent to -v[v[v...]]\n"
			"       on startup\n";
		return NULL;

	case CLI_GENERATE:
		if (a->pos > e->args)
			return NULL;
		return ast_cli_complete(a->word, choices, a->n);
	}
	/* all the above return, so we proceed with the handler.
	 * we are guaranteed to be called with argc >= e->args;
	 */

	if (argc < e->args + 1)
		return CLI_SHOWUSAGE;

	if (argc == e->args + 1 && !strcasecmp(argv[e->args], "off")) {
		newlevel = 0;
		goto done;
	}
	if (!strcasecmp(argv[e->args], "atleast"))
		atleast = 1;
	if (argc != e->args + atleast + 1)
		return CLI_SHOWUSAGE;
	if (sscanf(argv[e->args + atleast], "%d", &newlevel) != 1)
		return CLI_SHOWUSAGE;

done:
	if (!atleast || newlevel > option_verbose)
		option_verbose = newlevel;
	if (oldval > 0 && option_verbose == 0)
		ast_cli(fd, "Verbosity is now OFF\n");
	else if (option_verbose > 0) {
		if (oldval == option_verbose)
			ast_cli(fd, "Verbosity is at least %d\n", option_verbose);
		else
			ast_cli(fd, "Verbosity was %d and is now %d\n", oldval, option_verbose);
	}

	return CLI_SUCCESS;
}

static char *handle_set_debug(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	int oldval = option_debug;
	int newlevel;
	int atleast = 0;
	char *filename = '\0';
	static char *choices[] = { "off", "atleast", NULL };
	int fd = a->fd;
	int argc = a->argc;
	char **argv = a->argv;

	switch (cmd) {
	case CLI_INIT:
		e->command = "core set debug";
		e->usage =
			"Usage: core set debug [atleast] <level> [filename]\n"
			"       core set debug off\n"
			"       Sets level of core debug messages to be displayed. 0 or 'off' means\n"
			"       no messages should be displayed.  Equivalent to -d[d[d...]]\n"
			"       on startup.  If filename is specified, debugging will be\n"
			"       limited to just that file.\n";
		return NULL;

	case CLI_GENERATE:
		if (a->pos > e->args)
			return NULL;
		return ast_cli_complete(a->word, choices, a->n);
	}
	/* all the above return, so we proceed with the handler.
	 * we are guaranteed to be called with argc >= e->args;
	 */

	if (argc < e->args + 1)
		return CLI_SHOWUSAGE;

	if (argc == e->args + 1 && !strcasecmp(argv[e->args], "off")) {
		newlevel = 0;
		goto done;
	}
	if (!strcasecmp(argv[e->args], "atleast"))
		atleast = 1;
	if (argc < e->args + atleast + 1 || argc > e->args + atleast + 2)
		return CLI_SHOWUSAGE;
	if (sscanf(argv[e->args + atleast], "%d", &newlevel) != 1)
		return CLI_SHOWUSAGE;

	if (argc == e->args + atleast + 1) {
		debug_filename[0] = '\0';
	} else {
		ast_copy_string(debug_filename, argv[e->args + atleast + 1], sizeof(debug_filename));
	}

done:
	if (!atleast || newlevel > option_debug)
		option_debug = newlevel;

	if (oldval > 0 && option_debug == 0)
		ast_cli(fd, "Core debug is now OFF\n");
	else if (option_debug > 0) {
		if (filename) {
			if (oldval == option_debug)
				ast_cli(fd, "Core debug is at least %d, file '%s'\n", option_debug, filename);
			else
				ast_cli(fd, "Core debug was %d and is now %d, file '%s'\n", oldval, option_debug, filename);
		} else {
			if (oldval == option_debug)
				ast_cli(fd, "Core debug is at least %d\n", option_debug);
			else
				ast_cli(fd, "Core debug was %d and is now %d\n", oldval, option_debug);
		}
	}

	return CLI_SUCCESS;
}

static int handle_logger_mute(int fd, int argc, char *argv[])
{
	if (argc != 2)
		return RESULT_SHOWUSAGE;
	ast_console_toggle_mute(fd);
	return RESULT_SUCCESS;
}

static char *handle_unload(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	/* "module unload mod_1 [mod_2 .. mod_N]" */
	int x;
	int force = AST_FORCE_SOFT;
	char *s;

	switch (cmd) {
	case CLI_INIT:
		e->command = "module unload";
		e->usage =
			"Usage: module unload [-f|-h] <module_1> [<module_2> ... ]\n"
			"       Unloads the specified module from Asterisk. The -f\n"
			"       option causes the module to be unloaded even if it is\n"
			"       in use (may cause a crash) and the -h module causes the\n"
			"       module to be unloaded even if the module says it cannot, \n"
			"       which almost always will cause a crash.\n";
		return NULL;

	case CLI_GENERATE:
		return ast_module_helper(a->line, a->word, a->pos, a->n, a->pos, 0);
	}
	if (a->argc < e->args + 1)
		return CLI_SHOWUSAGE;
	x = e->args;	/* first argument */
	s = a->argv[x];
	if (s[0] == '-') {
		if (s[1] == 'f')
			force = AST_FORCE_FIRM;
		else if (s[1] == 'h')
			force = AST_FORCE_HARD;
		else
			return CLI_SHOWUSAGE;
		if (a->argc < e->args + 2)	/* need at least one module name */
			return CLI_SHOWUSAGE;
		x++;	/* skip this argument */
	}

	for (; x < a->argc; x++) {
		if (ast_unload_resource(a->argv[x], force)) {
			ast_cli(a->fd, "Unable to unload resource %s\n", a->argv[x]);
			return CLI_FAILURE;
		}
	}
	return CLI_SUCCESS;
}

static char *handle_unload_deprecated(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	char *res = handle_unload(e, cmd, a);
	if (cmd == CLI_INIT)
		e->command = "unload";	/* XXX override */
	return res;
}

#define MODLIST_FORMAT  "%-30s %-40.40s %-10d\n"
#define MODLIST_FORMAT2 "%-30s %-40.40s %-10s\n"

AST_MUTEX_DEFINE_STATIC(climodentrylock);
static int climodentryfd = -1;

static int modlist_modentry(const char *module, const char *description, int usecnt, const char *like)
{
	/* Comparing the like with the module */
	if (strcasestr(module, like) ) {
		ast_cli(climodentryfd, MODLIST_FORMAT, module, description, usecnt);
		return 1;
	} 
	return 0;
}

static void print_uptimestr(int fd, time_t timeval, const char *prefix, int printsec)
{
	int x; /* the main part - years, weeks, etc. */
	char timestr[256]="", *s = timestr;
	size_t maxbytes = sizeof(timestr);

#define SECOND (1)
#define MINUTE (SECOND*60)
#define HOUR (MINUTE*60)
#define DAY (HOUR*24)
#define WEEK (DAY*7)
#define YEAR (DAY*365)
#define ESS(x) ((x == 1) ? "" : "s")	/* plural suffix */
#define NEEDCOMMA(x) ((x)? ",": "")	/* define if we need a comma */
	if (timeval < 0)	/* invalid, nothing to show */
		return;
	if (printsec)  {	/* plain seconds output */
		ast_build_string(&s, &maxbytes, "%lu", (u_long)timeval);
		timeval = 0; /* bypass the other cases */
	}
	if (timeval > YEAR) {
		x = (timeval / YEAR);
		timeval -= (x * YEAR);
		ast_build_string(&s, &maxbytes, "%d year%s%s ", x, ESS(x),NEEDCOMMA(timeval));
	}
	if (timeval > WEEK) {
		x = (timeval / WEEK);
		timeval -= (x * WEEK);
		ast_build_string(&s, &maxbytes, "%d week%s%s ", x, ESS(x),NEEDCOMMA(timeval));
	}
	if (timeval > DAY) {
		x = (timeval / DAY);
		timeval -= (x * DAY);
		ast_build_string(&s, &maxbytes, "%d day%s%s ", x, ESS(x),NEEDCOMMA(timeval));
	}
	if (timeval > HOUR) {
		x = (timeval / HOUR);
		timeval -= (x * HOUR);
		ast_build_string(&s, &maxbytes, "%d hour%s%s ", x, ESS(x),NEEDCOMMA(timeval));
	}
	if (timeval > MINUTE) {
		x = (timeval / MINUTE);
		timeval -= (x * MINUTE);
		ast_build_string(&s, &maxbytes, "%d minute%s%s ", x, ESS(x),NEEDCOMMA(timeval));
	}
	x = timeval;
	if (x > 0)
		ast_build_string(&s, &maxbytes, "%d second%s ", x, ESS(x));
	if (timestr[0] != '\0')
		ast_cli(fd, "%s: %s\n", prefix, timestr);
}

static char * handle_showuptime(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	time_t curtime;
	int printsec;

	switch (cmd) {
        case CLI_INIT:
		e->command = "core show uptime";
		e->usage =
			"Usage: core show uptime [seconds]\n"
			"       Shows Asterisk uptime information.\n"
			"       The seconds word returns the uptime in seconds only.\n";
		return NULL;

	case CLI_GENERATE:
		return (a->pos > e->args || a->n > 0) ? NULL : "seconds";
	}
	/* regular handler */
	if (a->argc == e->args+1 && !strcasecmp(a->argv[e->args],"seconds"))
		printsec = 1;
	else if (a->argc == e->args)
		printsec = 0;
	else
		return CLI_SHOWUSAGE;
	curtime = time(NULL);
	if (ast_startuptime)
		print_uptimestr(a->fd, curtime - ast_startuptime, "System uptime", printsec);
	if (ast_lastreloadtime)
		print_uptimestr(a->fd, curtime - ast_lastreloadtime, "Last reload", printsec);
	return CLI_SUCCESS;
}

static char *handle_modlist(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	char *like;

	switch (cmd) {
	case CLI_INIT:
		e->command = "module show";
		e->usage =
			"Usage: module show [like keyword]\n"
			"       Shows Asterisk modules currently in use, and usage statistics.\n";
		return NULL;

	case CLI_GENERATE:
		if (a->pos == e->args)
			return a->n == 0 ? strdup("like") : NULL;
		else if (a->pos == e->args+1 && strcasestr(a->line," like "))
			return ast_module_helper(a->line, a->word, a->pos, a->n, a->pos, 0);
		else
			return NULL;
	}
	/* all the above return, so we proceed with the handler.
	 * we are guaranteed to have argc >= e->args
	 */
	if (a->argc == e->args)
		like = "";
	else if (a->argc == e->args + 2 && !strcmp(a->argv[e->args],"like"))
		like = a->argv[e->args + 1];
	else
		return CLI_SHOWUSAGE;
		
	ast_mutex_lock(&climodentrylock);
	climodentryfd = a->fd; /* global, protected by climodentrylock */
	ast_cli(a->fd, MODLIST_FORMAT2, "Module", "Description", "Use Count");
	ast_cli(a->fd,"%d modules loaded\n", ast_update_module_list(modlist_modentry, like));
	climodentryfd = -1;
	ast_mutex_unlock(&climodentrylock);
	return RESULT_SUCCESS;
}
#undef MODLIST_FORMAT
#undef MODLIST_FORMAT2

/* core show channels [concise|verbose] */
static int handle_chanlist(int fd, int argc, char *argv[])
{
#define FORMAT_STRING  "%-20.20s %-20.20s %-7.7s %-30.30s\n"
#define FORMAT_STRING2 "%-20.20s %-20.20s %-7.7s %-30.30s\n"
#define CONCISE_FORMAT_STRING  "%s!%s!%s!%d!%s!%s!%s!%s!%s!%d!%s!%s\n"
#define VERBOSE_FORMAT_STRING  "%-20.20s %-20.20s %-16.16s %4d %-7.7s %-12.12s %-25.25s %-15.15s %8.8s %-11.11s %-20.20s\n"
#define VERBOSE_FORMAT_STRING2 "%-20.20s %-20.20s %-16.16s %-4.4s %-7.7s %-12.12s %-25.25s %-15.15s %8.8s %-11.11s %-20.20s\n"

	struct ast_channel *c = NULL;
	char durbuf[10] = "-";
	char locbuf[40];
	char appdata[40];
	int duration;
	int durh, durm, durs;
	int numchans = 0, concise = 0, verbose = 0;

	concise = (argc == 4 && (!strcasecmp(argv[3],"concise")));
	verbose = (argc == 4 && (!strcasecmp(argv[3],"verbose")));

	if (argc < 3 || argc > 4 || (argc == 4 && !concise && !verbose))
		return RESULT_SHOWUSAGE;

	if (!concise && !verbose)
		ast_cli(fd, FORMAT_STRING2, "Channel", "Location", "State", "Application(Data)");
	else if (verbose)
		ast_cli(fd, VERBOSE_FORMAT_STRING2, "Channel", "Context", "Extension", "Priority", "State", "Application", "Data", 
		        "CallerID", "Duration", "Accountcode", "BridgedTo");

	while ((c = ast_channel_walk_locked(c)) != NULL) {
		struct ast_channel *bc = ast_bridged_channel(c);
		if ((concise || verbose)  && c->cdr && !ast_tvzero(c->cdr->start)) {
			duration = (int)(ast_tvdiff_ms(ast_tvnow(), c->cdr->start) / 1000);
			if (verbose) {
				durh = duration / 3600;
				durm = (duration % 3600) / 60;
				durs = duration % 60;
				snprintf(durbuf, sizeof(durbuf), "%02d:%02d:%02d", durh, durm, durs);
			} else {
				snprintf(durbuf, sizeof(durbuf), "%d", duration);
			}				
		} else {
			durbuf[0] = '\0';
		}
		if (concise) {
			ast_cli(fd, CONCISE_FORMAT_STRING, c->name, c->context, c->exten, c->priority, ast_state2str(c->_state),
			        c->appl ? c->appl : "(None)",
				S_OR(c->data, ""),	/* XXX different from verbose ? */
			        S_OR(c->cid.cid_num, ""),
			        S_OR(c->accountcode, ""),
				c->amaflags, 
			        durbuf,
				bc ? bc->name : "(None)");
		} else if (verbose) {
			ast_cli(fd, VERBOSE_FORMAT_STRING, c->name, c->context, c->exten, c->priority, ast_state2str(c->_state),
			        c->appl ? c->appl : "(None)",
				c->data ? S_OR(c->data, "(Empty)" ): "(None)",
			        S_OR(c->cid.cid_num, ""),
				durbuf,
			        S_OR(c->accountcode, ""),
				bc ? bc->name : "(None)");
		} else {
			if (!ast_strlen_zero(c->context) && !ast_strlen_zero(c->exten)) 
				snprintf(locbuf, sizeof(locbuf), "%s@%s:%d", c->exten, c->context, c->priority);
			else
				strcpy(locbuf, "(None)");
			if (c->appl)
				snprintf(appdata, sizeof(appdata), "%s(%s)", c->appl, c->data ? c->data : "");
			else
				strcpy(appdata, "(None)");
			ast_cli(fd, FORMAT_STRING, c->name, locbuf, ast_state2str(c->_state), appdata);
		}
		numchans++;
		ast_channel_unlock(c);
	}
	if (!concise) {
		ast_cli(fd, "%d active channel%s\n", numchans, ESS(numchans));
		if (option_maxcalls)
			ast_cli(fd, "%d of %d max active call%s (%5.2f%% of capacity)\n",
				ast_active_calls(), option_maxcalls, ESS(ast_active_calls()),
				((double)ast_active_calls() / (double)option_maxcalls) * 100.0);
		else
			ast_cli(fd, "%d active call%s\n", ast_active_calls(), ESS(ast_active_calls()));
	}
	return RESULT_SUCCESS;
	
#undef FORMAT_STRING
#undef FORMAT_STRING2
#undef CONCISE_FORMAT_STRING
#undef VERBOSE_FORMAT_STRING
#undef VERBOSE_FORMAT_STRING2
}

static char showchan_help[] = 
"Usage: core show channel <channel>\n"
"       Shows lots of information about the specified channel.\n";

static char commandcomplete_help[] = 
"Usage: _command complete \"<line>\" text state\n"
"       This function is used internally to help with command completion and should.\n"
"       never be called by the user directly.\n";

static char commandnummatches_help[] = 
"Usage: _command nummatches \"<line>\" text \n"
"       This function is used internally to help with command completion and should.\n"
"       never be called by the user directly.\n";

static char commandmatchesarray_help[] = 
"Usage: _command matchesarray \"<line>\" text \n"
"       This function is used internally to help with command completion and should.\n"
"       never be called by the user directly.\n";

static int handle_softhangup(int fd, int argc, char *argv[])
{
	struct ast_channel *c=NULL;
	if (argc != 3)
		return RESULT_SHOWUSAGE;
	c = ast_get_channel_by_name_locked(argv[2]);
	if (c) {
		ast_cli(fd, "Requested Hangup on channel '%s'\n", c->name);
		ast_softhangup(c, AST_SOFTHANGUP_EXPLICIT);
		ast_channel_unlock(c);
	} else
		ast_cli(fd, "%s is not a known channel\n", argv[2]);
	return RESULT_SUCCESS;
}

static char *__ast_cli_generator(const char *text, const char *word, int state, int lock);

static int handle_commandmatchesarray(int fd, int argc, char *argv[])
{
	char *buf, *obuf;
	int buflen = 2048;
	int len = 0;
	char **matches;
	int x, matchlen;

	if (argc != 4)
		return RESULT_SHOWUSAGE;
	if (!(buf = ast_malloc(buflen)))
		return RESULT_FAILURE;
	buf[len] = '\0';
	matches = ast_cli_completion_matches(argv[2], argv[3]);
	if (matches) {
		for (x=0; matches[x]; x++) {
			matchlen = strlen(matches[x]) + 1;
			if (len + matchlen >= buflen) {
				buflen += matchlen * 3;
				obuf = buf;
				if (!(buf = ast_realloc(obuf, buflen))) 
					/* Memory allocation failure...  Just free old buffer and be done */
					free(obuf);
			}
			if (buf)
				len += sprintf( buf + len, "%s ", matches[x]);
			free(matches[x]);
			matches[x] = NULL;
		}
		free(matches);
	}

	if (buf) {
		ast_cli(fd, "%s%s",buf, AST_CLI_COMPLETE_EOF);
		free(buf);
	} else
		ast_cli(fd, "NULL\n");

	return RESULT_SUCCESS;
}



static int handle_commandnummatches(int fd, int argc, char *argv[])
{
	int matches = 0;

	if (argc != 4)
		return RESULT_SHOWUSAGE;

	matches = ast_cli_generatornummatches(argv[2], argv[3]);

	ast_cli(fd, "%d", matches);

	return RESULT_SUCCESS;
}

static int handle_commandcomplete(int fd, int argc, char *argv[])
{
	char *buf;

	if (argc != 5)
		return RESULT_SHOWUSAGE;
	buf = __ast_cli_generator(argv[2], argv[3], atoi(argv[4]), 0);
	if (buf) {
		ast_cli(fd, buf);
		free(buf);
	} else
		ast_cli(fd, "NULL\n");
	return RESULT_SUCCESS;
}

static char *handle_core_set_debug_channel(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	struct ast_channel *c = NULL;
	int is_all, is_off = 0;

	switch (cmd) {
	case CLI_INIT:
		e->command = "core set debug channel";
		e->usage =
			"Usage: core set debug channel <all|channel> [off]\n"
			"       Enables/disables debugging on all or on a specific channel.\n";
		return NULL;

	case CLI_GENERATE:
		/* XXX remember to handle the optional "off" */
		if (a->pos != e->args)
			return NULL;
		return a->n == 0 ? strdup("all") : ast_complete_channels(a->line, a->word, a->pos, a->n - 1, e->args);
	}
	/* 'core set debug channel {all|chan_id}' */
	if (a->argc == e->args + 2) {
		if (!strcasecmp(a->argv[e->args + 1], "off"))
			is_off = 1;
		else
			return CLI_SHOWUSAGE;
	} else if (a->argc != e->args + 1)
		return CLI_SHOWUSAGE;

	is_all = !strcasecmp("all", a->argv[e->args]);
	if (is_all) {
		if (is_off) {
			global_fin &= ~DEBUGCHAN_FLAG;
			global_fout &= ~DEBUGCHAN_FLAG;
		} else {
			global_fin |= DEBUGCHAN_FLAG;
			global_fout |= DEBUGCHAN_FLAG;
		}
		c = ast_channel_walk_locked(NULL);
	} else {
		c = ast_get_channel_by_name_locked(a->argv[e->args]);
		if (c == NULL)
			ast_cli(a->fd, "No such channel %s\n", a->argv[e->args]);
	}
	while (c) {
		if (!(c->fin & DEBUGCHAN_FLAG) || !(c->fout & DEBUGCHAN_FLAG)) {
			if (is_off) {
				c->fin &= ~DEBUGCHAN_FLAG;
				c->fout &= ~DEBUGCHAN_FLAG;
			} else {
				c->fin |= DEBUGCHAN_FLAG;
				c->fout |= DEBUGCHAN_FLAG;
			}
			ast_cli(a->fd, "Debugging %s on channel %s\n", is_off ? "disabled" : "enabled", c->name);
		}
		ast_channel_unlock(c);
		if (!is_all)
			break;
		c = ast_channel_walk_locked(c);
	}
	ast_cli(a->fd, "Debugging on new channels is %s\n", is_off ? "disabled" : "enabled");
	return RESULT_SUCCESS;
}

static char *handle_debugchan_deprecated(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	char *res;

	if (cmd == CLI_HANDLER && a->argc != e->args + 1)
		return CLI_SHOWUSAGE;
	res = handle_core_set_debug_channel(e, cmd, a);
	if (cmd == CLI_INIT)
		e->command = "debug channel";
	return res;
}

static char *handle_nodebugchan_deprecated(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	char *res;
	if (cmd == CLI_HANDLER) {
		if (a->argc != e->args + 1)
			return CLI_SHOWUSAGE;
		/* pretend we have an extra "off" at the end. We can do this as the array
		 * is NULL terminated so we overwrite that entry.
		 */
		a->argv[e->args+1] = "off";
		a->argc++;
	}
	res = handle_core_set_debug_channel(e, cmd, a);
	if (cmd == CLI_INIT)
		e->command = "no debug channel";
	return res;
}
		
static int handle_showchan(int fd, int argc, char *argv[])
{
	struct ast_channel *c=NULL;
	struct timeval now;
	char buf[2048];
	char cdrtime[256];
	char nf[256], wf[256], rf[256];
	long elapsed_seconds=0;
	int hour=0, min=0, sec=0;
	
	if (argc != 4)
		return RESULT_SHOWUSAGE;
	now = ast_tvnow();
	c = ast_get_channel_by_name_locked(argv[3]);
	if (!c) {
		ast_cli(fd, "%s is not a known channel\n", argv[3]);
		return RESULT_SUCCESS;
	}
	if(c->cdr) {
		elapsed_seconds = now.tv_sec - c->cdr->start.tv_sec;
		hour = elapsed_seconds / 3600;
		min = (elapsed_seconds % 3600) / 60;
		sec = elapsed_seconds % 60;
		snprintf(cdrtime, sizeof(cdrtime), "%dh%dm%ds", hour, min, sec);
	} else
		strcpy(cdrtime, "N/A");
	ast_cli(fd, 
		" -- General --\n"
		"           Name: %s\n"
		"           Type: %s\n"
		"       UniqueID: %s\n"
		"      Caller ID: %s\n"
		" Caller ID Name: %s\n"
		"    DNID Digits: %s\n"
		"          State: %s (%d)\n"
		"          Rings: %d\n"
		"  NativeFormats: %s\n"
		"    WriteFormat: %s\n"
		"     ReadFormat: %s\n"
		" WriteTranscode: %s\n"
		"  ReadTranscode: %s\n"
		"1st File Descriptor: %d\n"
		"      Frames in: %d%s\n"
		"     Frames out: %d%s\n"
		" Time to Hangup: %ld\n"
		"   Elapsed Time: %s\n"
		"  Direct Bridge: %s\n"
		"Indirect Bridge: %s\n"
		" --   PBX   --\n"
		"        Context: %s\n"
		"      Extension: %s\n"
		"       Priority: %d\n"
		"     Call Group: %llu\n"
		"   Pickup Group: %llu\n"
		"    Application: %s\n"
		"           Data: %s\n"
		"    Blocking in: %s\n",
		c->name, c->tech->type, c->uniqueid,
		S_OR(c->cid.cid_num, "(N/A)"),
		S_OR(c->cid.cid_name, "(N/A)"),
		S_OR(c->cid.cid_dnid, "(N/A)"), ast_state2str(c->_state), c->_state, c->rings, 
		ast_getformatname_multiple(nf, sizeof(nf), c->nativeformats), 
		ast_getformatname_multiple(wf, sizeof(wf), c->writeformat), 
		ast_getformatname_multiple(rf, sizeof(rf), c->readformat),
		c->writetrans ? "Yes" : "No",
		c->readtrans ? "Yes" : "No",
		c->fds[0],
		c->fin & ~DEBUGCHAN_FLAG, (c->fin & DEBUGCHAN_FLAG) ? " (DEBUGGED)" : "",
		c->fout & ~DEBUGCHAN_FLAG, (c->fout & DEBUGCHAN_FLAG) ? " (DEBUGGED)" : "",
		(long)c->whentohangup,
		cdrtime, c->_bridge ? c->_bridge->name : "<none>", ast_bridged_channel(c) ? ast_bridged_channel(c)->name : "<none>", 
		c->context, c->exten, c->priority, c->callgroup, c->pickupgroup, ( c->appl ? c->appl : "(N/A)" ),
		( c-> data ? S_OR(c->data, "(Empty)") : "(None)"),
		(ast_test_flag(c, AST_FLAG_BLOCKING) ? c->blockproc : "(Not Blocking)"));
	
	if(pbx_builtin_serialize_variables(c,buf,sizeof(buf)))
		ast_cli(fd,"      Variables:\n%s\n",buf);
	if(c->cdr && ast_cdr_serialize_variables(c->cdr,buf, sizeof(buf), '=', '\n', 1))
		ast_cli(fd,"  CDR Variables:\n%s\n",buf);
	
	ast_channel_unlock(c);
	return RESULT_SUCCESS;
}

/*
 * helper function to generate CLI matches from a fixed set of values.
 * A NULL word is acceptable.
 */
char *ast_cli_complete(const char *word, char *const choices[], int state)
{
	int i, which = 0, len;
	len = ast_strlen_zero(word) ? 0 : strlen(word);

	for (i = 0; choices[i]; i++) {
		if ((!len || !strncasecmp(word, choices[i], len)) && ++which > state)
			return ast_strdup(choices[i]);
	}
	return NULL;
}

static char *complete_show_channels(const char *line, const char *word, int pos, int state)
{
	static char *choices[] = { "concise", "verbose", NULL };

	return (pos != 3) ? NULL : ast_cli_complete(word, choices, state);
}

char *ast_complete_channels(const char *line, const char *word, int pos, int state, int rpos)
{
	struct ast_channel *c = NULL;
	int which = 0;
	int wordlen;
	char notfound = '\0';
	char *ret = &notfound; /* so NULL can break the loop */

	if (pos != rpos)
		return NULL;

	wordlen = strlen(word);	

	while (ret == &notfound && (c = ast_channel_walk_locked(c))) {
		if (!strncasecmp(word, c->name, wordlen) && ++which > state)
			ret = ast_strdup(c->name);
		ast_channel_unlock(c);
	}
	return ret == &notfound ? NULL : ret;
}

static char *complete_ch_3(const char *line, const char *word, int pos, int state)
{
	return ast_complete_channels(line, word, pos, state, 2);
}

static char *complete_ch_4(const char *line, const char *word, int pos, int state)
{
	return ast_complete_channels(line, word, pos, state, 3);
}

static char *complete_ch_5(const char *line, const char *word, int pos, int state)
{
	return ast_complete_channels(line, word, pos, state, 4);
}


static int group_show_channels(int fd, int argc, char *argv[])
{
#define FORMAT_STRING  "%-25s  %-20s  %-20s\n"

	struct ast_channel *c = NULL;
	int numchans = 0;
	struct ast_var_t *current;
	struct varshead *headp;
	regex_t regexbuf;
	int havepattern = 0;

	if (argc < 3 || argc > 4)
		return RESULT_SHOWUSAGE;
	
	if (argc == 4) {
		if (regcomp(&regexbuf, argv[3], REG_EXTENDED | REG_NOSUB))
			return RESULT_SHOWUSAGE;
		havepattern = 1;
	}

	ast_cli(fd, FORMAT_STRING, "Channel", "Group", "Category");
	while ( (c = ast_channel_walk_locked(c)) != NULL) {
		headp=&c->varshead;
		AST_LIST_TRAVERSE(headp,current,entries) {
			if (!strncmp(ast_var_name(current), GROUP_CATEGORY_PREFIX "_", strlen(GROUP_CATEGORY_PREFIX) + 1)) {
				if (!havepattern || !regexec(&regexbuf, ast_var_value(current), 0, NULL, 0)) {
					ast_cli(fd, FORMAT_STRING, c->name, ast_var_value(current),
						(ast_var_name(current) + strlen(GROUP_CATEGORY_PREFIX) + 1));
					numchans++;
				}
			} else if (!strcmp(ast_var_name(current), GROUP_CATEGORY_PREFIX)) {
				if (!havepattern || !regexec(&regexbuf, ast_var_value(current), 0, NULL, 0)) {
					ast_cli(fd, FORMAT_STRING, c->name, ast_var_value(current), "(default)");
					numchans++;
				}
			}
		}
		numchans++;
		ast_channel_unlock(c);
	}

	if (havepattern)
		regfree(&regexbuf);

	ast_cli(fd, "%d active channel%s\n", numchans, (numchans != 1) ? "s" : "");
	return RESULT_SUCCESS;
#undef FORMAT_STRING
}

static int handle_help(int fd, int argc, char *argv[]);

static char * complete_help(const char *text, const char *word, int pos, int state)
{
	/* skip first 4 or 5 chars, "help "*/
	int l = strlen(text);

	if (l > 5)
		l = 5;
	text += l;
	/* XXX watch out, should stop to the non-generator parts */
	return __ast_cli_generator(text, word, state, 0);
}

/* XXX Nothing in this array can currently be deprecated...
   You have to change the way find_cli works in order to remove this array
   I recommend doing this eventually...
 */
static struct ast_cli_entry builtins[] = {
	/* Keep alphabetized, with longer matches first (example: abcd before abc) */
	{ { "_command", "complete", NULL },
	handle_commandcomplete, "Command complete",
	commandcomplete_help },

	{ { "_command", "nummatches", NULL },
	handle_commandnummatches, "Returns number of command matches",
	commandnummatches_help },

	{ { "_command", "matchesarray", NULL },
	handle_commandmatchesarray, "Returns command matches array",
	commandmatchesarray_help },

	{ { NULL }, NULL, NULL, NULL }
};

static struct ast_cli_entry cli_debug_channel_deprecated = NEW_CLI(handle_debugchan_deprecated, "Enable debugging on channel");
static struct ast_cli_entry cli_module_load_deprecated = NEW_CLI(handle_load_deprecated, "Load a module");
static struct ast_cli_entry cli_module_reload_deprecated = NEW_CLI(handle_reload_deprecated, "reload modules by name");
static struct ast_cli_entry cli_module_unload_deprecated = NEW_CLI(handle_unload_deprecated, "unload modules by name");

static struct ast_cli_entry cli_cli[] = {
	/* Deprecated, but preferred command is now consolidated (and already has a deprecated command for it). */
	NEW_CLI(handle_nodebugchan_deprecated, "Disable debugging on channel(s)"),

	{ { "core", "show", "channels", NULL },
	handle_chanlist, "Display information on channels",
	chanlist_help, complete_show_channels },

	{ { "core", "show", "channel", NULL },
	handle_showchan, "Display information on a specific channel",
	showchan_help, complete_ch_4 },

	NEW_CLI(handle_core_set_debug_channel, "Enable/disable debugging on a channel",
		.deprecate_cmd = &cli_debug_channel_deprecated),

	NEW_CLI(handle_set_debug, "Set level of debug chattiness"),

	NEW_CLI(handle_verbose, "Set level of verboseness"),

	{ { "group", "show", "channels", NULL },
	group_show_channels, "Display active channels with group(s)",
	group_show_channels_help },

	{ { "help", NULL },
	handle_help, "Display help list, or specific help on a command",
	help_help, complete_help },

	{ { "logger", "mute", NULL },
	handle_logger_mute, "Toggle logging output to a console",
	logger_mute_help },

	NEW_CLI(handle_modlist, "List modules and info"),

	NEW_CLI(handle_load, "Load a module by name", .deprecate_cmd = &cli_module_load_deprecated),

	NEW_CLI(handle_reload, "Reload configuration", .deprecate_cmd = &cli_module_reload_deprecated),

	NEW_CLI(handle_unload, "Unload a module by name", .deprecate_cmd = &cli_module_unload_deprecated ),

	NEW_CLI(handle_showuptime, "Show uptime information"),

	{ { "soft", "hangup", NULL },
	handle_softhangup, "Request a hangup on a given channel",
	softhangup_help, complete_ch_3 },
};

/*! \brief initialize the _full_cmd string in * each of the builtins. */
void ast_builtins_init(void)
{
	struct ast_cli_entry *e;

	for (e = builtins; e->cmda[0] != NULL; e++) {
		char buf[80];
		ast_join(buf, sizeof(buf), e->cmda);
		e->_full_cmd = strdup(buf);
		if (!e->_full_cmd)
			ast_log(LOG_WARNING, "-- cannot allocate <%s>\n", buf);
	}

	ast_cli_register_multiple(cli_cli, sizeof(cli_cli) / sizeof(struct ast_cli_entry));
}

/*
 * We have two sets of commands: builtins are stored in a
 * NULL-terminated array of ast_cli_entry, whereas external
 * commands are in a list.
 * When navigating, we need to keep two pointers and get
 * the next one in lexicographic order. For the purpose,
 * we use a structure.
 */

struct cli_iterator {
	struct ast_cli_entry *builtins;
	struct ast_cli_entry *helpers;
};

static struct ast_cli_entry *cli_next(struct cli_iterator *i)
{
	struct ast_cli_entry *e;

	if (i->builtins == NULL && i->helpers == NULL) {
		/* initialize */
		i->builtins = builtins;
		i->helpers = AST_LIST_FIRST(&helpers);
	}
	e = i->builtins; /* temporary */
	if (!e->cmda[0] || (i->helpers &&
		    strcmp(i->helpers->_full_cmd, e->_full_cmd) < 0)) {
		/* Use helpers */
		e = i->helpers;
		if (e)
			i->helpers = AST_LIST_NEXT(e, list);
	} else { /* use builtin. e is already set  */
		(i->builtins)++;	/* move to next */
	}
	return e;
}

/*!
 * \brief locate a cli command in the 'helpers' list (which must be locked).
 * exact has 3 values:
 *      0       returns if the search key is equal or longer than the entry.
 *      -1      true if the mismatch is on the last word XXX not true!
 *      1       true only on complete, exact match.
 */
static struct ast_cli_entry *find_cli(char *const cmds[], int match_type)
{
	int matchlen = -1;	/* length of longest match so far */
	struct ast_cli_entry *cand = NULL, *e=NULL;
	struct cli_iterator i = { NULL, NULL};

	while( (e = cli_next(&i)) ) {
		int y;
		for (y = 0 ; cmds[y] && e->cmda[y]; y++) {
			if (strcasecmp(e->cmda[y], cmds[y]))
				break;
		}
		if (e->cmda[y] == NULL) {	/* no more words in candidate */
			if (cmds[y] == NULL)	/* this is an exact match, cannot do better */
				break;
			/* here the search key is longer than the candidate */
			if (match_type != 0)	/* but we look for almost exact match... */
				continue;	/* so we skip this one. */
			/* otherwise we like it (case 0) */
		} else {			/* still words in candidate */
			if (cmds[y] == NULL)	/* search key is shorter, not good */
				continue;
			/* if we get here, both words exist but there is a mismatch */
			if (match_type == 0)	/* not the one we look for */
				continue;
			if (match_type == 1)	/* not the one we look for */
				continue;
			if (cmds[y+1] != NULL || e->cmda[y+1] != NULL)	/* not the one we look for */
				continue;
			/* we are in case match_type == -1 and mismatch on last word */
		}
		if (y > matchlen) {	/* remember the candidate */
			matchlen = y;
			cand = e;
		}
	}
	return e ? e : cand;
}

static char *find_best(char *argv[])
{
	static char cmdline[80];
	int x;
	/* See how close we get, then print the candidate */
	char *myargv[AST_MAX_CMD_LEN];
	for (x=0;x<AST_MAX_CMD_LEN;x++)
		myargv[x]=NULL;
	AST_LIST_LOCK(&helpers);
	for (x=0;argv[x];x++) {
		myargv[x] = argv[x];
		if (!find_cli(myargv, -1))
			break;
	}
	AST_LIST_UNLOCK(&helpers);
	ast_join(cmdline, sizeof(cmdline), myargv);
	return cmdline;
}

static int __ast_cli_unregister(struct ast_cli_entry *e, struct ast_cli_entry *ed)
{
	if (e->deprecate_cmd) {
		__ast_cli_unregister(e->deprecate_cmd, e);
	}
	if (e->inuse) {
		ast_log(LOG_WARNING, "Can't remove command that is in use\n");
	} else {
		AST_LIST_LOCK(&helpers);
		AST_LIST_REMOVE(&helpers, e, list);
		AST_LIST_UNLOCK(&helpers);
		free(e->_full_cmd);
		e->_full_cmd = NULL;
		if (e->new_handler) {
			/* this is a new-style entry. Reset fields and free memory. */
			bzero((char **)(e->cmda), sizeof(e->cmda));
			free(e->command);
			e->command = NULL;
			e->usage = NULL;
		}
	}
	return 0;
}

static int __ast_cli_register(struct ast_cli_entry *e, struct ast_cli_entry *ed)
{
	struct ast_cli_entry *cur;
	char fulle[80] ="";
	int i, lf, ret = -1;

	if (e->handler == NULL) {	/* new style entry, run the handler to init fields */
		struct ast_cli_args a;	/* fake argument */
		char **dst = (char **)e->cmda;	/* need to cast as the entry is readonly */
		char *s;

		bzero (&a, sizeof(a));
		e->new_handler(e, CLI_INIT, &a);
		/* XXX check that usage and command are filled up */
		s = ast_skip_blanks(e->command);
		s = e->command = ast_strdup(s);
		for (i=0; !ast_strlen_zero(s) && i < AST_MAX_CMD_LEN-1; i++) {
			*dst++ = s;	/* store string */
			s = ast_skip_nonblanks(s);
			if (*s == '\0')	/* we are done */
				break;
			*s++ = '\0';
			s = ast_skip_blanks(s);
		}
		*dst++ = NULL;
	}
	for (i = 0; e->cmda[i]; i++)
		;
	e->args = i;
	ast_join(fulle, sizeof(fulle), e->cmda);
	AST_LIST_LOCK(&helpers);
	
	if (find_cli(e->cmda, 1)) {
		AST_LIST_UNLOCK(&helpers);
		ast_log(LOG_WARNING, "Command '%s' already registered (or something close enough)\n", fulle);
		goto done;
	}
	e->_full_cmd = ast_strdup(fulle);
	if (!e->_full_cmd)
		goto done;

	if (ed) {
		e->deprecated = 1;
		e->summary = ed->summary;
		e->usage = ed->usage;
		/* XXX If command A deprecates command B, and command B deprecates command C...
		   Do we want to show command A or command B when telling the user to use new syntax?
		   This currently would show command A.
		   To show command B, you just need to always use ed->_full_cmd.
		 */
		e->_deprecated_by = S_OR(ed->_deprecated_by, ed->_full_cmd);
	} else {
		e->deprecated = 0;
	}

	lf = strlen(fulle);
	AST_LIST_TRAVERSE_SAFE_BEGIN(&helpers, cur, list) {
		int len = strlen(cur->_full_cmd);
		if (lf < len)
			len = lf;
		if (strncasecmp(fulle, cur->_full_cmd, len) < 0) {
			AST_LIST_INSERT_BEFORE_CURRENT(&helpers, e, list); 
			break;
		}
	}
	AST_LIST_TRAVERSE_SAFE_END;

	if (!cur)
		AST_LIST_INSERT_TAIL(&helpers, e, list); 
	ret = 0;	/* success */

done:
	AST_LIST_UNLOCK(&helpers);

	if (e->deprecate_cmd) {
		/* This command deprecates another command.  Register that one also. */
		__ast_cli_register(e->deprecate_cmd, e);
	}
	
	return ret;
}

/* wrapper function, so we can unregister deprecated commands recursively */
int ast_cli_unregister(struct ast_cli_entry *e)
{
	return __ast_cli_unregister(e, NULL);
}

/* wrapper function, so we can register deprecated commands recursively */
int ast_cli_register(struct ast_cli_entry *e)
{
	return __ast_cli_register(e, NULL);
}

/*
 * register/unregister an array of entries.
 */
void ast_cli_register_multiple(struct ast_cli_entry *e, int len)
{
	int i;

	for (i = 0; i < len; i++)
		ast_cli_register(e + i);
}

void ast_cli_unregister_multiple(struct ast_cli_entry *e, int len)
{
	int i;

	for (i = 0; i < len; i++)
		ast_cli_unregister(e + i);
}


/*! \brief helper for help_workhorse and final part of
 * handle_help. if locked = 0 it's just help_workhorse,
 * otherwise assume the list is already locked and print
 * an error message if not found.
 */
static int help1(int fd, char *match[], int locked)
{
	char matchstr[80] = "";
	struct ast_cli_entry *e;
	int len = 0;
	int found = 0;
	struct cli_iterator i = { NULL, NULL};

	if (match) {
		ast_join(matchstr, sizeof(matchstr), match);
		len = strlen(matchstr);
	}
	if (!locked)
		AST_LIST_LOCK(&helpers);
	while ( (e = cli_next(&i)) ) {
		/* Hide commands that start with '_' */
		if (e->_full_cmd[0] == '_')
			continue;
		/* Hide commands that are marked as deprecated. */
		if (e->deprecated)
			continue;
		if (match && strncasecmp(matchstr, e->_full_cmd, len))
			continue;
		ast_cli(fd, "%25.25s  %s\n", e->_full_cmd, S_OR(e->summary, "<no description available>"));
		found++;
	}
	AST_LIST_UNLOCK(&helpers);
	if (!locked && !found && matchstr[0])
		ast_cli(fd, "No such command '%s'.\n", matchstr);
	return 0;
}

static int help_workhorse(int fd, char *match[])
{
	return help1(fd, match, 0 /* do not print errors */);
}

static int handle_help(int fd, int argc, char *argv[])
{
	char fullcmd[80];
	struct ast_cli_entry *e;

	if (argc < 1)
		return RESULT_SHOWUSAGE;
	if (argc == 1)
		return help_workhorse(fd, NULL);

	AST_LIST_LOCK(&helpers);
	e = find_cli(argv + 1, 1);	/* try exact match first */
	if (!e)
		return help1(fd, argv + 1, 1 /* locked */);
	if (e->usage)
		ast_cli(fd, "%s", e->usage);
	else {
		ast_join(fullcmd, sizeof(fullcmd), argv+1);
		ast_cli(fd, "No help text available for '%s'.\n", fullcmd);
	}
	AST_LIST_UNLOCK(&helpers);
	return RESULT_SUCCESS;
}

static char *parse_args(const char *s, int *argc, char *argv[], int max, int *trailingwhitespace)
{
	char *dup, *cur;
	int x = 0;
	int quoted = 0;
	int escaped = 0;
	int whitespace = 1;

	*trailingwhitespace = 0;
	if (s == NULL)	/* invalid, though! */
		return NULL;
	/* make a copy to store the parsed string */
	if (!(dup = ast_strdup(s)))
		return NULL;

	cur = dup;
	/* scan the original string copying into cur when needed */
	for (; *s ; s++) {
		if (x >= max - 1) {
			ast_log(LOG_WARNING, "Too many arguments, truncating at %s\n", s);
			break;
		}
		if (*s == '"' && !escaped) {
			quoted = !quoted;
			if (quoted && whitespace) {
				/* start a quoted string from previous whitespace: new argument */
				argv[x++] = cur;
				whitespace = 0;
			}
		} else if ((*s == ' ' || *s == '\t') && !(quoted || escaped)) {
			/* If we are not already in whitespace, and not in a quoted string or
			   processing an escape sequence, and just entered whitespace, then
			   finalize the previous argument and remember that we are in whitespace
			*/
			if (!whitespace) {
				*cur++ = '\0';
				whitespace = 1;
			}
		} else if (*s == '\\' && !escaped) {
			escaped = 1;
		} else {
			if (whitespace) {
				/* we leave whitespace, and are not quoted. So it's a new argument */
				argv[x++] = cur;
				whitespace = 0;
			}
			*cur++ = *s;
			escaped = 0;
		}
	}
	/* Null terminate */
	*cur++ = '\0';
	/* XXX put a NULL in the last argument, because some functions that take
	 * the array may want a null-terminated array.
	 * argc still reflects the number of non-NULL entries.
	 */
	argv[x] = NULL;
	*argc = x;
	*trailingwhitespace = whitespace;
	return dup;
}

/*! \brief Return the number of unique matches for the generator */
int ast_cli_generatornummatches(const char *text, const char *word)
{
	int matches = 0, i = 0;
	char *buf = NULL, *oldbuf = NULL;

	while ((buf = ast_cli_generator(text, word, i++))) {
		if (!oldbuf || strcmp(buf,oldbuf))
			matches++;
		if (oldbuf)
			free(oldbuf);
		oldbuf = buf;
	}
	if (oldbuf)
		free(oldbuf);
	return matches;
}

char **ast_cli_completion_matches(const char *text, const char *word)
{
	char **match_list = NULL, *retstr, *prevstr;
	size_t match_list_len, max_equal, which, i;
	int matches = 0;

	/* leave entry 0 free for the longest common substring */
	match_list_len = 1;
	while ((retstr = ast_cli_generator(text, word, matches)) != NULL) {
		if (matches + 1 >= match_list_len) {
			match_list_len <<= 1;
			if (!(match_list = ast_realloc(match_list, match_list_len * sizeof(*match_list))))
				return NULL;
		}
		match_list[++matches] = retstr;
	}

	if (!match_list)
		return match_list; /* NULL */

	/* Find the longest substring that is common to all results
	 * (it is a candidate for completion), and store a copy in entry 0.
	 */
	prevstr = match_list[1];
	max_equal = strlen(prevstr);
	for (which = 2; which <= matches; which++) {
		for (i = 0; i < max_equal && toupper(prevstr[i]) == toupper(match_list[which][i]); i++)
			continue;
		max_equal = i;
	}

	if (!(retstr = ast_malloc(max_equal + 1)))
		return NULL;
	
	ast_copy_string(retstr, match_list[1], max_equal + 1);
	match_list[0] = retstr;

	/* ensure that the array is NULL terminated */
	if (matches + 1 >= match_list_len) {
		if (!(match_list = ast_realloc(match_list, (match_list_len + 1) * sizeof(*match_list))))
			return NULL;
	}
	match_list[matches + 1] = NULL;

	return match_list;
}

static char *__ast_cli_generator(const char *text, const char *word, int state, int lock)
{
	char *argv[AST_MAX_ARGS];
	struct ast_cli_entry *e;
	struct cli_iterator i = { NULL, NULL };
	int x = 0, argindex, matchlen;
	int matchnum=0;
	char *ret = NULL;
	char matchstr[80] = "";
	int tws = 0;
	char *dup = parse_args(text, &x, argv, sizeof(argv) / sizeof(argv[0]), &tws);

	if (!dup)	/* error */
		return NULL;
	argindex = (!ast_strlen_zero(word) && x>0) ? x-1 : x;
	/* rebuild the command, ignore tws */
	ast_join(matchstr, sizeof(matchstr)-1, argv);
	matchlen = strlen(matchstr);
	if (tws) {
		strcat(matchstr, " "); /* XXX */
		if (matchlen)
			matchlen++;
	}
	if (lock)
		AST_LIST_LOCK(&helpers);
	while ( (e = cli_next(&i)) ) {
		int lc = strlen(e->_full_cmd);
		if (e->_full_cmd[0] != '_' && lc > 0 && matchlen <= lc &&
				!strncasecmp(matchstr, e->_full_cmd, matchlen)) {
			/* Found initial part, return a copy of the next word... */
			if (e->cmda[argindex] && ++matchnum > state) {
				ret = strdup(e->cmda[argindex]); /* we need a malloced string */
				break;
			}
		} else if (!strncasecmp(matchstr, e->_full_cmd, lc) && matchstr[lc] < 33) {
			/* This entry is a prefix of the command string entered
			 * (only one entry in the list should have this property).
			 * Run the generator if one is available. In any case we are done.
			 */
			if (e->generator)
				ret = e->generator(matchstr, word, argindex, state - matchnum);
			else if (e->new_handler) {	/* new style command */
				struct ast_cli_args a = {
					.line = matchstr, .word = word,
					.pos = argindex,
					.n = state - matchnum };
				ret = e->new_handler(e, CLI_GENERATE, &a);
			}
			if (ret)
				break;
		}
	}
	if (lock)
		AST_LIST_UNLOCK(&helpers);
	free(dup);
	return ret;
}

char *ast_cli_generator(const char *text, const char *word, int state)
{
	return __ast_cli_generator(text, word, state, 1);
}

int ast_cli_command(int fd, const char *s)
{
	char *args[AST_MAX_ARGS + 1];
	struct ast_cli_entry *e;
	int x;
	char *dup;
	int tws;
	
	if (!(dup = parse_args(s, &x, args + 1, AST_MAX_ARGS, &tws)))
		return -1;

	/* We need at least one entry, or ignore */
	if (x > 0) {
		AST_LIST_LOCK(&helpers);
		e = find_cli(args + 1, 0);
		if (e)
			e->inuse++;
		AST_LIST_UNLOCK(&helpers);
		if (e) {
			int res;
			/* within calling the handler, argv[-1] contains a pointer
			 * to the cli entry, and the array is null-terminated
			 */
			args[0] = (char *)e;
			if (e->new_handler) {	/* new style */
				struct ast_cli_args a = {
					.fd = fd, .argc = x, .argv = args+1 };
				res = (int)e->new_handler(e, CLI_HANDLER, &a);
			} else {		/* old style */
				res = e->handler(fd, x, args + 1);
			}
			switch (res) {
			case RESULT_SHOWUSAGE:
				if (e->usage)
					ast_cli(fd, "%s", e->usage);
				else
					ast_cli(fd, "Invalid usage, but no usage information available.\n");
				break;
			default:
				AST_LIST_LOCK(&helpers);
				if (e->deprecated == 1) {
					ast_cli(fd, "The '%s' command is deprecated and will be removed in a future release. Please use '%s' instead.\n", e->_full_cmd, e->_deprecated_by);
					e->deprecated = 2;
				}
				AST_LIST_UNLOCK(&helpers);
				break;
			}
		} else 
			ast_cli(fd, "No such command '%s' (type 'help' for help)\n", find_best(args + 1));
		if (e)
			ast_atomic_fetchadd_int(&e->inuse, -1);
	}
	free(dup);
	
	return 0;
}
