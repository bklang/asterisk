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
 * \brief Channel Management
 *
 * \author Mark Spencer <markster@digium.com>
 */

#include "asterisk.h"

ASTERISK_FILE_VERSION(__FILE__, "$Revision$")

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <sys/time.h>
#include <signal.h>
#include <errno.h>
#include <unistd.h>
#include <math.h>

#ifdef HAVE_ZAPTEL
#include <sys/ioctl.h>
#include <zaptel/zaptel.h>
#endif

#include "asterisk/pbx.h"
#include "asterisk/frame.h"
#include "asterisk/sched.h"
#include "asterisk/options.h"
#include "asterisk/channel.h"
#include "asterisk/chanspy.h"
#include "asterisk/musiconhold.h"
#include "asterisk/logger.h"
#include "asterisk/say.h"
#include "asterisk/file.h"
#include "asterisk/cli.h"
#include "asterisk/translate.h"
#include "asterisk/manager.h"
#include "asterisk/chanvars.h"
#include "asterisk/linkedlists.h"
#include "asterisk/indications.h"
#include "asterisk/monitor.h"
#include "asterisk/causes.h"
#include "asterisk/callerid.h"
#include "asterisk/utils.h"
#include "asterisk/lock.h"
#include "asterisk/app.h"
#include "asterisk/transcap.h"
#include "asterisk/devicestate.h"
#include "asterisk/sha1.h"
#include "asterisk/threadstorage.h"
#include "asterisk/slinfactory.h"

struct channel_spy_trans {
	int last_format;
	struct ast_trans_pvt *path;
};

struct ast_channel_spy_list {
	struct channel_spy_trans read_translator;
	struct channel_spy_trans write_translator;
	AST_LIST_HEAD_NOLOCK(, ast_channel_spy) list;
};

struct ast_channel_whisper_buffer {
	ast_mutex_t lock;
	struct ast_slinfactory sf;
	unsigned int original_format;
	struct ast_trans_pvt *path;
};

/* uncomment if you have problems with 'monitoring' synchronized files */
#if 0
#define MONITOR_CONSTANT_DELAY
#define MONITOR_DELAY	150 * 8		/* 150 ms of MONITORING DELAY */
#endif

/*! Prevent new channel allocation if shutting down. */
static int shutting_down;

static int uniqueint;

unsigned long global_fin, global_fout;

AST_THREADSTORAGE(state2str_threadbuf, state2str_threadbuf_init);
#define STATE2STR_BUFSIZE   32

/*! 100ms */
#define AST_DEFAULT_EMULATE_DTMF_DURATION 100

struct chanlist {
	const struct ast_channel_tech *tech;
	AST_LIST_ENTRY(chanlist) list;
};

/*! the list of registered channel types */
static AST_LIST_HEAD_NOLOCK_STATIC(backends, chanlist);

/*! the list of channels we have. Note that the lock for this list is used for
    both the channels list and the backends list.  */
static AST_LIST_HEAD_STATIC(channels, ast_channel);

/*! map AST_CAUSE's to readable string representations */
const struct ast_cause {
	int cause;
	const char *name;
	const char *desc;
} causes[] = {
	{ AST_CAUSE_UNALLOCATED, "UNALLOCATED", "Unallocated (unassigned) number" },
	{ AST_CAUSE_NO_ROUTE_TRANSIT_NET, "NO_ROUTE_TRANSIT_NET", "No route to specified transmit network" },
	{ AST_CAUSE_NO_ROUTE_DESTINATION, "NO_ROUTE_DESTINATION", "No route to destination" },
	{ AST_CAUSE_CHANNEL_UNACCEPTABLE, "CHANNEL_UNACCEPTABLE", "Channel unacceptable" },
	{ AST_CAUSE_CALL_AWARDED_DELIVERED, "CALL_AWARDED_DELIVERED", "Call awarded and being delivered in an established channel" },
	{ AST_CAUSE_NORMAL_CLEARING, "NORMAL_CLEARING", "Normal Clearing" },
	{ AST_CAUSE_USER_BUSY, "USER_BUSY", "User busy" },
	{ AST_CAUSE_NO_USER_RESPONSE, "NO_USER_RESPONSE", "No user responding" },
	{ AST_CAUSE_NO_ANSWER, "NO_ANSWER", "User alerting, no answer" },
	{ AST_CAUSE_CALL_REJECTED, "CALL_REJECTED", "Call Rejected" },
	{ AST_CAUSE_NUMBER_CHANGED, "NUMBER_CHANGED", "Number changed" },
	{ AST_CAUSE_DESTINATION_OUT_OF_ORDER, "DESTINATION_OUT_OF_ORDER", "Destination out of order" },
	{ AST_CAUSE_INVALID_NUMBER_FORMAT, "INVALID_NUMBER_FORMAT", "Invalid number format" },
	{ AST_CAUSE_FACILITY_REJECTED, "FACILITY_REJECTED", "Facility rejected" },
	{ AST_CAUSE_RESPONSE_TO_STATUS_ENQUIRY, "RESPONSE_TO_STATUS_ENQUIRY", "Response to STATus ENQuiry" },
	{ AST_CAUSE_NORMAL_UNSPECIFIED, "NORMAL_UNSPECIFIED", "Normal, unspecified" },
	{ AST_CAUSE_NORMAL_CIRCUIT_CONGESTION, "NORMAL_CIRCUIT_CONGESTION", "Circuit/channel congestion" },
	{ AST_CAUSE_NETWORK_OUT_OF_ORDER, "NETWORK_OUT_OF_ORDER", "Network out of order" },
	{ AST_CAUSE_NORMAL_TEMPORARY_FAILURE, "NORMAL_TEMPORARY_FAILURE", "Temporary failure" },
	{ AST_CAUSE_SWITCH_CONGESTION, "SWITCH_CONGESTION", "Switching equipment congestion" },
	{ AST_CAUSE_ACCESS_INFO_DISCARDED, "ACCESS_INFO_DISCARDED", "Access information discarded" },
	{ AST_CAUSE_REQUESTED_CHAN_UNAVAIL, "REQUESTED_CHAN_UNAVAIL", "Requested channel not available" },
	{ AST_CAUSE_PRE_EMPTED, "PRE_EMPTED", "Pre-empted" },
	{ AST_CAUSE_FACILITY_NOT_SUBSCRIBED, "FACILITY_NOT_SUBSCRIBED", "Facility not subscribed" },
	{ AST_CAUSE_OUTGOING_CALL_BARRED, "OUTGOING_CALL_BARRED", "Outgoing call barred" },
	{ AST_CAUSE_INCOMING_CALL_BARRED, "INCOMING_CALL_BARRED", "Incoming call barred" },
	{ AST_CAUSE_BEARERCAPABILITY_NOTAUTH, "BEARERCAPABILITY_NOTAUTH", "Bearer capability not authorized" },
	{ AST_CAUSE_BEARERCAPABILITY_NOTAVAIL, "BEARERCAPABILITY_NOTAVAIL", "Bearer capability not available" },
	{ AST_CAUSE_BEARERCAPABILITY_NOTIMPL, "BEARERCAPABILITY_NOTIMPL", "Bearer capability not implemented" },
	{ AST_CAUSE_CHAN_NOT_IMPLEMENTED, "CHAN_NOT_IMPLEMENTED", "Channel not implemented" },
	{ AST_CAUSE_FACILITY_NOT_IMPLEMENTED, "FACILITY_NOT_IMPLEMENTED", "Facility not implemented" },
	{ AST_CAUSE_INVALID_CALL_REFERENCE, "INVALID_CALL_REFERENCE", "Invalid call reference value" },
	{ AST_CAUSE_INCOMPATIBLE_DESTINATION, "INCOMPATIBLE_DESTINATION", "Incompatible destination" },
	{ AST_CAUSE_INVALID_MSG_UNSPECIFIED, "INVALID_MSG_UNSPECIFIED", "Invalid message unspecified" },
	{ AST_CAUSE_MANDATORY_IE_MISSING, "MANDATORY_IE_MISSING", "Mandatory information element is missing" },
	{ AST_CAUSE_MESSAGE_TYPE_NONEXIST, "MESSAGE_TYPE_NONEXIST", "Message type nonexist." },
	{ AST_CAUSE_WRONG_MESSAGE, "WRONG_MESSAGE", "Wrong message" },
	{ AST_CAUSE_IE_NONEXIST, "IE_NONEXIST", "Info. element nonexist or not implemented" },
	{ AST_CAUSE_INVALID_IE_CONTENTS, "INVALID_IE_CONTENTS", "Invalid information element contents" },
	{ AST_CAUSE_WRONG_CALL_STATE, "WRONG_CALL_STATE", "Message not compatible with call state" },
	{ AST_CAUSE_RECOVERY_ON_TIMER_EXPIRE, "RECOVERY_ON_TIMER_EXPIRE", "Recover on timer expiry" },
	{ AST_CAUSE_MANDATORY_IE_LENGTH_ERROR, "MANDATORY_IE_LENGTH_ERROR", "Mandatory IE length error" },
	{ AST_CAUSE_PROTOCOL_ERROR, "PROTOCOL_ERROR", "Protocol error, unspecified" },
	{ AST_CAUSE_INTERWORKING, "INTERWORKING", "Interworking, unspecified" },
};

struct ast_variable *ast_channeltype_list(void)
{
	struct chanlist *cl;
	struct ast_variable *var=NULL, *prev = NULL;
	AST_LIST_TRAVERSE(&backends, cl, list) {
		if (prev)  {
			if ((prev->next = ast_variable_new(cl->tech->type, cl->tech->description)))
				prev = prev->next;
		} else {
			var = ast_variable_new(cl->tech->type, cl->tech->description);
			prev = var;
		}
	}
	return var;
}

static int show_channeltypes(int fd, int argc, char *argv[])
{
#define FORMAT  "%-10.10s  %-40.40s %-12.12s %-12.12s %-12.12s\n"
	struct chanlist *cl;
	int count_chan = 0;

	ast_cli(fd, FORMAT, "Type", "Description",       "Devicestate", "Indications", "Transfer");
	ast_cli(fd, FORMAT, "----------", "-----------", "-----------", "-----------", "--------");
	if (AST_LIST_LOCK(&channels)) {
		ast_log(LOG_WARNING, "Unable to lock channel list\n");
		return -1;
	}
	AST_LIST_TRAVERSE(&backends, cl, list) {
		ast_cli(fd, FORMAT, cl->tech->type, cl->tech->description,
			(cl->tech->devicestate) ? "yes" : "no",
			(cl->tech->indicate) ? "yes" : "no",
			(cl->tech->transfer) ? "yes" : "no");
		count_chan++;
	}
	AST_LIST_UNLOCK(&channels);
	ast_cli(fd, "----------\n%d channel drivers registered.\n", count_chan);
	return RESULT_SUCCESS;

#undef FORMAT

}

static int show_channeltype_deprecated(int fd, int argc, char *argv[])
{
	struct chanlist *cl = NULL;

	if (argc != 3)
		return RESULT_SHOWUSAGE;
	
	if (AST_LIST_LOCK(&channels)) {
		ast_log(LOG_WARNING, "Unable to lock channel list\n");
		return RESULT_FAILURE;
	}

	AST_LIST_TRAVERSE(&backends, cl, list) {
		if (!strncasecmp(cl->tech->type, argv[2], strlen(cl->tech->type))) {
			break;
		}
	}


	if (!cl) {
		ast_cli(fd, "\n%s is not a registered channel driver.\n", argv[2]);
		AST_LIST_UNLOCK(&channels);
		return RESULT_FAILURE;
	}

	ast_cli(fd,
		"-- Info about channel driver: %s --\n"
		"  Device State: %s\n"
		"    Indication: %s\n"
		"     Transfer : %s\n"
		"  Capabilities: %d\n"
		"   Digit Begin: %s\n"
		"     Digit End: %s\n"
		"    Send HTML : %s\n"
		" Image Support: %s\n"
		"  Text Support: %s\n",
		cl->tech->type,
		(cl->tech->devicestate) ? "yes" : "no",
		(cl->tech->indicate) ? "yes" : "no",
		(cl->tech->transfer) ? "yes" : "no",
		(cl->tech->capabilities) ? cl->tech->capabilities : -1,
		(cl->tech->send_digit_begin) ? "yes" : "no",
		(cl->tech->send_digit_end) ? "yes" : "no",
		(cl->tech->send_html) ? "yes" : "no",
		(cl->tech->send_image) ? "yes" : "no",
		(cl->tech->send_text) ? "yes" : "no"
		
	);

	AST_LIST_UNLOCK(&channels);
	return RESULT_SUCCESS;
}

static int show_channeltype(int fd, int argc, char *argv[])
{
	struct chanlist *cl = NULL;

	if (argc != 4)
		return RESULT_SHOWUSAGE;
	
	if (AST_LIST_LOCK(&channels)) {
		ast_log(LOG_WARNING, "Unable to lock channel list\n");
		return RESULT_FAILURE;
	}

	AST_LIST_TRAVERSE(&backends, cl, list) {
		if (!strncasecmp(cl->tech->type, argv[3], strlen(cl->tech->type))) {
			break;
		}
	}


	if (!cl) {
		ast_cli(fd, "\n%s is not a registered channel driver.\n", argv[3]);
		AST_LIST_UNLOCK(&channels);
		return RESULT_FAILURE;
	}

	ast_cli(fd,
		"-- Info about channel driver: %s --\n"
		"  Device State: %s\n"
		"    Indication: %s\n"
		"     Transfer : %s\n"
		"  Capabilities: %d\n"
		"   Digit Begin: %s\n"
		"     Digit End: %s\n"
		"    Send HTML : %s\n"
		" Image Support: %s\n"
		"  Text Support: %s\n",
		cl->tech->type,
		(cl->tech->devicestate) ? "yes" : "no",
		(cl->tech->indicate) ? "yes" : "no",
		(cl->tech->transfer) ? "yes" : "no",
		(cl->tech->capabilities) ? cl->tech->capabilities : -1,
		(cl->tech->send_digit_begin) ? "yes" : "no",
		(cl->tech->send_digit_end) ? "yes" : "no",
		(cl->tech->send_html) ? "yes" : "no",
		(cl->tech->send_image) ? "yes" : "no",
		(cl->tech->send_text) ? "yes" : "no"
		
	);

	AST_LIST_UNLOCK(&channels);
	return RESULT_SUCCESS;
}

static char *complete_channeltypes_deprecated(const char *line, const char *word, int pos, int state)
{
	struct chanlist *cl;
	int which = 0;
	int wordlen;
	char *ret = NULL;

	if (pos != 2)
		return NULL;

	wordlen = strlen(word);

	AST_LIST_TRAVERSE(&backends, cl, list) {
		if (!strncasecmp(word, cl->tech->type, wordlen) && ++which > state) {
			ret = strdup(cl->tech->type);
			break;
		}
	}
	
	return ret;
}

static char *complete_channeltypes(const char *line, const char *word, int pos, int state)
{
	struct chanlist *cl;
	int which = 0;
	int wordlen;
	char *ret = NULL;

	if (pos != 3)
		return NULL;

	wordlen = strlen(word);

	AST_LIST_TRAVERSE(&backends, cl, list) {
		if (!strncasecmp(word, cl->tech->type, wordlen) && ++which > state) {
			ret = strdup(cl->tech->type);
			break;
		}
	}
	
	return ret;
}

static char show_channeltypes_usage[] =
"Usage: core show channeltypes\n"
"       Lists available channel types registered in your Asterisk server.\n";

static char show_channeltype_usage[] =
"Usage: core show channeltype <name>\n"
"	Show details about the specified channel type, <name>.\n";

static struct ast_cli_entry cli_show_channeltypes_deprecated = {
	{ "show", "channeltypes", NULL },
	show_channeltypes, NULL,
	NULL };

static struct ast_cli_entry cli_show_channeltype_deprecated = {
	{ "show", "channeltype", NULL },
	show_channeltype_deprecated, NULL,
	NULL, complete_channeltypes_deprecated };

static struct ast_cli_entry cli_channel[] = {
	{ { "core", "show", "channeltypes", NULL },
	show_channeltypes, "List available channel types",
	show_channeltypes_usage, NULL, &cli_show_channeltypes_deprecated },

	{ { "core", "show", "channeltype", NULL },
	show_channeltype, "Give more details on that channel type",
	show_channeltype_usage, complete_channeltypes, &cli_show_channeltype_deprecated },
};

/*! \brief Checks to see if a channel is needing hang up */
int ast_check_hangup(struct ast_channel *chan)
{
	if (chan->_softhangup)		/* yes if soft hangup flag set */
		return 1;
	if (!chan->tech_pvt)		/* yes if no technology private data */
		return 1;
	if (!chan->whentohangup)	/* no if no hangup scheduled */
		return 0;
	if (chan->whentohangup > time(NULL)) 	/* no if hangup time has not come yet. */
		return 0;
	chan->_softhangup |= AST_SOFTHANGUP_TIMEOUT;	/* record event */
	return 1;
}

static int ast_check_hangup_locked(struct ast_channel *chan)
{
	int res;
	ast_channel_lock(chan);
	res = ast_check_hangup(chan);
	ast_channel_unlock(chan);
	return res;
}

/*! \brief printf the string into a correctly sized mallocd buffer, and return the buffer */
char *ast_safe_string_alloc(const char *fmt, ...)
{
	char *b2,buf[1];
	int len;

	va_list args;
	va_start(args, fmt);
	len = vsnprintf(buf, 1, fmt, args);
	b2 = ast_malloc(len+1);
	vsnprintf(b2, len+1,  fmt, args);
	va_end(args);
	return b2;
}

/*! \brief Initiate system shutdown */
void ast_begin_shutdown(int hangup)
{
	struct ast_channel *c;
	shutting_down = 1;
	if (hangup) {
		AST_LIST_LOCK(&channels);
		AST_LIST_TRAVERSE(&channels, c, chan_list)
			ast_softhangup(c, AST_SOFTHANGUP_SHUTDOWN);
		AST_LIST_UNLOCK(&channels);
	}
}

/*! \brief returns number of active/allocated channels */
int ast_active_channels(void)
{
	struct ast_channel *c;
	int cnt = 0;
	AST_LIST_LOCK(&channels);
	AST_LIST_TRAVERSE(&channels, c, chan_list)
		cnt++;
	AST_LIST_UNLOCK(&channels);
	return cnt;
}

/*! \brief Cancel a shutdown in progress */
void ast_cancel_shutdown(void)
{
	shutting_down = 0;
}

/*! \brief Returns non-zero if Asterisk is being shut down */
int ast_shutting_down(void)
{
	return shutting_down;
}

/*! \brief Set when to hangup channel */
void ast_channel_setwhentohangup(struct ast_channel *chan, time_t offset)
{
	chan->whentohangup = offset ? time(NULL) + offset : 0;
	ast_queue_frame(chan, &ast_null_frame);
	return;
}

/*! \brief Compare a offset with when to hangup channel */
int ast_channel_cmpwhentohangup(struct ast_channel *chan, time_t offset)
{
	time_t whentohangup;

	if (chan->whentohangup == 0) {
		return (offset == 0) ? 0 : -1;
	} else {
		if (offset == 0)	/* XXX why is this special ? */
			return (1);
		else {
			whentohangup = offset + time (NULL);
			if (chan->whentohangup < whentohangup)
				return (1);
			else if (chan->whentohangup == whentohangup)
				return (0);
			else
				return (-1);
		}
	}
}

/*! \brief Register a new telephony channel in Asterisk */
int ast_channel_register(const struct ast_channel_tech *tech)
{
	struct chanlist *chan;

	AST_LIST_LOCK(&channels);

	AST_LIST_TRAVERSE(&backends, chan, list) {
		if (!strcasecmp(tech->type, chan->tech->type)) {
			ast_log(LOG_WARNING, "Already have a handler for type '%s'\n", tech->type);
			AST_LIST_UNLOCK(&channels);
			return -1;
		}
	}
	
	if (!(chan = ast_calloc(1, sizeof(*chan)))) {
		AST_LIST_UNLOCK(&channels);
		return -1;
	}
	chan->tech = tech;
	AST_LIST_INSERT_HEAD(&backends, chan, list);

	if (option_debug)
		ast_log(LOG_DEBUG, "Registered handler for '%s' (%s)\n", chan->tech->type, chan->tech->description);

	if (option_verbose > 1)
		ast_verbose(VERBOSE_PREFIX_2 "Registered channel type '%s' (%s)\n", chan->tech->type,
			    chan->tech->description);

	AST_LIST_UNLOCK(&channels);
	return 0;
}

void ast_channel_unregister(const struct ast_channel_tech *tech)
{
	struct chanlist *chan;

	if (option_debug)
		ast_log(LOG_DEBUG, "Unregistering channel type '%s'\n", tech->type);

	AST_LIST_LOCK(&channels);

	AST_LIST_TRAVERSE_SAFE_BEGIN(&backends, chan, list) {
		if (chan->tech == tech) {
			AST_LIST_REMOVE_CURRENT(&backends, list);
			free(chan);
			if (option_verbose > 1)
				ast_verbose(VERBOSE_PREFIX_2 "Unregistered channel type '%s'\n", tech->type);
			break;	
		}
	}
	AST_LIST_TRAVERSE_SAFE_END

	AST_LIST_UNLOCK(&channels);
}

const struct ast_channel_tech *ast_get_channel_tech(const char *name)
{
	struct chanlist *chanls;
	const struct ast_channel_tech *ret = NULL;

	if (AST_LIST_LOCK(&channels)) {
		ast_log(LOG_WARNING, "Unable to lock channel tech list\n");
		return NULL;
	}

	AST_LIST_TRAVERSE(&backends, chanls, list) {
		if (!strcasecmp(name, chanls->tech->type)) {
			ret = chanls->tech;
			break;
		}
	}

	AST_LIST_UNLOCK(&channels);
	
	return ret;
}

/*! \brief Gives the string form of a given hangup cause */
const char *ast_cause2str(int cause)
{
	int x;

	for (x=0; x < sizeof(causes) / sizeof(causes[0]); x++) {
		if (causes[x].cause == cause)
			return causes[x].desc;
	}

	return "Unknown";
}

