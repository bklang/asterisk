/*
 * Asterisk -- A telephony toolkit for Linux.
 *
 * Top level source file for asterisk
 * 
 * Copyright (C) 1999, Mark Spencer
 *
 * Mark Spencer <markster@linux-support.net>
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License
 */

#include <unistd.h>
#include <stdlib.h>
#include <asterisk/logger.h>
#include <asterisk/options.h>
#include <asterisk/cli.h>
#include <asterisk/channel.h>
#include <asterisk/ulaw.h>
#include <asterisk/alaw.h>
#include <asterisk/callerid.h>
#include <asterisk/module.h>
#include <asterisk/image.h>
#include <asterisk/tdd.h>
#include <asterisk/term.h>
#include <asterisk/manager.h>
#include <asterisk/pbx.h>
#include <asterisk/enum.h>
#include <asterisk/rtp.h>
#include <sys/resource.h>
#include <fcntl.h>
#include <stdio.h>
#include <signal.h>
#include <sched.h>
#include <asterisk/io.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>
#include "editline/histedit.h"
#include "asterisk.h"
#include <asterisk/config.h>

#define AST_MAX_CONNECTS 128
#define NUM_MSGS 64

int option_verbose=0;
int option_debug=0;
int option_nofork=0;
int option_quiet=0;
int option_console=0;
int option_highpriority=0;
int option_remote=0;
int option_exec=0;
int option_initcrypto=0;
int option_nocolor;
int option_dumpcore = 0;
int option_overrideconfig = 0;
int fully_booted = 0;

static int ast_socket = -1;		/* UNIX Socket for allowing remote control */
static int ast_consock = -1;		/* UNIX Socket for controlling another asterisk */
static int mainpid;
struct console {
	int fd;					/* File descriptor */
	int p[2];				/* Pipe */
	pthread_t t;			/* Thread of handler */
};

static struct ast_atexit {
	void (*func)(void);
	struct ast_atexit *next;
} *atexits = NULL;
static ast_mutex_t atexitslock = AST_MUTEX_INITIALIZER;

time_t ast_startuptime;
time_t ast_lastreloadtime;

static History *el_hist = NULL;
static EditLine *el = NULL;
static char *remotehostname;

struct console consoles[AST_MAX_CONNECTS];

char defaultlanguage[MAX_LANGUAGE] = DEFAULT_LANGUAGE;

static int ast_el_add_history(char *);
static int ast_el_read_history(char *);
static int ast_el_write_history(char *);

char ast_config_AST_CONFIG_DIR[AST_CONFIG_MAX_PATH];
char ast_config_AST_CONFIG_FILE[AST_CONFIG_MAX_PATH];
char ast_config_AST_MODULE_DIR[AST_CONFIG_MAX_PATH];
char ast_config_AST_SPOOL_DIR[AST_CONFIG_MAX_PATH];
char ast_config_AST_VAR_DIR[AST_CONFIG_MAX_PATH];
char ast_config_AST_LOG_DIR[AST_CONFIG_MAX_PATH];
char ast_config_AST_AGI_DIR[AST_CONFIG_MAX_PATH];
char ast_config_AST_DB[AST_CONFIG_MAX_PATH];
char ast_config_AST_KEY_DIR[AST_CONFIG_MAX_PATH];
char ast_config_AST_PID[AST_CONFIG_MAX_PATH];
char ast_config_AST_SOCKET[AST_CONFIG_MAX_PATH];
char ast_config_AST_RUN_DIR[AST_CONFIG_MAX_PATH];

int ast_register_atexit(void (*func)(void))
{
	int res = -1;
	struct ast_atexit *ae;
	ast_unregister_atexit(func);
	ae = malloc(sizeof(struct ast_atexit));
	ast_mutex_lock(&atexitslock);
	if (ae) {
		memset(ae, 0, sizeof(struct ast_atexit));
		ae->next = atexits;
		ae->func = func;
		atexits = ae;
		res = 0;
	}
	ast_mutex_unlock(&atexitslock);
	return res;
}

void ast_unregister_atexit(void (*func)(void))
{
	struct ast_atexit *ae, *prev = NULL;
	ast_mutex_lock(&atexitslock);
	ae = atexits;
	while(ae) {
		if (ae->func == func) {
			if (prev)
				prev->next = ae->next;
			else
				atexits = ae->next;
			break;
		}
		prev = ae;
		ae = ae->next;
	}
	ast_mutex_unlock(&atexitslock);
}

static int fdprint(int fd, const char *s)
{
	return write(fd, s, strlen(s) + 1);
}

/*
 * write the string to all attached console clients
 */
static void ast_network_puts(const char *string)
{
    int x;
    for (x=0;x<AST_MAX_CONNECTS; x++) {
	if (consoles[x].fd > -1) 
	    fdprint(consoles[x].p[1], string);
    }
}


/*
 * write the string to the console, and all attached
 * console clients
 */
void ast_console_puts(const char *string)
{
    fputs(string, stdout);
    fflush(stdout);
    ast_network_puts(string);
}

static void network_verboser(const char *s, int pos, int replace, int complete)
     /* ARGUSED */
{
    ast_network_puts(s);
}

static pthread_t lthread;

static void *netconsole(void *vconsole)
{
	struct console *con = vconsole;
	char hostname[256];
	char tmp[512];
	int res;
	int max;
	fd_set rfds;
	
	if (gethostname(hostname, sizeof(hostname)))
		strncpy(hostname, "<Unknown>", sizeof(hostname)-1);
	snprintf(tmp, sizeof(tmp), "%s/%d/%s\n", hostname, mainpid, ASTERISK_VERSION);
	fdprint(con->fd, tmp);
	for(;;) {
		FD_ZERO(&rfds);	
		FD_SET(con->fd, &rfds);
		FD_SET(con->p[0], &rfds);
		max = con->fd;
		if (con->p[0] > max)
			max = con->p[0];
		res = ast_select(max + 1, &rfds, NULL, NULL, NULL);
		if (res < 0) {
			ast_log(LOG_WARNING, "select returned < 0: %s\n", strerror(errno));
			continue;
		}
		if (FD_ISSET(con->fd, &rfds)) {
			res = read(con->fd, tmp, sizeof(tmp));
			if (res < 1) {
				break;
			}
			tmp[res] = 0;
			ast_cli_command(con->fd, tmp);
		}
		if (FD_ISSET(con->p[0], &rfds)) {
			res = read(con->p[0], tmp, sizeof(tmp));
			if (res < 1) {
				ast_log(LOG_ERROR, "read returned %d\n", res);
				break;
			}
			res = write(con->fd, tmp, res);
			if (res < 1)
				break;
		}
	}
	if (option_verbose > 2) 
		ast_verbose(VERBOSE_PREFIX_3 "Remote UNIX connection disconnected\n");
	close(con->fd);
	close(con->p[0]);
	close(con->p[1]);
	con->fd = -1;
	
	return NULL;
}

