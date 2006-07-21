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
 * \brief Asterisk Logger
 * 
 * Logging routines
 *
 * \author Mark Spencer <markster@digium.com>
 */

#include "asterisk.h"

ASTERISK_FILE_VERSION(__FILE__, "$Revision$")

#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <unistd.h>
#include <time.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/stat.h>
#ifdef STACK_BACKTRACES
#include <execinfo.h>
#define MAX_BACKTRACE_FRAMES 20
#endif

#define SYSLOG_NAMES /* so we can map syslog facilities names to their numeric values,
		        from <syslog.h> which is included by logger.h */
#include <syslog.h>

static int syslog_level_map[] = {
	LOG_DEBUG,
	LOG_INFO,    /* arbitrary equivalent of LOG_EVENT */
	LOG_NOTICE,
	LOG_WARNING,
	LOG_ERR,
	LOG_DEBUG,
	LOG_DEBUG
};

#define SYSLOG_NLEVELS sizeof(syslog_level_map) / sizeof(int)

#include "asterisk/logger.h"
#include "asterisk/lock.h"
#include "asterisk/options.h"
#include "asterisk/channel.h"
#include "asterisk/config.h"
#include "asterisk/term.h"
#include "asterisk/cli.h"
#include "asterisk/utils.h"
#include "asterisk/manager.h"

#define MAX_MSG_QUEUE 200

#if defined(__linux__) && !defined(__NR_gettid)
#include <asm/unistd.h>
#endif

#if defined(__linux__) && defined(__NR_gettid)
#define GETTID() syscall(__NR_gettid)
#else
#define GETTID() getpid()
#endif


static char dateformat[256] = "%b %e %T";		/* Original Asterisk Format */

AST_MUTEX_DEFINE_STATIC(msglist_lock);
AST_MUTEX_DEFINE_STATIC(loglock);
static int filesize_reload_needed = 0;
static int global_logmask = -1;

static struct {
	unsigned int queue_log:1;
	unsigned int event_log:1;
} logfiles = { 1, 1 };

static struct msglist {
	char *msg;
	struct msglist *next;
} *list = NULL, *last = NULL;

static char hostname[MAXHOSTNAMELEN];

enum logtypes {
	LOGTYPE_SYSLOG,
	LOGTYPE_FILE,
	LOGTYPE_CONSOLE,
};

struct logchannel {
	int logmask;			/* What to log to this channel */
	int disabled;			/* If this channel is disabled or not */
	int facility; 			/* syslog facility */
	enum logtypes type;		/* Type of log channel */
	FILE *fileptr;			/* logfile logging file pointer */
	char filename[256];		/* Filename */
	struct logchannel *next;	/* Next channel in chain */
};

static struct logchannel *logchannels = NULL;

static int msgcnt = 0;

static FILE *eventlog = NULL;
static FILE *qlog = NULL;

static char *levels[] = {
	"DEBUG",
	"EVENT",
	"NOTICE",
	"WARNING",
	"ERROR",
	"VERBOSE",
	"DTMF"
};

static int colors[] = {
	COLOR_BRGREEN,
	COLOR_BRBLUE,
	COLOR_YELLOW,
	COLOR_BRRED,
	COLOR_RED,
	COLOR_GREEN,
	COLOR_BRGREEN
};

static int make_components(char *s, int lineno)
{
	char *w;
	int res = 0;
	char *stringp = s;

	while ((w = strsep(&stringp, ","))) {
		w = ast_skip_blanks(w);
		if (!strcasecmp(w, "error")) 
			res |= (1 << __LOG_ERROR);
		else if (!strcasecmp(w, "warning"))
			res |= (1 << __LOG_WARNING);
		else if (!strcasecmp(w, "notice"))
			res |= (1 << __LOG_NOTICE);
		else if (!strcasecmp(w, "event"))
			res |= (1 << __LOG_EVENT);
		else if (!strcasecmp(w, "debug"))
			res |= (1 << __LOG_DEBUG);
		else if (!strcasecmp(w, "verbose"))
			res |= (1 << __LOG_VERBOSE);
		else if (!strcasecmp(w, "dtmf"))
			res |= (1 << __LOG_DTMF);
		else {
			fprintf(stderr, "Logfile Warning: Unknown keyword '%s' at line %d of logger.conf\n", w, lineno);
		}
	}

	return res;
}

