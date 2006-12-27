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
 * \brief Call Detail Record API 
 *
 * \author Mark Spencer <markster@digium.com>
 * 
 * \note Includes code and algorithms from the Zapata library.
 *
 * \note We do a lot of checking here in the CDR code to try to be sure we don't ever let a CDR slip
 * through our fingers somehow.  If someone allocates a CDR, it must be completely handled normally
 * or a WARNING shall be logged, so that we can best keep track of any escape condition where the CDR
 * isn't properly generated and posted.
 */


#include "asterisk.h"

ASTERISK_FILE_VERSION(__FILE__, "$Revision$")

#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <signal.h>

#include "asterisk/lock.h"
#include "asterisk/channel.h"
#include "asterisk/cdr.h"
#include "asterisk/logger.h"
#include "asterisk/callerid.h"
#include "asterisk/causes.h"
#include "asterisk/options.h"
#include "asterisk/linkedlists.h"
#include "asterisk/utils.h"
#include "asterisk/sched.h"
#include "asterisk/config.h"
#include "asterisk/cli.h"
#include "asterisk/stringfields.h"

/*! Default AMA flag for billing records (CDR's) */
int ast_default_amaflags = AST_CDR_DOCUMENTATION;
char ast_default_accountcode[AST_MAX_ACCOUNT_CODE];

struct ast_cdr_beitem {
	char name[20];
	char desc[80];
	ast_cdrbe be;
	AST_LIST_ENTRY(ast_cdr_beitem) list;
};

static AST_LIST_HEAD_STATIC(be_list, ast_cdr_beitem);

struct ast_cdr_batch_item {
	struct ast_cdr *cdr;
	struct ast_cdr_batch_item *next;
};

static struct ast_cdr_batch {
	int size;
	struct ast_cdr_batch_item *head;
	struct ast_cdr_batch_item *tail;
} *batch = NULL;

static struct sched_context *sched;
static int cdr_sched = -1;
static pthread_t cdr_thread = AST_PTHREADT_NULL;

#define BATCH_SIZE_DEFAULT 100
#define BATCH_TIME_DEFAULT 300
#define BATCH_SCHEDULER_ONLY_DEFAULT 0
#define BATCH_SAFE_SHUTDOWN_DEFAULT 1

static int enabled;
static int batchmode;
static int batchsize;
static int batchtime;
static int batchscheduleronly;
static int batchsafeshutdown;

AST_MUTEX_DEFINE_STATIC(cdr_batch_lock);

/* these are used to wake up the CDR thread when there's work to do */
AST_MUTEX_DEFINE_STATIC(cdr_pending_lock);
static ast_cond_t cdr_pending_cond;


/*! Register a CDR driver. Each registered CDR driver generates a CDR 
	\return 0 on success, -1 on failure 
*/
int ast_cdr_register(const char *name, const char *desc, ast_cdrbe be)
{
	struct ast_cdr_beitem *i;

	if (!name)
		return -1;
	if (!be) {
		ast_log(LOG_WARNING, "CDR engine '%s' lacks backend\n", name);
		return -1;
	}

	AST_LIST_LOCK(&be_list);
	AST_LIST_TRAVERSE(&be_list, i, list) {
		if (!strcasecmp(name, i->name))
			break;
	}
	AST_LIST_UNLOCK(&be_list);

	if (i) {
		ast_log(LOG_WARNING, "Already have a CDR backend called '%s'\n", name);
		return -1;
	}

	if (!(i = ast_calloc(1, sizeof(*i)))) 	
		return -1;

	i->be = be;
	ast_copy_string(i->name, name, sizeof(i->name));
	ast_copy_string(i->desc, desc, sizeof(i->desc));

	AST_LIST_LOCK(&be_list);
	AST_LIST_INSERT_HEAD(&be_list, i, list);
	AST_LIST_UNLOCK(&be_list);

	return 0;
}

/*! unregister a CDR driver */
void ast_cdr_unregister(const char *name)
{
	struct ast_cdr_beitem *i = NULL;

	AST_LIST_LOCK(&be_list);
	AST_LIST_TRAVERSE_SAFE_BEGIN(&be_list, i, list) {
		if (!strcasecmp(name, i->name)) {
			AST_LIST_REMOVE_CURRENT(&be_list, list);
			if (option_verbose > 1)
				ast_verbose(VERBOSE_PREFIX_2 "Unregistered '%s' CDR backend\n", name);
			free(i);
			break;
		}
	}
	AST_LIST_TRAVERSE_SAFE_END;
	AST_LIST_UNLOCK(&be_list);
}

/*! Duplicate a CDR record 
	\returns Pointer to new CDR record
*/
struct ast_cdr *ast_cdr_dup(struct ast_cdr *cdr) 
{
	struct ast_cdr *newcdr = ast_cdr_alloc();

	if (!newcdr)
		return NULL;

	memcpy(newcdr, cdr, sizeof(*newcdr));
	/* The varshead is unusable, volatile even, after the memcpy so we take care of that here */
	memset(&newcdr->varshead, 0, sizeof(newcdr->varshead));
	ast_cdr_copy_vars(newcdr, cdr);
	newcdr->next = NULL;

	return newcdr;
}