static void *listener(void *unused)
{
	struct sockaddr_un sun;
	fd_set fds;
	int s;
	int len;
	int x;
	int flags;
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	for(;;) {
		if (ast_socket < 0)
			return NULL;
		FD_ZERO(&fds);
		FD_SET(ast_socket, &fds);
		s = ast_select(ast_socket + 1, &fds, NULL, NULL, NULL);
		if (s < 0) {
			ast_log(LOG_WARNING, "Select retured error: %s\n", strerror(errno));
			continue;
		}
		len = sizeof(sun);
		s = accept(ast_socket, (struct sockaddr *)&sun, &len);
		if (s < 0) {
			ast_log(LOG_WARNING, "Accept retured %d: %s\n", s, strerror(errno));
		} else {
			for (x=0;x<AST_MAX_CONNECTS;x++) {
				if (consoles[x].fd < 0) {
					if (socketpair(AF_LOCAL, SOCK_STREAM, 0, consoles[x].p)) {
						ast_log(LOG_ERROR, "Unable to create pipe: %s\n", strerror(errno));
						consoles[x].fd = -1;
						fdprint(s, "Server failed to create pipe\n");
						close(s);
						break;
					}
					flags = fcntl(consoles[x].p[1], F_GETFL);
					fcntl(consoles[x].p[1], F_SETFL, flags | O_NONBLOCK);
					consoles[x].fd = s;
					if (pthread_create(&consoles[x].t, &attr, netconsole, &consoles[x])) {
						ast_log(LOG_ERROR, "Unable to spawn thread to handle connection\n");
						consoles[x].fd = -1;
						fdprint(s, "Server failed to spawn thread\n");
						close(s);
					}
					break;
				}
			}
			if (x >= AST_MAX_CONNECTS) {
				fdprint(s, "No more connections allowed\n");
				ast_log(LOG_WARNING, "No more connections allowed\n");
				close(s);
			} else if (consoles[x].fd > -1) {
				if (option_verbose > 2) 
					ast_verbose(VERBOSE_PREFIX_3 "Remote UNIX connection\n");
			}
		}
	}
	return NULL;
}

static int ast_makesocket(void)
{
	struct sockaddr_un sun;
	int res;
	int x;
	for (x=0;x<AST_MAX_CONNECTS;x++)	
		consoles[x].fd = -1;
	unlink((char *)ast_config_AST_SOCKET);
	ast_socket = socket(PF_LOCAL, SOCK_STREAM, 0);
	if (ast_socket < 0) {
		ast_log(LOG_WARNING, "Unable to create control socket: %s\n", strerror(errno));
		return -1;
	}		
	memset(&sun, 0, sizeof(sun));
	sun.sun_family = AF_LOCAL;
	strncpy(sun.sun_path, (char *)ast_config_AST_SOCKET, sizeof(sun.sun_path)-1);
	res = bind(ast_socket, (struct sockaddr *)&sun, sizeof(sun));
	if (res) {
		ast_log(LOG_WARNING, "Unable to bind socket to %s: %s\n", (char *)ast_config_AST_SOCKET, strerror(errno));
		close(ast_socket);
		ast_socket = -1;
		return -1;
	}
	res = listen(ast_socket, 2);
	if (res < 0) {
		ast_log(LOG_WARNING, "Unable to listen on socket %s: %s\n", (char *)ast_config_AST_SOCKET, strerror(errno));
		close(ast_socket);
		ast_socket = -1;
		return -1;
	}
	ast_register_verbose(network_verboser);
	pthread_create(&lthread, NULL, listener, NULL);
	return 0;
}

static int ast_tryconnect(void)
{
	struct sockaddr_un sun;
	int res;
	ast_consock = socket(PF_LOCAL, SOCK_STREAM, 0);
	if (ast_consock < 0) {
		ast_log(LOG_WARNING, "Unable to create socket: %s\n", strerror(errno));
		return 0;
	}
	memset(&sun, 0, sizeof(sun));
	sun.sun_family = AF_LOCAL;
	strncpy(sun.sun_path, (char *)ast_config_AST_SOCKET, sizeof(sun.sun_path)-1);
	res = connect(ast_consock, (struct sockaddr *)&sun, sizeof(sun));
	if (res) {
		close(ast_consock);
		ast_consock = -1;
		return 0;
	} else
		return 1;
}

static void urg_handler(int num)
{
	/* Called by soft_hangup to interrupt the select, read, or other
	   system call.  We don't actually need to do anything though.  */
	/* Cannot EVER ast_log from within a signal handler */
	if (option_debug) 
		printf("Urgent handler\n");
	signal(num, urg_handler);
	return;
}

static void hup_handler(int num)
{
	if (option_verbose > 1) 
		printf("Received HUP signal -- Reloading configs\n");
	/* XXX This could deadlock XXX */
	ast_module_reload();
}

static void child_handler(int sig)
{
	/* Must not ever ast_log or ast_verbose within signal handler */
	int n, status;

	/*
	 * Reap all dead children -- not just one
	 */
	for (n = 0; wait4(-1, &status, WNOHANG, NULL) > 0; n++)
		;
	if (n == 0 && option_debug)	
		printf("Huh?  Child handler, but nobody there?\n");
}

static void set_title(char *text)
{
	/* Set an X-term or screen title */
	if (getenv("TERM") && strstr(getenv("TERM"), "xterm"))
		fprintf(stdout, "\033]2;%s\007", text);
}

static void set_icon(char *text)
{
	if (getenv("TERM") && strstr(getenv("TERM"), "xterm"))
		fprintf(stdout, "\033]1;%s\007", text);
}