static struct logchannel *make_logchannel(char *channel, char *components, int lineno)
{
	struct logchannel *chan;
	char *facility;
#ifndef SOLARIS
	CODE *cptr;
#endif

	if (ast_strlen_zero(channel) || !(chan = ast_calloc(1, sizeof(*chan))))
		return NULL;

	if (!strcasecmp(channel, "console")) {
		chan->type = LOGTYPE_CONSOLE;
	} else if (!strncasecmp(channel, "syslog", 6)) {
		/*
		* syntax is:
		*  syslog.facility => level,level,level
		*/
		facility = strchr(channel, '.');
		if(!facility++ || !facility) {
			facility = "local0";
		}

#ifndef SOLARIS
		/*
 		* Walk through the list of facilitynames (defined in sys/syslog.h)
		* to see if we can find the one we have been given
		*/
		chan->facility = -1;
 		cptr = facilitynames;
		while (cptr->c_name) {
			if (!strcasecmp(facility, cptr->c_name)) {
		 		chan->facility = cptr->c_val;
				break;
			}
			cptr++;
		}
#else
		chan->facility = -1;
		if (!strcasecmp(facility, "kern")) 
			chan->facility = LOG_KERN;
		else if (!strcasecmp(facility, "USER")) 
			chan->facility = LOG_USER;
		else if (!strcasecmp(facility, "MAIL")) 
			chan->facility = LOG_MAIL;
		else if (!strcasecmp(facility, "DAEMON")) 
			chan->facility = LOG_DAEMON;
		else if (!strcasecmp(facility, "AUTH")) 
			chan->facility = LOG_AUTH;
		else if (!strcasecmp(facility, "SYSLOG")) 
			chan->facility = LOG_SYSLOG;
		else if (!strcasecmp(facility, "LPR")) 
			chan->facility = LOG_LPR;
		else if (!strcasecmp(facility, "NEWS")) 
			chan->facility = LOG_NEWS;
		else if (!strcasecmp(facility, "UUCP")) 
			chan->facility = LOG_UUCP;
		else if (!strcasecmp(facility, "CRON")) 
			chan->facility = LOG_CRON;
		else if (!strcasecmp(facility, "LOCAL0")) 
			chan->facility = LOG_LOCAL0;
		else if (!strcasecmp(facility, "LOCAL1")) 
			chan->facility = LOG_LOCAL1;
		else if (!strcasecmp(facility, "LOCAL2")) 
			chan->facility = LOG_LOCAL2;
		else if (!strcasecmp(facility, "LOCAL3")) 
			chan->facility = LOG_LOCAL3;
		else if (!strcasecmp(facility, "LOCAL4")) 
			chan->facility = LOG_LOCAL4;
		else if (!strcasecmp(facility, "LOCAL5")) 
			chan->facility = LOG_LOCAL5;
		else if (!strcasecmp(facility, "LOCAL6")) 
			chan->facility = LOG_LOCAL6;
		else if (!strcasecmp(facility, "LOCAL7")) 
			chan->facility = LOG_LOCAL7;
#endif /* Solaris */

		if (0 > chan->facility) {
			fprintf(stderr, "Logger Warning: bad syslog facility in logger.conf\n");
			free(chan);
			return NULL;
		}

		chan->type = LOGTYPE_SYSLOG;
		snprintf(chan->filename, sizeof(chan->filename), "%s", channel);
		openlog("asterisk", LOG_PID, chan->facility);
	} else {
		if (channel[0] == '/') {
			if(!ast_strlen_zero(hostname)) { 
				snprintf(chan->filename, sizeof(chan->filename) - 1,"%s.%s", channel, hostname);
			} else {
				ast_copy_string(chan->filename, channel, sizeof(chan->filename));
			}
		}		  
		
		if(!ast_strlen_zero(hostname)) {
			snprintf(chan->filename, sizeof(chan->filename), "%s/%s.%s",(char *)ast_config_AST_LOG_DIR, channel, hostname);
		} else {
			snprintf(chan->filename, sizeof(chan->filename), "%s/%s", (char *)ast_config_AST_LOG_DIR, channel);
		}
		chan->fileptr = fopen(chan->filename, "a");
		if (!chan->fileptr) {
			/* Can't log here, since we're called with a lock */
			fprintf(stderr, "Logger Warning: Unable to open log file '%s': %s\n", chan->filename, strerror(errno));
		} 
		chan->type = LOGTYPE_FILE;
	}
	chan->logmask = make_components(components, lineno);
	return chan;
}