/*! \brief Convert a symbolic hangup cause to number */
int ast_str2cause(const char *name)
{
	int x;

	for (x = 0; x < sizeof(causes) / sizeof(causes[0]); x++)
		if (strncasecmp(causes[x].name, name, strlen(causes[x].name)) == 0)
			return causes[x].cause;

	return -1;
}

/*! \brief Gives the string form of a given channel state */
char *ast_state2str(enum ast_channel_state state)
{
	char *buf;

	switch(state) {
	case AST_STATE_DOWN:
		return "Down";
	case AST_STATE_RESERVED:
		return "Rsrvd";
	case AST_STATE_OFFHOOK:
		return "OffHook";
	case AST_STATE_DIALING:
		return "Dialing";
	case AST_STATE_RING:
		return "Ring";
	case AST_STATE_RINGING:
		return "Ringing";
	case AST_STATE_UP:
		return "Up";
	case AST_STATE_BUSY:
		return "Busy";
	case AST_STATE_DIALING_OFFHOOK:
		return "Dialing Offhook";
	case AST_STATE_PRERING:
		return "Pre-ring";
	default:
		if (!(buf = ast_threadstorage_get(&state2str_threadbuf, STATE2STR_BUFSIZE)))
			return "Unknown";
		snprintf(buf, STATE2STR_BUFSIZE, "Unknown (%d)", state);
		return buf;
	}
}

/*! \brief Gives the string form of a given transfer capability */
char *ast_transfercapability2str(int transfercapability)
{
	switch(transfercapability) {
	case AST_TRANS_CAP_SPEECH:
		return "SPEECH";
	case AST_TRANS_CAP_DIGITAL:
		return "DIGITAL";
	case AST_TRANS_CAP_RESTRICTED_DIGITAL:
		return "RESTRICTED_DIGITAL";
	case AST_TRANS_CAP_3_1K_AUDIO:
		return "3K1AUDIO";
	case AST_TRANS_CAP_DIGITAL_W_TONES:
		return "DIGITAL_W_TONES";
	case AST_TRANS_CAP_VIDEO:
		return "VIDEO";
	default:
		return "UNKNOWN";
	}
}

/*! \brief Pick the best audio codec */
int ast_best_codec(int fmts)
{
	/* This just our opinion, expressed in code.  We are asked to choose
	   the best codec to use, given no information */
	int x;
	static int prefs[] =
	{
		/*! Okay, ulaw is used by all telephony equipment, so start with it */
		AST_FORMAT_ULAW,
		/*! Unless of course, you're a silly European, so then prefer ALAW */
		AST_FORMAT_ALAW,
		/*! G.722 is better then all below, but not as common as the above... so give ulaw and alaw priority */
		AST_FORMAT_G722,
		/*! Okay, well, signed linear is easy to translate into other stuff */
		AST_FORMAT_SLINEAR,
		/*! G.726 is standard ADPCM, in RFC3551 packing order */
		AST_FORMAT_G726,
		/*! G.726 is standard ADPCM, in AAL2 packing order */
		AST_FORMAT_G726_AAL2,
		/*! ADPCM has great sound quality and is still pretty easy to translate */
		AST_FORMAT_ADPCM,
		/*! Okay, we're down to vocoders now, so pick GSM because it's small and easier to
		    translate and sounds pretty good */
		AST_FORMAT_GSM,
		/*! iLBC is not too bad */
		AST_FORMAT_ILBC,
		/*! Speex is free, but computationally more expensive than GSM */
		AST_FORMAT_SPEEX,
		/*! Ick, LPC10 sounds terrible, but at least we have code for it, if you're tacky enough
		    to use it */
		AST_FORMAT_LPC10,
		/*! G.729a is faster than 723 and slightly less expensive */
		AST_FORMAT_G729A,
		/*! Down to G.723.1 which is proprietary but at least designed for voice */
		AST_FORMAT_G723_1,
	};

	/* Strip out video */
	fmts &= AST_FORMAT_AUDIO_MASK;
	
	/* Find the first preferred codec in the format given */
	for (x=0; x < (sizeof(prefs) / sizeof(prefs[0]) ); x++)
		if (fmts & prefs[x])
			return prefs[x];
	ast_log(LOG_WARNING, "Don't know any of 0x%x formats\n", fmts);
	return 0;
}

static const struct ast_channel_tech null_tech = {
	.type = "NULL",
	.description = "Null channel (should not see this)",
};

/*! \brief Create a new channel structure */
struct ast_channel *ast_channel_alloc(int needqueue, int state, const char *cid_num, const char *cid_name, const char *name_fmt, ...)
{
	struct ast_channel *tmp;
	int x;
	int flags;
	struct varshead *headp;
	va_list ap1, ap2;

	/* If shutting down, don't allocate any new channels */
	if (shutting_down) {
		ast_log(LOG_WARNING, "Channel allocation failed: Refusing due to active shutdown\n");
		return NULL;
	}

	if (!(tmp = ast_calloc(1, sizeof(*tmp))))
		return NULL;

	if (!(tmp->sched = sched_context_create())) {
		ast_log(LOG_WARNING, "Channel allocation failed: Unable to create schedule context\n");
		free(tmp);
		return NULL;
	}
	
	ast_string_field_init(tmp, 128);

	/* Don't bother initializing the last two FD here, because they
	   will *always* be set just a few lines down (AST_TIMING_FD,
	   AST_ALERT_FD). */
	for (x = 0; x < AST_MAX_FDS - 2; x++)
		tmp->fds[x] = -1;

#ifdef HAVE_ZAPTEL
	tmp->timingfd = open("/dev/zap/timer", O_RDWR);
	if (tmp->timingfd > -1) {
		/* Check if timing interface supports new
		   ping/pong scheme */
		flags = 1;
		if (!ioctl(tmp->timingfd, ZT_TIMERPONG, &flags))
			needqueue = 0;
	}
#else
	tmp->timingfd = -1;					
#endif					

	if (needqueue) {
		if (pipe(tmp->alertpipe)) {
			ast_log(LOG_WARNING, "Channel allocation failed: Can't create alert pipe!\n");
			ast_string_field_free_pools(tmp);
			free(tmp);
			return NULL;
		} else {
			flags = fcntl(tmp->alertpipe[0], F_GETFL);
			fcntl(tmp->alertpipe[0], F_SETFL, flags | O_NONBLOCK);
			flags = fcntl(tmp->alertpipe[1], F_GETFL);
			fcntl(tmp->alertpipe[1], F_SETFL, flags | O_NONBLOCK);
		}
	} else	/* Make sure we've got it done right if they don't */
		tmp->alertpipe[0] = tmp->alertpipe[1] = -1;

	/* Always watch the alertpipe */
	tmp->fds[AST_ALERT_FD] = tmp->alertpipe[0];
	/* And timing pipe */
	tmp->fds[AST_TIMING_FD] = tmp->timingfd;
	ast_string_field_set(tmp, name, "**Unknown**");

	/* Initial state */
	tmp->_state = state;

	tmp->streamid = -1;
	
	tmp->fin = global_fin;
	tmp->fout = global_fout;

	if (ast_strlen_zero(ast_config_AST_SYSTEM_NAME)) {
		ast_string_field_build(tmp, uniqueid, "%li.%d", (long) time(NULL), 
			ast_atomic_fetchadd_int(&uniqueint, 1));
	} else {
		ast_string_field_build(tmp, uniqueid, "%s-%li.%d", ast_config_AST_SYSTEM_NAME, 
			(long) time(NULL), ast_atomic_fetchadd_int(&uniqueint, 1));
	}

	if (!ast_strlen_zero(name_fmt)) {
		/* Almost every channel is calling this function, and setting the name via the ast_string_field_build() call.
		 * And they all use slightly different formats for their name string.
		 * This means, to set the name here, we have to accept variable args, and call the string_field_build from here.
		 * This means, that the stringfields must have a routine that takes the va_lists directly, and 
		 * uses them to build the string, instead of forming the va_lists internally from the vararg ... list.
		 * This new function was written so this can be accomplished.
		 */
		va_start(ap1, name_fmt);
		va_start(ap2, name_fmt);
		ast_string_field_build_va(tmp, name, name_fmt, ap1, ap2);
		va_end(ap1);
		va_end(ap2);

		/* and now, since the channel structure is built, and has its name, let's call the
		 * manager event generator with this Newchannel event. This is the proper and correct
		 * place to make this call, but you sure do have to pass a lot of data into this func
		 * to do it here!
		 */
		manager_event(EVENT_FLAG_CALL, "Newchannel",
			      "Channel: %s\r\n"
			      "State: %s\r\n"
			      "CallerIDNum: %s\r\n"
			      "CallerIDName: %s\r\n"
			      "Uniqueid: %s\r\n",
			      tmp->name, ast_state2str(state),
			      S_OR(cid_num, "<unknown>"),
			      S_OR(cid_name, "<unknown>"),
			      tmp->uniqueid);
	}

	/* Experiment: under what conditions do we NOT want to track cdrs on channels? */
	tmp->cdr = ast_cdr_alloc();
	ast_cdr_init(tmp->cdr, tmp);
	ast_cdr_start(tmp->cdr);
	
	headp = &tmp->varshead;
	AST_LIST_HEAD_INIT_NOLOCK(headp);
	
	ast_mutex_init(&tmp->lock);
	
	AST_LIST_HEAD_INIT_NOLOCK(&tmp->datastores);
	
	strcpy(tmp->context, "default");
	strcpy(tmp->exten, "s");
	tmp->priority = 1;
	
	ast_string_field_set(tmp, language, defaultlanguage);
	tmp->amaflags = ast_default_amaflags;
	ast_string_field_set(tmp, accountcode, ast_default_accountcode);

	tmp->tech = &null_tech;

	AST_LIST_LOCK(&channels);
	AST_LIST_INSERT_HEAD(&channels, tmp, chan_list);
	AST_LIST_UNLOCK(&channels);

	return tmp;
}

/*! \brief Queue an outgoing media frame */
int ast_queue_frame(struct ast_channel *chan, struct ast_frame *fin)
{
	struct ast_frame *f;
	struct ast_frame *cur;
	int blah = 1;
	int qlen = 0;

	/* Build us a copy and free the original one */
	if (!(f = ast_frdup(fin))) {
		ast_log(LOG_WARNING, "Unable to duplicate frame\n");
		return -1;
	}
	ast_channel_lock(chan);

	/* See if the last frame on the queue is a hangup, if so don't queue anything */
	if ((cur = AST_LIST_LAST(&chan->readq)) && (cur->frametype == AST_FRAME_CONTROL) && (cur->subclass == AST_CONTROL_HANGUP)) {
		ast_frfree(f);
		ast_channel_unlock(chan);
		return 0;
	}

	/* Count how many frames exist on the queue */
	AST_LIST_TRAVERSE(&chan->readq, cur, frame_list) {
		qlen++;
	}

	/* Allow up to 96 voice frames outstanding, and up to 128 total frames */
	if (((fin->frametype == AST_FRAME_VOICE) && (qlen > 96)) || (qlen  > 128)) {
		if (fin->frametype != AST_FRAME_VOICE) {
			ast_log(LOG_WARNING, "Exceptionally long queue length queuing to %s\n", chan->name);
			CRASH;
		} else {
			if (option_debug)
				ast_log(LOG_DEBUG, "Dropping voice to exceptionally long queue on %s\n", chan->name);
			ast_frfree(f);
			ast_channel_unlock(chan);
			return 0;
		}
	}
	AST_LIST_INSERT_TAIL(&chan->readq, f, frame_list);
	if (chan->alertpipe[1] > -1) {
		if (write(chan->alertpipe[1], &blah, sizeof(blah)) != sizeof(blah))
			ast_log(LOG_WARNING, "Unable to write to alert pipe on %s, frametype/subclass %d/%d (qlen = %d): %s!\n",
				chan->name, f->frametype, f->subclass, qlen, strerror(errno));
#ifdef HAVE_ZAPTEL
	} else if (chan->timingfd > -1) {
		ioctl(chan->timingfd, ZT_TIMERPING, &blah);
#endif				
	} else if (ast_test_flag(chan, AST_FLAG_BLOCKING)) {
		pthread_kill(chan->blocker, SIGURG);
	}
	ast_channel_unlock(chan);
	return 0;
}

/*! \brief Queue a hangup frame for channel */
int ast_queue_hangup(struct ast_channel *chan)
{
	struct ast_frame f = { AST_FRAME_CONTROL, AST_CONTROL_HANGUP };
	/* Yeah, let's not change a lock-critical value without locking */
	if (!ast_channel_trylock(chan)) {
		chan->_softhangup |= AST_SOFTHANGUP_DEV;
		ast_channel_unlock(chan);
	}
	return ast_queue_frame(chan, &f);
}

/*! \brief Queue a control frame */
int ast_queue_control(struct ast_channel *chan, enum ast_control_frame_type control)
{
	struct ast_frame f = { AST_FRAME_CONTROL, };

	f.subclass = control;

	return ast_queue_frame(chan, &f);
}

/*! \brief Queue a control frame with payload */
int ast_queue_control_data(struct ast_channel *chan, enum ast_control_frame_type control,
			   const void *data, size_t datalen)
{
	struct ast_frame f = { AST_FRAME_CONTROL, };

	f.subclass = control;
	f.data = (void *) data;
	f.datalen = datalen;

	return ast_queue_frame(chan, &f);
}

/*! \brief Set defer DTMF flag on channel */
int ast_channel_defer_dtmf(struct ast_channel *chan)
{
	int pre = 0;

	if (chan) {
		pre = ast_test_flag(chan, AST_FLAG_DEFER_DTMF);
		ast_set_flag(chan, AST_FLAG_DEFER_DTMF);
	}
	return pre;
}

/*! \brief Unset defer DTMF flag on channel */
void ast_channel_undefer_dtmf(struct ast_channel *chan)
{
	if (chan)
		ast_clear_flag(chan, AST_FLAG_DEFER_DTMF);
}

/*!
 * \brief Helper function to find channels.
 *
 * It supports these modes:
 *
 * prev != NULL : get channel next in list after prev
 * name != NULL : get channel with matching name
 * name != NULL && namelen != 0 : get channel whose name starts with prefix
 * exten != NULL : get channel whose exten or macroexten matches
 * context != NULL && exten != NULL : get channel whose context or macrocontext
 *
 * It returns with the channel's lock held. If getting the individual lock fails,
 * unlock and retry quickly up to 10 times, then give up.
 *
 * \note XXX Note that this code has cost O(N) because of the need to verify
 * that the object is still on the global list.
 *
 * \note XXX also note that accessing fields (e.g. c->name in ast_log())
 * can only be done with the lock held or someone could delete the
 * object while we work on it. This causes some ugliness in the code.
 * Note that removing the first ast_log() may be harmful, as it would
 * shorten the retry period and possibly cause failures.
 * We should definitely go for a better scheme that is deadlock-free.
 */
static struct ast_channel *channel_find_locked(const struct ast_channel *prev,
					       const char *name, const int namelen,
					       const char *context, const char *exten)
{
	const char *msg = prev ? "deadlock" : "initial deadlock";
	int retries;
	struct ast_channel *c;

	for (retries = 0; retries < 10; retries++) {
		int done;
		AST_LIST_LOCK(&channels);
		AST_LIST_TRAVERSE(&channels, c, chan_list) {
			if (prev) {	/* look for next item */
				if (c != prev)	/* not this one */
					continue;
				/* found, prepare to return c->next */
				if ((c = AST_LIST_NEXT(c, chan_list)) == NULL) break;
				/* If prev was the last item on the channel list, then we just
				 * want to return NULL, instead of trying to deref NULL in the
				 * next section.
				 */
			}
			if (name) { /* want match by name */
				if ((!namelen && strcasecmp(c->name, name)) ||
				    (namelen && strncasecmp(c->name, name, namelen)))
					continue;	/* name match failed */
			} else if (exten) {
				if (context && strcasecmp(c->context, context) &&
				    strcasecmp(c->macrocontext, context))
					continue;	/* context match failed */
				if (strcasecmp(c->exten, exten) &&
				    strcasecmp(c->macroexten, exten))
					continue;	/* exten match failed */
			}
			/* if we get here, c points to the desired record */
			break;
		}
		/* exit if chan not found or mutex acquired successfully */
		/* this is slightly unsafe, as we _should_ hold the lock to access c->name */
		done = c == NULL || ast_channel_trylock(c) == 0;
		if (!done)
			if (option_debug)
				ast_log(LOG_DEBUG, "Avoiding %s for channel '%p'\n", msg, c);
		AST_LIST_UNLOCK(&channels);
		if (done)
			return c;
		usleep(1);	/* give other threads a chance before retrying */
	}
	/*
 	 * c is surely not null, but we don't have the lock so cannot
	 * access c->name
	 */
	if (option_debug)
		ast_log(LOG_DEBUG, "Failure, could not lock '%p' after %d retries!\n",
			c, retries);

	return NULL;
}

/*! \brief Browse channels in use */
struct ast_channel *ast_channel_walk_locked(const struct ast_channel *prev)
{
	return channel_find_locked(prev, NULL, 0, NULL, NULL);
}

/*! \brief Get channel by name and lock it */
struct ast_channel *ast_get_channel_by_name_locked(const char *name)
{
	return channel_find_locked(NULL, name, 0, NULL, NULL);
}

/*! \brief Get channel by name prefix and lock it */
struct ast_channel *ast_get_channel_by_name_prefix_locked(const char *name, const int namelen)
{
	return channel_find_locked(NULL, name, namelen, NULL, NULL);
}

/*! \brief Get next channel by name prefix and lock it */
struct ast_channel *ast_walk_channel_by_name_prefix_locked(const struct ast_channel *chan, const char *name,
							   const int namelen)
{
	return channel_find_locked(chan, name, namelen, NULL, NULL);
}

/*! \brief Get channel by exten (and optionally context) and lock it */
struct ast_channel *ast_get_channel_by_exten_locked(const char *exten, const char *context)
{
	return channel_find_locked(NULL, NULL, 0, context, exten);
}

/*! \brief Get next channel by exten (and optionally context) and lock it */
struct ast_channel *ast_walk_channel_by_exten_locked(const struct ast_channel *chan, const char *exten,
						     const char *context)
{
	return channel_find_locked(chan, NULL, 0, context, exten);
}

/*! \brief Wait, look for hangups and condition arg */
int ast_safe_sleep_conditional(struct ast_channel *chan, int ms, int (*cond)(void*), void *data)
{
	struct ast_frame *f;

	while (ms > 0) {
		if (cond && ((*cond)(data) == 0))
			return 0;
		ms = ast_waitfor(chan, ms);
		if (ms < 0)
			return -1;
		if (ms > 0) {
			f = ast_read(chan);
			if (!f)
				return -1;
			ast_frfree(f);
		}
	}
	return 0;
}

/*! \brief Wait, look for hangups */
int ast_safe_sleep(struct ast_channel *chan, int ms)
{
	return ast_safe_sleep_conditional(chan, ms, NULL, NULL);
}

static void free_cid(struct ast_callerid *cid)
{
	if (cid->cid_dnid)
		free(cid->cid_dnid);
	if (cid->cid_num)
		free(cid->cid_num);	
	if (cid->cid_name)
		free(cid->cid_name);	
	if (cid->cid_ani)
		free(cid->cid_ani);
	if (cid->cid_rdnis)
		free(cid->cid_rdnis);
}

/*! \brief Free a channel structure */
void ast_channel_free(struct ast_channel *chan)
{
	int fd;
	struct ast_var_t *vardata;
	struct ast_frame *f;
	struct varshead *headp;
	struct ast_datastore *datastore = NULL;
	char name[AST_CHANNEL_NAME];
	
	headp=&chan->varshead;
	
	AST_LIST_LOCK(&channels);
	AST_LIST_REMOVE(&channels, chan, chan_list);
	/* Lock and unlock the channel just to be sure nobody
	   has it locked still */
	ast_channel_lock(chan);
	ast_channel_unlock(chan);
	if (chan->tech_pvt) {
		ast_log(LOG_WARNING, "Channel '%s' may not have been hung up properly\n", chan->name);
		free(chan->tech_pvt);
	}

	if (chan->sched)
		sched_context_destroy(chan->sched);

	ast_copy_string(name, chan->name, sizeof(name));

	/* Stop monitoring */
	if (chan->monitor)
		chan->monitor->stop( chan, 0 );

	/* If there is native format music-on-hold state, free it */
	if (chan->music_state)
		ast_moh_cleanup(chan);

	/* if someone is whispering on the channel, stop them */
	if (chan->whisper)
		ast_channel_whisper_stop(chan);

	/* Free translators */
	if (chan->readtrans)
		ast_translator_free_path(chan->readtrans);
	if (chan->writetrans)
		ast_translator_free_path(chan->writetrans);
	if (chan->pbx)
		ast_log(LOG_WARNING, "PBX may not have been terminated properly on '%s'\n", chan->name);
	free_cid(&chan->cid);
	ast_mutex_destroy(&chan->lock);
	/* Close pipes if appropriate */
	if ((fd = chan->alertpipe[0]) > -1)
		close(fd);
	if ((fd = chan->alertpipe[1]) > -1)
		close(fd);
	if ((fd = chan->timingfd) > -1)
		close(fd);
	while ((f = AST_LIST_REMOVE_HEAD(&chan->readq, frame_list)))
		ast_frfree(f);
	
	/* Get rid of each of the data stores on the channel */
	while ((datastore = AST_LIST_REMOVE_HEAD(&chan->datastores, entry)))
		/* Free the data store */
		ast_channel_datastore_free(datastore);
	AST_LIST_HEAD_INIT_NOLOCK(&chan->datastores);

	/* loop over the variables list, freeing all data and deleting list items */
	/* no need to lock the list, as the channel is already locked */
	
	while ((vardata = AST_LIST_REMOVE_HEAD(headp, entries)))
		ast_var_delete(vardata);

	/* Destroy the jitterbuffer */
	ast_jb_destroy(chan);

	ast_string_field_free_pools(chan);
	free(chan);
	AST_LIST_UNLOCK(&channels);

	ast_device_state_changed_literal(name);
}

