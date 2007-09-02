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
 * \brief Configuration File Parser
 *
 * \author Mark Spencer <markster@digium.com>
 *
 * Includes the Asterisk Realtime API - ARA
 * See doc/realtime.txt and doc/extconfig.txt
 */

#include "asterisk.h"

ASTERISK_FILE_VERSION(__FILE__, "$Revision$")

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/socket.h>		/* for AF_INET */
#define AST_INCLUDE_GLOB 1
#ifdef AST_INCLUDE_GLOB
#if defined(__Darwin__) || defined(__CYGWIN__)
#define GLOB_ABORTED GLOB_ABEND
#endif
# include <glob.h>
#endif

#include "asterisk/config.h"
#include "asterisk/cli.h"
#include "asterisk/lock.h"
#include "asterisk/options.h"
#include "asterisk/logger.h"
#include "asterisk/utils.h"
#include "asterisk/channel.h"
#include "asterisk/app.h"

#define MAX_NESTED_COMMENTS 128
#define COMMENT_START ";--"
#define COMMENT_END "--;"
#define COMMENT_META ';'
#define COMMENT_TAG '-'

static char *extconfig_conf = "extconfig.conf";


/*! \brief Structure to keep comments for rewriting configuration files */
struct ast_comment {
	struct ast_comment *next;
	char cmt[0];
};

/*! \brief Hold the mtime for config files, so if we don't need to reread our config, don't. */
struct cache_file_include {
	AST_LIST_ENTRY(cache_file_include) list;
	char include[0];
};

struct cache_file_mtime {
	AST_LIST_ENTRY(cache_file_mtime) list;
	AST_LIST_HEAD(includes, cache_file_include) includes;
	unsigned int has_exec:1;
	time_t mtime;
	char filename[0];
};

static AST_LIST_HEAD_STATIC(cfmtime_head, cache_file_mtime);

#define CB_INCR 250

static void CB_INIT(char **comment_buffer, int *comment_buffer_size, char **lline_buffer, int *lline_buffer_size)
{
	if (!(*comment_buffer)) {
		*comment_buffer = ast_malloc(CB_INCR);
		if (!(*comment_buffer))
			return;
		(*comment_buffer)[0] = 0;
		*comment_buffer_size = CB_INCR;
		*lline_buffer = ast_malloc(CB_INCR);
		if (!(*lline_buffer))
			return;
		(*lline_buffer)[0] = 0;
		*lline_buffer_size = CB_INCR;
	} else {
		(*comment_buffer)[0] = 0;
		(*lline_buffer)[0] = 0;
	}
}

static void  CB_ADD(char **comment_buffer, int *comment_buffer_size, char *str)
{
	int rem = *comment_buffer_size - strlen(*comment_buffer) - 1;
	int siz = strlen(str);
	if (rem < siz+1) {
		*comment_buffer = ast_realloc(*comment_buffer, *comment_buffer_size + CB_INCR + siz + 1);
		if (!(*comment_buffer))
			return;
		*comment_buffer_size += CB_INCR+siz+1;
	}
	strcat(*comment_buffer,str);
}

static void  CB_ADD_LEN(char **comment_buffer, int *comment_buffer_size, char *str, int len)
{
	int cbl = strlen(*comment_buffer) + 1;
	int rem = *comment_buffer_size - cbl;
	if (rem < len+1) {
		*comment_buffer = ast_realloc(*comment_buffer, *comment_buffer_size + CB_INCR + len + 1);
		if (!(*comment_buffer))
			return;
		*comment_buffer_size += CB_INCR+len+1;
	}
	strncat(*comment_buffer,str,len);
	(*comment_buffer)[cbl+len-1] = 0;
}

static void  LLB_ADD(char **lline_buffer, int *lline_buffer_size, char *str)
{
	int rem = *lline_buffer_size - strlen(*lline_buffer) - 1;
	int siz = strlen(str);
	if (rem < siz+1) {
		*lline_buffer = ast_realloc(*lline_buffer, *lline_buffer_size + CB_INCR + siz + 1);
		if (!(*lline_buffer)) 
			return;
		*lline_buffer_size += CB_INCR + siz + 1;
	}
	strcat(*lline_buffer,str);
}

static void CB_RESET(char **comment_buffer, char **lline_buffer)  
{ 
	(*comment_buffer)[0] = 0; 
	(*lline_buffer)[0] = 0;
}


static struct ast_comment *ALLOC_COMMENT(const char *buffer)
{ 
	struct ast_comment *x;
	x = ast_calloc(1, sizeof(*x)+strlen(buffer)+1);
	strcpy(x->cmt, buffer);
	return x;
}


static struct ast_config_map {
	struct ast_config_map *next;
	char *name;
	char *driver;
	char *database;
	char *table;
	char stuff[0];
} *config_maps = NULL;

AST_MUTEX_DEFINE_STATIC(config_lock);
static struct ast_config_engine *config_engine_list;

#define MAX_INCLUDE_LEVEL 10

struct ast_category {
	char name[80];
	int ignored;			/*!< do not let user of the config see this category */
	int include_level;
	char *file;	           /*!< the file name from whence this declaration was read */
	int lineno;
	struct ast_comment *precomments;
	struct ast_comment *sameline;
	struct ast_variable *root;
	struct ast_variable *last;
	struct ast_category *next;
};

struct ast_config {
	struct ast_category *root;
	struct ast_category *last;
	struct ast_category *current;
	struct ast_category *last_browse;     /*!< used to cache the last category supplied via category_browse */
	int include_level;
	int max_include_level;
	struct ast_config_include *includes;  /*!< a list of inclusions, which should describe the entire tree */
};

struct ast_config_include {
	char *include_location_file;     /*!< file name in which the include occurs */
	int  include_location_lineno;    /*!< lineno where include occurred */
	int  exec;                       /*!< set to non-zero if itsa #exec statement */
	char *exec_file;                 /*!< if it's an exec, you'll have both the /var/tmp to read, and the original script */
	char *included_file;             /*!< file name included */
	int inclusion_count;             /*!< if the file is included more than once, a running count thereof -- but, worry not,
	                                      we explode the instances and will include those-- so all entries will be unique */
	int output;                      /*!< a flag to indicate if the inclusion has been output */
	struct ast_config_include *next; /*!< ptr to next inclusion in the list */
};

struct ast_variable *ast_variable_new(const char *name, const char *value, const char *filename) 
{
	struct ast_variable *variable;
	int name_len = strlen(name) + 1;	

	if ((variable = ast_calloc(1, name_len + strlen(value) + 1 + strlen(filename) + 1 + sizeof(*variable)))) {
		variable->name = variable->stuff;
		variable->value = variable->stuff + name_len;		
		variable->file = variable->stuff + name_len + strlen(value) + 1;
		strcpy(variable->name,name);
		strcpy(variable->value,value);
		strcpy(variable->file,filename);
	}
	return variable;
}

struct ast_config_include *ast_include_new(struct ast_config *conf, const char *from_file, const char *included_file, int is_exec, const char *exec_file, int from_lineno, char *real_included_file_name, int real_included_file_name_size)
{
	/* a file should be included ONCE. Otherwise, if one of the instances is changed,
       then all be changed. -- how do we know to include it? -- Handling modified 
       instances is possible, I'd have
       to create a new master for each instance. */
	struct ast_config_include *inc;
	struct stat statbuf;
	
	inc = ast_include_find(conf, included_file);
	if (inc) {
		do {
			inc->inclusion_count++;
			snprintf(real_included_file_name, real_included_file_name_size, "%s~~%d", included_file, inc->inclusion_count);
		} while (stat(real_included_file_name, &statbuf) == 0);
		ast_log(LOG_WARNING,"'%s', line %d:  Same File included more than once! This data will be saved in %s if saved back to disk.\n", from_file, from_lineno, real_included_file_name);
	} else
		*real_included_file_name = 0;
	
	inc = ast_calloc(1,sizeof(struct ast_config_include));
	inc->include_location_file = ast_strdup(from_file);
	inc->include_location_lineno = from_lineno;
	if (!ast_strlen_zero(real_included_file_name))
		inc->included_file = ast_strdup(real_included_file_name);
	else
		inc->included_file = ast_strdup(included_file);
	