static int set_priority(int pri)
{
	struct sched_param sched;
	memset(&sched, 0, sizeof(sched));
	/* We set ourselves to a high priority, that we might pre-empt everything
	   else.  If your PBX has heavy activity on it, this is a good thing.  */
#ifdef __linux__
	if (pri) {  
		sched.sched_priority = 10;
		if (sched_setscheduler(0, SCHED_RR, &sched)) {
			ast_log(LOG_WARNING, "Unable to set high priority\n");
			return -1;
		} else
			if (option_verbose)
				ast_verbose("Set to realtime thread\n");
	} else {
		sched.sched_priority = 0;
		if (sched_setscheduler(0, SCHED_OTHER, &sched)) {
			ast_log(LOG_WARNING, "Unable to set normal priority\n");
			return -1;
		}
	}
#else
	if (pri) {
		if (setpriority(PRIO_PROCESS, 0, -10) == -1) {
			ast_log(LOG_WARNING, "Unable to set high priority\n");
			return -1;
		} else
			if (option_verbose)
				ast_verbose("Set to high priority\n");
	} else {
		if (setpriority(PRIO_PROCESS, 0, 0) == -1) {
			ast_log(LOG_WARNING, "Unable to set normal priority\n");
			return -1;
		}
	}
#endif
	return 0;
}

static char *_argv[256];

static int shuttingdown = 0;

static void ast_run_atexits(void)
{
	struct ast_atexit *ae;
	ast_mutex_lock(&atexitslock);
	ae = atexits;
	while(ae) {
		if (ae->func) 
			ae->func();
		ae = ae->next;
	}
	ast_mutex_unlock(&atexitslock);
}

static void quit_handler(int num, int nice, int safeshutdown, int restart)
{
	char filename[80] = "";
	time_t s,e;
	int x;
	if (safeshutdown) {
		shuttingdown = 1;
		if (!nice) {
			/* Begin shutdown routine, hanging up active channels */
			ast_begin_shutdown(1);
			if (option_verbose && option_console)
				ast_verbose("Beginning asterisk %s....\n", restart ? "restart" : "shutdown");
			time(&s);
			for(;;) {
				time(&e);
				/* Wait up to 15 seconds for all channels to go away */
				if ((e - s) > 15)
					break;
				if (!ast_active_channels())
					break;
				if (!shuttingdown)
					break;
				/* Sleep 1/10 of a second */
				usleep(100000);
			}
		} else {
			if (nice < 2)
				ast_begin_shutdown(0);
			if (option_verbose && option_console)
				ast_verbose("Waiting for inactivity to perform %s...\n", restart ? "restart" : "halt");
			for(;;) {
				if (!ast_active_channels())
					break;
				if (!shuttingdown)
					break;
				sleep(1);
			}
		}

		if (!shuttingdown) {
			if (option_verbose && option_console)
				ast_verbose("Asterisk %s cancelled.\n", restart ? "restart" : "shutdown");
			return;
		}
	}
	if (option_console || option_remote) {
		if (getenv("HOME")) 
			snprintf(filename, sizeof(filename), "%s/.asterisk_history", getenv("HOME"));
		if (strlen(filename))
			ast_el_write_history(filename);
		if (el != NULL)
			el_end(el);
		if (el_hist != NULL)
			history_end(el_hist);
	}
	if (option_verbose)
		ast_verbose("Executing last minute cleanups\n");
	ast_run_atexits();
	/* Called on exit */
	if (option_verbose && option_console)
		ast_verbose("Asterisk %s ending (%d).\n", ast_active_channels() ? "uncleanly" : "cleanly", num);
	else if (option_debug)
		ast_log(LOG_DEBUG, "Asterisk ending (%d).\n", num);
	manager_event(EVENT_FLAG_SYSTEM, "Shutdown", "Shutdown: %s\r\nRestart: %s\r\n", ast_active_channels() ? "Uncleanly" : "Cleanly", restart ? "True" : "False");
	if (ast_socket > -1) {
		close(ast_socket);
		ast_socket = -1;
	}
	if (ast_consock > -1)
		close(ast_consock);
	if (ast_socket > -1)
		unlink((char *)ast_config_AST_SOCKET);
	if (!option_remote) unlink((char *)ast_config_AST_PID);
	printf(term_quit());
	if (restart) {
		if (option_verbose || option_console)
			ast_verbose("Preparing for Asterisk restart...\n");
		/* Mark all FD's for closing on exec */
		for (x=3;x<32768;x++) {
			fcntl(x, F_SETFD, FD_CLOEXEC);
		}
		if (option_verbose || option_console)
			ast_verbose("Restarting Asterisk NOW...\n");
		execvp(_argv[0], _argv);
	}
	exit(0);
}

static void __quit_handler(int num)
{
	quit_handler(num, 0, 1, 0);
}

static pthread_t consolethread = (pthread_t) -1;

static const char *fix_header(char *outbuf, int maxout, const char *s, char *cmp)
{
	const char *c;
	if (!strncmp(s, cmp, strlen(cmp))) {
		c = s + strlen(cmp);
		term_color(outbuf, cmp, COLOR_GRAY, 0, maxout);
		return c;
	}
	return NULL;
}

static void console_verboser(const char *s, int pos, int replace, int complete)
{
	char tmp[80];
	const char *c=NULL;
	/* Return to the beginning of the line */
	if (!pos) {
		fprintf(stdout, "\r");
		if ((c = fix_header(tmp, sizeof(tmp), s, VERBOSE_PREFIX_4)) ||
			(c = fix_header(tmp, sizeof(tmp), s, VERBOSE_PREFIX_3)) ||
			(c = fix_header(tmp, sizeof(tmp), s, VERBOSE_PREFIX_2)) ||
			(c = fix_header(tmp, sizeof(tmp), s, VERBOSE_PREFIX_1)))
			fputs(tmp, stdout);
	}
	if (c)
		fputs(c + pos,stdout);
	else
		fputs(s + pos,stdout);
	fflush(stdout);
	if (complete)
	/* Wake up a select()ing console */
		if (consolethread != (pthread_t) -1)
			pthread_kill(consolethread, SIGURG);
}

static void consolehandler(char *s)
{
	printf(term_end());
	fflush(stdout);
	/* Called when readline data is available */
	if (s && strlen(s))
		ast_el_add_history(s);
	/* Give the console access to the shell */
	if (s) {
		/* The real handler for bang */
		if (s[0] == '!') {
			if (s[1])
				system(s+1);
			else
				system(getenv("SHELL") ? getenv("SHELL") : "/bin/sh");
		} else 
		ast_cli_command(STDOUT_FILENO, s);
	} else
		fprintf(stdout, "\nUse \"quit\" to exit\n");
}