struct ast_datastore *ast_channel_datastore_alloc(const struct ast_datastore_info *info, char *uid)
{
	struct ast_datastore *datastore = NULL;

	/* Make sure we at least have type so we can identify this */
	if (info == NULL) {
		return NULL;
	}

	/* Allocate memory for datastore and clear it */
	datastore = ast_calloc(1, sizeof(*datastore));
	if (datastore == NULL) {
		return NULL;
	}

	datastore->info = info;

	datastore->uid = ast_strdup(uid);

	return datastore;
}

int ast_channel_datastore_free(struct ast_datastore *datastore)
{
	int res = 0;

	/* Using the destroy function (if present) destroy the data */
	if (datastore->info->destroy != NULL && datastore->data != NULL) {
		datastore->info->destroy(datastore->data);
		datastore->data = NULL;
	}

	/* Free allocated UID memory */
	if (datastore->uid != NULL) {
		free(datastore->uid);
		datastore->uid = NULL;
	}

	/* Finally free memory used by ourselves */
	free(datastore);

	return res;
}

int ast_channel_datastore_add(struct ast_channel *chan, struct ast_datastore *datastore)
{
	int res = 0;

	AST_LIST_INSERT_HEAD(&chan->datastores, datastore, entry);

	return res;
}

int ast_channel_datastore_remove(struct ast_channel *chan, struct ast_datastore *datastore)
{
	struct ast_datastore *datastore2 = NULL;
	int res = -1;

	/* Find our position and remove ourselves */
	AST_LIST_TRAVERSE_SAFE_BEGIN(&chan->datastores, datastore2, entry) {
		if (datastore2 == datastore) {
			AST_LIST_REMOVE_CURRENT(&chan->datastores, entry);
			res = 0;
			break;
		}
	}
	AST_LIST_TRAVERSE_SAFE_END

	return res;
}

struct ast_datastore *ast_channel_datastore_find(struct ast_channel *chan, const struct ast_datastore_info *info, char *uid)
{
	struct ast_datastore *datastore = NULL;
	
	if (info == NULL)
		return NULL;

	AST_LIST_TRAVERSE_SAFE_BEGIN(&chan->datastores, datastore, entry) {
		if (datastore->info == info) {
			if (uid != NULL && datastore->uid != NULL) {
				if (!strcasecmp(uid, datastore->uid)) {
					/* Matched by type AND uid */
					break;
				}
			} else {
				/* Matched by type at least */
				break;
			}
		}
	}
	AST_LIST_TRAVERSE_SAFE_END

	return datastore;
}

int ast_channel_spy_add(struct ast_channel *chan, struct ast_channel_spy *spy)
{
	/* Link the owner channel to the spy */
	spy->chan = chan;

	if (!ast_test_flag(spy, CHANSPY_FORMAT_AUDIO)) {
		ast_log(LOG_WARNING, "Could not add channel spy '%s' to channel '%s', only audio format spies are supported.\n",
			spy->type, chan->name);
		return -1;
	}

	if (ast_test_flag(spy, CHANSPY_READ_VOLADJUST) && (spy->read_queue.format != AST_FORMAT_SLINEAR)) {
		ast_log(LOG_WARNING, "Cannot provide volume adjustment on '%s' format spies\n",
			ast_getformatname(spy->read_queue.format));
		return -1;
	}

	if (ast_test_flag(spy, CHANSPY_WRITE_VOLADJUST) && (spy->write_queue.format != AST_FORMAT_SLINEAR)) {
		ast_log(LOG_WARNING, "Cannot provide volume adjustment on '%s' format spies\n",
			ast_getformatname(spy->write_queue.format));
		return -1;
	}

	if (ast_test_flag(spy, CHANSPY_MIXAUDIO) &&
	    ((spy->read_queue.format != AST_FORMAT_SLINEAR) ||
	     (spy->write_queue.format != AST_FORMAT_SLINEAR))) {
		ast_log(LOG_WARNING, "Cannot provide audio mixing on '%s'-'%s' format spies\n",
			ast_getformatname(spy->read_queue.format), ast_getformatname(spy->write_queue.format));
		return -1;
	}

	if (!chan->spies) {
		if (!(chan->spies = ast_calloc(1, sizeof(*chan->spies)))) {
			return -1;
		}

		AST_LIST_HEAD_INIT_NOLOCK(&chan->spies->list);
		AST_LIST_INSERT_HEAD(&chan->spies->list, spy, list);
	} else {
		AST_LIST_INSERT_TAIL(&chan->spies->list, spy, list);
	}

	if (ast_test_flag(spy, CHANSPY_TRIGGER_MODE) != CHANSPY_TRIGGER_NONE) {
		ast_cond_init(&spy->trigger, NULL);
		ast_set_flag(spy, CHANSPY_TRIGGER_READ);
		ast_clear_flag(spy, CHANSPY_TRIGGER_WRITE);
	}

	if (option_debug)
		ast_log(LOG_DEBUG, "Spy %s added to channel %s\n",
			spy->type, chan->name);

	return 0;
}

/* Clean up a channel's spy information */
static void spy_cleanup(struct ast_channel *chan)
{
	if (!AST_LIST_EMPTY(&chan->spies->list))
		return;
	if (chan->spies->read_translator.path)
		ast_translator_free_path(chan->spies->read_translator.path);
	if (chan->spies->write_translator.path)
		ast_translator_free_path(chan->spies->write_translator.path);
	free(chan->spies);
	chan->spies = NULL;
	return;
}

/* Detach a spy from it's channel */
static void spy_detach(struct ast_channel_spy *spy, struct ast_channel *chan)
{
	ast_mutex_lock(&spy->lock);

	/* We only need to poke them if they aren't already done */
	if (spy->status != CHANSPY_DONE) {
		/* Indicate to the spy to stop */
		spy->status = CHANSPY_STOP;
		spy->chan = NULL;
		/* Poke the spy if needed */
		if (ast_test_flag(spy, CHANSPY_TRIGGER_MODE) != CHANSPY_TRIGGER_NONE)
			ast_cond_signal(&spy->trigger);
	}

	/* Print it out while we still have a lock so the structure can't go away (if signalled above) */
	if (option_debug)
		ast_log(LOG_DEBUG, "Spy %s removed from channel %s\n", spy->type, chan->name);

	ast_mutex_unlock(&spy->lock);

	return;
}

void ast_channel_spy_stop_by_type(struct ast_channel *chan, const char *type)
{
	struct ast_channel_spy *spy = NULL;
	
	if (!chan->spies)
		return;

	AST_LIST_TRAVERSE_SAFE_BEGIN(&chan->spies->list, spy, list) {
		ast_mutex_lock(&spy->lock);
		if ((spy->type == type) && (spy->status == CHANSPY_RUNNING)) {
			ast_mutex_unlock(&spy->lock);
			AST_LIST_REMOVE_CURRENT(&chan->spies->list, list);
			spy_detach(spy, chan);
		} else
			ast_mutex_unlock(&spy->lock);
	}
	AST_LIST_TRAVERSE_SAFE_END
	spy_cleanup(chan);
}

void ast_channel_spy_trigger_wait(struct ast_channel_spy *spy)
{
	struct timeval tv;
	struct timespec ts;

	tv = ast_tvadd(ast_tvnow(), ast_samp2tv(50000, 1000));
	ts.tv_sec = tv.tv_sec;
	ts.tv_nsec = tv.tv_usec * 1000;

	ast_cond_timedwait(&spy->trigger, &spy->lock, &ts);
}

void ast_channel_spy_remove(struct ast_channel *chan, struct ast_channel_spy *spy)
{
	if (!chan->spies)
		return;

	AST_LIST_REMOVE(&chan->spies->list, spy, list);
	spy_detach(spy, chan);
	spy_cleanup(chan);
}

void ast_channel_spy_free(struct ast_channel_spy *spy)
{
	struct ast_frame *f = NULL;

	if (spy->status == CHANSPY_DONE)
		return;

	/* Switch status to done in case we get called twice */
	spy->status = CHANSPY_DONE;

	/* Drop any frames in the queue */
	while ((f = AST_LIST_REMOVE_HEAD(&spy->write_queue.list, frame_list)))
		ast_frfree(f);
	while ((f = AST_LIST_REMOVE_HEAD(&spy->read_queue.list, frame_list)))
		ast_frfree(f);

	/* Destroy the condition if in use */
	if (ast_test_flag(spy, CHANSPY_TRIGGER_MODE) != CHANSPY_TRIGGER_NONE)
		ast_cond_destroy(&spy->trigger);

	/* Destroy our mutex since it is no longer in use */
	ast_mutex_destroy(&spy->lock);

	return;
}

static void detach_spies(struct ast_channel *chan)
{
	struct ast_channel_spy *spy = NULL;

	if (!chan->spies)
		return;

	AST_LIST_TRAVERSE_SAFE_BEGIN(&chan->spies->list, spy, list) {
		AST_LIST_REMOVE_CURRENT(&chan->spies->list, list);
		spy_detach(spy, chan);
	}
	AST_LIST_TRAVERSE_SAFE_END

	spy_cleanup(chan);
}

/*! \brief Softly hangup a channel, don't lock */
int ast_softhangup_nolock(struct ast_channel *chan, int cause)
{
	if (option_debug)
		ast_log(LOG_DEBUG, "Soft-Hanging up channel '%s'\n", chan->name);
	/* Inform channel driver that we need to be hung up, if it cares */
	chan->_softhangup |= cause;
	ast_queue_frame(chan, &ast_null_frame);
	/* Interrupt any poll call or such */
	if (ast_test_flag(chan, AST_FLAG_BLOCKING))
		pthread_kill(chan->blocker, SIGURG);
	return 0;
}

/*! \brief Softly hangup a channel, lock */
int ast_softhangup(struct ast_channel *chan, int cause)
{
	int res;
	ast_channel_lock(chan);
	res = ast_softhangup_nolock(chan, cause);
	ast_channel_unlock(chan);
	return res;
}

enum spy_direction {
	SPY_READ,
	SPY_WRITE,
};

#define SPY_QUEUE_SAMPLE_LIMIT 4000			/* half of one second */

static void queue_frame_to_spies(struct ast_channel *chan, struct ast_frame *f, enum spy_direction dir)
{
	struct ast_frame *translated_frame = NULL;
	struct ast_channel_spy *spy;
	struct channel_spy_trans *trans;

	trans = (dir == SPY_READ) ? &chan->spies->read_translator : &chan->spies->write_translator;

	AST_LIST_TRAVERSE(&chan->spies->list, spy, list) {
		struct ast_channel_spy_queue *queue;
		struct ast_frame *duped_fr;

		ast_mutex_lock(&spy->lock);

		queue = (dir == SPY_READ) ? &spy->read_queue : &spy->write_queue;

		if ((queue->format == AST_FORMAT_SLINEAR) && (f->subclass != AST_FORMAT_SLINEAR)) {
			if (!translated_frame) {
				if (trans->path && (trans->last_format != f->subclass)) {
					ast_translator_free_path(trans->path);
					trans->path = NULL;
				}
				if (!trans->path) {
					if (option_debug)
						ast_log(LOG_DEBUG, "Building translator from %s to SLINEAR for spies on channel %s\n",
							ast_getformatname(f->subclass), chan->name);
					if ((trans->path = ast_translator_build_path(AST_FORMAT_SLINEAR, f->subclass)) == NULL) {
						ast_log(LOG_WARNING, "Cannot build a path from %s to %s\n",
							ast_getformatname(f->subclass), ast_getformatname(AST_FORMAT_SLINEAR));
						ast_mutex_unlock(&spy->lock);
						continue;
					} else {
						trans->last_format = f->subclass;
					}
				}
				if (!(translated_frame = ast_translate(trans->path, f, 0))) {
					ast_log(LOG_ERROR, "Translation to %s failed, dropping frame for spies\n",
						ast_getformatname(AST_FORMAT_SLINEAR));
					ast_mutex_unlock(&spy->lock);
					break;
				}
			}
			duped_fr = ast_frdup(translated_frame);
		} else if (f->subclass != queue->format) {
			ast_log(LOG_WARNING, "Spy '%s' on channel '%s' wants format '%s', but frame is '%s', dropping\n",
				spy->type, chan->name,
				ast_getformatname(queue->format), ast_getformatname(f->subclass));
			ast_mutex_unlock(&spy->lock);
			continue;
		} else
			duped_fr = ast_frdup(f);

		AST_LIST_INSERT_TAIL(&queue->list, duped_fr, frame_list);

		queue->samples += f->samples;

		if (queue->samples > SPY_QUEUE_SAMPLE_LIMIT) {
			if (ast_test_flag(spy, CHANSPY_TRIGGER_MODE) != CHANSPY_TRIGGER_NONE) {
				switch (ast_test_flag(spy, CHANSPY_TRIGGER_MODE)) {
				case CHANSPY_TRIGGER_READ:
					if (dir == SPY_WRITE) {
						ast_set_flag(spy, CHANSPY_TRIGGER_WRITE);
						ast_clear_flag(spy, CHANSPY_TRIGGER_READ);
						if (option_debug)
							ast_log(LOG_DEBUG, "Switching spy '%s' on '%s' to write-trigger mode\n",
								spy->type, chan->name);
					}
					break;
				case CHANSPY_TRIGGER_WRITE:
					if (dir == SPY_READ) {
						ast_set_flag(spy, CHANSPY_TRIGGER_READ);
						ast_clear_flag(spy, CHANSPY_TRIGGER_WRITE);
						if (option_debug)
							ast_log(LOG_DEBUG, "Switching spy '%s' on '%s' to read-trigger mode\n",
								spy->type, chan->name);
					}
					break;
				}
				if (option_debug)
					ast_log(LOG_DEBUG, "Triggering queue flush for spy '%s' on '%s'\n",
						spy->type, chan->name);
				ast_set_flag(spy, CHANSPY_TRIGGER_FLUSH);
				ast_cond_signal(&spy->trigger);
			} else {
				if (option_debug)
					ast_log(LOG_DEBUG, "Spy '%s' on channel '%s' %s queue too long, dropping frames\n",
						spy->type, chan->name, (dir == SPY_READ) ? "read" : "write");
				while (queue->samples > SPY_QUEUE_SAMPLE_LIMIT) {
					struct ast_frame *drop = AST_LIST_REMOVE_HEAD(&queue->list, frame_list);
					queue->samples -= drop->samples;
					ast_frfree(drop);
				}
			}
		} else {
			switch (ast_test_flag(spy, CHANSPY_TRIGGER_MODE)) {
			case CHANSPY_TRIGGER_READ:
				if (dir == SPY_READ)
					ast_cond_signal(&spy->trigger);
				break;
			case CHANSPY_TRIGGER_WRITE:
				if (dir == SPY_WRITE)
					ast_cond_signal(&spy->trigger);
				break;
			}
		}

		ast_mutex_unlock(&spy->lock);
	}

	if (translated_frame)
		ast_frfree(translated_frame);
}

static void free_translation(struct ast_channel *clone)
{
	if (clone->writetrans)
		ast_translator_free_path(clone->writetrans);
	if (clone->readtrans)
		ast_translator_free_path(clone->readtrans);
	clone->writetrans = NULL;
	clone->readtrans = NULL;
	clone->rawwriteformat = clone->nativeformats;
	clone->rawreadformat = clone->nativeformats;
}

/*! \brief Hangup a channel */
int ast_hangup(struct ast_channel *chan)
{
	int res = 0;

	/* Don't actually hang up a channel that will masquerade as someone else, or
	   if someone is going to masquerade as us */
	ast_channel_lock(chan);

	detach_spies(chan);		/* get rid of spies */

	if (chan->masq) {
		if (ast_do_masquerade(chan))
			ast_log(LOG_WARNING, "Failed to perform masquerade\n");
	}

	if (chan->masq) {
		ast_log(LOG_WARNING, "%s getting hung up, but someone is trying to masq into us?!?\n", chan->name);
		ast_channel_unlock(chan);
		return 0;
	}
	/* If this channel is one which will be masqueraded into something,
	   mark it as a zombie already, so we know to free it later */
	if (chan->masqr) {
		ast_set_flag(chan, AST_FLAG_ZOMBIE);
		ast_channel_unlock(chan);
		return 0;
	}
	free_translation(chan);
	/* Close audio stream */
	if (chan->stream) {
		ast_closestream(chan->stream);
		chan->stream = NULL;
	}
	/* Close video stream */
	if (chan->vstream) {
		ast_closestream(chan->vstream);
		chan->vstream = NULL;
	}
	if (chan->sched) {
		sched_context_destroy(chan->sched);
		chan->sched = NULL;
	}
	
	if (chan->generatordata)	/* Clear any tone stuff remaining */
		chan->generator->release(chan, chan->generatordata);
	chan->generatordata = NULL;
	chan->generator = NULL;
	if (chan->cdr) {		/* End the CDR if it hasn't already */
		ast_cdr_end(chan->cdr);
		ast_cdr_detach(chan->cdr);	/* Post and Free the CDR */
		chan->cdr = NULL;
	}
	if (ast_test_flag(chan, AST_FLAG_BLOCKING)) {
		ast_log(LOG_WARNING, "Hard hangup called by thread %ld on %s, while fd "
					"is blocked by thread %ld in procedure %s!  Expect a failure\n",
					(long)pthread_self(), chan->name, (long)chan->blocker, chan->blockproc);
		CRASH;
	}
	if (!ast_test_flag(chan, AST_FLAG_ZOMBIE)) {
		if (option_debug)
			ast_log(LOG_DEBUG, "Hanging up channel '%s'\n", chan->name);
		if (chan->tech->hangup)
			res = chan->tech->hangup(chan);
	} else {
		if (option_debug)
			ast_log(LOG_DEBUG, "Hanging up zombie '%s'\n", chan->name);
	}
			
	ast_channel_unlock(chan);
	manager_event(EVENT_FLAG_CALL, "Hangup",
			"Channel: %s\r\n"
			"Uniqueid: %s\r\n"
			"Cause: %d\r\n"
			"Cause-txt: %s\r\n",
			chan->name,
			chan->uniqueid,
			chan->hangupcause,
			ast_cause2str(chan->hangupcause)
			);
	ast_channel_free(chan);
	return res;
}

int ast_answer(struct ast_channel *chan)
{
	int res = 0;
	ast_channel_lock(chan);
	/* You can't answer an outbound call */
	if (ast_test_flag(chan, AST_FLAG_OUTGOING)) {
		ast_channel_unlock(chan);
		return 0;
	}
	/* Stop if we're a zombie or need a soft hangup */
	if (ast_test_flag(chan, AST_FLAG_ZOMBIE) || ast_check_hangup(chan)) {
		ast_channel_unlock(chan);
		return -1;
	}
	switch(chan->_state) {
	case AST_STATE_RINGING:
	case AST_STATE_RING:
		if (chan->tech->answer)
			res = chan->tech->answer(chan);
		ast_setstate(chan, AST_STATE_UP);
		ast_cdr_answer(chan->cdr);
		break;
	case AST_STATE_UP:
		ast_cdr_answer(chan->cdr);
		break;
	default:
		break;
	}
	ast_channel_unlock(chan);
	return res;
}

void ast_deactivate_generator(struct ast_channel *chan)
{
	ast_channel_lock(chan);
	if (chan->generatordata) {
		if (chan->generator && chan->generator->release)
			chan->generator->release(chan, chan->generatordata);
		chan->generatordata = NULL;
		chan->generator = NULL;
		chan->fds[AST_GENERATOR_FD] = -1;
		ast_clear_flag(chan, AST_FLAG_WRITE_INT);
		ast_settimeout(chan, 0, NULL, NULL);
	}
	ast_channel_unlock(chan);
}

static int generator_force(void *data)
{
	/* Called if generator doesn't have data */
	void *tmp;
	int res;
	int (*generate)(struct ast_channel *chan, void *tmp, int datalen, int samples);
	struct ast_channel *chan = data;
	tmp = chan->generatordata;
	chan->generatordata = NULL;
	generate = chan->generator->generate;
	res = generate(chan, tmp, 0, 160);
	chan->generatordata = tmp;
	if (res) {
		if (option_debug)
			ast_log(LOG_DEBUG, "Auto-deactivating generator\n");
		ast_deactivate_generator(chan);
	}
	return 0;
}

int ast_activate_generator(struct ast_channel *chan, struct ast_generator *gen, void *params)
{
	int res = 0;

	ast_channel_lock(chan);

	if (chan->generatordata) {
		if (chan->generator && chan->generator->release)
			chan->generator->release(chan, chan->generatordata);
		chan->generatordata = NULL;
	}

	ast_prod(chan);
	if (gen->alloc && !(chan->generatordata = gen->alloc(chan, params))) {
		res = -1;
	}
	
	if (!res) {
		ast_settimeout(chan, 160, generator_force, chan);
		chan->generator = gen;
	}

	ast_channel_unlock(chan);

	return res;
}

/*! \brief Wait for x amount of time on a file descriptor to have input.  */
int ast_waitfor_n_fd(int *fds, int n, int *ms, int *exception)
{
	int winner = -1;
	ast_waitfor_nandfds(NULL, 0, fds, n, exception, &winner, ms);
	return winner;
}