	inc->exec = is_exec;
	if (is_exec)
		inc->exec_file = ast_strdup(exec_file);
	
	/* attach this new struct to the conf struct */
	inc->next = conf->includes;
	conf->includes = inc;
	
	return inc;
}

void ast_include_rename(struct ast_config *conf, const char *from_file, const char *to_file)
{
	struct ast_config_include *incl;
	struct ast_category *cat;
	struct ast_variable *v;
	
	int from_len = strlen(from_file);
	int to_len = strlen(to_file);
	
	if (strcmp(from_file, to_file) == 0) /* no use wasting time if the name is the same */
		return;
	
	/* the manager code allows you to read in one config file, then
       write it back out under a different name. But, the new arrangement
	   ties output lines to the file name. So, before you try to write
       the config file to disk, better riffle thru the data and make sure
       the file names are changed.
	*/
	/* file names are on categories, includes (of course), and on variables. So,
	   traverse all this and swap names */

	for (incl = conf->includes; incl; incl=incl->next) {
		if (strcmp(incl->include_location_file,from_file) == 0) {
			if (from_len >= to_len)
				strcpy(incl->include_location_file, to_file);
			else {
				free(incl->include_location_file);
				incl->include_location_file = strdup(to_file);
			}
		}
	}
	for (cat = conf->root; cat; cat = cat->next) {
		if (strcmp(cat->file,from_file) == 0) {
			if (from_len >= to_len)
				strcpy(cat->file, to_file);
			else {
				free(cat->file);
				cat->file = strdup(to_file);
			}
		}
		for (v = cat->root; v; v = v->next) {
			if (strcmp(v->file,from_file) == 0) {
				if (from_len >= to_len)
					strcpy(v->file, to_file);
				else {
					free(v->file);
					v->file = strdup(to_file);
				}
			}
		}
	}
}

struct ast_config_include *ast_include_find(struct ast_config *conf, const char *included_file)
{
	struct ast_config_include *x;
	for (x=conf->includes;x;x=x->next)
	{
		if (strcmp(x->included_file,included_file) == 0)
			return x;
	}
	return 0;
}


void ast_variable_append(struct ast_category *category, struct ast_variable *variable)
{
	if (!variable)
		return;
	if (category->last)
		category->last->next = variable;
	else
		category->root = variable;
	category->last = variable;
	while (category->last->next)
		category->last = category->last->next;
}

void ast_variables_destroy(struct ast_variable *v)
{
	struct ast_variable *vn;

	while (v) {
		vn = v;
		v = v->next;
		ast_free(vn);
	}
}

struct ast_variable *ast_variable_browse(const struct ast_config *config, const char *category)
{
	struct ast_category *cat = NULL;

	if (category && config->last_browse && (config->last_browse->name == category))
		cat = config->last_browse;
	else
		cat = ast_category_get(config, category);

	return (cat) ? cat->root : NULL;
}

const char *ast_config_option(struct ast_config *cfg, const char *cat, const char *var)
{
	const char *tmp;
	tmp = ast_variable_retrieve(cfg, cat, var);
	if (!tmp)
		tmp = ast_variable_retrieve(cfg, "general", var);
	return tmp;
}


const char *ast_variable_retrieve(const struct ast_config *config, const char *category, const char *variable)
{
	struct ast_variable *v;

	if (category) {
		for (v = ast_variable_browse(config, category); v; v = v->next) {
			if (!strcasecmp(variable, v->name))
				return v->value;
		}
	} else {
		struct ast_category *cat;

		for (cat = config->root; cat; cat = cat->next)
			for (v = cat->root; v; v = v->next)
				if (!strcasecmp(variable, v->name))
					return v->value;
	}

	return NULL;
}

static struct ast_variable *variable_clone(const struct ast_variable *old)
{
	struct ast_variable *new = ast_variable_new(old->name, old->value, old->file);

	if (new) {
		new->lineno = old->lineno;
		new->object = old->object;
		new->blanklines = old->blanklines;
		/* TODO: clone comments? */
	}

	return new;
}
 
static void move_variables(struct ast_category *old, struct ast_category *new)
{
	struct ast_variable *var = old->root;
	old->root = NULL;
#if 1
	/* we can just move the entire list in a single op */
	ast_variable_append(new, var);
#else
	while (var) {
		struct ast_variable *next = var->next;
		var->next = NULL;
		ast_variable_append(new, var);
		var = next;
	}
#endif
}

struct ast_category *ast_category_new(const char *name, const char *in_file, int lineno) 
{
	struct ast_category *category;

	if ((category = ast_calloc(1, sizeof(*category))))
		ast_copy_string(category->name, name, sizeof(category->name));
	category->file = strdup(in_file);
	category->lineno = lineno; /* if you don't know the lineno, set it to 999999 or something real big */
	return category;
}

static struct ast_category *category_get(const struct ast_config *config, const char *category_name, int ignored)
{
	struct ast_category *cat;

	/* try exact match first, then case-insensitive match */
	for (cat = config->root; cat; cat = cat->next) {
		if (cat->name == category_name && (ignored || !cat->ignored))
			return cat;
	}

	for (cat = config->root; cat; cat = cat->next) {
		if (!strcasecmp(cat->name, category_name) && (ignored || !cat->ignored))
			return cat;
	}

	return NULL;
}

struct ast_category *ast_category_get(const struct ast_config *config, const char *category_name)
{
	return category_get(config, category_name, 0);
}

int ast_category_exist(const struct ast_config *config, const char *category_name)
{
	return !!ast_category_get(config, category_name);
}

void ast_category_append(struct ast_config *config, struct ast_category *category)
{
	if (config->last)
		config->last->next = category;
	else
		config->root = category;
	category->include_level = config->include_level;
	config->last = category;
	config->current = category;
}

void ast_category_destroy(struct ast_category *cat)
{
	ast_variables_destroy(cat->root);
	if (cat->file)
		free(cat->file);
	
	ast_free(cat);
}

static void ast_includes_destroy(struct ast_config_include *incls)
{
	struct ast_config_include *incl,*inclnext;
	
	for (incl=incls; incl; incl = inclnext) {
		inclnext = incl->next;
		if (incl->include_location_file)
			free(incl->include_location_file);
		if (incl->exec_file)
			free(incl->exec_file);
		if (incl->included_file)
			free(incl->included_file);
		free(incl);
	}
}

static struct ast_category *next_available_category(struct ast_category *cat)
{
	for (; cat && cat->ignored; cat = cat->next);

	return cat;
}

struct ast_variable *ast_category_root(struct ast_config *config, char *cat)
{
	struct ast_category *category = ast_category_get(config, cat);
	if (category)
		return category->root;
	return NULL;
}

char *ast_category_browse(struct ast_config *config, const char *prev)
{	
	struct ast_category *cat = NULL;

	if (prev && config->last_browse && (config->last_browse->name == prev))
		cat = config->last_browse->next;
	else if (!prev && config->root)
		cat = config->root;
	else if (prev) {
		for (cat = config->root; cat; cat = cat->next) {
			if (cat->name == prev) {
				cat = cat->next;
				break;
			}
		}
		if (!cat) {
			for (cat = config->root; cat; cat = cat->next) {
				if (!strcasecmp(cat->name, prev)) {
					cat = cat->next;
					break;
				}
			}
		}
	}
	
	if (cat)
		cat = next_available_category(cat);

	config->last_browse = cat;
	return (cat) ? cat->name : NULL;
}

struct ast_variable *ast_category_detach_variables(struct ast_category *cat)
{
	struct ast_variable *v;

	v = cat->root;
	cat->root = NULL;
	cat->last = NULL;

	return v;
}

void ast_category_rename(struct ast_category *cat, const char *name)
{
	ast_copy_string(cat->name, name, sizeof(cat->name));
}

static void inherit_category(struct ast_category *new, const struct ast_category *base)
{
	struct ast_variable *var;

	for (var = base->root; var; var = var->next)
		ast_variable_append(new, variable_clone(var));
}

