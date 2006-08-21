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
 * \brief Provide a directory of extensions
 *
 * \author Mark Spencer <markster@digium.com>
 * 
 * \ingroup applications
 */
 
#include "asterisk.h"

ASTERISK_FILE_VERSION(__FILE__, "$Revision$")

#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>

#include "asterisk/lock.h"
#include "asterisk/file.h"
#include "asterisk/logger.h"
#include "asterisk/channel.h"
#include "asterisk/pbx.h"
#include "asterisk/module.h"
#include "asterisk/config.h"
#include "asterisk/say.h"
#include "asterisk/utils.h"
#include "asterisk/app.h"

#ifdef USE_ODBC_STORAGE
#include <errno.h>
#include <sys/mman.h>
#include "asterisk/res_odbc.h"

static char odbc_database[80] = "asterisk";
static char odbc_table[80] = "voicemessages";
static char vmfmts[80] = "wav";
#endif

static char *app = "Directory";

static char *synopsis = "Provide directory of voicemail extensions";
static char *descrip =
"  Directory(vm-context[|dial-context[|options]]): This application will present\n"
"the calling channel with a directory of extensions from which they can search\n"
"by name. The list of names and corresponding extensions is retrieved from the\n"
"voicemail configuration file, voicemail.conf.\n"
"  This application will immediately exit if one of the following DTMF digits are\n"
"received and the extension to jump to exists:\n"
"    0 - Jump to the 'o' extension, if it exists.\n"
"    * - Jump to the 'a' extension, if it exists.\n\n"
"  Parameters:\n"
"    vm-context   - This is the context within voicemail.conf to use for the\n"
"                   Directory.\n"
"    dial-context - This is the dialplan context to use when looking for an\n"
"                   extension that the user has selected, or when jumping to the\n"
"                   'o' or 'a' extension.\n\n"
"  Options:\n"
"    e - In addition to the name, also read the extension number to the\n"
"        caller before presenting dialing options.\n"
"    f - Allow the caller to enter the first name of a user in the directory\n"
"        instead of using the last name.\n";

/* For simplicity, I'm keeping the format compatible with the voicemail config,
   but i'm open to suggestions for isolating it */

#define VOICEMAIL_CONFIG "voicemail.conf"

/* How many digits to read in */
#define NUMDIGITS 3