/*! \brief Wait for x amount of time on a file descriptor to have input.  */
struct ast_channel *ast_waitfor_nandfds(struct ast_channel **c, int n, int *fds, int nfds,
	int *exception, int *outfd, int *ms)
{
	struct timeval start = { 0 , 0 };
	struct pollfd *pfds;
	int res;
	long rms;
	int x, y, max;
	int sz;
	time_t now = 0;
	long whentohangup = 0, diff;
	struct ast_channel *winner = NULL;
	struct fdmap {
		int chan;
		int fdno;
	} *fdmap;

	sz = n * AST_MAX_FDS + nfds;
	pfds = alloca(sizeof(*pfds) * sz);
	fdmap = alloca(sizeof(*fdmap) * sz);

	if (outfd)
		*outfd = -99999;
	if (exception)
		*exception = 0;
	
	/* Perform any pending masquerades */
	for (x=0; x < n; x++) {
		ast_channel_lock(c[x]);
		if (c[x]->masq) {
			if (ast_do_masquerade(c[x])) {
				ast_log(LOG_WARNING, "Masquerade failed\n");
				*ms = -1;
				ast_channel_unlock(c[x]);
				return NULL;
			}
		}
		if (c[x]->whentohangup) {
			if (!whentohangup)
				time(&now);
			diff = c[x]->whentohangup - now;
			if (diff < 1) {
				/* Should already be hungup */
				c[x]->_softhangup |= AST_SOFTHANGUP_TIMEOUT;
				ast_channel_unlock(c[x]);
				return c[x];
			}
			if (!whentohangup || (diff < whentohangup))
				whentohangup = diff;
		}
		ast_channel_unlock(c[x]);
	}
	/* Wait full interval */
	rms = *ms;
	if (whentohangup) {
		rms = whentohangup * 1000;              /* timeout in milliseconds */
		if (*ms >= 0 && *ms < rms)		/* original *ms still smaller */
			rms =  *ms;
	}
	/*
	 * Build the pollfd array, putting the channels' fds first,
	 * followed by individual fds. Order is important because
	 * individual fd's must have priority over channel fds.
	 */
	max = 0;
	for (x=0; x<n; x++) {
		for (y=0; y<AST_MAX_FDS; y++) {
			fdmap[max].fdno = y;  /* fd y is linked to this pfds */
			fdmap[max].chan = x;  /* channel x is linked to this pfds */
			max += ast_add_fd(&pfds[max], c[x]->fds[y]);
		}
		CHECK_BLOCKING(c[x]);
	}
	/* Add the individual fds */
	for (x=0; x<nfds; x++) {
		fdmap[max].chan = -1;
		max += ast_add_fd(&pfds[max], fds[x]);
	}

	if (*ms > 0)
		start = ast_tvnow();
	
	if (sizeof(int) == 4) {	/* XXX fix timeout > 600000 on linux x86-32 */
		do {
			int kbrms = rms;
			if (kbrms > 600000)
				kbrms = 600000;
			res = poll(pfds, max, kbrms);
			if (!res)
				rms -= kbrms;
		} while (!res && (rms > 0));
	} else {
		res = poll(pfds, max, rms);
	}
	for (x=0; x<n; x++)
		ast_clear_flag(c[x], AST_FLAG_BLOCKING);
	if (res < 0) { /* Simulate a timeout if we were interrupted */
		if (errno != EINTR)
			*ms = -1;
		return NULL;
	}
	if (whentohangup) {   /* if we have a timeout, check who expired */
		time(&now);
		for (x=0; x<n; x++) {
			if (c[x]->whentohangup && now >= c[x]->whentohangup) {
				c[x]->_softhangup |= AST_SOFTHANGUP_TIMEOUT;
				if (winner == NULL)
					winner = c[x];
			}
		}
	}
	if (res == 0) { /* no fd ready, reset timeout and done */
		*ms = 0;	/* XXX use 0 since we may not have an exact timeout. */
		return winner;
	}
	/*
	 * Then check if any channel or fd has a pending event.
	 * Remember to check channels first and fds last, as they
	 * must have priority on setting 'winner'
	 */
	for (x = 0; x < max; x++) {
		res = pfds[x].revents;
		if (res == 0)
			continue;
		if (fdmap[x].chan >= 0) {	/* this is a channel */
			winner = c[fdmap[x].chan];	/* override previous winners */
			if (res & POLLPRI)
				ast_set_flag(winner, AST_FLAG_EXCEPTION);
			else
				ast_clear_flag(winner, AST_FLAG_EXCEPTION);
			winner->fdno = fdmap[x].fdno;
		} else {			/* this is an fd */
			if (outfd)
				*outfd = pfds[x].fd;
			if (exception)
				*exception = (res & POLLPRI) ? -1 : 0;
			winner = NULL;
		}
	}
	if (*ms > 0) {
		*ms -= ast_tvdiff_ms(ast_tvnow(), start);
		if (*ms < 0)
			*ms = 0;
	}
	return winner;
}

struct ast_channel *ast_waitfor_n(struct ast_channel **c, int n, int *ms)
{
	return ast_waitfor_nandfds(c, n, NULL, 0, NULL, NULL, ms);
}

int ast_waitfor(struct ast_channel *c, int ms)
{
	int oldms = ms;	/* -1 if no timeout */

	ast_waitfor_nandfds(&c, 1, NULL, 0, NULL, NULL, &ms);
	if ((ms < 0) && (oldms < 0))
		ms = 0;
	return ms;
}

/* XXX never to be called with ms = -1 */
int ast_waitfordigit(struct ast_channel *c, int ms)
{
	return ast_waitfordigit_full(c, ms, -1, -1);
}

int ast_settimeout(struct ast_channel *c, int samples, int (*func)(void *data), void *data)
{
	int res = -1;
#ifdef HAVE_ZAPTEL
	if (c->timingfd > -1) {
		if (!func) {
			samples = 0;
			data = 0;
		}
		if (option_debug)
			ast_log(LOG_DEBUG, "Scheduling timer at %d sample intervals\n", samples);
		res = ioctl(c->timingfd, ZT_TIMERCONFIG, &samples);
		c->timingfunc = func;
		c->timingdata = data;
	}
#endif	
	return res;
}

int ast_waitfordigit_full(struct ast_channel *c, int ms, int audiofd, int cmdfd)
{

	/* Stop if we're a zombie or need a soft hangup */
	if (ast_test_flag(c, AST_FLAG_ZOMBIE) || ast_check_hangup(c))
		return -1;
	/* Wait for a digit, no more than ms milliseconds total. */
	while (ms) {
		struct ast_channel *rchan;
		int outfd;

		errno = 0;
		rchan = ast_waitfor_nandfds(&c, 1, &cmdfd, (cmdfd > -1) ? 1 : 0, NULL, &outfd, &ms);
		if (!rchan && outfd < 0 && ms) {
			if (errno == 0 || errno == EINTR)
				continue;
			ast_log(LOG_WARNING, "Wait failed (%s)\n", strerror(errno));
			return -1;
		} else if (outfd > -1) {
			/* The FD we were watching has something waiting */
			return 1;
		} else if (rchan) {
			int res;
			struct ast_frame *f = ast_read(c);
			if (!f)
				return -1;

			switch(f->frametype) {
			case AST_FRAME_DTMF:
				res = f->subclass;
				ast_frfree(f);
				return res;
			case AST_FRAME_CONTROL:
				switch(f->subclass) {
				case AST_CONTROL_HANGUP:
					ast_frfree(f);
					return -1;
				case AST_CONTROL_RINGING:
				case AST_CONTROL_ANSWER:
					/* Unimportant */
					break;
				default:
					ast_log(LOG_WARNING, "Unexpected control subclass '%d'\n", f->subclass);
					break;
				}
				break;
			case AST_FRAME_VOICE:
				/* Write audio if appropriate */
				if (audiofd > -1)
					write(audiofd, f->data, f->datalen);
			default:
				/* Ignore */
				break;
			}
			ast_frfree(f);
		}
	}
	return 0; /* Time is up */
}

static struct ast_frame *__ast_read(struct ast_channel *chan, int dropaudio)
{
	struct ast_frame *f = NULL;	/* the return value */
	int blah;
	int prestate;

	/* this function is very long so make sure there is only one return
	 * point at the end (there is only one exception to this).
	 */
	ast_channel_lock(chan);
	if (chan->masq) {
		if (ast_do_masquerade(chan))
			ast_log(LOG_WARNING, "Failed to perform masquerade\n");
		else
			f =  &ast_null_frame;
		goto done;
	}

	/* Stop if we're a zombie or need a soft hangup */
	if (ast_test_flag(chan, AST_FLAG_ZOMBIE) || ast_check_hangup(chan)) {
		if (chan->generator)
			ast_deactivate_generator(chan);
		goto done;
	}
	prestate = chan->_state;

	if (!ast_test_flag(chan, AST_FLAG_DEFER_DTMF | AST_FLAG_EMULATE_DTMF | AST_FLAG_IN_DTMF) && 
	    !ast_strlen_zero(chan->dtmfq)) {
		/* We have DTMF that has been deferred.  Return it now */
		chan->dtmff.subclass = chan->dtmfq[0];
		/* Drop first digit from the buffer */
		memmove(chan->dtmfq, chan->dtmfq + 1, sizeof(chan->dtmfq) - 1);
		f = &chan->dtmff;
		if (ast_test_flag(chan, AST_FLAG_END_DTMF_ONLY))
			chan->dtmff.frametype = AST_FRAME_DTMF_END;
		else {
			chan->dtmff.frametype = AST_FRAME_DTMF_BEGIN;
			ast_set_flag(chan, AST_FLAG_EMULATE_DTMF);
			chan->emulate_dtmf_digit = f->subclass;
			chan->emulate_dtmf_duration = AST_DEFAULT_EMULATE_DTMF_DURATION;
			chan->dtmf_begin_tv = ast_tvnow();
		}
		goto done;
	}
	
	/* Read and ignore anything on the alertpipe, but read only
	   one sizeof(blah) per frame that we send from it */
	if (chan->alertpipe[0] > -1)
		read(chan->alertpipe[0], &blah, sizeof(blah));

#ifdef HAVE_ZAPTEL
	if (chan->timingfd > -1 && chan->fdno == AST_TIMING_FD && ast_test_flag(chan, AST_FLAG_EXCEPTION)) {
		int res;

		ast_clear_flag(chan, AST_FLAG_EXCEPTION);
		blah = -1;
		/* IF we can't get event, assume it's an expired as-per the old interface */
		res = ioctl(chan->timingfd, ZT_GETEVENT, &blah);
		if (res)
			blah = ZT_EVENT_TIMER_EXPIRED;

		if (blah == ZT_EVENT_TIMER_PING) {
			if (AST_LIST_EMPTY(&chan->readq) || !AST_LIST_NEXT(AST_LIST_FIRST(&chan->readq), frame_list)) {
				/* Acknowledge PONG unless we need it again */
				if (ioctl(chan->timingfd, ZT_TIMERPONG, &blah)) {
					ast_log(LOG_WARNING, "Failed to pong timer on '%s': %s\n", chan->name, strerror(errno));
				}
			}
		} else if (blah == ZT_EVENT_TIMER_EXPIRED) {
			ioctl(chan->timingfd, ZT_TIMERACK, &blah);
			if (chan->timingfunc) {
				/* save a copy of func/data before unlocking the channel */
				int (*func)(void *) = chan->timingfunc;
				void *data = chan->timingdata;
				ast_channel_unlock(chan);
				func(data);
			} else {
				blah = 0;
				ioctl(chan->timingfd, ZT_TIMERCONFIG, &blah);
				chan->timingdata = NULL;
				ast_channel_unlock(chan);
			}
			/* cannot 'goto done' because the channel is already unlocked */
			return &ast_null_frame;
		} else
			ast_log(LOG_NOTICE, "No/unknown event '%d' on timer for '%s'?\n", blah, chan->name);
	} else
#endif
	if (chan->fds[AST_GENERATOR_FD] > -1 && chan->fdno == AST_GENERATOR_FD) {
		/* if the AST_GENERATOR_FD is set, call the generator with args
		 * set to -1 so it can do whatever it needs to.
		 */
		void *tmp = chan->generatordata;
		chan->generatordata = NULL;     /* reset to let ast_write get through */
		chan->generator->generate(chan, tmp, -1, -1);
		chan->generatordata = tmp;
		f = &ast_null_frame;
		goto done;
	}

	/* Check for pending read queue */
	if (!AST_LIST_EMPTY(&chan->readq)) {
		f = AST_LIST_REMOVE_HEAD(&chan->readq, frame_list);
		/* Interpret hangup and return NULL */
		/* XXX why not the same for frames from the channel ? */
		if (f->frametype == AST_FRAME_CONTROL && f->subclass == AST_CONTROL_HANGUP) {
			ast_frfree(f);
			f = NULL;
		}
	} else {
		chan->blocker = pthread_self();
		if (ast_test_flag(chan, AST_FLAG_EXCEPTION)) {
			if (chan->tech->exception)
				f = chan->tech->exception(chan);
			else {
				ast_log(LOG_WARNING, "Exception flag set on '%s', but no exception handler\n", chan->name);
				f = &ast_null_frame;
			}
			/* Clear the exception flag */
			ast_clear_flag(chan, AST_FLAG_EXCEPTION);
		} else if (chan->tech->read)
			f = chan->tech->read(chan);
		else
			ast_log(LOG_WARNING, "No read routine on channel %s\n", chan->name);
	}

	if (f) {
		/* if the channel driver returned more than one frame, stuff the excess
		   into the readq for the next ast_read call (note that we can safely assume
		   that the readq is empty, because otherwise we would not have called into
		   the channel driver and f would be only a single frame)
		*/
		if (AST_LIST_NEXT(f, frame_list)) {
			AST_LIST_HEAD_SET_NOLOCK(&chan->readq, AST_LIST_NEXT(f, frame_list));
			AST_LIST_NEXT(f, frame_list) = NULL;
		}

		switch (f->frametype) {
		case AST_FRAME_CONTROL:
			if (f->subclass == AST_CONTROL_ANSWER) {
				if (!ast_test_flag(chan, AST_FLAG_OUTGOING)) {
					if (option_debug)
						ast_log(LOG_DEBUG, "Ignoring answer on an inbound call!\n");
					ast_frfree(f);
					f = &ast_null_frame;
				} else if (prestate == AST_STATE_UP) {
					if (option_debug)
						ast_log(LOG_DEBUG, "Dropping duplicate answer!\n");
					ast_frfree(f);
					f = &ast_null_frame;
				} else {
					/* Answer the CDR */
					ast_setstate(chan, AST_STATE_UP);
					if (!chan->cdr) { /* up till now, this insertion hasn't been done. Therefore,
										 to keep from throwing off the basic order of the universe,
										 we will try to keep this cdr from getting posted. */
						chan->cdr = ast_cdr_alloc();
						ast_cdr_init(chan->cdr, chan);
						ast_cdr_start(chan->cdr);
					}
					
					ast_cdr_answer(chan->cdr);
				}
			}
			break;
		case AST_FRAME_DTMF_END:
			ast_log(LOG_DTMF, "DTMF end '%c' received on %s, duration %ld ms\n", f->subclass, chan->name, f->len);
			/* Queue it up if DTMF is deffered, or if DTMF emulation is forced.
			 * However, only let emulation be forced if the other end cares about BEGIN frames */
			if ( ast_test_flag(chan, AST_FLAG_DEFER_DTMF) ||
				(ast_test_flag(chan, AST_FLAG_EMULATE_DTMF) && !ast_test_flag(chan, AST_FLAG_END_DTMF_ONLY)) ) {
				if (strlen(chan->dtmfq) < sizeof(chan->dtmfq) - 2)
					chan->dtmfq[strlen(chan->dtmfq)] = f->subclass;
				else
					ast_log(LOG_WARNING, "Dropping deferred DTMF digits on %s\n", chan->name);
				ast_frfree(f);
				f = &ast_null_frame;
			} else if (!ast_test_flag(chan, AST_FLAG_IN_DTMF | AST_FLAG_END_DTMF_ONLY)) {
				f->frametype = AST_FRAME_DTMF_BEGIN;
				ast_set_flag(chan, AST_FLAG_EMULATE_DTMF);
				chan->emulate_dtmf_digit = f->subclass;
				chan->dtmf_begin_tv = ast_tvnow();
				if (f->len)
					chan->emulate_dtmf_duration = f->len;
				else
					chan->emulate_dtmf_duration = AST_DEFAULT_EMULATE_DTMF_DURATION;
			} else {
				ast_clear_flag(chan, AST_FLAG_IN_DTMF);
				if (!f->len)
					f->len = ast_tvdiff_ms(ast_tvnow(), chan->dtmf_begin_tv);
			}
			break;
		case AST_FRAME_DTMF_BEGIN:
			ast_log(LOG_DTMF, "DTMF begin '%c' received on %s\n", f->subclass, chan->name);
			if (ast_test_flag(chan, AST_FLAG_DEFER_DTMF | AST_FLAG_END_DTMF_ONLY)) {
				ast_frfree(f);
				f = &ast_null_frame;
			} else {
				ast_set_flag(chan, AST_FLAG_IN_DTMF);
				chan->dtmf_begin_tv = ast_tvnow();
			}
			break;
		case AST_FRAME_VOICE:
			/* The EMULATE_DTMF flag must be cleared here as opposed to when the samples
			 * first get to zero, because we want to make sure we pass at least one
			 * voice frame through before starting the next digit, to ensure a gap
			 * between DTMF digits. */
			if (ast_test_flag(chan, AST_FLAG_EMULATE_DTMF) && !chan->emulate_dtmf_duration) {
				ast_clear_flag(chan, AST_FLAG_EMULATE_DTMF);
				chan->emulate_dtmf_digit = 0;
			}

			if (dropaudio || ast_test_flag(chan, AST_FLAG_IN_DTMF)) {
				ast_frfree(f);
				f = &ast_null_frame;
			} else if (ast_test_flag(chan, AST_FLAG_EMULATE_DTMF)) {
				if ((f->samples / 8) >= chan->emulate_dtmf_duration) { /* XXX 8kHz */
					chan->emulate_dtmf_duration = 0;
					f->frametype = AST_FRAME_DTMF_END;
					f->subclass = chan->emulate_dtmf_digit;
					f->len = ast_tvdiff_ms(ast_tvnow(), chan->dtmf_begin_tv);
				} else {
					chan->emulate_dtmf_duration -= f->samples / 8; /* XXX 8kHz */
					ast_frfree(f);
					f = &ast_null_frame;
				}
			} else if (!(f->subclass & chan->nativeformats)) {
				/* This frame can't be from the current native formats -- drop it on the
				   floor */
				ast_log(LOG_NOTICE, "Dropping incompatible voice frame on %s of format %s since our native format has changed to %s\n",
					chan->name, ast_getformatname(f->subclass), ast_getformatname(chan->nativeformats));
				ast_frfree(f);
				f = &ast_null_frame;
			} else {
				if (chan->spies)
					queue_frame_to_spies(chan, f, SPY_READ);
				
				if (chan->monitor && chan->monitor->read_stream ) {
					/* XXX what does this do ? */
#ifndef MONITOR_CONSTANT_DELAY
					int jump = chan->outsmpl - chan->insmpl - 4 * f->samples;
					if (jump >= 0) {
						jump = chan->outsmpl - chan->insmpl;
						if (ast_seekstream(chan->monitor->read_stream, jump, SEEK_FORCECUR) == -1)
							ast_log(LOG_WARNING, "Failed to perform seek in monitoring read stream, synchronization between the files may be broken\n");
						chan->insmpl += jump + f->samples;
					} else
						chan->insmpl+= f->samples;
#else
					int jump = chan->outsmpl - chan->insmpl;
					if (jump - MONITOR_DELAY >= 0) {
						if (ast_seekstream(chan->monitor->read_stream, jump - f->samples, SEEK_FORCECUR) == -1)
							ast_log(LOG_WARNING, "Failed to perform seek in monitoring read stream, synchronization between the files may be broken\n");
						chan->insmpl += jump;
					} else
						chan->insmpl += f->samples;
#endif
					if (chan->monitor->state == AST_MONITOR_RUNNING) {
						if (ast_writestream(chan->monitor->read_stream, f) < 0)
							ast_log(LOG_WARNING, "Failed to write data to channel monitor read stream\n");
					}
				}

				if (chan->readtrans && (f = ast_translate(chan->readtrans, f, 1)) == NULL)
					f = &ast_null_frame;

				/* Run generator sitting on the line if timing device not available
				* and synchronous generation of outgoing frames is necessary       */
				if (chan->generatordata &&  !ast_internal_timing_enabled(chan)) {
					void *tmp = chan->generatordata;
					int res;

					if (chan->timingfunc) {
						if (option_debug > 1)
							ast_log(LOG_DEBUG, "Generator got voice, switching to phase locked mode\n");
						ast_settimeout(chan, 0, NULL, NULL);
					}

					chan->generatordata = NULL;	/* reset, to let writes go through */
					res = chan->generator->generate(chan, tmp, f->datalen, f->samples);
					chan->generatordata = tmp;
					if (res) {
						if (option_debug > 1)
							ast_log(LOG_DEBUG, "Auto-deactivating generator\n");
						ast_deactivate_generator(chan);
					}

				} else if (f->frametype == AST_FRAME_CNG) {
					if (chan->generator && !chan->timingfunc && (chan->timingfd > -1)) {
						if (option_debug > 1)
							ast_log(LOG_DEBUG, "Generator got CNG, switching to timed mode\n");
						ast_settimeout(chan, 160, generator_force, chan);
					}
				}
			}
		default:
			/* Just pass it on! */
			break;
		}
	} else {
		/* Make sure we always return NULL in the future */
		chan->_softhangup |= AST_SOFTHANGUP_DEV;
		if (chan->generator)
			ast_deactivate_generator(chan);
		/* End the CDR if appropriate */
		if (chan->cdr)
			ast_cdr_end(chan->cdr);
	}

	/* High bit prints debugging */
	if (chan->fin & DEBUGCHAN_FLAG)
		ast_frame_dump(chan->name, f, "<<");
	chan->fin = FRAMECOUNT_INC(chan->fin);

done:
	ast_channel_unlock(chan);
	return f;
}

int ast_internal_timing_enabled(struct ast_channel *chan)
{
	int ret = ast_opt_internal_timing && chan->timingfd > -1;
	if (option_debug > 4)
		ast_log(LOG_DEBUG, "Internal timing is %s (option_internal_timing=%d chan->timingfd=%d)\n", ret? "enabled": "disabled", ast_opt_internal_timing, chan->timingfd);
	return ret;
}

struct ast_frame *ast_read(struct ast_channel *chan)
{
	return __ast_read(chan, 0);
}

struct ast_frame *ast_read_noaudio(struct ast_channel *chan)
{
	return __ast_read(chan, 1);
}

int ast_indicate(struct ast_channel *chan, int condition)
{
	return ast_indicate_data(chan, condition, NULL, 0);
}

