
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
 * \brief Meet me conference bridge
 *
 * \author Mark Spencer <markster@digium.com>
 * 
 * \ingroup applications
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#ifdef __linux__
#include <linux/zaptel.h>
#else
#include <zaptel.h>
#endif /* __linux__ */

#include "asterisk.h"

ASTERISK_FILE_VERSION(__FILE__, "$Revision$")

#include "asterisk/lock.h"
#include "asterisk/file.h"
#include "asterisk/logger.h"
#include "asterisk/channel.h"
#include "asterisk/pbx.h"
#include "asterisk/module.h"
#include "asterisk/config.h"
#include "asterisk/app.h"
#include "asterisk/dsp.h"
#include "asterisk/musiconhold.h"
#include "asterisk/manager.h"
#include "asterisk/options.h"
#include "asterisk/cli.h"
#include "asterisk/say.h"
#include "asterisk/utils.h"
#include "asterisk/translate.h"
#include "asterisk/ulaw.h"

static const char *tdesc = "MeetMe conference bridge";

static const char *app = "MeetMe";
static const char *app2 = "MeetMeCount";
static const char *app3 = "MeetMeAdmin";

static const char *synopsis = "MeetMe conference bridge";
static const char *synopsis2 = "MeetMe participant count";
static const char *synopsis3 = "MeetMe conference Administration";

static const char *descrip =
"  MeetMe([confno][,[options][,pin]]): Enters the user into a specified MeetMe\n"
"conference.  If the conference number is omitted, the user will be prompted\n"
"to enter one.  User can exit the conference by hangup, or if the 'p' option\n"
"is specified, by pressing '#'.\n"
"Please note: A ZAPTEL INTERFACE MUST BE INSTALLED FOR CONFERENCING TO WORK!\n\n"

"The option string may contain zero or more of the following characters:\n"
"      'a' -- set admin mode\n"
"      'A' -- set marked mode\n"
"      'b' -- run AGI script specified in ${MEETME_AGI_BACKGROUND}\n"
"             Default: conf-background.agi  (Note: This does not work with\n"
"             non-Zap channels in the same conference)\n"
"      'c' -- announce user(s) count on joining a conference\n"
"      'd' -- dynamically add conference\n"
"      'D' -- dynamically add conference, prompting for a PIN\n"
"      'e' -- select an empty conference\n"
"      'E' -- select an empty pinless conference\n"
"      'i' -- announce user join/leave\n"
"      'm' -- set monitor only mode (Listen only, no talking)\n"
"      'M' -- enable music on hold when the conference has a single caller\n"
"      'o' -- set talker optimization - treats talkers who aren't speaking as\n"
"             being muted, meaning (a) No encode is done on transmission and\n"
"             (b) Received audio that is not registered as talking is omitted\n"
"             causing no buildup in background noise\n"
"      'p' -- allow user to exit the conference by pressing '#'\n"
"      'P' -- always prompt for the pin even if it is specified\n"
"      'q' -- quiet mode (don't play enter/leave sounds)\n"
"      'r' -- Record conference (records as ${MEETME_RECORDINGFILE}\n"
"             using format ${MEETME_RECORDINGFORMAT}). Default filename is\n"
"             meetme-conf-rec-${CONFNO}-${UNIQUEID} and the default format is\n"
"             wav.\n"
"      's' -- Present menu (user or admin) when '*' is received ('send' to menu)\n"
"      't' -- set talk only mode. (Talk only, no listening)\n"
"      'T' -- set talker detection (sent to manager interface and meetme list)\n"
"      'v' -- video mode\n"
"      'w' -- wait until the marked user enters the conference\n"
"      'x' -- close the conference when last marked user exits\n"
"      'X' -- allow user to exit the conference by entering a valid single\n"
"             digit extension ${MEETME_EXIT_CONTEXT} or the current context\n"
"             if that variable is not defined.\n"
"      '1' -- do not play message when first person enters\n";

static const char *descrip2 =
"  MeetMeCount(confno[|var]): Plays back the number of users in the specified\n"
"MeetMe conference. If var is specified, playback will be skipped and the value\n"
"will be returned in the variable. Upon app completion, MeetMeCount will hangup\n"
"the channel, unless priority n+1 exists, in which case priority progress will\n"
"continue.\n"
"A ZAPTEL INTERFACE MUST BE INSTALLED FOR CONFERENCING FUNCTIONALITY.\n";

static const char *descrip3 = 
"  MeetMeAdmin(confno,command[,user]): Run admin command for conference\n"
"      'e' -- Eject last user that joined\n"
"      'k' -- Kick one user out of conference\n"
"      'K' -- Kick all users out of conference\n"
"      'l' -- Unlock conference\n"
"      'L' -- Lock conference\n"
"      'm' -- Unmute conference\n"
"      'M' -- Mute conference\n"
"      'n' -- Unmute entire conference (except admin)\n"
"      'N' -- Mute entire conference (except admin)\n"
"";

#define CONFIG_FILE_NAME "meetme.conf"

LOCAL_USER_DECL;

struct ast_conference {
	ast_mutex_t playlock;				/* Conference specific lock (players) */
	ast_mutex_t listenlock;				/* Conference specific lock (listeners) */
	char confno[AST_MAX_EXTENSION];		/* Conference */
	struct ast_channel *chan;		/* Announcements channel */
	struct ast_channel *lchan;		/* Listen/Record channel */
	int fd;					/* Announcements fd */
	int zapconf;				/* Zaptel Conf # */
	int users;				/* Number of active users */
	int markedusers;			/* Number of marked users */
	struct ast_conf_user *firstuser;	/* Pointer to the first user struct */
	struct ast_conf_user *lastuser;		/* Pointer to the last user struct */
	time_t start;				/* Start time (s) */
	int refcount;				/* reference count of usage */
	unsigned int recording:2;				/* recording status */
	unsigned int isdynamic:1;				/* Created on the fly? */
	unsigned int locked:1;				/* Is the conference locked? */
	pthread_t recordthread;			/* thread for recording */
	pthread_attr_t attr;			/* thread attribute */
	const char *recordingfilename;		/* Filename to record the Conference into */
	const char *recordingformat;			/* Format to record the Conference in */
	char pin[AST_MAX_EXTENSION];		/* If protected by a PIN */
	char pinadmin[AST_MAX_EXTENSION];	/* If protected by a admin PIN */
	struct ast_frame *transframe[32];
	struct ast_frame *origframe;
	struct ast_trans_pvt *transpath[32];
	AST_LIST_ENTRY(ast_conference) list;
};

static AST_LIST_HEAD_STATIC(confs, ast_conference);

struct volume {
	int desired;				/* Desired volume adjustment */
	int actual;				/* Actual volume adjustment (for channels that can't adjust) */
};

struct ast_conf_user {
	int user_no;				/* User Number */
	struct ast_conf_user *prevuser;		/* Pointer to the previous user */
	struct ast_conf_user *nextuser;		/* Pointer to the next user */
	int userflags;				/* Flags as set in the conference */
	int adminflags;				/* Flags set by the Admin */
	struct ast_channel *chan;		/* Connected channel */
	int talking;				/* Is user talking */
	int zapchannel;				/* Is a Zaptel channel */
	char usrvalue[50];			/* Custom User Value */
	char namerecloc[AST_MAX_EXTENSION];	/* Name Recorded file Location */
	time_t jointime;			/* Time the user joined the conference */
	struct volume talk;
	struct volume listen;
};

static int audio_buffers;			/* The number of audio buffers to be allocated on pseudo channels
						   when in a conference
						*/

#define DEFAULT_AUDIO_BUFFERS 32		/* each buffer is 20ms, so this is 640ms total */

#define ADMINFLAG_MUTED (1 << 1)		/* User is muted */
#define ADMINFLAG_KICKME (1 << 2)		/* User is kicked */
#define MEETME_DELAYDETECTTALK 		300
#define MEETME_DELAYDETECTENDTALK 	1000

#define AST_FRAME_BITS 32

enum volume_action {
	VOL_UP,
	VOL_DOWN,
};

static int admin_exec(struct ast_channel *chan, void *data);

static void *recordthread(void *args);

#include "enter.h"
#include "leave.h"

#define ENTER	0
#define LEAVE	1

#define MEETME_RECORD_OFF	0
#define MEETME_RECORD_STARTED	1
#define MEETME_RECORD_ACTIVE	2
#define MEETME_RECORD_TERMINATE	3

#define CONF_SIZE 320

#define CONFFLAG_ADMIN	(1 << 1)		/* If set the user has admin access on the conference */
#define CONFFLAG_MONITOR (1 << 2)		/* If set the user can only receive audio from the conference */
#define CONFFLAG_POUNDEXIT (1 << 3)		/* If set asterisk will exit conference when '#' is pressed */
#define CONFFLAG_STARMENU (1 << 4)		/* If set asterisk will provide a menu to the user when '*' is pressed */
#define CONFFLAG_TALKER (1 << 5)		/* If set the use can only send audio to the conference */
#define CONFFLAG_QUIET (1 << 6)			/* If set there will be no enter or leave sounds */
#define CONFFLAG_VIDEO (1 << 7)			/* Set to enable video mode */
#define CONFFLAG_AGI (1 << 8)			/* Set to run AGI Script in Background */
#define CONFFLAG_MOH (1 << 9)			/* Set to have music on hold when user is alone in conference */
#define CONFFLAG_MARKEDEXIT (1 << 10)		/* If set the MeetMe will return if all marked with this flag left */
#define CONFFLAG_WAITMARKED (1 << 11)		/* If set, the MeetMe will wait until a marked user enters */
#define CONFFLAG_EXIT_CONTEXT (1 << 12)		/* If set, the MeetMe will exit to the specified context */
#define CONFFLAG_MARKEDUSER (1 << 13)		/* If set, the user will be marked */
#define CONFFLAG_INTROUSER (1 << 14)		/* If set, user will be ask record name on entry of conference */
#define CONFFLAG_RECORDCONF (1<< 15)		/* If set, the MeetMe will be recorded */
#define CONFFLAG_MONITORTALKER (1 << 16)	/* If set, the user will be monitored if the user is talking or not */
#define CONFFLAG_DYNAMIC (1 << 17)
#define CONFFLAG_DYNAMICPIN (1 << 18)
#define CONFFLAG_EMPTY (1 << 19)
#define CONFFLAG_EMPTYNOPIN (1 << 20)
#define CONFFLAG_ALWAYSPROMPT (1 << 21)
#define CONFFLAG_ANNOUNCEUSERCOUNT (1 << 22)	/* If set, when user joins the conference, they will be told the number of users that are already in */
#define CONFFLAG_OPTIMIZETALKER (1 << 23)	/* If set, treats talking users as muted users */
#define CONFFLAG_NOONLYPERSON (1 << 24)		/* If set, won't speak the extra prompt when the first person enters the conference */