static const char *ast_cdr_getvar_internal(struct ast_cdr *cdr, const char *name, int recur) 
{
	if (ast_strlen_zero(name))
		return NULL;

	for (; cdr; cdr = recur ? cdr->next : NULL) {
		struct ast_var_t *variables;
		struct varshead *headp = &cdr->varshead;
		AST_LIST_TRAVERSE(headp, variables, entries) {
			if (!strcasecmp(name, ast_var_name(variables)))
				return ast_var_value(variables);
		}
	}

	return NULL;
}

static void cdr_get_tv(struct timeval tv, const char *fmt, char *buf, int bufsize)
{
	if (fmt == NULL) {	/* raw mode */
		snprintf(buf, bufsize, "%ld.%06ld", (long)tv.tv_sec, (long)tv.tv_usec);
	} else {  
		time_t t = tv.tv_sec;
		if (t) {
			struct tm tm;
			localtime_r(&t, &tm);
			strftime(buf, bufsize, fmt, &tm);
		}
	}
}

/*! CDR channel variable retrieval */
void ast_cdr_getvar(struct ast_cdr *cdr, const char *name, char **ret, char *workspace, int workspacelen, int recur, int raw) 
{
	const char *fmt = "%Y-%m-%d %T";
	const char *varbuf;

	*ret = NULL;
	/* special vars (the ones from the struct ast_cdr when requested by name) 
	   I'd almost say we should convert all the stringed vals to vars */

	if (!strcasecmp(name, "clid"))
		ast_copy_string(workspace, cdr->clid, workspacelen);
	else if (!strcasecmp(name, "src"))
		ast_copy_string(workspace, cdr->src, workspacelen);
	else if (!strcasecmp(name, "dst"))
		ast_copy_string(workspace, cdr->dst, workspacelen);
	else if (!strcasecmp(name, "dcontext"))
		ast_copy_string(workspace, cdr->dcontext, workspacelen);
	else if (!strcasecmp(name, "channel"))
		ast_copy_string(workspace, cdr->channel, workspacelen);
	else if (!strcasecmp(name, "dstchannel"))
		ast_copy_string(workspace, cdr->dstchannel, workspacelen);
	else if (!strcasecmp(name, "lastapp"))
		ast_copy_string(workspace, cdr->lastapp, workspacelen);
	else if (!strcasecmp(name, "lastdata"))
		ast_copy_string(workspace, cdr->lastdata, workspacelen);
	else if (!strcasecmp(name, "start"))
		cdr_get_tv(cdr->start, raw ? NULL : fmt, workspace, workspacelen);
	else if (!strcasecmp(name, "answer"))
		cdr_get_tv(cdr->answer, raw ? NULL : fmt, workspace, workspacelen);
	else if (!strcasecmp(name, "end"))
		cdr_get_tv(cdr->end, raw ? NULL : fmt, workspace, workspacelen);
	else if (!strcasecmp(name, "duration"))
		snprintf(workspace, workspacelen, "%ld", cdr->duration);
	else if (!strcasecmp(name, "billsec"))
		snprintf(workspace, workspacelen, "%ld", cdr->billsec);
	else if (!strcasecmp(name, "disposition")) {
		if (raw) {
			snprintf(workspace, workspacelen, "%ld", cdr->disposition);
		} else {
			ast_copy_string(workspace, ast_cdr_disp2str(cdr->disposition), workspacelen);
		}
	} else if (!strcasecmp(name, "amaflags")) {
		if (raw) {
			snprintf(workspace, workspacelen, "%ld", cdr->amaflags);
		} else {
			ast_copy_string(workspace, ast_cdr_flags2str(cdr->amaflags), workspacelen);
		}
	} else if (!strcasecmp(name, "accountcode"))
		ast_copy_string(workspace, cdr->accountcode, workspacelen);
	else if (!strcasecmp(name, "uniqueid"))
		ast_copy_string(workspace, cdr->uniqueid, workspacelen);
	else if (!strcasecmp(name, "userfield"))
		ast_copy_string(workspace, cdr->userfield, workspacelen);
	else if ((varbuf = ast_cdr_getvar_internal(cdr, name, recur)))
		ast_copy_string(workspace, varbuf, workspacelen);
	else
		workspace[0] = '\0';

	if (!ast_strlen_zero(workspace))
		*ret = workspace;
}

/* readonly cdr variables */
static	const char *cdr_readonly_vars[] = { "clid", "src", "dst", "dcontext", "channel", "dstchannel",
				    "lastapp", "lastdata", "start", "answer", "end", "duration",
				    "billsec", "disposition", "amaflags", "accountcode", "uniqueid",
				    "userfield", NULL };
/*! Set a CDR channel variable 
	\note You can't set the CDR variables that belong to the actual CDR record, like "billsec".
*/
int ast_cdr_setvar(struct ast_cdr *cdr, const char *name, const char *value, int recur) 
{
	struct ast_var_t *newvariable;
	struct varshead *headp;
	int x;
	
	for(x = 0; cdr_readonly_vars[x]; x++) {
		if (!strcasecmp(name, cdr_readonly_vars[x])) {
			ast_log(LOG_ERROR, "Attempt to set the '%s' read-only variable!.\n", name);
			return -1;
		}
	}

	if (!cdr) {
		ast_log(LOG_ERROR, "Attempt to set a variable on a nonexistent CDR record.\n");
		return -1;
	}

	for (; cdr; cdr = recur ? cdr->next : NULL) {
		headp = &cdr->varshead;
		AST_LIST_TRAVERSE_SAFE_BEGIN(headp, newvariable, entries) {
			if (!strcasecmp(ast_var_name(newvariable), name)) {
				/* there is already such a variable, delete it */
				AST_LIST_REMOVE_CURRENT(headp, entries);
				ast_var_delete(newvariable);
				break;
			}
		}
		AST_LIST_TRAVERSE_SAFE_END;

		if (value) {
			newvariable = ast_var_assign(name, value);
			AST_LIST_INSERT_HEAD(headp, newvariable, entries);
		}
	}

	return 0;
}