static int remoteconsolehandler(char *s)
{
	int ret = 0;
	/* Called when readline data is available */
	if (s && strlen(s))
		ast_el_add_history(s);
	/* Give the console access to the shell */
	if (s) {
		/* The real handler for bang */
		if (s[0] == '!') {
			if (s[1])
				system(s+1);
			else
				system(getenv("SHELL") ? getenv("SHELL") : "/bin/sh");
			ret = 1;
		}
		if ((strncasecmp(s, "quit", 4) == 0 || strncasecmp(s, "exit", 4) == 0) &&
		    (s[4] == '\0' || isspace(s[4]))) {
			quit_handler(0, 0, 0, 0);
			ret = 1;
		}
	} else
		fprintf(stdout, "\nUse \"quit\" to exit\n");

	return ret;
}

static char quit_help[] = 
"Usage: quit\n"
"       Exits Asterisk.\n";

static char abort_halt_help[] = 
"Usage: abort shutdown\n"
"       Causes Asterisk to abort an executing shutdown or restart, and resume normal\n"
"       call operations.\n";

static char shutdown_now_help[] = 
"Usage: stop now\n"
"       Shuts down a running Asterisk immediately, hanging up all active calls .\n";

static char shutdown_gracefully_help[] = 
"Usage: stop gracefully\n"
"       Causes Asterisk to not accept new calls, and exit when all\n"
"       active calls have terminated normally.\n";

static char shutdown_when_convenient_help[] = 
"Usage: stop when convenient\n"
"       Causes Asterisk to perform a shutdown when all active calls have ended.\n";

static char restart_now_help[] = 
"Usage: restart now\n"
"       Causes Asterisk to hangup all calls and exec() itself performing a cold.\n"
"       restart.\n";

static char restart_gracefully_help[] = 
"Usage: restart gracefully\n"
"       Causes Asterisk to stop accepting new calls and exec() itself performing a cold.\n"
"       restart when all active calls have ended.\n";

static char restart_when_convenient_help[] = 
"Usage: restart when convenient\n"
"       Causes Asterisk to perform a cold restart when all active calls have ended.\n";

static char bang_help[] =
"Usage: !<command>\n"
"       Executes a given shell command\n";

#if 0
static int handle_quit(int fd, int argc, char *argv[])
{
	if (argc != 1)
		return RESULT_SHOWUSAGE;
	quit_handler(0, 0, 1, 0);
	return RESULT_SUCCESS;
}
#endif

static int no_more_quit(int fd, int argc, char *argv[])
{
	if (argc != 1)
		return RESULT_SHOWUSAGE;
	ast_cli(fd, "The QUIT and EXIT commands may no longer be used to shutdown the PBX.\n"
	            "Please use STOP NOW instead, if you wish to shutdown the PBX.\n");
	return RESULT_SUCCESS;
}

static int handle_shutdown_now(int fd, int argc, char *argv[])
{
	if (argc != 2)
		return RESULT_SHOWUSAGE;
	quit_handler(0, 0 /* Not nice */, 1 /* safely */, 0 /* not restart */);
	return RESULT_SUCCESS;
}

static int handle_shutdown_gracefully(int fd, int argc, char *argv[])
{
	if (argc != 2)
		return RESULT_SHOWUSAGE;
	quit_handler(0, 1 /* nicely */, 1 /* safely */, 0 /* no restart */);
	return RESULT_SUCCESS;
}

static int handle_shutdown_when_convenient(int fd, int argc, char *argv[])
{
	if (argc != 3)
		return RESULT_SHOWUSAGE;
	quit_handler(0, 2 /* really nicely */, 1 /* safely */, 0 /* don't restart */);
	return RESULT_SUCCESS;
}

static int handle_restart_now(int fd, int argc, char *argv[])
{
	if (argc != 2)
		return RESULT_SHOWUSAGE;
	quit_handler(0, 0 /* not nicely */, 1 /* safely */, 1 /* restart */);
	return RESULT_SUCCESS;
}

static int handle_restart_gracefully(int fd, int argc, char *argv[])
{
	if (argc != 2)
		return RESULT_SHOWUSAGE;
	quit_handler(0, 1 /* nicely */, 1 /* safely */, 1 /* restart */);
	return RESULT_SUCCESS;
}

static int handle_restart_when_convenient(int fd, int argc, char *argv[])
{
	if (argc != 3)
		return RESULT_SHOWUSAGE;
	quit_handler(0, 2 /* really nicely */, 1 /* safely */, 1 /* restart */);
	return RESULT_SUCCESS;
}

static int handle_abort_halt(int fd, int argc, char *argv[])
{
	if (argc != 2)
		return RESULT_SHOWUSAGE;
	ast_cancel_shutdown();
	shuttingdown = 0;
	return RESULT_SUCCESS;
}

static int handle_bang(int fd, int argc, char *argv[])
{
	return RESULT_SUCCESS;
}

#define ASTERISK_PROMPT "*CLI> "

#define ASTERISK_PROMPT2 "%s*CLI> "

static struct ast_cli_entry aborthalt = { { "abort", "halt", NULL }, handle_abort_halt, "Cancel a running halt", abort_halt_help };

static struct ast_cli_entry quit = 	{ { "quit", NULL }, no_more_quit, "Exit Asterisk", quit_help };
static struct ast_cli_entry astexit = 	{ { "exit", NULL }, no_more_quit, "Exit Asterisk", quit_help };

static struct ast_cli_entry astshutdownnow = 	{ { "stop", "now", NULL }, handle_shutdown_now, "Shut down Asterisk immediately", shutdown_now_help };
static struct ast_cli_entry astshutdowngracefully = 	{ { "stop", "gracefully", NULL }, handle_shutdown_gracefully, "Gracefully shut down Asterisk", shutdown_gracefully_help };
static struct ast_cli_entry astshutdownwhenconvenient = 	{ { "stop", "when","convenient", NULL }, handle_shutdown_when_convenient, "Shut down Asterisk at empty call volume", shutdown_when_convenient_help };
static struct ast_cli_entry astrestartnow = 	{ { "restart", "now", NULL }, handle_restart_now, "Restart Asterisk immediately", restart_now_help };
static struct ast_cli_entry astrestartgracefully = 	{ { "restart", "gracefully", NULL }, handle_restart_gracefully, "Restart Asterisk gracefully", restart_gracefully_help };
static struct ast_cli_entry astrestartwhenconvenient= 	{ { "restart", "when", "convenient", NULL }, handle_restart_when_convenient, "Restart Asterisk at empty call volume", restart_when_convenient_help };
static struct ast_cli_entry astbang = { { "!", NULL }, handle_bang, "Execute a shell command", bang_help };