AST_APP_OPTIONS(meetme_opts, {
	AST_APP_OPTION('A', CONFFLAG_MARKEDUSER ),
	AST_APP_OPTION('a', CONFFLAG_ADMIN ),
	AST_APP_OPTION('b', CONFFLAG_AGI ),
	AST_APP_OPTION('c', CONFFLAG_ANNOUNCEUSERCOUNT ),
	AST_APP_OPTION('D', CONFFLAG_DYNAMICPIN ),
	AST_APP_OPTION('d', CONFFLAG_DYNAMIC ),
	AST_APP_OPTION('E', CONFFLAG_EMPTYNOPIN ),
	AST_APP_OPTION('e', CONFFLAG_EMPTY ),
	AST_APP_OPTION('i', CONFFLAG_INTROUSER ),
	AST_APP_OPTION('M', CONFFLAG_MOH ),
	AST_APP_OPTION('m', CONFFLAG_MONITOR ),
	AST_APP_OPTION('o', CONFFLAG_OPTIMIZETALKER ),
	AST_APP_OPTION('P', CONFFLAG_ALWAYSPROMPT ),
	AST_APP_OPTION('p', CONFFLAG_POUNDEXIT ),
	AST_APP_OPTION('q', CONFFLAG_QUIET ),
	AST_APP_OPTION('r', CONFFLAG_RECORDCONF ),
	AST_APP_OPTION('s', CONFFLAG_STARMENU ),
	AST_APP_OPTION('T', CONFFLAG_MONITORTALKER ),
	AST_APP_OPTION('t', CONFFLAG_TALKER ),
	AST_APP_OPTION('w', CONFFLAG_WAITMARKED ),
	AST_APP_OPTION('X', CONFFLAG_EXIT_CONTEXT ),
	AST_APP_OPTION('x', CONFFLAG_MARKEDEXIT ),
	AST_APP_OPTION('1', CONFFLAG_NOONLYPERSON ),
});

static char *istalking(int x)
{
	if (x > 0)
		return "(talking)";
	else if (x < 0)
		return "(unmonitored)";
	else 
		return "(not talking)";
}

static int careful_write(int fd, unsigned char *data, int len, int block)
{
	int res;
	int x;

	while (len) {
		if (block) {
			x = ZT_IOMUX_WRITE | ZT_IOMUX_SIGEVENT;
			res = ioctl(fd, ZT_IOMUX, &x);
		} else
			res = 0;
		if (res >= 0)
			res = write(fd, data, len);
		if (res < 1) {
			if (errno != EAGAIN) {
				ast_log(LOG_WARNING, "Failed to write audio data to conference: %s\n", strerror(errno));
				return -1;
			} else
				return 0;
		}
		len -= res;
		data += res;
	}

	return 0;
}

/* Map 'volume' levels from -5 through +5 into
   decibel (dB) settings for channel drivers
   Note: these are not a straight linear-to-dB
   conversion... the numbers have been modified
   to give the user a better level of adjustability
*/
static signed char gain_map[] = {
	-15,
	-13,
	-10,
	-6,
	0,
	0,
	0,
	6,
	10,
	13,
	15,
};

static int set_talk_volume(struct ast_conf_user *user, int volume)
{
	signed char gain_adjust;

	/* attempt to make the adjustment in the channel driver;
	   if successful, don't adjust in the frame reading routine
	*/
	gain_adjust = gain_map[volume + 5];

	return ast_channel_setoption(user->chan, AST_OPTION_RXGAIN, &gain_adjust, sizeof(gain_adjust), 0);
}

static int set_listen_volume(struct ast_conf_user *user, int volume)
{
	signed char gain_adjust;

	/* attempt to make the adjustment in the channel driver;
	   if successful, don't adjust in the frame reading routine
	*/
	gain_adjust = gain_map[volume + 5];

	return ast_channel_setoption(user->chan, AST_OPTION_TXGAIN, &gain_adjust, sizeof(gain_adjust), 0);
}

static void tweak_volume(struct volume *vol, enum volume_action action)
{
	switch (action) {
	case VOL_UP:
		switch (vol->desired) {
		case 5:
			break;
		case 0:
			vol->desired = 2;
			break;
		case -2:
			vol->desired = 0;
			break;
		default:
			vol->desired++;
			break;
		}
		break;
	case VOL_DOWN:
		switch (vol->desired) {
		case -5:
			break;
		case 2:
			vol->desired = 0;
			break;
		case 0:
			vol->desired = -2;
			break;
		default:
			vol->desired--;
			break;
		}
	}
}

static void tweak_talk_volume(struct ast_conf_user *user, enum volume_action action)
{
	tweak_volume(&user->talk, action);
	/* attempt to make the adjustment in the channel driver;
	   if successful, don't adjust in the frame reading routine
	*/
	if (!set_talk_volume(user, user->talk.desired))
		user->talk.actual = 0;
	else
		user->talk.actual = user->talk.desired;
}

static void tweak_listen_volume(struct ast_conf_user *user, enum volume_action action)
{
	tweak_volume(&user->listen, action);
	/* attempt to make the adjustment in the channel driver;
	   if successful, don't adjust in the frame reading routine
	*/
	if (!set_listen_volume(user, user->listen.desired))
		user->listen.actual = 0;
	else
		user->listen.actual = user->listen.desired;
}

static void reset_volumes(struct ast_conf_user *user)
{
	signed char zero_volume = 0;

	ast_channel_setoption(user->chan, AST_OPTION_TXGAIN, &zero_volume, sizeof(zero_volume), 0);
	ast_channel_setoption(user->chan, AST_OPTION_RXGAIN, &zero_volume, sizeof(zero_volume), 0);
}

static void conf_play(struct ast_channel *chan, struct ast_conference *conf, int sound)
{
	unsigned char *data;
	int len;
	int res = -1;

	if (!chan->_softhangup)
		res = ast_autoservice_start(chan);

	AST_LIST_LOCK(&confs);

	switch(sound) {
	case ENTER:
		data = enter;
		len = sizeof(enter);
		break;
	case LEAVE:
		data = leave;
		len = sizeof(leave);
		break;
	default:
		data = NULL;
		len = 0;
	}
	if (data) {
		careful_write(conf->fd, data, len, 1);
	}

	AST_LIST_UNLOCK(&confs);

	if (!res) 
		ast_autoservice_stop(chan);
}

static struct ast_conference *build_conf(char *confno, char *pin, char *pinadmin, int make, int dynamic, int refcount)
{
	struct ast_conference *cnf;
	struct zt_confinfo ztc;

	AST_LIST_LOCK(&confs);

	AST_LIST_TRAVERSE(&confs, cnf, list) {
		if (!strcmp(confno, cnf->confno)) 
			break;
	}

	if (!cnf && (make || dynamic)) {
		/* Make a new one */
		if ((cnf = ast_calloc(1, sizeof(*cnf)))) {
			ast_mutex_init(&cnf->playlock);
			ast_mutex_init(&cnf->listenlock);
			ast_copy_string(cnf->confno, confno, sizeof(cnf->confno));
			ast_copy_string(cnf->pin, pin, sizeof(cnf->pin));
			ast_copy_string(cnf->pinadmin, pinadmin, sizeof(cnf->pinadmin));
			cnf->refcount = 0;
			cnf->markedusers = 0;
			cnf->chan = ast_request("zap", AST_FORMAT_SLINEAR, "pseudo", NULL);
			if (cnf->chan) {
				ast_set_read_format(cnf->chan, AST_FORMAT_SLINEAR);
				ast_set_write_format(cnf->chan, AST_FORMAT_SLINEAR);
				cnf->fd = cnf->chan->fds[0];	/* for use by conf_play() */
			} else {
				ast_log(LOG_WARNING, "Unable to open pseudo channel - trying device\n");
				cnf->fd = open("/dev/zap/pseudo", O_RDWR);
				if (cnf->fd < 0) {
					ast_log(LOG_WARNING, "Unable to open pseudo device\n");
					free(cnf);
					cnf = NULL;
					goto cnfout;
				}
			}
			memset(&ztc, 0, sizeof(ztc));
			/* Setup a new zap conference */
			ztc.chan = 0;
			ztc.confno = -1;
			ztc.confmode = ZT_CONF_CONFANN | ZT_CONF_CONFANNMON;
			if (ioctl(cnf->fd, ZT_SETCONF, &ztc)) {
				ast_log(LOG_WARNING, "Error setting conference\n");
				if (cnf->chan)
					ast_hangup(cnf->chan);
				else
					close(cnf->fd);
				free(cnf);
				cnf = NULL;
				goto cnfout;
			}
			cnf->lchan = ast_request("zap", AST_FORMAT_SLINEAR, "pseudo", NULL);
			if (cnf->lchan) {
				ast_set_read_format(cnf->lchan, AST_FORMAT_SLINEAR);
				ast_set_write_format(cnf->lchan, AST_FORMAT_SLINEAR);
				ztc.chan = 0;
				ztc.confmode = ZT_CONF_CONFANN | ZT_CONF_CONFANNMON;
				if (ioctl(cnf->lchan->fds[0], ZT_SETCONF, &ztc)) {
					ast_log(LOG_WARNING, "Error setting conference\n");
					ast_hangup(cnf->lchan);
					cnf->lchan = NULL;
				}
			}
			/* Fill the conference struct */
			cnf->start = time(NULL);
			cnf->zapconf = ztc.confno;
			cnf->isdynamic = dynamic ? 1 : 0;
			cnf->firstuser = NULL;
			cnf->lastuser = NULL;
			cnf->locked = 0;
			if (option_verbose > 2)
				ast_verbose(VERBOSE_PREFIX_3 "Created MeetMe conference %d for conference '%s'\n", cnf->zapconf, cnf->confno);
			AST_LIST_INSERT_HEAD(&confs, cnf, list);
		} 
	}
 cnfout:
	if (cnf){ 
		cnf->refcount += refcount;
	}
	AST_LIST_UNLOCK(&confs);
	return cnf;
}

static int confs_show(int fd, int argc, char **argv)
{
	ast_cli(fd, "Deprecated! Please use 'meetme' instead.\n");

	return RESULT_SUCCESS;
}

static char show_confs_usage[] =
"Deprecated! Please use 'meetme' instead.\n";

static struct ast_cli_entry cli_show_confs = {
	{ "show", "conferences", NULL }, confs_show,
	"Show status of conferences", show_confs_usage, NULL };
	