int ast_indicate_data(struct ast_channel *chan, int condition, const void *data, size_t datalen)
{
	int res = -1;

	ast_channel_lock(chan);
	/* Stop if we're a zombie or need a soft hangup */
	if (ast_test_flag(chan, AST_FLAG_ZOMBIE) || ast_check_hangup(chan)) {
		ast_channel_unlock(chan);
		return -1;
	}
	if (chan->tech->indicate)
		res = chan->tech->indicate(chan, condition, data, datalen);
	ast_channel_unlock(chan);
	if (!chan->tech->indicate || res) {
		/*
		 * Device does not support (that) indication, lets fake
		 * it by doing our own tone generation. (PM2002)
		 */
		if (condition < 0)
			ast_playtones_stop(chan);
		else {
			const struct tone_zone_sound *ts = NULL;
			switch (condition) {
			case AST_CONTROL_RINGING:
				ts = ast_get_indication_tone(chan->zone, "ring");
				break;
			case AST_CONTROL_BUSY:
				ts = ast_get_indication_tone(chan->zone, "busy");
				break;
			case AST_CONTROL_CONGESTION:
				ts = ast_get_indication_tone(chan->zone, "congestion");
				break;
			}
			if (ts && ts->data[0]) {
				if (option_debug)
					ast_log(LOG_DEBUG, "Driver for channel '%s' does not support indication %d, emulating it\n", chan->name, condition);
				ast_playtones_start(chan,0,ts->data, 1);
				res = 0;
			} else if (condition == AST_CONTROL_PROGRESS) {
				/* ast_playtones_stop(chan); */
			} else if (condition == AST_CONTROL_PROCEEDING) {
				/* Do nothing, really */
			} else if (condition == AST_CONTROL_HOLD) {
				/* Do nothing.... */
			} else if (condition == AST_CONTROL_UNHOLD) {
				/* Do nothing.... */
			} else if (condition == AST_CONTROL_VIDUPDATE) {
				/* Do nothing.... */
			} else {
				/* not handled */
				ast_log(LOG_WARNING, "Unable to handle indication %d for '%s'\n", condition, chan->name);
				res = -1;
			}
		}
	}
	return res;
}

int ast_recvchar(struct ast_channel *chan, int timeout)
{
	int c;
	char *buf = ast_recvtext(chan, timeout);
	if (buf == NULL)
		return -1;	/* error or timeout */
	c = *(unsigned char *)buf;
	free(buf);
	return c;
}

char *ast_recvtext(struct ast_channel *chan, int timeout)
{
	int res, done = 0;
	char *buf = NULL;
	
	while (!done) {
		struct ast_frame *f;
		if (ast_check_hangup(chan))
			break;
		res = ast_waitfor(chan, timeout);
		if (res <= 0) /* timeout or error */
			break;
		timeout = res;	/* update timeout */
		f = ast_read(chan);
		if (f == NULL)
			break; /* no frame */
		if (f->frametype == AST_FRAME_CONTROL && f->subclass == AST_CONTROL_HANGUP)
			done = 1;	/* force a break */
		else if (f->frametype == AST_FRAME_TEXT) {		/* what we want */
			buf = ast_strndup((char *) f->data, f->datalen);	/* dup and break */
			done = 1;
		}
		ast_frfree(f);
	}
	return buf;
}

int ast_sendtext(struct ast_channel *chan, const char *text)
{
	int res = 0;
	/* Stop if we're a zombie or need a soft hangup */
	if (ast_test_flag(chan, AST_FLAG_ZOMBIE) || ast_check_hangup(chan))
		return -1;
	CHECK_BLOCKING(chan);
	if (chan->tech->send_text)
		res = chan->tech->send_text(chan, text);
	ast_clear_flag(chan, AST_FLAG_BLOCKING);
	return res;
}

int ast_senddigit_begin(struct ast_channel *chan, char digit)
{
	int res = -1;

	if (chan->tech->send_digit_begin)
		res = chan->tech->send_digit_begin(chan, digit);

	if (res) {
		/*
		 * Device does not support DTMF tones, lets fake
		 * it by doing our own generation. (PM2002)
		 */
		static const char* dtmf_tones[] = {
			"!941+1336/100,!0/100",	/* 0 */
			"!697+1209/100,!0/100",	/* 1 */
			"!697+1336/100,!0/100",	/* 2 */
			"!697+1477/100,!0/100",	/* 3 */
			"!770+1209/100,!0/100",	/* 4 */
			"!770+1336/100,!0/100",	/* 5 */
			"!770+1477/100,!0/100",	/* 6 */
			"!852+1209/100,!0/100",	/* 7 */
			"!852+1336/100,!0/100",	/* 8 */
			"!852+1477/100,!0/100",	/* 9 */
			"!697+1633/100,!0/100",	/* A */
			"!770+1633/100,!0/100",	/* B */
			"!852+1633/100,!0/100",	/* C */
			"!941+1633/100,!0/100",	/* D */
			"!941+1209/100,!0/100",	/* * */
			"!941+1477/100,!0/100" };	/* # */
		if (digit >= '0' && digit <='9')
			ast_playtones_start(chan, 0, dtmf_tones[digit-'0'], 0);
		else if (digit >= 'A' && digit <= 'D')
			ast_playtones_start(chan, 0, dtmf_tones[digit-'A'+10], 0);
		else if (digit == '*')
			ast_playtones_start(chan, 0, dtmf_tones[14], 0);
		else if (digit == '#')
			ast_playtones_start(chan, 0, dtmf_tones[15], 0);
		else {
			/* not handled */
			if (option_debug)
				ast_log(LOG_DEBUG, "Unable to generate DTMF tone '%c' for '%s'\n", digit, chan->name);
		}
	}

	return 0;
}

int ast_senddigit_end(struct ast_channel *chan, char digit, unsigned int duration)
{
	int res = -1;

	if (chan->tech->send_digit_end)
		res = chan->tech->send_digit_end(chan, digit, duration);

	if (res && chan->generator)
		ast_playtones_stop(chan);
	
	return 0;
}

int ast_senddigit(struct ast_channel *chan, char digit)
{
	if (!ast_test_flag(chan, AST_FLAG_END_DTMF_ONLY)) {
		ast_senddigit_begin(chan, digit);
		ast_safe_sleep(chan, 100); /* XXX 100ms ... probably should be configurable */
	}
	
	return ast_senddigit_end(chan, digit, 100);
}

int ast_prod(struct ast_channel *chan)
{
	struct ast_frame a = { AST_FRAME_VOICE };
	char nothing[128];

	/* Send an empty audio frame to get things moving */
	if (chan->_state != AST_STATE_UP) {
		if (option_debug)
			ast_log(LOG_DEBUG, "Prodding channel '%s'\n", chan->name);
		a.subclass = chan->rawwriteformat;
		a.data = nothing + AST_FRIENDLY_OFFSET;
		a.src = "ast_prod";
		if (ast_write(chan, &a))
			ast_log(LOG_WARNING, "Prodding channel '%s' failed\n", chan->name);
	}
	return 0;
}

int ast_write_video(struct ast_channel *chan, struct ast_frame *fr)
{
	int res;
	if (!chan->tech->write_video)
		return 0;
	res = ast_write(chan, fr);
	if (!res)
		res = 1;
	return res;
}

int ast_write(struct ast_channel *chan, struct ast_frame *fr)
{
	int res = -1;
	struct ast_frame *f = NULL;

	/* Stop if we're a zombie or need a soft hangup */
	ast_channel_lock(chan);
	if (ast_test_flag(chan, AST_FLAG_ZOMBIE) || ast_check_hangup(chan))
		goto done;

	/* Handle any pending masquerades */
	if (chan->masq && ast_do_masquerade(chan)) {
		ast_log(LOG_WARNING, "Failed to perform masquerade\n");
		goto done;
	}
	if (chan->masqr) {
		res = 0;	/* XXX explain, why 0 ? */
		goto done;
	}
	if (chan->generatordata) {
		if (ast_test_flag(chan, AST_FLAG_WRITE_INT))
			ast_deactivate_generator(chan);
		else {
			res = 0;	/* XXX explain, why 0 ? */
			goto done;
		}
	}
	/* High bit prints debugging */
	if (chan->fout & DEBUGCHAN_FLAG)
		ast_frame_dump(chan->name, fr, ">>");
	CHECK_BLOCKING(chan);
	switch(fr->frametype) {
	case AST_FRAME_CONTROL:
		res = (chan->tech->indicate == NULL) ? 0 :
			chan->tech->indicate(chan, fr->subclass, fr->data, fr->datalen);
		break;
	case AST_FRAME_DTMF_BEGIN:
		ast_clear_flag(chan, AST_FLAG_BLOCKING);
		ast_channel_unlock(chan);
		res = ast_senddigit_begin(chan, fr->subclass);
		ast_channel_lock(chan);
		CHECK_BLOCKING(chan);
		break;
	case AST_FRAME_DTMF_END:
		ast_clear_flag(chan, AST_FLAG_BLOCKING);
		ast_channel_unlock(chan);
		res = ast_senddigit_end(chan, fr->subclass, fr->len);
		ast_channel_lock(chan);
		CHECK_BLOCKING(chan);
		break;
	case AST_FRAME_TEXT:
		res = (chan->tech->send_text == NULL) ? 0 :
			chan->tech->send_text(chan, (char *) fr->data);
		break;
	case AST_FRAME_HTML:
		res = (chan->tech->send_html == NULL) ? 0 :
			chan->tech->send_html(chan, fr->subclass, (char *) fr->data, fr->datalen);
		break;
	case AST_FRAME_VIDEO:
		/* XXX Handle translation of video codecs one day XXX */
		res = (chan->tech->write_video == NULL) ? 0 :
			chan->tech->write_video(chan, fr);
		break;
	case AST_FRAME_MODEM:
		res = (chan->tech->write == NULL) ? 0 :
			chan->tech->write(chan, fr);
		break;
	case AST_FRAME_VOICE:
		if (chan->tech->write == NULL)
			break;	/*! \todo XXX should return 0 maybe ? */

		/* If someone is whispering on this channel then we must ensure that we are always getting signed linear frames */
		if (ast_test_flag(chan, AST_FLAG_WHISPER)) {
			if (fr->subclass == AST_FORMAT_SLINEAR)
				f = fr;
			else {
				ast_mutex_lock(&chan->whisper->lock);
				if (chan->writeformat != AST_FORMAT_SLINEAR) {
					/* Rebuild the translation path and set our write format back to signed linear */
					chan->whisper->original_format = chan->writeformat;
					ast_set_write_format(chan, AST_FORMAT_SLINEAR);
					if (chan->whisper->path)
						ast_translator_free_path(chan->whisper->path);
					chan->whisper->path = ast_translator_build_path(AST_FORMAT_SLINEAR, chan->whisper->original_format);
				}
				/* Translate frame using the above translation path */
				f = (chan->whisper->path) ? ast_translate(chan->whisper->path, fr, 0) : fr;
				ast_mutex_unlock(&chan->whisper->lock);
			}
		} else {
			/* If the frame is in the raw write format, then it's easy... just use the frame - otherwise we will have to translate */
			if (fr->subclass == chan->rawwriteformat)
				f = fr;
			else
				f = (chan->writetrans) ? ast_translate(chan->writetrans, fr, 0) : fr;
		}

		/* If we have no frame of audio, then we have to bail out */
		if (f == NULL) {
			res = 0;
			break;
		}

		/* If spies are on the channel then queue the frame out to them */
		if (chan->spies)
			queue_frame_to_spies(chan, f, SPY_WRITE);

		/* If Monitor is running on this channel, then we have to write frames out there too */
		if (chan->monitor && chan->monitor->write_stream) {
			/* XXX must explain this code */
#ifndef MONITOR_CONSTANT_DELAY
			int jump = chan->insmpl - chan->outsmpl - 4 * f->samples;
			if (jump >= 0) {
				jump = chan->insmpl - chan->outsmpl;
				if (ast_seekstream(chan->monitor->write_stream, jump, SEEK_FORCECUR) == -1)
					ast_log(LOG_WARNING, "Failed to perform seek in monitoring write stream, synchronization between the files may be broken\n");
				chan->outsmpl += jump + f->samples;
			} else
				chan->outsmpl += f->samples;
#else
			int jump = chan->insmpl - chan->outsmpl;
			if (jump - MONITOR_DELAY >= 0) {
				if (ast_seekstream(chan->monitor->write_stream, jump - f->samples, SEEK_FORCECUR) == -1)
					ast_log(LOG_WARNING, "Failed to perform seek in monitoring write stream, synchronization between the files may be broken\n");
				chan->outsmpl += jump;
			} else
				chan->outsmpl += f->samples;
#endif
			if (chan->monitor->state == AST_MONITOR_RUNNING) {
				if (ast_writestream(chan->monitor->write_stream, f) < 0)
					ast_log(LOG_WARNING, "Failed to write data to channel monitor write stream\n");
			}
		}

		/* Finally the good part! Write this out to the channel */
		if (ast_test_flag(chan, AST_FLAG_WHISPER)) {
			/* frame is assumed to be in SLINEAR, since that is
			   required for whisper mode */
			ast_frame_adjust_volume(f, -2);
			if (ast_slinfactory_available(&chan->whisper->sf) >= f->samples) {
				short buf[f->samples];
				struct ast_frame whisper = {
					.frametype = AST_FRAME_VOICE,
					.subclass = AST_FORMAT_SLINEAR,
					.data = buf,
					.datalen = sizeof(buf),
					.samples = f->samples,
				};
				
				ast_mutex_lock(&chan->whisper->lock);
				if (ast_slinfactory_read(&chan->whisper->sf, buf, f->samples))
					ast_frame_slinear_sum(f, &whisper);
				ast_mutex_unlock(&chan->whisper->lock);
			}
			/* and now put it through the regular translator */
			f = (chan->writetrans) ? ast_translate(chan->writetrans, f, 0) : f;
		}
		if (f)
			res = chan->tech->write(chan, f);
		else
			res = 0;
		break;
	case AST_FRAME_NULL:
	case AST_FRAME_IAX:
		/* Ignore these */
		res = 0;
		break;
	default:
		/* At this point, fr is the incoming frame and f is NULL.  Channels do
		 * not expect to get NULL as a frame pointer and will segfault.  Hence,
		 * we output the original frame passed in. */
		res = chan->tech->write(chan, fr);
		break;
	}

	if (f && f != fr)
		ast_frfree(f);
	ast_clear_flag(chan, AST_FLAG_BLOCKING);
	/* Consider a write failure to force a soft hangup */
	if (res < 0)
		chan->_softhangup |= AST_SOFTHANGUP_DEV;
	else {
		chan->fout = FRAMECOUNT_INC(chan->fout);
	}
done:
	ast_channel_unlock(chan);
	return res;
}

static int set_format(struct ast_channel *chan, int fmt, int *rawformat, int *format,
		      struct ast_trans_pvt **trans, const int direction)
{
	int native;
	int res;
	
	/* Make sure we only consider audio */
	fmt &= AST_FORMAT_AUDIO_MASK;
	
	native = chan->nativeformats;
	/* Find a translation path from the native format to one of the desired formats */
	if (!direction)
		/* reading */
		res = ast_translator_best_choice(&fmt, &native);
	else
		/* writing */
		res = ast_translator_best_choice(&native, &fmt);

	if (res < 0) {
		ast_log(LOG_WARNING, "Unable to find a codec translation path from %s to %s\n",
			ast_getformatname(native), ast_getformatname(fmt));
		return -1;
	}
	
	/* Now we have a good choice for both. */
	ast_channel_lock(chan);

	if ((*rawformat == native) && (*format == fmt) && ((*rawformat == *format) || (*trans))) {
		/* the channel is already in these formats, so nothing to do */
		ast_channel_unlock(chan);
		return 0;
	}

	*rawformat = native;
	/* User perspective is fmt */
	*format = fmt;
	/* Free any read translation we have right now */
	if (*trans)
		ast_translator_free_path(*trans);
	/* Build a translation path from the raw format to the desired format */
	if (!direction)
		/* reading */
		*trans = ast_translator_build_path(*format, *rawformat);
	else
		/* writing */
		*trans = ast_translator_build_path(*rawformat, *format);
	ast_channel_unlock(chan);
	if (option_debug)
		ast_log(LOG_DEBUG, "Set channel %s to %s format %s\n", chan->name,
			direction ? "write" : "read", ast_getformatname(fmt));
	return 0;
}

int ast_set_read_format(struct ast_channel *chan, int fmt)
{
	return set_format(chan, fmt, &chan->rawreadformat, &chan->readformat,
			  &chan->readtrans, 0);
}

int ast_set_write_format(struct ast_channel *chan, int fmt)
{
	return set_format(chan, fmt, &chan->rawwriteformat, &chan->writeformat,
			  &chan->writetrans, 1);
}

struct ast_channel *__ast_request_and_dial(const char *type, int format, void *data, int timeout, int *outstate, const char *cid_num, const char *cid_name, struct outgoing_helper *oh)
{
	int dummy_outstate;
	int cause = 0;
	struct ast_channel *chan;
	int res = 0;
	
	if (outstate)
		*outstate = 0;
	else
		outstate = &dummy_outstate;	/* make outstate always a valid pointer */

	chan = ast_request(type, format, data, &cause);
	if (!chan) {
		ast_log(LOG_NOTICE, "Unable to request channel %s/%s\n", type, (char *)data);
		/* compute error and return */
		if (cause == AST_CAUSE_BUSY)
			*outstate = AST_CONTROL_BUSY;
		else if (cause == AST_CAUSE_CONGESTION)
			*outstate = AST_CONTROL_CONGESTION;
		return NULL;
	}

	if (oh) {
		if (oh->vars)	
			ast_set_variables(chan, oh->vars);
		/* XXX why is this necessary, for the parent_channel perhaps ? */
		if (!ast_strlen_zero(oh->cid_num) && !ast_strlen_zero(oh->cid_name))
			ast_set_callerid(chan, oh->cid_num, oh->cid_name, oh->cid_num);
		if (oh->parent_channel)
			ast_channel_inherit_variables(oh->parent_channel, chan);
		if (oh->account)
			ast_cdr_setaccount(chan, oh->account);	
	}
	ast_set_callerid(chan, cid_num, cid_name, cid_num);

	

	if (!chan->cdr) { /* up till now, this insertion hasn't been done. Therefore,
				to keep from throwing off the basic order of the universe,
				we will try to keep this cdr from getting posted. */
		chan->cdr = ast_cdr_alloc();
		ast_cdr_init(chan->cdr, chan);
		ast_cdr_start(chan->cdr);
	}
	if (ast_call(chan, data, 0)) {	/* ast_call failed... */
		ast_log(LOG_NOTICE, "Unable to call channel %s/%s\n", type, (char *)data);
	} else {
		res = 1;	/* mark success in case chan->_state is already AST_STATE_UP */
		while (timeout && chan->_state != AST_STATE_UP) {
			struct ast_frame *f;
			res = ast_waitfor(chan, timeout);
			if (res <= 0) /* error, timeout, or done */
				break;
			if (timeout > -1)
				timeout = res;
			f = ast_read(chan);
			if (!f) {
				*outstate = AST_CONTROL_HANGUP;
				res = 0;
				break;
			}
			if (f->frametype == AST_FRAME_CONTROL) {
				switch (f->subclass) {
				case AST_CONTROL_RINGING:	/* record but keep going */
					*outstate = f->subclass;
					break;

				case AST_CONTROL_BUSY:
				case AST_CONTROL_CONGESTION:
				case AST_CONTROL_ANSWER:
					*outstate = f->subclass;
					timeout = 0;		/* trick to force exit from the while() */
					break;

				/* Ignore these */
				case AST_CONTROL_PROGRESS:
				case AST_CONTROL_PROCEEDING:
				case AST_CONTROL_HOLD:
				case AST_CONTROL_UNHOLD:
				case AST_CONTROL_VIDUPDATE:
				case -1:			/* Ignore -- just stopping indications */
					break;

				default:
					ast_log(LOG_NOTICE, "Don't know what to do with control frame %d\n", f->subclass);
				}
			}
			ast_frfree(f);
		}
	}

	/* Final fixups */
	if (oh) {
		if (!ast_strlen_zero(oh->context))
			ast_copy_string(chan->context, oh->context, sizeof(chan->context));
		if (!ast_strlen_zero(oh->exten))
			ast_copy_string(chan->exten, oh->exten, sizeof(chan->exten));
		if (oh->priority)	
			chan->priority = oh->priority;
	}
	if (chan->_state == AST_STATE_UP)
		*outstate = AST_CONTROL_ANSWER;

	if (res <= 0) {
		if (!chan->cdr && (chan->cdr = ast_cdr_alloc()))
			ast_cdr_init(chan->cdr, chan);
		if (chan->cdr) {
			char tmp[256];
			snprintf(tmp, sizeof(tmp), "%s/%s", type, (char *)data);
			ast_cdr_setapp(chan->cdr,"Dial",tmp);
			ast_cdr_update(chan);
			ast_cdr_start(chan->cdr);
			ast_cdr_end(chan->cdr);
			/* If the cause wasn't handled properly */
			if (ast_cdr_disposition(chan->cdr,chan->hangupcause))
				ast_cdr_failed(chan->cdr);
		}
		ast_hangup(chan);
		chan = NULL;
	}
	return chan;
}

struct ast_channel *ast_request_and_dial(const char *type, int format, void *data, int timeout, int *outstate, const char *cidnum, const char *cidname)
{
	return __ast_request_and_dial(type, format, data, timeout, outstate, cidnum, cidname, NULL);
}

struct ast_channel *ast_request(const char *type, int format, void *data, int *cause)
{
	struct chanlist *chan;
	struct ast_channel *c;
	int capabilities;
	int fmt;
	int res;
	int foo;
	int videoformat = format & AST_FORMAT_VIDEO_MASK;

	if (!cause)
		cause = &foo;
	*cause = AST_CAUSE_NOTDEFINED;

	if (AST_LIST_LOCK(&channels)) {
		ast_log(LOG_WARNING, "Unable to lock channel list\n");
		return NULL;
	}