static void init_logger_chain(void)
{
	struct logchannel *chan, *cur;
	struct ast_config *cfg;
	struct ast_variable *var;
	char *s;

	/* delete our list of log channels */
	ast_mutex_lock(&loglock);
	chan = logchannels;
	while (chan) {
		cur = chan->next;
		free(chan);
		chan = cur;
	}
	logchannels = NULL;
	ast_mutex_unlock(&loglock);
	
	global_logmask = 0;
	/* close syslog */
	closelog();
	
	cfg = ast_config_load("logger.conf");
	
	/* If no config file, we're fine, set default options. */
	if (!cfg) {
		fprintf(stderr, "Unable to open logger.conf: %s\n", strerror(errno));
		if (!(chan = ast_calloc(1, sizeof(*chan))))
			return;
		chan->type = LOGTYPE_CONSOLE;
		chan->logmask = 28; /*warning,notice,error */
		chan->next = logchannels;
		logchannels = chan;
		global_logmask |= chan->logmask;
		return;
	}
	
	ast_mutex_lock(&loglock);
	if ((s = ast_variable_retrieve(cfg, "general", "appendhostname"))) {
		if(ast_true(s)) {
			if(gethostname(hostname, sizeof(hostname)-1)) {
				ast_copy_string(hostname, "unknown", sizeof(hostname));
				ast_log(LOG_WARNING, "What box has no hostname???\n");
			}
		} else
			hostname[0] = '\0';
	} else
		hostname[0] = '\0';
	if ((s = ast_variable_retrieve(cfg, "general", "dateformat"))) {
		ast_copy_string(dateformat, s, sizeof(dateformat));
	} else
		ast_copy_string(dateformat, "%b %e %T", sizeof(dateformat));
	if ((s = ast_variable_retrieve(cfg, "general", "queue_log"))) {
		logfiles.queue_log = ast_true(s);
	}
	if ((s = ast_variable_retrieve(cfg, "general", "event_log"))) {
		logfiles.event_log = ast_true(s);
	}

	var = ast_variable_browse(cfg, "logfiles");
	while(var) {
		chan = make_logchannel(var->name, var->value, var->lineno);
		if (chan) {
			chan->next = logchannels;
			logchannels = chan;
			global_logmask |= chan->logmask;
		}
		var = var->next;
	}

	ast_config_destroy(cfg);
	ast_mutex_unlock(&loglock);
}

void ast_queue_log(const char *queuename, const char *callid, const char *agent, const char *event, const char *fmt, ...)
{
	va_list ap;
	ast_mutex_lock(&loglock);
	if (qlog) {
		va_start(ap, fmt);
		fprintf(qlog, "%ld|%s|%s|%s|%s|", (long)time(NULL), callid, queuename, agent, event);
		vfprintf(qlog, fmt, ap);
		fprintf(qlog, "\n");
		va_end(ap);
		fflush(qlog);
	}
	ast_mutex_unlock(&loglock);
}