struct ast_config *ast_config_new(void) 
{
	struct ast_config *config;

	if ((config = ast_calloc(1, sizeof(*config))))
		config->max_include_level = MAX_INCLUDE_LEVEL;
	return config;
}

int ast_variable_delete(struct ast_category *category, const char *variable, const char *match)
{
	struct ast_variable *cur, *prev=NULL, *curn;
	int res = -1;
	cur = category->root;
	while (cur) {
		if (cur->name == variable) {
			if (prev) {
				prev->next = cur->next;
				if (cur == category->last)
					category->last = prev;
			} else {
				category->root = cur->next;
				if (cur == category->last)
					category->last = NULL;
			}
			cur->next = NULL;
			ast_variables_destroy(cur);
			return 0;
		}
		prev = cur;
		cur = cur->next;
	}

	prev = NULL;
	cur = category->root;
	while (cur) {
		curn = cur->next;
		if (!strcasecmp(cur->name, variable) && (ast_strlen_zero(match) || !strcasecmp(cur->value, match))) {
			if (prev) {
				prev->next = cur->next;
				if (cur == category->last)
					category->last = prev;
			} else {
				category->root = cur->next;
				if (cur == category->last)
					category->last = NULL;
			}
			cur->next = NULL;
			ast_variables_destroy(cur);
			res = 0;
		} else
			prev = cur;

		cur = curn;
	}
	return res;
}

int ast_variable_update(struct ast_category *category, const char *variable, 
						const char *value, const char *match, unsigned int object)
{
	struct ast_variable *cur, *prev=NULL, *newer=NULL;

	for (cur = category->root; cur; prev = cur, cur = cur->next) {
		if (strcasecmp(cur->name, variable) ||
			(!ast_strlen_zero(match) && strcasecmp(cur->value, match)))
			continue;

		if (!(newer = ast_variable_new(variable, value, cur->file)))
			return -1;
	
		newer->next = cur->next;
		newer->object = cur->object || object;
		if (prev)
			prev->next = newer;
		else
			category->root = newer;
		if (category->last == cur)
			category->last = newer;

		cur->next = NULL;
		ast_variables_destroy(cur);

		return 0;
	}

	if (prev)
		prev->next = newer;
	else
		category->root = newer;

	return 0;
}

int ast_category_delete(struct ast_config *cfg, const char *category)
{
	struct ast_category *prev=NULL, *cat;
	cat = cfg->root;
	while (cat) {
		if (cat->name == category) {
			ast_variables_destroy(cat->root);
			if (prev) {
				prev->next = cat->next;
				if (cat == cfg->last)
					cfg->last = prev;
			} else {
				cfg->root = cat->next;
				if (cat == cfg->last)
					cfg->last = NULL;
			}
			ast_free(cat);
			return 0;
		}
		prev = cat;
		cat = cat->next;
	}

	prev = NULL;
	cat = cfg->root;
	while (cat) {
		if (!strcasecmp(cat->name, category)) {
			ast_variables_destroy(cat->root);
			if (prev) {
				prev->next = cat->next;
				if (cat == cfg->last)
					cfg->last = prev;
			} else {
				cfg->root = cat->next;
				if (cat == cfg->last)
					cfg->last = NULL;
			}
			ast_free(cat);
			return 0;
		}
		prev = cat;
		cat = cat->next;
	}
	return -1;
}

void ast_config_destroy(struct ast_config *cfg)
{
	struct ast_category *cat, *catn;

	if (!cfg)
		return;

	ast_includes_destroy(cfg->includes);

	cat = cfg->root;
	while (cat) {
		ast_variables_destroy(cat->root);
		catn = cat;
		cat = cat->next;
		ast_free(catn);
	}
	ast_free(cfg);
}

struct ast_category *ast_config_get_current_category(const struct ast_config *cfg)
{
	return cfg->current;
}

void ast_config_set_current_category(struct ast_config *cfg, const struct ast_category *cat)
{
	/* cast below is just to silence compiler warning about dropping "const" */
	cfg->current = (struct ast_category *) cat;
}

enum config_cache_attribute_enum {
	ATTRIBUTE_INCLUDE = 0,
	ATTRIBUTE_EXEC = 1,
};

static void config_cache_attribute(const char *configfile, enum config_cache_attribute_enum attrtype, const char *filename)
{
	struct cache_file_mtime *cfmtime;
	struct cache_file_include *cfinclude;
	struct stat statbuf = { 0, };

	/* Find our cached entry for this configuration file */
	AST_LIST_LOCK(&cfmtime_head);
	AST_LIST_TRAVERSE(&cfmtime_head, cfmtime, list) {
		if (!strcmp(cfmtime->filename, configfile))
			break;
	}
	if (!cfmtime) {
		cfmtime = ast_calloc(1, sizeof(*cfmtime) + strlen(configfile) + 1);
		if (!cfmtime) {
			AST_LIST_UNLOCK(&cfmtime_head);
			return;
		}
		AST_LIST_HEAD_INIT(&cfmtime->includes);
		strcpy(cfmtime->filename, configfile);
		/* Note that the file mtime is initialized to 0, i.e. 1970 */
		AST_LIST_INSERT_TAIL(&cfmtime_head, cfmtime, list);
	}

	if (!stat(configfile, &statbuf))
		cfmtime->mtime = 0;
	else
		cfmtime->mtime = statbuf.st_mtime;

	switch (attrtype) {
	case ATTRIBUTE_INCLUDE:
		cfinclude = ast_calloc(1, sizeof(*cfinclude) + strlen(filename) + 1);
		if (!cfinclude) {
			AST_LIST_UNLOCK(&cfmtime_head);
			return;
		}
		strcpy(cfinclude->include, filename);
		AST_LIST_INSERT_TAIL(&cfmtime->includes, cfinclude, list);
		break;
	case ATTRIBUTE_EXEC:
		cfmtime->has_exec = 1;
		break;
	}
	AST_LIST_UNLOCK(&cfmtime_head);
}