static int conf_cmd(int fd, int argc, char **argv) {
	/* Process the command */
	struct ast_conference *cnf;
	struct ast_conf_user *user;
	int hr, min, sec;
	int i = 0, total = 0;
	time_t now;
	char *header_format = "%-14s %-14s %-10s %-8s  %-8s\n";
	char *data_format = "%-12.12s   %4.4d	      %4.4s       %02d:%02d:%02d  %-8s\n";
	char cmdline[1024] = "";

	if (argc > 8)
		ast_cli(fd, "Invalid Arguments.\n");
	/* Check for length so no buffer will overflow... */
	for (i = 0; i < argc; i++) {
		if (strlen(argv[i]) > 100)
			ast_cli(fd, "Invalid Arguments.\n");
	}
	if (argc == 1) {
		/* 'MeetMe': List all the conferences */	
		now = time(NULL);
		if (AST_LIST_EMPTY(&confs)) {
			ast_cli(fd, "No active MeetMe conferences.\n");
			return RESULT_SUCCESS;
		}
		ast_cli(fd, header_format, "Conf Num", "Parties", "Marked", "Activity", "Creation");
		AST_LIST_TRAVERSE(&confs, cnf, list) {
			if (cnf->markedusers == 0)
				strcpy(cmdline, "N/A ");
			else 
				snprintf(cmdline, sizeof(cmdline), "%4.4d", cnf->markedusers);
			hr = (now - cnf->start) / 3600;
			min = ((now - cnf->start) % 3600) / 60;
			sec = (now - cnf->start) % 60;

			ast_cli(fd, data_format, cnf->confno, cnf->users, cmdline, hr, min, sec, cnf->isdynamic ? "Dynamic" : "Static");

			total += cnf->users; 	
		}
		ast_cli(fd, "* Total number of MeetMe users: %d\n", total);
		return RESULT_SUCCESS;
	}
	if (argc < 3)
		return RESULT_SHOWUSAGE;
	ast_copy_string(cmdline, argv[2], sizeof(cmdline));	/* Argv 2: conference number */
	if (strstr(argv[1], "lock")) {	
		if (strcmp(argv[1], "lock") == 0) {
			/* Lock */
			strncat(cmdline, "|L", sizeof(cmdline) - strlen(cmdline) - 1);
		} else {
			/* Unlock */
			strncat(cmdline, "|l", sizeof(cmdline) - strlen(cmdline) - 1);
		}
	} else if (strstr(argv[1], "mute")) { 
		if (argc < 4)
			return RESULT_SHOWUSAGE;
		if (strcmp(argv[1], "mute") == 0) {
			/* Mute */
			if (strcmp(argv[3], "all") == 0) {
				strncat(cmdline, "|N", sizeof(cmdline) - strlen(cmdline) - 1);
			} else {
				strncat(cmdline, "|M|", sizeof(cmdline) - strlen(cmdline) - 1);	
				strncat(cmdline, argv[3], sizeof(cmdline) - strlen(cmdline) - 1);
			}
		} else {
			/* Unmute */
			if (strcmp(argv[3], "all") == 0) {
				strncat(cmdline, "|n", sizeof(cmdline) - strlen(cmdline) - 1);
			} else {
				strncat(cmdline, "|m|", sizeof(cmdline) - strlen(cmdline) - 1);
				strncat(cmdline, argv[3], sizeof(cmdline) - strlen(cmdline) - 1);
			}
		}
	} else if (strcmp(argv[1], "kick") == 0) {
		if (argc < 4)
			return RESULT_SHOWUSAGE;
		if (strcmp(argv[3], "all") == 0) {
			/* Kick all */
			strncat(cmdline, "|K", sizeof(cmdline) - strlen(cmdline) - 1);
		} else {
			/* Kick a single user */
			strncat(cmdline, "|k|", sizeof(cmdline) - strlen(cmdline) - 1);
			strncat(cmdline, argv[3], sizeof(cmdline) - strlen(cmdline) - 1);
		}	
	} else if(strcmp(argv[1], "list") == 0) {
		/* List all the users in a conference */
		if (AST_LIST_EMPTY(&confs)) {
			ast_cli(fd, "No active conferences.\n");
			return RESULT_SUCCESS;	
		}
		/* Find the right conference */
		AST_LIST_TRAVERSE(&confs, cnf, list) {
			if (strcmp(cnf->confno, argv[2]) == 0)
				break;
		}
		if (!cnf) {
			ast_cli(fd, "No such conference: %s.\n",argv[2]);
			return RESULT_SUCCESS;
		}
		/* Show all the users */
		for (user = cnf->firstuser; user; user = user->nextuser){
			now = time(NULL);
			hr = (now - user->jointime) / 3600;
			min = ((now - user->jointime) % 3600) / 60;
			sec = (now - user->jointime) % 60;


			ast_cli(fd, "User #: %-2.2d %12.12s %-20.20s Channel: %s %s %s %s %s %02d:%02d:%02d\n",
				user->user_no,
				user->chan->cid.cid_num ? user->chan->cid.cid_num : "<unknown>",
				user->chan->cid.cid_name ? user->chan->cid.cid_name : "<no name>",
				user->chan->name,
				user->userflags & CONFFLAG_ADMIN ? "(Admin)" : "",
				user->userflags & CONFFLAG_MONITOR ? "(Listen only)" : "",
				user->adminflags & ADMINFLAG_MUTED ? "(Admn Muted)" : "",
				istalking(user->talking), hr, min, sec);
		}
		ast_cli(fd,"%d users in that conference.\n",cnf->users);

		return RESULT_SUCCESS;
	} else 
		return RESULT_SHOWUSAGE;
	ast_log(LOG_DEBUG, "Cmdline: %s\n", cmdline);
	admin_exec(NULL, cmdline);

	return 0;
}

static char *complete_confcmd(const char *line, const char *word, int pos, int state) {
#define CONF_COMMANDS 6
	int which = 0, x = 0;
	struct ast_conference *cnf = NULL;
	struct ast_conf_user *usr = NULL;
	char *confno = NULL;
	char usrno[50] = "";
	char cmds[CONF_COMMANDS][20] = {"lock", "unlock", "mute", "unmute", "kick", "list"};
	char *myline;
	
	if (pos == 1) {
		/* Command */
		for (x = 0;x < CONF_COMMANDS; x++) {
			if (!strncasecmp(cmds[x], word, strlen(word))) {
				if (++which > state) {
					return strdup(cmds[x]);
				}
			}
		}
	} else if (pos == 2) {
		/* Conference Number */
		AST_LIST_LOCK(&confs);
		AST_LIST_TRAVERSE(&confs, cnf, list) {
			if (!strncasecmp(word, cnf->confno, strlen(word))) {
				if (++which > state)
					break;
			}
		}
		AST_LIST_UNLOCK(&confs);
		return cnf ? strdup(cnf->confno) : NULL;
	} else if (pos == 3) {
		/* User Number || Conf Command option*/
		if (strstr(line, "mute") || strstr(line, "kick")) {
			if ((state == 0) && (strstr(line, "kick") || strstr(line,"mute")) && !(strncasecmp(word, "all", strlen(word)))) {
				return strdup("all");
			}
			which++;
			AST_LIST_LOCK(&confs);

			/* TODO: Find the conf number from the cmdline (ignore spaces) <- test this and make it fail-safe! */
			myline = ast_strdupa(line);
			if (strsep(&myline, " ") && strsep(&myline, " ") && !confno) {
				while((confno = strsep(&myline, " ")) && (strcmp(confno, " ") == 0))
					;
			}
			
			AST_LIST_TRAVERSE(&confs, cnf, list) {
				if (!strcmp(confno, cnf->confno))
				    break;
			}

			if (cnf) {
				/* Search for the user */
				for (usr = cnf->firstuser; usr; usr = usr->nextuser) {
					snprintf(usrno, sizeof(usrno), "%d", usr->user_no);
					if (!strncasecmp(word, usrno, strlen(word))) {
						if (++which > state)
							break;
					}
				}
			}
			AST_LIST_UNLOCK(&confs);
			return usr ? strdup(usrno) : NULL;
		}
	}

	return NULL;
}
	
static char conf_usage[] =
"Usage: meetme  (un)lock|(un)mute|kick|list <confno> <usernumber>\n"
"       Executes a command for the conference or on a conferee\n";

static struct ast_cli_entry cli_conf = {
	{"meetme", NULL, NULL }, conf_cmd,
	"Execute a command on a conference or conferee", conf_usage, complete_confcmd};

static void conf_flush(int fd, struct ast_channel *chan)
{
	int x;

	/* read any frames that may be waiting on the channel
	   and throw them away
	*/
	if (chan) {
		struct ast_frame *f;

		/* when no frames are available, this will wait
		   for 1 millisecond maximum
		*/
		while (ast_waitfor(chan, 1)) {
			f = ast_read(chan);
			if (f)
				ast_frfree(f);
			else /* channel was hung up or something else happened */
				break;
		}
	}

	/* flush any data sitting in the pseudo channel */
	x = ZT_FLUSH_ALL;
	if (ioctl(fd, ZT_FLUSH, &x))
		ast_log(LOG_WARNING, "Error flushing channel\n");

}

/* Remove the conference from the list and free it.
   We assume that this was called while holding conflock. */
static int conf_free(struct ast_conference *conf)
{
	int x;
	
	AST_LIST_REMOVE(&confs, conf, list);

	if (conf->recording == MEETME_RECORD_ACTIVE) {
		conf->recording = MEETME_RECORD_TERMINATE;
		AST_LIST_UNLOCK(&confs);
		while (1) {
			AST_LIST_LOCK(&confs);
			if (conf->recording == MEETME_RECORD_OFF)
				break;
			AST_LIST_UNLOCK(&confs);
		}
	}

	for (x=0;x<AST_FRAME_BITS;x++) {
		if (conf->transframe[x])
			ast_frfree(conf->transframe[x]);
		if (conf->transpath[x])
			ast_translator_free_path(conf->transpath[x]);
	}
	if (conf->origframe)
		ast_frfree(conf->origframe);
	if (conf->lchan)
		ast_hangup(conf->lchan);
	if (conf->chan)
		ast_hangup(conf->chan);
	else
		close(conf->fd);
	
	free(conf);

	return 0;
}