int reload_logger(int rotate)
{
	char old[PATH_MAX] = "";
	char new[PATH_MAX];
	int event_rotate = rotate, queue_rotate = rotate;
	struct logchannel *f;
	FILE *myf;
	int x, res = 0;

	ast_mutex_lock(&msglist_lock);	/* to avoid deadlock */
	ast_mutex_lock(&loglock);
	if (eventlog) 
		fclose(eventlog);
	else 
		event_rotate = 0;
	eventlog = NULL;

	if (qlog) 
		fclose(qlog);
	else 
		queue_rotate = 0;
	qlog = NULL;

	mkdir((char *)ast_config_AST_LOG_DIR, 0755);

	f = logchannels;
	while(f) {
		if (f->disabled) {
			f->disabled = 0;	/* Re-enable logging at reload */
			manager_event(EVENT_FLAG_SYSTEM, "LogChannel", "Channel: %s\r\nEnabled: Yes\r\n", f->filename);
		}
		if (f->fileptr && (f->fileptr != stdout) && (f->fileptr != stderr)) {
			fclose(f->fileptr);	/* Close file */
			f->fileptr = NULL;
			if(rotate) {
				ast_copy_string(old, f->filename, sizeof(old));
	
				for(x=0;;x++) {
					snprintf(new, sizeof(new), "%s.%d", f->filename, x);
					myf = fopen((char *)new, "r");
					if (myf) {
						fclose(myf);
					} else {
						break;
					}
				}
	    
				/* do it */
				if (rename(old,new))
					fprintf(stderr, "Unable to rename file '%s' to '%s'\n", old, new);
			}
		}
		f = f->next;
	}

	filesize_reload_needed = 0;
	
	init_logger_chain();

	if (logfiles.event_log) {
		snprintf(old, sizeof(old), "%s/%s", (char *)ast_config_AST_LOG_DIR, EVENTLOG);
		if (event_rotate) {
			for (x=0;;x++) {
				snprintf(new, sizeof(new), "%s/%s.%d", (char *)ast_config_AST_LOG_DIR, EVENTLOG,x);
				myf = fopen((char *)new, "r");
				if (myf) 	/* File exists */
					fclose(myf);
				else
					break;
			}
	
			/* do it */
			if (rename(old,new))
				ast_log(LOG_ERROR, "Unable to rename file '%s' to '%s'\n", old, new);
		}

		eventlog = fopen(old, "a");
		if (eventlog) {
			ast_log(LOG_EVENT, "Restarted Asterisk Event Logger\n");
			if (option_verbose)
				ast_verbose("Asterisk Event Logger restarted\n");
		} else {
			ast_log(LOG_ERROR, "Unable to create event log: %s\n", strerror(errno));
			res = -1;
		}
	}

	if (logfiles.queue_log) {
		snprintf(old, sizeof(old), "%s/%s", (char *)ast_config_AST_LOG_DIR, QUEUELOG);
		if (queue_rotate) {
			for (x = 0; ; x++) {
				snprintf(new, sizeof(new), "%s/%s.%d", (char *)ast_config_AST_LOG_DIR, QUEUELOG, x);
				myf = fopen((char *)new, "r");
				if (myf) 	/* File exists */
					fclose(myf);
				else
					break;
			}
	
			/* do it */
			if (rename(old, new))
				ast_log(LOG_ERROR, "Unable to rename file '%s' to '%s'\n", old, new);
		}

		qlog = fopen(old, "a");
		if (qlog) {
			ast_queue_log("NONE", "NONE", "NONE", "CONFIGRELOAD", "%s", "");
			ast_log(LOG_EVENT, "Restarted Asterisk Queue Logger\n");
			if (option_verbose)
				ast_verbose("Asterisk Queue Logger restarted\n");
		} else {
			ast_log(LOG_ERROR, "Unable to create queue log: %s\n", strerror(errno));
			res = -1;
		}
	}
	ast_mutex_unlock(&loglock);
	ast_mutex_unlock(&msglist_lock);

	return res;
}

static int handle_logger_reload(int fd, int argc, char *argv[])
{
	if(reload_logger(0)) {
		ast_cli(fd, "Failed to reload the logger\n");
		return RESULT_FAILURE;
	} else
		return RESULT_SUCCESS;
}

static int handle_logger_rotate(int fd, int argc, char *argv[])
{
	if(reload_logger(1)) {
		ast_cli(fd, "Failed to reload the logger and rotate log files\n");
		return RESULT_FAILURE;
	} else
		return RESULT_SUCCESS;
}