	AST_LIST_TRAVERSE(&backends, chan, list) {
		if (strcasecmp(type, chan->tech->type))
			continue;

		capabilities = chan->tech->capabilities;
		fmt = format & AST_FORMAT_AUDIO_MASK;
		res = ast_translator_best_choice(&fmt, &capabilities);
		if (res < 0) {
			ast_log(LOG_WARNING, "No translator path exists for channel type %s (native %d) to %d\n", type, chan->tech->capabilities, format);
			AST_LIST_UNLOCK(&channels);
			return NULL;
		}
		AST_LIST_UNLOCK(&channels);
		if (!chan->tech->requester)
			return NULL;
		
		if (!(c = chan->tech->requester(type, capabilities | videoformat, data, cause)))
			return NULL;

		/* no need to generate a Newchannel event here; it is done in the channel_alloc call */
		return c;
	}

	ast_log(LOG_WARNING, "No channel type registered for '%s'\n", type);
	*cause = AST_CAUSE_NOSUCHDRIVER;
	AST_LIST_UNLOCK(&channels);

	return NULL;
}

int ast_call(struct ast_channel *chan, char *addr, int timeout)
{
	/* Place an outgoing call, but don't wait any longer than timeout ms before returning.
	   If the remote end does not answer within the timeout, then do NOT hang up, but
	   return anyway.  */
	int res = -1;
	/* Stop if we're a zombie or need a soft hangup */
	ast_channel_lock(chan);
	if (!ast_test_flag(chan, AST_FLAG_ZOMBIE) && !ast_check_hangup(chan)) {
		if (chan->tech->call)
			res = chan->tech->call(chan, addr, timeout);
		ast_set_flag(chan, AST_FLAG_OUTGOING);
	}
	ast_channel_unlock(chan);
	return res;
}

/*!
  \brief Transfer a call to dest, if the channel supports transfer

  Called by:
    \arg app_transfer
    \arg the manager interface
*/
int ast_transfer(struct ast_channel *chan, char *dest)
{
	int res = -1;

	/* Stop if we're a zombie or need a soft hangup */
	ast_channel_lock(chan);
	if (!ast_test_flag(chan, AST_FLAG_ZOMBIE) && !ast_check_hangup(chan)) {
		if (chan->tech->transfer) {
			res = chan->tech->transfer(chan, dest);
			if (!res)
				res = 1;
		} else
			res = 0;
	}
	ast_channel_unlock(chan);
	return res;
}

int ast_readstring(struct ast_channel *c, char *s, int len, int timeout, int ftimeout, char *enders)
{
	return ast_readstring_full(c, s, len, timeout, ftimeout, enders, -1, -1);
}

int ast_readstring_full(struct ast_channel *c, char *s, int len, int timeout, int ftimeout, char *enders, int audiofd, int ctrlfd)
{
	int pos = 0;	/* index in the buffer where we accumulate digits */
	int to = ftimeout;

	/* Stop if we're a zombie or need a soft hangup */
	if (ast_test_flag(c, AST_FLAG_ZOMBIE) || ast_check_hangup(c))
		return -1;
	if (!len)
		return -1;
	for (;;) {
		int d;
		if (c->stream) {
			d = ast_waitstream_full(c, AST_DIGIT_ANY, audiofd, ctrlfd);
			ast_stopstream(c);
			usleep(1000);
			if (!d)
				d = ast_waitfordigit_full(c, to, audiofd, ctrlfd);
		} else {
			d = ast_waitfordigit_full(c, to, audiofd, ctrlfd);
		}
		if (d < 0)
			return -1;
		if (d == 0) {
			s[pos]='\0';
			return 1;
		}
		if (d == 1) {
			s[pos]='\0';
			return 2;
		}
		if (!strchr(enders, d))
			s[pos++] = d;
		if (strchr(enders, d) || (pos >= len)) {
			s[pos]='\0';
			return 0;
		}
		to = timeout;
	}
	/* Never reached */
	return 0;
}

int ast_channel_supports_html(struct ast_channel *chan)
{
	return (chan->tech->send_html) ? 1 : 0;
}

int ast_channel_sendhtml(struct ast_channel *chan, int subclass, const char *data, int datalen)
{
	if (chan->tech->send_html)
		return chan->tech->send_html(chan, subclass, data, datalen);
	return -1;
}

int ast_channel_sendurl(struct ast_channel *chan, const char *url)
{
	return ast_channel_sendhtml(chan, AST_HTML_URL, url, strlen(url) + 1);
}

int ast_channel_make_compatible(struct ast_channel *chan, struct ast_channel *peer)
{
	int src;
	int dst;

	/* Set up translation from the chan to the peer */
	src = chan->nativeformats;
	dst = peer->nativeformats;
	if (ast_translator_best_choice(&dst, &src) < 0) {
		ast_log(LOG_WARNING, "No path to translate from %s(%d) to %s(%d)\n", chan->name, src, peer->name, dst);
		return -1;
	}

	/* if the best path is not 'pass through', then
	   transcoding is needed; if desired, force transcode path
	   to use SLINEAR between channels, but only if there is
	   no direct conversion available */
	if ((src != dst) && ast_opt_transcode_via_slin &&
	    (ast_translate_path_steps(dst, src) != 1))
		dst = AST_FORMAT_SLINEAR;
	if (ast_set_read_format(chan, dst) < 0) {
		ast_log(LOG_WARNING, "Unable to set read format on channel %s to %d\n", chan->name, dst);
		return -1;
	}
	if (ast_set_write_format(peer, dst) < 0) {
		ast_log(LOG_WARNING, "Unable to set write format on channel %s to %d\n", peer->name, dst);
		return -1;
	}

	/* Set up translation from the peer to the chan */
	src = peer->nativeformats;
	dst = chan->nativeformats;
	if (ast_translator_best_choice(&dst, &src) < 0) {
		ast_log(LOG_WARNING, "No path to translate from %s(%d) to %s(%d)\n", peer->name, src, chan->name, dst);
		return -1;
	}

	/* if the best path is not 'pass through', then
	   transcoding is needed; if desired, force transcode path
	   to use SLINEAR between channels, but only if there is
	   no direct conversion available */
	if ((src != dst) && ast_opt_transcode_via_slin &&
	    (ast_translate_path_steps(dst, src) != 1))
		dst = AST_FORMAT_SLINEAR;
	if (ast_set_read_format(peer, dst) < 0) {
		ast_log(LOG_WARNING, "Unable to set read format on channel %s to %d\n", peer->name, dst);
		return -1;
	}
	if (ast_set_write_format(chan, dst) < 0) {
		ast_log(LOG_WARNING, "Unable to set write format on channel %s to %d\n", chan->name, dst);
		return -1;
	}
	return 0;
}

int ast_channel_masquerade(struct ast_channel *original, struct ast_channel *clone)
{
	int res = -1;
	struct ast_channel *final_orig = original, *final_clone = clone;

	ast_channel_lock(original);
	while (ast_channel_trylock(clone)) {
		ast_channel_unlock(original);
		usleep(1);
		ast_channel_lock(original);
	}

	/* each of these channels may be sitting behind a channel proxy (i.e. chan_agent)
	   and if so, we don't really want to masquerade it, but its proxy */
	if (original->_bridge && (original->_bridge != ast_bridged_channel(original)) && (original->_bridge->_bridge != original))
		final_orig = original->_bridge;

	if (clone->_bridge && (clone->_bridge != ast_bridged_channel(clone)) && (clone->_bridge->_bridge != clone))
		final_clone = clone->_bridge;

	if ((final_orig != original) || (final_clone != clone)) {
		ast_channel_lock(final_orig);
		while (ast_channel_trylock(final_clone)) {
			ast_channel_unlock(final_orig);
			usleep(1);
			ast_channel_lock(final_orig);
		}
		ast_channel_unlock(clone);
		ast_channel_unlock(original);
		original = final_orig;
		clone = final_clone;
	}

	if (original == clone) {
		ast_log(LOG_WARNING, "Can't masquerade channel '%s' into itself!\n", original->name);
		ast_channel_unlock(clone);
		ast_channel_unlock(original);
		return -1;
	}

	if (option_debug)
		ast_log(LOG_DEBUG, "Planning to masquerade channel %s into the structure of %s\n",
			clone->name, original->name);
	if (original->masq) {
		ast_log(LOG_WARNING, "%s is already going to masquerade as %s\n",
			original->masq->name, original->name);
	} else if (clone->masqr) {
		ast_log(LOG_WARNING, "%s is already going to masquerade as %s\n",
			clone->name, clone->masqr->name);
	} else {
		original->masq = clone;
		clone->masqr = original;
		ast_queue_frame(original, &ast_null_frame);
		ast_queue_frame(clone, &ast_null_frame);
		if (option_debug)
			ast_log(LOG_DEBUG, "Done planning to masquerade channel %s into the structure of %s\n", clone->name, original->name);
		res = 0;
	}

	ast_channel_unlock(clone);
	ast_channel_unlock(original);

	return res;
}

void ast_change_name(struct ast_channel *chan, char *newname)
{
	manager_event(EVENT_FLAG_CALL, "Rename", "Oldname: %s\r\nNewname: %s\r\nUniqueid: %s\r\n", chan->name, newname, chan->uniqueid);
	ast_string_field_set(chan, name, newname);
}

void ast_channel_inherit_variables(const struct ast_channel *parent, struct ast_channel *child)
{
	struct ast_var_t *current, *newvar;
	const char *varname;

	AST_LIST_TRAVERSE(&parent->varshead, current, entries) {
		int vartype = 0;

		varname = ast_var_full_name(current);
		if (!varname)
			continue;

		if (varname[0] == '_') {
			vartype = 1;
			if (varname[1] == '_')
				vartype = 2;
		}

		switch (vartype) {
		case 1:
			newvar = ast_var_assign(&varname[1], ast_var_value(current));
			if (newvar) {
				AST_LIST_INSERT_TAIL(&child->varshead, newvar, entries);
				if (option_debug)
					ast_log(LOG_DEBUG, "Copying soft-transferable variable %s.\n", ast_var_name(newvar));
			}
			break;
		case 2:
			newvar = ast_var_assign(ast_var_full_name(current), ast_var_value(current));
			if (newvar) {
				AST_LIST_INSERT_TAIL(&child->varshead, newvar, entries);
				if (option_debug)
					ast_log(LOG_DEBUG, "Copying hard-transferable variable %s.\n", ast_var_name(newvar));
			}
			break;
		default:
			if (option_debug)
				ast_log(LOG_DEBUG, "Not copying variable %s.\n", ast_var_name(current));
			break;
		}
	}
}

/*!
  \brief Clone channel variables from 'clone' channel into 'original' channel

  All variables except those related to app_groupcount are cloned.
  Variables are actually _removed_ from 'clone' channel, presumably
  because it will subsequently be destroyed.

  \note Assumes locks will be in place on both channels when called.
*/
static void clone_variables(struct ast_channel *original, struct ast_channel *clone)
{
	struct ast_var_t *varptr;

	/* we need to remove all app_groupcount related variables from the original
	   channel before merging in the clone's variables; any groups assigned to the
	   original channel should be released, only those assigned to the clone
	   should remain
	*/

	AST_LIST_TRAVERSE_SAFE_BEGIN(&original->varshead, varptr, entries) {
		if (!strncmp(ast_var_name(varptr), GROUP_CATEGORY_PREFIX, strlen(GROUP_CATEGORY_PREFIX))) {
			AST_LIST_REMOVE_CURRENT(&original->varshead, entries);
			ast_var_delete(varptr);
		}
	}
	AST_LIST_TRAVERSE_SAFE_END;

	/* Append variables from clone channel into original channel */
	/* XXX Is this always correct?  We have to in order to keep MACROS working XXX */
	if (AST_LIST_FIRST(&clone->varshead))
		AST_LIST_APPEND_LIST(&original->varshead, &clone->varshead, entries);
}

/*!
  \brief Masquerade a channel

  \note Assumes channel will be locked when called
*/
int ast_do_masquerade(struct ast_channel *original)
{
	int x,i;
	int res=0;
	int origstate;
	struct ast_frame *cur;
	const struct ast_channel_tech *t;
	void *t_pvt;
	struct ast_callerid tmpcid;
	struct ast_channel *clone = original->masq;
	struct ast_channel_spy_list *spy_list = NULL;
	struct ast_channel_spy *spy = NULL;
	struct ast_cdr *cdr;
	int rformat = original->readformat;
	int wformat = original->writeformat;
	char newn[100];
	char orig[100];
	char masqn[100];
	char zombn[100];

	if (option_debug > 3)
		ast_log(LOG_DEBUG, "Actually Masquerading %s(%d) into the structure of %s(%d)\n",
			clone->name, clone->_state, original->name, original->_state);

	/* XXX This is a seriously wacked out operation.  We're essentially putting the guts of
	   the clone channel into the original channel.  Start by killing off the original
	   channel's backend.   I'm not sure we're going to keep this function, because
	   while the features are nice, the cost is very high in terms of pure nastiness. XXX */

	/* We need the clone's lock, too */
	ast_channel_lock(clone);

	if (option_debug > 1)
		ast_log(LOG_DEBUG, "Got clone lock for masquerade on '%s' at %p\n", clone->name, &clone->lock);

	/* Having remembered the original read/write formats, we turn off any translation on either
	   one */
	free_translation(clone);
	free_translation(original);


	/* Unlink the masquerade */
	original->masq = NULL;
	clone->masqr = NULL;
	
	/* Save the original name */
	ast_copy_string(orig, original->name, sizeof(orig));
	/* Save the new name */
	ast_copy_string(newn, clone->name, sizeof(newn));
	/* Create the masq name */
	snprintf(masqn, sizeof(masqn), "%s<MASQ>", newn);
		
	/* Copy the name from the clone channel */
	ast_string_field_set(original, name, newn);

	/* Mangle the name of the clone channel */
	ast_string_field_set(clone, name, masqn);
	
	/* Notify any managers of the change, first the masq then the other */
	manager_event(EVENT_FLAG_CALL, "Rename", "Oldname: %s\r\nNewname: %s\r\nUniqueid: %s\r\n", newn, masqn, clone->uniqueid);
	manager_event(EVENT_FLAG_CALL, "Rename", "Oldname: %s\r\nNewname: %s\r\nUniqueid: %s\r\n", orig, newn, original->uniqueid);

	/* Swap the technologies */	
	t = original->tech;
	original->tech = clone->tech;
	clone->tech = t;

	/* Swap the cdrs */
	cdr = original->cdr;
	original->cdr = clone->cdr;
	clone->cdr = cdr;

	t_pvt = original->tech_pvt;
	original->tech_pvt = clone->tech_pvt;
	clone->tech_pvt = t_pvt;

	/* Swap the readq's */
	cur = AST_LIST_FIRST(&original->readq);
	AST_LIST_HEAD_SET_NOLOCK(&original->readq, AST_LIST_FIRST(&clone->readq));
	AST_LIST_HEAD_SET_NOLOCK(&clone->readq, cur);

	/* Swap the alertpipes */
	for (i = 0; i < 2; i++) {
		x = original->alertpipe[i];
		original->alertpipe[i] = clone->alertpipe[i];
		clone->alertpipe[i] = x;
	}

	/* Swap the raw formats */
	x = original->rawreadformat;
	original->rawreadformat = clone->rawreadformat;
	clone->rawreadformat = x;
	x = original->rawwriteformat;
	original->rawwriteformat = clone->rawwriteformat;
	clone->rawwriteformat = x;

	/* Swap the spies */
	spy_list = original->spies;
	original->spies = clone->spies;
	clone->spies = spy_list;

	/* Update channel on respective spy lists if present */
	if (original->spies) {
		AST_LIST_TRAVERSE(&original->spies->list, spy, list) {
			ast_mutex_lock(&spy->lock);
			spy->chan = original;
			ast_mutex_unlock(&spy->lock);
		}
	}
	if (clone->spies) {
		AST_LIST_TRAVERSE(&clone->spies->list, spy, list) {
			ast_mutex_lock(&spy->lock);
			spy->chan = clone;
			ast_mutex_unlock(&spy->lock);
		}
	}

	/* Save any pending frames on both sides.  Start by counting
	 * how many we're going to need... */
	x = 0;
	if (original->alertpipe[1] > -1) {
		AST_LIST_TRAVERSE(&clone->readq, cur, frame_list)
			x++;
	}

	/* If we had any, prepend them to the ones already in the queue, and 
	 * load up the alertpipe */
	if (AST_LIST_FIRST(&clone->readq)) {
		AST_LIST_INSERT_TAIL(&clone->readq, AST_LIST_FIRST(&original->readq), frame_list);
		AST_LIST_HEAD_SET_NOLOCK(&original->readq, AST_LIST_FIRST(&clone->readq));
		AST_LIST_HEAD_SET_NOLOCK(&clone->readq, NULL);
		for (i = 0; i < x; i++)
			write(original->alertpipe[1], &x, sizeof(x));
	}
	
	clone->_softhangup = AST_SOFTHANGUP_DEV;


	/* And of course, so does our current state.  Note we need not
	   call ast_setstate since the event manager doesn't really consider
	   these separate.  We do this early so that the clone has the proper
	   state of the original channel. */
	origstate = original->_state;
	original->_state = clone->_state;
	clone->_state = origstate;

	if (clone->tech->fixup){
		res = clone->tech->fixup(original, clone);
		if (res)
			ast_log(LOG_WARNING, "Fixup failed on channel %s, strange things may happen.\n", clone->name);
	}

	/* Start by disconnecting the original's physical side */
	if (clone->tech->hangup)
		res = clone->tech->hangup(clone);
	if (res) {
		ast_log(LOG_WARNING, "Hangup failed!  Strange things may happen!\n");
		ast_channel_unlock(clone);
		return -1;
	}
	
	snprintf(zombn, sizeof(zombn), "%s<ZOMBIE>", orig);
	/* Mangle the name of the clone channel */
	ast_string_field_set(clone, name, zombn);
	manager_event(EVENT_FLAG_CALL, "Rename", "Oldname: %s\r\nNewname: %s\r\nUniqueid: %s\r\n", masqn, zombn, clone->uniqueid);

	/* Update the type. */
	t_pvt = original->monitor;
	original->monitor = clone->monitor;
	clone->monitor = t_pvt;
	
	/* Keep the same language.  */
	ast_string_field_set(original, language, clone->language);
	/* Copy the FD's other than the generator fd */
	for (x = 0; x < AST_MAX_FDS; x++) {
		if (x != AST_GENERATOR_FD)
			original->fds[x] = clone->fds[x];
	}

	/* move any whisperer over */
	ast_channel_whisper_stop(original);
	if (ast_test_flag(clone, AST_FLAG_WHISPER)) {
		original->whisper = clone->whisper;
		ast_set_flag(original, AST_FLAG_WHISPER);
		clone->whisper = NULL;
		ast_clear_flag(clone, AST_FLAG_WHISPER);
	}

	/* Move data stores over */
	if (AST_LIST_FIRST(&clone->datastores))
                AST_LIST_INSERT_TAIL(&original->datastores, AST_LIST_FIRST(&clone->datastores), entry);
	AST_LIST_HEAD_INIT_NOLOCK(&clone->datastores);

	clone_variables(original, clone);
	AST_LIST_HEAD_INIT_NOLOCK(&clone->varshead);
	/* Presense of ADSI capable CPE follows clone */
	original->adsicpe = clone->adsicpe;
	/* Bridge remains the same */
	/* CDR fields remain the same */
	/* XXX What about blocking, softhangup, blocker, and lock and blockproc? XXX */
	/* Application and data remain the same */
	/* Clone exception  becomes real one, as with fdno */
	ast_copy_flags(original, clone, AST_FLAG_EXCEPTION);
	original->fdno = clone->fdno;
	/* Schedule context remains the same */
	/* Stream stuff stays the same */
	/* Keep the original state.  The fixup code will need to work with it most likely */

	/* Just swap the whole structures, nevermind the allocations, they'll work themselves
	   out. */
	tmpcid = original->cid;
	original->cid = clone->cid;
	clone->cid = tmpcid;
	
	/* Restore original timing file descriptor */
	original->fds[AST_TIMING_FD] = original->timingfd;
	
	/* Our native formats are different now */
	original->nativeformats = clone->nativeformats;
	
	/* Context, extension, priority, app data, jump table,  remain the same */
	/* pvt switches.  pbx stays the same, as does next */
	
	/* Set the write format */
	ast_set_write_format(original, wformat);

	/* Set the read format */
	ast_set_read_format(original, rformat);

	/* Copy the music class */
	ast_string_field_set(original, musicclass, clone->musicclass);

	if (option_debug)
		ast_log(LOG_DEBUG, "Putting channel %s in %d/%d formats\n", original->name, wformat, rformat);

	/* Okay.  Last thing is to let the channel driver know about all this mess, so he
	   can fix up everything as best as possible */
	if (original->tech->fixup) {
		res = original->tech->fixup(clone, original);
		if (res) {
			ast_log(LOG_WARNING, "Channel for type '%s' could not fixup channel %s\n",
				original->tech->type, original->name);
			ast_channel_unlock(clone);
			return -1;
		}
	} else
		ast_log(LOG_WARNING, "Channel type '%s' does not have a fixup routine (for %s)!  Bad things may happen.\n",
			original->tech->type, original->name);
	
	/* Now, at this point, the "clone" channel is totally F'd up.  We mark it as
	   a zombie so nothing tries to touch it.  If it's already been marked as a
	   zombie, then free it now (since it already is considered invalid). */
	if (ast_test_flag(clone, AST_FLAG_ZOMBIE)) {
		if (option_debug)
			ast_log(LOG_DEBUG, "Destroying channel clone '%s'\n", clone->name);
		ast_channel_unlock(clone);
		manager_event(EVENT_FLAG_CALL, "Hangup",
			"Channel: %s\r\n"
			"Uniqueid: %s\r\n"
			"Cause: %d\r\n"
			"Cause-txt: %s\r\n",
			clone->name,
			clone->uniqueid,
			clone->hangupcause,
			ast_cause2str(clone->hangupcause)
			);
		ast_channel_free(clone);
	} else {
		if (option_debug)
			ast_log(LOG_DEBUG, "Released clone lock on '%s'\n", clone->name);
		ast_set_flag(clone, AST_FLAG_ZOMBIE);
		ast_queue_frame(clone, &ast_null_frame);
		ast_channel_unlock(clone);
	}
	
	/* Signal any blocker */
	if (ast_test_flag(original, AST_FLAG_BLOCKING))
		pthread_kill(original->blocker, SIGURG);
	if (option_debug)
		ast_log(LOG_DEBUG, "Done Masquerading %s (%d)\n", original->name, original->_state);
	return 0;
}