static int conf_run(struct ast_channel *chan, struct ast_conference *conf, int confflags)
{
	struct ast_conf_user *user = NULL;
	struct ast_conf_user *usr = NULL;
	int fd;
	struct zt_confinfo ztc, ztc_empty;
	struct ast_frame *f;
	struct ast_channel *c;
	struct ast_frame fr;
	int outfd;
	int ms;
	int nfds;
	int res;
	int flags;
	int retryzap;
	int origfd;
	int musiconhold = 0;
	int firstpass = 0;
	int lastmarked = 0;
	int currentmarked = 0;
	int ret = -1;
	int x;
	int menu_active = 0;
	int using_pseudo = 0;
	int duration=20;
	int hr, min, sec;
	time_t now;
	struct ast_dsp *dsp=NULL;
	struct ast_app *app;
	const char *agifile;
	const char *agifiledefault = "conf-background.agi";
	char meetmesecs[30] = "";
	char exitcontext[AST_MAX_CONTEXT] = "";
	char recordingtmp[AST_MAX_EXTENSION] = "";
	char members[10] = "";
	int dtmf;
	ZT_BUFFERINFO bi;
	char __buf[CONF_SIZE + AST_FRIENDLY_OFFSET];
	char *buf = __buf + AST_FRIENDLY_OFFSET;

	if (!(user = ast_calloc(1, sizeof(*user)))) {
		AST_LIST_LOCK(&confs);
		conf->refcount--;
		if (!conf->refcount){
			conf_free(conf);
		}
		AST_LIST_UNLOCK(&confs);
		return ret;
	}

	if (confflags & CONFFLAG_RECORDCONF) {
		if (!conf->recordingfilename) {
			conf->recordingfilename = pbx_builtin_getvar_helper(chan, "MEETME_RECORDINGFILE");
			if (!conf->recordingfilename) {
				snprintf(recordingtmp, sizeof(recordingtmp), "meetme-conf-rec-%s-%s", conf->confno, chan->uniqueid);
				conf->recordingfilename = ast_strdupa(recordingtmp);
			}
			conf->recordingformat = pbx_builtin_getvar_helper(chan, "MEETME_RECORDINGFORMAT");
			if (!conf->recordingformat) {
				snprintf(recordingtmp, sizeof(recordingtmp), "wav");
				conf->recordingformat = ast_strdupa(recordingtmp);
			}
			ast_verbose(VERBOSE_PREFIX_4 "Starting recording of MeetMe Conference %s into file %s.%s.\n",
				    conf->confno, conf->recordingfilename, conf->recordingformat);
		}
	}

	if ((conf->recording == MEETME_RECORD_OFF) && ((confflags & CONFFLAG_RECORDCONF) || (conf->lchan))) {
		pthread_attr_init(&conf->attr);
		pthread_attr_setdetachstate(&conf->attr, PTHREAD_CREATE_DETACHED);
		ast_pthread_create(&conf->recordthread, &conf->attr, recordthread, conf);
	}

	time(&user->jointime);

	if (conf->locked && (!(confflags & CONFFLAG_ADMIN))) {
		/* Sorry, but this confernce is locked! */	
		if (!ast_streamfile(chan, "conf-locked", chan->language))
			ast_waitstream(chan, "");
		goto outrun;
	}

	if (confflags & CONFFLAG_MARKEDUSER)
		conf->markedusers++;
      
   	ast_mutex_lock(&conf->playlock);
	if (!conf->firstuser) {
		/* Fill the first new User struct */
		user->user_no = 1;
		conf->firstuser = user;
		conf->lastuser = user;
	} else {
		/* Fill the new user struct */	
		user->user_no = conf->lastuser->user_no + 1; 
		user->prevuser = conf->lastuser;
		if (conf->lastuser->nextuser) {
			ast_log(LOG_WARNING, "Error in User Management!\n");
			ast_mutex_unlock(&conf->playlock);
			goto outrun;
		} else {
			conf->lastuser->nextuser = user;
			conf->lastuser = user;
		}
	}

	user->chan = chan;
	user->userflags = confflags;
	user->adminflags = 0;
	user->talking = -1;
	conf->users++;
	/* Update table */
	snprintf(members, sizeof(members), "%d", conf->users);
	ast_update_realtime("meetme", "confno", conf->confno, "members", members , NULL);

	ast_mutex_unlock(&conf->playlock);

	if (confflags & CONFFLAG_EXIT_CONTEXT) {
		if ((agifile = pbx_builtin_getvar_helper(chan, "MEETME_EXIT_CONTEXT"))) 
			ast_copy_string(exitcontext, agifile, sizeof(exitcontext));
		else if (!ast_strlen_zero(chan->macrocontext)) 
			ast_copy_string(exitcontext, chan->macrocontext, sizeof(exitcontext));
		else
			ast_copy_string(exitcontext, chan->context, sizeof(exitcontext));
	}

	if (!(confflags & CONFFLAG_QUIET) && (confflags & CONFFLAG_INTROUSER)) {
		snprintf(user->namerecloc, sizeof(user->namerecloc),
			 "%s/meetme/meetme-username-%s-%d", ast_config_AST_SPOOL_DIR,
			 conf->confno, user->user_no);
		res = ast_record_review(chan, "vm-rec-name", user->namerecloc, 10, "sln", &duration, NULL);
		if (res == -1)
			goto outrun;
	}

	if ( !(confflags & (CONFFLAG_QUIET | CONFFLAG_NOONLYPERSON)) ) {
		if (conf->users == 1 && !(confflags & CONFFLAG_WAITMARKED))
			if (!ast_streamfile(chan, "conf-onlyperson", chan->language))
				ast_waitstream(chan, "");
		if ((confflags & CONFFLAG_WAITMARKED) && conf->markedusers == 0)
			if (!ast_streamfile(chan, "conf-waitforleader", chan->language))
				ast_waitstream(chan, "");
	}

	if (!(confflags & CONFFLAG_QUIET) && (confflags & CONFFLAG_ANNOUNCEUSERCOUNT) && conf->users > 1) {
		int keepplaying = 1;

		if (conf->users == 2) { 
			if (!ast_streamfile(chan,"conf-onlyone",chan->language)) {
				res = ast_waitstream(chan, AST_DIGIT_ANY);
				if (res > 0)
					keepplaying=0;
				else if (res == -1)
					goto outrun;
			}
		} else { 
			if (!ast_streamfile(chan, "conf-thereare", chan->language)) {
				res = ast_waitstream(chan, AST_DIGIT_ANY);
				if (res > 0)
					keepplaying=0;
				else if (res == -1)
					goto outrun;
			}
			if (keepplaying) {
				res = ast_say_number(chan, conf->users - 1, AST_DIGIT_ANY, chan->language, (char *) NULL);
				if (res > 0)
					keepplaying=0;
				else if (res == -1)
					goto outrun;
			}
			if (keepplaying && !ast_streamfile(chan, "conf-otherinparty", chan->language)) {
				res = ast_waitstream(chan, AST_DIGIT_ANY);
				if (res > 0)
					keepplaying=0;
				else if (res == -1) 
					goto outrun;
			}
		}
	}

	ast_indicate(chan, -1);

	if (ast_set_write_format(chan, AST_FORMAT_SLINEAR) < 0) {
		ast_log(LOG_WARNING, "Unable to set '%s' to write linear mode\n", chan->name);
		goto outrun;
	}

	if (ast_set_read_format(chan, AST_FORMAT_SLINEAR) < 0) {
		ast_log(LOG_WARNING, "Unable to set '%s' to read linear mode\n", chan->name);
		goto outrun;
	}

	retryzap = strcasecmp(chan->tech->type, "Zap");
	user->zapchannel = !retryzap;

 zapretry:
	origfd = chan->fds[0];
	if (retryzap) {
		fd = open("/dev/zap/pseudo", O_RDWR);
		if (fd < 0) {
			ast_log(LOG_WARNING, "Unable to open pseudo channel: %s\n", strerror(errno));
			goto outrun;
		}
		using_pseudo = 1;
		/* Make non-blocking */
		flags = fcntl(fd, F_GETFL);
		if (flags < 0) {
			ast_log(LOG_WARNING, "Unable to get flags: %s\n", strerror(errno));
			close(fd);
			goto outrun;
		}
		if (fcntl(fd, F_SETFL, flags | O_NONBLOCK)) {
			ast_log(LOG_WARNING, "Unable to set flags: %s\n", strerror(errno));
			close(fd);
			goto outrun;
		}
		/* Setup buffering information */
		memset(&bi, 0, sizeof(bi));
		bi.bufsize = CONF_SIZE/2;
		bi.txbufpolicy = ZT_POLICY_IMMEDIATE;
		bi.rxbufpolicy = ZT_POLICY_IMMEDIATE;
		bi.numbufs = audio_buffers;
		if (ioctl(fd, ZT_SET_BUFINFO, &bi)) {
			ast_log(LOG_WARNING, "Unable to set buffering information: %s\n", strerror(errno));
			close(fd);
			goto outrun;
		}
		x = 1;
		if (ioctl(fd, ZT_SETLINEAR, &x)) {
			ast_log(LOG_WARNING, "Unable to set linear mode: %s\n", strerror(errno));
			close(fd);
			goto outrun;
		}
		nfds = 1;
	} else {
		/* XXX Make sure we're not running on a pseudo channel XXX */
		fd = chan->fds[0];
		nfds = 0;
	}
	memset(&ztc, 0, sizeof(ztc));
	memset(&ztc_empty, 0, sizeof(ztc_empty));
	/* Check to see if we're in a conference... */
	ztc.chan = 0;	
	if (ioctl(fd, ZT_GETCONF, &ztc)) {
		ast_log(LOG_WARNING, "Error getting conference\n");
		close(fd);
		goto outrun;
	}
	if (ztc.confmode) {
		/* Whoa, already in a conference...  Retry... */
		if (!retryzap) {
			ast_log(LOG_DEBUG, "Zap channel is in a conference already, retrying with pseudo\n");
			retryzap = 1;
			goto zapretry;
		}
	}
	memset(&ztc, 0, sizeof(ztc));
	/* Add us to the conference */
	ztc.chan = 0;	
	ztc.confno = conf->zapconf;

	ast_mutex_lock(&conf->playlock);

	if (!(confflags & CONFFLAG_QUIET) && (confflags & CONFFLAG_INTROUSER) && conf->users > 1) {
		if (conf->chan && ast_fileexists(user->namerecloc, NULL, NULL)) {
			if (!ast_streamfile(conf->chan, user->namerecloc, chan->language))
				ast_waitstream(conf->chan, "");
			if (!ast_streamfile(conf->chan, "conf-hasjoin", chan->language))
				ast_waitstream(conf->chan, "");
		}
	}

	if (confflags & CONFFLAG_MONITOR)
		ztc.confmode = ZT_CONF_CONFMON | ZT_CONF_LISTENER;
	else if (confflags & CONFFLAG_TALKER)
		ztc.confmode = ZT_CONF_CONF | ZT_CONF_TALKER;
	else 
		ztc.confmode = ZT_CONF_CONF | ZT_CONF_TALKER | ZT_CONF_LISTENER;

	if (ioctl(fd, ZT_SETCONF, &ztc)) {
		ast_log(LOG_WARNING, "Error setting conference\n");
		close(fd);
		ast_mutex_unlock(&conf->playlock);
		goto outrun;
	}
	ast_log(LOG_DEBUG, "Placed channel %s in ZAP conf %d\n", chan->name, conf->zapconf);

	manager_event(EVENT_FLAG_CALL, "MeetmeJoin", 
		      "Channel: %s\r\n"
		      "Uniqueid: %s\r\n"
		      "Meetme: %s\r\n"
		      "Usernum: %d\r\n",
		      chan->name, chan->uniqueid, conf->confno, user->user_no);

	if (!firstpass && !(confflags & CONFFLAG_MONITOR) && !(confflags & CONFFLAG_ADMIN)) {
		firstpass = 1;
		if (!(confflags & CONFFLAG_QUIET))
			if (!(confflags & CONFFLAG_WAITMARKED) || (conf->markedusers >= 1))
				conf_play(chan, conf, ENTER);
	}

	ast_mutex_unlock(&conf->playlock);

	conf_flush(fd, chan);

	if (confflags & CONFFLAG_AGI) {
		/* Get name of AGI file to run from $(MEETME_AGI_BACKGROUND)
		   or use default filename of conf-background.agi */

		agifile = pbx_builtin_getvar_helper(chan, "MEETME_AGI_BACKGROUND");
		if (!agifile)
			agifile = agifiledefault;

		if (user->zapchannel) {
			/*  Set CONFMUTE mode on Zap channel to mute DTMF tones */
			x = 1;
			ast_channel_setoption(chan, AST_OPTION_TONE_VERIFY, &x, sizeof(char), 0);
		}
		/* Find a pointer to the agi app and execute the script */
		app = pbx_findapp("agi");
		if (app) {
			char *s = ast_strdupa(agifile);
			ret = pbx_exec(chan, app, s, 1);
		} else {
			ast_log(LOG_WARNING, "Could not find application (agi)\n");
			ret = -2;
		}
		if (user->zapchannel) {
			/*  Remove CONFMUTE mode on Zap channel */
			x = 0;
			ast_channel_setoption(chan, AST_OPTION_TONE_VERIFY, &x, sizeof(char), 0);
		}
	} else {
		if (user->zapchannel && (confflags & CONFFLAG_STARMENU)) {
			/*  Set CONFMUTE mode on Zap channel to mute DTMF tones when the menu is enabled */
			x = 1;
			ast_channel_setoption(chan, AST_OPTION_TONE_VERIFY, &x, sizeof(char), 0);
		}	
		if (confflags & (CONFFLAG_MONITORTALKER | CONFFLAG_OPTIMIZETALKER) && !(dsp = ast_dsp_new())) {
			ast_log(LOG_WARNING, "Unable to allocate DSP!\n");
			res = -1;
		}
		for(;;) {
			int menu_was_active = 0;

			outfd = -1;
			ms = -1;
			
			/* if we have just exited from the menu, and the user had a channel-driver
			   volume adjustment, restore it
			*/
			if (!menu_active && menu_was_active && user->listen.desired && !user->listen.actual)
				set_talk_volume(user, user->listen.desired);

			menu_was_active = menu_active;

			currentmarked = conf->markedusers;
			if (!(confflags & CONFFLAG_QUIET) &&
			    (confflags & CONFFLAG_MARKEDUSER) &&
			    (confflags & CONFFLAG_WAITMARKED) &&
			    lastmarked == 0) {
				if (currentmarked == 1 && conf->users > 1) {
					ast_say_number(chan, conf->users - 1, AST_DIGIT_ANY, chan->language, (char *) NULL);
					if (conf->users - 1 == 1) {
						if (!ast_streamfile(chan, "conf-userwilljoin", chan->language))
							ast_waitstream(chan, "");
					} else {
						if (!ast_streamfile(chan, "conf-userswilljoin", chan->language))
							ast_waitstream(chan, "");
					}
				}
				if (conf->users == 1 && ! (confflags & CONFFLAG_MARKEDUSER))
					if (!ast_streamfile(chan, "conf-onlyperson", chan->language))
						ast_waitstream(chan, "");
			}

			c = ast_waitfor_nandfds(&chan, 1, &fd, nfds, NULL, &outfd, &ms);
			
			
			/* Update the struct with the actual confflags */
			user->userflags = confflags;
			
			if (confflags & CONFFLAG_WAITMARKED) {
				if(currentmarked == 0) {
					if (lastmarked != 0) {
						if (!(confflags & CONFFLAG_QUIET))
							if (!ast_streamfile(chan, "conf-leaderhasleft", chan->language))
								ast_waitstream(chan, "");
						if(confflags & CONFFLAG_MARKEDEXIT)
							break;
						else {
							ztc.confmode = ZT_CONF_CONF;
							if (ioctl(fd, ZT_SETCONF, &ztc)) {
								ast_log(LOG_WARNING, "Error setting conference\n");
								close(fd);
								goto outrun;
							}
						}
					}
					if (musiconhold == 0 && (confflags & CONFFLAG_MOH)) {
						ast_moh_start(chan, NULL);
						musiconhold = 1;
					} else {
						ztc.confmode = ZT_CONF_CONF;
						if (ioctl(fd, ZT_SETCONF, &ztc)) {
							ast_log(LOG_WARNING, "Error setting conference\n");
							close(fd);
							goto outrun;
						}
					}
				} else if(currentmarked >= 1 && lastmarked == 0) {
					if (confflags & CONFFLAG_MONITOR)
						ztc.confmode = ZT_CONF_CONFMON | ZT_CONF_LISTENER;
					else if (confflags & CONFFLAG_TALKER)
						ztc.confmode = ZT_CONF_CONF | ZT_CONF_TALKER;
					else
						ztc.confmode = ZT_CONF_CONF | ZT_CONF_TALKER | ZT_CONF_LISTENER;
					if (ioctl(fd, ZT_SETCONF, &ztc)) {
						ast_log(LOG_WARNING, "Error setting conference\n");
						close(fd);
						goto outrun;
					}
					if (musiconhold && (confflags & CONFFLAG_MOH)) {
						ast_moh_stop(chan);
						musiconhold = 0;
					}
					if ( !(confflags & CONFFLAG_QUIET) && !(confflags & CONFFLAG_MARKEDUSER)) {
						if (!ast_streamfile(chan, "conf-placeintoconf", chan->language))
							ast_waitstream(chan, "");
						conf_play(chan, conf, ENTER);
					}
				}
			}

			/* trying to add moh for single person conf */
			if ((confflags & CONFFLAG_MOH) && !(confflags & CONFFLAG_WAITMARKED)) {
				if (conf->users == 1) {
					if (musiconhold == 0) {
						ast_moh_start(chan, NULL);
						musiconhold = 1;
					} 
				} else {
					if (musiconhold) {
						ast_moh_stop(chan);
						musiconhold = 0;
					}
				}
			}
			
			/* Leave if the last marked user left */
			if (currentmarked == 0 && lastmarked != 0 && (confflags & CONFFLAG_MARKEDEXIT)) {
				ret = -1;
				break;
			}
	
			/* Check if the admin changed my modes */
			if (user->adminflags) {			
				/* Set the new modes */
				if ((user->adminflags & ADMINFLAG_MUTED) && (ztc.confmode & ZT_CONF_TALKER)) {
					ztc.confmode ^= ZT_CONF_TALKER;
					if (ioctl(fd, ZT_SETCONF, &ztc)) {
						ast_log(LOG_WARNING, "Error setting conference - Un/Mute \n");
						ret = -1;
						break;
					}
				}
				if (!(user->adminflags & ADMINFLAG_MUTED) && !(confflags & CONFFLAG_MONITOR) && !(ztc.confmode & ZT_CONF_TALKER)) {
					ztc.confmode |= ZT_CONF_TALKER;
					if (ioctl(fd, ZT_SETCONF, &ztc)) {
						ast_log(LOG_WARNING, "Error setting conference - Un/Mute \n");
						ret = -1;
						break;
					}
				}
				if (user->adminflags & ADMINFLAG_KICKME) {
					/* You have been kicked. */
					if (!ast_streamfile(chan, "conf-kicked", chan->language))
						ast_waitstream(chan, "");
					ret = 0;
					break;
				}
			} else if (!(confflags & CONFFLAG_MONITOR) && !(ztc.confmode & ZT_CONF_TALKER)) {
				ztc.confmode |= ZT_CONF_TALKER;
				if (ioctl(fd, ZT_SETCONF, &ztc)) {
					ast_log(LOG_WARNING, "Error setting conference - Un/Mute \n");
					ret = -1;
					break;
				}
			}

			if (c) {
				if (c->fds[0] != origfd) {
					if (using_pseudo) {
						/* Kill old pseudo */
						close(fd);
						using_pseudo = 0;
					}
					ast_log(LOG_DEBUG, "Ooh, something swapped out under us, starting over\n");
					retryzap = strcasecmp(c->tech->type, "Zap");
					user->zapchannel = !retryzap;
					goto zapretry;
				}
				if ((confflags & CONFFLAG_MONITOR) || (user->adminflags & ADMINFLAG_MUTED))
					f = ast_read_noaudio(c);
				else
					f = ast_read(c);
				if (!f)
					break;
				if ((f->frametype == AST_FRAME_VOICE) && (f->subclass == AST_FORMAT_SLINEAR)) {
					if (user->talk.actual)
						ast_frame_adjust_volume(f, user->talk.actual);

					if (confflags & (CONFFLAG_MONITORTALKER | CONFFLAG_OPTIMIZETALKER)) {
						int totalsilence;

						if (user->talking == -1)
							user->talking = 0;

						res = ast_dsp_silence(dsp, f, &totalsilence);
						if (!user->talking && totalsilence < MEETME_DELAYDETECTTALK) {
							user->talking = 1;
							if (confflags & CONFFLAG_MONITORTALKER)
								manager_event(EVENT_FLAG_CALL, "MeetmeTalking",
								      "Channel: %s\r\n"
								      "Uniqueid: %s\r\n"
								      "Meetme: %s\r\n"
								      "Usernum: %d\r\n",
								      chan->name, chan->uniqueid, conf->confno, user->user_no);
						}
						if (user->talking && totalsilence > MEETME_DELAYDETECTENDTALK) {
							user->talking = 0;
							if (confflags & CONFFLAG_MONITORTALKER)
								manager_event(EVENT_FLAG_CALL, "MeetmeStopTalking",
								      "Channel: %s\r\n"
								      "Uniqueid: %s\r\n"
								      "Meetme: %s\r\n"
								      "Usernum: %d\r\n",
								      chan->name, chan->uniqueid, conf->confno, user->user_no);
						}
					}
					if (using_pseudo) {
						/* Absolutely do _not_ use careful_write here...
						   it is important that we read data from the channel
						   as fast as it arrives, and feed it into the conference.
						   The buffering in the pseudo channel will take care of any
						   timing differences, unless they are so drastic as to lose
						   audio frames (in which case carefully writing would only
						   have delayed the audio even further).
						*/
						/* As it turns out, we do want to use careful write.  We just
						   don't want to block, but we do want to at least *try*
						   to write out all the samples.
						 */
						if (user->talking || !(confflags & CONFFLAG_OPTIMIZETALKER))
							careful_write(fd, f->data, f->datalen, 0);
					}
				} else if ((f->frametype == AST_FRAME_DTMF) && (confflags & CONFFLAG_EXIT_CONTEXT)) {
					char tmp[2];

					tmp[0] = f->subclass;
					tmp[1] = '\0';
					if (!ast_goto_if_exists(chan, exitcontext, tmp, 1)) {
						ast_log(LOG_DEBUG, "Got DTMF %c, goto context %s\n", tmp[0], exitcontext);
						ret = 0;
						break;
					} else if (option_debug > 1)
						ast_log(LOG_DEBUG, "Exit by single digit did not work in meetme. Extension %s does not exist in context %s\n", tmp, exitcontext);
				} else if ((f->frametype == AST_FRAME_DTMF) && (f->subclass == '#') && (confflags & CONFFLAG_POUNDEXIT)) {
					ret = 0;
					break;
				} else if (((f->frametype == AST_FRAME_DTMF) && (f->subclass == '*') && (confflags & CONFFLAG_STARMENU)) || ((f->frametype == AST_FRAME_DTMF) && menu_active)) {
					if (ioctl(fd, ZT_SETCONF, &ztc_empty)) {
						ast_log(LOG_WARNING, "Error setting conference\n");
						close(fd);
						goto outrun;
					}

					/* if we are entering the menu, and the user has a channel-driver
					   volume adjustment, clear it
					*/
					if (!menu_active && user->talk.desired && !user->talk.actual)
						set_talk_volume(user, 0);

					if (musiconhold) {
			   			ast_moh_stop(chan);
					}
					if ((confflags & CONFFLAG_ADMIN)) {
						/* Admin menu */
						if (!menu_active) {
							menu_active = 1;
							/* Record this sound! */
							if (!ast_streamfile(chan, "conf-adminmenu", chan->language)) {
								dtmf = ast_waitstream(chan, AST_DIGIT_ANY);
								ast_stopstream(chan);
							} else 
								dtmf = 0;
						} else 
							dtmf = f->subclass;
						if (dtmf) {
							switch(dtmf) {
							case '1': /* Un/Mute */
								menu_active = 0;
								if (ztc.confmode & ZT_CONF_TALKER) {
	 						       		ztc.confmode = ZT_CONF_CONF | ZT_CONF_LISTENER;
	 						       		confflags |= CONFFLAG_MONITOR ^ CONFFLAG_TALKER;
								} else {
									ztc.confmode = ZT_CONF_CONF | ZT_CONF_TALKER | ZT_CONF_LISTENER;
									confflags ^= CONFFLAG_MONITOR | CONFFLAG_TALKER;
								}
								if (ioctl(fd, ZT_SETCONF, &ztc)) {
									ast_log(LOG_WARNING, "Error setting conference - Un/Mute \n");
									ret = -1;
									break;
								}
								if (ztc.confmode & ZT_CONF_TALKER) {
									if (!ast_streamfile(chan, "conf-unmuted", chan->language))
										ast_waitstream(chan, "");
								} else {
									if (!ast_streamfile(chan, "conf-muted", chan->language))
										ast_waitstream(chan, "");
								}
								break;
							case '2': /* Un/Lock the Conference */
								menu_active = 0;
								if (conf->locked) {
									conf->locked = 0;
									if (!ast_streamfile(chan, "conf-unlockednow", chan->language))
										ast_waitstream(chan, "");
								} else {
									conf->locked = 1;
									if (!ast_streamfile(chan, "conf-lockednow", chan->language))
										ast_waitstream(chan, "");
								}
								break;
							case '3': /* Eject last user */
								menu_active = 0;
								usr = conf->lastuser;
								if ((usr->chan->name == chan->name)||(usr->userflags & CONFFLAG_ADMIN)) {
									if(!ast_streamfile(chan, "conf-errormenu", chan->language))
										ast_waitstream(chan, "");
								} else 
									usr->adminflags |= ADMINFLAG_KICKME;
								ast_stopstream(chan);
								break;	
							case '4':
								tweak_listen_volume(user, VOL_DOWN);
								break;
							case '6':
								tweak_listen_volume(user, VOL_UP);
								break;
							case '7':
								tweak_talk_volume(user, VOL_DOWN);
								break;
							case '8':
								menu_active = 0;
								break;
							case '9':
								tweak_talk_volume(user, VOL_UP);
								break;
							default:
								menu_active = 0;
								/* Play an error message! */
								if (!ast_streamfile(chan, "conf-errormenu", chan->language))
									ast_waitstream(chan, "");
								break;
							}
						}
					} else {
						/* User menu */
						if (!menu_active) {
							menu_active = 1;
							if (!ast_streamfile(chan, "conf-usermenu", chan->language)) {
								dtmf = ast_waitstream(chan, AST_DIGIT_ANY);
								ast_stopstream(chan);
							} else
								dtmf = 0;
						} else 
							dtmf = f->subclass;
						if (dtmf) {
							switch(dtmf) {
							case '1': /* Un/Mute */
								menu_active = 0;
								if (ztc.confmode & ZT_CONF_TALKER) {
	 						       		ztc.confmode = ZT_CONF_CONF | ZT_CONF_LISTENER;
	 						       		confflags |= CONFFLAG_MONITOR ^ CONFFLAG_TALKER;
								} else if (!(user->adminflags & ADMINFLAG_MUTED)) {
									ztc.confmode = ZT_CONF_CONF | ZT_CONF_TALKER | ZT_CONF_LISTENER;
									confflags ^= CONFFLAG_MONITOR | CONFFLAG_TALKER;
								}
								if (ioctl(fd, ZT_SETCONF, &ztc)) {
									ast_log(LOG_WARNING, "Error setting conference - Un/Mute \n");
									ret = -1;
									break;
								}
								if (ztc.confmode & ZT_CONF_TALKER) {
									if (!ast_streamfile(chan, "conf-unmuted", chan->language))
										ast_waitstream(chan, "");
								} else {
									if (!ast_streamfile(chan, "conf-muted", chan->language))
										ast_waitstream(chan, "");
								}
								break;
							case '4':
								tweak_listen_volume(user, VOL_DOWN);
								break;
							case '6':
								tweak_listen_volume(user, VOL_UP);
								break;
							case '7':
								tweak_talk_volume(user, VOL_DOWN);
								break;
							case '8':
								menu_active = 0;
								break;
							case '9':
								tweak_talk_volume(user, VOL_UP);
								break;
							default:
								menu_active = 0;
								if (!ast_streamfile(chan, "conf-errormenu", chan->language))
									ast_waitstream(chan, "");
								break;
							}
						}
					}
					if (musiconhold)
			   			ast_moh_start(chan, NULL);

					if (ioctl(fd, ZT_SETCONF, &ztc)) {
						ast_log(LOG_WARNING, "Error setting conference\n");
						close(fd);
						AST_LIST_UNLOCK(&confs);
						goto outrun;
					}

					conf_flush(fd, chan);
				} else if (option_debug) {
					ast_log(LOG_DEBUG,
						"Got unrecognized frame on channel %s, f->frametype=%d,f->subclass=%d\n",
						chan->name, f->frametype, f->subclass);
				}
				ast_frfree(f);
			} else if (outfd > -1) {
				res = read(outfd, buf, CONF_SIZE);
				if (res > 0) {
					memset(&fr, 0, sizeof(fr));
					fr.frametype = AST_FRAME_VOICE;
					fr.subclass = AST_FORMAT_SLINEAR;
					fr.datalen = res;
					fr.samples = res/2;
					fr.data = buf;
					fr.offset = AST_FRIENDLY_OFFSET;
					if (!user->listen.actual && 
						((confflags & CONFFLAG_MONITOR) || 
						 (user->adminflags & ADMINFLAG_MUTED) ||
						 (!user->talking && (confflags & CONFFLAG_OPTIMIZETALKER))
						 )) {
						int index;
						for (index=0;index<AST_FRAME_BITS;index++)
							if (chan->rawwriteformat & (1 << index))
								break;
						if (index >= AST_FRAME_BITS)
							goto bailoutandtrynormal;
						ast_mutex_lock(&conf->listenlock);
						if (!conf->transframe[index]) {
							if (conf->origframe) {
								if (!conf->transpath[index])
									conf->transpath[index] = ast_translator_build_path((1 << index), AST_FORMAT_SLINEAR);
								if (conf->transpath[index]) {
									conf->transframe[index] = ast_translate(conf->transpath[index], conf->origframe, 0);
									if (!conf->transframe[index])
										conf->transframe[index] = &ast_null_frame;
								}
							}
						}
						if (conf->transframe[index]) {
 							if (conf->transframe[index]->frametype != AST_FRAME_NULL) {
	 							if (ast_write(chan, conf->transframe[index]))
									ast_log(LOG_WARNING, "Unable to write frame to channel: %s\n", strerror(errno));
							}
						} else {
							ast_mutex_unlock(&conf->listenlock);
							goto bailoutandtrynormal;
						}
						ast_mutex_unlock(&conf->listenlock);
					} else {
bailoutandtrynormal:					
						if (user->listen.actual)
							ast_frame_adjust_volume(&fr, user->listen.actual);
						if (ast_write(chan, &fr) < 0) {
							ast_log(LOG_WARNING, "Unable to write frame to channel: %s\n", strerror(errno));
						}
					}
				} else 
					ast_log(LOG_WARNING, "Failed to read frame: %s\n", strerror(errno));
			}
			lastmarked = currentmarked;
		}
	}

	if (musiconhold)
		ast_moh_stop(chan);
	
	if (using_pseudo)
		close(fd);
	else {
		/* Take out of conference */
		ztc.chan = 0;	
		ztc.confno = 0;
		ztc.confmode = 0;
		if (ioctl(fd, ZT_SETCONF, &ztc)) {
			ast_log(LOG_WARNING, "Error setting conference\n");
		}
	}

	reset_volumes(user);

	AST_LIST_LOCK(&confs);
	if (!(confflags & CONFFLAG_QUIET) && !(confflags & CONFFLAG_MONITOR) && !(confflags & CONFFLAG_ADMIN))
		conf_play(chan, conf, LEAVE);

	if (!(confflags & CONFFLAG_QUIET) && (confflags & CONFFLAG_INTROUSER)) {
		if (ast_fileexists(user->namerecloc, NULL, NULL)) {
			if ((conf->chan) && (conf->users > 1)) {
				if (!ast_streamfile(conf->chan, user->namerecloc, chan->language))
					ast_waitstream(conf->chan, "");
				if (!ast_streamfile(conf->chan, "conf-hasleft", chan->language))
					ast_waitstream(conf->chan, "");
			}
			ast_filedelete(user->namerecloc, NULL);
		}
	}
	AST_LIST_UNLOCK(&confs);

 outrun:
	AST_LIST_LOCK(&confs);

	if (dsp)
		ast_dsp_free(dsp);
	
	if (user->user_no) { /* Only cleanup users who really joined! */
		now = time(NULL);
		hr = (now - user->jointime) / 3600;
		min = ((now - user->jointime) % 3600) / 60;
		sec = (now - user->jointime) % 60;

		manager_event(EVENT_FLAG_CALL, "MeetmeLeave",
			"Channel: %s\r\n"
			"Uniqueid: %s\r\n"
			"Meetme: %s\r\n"
			"Usernum: %d\r\n"
		        "CIDnum: %s\r\n"
			"CIDname: %s\r\n"
		        "Duration: %02d:%02d:%02d\r\n",
			 chan->name, chan->uniqueid, conf->confno, 
			  user->user_no,
			  user->chan->cid.cid_num ? user->chan->cid.cid_num :
		         "<unknown>",
		         user->chan->cid.cid_name ? user->chan->cid.cid_name :
		          "<no name>", hr, min, sec);

		conf->users--;
		conf->refcount--;
		/* Update table */
		snprintf(members, sizeof(members), "%d", conf->users);
		ast_update_realtime("meetme", "confno", conf->confno, "members", members, NULL);
		if (confflags & CONFFLAG_MARKEDUSER) 
			conf->markedusers--;
		if (!conf->users) {
			/* close this one when no more users and no references*/
			if (!conf->refcount){
				conf_free(conf);
			}
		} else {
			/* Remove the user struct */ 
			if (user == conf->firstuser) {
				if (user->nextuser) {
					/* There is another entry */
					user->nextuser->prevuser = NULL;
				} else {
					/* We are the only entry */
					conf->lastuser = NULL;
				}
				/* In either case */
				conf->firstuser = user->nextuser;
			} else if (user == conf->lastuser){
				if (user->prevuser)
					user->prevuser->nextuser = NULL;
				else
					ast_log(LOG_ERROR, "Bad bad bad!  We're the last, not the first, but nobody before us??\n");
				conf->lastuser = user->prevuser;
			} else {
				if (user->nextuser)
					user->nextuser->prevuser = user->prevuser;
				else
					ast_log(LOG_ERROR, "Bad! Bad! Bad! user->nextuser is NULL but we're not the end!\n");
				if (user->prevuser)
					user->prevuser->nextuser = user->nextuser;
				else
					ast_log(LOG_ERROR, "Bad! Bad! Bad! user->prevuser is NULL but we're not the beginning!\n");
			}
		}
		/* Return the number of seconds the user was in the conf */
		snprintf(meetmesecs, sizeof(meetmesecs), "%d", (int) (time(NULL) - user->jointime));
		pbx_builtin_setvar_helper(chan, "MEETMESECS", meetmesecs);
	}
	free(user);
	AST_LIST_UNLOCK(&confs);

	return ret;
}