/*! \brief CLI command to show logging system configuration */
static int handle_logger_show_channels(int fd, int argc, char *argv[])
{
#define FORMATL	"%-35.35s %-8.8s %-9.9s "
	struct logchannel *chan;

	ast_mutex_lock(&loglock);

	chan = logchannels;
	ast_cli(fd,FORMATL, "Channel", "Type", "Status");
	ast_cli(fd, "Configuration\n");
	ast_cli(fd,FORMATL, "-------", "----", "------");
	ast_cli(fd, "-------------\n");
	while (chan) {
		ast_cli(fd, FORMATL, chan->filename, chan->type==LOGTYPE_CONSOLE ? "Console" : (chan->type==LOGTYPE_SYSLOG ? "Syslog" : "File"),
			chan->disabled ? "Disabled" : "Enabled");
		ast_cli(fd, " - ");
		if (chan->logmask & (1 << __LOG_DEBUG)) 
			ast_cli(fd, "Debug ");
		if (chan->logmask & (1 << __LOG_DTMF)) 
			ast_cli(fd, "DTMF ");
		if (chan->logmask & (1 << __LOG_VERBOSE)) 
			ast_cli(fd, "Verbose ");
		if (chan->logmask & (1 << __LOG_WARNING)) 
			ast_cli(fd, "Warning ");
		if (chan->logmask & (1 << __LOG_NOTICE)) 
			ast_cli(fd, "Notice ");
		if (chan->logmask & (1 << __LOG_ERROR)) 
			ast_cli(fd, "Error ");
		if (chan->logmask & (1 << __LOG_EVENT)) 
			ast_cli(fd, "Event ");
		ast_cli(fd, "\n");
		chan = chan->next;
	}
	ast_cli(fd, "\n");

	ast_mutex_unlock(&loglock);
 		
	return RESULT_SUCCESS;
}

static struct verb {
	void (*verboser)(const char *string, int opos, int replacelast, int complete);
	struct verb *next;
} *verboser = NULL;


static char logger_reload_help[] =
"Usage: logger reload\n"
"       Reloads the logger subsystem state.  Use after restarting syslogd(8) if you are using syslog logging.\n";

static char logger_rotate_help[] =
"Usage: logger rotate\n"
"       Rotates and Reopens the log files.\n";

static char logger_show_channels_help[] =
"Usage: logger show channels\n"
"       Show configured logger channels.\n";

static struct ast_cli_entry logger_show_channels_cli = 
	{ { "logger", "show", "channels", NULL }, 
	handle_logger_show_channels, "List configured log channels",
	logger_show_channels_help };

static struct ast_cli_entry reload_logger_cli = 
	{ { "logger", "reload", NULL }, 
	handle_logger_reload, "Reopens the log files",
	logger_reload_help };

static struct ast_cli_entry rotate_logger_cli = 
	{ { "logger", "rotate", NULL }, 
	handle_logger_rotate, "Rotates and reopens the log files",
	logger_rotate_help };

static int handle_SIGXFSZ(int sig) 
{
	/* Indicate need to reload */
	filesize_reload_needed = 1;
	return 0;
}

int init_logger(void)
{
	char tmp[256];
	int res = 0;

	/* auto rotate if sig SIGXFSZ comes a-knockin */
	(void) signal(SIGXFSZ,(void *) handle_SIGXFSZ);

	/* register the relaod logger cli command */
	ast_cli_register(&reload_logger_cli);
	ast_cli_register(&rotate_logger_cli);
	ast_cli_register(&logger_show_channels_cli);

	mkdir((char *)ast_config_AST_LOG_DIR, 0755);
  
	/* create log channels */
	init_logger_chain();

	/* create the eventlog */
	if (logfiles.event_log) {
		mkdir((char *)ast_config_AST_LOG_DIR, 0755);
		snprintf(tmp, sizeof(tmp), "%s/%s", (char *)ast_config_AST_LOG_DIR, EVENTLOG);
		eventlog = fopen((char *)tmp, "a");
		if (eventlog) {
			ast_log(LOG_EVENT, "Started Asterisk Event Logger\n");
			if (option_verbose)
				ast_verbose("Asterisk Event Logger Started %s\n",(char *)tmp);
		} else {
			ast_log(LOG_ERROR, "Unable to create event log: %s\n", strerror(errno));
			res = -1;
		}
	}

	if (logfiles.queue_log) {
		snprintf(tmp, sizeof(tmp), "%s/%s", (char *)ast_config_AST_LOG_DIR, QUEUELOG);
		qlog = fopen(tmp, "a");
		ast_queue_log("NONE", "NONE", "NONE", "QUEUESTART", "%s", "");
	}
	return res;
}