static int ast_el_read_char(EditLine *el, char *cp)
{
        int num_read=0;
	int lastpos=0;
	fd_set rfds;
	int res;
	int max;
	char buf[512];

	for (;;) {
		FD_ZERO(&rfds);
		FD_SET(ast_consock, &rfds);
		max = ast_consock;
		if (!option_exec) {
			FD_SET(STDIN_FILENO, &rfds);
			if (STDIN_FILENO > max)
				max = STDIN_FILENO;
		}
		res = ast_select(max+1, &rfds, NULL, NULL, NULL);
		if (res < 0) {
			if (errno == EINTR)
				continue;
			ast_log(LOG_ERROR, "select failed: %s\n", strerror(errno));
			break;
		}

		if (FD_ISSET(STDIN_FILENO, &rfds)) {
			num_read = read(STDIN_FILENO, cp, 1);
			if (num_read < 1) {
				break;
			} else 
				return (num_read);
		}
		if (FD_ISSET(ast_consock, &rfds)) {
			res = read(ast_consock, buf, sizeof(buf) - 1);
			/* if the remote side disappears exit */
			if (res < 1) {
				fprintf(stderr, "\nDisconnected from Asterisk server\n");
				quit_handler(0, 0, 0, 0);
			}

			buf[res] = '\0';

			if (!option_exec && !lastpos)
				write(STDOUT_FILENO, "\r", 1);
			write(STDOUT_FILENO, buf, res);
			if ((buf[res-1] == '\n') || (buf[res-2] == '\n')) {
				*cp = CC_REFRESH;
				return(1);
			} else {
				lastpos = 1;
			}
		}
	}

	*cp = '\0';
	return (0);
}

static char *cli_prompt(EditLine *el)
{
	static char prompt[80];

	if (remotehostname)
		snprintf(prompt, sizeof(prompt), ASTERISK_PROMPT2, remotehostname);
	else
		snprintf(prompt, sizeof(prompt), ASTERISK_PROMPT);

	return(prompt);	
}

static char **ast_el_strtoarr(char *buf)
{
	char **match_list = NULL, *retstr;
        size_t match_list_len;
	int matches = 0;

        match_list_len = 1;
	while ( (retstr = strsep(&buf, " ")) != NULL) {

                if (matches + 1 >= match_list_len) {
                        match_list_len <<= 1;
                        match_list = realloc(match_list, match_list_len * sizeof(char *));
		}

		match_list[matches++] = retstr;
	}

        if (!match_list)
                return (char **) NULL;

	if (matches>= match_list_len)
		match_list = realloc(match_list, (match_list_len + 1) * sizeof(char *));

	match_list[matches] = (char *) NULL;

	return match_list;
}

static int ast_el_sort_compare(const void *i1, const void *i2)
{
	char *s1, *s2;

	s1 = ((char **)i1)[0];
	s2 = ((char **)i2)[0];

	return strcasecmp(s1, s2);
}

static int ast_cli_display_match_list(char **matches, int len, int max)
{
	int i, idx, limit, count;
	int screenwidth = 0;
	int numoutput = 0, numoutputline = 0;

	screenwidth = ast_get_termcols(STDOUT_FILENO);

	/* find out how many entries can be put on one line, with two spaces between strings */
	limit = screenwidth / (max + 2);
	if (limit == 0)
		limit = 1;

	/* how many lines of output */
	count = len / limit;
	if (count * limit < len)
		count++;

	idx = 1;

	qsort(&matches[0], (size_t)(len + 1), sizeof(char *), ast_el_sort_compare);

	for (; count > 0; count--) {
		numoutputline = 0;
		for (i=0; i < limit && matches[idx]; i++, idx++) {

			/* Don't print dupes */
			if ( (matches[idx+1] != NULL && strcmp(matches[idx], matches[idx+1]) == 0 ) ) {
				i--;
				continue;
			}

			numoutput++;  numoutputline++;
			fprintf(stdout, "%-*s  ", max, matches[idx]);
		}
		if (numoutputline > 0)
			fprintf(stdout, "\n");
	}

	return numoutput;
}


static char *cli_complete(EditLine *el, int ch)
{
	int len=0;
	char *ptr;
	int nummatches = 0;
	char **matches;
	int retval = CC_ERROR;
	char buf[1024];
	int res;

	LineInfo *lf = (LineInfo *)el_line(el);

	*(char *)lf->cursor = '\0';
	ptr = (char *)lf->cursor;
	if (ptr) {
		while (ptr > lf->buffer) {
			if (isspace(*ptr)) {
				ptr++;
				break;
			}
			ptr--;
		}
	}

	len = lf->cursor - ptr;

	if (option_remote) {
		snprintf(buf, sizeof(buf),"_COMMAND NUMMATCHES \"%s\" \"%s\"", lf->buffer, ptr); 
		fdprint(ast_consock, buf);
		res = read(ast_consock, buf, sizeof(buf));
		buf[res] = '\0';
		nummatches = atoi(buf);

		if (nummatches > 0) {
			snprintf(buf, sizeof(buf),"_COMMAND MATCHESARRAY \"%s\" \"%s\"", lf->buffer, ptr); 
			fdprint(ast_consock, buf);
			res = read(ast_consock, buf, sizeof(buf));
			buf[res] = '\0';

			matches = ast_el_strtoarr(buf);
		} else
			matches = (char **) NULL;


	}  else {

		nummatches = ast_cli_generatornummatches((char *)lf->buffer,ptr);
		matches = ast_cli_completion_matches((char *)lf->buffer,ptr);
	}

	if (matches) {
		int i;
		int matches_num, maxlen, match_len;

		if (matches[0][0] != '\0') {
			el_deletestr(el, (int) len);
			el_insertstr(el, matches[0]);
			retval = CC_REFRESH;
		}

		if (nummatches == 1) {
			/* Found an exact match */
			el_insertstr(el, " ");
			retval = CC_REFRESH;
		} else {
			/* Must be more than one match */
			for (i=1, maxlen=0; matches[i]; i++) {
				match_len = strlen(matches[i]);
				if (match_len > maxlen)
					maxlen = match_len;
			}
			matches_num = i - 1;
			if (matches_num >1) {
				fprintf(stdout, "\n");
				ast_cli_display_match_list(matches, nummatches, maxlen);
				retval = CC_REDISPLAY;
			} else { 
				el_insertstr(el," ");
				retval = CC_REFRESH;
			}
		}
	free(matches);
	}

	return (char *)retval;
}