int ast_cdr_copy_vars(struct ast_cdr *to_cdr, struct ast_cdr *from_cdr)
{
	struct ast_var_t *variables, *newvariable = NULL;
	struct varshead *headpa, *headpb;
	const char *var, *val;
	int x = 0;

	headpa = &from_cdr->varshead;
	headpb = &to_cdr->varshead;

	AST_LIST_TRAVERSE(headpa,variables,entries) {
		if (variables &&
		    (var = ast_var_name(variables)) && (val = ast_var_value(variables)) &&
		    !ast_strlen_zero(var) && !ast_strlen_zero(val)) {
			newvariable = ast_var_assign(var, val);
			AST_LIST_INSERT_HEAD(headpb, newvariable, entries);
			x++;
		}
	}

	return x;
}

int ast_cdr_serialize_variables(struct ast_cdr *cdr, char *buf, size_t size, char delim, char sep, int recur) 
{
	struct ast_var_t *variables;
	const char *var, *val;
	char *tmp;
	char workspace[256];
	int total = 0, x = 0, i;

	memset(buf, 0, size);

	for (; cdr; cdr = recur ? cdr->next : NULL) {
		if (++x > 1)
			ast_build_string(&buf, &size, "\n");

		AST_LIST_TRAVERSE(&cdr->varshead, variables, entries) {
			if (variables &&
			    (var = ast_var_name(variables)) && (val = ast_var_value(variables)) &&
			    !ast_strlen_zero(var) && !ast_strlen_zero(val)) {
				if (ast_build_string(&buf, &size, "level %d: %s%c%s%c", x, var, delim, val, sep)) {
 					ast_log(LOG_ERROR, "Data Buffer Size Exceeded!\n");
 					break;
				} else
					total++;
			} else 
				break;
		}

		for (i = 0; cdr_readonly_vars[i]; i++) {
			ast_cdr_getvar(cdr, cdr_readonly_vars[i], &tmp, workspace, sizeof(workspace), 0, 0);
			if (!tmp)
				continue;
			
			if (ast_build_string(&buf, &size, "level %d: %s%c%s%c", x, cdr_readonly_vars[i], delim, tmp, sep)) {
				ast_log(LOG_ERROR, "Data Buffer Size Exceeded!\n");
				break;
			} else
				total++;
		}
	}

	return total;
}


void ast_cdr_free_vars(struct ast_cdr *cdr, int recur)
{

	/* clear variables */
	for (; cdr; cdr = recur ? cdr->next : NULL) {
		struct ast_var_t *vardata;
		struct varshead *headp = &cdr->varshead;
		while ((vardata = AST_LIST_REMOVE_HEAD(headp, entries)))
			ast_var_delete(vardata);
	}
}

/*! \brief  print a warning if cdr already posted */
static void check_post(struct ast_cdr *cdr)
{
	if (ast_test_flag(cdr, AST_CDR_FLAG_POSTED))
		ast_log(LOG_NOTICE, "CDR on channel '%s' already posted\n", S_OR(cdr->channel, "<unknown>"));
}

/*! \brief  print a warning if cdr already started */
static void check_start(struct ast_cdr *cdr)
{
	if (!ast_tvzero(cdr->start))
		ast_log(LOG_NOTICE, "CDR on channel '%s' already started\n", S_OR(cdr->channel, "<unknown>"));
}

void ast_cdr_free(struct ast_cdr *cdr)
{

	while (cdr) {
		struct ast_cdr *next = cdr->next;
		char *chan = S_OR(cdr->channel, "<unknown>");
		if (!ast_test_flag(cdr, AST_CDR_FLAG_POSTED) && !ast_test_flag(cdr, AST_CDR_FLAG_POST_DISABLED))
			ast_log(LOG_NOTICE, "CDR on channel '%s' not posted\n", chan);
		if (ast_tvzero(cdr->end))
			ast_log(LOG_NOTICE, "CDR on channel '%s' lacks end\n", chan);
		if (ast_tvzero(cdr->start))
			ast_log(LOG_NOTICE, "CDR on channel '%s' lacks start\n", chan);

		ast_cdr_free_vars(cdr, 0);
		free(cdr);
		cdr = next;
	}
}

struct ast_cdr *ast_cdr_alloc(void)
{
	return ast_calloc(1, sizeof(struct ast_cdr));
}

void ast_cdr_start(struct ast_cdr *cdr)
{
	char *chan; 

	for (; cdr; cdr = cdr->next) {
		if (!ast_test_flag(cdr, AST_CDR_FLAG_LOCKED)) {
			chan = S_OR(cdr->channel, "<unknown>");
			check_post(cdr);
			check_start(cdr);
			cdr->start = ast_tvnow();
		}
	}
}