/*
  This function looks for a conference via the RealTime module
*/
static struct ast_conference *find_conf_realtime(struct ast_channel *chan, char *confno, int make, int dynamic, char *dynamic_pin, int refcount)
{

	struct ast_variable *var;
	struct ast_conference *cnf;

	/* Check first in the conference list */
	AST_LIST_LOCK(&confs);
	AST_LIST_TRAVERSE(&confs, cnf, list) {
		if (!strcmp(confno, cnf->confno)) 
			break;
	}
	if (cnf){
		cnf->refcount += refcount;
	}
	AST_LIST_UNLOCK(&confs);

	if (!cnf) {
		char *pin = NULL, *pinadmin = NULL; /* For temp use */

		cnf = ast_calloc(1, sizeof(struct ast_conference));
		if (!cnf) {
			ast_log(LOG_ERROR, "Out of memory\n");
			return NULL;
		}

		var = ast_load_realtime("meetme", "confno", confno, NULL);
		while (var) {
			if (!strcasecmp(var->name, "confno")) {
				ast_copy_string(cnf->confno, var->value, sizeof(cnf->confno));
			} else if (!strcasecmp(var->name, "pin")) {
				pin = ast_strdupa(var->value);
			} else if (!strcasecmp(var->name, "adminpin")) {
				pinadmin = ast_strdupa(var->value);
			}
			var = var->next;
		}
		ast_variables_destroy(var);

		cnf = build_conf(confno, pin ? pin : "", pinadmin ? pinadmin : "", make, dynamic, refcount);
	}