#ifdef USE_ODBC_STORAGE
static void retrieve_file(char *dir)
{
	int x = 0;
	int res;
	int fd=-1;
	size_t fdlen = 0;
	void *fdm=NULL;
	SQLHSTMT stmt;
	char sql[256];
	char fmt[80]="";
	char *c;
	SQLLEN colsize;
	char full_fn[256];

	odbc_obj *obj;
	obj = fetch_odbc_obj(odbc_database, 0);
	if (obj) {
		do {
			ast_copy_string(fmt, vmfmts, sizeof(fmt));
			c = strchr(fmt, '|');
			if (c)
				*c = '\0';
			if (!strcasecmp(fmt, "wav49"))
				strcpy(fmt, "WAV");
			snprintf(full_fn, sizeof(full_fn), "%s.%s", dir, fmt);
			res = SQLAllocHandle(SQL_HANDLE_STMT, obj->con, &stmt);
			if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
				ast_log(LOG_WARNING, "SQL Alloc Handle failed!\n");
				break;
			}
			snprintf(sql, sizeof(sql), "SELECT recording FROM %s WHERE dir=? AND msgnum=-1", odbc_table);
			res = SQLPrepare(stmt, (unsigned char *)sql, SQL_NTS);
			if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
				ast_log(LOG_WARNING, "SQL Prepare failed![%s]\n", sql);
				SQLFreeHandle(SQL_HANDLE_STMT, stmt);
				break;
			}
			SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_CHAR, strlen(dir), 0, (void *)dir, 0, NULL);
			res = odbc_smart_execute(obj, stmt);
			if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
				ast_log(LOG_WARNING, "SQL Execute error!\n[%s]\n\n", sql);
				SQLFreeHandle(SQL_HANDLE_STMT, stmt);
				break;
			}
			res = SQLFetch(stmt);
			if (res == SQL_NO_DATA) {
				SQLFreeHandle(SQL_HANDLE_STMT, stmt);
				break;
			} else if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
				ast_log(LOG_WARNING, "SQL Fetch error!\n[%s]\n\n", sql);
				SQLFreeHandle(SQL_HANDLE_STMT, stmt);
				break;
			}
			fd = open(full_fn, O_RDWR | O_CREAT | O_TRUNC, 0770);
			if (fd < 0) {
				ast_log(LOG_WARNING, "Failed to write '%s': %s\n", full_fn, strerror(errno));
				SQLFreeHandle(SQL_HANDLE_STMT, stmt);
				break;
			}

			res = SQLGetData(stmt, 1, SQL_BINARY, NULL, 0, &colsize);
			fdlen = colsize;
			if (fd > -1) {
				char tmp[1]="";
				lseek(fd, fdlen - 1, SEEK_SET);
				if (write(fd, tmp, 1) != 1) {
					close(fd);
					fd = -1;
					break;
				}
				if (fd > -1)
					fdm = mmap(NULL, fdlen, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
			}
			if (fdm) {
				memset(fdm, 0, fdlen);
				res = SQLGetData(stmt, x + 1, SQL_BINARY, fdm, fdlen, &colsize);
				if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
					ast_log(LOG_WARNING, "SQL Get Data error!\n[%s]\n\n", sql);
					SQLFreeHandle(SQL_HANDLE_STMT, stmt);
					break;
				}
			}
			SQLFreeHandle(SQL_HANDLE_STMT, stmt);
		} while (0);
	} else
		ast_log(LOG_WARNING, "Failed to obtain database object for '%s'!\n", odbc_database);
	if (fdm)
		munmap(fdm, fdlen);
	if (fd > -1)
		close(fd);
	return;
}
#endif

static char *convert(char *lastname)
{
	char *tmp;
	int lcount = 0;
	tmp = ast_malloc(NUMDIGITS + 1);
	if (tmp) {
		while((*lastname > 32) && lcount < NUMDIGITS) {
			switch(toupper(*lastname)) {
			case '1':
				tmp[lcount++] = '1';
				break;
			case '2':
			case 'A':
			case 'B':
			case 'C':
				tmp[lcount++] = '2';
				break;
			case '3':
			case 'D':
			case 'E':
			case 'F':
				tmp[lcount++] = '3';
				break;
			case '4':
			case 'G':
			case 'H':
			case 'I':
				tmp[lcount++] = '4';
				break;
			case '5':
			case 'J':
			case 'K':
			case 'L':
				tmp[lcount++] = '5';
				break;
			case '6':
			case 'M':
			case 'N':
			case 'O':
				tmp[lcount++] = '6';
				break;
			case '7':
			case 'P':
			case 'Q':
			case 'R':
			case 'S':
				tmp[lcount++] = '7';
				break;
			case '8':
			case 'T':
			case 'U':
			case 'V':
				tmp[lcount++] = '8';
				break;
			case '9':
			case 'W':
			case 'X':
			case 'Y':
			case 'Z':
				tmp[lcount++] = '9';
				break;
			}
			lastname++;
		}
		tmp[lcount] = '\0';
	}
	return tmp;
}

/* play name of mailbox owner.
 * returns:  -1 for bad or missing extension
 *           '1' for selected entry from directory
 *           '*' for skipped entry from directory
 */