void ast_cdr_answer(struct ast_cdr *cdr)
{

	for (; cdr; cdr = cdr->next) {
		check_post(cdr);
		if (cdr->disposition < AST_CDR_ANSWERED)
			cdr->disposition = AST_CDR_ANSWERED;
		if (ast_tvzero(cdr->answer))
			cdr->answer = ast_tvnow();
	}
}

void ast_cdr_busy(struct ast_cdr *cdr)
{

	for (; cdr; cdr = cdr->next) {
		if (!ast_test_flag(cdr, AST_CDR_FLAG_LOCKED)) {
			check_post(cdr);
			if (cdr->disposition < AST_CDR_BUSY)
				cdr->disposition = AST_CDR_BUSY;
		}
	}
}

void ast_cdr_failed(struct ast_cdr *cdr)
{
	for (; cdr; cdr = cdr->next) {
		check_post(cdr);
		if (!ast_test_flag(cdr, AST_CDR_FLAG_LOCKED)) {
			if (cdr->disposition < AST_CDR_FAILED)
				cdr->disposition = AST_CDR_FAILED;
		}
	}
}

int ast_cdr_disposition(struct ast_cdr *cdr, int cause)
{
	int res = 0;

	for (; cdr; cdr = cdr->next) {
		switch(cause) {
		case AST_CAUSE_BUSY:
			ast_cdr_busy(cdr);
			break;
		case AST_CAUSE_FAILURE:
			ast_cdr_failed(cdr);
			break;
		case AST_CAUSE_NORMAL:
			break;
		case AST_CAUSE_NOTDEFINED:
			res = -1;
			break;
		default:
			res = -1;
			ast_log(LOG_WARNING, "Cause not handled\n");
		}
	}
	return res;
}

void ast_cdr_setdestchan(struct ast_cdr *cdr, const char *chann)
{
	for (; cdr; cdr = cdr->next) {
		check_post(cdr);
		if (!ast_test_flag(cdr, AST_CDR_FLAG_LOCKED))
			ast_copy_string(cdr->dstchannel, chann, sizeof(cdr->dstchannel));
	}
}

void ast_cdr_setapp(struct ast_cdr *cdr, char *app, char *data)
{

	for (; cdr; cdr = cdr->next) {
		if (!ast_test_flag(cdr, AST_CDR_FLAG_LOCKED)) {
			check_post(cdr);
			if (!app)
				app = "";
			ast_copy_string(cdr->lastapp, app, sizeof(cdr->lastapp));
			if (!data)
				data = "";
			ast_copy_string(cdr->lastdata, data, sizeof(cdr->lastdata));
		}
	}
}

/* set cid info for one record */
static void set_one_cid(struct ast_cdr *cdr, struct ast_channel *c)
{
	/* Grab source from ANI or normal Caller*ID */
	const char *num = S_OR(c->cid.cid_ani, c->cid.cid_num);
	
	if (!ast_strlen_zero(c->cid.cid_name)) {
		if (!ast_strlen_zero(num))	/* both name and number */
			snprintf(cdr->clid, sizeof(cdr->clid), "\"%s\" <%s>", c->cid.cid_name, num);
		else				/* only name */
			ast_copy_string(cdr->clid, c->cid.cid_name, sizeof(cdr->clid));
	} else if (!ast_strlen_zero(num)) {	/* only number */
		ast_copy_string(cdr->clid, num, sizeof(cdr->clid));
	} else {				/* nothing known */
		cdr->clid[0] = '\0';
	}
	ast_copy_string(cdr->src, S_OR(num, ""), sizeof(cdr->src));

}
int ast_cdr_setcid(struct ast_cdr *cdr, struct ast_channel *c)
{
	for (; cdr; cdr = cdr->next) {
		if (ast_test_flag(cdr, AST_CDR_FLAG_LOCKED))
			set_one_cid(cdr, c);
	}
	return 0;
}

int ast_cdr_init(struct ast_cdr *cdr, struct ast_channel *c)
{
	char *chan;

	for ( ; cdr ; cdr = cdr->next) {
		if (!ast_test_flag(cdr, AST_CDR_FLAG_LOCKED)) {
			chan = S_OR(cdr->channel, "<unknown>");
			if (!ast_strlen_zero(cdr->channel)) 
				ast_log(LOG_WARNING, "CDR already initialized on '%s'\n", chan); 
			ast_copy_string(cdr->channel, c->name, sizeof(cdr->channel));
			set_one_cid(cdr, c);

			cdr->disposition = (c->_state == AST_STATE_UP) ?  AST_CDR_ANSWERED : AST_CDR_NOANSWER;
			cdr->amaflags = c->amaflags ? c->amaflags :  ast_default_amaflags;
			ast_copy_string(cdr->accountcode, c->accountcode, sizeof(cdr->accountcode));
			/* Destination information */
			ast_copy_string(cdr->dst, c->exten, sizeof(cdr->dst));
			ast_copy_string(cdr->dcontext, c->context, sizeof(cdr->dcontext));
			/* Unique call identifier */
			ast_copy_string(cdr->uniqueid, c->uniqueid, sizeof(cdr->uniqueid));
		}
	}
	return 0;
}