void close_logger(void)
{
	struct msglist *m, *tmp;

	ast_mutex_lock(&msglist_lock);
	m = list;
	while(m) {
		if (m->msg) {
			free(m->msg);
		}
		tmp = m->next;
		free(m);
		m = tmp;
	}
	list = last = NULL;
	msgcnt = 0;
	ast_mutex_unlock(&msglist_lock);
	return;
}

static void ast_log_vsyslog(int level, const char *file, int line, const char *function, const char *fmt, va_list args) 
{
	char buf[BUFSIZ];
	char *s;

	if (level >= SYSLOG_NLEVELS) {
		/* we are locked here, so cannot ast_log() */
		fprintf(stderr, "ast_log_vsyslog called with bogus level: %d\n", level);
		return;
	}
	if (level == __LOG_VERBOSE) {
		snprintf(buf, sizeof(buf), "VERBOSE[%ld]: ", (long)GETTID());
		level = __LOG_DEBUG;
	} else if (level == __LOG_DTMF) {
		snprintf(buf, sizeof(buf), "DTMF[%ld]: ", (long)GETTID());
		level = __LOG_DEBUG;
	} else {
		snprintf(buf, sizeof(buf), "%s[%ld]: %s:%d in %s: ",
			 levels[level], (long)GETTID(), file, line, function);
	}
	s = buf + strlen(buf);
	vsnprintf(s, sizeof(buf) - strlen(buf), fmt, args);
	term_strip(s, s, strlen(s) + 1);
	syslog(syslog_level_map[level], "%s", buf);
}

/*
 * send log messages to syslog and/or the console
 */