static int play_mailbox_owner(struct ast_channel *chan, char *context,
		char *dialcontext, char *ext, char *name, int readext,
		int fromappvm)
{
	int res = 0;
	int loop;
	char fn[256];

	/* Check for the VoiceMail2 greeting first */
	snprintf(fn, sizeof(fn), "%s/voicemail/%s/%s/greet",
		ast_config_AST_SPOOL_DIR, context, ext);
#ifdef USE_ODBC_STORAGE
	retrieve_file(fn);
#endif

	if (ast_fileexists(fn, NULL, chan->language) <= 0) {
		/* no file, check for an old-style Voicemail greeting */
		snprintf(fn, sizeof(fn), "%s/vm/%s/greet",
			ast_config_AST_SPOOL_DIR, ext);
	}
#ifdef USE_ODBC_STORAGE
	retrieve_file(fn2);
#endif

	if (ast_fileexists(fn, NULL, chan->language) > 0) {
		res = ast_stream_and_wait(chan, fn, chan->language, AST_DIGIT_ANY);
		ast_stopstream(chan);
		/* If Option 'e' was specified, also read the extension number with the name */
		if (readext) {
			ast_stream_and_wait(chan, "vm-extension", chan->language, AST_DIGIT_ANY);
			res = ast_say_character_str(chan, ext, AST_DIGIT_ANY, chan->language);
		}
	} else {
		res = ast_say_character_str(chan, S_OR(name, ext), AST_DIGIT_ANY, chan->language);
		if (!ast_strlen_zero(name) && readext) {
			ast_stream_and_wait(chan, "vm-extension", chan->language, AST_DIGIT_ANY);
			res = ast_say_character_str(chan, ext, AST_DIGIT_ANY, chan->language);
		}
	}
#ifdef USE_ODBC_STORAGE
	ast_filedelete(fn, NULL);	
	ast_filedelete(fn2, NULL);	
#endif

	for (loop = 3 ; loop > 0; loop--) {
		if (!res)
			res = ast_stream_and_wait(chan, "dir-instr", chan->language, AST_DIGIT_ANY);
		if (!res)
			res = ast_waitfordigit(chan, 3000);
		ast_stopstream(chan);
	
		if (res < 0) /* User hungup, so jump out now */
			break;
		if (res == '1') {	/* Name selected */
			if (fromappvm) {
				/* We still want to set the exten though */
				ast_copy_string(chan->exten, ext, sizeof(chan->exten));
			} else {
				if (ast_goto_if_exists(chan, dialcontext, ext, 1)) {
					ast_log(LOG_WARNING,
						"Can't find extension '%s' in context '%s'.  "
						"Did you pass the wrong context to Directory?\n",
						ext, dialcontext);
					res = -1;
				}
			}
			break;
		}
		if (res == '*') /* Skip to next match in list */
			break;

		/* Not '1', or '*', so decrement number of tries */
		res = 0;
	}

	return(res);
}

static struct ast_config *realtime_directory(char *context)
{
	struct ast_config *cfg;
	struct ast_config *rtdata;
	struct ast_category *cat;
	struct ast_variable *var;
	char *mailbox;
	char *fullname;
	char *hidefromdir;
	char tmp[100];

	/* Load flat file config. */
	cfg = ast_config_load(VOICEMAIL_CONFIG);

	if (!cfg) {
		/* Loading config failed. */
		ast_log(LOG_WARNING, "Loading config failed.\n");
		return NULL;
	}

	/* Get realtime entries, categorized by their mailbox number
	   and present in the requested context */
	rtdata = ast_load_realtime_multientry("voicemail", "mailbox LIKE", "%", "context", context, NULL);

	/* if there are no results, just return the entries from the config file */
	if (!rtdata)
		return cfg;

	/* Does the context exist within the config file? If not, make one */
	cat = ast_category_get(cfg, context);
	if (!cat) {
		cat = ast_category_new(context);
		if (!cat) {
			ast_log(LOG_WARNING, "Out of memory\n");
			ast_config_destroy(cfg);
			return NULL;
		}
		ast_category_append(cfg, cat);
	}