void ast_cdr_end(struct ast_cdr *cdr)
{
	for ( ; cdr ; cdr = cdr->next) {
		check_post(cdr);
		if (ast_tvzero(cdr->end))
			cdr->end = ast_tvnow();
		if (ast_tvzero(cdr->start)) {
			ast_log(LOG_WARNING, "CDR on channel '%s' has not started\n", S_OR(cdr->channel, "<unknown>"));
			cdr->disposition = AST_CDR_FAILED;
		} else
			cdr->duration = cdr->end.tv_sec - cdr->start.tv_sec;
		cdr->billsec = ast_tvzero(cdr->answer) ? 0 : cdr->end.tv_sec - cdr->answer.tv_sec;
	}
}

char *ast_cdr_disp2str(int disposition)
{
	switch (disposition) {
	case AST_CDR_NOANSWER:
		return "NO ANSWER";
	case AST_CDR_FAILED:
		return "FAILED";		
	case AST_CDR_BUSY:
		return "BUSY";		
	case AST_CDR_ANSWERED:
		return "ANSWERED";
	}
	return "UNKNOWN";
}

/*! Converts AMA flag to printable string */
char *ast_cdr_flags2str(int flag)
{
	switch(flag) {
	case AST_CDR_OMIT:
		return "OMIT";
	case AST_CDR_BILLING:
		return "BILLING";
	case AST_CDR_DOCUMENTATION:
		return "DOCUMENTATION";
	}
	return "Unknown";
}

int ast_cdr_setaccount(struct ast_channel *chan, const char *account)
{
	struct ast_cdr *cdr = chan->cdr;

	ast_string_field_set(chan, accountcode, account);
	for ( ; cdr ; cdr = cdr->next) {
		if (!ast_test_flag(cdr, AST_CDR_FLAG_LOCKED))
			ast_copy_string(cdr->accountcode, chan->accountcode, sizeof(cdr->accountcode));
	}
	return 0;
}

int ast_cdr_setamaflags(struct ast_channel *chan, const char *flag)
{
	struct ast_cdr *cdr;
	int newflag = ast_cdr_amaflags2int(flag);
	if (newflag) {
		for (cdr = chan->cdr; cdr; cdr = cdr->next)
			cdr->amaflags = newflag;
	}

	return 0;
}

int ast_cdr_setuserfield(struct ast_channel *chan, const char *userfield)
{
	struct ast_cdr *cdr = chan->cdr;

	for ( ; cdr ; cdr = cdr->next) {
		if (!ast_test_flag(cdr, AST_CDR_FLAG_LOCKED)) 
			ast_copy_string(cdr->userfield, userfield, sizeof(cdr->userfield));
	}

	return 0;
}

int ast_cdr_appenduserfield(struct ast_channel *chan, const char *userfield)
{
	struct ast_cdr *cdr = chan->cdr;

	for ( ; cdr ; cdr = cdr->next) {
		int len = strlen(cdr->userfield);

		if (!ast_test_flag(cdr, AST_CDR_FLAG_LOCKED))
			ast_copy_string(cdr->userfield + len, userfield, sizeof(cdr->userfield) - len);
	}

	return 0;
}

int ast_cdr_update(struct ast_channel *c)
{
	struct ast_cdr *cdr = c->cdr;

	for ( ; cdr ; cdr = cdr->next) {
		if (!ast_test_flag(cdr, AST_CDR_FLAG_LOCKED)) {
			set_one_cid(cdr, c);

			/* Copy account code et-al */	
			ast_copy_string(cdr->accountcode, c->accountcode, sizeof(cdr->accountcode));
			/* Destination information */ /* XXX privilege macro* ? */
			ast_copy_string(cdr->dst, S_OR(c->macroexten, c->exten), sizeof(cdr->dst));
			ast_copy_string(cdr->dcontext, S_OR(c->macrocontext, c->context), sizeof(cdr->dcontext));
		}
	}

	return 0;
}

int ast_cdr_amaflags2int(const char *flag)
{
	if (!strcasecmp(flag, "default"))
		return 0;
	if (!strcasecmp(flag, "omit"))
		return AST_CDR_OMIT;
	if (!strcasecmp(flag, "billing"))
		return AST_CDR_BILLING;
	if (!strcasecmp(flag, "documentation"))
		return AST_CDR_DOCUMENTATION;
	return -1;
}

static void post_cdr(struct ast_cdr *cdr)
{
	char *chan;
	struct ast_cdr_beitem *i;

	for ( ; cdr ; cdr = cdr->next) {
		chan = S_OR(cdr->channel, "<unknown>");
		check_post(cdr);
		if (ast_tvzero(cdr->end))
			ast_log(LOG_WARNING, "CDR on channel '%s' lacks end\n", chan);
		if (ast_tvzero(cdr->start))
			ast_log(LOG_WARNING, "CDR on channel '%s' lacks start\n", chan);
		ast_set_flag(cdr, AST_CDR_FLAG_POSTED);
		AST_LIST_LOCK(&be_list);
		AST_LIST_TRAVERSE(&be_list, i, list) {
			i->be(cdr);
		}
		AST_LIST_UNLOCK(&be_list);
	}
}