	return cnf;
}


static struct ast_conference *find_conf(struct ast_channel *chan, char *confno, int make, int dynamic, char *dynamic_pin, int refcount)
{
	struct ast_config *cfg;
	struct ast_variable *var;
	struct ast_conference *cnf;
	char *parse;
	AST_DECLARE_APP_ARGS(args,
		AST_APP_ARG(confno);
		AST_APP_ARG(pin);
		AST_APP_ARG(pinadmin);
	);

	/* Check first in the conference list */
	AST_LIST_LOCK(&confs);
	AST_LIST_TRAVERSE(&confs, cnf, list) {
		if (!strcmp(confno, cnf->confno)) 
			break;
	}
	if (cnf){
		cnf->refcount += refcount;
	}
	AST_LIST_UNLOCK(&confs);

	if (!cnf) {
		if (dynamic) {
			/* No need to parse meetme.conf */
			ast_log(LOG_DEBUG, "Building dynamic conference '%s'\n", confno);
			if (dynamic_pin) {
				if (dynamic_pin[0] == 'q') {
					/* Query the user to enter a PIN */
					if (ast_app_getdata(chan, "conf-getpin", dynamic_pin, AST_MAX_EXTENSION - 1, 0) < 0)
						return NULL;
				}
				cnf = build_conf(confno, dynamic_pin, "", make, dynamic, refcount);
			} else {
				cnf = build_conf(confno, "", "", make, dynamic, refcount);
			}
		} else {
			/* Check the config */
			cfg = ast_config_load(CONFIG_FILE_NAME);
			if (!cfg) {
				ast_log(LOG_WARNING, "No %s file :(\n", CONFIG_FILE_NAME);
				return NULL;
			}
			for (var = ast_variable_browse(cfg, "rooms"); var; var = var->next) {
				if (strcasecmp(var->name, "conf"))
					continue;
				
				if (!(parse = ast_strdupa(var->value)))
					return NULL;
				
				AST_NONSTANDARD_APP_ARGS(args, parse, ',');
				if (!strcasecmp(args.confno, confno)) {
					/* Bingo it's a valid conference */
					cnf = build_conf(args.confno,
							S_OR(args.pin, ""),
							S_OR(args.pinadmin, ""),
							make, dynamic, refcount);
					break;
				}
			}
			if (!var) {
				ast_log(LOG_DEBUG, "%s isn't a valid conference\n", confno);
			}
			ast_config_destroy(cfg);
		}
	} else if (dynamic_pin) {
		/* Correct for the user selecting 'D' instead of 'd' to have
		   someone join into a conference that has already been created
		   with a pin. */
		if (dynamic_pin[0] == 'q')
			dynamic_pin[0] = '\0';
	}