static int process_text_line(struct ast_config *cfg, struct ast_category **cat, char *buf, int lineno, const char *configfile, struct ast_flags flags,
							 char **comment_buffer, int *comment_buffer_size, char **lline_buffer, int *lline_buffer_size, const char *suggested_include_file)
{
	char *c;
	char *cur = buf;
	struct ast_variable *v;
	char cmd[512], exec_file[512];
	int object, do_exec, do_include;

	/* Actually parse the entry */
	if (cur[0] == '[') {
		struct ast_category *newcat = NULL;
		char *catname;

		/* A category header */
		c = strchr(cur, ']');
		if (!c) {
			ast_log(LOG_WARNING, "parse error: no closing ']', line %d of %s\n", lineno, configfile);
			return -1;
		}
		*c++ = '\0';
		cur++;
 		if (*c++ != '(')
 			c = NULL;
		catname = cur;
		if (!(*cat = newcat = ast_category_new(catname, ast_strlen_zero(suggested_include_file)?configfile:suggested_include_file, lineno))) {
			return -1;
		}
		(*cat)->lineno = lineno;
		
		/* add comments */
		if (ast_test_flag(&flags, CONFIG_FLAG_WITHCOMMENTS) && *comment_buffer && (*comment_buffer)[0] ) {
			newcat->precomments = ALLOC_COMMENT(*comment_buffer);
		}
		if (ast_test_flag(&flags, CONFIG_FLAG_WITHCOMMENTS) && *lline_buffer && (*lline_buffer)[0] ) {
			newcat->sameline = ALLOC_COMMENT(*lline_buffer);
		}
		if (ast_test_flag(&flags, CONFIG_FLAG_WITHCOMMENTS))
			CB_RESET(comment_buffer, lline_buffer);
		
 		/* If there are options or categories to inherit from, process them now */
 		if (c) {
 			if (!(cur = strchr(c, ')'))) {
 				ast_log(LOG_WARNING, "parse error: no closing ')', line %d of %s\n", lineno, configfile);
 				return -1;
 			}
 			*cur = '\0';
 			while ((cur = strsep(&c, ","))) {
				if (!strcasecmp(cur, "!")) {
					(*cat)->ignored = 1;
				} else if (!strcasecmp(cur, "+")) {
					*cat = category_get(cfg, catname, 1);
					if (!(*cat)) {
						if (newcat)
							ast_category_destroy(newcat);
						ast_log(LOG_WARNING, "Category addition requested, but category '%s' does not exist, line %d of %s\n", catname, lineno, configfile);
						return -1;
					}
					if (newcat) {
						move_variables(newcat, *cat);
						ast_category_destroy(newcat);
						newcat = NULL;
					}
				} else {
					struct ast_category *base;
 				
					base = category_get(cfg, cur, 1);
					if (!base) {
						ast_log(LOG_WARNING, "Inheritance requested, but category '%s' does not exist, line %d of %s\n", cur, lineno, configfile);
						return -1;
					}
					inherit_category(*cat, base);
				}
 			}
 		}
		if (newcat)
			ast_category_append(cfg, *cat);
	} else if (cur[0] == '#') {
		/* A directive */
		cur++;
		c = cur;
		while (*c && (*c > 32)) c++;
		if (*c) {
			*c = '\0';
			/* Find real argument */
			c = ast_skip_blanks(c + 1);
			if (!(*c))
				c = NULL;
		} else 
			c = NULL;
		do_include = !strcasecmp(cur, "include");
		if (!do_include)
			do_exec = !strcasecmp(cur, "exec");
		else
			do_exec = 0;
		if (do_exec && !ast_opt_exec_includes) {
			ast_log(LOG_WARNING, "Cannot perform #exec unless execincludes option is enabled in asterisk.conf (options section)!\n");
			do_exec = 0;
		}
		if (do_include || do_exec) {
			if (c) {
				char *cur2;
				char real_inclusion_name[256];
				struct ast_config_include *inclu;
				
				/* Strip off leading and trailing "'s and <>'s */
				while ((*c == '<') || (*c == '>') || (*c == '\"')) c++;
				/* Get rid of leading mess */
				cur = c;
				cur2 = cur;
				while (!ast_strlen_zero(cur)) {
					c = cur + strlen(cur) - 1;
					if ((*c == '>') || (*c == '<') || (*c == '\"'))
						*c = '\0';
					else
						break;
				}
				/* #exec </path/to/executable>
				   We create a tmp file, then we #include it, then we delete it. */
				if (do_exec) {
					if (!ast_test_flag(&flags, CONFIG_FLAG_NOCACHE))
						config_cache_attribute(configfile, ATTRIBUTE_EXEC, NULL);
					snprintf(exec_file, sizeof(exec_file), "/var/tmp/exec.%d.%ld", (int)time(NULL), (long)pthread_self());
					snprintf(cmd, sizeof(cmd), "%s > %s 2>&1", cur, exec_file);
					ast_safe_system(cmd);
					cur = exec_file;
				} else {
					if (!ast_test_flag(&flags, CONFIG_FLAG_NOCACHE))
						config_cache_attribute(configfile, ATTRIBUTE_INCLUDE, cur);
					exec_file[0] = '\0';
				}
				/* A #include */
				/* record this inclusion */
				inclu = ast_include_new(cfg, configfile, cur, do_exec, cur2, lineno, real_inclusion_name, sizeof(real_inclusion_name));

				do_include = ast_config_internal_load(cur, cfg, flags, real_inclusion_name) ? 1 : 0;
				if (!ast_strlen_zero(exec_file))
					unlink(exec_file);
				if (!do_include)
					return 0;

			} else {
				ast_log(LOG_WARNING, "Directive '#%s' needs an argument (%s) at line %d of %s\n", 
						do_exec ? "exec" : "include",
						do_exec ? "/path/to/executable" : "filename",
						lineno,
						configfile);
			}
		}
		else 
			ast_log(LOG_WARNING, "Unknown directive '%s' at line %d of %s\n", cur, lineno, configfile);
	} else {
		/* Just a line (variable = value) */
		if (!(*cat)) {
			ast_log(LOG_WARNING,
				"parse error: No category context for line %d of %s\n", lineno, configfile);
			return -1;
		}
		c = strchr(cur, '=');
		if (c) {
			*c = 0;
			c++;
			/* Ignore > in => */
			if (*c== '>') {
				object = 1;
				c++;
			} else
				object = 0;
			if ((v = ast_variable_new(ast_strip(cur), ast_strip(c), *suggested_include_file ? suggested_include_file : configfile))) {
				v->lineno = lineno;
				v->object = object;
				/* Put and reset comments */
				v->blanklines = 0;
				ast_variable_append(*cat, v);
				/* add comments */
				if (ast_test_flag(&flags, CONFIG_FLAG_WITHCOMMENTS) && *comment_buffer && (*comment_buffer)[0] ) {
					v->precomments = ALLOC_COMMENT(*comment_buffer);
				}
				if (ast_test_flag(&flags, CONFIG_FLAG_WITHCOMMENTS) && *lline_buffer && (*lline_buffer)[0] ) {
					v->sameline = ALLOC_COMMENT(*lline_buffer);
				}
				if (ast_test_flag(&flags, CONFIG_FLAG_WITHCOMMENTS))
					CB_RESET(comment_buffer, lline_buffer);
				
			} else {
				return -1;
			}
		} else {
			ast_log(LOG_WARNING, "No '=' (equal sign) in line %d of %s\n", lineno, configfile);
		}
	}
	return 0;
}

static struct ast_config *config_text_file_load(const char *database, const char *table, const char *filename, struct ast_config *cfg, struct ast_flags flags, const char *suggested_include_file)
{
	char fn[256];
	char buf[8192];
	char *new_buf, *comment_p, *process_buf;
	FILE *f;
	int lineno=0;
	int comment = 0, nest[MAX_NESTED_COMMENTS];
	struct ast_category *cat = NULL;
	int count = 0;
	struct stat statbuf;
	struct cache_file_mtime *cfmtime = NULL;
	struct cache_file_include *cfinclude;
	/*! Growable string buffer */
	char *comment_buffer=0;   /*!< this will be a comment collector.*/
	int   comment_buffer_size=0;  /*!< the amount of storage so far alloc'd for the comment_buffer */

	char *lline_buffer=0;    /*!< A buffer for stuff behind the ; */
	int  lline_buffer_size=0;

	if (cfg)
		cat = ast_config_get_current_category(cfg);

	if (filename[0] == '/') {
		ast_copy_string(fn, filename, sizeof(fn));
	} else {
		snprintf(fn, sizeof(fn), "%s/%s", (char *)ast_config_AST_CONFIG_DIR, filename);
	}