void ast_cdr_reset(struct ast_cdr *cdr, struct ast_flags *_flags)
{
	struct ast_cdr *dup;
	struct ast_flags flags = { 0 };

	if (_flags)
		ast_copy_flags(&flags, _flags, AST_FLAGS_ALL);

	for ( ; cdr ; cdr = cdr->next) {
		/* Detach if post is requested */
		if (ast_test_flag(&flags, AST_CDR_FLAG_LOCKED) || !ast_test_flag(cdr, AST_CDR_FLAG_LOCKED)) {
			if (ast_test_flag(&flags, AST_CDR_FLAG_POSTED)) {
				ast_cdr_end(cdr);
				if ((dup = ast_cdr_dup(cdr))) {
					ast_cdr_detach(dup);
				}
				ast_set_flag(cdr, AST_CDR_FLAG_POSTED);
			}

			/* clear variables */
			if (!ast_test_flag(&flags, AST_CDR_FLAG_KEEP_VARS)) {
				ast_cdr_free_vars(cdr, 0);
			}

			/* Reset to initial state */
			ast_clear_flag(cdr, AST_FLAGS_ALL);	
			memset(&cdr->start, 0, sizeof(cdr->start));
			memset(&cdr->end, 0, sizeof(cdr->end));
			memset(&cdr->answer, 0, sizeof(cdr->answer));
			cdr->billsec = 0;
			cdr->duration = 0;
			ast_cdr_start(cdr);
			cdr->disposition = AST_CDR_NOANSWER;
		}
	}
}

struct ast_cdr *ast_cdr_append(struct ast_cdr *cdr, struct ast_cdr *newcdr) 
{
	struct ast_cdr *ret;

	if (cdr) {
		ret = cdr;

		while (cdr->next)
			cdr = cdr->next;
		cdr->next = newcdr;
	} else {
		ret = newcdr;
	}

	return ret;
}

/*! \note Don't call without cdr_batch_lock */
static void reset_batch(void)
{
	batch->size = 0;
	batch->head = NULL;
	batch->tail = NULL;
}

/*! \note Don't call without cdr_batch_lock */
static int init_batch(void)
{
	/* This is the single meta-batch used to keep track of all CDRs during the entire life of the program */
	if (!(batch = ast_malloc(sizeof(*batch))))
		return -1;

	reset_batch();

	return 0;
}

static void *do_batch_backend_process(void *data)
{
	struct ast_cdr_batch_item *processeditem;
	struct ast_cdr_batch_item *batchitem = data;

	/* Push each CDR into storage mechanism(s) and free all the memory */
	while (batchitem) {
		post_cdr(batchitem->cdr);
		ast_cdr_free(batchitem->cdr);
		processeditem = batchitem;
		batchitem = batchitem->next;
		free(processeditem);
	}

	return NULL;
}

void ast_cdr_submit_batch(int shutdown)
{
	struct ast_cdr_batch_item *oldbatchitems = NULL;
	pthread_attr_t attr;
	pthread_t batch_post_thread = AST_PTHREADT_NULL;

	/* if there's no batch, or no CDRs in the batch, then there's nothing to do */
	if (!batch || !batch->head)
		return;

	/* move the old CDRs aside, and prepare a new CDR batch */
	ast_mutex_lock(&cdr_batch_lock);
	oldbatchitems = batch->head;
	reset_batch();
	ast_mutex_unlock(&cdr_batch_lock);

	/* if configured, spawn a new thread to post these CDRs,
	   also try to save as much as possible if we are shutting down safely */
	if (batchscheduleronly || shutdown) {
		if (option_debug)
			ast_log(LOG_DEBUG, "CDR single-threaded batch processing begins now\n");
		do_batch_backend_process(oldbatchitems);
	} else {
		pthread_attr_init(&attr);
		pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
		if (ast_pthread_create_background(&batch_post_thread, &attr, do_batch_backend_process, oldbatchitems)) {
			ast_log(LOG_WARNING, "CDR processing thread could not detach, now trying in this thread\n");
			do_batch_backend_process(oldbatchitems);
		} else {
			if (option_debug)
				ast_log(LOG_DEBUG, "CDR multi-threaded batch processing begins now\n");
		}
	}
}

static int submit_scheduled_batch(void *data)
{
	ast_cdr_submit_batch(0);
	/* manually reschedule from this point in time */
	cdr_sched = ast_sched_add(sched, batchtime * 1000, submit_scheduled_batch, NULL);
	/* returning zero so the scheduler does not automatically reschedule */
	return 0;
}

static void submit_unscheduled_batch(void)
{
	/* this is okay since we are not being called from within the scheduler */
	if (cdr_sched > -1)
		ast_sched_del(sched, cdr_sched);
	/* schedule the submission to occur ASAP (1 ms) */
	cdr_sched = ast_sched_add(sched, 1, submit_scheduled_batch, NULL);
	/* signal the do_cdr thread to wakeup early and do some work (that lazy thread ;) */
	ast_mutex_lock(&cdr_pending_lock);
	ast_cond_signal(&cdr_pending_cond);
	ast_mutex_unlock(&cdr_pending_lock);
}