void ast_set_callerid(struct ast_channel *chan, const char *callerid, const char *calleridname, const char *ani)
{
	if (callerid) {
		if (chan->cid.cid_num)
			free(chan->cid.cid_num);
		chan->cid.cid_num = ast_strdup(callerid);
	}
	if (calleridname) {
		if (chan->cid.cid_name)
			free(chan->cid.cid_name);
		chan->cid.cid_name = ast_strdup(calleridname);
	}
	if (ani) {
		if (chan->cid.cid_ani)
			free(chan->cid.cid_ani);
		chan->cid.cid_ani = ast_strdup(ani);
	}
	if (chan->cdr)
		ast_cdr_setcid(chan->cdr, chan);
	manager_event(EVENT_FLAG_CALL, "Newcallerid",
				"Channel: %s\r\n"
				"CallerID: %s\r\n"
				"CallerIDName: %s\r\n"
				"Uniqueid: %s\r\n"
				"CID-CallingPres: %d (%s)\r\n",
				chan->name,
				S_OR(chan->cid.cid_num, "<Unknown>"),
				S_OR(chan->cid.cid_name, "<Unknown>"),
				chan->uniqueid,
				chan->cid.cid_pres,
				ast_describe_caller_presentation(chan->cid.cid_pres)
				);
}

int ast_setstate(struct ast_channel *chan, enum ast_channel_state state)
{
	int oldstate = chan->_state;

	if (oldstate == state)
		return 0;

	chan->_state = state;
	ast_device_state_changed_literal(chan->name);
	/* setstate used to conditionally report Newchannel; this is no more */
	manager_event(EVENT_FLAG_CALL,
		      "Newstate",
		      "Channel: %s\r\n"
		      "State: %s\r\n"
		      "CallerID: %s\r\n"
		      "CallerIDName: %s\r\n"
		      "Uniqueid: %s\r\n",
		      chan->name, ast_state2str(chan->_state),
		      S_OR(chan->cid.cid_num, "<unknown>"),
		      S_OR(chan->cid.cid_name, "<unknown>"),
		      chan->uniqueid);

	return 0;
}

/*! \brief Find bridged channel */
struct ast_channel *ast_bridged_channel(struct ast_channel *chan)
{
	struct ast_channel *bridged;
	bridged = chan->_bridge;
	if (bridged && bridged->tech->bridged_channel)
		bridged = bridged->tech->bridged_channel(chan, bridged);
	return bridged;
}

static void bridge_playfile(struct ast_channel *chan, struct ast_channel *peer, const char *sound, int remain)
{
	int min = 0, sec = 0, check;

	check = ast_autoservice_start(peer);
	if (check)
		return;

	if (remain > 0) {
		if (remain / 60 > 1) {
			min = remain / 60;
			sec = remain % 60;
		} else {
			sec = remain;
		}
	}
	
	if (!strcmp(sound,"timeleft")) {	/* Queue support */
		ast_stream_and_wait(chan, "vm-youhave", chan->language, "");
		if (min) {
			ast_say_number(chan, min, AST_DIGIT_ANY, chan->language, NULL);
			ast_stream_and_wait(chan, "queue-minutes", chan->language, "");
		}
		if (sec) {
			ast_say_number(chan, sec, AST_DIGIT_ANY, chan->language, NULL);
			ast_stream_and_wait(chan, "queue-seconds", chan->language, "");
		}
	} else {
		ast_stream_and_wait(chan, sound, chan->language, "");
	}

	ast_autoservice_stop(peer);
}

static enum ast_bridge_result ast_generic_bridge(struct ast_channel *c0, struct ast_channel *c1,
						 struct ast_bridge_config *config, struct ast_frame **fo,
						 struct ast_channel **rc, struct timeval bridge_end)
{
	/* Copy voice back and forth between the two channels. */
	struct ast_channel *cs[3];
	struct ast_frame *f;
	enum ast_bridge_result res = AST_BRIDGE_COMPLETE;
	int o0nativeformats;
	int o1nativeformats;
	int watch_c0_dtmf;
	int watch_c1_dtmf;
	void *pvt0, *pvt1;
	/* Indicates whether a frame was queued into a jitterbuffer */
	int frame_put_in_jb = 0;
	int jb_in_use;
	int to;
	
	cs[0] = c0;
	cs[1] = c1;
	pvt0 = c0->tech_pvt;
	pvt1 = c1->tech_pvt;
	o0nativeformats = c0->nativeformats;
	o1nativeformats = c1->nativeformats;
	watch_c0_dtmf = config->flags & AST_BRIDGE_DTMF_CHANNEL_0;
	watch_c1_dtmf = config->flags & AST_BRIDGE_DTMF_CHANNEL_1;

	/* Check the need of a jitterbuffer for each channel */
	jb_in_use = ast_jb_do_usecheck(c0, c1);

	for (;;) {
		struct ast_channel *who, *other;

		if ((c0->tech_pvt != pvt0) || (c1->tech_pvt != pvt1) ||
		    (o0nativeformats != c0->nativeformats) ||
		    (o1nativeformats != c1->nativeformats)) {
			/* Check for Masquerade, codec changes, etc */
			res = AST_BRIDGE_RETRY;
			break;
		}
		if (bridge_end.tv_sec) {
			to = ast_tvdiff_ms(bridge_end, ast_tvnow());
			if (to <= 0) {
				if (config->timelimit)
					res = AST_BRIDGE_RETRY;
				else
					res = AST_BRIDGE_COMPLETE;
				break;
			}
		} else
			to = -1;
		/* Calculate the appropriate max sleep interval - in general, this is the time,
		   left to the closest jb delivery moment */
		if (jb_in_use)
			to = ast_jb_get_when_to_wakeup(c0, c1, to);
		who = ast_waitfor_n(cs, 2, &to);
		if (!who) {
			/* No frame received within the specified timeout - check if we have to deliver now */
			if (jb_in_use)
				ast_jb_get_and_deliver(c0, c1);
			if (c0->_softhangup == AST_SOFTHANGUP_UNBRIDGE || c1->_softhangup == AST_SOFTHANGUP_UNBRIDGE) {
				if (c0->_softhangup == AST_SOFTHANGUP_UNBRIDGE)
					c0->_softhangup = 0;
				if (c1->_softhangup == AST_SOFTHANGUP_UNBRIDGE)
					c1->_softhangup = 0;
				c0->_bridge = c1;
				c1->_bridge = c0;
			}
			continue;
		}
		f = ast_read(who);
		if (!f) {
			*fo = NULL;
			*rc = who;
			if (option_debug)
				ast_log(LOG_DEBUG, "Didn't get a frame from channel: %s\n",who->name);
			break;
		}

		other = (who == c0) ? c1 : c0; /* the 'other' channel */
		/* Try add the frame info the who's bridged channel jitterbuff */
		if (jb_in_use)
			frame_put_in_jb = !ast_jb_put(other, f);

		if ((f->frametype == AST_FRAME_CONTROL) && !(config->flags & AST_BRIDGE_IGNORE_SIGS)) {
			int bridge_exit = 0;

			switch (f->subclass) {
			case AST_CONTROL_HOLD:
			case AST_CONTROL_UNHOLD:
			case AST_CONTROL_VIDUPDATE:
				ast_indicate_data(other, f->subclass, f->data, f->datalen);
				break;
			default:
				*fo = f;
				*rc = who;
				bridge_exit = 1;
				if (option_debug)
					ast_log(LOG_DEBUG, "Got a FRAME_CONTROL (%d) frame on channel %s\n", f->subclass, who->name);
				break;
			}
			if (bridge_exit)
				break;
		}
		if ((f->frametype == AST_FRAME_VOICE) ||
		    (f->frametype == AST_FRAME_DTMF_BEGIN) ||
		    (f->frametype == AST_FRAME_DTMF) ||
		    (f->frametype == AST_FRAME_VIDEO) ||
		    (f->frametype == AST_FRAME_IMAGE) ||
		    (f->frametype == AST_FRAME_HTML) ||
		    (f->frametype == AST_FRAME_MODEM) ||
		    (f->frametype == AST_FRAME_TEXT)) {
			/* monitored dtmf causes exit from bridge */
			int monitored_source = (who == c0) ? watch_c0_dtmf : watch_c1_dtmf;

			if (monitored_source && 
				(f->frametype == AST_FRAME_DTMF_END || 
				f->frametype == AST_FRAME_DTMF_BEGIN)) {
				*fo = f;
				*rc = who;
				if (option_debug)
					ast_log(LOG_DEBUG, "Got DTMF %s on channel (%s)\n", 
						f->frametype == AST_FRAME_DTMF_END ? "end" : "begin",
						who->name);
				break;
			}
			/* Write immediately frames, not passed through jb */
			if (!frame_put_in_jb)
				ast_write(other, f);
				
			/* Check if we have to deliver now */
			if (jb_in_use)
				ast_jb_get_and_deliver(c0, c1);
		}
		/* XXX do we want to pass on also frames not matched above ? */
		ast_frfree(f);

		/* Swap who gets priority */
		cs[2] = cs[0];
		cs[0] = cs[1];
		cs[1] = cs[2];
	}
	return res;
}

/*! \brief Bridge two channels together */
enum ast_bridge_result ast_channel_bridge(struct ast_channel *c0, struct ast_channel *c1,
					  struct ast_bridge_config *config, struct ast_frame **fo, struct ast_channel **rc)
{
	struct ast_channel *who = NULL;
	enum ast_bridge_result res = AST_BRIDGE_COMPLETE;
	int nativefailed=0;
	int firstpass;
	int o0nativeformats;
	int o1nativeformats;
	long time_left_ms=0;
	struct timeval nexteventts = { 0, };
	char caller_warning = 0;
	char callee_warning = 0;

	if (c0->_bridge) {
		ast_log(LOG_WARNING, "%s is already in a bridge with %s\n",
			c0->name, c0->_bridge->name);
		return -1;
	}
	if (c1->_bridge) {
		ast_log(LOG_WARNING, "%s is already in a bridge with %s\n",
			c1->name, c1->_bridge->name);
		return -1;
	}
	
	/* Stop if we're a zombie or need a soft hangup */
	if (ast_test_flag(c0, AST_FLAG_ZOMBIE) || ast_check_hangup_locked(c0) ||
	    ast_test_flag(c1, AST_FLAG_ZOMBIE) || ast_check_hangup_locked(c1))
		return -1;

	*fo = NULL;
	firstpass = config->firstpass;
	config->firstpass = 0;

	if (ast_tvzero(config->start_time))
		config->start_time = ast_tvnow();
	time_left_ms = config->timelimit;

	caller_warning = ast_test_flag(&config->features_caller, AST_FEATURE_PLAY_WARNING);
	callee_warning = ast_test_flag(&config->features_callee, AST_FEATURE_PLAY_WARNING);

	if (config->start_sound && firstpass) {
		if (caller_warning)
			bridge_playfile(c0, c1, config->start_sound, time_left_ms / 1000);
		if (callee_warning)
			bridge_playfile(c1, c0, config->start_sound, time_left_ms / 1000);
	}

	/* Keep track of bridge */
	c0->_bridge = c1;
	c1->_bridge = c0;

	/* \todo  XXX here should check that cid_num is not NULL */
	manager_event(EVENT_FLAG_CALL, "Link",
		      "Channel1: %s\r\n"
		      "Channel2: %s\r\n"
		      "Uniqueid1: %s\r\n"
		      "Uniqueid2: %s\r\n"
		      "CallerID1: %s\r\n"
		      "CallerID2: %s\r\n",
		      c0->name, c1->name, c0->uniqueid, c1->uniqueid, c0->cid.cid_num, c1->cid.cid_num);

	o0nativeformats = c0->nativeformats;
	o1nativeformats = c1->nativeformats;

	if (config->feature_timer) {
		nexteventts = ast_tvadd(config->start_time, ast_samp2tv(config->feature_timer, 1000));
	} else if (config->timelimit) {
		nexteventts = ast_tvadd(config->start_time, ast_samp2tv(config->timelimit, 1000));
		if (caller_warning || callee_warning)
			nexteventts = ast_tvsub(nexteventts, ast_samp2tv(config->play_warning, 1000));
	}

	if (!c0->tech->send_digit_begin)
		ast_set_flag(c1, AST_FLAG_END_DTMF_ONLY);
	if (!c1->tech->send_digit_begin)
		ast_set_flag(c0, AST_FLAG_END_DTMF_ONLY);

	for (/* ever */;;) {
		struct timeval now = { 0, };
		int to;

		to = -1;

		if (!ast_tvzero(nexteventts)) {
			now = ast_tvnow();
			to = ast_tvdiff_ms(nexteventts, now);
			if (to <= 0) {
				if (!config->timelimit) {
					res = AST_BRIDGE_COMPLETE;
					break;
				}
				to = 0;
			}
		}

		if (config->timelimit) {
			time_left_ms = config->timelimit - ast_tvdiff_ms(now, config->start_time);
			if (time_left_ms < to)
				to = time_left_ms;

			if (time_left_ms <= 0) {
				if (caller_warning && config->end_sound)
					bridge_playfile(c0, c1, config->end_sound, 0);
				if (callee_warning && config->end_sound)
					bridge_playfile(c1, c0, config->end_sound, 0);
				*fo = NULL;
				if (who)
					*rc = who;
				res = 0;
				break;
			}
			
			if (!to) {
				if (time_left_ms >= 5000 && config->warning_sound && config->play_warning) {
					int t = (time_left_ms + 500) / 1000; /* round to nearest second */
					if (caller_warning)
						bridge_playfile(c0, c1, config->warning_sound, t);
					if (callee_warning)
						bridge_playfile(c1, c0, config->warning_sound, t);
				}
				if (config->warning_freq && (time_left_ms > (config->warning_freq + 5000)))
					nexteventts = ast_tvadd(nexteventts, ast_samp2tv(config->warning_freq, 1000));
				else
					nexteventts = ast_tvadd(config->start_time, ast_samp2tv(config->timelimit, 1000));
			}
		}

		if (c0->_softhangup == AST_SOFTHANGUP_UNBRIDGE || c1->_softhangup == AST_SOFTHANGUP_UNBRIDGE) {
			if (c0->_softhangup == AST_SOFTHANGUP_UNBRIDGE)
				c0->_softhangup = 0;
			if (c1->_softhangup == AST_SOFTHANGUP_UNBRIDGE)
				c1->_softhangup = 0;
			c0->_bridge = c1;
			c1->_bridge = c0;
			if (option_debug)
				ast_log(LOG_DEBUG, "Unbridge signal received. Ending native bridge.\n");
			continue;
		}
		
		/* Stop if we're a zombie or need a soft hangup */
		if (ast_test_flag(c0, AST_FLAG_ZOMBIE) || ast_check_hangup_locked(c0) ||
		    ast_test_flag(c1, AST_FLAG_ZOMBIE) || ast_check_hangup_locked(c1)) {
			*fo = NULL;
			if (who)
				*rc = who;
			res = 0;
			if (option_debug)
				ast_log(LOG_DEBUG, "Bridge stops because we're zombie or need a soft hangup: c0=%s, c1=%s, flags: %s,%s,%s,%s\n",
					c0->name, c1->name,
					ast_test_flag(c0, AST_FLAG_ZOMBIE) ? "Yes" : "No",
					ast_check_hangup(c0) ? "Yes" : "No",
					ast_test_flag(c1, AST_FLAG_ZOMBIE) ? "Yes" : "No",
					ast_check_hangup(c1) ? "Yes" : "No");
			break;
		}

		if (c0->tech->bridge &&
		    (config->timelimit == 0) &&
		    (c0->tech->bridge == c1->tech->bridge) &&
		    !nativefailed && !c0->monitor && !c1->monitor &&
		    !c0->spies && !c1->spies && !ast_test_flag(&(config->features_callee),AST_FEATURE_REDIRECT) &&
		    !ast_test_flag(&(config->features_caller),AST_FEATURE_REDIRECT) ) {
			/* Looks like they share a bridge method and nothing else is in the way */
			ast_set_flag(c0, AST_FLAG_NBRIDGE);
			ast_set_flag(c1, AST_FLAG_NBRIDGE);
			if ((res = c0->tech->bridge(c0, c1, config->flags, fo, rc, to)) == AST_BRIDGE_COMPLETE) {
				/* \todo  XXX here should check that cid_num is not NULL */
				manager_event(EVENT_FLAG_CALL, "Unlink",
					      "Channel1: %s\r\n"
					      "Channel2: %s\r\n"
					      "Uniqueid1: %s\r\n"
					      "Uniqueid2: %s\r\n"
					      "CallerID1: %s\r\n"
					      "CallerID2: %s\r\n",
					      c0->name, c1->name, c0->uniqueid, c1->uniqueid, c0->cid.cid_num, c1->cid.cid_num);
				if (option_debug)
					ast_log(LOG_DEBUG, "Returning from native bridge, channels: %s, %s\n", c0->name, c1->name);

				ast_clear_flag(c0, AST_FLAG_NBRIDGE);
				ast_clear_flag(c1, AST_FLAG_NBRIDGE);

				if (c0->_softhangup == AST_SOFTHANGUP_UNBRIDGE || c1->_softhangup == AST_SOFTHANGUP_UNBRIDGE)
					continue;

				c0->_bridge = NULL;
				c1->_bridge = NULL;

				return res;
			} else {
				ast_clear_flag(c0, AST_FLAG_NBRIDGE);
				ast_clear_flag(c1, AST_FLAG_NBRIDGE);
			}
			switch (res) {
			case AST_BRIDGE_RETRY:
				continue;
			default:
				if (option_verbose > 2)
					ast_verbose(VERBOSE_PREFIX_3 "Native bridging %s and %s ended\n",
						    c0->name, c1->name);
				/* fallthrough */
			case AST_BRIDGE_FAILED_NOWARN:
				nativefailed++;
				break;
			}
		}
	
		if (((c0->writeformat != c1->readformat) || (c0->readformat != c1->writeformat) ||
		    (c0->nativeformats != o0nativeformats) || (c1->nativeformats != o1nativeformats)) &&
		    !(c0->generator || c1->generator)) {
			if (ast_channel_make_compatible(c0, c1)) {
				ast_log(LOG_WARNING, "Can't make %s and %s compatible\n", c0->name, c1->name);
				/* \todo  XXX here should check that cid_num is not NULL */
                                manager_event(EVENT_FLAG_CALL, "Unlink",
					      "Channel1: %s\r\n"
					      "Channel2: %s\r\n"
					      "Uniqueid1: %s\r\n"
					      "Uniqueid2: %s\r\n"
					      "CallerID1: %s\r\n"
					      "CallerID2: %s\r\n",
					      c0->name, c1->name, c0->uniqueid, c1->uniqueid, c0->cid.cid_num, c1->cid.cid_num);
				return AST_BRIDGE_FAILED;
			}
			o0nativeformats = c0->nativeformats;
			o1nativeformats = c1->nativeformats;
		}
		res = ast_generic_bridge(c0, c1, config, fo, rc, nexteventts);
		if (res != AST_BRIDGE_RETRY)
			break;
	}

	ast_clear_flag(c0, AST_FLAG_END_DTMF_ONLY);
	ast_clear_flag(c1, AST_FLAG_END_DTMF_ONLY);

	c0->_bridge = NULL;
	c1->_bridge = NULL;

	/* \todo  XXX here should check that cid_num is not NULL */
	manager_event(EVENT_FLAG_CALL, "Unlink",
		      "Channel1: %s\r\n"
		      "Channel2: %s\r\n"
		      "Uniqueid1: %s\r\n"
		      "Uniqueid2: %s\r\n"
		      "CallerID1: %s\r\n"
		      "CallerID2: %s\r\n",
		      c0->name, c1->name, c0->uniqueid, c1->uniqueid, c0->cid.cid_num, c1->cid.cid_num);
	if (option_debug)
		ast_log(LOG_DEBUG, "Bridge stops bridging channels %s and %s\n", c0->name, c1->name);

	return res;
}

/*! \brief Sets an option on a channel */
int ast_channel_setoption(struct ast_channel *chan, int option, void *data, int datalen, int block)
{
	int res;

	if (chan->tech->setoption) {
		res = chan->tech->setoption(chan, option, data, datalen);
		if (res < 0)
			return res;
	} else {
		errno = ENOSYS;
		return -1;
	}
	if (block) {
		/* XXX Implement blocking -- just wait for our option frame reply, discarding
		   intermediate packets. XXX */
		ast_log(LOG_ERROR, "XXX Blocking not implemented yet XXX\n");
		return -1;
	}
	return 0;
}

struct tonepair_def {
	int freq1;
	int freq2;
	int duration;
	int vol;
};

struct tonepair_state {
	int fac1;
	int fac2;
	int v1_1;
	int v2_1;
	int v3_1;
	int v1_2;
	int v2_2;
	int v3_2;
	int origwfmt;
	int pos;
	int duration;
	int modulate;
	struct ast_frame f;
	unsigned char offset[AST_FRIENDLY_OFFSET];
	short data[4000];
};

static void tonepair_release(struct ast_channel *chan, void *params)
{
	struct tonepair_state *ts = params;

	if (chan)
		ast_set_write_format(chan, ts->origwfmt);
	free(ts);
}