	if (ast_test_flag(&flags, CONFIG_FLAG_WITHCOMMENTS)) {
		CB_INIT(&comment_buffer, &comment_buffer_size, &lline_buffer, &lline_buffer_size);
		if (!lline_buffer || !comment_buffer) {
			ast_log(LOG_ERROR, "Failed to initialize the comment buffer!\n");
			return NULL;
		}
	}
#ifdef AST_INCLUDE_GLOB
	{
		int glob_ret;
		glob_t globbuf;
		globbuf.gl_offs = 0;	/* initialize it to silence gcc */
#ifdef SOLARIS
		glob_ret = glob(fn, GLOB_NOCHECK, NULL, &globbuf);
#else
		glob_ret = glob(fn, GLOB_NOMAGIC|GLOB_BRACE, NULL, &globbuf);
#endif
		if (glob_ret == GLOB_NOSPACE)
			ast_log(LOG_WARNING,
				"Glob Expansion of pattern '%s' failed: Not enough memory\n", fn);
		else if (glob_ret  == GLOB_ABORTED)
			ast_log(LOG_WARNING,
				"Glob Expansion of pattern '%s' failed: Read error\n", fn);
		else  {
			/* loop over expanded files */
			int i;
			for (i=0; i<globbuf.gl_pathc; i++) {
				ast_copy_string(fn, globbuf.gl_pathv[i], sizeof(fn));
#endif
	do {
		if (stat(fn, &statbuf))
			continue;

		if (!S_ISREG(statbuf.st_mode)) {
			ast_log(LOG_WARNING, "'%s' is not a regular file, ignoring\n", fn);
			continue;
		}

		if (!ast_test_flag(&flags, CONFIG_FLAG_NOCACHE)) {
			/* Find our cached entry for this configuration file */
			AST_LIST_LOCK(&cfmtime_head);
			AST_LIST_TRAVERSE(&cfmtime_head, cfmtime, list) {
				if (!strcmp(cfmtime->filename, fn))
					break;
			}
			if (!cfmtime) {
				cfmtime = ast_calloc(1, sizeof(*cfmtime) + strlen(fn) + 1);
				if (!cfmtime)
					continue;
				AST_LIST_HEAD_INIT(&cfmtime->includes);
				strcpy(cfmtime->filename, fn);
				/* Note that the file mtime is initialized to 0, i.e. 1970 */
				AST_LIST_INSERT_TAIL(&cfmtime_head, cfmtime, list);
			}
		}

		if (cfmtime && (!cfmtime->has_exec) && (cfmtime->mtime == statbuf.st_mtime) && ast_test_flag(&flags, CONFIG_FLAG_FILEUNCHANGED)) {
			/* File is unchanged, what about the (cached) includes (if any)? */
			int unchanged = 1;
			AST_LIST_TRAVERSE(&cfmtime->includes, cfinclude, list) {
				/* We must glob here, because if we did not, then adding a file to globbed directory would
				 * incorrectly cause no reload to be necessary. */
				char fn2[256];
#ifdef AST_INCLUDE_GLOB
				int glob_ret;
				glob_t globbuf = { .gl_offs = 0 };
#ifdef SOLARIS
				glob_ret = glob(cfinclude->include, GLOB_NOCHECK, NULL, &globbuf);
#else
				glob_ret = glob(cfinclude->include, GLOB_NOMAGIC|GLOB_BRACE, NULL, &globbuf);
#endif
				/* On error, we reparse */
				if (glob_ret == GLOB_NOSPACE || glob_ret  == GLOB_ABORTED)
					unchanged = 0;
				else  {
					/* loop over expanded files */
					int j;
					for (j = 0; j < globbuf.gl_pathc; j++) {
						ast_copy_string(fn2, globbuf.gl_pathv[j], sizeof(fn2));
#else
						ast_copy_string(fn2, cfinclude->include);
#endif
						if (config_text_file_load(NULL, NULL, fn2, NULL, flags, "") == NULL) { /* that last field needs to be looked at in this case... TODO */
							unchanged = 0;
							/* One change is enough to short-circuit and reload the whole shebang */
							break;
						}
#ifdef AST_INCLUDE_GLOB
					}
				}
#endif
			}

			if (unchanged) {
				AST_LIST_UNLOCK(&cfmtime_head);
				return CONFIG_STATUS_FILEUNCHANGED;
			}
		}
		if (!ast_test_flag(&flags, CONFIG_FLAG_NOCACHE))
			AST_LIST_UNLOCK(&cfmtime_head);

		/* If cfg is NULL, then we just want an answer */
		if (cfg == NULL)
			return NULL;

		if (cfmtime)
			cfmtime->mtime = statbuf.st_mtime;

		ast_verb(2, "Parsing '%s': ", fn);
			fflush(stdout);
		if (!(f = fopen(fn, "r"))) {
			ast_debug(1, "No file to parse: %s\n", fn);
			ast_verb(2, "Not found (%s)\n", strerror(errno));
			continue;
		}
		count++;
		ast_debug(1, "Parsing %s\n", fn);
		ast_verb(2, "Found\n");
		while (!feof(f)) {
			lineno++;
			if (fgets(buf, sizeof(buf), f)) {
				if (ast_test_flag(&flags, CONFIG_FLAG_WITHCOMMENTS)) {
					CB_ADD(&comment_buffer, &comment_buffer_size, lline_buffer);       /* add the current lline buffer to the comment buffer */
					lline_buffer[0] = 0;        /* erase the lline buffer */
				}
				
				new_buf = buf;
				if (comment) 
					process_buf = NULL;
				else
					process_buf = buf;
				
				while ((comment_p = strchr(new_buf, COMMENT_META))) {
					if ((comment_p > new_buf) && (*(comment_p-1) == '\\')) {
						/* Escaped semicolons aren't comments. */
						new_buf = comment_p + 1;
					} else if (comment_p[1] == COMMENT_TAG && comment_p[2] == COMMENT_TAG && (comment_p[3] != '-')) {
						/* Meta-Comment start detected ";--" */
						if (comment < MAX_NESTED_COMMENTS) {
							*comment_p = '\0';
							new_buf = comment_p + 3;
							comment++;
							nest[comment-1] = lineno;
						} else {
							ast_log(LOG_ERROR, "Maximum nest limit of %d reached.\n", MAX_NESTED_COMMENTS);
						}
					} else if ((comment_p >= new_buf + 2) &&
						   (*(comment_p - 1) == COMMENT_TAG) &&
						   (*(comment_p - 2) == COMMENT_TAG)) {
						/* Meta-Comment end detected */
						comment--;
						new_buf = comment_p + 1;
						if (!comment) {
							/* Back to non-comment now */
							if (process_buf) {
								/* Actually have to move what's left over the top, then continue */
								char *oldptr;
								oldptr = process_buf + strlen(process_buf);
								if (ast_test_flag(&flags, CONFIG_FLAG_WITHCOMMENTS)) {
									CB_ADD(&comment_buffer, &comment_buffer_size, ";");
									CB_ADD_LEN(&comment_buffer, &comment_buffer_size, oldptr+1, new_buf-oldptr-1);
								}
								
								memmove(oldptr, new_buf, strlen(new_buf) + 1);
								new_buf = oldptr;
							} else
								process_buf = new_buf;
						}
					} else {
						if (!comment) {
							/* If ; is found, and we are not nested in a comment, 
							   we immediately stop all comment processing */
							if (ast_test_flag(&flags, CONFIG_FLAG_WITHCOMMENTS)) {
								LLB_ADD(&lline_buffer, &lline_buffer_size, comment_p);
							}
							*comment_p = '\0'; 
							new_buf = comment_p;
						} else
							new_buf = comment_p + 1;
					}
				}
				if (ast_test_flag(&flags, CONFIG_FLAG_WITHCOMMENTS) && comment && !process_buf )
				{
					CB_ADD(&comment_buffer, &comment_buffer_size, buf);  /* the whole line is a comment, store it */
				}
				
				if (process_buf) {
					char *buf = ast_strip(process_buf);
					if (!ast_strlen_zero(buf)) {
						if (process_text_line(cfg, &cat, buf, lineno, fn, flags, &comment_buffer, &comment_buffer_size, &lline_buffer, &lline_buffer_size, suggested_include_file)) {
							cfg = NULL;
							break;
						}
					}
				}
			}
		}
		fclose(f);		
	} while (0);
	if (comment) {
		ast_log(LOG_WARNING,"Unterminated comment detected beginning on line %d\n", nest[comment - 1]);
	}
#ifdef AST_INCLUDE_GLOB
					if (cfg == NULL || cfg == CONFIG_STATUS_FILEUNCHANGED)
						break;
				}
				globfree(&globbuf);
			}
		}
#endif

	if (cfg && cfg != CONFIG_STATUS_FILEUNCHANGED && cfg->include_level == 1 && ast_test_flag(&flags, CONFIG_FLAG_WITHCOMMENTS) && comment_buffer) {
		ast_free(comment_buffer);
		ast_free(lline_buffer);
		comment_buffer = NULL;
		lline_buffer = NULL;
		comment_buffer_size = 0;
		lline_buffer_size = 0;
	}
	
	if (count == 0)
		return NULL;

	return cfg;
}


