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
 * \brief Implementation of Inter-Asterisk eXchange Version 2
 *
 * \author Mark Spencer <markster@digium.com>
 *
 * \par See also
 * \arg \ref Config_iax
 *
 * \ingroup channel_drivers
 */

#include <stdlib.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <dirent.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netinet/in_systm.h>
#include <netinet/ip.h>
#include <sys/time.h>
#include <sys/signal.h>
#include <signal.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <netdb.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <regex.h>
#ifdef IAX_TRUNKING
#include <sys/ioctl.h>
#ifdef __linux__
#include <linux/zaptel.h>
#else
#include <zaptel.h>
#endif /* __linux__ */
#endif

#include "asterisk.h"

ASTERISK_FILE_VERSION(__FILE__, "$Revision$")

#include "asterisk/lock.h"
#include "asterisk/frame.h" 
#include "asterisk/channel.h"
#include "asterisk/logger.h"
#include "asterisk/module.h"
#include "asterisk/pbx.h"
#include "asterisk/sched.h"
#include "asterisk/io.h"
#include "asterisk/config.h"
#include "asterisk/options.h"
#include "asterisk/cli.h"
#include "asterisk/translate.h"
#include "asterisk/md5.h"
#include "asterisk/cdr.h"
#include "asterisk/crypto.h"
#include "asterisk/acl.h"
#include "asterisk/manager.h"
#include "asterisk/callerid.h"
#include "asterisk/app.h"
#include "asterisk/astdb.h"
#include "asterisk/musiconhold.h"
#include "asterisk/features.h"
#include "asterisk/utils.h"
#include "asterisk/causes.h"
#include "asterisk/localtime.h"
#include "asterisk/aes.h"
#include "asterisk/dnsmgr.h"
#include "asterisk/devicestate.h"
#include "asterisk/netsock.h"

#include "iax2.h"
#include "iax2-parser.h"
#include "iax2-provision.h"

/* Define NEWJB to use the new channel independent jitterbuffer,
 * otherwise, use the old jitterbuffer */
#define NEWJB

#ifdef NEWJB
#include "../jitterbuf.h"
#endif

#ifndef IPTOS_MINCOST
#define IPTOS_MINCOST 0x02
#endif

#ifdef SO_NO_CHECK
static int nochecksums = 0;
#endif

/*
 * Uncomment to try experimental IAX bridge optimization,
 * designed to reduce latency when IAX calls cannot
 * be trasnferred -- obsolete
 */

/* #define BRIDGE_OPTIMIZATION  */


#define PTR_TO_CALLNO(a) ((unsigned short)(unsigned long)(a))
#define CALLNO_TO_PTR(a) ((void *)(unsigned long)(a))

#define DEFAULT_RETRY_TIME 1000
#define MEMORY_SIZE 100
#define DEFAULT_DROP 3
/* Flag to use with trunk calls, keeping these calls high up.  It halves our effective use
   but keeps the division between trunked and non-trunked better. */
#define TRUNK_CALL_START	0x4000

#define DEBUG_SUPPORT

#define MIN_REUSE_TIME		60	/* Don't reuse a call number within 60 seconds */

/* Sample over last 100 units to determine historic jitter */
#define GAMMA (0.01)

static struct ast_codec_pref prefs;

static const char desc[] = "Inter Asterisk eXchange (Ver 2)";
static const char tdesc[] = "Inter Asterisk eXchange Driver (Ver 2)";
static const char channeltype[] = "IAX2";

static char context[80] = "default";

static char language[MAX_LANGUAGE] = "";
static char regcontext[AST_MAX_CONTEXT] = "";

static int max_retries = 4;
static int ping_time = 20;
static int lagrq_time = 10;
static int maxtrunkcall = TRUNK_CALL_START;
static int maxnontrunkcall = 1;
static int maxjitterbuffer=1000;
#ifdef NEWJB
static int resyncthreshold=1000;
static int maxjitterinterps=10;
#endif
static int jittershrinkrate=2;
static int trunkfreq = 20;
static int authdebug = 1;
static int autokill = 0;
static int iaxcompat = 0;

static int iaxdefaultdpcache=10 * 60;	/* Cache dialplan entries for 10 minutes by default */

static int iaxdefaulttimeout = 5;		/* Default to wait no more than 5 seconds for a reply to come back */

static int tos = 0;

static int min_reg_expire;
static int max_reg_expire;

static int timingfd = -1;				/* Timing file descriptor */

static struct ast_netsock_list *netsock;
static int defaultsockfd = -1;

static int usecnt;
AST_MUTEX_DEFINE_STATIC(usecnt_lock);

int (*iax2_regfunk)(char *username, int onoff) = NULL;

/* Ethernet, etc */
#define IAX_CAPABILITY_FULLBANDWIDTH 	0xFFFF
/* T1, maybe ISDN */
#define IAX_CAPABILITY_MEDBANDWIDTH 	(IAX_CAPABILITY_FULLBANDWIDTH & 	\
							~AST_FORMAT_SLINEAR & 	\
							~AST_FORMAT_ULAW & 	\
							~AST_FORMAT_ALAW) 
/* A modem */
#define IAX_CAPABILITY_LOWBANDWIDTH		(IAX_CAPABILITY_MEDBANDWIDTH & 	\
							~AST_FORMAT_G726 & 	\
							~AST_FORMAT_ADPCM)

#define IAX_CAPABILITY_LOWFREE		(IAX_CAPABILITY_LOWBANDWIDTH & 		\
							 ~AST_FORMAT_G723_1)


#define DEFAULT_MAXMS		2000		/* Must be faster than 2 seconds by default */
#define DEFAULT_FREQ_OK		60 * 1000	/* How often to check for the host to be up */
#define DEFAULT_FREQ_NOTOK	10 * 1000	/* How often to check, if the host is down... */

static	struct io_context *io;
static	struct sched_context *sched;

static int iax2_capability = IAX_CAPABILITY_FULLBANDWIDTH;

static int iax2_dropcount = DEFAULT_DROP;

static int iaxdebug = 0;

static int iaxtrunkdebug = 0;

static int test_losspct = 0;
#ifdef IAXTESTS
static int test_late = 0;
static int test_resync = 0;
static int test_jit = 0;
static int test_jitpct = 0;
#endif /* IAXTESTS */

static char accountcode[AST_MAX_ACCOUNT_CODE];
static int amaflags = 0;
static int delayreject = 0;
static int iax2_encryption = 0;

static struct ast_flags globalflags = { 0 };

static pthread_t netthreadid = AST_PTHREADT_NULL;

enum {
	IAX_STATE_STARTED = 		(1 << 0),
	IAX_STATE_AUTHENTICATED = 	(1 << 1),
	IAX_STATE_TBD = 		(1 << 2)
} iax2_state;

struct iax2_context {
	char context[AST_MAX_CONTEXT];
	struct iax2_context *next;
};

enum {
	IAX_HASCALLERID = 	(1 << 0),	/*!< CallerID has been specified */
	IAX_DELME =		(1 << 1),	/*!< Needs to be deleted */
	IAX_TEMPONLY =		(1 << 2),	/*!< Temporary (realtime) */
	IAX_TRUNK =		(1 << 3),	/*!< Treat as a trunk */
	IAX_NOTRANSFER =	(1 << 4),	/*!< Don't native bridge */
	IAX_USEJITTERBUF =	(1 << 5),	/*!< Use jitter buffer */
	IAX_DYNAMIC =		(1 << 6),	/*!< dynamic peer */
	IAX_SENDANI = 		(1 << 7),	/*!< Send ANI along with CallerID */
	IAX_MESSAGEDETAIL =	(1 << 8),	/*!< Show exact numbers */
	IAX_ALREADYGONE =	(1 << 9),	/*!< Already disconnected */
	IAX_PROVISION =		(1 << 10),	/*!< This is a provisioning request */
	IAX_QUELCH = 		(1 << 11),	/*!< Whether or not we quelch audio */
	IAX_ENCRYPTED =		(1 << 12),	/*!< Whether we should assume encrypted tx/rx */
	IAX_KEYPOPULATED = 	(1 << 13),	/*!< Whether we have a key populated */
	IAX_CODEC_USER_FIRST = 	(1 << 14),	/*!< are we willing to let the other guy choose the codec? */
	IAX_CODEC_NOPREFS =  	(1 << 15), 	/*!< Force old behaviour by turning off prefs */
	IAX_CODEC_NOCAP = 	(1 << 16),	/*!< only consider requested format and ignore capabilities*/
	IAX_RTCACHEFRIENDS = 	(1 << 17), 	/*!< let realtime stay till your reload */
	IAX_RTUPDATE = 		(1 << 18), 	/*!< Send a realtime update */
	IAX_RTAUTOCLEAR = 	(1 << 19), 	/*!< erase me on expire */ 
	IAX_FORCEJITTERBUF =	(1 << 20),	/*!< Force jitterbuffer, even when bridged to a channel that can take jitter */ 
	IAX_RTIGNOREREGEXPIRE =	(1 << 21),	/*!< When using realtime, ignore registration expiration */
	IAX_TRUNKTIMESTAMPS =	(1 << 22)	/*!< Send trunk timestamps */
} iax2_flags;

static int global_rtautoclear = 120;

static int reload_config(void);
static int iax2_reload(int fd, int argc, char *argv[]);


struct iax2_user {
	char name[80];
	char secret[80];
	char dbsecret[80];
	int authmethods;
	int encmethods;
	char accountcode[AST_MAX_ACCOUNT_CODE];
	char inkeys[80];				/*!< Key(s) this user can use to authenticate to us */
	char language[MAX_LANGUAGE];
	int amaflags;
	unsigned int flags;
	int capability;
	char cid_num[AST_MAX_EXTENSION];
	char cid_name[AST_MAX_EXTENSION];
	struct ast_codec_pref prefs;
	struct ast_ha *ha;
	struct iax2_context *contexts;
	struct iax2_user *next;
	struct ast_variable *vars;
};

struct iax2_peer {
	char name[80];
	char username[80];		
	char secret[80];
	char dbsecret[80];
	char outkey[80];				/*!< What key we use to talk to this peer */
	char context[AST_MAX_CONTEXT];			/*!< For transfers only */
	char regexten[AST_MAX_EXTENSION];		/*!< Extension to register (if regcontext is used) */
	char peercontext[AST_MAX_EXTENSION];		/*!< Context to pass to peer */
	char mailbox[AST_MAX_EXTENSION];		/*!< Mailbox */
	struct ast_codec_pref prefs;
	struct ast_dnsmgr_entry *dnsmgr;		/*!< DNS refresh manager */
	struct sockaddr_in addr;
	int formats;
	int sockfd;					/*!< Socket to use for transmission */
	struct in_addr mask;
	unsigned int flags;

	/* Dynamic Registration fields */
	struct sockaddr_in defaddr;			/*!< Default address if there is one */
	int authmethods;				/*!< Authentication methods (IAX_AUTH_*) */
	int encmethods;					/*!< Encryption methods (IAX_ENCRYPT_*) */
	char inkeys[80];				/*!< Key(s) this peer can use to authenticate to us */

	/* Suggested caller id if registering */
	char cid_num[AST_MAX_EXTENSION];		/*!< Default context (for transfer really) */
	char cid_name[AST_MAX_EXTENSION];		/*!< Default context (for transfer really) */
	
	int expire;					/*!< Schedule entry for expiry */
	int expiry;					/*!< How soon to expire */
	int capability;					/*!< Capability */
	char zonetag[80];				/*!< Time Zone */

	/* Qualification */
	int callno;					/*!< Call number of POKE request */
	int pokeexpire;					/*!< When to expire poke */
	int lastms;					/*!< How long last response took (in ms), or -1 for no response */
	int maxms;					/*!< Max ms we will accept for the host to be up, 0 to not monitor */

	int pokefreqok;					/*!< How often to check if the host is up */
	int pokefreqnotok;				/*!< How often to check when the host has been determined to be down */
	int historicms;					/*!< How long recent average responses took */
	int smoothing;					/*!< Sample over how many units to determine historic ms */
	
	struct ast_ha *ha;
	struct iax2_peer *next;
};

#define IAX2_TRUNK_PREFACE (sizeof(struct iax_frame) + sizeof(struct ast_iax2_meta_hdr) + sizeof(struct ast_iax2_meta_trunk_hdr))

static struct iax2_trunk_peer {
	ast_mutex_t lock;
	int sockfd;
	struct sockaddr_in addr;
	struct timeval txtrunktime;		/*!< Transmit trunktime */
	struct timeval rxtrunktime;		/*!< Receive trunktime */
	struct timeval lasttxtime;		/*!< Last transmitted trunktime */
	struct timeval trunkact;		/*!< Last trunk activity */
	unsigned int lastsent;			/*!< Last sent time */
	/* Trunk data and length */
	unsigned char *trunkdata;
	unsigned int trunkdatalen;
	unsigned int trunkdataalloc;
	struct iax2_trunk_peer *next;
	int trunkerror;
	int calls;
} *tpeers = NULL;

AST_MUTEX_DEFINE_STATIC(tpeerlock);

struct iax_firmware {
	struct iax_firmware *next;
	int fd;
	int mmaplen;
	int dead;
	struct ast_iax2_firmware_header *fwh;
	unsigned char *buf;
};

enum iax_reg_state {
	REG_STATE_UNREGISTERED = 0,
	REG_STATE_REGSENT,
	REG_STATE_AUTHSENT,
	REG_STATE_REGISTERED,
	REG_STATE_REJECTED,
	REG_STATE_TIMEOUT,
	REG_STATE_NOAUTH
};

enum iax_transfer_state {
	TRANSFER_NONE = 0,
	TRANSFER_BEGIN,
	TRANSFER_READY,
	TRANSFER_RELEASED,
	TRANSFER_PASSTHROUGH
};

struct iax2_registry {
	struct sockaddr_in addr;		/*!< Who we connect to for registration purposes */
	char username[80];
	char secret[80];			/*!< Password or key name in []'s */
	char random[80];
	int expire;				/*!< Sched ID of expiration */
	int refresh;				/*!< How often to refresh */
	enum iax_reg_state regstate;
	int messages;				/*!< Message count */
	int callno;				/*!< Associated call number if applicable */
	struct sockaddr_in us;			/*!< Who the server thinks we are */
	struct iax2_registry *next;
};

static struct iax2_registry *registrations;

/* Don't retry more frequently than every 10 ms, or less frequently than every 5 seconds */
#define MIN_RETRY_TIME		100
#define MAX_RETRY_TIME  	10000

#define MAX_JITTER_BUFFER 	50
#define MIN_JITTER_BUFFER 	10

#define DEFAULT_TRUNKDATA	640 * 10	/*!< 40ms, uncompressed linear * 10 channels */
#define MAX_TRUNKDATA		640 * 200	/*!< 40ms, uncompressed linear * 200 channels */

#define MAX_TIMESTAMP_SKEW	160		/*!< maximum difference between actual and predicted ts for sending */

/* If consecutive voice frame timestamps jump by more than this many milliseconds, then jitter buffer will resync */
#define TS_GAP_FOR_JB_RESYNC	5000

/* If we have more than this much excess real jitter buffer, shrink it. */
static int max_jitter_buffer = MAX_JITTER_BUFFER;
/* If we have less than this much excess real jitter buffer, enlarge it. */
static int min_jitter_buffer = MIN_JITTER_BUFFER;

struct iax_rr {
	int jitter;
	int losspct;
	int losscnt;
	int packets;
	int delay;
	int dropped;
	int ooo;
};

struct chan_iax2_pvt {
	/*! Socket to send/receive on for this call */
	int sockfd;
	/*! Last received voice format */
	int voiceformat;
	/*! Last received video format */
	int videoformat;
	/*! Last sent voice format */
	int svoiceformat;
	/*! Last sent video format */
	int svideoformat;
	/*! What we are capable of sending */
	int capability;
	/*! Last received timestamp */
	unsigned int last;
	/*! Last sent timestamp - never send the same timestamp twice in a single call */
	unsigned int lastsent;
	/*! Next outgoing timestamp if everything is good */
	unsigned int nextpred;
	/*! True if the last voice we transmitted was not silence/CNG */
	int notsilenttx;
	/*! Ping time */
	unsigned int pingtime;
	/*! Max time for initial response */
	int maxtime;
	/*! Peer Address */
	struct sockaddr_in addr;
	struct ast_codec_pref prefs;
	/*! Our call number */
	unsigned short callno;
	/*! Peer callno */
	unsigned short peercallno;
	/*! Peer selected format */
	int peerformat;
	/*! Peer capability */
	int peercapability;
	/*! timeval that we base our transmission on */
	struct timeval offset;
	/*! timeval that we base our delivery on */
	struct timeval rxcore;
#ifdef NEWJB
	/*! The jitterbuffer */
        jitterbuf *jb;
	/*! active jb read scheduler id */
        int jbid;                       
#else
	/*! Historical delivery time */
	int history[MEMORY_SIZE];
	/*! Current base jitterbuffer */
	int jitterbuffer;
	/*! Current jitter measure */
	int jitter;
	/*! Historic jitter value */
	int historicjitter;
#endif
	/*! LAG */
	int lag;
	/*! Error, as discovered by the manager */
	int error;
	/*! Owner if we have one */
	struct ast_channel *owner;
	/*! What's our state? */
	struct ast_flags state;
	/*! Expiry (optional) */
	int expiry;
	/*! Next outgoing sequence number */
	unsigned char oseqno;
	/*! Next sequence number they have not yet acknowledged */
	unsigned char rseqno;
	/*! Next incoming sequence number */
	unsigned char iseqno;
	/*! Last incoming sequence number we have acknowledged */
	unsigned char aseqno;
	/*! Peer name */
	char peer[80];
	/*! Default Context */
	char context[80];
	/*! Caller ID if available */
	char cid_num[80];
	char cid_name[80];
	/*! Hidden Caller ID (i.e. ANI) if appropriate */
	char ani[80];
	/*! DNID */
	char dnid[80];
	/*! Requested Extension */
	char exten[AST_MAX_EXTENSION];
	/*! Expected Username */
	char username[80];
	/*! Expected Secret */
	char secret[80];
	/*! permitted authentication methods */
	int authmethods;
	/*! permitted encryption methods */
	int encmethods;
	/*! MD5 challenge */
	char challenge[10];
	/*! Public keys permitted keys for incoming authentication */
	char inkeys[80];
	/*! Private key for outgoing authentication */
	char outkey[80];
	/*! Encryption AES-128 Key */
	aes_encrypt_ctx ecx;
	/*! Decryption AES-128 Key */
	aes_decrypt_ctx dcx;
	/*! 32 bytes of semi-random data */
	unsigned char semirand[32];
	/*! Preferred language */
	char language[MAX_LANGUAGE];
	/*! Hostname/peername for naming purposes */
	char host[80];
	/*! Associated registry */
	struct iax2_registry *reg;
	/*! Associated peer for poking */
	struct iax2_peer *peerpoke;
	/*! IAX_ flags */
	unsigned int flags;

	/*! Transferring status */
	enum iax_transfer_state transferring;
	/*! Transfer identifier */
	int transferid;
	/*! Who we are IAX transfering to */
	struct sockaddr_in transfer;
	/*! What's the new call number for the transfer */
	unsigned short transfercallno;
	/*! Transfer decrypt AES-128 Key */
	aes_encrypt_ctx tdcx;

	/*! Status of knowledge of peer ADSI capability */
	int peeradsicpe;
	
	/*! Who we are bridged to */
	unsigned short bridgecallno;
	unsigned int bridgesfmt;
	struct ast_trans_pvt *bridgetrans;
	
	int pingid;			/*!< Transmit PING request */
	int lagid;			/*!< Retransmit lag request */
	int autoid;			/*!< Auto hangup for Dialplan requestor */
	int authid;			/*!< Authentication rejection ID */
	int authfail;			/*!< Reason to report failure */
	int initid;			/*!< Initial peer auto-congest ID (based on qualified peers) */
	int calling_ton;
	int calling_tns;
	int calling_pres;
	char dproot[AST_MAX_EXTENSION];
	char accountcode[AST_MAX_ACCOUNT_CODE];
	int amaflags;
	struct iax2_dpcache *dpentries;
	struct ast_variable *vars;
	/*! last received remote rr */
	struct iax_rr remote_rr;
	/*! Current base time: (just for stats) */
	int min;
	/*! Dropped frame count: (just for stats) */
	int frames_dropped;
	/*! received frame count: (just for stats) */
	int frames_received;
};

static struct ast_iax2_queue {
	struct iax_frame *head;
	struct iax_frame *tail;
	int count;
	ast_mutex_t lock;
} iaxq;

static struct ast_user_list {
	struct iax2_user *users;
	ast_mutex_t lock;
} userl;

static struct ast_peer_list {
	struct iax2_peer *peers;
	ast_mutex_t lock;
} peerl;

static struct ast_firmware_list {
	struct iax_firmware *wares;
	ast_mutex_t lock;
} waresl;

/*! Extension exists */
#define CACHE_FLAG_EXISTS		(1 << 0)
/*! Extension is nonexistent */
#define CACHE_FLAG_NONEXISTENT		(1 << 1)
/*! Extension can exist */
#define CACHE_FLAG_CANEXIST		(1 << 2)
/*! Waiting to hear back response */
#define CACHE_FLAG_PENDING		(1 << 3)
/*! Timed out */
#define CACHE_FLAG_TIMEOUT		(1 << 4)
/*! Request transmitted */
#define CACHE_FLAG_TRANSMITTED		(1 << 5)
/*! Timeout */
#define CACHE_FLAG_UNKNOWN		(1 << 6)
/*! Matchmore */
#define CACHE_FLAG_MATCHMORE		(1 << 7)

static struct iax2_dpcache {
	char peercontext[AST_MAX_CONTEXT];
	char exten[AST_MAX_EXTENSION];
	struct timeval orig;
	struct timeval expiry;
	int flags;
	unsigned short callno;
	int waiters[256];
	struct iax2_dpcache *next;
	struct iax2_dpcache *peer;	/*!< For linking in peers */
} *dpcache;

AST_MUTEX_DEFINE_STATIC(dpcache_lock);

static void reg_source_db(struct iax2_peer *p);
static struct iax2_peer *realtime_peer(const char *peername, struct sockaddr_in *sin);

static void destroy_peer(struct iax2_peer *peer);
static int ast_cli_netstats(int fd, int limit_fmt);

static void iax_debug_output(const char *data)
{
	if (iaxdebug)
		ast_verbose("%s", data);
}

static void iax_error_output(const char *data)
{
	ast_log(LOG_WARNING, "%s", data);
}

#ifdef NEWJB
static void jb_error_output(const char *fmt, ...)
{
	va_list args;
	char buf[1024];

	va_start(args, fmt);
	vsnprintf(buf, 1024, fmt, args);
	va_end(args);

	ast_log(LOG_ERROR, buf);
}

static void jb_warning_output(const char *fmt, ...)
{
	va_list args;
	char buf[1024];

	va_start(args, fmt);
	vsnprintf(buf, 1024, fmt, args);
	va_end(args);

	ast_log(LOG_WARNING, buf);
}

static void jb_debug_output(const char *fmt, ...)
{
	va_list args;
	char buf[1024];

	va_start(args, fmt);
	vsnprintf(buf, 1024, fmt, args);
	va_end(args);

	ast_verbose(buf);
}
#endif


/* XXX We probably should use a mutex when working with this XXX */
static struct chan_iax2_pvt *iaxs[IAX_MAX_CALLS];
static ast_mutex_t iaxsl[IAX_MAX_CALLS];
static struct timeval lastused[IAX_MAX_CALLS];


static int send_command(struct chan_iax2_pvt *, char, int, unsigned int, const unsigned char *, int, int);
static int send_command_locked(unsigned short callno, char, int, unsigned int, const unsigned char *, int, int);
static int send_command_immediate(struct chan_iax2_pvt *, char, int, unsigned int, const unsigned char *, int, int);
static int send_command_final(struct chan_iax2_pvt *, char, int, unsigned int, const unsigned char *, int, int);
static int send_command_transfer(struct chan_iax2_pvt *, char, int, unsigned int, const unsigned char *, int);
static struct iax2_user *build_user(const char *name, struct ast_variable *v, int temponly);
static void destroy_user(struct iax2_user *user);
static int expire_registry(void *data);
static int iax2_write(struct ast_channel *c, struct ast_frame *f);
static int iax2_do_register(struct iax2_registry *reg);
static void prune_peers(void);
static int iax2_poke_peer(struct iax2_peer *peer, int heldcall);
static int iax2_provision(struct sockaddr_in *end, int sockfd, char *dest, const char *template, int force);

static struct ast_channel *iax2_request(const char *type, int format, void *data, int *cause);
static int iax2_devicestate(void *data);
static int iax2_digit(struct ast_channel *c, char digit);
static int iax2_sendtext(struct ast_channel *c, const char *text);
static int iax2_sendimage(struct ast_channel *c, struct ast_frame *img);
static int iax2_sendhtml(struct ast_channel *c, int subclass, const char *data, int datalen);
static int iax2_call(struct ast_channel *c, char *dest, int timeout);
static int iax2_hangup(struct ast_channel *c);
static int iax2_answer(struct ast_channel *c);
static struct ast_frame *iax2_read(struct ast_channel *c);
static int iax2_write(struct ast_channel *c, struct ast_frame *f);
static int iax2_indicate(struct ast_channel *c, int condition);
static int iax2_setoption(struct ast_channel *c, int option, void *data, int datalen);
static enum ast_bridge_result iax2_bridge(struct ast_channel *c0, struct ast_channel *c1, int flags, struct ast_frame **fo, struct ast_channel **rc, int timeoutms);
static int iax2_transfer(struct ast_channel *c, const char *dest);
static int iax2_fixup(struct ast_channel *oldchannel, struct ast_channel *newchan);

static const struct ast_channel_tech iax2_tech = {
	.type = channeltype,
	.description = tdesc,
	.capabilities = IAX_CAPABILITY_FULLBANDWIDTH,
	.properties = AST_CHAN_TP_WANTSJITTER,
	.requester = iax2_request,
	.devicestate = iax2_devicestate,
	.send_digit = iax2_digit,
	.send_text = iax2_sendtext,
	.send_image = iax2_sendimage,
	.send_html = iax2_sendhtml,
	.call = iax2_call,
	.hangup = iax2_hangup,
	.answer = iax2_answer,
	.read = iax2_read,
	.write = iax2_write,
	.write_video = iax2_write,
	.indicate = iax2_indicate,
	.setoption = iax2_setoption,
	.bridge = iax2_bridge,
	.transfer = iax2_transfer,
	.fixup = iax2_fixup,
};

static int send_ping(void *data)
{
	int callno = (long)data;
	/* Ping only if it's real, not if it's bridged */
	if (iaxs[callno]) {
#ifdef BRIDGE_OPTIMIZATION
		if (!iaxs[callno]->bridgecallno)
#endif
			send_command(iaxs[callno], AST_FRAME_IAX, IAX_COMMAND_PING, 0, NULL, 0, -1);
		return 1;
	} else
		return 0;
}

static int get_encrypt_methods(const char *s)
{
	int e;
	if (!strcasecmp(s, "aes128"))
		e = IAX_ENCRYPT_AES128;
	else if (ast_true(s))
		e = IAX_ENCRYPT_AES128;
	else
		e = 0;
	return e;
}

static int send_lagrq(void *data)
{
	int callno = (long)data;
	/* Ping only if it's real not if it's bridged */
	if (iaxs[callno]) {
#ifdef BRIDGE_OPTIMIZATION
		if (!iaxs[callno]->bridgecallno)
#endif		
			send_command(iaxs[callno], AST_FRAME_IAX, IAX_COMMAND_LAGRQ, 0, NULL, 0, -1);
		return 1;
	} else
		return 0;
}

static unsigned char compress_subclass(int subclass)
{
	int x;
	int power=-1;
	/* If it's 128 or smaller, just return it */
	if (subclass < IAX_FLAG_SC_LOG)
		return subclass;
	/* Otherwise find its power */
	for (x = 0; x < IAX_MAX_SHIFT; x++) {
		if (subclass & (1 << x)) {
			if (power > -1) {
				ast_log(LOG_WARNING, "Can't compress subclass %d\n", subclass);
				return 0;
			} else
				power = x;
		}
	}
	return power | IAX_FLAG_SC_LOG;
}

static int uncompress_subclass(unsigned char csub)
{
	/* If the SC_LOG flag is set, return 2^csub otherwise csub */
	if (csub & IAX_FLAG_SC_LOG) {
		/* special case for 'compressed' -1 */
		if (csub == 0xff)
			return -1;
		else
			return 1 << (csub & ~IAX_FLAG_SC_LOG & IAX_MAX_SHIFT);
	}
	else
		return csub;
}

static struct iax2_peer *find_peer(const char *name, int realtime) 
{
	struct iax2_peer *peer;
	ast_mutex_lock(&peerl.lock);
	for(peer = peerl.peers; peer; peer = peer->next) {
		if (!strcasecmp(peer->name, name)) {
			break;
		}
	}
	ast_mutex_unlock(&peerl.lock);
	if(!peer && realtime)
		peer = realtime_peer(name, NULL);
	return peer;
}

static int iax2_getpeername(struct sockaddr_in sin, char *host, int len, int lockpeer)
{
	struct iax2_peer *peer;
	int res = 0;

	if (lockpeer)
		ast_mutex_lock(&peerl.lock);
	peer = peerl.peers;
	while (peer) {
		if ((peer->addr.sin_addr.s_addr == sin.sin_addr.s_addr) &&
				(peer->addr.sin_port == sin.sin_port)) {
					ast_copy_string(host, peer->name, len);
					res = 1;
					break;
		}
		peer = peer->next;
	}
	if (lockpeer)
		ast_mutex_unlock(&peerl.lock);
	if (!peer) {
		peer = realtime_peer(NULL, &sin);
		if (peer) {
			ast_copy_string(host, peer->name, len);
			if (ast_test_flag(peer, IAX_TEMPONLY))
				destroy_peer(peer);
			res = 1;
		}
	}

	return res;
}

static struct chan_iax2_pvt *new_iax(struct sockaddr_in *sin, int lockpeer, const char *host)
{
	struct chan_iax2_pvt *tmp;

	if (!(tmp = ast_calloc(1, sizeof(*tmp))))
		return NULL;

	tmp->prefs = prefs;
	tmp->callno = 0;
	tmp->peercallno = 0;
	tmp->transfercallno = 0;
	tmp->bridgecallno = 0;
	tmp->pingid = -1;
	tmp->lagid = -1;
	tmp->autoid = -1;
	tmp->authid = -1;
	tmp->initid = -1;
	/* ast_copy_string(tmp->context, context, sizeof(tmp->context)); */
	ast_copy_string(tmp->exten, "s", sizeof(tmp->exten));
	ast_copy_string(tmp->host, host, sizeof(tmp->host));
#ifdef NEWJB
	{
		jb_conf jbconf;

		tmp->jb = jb_new();
		tmp->jbid = -1;
		jbconf.max_jitterbuf = maxjitterbuffer;
		jbconf.resync_threshold = resyncthreshold;
		jbconf.max_contig_interp = maxjitterinterps;
		jb_setconf(tmp->jb,&jbconf);
	}
#endif
	return tmp;
}

static struct iax_frame *iaxfrdup2(struct iax_frame *fr)
{
	/* Malloc() a copy of a frame */
	struct iax_frame *new = iax_frame_new(DIRECTION_INGRESS, fr->af.datalen);
	if (new) {
		memcpy(new, fr, sizeof(struct iax_frame));	
		iax_frame_wrap(new, &fr->af);
		new->data = NULL;
		new->datalen = 0;
		new->direction = DIRECTION_INGRESS;
		new->retrans = -1;
	}
	return new;
}

#define NEW_PREVENT 	0
#define NEW_ALLOW 	1
#define NEW_FORCE 	2

static int match(struct sockaddr_in *sin, unsigned short callno, unsigned short dcallno, struct chan_iax2_pvt *cur)
{
	if ((cur->addr.sin_addr.s_addr == sin->sin_addr.s_addr) &&
		(cur->addr.sin_port == sin->sin_port)) {
		/* This is the main host */
		if ((cur->peercallno == callno) ||
			((dcallno == cur->callno) && !cur->peercallno)) {
			/* That's us.  Be sure we keep track of the peer call number */
			return 1;
		}
	}
	if ((cur->transfer.sin_addr.s_addr == sin->sin_addr.s_addr) &&
	    (cur->transfer.sin_port == sin->sin_port) && (cur->transferring)) {
		/* We're transferring */
		if (dcallno == cur->callno)
			return 1;
	}
	return 0;
}

static void update_max_trunk(void)
{
	int max = TRUNK_CALL_START;
	int x;
	/* XXX Prolly don't need locks here XXX */
	for (x=TRUNK_CALL_START;x<IAX_MAX_CALLS - 1; x++) {
		if (iaxs[x])
			max = x + 1;
	}
	maxtrunkcall = max;
	if (option_debug && iaxdebug)
		ast_log(LOG_DEBUG, "New max trunk callno is %d\n", max);
}

static void update_max_nontrunk(void)
{
	int max = 1;
	int x;
	/* XXX Prolly don't need locks here XXX */
	for (x=1;x<TRUNK_CALL_START - 1; x++) {
		if (iaxs[x])
			max = x + 1;
	}
	maxnontrunkcall = max;
	if (option_debug && iaxdebug)
		ast_log(LOG_DEBUG, "New max nontrunk callno is %d\n", max);
}

static int make_trunk(unsigned short callno, int locked)
{
	int x;
	int res= 0;
	struct timeval now;
	if (iaxs[callno]->oseqno) {
		ast_log(LOG_WARNING, "Can't make trunk once a call has started!\n");
		return -1;
	}
	if (callno & TRUNK_CALL_START) {
		ast_log(LOG_WARNING, "Call %d is already a trunk\n", callno);
		return -1;
	}
	gettimeofday(&now, NULL);
	for (x=TRUNK_CALL_START;x<IAX_MAX_CALLS - 1; x++) {
		ast_mutex_lock(&iaxsl[x]);
		if (!iaxs[x] && ((now.tv_sec - lastused[x].tv_sec) > MIN_REUSE_TIME)) {
			iaxs[x] = iaxs[callno];
			iaxs[x]->callno = x;
			iaxs[callno] = NULL;
			/* Update the two timers that should have been started */
			if (iaxs[x]->pingid > -1)
				ast_sched_del(sched, iaxs[x]->pingid);
			if (iaxs[x]->lagid > -1)
				ast_sched_del(sched, iaxs[x]->lagid);
			iaxs[x]->pingid = ast_sched_add(sched, ping_time * 1000, send_ping, (void *)(long)x);
			iaxs[x]->lagid = ast_sched_add(sched, lagrq_time * 1000, send_lagrq, (void *)(long)x);
			if (locked)
				ast_mutex_unlock(&iaxsl[callno]);
			res = x;
			if (!locked)
				ast_mutex_unlock(&iaxsl[x]);
			break;
		}
		ast_mutex_unlock(&iaxsl[x]);
	}
	if (x >= IAX_MAX_CALLS - 1) {
		ast_log(LOG_WARNING, "Unable to trunk call: Insufficient space\n");
		return -1;
	}
	ast_log(LOG_DEBUG, "Made call %d into trunk call %d\n", callno, x);
	/* We move this call from a non-trunked to a trunked call */
	update_max_trunk();
	update_max_nontrunk();
	return res;
}

static int find_callno(unsigned short callno, unsigned short dcallno, struct sockaddr_in *sin, int new, int lockpeer, int sockfd)
{
	int res = 0;
	int x;
	struct timeval now;
	char iabuf[INET_ADDRSTRLEN];
	char host[80];
	if (new <= NEW_ALLOW) {
		/* Look for an existing connection first */
		for (x=1;(res < 1) && (x<maxnontrunkcall);x++) {
			ast_mutex_lock(&iaxsl[x]);
			if (iaxs[x]) {
				/* Look for an exact match */
				if (match(sin, callno, dcallno, iaxs[x])) {
					res = x;
				}
			}
			ast_mutex_unlock(&iaxsl[x]);
		}
		for (x=TRUNK_CALL_START;(res < 1) && (x<maxtrunkcall);x++) {
			ast_mutex_lock(&iaxsl[x]);
			if (iaxs[x]) {
				/* Look for an exact match */
				if (match(sin, callno, dcallno, iaxs[x])) {
					res = x;
				}
			}
			ast_mutex_unlock(&iaxsl[x]);
		}
	}
	if ((res < 1) && (new >= NEW_ALLOW)) {
		if (!iax2_getpeername(*sin, host, sizeof(host), lockpeer))
			snprintf(host, sizeof(host), "%s:%d", ast_inet_ntoa(iabuf, sizeof(iabuf), sin->sin_addr), ntohs(sin->sin_port));
		gettimeofday(&now, NULL);
		for (x=1;x<TRUNK_CALL_START;x++) {
			/* Find first unused call number that hasn't been used in a while */
			ast_mutex_lock(&iaxsl[x]);
			if (!iaxs[x] && ((now.tv_sec - lastused[x].tv_sec) > MIN_REUSE_TIME)) break;
			ast_mutex_unlock(&iaxsl[x]);
		}
		/* We've still got lock held if we found a spot */
		if (x >= TRUNK_CALL_START) {
			ast_log(LOG_WARNING, "No more space\n");
			return 0;
		}
		iaxs[x] = new_iax(sin, lockpeer, host);
		update_max_nontrunk();
		if (iaxs[x]) {
			if (option_debug && iaxdebug)
				ast_log(LOG_DEBUG, "Creating new call structure %d\n", x);
			iaxs[x]->sockfd = sockfd;
			iaxs[x]->addr.sin_port = sin->sin_port;
			iaxs[x]->addr.sin_family = sin->sin_family;
			iaxs[x]->addr.sin_addr.s_addr = sin->sin_addr.s_addr;
			iaxs[x]->peercallno = callno;
			iaxs[x]->callno = x;
			iaxs[x]->pingtime = DEFAULT_RETRY_TIME;
			iaxs[x]->expiry = min_reg_expire;
			iaxs[x]->pingid = ast_sched_add(sched, ping_time * 1000, send_ping, (void *)(long)x);
			iaxs[x]->lagid = ast_sched_add(sched, lagrq_time * 1000, send_lagrq, (void *)(long)x);
			iaxs[x]->amaflags = amaflags;
			ast_copy_flags(iaxs[x], (&globalflags), IAX_NOTRANSFER | IAX_USEJITTERBUF | IAX_FORCEJITTERBUF);	
			ast_copy_string(iaxs[x]->accountcode, accountcode, sizeof(iaxs[x]->accountcode));
		} else {
			ast_log(LOG_WARNING, "Out of resources\n");
			ast_mutex_unlock(&iaxsl[x]);
			return 0;
		}
		ast_mutex_unlock(&iaxsl[x]);
		res = x;
	}
	return res;
}

static void iax2_frame_free(struct iax_frame *fr)
{
	if (fr->retrans > -1)
		ast_sched_del(sched, fr->retrans);
	iax_frame_free(fr);
}

static int iax2_queue_frame(int callno, struct ast_frame *f)
{
	/* Assumes lock for callno is already held... */
	for (;;) {
		if (iaxs[callno] && iaxs[callno]->owner) {
			if (ast_mutex_trylock(&iaxs[callno]->owner->lock)) {
				/* Avoid deadlock by pausing and trying again */
				ast_mutex_unlock(&iaxsl[callno]);
				usleep(1);
				ast_mutex_lock(&iaxsl[callno]);
			} else {
				ast_queue_frame(iaxs[callno]->owner, f);
				ast_mutex_unlock(&iaxs[callno]->owner->lock);
				break;
			}
		} else
			break;
	}
	return 0;
}

static void destroy_firmware(struct iax_firmware *cur)
{
	/* Close firmware */
	if (cur->fwh) {
		munmap(cur->fwh, ntohl(cur->fwh->datalen) + sizeof(*(cur->fwh)));
	}
	close(cur->fd);
	free(cur);
}

static int try_firmware(char *s)
{
	struct stat stbuf;
	struct iax_firmware *cur;
	int ifd;
	int fd;
	int res;
	
	struct ast_iax2_firmware_header *fwh, fwh2;
	struct MD5Context md5;
	unsigned char sum[16];
	unsigned char buf[1024];
	int len, chunk;
	char *s2;
	char *last;
	s2 = alloca(strlen(s) + 100);
	if (!s2) {
		ast_log(LOG_WARNING, "Alloca failed!\n");
		return -1;
	}
	last = strrchr(s, '/');
	if (last)
		last++;
	else
		last = s;
	snprintf(s2, strlen(s) + 100, "/var/tmp/%s-%ld", last, (unsigned long)rand());
	res = stat(s, &stbuf);
	if (res < 0) {
		ast_log(LOG_WARNING, "Failed to stat '%s': %s\n", s, strerror(errno));
		return -1;
	}
	/* Make sure it's not a directory */
	if (S_ISDIR(stbuf.st_mode))
		return -1;
	ifd = open(s, O_RDONLY);
	if (ifd < 0) {
		ast_log(LOG_WARNING, "Cannot open '%s': %s\n", s, strerror(errno));
		return -1;
	}
	fd = open(s2, O_RDWR | O_CREAT | O_EXCL);
	if (fd < 0) {
		ast_log(LOG_WARNING, "Cannot open '%s' for writing: %s\n", s2, strerror(errno));
		close(ifd);
		return -1;
	}
	/* Unlink our newly created file */
	unlink(s2);
	
	/* Now copy the firmware into it */
	len = stbuf.st_size;
	while(len) {
		chunk = len;
		if (chunk > sizeof(buf))
			chunk = sizeof(buf);
		res = read(ifd, buf, chunk);
		if (res != chunk) {
			ast_log(LOG_WARNING, "Only read %d of %d bytes of data :(: %s\n", res, chunk, strerror(errno));
			close(ifd);
			close(fd);
			return -1;
		}
		res = write(fd, buf, chunk);
		if (res != chunk) {
			ast_log(LOG_WARNING, "Only write %d of %d bytes of data :(: %s\n", res, chunk, strerror(errno));
			close(ifd);
			close(fd);
			return -1;
		}
		len -= chunk;
	}
	close(ifd);
	/* Return to the beginning */
	lseek(fd, 0, SEEK_SET);
	if ((res = read(fd, &fwh2, sizeof(fwh2))) != sizeof(fwh2)) {
		ast_log(LOG_WARNING, "Unable to read firmware header in '%s'\n", s);
		close(fd);
		return -1;
	}
	if (ntohl(fwh2.magic) != IAX_FIRMWARE_MAGIC) {
		ast_log(LOG_WARNING, "'%s' is not a valid firmware file\n", s);
		close(fd);
		return -1;
	}
	if (ntohl(fwh2.datalen) != (stbuf.st_size - sizeof(fwh2))) {
		ast_log(LOG_WARNING, "Invalid data length in firmware '%s'\n", s);
		close(fd);
		return -1;
	}
	if (fwh2.devname[sizeof(fwh2.devname) - 1] || ast_strlen_zero((char *)fwh2.devname)) {
		ast_log(LOG_WARNING, "No or invalid device type specified for '%s'\n", s);
		close(fd);
		return -1;
	}
	fwh = mmap(NULL, stbuf.st_size, PROT_READ, MAP_PRIVATE, fd, 0); 
	if (!fwh) {
		ast_log(LOG_WARNING, "mmap failed: %s\n", strerror(errno));
		close(fd);
		return -1;
	}
	MD5Init(&md5);
	MD5Update(&md5, fwh->data, ntohl(fwh->datalen));
	MD5Final(sum, &md5);
	if (memcmp(sum, fwh->chksum, sizeof(sum))) {
		ast_log(LOG_WARNING, "Firmware file '%s' fails checksum\n", s);
		munmap(fwh, stbuf.st_size);
		close(fd);
		return -1;
	}
	cur = waresl.wares;
	while(cur) {
		if (!strcmp((char *)cur->fwh->devname, (char *)fwh->devname)) {
			/* Found a candidate */
			if (cur->dead || (ntohs(cur->fwh->version) < ntohs(fwh->version)))
				/* The version we have on loaded is older, load this one instead */
				break;
			/* This version is no newer than what we have.  Don't worry about it.
			   We'll consider it a proper load anyhow though */
			munmap(fwh, stbuf.st_size);
			close(fd);
			return 0;
		}
		cur = cur->next;
	}
	if (!cur) {
		/* Allocate a new one and link it */
		if ((cur = ast_calloc(1, sizeof(*cur)))) {
			cur->fd = -1;
			cur->next = waresl.wares;
			waresl.wares = cur;
		}
	}
	if (cur) {
		if (cur->fwh) {
			munmap(cur->fwh, cur->mmaplen);
		}
		if (cur->fd > -1)
			close(cur->fd);
		cur->fwh = fwh;
		cur->fd = fd;
		cur->mmaplen = stbuf.st_size;
		cur->dead = 0;
	}
	return 0;
}

static int iax_check_version(char *dev)
{
	int res = 0;
	struct iax_firmware *cur;
	if (!ast_strlen_zero(dev)) {
		ast_mutex_lock(&waresl.lock);
		cur = waresl.wares;
		while(cur) {
			if (!strcmp(dev, (char *)cur->fwh->devname)) {
				res = ntohs(cur->fwh->version);
				break;
			}
			cur = cur->next;
		}
		ast_mutex_unlock(&waresl.lock);
	}
	return res;
}

static int iax_firmware_append(struct iax_ie_data *ied, const unsigned char *dev, unsigned int desc)
{
	int res = -1;
	unsigned int bs = desc & 0xff;
	unsigned int start = (desc >> 8) & 0xffffff;
	unsigned int bytes;
	struct iax_firmware *cur;
	if (!ast_strlen_zero((char *)dev) && bs) {
		start *= bs;
		ast_mutex_lock(&waresl.lock);
		cur = waresl.wares;
		while(cur) {
			if (!strcmp((char *)dev, (char *)cur->fwh->devname)) {
				iax_ie_append_int(ied, IAX_IE_FWBLOCKDESC, desc);
				if (start < ntohl(cur->fwh->datalen)) {
					bytes = ntohl(cur->fwh->datalen) - start;
					if (bytes > bs)
						bytes = bs;
					iax_ie_append_raw(ied, IAX_IE_FWBLOCKDATA, cur->fwh->data + start, bytes);
				} else {
					bytes = 0;
					iax_ie_append(ied, IAX_IE_FWBLOCKDATA);
				}
				if (bytes == bs)
					res = 0;
				else
					res = 1;
				break;
			}
			cur = cur->next;
		}
		ast_mutex_unlock(&waresl.lock);
	}
	return res;
}


static void reload_firmware(void)
{
	struct iax_firmware *cur, *curl, *curp;
	DIR *fwd;
	struct dirent *de;
	char dir[256];
	char fn[256];
	/* Mark all as dead */
	ast_mutex_lock(&waresl.lock);
	cur = waresl.wares;
	while(cur) {
		cur->dead = 1;
		cur = cur->next;
	}
	/* Now that we've freed them, load the new ones */
	snprintf(dir, sizeof(dir), "%s/firmware/iax", (char *)ast_config_AST_VAR_DIR);
	fwd = opendir(dir);
	if (fwd) {
		while((de = readdir(fwd))) {
			if (de->d_name[0] != '.') {
				snprintf(fn, sizeof(fn), "%s/%s", dir, de->d_name);
				if (!try_firmware(fn)) {
					if (option_verbose > 1)
						ast_verbose(VERBOSE_PREFIX_2 "Loaded firmware '%s'\n", de->d_name);
				}
			}
		}
		closedir(fwd);
	} else 
		ast_log(LOG_WARNING, "Error opening firmware directory '%s': %s\n", dir, strerror(errno));

	/* Clean up leftovers */
	cur = waresl.wares;
	curp = NULL;
	while(cur) {
		curl = cur;
		cur = cur->next;
		if (curl->dead) {
			if (curp) {
				curp->next = cur;
			} else {
				waresl.wares = cur;
			}
			destroy_firmware(curl);
		} else {
			curp = cur;
		}
	}
	ast_mutex_unlock(&waresl.lock);
}

static int iax2_send(struct chan_iax2_pvt *pvt, struct ast_frame *f, unsigned int ts, int seqno, int now, int transfer, int final);

static int __do_deliver(void *data)
{
	/* Just deliver the packet by using queueing.  This is called by
	  the IAX thread with the iaxsl lock held. */
	struct iax_frame *fr = data;
	fr->retrans = -1;
	if (iaxs[fr->callno] && !ast_test_flag(iaxs[fr->callno], IAX_ALREADYGONE))
		iax2_queue_frame(fr->callno, &fr->af);
	/* Free our iax frame */
	iax2_frame_free(fr);
	/* And don't run again */
	return 0;
}

#ifndef NEWJB
static int do_deliver(void *data)
{
	/* Locking version of __do_deliver */
	struct iax_frame *fr = data;
	int callno = fr->callno;
	int res;
	ast_mutex_lock(&iaxsl[callno]);
	res = __do_deliver(data);
	ast_mutex_unlock(&iaxsl[callno]);
	return res;
}
#endif /* NEWJB */

static int handle_error(void)
{
	/* XXX Ideally we should figure out why an error occured and then abort those
	   rather than continuing to try.  Unfortunately, the published interface does
	   not seem to work XXX */
#if 0
	struct sockaddr_in *sin;
	int res;
	struct msghdr m;
	struct sock_extended_err e;
	m.msg_name = NULL;
	m.msg_namelen = 0;
	m.msg_iov = NULL;
	m.msg_control = &e;
	m.msg_controllen = sizeof(e);
	m.msg_flags = 0;
	res = recvmsg(netsocket, &m, MSG_ERRQUEUE);
	if (res < 0)
		ast_log(LOG_WARNING, "Error detected, but unable to read error: %s\n", strerror(errno));
	else {
		if (m.msg_controllen) {
			sin = (struct sockaddr_in *)SO_EE_OFFENDER(&e);
			if (sin) 
				ast_log(LOG_WARNING, "Receive error from %s\n", ast_inet_ntoa(iabuf, sizeof(iabuf), sin->sin_addr));
			else
				ast_log(LOG_WARNING, "No address detected??\n");
		} else {
			ast_log(LOG_WARNING, "Local error: %s\n", strerror(e.ee_errno));
		}
	}
#endif
	return 0;
}

static int transmit_trunk(struct iax_frame *f, struct sockaddr_in *sin, int sockfd)
{
	int res;
	res = sendto(sockfd, f->data, f->datalen, 0,(struct sockaddr *)sin,
					sizeof(*sin));
	if (res < 0) {
		if (option_debug)
			ast_log(LOG_DEBUG, "Received error: %s\n", strerror(errno));
		handle_error();
	} else
		res = 0;
	return res;
}

static int send_packet(struct iax_frame *f)
{
	int res;
	char iabuf[INET_ADDRSTRLEN];
	/* Called with iaxsl held */
	if (option_debug > 2 && iaxdebug)
		ast_log(LOG_DEBUG, "Sending %d on %d/%d to %s:%d\n", f->ts, f->callno, iaxs[f->callno]->peercallno, ast_inet_ntoa(iabuf, sizeof(iabuf), iaxs[f->callno]->addr.sin_addr), ntohs(iaxs[f->callno]->addr.sin_port));
	/* Don't send if there was an error, but return error instead */
	if (!f->callno) {
		ast_log(LOG_WARNING, "Call number = %d\n", f->callno);
		return -1;
	}
	if (!iaxs[f->callno])
		return -1;
	if (iaxs[f->callno]->error)
		return -1;
	if (f->transfer) {
		if (iaxdebug)
			iax_showframe(f, NULL, 0, &iaxs[f->callno]->transfer, f->datalen - sizeof(struct ast_iax2_full_hdr));
		res = sendto(iaxs[f->callno]->sockfd, f->data, f->datalen, 0,(struct sockaddr *)&iaxs[f->callno]->transfer,
					sizeof(iaxs[f->callno]->transfer));
	} else {
		if (iaxdebug)
			iax_showframe(f, NULL, 0, &iaxs[f->callno]->addr, f->datalen - sizeof(struct ast_iax2_full_hdr));
		res = sendto(iaxs[f->callno]->sockfd, f->data, f->datalen, 0,(struct sockaddr *)&iaxs[f->callno]->addr,
					sizeof(iaxs[f->callno]->addr));
	}
	if (res < 0) {
		if (option_debug && iaxdebug)
			ast_log(LOG_DEBUG, "Received error: %s\n", strerror(errno));
		handle_error();
	} else
		res = 0;
	return res;
}


static int iax2_predestroy(int callno)
{
	struct ast_channel *c;
	struct chan_iax2_pvt *pvt;
	ast_mutex_lock(&iaxsl[callno]);
	pvt = iaxs[callno];
	if (!pvt) {
		ast_mutex_unlock(&iaxsl[callno]);
		return -1;
	}
	if (!ast_test_flag(pvt, IAX_ALREADYGONE)) {
		/* No more pings or lagrq's */
		if (pvt->pingid > -1)
			ast_sched_del(sched, pvt->pingid);
		if (pvt->lagid > -1)
			ast_sched_del(sched, pvt->lagid);
		if (pvt->autoid > -1)
			ast_sched_del(sched, pvt->autoid);
		if (pvt->authid > -1)
			ast_sched_del(sched, pvt->authid);
		if (pvt->initid > -1)
			ast_sched_del(sched, pvt->initid);
#ifdef NEWJB
		if (pvt->jbid > -1)
			ast_sched_del(sched, pvt->jbid);
		pvt->jbid = -1;
#endif
		pvt->pingid = -1;
		pvt->lagid = -1;
		pvt->autoid = -1;
		pvt->initid = -1;
		pvt->authid = -1;
		ast_set_flag(pvt, IAX_ALREADYGONE);	
	}
	c = pvt->owner;
	if (c) {
		c->_softhangup |= AST_SOFTHANGUP_DEV;
		c->tech_pvt = NULL;
		ast_queue_hangup(c);
		pvt->owner = NULL;
		ast_mutex_lock(&usecnt_lock);
		usecnt--;
		if (usecnt < 0) 
			ast_log(LOG_WARNING, "Usecnt < 0???\n");
		ast_mutex_unlock(&usecnt_lock);
	}
	ast_mutex_unlock(&iaxsl[callno]);
	ast_update_use_count();
	return 0;
}

static int iax2_predestroy_nolock(int callno)
{
	int res;
	ast_mutex_unlock(&iaxsl[callno]);
	res = iax2_predestroy(callno);
	ast_mutex_lock(&iaxsl[callno]);
	return res;
}

static void iax2_destroy(int callno)
{
	struct chan_iax2_pvt *pvt;
	struct iax_frame *cur;
	struct ast_channel *owner;

retry:
	ast_mutex_lock(&iaxsl[callno]);
	pvt = iaxs[callno];
	gettimeofday(&lastused[callno], NULL);

	if (pvt)
		owner = pvt->owner;
	else
		owner = NULL;
	if (owner) {
		if (ast_mutex_trylock(&owner->lock)) {
			ast_log(LOG_NOTICE, "Avoiding IAX destroy deadlock\n");
			ast_mutex_unlock(&iaxsl[callno]);
			usleep(1);
			goto retry;
		}
	}
	if (!owner)
		iaxs[callno] = NULL;
	if (pvt) {
		if (!owner)
			pvt->owner = NULL;
		/* No more pings or lagrq's */
		if (pvt->pingid > -1)
			ast_sched_del(sched, pvt->pingid);
		if (pvt->lagid > -1)
			ast_sched_del(sched, pvt->lagid);
		if (pvt->autoid > -1)
			ast_sched_del(sched, pvt->autoid);
		if (pvt->authid > -1)
			ast_sched_del(sched, pvt->authid);
		if (pvt->initid > -1)
			ast_sched_del(sched, pvt->initid);
#ifdef NEWJB
		if (pvt->jbid > -1)
			ast_sched_del(sched, pvt->jbid);
		pvt->jbid = -1;
#endif
		pvt->pingid = -1;
		pvt->lagid = -1;
		pvt->autoid = -1;
		pvt->authid = -1;
		pvt->initid = -1;
		if (pvt->bridgetrans)
			ast_translator_free_path(pvt->bridgetrans);
		pvt->bridgetrans = NULL;

		/* Already gone */
		ast_set_flag(pvt, IAX_ALREADYGONE);	

		if (owner) {
			/* If there's an owner, prod it to give up */
			owner->_softhangup |= AST_SOFTHANGUP_DEV;
			ast_queue_hangup(owner);
		}

		for (cur = iaxq.head; cur ; cur = cur->next) {
			/* Cancel any pending transmissions */
			if (cur->callno == pvt->callno) 
				cur->retries = -1;
		}
		if (pvt->reg) {
			pvt->reg->callno = 0;
		}
		if (!owner) {
			if (pvt->vars) {
				ast_variables_destroy(pvt->vars);
				pvt->vars = NULL;
			}
#ifdef NEWJB
 			{
                            jb_frame frame;
                            while(jb_getall(pvt->jb,&frame) == JB_OK)
				iax2_frame_free(frame.data);
                            jb_destroy(pvt->jb);
                        }
#endif
			free(pvt);
		}
	}
	if (owner) {
		ast_mutex_unlock(&owner->lock);
	}
	ast_mutex_unlock(&iaxsl[callno]);
	if (callno & 0x4000)
		update_max_trunk();
}
static void iax2_destroy_nolock(int callno)
{	
	/* Actually it's easier to unlock, kill it, and relock */
	ast_mutex_unlock(&iaxsl[callno]);
	iax2_destroy(callno);
	ast_mutex_lock(&iaxsl[callno]);
}

static int update_packet(struct iax_frame *f)
{
	/* Called with iaxsl lock held, and iaxs[callno] non-NULL */
	struct ast_iax2_full_hdr *fh = f->data;
	/* Mark this as a retransmission */
	fh->dcallno = ntohs(IAX_FLAG_RETRANS | f->dcallno);
	/* Update iseqno */
	f->iseqno = iaxs[f->callno]->iseqno;
	fh->iseqno = f->iseqno;
	return 0;
}

static int attempt_transmit(void *data)
{
	/* Attempt to transmit the frame to the remote peer...
	   Called without iaxsl held. */
	struct iax_frame *f = data;
	int freeme=0;
	int callno = f->callno;
	char iabuf[INET_ADDRSTRLEN];
	/* Make sure this call is still active */
	if (callno) 
		ast_mutex_lock(&iaxsl[callno]);
	if ((f->callno) && iaxs[f->callno]) {
		if ((f->retries < 0) /* Already ACK'd */ ||
		    (f->retries >= max_retries) /* Too many attempts */) {
				/* Record an error if we've transmitted too many times */
				if (f->retries >= max_retries) {
					if (f->transfer) {
						/* Transfer timeout */
						send_command(iaxs[f->callno], AST_FRAME_IAX, IAX_COMMAND_TXREJ, 0, NULL, 0, -1);
					} else if (f->final) {
						if (f->final) 
							iax2_destroy_nolock(f->callno);
					} else {
						if (iaxs[f->callno]->owner)
							ast_log(LOG_WARNING, "Max retries exceeded to host %s on %s (type = %d, subclass = %d, ts=%d, seqno=%d)\n", ast_inet_ntoa(iabuf, sizeof(iabuf), iaxs[f->callno]->addr.sin_addr),iaxs[f->callno]->owner->name , f->af.frametype, f->af.subclass, f->ts, f->oseqno);
						iaxs[f->callno]->error = ETIMEDOUT;
						if (iaxs[f->callno]->owner) {
							struct ast_frame fr = { 0, };
							/* Hangup the fd */
							fr.frametype = AST_FRAME_CONTROL;
							fr.subclass = AST_CONTROL_HANGUP;
							iax2_queue_frame(f->callno, &fr);
							/* Remember, owner could disappear */
							if (iaxs[f->callno]->owner)
								iaxs[f->callno]->owner->hangupcause = AST_CAUSE_DESTINATION_OUT_OF_ORDER;
						} else {
							if (iaxs[f->callno]->reg) {
								memset(&iaxs[f->callno]->reg->us, 0, sizeof(iaxs[f->callno]->reg->us));
								iaxs[f->callno]->reg->regstate = REG_STATE_TIMEOUT;
								iaxs[f->callno]->reg->refresh = IAX_DEFAULT_REG_EXPIRE;
							}
							iax2_destroy_nolock(f->callno);
						}
					}

				}
				freeme++;
		} else {
			/* Update it if it needs it */
			update_packet(f);
			/* Attempt transmission */
			send_packet(f);
			f->retries++;
			/* Try again later after 10 times as long */
			f->retrytime *= 10;
			if (f->retrytime > MAX_RETRY_TIME)
				f->retrytime = MAX_RETRY_TIME;
			/* Transfer messages max out at one second */
			if (f->transfer && (f->retrytime > 1000))
				f->retrytime = 1000;
			f->retrans = ast_sched_add(sched, f->retrytime, attempt_transmit, f);
		}
	} else {
		/* Make sure it gets freed */
		f->retries = -1;
		freeme++;
	}
	if (callno)
		ast_mutex_unlock(&iaxsl[callno]);
	/* Do not try again */
	if (freeme) {
		/* Don't attempt delivery, just remove it from the queue */
		ast_mutex_lock(&iaxq.lock);
		if (f->prev) 
			f->prev->next = f->next;
		else
			iaxq.head = f->next;
		if (f->next)
			f->next->prev = f->prev;
		else
			iaxq.tail = f->prev;
		iaxq.count--;
		ast_mutex_unlock(&iaxq.lock);
		f->retrans = -1;
		/* Free the IAX frame */
		iax2_frame_free(f);
	}
	return 0;
}

static int iax2_set_jitter(int fd, int argc, char *argv[])
{
#ifdef NEWJB
	ast_cli(fd, "sorry, this command is deprecated\n");
	return RESULT_SUCCESS;
#else
	if ((argc != 4) && (argc != 5))
		return RESULT_SHOWUSAGE;
	if (argc == 4) {
		max_jitter_buffer = atoi(argv[3]);
		if (max_jitter_buffer < 0)
			max_jitter_buffer = 0;
	} else {
		if (argc == 5) {
			if ((atoi(argv[3]) >= 0) && (atoi(argv[3]) < IAX_MAX_CALLS)) {
				if (iaxs[atoi(argv[3])]) {
					iaxs[atoi(argv[3])]->jitterbuffer = atoi(argv[4]);
					if (iaxs[atoi(argv[3])]->jitterbuffer < 0)
						iaxs[atoi(argv[3])]->jitterbuffer = 0;
				} else
					ast_cli(fd, "No such call '%d'\n", atoi(argv[3]));
			} else
				ast_cli(fd, "%d is not a valid call number\n", atoi(argv[3]));
		}
	}
	return RESULT_SUCCESS;
#endif
}

static char jitter_usage[] = 
"Usage: iax set jitter [callid] <value>\n"
"       If used with a callid, it sets the jitter buffer to the given static\n"
"value (until its next calculation).  If used without a callid, the value is used\n"
"to establish the maximum excess jitter buffer that is permitted before the jitter\n"
"buffer size is reduced.";

static int iax2_prune_realtime(int fd, int argc, char *argv[])
{
	struct iax2_peer *peer;

	if (argc != 4)
        return RESULT_SHOWUSAGE;
	if (!strcmp(argv[3],"all")) {
		reload_config();
		ast_cli(fd, "OK cache is flushed.\n");
	} else if ((peer = find_peer(argv[3], 0))) {
		if(ast_test_flag(peer, IAX_RTCACHEFRIENDS)) {
			ast_set_flag(peer, IAX_RTAUTOCLEAR);
			expire_registry(peer);
			ast_cli(fd, "OK peer %s was removed from the cache.\n", argv[3]);
		} else {
			ast_cli(fd, "SORRY peer %s is not eligible for this operation.\n", argv[3]);
		}
	} else {
		ast_cli(fd, "SORRY peer %s was not found in the cache.\n", argv[3]);
	}
	
	return RESULT_SUCCESS;
}

static int iax2_test_losspct(int fd, int argc, char *argv[])
{
       if (argc != 4)
               return RESULT_SHOWUSAGE;

       test_losspct = atoi(argv[3]);

       return RESULT_SUCCESS;
}

#ifdef IAXTESTS
static int iax2_test_late(int fd, int argc, char *argv[])
{
	if (argc != 4)
		return RESULT_SHOWUSAGE;

	test_late = atoi(argv[3]);

	return RESULT_SUCCESS;
}

static int iax2_test_resync(int fd, int argc, char *argv[])
{
	if (argc != 4)
		return RESULT_SHOWUSAGE;

	test_resync = atoi(argv[3]);

	return RESULT_SUCCESS;
}

static int iax2_test_jitter(int fd, int argc, char *argv[])
{
	if (argc < 4 || argc > 5)
		return RESULT_SHOWUSAGE;

	test_jit = atoi(argv[3]);
	if (argc == 5) 
		test_jitpct = atoi(argv[4]);

	return RESULT_SUCCESS;
}
#endif /* IAXTESTS */

/*! \brief  peer_status: Report Peer status in character string */
/* 	returns 1 if peer is online, -1 if unmonitored */
static int peer_status(struct iax2_peer *peer, char *status, int statuslen)
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
		res = -1;
	}
	return res;
}

/*! \brief Show one peer in detail */
static int iax2_show_peer(int fd, int argc, char *argv[])
{
	char status[30];
	char cbuf[256];
	char iabuf[INET_ADDRSTRLEN];
	struct iax2_peer *peer;
	char codec_buf[512];
	int x = 0, codec = 0, load_realtime = 0;

	if (argc < 4)
		return RESULT_SHOWUSAGE;

	load_realtime = (argc == 5 && !strcmp(argv[4], "load")) ? 1 : 0;

	peer = find_peer(argv[3], load_realtime);
	if (peer) {
		ast_cli(fd,"\n\n");
		ast_cli(fd, "  * Name       : %s\n", peer->name);
		ast_cli(fd, "  Secret       : %s\n", ast_strlen_zero(peer->secret)?"<Not set>":"<Set>");
		ast_cli(fd, "  Context      : %s\n", peer->context);
		ast_cli(fd, "  Mailbox      : %s\n", peer->mailbox);
		ast_cli(fd, "  Dynamic      : %s\n", ast_test_flag(peer, IAX_DYNAMIC) ? "Yes":"No");
		ast_cli(fd, "  Callerid     : %s\n", ast_callerid_merge(cbuf, sizeof(cbuf), peer->cid_name, peer->cid_num, "<unspecified>"));
		ast_cli(fd, "  Expire       : %d\n", peer->expire);
		ast_cli(fd, "  ACL          : %s\n", (peer->ha?"Yes":"No"));
		ast_cli(fd, "  Addr->IP     : %s Port %d\n",  peer->addr.sin_addr.s_addr ? ast_inet_ntoa(iabuf, sizeof(iabuf), peer->addr.sin_addr) : "(Unspecified)", ntohs(peer->addr.sin_port));
		ast_cli(fd, "  Defaddr->IP  : %s Port %d\n", ast_inet_ntoa(iabuf, sizeof(iabuf), peer->defaddr.sin_addr), ntohs(peer->defaddr.sin_port));
		ast_cli(fd, "  Username     : %s\n", peer->username);
		ast_cli(fd, "  Codecs       : ");
		ast_getformatname_multiple(codec_buf, sizeof(codec_buf) -1, peer->capability);
		ast_cli(fd, "%s\n", codec_buf);

		ast_cli(fd, "  Codec Order  : (");
		for(x = 0; x < 32 ; x++) {
			codec = ast_codec_pref_index(&peer->prefs,x);
			if(!codec)
				break;
			ast_cli(fd, "%s", ast_getformatname(codec));
			if(x < 31 && ast_codec_pref_index(&peer->prefs,x+1))
				ast_cli(fd, "|");
		}

		if (!x)
			ast_cli(fd, "none");
		ast_cli(fd, ")\n");

		ast_cli(fd, "  Status       : ");
		peer_status(peer, status, sizeof(status));	
		ast_cli(fd, "%s\n",status);
		ast_cli(fd, "  Qualify      : every %d when OK, every %d when UNREACHABLE (sample smoothing %s)\n", peer->pokefreqok, peer->pokefreqnotok, (peer->smoothing == 1) ? "On" : "Off");
		ast_cli(fd,"\n");
		if (ast_test_flag(peer, IAX_TEMPONLY))
			destroy_peer(peer);
	} else {
		ast_cli(fd,"Peer %s not found.\n", argv[3]);
		ast_cli(fd,"\n");
	}

	return RESULT_SUCCESS;
}

static char *complete_iax2_show_peer(const char *line, const char *word, int pos, int state)
{
	int which = 0;
	struct iax2_peer *p;
	char *res = NULL;
	int wordlen = strlen(word);

	/* 0 - iax2; 1 - show; 2 - peer; 3 - <peername> */
	if (pos == 3) {
		ast_mutex_lock(&peerl.lock);
		for (p = peerl.peers ; p ; p = p->next) {
			if (!strncasecmp(p->name, word, wordlen)) {
				if (++which > state) {
					res = ast_strdup(p->name);
					break;
				}
			}
		}
		ast_mutex_unlock(&peerl.lock);
	}

	return res;
}

static int iax2_show_stats(int fd, int argc, char *argv[])
{
	struct iax_frame *cur;
	int cnt = 0, dead=0, final=0;
	if (argc != 3)
		return RESULT_SHOWUSAGE;
	for (cur = iaxq.head; cur ; cur = cur->next) {
		if (cur->retries < 0)
			dead++;
		if (cur->final)
			final++;
		cnt++;
	}
	ast_cli(fd, "    IAX Statistics\n");
	ast_cli(fd, "---------------------\n");
	ast_cli(fd, "Outstanding frames: %d (%d ingress, %d egress)\n", iax_get_frames(), iax_get_iframes(), iax_get_oframes());
	ast_cli(fd, "Packets in transmit queue: %d dead, %d final, %d total\n", dead, final, cnt);
	return RESULT_SUCCESS;
}

static int iax2_show_cache(int fd, int argc, char *argv[])
{
	struct iax2_dpcache *dp;
	char tmp[1024], *pc;
	int s;
	int x,y;
	struct timeval tv;
	gettimeofday(&tv, NULL);
	ast_mutex_lock(&dpcache_lock);
	dp = dpcache;
	ast_cli(fd, "%-20.20s %-12.12s %-9.9s %-8.8s %s\n", "Peer/Context", "Exten", "Exp.", "Wait.", "Flags");
	while(dp) {
		s = dp->expiry.tv_sec - tv.tv_sec;
		tmp[0] = '\0';
		if (dp->flags & CACHE_FLAG_EXISTS)
			strncat(tmp, "EXISTS|", sizeof(tmp) - strlen(tmp) - 1);
		if (dp->flags & CACHE_FLAG_NONEXISTENT)
			strncat(tmp, "NONEXISTENT|", sizeof(tmp) - strlen(tmp) - 1);
		if (dp->flags & CACHE_FLAG_CANEXIST)
			strncat(tmp, "CANEXIST|", sizeof(tmp) - strlen(tmp) - 1);
		if (dp->flags & CACHE_FLAG_PENDING)
			strncat(tmp, "PENDING|", sizeof(tmp) - strlen(tmp) - 1);
		if (dp->flags & CACHE_FLAG_TIMEOUT)
			strncat(tmp, "TIMEOUT|", sizeof(tmp) - strlen(tmp) - 1);
		if (dp->flags & CACHE_FLAG_TRANSMITTED)
			strncat(tmp, "TRANSMITTED|", sizeof(tmp) - strlen(tmp) - 1);
		if (dp->flags & CACHE_FLAG_MATCHMORE)
			strncat(tmp, "MATCHMORE|", sizeof(tmp) - strlen(tmp) - 1);
		if (dp->flags & CACHE_FLAG_UNKNOWN)
			strncat(tmp, "UNKNOWN|", sizeof(tmp) - strlen(tmp) - 1);
		/* Trim trailing pipe */
		if (!ast_strlen_zero(tmp))
			tmp[strlen(tmp) - 1] = '\0';
		else
			ast_copy_string(tmp, "(none)", sizeof(tmp));
		y=0;
		pc = strchr(dp->peercontext, '@');
		if (!pc)
			pc = dp->peercontext;
		else
			pc++;
		for (x=0;x<sizeof(dp->waiters) / sizeof(dp->waiters[0]); x++)
			if (dp->waiters[x] > -1)
				y++;
		if (s > 0)
			ast_cli(fd, "%-20.20s %-12.12s %-9d %-8d %s\n", pc, dp->exten, s, y, tmp);
		else
			ast_cli(fd, "%-20.20s %-12.12s %-9.9s %-8d %s\n", pc, dp->exten, "(expired)", y, tmp);
		dp = dp->next;
	}
	ast_mutex_unlock(&dpcache_lock);
	return RESULT_SUCCESS;
}

static unsigned int calc_rxstamp(struct chan_iax2_pvt *p, unsigned int offset);

#ifdef BRIDGE_OPTIMIZATION
static unsigned int calc_fakestamp(struct chan_iax2_pvt *from, struct chan_iax2_pvt *to, unsigned int ts);

static int forward_delivery(struct iax_frame *fr)
{
	struct chan_iax2_pvt *p1, *p2;
	char iabuf[INET_ADDRSTRLEN];
	int res, orig_ts;

	p1 = iaxs[fr->callno];
	p2 = iaxs[p1->bridgecallno];
	if (!p1)
		return -1;
	if (!p2)
		return -1;

	if (option_debug)
		ast_log(LOG_DEBUG, "forward_delivery: Forwarding ts=%d on %d/%d to %d/%d on %s:%d\n",
				fr->ts,
				p1->callno, p1->peercallno,
				p2->callno, p2->peercallno,
				ast_inet_ntoa(iabuf, sizeof(iabuf), p2->addr.sin_addr),
				ntohs(p2->addr.sin_port));

	/* Undo wraparound - which can happen when full VOICE frame wasn't sent by our peer.
	   This is necessary for when our peer is chan_iax2.c v1.1nn or earlier which didn't
	   send full frame on timestamp wrap when doing optimized bridging
	   (actually current code STILL doesn't)
	*/
	if (fr->ts + 50000 <= p1->last) {
		fr->ts = ( (p1->last & 0xFFFF0000) + 0x10000) | (fr->ts & 0xFFFF);
		if (option_debug)
			ast_log(LOG_DEBUG, "forward_delivery: pushed forward timestamp to %u\n", fr->ts);
	}

	/* Send with timestamp adjusted to the origin of the outbound leg */
	/* But don't destroy inbound timestamp still needed later to set "last" */
	orig_ts = fr->ts;
	fr->ts = calc_fakestamp(p1, p2, fr->ts);
	res = iax2_send(p2, &fr->af, fr->ts, -1, 0, 0, 0);
	fr->ts = orig_ts;
	return res;
}
#endif

static void unwrap_timestamp(struct iax_frame *fr)
{
	int x;

	if ( (fr->ts & 0xFFFF0000) == (iaxs[fr->callno]->last & 0xFFFF0000) ) {
		x = fr->ts - iaxs[fr->callno]->last;
		if (x < -50000) {
			/* Sudden big jump backwards in timestamp:
			   What likely happened here is that miniframe timestamp has circled but we haven't
			   gotten the update from the main packet.  We'll just pretend that we did, and
			   update the timestamp appropriately. */
			fr->ts = ( (iaxs[fr->callno]->last & 0xFFFF0000) + 0x10000) | (fr->ts & 0xFFFF);
			if (option_debug && iaxdebug)
				ast_log(LOG_DEBUG, "schedule_delivery: pushed forward timestamp\n");
		}
		if (x > 50000) {
			/* Sudden apparent big jump forwards in timestamp:
			   What's likely happened is this is an old miniframe belonging to the previous
			   top-16-bit timestamp that has turned up out of order.
			   Adjust the timestamp appropriately. */
			fr->ts = ( (iaxs[fr->callno]->last & 0xFFFF0000) - 0x10000) | (fr->ts & 0xFFFF);
			if (option_debug && iaxdebug)
				ast_log(LOG_DEBUG, "schedule_delivery: pushed back timestamp\n");
		}
	}
}

#ifdef NEWJB
static int get_from_jb(void *p);

static void update_jbsched(struct chan_iax2_pvt *pvt) {
    int when;

    when = ast_tvdiff_ms(ast_tvnow(), pvt->rxcore);

    /*    fprintf(stderr, "now = %d, next=%d\n", when, jb_next(pvt->jb)); */

    when = jb_next(pvt->jb) - when;
    /*   fprintf(stderr, "when = %d\n", when); */

    if(pvt->jbid > -1) ast_sched_del(sched, pvt->jbid);

    if(when <= 0) {
      /* XXX should really just empty until when > 0.. */
      when = 1;
    }

    pvt->jbid = ast_sched_add(sched, when, get_from_jb, (void *)pvt);
}

static int get_from_jb(void *p) 
{
	/* make sure pvt is valid! */	
    struct chan_iax2_pvt *pvt = p;
    struct iax_frame *fr;
    jb_frame frame;
    int ret;
    long now;
    long next;
    struct timeval tv;

    ast_mutex_lock(&iaxsl[pvt->callno]);
    /*  fprintf(stderr, "get_from_jb called\n"); */
    pvt->jbid = -1;

    gettimeofday(&tv,NULL);
    /* round up a millisecond since ast_sched_runq does; */
    /* prevents us from spinning while waiting for our now */
    /* to catch up with runq's now */
    tv.tv_usec += 1000;

    now = ast_tvdiff_ms(tv, pvt->rxcore);

    if(now >= (next = jb_next(pvt->jb))) {
		ret = jb_get(pvt->jb,&frame,now,ast_codec_interp_len(pvt->voiceformat));
		switch(ret) {
		case JB_OK:
			/*if(frame.type == JB_TYPE_VOICE && next + 20 != jb_next(pvt->jb)) fprintf(stderr, "NEXT %ld is not %ld+20!\n", jb_next(pvt->jb), next); */
			fr = frame.data;
			__do_deliver(fr);
		    break;
		case JB_INTERP:
		    {
			struct ast_frame af;
	
			/*if(next + 20 != jb_next(pvt->jb)) fprintf(stderr, "NEXT %ld is not %ld+20!\n", jb_next(pvt->jb), next); */
	
			/* create an interpolation frame */
			/*fprintf(stderr, "Making Interpolation frame\n"); */
			af.frametype = AST_FRAME_VOICE;
			af.subclass = pvt->voiceformat;
			af.datalen  = 0;
			af.samples  = frame.ms * 8;
			af.mallocd  = 0;
			af.src  = "IAX2 JB interpolation";
			af.data  = NULL;
			af.delivery = ast_tvadd(pvt->rxcore, ast_samp2tv(next, 1000));
			af.offset=AST_FRIENDLY_OFFSET;
	
			/* queue the frame:  For consistency, we would call __do_deliver here, but __do_deliver wants an iax_frame,
			 * which we'd need to malloc, and then it would free it.  That seems like a drag */
			if (iaxs[pvt->callno] && !ast_test_flag(iaxs[pvt->callno], IAX_ALREADYGONE))
				iax2_queue_frame(pvt->callno, &af);
		    }
		    break;
		case JB_DROP:
			/*if(next != jb_next(pvt->jb)) fprintf(stderr, "NEXT %ld is not next %ld!\n", jb_next(pvt->jb), next); */
			iax2_frame_free(frame.data);
		    break;
		case JB_NOFRAME:
		case JB_EMPTY:
			/* do nothing */
		    break;
		default:
			/* shouldn't happen */
		    break;
		}
    }
    update_jbsched(pvt);
    ast_mutex_unlock(&iaxsl[pvt->callno]);
    return 0;
}
#endif

/* while we transition from the old JB to the new one, we can either make two schedule_delivery functions, or 
 * make preprocessor swiss-cheese out of this one.  I'm not sure which is less revolting.. */
static int schedule_delivery(struct iax_frame *fr, int updatehistory, int fromtrunk)
{
#ifdef NEWJB
	int type, len;
	int ret;
#else
	int x;
	int ms;
	int delay;
	unsigned int orig_ts;
	int drops[MEMORY_SIZE];
	int min, max=0, prevjitterbuffer, maxone=0,y,z, match;

	/* Remember current jitterbuffer so we can log any change */
	prevjitterbuffer = iaxs[fr->callno]->jitterbuffer;
	/* Similarly for the frame timestamp */
	orig_ts = fr->ts;
#endif

#if 0
	if (option_debug && iaxdebug)
		ast_log(LOG_DEBUG, "schedule_delivery: ts=%d, last=%d, update=%d\n",
				fr->ts, iaxs[fr->callno]->last, updatehistory);
#endif

	/* Attempt to recover wrapped timestamps */
	unwrap_timestamp(fr);

	if (updatehistory) {
#ifndef NEWJB

		/* Attempt to spot a change of timebase on timestamps coming from the other side
		   We detect by noticing a jump in consecutive timestamps that can't reasonably be explained
		   by network jitter or reordering.  Sometimes, also, the peer stops sending us frames
		   for a while - in this case this code might also resync us.  But that's not a bad thing.
		   Be careful of non-voice frames which are timestamped differently (especially ACKS!)
		   [that's why we only do this when updatehistory is true]
		*/
		x = fr->ts - iaxs[fr->callno]->last;
		if (x > TS_GAP_FOR_JB_RESYNC || x < -TS_GAP_FOR_JB_RESYNC) {
			if (option_debug && iaxdebug)
				ast_log(LOG_DEBUG, "schedule_delivery: call=%d: TS jumped.  resyncing rxcore (ts=%d, last=%d)\n",
							fr->callno, fr->ts, iaxs[fr->callno]->last);
			/* zap rxcore - calc_rxstamp will make a new one based on this frame */
			iaxs[fr->callno]->rxcore = ast_tv(0, 0);
			/* wipe "last" if stamps have jumped backwards */
			if (x<0)
				iaxs[fr->callno]->last = 0;
			/* should we also empty history? */
		}
		/* ms is a measure of the "lateness" of the frame relative to the "reference"
		   frame we received.  (initially the very first, but also see code just above here).
		   Understand that "ms" can easily be -ve if lag improves since the reference frame.
		   Called by IAX thread, with iaxsl lock held. */
		ms = calc_rxstamp(iaxs[fr->callno], fr->ts) - fr->ts;
	
		/* Rotate our history queue of "lateness".  Don't worry about those initial
		   zeros because the first entry will always be zero */
		for (x=0;x<MEMORY_SIZE - 1;x++) 
			iaxs[fr->callno]->history[x] = iaxs[fr->callno]->history[x+1];
		/* Add a history entry for this one */
		iaxs[fr->callno]->history[x] = ms;
#endif
	}
#ifndef NEWJB
	else
		ms = 0;
#endif


	/* delivery time is sender's sent timestamp converted back into absolute time according to our clock */
	if ( !fromtrunk && !ast_tvzero(iaxs[fr->callno]->rxcore))
		fr->af.delivery = ast_tvadd(iaxs[fr->callno]->rxcore, ast_samp2tv(fr->ts, 1000));
	else {
#if 0
		ast_log(LOG_DEBUG, "schedule_delivery: set delivery to 0 as we don't have an rxcore yet, or frame is from trunk.\n");
#endif
		fr->af.delivery = ast_tv(0,0);
	}

#ifndef NEWJB
	/* Initialize the minimum to reasonable values.  It's too much
	   work to do the same for the maximum, repeatedly */
	min=iaxs[fr->callno]->history[0];
	for (z=0;z < iax2_dropcount + 1;z++) {
		/* Start very optimistic ;-) */
		max=-999999999;
		for (x=0;x<MEMORY_SIZE;x++) {
			if (max < iaxs[fr->callno]->history[x]) {
				/* We have a candidate new maximum value.  Make
				   sure it's not in our drop list */
				match = 0;
				for (y=0;!match && (y<z);y++)
					match |= (drops[y] == x);
				if (!match) {
					/* It's not in our list, use it as the new maximum */
					max = iaxs[fr->callno]->history[x];
					maxone = x;
				}
				
			}
			if (!z) {
				/* On our first pass, find the minimum too */
				if (min > iaxs[fr->callno]->history[x])
					min = iaxs[fr->callno]->history[x];
			}
		}
#if 1
		drops[z] = maxone;
#endif
	}
#endif

#ifdef NEWJB
	type = JB_TYPE_CONTROL;
	len = 0;

	if(fr->af.frametype == AST_FRAME_VOICE) {
		type = JB_TYPE_VOICE;
		len = ast_codec_get_samples(&fr->af) / 8;
	} else if(fr->af.frametype == AST_FRAME_CNG) {
		type = JB_TYPE_SILENCE;
	}

	if ( (!ast_test_flag(iaxs[fr->callno], IAX_USEJITTERBUF)) ) {
		__do_deliver(fr);
		return 0;
	}

	/* if the user hasn't requested we force the use of the jitterbuffer, and we're bridged to
	 * a channel that can accept jitter, then flush and suspend the jb, and send this frame straight through */
	if( (!ast_test_flag(iaxs[fr->callno], IAX_FORCEJITTERBUF)) &&
	    iaxs[fr->callno]->owner && ast_bridged_channel(iaxs[fr->callno]->owner) &&
	    (ast_bridged_channel(iaxs[fr->callno]->owner)->tech->properties & AST_CHAN_TP_WANTSJITTER)) {
                jb_frame frame;

                /* deliver any frames in the jb */
                while(jb_getall(iaxs[fr->callno]->jb,&frame) == JB_OK)
                        __do_deliver(frame.data);

		jb_reset(iaxs[fr->callno]->jb);

		if (iaxs[fr->callno]->jbid > -1)
                        ast_sched_del(sched, iaxs[fr->callno]->jbid);

		iaxs[fr->callno]->jbid = -1;

		/* deliver this frame now */
		__do_deliver(fr);
		return 0;

	}


	/* insert into jitterbuffer */
	/* TODO: Perhaps we could act immediately if it's not droppable and late */
	ret = jb_put(iaxs[fr->callno]->jb, fr, type, len, fr->ts,
			calc_rxstamp(iaxs[fr->callno],fr->ts));
	if (ret == JB_DROP) {
		iax2_frame_free(fr);
	} else if (ret == JB_SCHED) {
		update_jbsched(iaxs[fr->callno]);
	}
#else
	/* Just for reference, keep the "jitter" value, the difference between the
	   earliest and the latest. */
	if (max >= min)
		iaxs[fr->callno]->jitter = max - min;	
	
	/* IIR filter for keeping track of historic jitter, but always increase
	   historic jitter immediately for increase */
	
	if (iaxs[fr->callno]->jitter > iaxs[fr->callno]->historicjitter )
		iaxs[fr->callno]->historicjitter = iaxs[fr->callno]->jitter;
	else
		iaxs[fr->callno]->historicjitter = GAMMA * (double)iaxs[fr->callno]->jitter + (1-GAMMA) * 
			iaxs[fr->callno]->historicjitter;

	/* If our jitter buffer is too big (by a significant margin), then we slowly
	   shrink it to avoid letting the change be perceived */
	if (max < iaxs[fr->callno]->jitterbuffer - max_jitter_buffer)
		iaxs[fr->callno]->jitterbuffer -= jittershrinkrate;

	/* If our jitter buffer headroom is too small (by a significant margin), then we slowly enlarge it */
	/* min_jitter_buffer should be SMALLER than max_jitter_buffer - leaving a "no mans land"
	   in between - otherwise the jitterbuffer size will hunt up and down causing unnecessary
	   disruption.  Set maxexcessbuffer to say 150msec, minexcessbuffer to say 50 */
	if (max > iaxs[fr->callno]->jitterbuffer - min_jitter_buffer)
		iaxs[fr->callno]->jitterbuffer += jittershrinkrate;

	/* If our jitter buffer is smaller than our maximum delay, grow the jitter
	   buffer immediately to accomodate it (and a little more).  */
	if (max > iaxs[fr->callno]->jitterbuffer)
		iaxs[fr->callno]->jitterbuffer = max 
			/* + ((float)iaxs[fr->callno]->jitter) * 0.1 */;

	/* update "min", just for RRs and stats */
	iaxs[fr->callno]->min = min; 

	/* Subtract the lateness from our jitter buffer to know how long to wait
	   before sending our packet.  */
	delay = iaxs[fr->callno]->jitterbuffer - ms;

	/* Whatever happens, no frame waits longer than maxjitterbuffer */
	if (delay > maxjitterbuffer)
		delay = maxjitterbuffer;
	
	/* If jitter buffer is disabled then just pretend the frame is "right on time" */
	/* If frame came from trunk, also don't do any delay */
	if ( (!ast_test_flag(iaxs[fr->callno], IAX_USEJITTERBUF)) || fromtrunk )
		delay = 0;

	if (option_debug && iaxdebug) {
		/* Log jitter stats for possible offline analysis */
		ast_log(LOG_DEBUG, "Jitter: call=%d ts=%d orig=%d last=%d %s: min=%d max=%d jb=%d %+d lateness=%d jbdelay=%d jitter=%d historic=%d\n",
					fr->callno, fr->ts, orig_ts, iaxs[fr->callno]->last,
					(fr->af.frametype == AST_FRAME_VOICE) ? "VOICE" : "CONTROL",
					min, max, iaxs[fr->callno]->jitterbuffer,
					iaxs[fr->callno]->jitterbuffer - prevjitterbuffer,
					ms, delay,
					iaxs[fr->callno]->jitter, iaxs[fr->callno]->historicjitter);
	}

	if (delay < 1) {
		/* Don't deliver it more than 4 ms late */
		if ((delay > -4) || (fr->af.frametype != AST_FRAME_VOICE)) {
			if (option_debug && iaxdebug)
				ast_log(LOG_DEBUG, "schedule_delivery: Delivering immediately (Calculated delay is %d)\n", delay);
			__do_deliver(fr);
		} else {
			if (option_debug && iaxdebug)
				ast_log(LOG_DEBUG, "schedule_delivery: Dropping voice packet since %dms delay is too old\n", delay);
			iaxs[fr->callno]->frames_dropped++;
			/* Free our iax frame */
			iax2_frame_free(fr);
		}
	} else {
		if (option_debug && iaxdebug)
			ast_log(LOG_DEBUG, "schedule_delivery: Scheduling delivery in %d ms\n", delay);
		fr->retrans = ast_sched_add(sched, delay, do_deliver, fr);
	}
#endif
	return 0;
}

static int iax2_transmit(struct iax_frame *fr)
{
	/* Lock the queue and place this packet at the end */
	fr->next = NULL;
	fr->prev = NULL;
	/* By setting this to 0, the network thread will send it for us, and
	   queue retransmission if necessary */
	fr->sentyet = 0;
	ast_mutex_lock(&iaxq.lock);
	if (!iaxq.head) {
		/* Empty queue */
		iaxq.head = fr;
		iaxq.tail = fr;
	} else {
		/* Double link */
		iaxq.tail->next = fr;
		fr->prev = iaxq.tail;
		iaxq.tail = fr;
	}
	iaxq.count++;
	ast_mutex_unlock(&iaxq.lock);
	/* Wake up the network thread */
	pthread_kill(netthreadid, SIGURG);
	return 0;
}



static int iax2_digit(struct ast_channel *c, char digit)
{
	return send_command_locked(PTR_TO_CALLNO(c->tech_pvt), AST_FRAME_DTMF, digit, 0, NULL, 0, -1);
}

static int iax2_sendtext(struct ast_channel *c, const char *text)
{
	
	return send_command_locked(PTR_TO_CALLNO(c->tech_pvt), AST_FRAME_TEXT,
		0, 0, (unsigned char *)text, strlen(text) + 1, -1);
}

static int iax2_sendimage(struct ast_channel *c, struct ast_frame *img)
{
	return send_command_locked(PTR_TO_CALLNO(c->tech_pvt), AST_FRAME_IMAGE, img->subclass, 0, img->data, img->datalen, -1);
}

static int iax2_sendhtml(struct ast_channel *c, int subclass, const char *data, int datalen)
{
	return send_command_locked(PTR_TO_CALLNO(c->tech_pvt), AST_FRAME_HTML, subclass, 0, (unsigned char *)data, datalen, -1);
}

static int iax2_fixup(struct ast_channel *oldchannel, struct ast_channel *newchan)
{
	unsigned short callno = PTR_TO_CALLNO(newchan->tech_pvt);
	ast_mutex_lock(&iaxsl[callno]);
	if (iaxs[callno])
		iaxs[callno]->owner = newchan;
	else
		ast_log(LOG_WARNING, "Uh, this isn't a good sign...\n");
	ast_mutex_unlock(&iaxsl[callno]);
	return 0;
}

static struct iax2_peer *build_peer(const char *name, struct ast_variable *v, int temponly);
static struct iax2_user *build_user(const char *name, struct ast_variable *v, int temponly);

static void destroy_user(struct iax2_user *user);
static int expire_registry(void *data);

static struct iax2_peer *realtime_peer(const char *peername, struct sockaddr_in *sin)
{
	struct ast_variable *var;
	struct ast_variable *tmp;
	struct iax2_peer *peer=NULL;
	time_t regseconds, nowtime;
	int dynamic=0;

	if (peername)
		var = ast_load_realtime("iaxpeers", "name", peername, NULL);
	else {
		char iabuf[INET_ADDRSTRLEN];
		char porta[25];
		ast_inet_ntoa(iabuf, sizeof(iabuf), sin->sin_addr);
		sprintf(porta, "%d", ntohs(sin->sin_port));
		var = ast_load_realtime("iaxpeers", "ipaddr", iabuf, "port", porta, NULL);
		if (var) {
			/* We'll need the peer name in order to build the structure! */
			tmp = var;
			while(tmp) {
				if (!strcasecmp(tmp->name, "name"))
					peername = tmp->value;
				tmp = tmp->next;
			}
		}
	}
	if (!var)
		return NULL;

	peer = build_peer(peername, var, ast_test_flag((&globalflags), IAX_RTCACHEFRIENDS) ? 0 : 1);
	
	if (!peer)
		return NULL;

	tmp = var;
	while(tmp) {
		/* Make sure it's not a user only... */
		if (!strcasecmp(tmp->name, "type")) {
			if (strcasecmp(tmp->value, "friend") &&
			    strcasecmp(tmp->value, "peer")) {
				/* Whoops, we weren't supposed to exist! */
				destroy_peer(peer);
				peer = NULL;
				break;
			} 
		} else if (!strcasecmp(tmp->name, "regseconds")) {
			if (sscanf(tmp->value, "%ld", (time_t *)&regseconds) != 1)
				regseconds = 0;
		} else if (!strcasecmp(tmp->name, "ipaddr")) {
			inet_aton(tmp->value, &(peer->addr.sin_addr));
		} else if (!strcasecmp(tmp->name, "port")) {
			peer->addr.sin_port = htons(atoi(tmp->value));
		} else if (!strcasecmp(tmp->name, "host")) {
			if (!strcasecmp(tmp->value, "dynamic"))
				dynamic = 1;
		}
		tmp = tmp->next;
	}
	if (!peer)
		return NULL;

	ast_variables_destroy(var);

	if (ast_test_flag((&globalflags), IAX_RTCACHEFRIENDS)) {
		ast_copy_flags(peer, &globalflags, IAX_RTAUTOCLEAR|IAX_RTCACHEFRIENDS);
		if (ast_test_flag(peer, IAX_RTAUTOCLEAR)) {
			if (peer->expire > -1)
				ast_sched_del(sched, peer->expire);
			peer->expire = ast_sched_add(sched, (global_rtautoclear) * 1000, expire_registry, peer);
		}
		ast_mutex_lock(&peerl.lock);
		peer->next = peerl.peers;
		peerl.peers = peer;
		ast_mutex_unlock(&peerl.lock);
		if (ast_test_flag(peer, IAX_DYNAMIC))
			reg_source_db(peer);
	} else {
		ast_set_flag(peer, IAX_TEMPONLY);	
	}

	if (!ast_test_flag(&globalflags, IAX_RTIGNOREREGEXPIRE) && dynamic) {
		time(&nowtime);
		if ((nowtime - regseconds) > IAX_DEFAULT_REG_EXPIRE) {
			memset(&peer->addr, 0, sizeof(peer->addr));
			if (option_debug)
				ast_log(LOG_DEBUG, "realtime_peer: Bah, '%s' is expired (%d/%d/%d)!\n",
						peername, (int)(nowtime - regseconds), (int)regseconds, (int)nowtime);
		}
		else {
			if (option_debug)
				ast_log(LOG_DEBUG, "realtime_peer: Registration for '%s' still active (%d/%d/%d)!\n",
						peername, (int)(nowtime - regseconds), (int)regseconds, (int)nowtime);
		}
	}

	return peer;
}

static struct iax2_user *realtime_user(const char *username)
{
	struct ast_variable *var;
	struct ast_variable *tmp;
	struct iax2_user *user=NULL;

	var = ast_load_realtime("iaxusers", "name", username, NULL);
	if (!var)
		return NULL;

	tmp = var;
	while(tmp) {
		/* Make sure it's not a peer only... */
		if (!strcasecmp(tmp->name, "type")) {
			if (strcasecmp(tmp->value, "friend") &&
			    strcasecmp(tmp->value, "user")) {
				return NULL;
			} 
		}
		tmp = tmp->next;
	}

	user = build_user(username, var, !ast_test_flag((&globalflags), IAX_RTCACHEFRIENDS));
	if (!user)
		return NULL;

	ast_variables_destroy(var);

	if (ast_test_flag((&globalflags), IAX_RTCACHEFRIENDS)) {
		ast_set_flag(user, IAX_RTCACHEFRIENDS);
		ast_mutex_lock(&userl.lock);
 		user->next = userl.users;
		userl.users = user;
		ast_mutex_unlock(&userl.lock);
	} else {
		ast_set_flag(user, IAX_TEMPONLY);	
	}

	return user;
}

static void realtime_update_peer(const char *peername, struct sockaddr_in *sin)
{
	char port[10];
	char ipaddr[20];
	char regseconds[20];
	time_t nowtime;
	
	time(&nowtime);
	snprintf(regseconds, sizeof(regseconds), "%d", (int)nowtime);
	ast_inet_ntoa(ipaddr, sizeof(ipaddr), sin->sin_addr);
	snprintf(port, sizeof(port), "%d", ntohs(sin->sin_port));
	ast_update_realtime("iaxpeers", "name", peername, "ipaddr", ipaddr, "port", port, "regseconds", regseconds, NULL);
}

struct create_addr_info {
	int capability;
	unsigned int flags;
	int maxtime;
	int encmethods;
	int found;
	int sockfd;
	char username[80];
	char secret[80];
	char outkey[80];
	char timezone[80];
	char prefs[32];
	char context[AST_MAX_CONTEXT];
	char peercontext[AST_MAX_CONTEXT];
};

static int create_addr(const char *peername, struct sockaddr_in *sin, struct create_addr_info *cai)
{
	struct ast_hostent ahp;
	struct hostent *hp;
	struct iax2_peer *peer;

	ast_clear_flag(cai, IAX_SENDANI | IAX_TRUNK);
	cai->sockfd = defaultsockfd;
	cai->maxtime = 0;
	sin->sin_family = AF_INET;

	if (!(peer = find_peer(peername, 1))) {
		cai->found = 0;

		hp = ast_gethostbyname(peername, &ahp);
		if (hp) {
			memcpy(&sin->sin_addr, hp->h_addr, sizeof(sin->sin_addr));
			sin->sin_port = htons(IAX_DEFAULT_PORTNO);
			/* use global iax prefs for unknown peer/user */
			ast_codec_pref_convert(&prefs, cai->prefs, sizeof(cai->prefs), 1);
			return 0;
		} else {
			ast_log(LOG_WARNING, "No such host: %s\n", peername);
			return -1;
		}
	}

	cai->found = 1;
	
	/* if the peer has no address (current or default), return failure */
	if (!(peer->addr.sin_addr.s_addr || peer->defaddr.sin_addr.s_addr)) {
		if (ast_test_flag(peer, IAX_TEMPONLY))
			destroy_peer(peer);
		return -1;
	}

	/* if the peer is being monitored and is currently unreachable, return failure */
	if (peer->maxms && ((peer->lastms > peer->maxms) || (peer->lastms < 0))) {
		if (ast_test_flag(peer, IAX_TEMPONLY))
			destroy_peer(peer);
		return -1;
	}

	ast_copy_flags(cai, peer, IAX_SENDANI | IAX_TRUNK | IAX_NOTRANSFER | IAX_USEJITTERBUF | IAX_FORCEJITTERBUF);
	cai->maxtime = peer->maxms;
	cai->capability = peer->capability;
	cai->encmethods = peer->encmethods;
	cai->sockfd = peer->sockfd;
	ast_codec_pref_convert(&peer->prefs, cai->prefs, sizeof(cai->prefs), 1);
	ast_copy_string(cai->context, peer->context, sizeof(cai->context));
	ast_copy_string(cai->peercontext, peer->peercontext, sizeof(cai->peercontext));
	ast_copy_string(cai->username, peer->username, sizeof(cai->username));
	ast_copy_string(cai->timezone, peer->zonetag, sizeof(cai->timezone));
	ast_copy_string(cai->outkey, peer->outkey, sizeof(cai->outkey));
	if (ast_strlen_zero(peer->dbsecret)) {
		ast_copy_string(cai->secret, peer->secret, sizeof(cai->secret));
	} else {
		char *family;
		char *key = NULL;

		family = ast_strdupa(peer->dbsecret);
		key = strchr(family, '/');
		if (key)
			*key++ = '\0';
		if (!key || ast_db_get(family, key, cai->secret, sizeof(cai->secret))) {
			ast_log(LOG_WARNING, "Unable to retrieve database password for family/key '%s'!\n", peer->dbsecret);
			if (ast_test_flag(peer, IAX_TEMPONLY))
				destroy_peer(peer);
			return -1;
		}
	}

	if (peer->addr.sin_addr.s_addr) {
		sin->sin_addr = peer->addr.sin_addr;
		sin->sin_port = peer->addr.sin_port;
	} else {
		sin->sin_addr = peer->defaddr.sin_addr;
		sin->sin_port = peer->defaddr.sin_port;
	}

	if (ast_test_flag(peer, IAX_TEMPONLY))
		destroy_peer(peer);

	return 0;
}

static int auto_congest(void *nothing)
{
	int callno = PTR_TO_CALLNO(nothing);
	struct ast_frame f = { AST_FRAME_CONTROL, AST_CONTROL_CONGESTION };
	ast_mutex_lock(&iaxsl[callno]);
	if (iaxs[callno]) {
		iaxs[callno]->initid = -1;
		iax2_queue_frame(callno, &f);
		ast_log(LOG_NOTICE, "Auto-congesting call due to slow response\n");
	}
	ast_mutex_unlock(&iaxsl[callno]);
	return 0;
}

static unsigned int iax2_datetime(char *tz)
{
	time_t t;
	struct tm tm;
	unsigned int tmp;
	time(&t);
	localtime_r(&t, &tm);
	if (!ast_strlen_zero(tz))
		ast_localtime(&t, &tm, tz);
	tmp  = (tm.tm_sec >> 1) & 0x1f;			/* 5 bits of seconds */
	tmp |= (tm.tm_min & 0x3f) << 5;			/* 6 bits of minutes */
	tmp |= (tm.tm_hour & 0x1f) << 11;		/* 5 bits of hours */
	tmp |= (tm.tm_mday & 0x1f) << 16;		/* 5 bits of day of month */
	tmp |= ((tm.tm_mon + 1) & 0xf) << 21;		/* 4 bits of month */
	tmp |= ((tm.tm_year - 100) & 0x7f) << 25;	/* 7 bits of year */
	return tmp;
}

struct parsed_dial_string {
	char *username;
	char *password;
	char *key;
	char *peer;
	char *port;
	char *exten;
	char *context;
	char *options;
};

/*!
 * \brief Parses an IAX dial string into its component parts.
 * \param data the string to be parsed
 * \param pds pointer to a \c struct \c parsed_dial_string to be filled in
 * \return nothing
 *
 * This function parses the string and fills the structure
 * with pointers to its component parts. The input string
 * will be modified.
 *
 * \note This function supports both plaintext passwords and RSA
 * key names; if the password string is formatted as '[keyname]',
 * then the keyname will be placed into the key field, and the
 * password field will be set to NULL.
 *
 * \note The dial string format is:
 *       [username[:password]@]peer[:port][/exten[@@context]][/options]
 */
static void parse_dial_string(char *data, struct parsed_dial_string *pds)
{
	if (ast_strlen_zero(data))
		return;

	pds->peer = strsep(&data, "/");
	pds->exten = strsep(&data, "/");
	pds->options = data;

	if (pds->exten) {
		data = pds->exten;
		pds->exten = strsep(&data, "@");
		pds->context = data;
	}

	if (strchr(pds->peer, '@')) {
		data = pds->peer;
		pds->username = strsep(&data, "@");
		pds->peer = data;
	}

	if (pds->username) {
		data = pds->username;
		pds->username = strsep(&data, ":");
		pds->password = data;
	}

	data = pds->peer;
	pds->peer = strsep(&data, ":");
	pds->port = data;

	/* check for a key name wrapped in [] in the secret position, if found,
	   move it to the key field instead
	*/
	if (pds->password && (pds->password[0] == '[')) {
		pds->key = ast_strip_quoted(pds->password, "[", "]");
		pds->password = NULL;
	}
}

static int iax2_call(struct ast_channel *c, char *dest, int timeout)
{
	struct sockaddr_in sin;
	char *l=NULL, *n=NULL, *tmpstr;
	struct iax_ie_data ied;
	char *defaultrdest = "s";
	unsigned short callno = PTR_TO_CALLNO(c->tech_pvt);
	struct parsed_dial_string pds;
	struct create_addr_info cai;

	if ((c->_state != AST_STATE_DOWN) && (c->_state != AST_STATE_RESERVED)) {
		ast_log(LOG_WARNING, "Channel is already in use (%s)?\n", c->name);
		return -1;
	}

	memset(&cai, 0, sizeof(cai));
	cai.encmethods = iax2_encryption;

	memset(&pds, 0, sizeof(pds));
	tmpstr = ast_strdupa(dest);
	parse_dial_string(tmpstr, &pds);

	if (!pds.exten)
		pds.exten = defaultrdest;

	if (create_addr(pds.peer, &sin, &cai)) {
		ast_log(LOG_WARNING, "No address associated with '%s'\n", pds.peer);
		return -1;
	}

	if (!pds.username && !ast_strlen_zero(cai.username))
		pds.username = cai.username;
	if (!pds.password && !ast_strlen_zero(cai.secret))
		pds.password = cai.secret;
	if (!pds.key && !ast_strlen_zero(cai.outkey))
		pds.key = cai.outkey;
	if (!pds.context && !ast_strlen_zero(cai.peercontext))
		pds.context = cai.peercontext;

	/* Keep track of the context for outgoing calls too */
	ast_copy_string(c->context, cai.context, sizeof(c->context));

	if (pds.port)
		sin.sin_port = htons(atoi(pds.port));

	l = c->cid.cid_num;
	n = c->cid.cid_name;

	/* Now build request */	
	memset(&ied, 0, sizeof(ied));

	/* On new call, first IE MUST be IAX version of caller */
	iax_ie_append_short(&ied, IAX_IE_VERSION, IAX_PROTO_VERSION);
	iax_ie_append_str(&ied, IAX_IE_CALLED_NUMBER, pds.exten);
	if (pds.options && strchr(pds.options, 'a')) {
		/* Request auto answer */
		iax_ie_append(&ied, IAX_IE_AUTOANSWER);
	}

	iax_ie_append_str(&ied, IAX_IE_CODEC_PREFS, cai.prefs);

	if (l) {
		iax_ie_append_str(&ied, IAX_IE_CALLING_NUMBER, l);
		iax_ie_append_byte(&ied, IAX_IE_CALLINGPRES, c->cid.cid_pres);
	} else {
		if (n)
			iax_ie_append_byte(&ied, IAX_IE_CALLINGPRES, c->cid.cid_pres);
		else
			iax_ie_append_byte(&ied, IAX_IE_CALLINGPRES, AST_PRES_NUMBER_NOT_AVAILABLE);
	}

	iax_ie_append_byte(&ied, IAX_IE_CALLINGTON, c->cid.cid_ton);
	iax_ie_append_short(&ied, IAX_IE_CALLINGTNS, c->cid.cid_tns);

	if (n)
		iax_ie_append_str(&ied, IAX_IE_CALLING_NAME, n);
	if (ast_test_flag(iaxs[callno], IAX_SENDANI) && c->cid.cid_ani)
		iax_ie_append_str(&ied, IAX_IE_CALLING_ANI, c->cid.cid_ani);

	if (!ast_strlen_zero(c->language))
		iax_ie_append_str(&ied, IAX_IE_LANGUAGE, c->language);
	if (!ast_strlen_zero(c->cid.cid_dnid))
		iax_ie_append_str(&ied, IAX_IE_DNID, c->cid.cid_dnid);

	if (pds.context)
		iax_ie_append_str(&ied, IAX_IE_CALLED_CONTEXT, pds.context);

	if (pds.username)
		iax_ie_append_str(&ied, IAX_IE_USERNAME, pds.username);

	if (cai.encmethods)
		iax_ie_append_short(&ied, IAX_IE_ENCRYPTION, cai.encmethods);

	ast_mutex_lock(&iaxsl[callno]);

	if (!ast_strlen_zero(c->context))
		ast_copy_string(iaxs[callno]->context, c->context, sizeof(iaxs[callno]->context));

	if (pds.username)
		ast_copy_string(iaxs[callno]->username, pds.username, sizeof(iaxs[callno]->username));

	iaxs[callno]->encmethods = cai.encmethods;

	if (pds.key)
		ast_copy_string(iaxs[callno]->outkey, pds.key, sizeof(iaxs[callno]->outkey));
	if (pds.password)
		ast_copy_string(iaxs[callno]->secret, pds.password, sizeof(iaxs[callno]->secret));

	iax_ie_append_int(&ied, IAX_IE_FORMAT, c->nativeformats);
	iax_ie_append_int(&ied, IAX_IE_CAPABILITY, iaxs[callno]->capability);
	iax_ie_append_short(&ied, IAX_IE_ADSICPE, c->adsicpe);
	iax_ie_append_int(&ied, IAX_IE_DATETIME, iax2_datetime(cai.timezone));

	if (iaxs[callno]->maxtime) {
		/* Initialize pingtime and auto-congest time */
		iaxs[callno]->pingtime = iaxs[callno]->maxtime / 2;
		iaxs[callno]->initid = ast_sched_add(sched, iaxs[callno]->maxtime * 2, auto_congest, CALLNO_TO_PTR(callno));
	} else if (autokill) {
		iaxs[callno]->pingtime = autokill / 2;
		iaxs[callno]->initid = ast_sched_add(sched, autokill * 2, auto_congest, CALLNO_TO_PTR(callno));
	}

	/* Transmit the string in a "NEW" request */
	send_command(iaxs[callno], AST_FRAME_IAX, IAX_COMMAND_NEW, 0, ied.buf, ied.pos, -1);

	ast_mutex_unlock(&iaxsl[callno]);
	ast_setstate(c, AST_STATE_RINGING);
	
	return 0;
}

static int iax2_hangup(struct ast_channel *c) 
{
	unsigned short callno = PTR_TO_CALLNO(c->tech_pvt);
	int alreadygone;
 	struct iax_ie_data ied;
 	memset(&ied, 0, sizeof(ied));
	ast_mutex_lock(&iaxsl[callno]);
	if (callno && iaxs[callno]) {
		ast_log(LOG_DEBUG, "We're hanging up %s now...\n", c->name);
		alreadygone = ast_test_flag(iaxs[callno], IAX_ALREADYGONE);
		/* Send the hangup unless we have had a transmission error or are already gone */
 		iax_ie_append_byte(&ied, IAX_IE_CAUSECODE, (unsigned char)c->hangupcause);
		if (!iaxs[callno]->error && !alreadygone) 
 			send_command_final(iaxs[callno], AST_FRAME_IAX, IAX_COMMAND_HANGUP, 0, ied.buf, ied.pos, -1);
		/* Explicitly predestroy it */
		iax2_predestroy_nolock(callno);
		/* If we were already gone to begin with, destroy us now */
		if (alreadygone) {
			ast_log(LOG_DEBUG, "Really destroying %s now...\n", c->name);
			iax2_destroy_nolock(callno);
		}
	}
	ast_mutex_unlock(&iaxsl[callno]);
	if (option_verbose > 2) 
		ast_verbose(VERBOSE_PREFIX_3 "Hungup '%s'\n", c->name);
	return 0;
}

static int iax2_setoption(struct ast_channel *c, int option, void *data, int datalen)
{
	struct ast_option_header *h;
	int res;

	switch (option) {
	case AST_OPTION_TXGAIN:
	case AST_OPTION_RXGAIN:
		/* these two cannot be sent, because they require a result */
		errno = ENOSYS;
		return -1;
	default:
		if (!(h = ast_malloc(datalen + sizeof(*h))))
			return -1;

		h->flag = AST_OPTION_FLAG_REQUEST;
		h->option = htons(option);
		memcpy(h->data, data, datalen);
		res = send_command_locked(PTR_TO_CALLNO(c->tech_pvt), AST_FRAME_CONTROL,
					  AST_CONTROL_OPTION, 0, (unsigned char *) h,
					  datalen + sizeof(*h), -1);
		free(h);
		return res;
	}
}

static struct ast_frame *iax2_read(struct ast_channel *c) 
{
	static struct ast_frame f = { AST_FRAME_NULL, };
	ast_log(LOG_NOTICE, "I should never be called!\n");
	return &f;
}

static int iax2_start_transfer(unsigned short callno0, unsigned short callno1)
{
	int res;
	struct iax_ie_data ied0;
	struct iax_ie_data ied1;
	unsigned int transferid = rand();
	memset(&ied0, 0, sizeof(ied0));
	iax_ie_append_addr(&ied0, IAX_IE_APPARENT_ADDR, &iaxs[callno1]->addr);
	iax_ie_append_short(&ied0, IAX_IE_CALLNO, iaxs[callno1]->peercallno);
	iax_ie_append_int(&ied0, IAX_IE_TRANSFERID, transferid);

	memset(&ied1, 0, sizeof(ied1));
	iax_ie_append_addr(&ied1, IAX_IE_APPARENT_ADDR, &iaxs[callno0]->addr);
	iax_ie_append_short(&ied1, IAX_IE_CALLNO, iaxs[callno0]->peercallno);
	iax_ie_append_int(&ied1, IAX_IE_TRANSFERID, transferid);
	
	res = send_command(iaxs[callno0], AST_FRAME_IAX, IAX_COMMAND_TXREQ, 0, ied0.buf, ied0.pos, -1);
	if (res)
		return -1;
	res = send_command(iaxs[callno1], AST_FRAME_IAX, IAX_COMMAND_TXREQ, 0, ied1.buf, ied1.pos, -1);
	if (res)
		return -1;
	iaxs[callno0]->transferring = TRANSFER_BEGIN;
	iaxs[callno1]->transferring = TRANSFER_BEGIN;
	return 0;
}

static void lock_both(unsigned short callno0, unsigned short callno1)
{
	ast_mutex_lock(&iaxsl[callno0]);
	while (ast_mutex_trylock(&iaxsl[callno1])) {
		ast_mutex_unlock(&iaxsl[callno0]);
		usleep(10);
		ast_mutex_lock(&iaxsl[callno0]);
	}
}

static void unlock_both(unsigned short callno0, unsigned short callno1)
{
	ast_mutex_unlock(&iaxsl[callno1]);
	ast_mutex_unlock(&iaxsl[callno0]);
}

static enum ast_bridge_result iax2_bridge(struct ast_channel *c0, struct ast_channel *c1, int flags, struct ast_frame **fo, struct ast_channel **rc, int timeoutms)
{
	struct ast_channel *cs[3];
	struct ast_channel *who;
	int to = -1;
	int res = -1;
	int transferstarted=0;
	struct ast_frame *f;
	unsigned short callno0 = PTR_TO_CALLNO(c0->tech_pvt);
	unsigned short callno1 = PTR_TO_CALLNO(c1->tech_pvt);
	struct timeval waittimer = {0, 0}, tv;

	lock_both(callno0, callno1);
	/* Put them in native bridge mode */
	if (!flags & (AST_BRIDGE_DTMF_CHANNEL_0 | AST_BRIDGE_DTMF_CHANNEL_1)) {
		iaxs[callno0]->bridgecallno = callno1;
		iaxs[callno1]->bridgecallno = callno0;
	}
	unlock_both(callno0, callno1);

	/* If not, try to bridge until we can execute a transfer, if we can */
	cs[0] = c0;
	cs[1] = c1;
	for (/* ever */;;) {
		/* Check in case we got masqueraded into */
		if ((c0->type != channeltype) || (c1->type != channeltype)) {
			if (option_verbose > 2)
				ast_verbose(VERBOSE_PREFIX_3 "Can't masquerade, we're different...\n");
			/* Remove from native mode */
			if (c0->type == channeltype) {
				ast_mutex_lock(&iaxsl[callno0]);
				iaxs[callno0]->bridgecallno = 0;
				ast_mutex_unlock(&iaxsl[callno0]);
			}
			if (c1->type == channeltype) {
				ast_mutex_lock(&iaxsl[callno1]);
				iaxs[callno1]->bridgecallno = 0;
				ast_mutex_unlock(&iaxsl[callno1]);
			}
			return AST_BRIDGE_FAILED_NOWARN;
		}
		if (c0->nativeformats != c1->nativeformats) {
			if (option_verbose > 2) {
				char buf0[255];
				char buf1[255];
				ast_getformatname_multiple(buf0, sizeof(buf0) -1, c0->nativeformats);
				ast_getformatname_multiple(buf1, sizeof(buf1) -1, c1->nativeformats);
				ast_verbose(VERBOSE_PREFIX_3 "Operating with different codecs %d[%s] %d[%s] , can't native bridge...\n", c0->nativeformats, buf0, c1->nativeformats, buf1);
			}
			/* Remove from native mode */
			lock_both(callno0, callno1);
			iaxs[callno0]->bridgecallno = 0;
			iaxs[callno1]->bridgecallno = 0;
			unlock_both(callno0, callno1);
			return AST_BRIDGE_FAILED_NOWARN;
		}
		/* check if transfered and if we really want native bridging */
		if (!transferstarted && !ast_test_flag(iaxs[callno0], IAX_NOTRANSFER) && !ast_test_flag(iaxs[callno1], IAX_NOTRANSFER) && 
		!(flags & (AST_BRIDGE_DTMF_CHANNEL_0 | AST_BRIDGE_DTMF_CHANNEL_1))) {
			/* Try the transfer */
			if (iax2_start_transfer(callno0, callno1))
				ast_log(LOG_WARNING, "Unable to start the transfer\n");
			transferstarted = 1;
		}
		if ((iaxs[callno0]->transferring == TRANSFER_RELEASED) && (iaxs[callno1]->transferring == TRANSFER_RELEASED)) {
			/* Call has been transferred.  We're no longer involved */
			gettimeofday(&tv, NULL);
			if (ast_tvzero(waittimer)) {
				waittimer = tv;
			} else if (tv.tv_sec - waittimer.tv_sec > IAX_LINGER_TIMEOUT) {
				c0->_softhangup |= AST_SOFTHANGUP_DEV;
				c1->_softhangup |= AST_SOFTHANGUP_DEV;
				*fo = NULL;
				*rc = c0;
				res = AST_BRIDGE_COMPLETE;
				break;
			}
		}
		to = 1000;
		who = ast_waitfor_n(cs, 2, &to);
		if (timeoutms > -1) {
			timeoutms -= (1000 - to);
			if (timeoutms < 0)
				timeoutms = 0;
		}
		if (!who) {
			if (!timeoutms) {
				res = AST_BRIDGE_RETRY;
				break;
			}
			if (ast_check_hangup(c0) || ast_check_hangup(c1)) {
				res = AST_BRIDGE_FAILED;
				break;
			}
			continue;
		}
		f = ast_read(who);
		if (!f) {
			*fo = NULL;
			*rc = who;
			res = AST_BRIDGE_COMPLETE;
			break;
		}
		if ((f->frametype == AST_FRAME_CONTROL) && !(flags & AST_BRIDGE_IGNORE_SIGS)) {
			*fo = f;
			*rc = who;
			res =  AST_BRIDGE_COMPLETE;
			break;
		}
		if ((f->frametype == AST_FRAME_VOICE) ||
		    (f->frametype == AST_FRAME_TEXT) ||
		    (f->frametype == AST_FRAME_VIDEO) || 
		    (f->frametype == AST_FRAME_IMAGE) ||
		    (f->frametype == AST_FRAME_DTMF)) {
			if ((f->frametype == AST_FRAME_DTMF) && 
			    (flags & (AST_BRIDGE_DTMF_CHANNEL_0 | AST_BRIDGE_DTMF_CHANNEL_1))) {
				if ((who == c0)) {
					if  ((flags & AST_BRIDGE_DTMF_CHANNEL_0)) {
						*rc = c0;
						*fo = f;
						res = AST_BRIDGE_COMPLETE;
						/* Remove from native mode */
						break;
					} else 
						goto tackygoto;
				} else
				if ((who == c1)) {
					if (flags & AST_BRIDGE_DTMF_CHANNEL_1) {
						*rc = c1;
						*fo = f;
						res =  AST_BRIDGE_COMPLETE;
						break;
					} else
						goto tackygoto;
				}
			} else {
#if 0
				if (iaxdebug && option_debug)
					ast_log(LOG_DEBUG, "Read from %s\n", who->name);
				if (who == last) 
					ast_log(LOG_DEBUG, "Servicing channel %s twice in a row?\n", last->name);
				last = who;
#endif
tackygoto:
				if (who == c0) 
					ast_write(c1, f);
				else 
					ast_write(c0, f);
			}
			ast_frfree(f);
		} else
			ast_frfree(f);
		/* Swap who gets priority */
		cs[2] = cs[0];
		cs[0] = cs[1];
		cs[1] = cs[2];
	}
	lock_both(callno0, callno1);
	if(iaxs[callno0])
		iaxs[callno0]->bridgecallno = 0;
	if(iaxs[callno1])
		iaxs[callno1]->bridgecallno = 0;
	unlock_both(callno0, callno1);
	return res;
}

static int iax2_answer(struct ast_channel *c)
{
	unsigned short callno = PTR_TO_CALLNO(c->tech_pvt);
	if (option_debug)
		ast_log(LOG_DEBUG, "Answering IAX2 call\n");
	return send_command_locked(callno, AST_FRAME_CONTROL, AST_CONTROL_ANSWER, 0, NULL, 0, -1);
}

static int iax2_indicate(struct ast_channel *c, int condition)
{
	unsigned short callno = PTR_TO_CALLNO(c->tech_pvt);
	if (option_debug && iaxdebug)
		ast_log(LOG_DEBUG, "Indicating condition %d\n", condition);
	return send_command_locked(callno, AST_FRAME_CONTROL, condition, 0, NULL, 0, -1);
}
	
static int iax2_transfer(struct ast_channel *c, const char *dest)
{
	unsigned short callno = PTR_TO_CALLNO(c->tech_pvt);
	struct iax_ie_data ied;
	char tmp[256], *context;
	ast_copy_string(tmp, dest, sizeof(tmp));
	context = strchr(tmp, '@');
	if (context) {
		*context = '\0';
		context++;
	}
	memset(&ied, 0, sizeof(ied));
	iax_ie_append_str(&ied, IAX_IE_CALLED_NUMBER, tmp);
	if (context)
		iax_ie_append_str(&ied, IAX_IE_CALLED_CONTEXT, context);
	if (option_debug)
		ast_log(LOG_DEBUG, "Transferring '%s' to '%s'\n", c->name, dest);
	return send_command_locked(callno, AST_FRAME_IAX, IAX_COMMAND_TRANSFER, 0, ied.buf, ied.pos, -1);
}
	

static int iax2_write(struct ast_channel *c, struct ast_frame *f);

static int iax2_getpeertrunk(struct sockaddr_in sin)
{
	struct iax2_peer *peer;
	int res = 0;
	ast_mutex_lock(&peerl.lock);
	peer = peerl.peers;
	while(peer) {
		if ((peer->addr.sin_addr.s_addr == sin.sin_addr.s_addr) &&
				(peer->addr.sin_port == sin.sin_port)) {
					res = ast_test_flag(peer, IAX_TRUNK);
					break;
		}
		peer = peer->next;
	}
	ast_mutex_unlock(&peerl.lock);
	return res;
}

/*! \brief  Create new call, interface with the PBX core */
static struct ast_channel *ast_iax2_new(int callno, int state, int capability)
{
	struct ast_channel *tmp;
	struct chan_iax2_pvt *i;
	struct ast_variable *v = NULL;

	/* Don't hold call lock */
	ast_mutex_unlock(&iaxsl[callno]);
	tmp = ast_channel_alloc(1);
	ast_mutex_lock(&iaxsl[callno]);
	i = iaxs[callno];
	if (i && tmp) {
		tmp->tech = &iax2_tech;
		snprintf(tmp->name, sizeof(tmp->name), "IAX2/%s-%d", i->host, i->callno);
		tmp->type = channeltype;
		/* We can support any format by default, until we get restricted */
		tmp->nativeformats = capability;
		tmp->readformat = ast_best_codec(capability);
		tmp->writeformat = ast_best_codec(capability);
		tmp->tech_pvt = CALLNO_TO_PTR(i->callno);

		if (!ast_strlen_zero(i->cid_num))
			tmp->cid.cid_num = ast_strdup(i->cid_num);
		if (!ast_strlen_zero(i->cid_name))
			tmp->cid.cid_name = ast_strdup(i->cid_name);
		if (!ast_strlen_zero(i->ani))
			tmp->cid.cid_ani = ast_strdup(i->ani);
		if (!ast_strlen_zero(i->language))
			ast_copy_string(tmp->language, i->language, sizeof(tmp->language));
		if (!ast_strlen_zero(i->dnid))
			tmp->cid.cid_dnid = ast_strdup(i->dnid);
		tmp->cid.cid_pres = i->calling_pres;
		tmp->cid.cid_ton = i->calling_ton;
		tmp->cid.cid_tns = i->calling_tns;
		if (!ast_strlen_zero(i->accountcode))
			ast_copy_string(tmp->accountcode, i->accountcode, sizeof(tmp->accountcode));
		if (i->amaflags)
			tmp->amaflags = i->amaflags;
		ast_copy_string(tmp->context, i->context, sizeof(tmp->context));
		ast_copy_string(tmp->exten, i->exten, sizeof(tmp->exten));
		tmp->adsicpe = i->peeradsicpe;
		i->owner = tmp;
		i->capability = capability;
		ast_setstate(tmp, state);
		ast_mutex_lock(&usecnt_lock);
		usecnt++;
		ast_mutex_unlock(&usecnt_lock);
		ast_update_use_count();
		if (state != AST_STATE_DOWN) {
			if (ast_pbx_start(tmp)) {
				ast_log(LOG_WARNING, "Unable to start PBX on %s\n", tmp->name);
				ast_hangup(tmp);
				tmp = NULL;
			}
		}
		for (v = i->vars ; v ; v = v->next)
			pbx_builtin_setvar_helper(tmp,v->name,v->value);
		
	}
	return tmp;
}

static unsigned int calc_txpeerstamp(struct iax2_trunk_peer *tpeer, int sampms, struct timeval *tv)
{
	unsigned long int mssincetx; /* unsigned to handle overflows */
	long int ms, pred;

	tpeer->trunkact = *tv;
	mssincetx = ast_tvdiff_ms(*tv, tpeer->lasttxtime);
	if (mssincetx > 5000 || ast_tvzero(tpeer->txtrunktime)) {
		/* If it's been at least 5 seconds since the last time we transmitted on this trunk, reset our timers */
		tpeer->txtrunktime = *tv;
		tpeer->lastsent = 999999;
	}
	/* Update last transmit time now */
	tpeer->lasttxtime = *tv;
	
	/* Calculate ms offset */
	ms = ast_tvdiff_ms(*tv, tpeer->txtrunktime);
	/* Predict from last value */
	pred = tpeer->lastsent + sampms;
	if (abs(ms - pred) < MAX_TIMESTAMP_SKEW)
		ms = pred;
	
	/* We never send the same timestamp twice, so fudge a little if we must */
	if (ms == tpeer->lastsent)
		ms = tpeer->lastsent + 1;
	tpeer->lastsent = ms;
	return ms;
}

static unsigned int fix_peerts(struct timeval *tv, int callno, unsigned int ts)
{
	long ms;	/* NOT unsigned */
	if (ast_tvzero(iaxs[callno]->rxcore)) {
		/* Initialize rxcore time if appropriate */
		gettimeofday(&iaxs[callno]->rxcore, NULL);
		/* Round to nearest 20ms so traces look pretty */
		iaxs[callno]->rxcore.tv_usec -= iaxs[callno]->rxcore.tv_usec % 20000;
	}
	/* Calculate difference between trunk and channel */
	ms = ast_tvdiff_ms(*tv, iaxs[callno]->rxcore);
	/* Return as the sum of trunk time and the difference between trunk and real time */
	return ms + ts;
}

static unsigned int calc_timestamp(struct chan_iax2_pvt *p, unsigned int ts, struct ast_frame *f)
{
	int ms;
	int voice = 0;
	int genuine = 0;
	int adjust;
	struct timeval *delivery = NULL;


	/* What sort of frame do we have?: voice is self-explanatory
	   "genuine" means an IAX frame - things like LAGRQ/RP, PING/PONG, ACK
	   non-genuine frames are CONTROL frames [ringing etc], DTMF
	   The "genuine" distinction is needed because genuine frames must get a clock-based timestamp,
	   the others need a timestamp slaved to the voice frames so that they go in sequence
	*/
	if (f) {
		if (f->frametype == AST_FRAME_VOICE) {
			voice = 1;
			delivery = &f->delivery;
		} else if (f->frametype == AST_FRAME_IAX) {
			genuine = 1;
		} else if (f->frametype == AST_FRAME_CNG) {
			p->notsilenttx = 0;	
		}
	}
	if (ast_tvzero(p->offset)) {
		gettimeofday(&p->offset, NULL);
		/* Round to nearest 20ms for nice looking traces */
		p->offset.tv_usec -= p->offset.tv_usec % 20000;
	}
	/* If the timestamp is specified, just send it as is */
	if (ts)
		return ts;
	/* If we have a time that the frame arrived, always use it to make our timestamp */
	if (delivery && !ast_tvzero(*delivery)) {
		ms = ast_tvdiff_ms(*delivery, p->offset);
		if (option_debug > 2 && iaxdebug)
			ast_log(LOG_DEBUG, "calc_timestamp: call %d/%d: Timestamp slaved to delivery time\n", p->callno, iaxs[p->callno]->peercallno);
	} else {
		ms = ast_tvdiff_ms(ast_tvnow(), p->offset);
		if (ms < 0)
			ms = 0;
		if (voice) {
			/* On a voice frame, use predicted values if appropriate */
			if (p->notsilenttx && abs(ms - p->nextpred) <= MAX_TIMESTAMP_SKEW) {
				/* Adjust our txcore, keeping voice and non-voice synchronized */
				/* AN EXPLANATION:
				   When we send voice, we usually send "calculated" timestamps worked out
			 	   on the basis of the number of samples sent. When we send other frames,
				   we usually send timestamps worked out from the real clock.
				   The problem is that they can tend to drift out of step because the 
			    	   source channel's clock and our clock may not be exactly at the same rate.
				   We fix this by continuously "tweaking" p->offset.  p->offset is "time zero"
				   for this call.  Moving it adjusts timestamps for non-voice frames.
				   We make the adjustment in the style of a moving average.  Each time we
				   adjust p->offset by 10% of the difference between our clock-derived
				   timestamp and the predicted timestamp.  That's why you see "10000"
				   below even though IAX2 timestamps are in milliseconds.
				   The use of a moving average avoids offset moving too radically.
				   Generally, "adjust" roams back and forth around 0, with offset hardly
				   changing at all.  But if a consistent different starts to develop it
				   will be eliminated over the course of 10 frames (200-300msecs) 
				*/
				adjust = (ms - p->nextpred);
				if (adjust < 0)
					p->offset = ast_tvsub(p->offset, ast_samp2tv(abs(adjust), 10000));
				else if (adjust > 0)
					p->offset = ast_tvadd(p->offset, ast_samp2tv(adjust, 10000));

				if (!p->nextpred) {
					p->nextpred = ms; /*f->samples / 8;*/
					if (p->nextpred <= p->lastsent)
						p->nextpred = p->lastsent + 3;
				}
				ms = p->nextpred;
			} else {
			       /* in this case, just use the actual
				* time, since we're either way off
				* (shouldn't happen), or we're  ending a
				* silent period -- and seed the next
				* predicted time.  Also, round ms to the
				* next multiple of frame size (so our
				* silent periods are multiples of
				* frame size too) */

				if (iaxdebug && abs(ms - p->nextpred) > MAX_TIMESTAMP_SKEW )
					ast_log(LOG_DEBUG, "predicted timestamp skew (%u) > max (%u), using real ts instead.\n",
						abs(ms - p->nextpred), MAX_TIMESTAMP_SKEW);

				if (f->samples >= 8) /* check to make sure we dont core dump */
				{
					int diff = ms % (f->samples / 8);
					if (diff)
					    ms += f->samples/8 - diff;
				}

				p->nextpred = ms;
				p->notsilenttx = 1;
			}
		} else {
			/* On a dataframe, use last value + 3 (to accomodate jitter buffer shrinking) if appropriate unless
			   it's a genuine frame */
			if (genuine) {
				/* genuine (IAX LAGRQ etc) must keep their clock-based stamps */
				if (ms <= p->lastsent)
					ms = p->lastsent + 3;
			} else if (abs(ms - p->lastsent) <= MAX_TIMESTAMP_SKEW) {
				/* non-genuine frames (!?) (DTMF, CONTROL) should be pulled into the predicted stream stamps */
				ms = p->lastsent + 3;
			}
		}
	}
	p->lastsent = ms;
	if (voice)
		p->nextpred = p->nextpred + f->samples / 8;
#if 0
	printf("TS: %s - %dms\n", voice ? "Audio" : "Control", ms);
#endif	
	return ms;
}

#ifdef BRIDGE_OPTIMIZATION
static unsigned int calc_fakestamp(struct chan_iax2_pvt *p1, struct chan_iax2_pvt *p2, unsigned int fakets)
{
	int ms;
	/* Receive from p1, send to p2 */
	
	/* Setup rxcore if necessary on outgoing channel */
	if (ast_tvzero(p1->rxcore))
		p1->rxcore = ast_tvnow();

	/* Setup txcore if necessary on outgoing channel */
	if (ast_tvzero(p2->offset))
		p2->offset = ast_tvnow();
	
	/* Now, ts is the timestamp of the original packet in the orignal context.
	   Adding rxcore to it gives us when we would want the packet to be delivered normally.
	   Subtracting txcore of the outgoing channel gives us what we'd expect */
	
	ms = ast_tvdiff_ms(p1->rxcore, p2->offset);
	fakets += ms;

	p2->lastsent = fakets;
	return fakets;
}
#endif

static unsigned int calc_rxstamp(struct chan_iax2_pvt *p, unsigned int offset)
{
	/* Returns where in "receive time" we are.  That is, how many ms
	   since we received (or would have received) the frame with timestamp 0 */
	int ms;
#ifdef IAXTESTS
	int jit;
#endif /* IAXTESTS */
	/* Setup rxcore if necessary */
	if (ast_tvzero(p->rxcore)) {
		p->rxcore = ast_tvnow();
		if (option_debug && iaxdebug)
			ast_log(LOG_DEBUG, "calc_rxstamp: call=%d: rxcore set to %d.%6.6d - %dms\n",
					p->callno, (int)(p->rxcore.tv_sec), (int)(p->rxcore.tv_usec), offset);
		p->rxcore = ast_tvsub(p->rxcore, ast_samp2tv(offset, 1000));
#if 1
		if (option_debug && iaxdebug)
			ast_log(LOG_DEBUG, "calc_rxstamp: call=%d: works out as %d.%6.6d\n",
					p->callno, (int)(p->rxcore.tv_sec),(int)( p->rxcore.tv_usec));
#endif
	}

	ms = ast_tvdiff_ms(ast_tvnow(), p->rxcore);
#ifdef IAXTESTS
	if (test_jit) {
		if (!test_jitpct || ((100.0 * rand() / (RAND_MAX + 1.0)) < test_jitpct)) {
			jit = (int)((float)test_jit * rand() / (RAND_MAX + 1.0));
			if ((int)(2.0 * rand() / (RAND_MAX + 1.0)))
				jit = -jit;
			ms += jit;
		}
	}
	if (test_late) {
		ms += test_late;
		test_late = 0;
	}
#endif /* IAXTESTS */
	return ms;
}

static struct iax2_trunk_peer *find_tpeer(struct sockaddr_in *sin, int fd)
{
	struct iax2_trunk_peer *tpeer;
	char iabuf[INET_ADDRSTRLEN];
	/* Finds and locks trunk peer */
	ast_mutex_lock(&tpeerlock);
	tpeer = tpeers;
	while(tpeer) {
		/* We don't lock here because tpeer->addr *never* changes */
		if (!inaddrcmp(&tpeer->addr, sin)) {
			ast_mutex_lock(&tpeer->lock);
			break;
		}
		tpeer = tpeer->next;
	}
	if (!tpeer) {
		if ((tpeer = ast_calloc(1, sizeof(*tpeer)))) {
			ast_mutex_init(&tpeer->lock);
			tpeer->lastsent = 9999;
			memcpy(&tpeer->addr, sin, sizeof(tpeer->addr));
			tpeer->trunkact = ast_tvnow();
			ast_mutex_lock(&tpeer->lock);
			tpeer->next = tpeers;
			tpeer->sockfd = fd;
			tpeers = tpeer;
#ifdef SO_NO_CHECK
			setsockopt(tpeer->sockfd, SOL_SOCKET, SO_NO_CHECK, &nochecksums, sizeof(nochecksums));
#endif
			ast_log(LOG_DEBUG, "Created trunk peer for '%s:%d'\n", ast_inet_ntoa(iabuf, sizeof(iabuf), tpeer->addr.sin_addr), ntohs(tpeer->addr.sin_port));
		}
	}
	ast_mutex_unlock(&tpeerlock);
	return tpeer;
}

static int iax2_trunk_queue(struct chan_iax2_pvt *pvt, struct iax_frame *fr)
{
	struct ast_frame *f;
	struct iax2_trunk_peer *tpeer;
	void *tmp, *ptr;
	struct ast_iax2_meta_trunk_entry *met;
	struct ast_iax2_meta_trunk_mini *mtm;
	char iabuf[INET_ADDRSTRLEN];

	f = &fr->af;
	tpeer = find_tpeer(&pvt->addr, pvt->sockfd);
	if (tpeer) {
		if (tpeer->trunkdatalen + f->datalen + 4 >= tpeer->trunkdataalloc) {
			/* Need to reallocate space */
			if (tpeer->trunkdataalloc < MAX_TRUNKDATA) {
				if (!(tmp = ast_realloc(tpeer->trunkdata, tpeer->trunkdataalloc + DEFAULT_TRUNKDATA + IAX2_TRUNK_PREFACE))) {
					ast_mutex_unlock(&tpeer->lock);
					return -1;
				}
				
				tpeer->trunkdataalloc += DEFAULT_TRUNKDATA;
				tpeer->trunkdata = tmp;
				ast_log(LOG_DEBUG, "Expanded trunk '%s:%d' to %d bytes\n", ast_inet_ntoa(iabuf, sizeof(iabuf), tpeer->addr.sin_addr), ntohs(tpeer->addr.sin_port), tpeer->trunkdataalloc);
			} else {
				ast_log(LOG_WARNING, "Maximum trunk data space exceeded to %s:%d\n", ast_inet_ntoa(iabuf, sizeof(iabuf), tpeer->addr.sin_addr), ntohs(tpeer->addr.sin_port));
				ast_mutex_unlock(&tpeer->lock);
				return -1;
			}
		}

		/* Append to meta frame */
		ptr = tpeer->trunkdata + IAX2_TRUNK_PREFACE + tpeer->trunkdatalen;
		if (ast_test_flag(&globalflags, IAX_TRUNKTIMESTAMPS)) {
			mtm = (struct ast_iax2_meta_trunk_mini *)ptr;
			mtm->len = htons(f->datalen);
			mtm->mini.callno = htons(pvt->callno);
			mtm->mini.ts = htons(0xffff & fr->ts);
			ptr += sizeof(struct ast_iax2_meta_trunk_mini);
			tpeer->trunkdatalen += sizeof(struct ast_iax2_meta_trunk_mini);
		} else {
			met = (struct ast_iax2_meta_trunk_entry *)ptr;
			/* Store call number and length in meta header */
			met->callno = htons(pvt->callno);
			met->len = htons(f->datalen);
			/* Advance pointers/decrease length past trunk entry header */
			ptr += sizeof(struct ast_iax2_meta_trunk_entry);
			tpeer->trunkdatalen += sizeof(struct ast_iax2_meta_trunk_entry);
		}
		/* Copy actual trunk data */
		memcpy(ptr, f->data, f->datalen);
		tpeer->trunkdatalen += f->datalen;

		tpeer->calls++;
		ast_mutex_unlock(&tpeer->lock);
	}
	return 0;
}

static void build_enc_keys(const unsigned char *digest, aes_encrypt_ctx *ecx, aes_decrypt_ctx *dcx)
{
	aes_encrypt_key128(digest, ecx);
	aes_decrypt_key128(digest, dcx);
}

static void memcpy_decrypt(unsigned char *dst, const unsigned char *src, int len, aes_decrypt_ctx *dcx)
{
#if 0
	/* Debug with "fake encryption" */
	int x;
	if (len % 16)
		ast_log(LOG_WARNING, "len should be multiple of 16, not %d!\n", len);
	for (x=0;x<len;x++)
		dst[x] = src[x] ^ 0xff;
#else	
	unsigned char lastblock[16] = { 0 };
	int x;
	while(len > 0) {
		aes_decrypt(src, dst, dcx);
		for (x=0;x<16;x++)
			dst[x] ^= lastblock[x];
		memcpy(lastblock, src, sizeof(lastblock));
		dst += 16;
		src += 16;
		len -= 16;
	}
#endif
}

static void memcpy_encrypt(unsigned char *dst, const unsigned char *src, int len, aes_encrypt_ctx *ecx)
{
#if 0
	/* Debug with "fake encryption" */
	int x;
	if (len % 16)
		ast_log(LOG_WARNING, "len should be multiple of 16, not %d!\n", len);
	for (x=0;x<len;x++)
		dst[x] = src[x] ^ 0xff;
#else
	unsigned char curblock[16] = { 0 };
	int x;
	while(len > 0) {
		for (x=0;x<16;x++)
			curblock[x] ^= src[x];
		aes_encrypt(curblock, dst, ecx);
		memcpy(curblock, dst, sizeof(curblock)); 
		dst += 16;
		src += 16;
		len -= 16;
	}
#endif
}

static int decode_frame(aes_decrypt_ctx *dcx, struct ast_iax2_full_hdr *fh, struct ast_frame *f, int *datalen)
{
	int padding;
	unsigned char *workspace;
	workspace = alloca(*datalen);
	if (!workspace)
		return -1;
	if (ntohs(fh->scallno) & IAX_FLAG_FULL) {
		struct ast_iax2_full_enc_hdr *efh = (struct ast_iax2_full_enc_hdr *)fh;
		if (*datalen < 16 + sizeof(struct ast_iax2_full_hdr))
			return -1;
		/* Decrypt */
		memcpy_decrypt(workspace, efh->encdata, *datalen - sizeof(struct ast_iax2_full_enc_hdr), dcx);

		padding = 16 + (workspace[15] & 0xf);
		if (option_debug && iaxdebug)
			ast_log(LOG_DEBUG, "Decoding full frame with length %d (padding = %d) (15=%02x)\n", *datalen, padding, workspace[15]);
		if (*datalen < padding + sizeof(struct ast_iax2_full_hdr))
			return -1;

		*datalen -= padding;
		memcpy(efh->encdata, workspace + padding, *datalen - sizeof(struct ast_iax2_full_enc_hdr));
		f->frametype = fh->type;
		if (f->frametype == AST_FRAME_VIDEO) {
			f->subclass = uncompress_subclass(fh->csub & ~0x40) | ((fh->csub >> 6) & 0x1);
		} else {
			f->subclass = uncompress_subclass(fh->csub);
		}
	} else {
		struct ast_iax2_mini_enc_hdr *efh = (struct ast_iax2_mini_enc_hdr *)fh;
		if (option_debug && iaxdebug)
			ast_log(LOG_DEBUG, "Decoding mini with length %d\n", *datalen);
		if (*datalen < 16 + sizeof(struct ast_iax2_mini_hdr))
			return -1;
		/* Decrypt */
		memcpy_decrypt(workspace, efh->encdata, *datalen - sizeof(struct ast_iax2_mini_enc_hdr), dcx);
		padding = 16 + (workspace[15] & 0x0f);
		if (*datalen < padding + sizeof(struct ast_iax2_mini_hdr))
			return -1;
		*datalen -= padding;
		memcpy(efh->encdata, workspace + padding, *datalen - sizeof(struct ast_iax2_mini_enc_hdr));
	}
	return 0;
}

static int encrypt_frame(aes_encrypt_ctx *ecx, struct ast_iax2_full_hdr *fh, unsigned char *poo, int *datalen)
{
	int padding;
	unsigned char *workspace;
	workspace = alloca(*datalen + 32);
	if (!workspace)
		return -1;
	if (ntohs(fh->scallno) & IAX_FLAG_FULL) {
		struct ast_iax2_full_enc_hdr *efh = (struct ast_iax2_full_enc_hdr *)fh;
		if (option_debug && iaxdebug)
			ast_log(LOG_DEBUG, "Encoding full frame %d/%d with length %d\n", fh->type, fh->csub, *datalen);
		padding = 16 - ((*datalen - sizeof(struct ast_iax2_full_enc_hdr)) % 16);
		padding = 16 + (padding & 0xf);
		memcpy(workspace, poo, padding);
		memcpy(workspace + padding, efh->encdata, *datalen - sizeof(struct ast_iax2_full_enc_hdr));
		workspace[15] &= 0xf0;
		workspace[15] |= (padding & 0xf);
		if (option_debug && iaxdebug)
			ast_log(LOG_DEBUG, "Encoding full frame %d/%d with length %d + %d padding (15=%02x)\n", fh->type, fh->csub, *datalen, padding, workspace[15]);
		*datalen += padding;
		memcpy_encrypt(efh->encdata, workspace, *datalen - sizeof(struct ast_iax2_full_enc_hdr), ecx);
		if (*datalen >= 32 + sizeof(struct ast_iax2_full_enc_hdr))
			memcpy(poo, workspace + *datalen - 32, 32);
	} else {
		struct ast_iax2_mini_enc_hdr *efh = (struct ast_iax2_mini_enc_hdr *)fh;
		if (option_debug && iaxdebug)
			ast_log(LOG_DEBUG, "Encoding mini frame with length %d\n", *datalen);
		padding = 16 - ((*datalen - sizeof(struct ast_iax2_mini_enc_hdr)) % 16);
		padding = 16 + (padding & 0xf);
		memcpy(workspace, poo, padding);
		memcpy(workspace + padding, efh->encdata, *datalen - sizeof(struct ast_iax2_mini_enc_hdr));
		workspace[15] &= 0xf0;
		workspace[15] |= (padding & 0x0f);
		*datalen += padding;
		memcpy_encrypt(efh->encdata, workspace, *datalen - sizeof(struct ast_iax2_mini_enc_hdr), ecx);
		if (*datalen >= 32 + sizeof(struct ast_iax2_mini_enc_hdr))
			memcpy(poo, workspace + *datalen - 32, 32);
	}
	return 0;
}

static int decrypt_frame(int callno, struct ast_iax2_full_hdr *fh, struct ast_frame *f, int *datalen)
{
	int res=-1;
	if (!ast_test_flag(iaxs[callno], IAX_KEYPOPULATED)) {
		/* Search for possible keys, given secrets */
		struct MD5Context md5;
		unsigned char digest[16];
		char *tmppw, *stringp;
		
		stringp = ast_strdupa(iaxs[callno]->secret);
		while ((tmppw = strsep(&stringp, ";"))) {
			MD5Init(&md5);
			MD5Update(&md5, (unsigned char *)iaxs[callno]->challenge, strlen(iaxs[callno]->challenge));
			MD5Update(&md5, (unsigned char *)tmppw, strlen(tmppw));
			MD5Final(digest, &md5);
			build_enc_keys(digest, &iaxs[callno]->ecx, &iaxs[callno]->dcx);
			res = decode_frame(&iaxs[callno]->dcx, fh, f, datalen);
			if (!res) {
				ast_set_flag(iaxs[callno], IAX_KEYPOPULATED);
				break;
			}
		}
	} else 
		res = decode_frame(&iaxs[callno]->dcx, fh, f, datalen);
	return res;
}

static int iax2_send(struct chan_iax2_pvt *pvt, struct ast_frame *f, unsigned int ts, int seqno, int now, int transfer, int final)
{
	/* Queue a packet for delivery on a given private structure.  Use "ts" for
	   timestamp, or calculate if ts is 0.  Send immediately without retransmission
	   or delayed, with retransmission */
	struct ast_iax2_full_hdr *fh;
	struct ast_iax2_mini_hdr *mh;
	struct ast_iax2_video_hdr *vh;
	struct {
		struct iax_frame fr2;
		unsigned char buffer[4096];
	} frb;
	struct iax_frame *fr;
	int res;
	int sendmini=0;
	unsigned int lastsent;
	unsigned int fts;
		
	if (!pvt) {
		ast_log(LOG_WARNING, "No private structure for packet?\n");
		return -1;
	}
	
	lastsent = pvt->lastsent;

	/* Calculate actual timestamp */
	fts = calc_timestamp(pvt, ts, f);

	/* Bail here if this is an "interp" frame; we don't want or need to send these placeholders out
	 * (the endpoint should detect the lost packet itself).  But, we want to do this here, so that we
	 * increment the "predicted timestamps" for voice, if we're predecting */
	if(f->frametype == AST_FRAME_VOICE && f->datalen == 0)
	    return 0;


	if ((ast_test_flag(pvt, IAX_TRUNK) || ((fts & 0xFFFF0000L) == (lastsent & 0xFFFF0000L)))
		/* High two bytes are the same on timestamp, or sending on a trunk */ &&
	    (f->frametype == AST_FRAME_VOICE) 
		/* is a voice frame */ &&
		(f->subclass == pvt->svoiceformat) 
		/* is the same type */ ) {
			/* Force immediate rather than delayed transmission */
			now = 1;
			/* Mark that mini-style frame is appropriate */
			sendmini = 1;
	}
	if (((fts & 0xFFFF8000L) == (lastsent & 0xFFFF8000L)) && 
		(f->frametype == AST_FRAME_VIDEO) &&
		((f->subclass & ~0x1) == pvt->svideoformat)) {
			now = 1;
			sendmini = 1;
	}
	/* Allocate an iax_frame */
	if (now) {
		fr = &frb.fr2;
	} else
		fr = iax_frame_new(DIRECTION_OUTGRESS, ast_test_flag(pvt, IAX_ENCRYPTED) ? f->datalen + 32 : f->datalen);
	if (!fr) {
		ast_log(LOG_WARNING, "Out of memory\n");
		return -1;
	}
	/* Copy our prospective frame into our immediate or retransmitted wrapper */
	iax_frame_wrap(fr, f);

	fr->ts = fts;
	fr->callno = pvt->callno;
	fr->transfer = transfer;
	fr->final = final;
	if (!sendmini) {
		/* We need a full frame */
		if (seqno > -1)
			fr->oseqno = seqno;
		else
			fr->oseqno = pvt->oseqno++;
		fr->iseqno = pvt->iseqno;
		fh = (struct ast_iax2_full_hdr *)(fr->af.data - sizeof(struct ast_iax2_full_hdr));
		fh->scallno = htons(fr->callno | IAX_FLAG_FULL);
		fh->ts = htonl(fr->ts);
		fh->oseqno = fr->oseqno;
		if (transfer) {
			fh->iseqno = 0;
		} else
			fh->iseqno = fr->iseqno;
		/* Keep track of the last thing we've acknowledged */
		if (!transfer)
			pvt->aseqno = fr->iseqno;
		fh->type = fr->af.frametype & 0xFF;
		if (fr->af.frametype == AST_FRAME_VIDEO)
			fh->csub = compress_subclass(fr->af.subclass & ~0x1) | ((fr->af.subclass & 0x1) << 6);
		else
			fh->csub = compress_subclass(fr->af.subclass);
		if (transfer) {
			fr->dcallno = pvt->transfercallno;
		} else
			fr->dcallno = pvt->peercallno;
		fh->dcallno = htons(fr->dcallno);
		fr->datalen = fr->af.datalen + sizeof(struct ast_iax2_full_hdr);
		fr->data = fh;
		fr->retries = 0;
		/* Retry after 2x the ping time has passed */
		fr->retrytime = pvt->pingtime * 2;
		if (fr->retrytime < MIN_RETRY_TIME)
			fr->retrytime = MIN_RETRY_TIME;
		if (fr->retrytime > MAX_RETRY_TIME)
			fr->retrytime = MAX_RETRY_TIME;
		/* Acks' don't get retried */
		if ((f->frametype == AST_FRAME_IAX) && (f->subclass == IAX_COMMAND_ACK))
			fr->retries = -1;
		else if (f->frametype == AST_FRAME_VOICE)
			pvt->svoiceformat = f->subclass;
		else if (f->frametype == AST_FRAME_VIDEO)
			pvt->svideoformat = f->subclass & ~0x1;
		if (ast_test_flag(pvt, IAX_ENCRYPTED)) {
			if (ast_test_flag(pvt, IAX_KEYPOPULATED)) {
				if (iaxdebug) {
					if (fr->transfer)
						iax_showframe(fr, NULL, 2, &pvt->transfer, fr->datalen - sizeof(struct ast_iax2_full_hdr));
					else
						iax_showframe(fr, NULL, 2, &pvt->addr, fr->datalen - sizeof(struct ast_iax2_full_hdr));
				}
				encrypt_frame(&pvt->ecx, fh, pvt->semirand, &fr->datalen);
			} else
				ast_log(LOG_WARNING, "Supposed to send packet encrypted, but no key?\n");
		}
	
		if (now) {
			res = send_packet(fr);
		} else
			res = iax2_transmit(fr);
	} else {
		if (ast_test_flag(pvt, IAX_TRUNK)) {
			iax2_trunk_queue(pvt, fr);
			res = 0;
		} else if (fr->af.frametype == AST_FRAME_VIDEO) {
			/* Video frame have no sequence number */
			fr->oseqno = -1;
			fr->iseqno = -1;
			vh = (struct ast_iax2_video_hdr *)(fr->af.data - sizeof(struct ast_iax2_video_hdr));
			vh->zeros = 0;
			vh->callno = htons(0x8000 | fr->callno);
			vh->ts = htons((fr->ts & 0x7FFF) | (fr->af.subclass & 0x1 ? 0x8000 : 0));
			fr->datalen = fr->af.datalen + sizeof(struct ast_iax2_video_hdr);
			fr->data = vh;
			fr->retries = -1;
			res = send_packet(fr);			
		} else {
			/* Mini-frames have no sequence number */
			fr->oseqno = -1;
			fr->iseqno = -1;
			/* Mini frame will do */
			mh = (struct ast_iax2_mini_hdr *)(fr->af.data - sizeof(struct ast_iax2_mini_hdr));
			mh->callno = htons(fr->callno);
			mh->ts = htons(fr->ts & 0xFFFF);
			fr->datalen = fr->af.datalen + sizeof(struct ast_iax2_mini_hdr);
			fr->data = mh;
			fr->retries = -1;
			if (ast_test_flag(pvt, IAX_ENCRYPTED)) {
				if (ast_test_flag(pvt, IAX_KEYPOPULATED)) {
					encrypt_frame(&pvt->ecx, (struct ast_iax2_full_hdr *)mh, pvt->semirand, &fr->datalen);
				} else
					ast_log(LOG_WARNING, "Supposed to send packet encrypted, but no key?\n");
			}
			res = send_packet(fr);
		}
	}
	return res;
}



static int iax2_show_users(int fd, int argc, char *argv[])
{
	regex_t regexbuf;
	int havepattern = 0;

#define FORMAT "%-15.15s  %-20.20s  %-15.15s  %-15.15s  %-5.5s  %-5.10s\n"
#define FORMAT2 "%-15.15s  %-20.20s  %-15.15d  %-15.15s  %-5.5s  %-5.10s\n"

	struct iax2_user *user;
	char auth[90];
	char *pstr = "";

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

	ast_mutex_lock(&userl.lock);
	ast_cli(fd, FORMAT, "Username", "Secret", "Authen", "Def.Context", "A/C","Codec Pref");
	for(user=userl.users;user;user=user->next) {
		if (havepattern && regexec(&regexbuf, user->name, 0, NULL, 0))
			continue;

		if (!ast_strlen_zero(user->secret)) {
  			ast_copy_string(auth,user->secret,sizeof(auth));
		} else if (!ast_strlen_zero(user->inkeys)) {
  			snprintf(auth, sizeof(auth), "Key: %-15.15s ", user->inkeys);
 		} else
			ast_copy_string(auth, "-no secret-", sizeof(auth));

		if(ast_test_flag(user,IAX_CODEC_NOCAP))
			pstr = "REQ Only";
		else if(ast_test_flag(user,IAX_CODEC_NOPREFS))
			pstr = "Disabled";
		else
			pstr = ast_test_flag(user,IAX_CODEC_USER_FIRST) ? "Caller" : "Host";

		ast_cli(fd, FORMAT2, user->name, auth, user->authmethods, 
				user->contexts ? user->contexts->context : context,
				user->ha ? "Yes" : "No", pstr);

	}
	ast_mutex_unlock(&userl.lock);

	if (havepattern)
		regfree(&regexbuf);

	return RESULT_SUCCESS;
#undef FORMAT
#undef FORMAT2
}

static int __iax2_show_peers(int manager, int fd, int argc, char *argv[])
{
	regex_t regexbuf;
	int havepattern = 0;
	int total_peers = 0;
	int online_peers = 0;
	int offline_peers = 0;
	int unmonitored_peers = 0;

#define FORMAT2 "%-15.15s  %-15.15s %s  %-15.15s  %-8s  %s %-10s%s"
#define FORMAT "%-15.15s  %-15.15s %s  %-15.15s  %-5d%s  %s %-10s%s"

	struct iax2_peer *peer;
	char name[256];
	char iabuf[INET_ADDRSTRLEN];
	int registeredonly=0;
	char *term = manager ? "\r\n" : "\n";

	switch (argc) {
	case 6:
 		if (!strcasecmp(argv[3], "registered"))
			registeredonly = 1;
		else
			return RESULT_SHOWUSAGE;
		if (!strcasecmp(argv[4], "like")) {
			if (regcomp(&regexbuf, argv[5], REG_EXTENDED | REG_NOSUB))
				return RESULT_SHOWUSAGE;
			havepattern = 1;
		} else
			return RESULT_SHOWUSAGE;
		break;
	case 5:
		if (!strcasecmp(argv[3], "like")) {
			if (regcomp(&regexbuf, argv[4], REG_EXTENDED | REG_NOSUB))
				return RESULT_SHOWUSAGE;
			havepattern = 1;
		} else
			return RESULT_SHOWUSAGE;
		break;
	case 4:
 		if (!strcasecmp(argv[3], "registered"))
			registeredonly = 1;
		else
			return RESULT_SHOWUSAGE;
		break;
	case 3:
		break;
	default:
		return RESULT_SHOWUSAGE;
	}

	ast_mutex_lock(&peerl.lock);
	ast_cli(fd, FORMAT2, "Name/Username", "Host", "   ", "Mask", "Port", "   ", "Status", term);
	for (peer = peerl.peers;peer;peer = peer->next) {
		char nm[20];
		char status[20];
		char srch[2000];
		int retstatus;

		if (registeredonly && !peer->addr.sin_addr.s_addr)
			continue;
		if (havepattern && regexec(&regexbuf, peer->name, 0, NULL, 0))
			continue;

		if (!ast_strlen_zero(peer->username))
			snprintf(name, sizeof(name), "%s/%s", peer->name, peer->username);
		else
			ast_copy_string(name, peer->name, sizeof(name));
		
		retstatus = peer_status(peer, status, sizeof(status));
		if (retstatus > 0)
			online_peers++;
		else if (!retstatus)
			offline_peers++;
		else
			unmonitored_peers++;
		
		ast_copy_string(nm, ast_inet_ntoa(iabuf, sizeof(iabuf), peer->mask), sizeof(nm));

		snprintf(srch, sizeof(srch), FORMAT, name, 
					peer->addr.sin_addr.s_addr ? ast_inet_ntoa(iabuf, sizeof(iabuf), peer->addr.sin_addr) : "(Unspecified)",
					ast_test_flag(peer, IAX_DYNAMIC) ? "(D)" : "(S)",
					nm,
					ntohs(peer->addr.sin_port), ast_test_flag(peer, IAX_TRUNK) ? "(T)" : "   ",
					peer->encmethods ? "(E)" : "   ", status, term);

		ast_cli(fd, FORMAT, name, 
					peer->addr.sin_addr.s_addr ? ast_inet_ntoa(iabuf, sizeof(iabuf), peer->addr.sin_addr) : "(Unspecified)",
					ast_test_flag(peer, IAX_DYNAMIC) ? "(D)" : "(S)",
					nm,
					ntohs(peer->addr.sin_port), ast_test_flag(peer, IAX_TRUNK) ? "(T)" : "   ",
					peer->encmethods ? "(E)" : "   ", status, term);
		total_peers++;
	}
	ast_mutex_unlock(&peerl.lock);

	ast_cli(fd,"%d iax2 peers [%d online, %d offline, %d unmonitored]%s", total_peers, online_peers, offline_peers, unmonitored_peers, term);

	if (havepattern)
		regfree(&regexbuf);

	return RESULT_SUCCESS;
#undef FORMAT
#undef FORMAT2
}

static int iax2_show_peers(int fd, int argc, char *argv[])
{
	return __iax2_show_peers(0, fd, argc, argv);
}
static int manager_iax2_show_netstats( struct mansession *s, struct message *m )
{
	ast_cli_netstats(s->fd, 0);
	ast_cli(s->fd, "\r\n");
	return RESULT_SUCCESS;
}

static int iax2_show_firmware(int fd, int argc, char *argv[])
{
#define FORMAT2 "%-15.15s  %-15.15s %-15.15s\n"
#if !defined(__FreeBSD__)
#define FORMAT "%-15.15s  %-15d %-15d\n"
#else /* __FreeBSD__ */
#define FORMAT "%-15.15s  %-15d %-15d\n" /* XXX 2.95 ? */
#endif /* __FreeBSD__ */
	struct iax_firmware *cur;
	if ((argc != 3) && (argc != 4))
		return RESULT_SHOWUSAGE;
	ast_mutex_lock(&waresl.lock);
	
	ast_cli(fd, FORMAT2, "Device", "Version", "Size");
	for (cur = waresl.wares;cur;cur = cur->next) {
		if ((argc == 3) || (!strcasecmp(argv[3], (char *)cur->fwh->devname))) 
			ast_cli(fd, FORMAT, cur->fwh->devname, ntohs(cur->fwh->version),
				(int)ntohl(cur->fwh->datalen));
	}
	ast_mutex_unlock(&waresl.lock);
	return RESULT_SUCCESS;
#undef FORMAT
#undef FORMAT2
}

/* JDG: callback to display iax peers in manager */
static int manager_iax2_show_peers( struct mansession *s, struct message *m )
{
	char *a[] = { "iax2", "show", "users" };
	int ret;
	char *id;
	id = astman_get_header(m,"ActionID");
	if (!ast_strlen_zero(id))
		ast_cli(s->fd, "ActionID: %s\r\n",id);
	ret = __iax2_show_peers(1, s->fd, 3, a );
	ast_cli(s->fd, "\r\n\r\n" );
	return ret;
} /* /JDG */

static char *regstate2str(int regstate)
{
	switch(regstate) {
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

static int iax2_show_registry(int fd, int argc, char *argv[])
{
#define FORMAT2 "%-20.20s  %-10.10s  %-20.20s %8.8s  %s\n"
#define FORMAT "%-20.20s  %-10.10s  %-20.20s %8d  %s\n"
	struct iax2_registry *reg;
	char host[80];
	char perceived[80];
	char iabuf[INET_ADDRSTRLEN];
	if (argc != 3)
		return RESULT_SHOWUSAGE;
	ast_mutex_lock(&peerl.lock);
	ast_cli(fd, FORMAT2, "Host", "Username", "Perceived", "Refresh", "State");
	for (reg = registrations;reg;reg = reg->next) {
		snprintf(host, sizeof(host), "%s:%d", ast_inet_ntoa(iabuf, sizeof(iabuf), reg->addr.sin_addr), ntohs(reg->addr.sin_port));
		if (reg->us.sin_addr.s_addr) 
			snprintf(perceived, sizeof(perceived), "%s:%d", ast_inet_ntoa(iabuf, sizeof(iabuf), reg->us.sin_addr), ntohs(reg->us.sin_port));
		else
			ast_copy_string(perceived, "<Unregistered>", sizeof(perceived));
		ast_cli(fd, FORMAT, host, 
					reg->username, perceived, reg->refresh, regstate2str(reg->regstate));
	}
	ast_mutex_unlock(&peerl.lock);
	return RESULT_SUCCESS;
#undef FORMAT
#undef FORMAT2
}

#ifndef NEWJB
static int jitterbufsize(struct chan_iax2_pvt *pvt) {
	int min, i;
	min = 99999999;
	for (i=0; i<MEMORY_SIZE; i++) {
		if (pvt->history[i] < min)
			min = pvt->history[i];
	}
	if (pvt->jitterbuffer - min > maxjitterbuffer)
		return maxjitterbuffer;
	else
		return pvt->jitterbuffer - min;
}
#endif

static int iax2_show_channels(int fd, int argc, char *argv[])
{
#define FORMAT2 "%-20.20s  %-15.15s  %-10.10s  %-11.11s  %-11.11s  %-7.7s  %-6.6s  %-6.6s  %s\n"
#define FORMAT  "%-20.20s  %-15.15s  %-10.10s  %5.5d/%5.5d  %5.5d/%5.5d  %-5.5dms  %-4.4dms  %-4.4dms  %-6.6s\n"
#define FORMATB "%-20.20s  %-15.15s  %-10.10s  %5.5d/%5.5d  %5.5d/%5.5d  [Native Bridged to ID=%5.5d]\n"
	int x;
	int numchans = 0;
	char iabuf[INET_ADDRSTRLEN];

	if (argc != 3)
		return RESULT_SHOWUSAGE;
	ast_cli(fd, FORMAT2, "Channel", "Peer", "Username", "ID (Lo/Rem)", "Seq (Tx/Rx)", "Lag", "Jitter", "JitBuf", "Format");
	for (x=0;x<IAX_MAX_CALLS;x++) {
		ast_mutex_lock(&iaxsl[x]);
		if (iaxs[x]) {
#ifdef BRIDGE_OPTIMIZATION
			if (iaxs[x]->bridgecallno)
				ast_cli(fd, FORMATB,
						iaxs[x]->owner ? iaxs[x]->owner->name : "(None)",
						ast_inet_ntoa(iabuf, sizeof(iabuf), iaxs[x]->addr.sin_addr), 
						!ast_strlen_zero(iaxs[x]->username) ? iaxs[x]->username : "(None)", 
						iaxs[x]->callno, iaxs[x]->peercallno, 
						iaxs[x]->oseqno, iaxs[x]->iseqno, 
						iaxs[x]->bridgecallno );
			else
#endif
			{
				int lag, jitter, localdelay;
#ifdef NEWJB
				jb_info jbinfo;

				if(ast_test_flag(iaxs[x], IAX_USEJITTERBUF)) {
					jb_getinfo(iaxs[x]->jb, &jbinfo);
					jitter = jbinfo.jitter;
					localdelay = jbinfo.current - jbinfo.min;
				} else {
					jitter = -1;
					localdelay = 0;
				}
#else
				jitter = iaxs[x]->jitter;
				localdelay = ast_test_flag(iaxs[x], IAX_USEJITTERBUF) ? jitterbufsize(iaxs[x]) : 0;
#endif
				lag = iaxs[x]->remote_rr.delay;
				ast_cli(fd, FORMAT,
						iaxs[x]->owner ? iaxs[x]->owner->name : "(None)",
						ast_inet_ntoa(iabuf, sizeof(iabuf), iaxs[x]->addr.sin_addr), 
						!ast_strlen_zero(iaxs[x]->username) ? iaxs[x]->username : "(None)", 
						iaxs[x]->callno, iaxs[x]->peercallno, 
						iaxs[x]->oseqno, iaxs[x]->iseqno, 
						lag,
						jitter,
						localdelay,
						ast_getformatname(iaxs[x]->voiceformat) );
			}
			numchans++;
		}
		ast_mutex_unlock(&iaxsl[x]);
	}
	ast_cli(fd, "%d active IAX channel%s\n", numchans, (numchans != 1) ? "s" : "");
	return RESULT_SUCCESS;
#undef FORMAT
#undef FORMAT2
#undef FORMATB
}

static int ast_cli_netstats(int fd, int limit_fmt)
{
	int x;
	int numchans = 0;
	for (x=0;x<IAX_MAX_CALLS;x++) {
		ast_mutex_lock(&iaxsl[x]);
		if (iaxs[x]) {
#ifdef BRIDGE_OPTIMIZATION
			if (iaxs[x]->bridgecallno) {
				if (limit_fmt)	
					ast_cli(fd, "%-25.25s <NATIVE BRIDGED>",
						iaxs[x]->owner ? iaxs[x]->owner->name : "(None)");
				else
					ast_cli(fd, "%s <NATIVE BRIDGED>",
						iaxs[x]->owner ? iaxs[x]->owner->name : "(None)");
                        } else
#endif
			{
				int localjitter, localdelay, locallost, locallosspct, localdropped, localooo;
				char *fmt;
#ifdef NEWJB
				jb_info jbinfo;

				if(ast_test_flag(iaxs[x], IAX_USEJITTERBUF)) {
					jb_getinfo(iaxs[x]->jb, &jbinfo);
					localjitter = jbinfo.jitter;
					localdelay = jbinfo.current - jbinfo.min;
					locallost = jbinfo.frames_lost;
					locallosspct = jbinfo.losspct/1000;
					localdropped = jbinfo.frames_dropped;
					localooo = jbinfo.frames_ooo;
				} else {
					localjitter = -1;
					localdelay = 0;
					locallost = -1;
					locallosspct = -1;
					localdropped = 0;
					localooo = -1;
				}
#else
				localjitter = iaxs[x]->jitter;
				if(ast_test_flag(iaxs[x], IAX_USEJITTERBUF)) 
				{
					localdelay = jitterbufsize(iaxs[x]);
					localdropped = iaxs[x]->frames_dropped;
				} else {
					localdelay = localdropped = 0;
				}
				locallost = locallosspct = localooo = -1;
#endif
				if (limit_fmt)
					fmt = "%-25.25s %4d %4d %4d %5d %3d %5d %4d %6d %4d %4d %5d %3d %5d %4d %6d\n";
				else
					fmt = "%s %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d\n";
				ast_cli(fd, fmt,
						iaxs[x]->owner ? iaxs[x]->owner->name : "(None)",
						iaxs[x]->pingtime,
						localjitter, 
						localdelay,
						locallost,
						locallosspct,
						localdropped,
						localooo,
						iaxs[x]->frames_received/1000,
						iaxs[x]->remote_rr.jitter,
						iaxs[x]->remote_rr.delay,
						iaxs[x]->remote_rr.losscnt,
						iaxs[x]->remote_rr.losspct,
						iaxs[x]->remote_rr.dropped,
						iaxs[x]->remote_rr.ooo,
						iaxs[x]->remote_rr.packets/1000
				);
			}
			numchans++;
		}
		ast_mutex_unlock(&iaxsl[x]);
	}
	return numchans;
}

static int iax2_show_netstats(int fd, int argc, char *argv[])
{
	int numchans = 0;
	if (argc != 3)
		return RESULT_SHOWUSAGE;
	ast_cli(fd, "                                -------- LOCAL ---------------------  -------- REMOTE --------------------\n");
	ast_cli(fd, "Channel                    RTT  Jit  Del  Lost   %%  Drop  OOO  Kpkts  Jit  Del  Lost   %%  Drop  OOO  Kpkts\n");
	numchans = ast_cli_netstats(fd, 1);
	ast_cli(fd, "%d active IAX channel%s\n", numchans, (numchans != 1) ? "s" : "");
	return RESULT_SUCCESS;
}

static int iax2_do_debug(int fd, int argc, char *argv[])
{
	if (argc != 2)
		return RESULT_SHOWUSAGE;
	iaxdebug = 1;
	ast_cli(fd, "IAX2 Debugging Enabled\n");
	return RESULT_SUCCESS;
}

static int iax2_do_trunk_debug(int fd, int argc, char *argv[])
{
	if (argc != 3)
		return RESULT_SHOWUSAGE;
	iaxtrunkdebug = 1;
	ast_cli(fd, "IAX2 Trunk Debug Requested\n");
	return RESULT_SUCCESS;
}

static int iax2_do_jb_debug(int fd, int argc, char *argv[])
{
	if (argc != 3)
		return RESULT_SHOWUSAGE;
#ifdef NEWJB
	jb_setoutput(jb_error_output, jb_warning_output, jb_debug_output);
#endif
	ast_cli(fd, "IAX2 Jitterbuffer Debugging Enabled\n");
	return RESULT_SUCCESS;
}

static int iax2_no_debug(int fd, int argc, char *argv[])
{
	if (argc != 3)
		return RESULT_SHOWUSAGE;
	iaxdebug = 0;
	ast_cli(fd, "IAX2 Debugging Disabled\n");
	return RESULT_SUCCESS;
}

static int iax2_no_trunk_debug(int fd, int argc, char *argv[])
{
	if (argc != 4)
		return RESULT_SHOWUSAGE;
	iaxtrunkdebug = 0;
	ast_cli(fd, "IAX2 Trunk Debugging Disabled\n");
	return RESULT_SUCCESS;
}

static int iax2_no_jb_debug(int fd, int argc, char *argv[])
{
	if (argc != 4)
		return RESULT_SHOWUSAGE;
#ifdef NEWJB
	jb_setoutput(jb_error_output, jb_warning_output, NULL);
	jb_debug_output("\n");
#endif
	ast_cli(fd, "IAX2 Jitterbuffer Debugging Disabled\n");
	return RESULT_SUCCESS;
}

static int iax2_write(struct ast_channel *c, struct ast_frame *f)
{
	unsigned short callno = PTR_TO_CALLNO(c->tech_pvt);
	int res = -1;
	ast_mutex_lock(&iaxsl[callno]);
	if (iaxs[callno]) {
	/* If there's an outstanding error, return failure now */
		if (!iaxs[callno]->error) {
			if (ast_test_flag(iaxs[callno], IAX_ALREADYGONE))
				res = 0;
				/* Don't waste bandwidth sending null frames */
			else if (f->frametype == AST_FRAME_NULL)
				res = 0;
			else if ((f->frametype == AST_FRAME_VOICE) && ast_test_flag(iaxs[callno], IAX_QUELCH))
				res = 0;
			else if (!ast_test_flag(&iaxs[callno]->state, IAX_STATE_STARTED))
				res = 0;
			else
			/* Simple, just queue for transmission */
				res = iax2_send(iaxs[callno], f, 0, -1, 0, 0, 0);
		} else {
			ast_log(LOG_DEBUG, "Write error: %s\n", strerror(errno));
		}
	}
	/* If it's already gone, just return */
	ast_mutex_unlock(&iaxsl[callno]);
	return res;
}

static int __send_command(struct chan_iax2_pvt *i, char type, int command, unsigned int ts, const unsigned char *data, int datalen, int seqno, 
		int now, int transfer, int final)
{
	struct ast_frame f;
	f.frametype = type;
	f.subclass = command;
	f.datalen = datalen;
	f.samples = 0;
	f.mallocd = 0;
	f.offset = 0;
	f.src = (char *)__FUNCTION__;
	f.data = (char *)data;
	return iax2_send(i, &f, ts, seqno, now, transfer, final);
}

static int send_command(struct chan_iax2_pvt *i, char type, int command, unsigned int ts, const unsigned char *data, int datalen, int seqno)
{
	return __send_command(i, type, command, ts, data, datalen, seqno, 0, 0, 0);
}

static int send_command_locked(unsigned short callno, char type, int command, unsigned int ts, const unsigned char *data, int datalen, int seqno)
{
	int res;
	ast_mutex_lock(&iaxsl[callno]);
	res = send_command(iaxs[callno], type, command, ts, data, datalen, seqno);
	ast_mutex_unlock(&iaxsl[callno]);
	return res;
}

#ifdef BRIDGE_OPTIMIZATION
static int forward_command(struct chan_iax2_pvt *i, char type, int command, unsigned int ts, const char *data, int datalen, int seqno)
{
	return __send_command(iaxs[i->bridgecallno], type, command, ts, data, datalen, seqno, 0, 0, 0);
}
#endif

static int send_command_final(struct chan_iax2_pvt *i, char type, int command, unsigned int ts, const unsigned char *data, int datalen, int seqno)
{
	/* It is assumed that the callno has already been locked */
	iax2_predestroy_nolock(i->callno);
	return __send_command(i, type, command, ts, data, datalen, seqno, 0, 0, 1);
}

static int send_command_immediate(struct chan_iax2_pvt *i, char type, int command, unsigned int ts, const unsigned char *data, int datalen, int seqno)
{
	return __send_command(i, type, command, ts, data, datalen, seqno, 1, 0, 0);
}

static int send_command_transfer(struct chan_iax2_pvt *i, char type, int command, unsigned int ts, const unsigned char *data, int datalen)
{
	return __send_command(i, type, command, ts, data, datalen, 0, 0, 1, 0);
}

static int apply_context(struct iax2_context *con, char *context)
{
	while(con) {
		if (!strcmp(con->context, context) || !strcmp(con->context, "*"))
			return -1;
		con = con->next;
	}
	return 0;
}


static int check_access(int callno, struct sockaddr_in *sin, struct iax_ies *ies)
{
	/* Start pessimistic */
	int res = -1;
	int version = 2;
	struct iax2_user *user, *best = NULL;
	int bestscore = 0;
	int gotcapability=0;
	char iabuf[INET_ADDRSTRLEN];
	struct ast_variable *v = NULL, *tmpvar = NULL;

	if (!iaxs[callno])
		return res;
	if (ies->called_number)
		ast_copy_string(iaxs[callno]->exten, ies->called_number, sizeof(iaxs[callno]->exten));
	if (ies->calling_number) {
		ast_shrink_phone_number(ies->calling_number);
		ast_copy_string(iaxs[callno]->cid_num, ies->calling_number, sizeof(iaxs[callno]->cid_num));
	}
	if (ies->calling_name)
		ast_copy_string(iaxs[callno]->cid_name, ies->calling_name, sizeof(iaxs[callno]->cid_name));
	if (ies->calling_ani)
		ast_copy_string(iaxs[callno]->ani, ies->calling_ani, sizeof(iaxs[callno]->ani));
	if (ies->dnid)
		ast_copy_string(iaxs[callno]->dnid, ies->dnid, sizeof(iaxs[callno]->dnid));
	if (ies->called_context)
		ast_copy_string(iaxs[callno]->context, ies->called_context, sizeof(iaxs[callno]->context));
	if (ies->language)
		ast_copy_string(iaxs[callno]->language, ies->language, sizeof(iaxs[callno]->language));
	if (ies->username)
		ast_copy_string(iaxs[callno]->username, ies->username, sizeof(iaxs[callno]->username));
	if (ies->calling_ton > -1)
		iaxs[callno]->calling_ton = ies->calling_ton;
	if (ies->calling_tns > -1)
		iaxs[callno]->calling_tns = ies->calling_tns;
	if (ies->calling_pres > -1)
		iaxs[callno]->calling_pres = ies->calling_pres;
	if (ies->format)
		iaxs[callno]->peerformat = ies->format;
	if (ies->adsicpe)
		iaxs[callno]->peeradsicpe = ies->adsicpe;
	if (ies->capability) {
		gotcapability = 1;
		iaxs[callno]->peercapability = ies->capability;
	} 
	if (ies->version)
		version = ies->version;

	if(ies->codec_prefs)
		ast_codec_pref_convert(&iaxs[callno]->prefs, ies->codec_prefs, 32, 0);
	
	if (!gotcapability) 
		iaxs[callno]->peercapability = iaxs[callno]->peerformat;
	if (version > IAX_PROTO_VERSION) {
		ast_log(LOG_WARNING, "Peer '%s' has too new a protocol version (%d) for me\n", 
			ast_inet_ntoa(iabuf, sizeof(iabuf), sin->sin_addr), version);
		return res;
	}
	ast_mutex_lock(&userl.lock);
	/* Search the userlist for a compatible entry, and fill in the rest */
	user = userl.users;
	while(user) {
		if ((ast_strlen_zero(iaxs[callno]->username) ||				/* No username specified */
			!strcmp(iaxs[callno]->username, user->name))	/* Or this username specified */
			&& ast_apply_ha(user->ha, sin) 	/* Access is permitted from this IP */
			&& (ast_strlen_zero(iaxs[callno]->context) ||			/* No context specified */
			     apply_context(user->contexts, iaxs[callno]->context))) {			/* Context is permitted */
			if (!ast_strlen_zero(iaxs[callno]->username)) {
				/* Exact match, stop right now. */
				best = user;
				break;
			} else if (ast_strlen_zero(user->secret) && ast_strlen_zero(user->inkeys)) {
				/* No required authentication */
				if (user->ha) {
					/* There was host authentication and we passed, bonus! */
					if (bestscore < 4) {
						bestscore = 4;
						best = user;
					}
				} else {
					/* No host access, but no secret, either, not bad */
					if (bestscore < 3) {
						bestscore = 3;
						best = user;
					}
				}
			} else {
				if (user->ha) {
					/* Authentication, but host access too, eh, it's something.. */
					if (bestscore < 2) {
						bestscore = 2;
						best = user;
					}
				} else {
					/* Authentication and no host access...  This is our baseline */
					if (bestscore < 1) {
						bestscore = 1;
						best = user;
					}
				}
			}
		}
		user = user->next;	
	}
	ast_mutex_unlock(&userl.lock);
	user = best;
	if (!user && !ast_strlen_zero(iaxs[callno]->username) && (strlen(iaxs[callno]->username) < 128)) {
		user = realtime_user(iaxs[callno]->username);
		if (user && !ast_strlen_zero(iaxs[callno]->context) &&			/* No context specified */
			     !apply_context(user->contexts, iaxs[callno]->context)) {			/* Context is permitted */
			destroy_user(user);
			user = NULL;
		}
	}
	if (user) {
		/* We found our match (use the first) */
		/* copy vars */
		for (v = user->vars ; v ; v = v->next) {
			if((tmpvar = ast_variable_new(v->name, v->value))) {
				tmpvar->next = iaxs[callno]->vars; 
				iaxs[callno]->vars = tmpvar;
			}
		}
		iaxs[callno]->prefs = user->prefs;
		ast_copy_flags(iaxs[callno], user, IAX_CODEC_USER_FIRST);
		ast_copy_flags(iaxs[callno], user, IAX_CODEC_NOPREFS);
		ast_copy_flags(iaxs[callno], user, IAX_CODEC_NOCAP);
		iaxs[callno]->encmethods = user->encmethods;
		/* Store the requested username if not specified */
		if (ast_strlen_zero(iaxs[callno]->username))
			ast_copy_string(iaxs[callno]->username, user->name, sizeof(iaxs[callno]->username));
		/* Store whether this is a trunked call, too, of course, and move if appropriate */
		ast_copy_flags(iaxs[callno], user, IAX_TRUNK);
		iaxs[callno]->capability = user->capability;
		/* And use the default context */
		if (ast_strlen_zero(iaxs[callno]->context)) {
			if (user->contexts)
				ast_copy_string(iaxs[callno]->context, user->contexts->context, sizeof(iaxs[callno]->context));
			else
				ast_copy_string(iaxs[callno]->context, context, sizeof(iaxs[callno]->context));
		}
		/* And any input keys */
		ast_copy_string(iaxs[callno]->inkeys, user->inkeys, sizeof(iaxs[callno]->inkeys));
		/* And the permitted authentication methods */
		iaxs[callno]->authmethods = user->authmethods;
		/* If they have callerid, override the given caller id.  Always store the ANI */
		if (!ast_strlen_zero(iaxs[callno]->cid_num) || !ast_strlen_zero(iaxs[callno]->cid_name)) {
			if (ast_test_flag(user, IAX_HASCALLERID)) {
				iaxs[callno]->calling_tns = 0;
				iaxs[callno]->calling_ton = 0;
				ast_copy_string(iaxs[callno]->cid_num, user->cid_num, sizeof(iaxs[callno]->cid_num));
				ast_copy_string(iaxs[callno]->cid_name, user->cid_name, sizeof(iaxs[callno]->cid_name));
				iaxs[callno]->calling_pres = AST_PRES_ALLOWED_USER_NUMBER_PASSED_SCREEN;
			}
			ast_copy_string(iaxs[callno]->ani, user->cid_num, sizeof(iaxs[callno]->ani));
		} else {
			iaxs[callno]->calling_pres = AST_PRES_NUMBER_NOT_AVAILABLE;
		}
		if (!ast_strlen_zero(user->accountcode))
			ast_copy_string(iaxs[callno]->accountcode, user->accountcode, sizeof(iaxs[callno]->accountcode));
		if (user->amaflags)
			iaxs[callno]->amaflags = user->amaflags;
		if (!ast_strlen_zero(user->language))
			ast_copy_string(iaxs[callno]->language, user->language, sizeof(iaxs[callno]->language));
		ast_copy_flags(iaxs[callno], user, IAX_NOTRANSFER | IAX_USEJITTERBUF | IAX_FORCEJITTERBUF);	
		/* Keep this check last */
		if (!ast_strlen_zero(user->dbsecret)) {
			char *family, *key=NULL;
			family = ast_strdupa(user->dbsecret);
			key = strchr(family, '/');
			if (key)
				*key++ = '\0';
			if (!key || ast_db_get(family, key, iaxs[callno]->secret, sizeof(iaxs[callno]->secret))) {
				ast_log(LOG_WARNING, "Unable to retrieve database password for family/key '%s'!\n", user->dbsecret);
				if (ast_test_flag(user, IAX_TEMPONLY)) {
					destroy_user(user);
					user = NULL;
				}
			}
		} else
			ast_copy_string(iaxs[callno]->secret, user->secret, sizeof(iaxs[callno]->secret)); 
		res = 0;
	}
	ast_set2_flag(iaxs[callno], iax2_getpeertrunk(*sin), IAX_TRUNK);	
	return res;
}

static int raw_hangup(struct sockaddr_in *sin, unsigned short src, unsigned short dst, int sockfd)
{
	struct ast_iax2_full_hdr fh;
	char iabuf[INET_ADDRSTRLEN];
	fh.scallno = htons(src | IAX_FLAG_FULL);
	fh.dcallno = htons(dst);
	fh.ts = 0;
	fh.oseqno = 0;
	fh.iseqno = 0;
	fh.type = AST_FRAME_IAX;
	fh.csub = compress_subclass(IAX_COMMAND_INVAL);
	if (iaxdebug)
		 iax_showframe(NULL, &fh, 0, sin, 0);
#if 0
	if (option_debug)
#endif	
		ast_log(LOG_DEBUG, "Raw Hangup %s:%d, src=%d, dst=%d\n",
			ast_inet_ntoa(iabuf, sizeof(iabuf), sin->sin_addr), ntohs(sin->sin_port), src, dst);
	return sendto(sockfd, &fh, sizeof(fh), 0, (struct sockaddr *)sin, sizeof(*sin));
}

static void merge_encryption(struct chan_iax2_pvt *p, unsigned int enc)
{
	/* Select exactly one common encryption if there are any */
	p->encmethods &= enc;
	if (p->encmethods) {
		if (p->encmethods & IAX_ENCRYPT_AES128)
			p->encmethods = IAX_ENCRYPT_AES128;
		else
			p->encmethods = 0;
	}
}

static int authenticate_request(struct chan_iax2_pvt *p)
{
	struct iax_ie_data ied;
	int res;
	memset(&ied, 0, sizeof(ied));
	iax_ie_append_short(&ied, IAX_IE_AUTHMETHODS, p->authmethods);
	if (p->authmethods & (IAX_AUTH_MD5 | IAX_AUTH_RSA)) {
		snprintf(p->challenge, sizeof(p->challenge), "%d", rand());
		iax_ie_append_str(&ied, IAX_IE_CHALLENGE, p->challenge);
	}
	if (p->encmethods)
		iax_ie_append_short(&ied, IAX_IE_ENCRYPTION, p->encmethods);
	iax_ie_append_str(&ied,IAX_IE_USERNAME, p->username);
	res = send_command(p, AST_FRAME_IAX, IAX_COMMAND_AUTHREQ, 0, ied.buf, ied.pos, -1);
	if (p->encmethods)
		ast_set_flag(p, IAX_ENCRYPTED);
	return res;
}

static int authenticate_verify(struct chan_iax2_pvt *p, struct iax_ies *ies)
{
	char requeststr[256];
	char md5secret[256] = "";
	char secret[256] = "";
	char rsasecret[256] = "";
	int res = -1; 
	int x;
	
	if (!ast_test_flag(&p->state, IAX_STATE_AUTHENTICATED))
		return res;
	if (ies->password)
		ast_copy_string(secret, ies->password, sizeof(secret));
	if (ies->md5_result)
		ast_copy_string(md5secret, ies->md5_result, sizeof(md5secret));
	if (ies->rsa_result)
		ast_copy_string(rsasecret, ies->rsa_result, sizeof(rsasecret));
	if ((p->authmethods & IAX_AUTH_RSA) && !ast_strlen_zero(rsasecret) && !ast_strlen_zero(p->inkeys)) {
		struct ast_key *key;
		char *keyn;
		char tmpkey[256];
		char *stringp=NULL;
		ast_copy_string(tmpkey, p->inkeys, sizeof(tmpkey));
		stringp=tmpkey;
		keyn = strsep(&stringp, ":");
		while(keyn) {
			key = ast_key_get(keyn, AST_KEY_PUBLIC);
			if (key && !ast_check_signature(key, p->challenge, rsasecret)) {
				res = 0;
				break;
			} else if (!key)
				ast_log(LOG_WARNING, "requested inkey '%s' for RSA authentication does not exist\n", keyn);
			keyn = strsep(&stringp, ":");
		}
	} else if (p->authmethods & IAX_AUTH_MD5) {
		struct MD5Context md5;
		unsigned char digest[16];
		char *tmppw, *stringp;
		
		stringp = ast_strdupa(p->secret);
		while ((tmppw = strsep(&stringp, ";"))) {
			MD5Init(&md5);
			MD5Update(&md5, (unsigned char *)p->challenge, strlen(p->challenge));
			MD5Update(&md5, (unsigned char *)tmppw, strlen(tmppw));
			MD5Final(digest, &md5);
			/* If they support md5, authenticate with it.  */
			for (x=0;x<16;x++)
				sprintf(requeststr + (x << 1), "%2.2x", digest[x]); /* safe */
			if (!strcasecmp(requeststr, md5secret)) {
				res = 0;
				break;
			}
		}
	} else if (p->authmethods & IAX_AUTH_PLAINTEXT) {
		if (!strcmp(secret, p->secret))
			res = 0;
	}
	return res;
}

/*! \brief Verify inbound registration */
static int register_verify(int callno, struct sockaddr_in *sin, struct iax_ies *ies)
{
	char requeststr[256] = "";
	char peer[256] = "";
	char md5secret[256] = "";
	char rsasecret[256] = "";
	char secret[256] = "";
	char iabuf[INET_ADDRSTRLEN];
	struct iax2_peer *p;
	struct ast_key *key;
	char *keyn;
	int x;
	int expire = 0;

	ast_clear_flag(&iaxs[callno]->state, IAX_STATE_AUTHENTICATED);
	iaxs[callno]->peer[0] = '\0';
	if (ies->username)
		ast_copy_string(peer, ies->username, sizeof(peer));
	if (ies->password)
		ast_copy_string(secret, ies->password, sizeof(secret));
	if (ies->md5_result)
		ast_copy_string(md5secret, ies->md5_result, sizeof(md5secret));
	if (ies->rsa_result)
		ast_copy_string(rsasecret, ies->rsa_result, sizeof(rsasecret));
	if (ies->refresh)
		expire = ies->refresh;

	if (ast_strlen_zero(peer)) {
		ast_log(LOG_NOTICE, "Empty registration from %s\n", ast_inet_ntoa(iabuf, sizeof(iabuf), sin->sin_addr));
		return -1;
	}
	/* We release the lock for the call to prevent a deadlock, but it's okay because
	   only the current thread could possibly make it go away or make changes */
	ast_mutex_unlock(&iaxsl[callno]);
	/* SLD: first call to lookup peer during registration */
	p = find_peer(peer, 1);
	ast_mutex_lock(&iaxsl[callno]);

	if (!p) {
		if (authdebug)
			ast_log(LOG_NOTICE, "No registration for peer '%s' (from %s)\n", peer, ast_inet_ntoa(iabuf, sizeof(iabuf), sin->sin_addr));
		return -1;
	}

	if (!ast_test_flag(p, IAX_DYNAMIC)) {
		if (authdebug)
			ast_log(LOG_NOTICE, "Peer '%s' is not dynamic (from %s)\n", peer, ast_inet_ntoa(iabuf, sizeof(iabuf), sin->sin_addr));
		if (ast_test_flag(p, IAX_TEMPONLY))
			destroy_peer(p);
		return -1;
	}

	if (!ast_apply_ha(p->ha, sin)) {
		if (authdebug)
			ast_log(LOG_NOTICE, "Host %s denied access to register peer '%s'\n", ast_inet_ntoa(iabuf, sizeof(iabuf), sin->sin_addr), p->name);
		if (ast_test_flag(p, IAX_TEMPONLY))
			destroy_peer(p);
		return -1;
	}
	ast_copy_string(iaxs[callno]->secret, p->secret, sizeof(iaxs[callno]->secret));
	ast_copy_string(iaxs[callno]->inkeys, p->inkeys, sizeof(iaxs[callno]->inkeys));
	/* Check secret against what we have on file */
	if (!ast_strlen_zero(rsasecret) && (p->authmethods & IAX_AUTH_RSA) && !ast_strlen_zero(iaxs[callno]->challenge)) {
		if (!ast_strlen_zero(p->inkeys)) {
			char tmpkeys[256];
			char *stringp=NULL;
			ast_copy_string(tmpkeys, p->inkeys, sizeof(tmpkeys));
			stringp=tmpkeys;
			keyn = strsep(&stringp, ":");
			while(keyn) {
				key = ast_key_get(keyn, AST_KEY_PUBLIC);
				if (key && !ast_check_signature(key, iaxs[callno]->challenge, rsasecret)) {
					ast_set_flag(&iaxs[callno]->state, IAX_STATE_AUTHENTICATED);
					break;
				} else if (!key) 
					ast_log(LOG_WARNING, "requested inkey '%s' does not exist\n", keyn);
				keyn = strsep(&stringp, ":");
			}
			if (!keyn) {
				if (authdebug)
					ast_log(LOG_NOTICE, "Host %s failed RSA authentication with inkeys '%s'\n", peer, p->inkeys);
				if (ast_test_flag(p, IAX_TEMPONLY))
					destroy_peer(p);
				return -1;
			}
		} else {
			if (authdebug)
				ast_log(LOG_NOTICE, "Host '%s' trying to do RSA authentication, but we have no inkeys\n", peer);
			if (ast_test_flag(p, IAX_TEMPONLY))
				destroy_peer(p);
			return -1;
		}
	} else if (!ast_strlen_zero(secret) && (p->authmethods & IAX_AUTH_PLAINTEXT)) {
		/* They've provided a plain text password and we support that */
		if (strcmp(secret, p->secret)) {
			if (authdebug)
				ast_log(LOG_NOTICE, "Host %s did not provide proper plaintext password for '%s'\n", ast_inet_ntoa(iabuf, sizeof(iabuf), sin->sin_addr), p->name);
			if (ast_test_flag(p, IAX_TEMPONLY))
				destroy_peer(p);
			return -1;
		} else
			ast_set_flag(&iaxs[callno]->state, IAX_STATE_AUTHENTICATED);
	} else if (!ast_strlen_zero(md5secret) && (p->authmethods & IAX_AUTH_MD5) && !ast_strlen_zero(iaxs[callno]->challenge)) {
		struct MD5Context md5;
		unsigned char digest[16];
		char *tmppw, *stringp;
		
		stringp = ast_strdupa(p->secret);
		while ((tmppw = strsep(&stringp, ";"))) {
			MD5Init(&md5);
			MD5Update(&md5, (unsigned char *)iaxs[callno]->challenge, strlen(iaxs[callno]->challenge));
			MD5Update(&md5, (unsigned char *)tmppw, strlen(tmppw));
			MD5Final(digest, &md5);
			for (x=0;x<16;x++)
				sprintf(requeststr + (x << 1), "%2.2x", digest[x]); /* safe */
			if (!strcasecmp(requeststr, md5secret)) 
				break;
		}
		if (tmppw) {
			ast_set_flag(&iaxs[callno]->state, IAX_STATE_AUTHENTICATED);
		} else {
			if (authdebug)
				ast_log(LOG_NOTICE, "Host %s failed MD5 authentication for '%s' (%s != %s)\n", ast_inet_ntoa(iabuf, sizeof(iabuf), sin->sin_addr), p->name, requeststr, md5secret);
			if (ast_test_flag(p, IAX_TEMPONLY))
				destroy_peer(p);
			return -1;
		}
	} else if (!ast_strlen_zero(md5secret) || !ast_strlen_zero(secret)) {
		if (authdebug)
			ast_log(LOG_NOTICE, "Inappropriate authentication received\n");
		if (ast_test_flag(p, IAX_TEMPONLY))
			destroy_peer(p);
		return -1;
	}
	ast_copy_string(iaxs[callno]->peer, peer, sizeof(iaxs[callno]->peer));
	/* Choose lowest expiry number */
	if (expire && (expire < iaxs[callno]->expiry)) 
		iaxs[callno]->expiry = expire;

	ast_device_state_changed("IAX2/%s", p->name); /* Activate notification */

	if (ast_test_flag(p, IAX_TEMPONLY))
		destroy_peer(p);
	return 0;
	
}

static int authenticate(char *challenge, char *secret, char *keyn, int authmethods, struct iax_ie_data *ied, struct sockaddr_in *sin, aes_encrypt_ctx *ecx, aes_decrypt_ctx *dcx)
{
	int res = -1;
	int x;
	char iabuf[INET_ADDRSTRLEN];
	if (!ast_strlen_zero(keyn)) {
		if (!(authmethods & IAX_AUTH_RSA)) {
			if (ast_strlen_zero(secret)) 
				ast_log(LOG_NOTICE, "Asked to authenticate to %s with an RSA key, but they don't allow RSA authentication\n", ast_inet_ntoa(iabuf, sizeof(iabuf), sin->sin_addr));
		} else if (ast_strlen_zero(challenge)) {
			ast_log(LOG_NOTICE, "No challenge provided for RSA authentication to %s\n", ast_inet_ntoa(iabuf, sizeof(iabuf), sin->sin_addr));
		} else {
			char sig[256];
			struct ast_key *key;
			key = ast_key_get(keyn, AST_KEY_PRIVATE);
			if (!key) {
				ast_log(LOG_NOTICE, "Unable to find private key '%s'\n", keyn);
			} else {
				if (ast_sign(key, challenge, sig)) {
					ast_log(LOG_NOTICE, "Unable to sign challenge withy key\n");
					res = -1;
				} else {
					iax_ie_append_str(ied, IAX_IE_RSA_RESULT, sig);
					res = 0;
				}
			}
		}
	} 
	/* Fall back */
	if (res && !ast_strlen_zero(secret)) {
		if ((authmethods & IAX_AUTH_MD5) && !ast_strlen_zero(challenge)) {
			struct MD5Context md5;
			unsigned char digest[16];
			char digres[128];
			MD5Init(&md5);
			MD5Update(&md5, (unsigned char *)challenge, strlen(challenge));
			MD5Update(&md5, (unsigned char *)secret, strlen(secret));
			MD5Final(digest, &md5);
			/* If they support md5, authenticate with it.  */
			for (x=0;x<16;x++)
				sprintf(digres + (x << 1),  "%2.2x", digest[x]); /* safe */
			if (ecx && dcx)
				build_enc_keys(digest, ecx, dcx);
			iax_ie_append_str(ied, IAX_IE_MD5_RESULT, digres);
			res = 0;
		} else if (authmethods & IAX_AUTH_PLAINTEXT) {
			iax_ie_append_str(ied, IAX_IE_PASSWORD, secret);
			res = 0;
		} else
			ast_log(LOG_NOTICE, "No way to send secret to peer '%s' (their methods: %d)\n", ast_inet_ntoa(iabuf, sizeof(iabuf), sin->sin_addr), authmethods);
	}
	return res;
}

static int authenticate_reply(struct chan_iax2_pvt *p, struct sockaddr_in *sin, struct iax_ies *ies, char *override, char *okey)
{
	struct iax2_peer *peer;
	/* Start pessimistic */
	int res = -1;
	int authmethods = 0;
	struct iax_ie_data ied;
	
	memset(&ied, 0, sizeof(ied));
	
	if (ies->username)
		ast_copy_string(p->username, ies->username, sizeof(p->username));
	if (ies->challenge)
		ast_copy_string(p->challenge, ies->challenge, sizeof(p->challenge));
	if (ies->authmethods)
		authmethods = ies->authmethods;
	if (authmethods & IAX_AUTH_MD5)
		merge_encryption(p, ies->encmethods);
	else
		p->encmethods = 0;

	/* Check for override RSA authentication first */
	if (!ast_strlen_zero(override) || !ast_strlen_zero(okey)) {
		/* Normal password authentication */
		res = authenticate(p->challenge, override, okey, authmethods, &ied, sin, &p->ecx, &p->dcx);
	} else {
		ast_mutex_lock(&peerl.lock);
		peer = peerl.peers;
		while(peer) {
			if ((ast_strlen_zero(p->peer) || !strcmp(p->peer, peer->name)) 
								/* No peer specified at our end, or this is the peer */
			 && (ast_strlen_zero(peer->username) || (!strcmp(peer->username, p->username)))
			 					/* No username specified in peer rule, or this is the right username */
			 && (!peer->addr.sin_addr.s_addr || ((sin->sin_addr.s_addr & peer->mask.s_addr) == (peer->addr.sin_addr.s_addr & peer->mask.s_addr)))
			 					/* No specified host, or this is our host */
			) {
				res = authenticate(p->challenge, peer->secret, peer->outkey, authmethods, &ied, sin, &p->ecx, &p->dcx);
				if (!res)
					break;	
			}
			peer = peer->next;
		}
		ast_mutex_unlock(&peerl.lock);
		if (!peer) {
			/* We checked our list and didn't find one.  It's unlikely, but possible, 
			   that we're trying to authenticate *to* a realtime peer */
			if ((peer = realtime_peer(p->peer, NULL))) {
				res = authenticate(p->challenge, peer->secret,peer->outkey, authmethods, &ied, sin, &p->ecx, &p->dcx);
				if (ast_test_flag(peer, IAX_TEMPONLY))
					destroy_peer(peer);
			}
		}
	}
	if (ies->encmethods)
		ast_set_flag(p, IAX_ENCRYPTED | IAX_KEYPOPULATED);
	if (!res)
		res = send_command(p, AST_FRAME_IAX, IAX_COMMAND_AUTHREP, 0, ied.buf, ied.pos, -1);
	return res;
}

static int iax2_do_register(struct iax2_registry *reg);

static int iax2_do_register_s(void *data)
{
	struct iax2_registry *reg = data;
	reg->expire = -1;
	iax2_do_register(reg);
	return 0;
}

static int try_transfer(struct chan_iax2_pvt *pvt, struct iax_ies *ies)
{
	int newcall = 0;
	char newip[256];
	struct iax_ie_data ied;
	struct sockaddr_in new;
	
	
	memset(&ied, 0, sizeof(ied));
	if (ies->apparent_addr)
		memcpy(&new, ies->apparent_addr, sizeof(new));
	if (ies->callno)
		newcall = ies->callno;
	if (!newcall || !new.sin_addr.s_addr || !new.sin_port) {
		ast_log(LOG_WARNING, "Invalid transfer request\n");
		return -1;
	}
	pvt->transfercallno = newcall;
	memcpy(&pvt->transfer, &new, sizeof(pvt->transfer));
	inet_aton(newip, &pvt->transfer.sin_addr);
	pvt->transfer.sin_family = AF_INET;
	pvt->transferring = TRANSFER_BEGIN;
	pvt->transferid = ies->transferid;
	if (ies->transferid)
		iax_ie_append_int(&ied, IAX_IE_TRANSFERID, ies->transferid);
	send_command_transfer(pvt, AST_FRAME_IAX, IAX_COMMAND_TXCNT, 0, ied.buf, ied.pos);
	return 0; 
}

static int complete_dpreply(struct chan_iax2_pvt *pvt, struct iax_ies *ies)
{
	char exten[256] = "";
	int status = CACHE_FLAG_UNKNOWN;
	int expiry = iaxdefaultdpcache;
	int x;
	int matchmore = 0;
	struct iax2_dpcache *dp, *prev;
	
	if (ies->called_number)
		ast_copy_string(exten, ies->called_number, sizeof(exten));

	if (ies->dpstatus & IAX_DPSTATUS_EXISTS)
		status = CACHE_FLAG_EXISTS;
	else if (ies->dpstatus & IAX_DPSTATUS_CANEXIST)
		status = CACHE_FLAG_CANEXIST;
	else if (ies->dpstatus & IAX_DPSTATUS_NONEXISTENT)
		status = CACHE_FLAG_NONEXISTENT;

	if (ies->dpstatus & IAX_DPSTATUS_IGNOREPAT) {
		/* Don't really do anything with this */
	}
	if (ies->refresh)
		expiry = ies->refresh;
	if (ies->dpstatus & IAX_DPSTATUS_MATCHMORE)
		matchmore = CACHE_FLAG_MATCHMORE;
	ast_mutex_lock(&dpcache_lock);
	prev = NULL;
	dp = pvt->dpentries;
	while(dp) {
		if (!strcmp(dp->exten, exten)) {
			/* Let them go */
			if (prev)
				prev->peer = dp->peer;
			else
				pvt->dpentries = dp->peer;
			dp->peer = NULL;
			dp->callno = 0;
			dp->expiry.tv_sec = dp->orig.tv_sec + expiry;
			if (dp->flags & CACHE_FLAG_PENDING) {
				dp->flags &= ~CACHE_FLAG_PENDING;
				dp->flags |= status;
				dp->flags |= matchmore;
			}
			/* Wake up waiters */
			for (x=0;x<sizeof(dp->waiters) / sizeof(dp->waiters[0]); x++)
				if (dp->waiters[x] > -1)
					write(dp->waiters[x], "asdf", 4);
		}
		prev = dp;
		dp = dp->peer;
	}
	ast_mutex_unlock(&dpcache_lock);
	return 0;
}

static int complete_transfer(int callno, struct iax_ies *ies)
{
	int peercallno = 0;
	struct chan_iax2_pvt *pvt = iaxs[callno];
	struct iax_frame *cur;

	if (ies->callno)
		peercallno = ies->callno;

	if (peercallno < 1) {
		ast_log(LOG_WARNING, "Invalid transfer request\n");
		return -1;
	}
	memcpy(&pvt->addr, &pvt->transfer, sizeof(pvt->addr));
	memset(&pvt->transfer, 0, sizeof(pvt->transfer));
	/* Reset sequence numbers */
	pvt->oseqno = 0;
	pvt->rseqno = 0;
	pvt->iseqno = 0;
	pvt->aseqno = 0;
	pvt->peercallno = peercallno;
	pvt->transferring = TRANSFER_NONE;
	pvt->svoiceformat = -1;
	pvt->voiceformat = 0;
	pvt->svideoformat = -1;
	pvt->videoformat = 0;
	pvt->transfercallno = -1;
	memset(&pvt->rxcore, 0, sizeof(pvt->rxcore));
	memset(&pvt->offset, 0, sizeof(pvt->offset));
#ifdef NEWJB
	{	/* reset jitterbuffer */
	    	jb_frame frame;
            	while(jb_getall(pvt->jb,&frame) == JB_OK)
                	iax2_frame_free(frame.data);

		jb_reset(pvt->jb);
	}
#else
	memset(&pvt->history, 0, sizeof(pvt->history));
	pvt->jitterbuffer = 0;
	pvt->jitter = 0;
	pvt->historicjitter = 0;
#endif
	pvt->lag = 0;
	pvt->last = 0;
	pvt->lastsent = 0;
	pvt->nextpred = 0;
	pvt->pingtime = DEFAULT_RETRY_TIME;
	ast_mutex_lock(&iaxq.lock);
	for (cur = iaxq.head; cur ; cur = cur->next) {
		/* We must cancel any packets that would have been transmitted
		   because now we're talking to someone new.  It's okay, they
		   were transmitted to someone that didn't care anyway. */
		if (callno == cur->callno) 
			cur->retries = -1;
	}
	ast_mutex_unlock(&iaxq.lock);
	return 0; 
}

/*! \brief Acknowledgment received for OUR registration */
static int iax2_ack_registry(struct iax_ies *ies, struct sockaddr_in *sin, int callno)
{
	struct iax2_registry *reg;
	/* Start pessimistic */
	char peer[256] = "";
	char msgstatus[40];
	int refresh = 0;
	char ourip[256] = "<Unspecified>";
	struct sockaddr_in oldus;
	struct sockaddr_in us;
	char iabuf[INET_ADDRSTRLEN];
	int oldmsgs;

	memset(&us, 0, sizeof(us));
	if (ies->apparent_addr)
		memcpy(&us, ies->apparent_addr, sizeof(us));
	if (ies->username)
		ast_copy_string(peer, ies->username, sizeof(peer));
	if (ies->refresh)
		refresh = ies->refresh;
	if (ies->calling_number) {
		/* We don't do anything with it really, but maybe we should */
	}
	reg = iaxs[callno]->reg;
	if (!reg) {
		ast_log(LOG_WARNING, "Registry acknowledge on unknown registry '%s'\n", peer);
		return -1;
	}
	memcpy(&oldus, &reg->us, sizeof(oldus));
	oldmsgs = reg->messages;
	if (inaddrcmp(&reg->addr, sin)) {
		ast_log(LOG_WARNING, "Received unsolicited registry ack from '%s'\n", ast_inet_ntoa(iabuf, sizeof(iabuf), sin->sin_addr));
		return -1;
	}
	memcpy(&reg->us, &us, sizeof(reg->us));
	reg->messages = ies->msgcount;
	/* always refresh the registration at the interval requested by the server
	   we are registering to
	*/
	reg->refresh = refresh;
	if (reg->expire > -1)
		ast_sched_del(sched, reg->expire);
	reg->expire = ast_sched_add(sched, (5 * reg->refresh / 6) * 1000, iax2_do_register_s, reg);
	if ((inaddrcmp(&oldus, &reg->us) || (reg->messages != oldmsgs)) && (option_verbose > 2)) {
		if (reg->messages > 65534)
			snprintf(msgstatus, sizeof(msgstatus), " with message(s) waiting\n");
		else if (reg->messages > 1)
			snprintf(msgstatus, sizeof(msgstatus), " with %d messages waiting\n", reg->messages);
		else if (reg->messages > 0)
			snprintf(msgstatus, sizeof(msgstatus), " with 1 message waiting\n");
		else
			snprintf(msgstatus, sizeof(msgstatus), " with no messages waiting\n");
		snprintf(ourip, sizeof(ourip), "%s:%d", ast_inet_ntoa(iabuf, sizeof(iabuf), reg->us.sin_addr), ntohs(reg->us.sin_port));
		ast_verbose(VERBOSE_PREFIX_3 "Registered IAX2 to '%s', who sees us as %s%s\n", ast_inet_ntoa(iabuf, sizeof(iabuf), sin->sin_addr), ourip, msgstatus);
		manager_event(EVENT_FLAG_SYSTEM, "Registry", "Channel: IAX2\r\nDomain: %s\r\nStatus: Registered\r\n", ast_inet_ntoa(iabuf, sizeof(iabuf), sin->sin_addr));
	}
	reg->regstate = REG_STATE_REGISTERED;
	return 0;
}

static int iax2_register(char *value, int lineno)
{
	struct iax2_registry *reg;
	char copy[256];
	char *username, *hostname, *secret;
	char *porta;
	char *stringp=NULL;
	
	struct ast_hostent ahp; struct hostent *hp;
	if (!value)
		return -1;
	ast_copy_string(copy, value, sizeof(copy));
	stringp=copy;
	username = strsep(&stringp, "@");
	hostname = strsep(&stringp, "@");
	if (!hostname) {
		ast_log(LOG_WARNING, "Format for registration is user[:secret]@host[:port] at line %d", lineno);
		return -1;
	}
	stringp=username;
	username = strsep(&stringp, ":");
	secret = strsep(&stringp, ":");
	stringp=hostname;
	hostname = strsep(&stringp, ":");
	porta = strsep(&stringp, ":");
	
	if (porta && !atoi(porta)) {
		ast_log(LOG_WARNING, "%s is not a valid port number at line %d\n", porta, lineno);
		return -1;
	}
	hp = ast_gethostbyname(hostname, &ahp);
	if (!hp) {
		ast_log(LOG_WARNING, "Host '%s' not found at line %d\n", hostname, lineno);
		return -1;
	}
	if (!(reg = ast_calloc(1, sizeof(*reg))))
		return -1;
	ast_copy_string(reg->username, username, sizeof(reg->username));
	if (secret)
		ast_copy_string(reg->secret, secret, sizeof(reg->secret));
	reg->expire = -1;
	reg->refresh = IAX_DEFAULT_REG_EXPIRE;
	reg->addr.sin_family = AF_INET;
	memcpy(&reg->addr.sin_addr, hp->h_addr, sizeof(&reg->addr.sin_addr));
	reg->addr.sin_port = porta ? htons(atoi(porta)) : htons(IAX_DEFAULT_PORTNO);
	reg->next = registrations;
	reg->callno = 0;
	registrations = reg;
	
	return 0;
}

static void register_peer_exten(struct iax2_peer *peer, int onoff)
{
	char multi[256];
	char *stringp, *ext;
	if (!ast_strlen_zero(regcontext)) {
		ast_copy_string(multi, ast_strlen_zero(peer->regexten) ? peer->name : peer->regexten, sizeof(multi));
		stringp = multi;
		while((ext = strsep(&stringp, "&"))) {
			if (onoff) {
				if (!ast_exists_extension(NULL, regcontext, ext, 1, NULL))
					ast_add_extension(regcontext, 1, ext, 1, NULL, NULL, "Noop", ast_strdup(peer->name), free, channeltype);
			} else
				ast_context_remove_extension(regcontext, ext, 1, NULL);
		}
	}
}
static void prune_peers(void);

static int expire_registry(void *data)
{
	struct iax2_peer *p = data;

	ast_log(LOG_DEBUG, "Expiring registration for peer '%s'\n", p->name);
	/* Reset the address */
	memset(&p->addr, 0, sizeof(p->addr));
	/* Reset expire notice */
	p->expire = -1;
	/* Reset expiry value */
	p->expiry = min_reg_expire;
	if (!ast_test_flag(p, IAX_TEMPONLY))
		ast_db_del("IAX/Registry", p->name);
	register_peer_exten(p, 0);
	ast_device_state_changed("IAX2/%s", p->name); /* Activate notification */
	if (iax2_regfunk)
		iax2_regfunk(p->name, 0);

	if (ast_test_flag(p, IAX_RTAUTOCLEAR)) {
		ast_set_flag(p, IAX_DELME);
		prune_peers();
	}

	return 0;
}


static int iax2_poke_peer(struct iax2_peer *peer, int heldcall);

static void reg_source_db(struct iax2_peer *p)
{
	char data[80];
	struct in_addr in;
	char iabuf[INET_ADDRSTRLEN];
	char *c, *d;
	if (!ast_test_flag(p, IAX_TEMPONLY) && (!ast_db_get("IAX/Registry", p->name, data, sizeof(data)))) {
		c = strchr(data, ':');
		if (c) {
			*c = '\0';
			c++;
			if (inet_aton(data, &in)) {
				d = strchr(c, ':');
				if (d) {
					*d = '\0';
					d++;
					if (option_verbose > 2)
						ast_verbose(VERBOSE_PREFIX_3 "Seeding '%s' at %s:%d for %d\n", p->name, 
						ast_inet_ntoa(iabuf, sizeof(iabuf), in), atoi(c), atoi(d));
					iax2_poke_peer(p, 0);
					p->expiry = atoi(d);
					memset(&p->addr, 0, sizeof(p->addr));
					p->addr.sin_family = AF_INET;
					p->addr.sin_addr = in;
					p->addr.sin_port = htons(atoi(c));
					if (p->expire > -1)
						ast_sched_del(sched, p->expire);
					ast_device_state_changed("IAX2/%s", p->name); /* Activate notification */
					p->expire = ast_sched_add(sched, (p->expiry + 10) * 1000, expire_registry, (void *)p);
					if (iax2_regfunk)
						iax2_regfunk(p->name, 1);
					register_peer_exten(p, 1);
				}					
					
			}
		}
	}
}

static int update_registry(char *name, struct sockaddr_in *sin, int callno, char *devtype, int fd, unsigned short refresh)
{
	/* Called from IAX thread only, with proper iaxsl lock */
	struct iax_ie_data ied;
	struct iax2_peer *p;
	int msgcount;
	char data[80];
	char iabuf[INET_ADDRSTRLEN];
	int version;

	memset(&ied, 0, sizeof(ied));

	/* SLD: Another find_peer call during registration - this time when we are really updating our registration */
	if (!(p = find_peer(name, 1))) {
		ast_log(LOG_WARNING, "No such peer '%s'\n", name);
		return -1;
	}

	if (ast_test_flag((&globalflags), IAX_RTUPDATE) && (ast_test_flag(p, IAX_TEMPONLY|IAX_RTCACHEFRIENDS)))
		realtime_update_peer(name, sin);
	if (inaddrcmp(&p->addr, sin)) {
		if (iax2_regfunk)
			iax2_regfunk(p->name, 1);
		/* Stash the IP address from which they registered */
		memcpy(&p->addr, sin, sizeof(p->addr));
		snprintf(data, sizeof(data), "%s:%d:%d", ast_inet_ntoa(iabuf, sizeof(iabuf), sin->sin_addr), ntohs(sin->sin_port), p->expiry);
		if (!ast_test_flag(p, IAX_TEMPONLY) && sin->sin_addr.s_addr) {
			ast_db_put("IAX/Registry", p->name, data);
			if  (option_verbose > 2)
				ast_verbose(VERBOSE_PREFIX_3 "Registered IAX2 '%s' (%s) at %s:%d\n", p->name, 
					    ast_test_flag(&iaxs[callno]->state, IAX_STATE_AUTHENTICATED) ? "AUTHENTICATED" : "UNAUTHENTICATED", ast_inet_ntoa(iabuf, sizeof(iabuf), sin->sin_addr), ntohs(sin->sin_port));
			manager_event(EVENT_FLAG_SYSTEM, "PeerStatus", "Peer: IAX2/%s\r\nPeerStatus: Registered\r\n", p->name);
			register_peer_exten(p, 1);
			ast_device_state_changed("IAX2/%s", p->name); /* Activate notification */
		} else if (!ast_test_flag(p, IAX_TEMPONLY)) {
			if  (option_verbose > 2)
				ast_verbose(VERBOSE_PREFIX_3 "Unregistered IAX2 '%s' (%s)\n", p->name, 
					    ast_test_flag(&iaxs[callno]->state, IAX_STATE_AUTHENTICATED) ? "AUTHENTICATED" : "UNAUTHENTICATED");
			manager_event(EVENT_FLAG_SYSTEM, "PeerStatus", "Peer: IAX2/%s\r\nPeerStatus: Unregistered\r\n", p->name);
			register_peer_exten(p, 0);
			ast_db_del("IAX/Registry", p->name);
			ast_device_state_changed("IAX2/%s", p->name); /* Activate notification */
		}
		/* Update the host */
		/* Verify that the host is really there */
		iax2_poke_peer(p, callno);
	}		
	/* Store socket fd */
	p->sockfd = fd;
	/* Setup the expiry */
	if (p->expire > -1)
		ast_sched_del(sched, p->expire);
	/* treat an unspecified refresh interval as the minimum */
	if (!refresh)
		refresh = min_reg_expire;
	if (refresh > max_reg_expire) {
		ast_log(LOG_NOTICE, "Restricting registration for peer '%s' to %d seconds (requested %d)\n",
			p->name, max_reg_expire, refresh);
		p->expiry = max_reg_expire;
	} else if (refresh < min_reg_expire) {
		ast_log(LOG_NOTICE, "Restricting registration for peer '%s' to %d seconds (requested %d)\n",
			p->name, min_reg_expire, refresh);
		p->expiry = min_reg_expire;
	} else {
		p->expiry = refresh;
	}
	if (p->expiry && sin->sin_addr.s_addr)
		p->expire = ast_sched_add(sched, (p->expiry + 10) * 1000, expire_registry, (void *)p);
	iax_ie_append_str(&ied, IAX_IE_USERNAME, p->name);
	iax_ie_append_int(&ied, IAX_IE_DATETIME, iax2_datetime(p->zonetag));
	if (sin->sin_addr.s_addr) {
		iax_ie_append_short(&ied, IAX_IE_REFRESH, p->expiry);
		iax_ie_append_addr(&ied, IAX_IE_APPARENT_ADDR, &p->addr);
		if (!ast_strlen_zero(p->mailbox)) {
			if (ast_test_flag(p, IAX_MESSAGEDETAIL)) {
				int new, old;
				ast_app_messagecount(p->mailbox, &new, &old);
				if (new > 255)
					new = 255;
				if (old > 255)
					old = 255;
				msgcount = (old << 8) | new;
			} else {
				msgcount = ast_app_has_voicemail(p->mailbox, NULL);
				if (msgcount)
					msgcount = 65535;
			}
			iax_ie_append_short(&ied, IAX_IE_MSGCOUNT, msgcount);
		}
		if (ast_test_flag(p, IAX_HASCALLERID)) {
			iax_ie_append_str(&ied, IAX_IE_CALLING_NUMBER, p->cid_num);
			iax_ie_append_str(&ied, IAX_IE_CALLING_NAME, p->cid_name);
		}
	}
	version = iax_check_version(devtype);
	if (version) 
		iax_ie_append_short(&ied, IAX_IE_FIRMWAREVER, version);
	if (ast_test_flag(p, IAX_TEMPONLY))
		destroy_peer(p);
	return send_command_final(iaxs[callno], AST_FRAME_IAX, IAX_COMMAND_REGACK, 0, ied.buf, ied.pos, -1);
}

static int registry_authrequest(char *name, int callno)
{
	struct iax_ie_data ied;
	struct iax2_peer *p;
	/* SLD: third call to find_peer in registration */
	p = find_peer(name, 1);
	if (p) {
		memset(&ied, 0, sizeof(ied));
		iax_ie_append_short(&ied, IAX_IE_AUTHMETHODS, p->authmethods);
		if (p->authmethods & (IAX_AUTH_RSA | IAX_AUTH_MD5)) {
			/* Build the challenge */
			snprintf(iaxs[callno]->challenge, sizeof(iaxs[callno]->challenge), "%d", rand());
			iax_ie_append_str(&ied, IAX_IE_CHALLENGE, iaxs[callno]->challenge);
		}
		iax_ie_append_str(&ied, IAX_IE_USERNAME, name);
		if (ast_test_flag(p, IAX_TEMPONLY))
			destroy_peer(p);
		return send_command(iaxs[callno], AST_FRAME_IAX, IAX_COMMAND_REGAUTH, 0, ied.buf, ied.pos, -1);;
	} 
	ast_log(LOG_WARNING, "No such peer '%s'\n", name);
	return 0;
}

static int registry_rerequest(struct iax_ies *ies, int callno, struct sockaddr_in *sin)
{
	struct iax2_registry *reg;
	/* Start pessimistic */
	struct iax_ie_data ied;
	char peer[256] = "";
	char iabuf[INET_ADDRSTRLEN];
	char challenge[256] = "";
	int res;
	int authmethods = 0;
	if (ies->authmethods)
		authmethods = ies->authmethods;
	if (ies->username)
		ast_copy_string(peer, ies->username, sizeof(peer));
	if (ies->challenge)
		ast_copy_string(challenge, ies->challenge, sizeof(challenge));
	memset(&ied, 0, sizeof(ied));
	reg = iaxs[callno]->reg;
	if (reg) {
			if (inaddrcmp(&reg->addr, sin)) {
				ast_log(LOG_WARNING, "Received unsolicited registry authenticate request from '%s'\n", ast_inet_ntoa(iabuf, sizeof(iabuf), sin->sin_addr));
				return -1;
			}
			if (ast_strlen_zero(reg->secret)) {
				ast_log(LOG_NOTICE, "No secret associated with peer '%s'\n", reg->username);
				reg->regstate = REG_STATE_NOAUTH;
				return -1;
			}
			iax_ie_append_str(&ied, IAX_IE_USERNAME, reg->username);
			iax_ie_append_short(&ied, IAX_IE_REFRESH, reg->refresh);
			if (reg->secret[0] == '[') {
				char tmpkey[256];
				ast_copy_string(tmpkey, reg->secret + 1, sizeof(tmpkey));
				tmpkey[strlen(tmpkey) - 1] = '\0';
				res = authenticate(challenge, NULL, tmpkey, authmethods, &ied, sin, NULL, NULL);
			} else
				res = authenticate(challenge, reg->secret, NULL, authmethods, &ied, sin, NULL, NULL);
			if (!res) {
				reg->regstate = REG_STATE_AUTHSENT;
				return send_command(iaxs[callno], AST_FRAME_IAX, IAX_COMMAND_REGREQ, 0, ied.buf, ied.pos, -1);
			} else
				return -1;
			ast_log(LOG_WARNING, "Registry acknowledge on unknown registery '%s'\n", peer);
	} else	
		ast_log(LOG_NOTICE, "Can't reregister without a reg\n");
	return -1;
}

static int stop_stuff(int callno)
{
		if (iaxs[callno]->lagid > -1)
			ast_sched_del(sched, iaxs[callno]->lagid);
		iaxs[callno]->lagid = -1;
		if (iaxs[callno]->pingid > -1)
			ast_sched_del(sched, iaxs[callno]->pingid);
		iaxs[callno]->pingid = -1;
		if (iaxs[callno]->autoid > -1)
			ast_sched_del(sched, iaxs[callno]->autoid);
		iaxs[callno]->autoid = -1;
		if (iaxs[callno]->initid > -1)
			ast_sched_del(sched, iaxs[callno]->initid);
		iaxs[callno]->initid = -1;
		if (iaxs[callno]->authid > -1)
			ast_sched_del(sched, iaxs[callno]->authid);
		iaxs[callno]->authid = -1;
#ifdef NEWJB
		if (iaxs[callno]->jbid > -1)
			ast_sched_del(sched, iaxs[callno]->jbid);
		iaxs[callno]->jbid = -1;
#endif
		return 0;
}

static int auth_reject(void *nothing)
{
	/* Called from IAX thread only, without iaxs lock */
	int callno = (int)(long)(nothing);
	struct iax_ie_data ied;
	ast_mutex_lock(&iaxsl[callno]);
	if (iaxs[callno]) {
		iaxs[callno]->authid = -1;
		memset(&ied, 0, sizeof(ied));
		if (iaxs[callno]->authfail == IAX_COMMAND_REGREJ) {
			iax_ie_append_str(&ied, IAX_IE_CAUSE, "Registration Refused");
			iax_ie_append_byte(&ied, IAX_IE_CAUSECODE, AST_CAUSE_FACILITY_REJECTED);
		} else if (iaxs[callno]->authfail == IAX_COMMAND_REJECT) {
			iax_ie_append_str(&ied, IAX_IE_CAUSE, "No authority found");
			iax_ie_append_byte(&ied, IAX_IE_CAUSECODE, AST_CAUSE_FACILITY_NOT_SUBSCRIBED);
		}
		send_command_final(iaxs[callno], AST_FRAME_IAX, iaxs[callno]->authfail, 0, ied.buf, ied.pos, -1);
	}
	ast_mutex_unlock(&iaxsl[callno]);
	return 0;
}

static int auth_fail(int callno, int failcode)
{
	/* Schedule sending the authentication failure in one second, to prevent
	   guessing */
	ast_mutex_lock(&iaxsl[callno]);
	iaxs[callno]->authfail = failcode;
	if (delayreject) {
		ast_mutex_lock(&iaxsl[callno]);
		if (iaxs[callno]->authid > -1)
			ast_sched_del(sched, iaxs[callno]->authid);
		iaxs[callno]->authid = ast_sched_add(sched, 1000, auth_reject, (void *)(long)callno);
		ast_mutex_unlock(&iaxsl[callno]);
	} else
		auth_reject((void *)(long)callno);
	ast_mutex_unlock(&iaxsl[callno]);
	return 0;
}

static int auto_hangup(void *nothing)
{
	/* Called from IAX thread only, without iaxs lock */
	int callno = (int)(long)(nothing);
	struct iax_ie_data ied;
	ast_mutex_lock(&iaxsl[callno]);
	if (iaxs[callno]) {
		iaxs[callno]->autoid = -1;
		memset(&ied, 0, sizeof(ied));
		iax_ie_append_str(&ied, IAX_IE_CAUSE, "Timeout");
		iax_ie_append_byte(&ied, IAX_IE_CAUSECODE, AST_CAUSE_NO_USER_RESPONSE);
		send_command_final(iaxs[callno], AST_FRAME_IAX, IAX_COMMAND_HANGUP, 0, ied.buf, ied.pos, -1);
	}
	ast_mutex_unlock(&iaxsl[callno]);
	return 0;
}

static void iax2_dprequest(struct iax2_dpcache *dp, int callno)
{
	struct iax_ie_data ied;
	/* Auto-hangup with 30 seconds of inactivity */
	if (iaxs[callno]->autoid > -1)
		ast_sched_del(sched, iaxs[callno]->autoid);
	iaxs[callno]->autoid = ast_sched_add(sched, 30000, auto_hangup, (void *)(long)callno);
	memset(&ied, 0, sizeof(ied));
	iax_ie_append_str(&ied, IAX_IE_CALLED_NUMBER, dp->exten);
	send_command(iaxs[callno], AST_FRAME_IAX, IAX_COMMAND_DPREQ, 0, ied.buf, ied.pos, -1);
	dp->flags |= CACHE_FLAG_TRANSMITTED;
}

static int iax2_vnak(int callno)
{
	return send_command_immediate(iaxs[callno], AST_FRAME_IAX, IAX_COMMAND_VNAK, 0, NULL, 0, iaxs[callno]->iseqno);
}

static void vnak_retransmit(int callno, int last)
{
	struct iax_frame *f;
	ast_mutex_lock(&iaxq.lock);
	f = iaxq.head;
	while(f) {
		/* Send a copy immediately */
		if ((f->callno == callno) && iaxs[f->callno] &&
			(f->oseqno >= last)) {
			send_packet(f);
		}
		f = f->next;
	}
	ast_mutex_unlock(&iaxq.lock);
}

static int iax2_poke_peer_s(void *data)
{
	struct iax2_peer *peer = data;
	peer->pokeexpire = -1;
	iax2_poke_peer(peer, 0);
	return 0;
}

static int send_trunk(struct iax2_trunk_peer *tpeer, struct timeval *now)
{
	int res = 0;
	struct iax_frame *fr;
	struct ast_iax2_meta_hdr *meta;
	struct ast_iax2_meta_trunk_hdr *mth;
	int calls = 0;
	
	/* Point to frame */
	fr = (struct iax_frame *)tpeer->trunkdata;
	/* Point to meta data */
	meta = (struct ast_iax2_meta_hdr *)fr->afdata;
	mth = (struct ast_iax2_meta_trunk_hdr *)meta->data;
	if (tpeer->trunkdatalen) {
		/* We're actually sending a frame, so fill the meta trunk header and meta header */
		meta->zeros = 0;
		meta->metacmd = IAX_META_TRUNK;
		if (ast_test_flag(&globalflags, IAX_TRUNKTIMESTAMPS))
			meta->cmddata = IAX_META_TRUNK_MINI;
		else
			meta->cmddata = IAX_META_TRUNK_SUPERMINI;
		mth->ts = htonl(calc_txpeerstamp(tpeer, trunkfreq, now));
		/* And the rest of the ast_iax2 header */
		fr->direction = DIRECTION_OUTGRESS;
		fr->retrans = -1;
		fr->transfer = 0;
		/* Any appropriate call will do */
		fr->data = fr->afdata;
		fr->datalen = tpeer->trunkdatalen + sizeof(struct ast_iax2_meta_hdr) + sizeof(struct ast_iax2_meta_trunk_hdr);
		res = transmit_trunk(fr, &tpeer->addr, tpeer->sockfd);
		calls = tpeer->calls;
#if 0
		ast_log(LOG_DEBUG, "Trunking %d call chunks in %d bytes to %s:%d, ts=%d\n", calls, fr->datalen, ast_inet_ntoa(iabuf, sizeof(iabuf), tpeer->addr.sin_addr), ntohs(tpeer->addr.sin_port), ntohl(mth->ts));
#endif		
		/* Reset transmit trunk side data */
		tpeer->trunkdatalen = 0;
		tpeer->calls = 0;
	}
	if (res < 0)
		return res;
	return calls;
}

static inline int iax2_trunk_expired(struct iax2_trunk_peer *tpeer, struct timeval *now)
{
	/* Drop when trunk is about 5 seconds idle */
	if (now->tv_sec > tpeer->trunkact.tv_sec + 5) 
		return 1;
	return 0;
}

static int timing_read(int *id, int fd, short events, void *cbdata)
{
	char buf[1024];
	int res;
	char iabuf[INET_ADDRSTRLEN];
	struct iax2_trunk_peer *tpeer, *prev = NULL, *drop=NULL;
	int processed = 0;
	int totalcalls = 0;
#ifdef ZT_TIMERACK
	int x = 1;
#endif
	struct timeval now;
	if (iaxtrunkdebug)
		ast_verbose("Beginning trunk processing. Trunk queue ceiling is %d bytes per host\n", MAX_TRUNKDATA);
	gettimeofday(&now, NULL);
	if (events & AST_IO_PRI) {
#ifdef ZT_TIMERACK
		/* Great, this is a timing interface, just call the ioctl */
		if (ioctl(fd, ZT_TIMERACK, &x)) 
			ast_log(LOG_WARNING, "Unable to acknowledge zap timer\n");
		res = 0;
#endif		
	} else {
		/* Read and ignore from the pseudo channel for timing */
		res = read(fd, buf, sizeof(buf));
		if (res < 1) {
			ast_log(LOG_WARNING, "Unable to read from timing fd\n");
			ast_mutex_unlock(&peerl.lock);
			return 1;
		}
	}
	/* For each peer that supports trunking... */
	ast_mutex_lock(&tpeerlock);
	tpeer = tpeers;
	while(tpeer) {
		processed++;
		res = 0;
		ast_mutex_lock(&tpeer->lock);
		/* We can drop a single tpeer per pass.  That makes all this logic
		   substantially easier */
		if (!drop && iax2_trunk_expired(tpeer, &now)) {
			/* Take it out of the list, but don't free it yet, because it
			   could be in use */
			if (prev)
				prev->next = tpeer->next;
			else
				tpeers = tpeer->next;
			drop = tpeer;
		} else {
			res = send_trunk(tpeer, &now);
			if (iaxtrunkdebug)
				ast_verbose(" - Trunk peer (%s:%d) has %d call chunk%s in transit, %d bytes backloged and has hit a high water mark of %d bytes\n", ast_inet_ntoa(iabuf, sizeof(iabuf), tpeer->addr.sin_addr), ntohs(tpeer->addr.sin_port), res, (res != 1) ? "s" : "", tpeer->trunkdatalen, tpeer->trunkdataalloc);
		}		
		totalcalls += res;	
		res = 0;
		ast_mutex_unlock(&tpeer->lock);
		prev = tpeer;
		tpeer = tpeer->next;
	}
	ast_mutex_unlock(&tpeerlock);
	if (drop) {
		ast_mutex_lock(&drop->lock);
		/* Once we have this lock, we're sure nobody else is using it or could use it once we release it, 
		   because by the time they could get tpeerlock, we've already grabbed it */
		ast_log(LOG_DEBUG, "Dropping unused iax2 trunk peer '%s:%d'\n", ast_inet_ntoa(iabuf, sizeof(iabuf), drop->addr.sin_addr), ntohs(drop->addr.sin_port));
		free(drop->trunkdata);
		ast_mutex_unlock(&drop->lock);
		ast_mutex_destroy(&drop->lock);
		free(drop);
		
	}
	if (iaxtrunkdebug)
		ast_verbose("Ending trunk processing with %d peers and %d call chunks processed\n", processed, totalcalls);
	iaxtrunkdebug =0;
	return 1;
}

struct dpreq_data {
	int callno;
	char context[AST_MAX_EXTENSION];
	char callednum[AST_MAX_EXTENSION];
	char *callerid;
};

static void dp_lookup(int callno, char *context, char *callednum, char *callerid, int skiplock)
{
	unsigned short dpstatus = 0;
	struct iax_ie_data ied1;
	int mm;

	memset(&ied1, 0, sizeof(ied1));
	mm = ast_matchmore_extension(NULL, context, callednum, 1, callerid);
	/* Must be started */
	if (!strcmp(callednum, ast_parking_ext()) || ast_exists_extension(NULL, context, callednum, 1, callerid)) {
		dpstatus = IAX_DPSTATUS_EXISTS;
	} else if (ast_canmatch_extension(NULL, context, callednum, 1, callerid)) {
		dpstatus = IAX_DPSTATUS_CANEXIST;
	} else {
		dpstatus = IAX_DPSTATUS_NONEXISTENT;
	}
	if (ast_ignore_pattern(context, callednum))
		dpstatus |= IAX_DPSTATUS_IGNOREPAT;
	if (mm)
		dpstatus |= IAX_DPSTATUS_MATCHMORE;
	if (!skiplock)
		ast_mutex_lock(&iaxsl[callno]);
	if (iaxs[callno]) {
		iax_ie_append_str(&ied1, IAX_IE_CALLED_NUMBER, callednum);
		iax_ie_append_short(&ied1, IAX_IE_DPSTATUS, dpstatus);
		iax_ie_append_short(&ied1, IAX_IE_REFRESH, iaxdefaultdpcache);
		send_command(iaxs[callno], AST_FRAME_IAX, IAX_COMMAND_DPREP, 0, ied1.buf, ied1.pos, -1);
	}
	if (!skiplock)
		ast_mutex_unlock(&iaxsl[callno]);
}

static void *dp_lookup_thread(void *data)
{
	/* Look up for dpreq */
	struct dpreq_data *dpr = data;
	dp_lookup(dpr->callno, dpr->context, dpr->callednum, dpr->callerid, 0);
	if (dpr->callerid)
		free(dpr->callerid);
	free(dpr);
	return NULL;
}

static void spawn_dp_lookup(int callno, char *context, char *callednum, char *callerid)
{
	pthread_t newthread;
	struct dpreq_data *dpr;
	
	if (!(dpr = ast_calloc(1, sizeof(*dpr))))
		return;

	dpr->callno = callno;
	ast_copy_string(dpr->context, context, sizeof(dpr->context));
	ast_copy_string(dpr->callednum, callednum, sizeof(dpr->callednum));
	if (callerid)
		dpr->callerid = ast_strdup(callerid);
	if (ast_pthread_create(&newthread, NULL, dp_lookup_thread, dpr)) {
		ast_log(LOG_WARNING, "Unable to start lookup thread!\n");
	}
}

struct iax_dual {
	struct ast_channel *chan1;
	struct ast_channel *chan2;
};

static void *iax_park_thread(void *stuff)
{
	struct ast_channel *chan1, *chan2;
	struct iax_dual *d;
	struct ast_frame *f;
	int ext;
	int res;
	d = stuff;
	chan1 = d->chan1;
	chan2 = d->chan2;
	free(d);
	f = ast_read(chan1);
	if (f)
		ast_frfree(f);
	res = ast_park_call(chan1, chan2, 0, &ext);
	ast_hangup(chan2);
	ast_log(LOG_NOTICE, "Parked on extension '%d'\n", ext);
	return NULL;
}

static int iax_park(struct ast_channel *chan1, struct ast_channel *chan2)
{
	struct iax_dual *d;
	struct ast_channel *chan1m, *chan2m;
	pthread_t th;
	chan1m = ast_channel_alloc(0);
	chan2m = ast_channel_alloc(0);
	if (chan2m && chan1m) {
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
		snprintf(chan2m->name, sizeof (chan2m->name), "IAXPeer/%s",chan2->name);
		/* Make formats okay */
		chan2m->readformat = chan2->readformat;
		chan2m->writeformat = chan2->writeformat;
		ast_channel_masquerade(chan2m, chan2);
		/* Setup the extensions and such */
		ast_copy_string(chan2m->context, chan2->context, sizeof(chan2m->context));
		ast_copy_string(chan2m->exten, chan2->exten, sizeof(chan2m->exten));
		chan2m->priority = chan2->priority;
		if (ast_do_masquerade(chan2m)) {
			ast_log(LOG_WARNING, "Masquerade failed :(\n");
			ast_hangup(chan2m);
			return -1;
		}
	} else {
		if (chan1m)
			ast_hangup(chan1m);
		if (chan2m)
			ast_hangup(chan2m);
		return -1;
	}
	if ((d = ast_calloc(1, sizeof(*d)))) {
		d->chan1 = chan1m;
		d->chan2 = chan2m;
		if (!ast_pthread_create(&th, NULL, iax_park_thread, d))
			return 0;
		free(d);
	}
	return -1;
}


static int iax2_provision(struct sockaddr_in *end, int sockfd, char *dest, const char *template, int force);

static int check_provisioning(struct sockaddr_in *sin, int sockfd, char *si, unsigned int ver)
{
	unsigned int ourver;
	char rsi[80];
	snprintf(rsi, sizeof(rsi), "si-%s", si);
	if (iax_provision_version(&ourver, rsi, 1))
		return 0;
	if (option_debug)
		ast_log(LOG_DEBUG, "Service identifier '%s', we think '%08x', they think '%08x'\n", si, ourver, ver);
	if (ourver != ver) 
		iax2_provision(sin, sockfd, NULL, rsi, 1);
	return 0;
}

static void construct_rr(struct chan_iax2_pvt *pvt, struct iax_ie_data *iep) 
{
#ifdef NEWJB
	jb_info stats;
	jb_getinfo(pvt->jb, &stats);
	
	memset(iep, 0, sizeof(*iep));

	iax_ie_append_int(iep,IAX_IE_RR_JITTER, stats.jitter);
	if(stats.frames_in == 0) stats.frames_in = 1;
	iax_ie_append_int(iep,IAX_IE_RR_LOSS, ((0xff & (stats.losspct/1000)) << 24 | (stats.frames_lost & 0x00ffffff)));
	iax_ie_append_int(iep,IAX_IE_RR_PKTS, stats.frames_in);
	iax_ie_append_short(iep,IAX_IE_RR_DELAY, stats.current - stats.min);
	iax_ie_append_int(iep,IAX_IE_RR_DROPPED, stats.frames_dropped);
	iax_ie_append_int(iep,IAX_IE_RR_OOO, stats.frames_ooo);
#else
	memset(iep, 0, sizeof(*iep));
	iax_ie_append_int(iep,IAX_IE_RR_JITTER, pvt->jitter);
	iax_ie_append_int(iep,IAX_IE_RR_PKTS, pvt->frames_received);
	if(!ast_test_flag(pvt, IAX_USEJITTERBUF)) 
		iax_ie_append_short(iep,IAX_IE_RR_DELAY, 0);
	else
		iax_ie_append_short(iep,IAX_IE_RR_DELAY, pvt->jitterbuffer - pvt->min);
	iax_ie_append_int(iep,IAX_IE_RR_DROPPED, pvt->frames_dropped);
	/* don't know, don't send! iax_ie_append_int(&ied,IAX_IE_RR_OOO, 0); */
	/* don't know, don't send! iax_ie_append_int(&ied,IAX_IE_RR_LOSS, 0); */
#endif
}

static void save_rr(struct iax_frame *fr, struct iax_ies *ies) 
{
	iaxs[fr->callno]->remote_rr.jitter = ies->rr_jitter;
	iaxs[fr->callno]->remote_rr.losspct = ies->rr_loss >> 24;
	iaxs[fr->callno]->remote_rr.losscnt = ies->rr_loss & 0xffffff;
	iaxs[fr->callno]->remote_rr.packets = ies->rr_pkts;
	iaxs[fr->callno]->remote_rr.delay = ies->rr_delay;
	iaxs[fr->callno]->remote_rr.dropped = ies->rr_dropped;
	iaxs[fr->callno]->remote_rr.ooo = ies->rr_ooo;
}

static int socket_read(int *id, int fd, short events, void *cbdata)
{
	struct sockaddr_in sin;
	int res;
	int updatehistory=1;
	int new = NEW_PREVENT;
	unsigned char buf[4096]; 
	void *ptr;
	socklen_t len = sizeof(sin);
	int dcallno = 0;
	struct ast_iax2_full_hdr *fh = (struct ast_iax2_full_hdr *)buf;
	struct ast_iax2_mini_hdr *mh = (struct ast_iax2_mini_hdr *)buf;
	struct ast_iax2_meta_hdr *meta = (struct ast_iax2_meta_hdr *)buf;
	struct ast_iax2_video_hdr *vh = (struct ast_iax2_video_hdr *)buf;
	struct ast_iax2_meta_trunk_hdr *mth;
	struct ast_iax2_meta_trunk_entry *mte;
	struct ast_iax2_meta_trunk_mini *mtm;
	char dblbuf[4096];	/* Declaration of dblbuf must immediately *preceed* fr  on the stack */
	struct iax_frame fr;
	struct iax_frame *cur;
	char iabuf[INET_ADDRSTRLEN];
	struct ast_frame f;
	struct ast_channel *c;
	struct iax2_dpcache *dp;
	struct iax2_peer *peer;
	struct iax2_trunk_peer *tpeer;
	struct timeval rxtrunktime;
	struct iax_ies ies;
	struct iax_ie_data ied0, ied1;
	int format;
	int exists;
	int minivid = 0;
	unsigned int ts;
	char empty[32]="";		/* Safety measure */
	struct iax_frame *duped_fr;
	char host_pref_buf[128];
	char caller_pref_buf[128];
	struct ast_codec_pref pref,rpref;
	char *using_prefs = "mine";

	dblbuf[0] = 0;	/* Keep GCC from whining */
	fr.callno = 0;
	
	res = recvfrom(fd, buf, sizeof(buf), 0,(struct sockaddr *) &sin, &len);
	if (res < 0) {
		if (errno != ECONNREFUSED)
			ast_log(LOG_WARNING, "Error: %s\n", strerror(errno));
		handle_error();
		return 1;
	}
	if(test_losspct) { /* simulate random loss condition */
		if( (100.0*rand()/(RAND_MAX+1.0)) < test_losspct) 
			return 1;
 
	}
	if (res < sizeof(struct ast_iax2_mini_hdr)) {
		ast_log(LOG_WARNING, "midget packet received (%d of %d min)\n", res, (int)sizeof(struct ast_iax2_mini_hdr));
		return 1;
	}
	if ((vh->zeros == 0) && (ntohs(vh->callno) & 0x8000)) {
		/* This is a video frame, get call number */
		fr.callno = find_callno(ntohs(vh->callno) & ~0x8000, dcallno, &sin, new, 1, fd);
		minivid = 1;
	} else if (meta->zeros == 0) {
		unsigned char metatype;
		/* This is a meta header */
		switch(meta->metacmd) {
		case IAX_META_TRUNK:
			if (res < sizeof(struct ast_iax2_meta_hdr) + sizeof(struct ast_iax2_meta_trunk_hdr)) {
				ast_log(LOG_WARNING, "midget meta trunk packet received (%d of %d min)\n", res, (int)sizeof(struct ast_iax2_mini_hdr));
				return 1;
			}
			mth = (struct ast_iax2_meta_trunk_hdr *)(meta->data);
			ts = ntohl(mth->ts);
			metatype = meta->cmddata;
			res -= (sizeof(struct ast_iax2_meta_hdr) + sizeof(struct ast_iax2_meta_trunk_hdr));
			ptr = mth->data;
			tpeer = find_tpeer(&sin, fd);
			if (!tpeer) {
				ast_log(LOG_WARNING, "Unable to accept trunked packet from '%s:%d': No matching peer\n", ast_inet_ntoa(iabuf, sizeof(iabuf), sin.sin_addr), ntohs(sin.sin_port));
				return 1;
			}
			tpeer->trunkact = ast_tvnow();
			if (!ts || ast_tvzero(tpeer->rxtrunktime))
				tpeer->rxtrunktime = tpeer->trunkact;
			rxtrunktime = tpeer->rxtrunktime;
			ast_mutex_unlock(&tpeer->lock);
			while(res >= sizeof(struct ast_iax2_meta_trunk_entry)) {
				/* Process channels */
				unsigned short callno, trunked_ts, len;

				if(metatype == IAX_META_TRUNK_MINI) {
					mtm = (struct ast_iax2_meta_trunk_mini *)ptr;
					ptr += sizeof(struct ast_iax2_meta_trunk_mini);
					res -= sizeof(struct ast_iax2_meta_trunk_mini);
					len = ntohs(mtm->len);
					callno = ntohs(mtm->mini.callno);
					trunked_ts = ntohs(mtm->mini.ts);
				} else if ( metatype == IAX_META_TRUNK_SUPERMINI ) {
					mte = (struct ast_iax2_meta_trunk_entry *)ptr;
					ptr += sizeof(struct ast_iax2_meta_trunk_entry);
					res -= sizeof(struct ast_iax2_meta_trunk_entry);
					len = ntohs(mte->len);
					callno = ntohs(mte->callno);
					trunked_ts = 0;
				} else {
					ast_log(LOG_WARNING, "Unknown meta trunk cmd from '%s:%d': dropping\n", ast_inet_ntoa(iabuf, sizeof(iabuf), sin.sin_addr), ntohs(sin.sin_port));
					break;
				}
				/* Stop if we don't have enough data */
				if (len > res)
					break;
				fr.callno = find_callno(callno & ~IAX_FLAG_FULL, 0, &sin, NEW_PREVENT, 1, fd);
				if (fr.callno) {
					ast_mutex_lock(&iaxsl[fr.callno]);
					/* If it's a valid call, deliver the contents.  If not, we
					   drop it, since we don't have a scallno to use for an INVAL */
					/* Process as a mini frame */
					f.frametype = AST_FRAME_VOICE;
					if (iaxs[fr.callno]) {
						if (iaxs[fr.callno]->voiceformat > 0) {
							f.subclass = iaxs[fr.callno]->voiceformat;
							f.datalen = len;
							if (f.datalen >= 0) {
								if (f.datalen)
									f.data = ptr;
								else
									f.data = NULL;
								if(trunked_ts) {
									fr.ts = (iaxs[fr.callno]->last & 0xFFFF0000L) | (trunked_ts & 0xffff);
								} else
									fr.ts = fix_peerts(&rxtrunktime, fr.callno, ts);
								/* Don't pass any packets until we're started */
								if (ast_test_flag(&iaxs[fr.callno]->state, IAX_STATE_STARTED)) {
									/* Common things */
									f.src = "IAX2";
									f.mallocd = 0;
									f.offset = 0;
									if (f.datalen && (f.frametype == AST_FRAME_VOICE)) 
										f.samples = ast_codec_get_samples(&f);
									else
										f.samples = 0;
									fr.outoforder = 0;
									iax_frame_wrap(&fr, &f);
#ifdef BRIDGE_OPTIMIZATION
									if (iaxs[fr.callno]->bridgecallno) {
										forward_delivery(&fr);
									} else {
										duped_fr = iaxfrdup2(&fr);
										if (duped_fr) {
											schedule_delivery(duped_fr, updatehistory, 1);
											fr.ts = duped_fr->ts;
										}
									}
#else
									duped_fr = iaxfrdup2(&fr);
									if (duped_fr) {
										schedule_delivery(duped_fr, updatehistory, 1);
										fr.ts = duped_fr->ts;
									}
#endif
									if (iaxs[fr.callno]->last < fr.ts) {
										iaxs[fr.callno]->last = fr.ts;
#if 1
										if (option_debug)
											ast_log(LOG_DEBUG, "For call=%d, set last=%d\n", fr.callno, fr.ts);
#endif
									}
								}
							} else {
								ast_log(LOG_WARNING, "Datalen < 0?\n");
							}
						} else {
							ast_log(LOG_WARNING, "Received trunked frame before first full voice frame\n ");
							iax2_vnak(fr.callno);
						}
					}
					ast_mutex_unlock(&iaxsl[fr.callno]);
				}
				ptr += len;
				res -= len;
			}
			
		}
		return 1;
	}
#ifdef DEBUG_SUPPORT
	if (iaxdebug)
		iax_showframe(NULL, fh, 1, &sin, res - sizeof(struct ast_iax2_full_hdr));
#endif
	if (ntohs(mh->callno) & IAX_FLAG_FULL) {
		/* Get the destination call number */
		dcallno = ntohs(fh->dcallno) & ~IAX_FLAG_RETRANS;
		/* Retrieve the type and subclass */
		f.frametype = fh->type;
		if (f.frametype == AST_FRAME_VIDEO) {
			f.subclass = uncompress_subclass(fh->csub & ~0x40) | ((fh->csub >> 6) & 0x1);
		} else {
			f.subclass = uncompress_subclass(fh->csub);
		}
		if ((f.frametype == AST_FRAME_IAX) && ((f.subclass == IAX_COMMAND_NEW) || (f.subclass == IAX_COMMAND_REGREQ) ||
						       (f.subclass == IAX_COMMAND_POKE) || (f.subclass == IAX_COMMAND_FWDOWNL) ||
						       (f.subclass == IAX_COMMAND_REGREL)))
			new = NEW_ALLOW;
	} else {
		/* Don't know anything about it yet */
		f.frametype = AST_FRAME_NULL;
		f.subclass = 0;
	}

	if (!fr.callno)
		fr.callno = find_callno(ntohs(mh->callno) & ~IAX_FLAG_FULL, dcallno, &sin, new, 1, fd);

	if (fr.callno > 0) 
		ast_mutex_lock(&iaxsl[fr.callno]);

	if (!fr.callno || !iaxs[fr.callno]) {
		/* A call arrived for a nonexistent destination.  Unless it's an "inval"
		   frame, reply with an inval */
		if (ntohs(mh->callno) & IAX_FLAG_FULL) {
			/* We can only raw hangup control frames */
			if (((f.subclass != IAX_COMMAND_INVAL) &&
				 (f.subclass != IAX_COMMAND_TXCNT) &&
				 (f.subclass != IAX_COMMAND_TXACC) &&
				 (f.subclass != IAX_COMMAND_FWDOWNL))||
			    (f.frametype != AST_FRAME_IAX))
				raw_hangup(&sin, ntohs(fh->dcallno) & ~IAX_FLAG_RETRANS, ntohs(mh->callno) & ~IAX_FLAG_FULL,
				fd);
		}
		if (fr.callno > 0) 
			ast_mutex_unlock(&iaxsl[fr.callno]);
		return 1;
	}
	if (ast_test_flag(iaxs[fr.callno], IAX_ENCRYPTED)) {
		if (decrypt_frame(fr.callno, fh, &f, &res)) {
			ast_log(LOG_NOTICE, "Packet Decrypt Failed!\n");
			ast_mutex_unlock(&iaxsl[fr.callno]);
			return 1;
		}
#ifdef DEBUG_SUPPORT
		else if (iaxdebug)
			iax_showframe(NULL, fh, 3, &sin, res - sizeof(struct ast_iax2_full_hdr));
#endif
	}

	/* count this frame */
	iaxs[fr.callno]->frames_received++;

	if (!inaddrcmp(&sin, &iaxs[fr.callno]->addr) && !minivid &&
		f.subclass != IAX_COMMAND_TXCNT &&		/* for attended transfer */
		f.subclass != IAX_COMMAND_TXACC)		/* for attended transfer */
		iaxs[fr.callno]->peercallno = (unsigned short)(ntohs(mh->callno) & ~IAX_FLAG_FULL);
	if (ntohs(mh->callno) & IAX_FLAG_FULL) {
		if (option_debug  && iaxdebug)
			ast_log(LOG_DEBUG, "Received packet %d, (%d, %d)\n", fh->oseqno, f.frametype, f.subclass);
		/* Check if it's out of order (and not an ACK or INVAL) */
		fr.oseqno = fh->oseqno;
		fr.iseqno = fh->iseqno;
		fr.ts = ntohl(fh->ts);
#ifdef IAXTESTS
		if (test_resync) {
			if (option_debug)
				ast_log(LOG_DEBUG, "Simulating frame ts resync, was %u now %u\n", fr.ts, fr.ts + test_resync);
			fr.ts += test_resync;
		}
#endif /* IAXTESTS */
#if 0
		if ( (ntohs(fh->dcallno) & IAX_FLAG_RETRANS) ||
		     ( (f.frametype != AST_FRAME_VOICE) && ! (f.frametype == AST_FRAME_IAX &&
								(f.subclass == IAX_COMMAND_NEW ||
								 f.subclass == IAX_COMMAND_AUTHREQ ||
								 f.subclass == IAX_COMMAND_ACCEPT ||
								 f.subclass == IAX_COMMAND_REJECT))      ) )
#endif
		if ((ntohs(fh->dcallno) & IAX_FLAG_RETRANS) || (f.frametype != AST_FRAME_VOICE))
			updatehistory = 0;
		if ((iaxs[fr.callno]->iseqno != fr.oseqno) &&
			(iaxs[fr.callno]->iseqno ||
				((f.subclass != IAX_COMMAND_TXCNT) &&
				(f.subclass != IAX_COMMAND_TXREADY) &&		/* for attended transfer */
				(f.subclass != IAX_COMMAND_TXREL) &&		/* for attended transfer */
				(f.subclass != IAX_COMMAND_UNQUELCH ) &&	/* for attended transfer */
				(f.subclass != IAX_COMMAND_TXACC)) ||
				(f.frametype != AST_FRAME_IAX))) {
			if (
			 ((f.subclass != IAX_COMMAND_ACK) &&
			  (f.subclass != IAX_COMMAND_INVAL) &&
			  (f.subclass != IAX_COMMAND_TXCNT) &&
			  (f.subclass != IAX_COMMAND_TXREADY) &&		/* for attended transfer */
			  (f.subclass != IAX_COMMAND_TXREL) &&		/* for attended transfer */
			  (f.subclass != IAX_COMMAND_UNQUELCH ) &&	/* for attended transfer */
			  (f.subclass != IAX_COMMAND_TXACC) &&
			  (f.subclass != IAX_COMMAND_VNAK)) ||
			  (f.frametype != AST_FRAME_IAX)) {
			 	/* If it's not an ACK packet, it's out of order. */
				if (option_debug)
					ast_log(LOG_DEBUG, "Packet arrived out of order (expecting %d, got %d) (frametype = %d, subclass = %d)\n", 
					iaxs[fr.callno]->iseqno, fr.oseqno, f.frametype, f.subclass);
				if (iaxs[fr.callno]->iseqno > fr.oseqno) {
					/* If we've already seen it, ack it XXX There's a border condition here XXX */
					if ((f.frametype != AST_FRAME_IAX) || 
							((f.subclass != IAX_COMMAND_ACK) && (f.subclass != IAX_COMMAND_INVAL))) {
						if (option_debug)
							ast_log(LOG_DEBUG, "Acking anyway\n");
						/* XXX Maybe we should handle its ack to us, but then again, it's probably outdated anyway, and if
						   we have anything to send, we'll retransmit and get an ACK back anyway XXX */
						send_command_immediate(iaxs[fr.callno], AST_FRAME_IAX, IAX_COMMAND_ACK, fr.ts, NULL, 0,fr.iseqno);
					}
				} else {
					/* Send a VNAK requesting retransmission */
					iax2_vnak(fr.callno);
				}
				ast_mutex_unlock(&iaxsl[fr.callno]);
				return 1;
			}
		} else {
			/* Increment unless it's an ACK or VNAK */
			if (((f.subclass != IAX_COMMAND_ACK) &&
			    (f.subclass != IAX_COMMAND_INVAL) &&
			    (f.subclass != IAX_COMMAND_TXCNT) &&
			    (f.subclass != IAX_COMMAND_TXACC) &&
				(f.subclass != IAX_COMMAND_VNAK)) ||
			    (f.frametype != AST_FRAME_IAX))
				iaxs[fr.callno]->iseqno++;
		}
		/* A full frame */
		if (res < sizeof(struct ast_iax2_full_hdr)) {
			ast_log(LOG_WARNING, "midget packet received (%d of %d min)\n", res, (int)sizeof(struct ast_iax2_full_hdr));
			ast_mutex_unlock(&iaxsl[fr.callno]);
			return 1;
		}
		f.datalen = res - sizeof(struct ast_iax2_full_hdr);

		/* Handle implicit ACKing unless this is an INVAL, and only if this is 
		   from the real peer, not the transfer peer */
		if (!inaddrcmp(&sin, &iaxs[fr.callno]->addr) && 
		    ((f.subclass != IAX_COMMAND_INVAL) ||
		     (f.frametype != AST_FRAME_IAX))) {
			unsigned char x;
			/* XXX This code is not very efficient.  Surely there is a better way which still
			       properly handles boundary conditions? XXX */
			/* First we have to qualify that the ACKed value is within our window */
			for (x=iaxs[fr.callno]->rseqno; x != iaxs[fr.callno]->oseqno; x++)
				if (fr.iseqno == x)
					break;
			if ((x != iaxs[fr.callno]->oseqno) || (iaxs[fr.callno]->oseqno == fr.iseqno)) {
				/* The acknowledgement is within our window.  Time to acknowledge everything
				   that it says to */
				for (x=iaxs[fr.callno]->rseqno; x != fr.iseqno; x++) {
					/* Ack the packet with the given timestamp */
					if (option_debug && iaxdebug)
						ast_log(LOG_DEBUG, "Cancelling transmission of packet %d\n", x);
					ast_mutex_lock(&iaxq.lock);
					for (cur = iaxq.head; cur ; cur = cur->next) {
						/* If it's our call, and our timestamp, mark -1 retries */
						if ((fr.callno == cur->callno) && (x == cur->oseqno)) {
							cur->retries = -1;
							/* Destroy call if this is the end */
							if (cur->final) { 
								if (iaxdebug && option_debug)
									ast_log(LOG_DEBUG, "Really destroying %d, having been acked on final message\n", fr.callno);
								iax2_destroy_nolock(fr.callno);
							}
						}
					}
					ast_mutex_unlock(&iaxq.lock);
				}
				/* Note how much we've received acknowledgement for */
				if (iaxs[fr.callno])
					iaxs[fr.callno]->rseqno = fr.iseqno;
				else {
					/* Stop processing now */
					ast_mutex_unlock(&iaxsl[fr.callno]);
					return 1;
				}
			} else
				ast_log(LOG_DEBUG, "Received iseqno %d not within window %d->%d\n", fr.iseqno, iaxs[fr.callno]->rseqno, iaxs[fr.callno]->oseqno);
		}
		if (inaddrcmp(&sin, &iaxs[fr.callno]->addr) && 
			((f.frametype != AST_FRAME_IAX) || 
			 ((f.subclass != IAX_COMMAND_TXACC) &&
			  (f.subclass != IAX_COMMAND_TXCNT)))) {
			/* Only messages we accept from a transfer host are TXACC and TXCNT */
			ast_mutex_unlock(&iaxsl[fr.callno]);
			return 1;
		}

		if (f.datalen) {
			if (f.frametype == AST_FRAME_IAX) {
				if (iax_parse_ies(&ies, buf + sizeof(struct ast_iax2_full_hdr), f.datalen)) {
					ast_log(LOG_WARNING, "Undecodable frame received from '%s'\n", ast_inet_ntoa(iabuf, sizeof(iabuf), sin.sin_addr));
					ast_mutex_unlock(&iaxsl[fr.callno]);
					return 1;
				}
				f.data = NULL;
			} else
				f.data = buf + sizeof(struct ast_iax2_full_hdr);
		} else {
			if (f.frametype == AST_FRAME_IAX)
				f.data = NULL;
			else
				f.data = empty;
			memset(&ies, 0, sizeof(ies));
		}
		if (f.frametype == AST_FRAME_VOICE) {
			if (f.subclass != iaxs[fr.callno]->voiceformat) {
					iaxs[fr.callno]->voiceformat = f.subclass;
					ast_log(LOG_DEBUG, "Ooh, voice format changed to %d\n", f.subclass);
					if (iaxs[fr.callno]->owner) {
						int orignative;
retryowner:
						if (ast_mutex_trylock(&iaxs[fr.callno]->owner->lock)) {
							ast_mutex_unlock(&iaxsl[fr.callno]);
							usleep(1);
							ast_mutex_lock(&iaxsl[fr.callno]);
							if (iaxs[fr.callno] && iaxs[fr.callno]->owner) goto retryowner;
						}
						if (iaxs[fr.callno]) {
							if (iaxs[fr.callno]->owner) {
								orignative = iaxs[fr.callno]->owner->nativeformats;
								iaxs[fr.callno]->owner->nativeformats = f.subclass;
								if (iaxs[fr.callno]->owner->readformat)
									ast_set_read_format(iaxs[fr.callno]->owner, iaxs[fr.callno]->owner->readformat);
								iaxs[fr.callno]->owner->nativeformats = orignative;
								ast_mutex_unlock(&iaxs[fr.callno]->owner->lock);
							}
						} else {
							ast_log(LOG_DEBUG, "Neat, somebody took away the channel at a magical time but i found it!\n");
							ast_mutex_unlock(&iaxsl[fr.callno]);
							return 1;
						}
					}
			}
		}
		if (f.frametype == AST_FRAME_VIDEO) {
			if (f.subclass != iaxs[fr.callno]->videoformat) {
				ast_log(LOG_DEBUG, "Ooh, video format changed to %d\n", f.subclass & ~0x1);
				iaxs[fr.callno]->videoformat = f.subclass & ~0x1;
			}
		}
		if (f.frametype == AST_FRAME_IAX) {
			if (iaxs[fr.callno]->initid > -1) {
				/* Don't auto congest anymore since we've gotten something usefulb ack */
				ast_sched_del(sched, iaxs[fr.callno]->initid);
				iaxs[fr.callno]->initid = -1;
			}
			/* Handle the IAX pseudo frame itself */
			if (option_debug && iaxdebug)
				ast_log(LOG_DEBUG, "IAX subclass %d received\n", f.subclass);

                        /* Update last ts unless the frame's timestamp originated with us. */
			if (iaxs[fr.callno]->last < fr.ts &&
                            f.subclass != IAX_COMMAND_ACK &&
                            f.subclass != IAX_COMMAND_PONG &&
                            f.subclass != IAX_COMMAND_LAGRP) {
				iaxs[fr.callno]->last = fr.ts;
				if (option_debug && iaxdebug)
					ast_log(LOG_DEBUG, "For call=%d, set last=%d\n", fr.callno, fr.ts);
			}

			switch(f.subclass) {
			case IAX_COMMAND_ACK:
				/* Do nothing */
				break;
			case IAX_COMMAND_QUELCH:
				if (ast_test_flag(&iaxs[fr.callno]->state, IAX_STATE_STARTED)) {
				        /* Generate Manager Hold event, if necessary*/
					if (iaxs[fr.callno]->owner) {
						manager_event(EVENT_FLAG_CALL, "Hold",
							"Channel: %s\r\n"
							"Uniqueid: %s\r\n",
							iaxs[fr.callno]->owner->name, 
							iaxs[fr.callno]->owner->uniqueid);
					}

					ast_set_flag(iaxs[fr.callno], IAX_QUELCH);
					if (ies.musiconhold) {
						if (iaxs[fr.callno]->owner &&
							ast_bridged_channel(iaxs[fr.callno]->owner))
								ast_moh_start(ast_bridged_channel(iaxs[fr.callno]->owner), NULL);
					}
				}
				break;
			case IAX_COMMAND_UNQUELCH:
				if (ast_test_flag(&iaxs[fr.callno]->state, IAX_STATE_STARTED)) {
				        /* Generate Manager Unhold event, if necessary*/
					if (iaxs[fr.callno]->owner && ast_test_flag(iaxs[fr.callno], IAX_QUELCH)) {
						manager_event(EVENT_FLAG_CALL, "Unhold",
							"Channel: %s\r\n"
							"Uniqueid: %s\r\n",
							iaxs[fr.callno]->owner->name, 
							iaxs[fr.callno]->owner->uniqueid);
					}

					ast_clear_flag(iaxs[fr.callno], IAX_QUELCH);
					if (iaxs[fr.callno]->owner &&
						ast_bridged_channel(iaxs[fr.callno]->owner))
							ast_moh_stop(ast_bridged_channel(iaxs[fr.callno]->owner));
				}
				break;
			case IAX_COMMAND_TXACC:
				if (iaxs[fr.callno]->transferring == TRANSFER_BEGIN) {
					/* Ack the packet with the given timestamp */
					ast_mutex_lock(&iaxq.lock);
					for (cur = iaxq.head; cur ; cur = cur->next) {
						/* Cancel any outstanding txcnt's */
						if ((fr.callno == cur->callno) && (cur->transfer))
							cur->retries = -1;
					}
					ast_mutex_unlock(&iaxq.lock);
					memset(&ied1, 0, sizeof(ied1));
					iax_ie_append_short(&ied1, IAX_IE_CALLNO, iaxs[fr.callno]->callno);
					send_command(iaxs[fr.callno], AST_FRAME_IAX, IAX_COMMAND_TXREADY, 0, ied1.buf, ied1.pos, -1);
					iaxs[fr.callno]->transferring = TRANSFER_READY;
				}
				break;
			case IAX_COMMAND_NEW:
				/* Ignore if it's already up */
				if (ast_test_flag(&iaxs[fr.callno]->state, IAX_STATE_STARTED | IAX_STATE_TBD))
					break;
				if (ies.provverpres && ies.serviceident && sin.sin_addr.s_addr)
					check_provisioning(&sin, fd, ies.serviceident, ies.provver);
				/* If we're in trunk mode, do it now, and update the trunk number in our frame before continuing */
				if (ast_test_flag(iaxs[fr.callno], IAX_TRUNK)) {
					fr.callno = make_trunk(fr.callno, 1);
				}
				/* For security, always ack immediately */
				if (delayreject)
					send_command_immediate(iaxs[fr.callno], AST_FRAME_IAX, IAX_COMMAND_ACK, fr.ts, NULL, 0,fr.iseqno);
				if (check_access(fr.callno, &sin, &ies)) {
					/* They're not allowed on */
					auth_fail(fr.callno, IAX_COMMAND_REJECT);
					if (authdebug)
						ast_log(LOG_NOTICE, "Rejected connect attempt from %s, who was trying to reach '%s@%s'\n", ast_inet_ntoa(iabuf, sizeof(iabuf), sin.sin_addr), iaxs[fr.callno]->exten, iaxs[fr.callno]->context);
					break;
				}
				/* This might re-enter the IAX code and need the lock */
				if (strcasecmp(iaxs[fr.callno]->exten, "TBD")) {
					ast_mutex_unlock(&iaxsl[fr.callno]);
					exists = ast_exists_extension(NULL, iaxs[fr.callno]->context, iaxs[fr.callno]->exten, 1, iaxs[fr.callno]->cid_num);
					ast_mutex_lock(&iaxsl[fr.callno]);
				} else
					exists = 0;
				if (ast_strlen_zero(iaxs[fr.callno]->secret) && ast_strlen_zero(iaxs[fr.callno]->inkeys)) {
					if (strcmp(iaxs[fr.callno]->exten, "TBD") && !exists) {
						memset(&ied0, 0, sizeof(ied0));
						iax_ie_append_str(&ied0, IAX_IE_CAUSE, "No such context/extension");
						iax_ie_append_byte(&ied0, IAX_IE_CAUSECODE, AST_CAUSE_NO_ROUTE_DESTINATION);
						send_command_final(iaxs[fr.callno], AST_FRAME_IAX, IAX_COMMAND_REJECT, 0, ied0.buf, ied0.pos, -1);
						if (authdebug)
							ast_log(LOG_NOTICE, "Rejected connect attempt from %s, request '%s@%s' does not exist\n", ast_inet_ntoa(iabuf, sizeof(iabuf), sin.sin_addr), iaxs[fr.callno]->exten, iaxs[fr.callno]->context);
					} else {
						/* Select an appropriate format */

						if(ast_test_flag(iaxs[fr.callno], IAX_CODEC_NOPREFS)) {
							if(ast_test_flag(iaxs[fr.callno], IAX_CODEC_NOCAP)) {
								using_prefs = "reqonly";
							} else {
								using_prefs = "disabled";
							}
							format = iaxs[fr.callno]->peerformat & iaxs[fr.callno]->capability;
							memset(&pref, 0, sizeof(pref));
							strcpy(caller_pref_buf, "disabled");
							strcpy(host_pref_buf, "disabled");
						} else {
							using_prefs = "mine";
							if(ies.codec_prefs) {
								ast_codec_pref_convert(&rpref, ies.codec_prefs, 32, 0);
								/* If we are codec_first_choice we let the caller have the 1st shot at picking the codec.*/
								if (ast_test_flag(iaxs[fr.callno], IAX_CODEC_USER_FIRST)) {
									pref = rpref;
									using_prefs = "caller";
								} else {
									pref = iaxs[fr.callno]->prefs;
								}
							} else
								pref = iaxs[fr.callno]->prefs;
						
							format = ast_codec_choose(&pref, iaxs[fr.callno]->capability & iaxs[fr.callno]->peercapability, 0);
							ast_codec_pref_string(&rpref, caller_pref_buf, sizeof(caller_pref_buf) - 1);
							ast_codec_pref_string(&iaxs[fr.callno]->prefs, host_pref_buf, sizeof(host_pref_buf) - 1);
						}
						if (!format) {
							if(!ast_test_flag(iaxs[fr.callno], IAX_CODEC_NOCAP))
								format = iaxs[fr.callno]->peercapability & iaxs[fr.callno]->capability;
							if (!format) {
								memset(&ied0, 0, sizeof(ied0));
								iax_ie_append_str(&ied0, IAX_IE_CAUSE, "Unable to negotiate codec");
								iax_ie_append_byte(&ied0, IAX_IE_CAUSECODE, AST_CAUSE_BEARERCAPABILITY_NOTAVAIL);
								send_command_final(iaxs[fr.callno], AST_FRAME_IAX, IAX_COMMAND_REJECT, 0, ied0.buf, ied0.pos, -1);
								if (authdebug) {
									if(ast_test_flag(iaxs[fr.callno], IAX_CODEC_NOCAP))
										ast_log(LOG_NOTICE, "Rejected connect attempt from %s, requested 0x%x incompatible with our capability 0x%x.\n", ast_inet_ntoa(iabuf, sizeof(iabuf), sin.sin_addr), iaxs[fr.callno]->peerformat, iaxs[fr.callno]->capability);
									else 
										ast_log(LOG_NOTICE, "Rejected connect attempt from %s, requested/capability 0x%x/0x%x incompatible with our capability 0x%x.\n", ast_inet_ntoa(iabuf, sizeof(iabuf), sin.sin_addr), iaxs[fr.callno]->peerformat, iaxs[fr.callno]->peercapability, iaxs[fr.callno]->capability);
								}
							} else {
								/* Pick one... */
								if(ast_test_flag(iaxs[fr.callno], IAX_CODEC_NOCAP)) {
									if(!(iaxs[fr.callno]->peerformat & iaxs[fr.callno]->capability))
										format = 0;
								} else {
									if(ast_test_flag(iaxs[fr.callno], IAX_CODEC_NOPREFS)) {
										using_prefs = ast_test_flag(iaxs[fr.callno], IAX_CODEC_NOCAP) ? "reqonly" : "disabled";
										memset(&pref, 0, sizeof(pref));
										format = ast_best_codec(iaxs[fr.callno]->peercapability & iaxs[fr.callno]->capability);
										strcpy(caller_pref_buf,"disabled");
										strcpy(host_pref_buf,"disabled");
									} else {
										using_prefs = "mine";
										if(ies.codec_prefs) {
											/* Do the opposite of what we tried above. */
											if (ast_test_flag(iaxs[fr.callno], IAX_CODEC_USER_FIRST)) {
												pref = iaxs[fr.callno]->prefs;								
											} else {
												pref = rpref;
												using_prefs = "caller";
											}
											format = ast_codec_choose(&pref, iaxs[fr.callno]->peercapability & iaxs[fr.callno]->capability, 1);
									
										} else /* if no codec_prefs IE do it the old way */
											format = ast_best_codec(iaxs[fr.callno]->peercapability & iaxs[fr.callno]->capability);	
									}
								}

								if (!format) {
									memset(&ied0, 0, sizeof(ied0));
									iax_ie_append_str(&ied0, IAX_IE_CAUSE, "Unable to negotiate codec");
									iax_ie_append_byte(&ied0, IAX_IE_CAUSECODE, AST_CAUSE_BEARERCAPABILITY_NOTAVAIL);
									ast_log(LOG_ERROR, "No best format in 0x%x???\n", iaxs[fr.callno]->peercapability & iaxs[fr.callno]->capability);
									send_command_final(iaxs[fr.callno], AST_FRAME_IAX, IAX_COMMAND_REJECT, 0, ied0.buf, ied0.pos, -1);
									if (authdebug)
										ast_log(LOG_NOTICE, "Rejected connect attempt from %s, requested/capability 0x%x/0x%x incompatible with our capability 0x%x.\n", ast_inet_ntoa(iabuf, sizeof(iabuf), sin.sin_addr), iaxs[fr.callno]->peerformat, iaxs[fr.callno]->peercapability, iaxs[fr.callno]->capability);
									ast_set_flag(iaxs[fr.callno], IAX_ALREADYGONE);	
									break;
								}
							}
						}
						if (format) {
							/* No authentication required, let them in */
							memset(&ied1, 0, sizeof(ied1));
							iax_ie_append_int(&ied1, IAX_IE_FORMAT, format);
							send_command(iaxs[fr.callno], AST_FRAME_IAX, IAX_COMMAND_ACCEPT, 0, ied1.buf, ied1.pos, -1);
							if (strcmp(iaxs[fr.callno]->exten, "TBD")) {
								ast_set_flag(&iaxs[fr.callno]->state, IAX_STATE_STARTED);
								if (option_verbose > 2) 
									ast_verbose(VERBOSE_PREFIX_3 "Accepting UNAUTHENTICATED call from %s:\n"
												"%srequested format = %s,\n"
												"%srequested prefs = %s,\n"
												"%sactual format = %s,\n"
												"%shost prefs = %s,\n"
												"%spriority = %s\n",
												ast_inet_ntoa(iabuf, sizeof(iabuf), sin.sin_addr), 
												VERBOSE_PREFIX_4,
												ast_getformatname(iaxs[fr.callno]->peerformat), 
												VERBOSE_PREFIX_4,
												caller_pref_buf,
												VERBOSE_PREFIX_4,
												ast_getformatname(format), 
												VERBOSE_PREFIX_4,
												host_pref_buf, 
												VERBOSE_PREFIX_4,
												using_prefs);
								
								if(!(c = ast_iax2_new(fr.callno, AST_STATE_RING, format)))
									iax2_destroy_nolock(fr.callno);
							} else {
								ast_set_flag(&iaxs[fr.callno]->state, IAX_STATE_TBD);
								/* If this is a TBD call, we're ready but now what...  */
								if (option_verbose > 2)
									ast_verbose(VERBOSE_PREFIX_3 "Accepted unauthenticated TBD call from %s\n", ast_inet_ntoa(iabuf, sizeof(iabuf), sin.sin_addr));
							}
						}
					}
					break;
				}
				if (iaxs[fr.callno]->authmethods & IAX_AUTH_MD5)
					merge_encryption(iaxs[fr.callno],ies.encmethods);
				else
					iaxs[fr.callno]->encmethods = 0;
				authenticate_request(iaxs[fr.callno]);
				ast_set_flag(&iaxs[fr.callno]->state, IAX_STATE_AUTHENTICATED);
				break;
			case IAX_COMMAND_DPREQ:
				/* Request status in the dialplan */
				if (ast_test_flag(&iaxs[fr.callno]->state, IAX_STATE_TBD) &&
					!ast_test_flag(&iaxs[fr.callno]->state, IAX_STATE_STARTED) && ies.called_number) {
					if (iaxcompat) {
						/* Spawn a thread for the lookup */
						spawn_dp_lookup(fr.callno, iaxs[fr.callno]->context, ies.called_number, iaxs[fr.callno]->cid_num);
					} else {
						/* Just look it up */
						dp_lookup(fr.callno, iaxs[fr.callno]->context, ies.called_number, iaxs[fr.callno]->cid_num, 1);
					}
				}
				break;
			case IAX_COMMAND_HANGUP:
				ast_set_flag(iaxs[fr.callno], IAX_ALREADYGONE);
				ast_log(LOG_DEBUG, "Immediately destroying %d, having received hangup\n", fr.callno);
				/* Set hangup cause according to remote */
				if (ies.causecode && iaxs[fr.callno]->owner)
					iaxs[fr.callno]->owner->hangupcause = ies.causecode;
				/* Send ack immediately, before we destroy */
				send_command_immediate(iaxs[fr.callno], AST_FRAME_IAX, IAX_COMMAND_ACK, fr.ts, NULL, 0,fr.iseqno);
				iax2_destroy_nolock(fr.callno);
				break;
			case IAX_COMMAND_REJECT:
				memset(&f, 0, sizeof(f));
				f.frametype = AST_FRAME_CONTROL;
				f.subclass = AST_CONTROL_CONGESTION;

				/* Set hangup cause according to remote */
				if (ies.causecode && iaxs[fr.callno]->owner)
					iaxs[fr.callno]->owner->hangupcause = ies.causecode;

				iax2_queue_frame(fr.callno, &f);
				if (ast_test_flag(iaxs[fr.callno], IAX_PROVISION)) {
					/* Send ack immediately, before we destroy */
					send_command_immediate(iaxs[fr.callno], AST_FRAME_IAX, IAX_COMMAND_ACK, fr.ts, NULL, 0,fr.iseqno);
					iax2_destroy_nolock(fr.callno);
					break;
				}
				if (iaxs[fr.callno]->owner) {
					if (authdebug)
						ast_log(LOG_WARNING, "Call rejected by %s: %s\n", ast_inet_ntoa(iabuf, sizeof(iabuf), iaxs[fr.callno]->addr.sin_addr), ies.cause ? ies.cause : "<Unknown>");
				}
				ast_log(LOG_DEBUG, "Immediately destroying %d, having received reject\n", fr.callno);
				/* Send ack immediately, before we destroy */
				send_command_immediate(iaxs[fr.callno], AST_FRAME_IAX, IAX_COMMAND_ACK, fr.ts, NULL, 0,fr.iseqno);
				iaxs[fr.callno]->error = EPERM;
				iax2_destroy_nolock(fr.callno);
				break;
			case IAX_COMMAND_TRANSFER:
				if (iaxs[fr.callno]->owner && ast_bridged_channel(iaxs[fr.callno]->owner) && ies.called_number) {
					if (!strcmp(ies.called_number, ast_parking_ext())) {
						if (iax_park(ast_bridged_channel(iaxs[fr.callno]->owner), iaxs[fr.callno]->owner)) {
							ast_log(LOG_WARNING, "Failed to park call on '%s'\n", ast_bridged_channel(iaxs[fr.callno]->owner)->name);
						} else
							ast_log(LOG_DEBUG, "Parked call on '%s'\n", ast_bridged_channel(iaxs[fr.callno]->owner)->name);
					} else {
						if (ast_async_goto(ast_bridged_channel(iaxs[fr.callno]->owner), iaxs[fr.callno]->context, ies.called_number, 1))
							ast_log(LOG_WARNING, "Async goto of '%s' to '%s@%s' failed\n", ast_bridged_channel(iaxs[fr.callno]->owner)->name, 
								ies.called_number, iaxs[fr.callno]->context);
						else
							ast_log(LOG_DEBUG, "Async goto of '%s' to '%s@%s' started\n", ast_bridged_channel(iaxs[fr.callno]->owner)->name, 
								ies.called_number, iaxs[fr.callno]->context);
					}
				} else
						ast_log(LOG_DEBUG, "Async goto not applicable on call %d\n", fr.callno);
				break;
			case IAX_COMMAND_ACCEPT:
				/* Ignore if call is already up or needs authentication or is a TBD */
				if (ast_test_flag(&iaxs[fr.callno]->state, IAX_STATE_STARTED | IAX_STATE_TBD | IAX_STATE_AUTHENTICATED))
					break;
				if (ast_test_flag(iaxs[fr.callno], IAX_PROVISION)) {
					/* Send ack immediately, before we destroy */
					send_command_immediate(iaxs[fr.callno], AST_FRAME_IAX, IAX_COMMAND_ACK, fr.ts, NULL, 0,fr.iseqno);
					iax2_destroy_nolock(fr.callno);
					break;
				}
				if (ies.format) {
					iaxs[fr.callno]->peerformat = ies.format;
				} else {
					if (iaxs[fr.callno]->owner)
						iaxs[fr.callno]->peerformat = iaxs[fr.callno]->owner->nativeformats;
					else
						iaxs[fr.callno]->peerformat = iaxs[fr.callno]->capability;
				}
				if (option_verbose > 2)
					ast_verbose(VERBOSE_PREFIX_3 "Call accepted by %s (format %s)\n", ast_inet_ntoa(iabuf, sizeof(iabuf), iaxs[fr.callno]->addr.sin_addr), ast_getformatname(iaxs[fr.callno]->peerformat));
				if (!(iaxs[fr.callno]->peerformat & iaxs[fr.callno]->capability)) {
					memset(&ied0, 0, sizeof(ied0));
					iax_ie_append_str(&ied0, IAX_IE_CAUSE, "Unable to negotiate codec");
					iax_ie_append_byte(&ied0, IAX_IE_CAUSECODE, AST_CAUSE_BEARERCAPABILITY_NOTAVAIL);
					send_command_final(iaxs[fr.callno], AST_FRAME_IAX, IAX_COMMAND_REJECT, 0, ied0.buf, ied0.pos, -1);
					if (authdebug)
						ast_log(LOG_NOTICE, "Rejected call to %s, format 0x%x incompatible with our capability 0x%x.\n", ast_inet_ntoa(iabuf, sizeof(iabuf), sin.sin_addr), iaxs[fr.callno]->peerformat, iaxs[fr.callno]->capability);
				} else {
					ast_set_flag(&iaxs[fr.callno]->state, IAX_STATE_STARTED);
					if (iaxs[fr.callno]->owner) {
						/* Switch us to use a compatible format */
						iaxs[fr.callno]->owner->nativeformats = iaxs[fr.callno]->peerformat;
						if (option_verbose > 2)
							ast_verbose(VERBOSE_PREFIX_3 "Format for call is %s\n", ast_getformatname(iaxs[fr.callno]->owner->nativeformats));
retryowner2:
						if (ast_mutex_trylock(&iaxs[fr.callno]->owner->lock)) {
							ast_mutex_unlock(&iaxsl[fr.callno]);
							usleep(1);
							ast_mutex_lock(&iaxsl[fr.callno]);
							if (iaxs[fr.callno] && iaxs[fr.callno]->owner) goto retryowner2;
						}
						
						if (iaxs[fr.callno] && iaxs[fr.callno]->owner) {
							/* Setup read/write formats properly. */
							if (iaxs[fr.callno]->owner->writeformat)
								ast_set_write_format(iaxs[fr.callno]->owner, iaxs[fr.callno]->owner->writeformat);	
							if (iaxs[fr.callno]->owner->readformat)
								ast_set_read_format(iaxs[fr.callno]->owner, iaxs[fr.callno]->owner->readformat);	
							ast_mutex_unlock(&iaxs[fr.callno]->owner->lock);
						}
					}
				}
				ast_mutex_lock(&dpcache_lock);
				dp = iaxs[fr.callno]->dpentries;
				while(dp) {
					if (!(dp->flags & CACHE_FLAG_TRANSMITTED)) {
						iax2_dprequest(dp, fr.callno);
					}
					dp = dp->peer;
				}
				ast_mutex_unlock(&dpcache_lock);
				break;
			case IAX_COMMAND_POKE:
				/* Send back a pong packet with the original timestamp */
				send_command_final(iaxs[fr.callno], AST_FRAME_IAX, IAX_COMMAND_PONG, fr.ts, NULL, 0, -1);
				break;
			case IAX_COMMAND_PING:
#ifdef BRIDGE_OPTIMIZATION
				if (iaxs[fr.callno]->bridgecallno) {
					/* If we're in a bridged call, just forward this */
					forward_command(iaxs[fr.callno], AST_FRAME_IAX, IAX_COMMAND_PING, fr.ts, NULL, 0, -1);
				} else {
					struct iax_ie_data pingied;
					construct_rr(iaxs[fr.callno], &pingied);
					/* Send back a pong packet with the original timestamp */
					send_command(iaxs[fr.callno], AST_FRAME_IAX, IAX_COMMAND_PONG, fr.ts, pingied.buf, pingied.pos, -1);
				}
#else				
				{
					struct iax_ie_data pingied;
					construct_rr(iaxs[fr.callno], &pingied);
				/* Send back a pong packet with the original timestamp */
					send_command(iaxs[fr.callno], AST_FRAME_IAX, IAX_COMMAND_PONG, fr.ts, pingied.buf, pingied.pos, -1);
				}
#endif			
				break;
			case IAX_COMMAND_PONG:
#ifdef BRIDGE_OPTIMIZATION
				if (iaxs[fr.callno]->bridgecallno) {
					/* Forward to the other side of the bridge */
					forward_command(iaxs[fr.callno], AST_FRAME_IAX, IAX_COMMAND_PONG, fr.ts, NULL, 0, -1);
				} else {
					/* Calculate ping time */
					iaxs[fr.callno]->pingtime =  calc_timestamp(iaxs[fr.callno], 0, &f) - fr.ts;
				}
#else
				/* Calculate ping time */
				iaxs[fr.callno]->pingtime =  calc_timestamp(iaxs[fr.callno], 0, &f) - fr.ts;
#endif
				/* save RR info */
				save_rr(&fr, &ies);

				if (iaxs[fr.callno]->peerpoke) {
					peer = iaxs[fr.callno]->peerpoke;
					if ((peer->lastms < 0)  || (peer->historicms > peer->maxms)) {
						if (iaxs[fr.callno]->pingtime <= peer->maxms) {
							ast_log(LOG_NOTICE, "Peer '%s' is now REACHABLE! Time: %d\n", peer->name, iaxs[fr.callno]->pingtime);
							manager_event(EVENT_FLAG_SYSTEM, "PeerStatus", "Peer: IAX2/%s\r\nPeerStatus: Reachable\r\nTime: %d\r\n", peer->name, iaxs[fr.callno]->pingtime); 
							ast_device_state_changed("IAX2/%s", peer->name); /* Activate notification */
						}
					} else if ((peer->historicms > 0) && (peer->historicms <= peer->maxms)) {
						if (iaxs[fr.callno]->pingtime > peer->maxms) {
							ast_log(LOG_NOTICE, "Peer '%s' is now TOO LAGGED (%d ms)!\n", peer->name, iaxs[fr.callno]->pingtime);
							manager_event(EVENT_FLAG_SYSTEM, "PeerStatus", "Peer: IAX2/%s\r\nPeerStatus: Lagged\r\nTime: %d\r\n", peer->name, iaxs[fr.callno]->pingtime); 
							ast_device_state_changed("IAX2/%s", peer->name); /* Activate notification */
						}
					}
					peer->lastms = iaxs[fr.callno]->pingtime;
					if (peer->smoothing && (peer->lastms > -1))
						peer->historicms = (iaxs[fr.callno]->pingtime + peer->historicms) / 2;
					else if (peer->smoothing && peer->lastms < 0)
						peer->historicms = (0 + peer->historicms) / 2;
					else					
						peer->historicms = iaxs[fr.callno]->pingtime;

					if (peer->pokeexpire > -1)
						ast_sched_del(sched, peer->pokeexpire);
					send_command_immediate(iaxs[fr.callno], AST_FRAME_IAX, IAX_COMMAND_ACK, fr.ts, NULL, 0,fr.iseqno);
					iax2_destroy_nolock(fr.callno);
					peer->callno = 0;
					/* Try again eventually */
						ast_log(LOG_DEBUG, "Peer lastms %d, historicms %d, maxms %d\n", peer->lastms, peer->historicms, peer->maxms);
					if ((peer->lastms < 0)  || (peer->historicms > peer->maxms)) 
						peer->pokeexpire = ast_sched_add(sched, peer->pokefreqnotok, iax2_poke_peer_s, peer);
					else
						peer->pokeexpire = ast_sched_add(sched, peer->pokefreqok, iax2_poke_peer_s, peer);
				}
				break;
			case IAX_COMMAND_LAGRQ:
			case IAX_COMMAND_LAGRP:
#ifdef BRIDGE_OPTIMIZATION
				if (iaxs[fr.callno]->bridgecallno) {
					forward_command(iaxs[fr.callno], AST_FRAME_IAX, f.subclass, fr.ts, NULL, 0, -1);
				} else {
#endif				
					f.src = "LAGRQ";
					f.mallocd = 0;
					f.offset = 0;
					f.samples = 0;
					iax_frame_wrap(&fr, &f);
					if(f.subclass == IAX_COMMAND_LAGRQ) {
					    /* Received a LAGRQ - echo back a LAGRP */
					    fr.af.subclass = IAX_COMMAND_LAGRP;
					    iax2_send(iaxs[fr.callno], &fr.af, fr.ts, -1, 0, 0, 0);
					} else {
					    /* Received LAGRP in response to our LAGRQ */
					    unsigned int ts;
					    /* This is a reply we've been given, actually measure the difference */
					    ts = calc_timestamp(iaxs[fr.callno], 0, &fr.af);
					    iaxs[fr.callno]->lag = ts - fr.ts;
					    if (option_debug && iaxdebug)
						ast_log(LOG_DEBUG, "Peer %s lag measured as %dms\n",
								ast_inet_ntoa(iabuf, sizeof(iabuf), iaxs[fr.callno]->addr.sin_addr), iaxs[fr.callno]->lag);
					}
#ifdef BRIDGE_OPTIMIZATION
				}
#endif				
				break;
			case IAX_COMMAND_AUTHREQ:
				if (ast_test_flag(&iaxs[fr.callno]->state, IAX_STATE_STARTED | IAX_STATE_TBD)) {
					ast_log(LOG_WARNING, "Call on %s is already up, can't start on it\n", iaxs[fr.callno]->owner ? iaxs[fr.callno]->owner->name : "<Unknown>");
					break;
				}
				if (authenticate_reply(iaxs[fr.callno], &iaxs[fr.callno]->addr, &ies, iaxs[fr.callno]->secret, iaxs[fr.callno]->outkey)) {
					ast_log(LOG_WARNING, 
						"I don't know how to authenticate %s to %s\n", 
						ies.username ? ies.username : "<unknown>", ast_inet_ntoa(iabuf, sizeof(iabuf), iaxs[fr.callno]->addr.sin_addr));
				}
				break;
			case IAX_COMMAND_AUTHREP:
				/* For security, always ack immediately */
				if (delayreject)
					send_command_immediate(iaxs[fr.callno], AST_FRAME_IAX, IAX_COMMAND_ACK, fr.ts, NULL, 0,fr.iseqno);
				/* Ignore once we've started */
				if (ast_test_flag(&iaxs[fr.callno]->state, IAX_STATE_STARTED | IAX_STATE_TBD)) {
					ast_log(LOG_WARNING, "Call on %s is already up, can't start on it\n", iaxs[fr.callno]->owner ? iaxs[fr.callno]->owner->name : "<Unknown>");
					break;
				}
				if (authenticate_verify(iaxs[fr.callno], &ies)) {
					if (authdebug)
						ast_log(LOG_NOTICE, "Host %s failed to authenticate as %s\n", ast_inet_ntoa(iabuf, sizeof(iabuf), iaxs[fr.callno]->addr.sin_addr), iaxs[fr.callno]->username);
					memset(&ied0, 0, sizeof(ied0));
					auth_fail(fr.callno, IAX_COMMAND_REJECT);
					break;
				}
				if (strcasecmp(iaxs[fr.callno]->exten, "TBD")) {
					/* This might re-enter the IAX code and need the lock */
					exists = ast_exists_extension(NULL, iaxs[fr.callno]->context, iaxs[fr.callno]->exten, 1, iaxs[fr.callno]->cid_num);
				} else
					exists = 0;
				if (strcmp(iaxs[fr.callno]->exten, "TBD") && !exists) {
					if (authdebug)
						ast_log(LOG_NOTICE, "Rejected connect attempt from %s, request '%s@%s' does not exist\n", ast_inet_ntoa(iabuf, sizeof(iabuf), sin.sin_addr), iaxs[fr.callno]->exten, iaxs[fr.callno]->context);
					memset(&ied0, 0, sizeof(ied0));
					iax_ie_append_str(&ied0, IAX_IE_CAUSE, "No such context/extension");
					iax_ie_append_byte(&ied0, IAX_IE_CAUSECODE, AST_CAUSE_NO_ROUTE_DESTINATION);
					send_command_final(iaxs[fr.callno], AST_FRAME_IAX, IAX_COMMAND_REJECT, 0, ied0.buf, ied0.pos, -1);
				} else {
					/* Select an appropriate format */
					if(ast_test_flag(iaxs[fr.callno], IAX_CODEC_NOPREFS)) {
						if(ast_test_flag(iaxs[fr.callno], IAX_CODEC_NOCAP)) {
							using_prefs = "reqonly";
						} else {
							using_prefs = "disabled";
						}
						format = iaxs[fr.callno]->peerformat & iaxs[fr.callno]->capability;
						memset(&pref, 0, sizeof(pref));
						strcpy(caller_pref_buf, "disabled");
						strcpy(host_pref_buf, "disabled");
					} else {
						using_prefs = "mine";
						if(ies.codec_prefs) {
							/* If we are codec_first_choice we let the caller have the 1st shot at picking the codec.*/
							ast_codec_pref_convert(&rpref, ies.codec_prefs, 32, 0);
							if (ast_test_flag(iaxs[fr.callno], IAX_CODEC_USER_FIRST)) {
								ast_codec_pref_convert(&pref, ies.codec_prefs, 32, 0);
								using_prefs = "caller";
							} else {
								pref = iaxs[fr.callno]->prefs;
							}
						} else /* if no codec_prefs IE do it the old way */
							pref = iaxs[fr.callno]->prefs;
					
						format = ast_codec_choose(&pref, iaxs[fr.callno]->capability & iaxs[fr.callno]->peercapability, 0);
						ast_codec_pref_string(&rpref, caller_pref_buf, sizeof(caller_pref_buf) - 1);
						ast_codec_pref_string(&iaxs[fr.callno]->prefs, host_pref_buf, sizeof(host_pref_buf) - 1);
					}
					if (!format) {
						if(!ast_test_flag(iaxs[fr.callno], IAX_CODEC_NOCAP)) {
							ast_log(LOG_DEBUG, "We don't do requested format %s, falling back to peer capability %d\n", ast_getformatname(iaxs[fr.callno]->peerformat), iaxs[fr.callno]->peercapability);
							format = iaxs[fr.callno]->peercapability & iaxs[fr.callno]->capability;
						}
						if (!format) {
							if (authdebug) {
								if(ast_test_flag(iaxs[fr.callno], IAX_CODEC_NOCAP)) 
									ast_log(LOG_NOTICE, "Rejected connect attempt from %s, requested 0x%x incompatible with our capability 0x%x.\n", ast_inet_ntoa(iabuf, sizeof(iabuf), sin.sin_addr), iaxs[fr.callno]->peerformat, iaxs[fr.callno]->capability);
								else
									ast_log(LOG_NOTICE, "Rejected connect attempt from %s, requested/capability 0x%x/0x%x incompatible with our capability 0x%x.\n", ast_inet_ntoa(iabuf, sizeof(iabuf), sin.sin_addr), iaxs[fr.callno]->peerformat, iaxs[fr.callno]->peercapability, iaxs[fr.callno]->capability);
							}
							memset(&ied0, 0, sizeof(ied0));
							iax_ie_append_str(&ied0, IAX_IE_CAUSE, "Unable to negotiate codec");
							iax_ie_append_byte(&ied0, IAX_IE_CAUSECODE, AST_CAUSE_BEARERCAPABILITY_NOTAVAIL);
							send_command_final(iaxs[fr.callno], AST_FRAME_IAX, IAX_COMMAND_REJECT, 0, ied0.buf, ied0.pos, -1);
						} else {
							/* Pick one... */
							if(ast_test_flag(iaxs[fr.callno], IAX_CODEC_NOCAP)) {
								if(!(iaxs[fr.callno]->peerformat & iaxs[fr.callno]->capability))
									format = 0;
							} else {
								if(ast_test_flag(iaxs[fr.callno], IAX_CODEC_NOPREFS)) {
									using_prefs = ast_test_flag(iaxs[fr.callno], IAX_CODEC_NOCAP) ? "reqonly" : "disabled";
									memset(&pref, 0, sizeof(pref));
									format = ast_test_flag(iaxs[fr.callno], IAX_CODEC_NOCAP) ?
										iaxs[fr.callno]->peerformat : ast_best_codec(iaxs[fr.callno]->peercapability & iaxs[fr.callno]->capability);
									strcpy(caller_pref_buf,"disabled");
									strcpy(host_pref_buf,"disabled");
								} else {
									using_prefs = "mine";
									if(ies.codec_prefs) {
										/* Do the opposite of what we tried above. */
										if (ast_test_flag(iaxs[fr.callno], IAX_CODEC_USER_FIRST)) {
											pref = iaxs[fr.callno]->prefs;						
										} else {
											pref = rpref;
											using_prefs = "caller";
										}
										format = ast_codec_choose(&pref, iaxs[fr.callno]->peercapability & iaxs[fr.callno]->capability, 1);
									} else /* if no codec_prefs IE do it the old way */
										format = ast_best_codec(iaxs[fr.callno]->peercapability & iaxs[fr.callno]->capability);	
								}
							}
							if (!format) {
								ast_log(LOG_ERROR, "No best format in 0x%x???\n", iaxs[fr.callno]->peercapability & iaxs[fr.callno]->capability);
								if (authdebug) {
									if(ast_test_flag(iaxs[fr.callno], IAX_CODEC_NOCAP))
										ast_log(LOG_NOTICE, "Rejected connect attempt from %s, requested 0x%x incompatible with our capability 0x%x.\n", ast_inet_ntoa(iabuf, sizeof(iabuf), sin.sin_addr), iaxs[fr.callno]->peerformat, iaxs[fr.callno]->capability);
									else
										ast_log(LOG_NOTICE, "Rejected connect attempt from %s, requested/capability 0x%x/0x%x incompatible with our capability 0x%x.\n", ast_inet_ntoa(iabuf, sizeof(iabuf), sin.sin_addr), iaxs[fr.callno]->peerformat, iaxs[fr.callno]->peercapability, iaxs[fr.callno]->capability);
								}
								memset(&ied0, 0, sizeof(ied0));
								iax_ie_append_str(&ied0, IAX_IE_CAUSE, "Unable to negotiate codec");
								iax_ie_append_byte(&ied0, IAX_IE_CAUSECODE, AST_CAUSE_BEARERCAPABILITY_NOTAVAIL);
								send_command_final(iaxs[fr.callno], AST_FRAME_IAX, IAX_COMMAND_REJECT, 0, ied0.buf, ied0.pos, -1);
							}
						}
					}
					if (format) {
						/* Authentication received */
						memset(&ied1, 0, sizeof(ied1));
						iax_ie_append_int(&ied1, IAX_IE_FORMAT, format);
						send_command(iaxs[fr.callno], AST_FRAME_IAX, IAX_COMMAND_ACCEPT, 0, ied1.buf, ied1.pos, -1);
						if (strcmp(iaxs[fr.callno]->exten, "TBD")) {
							ast_set_flag(&iaxs[fr.callno]->state, IAX_STATE_STARTED);
							if (option_verbose > 2) 
								ast_verbose(VERBOSE_PREFIX_3 "Accepting AUTHENTICATED call from %s:\n"
											"%srequested format = %s,\n"
											"%srequested prefs = %s,\n"
											"%sactual format = %s,\n"
											"%shost prefs = %s,\n"
											"%spriority = %s\n", 
											ast_inet_ntoa(iabuf, sizeof(iabuf), sin.sin_addr), 
											VERBOSE_PREFIX_4,
											ast_getformatname(iaxs[fr.callno]->peerformat),
											VERBOSE_PREFIX_4,
											caller_pref_buf,
											VERBOSE_PREFIX_4,
											ast_getformatname(format),
											VERBOSE_PREFIX_4,
											host_pref_buf,
											VERBOSE_PREFIX_4,
											using_prefs);

							ast_set_flag(&iaxs[fr.callno]->state, IAX_STATE_STARTED);
							if(!(c = ast_iax2_new(fr.callno, AST_STATE_RING, format)))
								iax2_destroy_nolock(fr.callno);
						} else {
							ast_set_flag(&iaxs[fr.callno]->state, IAX_STATE_TBD);
							/* If this is a TBD call, we're ready but now what...  */
							if (option_verbose > 2)
								ast_verbose(VERBOSE_PREFIX_3 "Accepted AUTHENTICATED TBD call from %s\n", ast_inet_ntoa(iabuf, sizeof(iabuf), sin.sin_addr));
						}
					}
				}
				break;
			case IAX_COMMAND_DIAL:
				if (ast_test_flag(&iaxs[fr.callno]->state, IAX_STATE_TBD)) {
					ast_clear_flag(&iaxs[fr.callno]->state, IAX_STATE_TBD);
					ast_copy_string(iaxs[fr.callno]->exten, ies.called_number ? ies.called_number : "s", sizeof(iaxs[fr.callno]->exten));	
					if (!ast_exists_extension(NULL, iaxs[fr.callno]->context, iaxs[fr.callno]->exten, 1, iaxs[fr.callno]->cid_num)) {
						if (authdebug)
							ast_log(LOG_NOTICE, "Rejected dial attempt from %s, request '%s@%s' does not exist\n", ast_inet_ntoa(iabuf, sizeof(iabuf), sin.sin_addr), iaxs[fr.callno]->exten, iaxs[fr.callno]->context);
						memset(&ied0, 0, sizeof(ied0));
						iax_ie_append_str(&ied0, IAX_IE_CAUSE, "No such context/extension");
						iax_ie_append_byte(&ied0, IAX_IE_CAUSECODE, AST_CAUSE_NO_ROUTE_DESTINATION);
						send_command_final(iaxs[fr.callno], AST_FRAME_IAX, IAX_COMMAND_REJECT, 0, ied0.buf, ied0.pos, -1);
					} else {
						ast_set_flag(&iaxs[fr.callno]->state, IAX_STATE_STARTED);
						if (option_verbose > 2) 
							ast_verbose(VERBOSE_PREFIX_3 "Accepting DIAL from %s, formats = 0x%x\n", ast_inet_ntoa(iabuf, sizeof(iabuf), sin.sin_addr), iaxs[fr.callno]->peerformat);
						ast_set_flag(&iaxs[fr.callno]->state, IAX_STATE_STARTED);
						send_command(iaxs[fr.callno], AST_FRAME_CONTROL, AST_CONTROL_PROGRESS, 0, NULL, 0, -1);
						if(!(c = ast_iax2_new(fr.callno, AST_STATE_RING, iaxs[fr.callno]->peerformat)))
							iax2_destroy_nolock(fr.callno);
					}
				}
				break;
			case IAX_COMMAND_INVAL:
				iaxs[fr.callno]->error = ENOTCONN;
				ast_log(LOG_DEBUG, "Immediately destroying %d, having received INVAL\n", fr.callno);
				iax2_destroy_nolock(fr.callno);
				if (option_debug)
					ast_log(LOG_DEBUG, "Destroying call %d\n", fr.callno);
				break;
			case IAX_COMMAND_VNAK:
				ast_log(LOG_DEBUG, "Received VNAK: resending outstanding frames\n");
				/* Force retransmission */
				vnak_retransmit(fr.callno, fr.iseqno);
				break;
			case IAX_COMMAND_REGREQ:
			case IAX_COMMAND_REGREL:
				/* For security, always ack immediately */
				if (delayreject)
					send_command_immediate(iaxs[fr.callno], AST_FRAME_IAX, IAX_COMMAND_ACK, fr.ts, NULL, 0,fr.iseqno);
				if (register_verify(fr.callno, &sin, &ies)) {
					/* Send delayed failure */
					auth_fail(fr.callno, IAX_COMMAND_REGREJ);
					break;
				}
				if ((ast_strlen_zero(iaxs[fr.callno]->secret) && ast_strlen_zero(iaxs[fr.callno]->inkeys)) || ast_test_flag(&iaxs[fr.callno]->state, IAX_STATE_AUTHENTICATED)) {
					if (f.subclass == IAX_COMMAND_REGREL)
						memset(&sin, 0, sizeof(sin));
					if (update_registry(iaxs[fr.callno]->peer, &sin, fr.callno, ies.devicetype, fd, ies.refresh))
						ast_log(LOG_WARNING, "Registry error\n");
					if (ies.provverpres && ies.serviceident && sin.sin_addr.s_addr)
						check_provisioning(&sin, fd, ies.serviceident, ies.provver);
					break;
				}
				registry_authrequest(iaxs[fr.callno]->peer, fr.callno);
				break;
			case IAX_COMMAND_REGACK:
				if (iax2_ack_registry(&ies, &sin, fr.callno)) 
					ast_log(LOG_WARNING, "Registration failure\n");
				/* Send ack immediately, before we destroy */
				send_command_immediate(iaxs[fr.callno], AST_FRAME_IAX, IAX_COMMAND_ACK, fr.ts, NULL, 0,fr.iseqno);
				iax2_destroy_nolock(fr.callno);
				break;
			case IAX_COMMAND_REGREJ:
				if (iaxs[fr.callno]->reg) {
					if (authdebug) {
						ast_log(LOG_NOTICE, "Registration of '%s' rejected: '%s' from: '%s'\n", iaxs[fr.callno]->reg->username, ies.cause ? ies.cause : "<unknown>", ast_inet_ntoa(iabuf, sizeof(iabuf), sin.sin_addr));
						manager_event(EVENT_FLAG_SYSTEM, "Registry", "Channel: IAX2\r\nUsername: %s\r\nStatus: Rejected\r\nCause: %s\r\n", iaxs[fr.callno]->reg->username, ies.cause ? ies.cause : "<unknown>");
					}
					iaxs[fr.callno]->reg->regstate = REG_STATE_REJECTED;
				}
				/* Send ack immediately, before we destroy */
				send_command_immediate(iaxs[fr.callno], AST_FRAME_IAX, IAX_COMMAND_ACK, fr.ts, NULL, 0,fr.iseqno);
				iax2_destroy_nolock(fr.callno);
				break;
			case IAX_COMMAND_REGAUTH:
				/* Authentication request */
				if (registry_rerequest(&ies, fr.callno, &sin)) {
					memset(&ied0, 0, sizeof(ied0));
					iax_ie_append_str(&ied0, IAX_IE_CAUSE, "No authority found");
					iax_ie_append_byte(&ied0, IAX_IE_CAUSECODE, AST_CAUSE_FACILITY_NOT_SUBSCRIBED);
					send_command_final(iaxs[fr.callno], AST_FRAME_IAX, IAX_COMMAND_REJECT, 0, ied0.buf, ied0.pos, -1);
				}
				break;
			case IAX_COMMAND_TXREJ:
				iaxs[fr.callno]->transferring = 0;
				if (option_verbose > 2) 
					ast_verbose(VERBOSE_PREFIX_3 "Channel '%s' unable to transfer\n", iaxs[fr.callno]->owner ? iaxs[fr.callno]->owner->name : "<Unknown>");
				memset(&iaxs[fr.callno]->transfer, 0, sizeof(iaxs[fr.callno]->transfer));
				if (iaxs[fr.callno]->bridgecallno) {
					if (iaxs[iaxs[fr.callno]->bridgecallno]->transferring) {
						iaxs[iaxs[fr.callno]->bridgecallno]->transferring = 0;
						send_command(iaxs[iaxs[fr.callno]->bridgecallno], AST_FRAME_IAX, IAX_COMMAND_TXREJ, 0, NULL, 0, -1);
					}
				}
				break;
			case IAX_COMMAND_TXREADY:
				if (iaxs[fr.callno]->transferring == TRANSFER_BEGIN) {
					iaxs[fr.callno]->transferring = TRANSFER_READY;
					if (option_verbose > 2) 
						ast_verbose(VERBOSE_PREFIX_3 "Channel '%s' ready to transfer\n", iaxs[fr.callno]->owner ? iaxs[fr.callno]->owner->name : "<Unknown>");
					if (iaxs[fr.callno]->bridgecallno) {
						if (iaxs[iaxs[fr.callno]->bridgecallno]->transferring == TRANSFER_READY) {
							if (option_verbose > 2) 
								ast_verbose(VERBOSE_PREFIX_3 "Releasing %s and %s\n", iaxs[fr.callno]->owner ? iaxs[fr.callno]->owner->name : "<Unknown>",
										iaxs[iaxs[fr.callno]->bridgecallno]->owner ? iaxs[iaxs[fr.callno]->bridgecallno]->owner->name : "<Unknown>");

							/* They're both ready, now release them. */
							iaxs[iaxs[fr.callno]->bridgecallno]->transferring = TRANSFER_RELEASED;
							iaxs[fr.callno]->transferring = TRANSFER_RELEASED;
							ast_set_flag(iaxs[iaxs[fr.callno]->bridgecallno], IAX_ALREADYGONE);
							ast_set_flag(iaxs[fr.callno], IAX_ALREADYGONE);

							/* Stop doing lag & ping requests */
							stop_stuff(fr.callno);
							stop_stuff(iaxs[fr.callno]->bridgecallno);

							memset(&ied0, 0, sizeof(ied0));
							memset(&ied1, 0, sizeof(ied1));
							iax_ie_append_short(&ied0, IAX_IE_CALLNO, iaxs[iaxs[fr.callno]->bridgecallno]->peercallno);
							iax_ie_append_short(&ied1, IAX_IE_CALLNO, iaxs[fr.callno]->peercallno);
							send_command(iaxs[fr.callno], AST_FRAME_IAX, IAX_COMMAND_TXREL, 0, ied0.buf, ied0.pos, -1);
							send_command(iaxs[iaxs[fr.callno]->bridgecallno], AST_FRAME_IAX, IAX_COMMAND_TXREL, 0, ied1.buf, ied1.pos, -1);

						}
					}
				}
				break;
			case IAX_COMMAND_TXREQ:
				try_transfer(iaxs[fr.callno], &ies);
				break;
			case IAX_COMMAND_TXCNT:
				if (iaxs[fr.callno]->transferring)
					send_command_transfer(iaxs[fr.callno], AST_FRAME_IAX, IAX_COMMAND_TXACC, 0, NULL, 0);
				break;
			case IAX_COMMAND_TXREL:
				/* Send ack immediately, rather than waiting until we've changed addresses */
				send_command_immediate(iaxs[fr.callno], AST_FRAME_IAX, IAX_COMMAND_ACK, fr.ts, NULL, 0,fr.iseqno);
				complete_transfer(fr.callno, &ies);
				stop_stuff(fr.callno);	/* for attended transfer to work with libiax */
				break;	
			case IAX_COMMAND_DPREP:
				complete_dpreply(iaxs[fr.callno], &ies);
				break;
			case IAX_COMMAND_UNSUPPORT:
				ast_log(LOG_NOTICE, "Peer did not understand our iax command '%d'\n", ies.iax_unknown);
				break;
			case IAX_COMMAND_FWDOWNL:
				/* Firmware download */
				memset(&ied0, 0, sizeof(ied0));
				res = iax_firmware_append(&ied0, (unsigned char *)ies.devicetype, ies.fwdesc);
				if (res < 0)
					send_command_final(iaxs[fr.callno], AST_FRAME_IAX, IAX_COMMAND_REJECT, 0, ied0.buf, ied0.pos, -1);
				else if (res > 0)
					send_command_final(iaxs[fr.callno], AST_FRAME_IAX, IAX_COMMAND_FWDATA, 0, ied0.buf, ied0.pos, -1);
				else
					send_command(iaxs[fr.callno], AST_FRAME_IAX, IAX_COMMAND_FWDATA, 0, ied0.buf, ied0.pos, -1);
				break;
			default:
				ast_log(LOG_DEBUG, "Unknown IAX command %d on %d/%d\n", f.subclass, fr.callno, iaxs[fr.callno]->peercallno);
				memset(&ied0, 0, sizeof(ied0));
				iax_ie_append_byte(&ied0, IAX_IE_IAX_UNKNOWN, f.subclass);
				send_command(iaxs[fr.callno], AST_FRAME_IAX, IAX_COMMAND_UNSUPPORT, 0, ied0.buf, ied0.pos, -1);
			}
			/* Don't actually pass these frames along */
			if ((f.subclass != IAX_COMMAND_ACK) && 
			  (f.subclass != IAX_COMMAND_TXCNT) && 
			  (f.subclass != IAX_COMMAND_TXACC) && 
			  (f.subclass != IAX_COMMAND_INVAL) &&
			  (f.subclass != IAX_COMMAND_VNAK)) { 
			  	if (iaxs[fr.callno] && iaxs[fr.callno]->aseqno != iaxs[fr.callno]->iseqno)
					send_command_immediate(iaxs[fr.callno], AST_FRAME_IAX, IAX_COMMAND_ACK, fr.ts, NULL, 0,fr.iseqno);
			}
			ast_mutex_unlock(&iaxsl[fr.callno]);
			return 1;
		}
		/* Unless this is an ACK or INVAL frame, ack it */
		if (iaxs[fr.callno]->aseqno != iaxs[fr.callno]->iseqno)
			send_command_immediate(iaxs[fr.callno], AST_FRAME_IAX, IAX_COMMAND_ACK, fr.ts, NULL, 0,fr.iseqno);
	} else if (minivid) {
		f.frametype = AST_FRAME_VIDEO;
		if (iaxs[fr.callno]->videoformat > 0) 
			f.subclass = iaxs[fr.callno]->videoformat | (ntohs(vh->ts) & 0x8000 ? 1 : 0);
		else {
			ast_log(LOG_WARNING, "Received mini frame before first full video frame\n ");
			iax2_vnak(fr.callno);
			ast_mutex_unlock(&iaxsl[fr.callno]);
			return 1;
		}
		f.datalen = res - sizeof(struct ast_iax2_video_hdr);
		if (f.datalen)
			f.data = buf + sizeof(struct ast_iax2_video_hdr);
		else
			f.data = NULL;
#ifdef IAXTESTS
		if (test_resync) {
			fr.ts = (iaxs[fr.callno]->last & 0xFFFF8000L) | ((ntohs(mh->ts) + test_resync) & 0x7fff);
		} else
#endif /* IAXTESTS */
		fr.ts = (iaxs[fr.callno]->last & 0xFFFF8000L) | (ntohs(mh->ts) & 0x7fff);
	} else {
		/* A mini frame */
		f.frametype = AST_FRAME_VOICE;
		if (iaxs[fr.callno]->voiceformat > 0)
			f.subclass = iaxs[fr.callno]->voiceformat;
		else {
			ast_log(LOG_WARNING, "Received mini frame before first full voice frame\n ");
			iax2_vnak(fr.callno);
			ast_mutex_unlock(&iaxsl[fr.callno]);
			return 1;
		}
		f.datalen = res - sizeof(struct ast_iax2_mini_hdr);
		if (f.datalen < 0) {
			ast_log(LOG_WARNING, "Datalen < 0?\n");
			ast_mutex_unlock(&iaxsl[fr.callno]);
			return 1;
		}
		if (f.datalen)
			f.data = buf + sizeof(struct ast_iax2_mini_hdr);
		else
			f.data = NULL;
#ifdef IAXTESTS
		if (test_resync) {
			fr.ts = (iaxs[fr.callno]->last & 0xFFFF0000L) | ((ntohs(mh->ts) + test_resync) & 0xffff);
		} else
#endif /* IAXTESTS */
		fr.ts = (iaxs[fr.callno]->last & 0xFFFF0000L) | ntohs(mh->ts);
		/* FIXME? Surely right here would be the right place to undo timestamp wraparound? */
	}
	/* Don't pass any packets until we're started */
	if (!ast_test_flag(&iaxs[fr.callno]->state, IAX_STATE_STARTED)) {
		ast_mutex_unlock(&iaxsl[fr.callno]);
		return 1;
	}
	/* Common things */
	f.src = "IAX2";
	f.mallocd = 0;
	f.offset = 0;
	if (f.datalen && (f.frametype == AST_FRAME_VOICE)) {
		f.samples = ast_codec_get_samples(&f);
		/* We need to byteswap incoming slinear samples from network byte order */
		if (f.subclass == AST_FORMAT_SLINEAR)
			ast_frame_byteswap_be(&f);
	} else
		f.samples = 0;
	iax_frame_wrap(&fr, &f);

	/* If this is our most recent packet, use it as our basis for timestamping */
	if (iaxs[fr.callno]->last < fr.ts) {
		/*iaxs[fr.callno]->last = fr.ts; (do it afterwards cos schedule/forward_delivery needs the last ts too)*/
		fr.outoforder = 0;
	} else {
		if (option_debug && iaxdebug)
			ast_log(LOG_DEBUG, "Received out of order packet... (type=%d, subclass %d, ts = %d, last = %d)\n", f.frametype, f.subclass, fr.ts, iaxs[fr.callno]->last);
		fr.outoforder = -1;
	}
#ifdef BRIDGE_OPTIMIZATION
	if (iaxs[fr.callno]->bridgecallno) {
		forward_delivery(&fr);
	} else {
		duped_fr = iaxfrdup2(&fr);
		if (duped_fr) {
			schedule_delivery(duped_fr, updatehistory, 0);
			fr.ts = duped_fr->ts;
		}
	}
#else
	duped_fr = iaxfrdup2(&fr);
	if (duped_fr) {
		schedule_delivery(duped_fr, updatehistory, 0);
		fr.ts = duped_fr->ts;
	}
#endif

	if (iaxs[fr.callno]->last < fr.ts) {
		iaxs[fr.callno]->last = fr.ts;
#if 1
		if (option_debug && iaxdebug)
			ast_log(LOG_DEBUG, "For call=%d, set last=%d\n", fr.callno, fr.ts);
#endif
	}

	/* Always run again */
	ast_mutex_unlock(&iaxsl[fr.callno]);
	return 1;
}

static int iax2_do_register(struct iax2_registry *reg)
{
	struct iax_ie_data ied;
	if (option_debug && iaxdebug)
		ast_log(LOG_DEBUG, "Sending registration request for '%s'\n", reg->username);
	if (!reg->callno) {
		if (option_debug)
			ast_log(LOG_DEBUG, "Allocate call number\n");
		reg->callno = find_callno(0, 0, &reg->addr, NEW_FORCE, 1, defaultsockfd);
		if (reg->callno < 1) {
			ast_log(LOG_WARNING, "Unable to create call for registration\n");
			return -1;
		} else if (option_debug)
			ast_log(LOG_DEBUG, "Registration created on call %d\n", reg->callno);
		iaxs[reg->callno]->reg = reg;
	}
	/* Schedule the next registration attempt */
	if (reg->expire > -1)
		ast_sched_del(sched, reg->expire);
	/* Setup the next registration a little early */
	reg->expire  = ast_sched_add(sched, (5 * reg->refresh / 6) * 1000, iax2_do_register_s, reg);
	/* Send the request */
	memset(&ied, 0, sizeof(ied));
	iax_ie_append_str(&ied, IAX_IE_USERNAME, reg->username);
	iax_ie_append_short(&ied, IAX_IE_REFRESH, reg->refresh);
	send_command(iaxs[reg->callno],AST_FRAME_IAX, IAX_COMMAND_REGREQ, 0, ied.buf, ied.pos, -1);
	reg->regstate = REG_STATE_REGSENT;
	return 0;
}

static char *iax2_prov_complete_template_3rd(const char *line, const char *word, int pos, int state)
{
	if (pos != 3)
		return NULL;
	return iax_prov_complete_template(line, word, pos, state);
}

static int iax2_provision(struct sockaddr_in *end, int sockfd, char *dest, const char *template, int force)
{
	/* Returns 1 if provisioned, -1 if not able to find destination, or 0 if no provisioning
	   is found for template */
	struct iax_ie_data provdata;
	struct iax_ie_data ied;
	unsigned int sig;
	struct sockaddr_in sin;
	int callno;
	struct create_addr_info cai;

	memset(&cai, 0, sizeof(cai));

	if (option_debug)
		ast_log(LOG_DEBUG, "Provisioning '%s' from template '%s'\n", dest, template);

	if (iax_provision_build(&provdata, &sig, template, force)) {
		ast_log(LOG_DEBUG, "No provisioning found for template '%s'\n", template);
		return 0;
	}

	if (end) {
		memcpy(&sin, end, sizeof(sin));
		cai.sockfd = sockfd;
	} else if (create_addr(dest, &sin, &cai))
		return -1;

	/* Build the rest of the message */
	memset(&ied, 0, sizeof(ied));
	iax_ie_append_raw(&ied, IAX_IE_PROVISIONING, provdata.buf, provdata.pos);

	callno = find_callno(0, 0, &sin, NEW_FORCE, 1, cai.sockfd);
	if (!callno)
		return -1;

	ast_mutex_lock(&iaxsl[callno]);
	if (iaxs[callno]) {
		/* Schedule autodestruct in case they don't ever give us anything back */
		if (iaxs[callno]->autoid > -1)
			ast_sched_del(sched, iaxs[callno]->autoid);
		iaxs[callno]->autoid = ast_sched_add(sched, 15000, auto_hangup, (void *)(long)callno);
		ast_set_flag(iaxs[callno], IAX_PROVISION);
		/* Got a call number now, so go ahead and send the provisioning information */
		send_command(iaxs[callno], AST_FRAME_IAX, IAX_COMMAND_PROVISION, 0, ied.buf, ied.pos, -1);
	}
	ast_mutex_unlock(&iaxsl[callno]);

	return 1;
}

static char *papp = "IAX2Provision";
static char *psyn = "Provision a calling IAXy with a given template";
static char *pdescrip = 
"  IAX2Provision([template]): Provisions the calling IAXy (assuming\n"
"the calling entity is in fact an IAXy) with the given template or\n"
"default if one is not specified.  Returns -1 on error or 0 on success.\n";

/*! iax2provision
\ingroup applications
*/
static int iax2_prov_app(struct ast_channel *chan, void *data)
{
	int res;
	char *sdata;
	char *opts;
	int force =0;
	unsigned short callno = PTR_TO_CALLNO(chan->tech_pvt);
	char iabuf[INET_ADDRSTRLEN];
	if (ast_strlen_zero(data))
		data = "default";
	sdata = ast_strdupa(data);
	opts = strchr(sdata, '|');
	if (opts)
		*opts='\0';

	if (chan->type != channeltype) {
		ast_log(LOG_NOTICE, "Can't provision a non-IAX device!\n");
		return -1;
	} 
	if (!callno || !iaxs[callno] || !iaxs[callno]->addr.sin_addr.s_addr) {
		ast_log(LOG_NOTICE, "Can't provision something with no IP?\n");
		return -1;
	}
	res = iax2_provision(&iaxs[callno]->addr, iaxs[callno]->sockfd, NULL, sdata, force);
	if (option_verbose > 2)
		ast_verbose(VERBOSE_PREFIX_3 "Provisioned IAXY at '%s' with '%s'= %d\n", 
		ast_inet_ntoa(iabuf, sizeof(iabuf), iaxs[callno]->addr.sin_addr),
		sdata, res);
	return res;
}


static int iax2_prov_cmd(int fd, int argc, char *argv[])
{
	int force = 0;
	int res;
	if (argc < 4)
		return RESULT_SHOWUSAGE;
	if ((argc > 4)) {
		if (!strcasecmp(argv[4], "forced"))
			force = 1;
		else
			return RESULT_SHOWUSAGE;
	}
	res = iax2_provision(NULL, -1, argv[2], argv[3], force);
	if (res < 0)
		ast_cli(fd, "Unable to find peer/address '%s'\n", argv[2]);
	else if (res < 1)
		ast_cli(fd, "No template (including wildcard) matching '%s'\n", argv[3]);
	else
		ast_cli(fd, "Provisioning '%s' with template '%s'%s\n", argv[2], argv[3], force ? ", forced" : "");
	return RESULT_SUCCESS;
}

static int iax2_poke_noanswer(void *data)
{
	struct iax2_peer *peer = data;
	peer->pokeexpire = -1;
	if (peer->lastms > -1) {
		ast_log(LOG_NOTICE, "Peer '%s' is now UNREACHABLE! Time: %d\n", peer->name, peer->lastms);
		manager_event(EVENT_FLAG_SYSTEM, "PeerStatus", "Peer: IAX2/%s\r\nPeerStatus: Unreachable\r\nTime: %d\r\n", peer->name, peer->lastms);
		ast_device_state_changed("IAX2/%s", peer->name); /* Activate notification */
	}
	if (peer->callno > 0)
		iax2_destroy(peer->callno);
	peer->callno = 0;
	peer->lastms = -1;
	/* Try again quickly */
	peer->pokeexpire = ast_sched_add(sched, peer->pokefreqnotok, iax2_poke_peer_s, peer);
	return 0;
}

static int iax2_poke_peer(struct iax2_peer *peer, int heldcall)
{
	if (!peer->maxms || !peer->addr.sin_addr.s_addr) {
		/* IF we have no IP, or this isn't to be monitored, return
		  imeediately after clearing things out */
		peer->lastms = 0;
		peer->historicms = 0;
		peer->pokeexpire = -1;
		peer->callno = 0;
		return 0;
	}
	if (peer->callno > 0) {
		ast_log(LOG_NOTICE, "Still have a callno...\n");
		iax2_destroy(peer->callno);
	}
	if (heldcall)
		ast_mutex_unlock(&iaxsl[heldcall]);
	peer->callno = find_callno(0, 0, &peer->addr, NEW_FORCE, 0, peer->sockfd);
	if (heldcall)
		ast_mutex_lock(&iaxsl[heldcall]);
	if (peer->callno < 1) {
		ast_log(LOG_WARNING, "Unable to allocate call for poking peer '%s'\n", peer->name);
		return -1;
	}
	if (peer->pokeexpire > -1)
		ast_sched_del(sched, peer->pokeexpire);
	/* Speed up retransmission times */
	iaxs[peer->callno]->pingtime = peer->maxms / 4 + 1;
	iaxs[peer->callno]->peerpoke = peer;
	send_command(iaxs[peer->callno], AST_FRAME_IAX, IAX_COMMAND_POKE, 0, NULL, 0, -1);
	
	/* If the host is already unreachable then use the unreachable interval instead */
	if (peer->lastms < 0) {
		peer->pokeexpire = ast_sched_add(sched, peer->pokefreqnotok, iax2_poke_noanswer, peer);
	} else
		peer->pokeexpire = ast_sched_add(sched, DEFAULT_MAXMS * 2, iax2_poke_noanswer, peer);

	return 0;
}

static void free_context(struct iax2_context *con)
{
	struct iax2_context *conl;
	while(con) {
		conl = con;
		con = con->next;
		free(conl);
	}
}

static struct ast_channel *iax2_request(const char *type, int format, void *data, int *cause)
{
	int callno;
	int res;
	int fmt, native;
	struct sockaddr_in sin;
	struct ast_channel *c;
	struct parsed_dial_string pds;
	struct create_addr_info cai;
	char *tmpstr;

	memset(&pds, 0, sizeof(pds));
	tmpstr = ast_strdupa(data);
	parse_dial_string(tmpstr, &pds);

	memset(&cai, 0, sizeof(cai));
	cai.capability = iax2_capability;

	ast_copy_flags(&cai, &globalflags, IAX_NOTRANSFER | IAX_USEJITTERBUF | IAX_FORCEJITTERBUF);

	if (!pds.peer) {
		ast_log(LOG_WARNING, "No peer given\n");
		return NULL;
	}
	       
	
	/* Populate our address from the given */
	if (create_addr(pds.peer, &sin, &cai)) {
		*cause = AST_CAUSE_UNREGISTERED;
		return NULL;
	}

	if (pds.port)
		sin.sin_port = htons(atoi(pds.port));

	callno = find_callno(0, 0, &sin, NEW_FORCE, 1, cai.sockfd);
	if (callno < 1) {
		ast_log(LOG_WARNING, "Unable to create call\n");
		*cause = AST_CAUSE_CONGESTION;
		return NULL;
	}

	ast_mutex_lock(&iaxsl[callno]);

	/* If this is a trunk, update it now */
	ast_copy_flags(iaxs[callno], &cai, IAX_TRUNK | IAX_SENDANI | IAX_NOTRANSFER | IAX_USEJITTERBUF | IAX_FORCEJITTERBUF);	
	if (ast_test_flag(&cai, IAX_TRUNK))
		callno = make_trunk(callno, 1);
	iaxs[callno]->maxtime = cai.maxtime;
	if (cai.found)
		ast_copy_string(iaxs[callno]->host, pds.peer, sizeof(iaxs[callno]->host));

	c = ast_iax2_new(callno, AST_STATE_DOWN, cai.capability);

	ast_mutex_unlock(&iaxsl[callno]);

	if (c) {
		/* Choose a format we can live with */
		if (c->nativeformats & format) 
			c->nativeformats &= format;
		else {
			native = c->nativeformats;
			fmt = format;
			res = ast_translator_best_choice(&fmt, &native);
			if (res < 0) {
				ast_log(LOG_WARNING, "Unable to create translator path for %s to %s on %s\n",
					ast_getformatname(c->nativeformats), ast_getformatname(fmt), c->name);
				ast_hangup(c);
				return NULL;
			}
			c->nativeformats = native;
		}
		c->readformat = ast_best_codec(c->nativeformats);
		c->writeformat = c->readformat;
	}

	return c;
}

static void *network_thread(void *ignore)
{
	/* Our job is simple: Send queued messages, retrying if necessary.  Read frames 
	   from the network, and queue them for delivery to the channels */
	int res, count;
	struct iax_frame *f, *freeme;
	if (timingfd > -1)
		ast_io_add(io, timingfd, timing_read, AST_IO_IN | AST_IO_PRI, NULL);
	for(;;) {
		/* Go through the queue, sending messages which have not yet been
		   sent, and scheduling retransmissions if appropriate */
		ast_mutex_lock(&iaxq.lock);
		f = iaxq.head;
		count = 0;
		while(f) {
			freeme = NULL;
			if (!f->sentyet) {
				f->sentyet++;
				/* Send a copy immediately -- errors here are ok, so don't bother locking */
				if (iaxs[f->callno]) {
					send_packet(f);
					count++;
				} 
				if (f->retries < 0) {
					/* This is not supposed to be retransmitted */
					if (f->prev) 
						f->prev->next = f->next;
					else
						iaxq.head = f->next;
					if (f->next)
						f->next->prev = f->prev;
					else
						iaxq.tail = f->prev;
					iaxq.count--;
					/* Free the iax frame */
					freeme = f;
				} else {
					/* We need reliable delivery.  Schedule a retransmission */
					f->retries++;
					f->retrans = ast_sched_add(sched, f->retrytime, attempt_transmit, f);
				}
			}
			f = f->next;
			if (freeme)
				iax_frame_free(freeme);
		}
		ast_mutex_unlock(&iaxq.lock);
		if (count >= 20)
			ast_log(LOG_DEBUG, "chan_iax2: Sent %d queued outbound frames all at once\n", count);

		/* Now do the IO, and run scheduled tasks */
		res = ast_sched_wait(sched);
		if ((res > 1000) || (res < 0))
			res = 1000;
		res = ast_io_wait(io, res);
		if (res >= 0) {
			if (res >= 20)
				ast_log(LOG_DEBUG, "chan_iax2: ast_io_wait ran %d I/Os all at once\n", res);
			count = ast_sched_runq(sched);
			if (count >= 20)
				ast_log(LOG_DEBUG, "chan_iax2: ast_sched_runq ran %d scheduled tasks all at once\n", count);
		}
	}
	return NULL;
}

static int start_network_thread(void)
{
	return ast_pthread_create(&netthreadid, NULL, network_thread, NULL);
}

static struct iax2_context *build_context(char *context)
{
	struct iax2_context *con;

	if ((con = ast_calloc(1, sizeof(*con))))
		ast_copy_string(con->context, context, sizeof(con->context));
	
	return con;
}

static int get_auth_methods(char *value)
{
	int methods = 0;
	if (strstr(value, "rsa"))
		methods |= IAX_AUTH_RSA;
	if (strstr(value, "md5"))
		methods |= IAX_AUTH_MD5;
	if (strstr(value, "plaintext"))
		methods |= IAX_AUTH_PLAINTEXT;
	return methods;
}


/*! \brief Check if address can be used as packet source.
 \return 0  address available, 1  address unavailable, -1  error
*/
static int check_srcaddr(struct sockaddr *sa, socklen_t salen)
{
	int sd;
	int res;
	
	sd = socket(AF_INET, SOCK_DGRAM, 0);
	if (sd < 0) {
		ast_log(LOG_ERROR, "Socket: %s\n", strerror(errno));
		return -1;
	}

	res = bind(sd, sa, salen);
	if (res < 0) {
		ast_log(LOG_DEBUG, "Can't bind: %s\n", strerror(errno));
		close(sd);
		return 1;
	}

	close(sd);
	return 0;
}

/*! \brief Parse the "sourceaddress" value,
  lookup in netsock list and set peer's sockfd. Defaults to defaultsockfd if
  not found. */
static int peer_set_srcaddr(struct iax2_peer *peer, const char *srcaddr)
{
	struct sockaddr_in sin;
	int nonlocal = 1;
	int port = IAX_DEFAULT_PORTNO;
	int sockfd = defaultsockfd;
	char *tmp;
	char *addr;
	char *portstr;

	tmp = ast_strdupa(srcaddr);

	addr = strsep(&tmp, ":");
	portstr = tmp;

	if (portstr) {
		port = atoi(portstr);
		if (port < 1)
			port = IAX_DEFAULT_PORTNO;
	}
	
	if (!ast_get_ip(&sin, addr)) {
		struct ast_netsock *sock;
		int res;

		sin.sin_port = 0;
		res = check_srcaddr((struct sockaddr *) &sin, sizeof(sin));
		if (res == 0) {
			/* ip address valid. */
			sin.sin_port = htons(port);
			sock = ast_netsock_find(netsock, &sin);
			if (sock) {
				sockfd = ast_netsock_sockfd(sock);
				nonlocal = 0;
			}
		}
	}
		
	peer->sockfd = sockfd;

	if (nonlocal) {
		ast_log(LOG_WARNING, "Non-local or unbound address specified (%s) in sourceaddress for '%s', reverting to default\n",
			srcaddr, peer->name);
		return -1;
	} else {
		ast_log(LOG_DEBUG, "Using sourceaddress %s for '%s'\n", srcaddr, peer->name);
		return 0;
	}
}

		
/*! \brief Create peer structure based on configuration */
static struct iax2_peer *build_peer(const char *name, struct ast_variable *v, int temponly)
{
	struct iax2_peer *peer;
	struct iax2_peer *prev;
	struct ast_ha *oldha = NULL;
	int maskfound=0;
	int found=0;
	prev = NULL;
	ast_mutex_lock(&peerl.lock);
	if (!temponly) {
		peer = peerl.peers;
		while(peer) {
			if (!strcmp(peer->name, name)) {	
				break;
			}
			prev = peer;
			peer = peer->next;
		}
	} else
		peer = NULL;	
	if (peer) {
		found++;
		oldha = peer->ha;
		peer->ha = NULL;
		/* Already in the list, remove it and it will be added back (or FREE'd) */
		if (prev) {
			prev->next = peer->next;
		} else {
			peerl.peers = peer->next;
		}
		ast_mutex_unlock(&peerl.lock);
 	} else {
		ast_mutex_unlock(&peerl.lock);
		if ((peer = ast_calloc(1, sizeof(*peer)))) {
			peer->expire = -1;
			peer->pokeexpire = -1;
			peer->sockfd = defaultsockfd;
		}
	}
	if (peer) {
		ast_copy_flags(peer, &globalflags, IAX_MESSAGEDETAIL | IAX_USEJITTERBUF | IAX_FORCEJITTERBUF);
		peer->encmethods = iax2_encryption;
		peer->secret[0] = '\0';
		if (!found) {
			ast_copy_string(peer->name, name, sizeof(peer->name));
			peer->addr.sin_port = htons(IAX_DEFAULT_PORTNO);
			peer->expiry = min_reg_expire;
		}
		peer->prefs = prefs;
		peer->capability = iax2_capability;
		peer->smoothing = 0;
		peer->pokefreqok = DEFAULT_FREQ_OK;
		peer->pokefreqnotok = DEFAULT_FREQ_NOTOK;
		peer->context[0] = '\0';
		while(v) {
			if (!strcasecmp(v->name, "secret")) {
				if (!ast_strlen_zero(peer->secret)) {
					strncpy(peer->secret + strlen(peer->secret), ";", sizeof(peer->secret)-strlen(peer->secret) - 1);
					strncpy(peer->secret + strlen(peer->secret), v->value, sizeof(peer->secret)-strlen(peer->secret) - 1);
				} else
					ast_copy_string(peer->secret, v->value, sizeof(peer->secret));
			} else if (!strcasecmp(v->name, "mailbox")) {
				ast_copy_string(peer->mailbox, v->value, sizeof(peer->mailbox));
			} else if (!strcasecmp(v->name, "dbsecret")) {
				ast_copy_string(peer->dbsecret, v->value, sizeof(peer->dbsecret));
			} else if (!strcasecmp(v->name, "mailboxdetail")) {
				ast_set2_flag(peer, ast_true(v->value), IAX_MESSAGEDETAIL);	
			} else if (!strcasecmp(v->name, "trunk")) {
				ast_set2_flag(peer, ast_true(v->value), IAX_TRUNK);	
				if (ast_test_flag(peer, IAX_TRUNK) && (timingfd < 0)) {
					ast_log(LOG_WARNING, "Unable to support trunking on peer '%s' without zaptel timing\n", peer->name);
					ast_clear_flag(peer, IAX_TRUNK);
				}
			} else if (!strcasecmp(v->name, "auth")) {
				peer->authmethods = get_auth_methods(v->value);
			} else if (!strcasecmp(v->name, "encryption")) {
				peer->encmethods = get_encrypt_methods(v->value);
			} else if (!strcasecmp(v->name, "notransfer")) {
				ast_set2_flag(peer, ast_true(v->value), IAX_NOTRANSFER);	
			} else if (!strcasecmp(v->name, "jitterbuffer")) {
				ast_set2_flag(peer, ast_true(v->value), IAX_USEJITTERBUF);	
			} else if (!strcasecmp(v->name, "forcejitterbuffer")) {
				ast_set2_flag(peer, ast_true(v->value), IAX_FORCEJITTERBUF);	
			} else if (!strcasecmp(v->name, "host")) {
				if (!strcasecmp(v->value, "dynamic")) {
					/* They'll register with us */
					ast_set_flag(peer, IAX_DYNAMIC);	
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
				} else {
					/* Non-dynamic.  Make sure we become that way if we're not */
					if (peer->expire > -1)
						ast_sched_del(sched, peer->expire);
					peer->expire = -1;
					ast_clear_flag(peer, IAX_DYNAMIC);
					if (ast_dnsmgr_lookup(v->value, &peer->addr.sin_addr, &peer->dnsmgr)) {
						free(peer);
						return NULL;
					}
					if (!peer->addr.sin_port)
						peer->addr.sin_port = htons(IAX_DEFAULT_PORTNO);
				}
				if (!maskfound)
					inet_aton("255.255.255.255", &peer->mask);
			} else if (!strcasecmp(v->name, "defaultip")) {
				if (ast_get_ip(&peer->defaddr, v->value)) {
					free(peer);
					return NULL;
				}
			} else if (!strcasecmp(v->name, "sourceaddress")) {
				peer_set_srcaddr(peer, v->value);
			} else if (!strcasecmp(v->name, "permit") ||
					   !strcasecmp(v->name, "deny")) {
				peer->ha = ast_append_ha(v->name, v->value, peer->ha);
			} else if (!strcasecmp(v->name, "mask")) {
				maskfound++;
				inet_aton(v->value, &peer->mask);
			} else if (!strcasecmp(v->name, "context")) {
				if (ast_strlen_zero(peer->context))
					ast_copy_string(peer->context, v->value, sizeof(peer->context));
			} else if (!strcasecmp(v->name, "regexten")) {
				ast_copy_string(peer->regexten, v->value, sizeof(peer->regexten));
			} else if (!strcasecmp(v->name, "peercontext")) {
				if (ast_strlen_zero(peer->peercontext))
					ast_copy_string(peer->peercontext, v->value, sizeof(peer->peercontext));
			} else if (!strcasecmp(v->name, "port")) {
				if (ast_test_flag(peer, IAX_DYNAMIC))
					peer->defaddr.sin_port = htons(atoi(v->value));
				else
					peer->addr.sin_port = htons(atoi(v->value));
			} else if (!strcasecmp(v->name, "username")) {
				ast_copy_string(peer->username, v->value, sizeof(peer->username));
			} else if (!strcasecmp(v->name, "allow")) {
				ast_parse_allow_disallow(&peer->prefs, &peer->capability, v->value, 1);
			} else if (!strcasecmp(v->name, "disallow")) {
				ast_parse_allow_disallow(&peer->prefs, &peer->capability, v->value, 0);
			} else if (!strcasecmp(v->name, "callerid")) {
				ast_callerid_split(v->value, peer->cid_name, sizeof(peer->cid_name),
									peer->cid_num, sizeof(peer->cid_num));
				ast_set_flag(peer, IAX_HASCALLERID);	
			} else if (!strcasecmp(v->name, "sendani")) {
				ast_set2_flag(peer, ast_true(v->value), IAX_SENDANI);	
			} else if (!strcasecmp(v->name, "inkeys")) {
				ast_copy_string(peer->inkeys, v->value, sizeof(peer->inkeys));
			} else if (!strcasecmp(v->name, "outkey")) {
				ast_copy_string(peer->outkey, v->value, sizeof(peer->outkey));
			} else if (!strcasecmp(v->name, "qualify")) {
				if (!strcasecmp(v->value, "no")) {
					peer->maxms = 0;
				} else if (!strcasecmp(v->value, "yes")) {
					peer->maxms = DEFAULT_MAXMS;
				} else if (sscanf(v->value, "%d", &peer->maxms) != 1) {
					ast_log(LOG_WARNING, "Qualification of peer '%s' should be 'yes', 'no', or a number of milliseconds at line %d of iax.conf\n", peer->name, v->lineno);
					peer->maxms = 0;
				}
			} else if (!strcasecmp(v->name, "qualifysmoothing")) {
				peer->smoothing = ast_true(v->value);
			} else if (!strcasecmp(v->name, "qualifyfreqok")) {
				if (sscanf(v->value, "%d", &peer->pokefreqok) != 1) {
					ast_log(LOG_WARNING, "Qualification testing frequency of peer '%s' when OK should a number of milliseconds at line %d of iax.conf\n", peer->name, v->lineno);
				}
			} else if (!strcasecmp(v->name, "qualifyfreqnotok")) {
				if (sscanf(v->value, "%d", &peer->pokefreqnotok) != 1) {
					ast_log(LOG_WARNING, "Qualification testing frequency of peer '%s' when NOT OK should be a number of milliseconds at line %d of iax.conf\n", peer->name, v->lineno);
				} else ast_log(LOG_WARNING, "Set peer->pokefreqnotok to %d\n", peer->pokefreqnotok);
			} else if (!strcasecmp(v->name, "timezone")) {
				ast_copy_string(peer->zonetag, v->value, sizeof(peer->zonetag));
			}/* else if (strcasecmp(v->name,"type")) */
			/*	ast_log(LOG_WARNING, "Ignoring %s\n", v->name); */
			v=v->next;
		}
		if (!peer->authmethods)
			peer->authmethods = IAX_AUTH_MD5 | IAX_AUTH_PLAINTEXT;
		ast_clear_flag(peer, IAX_DELME);	
		/* Make sure these are IPv4 addresses */
		peer->addr.sin_family = AF_INET;
	}
	if (oldha)
		ast_free_ha(oldha);
	return peer;
}

/*! \brief Create in-memory user structure from configuration */
static struct iax2_user *build_user(const char *name, struct ast_variable *v, int temponly)
{
	struct iax2_user *prev, *user;
	struct iax2_context *con, *conl = NULL;
	struct ast_ha *oldha = NULL;
	struct iax2_context *oldcon = NULL;
	int format;
	char *varname = NULL, *varval = NULL;
	struct ast_variable *tmpvar = NULL;
	
	prev = NULL;
	ast_mutex_lock(&userl.lock);
	if (!temponly) {
		user = userl.users;
		while(user) {
			if (!strcmp(user->name, name)) {	
				break;
			}
			prev = user;
			user = user->next;
		}
	} else
		user = NULL;
	
	if (user) {
		oldha = user->ha;
		oldcon = user->contexts;
		user->ha = NULL;
		user->contexts = NULL;
		/* Already in the list, remove it and it will be added back (or FREE'd) */
		if (prev) {
			prev->next = user->next;
		} else {
			userl.users = user->next;
		}
		ast_mutex_unlock(&userl.lock);
 	} else {
		ast_mutex_unlock(&userl.lock);
		/* This is going to memset'd to 0 in the next block */
		user = ast_malloc(sizeof(*user));
	}
	
	if (user) {
		memset(user, 0, sizeof(struct iax2_user));
		user->prefs = prefs;
		user->capability = iax2_capability;
		user->encmethods = iax2_encryption;
		ast_copy_string(user->name, name, sizeof(user->name));
		ast_copy_string(user->language, language, sizeof(user->language));
		ast_copy_flags(user, &globalflags, IAX_USEJITTERBUF | IAX_FORCEJITTERBUF | IAX_CODEC_USER_FIRST | IAX_CODEC_NOPREFS | IAX_CODEC_NOCAP);	
		while(v) {
			if (!strcasecmp(v->name, "context")) {
				con = build_context(v->value);
				if (con) {
					if (conl)
						conl->next = con;
					else
						user->contexts = con;
					conl = con;
				}
			} else if (!strcasecmp(v->name, "permit") ||
					   !strcasecmp(v->name, "deny")) {
				user->ha = ast_append_ha(v->name, v->value, user->ha);
			} else if (!strcasecmp(v->name, "setvar")) {
				varname = ast_strdupa(v->value);
				if ((varval = strchr(varname,'='))) {
					*varval = '\0';
					varval++;
					if((tmpvar = ast_variable_new(varname, varval))) {
						tmpvar->next = user->vars; 
						user->vars = tmpvar;
					}
				}
			} else if (!strcasecmp(v->name, "allow")) {
				ast_parse_allow_disallow(&user->prefs, &user->capability, v->value, 1);
			} else if (!strcasecmp(v->name, "disallow")) {
				ast_parse_allow_disallow(&user->prefs, &user->capability,v->value, 0);
			} else if (!strcasecmp(v->name, "trunk")) {
				ast_set2_flag(user, ast_true(v->value), IAX_TRUNK);	
				if (ast_test_flag(user, IAX_TRUNK) && (timingfd < 0)) {
					ast_log(LOG_WARNING, "Unable to support trunking on user '%s' without zaptel timing\n", user->name);
					ast_clear_flag(user, IAX_TRUNK);
				}
			} else if (!strcasecmp(v->name, "auth")) {
				user->authmethods = get_auth_methods(v->value);
			} else if (!strcasecmp(v->name, "encryption")) {
				user->encmethods = get_encrypt_methods(v->value);
			} else if (!strcasecmp(v->name, "notransfer")) {
				ast_set2_flag(user, ast_true(v->value), IAX_NOTRANSFER);	
			} else if (!strcasecmp(v->name, "codecpriority")) {
				if(!strcasecmp(v->value, "caller"))
					ast_set_flag(user, IAX_CODEC_USER_FIRST);
				else if(!strcasecmp(v->value, "disabled"))
					ast_set_flag(user, IAX_CODEC_NOPREFS);
				else if(!strcasecmp(v->value, "reqonly")) {
					ast_set_flag(user, IAX_CODEC_NOCAP);
					ast_set_flag(user, IAX_CODEC_NOPREFS);
				}
			} else if (!strcasecmp(v->name, "jitterbuffer")) {
				ast_set2_flag(user, ast_true(v->value), IAX_USEJITTERBUF);	
			} else if (!strcasecmp(v->name, "forcejitterbuffer")) {
				ast_set2_flag(user, ast_true(v->value), IAX_FORCEJITTERBUF);	
			} else if (!strcasecmp(v->name, "dbsecret")) {
				ast_copy_string(user->dbsecret, v->value, sizeof(user->dbsecret));
			} else if (!strcasecmp(v->name, "secret")) {
				if (!ast_strlen_zero(user->secret)) {
					strncpy(user->secret + strlen(user->secret), ";", sizeof(user->secret) - strlen(user->secret) - 1);
					strncpy(user->secret + strlen(user->secret), v->value, sizeof(user->secret) - strlen(user->secret) - 1);
				} else
					ast_copy_string(user->secret, v->value, sizeof(user->secret));
			} else if (!strcasecmp(v->name, "callerid")) {
				ast_callerid_split(v->value, user->cid_name, sizeof(user->cid_name), user->cid_num, sizeof(user->cid_num));
				ast_set_flag(user, IAX_HASCALLERID);	
			} else if (!strcasecmp(v->name, "accountcode")) {
				ast_copy_string(user->accountcode, v->value, sizeof(user->accountcode));
			} else if (!strcasecmp(v->name, "language")) {
				ast_copy_string(user->language, v->value, sizeof(user->language));
			} else if (!strcasecmp(v->name, "amaflags")) {
				format = ast_cdr_amaflags2int(v->value);
				if (format < 0) {
					ast_log(LOG_WARNING, "Invalid AMA Flags: %s at line %d\n", v->value, v->lineno);
				} else {
					user->amaflags = format;
				}
			} else if (!strcasecmp(v->name, "inkeys")) {
				ast_copy_string(user->inkeys, v->value, sizeof(user->inkeys));
			}/* else if (strcasecmp(v->name,"type")) */
			/*	ast_log(LOG_WARNING, "Ignoring %s\n", v->name); */
			v = v->next;
		}
		if (!user->authmethods) {
			if (!ast_strlen_zero(user->secret)) {
				user->authmethods = IAX_AUTH_MD5 | IAX_AUTH_PLAINTEXT;
				if (!ast_strlen_zero(user->inkeys))
					user->authmethods |= IAX_AUTH_RSA;
			} else if (!ast_strlen_zero(user->inkeys)) {
				user->authmethods = IAX_AUTH_RSA;
			} else {
				user->authmethods = IAX_AUTH_MD5 | IAX_AUTH_PLAINTEXT;
			}
		}
		ast_clear_flag(user, IAX_DELME);
	}
	if (oldha)
		ast_free_ha(oldha);
	if (oldcon)
		free_context(oldcon);
	return user;
}

static void delete_users(void)
{
	struct iax2_user *user;
	struct iax2_peer *peer;
	struct iax2_registry *reg, *regl;

	ast_mutex_lock(&userl.lock);
	for (user=userl.users;user;) {
		ast_set_flag(user, IAX_DELME);
		user = user->next;
	}
	ast_mutex_unlock(&userl.lock);
	for (reg = registrations;reg;) {
		regl = reg;
		reg = reg->next;
		if (regl->expire > -1) {
			ast_sched_del(sched, regl->expire);
		}
		if (regl->callno) {
			/* XXX Is this a potential lock?  I don't think so, but you never know */
			ast_mutex_lock(&iaxsl[regl->callno]);
			if (iaxs[regl->callno]) {
				iaxs[regl->callno]->reg = NULL;
				iax2_destroy_nolock(regl->callno);
			}
			ast_mutex_unlock(&iaxsl[regl->callno]);
		}
		free(regl);
	}
	registrations = NULL;
	ast_mutex_lock(&peerl.lock);
	for (peer=peerl.peers;peer;) {
		/* Assume all will be deleted, and we'll find out for sure later */
		ast_set_flag(peer, IAX_DELME);
		peer = peer->next;
	}
	ast_mutex_unlock(&peerl.lock);
}

static void destroy_user(struct iax2_user *user)
{
	ast_free_ha(user->ha);
	free_context(user->contexts);
	if(user->vars) {
		ast_variables_destroy(user->vars);
		user->vars = NULL;
	}
	free(user);
}

static void prune_users(void)
{
	struct iax2_user *user, *usernext, *userlast = NULL;
	ast_mutex_lock(&userl.lock);
	for (user=userl.users;user;) {
		usernext = user->next;
		if (ast_test_flag(user, IAX_DELME)) {
			destroy_user(user);
			if (userlast)
				userlast->next = usernext;
			else
				userl.users = usernext;
		} else
			userlast = user;
		user = usernext;
	}
	ast_mutex_unlock(&userl.lock);
}

static void destroy_peer(struct iax2_peer *peer)
{
	int x;
	ast_free_ha(peer->ha);
	for (x=0;x<IAX_MAX_CALLS;x++) {
		ast_mutex_lock(&iaxsl[x]);
		if (iaxs[x] && (iaxs[x]->peerpoke == peer)) {
			iax2_destroy(x);
		}
		ast_mutex_unlock(&iaxsl[x]);
	}
	/* Delete it, it needs to disappear */
	if (peer->expire > -1)
		ast_sched_del(sched, peer->expire);
	if (peer->pokeexpire > -1)
		ast_sched_del(sched, peer->pokeexpire);
	if (peer->callno > 0)
		iax2_destroy(peer->callno);
	register_peer_exten(peer, 0);
	if (peer->dnsmgr)
		ast_dnsmgr_release(peer->dnsmgr);
	free(peer);
}

static void prune_peers(void){
	/* Prune peers who still are supposed to be deleted */
	struct iax2_peer *peer, *peerlast, *peernext;
	ast_mutex_lock(&peerl.lock);
	peerlast = NULL;
	for (peer=peerl.peers;peer;) {
		peernext = peer->next;
		if (ast_test_flag(peer, IAX_DELME)) {
			destroy_peer(peer);
			if (peerlast)
				peerlast->next = peernext;
			else
				peerl.peers = peernext;
		} else
			peerlast = peer;
		peer=peernext;
	}
	ast_mutex_unlock(&peerl.lock);
}

static void set_timing(void)
{
#ifdef IAX_TRUNKING
	int bs = trunkfreq * 8;
	if (timingfd > -1) {
		if (
#ifdef ZT_TIMERACK
			ioctl(timingfd, ZT_TIMERCONFIG, &bs) &&
#endif			
			ioctl(timingfd, ZT_SET_BLOCKSIZE, &bs))
			ast_log(LOG_WARNING, "Unable to set blocksize on timing source\n");
	}
#endif
}


/*! \brief Load configuration */
static int set_config(char *config_file, int reload)
{
	struct ast_config *cfg;
	int capability=iax2_capability;
	struct ast_variable *v;
	char *cat;
	char *utype;
	char *tosval;
	int format;
	int portno = IAX_DEFAULT_PORTNO;
	int  x;
	struct iax2_user *user;
	struct iax2_peer *peer;
	struct ast_netsock *ns;
#if 0
	static unsigned short int last_port=0;
#endif

	cfg = ast_config_load(config_file);
	
	if (!cfg) {
		ast_log(LOG_ERROR, "Unable to load config %s\n", config_file);
		return -1;
	}

	/* Reset global codec prefs */	
	memset(&prefs, 0 , sizeof(struct ast_codec_pref));
	
	/* Reset Global Flags */
	memset(&globalflags, 0, sizeof(globalflags));
	ast_set_flag(&globalflags, IAX_RTUPDATE);

#ifdef SO_NO_CHECK
	nochecksums = 0;
#endif

	min_reg_expire = IAX_DEFAULT_REG_EXPIRE;
	max_reg_expire = IAX_DEFAULT_REG_EXPIRE;

	v = ast_variable_browse(cfg, "general");

	/* Seed initial tos value */
	tosval = ast_variable_retrieve(cfg, "general", "tos");
	if (tosval) {
		if (ast_str2tos(tosval, &tos))
			ast_log(LOG_WARNING, "Invalid tos value, should be 'lowdelay', 'throughput', 'reliability', 'mincost', or 'none'\n");
	}
	while(v) {
		if (!strcasecmp(v->name, "bindport")){ 
			if (reload)
				ast_log(LOG_NOTICE, "Ignoring bindport on reload\n");
			else
				portno = atoi(v->value);
		} else if (!strcasecmp(v->name, "pingtime")) 
			ping_time = atoi(v->value);
		else if (!strcasecmp(v->name, "nochecksums")) {
#ifdef SO_NO_CHECK
			if (ast_true(v->value))
				nochecksums = 1;
			else
				nochecksums = 0;
#else
			if (ast_true(v->value))
				ast_log(LOG_WARNING, "Disabling RTP checksums is not supported on this operating system!\n");
#endif
		}
		else if (!strcasecmp(v->name, "maxjitterbuffer")) 
			maxjitterbuffer = atoi(v->value);
#ifdef NEWJB
		else if (!strcasecmp(v->name, "resyncthreshold")) 
			resyncthreshold = atoi(v->value);
		else if (!strcasecmp(v->name, "maxjitterinterps")) 
			maxjitterinterps = atoi(v->value);
#endif
		else if (!strcasecmp(v->name, "jittershrinkrate")) 
			jittershrinkrate = atoi(v->value);
		else if (!strcasecmp(v->name, "maxexcessbuffer")) 
			max_jitter_buffer = atoi(v->value);
		else if (!strcasecmp(v->name, "minexcessbuffer")) 
			min_jitter_buffer = atoi(v->value);
		else if (!strcasecmp(v->name, "lagrqtime")) 
			lagrq_time = atoi(v->value);
		else if (!strcasecmp(v->name, "dropcount")) 
			iax2_dropcount = atoi(v->value);
		else if (!strcasecmp(v->name, "maxregexpire")) 
			max_reg_expire = atoi(v->value);
		else if (!strcasecmp(v->name, "minregexpire")) 
			min_reg_expire = atoi(v->value);
		else if (!strcasecmp(v->name, "bindaddr")) {
			if (reload) {
				ast_log(LOG_NOTICE, "Ignoring bindaddr on reload\n");
			} else {
				if (!(ns = ast_netsock_bind(netsock, io, v->value, portno, tos, socket_read, NULL))) {
					ast_log(LOG_WARNING, "Unable apply binding to '%s' at line %d\n", v->value, v->lineno);
				} else {
					if (option_verbose > 1) {
						if (strchr(v->value, ':'))
							ast_verbose(VERBOSE_PREFIX_2 "Binding IAX2 to '%s'\n", v->value);
						else
							ast_verbose(VERBOSE_PREFIX_2 "Binding IAX2 to '%s:%d'\n", v->value, portno);
					}
					if (defaultsockfd < 0) 
						defaultsockfd = ast_netsock_sockfd(ns);
					ast_netsock_unref(ns);
				}
			}
		} else if (!strcasecmp(v->name, "authdebug"))
			authdebug = ast_true(v->value);
		else if (!strcasecmp(v->name, "encryption"))
			iax2_encryption = get_encrypt_methods(v->value);
		else if (!strcasecmp(v->name, "notransfer"))
			ast_set2_flag((&globalflags), ast_true(v->value), IAX_NOTRANSFER);	
		else if (!strcasecmp(v->name, "codecpriority")) {
			if(!strcasecmp(v->value, "caller"))
				ast_set_flag((&globalflags), IAX_CODEC_USER_FIRST);
			else if(!strcasecmp(v->value, "disabled"))
				ast_set_flag((&globalflags), IAX_CODEC_NOPREFS);
			else if(!strcasecmp(v->value, "reqonly")) {
				ast_set_flag((&globalflags), IAX_CODEC_NOCAP);
				ast_set_flag((&globalflags), IAX_CODEC_NOPREFS);
			}
		} else if (!strcasecmp(v->name, "jitterbuffer"))
			ast_set2_flag((&globalflags), ast_true(v->value), IAX_USEJITTERBUF);	
		else if (!strcasecmp(v->name, "forcejitterbuffer"))
			ast_set2_flag((&globalflags), ast_true(v->value), IAX_FORCEJITTERBUF);	
		else if (!strcasecmp(v->name, "delayreject"))
			delayreject = ast_true(v->value);
		else if (!strcasecmp(v->name, "mailboxdetail"))
			ast_set2_flag((&globalflags), ast_true(v->value), IAX_MESSAGEDETAIL);	
		else if (!strcasecmp(v->name, "rtcachefriends"))
			ast_set2_flag((&globalflags), ast_true(v->value), IAX_RTCACHEFRIENDS);	
		else if (!strcasecmp(v->name, "rtignoreregexpire"))
			ast_set2_flag((&globalflags), ast_true(v->value), IAX_RTIGNOREREGEXPIRE);	
		else if (!strcasecmp(v->name, "rtupdate"))
			ast_set2_flag((&globalflags), ast_true(v->value), IAX_RTUPDATE);
		else if (!strcasecmp(v->name, "trunktimestamps"))
			ast_set2_flag(&globalflags, ast_true(v->value), IAX_TRUNKTIMESTAMPS);
		else if (!strcasecmp(v->name, "rtautoclear")) {
			int i = atoi(v->value);
			if(i > 0)
				global_rtautoclear = i;
			else
				i = 0;
			ast_set2_flag((&globalflags), i || ast_true(v->value), IAX_RTAUTOCLEAR);	
		} else if (!strcasecmp(v->name, "trunkfreq")) {
			trunkfreq = atoi(v->value);
			if (trunkfreq < 10)
				trunkfreq = 10;
		} else if (!strcasecmp(v->name, "autokill")) {
			if (sscanf(v->value, "%d", &x) == 1) {
				if (x >= 0)
					autokill = x;
				else
					ast_log(LOG_NOTICE, "Nice try, but autokill has to be >0 or 'yes' or 'no' at line %d\n", v->lineno);
			} else if (ast_true(v->value)) {
				autokill = DEFAULT_MAXMS;
			} else {
				autokill = 0;
			}
		} else if (!strcasecmp(v->name, "bandwidth")) {
			if (!strcasecmp(v->value, "low")) {
				capability = IAX_CAPABILITY_LOWBANDWIDTH;
			} else if (!strcasecmp(v->value, "medium")) {
				capability = IAX_CAPABILITY_MEDBANDWIDTH;
			} else if (!strcasecmp(v->value, "high")) {
				capability = IAX_CAPABILITY_FULLBANDWIDTH;
			} else
				ast_log(LOG_WARNING, "bandwidth must be either low, medium, or high\n");
		} else if (!strcasecmp(v->name, "allow")) {
			ast_parse_allow_disallow(&prefs, &capability, v->value, 1);
		} else if (!strcasecmp(v->name, "disallow")) {
			ast_parse_allow_disallow(&prefs, &capability, v->value, 0);
		} else if (!strcasecmp(v->name, "register")) {
			iax2_register(v->value, v->lineno);
		} else if (!strcasecmp(v->name, "iaxcompat")) {
			iaxcompat = ast_true(v->value);
		} else if (!strcasecmp(v->name, "regcontext")) {
			ast_copy_string(regcontext, v->value, sizeof(regcontext));
			/* Create context if it doesn't exist already */
			if (!ast_context_find(regcontext))
				ast_context_create(NULL, regcontext, channeltype);
		} else if (!strcasecmp(v->name, "tos")) {
			if (ast_str2tos(v->value, &tos))
				ast_log(LOG_WARNING, "Invalid tos value at line %d, should be 'lowdelay', 'throughput', 'reliability', 'mincost', or 'none'\n", v->lineno);
		} else if (!strcasecmp(v->name, "accountcode")) {
			ast_copy_string(accountcode, v->value, sizeof(accountcode));
		} else if (!strcasecmp(v->name, "amaflags")) {
			format = ast_cdr_amaflags2int(v->value);
			if (format < 0) {
				ast_log(LOG_WARNING, "Invalid AMA Flags: %s at line %d\n", v->value, v->lineno);
			} else {
				amaflags = format;
			}
		} else if (!strcasecmp(v->name, "language")) {
                        ast_copy_string(language, v->value, sizeof(language));
		} /*else if (strcasecmp(v->name,"type")) */
		/*	ast_log(LOG_WARNING, "Ignoring %s\n", v->name); */
		v = v->next;
	}
	if (min_reg_expire > max_reg_expire) {
		ast_log(LOG_WARNING, "Minimum registration interval of %d is more than maximum of %d, resetting minimum to %d\n",
			min_reg_expire, max_reg_expire, max_reg_expire);
		min_reg_expire = max_reg_expire;
	}
	iax2_capability = capability;
	cat = ast_category_browse(cfg, NULL);
	while(cat) {
		if (strcasecmp(cat, "general")) {
			utype = ast_variable_retrieve(cfg, cat, "type");
			if (utype) {
				if (!strcasecmp(utype, "user") || !strcasecmp(utype, "friend")) {
					user = build_user(cat, ast_variable_browse(cfg, cat), 0);
					if (user) {
						ast_mutex_lock(&userl.lock);
						user->next = userl.users;
						userl.users = user;
						ast_mutex_unlock(&userl.lock);
					}
				}
				if (!strcasecmp(utype, "peer") || !strcasecmp(utype, "friend")) {
					peer = build_peer(cat, ast_variable_browse(cfg, cat), 0);
					if (peer) {
						ast_mutex_lock(&peerl.lock);
						peer->next = peerl.peers;
						peerl.peers = peer;
						ast_mutex_unlock(&peerl.lock);
						if (ast_test_flag(peer, IAX_DYNAMIC))
							reg_source_db(peer);
					}
				} else if (strcasecmp(utype, "user")) {
					ast_log(LOG_WARNING, "Unknown type '%s' for '%s' in %s\n", utype, cat, config_file);
				}
			} else
				ast_log(LOG_WARNING, "Section '%s' lacks type\n", cat);
		}
		cat = ast_category_browse(cfg, cat);
	}
	ast_config_destroy(cfg);
	set_timing();
	return capability;
}

static int reload_config(void)
{
	char *config = "iax.conf";
	struct iax2_registry *reg;
	struct iax2_peer *peer;
	ast_copy_string(accountcode, "", sizeof(accountcode));
	ast_copy_string(language, "", sizeof(language));
	amaflags = 0;
	delayreject = 0;
	ast_clear_flag((&globalflags), IAX_NOTRANSFER);	
	ast_clear_flag((&globalflags), IAX_USEJITTERBUF);	
	ast_clear_flag((&globalflags), IAX_FORCEJITTERBUF);	
	delete_users();
	set_config(config,1);
	prune_peers();
	prune_users();
	for (reg = registrations; reg; reg = reg->next)
		iax2_do_register(reg);
	/* Qualify hosts, too */
	ast_mutex_lock(&peerl.lock);
	for (peer = peerl.peers; peer; peer = peer->next)
		iax2_poke_peer(peer, 0);
	ast_mutex_unlock(&peerl.lock);
	reload_firmware();
	iax_provision_reload();
	return 0;
}

static int iax2_reload(int fd, int argc, char *argv[])
{
	return reload_config();
}

int reload(void)
{
	return reload_config();
}

static int cache_get_callno_locked(const char *data)
{
	struct sockaddr_in sin;
	int x;
	int callno;
	struct iax_ie_data ied;
	struct create_addr_info cai;
	struct parsed_dial_string pds;
	char *tmpstr;

	for (x=0; x<IAX_MAX_CALLS; x++) {
		/* Look for an *exact match* call.  Once a call is negotiated, it can only
		   look up entries for a single context */
		if (!ast_mutex_trylock(&iaxsl[x])) {
			if (iaxs[x] && !strcasecmp(data, iaxs[x]->dproot))
				return x;
			ast_mutex_unlock(&iaxsl[x]);
		}
	}

	/* No match found, we need to create a new one */

	memset(&cai, 0, sizeof(cai));
	memset(&ied, 0, sizeof(ied));
	memset(&pds, 0, sizeof(pds));

	tmpstr = ast_strdupa(data);
	parse_dial_string(tmpstr, &pds);

	/* Populate our address from the given */
	if (create_addr(pds.peer, &sin, &cai))
		return -1;

	ast_log(LOG_DEBUG, "peer: %s, username: %s, password: %s, context: %s\n",
		pds.peer, pds.username, pds.password, pds.context);

	callno = find_callno(0, 0, &sin, NEW_FORCE, 1, cai.sockfd);
	if (callno < 1) {
		ast_log(LOG_WARNING, "Unable to create call\n");
		return -1;
	}

	ast_mutex_lock(&iaxsl[callno]);
	ast_copy_string(iaxs[callno]->dproot, data, sizeof(iaxs[callno]->dproot));
	iaxs[callno]->capability = IAX_CAPABILITY_FULLBANDWIDTH;

	iax_ie_append_short(&ied, IAX_IE_VERSION, IAX_PROTO_VERSION);
	iax_ie_append_str(&ied, IAX_IE_CALLED_NUMBER, "TBD");
	/* the string format is slightly different from a standard dial string,
	   because the context appears in the 'exten' position
	*/
	if (pds.exten)
		iax_ie_append_str(&ied, IAX_IE_CALLED_CONTEXT, pds.exten);
	if (pds.username)
		iax_ie_append_str(&ied, IAX_IE_USERNAME, pds.username);
	iax_ie_append_int(&ied, IAX_IE_FORMAT, IAX_CAPABILITY_FULLBANDWIDTH);
	iax_ie_append_int(&ied, IAX_IE_CAPABILITY, IAX_CAPABILITY_FULLBANDWIDTH);
	/* Keep password handy */
	if (pds.password)
		ast_copy_string(iaxs[callno]->secret, pds.password, sizeof(iaxs[callno]->secret));
	if (pds.key)
		ast_copy_string(iaxs[callno]->outkey, pds.key, sizeof(iaxs[callno]->outkey));
	/* Start the call going */
	send_command(iaxs[callno], AST_FRAME_IAX, IAX_COMMAND_NEW, 0, ied.buf, ied.pos, -1);

	return callno;
}

static struct iax2_dpcache *find_cache(struct ast_channel *chan, const char *data, const char *context, const char *exten, int priority)
{
	struct iax2_dpcache *dp, *prev = NULL, *next;
	struct timeval tv;
	int x;
	int com[2];
	int timeout;
	int old=0;
	int outfd;
	int abort;
	int callno;
	struct ast_channel *c;
	struct ast_frame *f;
	gettimeofday(&tv, NULL);
	dp = dpcache;
	while(dp) {
		next = dp->next;
		/* Expire old caches */
		if (ast_tvcmp(tv, dp->expiry) > 0) {
				/* It's expired, let it disappear */
				if (prev)
					prev->next = dp->next;
				else
					dpcache = dp->next;
				if (!dp->peer && !(dp->flags & CACHE_FLAG_PENDING) && !dp->callno) {
					/* Free memory and go again */
					free(dp);
				} else {
					ast_log(LOG_WARNING, "DP still has peer field or pending or callno (flags = %d, peer = %p callno = %d)\n", dp->flags, dp->peer, dp->callno);
				}
				dp = next;
				continue;
		}
		/* We found an entry that matches us! */
		if (!strcmp(dp->peercontext, data) && !strcmp(dp->exten, exten)) 
			break;
		prev = dp;
		dp = next;
	}
	if (!dp) {
		/* No matching entry.  Create a new one. */
		/* First, can we make a callno? */
		callno = cache_get_callno_locked(data);
		if (callno < 0) {
			ast_log(LOG_WARNING, "Unable to generate call for '%s'\n", data);
			return NULL;
		}
		if (!(dp = ast_calloc(1, sizeof(*dp)))) {
			ast_mutex_unlock(&iaxsl[callno]);
			return NULL;
		}
		ast_copy_string(dp->peercontext, data, sizeof(dp->peercontext));
		ast_copy_string(dp->exten, exten, sizeof(dp->exten));
		gettimeofday(&dp->expiry, NULL);
		dp->orig = dp->expiry;
		/* Expires in 30 mins by default */
		dp->expiry.tv_sec += iaxdefaultdpcache;
		dp->next = dpcache;
		dp->flags = CACHE_FLAG_PENDING;
		for (x=0;x<sizeof(dp->waiters) / sizeof(dp->waiters[0]); x++)
			dp->waiters[x] = -1;
		dpcache = dp;
		dp->peer = iaxs[callno]->dpentries;
		iaxs[callno]->dpentries = dp;
		/* Send the request if we're already up */
		if (ast_test_flag(&iaxs[callno]->state, IAX_STATE_STARTED))
			iax2_dprequest(dp, callno);
		ast_mutex_unlock(&iaxsl[callno]);
	}
	/* By here we must have a dp */
	if (dp->flags & CACHE_FLAG_PENDING) {
		/* Okay, here it starts to get nasty.  We need a pipe now to wait
		   for a reply to come back so long as it's pending */
		for (x=0;x<sizeof(dp->waiters) / sizeof(dp->waiters[0]); x++) {
			/* Find an empty slot */
			if (dp->waiters[x] < 0)
				break;
		}
		if (x >= sizeof(dp->waiters) / sizeof(dp->waiters[0])) {
			ast_log(LOG_WARNING, "No more waiter positions available\n");
			return NULL;
		}
		if (pipe(com)) {
			ast_log(LOG_WARNING, "Unable to create pipe for comm\n");
			return NULL;
		}
		dp->waiters[x] = com[1];
		/* Okay, now we wait */
		timeout = iaxdefaulttimeout * 1000;
		/* Temporarily unlock */
		ast_mutex_unlock(&dpcache_lock);
		/* Defer any dtmf */
		if (chan)
			old = ast_channel_defer_dtmf(chan);
		abort = 0;
		while(timeout) {
			c = ast_waitfor_nandfds(&chan, chan ? 1 : 0, &com[0], 1, NULL, &outfd, &timeout);
			if (outfd > -1) {
				break;
			}
			if (c) {
				f = ast_read(c);
				if (f)
					ast_frfree(f);
				else {
					/* Got hung up on, abort! */
					break;
					abort = 1;
				}
			}
		}
		if (!timeout) {
			ast_log(LOG_WARNING, "Timeout waiting for %s exten %s\n", data, exten);
		}
		ast_mutex_lock(&dpcache_lock);
		dp->waiters[x] = -1;
		close(com[1]);
		close(com[0]);
		if (abort) {
			/* Don't interpret anything, just abort.  Not sure what th epoint
			  of undeferring dtmf on a hung up channel is but hey whatever */
			if (!old && chan)
				ast_channel_undefer_dtmf(chan);
			return NULL;
		}
		if (!(dp->flags & CACHE_FLAG_TIMEOUT)) {
			/* Now to do non-independent analysis the results of our wait */
			if (dp->flags & CACHE_FLAG_PENDING) {
				/* Still pending... It's a timeout.  Wake everybody up.  Consider it no longer
				   pending.  Don't let it take as long to timeout. */
				dp->flags &= ~CACHE_FLAG_PENDING;
				dp->flags |= CACHE_FLAG_TIMEOUT;
				/* Expire after only 60 seconds now.  This is designed to help reduce backlog in heavily loaded
				   systems without leaving it unavailable once the server comes back online */
				dp->expiry.tv_sec = dp->orig.tv_sec + 60;
				for (x=0;x<sizeof(dp->waiters) / sizeof(dp->waiters[0]); x++)
					if (dp->waiters[x] > -1)
						write(dp->waiters[x], "asdf", 4);
			}
		}
		/* Our caller will obtain the rest */
		if (!old && chan)
			ast_channel_undefer_dtmf(chan);
	}
	return dp;	
}

/*! \brief Part of the IAX2 switch interface */
static int iax2_exists(struct ast_channel *chan, const char *context, const char *exten, int priority, const char *callerid, const char *data)
{
	struct iax2_dpcache *dp;
	int res = 0;
#if 0
	ast_log(LOG_NOTICE, "iax2_exists: con: %s, exten: %s, pri: %d, cid: %s, data: %s\n", context, exten, priority, callerid ? callerid : "<unknown>", data);
#endif
	if ((priority != 1) && (priority != 2))
		return 0;
	ast_mutex_lock(&dpcache_lock);
	dp = find_cache(chan, data, context, exten, priority);
	if (dp) {
		if (dp->flags & CACHE_FLAG_EXISTS)
			res= 1;
	}
	ast_mutex_unlock(&dpcache_lock);
	if (!dp) {
		ast_log(LOG_WARNING, "Unable to make DP cache\n");
	}
	return res;
}

/*! \brief part of the IAX2 dial plan switch interface */
static int iax2_canmatch(struct ast_channel *chan, const char *context, const char *exten, int priority, const char *callerid, const char *data)
{
	int res = 0;
	struct iax2_dpcache *dp;
#if 0
	ast_log(LOG_NOTICE, "iax2_canmatch: con: %s, exten: %s, pri: %d, cid: %s, data: %s\n", context, exten, priority, callerid ? callerid : "<unknown>", data);
#endif
	if ((priority != 1) && (priority != 2))
		return 0;
	ast_mutex_lock(&dpcache_lock);
	dp = find_cache(chan, data, context, exten, priority);
	if (dp) {
		if (dp->flags & CACHE_FLAG_CANEXIST)
			res= 1;
	}
	ast_mutex_unlock(&dpcache_lock);
	if (!dp) {
		ast_log(LOG_WARNING, "Unable to make DP cache\n");
	}
	return res;
}

/*! \brief Part of the IAX2 Switch interface */
static int iax2_matchmore(struct ast_channel *chan, const char *context, const char *exten, int priority, const char *callerid, const char *data)
{
	int res = 0;
	struct iax2_dpcache *dp;
#if 0
	ast_log(LOG_NOTICE, "iax2_matchmore: con: %s, exten: %s, pri: %d, cid: %s, data: %s\n", context, exten, priority, callerid ? callerid : "<unknown>", data);
#endif
	if ((priority != 1) && (priority != 2))
		return 0;
	ast_mutex_lock(&dpcache_lock);
	dp = find_cache(chan, data, context, exten, priority);
	if (dp) {
		if (dp->flags & CACHE_FLAG_MATCHMORE)
			res= 1;
	}
	ast_mutex_unlock(&dpcache_lock);
	if (!dp) {
		ast_log(LOG_WARNING, "Unable to make DP cache\n");
	}
	return res;
}

/*! \brief Execute IAX2 dialplan switch */
static int iax2_exec(struct ast_channel *chan, const char *context, const char *exten, int priority, const char *callerid, int newstack, const char *data)
{
	char odata[256];
	char req[256];
	char *ncontext;
	struct iax2_dpcache *dp;
	struct ast_app *dial;
#if 0
	ast_log(LOG_NOTICE, "iax2_exec: con: %s, exten: %s, pri: %d, cid: %s, data: %s, newstack: %d\n", context, exten, priority, callerid ? callerid : "<unknown>", data, newstack);
#endif
	if (priority == 2) {
		/* Indicate status, can be overridden in dialplan */
		const char *dialstatus = pbx_builtin_getvar_helper(chan, "DIALSTATUS");
		if (dialstatus) {
			dial = pbx_findapp(dialstatus);
			if (dial) 
				pbx_exec(chan, dial, "", newstack);
		}
		return -1;
	} else if (priority != 1)
		return -1;
	ast_mutex_lock(&dpcache_lock);
	dp = find_cache(chan, data, context, exten, priority);
	if (dp) {
		if (dp->flags & CACHE_FLAG_EXISTS) {
			ast_copy_string(odata, data, sizeof(odata));
			ncontext = strchr(odata, '/');
			if (ncontext) {
				*ncontext = '\0';
				ncontext++;
				snprintf(req, sizeof(req), "IAX2/%s/%s@%s", odata, exten, ncontext);
			} else {
				snprintf(req, sizeof(req), "IAX2/%s/%s", odata, exten);
			}
			if (option_verbose > 2)
				ast_verbose(VERBOSE_PREFIX_3 "Executing Dial('%s')\n", req);
		} else {
			ast_mutex_unlock(&dpcache_lock);
			ast_log(LOG_WARNING, "Can't execute nonexistent extension '%s[@%s]' in data '%s'\n", exten, context, data);
			return -1;
		}
	}
	ast_mutex_unlock(&dpcache_lock);
	dial = pbx_findapp("Dial");
	if (dial) {
		return pbx_exec(chan, dial, req, newstack);
	} else {
		ast_log(LOG_WARNING, "No dial application registered\n");
	}
	return -1;
}

static char *function_iaxpeer(struct ast_channel *chan, char *cmd, char *data, char *buf, size_t len)
{
	char *ret = NULL;
	struct iax2_peer *peer;
	char *peername, *colname;
	char iabuf[INET_ADDRSTRLEN];

	peername = ast_strdupa(data);

	/* if our channel, return the IP address of the endpoint of current channel */
	if (!strcmp(peername,"CURRENTCHANNEL")) {
	        unsigned short callno = PTR_TO_CALLNO(chan->tech_pvt);
		ast_copy_string(buf, iaxs[callno]->addr.sin_addr.s_addr ? ast_inet_ntoa(iabuf, sizeof(iabuf), iaxs[callno]->addr.sin_addr) : "", len);
		return buf;
	}

	if ((colname = strchr(peername, ':'))) {
		*colname = '\0';
		colname++;
	} else {
		colname = "ip";
	}
	if (!(peer = find_peer(peername, 1)))
		return ret;

	if (!strcasecmp(colname, "ip")) {
		ast_copy_string(buf, peer->addr.sin_addr.s_addr ? ast_inet_ntoa(iabuf, sizeof(iabuf), peer->addr.sin_addr) : "", len);
	} else  if (!strcasecmp(colname, "status")) {
		peer_status(peer, buf, len); 
	} else  if (!strcasecmp(colname, "mailbox")) {
		ast_copy_string(buf, peer->mailbox, len);
	} else  if (!strcasecmp(colname, "context")) {
		ast_copy_string(buf, peer->context, len);
	} else  if (!strcasecmp(colname, "expire")) {
		snprintf(buf, len, "%d", peer->expire);
	} else  if (!strcasecmp(colname, "dynamic")) {
		ast_copy_string(buf, (ast_test_flag(peer, IAX_DYNAMIC) ? "yes" : "no"), len);
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

	return ret;
}

struct ast_custom_function iaxpeer_function = {
    .name = "IAXPEER",
    .synopsis = "Gets IAX peer information",
    .syntax = "IAXPEER(<peername|CURRENTCHANNEL>[:item])",
    .read = function_iaxpeer,
	.desc = "If peername specified, valid items are:\n"
	"- ip (default)          The IP address.\n"
	"- status                The peer's status (if qualify=yes)\n"
	"- mailbox               The configured mailbox.\n"
	"- context               The configured context.\n"
	"- expire                The epoch time of the next expire.\n"
	"- dynamic               Is it dynamic? (yes/no).\n"
	"- callerid_name         The configured Caller ID name.\n"
	"- callerid_num          The configured Caller ID number.\n"
	"- codecs                The configured codecs.\n"
	"- codec[x]              Preferred codec index number 'x' (beginning with zero).\n"
	"\n"
	"If CURRENTCHANNEL specified, returns IP address of current channel\n"
	"\n"
};


/*! \brief Part of the device state notification system ---*/
static int iax2_devicestate(void *data) 
{
	char *dest = (char *) data;
	struct iax2_peer *p;
	int found = 0;
	char *ext, *host;
	char tmp[256];
	int res = AST_DEVICE_INVALID;

	ast_copy_string(tmp, dest, sizeof(tmp));
	host = strchr(tmp, '@');
	if (host) {
		*host = '\0';
		host++;
		ext = tmp;
	} else {
		host = tmp;
		ext = NULL;
	}

	if (option_debug > 2)
		ast_log(LOG_DEBUG, "Checking device state for device %s\n", dest);

	/* SLD: FIXME: second call to find_peer during registration */
	p = find_peer(host, 1);
	if (p) {
		found++;
		res = AST_DEVICE_UNAVAILABLE;
		if (option_debug > 2) 
			ast_log(LOG_DEBUG, "iax2_devicestate(%s): Found peer. What's device state of %s? addr=%d, defaddr=%d maxms=%d, lastms=%d\n",
				host, dest, p->addr.sin_addr.s_addr, p->defaddr.sin_addr.s_addr, p->maxms, p->lastms);

		if ((p->addr.sin_addr.s_addr || p->defaddr.sin_addr.s_addr) &&
		    (!p->maxms || ((p->lastms > -1) && (p->historicms <= p->maxms)))) {
			/* Peer is registered, or have default IP address
			   and a valid registration */
			if (p->historicms == 0 || p->historicms <= p->maxms)
				/* let the core figure out whether it is in use or not */
				res = AST_DEVICE_UNKNOWN;	
		}
	} else {
		if (option_debug > 2) 
			ast_log(LOG_DEBUG, "Devicestate: Can't find peer %s.\n", host);
	}
	
	if (p && ast_test_flag(p, IAX_TEMPONLY))
		destroy_peer(p);
	return res;
}

static struct ast_switch iax2_switch = 
{
	name: 			"IAX2",
	description: 		"IAX Remote Dialplan Switch",
	exists:			iax2_exists,
	canmatch:		iax2_canmatch,
	exec:			iax2_exec,
	matchmore:		iax2_matchmore,
};

static char show_stats_usage[] =
"Usage: iax show stats\n"
"       Display statistics on IAX channel driver.\n";

static char show_cache_usage[] =
"Usage: iax show cache\n"
"       Display currently cached IAX Dialplan results.\n";

static char show_peer_usage[] =
"Usage: iax show peer <name>\n"
"       Display details on specific IAX peer\n";

static char prune_realtime_usage[] =
"Usage: iax2 prune realtime [<peername>|all]\n"
"       Prunes object(s) from the cache\n";

static char iax2_reload_usage[] =
"Usage: iax2 reload\n"
"       Reloads IAX configuration from iax.conf\n";

static char show_prov_usage[] =
"Usage: iax2 provision <host> <template> [forced]\n"
"       Provisions the given peer or IP address using a template\n"
"       matching either 'template' or '*' if the template is not\n"
"       found.  If 'forced' is specified, even empty provisioning\n"
"       fields will be provisioned as empty fields.\n";

static char show_users_usage[] = 
"Usage: iax2 show users [like <pattern>]\n"
"       Lists all known IAX2 users.\n"
"       Optional regular expression pattern is used to filter the user list.\n";

static char show_channels_usage[] = 
"Usage: iax2 show channels\n"
"       Lists all currently active IAX channels.\n";

static char show_netstats_usage[] = 
"Usage: iax2 show netstats\n"
"       Lists network status for all currently active IAX channels.\n";

static char show_peers_usage[] = 
"Usage: iax2 show peers [registered] [like <pattern>]\n"
"       Lists all known IAX2 peers.\n"
"       Optional 'registered' argument lists only peers with known addresses.\n"
"       Optional regular expression pattern is used to filter the peer list.\n";

static char show_firmware_usage[] = 
"Usage: iax2 show firmware\n"
"       Lists all known IAX firmware images.\n";

static char show_reg_usage[] =
"Usage: iax2 show registry\n"
"       Lists all registration requests and status.\n";

static char debug_usage[] = 
"Usage: iax2 debug\n"
"       Enables dumping of IAX packets for debugging purposes\n";

static char no_debug_usage[] = 
"Usage: iax2 no debug\n"
"       Disables dumping of IAX packets for debugging purposes\n";

static char debug_trunk_usage[] =
"Usage: iax2 trunk debug\n"
"       Requests current status of IAX trunking\n";

static char no_debug_trunk_usage[] =
"Usage: iax2 no trunk debug\n"
"       Requests current status of IAX trunking\n";

static char debug_jb_usage[] =
"Usage: iax2 jb debug\n"
"       Enables jitterbuffer debugging information\n";

static char no_debug_jb_usage[] =
"Usage: iax2 no jb debug\n"
"       Disables jitterbuffer debugging information\n";

static char iax2_test_losspct_usage[] =
"Usage: iax2 test losspct <percentage>\n"
"       For testing, throws away <percentage> percent of incoming packets\n";

#ifdef IAXTESTS
static char iax2_test_late_usage[] =
"Usage: iax2 test late <ms>\n"
"       For testing, count the next frame as <ms> ms late\n";

static char iax2_test_resync_usage[] =
"Usage: iax2 test resync <ms>\n"
"       For testing, adjust all future frames by <ms> ms\n";

static char iax2_test_jitter_usage[] =
"Usage: iax2 test jitter <ms> <pct>\n"
"       For testing, simulate maximum jitter of +/- <ms> on <pct> percentage of packets. If <pct> is not specified, adds jitter to all packets.\n";
#endif /* IAXTESTS */

static struct ast_cli_entry iax2_cli[] = {
	{ { "iax2", "set", "jitter", NULL }, iax2_set_jitter,
	  "Sets IAX jitter buffer", jitter_usage },
	{ { "iax2", "show", "stats", NULL }, iax2_show_stats,
	  "Display IAX statistics", show_stats_usage },
	{ { "iax2", "show", "cache", NULL }, iax2_show_cache,
	  "Display IAX cached dialplan", show_cache_usage },
	{ { "iax2", "show", "peer", NULL }, iax2_show_peer,
	  "Show details on specific IAX peer", show_peer_usage, complete_iax2_show_peer },
	{ { "iax2", "prune", "realtime", NULL }, iax2_prune_realtime,
	  "Prune a cached realtime lookup", prune_realtime_usage, complete_iax2_show_peer },
	{ { "iax2", "reload", NULL }, iax2_reload,
	  "Reload IAX configuration", iax2_reload_usage },
	{ { "iax2", "show", "users", NULL }, iax2_show_users,
	  "Show defined IAX users", show_users_usage },
	{ { "iax2", "show", "firmware", NULL }, iax2_show_firmware,
	  "Show available IAX firmwares", show_firmware_usage },
	{ { "iax2", "show", "channels", NULL }, iax2_show_channels,
	  "Show active IAX channels", show_channels_usage },
	{ { "iax2", "show", "netstats", NULL }, iax2_show_netstats,
	  "Show active IAX channel netstats", show_netstats_usage },
	{ { "iax2", "show", "peers", NULL }, iax2_show_peers,
	  "Show defined IAX peers", show_peers_usage },
	{ { "iax2", "show", "registry", NULL }, iax2_show_registry,
	  "Show IAX registration status", show_reg_usage },
	{ { "iax2", "debug", NULL }, iax2_do_debug,
	  "Enable IAX debugging", debug_usage },
	{ { "iax2", "trunk", "debug", NULL }, iax2_do_trunk_debug,
	  "Enable IAX trunk debugging", debug_trunk_usage },
	{ { "iax2", "jb", "debug", NULL }, iax2_do_jb_debug,
	  "Enable IAX jitterbuffer debugging", debug_jb_usage },
	{ { "iax2", "no", "debug", NULL }, iax2_no_debug,
	  "Disable IAX debugging", no_debug_usage },
	{ { "iax2", "no", "trunk", "debug", NULL }, iax2_no_trunk_debug,
	  "Disable IAX trunk debugging", no_debug_trunk_usage },
	{ { "iax2", "no", "jb", "debug", NULL }, iax2_no_jb_debug,
	  "Disable IAX jitterbuffer debugging", no_debug_jb_usage },
	{ { "iax2", "test", "losspct", NULL }, iax2_test_losspct,
	  "Set IAX2 incoming frame loss percentage", iax2_test_losspct_usage },
	{ { "iax2", "provision", NULL }, iax2_prov_cmd,
	  "Provision an IAX device", show_prov_usage, iax2_prov_complete_template_3rd },
#ifdef IAXTESTS
	{ { "iax2", "test", "late", NULL }, iax2_test_late,
	  "Test the receipt of a late frame", iax2_test_late_usage },
	{ { "iax2", "test", "resync", NULL }, iax2_test_resync,
	  "Test a resync in received timestamps", iax2_test_resync_usage },
	{ { "iax2", "test", "jitter", NULL }, iax2_test_jitter,
	  "Simulates jitter for testing", iax2_test_jitter_usage },
#endif /* IAXTESTS */
};

static int __unload_module(void)
{
	int x;
	/* Cancel the network thread, close the net socket */
	if (netthreadid != AST_PTHREADT_NULL) {
		pthread_cancel(netthreadid);
		pthread_join(netthreadid, NULL);
	}
	ast_netsock_release(netsock);
	for (x=0;x<IAX_MAX_CALLS;x++)
		if (iaxs[x])
			iax2_destroy(x);
	ast_manager_unregister( "IAXpeers" );
	ast_manager_unregister( "IAXnetstats" );
	ast_unregister_application(papp);
	ast_cli_unregister_multiple(iax2_cli, sizeof(iax2_cli) / sizeof(iax2_cli[0]));
	ast_unregister_switch(&iax2_switch);
	ast_channel_unregister(&iax2_tech);
	delete_users();
	iax_provision_unload();
	return 0;
}

int unload_module()
{
	ast_mutex_destroy(&iaxq.lock);
	ast_mutex_destroy(&userl.lock);
	ast_mutex_destroy(&peerl.lock);
	ast_mutex_destroy(&waresl.lock);
	ast_custom_function_unregister(&iaxpeer_function);
	return __unload_module();
}


/*! \brief Load IAX2 module, load configuraiton ---*/
int load_module(void)
{
	char *config = "iax.conf";
	int res = 0;
	int x;
	struct iax2_registry *reg;
	struct iax2_peer *peer;
	
	struct ast_netsock *ns;
	struct sockaddr_in sin;
	
	ast_custom_function_register(&iaxpeer_function);

	iax_set_output(iax_debug_output);
	iax_set_error(iax_error_output);
#ifdef NEWJB
	jb_setoutput(jb_error_output, jb_warning_output, NULL);
#endif
	
	sin.sin_family = AF_INET;
	sin.sin_port = htons(IAX_DEFAULT_PORTNO);
	sin.sin_addr.s_addr = INADDR_ANY;

#ifdef IAX_TRUNKING
#ifdef ZT_TIMERACK
	timingfd = open("/dev/zap/timer", O_RDWR);
	if (timingfd < 0)
#endif
		timingfd = open("/dev/zap/pseudo", O_RDWR);
	if (timingfd < 0) 
		ast_log(LOG_WARNING, "Unable to open IAX timing interface: %s\n", strerror(errno));
#endif		

	memset(iaxs, 0, sizeof(iaxs));

	for (x=0;x<IAX_MAX_CALLS;x++)
		ast_mutex_init(&iaxsl[x]);
	
	io = io_context_create();
	sched = sched_context_create();
	
	if (!io || !sched) {
		ast_log(LOG_ERROR, "Out of memory\n");
		return -1;
	}

	netsock = ast_netsock_list_alloc();
	if (!netsock) {
		ast_log(LOG_ERROR, "Could not allocate netsock list.\n");
		return -1;
	}
	ast_netsock_init(netsock);

	ast_mutex_init(&iaxq.lock);
	ast_mutex_init(&userl.lock);
	ast_mutex_init(&peerl.lock);
	ast_mutex_init(&waresl.lock);
	
	ast_cli_register_multiple(iax2_cli, sizeof(iax2_cli) / sizeof(iax2_cli[0]));

	ast_register_application(papp, iax2_prov_app, psyn, pdescrip);
	
	ast_manager_register( "IAXpeers", 0, manager_iax2_show_peers, "List IAX Peers" );
	ast_manager_register( "IAXnetstats", 0, manager_iax2_show_netstats, "Show IAX Netstats" );

	set_config(config, 0);

 	if (ast_channel_register(&iax2_tech)) {
		ast_log(LOG_ERROR, "Unable to register channel class %s\n", channeltype);
		__unload_module();
		return -1;
	}

	if (ast_register_switch(&iax2_switch)) 
		ast_log(LOG_ERROR, "Unable to register IAX switch\n");
	
	if (defaultsockfd < 0) {
		if (!(ns = ast_netsock_bindaddr(netsock, io, &sin, tos, socket_read, NULL))) {
			ast_log(LOG_ERROR, "Unable to create network socket: %s\n", strerror(errno));
			return -1;
		} else {
			if (option_verbose > 1)
				ast_verbose(VERBOSE_PREFIX_2 "Binding IAX2 to default address 0.0.0.0:%d\n", IAX_DEFAULT_PORTNO);
			defaultsockfd = ast_netsock_sockfd(ns);
			ast_netsock_unref(ns);
		}
	}
	
	res = start_network_thread();
	if (!res) {
		if (option_verbose > 1) 
			ast_verbose(VERBOSE_PREFIX_2 "IAX Ready and Listening\n");
	} else {
		ast_log(LOG_ERROR, "Unable to start network thread\n");
		ast_netsock_release(netsock);
	}

	for (reg = registrations; reg; reg = reg->next)
		iax2_do_register(reg);
	ast_mutex_lock(&peerl.lock);
	for (peer = peerl.peers; peer; peer = peer->next) {
		if (peer->sockfd < 0)
			peer->sockfd = defaultsockfd;
		iax2_poke_peer(peer, 0);
	}
	ast_mutex_unlock(&peerl.lock);
	reload_firmware();
	iax_provision_reload();
	return res;
}

char *description()
{
	return (char *) desc;
}

int usecount()
{
	return usecnt;
}

char *key()
{
	return ASTERISK_GPL_KEY;
}
