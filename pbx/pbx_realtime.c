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
 * \brief Realtime PBX Module
 *
 * \arg See also: \ref AstARA
 */

#include "asterisk.h"

ASTERISK_FILE_VERSION(__FILE__, "$Revision$")

#include <signal.h>

#include "asterisk/file.h"
#include "asterisk/logger.h"
#include "asterisk/channel.h"
#include "asterisk/config.h"
#include "asterisk/pbx.h"
#include "asterisk/module.h"
#include "asterisk/frame.h"
#include "asterisk/term.h"
#include "asterisk/manager.h"
#include "asterisk/cli.h"
#include "asterisk/lock.h"
#include "asterisk/md5.h"
#include "asterisk/linkedlists.h"
#include "asterisk/chanvars.h"
#include "asterisk/sched.h"
#include "asterisk/io.h"
#include "asterisk/utils.h"
#include "asterisk/crypto.h"
#include "asterisk/astdb.h"
#include "asterisk/app.h"
#include "asterisk/astobj2.h"

#define MODE_MATCH 		0
#define MODE_MATCHMORE 	1
#define MODE_CANMATCH 	2

#define EXT_DATA_SIZE 256

enum option_flags {
	OPTION_PATTERNS_DISABLED = (1 << 0),
};

AST_APP_OPTIONS(switch_opts, {
	AST_APP_OPTION('p', OPTION_PATTERNS_DISABLED),
});

struct cache_entry {
	struct timeval when;
	struct ast_variable *var;
	int priority;
	char *context;
	char exten[2];
};

struct ao2_container *cache;
pthread_t cleanup_thread = 0;

static int cache_hash(const void *obj, const int flags)
{
	const struct cache_entry *e = obj;
	return ast_str_case_hash(e->exten) + e->priority;
}

static int cache_cmp(void *obj, void *arg, int flags)
{
	struct cache_entry *e = obj, *f = arg;
	return e->priority != f->priority ? 0 :
		strcmp(e->exten, f->exten) ? 0 :
		strcmp(e->context, f->context) ? 0 :
		CMP_MATCH;
}

static struct ast_variable *dup_vars(struct ast_variable *v)
{
	struct ast_variable *new, *list = NULL;
	for (; v; v = v->next) {
		if (!(new = ast_variable_new(v->name, v->value, v->file))) {
			ast_variables_destroy(list);
			return NULL;
		}
		/* Reversed list in cache, but when we duplicate out of the cache,
		 * it's back to correct order. */
		new->next = list;
		list = new;
	}
	return list;
}

static void free_entry(void *obj)
{
	struct cache_entry *e = obj;
	ast_variables_destroy(e->var);
}

static int purge_old_fn(void *obj, void *arg, int flags)
{
	struct cache_entry *e = obj;
	struct timeval *now = arg;
	return ast_tvdiff_ms(*now, e->when) >= 1000 ? CMP_MATCH : 0;
}

static void *cleanup(void *unused)
{
	struct timespec forever = { 999999999, 0 }, one_second = { 1, 0 };
	struct timeval now;

	for (;;) {
		pthread_testcancel();
		if (ao2_container_count(cache) == 0) {
			nanosleep(&forever, NULL);
		}
		pthread_testcancel();
		now = ast_tvnow();
		ao2_callback(cache, OBJ_MULTIPLE | OBJ_UNLINK | OBJ_NODATA, purge_old_fn, &now);
		pthread_testcancel();
		nanosleep(&one_second, NULL);
	}

	return NULL;
}


/* Realtime switch looks up extensions in the supplied realtime table.

	[context@][realtimetable][/options]

	If the realtimetable is omitted it is assumed to be "extensions".  If no context is 
	specified the context is assumed to be whatever is the container.

	The realtime table should have entries for context,exten,priority,app,args
	
	The realtime table currently does not support callerid fields.

*/