static void *tonepair_alloc(struct ast_channel *chan, void *params)
{
	struct tonepair_state *ts;
	struct tonepair_def *td = params;

	if (!(ts = ast_calloc(1, sizeof(*ts))))
		return NULL;
	ts->origwfmt = chan->writeformat;
	if (ast_set_write_format(chan, AST_FORMAT_SLINEAR)) {
		ast_log(LOG_WARNING, "Unable to set '%s' to signed linear format (write)\n", chan->name);
		tonepair_release(NULL, ts);
		ts = NULL;
	} else {
		ts->fac1 = 2.0 * cos(2.0 * M_PI * (td->freq1 / 8000.0)) * 32768.0;
		ts->v1_1 = 0;
		ts->v2_1 = sin(-4.0 * M_PI * (td->freq1 / 8000.0)) * td->vol;
		ts->v3_1 = sin(-2.0 * M_PI * (td->freq1 / 8000.0)) * td->vol;
		ts->v2_1 = 0;
		ts->fac2 = 2.0 * cos(2.0 * M_PI * (td->freq2 / 8000.0)) * 32768.0;
		ts->v2_2 = sin(-4.0 * M_PI * (td->freq2 / 8000.0)) * td->vol;
		ts->v3_2 = sin(-2.0 * M_PI * (td->freq2 / 8000.0)) * td->vol;
		ts->duration = td->duration;
		ts->modulate = 0;
	}
	/* Let interrupts interrupt :) */
	ast_set_flag(chan, AST_FLAG_WRITE_INT);
	return ts;
}

static int tonepair_generator(struct ast_channel *chan, void *data, int len, int samples)
{
	struct tonepair_state *ts = data;
	int x;

	/* we need to prepare a frame with 16 * timelen samples as we're
	 * generating SLIN audio
	 */
	len = samples * 2;

	if (len > sizeof(ts->data) / 2 - 1) {
		ast_log(LOG_WARNING, "Can't generate that much data!\n");
		return -1;
	}
	memset(&ts->f, 0, sizeof(ts->f));
 	for (x=0;x<len/2;x++) {
 		ts->v1_1 = ts->v2_1;
 		ts->v2_1 = ts->v3_1;
 		ts->v3_1 = (ts->fac1 * ts->v2_1 >> 15) - ts->v1_1;
 		
 		ts->v1_2 = ts->v2_2;
 		ts->v2_2 = ts->v3_2;
 		ts->v3_2 = (ts->fac2 * ts->v2_2 >> 15) - ts->v1_2;
 		if (ts->modulate) {
 			int p;
 			p = ts->v3_2 - 32768;
 			if (p < 0) p = -p;
 			p = ((p * 9) / 10) + 1;
 			ts->data[x] = (ts->v3_1 * p) >> 15;
 		} else
 			ts->data[x] = ts->v3_1 + ts->v3_2; 
 	}
	ts->f.frametype = AST_FRAME_VOICE;
	ts->f.subclass = AST_FORMAT_SLINEAR;
	ts->f.datalen = len;
	ts->f.samples = samples;
	ts->f.offset = AST_FRIENDLY_OFFSET;
	ts->f.data = ts->data;
	ast_write(chan, &ts->f);
	ts->pos += x;
	if (ts->duration > 0) {
		if (ts->pos >= ts->duration * 8)
			return -1;
	}
	return 0;
}

static struct ast_generator tonepair = {
	alloc: tonepair_alloc,
	release: tonepair_release,
	generate: tonepair_generator,
};

int ast_tonepair_start(struct ast_channel *chan, int freq1, int freq2, int duration, int vol)
{
	struct tonepair_def d = { 0, };

	d.freq1 = freq1;
	d.freq2 = freq2;
	d.duration = duration;
	d.vol = (vol < 1) ? 8192 : vol; /* force invalid to 8192 */
	if (ast_activate_generator(chan, &tonepair, &d))
		return -1;
	return 0;
}

void ast_tonepair_stop(struct ast_channel *chan)
{
	ast_deactivate_generator(chan);
}

int ast_tonepair(struct ast_channel *chan, int freq1, int freq2, int duration, int vol)
{
	int res;

	if ((res = ast_tonepair_start(chan, freq1, freq2, duration, vol)))
		return res;

	/* Give us some wiggle room */
	while (chan->generatordata && ast_waitfor(chan, 100) >= 0) {
		struct ast_frame *f = ast_read(chan);
		if (f)
			ast_frfree(f);
		else
			return -1;
	}
	return 0;
}

ast_group_t ast_get_group(const char *s)
{
	char *piece;
	char *c;
	int start=0, finish=0, x;
	ast_group_t group = 0;

	c = ast_strdupa(s);
	
	while ((piece = strsep(&c, ","))) {
		if (sscanf(piece, "%d-%d", &start, &finish) == 2) {
			/* Range */
		} else if (sscanf(piece, "%d", &start)) {
			/* Just one */
			finish = start;
		} else {
			ast_log(LOG_ERROR, "Syntax error parsing group configuration '%s' at '%s'. Ignoring.\n", s, piece);
			continue;
		}
		for (x = start; x <= finish; x++) {
			if ((x > 63) || (x < 0)) {
				ast_log(LOG_WARNING, "Ignoring invalid group %d (maximum group is 63)\n", x);
			} else
				group |= ((ast_group_t) 1 << x);
		}
	}
	return group;
}

static int (*ast_moh_start_ptr)(struct ast_channel *, const char *, const char *) = NULL;
static void (*ast_moh_stop_ptr)(struct ast_channel *) = NULL;
static void (*ast_moh_cleanup_ptr)(struct ast_channel *) = NULL;

void ast_install_music_functions(int (*start_ptr)(struct ast_channel *, const char *, const char *),
				 void (*stop_ptr)(struct ast_channel *),
				 void (*cleanup_ptr)(struct ast_channel *))
{
	ast_moh_start_ptr = start_ptr;
	ast_moh_stop_ptr = stop_ptr;
	ast_moh_cleanup_ptr = cleanup_ptr;
}

void ast_uninstall_music_functions(void)
{
	ast_moh_start_ptr = NULL;
	ast_moh_stop_ptr = NULL;
	ast_moh_cleanup_ptr = NULL;
}

/*! \brief Turn on music on hold on a given channel */
int ast_moh_start(struct ast_channel *chan, const char *mclass, const char *interpclass)
{
	if (ast_moh_start_ptr)
		return ast_moh_start_ptr(chan, mclass, interpclass);

	if (option_verbose > 2) {
		ast_verbose(VERBOSE_PREFIX_3 "Music class %s requested but no musiconhold loaded.\n", 
			mclass ? mclass : (interpclass ? interpclass : "default"));
	}

	return 0;
}

/*! \brief Turn off music on hold on a given channel */
void ast_moh_stop(struct ast_channel *chan)
{
	if (ast_moh_stop_ptr)
		ast_moh_stop_ptr(chan);
}

void ast_moh_cleanup(struct ast_channel *chan)
{
	if (ast_moh_cleanup_ptr)
		ast_moh_cleanup_ptr(chan);
}

void ast_channels_init(void)
{
	ast_cli_register_multiple(cli_channel, sizeof(cli_channel) / sizeof(struct ast_cli_entry));
}

/*! \brief Print call group and pickup group ---*/
char *ast_print_group(char *buf, int buflen, ast_group_t group)
{
	unsigned int i;
	int first=1;
	char num[3];

	buf[0] = '\0';
	
	if (!group)	/* Return empty string if no group */
		return buf;

	for (i = 0; i <= 63; i++) {	/* Max group is 63 */
		if (group & ((ast_group_t) 1 << i)) {
	   		if (!first) {
				strncat(buf, ", ", buflen);
			} else {
				first=0;
	  		}
			snprintf(num, sizeof(num), "%u", i);
			strncat(buf, num, buflen);
		}
	}
	return buf;
}

void ast_set_variables(struct ast_channel *chan, struct ast_variable *vars)
{
	struct ast_variable *cur;

	for (cur = vars; cur; cur = cur->next)
		pbx_builtin_setvar_helper(chan, cur->name, cur->value);	
}

static void copy_data_from_queue(struct ast_channel_spy_queue *queue, short *buf, unsigned int samples)
{
	struct ast_frame *f;
	int tocopy;
	int bytestocopy;

	while (samples) {
		if (!(f = AST_LIST_FIRST(&queue->list))) {
			ast_log(LOG_ERROR, "Ran out of frames before buffer filled!\n");
			break;
		}

		tocopy = (f->samples > samples) ? samples : f->samples;
		bytestocopy = ast_codec_get_len(queue->format, tocopy);
		memcpy(buf, f->data, bytestocopy);
		samples -= tocopy;
		buf += tocopy;
		f->samples -= tocopy;
		f->data += bytestocopy;
		f->datalen -= bytestocopy;
		f->offset += bytestocopy;
		queue->samples -= tocopy;

		if (!f->samples)
			ast_frfree(AST_LIST_REMOVE_HEAD(&queue->list, frame_list));
	}
}

struct ast_frame *ast_channel_spy_read_frame(struct ast_channel_spy *spy, unsigned int samples)
{
	struct ast_frame *result;
	/* buffers are allocated to hold SLINEAR, which is the largest format */
        short read_buf[samples];
        short write_buf[samples];
	struct ast_frame *read_frame;
	struct ast_frame *write_frame;
	int need_dup;
	struct ast_frame stack_read_frame = { .frametype = AST_FRAME_VOICE,
					      .subclass = spy->read_queue.format,
					      .data = read_buf,
					      .samples = samples,
					      .datalen = ast_codec_get_len(spy->read_queue.format, samples),
	};
	struct ast_frame stack_write_frame = { .frametype = AST_FRAME_VOICE,
					       .subclass = spy->write_queue.format,
					       .data = write_buf,
					       .samples = samples,
					       .datalen = ast_codec_get_len(spy->write_queue.format, samples),
	};

	/* if a flush has been requested, dump everything in whichever queue is larger */
	if (ast_test_flag(spy, CHANSPY_TRIGGER_FLUSH)) {
		if (spy->read_queue.samples > spy->write_queue.samples) {
			if (ast_test_flag(spy, CHANSPY_READ_VOLADJUST)) {
				AST_LIST_TRAVERSE(&spy->read_queue.list, result, frame_list)
					ast_frame_adjust_volume(result, spy->read_vol_adjustment);
			}
			result = AST_LIST_FIRST(&spy->read_queue.list);
			AST_LIST_HEAD_SET_NOLOCK(&spy->read_queue.list, NULL);
			spy->read_queue.samples = 0;
		} else {
			if (ast_test_flag(spy, CHANSPY_WRITE_VOLADJUST)) {
				AST_LIST_TRAVERSE(&spy->write_queue.list, result, frame_list)
					ast_frame_adjust_volume(result, spy->write_vol_adjustment);
			}
			result = AST_LIST_FIRST(&spy->write_queue.list);
			AST_LIST_HEAD_SET_NOLOCK(&spy->write_queue.list, NULL);
			spy->write_queue.samples = 0;
		}
		ast_clear_flag(spy, CHANSPY_TRIGGER_FLUSH);
		return result;
	}

	if ((spy->read_queue.samples < samples) || (spy->write_queue.samples < samples))
		return NULL;

	/* short-circuit if both head frames have exactly what we want */
	if ((AST_LIST_FIRST(&spy->read_queue.list)->samples == samples) &&
	    (AST_LIST_FIRST(&spy->write_queue.list)->samples == samples)) {
		read_frame = AST_LIST_REMOVE_HEAD(&spy->read_queue.list, frame_list);
		write_frame = AST_LIST_REMOVE_HEAD(&spy->write_queue.list, frame_list);

		spy->read_queue.samples -= samples;
		spy->write_queue.samples -= samples;

		need_dup = 0;
	} else {
		copy_data_from_queue(&spy->read_queue, read_buf, samples);
		copy_data_from_queue(&spy->write_queue, write_buf, samples);

		read_frame = &stack_read_frame;
		write_frame = &stack_write_frame;
		need_dup = 1;
	}
	
	if (ast_test_flag(spy, CHANSPY_READ_VOLADJUST))
		ast_frame_adjust_volume(read_frame, spy->read_vol_adjustment);

	if (ast_test_flag(spy, CHANSPY_WRITE_VOLADJUST))
		ast_frame_adjust_volume(write_frame, spy->write_vol_adjustment);

	if (ast_test_flag(spy, CHANSPY_MIXAUDIO)) {
		ast_frame_slinear_sum(read_frame, write_frame);

		if (need_dup)
			result = ast_frdup(read_frame);
		else {
			result = read_frame;
			ast_frfree(write_frame);
		}
	} else {
		if (need_dup) {
			result = ast_frdup(read_frame);
			AST_LIST_NEXT(result, frame_list) = ast_frdup(write_frame);
		} else {
			result = read_frame;
			AST_LIST_NEXT(result, frame_list) = write_frame;
		}
	}

	return result;
}

static void *silence_generator_alloc(struct ast_channel *chan, void *data)
{
	/* just store the data pointer in the channel structure */
	return data;
}

static void silence_generator_release(struct ast_channel *chan, void *data)
{
	/* nothing to do */
}

static int silence_generator_generate(struct ast_channel *chan, void *data, int len, int samples)
{
	short buf[samples];
	struct ast_frame frame = {
		.frametype = AST_FRAME_VOICE,
		.subclass = AST_FORMAT_SLINEAR,
		.data = buf,
		.samples = samples,
		.datalen = sizeof(buf),
	};
	memset(buf, 0, sizeof(buf));
	if (ast_write(chan, &frame))
		return -1;
	return 0;
}

static struct ast_generator silence_generator = {
	.alloc = silence_generator_alloc,
	.release = silence_generator_release,
	.generate = silence_generator_generate,
};

struct ast_silence_generator {
	int old_write_format;
};

struct ast_silence_generator *ast_channel_start_silence_generator(struct ast_channel *chan)
{
	struct ast_silence_generator *state;

	if (!(state = ast_calloc(1, sizeof(*state)))) {
		return NULL;
	}

	state->old_write_format = chan->writeformat;

	if (ast_set_write_format(chan, AST_FORMAT_SLINEAR) < 0) {
		ast_log(LOG_ERROR, "Could not set write format to SLINEAR\n");
		free(state);
		return NULL;
	}

	ast_activate_generator(chan, &silence_generator, state);

	if (option_debug)
		ast_log(LOG_DEBUG, "Started silence generator on '%s'\n", chan->name);

	return state;
}

void ast_channel_stop_silence_generator(struct ast_channel *chan, struct ast_silence_generator *state)
{
	if (!state)
		return;

	ast_deactivate_generator(chan);

	if (option_debug)
		ast_log(LOG_DEBUG, "Stopped silence generator on '%s'\n", chan->name);

	if (ast_set_write_format(chan, state->old_write_format) < 0)
		ast_log(LOG_ERROR, "Could not return write format to its original state\n");

	free(state);
}


/*! \ brief Convert channel reloadreason (ENUM) to text string for manager event */
const char *channelreloadreason2txt(enum channelreloadreason reason)
{
	switch (reason) {
	case CHANNEL_MODULE_LOAD:
		return "LOAD (Channel module load)";

	case CHANNEL_MODULE_RELOAD:
		return "RELOAD (Channel module reload)";

	case CHANNEL_CLI_RELOAD:
		return "CLIRELOAD (Channel module reload by CLI command)";

	default:
		return "MANAGERRELOAD (Channel module reload by manager)";
	}
};

#ifdef DEBUG_CHANNEL_LOCKS

/*! \brief Unlock AST channel (and print debugging output) 
\note You need to enable DEBUG_CHANNEL_LOCKS for this function
*/
int ast_channel_unlock(struct ast_channel *chan)
{
	int res = 0;
	if (option_debug > 2) 
		ast_log(LOG_DEBUG, "::::==== Unlocking AST channel %s\n", chan->name);
	
	if (!chan) {
		if (option_debug)
			ast_log(LOG_DEBUG, "::::==== Unlocking non-existing channel \n");
		return 0;
	}

	res = ast_mutex_unlock(&chan->lock);

	if (option_debug > 2) {
#ifdef DEBUG_THREADS
		int count = 0;
		if ((count = chan->lock.reentrancy))
			ast_log(LOG_DEBUG, ":::=== Still have %d locks (recursive)\n", count);
#endif
		if (!res)
			if (option_debug)
				ast_log(LOG_DEBUG, "::::==== Channel %s was unlocked\n", chan->name);
			if (res == EINVAL) {
				if (option_debug)
					ast_log(LOG_DEBUG, "::::==== Channel %s had no lock by this thread. Failed unlocking\n", chan->name);
			}
		}
		if (res == EPERM) {
			/* We had no lock, so okay any way*/
			if (option_debug > 3)
				ast_log(LOG_DEBUG, "::::==== Channel %s was not locked at all \n", chan->name);
		res = 0;
	}
	return res;
}

/*! \brief Lock AST channel (and print debugging output)
\note You need to enable DEBUG_CHANNEL_LOCKS for this function */
int ast_channel_lock(struct ast_channel *chan)
{
	int res;

	if (option_debug > 3)
		ast_log(LOG_DEBUG, "====:::: Locking AST channel %s\n", chan->name);

	res = ast_mutex_lock(&chan->lock);

	if (option_debug > 3) {
#ifdef DEBUG_THREADS
		int count = 0;
		if ((count = chan->lock.reentrancy))
			ast_log(LOG_DEBUG, ":::=== Now have %d locks (recursive)\n", count);
#endif
		if (!res)
			ast_log(LOG_DEBUG, "::::==== Channel %s was locked\n", chan->name);
		if (res == EDEADLK) {
		/* We had no lock, so okey any way */
		if (option_debug > 3)
			ast_log(LOG_DEBUG, "::::==== Channel %s was not locked by us. Lock would cause deadlock.\n", chan->name);
		}
		if (res == EINVAL) {
			if (option_debug > 3)
				ast_log(LOG_DEBUG, "::::==== Channel %s lock failed. No mutex.\n", chan->name);
		}
	}
	return res;
}

/*! \brief Lock AST channel (and print debugging output)
\note	You need to enable DEBUG_CHANNEL_LOCKS for this function */
int ast_channel_trylock(struct ast_channel *chan)
{
	int res;

	if (option_debug > 2)
		ast_log(LOG_DEBUG, "====:::: Trying to lock AST channel %s\n", chan->name);

	res = ast_mutex_trylock(&chan->lock);

	if (option_debug > 2) {
#ifdef DEBUG_THREADS
		int count = 0;
		if ((count = chan->lock.reentrancy))
			ast_log(LOG_DEBUG, ":::=== Now have %d locks (recursive)\n", count);
#endif
		if (!res)
			ast_log(LOG_DEBUG, "::::==== Channel %s was locked\n", chan->name);
		if (res == EBUSY) {
			/* We failed to lock */
			if (option_debug > 2)
				ast_log(LOG_DEBUG, "::::==== Channel %s failed to lock. Not waiting around...\n", chan->name);
		}
		if (res == EDEADLK) {
			/* We had no lock, so okey any way*/
			if (option_debug > 2)
				ast_log(LOG_DEBUG, "::::==== Channel %s was not locked. Lock would cause deadlock.\n", chan->name);
		}
		if (res == EINVAL && option_debug > 2)
			ast_log(LOG_DEBUG, "::::==== Channel %s lock failed. No mutex.\n", chan->name);
	}
	return res;
}

#endif

/*
 * Wrappers for various ast_say_*() functions that call the full version
 * of the same functions.
 * The proper place would be say.c, but that file is optional and one
 * must be able to build asterisk even without it (using a loadable 'say'
 * implementation that only supplies the 'full' version of the functions.
 */

int ast_say_number(struct ast_channel *chan, int num,
	const char *ints, const char *language, const char *options)
{
        return ast_say_number_full(chan, num, ints, language, options, -1, -1);
}

int ast_say_enumeration(struct ast_channel *chan, int num,
	const char *ints, const char *language, const char *options)
{
        return ast_say_enumeration_full(chan, num, ints, language, options, -1, -1);
}

int ast_say_digits(struct ast_channel *chan, int num,
	const char *ints, const char *lang)
{
        return ast_say_digits_full(chan, num, ints, lang, -1, -1);
}

int ast_say_digit_str(struct ast_channel *chan, const char *str,
	const char *ints, const char *lang)
{
        return ast_say_digit_str_full(chan, str, ints, lang, -1, -1);
}

int ast_say_character_str(struct ast_channel *chan, const char *str,
	const char *ints, const char *lang)
{
        return ast_say_character_str_full(chan, str, ints, lang, -1, -1);
}

int ast_say_phonetic_str(struct ast_channel *chan, const char *str,
	const char *ints, const char *lang)
{
        return ast_say_phonetic_str_full(chan, str, ints, lang, -1, -1);
}

int ast_say_digits_full(struct ast_channel *chan, int num,
	const char *ints, const char *lang, int audiofd, int ctrlfd)
{
        char buf[256];

        snprintf(buf, sizeof(buf), "%d", num);
        return ast_say_digit_str_full(chan, buf, ints, lang, audiofd, ctrlfd);
}

int ast_channel_whisper_start(struct ast_channel *chan)
{
	if (chan->whisper)
		return -1;

	if (!(chan->whisper = ast_calloc(1, sizeof(*chan->whisper))))
		return -1;

	ast_mutex_init(&chan->whisper->lock);
	ast_slinfactory_init(&chan->whisper->sf);
	ast_set_flag(chan, AST_FLAG_WHISPER);

	return 0;
}

int ast_channel_whisper_feed(struct ast_channel *chan, struct ast_frame *f)
{
	if (!chan->whisper)
		return -1;

	ast_mutex_lock(&chan->whisper->lock);
	ast_slinfactory_feed(&chan->whisper->sf, f);
	ast_mutex_unlock(&chan->whisper->lock);

	return 0;
}

void ast_channel_whisper_stop(struct ast_channel *chan)
{
	if (!chan->whisper)
		return;

	ast_clear_flag(chan, AST_FLAG_WHISPER);
	if (chan->whisper->path)
		ast_translator_free_path(chan->whisper->path);
	if (chan->whisper->original_format && chan->writeformat == AST_FORMAT_SLINEAR)
		ast_set_write_format(chan, chan->whisper->original_format);
	ast_slinfactory_destroy(&chan->whisper->sf);
	ast_mutex_destroy(&chan->whisper->lock);
	free(chan->whisper);
	chan->whisper = NULL;
}