void ast_log(int level, const char *file, int line, const char *function, const char *fmt, ...)
{
	struct logchannel *chan;
	char buf[BUFSIZ];
	time_t t;
	struct tm tm;
	char date[256];

	va_list ap;
	
	/* don't display LOG_DEBUG messages unless option_verbose _or_ option_debug
	   are non-zero; LOG_DEBUG messages can still be displayed if option_debug
	   is zero, if option_verbose is non-zero (this allows for 'level zero'
	   LOG_DEBUG messages to be displayed, if the logmask on any channel
	   allows it)
	*/
	if (!option_verbose && !option_debug && (level == __LOG_DEBUG)) {
		return;
	}

	/* Ignore anything that never gets logged anywhere */
	if (!(global_logmask & (1 << level)))
		return;
	
	/* Ignore anything other than the currently debugged file if there is one */
	if ((level == __LOG_DEBUG) && !ast_strlen_zero(debug_filename) && strcasecmp(debug_filename, file))
		return;

	/* begin critical section */
	ast_mutex_lock(&loglock);

	time(&t);
	localtime_r(&t, &tm);
	strftime(date, sizeof(date), dateformat, &tm);

	if (logfiles.event_log && level == __LOG_EVENT) {
		va_start(ap, fmt);

		fprintf(eventlog, "%s asterisk[%ld]: ", date, (long)getpid());
		vfprintf(eventlog, fmt, ap);
		fflush(eventlog);

		va_end(ap);
		ast_mutex_unlock(&loglock);
		return;
	}

	if (logchannels) {
		chan = logchannels;
		while(chan && !chan->disabled) {
			/* Check syslog channels */
			if (chan->type == LOGTYPE_SYSLOG && (chan->logmask & (1 << level))) {
				va_start(ap, fmt);
				ast_log_vsyslog(level, file, line, function, fmt, ap);
				va_end(ap);
			/* Console channels */
			} else if ((chan->logmask & (1 << level)) && (chan->type == LOGTYPE_CONSOLE)) {
				char linestr[128];
				char tmp1[80], tmp2[80], tmp3[80], tmp4[80];

				if (level != __LOG_VERBOSE) {
					sprintf(linestr, "%d", line);
					snprintf(buf, sizeof(buf), ast_opt_timestamp ? "[%s] %s[%ld]: %s:%s %s: " : "%s %s[%ld]: %s:%s %s: ",
						date,
						term_color(tmp1, levels[level], colors[level], 0, sizeof(tmp1)),
						(long)GETTID(),
						term_color(tmp2, file, COLOR_BRWHITE, 0, sizeof(tmp2)),
						term_color(tmp3, linestr, COLOR_BRWHITE, 0, sizeof(tmp3)),
						term_color(tmp4, function, COLOR_BRWHITE, 0, sizeof(tmp4)));
					
					ast_console_puts_mutable(buf);
					va_start(ap, fmt);
					vsnprintf(buf, sizeof(buf), fmt, ap);
					va_end(ap);
					ast_console_puts_mutable(buf);
				}
			/* File channels */
			} else if ((chan->logmask & (1 << level)) && (chan->fileptr)) {
				int res;
				snprintf(buf, sizeof(buf), ast_opt_timestamp ? "[%s] %s[%ld]: " : "%s %s[%ld] %s: ", date,
					levels[level], (long)GETTID(), file);
				res = fprintf(chan->fileptr, buf);
				if (res <= 0 && buf[0] != '\0') {	/* Error, no characters printed */
					fprintf(stderr,"**** Asterisk Logging Error: ***********\n");
					if (errno == ENOMEM || errno == ENOSPC) {
						fprintf(stderr, "Asterisk logging error: Out of disk space, can't log to log file %s\n", chan->filename);
					} else
						fprintf(stderr, "Logger Warning: Unable to write to log file '%s': %s (disabled)\n", chan->filename, strerror(errno));
					manager_event(EVENT_FLAG_SYSTEM, "LogChannel", "Channel: %s\r\nEnabled: No\r\nReason: %d - %s\r\n", chan->filename, errno, strerror(errno));
					chan->disabled = 1;	
				} else {
					/* No error message, continue printing */
					va_start(ap, fmt);
					vsnprintf(buf, sizeof(buf), fmt, ap);
					va_end(ap);
					term_strip(buf, buf, sizeof(buf));
					fputs(buf, chan->fileptr);
					fflush(chan->fileptr);
				}
			}
			chan = chan->next;
		}
	} else {
		/* 
		 * we don't have the logger chain configured yet,
		 * so just log to stdout 
		*/
		if (level != __LOG_VERBOSE) {
			va_start(ap, fmt);
			vsnprintf(buf, sizeof(buf), fmt, ap);
			va_end(ap);
			fputs(buf, stdout);
		}
	}

	ast_mutex_unlock(&loglock);
	/* end critical section */
	if (filesize_reload_needed) {
		reload_logger(1);
		ast_log(LOG_EVENT,"Rotated Logs Per SIGXFSZ (Exceeded file size limit)\n");
		if (option_verbose)
			ast_verbose("Rotated Logs Per SIGXFSZ (Exceeded file size limit)\n");
	}
}

void ast_backtrace(void)
{
#ifdef STACK_BACKTRACES
	int count=0, i=0;
	void **addresses;
	char **strings;

	if ((addresses = ast_calloc(MAX_BACKTRACE_FRAMES, sizeof(*addresses)))) {
		count = backtrace(addresses, MAX_BACKTRACE_FRAMES);
		if ((strings = backtrace_symbols(addresses, count))) {
			ast_log(LOG_DEBUG, "Got %d backtrace record%c\n", count, count != 1 ? 's' : ' ');
			for (i=0; i < count ; i++) {
				ast_log(LOG_DEBUG, "#%d: [%08X] %s\n", i, (unsigned int)addresses[i], strings[i]);
			}
			free(strings);
		} else {
			ast_log(LOG_DEBUG, "Could not allocate memory for backtrace\n");
		}
		free(addresses);
	}
#else
#ifdef Linux
	ast_log(LOG_WARNING, "Must compile with 'make dont-optimize' for stack backtraces\n");
#else
	ast_log(LOG_WARNING, "Inline stack backtraces are only available on the Linux platform.\n");
#endif
#endif
}