static struct ast_variable *realtime_switch_common(const char *table, const char *context, const char *exten, int priority, int mode, struct ast_flags flags)
{
	struct ast_variable *var;
	struct ast_config *cfg;
	char pri[20];
	char *ematch;
	char rexten[AST_MAX_EXTENSION + 20]="";
	int match;
	/* Optimization: since we don't support hints in realtime, it's silly to
	 * query for a hint here, since we won't actually do anything with it.
	 * This just wastes CPU time and resources. */
	if (priority < 0) {
		return NULL;
	}
	snprintf(pri, sizeof(pri), "%d", priority);
	switch(mode) {
	case MODE_MATCHMORE:
		ematch = "exten LIKE";
		snprintf(rexten, sizeof(rexten), "%s_%%", exten);
		break;
	case MODE_CANMATCH:
		ematch = "exten LIKE";
		snprintf(rexten, sizeof(rexten), "%s%%", exten);
		break;
	case MODE_MATCH:
	default:
		ematch = "exten";
		ast_copy_string(rexten, exten, sizeof(rexten));
	}
	var = ast_load_realtime(table, ematch, rexten, "context", context, "priority", pri, SENTINEL);
	if (!var && !ast_test_flag(&flags, OPTION_PATTERNS_DISABLED)) {
		cfg = ast_load_realtime_multientry(table, "exten LIKE", "\\_%", "context", context, "priority", pri, SENTINEL);	
		if (cfg) {
			char *cat = ast_category_browse(cfg, NULL);

			while(cat) {
				switch(mode) {
				case MODE_MATCHMORE:
					match = ast_extension_close(cat, exten, 1);
					break;
				case MODE_CANMATCH:
					match = ast_extension_close(cat, exten, 0);
					break;
				case MODE_MATCH:
				default:
					match = ast_extension_match(cat, exten);
				}
				if (match) {
					var = ast_category_detach_variables(ast_category_get(cfg, cat));
					break;
				}
				cat = ast_category_browse(cfg, cat);
			}
			ast_config_destroy(cfg);
		}
	}
	return var;
}

static struct ast_variable *realtime_common(const char *context, const char *exten, int priority, const char *data, int mode)
{
	const char *ctx = NULL;
	char *table;
	struct ast_variable *var=NULL;
	struct ast_flags flags = { 0, };
	struct cache_entry *ce;
	struct {
		struct cache_entry ce;
		char exten[AST_MAX_EXTENSION];
	} cache_search = { { .priority = priority, .context = (char *) context }, };
	char *buf = ast_strdupa(data);
	if (buf) {
		/* "Realtime" prefix is stripped off in the parent engine.  The
		 * remaining string is: [[context@]table][/opts] */
		char *opts = strchr(buf, '/');
		if (opts)
			*opts++ = '\0';
		table = strchr(buf, '@');
		if (table) {
			*table++ = '\0';
			ctx = buf;
		}
		ctx = S_OR(ctx, context);
		table = S_OR(table, "extensions");
		if (!ast_strlen_zero(opts)) {
			ast_app_parse_options(switch_opts, &flags, NULL, opts);
		}
		ast_copy_string(cache_search.exten, exten, sizeof(cache_search.exten));
		if (mode == MODE_MATCH && (ce = ao2_find(cache, &cache_search, OBJ_POINTER))) {
			var = dup_vars(ce->var);
			ao2_ref(ce, -1);
		} else {
			var = realtime_switch_common(table, ctx, exten, priority, mode, flags);
			do {
				struct ast_variable *new;
				/* Only cache matches */
				if (mode != MODE_MATCH) {
					break;
				}
				if (!(new = dup_vars(var))) {
					break;
				}
				if (!(ce = ao2_alloc(sizeof(*ce) + strlen(exten) + strlen(context), free_entry))) {
					ast_variables_destroy(new);
					break;
				}
				ce->context = ce->exten + strlen(exten) + 1;
				strcpy(ce->exten, exten); /* SAFE */
				strcpy(ce->context, context); /* SAFE */
				ce->priority = priority;
				ce->var = new;
				ce->when = ast_tvnow();
				ao2_link(cache, ce);
				pthread_kill(cleanup_thread, SIGURG);
				ao2_ref(ce, -1);
			} while (0);
		}
	}
	return var;
}

static int realtime_exists(struct ast_channel *chan, const char *context, const char *exten, int priority, const char *callerid, const char *data)
{
	struct ast_variable *var = realtime_common(context, exten, priority, data, MODE_MATCH);
	if (var) {
		ast_variables_destroy(var);
		return 1;
	}
	return 0;
}

static int realtime_canmatch(struct ast_channel *chan, const char *context, const char *exten, int priority, const char *callerid, const char *data)
{
	struct ast_variable *var = realtime_common(context, exten, priority, data, MODE_CANMATCH);
	if (var) {
		ast_variables_destroy(var);
		return 1;
	}
	return 0;
}