	mailbox = NULL;
	while ( (mailbox = ast_category_browse(rtdata, mailbox)) ) {
		fullname = ast_variable_retrieve(rtdata, mailbox, "fullname");
		hidefromdir = ast_variable_retrieve(rtdata, mailbox, "hidefromdir");
		snprintf(tmp, sizeof(tmp), "no-password,%s,hidefromdir=%s",
			 fullname ? fullname : "",
			 hidefromdir ? hidefromdir : "no");
		var = ast_variable_new(mailbox, tmp);
		if (var)
			ast_variable_append(cat, var);
		else
			ast_log(LOG_WARNING, "Out of memory adding mailbox '%s'\n", mailbox);
	}
	ast_config_destroy(rtdata);

	return cfg;
}

static int do_directory(struct ast_channel *chan, struct ast_config *cfg, char *context, char *dialcontext, char digit, int last, int readext, int fromappvm)
{
	/* Read in the first three digits..  "digit" is the first digit, already read */
	char ext[NUMDIGITS + 1];
	char name[80] = "";
	struct ast_variable *v;
	int res;
	int found=0;
	int lastuserchoice = 0;
	char *start, *pos, *conv,*stringp=NULL;

	if (ast_strlen_zero(context)) {
		ast_log(LOG_WARNING,
			"Directory must be called with an argument "
			"(context in which to interpret extensions)\n");
		return -1;
	}
	if (digit == '0') {
		if (!ast_goto_if_exists(chan, chan->context, "o", 1) ||
		    (!ast_strlen_zero(chan->macrocontext) &&
		     !ast_goto_if_exists(chan, chan->macrocontext, "o", 1))) {
			return 0;
		} else {
			ast_log(LOG_WARNING, "Can't find extension 'o' in current context.  "
				"Not Exiting the Directory!\n");
			res = 0;
		}
	}	
	if (digit == '*') {
		if (!ast_goto_if_exists(chan, chan->context, "a", 1) ||
		    (!ast_strlen_zero(chan->macrocontext) &&
		     !ast_goto_if_exists(chan, chan->macrocontext, "a", 1))) {
			return 0;
		} else {
			ast_log(LOG_WARNING, "Can't find extension 'a' in current context.  "
				"Not Exiting the Directory!\n");
			res = 0;
		}
	}	
	memset(ext, 0, sizeof(ext));
	ext[0] = digit;
	res = 0;
	if (ast_readstring(chan, ext + 1, NUMDIGITS - 1, 3000, 3000, "#") < 0) res = -1;
	if (!res) {
		/* Search for all names which start with those digits */
		v = ast_variable_browse(cfg, context);
		while(v && !res) {
			/* Find all candidate extensions */
			while(v) {
				/* Find a candidate extension */
				start = strdup(v->value);
				if (start && !strcasestr(start, "hidefromdir=yes")) {
					stringp=start;
					strsep(&stringp, ",");
					pos = strsep(&stringp, ",");
					if (pos) {
						ast_copy_string(name, pos, sizeof(name));
						/* Grab the last name */
						if (last && strrchr(pos,' '))
							pos = strrchr(pos, ' ') + 1;
						conv = convert(pos);
						if (conv) {
							if (!strcmp(conv, ext)) {
								/* Match! */
								found++;
								free(conv);
								free(start);
								break;
							}
							free(conv);
						}
					}
					free(start);
				}
				v = v->next;
			}

			if (v) {
				/* We have a match -- play a greeting if they have it */
				res = play_mailbox_owner(chan, context, dialcontext, v->name, name, readext, fromappvm);
				switch (res) {
					case -1:
						/* user pressed '1' but extension does not exist, or
						 * user hungup
						 */
						lastuserchoice = 0;
						break;
					case '1':
						/* user pressed '1' and extensions exists;
						   play_mailbox_owner will already have done
						   a goto() on the channel
						 */
						lastuserchoice = res;
						break;
					case '*':
						/* user pressed '*' to skip something found */
						lastuserchoice = res;
						res = 0;
						break;
					default:
						break;
				}
				v = v->next;
			}
		}

		if (lastuserchoice != '1') {
			res = ast_streamfile(chan, found ? "dir-nomore" : "dir-nomatch", chan->language);
			if (!res)
				res = 1;
			return res;
		}
		return 0;
	}
	return res;
}