static int ast_el_initialize(void)
{
	HistEvent ev;

	if (el != NULL)
		el_end(el);
	if (el_hist != NULL)
		history_end(el_hist);

	el = el_init("asterisk", stdin, stdout, stderr);
	el_set(el, EL_PROMPT, cli_prompt);

	el_set(el, EL_EDITMODE, 1);		
	el_set(el, EL_EDITOR, "emacs");		
	el_hist = history_init();
	if (!el || !el_hist)
		return -1;

	/* setup history with 100 entries */
	history(el_hist, &ev, H_SETSIZE, 100);

	el_set(el, EL_HIST, history, el_hist);

	el_set(el, EL_ADDFN, "ed-complete", "Complete argument", cli_complete);
	/* Bind <tab> to command completion */
	el_set(el, EL_BIND, "^I", "ed-complete", NULL);
	/* Bind ? to command completion */
	el_set(el, EL_BIND, "?", "ed-complete", NULL);

	return 0;
}

static int ast_el_add_history(char *buf)
{
	HistEvent ev;

	if (el_hist == NULL || el == NULL)
		ast_el_initialize();

	return (history(el_hist, &ev, H_ENTER, buf));
}

static int ast_el_write_history(char *filename)
{
	HistEvent ev;

	if (el_hist == NULL || el == NULL)
		ast_el_initialize();

	return (history(el_hist, &ev, H_SAVE, filename));
}

static int ast_el_read_history(char *filename)
{
	char buf[256];
	FILE *f;
	int ret = -1;

	if (el_hist == NULL || el == NULL)
		ast_el_initialize();

	if ((f = fopen(filename, "r")) == NULL)
		return ret;

	while (!feof(f)) {
		fgets(buf, sizeof(buf), f);
		if (!strcmp(buf, "_HiStOrY_V2_\n"))
			continue;
		if ((ret = ast_el_add_history(buf)) == -1)
			break;
	}
	fclose(f);

	return ret;
}

static void ast_remotecontrol(char * data)
{
	char buf[80];
	int res;
	char filename[80] = "";
	char *hostname;
	char *cpid;
	char *version;
	int pid;
	char tmp[80];
	char *stringp=NULL;

	char *ebuf;
	int num = 0;

	read(ast_consock, buf, sizeof(buf));
	if (data)
		write(ast_consock, data, strlen(data) + 1);
	stringp=buf;
	hostname = strsep(&stringp, "/");
	cpid = strsep(&stringp, "/");
	version = strsep(&stringp, "\n");
	if (!version)
		version = "<Version Unknown>";
	stringp=hostname;
	strsep(&stringp, ".");
	if (cpid)
		pid = atoi(cpid);
	else
		pid = -1;
	snprintf(tmp, sizeof(tmp), "set verbose atleast %d", option_verbose);
	fdprint(ast_consock, tmp);
	ast_verbose("Connected to Asterisk %s currently running on %s (pid = %d)\n", version, hostname, pid);
	remotehostname = hostname;
	if (getenv("HOME")) 
		snprintf(filename, sizeof(filename), "%s/.asterisk_history", getenv("HOME"));
	if (el_hist == NULL || el == NULL)
		ast_el_initialize();

	el_set(el, EL_GETCFN, ast_el_read_char);

	if (strlen(filename))
		ast_el_read_history(filename);

	ast_cli_register(&quit);
	ast_cli_register(&astexit);
#if 0
	ast_cli_register(&astshutdown);
#endif	
	if (option_exec && data) {  /* hack to print output then exit if asterisk -rx is used */
		char tempchar;
		ast_el_read_char(el, &tempchar);
		return;
	}
	for(;;) {
		ebuf = (char *)el_gets(el, &num);

		if (ebuf && strlen(ebuf)) {
			if (ebuf[strlen(ebuf)-1] == '\n')
				ebuf[strlen(ebuf)-1] = '\0';
			if (!remoteconsolehandler(ebuf)) {
				res = write(ast_consock, ebuf, strlen(ebuf) + 1);
				if (res < 1) {
					ast_log(LOG_WARNING, "Unable to write: %s\n", strerror(errno));
					break;
				}
			}
		}
	}
	printf("\nDisconnected from Asterisk server\n");
}

static int show_cli_help(void) {
	printf("Asterisk " ASTERISK_VERSION ", Copyright (C) 2000-2002, Digium.\n");
	printf("Usage: asterisk [OPTIONS]\n");
	printf("Valid Options:\n");
	printf("   -h           This help screen\n");
	printf("   -r           Connect to Asterisk on this machine\n");
	printf("   -f           Do not fork\n");
	printf("   -n           Disable console colorization\n");
	printf("   -p           Run as pseudo-realtime thread\n");
	printf("   -v           Increase verbosity (multiple v's = more verbose)\n");
	printf("   -q           Quiet mode (supress output)\n");
	printf("   -g           Dump core in case of a crash\n");
	printf("   -x <cmd>     Execute command <cmd> (only valid with -r)\n");
	printf("   -i           Initializie crypto keys at startup\n");
	printf("   -c           Provide console CLI\n");
	printf("   -d           Enable extra debugging\n");
	printf("\n");
	return 0;
}