void ast_verbose(const char *fmt, ...)
{
	static char stuff[4096];
	static int len = 0;
	static int replacelast = 0;

	int complete;
	int olen;
	struct msglist *m;
	struct verb *v;
	
	va_list ap;
	va_start(ap, fmt);

	if (ast_opt_timestamp) {
		time_t t;
		struct tm tm;
		char date[40];
		char *datefmt;

		time(&t);
		localtime_r(&t, &tm);
		strftime(date, sizeof(date), dateformat, &tm);
		datefmt = alloca(strlen(date) + 3 + strlen(fmt) + 1);
	}

	/* this lock is also protecting against multiple threads
	   being in this function at the same time, so it must be
	   held before any of the static variables are accessed
	*/
	ast_mutex_lock(&msglist_lock);

	/* there is a potential security problem here: if formatting
	   the current date using 'dateformat' results in a string
	   containing '%', then the vsnprintf() call below will
	   probably try to access random memory
	*/
	vsnprintf(stuff + len, sizeof(stuff) - len, fmt, ap);
	va_end(ap);

	olen = len;
	len = strlen(stuff);

	complete = (stuff[len - 1] == '\n') ? 1 : 0;

	/* If we filled up the stuff completely, then log it even without the '\n' */
	if (len >= sizeof(stuff) - 1) {
		complete = 1;
		len = 0;
	}

	if (complete) {
		if (msgcnt < MAX_MSG_QUEUE) {
			/* Allocate new structure */
			if ((m = ast_malloc(sizeof(*m))))
				msgcnt++;
		} else {
			/* Recycle the oldest entry */
			m = list;
			list = list->next;
			free(m->msg);
		}
		if (m) {
			if ((m->msg = ast_strdup(stuff))) {
				if (last)
					last->next = m;
				else
					list = m;
				m->next = NULL;
				last = m;
			} else {
				msgcnt--;
				free(m);
			}
		}
	}

	for (v = verboser; v; v = v->next)
		v->verboser(stuff, olen, replacelast, complete);

	ast_log(LOG_VERBOSE, "%s", stuff);

	if (len) {
		if (!complete)
			replacelast = 1;
		else 
			replacelast = len = 0;
	}

	ast_mutex_unlock(&msglist_lock);
}

int ast_verbose_dmesg(void (*v)(const char *string, int opos, int replacelast, int complete))
{
	struct msglist *m;
	ast_mutex_lock(&msglist_lock);
	m = list;
	while(m) {
		/* Send all the existing entries that we have queued (i.e. they're likely to have missed) */
		v(m->msg, 0, 0, 1);
		m = m->next;
	}
	ast_mutex_unlock(&msglist_lock);
	return 0;
}

int ast_register_verbose(void (*v)(const char *string, int opos, int replacelast, int complete)) 
{
	struct msglist *m;
	struct verb *tmp;
	/* XXX Should be more flexible here, taking > 1 verboser XXX */
	if ((tmp = ast_malloc(sizeof(*tmp)))) {
		tmp->verboser = v;
		ast_mutex_lock(&msglist_lock);
		tmp->next = verboser;
		verboser = tmp;
		m = list;
		while(m) {
			/* Send all the existing entries that we have queued (i.e. they're likely to have missed) */
			v(m->msg, 0, 0, 1);
			m = m->next;
		}
		ast_mutex_unlock(&msglist_lock);
		return 0;
	}
	return -1;
}

int ast_unregister_verbose(void (*v)(const char *string, int opos, int replacelast, int complete))
{
	int res = -1;
	struct verb *tmp, *tmpl=NULL;
	ast_mutex_lock(&msglist_lock);
	tmp = verboser;
	while(tmp) {
		if (tmp->verboser == v)	{
			if (tmpl)
				tmpl->next = tmp->next;
			else
				verboser = tmp->next;
			free(tmp);
			break;
		}
		tmpl = tmp;
		tmp = tmp->next;
	}
	if (tmp)
		res = 0;
	ast_mutex_unlock(&msglist_lock);
	return res;
}