static int directory_exec(struct ast_channel *chan, void *data)
{
	int res = 0;
	struct ast_module_user *u;
	struct ast_config *cfg;
	int last = 1;
	int readext = 0;
	int fromappvm = 0;
	char *dirintro, *parse;
	AST_DECLARE_APP_ARGS(args,
		AST_APP_ARG(vmcontext);
		AST_APP_ARG(dialcontext);
		AST_APP_ARG(options);
	);

	if (ast_strlen_zero(data)) {
		ast_log(LOG_WARNING, "Directory requires an argument (context[,dialcontext])\n");
		return -1;
	}

	u = ast_module_user_add(chan);

	parse = ast_strdupa(data);

	AST_STANDARD_APP_ARGS(args, parse);
		
	if (args.options) {
		if (strchr(args.options, 'f'))
			last = 0;
		if (strchr(args.options, 'e'))
			readext = 1;
		if (strchr(args.options, 'v'))
			fromappvm = 1;
	}

	if (ast_strlen_zero(args.dialcontext))	
		args.dialcontext = args.vmcontext;

	cfg = realtime_directory(args.vmcontext);
	if (!cfg) {
		ast_log(LOG_ERROR, "Unable to read the configuration data!\n");
		ast_module_user_remove(u);
		return -1;
	}

	dirintro = ast_variable_retrieve(cfg, args.vmcontext, "directoryintro");
	if (ast_strlen_zero(dirintro))
		dirintro = ast_variable_retrieve(cfg, "general", "directoryintro");
	if (ast_strlen_zero(dirintro))
		dirintro = last ? "dir-intro" : "dir-intro-fn";

	if (chan->_state != AST_STATE_UP) 
		res = ast_answer(chan);

	for (;;) {
		if (!res)
			res = ast_stream_and_wait(chan, dirintro, chan->language, AST_DIGIT_ANY);
		ast_stopstream(chan);
		if (!res)
			res = ast_waitfordigit(chan, 5000);
		if (res > 0) {
			res = do_directory(chan, cfg, args.vmcontext, args.dialcontext, res, last, readext, fromappvm);
			if (res > 0) {
				res = ast_waitstream(chan, AST_DIGIT_ANY);
				ast_stopstream(chan);
				if (res >= 0)
					continue;
			}
		}
		break;
	}
	ast_config_destroy(cfg);
	ast_module_user_remove(u);
	return res;
}

static int unload_module(void)
{
	int res;
	res = ast_unregister_application(app);
	return res;
}

static int load_module(void)
{
#ifdef USE_ODBC_STORAGE
	struct ast_config *cfg = ast_config_load(VOICEMAIL_CONFIG);
	char *tmp;

	if (cfg) {
		if ((tmp = ast_variable_retrieve(cfg, "general", "odbcstorage"))) {
			ast_copy_string(odbc_database, tmp, sizeof(odbc_database));
		}
		if ((tmp = ast_variable_retrieve(cfg, "general", "odbctable"))) {
			ast_copy_string(odbc_table, tmp, sizeof(odbc_table));
		}
		if ((tmp = ast_variable_retrieve(cfg, "general", "format"))) {
			ast_copy_string(vmfmts, tmp, sizeof(vmfmts));
		}
		ast_config_destroy(cfg);
	} else
		ast_log(LOG_WARNING, "Unable to load " VOICEMAIL_CONFIG " - ODBC defaults will be used\n");
#endif

	return ast_register_application(app, directory_exec, synopsis, descrip);
}

AST_MODULE_INFO_STANDARD(ASTERISK_GPL_KEY, "Extension Directory");