static int realtime_exec(struct ast_channel *chan, const char *context, const char *exten, int priority, const char *callerid, const char *data)
{
	int res = -1;
	struct ast_variable *var = realtime_common(context, exten, priority, data, MODE_MATCH);

	if (var) {
		char *tmp="";
		char *app = NULL;
		struct ast_variable *v;

		for (v = var; v ; v = v->next) {
			if (!strcasecmp(v->name, "app"))
				app = ast_strdupa(v->value);
			else if (!strcasecmp(v->name, "appdata")) {
				if (ast_compat_pbx_realtime) {
					char *ptr;
					int in = 0;
					tmp = alloca(strlen(v->value) * 2 + 1);
					for (ptr = tmp; *v->value; v->value++) {
						if (*v->value == ',') {
							*ptr++ = '\\';
							*ptr++ = ',';
						} else if (*v->value == '|' && !in) {
							*ptr++ = ',';
						} else {
							*ptr++ = *v->value;
						}

						/* Don't escape '|', meaning 'or', inside expressions ($[ ]) */
						if (v->value[0] == '[' && v->value[-1] == '$') {
							in++;
						} else if (v->value[0] == ']' && in) {
							in--;
						}
					}
					*ptr = '\0';
				} else {
					tmp = ast_strdupa(v->value);
				}
			}
		}
		ast_variables_destroy(var);
		if (!ast_strlen_zero(app)) {
			struct ast_app *a = pbx_findapp(app);
			if (a) {
				char appdata[512];
				char tmp1[80];
				char tmp2[80];
				char tmp3[EXT_DATA_SIZE];

				appdata[0] = 0; /* just in case the substitute var func isn't called */
				if(!ast_strlen_zero(tmp))
					pbx_substitute_variables_helper(chan, tmp, appdata, sizeof(appdata) - 1);
				ast_verb(3, "Executing %s(\"%s\", \"%s\")\n",
						 term_color(tmp1, app, COLOR_BRCYAN, 0, sizeof(tmp1)),
						 term_color(tmp2, chan->name, COLOR_BRMAGENTA, 0, sizeof(tmp2)),
						 term_color(tmp3, S_OR(appdata, ""), COLOR_BRMAGENTA, 0, sizeof(tmp3)));
				manager_event(EVENT_FLAG_DIALPLAN, "Newexten",
							  "Channel: %s\r\n"
							  "Context: %s\r\n"
							  "Extension: %s\r\n"
							  "Priority: %d\r\n"
							  "Application: %s\r\n"
							  "AppData: %s\r\n"
							  "Uniqueid: %s\r\n",
							  chan->name, chan->context, chan->exten, chan->priority, app, !ast_strlen_zero(appdata) ? appdata : "(NULL)", chan->uniqueid);
				
				res = pbx_exec(chan, a, appdata);
			} else
				ast_log(LOG_NOTICE, "No such application '%s' for extension '%s' in context '%s'\n", app, exten, context);
		} else {
			ast_log(LOG_WARNING, "No application specified for realtime extension '%s' in context '%s'\n", exten, context);
		}
	}
	return res;
}

static int realtime_matchmore(struct ast_channel *chan, const char *context, const char *exten, int priority, const char *callerid, const char *data)
{
	struct ast_variable *var = realtime_common(context, exten, priority, data, MODE_MATCHMORE);
	if (var) {
		ast_variables_destroy(var);
		return 1;
	}
	return 0;
}

static struct ast_switch realtime_switch =
{
        name:                   "Realtime",
        description:   		"Realtime Dialplan Switch",
        exists:                 realtime_exists,
        canmatch:               realtime_canmatch,
        exec:                   realtime_exec,
        matchmore:              realtime_matchmore,
};

static int unload_module(void)
{
	ast_unregister_switch(&realtime_switch);
	pthread_cancel(cleanup_thread);
	pthread_kill(cleanup_thread, SIGURG);
	pthread_join(cleanup_thread, NULL);
	/* Destroy all remaining entries */
	ao2_ref(cache, -1);
	return 0;
}

static int load_module(void)
{
	if (!(cache = ao2_container_alloc(573, cache_hash, cache_cmp))) {
		return AST_MODULE_LOAD_FAILURE;
	}

	if (ast_pthread_create(&cleanup_thread, NULL, cleanup, NULL)) {
		return AST_MODULE_LOAD_FAILURE;
	}

	if (ast_register_switch(&realtime_switch))
		return AST_MODULE_LOAD_FAILURE;
	return AST_MODULE_LOAD_SUCCESS;
}

AST_MODULE_INFO_STANDARD(ASTERISK_GPL_KEY, "Realtime Switch");