/* NOTE: categories and variables each have a file and lineno attribute. On a save operation, these are used to determine
   which file and line number to write out to. Thus, an entire hierarchy of config files (via #include statements) can be
   recreated. BUT, care must be taken to make sure that every cat and var has the proper file name stored, or you may
   be shocked and mystified as to why things are not showing up in the files! 

   Also, All #include/#exec statements are recorded in the "includes" LL in the ast_config structure. The file name
   and line number are stored for each include, plus the name of the file included, so that these statements may be
   included in the output files on a file_save operation. 

   The lineno's are really just for relative placement in the file. There is no attempt to make sure that blank lines
   are included to keep the lineno's the same between input and output. The lineno fields are used mainly to determine
   the position of the #include and #exec directives. So, blank lines tend to disappear from a read/rewrite operation,
   and a header gets added.

   vars and category heads are output in the order they are stored in the config file. So, if the software
   shuffles these at all, then the placement of #include directives might get a little mixed up, because the
   file/lineno data probably won't get changed.

*/

static void gen_header(FILE *f1, const char *configfile, const char *fn, const char *generator)
{
	char date[256]="";
	time_t t;
	time(&t);
	ast_copy_string(date, ctime(&t), sizeof(date));

	fprintf(f1, ";!\n");
	fprintf(f1, ";! Automatically generated configuration file\n");
	if (strcmp(configfile, fn))
		fprintf(f1, ";! Filename: %s (%s)\n", configfile, fn);
	else
		fprintf(f1, ";! Filename: %s\n", configfile);
	fprintf(f1, ";! Generator: %s\n", generator);
	fprintf(f1, ";! Creation Date: %s", date);
	fprintf(f1, ";!\n");
}

static void set_fn(char *fn, int fn_size, const char *file, const char *configfile)
{
	if (!file || file[0] == 0) {
		if (configfile[0] == '/')
			ast_copy_string(fn, configfile, fn_size);
		else
			snprintf(fn, fn_size, "%s/%s", ast_config_AST_CONFIG_DIR, configfile);
	} else if (file[0] == '/') 
		ast_copy_string(fn, file, fn_size);
	else
		snprintf(fn, fn_size, "%s/%s", ast_config_AST_CONFIG_DIR, file);
}