	return cnf;
}

/*! \brief The MeetmeCount application */
static int count_exec(struct ast_channel *chan, void *data)
{
	struct localuser *u;
	int res = 0;
	struct ast_conference *conf;
	int count;
	char *localdata;
	char val[80] = "0"; 
	AST_DECLARE_APP_ARGS(args,
		AST_APP_ARG(confno);
		AST_APP_ARG(varname);
	);

	if (ast_strlen_zero(data)) {
		ast_log(LOG_WARNING, "MeetMeCount requires an argument (conference number)\n");
		return -1;
	}

	LOCAL_USER_ADD(u);
	
	if (!(localdata = ast_strdupa(data))) {
		LOCAL_USER_REMOVE(u);
		return -1;
	}

	AST_STANDARD_APP_ARGS(args, localdata);
	
	conf = find_conf(chan, args.confno, 0, 0, NULL, 0);
	if (conf)
		count = conf->users;
	else
		count = 0;

	if (!ast_strlen_zero(args.varname)){
		/* have var so load it and exit */
		snprintf(val, sizeof(val), "%d",count);
		pbx_builtin_setvar_helper(chan, args.varname, val);
	} else {
		if (chan->_state != AST_STATE_UP)
			ast_answer(chan);
		res = ast_say_number(chan, count, "", chan->language, (char *) NULL); /* Needs gender */
	}
	LOCAL_USER_REMOVE(u);

	return res;
}

/*! \brief The meetme() application */
static int conf_exec(struct ast_channel *chan, void *data)
{
	int res=-1;
	struct localuser *u;
	char confno[AST_MAX_EXTENSION] = "";
	int allowretry = 0;
	int retrycnt = 0;
	struct ast_conference *cnf;
	struct ast_flags confflags = {0};
	int dynamic = 0;
	int empty = 0, empty_no_pin = 0;
	int always_prompt = 0;
	char *notdata, *info, the_pin[AST_MAX_EXTENSION] = "";
	AST_DECLARE_APP_ARGS(args,
		AST_APP_ARG(confno);
		AST_APP_ARG(options);
		AST_APP_ARG(pin);
	);

	LOCAL_USER_ADD(u);

	if (ast_strlen_zero(data)) {
		allowretry = 1;
		notdata = "";
	} else {
		notdata = data;
	}
	
	if (chan->_state != AST_STATE_UP)
		ast_answer(chan);

	info = ast_strdupa(notdata);

	AST_STANDARD_APP_ARGS(args, info);	

	if (args.confno) {
		ast_copy_string(confno, args.confno, sizeof(confno));
		if (ast_strlen_zero(confno)) {
			allowretry = 1;
		}
	}
	
	if (args.pin)
		ast_copy_string(the_pin, args.pin, sizeof(the_pin));

	if (args.options) {
		ast_app_parse_options(meetme_opts, &confflags, NULL, args.options);
		dynamic = ast_test_flag(&confflags, CONFFLAG_DYNAMIC | CONFFLAG_DYNAMICPIN);
		if (ast_test_flag(&confflags, CONFFLAG_DYNAMICPIN) && !args.pin)
			strcpy(the_pin, "q");

		empty = ast_test_flag(&confflags, CONFFLAG_EMPTY | CONFFLAG_EMPTYNOPIN);
		empty_no_pin = ast_test_flag(&confflags, CONFFLAG_EMPTYNOPIN);
		always_prompt = ast_test_flag(&confflags, CONFFLAG_ALWAYSPROMPT);
	}

	do {
		if (retrycnt > 3)
			allowretry = 0;
		if (empty) {
			int i, map[1024] = { 0, };
			struct ast_config *cfg;
			struct ast_variable *var;
			int confno_int;

			AST_LIST_LOCK(&confs);
			AST_LIST_TRAVERSE(&confs, cnf, list) {
				if (sscanf(cnf->confno, "%d", &confno_int) == 1) {
					/* Disqualify in use conference */
					if (confno_int >= 0 && confno_int < 1024)
						map[confno_int]++;
				}
			}
			AST_LIST_UNLOCK(&confs);

			/* We only need to load the config file for static and empty_no_pin (otherwise we don't care) */
			if ((empty_no_pin) || (!dynamic)) {
				cfg = ast_config_load(CONFIG_FILE_NAME);
				if (cfg) {
					var = ast_variable_browse(cfg, "rooms");
					while (var) {
						if (!strcasecmp(var->name, "conf")) {
							char *stringp = ast_strdupa(var->value);
							if (stringp) {
								char *confno_tmp = strsep(&stringp, "|,");
								int found = 0;
								if (sscanf(confno_tmp, "%d", &confno_int) == 1) {
									if ((confno_int >= 0) && (confno_int < 1024)) {
										if (stringp && empty_no_pin) {
											map[confno_int]++;
										}
									}
								}
								if (!dynamic) {
									/* For static:  run through the list and see if this conference is empty */
									AST_LIST_LOCK(&confs);
									AST_LIST_TRAVERSE(&confs, cnf, list) {
										if (!strcmp(confno_tmp, cnf->confno)) {
											/* The conference exists, therefore it's not empty */
											found = 1;
											break;
										}
									}
									AST_LIST_UNLOCK(&confs);
									if (!found) {
										/* At this point, we have a confno_tmp (static conference) that is empty */
										if ((empty_no_pin && ast_strlen_zero(stringp)) || (!empty_no_pin)) {
											/* Case 1:  empty_no_pin and pin is nonexistent (NULL)
											 * Case 2:  empty_no_pin and pin is blank (but not NULL)
											 * Case 3:  not empty_no_pin
											 */
											ast_copy_string(confno, confno_tmp, sizeof(confno));
											break;
											/* XXX the map is not complete (but we do have a confno) */
										}
									}
								}
							}
						}
						var = var->next;
					}
					ast_config_destroy(cfg);
				}
			}

			/* Select first conference number not in use */
			if (ast_strlen_zero(confno) && dynamic) {
				for (i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
					if (!map[i]) {
						snprintf(confno, sizeof(confno), "%d", i);
						break;
					}
				}
			}

			/* Not found? */
			if (ast_strlen_zero(confno)) {
				res = ast_streamfile(chan, "conf-noempty", chan->language);
				if (!res)
					ast_waitstream(chan, "");
			} else {
				if (sscanf(confno, "%d", &confno_int) == 1) {
					res = ast_streamfile(chan, "conf-enteringno", chan->language);
					if (!res) {
						ast_waitstream(chan, "");
						res = ast_say_digits(chan, confno_int, "", chan->language);
					}
				} else {
					ast_log(LOG_ERROR, "Could not scan confno '%s'\n", confno);
				}
			}
		}

		while (allowretry && (ast_strlen_zero(confno)) && (++retrycnt < 4)) {
			/* Prompt user for conference number */
			res = ast_app_getdata(chan, "conf-getconfno", confno, sizeof(confno) - 1, 0);
			if (res < 0) {
				/* Don't try to validate when we catch an error */
				confno[0] = '\0';
				allowretry = 0;
				break;
			}
		}
		if (!ast_strlen_zero(confno)) {
			/* Check the validity of the conference */
			cnf = find_conf(chan, confno, 1, dynamic, the_pin, 1);
			if (!cnf) {
				cnf = find_conf_realtime(chan, confno, 1, dynamic, the_pin, 1);
			}
			if (!cnf) {
				res = ast_streamfile(chan, "conf-invalid", chan->language);
				if (!res)
					ast_waitstream(chan, "");
				res = -1;
				if (allowretry)
					confno[0] = '\0';
			} else {
				if ((!ast_strlen_zero(cnf->pin) &&
				     !ast_test_flag(&confflags, CONFFLAG_ADMIN)) ||
				    (!ast_strlen_zero(cnf->pinadmin) &&
				     ast_test_flag(&confflags, CONFFLAG_ADMIN))) {
					char pin[AST_MAX_EXTENSION]="";
					int j;

					/* Allow the pin to be retried up to 3 times */
					for (j = 0; j < 3; j++) {
						if (*the_pin && (always_prompt == 0)) {
							ast_copy_string(pin, the_pin, sizeof(pin));
							res = 0;
						} else {
							/* Prompt user for pin if pin is required */
							res = ast_app_getdata(chan, "conf-getpin", pin + strlen(pin), sizeof(pin) - 1 - strlen(pin), 0);
						}
						if (res >= 0) {
							if (!strcasecmp(pin, cnf->pin) ||
							    (!ast_strlen_zero(cnf->pinadmin) &&
							     !strcasecmp(pin, cnf->pinadmin))) {
								/* Pin correct */
								allowretry = 0;
								if (!ast_strlen_zero(cnf->pinadmin) && !strcasecmp(pin, cnf->pinadmin)) 
									ast_set_flag(&confflags, CONFFLAG_ADMIN);
								/* Run the conference */
								res = conf_run(chan, cnf, confflags.flags);
								break;
							} else {
								/* Pin invalid */
								res = ast_streamfile(chan, "conf-invalidpin", chan->language);
								if (!res)
									ast_waitstream(chan, AST_DIGIT_ANY);
								if (res < 0) {
									AST_LIST_LOCK(&confs);
									cnf->refcount--;
									if (!cnf->refcount){
										conf_free(cnf);
									}
									AST_LIST_UNLOCK(&confs);
									break;
								}
								pin[0] = res;
								pin[1] = '\0';
								res = -1;
								if (allowretry)
									confno[0] = '\0';
							}
						} else {
							/* failed when getting the pin */
							res = -1;
							allowretry = 0;
							/* see if we need to get rid of the conference */
							AST_LIST_LOCK(&confs);
							cnf->refcount--;
							if (!cnf->refcount) {
								conf_free(cnf);
							}
							AST_LIST_UNLOCK(&confs);
							break;
						}

						/* Don't retry pin with a static pin */
						if (*the_pin && (always_prompt==0)) {
							break;
						}
					}
				} else {
					/* No pin required */
					allowretry = 0;

					/* Run the conference */
					res = conf_run(chan, cnf, confflags.flags);
				}
			}
		}
	} while (allowretry);
	
	LOCAL_USER_REMOVE(u);
	
	return res;
}