static void ast_readconfig(void) {
	struct ast_config *cfg;
	struct ast_variable *v;
	char *config = ASTCONFPATH;

	if (option_overrideconfig == 1) {
	    cfg = ast_load((char *)ast_config_AST_CONFIG_FILE);
	} else {
	    cfg = ast_load(config);
	}

	/* init with buildtime config */
	strncpy((char *)ast_config_AST_CONFIG_DIR,AST_CONFIG_DIR,sizeof(ast_config_AST_CONFIG_DIR)-1);
	strncpy((char *)ast_config_AST_SPOOL_DIR,AST_SPOOL_DIR,sizeof(ast_config_AST_SPOOL_DIR)-1);
	strncpy((char *)ast_config_AST_MODULE_DIR,AST_MODULE_DIR,sizeof(ast_config_AST_VAR_DIR)-1);
	strncpy((char *)ast_config_AST_VAR_DIR,AST_VAR_DIR,sizeof(ast_config_AST_VAR_DIR)-1);
	strncpy((char *)ast_config_AST_LOG_DIR,AST_LOG_DIR,sizeof(ast_config_AST_LOG_DIR)-1);
	strncpy((char *)ast_config_AST_AGI_DIR,AST_AGI_DIR,sizeof(ast_config_AST_AGI_DIR)-1);
	strncpy((char *)ast_config_AST_DB,AST_DB,sizeof(ast_config_AST_DB)-1);
	strncpy((char *)ast_config_AST_KEY_DIR,AST_KEY_DIR,sizeof(ast_config_AST_KEY_DIR)-1);
	strncpy((char *)ast_config_AST_PID,AST_PID,sizeof(ast_config_AST_PID)-1);
	strncpy((char *)ast_config_AST_SOCKET,AST_SOCKET,sizeof(ast_config_AST_SOCKET)-1);
	strncpy((char *)ast_config_AST_RUN_DIR,AST_RUN_DIR,sizeof(ast_config_AST_RUN_DIR)-1);
	
	/* no asterisk.conf? no problem, use buildtime config! */
	if (!cfg) {
	    return;
	}
	v = ast_variable_browse(cfg, "directories");
	while(v) {
		if (!strcasecmp(v->name, "astetcdir")) {
		    strncpy((char *)ast_config_AST_CONFIG_DIR,v->value,sizeof(ast_config_AST_CONFIG_DIR)-1);
		} else if (!strcasecmp(v->name, "astspooldir")) {
		    strncpy((char *)ast_config_AST_SPOOL_DIR,v->value,sizeof(ast_config_AST_SPOOL_DIR)-1);
		} else if (!strcasecmp(v->name, "astvarlibdir")) {
		    strncpy((char *)ast_config_AST_VAR_DIR,v->value,sizeof(ast_config_AST_VAR_DIR)-1);
		    snprintf((char *)ast_config_AST_DB,sizeof(ast_config_AST_DB)-1,"%s/%s",v->value,"astdb");    
		} else if (!strcasecmp(v->name, "astlogdir")) {
		    strncpy((char *)ast_config_AST_LOG_DIR,v->value,sizeof(ast_config_AST_LOG_DIR)-1);
		} else if (!strcasecmp(v->name, "astagidir")) {
		    strncpy((char *)ast_config_AST_AGI_DIR,v->value,sizeof(ast_config_AST_AGI_DIR)-1);
		} else if (!strcasecmp(v->name, "astrundir")) {
		    snprintf((char *)ast_config_AST_PID,sizeof(ast_config_AST_PID)-1,"%s/%s",v->value,"asterisk.pid");    
		    snprintf((char *)ast_config_AST_SOCKET,sizeof(ast_config_AST_SOCKET)-1,"%s/%s",v->value,"asterisk.ctl");    
		    strncpy((char *)ast_config_AST_RUN_DIR,v->value,sizeof(ast_config_AST_RUN_DIR)-1);
		} else if (!strcasecmp(v->name, "astmoddir")) {
		    strncpy((char *)ast_config_AST_MODULE_DIR,v->value,sizeof(ast_config_AST_MODULE_DIR)-1);
		}
		v = v->next;
	}
	ast_destroy(cfg);
}