int config_text_file_save(const char *configfile, const struct ast_config *cfg, const char *generator)
{
	FILE *f;
	char fn[256];
	struct ast_variable *var;
	struct ast_category *cat;
	struct ast_comment *cmt;
	struct ast_config_include *incl;
	int blanklines = 0;

	/* reset all the output flags, in case this isn't our first time saving this data */

	for (incl=cfg->includes; incl; incl = incl->next)
		incl->output = 0;

	/* go thru all the inclusions and make sure all the files involved (configfile plus all its inclusions)
	   are all truncated to zero bytes and have that nice header*/

	for (incl=cfg->includes; incl; incl = incl->next)
	{
		if (!incl->exec) { /* leave the execs alone -- we'll write out the #exec directives, but won't zero out the include files or exec files*/
			FILE *f1;

			set_fn(fn, sizeof(fn), incl->included_file, configfile); /* normally, fn is just set to incl->included_file, prepended with config dir if relative */
			f1 = fopen(fn,"w");
			if (f1) {
				gen_header(f1, configfile, fn, generator);
				fclose(f1); /* this should zero out the file */
			} else {
				ast_debug(1, "Unable to open for writing: %s\n", fn);
				ast_verb(2, "Unable to write %s (%s)", fn, strerror(errno));
			}
		}
	}

	set_fn(fn, sizeof(fn), 0, configfile); /* just set fn to absolute ver of configfile */
#ifdef __CYGWIN__	
	if ((f = fopen(fn, "w+"))) {
#else
	if ((f = fopen(fn, "w"))) {
#endif	    
		ast_verb(2, "Saving '%s': ", fn);
		gen_header(f, configfile, fn, generator);
		cat = cfg->root;
		fclose(f);
		
		/* from here out, we open each involved file and concat the stuff we need to add to the end and immediately close... */
		/* since each var, cat, and associated comments can come from any file, we have to be 
		   mobile, and open each file, print, and close it on an entry-by-entry basis */

		while (cat) {
			set_fn(fn, sizeof(fn), cat->file, configfile);
			f = fopen(fn, "a");
			if (!f)
			{
				ast_debug(1, "Unable to open for writing: %s\n", fn);
				ast_verb(2, "Unable to write %s (%s)", fn, strerror(errno));
				return -1;
			}

			/* dump any includes that happen before this category header */
			for (incl=cfg->includes; incl; incl = incl->next) {
				if (strcmp(incl->include_location_file, cat->file) == 0){
					if (cat->lineno > incl->include_location_lineno && !incl->output) {
						if (incl->exec)
							fprintf(f,"#exec \"%s\"\n", incl->exec_file);
						else
							fprintf(f,"#include \"%s\"\n", incl->included_file);
						incl->output = 1;
					}
				}
			}
			
			/* Dump section with any appropriate comment */
			for (cmt = cat->precomments; cmt; cmt=cmt->next) {
				if (cmt->cmt[0] != ';' || cmt->cmt[1] != '!')
					fprintf(f,"%s", cmt->cmt);
			}
			if (!cat->precomments)
				fprintf(f,"\n");
			fprintf(f, "[%s]", cat->name);
			for (cmt = cat->sameline; cmt; cmt=cmt->next) {
				fprintf(f,"%s", cmt->cmt);
			}
			if (!cat->sameline)
				fprintf(f,"\n");
			fclose(f);
			
			var = cat->root;
			while (var) {
				set_fn(fn, sizeof(fn), var->file, configfile);
				f = fopen(fn, "a");
				if (!f)
				{
					ast_debug(1, "Unable to open for writing: %s\n", fn);
					ast_verb(2, "Unable to write %s (%s)", fn, strerror(errno));
					return -1;
				}
				
				/* dump any includes that happen before this category header */
				for (incl=cfg->includes; incl; incl = incl->next) {
					if (strcmp(incl->include_location_file, var->file) == 0){
						if (var->lineno > incl->include_location_lineno && !incl->output) {
							if (incl->exec)
								fprintf(f,"#exec \"%s\"\n", incl->exec_file);
							else
								fprintf(f,"#include \"%s\"\n", incl->included_file);
							incl->output = 1;
						}
					}
				}
				
				for (cmt = var->precomments; cmt; cmt=cmt->next) {
					if (cmt->cmt[0] != ';' || cmt->cmt[1] != '!')
						fprintf(f,"%s", cmt->cmt);
				}
				if (var->sameline) 
					fprintf(f, "%s %s %s  %s", var->name, (var->object ? "=>" : "="), var->value, var->sameline->cmt);
				else	
					fprintf(f, "%s %s %s\n", var->name, (var->object ? "=>" : "="), var->value);
				if (var->blanklines) {
					blanklines = var->blanklines;
					while (blanklines--)
						fprintf(f, "\n");
				}
				
				fclose(f);
				
				var = var->next;
			}
			cat = cat->next;
		}
		if (!option_debug)
			ast_verb(2, "Saved\n");
	} else {
		ast_debug(1, "Unable to open for writing: %s\n", fn);
		ast_verb(2, "Unable to write (%s)", strerror(errno));
		return -1;
	}

	/* Now, for files with trailing #include/#exec statements,
	   we have to make sure every entry is output */

	for (incl=cfg->includes; incl; incl = incl->next) {
		if (!incl->output) {
			/* open the respective file */
			set_fn(fn, sizeof(fn), incl->include_location_file, configfile);
			f = fopen(fn, "a");
			if (!f)
			{
				ast_debug(1, "Unable to open for writing: %s\n", fn);
				ast_verb(2, "Unable to write %s (%s)", fn, strerror(errno));
				return -1;
			}
			
			/* output the respective include */
			if (incl->exec)
				fprintf(f,"#exec \"%s\"\n", incl->exec_file);
			else
				fprintf(f,"#include \"%s\"\n", incl->included_file);
			fclose(f);
			incl->output = 1;
		}
	}
				
	return 0;
}

static void clear_config_maps(void) 
{
	struct ast_config_map *map;

	ast_mutex_lock(&config_lock);

	while (config_maps) {
		map = config_maps;
		config_maps = config_maps->next;
		ast_free(map);
	}
		
	ast_mutex_unlock(&config_lock);
}

static int append_mapping(char *name, char *driver, char *database, char *table)
{
	struct ast_config_map *map;
	int length;

	length = sizeof(*map);
	length += strlen(name) + 1;
	length += strlen(driver) + 1;
	length += strlen(database) + 1;
	if (table)
		length += strlen(table) + 1;

	if (!(map = ast_calloc(1, length)))
		return -1;

	map->name = map->stuff;
	strcpy(map->name, name);
	map->driver = map->name + strlen(map->name) + 1;
	strcpy(map->driver, driver);
	map->database = map->driver + strlen(map->driver) + 1;
	strcpy(map->database, database);
	if (table) {
		map->table = map->database + strlen(map->database) + 1;
		strcpy(map->table, table);
	}
	map->next = config_maps;

	ast_verb(2, "Binding %s to %s/%s/%s\n", map->name, map->driver, map->database, map->table ? map->table : map->name);

	config_maps = map;
	return 0;
}

int read_config_maps(void) 
{
	struct ast_config *config, *configtmp;
	struct ast_variable *v;
	char *driver, *table, *database, *stringp, *tmp;
	struct ast_flags flags = { 0 };

	clear_config_maps();

	configtmp = ast_config_new();
	configtmp->max_include_level = 1;
	config = ast_config_internal_load(extconfig_conf, configtmp, flags, "");
	if (!config) {
		ast_config_destroy(configtmp);
		return 0;
	}

	for (v = ast_variable_browse(config, "settings"); v; v = v->next) {
		stringp = v->value;
		driver = strsep(&stringp, ",");

		if ((tmp = strchr(stringp, '\"')))
			stringp = tmp;

		/* check if the database text starts with a double quote */
		if (*stringp == '"') {
			stringp++;
			database = strsep(&stringp, "\"");
			strsep(&stringp, ",");
		} else {
			/* apparently this text has no quotes */
			database = strsep(&stringp, ",");
		}

		table = strsep(&stringp, ",");

		if (!strcmp(v->name, extconfig_conf)) {
			ast_log(LOG_WARNING, "Cannot bind '%s'!\n", extconfig_conf);
			continue;
		}

		if (!strcmp(v->name, "asterisk.conf")) {
			ast_log(LOG_WARNING, "Cannot bind 'asterisk.conf'!\n");
			continue;
		}

		if (!strcmp(v->name, "logger.conf")) {
			ast_log(LOG_WARNING, "Cannot bind 'logger.conf'!\n");
			continue;
		}

		if (!driver || !database)
			continue;
		if (!strcasecmp(v->name, "sipfriends")) {
			ast_log(LOG_WARNING, "The 'sipfriends' table is obsolete, update your config to use sipusers and sippeers, though they can point to the same table.\n");
			append_mapping("sipusers", driver, database, table ? table : "sipfriends");
			append_mapping("sippeers", driver, database, table ? table : "sipfriends");
		} else if (!strcasecmp(v->name, "iaxfriends")) {
			ast_log(LOG_WARNING, "The 'iaxfriends' table is obsolete, update your config to use iaxusers and iaxpeers, though they can point to the same table.\n");
			append_mapping("iaxusers", driver, database, table ? table : "iaxfriends");
			append_mapping("iaxpeers", driver, database, table ? table : "iaxfriends");
		} else 
			append_mapping(v->name, driver, database, table);
	}
		
	ast_config_destroy(config);
	return 0;
}

int ast_config_engine_register(struct ast_config_engine *new) 
{
	struct ast_config_engine *ptr;

	ast_mutex_lock(&config_lock);

	if (!config_engine_list) {
		config_engine_list = new;
	} else {
		for (ptr = config_engine_list; ptr->next; ptr=ptr->next);
		ptr->next = new;
	}

	ast_mutex_unlock(&config_lock);
	ast_log(LOG_NOTICE,"Registered Config Engine %s\n", new->name);

	return 1;
}

int ast_config_engine_deregister(struct ast_config_engine *del) 
{
	struct ast_config_engine *ptr, *last=NULL;

	ast_mutex_lock(&config_lock);

	for (ptr = config_engine_list; ptr; ptr=ptr->next) {
		if (ptr == del) {
			if (last)
				last->next = ptr->next;
			else
				config_engine_list = ptr->next;
			break;
		}
		last = ptr;
	}

	ast_mutex_unlock(&config_lock);

	return 0;
}

/*! \brief Find realtime engine for realtime family */
static struct ast_config_engine *find_engine(const char *family, char *database, int dbsiz, char *table, int tabsiz) 
{
	struct ast_config_engine *eng, *ret = NULL;
	struct ast_config_map *map;

	ast_mutex_lock(&config_lock);

	for (map = config_maps; map; map = map->next) {
		if (!strcasecmp(family, map->name)) {
			if (database)
				ast_copy_string(database, map->database, dbsiz);
			if (table)
				ast_copy_string(table, map->table ? map->table : family, tabsiz);
			break;
		}
	}

	/* Check if the required driver (engine) exist */
	if (map) {
		for (eng = config_engine_list; !ret && eng; eng = eng->next) {
			if (!strcasecmp(eng->name, map->driver))
				ret = eng;
		}
	}

	ast_mutex_unlock(&config_lock);
	
	/* if we found a mapping, but the engine is not available, then issue a warning */
	if (map && !ret)
		ast_log(LOG_WARNING, "Realtime mapping for '%s' found to engine '%s', but the engine is not available\n", map->name, map->driver);

	return ret;
}

static struct ast_config_engine text_file_engine = {
	.name = "text",
	.load_func = config_text_file_load,
};

struct ast_config *ast_config_internal_load(const char *filename, struct ast_config *cfg, struct ast_flags flags, const char *suggested_include_file)
{
	char db[256];
	char table[256];
	struct ast_config_engine *loader = &text_file_engine;
	struct ast_config *result; 

	if (cfg->include_level == cfg->max_include_level) {
		ast_log(LOG_WARNING, "Maximum Include level (%d) exceeded\n", cfg->max_include_level);
		return NULL;
	}

	cfg->include_level++;

	if (strcmp(filename, extconfig_conf) && strcmp(filename, "asterisk.conf") && config_engine_list) {
		struct ast_config_engine *eng;

		eng = find_engine(filename, db, sizeof(db), table, sizeof(table));


		if (eng && eng->load_func) {
			loader = eng;
		} else {
			eng = find_engine("global", db, sizeof(db), table, sizeof(table));
			if (eng && eng->load_func)
				loader = eng;
		}
	}

	result = loader->load_func(db, table, filename, cfg, flags, suggested_include_file);

	if (result && result != CONFIG_STATUS_FILEUNCHANGED)
		result->include_level--;
	else
		cfg->include_level--;

	return result;
}

struct ast_config *ast_config_load(const char *filename, struct ast_flags flags)
{
	struct ast_config *cfg;
	struct ast_config *result;

	cfg = ast_config_new();
	if (!cfg)
		return NULL;

	result = ast_config_internal_load(filename, cfg, flags, "");
	if (!result || result == CONFIG_STATUS_FILEUNCHANGED)
		ast_config_destroy(cfg);

	return result;
}

static struct ast_variable *ast_load_realtime_helper(const char *family, va_list ap)
{
	struct ast_config_engine *eng;
	char db[256]="";
	char table[256]="";
	struct ast_variable *res=NULL;

	eng = find_engine(family, db, sizeof(db), table, sizeof(table));
	if (eng && eng->realtime_func) 
		res = eng->realtime_func(db, table, ap);

	return res;
}

struct ast_variable *ast_load_realtime_all(const char *family, ...)
{
	struct ast_variable *res;
	va_list ap;

	va_start(ap, family);
	res = ast_load_realtime_helper(family, ap);
	va_end(ap);

	return res;
}

struct ast_variable *ast_load_realtime(const char *family, ...)
{
	struct ast_variable *res, *cur, *prev = NULL, *freeme = NULL;
	va_list ap;

	va_start(ap, family);
	res = ast_load_realtime_helper(family, ap);
	va_end(ap);

	/* Eliminate blank entries */
	for (cur = res; cur; cur = cur->next) {
		if (freeme) {
			ast_free(freeme);
			freeme = NULL;
		}

		if (ast_strlen_zero(cur->value)) {
			if (prev)
				prev->next = cur->next;
			else
				res = cur->next;
			freeme = cur;
		} else {
			prev = cur;
		}
	}
	return res;
}

/*! \brief Check if realtime engine is configured for family */
int ast_check_realtime(const char *family)
{
	struct ast_config_engine *eng;

	eng = find_engine(family, NULL, 0, NULL, 0);
	if (eng)
		return 1;
	return 0;

}

/*! \brief Check if there's any realtime engines loaded */
int ast_realtime_enabled()
{
	return config_maps ? 1 : 0;
}

struct ast_config *ast_load_realtime_multientry(const char *family, ...)
{
	struct ast_config_engine *eng;
	char db[256]="";
	char table[256]="";
	struct ast_config *res=NULL;
	va_list ap;

	va_start(ap, family);
	eng = find_engine(family, db, sizeof(db), table, sizeof(table));
	if (eng && eng->realtime_multi_func) 
		res = eng->realtime_multi_func(db, table, ap);
	va_end(ap);

	return res;
}

int ast_update_realtime(const char *family, const char *keyfield, const char *lookup, ...)
{
	struct ast_config_engine *eng;
	int res = -1;
	char db[256]="";
	char table[256]="";
	va_list ap;

	va_start(ap, lookup);
	eng = find_engine(family, db, sizeof(db), table, sizeof(table));
	if (eng && eng->update_func) 
		res = eng->update_func(db, table, keyfield, lookup, ap);
	va_end(ap);

	return res;
}

int ast_store_realtime(const char *family, ...) {
	struct ast_config_engine *eng;
	int res = -1;
	char db[256]="";
	char table[256]="";
	va_list ap;

	va_start(ap, family);
	eng = find_engine(family, db, sizeof(db), table, sizeof(table));
	if (eng && eng->store_func) 
		res = eng->store_func(db, table, ap);
	va_end(ap);

	return res;
}

int ast_destroy_realtime(const char *family, const char *keyfield, const char *lookup, ...) {
	struct ast_config_engine *eng;
	int res = -1;
	char db[256]="";
	char table[256]="";
	va_list ap;

	va_start(ap, lookup);
	eng = find_engine(family, db, sizeof(db), table, sizeof(table));
	if (eng && eng->destroy_func) 
		res = eng->destroy_func(db, table, keyfield, lookup, ap);
	va_end(ap);

	return res;
}

/*! \brief Helper function to parse arguments
 * See documentation in config.h
 */
int ast_parse_arg(const char *arg, enum ast_parse_flags flags,
        void *p_result, ...)
{
	va_list ap;
	int error = 0;

	va_start(ap, p_result);
	switch (flags & PARSE_TYPE) {
	case PARSE_INT32:
	    {
		int32_t *result = p_result;
		int32_t x, def = result ? *result : 0,
			high = (int32_t)0x7fffffff,
			low  = (int32_t)0x80000000;
		/* optional argument: first default value, then range */
		if (flags & PARSE_DEFAULT)
			def = va_arg(ap, int32_t);
		if (flags & (PARSE_IN_RANGE|PARSE_OUT_RANGE)) {
			/* range requested, update bounds */
			low = va_arg(ap, int32_t);
			high = va_arg(ap, int32_t);
		}
		x = strtol(arg, NULL, 0);
		error = (x < low) || (x > high);
		if (flags & PARSE_OUT_RANGE)
			error = !error;
		if (result)
			*result  = error ? def : x;
		ast_debug(3,
			"extract int from [%s] in [%d, %d] gives [%d](%d)\n",
			arg, low, high,
			result ? *result : x, error);
		break;
	    }

	case PARSE_UINT32:
	    {
		uint32_t *result = p_result;
		uint32_t x, def = result ? *result : 0,
			low = 0, high = (uint32_t)~0;
		/* optional argument: first default value, then range */
		if (flags & PARSE_DEFAULT)
			def = va_arg(ap, uint32_t);
		if (flags & (PARSE_IN_RANGE|PARSE_OUT_RANGE)) {
			/* range requested, update bounds */
			low = va_arg(ap, uint32_t);
			high = va_arg(ap, uint32_t);
		}
		x = strtoul(arg, NULL, 0);
		error = (x < low) || (x > high);
		if (flags & PARSE_OUT_RANGE)
			error = !error;
		if (result)
			*result  = error ? def : x;
		ast_debug(3,
			"extract uint from [%s] in [%u, %u] gives [%u](%d)\n",
			arg, low, high,
			result ? *result : x, error);
		break;
	    }

	case PARSE_INADDR:
	    {
		char *port, *buf;
		struct sockaddr_in _sa_buf;	/* buffer for the result */
		struct sockaddr_in *sa = p_result ?
			(struct sockaddr_in *)p_result : &_sa_buf;
		/* default is either the supplied value or the result itself */
		struct sockaddr_in *def = (flags & PARSE_DEFAULT) ?
			va_arg(ap, struct sockaddr_in *) : sa;
		struct hostent *hp;
		struct ast_hostent ahp;

		bzero(&_sa_buf, sizeof(_sa_buf)); /* clear buffer */
		/* duplicate the string to strip away the :port */
		port = ast_strdupa(arg);
		buf = strsep(&port, ":");
		sa->sin_family = AF_INET;	/* assign family */
		/*
		 * honor the ports flag setting, assign default value
		 * in case of errors or field unset.
		 */
		flags &= PARSE_PORT_MASK; /* the only flags left to process */
		if (port) {
			if (flags == PARSE_PORT_FORBID) {
				error = 1;	/* port was forbidden */
				sa->sin_port = def->sin_port;
			} else if (flags == PARSE_PORT_IGNORE)
				sa->sin_port = def->sin_port;
			else /* accept or require */
				sa->sin_port = htons(strtol(port, NULL, 0));
		} else {
			sa->sin_port = def->sin_port;
			if (flags == PARSE_PORT_REQUIRE)
				error = 1;
		}
		/* Now deal with host part, even if we have errors before. */
		hp = ast_gethostbyname(buf, &ahp);
		if (hp)	/* resolved successfully */
			memcpy(&sa->sin_addr, hp->h_addr, sizeof(sa->sin_addr));
		else {
			error = 1;
			sa->sin_addr = def->sin_addr;
		}
		ast_debug(3,
			"extract inaddr from [%s] gives [%s:%d](%d)\n",
			arg, ast_inet_ntoa(sa->sin_addr),
			ntohs(sa->sin_port), error);
	    	break;
	    }
	}
	va_end(ap);
	return error;
}

static int config_command(int fd, int argc, char **argv) 
{
	struct ast_config_engine *eng;
	struct ast_config_map *map;
	
	ast_mutex_lock(&config_lock);

	ast_cli(fd, "\n\n");
	for (eng = config_engine_list; eng; eng = eng->next) {
		ast_cli(fd, "\nConfig Engine: %s\n", eng->name);
		for (map = config_maps; map; map = map->next)
			if (!strcasecmp(map->driver, eng->name)) {
				ast_cli(fd, "===> %s (db=%s, table=%s)\n", map->name, map->database,
					map->table ? map->table : map->name);
			}
	}
	ast_cli(fd,"\n\n");
	
	ast_mutex_unlock(&config_lock);

	return 0;
}

static char show_config_help[] =
	"Usage: core show config mappings\n"
	"	Shows the filenames to config engines.\n";

static struct ast_cli_entry cli_config[] = {
	{ { "core", "show", "config", "mappings", NULL },
	config_command, "Display config mappings (file names to config engines)",
	show_config_help },
};

int register_config_cli() 
{
	ast_cli_register_multiple(cli_config, sizeof(cli_config) / sizeof(struct ast_cli_entry));
	return 0;
}
