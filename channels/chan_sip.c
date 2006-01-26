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

/*!
 * \file
 * \brief Implementation of Session Initiation Protocol
 *
 * \author Mark Spencer <markster@digium.com>
 *
 * See Also:
 * \arg \ref AstCREDITS
 *
 * Implementation of RFC 3261 - without S/MIME, TCP and TLS support
 * Configuration file \link Config_sip sip.conf \endlink
 *
 *
 * \todo SIP over TCP
 * \todo SIP over TLS
 * \todo Better support of forking
 *
 * \ingroup channel_drivers
 */


#include <stdio.h>
#include <ctype.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <errno.h>
#include <stdlib.h>
#include <fcntl.h>
#include <netdb.h>
#include <signal.h>
#include <sys/signal.h>
#include <netinet/in.h>
#include <netinet/in_systm.h>
#include <arpa/inet.h>
#include <netinet/ip.h>
#include <regex.h>

#include "asterisk.h"

ASTERISK_FILE_VERSION(__FILE__, "$Revision$")

#include "asterisk/lock.h"
#include "asterisk/channel.h"
#include "asterisk/config.h"
#include "asterisk/logger.h"
#include "asterisk/module.h"
#include "asterisk/pbx.h"
#include "asterisk/options.h"
#include "asterisk/lock.h"
#include "asterisk/sched.h"
#include "asterisk/io.h"
#include "asterisk/rtp.h"
#include "asterisk/acl.h"
#include "asterisk/manager.h"
#include "asterisk/callerid.h"
#include "asterisk/cli.h"
#include "asterisk/app.h"
#include "asterisk/musiconhold.h"
#include "asterisk/dsp.h"
#include "asterisk/features.h"
#include "asterisk/acl.h"
#include "asterisk/srv.h"
#include "asterisk/astdb.h"
#include "asterisk/causes.h"
#include "asterisk/utils.h"
#include "asterisk/file.h"
#include "asterisk/astobj.h"
#include "asterisk/dnsmgr.h"
#include "asterisk/devicestate.h"
#include "asterisk/linkedlists.h"
#include "asterisk/stringfields.h"

#ifdef OSP_SUPPORT
#include "asterisk/astosp.h"
#endif

#ifndef FALSE
#define FALSE	0
#endif

#ifndef TRUE
#define TRUE 1
#endif
 
#define VIDEO_CODEC_MASK	0x1fc0000 /*!< Video codecs from H.261 thru AST_FORMAT_MAX_VIDEO */
#ifndef IPTOS_MINCOST
#define IPTOS_MINCOST		0x02
#endif

/* #define VOCAL_DATA_HACK */

#define DEFAULT_DEFAULT_EXPIRY  120
#define DEFAULT_MIN_EXPIRY	60
#define DEFAULT_MAX_EXPIRY	3600
#define DEFAULT_REGISTRATION_TIMEOUT	20
#define DEFAULT_MAX_FORWARDS	"70"

/* guard limit must be larger than guard secs */
/* guard min must be < 1000, and should be >= 250 */
#define EXPIRY_GUARD_SECS	15		/*!< How long before expiry do we reregister */
#define EXPIRY_GUARD_LIMIT	30		/*!< Below here, we use EXPIRY_GUARD_PCT instead of 
					 	  EXPIRY_GUARD_SECS */
#define EXPIRY_GUARD_MIN	500		/*!< This is the minimum guard time applied. If 
					 	  GUARD_PCT turns out to be lower than this, it 
						   will use this time instead.
						   This is in milliseconds. */
#define EXPIRY_GUARD_PCT	0.20		/*!< Percentage of expires timeout to use when 
						   below EXPIRY_GUARD_LIMIT */
#define DEFAULT_EXPIRY 900			/*!< Expire slowly */

static int min_expiry = DEFAULT_MIN_EXPIRY;	/*!< Minimum accepted registration time */
static int max_expiry = DEFAULT_MAX_EXPIRY;	/*!< Maximum accepted registration time */
static int default_expiry = DEFAULT_DEFAULT_EXPIRY;
static int expiry = DEFAULT_EXPIRY;

#ifndef MAX
#define MAX(a,b) ((a) > (b) ? (a) : (b))
#endif

#define CALLERID_UNKNOWN	"Unknown"



#define DEFAULT_MAXMS		2000		/*!< Qualification: Must be faster than 2 seconds by default */
#define DEFAULT_FREQ_OK		60 * 1000	/*!< Qualification: How often to check for the host to be up */
#define DEFAULT_FREQ_NOTOK	10 * 1000	/*!< Qualification: How often to check, if the host is down... */

#define DEFAULT_RETRANS		1000		/*!< How frequently to retransmit Default: 2 * 500 ms in RFC 3261 */
#define MAX_RETRANS		6		/*!< Try only 6 times for retransmissions, a total of 7 transmissions */
#define MAX_AUTHTRIES		3		/*!< Try authentication three times, then fail */

#define SIP_MAX_HEADERS		64			/*!< Max amount of SIP headers to read */
#define SIP_MAX_LINES 		64			/*!< Max amount of lines in SIP attachment (like SDP) */


static const char desc[] = "Session Initiation Protocol (SIP)";
static const char channeltype[] = "SIP";
static const char config[] = "sip.conf";
static const char notify_config[] = "sip_notify.conf";

#define RTP 	1
#define NO_RTP	0

/* Do _NOT_ make any changes to this enum, or the array following it;
   if you think you are doing the right thing, you are probably
   not doing the right thing. If you think there are changes
   needed, get someone else to review them first _before_
   submitting a patch. If these two lists do not match properly
   bad things will happen.
*/

enum subscriptiontype { 
	NONE = 0,
	TIMEOUT,
	XPIDF_XML,
	DIALOG_INFO_XML,
	CPIM_PIDF_XML,
	PIDF_XML
};

static const struct cfsubscription_types {
	enum subscriptiontype type;
	const char * const event;
	const char * const mediatype;
	const char * const text;
} subscription_types[] = {
	{ NONE,            "-",        "unknown",	                  "unknown" },
 	/* IETF draft: draft-ietf-sipping-dialog-package-05.txt */
	{ DIALOG_INFO_XML, "dialog",   "application/dialog-info+xml", "dialog-info+xml" },
	{ CPIM_PIDF_XML,   "presence", "application/cpim-pidf+xml",   "cpim-pidf+xml" },  /* RFC 3863 */
	{ PIDF_XML,        "presence", "application/pidf+xml",        "pidf+xml" },       /* RFC 3863 */
	{ XPIDF_XML,       "presence", "application/xpidf+xml",       "xpidf+xml" }       /* Pre-RFC 3863 with MS additions */
};

enum sipmethod {
	SIP_UNKNOWN,
	SIP_RESPONSE,
	SIP_REGISTER,
	SIP_OPTIONS,
	SIP_NOTIFY,
	SIP_INVITE,
	SIP_ACK,
	SIP_PRACK,
	SIP_BYE,
	SIP_REFER,
	SIP_SUBSCRIBE,
	SIP_MESSAGE,
	SIP_UPDATE,
	SIP_INFO,
	SIP_CANCEL,
	SIP_PUBLISH,
} sip_method_list;

enum sip_auth_type {
	PROXY_AUTH,
	WWW_AUTH,
};

/*! XXX Note that sip_methods[i].id == i must hold or the code breaks */
static const struct  cfsip_methods { 
	enum sipmethod id;
	int need_rtp;		/*!< when this is the 'primary' use for a pvt structure, does it need RTP? */
	char * const text;
} sip_methods[] = {
	{ SIP_UNKNOWN,	 RTP,    "-UNKNOWN-" },
	{ SIP_RESPONSE,	 NO_RTP, "SIP/2.0" },
	{ SIP_REGISTER,	 NO_RTP, "REGISTER" },
 	{ SIP_OPTIONS,	 NO_RTP, "OPTIONS" },
	{ SIP_NOTIFY,	 NO_RTP, "NOTIFY" },
	{ SIP_INVITE,	 RTP,    "INVITE" },
	{ SIP_ACK,	 NO_RTP, "ACK" },
	{ SIP_PRACK,	 NO_RTP, "PRACK" },
	{ SIP_BYE,	 NO_RTP, "BYE" },
	{ SIP_REFER,	 NO_RTP, "REFER" },
	{ SIP_SUBSCRIBE, NO_RTP, "SUBSCRIBE" },
	{ SIP_MESSAGE,	 NO_RTP, "MESSAGE" },
	{ SIP_UPDATE,	 NO_RTP, "UPDATE" },
	{ SIP_INFO,	 NO_RTP, "INFO" },
	{ SIP_CANCEL,	 NO_RTP, "CANCEL" },
	{ SIP_PUBLISH,	 NO_RTP, "PUBLISH" }
};

/*! \brief Structure for conversion between compressed SIP and "normal" SIP */
static const struct cfalias {
	char * const fullname;
	char * const shortname;
} aliases[] = {
	{ "Content-Type", "c" },
	{ "Content-Encoding", "e" },
	{ "From", "f" },
	{ "Call-ID", "i" },
	{ "Contact", "m" },
	{ "Content-Length", "l" },
	{ "Subject", "s" },
	{ "To", "t" },
	{ "Supported", "k" },
	{ "Refer-To", "r" },
	{ "Referred-By", "b" },
	{ "Allow-Events", "u" },
	{ "Event", "o" },
	{ "Via", "v" },
	{ "Accept-Contact",      "a" },
	{ "Reject-Contact",      "j" },
	{ "Request-Disposition", "d" },
	{ "Session-Expires",     "x" },
};

/*!  Define SIP option tags, used in Require: and Supported: headers 
 	We need to be aware of these properties in the phones to use 
	the replace: header. We should not do that without knowing
	that the other end supports it... 
	This is nothing we can configure, we learn by the dialog
	Supported: header on the REGISTER (peer) or the INVITE
	(other devices)
	We are not using many of these today, but will in the future.
	This is documented in RFC 3261
*/
#define SUPPORTED		1
#define NOT_SUPPORTED		0

#define SIP_OPT_REPLACES	(1 << 0)
#define SIP_OPT_100REL		(1 << 1)
#define SIP_OPT_TIMER		(1 << 2)
#define SIP_OPT_EARLY_SESSION	(1 << 3)
#define SIP_OPT_JOIN		(1 << 4)
#define SIP_OPT_PATH		(1 << 5)
#define SIP_OPT_PREF		(1 << 6)
#define SIP_OPT_PRECONDITION	(1 << 7)
#define SIP_OPT_PRIVACY		(1 << 8)
#define SIP_OPT_SDP_ANAT	(1 << 9)
#define SIP_OPT_SEC_AGREE	(1 << 10)
#define SIP_OPT_EVENTLIST	(1 << 11)
#define SIP_OPT_GRUU		(1 << 12)
#define SIP_OPT_TARGET_DIALOG	(1 << 13)

/*! \brief List of well-known SIP options. If we get this in a require,
   we should check the list and answer accordingly. */
static const struct cfsip_options {
	int id;			/*!< Bitmap ID */
	int supported;		/*!< Supported by Asterisk ? */
	char * const text;	/*!< Text id, as in standard */
} sip_options[] = {
	/* Replaces: header for transfer */
	{ SIP_OPT_REPLACES,	SUPPORTED,	"replaces" },	
	/* RFC3262: PRACK 100% reliability */
	{ SIP_OPT_100REL,	NOT_SUPPORTED,	"100rel" },	
	/* SIP Session Timers */
	{ SIP_OPT_TIMER,	NOT_SUPPORTED,	"timer" },
	/* RFC3959: SIP Early session support */
	{ SIP_OPT_EARLY_SESSION, NOT_SUPPORTED,	"early-session" },
	/* SIP Join header support */
	{ SIP_OPT_JOIN,		NOT_SUPPORTED,	"join" },
	/* RFC3327: Path support */
	{ SIP_OPT_PATH,		NOT_SUPPORTED,	"path" },
	/* RFC3840: Callee preferences */
	{ SIP_OPT_PREF,		NOT_SUPPORTED,	"pref" },
	/* RFC3312: Precondition support */
	{ SIP_OPT_PRECONDITION,	NOT_SUPPORTED,	"precondition" },
	/* RFC3323: Privacy with proxies*/
	{ SIP_OPT_PRIVACY,	NOT_SUPPORTED,	"privacy" },
	/* RFC4092: Usage of the SDP ANAT Semantics in the SIP */
	{ SIP_OPT_SDP_ANAT,	NOT_SUPPORTED,	"sdp-anat" },
	/* RFC3329: Security agreement mechanism */
	{ SIP_OPT_SEC_AGREE,	NOT_SUPPORTED,	"sec_agree" },
	/* SIMPLE events:  draft-ietf-simple-event-list-07.txt */
	{ SIP_OPT_EVENTLIST,	NOT_SUPPORTED,	"eventlist" },
	/* GRUU: Globally Routable User Agent URI's */
	{ SIP_OPT_GRUU,		NOT_SUPPORTED,	"gruu" },
	/* Target-dialog: draft-ietf-sip-target-dialog-00.txt */
	{ SIP_OPT_TARGET_DIALOG,NOT_SUPPORTED,	"target-dialog" },
};


/*! \brief SIP Methods we support */
#define ALLOWED_METHODS "INVITE, ACK, CANCEL, OPTIONS, BYE, REFER, SUBSCRIBE, NOTIFY"

/*! \brief SIP Extensions we support */
#define SUPPORTED_EXTENSIONS "replaces" 

#define SIP_MAX_PACKET		4096	/*!< Also from RFC 3261 (2543), should sub headers tho */

/* Default values, set and reset in reload_config before reading configuration */
/* These are default values in the source. There are other recommended values in the
   sip.conf.sample for new installations. These may differ to keep backwards compatibility,
   yet encouraging new behaviour on new installations 
 */
#define DEFAULT_SIP_PORT	5060	/*!< From RFC 3261 (former 2543) */
#define DEFAULT_CONTEXT		"default"
#define DEFAULT_MUSICCLASS	"default"
#define DEFAULT_VMEXTEN 	"asterisk"
#define DEFAULT_CALLERID 	"asterisk"
#define DEFAULT_NOTIFYMIME 	"application/simple-message-summary"
#define DEFAULT_MWITIME 	10
#define DEFAULT_ALLOWGUEST	TRUE
#define DEFAULT_VIDEOSUPPORT	FALSE
#define DEFAULT_SRVLOOKUP	FALSE		/*!< Recommended setting is ON */
#define DEFAULT_COMPACTHEADERS	FALSE
#define DEFAULT_TOS		FALSE
#define DEFAULT_ALLOW_EXT_DOM	TRUE
#define DEFAULT_REALM		"asterisk"
#define DEFAULT_NOTIFYRINGING	TRUE
#define DEFAULT_PEDANTIC	FALSE
#define DEFAULT_AUTOCREATEPEER	FALSE
#define DEFAULT_QUALIFY		FALSE
#ifndef DEFAULT_USERAGENT
#define DEFAULT_USERAGENT "Asterisk PBX"	/*!< Default Useragent: header unless re-defined in sip.conf */
#endif

/* Default setttings are used as a channel setting and as a default when
   configuring devices */
static char default_context[AST_MAX_CONTEXT];
static char default_subscribecontext[AST_MAX_CONTEXT];
static char default_language[MAX_LANGUAGE];
static char default_callerid[AST_MAX_EXTENSION];
static char default_fromdomain[AST_MAX_EXTENSION];
static char default_notifymime[AST_MAX_EXTENSION];
static int default_qualify;		/*!< Default Qualify= setting */
static char default_vmexten[AST_MAX_EXTENSION];
static char default_musicclass[MAX_MUSICCLASS];		/*!< Global music on hold class */

/* Global settings only apply to the channel */
static int global_notifyringing;	/*!< Send notifications on ringing */
static int srvlookup;			/*!< SRV Lookup on or off. Default is off, RFC behavior is on */
static int pedanticsipchecking;		/*!< Extra checking ?  Default off */
static int autocreatepeer;		/*!< Auto creation of peers at registration? Default off. */
static int relaxdtmf;			/*!< Relax DTMF */
static int global_rtptimeout;		/*!< Time out call if no RTP */
static int global_rtpholdtimeout;
static int global_rtpkeepalive;		/*!< Send RTP keepalives */
static int global_reg_timeout;	
static int global_regattempts_max;	/*!< Registration attempts before giving up */
static int global_allowguest;		/*!< allow unauthenticated users/peers to connect? */
static int global_mwitime;		/*!< Time between MWI checks for peers */
static int global_tos;			/*!< IP Type of service */
static int global_videosupport;		/*!< Videosupport on or off */
static int compactheaders;		/*!< send compact sip headers */
static int recordhistory;		/*!< Record SIP history. Off by default */
static int dumphistory;			/*!< Dump history to verbose before destroying SIP dialog */
static char global_realm[MAXHOSTNAMELEN]; 		/*!< Default realm */
static char regcontext[AST_MAX_CONTEXT];		/*!< Context for auto-extensions */
static char global_useragent[AST_MAX_EXTENSION];	/*!< Useragent for the SIP channel */
static int allow_external_domains;	/*!< Accept calls to external SIP domains? */

/*! \brief Codecs that we support by default: */
static int global_capability = AST_FORMAT_ULAW | AST_FORMAT_ALAW | AST_FORMAT_GSM | AST_FORMAT_H263;
static int noncodeccapability = AST_RTP_DTMF;

/* Object counters */
static int suserobjs = 0;		/*!< Static users */
static int ruserobjs = 0;		/*!< Realtime users */
static int speerobjs = 0;		/*!< Statis peers */
static int rpeerobjs = 0;		/*!< Realtime peers */
static int apeerobjs = 0;		/*!< Autocreated peer objects */
static int regobjs = 0;			/*!< Registry objects */

static struct ast_flags global_flags = {0};		/*!< global SIP_ flags */
static struct ast_flags global_flags_page2 = {0};	/*!< more global SIP_ flags */

static int usecnt =0;

AST_MUTEX_DEFINE_STATIC(usecnt_lock);

AST_MUTEX_DEFINE_STATIC(rand_lock);			/*!< Lock for thread-safe random generator */

/*! \brief Protect the SIP dialog list (of sip_pvt's) */
AST_MUTEX_DEFINE_STATIC(iflock);

/*! \brief Protect the monitoring thread, so only one process can kill or start it, and not
   when it's doing something critical. */
AST_MUTEX_DEFINE_STATIC(netlock);

AST_MUTEX_DEFINE_STATIC(monlock);

/*! \brief This is the thread for the monitor which checks for input on the channels
   which are not currently in use.  */
static pthread_t monitor_thread = AST_PTHREADT_NULL;

static int restart_monitor(void);


static struct in_addr __ourip;
static struct sockaddr_in outboundproxyip;
static int ourport;
static struct sockaddr_in debugaddr;


static struct sched_context *sched;
static struct io_context *io;


#define DEC_CALL_LIMIT	0
#define INC_CALL_LIMIT	1

static struct ast_codec_pref prefs;


/*! \brief sip_request: The data grabbed from the UDP socket */
struct sip_request {
	char *rlPart1; 		/*!< SIP Method Name or "SIP/2.0" protocol version */
	char *rlPart2; 		/*!< The Request URI or Response Status */
	int len;		/*!< Length */
	int headers;		/*!< # of SIP Headers */
	int method;		/*!< Method of this request */
	char *header[SIP_MAX_HEADERS];
	int lines;		/*!< SDP Content */
	char *line[SIP_MAX_LINES];
	char data[SIP_MAX_PACKET];
	int debug;		/*!< Debug flag for this packet */
	unsigned int flags;	/*!< SIP_PKT Flags for this packet */
};

struct sip_pkt;

/*! \brief Parameters to the transmit_invite function */
struct sip_invite_param {
	const char *distinctive_ring;	/*!< Distinctive ring header */
	const char *osptoken;		/*!< OSP token for this call */
	int addsipheaders;	/*!< Add extra SIP headers */
	const char *uri_options;	/*!< URI options to add to the URI */
	const char *vxml_url;		/*!< VXML url for Cisco phones */
	char *auth;		/*!< Authentication */
	char *authheader;	/*!< Auth header */
	enum sip_auth_type auth_type;	/*!< Authentication type */
};

struct sip_route {
	struct sip_route *next;
	char hop[0];
};

enum domain_mode {
	SIP_DOMAIN_AUTO,	/*!< This domain is auto-configured */
	SIP_DOMAIN_CONFIG,	/*!< This domain is from configuration */
};

struct domain {
	char domain[MAXHOSTNAMELEN];		/*!< SIP domain we are responsible for */
	char context[AST_MAX_EXTENSION];	/*!< Incoming context for this domain */
	enum domain_mode mode;			/*!< How did we find this domain? */
	AST_LIST_ENTRY(domain) list;		/*!< List mechanics */
};

static AST_LIST_HEAD_STATIC(domain_list, domain);	/*!< The SIP domain list */


/*! \brief sip_history: Structure for saving transactions within a SIP dialog */
struct sip_history {
	AST_LIST_ENTRY(sip_history) list;
	char event[0];	/* actually more, depending on needs */
};

AST_LIST_HEAD_NOLOCK(sip_history_head, sip_history); /*!< history list, entry in sip_pvt */

/*! \brief sip_auth: Creadentials for authentication to other SIP services */
struct sip_auth {
	char realm[AST_MAX_EXTENSION];  /*!< Realm in which these credentials are valid */
	char username[256];             /*!< Username */
	char secret[256];               /*!< Secret */
	char md5secret[256];            /*!< MD5Secret */
	struct sip_auth *next;          /*!< Next auth structure in list */
};

#define SIP_ALREADYGONE		(1 << 0)	/*!< Whether or not we've already been destroyed by our peer */
#define SIP_NEEDDESTROY		(1 << 1)	/*!< if we need to be destroyed */
#define SIP_NOVIDEO		(1 << 2)	/*!< Didn't get video in invite, don't offer */
#define SIP_RINGING		(1 << 3)	/*!< Have sent 180 ringing */
#define SIP_PROGRESS_SENT	(1 << 4)	/*!< Have sent 183 message progress */
#define SIP_NEEDREINVITE	(1 << 5)	/*!< Do we need to send another reinvite? */
#define SIP_PENDINGBYE		(1 << 6)	/*!< Need to send bye after we ack? */
#define SIP_GOTREFER		(1 << 7)	/*!< Got a refer? */
#define SIP_PROMISCREDIR	(1 << 8)	/*!< Promiscuous redirection */
#define SIP_TRUSTRPID		(1 << 9)	/*!< Trust RPID headers? */
#define SIP_USEREQPHONE		(1 << 10)	/*!< Add user=phone to numeric URI. Default off */
#define SIP_REALTIME		(1 << 11)	/*!< Flag for realtime users */
#define SIP_USECLIENTCODE	(1 << 12)	/*!< Trust X-ClientCode info message */
#define SIP_OUTGOING		(1 << 13)	/*!< Is this an outgoing call? */
#define SIP_SELFDESTRUCT	(1 << 14)	
#define SIP_DYNAMIC		(1 << 15)	/*!< Is this a dynamic peer? */
/* --- Choices for DTMF support in SIP channel */
#define SIP_DTMF		(3 << 16)	/*!< three settings, uses two bits */
#define SIP_DTMF_RFC2833	(0 << 16)	/*!< RTP DTMF */
#define SIP_DTMF_INBAND		(1 << 16)	/*!< Inband audio, only for ULAW/ALAW */
#define SIP_DTMF_INFO		(2 << 16)	/*!< SIP Info messages */
#define SIP_DTMF_AUTO		(3 << 16)	/*!< AUTO switch between rfc2833 and in-band DTMF */
/* NAT settings */
#define SIP_NAT			(3 << 18)	/*!< four settings, uses two bits */
#define SIP_NAT_NEVER		(0 << 18)	/*!< No nat support */
#define SIP_NAT_RFC3581		(1 << 18)
#define SIP_NAT_ROUTE		(2 << 18)
#define SIP_NAT_ALWAYS		(3 << 18)
/* re-INVITE related settings */
#define SIP_REINVITE		(3 << 20)	/*!< two bits used */
#define SIP_CAN_REINVITE	(1 << 20)	/*!< allow peers to be reinvited to send media directly p2p */
#define SIP_REINVITE_UPDATE	(2 << 20)	/*!< use UPDATE (RFC3311) when reinviting this peer */
/* "insecure" settings */
#define SIP_INSECURE_PORT	(1 << 22)	/*!< don't require matching port for incoming requests */
#define SIP_INSECURE_INVITE	(1 << 23)	/*!< don't require authentication for incoming INVITEs */
/* Sending PROGRESS in-band settings */
#define SIP_PROG_INBAND		(3 << 24)	/*!< three settings, uses two bits */
#define SIP_PROG_INBAND_NEVER	(0 << 24)
#define SIP_PROG_INBAND_NO	(1 << 24)
#define SIP_PROG_INBAND_YES	(2 << 24)
/* Open Settlement Protocol authentication */
#define SIP_OSPAUTH		(3 << 26)	/*!< four settings, uses two bits */
#define SIP_OSPAUTH_NO		(0 << 26)
#define SIP_OSPAUTH_GATEWAY	(1 << 26)
#define SIP_OSPAUTH_PROXY	(2 << 26)
#define SIP_OSPAUTH_EXCLUSIVE	(3 << 26)
/* Call states */
#define SIP_CALL_ONHOLD		(1 << 28)	 
#define SIP_CALL_LIMIT		(1 << 29)
/* Remote Party-ID Support */
#define SIP_SENDRPID		(1 << 30)
/* Did this connection increment the counter of in-use calls? */
#define SIP_INC_COUNT (1 << 31)

#define SIP_FLAGS_TO_COPY \
	(SIP_PROMISCREDIR | SIP_TRUSTRPID | SIP_SENDRPID | SIP_DTMF | SIP_REINVITE | \
	 SIP_PROG_INBAND | SIP_OSPAUTH | SIP_USECLIENTCODE | SIP_NAT | \
	 SIP_INSECURE_PORT | SIP_INSECURE_INVITE)

/* a new page of flags for peer */
#define SIP_PAGE2_RTCACHEFRIENDS	(1 << 0)
#define SIP_PAGE2_RTUPDATE		(1 << 1)
#define SIP_PAGE2_RTAUTOCLEAR		(1 << 2)
#define SIP_PAGE2_IGNOREREGEXPIRE	(1 << 3)
#define SIP_PAGE2_RT_FROMCONTACT 	(1 << 4)
#define SIP_PAGE2_DEBUG			(3 << 5)
#define SIP_PAGE2_DEBUG_CONFIG 		(1 << 5)
#define SIP_PAGE2_DEBUG_CONSOLE 	(1 << 6)

/* SIP packet flags */
#define SIP_PKT_DEBUG		(1 << 0)	/*!< Debug this packet */
#define SIP_PKT_WITH_TOTAG	(1 << 1)	/*!< This packet has a to-tag */

#define sipdebug		ast_test_flag(&global_flags_page2, SIP_PAGE2_DEBUG)
#define sipdebug_config		ast_test_flag(&global_flags_page2, SIP_PAGE2_DEBUG_CONFIG)
#define sipdebug_console	ast_test_flag(&global_flags_page2, SIP_PAGE2_DEBUG_CONSOLE)

static int global_rtautoclear = 120;

/*! \brief sip_pvt: PVT structures are used for each SIP dialog, ie. a call, a registration, a subscribe  */
static struct sip_pvt {
	ast_mutex_t lock;			/*!< Dialog private lock */
	int method;				/*!< SIP method that opened this dialog */
	AST_DECLARE_STRING_FIELDS(
		AST_STRING_FIELD(callid);	/*!< Global CallID */
		AST_STRING_FIELD(randdata);	/*!< Random data */
		AST_STRING_FIELD(accountcode);	/*!< Account code */
		AST_STRING_FIELD(realm);	/*!< Authorization realm */
		AST_STRING_FIELD(nonce);	/*!< Authorization nonce */
		AST_STRING_FIELD(opaque);	/*!< Opaque nonsense */
		AST_STRING_FIELD(qop);		/*!< Quality of Protection, since SIP wasn't complicated enough yet. */
		AST_STRING_FIELD(domain);	/*!< Authorization domain */
		AST_STRING_FIELD(refer_to);	/*!< Place to store REFER-TO extension */
		AST_STRING_FIELD(referred_by);	/*!< Place to store REFERRED-BY extension */
		AST_STRING_FIELD(refer_contact);/*!< Place to store Contact info from a REFER extension */
		AST_STRING_FIELD(from);		/*!< The From: header */
		AST_STRING_FIELD(useragent);	/*!< User agent in SIP request */
		AST_STRING_FIELD(exten);	/*!< Extension where to start */
		AST_STRING_FIELD(context);	/*!< Context for this call */
		AST_STRING_FIELD(subscribecontext); /*!< Subscribecontext */
		AST_STRING_FIELD(fromdomain);	/*!< Domain to show in the from field */
		AST_STRING_FIELD(fromuser);	/*!< User to show in the user field */
		AST_STRING_FIELD(fromname);	/*!< Name to show in the user field */
		AST_STRING_FIELD(tohost);	/*!< Host we should put in the "to" field */
		AST_STRING_FIELD(language);	/*!< Default language for this call */
		AST_STRING_FIELD(musicclass);	/*!< Music on Hold class */
		AST_STRING_FIELD(rdnis);	/*!< Referring DNIS */
		AST_STRING_FIELD(theirtag);	/*!< Their tag */
		AST_STRING_FIELD(username);	/*!< [user] name */
		AST_STRING_FIELD(peername);	/*!< [peer] name, not set if [user] */
		AST_STRING_FIELD(authname);	/*!< Who we use for authentication */
		AST_STRING_FIELD(uri);		/*!< Original requested URI */
		AST_STRING_FIELD(okcontacturi);	/*!< URI from the 200 OK on INVITE */
		AST_STRING_FIELD(peersecret);	/*!< Password */
		AST_STRING_FIELD(peermd5secret);
		AST_STRING_FIELD(cid_num);	/*!< Caller*ID */
		AST_STRING_FIELD(cid_name);	/*!< Caller*ID */
		AST_STRING_FIELD(via);		/*!< Via: header */
		AST_STRING_FIELD(fullcontact);	/*!< The Contact: that the UA registers with us */
		AST_STRING_FIELD(our_contact);	/*!< Our contact header */
		AST_STRING_FIELD(rpid);		/*!< Our RPID header */
		AST_STRING_FIELD(rpid_from);	/*!< Our RPID From header */
	);
	struct ast_codec_pref prefs;		/*!< codec prefs */
	unsigned int ocseq;			/*!< Current outgoing seqno */
	unsigned int icseq;			/*!< Current incoming seqno */
	ast_group_t callgroup;			/*!< Call group */
	ast_group_t pickupgroup;		/*!< Pickup group */
	int lastinvite;				/*!< Last Cseq of invite */
	unsigned int flags;			/*!< SIP_ flags */	
	int timer_t1;				/*!< SIP timer T1, ms rtt */
	unsigned int sipoptions;		/*!< Supported SIP sipoptions on the other end */
	int capability;				/*!< Special capability (codec) */
	int jointcapability;			/*!< Supported capability at both ends (codecs ) */
	int peercapability;			/*!< Supported peer capability */
	int prefcodec;				/*!< Preferred codec (outbound only) */
	int noncodeccapability;
	int callingpres;			/*!< Calling presentation */
	int authtries;				/*!< Times we've tried to authenticate */
	int expiry;				/*!< How long we take to expire */
	int branch;				/*!< One random number */
	char tag[11];				/*!< Another random number */
	int sessionid;				/*!< SDP Session ID */
	int sessionversion;			/*!< SDP Session Version */
	struct sockaddr_in sa;			/*!< Our peer */
	struct sockaddr_in redirip;		/*!< Where our RTP should be going if not to us */
	struct sockaddr_in vredirip;		/*!< Where our Video RTP should be going if not to us */
	int redircodecs;			/*!< Redirect codecs */
	struct sockaddr_in recv;		/*!< Received as */
	struct in_addr ourip;			/*!< Our IP */
	struct ast_channel *owner;		/*!< Who owns us */
	struct sip_pvt *refer_call;		/*!< Call we are referring */
	struct sip_route *route;		/*!< Head of linked list of routing steps (fm Record-Route) */
	int route_persistant;			/*!< Is this the "real" route? */
	struct sip_auth *peerauth;		/*!< Realm authentication */
	int noncecount;				/*!< Nonce-count */
	char lastmsg[256];			/*!< Last Message sent/received */
	int amaflags;				/*!< AMA Flags */
	int pendinginvite;			/*!< Any pending invite */
#ifdef OSP_SUPPORT
	int osphandle;				/*!< OSP Handle for call */
	time_t ospstart;			/*!< OSP Start time */
	unsigned int osptimelimit;		/*!< OSP call duration limit */
#endif
	struct sip_request initreq;		/*!< Initial request */
	
	int maxtime;				/*!< Max time for first response */
	int initid;				/*!< Auto-congest ID if appropriate */
	int autokillid;				/*!< Auto-kill ID */
	time_t lastrtprx;			/*!< Last RTP received */
	time_t lastrtptx;			/*!< Last RTP sent */
	int rtptimeout;				/*!< RTP timeout time */
	int rtpholdtimeout;			/*!< RTP timeout when on hold */
	int rtpkeepalive;			/*!< Send RTP packets for keepalive */
	enum subscriptiontype subscribed;	/*!< Is this dialog a subscription?  */
	int stateid;
	int laststate;                          /*!< Last known extension state */
	int dialogver;
	
	struct ast_dsp *vad;			/*!< Voice Activation Detection dsp */
	
	struct sip_peer *peerpoke;		/*!< If this dialog is to poke a peer, which one */
	struct sip_registry *registry;		/*!< If this is a REGISTER dialog, to which registry */
	struct ast_rtp *rtp;			/*!< RTP Session */
	struct ast_rtp *vrtp;			/*!< Video RTP session */
	struct sip_pkt *packets;		/*!< Packets scheduled for re-transmission */
	struct sip_history_head *history;	/*!< History of this SIP dialog */
	struct ast_variable *chanvars;		/*!< Channel variables to set for call */
	struct sip_pvt *next;			/*!< Next dialog in chain */
	struct sip_invite_param *options;	/*!< Options for INVITE */
} *iflist = NULL;

#define FLAG_RESPONSE (1 << 0)
#define FLAG_FATAL (1 << 1)

/*! \brief sip packet - read in sipsock_read(), transmitted in send_request() */
struct sip_pkt {
	struct sip_pkt *next;			/*!< Next packet */
	int retrans;				/*!< Retransmission number */
	int method;				/*!< SIP method for this packet */
	int seqno;				/*!< Sequence number */
	unsigned int flags;			/*!< non-zero if this is a response packet (e.g. 200 OK) */
	struct sip_pvt *owner;			/*!< Owner AST call */
	int retransid;				/*!< Retransmission ID */
	int timer_a;				/*!< SIP timer A, retransmission timer */
	int timer_t1;				/*!< SIP Timer T1, estimated RTT or 500 ms */
	int packetlen;				/*!< Length of packet */
	char data[0];
};	

/*! \brief Structure for SIP user data. User's place calls to us */
struct sip_user {
	/* Users who can access various contexts */
	ASTOBJ_COMPONENTS(struct sip_user);
	char secret[80];		/*!< Password */
	char md5secret[80];		/*!< Password in md5 */
	char context[AST_MAX_CONTEXT];	/*!< Default context for incoming calls */
	char subscribecontext[AST_MAX_CONTEXT];	/* Default context for subscriptions */
	char cid_num[80];		/*!< Caller ID num */
	char cid_name[80];		/*!< Caller ID name */
	char accountcode[AST_MAX_ACCOUNT_CODE];	/* Account code */
	char language[MAX_LANGUAGE];	/*!< Default language for this user */
	char musicclass[MAX_MUSICCLASS];/*!< Music on Hold class */
	char useragent[256];		/*!< User agent in SIP request */
	struct ast_codec_pref prefs;	/*!< codec prefs */
	ast_group_t callgroup;		/*!< Call group */
	ast_group_t pickupgroup;	/*!< Pickup Group */
	unsigned int flags;		/*!< SIP flags */	
	unsigned int sipoptions;	/*!< Supported SIP options */
	struct ast_flags flags_page2;	/*!< SIP_PAGE2 flags */
	int amaflags;			/*!< AMA flags for billing */
	int callingpres;		/*!< Calling id presentation */
	int capability;			/*!< Codec capability */
	int inUse;			/*!< Number of calls in use */
	int call_limit;			/*!< Limit of concurrent calls */
	struct ast_ha *ha;		/*!< ACL setting */
	struct ast_variable *chanvars;	/*!< Variables to set for channel created by user */
};

/* Structure for SIP peer data, we place calls to peers if registered  or fixed IP address (host) */
struct sip_peer {
	ASTOBJ_COMPONENTS(struct sip_peer);	/*!< name, refcount, objflags,  object pointers */
					/*!< peer->name is the unique name of this object */
	char secret[80];		/*!< Password */
	char md5secret[80];		/*!< Password in MD5 */
	struct sip_auth *auth;		/*!< Realm authentication list */
	char context[AST_MAX_CONTEXT];	/*!< Default context for incoming calls */
	char subscribecontext[AST_MAX_CONTEXT];	/*!< Default context for subscriptions */
	char username[80];		/*!< Temporary username until registration */ 
	char accountcode[AST_MAX_ACCOUNT_CODE];	/*!< Account code */
	int amaflags;			/*!< AMA Flags (for billing) */
	char tohost[MAXHOSTNAMELEN];	/*!< If not dynamic, IP address */
	char regexten[AST_MAX_EXTENSION]; /*!< Extension to register (if regcontext is used) */
	char fromuser[80];		/*!< From: user when calling this peer */
	char fromdomain[MAXHOSTNAMELEN];	/*!< From: domain when calling this peer */
	char fullcontact[256];		/*!< Contact registered with us (not in sip.conf) */
	char cid_num[80];		/*!< Caller ID num */
	char cid_name[80];		/*!< Caller ID name */
	int callingpres;		/*!< Calling id presentation */
	int inUse;			/*!< Number of calls in use */
	int call_limit;			/*!< Limit of concurrent calls */
	char vmexten[AST_MAX_EXTENSION]; /*!< Dialplan extension for MWI notify message*/
	char mailbox[AST_MAX_EXTENSION]; /*!< Mailbox setting for MWI checks */
	char language[MAX_LANGUAGE];	/*!<  Default language for prompts */
	char musicclass[MAX_MUSICCLASS];/*!<  Music on Hold class */
	char useragent[256];		/*!<  User agent in SIP request (saved from registration) */
	struct ast_codec_pref prefs;	/*!<  codec prefs */
	int lastmsgssent;
	time_t	lastmsgcheck;		/*!<  Last time we checked for MWI */
	unsigned int flags;		/*!<  SIP flags */	
	unsigned int sipoptions;	/*!<  Supported SIP options */
	struct ast_flags flags_page2;	/*!<  SIP_PAGE2 flags */
	int expire;			/*!<  When to expire this peer registration */
	int capability;			/*!<  Codec capability */
	int rtptimeout;			/*!<  RTP timeout */
	int rtpholdtimeout;		/*!<  RTP Hold Timeout */
	int rtpkeepalive;		/*!<  Send RTP packets for keepalive */
	ast_group_t callgroup;		/*!<  Call group */
	ast_group_t pickupgroup;	/*!<  Pickup group */
	struct ast_dnsmgr_entry *dnsmgr;/*!<  DNS refresh manager for peer */
	struct sockaddr_in addr;	/*!<  IP address of peer */

	/* Qualification */
	struct sip_pvt *call;		/*!<  Call pointer */
	int pokeexpire;			/*!<  When to expire poke (qualify= checking) */
	int lastms;			/*!<  How long last response took (in ms), or -1 for no response */
	int maxms;			/*!<  Max ms we will accept for the host to be up, 0 to not monitor */
	struct timeval ps;		/*!<  Ping send time */
	
	struct sockaddr_in defaddr;	/*!<  Default IP address, used until registration */
	struct ast_ha *ha;		/*!<  Access control list */
	struct ast_variable *chanvars;	/*!<  Variables to set for channel created by user */
	int lastmsg;
};

AST_MUTEX_DEFINE_STATIC(sip_reload_lock);
static int sip_reloading = 0;
static enum channelreloadreason sip_reloadreason;	/*!< Reason for last reload/load of configuration */

/* States for outbound registrations (with register= lines in sip.conf */
#define REG_STATE_UNREGISTERED		0
#define REG_STATE_REGSENT		1
#define REG_STATE_AUTHSENT		2
#define REG_STATE_REGISTERED   		3
#define REG_STATE_REJECTED	   	4
#define REG_STATE_TIMEOUT	   	5
#define REG_STATE_NOAUTH	   	6
#define REG_STATE_FAILED		7


/*! \brief sip_registry: Registrations with other SIP proxies */
struct sip_registry {
	ASTOBJ_COMPONENTS_FULL(struct sip_registry,1,1);
	AST_DECLARE_STRING_FIELDS(
		AST_STRING_FIELD(callid);	/*!< Global Call-ID */
		AST_STRING_FIELD(realm);	/*!< Authorization realm */
		AST_STRING_FIELD(nonce);	/*!< Authorization nonce */
		AST_STRING_FIELD(opaque);	/*!< Opaque nonsense */
		AST_STRING_FIELD(qop);		/*!< Quality of Protection, since SIP wasn't complicated enough yet. */
		AST_STRING_FIELD(domain);	/*!< Authorization domain */
		AST_STRING_FIELD(username);	/*!< Who we are registering as */
		AST_STRING_FIELD(authuser);	/*!< Who we *authenticate* as */
		AST_STRING_FIELD(hostname);	/*!< Domain or host we register to */
		AST_STRING_FIELD(secret);	/*!< Password in clear text */	
		AST_STRING_FIELD(md5secret);	/*!< Password in md5 */
		AST_STRING_FIELD(contact);	/*!< Contact extension */
		AST_STRING_FIELD(random);
	);
	int portno;			/*!<  Optional port override */
	int expire;			/*!< Sched ID of expiration */
	int regattempts;		/*!< Number of attempts (since the last success) */
	int timeout; 			/*!< sched id of sip_reg_timeout */
	int refresh;			/*!< How often to refresh */
	struct sip_pvt *call;		/*!< create a sip_pvt structure for each outbound "registration dialog" in progress */
	int regstate;			/*!< Registration state (see above) */
	int callid_valid;		/*!< 0 means we haven't chosen callid for this registry yet. */
	unsigned int ocseq;		/*!< Sequence number we got to for REGISTERs for this registry */
	struct sockaddr_in us;		/*!< Who the server thinks we are */
	int noncecount;			/*!< Nonce-count */
	char lastmsg[256];		/*!< Last Message sent/received */
};

/*! \brief  The user list: Users and friends */
static struct ast_user_list {
	ASTOBJ_CONTAINER_COMPONENTS(struct sip_user);
} userl;

/*! \brief  The peer list: Peers and Friends */
static struct ast_peer_list {
	ASTOBJ_CONTAINER_COMPONENTS(struct sip_peer);
} peerl;

/*! \brief  The register list: Other SIP proxys we register with and place calls to */
static struct ast_register_list {
	ASTOBJ_CONTAINER_COMPONENTS(struct sip_registry);
	int recheck;
} regl;


static int __sip_do_register(struct sip_registry *r);

static int sipsock  = -1;


static struct sockaddr_in bindaddr = { 0, };
static struct sockaddr_in externip;
static char externhost[MAXHOSTNAMELEN] = "";
static time_t externexpire = 0;
static int externrefresh = 10;
static struct ast_ha *localaddr;
static int callevents;				/*!< Whether we send manager events or not */

/* The list of manual NOTIFY types we know how to send */
struct ast_config *notify_types;

static struct sip_auth *authl = NULL;          /*!< Authentication list */


static int transmit_response(struct sip_pvt *p, char *msg, struct sip_request *req);
static int transmit_response_with_sdp(struct sip_pvt *p, char *msg, struct sip_request *req, int retrans);
static int transmit_response_with_unsupported(struct sip_pvt *p, char *msg, struct sip_request *req, char *unsupported);
static int transmit_response_with_auth(struct sip_pvt *p, char *msg, struct sip_request *req, const char *rand, int reliable, char *header, int stale);
static int transmit_request(struct sip_pvt *p, int sipmethod, int inc, int reliable, int newbranch);
static int transmit_request_with_auth(struct sip_pvt *p, int sipmethod, int inc, int reliable, int newbranch);
static int transmit_invite(struct sip_pvt *p, int sipmethod, int sendsdp, int init);
static int transmit_reinvite_with_sdp(struct sip_pvt *p);
static int transmit_info_with_digit(struct sip_pvt *p, char digit);
static int transmit_info_with_vidupdate(struct sip_pvt *p);
static int transmit_message_with_text(struct sip_pvt *p, const char *text);
static int transmit_refer(struct sip_pvt *p, const char *dest);
static int sip_sipredirect(struct sip_pvt *p, const char *dest);
static struct sip_peer *temp_peer(const char *name);
static int do_proxy_auth(struct sip_pvt *p, struct sip_request *req, char *header, char *respheader, int sipmethod, int init);
static void free_old_route(struct sip_route *route);
static int build_reply_digest(struct sip_pvt *p, int method, char *digest, int digest_len);
static int update_call_counter(struct sip_pvt *fup, int event);
static struct sip_peer *build_peer(const char *name, struct ast_variable *v, int realtime);
static struct sip_user *build_user(const char *name, struct ast_variable *v, int realtime);
static int sip_do_reload(enum channelreloadreason reason);
static int expire_register(void *data);

static struct ast_channel *sip_request_call(const char *type, int format, void *data, int *cause);
static int sip_devicestate(void *data);
static int sip_sendtext(struct ast_channel *ast, const char *text);
static int sip_call(struct ast_channel *ast, char *dest, int timeout);
static int sip_hangup(struct ast_channel *ast);
static int sip_answer(struct ast_channel *ast);
static struct ast_frame *sip_read(struct ast_channel *ast);
static int sip_write(struct ast_channel *ast, struct ast_frame *frame);
static int sip_indicate(struct ast_channel *ast, int condition);
static int sip_transfer(struct ast_channel *ast, const char *dest);
static int sip_fixup(struct ast_channel *oldchan, struct ast_channel *newchan);
static int sip_senddigit(struct ast_channel *ast, char digit);
static int clear_realm_authentication(struct sip_auth *authlist);                            /* Clear realm authentication list (at reload) */
static struct sip_auth *add_realm_authentication(struct sip_auth *authlist, char *configuration, int lineno);   /* Add realm authentication in list */
static struct sip_auth *find_realm_authentication(struct sip_auth *authlist, const char *realm);         /* Find authentication for a specific realm */
static int check_sip_domain(const char *domain, char *context, size_t len); /* Check if domain is one of our local domains */
static void append_date(struct sip_request *req);	/* Append date to SIP packet */
static int determine_firstline_parts(struct sip_request *req);
static void sip_dump_history(struct sip_pvt *dialog);	/* Dump history to LOG_DEBUG at end of dialog, before destroying data */
static const struct cfsubscription_types *find_subscription_type(enum subscriptiontype subtype);
static int transmit_state_notify(struct sip_pvt *p, int state, int full);
static char *gettag(struct sip_request *req, char *header, char *tagbuf, int tagbufsize);
int find_sip_method(char *msg);
unsigned int parse_sip_options(struct sip_pvt *pvt, char *supported);

/*! \brief Definition of this channel for PBX channel registration */
static const struct ast_channel_tech sip_tech = {
	.type = channeltype,
	.description = "Session Initiation Protocol (SIP)",
	.capabilities = ((AST_FORMAT_MAX_AUDIO << 1) - 1),
	.properties = AST_CHAN_TP_WANTSJITTER,
	.requester = sip_request_call,
	.devicestate = sip_devicestate,
	.call = sip_call,
	.hangup = sip_hangup,
	.answer = sip_answer,
	.read = sip_read,
	.write = sip_write,
	.write_video = sip_write,
	.indicate = sip_indicate,
	.transfer = sip_transfer,
	.fixup = sip_fixup,
	.send_digit = sip_senddigit,
	.bridge = ast_rtp_bridge,
	.send_text = sip_sendtext,
};

/*!
  \brief Thread-safe random number generator
  \return a random number

  This function uses a mutex lock to guarantee that no
  two threads will receive the same random number.
 */
static force_inline int thread_safe_rand(void)
{
	int val;

	ast_mutex_lock(&rand_lock);
	val = rand();
	ast_mutex_unlock(&rand_lock);
	
	return val;
}

/*! \brief  find_sip_method: Find SIP method from header
 * Strictly speaking, SIP methods are case SENSITIVE, but we don't check 
 * following Jon Postel's rule: Be gentle in what you accept, strict with what you send */
int find_sip_method(char *msg)
{
	int i, res = 0;
	
	if (ast_strlen_zero(msg))
		return 0;

	for (i = 1; (i < (sizeof(sip_methods) / sizeof(sip_methods[0]))) && !res; i++) {
		if (!strcasecmp(sip_methods[i].text, msg)) 
			res = sip_methods[i].id;
	}
	return res;
}

/*! \brief  parse_sip_options: Parse supported header in incoming packet */
unsigned int parse_sip_options(struct sip_pvt *pvt, char *supported)
{
	char *next = NULL;
	char *sep = NULL;
	char *temp = ast_strdupa(supported);
	int i;
	unsigned int profile = 0;

	if (ast_strlen_zero(supported) )
		return 0;

	if (option_debug > 2 && sipdebug)
		ast_log(LOG_DEBUG, "Begin: parsing SIP \"Supported: %s\"\n", supported);

	next = temp;
	while (next) {
		char res=0;
		if ( (sep = strchr(next, ',')) != NULL) {
			*sep = '\0';
			sep++;
		}
		while (*next == ' ')	/* Skip spaces */
			next++;
		if (option_debug > 2 && sipdebug)
			ast_log(LOG_DEBUG, "Found SIP option: -%s-\n", next);
		for (i=0; (i < (sizeof(sip_options) / sizeof(sip_options[0]))) && !res; i++) {
			if (!strcasecmp(next, sip_options[i].text)) {
				profile |= sip_options[i].id;
				res = 1;
				if (option_debug > 2 && sipdebug)
					ast_log(LOG_DEBUG, "Matched SIP option: %s\n", next);
			}
		}
		if (!res) 
			if (option_debug > 2 && sipdebug)
				ast_log(LOG_DEBUG, "Found no match for SIP option: %s (Please file bug report!)\n", next);
		next = sep;
	}
	if (pvt) {
		pvt->sipoptions = profile;
		if (option_debug)
			ast_log(LOG_DEBUG, "* SIP extension value: %d for call %s\n", profile, pvt->callid);
	}
	return profile;
}

/*! \brief  sip_debug_test_addr: See if we pass debug IP filter */
static inline int sip_debug_test_addr(struct sockaddr_in *addr) 
{
	if (!sipdebug)
		return 0;
	if (debugaddr.sin_addr.s_addr) {
		if (((ntohs(debugaddr.sin_port) != 0)
			&& (debugaddr.sin_port != addr->sin_port))
			|| (debugaddr.sin_addr.s_addr != addr->sin_addr.s_addr))
			return 0;
	}
	return 1;
}

/*! \brief  sip_debug_test_pvt: Test PVT for debugging output */
static inline int sip_debug_test_pvt(struct sip_pvt *p) 
{
	if (!sipdebug)
		return 0;
	return sip_debug_test_addr(((ast_test_flag(p, SIP_NAT) & SIP_NAT_ROUTE) ? &p->recv : &p->sa));
}


/*! \brief  __sip_xmit: Transmit SIP message */
static int __sip_xmit(struct sip_pvt *p, char *data, int len)
{
	int res;
	char iabuf[INET_ADDRSTRLEN];

	if (ast_test_flag(p, SIP_NAT) & SIP_NAT_ROUTE)
		res=sendto(sipsock, data, len, 0, (struct sockaddr *)&p->recv, sizeof(struct sockaddr_in));
	else
		res=sendto(sipsock, data, len, 0, (struct sockaddr *)&p->sa, sizeof(struct sockaddr_in));

	if (res != len) {
		ast_log(LOG_WARNING, "sip_xmit of %p (len %d) to %s:%d returned %d: %s\n", data, len, ast_inet_ntoa(iabuf, sizeof(iabuf), p->sa.sin_addr), ntohs(p->sa.sin_port), res, strerror(errno));
	}
	return res;
}

static void sip_destroy(struct sip_pvt *p);

/*! \brief  build_via: Build a Via header for a request */
static void build_via(struct sip_pvt *p)
{
	char iabuf[INET_ADDRSTRLEN];
	/* Work around buggy UNIDEN UIP200 firmware */
	const char *rport = ast_test_flag(p, SIP_NAT) & SIP_NAT_RFC3581 ? ";rport" : "";

	/* z9hG4bK is a magic cookie.  See RFC 3261 section 8.1.1.7 */
	ast_string_field_build(p, via, "SIP/2.0/UDP %s:%d;branch=z9hG4bK%08x%s",
			       ast_inet_ntoa(iabuf, sizeof(iabuf), p->ourip), ourport, p->branch, rport);
}

/*! \brief  ast_sip_ouraddrfor: NAT fix - decide which IP address to use for ASterisk server? */
/* Only used for outbound registrations */
static int ast_sip_ouraddrfor(struct in_addr *them, struct in_addr *us)
{
	/*
	 * Using the localaddr structure built up with localnet statements
	 * apply it to their address to see if we need to substitute our
	 * externip or can get away with our internal bindaddr
	 */
	struct sockaddr_in theirs;
	theirs.sin_addr = *them;
	if (localaddr && externip.sin_addr.s_addr &&
	   ast_apply_ha(localaddr, &theirs)) {
		char iabuf[INET_ADDRSTRLEN];
		if (externexpire && (time(NULL) >= externexpire)) {
			struct ast_hostent ahp;
			struct hostent *hp;
			time(&externexpire);
			externexpire += externrefresh;
			if ((hp = ast_gethostbyname(externhost, &ahp))) {
				memcpy(&externip.sin_addr, hp->h_addr, sizeof(externip.sin_addr));
			} else
				ast_log(LOG_NOTICE, "Warning: Re-lookup of '%s' failed!\n", externhost);
		}
		memcpy(us, &externip.sin_addr, sizeof(struct in_addr));
		ast_inet_ntoa(iabuf, sizeof(iabuf), *(struct in_addr *)&them->s_addr);
		ast_log(LOG_DEBUG, "Target address %s is not local, substituting externip\n", iabuf);
	}
	else if (bindaddr.sin_addr.s_addr)
		memcpy(us, &bindaddr.sin_addr, sizeof(struct in_addr));
	else
		return ast_ouraddrfor(them, us);
	return 0;
}

/*! \brief  append_history: Append to SIP dialog history 
	\return Always returns 0 */
#define append_history(p, event, fmt , args... )	append_history_full(p, "%-15s " fmt, event, ## args)

static int append_history_full(struct sip_pvt *p, const char *fmt, ...)
	__attribute__ ((format (printf, 2, 3)));

/*! \brief  Append to SIP dialog history with arg list  */
static void append_history_va(struct sip_pvt *p, const char *fmt, va_list ap)
{
	char buf[80], *c = buf; /* max history length */
	struct sip_history *hist;
	int l;

	vsnprintf(buf, sizeof(buf), fmt, ap);
	strsep(&c, "\r\n"); /* Trim up everything after \r or \n */
	l = strlen(buf) + 1;
	if (!(hist = ast_calloc(1, sizeof(*hist) + l)))
		return;
	if (!p->history && !(p->history = ast_calloc(1, sizeof(*p->history)))) {
		free(hist);
		return;
	}
	memcpy(hist->event, buf, l);
	AST_LIST_INSERT_TAIL(p->history, hist, list);
}

/*! \brief  Append to SIP dialog history with arg list  */
static int append_history_full(struct sip_pvt *p, const char *fmt, ...)
{
        va_list ap;

	if (!recordhistory || !p)
		return 0;
        va_start(ap, fmt);
        append_history_va(p, fmt, ap);
        va_end(ap);

        return 0;
}

/*! \brief  retrans_pkt: Retransmit SIP message if no answer */
static int retrans_pkt(void *data)
{
	struct sip_pkt *pkt=data, *prev, *cur = NULL;
	char iabuf[INET_ADDRSTRLEN];
	int reschedule = DEFAULT_RETRANS;

	/* Lock channel */
	ast_mutex_lock(&pkt->owner->lock);

	if (pkt->retrans < MAX_RETRANS) {
		pkt->retrans++;
 		if (!pkt->timer_t1) {	/* Re-schedule using timer_a and timer_t1 */
			if (sipdebug && option_debug > 3)
 				ast_log(LOG_DEBUG, "SIP TIMER: Not rescheduling id #%d:%s (Method %d) (No timer T1)\n", pkt->retransid, sip_methods[pkt->method].text, pkt->method);
		} else {
 			int siptimer_a;

 			if (sipdebug && option_debug > 3)
 				ast_log(LOG_DEBUG, "SIP TIMER: Rescheduling retransmission #%d (%d) %s - %d\n", pkt->retransid, pkt->retrans, sip_methods[pkt->method].text, pkt->method);
 			if (!pkt->timer_a)
 				pkt->timer_a = 2 ;
 			else
 				pkt->timer_a = 2 * pkt->timer_a;
 
 			/* For non-invites, a maximum of 4 secs */
 			siptimer_a = pkt->timer_t1 * pkt->timer_a;	/* Double each time */
 			if (pkt->method != SIP_INVITE && siptimer_a > 4000)
 				siptimer_a = 4000;
 		
 			/* Reschedule re-transmit */
			reschedule = siptimer_a;
 			if (option_debug > 3)
 				ast_log(LOG_DEBUG, "** SIP timers: Rescheduling retransmission %d to %d ms (t1 %d ms (Retrans id #%d)) \n", pkt->retrans +1, siptimer_a, pkt->timer_t1, pkt->retransid);
 		} 

		if (pkt->owner && sip_debug_test_pvt(pkt->owner)) {
			if (ast_test_flag(pkt->owner, SIP_NAT) & SIP_NAT_ROUTE)
				ast_verbose("Retransmitting #%d (NAT) to %s:%d:\n%s\n---\n", pkt->retrans, ast_inet_ntoa(iabuf, sizeof(iabuf), pkt->owner->recv.sin_addr), ntohs(pkt->owner->recv.sin_port), pkt->data);
			else
				ast_verbose("Retransmitting #%d (no NAT) to %s:%d:\n%s\n---\n", pkt->retrans, ast_inet_ntoa(iabuf, sizeof(iabuf), pkt->owner->sa.sin_addr), ntohs(pkt->owner->sa.sin_port), pkt->data);
		}

		append_history(pkt->owner, "ReTx", "%d %s", reschedule, pkt->data);
		__sip_xmit(pkt->owner, pkt->data, pkt->packetlen);
		ast_mutex_unlock(&pkt->owner->lock);
		return  reschedule;
	} 
	/* Too many retries */
	if (pkt->owner && pkt->method != SIP_OPTIONS) {
		if (ast_test_flag(pkt, FLAG_FATAL) || sipdebug)	/* Tell us if it's critical or if we're debugging */ ast_log(LOG_WARNING, "Maximum retries exceeded on transmission %s for seqno %d (%s %s)\n", pkt->owner->callid, pkt->seqno, (ast_test_flag(pkt, FLAG_FATAL)) ? "Critical" : "Non-critical", (ast_test_flag(pkt, FLAG_RESPONSE)) ? "Response" : "Request"); } else {
		if ((pkt->method == SIP_OPTIONS) && sipdebug)
			ast_log(LOG_WARNING, "Cancelling retransmit of OPTIONs (call id %s) \n", pkt->owner->callid);
	}
	append_history(pkt->owner, "MaxRetries", "%s", (ast_test_flag(pkt, FLAG_FATAL)) ? "(Critical)" : "(Non-critical)");
 		
	pkt->retransid = -1;

	if (ast_test_flag(pkt, FLAG_FATAL)) {
		while(pkt->owner->owner && ast_mutex_trylock(&pkt->owner->owner->lock)) {
			ast_mutex_unlock(&pkt->owner->lock);
			usleep(1);
			ast_mutex_lock(&pkt->owner->lock);
		}
		if (pkt->owner->owner) {
			ast_set_flag(pkt->owner, SIP_ALREADYGONE);
			ast_log(LOG_WARNING, "Hanging up call %s - no reply to our critical packet.\n", pkt->owner->callid);
			ast_queue_hangup(pkt->owner->owner);
			ast_mutex_unlock(&pkt->owner->owner->lock);
		} else {
			/* If no channel owner, destroy now */
			ast_set_flag(pkt->owner, SIP_NEEDDESTROY);	
		}
	}
	/* In any case, go ahead and remove the packet */
	prev = NULL;
	cur = pkt->owner->packets;
	while(cur) {
		if (cur == pkt)
			break;
		prev = cur;
		cur = cur->next;
	}
	if (cur) {
		if (prev)
			prev->next = cur->next;
		else
			pkt->owner->packets = cur->next;
		ast_mutex_unlock(&pkt->owner->lock);
		free(cur);
		pkt = NULL;
	} else
		ast_log(LOG_WARNING, "Weird, couldn't find packet owner!\n");
	if (pkt)
		ast_mutex_unlock(&pkt->owner->lock);
	return 0;
}

/*! \brief  __sip_reliable_xmit: transmit packet with retransmits */
static int __sip_reliable_xmit(struct sip_pvt *p, int seqno, int resp, char *data, int len, int fatal, int sipmethod)
{
	struct sip_pkt *pkt;
	int siptimer_a = DEFAULT_RETRANS;

	if (!(pkt = ast_calloc(1, sizeof(*pkt) + len + 1)))
		return -1;
	memcpy(pkt->data, data, len);
	pkt->method = sipmethod;
	pkt->packetlen = len;
	pkt->next = p->packets;
	pkt->owner = p;
	pkt->seqno = seqno;
	pkt->flags = resp;
	pkt->data[len] = '\0';
	pkt->timer_t1 = p->timer_t1;	/* Set SIP timer T1 */
	if (fatal)
		ast_set_flag(pkt, FLAG_FATAL);
	if (pkt->timer_t1)
		siptimer_a = pkt->timer_t1 * 2;

	/* Schedule retransmission */
	pkt->retransid = ast_sched_add_variable(sched, siptimer_a, retrans_pkt, pkt, 1);
	if (option_debug > 3 && sipdebug)
		ast_log(LOG_DEBUG, "*** SIP TIMER: Initalizing retransmit timer on packet: Id  #%d\n", pkt->retransid);
	pkt->next = p->packets;
	p->packets = pkt;

	__sip_xmit(pkt->owner, pkt->data, pkt->packetlen);	/* Send packet */
	if (sipmethod == SIP_INVITE) {
		/* Note this is a pending invite */
		p->pendinginvite = seqno;
	}
	return 0;
}

/*! \brief  __sip_autodestruct: Kill a SIP dialog (called by scheduler) */
static int __sip_autodestruct(void *data)
{
	struct sip_pvt *p = data;


	/* If this is a subscription, tell the phone that we got a timeout */
	if (p->subscribed) {
		p->subscribed = TIMEOUT;
		transmit_state_notify(p, AST_EXTENSION_DEACTIVATED, 1);	/* Send last notification */
		p->subscribed = NONE;
		append_history(p, "Subscribestatus", "timeout");
		if (option_debug > 2)
			ast_log(LOG_DEBUG, "Re-scheduled destruction of SIP subsription %s\n", p->callid ? p->callid : "<unknown>");
		return 10000;	/* Reschedule this destruction so that we know that it's gone */
	}

	/* Reset schedule ID */
	p->autokillid = -1;

	if (option_debug)
		ast_log(LOG_DEBUG, "Auto destroying call '%s'\n", p->callid);
	append_history(p, "AutoDestroy", "");
	if (p->owner) {
		ast_log(LOG_WARNING, "Autodestruct on dialog '%s' with owner in place (Method: %s)\n", p->callid, sip_methods[p->method].text);
		ast_queue_hangup(p->owner);
	} else {
		sip_destroy(p);
	}
	return 0;
}

/*! \brief  sip_scheddestroy: Schedule destruction of SIP call */
static int sip_scheddestroy(struct sip_pvt *p, int ms)
{
	if (sip_debug_test_pvt(p))
		ast_verbose("Scheduling destruction of SIP dialog '%s' in %d ms (Method: %s)\n", p->callid, ms, sip_methods[p->method].text);
	if (recordhistory)
		append_history(p, "SchedDestroy", "%d ms", ms);

	if (p->autokillid > -1)
		ast_sched_del(sched, p->autokillid);
	p->autokillid = ast_sched_add(sched, ms, __sip_autodestruct, p);
	return 0;
}

/*! \brief  sip_cancel_destroy: Cancel destruction of SIP dialog */
static int sip_cancel_destroy(struct sip_pvt *p)
{
	if (p->autokillid > -1)
		ast_sched_del(sched, p->autokillid);
	append_history(p, "CancelDestroy", "");
	p->autokillid = -1;
	return 0;
}

/*! \brief  __sip_ack: Acknowledges receipt of a packet and stops retransmission */
static int __sip_ack(struct sip_pvt *p, int seqno, int resp, int sipmethod)
{
	struct sip_pkt *cur, *prev = NULL;
	int res = -1;
	int resetinvite = 0;
	/* Just in case... */
	char *msg;

	msg = sip_methods[sipmethod].text;

	cur = p->packets;
	while(cur) {
		if ((cur->seqno == seqno) && ((ast_test_flag(cur, FLAG_RESPONSE)) == resp) &&
			((ast_test_flag(cur, FLAG_RESPONSE)) || 
			 (!strncasecmp(msg, cur->data, strlen(msg)) && (cur->data[strlen(msg)] < 33)))) {
			ast_mutex_lock(&p->lock);
			if (!resp && (seqno == p->pendinginvite)) {
				ast_log(LOG_DEBUG, "Acked pending invite %d\n", p->pendinginvite);
				p->pendinginvite = 0;
				resetinvite = 1;
			}
			/* this is our baby */
			if (prev)
				prev->next = cur->next;
			else
				p->packets = cur->next;
			if (cur->retransid > -1) {
				if (sipdebug && option_debug > 3)
					ast_log(LOG_DEBUG, "** SIP TIMER: Cancelling retransmit of packet (reply received) Retransid #%d\n", cur->retransid);
				ast_sched_del(sched, cur->retransid);
			}
			free(cur);
			ast_mutex_unlock(&p->lock);
			res = 0;
			break;
		}
		prev = cur;
		cur = cur->next;
	}
	ast_log(LOG_DEBUG, "Stopping retransmission on '%s' of %s %d: Match %s\n", p->callid, resp ? "Response" : "Request", seqno, res ? "Not Found" : "Found");
	return res;
}

/* Pretend to ack all packets */
static int __sip_pretend_ack(struct sip_pvt *p)
{
	struct sip_pkt *cur=NULL;

	while(p->packets) {
		if (cur == p->packets) {
			ast_log(LOG_WARNING, "Have a packet that doesn't want to give up! %s\n", sip_methods[cur->method].text);
			return -1;
		}
		cur = p->packets;
		if (cur->method)
			__sip_ack(p, p->packets->seqno, (ast_test_flag(p->packets, FLAG_RESPONSE)), cur->method);
		else {	/* Unknown packet type */
			char *c;
			char method[128];
			ast_copy_string(method, p->packets->data, sizeof(method));
			c = ast_skip_blanks(method); /* XXX what ? */
			*c = '\0';
			__sip_ack(p, p->packets->seqno, (ast_test_flag(p->packets, FLAG_RESPONSE)), find_sip_method(method));
		}
	}
	return 0;
}

/*! \brief  __sip_semi_ack: Acks receipt of packet, keep it around (used for provisional responses) */
static int __sip_semi_ack(struct sip_pvt *p, int seqno, int resp, int sipmethod)
{
	struct sip_pkt *cur;
	int res = -1;
	char *msg = sip_methods[sipmethod].text;

	cur = p->packets;
	while(cur) {
		if ((cur->seqno == seqno) && ((ast_test_flag(cur, FLAG_RESPONSE)) == resp) &&
			((ast_test_flag(cur, FLAG_RESPONSE)) || 
			 (!strncasecmp(msg, cur->data, strlen(msg)) && (cur->data[strlen(msg)] < 33)))) {
			/* this is our baby */
			if (cur->retransid > -1) {
				if (option_debug > 3 && sipdebug)
					ast_log(LOG_DEBUG, "*** SIP TIMER: Cancelling retransmission #%d - %s (got response)\n", cur->retransid, msg);
				ast_sched_del(sched, cur->retransid);
			}
			cur->retransid = -1;
			res = 0;
			break;
		}
		cur = cur->next;
	}
	ast_log(LOG_DEBUG, "(Provisional) Stopping retransmission (but retaining packet) on '%s' %s %d: %s\n", p->callid, resp ? "Response" : "Request", seqno, res ? "Not Found" : "Found");
	return res;
}

static void parse_request(struct sip_request *req);
static char *get_header(struct sip_request *req, char *name);
static void copy_request(struct sip_request *dst,struct sip_request *src);

/*! \brief  parse_copy: Copy SIP request, parse it */
static void parse_copy(struct sip_request *dst, struct sip_request *src)
{
	memset(dst, 0, sizeof(*dst));
	memcpy(dst->data, src->data, sizeof(dst->data));
	dst->len = src->len;
	parse_request(dst);
}

/*! \brief  send_response: Transmit response on SIP request*/
static int send_response(struct sip_pvt *p, struct sip_request *req, int reliable, int seqno)
{
	int res;

	if (sip_debug_test_pvt(p)) {
		char iabuf[INET_ADDRSTRLEN];
		if (ast_test_flag(p, SIP_NAT) & SIP_NAT_ROUTE)
			ast_verbose("%sTransmitting (NAT) to %s:%d:\n%s\n---\n", reliable ? "Reliably " : "", ast_inet_ntoa(iabuf, sizeof(iabuf), p->recv.sin_addr), ntohs(p->recv.sin_port), req->data);
		else
			ast_verbose("%sTransmitting (no NAT) to %s:%d:\n%s\n---\n", reliable ? "Reliably " : "", ast_inet_ntoa(iabuf, sizeof(iabuf), p->sa.sin_addr), ntohs(p->sa.sin_port), req->data);
	}
	if (recordhistory) {
		struct sip_request tmp;
		parse_copy(&tmp, req);
		append_history(p, reliable ? "TxRespRel" : "TxResp", "%s / %s", tmp.data, get_header(&tmp, "CSeq"));
	}
	res = (reliable) ?
		__sip_reliable_xmit(p, seqno, 1, req->data, req->len, (reliable > 1), req->method) :
		__sip_xmit(p, req->data, req->len);
	if (res > 0)
		return 0;
	return res;
}

/*! \brief  send_request: Send SIP Request to the other part of the dialogue */
static int send_request(struct sip_pvt *p, struct sip_request *req, int reliable, int seqno)
{
	int res;

	if (sip_debug_test_pvt(p)) {
		char iabuf[INET_ADDRSTRLEN];
		if (ast_test_flag(p, SIP_NAT) & SIP_NAT_ROUTE)
			ast_verbose("%sTransmitting (NAT) to %s:%d:\n%s\n---\n", reliable ? "Reliably " : "", ast_inet_ntoa(iabuf, sizeof(iabuf), p->recv.sin_addr), ntohs(p->recv.sin_port), req->data);
		else
			ast_verbose("%sTransmitting (no NAT) to %s:%d:\n%s\n---\n", reliable ? "Reliably " : "", ast_inet_ntoa(iabuf, sizeof(iabuf), p->sa.sin_addr), ntohs(p->sa.sin_port), req->data);
	}
	if (recordhistory) {
		struct sip_request tmp;
		parse_copy(&tmp, req);
		append_history(p, reliable ? "TxReqRel" : "TxReq", "%s / %s", tmp.data, get_header(&tmp, "CSeq"));
	}
	res = (reliable) ?
		__sip_reliable_xmit(p, seqno, 0, req->data, req->len, (reliable > 1), req->method) :
		__sip_xmit(p, req->data, req->len);
	return res;
}

/*! \brief  get_in_brackets: Pick out text in brackets from character string */
/* returns pointer to terminated stripped string. modifies input string. */
static char *get_in_brackets(char *tmp)
{
	char *parse;
	char *first_quote;
	char *first_bracket;
	char *second_bracket;
	char last_char;

	parse = tmp;
	while (1) {
		first_quote = strchr(parse, '"');
		first_bracket = strchr(parse, '<');
		if (first_quote && first_bracket && (first_quote < first_bracket)) {
			last_char = '\0';
			for (parse = first_quote + 1; *parse; parse++) {
				if ((*parse == '"') && (last_char != '\\'))
					break;
				last_char = *parse;
			}
			if (!*parse) {
				ast_log(LOG_WARNING, "No closing quote found in '%s'\n", tmp);
				return tmp;
			}
			parse++;
			continue;
		}
		if (first_bracket) {
			second_bracket = strchr(first_bracket + 1, '>');
			if (second_bracket) {
				*second_bracket = '\0';
				return first_bracket + 1;
			} else {
				ast_log(LOG_WARNING, "No closing bracket found in '%s'\n", tmp);
				return tmp;
			}
		}
		return tmp;
	}
}

/*! \brief  sip_sendtext: Send SIP MESSAGE text within a call */
/*      Called from PBX core text message functions */
static int sip_sendtext(struct ast_channel *ast, const char *text)
{
	struct sip_pvt *p = ast->tech_pvt;
	int debug=sip_debug_test_pvt(p);

	if (debug)
		ast_verbose("Sending text %s on %s\n", text, ast->name);
	if (!p)
		return -1;
	if (ast_strlen_zero(text))
		return 0;
	if (debug)
		ast_verbose("Really sending text %s on %s\n", text, ast->name);
	transmit_message_with_text(p, text);
	return 0;	
}

/*! \brief  realtime_update_peer: Update peer object in realtime storage */
static void realtime_update_peer(const char *peername, struct sockaddr_in *sin, const char *username, const char *fullcontact, int expirey)
{
	char port[10];
	char ipaddr[20];
	char regseconds[20];
	time_t nowtime;
	
	time(&nowtime);
	nowtime += expirey;
	snprintf(regseconds, sizeof(regseconds), "%d", (int)nowtime);	/* Expiration time */
	ast_inet_ntoa(ipaddr, sizeof(ipaddr), sin->sin_addr);
	snprintf(port, sizeof(port), "%d", ntohs(sin->sin_port));
	
	if (fullcontact)
		ast_update_realtime("sippeers", "name", peername, "ipaddr", ipaddr, "port", port, "regseconds", regseconds, "username", username, "fullcontact", fullcontact, NULL);
	else
		ast_update_realtime("sippeers", "name", peername, "ipaddr", ipaddr, "port", port, "regseconds", regseconds, "username", username, NULL);
}

/*! \brief  register_peer_exten: Automatically add peer extension to dial plan */
static void register_peer_exten(struct sip_peer *peer, int onoff)
{
	char multi[256];
	char *stringp, *ext;
	if (!ast_strlen_zero(regcontext)) {
		ast_copy_string(multi, ast_strlen_zero(peer->regexten) ? peer->name : peer->regexten, sizeof(multi));
		stringp = multi;
		while((ext = strsep(&stringp, "&"))) {
			if (onoff)
				ast_add_extension(regcontext, 1, ext, 1, NULL, NULL, "Noop", ast_strdup(peer->name), free, channeltype);
			else
				ast_context_remove_extension(regcontext, ext, 1, NULL);
		}
	}
}

/*! \brief  sip_destroy_peer: Destroy peer object from memory */
static void sip_destroy_peer(struct sip_peer *peer)
{
	/* Delete it, it needs to disappear */
	if (peer->call)
		sip_destroy(peer->call);
	if (peer->chanvars) {
		ast_variables_destroy(peer->chanvars);
		peer->chanvars = NULL;
	}
	if (peer->expire > -1)
		ast_sched_del(sched, peer->expire);
	if (peer->pokeexpire > -1)
		ast_sched_del(sched, peer->pokeexpire);
	register_peer_exten(peer, 0);
	ast_free_ha(peer->ha);
	if (ast_test_flag(peer, SIP_SELFDESTRUCT))
		apeerobjs--;
	else if (ast_test_flag(peer, SIP_REALTIME))
		rpeerobjs--;
	else
		speerobjs--;
	clear_realm_authentication(peer->auth);
	peer->auth = (struct sip_auth *) NULL;
	if (peer->dnsmgr)
		ast_dnsmgr_release(peer->dnsmgr);
	free(peer);
}

/*! \brief  update_peer: Update peer data in database (if used) */
static void update_peer(struct sip_peer *p, int expiry)
{
	int rtcachefriends = ast_test_flag(&(p->flags_page2), SIP_PAGE2_RTCACHEFRIENDS);
	if (ast_test_flag((&global_flags_page2), SIP_PAGE2_RTUPDATE) &&
		(ast_test_flag(p, SIP_REALTIME) || rtcachefriends)) {
		realtime_update_peer(p->name, &p->addr, p->username, rtcachefriends ? p->fullcontact : NULL, expiry);
	}
}


/*! \brief  realtime_peer: Get peer from realtime storage
 * Checks the "sippeers" realtime family from extconfig.conf 
 * \todo Consider adding check of port address when matching here to follow the same
 * 	algorithm as for static peers. Will we break anything by adding that?
*/
static struct sip_peer *realtime_peer(const char *peername, struct sockaddr_in *sin)
{
	struct sip_peer *peer=NULL;
	struct ast_variable *var;
	struct ast_variable *tmp;
	char *newpeername = (char *) peername;
	char iabuf[80];

	/* First check on peer name */
	if (newpeername) 
		var = ast_load_realtime("sippeers", "name", peername, NULL);
	else if (sin) {	/* Then check on IP address for dynamic peers */
		ast_inet_ntoa(iabuf, sizeof(iabuf), sin->sin_addr);
		var = ast_load_realtime("sippeers", "host", iabuf, NULL);	/* First check for fixed IP hosts */
		if (!var)
			var = ast_load_realtime("sippeers", "ipaddr", iabuf, NULL);	/* Then check for registred hosts */
	
	} else
		return NULL;

	if (!var)
		return NULL;

	for (tmp = var; tmp; tmp = tmp->next) {
		/* If this is type=user, then skip this object. */
		if (!strcasecmp(tmp->name, "type") &&
		    !strcasecmp(tmp->value, "user")) {
			ast_variables_destroy(var);
			return NULL;
		} else if (!newpeername && !strcasecmp(tmp->name, "name")) {
			newpeername = tmp->value;
		}
	}
	
	if (!newpeername) {	/* Did not find peer in realtime */
		ast_log(LOG_WARNING, "Cannot Determine peer name ip=%s\n", iabuf);
		ast_variables_destroy(var);
		return (struct sip_peer *) NULL;
	}

	/* Peer found in realtime, now build it in memory */
	peer = build_peer(newpeername, var, !ast_test_flag((&global_flags_page2), SIP_PAGE2_RTCACHEFRIENDS));
	if (!peer) {
		ast_variables_destroy(var);
		return (struct sip_peer *) NULL;
	}

	if (ast_test_flag((&global_flags_page2), SIP_PAGE2_RTCACHEFRIENDS)) {
		/* Cache peer */
		ast_copy_flags((&peer->flags_page2),(&global_flags_page2), SIP_PAGE2_RTAUTOCLEAR|SIP_PAGE2_RTCACHEFRIENDS);
		if (ast_test_flag((&global_flags_page2), SIP_PAGE2_RTAUTOCLEAR)) {
			if (peer->expire > -1) {
				ast_sched_del(sched, peer->expire);
			}
			peer->expire = ast_sched_add(sched, (global_rtautoclear) * 1000, expire_register, (void *)peer);
		}
		ASTOBJ_CONTAINER_LINK(&peerl,peer);
	} else {
		ast_set_flag(peer, SIP_REALTIME);
	}
	ast_variables_destroy(var);

	return peer;
}

/*! \brief  sip_addrcmp: Support routine for find_peer */
static int sip_addrcmp(char *name, struct sockaddr_in *sin)
{
	/* We know name is the first field, so we can cast */
	struct sip_peer *p = (struct sip_peer *)name;
	return 	!(!inaddrcmp(&p->addr, sin) || 
					(ast_test_flag(p, SIP_INSECURE_PORT) &&
					(p->addr.sin_addr.s_addr == sin->sin_addr.s_addr)));
}

/*! \brief  find_peer: Locate peer by name or ip address 
 *	This is used on incoming SIP message to find matching peer on ip
	or outgoing message to find matching peer on name */
static struct sip_peer *find_peer(const char *peer, struct sockaddr_in *sin, int realtime)
{
	struct sip_peer *p = NULL;

	if (peer)
		p = ASTOBJ_CONTAINER_FIND(&peerl, peer);
	else
		p = ASTOBJ_CONTAINER_FIND_FULL(&peerl, sin, name, sip_addr_hashfunc, 1, sip_addrcmp);

	if (!p && realtime) {
		p = realtime_peer(peer, sin);
	}
	return p;
}

/*! \brief  sip_destroy_user: Remove user object from in-memory storage */
static void sip_destroy_user(struct sip_user *user)
{
	ast_free_ha(user->ha);
	if (user->chanvars) {
		ast_variables_destroy(user->chanvars);
		user->chanvars = NULL;
	}
	if (ast_test_flag(user, SIP_REALTIME))
		ruserobjs--;
	else
		suserobjs--;
	free(user);
}

/*! \brief  realtime_user: Load user from realtime storage
 * Loads user from "sipusers" category in realtime (extconfig.conf)
 * Users are matched on From: user name (the domain in skipped) */
static struct sip_user *realtime_user(const char *username)
{
	struct ast_variable *var;
	struct ast_variable *tmp;
	struct sip_user *user = NULL;

	var = ast_load_realtime("sipusers", "name", username, NULL);

	if (!var)
		return NULL;

	for (tmp = var; tmp; tmp = tmp->next) {
		if (!strcasecmp(tmp->name, "type") &&
			!strcasecmp(tmp->value, "peer")) {
			ast_variables_destroy(var);
			return NULL;
		}
	}

	user = build_user(username, var, !ast_test_flag((&global_flags_page2), SIP_PAGE2_RTCACHEFRIENDS));
	
	if (!user) {	/* No user found */
		ast_variables_destroy(var);
		return NULL;
	}

	if (ast_test_flag((&global_flags_page2), SIP_PAGE2_RTCACHEFRIENDS)) {
		ast_set_flag((&user->flags_page2), SIP_PAGE2_RTCACHEFRIENDS);
		suserobjs++;
		ASTOBJ_CONTAINER_LINK(&userl,user);
	} else {
		/* Move counter from s to r... */
		suserobjs--;
		ruserobjs++;
		ast_set_flag(user, SIP_REALTIME);
	}
	ast_variables_destroy(var);
	return user;
}

/*! \brief  find_user: Locate user by name 
 * Locates user by name (From: sip uri user name part) first
 * from in-memory list (static configuration) then from 
 * realtime storage (defined in extconfig.conf) */
static struct sip_user *find_user(const char *name, int realtime)
{
	struct sip_user *u = NULL;
	u = ASTOBJ_CONTAINER_FIND(&userl,name);
	if (!u && realtime) {
		u = realtime_user(name);
	}
	return u;
}

/*! \brief  create_addr_from_peer: create address structure from peer reference */
static int create_addr_from_peer(struct sip_pvt *r, struct sip_peer *peer)
{
	if ((peer->addr.sin_addr.s_addr || peer->defaddr.sin_addr.s_addr) &&
	    (!peer->maxms || ((peer->lastms >= 0)  && (peer->lastms <= peer->maxms)))) {
		if (peer->addr.sin_addr.s_addr) {
			r->sa.sin_family = peer->addr.sin_family;
			r->sa.sin_addr = peer->addr.sin_addr;
			r->sa.sin_port = peer->addr.sin_port;
		} else {
			r->sa.sin_family = peer->defaddr.sin_family;
			r->sa.sin_addr = peer->defaddr.sin_addr;
			r->sa.sin_port = peer->defaddr.sin_port;
		}
		memcpy(&r->recv, &r->sa, sizeof(r->recv));
	} else {
		return -1;
	}

	ast_copy_flags(r, peer, SIP_FLAGS_TO_COPY);
	r->capability = peer->capability;
	r->prefs = peer->prefs;
	if (r->rtp) {
		ast_log(LOG_DEBUG, "Setting NAT on RTP to %d\n", (ast_test_flag(r, SIP_NAT) & SIP_NAT_ROUTE));
		ast_rtp_setnat(r->rtp, (ast_test_flag(r, SIP_NAT) & SIP_NAT_ROUTE));
	}
	if (r->vrtp) {
		ast_log(LOG_DEBUG, "Setting NAT on VRTP to %d\n", (ast_test_flag(r, SIP_NAT) & SIP_NAT_ROUTE));
		ast_rtp_setnat(r->vrtp, (ast_test_flag(r, SIP_NAT) & SIP_NAT_ROUTE));
	}
	ast_string_field_set(r, peername, peer->username);
	ast_string_field_set(r, authname, peer->username);
	ast_string_field_set(r, username, peer->username);
	ast_string_field_set(r, peersecret, peer->secret);
	ast_string_field_set(r, peermd5secret, peer->md5secret);
	ast_string_field_set(r, tohost, peer->tohost);
	ast_string_field_set(r, fullcontact, peer->fullcontact);
	if (!r->initreq.headers && !ast_strlen_zero(peer->fromdomain)) {
		char *tmpcall;
		char *c;
		tmpcall = ast_strdupa(r->callid);
		if (tmpcall) {
			c = strchr(tmpcall, '@');
			if (c) {
				*c = '\0';
				ast_string_field_build(r, callid, "%s@%s", tmpcall, peer->fromdomain);
			}
		}
	}
	if (ast_strlen_zero(r->tohost)) {
		char iabuf[INET_ADDRSTRLEN];

		if (peer->addr.sin_addr.s_addr)
			ast_inet_ntoa(iabuf, sizeof(iabuf), peer->addr.sin_addr);
		else
			ast_inet_ntoa(iabuf, sizeof(iabuf), peer->defaddr.sin_addr);
		ast_string_field_set(r, tohost, iabuf);
	}
	if (!ast_strlen_zero(peer->fromdomain))
		ast_string_field_set(r, fromdomain, peer->fromdomain);
	if (!ast_strlen_zero(peer->fromuser))
		ast_string_field_set(r, fromuser, peer->fromuser);
	r->maxtime = peer->maxms;
	r->callgroup = peer->callgroup;
	r->pickupgroup = peer->pickupgroup;
	/* Set timer T1 to RTT for this peer (if known by qualify=) */
	if (peer->maxms && peer->lastms)
		r->timer_t1 = peer->lastms;
	if ((ast_test_flag(r, SIP_DTMF) == SIP_DTMF_RFC2833) || (ast_test_flag(r, SIP_DTMF) == SIP_DTMF_AUTO))
		r->noncodeccapability |= AST_RTP_DTMF;
	else
		r->noncodeccapability &= ~AST_RTP_DTMF;
	ast_string_field_set(r, context, peer->context);
	r->rtptimeout = peer->rtptimeout;
	r->rtpholdtimeout = peer->rtpholdtimeout;
	r->rtpkeepalive = peer->rtpkeepalive;
	if (peer->call_limit)
		ast_set_flag(r, SIP_CALL_LIMIT);

	return 0;
}

/*! \brief  create_addr: create address structure from peer name
 *      Or, if peer not found, find it in the global DNS 
 *      returns TRUE (-1) on failure, FALSE on success */
static int create_addr(struct sip_pvt *dialog, const char *opeer)
{
	struct hostent *hp;
	struct ast_hostent ahp;
	struct sip_peer *p;
	int found=0;
	char *port;
	int portno;
	char host[MAXHOSTNAMELEN], *hostn;
	char peer[256];

	ast_copy_string(peer, opeer, sizeof(peer));
	port = strchr(peer, ':');
	if (port) {
		*port = '\0';
		port++;
	}
	dialog->sa.sin_family = AF_INET;
	dialog->timer_t1 = 500; /* Default SIP retransmission timer T1 (RFC 3261) */
	p = find_peer(peer, NULL, 1);

	if (p) {
		found++;
		if (create_addr_from_peer(dialog, p))
			ASTOBJ_UNREF(p, sip_destroy_peer);
	}
	if (!p) {
		if (found)
			return -1;

		hostn = peer;
		if (port)
			portno = atoi(port);
		else
			portno = DEFAULT_SIP_PORT;
		if (srvlookup) {
			char service[MAXHOSTNAMELEN];
			int tportno;
			int ret;
			snprintf(service, sizeof(service), "_sip._udp.%s", peer);
			ret = ast_get_srv(NULL, host, sizeof(host), &tportno, service);
			if (ret > 0) {
				hostn = host;
				portno = tportno;
			}
		}
		hp = ast_gethostbyname(hostn, &ahp);
		if (hp) {
			ast_string_field_set(dialog, tohost, peer);
			memcpy(&dialog->sa.sin_addr, hp->h_addr, sizeof(dialog->sa.sin_addr));
			dialog->sa.sin_port = htons(portno);
			memcpy(&dialog->recv, &dialog->sa, sizeof(dialog->recv));
			return 0;
		} else {
			ast_log(LOG_WARNING, "No such host: %s\n", peer);
			return -1;
		}
	} else {
		ASTOBJ_UNREF(p, sip_destroy_peer);
		return 0;
	}
}

/*! \brief  auto_congest: Scheduled congestion on a call */
static int auto_congest(void *nothing)
{
	struct sip_pvt *p = nothing;
	ast_mutex_lock(&p->lock);
	p->initid = -1;
	if (p->owner) {
		if (!ast_mutex_trylock(&p->owner->lock)) {
			ast_log(LOG_NOTICE, "Auto-congesting %s\n", p->owner->name);
			ast_queue_control(p->owner, AST_CONTROL_CONGESTION);
			ast_mutex_unlock(&p->owner->lock);
		}
	}
	ast_mutex_unlock(&p->lock);
	return 0;
}




/*! \brief  sip_call: Initiate SIP call from PBX 
 *      used from the dial() application      */
static int sip_call(struct ast_channel *ast, char *dest, int timeout)
{
	int res;
	struct sip_pvt *p;
#ifdef OSP_SUPPORT
	const char *osphandle = NULL;
#endif	
	struct varshead *headp;
	struct ast_var_t *current;
	

	
	p = ast->tech_pvt;
	if ((ast->_state != AST_STATE_DOWN) && (ast->_state != AST_STATE_RESERVED)) {
		ast_log(LOG_WARNING, "sip_call called on %s, neither down nor reserved\n", ast->name);
		return -1;
	}


	/* Check whether there is vxml_url, distinctive ring variables */

	headp=&ast->varshead;
	AST_LIST_TRAVERSE(headp,current,entries) {
		/* Check whether there is a VXML_URL variable */
		if (!p->options->vxml_url && !strcasecmp(ast_var_name(current), "VXML_URL")) {
			p->options->vxml_url = ast_var_value(current);
               } else if (!p->options->uri_options && !strcasecmp(ast_var_name(current), "SIP_URI_OPTIONS")) {
                       p->options->uri_options = ast_var_value(current);
		} else if (!p->options->distinctive_ring && !strcasecmp(ast_var_name(current), "ALERT_INFO")) {
			/* Check whether there is a ALERT_INFO variable */
			p->options->distinctive_ring = ast_var_value(current);
		} else if (!p->options->addsipheaders && !strncasecmp(ast_var_name(current), "SIPADDHEADER", strlen("SIPADDHEADER"))) {
			/* Check whether there is a variable with a name starting with SIPADDHEADER */
			p->options->addsipheaders = 1;
		}

		
#ifdef OSP_SUPPORT
		else if (!p->options->osptoken && !strcasecmp(ast_var_name(current), "OSPTOKEN")) {
			p->options->osptoken = ast_var_value(current);
		} else if (!osphandle && !strcasecmp(ast_var_name(current), "OSPHANDLE")) {
			osphandle = ast_var_value(current);
		}
#endif
	}
	
	res = 0;
	ast_set_flag(p, SIP_OUTGOING);
#ifdef OSP_SUPPORT
	if (!p->options->osptoken || !osphandle || (sscanf(osphandle, "%d", &p->osphandle) != 1)) {
		/* Force Disable OSP support */
		if (option_debug)
			ast_log(LOG_DEBUG, "Disabling OSP support for this call. osptoken = %s, osphandle = %s\n", p->options->osptoken, osphandle);
		p->options->osptoken = NULL;
		osphandle = NULL;
		p->osphandle = -1;
	}
#endif
	ast_log(LOG_DEBUG, "Outgoing Call for %s\n", p->username);
	res = update_call_counter(p, INC_CALL_LIMIT);
	if ( res != -1 ) {
		p->callingpres = ast->cid.cid_pres;
		p->jointcapability = p->capability;
		transmit_invite(p, SIP_INVITE, 1, 2);
		if (p->maxtime) {
			/* Initialize auto-congest time */
			p->initid = ast_sched_add(sched, p->maxtime * 4, auto_congest, p);
		}
	}
	return res;
}

/*! \brief  sip_registry_destroy: Destroy registry object */
/*	Objects created with the register= statement in static configuration */
static void sip_registry_destroy(struct sip_registry *reg)
{
	/* Really delete */
	if (reg->call) {
		/* Clear registry before destroying to ensure
		   we don't get reentered trying to grab the registry lock */
		reg->call->registry = NULL;
		sip_destroy(reg->call);
	}
	if (reg->expire > -1)
		ast_sched_del(sched, reg->expire);
	if (reg->timeout > -1)
		ast_sched_del(sched, reg->timeout);
	ast_string_field_free_all(reg);
	regobjs--;
	free(reg);
	
}

/*! \brief   __sip_destroy: Execute destrucion of SIP dialog structure, release memory */
static void __sip_destroy(struct sip_pvt *p, int lockowner)
{
	struct sip_pvt *cur, *prev = NULL;
	struct sip_pkt *cp;

	if (sip_debug_test_pvt(p))
		ast_verbose("Destroying SIP dialog '%s' Method: %s\n", p->callid, sip_methods[p->method].text);

	if (dumphistory)
		sip_dump_history(p);

	if (p->options)
		free(p->options);

	if (p->stateid > -1)
		ast_extension_state_del(p->stateid, NULL);
	if (p->initid > -1)
		ast_sched_del(sched, p->initid);
	if (p->autokillid > -1)
		ast_sched_del(sched, p->autokillid);

	if (p->rtp) {
		ast_rtp_destroy(p->rtp);
	}
	if (p->vrtp) {
		ast_rtp_destroy(p->vrtp);
	}
	if (p->route) {
		free_old_route(p->route);
		p->route = NULL;
	}
	if (p->registry) {
		if (p->registry->call == p)
			p->registry->call = NULL;
		ASTOBJ_UNREF(p->registry, sip_registry_destroy);
	}

	/* Unlink us from the owner if we have one */
	if (p->owner) {
		if (lockowner)
			ast_mutex_lock(&p->owner->lock);
		if (option_debug)
			ast_log(LOG_DEBUG, "Detaching from %s\n", p->owner->name);
		p->owner->tech_pvt = NULL;
		if (lockowner)
			ast_mutex_unlock(&p->owner->lock);
	}
	/* Clear history */
	if (p->history) {
		while(!AST_LIST_EMPTY(p->history)) {
			struct sip_history *hist = AST_LIST_FIRST(p->history);
			AST_LIST_REMOVE_HEAD(p->history, list);
			free(hist);
		}
		free(p->history);
		p->history = NULL;
	}

	cur = iflist;
	while(cur) {
		if (cur == p) {
			if (prev)
				prev->next = cur->next;
			else
				iflist = cur->next;
			break;
		}
		prev = cur;
		cur = cur->next;
	}
	if (!cur) {
		ast_log(LOG_WARNING, "Trying to destroy \"%s\", not found in dialog list?!?! \n", p->callid);
		return;
	} 
	if (p->initid > -1)
		ast_sched_del(sched, p->initid);

	while((cp = p->packets)) {
		p->packets = p->packets->next;
		if (cp->retransid > -1) {
			ast_sched_del(sched, cp->retransid);
		}
		free(cp);
	}
	if (p->chanvars) {
		ast_variables_destroy(p->chanvars);
		p->chanvars = NULL;
	}
	ast_mutex_destroy(&p->lock);

	ast_string_field_free_all(p);

	free(p);
}

/*! \brief  update_call_counter: Handle call_limit for SIP users 
 * Setting a call-limit will cause calls above the limit not to be accepted.
 *
 * Remember that for a type=friend, there's one limit for the user and
 * another for the peer, not a combined call limit.
 * This will cause unexpected behaviour in subscriptions, since a "friend"
 * is *two* devices in Asterisk, not one.
 *
 * Thought: For realtime, we should propably update storage with inuse counter... 
 */
static int update_call_counter(struct sip_pvt *fup, int event)
{
	char name[256];
	int *inuse, *call_limit;
	int outgoing = ast_test_flag(fup, SIP_OUTGOING);
	struct sip_user *u = NULL;
	struct sip_peer *p = NULL;

	if (option_debug > 2)
		ast_log(LOG_DEBUG, "Updating call counter for %s call\n", outgoing ? "outgoing" : "incoming");
	/* Test if we need to check call limits, in order to avoid 
	   realtime lookups if we do not need it */
	if (!ast_test_flag(fup, SIP_CALL_LIMIT))
		return 0;

	ast_copy_string(name, fup->username, sizeof(name));

	/* Check the list of users */
	if (!outgoing)	/* Only check users for incoming calls */
		u = find_user(name, 1);

	if (u) {
		inuse = &u->inUse;
		call_limit = &u->call_limit;
		p = NULL;
	} else {
		/* Try to find peer */
		if (!p)
			p = find_peer(fup->peername, NULL, 1);
		if (p) {
			inuse = &p->inUse;
			call_limit = &p->call_limit;
			ast_copy_string(name, fup->peername, sizeof(name));
		} else {
			if (option_debug > 1)
				ast_log(LOG_DEBUG, "%s is not a local user, no call limit\n", name);
			return 0;
		}
	}
	switch(event) {
		/* incoming and outgoing affects the inUse counter */
		case DEC_CALL_LIMIT:
			if ( *inuse > 0 ) {
			         if (ast_test_flag(fup, SIP_INC_COUNT))
				         (*inuse)--;
			} else {
				*inuse = 0;
			}
			if (option_debug > 1 || sipdebug) {
				ast_log(LOG_DEBUG, "Call %s %s '%s' removed from call limit %d\n", outgoing ? "to" : "from", u ? "user":"peer", name, *call_limit);
			}
			break;
		case INC_CALL_LIMIT:
			if (*call_limit > 0 ) {
				if (*inuse >= *call_limit) {
					ast_log(LOG_ERROR, "Call %s %s '%s' rejected due to usage limit of %d\n", outgoing ? "to" : "from", u ? "user":"peer", name, *call_limit);
					if (u)
						ASTOBJ_UNREF(u, sip_destroy_user);
					else
						ASTOBJ_UNREF(p, sip_destroy_peer);
					return -1; 
				}
			}
			(*inuse)++;
	                ast_set_flag(fup, SIP_INC_COUNT);
			if (option_debug > 1 || sipdebug) {
				ast_log(LOG_DEBUG, "Call %s %s '%s' is %d out of %d\n", outgoing ? "to" : "from", u ? "user":"peer", name, *inuse, *call_limit);
			}
			break;
		default:
			ast_log(LOG_ERROR, "update_call_counter(%s, %d) called with no event!\n", name, event);
	}
	if (u)
		ASTOBJ_UNREF(u, sip_destroy_user);
	else
		ASTOBJ_UNREF(p, sip_destroy_peer);
	return 0;
}

/*! \brief  sip_destroy: Destroy SIP call structure */
static void sip_destroy(struct sip_pvt *p)
{
	ast_mutex_lock(&iflock);
	__sip_destroy(p, 1);
	ast_mutex_unlock(&iflock);
}


static int transmit_response_reliable(struct sip_pvt *p, char *msg, struct sip_request *req, int fatal);

/*! \brief  hangup_sip2cause: Convert SIP hangup causes to Asterisk hangup causes */
static int hangup_sip2cause(int cause)
{
/* Possible values taken from causes.h */

	switch(cause) {
		case 401:	/* Unauthorized */
			return AST_CAUSE_CALL_REJECTED;
		case 403:	/* Not found */
			return AST_CAUSE_CALL_REJECTED;
		case 404:	/* Not found */
			return AST_CAUSE_UNALLOCATED;
		case 405:	/* Method not allowed */
			return AST_CAUSE_INTERWORKING;
		case 407:	/* Proxy authentication required */
			return AST_CAUSE_CALL_REJECTED;
		case 408:	/* No reaction */
			return AST_CAUSE_NO_USER_RESPONSE;
		case 409:	/* Conflict */
			return AST_CAUSE_NORMAL_TEMPORARY_FAILURE;
		case 410:	/* Gone */
			return AST_CAUSE_UNALLOCATED;
		case 411:	/* Length required */
			return AST_CAUSE_INTERWORKING;
		case 413:	/* Request entity too large */
			return AST_CAUSE_INTERWORKING;
		case 414:	/* Request URI too large */
			return AST_CAUSE_INTERWORKING;
		case 415:	/* Unsupported media type */
			return AST_CAUSE_INTERWORKING;
		case 420:	/* Bad extension */
			return AST_CAUSE_NO_ROUTE_DESTINATION;
		case 480:	/* No answer */
			return AST_CAUSE_FAILURE;
		case 481:	/* No answer */
			return AST_CAUSE_INTERWORKING;
		case 482:	/* Loop detected */
			return AST_CAUSE_INTERWORKING;
		case 483:	/* Too many hops */
			return AST_CAUSE_NO_ANSWER;
		case 484:	/* Address incomplete */
			return AST_CAUSE_INVALID_NUMBER_FORMAT;
		case 485:	/* Ambigous */
			return AST_CAUSE_UNALLOCATED;
		case 486:	/* Busy everywhere */
			return AST_CAUSE_BUSY;
		case 487:	/* Request terminated */
			return AST_CAUSE_INTERWORKING;
		case 488:	/* No codecs approved */
			return AST_CAUSE_BEARERCAPABILITY_NOTAVAIL;
		case 491:	/* Request pending */
			return AST_CAUSE_INTERWORKING;
		case 493:	/* Undecipherable */
			return AST_CAUSE_INTERWORKING;
		case 500:	/* Server internal failure */
			return AST_CAUSE_FAILURE;
		case 501:	/* Call rejected */
			return AST_CAUSE_FACILITY_REJECTED;
		case 502:	
			return AST_CAUSE_DESTINATION_OUT_OF_ORDER;
		case 503:	/* Service unavailable */
			return AST_CAUSE_CONGESTION;
		case 504:	/* Gateway timeout */
			return AST_CAUSE_RECOVERY_ON_TIMER_EXPIRE;
		case 505:	/* SIP version not supported */
			return AST_CAUSE_INTERWORKING;
		case 600:	/* Busy everywhere */
			return AST_CAUSE_USER_BUSY;
		case 603:	/* Decline */
			return AST_CAUSE_CALL_REJECTED;
		case 604:	/* Does not exist anywhere */
			return AST_CAUSE_UNALLOCATED;
		case 606:	/* Not acceptable */
			return AST_CAUSE_BEARERCAPABILITY_NOTAVAIL;
		default:
			return AST_CAUSE_NORMAL;
	}
	/* Never reached */
	return 0;
}


/*! \brief  hangup_cause2sip: Convert Asterisk hangup causes to SIP codes 
\verbatim
 Possible values from causes.h
        AST_CAUSE_NOTDEFINED    AST_CAUSE_NORMAL        AST_CAUSE_BUSY
        AST_CAUSE_FAILURE       AST_CAUSE_CONGESTION    AST_CAUSE_UNALLOCATED

	In addition to these, a lot of PRI codes is defined in causes.h 
	...should we take care of them too ?
	
	Quote RFC 3398

   ISUP Cause value                        SIP response
   ----------------                        ------------
   1  unallocated number                   404 Not Found
   2  no route to network                  404 Not found
   3  no route to destination              404 Not found
   16 normal call clearing                 --- (*)
   17 user busy                            486 Busy here
   18 no user responding                   408 Request Timeout
   19 no answer from the user              480 Temporarily unavailable
   20 subscriber absent                    480 Temporarily unavailable
   21 call rejected                        403 Forbidden (+)
   22 number changed (w/o diagnostic)      410 Gone
   22 number changed (w/ diagnostic)       301 Moved Permanently
   23 redirection to new destination       410 Gone
   26 non-selected user clearing           404 Not Found (=)
   27 destination out of order             502 Bad Gateway
   28 address incomplete                   484 Address incomplete
   29 facility rejected                    501 Not implemented
   31 normal unspecified                   480 Temporarily unavailable
\endverbatim
*/
static char *hangup_cause2sip(int cause)
{
	switch(cause)
	{
		case AST_CAUSE_UNALLOCATED:		/* 1 */
		case AST_CAUSE_NO_ROUTE_DESTINATION:	/* 3 IAX2: Can't find extension in context */
		case AST_CAUSE_NO_ROUTE_TRANSIT_NET:	/* 2 */
			return "404 Not Found";
                case AST_CAUSE_CONGESTION:		/* 34 */
                case AST_CAUSE_SWITCH_CONGESTION:	/* 42 */
                        return "503 Service Unavailable";
		case AST_CAUSE_NO_USER_RESPONSE:	/* 18 */
			return "408 Request Timeout";
		case AST_CAUSE_NO_ANSWER:		/* 19 */
			return "480 Temporarily unavailable";
		case AST_CAUSE_CALL_REJECTED:		/* 21 */
			return "403 Forbidden";
		case AST_CAUSE_NUMBER_CHANGED:		/* 22 */
			return "410 Gone";
		case AST_CAUSE_NORMAL_UNSPECIFIED:	/* 31 */
			return "480 Temporarily unavailable";
		case AST_CAUSE_INVALID_NUMBER_FORMAT:
			return "484 Address incomplete";
		case AST_CAUSE_USER_BUSY:
			return "486 Busy here";
		case AST_CAUSE_FAILURE:
                	return "500 Server internal failure";
		case AST_CAUSE_FACILITY_REJECTED:	/* 29 */
			return "501 Not Implemented";
		case AST_CAUSE_CHAN_NOT_IMPLEMENTED:
			return "503 Service Unavailable";
		/* Used in chan_iax2 */
		case AST_CAUSE_DESTINATION_OUT_OF_ORDER:
			return "502 Bad Gateway";
		case AST_CAUSE_BEARERCAPABILITY_NOTAVAIL:	/* Can't find codec to connect to host */
			return "488 Not Acceptable Here";
			
		case AST_CAUSE_NOTDEFINED:
		default:
			ast_log(LOG_DEBUG, "AST hangup cause %d (no match found in SIP)\n", cause);
			return NULL;
	}

	/* Never reached */
	return 0;
}


/*! \brief  sip_hangup: Hangup SIP call
 * Part of PBX interface, called from ast_hangup */
static int sip_hangup(struct ast_channel *ast)
{
	struct sip_pvt *p = ast->tech_pvt;
	int needcancel = 0;
	struct ast_flags locflags = {0};

	if (!p) {
		ast_log(LOG_DEBUG, "Asked to hangup channel not connected\n");
		return 0;
	}
	if (option_debug && sipdebug)
		ast_log(LOG_DEBUG, "Hangup call %s, SIP callid %s)\n", ast->name, p->callid);

	ast_mutex_lock(&p->lock);
#ifdef OSP_SUPPORT
	if ((p->osphandle > -1) && (ast->_state == AST_STATE_UP)) {
		ast_osp_terminate(p->osphandle, AST_CAUSE_NORMAL, p->ospstart, time(NULL) - p->ospstart);
	}
#endif	
	if (option_debug && sipdebug)
		ast_log(LOG_DEBUG, "update_call_counter(%s) - decrement call limit counter on hangup\n", p->username);
	update_call_counter(p, DEC_CALL_LIMIT);
	/* Determine how to disconnect */
	if (p->owner != ast) {
		ast_log(LOG_WARNING, "Huh?  We aren't the owner? Can't hangup call.\n");
		ast_mutex_unlock(&p->lock);
		return 0;
	}
	/* If the call is not UP, we need to send CANCEL instead of BYE */
	if (ast->_state != AST_STATE_UP)
		needcancel = 1;

	/* Disconnect */
	p = ast->tech_pvt;
	if (p->vad) {
		ast_dsp_free(p->vad);
	}
	p->owner = NULL;
	ast->tech_pvt = NULL;

	ast_mutex_lock(&usecnt_lock);
	usecnt--;
	ast_mutex_unlock(&usecnt_lock);
	ast_update_use_count();

	ast_set_flag(&locflags, SIP_NEEDDESTROY);	

	/* Start the process if it's not already started */
	if (!ast_test_flag(p, SIP_ALREADYGONE) && !ast_strlen_zero(p->initreq.data)) {
		if (needcancel) {	/* Outgoing call, not up */
			if (ast_test_flag(p, SIP_OUTGOING)) {
				transmit_request_with_auth(p, SIP_CANCEL, p->ocseq, 1, 0);
				/* Actually don't destroy us yet, wait for the 487 on our original 
				   INVITE, but do set an autodestruct just in case we never get it. */
				ast_clear_flag(&locflags, SIP_NEEDDESTROY);
				sip_scheddestroy(p, 15000);
				/* stop retransmitting an INVITE that has not received a response */
				__sip_pretend_ack(p);
				if ( p->initid != -1 ) {
					/* channel still up - reverse dec of inUse counter
					   only if the channel is not auto-congested */
					update_call_counter(p, INC_CALL_LIMIT);
				}
			} else {	/* Incoming call, not up */
				char *res;
				if (ast->hangupcause && ((res = hangup_cause2sip(ast->hangupcause)))) {
					transmit_response_reliable(p, res, &p->initreq, 1);
				} else 
					transmit_response_reliable(p, "603 Declined", &p->initreq, 1);
			}
		} else {	/* Call is in UP state, send BYE */
			if (!p->pendinginvite) {
				/* Send a hangup */
				transmit_request_with_auth(p, SIP_BYE, 0, 1, 1);
			} else {
				/* Note we will need a BYE when this all settles out
				   but we can't send one while we have "INVITE" outstanding. */
				ast_set_flag(p, SIP_PENDINGBYE);	
				ast_clear_flag(p, SIP_NEEDREINVITE);	
			}
		}
	}
	ast_copy_flags(p, (&locflags), SIP_NEEDDESTROY);	
	ast_mutex_unlock(&p->lock);
	return 0;
}

/*! \brief  sip_answer: Answer SIP call , send 200 OK on Invite 
 * Part of PBX interface */
static int sip_answer(struct ast_channel *ast)
{
	int res = 0,fmt;
	const char *codec;
	struct sip_pvt *p = ast->tech_pvt;

	ast_mutex_lock(&p->lock);
	if (ast->_state != AST_STATE_UP) {
#ifdef OSP_SUPPORT	
		time(&p->ospstart);
#endif
	
		codec=pbx_builtin_getvar_helper(p->owner,"SIP_CODEC");
		if (codec) {
			fmt=ast_getformatbyname(codec);
			if (fmt) {
				ast_log(LOG_NOTICE, "Changing codec to '%s' for this call because of ${SIP_CODEC) variable\n",codec);
				if (p->jointcapability & fmt) {
					p->jointcapability &= fmt;
					p->capability &= fmt;
				} else
					ast_log(LOG_NOTICE, "Ignoring ${SIP_CODEC} variable because it is not shared by both ends.\n");
			} else ast_log(LOG_NOTICE, "Ignoring ${SIP_CODEC} variable because of unrecognized/not configured codec (check allow/disallow in sip.conf): %s\n",codec);
		}

		ast_setstate(ast, AST_STATE_UP);
		if (option_debug)
			ast_log(LOG_DEBUG, "sip_answer(%s)\n", ast->name);
		res = transmit_response_with_sdp(p, "200 OK", &p->initreq, 1);
	}
	ast_mutex_unlock(&p->lock);
	return res;
}

/*! \brief  sip_write: Send frame to media channel (rtp) */
static int sip_write(struct ast_channel *ast, struct ast_frame *frame)
{
	struct sip_pvt *p = ast->tech_pvt;
	int res = 0;
	switch (frame->frametype) {
	case AST_FRAME_VOICE:
		if (!(frame->subclass & ast->nativeformats)) {
			ast_log(LOG_WARNING, "Asked to transmit frame type %d, while native formats is %d (read/write = %d/%d)\n",
				frame->subclass, ast->nativeformats, ast->readformat, ast->writeformat);
			return 0;
		}
		if (p) {
			ast_mutex_lock(&p->lock);
			if (p->rtp) {
				/* If channel is not up, activate early media session */
				if ((ast->_state != AST_STATE_UP) && !ast_test_flag(p, SIP_PROGRESS_SENT) && !ast_test_flag(p, SIP_OUTGOING)) {
					transmit_response_with_sdp(p, "183 Session Progress", &p->initreq, 0);
					ast_set_flag(p, SIP_PROGRESS_SENT);	
				}
				time(&p->lastrtptx);
				res =  ast_rtp_write(p->rtp, frame);
			}
			ast_mutex_unlock(&p->lock);
		}
		break;
	case AST_FRAME_VIDEO:
		if (p) {
			ast_mutex_lock(&p->lock);
			if (p->vrtp) {
				/* Activate video early media */
				if ((ast->_state != AST_STATE_UP) && !ast_test_flag(p, SIP_PROGRESS_SENT) && !ast_test_flag(p, SIP_OUTGOING)) {
					transmit_response_with_sdp(p, "183 Session Progress", &p->initreq, 0);
					ast_set_flag(p, SIP_PROGRESS_SENT);	
				}
				time(&p->lastrtptx);
				res =  ast_rtp_write(p->vrtp, frame);
			}
			ast_mutex_unlock(&p->lock);
		}
		break;
	case AST_FRAME_IMAGE:
		return 0;
		break;
	default: 
		ast_log(LOG_WARNING, "Can't send %d type frames with SIP write\n", frame->frametype);
		return 0;
	}

	return res;
}

/*! \brief  sip_fixup: Fix up a channel:  If a channel is consumed, this is called.
        Basically update any ->owner links */
static int sip_fixup(struct ast_channel *oldchan, struct ast_channel *newchan)
{
	struct sip_pvt *p = newchan->tech_pvt;
	ast_mutex_lock(&p->lock);
	if (p->owner != oldchan) {
		ast_log(LOG_WARNING, "old channel wasn't %p but was %p\n", oldchan, p->owner);
		ast_mutex_unlock(&p->lock);
		return -1;
	}
	p->owner = newchan;
	ast_mutex_unlock(&p->lock);
	return 0;
}

/*! \brief  sip_senddigit: Send DTMF character on SIP channel */
/*    within one call, we're able to transmit in many methods simultaneously */
static int sip_senddigit(struct ast_channel *ast, char digit)
{
	struct sip_pvt *p = ast->tech_pvt;
	int res = 0;
	ast_mutex_lock(&p->lock);
	switch (ast_test_flag(p, SIP_DTMF)) {
	case SIP_DTMF_INFO:
		transmit_info_with_digit(p, digit);
		break;
	case SIP_DTMF_RFC2833:
		if (p->rtp)
			ast_rtp_senddigit(p->rtp, digit);
		break;
	case SIP_DTMF_INBAND:
		res = -1;
		break;
	}
	ast_mutex_unlock(&p->lock);
	return res;
}



/*! \brief  sip_transfer: Transfer SIP call */
static int sip_transfer(struct ast_channel *ast, const char *dest)
{
	struct sip_pvt *p = ast->tech_pvt;
	int res;

	ast_mutex_lock(&p->lock);
	if (ast->_state == AST_STATE_RING)
		res = sip_sipredirect(p, dest);
	else
		res = transmit_refer(p, dest);
	ast_mutex_unlock(&p->lock);
	return res;
}

/*! \brief  sip_indicate: Play indication to user 
 * With SIP a lot of indications is sent as messages, letting the device play
   the indication - busy signal, congestion etc */
static int sip_indicate(struct ast_channel *ast, int condition)
{
	struct sip_pvt *p = ast->tech_pvt;
	int res = 0;

	ast_mutex_lock(&p->lock);
	switch(condition) {
	case AST_CONTROL_RINGING:
		if (ast->_state == AST_STATE_RING) {
			if (!ast_test_flag(p, SIP_PROGRESS_SENT) ||
			    (ast_test_flag(p, SIP_PROG_INBAND) == SIP_PROG_INBAND_NEVER)) {				
				/* Send 180 ringing if out-of-band seems reasonable */
				transmit_response(p, "180 Ringing", &p->initreq);
				ast_set_flag(p, SIP_RINGING);
				if (ast_test_flag(p, SIP_PROG_INBAND) != SIP_PROG_INBAND_YES)
					break;
			} else {
				/* Well, if it's not reasonable, just send in-band */
			}
		}
		res = -1;
		break;
	case AST_CONTROL_BUSY:
		if (ast->_state != AST_STATE_UP) {
			transmit_response(p, "486 Busy Here", &p->initreq);
			ast_set_flag(p, SIP_ALREADYGONE);	
			ast_softhangup_nolock(ast, AST_SOFTHANGUP_DEV);
			break;
		}
		res = -1;
		break;
	case AST_CONTROL_CONGESTION:
		if (ast->_state != AST_STATE_UP) {
			transmit_response(p, "503 Service Unavailable", &p->initreq);
			ast_set_flag(p, SIP_ALREADYGONE);	
			ast_softhangup_nolock(ast, AST_SOFTHANGUP_DEV);
			break;
		}
		res = -1;
		break;
	case AST_CONTROL_PROCEEDING:
		if ((ast->_state != AST_STATE_UP) && !ast_test_flag(p, SIP_PROGRESS_SENT) && !ast_test_flag(p, SIP_OUTGOING)) {
			transmit_response(p, "100 Trying", &p->initreq);
			break;
		}
		res = -1;
		break;
	case AST_CONTROL_PROGRESS:
		if ((ast->_state != AST_STATE_UP) && !ast_test_flag(p, SIP_PROGRESS_SENT) && !ast_test_flag(p, SIP_OUTGOING)) {
			transmit_response_with_sdp(p, "183 Session Progress", &p->initreq, 0);
			ast_set_flag(p, SIP_PROGRESS_SENT);	
			break;
		}
		res = -1;
		break;
	case AST_CONTROL_HOLD:	/* The other part of the bridge are put on hold */
		if (sipdebug)
			ast_log(LOG_DEBUG, "Bridged channel now on hold - %s\n", p->callid);
		res = -1;
		break;
	case AST_CONTROL_UNHOLD:	/* The other part of the bridge are back from hold */
		if (sipdebug)
			ast_log(LOG_DEBUG, "Bridged channel is back from hold, let's talk! : %s\n", p->callid);
		res = -1;
		break;
	case AST_CONTROL_VIDUPDATE:	/* Request a video frame update */
		if (p->vrtp && !ast_test_flag(p, SIP_NOVIDEO)) {
			transmit_info_with_vidupdate(p);
			res = 0;
		} else
			res = -1;
		break;
	case -1:
		res = -1;
		break;
	default:
		ast_log(LOG_WARNING, "Don't know how to indicate condition %d\n", condition);
		res = -1;
		break;
	}
	ast_mutex_unlock(&p->lock);
	return res;
}



/*! \brief  sip_new: Initiate a call in the SIP channel */
/*      called from sip_request_call (calls from the pbx ) */
static struct ast_channel *sip_new(struct sip_pvt *i, int state, const char *title)
{
	struct ast_channel *tmp;
	struct ast_variable *v = NULL;
	int fmt;
	int what;
#ifdef OSP_SUPPORT
	char iabuf[INET_ADDRSTRLEN];
	char peer[MAXHOSTNAMELEN];
#endif	
	
	ast_mutex_unlock(&i->lock);
	/* Don't hold a sip pvt lock while we allocate a channel */
	tmp = ast_channel_alloc(1);
	ast_mutex_lock(&i->lock);
	if (!tmp) {
		ast_log(LOG_WARNING, "Unable to allocate SIP channel structure\n");
		return NULL;
	}
	tmp->tech = &sip_tech;
	/* Select our native format based on codec preference until we receive
	   something from another device to the contrary. */
	if (i->jointcapability)
		what = i->jointcapability;
	else if (i->capability)
		what = i->capability;
	else
		what = global_capability;
	tmp->nativeformats = ast_codec_choose(&i->prefs, what, 1) | (i->jointcapability & AST_FORMAT_VIDEO_MASK);
	fmt = ast_best_codec(tmp->nativeformats);

	if (title)
		snprintf(tmp->name, sizeof(tmp->name), "SIP/%s-%04x", title, thread_safe_rand() & 0xffff);
	else if (strchr(i->fromdomain,':'))
		snprintf(tmp->name, sizeof(tmp->name), "SIP/%s-%08x", strchr(i->fromdomain,':')+1, (int)(long)(i));
	else
		snprintf(tmp->name, sizeof(tmp->name), "SIP/%s-%08x", i->fromdomain, (int)(long)(i));

	tmp->type = channeltype;
	if (ast_test_flag(i, SIP_DTMF) ==  SIP_DTMF_INBAND) {
		i->vad = ast_dsp_new();
		ast_dsp_set_features(i->vad, DSP_FEATURE_DTMF_DETECT);
		if (relaxdtmf)
			ast_dsp_digitmode(i->vad, DSP_DIGITMODE_DTMF | DSP_DIGITMODE_RELAXDTMF);
	}
	if (i->rtp) {
		tmp->fds[0] = ast_rtp_fd(i->rtp);
		tmp->fds[1] = ast_rtcp_fd(i->rtp);
	}
	if (i->vrtp) {
		tmp->fds[2] = ast_rtp_fd(i->vrtp);
		tmp->fds[3] = ast_rtcp_fd(i->vrtp);
	}
	if (state == AST_STATE_RING)
		tmp->rings = 1;
	tmp->adsicpe = AST_ADSI_UNAVAILABLE;
	tmp->writeformat = fmt;
	tmp->rawwriteformat = fmt;
	tmp->readformat = fmt;
	tmp->rawreadformat = fmt;
	tmp->tech_pvt = i;

	tmp->callgroup = i->callgroup;
	tmp->pickupgroup = i->pickupgroup;
	tmp->cid.cid_pres = i->callingpres;
	if (!ast_strlen_zero(i->accountcode))
		ast_copy_string(tmp->accountcode, i->accountcode, sizeof(tmp->accountcode));
	if (i->amaflags)
		tmp->amaflags = i->amaflags;
	if (!ast_strlen_zero(i->language))
		ast_copy_string(tmp->language, i->language, sizeof(tmp->language));
	if (!ast_strlen_zero(i->musicclass))
		ast_copy_string(tmp->musicclass, i->musicclass, sizeof(tmp->musicclass));
	i->owner = tmp;
	ast_mutex_lock(&usecnt_lock);
	usecnt++;
	ast_mutex_unlock(&usecnt_lock);
	ast_copy_string(tmp->context, i->context, sizeof(tmp->context));
	ast_copy_string(tmp->exten, i->exten, sizeof(tmp->exten));
	if (!ast_strlen_zero(i->cid_num)) 
		tmp->cid.cid_num = ast_strdup(i->cid_num);
	if (!ast_strlen_zero(i->cid_name))
		tmp->cid.cid_name = ast_strdup(i->cid_name);
	if (!ast_strlen_zero(i->rdnis))
		tmp->cid.cid_rdnis = ast_strdup(i->rdnis);
	if (!ast_strlen_zero(i->exten) && strcmp(i->exten, "s"))
		tmp->cid.cid_dnid = ast_strdup(i->exten);
	tmp->priority = 1;
	if (!ast_strlen_zero(i->uri)) {
		pbx_builtin_setvar_helper(tmp, "SIPURI", i->uri);
	}
	if (!ast_strlen_zero(i->domain)) {
		pbx_builtin_setvar_helper(tmp, "SIPDOMAIN", i->domain);
	}
	if (!ast_strlen_zero(i->useragent)) {
		pbx_builtin_setvar_helper(tmp, "SIPUSERAGENT", i->useragent);
	}
	if (!ast_strlen_zero(i->callid)) {
		pbx_builtin_setvar_helper(tmp, "SIPCALLID", i->callid);
	}
#ifdef OSP_SUPPORT
	snprintf(peer, sizeof(peer), "[%s]:%d", ast_inet_ntoa(iabuf, sizeof(iabuf), i->sa.sin_addr), ntohs(i->sa.sin_port));
	pbx_builtin_setvar_helper(tmp, "OSPPEER", peer);
#endif
	ast_setstate(tmp, state);
	if (state != AST_STATE_DOWN) {
		if (ast_pbx_start(tmp)) {
			ast_log(LOG_WARNING, "Unable to start PBX on %s\n", tmp->name);
			ast_hangup(tmp);
			tmp = NULL;
		}
	}
	/* Set channel variables for this call from configuration */
	for (v = i->chanvars ; v ; v = v->next)
		pbx_builtin_setvar_helper(tmp,v->name,v->value);
				
	return tmp;
}

/*! \brief  get_sdp_by_line: Reads one line of SIP message body */
static char* get_sdp_by_line(char* line, char *name, int nameLen)
{
	if (strncasecmp(line, name, nameLen) == 0 && line[nameLen] == '=') {
		return ast_skip_blanks(line + nameLen + 1);
	}
	return "";
}

/*! \brief  get_sdp: Gets all kind of SIP message bodies, including SDP,
   but the name wrongly applies _only_ sdp */
static char *get_sdp(struct sip_request *req, char *name) 
{
	int x;
	int len = strlen(name);
	char *r;

	for (x=0; x<req->lines; x++) {
		r = get_sdp_by_line(req->line[x], name, len);
		if (r[0] != '\0')
			return r;
	}
	return "";
}


static void sdpLineNum_iterator_init(int* iterator) 
{
	*iterator = 0;
}

static char* get_sdp_iterate(int* iterator,
			     struct sip_request *req, char *name)
{
	int len = strlen(name);
	char *r;

	while (*iterator < req->lines) {
		r = get_sdp_by_line(req->line[(*iterator)++], name, len);
		if (r[0] != '\0')
			return r;
	}
	return "";
}

static char *find_alias(const char *name, char *_default)
{
	int x;
	for (x=0;x<sizeof(aliases) / sizeof(aliases[0]); x++) 
		if (!strcasecmp(aliases[x].fullname, name))
			return aliases[x].shortname;
	return _default;
}

static char *__get_header(struct sip_request *req, char *name, int *start)
{
	int pass;

	/*
	 * Technically you can place arbitrary whitespace both before and after the ':' in
	 * a header, although RFC3261 clearly says you shouldn't before, and place just
	 * one afterwards.  If you shouldn't do it, what absolute idiot decided it was 
	 * a good idea to say you can do it, and if you can do it, why in the hell would.
	 * you say you shouldn't.
	 * Anyways, pedanticsipchecking controls whether we allow spaces before ':',
	 * and we always allow spaces after that for compatibility.
	 */
	for (pass = 0; name && pass < 2;pass++) {
		int x, len = strlen(name);
		for (x=*start; x<req->headers; x++) {
			if (!strncasecmp(req->header[x], name, len)) {
				char *r = req->header[x] + len;	/* skip name */
				if (pedanticsipchecking)
					r = ast_skip_blanks(r);

				if (*r == ':') {
					*start = x+1;
					return ast_skip_blanks(r+1);
				}
			}
		}
		if (pass == 0) /* Try aliases */
			name = find_alias(name, NULL);
	}

	/* Don't return NULL, so get_header is always a valid pointer */
	return "";
}

/*! \brief  get_header: Get header from SIP request */
static char *get_header(struct sip_request *req, char *name)
{
	int start = 0;
	return __get_header(req, name, &start);
}

/*! \brief  sip_rtp_read: Read RTP from network */
static struct ast_frame *sip_rtp_read(struct ast_channel *ast, struct sip_pvt *p)
{
	/* Retrieve audio/etc from channel.  Assumes p->lock is already held. */
	struct ast_frame *f;
	static struct ast_frame null_frame = { AST_FRAME_NULL, };
	
	if (!p->rtp) {
		/* We have no RTP allocated for this channel */
		return &null_frame;
	}

	switch(ast->fdno) {
	case 0:
		f = ast_rtp_read(p->rtp);	/* RTP Audio */
		break;
	case 1:
		f = ast_rtcp_read(p->rtp);	/* RTCP Control Channel */
		break;
	case 2:
		f = ast_rtp_read(p->vrtp);	/* RTP Video */
		break;
	case 3:
		f = ast_rtcp_read(p->vrtp);	/* RTCP Control Channel for video */
		break;
	default:
		f = &null_frame;
	}
	/* Don't forward RFC2833 if we're not supposed to */
	if (f && (f->frametype == AST_FRAME_DTMF) && (ast_test_flag(p, SIP_DTMF) != SIP_DTMF_RFC2833))
		return &null_frame;
	if (p->owner) {
		/* We already hold the channel lock */
		if (f->frametype == AST_FRAME_VOICE) {
			if (f->subclass != (p->owner->nativeformats & AST_FORMAT_AUDIO_MASK)) {
				ast_log(LOG_DEBUG, "Oooh, format changed to %d\n", f->subclass);
				p->owner->nativeformats = (p->owner->nativeformats & AST_FORMAT_VIDEO_MASK) | f->subclass;
				ast_set_read_format(p->owner, p->owner->readformat);
				ast_set_write_format(p->owner, p->owner->writeformat);
			}
			if ((ast_test_flag(p, SIP_DTMF) == SIP_DTMF_INBAND) && p->vad) {
				f = ast_dsp_process(p->owner, p->vad, f);
				if (f && (f->frametype == AST_FRAME_DTMF)) 
					ast_log(LOG_DEBUG, "* Detected inband DTMF '%c'\n", f->subclass);
			}
		}
	}
	return f;
}

/*! \brief  sip_read: Read SIP RTP from channel */
static struct ast_frame *sip_read(struct ast_channel *ast)
{
	struct ast_frame *fr;
	struct sip_pvt *p = ast->tech_pvt;
	ast_mutex_lock(&p->lock);
	fr = sip_rtp_read(ast, p);
	time(&p->lastrtprx);
	ast_mutex_unlock(&p->lock);
	return fr;
}

/*! \brief  build_callid_pvt: Build SIP Call-ID value for a non-REGISTER transaction */
static void build_callid_pvt(struct sip_pvt *pvt)
{
	int val[4];
	int x;
	char iabuf[INET_ADDRSTRLEN];

	for (x=0; x<4; x++)
		val[x] = thread_safe_rand();

	if (ast_strlen_zero(pvt->fromdomain))
		/* It's not important that we really use our right IP here... */
		ast_string_field_build(pvt, callid, "%08x%08x%08x%08x@%s",
				       val[0], val[1], val[2], val[3],
				       ast_inet_ntoa(iabuf, sizeof(iabuf), pvt->ourip));
	else
		ast_string_field_build(pvt, callid, "%08x%08x%08x%08x@%s",
				       val[0], val[1], val[2], val[3],
				       pvt->fromdomain);
}

/*! \brief  build_callid_registry: Build SIP Call-ID value for a REGISTER transaction */
static void build_callid_registry(struct sip_registry *reg, struct in_addr ourip, const char *fromdomain)
{
	int val[4];
	int x;
	char iabuf[INET_ADDRSTRLEN];

	for (x=0; x<4; x++)
		val[x] = thread_safe_rand();

	if (ast_strlen_zero(fromdomain))
		/* It's not important that we really use our right IP here... */
		ast_string_field_build(reg, callid, "%08x%08x%08x%08x@%s",
				       val[0], val[1], val[2], val[3],
				       ast_inet_ntoa(iabuf, sizeof(iabuf), ourip));
	else
		ast_string_field_build(reg, callid, "%08x%08x%08x%08x@%s",
				       val[0], val[1], val[2], val[3],
				       fromdomain);
}

static void make_our_tag(char *tagbuf, size_t len)
{
	snprintf(tagbuf, len, "as%08x", thread_safe_rand());
}

/*! \brief  sip_alloc: Allocate SIP_PVT structure and set defaults */
static struct sip_pvt *sip_alloc(ast_string_field callid, struct sockaddr_in *sin,
				 int useglobal_nat, const int intended_method)
{
	struct sip_pvt *p;

	if (!(p = ast_calloc(1, sizeof(*p))))
		return NULL;

	if (ast_string_field_init(p)) {
		free(p);
		return NULL;
	}

	ast_mutex_init(&p->lock);

	p->method = intended_method;
	p->initid = -1;
	p->autokillid = -1;
	p->subscribed = NONE;
	p->stateid = -1;
	p->prefs = prefs;
	if (intended_method != SIP_OPTIONS)	/* Peerpoke has it's own system */
		p->timer_t1 = 500;	/* Default SIP retransmission timer T1 (RFC 3261) */
#ifdef OSP_SUPPORT
	p->osphandle = -1;
	p->osptimelimit = 0;
#endif	
	if (sin) {
		memcpy(&p->sa, sin, sizeof(p->sa));
		if (ast_sip_ouraddrfor(&p->sa.sin_addr,&p->ourip))
			memcpy(&p->ourip, &__ourip, sizeof(p->ourip));
	} else {
		memcpy(&p->ourip, &__ourip, sizeof(p->ourip));
	}

	p->branch = thread_safe_rand();	
	make_our_tag(p->tag, sizeof(p->tag));
	/* Start with 101 instead of 1 */
	p->ocseq = 101;

	if (sip_methods[intended_method].need_rtp) {
		p->rtp = ast_rtp_new_with_bindaddr(sched, io, 1, 0, bindaddr.sin_addr);
		if (global_videosupport)
			p->vrtp = ast_rtp_new_with_bindaddr(sched, io, 1, 0, bindaddr.sin_addr);
		if (!p->rtp || (global_videosupport && !p->vrtp)) {
			ast_log(LOG_WARNING, "Unable to create RTP audio %s session: %s\n", global_videosupport ? "and video" : "", strerror(errno));
			ast_mutex_destroy(&p->lock);
			if (p->chanvars) {
				ast_variables_destroy(p->chanvars);
				p->chanvars = NULL;
			}
			free(p);
			return NULL;
		}
		ast_rtp_settos(p->rtp, global_tos);
		if (p->vrtp)
			ast_rtp_settos(p->vrtp, global_tos);
		p->rtptimeout = global_rtptimeout;
		p->rtpholdtimeout = global_rtpholdtimeout;
		p->rtpkeepalive = global_rtpkeepalive;
	}

	if (useglobal_nat && sin) {
		/* Setup NAT structure according to global settings if we have an address */
		ast_copy_flags(p, &global_flags, SIP_NAT);
		memcpy(&p->recv, sin, sizeof(p->recv));
		if (p->rtp)
			ast_rtp_setnat(p->rtp, (ast_test_flag(p, SIP_NAT) & SIP_NAT_ROUTE));
		if (p->vrtp)
			ast_rtp_setnat(p->vrtp, (ast_test_flag(p, SIP_NAT) & SIP_NAT_ROUTE));
	}

	if (p->method != SIP_REGISTER)
		ast_string_field_set(p, fromdomain, default_fromdomain);
	build_via(p);
	if (!callid)
		build_callid_pvt(p);
	else
		ast_string_field_set(p, callid, callid);
	ast_copy_flags(p, &global_flags, SIP_FLAGS_TO_COPY);
	/* Assign default music on hold class */
	ast_string_field_set(p, musicclass, default_musicclass);
	p->capability = global_capability;
	if ((ast_test_flag(p, SIP_DTMF) == SIP_DTMF_RFC2833) || (ast_test_flag(p, SIP_DTMF) == SIP_DTMF_AUTO))
		p->noncodeccapability |= AST_RTP_DTMF;
	ast_string_field_set(p, context, default_context);

	/* Add to active dialog list */
	ast_mutex_lock(&iflock);
	p->next = iflist;
	iflist = p;
	ast_mutex_unlock(&iflock);
	if (option_debug)
		ast_log(LOG_DEBUG, "Allocating new SIP dialog for %s - %s (%s)\n", callid ? callid : "(No Call-ID)", sip_methods[intended_method].text, p->rtp ? "With RTP" : "No RTP");
	return p;
}

/*! \brief  find_call: Connect incoming SIP message to current dialog or create new dialog structure */
/*               Called by handle_request, sipsock_read */
static struct sip_pvt *find_call(struct sip_request *req, struct sockaddr_in *sin, const int intended_method)
{
	struct sip_pvt *p;
	char *callid;
	char *tag = "";
	char totag[128];
	char fromtag[128];

	callid = get_header(req, "Call-ID");

	if (pedanticsipchecking) {
		/* In principle Call-ID's uniquely identify a call, but with a forking SIP proxy
		   we need more to identify a branch - so we have to check branch, from
		   and to tags to identify a call leg.
		   For Asterisk to behave correctly, you need to turn on pedanticsipchecking
		   in sip.conf
		   */
		if (gettag(req, "To", totag, sizeof(totag)))
			ast_set_flag(req, SIP_PKT_WITH_TOTAG);	/* Used in handle_request/response */
		gettag(req, "From", fromtag, sizeof(fromtag));

		if (req->method == SIP_RESPONSE)
			tag = totag;
		else
			tag = fromtag;
			

		if (option_debug > 4 )
			ast_log(LOG_DEBUG, "= Looking for  Call ID: %s (Checking %s) --From tag %s --To-tag %s  \n", callid, req->method==SIP_RESPONSE ? "To" : "From", fromtag, totag);
	}

	ast_mutex_lock(&iflock);
	p = iflist;
	while(p) {	/* In pedantic, we do not want packets with bad syntax to be connected to a PVT */
		int found = 0;
		if (req->method == SIP_REGISTER)
			found = (!strcmp(p->callid, callid));
		else 
			found = (!strcmp(p->callid, callid) && 
			(!pedanticsipchecking || !tag || ast_strlen_zero(p->theirtag) || !strcmp(p->theirtag, tag))) ;

		if (option_debug > 4)
			ast_log(LOG_DEBUG, "= %s Their Call ID: %s Their Tag %s Our tag: %s\n", found ? "Found" : "No match", p->callid, p->theirtag, p->tag);

		/* If we get a new request within an existing to-tag - check the to tag as well */
		if (pedanticsipchecking && found  && req->method != SIP_RESPONSE) {	/* SIP Request */
			if (p->tag[0] == '\0' && totag[0]) {
				/* We have no to tag, but they have. Wrong dialog */
				found = 0;
			} else if (totag[0]) {			/* Both have tags, compare them */
				if (strcmp(totag, p->tag)) {
					found = 0;		/* This is not our packet */
				}
			}
			if (!found && option_debug > 4)
				ast_log(LOG_DEBUG, "= Being pedantic: This is not our match on request: Call ID: %s Ourtag <null> Totag %s Method %s\n", p->callid, totag, sip_methods[req->method].text);
		}


		if (found) {
			/* Found the call */
			ast_mutex_lock(&p->lock);
			ast_mutex_unlock(&iflock);
			return p;
		}
		p = p->next;
	}
	ast_mutex_unlock(&iflock);
	p = sip_alloc(callid, sin, 1, intended_method);
	if (p)
		ast_mutex_lock(&p->lock);
	return p;
}

/*! \brief  sip_register: Parse register=> line in sip.conf and add to registry */
static int sip_register(char *value, int lineno)
{
	struct sip_registry *reg;
	char copy[256];
	char *username=NULL, *hostname=NULL, *secret=NULL, *authuser=NULL;
	char *porta=NULL;
	char *contact=NULL;
	char *stringp=NULL;
	
	if (!value)
		return -1;
	ast_copy_string(copy, value, sizeof(copy));
	stringp=copy;
	username = stringp;
	hostname = strrchr(stringp, '@');
	if (hostname) {
		*hostname = '\0';
		hostname++;
	}
	if (ast_strlen_zero(username) || ast_strlen_zero(hostname)) {
		ast_log(LOG_WARNING, "Format for registration is user[:secret[:authuser]]@host[:port][/contact] at line %d\n", lineno);
		return -1;
	}
	stringp=username;
	username = strsep(&stringp, ":");
	if (username) {
		secret = strsep(&stringp, ":");
		if (secret) 
			authuser = strsep(&stringp, ":");
	}
	stringp = hostname;
	hostname = strsep(&stringp, "/");
	if (hostname) 
		contact = strsep(&stringp, "/");
	if (ast_strlen_zero(contact))
		contact = "s";
	stringp=hostname;
	hostname = strsep(&stringp, ":");
	porta = strsep(&stringp, ":");
	
	if (porta && !atoi(porta)) {
		ast_log(LOG_WARNING, "%s is not a valid port number at line %d\n", porta, lineno);
		return -1;
	}
	if (!(reg = ast_calloc(1, sizeof(*reg))))
		return -1;

	if (ast_string_field_init(reg)) {
		ast_log(LOG_ERROR, "Out of memory. Can't allocate SIP registry entry\n");
		free(reg);
		return -1;
	}

	regobjs++;
	ASTOBJ_INIT(reg);
	ast_string_field_set(reg, contact, contact);
	if (username)
		ast_string_field_set(reg, username, username);
	if (hostname)
		ast_string_field_set(reg, hostname, hostname);
	if (authuser)
		ast_string_field_set(reg, authuser, authuser);
	if (secret)
		ast_string_field_set(reg, secret, secret);
	reg->expire = -1;
	reg->timeout =  -1;
	reg->refresh = default_expiry;
	reg->portno = porta ? atoi(porta) : 0;
	reg->callid_valid = 0;
	reg->ocseq = 101;
	ASTOBJ_CONTAINER_LINK(&regl, reg);
	ASTOBJ_UNREF(reg,sip_registry_destroy);
	return 0;
}

/*! \brief  lws2sws: Parse multiline SIP headers into one header */
/* This is enabled if pedanticsipchecking is enabled */
static int lws2sws(char *msgbuf, int len) 
{ 
	int h = 0, t = 0; 
	int lws = 0; 

	for (; h < len;) { 
		/* Eliminate all CRs */ 
		if (msgbuf[h] == '\r') { 
			h++; 
			continue; 
		} 
		/* Check for end-of-line */ 
		if (msgbuf[h] == '\n') { 
			/* Check for end-of-message */ 
			if (h + 1 == len) 
				break; 
			/* Check for a continuation line */ 
			if (msgbuf[h + 1] == ' ' || msgbuf[h + 1] == '\t') { 
				/* Merge continuation line */ 
				h++; 
				continue; 
			} 
			/* Propagate LF and start new line */ 
			msgbuf[t++] = msgbuf[h++]; 
			lws = 0;
			continue; 
		} 
		if (msgbuf[h] == ' ' || msgbuf[h] == '\t') { 
			if (lws) { 
				h++; 
				continue; 
			} 
			msgbuf[t++] = msgbuf[h++]; 
			lws = 1; 
			continue; 
		} 
		msgbuf[t++] = msgbuf[h++]; 
		if (lws) 
			lws = 0; 
	} 
	msgbuf[t] = '\0'; 
	return t; 
}

/*! \brief  parse_request: Parse a SIP message */
static void parse_request(struct sip_request *req)
{
	/* Divide fields by NULL's */
	char *c;
	int f = 0;

	c = req->data;

	/* First header starts immediately */
	req->header[f] = c;
	while(*c) {
		if (*c == '\n') {
			/* We've got a new header */
			*c = 0;

			if (sipdebug && option_debug > 3)
				ast_log(LOG_DEBUG, "Header %d: %s (%d)\n", f, req->header[f], (int) strlen(req->header[f]));
			if (ast_strlen_zero(req->header[f])) {
				/* Line by itself means we're now in content */
				c++;
				break;
			}
			if (f >= SIP_MAX_HEADERS - 1) {
				ast_log(LOG_WARNING, "Too many SIP headers. Ignoring.\n");
			} else
				f++;
			req->header[f] = c + 1;
		} else if (*c == '\r') {
			/* Ignore but eliminate \r's */
			*c = 0;
		}
		c++;
	}
	/* Check for last header */
	if (!ast_strlen_zero(req->header[f])) {
		if (sipdebug && option_debug > 3)
			ast_log(LOG_DEBUG, "Header %d: %s (%d)\n", f, req->header[f], (int) strlen(req->header[f]));
		f++;
	}
	req->headers = f;
	/* Now we process any mime content */
	f = 0;
	req->line[f] = c;
	while(*c) {
		if (*c == '\n') {
			/* We've got a new line */
			*c = 0;
			if (sipdebug && option_debug > 3)
				ast_log(LOG_DEBUG, "Line: %s (%d)\n", req->line[f], (int) strlen(req->line[f]));
			if (f >= SIP_MAX_LINES - 1) {
				ast_log(LOG_WARNING, "Too many SDP lines. Ignoring.\n");
			} else
				f++;
			req->line[f] = c + 1;
		} else if (*c == '\r') {
			/* Ignore and eliminate \r's */
			*c = 0;
		}
		c++;
	}
	/* Check for last line */
	if (!ast_strlen_zero(req->line[f])) 
		f++;
	req->lines = f;
	if (*c) 
		ast_log(LOG_WARNING, "Odd content, extra stuff left over ('%s')\n", c);
	/* Split up the first line parts */
	determine_firstline_parts(req);
}

/*! \brief  process_sdp: Process SIP SDP and activate RTP channels*/
static int process_sdp(struct sip_pvt *p, struct sip_request *req)
{
	char *m;
	char *c;
	char *a;
	char host[258];
	char iabuf[INET_ADDRSTRLEN];
	int len = -1;
	int portno = -1;
	int vportno = -1;
	int peercapability, peernoncodeccapability;
	int vpeercapability=0, vpeernoncodeccapability=0;
	struct sockaddr_in sin;
	char *codecs;
	struct hostent *hp;
	struct ast_hostent ahp;
	int codec;
	int destiterator = 0;
	int iterator;
	int sendonly = 0;
	int x,y;
	int debug=sip_debug_test_pvt(p);
	struct ast_channel *bridgepeer = NULL;

	if (!p->rtp) {
		ast_log(LOG_ERROR, "Got SDP but have no RTP session allocated.\n");
		return -1;
	}

	/* Update our last rtprx when we receive an SDP, too */
	time(&p->lastrtprx);
	time(&p->lastrtptx);

	/* Get codec and RTP info from SDP */
	if (strcasecmp(get_header(req, "Content-Type"), "application/sdp")) {
		ast_log(LOG_NOTICE, "Content is '%s', not 'application/sdp'\n", get_header(req, "Content-Type"));
		return -1;
	}
	m = get_sdp(req, "m");
	sdpLineNum_iterator_init(&destiterator);
	c = get_sdp_iterate(&destiterator, req, "c");
	if (ast_strlen_zero(m) || ast_strlen_zero(c)) {
		ast_log(LOG_WARNING, "Insufficient information for SDP (m = '%s', c = '%s')\n", m, c);
		return -1;
	}
	if (sscanf(c, "IN IP4 %256s", host) != 1) {
		ast_log(LOG_WARNING, "Invalid host in c= line, '%s'\n", c);
		return -1;
	}
	/* XXX This could block for a long time, and block the main thread! XXX */
	hp = ast_gethostbyname(host, &ahp);
	if (!hp) {
		ast_log(LOG_WARNING, "Unable to lookup host in c= line, '%s'\n", c);
		return -1;
	}
	sdpLineNum_iterator_init(&iterator);
	ast_set_flag(p, SIP_NOVIDEO);	
	while ((m = get_sdp_iterate(&iterator, req, "m"))[0] != '\0') {
		int found = 0;
		if ((sscanf(m, "audio %d/%d RTP/AVP %n", &x, &y, &len) == 2) ||
		    (sscanf(m, "audio %d RTP/AVP %n", &x, &len) == 1)) {
			found = 1;
			portno = x;
			/* Scan through the RTP payload types specified in a "m=" line: */
			ast_rtp_pt_clear(p->rtp);
			codecs = m + len;
			while(!ast_strlen_zero(codecs)) {
				if (sscanf(codecs, "%d%n", &codec, &len) != 1) {
					ast_log(LOG_WARNING, "Error in codec string '%s'\n", codecs);
					return -1;
				}
				if (debug)
					ast_verbose("Found RTP audio format %d\n", codec);
				ast_rtp_set_m_type(p->rtp, codec);
				codecs = ast_skip_blanks(codecs + len);
			}
		}
		if (p->vrtp)
			ast_rtp_pt_clear(p->vrtp);  /* Must be cleared in case no m=video line exists */

		if (p->vrtp && (sscanf(m, "video %d RTP/AVP %n", &x, &len) == 1)) {
			found = 1;
			ast_clear_flag(p, SIP_NOVIDEO);	
			vportno = x;
			/* Scan through the RTP payload types specified in a "m=" line: */
			codecs = m + len;
			while(!ast_strlen_zero(codecs)) {
				if (sscanf(codecs, "%d%n", &codec, &len) != 1) {
					ast_log(LOG_WARNING, "Error in codec string '%s'\n", codecs);
					return -1;
				}
				if (debug)
					ast_verbose("Found RTP video format %d\n", codec);
				ast_rtp_set_m_type(p->vrtp, codec);
				codecs = ast_skip_blanks(codecs + len);
			}
		}
		if (!found )
			ast_log(LOG_WARNING, "Unknown SDP media type in offer: %s\n", m);
	}
	if (portno == -1 && vportno == -1) {
		/* No acceptable offer found in SDP */
		return -2;
	}
	/* Check for Media-description-level-address for audio */
	if (pedanticsipchecking) {
		c = get_sdp_iterate(&destiterator, req, "c");
		if (!ast_strlen_zero(c)) {
			if (sscanf(c, "IN IP4 %256s", host) != 1) {
				ast_log(LOG_WARNING, "Invalid secondary host in c= line, '%s'\n", c);
			} else {
				/* XXX This could block for a long time, and block the main thread! XXX */
				hp = ast_gethostbyname(host, &ahp);
				if (!hp) {
					ast_log(LOG_WARNING, "Unable to lookup host in secondary c= line, '%s'\n", c);
				}
			}
		}
	}
	/* RTP addresses and ports for audio and video */
	sin.sin_family = AF_INET;
	memcpy(&sin.sin_addr, hp->h_addr, sizeof(sin.sin_addr));

	/* Setup audio port number */
	sin.sin_port = htons(portno);
	if (p->rtp && sin.sin_port) {
		ast_rtp_set_peer(p->rtp, &sin);
		if (debug) {
			ast_verbose("Peer audio RTP is at port %s:%d\n", ast_inet_ntoa(iabuf,sizeof(iabuf), sin.sin_addr), ntohs(sin.sin_port));
			ast_log(LOG_DEBUG,"Peer audio RTP is at port %s:%d\n",ast_inet_ntoa(iabuf, sizeof(iabuf), sin.sin_addr), ntohs(sin.sin_port));
		}
	}
	/* Check for Media-description-level-address for video */
	if (pedanticsipchecking) {
		c = get_sdp_iterate(&destiterator, req, "c");
		if (!ast_strlen_zero(c)) {
			if (sscanf(c, "IN IP4 %256s", host) != 1) {
				ast_log(LOG_WARNING, "Invalid secondary host in c= line, '%s'\n", c);
			} else {
				/* XXX This could block for a long time, and block the main thread! XXX */
				hp = ast_gethostbyname(host, &ahp);
				if (!hp) {
					ast_log(LOG_WARNING, "Unable to lookup host in secondary c= line, '%s'\n", c);
				}
			}
		}
	}
	/* Setup video port number */
	sin.sin_port = htons(vportno);
	if (p->vrtp && sin.sin_port) {
		ast_rtp_set_peer(p->vrtp, &sin);
		if (debug) {
			ast_verbose("Peer video RTP is at port %s:%d\n", ast_inet_ntoa(iabuf,sizeof(iabuf), sin.sin_addr), ntohs(sin.sin_port));
			ast_log(LOG_DEBUG,"Peer video RTP is at port %s:%d\n",ast_inet_ntoa(iabuf, sizeof(iabuf), sin.sin_addr), ntohs(sin.sin_port));
		}
	}

	/* Next, scan through each "a=rtpmap:" line, noting each
	 * specified RTP payload type (with corresponding MIME subtype):
	 */
	sdpLineNum_iterator_init(&iterator);
	while ((a = get_sdp_iterate(&iterator, req, "a"))[0] != '\0') {
		char* mimeSubtype = ast_strdupa(a); /* ensures we have enough space */
		if (!strcasecmp(a, "sendonly")) {
			sendonly=1;
			continue;
		}
		if (!strcasecmp(a, "sendrecv")) {
		  	sendonly=0;
		}
		if (sscanf(a, "rtpmap: %u %[^/]/", &codec, mimeSubtype) != 2) continue;
		if (debug)
			ast_verbose("Found description format %s\n", mimeSubtype);
		/* Note: should really look at the 'freq' and '#chans' params too */
		ast_rtp_set_rtpmap_type(p->rtp, codec, "audio", mimeSubtype);
		if (p->vrtp)
			ast_rtp_set_rtpmap_type(p->vrtp, codec, "video", mimeSubtype);
	}

	/* Now gather all of the codecs that were asked for: */
	ast_rtp_get_current_formats(p->rtp,
				&peercapability, &peernoncodeccapability);
	if (p->vrtp)
		ast_rtp_get_current_formats(p->vrtp,
				&vpeercapability, &vpeernoncodeccapability);
	p->jointcapability = p->capability & (peercapability | vpeercapability);
	p->peercapability = (peercapability | vpeercapability);
	p->noncodeccapability = noncodeccapability & peernoncodeccapability;
	
	if (ast_test_flag(p, SIP_DTMF) == SIP_DTMF_AUTO) {
		ast_clear_flag(p, SIP_DTMF);
		if (p->noncodeccapability & AST_RTP_DTMF) {
			/* XXX Would it be reasonable to drop the DSP at this point? XXX */
			ast_set_flag(p, SIP_DTMF_RFC2833);
		} else {
			ast_set_flag(p, SIP_DTMF_INBAND);
		}
	}
	
	if (debug) {
		/* shame on whoever coded this.... */
		const unsigned slen=512;
		char s1[slen], s2[slen], s3[slen], s4[slen];

		ast_verbose("Capabilities: us - %s, peer - audio=%s/video=%s, combined - %s\n",
			ast_getformatname_multiple(s1, slen, p->capability),
			ast_getformatname_multiple(s2, slen, peercapability),
			ast_getformatname_multiple(s3, slen, vpeercapability),
			ast_getformatname_multiple(s4, slen, p->jointcapability));

		ast_verbose("Non-codec capabilities: us - %s, peer - %s, combined - %s\n",
			ast_rtp_lookup_mime_multiple(s1, slen, noncodeccapability, 0),
			ast_rtp_lookup_mime_multiple(s2, slen, peernoncodeccapability, 0),
			ast_rtp_lookup_mime_multiple(s3, slen, p->noncodeccapability, 0));
	}
	if (!p->jointcapability) {
		ast_log(LOG_NOTICE, "No compatible codecs!\n");
		return -1;
	}

	if (!p->owner) 	/* There's no open channel owning us */
		return 0;

	if (!(p->owner->nativeformats & p->jointcapability & AST_FORMAT_AUDIO_MASK)) {
		const unsigned slen=512;
		char s1[slen], s2[slen];
		ast_log(LOG_DEBUG, "Oooh, we need to change our formats since our peer supports only %s and not %s\n", 
				ast_getformatname_multiple(s1, slen, p->jointcapability),
				ast_getformatname_multiple(s2, slen, p->owner->nativeformats));
		p->owner->nativeformats = ast_codec_choose(&p->prefs, p->jointcapability, 1) | (p->capability & vpeercapability);
		ast_set_read_format(p->owner, p->owner->readformat);
		ast_set_write_format(p->owner, p->owner->writeformat);
	}
	if ((bridgepeer=ast_bridged_channel(p->owner))) {
		/* We have a bridge */
		/* Turn on/off music on hold if we are holding/unholding */
		struct ast_frame af = { AST_FRAME_NULL, };
		if (sin.sin_addr.s_addr && !sendonly) {
			ast_moh_stop(bridgepeer);
		
			/* Activate a re-invite */
			ast_queue_frame(p->owner, &af);
		} else {
			/* No address for RTP, we're on hold */
			
			ast_moh_start(bridgepeer, NULL);
			if (sendonly)
				ast_rtp_stop(p->rtp);
			/* Activate a re-invite */
			ast_queue_frame(p->owner, &af);
		}
	}

	/* Manager Hold and Unhold events must be generated, if necessary */
	if (sin.sin_addr.s_addr && !sendonly) {	        
	        append_history(p, "Unhold", "%s", req->data);

		if (callevents && ast_test_flag(p, SIP_CALL_ONHOLD)) {
			manager_event(EVENT_FLAG_CALL, "Unhold",
				"Channel: %s\r\n"
				"Uniqueid: %s\r\n",
				p->owner->name, 
				p->owner->uniqueid);

       		}
		ast_clear_flag(p, SIP_CALL_ONHOLD);
	} else {	        
		/* No address for RTP, we're on hold */
	        append_history(p, "Hold", "%s", req->data);

	        if (callevents && !ast_test_flag(p, SIP_CALL_ONHOLD)) {
			manager_event(EVENT_FLAG_CALL, "Hold",
				"Channel: %s\r\n"
		   	    	"Uniqueid: %s\r\n",
				p->owner->name, 
				p->owner->uniqueid);
		}
		ast_set_flag(p, SIP_CALL_ONHOLD);
	}

	return 0;
}

/*! \brief  add_header: Add header to SIP message */
static int add_header(struct sip_request *req, const char *var, const char *value)
{
	int x = 0;

	if (req->headers == SIP_MAX_HEADERS) {
		ast_log(LOG_WARNING, "Out of SIP header space\n");
		return -1;
	}

	if (req->lines) {
		ast_log(LOG_WARNING, "Can't add more headers when lines have been added\n");
		return -1;
	}

	if (req->len >= sizeof(req->data) - 4) {
		ast_log(LOG_WARNING, "Out of space, can't add anymore (%s:%s)\n", var, value);
		return -1;
	}

	req->header[req->headers] = req->data + req->len;

	if (compactheaders) {
		for (x = 0; x < (sizeof(aliases) / sizeof(aliases[0])); x++)
			if (!strcasecmp(aliases[x].fullname, var))
				var = aliases[x].shortname;
	}

	snprintf(req->header[req->headers], sizeof(req->data) - req->len - 4, "%s: %s\r\n", var, value);
	req->len += strlen(req->header[req->headers]);
	req->headers++;

	return 0;	
}

/*! \brief  add_header_contentLen: Add 'Content-Length' header to SIP message */
static int add_header_contentLength(struct sip_request *req, int len)
{
	char clen[10];

	snprintf(clen, sizeof(clen), "%d", len);
	return add_header(req, "Content-Length", clen);
}

/*! \brief  add_blank_header: Add blank header to SIP message */
static int add_blank_header(struct sip_request *req)
{
	if (req->headers == SIP_MAX_HEADERS)  {
		ast_log(LOG_WARNING, "Out of SIP header space\n");
		return -1;
	}
	if (req->lines) {
		ast_log(LOG_WARNING, "Can't add more headers when lines have been added\n");
		return -1;
	}
	if (req->len >= sizeof(req->data) - 4) {
		ast_log(LOG_WARNING, "Out of space, can't add anymore\n");
		return -1;
	}
	req->header[req->headers] = req->data + req->len;
	snprintf(req->header[req->headers], sizeof(req->data) - req->len, "\r\n");
	req->len += strlen(req->header[req->headers]);
	req->headers++;
	return 0;	
}

/*! \brief  add_line: Add content (not header) to SIP message */
static int add_line(struct sip_request *req, const char *line)
{
	if (req->lines == SIP_MAX_LINES)  {
		ast_log(LOG_WARNING, "Out of SIP line space\n");
		return -1;
	}
	if (!req->lines) {
		/* Add extra empty return */
		snprintf(req->data + req->len, sizeof(req->data) - req->len, "\r\n");
		req->len += strlen(req->data + req->len);
	}
	if (req->len >= sizeof(req->data) - 4) {
		ast_log(LOG_WARNING, "Out of space, can't add anymore\n");
		return -1;
	}
	req->line[req->lines] = req->data + req->len;
	snprintf(req->line[req->lines], sizeof(req->data) - req->len, "%s", line);
	req->len += strlen(req->line[req->lines]);
	req->lines++;
	return 0;	
}

/*! \brief  copy_header: Copy one header field from one request to another */
static int copy_header(struct sip_request *req, struct sip_request *orig, char *field)
{
	char *tmp;
	tmp = get_header(orig, field);
	if (!ast_strlen_zero(tmp)) {
		/* Add what we're responding to */
		return add_header(req, field, tmp);
	}
	ast_log(LOG_NOTICE, "No field '%s' present to copy\n", field);
	return -1;
}

/*! \brief  copy_all_header: Copy all headers from one request to another */
static int copy_all_header(struct sip_request *req, struct sip_request *orig, char *field)
{
	char *tmp;
	int start = 0;
	int copied = 0;
	for (;;) {
		tmp = __get_header(orig, field, &start);
		if (!ast_strlen_zero(tmp)) {
			/* Add what we're responding to */
			add_header(req, field, tmp);
			copied++;
		} else
			break;
	}
	return copied ? 0 : -1;
}

/*! \brief  copy_via_headers: Copy SIP VIA Headers from the request to the response */
/*	If the client indicates that it wishes to know the port we received from,
	it adds ;rport without an argument to the topmost via header. We need to
	add the port number (from our point of view) to that parameter.
	We always add ;received=<ip address> to the topmost via header.
	Received: RFC 3261, rport RFC 3581 */
static int copy_via_headers(struct sip_pvt *p, struct sip_request *req, struct sip_request *orig, char *field)
{
	char tmp[256], *oh, *end;
	int start = 0;
	int copied = 0;
	char iabuf[INET_ADDRSTRLEN];

	for (;;) {
		oh = __get_header(orig, field, &start);
		if (!ast_strlen_zero(oh)) {
			if (!copied) {	/* Only check for empty rport in topmost via header */
				char *rport;
				char new[256];

				/* Find ;rport;  (empty request) */
				rport = strstr(oh, ";rport");
				if (rport && *(rport+6) == '=') 
					rport = NULL;		/* We already have a parameter to rport */

				if (rport && (ast_test_flag(p, SIP_NAT) == SIP_NAT_ALWAYS)) {
					/* We need to add received port - rport */
					ast_copy_string(tmp, oh, sizeof(tmp));

					rport = strstr(tmp, ";rport");

					if (rport) {
						end = strchr(rport + 1, ';');
						if (end)
							memmove(rport, end, strlen(end) + 1);
						else
							*rport = '\0';
					}

					/* Add rport to first VIA header if requested */
					/* Whoo hoo!  Now we can indicate port address translation too!  Just
				   	   another RFC (RFC3581). I'll leave the original comments in for
				   	   posterity.  */
					snprintf(new, sizeof(new), "%s;received=%s;rport=%d", tmp, ast_inet_ntoa(iabuf, sizeof(iabuf), p->recv.sin_addr), ntohs(p->recv.sin_port));
				} else {
					/* We should *always* add a received to the topmost via */
					snprintf(new, sizeof(new), "%s;received=%s", oh, ast_inet_ntoa(iabuf, sizeof(iabuf), p->recv.sin_addr));
				}
				add_header(req, field, new);
			} else {
				/* Add the following via headers untouched */
				add_header(req, field, oh);
			}
			copied++;
		} else
			break;
	}
	if (!copied) {
		ast_log(LOG_NOTICE, "No header field '%s' present to copy\n", field);
		return -1;
	}
	return 0;
}

/*! \brief  add_route: Add route header into request per learned route */
static void add_route(struct sip_request *req, struct sip_route *route)
{
	char r[256], *p;
	int n, rem = sizeof(r);

	if (!route) return;

	p = r;
	while (route) {
		n = strlen(route->hop);
		if ((n+3)>rem) break;
		if (p != r) {
			*p++ = ',';
			--rem;
		}
		*p++ = '<';
		ast_copy_string(p, route->hop, rem);  p += n;
		*p++ = '>';
		rem -= (n+2);
		route = route->next;
	}
	*p = '\0';
	add_header(req, "Route", r);
}

/*! \brief  set_destination: Set destination from SIP URI */
static void set_destination(struct sip_pvt *p, char *uri)
{
	char *h, *maddr, hostname[256];
	char iabuf[INET_ADDRSTRLEN];
	int port, hn;
	struct hostent *hp;
	struct ast_hostent ahp;
	int debug=sip_debug_test_pvt(p);

	/* Parse uri to h (host) and port - uri is already just the part inside the <> */
	/* general form we are expecting is sip[s]:username[:password]@host[:port][;...] */

	if (debug)
		ast_verbose("set_destination: Parsing <%s> for address/port to send to\n", uri);

	/* Find and parse hostname */
	h = strchr(uri, '@');
	if (h)
		++h;
	else {
		h = uri;
		if (strncmp(h, "sip:", 4) == 0)
			h += 4;
		else if (strncmp(h, "sips:", 5) == 0)
			h += 5;
	}
	hn = strcspn(h, ":;>") + 1;
	if (hn > sizeof(hostname)) 
		hn = sizeof(hostname);
	ast_copy_string(hostname, h, hn);
	h += hn - 1;

	/* Is "port" present? if not default to DEFAULT_SIP_PORT */
	if (*h == ':') {
		/* Parse port */
		++h;
		port = strtol(h, &h, 10);
	}
	else
		port = DEFAULT_SIP_PORT;

	/* Got the hostname:port - but maybe there's a "maddr=" to override address? */
	maddr = strstr(h, "maddr=");
	if (maddr) {
		maddr += 6;
		hn = strspn(maddr, "0123456789.") + 1;
		if (hn > sizeof(hostname)) hn = sizeof(hostname);
		ast_copy_string(hostname, maddr, hn);
	}
	
	hp = ast_gethostbyname(hostname, &ahp);
	if (hp == NULL)  {
		ast_log(LOG_WARNING, "Can't find address for host '%s'\n", hostname);
		return;
	}
	p->sa.sin_family = AF_INET;
	memcpy(&p->sa.sin_addr, hp->h_addr, sizeof(p->sa.sin_addr));
	p->sa.sin_port = htons(port);
	if (debug)
		ast_verbose("set_destination: set destination to %s, port %d\n", ast_inet_ntoa(iabuf, sizeof(iabuf), p->sa.sin_addr), port);
}

/*! \brief  init_resp: Initialize SIP response, based on SIP request */
static int init_resp(struct sip_request *req, char *resp, struct sip_request *orig)
{
	/* Initialize a response */
	if (req->headers || req->len) {
		ast_log(LOG_WARNING, "Request already initialized?!?\n");
		return -1;
	}
	req->method = SIP_RESPONSE;
	req->header[req->headers] = req->data + req->len;
	snprintf(req->header[req->headers], sizeof(req->data) - req->len, "SIP/2.0 %s\r\n", resp);
	req->len += strlen(req->header[req->headers]);
	req->headers++;
	return 0;
}

/*! \brief  init_req: Initialize SIP request */
static int init_req(struct sip_request *req, int sipmethod, const char *recip)
{
	/* Initialize a response */
	if (req->headers || req->len) {
		ast_log(LOG_WARNING, "Request already initialized?!?\n");
		return -1;
	}
	req->header[req->headers] = req->data + req->len;
	snprintf(req->header[req->headers], sizeof(req->data) - req->len, "%s %s SIP/2.0\r\n", sip_methods[sipmethod].text, recip);
	req->len += strlen(req->header[req->headers]);
	req->headers++;
	req->method = sipmethod;
	return 0;
}


/*! \brief  respprep: Prepare SIP response packet */
static int respprep(struct sip_request *resp, struct sip_pvt *p, char *msg, struct sip_request *req)
{
	char newto[256], *ot;

	memset(resp, 0, sizeof(*resp));
	init_resp(resp, msg, req);
	copy_via_headers(p, resp, req, "Via");
	if (msg[0] == '2')
		copy_all_header(resp, req, "Record-Route");
	copy_header(resp, req, "From");
	ot = get_header(req, "To");
	if (!strcasestr(ot, "tag=") && strncmp(msg, "100", 3)) {
		/* Add the proper tag if we don't have it already.  If they have specified
		   their tag, use it.  Otherwise, use our own tag */
		if (!ast_strlen_zero(p->theirtag) && ast_test_flag(p, SIP_OUTGOING))
			snprintf(newto, sizeof(newto), "%s;tag=%s", ot, p->theirtag);
		else if (p->tag && !ast_test_flag(p, SIP_OUTGOING))
			snprintf(newto, sizeof(newto), "%s;tag=%s", ot, p->tag);
		else {
			ast_copy_string(newto, ot, sizeof(newto));
			newto[sizeof(newto) - 1] = '\0';
		}
		ot = newto;
	}
	add_header(resp, "To", ot);
	copy_header(resp, req, "Call-ID");
	copy_header(resp, req, "CSeq");
	add_header(resp, "User-Agent", global_useragent);
	add_header(resp, "Allow", ALLOWED_METHODS);
	if (msg[0] == '2' && (p->method == SIP_SUBSCRIBE || p->method == SIP_REGISTER)) {
		/* For registration responses, we also need expiry and
		   contact info */
		char tmp[256];

		snprintf(tmp, sizeof(tmp), "%d", p->expiry);
		add_header(resp, "Expires", tmp);
		if (p->expiry) {	/* Only add contact if we have an expiry time */
			char contact[256];
			snprintf(contact, sizeof(contact), "%s;expires=%d", p->our_contact, p->expiry);
			add_header(resp, "Contact", contact);	/* Not when we unregister */
		}
	} else if (p->our_contact[0]) {
		add_header(resp, "Contact", p->our_contact);
	}
	return 0;
}

/*! \brief  reqprep: Initialize a SIP request response packet */
static int reqprep(struct sip_request *req, struct sip_pvt *p, int sipmethod, int seqno, int newbranch)
{
	struct sip_request *orig = &p->initreq;
	char stripped[80];
	char tmp[80];
	char newto[256];
	const char *c;
	char *n;
	char *ot, *of;
	int is_strict = 0;		/*!< Strict routing flag */

	memset(req, 0, sizeof(struct sip_request));
	
	snprintf(p->lastmsg, sizeof(p->lastmsg), "Tx: %s", sip_methods[sipmethod].text);
	
	if (!seqno) {
		p->ocseq++;
		seqno = p->ocseq;
	}
	
	if (newbranch) {
		p->branch ^= thread_safe_rand();
		build_via(p);
	}

	/* Check for strict or loose router */
	if (p->route && !ast_strlen_zero(p->route->hop) && strstr(p->route->hop,";lr") == NULL) {
		is_strict = 1;
		if (sipdebug)
			ast_log(LOG_DEBUG, "Strict routing enforced for session %s\n", p->callid);
	}

	if (sipmethod == SIP_CANCEL) {
		c = p->initreq.rlPart2;	/* Use original URI */
	} else if (sipmethod == SIP_ACK) {
		/* Use URI from Contact: in 200 OK (if INVITE) 
		(we only have the contacturi on INVITEs) */
		if (!ast_strlen_zero(p->okcontacturi))
			c = is_strict ? p->route->hop : p->okcontacturi;
 		else
 			c = p->initreq.rlPart2;
	} else if (!ast_strlen_zero(p->okcontacturi)) {
		c = is_strict ? p->route->hop : p->okcontacturi; /* Use for BYE or REINVITE */
	} else if (!ast_strlen_zero(p->uri)) {
		c = p->uri;
	} else {
		/* We have no URI, use To: or From:  header as URI (depending on direction) */
		ast_copy_string(stripped, get_header(orig, (ast_test_flag(p, SIP_OUTGOING)) ? "To" : "From"),
				sizeof(stripped));
		c = get_in_brackets(stripped);
		n = strchr(c, ';');
		if (n)
			*n = '\0';
	}	
	init_req(req, sipmethod, c);

	snprintf(tmp, sizeof(tmp), "%d %s", seqno, sip_methods[sipmethod].text);

	add_header(req, "Via", p->via);
	if (p->route) {
		set_destination(p, p->route->hop);
		if (is_strict)
			add_route(req, p->route->next);
		else
			add_route(req, p->route);
	}

	ot = get_header(orig, "To");
	of = get_header(orig, "From");

	/* Add tag *unless* this is a CANCEL, in which case we need to send it exactly
	   as our original request, including tag (or presumably lack thereof) */
	if (!strcasestr(ot, "tag=") && sipmethod != SIP_CANCEL) {
		/* Add the proper tag if we don't have it already.  If they have specified
		   their tag, use it.  Otherwise, use our own tag */
		if (ast_test_flag(p, SIP_OUTGOING) && !ast_strlen_zero(p->theirtag))
			snprintf(newto, sizeof(newto), "%s;tag=%s", ot, p->theirtag);
		else if (!ast_test_flag(p, SIP_OUTGOING))
			snprintf(newto, sizeof(newto), "%s;tag=%s", ot, p->tag);
		else
			snprintf(newto, sizeof(newto), "%s", ot);
		ot = newto;
	}

	if (ast_test_flag(p, SIP_OUTGOING)) {
		add_header(req, "From", of);
		add_header(req, "To", ot);
	} else {
		add_header(req, "From", ot);
		add_header(req, "To", of);
	}
	add_header(req, "Contact", p->our_contact);
	copy_header(req, orig, "Call-ID");
	add_header(req, "CSeq", tmp);

	add_header(req, "User-Agent", global_useragent);
	add_header(req, "Max-Forwards", DEFAULT_MAX_FORWARDS);

	if (!ast_strlen_zero(p->rpid))
		add_header(req, "Remote-Party-ID", p->rpid);

	return 0;
}

/*! \brief  __transmit_response: Base transmit response function */
static int __transmit_response(struct sip_pvt *p, char *msg, struct sip_request *req, int reliable)
{
	struct sip_request resp;
	int seqno = 0;

	if (reliable && (sscanf(get_header(req, "CSeq"), "%d ", &seqno) != 1)) {
		ast_log(LOG_WARNING, "Unable to determine sequence number from '%s'\n", get_header(req, "CSeq"));
		return -1;
	}
	respprep(&resp, p, msg, req);
	add_header_contentLength(&resp, 0);
	/* If we are cancelling an incoming invite for some reason, add information
		about the reason why we are doing this in clear text */
	if (p->owner && p->owner->hangupcause) {
		add_header(&resp, "X-Asterisk-HangupCause", ast_cause2str(p->owner->hangupcause));
	}
	add_blank_header(&resp);
	return send_response(p, &resp, reliable, seqno);
}

/*! \brief  transmit_response: Transmit response, no retransmits */
static int transmit_response(struct sip_pvt *p, char *msg, struct sip_request *req) 
{
	return __transmit_response(p, msg, req, 0);
}

/*! \brief  transmit_response_with_unsupported: Transmit response, no retransmits */
static int transmit_response_with_unsupported(struct sip_pvt *p, char *msg, struct sip_request *req, char *unsupported) 
{
	struct sip_request resp;
	respprep(&resp, p, msg, req);
	append_date(&resp);
	add_header(&resp, "Unsupported", unsupported);
	return send_response(p, &resp, 0, 0);
}

/*! \brief  transmit_response_reliable: Transmit response, Make sure you get a reply */
static int transmit_response_reliable(struct sip_pvt *p, char *msg, struct sip_request *req, int fatal)
{
	return __transmit_response(p, msg, req, fatal ? 2 : 1);
}

/*! \brief  append_date: Append date to SIP message */
static void append_date(struct sip_request *req)
{
	char tmpdat[256];
	struct tm tm;
	time_t t;

	time(&t);
	gmtime_r(&t, &tm);
	strftime(tmpdat, sizeof(tmpdat), "%a, %d %b %Y %T GMT", &tm);
	add_header(req, "Date", tmpdat);
}

/*! \brief  transmit_response_with_date: Append date and content length before transmitting response */
static int transmit_response_with_date(struct sip_pvt *p, char *msg, struct sip_request *req)
{
	struct sip_request resp;
	respprep(&resp, p, msg, req);
	append_date(&resp);
	add_header_contentLength(&resp, 0);
	add_blank_header(&resp);
	return send_response(p, &resp, 0, 0);
}

/*! \brief  transmit_response_with_allow: Append Accept header, content length before transmitting response */
static int transmit_response_with_allow(struct sip_pvt *p, char *msg, struct sip_request *req, int reliable)
{
	struct sip_request resp;
	respprep(&resp, p, msg, req);
	add_header(&resp, "Accept", "application/sdp");
	add_header_contentLength(&resp, 0);
	add_blank_header(&resp);
	return send_response(p, &resp, reliable, 0);
}

/* transmit_response_with_auth: Respond with authorization request */
static int transmit_response_with_auth(struct sip_pvt *p, char *msg, struct sip_request *req, const char *randdata, int reliable, char *header, int stale)
{
	struct sip_request resp;
	char tmp[256];
	int seqno = 0;

	if (reliable && (sscanf(get_header(req, "CSeq"), "%d ", &seqno) != 1)) {
		ast_log(LOG_WARNING, "Unable to determine sequence number from '%s'\n", get_header(req, "CSeq"));
		return -1;
	}
	/* Stale means that they sent us correct authentication, but 
	   based it on an old challenge (nonce) */
	snprintf(tmp, sizeof(tmp), "Digest realm=\"%s\", nonce=\"%s\"%s", global_realm, randdata, stale ? ", stale=true" : "");
	respprep(&resp, p, msg, req);
	add_header(&resp, header, tmp);
	add_header_contentLength(&resp, 0);
	add_blank_header(&resp);
	return send_response(p, &resp, reliable, seqno);
}

/*! \brief  add_text: Add text body to SIP message */
static int add_text(struct sip_request *req, const char *text)
{
	/* XXX Convert \n's to \r\n's XXX */
	add_header(req, "Content-Type", "text/plain");
	add_header_contentLength(req, strlen(text));
	add_line(req, text);
	return 0;
}

/*! \brief  add_digit: add DTMF INFO tone to sip message */
/* Always adds default duration 250 ms, regardless of what came in over the line */
static int add_digit(struct sip_request *req, char digit)
{
	char tmp[256];

	snprintf(tmp, sizeof(tmp), "Signal=%c\r\nDuration=250\r\n", digit);
	add_header(req, "Content-Type", "application/dtmf-relay");
	add_header_contentLength(req, strlen(tmp));
	add_line(req, tmp);
	return 0;
}

/*! \brief  add_vidupdate: add XML encoded media control with update */
/* XML: The only way to turn 0 bits of information into a few hundred. */
static int add_vidupdate(struct sip_request *req)
{
	const char *xml_is_a_huge_waste_of_space =
		"<?xml version=\"1.0\" encoding=\"utf-8\" ?>\r\n"
		" <media_control>\r\n"
		"  <vc_primitive>\r\n"
		"   <to_encoder>\r\n"
		"    <picture_fast_update>\r\n"
		"    </picture_fast_update>\r\n"
		"   </to_encoder>\r\n"
		"  </vc_primitive>\r\n"
		" </media_control>\r\n";
	add_header(req, "Content-Type", "application/media_control+xml");
	add_header_contentLength(req, strlen(xml_is_a_huge_waste_of_space));
	add_line(req, xml_is_a_huge_waste_of_space);
	return 0;
}

static void add_codec_to_sdp(const struct sip_pvt *p, int codec, int sample_rate,
			     char **m_buf, size_t *m_size, char **a_buf, size_t *a_size,
			     int debug)
{
	int rtp_code;

	if (debug)
		ast_verbose("Adding codec 0x%x (%s) to SDP\n", codec, ast_getformatname(codec));
	if ((rtp_code = ast_rtp_lookup_code(p->rtp, 1, codec)) == -1)
		return;

	ast_build_string(m_buf, m_size, " %d", rtp_code);
	ast_build_string(a_buf, a_size, "a=rtpmap:%d %s/%d\r\n", rtp_code,
			 ast_rtp_lookup_mime_subtype(1, codec),
			 sample_rate);
	if (codec == AST_FORMAT_G729A)
		/* Indicate that we don't support VAD (G.729 annex B) */
		ast_build_string(a_buf, a_size, "a=fmtp:%d annexb=no\r\n", rtp_code);
}

static void add_noncodec_to_sdp(const struct sip_pvt *p, int format, int sample_rate,
				char **m_buf, size_t *m_size, char **a_buf, size_t *a_size,
				int debug)
{
	int rtp_code;

	if (debug)
		ast_verbose("Adding non-codec 0x%x (%s) to SDP\n", format, ast_rtp_lookup_mime_subtype(0, format));
	if ((rtp_code = ast_rtp_lookup_code(p->rtp, 0, format)) == -1)
		return;

	ast_build_string(m_buf, m_size, " %d", rtp_code);
	ast_build_string(a_buf, a_size, "a=rtpmap:%d %s/%d\r\n", rtp_code,
			 ast_rtp_lookup_mime_subtype(0, format),
			 sample_rate);
	if (format == AST_RTP_DTMF)
		/* Indicate we support DTMF and FLASH... */
		ast_build_string(a_buf, a_size, "a=fmtp:%d 0-16\r\n", rtp_code);
}

/*! \brief  add_sdp: Add Session Description Protocol message */
static int add_sdp(struct sip_request *resp, struct sip_pvt *p)
{
	int len = 0;
	int pref_codec;
	int alreadysent = 0;
	struct sockaddr_in sin;
	struct sockaddr_in vsin;
	char v[256];
	char s[256];
	char o[256];
	char c[256];
	char t[256];
	char m_audio[256];
	char m_video[256];
	char a_audio[1024];
	char a_video[1024];
	char *m_audio_next = m_audio;
	char *m_video_next = m_video;
	size_t m_audio_left = sizeof(m_audio);
	size_t m_video_left = sizeof(m_video);
	char *a_audio_next = a_audio;
	char *a_video_next = a_video;
	size_t a_audio_left = sizeof(a_audio);
	size_t a_video_left = sizeof(a_video);
	char iabuf[INET_ADDRSTRLEN];
	int x;
	int capability;
	struct sockaddr_in dest;
	struct sockaddr_in vdest = { 0, };
	int debug;
	
	debug = sip_debug_test_pvt(p);

	len = 0;
	if (!p->rtp) {
		ast_log(LOG_WARNING, "No way to add SDP without an RTP structure\n");
		return -1;
	}
	capability = p->capability;
		
	if (!p->sessionid) {
		p->sessionid = getpid();
		p->sessionversion = p->sessionid;
	} else
		p->sessionversion++;
	ast_rtp_get_us(p->rtp, &sin);
	if (p->vrtp)
		ast_rtp_get_us(p->vrtp, &vsin);

	if (p->redirip.sin_addr.s_addr) {
		dest.sin_port = p->redirip.sin_port;
		dest.sin_addr = p->redirip.sin_addr;
		if (p->redircodecs)
			capability = p->redircodecs;
	} else {
		dest.sin_addr = p->ourip;
		dest.sin_port = sin.sin_port;
	}

	/* Determine video destination */
	if (p->vrtp) {
		if (p->vredirip.sin_addr.s_addr) {
			vdest.sin_port = p->vredirip.sin_port;
			vdest.sin_addr = p->vredirip.sin_addr;
		} else {
			vdest.sin_addr = p->ourip;
			vdest.sin_port = vsin.sin_port;
		}
	}
	if (debug){
		ast_verbose("We're at %s port %d\n", ast_inet_ntoa(iabuf, sizeof(iabuf), p->ourip), ntohs(sin.sin_port));	
		if (p->vrtp)
			ast_verbose("Video is at %s port %d\n", ast_inet_ntoa(iabuf, sizeof(iabuf), p->ourip), ntohs(vsin.sin_port));	
	}

	/* We break with the "recommendation" and send our IP, in order that our
	   peer doesn't have to ast_gethostbyname() us */

	snprintf(v, sizeof(v), "v=0\r\n");
	snprintf(o, sizeof(o), "o=root %d %d IN IP4 %s\r\n", p->sessionid, p->sessionversion, ast_inet_ntoa(iabuf, sizeof(iabuf), dest.sin_addr));
	snprintf(s, sizeof(s), "s=session\r\n");
	snprintf(c, sizeof(c), "c=IN IP4 %s\r\n", ast_inet_ntoa(iabuf, sizeof(iabuf), dest.sin_addr));
	snprintf(t, sizeof(t), "t=0 0\r\n");

	ast_build_string(&m_audio_next, &m_audio_left, "m=audio %d RTP/AVP", ntohs(dest.sin_port));
	ast_build_string(&m_video_next, &m_video_left, "m=video %d RTP/AVP", ntohs(vdest.sin_port));

	/* Prefer the codec we were requested to use, first, no matter what */
	if (capability & p->prefcodec) {
		if (p->prefcodec <= AST_FORMAT_MAX_AUDIO)
			add_codec_to_sdp(p, p->prefcodec, 8000,
					 &m_audio_next, &m_audio_left,
					 &a_audio_next, &a_audio_left,
					 debug);
		else
			add_codec_to_sdp(p, p->prefcodec, 90000,
					 &m_video_next, &m_video_left,
					 &a_video_next, &a_video_left,
					 debug);
		alreadysent |= p->prefcodec;
	}

	/* Start by sending our preferred codecs */
	for (x = 0; x < 32; x++) {
		if (!(pref_codec = ast_codec_pref_index(&p->prefs, x)))
			break; 

		if (!(capability & pref_codec))
			continue;

		if (alreadysent & pref_codec)
			continue;

		if (pref_codec <= AST_FORMAT_MAX_AUDIO)
			add_codec_to_sdp(p, pref_codec, 8000,
					 &m_audio_next, &m_audio_left,
					 &a_audio_next, &a_audio_left,
					 debug);
		else
			add_codec_to_sdp(p, pref_codec, 90000,
					 &m_video_next, &m_video_left,
					 &a_video_next, &a_video_left,
					 debug);
		alreadysent |= pref_codec;
	}

	/* Now send any other common codecs, and non-codec formats: */
	for (x = 1; x <= ((global_videosupport && p->vrtp) ? AST_FORMAT_MAX_VIDEO : AST_FORMAT_MAX_AUDIO); x <<= 1) {
		if (!(capability & x))
			continue;

		if (alreadysent & x)
			continue;

		if (x <= AST_FORMAT_MAX_AUDIO)
			add_codec_to_sdp(p, x, 8000,
					 &m_audio_next, &m_audio_left,
					 &a_audio_next, &a_audio_left,
					 debug);
		else
			add_codec_to_sdp(p, x, 90000,
					 &m_video_next, &m_video_left,
					 &a_video_next, &a_video_left,
					 debug);
	}

	for (x = 1; x <= AST_RTP_MAX; x <<= 1) {
		if (!(p->noncodeccapability & x))
			continue;

		add_noncodec_to_sdp(p, x, 8000,
				    &m_audio_next, &m_audio_left,
				    &a_audio_next, &a_audio_left,
				    debug);
	}

	ast_build_string(&a_audio_next, &a_audio_left, "a=silenceSupp:off - - - -\r\n");

	if ((m_audio_left < 2) || (m_video_left < 2) || (a_audio_left == 0) || (a_video_left == 0))
		ast_log(LOG_WARNING, "SIP SDP may be truncated due to undersized buffer!!\n");

	ast_build_string(&m_audio_next, &m_audio_left, "\r\n");
	ast_build_string(&m_video_next, &m_video_left, "\r\n");

	len = strlen(v) + strlen(s) + strlen(o) + strlen(c) + strlen(t) + strlen(m_audio) + strlen(a_audio);
	if ((p->vrtp) && (!ast_test_flag(p, SIP_NOVIDEO)) && (capability & VIDEO_CODEC_MASK)) /* only if video response is appropriate */
		len += strlen(m_video) + strlen(a_video);

	add_header(resp, "Content-Type", "application/sdp");
	add_header_contentLength(resp, len);
	add_line(resp, v);
	add_line(resp, o);
	add_line(resp, s);
	add_line(resp, c);
	add_line(resp, t);
	add_line(resp, m_audio);
	add_line(resp, a_audio);
	if ((p->vrtp) && (!ast_test_flag(p, SIP_NOVIDEO)) && (capability & VIDEO_CODEC_MASK)) { /* only if video response is appropriate */
		add_line(resp, m_video);
		add_line(resp, a_video);
	}

	/* Update lastrtprx when we send our SDP */
	time(&p->lastrtprx);
	time(&p->lastrtptx);

	return 0;
}

/*! \brief  copy_request: copy SIP request (mostly used to save request for responses) */
static void copy_request(struct sip_request *dst, struct sip_request *src)
{
	long offset;
	int x;
	offset = ((void *)dst) - ((void *)src);
	/* First copy stuff */
	memcpy(dst, src, sizeof(*dst));
	/* Now fix pointer arithmetic */
	for (x=0; x < src->headers; x++)
		dst->header[x] += offset;
	for (x=0; x < src->lines; x++)
		dst->line[x] += offset;
}

/*! \brief  transmit_response_with_sdp: Used for 200 OK and 183 early media */
static int transmit_response_with_sdp(struct sip_pvt *p, char *msg, struct sip_request *req, int retrans)
{
	struct sip_request resp;
	int seqno;
	if (sscanf(get_header(req, "CSeq"), "%d ", &seqno) != 1) {
		ast_log(LOG_WARNING, "Unable to get seqno from '%s'\n", get_header(req, "CSeq"));
		return -1;
	}
	respprep(&resp, p, msg, req);
	if (p->rtp) {
		add_sdp(&resp, p);
	} else {
		ast_log(LOG_ERROR, "Can't add SDP to response, since we have no RTP session allocated. Call-ID %s\n", p->callid);
	}
	return send_response(p, &resp, retrans, seqno);
}

/*! \brief  determine_firstline_parts: parse first line of incoming SIP request */
static int determine_firstline_parts( struct sip_request *req ) 
{
	char *e, *cmd;
	int len;
  
	cmd = ast_skip_blanks(req->header[0]);
	if (!*cmd)
		return -1;
	req->rlPart1 = cmd;
	e = ast_skip_nonblanks(cmd);
	/* Get the command */
	if (*e)
		*e++ = '\0';
	e = ast_skip_blanks(e);
	if ( !*e )
		return -1;

	if ( !strcasecmp(cmd, "SIP/2.0") ) {
		/* We have a response */
		req->rlPart2 = e;
		len = strlen( req->rlPart2 );
		if ( len < 2 ) { 
			return -1;
		}
		ast_trim_blanks(e);
	} else {
		/* We have a request */
		if ( *e == '<' ) { 
			e++;
			if ( !*e ) { 
				return -1; 
			}  
		}
		req->rlPart2 = e;	/* URI */
		if ( ( e= strrchr( req->rlPart2, 'S' ) ) == NULL ) {
			return -1;
		}
		/* XXX maybe trim_blanks() ? */
		while( isspace( *(--e) ) ) {}
		if ( *e == '>' ) {
			*e = '\0';
		} else {
			*(++e)= '\0';
		}
	}
	return 1;
}

/*! \brief  transmit_reinvite_with_sdp: Transmit reinvite with SDP :-) */
/* 	A re-invite is basically a new INVITE with the same CALL-ID and TAG as the
	INVITE that opened the SIP dialogue 
	We reinvite so that the audio stream (RTP) go directly between
	the SIP UAs. SIP Signalling stays with * in the path.
*/
static int transmit_reinvite_with_sdp(struct sip_pvt *p)
{
	struct sip_request req;
	if (ast_test_flag(p, SIP_REINVITE_UPDATE))
		reqprep(&req, p, SIP_UPDATE, 0, 1);
	else 
		reqprep(&req, p, SIP_INVITE, 0, 1);
	
	add_header(&req, "Allow", ALLOWED_METHODS);
	if (sipdebug)
		add_header(&req, "X-asterisk-info", "SIP re-invite (RTP bridge)");
	add_sdp(&req, p);
	/* Use this as the basis */
	copy_request(&p->initreq, &req);
	parse_request(&p->initreq);
	if (sip_debug_test_pvt(p))
		ast_verbose("%d headers, %d lines\n", p->initreq.headers, p->initreq.lines);
	p->lastinvite = p->ocseq;
	ast_set_flag(p, SIP_OUTGOING);
	return send_request(p, &req, 1, p->ocseq);
}

/*! \brief  extract_uri: Check Contact: URI of SIP message */
static void extract_uri(struct sip_pvt *p, struct sip_request *req)
{
	char stripped[256];
	char *c, *n;
	ast_copy_string(stripped, get_header(req, "Contact"), sizeof(stripped));
	c = get_in_brackets(stripped);
	n = strchr(c, ';');
	if (n)
		*n = '\0';
	if (!ast_strlen_zero(c))
		ast_string_field_set(p, uri, c);
}

/*! \brief  build_contact: Build contact header - the contact header we send out */
static void build_contact(struct sip_pvt *p)
{
	char iabuf[INET_ADDRSTRLEN];

	/* Construct Contact: header */
	if (ourport != 5060)	/* Needs to be 5060, according to the RFC */
		ast_string_field_build(p, our_contact, "<sip:%s%s%s:%d>", p->exten, ast_strlen_zero(p->exten) ? "" : "@", ast_inet_ntoa(iabuf, sizeof(iabuf), p->ourip), ourport);
	else
		ast_string_field_build(p, our_contact, "<sip:%s%s%s>", p->exten, ast_strlen_zero(p->exten) ? "" : "@", ast_inet_ntoa(iabuf, sizeof(iabuf), p->ourip));
}

/*! \brief  build_rpid: Build the Remote Party-ID & From using callingpres options */
static void build_rpid(struct sip_pvt *p)
{
	int send_pres_tags = 1;
	const char *privacy=NULL;
	const char *screen=NULL;
	char buf[256];
	const char *clid = default_callerid;
	const char *clin = NULL;
	char iabuf[INET_ADDRSTRLEN];
	const char *fromdomain;

	if (!ast_strlen_zero(p->rpid) || !ast_strlen_zero(p->rpid_from))  
		return;

	if (p->owner && p->owner->cid.cid_num)
		clid = p->owner->cid.cid_num;
	if (p->owner && p->owner->cid.cid_name)
		clin = p->owner->cid.cid_name;
	if (ast_strlen_zero(clin))
		clin = clid;

	switch (p->callingpres) {
	case AST_PRES_ALLOWED_USER_NUMBER_NOT_SCREENED:
		privacy = "off";
		screen = "no";
		break;
	case AST_PRES_ALLOWED_USER_NUMBER_PASSED_SCREEN:
		privacy = "off";
		screen = "pass";
		break;
	case AST_PRES_ALLOWED_USER_NUMBER_FAILED_SCREEN:
		privacy = "off";
		screen = "fail";
		break;
	case AST_PRES_ALLOWED_NETWORK_NUMBER:
		privacy = "off";
		screen = "yes";
		break;
	case AST_PRES_PROHIB_USER_NUMBER_NOT_SCREENED:
		privacy = "full";
		screen = "no";
		break;
	case AST_PRES_PROHIB_USER_NUMBER_PASSED_SCREEN:
		privacy = "full";
		screen = "pass";
		break;
	case AST_PRES_PROHIB_USER_NUMBER_FAILED_SCREEN:
		privacy = "full";
		screen = "fail";
		break;
	case AST_PRES_PROHIB_NETWORK_NUMBER:
		privacy = "full";
		screen = "pass";
		break;
	case AST_PRES_NUMBER_NOT_AVAILABLE:
		send_pres_tags = 0;
		break;
	default:
		ast_log(LOG_WARNING, "Unsupported callingpres (%d)\n", p->callingpres);
		if ((p->callingpres & AST_PRES_RESTRICTION) != AST_PRES_ALLOWED)
			privacy = "full";
		else
			privacy = "off";
		screen = "no";
		break;
	}
	
	fromdomain = ast_strlen_zero(p->fromdomain) ? ast_inet_ntoa(iabuf, sizeof(iabuf), p->ourip) : p->fromdomain;

	snprintf(buf, sizeof(buf), "\"%s\" <sip:%s@%s>", clin, clid, fromdomain);
	if (send_pres_tags)
		snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), ";privacy=%s;screen=%s", privacy, screen);
	ast_string_field_set(p, rpid, buf);

	ast_string_field_build(p, rpid_from, "\"%s\" <sip:%s@%s>;tag=%s", clin,
			       ast_strlen_zero(p->fromuser) ? clid : p->fromuser,
			       fromdomain, p->tag);
}

/*! \brief  initreqprep: Initiate new SIP request to peer/user */
static void initreqprep(struct sip_request *req, struct sip_pvt *p, int sipmethod)
{
	char invite_buf[256] = "";
	char *invite = invite_buf;
	size_t invite_max = sizeof(invite_buf);
	char from[256];
	char to[256];
	char tmp[BUFSIZ/2];
	char tmp2[BUFSIZ/2];
	char iabuf[INET_ADDRSTRLEN];
	const char *l = NULL, *n = NULL;
	int x;
	char urioptions[256]="";

	if (ast_test_flag(p, SIP_USEREQPHONE)) {
	 	char onlydigits = 1;
		x=0;

		/* Test p->username against allowed characters in AST_DIGIT_ANY
		If it matches the allowed characters list, then sipuser = ";user=phone"
		If not, then sipuser = ""
        	*/
        	/* + is allowed in first position in a tel: uri */
        	if (p->username && p->username[0] == '+')
			x=1;

		for (; x < strlen(p->username); x++) {
			if (!strchr(AST_DIGIT_ANYNUM, p->username[x])) {
                		onlydigits = 0;
				break;
			}
		}

		/* If we have only digits, add ;user=phone to the uri */
		if (onlydigits)
			strcpy(urioptions, ";user=phone");
	}


	snprintf(p->lastmsg, sizeof(p->lastmsg), "Init: %s", sip_methods[sipmethod].text);

	if (p->owner) {
		l = p->owner->cid.cid_num;
		n = p->owner->cid.cid_name;
	}
	/* if we are not sending RPID and user wants his callerid restricted */
	if (!ast_test_flag(p, SIP_SENDRPID) && ((p->callingpres & AST_PRES_RESTRICTION) != AST_PRES_ALLOWED)) {
		l = CALLERID_UNKNOWN;
		n = l;
	}
	if (!l)
		l = default_callerid;
	if (ast_strlen_zero(n))
		n = l;
	/* Allow user to be overridden */
	if (!ast_strlen_zero(p->fromuser))
		l = p->fromuser;
	else /* Save for any further attempts */
		ast_string_field_set(p, fromuser, l);

	/* Allow user to be overridden */
	if (!ast_strlen_zero(p->fromname))
		n = p->fromname;
	else /* Save for any further attempts */
		ast_string_field_set(p, fromname, n);

	if (pedanticsipchecking) {
		ast_uri_encode(n, tmp, sizeof(tmp), 0);
		n = tmp;
		ast_uri_encode(l, tmp2, sizeof(tmp2), 0);
		l = tmp2;
	}

	if ((ourport != 5060) && ast_strlen_zero(p->fromdomain))	/* Needs to be 5060 */
		snprintf(from, sizeof(from), "\"%s\" <sip:%s@%s:%d>;tag=%s", n, l, ast_strlen_zero(p->fromdomain) ? ast_inet_ntoa(iabuf, sizeof(iabuf), p->ourip) : p->fromdomain, ourport, p->tag);
	else
		snprintf(from, sizeof(from), "\"%s\" <sip:%s@%s>;tag=%s", n, l, ast_strlen_zero(p->fromdomain) ? ast_inet_ntoa(iabuf, sizeof(iabuf), p->ourip) : p->fromdomain, p->tag);

	/* If we're calling a registered SIP peer, use the fullcontact to dial to the peer */
	if (!ast_strlen_zero(p->fullcontact)) {
		/* If we have full contact, trust it */
		ast_build_string(&invite, &invite_max, "%s", p->fullcontact);
	} else {
		/* Otherwise, use the username while waiting for registration */
		ast_build_string(&invite, &invite_max, "sip:");
		if (!ast_strlen_zero(p->username)) {
			n = p->username;
			if (pedanticsipchecking) {
				ast_uri_encode(n, tmp, sizeof(tmp), 0);
				n = tmp;
			}
			ast_build_string(&invite, &invite_max, "%s@", n);
		}
		ast_build_string(&invite, &invite_max, "%s", p->tohost);
		if (ntohs(p->sa.sin_port) != 5060)		/* Needs to be 5060 */
			ast_build_string(&invite, &invite_max, ":%d", ntohs(p->sa.sin_port));
		ast_build_string(&invite, &invite_max, "%s", urioptions);
	}

	/* If custom URI options have been provided, append them */
	if (p->options && p->options->uri_options)
		ast_build_string(&invite, &invite_max, ";%s", p->options->uri_options);
	
	ast_string_field_set(p, uri, invite_buf);

	/* If there is a VXML URL append it to the SIP URL */
	if (p->options && p->options->vxml_url) {
		snprintf(to, sizeof(to), "<%s>;%s", p->uri, p->options->vxml_url);
	} else {
		snprintf(to, sizeof(to), "<%s>", p->uri);
	}
	memset(req, 0, sizeof(struct sip_request));
	init_req(req, sipmethod, p->uri);
	snprintf(tmp, sizeof(tmp), "%d %s", ++p->ocseq, sip_methods[sipmethod].text);

	add_header(req, "Via", p->via);
	/* SLD: FIXME?: do Route: here too?  I think not cos this is the first request.
	 * OTOH, then we won't have anything in p->route anyway */
	/* Build Remote Party-ID and From */
	if (ast_test_flag(p, SIP_SENDRPID) && (sipmethod == SIP_INVITE)) {
		build_rpid(p);
		add_header(req, "From", p->rpid_from);
	} else {
		add_header(req, "From", from);
	}
	add_header(req, "To", to);
	ast_string_field_set(p, exten, l);
	build_contact(p);
	add_header(req, "Contact", p->our_contact);
	add_header(req, "Call-ID", p->callid);
	add_header(req, "CSeq", tmp);
	add_header(req, "User-Agent", global_useragent);
	add_header(req, "Max-Forwards", DEFAULT_MAX_FORWARDS);
	if (!ast_strlen_zero(p->rpid))
		add_header(req, "Remote-Party-ID", p->rpid);
}

/*! \brief  transmit_invite: Build REFER/INVITE/OPTIONS message and transmit it */
static int transmit_invite(struct sip_pvt *p, int sipmethod, int sdp, int init)
{
	struct sip_request req;
	
	req.method = sipmethod;
	if (init) {
		/* Bump branch even on initial requests */
		p->branch ^= thread_safe_rand();
		build_via(p);
		if (init > 1)
			initreqprep(&req, p, sipmethod);
		else
			reqprep(&req, p, sipmethod, 0, 1);
	} else
		reqprep(&req, p, sipmethod, 0, 1);
		
	if (p->options && p->options->auth)
		add_header(&req, p->options->authheader, p->options->auth);
	append_date(&req);
	if (sipmethod == SIP_REFER) {	/* Call transfer */
		if (!ast_strlen_zero(p->refer_to))
			add_header(&req, "Refer-To", p->refer_to);
		if (!ast_strlen_zero(p->referred_by))
			add_header(&req, "Referred-By", p->referred_by);
	}
#ifdef OSP_SUPPORT
	if ((req.method != SIP_OPTIONS) && p->options && !ast_strlen_zero(p->options->osptoken)) {
		ast_log(LOG_DEBUG,"Adding OSP Token: %s\n", p->options->osptoken);
		add_header(&req, "P-OSP-Auth-Token", p->options->osptoken);
	}
#endif
	if (p->options && !ast_strlen_zero(p->options->distinctive_ring))
	{
		add_header(&req, "Alert-Info", p->options->distinctive_ring);
	}
	add_header(&req, "Allow", ALLOWED_METHODS);
	if (p->options && p->options->addsipheaders ) {
		struct ast_channel *ast;
		struct varshead *headp = (struct varshead *) NULL;
		const struct ast_var_t *current;

		ast = p->owner;	/* The owner channel */
		if (ast) {
			char *headdup;
	 		headp = &ast->varshead;
			if (!headp)
				ast_log(LOG_WARNING,"No Headp for the channel...ooops!\n");
			else {
				AST_LIST_TRAVERSE(headp, current, entries) {  
					/* SIPADDHEADER: Add SIP header to outgoing call        */
					if (!strncasecmp(ast_var_name(current), "SIPADDHEADER", strlen("SIPADDHEADER"))) {
						char *content, *end;
						const char *header = ast_var_value(current);

						headdup = ast_strdupa(header);
						/* Strip of the starting " (if it's there) */
						if (*headdup == '"')
					 		headdup++;
						if ((content = strchr(headdup, ':'))) {
							*content = '\0';
							content++;	/* Move pointer ahead */
							/* Skip white space */
							while (*content == ' ')
						  		content++;
							/* Strip the ending " (if it's there) */
					 		end = content + strlen(content) -1;	
							if (*end == '"')
								*end = '\0';
						
							add_header(&req, headdup, content);
							if (sipdebug)
								ast_log(LOG_DEBUG, "Adding SIP Header \"%s\" with content :%s: \n", headdup, content);
						}
					}
				}
			}
		}
	}
	if (sdp && p->rtp) {
		add_sdp(&req, p);
	} else {
		add_header_contentLength(&req, 0);
		add_blank_header(&req);
	}

	if (!p->initreq.headers) {
		/* Use this as the basis */
		copy_request(&p->initreq, &req);
		parse_request(&p->initreq);
		if (sip_debug_test_pvt(p))
			ast_verbose("%d headers, %d lines\n", p->initreq.headers, p->initreq.lines);
	}
	p->lastinvite = p->ocseq;
	return send_request(p, &req, init ? 2 : 1, p->ocseq);
}

/*! \brief  transmit_state_notify: Used in the SUBSCRIBE notification subsystem */
static int transmit_state_notify(struct sip_pvt *p, int state, int full)
{
	char tmp[4000], from[256], to[256];
	char *t = tmp, *c, *a, *mfrom, *mto;
	size_t maxbytes = sizeof(tmp);
	struct sip_request req;
	char hint[AST_MAX_EXTENSION];
	char *statestring = "terminated";
	const struct cfsubscription_types *subscriptiontype;
	enum state { NOTIFY_OPEN, NOTIFY_INUSE, NOTIFY_CLOSED } local_state = NOTIFY_OPEN;
	char *pidfstate = "--";
	char *pidfnote= "Ready";

	memset(from, 0, sizeof(from));
	memset(to, 0, sizeof(to));
	memset(tmp, 0, sizeof(tmp));

	switch (state) {
	case (AST_EXTENSION_RINGING | AST_EXTENSION_INUSE):
		if (global_notifyringing)
			statestring = "early";
		else
			statestring = "confirmed";
		local_state = NOTIFY_INUSE;
		pidfstate = "busy";
		pidfnote = "Ringing";
		break;
	case AST_EXTENSION_RINGING:
		statestring = "early";
		local_state = NOTIFY_INUSE;
		pidfstate = "busy";
		pidfnote = "Ringing";
		break;
	case AST_EXTENSION_INUSE:
		statestring = "confirmed";
		local_state = NOTIFY_INUSE;
		pidfstate = "busy";
		pidfnote = "On the phone";
		break;
	case AST_EXTENSION_BUSY:
		statestring = "confirmed";
		local_state = NOTIFY_CLOSED;
		pidfstate = "busy";
		pidfnote = "On the phone";
		break;
	case AST_EXTENSION_UNAVAILABLE:
		statestring = "confirmed";
		local_state = NOTIFY_CLOSED;
		pidfstate = "away";
		pidfnote = "Unavailable";
		break;
	case AST_EXTENSION_NOT_INUSE:
	default:
		/* Default setting */
		break;
	}

	subscriptiontype = find_subscription_type(p->subscribed);
	
	/* Check which device/devices we are watching  and if they are registered */
	if (ast_get_hint(hint, sizeof(hint), NULL, 0, NULL, p->context, p->exten)) {
		/* If they are not registered, we will override notification and show no availability */
		if (ast_device_state(hint) == AST_DEVICE_UNAVAILABLE) {
			local_state = NOTIFY_CLOSED;
			pidfstate = "away";
			pidfnote = "Not online";
		}
	}

	ast_copy_string(from, get_header(&p->initreq, "From"), sizeof(from));
	c = get_in_brackets(from);
	if (strncmp(c, "sip:", 4)) {
		ast_log(LOG_WARNING, "Huh?  Not a SIP header (%s)?\n", c);
		return -1;
	}
	if ((a = strchr(c, ';')))
		*a = '\0';
	mfrom = c;

	ast_copy_string(to, get_header(&p->initreq, "To"), sizeof(to));
	c = get_in_brackets(to);
	if (strncmp(c, "sip:", 4)) {
		ast_log(LOG_WARNING, "Huh?  Not a SIP header (%s)?\n", c);
		return -1;
	}
	if ((a = strchr(c, ';')))
		*a = '\0';
	mto = c;

	reqprep(&req, p, SIP_NOTIFY, 0, 1);

	
	add_header(&req, "Event", subscriptiontype->event);
	add_header(&req, "Content-Type", subscriptiontype->mediatype);
	switch(state) {
	case AST_EXTENSION_DEACTIVATED:
		if (p->subscribed == TIMEOUT)
			add_header(&req, "Subscription-State", "terminated;reason=timeout");
		else {
			add_header(&req, "Subscription-State", "terminated;reason=probation");
			add_header(&req, "Retry-After", "60");
		}
		break;
	case AST_EXTENSION_REMOVED:
		add_header(&req, "Subscription-State", "terminated;reason=noresource");
		break;
		break;
	default:
		if (p->expiry)
			add_header(&req, "Subscription-State", "active");
		else	/* Expired */
			add_header(&req, "Subscription-State", "terminated;reason=timeout");
	}
	switch (p->subscribed) {
	case XPIDF_XML:
	case CPIM_PIDF_XML:
		ast_build_string(&t, &maxbytes, "<?xml version=\"1.0\"?>\n");
		ast_build_string(&t, &maxbytes, "<!DOCTYPE presence PUBLIC \"-//IETF//DTD RFCxxxx XPIDF 1.0//EN\" \"xpidf.dtd\">\n");
		ast_build_string(&t, &maxbytes, "<presence>\n");
		ast_build_string(&t, &maxbytes, "<presentity uri=\"%s;method=SUBSCRIBE\" />\n", mfrom);
		ast_build_string(&t, &maxbytes, "<atom id=\"%s\">\n", p->exten);
		ast_build_string(&t, &maxbytes, "<address uri=\"%s;user=ip\" priority=\"0.800000\">\n", mto);
		ast_build_string(&t, &maxbytes, "<status status=\"%s\" />\n", (local_state ==  NOTIFY_OPEN) ? "open" : (local_state == NOTIFY_INUSE) ? "inuse" : "closed");
		ast_build_string(&t, &maxbytes, "<msnsubstatus substatus=\"%s\" />\n", (local_state == NOTIFY_OPEN) ? "online" : (local_state == NOTIFY_INUSE) ? "onthephone" : "offline");
		ast_build_string(&t, &maxbytes, "</address>\n</atom>\n</presence>\n");
		break;
	case PIDF_XML: /* Eyebeam supports this format */
		ast_build_string(&t, &maxbytes, "<?xml version=\"1.0\" encoding=\"ISO-8859-1\"?>\n");
		ast_build_string(&t, &maxbytes, "<presence xmlns=\"urn:ietf:params:xml:ns:pidf\" \nxmlns:pp=\"urn:ietf:params:xml:ns:pidf:person\"\nxmlns:es=\"urn:ietf:params:xml:ns:pidf:rpid:status:rpid-status\"\nxmlns:ep=\"urn:ietf:params:xml:ns:pidf:rpid:rpid-person\"\nentity=\"%s\">\n", mfrom);
		ast_build_string(&t, &maxbytes, "<pp:person><status>\n");
		if (pidfstate[0] != '-')
			ast_build_string(&t, &maxbytes, "<ep:activities><ep:%s/></ep:activities>\n", pidfstate);
		ast_build_string(&t, &maxbytes, "</status></pp:person>\n");
		ast_build_string(&t, &maxbytes, "<note>%s</note>\n", pidfnote); /* Note */
		ast_build_string(&t, &maxbytes, "<tuple id=\"%s\">\n", p->exten); /* Tuple start */
		ast_build_string(&t, &maxbytes, "<contact priority=\"1\">%s</contact>\n", mto);
		if (pidfstate[0] == 'b') /* Busy? Still open ... */
			ast_build_string(&t, &maxbytes, "<status><basic>open</basic></status>\n");
		else
			ast_build_string(&t, &maxbytes, "<status><basic>%s</basic></status>\n", (local_state != NOTIFY_CLOSED) ? "open" : "closed");
		ast_build_string(&t, &maxbytes, "</tuple>\n</presence>\n");
		break;
	case DIALOG_INFO_XML: /* SNOM subscribes in this format */
		ast_build_string(&t, &maxbytes, "<?xml version=\"1.0\"?>\n");
		ast_build_string(&t, &maxbytes, "<dialog-info xmlns=\"urn:ietf:params:xml:ns:dialog-info\" version=\"%d\" state=\"%s\" entity=\"%s\">\n", p->dialogver++, full ? "full":"partial", mto);
		if ((state & AST_EXTENSION_RINGING) && global_notifyringing)
			ast_build_string(&t, &maxbytes, "<dialog id=\"%s\" direction=\"recipient\">\n", p->exten);
		else
			ast_build_string(&t, &maxbytes, "<dialog id=\"%s\">\n", p->exten);
		ast_build_string(&t, &maxbytes, "<state>%s</state>\n", statestring);
		ast_build_string(&t, &maxbytes, "</dialog>\n</dialog-info>\n");
		break;
	case NONE:
	default:
		break;
	}

	if (t > tmp + sizeof(tmp))
		ast_log(LOG_WARNING, "Buffer overflow detected!!  (Please file a bug report)\n");

	add_header_contentLength(&req, strlen(tmp));
	add_line(&req, tmp);

	return send_request(p, &req, 1, p->ocseq);
}

/*! \brief  transmit_notify_with_mwi: Notify user of messages waiting in voicemail */
/*      Notification only works for registered peers with mailbox= definitions
 *      in sip.conf
 *      We use the SIP Event package message-summary
 *      MIME type defaults to  "application/simple-message-summary";
 */
static int transmit_notify_with_mwi(struct sip_pvt *p, int newmsgs, int oldmsgs, char *vmexten)
{
	struct sip_request req;
	char tmp[500];
	char *t = tmp;
	size_t maxbytes = sizeof(tmp);
	char iabuf[INET_ADDRSTRLEN];

	initreqprep(&req, p, SIP_NOTIFY);
	add_header(&req, "Event", "message-summary");
	add_header(&req, "Content-Type", default_notifymime);

	ast_build_string(&t, &maxbytes, "Messages-Waiting: %s\r\n", newmsgs ? "yes" : "no");
	ast_build_string(&t, &maxbytes, "Message-Account: sip:%s@%s\r\n", !ast_strlen_zero(vmexten) ? vmexten : default_vmexten, ast_strlen_zero(p->fromdomain) ? ast_inet_ntoa(iabuf, sizeof(iabuf), p->ourip) : p->fromdomain);
	ast_build_string(&t, &maxbytes, "Voice-Message: %d/%d (0/0)\r\n", newmsgs, oldmsgs);

	if (t > tmp + sizeof(tmp))
		ast_log(LOG_WARNING, "Buffer overflow detected!!  (Please file a bug report)\n");

	add_header_contentLength(&req, strlen(tmp));
	add_line(&req, tmp);

	if (!p->initreq.headers) { /* Use this as the basis */
		copy_request(&p->initreq, &req);
		parse_request(&p->initreq);
		if (sip_debug_test_pvt(p))
			ast_verbose("%d headers, %d lines\n", p->initreq.headers, p->initreq.lines);
		determine_firstline_parts(&p->initreq);
	}

	return send_request(p, &req, 1, p->ocseq);
}

/*! \brief  transmit_sip_request: Transmit SIP request */
static int transmit_sip_request(struct sip_pvt *p,struct sip_request *req)
{
	if (!p->initreq.headers) {
		/* Use this as the basis */
		copy_request(&p->initreq, req);
		parse_request(&p->initreq);
		if (sip_debug_test_pvt(p))
			ast_verbose("%d headers, %d lines\n", p->initreq.headers, p->initreq.lines);
		determine_firstline_parts(&p->initreq);
	}

	return send_request(p, req, 0, p->ocseq);
}

/*! \brief  transmit_notify_with_sipfrag: Notify a transferring party of the status of trasnfer */
/*      Apparently the draft SIP REFER structure was too simple, so it was decided that the
 *      status of transfers also needed to be sent via NOTIFY instead of just the 202 Accepted
 *      that had worked heretofore.
 */
static int transmit_notify_with_sipfrag(struct sip_pvt *p, int cseq)
{
	struct sip_request req;
	char tmp[20];
	reqprep(&req, p, SIP_NOTIFY, 0, 1);
	snprintf(tmp, sizeof(tmp), "refer;id=%d", cseq);
	add_header(&req, "Event", tmp);
	add_header(&req, "Subscription-state", "terminated;reason=noresource");
	add_header(&req, "Content-Type", "message/sipfrag;version=2.0");

	strcpy(tmp, "SIP/2.0 200 OK");
	add_header_contentLength(&req, strlen(tmp));
	add_line(&req, tmp);

	if (!p->initreq.headers) {
		/* Use this as the basis */
		copy_request(&p->initreq, &req);
		parse_request(&p->initreq);
		if (sip_debug_test_pvt(p))
			ast_verbose("%d headers, %d lines\n", p->initreq.headers, p->initreq.lines);
		determine_firstline_parts(&p->initreq);
	}

	return send_request(p, &req, 1, p->ocseq);
}

static char *regstate2str(int regstate)
{
	switch(regstate) {
	case REG_STATE_FAILED:
		return "Failed";
	case REG_STATE_UNREGISTERED:
		return "Unregistered";
	case REG_STATE_REGSENT:
		return "Request Sent";
	case REG_STATE_AUTHSENT:
		return "Auth. Sent";
	case REG_STATE_REGISTERED:
		return "Registered";
	case REG_STATE_REJECTED:
		return "Rejected";
	case REG_STATE_TIMEOUT:
		return "Timeout";
	case REG_STATE_NOAUTH:
		return "No Authentication";
	default:
		return "Unknown";
	}
}

static int transmit_register(struct sip_registry *r, int sipmethod, char *auth, char *authheader);

/*! \brief  sip_reregister: Update registration with SIP Proxy*/
static int sip_reregister(void *data) 
{
	/* if we are here, we know that we need to reregister. */
	struct sip_registry *r= ASTOBJ_REF((struct sip_registry *) data);

	/* if we couldn't get a reference to the registry object, punt */
	if (!r)
		return 0;

	if (r->call && recordhistory)
		append_history(r->call, "RegistryRenew", "Account: %s@%s", r->username, r->hostname);
	/* Since registry's are only added/removed by the the monitor thread, this
	   may be overkill to reference/dereference at all here */
	if (sipdebug)
		ast_log(LOG_NOTICE, "   -- Re-registration for  %s@%s\n", r->username, r->hostname);

	r->expire = -1;
	__sip_do_register(r);
	ASTOBJ_UNREF(r, sip_registry_destroy);
	return 0;
}

/*! \brief  __sip_do_register: Register with SIP proxy */
static int __sip_do_register(struct sip_registry *r)
{
	int res;

	res = transmit_register(r, SIP_REGISTER, NULL, NULL);
	return res;
}

/*! \brief  sip_reg_timeout: Registration timeout, register again */
static int sip_reg_timeout(void *data)
{

	/* if we are here, our registration timed out, so we'll just do it over */
	struct sip_registry *r = ASTOBJ_REF((struct sip_registry *) data);
	struct sip_pvt *p;
	int res;

	/* if we couldn't get a reference to the registry object, punt */
	if (!r)
		return 0;

	ast_log(LOG_NOTICE, "   -- Registration for '%s@%s' timed out, trying again (Attempt #%d)\n", r->username, r->hostname, r->regattempts); 
	if (r->call) {
		/* Unlink us, destroy old call.  Locking is not relevant here because all this happens
		   in the single SIP manager thread. */
		p = r->call;
		if (p->registry)
			ASTOBJ_UNREF(p->registry, sip_registry_destroy);
		r->call = NULL;
		ast_set_flag(p, SIP_NEEDDESTROY);	
		/* Pretend to ACK anything just in case */
		__sip_pretend_ack(p);
	}
	/* If we have a limit, stop registration and give up */
	if (global_regattempts_max && (r->regattempts > global_regattempts_max)) {
		/* Ok, enough is enough. Don't try any more */
		/* We could add an external notification here... 
			steal it from app_voicemail :-) */
		ast_log(LOG_NOTICE, "   -- Giving up forever trying to register '%s@%s'\n", r->username, r->hostname);
		r->regstate = REG_STATE_FAILED;
	} else {
		r->regstate = REG_STATE_UNREGISTERED;
		r->timeout = -1;
		res=transmit_register(r, SIP_REGISTER, NULL, NULL);
	}
	manager_event(EVENT_FLAG_SYSTEM, "Registry", "Channel: SIP\r\nUsername: %s\r\nDomain: %s\r\nStatus: %s\r\n", r->username, r->hostname, regstate2str(r->regstate));
	ASTOBJ_UNREF(r, sip_registry_destroy);
	return 0;
}

/*! \brief  transmit_register: Transmit register to SIP proxy or UA */
static int transmit_register(struct sip_registry *r, int sipmethod, char *auth, char *authheader)
{
	struct sip_request req;
	char from[256];
	char to[256];
	char tmp[80];
	char addr[80];
	struct sip_pvt *p;

	/* exit if we are already in process with this registrar ?*/
	if ( r == NULL || ((auth==NULL) && (r->regstate==REG_STATE_REGSENT || r->regstate==REG_STATE_AUTHSENT))) {
		ast_log(LOG_NOTICE, "Strange, trying to register %s@%s when registration already pending\n", r->username, r->hostname);
		return 0;
	}

	if (r->call) {	/* We have a registration */
		if (!auth) {
			ast_log(LOG_WARNING, "Already have a REGISTER going on to %s@%s?? \n", r->username, r->hostname);
			return 0;
		} else {
			p = r->call;
			make_our_tag(p->tag, sizeof(p->tag));	/* create a new local tag for every register attempt */
			ast_string_field_free(p, theirtag);	/* forget their old tag, so we don't match tags when getting response */
		}
	} else {
		/* Build callid for registration if we haven't registered before */
		if (!r->callid_valid) {
			build_callid_registry(r, __ourip, default_fromdomain);
			r->callid_valid = 1;
		}
		/* Allocate SIP packet for registration */
		p=sip_alloc( r->callid, NULL, 0, SIP_REGISTER);
		if (!p) {
			ast_log(LOG_WARNING, "Unable to allocate registration call\n");
			return 0;
		}
		if (recordhistory)
			append_history(p, "RegistryInit", "Account: %s@%s", r->username, r->hostname);
		/* Find address to hostname */
		if (create_addr(p, r->hostname)) {
			/* we have what we hope is a temporary network error,
			 * probably DNS.  We need to reschedule a registration try */
			sip_destroy(p);
			if (r->timeout > -1) {
				ast_sched_del(sched, r->timeout);
				r->timeout = ast_sched_add(sched, global_reg_timeout*1000, sip_reg_timeout, r);
				ast_log(LOG_WARNING, "Still have a registration timeout for %s@%s (create_addr() error), %d\n", r->username, r->hostname, r->timeout);
			} else {
				r->timeout = ast_sched_add(sched, global_reg_timeout*1000, sip_reg_timeout, r);
				ast_log(LOG_WARNING, "Probably a DNS error for registration to %s@%s, trying REGISTER again (after %d seconds)\n", r->username, r->hostname, global_reg_timeout);
			}
			r->regattempts++;
			return 0;
		}
		/* Copy back Call-ID in case create_addr changed it */
		ast_string_field_set(r, callid, p->callid);
		if (r->portno)
			p->sa.sin_port = htons(r->portno);
		ast_set_flag(p, SIP_OUTGOING);	/* Registration is outgoing call */
		r->call=p;			/* Save pointer to SIP packet */
		p->registry = ASTOBJ_REF(r);	/* Add pointer to registry in packet */
		if (!ast_strlen_zero(r->secret))	/* Secret (password) */
			ast_string_field_set(p, peersecret, r->secret);
		if (!ast_strlen_zero(r->md5secret))
			ast_string_field_set(p, peermd5secret, r->md5secret);
		/* User name in this realm  
		- if authuser is set, use that, otherwise use username */
		if (!ast_strlen_zero(r->authuser)) {	
			ast_string_field_set(p, peername, r->authuser);
			ast_string_field_set(p, authname, r->authuser);
		} else {
			if (!ast_strlen_zero(r->username)) {
				ast_string_field_set(p, peername, r->username);
				ast_string_field_set(p, authname, r->username);
				ast_string_field_set(p, fromuser, r->username);
			}
		}
		if (!ast_strlen_zero(r->username))
			ast_string_field_set(p, username, r->username);
		/* Save extension in packet */
		ast_string_field_set(p, exten, r->contact);

		/*
		  check which address we should use in our contact header 
		  based on whether the remote host is on the external or
		  internal network so we can register through nat
		 */
		if (ast_sip_ouraddrfor(&p->sa.sin_addr, &p->ourip))
			memcpy(&p->ourip, &bindaddr.sin_addr, sizeof(p->ourip));
		build_contact(p);
	}

	/* set up a timeout */
	if (auth == NULL)  {
		if (r->timeout > -1) {
			ast_log(LOG_WARNING, "Still have a registration timeout, #%d - deleting it\n", r->timeout);
			ast_sched_del(sched, r->timeout);
		}
		r->timeout = ast_sched_add(sched, global_reg_timeout * 1000, sip_reg_timeout, r);
		ast_log(LOG_DEBUG, "Scheduled a registration timeout for %s id  #%d \n", r->hostname, r->timeout);
	}

	if (strchr(r->username, '@')) {
		snprintf(from, sizeof(from), "<sip:%s>;tag=%s", r->username, p->tag);
		if (!ast_strlen_zero(p->theirtag))
			snprintf(to, sizeof(to), "<sip:%s>;tag=%s", r->username, p->theirtag);
		else
			snprintf(to, sizeof(to), "<sip:%s>", r->username);
	} else {
		snprintf(from, sizeof(from), "<sip:%s@%s>;tag=%s", r->username, p->tohost, p->tag);
		if (!ast_strlen_zero(p->theirtag))
			snprintf(to, sizeof(to), "<sip:%s@%s>;tag=%s", r->username, p->tohost, p->theirtag);
		else
			snprintf(to, sizeof(to), "<sip:%s@%s>", r->username, p->tohost);
	}
	
	/* Fromdomain is what we are registering to, regardless of actual
	   host name from SRV */
	if (!ast_strlen_zero(p->fromdomain))
		snprintf(addr, sizeof(addr), "sip:%s", p->fromdomain);
	else
		snprintf(addr, sizeof(addr), "sip:%s", r->hostname);
	ast_string_field_set(p, uri, addr);

	p->branch ^= thread_safe_rand();

	memset(&req, 0, sizeof(req));
	init_req(&req, sipmethod, addr);

	/* Add to CSEQ */
	snprintf(tmp, sizeof(tmp), "%u %s", ++r->ocseq, sip_methods[sipmethod].text);
	p->ocseq = r->ocseq;

	build_via(p);
	add_header(&req, "Via", p->via);
	add_header(&req, "From", from);
	add_header(&req, "To", to);
	add_header(&req, "Call-ID", p->callid);
	add_header(&req, "CSeq", tmp);
	add_header(&req, "User-Agent", global_useragent);
	add_header(&req, "Max-Forwards", DEFAULT_MAX_FORWARDS);

	
	if (auth) 	/* Add auth header */
		add_header(&req, authheader, auth);
	else if (!ast_strlen_zero(r->nonce)) {
		char digest[1024];

		/* We have auth data to reuse, build a digest header! */
		if (sipdebug)
			ast_log(LOG_DEBUG, "   >>> Re-using Auth data for %s@%s\n", r->username, r->hostname);
		ast_string_field_set(p, realm, r->realm);
		ast_string_field_set(p, nonce, r->nonce);
		ast_string_field_set(p, domain, r->domain);
		ast_string_field_set(p, opaque, r->opaque);
		ast_string_field_set(p, qop, r->qop);
		p->noncecount = r->noncecount++;

		memset(digest,0,sizeof(digest));
		if(!build_reply_digest(p, sipmethod, digest, sizeof(digest)))
			add_header(&req, "Authorization", digest);
		else
			ast_log(LOG_NOTICE, "No authorization available for authentication of registration to %s@%s\n", r->username, r->hostname);
	
	}

	snprintf(tmp, sizeof(tmp), "%d", default_expiry);
	add_header(&req, "Expires", tmp);
	add_header(&req, "Contact", p->our_contact);
	add_header(&req, "Event", "registration");
	add_header_contentLength(&req, 0);
	add_blank_header(&req);
	copy_request(&p->initreq, &req);
	parse_request(&p->initreq);
	if (sip_debug_test_pvt(p)) {
		ast_verbose("REGISTER %d headers, %d lines\n", p->initreq.headers, p->initreq.lines);
	}
	determine_firstline_parts(&p->initreq);
	r->regstate=auth?REG_STATE_AUTHSENT:REG_STATE_REGSENT;
	r->regattempts++;	/* Another attempt */
	if (option_debug > 3)
		ast_verbose("REGISTER attempt %d to %s@%s\n", r->regattempts, r->username, r->hostname);
	return send_request(p, &req, 2, p->ocseq);
}

/*! \brief  transmit_message_with_text: Transmit text with SIP MESSAGE method */
static int transmit_message_with_text(struct sip_pvt *p, const char *text)
{
	struct sip_request req;
	reqprep(&req, p, SIP_MESSAGE, 0, 1);
	add_text(&req, text);
	return send_request(p, &req, 1, p->ocseq);
}

/*! \brief  transmit_refer: Transmit SIP REFER message */
static int transmit_refer(struct sip_pvt *p, const char *dest)
{
	struct sip_request req;
	char from[256];
	char *of, *c;
	char referto[256];

	if (ast_test_flag(p, SIP_OUTGOING)) 
		of = get_header(&p->initreq, "To");
	else
		of = get_header(&p->initreq, "From");
	ast_copy_string(from, of, sizeof(from));
	of = get_in_brackets(from);
	ast_string_field_set(p, from, of);
	if (strncmp(of, "sip:", 4)) {
		ast_log(LOG_NOTICE, "From address missing 'sip:', using it anyway\n");
	} else
		of += 4;
	/* Get just the username part */
	if ((c = strchr(dest, '@'))) {
		c = NULL;
	} else if ((c = strchr(of, '@'))) {
		*c = '\0';
		c++;
	}
	if (c) {
		snprintf(referto, sizeof(referto), "<sip:%s@%s>", dest, c);
	} else {
		snprintf(referto, sizeof(referto), "<sip:%s>", dest);
	}

	/* save in case we get 407 challenge */
	ast_string_field_set(p, refer_to, referto);
	ast_string_field_set(p, referred_by, p->our_contact);

	reqprep(&req, p, SIP_REFER, 0, 1);
	add_header(&req, "Refer-To", referto);
	if (!ast_strlen_zero(p->our_contact))
		add_header(&req, "Referred-By", p->our_contact);
	add_blank_header(&req);
	return send_request(p, &req, 1, p->ocseq);
}

/*! \brief  transmit_info_with_digit: Send SIP INFO dtmf message, see Cisco documentation on cisco.co
m **/
static int transmit_info_with_digit(struct sip_pvt *p, char digit)
{
	struct sip_request req;
	reqprep(&req, p, SIP_INFO, 0, 1);
	add_digit(&req, digit);
	return send_request(p, &req, 1, p->ocseq);
}

/*! \brief  transmit_info_with_vidupdate: Send SIP INFO with video update request */
static int transmit_info_with_vidupdate(struct sip_pvt *p)
{
	struct sip_request req;
	reqprep(&req, p, SIP_INFO, 0, 1);
	add_vidupdate(&req);
	return send_request(p, &req, 1, p->ocseq);
}

/*! \brief  transmit_request: transmit generic SIP request */
static int transmit_request(struct sip_pvt *p, int sipmethod, int seqno, int reliable, int newbranch)
{
	struct sip_request resp;
	reqprep(&resp, p, sipmethod, seqno, newbranch);
	add_header_contentLength(&resp, 0);
	add_blank_header(&resp);
	return send_request(p, &resp, reliable, seqno ? seqno : p->ocseq);
}

/*! \brief  transmit_request_with_auth: Transmit SIP request, auth added */
static int transmit_request_with_auth(struct sip_pvt *p, int sipmethod, int seqno, int reliable, int newbranch)
{
	struct sip_request resp;

	reqprep(&resp, p, sipmethod, seqno, newbranch);
	if (!ast_strlen_zero(p->realm)) {
		char digest[1024];

		memset(digest, 0, sizeof(digest));
		if(!build_reply_digest(p, sipmethod, digest, sizeof(digest))) {
			if (p->options && p->options->auth_type == PROXY_AUTH)
				add_header(&resp, "Proxy-Authorization", digest);
			else if (p->options && p->options->auth_type == WWW_AUTH)
				add_header(&resp, "Authorization", digest);
			else	/* Default, to be backwards compatible (maybe being too careful, but leaving it for now) */
				add_header(&resp, "Proxy-Authorization", digest);
		} else
			ast_log(LOG_WARNING, "No authentication available for call %s\n", p->callid);
	}
	/* If we are hanging up and know a cause for that, send it in clear text to make
		debugging easier. */
	if (sipmethod == SIP_BYE) {
		if (p->owner && p->owner->hangupcause)	{
			add_header(&resp, "X-Asterisk-HangupCause", ast_cause2str(p->owner->hangupcause));
		}
	}

	add_header_contentLength(&resp, 0);
	add_blank_header(&resp);
	return send_request(p, &resp, reliable, seqno ? seqno : p->ocseq);	
}

static void destroy_association(struct sip_peer *peer)
{
	if (!ast_test_flag((&global_flags_page2), SIP_PAGE2_IGNOREREGEXPIRE)) {
		if (ast_test_flag(&(peer->flags_page2), SIP_PAGE2_RT_FROMCONTACT)) {
			ast_update_realtime("sippeers", "name", peer->name, "fullcontact", "", "ipaddr", "", "port", "", "regseconds", "0", "username", "", NULL);
		} else {
			ast_db_del("SIP/Registry", peer->name);
		}
	}
}

/*! \brief  expire_register: Expire registration of SIP peer */
static int expire_register(void *data)
{
	struct sip_peer *peer = data;

	memset(&peer->addr, 0, sizeof(peer->addr));

	destroy_association(peer);
	
	manager_event(EVENT_FLAG_SYSTEM, "PeerStatus", "Peer: SIP/%s\r\nPeerStatus: Unregistered\r\nCause: Expired\r\n", peer->name);
	register_peer_exten(peer, 0);
	peer->expire = -1;
	ast_device_state_changed("SIP/%s", peer->name);
	if (ast_test_flag(peer, SIP_SELFDESTRUCT) || ast_test_flag((&peer->flags_page2), SIP_PAGE2_RTAUTOCLEAR)) {
		peer = ASTOBJ_CONTAINER_UNLINK(&peerl, peer);
		ASTOBJ_UNREF(peer, sip_destroy_peer);
	}

	return 0;
}

static int sip_poke_peer(struct sip_peer *peer);

static int sip_poke_peer_s(void *data)
{
	struct sip_peer *peer = data;
	peer->pokeexpire = -1;
	sip_poke_peer(peer);
	return 0;
}

/*! \brief  reg_source_db: Get registration details from Asterisk DB */
static void reg_source_db(struct sip_peer *peer)
{
	char data[256];
	char iabuf[INET_ADDRSTRLEN];
	struct in_addr in;
	int expiry;
	int port;
	char *scan, *addr, *port_str, *expiry_str, *username, *contact;

	if (ast_test_flag(&(peer->flags_page2), SIP_PAGE2_RT_FROMCONTACT)) 
		return;
	if (ast_db_get("SIP/Registry", peer->name, data, sizeof(data)))
		return;

	scan = data;
	addr = strsep(&scan, ":");
	port_str = strsep(&scan, ":");
	expiry_str = strsep(&scan, ":");
	username = strsep(&scan, ":");
	contact = scan;	/* Contact include sip: and has to be the last part of the database entry as long as we use : as a separator */

	if (!inet_aton(addr, &in))
		return;

	if (port_str)
		port = atoi(port_str);
	else
		return;

	if (expiry_str)
		expiry = atoi(expiry_str);
	else
		return;

	if (username)
		ast_copy_string(peer->username, username, sizeof(peer->username));
	if (contact)
		ast_copy_string(peer->fullcontact, contact, sizeof(peer->fullcontact));

	if (option_verbose > 2)
		ast_verbose(VERBOSE_PREFIX_3 "SIP Seeding peer from astdb: '%s' at %s@%s:%d for %d\n",
			    peer->name, peer->username, ast_inet_ntoa(iabuf, sizeof(iabuf), in), port, expiry);

	memset(&peer->addr, 0, sizeof(peer->addr));
	peer->addr.sin_family = AF_INET;
	peer->addr.sin_addr = in;
	peer->addr.sin_port = htons(port);
	if (sipsock < 0) {
		/* SIP isn't up yet, so schedule a poke only, pretty soon */
		if (peer->pokeexpire > -1)
			ast_sched_del(sched, peer->pokeexpire);
		peer->pokeexpire = ast_sched_add(sched, thread_safe_rand() % 5000 + 1, sip_poke_peer_s, peer);
	} else
		sip_poke_peer(peer);
	if (peer->expire > -1)
		ast_sched_del(sched, peer->expire);
	peer->expire = ast_sched_add(sched, (expiry + 10) * 1000, expire_register, peer);
	register_peer_exten(peer, 1);
}

/*! \brief  parse_ok_contact: Parse contact header for 200 OK on INVITE */
static int parse_ok_contact(struct sip_pvt *pvt, struct sip_request *req)
{
	char contact[250]; 
	char *c, *n, *pt;
	int port;
	struct hostent *hp;
	struct ast_hostent ahp;
	struct sockaddr_in oldsin;

	/* Look for brackets */
	ast_copy_string(contact, get_header(req, "Contact"), sizeof(contact));
	c = get_in_brackets(contact);

	/* Save full contact to call pvt for later bye or re-invite */
	ast_string_field_set(pvt, fullcontact, c);

	/* Save URI for later ACKs, BYE or RE-invites */
	ast_string_field_set(pvt, okcontacturi, c);
	
	/* Make sure it's a SIP URL */
	if (strncasecmp(c, "sip:", 4)) {
		ast_log(LOG_NOTICE, "'%s' is not a valid SIP contact (missing sip:) trying to use anyway\n", c);
	} else
		c += 4;

	/* Ditch arguments */
	n = strchr(c, ';');
	if (n) 
		*n = '\0';

	/* Grab host */
	n = strchr(c, '@');
	if (!n) {
		n = c;
		c = NULL;
	} else {
		*n = '\0';
		n++;
	}
	pt = strchr(n, ':');
	if (pt) {
		*pt = '\0';
		pt++;
		port = atoi(pt);
	} else
		port = DEFAULT_SIP_PORT;

	memcpy(&oldsin, &pvt->sa, sizeof(oldsin));

	if (!(ast_test_flag(pvt, SIP_NAT) & SIP_NAT_ROUTE)) {
		/* XXX This could block for a long time XXX */
		/* We should only do this if it's a name, not an IP */
		hp = ast_gethostbyname(n, &ahp);
		if (!hp)  {
			ast_log(LOG_WARNING, "Invalid host '%s'\n", n);
			return -1;
		}
		pvt->sa.sin_family = AF_INET;
		memcpy(&pvt->sa.sin_addr, hp->h_addr, sizeof(pvt->sa.sin_addr));
		pvt->sa.sin_port = htons(port);
	} else {
		/* Don't trust the contact field.  Just use what they came to us
		   with. */
		memcpy(&pvt->sa, &pvt->recv, sizeof(pvt->sa));
	}
	return 0;
}


enum parse_register_result {
	PARSE_REGISTER_FAILED,
	PARSE_REGISTER_UPDATE,
	PARSE_REGISTER_QUERY,
};

/*! \brief  parse_register_contact: Parse contact header and save registration */
static enum parse_register_result parse_register_contact(struct sip_pvt *pvt, struct sip_peer *p, struct sip_request *req)
{
	char contact[80]; 
	char data[256];
	char iabuf[INET_ADDRSTRLEN];
	char *expires = get_header(req, "Expires");
	int expiry = atoi(expires);
	char *c, *n, *pt;
	int port;
	char *useragent;
	struct hostent *hp;
	struct ast_hostent ahp;
	struct sockaddr_in oldsin;

	if (ast_strlen_zero(expires)) {	/* No expires header */
		expires = strcasestr(get_header(req, "Contact"), ";expires=");
		if (expires) {
			char *ptr;
			if ((ptr = strchr(expires, ';')))
				*ptr = '\0';
			if (sscanf(expires + 9, "%d", &expiry) != 1)
				expiry = default_expiry;
		} else {
			/* Nothing has been specified */
			expiry = default_expiry;
		}
	}
	/* Look for brackets */
	ast_copy_string(contact, get_header(req, "Contact"), sizeof(contact));
	if (strchr(contact, '<') == NULL) {	/* No <, check for ; and strip it */
		char *ptr = strchr(contact, ';');	/* This is Header options, not URI options */
		if (ptr)
			*ptr = '\0';
	}
	c = get_in_brackets(contact);

	/* if they did not specify Contact: or Expires:, they are querying
	   what we currently have stored as their contact address, so return
	   it
	*/
	if (ast_strlen_zero(c) && ast_strlen_zero(expires)) {
		if ((p->expire > -1) && !ast_strlen_zero(p->fullcontact)) {
			/* tell them when the registration is going to expire */
			pvt->expiry = ast_sched_when(sched, p->expire);
			return PARSE_REGISTER_QUERY;
		} else {
			return PARSE_REGISTER_FAILED;
		}
	} else if (!strcasecmp(c, "*") || !expiry) {	/* Unregister this peer */
		/* This means remove all registrations and return OK */
		memset(&p->addr, 0, sizeof(p->addr));
		if (p->expire > -1)
			ast_sched_del(sched, p->expire);
		p->expire = -1;

		destroy_association(p);
		
		register_peer_exten(p, 0);
		p->fullcontact[0] = '\0';
		p->useragent[0] = '\0';
		p->sipoptions = 0;
		p->lastms = 0;

		if (option_verbose > 2)
			ast_verbose(VERBOSE_PREFIX_3 "Unregistered SIP '%s'\n", p->name);
			manager_event(EVENT_FLAG_SYSTEM, "PeerStatus", "Peer: SIP/%s\r\nPeerStatus: Unregistered\r\n", p->name);
		return PARSE_REGISTER_UPDATE;
	}
	ast_copy_string(p->fullcontact, c, sizeof(p->fullcontact));
	/* For the 200 OK, we should use the received contact */
	ast_string_field_build(pvt, our_contact, "<%s>", c);
	/* Make sure it's a SIP URL */
	if (strncasecmp(c, "sip:", 4)) {
		ast_log(LOG_NOTICE, "'%s' is not a valid SIP contact (missing sip:) trying to use anyway\n", c);
	} else
		c += 4;
	/* Ditch q */
	n = strchr(c, ';');
	if (n) {
		*n = '\0';
	}
	/* Grab host */
	n = strchr(c, '@');
	if (!n) {
		n = c;
		c = NULL;
	} else {
		*n = '\0';
		n++;
	}
	pt = strchr(n, ':');
	if (pt) {
		*pt = '\0';
		pt++;
		port = atoi(pt);
	} else
		port = DEFAULT_SIP_PORT;
	memcpy(&oldsin, &p->addr, sizeof(oldsin));
	if (!(ast_test_flag(p, SIP_NAT) & SIP_NAT_ROUTE)) {
		/* XXX This could block for a long time XXX */
		hp = ast_gethostbyname(n, &ahp);
		if (!hp)  {
			ast_log(LOG_WARNING, "Invalid host '%s'\n", n);
			return PARSE_REGISTER_FAILED;
		}
		p->addr.sin_family = AF_INET;
		memcpy(&p->addr.sin_addr, hp->h_addr, sizeof(p->addr.sin_addr));
		p->addr.sin_port = htons(port);
	} else {
		/* Don't trust the contact field.  Just use what they came to us
		   with */
		memcpy(&p->addr, &pvt->recv, sizeof(p->addr));
	}

	if (c)	/* Overwrite the default username from config at registration */
		ast_copy_string(p->username, c, sizeof(p->username));
	else
		p->username[0] = '\0';

	if (p->expire > -1)
		ast_sched_del(sched, p->expire);
	if (expiry > max_expiry)
		expiry = max_expiry;
	if (expiry < min_expiry)
		expiry = min_expiry;
	if (!ast_test_flag(p, SIP_REALTIME))
		p->expire = ast_sched_add(sched, (expiry + 10) * 1000, expire_register, p);
	else
		p->expire = -1;
	pvt->expiry = expiry;
	snprintf(data, sizeof(data), "%s:%d:%d:%s:%s", ast_inet_ntoa(iabuf, sizeof(iabuf), p->addr.sin_addr), ntohs(p->addr.sin_port), expiry, p->username, p->fullcontact);
	if (!ast_test_flag((&p->flags_page2), SIP_PAGE2_RT_FROMCONTACT)) 
		ast_db_put("SIP/Registry", p->name, data);
	manager_event(EVENT_FLAG_SYSTEM, "PeerStatus", "Peer: SIP/%s\r\nPeerStatus: Registered\r\n", p->name);
	if (inaddrcmp(&p->addr, &oldsin)) {
		sip_poke_peer(p);
		if (option_verbose > 2)
			ast_verbose(VERBOSE_PREFIX_3 "Registered SIP '%s' at %s port %d expires %d\n", p->name, ast_inet_ntoa(iabuf, sizeof(iabuf), p->addr.sin_addr), ntohs(p->addr.sin_port), expiry);
		register_peer_exten(p, 1);
	}
	
	/* Save SIP options profile */
	p->sipoptions = pvt->sipoptions;

	/* Save User agent */
	useragent = get_header(req, "User-Agent");
	if (useragent && strcasecmp(useragent, p->useragent)) {
		ast_copy_string(p->useragent, useragent, sizeof(p->useragent));
		if (option_verbose > 3) {
			ast_verbose(VERBOSE_PREFIX_3 "Saved useragent \"%s\" for peer %s\n",p->useragent,p->name);  
		}
	}
	return PARSE_REGISTER_UPDATE;
}

/*! \brief  free_old_route: Remove route from route list */
static void free_old_route(struct sip_route *route)
{
	struct sip_route *next;
	while (route) {
		next = route->next;
		free(route);
		route = next;
	}
}

/*! \brief  list_route: List all routes - mostly for debugging */
static void list_route(struct sip_route *route)
{
	if (!route) {
		ast_verbose("list_route: no route\n");
		return;
	}
	while (route) {
		ast_verbose("list_route: hop: <%s>\n", route->hop);
		route = route->next;
	}
}

/*! \brief  build_route: Build route list from Record-Route header */
static void build_route(struct sip_pvt *p, struct sip_request *req, int backwards)
{
	struct sip_route *thishop, *head, *tail;
	int start = 0;
	int len;
	char *rr, *contact, *c;

	/* Once a persistant route is set, don't fool with it */
	if (p->route && p->route_persistant) {
		ast_log(LOG_DEBUG, "build_route: Retaining previous route: <%s>\n", p->route->hop);
		return;
	}

	if (p->route) {
		free_old_route(p->route);
		p->route = NULL;
	}
	
	p->route_persistant = backwards;
	
	/* We build up head, then assign it to p->route when we're done */
	head = NULL;  tail = head;
	/* 1st we pass through all the hops in any Record-Route headers */
	for (;;) {
		/* Each Record-Route header */
		rr = __get_header(req, "Record-Route", &start);
		if (*rr == '\0') break;
		for (;;) {
			/* Each route entry */
			/* Find < */
			rr = strchr(rr, '<');
			if (!rr) 
				break; /* No more hops */
			++rr;
			len = strcspn(rr, ">") + 1;
			/* Make a struct route */
			if ((thishop = ast_malloc(sizeof(*thishop) + len))) {
				/* ast_calloc is not needed because all fields are initialized in this block */
				ast_copy_string(thishop->hop, rr, len);
				ast_log(LOG_DEBUG, "build_route: Record-Route hop: <%s>\n", thishop->hop);
				/* Link in */
				if (backwards) {
					/* Link in at head so they end up in reverse order */
					thishop->next = head;
					head = thishop;
					/* If this was the first then it'll be the tail */
					if (!tail) tail = thishop;
				} else {
					thishop->next = NULL;
					/* Link in at the end */
					if (tail)
						tail->next = thishop;
					else
						head = thishop;
					tail = thishop;
				}
			}
			rr += len;
		}
	}

	/* Only append the contact if we are dealing with a strict router */
	if (!head || (!ast_strlen_zero(head->hop) && strstr(head->hop,";lr") == NULL) ) {
		/* 2nd append the Contact: if there is one */
		/* Can be multiple Contact headers, comma separated values - we just take the first */
		contact = get_header(req, "Contact");
		if (!ast_strlen_zero(contact)) {
			ast_log(LOG_DEBUG, "build_route: Contact hop: %s\n", contact);
			/* Look for <: delimited address */
			c = strchr(contact, '<');
			if (c) {
				/* Take to > */
				++c;
				len = strcspn(c, ">") + 1;
			} else {
				/* No <> - just take the lot */
				c = contact;
				len = strlen(contact) + 1;
			}
			if ((thishop = ast_malloc(sizeof(*thishop) + len))) {
				/* ast_calloc is not needed because all fields are initialized in this block */
				ast_copy_string(thishop->hop, c, len);
				thishop->next = NULL;
				/* Goes at the end */
				if (tail)
					tail->next = thishop;
				else
					head = thishop;
			}
		}
	}

	/* Store as new route */
	p->route = head;

	/* For debugging dump what we ended up with */
	if (sip_debug_test_pvt(p))
		list_route(p->route);
}

#ifdef OSP_SUPPORT
/*! \brief  check_osptoken: Validate OSP token for user authrroization */
static int check_osptoken (struct sip_pvt *p, char *token)
{
	char tmp[80];

	if (ast_osp_validate (NULL, token, &p->osphandle, &p->osptimelimit, p->cid_num, p->sa.sin_addr, p->exten) < 1) {
		return (-1);
	} else {
		snprintf (tmp, sizeof (tmp), "%d", p->osphandle);
		pbx_builtin_setvar_helper (p->owner, "_OSPHANDLE", tmp);
		return (0);
	}
}
#endif

/*! \brief  check_auth: Check user authorization from peer definition */
/*      Some actions, like REGISTER and INVITEs from peers require
        authentication (if peer have secret set) */
static int check_auth(struct sip_pvt *p, struct sip_request *req, const char *username,
		      const char *secret, const char *md5secret, int sipmethod,
		      char *uri, int reliable, int ignore)
{
	int res = -1;
	char *response = "407 Proxy Authentication Required";
	char *reqheader = "Proxy-Authorization";
	char *respheader = "Proxy-Authenticate";
	char *authtoken;
#ifdef OSP_SUPPORT
	char *osptoken;
#endif
	/* Always OK if no secret */
	if (ast_strlen_zero(secret) && ast_strlen_zero(md5secret)
#ifdef OSP_SUPPORT
	    && !ast_test_flag(p, SIP_OSPAUTH)
	    && global_allowguest != 2
#endif
		)
		return 0;
	if (sipmethod == SIP_REGISTER || sipmethod == SIP_SUBSCRIBE) {
		/* On a REGISTER, we have to use 401 and its family of headers instead of 407 and its family
		   of headers -- GO SIP!  Whoo hoo!  Two things that do the same thing but are used in
		   different circumstances! What a surprise. */
		response = "401 Unauthorized";
		reqheader = "Authorization";
		respheader = "WWW-Authenticate";
	}
#ifdef OSP_SUPPORT
	else {
		ast_log (LOG_DEBUG, "Checking OSP Authentication!\n");
		osptoken = get_header (req, "P-OSP-Auth-Token");
		switch (ast_test_flag (p, SIP_OSPAUTH)) {
			case SIP_OSPAUTH_NO:
				break;
			case SIP_OSPAUTH_GATEWAY:
				if (ast_strlen_zero (osptoken)) {
					if (ast_strlen_zero (secret) && ast_strlen_zero (md5secret)) {
						return (0);
					}
				}
				else {
					return (check_osptoken (p, osptoken));
				}
				break;
			case SIP_OSPAUTH_PROXY:
				if (ast_strlen_zero (osptoken)) {
					return (0);
				} 
				else {
					return (check_osptoken (p, osptoken));
				}
				break;
			case SIP_OSPAUTH_EXCLUSIVE:
				if (ast_strlen_zero (osptoken)) {
					return (-1);
				}
				else {
					return (check_osptoken (p, osptoken));
				}
				break;
			default:
				return (-1);
		}
 	}
#endif	
	authtoken =  get_header(req, reqheader);	
	if (ignore && !ast_strlen_zero(p->randdata) && ast_strlen_zero(authtoken)) {
		/* This is a retransmitted invite/register/etc, don't reconstruct authentication
		   information */
		if (!ast_strlen_zero(p->randdata)) {
			if (!reliable) {
				/* Resend message if this was NOT a reliable delivery.   Otherwise the
				   retransmission should get it */
				transmit_response_with_auth(p, response, req, p->randdata, reliable, respheader, 0);
				/* Schedule auto destroy in 15 seconds */
				sip_scheddestroy(p, 15000);
			}
			res = 1;
		}
	} else if (ast_strlen_zero(p->randdata) || ast_strlen_zero(authtoken)) {
		ast_string_field_build(p, randdata, "%08x", thread_safe_rand());
		transmit_response_with_auth(p, response, req, p->randdata, reliable, respheader, 0);
		/* Schedule auto destroy in 15 seconds */
		sip_scheddestroy(p, 15000);
		res = 1;
	} else {
		/* Whoever came up with the authentication section of SIP can suck my %&#$&* for not putting
		   an example in the spec of just what it is you're doing a hash on. */
		char a1[256];
		char a2[256];
		char a1_hash[256];
		char a2_hash[256];
		char resp[256];
		char resp_hash[256]="";
		char tmp[256];
		char *c;
		char *z;
		char *ua_hash ="";
		char *resp_uri ="";
		char *nonce = "";
		char *digestusername = "";
		int  wrongnonce = 0;
		const char *usednonce = p->randdata;

		/* Find their response among the mess that we'r sent for comparison */
		ast_copy_string(tmp, authtoken, sizeof(tmp));
		c = tmp;

		while(c) {
			c = ast_skip_blanks(c);
			if (!*c)
				break;
			if (!strncasecmp(c, "response=", strlen("response="))) {
				c+= strlen("response=");
				if ((*c == '\"')) {
					ua_hash=++c;
					if ((c = strchr(c,'\"')))
						*c = '\0';

				} else {
					ua_hash=c;
					if ((c = strchr(c,',')))
						*c = '\0';
				}

			} else if (!strncasecmp(c, "uri=", strlen("uri="))) {
				c+= strlen("uri=");
				if ((*c == '\"')) {
					resp_uri=++c;
					if ((c = strchr(c,'\"')))
						*c = '\0';
				} else {
					resp_uri=c;
					if ((c = strchr(c,',')))
						*c = '\0';
				}

			} else if (!strncasecmp(c, "username=", strlen("username="))) {
				c+= strlen("username=");
				if ((*c == '\"')) {
					digestusername=++c;
					if((c = strchr(c,'\"')))
						*c = '\0';
				} else {
					digestusername=c;
					if((c = strchr(c,',')))
						*c = '\0';
				}
			} else if (!strncasecmp(c, "nonce=", strlen("nonce="))) {
				c+= strlen("nonce=");
				if ((*c == '\"')) {
					nonce=++c;
					if ((c = strchr(c,'\"')))
						*c = '\0';
				} else {
					nonce=c;
					if ((c = strchr(c,',')))
						*c = '\0';
				}

			} else
				if ((z = strchr(c,' ')) || (z = strchr(c,','))) c=z;
			if (c)
				c++;
		}
		/* Verify that digest username matches  the username we auth as */
		if (strcmp(username, digestusername)) {
			/* Oops, we're trying something here */
			return -2;
		}

		/* Verify nonce from request matches our nonce.  If not, send 401 with new nonce */
		if (strcasecmp(p->randdata, nonce)) {
			wrongnonce = 1;
			usednonce = nonce;
		}

		snprintf(a1, sizeof(a1), "%s:%s:%s", username, global_realm, secret);

		if (!ast_strlen_zero(resp_uri))
			snprintf(a2, sizeof(a2), "%s:%s", sip_methods[sipmethod].text, resp_uri);
		else
			snprintf(a2, sizeof(a2), "%s:%s", sip_methods[sipmethod].text, uri);

		if (!ast_strlen_zero(md5secret))
			snprintf(a1_hash, sizeof(a1_hash), "%s", md5secret);
		else
			ast_md5_hash(a1_hash, a1);

		ast_md5_hash(a2_hash, a2);

		snprintf(resp, sizeof(resp), "%s:%s:%s", a1_hash, usednonce, a2_hash);
		ast_md5_hash(resp_hash, resp);

		if (wrongnonce) {
			ast_string_field_build(p, randdata, "%08x", thread_safe_rand());
			if (ua_hash && !strncasecmp(ua_hash, resp_hash, strlen(resp_hash))) {
				if (sipdebug)
					ast_log(LOG_NOTICE, "stale nonce received from '%s'\n", get_header(req, "To"));
				/* We got working auth token, based on stale nonce . */
				transmit_response_with_auth(p, response, req, p->randdata, reliable, respheader, 1);
			} else {
				/* Everything was wrong, so give the device one more try with a new challenge */
				if (sipdebug)
					ast_log(LOG_NOTICE, "Bad authentication received from '%s'\n", get_header(req, "To"));
				transmit_response_with_auth(p, response, req, p->randdata, reliable, respheader, 0);
			}

			/* Schedule auto destroy in 15 seconds */
			sip_scheddestroy(p, 15000);
			return 1;
		} 
		/* resp_hash now has the expected response, compare the two */
		if (ua_hash && !strncasecmp(ua_hash, resp_hash, strlen(resp_hash))) {
			/* Auth is OK */
			res = 0;
		}
	}
	/* Failure */
	return res;
}

/*! \brief  cb_extensionstate: Callback for the devicestate notification (SUBSCRIBE) support subsystem */
/*    If you add an "hint" priority to the extension in the dial plan,
      you will get notifications on device state changes */
static int cb_extensionstate(char *context, char* exten, int state, void *data)
{
	struct sip_pvt *p = data;

	switch(state) {
	case AST_EXTENSION_DEACTIVATED:	/* Retry after a while */
	case AST_EXTENSION_REMOVED:	/* Extension is gone */
		if (p->autokillid > -1)
			sip_cancel_destroy(p);	/* Remove subscription expiry for renewals */
		sip_scheddestroy(p, 15000);	/* Delete subscription in 15 secs */
		ast_verbose(VERBOSE_PREFIX_2 "Extension state: Watcher for hint %s %s. Notify User %s\n", exten, state == AST_EXTENSION_DEACTIVATED ? "deactivated" : "removed", p->username);
		p->stateid = -1;
		p->subscribed = NONE;
		append_history(p, "Subscribestatus", "%s", state == AST_EXTENSION_REMOVED ? "HintRemoved" : "Deactivated");
		break;
	default:	/* Tell user */
		p->laststate = state;
		break;
	}
	transmit_state_notify(p, state, 1);

	if (option_debug > 1)
		ast_verbose(VERBOSE_PREFIX_1 "Extension Changed %s new state %s for Notify User %s\n", exten, ast_extension_state2str(state), p->username);
	return 0;
}

/*! \brief  register_verify: Verify registration of user */
static int register_verify(struct sip_pvt *p, struct sockaddr_in *sin, struct sip_request *req, char *uri, int ignore)
{
	int res = -3;
	struct sip_peer *peer;
	char tmp[256];
	char iabuf[INET_ADDRSTRLEN];
	char *name, *c;
	char *t;
	char *domain;

	/* Terminate URI */
	t = uri;
	while(*t && (*t > 32) && (*t != ';'))
		t++;
	*t = '\0';
	
	ast_copy_string(tmp, get_header(req, "To"), sizeof(tmp));
	if (pedanticsipchecking)
		ast_uri_decode(tmp);

	c = get_in_brackets(tmp);
	/* Ditch ;user=phone */
	name = strchr(c, ';');
	if (name)
		*name = '\0';

	if (!strncmp(c, "sip:", 4)) {
		name = c + 4;
	} else {
		name = c;
		ast_log(LOG_NOTICE, "Invalid to address: '%s' from %s (missing sip:) trying to use anyway...\n", c, ast_inet_ntoa(iabuf, sizeof(iabuf), sin->sin_addr));
	}

	/* Strip off the domain name */
	if ((c = strchr(name, '@'))) {
		*c++ = '\0';
		domain = c;
		if ((c = strchr(domain, ':')))	/* Remove :port */
			*c = '\0';
		if (!AST_LIST_EMPTY(&domain_list)) {
			if (!check_sip_domain(domain, NULL, 0)) {
				transmit_response(p, "404 Not found (unknown domain)", &p->initreq);
				return -3;
			}
		}
	}

	ast_string_field_set(p, exten, name);
	build_contact(p);
	peer = find_peer(name, NULL, 1);
	if (!(peer && ast_apply_ha(peer->ha, sin))) {
		if (peer)
			ASTOBJ_UNREF(peer, sip_destroy_peer);
	}
	if (peer) {
		if (!ast_test_flag(peer, SIP_DYNAMIC)) {
			ast_log(LOG_ERROR, "Peer '%s' is trying to register, but not configured as host=dynamic\n", peer->name);
		} else {
			ast_copy_flags(p, peer, SIP_NAT);
			transmit_response(p, "100 Trying", req);
			if (!(res = check_auth(p, req, peer->name, peer->secret, peer->md5secret, SIP_REGISTER, uri, 0, ignore))) {
				sip_cancel_destroy(p);
				switch (parse_register_contact(p, peer, req)) {
				case PARSE_REGISTER_FAILED:
					ast_log(LOG_WARNING, "Failed to parse contact info\n");
					break;
				case PARSE_REGISTER_QUERY:
					transmit_response_with_date(p, "200 OK", req);
					peer->lastmsgssent = -1;
					res = 0;
					break;
				case PARSE_REGISTER_UPDATE:
					update_peer(peer, p->expiry);
					/* Say OK and ask subsystem to retransmit msg counter */
					transmit_response_with_date(p, "200 OK", req);
					peer->lastmsgssent = -1;
					res = 0;
					break;
				}
			} 
		}
	}
	if (!peer && autocreatepeer) {
		/* Create peer if we have autocreate mode enabled */
		peer = temp_peer(name);
		if (peer) {
			ASTOBJ_CONTAINER_LINK(&peerl, peer);
			peer->lastmsgssent = -1;
			sip_cancel_destroy(p);
			switch (parse_register_contact(p, peer, req)) {
			case PARSE_REGISTER_FAILED:
				ast_log(LOG_WARNING, "Failed to parse contact info\n");
				break;
			case PARSE_REGISTER_QUERY:
				transmit_response_with_date(p, "200 OK", req);
				peer->lastmsgssent = -1;
				res = 0;
				break;
			case PARSE_REGISTER_UPDATE:
				/* Say OK and ask subsystem to retransmit msg counter */
				transmit_response_with_date(p, "200 OK", req);
				manager_event(EVENT_FLAG_SYSTEM, "PeerStatus", "Peer: SIP/%s\r\nPeerStatus: Registered\r\n", peer->name);
				peer->lastmsgssent = -1;
				res = 0;
				break;
			}
		}
	}
	if (!res) {
		ast_device_state_changed("SIP/%s", peer->name);
	}
	if (res < 0) {
		switch (res) {
		case -1:
			/* Wrong password in authentication. Go away, don't try again until you fixed it */
			transmit_response(p, "403 Forbidden (Bad auth)", &p->initreq);
			break;
		case -2:
			/* Username and digest username does not match. 
			   Asterisk uses the From: username for authentication. We need the
			   users to use the same authentication user name until we support
			   proper authentication by digest auth name */
			transmit_response(p, "403 Authentication user name does not match account name", &p->initreq);
			break;
		case -3:
			/* URI not found */
			transmit_response(p, "404 Not found", &p->initreq);
			/* Set res back to -2 because we don't want to return an invalid domain message. That check already happened up above. */
			res = -2;
			break;
		}
		if (option_debug > 1) {
			ast_log(LOG_DEBUG, "SIP REGISTER attempt failed for %s : %s\n",
				peer->name,
				(res == -1) ? "Bad password" : ((res == -2 ) ? "Bad digest user" : "Peer not found"));
		}
	}
	if (peer)
		ASTOBJ_UNREF(peer, sip_destroy_peer);

	return res;
}

/*! \brief  get_rdnis: get referring dnis */
static int get_rdnis(struct sip_pvt *p, struct sip_request *oreq)
{
	char tmp[256], *c, *a;
	struct sip_request *req;
	
	req = oreq;
	if (!req)
		req = &p->initreq;
	ast_copy_string(tmp, get_header(req, "Diversion"), sizeof(tmp));
	if (ast_strlen_zero(tmp))
		return 0;
	c = get_in_brackets(tmp);
	if (strncmp(c, "sip:", 4)) {
		ast_log(LOG_WARNING, "Huh?  Not an RDNIS SIP header (%s)?\n", c);
		return -1;
	}
	c += 4;
	if ((a = strchr(c, '@')) || (a = strchr(c, ';'))) {
		*a = '\0';
	}
	if (sip_debug_test_pvt(p))
		ast_verbose("RDNIS is %s\n", c);
	ast_string_field_set(p, rdnis, c);

	return 0;
}

/*! \brief  get_destination: Find out who the call is for */
static int get_destination(struct sip_pvt *p, struct sip_request *oreq)
{
	char tmp[256] = "", *uri, *a;
	char tmpf[256], *from;
	struct sip_request *req;
	
	req = oreq;
	if (!req)
		req = &p->initreq;
	if (req->rlPart2)
		ast_copy_string(tmp, req->rlPart2, sizeof(tmp));
	uri = get_in_brackets(tmp);
	
	ast_copy_string(tmpf, get_header(req, "From"), sizeof(tmpf));

	from = get_in_brackets(tmpf);
	
	if (strncmp(uri, "sip:", 4)) {
		ast_log(LOG_WARNING, "Huh?  Not a SIP header (%s)?\n", uri);
		return -1;
	}
	uri += 4;
	if (!ast_strlen_zero(from)) {
		if (strncmp(from, "sip:", 4)) {
			ast_log(LOG_WARNING, "Huh?  Not a SIP header (%s)?\n", from);
			return -1;
		}
		from += 4;
	} else
		from = NULL;

	if (pedanticsipchecking) {
		ast_uri_decode(uri);
		ast_uri_decode(from);
	}

	/* Get the target domain */
	if ((a = strchr(uri, '@'))) {
		char *colon;
		*a = '\0';
		a++;
		colon = strchr(a, ':'); /* Remove :port */
		if (colon)
			*colon = '\0';
		ast_string_field_set(p, domain, a);
	}
	/* Skip any options */
	if ((a = strchr(uri, ';'))) {
		*a = '\0';
	}

	if (!AST_LIST_EMPTY(&domain_list)) {
		char domain_context[AST_MAX_EXTENSION];

		domain_context[0] = '\0';
		if (!check_sip_domain(p->domain, domain_context, sizeof(domain_context))) {
			if (!allow_external_domains && (req->method == SIP_INVITE || req->method == SIP_REFER)) {
				ast_log(LOG_DEBUG, "Got SIP %s to non-local domain '%s'; refusing request.\n", sip_methods[req->method].text, p->domain);
				return -2;
			}
		}
		/* If we have a context defined, overwrite the original context */
		if (!ast_strlen_zero(domain_context))
			ast_string_field_set(p, context, domain_context);
	}

	if (from) {
		if ((a = strchr(from, ';')))
			*a = '\0';
		if ((a = strchr(from, '@'))) {
			*a = '\0';
			ast_string_field_set(p, fromdomain, a + 1);
		} else
			ast_string_field_set(p, fromdomain, from);
	}
	if (sip_debug_test_pvt(p))
		ast_verbose("Looking for %s in %s (domain %s)\n", uri, p->context, p->domain);

	/* Return 0 if we have a matching extension */
	if (ast_exists_extension(NULL, p->context, uri, 1, from) ||
		!strcmp(uri, ast_pickup_ext())) {
		if (!oreq)
			ast_string_field_set(p, exten, uri);
		return 0;
	}

	/* Return 1 for overlap dialling support */
	if (ast_canmatch_extension(NULL, p->context, uri, 1, from) ||
	    !strncmp(uri, ast_pickup_ext(),strlen(uri))) {
		return 1;
	}
	
	return -1;
}

/*! \brief  get_sip_pvt_byid_locked: Lock interface lock and find matching pvt lock  */
static struct sip_pvt *get_sip_pvt_byid_locked(char *callid) 
{
	struct sip_pvt *sip_pvt_ptr = NULL;
	
	/* Search interfaces and find the match */
	ast_mutex_lock(&iflock);
	sip_pvt_ptr = iflist;
	while(sip_pvt_ptr) {
		if (!strcmp(sip_pvt_ptr->callid, callid)) {
			/* Go ahead and lock it (and its owner) before returning */
			ast_mutex_lock(&sip_pvt_ptr->lock);
			if (sip_pvt_ptr->owner) {
				while(ast_mutex_trylock(&sip_pvt_ptr->owner->lock)) {
					ast_mutex_unlock(&sip_pvt_ptr->lock);
					usleep(1);
					ast_mutex_lock(&sip_pvt_ptr->lock);
					if (!sip_pvt_ptr->owner)
						break;
				}
			}
			break;
		}
		sip_pvt_ptr = sip_pvt_ptr->next;
	}
	ast_mutex_unlock(&iflock);
	return sip_pvt_ptr;
}

/*! \brief  get_refer_info: Call transfer support (the REFER method) */
static int get_refer_info(struct sip_pvt *sip_pvt, struct sip_request *outgoing_req)
{

	char *p_refer_to = NULL, *p_referred_by = NULL, *h_refer_to = NULL, *h_referred_by = NULL, *h_contact = NULL;
	char *replace_callid = "", *refer_to = NULL, *referred_by = NULL, *ptr = NULL;
	struct sip_request *req = NULL;
	struct sip_pvt *sip_pvt_ptr = NULL;
	struct ast_channel *chan = NULL, *peer = NULL;

	req = outgoing_req;

	if (!req) {
		req = &sip_pvt->initreq;
	}
	
	if (!( (p_refer_to = get_header(req, "Refer-To")) && (h_refer_to = ast_strdupa(p_refer_to)) )) {
		ast_log(LOG_WARNING, "No Refer-To Header That's illegal\n");
		return -1;
	}

	refer_to = get_in_brackets(h_refer_to);

	if (!( (p_referred_by = get_header(req, "Referred-By")) && (h_referred_by = ast_strdupa(p_referred_by)) )) {
		ast_log(LOG_WARNING, "No Referrred-By Header That's not illegal\n");
		return -1;
	} else {
		if (pedanticsipchecking) {
			ast_uri_decode(h_referred_by);
		}
		referred_by = get_in_brackets(h_referred_by);
	}
	h_contact = get_header(req, "Contact");
	
	if (strncmp(refer_to, "sip:", 4)) {
		ast_log(LOG_WARNING, "Refer-to: Huh?  Not a SIP header (%s)?\n", refer_to);
		return -1;
	}

	if (strncmp(referred_by, "sip:", 4)) {
		ast_log(LOG_WARNING, "Referred-by: Huh?  Not a SIP header (%s) Ignoring?\n", referred_by);
		referred_by = NULL;
	}

	if (refer_to)
		refer_to += 4;

	if (referred_by)
		referred_by += 4;
	
	if ((ptr = strchr(refer_to, '?'))) {
		/* Search for arguments */
		*ptr = '\0';
		ptr++;
		if (!strncasecmp(ptr, "REPLACES=", 9)) {
			char *p;
			replace_callid = ast_strdupa(ptr + 9);
			/* someday soon to support invite/replaces properly!
			   replaces_header = ast_strdupa(replace_callid); 
			   -anthm
			*/
			ast_uri_decode(replace_callid);
			if ((ptr = strchr(replace_callid, '%'))) 
				*ptr = '\0';
			if ((ptr = strchr(replace_callid, ';'))) 
				*ptr = '\0';
			/* Skip leading whitespace XXX memmove behaviour with overlaps ? */
			p = ast_skip_blanks(replace_callid);
			if (p != replace_callid)
				memmove(replace_callid, p, strlen(p));
		}
	}
	
	if ((ptr = strchr(refer_to, '@')))	/* Skip domain (should be saved in SIPDOMAIN) */
		*ptr = '\0';
	if ((ptr = strchr(refer_to, ';'))) 
		*ptr = '\0';
	
	if (referred_by) {
		if ((ptr = strchr(referred_by, '@')))
			*ptr = '\0';
		if ((ptr = strchr(referred_by, ';'))) 
			*ptr = '\0';
	}
	
	if (sip_debug_test_pvt(sip_pvt)) {
		ast_verbose("Transfer to %s in %s\n", refer_to, sip_pvt->context);
		if (referred_by)
			ast_verbose("Transfer from %s in %s\n", referred_by, sip_pvt->context);
	}
	if (!ast_strlen_zero(replace_callid)) {	
		/* This is a supervised transfer */
		ast_log(LOG_DEBUG,"Assigning Replace-Call-ID Info %s to REPLACE_CALL_ID\n",replace_callid);
		
		ast_string_field_free(sip_pvt, refer_to);
		ast_string_field_free(sip_pvt, referred_by);
		ast_string_field_free(sip_pvt, refer_contact);
		sip_pvt->refer_call = NULL;
		if ((sip_pvt_ptr = get_sip_pvt_byid_locked(replace_callid))) {
			sip_pvt->refer_call = sip_pvt_ptr;
			if (sip_pvt->refer_call == sip_pvt) {
				ast_log(LOG_NOTICE, "Supervised transfer attempted to transfer into same call id (%s == %s)!\n", replace_callid, sip_pvt->callid);
				sip_pvt->refer_call = NULL;
			} else
				return 0;
		} else {
			ast_log(LOG_NOTICE, "Supervised transfer requested, but unable to find callid '%s'.  Both legs must reside on Asterisk box to transfer at this time.\n", replace_callid);
			/* XXX The refer_to could contain a call on an entirely different machine, requiring an 
	    		  INVITE with a replaces header -anthm XXX */
			/* The only way to find out is to use the dialplan - oej */
		}
	} else if (ast_exists_extension(NULL, sip_pvt->context, refer_to, 1, NULL) || !strcmp(refer_to, ast_parking_ext())) {
		/* This is an unsupervised transfer (blind transfer) */
		
		ast_log(LOG_DEBUG,"Unsupervised transfer to (Refer-To): %s\n", refer_to);
		if (referred_by)
			ast_log(LOG_DEBUG,"Transferred by  (Referred-by: ) %s \n", referred_by);
		ast_log(LOG_DEBUG,"Transfer Contact Info %s (REFER_CONTACT)\n", h_contact);
		ast_string_field_set(sip_pvt, refer_to, refer_to);
		if (referred_by)
			ast_string_field_set(sip_pvt, referred_by, referred_by);
		if (h_contact) {
			ast_string_field_set(sip_pvt, refer_contact, h_contact);
		}
		sip_pvt->refer_call = NULL;
		if ((chan = sip_pvt->owner) && (peer = ast_bridged_channel(sip_pvt->owner))) {
			pbx_builtin_setvar_helper(chan, "BLINDTRANSFER", peer->name);
			pbx_builtin_setvar_helper(peer, "BLINDTRANSFER", chan->name);
		}
		return 0;
	} else if (ast_canmatch_extension(NULL, sip_pvt->context, refer_to, 1, NULL)) {
		return 1;
	}

	return -1;
}

/*! \brief  Call transfer support (old way, deprecated in the IETF) */
static int get_also_info(struct sip_pvt *p, struct sip_request *oreq)
{
	char tmp[256], *c, *a;
	struct sip_request *req;
	
	req = oreq;
	if (!req)
		req = &p->initreq;
	ast_copy_string(tmp, get_header(req, "Also"), sizeof(tmp));
	
	c = get_in_brackets(tmp);
	
		
	if (strncmp(c, "sip:", 4)) {
		ast_log(LOG_WARNING, "Huh?  Not a SIP header (%s)?\n", c);
		return -1;
	}
	c += 4;
	if ((a = strchr(c, '@')))
		*a = '\0';
	if ((a = strchr(c, ';'))) 
		*a = '\0';
	
	if (sip_debug_test_pvt(p)) {
		ast_verbose("Looking for %s in %s\n", c, p->context);
	}
	if (ast_exists_extension(NULL, p->context, c, 1, NULL)) {
		/* This is an unsupervised transfer */
		if (option_debug)
			ast_log(LOG_DEBUG,"Assigning Extension %s to REFER-TO\n", c);
		ast_string_field_set(p, refer_to, c);
		ast_string_field_free(p, referred_by);
		ast_string_field_free(p, refer_contact);
		p->refer_call = NULL;
		return 0;
	} else if (ast_canmatch_extension(NULL, p->context, c, 1, NULL)) {
		return 1;
	}

	return -1;
}

/*! \brief check Via: header for hostname, port and rport request/answer */
static int check_via(struct sip_pvt *p, struct sip_request *req)
{
	char via[256];
	char iabuf[INET_ADDRSTRLEN];
	char *c, *pt;
	struct hostent *hp;
	struct ast_hostent ahp;

	ast_copy_string(via, get_header(req, "Via"), sizeof(via));

	/* Check for rport */
	c = strstr(via, ";rport");
	if (c && (c[6] != '='))	/* rport query, not answer */
		ast_set_flag(p, SIP_NAT_ROUTE);

	c = strchr(via, ';');
	if (c) 
		*c = '\0';

	c = strchr(via, ' ');
	if (c) {
		*c = '\0';
		c = ast_skip_blanks(c+1);
		if (strcasecmp(via, "SIP/2.0/UDP")) {
			ast_log(LOG_WARNING, "Don't know how to respond via '%s'\n", via);
			return -1;
		}
		pt = strchr(c, ':');
		if (pt)
			*pt++ = '\0';	/* remember port pointer */
		hp = ast_gethostbyname(c, &ahp);
		if (!hp) {
			ast_log(LOG_WARNING, "'%s' is not a valid host\n", c);
			return -1;
		}
		memset(&p->sa, 0, sizeof(p->sa));
		p->sa.sin_family = AF_INET;
		memcpy(&p->sa.sin_addr, hp->h_addr, sizeof(p->sa.sin_addr));
		p->sa.sin_port = htons(pt ? atoi(pt) : DEFAULT_SIP_PORT);

		if (sip_debug_test_pvt(p)) {
			c = (ast_test_flag(p, SIP_NAT) & SIP_NAT_ROUTE) ? "NAT" : "non-NAT";
			ast_verbose("Sending to %s : %d (%s)\n", ast_inet_ntoa(iabuf, sizeof(iabuf), p->sa.sin_addr), ntohs(p->sa.sin_port), c);
		}
	}
	return 0;
}

/*! \brief  Get caller id name from SIP headers */
static char *get_calleridname(char *input, char *output, size_t outputsize)
{
	char *end = strchr(input,'<');
	char *tmp = strchr(input,'\"');
	int bytes = 0;
	int maxbytes = outputsize - 1;

	if (!end || (end == input)) return NULL;
	/* move away from "<" */
	end--;
	/* we found "name" */
	if (tmp && tmp < end) {
		end = strchr(tmp+1, '\"');
		if (!end) return NULL;
		bytes = (int) (end - tmp);
		/* protect the output buffer */
		if (bytes > maxbytes)
			bytes = maxbytes;
		ast_copy_string(output, tmp + 1, bytes);
	} else {
		/* we didn't find "name" */
		/* clear the empty characters in the begining*/
		input = ast_skip_blanks(input);
		/* clear the empty characters in the end */
		while(*end && (*end < 33) && end > input)
			end--;
		if (end >= input) {
			bytes = (int) (end - input) + 2;
			/* protect the output buffer */
			if (bytes > maxbytes) {
				bytes = maxbytes;
			}
			ast_copy_string(output, input, bytes);
		}
		else
			return NULL;
	}
	return output;
}

/*! \brief  Get caller id number from Remote-Party-ID header field 
 *	Returns true if number should be restricted (privacy setting found)
 *	output is set to NULL if no number found
 */
static int get_rpid_num(char *input,char *output, int maxlen)
{
	char *start;
	char *end;

	start = strchr(input,':');
	if (!start) {
		output[0] = '\0';
		return 0;
	}
	start++;

	/* we found "number" */
	ast_copy_string(output,start,maxlen);
	output[maxlen-1] = '\0';

	end = strchr(output,'@');
	if (end)
		*end = '\0';
	else
		output[0] = '\0';
	if (strstr(input,"privacy=full") || strstr(input,"privacy=uri"))
		return AST_PRES_PROHIB_USER_NUMBER_NOT_SCREENED;

	return 0;
}


/*! \brief  Check if matching user or peer is defined */
/* 	Match user on From: user name and peer on IP/port */
/*	This is used on first invite (not re-invites) and subscribe requests */
static int check_user_full(struct sip_pvt *p, struct sip_request *req, int sipmethod, char *uri, int reliable, struct sockaddr_in *sin, int ignore, char *mailbox, int mailboxlen)
{
	struct sip_user *user = NULL;
	struct sip_peer *peer;
	char *of, from[256], *c;
	char *rpid, rpid_num[50];
	char iabuf[INET_ADDRSTRLEN];
	int res = 0;
	char *t;
	char calleridname[50];
	int debug=sip_debug_test_addr(sin);
	struct ast_variable *tmpvar = NULL, *v = NULL;

	/* Terminate URI */
	t = uri;
	while(*t && (*t > 32) && (*t != ';'))
		t++;
	*t = '\0';
	of = get_header(req, "From");
	if (pedanticsipchecking)
		ast_uri_decode(of);

	ast_copy_string(from, of, sizeof(from));
	
	memset(calleridname, 0, sizeof(calleridname));
	get_calleridname(from, calleridname, sizeof(calleridname));
	if (calleridname[0])
		ast_string_field_set(p, cid_name, calleridname);

	rpid = get_header(req, "Remote-Party-ID");
	memset(rpid_num, 0, sizeof(rpid_num));
	if (!ast_strlen_zero(rpid)) 
		p->callingpres = get_rpid_num(rpid,rpid_num, sizeof(rpid_num));

	of = get_in_brackets(from);
	if (ast_strlen_zero(p->exten)) {
		t = uri;
		if (!strncmp(t, "sip:", 4))
			t+= 4;
		ast_string_field_set(p, exten, t);
		t = strchr(p->exten, '@');
		if (t)
			*t = '\0';
		if (ast_strlen_zero(p->our_contact))
			build_contact(p);
	}
	/* save the URI part of the From header */
	ast_string_field_set(p, from, of);
	if (strncmp(of, "sip:", 4)) {
		ast_log(LOG_NOTICE, "From address missing 'sip:', using it anyway\n");
	} else
		of += 4;
	/* Get just the username part */
	if ((c = strchr(of, '@'))) {
		char *tmp;
		*c = '\0';
		if ((c = strchr(of, ':')))
			*c = '\0';
		tmp = ast_strdupa(of);
		if (tmp) {
			ast_shrink_phone_number(tmp);
			ast_string_field_set(p, cid_num, tmp);
		} else {
			ast_string_field_set(p, cid_num, of);
		}
	}
	if (ast_strlen_zero(of))
		return 0;

	if (!mailbox)	/* If it's a mailbox SUBSCRIBE, don't check users */
		user = find_user(of, 1);

	/* Find user based on user name in the from header */
	if (user && ast_apply_ha(user->ha, sin)) {
		ast_copy_flags(p, user, SIP_FLAGS_TO_COPY);
		/* copy channel vars */
		for (v = user->chanvars ; v ; v = v->next) {
			if ((tmpvar = ast_variable_new(v->name, v->value))) {
				tmpvar->next = p->chanvars; 
				p->chanvars = tmpvar;
			}
		}
		p->prefs = user->prefs;
		/* replace callerid if rpid found, and not restricted */
		if (!ast_strlen_zero(rpid_num) && ast_test_flag(p, SIP_TRUSTRPID)) {
			char *tmp;
			if (*calleridname)
				ast_string_field_set(p, cid_name, calleridname);
			tmp = ast_strdupa(rpid_num);
			if (tmp) {
				ast_shrink_phone_number(tmp);
				ast_string_field_set(p, cid_num, tmp);
			} else {
				ast_string_field_set(p, cid_num, rpid_num);
			}
		}

		if (p->rtp) {
			if (option_debug)
				ast_log(LOG_DEBUG, "Setting NAT on RTP to %d\n", (ast_test_flag(p, SIP_NAT) & SIP_NAT_ROUTE));
			ast_rtp_setnat(p->rtp, (ast_test_flag(p, SIP_NAT) & SIP_NAT_ROUTE));
		}
		if (p->vrtp) {
			if (option_debug)
				ast_log(LOG_DEBUG, "Setting NAT on VRTP to %d\n", (ast_test_flag(p, SIP_NAT) & SIP_NAT_ROUTE));
			ast_rtp_setnat(p->vrtp, (ast_test_flag(p, SIP_NAT) & SIP_NAT_ROUTE));
		}
		if (!(res = check_auth(p, req, user->name, user->secret, user->md5secret, sipmethod, uri, reliable, ignore))) {
			sip_cancel_destroy(p);
			ast_copy_flags(p, user, SIP_FLAGS_TO_COPY);
			/* Copy SIP extensions profile from INVITE */
			if (p->sipoptions)
				user->sipoptions = p->sipoptions;

			/* If we have a call limit, set flag */
			if (user->call_limit)
				ast_set_flag(p, SIP_CALL_LIMIT);
			if (!ast_strlen_zero(user->context))
				ast_string_field_set(p, context, user->context);
			if (!ast_strlen_zero(user->cid_num) && !ast_strlen_zero(p->cid_num)) {
				char *tmp = ast_strdupa(user->cid_num);
				if (tmp) {
					ast_shrink_phone_number(tmp);
					ast_string_field_set(p, cid_num, tmp);
				} else {
					ast_string_field_set(p, cid_num, user->cid_num);
				}
			}
			if (!ast_strlen_zero(user->cid_name) && !ast_strlen_zero(p->cid_num))
				ast_string_field_set(p, cid_name, user->cid_name);
			ast_string_field_set(p, username, user->name);
			ast_string_field_set(p, peersecret, user->secret);
			ast_string_field_set(p, peermd5secret, user->md5secret);
			ast_string_field_set(p, subscribecontext, user->subscribecontext);
			ast_string_field_set(p, accountcode, user->accountcode);
			ast_string_field_set(p, language, user->language);
			ast_string_field_set(p, musicclass, user->musicclass);
			p->amaflags = user->amaflags;
			p->callgroup = user->callgroup;
			p->pickupgroup = user->pickupgroup;
			p->callingpres = user->callingpres;
			p->capability = user->capability;
			p->jointcapability = user->capability;
			if (p->peercapability)
				p->jointcapability &= p->peercapability;
			if ((ast_test_flag(p, SIP_DTMF) == SIP_DTMF_RFC2833) || (ast_test_flag(p, SIP_DTMF) == SIP_DTMF_AUTO))
				p->noncodeccapability |= AST_RTP_DTMF;
			else
				p->noncodeccapability &= ~AST_RTP_DTMF;
		}
		if (user && debug)
			ast_verbose("Found user '%s'\n", user->name);
	} else {
		if (user) {
			if (!mailbox && debug)
				ast_verbose("Found user '%s', but fails host access\n", user->name);
			ASTOBJ_UNREF(user,sip_destroy_user);
		}
		user = NULL;
	}

	if (!user) {
		/* If we didn't find a user match, check for peers */
		if (sipmethod == SIP_SUBSCRIBE)
			/* For subscribes, match on peer name only */
			peer = find_peer(of, NULL, 1);
		else
			/* Look for peer based on the IP address we received data from */
			/* If peer is registered from this IP address or have this as a default
			   IP address, this call is from the peer 
			*/
			peer = find_peer(NULL, &p->recv, 1);

		if (peer) {
			if (debug)
				ast_verbose("Found peer '%s'\n", peer->name);
			/* Take the peer */
			ast_copy_flags(p, peer, SIP_FLAGS_TO_COPY);

			/* Copy SIP extensions profile to peer */
			if (p->sipoptions)
				peer->sipoptions = p->sipoptions;

			/* replace callerid if rpid found, and not restricted */
			if (!ast_strlen_zero(rpid_num) && ast_test_flag(p, SIP_TRUSTRPID)) {
				char *tmp = ast_strdupa(rpid_num);
				if (*calleridname)
					ast_string_field_set(p, cid_name, calleridname);
				if (tmp) {
					ast_shrink_phone_number(tmp);
					ast_string_field_set(p, cid_num, tmp);
				} else {
					ast_string_field_set(p, cid_num, rpid_num);
				}
			}
			if (p->rtp) {
				ast_log(LOG_DEBUG, "Setting NAT on RTP to %d\n", (ast_test_flag(p, SIP_NAT) & SIP_NAT_ROUTE));
				ast_rtp_setnat(p->rtp, (ast_test_flag(p, SIP_NAT) & SIP_NAT_ROUTE));
			}
			if (p->vrtp) {
				ast_log(LOG_DEBUG, "Setting NAT on VRTP to %d\n", (ast_test_flag(p, SIP_NAT) & SIP_NAT_ROUTE));
				ast_rtp_setnat(p->vrtp, (ast_test_flag(p, SIP_NAT) & SIP_NAT_ROUTE));
			}
			ast_string_field_set(p, peersecret, peer->secret);
			ast_string_field_set(p, peermd5secret, peer->md5secret);
			ast_string_field_set(p, subscribecontext, peer->subscribecontext);
			p->callingpres = peer->callingpres;
			if (peer->maxms && peer->lastms)
				p->timer_t1 = peer->lastms;
			if (ast_test_flag(peer, SIP_INSECURE_INVITE)) {
				/* Pretend there is no required authentication */
				ast_string_field_free(p, peersecret);
				ast_string_field_free(p, peermd5secret);
			}
			if (!(res = check_auth(p, req, peer->name, p->peersecret, p->peermd5secret, sipmethod, uri, reliable, ignore))) {
				ast_copy_flags(p, peer, SIP_FLAGS_TO_COPY);
				/* If we have a call limit, set flag */
				if (peer->call_limit)
					ast_set_flag(p, SIP_CALL_LIMIT);
				ast_string_field_set(p, peername, peer->name);
				ast_string_field_set(p, authname, peer->name);

				/* copy channel vars */
				for (v = peer->chanvars ; v ; v = v->next) {
					if ((tmpvar = ast_variable_new(v->name, v->value))) {
						tmpvar->next = p->chanvars; 
						p->chanvars = tmpvar;
					}
				}
				if (mailbox)
					snprintf(mailbox, mailboxlen, ",%s,", peer->mailbox);
				if (!ast_strlen_zero(peer->username)) {
					ast_string_field_set(p, username, peer->username);
					/* Use the default username for authentication on outbound calls */
					ast_string_field_set(p, authname, peer->username);
				}
				if (!ast_strlen_zero(peer->cid_num) && !ast_strlen_zero(p->cid_num)) {
					char *tmp = ast_strdupa(peer->cid_num);
					if (tmp) {
						ast_shrink_phone_number(tmp);
						ast_string_field_set(p, cid_num, tmp);
					} else {
						ast_string_field_set(p, cid_num, peer->cid_num);
					}
				}
				if (!ast_strlen_zero(peer->cid_name) && !ast_strlen_zero(p->cid_name)) 
					ast_string_field_set(p, cid_name, peer->cid_name);
				ast_string_field_set(p, fullcontact, peer->fullcontact);
				if (!ast_strlen_zero(peer->context))
					ast_string_field_set(p, context, peer->context);
				ast_string_field_set(p, peersecret, peer->secret);
				ast_string_field_set(p, peermd5secret, peer->md5secret);
				ast_string_field_set(p, language, peer->language);
				ast_string_field_set(p, accountcode, peer->accountcode);
				p->amaflags = peer->amaflags;
				p->callgroup = peer->callgroup;
				p->pickupgroup = peer->pickupgroup;
				p->capability = peer->capability;
				p->prefs = peer->prefs;
				p->jointcapability = peer->capability;
				if (p->peercapability)
					p->jointcapability &= p->peercapability;
				if ((ast_test_flag(p, SIP_DTMF) == SIP_DTMF_RFC2833) || (ast_test_flag(p, SIP_DTMF) == SIP_DTMF_AUTO))
					p->noncodeccapability |= AST_RTP_DTMF;
				else
					p->noncodeccapability &= ~AST_RTP_DTMF;
			}
			ASTOBJ_UNREF(peer, sip_destroy_peer);
		} else { 
			if (debug)
				ast_verbose("Found no matching peer or user for '%s:%d'\n", ast_inet_ntoa(iabuf, sizeof(iabuf), p->recv.sin_addr), ntohs(p->recv.sin_port));

			/* do we allow guests? */
			if (!global_allowguest)
				res = -1;  /* we don't want any guests, authentication will fail */
#ifdef OSP_SUPPORT			
			else if (global_allowguest == 2) {
				ast_copy_flags(p, &global_flags, SIP_OSPAUTH);
				res = check_auth(p, req, "", "", "", sipmethod, uri, reliable, ignore); 
			}
#endif
		}

	}

	if (user)
		ASTOBJ_UNREF(user, sip_destroy_user);
	return res;
}

/*! \brief  Find user */
static int check_user(struct sip_pvt *p, struct sip_request *req, int sipmethod, char *uri, int reliable, struct sockaddr_in *sin, int ignore)
{
	return check_user_full(p, req, sipmethod, uri, reliable, sin, ignore, NULL, 0);
}

/*! \brief  Get text out of a SIP MESSAGE packet */
static int get_msg_text(char *buf, int len, struct sip_request *req)
{
	int x;
	int y;

	buf[0] = '\0';
	y = len - strlen(buf) - 5;
	if (y < 0)
		y = 0;
	for (x=0;x<req->lines;x++) {
		strncat(buf, req->line[x], y); /* safe */
		y -= strlen(req->line[x]) + 1;
		if (y < 0)
			y = 0;
		if (y != 0)
			strcat(buf, "\n"); /* safe */
	}
	return 0;
}

                
/*! \brief  Receive SIP MESSAGE method messages */
/*	We only handle messages within current calls currently */
/*	Reference: RFC 3428 */
static void receive_message(struct sip_pvt *p, struct sip_request *req)
{
	char buf[1024];
	struct ast_frame f;
	char *content_type;

	content_type = get_header(req, "Content-Type");
	if (strcmp(content_type, "text/plain")) { /* No text/plain attachment */
		transmit_response(p, "415 Unsupported Media Type", req); /* Good enough, or? */
		ast_set_flag(p, SIP_NEEDDESTROY);
		return;
	}

	if (get_msg_text(buf, sizeof(buf), req)) {
		ast_log(LOG_WARNING, "Unable to retrieve text from %s\n", p->callid);
		transmit_response(p, "202 Accepted", req);
		ast_set_flag(p, SIP_NEEDDESTROY);
		return;
	}

	if (p->owner) {
		if (sip_debug_test_pvt(p))
			ast_verbose("Message received: '%s'\n", buf);
		memset(&f, 0, sizeof(f));
		f.frametype = AST_FRAME_TEXT;
		f.subclass = 0;
		f.offset = 0;
		f.data = buf;
		f.datalen = strlen(buf);
		ast_queue_frame(p->owner, &f);
		transmit_response(p, "202 Accepted", req); /* We respond 202 accepted, since we relay the message */
	} else { /* Message outside of a call, we do not support that */
		ast_log(LOG_WARNING,"Received message to %s from %s, dropped it...\n  Content-Type:%s\n  Message: %s\n", get_header(req,"To"), get_header(req,"From"), content_type, buf);
		transmit_response(p, "405 Method Not Allowed", req); /* Good enough, or? */
	}
	ast_set_flag(p, SIP_NEEDDESTROY);
	return;
}

/*! \brief  CLI Command to show calls within limits set by call_limit */
static int sip_show_inuse(int fd, int argc, char *argv[]) {
#define FORMAT  "%-25.25s %-15.15s %-15.15s \n"
#define FORMAT2 "%-25.25s %-15.15s %-15.15s \n"
	char ilimits[40];
	char iused[40];
	int showall = 0;

	if (argc < 3) 
		return RESULT_SHOWUSAGE;

	if (argc == 4 && !strcmp(argv[3],"all")) 
			showall = 1;
	
	ast_cli(fd, FORMAT, "* User name", "In use", "Limit");
	ASTOBJ_CONTAINER_TRAVERSE(&userl, 1, do {
		ASTOBJ_RDLOCK(iterator);
		if (iterator->call_limit)
			snprintf(ilimits, sizeof(ilimits), "%d", iterator->call_limit);
		else 
			ast_copy_string(ilimits, "N/A", sizeof(ilimits));
		snprintf(iused, sizeof(iused), "%d", iterator->inUse);
		if (showall || iterator->call_limit)
			ast_cli(fd, FORMAT2, iterator->name, iused, ilimits);
		ASTOBJ_UNLOCK(iterator);
	} while (0) );

	ast_cli(fd, FORMAT, "* Peer name", "In use", "Limit");

	ASTOBJ_CONTAINER_TRAVERSE(&peerl, 1, do {
		ASTOBJ_RDLOCK(iterator);
		if (iterator->call_limit)
			snprintf(ilimits, sizeof(ilimits), "%d", iterator->call_limit);
		else 
			ast_copy_string(ilimits, "N/A", sizeof(ilimits));
		snprintf(iused, sizeof(iused), "%d", iterator->inUse);
		if (showall || iterator->call_limit)
			ast_cli(fd, FORMAT2, iterator->name, iused, ilimits);
		ASTOBJ_UNLOCK(iterator);
	} while (0) );

	return RESULT_SUCCESS;
#undef FORMAT
#undef FORMAT2
}

/*! \brief  Convert NAT setting to text string */
static char *nat2str(int nat)
{
	switch(nat) {
	case SIP_NAT_NEVER:
		return "No";
	case SIP_NAT_ROUTE:
		return "Route";
	case SIP_NAT_ALWAYS:
		return "Always";
	case SIP_NAT_RFC3581:
		return "RFC3581";
	default:
		return "Unknown";
	}
}

/*! \brief  Report Peer status in character string */
/* 	returns 1 if peer is online, -1 if unmonitored */
static int peer_status(struct sip_peer *peer, char *status, int statuslen)
{
	int res = 0;
	if (peer->maxms) {
		if (peer->lastms < 0) {
			ast_copy_string(status, "UNREACHABLE", statuslen);
		} else if (peer->lastms > peer->maxms) {
			snprintf(status, statuslen, "LAGGED (%d ms)", peer->lastms);
			res = 1;
		} else if (peer->lastms) {
			snprintf(status, statuslen, "OK (%d ms)", peer->lastms);
			res = 1;
		} else {
			ast_copy_string(status, "UNKNOWN", statuslen);
		}
	} else { 
		ast_copy_string(status, "Unmonitored", statuslen);
		/* Checking if port is 0 */
		res = -1;
	}
	return res;
}
                           
/*! \brief  CLI Command 'SIP Show Users' */
static int sip_show_users(int fd, int argc, char *argv[])
{
	regex_t regexbuf;
	int havepattern = 0;

#define FORMAT  "%-25.25s  %-15.15s  %-15.15s  %-15.15s  %-5.5s%-10.10s\n"

	switch (argc) {
	case 5:
		if (!strcasecmp(argv[3], "like")) {
			if (regcomp(&regexbuf, argv[4], REG_EXTENDED | REG_NOSUB))
				return RESULT_SHOWUSAGE;
			havepattern = 1;
		} else
			return RESULT_SHOWUSAGE;
	case 3:
		break;
	default:
		return RESULT_SHOWUSAGE;
	}

	ast_cli(fd, FORMAT, "Username", "Secret", "Accountcode", "Def.Context", "ACL", "NAT");
	ASTOBJ_CONTAINER_TRAVERSE(&userl, 1, do {
		ASTOBJ_RDLOCK(iterator);

		if (havepattern && regexec(&regexbuf, iterator->name, 0, NULL, 0)) {
			ASTOBJ_UNLOCK(iterator);
			continue;
		}

		ast_cli(fd, FORMAT, iterator->name, 
			iterator->secret, 
			iterator->accountcode,
			iterator->context,
			iterator->ha ? "Yes" : "No",
			nat2str(ast_test_flag(iterator, SIP_NAT)));
		ASTOBJ_UNLOCK(iterator);
	} while (0)
	);

	if (havepattern)
		regfree(&regexbuf);

	return RESULT_SUCCESS;
#undef FORMAT
}

static char mandescr_show_peers[] = 
"Description: Lists SIP peers in text format with details on current status.\n"
"Variables: \n"
"  ActionID: <id>	Action ID for this transaction. Will be returned.\n";

static int _sip_show_peers(int fd, int *total, struct mansession *s, struct message *m, int argc, char *argv[]);

/*! \brief  Show SIP peers in the manager API */
/*    Inspired from chan_iax2 */
static int manager_sip_show_peers( struct mansession *s, struct message *m )
{
	char *id = astman_get_header(m,"ActionID");
	char *a[] = { "sip", "show", "peers" };
	char idtext[256] = "";
	int total = 0;

	if (!ast_strlen_zero(id))
		snprintf(idtext,256,"ActionID: %s\r\n",id);

	astman_send_ack(s, m, "Peer status list will follow");
	/* List the peers in separate manager events */
	_sip_show_peers(s->fd, &total, s, m, 3, a);
	/* Send final confirmation */
	ast_cli(s->fd,
	"Event: PeerlistComplete\r\n"
	"ListItems: %d\r\n"
	"%s"
	"\r\n", total, idtext);
	return 0;
}

/*! \brief  CLI Show Peers command */
static int sip_show_peers(int fd, int argc, char *argv[])
{
	return _sip_show_peers(fd, NULL, NULL, NULL, argc, argv);
}

/*! \brief  _sip_show_peers: Execute sip show peers command */
static int _sip_show_peers(int fd, int *total, struct mansession *s, struct message *m, int argc, char *argv[])
{
	regex_t regexbuf;
	int havepattern = 0;

#define FORMAT2 "%-25.25s  %-15.15s %-3.3s %-3.3s %-3.3s %-8s %-10s\n"
#define FORMAT  "%-25.25s  %-15.15s %-3.3s %-3.3s %-3.3s %-8d %-10s\n"

	char name[256];
	char iabuf[INET_ADDRSTRLEN];
	int total_peers = 0;
	int peers_online = 0;
	int peers_offline = 0;
	char *id;
	char idtext[256] = "";

	if (s) {	/* Manager - get ActionID */
		id = astman_get_header(m,"ActionID");
		if (!ast_strlen_zero(id))
			snprintf(idtext,256,"ActionID: %s\r\n",id);
	}

	switch (argc) {
	case 5:
		if (!strcasecmp(argv[3], "like")) {
			if (regcomp(&regexbuf, argv[4], REG_EXTENDED | REG_NOSUB))
				return RESULT_SHOWUSAGE;
			havepattern = 1;
		} else
			return RESULT_SHOWUSAGE;
	case 3:
		break;
	default:
		return RESULT_SHOWUSAGE;
	}

	if (!s) { /* Normal list */
		ast_cli(fd, FORMAT2, "Name/username", "Host", "Dyn", "Nat", "ACL", "Port", "Status");
	} 
	
	ASTOBJ_CONTAINER_TRAVERSE(&peerl, 1, do {
		char status[20] = "";
		char srch[2000];
		char pstatus;
		
		ASTOBJ_RDLOCK(iterator);

		if (havepattern && regexec(&regexbuf, iterator->name, 0, NULL, 0)) {
			ASTOBJ_UNLOCK(iterator);
			continue;
		}

		if (!ast_strlen_zero(iterator->username) && !s)
			snprintf(name, sizeof(name), "%s/%s", iterator->name, iterator->username);
		else
			ast_copy_string(name, iterator->name, sizeof(name));

		pstatus = peer_status(iterator, status, sizeof(status));
		if (pstatus) 	
			peers_online++;
		else	{
			if (pstatus == 0)
				peers_offline++;
			else {	/* Unmonitored */
				/* Checking if port is 0 */
				if ( ntohs(iterator->addr.sin_port) == 0 ) {
					peers_offline++;
				} else {
					peers_online++;
				}
			}
		}			
		
		snprintf(srch, sizeof(srch), FORMAT, name,
			iterator->addr.sin_addr.s_addr ? ast_inet_ntoa(iabuf, sizeof(iabuf), iterator->addr.sin_addr) : "(Unspecified)",
			ast_test_flag(iterator, SIP_DYNAMIC) ? " D " : "   ", 	/* Dynamic or not? */
			(ast_test_flag(iterator, SIP_NAT) & SIP_NAT_ROUTE) ? " N " : "   ",	/* NAT=yes? */
			iterator->ha ? " A " : "   ", 	/* permit/deny */
			ntohs(iterator->addr.sin_port), status);

		if (!s)  {/* Normal CLI list */
			ast_cli(fd, FORMAT, name, 
			iterator->addr.sin_addr.s_addr ? ast_inet_ntoa(iabuf, sizeof(iabuf), iterator->addr.sin_addr) : "(Unspecified)",
			ast_test_flag(iterator, SIP_DYNAMIC) ? " D " : "   ",  /* Dynamic or not? */
			(ast_test_flag(iterator, SIP_NAT) & SIP_NAT_ROUTE) ? " N " : "   ",	/* NAT=yes? */
			iterator->ha ? " A " : "   ",       /* permit/deny */
			
			ntohs(iterator->addr.sin_port), status);
		} else {	/* Manager format */
			/* The names here need to be the same as other channels */
			ast_cli(fd, 
			"Event: PeerEntry\r\n%s"
			"Channeltype: SIP\r\n"
			"ObjectName: %s\r\n"
			"ChanObjectType: peer\r\n"	/* "peer" or "user" */
			"IPaddress: %s\r\n"
			"IPport: %d\r\n"
			"Dynamic: %s\r\n"
			"Natsupport: %s\r\n"
			"ACL: %s\r\n"
			"Status: %s\r\n\r\n", 
			idtext,
			iterator->name, 
			iterator->addr.sin_addr.s_addr ? ast_inet_ntoa(iabuf, sizeof(iabuf), iterator->addr.sin_addr) : "-none-",
			ntohs(iterator->addr.sin_port), 
			ast_test_flag(iterator, SIP_DYNAMIC) ? "yes" : "no",  /* Dynamic or not? */
			(ast_test_flag(iterator, SIP_NAT) & SIP_NAT_ROUTE) ? "yes" : "no",	/* NAT=yes? */
			iterator->ha ? "yes" : "no",       /* permit/deny */
			status);
		}

		ASTOBJ_UNLOCK(iterator);

		total_peers++;
	} while(0) );

	if (!s) {
		ast_cli(fd,"%d sip peers [%d online , %d offline]\n",total_peers,peers_online,peers_offline);
	}

	if (havepattern)
		regfree(&regexbuf);

	if (total)
		*total = total_peers;
	

	return RESULT_SUCCESS;
#undef FORMAT
#undef FORMAT2
}

/*! \brief  sip_show_objects: List all allocated SIP Objects */
static int sip_show_objects(int fd, int argc, char *argv[])
{
	char tmp[256];
	if (argc != 3)
		return RESULT_SHOWUSAGE;
	ast_cli(fd, "-= User objects: %d static, %d realtime =-\n\n", suserobjs, ruserobjs);
	ASTOBJ_CONTAINER_DUMP(fd, tmp, sizeof(tmp), &userl);
	ast_cli(fd, "-= Peer objects: %d static, %d realtime, %d autocreate =-\n\n", speerobjs, rpeerobjs, apeerobjs);
	ASTOBJ_CONTAINER_DUMP(fd, tmp, sizeof(tmp), &peerl);
	ast_cli(fd, "-= Registry objects: %d =-\n\n", regobjs);
	ASTOBJ_CONTAINER_DUMP(fd, tmp, sizeof(tmp), &regl);
	return RESULT_SUCCESS;
}
/*! \brief  print_group: Print call group and pickup group */
static void  print_group(int fd, unsigned int group, int crlf) 
{
	char buf[256];
	ast_cli(fd, crlf ? "%s\r\n" : "%s\n", ast_print_group(buf, sizeof(buf), group) );
}

/*! \brief  dtmfmode2str: Convert DTMF mode to printable string */
static const char *dtmfmode2str(int mode)
{
	switch (mode) {
	case SIP_DTMF_RFC2833:
		return "rfc2833";
	case SIP_DTMF_INFO:
		return "info";
	case SIP_DTMF_INBAND:
		return "inband";
	case SIP_DTMF_AUTO:
		return "auto";
	}
	return "<error>";
}

/*! \brief  insecure2str: Convert Insecure setting to printable string */
static const char *insecure2str(int port, int invite)
{
	if (port && invite)
		return "port,invite";
	else if (port)
		return "port";
	else if (invite)
		return "invite";
	else
		return "no";
}

/*! \brief  sip_prune_realtime: Remove temporary realtime objects from memory (CLI) */
static int sip_prune_realtime(int fd, int argc, char *argv[])
{
	struct sip_peer *peer;
	struct sip_user *user;
	int pruneuser = 0;
	int prunepeer = 0;
	int multi = 0;
	char *name = NULL;
	regex_t regexbuf;

	switch (argc) {
	case 4:
		if (!strcasecmp(argv[3], "user"))
			return RESULT_SHOWUSAGE;
		if (!strcasecmp(argv[3], "peer"))
			return RESULT_SHOWUSAGE;
		if (!strcasecmp(argv[3], "like"))
			return RESULT_SHOWUSAGE;
		if (!strcasecmp(argv[3], "all")) {
			multi = 1;
			pruneuser = prunepeer = 1;
		} else {
			pruneuser = prunepeer = 1;
			name = argv[3];
		}
		break;
	case 5:
		if (!strcasecmp(argv[4], "like"))
			return RESULT_SHOWUSAGE;
		if (!strcasecmp(argv[3], "all"))
			return RESULT_SHOWUSAGE;
		if (!strcasecmp(argv[3], "like")) {
			multi = 1;
			name = argv[4];
			pruneuser = prunepeer = 1;
		} else if (!strcasecmp(argv[3], "user")) {
			pruneuser = 1;
			if (!strcasecmp(argv[4], "all"))
				multi = 1;
			else
				name = argv[4];
		} else if (!strcasecmp(argv[3], "peer")) {
			prunepeer = 1;
			if (!strcasecmp(argv[4], "all"))
				multi = 1;
			else
				name = argv[4];
		} else
			return RESULT_SHOWUSAGE;
		break;
	case 6:
		if (strcasecmp(argv[4], "like"))
			return RESULT_SHOWUSAGE;
		if (!strcasecmp(argv[3], "user")) {
			pruneuser = 1;
			name = argv[5];
		} else if (!strcasecmp(argv[3], "peer")) {
			prunepeer = 1;
			name = argv[5];
		} else
			return RESULT_SHOWUSAGE;
		break;
	default:
		return RESULT_SHOWUSAGE;
	}

	if (multi && name) {
		if (regcomp(&regexbuf, name, REG_EXTENDED | REG_NOSUB))
			return RESULT_SHOWUSAGE;
	}

	if (multi) {
		if (prunepeer) {
			int pruned = 0;

			ASTOBJ_CONTAINER_WRLOCK(&peerl);
			ASTOBJ_CONTAINER_TRAVERSE(&peerl, 1, do {
				ASTOBJ_RDLOCK(iterator);
				if (name && regexec(&regexbuf, iterator->name, 0, NULL, 0)) {
					ASTOBJ_UNLOCK(iterator);
					continue;
				};
				if (ast_test_flag((&iterator->flags_page2), SIP_PAGE2_RTCACHEFRIENDS)) {
					ASTOBJ_MARK(iterator);
					pruned++;
				}
				ASTOBJ_UNLOCK(iterator);
			} while (0) );
			if (pruned) {
				ASTOBJ_CONTAINER_PRUNE_MARKED(&peerl, sip_destroy_peer);
				ast_cli(fd, "%d peers pruned.\n", pruned);
			} else
				ast_cli(fd, "No peers found to prune.\n");
			ASTOBJ_CONTAINER_UNLOCK(&peerl);
		}
		if (pruneuser) {
			int pruned = 0;

			ASTOBJ_CONTAINER_WRLOCK(&userl);
			ASTOBJ_CONTAINER_TRAVERSE(&userl, 1, do {
				ASTOBJ_RDLOCK(iterator);
				if (name && regexec(&regexbuf, iterator->name, 0, NULL, 0)) {
					ASTOBJ_UNLOCK(iterator);
					continue;
				};
				if (ast_test_flag((&iterator->flags_page2), SIP_PAGE2_RTCACHEFRIENDS)) {
					ASTOBJ_MARK(iterator);
					pruned++;
				}
				ASTOBJ_UNLOCK(iterator);
			} while (0) );
			if (pruned) {
				ASTOBJ_CONTAINER_PRUNE_MARKED(&userl, sip_destroy_user);
				ast_cli(fd, "%d users pruned.\n", pruned);
			} else
				ast_cli(fd, "No users found to prune.\n");
			ASTOBJ_CONTAINER_UNLOCK(&userl);
		}
	} else {
		if (prunepeer) {
			if ((peer = ASTOBJ_CONTAINER_FIND_UNLINK(&peerl, name))) {
				if (!ast_test_flag((&peer->flags_page2), SIP_PAGE2_RTCACHEFRIENDS)) {
					ast_cli(fd, "Peer '%s' is not a Realtime peer, cannot be pruned.\n", name);
					ASTOBJ_CONTAINER_LINK(&peerl, peer);
				} else
					ast_cli(fd, "Peer '%s' pruned.\n", name);
				ASTOBJ_UNREF(peer, sip_destroy_peer);
			} else
				ast_cli(fd, "Peer '%s' not found.\n", name);
		}
		if (pruneuser) {
			if ((user = ASTOBJ_CONTAINER_FIND_UNLINK(&userl, name))) {
				if (!ast_test_flag((&user->flags_page2), SIP_PAGE2_RTCACHEFRIENDS)) {
					ast_cli(fd, "User '%s' is not a Realtime user, cannot be pruned.\n", name);
					ASTOBJ_CONTAINER_LINK(&userl, user);
				} else
					ast_cli(fd, "User '%s' pruned.\n", name);
				ASTOBJ_UNREF(user, sip_destroy_user);
			} else
				ast_cli(fd, "User '%s' not found.\n", name);
		}
	}

	return RESULT_SUCCESS;
}

/*! \brief  print_codec_to_cli: Print codec list from preference to CLI/manager */
static void print_codec_to_cli(int fd, struct ast_codec_pref *pref) 
{
	int x, codec;

	for(x = 0; x < 32 ; x++) {
		codec = ast_codec_pref_index(pref, x);
		if (!codec)
			break;
		ast_cli(fd, "%s", ast_getformatname(codec));
		if (x < 31 && ast_codec_pref_index(pref, x + 1))
			ast_cli(fd, ",");
	}
	if (!x)
		ast_cli(fd, "none");
}

static const char *domain_mode_to_text(const enum domain_mode mode)
{
	switch (mode) {
	case SIP_DOMAIN_AUTO:
		return "[Automatic]";
	case SIP_DOMAIN_CONFIG:
		return "[Configured]";
	}

	return "";
}

/*! \brief  sip_show_domains: CLI command to list local domains */
#define FORMAT "%-40.40s %-20.20s %-16.16s\n"
static int sip_show_domains(int fd, int argc, char *argv[])
{
	struct domain *d;

	if (AST_LIST_EMPTY(&domain_list)) {
		ast_cli(fd, "SIP Domain support not enabled.\n\n");
		return RESULT_SUCCESS;
	} else {
		ast_cli(fd, FORMAT, "Our local SIP domains:", "Context", "Set by");
		AST_LIST_LOCK(&domain_list);
		AST_LIST_TRAVERSE(&domain_list, d, list)
			ast_cli(fd, FORMAT, d->domain, ast_strlen_zero(d->context) ? "(default)": d->context,
				domain_mode_to_text(d->mode));
		AST_LIST_UNLOCK(&domain_list);
		ast_cli(fd, "\n");
		return RESULT_SUCCESS;
	}
}
#undef FORMAT

static char mandescr_show_peer[] = 
"Description: Show one SIP peer with details on current status.\n"
"  The XML format is under development, feedback welcome! /oej\n"
"Variables: \n"
"  Peer: <name>           The peer name you want to check.\n"
"  ActionID: <id>	  Optional action ID for this AMI transaction.\n";

static int _sip_show_peer(int type, int fd, struct mansession *s, struct message *m, int argc, char *argv[]);

/*! \brief  manager_sip_show_peer: Show SIP peers in the manager API  */
static int manager_sip_show_peer( struct mansession *s, struct message *m )
{
	char *id = astman_get_header(m,"ActionID");
	char *a[4];
	char *peer;
	int ret;

	peer = astman_get_header(m,"Peer");
	if (ast_strlen_zero(peer)) {
		astman_send_error(s, m, "Peer: <name> missing.\n");
		return 0;
	}
	a[0] = "sip";
	a[1] = "show";
	a[2] = "peer";
	a[3] = peer;

	if (!ast_strlen_zero(id))
		ast_cli(s->fd, "ActionID: %s\r\n",id);
	ret = _sip_show_peer(1, s->fd, s, m, 4, a );
	ast_cli( s->fd, "\r\n\r\n" );
	return ret;
}



/*! \brief  sip_show_peer: Show one peer in detail */
static int sip_show_peer(int fd, int argc, char *argv[])
{
	return _sip_show_peer(0, fd, NULL, NULL, argc, argv);
}

static int _sip_show_peer(int type, int fd, struct mansession *s, struct message *m, int argc, char *argv[])
{
	char status[30] = "";
	char cbuf[256];
	char iabuf[INET_ADDRSTRLEN];
	struct sip_peer *peer;
	char codec_buf[512];
	struct ast_codec_pref *pref;
	struct ast_variable *v;
	struct sip_auth *auth;
	int x = 0, codec = 0, load_realtime = 0;

	if (argc < 4)
		return RESULT_SHOWUSAGE;

	load_realtime = (argc == 5 && !strcmp(argv[4], "load")) ? 1 : 0;
	peer = find_peer(argv[3], NULL, load_realtime);
	if (s) { 	/* Manager */
		if (peer)
			ast_cli(s->fd, "Response: Success\r\n");
		else {
			snprintf (cbuf, sizeof(cbuf), "Peer %s not found.\n", argv[3]);
			astman_send_error(s, m, cbuf);
			return 0;
		}
	}
	if (peer && type==0 ) { /* Normal listing */
		ast_cli(fd,"\n\n");
		ast_cli(fd, "  * Name       : %s\n", peer->name);
		ast_cli(fd, "  Secret       : %s\n", ast_strlen_zero(peer->secret)?"<Not set>":"<Set>");
		ast_cli(fd, "  MD5Secret    : %s\n", ast_strlen_zero(peer->md5secret)?"<Not set>":"<Set>");
		auth = peer->auth;
		while(auth) {
			ast_cli(fd, "  Realm-auth   : Realm %-15.15s User %-10.20s ", auth->realm, auth->username);
			ast_cli(fd, "%s\n", !ast_strlen_zero(auth->secret)?"<Secret set>":(!ast_strlen_zero(auth->md5secret)?"<MD5secret set>" : "<Not set>"));
			auth = auth->next;
		}
		ast_cli(fd, "  Context      : %s\n", peer->context);
		ast_cli(fd, "  Subscr.Cont. : %s\n", ast_strlen_zero(peer->subscribecontext)?"<Not set>":peer->subscribecontext);
		ast_cli(fd, "  Language     : %s\n", peer->language);
		if (!ast_strlen_zero(peer->accountcode))
			ast_cli(fd, "  Accountcode  : %s\n", peer->accountcode);
		ast_cli(fd, "  AMA flags    : %s\n", ast_cdr_flags2str(peer->amaflags));
		ast_cli(fd, "  CallingPres  : %s\n", ast_describe_caller_presentation(peer->callingpres));
		if (!ast_strlen_zero(peer->fromuser))
			ast_cli(fd, "  FromUser     : %s\n", peer->fromuser);
		if (!ast_strlen_zero(peer->fromdomain))
			ast_cli(fd, "  FromDomain   : %s\n", peer->fromdomain);
		ast_cli(fd, "  Callgroup    : ");
		print_group(fd, peer->callgroup, 0);
		ast_cli(fd, "  Pickupgroup  : ");
		print_group(fd, peer->pickupgroup, 0);
		ast_cli(fd, "  Mailbox      : %s\n", peer->mailbox);
		ast_cli(fd, "  VM Extension : %s\n", peer->vmexten);
		ast_cli(fd, "  LastMsgsSent : %d\n", peer->lastmsgssent);
		ast_cli(fd, "  Call limit   : %d\n", peer->call_limit);
		ast_cli(fd, "  Dynamic      : %s\n", (ast_test_flag(peer, SIP_DYNAMIC)?"Yes":"No"));
		ast_cli(fd, "  Callerid     : %s\n", ast_callerid_merge(cbuf, sizeof(cbuf), peer->cid_name, peer->cid_num, "<unspecified>"));
		ast_cli(fd, "  Expire       : %d\n", peer->expire);
		ast_cli(fd, "  Insecure     : %s\n", insecure2str(ast_test_flag(peer, SIP_INSECURE_PORT), ast_test_flag(peer, SIP_INSECURE_INVITE)));
		ast_cli(fd, "  Nat          : %s\n", nat2str(ast_test_flag(peer, SIP_NAT)));
		ast_cli(fd, "  ACL          : %s\n", (peer->ha?"Yes":"No"));
		ast_cli(fd, "  CanReinvite  : %s\n", (ast_test_flag(peer, SIP_CAN_REINVITE)?"Yes":"No"));
		ast_cli(fd, "  PromiscRedir : %s\n", (ast_test_flag(peer, SIP_PROMISCREDIR)?"Yes":"No"));
		ast_cli(fd, "  User=Phone   : %s\n", (ast_test_flag(peer, SIP_USEREQPHONE)?"Yes":"No"));
		ast_cli(fd, "  Trust RPID   : %s\n", (ast_test_flag(peer, SIP_TRUSTRPID) ? "Yes" : "No"));
		ast_cli(fd, "  Send RPID    : %s\n", (ast_test_flag(peer, SIP_SENDRPID) ? "Yes" : "No"));

		/* - is enumerated */
		ast_cli(fd, "  DTMFmode     : %s\n", dtmfmode2str(ast_test_flag(peer, SIP_DTMF)));
		ast_cli(fd, "  LastMsg      : %d\n", peer->lastmsg);
		ast_cli(fd, "  ToHost       : %s\n", peer->tohost);
		ast_cli(fd, "  Addr->IP     : %s Port %d\n",  peer->addr.sin_addr.s_addr ? ast_inet_ntoa(iabuf, sizeof(iabuf), peer->addr.sin_addr) : "(Unspecified)", ntohs(peer->addr.sin_port));
		ast_cli(fd, "  Defaddr->IP  : %s Port %d\n", ast_inet_ntoa(iabuf, sizeof(iabuf), peer->defaddr.sin_addr), ntohs(peer->defaddr.sin_port));
		ast_cli(fd, "  Def. Username: %s\n", peer->username);
		ast_cli(fd, "  SIP Options  : ");
		if (peer->sipoptions) {
			for (x=0 ; (x < (sizeof(sip_options) / sizeof(sip_options[0]))); x++) {
				if (peer->sipoptions & sip_options[x].id)
					ast_cli(fd, "%s ", sip_options[x].text);
			}
		} else
			ast_cli(fd, "(none)");

		ast_cli(fd, "\n");
		ast_cli(fd, "  Codecs       : ");
		ast_getformatname_multiple(codec_buf, sizeof(codec_buf) -1, peer->capability);
		ast_cli(fd, "%s\n", codec_buf);
		ast_cli(fd, "  Codec Order  : (");
		print_codec_to_cli(fd, &peer->prefs);

		ast_cli(fd, ")\n");

		ast_cli(fd, "  Status       : ");
		peer_status(peer, status, sizeof(status));
		ast_cli(fd, "%s\n",status);
 		ast_cli(fd, "  Useragent    : %s\n", peer->useragent);
 		ast_cli(fd, "  Reg. Contact : %s\n", peer->fullcontact);
		if (peer->chanvars) {
 			ast_cli(fd, "  Variables    :\n");
			for (v = peer->chanvars ; v ; v = v->next)
 				ast_cli(fd, "                 %s = %s\n", v->name, v->value);
		}
		ast_cli(fd,"\n");
		ASTOBJ_UNREF(peer,sip_destroy_peer);
	} else  if (peer && type == 1) { /* manager listing */
		char *actionid = astman_get_header(m,"ActionID");

		ast_cli(fd, "Channeltype: SIP\r\n");
		if (actionid)
			ast_cli(fd, "ActionID: %s\r\n", actionid);
		ast_cli(fd, "ObjectName: %s\r\n", peer->name);
		ast_cli(fd, "ChanObjectType: peer\r\n");
		ast_cli(fd, "SecretExist: %s\r\n", ast_strlen_zero(peer->secret)?"N":"Y");
		ast_cli(fd, "MD5SecretExist: %s\r\n", ast_strlen_zero(peer->md5secret)?"N":"Y");
		ast_cli(fd, "Context: %s\r\n", peer->context);
		ast_cli(fd, "Language: %s\r\n", peer->language);
		if (!ast_strlen_zero(peer->accountcode))
			ast_cli(fd, "Accountcode: %s\r\n", peer->accountcode);
		ast_cli(fd, "AMAflags: %s\r\n", ast_cdr_flags2str(peer->amaflags));
		ast_cli(fd, "CID-CallingPres: %s\r\n", ast_describe_caller_presentation(peer->callingpres));
		if (!ast_strlen_zero(peer->fromuser))
			ast_cli(fd, "SIP-FromUser: %s\r\n", peer->fromuser);
		if (!ast_strlen_zero(peer->fromdomain))
			ast_cli(fd, "SIP-FromDomain: %s\r\n", peer->fromdomain);
		ast_cli(fd, "Callgroup: ");
		print_group(fd, peer->callgroup, 1);
		ast_cli(fd, "Pickupgroup: ");
		print_group(fd, peer->pickupgroup, 1);
		ast_cli(fd, "VoiceMailbox: %s\r\n", peer->mailbox);
		ast_cli(fd, "LastMsgsSent: %d\r\n", peer->lastmsgssent);
		ast_cli(fd, "Call limit: %d\r\n", peer->call_limit);
		ast_cli(fd, "Dynamic: %s\r\n", (ast_test_flag(peer, SIP_DYNAMIC)?"Y":"N"));
		ast_cli(fd, "Callerid: %s\r\n", ast_callerid_merge(cbuf, sizeof(cbuf), peer->cid_name, peer->cid_num, ""));
		ast_cli(fd, "RegExpire: %ld seconds\r\n", ast_sched_when(sched,peer->expire));
		ast_cli(fd, "SIP-AuthInsecure: %s\r\n", insecure2str(ast_test_flag(peer, SIP_INSECURE_PORT), ast_test_flag(peer, SIP_INSECURE_INVITE)));
		ast_cli(fd, "SIP-NatSupport: %s\r\n", nat2str(ast_test_flag(peer, SIP_NAT)));
		ast_cli(fd, "ACL: %s\r\n", (peer->ha?"Y":"N"));
		ast_cli(fd, "SIP-CanReinvite: %s\r\n", (ast_test_flag(peer, SIP_CAN_REINVITE)?"Y":"N"));
		ast_cli(fd, "SIP-PromiscRedir: %s\r\n", (ast_test_flag(peer, SIP_PROMISCREDIR)?"Y":"N"));
		ast_cli(fd, "SIP-UserPhone: %s\r\n", (ast_test_flag(peer, SIP_USEREQPHONE)?"Y":"N"));

		/* - is enumerated */
		ast_cli(fd, "SIP-DTMFmode %s\r\n", dtmfmode2str(ast_test_flag(peer, SIP_DTMF)));
		ast_cli(fd, "SIPLastMsg: %d\r\n", peer->lastmsg);
		ast_cli(fd, "ToHost: %s\r\n", peer->tohost);
		ast_cli(fd, "Address-IP: %s\r\nAddress-Port: %d\r\n",  peer->addr.sin_addr.s_addr ? ast_inet_ntoa(iabuf, sizeof(iabuf), peer->addr.sin_addr) : "", ntohs(peer->addr.sin_port));
		ast_cli(fd, "Default-addr-IP: %s\r\nDefault-addr-port: %d\r\n", ast_inet_ntoa(iabuf, sizeof(iabuf), peer->defaddr.sin_addr), ntohs(peer->defaddr.sin_port));
		ast_cli(fd, "Default-Username: %s\r\n", peer->username);
		ast_cli(fd, "Codecs: ");
		ast_getformatname_multiple(codec_buf, sizeof(codec_buf) -1, peer->capability);
		ast_cli(fd, "%s\r\n", codec_buf);
		ast_cli(fd, "CodecOrder: ");
		pref = &peer->prefs;
		for(x = 0; x < 32 ; x++) {
			codec = ast_codec_pref_index(pref,x);
			if (!codec)
				break;
			ast_cli(fd, "%s", ast_getformatname(codec));
			if (x < 31 && ast_codec_pref_index(pref,x+1))
				ast_cli(fd, ",");
		}

		ast_cli(fd, "\r\n");
		ast_cli(fd, "Status: ");
		peer_status(peer, status, sizeof(status));
		ast_cli(fd, "%s\r\n", status);
 		ast_cli(fd, "SIP-Useragent: %s\r\n", peer->useragent);
 		ast_cli(fd, "Reg-Contact : %s\r\n", peer->fullcontact);
		if (peer->chanvars) {
			for (v = peer->chanvars ; v ; v = v->next) {
 				ast_cli(fd, "ChanVariable:\n");
 				ast_cli(fd, " %s,%s\r\n", v->name, v->value);
			}
		}

		ASTOBJ_UNREF(peer,sip_destroy_peer);

	} else {
		ast_cli(fd,"Peer %s not found.\n", argv[3]);
		ast_cli(fd,"\n");
	}

	return RESULT_SUCCESS;
}

/*! \brief  sip_show_user: Show one user in detail */
static int sip_show_user(int fd, int argc, char *argv[])
{
	char cbuf[256];
	struct sip_user *user;
	struct ast_codec_pref *pref;
	struct ast_variable *v;
	int x = 0, codec = 0, load_realtime = 0;

	if (argc < 4)
		return RESULT_SHOWUSAGE;

	/* Load from realtime storage? */
	load_realtime = (argc == 5 && !strcmp(argv[4], "load")) ? 1 : 0;

	user = find_user(argv[3], load_realtime);
	if (user) {
		ast_cli(fd,"\n\n");
		ast_cli(fd, "  * Name       : %s\n", user->name);
		ast_cli(fd, "  Secret       : %s\n", ast_strlen_zero(user->secret)?"<Not set>":"<Set>");
		ast_cli(fd, "  MD5Secret    : %s\n", ast_strlen_zero(user->md5secret)?"<Not set>":"<Set>");
		ast_cli(fd, "  Context      : %s\n", user->context);
		ast_cli(fd, "  Language     : %s\n", user->language);
		if (!ast_strlen_zero(user->accountcode))
			ast_cli(fd, "  Accountcode  : %s\n", user->accountcode);
		ast_cli(fd, "  AMA flags    : %s\n", ast_cdr_flags2str(user->amaflags));
		ast_cli(fd, "  CallingPres  : %s\n", ast_describe_caller_presentation(user->callingpres));
		ast_cli(fd, "  Call limit   : %d\n", user->call_limit);
		ast_cli(fd, "  Callgroup    : ");
		print_group(fd, user->callgroup, 0);
		ast_cli(fd, "  Pickupgroup  : ");
		print_group(fd, user->pickupgroup, 0);
		ast_cli(fd, "  Callerid     : %s\n", ast_callerid_merge(cbuf, sizeof(cbuf), user->cid_name, user->cid_num, "<unspecified>"));
		ast_cli(fd, "  ACL          : %s\n", (user->ha?"Yes":"No"));
		ast_cli(fd, "  Codec Order  : (");
		pref = &user->prefs;
		for(x = 0; x < 32 ; x++) {
			codec = ast_codec_pref_index(pref,x);
			if (!codec)
				break;
			ast_cli(fd, "%s", ast_getformatname(codec));
			if (x < 31 && ast_codec_pref_index(pref,x+1))
				ast_cli(fd, "|");
		}

		if (!x)
			ast_cli(fd, "none");
		ast_cli(fd, ")\n");

		if (user->chanvars) {
 			ast_cli(fd, "  Variables    :\n");
			for (v = user->chanvars ; v ; v = v->next)
 				ast_cli(fd, "                 %s = %s\n", v->name, v->value);
		}
		ast_cli(fd,"\n");
		ASTOBJ_UNREF(user,sip_destroy_user);
	} else {
		ast_cli(fd,"User %s not found.\n", argv[3]);
		ast_cli(fd,"\n");
	}

	return RESULT_SUCCESS;
}

/*! \brief  Show SIP Registry (registrations with other SIP proxies */
static int sip_show_registry(int fd, int argc, char *argv[])
{
#define FORMAT2 "%-30.30s  %-12.12s  %8.8s %-20.20s\n"
#define FORMAT  "%-30.30s  %-12.12s  %8d %-20.20s\n"
	char host[80];

	if (argc != 3)
		return RESULT_SHOWUSAGE;
	ast_cli(fd, FORMAT2, "Host", "Username", "Refresh", "State");
	ASTOBJ_CONTAINER_TRAVERSE(&regl, 1, do {
		ASTOBJ_RDLOCK(iterator);
		snprintf(host, sizeof(host), "%s:%d", iterator->hostname, iterator->portno ? iterator->portno : DEFAULT_SIP_PORT);
		ast_cli(fd, FORMAT, host, iterator->username, iterator->refresh, regstate2str(iterator->regstate));
		ASTOBJ_UNLOCK(iterator);
	} while(0));
	return RESULT_SUCCESS;
#undef FORMAT
#undef FORMAT2
}

/*! \brief  sip_show_settings: List global settings for the SIP channel */
static int sip_show_settings(int fd, int argc, char *argv[])
{
	char tmp[BUFSIZ];
	int realtimepeers = 0;
	int realtimeusers = 0;

	realtimepeers = ast_check_realtime("sippeers");
	realtimeusers = ast_check_realtime("sipusers");

	if (argc != 3)
		return RESULT_SHOWUSAGE;
	ast_cli(fd, "\n\nGlobal Settings:\n");
	ast_cli(fd, "----------------\n");
	ast_cli(fd, "  SIP Port:               %d\n", ntohs(bindaddr.sin_port));
	ast_cli(fd, "  Bindaddress:            %s\n", ast_inet_ntoa(tmp, sizeof(tmp), bindaddr.sin_addr));
	ast_cli(fd, "  Videosupport:           %s\n", global_videosupport ? "Yes" : "No");
	ast_cli(fd, "  AutoCreatePeer:         %s\n", autocreatepeer ? "Yes" : "No");
	ast_cli(fd, "  Allow unknown access:   %s\n", global_allowguest ? "Yes" : "No");
	ast_cli(fd, "  Promsic. redir:         %s\n", ast_test_flag(&global_flags, SIP_PROMISCREDIR) ? "Yes" : "No");
	ast_cli(fd, "  SIP domain support:     %s\n", AST_LIST_EMPTY(&domain_list) ? "No" : "Yes");
	ast_cli(fd, "  Call to non-local dom.: %s\n", allow_external_domains ? "Yes" : "No");
	ast_cli(fd, "  URI user is phone no:   %s\n", ast_test_flag(&global_flags, SIP_USEREQPHONE) ? "Yes" : "No");
	ast_cli(fd, "  Our auth realm          %s\n", global_realm);
	ast_cli(fd, "  Realm. auth:            %s\n", authl ? "Yes": "No");
	ast_cli(fd, "  User Agent:             %s\n", global_useragent);
	ast_cli(fd, "  MWI checking interval:  %d secs\n", global_mwitime);
	ast_cli(fd, "  Reg. context:           %s\n", ast_strlen_zero(regcontext) ? "(not set)" : regcontext);
	ast_cli(fd, "  Caller ID:              %s\n", default_callerid);
	ast_cli(fd, "  From: Domain:           %s\n", default_fromdomain);
	ast_cli(fd, "  Record SIP history:     %s\n", recordhistory ? "On" : "Off");
	ast_cli(fd, "  Call Events:            %s\n", callevents ? "On" : "Off");
	ast_cli(fd, "  IP ToS:                 0x%x\n", global_tos);
#ifdef OSP_SUPPORT
	ast_cli(fd, "  OSP Support:            Yes\n");
#else
	ast_cli(fd, "  OSP Support:            No\n");
#endif
	if (!realtimepeers && !realtimeusers)
		ast_cli(fd, "  SIP realtime:           Disabled\n" );
	else
		ast_cli(fd, "  SIP realtime:           Enabled\n" );

	ast_cli(fd, "\nGlobal Signalling Settings:\n");
	ast_cli(fd, "---------------------------\n");
	ast_cli(fd, "  Codecs:                 ");
	print_codec_to_cli(fd, &prefs);
	ast_cli(fd, "\n");
	ast_cli(fd, "  Relax DTMF:             %s\n", relaxdtmf ? "Yes" : "No");
	ast_cli(fd, "  Compact SIP headers:    %s\n", compactheaders ? "Yes" : "No");
	ast_cli(fd, "  RTP Timeout:            %d %s\n", global_rtptimeout, global_rtptimeout ? "" : "(Disabled)" );
	ast_cli(fd, "  RTP Hold Timeout:       %d %s\n", global_rtpholdtimeout, global_rtpholdtimeout ? "" : "(Disabled)");
	ast_cli(fd, "  MWI NOTIFY mime type:   %s\n", default_notifymime);
	ast_cli(fd, "  DNS SRV lookup:         %s\n", srvlookup ? "Yes" : "No");
	ast_cli(fd, "  Pedantic SIP support:   %s\n", pedanticsipchecking ? "Yes" : "No");
	ast_cli(fd, "  Reg. min duration       %d secs\n", min_expiry);
	ast_cli(fd, "  Reg. max duration:      %d secs\n", max_expiry);
	ast_cli(fd, "  Reg. default duration:  %d secs\n", default_expiry);
	ast_cli(fd, "  Outbound reg. timeout:  %d secs\n", global_reg_timeout);
	ast_cli(fd, "  Outbound reg. attempts: %d\n", global_regattempts_max);
	ast_cli(fd, "  Notify ringing state:   %s\n", global_notifyringing ? "Yes" : "No");
	ast_cli(fd, "\nDefault Settings:\n");
	ast_cli(fd, "-----------------\n");
	ast_cli(fd, "  Context:                %s\n", default_context);
	ast_cli(fd, "  Nat:                    %s\n", nat2str(ast_test_flag(&global_flags, SIP_NAT)));
	ast_cli(fd, "  DTMF:                   %s\n", dtmfmode2str(ast_test_flag(&global_flags, SIP_DTMF)));
	ast_cli(fd, "  Qualify:                %d\n", default_qualify);
	ast_cli(fd, "  Use ClientCode:         %s\n", ast_test_flag(&global_flags, SIP_USECLIENTCODE) ? "Yes" : "No");
	ast_cli(fd, "  Progress inband:        %s\n", (ast_test_flag(&global_flags, SIP_PROG_INBAND) == SIP_PROG_INBAND_NEVER) ? "Never" : (ast_test_flag(&global_flags, SIP_PROG_INBAND) == SIP_PROG_INBAND_NO) ? "No" : "Yes" );
	ast_cli(fd, "  Language:               %s\n", ast_strlen_zero(default_language) ? "(Defaults to English)" : default_language);
	ast_cli(fd, "  Musicclass:             %s\n", default_musicclass);
	ast_cli(fd, "  Voice Mail Extension:   %s\n", default_vmexten);

	
	if (realtimepeers || realtimeusers) {
		ast_cli(fd, "\nRealtime SIP Settings:\n");
		ast_cli(fd, "----------------------\n");
		ast_cli(fd, "  Realtime Peers:         %s\n", realtimepeers ? "Yes" : "No");
		ast_cli(fd, "  Realtime Users:         %s\n", realtimeusers ? "Yes" : "No");
		ast_cli(fd, "  Cache Friends:          %s\n", ast_test_flag(&global_flags_page2, SIP_PAGE2_RTCACHEFRIENDS) ? "Yes" : "No");
		ast_cli(fd, "  Update:                 %s\n", ast_test_flag(&global_flags_page2, SIP_PAGE2_RTUPDATE) ? "Yes" : "No");
		ast_cli(fd, "  Ignore Reg. Expire:     %s\n", ast_test_flag(&global_flags_page2, SIP_PAGE2_IGNOREREGEXPIRE) ? "Yes" : "No");
		ast_cli(fd, "  Auto Clear:             %d\n", global_rtautoclear);
	}
	ast_cli(fd, "\n----\n");
	return RESULT_SUCCESS;
}

/*! \brief  subscription_type2str: Show subscription type in string format */
static const char *subscription_type2str(enum subscriptiontype subtype) {
	int i;

	for (i = 1; (i < (sizeof(subscription_types) / sizeof(subscription_types[0]))); i++) {
		if (subscription_types[i].type == subtype) {
			return subscription_types[i].text;
		}
	}
	return subscription_types[0].text;
}

/*! \brief  find_subscription_type: Find subscription type in array */
static const struct cfsubscription_types *find_subscription_type(enum subscriptiontype subtype) {
	int i;

	for (i = 1; (i < (sizeof(subscription_types) / sizeof(subscription_types[0]))); i++) {
		if (subscription_types[i].type == subtype) {
			return &subscription_types[i];
		}
	}
	return &subscription_types[0];
}

/* Forward declaration */
static int __sip_show_channels(int fd, int argc, char *argv[], int subscriptions);

/*! \brief  sip_show_channels: Show active SIP channels */
static int sip_show_channels(int fd, int argc, char *argv[])  
{
        return __sip_show_channels(fd, argc, argv, 0);
}
 
/*! \brief  sip_show_subscriptions: Show active SIP subscriptions */
static int sip_show_subscriptions(int fd, int argc, char *argv[])
{
        return __sip_show_channels(fd, argc, argv, 1);
}

static int __sip_show_channels(int fd, int argc, char *argv[], int subscriptions)
{
#define FORMAT3 "%-15.15s  %-10.10s  %-11.11s  %-15.15s  %-13.13s  %-15.15s\n"
#define FORMAT2 "%-15.15s  %-10.10s  %-11.11s  %-11.11s  %-4.4s  %-7.7s  %-15.15s\n"
#define FORMAT  "%-15.15s  %-10.10s  %-11.11s  %5.5d/%5.5d  %-4.4s  %-3.3s %-3.3s  %-15.15s\n"
	struct sip_pvt *cur;
	char iabuf[INET_ADDRSTRLEN];
	int numchans = 0;
	if (argc != 3)
		return RESULT_SHOWUSAGE;
	ast_mutex_lock(&iflock);
	cur = iflist;
	if (!subscriptions)
		ast_cli(fd, FORMAT2, "Peer", "User/ANR", "Call ID", "Seq (Tx/Rx)", "Format", "Hold", "Last Message");
	else
		ast_cli(fd, FORMAT3, "Peer", "User", "Call ID", "Extension", "Last state", "Type");
	while (cur) {
		if (cur->subscribed == NONE && !subscriptions) {
			ast_cli(fd, FORMAT, ast_inet_ntoa(iabuf, sizeof(iabuf), cur->sa.sin_addr), 
				ast_strlen_zero(cur->username) ? ( ast_strlen_zero(cur->cid_num) ? "(None)" : cur->cid_num ) : cur->username, 
				cur->callid, 
				cur->ocseq, cur->icseq, 
				ast_getformatname(cur->owner ? cur->owner->nativeformats : 0), 
				ast_test_flag(cur, SIP_CALL_ONHOLD) ? "Yes" : "No",
				ast_test_flag(cur, SIP_NEEDDESTROY) ? "(d)" : "",
				cur->lastmsg );
			numchans++;
		}
		if (cur->subscribed != NONE && subscriptions) {
			ast_cli(fd, FORMAT3, ast_inet_ntoa(iabuf, sizeof(iabuf), cur->sa.sin_addr),
			        ast_strlen_zero(cur->username) ? ( ast_strlen_zero(cur->cid_num) ? "(None)" : cur->cid_num ) : cur->username, 
			        cur->callid, cur->exten, ast_extension_state2str(cur->laststate), 
			        subscription_type2str(cur->subscribed));
			numchans++;
		}
		cur = cur->next;
	}
	ast_mutex_unlock(&iflock);
	if (!subscriptions)
		ast_cli(fd, "%d active SIP channel%s\n", numchans, (numchans != 1) ? "s" : "");
	else
		ast_cli(fd, "%d active SIP subscription%s\n", numchans, (numchans != 1) ? "s" : "");
	return RESULT_SUCCESS;
#undef FORMAT
#undef FORMAT2
#undef FORMAT3
}

/*! \brief  complete_sipch: Support routine for 'sip show channel' CLI */
static char *complete_sipch(const char *line, const char *word, int pos, int state)
{
	int which=0;
	struct sip_pvt *cur;
	char *c = NULL;
	int wordlen = strlen(word);

	ast_mutex_lock(&iflock);
	for (cur = iflist; cur; cur = cur->next) {
		if (!strncasecmp(word, cur->callid, wordlen)) {
			if (++which > state) {
				c = ast_strdup(cur->callid);
				break;
			}
		}
	}
	ast_mutex_unlock(&iflock);
	return c;
}

/*! \brief  complete_sip_peer: Do completion on peer name */
static char *complete_sip_peer(const char *word, const int state, int flags2)
{
	char *result = NULL;
	int wordlen = strlen(word);
	int which = 0;

	ASTOBJ_CONTAINER_TRAVERSE(&peerl, !result, do {
		/* locking of the object is not required because only the name and flags are being compared */
		if (!strncasecmp(word, iterator->name, wordlen)) {
			if (flags2 && !ast_test_flag((&iterator->flags_page2), flags2))
				continue;
			if (++which > state) {
				result = ast_strdup(iterator->name);
			}
		}
	} while(0) );
	return result;
}

/*! \brief  complete_sip_show_peer: Support routine for 'sip show peer' CLI */
static char *complete_sip_show_peer(const char *line, const char *word, int pos, int state)
{
	if (pos == 3)
		return complete_sip_peer(word, state, 0);

	return NULL;
}

/*! \brief  complete_sip_debug_peer: Support routine for 'sip debug peer' CLI */
static char *complete_sip_debug_peer(const char *line, const char *word, int pos, int state)
{
	if (pos == 3)
		return complete_sip_peer(word, state, 0);

	return NULL;
}

/*! \brief  complete_sip_user: Do completion on user name */
static char *complete_sip_user(const char *word, int state, int flags2)
{
	char *result = NULL;
	int wordlen = strlen(word);
	int which = 0;

	ASTOBJ_CONTAINER_TRAVERSE(&userl, !result, do {
		/* locking of the object is not required because only the name and flags are being compared */
		if (!strncasecmp(word, iterator->name, wordlen)) {
			if (flags2 && !ast_test_flag(&(iterator->flags_page2), flags2))
				continue;
			if (++which > state) {
				result = ast_strdup(iterator->name);
			}
		}
	} while(0) );
	return result;
}

/*! \brief  complete_sip_show_user: Support routine for 'sip show user' CLI */
static char *complete_sip_show_user(const char *line, const char *word, int pos, int state)
{
	if (pos == 3)
		return complete_sip_user(word, state, 0);

	return NULL;
}

/*! \brief  complete_sipnotify: Support routine for 'sip notify' CLI */
static char *complete_sipnotify(const char *line, const char *word, int pos, int state)
{
	char *c = NULL;

	if (pos == 2) {
		int which = 0;
		char *cat = NULL;
		int wordlen = strlen(word);

		/* do completion for notify type */

		if (!notify_types)
			return NULL;
		
		while ( (cat = ast_category_browse(notify_types, cat)) ) {
			if (!strncasecmp(word, cat, wordlen)) {
				if (++which > state) {
					c = ast_strdup(cat);
					break;
				}
			}
		}
		return c;
	}

	if (pos > 2)
		return complete_sip_peer(word, state, 0);

	return NULL;
}

/*! \brief  complete_sip_prune_realtime_peer: Support routine for 'sip prune realtime peer' CLI */
static char *complete_sip_prune_realtime_peer(const char *line, const char *word, int pos, int state)
{
	if (pos == 4)
		return complete_sip_peer(word, state, SIP_PAGE2_RTCACHEFRIENDS);
	return NULL;
}

/*! \brief  complete_sip_prune_realtime_user: Support routine for 'sip prune realtime user' CLI */
static char *complete_sip_prune_realtime_user(const char *line, const char *word, int pos, int state)
{
	if (pos == 4)
		return complete_sip_user(word, state, SIP_PAGE2_RTCACHEFRIENDS);

	return NULL;
}

/*! \brief  sip_show_channel: Show details of one call */
static int sip_show_channel(int fd, int argc, char *argv[])
{
	struct sip_pvt *cur;
	char iabuf[INET_ADDRSTRLEN];
	size_t len;
	int found = 0;

	if (argc != 4)
		return RESULT_SHOWUSAGE;
	len = strlen(argv[3]);
	ast_mutex_lock(&iflock);
	for (cur = iflist; cur; cur = cur->next) {
		if (!strncasecmp(cur->callid, argv[3], len)) {
			ast_cli(fd,"\n");
			if (cur->subscribed != NONE)
				ast_cli(fd, "  * Subscription (type: %s)\n", subscription_type2str(cur->subscribed));
			else
				ast_cli(fd, "  * SIP Call\n");
			ast_cli(fd, "  Direction:              %s\n", ast_test_flag(cur, SIP_OUTGOING)?"Outgoing":"Incoming");
			ast_cli(fd, "  Call-ID:                %s\n", cur->callid);
			ast_cli(fd, "  Our Codec Capability:   %d\n", cur->capability);
			ast_cli(fd, "  Non-Codec Capability:   %d\n", cur->noncodeccapability);
			ast_cli(fd, "  Their Codec Capability:   %d\n", cur->peercapability);
			ast_cli(fd, "  Joint Codec Capability:   %d\n", cur->jointcapability);
			ast_cli(fd, "  Format                  %s\n", ast_getformatname(cur->owner ? cur->owner->nativeformats : 0) );
			ast_cli(fd, "  Theoretical Address:    %s:%d\n", ast_inet_ntoa(iabuf, sizeof(iabuf), cur->sa.sin_addr), ntohs(cur->sa.sin_port));
			ast_cli(fd, "  Received Address:       %s:%d\n", ast_inet_ntoa(iabuf, sizeof(iabuf), cur->recv.sin_addr), ntohs(cur->recv.sin_port));
			ast_cli(fd, "  NAT Support:            %s\n", nat2str(ast_test_flag(cur, SIP_NAT)));
			ast_cli(fd, "  Audio IP:               %s %s\n", ast_inet_ntoa(iabuf, sizeof(iabuf), cur->redirip.sin_addr.s_addr ? cur->redirip.sin_addr : cur->ourip), cur->redirip.sin_addr.s_addr ? "(Outside bridge)" : "(local)" );
			ast_cli(fd, "  Our Tag:                %s\n", cur->tag);
			ast_cli(fd, "  Their Tag:              %s\n", cur->theirtag);
			ast_cli(fd, "  SIP User agent:         %s\n", cur->useragent);
			if (!ast_strlen_zero(cur->username))
				ast_cli(fd, "  Username:               %s\n", cur->username);
			if (!ast_strlen_zero(cur->peername))
				ast_cli(fd, "  Peername:               %s\n", cur->peername);
			if (!ast_strlen_zero(cur->uri))
				ast_cli(fd, "  Original uri:           %s\n", cur->uri);
			if (!ast_strlen_zero(cur->cid_num))
				ast_cli(fd, "  Caller-ID:              %s\n", cur->cid_num);
			ast_cli(fd, "  Need Destroy:           %d\n", ast_test_flag(cur, SIP_NEEDDESTROY));
			ast_cli(fd, "  Last Message:           %s\n", cur->lastmsg);
			ast_cli(fd, "  Promiscuous Redir:      %s\n", ast_test_flag(cur, SIP_PROMISCREDIR) ? "Yes" : "No");
			ast_cli(fd, "  Route:                  %s\n", cur->route ? cur->route->hop : "N/A");
			ast_cli(fd, "  DTMF Mode:              %s\n", dtmfmode2str(ast_test_flag(cur, SIP_DTMF)));
			ast_cli(fd, "  SIP Options:            ");
			if (cur->sipoptions) {
				int x;
				for (x=0 ; (x < (sizeof(sip_options) / sizeof(sip_options[0]))); x++) {
					if (cur->sipoptions & sip_options[x].id)
						ast_cli(fd, "%s ", sip_options[x].text);
				}
			} else
				ast_cli(fd, "(none)\n");
			ast_cli(fd, "\n\n");
			found++;
		}
	}
	ast_mutex_unlock(&iflock);
	if (!found) 
		ast_cli(fd, "No such SIP Call ID starting with '%s'\n", argv[3]);
	return RESULT_SUCCESS;
}

/*! \brief  sip_show_history: Show history details of one call */
static int sip_show_history(int fd, int argc, char *argv[])
{
	struct sip_pvt *cur;
	size_t len;
	int found = 0;

	if (argc != 4)
		return RESULT_SHOWUSAGE;
	if (!recordhistory)
		ast_cli(fd, "\n***Note: History recording is currently DISABLED.  Use 'sip history' to ENABLE.\n");
	len = strlen(argv[3]);
	ast_mutex_lock(&iflock);
	for (cur = iflist; cur; cur = cur->next) {
		if (!strncasecmp(cur->callid, argv[3], len)) {
			struct sip_history *hist;
			int x = 0;

			ast_cli(fd,"\n");
			if (cur->subscribed != NONE)
				ast_cli(fd, "  * Subscription\n");
			else
				ast_cli(fd, "  * SIP Call\n");
			if (cur->history)
				AST_LIST_TRAVERSE(cur->history, hist, list)
					ast_cli(fd, "%d. %s\n", x++, hist->event);
			if (x == 0)
				ast_cli(fd, "Call '%s' has no history\n", cur->callid);
			found++;
		}
	}
	ast_mutex_unlock(&iflock);
	if (!found) 
		ast_cli(fd, "No such SIP Call ID starting with '%s'\n", argv[3]);
	return RESULT_SUCCESS;
}

/*! \brief  dump_history: Dump SIP history to debug log file at end of 
  lifespan for SIP dialog */
void sip_dump_history(struct sip_pvt *dialog)
{
	int x = 0;
	struct sip_history *hist;

	if (!dialog)
		return;

	ast_log(LOG_DEBUG, "\n---------- SIP HISTORY for '%s' \n", dialog->callid);
	if (dialog->subscribed)
		ast_log(LOG_DEBUG, "  * Subscription\n");
	else
		ast_log(LOG_DEBUG, "  * SIP Call\n");
	if (dialog->history)
		AST_LIST_TRAVERSE(dialog->history, hist, list)
			ast_log(LOG_DEBUG, "  %d. %s\n", x++, hist->event);
	if (!x)
		ast_log(LOG_DEBUG, "Call '%s' has no history\n", dialog->callid);
	ast_log(LOG_DEBUG, "\n---------- END SIP HISTORY for '%s' \n", dialog->callid);
	
}


/*! \brief  handle_request_info: Receive SIP INFO Message */
/*    Doesn't read the duration of the DTMF signal */
static void handle_request_info(struct sip_pvt *p, struct sip_request *req)
{
	char buf[1024];
	unsigned int event;
	char *c;
	
	/* Need to check the media/type */
	c = get_header(req, "Content-Type");
	if (!strcasecmp(c, "application/dtmf-relay") ||
	    !strcasecmp(c, "application/vnd.nortelnetworks.digits")) {

		/* Try getting the "signal=" part */
		if (ast_strlen_zero(c = get_sdp(req, "Signal")) && ast_strlen_zero(c = get_sdp(req, "d"))) {
			ast_log(LOG_WARNING, "Unable to retrieve DTMF signal from INFO message from %s\n", p->callid);
			transmit_response(p, "200 OK", req); /* Should return error */
			return;
		} else {
			ast_copy_string(buf, c, sizeof(buf));
		}
	
		if (!p->owner) {	/* not a PBX call */
			transmit_response(p, "481 Call leg/transaction does not exist", req);
			ast_set_flag(p, SIP_NEEDDESTROY);
			return;
		}

		if (ast_strlen_zero(buf)) {
			transmit_response(p, "200 OK", req);
			return;
		}

		if (buf[0] == '*')
			event = 10;
		else if (buf[0] == '#')
			event = 11;
		else if ((buf[0] >= 'A') && (buf[0] <= 'D'))
			event = 12 + buf[0] - 'A';
		else
			event = atoi(buf);
		if (event == 16) {
			/* send a FLASH event */
			struct ast_frame f = { AST_FRAME_CONTROL, AST_CONTROL_FLASH, };
			ast_queue_frame(p->owner, &f);
			if (sipdebug)
				ast_verbose("* DTMF-relay event received: FLASH\n");
		} else {
			/* send a DTMF event */
			struct ast_frame f = { AST_FRAME_DTMF, };
			if (event < 10) {
				f.subclass = '0' + event;
			} else if (event < 11) {
				f.subclass = '*';
			} else if (event < 12) {
				f.subclass = '#';
			} else if (event < 16) {
				f.subclass = 'A' + (event - 12);
			}
			ast_queue_frame(p->owner, &f);
			if (sipdebug)
				ast_verbose("* DTMF-relay event received: %c\n", f.subclass);
		}
		transmit_response(p, "200 OK", req);
		return;
	} else if (!strcasecmp(c, "application/media_control+xml")) {
		/* Eh, we'll just assume it's a fast picture update for now */
		if (p->owner)
			ast_queue_control(p->owner, AST_CONTROL_VIDUPDATE);
		transmit_response(p, "200 OK", req);
		return;
	} else if (!ast_strlen_zero(c = get_header(req, "X-ClientCode"))) {
		/* Client code (from SNOM phone) */
		if (ast_test_flag(p, SIP_USECLIENTCODE)) {
			if (p->owner && p->owner->cdr)
				ast_cdr_setuserfield(p->owner, c);
			if (p->owner && ast_bridged_channel(p->owner) && ast_bridged_channel(p->owner)->cdr)
				ast_cdr_setuserfield(ast_bridged_channel(p->owner), c);
			transmit_response(p, "200 OK", req);
		} else {
			transmit_response(p, "403 Unauthorized", req);
		}
		return;
	}
	/* Other type of INFO message, not really understood by Asterisk */
	/* if (get_msg_text(buf, sizeof(buf), req)) { */

	ast_log(LOG_WARNING, "Unable to parse INFO message from %s. Content %s\n", p->callid, buf);
	transmit_response(p, "415 Unsupported media type", req);
	return;
}

/*! \brief  sip_do_debug: Enable SIP Debugging in CLI */
static int sip_do_debug_ip(int fd, int argc, char *argv[])
{
	struct hostent *hp;
	struct ast_hostent ahp;
	char iabuf[INET_ADDRSTRLEN];
	int port = 0;
	char *p, *arg;

	if (argc != 4)
		return RESULT_SHOWUSAGE;
	p = arg = argv[3];
	strsep(&p, ":");
	if (p)
		port = atoi(p);
	hp = ast_gethostbyname(arg, &ahp);
	if (hp == NULL)  {
		return RESULT_SHOWUSAGE;
	}
	debugaddr.sin_family = AF_INET;
	memcpy(&debugaddr.sin_addr, hp->h_addr, sizeof(debugaddr.sin_addr));
	debugaddr.sin_port = htons(port);
	if (port == 0)
		ast_cli(fd, "SIP Debugging Enabled for IP: %s\n", ast_inet_ntoa(iabuf, sizeof(iabuf), debugaddr.sin_addr));
	else
		ast_cli(fd, "SIP Debugging Enabled for IP: %s:%d\n", ast_inet_ntoa(iabuf, sizeof(iabuf), debugaddr.sin_addr), port);

	ast_set_flag(&global_flags_page2, SIP_PAGE2_DEBUG_CONSOLE);

	return RESULT_SUCCESS;
}

/*! \brief  sip_do_debug_peer: Turn on SIP debugging with peer mask */
static int sip_do_debug_peer(int fd, int argc, char *argv[])
{
	struct sip_peer *peer;
	char iabuf[INET_ADDRSTRLEN];
	if (argc != 4)
		return RESULT_SHOWUSAGE;
	peer = find_peer(argv[3], NULL, 1);
	if (peer) {
		if (peer->addr.sin_addr.s_addr) {
			debugaddr.sin_family = AF_INET;
			memcpy(&debugaddr.sin_addr, &peer->addr.sin_addr, sizeof(debugaddr.sin_addr));
			debugaddr.sin_port = peer->addr.sin_port;
			ast_cli(fd, "SIP Debugging Enabled for IP: %s:%d\n", ast_inet_ntoa(iabuf, sizeof(iabuf), debugaddr.sin_addr), ntohs(debugaddr.sin_port));
			ast_set_flag(&global_flags_page2, SIP_PAGE2_DEBUG_CONSOLE);
		} else
			ast_cli(fd, "Unable to get IP address of peer '%s'\n", argv[3]);
		ASTOBJ_UNREF(peer,sip_destroy_peer);
	} else
		ast_cli(fd, "No such peer '%s'\n", argv[3]);
	return RESULT_SUCCESS;
}

/*! \brief  sip_do_debug: Turn on SIP debugging (CLI command) */
static int sip_do_debug(int fd, int argc, char *argv[])
{
	int oldsipdebug = sipdebug_console;
	if (argc != 2) {
		if (argc != 4) 
			return RESULT_SHOWUSAGE;
		else if (strncmp(argv[2], "ip\0", 3) == 0)
			return sip_do_debug_ip(fd, argc, argv);
		else if (strncmp(argv[2], "peer\0", 5) == 0)
			return sip_do_debug_peer(fd, argc, argv);
		else return RESULT_SHOWUSAGE;
	}
	ast_set_flag(&global_flags_page2, SIP_PAGE2_DEBUG_CONSOLE);
	memset(&debugaddr, 0, sizeof(debugaddr));
	if (oldsipdebug)
		ast_cli(fd, "SIP Debugging re-enabled\n");
	else
		ast_cli(fd, "SIP Debugging enabled\n");
	return RESULT_SUCCESS;
}

/*! \brief  sip_notify: Send SIP notify to peer */
static int sip_notify(int fd, int argc, char *argv[])
{
	struct ast_variable *varlist;
	int i;

	if (argc < 4)
		return RESULT_SHOWUSAGE;

	if (!notify_types) {
		ast_cli(fd, "No %s file found, or no types listed there\n", notify_config);
		return RESULT_FAILURE;
	}

	varlist = ast_variable_browse(notify_types, argv[2]);

	if (!varlist) {
		ast_cli(fd, "Unable to find notify type '%s'\n", argv[2]);
		return RESULT_FAILURE;
	}

	for (i = 3; i < argc; i++) {
		struct sip_pvt *p;
		struct sip_request req;
		struct ast_variable *var;

		p = sip_alloc(NULL, NULL, 0, SIP_NOTIFY);
		if (!p) {
			ast_log(LOG_WARNING, "Unable to build sip pvt data for notify\n");
			return RESULT_FAILURE;
		}

		if (create_addr(p, argv[i])) {
			/* Maybe they're not registered, etc. */
			sip_destroy(p);
			ast_cli(fd, "Could not create address for '%s'\n", argv[i]);
			continue;
		}

		initreqprep(&req, p, SIP_NOTIFY);

		for (var = varlist; var; var = var->next)
			add_header(&req, var->name, var->value);

		add_blank_header(&req);
		/* Recalculate our side, and recalculate Call ID */
		if (ast_sip_ouraddrfor(&p->sa.sin_addr, &p->ourip))
			memcpy(&p->ourip, &__ourip, sizeof(p->ourip));
		build_via(p);
		build_callid_pvt(p);
		ast_cli(fd, "Sending NOTIFY of type '%s' to '%s'\n", argv[2], argv[i]);
		transmit_sip_request(p, &req);
		sip_scheddestroy(p, 15000);
	}

	return RESULT_SUCCESS;
}
/*! \brief  sip_do_history: Enable SIP History logging (CLI) */
static int sip_do_history(int fd, int argc, char *argv[])
{
	if (argc != 2) {
		return RESULT_SHOWUSAGE;
	}
	recordhistory = 1;
	ast_cli(fd, "SIP History Recording Enabled (use 'sip show history')\n");
	return RESULT_SUCCESS;
}

/*! \brief  sip_no_history: Disable SIP History logging (CLI) */
static int sip_no_history(int fd, int argc, char *argv[])
{
	if (argc != 3) {
		return RESULT_SHOWUSAGE;
	}
	recordhistory = 0;
	ast_cli(fd, "SIP History Recording Disabled\n");
	return RESULT_SUCCESS;
}

/*! \brief  sip_no_debug: Disable SIP Debugging in CLI */
static int sip_no_debug(int fd, int argc, char *argv[])

{
	if (argc != 3)
		return RESULT_SHOWUSAGE;
	ast_clear_flag(&global_flags_page2, SIP_PAGE2_DEBUG_CONSOLE);
	ast_cli(fd, "SIP Debugging Disabled\n");
	return RESULT_SUCCESS;
}

static int reply_digest(struct sip_pvt *p, struct sip_request *req, char *header, int sipmethod, char *digest, int digest_len);

/*! \brief  do_register_auth: Authenticate for outbound registration */
static int do_register_auth(struct sip_pvt *p, struct sip_request *req, char *header, char *respheader) 
{
	char digest[1024];
	p->authtries++;
	memset(digest,0,sizeof(digest));
	if (reply_digest(p, req, header, SIP_REGISTER, digest, sizeof(digest))) {
		/* There's nothing to use for authentication */
 		/* No digest challenge in request */
 		if (sip_debug_test_pvt(p) && p->registry)
 			ast_verbose("No authentication challenge, sending blank registration to domain/host name %s\n", p->registry->hostname);
 			/* No old challenge */
		return -1;
	}
	if (recordhistory)
		append_history(p, "RegistryAuth", "Try: %d", p->authtries);
 	if (sip_debug_test_pvt(p) && p->registry)
 		ast_verbose("Responding to challenge, registration to domain/host name %s\n", p->registry->hostname);
	return transmit_register(p->registry, SIP_REGISTER, digest, respheader); 
}

/*! \brief  do_proxy_auth: Add authentication on outbound SIP packet */
static int do_proxy_auth(struct sip_pvt *p, struct sip_request *req, char *header, char *respheader, int sipmethod, int init) 
{
	char digest[1024];

	if (!p->options && !(p->options = ast_calloc(1, sizeof(*p->options))))
		return -2;

	p->authtries++;
	if (option_debug > 1)
		ast_log(LOG_DEBUG, "Auth attempt %d on %s\n", p->authtries, sip_methods[sipmethod].text);
	memset(digest, 0, sizeof(digest));
	if (reply_digest(p, req, header, sipmethod, digest, sizeof(digest) )) {
		/* No way to authenticate */
		return -1;
	}
	/* Now we have a reply digest */
	p->options->auth = digest;
	p->options->authheader = respheader;
	return transmit_invite(p, sipmethod, sipmethod == SIP_INVITE, init); 
}

/*! \brief  reply_digest: reply to authentication for outbound registrations */
/*      This is used for register= servers in sip.conf, SIP proxies we register
        with  for receiving calls from.  */
/*	Returns -1 if we have no auth */
static int reply_digest(struct sip_pvt *p, struct sip_request *req,
	char *header, int sipmethod,  char *digest, int digest_len)
{
	char tmp[512];
	char *c;
	char oldnonce[256];

	/* table of recognised keywords, and places where they should be copied */
	const struct x {
		const char *key;
		int field_index;
	} *i, keys[] = {
		{ "realm=", ast_string_field_index(p, realm) },
		{ "nonce=", ast_string_field_index(p, nonce) },
		{ "opaque=", ast_string_field_index(p, opaque) },
		{ "qop=", ast_string_field_index(p, qop) },
		{ "domain=", ast_string_field_index(p, domain) },
		{ NULL, 0 },
	};

	ast_copy_string(tmp, get_header(req, header), sizeof(tmp));
	if (ast_strlen_zero(tmp)) 
		return -1;
	if (strncasecmp(tmp, "Digest ", strlen("Digest "))) {
		ast_log(LOG_WARNING, "missing Digest.\n");
		return -1;
	}
	c = tmp + strlen("Digest ");
	ast_copy_string(oldnonce, p->nonce, sizeof(oldnonce));
	while (c && *(c = ast_skip_blanks(c))) {	/* lookup for keys */
		for (i = keys; i->key != NULL; i++) {
			char *src, *separator;
			if (strncasecmp(c, i->key, strlen(i->key)) != 0)
				continue;
			/* Found. Skip keyword, take text in quotes or up to the separator. */
			c += strlen(i->key);
			if (*c == '\"') {
				src = ++c;
				separator = "\"";
			} else {
				src = c;
				separator = ",";
			}
			strsep(&c, separator); /* clear separator and move ptr */
			ast_string_field_index_set(p, i->field_index, src);
			break;
		}
		if (i->key == NULL) /* not found, try ',' */
			strsep(&c, ",");
	}
	/* Reset nonce count */
	if (strcmp(p->nonce, oldnonce)) 
		p->noncecount = 0;

	/* Save auth data for following registrations */
	if (p->registry) {
		struct sip_registry *r = p->registry;

		if (strcmp(r->nonce, p->nonce)) {
			ast_string_field_set(r, realm, p->realm);
			ast_string_field_set(r, nonce, p->nonce);
			ast_string_field_set(r, domain, p->domain);
			ast_string_field_set(r, opaque, p->opaque);
			ast_string_field_set(r, qop, p->qop);
			r->noncecount = 0;
		}
	}
	return build_reply_digest(p, sipmethod, digest, digest_len); 
}

/*! \brief  build_reply_digest:  Build reply digest */
/*      Build digest challenge for authentication of peers (for registration) 
	and users (for calls). Also used for authentication of CANCEL and BYE */
/*	Returns -1 if we have no auth */
static int build_reply_digest(struct sip_pvt *p, int method, char* digest, int digest_len)
{
	char a1[256];
	char a2[256];
	char a1_hash[256];
	char a2_hash[256];
	char resp[256];
	char resp_hash[256];
	char uri[256];
	char cnonce[80];
	char iabuf[INET_ADDRSTRLEN];
	const char *username;
	const char *secret;
	const char *md5secret;
	struct sip_auth *auth = (struct sip_auth *) NULL;	/* Realm authentication */

	if (!ast_strlen_zero(p->domain))
		ast_copy_string(uri, p->domain, sizeof(uri));
	else if (!ast_strlen_zero(p->uri))
		ast_copy_string(uri, p->uri, sizeof(uri));
	else
		snprintf(uri, sizeof(uri), "sip:%s@%s",p->username, ast_inet_ntoa(iabuf, sizeof(iabuf), p->sa.sin_addr));

	snprintf(cnonce, sizeof(cnonce), "%08x", thread_safe_rand());

 	/* Check if we have separate auth credentials */
 	if ((auth = find_realm_authentication(authl, p->realm))) {
		ast_log(LOG_WARNING, "use realm [%s] from peer [%s][%s]\n",
			auth->username, p->peername, p->username);
 		username = auth->username;
 		secret = auth->secret;
 		md5secret = auth->md5secret;
		if (sipdebug)
 			ast_log(LOG_DEBUG,"Using realm %s authentication for call %s\n", p->realm, p->callid);
 	} else {
 		/* No authentication, use peer or register= config */
 		username = p->authname;
 		secret =  p->peersecret;
 		md5secret = p->peermd5secret;
 	}
	if (ast_strlen_zero(username))	/* We have no authentication */
		return -1;
 

 	/* Calculate SIP digest response */
 	snprintf(a1,sizeof(a1),"%s:%s:%s", username, p->realm, secret);
	snprintf(a2,sizeof(a2),"%s:%s", sip_methods[method].text, uri);
	if (!ast_strlen_zero(md5secret))
		ast_copy_string(a1_hash, md5secret, sizeof(a1_hash));
	else
		ast_md5_hash(a1_hash,a1);
	ast_md5_hash(a2_hash,a2);

	p->noncecount++;
	if (!ast_strlen_zero(p->qop))
		snprintf(resp,sizeof(resp),"%s:%s:%08x:%s:%s:%s", a1_hash, p->nonce, p->noncecount, cnonce, "auth", a2_hash);
	else
		snprintf(resp,sizeof(resp),"%s:%s:%s", a1_hash, p->nonce, a2_hash);
	ast_md5_hash(resp_hash, resp);
	/* XXX We hard code our qop to "auth" for now.  XXX */
	if (!ast_strlen_zero(p->qop))
		snprintf(digest, digest_len, "Digest username=\"%s\", realm=\"%s\", algorithm=MD5, uri=\"%s\", nonce=\"%s\", response=\"%s\", opaque=\"%s\", qop=auth, cnonce=\"%s\", nc=%08x", username, p->realm, uri, p->nonce, resp_hash, p->opaque, cnonce, p->noncecount);
	else
		snprintf(digest, digest_len, "Digest username=\"%s\", realm=\"%s\", algorithm=MD5, uri=\"%s\", nonce=\"%s\", response=\"%s\", opaque=\"%s\"", username, p->realm, uri, p->nonce, resp_hash, p->opaque);

	return 0;
}
	
static char show_domains_usage[] = 
"Usage: sip show domains\n"
"       Lists all configured SIP local domains.\n"
"       Asterisk only responds to SIP messages to local domains.\n";

static char notify_usage[] =
"Usage: sip notify <type> <peer> [<peer>...]\n"
"       Send a NOTIFY message to a SIP peer or peers\n"
"       Message types are defined in sip_notify.conf\n";

static char show_users_usage[] = 
"Usage: sip show users [like <pattern>]\n"
"       Lists all known SIP users.\n"
"       Optional regular expression pattern is used to filter the user list.\n";

static char show_user_usage[] =
"Usage: sip show user <name> [load]\n"
"       Lists all details on one SIP user and the current status.\n"
"       Option \"load\" forces lookup of peer in realtime storage.\n";

static char show_inuse_usage[] = 
"Usage: sip show inuse [all]\n"
"       List all SIP users and peers usage counters and limits.\n"
"       Add option \"all\" to show all devices, not only those with a limit.\n";

static char show_channels_usage[] = 
"Usage: sip show channels\n"
"       Lists all currently active SIP channels.\n";

static char show_channel_usage[] = 
"Usage: sip show channel <channel>\n"
"       Provides detailed status on a given SIP channel.\n";

static char show_history_usage[] = 
"Usage: sip show history <channel>\n"
"       Provides detailed dialog history on a given SIP channel.\n";

static char show_peers_usage[] = 
"Usage: sip show peers [like <pattern>]\n"
"       Lists all known SIP peers.\n"
"       Optional regular expression pattern is used to filter the peer list.\n";

static char show_peer_usage[] =
"Usage: sip show peer <name> [load]\n"
"       Lists all details on one SIP peer and the current status.\n"
"       Option \"load\" forces lookup of peer in realtime storage.\n";

static char prune_realtime_usage[] =
"Usage: sip prune realtime [peer|user] [<name>|all|like <pattern>]\n"
"       Prunes object(s) from the cache.\n"
"       Optional regular expression pattern is used to filter the objects.\n";

static char show_reg_usage[] =
"Usage: sip show registry\n"
"       Lists all registration requests and status.\n";

static char debug_usage[] = 
"Usage: sip debug\n"
"       Enables dumping of SIP packets for debugging purposes\n\n"
"       sip debug ip <host[:PORT]>\n"
"       Enables dumping of SIP packets to and from host.\n\n"
"       sip debug peer <peername>\n"
"       Enables dumping of SIP packets to and from host.\n"
"       Require peer to be registered.\n";

static char no_debug_usage[] = 
"Usage: sip no debug\n"
"       Disables dumping of SIP packets for debugging purposes\n";

static char no_history_usage[] = 
"Usage: sip no history\n"
"       Disables recording of SIP dialog history for debugging purposes\n";

static char history_usage[] = 
"Usage: sip history\n"
"       Enables recording of SIP dialog history for debugging purposes.\n"
"Use 'sip show history' to view the history of a call number.\n";

static char sip_reload_usage[] =
"Usage: sip reload\n"
"       Reloads SIP configuration from sip.conf\n";

static char show_subscriptions_usage[] =
"Usage: sip show subscriptions\n" 
"       Shows active SIP subscriptions for extension states\n";

static char show_objects_usage[] =
"Usage: sip show objects\n" 
"       Shows status of known SIP objects\n";

static char show_settings_usage[] = 
"Usage: sip show settings\n"
"       Provides detailed list of the configuration of the SIP channel.\n";



/*! \brief  func_header_read: Read SIP header (dialplan function) */
static char *func_header_read(struct ast_channel *chan, char *cmd, char *data, char *buf, size_t len) 
{
	struct sip_pvt *p;
	char *content;
	
 	if (!data) {
		ast_log(LOG_WARNING, "This function requires a header name.\n");
		return NULL;
	}

	ast_mutex_lock(&chan->lock);
	if (chan->type != channeltype) {
		ast_log(LOG_WARNING, "This function can only be used on SIP channels.\n");
		ast_mutex_unlock(&chan->lock);
		return NULL;
	}

	p = chan->tech_pvt;

	/* If there is no private structure, this channel is no longer alive */
	if (!p) {
		ast_mutex_unlock(&chan->lock);
		return NULL;
	}

	content = get_header(&p->initreq, data);

	if (ast_strlen_zero(content)) {
		ast_mutex_unlock(&chan->lock);
		return NULL;
	}

	ast_copy_string(buf, content, len);
	ast_mutex_unlock(&chan->lock);

	return buf;
}


static struct ast_custom_function sip_header_function = {
	.name = "SIP_HEADER",
	.synopsis = "Gets or sets the specified SIP header",
	.syntax = "SIP_HEADER(<name>)",
	.read = func_header_read,
};

/*! \brief  function_check_sipdomain: Dial plan function to check if domain is local */
static char *func_check_sipdomain(struct ast_channel *chan, char *cmd, char *data, char *buf, size_t len)
{
	if (ast_strlen_zero(data)) {
		ast_log(LOG_WARNING, "CHECKSIPDOMAIN requires an argument - A domain name\n");
		return buf;
	}
	if (check_sip_domain(data, NULL, 0))
		ast_copy_string(buf, data, len);
	else
		buf[0] = '\0';
	return buf;
}

static struct ast_custom_function checksipdomain_function = {
	.name = "CHECKSIPDOMAIN",
	.synopsis = "Checks if domain is a local domain",
	.syntax = "CHECKSIPDOMAIN(<domain|IP>)",
	.read = func_check_sipdomain,
	.desc = "This function checks if the domain in the argument is configured\n"
		"as a local SIP domain that this Asterisk server is configured to handle.\n"
		"Returns the domain name if it is locally handled, otherwise an empty string.\n"
		"Check the domain= configuration in sip.conf\n",
};


/*! \brief  function_sippeer: ${SIPPEER()} Dialplan function - reads peer data */
static char *function_sippeer(struct ast_channel *chan, char *cmd, char *data, char *buf, size_t len)
{
	char *ret = NULL;
	struct sip_peer *peer;
	char *peername, *colname;
	char iabuf[INET_ADDRSTRLEN];

	if (!(peername = ast_strdupa(data)))
		return ret;

	if ((colname = strchr(peername, ':'))) {
		*colname = '\0';
		colname++;
	} else {
		colname = "ip";
	}
	if (!(peer = find_peer(peername, NULL, 1)))
		return ret;

	if (!strcasecmp(colname, "ip")) {
		ast_copy_string(buf, peer->addr.sin_addr.s_addr ? ast_inet_ntoa(iabuf, sizeof(iabuf), peer->addr.sin_addr) : "", len);
	} else  if (!strcasecmp(colname, "status")) {
		peer_status(peer, buf, sizeof(buf));
	} else  if (!strcasecmp(colname, "language")) {
		ast_copy_string(buf, peer->language, len);
	} else  if (!strcasecmp(colname, "regexten")) {
		ast_copy_string(buf, peer->regexten, len);
	} else  if (!strcasecmp(colname, "limit")) {
		snprintf(buf, len, "%d", peer->call_limit);
	} else  if (!strcasecmp(colname, "curcalls")) {
		snprintf(buf, len, "%d", peer->inUse);
	} else  if (!strcasecmp(colname, "useragent")) {
		ast_copy_string(buf, peer->useragent, len);
	} else  if (!strcasecmp(colname, "mailbox")) {
		ast_copy_string(buf, peer->mailbox, len);
	} else  if (!strcasecmp(colname, "context")) {
		ast_copy_string(buf, peer->context, len);
	} else  if (!strcasecmp(colname, "expire")) {
		snprintf(buf, len, "%d", peer->expire);
	} else  if (!strcasecmp(colname, "dynamic")) {
		ast_copy_string(buf, (ast_test_flag(peer, SIP_DYNAMIC) ? "yes" : "no"), len);
	} else  if (!strcasecmp(colname, "callerid_name")) {
		ast_copy_string(buf, peer->cid_name, len);
	} else  if (!strcasecmp(colname, "callerid_num")) {
		ast_copy_string(buf, peer->cid_num, len);
	} else  if (!strcasecmp(colname, "codecs")) {
		ast_getformatname_multiple(buf, len -1, peer->capability);
	} else  if (!strncasecmp(colname, "codec[", 6)) {
		char *codecnum, *ptr;
		int index = 0, codec = 0;
		
		codecnum = strchr(colname, '[');
		*codecnum = '\0';
		codecnum++;
		if ((ptr = strchr(codecnum, ']'))) {
			*ptr = '\0';
		}
		index = atoi(codecnum);
		if((codec = ast_codec_pref_index(&peer->prefs, index))) {
			ast_copy_string(buf, ast_getformatname(codec), len);
		}
	}
	ret = buf;

	ASTOBJ_UNREF(peer, sip_destroy_peer);

	return ret;
}

/*! \brief Structure to declare a dialplan function: SIPPEER */
struct ast_custom_function sippeer_function = {
	.name = "SIPPEER",
	.synopsis = "Gets SIP peer information",
	.syntax = "SIPPEER(<peername>[:item])",
	.read = function_sippeer,
	.desc = "Valid items are:\n"
	"- ip (default)          The IP address.\n"
	"- mailbox               The configured mailbox.\n"
	"- context               The configured context.\n"
	"- expire                The epoch time of the next expire.\n"
	"- dynamic               Is it dynamic? (yes/no).\n"
	"- callerid_name         The configured Caller ID name.\n"
	"- callerid_num          The configured Caller ID number.\n"
	"- codecs                The configured codecs.\n"
	"- status                Status (if qualify=yes).\n"
	"- regexten              Registration extension\n"
	"- limit                 Call limit (call-limit)\n"
	"- curcalls              Current amount of calls \n"
	"                        Only available if call-limit is set\n"
	"- language              Default language for peer\n"
	"- useragent             Current user agent id for peer\n"
	"- codec[x]              Preferred codec index number 'x' (beginning with zero).\n"
	"\n"
};

/*! \brief  function_sipchaninfo_read: ${SIPCHANINFO()} Dialplan function - reads sip channel data */
static char *function_sipchaninfo_read(struct ast_channel *chan, char *cmd, char *data, char *buf, size_t len) 
{
	struct sip_pvt *p;
	char iabuf[INET_ADDRSTRLEN];

	*buf = 0;
	
 	if (!data) {
		ast_log(LOG_WARNING, "This function requires a parameter name.\n");
		return NULL;
	}

	ast_mutex_lock(&chan->lock);
	if (chan->type != channeltype) {
		ast_log(LOG_WARNING, "This function can only be used on SIP channels.\n");
		ast_mutex_unlock(&chan->lock);
		return NULL;
	}

/* 	ast_verbose("function_sipchaninfo_read: %s\n", data); */
	p = chan->tech_pvt;

	/* If there is no private structure, this channel is no longer alive */
	if (!p) {
		ast_mutex_unlock(&chan->lock);
		return NULL;
	}

	if (!strcasecmp(data, "peerip")) {
		ast_copy_string(buf, p->sa.sin_addr.s_addr ? ast_inet_ntoa(iabuf, sizeof(iabuf), p->sa.sin_addr) : "", len);
	} else  if (!strcasecmp(data, "recvip")) {
		ast_copy_string(buf, p->recv.sin_addr.s_addr ? ast_inet_ntoa(iabuf, sizeof(iabuf), p->recv.sin_addr) : "", len);
	} else  if (!strcasecmp(data, "from")) {
		ast_copy_string(buf, p->from, len);
	} else  if (!strcasecmp(data, "uri")) {
		ast_copy_string(buf, p->uri, len);
	} else  if (!strcasecmp(data, "useragent")) {
		ast_copy_string(buf, p->useragent, len);
	} else  if (!strcasecmp(data, "peername")) {
		ast_copy_string(buf, p->peername, len);
	} else {
		ast_mutex_unlock(&chan->lock);
		return NULL;
	}
	ast_mutex_unlock(&chan->lock);

	return buf;
}

/*! \brief Structure to declare a dialplan function: SIPCHANINFO */
static struct ast_custom_function sipchaninfo_function = {
	.name = "SIPCHANINFO",
	.synopsis = "Gets the specified SIP parameter from the current channel",
	.syntax = "SIPCHANINFO(item)",
	.read = function_sipchaninfo_read,
	.desc = "Valid items are:\n"
	"- peerip                The IP address of the peer.\n"
	"- recvip                The source IP address of the peer.\n"
	"- from                  The URI from the From: header.\n"
	"- uri                   The URI from the Contact: header.\n"
	"- useragent             The useragent.\n"
	"- peername              The name of the peer.\n"
};



/*! \brief  parse_moved_contact: Parse 302 Moved temporalily response */
static void parse_moved_contact(struct sip_pvt *p, struct sip_request *req)
{
	char tmp[256];
	char *s, *e;
	ast_copy_string(tmp, get_header(req, "Contact"), sizeof(tmp));
	s = get_in_brackets(tmp);
	e = strchr(s, ';');
	if (e)
		*e = '\0';
	if (ast_test_flag(p, SIP_PROMISCREDIR)) {
		if (!strncasecmp(s, "sip:", 4))
			s += 4;
		e = strchr(s, '/');
		if (e)
			*e = '\0';
		ast_log(LOG_DEBUG, "Found promiscuous redirection to 'SIP/%s'\n", s);
		if (p->owner)
			snprintf(p->owner->call_forward, sizeof(p->owner->call_forward), "SIP/%s", s);
	} else {
		e = strchr(tmp, '@');
		if (e)
			*e = '\0';
		e = strchr(tmp, '/');
		if (e)
			*e = '\0';
		if (!strncasecmp(s, "sip:", 4))
			s += 4;
		ast_log(LOG_DEBUG, "Found 302 Redirect to extension '%s'\n", s);
		if (p->owner)
			ast_copy_string(p->owner->call_forward, s, sizeof(p->owner->call_forward));
	}
}

/*! \brief  check_pendings: Check pending actions on SIP call */
static void check_pendings(struct sip_pvt *p)
{
	/* Go ahead and send bye at this point */
	if (ast_test_flag(p, SIP_PENDINGBYE)) {
		transmit_request_with_auth(p, SIP_BYE, 0, 1, 1);
		ast_set_flag(p, SIP_NEEDDESTROY);	
		ast_clear_flag(p, SIP_NEEDREINVITE);	
	} else if (ast_test_flag(p, SIP_NEEDREINVITE)) {
		ast_log(LOG_DEBUG, "Sending pending reinvite on '%s'\n", p->callid);
		/* Didn't get to reinvite yet, so do it now */
		transmit_reinvite_with_sdp(p);
		ast_clear_flag(p, SIP_NEEDREINVITE);	
	}
}

/*! \brief  handle_response_invite: Handle SIP response in dialogue */
static void handle_response_invite(struct sip_pvt *p, int resp, char *rest, struct sip_request *req, int ignore, int seqno)
{
	int outgoing = ast_test_flag(p, SIP_OUTGOING);
	
	if (option_debug > 3) {
		int reinvite = (p->owner && p->owner->_state == AST_STATE_UP);
		if (reinvite)
			ast_log(LOG_DEBUG, "SIP response %d to RE-invite on %s call %s\n", resp, outgoing ? "outgoing" : "incoming", p->callid);
		else
			ast_log(LOG_DEBUG, "SIP response %d to standard invite\n", resp);
	}

	if (ast_test_flag(p, SIP_ALREADYGONE)) { /* This call is already gone */
		ast_log(LOG_DEBUG, "Got response on call that is already terminated: %s (ignoring)\n", p->callid);
		return;
	}

	switch (resp) {
	case 100:	/* Trying */
		sip_cancel_destroy(p);
		break;
	case 180:	/* 180 Ringing */
		sip_cancel_destroy(p);
		if (!ignore && p->owner) {
			ast_queue_control(p->owner, AST_CONTROL_RINGING);
			if (p->owner->_state != AST_STATE_UP)
				ast_setstate(p->owner, AST_STATE_RINGING);
		}
		if (!strcasecmp(get_header(req, "Content-Type"), "application/sdp")) {
			process_sdp(p, req);
			if (!ignore && p->owner) {
				/* Queue a progress frame only if we have SDP in 180 */
				ast_queue_control(p->owner, AST_CONTROL_PROGRESS);
			}
		}
		break;
	case 183:	/* Session progress */
		sip_cancel_destroy(p);
		if (!strcasecmp(get_header(req, "Content-Type"), "application/sdp")) {
			process_sdp(p, req);
		}
		if (!ignore && p->owner) {
			/* Queue a progress frame */
			ast_queue_control(p->owner, AST_CONTROL_PROGRESS);
		}
		break;
	case 200:	/* 200 OK on invite - someone's answering our call */
		sip_cancel_destroy(p);
		p->authtries = 0;
		if (!strcasecmp(get_header(req, "Content-Type"), "application/sdp")) {
			process_sdp(p, req);
		}

		/* Parse contact header for continued conversation */
		/* When we get 200 OK, we know which device (and IP) to contact for this call */
		/* This is important when we have a SIP proxy between us and the phone */
		if (outgoing) {
			parse_ok_contact(p, req);

			/* Save Record-Route for any later requests we make on this dialogue */
			build_route(p, req, 1);
		}
		
		if (!ignore && p->owner) {
			if (p->owner->_state != AST_STATE_UP) {
#ifdef OSP_SUPPORT	
				time(&p->ospstart);
#endif
				ast_queue_control(p->owner, AST_CONTROL_ANSWER);
			} else {	/* RE-invite */
				struct ast_frame af = { AST_FRAME_NULL, };
				ast_queue_frame(p->owner, &af);
			}
		} else {
			 /* It's possible we're getting an ACK after we've tried to disconnect
				  by sending CANCEL */
			/* THIS NEEDS TO BE CHECKED: OEJ */
			if (!ignore)
				ast_set_flag(p, SIP_PENDINGBYE);	
		}
		/* If I understand this right, the branch is different for a non-200 ACK only */
		transmit_request(p, SIP_ACK, seqno, 0, 1);
		check_pendings(p);
		break;
	case 407: /* Proxy authentication */
	case 401: /* Www auth */
		/* First we ACK */
		transmit_request(p, SIP_ACK, seqno, 0, 0);
		if (p->options)
			p->options->auth_type = (resp == 401 ? WWW_AUTH : PROXY_AUTH);

		/* Then we AUTH */
		ast_string_field_free(p, theirtag);	/* forget their old tag, so we don't match tags when getting response */
		if (!ignore) {
			char *authenticate = (resp == 401 ? "WWW-Authenticate" : "Proxy-Authenticate");
			char *authorization = (resp == 401 ? "Authorization" : "Proxy-Authorization");
			if ((p->authtries == MAX_AUTHTRIES) || do_proxy_auth(p, req, authenticate, authorization, SIP_INVITE, 1)) {
				ast_log(LOG_NOTICE, "Failed to authenticate on INVITE to '%s'\n", get_header(&p->initreq, "From"));
				ast_set_flag(p, SIP_NEEDDESTROY);	
				ast_set_flag(p, SIP_ALREADYGONE);	
				if (p->owner)
					ast_queue_control(p->owner, AST_CONTROL_CONGESTION);
			}
		}
		break;
	case 403: /* Forbidden */
		/* First we ACK */
		transmit_request(p, SIP_ACK, seqno, 0, 0);
		ast_log(LOG_WARNING, "Forbidden - wrong password on authentication for INVITE to '%s'\n", get_header(&p->initreq, "From"));
		if (!ignore && p->owner)
			ast_queue_control(p->owner, AST_CONTROL_CONGESTION);
		ast_set_flag(p, SIP_NEEDDESTROY);	
		ast_set_flag(p, SIP_ALREADYGONE);	
		break;
	case 404: /* Not found */
		transmit_request(p, SIP_ACK, seqno, 0, 0);
		if (p->owner && !ignore)
			ast_queue_control(p->owner, AST_CONTROL_CONGESTION);
		ast_set_flag(p, SIP_ALREADYGONE);	
		break;
	case 481: /* Call leg does not exist */
		/* Could be REFER or INVITE */
		ast_log(LOG_WARNING, "Re-invite to non-existing call leg on other UA. SIP dialog '%s'. Giving up.\n", p->callid);
		transmit_request(p, SIP_ACK, seqno, 0, 0);
		break;
	case 491: /* Pending */
		/* we have to wait a while, then retransmit */
		/* Transmission is rescheduled, so everything should be taken care of.
			We should support the retry-after at some point */
		break;
	case 501: /* Not implemented */
		if (p->owner)
			ast_queue_control(p->owner, AST_CONTROL_CONGESTION);
		break;
	}
}

/*! \brief  handle_response_register: Handle responses on REGISTER to services */
static int handle_response_register(struct sip_pvt *p, int resp, char *rest, struct sip_request *req, int ignore, int seqno)
{
	int expires, expires_ms;
	struct sip_registry *r;
	r=p->registry;

	switch (resp) {
	case 401:	/* Unauthorized */
		if ((p->authtries == MAX_AUTHTRIES) || do_register_auth(p, req, "WWW-Authenticate", "Authorization")) {
			ast_log(LOG_NOTICE, "Failed to authenticate on REGISTER to '%s@%s' (Tries %d)\n", p->registry->username, p->registry->hostname, p->authtries);
			ast_set_flag(p, SIP_NEEDDESTROY);	
			}
		break;
	case 403:	/* Forbidden */
		ast_log(LOG_WARNING, "Forbidden - wrong password on authentication for REGISTER for '%s' to '%s'\n", p->registry->username, p->registry->hostname);
		if (global_regattempts_max)
			p->registry->regattempts = global_regattempts_max+1;
		ast_sched_del(sched, r->timeout);
		ast_set_flag(p, SIP_NEEDDESTROY);	
		break;
	case 404:	/* Not found */
		ast_log(LOG_WARNING, "Got 404 Not found on SIP register to service %s@%s, giving up\n", p->registry->username,p->registry->hostname);
		if (global_regattempts_max)
			p->registry->regattempts = global_regattempts_max+1;
		ast_set_flag(p, SIP_NEEDDESTROY);	
		r->call = NULL;
		ast_sched_del(sched, r->timeout);
		break;
	case 407:	/* Proxy auth */
		if ((p->authtries == MAX_AUTHTRIES) || do_register_auth(p, req, "Proxy-Authenticate", "Proxy-Authorization")) {
			ast_log(LOG_NOTICE, "Failed to authenticate on REGISTER to '%s' (tries '%d')\n", get_header(&p->initreq, "From"), p->authtries);
			ast_set_flag(p, SIP_NEEDDESTROY);	
		}
		break;
	case 479:	/* SER: Not able to process the URI - address is wrong in register*/
		ast_log(LOG_WARNING, "Got error 479 on register to %s@%s, giving up (check config)\n", p->registry->username,p->registry->hostname);
		if (global_regattempts_max)
			p->registry->regattempts = global_regattempts_max+1;
		ast_set_flag(p, SIP_NEEDDESTROY);	
		r->call = NULL;
		ast_sched_del(sched, r->timeout);
		break;
	case 200:	/* 200 OK */
		if (!r) {
			ast_log(LOG_WARNING, "Got 200 OK on REGISTER that isn't a register\n");
			ast_set_flag(p, SIP_NEEDDESTROY);	
			return 0;
		}

		r->regstate=REG_STATE_REGISTERED;
		manager_event(EVENT_FLAG_SYSTEM, "Registry", "Channel: SIP\r\nDomain: %s\r\nStatus: %s\r\n", r->hostname, regstate2str(r->regstate));
		r->regattempts = 0;
		ast_log(LOG_DEBUG, "Registration successful\n");
		if (r->timeout > -1) {
			ast_log(LOG_DEBUG, "Cancelling timeout %d\n", r->timeout);
			ast_sched_del(sched, r->timeout);
		}
		r->timeout=-1;
		r->call = NULL;
		p->registry = NULL;
		/* Let this one hang around until we have all the responses */
		sip_scheddestroy(p, 32000);
		/* ast_set_flag(p, SIP_NEEDDESTROY);	*/

		/* set us up for re-registering */
		/* figure out how long we got registered for */
		if (r->expire > -1)
			ast_sched_del(sched, r->expire);
		/* according to section 6.13 of RFC, contact headers override
		   expires headers, so check those first */
		expires = 0;
		if (!ast_strlen_zero(get_header(req, "Contact"))) {
			char *contact = NULL;
			char *tmptmp = NULL;
			int start = 0;
			for(;;) {
				contact = __get_header(req, "Contact", &start);
				/* this loop ensures we get a contact header about our register request */
				if(!ast_strlen_zero(contact)) {
					if( (tmptmp=strstr(contact, p->our_contact))) {
						contact=tmptmp;
						break;
					}
				} else
					break;
			}
			tmptmp = strcasestr(contact, "expires=");
			if (tmptmp) {
				if (sscanf(tmptmp + 8, "%d;", &expires) != 1)
					expires = 0;
			}

		}
		if (!expires) 
			expires=atoi(get_header(req, "expires"));
		if (!expires)
			expires=default_expiry;

		expires_ms = expires * 1000;
		if (expires <= EXPIRY_GUARD_LIMIT)
			expires_ms -= MAX((expires_ms * EXPIRY_GUARD_PCT),EXPIRY_GUARD_MIN);
		else
			expires_ms -= EXPIRY_GUARD_SECS * 1000;
		if (sipdebug)
			ast_log(LOG_NOTICE, "Outbound Registration: Expiry for %s is %d sec (Scheduling reregistration in %d s)\n", r->hostname, expires, expires_ms/1000); 

		r->refresh= (int) expires_ms / 1000;

		/* Schedule re-registration before we expire */
		r->expire=ast_sched_add(sched, expires_ms, sip_reregister, r); 
		ASTOBJ_UNREF(r, sip_registry_destroy);
	}
	return 1;
}

/*! \brief  handle_response_peerpoke: Handle qualification responses (OPTIONS) */
static int handle_response_peerpoke(struct sip_pvt *p, int resp, char *rest, struct sip_request *req, int ignore, int seqno, int sipmethod)
{
	struct sip_peer *peer;
	int pingtime;
	struct timeval tv;

	if (resp != 100) {
		int statechanged = 0;
		int newstate = 0;
		peer = p->peerpoke;
		gettimeofday(&tv, NULL);
		pingtime = ast_tvdiff_ms(tv, peer->ps);
		if (pingtime < 1)
			pingtime = 1;
		if ((peer->lastms < 0)  || (peer->lastms > peer->maxms)) {
			if (pingtime <= peer->maxms) {
				ast_log(LOG_NOTICE, "Peer '%s' is now REACHABLE! (%dms / %dms)\n", peer->name, pingtime, peer->maxms);
				statechanged = 1;
				newstate = 1;
			}
		} else if ((peer->lastms > 0) && (peer->lastms <= peer->maxms)) {
			if (pingtime > peer->maxms) {
				ast_log(LOG_NOTICE, "Peer '%s' is now TOO LAGGED! (%dms / %dms)\n", peer->name, pingtime, peer->maxms);
				statechanged = 1;
				newstate = 2;
			}
		}
		if (!peer->lastms)
			statechanged = 1;
		peer->lastms = pingtime;
		peer->call = NULL;
		if (statechanged) {
			ast_device_state_changed("SIP/%s", peer->name);
			if (newstate == 2) {
				manager_event(EVENT_FLAG_SYSTEM, "PeerStatus", "Peer: SIP/%s\r\nPeerStatus: Lagged\r\nTime: %d\r\n", peer->name, pingtime);
			} else {
				manager_event(EVENT_FLAG_SYSTEM, "PeerStatus", "Peer: SIP/%s\r\nPeerStatus: Reachable\r\nTime: %d\r\n", peer->name, pingtime);
			}
		}

		if (peer->pokeexpire > -1)
			ast_sched_del(sched, peer->pokeexpire);
		if (sipmethod == SIP_INVITE)	/* Does this really happen? */
			transmit_request(p, SIP_ACK, seqno, 0, 0);
		ast_set_flag(p, SIP_NEEDDESTROY);	

		/* Try again eventually */
		if ((peer->lastms < 0)  || (peer->lastms > peer->maxms))
			peer->pokeexpire = ast_sched_add(sched, DEFAULT_FREQ_NOTOK, sip_poke_peer_s, peer);
		else
			peer->pokeexpire = ast_sched_add(sched, DEFAULT_FREQ_OK, sip_poke_peer_s, peer);
	}
	return 1;
}

/*! \brief  handle_response: Handle SIP response in dialogue */
static void handle_response(struct sip_pvt *p, int resp, char *rest, struct sip_request *req, int ignore, int seqno)
{
	char *msg, *c;
	struct ast_channel *owner;
	char iabuf[INET_ADDRSTRLEN];
	int sipmethod;
	int res = 1;

	c = get_header(req, "Cseq");
	msg = strchr(c, ' ');
	if (!msg)
		msg = "";
	else
		msg++;
	sipmethod = find_sip_method(msg);

	owner = p->owner;
	if (owner) 
		owner->hangupcause = hangup_sip2cause(resp);

	/* Acknowledge whatever it is destined for */
	if ((resp >= 100) && (resp <= 199))
		__sip_semi_ack(p, seqno, 0, sipmethod);
	else
		__sip_ack(p, seqno, 0, sipmethod);

	/* Get their tag if we haven't already */
	if (ast_strlen_zero(p->theirtag) || (resp >= 200)) {
		char tag[128];

		gettag(req, "To", tag, sizeof(tag));
		ast_string_field_set(p, theirtag, tag);
	}
	if (p->peerpoke) {
		/* We don't really care what the response is, just that it replied back. 
		   Well, as long as it's not a 100 response...  since we might
		   need to hang around for something more "definitive" */

		res = handle_response_peerpoke(p, resp, rest, req, ignore, seqno, sipmethod);
	} else if (ast_test_flag(p, SIP_OUTGOING)) {
		/* Acknowledge sequence number */
		if (p->initid > -1) {
			/* Don't auto congest anymore since we've gotten something useful back */
			ast_sched_del(sched, p->initid);
			p->initid = -1;
		}
		switch(resp) {
		case 100:	/* 100 Trying */
			if (sipmethod == SIP_INVITE) 
				handle_response_invite(p, resp, rest, req, ignore, seqno);
			break;
		case 183:	/* 183 Session Progress */
			if (sipmethod == SIP_INVITE) 
				handle_response_invite(p, resp, rest, req, ignore, seqno);
			break;
		case 180:	/* 180 Ringing */
			if (sipmethod == SIP_INVITE) 
				handle_response_invite(p, resp, rest, req, ignore, seqno);
			break;
		case 200:	/* 200 OK */
			p->authtries = 0;	/* Reset authentication counter */
			if (sipmethod == SIP_MESSAGE) {
				/* We successfully transmitted a message */
				ast_set_flag(p, SIP_NEEDDESTROY);	
			} else if (sipmethod == SIP_NOTIFY) {
				/* They got the notify, this is the end */
				if (p->owner) {
					ast_log(LOG_WARNING, "Notify answer on an owned channel?\n");
					ast_queue_hangup(p->owner);
				} else {
					if (p->subscribed == NONE) {
						ast_set_flag(p, SIP_NEEDDESTROY); 
					}
				}
			} else if (sipmethod == SIP_INVITE) {
				handle_response_invite(p, resp, rest, req, ignore, seqno);
			} else if (sipmethod == SIP_REGISTER) {
				res = handle_response_register(p, resp, rest, req, ignore, seqno);
			} 
			break;
		case 401: /* Not www-authorized on SIP method */
			if (sipmethod == SIP_INVITE) {
				handle_response_invite(p, resp, rest, req, ignore, seqno);
			} else if (p->registry && sipmethod == SIP_REGISTER) {
				res = handle_response_register(p, resp, rest, req, ignore, seqno);
			} else {
				ast_log(LOG_WARNING, "Got authentication request (401) on unknown %s to '%s'\n", sip_methods[sipmethod].text, get_header(req, "To"));
				ast_set_flag(p, SIP_NEEDDESTROY);	
			}
			break;
		case 403: /* Forbidden - we failed authentication */
			if (sipmethod == SIP_INVITE) {
				handle_response_invite(p, resp, rest, req, ignore, seqno);
			} else if (p->registry && sipmethod == SIP_REGISTER) {
				res = handle_response_register(p, resp, rest, req, ignore, seqno);
			} else {
				ast_log(LOG_WARNING, "Forbidden - wrong password on authentication for %s\n", msg);
			}
			break;
		case 404: /* Not found */
			if (p->registry && sipmethod == SIP_REGISTER) {
				res = handle_response_register(p, resp, rest, req, ignore, seqno);
			} else if (sipmethod == SIP_INVITE) {
				handle_response_invite(p, resp, rest, req, ignore, seqno);
			} else if (owner)
				ast_queue_control(p->owner, AST_CONTROL_CONGESTION);
			break;
		case 407: /* Proxy auth required */
			if (sipmethod == SIP_INVITE) {
				handle_response_invite(p, resp, rest, req, ignore, seqno);
			} else if (sipmethod == SIP_BYE || sipmethod == SIP_REFER) {
				if (ast_strlen_zero(p->authname))
					ast_log(LOG_WARNING, "Asked to authenticate %s, to %s:%d but we have no matching peer!\n",
							msg, ast_inet_ntoa(iabuf, sizeof(iabuf), p->recv.sin_addr), ntohs(p->recv.sin_port));
					ast_set_flag(p, SIP_NEEDDESTROY);	
				if ((p->authtries == MAX_AUTHTRIES) || do_proxy_auth(p, req, "Proxy-Authenticate", "Proxy-Authorization", sipmethod, 0)) {
					ast_log(LOG_NOTICE, "Failed to authenticate on %s to '%s'\n", msg, get_header(&p->initreq, "From"));
					ast_set_flag(p, SIP_NEEDDESTROY);	
				}
			} else if (p->registry && sipmethod == SIP_REGISTER) {
				res = handle_response_register(p, resp, rest, req, ignore, seqno);
			} else	/* We can't handle this, giving up in a bad way */
				ast_set_flag(p, SIP_NEEDDESTROY);	

			break;
		case 491: /* Pending */
			if (sipmethod == SIP_INVITE) {
				handle_response_invite(p, resp, rest, req, ignore, seqno);
			}
		case 501: /* Not Implemented */
			if (sipmethod == SIP_INVITE) {
				handle_response_invite(p, resp, rest, req, ignore, seqno);
			} else
				ast_log(LOG_WARNING, "Host '%s' does not implement '%s'\n", ast_inet_ntoa(iabuf, sizeof(iabuf), p->sa.sin_addr), msg);
			break;
		default:
			if ((resp >= 300) && (resp < 700)) {
				if ((option_verbose > 2) && (resp != 487))
					ast_verbose(VERBOSE_PREFIX_3 "Got SIP response %d \"%s\" back from %s\n", resp, rest, ast_inet_ntoa(iabuf, sizeof(iabuf), p->sa.sin_addr));
				ast_set_flag(p, SIP_ALREADYGONE);	
				if (p->rtp) {
					/* Immediately stop RTP */
					ast_rtp_stop(p->rtp);
				}
				if (p->vrtp) {
					/* Immediately stop VRTP */
					ast_rtp_stop(p->vrtp);
				}
				/* XXX Locking issues?? XXX */
				switch(resp) {
				case 300: /* Multiple Choices */
				case 301: /* Moved permenantly */
				case 302: /* Moved temporarily */
				case 305: /* Use Proxy */
					parse_moved_contact(p, req);
					/* Fall through */
				case 486: /* Busy here */
				case 600: /* Busy everywhere */
				case 603: /* Decline */
					if (p->owner)
						ast_queue_control(p->owner, AST_CONTROL_BUSY);
					break;
				case 487:
					/* channel now destroyed - dec the inUse counter */
					update_call_counter(p, DEC_CALL_LIMIT);
					break;
				case 482: /* SIP is incapable of performing a hairpin call, which
				             is yet another failure of not having a layer 2 (again, YAY
							 IETF for thinking ahead).  So we treat this as a call
							 forward and hope we end up at the right place... */
					ast_log(LOG_DEBUG, "Hairpin detected, setting up call forward for what it's worth\n");
					if (p->owner)
						snprintf(p->owner->call_forward, sizeof(p->owner->call_forward), "Local/%s@%s", p->username, p->context);
					/* Fall through */
				case 488: /* Not acceptable here - codec error */
				case 480: /* Temporarily Unavailable */
				case 404: /* Not Found */
				case 410: /* Gone */
				case 400: /* Bad Request */
				case 500: /* Server error */
				case 503: /* Service Unavailable */
					if (owner)
						ast_queue_control(p->owner, AST_CONTROL_CONGESTION);
					break;
				default:
					/* Send hangup */	
					if (owner)
						ast_queue_hangup(p->owner);
					break;
				}
				/* ACK on invite */
				if (sipmethod == SIP_INVITE) 
					transmit_request(p, SIP_ACK, seqno, 0, 0);
				ast_set_flag(p, SIP_ALREADYGONE);	
				if (!p->owner)
					ast_set_flag(p, SIP_NEEDDESTROY);	
			} else if ((resp >= 100) && (resp < 200)) {
				if (sipmethod == SIP_INVITE) {
					sip_cancel_destroy(p);
					if (!ast_strlen_zero(get_header(req, "Content-Type")))
						process_sdp(p, req);
					if (p->owner) {
						/* Queue a progress frame */
						ast_queue_control(p->owner, AST_CONTROL_PROGRESS);
					}
				}
			} else
				ast_log(LOG_NOTICE, "Dont know how to handle a %d %s response from %s\n", resp, rest, p->owner ? p->owner->name : ast_inet_ntoa(iabuf, sizeof(iabuf), p->sa.sin_addr));
		}
	} else {	
		/* Responses to OUTGOING SIP requests on INCOMING calls 
		   get handled here. As well as out-of-call message responses */
		if (req->debug)
			ast_verbose("SIP Response message for INCOMING dialog %s arrived\n", msg);
		if (resp == 200) {
			/* Tags in early session is replaced by the tag in 200 OK, which is 
		  	the final reply to our INVITE */
			char tag[128];

			gettag(req, "To", tag, sizeof(tag));
			ast_string_field_set(p, theirtag, tag);
		}

		switch(resp) {
		case 200:
			if (sipmethod == SIP_INVITE) {
				handle_response_invite(p, resp, rest, req, ignore, seqno);
			} else if (sipmethod == SIP_CANCEL) {
				ast_log(LOG_DEBUG, "Got 200 OK on CANCEL\n");
			} else if (sipmethod == SIP_MESSAGE)
				/* We successfully transmitted a message */
				ast_set_flag(p, SIP_NEEDDESTROY);	
			break;
		case 401:	/* www-auth */
		case 407:
			if (sipmethod == SIP_BYE || sipmethod == SIP_REFER) {
				char *auth, *auth2;

				if (resp == 407) {
					auth = "Proxy-Authenticate";
					auth2 = "Proxy-Authorization";
				} else {
					auth = "WWW-Authenticate";
					auth2 = "Authorization";
				}
				if ((p->authtries == MAX_AUTHTRIES) || do_proxy_auth(p, req, auth, auth2, sipmethod, 0)) {
					ast_log(LOG_NOTICE, "Failed to authenticate on %s to '%s'\n", msg, get_header(&p->initreq, "From"));
					ast_set_flag(p, SIP_NEEDDESTROY);	
				}
			} else if (sipmethod == SIP_INVITE) {
				handle_response_invite(p, resp, rest, req, ignore, seqno);
			}
			break;
		case 481:	/* Call leg does not exist */
			if (sipmethod == SIP_INVITE) {
				/* Re-invite failed */
				handle_response_invite(p, resp, rest, req, ignore, seqno);
			}
			break;
		default:	/* Errors without handlers */
			if ((resp >= 100) && (resp < 200)) {
				if (sipmethod == SIP_INVITE) {	/* re-invite */
					sip_cancel_destroy(p);
				}
			}
			if ((resp >= 300) && (resp < 700)) {
				if ((option_verbose > 2) && (resp != 487))
					ast_verbose(VERBOSE_PREFIX_3 "Incoming call: Got SIP response %d \"%s\" back from %s\n", resp, rest, ast_inet_ntoa(iabuf, sizeof(iabuf), p->sa.sin_addr));
				switch(resp) {
				case 488: /* Not acceptable here - codec error */
				case 603: /* Decline */
				case 500: /* Server error */
				case 503: /* Service Unavailable */

					if (sipmethod == SIP_INVITE) {	/* re-invite failed */
						sip_cancel_destroy(p);
					}
					break;
				}
			}
			break;
		}
	}
}

struct sip_dual {
	struct ast_channel *chan1;
	struct ast_channel *chan2;
	struct sip_request req;
};

/*! \brief  sip_park_thread: Park SIP call support function */
static void *sip_park_thread(void *stuff)
{
	struct ast_channel *chan1, *chan2;
	struct sip_dual *d;
	struct sip_request req;
	int ext;
	int res;
	d = stuff;
	chan1 = d->chan1;
	chan2 = d->chan2;
	copy_request(&req, &d->req);
	free(d);
	ast_mutex_lock(&chan1->lock);
	ast_do_masquerade(chan1);
	ast_mutex_unlock(&chan1->lock);
	res = ast_park_call(chan1, chan2, 0, &ext);
	/* Then hangup */
	ast_hangup(chan2);
	ast_log(LOG_DEBUG, "Parked on extension '%d'\n", ext);
	return NULL;
}

/*! \brief  sip_park: Park a call */
static int sip_park(struct ast_channel *chan1, struct ast_channel *chan2, struct sip_request *req)
{
	struct sip_dual *d;
	struct ast_channel *chan1m, *chan2m;
	pthread_t th;
	chan1m = ast_channel_alloc(0);
	chan2m = ast_channel_alloc(0);
	if ((!chan2m) || (!chan1m)) {
		if (chan1m)
			ast_hangup(chan1m);
		if (chan2m)
			ast_hangup(chan2m);
		return -1;
	}
	snprintf(chan1m->name, sizeof(chan1m->name), "Parking/%s", chan1->name);
	/* Make formats okay */
	chan1m->readformat = chan1->readformat;
	chan1m->writeformat = chan1->writeformat;
	ast_channel_masquerade(chan1m, chan1);
	/* Setup the extensions and such */
	ast_copy_string(chan1m->context, chan1->context, sizeof(chan1m->context));
	ast_copy_string(chan1m->exten, chan1->exten, sizeof(chan1m->exten));
	chan1m->priority = chan1->priority;
		
	/* We make a clone of the peer channel too, so we can play
	   back the announcement */
	snprintf(chan2m->name, sizeof (chan2m->name), "SIPPeer/%s",chan2->name);
	/* Make formats okay */
	chan2m->readformat = chan2->readformat;
	chan2m->writeformat = chan2->writeformat;
	ast_channel_masquerade(chan2m, chan2);
	/* Setup the extensions and such */
	ast_copy_string(chan2m->context, chan2->context, sizeof(chan2m->context));
	ast_copy_string(chan2m->exten, chan2->exten, sizeof(chan2m->exten));
	chan2m->priority = chan2->priority;
	ast_mutex_lock(&chan2m->lock);
	if (ast_do_masquerade(chan2m)) {
		ast_log(LOG_WARNING, "Masquerade failed :(\n");
		ast_mutex_unlock(&chan2m->lock);
		ast_hangup(chan2m);
		return -1;
	}
	ast_mutex_unlock(&chan2m->lock);
	if ((d = ast_calloc(1, sizeof(*d)))) {
		/* Save original request for followup */
		copy_request(&d->req, req);
		d->chan1 = chan1m;
		d->chan2 = chan2m;
		if (!ast_pthread_create(&th, NULL, sip_park_thread, d))
			return 0;
		free(d);
	}
	return -1;
}

/*! \brief  ast_quiet_chan: Turn off generator data */
static void ast_quiet_chan(struct ast_channel *chan) 
{
	if (chan && chan->_state == AST_STATE_UP) {
		if (chan->generatordata)
			ast_deactivate_generator(chan);
	}
}

/*! \brief  attempt_transfer: Attempt transfer of SIP call */
static int attempt_transfer(struct sip_pvt *p1, struct sip_pvt *p2)
{
	int res = 0;
	struct ast_channel 
		*chana = NULL,
		*chanb = NULL,
		*bridgea = NULL,
		*bridgeb = NULL,
		*peera = NULL,
		*peerb = NULL,
		*peerc = NULL,
		*peerd = NULL;

	if (!p1->owner || !p2->owner) {
		ast_log(LOG_WARNING, "Transfer attempted without dual ownership?\n");
		return -1;
	}
	chana = p1->owner;
	chanb = p2->owner;
	bridgea = ast_bridged_channel(chana);
	bridgeb = ast_bridged_channel(chanb);
	
	if (bridgea) {
		peera = chana;
		peerb = chanb;
		peerc = bridgea;
		peerd = bridgeb;
	} else if (bridgeb) {
		peera = chanb;
		peerb = chana;
		peerc = bridgeb;
		peerd = bridgea;
	}
	
	if (peera && peerb && peerc && (peerb != peerc)) {
		ast_quiet_chan(peera);
		ast_quiet_chan(peerb);
		ast_quiet_chan(peerc);
		ast_quiet_chan(peerd);

		if (peera->cdr && peerb->cdr) {
			peerb->cdr = ast_cdr_append(peerb->cdr, peera->cdr);
		} else if (peera->cdr) {
			peerb->cdr = peera->cdr;
		}
		peera->cdr = NULL;

		if (peerb->cdr && peerc->cdr) {
			peerb->cdr = ast_cdr_append(peerb->cdr, peerc->cdr);
		} else if (peerc->cdr) {
			peerb->cdr = peerc->cdr;
		}
		peerc->cdr = NULL;
		
		if (ast_channel_masquerade(peerb, peerc)) {
			ast_log(LOG_WARNING, "Failed to masquerade %s into %s\n", peerb->name, peerc->name);
			res = -1;
		}
		return res;
	} else {
		ast_log(LOG_NOTICE, "Transfer attempted with no appropriate bridged calls to transfer\n");
		if (chana)
			ast_softhangup_nolock(chana, AST_SOFTHANGUP_DEV);
		if (chanb)
			ast_softhangup_nolock(chanb, AST_SOFTHANGUP_DEV);
		return -1;
	}
	return 0;
}

/*! \brief  gettag: Get tag from packet */
static char *gettag(struct sip_request *req, char *header, char *tagbuf, int tagbufsize) 
{
	char *thetag, *sep;

	if (!tagbuf)
		return NULL;
	tagbuf[0] = '\0'; 	/* reset the buffer */
	thetag = get_header(req, header);
	thetag = strcasestr(thetag, ";tag=");
	if (thetag) {
		thetag += 5;
		ast_copy_string(tagbuf, thetag, tagbufsize);
		sep = strchr(tagbuf, ';');
		if (sep)
			*sep = '\0';
	}
	return thetag;
}

/*! \brief  handle_request_options: Handle incoming OPTIONS request */
static int handle_request_options(struct sip_pvt *p, struct sip_request *req, int debug)
{
	int res;

	res = get_destination(p, req);
	build_contact(p);
	/* XXX Should we authenticate OPTIONS? XXX */
	if (ast_strlen_zero(p->context))
		ast_string_field_set(p, context, default_context);
	if (res < 0)
		transmit_response_with_allow(p, "404 Not Found", req, 0);
	else if (res > 0)
		transmit_response_with_allow(p, "484 Address Incomplete", req, 0);
	else 
		transmit_response_with_allow(p, "200 OK", req, 0);
	/* Destroy if this OPTIONS was the opening request, but not if
	   it's in the middle of a normal call flow. */
	if (!p->lastinvite)
		ast_set_flag(p, SIP_NEEDDESTROY);	

	return res;
}

/*! \brief  handle_request_invite: Handle incoming INVITE request */
static int handle_request_invite(struct sip_pvt *p, struct sip_request *req, int debug, int ignore, int seqno, struct sockaddr_in *sin, int *recount, char *e)
{
	int res = 1;
	struct ast_channel *c=NULL;
	int gotdest;
	struct ast_frame af = { AST_FRAME_NULL, };
	char *supported;
	char *required;
	unsigned int required_profile = 0;

	/* Find out what they support */
	if (!p->sipoptions) {
		supported = get_header(req, "Supported");
		if (supported)
			parse_sip_options(p, supported);
	}
	required = get_header(req, "Required");
	if (!ast_strlen_zero(required)) {
		required_profile = parse_sip_options(NULL, required);
		if (required_profile) { 	/* They require something */
			/* At this point we support no extensions, so fail */
			transmit_response_with_unsupported(p, "420 Bad extension", req, required);
			if (!p->lastinvite)
				ast_set_flag(p, SIP_NEEDDESTROY);	
			return -1;
			
		}
	}

	/* Check if this is a loop */
	/* This happens since we do not properly support SIP domain
	   handling yet... -oej */
	if (ast_test_flag(p, SIP_OUTGOING) && p->owner && (p->owner->_state != AST_STATE_UP)) {
		/* This is a call to ourself.  Send ourselves an error code and stop
		   processing immediately, as SIP really has no good mechanism for
		   being able to call yourself */
		transmit_response(p, "482 Loop Detected", req);
		/* We do NOT destroy p here, so that our response will be accepted */
		return 0;
	}
	if (!ignore) {
		/* Use this as the basis */
		if (debug)
			ast_verbose("Using INVITE request as basis request - %s\n", p->callid);
		sip_cancel_destroy(p);
		/* This call is no longer outgoing if it ever was */
		ast_clear_flag(p, SIP_OUTGOING);
		/* This also counts as a pending invite */
		p->pendinginvite = seqno;
		copy_request(&p->initreq, req);
		check_via(p, req);
		if (p->owner) {
			/* Handle SDP here if we already have an owner */
			if (!strcasecmp(get_header(req, "Content-Type"), "application/sdp")) {
				if (process_sdp(p, req)) {
					transmit_response(p, "488 Not acceptable here", req);
					if (!p->lastinvite)
						ast_set_flag(p, SIP_NEEDDESTROY);	
					return -1;
				}
			} else {
				p->jointcapability = p->capability;
				ast_log(LOG_DEBUG, "Hm....  No sdp for the moment\n");
			}
		}
	} else if (debug)
		ast_verbose("Ignoring this INVITE request\n");
	if (!p->lastinvite && !ignore && !p->owner) {
		/* Handle authentication if this is our first invite */
		res = check_user(p, req, SIP_INVITE, e, 1, sin, ignore);
		if (res) {
			if (res < 0) {
				ast_log(LOG_NOTICE, "Failed to authenticate user %s\n", get_header(req, "From"));
				if (ignore)
					transmit_response(p, "403 Forbidden", req);
				else
					transmit_response_reliable(p, "403 Forbidden", req, 1);
				ast_set_flag(p, SIP_NEEDDESTROY);
				ast_string_field_free(p, theirtag);
			}
			return 0;
		}
		/* Process the SDP portion */
		if (!ast_strlen_zero(get_header(req, "Content-Type"))) {
			if (process_sdp(p, req)) {
				transmit_response(p, "488 Not acceptable here", req);
				ast_set_flag(p, SIP_NEEDDESTROY);	
				return -1;
			}
		} else {
			p->jointcapability = p->capability;
			ast_log(LOG_DEBUG, "Hm....  No sdp for the moment\n");
		}
		/* Queue NULL frame to prod ast_rtp_bridge if appropriate */
		if (p->owner)
			ast_queue_frame(p->owner, &af);
		/* Initialize the context if it hasn't been already */
		if (ast_strlen_zero(p->context))
			ast_string_field_set(p, context, default_context);
		/* Check number of concurrent calls -vs- incoming limit HERE */
		ast_log(LOG_DEBUG, "Checking SIP call limits for device %s\n", p->username);
		res = update_call_counter(p, INC_CALL_LIMIT);
		if (res) {
			if (res < 0) {
				ast_log(LOG_NOTICE, "Failed to place call for user %s, too many calls\n", p->username);
				if (ignore)
					transmit_response(p, "480 Temporarily Unavailable (Call limit)", req);
				else
					transmit_response_reliable(p, "480 Temporarily Unavailable (Call limit) ", req, 1);
				ast_set_flag(p, SIP_NEEDDESTROY);	
			}
			return 0;
		}
		/* Get destination right away */
		gotdest = get_destination(p, NULL);

		get_rdnis(p, NULL);
		extract_uri(p, req);
		build_contact(p);

		if (gotdest) {
			if (gotdest < 0) {
				if (ignore)
					transmit_response(p, "404 Not Found", req);
				else
					transmit_response_reliable(p, "404 Not Found", req, 1);
				update_call_counter(p, DEC_CALL_LIMIT);
			} else {
				if (ignore)
					transmit_response(p, "484 Address Incomplete", req);
				else
					transmit_response_reliable(p, "484 Address Incomplete", req, 1);
				update_call_counter(p, DEC_CALL_LIMIT);
			}
			ast_set_flag(p, SIP_NEEDDESTROY);		
		} else {
			/* If no extension was specified, use the s one */
			if (ast_strlen_zero(p->exten))
				ast_string_field_set(p, exten, "s");
			/* Initialize tag */	
			make_our_tag(p->tag, sizeof(p->tag));
			/* First invitation */
			c = sip_new(p, AST_STATE_DOWN, ast_strlen_zero(p->username) ? NULL : p->username);
			*recount = 1;
			/* Save Record-Route for any later requests we make on this dialogue */
			build_route(p, req, 0);
			if (c) {
				/* Pre-lock the call */
				ast_mutex_lock(&c->lock);
			}
		}
		
	} else {
		if (option_debug > 1 && sipdebug)
			ast_log(LOG_DEBUG, "Got a SIP re-invite for call %s\n", p->callid);
		c = p->owner;
	}
	if (!ignore && p)
		p->lastinvite = seqno;
	if (c) {
#ifdef OSP_SUPPORT
		ast_channel_setwhentohangup (c, p->osptimelimit);
#endif
		switch(c->_state) {
		case AST_STATE_DOWN:
			transmit_response(p, "100 Trying", req);
			ast_setstate(c, AST_STATE_RING);
			if (strcmp(p->exten, ast_pickup_ext())) {
				enum ast_pbx_result res;

				res = ast_pbx_start(c);

				switch (res) {
				case AST_PBX_FAILED:
					ast_log(LOG_WARNING, "Failed to start PBX :(\n");
					if (ignore)
						transmit_response(p, "503 Unavailable", req);
					else
						transmit_response_reliable(p, "503 Unavailable", req, 1);
					break;
				case AST_PBX_CALL_LIMIT:
					ast_log(LOG_WARNING, "Failed to start PBX (call limit reached) \n");
					if (ignore)
						transmit_response(p, "480 Temporarily Unavailable", req);
					else
						transmit_response_reliable(p, "480 Temporarily Unavailable", req, 1);
					break;
				case AST_PBX_SUCCESS:
					/* nothing to do */
					break;
				}

				if (res) {
					ast_log(LOG_WARNING, "Failed to start PBX :(\n");
					/* Unlock locks so ast_hangup can do its magic */
					ast_mutex_unlock(&c->lock);
					ast_mutex_unlock(&p->lock);
					ast_hangup(c);
					ast_mutex_lock(&p->lock);
					c = NULL;
				}
			} else {
				ast_mutex_unlock(&c->lock);
				if (ast_pickup_call(c)) {
					ast_log(LOG_NOTICE, "Nothing to pick up\n");
					if (ignore)
						transmit_response(p, "503 Unavailable", req);
					else
						transmit_response_reliable(p, "503 Unavailable", req, 1);
					ast_set_flag(p, SIP_ALREADYGONE);	
					/* Unlock locks so ast_hangup can do its magic */
					ast_mutex_unlock(&p->lock);
					ast_hangup(c);
					ast_mutex_lock(&p->lock);
					c = NULL;
				} else {
					ast_mutex_unlock(&p->lock);
					ast_setstate(c, AST_STATE_DOWN);
					ast_hangup(c);
					ast_mutex_lock(&p->lock);
					c = NULL;
				}
			}
			break;
		case AST_STATE_RING:
			transmit_response(p, "100 Trying", req);
			break;
		case AST_STATE_RINGING:
			transmit_response(p, "180 Ringing", req);
			break;
		case AST_STATE_UP:
			transmit_response_with_sdp(p, "200 OK", req, 1);
			break;
		default:
			ast_log(LOG_WARNING, "Don't know how to handle INVITE in state %d\n", c->_state);
			transmit_response(p, "100 Trying", req);
		}
	} else {
		if (p && !ast_test_flag(p, SIP_NEEDDESTROY) && !ignore) {
			if (!p->jointcapability) {
				if (ignore)
					transmit_response(p, "488 Not Acceptable Here (codec error)", req);
				else
					transmit_response_reliable(p, "488 Not Acceptable Here (codec error)", req, 1);
				ast_set_flag(p, SIP_NEEDDESTROY);	
			} else {
				ast_log(LOG_NOTICE, "Unable to create/find channel\n");
				if (ignore)
					transmit_response(p, "503 Unavailable", req);
				else
					transmit_response_reliable(p, "503 Unavailable", req, 1);
				ast_set_flag(p, SIP_NEEDDESTROY);	
			}
		}
	}
	return res;
}

/*! \brief  handle_request_refer: Handle incoming REFER request */
static int handle_request_refer(struct sip_pvt *p, struct sip_request *req, int debug, int ignore, int seqno, int *nounlock)
{
	struct ast_channel *c=NULL;
	int res;
	struct ast_channel *transfer_to;

	if (option_debug > 2)
		ast_log(LOG_DEBUG, "SIP call transfer received for call %s (REFER)!\n", p->callid);
	if (ast_strlen_zero(p->context))
		ast_string_field_set(p, context, default_context);
	res = get_refer_info(p, req);
	if (res < 0)
		transmit_response_with_allow(p, "404 Not Found", req, 1);
	else if (res > 0)
		transmit_response_with_allow(p, "484 Address Incomplete", req, 1);
	else {
		int nobye = 0;
		if (!ignore) {
			if (p->refer_call) {
				ast_log(LOG_DEBUG,"202 Accepted (supervised)\n");
				attempt_transfer(p, p->refer_call);
				if (p->refer_call->owner)
					ast_mutex_unlock(&p->refer_call->owner->lock);
				ast_mutex_unlock(&p->refer_call->lock);
				p->refer_call = NULL;
				ast_set_flag(p, SIP_GOTREFER);	
			} else {
				ast_log(LOG_DEBUG,"202 Accepted (blind)\n");
				c = p->owner;
				if (c) {
					transfer_to = ast_bridged_channel(c);
					if (transfer_to) {
						ast_log(LOG_DEBUG, "Got SIP blind transfer, applying to '%s'\n", transfer_to->name);
						ast_moh_stop(transfer_to);
						if (!strcmp(p->refer_to, ast_parking_ext())) {
							/* Must release c's lock now, because it will not longer
							    be accessible after the transfer! */
							*nounlock = 1;
							ast_mutex_unlock(&c->lock);
							sip_park(transfer_to, c, req);
							nobye = 1;
						} else {
							/* Must release c's lock now, because it will not longer
							    be accessible after the transfer! */
							*nounlock = 1;
							ast_mutex_unlock(&c->lock);
							ast_async_goto(transfer_to,p->context, p->refer_to,1);
						}
					} else {
						ast_log(LOG_DEBUG, "Got SIP blind transfer but nothing to transfer to.\n");
						ast_queue_hangup(p->owner);
					}
				}
				ast_set_flag(p, SIP_GOTREFER);	
			}
			transmit_response(p, "202 Accepted", req);
			transmit_notify_with_sipfrag(p, seqno);
			/* Always increment on a BYE */
			if (!nobye) {
				transmit_request_with_auth(p, SIP_BYE, 0, 1, 1);
				ast_set_flag(p, SIP_ALREADYGONE);	
			}
		}
	}
	return res;
}
/*! \brief  handle_request_cancel: Handle incoming CANCEL request */
static int handle_request_cancel(struct sip_pvt *p, struct sip_request *req, int debug, int ignore)
{
		
	check_via(p, req);
	ast_set_flag(p, SIP_ALREADYGONE);	
	if (p->rtp) {
		/* Immediately stop RTP */
		ast_rtp_stop(p->rtp);
	}
	if (p->vrtp) {
		/* Immediately stop VRTP */
		ast_rtp_stop(p->vrtp);
	}
	if (p->owner)
		ast_queue_hangup(p->owner);
	else
		ast_set_flag(p, SIP_NEEDDESTROY);	
	if (p->initreq.len > 0) {
		if (!ignore)
			transmit_response_reliable(p, "487 Request Terminated", &p->initreq, 1);
		transmit_response(p, "200 OK", req);
		return 1;
	} else {
		transmit_response(p, "481 Call Leg Does Not Exist", req);
		return 0;
	}
}

/*! \brief  handle_request_bye: Handle incoming BYE request */
static int handle_request_bye(struct sip_pvt *p, struct sip_request *req, int debug, int ignore)
{
	struct ast_channel *c=NULL;
	int res;
	struct ast_channel *bridged_to;
	char iabuf[INET_ADDRSTRLEN];
	
	if (p->pendinginvite && !ast_test_flag(p, SIP_OUTGOING) && !ignore)
		transmit_response_reliable(p, "487 Request Terminated", &p->initreq, 1);

	copy_request(&p->initreq, req);
	check_via(p, req);
	ast_set_flag(p, SIP_ALREADYGONE);	
	if (p->rtp) {
		/* Immediately stop RTP */
		ast_rtp_stop(p->rtp);
	}
	if (p->vrtp) {
		/* Immediately stop VRTP */
		ast_rtp_stop(p->vrtp);
	}
	if (!ast_strlen_zero(get_header(req, "Also"))) {
		ast_log(LOG_NOTICE, "Client '%s' using deprecated BYE/Also transfer method.  Ask vendor to support REFER instead\n",
			ast_inet_ntoa(iabuf, sizeof(iabuf), p->recv.sin_addr));
		if (ast_strlen_zero(p->context))
			ast_string_field_set(p, context, default_context);
		res = get_also_info(p, req);
		if (!res) {
			c = p->owner;
			if (c) {
				bridged_to = ast_bridged_channel(c);
				if (bridged_to) {
					/* Don't actually hangup here... */
					ast_moh_stop(bridged_to);
					ast_async_goto(bridged_to, p->context, p->refer_to,1);
				} else
					ast_queue_hangup(p->owner);
			}
		} else {
			ast_log(LOG_WARNING, "Invalid transfer information from '%s'\n", ast_inet_ntoa(iabuf, sizeof(iabuf), p->recv.sin_addr));
			if (p->owner)
				ast_queue_hangup(p->owner);
		}
	} else if (p->owner)
		ast_queue_hangup(p->owner);
	else
		ast_set_flag(p, SIP_NEEDDESTROY);	
	transmit_response(p, "200 OK", req);

	return 1;
}

/*! \brief  handle_request_message: Handle incoming MESSAGE request */
static int handle_request_message(struct sip_pvt *p, struct sip_request *req, int debug, int ignore)
{
	if (!ignore) {
		if (debug)
			ast_verbose("Receiving message!\n");
		receive_message(p, req);
	} else {
		transmit_response(p, "202 Accepted", req);
	}
	return 1;
}
/*! \brief  handle_request_subscribe: Handle incoming SUBSCRIBE request */
static int handle_request_subscribe(struct sip_pvt *p, struct sip_request *req, int debug, int ignore, struct sockaddr_in *sin, int seqno, char *e)
{
	int gotdest;
	int res = 0;
	int firststate = AST_EXTENSION_REMOVED;

	if (p->initreq.headers) {	
		/* We already have a dialog */
		if (p->initreq.method != SIP_SUBSCRIBE) {
			/* This is a SUBSCRIBE within another SIP dialog, which we do not support */
			/* For transfers, this could happen, but since we haven't seen it happening, let us just refuse this */
 			transmit_response(p, "403 Forbidden (within dialog)", req);
			/* Do not destroy session, since we will break the call if we do */
			ast_log(LOG_DEBUG, "Got a subscription within the context of another call, can't handle that - %s (Method %s)\n", p->callid, sip_methods[p->initreq.method].text);
			return 0;
		} else {
			if (debug)
				ast_log(LOG_DEBUG, "Got a re-subscribe on existing subscription %s\n", p->callid);
		}
	}
	if (!ignore && !p->initreq.headers) {
		/* Use this as the basis */
		if (debug)
			ast_verbose("Using latest SUBSCRIBE request as basis request\n");
		/* This call is no longer outgoing if it ever was */
		ast_clear_flag(p, SIP_OUTGOING);
		copy_request(&p->initreq, req);
		check_via(p, req);
	} else if (debug && ignore)
		ast_verbose("Ignoring this SUBSCRIBE request\n");

	if (!p->lastinvite) {
		char mailboxbuf[256]="";
		int found = 0;
		char *mailbox = NULL;
		int mailboxsize = 0;

		char *event = get_header(req, "Event");	/* Get Event package name */
		char *accept = get_header(req, "Accept");

 		if (!strcmp(event, "message-summary") && !strcmp(accept, "application/simple-message-summary")) {
			mailbox = mailboxbuf;
			mailboxsize = sizeof(mailboxbuf);
		}
		/* Handle authentication if this is our first subscribe */
		res = check_user_full(p, req, SIP_SUBSCRIBE, e, 0, sin, ignore, mailbox, mailboxsize);
		if (res) {
			if (res < 0) {
				ast_log(LOG_NOTICE, "Failed to authenticate user %s for SUBSCRIBE\n", get_header(req, "From"));
				ast_set_flag(p, SIP_NEEDDESTROY);	
			}
			return 0;
		}
		/* Initialize the context if it hasn't been already */
		if (!ast_strlen_zero(p->subscribecontext))
			ast_string_field_set(p, context, p->subscribecontext);
		else if (ast_strlen_zero(p->context))
			ast_string_field_set(p, context, default_context);
		/* Get destination right away */
		gotdest = get_destination(p, NULL);
		build_contact(p);
		if (gotdest) {
			if (gotdest < 0)
				transmit_response(p, "404 Not Found", req);
			else
				transmit_response(p, "484 Address Incomplete", req);	/* Overlap dialing on SUBSCRIBE?? */
			ast_set_flag(p, SIP_NEEDDESTROY);	
		} else {

			/* Initialize tag for new subscriptions */	
			if (ast_strlen_zero(p->tag))
				make_our_tag(p->tag, sizeof(p->tag));

			if (!strcmp(event, "presence") || !strcmp(event, "dialog")) { /* Presence, RFC 3842 */

 				/* Header from Xten Eye-beam Accept: multipart/related, application/rlmi+xml, application/pidf+xml, application/xpidf+xml */
 				if (strstr(accept, "application/pidf+xml")) {
 					p->subscribed = PIDF_XML;         /* RFC 3863 format */
 				} else if (strstr(accept, "application/dialog-info+xml")) {
 					p->subscribed = DIALOG_INFO_XML;
 					/* IETF draft: draft-ietf-sipping-dialog-package-05.txt */
 				} else if (strstr(accept, "application/cpim-pidf+xml")) {
 					p->subscribed = CPIM_PIDF_XML;    /* RFC 3863 format */
 				} else if (strstr(accept, "application/xpidf+xml")) {
 					p->subscribed = XPIDF_XML;        /* Early pre-RFC 3863 format with MSN additions (Microsoft Messenger) */
 				} else if (strstr(p->useragent, "Polycom")) {
 					p->subscribed = XPIDF_XML;        /*  Polycoms subscribe for "event: dialog" but don't include an "accept:" header */
				} else {
 					/* Can't find a format for events that we know about */
 					transmit_response(p, "489 Bad Event", req);
 					ast_set_flag(p, SIP_NEEDDESTROY);	
 					return 0;
 				}
 			} else if (!strcmp(event, "message-summary") && !strcmp(accept, "application/simple-message-summary")) {
				/* Looks like they actually want a mailbox status */

				/* At this point, we should check if they subscribe to a mailbox that
				  has the same extension as the peer or the mailbox id. If we configure
				  the context to be the same as a SIP domain, we could check mailbox
				  context as well. To be able to securely accept subscribes on mailbox
				  IDs, not extensions, we need to check the digest auth user to make
				  sure that the user has access to the mailbox.
				 
				  Since we do not act on this subscribe anyway, we might as well 
				  accept any authenticated peer with a mailbox definition in their 
				  config section.
				
				*/
				if (!ast_strlen_zero(mailbox)) {
					found++;
				}

				if (found){
					transmit_response(p, "200 OK", req);
					ast_set_flag(p, SIP_NEEDDESTROY);	
				} else {
					transmit_response(p, "404 Not found", req);
					ast_set_flag(p, SIP_NEEDDESTROY);	
				}
				return 0;
			} else { /* At this point, Asterisk does not understand the specified event */
				transmit_response(p, "489 Bad Event", req);
				if (option_debug > 1)
					ast_log(LOG_DEBUG, "Received SIP subscribe for unknown event package: %s\n", event);
				ast_set_flag(p, SIP_NEEDDESTROY);	
				return 0;
			}
			if (p->subscribed != NONE)
				p->stateid = ast_extension_state_add(p->context, p->exten, cb_extensionstate, p);
		}
	}

	if (!ignore && p)
		p->lastinvite = seqno;
	if (p && !ast_test_flag(p, SIP_NEEDDESTROY)) {
		p->expiry = atoi(get_header(req, "Expires"));

		/* check if the requested expiry-time is within the approved limits from sip.conf */
		if (p->expiry > max_expiry)
			p->expiry = max_expiry;
		if (p->expiry < min_expiry && p->expiry > 0)
			p->expiry = min_expiry;

		if (sipdebug || option_debug > 1)
			ast_log(LOG_DEBUG, "Adding subscription for extension %s context %s for peer %s\n", p->exten, p->context, p->username);
		if (p->autokillid > -1)
			sip_cancel_destroy(p);	/* Remove subscription expiry for renewals */
		if (p->expiry > 0)
			sip_scheddestroy(p, (p->expiry + 10) * 1000);	/* Set timer for destruction of call at expiration */

		if ((firststate = ast_extension_state(NULL, p->context, p->exten)) < 0) {
			ast_log(LOG_ERROR, "Got SUBSCRIBE for extensions without hint. Please add hint to %s in context %s\n", p->exten, p->context);
			transmit_response(p, "404 Not found", req);
			ast_set_flag(p, SIP_NEEDDESTROY);	
			return 0;
		} else {
			struct sip_pvt *p_old;

			transmit_response(p, "200 OK", req);
			transmit_state_notify(p, firststate, 1);	/* Send first notification */
			append_history(p, "Subscribestatus", "%s", ast_extension_state2str(firststate));

			/* remove any old subscription from this peer for the same exten/context,
			   as the peer has obviously forgotten about it and it's wasteful to wait
			   for it to expire and send NOTIFY messages to the peer only to have them
			   ignored (or generate errors)
			*/
			ast_mutex_lock(&iflock);
			for (p_old = iflist; p_old; p_old = p_old->next) {
				if (p_old == p)
					continue;
				if (p_old->initreq.method != SIP_SUBSCRIBE)
					continue;
				if (p_old->subscribed == NONE)
					continue;
				ast_mutex_lock(&p_old->lock);
				if (!strcmp(p_old->username, p->username)) {
					if (!strcmp(p_old->exten, p->exten) &&
					    !strcmp(p_old->context, p->context)) {
						ast_set_flag(p_old, SIP_NEEDDESTROY);
						ast_mutex_unlock(&p_old->lock);
						break;
					}
				}
				ast_mutex_unlock(&p_old->lock);
			}
			ast_mutex_unlock(&iflock);
		}
		if (!p->expiry)
			ast_set_flag(p, SIP_NEEDDESTROY);
	}
	return 1;
}

/*! \brief  handle_request_register: Handle incoming REGISTER request */
static int handle_request_register(struct sip_pvt *p, struct sip_request *req, int debug, int ignore, struct sockaddr_in *sin, char *e)
{
	int res = 0;
	char iabuf[INET_ADDRSTRLEN];

	/* Use this as the basis */
	if (debug)
		ast_verbose("Using latest REGISTER request as basis request\n");
	copy_request(&p->initreq, req);
	check_via(p, req);
	if ((res = register_verify(p, sin, req, e, ignore)) < 0) 
		ast_log(LOG_NOTICE, "Registration from '%s' failed for '%s' - %s\n", get_header(req, "To"), ast_inet_ntoa(iabuf, sizeof(iabuf), sin->sin_addr), (res == -1) ? "Wrong password" : (res == -2 ? "Username/auth name mismatch" : "Not a local SIP domain"));
	if (res < 1) {
		/* Destroy the session, but keep us around for just a bit in case they don't
		   get our 200 OK */
		sip_scheddestroy(p, 15*1000);
	}
	return res;
}

/*! \brief  handle_request: Handle SIP requests (methods) */
/*      this is where all incoming requests go first   */
static int handle_request(struct sip_pvt *p, struct sip_request *req, struct sockaddr_in *sin, int *recount, int *nounlock)
{
	/* Called with p->lock held, as well as p->owner->lock if appropriate, keeping things
	   relatively static */
	struct sip_request resp;
	char *cmd;
	char *cseq;
	char *useragent;
	int seqno;
	int len;
	int ignore=0;
	int respid;
	int res = 0;
	char iabuf[INET_ADDRSTRLEN];
	int debug = sip_debug_test_pvt(p);
	char *e;
	int error = 0;

	/* Clear out potential response */
	memset(&resp, 0, sizeof(resp));

	/* Get Method and Cseq */
	cseq = get_header(req, "Cseq");
	cmd = req->header[0];

	/* Must have Cseq */
	if (ast_strlen_zero(cmd) || ast_strlen_zero(cseq)) {
		ast_log(LOG_ERROR, "Missing Cseq. Dropping this SIP message, it's incomplete.\n");
		error = 1;
	}
	if (!error && sscanf(cseq, "%d%n", &seqno, &len) != 1) {
		ast_log(LOG_ERROR, "No seqno in '%s'. Dropping incomplete message.\n", cmd);
		error = 1;
	}
	if (error) {
		if (!p->initreq.header)	/* New call */
			ast_set_flag(p, SIP_NEEDDESTROY);	/* Make sure we destroy this dialog */
		return -1;
	}
	/* Get the command XXX */

	cmd = req->rlPart1;
	e = req->rlPart2;

	/* Save useragent of the client */
	useragent = get_header(req, "User-Agent");
	if (!ast_strlen_zero(useragent))
		ast_string_field_set(p, useragent, useragent);

	/* Find out SIP method for incoming request */
	if (req->method == SIP_RESPONSE) {	/* Response to our request */
		/* Response to our request -- Do some sanity checks */	
		if (!p->initreq.headers) {
			ast_log(LOG_DEBUG, "That's odd...  Got a response on a call we dont know about. Cseq %d Cmd %s\n", seqno, cmd);
			ast_set_flag(p, SIP_NEEDDESTROY);	
			return 0;
		} else if (p->ocseq && (p->ocseq < seqno)) {
			ast_log(LOG_DEBUG, "Ignoring out of order response %d (expecting %d)\n", seqno, p->ocseq);
			return -1;
		} else if (p->ocseq && (p->ocseq != seqno)) {
			/* ignore means "don't do anything with it" but still have to 
			   respond appropriately  */
			ignore=1;
		}
	
		e = ast_skip_blanks(e);
		if (sscanf(e, "%d %n", &respid, &len) != 1) {
			ast_log(LOG_WARNING, "Invalid response: '%s'\n", e);
		} else {
			/* More SIP ridiculousness, we have to ignore bogus contacts in 100 etc responses */
			if ((respid == 200) || ((respid >= 300) && (respid <= 399)))
				extract_uri(p, req);
			handle_response(p, respid, e + len, req, ignore, seqno);
		}
		return 0;
	}

	/* New SIP request coming in 
	   (could be new request in existing SIP dialog as well...) 
	 */			
	
	p->method = req->method;	/* Find out which SIP method they are using */
	if (option_debug > 2)
		ast_log(LOG_DEBUG, "**** Received %s (%d) - Command in SIP %s\n", sip_methods[p->method].text, sip_methods[p->method].id, cmd); 

	if (p->icseq && (p->icseq > seqno)) {
		if (option_debug)
			ast_log(LOG_DEBUG, "Ignoring too old SIP packet packet %d (expecting >= %d)\n", seqno, p->icseq);
		if (req->method != SIP_ACK)
			transmit_response(p, "503 Server error", req);	/* We must respond according to RFC 3261 sec 12.2 */
		return -1;
	} else if (p->icseq && (p->icseq == seqno) && req->method != SIP_ACK &&(p->method != SIP_CANCEL|| ast_test_flag(p, SIP_ALREADYGONE))) {
		/* ignore means "don't do anything with it" but still have to 
		   respond appropriately.  We do this if we receive a repeat of
		   the last sequence number  */
		ignore=2;
		if (option_debug > 2)
			ast_log(LOG_DEBUG, "Ignoring SIP message because of retransmit (%s Seqno %d, ours %d)\n", sip_methods[p->method].text, p->icseq, seqno);
	}
		
	if (seqno >= p->icseq)
		/* Next should follow monotonically (but not necessarily 
		   incrementally -- thanks again to the genius authors of SIP --
		   increasing */
		p->icseq = seqno;

	/* Find their tag if we haven't got it */
	if (ast_strlen_zero(p->theirtag)) {
		char tag[128];

		gettag(req, "From", tag, sizeof(tag));
		ast_string_field_set(p, theirtag, tag);
	}
	snprintf(p->lastmsg, sizeof(p->lastmsg), "Rx: %s", cmd);

	if (pedanticsipchecking) {
		/* If this is a request packet without a from tag, it's not
			correct according to RFC 3261  */
		/* Check if this a new request in a new dialog with a totag already attached to it,
			RFC 3261 - section 12.2 - and we don't want to mess with recovery  */
		if (!p->initreq.headers && ast_test_flag(req, SIP_PKT_WITH_TOTAG)) {
			/* If this is a first request and it got a to-tag, it is not for us */
			if (!ignore && req->method == SIP_INVITE) {
				transmit_response_reliable(p, "481 Call/Transaction Does Not Exist", req, 1);
				/* Will cease to exist after ACK */
			} else {
				transmit_response(p, "481 Call/Transaction Does Not Exist", req);
				ast_set_flag(p, SIP_NEEDDESTROY);
			}
			return res;
		}
	}

	/* Handle various incoming SIP methods in requests */
	switch (p->method) {
	case SIP_OPTIONS:
		res = handle_request_options(p, req, debug);
		break;
	case SIP_INVITE:
		res = handle_request_invite(p, req, debug, ignore, seqno, sin, recount, e);
		break;
	case SIP_REFER:
		res = handle_request_refer(p, req, debug, ignore, seqno, nounlock);
		break;
	case SIP_CANCEL:
		res = handle_request_cancel(p, req, debug, ignore);
		break;
	case SIP_BYE:
		res = handle_request_bye(p, req, debug, ignore);
		break;
	case SIP_MESSAGE:
		res = handle_request_message(p, req, debug, ignore);
		break;
	case SIP_SUBSCRIBE:
		res = handle_request_subscribe(p, req, debug, ignore, sin, seqno, e);
		break;
	case SIP_REGISTER:
		res = handle_request_register(p, req, debug, ignore, sin, e);
		break;
	case SIP_INFO:
		if (!ignore) {
			if (debug)
				ast_verbose("Receiving INFO!\n");
			handle_request_info(p, req);
		} else { /* if ignoring, transmit response */
			transmit_response(p, "200 OK", req);
		}
		break;
	case SIP_NOTIFY:
		/* XXX we get NOTIFY's from some servers. WHY?? Maybe we should
			look into this someday XXX */
		transmit_response(p, "200 OK", req);
		if (!p->lastinvite) 
			ast_set_flag(p, SIP_NEEDDESTROY);	
		break;
	case SIP_ACK:
		/* Make sure we don't ignore this */
		if (seqno == p->pendinginvite) {
			p->pendinginvite = 0;
			__sip_ack(p, seqno, FLAG_RESPONSE, -1);
			if (!ast_strlen_zero(get_header(req, "Content-Type"))) {
				if (process_sdp(p, req))
					return -1;
			} 
			check_pendings(p);
		}
		if (!p->lastinvite && ast_strlen_zero(p->randdata))
			ast_set_flag(p, SIP_NEEDDESTROY);	
		break;
	default:
		transmit_response_with_allow(p, "501 Method Not Implemented", req, 0);
		ast_log(LOG_NOTICE, "Unknown SIP command '%s' from '%s'\n", 
			cmd, ast_inet_ntoa(iabuf, sizeof(iabuf), p->sa.sin_addr));
		/* If this is some new method, and we don't have a call, destroy it now */
		if (!p->initreq.headers)
			ast_set_flag(p, SIP_NEEDDESTROY);	
		break;
	}
	return res;
}

/*! \brief  sipsock_read: Read data from SIP socket */
/*    Successful messages is connected to SIP call and forwarded to handle_request() */
static int sipsock_read(int *id, int fd, short events, void *ignore)
{
	struct sip_request req;
	struct sockaddr_in sin = { 0, };
	struct sip_pvt *p;
	int res;
	socklen_t len;
	int nounlock;
	int recount = 0;
	char iabuf[INET_ADDRSTRLEN];

	len = sizeof(sin);
	memset(&req, 0, sizeof(req));
	res = recvfrom(sipsock, req.data, sizeof(req.data) - 1, 0, (struct sockaddr *)&sin, &len);
	if (res < 0) {
#if !defined(__FreeBSD__)
		if (errno == EAGAIN)
			ast_log(LOG_NOTICE, "SIP: Received packet with bad UDP checksum\n");
		else 
#endif
		if (errno != ECONNREFUSED)
			ast_log(LOG_WARNING, "Recv error: %s\n", strerror(errno));
		return 1;
	}
	if (res == sizeof(req.data)) {
		ast_log(LOG_DEBUG, "Received packet exceeds buffer. Data is possibly lost\n");
	}
	req.data[res] = '\0';
	req.len = res;
	if(sip_debug_test_addr(&sin))
		ast_set_flag(&req, SIP_PKT_DEBUG);
	if (pedanticsipchecking)
		req.len = lws2sws(req.data, req.len);	/* Fix multiline headers */
	if (ast_test_flag(&req, SIP_PKT_DEBUG)) {
		ast_verbose("\n<-- SIP read from %s:%d: \n%s\n", ast_inet_ntoa(iabuf, sizeof(iabuf), sin.sin_addr), ntohs(sin.sin_port), req.data);
	}
	parse_request(&req);
	req.method = find_sip_method(req.rlPart1);
	if (ast_test_flag(&req, SIP_PKT_DEBUG)) {
		ast_verbose("--- (%d headers %d lines)", req.headers, req.lines);
		if (req.headers + req.lines == 0) 
			ast_verbose(" Nat keepalive ");
		ast_verbose("---\n");
	}

	if (req.headers < 2) {
		/* Must have at least two headers */
		return 1;
	}


	/* Process request, with netlock held */
retrylock:
	ast_mutex_lock(&netlock);
	p = find_call(&req, &sin, req.method);
	if (p) {
		/* Go ahead and lock the owner if it has one -- we may need it */
		if (p->owner && ast_mutex_trylock(&p->owner->lock)) {
			ast_log(LOG_DEBUG, "Failed to grab lock, trying again...\n");
			ast_mutex_unlock(&p->lock);
			ast_mutex_unlock(&netlock);
			/* Sleep infintismly short amount of time */
			usleep(1);
			goto retrylock;
		}
		memcpy(&p->recv, &sin, sizeof(p->recv));
		if (recordhistory) /* This is a response, note what it was for */
			append_history(p, "Rx", "%s / %s", req.data, get_header(&req, "CSeq"));
		nounlock = 0;
		if (handle_request(p, &req, &sin, &recount, &nounlock) == -1) {
			/* Request failed */
			ast_log(LOG_DEBUG, "SIP message could not be handled, bad request: %-70.70s\n", p->callid[0] ? p->callid : "<no callid>");
		}
		
		if (p->owner && !nounlock)
			ast_mutex_unlock(&p->owner->lock);
		ast_mutex_unlock(&p->lock);
	}
	ast_mutex_unlock(&netlock);
	if (recount)
		ast_update_use_count();

	return 1;
}

/*! \brief  sip_send_mwi_to_peer: Send message waiting indication */
static int sip_send_mwi_to_peer(struct sip_peer *peer)
{
	/* Called with peerl lock, but releases it */
	struct sip_pvt *p;
	int newmsgs, oldmsgs;

	/* Check for messages */
	ast_app_messagecount(peer->mailbox, &newmsgs, &oldmsgs);
	
	time(&peer->lastmsgcheck);
	
	/* Return now if it's the same thing we told them last time */
	if (((newmsgs << 8) | (oldmsgs)) == peer->lastmsgssent) {
		return 0;
	}
	
	if (!(p = sip_alloc(NULL, NULL, 0, SIP_NOTIFY)))
		return -1;
	
	peer->lastmsgssent = ((newmsgs << 8) | (oldmsgs));
	if (create_addr_from_peer(p, peer)) {
		/* Maybe they're not registered, etc. */
		sip_destroy(p);
		return 0;
	}
	/* Recalculate our side, and recalculate Call ID */
	if (ast_sip_ouraddrfor(&p->sa.sin_addr,&p->ourip))
		memcpy(&p->ourip, &__ourip, sizeof(p->ourip));
	build_via(p);
	build_callid_pvt(p);
	/* Send MWI */
	ast_set_flag(p, SIP_OUTGOING);
	transmit_notify_with_mwi(p, newmsgs, oldmsgs, peer->vmexten);
	sip_scheddestroy(p, 15000);
	return 0;
}

/*! \brief  do_monitor: The SIP monitoring thread */
static void *do_monitor(void *data)
{
	int res;
	struct sip_pvt *sip;
	struct sip_peer *peer = NULL;
	time_t t;
	int fastrestart =0;
	int lastpeernum = -1;
	int curpeernum;
	int reloading;

	/* Add an I/O event to our UDP socket */
	if (sipsock > -1) 
		ast_io_add(io, sipsock, sipsock_read, AST_IO_IN, NULL);
	
	/* This thread monitors all the frame relay interfaces which are not yet in use
	   (and thus do not have a separate thread) indefinitely */
	/* From here on out, we die whenever asked */
	for(;;) {
		/* Check for a reload request */
		ast_mutex_lock(&sip_reload_lock);
		reloading = sip_reloading;
		sip_reloading = 0;
		ast_mutex_unlock(&sip_reload_lock);
		if (reloading) {
			if (option_verbose > 0)
				ast_verbose(VERBOSE_PREFIX_1 "Reloading SIP\n");
			sip_do_reload(sip_reloadreason);
		}
		/* Check for interfaces needing to be killed */
		ast_mutex_lock(&iflock);
restartsearch:		
		time(&t);
		for (sip = iflist; sip; sip = sip->next) {
			ast_mutex_lock(&sip->lock);
			if (sip->rtp && sip->owner && (sip->owner->_state == AST_STATE_UP) && !sip->redirip.sin_addr.s_addr) {
				if (sip->lastrtptx && sip->rtpkeepalive && t > sip->lastrtptx + sip->rtpkeepalive) {
					/* Need to send an empty RTP packet */
					time(&sip->lastrtptx);
					ast_rtp_sendcng(sip->rtp, 0);
				}
				if (sip->lastrtprx && (sip->rtptimeout || sip->rtpholdtimeout) && t > sip->lastrtprx + sip->rtptimeout) {
					/* Might be a timeout now -- see if we're on hold */
					struct sockaddr_in sin;
					ast_rtp_get_peer(sip->rtp, &sin);
					if (sin.sin_addr.s_addr || 
							(sip->rtpholdtimeout && 
							  (t > sip->lastrtprx + sip->rtpholdtimeout))) {
						/* Needs a hangup */
						if (sip->rtptimeout) {
							while(sip->owner && ast_mutex_trylock(&sip->owner->lock)) {
								ast_mutex_unlock(&sip->lock);
								usleep(1);
								ast_mutex_lock(&sip->lock);
							}
							if (sip->owner) {
								ast_log(LOG_NOTICE, "Disconnecting call '%s' for lack of RTP activity in %ld seconds\n", sip->owner->name, (long)(t - sip->lastrtprx));
								/* Issue a softhangup */
								ast_softhangup(sip->owner, AST_SOFTHANGUP_DEV);
								ast_mutex_unlock(&sip->owner->lock);
							}
						}
					}
				}
			}
			if (ast_test_flag(sip, SIP_NEEDDESTROY) && !sip->packets && !sip->owner) {
				ast_mutex_unlock(&sip->lock);
				__sip_destroy(sip, 1);
				goto restartsearch;
			}
			ast_mutex_unlock(&sip->lock);
		}
		ast_mutex_unlock(&iflock);
		/* Don't let anybody kill us right away.  Nobody should lock the interface list
		   and wait for the monitor list, but the other way around is okay. */
		ast_mutex_lock(&monlock);
		/* Lock the network interface */
		ast_mutex_lock(&netlock);
		/* Okay, now that we know what to do, release the network lock */
		ast_mutex_unlock(&netlock);
		/* And from now on, we're okay to be killed, so release the monitor lock as well */
		ast_mutex_unlock(&monlock);
		pthread_testcancel();
		/* Wait for sched or io */
		res = ast_sched_wait(sched);
		if ((res < 0) || (res > 1000))
			res = 1000;
		/* If we might need to send more mailboxes, don't wait long at all.*/
		if (fastrestart)
			res = 1;
		res = ast_io_wait(io, res);
		if (res > 20)
			ast_log(LOG_DEBUG, "chan_sip: ast_io_wait ran %d all at once\n", res);
		ast_mutex_lock(&monlock);
		if (res >= 0)  {
			res = ast_sched_runq(sched);
			if (res >= 20)
				ast_log(LOG_DEBUG, "chan_sip: ast_sched_runq ran %d all at once\n", res);
		}

		/* needs work to send mwi to realtime peers */
		time(&t);
		fastrestart = 0;
		curpeernum = 0;
		peer = NULL;
		ASTOBJ_CONTAINER_TRAVERSE(&peerl, !peer, do {
			if ((curpeernum > lastpeernum) && !ast_strlen_zero(iterator->mailbox) && ((t - iterator->lastmsgcheck) > global_mwitime)) {
				fastrestart = 1;
				lastpeernum = curpeernum;
				peer = ASTOBJ_REF(iterator);
			};
			curpeernum++;
		} while (0)
		);
		if (peer) {
			ASTOBJ_WRLOCK(peer);
			sip_send_mwi_to_peer(peer);
			ASTOBJ_UNLOCK(peer);
			ASTOBJ_UNREF(peer,sip_destroy_peer);
		} else {
			/* Reset where we come from */
			lastpeernum = -1;
		}
		ast_mutex_unlock(&monlock);
	}
	/* Never reached */
	return NULL;
	
}

/*! \brief  restart_monitor: Start the channel monitor thread */
static int restart_monitor(void)
{
	pthread_attr_t attr;
	/* If we're supposed to be stopped -- stay stopped */
	if (monitor_thread == AST_PTHREADT_STOP)
		return 0;
	if (ast_mutex_lock(&monlock)) {
		ast_log(LOG_WARNING, "Unable to lock monitor\n");
		return -1;
	}
	if (monitor_thread == pthread_self()) {
		ast_mutex_unlock(&monlock);
		ast_log(LOG_WARNING, "Cannot kill myself\n");
		return -1;
	}
	if (monitor_thread != AST_PTHREADT_NULL) {
		/* Wake up the thread */
		pthread_kill(monitor_thread, SIGURG);
	} else {
		pthread_attr_init(&attr);
		pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
		/* Start a new monitor */
		if (ast_pthread_create(&monitor_thread, &attr, do_monitor, NULL) < 0) {
			ast_mutex_unlock(&monlock);
			ast_log(LOG_ERROR, "Unable to start monitor thread.\n");
			return -1;
		}
	}
	ast_mutex_unlock(&monlock);
	return 0;
}

/*! \brief  sip_poke_noanswer: No answer to Qualify poke */
static int sip_poke_noanswer(void *data)
{
	struct sip_peer *peer = data;
	
	peer->pokeexpire = -1;
	if (peer->lastms > -1) {
		ast_log(LOG_NOTICE, "Peer '%s' is now UNREACHABLE!  Last qualify: %d\n", peer->name, peer->lastms);
		manager_event(EVENT_FLAG_SYSTEM, "PeerStatus", "Peer: SIP/%s\r\nPeerStatus: Unreachable\r\nTime: %d\r\n", peer->name, -1);
	}
	if (peer->call)
		sip_destroy(peer->call);
	peer->call = NULL;
	peer->lastms = -1;
	ast_device_state_changed("SIP/%s", peer->name);
	/* Try again quickly */
	peer->pokeexpire = ast_sched_add(sched, DEFAULT_FREQ_NOTOK, sip_poke_peer_s, peer);
	return 0;
}

/*! \brief  sip_poke_peer: Check availability of peer, also keep NAT open */
/*	This is done with the interval in qualify= option in sip.conf */
/*	Default is 2 seconds */
static int sip_poke_peer(struct sip_peer *peer)
{
	struct sip_pvt *p;
	if (!peer->maxms || !peer->addr.sin_addr.s_addr) {
		/* IF we have no IP, or this isn't to be monitored, return
		  imeediately after clearing things out */
		if (peer->pokeexpire > -1)
			ast_sched_del(sched, peer->pokeexpire);
		peer->lastms = 0;
		peer->pokeexpire = -1;
		peer->call = NULL;
		return 0;
	}
	if (peer->call > 0) {
		if (sipdebug)
			ast_log(LOG_NOTICE, "Still have a QUALIFY dialog active, deleting\n");
		sip_destroy(peer->call);
	}
	if (!(p = peer->call = sip_alloc(NULL, NULL, 0, SIP_OPTIONS)))
		return -1;
	
	memcpy(&p->sa, &peer->addr, sizeof(p->sa));
	memcpy(&p->recv, &peer->addr, sizeof(p->sa));

	/* Send options to peer's fullcontact */
	if (!ast_strlen_zero(peer->fullcontact))
		ast_string_field_set(p, fullcontact, peer->fullcontact);

	if (!ast_strlen_zero(peer->tohost))
		ast_string_field_set(p, tohost, peer->tohost);
	else {
		char iabuf[INET_ADDRSTRLEN];

		ast_inet_ntoa(iabuf, sizeof(iabuf), peer->addr.sin_addr);
		ast_string_field_set(p, tohost, iabuf);
	}

	/* Recalculate our side, and recalculate Call ID */
	if (ast_sip_ouraddrfor(&p->sa.sin_addr,&p->ourip))
		memcpy(&p->ourip, &__ourip, sizeof(p->ourip));
	build_via(p);
	build_callid_pvt(p);

	if (peer->pokeexpire > -1)
		ast_sched_del(sched, peer->pokeexpire);
	p->peerpoke = peer;
	ast_set_flag(p, SIP_OUTGOING);
#ifdef VOCAL_DATA_HACK
	ast_copy_string(p->username, "__VOCAL_DATA_SHOULD_READ_THE_SIP_SPEC__", sizeof(p->username));
	transmit_invite(p, SIP_INVITE, 0, 2);
#else
	transmit_invite(p, SIP_OPTIONS, 0, 2);
#endif
	gettimeofday(&peer->ps, NULL);
	peer->pokeexpire = ast_sched_add(sched, DEFAULT_MAXMS * 2, sip_poke_noanswer, peer);

	return 0;
}

/*! \brief  sip_devicestate: Part of PBX channel interface */

/* Return values:---
	If we have qualify on and the device is not reachable, regardless of registration
	state we return AST_DEVICE_UNAVAILABLE

	For peers with call limit:
		not registered			AST_DEVICE_UNAVAILABLE
		registered, no call		AST_DEVICE_NOT_INUSE
		registered, calls possible	AST_DEVICE_INUSE
		registered, call limit reached	AST_DEVICE_BUSY
	For peers without call limit:
		not registered			AST_DEVICE_UNAVAILABLE
		registered			AST_DEVICE_UNKNOWN
*/
static int sip_devicestate(void *data)
{
	char *host;
	char *tmp;

	struct hostent *hp;
	struct ast_hostent ahp;
	struct sip_peer *p;

	int res = AST_DEVICE_INVALID;

	host = ast_strdupa(data);
	if ((tmp = strchr(host, '@')))
		host = tmp + 1;

	if (option_debug > 2) 
		ast_log(LOG_DEBUG, "Checking device state for peer %s\n", host);

	if ((p = find_peer(host, NULL, 1))) {
		if (p->addr.sin_addr.s_addr || p->defaddr.sin_addr.s_addr) {
			/* we have an address for the peer */
			/* if qualify is turned on, check the status */
			if (p->maxms && (p->lastms > p->maxms)) {
				res = AST_DEVICE_UNAVAILABLE;
			} else {
				/* qualify is not on, or the peer is responding properly */
				/* check call limit */
				if (p->call_limit && (p->inUse == p->call_limit))
					res = AST_DEVICE_BUSY;
				else if (p->call_limit && p->inUse)
					res = AST_DEVICE_INUSE;
				else if (p->call_limit)
					res = AST_DEVICE_NOT_INUSE;
				else
					res = AST_DEVICE_UNKNOWN;
			}
		} else {
			/* there is no address, it's unavailable */
			res = AST_DEVICE_UNAVAILABLE;
		}
		ASTOBJ_UNREF(p,sip_destroy_peer);
	} else {
		hp = ast_gethostbyname(host, &ahp);
		if (hp)
			res = AST_DEVICE_UNKNOWN;
	}

	return res;
}

/*! \brief  sip_request: PBX interface function -build SIP pvt structure */
/* SIP calls initiated by the PBX arrive here */
static struct ast_channel *sip_request_call(const char *type, int format, void *data, int *cause)
{
	int oldformat;
	struct sip_pvt *p;
	struct ast_channel *tmpc = NULL;
	char *ext, *host;
	char tmp[256];
	char *dest = data;

	oldformat = format;
	format &= ((AST_FORMAT_MAX_AUDIO << 1) - 1);
	if (!format) {
		ast_log(LOG_NOTICE, "Asked to get a channel of unsupported format %s while capability is %s\n", ast_getformatname(oldformat), ast_getformatname(global_capability));
		return NULL;
	}
	if (!(p = sip_alloc(NULL, NULL, 0, SIP_INVITE)))
		return NULL;

	if (!(p->options = ast_calloc(1, sizeof(*p->options))))
		return NULL;

	ast_copy_string(tmp, dest, sizeof(tmp));
	host = strchr(tmp, '@');
	if (host) {
		*host = '\0';
		host++;
		ext = tmp;
	} else {
		ext = strchr(tmp, '/');
		if (ext) {
			*ext++ = '\0';
			host = tmp;
		}
		else {
			host = tmp;
			ext = NULL;
		}
	}

	if (create_addr(p, host)) {
		*cause = AST_CAUSE_UNREGISTERED;
		sip_destroy(p);
		return NULL;
	}
	if (ast_strlen_zero(p->peername) && ext)
		ast_string_field_set(p, peername, ext);
	/* Recalculate our side, and recalculate Call ID */
	if (ast_sip_ouraddrfor(&p->sa.sin_addr,&p->ourip))
		memcpy(&p->ourip, &__ourip, sizeof(p->ourip));
	build_via(p);
	build_callid_pvt(p);
	
	/* We have an extension to call, don't use the full contact here */
	/* This to enable dialing registered peers with extension dialling,
	   like SIP/peername/extension 	
	   SIP/peername will still use the full contact */
	if (ext) {
		ast_string_field_set(p, username, ext);
		ast_string_field_free(p, fullcontact);
	}
#if 0
	printf("Setting up to call extension '%s' at '%s'\n", ext ? ext : "<none>", host);
#endif
	p->prefcodec = format;
	ast_mutex_lock(&p->lock);
	tmpc = sip_new(p, AST_STATE_DOWN, host);	/* Place the call */
	ast_mutex_unlock(&p->lock);
	if (!tmpc)
		sip_destroy(p);
	ast_update_use_count();
	restart_monitor();
	return tmpc;
}

/*! \brief  handle_common_options: Handle flag-type options common to users and peers */
static int handle_common_options(struct ast_flags *flags, struct ast_flags *mask, struct ast_variable *v)
{
	int res = 0;

	if (!strcasecmp(v->name, "trustrpid")) {
		ast_set_flag(mask, SIP_TRUSTRPID);
		ast_set2_flag(flags, ast_true(v->value), SIP_TRUSTRPID);
		res = 1;
	} else if (!strcasecmp(v->name, "sendrpid")) {
		ast_set_flag(mask, SIP_SENDRPID);
		ast_set2_flag(flags, ast_true(v->value), SIP_SENDRPID);
		res = 1;
	} else if (!strcasecmp(v->name, "useclientcode")) {
		ast_set_flag(mask, SIP_USECLIENTCODE);
		ast_set2_flag(flags, ast_true(v->value), SIP_USECLIENTCODE);
		res = 1;
	} else if (!strcasecmp(v->name, "dtmfmode")) {
		ast_set_flag(mask, SIP_DTMF);
		ast_clear_flag(flags, SIP_DTMF);
		if (!strcasecmp(v->value, "inband"))
			ast_set_flag(flags, SIP_DTMF_INBAND);
		else if (!strcasecmp(v->value, "rfc2833"))
			ast_set_flag(flags, SIP_DTMF_RFC2833);
		else if (!strcasecmp(v->value, "info"))
			ast_set_flag(flags, SIP_DTMF_INFO);
		else if (!strcasecmp(v->value, "auto"))
			ast_set_flag(flags, SIP_DTMF_AUTO);
		else {
			ast_log(LOG_WARNING, "Unknown dtmf mode '%s' on line %d, using rfc2833\n", v->value, v->lineno);
			ast_set_flag(flags, SIP_DTMF_RFC2833);
		}
	} else if (!strcasecmp(v->name, "nat")) {
		ast_set_flag(mask, SIP_NAT);
		ast_clear_flag(flags, SIP_NAT);
		if (!strcasecmp(v->value, "never"))
			ast_set_flag(flags, SIP_NAT_NEVER);
		else if (!strcasecmp(v->value, "route"))
			ast_set_flag(flags, SIP_NAT_ROUTE);
		else if (ast_true(v->value))
			ast_set_flag(flags, SIP_NAT_ALWAYS);
		else
			ast_set_flag(flags, SIP_NAT_RFC3581);
	} else if (!strcasecmp(v->name, "canreinvite")) {
		ast_set_flag(mask, SIP_REINVITE);
		ast_clear_flag(flags, SIP_REINVITE);
		if (!strcasecmp(v->value, "update"))
			ast_set_flag(flags, SIP_REINVITE_UPDATE | SIP_CAN_REINVITE);
		else
			ast_set2_flag(flags, ast_true(v->value), SIP_CAN_REINVITE);
	} else if (!strcasecmp(v->name, "insecure")) {
		ast_set_flag(mask, SIP_INSECURE_PORT | SIP_INSECURE_INVITE);
		ast_clear_flag(flags, SIP_INSECURE_PORT | SIP_INSECURE_INVITE);
		if (!strcasecmp(v->value, "very"))
			ast_set_flag(flags, SIP_INSECURE_PORT | SIP_INSECURE_INVITE);
		else if (ast_true(v->value))
			ast_set_flag(flags, SIP_INSECURE_PORT);
		else if (!ast_false(v->value)) {
			char buf[64];
			char *word, *next;

			ast_copy_string(buf, v->value, sizeof(buf));
			next = buf;
			while ((word = strsep(&next, ","))) {
				if (!strcasecmp(word, "port"))
					ast_set_flag(flags, SIP_INSECURE_PORT);
				else if (!strcasecmp(word, "invite"))
					ast_set_flag(flags, SIP_INSECURE_INVITE);
				else
					ast_log(LOG_WARNING, "Unknown insecure mode '%s' on line %d\n", v->value, v->lineno);
			}
		}
	} else if (!strcasecmp(v->name, "progressinband")) {
		ast_set_flag(mask, SIP_PROG_INBAND);
		ast_clear_flag(flags, SIP_PROG_INBAND);
		if (ast_true(v->value))
			ast_set_flag(flags, SIP_PROG_INBAND_YES);
		else if (strcasecmp(v->value, "never"))
			ast_set_flag(flags, SIP_PROG_INBAND_NO);
  	} else if (!strcasecmp(v->name, "allowguest")) {
#ifdef OSP_SUPPORT
  		if (!strcasecmp(v->value, "osp"))
			global_allowguest = 2;
		else 
#endif
			if (ast_true(v->value)) 
				global_allowguest = 1;
			else
				global_allowguest = 0;
#ifdef OSP_SUPPORT
	} else if (!strcasecmp(v->name, "ospauth")) {
		ast_set_flag(mask, SIP_OSPAUTH);
		ast_clear_flag(flags, SIP_OSPAUTH);
		if (!strcasecmp(v->value, "proxy"))
			ast_set_flag(flags, SIP_OSPAUTH_PROXY);
		else if (!strcasecmp(v->value, "gateway"))
			ast_set_flag(flags, SIP_OSPAUTH_GATEWAY);
		else if(!strcasecmp (v->value, "exclusive"))
 			ast_set_flag(flags, SIP_OSPAUTH_EXCLUSIVE);
#endif
	} else if (!strcasecmp(v->name, "promiscredir")) {
		ast_set_flag(mask, SIP_PROMISCREDIR);
		ast_set2_flag(flags, ast_true(v->value), SIP_PROMISCREDIR);
		res = 1;
	}

	return res;
}

/*! \brief  add_sip_domain: Add SIP domain to list of domains we are responsible for */
static int add_sip_domain(const char *domain, const enum domain_mode mode, const char *context)
{
	struct domain *d;

	if (ast_strlen_zero(domain)) {
		ast_log(LOG_WARNING, "Zero length domain.\n");
		return 1;
	}

	if (!(d = ast_calloc(1, sizeof(*d))))
		return 0;

	ast_copy_string(d->domain, domain, sizeof(d->domain));

	if (!ast_strlen_zero(context))
		ast_copy_string(d->context, context, sizeof(d->context));

	d->mode = mode;

	AST_LIST_LOCK(&domain_list);
	AST_LIST_INSERT_TAIL(&domain_list, d, list);
	AST_LIST_UNLOCK(&domain_list);

 	if (sipdebug)	
		ast_log(LOG_DEBUG, "Added local SIP domain '%s'\n", domain);

	return 1;
}

/*! \brief  check_sip_domain: Check if domain part of uri is local to our server */
static int check_sip_domain(const char *domain, char *context, size_t len)
{
	struct domain *d;
	int result = 0;

	AST_LIST_LOCK(&domain_list);
	AST_LIST_TRAVERSE(&domain_list, d, list) {
		if (strcasecmp(d->domain, domain))
			continue;

		if (len && !ast_strlen_zero(d->context))
			ast_copy_string(context, d->context, len);
		
		result = 1;
		break;
	}
	AST_LIST_UNLOCK(&domain_list);

	return result;
}

/*! \brief  clear_sip_domains: Clear our domain list (at reload) */
static void clear_sip_domains(void)
{
	struct domain *d;

	AST_LIST_LOCK(&domain_list);
	while ((d = AST_LIST_REMOVE_HEAD(&domain_list, list)))
		free(d);
	AST_LIST_UNLOCK(&domain_list);
}


/*! \brief  add_realm_authentication: Add realm authentication in list */
static struct sip_auth *add_realm_authentication(struct sip_auth *authlist, char *configuration, int lineno)
{
	char authcopy[256];
	char *username=NULL, *realm=NULL, *secret=NULL, *md5secret=NULL;
	char *stringp;
	struct sip_auth *auth;
	struct sip_auth *b = NULL, *a = authlist;

	if (ast_strlen_zero(configuration))
		return authlist;

	ast_log(LOG_DEBUG, "Auth config ::  %s\n", configuration);

	ast_copy_string(authcopy, configuration, sizeof(authcopy));
	stringp = authcopy;

	username = stringp;
	realm = strrchr(stringp, '@');
	if (realm) {
		*realm = '\0';
		realm++;
	}
	if (ast_strlen_zero(username) || ast_strlen_zero(realm)) {
		ast_log(LOG_WARNING, "Format for authentication entry is user[:secret]@realm at line %d\n", lineno);
		return authlist;
	}
	stringp = username;
	username = strsep(&stringp, ":");
	if (username) {
		secret = strsep(&stringp, ":");
		if (!secret) {
			stringp = username;
			md5secret = strsep(&stringp,"#");
		}
	}
	if (!(auth = ast_calloc(1, sizeof(*auth))))
		return authlist;

	ast_copy_string(auth->realm, realm, sizeof(auth->realm));
	ast_copy_string(auth->username, username, sizeof(auth->username));
	if (secret)
		ast_copy_string(auth->secret, secret, sizeof(auth->secret));
	if (md5secret)
		ast_copy_string(auth->md5secret, md5secret, sizeof(auth->md5secret));

	/* Add authentication to authl */
	if (!authlist) {	/* No existing list */
		return auth;
	} 
	while(a) {
		b = a;
		a = a->next;
	}
	b->next = auth;	/* Add structure add end of list */

	if (option_verbose > 2)
		ast_verbose("Added authentication for realm %s\n", realm);

	return authlist;

}

/*! \brief  clear_realm_authentication: Clear realm authentication list (at reload) */
static int clear_realm_authentication(struct sip_auth *authlist)
{
	struct sip_auth *a = authlist;
	struct sip_auth *b;

	while (a) {
		b = a;
		a = a->next;
		free(b);
	}

	return 1;
}

/*! \brief  find_realm_authentication: Find authentication for a specific realm */
static struct sip_auth *find_realm_authentication(struct sip_auth *authlist, const char *realm)
{
	struct sip_auth *a;

	for (a = authlist; a; a = a->next) {
		if (!strcasecmp(a->realm, realm))
			break;
	}
	
	return a;
}

/*! \brief  build_user: Initiate a SIP user structure from sip.conf */
static struct sip_user *build_user(const char *name, struct ast_variable *v, int realtime)
{
	struct sip_user *user;
	int format;
	struct ast_ha *oldha = NULL;
	char *varname = NULL, *varval = NULL;
	struct ast_variable *tmpvar = NULL;
	struct ast_flags userflags = {(0)};
	struct ast_flags mask = {(0)};


	if (!(user = ast_calloc(1, sizeof(*user))))
		return NULL;
		
	suserobjs++;
	ASTOBJ_INIT(user);
	ast_copy_string(user->name, name, sizeof(user->name));
	oldha = user->ha;
	user->ha = NULL;
	ast_copy_flags(user, &global_flags, SIP_FLAGS_TO_COPY);
	user->capability = global_capability;
	user->prefs = prefs;
	/* set default context */
	strcpy(user->context, default_context);
	strcpy(user->language, default_language);
	strcpy(user->musicclass, default_musicclass);
	for (; v; v = v->next) {
		if (handle_common_options(&userflags, &mask, v))
			continue;

		if (!strcasecmp(v->name, "context")) {
			ast_copy_string(user->context, v->value, sizeof(user->context));
		} else if (!strcasecmp(v->name, "subscribecontext")) {
			ast_copy_string(user->subscribecontext, v->value, sizeof(user->subscribecontext));
		} else if (!strcasecmp(v->name, "setvar")) {
			varname = ast_strdupa(v->value);
			if (varname && (varval = strchr(varname,'='))) {
				*varval = '\0';
				varval++;
				if ((tmpvar = ast_variable_new(varname, varval))) {
					tmpvar->next = user->chanvars;
					user->chanvars = tmpvar;
				}
			}
		} else if (!strcasecmp(v->name, "permit") ||
				   !strcasecmp(v->name, "deny")) {
			user->ha = ast_append_ha(v->name, v->value, user->ha);
		} else if (!strcasecmp(v->name, "secret")) {
			ast_copy_string(user->secret, v->value, sizeof(user->secret)); 
		} else if (!strcasecmp(v->name, "md5secret")) {
			ast_copy_string(user->md5secret, v->value, sizeof(user->md5secret));
		} else if (!strcasecmp(v->name, "callerid")) {
			ast_callerid_split(v->value, user->cid_name, sizeof(user->cid_name), user->cid_num, sizeof(user->cid_num));
		} else if (!strcasecmp(v->name, "callgroup")) {
			user->callgroup = ast_get_group(v->value);
		} else if (!strcasecmp(v->name, "pickupgroup")) {
			user->pickupgroup = ast_get_group(v->value);
		} else if (!strcasecmp(v->name, "language")) {
			ast_copy_string(user->language, v->value, sizeof(user->language));
		} else if (!strcasecmp(v->name, "musicclass") || !strcasecmp(v->name, "musiconhold")) {
			ast_copy_string(user->musicclass, v->value, sizeof(user->musicclass));
		} else if (!strcasecmp(v->name, "accountcode")) {
			ast_copy_string(user->accountcode, v->value, sizeof(user->accountcode));
		} else if (!strcasecmp(v->name, "call-limit")) {
			user->call_limit = atoi(v->value);
			if (user->call_limit < 0)
				user->call_limit = 0;
		} else if (!strcasecmp(v->name, "amaflags")) {
			format = ast_cdr_amaflags2int(v->value);
			if (format < 0) {
				ast_log(LOG_WARNING, "Invalid AMA Flags: %s at line %d\n", v->value, v->lineno);
			} else {
				user->amaflags = format;
			}
		} else if (!strcasecmp(v->name, "allow")) {
			ast_parse_allow_disallow(&user->prefs, &user->capability, v->value, 1);
		} else if (!strcasecmp(v->name, "disallow")) {
			ast_parse_allow_disallow(&user->prefs, &user->capability, v->value, 0);
		} else if (!strcasecmp(v->name, "callingpres")) {
			user->callingpres = ast_parse_caller_presentation(v->value);
			if (user->callingpres == -1)
				user->callingpres = atoi(v->value);
		}
	}
	ast_copy_flags(user, &userflags, mask.flags);
	ast_free_ha(oldha);
	return user;
}

/*! \brief  temp_peer: Create temporary peer (used in autocreatepeer mode) */
static struct sip_peer *temp_peer(const char *name)
{
	struct sip_peer *peer;

	if (!(peer = ast_calloc(1, sizeof(*peer))))
		return NULL;

	apeerobjs++;
	ASTOBJ_INIT(peer);

	peer->expire = -1;
	peer->pokeexpire = -1;
	ast_copy_string(peer->name, name, sizeof(peer->name));
	ast_copy_flags(peer, &global_flags, SIP_FLAGS_TO_COPY);
	strcpy(peer->context, default_context);
	strcpy(peer->subscribecontext, default_subscribecontext);
	strcpy(peer->language, default_language);
	strcpy(peer->musicclass, default_musicclass);
	peer->addr.sin_port = htons(DEFAULT_SIP_PORT);
	peer->addr.sin_family = AF_INET;
	peer->capability = global_capability;
	peer->rtptimeout = global_rtptimeout;
	peer->rtpholdtimeout = global_rtpholdtimeout;
	peer->rtpkeepalive = global_rtpkeepalive;
	ast_set_flag(peer, SIP_SELFDESTRUCT);
	ast_set_flag(peer, SIP_DYNAMIC);
	peer->prefs = prefs;
	reg_source_db(peer);

	return peer;
}

/*! \brief  build_peer: Build peer from config file */
static struct sip_peer *build_peer(const char *name, struct ast_variable *v, int realtime)
{
	struct sip_peer *peer = NULL;
	struct ast_ha *oldha = NULL;
	int obproxyfound=0;
	int found=0;
	int format=0;		/* Ama flags */
	time_t regseconds;
	char *varname = NULL, *varval = NULL;
	struct ast_variable *tmpvar = NULL;
	struct ast_flags peerflags = {(0)};
	struct ast_flags mask = {(0)};


	if (!realtime)
		/* Note we do NOT use find_peer here, to avoid realtime recursion */
		/* We also use a case-sensitive comparison (unlike find_peer) so
		   that case changes made to the peer name will be properly handled
		   during reload
		*/
		peer = ASTOBJ_CONTAINER_FIND_UNLINK_FULL(&peerl, name, name, 0, 0, strcmp);

	if (peer) {
		/* Already in the list, remove it and it will be added back (or FREE'd)  */
		found++;
 	} else {
		if (!(peer = ast_calloc(1, sizeof(*peer))))
			return NULL;

		if (realtime)
			rpeerobjs++;
		else
			speerobjs++;
		ASTOBJ_INIT(peer);
		peer->expire = -1;
		peer->pokeexpire = -1;
	}
	/* Note that our peer HAS had its reference count incrased */

	peer->lastmsgssent = -1;
	if (!found) {
		if (name)
			ast_copy_string(peer->name, name, sizeof(peer->name));
		peer->addr.sin_port = htons(DEFAULT_SIP_PORT);
		peer->addr.sin_family = AF_INET;
		peer->defaddr.sin_family = AF_INET;
	}
	/* If we have channel variables, remove them (reload) */
	if (peer->chanvars) {
		ast_variables_destroy(peer->chanvars);
		peer->chanvars = NULL;
	}
	strcpy(peer->context, default_context);
	strcpy(peer->subscribecontext, default_subscribecontext);
	strcpy(peer->vmexten, default_vmexten);
	strcpy(peer->language, default_language);
	strcpy(peer->musicclass, default_musicclass);
	ast_copy_flags(peer, &global_flags, SIP_USEREQPHONE);
	peer->secret[0] = '\0';
	peer->md5secret[0] = '\0';
	peer->cid_num[0] = '\0';
	peer->cid_name[0] = '\0';
	peer->fromdomain[0] = '\0';
	peer->fromuser[0] = '\0';
	peer->regexten[0] = '\0';
	peer->mailbox[0] = '\0';
	peer->callgroup = 0;
	peer->pickupgroup = 0;
	peer->rtpkeepalive = global_rtpkeepalive;
	peer->maxms = default_qualify;
	peer->prefs = prefs;
	oldha = peer->ha;
	peer->ha = NULL;
	peer->addr.sin_family = AF_INET;
	ast_copy_flags(peer, &global_flags, SIP_FLAGS_TO_COPY);
	peer->capability = global_capability;
	peer->rtptimeout = global_rtptimeout;
	peer->rtpholdtimeout = global_rtpholdtimeout;
	while(v) {
		if (handle_common_options(&peerflags, &mask, v)) {
			v = v->next;
			continue;
		}

		if (realtime && !strcasecmp(v->name, "regseconds")) {
			if (sscanf(v->value, "%ld", (time_t *)&regseconds) != 1)
				regseconds = 0;
		} else if (realtime && !strcasecmp(v->name, "ipaddr") && !ast_strlen_zero(v->value) ) {
			inet_aton(v->value, &(peer->addr.sin_addr));
		} else if (realtime && !strcasecmp(v->name, "name"))
			ast_copy_string(peer->name, v->value, sizeof(peer->name));
		else if (realtime && !strcasecmp(v->name, "fullcontact")) {
			ast_copy_string(peer->fullcontact, v->value, sizeof(peer->fullcontact));
			ast_set_flag((&peer->flags_page2), SIP_PAGE2_RT_FROMCONTACT);
		} else if (!strcasecmp(v->name, "secret")) 
			ast_copy_string(peer->secret, v->value, sizeof(peer->secret));
		else if (!strcasecmp(v->name, "md5secret")) 
			ast_copy_string(peer->md5secret, v->value, sizeof(peer->md5secret));
		else if (!strcasecmp(v->name, "auth"))
			peer->auth = add_realm_authentication(peer->auth, v->value, v->lineno);
		else if (!strcasecmp(v->name, "callerid")) {
			ast_callerid_split(v->value, peer->cid_name, sizeof(peer->cid_name), peer->cid_num, sizeof(peer->cid_num));
		} else if (!strcasecmp(v->name, "context")) {
			ast_copy_string(peer->context, v->value, sizeof(peer->context));
		} else if (!strcasecmp(v->name, "subscribecontext")) {
			ast_copy_string(peer->subscribecontext, v->value, sizeof(peer->subscribecontext));
		} else if (!strcasecmp(v->name, "fromdomain"))
			ast_copy_string(peer->fromdomain, v->value, sizeof(peer->fromdomain));
		else if (!strcasecmp(v->name, "usereqphone"))
			ast_set2_flag(peer, ast_true(v->value), SIP_USEREQPHONE);
		else if (!strcasecmp(v->name, "fromuser"))
			ast_copy_string(peer->fromuser, v->value, sizeof(peer->fromuser));
		else if (!strcasecmp(v->name, "host") || !strcasecmp(v->name, "outboundproxy")) {
			if (!strcasecmp(v->value, "dynamic")) {
				if (!strcasecmp(v->name, "outboundproxy") || obproxyfound) {
					ast_log(LOG_WARNING, "You can't have a dynamic outbound proxy, you big silly head at line %d.\n", v->lineno);
				} else {
					/* They'll register with us */
					ast_set_flag(peer, SIP_DYNAMIC);
					if (!found) {
						/* Initialize stuff iff we're not found, otherwise
						   we keep going with what we had */
						memset(&peer->addr.sin_addr, 0, 4);
						if (peer->addr.sin_port) {
							/* If we've already got a port, make it the default rather than absolute */
							peer->defaddr.sin_port = peer->addr.sin_port;
							peer->addr.sin_port = 0;
						}
					}
				}
			} else {
				/* Non-dynamic.  Make sure we become that way if we're not */
				if (peer->expire > -1)
					ast_sched_del(sched, peer->expire);
				peer->expire = -1;
				ast_clear_flag(peer, SIP_DYNAMIC);	
				if (!obproxyfound || !strcasecmp(v->name, "outboundproxy")) {
					if (ast_get_ip_or_srv(&peer->addr, v->value, "_sip._udp")) {
						ASTOBJ_UNREF(peer, sip_destroy_peer);
						return NULL;
					}
				}
				if (!strcasecmp(v->name, "outboundproxy"))
					obproxyfound=1;
				else {
					ast_copy_string(peer->tohost, v->value, sizeof(peer->tohost));
					if (!peer->addr.sin_port)
						peer->addr.sin_port = htons(DEFAULT_SIP_PORT);
				}
			}
		} else if (!strcasecmp(v->name, "defaultip")) {
			if (ast_get_ip(&peer->defaddr, v->value)) {
				ASTOBJ_UNREF(peer, sip_destroy_peer);
				return NULL;
			}
		} else if (!strcasecmp(v->name, "permit") || !strcasecmp(v->name, "deny")) {
			peer->ha = ast_append_ha(v->name, v->value, peer->ha);
		} else if (!strcasecmp(v->name, "port")) {
			if (!realtime && ast_test_flag(peer, SIP_DYNAMIC))
				peer->defaddr.sin_port = htons(atoi(v->value));
			else
				peer->addr.sin_port = htons(atoi(v->value));
		} else if (!strcasecmp(v->name, "callingpres")) {
			peer->callingpres = ast_parse_caller_presentation(v->value);
			if (peer->callingpres == -1)
				peer->callingpres = atoi(v->value);
		} else if (!strcasecmp(v->name, "username")) {
			ast_copy_string(peer->username, v->value, sizeof(peer->username));
		} else if (!strcasecmp(v->name, "language")) {
			ast_copy_string(peer->language, v->value, sizeof(peer->language));
		} else if (!strcasecmp(v->name, "regexten")) {
			ast_copy_string(peer->regexten, v->value, sizeof(peer->regexten));
		} else if (!strcasecmp(v->name, "call-limit") || !strcasecmp(v->name, "incominglimit")) {
			peer->call_limit = atoi(v->value);
			if (peer->call_limit < 0)
				peer->call_limit = 0;
		} else if (!strcasecmp(v->name, "amaflags")) {
			format = ast_cdr_amaflags2int(v->value);
			if (format < 0) {
				ast_log(LOG_WARNING, "Invalid AMA Flags for peer: %s at line %d\n", v->value, v->lineno);
			} else {
				peer->amaflags = format;
			}
		} else if (!strcasecmp(v->name, "accountcode")) {
			ast_copy_string(peer->accountcode, v->value, sizeof(peer->accountcode));
		} else if (!strcasecmp(v->name, "musicclass") || !strcasecmp(v->name, "musiconhold")) {
			ast_copy_string(peer->musicclass, v->value, sizeof(peer->musicclass));
		} else if (!strcasecmp(v->name, "mailbox")) {
			ast_copy_string(peer->mailbox, v->value, sizeof(peer->mailbox));
		} else if (!strcasecmp(v->name, "vmexten")) {
			ast_copy_string(peer->vmexten, v->value, sizeof(peer->vmexten));
		} else if (!strcasecmp(v->name, "callgroup")) {
			peer->callgroup = ast_get_group(v->value);
		} else if (!strcasecmp(v->name, "pickupgroup")) {
			peer->pickupgroup = ast_get_group(v->value);
		} else if (!strcasecmp(v->name, "allow")) {
			ast_parse_allow_disallow(&peer->prefs, &peer->capability, v->value, 1);
		} else if (!strcasecmp(v->name, "disallow")) {
			ast_parse_allow_disallow(&peer->prefs, &peer->capability, v->value, 0);
		} else if (!strcasecmp(v->name, "rtptimeout")) {
			if ((sscanf(v->value, "%d", &peer->rtptimeout) != 1) || (peer->rtptimeout < 0)) {
				ast_log(LOG_WARNING, "'%s' is not a valid RTP hold time at line %d.  Using default.\n", v->value, v->lineno);
				peer->rtptimeout = global_rtptimeout;
			}
		} else if (!strcasecmp(v->name, "rtpholdtimeout")) {
			if ((sscanf(v->value, "%d", &peer->rtpholdtimeout) != 1) || (peer->rtpholdtimeout < 0)) {
				ast_log(LOG_WARNING, "'%s' is not a valid RTP hold time at line %d.  Using default.\n", v->value, v->lineno);
				peer->rtpholdtimeout = global_rtpholdtimeout;
			}
		} else if (!strcasecmp(v->name, "rtpkeepalive")) {
			if ((sscanf(v->value, "%d", &peer->rtpkeepalive) != 1) || (peer->rtpkeepalive < 0)) {
				ast_log(LOG_WARNING, "'%s' is not a valid RTP keepalive time at line %d.  Using default.\n", v->value, v->lineno);
				peer->rtpkeepalive = global_rtpkeepalive;
			}
		} else if (!strcasecmp(v->name, "setvar")) {
			/* Set peer channel variable */
			varname = ast_strdupa(v->value);
			if (varname && (varval = strchr(varname,'='))) {
				*varval = '\0';
				varval++;
				if ((tmpvar = ast_variable_new(varname, varval))) {
					tmpvar->next = peer->chanvars;
					peer->chanvars = tmpvar;
				}
			}
		} else if (!strcasecmp(v->name, "qualify")) {
			if (!strcasecmp(v->value, "no")) {
				peer->maxms = 0;
			} else if (!strcasecmp(v->value, "yes")) {
				peer->maxms = DEFAULT_MAXMS;
			} else if (sscanf(v->value, "%d", &peer->maxms) != 1) {
				ast_log(LOG_WARNING, "Qualification of peer '%s' should be 'yes', 'no', or a number of milliseconds at line %d of sip.conf\n", peer->name, v->lineno);
				peer->maxms = 0;
			}
		}
		/* else if (strcasecmp(v->name,"type"))
		 *	ast_log(LOG_WARNING, "Ignoring %s\n", v->name);
		 */
		v=v->next;
	}
	if (!ast_test_flag((&global_flags_page2), SIP_PAGE2_IGNOREREGEXPIRE) && ast_test_flag(peer, SIP_DYNAMIC) && realtime) {
		time_t nowtime;

		time(&nowtime);
		if ((nowtime - regseconds) > 0) {
			destroy_association(peer);
			memset(&peer->addr, 0, sizeof(peer->addr));
			if (option_debug)
				ast_log(LOG_DEBUG, "Bah, we're expired (%d/%d/%d)!\n", (int)(nowtime - regseconds), (int)regseconds, (int)nowtime);
		}
	}
	ast_copy_flags(peer, &peerflags, mask.flags);
	if (!found && ast_test_flag(peer, SIP_DYNAMIC) && !ast_test_flag(peer, SIP_REALTIME))
		reg_source_db(peer);
	ASTOBJ_UNMARK(peer);
	ast_free_ha(oldha);
	return peer;
}

/*! \brief  reload_config: Re-read SIP.conf config file */
/*	This function reloads all config data, except for
	active peers (with registrations). They will only
	change configuration data at restart, not at reload.
	SIP debug and recordhistory state will not change
 */
static int reload_config(enum channelreloadreason reason)
{
	struct ast_config *cfg;
	struct ast_variable *v;
	struct sip_peer *peer;
	struct sip_user *user;
	struct ast_hostent ahp;
	char *cat;
	struct hostent *hp;
	int format;
	char iabuf[INET_ADDRSTRLEN];
	struct ast_flags dummy;
	int auto_sip_domains = 0;
	struct sockaddr_in old_bindaddr = bindaddr;
	int registry_count = 0, peer_count = 0, user_count = 0;

	cfg = ast_config_load(config);

	/* We *must* have a config file otherwise stop immediately */
	if (!cfg) {
		ast_log(LOG_NOTICE, "Unable to load config %s\n", config);
		return -1;
	}
	
	/* Clear all flags before setting default values */
	ast_clear_flag(&global_flags, AST_FLAGS_ALL);

	/* Reset IP addresses  */
	memset(&bindaddr, 0, sizeof(bindaddr));
	memset(&localaddr, 0, sizeof(localaddr));
	memset(&externip, 0, sizeof(externip));
	memset(&prefs, 0 , sizeof(prefs));
	outboundproxyip.sin_port = htons(DEFAULT_SIP_PORT);
	outboundproxyip.sin_family = AF_INET;	/* Type of address: IPv4 */
	ourport = DEFAULT_SIP_PORT;
	srvlookup = DEFAULT_SRVLOOKUP;
	global_tos = DEFAULT_TOS;
	externhost[0] = '\0';			/* External host name (for behind NAT DynDNS support) */
	externexpire = 0;			/* Expiration for DNS re-issuing */
	externrefresh = 10;
	memset(&outboundproxyip, 0, sizeof(outboundproxyip));

	/* Reset channel settings to default before re-configuring */
	allow_external_domains = DEFAULT_ALLOW_EXT_DOM;				/* Allow external invites */
	regcontext[0] = '\0';
	expiry = DEFAULT_EXPIRY;
	global_notifyringing = DEFAULT_NOTIFYRINGING;
	ast_copy_string(global_useragent, DEFAULT_USERAGENT, sizeof(global_useragent));
	ast_copy_string(default_notifymime, DEFAULT_NOTIFYMIME, sizeof(default_notifymime));
	ast_copy_string(global_realm, DEFAULT_REALM, sizeof(global_realm));
	ast_copy_string(default_callerid, DEFAULT_CALLERID, sizeof(default_callerid));
	global_videosupport = DEFAULT_VIDEOSUPPORT;
	compactheaders = DEFAULT_COMPACTHEADERS;
	global_reg_timeout = DEFAULT_REGISTRATION_TIMEOUT;
	global_regattempts_max = 0;
	pedanticsipchecking = DEFAULT_PEDANTIC;
	global_mwitime = DEFAULT_MWITIME;
	autocreatepeer = DEFAULT_AUTOCREATEPEER;
	global_allowguest = DEFAULT_ALLOWGUEST;
	global_rtptimeout = 0;
	global_rtpholdtimeout = 0;
	global_rtpkeepalive = 0;
	ast_set_flag(&global_flags_page2, SIP_PAGE2_RTUPDATE);

	/* Initialize some reasonable defaults at SIP reload (used both for channel and as default for peers and users */
	ast_copy_string(default_context, DEFAULT_CONTEXT, sizeof(default_context));
	default_subscribecontext[0] = '\0';
	default_language[0] = '\0';
	default_fromdomain[0] = '\0';
	default_qualify = DEFAULT_QUALIFY;
	ast_copy_string(default_musicclass, DEFAULT_MUSICCLASS, sizeof(default_musicclass));
	ast_copy_string(default_vmexten, DEFAULT_VMEXTEN, sizeof(default_vmexten));
	ast_set_flag(&global_flags, SIP_DTMF_RFC2833);			/*!< Default DTMF setting: RFC2833 */
	ast_set_flag(&global_flags, SIP_NAT_RFC3581);			/*!< NAT support if requested by device with rport */
	ast_set_flag(&global_flags, SIP_CAN_REINVITE);			/*!< Allow re-invites */

	/* Debugging settings, always default to off */
	dumphistory = FALSE;
	recordhistory = FALSE;
	ast_clear_flag(&global_flags_page2, SIP_PAGE2_DEBUG_CONFIG);

	/* Misc settings for the channel */
	relaxdtmf = 0;
	callevents = 0;

	/* Read the [general] config section of sip.conf (or from realtime config) */
	for (v = ast_variable_browse(cfg, "general"); v; v = v->next) {
		if (handle_common_options(&global_flags, &dummy, v))
			continue;

		/* Create the interface list */
		if (!strcasecmp(v->name, "context")) {
			ast_copy_string(default_context, v->value, sizeof(default_context));
		} else if (!strcasecmp(v->name, "realm")) {
			ast_copy_string(global_realm, v->value, sizeof(global_realm));
		} else if (!strcasecmp(v->name, "useragent")) {
			ast_copy_string(global_useragent, v->value, sizeof(global_useragent));
			ast_log(LOG_DEBUG, "Setting SIP channel User-Agent Name to %s\n", global_useragent);
		} else if (!strcasecmp(v->name, "rtcachefriends")) {
			ast_set2_flag((&global_flags_page2), ast_true(v->value), SIP_PAGE2_RTCACHEFRIENDS);	
		} else if (!strcasecmp(v->name, "rtupdate")) {
			ast_set2_flag((&global_flags_page2), ast_true(v->value), SIP_PAGE2_RTUPDATE);	
		} else if (!strcasecmp(v->name, "ignoreregexpire")) {
			ast_set2_flag((&global_flags_page2), ast_true(v->value), SIP_PAGE2_IGNOREREGEXPIRE);	
		} else if (!strcasecmp(v->name, "rtautoclear")) {
			int i = atoi(v->value);
			if (i > 0)
				global_rtautoclear = i;
			else
				i = 0;
			ast_set2_flag((&global_flags_page2), i || ast_true(v->value), SIP_PAGE2_RTAUTOCLEAR);
		} else if (!strcasecmp(v->name, "usereqphone")) {
			ast_set2_flag((&global_flags), ast_true(v->value), SIP_USEREQPHONE);	
		} else if (!strcasecmp(v->name, "relaxdtmf")) {
			relaxdtmf = ast_true(v->value);
		} else if (!strcasecmp(v->name, "checkmwi")) {
			if ((sscanf(v->value, "%d", &global_mwitime) != 1) || (global_mwitime < 0)) {
				ast_log(LOG_WARNING, "'%s' is not a valid MWI time setting at line %d.  Using default (10).\n", v->value, v->lineno);
				global_mwitime = DEFAULT_MWITIME;
			}
		} else if (!strcasecmp(v->name, "vmexten")) {
			ast_copy_string(default_vmexten, v->value, sizeof(default_vmexten));
		} else if (!strcasecmp(v->name, "rtptimeout")) {
			if ((sscanf(v->value, "%d", &global_rtptimeout) != 1) || (global_rtptimeout < 0)) {
				ast_log(LOG_WARNING, "'%s' is not a valid RTP hold time at line %d.  Using default.\n", v->value, v->lineno);
				global_rtptimeout = 0;
			}
		} else if (!strcasecmp(v->name, "rtpholdtimeout")) {
			if ((sscanf(v->value, "%d", &global_rtpholdtimeout) != 1) || (global_rtpholdtimeout < 0)) {
				ast_log(LOG_WARNING, "'%s' is not a valid RTP hold time at line %d.  Using default.\n", v->value, v->lineno);
				global_rtpholdtimeout = 0;
			}
		} else if (!strcasecmp(v->name, "rtpkeepalive")) {
			if ((sscanf(v->value, "%d", &global_rtpkeepalive) != 1) || (global_rtpkeepalive < 0)) {
				ast_log(LOG_WARNING, "'%s' is not a valid RTP keepalive time at line %d.  Using default.\n", v->value, v->lineno);
				global_rtpkeepalive = 0;
			}
		} else if (!strcasecmp(v->name, "videosupport")) {
			global_videosupport = ast_true(v->value);
		} else if (!strcasecmp(v->name, "compactheaders")) {
			compactheaders = ast_true(v->value);
		} else if (!strcasecmp(v->name, "notifymimetype")) {
			ast_copy_string(default_notifymime, v->value, sizeof(default_notifymime));
		} else if (!strcasecmp(v->name, "notifyringing")) {
			global_notifyringing = ast_true(v->value);
		} else if (!strcasecmp(v->name, "musicclass") || !strcasecmp(v->name, "musiconhold")) {
			ast_copy_string(default_musicclass, v->value, sizeof(default_musicclass));
		} else if (!strcasecmp(v->name, "language")) {
			ast_copy_string(default_language, v->value, sizeof(default_language));
		} else if (!strcasecmp(v->name, "regcontext")) {
			ast_copy_string(regcontext, v->value, sizeof(regcontext));
			/* Create context if it doesn't exist already */
			if (!ast_context_find(regcontext))
				ast_context_create(NULL, regcontext, channeltype);
		} else if (!strcasecmp(v->name, "callerid")) {
			ast_copy_string(default_callerid, v->value, sizeof(default_callerid));
		} else if (!strcasecmp(v->name, "fromdomain")) {
			ast_copy_string(default_fromdomain, v->value, sizeof(default_fromdomain));
		} else if (!strcasecmp(v->name, "outboundproxy")) {
			if (ast_get_ip_or_srv(&outboundproxyip, v->value, "_sip._udp") < 0)
				ast_log(LOG_WARNING, "Unable to locate host '%s'\n", v->value);
		} else if (!strcasecmp(v->name, "outboundproxyport")) {
			/* Port needs to be after IP */
			sscanf(v->value, "%d", &format);
			outboundproxyip.sin_port = htons(format);
		} else if (!strcasecmp(v->name, "autocreatepeer")) {
			autocreatepeer = ast_true(v->value);
		} else if (!strcasecmp(v->name, "srvlookup")) {
			srvlookup = ast_true(v->value);
		} else if (!strcasecmp(v->name, "pedantic")) {
			pedanticsipchecking = ast_true(v->value);
		} else if (!strcasecmp(v->name, "maxexpirey") || !strcasecmp(v->name, "maxexpiry")) {
			max_expiry = atoi(v->value);
			if (max_expiry < 1)
				max_expiry = DEFAULT_MAX_EXPIRY;
		} else if (!strcasecmp(v->name, "minexpirey") || !strcasecmp(v->name, "minexpiry")) {
			min_expiry = atoi(v->value);
			if (min_expiry < 1)
				min_expiry = DEFAULT_MIN_EXPIRY;
		} else if (!strcasecmp(v->name, "defaultexpiry") || !strcasecmp(v->name, "defaultexpirey")) {
			default_expiry = atoi(v->value);
			if (default_expiry < 1)
				default_expiry = DEFAULT_DEFAULT_EXPIRY;
		} else if (!strcasecmp(v->name, "sipdebug")) {
			if (ast_true(v->value))
				ast_set_flag(&global_flags_page2, SIP_PAGE2_DEBUG_CONFIG);
		} else if (!strcasecmp(v->name, "dumphistory")) {
			dumphistory = ast_true(v->value);
		} else if (!strcasecmp(v->name, "recordhistory")) {
			recordhistory = ast_true(v->value);
		} else if (!strcasecmp(v->name, "registertimeout")) {
			global_reg_timeout = atoi(v->value);
			if (global_reg_timeout < 1)
				global_reg_timeout = DEFAULT_REGISTRATION_TIMEOUT;
		} else if (!strcasecmp(v->name, "registerattempts")) {
			global_regattempts_max = atoi(v->value);
		} else if (!strcasecmp(v->name, "bindaddr")) {
			if (!(hp = ast_gethostbyname(v->value, &ahp))) {
				ast_log(LOG_WARNING, "Invalid address: %s\n", v->value);
			} else {
				memcpy(&bindaddr.sin_addr, hp->h_addr, sizeof(bindaddr.sin_addr));
			}
		} else if (!strcasecmp(v->name, "localnet")) {
			struct ast_ha *na;
			if (!(na = ast_append_ha("d", v->value, localaddr)))
				ast_log(LOG_WARNING, "Invalid localnet value: %s\n", v->value);
			else
				localaddr = na;
		} else if (!strcasecmp(v->name, "localmask")) {
			ast_log(LOG_WARNING, "Use of localmask is no long supported -- use localnet with mask syntax\n");
		} else if (!strcasecmp(v->name, "externip")) {
			if (!(hp = ast_gethostbyname(v->value, &ahp))) 
				ast_log(LOG_WARNING, "Invalid address for externip keyword: %s\n", v->value);
			else
				memcpy(&externip.sin_addr, hp->h_addr, sizeof(externip.sin_addr));
			externexpire = 0;
		} else if (!strcasecmp(v->name, "externhost")) {
			ast_copy_string(externhost, v->value, sizeof(externhost));
			if (!(hp = ast_gethostbyname(externhost, &ahp))) 
				ast_log(LOG_WARNING, "Invalid address for externhost keyword: %s\n", externhost);
			else
				memcpy(&externip.sin_addr, hp->h_addr, sizeof(externip.sin_addr));
			time(&externexpire);
		} else if (!strcasecmp(v->name, "externrefresh")) {
			if (sscanf(v->value, "%d", &externrefresh) != 1) {
				ast_log(LOG_WARNING, "Invalid externrefresh value '%s', must be an integer >0 at line %d\n", v->value, v->lineno);
				externrefresh = 10;
			}
		} else if (!strcasecmp(v->name, "allow")) {
			ast_parse_allow_disallow(&prefs, &global_capability, v->value, 1);
		} else if (!strcasecmp(v->name, "disallow")) {
			ast_parse_allow_disallow(&prefs, &global_capability, v->value, 0);
		} else if (!strcasecmp(v->name, "allowexternaldomains")) {
			allow_external_domains = ast_true(v->value);
		} else if (!strcasecmp(v->name, "autodomain")) {
			auto_sip_domains = ast_true(v->value);
		} else if (!strcasecmp(v->name, "domain")) {
			char *domain = ast_strdupa(v->value);
			char *context = strchr(domain, ',');

			if (context)
				*context++ = '\0';

			if (ast_strlen_zero(domain))
				ast_log(LOG_WARNING, "Empty domain specified at line %d\n", v->lineno);
			else if (ast_strlen_zero(context))
				ast_log(LOG_WARNING, "Empty context specified at line %d for domain '%s'\n", v->lineno, domain);
			else
				add_sip_domain(ast_strip(domain), SIP_DOMAIN_CONFIG, context ? ast_strip(context) : "");
		} else if (!strcasecmp(v->name, "register")) {
			if (sip_register(v->value, v->lineno) == 0)
				registry_count++;
		} else if (!strcasecmp(v->name, "tos")) {
			if (ast_str2tos(v->value, &global_tos))
				ast_log(LOG_WARNING, "Invalid tos value at line %d, should be 'lowdelay', 'throughput', 'reliability', 'mincost', or 'none'\n", v->lineno);
		} else if (!strcasecmp(v->name, "bindport")) {
			if (sscanf(v->value, "%d", &ourport) == 1) {
				bindaddr.sin_port = htons(ourport);
			} else {
				ast_log(LOG_WARNING, "Invalid port number '%s' at line %d of %s\n", v->value, v->lineno, config);
			}
		} else if (!strcasecmp(v->name, "qualify")) {
			if (!strcasecmp(v->value, "no")) {
				default_qualify = 0;
			} else if (!strcasecmp(v->value, "yes")) {
				default_qualify = DEFAULT_MAXMS;
			} else if (sscanf(v->value, "%d", &default_qualify) != 1) {
				ast_log(LOG_WARNING, "Qualification default should be 'yes', 'no', or a number of milliseconds at line %d of sip.conf\n", v->lineno);
				default_qualify = 0;
			}
		} else if (!strcasecmp(v->name, "callevents")) {
			callevents = ast_true(v->value);
		}
	}

	if (!allow_external_domains && AST_LIST_EMPTY(&domain_list)) {
		ast_log(LOG_WARNING, "To disallow external domains, you need to configure local SIP domains.\n");
		allow_external_domains = 1;
	}
	
	/* Build list of authentication to various SIP realms, i.e. service providers */
 	v = ast_variable_browse(cfg, "authentication");
 	while(v) {
 		/* Format for authentication is auth = username:password@realm */
 		if (!strcasecmp(v->name, "auth")) {
 			authl = add_realm_authentication(authl, v->value, v->lineno);
 		}
 		v = v->next;
 	}
	
	/* Load peers, users and friends */
	cat = NULL;
	while ( (cat = ast_category_browse(cfg, cat)) ) {
		const char *utype;
		if (!strcasecmp(cat, "general") || !strcasecmp(cat, "authentication"))
			continue;
		utype = ast_variable_retrieve(cfg, cat, "type");
		if (!utype) {
			ast_log(LOG_WARNING, "Section '%s' lacks type\n", cat);
			continue;
		} else {
			int is_user = 0, is_peer = 0;
			if (!strcasecmp(utype, "user"))
				is_user = 1;
			else if (!strcasecmp(utype, "friend"))
				is_user = is_peer = 1;
			else if (!strcasecmp(utype, "peer"))
				is_peer = 1;
			else {
				ast_log(LOG_WARNING, "Unknown type '%s' for '%s' in %s\n", utype, cat, "sip.conf");
				continue;
			}
			if (is_user) {
				user = build_user(cat, ast_variable_browse(cfg, cat), 0);
				if (user) {
					ASTOBJ_CONTAINER_LINK(&userl,user);
					ASTOBJ_UNREF(user, sip_destroy_user);
					user_count++;
				}
			}
			if (is_peer) {
				peer = build_peer(cat, ast_variable_browse(cfg, cat), 0);
				if (peer) {
					ASTOBJ_CONTAINER_LINK(&peerl,peer);
					ASTOBJ_UNREF(peer, sip_destroy_peer);
					peer_count++;
				}
			}
		}
	}
	if (ast_find_ourip(&__ourip, bindaddr)) {
		ast_log(LOG_WARNING, "Unable to get own IP address, SIP disabled\n");
		return 0;
	}
	if (!ntohs(bindaddr.sin_port))
		bindaddr.sin_port = ntohs(DEFAULT_SIP_PORT);
	bindaddr.sin_family = AF_INET;
	ast_mutex_lock(&netlock);
	if ((sipsock > -1) && (memcmp(&old_bindaddr, &bindaddr, sizeof(struct sockaddr_in)))) {
		close(sipsock);
		sipsock = -1;
	}
	if (sipsock < 0) {
		sipsock = socket(AF_INET, SOCK_DGRAM, 0);
		if (sipsock < 0) {
			ast_log(LOG_WARNING, "Unable to create SIP socket: %s\n", strerror(errno));
		} else {
			/* Allow SIP clients on the same host to access us: */
			const int reuseFlag = 1;
			setsockopt(sipsock, SOL_SOCKET, SO_REUSEADDR,
				   (const char*)&reuseFlag,
				   sizeof reuseFlag);

			if (bind(sipsock, (struct sockaddr *)&bindaddr, sizeof(bindaddr)) < 0) {
				ast_log(LOG_WARNING, "Failed to bind to %s:%d: %s\n",
				ast_inet_ntoa(iabuf, sizeof(iabuf), bindaddr.sin_addr), ntohs(bindaddr.sin_port),
				strerror(errno));
				close(sipsock);
				sipsock = -1;
			} else {
				if (option_verbose > 1) { 
					ast_verbose(VERBOSE_PREFIX_2 "SIP Listening on %s:%d\n", 
					ast_inet_ntoa(iabuf, sizeof(iabuf), bindaddr.sin_addr), ntohs(bindaddr.sin_port));
					ast_verbose(VERBOSE_PREFIX_2 "Using TOS bits %d\n", global_tos);
				}
				if (setsockopt(sipsock, IPPROTO_IP, IP_TOS, &global_tos, sizeof(global_tos))) 
					ast_log(LOG_WARNING, "Unable to set TOS to %d\n", global_tos);
			}
		}
	}
	ast_mutex_unlock(&netlock);

	/* Add default domains - host name, IP address and IP:port */
	/* Only do this if user added any sip domain with "localdomains" */
	/* In order to *not* break backwards compatibility */
	/* 	Some phones address us at IP only, some with additional port number */
	if (auto_sip_domains) {
		char temp[MAXHOSTNAMELEN];

		/* First our default IP address */
		if (bindaddr.sin_addr.s_addr) {
			ast_inet_ntoa(temp, sizeof(temp), bindaddr.sin_addr);
			add_sip_domain(temp, SIP_DOMAIN_AUTO, NULL);
		} else {
			ast_log(LOG_NOTICE, "Can't add wildcard IP address to domain list, please add IP address to domain manually.\n");
		}

		/* Our extern IP address, if configured */
		if (externip.sin_addr.s_addr) {
			ast_inet_ntoa(temp, sizeof(temp), externip.sin_addr);
			add_sip_domain(temp, SIP_DOMAIN_AUTO, NULL);
		}

		/* Extern host name (NAT traversal support) */
		if (!ast_strlen_zero(externhost))
			add_sip_domain(externhost, SIP_DOMAIN_AUTO, NULL);
		
		/* Our host name */
		if (!gethostname(temp, sizeof(temp)))
			add_sip_domain(temp, SIP_DOMAIN_AUTO, NULL);
	}

	/* Release configuration from memory */
	ast_config_destroy(cfg);

	/* Load the list of manual NOTIFY types to support */
	if (notify_types)
		ast_config_destroy(notify_types);
	notify_types = ast_config_load(notify_config);

	/* Done, tell the manager */
	manager_event(EVENT_FLAG_SYSTEM, "ChannelReload", "Channel: SIP\r\nReloadReason: %s\r\nRegistry_Count: %d\r\nPeer_Count: %d\r\nUser_Count: %d\r\n\r\n", channelreloadreason2txt(reason), registry_count, peer_count, user_count);

	return 0;
}

/*! \brief  sip_get_rtp_peer: Returns null if we can't reinvite (part of RTP interface) */
static struct ast_rtp *sip_get_rtp_peer(struct ast_channel *chan)
{
	struct sip_pvt *p;
	struct ast_rtp *rtp = NULL;
	p = chan->tech_pvt;
	if (!p)
		return NULL;
	ast_mutex_lock(&p->lock);
	if (p->rtp && ast_test_flag(p, SIP_CAN_REINVITE))
		rtp =  p->rtp;
	ast_mutex_unlock(&p->lock);
	return rtp;
}

/*! \brief  sip_get_vrtp_peer: Returns null if we can't reinvite video (part of RTP interface) */
static struct ast_rtp *sip_get_vrtp_peer(struct ast_channel *chan)
{
	struct sip_pvt *p;
	struct ast_rtp *rtp = NULL;
	p = chan->tech_pvt;
	if (!p)
		return NULL;

	ast_mutex_lock(&p->lock);
	if (p->vrtp && ast_test_flag(p, SIP_CAN_REINVITE))
		rtp = p->vrtp;
	ast_mutex_unlock(&p->lock);
	return rtp;
}

/*! \brief  sip_set_rtp_peer: Set the RTP peer for this call */
static int sip_set_rtp_peer(struct ast_channel *chan, struct ast_rtp *rtp, struct ast_rtp *vrtp, int codecs, int nat_active)
{
	struct sip_pvt *p;

	p = chan->tech_pvt;
	if (!p) 
		return -1;
	ast_mutex_lock(&p->lock);
	if (ast_test_flag(p, SIP_ALREADYGONE)) {
		/* If we're destroyed, don't bother */
		ast_mutex_unlock(&p->lock);
		return 0;
	}
	if (rtp)
		ast_rtp_get_peer(rtp, &p->redirip);
	else
		memset(&p->redirip, 0, sizeof(p->redirip));
	if (vrtp)
		ast_rtp_get_peer(vrtp, &p->vredirip);
	else
		memset(&p->vredirip, 0, sizeof(p->vredirip));
	p->redircodecs = codecs;
	if (!ast_test_flag(p, SIP_GOTREFER)) {
		if (!p->pendinginvite) {
			if (option_debug > 2) {
				char iabuf[INET_ADDRSTRLEN];
				ast_log(LOG_DEBUG, "Sending reinvite on SIP '%s' - It's audio soon redirected to IP %s\n", p->callid, ast_inet_ntoa(iabuf, sizeof(iabuf), rtp ? p->redirip.sin_addr : p->ourip));
			}
			transmit_reinvite_with_sdp(p);
		} else if (!ast_test_flag(p, SIP_PENDINGBYE)) {
			if (option_debug > 2) {
				char iabuf[INET_ADDRSTRLEN];
				ast_log(LOG_DEBUG, "Deferring reinvite on SIP '%s' - It's audio will be redirected to IP %s\n", p->callid, ast_inet_ntoa(iabuf, sizeof(iabuf), rtp ? p->redirip.sin_addr : p->ourip));
			}
			ast_set_flag(p, SIP_NEEDREINVITE);	
		}
	}
	/* Reset lastrtprx timer */
	time(&p->lastrtprx);
	time(&p->lastrtptx);
	ast_mutex_unlock(&p->lock);
	return 0;
}

static char *synopsis_dtmfmode = "Change the dtmfmode for a SIP call";
static char *descrip_dtmfmode = "SIPDtmfMode(inband|info|rfc2833): Changes the dtmfmode for a SIP call\n";
static char *app_dtmfmode = "SIPDtmfMode";

static char *app_sipaddheader = "SIPAddHeader";
static char *synopsis_sipaddheader = "Add a SIP header to the outbound call";

static char *descrip_sipaddheader = ""
"  SIPAddHeader(Header: Content)\n"
"Adds a header to a SIP call placed with DIAL.\n"
"Remember to user the X-header if you are adding non-standard SIP\n"
"headers, like \"X-Asterisk-Accountcode:\". Use this with care.\n"
"Adding the wrong headers may jeopardize the SIP dialog.\n"
"Always returns 0\n";


/*! \brief  sip_dtmfmode: change the DTMFmode for a SIP call (application) */
static int sip_dtmfmode(struct ast_channel *chan, void *data)
{
	struct sip_pvt *p;
	char *mode;
	if (data)
		mode = (char *)data;
	else {
		ast_log(LOG_WARNING, "This application requires the argument: info, inband, rfc2833\n");
		return 0;
	}
	ast_mutex_lock(&chan->lock);
	if (chan->type != channeltype) {
		ast_log(LOG_WARNING, "Call this application only on SIP incoming calls\n");
		ast_mutex_unlock(&chan->lock);
		return 0;
	}
	p = chan->tech_pvt;
	if (!p) {
		ast_mutex_unlock(&chan->lock);
		return 0;
	}
	ast_mutex_lock(&p->lock);
	if (!strcasecmp(mode,"info")) {
		ast_clear_flag(p, SIP_DTMF);
		ast_set_flag(p, SIP_DTMF_INFO);
	} else if (!strcasecmp(mode,"rfc2833")) {
		ast_clear_flag(p, SIP_DTMF);
		ast_set_flag(p, SIP_DTMF_RFC2833);
	} else if (!strcasecmp(mode,"inband")) { 
		ast_clear_flag(p, SIP_DTMF);
		ast_set_flag(p, SIP_DTMF_INBAND);
	} else
		ast_log(LOG_WARNING, "I don't know about this dtmf mode: %s\n",mode);
	if (ast_test_flag(p, SIP_DTMF) == SIP_DTMF_INBAND) {
		if (!p->vad) {
			p->vad = ast_dsp_new();
			ast_dsp_set_features(p->vad, DSP_FEATURE_DTMF_DETECT);
		}
	} else {
		if (p->vad) {
			ast_dsp_free(p->vad);
			p->vad = NULL;
		}
	}
	ast_mutex_unlock(&p->lock);
	ast_mutex_unlock(&chan->lock);
	return 0;
}

/*! \brief  sip_addheader: Add a SIP header */
static int sip_addheader(struct ast_channel *chan, void *data)
{
	int no = 0;
	int ok = 0;
	char varbuf[30];
	char *inbuf = (char *) data;
	
	if (ast_strlen_zero(inbuf)) {
		ast_log(LOG_WARNING, "This application requires the argument: Header\n");
		return 0;
	}
	ast_mutex_lock(&chan->lock);

	/* Check for headers */
	while (!ok && no <= 50) {
		no++;
		snprintf(varbuf, sizeof(varbuf), "_SIPADDHEADER%.2d", no);

		if( (pbx_builtin_getvar_helper(chan, (const char *) varbuf) == (const char *) NULL) )
			ok = 1;
	}
	if (ok) {
		pbx_builtin_setvar_helper (chan, varbuf, inbuf);
		if (sipdebug)
			ast_log(LOG_DEBUG,"SIP Header added \"%s\" as %s\n", inbuf, varbuf);
	} else {
		ast_log(LOG_WARNING, "Too many SIP headers added, max 50\n");
	}
	ast_mutex_unlock(&chan->lock);
	return 0;
}

/*! \brief  sip_sipredirect: Transfer call before connect with a 302 redirect
 * Called by the transfer() dialplan application through the sip_transfer()
 * pbx interface function if the call is in ringing state 
 * \todo Fix this function so that we wait for reply to the REFER and
 * 	 react to errors, denials or other issues the other end might have.
 */
static int sip_sipredirect(struct sip_pvt *p, const char *dest)
{
	char *cdest;
	char *extension, *host, *port;
	char tmp[80];
	
	if (!(cdest = ast_strdupa(dest)))
		return 0;
	
	extension = strsep(&cdest, "@");
	host = strsep(&cdest, ":");
	port = strsep(&cdest, ":");
	if (!extension) {
		ast_log(LOG_ERROR, "Missing mandatory argument: extension\n");
		return 0;
	}

	/* we'll issue the redirect message here */
	if (!host) {
		char *localtmp;
		ast_copy_string(tmp, get_header(&p->initreq, "To"), sizeof(tmp));
		if (!strlen(tmp)) {
			ast_log(LOG_ERROR, "Cannot retrieve the 'To' header from the original SIP request!\n");
			return 0;
		}
		if ((localtmp = strstr(tmp, "sip:")) && (localtmp = strchr(localtmp, '@'))) {
			char lhost[80], lport[80];
			memset(lhost, 0, sizeof(lhost));
			memset(lport, 0, sizeof(lport));
			localtmp++;
			/* This is okey because lhost and lport are as big as tmp */
			sscanf(localtmp, "%[^<>:; ]:%[^<>:; ]", lhost, lport);
			if (!strlen(lhost)) {
				ast_log(LOG_ERROR, "Can't find the host address\n");
				return 0;
			}
			if (!(host = ast_strdupa(lhost)))
				return 0;
			if (!ast_strlen_zero(lport)) {
				if (!(port = ast_strdupa(lport)))
					return 0;
			}
		}
	}

	ast_string_field_build(p, our_contact, "Transfer <sip:%s@%s%s%s>", extension, host, port ? ":" : "", port ? port : "");
	transmit_response_reliable(p, "302 Moved Temporarily", &p->initreq, 1);

	/* this is all that we want to send to that SIP device */
	ast_set_flag(p, SIP_ALREADYGONE);

	/* hangup here */
	return -1;
}

/*! \brief  sip_get_codec: Return SIP UA's codec (part of the RTP interface) */
static int sip_get_codec(struct ast_channel *chan)
{
	struct sip_pvt *p = chan->tech_pvt;
	return p->peercapability;	
}

/*! \brief  sip_rtp: Interface structure with callbacks used to connect to rtp module */
static struct ast_rtp_protocol sip_rtp = {
	type: channeltype,
	get_rtp_info: sip_get_rtp_peer,
	get_vrtp_info: sip_get_vrtp_peer,
	set_rtp_peer: sip_set_rtp_peer,
	get_codec: sip_get_codec,
};

/*! \brief  sip_poke_all_peers: Send a poke to all known peers */
static void sip_poke_all_peers(void)
{
	ASTOBJ_CONTAINER_TRAVERSE(&peerl, 1, do {
		ASTOBJ_WRLOCK(iterator);
		sip_poke_peer(iterator);
		ASTOBJ_UNLOCK(iterator);
	} while (0)
	);
}

/*! \brief  sip_send_all_registers: Send all known registrations */
static void sip_send_all_registers(void)
{
	int ms;
	int regspacing;
	if (!regobjs)
		return;
	regspacing = default_expiry * 1000/regobjs;
	if (regspacing > 100)
		regspacing = 100;
	ms = regspacing;
	ASTOBJ_CONTAINER_TRAVERSE(&regl, 1, do {
		ASTOBJ_WRLOCK(iterator);
		if (iterator->expire > -1)
			ast_sched_del(sched, iterator->expire);
		ms += regspacing;
		iterator->expire = ast_sched_add(sched, ms, sip_reregister, iterator);
		ASTOBJ_UNLOCK(iterator);
	} while (0)
	);
}

/*! \brief  sip_do_reload: Reload module */
static int sip_do_reload(enum channelreloadreason reason)
{
	clear_realm_authentication(authl);
	clear_sip_domains();
	authl = NULL;

	/* First, destroy all outstanding registry calls */
	/* This is needed, since otherwise active registry entries will not be destroyed */
	ASTOBJ_CONTAINER_TRAVERSE(&regl, 1, do {
		ASTOBJ_RDLOCK(iterator);
		if (iterator->call) {
			if (option_debug > 2)
				ast_log(LOG_DEBUG, "Destroying active SIP dialog for registry %s@%s\n", iterator->username, iterator->hostname);
			/* This will also remove references to the registry */
			sip_destroy(iterator->call);
		}
		ASTOBJ_UNLOCK(iterator);
	} while(0));

	ASTOBJ_CONTAINER_DESTROYALL(&userl, sip_destroy_user);
	ASTOBJ_CONTAINER_DESTROYALL(&regl, sip_registry_destroy);
	ASTOBJ_CONTAINER_MARKALL(&peerl);
	reload_config(reason);
	/* Prune peers who still are supposed to be deleted */
	ASTOBJ_CONTAINER_PRUNE_MARKED(&peerl, sip_destroy_peer);

	sip_poke_all_peers();
	sip_send_all_registers();

	return 0;
}

/*! \brief  sip_reload: Force reload of module from cli */
static int sip_reload(int fd, int argc, char *argv[])
{

	ast_mutex_lock(&sip_reload_lock);
	if (sip_reloading) {
		ast_verbose("Previous SIP reload not yet done\n");
	} else {
		sip_reloading = 1;
		if (fd)
			sip_reloadreason = CHANNEL_CLI_RELOAD;
		else
			sip_reloadreason = CHANNEL_MODULE_RELOAD;
	}
	ast_mutex_unlock(&sip_reload_lock);
	restart_monitor();

	return 0;
}

/*! \brief  reload: Part of Asterisk module interface */
int reload(void)
{
	return sip_reload(0, 0, NULL);
}

static struct ast_cli_entry  my_clis[] = {
	{ { "sip", "notify", NULL }, sip_notify, "Send a notify packet to a SIP peer", notify_usage, complete_sipnotify },
	{ { "sip", "show", "objects", NULL }, sip_show_objects, "Show all SIP object allocations", show_objects_usage },
	{ { "sip", "show", "users", NULL }, sip_show_users, "Show defined SIP users", show_users_usage },
	{ { "sip", "show", "user", NULL }, sip_show_user, "Show details on specific SIP user", show_user_usage, complete_sip_show_user },
	{ { "sip", "show", "subscriptions", NULL }, sip_show_subscriptions, "Show active SIP subscriptions", show_subscriptions_usage},
	{ { "sip", "show", "channels", NULL }, sip_show_channels, "Show active SIP channels", show_channels_usage},
	{ { "sip", "show", "channel", NULL }, sip_show_channel, "Show detailed SIP channel info", show_channel_usage, complete_sipch  },
	{ { "sip", "show", "history", NULL }, sip_show_history, "Show SIP dialog history", show_history_usage, complete_sipch  },
	{ { "sip", "show", "domains", NULL }, sip_show_domains, "List our local SIP domains.", show_domains_usage },
	{ { "sip", "show", "settings", NULL }, sip_show_settings, "Show SIP global settings", show_settings_usage  },
	{ { "sip", "debug", NULL }, sip_do_debug, "Enable SIP debugging", debug_usage },
	{ { "sip", "debug", "ip", NULL }, sip_do_debug, "Enable SIP debugging on IP", debug_usage },
	{ { "sip", "debug", "peer", NULL }, sip_do_debug, "Enable SIP debugging on Peername", debug_usage, complete_sip_debug_peer },
	{ { "sip", "show", "peer", NULL }, sip_show_peer, "Show details on specific SIP peer", show_peer_usage, complete_sip_show_peer },
	{ { "sip", "show", "peers", NULL }, sip_show_peers, "Show defined SIP peers", show_peers_usage },
	{ { "sip", "prune", "realtime", NULL }, sip_prune_realtime,
	  "Prune cached Realtime object(s)", prune_realtime_usage },
	{ { "sip", "prune", "realtime", "peer", NULL }, sip_prune_realtime,
	  "Prune cached Realtime peer(s)", prune_realtime_usage, complete_sip_prune_realtime_peer },
	{ { "sip", "prune", "realtime", "user", NULL }, sip_prune_realtime,
	  "Prune cached Realtime user(s)", prune_realtime_usage, complete_sip_prune_realtime_user },
	{ { "sip", "show", "inuse", NULL }, sip_show_inuse, "List all inuse/limits", show_inuse_usage },
	{ { "sip", "show", "registry", NULL }, sip_show_registry, "Show SIP registration status", show_reg_usage },
	{ { "sip", "history", NULL }, sip_do_history, "Enable SIP history", history_usage },
	{ { "sip", "no", "history", NULL }, sip_no_history, "Disable SIP history", no_history_usage },
	{ { "sip", "no", "debug", NULL }, sip_no_debug, "Disable SIP debugging", no_debug_usage },
	{ { "sip", "reload", NULL }, sip_reload, "Reload SIP configuration", sip_reload_usage },
};

/*! \brief  load_module: PBX load module - initialization */
int load_module()
{
	ASTOBJ_CONTAINER_INIT(&userl);	/* User object list */
	ASTOBJ_CONTAINER_INIT(&peerl);	/* Peer object list */
	ASTOBJ_CONTAINER_INIT(&regl);	/* Registry object list */

	sched = sched_context_create();
	if (!sched) {
		ast_log(LOG_WARNING, "Unable to create schedule context\n");
	}

	io = io_context_create();
	if (!io) {
		ast_log(LOG_WARNING, "Unable to create I/O context\n");
	}
	sip_reloadreason = CHANNEL_MODULE_LOAD;
	reload_config(sip_reloadreason);	/* Load the configuration from sip.conf */

	/* Make sure we can register our sip channel type */
	if (ast_channel_register(&sip_tech)) {
		ast_log(LOG_ERROR, "Unable to register channel type %s\n", channeltype);
		return -1;
	}

	/* Register all CLI functions for SIP */
	ast_cli_register_multiple(my_clis, sizeof(my_clis)/ sizeof(my_clis[0]));

	/* Tell the RTP subdriver that we're here */
	ast_rtp_proto_register(&sip_rtp);

	/* Register dialplan applications */
	ast_register_application(app_dtmfmode, sip_dtmfmode, synopsis_dtmfmode, descrip_dtmfmode);
	ast_register_application(app_sipaddheader, sip_addheader, synopsis_sipaddheader, descrip_sipaddheader);

	/* Register dialplan functions */
	ast_custom_function_register(&sip_header_function);
	ast_custom_function_register(&sippeer_function);
	ast_custom_function_register(&sipchaninfo_function);
	ast_custom_function_register(&checksipdomain_function);

	/* Register manager commands */
	ast_manager_register2("SIPpeers", EVENT_FLAG_SYSTEM, manager_sip_show_peers,
			"List SIP peers (text format)", mandescr_show_peers);
	ast_manager_register2("SIPshowpeer", EVENT_FLAG_SYSTEM, manager_sip_show_peer,
			"Show SIP peer (text format)", mandescr_show_peer);

	sip_poke_all_peers();	
	sip_send_all_registers();
	
	/* And start the monitor for the first time */
	restart_monitor();

	return 0;
}

int unload_module()
{
	struct sip_pvt *p, *pl;
	
	/* First, take us out of the channel type list */
	ast_channel_unregister(&sip_tech);

	ast_custom_function_unregister(&sipchaninfo_function);
	ast_custom_function_unregister(&sippeer_function);
	ast_custom_function_unregister(&sip_header_function);
	ast_custom_function_unregister(&checksipdomain_function);

	ast_unregister_application(app_dtmfmode);
	ast_unregister_application(app_sipaddheader);

	ast_cli_unregister_multiple(my_clis, sizeof(my_clis) / sizeof(my_clis[0]));

	ast_rtp_proto_unregister(&sip_rtp);

	ast_manager_unregister("SIPpeers");
	ast_manager_unregister("SIPshowpeer");

	if (!ast_mutex_lock(&iflock)) {
		/* Hangup all interfaces if they have an owner */
		p = iflist;
		while (p) {
			if (p->owner)
				ast_softhangup(p->owner, AST_SOFTHANGUP_APPUNLOAD);
			p = p->next;
		}
		ast_mutex_unlock(&iflock);
	} else {
		ast_log(LOG_WARNING, "Unable to lock the interface list\n");
		return -1;
	}

	if (!ast_mutex_lock(&monlock)) {
		if (monitor_thread && (monitor_thread != AST_PTHREADT_STOP)) {
			pthread_cancel(monitor_thread);
			pthread_kill(monitor_thread, SIGURG);
			pthread_join(monitor_thread, NULL);
		}
		monitor_thread = AST_PTHREADT_STOP;
		ast_mutex_unlock(&monlock);
	} else {
		ast_log(LOG_WARNING, "Unable to lock the monitor\n");
		return -1;
	}

	if (!ast_mutex_lock(&iflock)) {
		/* Destroy all the interfaces and free their memory */
		p = iflist;
		while (p) {
			pl = p;
			p = p->next;
			/* Free associated memory */
			ast_mutex_destroy(&pl->lock);
			if (pl->chanvars) {
				ast_variables_destroy(pl->chanvars);
				pl->chanvars = NULL;
			}
			free(pl);
		}
		iflist = NULL;
		ast_mutex_unlock(&iflock);
	} else {
		ast_log(LOG_WARNING, "Unable to lock the interface list\n");
		return -1;
	}

	/* Free memory for local network address mask */
	ast_free_ha(localaddr);

	ASTOBJ_CONTAINER_DESTROYALL(&userl, sip_destroy_user);
	ASTOBJ_CONTAINER_DESTROY(&userl);
	ASTOBJ_CONTAINER_DESTROYALL(&peerl, sip_destroy_peer);
	ASTOBJ_CONTAINER_DESTROY(&peerl);
	ASTOBJ_CONTAINER_DESTROYALL(&regl, sip_registry_destroy);
	ASTOBJ_CONTAINER_DESTROY(&regl);

	clear_realm_authentication(authl);
	clear_sip_domains();
	close(sipsock);
		
	return 0;
}

int usecount()
{
	return usecnt;
}

char *key()
{
	return ASTERISK_GPL_KEY;
}

char *description()
{
	return (char *) desc;
}