static struct ast_conf_user* find_user(struct ast_conference *conf, char *callerident) 
{
	struct ast_conf_user *user = NULL;
	int cid;
	
	sscanf(callerident, "%i", &cid);
	if (conf && callerident) {
		user = conf->firstuser;
		while (user) {
			if (cid == user->user_no)
				return user;
			user = user->nextuser;
		}
	}
	return NULL;
}

/*! \brief The MeetMeadmin application */
/* MeetMeAdmin(confno, command, caller) */
static int admin_exec(struct ast_channel *chan, void *data) {
	char *params;
	struct ast_conference *cnf;
	struct ast_conf_user *user = NULL;
	struct localuser *u;
	AST_DECLARE_APP_ARGS(args,
		AST_APP_ARG(confno);
		AST_APP_ARG(command);
		AST_APP_ARG(user);
	);
	
	LOCAL_USER_ADD(u);

	AST_LIST_LOCK(&confs);
	/* The param has the conference number the user and the command to execute */
	if (!ast_strlen_zero(data)) {		
		params = ast_strdupa((char *) data);

		AST_STANDARD_APP_ARGS(args, params);

		if (!args.command) {
			ast_log(LOG_WARNING, "MeetmeAdmin requires a command!\n");
			AST_LIST_UNLOCK(&confs);
			LOCAL_USER_REMOVE(u);
			return -1;
		}
		AST_LIST_TRAVERSE(&confs, cnf, list) {
			if (!strcmp(cnf->confno, args.confno))
				break;
		}
		
		if (args.user)
			user = find_user(cnf, args.user);
		
		if (cnf) {
			switch((int) (*args.command)) {
			case 76: /* L: Lock */ 
				cnf->locked = 1;
				break;
			case 108: /* l: Unlock */ 
				cnf->locked = 0;
				break;
			case 75: /* K: kick all users*/
				user = cnf->firstuser;
				while(user) {
					user->adminflags |= ADMINFLAG_KICKME;
					if (user->nextuser) {
						user = user->nextuser;
					} else {
						break;
					}
				}
				break;
			case 101: /* e: Eject last user*/
				user = cnf->lastuser;
				if (!(user->userflags & CONFFLAG_ADMIN)) {
					user->adminflags |= ADMINFLAG_KICKME;
					break;
				} else
					ast_log(LOG_NOTICE, "Not kicking last user, is an Admin!\n");
				break;
			case 77: /* M: Mute */ 
				if (user) {
					user->adminflags |= ADMINFLAG_MUTED;
				} else {
					ast_log(LOG_NOTICE, "Specified User not found!\n");
				}
				break;
			case 78: /* N: Mute all users */
				user = cnf->firstuser;
				while(user) {
					if (user && !(user->userflags & CONFFLAG_ADMIN))
						user->adminflags |= ADMINFLAG_MUTED;
					if (user->nextuser) {
						user = user->nextuser;
					} else {
						break;
					}
				}
				break;					
			case 109: /* m: Unmute */ 
				if (user && (user->adminflags & ADMINFLAG_MUTED)) {
					user->adminflags ^= ADMINFLAG_MUTED;
				} else {
					ast_log(LOG_NOTICE, "Specified User not found or he muted himself!\n");
				}
				break;
			case  110: /* n: Unmute all users */
				user = cnf->firstuser;
				while(user) {
					if (user && (user-> adminflags & ADMINFLAG_MUTED)) {
						user->adminflags ^= ADMINFLAG_MUTED;
					}
					if (user->nextuser) {
						user = user->nextuser;
					} else {
						break;
					}
				}
				break;
			case 107: /* k: Kick user */ 
				if (user) {
					user->adminflags |= ADMINFLAG_KICKME;
				} else {
					ast_log(LOG_NOTICE, "Specified User not found!");
				}
				break;
			}
		} else {
			ast_log(LOG_NOTICE, "Conference Number not found\n");
		}
	}
	AST_LIST_UNLOCK(&confs);

	LOCAL_USER_REMOVE(u);
	
	return 0;
}

static void *recordthread(void *args)
{
	struct ast_conference *cnf = args;
	struct ast_frame *f=NULL;
	int flags;
	struct ast_filestream *s=NULL;
	int res=0;
	int x;
	const char *oldrecordingfilename = NULL;

	if (!cnf || !cnf->lchan) {
		pthread_exit(0);
	}

	ast_stopstream(cnf->lchan);
	flags = O_CREAT|O_TRUNC|O_WRONLY;


	cnf->recording = MEETME_RECORD_ACTIVE;
	while (ast_waitfor(cnf->lchan, -1) > -1) {
		if (cnf->recording == MEETME_RECORD_TERMINATE) {
			AST_LIST_LOCK(&confs);
			AST_LIST_UNLOCK(&confs);
			break;
		}
		if (!s && cnf->recordingfilename && (cnf->recordingfilename != oldrecordingfilename)) {
			s = ast_writefile(cnf->recordingfilename, cnf->recordingformat, NULL, flags, 0, 0644);
			oldrecordingfilename = cnf->recordingfilename;
		}
		
		f = ast_read(cnf->lchan);
		if (!f) {
			res = -1;
			break;
		}
		if (f->frametype == AST_FRAME_VOICE) {
			ast_mutex_lock(&cnf->listenlock);
			for (x=0;x<AST_FRAME_BITS;x++) {
				/* Free any translations that have occured */
				if (cnf->transframe[x]) {
					ast_frfree(cnf->transframe[x]);
					cnf->transframe[x] = NULL;
				}
			}
			if (cnf->origframe)
				ast_frfree(cnf->origframe);
			cnf->origframe = f;
			ast_mutex_unlock(&cnf->listenlock);
			if (s)
				res = ast_writestream(s, f);
			if (res) {
				ast_frfree(f);
				break;
			}
		}
		ast_frfree(f);
	}
	cnf->recording = MEETME_RECORD_OFF;
	if (s)
		ast_closestream(s);
	
	pthread_exit(0);
}

static void load_config(void)
{
	struct ast_config *cfg;
	char *val;

	audio_buffers = DEFAULT_AUDIO_BUFFERS;

	if (!(cfg = ast_config_load(CONFIG_FILE_NAME)))
		return;

	if ((val = ast_variable_retrieve(cfg, "general", "audiobuffers"))) {
		if ((sscanf(val, "%d", &audio_buffers) != 1)) {
			ast_log(LOG_WARNING, "audiobuffers setting must be a number, not '%s'\n", val);
			audio_buffers = DEFAULT_AUDIO_BUFFERS;
		} else if ((audio_buffers < ZT_DEFAULT_NUM_BUFS) || (audio_buffers > ZT_MAX_NUM_BUFS)) {
			ast_log(LOG_WARNING, "audiobuffers setting must be between %d and %d\n",
				ZT_DEFAULT_NUM_BUFS, ZT_MAX_NUM_BUFS);
			audio_buffers = DEFAULT_AUDIO_BUFFERS;
		}
		if (audio_buffers != DEFAULT_AUDIO_BUFFERS)
			ast_log(LOG_NOTICE, "Audio buffers per channel set to %d\n", audio_buffers);
	}

	ast_config_destroy(cfg);
}

int unload_module(void)
{
	int res;
	
	res = ast_cli_unregister(&cli_show_confs);
	res |= ast_cli_unregister(&cli_conf);
	res |= ast_unregister_application(app3);
	res |= ast_unregister_application(app2);
	res |= ast_unregister_application(app);

	STANDARD_HANGUP_LOCALUSERS;

	return res;
}

int load_module(void)
{
	int res;

	load_config();

	res = ast_cli_register(&cli_show_confs);
	res |= ast_cli_register(&cli_conf);
	res |= ast_register_application(app3, admin_exec, synopsis3, descrip3);
	res |= ast_register_application(app2, count_exec, synopsis2, descrip2);
	res |= ast_register_application(app, conf_exec, synopsis, descrip);

	return res;
}

int reload(void)
{
	load_config();

	return 0;
}

char *description(void)
{
	return (char *) tdesc;
}

int usecount(void)
{
	int res;

	STANDARD_USECOUNT(res);

	return res;
}

char *key()
{
	return ASTERISK_GPL_KEY;
}
 