int main(int argc, char *argv[])
{
	char c;
	char filename[80] = "";
	char hostname[256];
	char tmp[80];
	char * xarg = NULL;
	int x;
	FILE *f;
	sigset_t sigs;
	int num;
	char *buf;

	/* Remember original args for restart */
	if (argc > sizeof(_argv) / sizeof(_argv[0]) - 1) {
		fprintf(stderr, "Truncating argument size to %d\n", sizeof(_argv) / sizeof(_argv[0]) - 1);
		argc = sizeof(_argv) / sizeof(_argv[0]) - 1;
	}
	for (x=0;x<argc;x++)
		_argv[x] = argv[x];
	_argv[x] = NULL;

	if (gethostname(hostname, sizeof(hostname)))
		strncpy(hostname, "<Unknown>", sizeof(hostname)-1);
	mainpid = getpid();
	ast_ulaw_init();
	ast_alaw_init();
	callerid_init();
	tdd_init();
	if (getenv("HOME")) 
		snprintf(filename, sizeof(filename), "%s/.asterisk_history", getenv("HOME"));
	/* Check if we're root */
	/*
	if (geteuid()) {
		ast_log(LOG_ERROR, "Must be run as root\n");
		exit(1);
	}
	*/
	/* Check for options */
	while((c=getopt(argc, argv, "hfdvqprgcinx:C:")) != EOF) {
		switch(c) {
		case 'd':
			option_debug++;
			option_nofork++;
			break;
		case 'c':
			option_console++;
			option_nofork++;
			break;
		case 'f':
			option_nofork++;
			break;
		case 'n':
			option_nocolor++;
			break;
		case 'r':
			option_remote++;
			option_nofork++;
			break;
		case 'p':
			option_highpriority++;
			break;
		case 'v':
			option_verbose++;
			option_nofork++;
			break;
		case 'q':
			option_quiet++;
			break;
		case 'x':
			option_exec++;
			xarg = optarg;
			break;
		case 'C':
			strncpy((char *)ast_config_AST_CONFIG_FILE,optarg,sizeof(ast_config_AST_CONFIG_FILE));
			option_overrideconfig++;
			break;
		case 'i':
			option_initcrypto++;
			break;
		case'g':
			option_dumpcore++;
			break;
		case 'h':
			show_cli_help();
			exit(0);
		case '?':
			exit(1);
		}
	}

	if (option_dumpcore) {
		struct rlimit l;
		memset(&l, 0, sizeof(l));
		l.rlim_cur = RLIM_INFINITY;
		l.rlim_max = RLIM_INFINITY;
		if (setrlimit(RLIMIT_CORE, &l)) {
			ast_log(LOG_WARNING, "Unable to disable core size resource limit: %s\n", strerror(errno));
		}
	}

	term_init();
	printf(term_end());
	fflush(stdout);
	if (option_console && !option_verbose) 
		ast_verbose("[ Reading Master Configuration ]");
	ast_readconfig();

	if (option_console) {
                if (el_hist == NULL || el == NULL)
                        ast_el_initialize();

                if (strlen(filename))
                        ast_el_read_history(filename);
	}

	if (ast_tryconnect()) {
		/* One is already running */
		if (option_remote) {
			if (option_exec) {
				ast_remotecontrol(xarg);
				quit_handler(0, 0, 0, 0);
				exit(0);
			}
			printf(term_quit());
			ast_register_verbose(console_verboser);
			ast_verbose( "Asterisk " ASTERISK_VERSION ", Copyright (C) 1999-2001 Linux Support Services, Inc.\n");
			ast_verbose( "Written by Mark Spencer <markster@linux-support.net>\n");
			ast_verbose( "=========================================================================\n");
			ast_remotecontrol(NULL);
			quit_handler(0, 0, 0, 0);
			exit(0);
		} else {
			ast_log(LOG_ERROR, "Asterisk already running on %s.  Use 'asterisk -r' to connect.\n", (char *)ast_config_AST_SOCKET);
			printf(term_quit());
			exit(1);
		}
	} else if (option_remote || option_exec) {
		ast_log(LOG_ERROR, "Unable to connect to remote asterisk\n");
		printf(term_quit());
		exit(1);
	}
	/* Blindly write pid file since we couldn't connect */
	unlink((char *)ast_config_AST_PID);
	f = fopen((char *)ast_config_AST_PID, "w");
	if (f) {
		fprintf(f, "%d\n", getpid());
		fclose(f);
	} else
		ast_log(LOG_WARNING, "Unable to open pid file '%s': %s\n", (char *)ast_config_AST_PID, strerror(errno));

	if (!option_verbose && !option_debug && !option_nofork && !option_console) {
		daemon(0,0);
		/* Blindly re-write pid file since we are forking */
		unlink((char *)ast_config_AST_PID);
		f = fopen((char *)ast_config_AST_PID, "w");
		if (f) {
			fprintf(f, "%d\n", getpid());
			fclose(f);
		} else
			ast_log(LOG_WARNING, "Unable to open pid file '%s': %s\n", (char *)ast_config_AST_PID, strerror(errno));
	}

	ast_makesocket();
	sigemptyset(&sigs);
	sigaddset(&sigs, SIGHUP);
	sigaddset(&sigs, SIGTERM);
	sigaddset(&sigs, SIGINT);
	sigaddset(&sigs, SIGPIPE);
	sigaddset(&sigs, SIGWINCH);
	pthread_sigmask(SIG_BLOCK, &sigs, NULL);
	if (option_console || option_verbose || option_remote)
		ast_register_verbose(console_verboser);
	/* Print a welcome message if desired */
	if (option_verbose || option_console) {
		ast_verbose( "Asterisk " ASTERISK_VERSION ", Copyright (C) 1999-2001 Linux Support Services, Inc.\n");
		ast_verbose( "Written by Mark Spencer <markster@linux-support.net>\n");
		ast_verbose( "=========================================================================\n");
	}
	if (option_console && !option_verbose) 
		ast_verbose("[ Booting...");

	signal(SIGURG, urg_handler);
	signal(SIGINT, __quit_handler);
	signal(SIGTERM, __quit_handler);
	signal(SIGHUP, hup_handler);
	signal(SIGCHLD, child_handler);
	signal(SIGPIPE, SIG_IGN);

	if (set_priority(option_highpriority)) {
		printf(term_quit());
		exit(1);
	}
	if (init_logger()) {
		printf(term_quit());
		exit(1);
	}
	if (init_manager()) {
		printf(term_quit());
		exit(1);
	}
	ast_rtp_init();
	if (ast_image_init()) {
		printf(term_quit());
		exit(1);
	}
	if (load_pbx()) {
		printf(term_quit());
		exit(1);
	}
	if (load_modules()) {
		printf(term_quit());
		exit(1);
	}
	if (init_framer()) {
		printf(term_quit());
		exit(1);
	}
	if (astdb_init()) {
		printf(term_quit());
		exit(1);
	}
	if (ast_enum_init()) {
		printf(term_quit());
		exit(1);
	}
	/* We might have the option of showing a console, but for now just
	   do nothing... */
	if (option_console && !option_verbose)
		ast_verbose(" ]\n");
	if (option_verbose || option_console)
		ast_verbose(term_color(tmp, "Asterisk Ready.\n", COLOR_BRWHITE, COLOR_BLACK, sizeof(tmp)));
	fully_booted = 1;
	pthread_sigmask(SIG_UNBLOCK, &sigs, NULL);
#ifdef __AST_DEBUG_MALLOC
	__ast_mm_init();
#endif	
	time(&ast_startuptime);
	ast_cli_register(&astshutdownnow);
	ast_cli_register(&astshutdowngracefully);
	ast_cli_register(&astrestartnow);
	ast_cli_register(&astrestartgracefully);
	ast_cli_register(&astrestartwhenconvenient);
	ast_cli_register(&astshutdownwhenconvenient);
	ast_cli_register(&aborthalt);
	ast_cli_register(&astbang);
	if (option_console) {
		/* Console stuff now... */
		/* Register our quit function */
		char title[256];
		set_icon("Asterisk");
		snprintf(title, sizeof(title), "Asterisk Console on '%s' (pid %d)", hostname, mainpid);
		set_title(title);
	    ast_cli_register(&quit);
	    ast_cli_register(&astexit);
		consolethread = pthread_self();

		for (;;) {
			buf = (char *)el_gets(el, &num);
			if (buf) {
				if (buf[strlen(buf)-1] == '\n')
					buf[strlen(buf)-1] = '\0';

				consolehandler((char *)buf);
			} else {
				if (option_remote)
 					ast_cli(STDOUT_FILENO, "\nUse EXIT or QUIT to exit the asterisk console\n");
				else
	 				ast_cli(STDOUT_FILENO, "\nUse STOP NOW to shutdown Asterisk\n");
			}
		}

	} else {
 		/* Do nothing */
		ast_select(0,NULL,NULL,NULL,NULL);
	}
	return 0;
}