void ast_cdr_detach(struct ast_cdr *cdr)
{
	struct ast_cdr_batch_item *newtail;
	int curr;

	/* maybe they disabled CDR stuff completely, so just drop it */
	if (!enabled) {
		if (option_debug)
			ast_log(LOG_DEBUG, "Dropping CDR !\n");
		ast_set_flag(cdr, AST_CDR_FLAG_POST_DISABLED);
		ast_cdr_free(cdr);
		return;
	}

	/* post stuff immediately if we are not in batch mode, this is legacy behaviour */
	if (!batchmode) {
		post_cdr(cdr);
		ast_cdr_free(cdr);
		return;
	}

	/* otherwise, each CDR gets put into a batch list (at the end) */
	if (option_debug)
		ast_log(LOG_DEBUG, "CDR detaching from this thread\n");

	/* we'll need a new tail for every CDR */
	if (!(newtail = ast_calloc(1, sizeof(*newtail)))) {
		post_cdr(cdr);
		ast_cdr_free(cdr);
		return;
	}

	/* don't traverse a whole list (just keep track of the tail) */
	ast_mutex_lock(&cdr_batch_lock);
	if (!batch)
		init_batch();
	if (!batch->head) {
		/* new batch is empty, so point the head at the new tail */
		batch->head = newtail;
	} else {
		/* already got a batch with something in it, so just append a new tail */
		batch->tail->next = newtail;
	}
	newtail->cdr = cdr;
	batch->tail = newtail;
	curr = batch->size++;
	ast_mutex_unlock(&cdr_batch_lock);

	/* if we have enough stuff to post, then do it */
	if (curr >= (batchsize - 1))
		submit_unscheduled_batch();
}

static void *do_cdr(void *data)
{
	struct timespec timeout;
	int schedms;
	int numevents = 0;

	for(;;) {
		struct timeval now;
		schedms = ast_sched_wait(sched);
		/* this shouldn't happen, but provide a 1 second default just in case */
		if (schedms <= 0)
			schedms = 1000;
		now = ast_tvadd(ast_tvnow(), ast_samp2tv(schedms, 1000));
		timeout.tv_sec = now.tv_sec;
		timeout.tv_nsec = now.tv_usec * 1000;
		/* prevent stuff from clobbering cdr_pending_cond, then wait on signals sent to it until the timeout expires */
		ast_mutex_lock(&cdr_pending_lock);
		ast_cond_timedwait(&cdr_pending_cond, &cdr_pending_lock, &timeout);
		numevents = ast_sched_runq(sched);
		ast_mutex_unlock(&cdr_pending_lock);
		if (option_debug > 1)
			ast_log(LOG_DEBUG, "Processed %d scheduled CDR batches from the run queue\n", numevents);
	}

	return NULL;
}

static int handle_cli_status(int fd, int argc, char *argv[])
{
	struct ast_cdr_beitem *beitem=NULL;
	int cnt=0;
	long nextbatchtime=0;

	if (argc > 2)
		return RESULT_SHOWUSAGE;

	ast_cli(fd, "CDR logging: %s\n", enabled ? "enabled" : "disabled");
	ast_cli(fd, "CDR mode: %s\n", batchmode ? "batch" : "simple");
	if (enabled) {
		if (batchmode) {
			if (batch)
				cnt = batch->size;
			if (cdr_sched > -1)
				nextbatchtime = ast_sched_when(sched, cdr_sched);
			ast_cli(fd, "CDR safe shut down: %s\n", batchsafeshutdown ? "enabled" : "disabled");
			ast_cli(fd, "CDR batch threading model: %s\n", batchscheduleronly ? "scheduler only" : "scheduler plus separate threads");
			ast_cli(fd, "CDR current batch size: %d record%s\n", cnt, (cnt != 1) ? "s" : "");
			ast_cli(fd, "CDR maximum batch size: %d record%s\n", batchsize, (batchsize != 1) ? "s" : "");
			ast_cli(fd, "CDR maximum batch time: %d second%s\n", batchtime, (batchtime != 1) ? "s" : "");
			ast_cli(fd, "CDR next scheduled batch processing time: %ld second%s\n", nextbatchtime, (nextbatchtime != 1) ? "s" : "");
		}
		AST_LIST_LOCK(&be_list);
		AST_LIST_TRAVERSE(&be_list, beitem, list) {
			ast_cli(fd, "CDR registered backend: %s\n", beitem->name);
		}
		AST_LIST_UNLOCK(&be_list);
	}

	return 0;
}

static int handle_cli_submit(int fd, int argc, char *argv[])
{
	if (argc > 2)
		return RESULT_SHOWUSAGE;

	submit_unscheduled_batch();
	ast_cli(fd, "Submitted CDRs to backend engines for processing.  This may take a while.\n");

	return 0;
}

static struct ast_cli_entry cli_submit = {
	{ "cdr", "submit", NULL },
	handle_cli_submit, "Posts all pending batched CDR data",
	"Usage: cdr submit\n"
	"       Posts all pending batched CDR data to the configured CDR backend engine modules.\n"
};

static struct ast_cli_entry cli_status = {
	{ "cdr", "status", NULL },
	handle_cli_status, "Display the CDR status",
	"Usage: cdr status\n"
	"	Displays the Call Detail Record engine system status.\n"
};

static int do_reload(void)
{
	struct ast_config *config;
	const char *enabled_value;
	const char *batched_value;
	const char *scheduleronly_value;
	const char *batchsafeshutdown_value;
	const char *size_value;
	const char *time_value;
	const char *end_before_h_value;
	int cfg_size;
	int cfg_time;
	int was_enabled;
	int was_batchmode;
	int res=0;

	ast_mutex_lock(&cdr_batch_lock);

	batchsize = BATCH_SIZE_DEFAULT;
	batchtime = BATCH_TIME_DEFAULT;
	batchscheduleronly = BATCH_SCHEDULER_ONLY_DEFAULT;
	batchsafeshutdown = BATCH_SAFE_SHUTDOWN_DEFAULT;
	was_enabled = enabled;
	was_batchmode = batchmode;
	enabled = 1;
	batchmode = 0;

	/* don't run the next scheduled CDR posting while reloading */
	if (cdr_sched > -1)
		ast_sched_del(sched, cdr_sched);

	if ((config = ast_config_load("cdr.conf"))) {
		if ((enabled_value = ast_variable_retrieve(config, "general", "enable"))) {
			enabled = ast_true(enabled_value);
		}
		if ((batched_value = ast_variable_retrieve(config, "general", "batch"))) {
			batchmode = ast_true(batched_value);
		}
		if ((scheduleronly_value = ast_variable_retrieve(config, "general", "scheduleronly"))) {
			batchscheduleronly = ast_true(scheduleronly_value);
		}
		if ((batchsafeshutdown_value = ast_variable_retrieve(config, "general", "safeshutdown"))) {
			batchsafeshutdown = ast_true(batchsafeshutdown_value);
		}
		if ((size_value = ast_variable_retrieve(config, "general", "size"))) {
			if (sscanf(size_value, "%d", &cfg_size) < 1)
				ast_log(LOG_WARNING, "Unable to convert '%s' to a numeric value.\n", size_value);
			else if (size_value < 0)
				ast_log(LOG_WARNING, "Invalid maximum batch size '%d' specified, using default\n", cfg_size);
			else
				batchsize = cfg_size;
		}
		if ((time_value = ast_variable_retrieve(config, "general", "time"))) {
			if (sscanf(time_value, "%d", &cfg_time) < 1)
				ast_log(LOG_WARNING, "Unable to convert '%s' to a numeric value.\n", time_value);
			else if (time_value < 0)
				ast_log(LOG_WARNING, "Invalid maximum batch time '%d' specified, using default\n", cfg_time);
			else
				batchtime = cfg_time;
		}
		if ((end_before_h_value = ast_variable_retrieve(config, "general", "endbeforehexten")))
			ast_set2_flag(&ast_options, ast_true(end_before_h_value), AST_OPT_FLAG_END_CDR_BEFORE_H_EXTEN);
	}

	if (enabled && !batchmode) {
		ast_log(LOG_NOTICE, "CDR simple logging enabled.\n");
	} else if (enabled && batchmode) {
		cdr_sched = ast_sched_add(sched, batchtime * 1000, submit_scheduled_batch, NULL);
		ast_log(LOG_NOTICE, "CDR batch mode logging enabled, first of either size %d or time %d seconds.\n", batchsize, batchtime);
	} else {
		ast_log(LOG_NOTICE, "CDR logging disabled, data will be lost.\n");
	}

	/* if this reload enabled the CDR batch mode, create the background thread
	   if it does not exist */
	if (enabled && batchmode && (!was_enabled || !was_batchmode) && (cdr_thread == AST_PTHREADT_NULL)) {
		ast_cond_init(&cdr_pending_cond, NULL);
		if (ast_pthread_create_background(&cdr_thread, NULL, do_cdr, NULL) < 0) {
			ast_log(LOG_ERROR, "Unable to start CDR thread.\n");
			ast_sched_del(sched, cdr_sched);
		} else {
			ast_cli_register(&cli_submit);
			ast_register_atexit(ast_cdr_engine_term);
			res = 0;
		}
	/* if this reload disabled the CDR and/or batch mode and there is a background thread,
	   kill it */
	} else if (((!enabled && was_enabled) || (!batchmode && was_batchmode)) && (cdr_thread != AST_PTHREADT_NULL)) {
		/* wake up the thread so it will exit */
		pthread_cancel(cdr_thread);
		pthread_kill(cdr_thread, SIGURG);
		pthread_join(cdr_thread, NULL);
		cdr_thread = AST_PTHREADT_NULL;
		ast_cond_destroy(&cdr_pending_cond);
		ast_cli_unregister(&cli_submit);
		ast_unregister_atexit(ast_cdr_engine_term);
		res = 0;
		/* if leaving batch mode, then post the CDRs in the batch,
		   and don't reschedule, since we are stopping CDR logging */
		if (!batchmode && was_batchmode) {
			ast_cdr_engine_term();
		}
	} else {
		res = 0;
	}

	ast_mutex_unlock(&cdr_batch_lock);
	ast_config_destroy(config);

	return res;
}

int ast_cdr_engine_init(void)
{
	int res;

	sched = sched_context_create();
	if (!sched) {
		ast_log(LOG_ERROR, "Unable to create schedule context.\n");
		return -1;
	}

	ast_cli_register(&cli_status);

	res = do_reload();
	if (res) {
		ast_mutex_lock(&cdr_batch_lock);
		res = init_batch();
		ast_mutex_unlock(&cdr_batch_lock);
	}

	return res;
}

/* \note This actually gets called a couple of times at shutdown.  Once, before we start
   hanging up channels, and then again, after the channel hangup timeout expires */
void ast_cdr_engine_term(void)
{
	ast_cdr_submit_batch(batchsafeshutdown);
}

int ast_cdr_engine_reload(void)
{
	return do_reload();
}

