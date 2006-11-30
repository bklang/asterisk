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

/*** MODULEINFO
	<use>zaptel</use>
 ***/

#include "asterisk.h"

ASTERISK_FILE_VERSION(__FILE__, "$Revision$")

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
#include <strings.h>
#include <errno.h>
#include <unistd.h>
#include <netdb.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <regex.h>

#ifdef HAVE_ZAPTEL
#include <sys/ioctl.h>
#include <zaptel/zaptel.h>
#endif

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
#include "asterisk/stringfields.h"
#include "asterisk/linkedlists.h"

#include "iax2.h"
#include "iax2-parser.h"
#include "iax2-provision.h"
#include "jitterbuf.h"

/* Define SCHED_MULTITHREADED to run the scheduler in a special
   multithreaded mode. */
#define SCHED_MULTITHREADED

/* Define DEBUG_SCHED_MULTITHREADED to keep track of where each
   thread is actually doing. */
#define DEBUG_SCHED_MULTITHREAD

#ifndef IPTOS_MINCOST
#define IPTOS_MINCOST 0x02
#endif

#ifdef SO_NO_CHECK
static int nochecksums = 0;
#endif


#define PTR_TO_CALLNO(a) ((unsigned short)(unsigned long)(a))
#define CALLNO_TO_PTR(a) ((void *)(unsigned long)(a))

#define DEFAULT_THREAD_COUNT 10
#define DEFAULT_MAX_THREAD_COUNT 100
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

static const char tdesc[] = "Inter Asterisk eXchange Driver (Ver 2)";


/*! \brief Maximum transimission unit for the UDP packet in the trunk not to be
    fragmented. This is based on 1516 - ethernet - ip - udp - iax minus one g711 frame = 1240 */
#define MAX_TRUNK_MTU 1240 

static int global_max_trunk_mtu; 	/*!< Maximum MTU, 0 if not used */
static int trunk_timed, trunk_untimed, trunk_maxmtu, trunk_nmaxmtu ; 	/*!< Trunk MTU statistics */


static char context[80] = "default";

static char language[MAX_LANGUAGE] = "";
static char regcontext[AST_MAX_CONTEXT] = "";

static int maxauthreq = 3;
static int max_retries = 4;
static int ping_time = 20;
static int lagrq_time = 10;
static int maxtrunkcall = TRUNK_CALL_START;
static int maxnontrunkcall = 1;
static int maxjitterbuffer=1000;
static int resyncthreshold=1000;
static int maxjitterinterps=10;
static int trunkfreq = 20;
static int authdebug = 1;
static int autokill = 0;
static int iaxcompat = 0;

static int iaxdefaultdpcache=10 * 60;	/* Cache dialplan entries for 10 minutes by default */

static int iaxdefaulttimeout = 5;		/* Default to wait no more than 5 seconds for a reply to come back */

static unsigned int tos = 0;

static int min_reg_expire;
static int max_reg_expire;

static int timingfd = -1;				/* Timing file descriptor */

static struct ast_netsock_list *netsock;
static int defaultsockfd = -1;

int (*iax2_regfunk)(const char *username, int onoff) = NULL;

/* Ethernet, etc */
#define IAX_CAPABILITY_FULLBANDWIDTH 	0xFFFF
/* T1, maybe ISDN */
#define IAX_CAPABILITY_MEDBANDWIDTH 	(IAX_CAPABILITY_FULLBANDWIDTH & 	\
					 ~AST_FORMAT_SLINEAR &			\
					 ~AST_FORMAT_ULAW &			\
					 ~AST_FORMAT_ALAW &			\
					 ~AST_FORMAT_G722) 
/* A modem */
#define IAX_CAPABILITY_LOWBANDWIDTH	(IAX_CAPABILITY_MEDBANDWIDTH & 		\
					 ~AST_FORMAT_G726 &			\
					 ~AST_FORMAT_G726_AAL2 &		\
					 ~AST_FORMAT_ADPCM)

#define IAX_CAPABILITY_LOWFREE		(IAX_CAPABILITY_LOWBANDWIDTH & 		\
					 ~AST_FORMAT_G723_1)


#define DEFAULT_MAXMS		2000		/* Must be faster than 2 seconds by default */
#define DEFAULT_FREQ_OK		60 * 1000	/* How often to check for the host to be up */
#define DEFAULT_FREQ_NOTOK	10 * 1000	/* How often to check, if the host is down... */

static	struct io_context *io;
static	struct sched_context *sched;

static int iax2_capability = IAX_CAPABILITY_FULLBANDWIDTH;

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
static char mohinterpret[MAX_MUSICCLASS];
static char mohsuggest[MAX_MUSICCLASS];
static int amaflags = 0;
static int adsi = 0;
static int delayreject = 0;
static int iax2_encryption = 0;

static struct ast_flags globalflags = { 0 };

static pthread_t netthreadid = AST_PTHREADT_NULL;
static pthread_t schedthreadid = AST_PTHREADT_NULL;
AST_MUTEX_DEFINE_STATIC(sched_lock);
static ast_cond_t sched_cond;

enum {
	IAX_STATE_STARTED = 		(1 << 0),
	IAX_STATE_AUTHENTICATED = 	(1 << 1),
	IAX_STATE_TBD = 		(1 << 2),
	IAX_STATE_UNCHANGED = 		(1 << 3),
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
        /* (1 << 8) is currently unused due to the deprecation of an old option. Go ahead, take it! */
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
	IAX_TRUNKTIMESTAMPS =	(1 << 22),	/*!< Send trunk timestamps */
	IAX_TRANSFERMEDIA = 	(1 << 23),      /*!< When doing IAX2 transfers, transfer media only */
	IAX_MAXAUTHREQ =        (1 << 24),      /*!< Maximum outstanding AUTHREQ restriction is in place */
} iax2_flags;

static int global_rtautoclear = 120;

static int reload_config(void);
static int iax2_reload(int fd, int argc, char *argv[]);


struct iax2_user {
	AST_DECLARE_STRING_FIELDS(
		AST_STRING_FIELD(name);
		AST_STRING_FIELD(secret);
		AST_STRING_FIELD(dbsecret);
		AST_STRING_FIELD(accountcode);
		AST_STRING_FIELD(mohinterpret);
		AST_STRING_FIELD(mohsuggest);
		AST_STRING_FIELD(inkeys);               /*!< Key(s) this user can use to authenticate to us */
		AST_STRING_FIELD(language);
		AST_STRING_FIELD(cid_num);
		AST_STRING_FIELD(cid_name);
	);
	
	int authmethods;
	int encmethods;
	int amaflags;
	int adsi;
	unsigned int flags;
	int capability;
	int maxauthreq; /*!< Maximum allowed outstanding AUTHREQs */
	int curauthreq; /*!< Current number of outstanding AUTHREQs */
	struct ast_codec_pref prefs;
	struct ast_ha *ha;
	struct iax2_context *contexts;
	struct ast_variable *vars;
	AST_LIST_ENTRY(iax2_user) entry;
};

struct iax2_peer {
	AST_DECLARE_STRING_FIELDS(
		AST_STRING_FIELD(name);
		AST_STRING_FIELD(username);
		AST_STRING_FIELD(secret);
		AST_STRING_FIELD(dbsecret);
		AST_STRING_FIELD(outkey);	    /*!< What key we use to talk to this peer */

		AST_STRING_FIELD(regexten);     /*!< Extension to register (if regcontext is used) */
		AST_STRING_FIELD(context);      /*!< For transfers only */
		AST_STRING_FIELD(peercontext);  /*!< Context to pass to peer */
		AST_STRING_FIELD(mailbox);	    /*!< Mailbox */
		AST_STRING_FIELD(mohinterpret);
		AST_STRING_FIELD(mohsuggest);
		AST_STRING_FIELD(inkeys);		/*!< Key(s) this peer can use to authenticate to us */
		/* Suggested caller id if registering */
		AST_STRING_FIELD(cid_num);		/*!< Default context (for transfer really) */
		AST_STRING_FIELD(cid_name);		/*!< Default context (for transfer really) */
		AST_STRING_FIELD(zonetag);		/*!< Time Zone */
	);
	struct ast_codec_pref prefs;
	struct ast_dnsmgr_entry *dnsmgr;		/*!< DNS refresh manager */
	struct sockaddr_in addr;
	int formats;
	int sockfd;					/*!< Socket to use for transmission */
	struct in_addr mask;
	int adsi;
	unsigned int flags;

	/* Dynamic Registration fields */
	struct sockaddr_in defaddr;			/*!< Default address if there is one */
	int authmethods;				/*!< Authentication methods (IAX_AUTH_*) */
	int encmethods;					/*!< Encryption methods (IAX_ENCRYPT_*) */

	int expire;					/*!< Schedule entry for expiry */
	int expiry;					/*!< How soon to expire */
	int capability;					/*!< Capability */

	/* Qualification */
	int callno;					/*!< Call number of POKE request */
	int pokeexpire;					/*!< Scheduled qualification-related task (ie iax2_poke_peer_s or iax2_poke_noanswer) */
	int lastms;					/*!< How long last response took (in ms), or -1 for no response */
	int maxms;					/*!< Max ms we will accept for the host to be up, 0 to not monitor */

	int pokefreqok;					/*!< How often to check if the host is up */
	int pokefreqnotok;				/*!< How often to check when the host has been determined to be down */
	int historicms;					/*!< How long recent average responses took */
	int smoothing;					/*!< Sample over how many units to determine historic ms */
	
	struct ast_ha *ha;
	AST_LIST_ENTRY(iax2_peer) entry;
};

#define IAX2_TRUNK_PREFACE (sizeof(struct iax_frame) + sizeof(struct ast_iax2_meta_hdr) + sizeof(struct ast_iax2_meta_trunk_hdr))

struct iax2_trunk_peer {
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
	int trunkmaxmtu;
	int trunkerror;
	int calls;
	AST_LIST_ENTRY(iax2_trunk_peer) list;
};

static AST_LIST_HEAD_STATIC(tpeers, iax2_trunk_peer);

struct iax_firmware {
	AST_LIST_ENTRY(iax_firmware) list;
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
	TRANSFER_PASSTHROUGH,
	TRANSFER_MBEGIN,
	TRANSFER_MREADY,
	TRANSFER_MRELEASED,
	TRANSFER_MPASSTHROUGH,
	TRANSFER_MEDIA,
	TRANSFER_MEDIAPASS
};

struct iax2_registry {
	struct sockaddr_in addr;		/*!< Who we connect to for registration purposes */
	char username[80];
	char secret[80];			/*!< Password or key name in []'s */
	char random[80];
	int expire;				/*!< Sched ID of expiration */
	int refresh;				/*!< How often to refresh */
	enum iax_reg_state regstate;
	int messages;				/*!< Message count, low 8 bits = new, high 8 bits = old */
	int callno;				/*!< Associated call number if applicable */
	struct sockaddr_in us;			/*!< Who the server thinks we are */
	struct iax2_registry *next;
	struct ast_dnsmgr_entry *dnsmgr;	/*!< DNS refresh manager */
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

static int iaxthreadcount = DEFAULT_THREAD_COUNT;
static int iaxmaxthreadcount = DEFAULT_MAX_THREAD_COUNT;
static int iaxdynamicthreadcount = 0;

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
	/*! Actual used codec preferences */
	struct ast_codec_pref prefs;
	/*! Requested codec preferences */
	struct ast_codec_pref rprefs;
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
	/*! The jitterbuffer */
        jitterbuf *jb;
	/*! active jb read scheduler id */
        int jbid;                       
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

	AST_DECLARE_STRING_FIELDS(
		/*! Peer name */
		AST_STRING_FIELD(peer);
		/*! Default Context */
		AST_STRING_FIELD(context);
		/*! Caller ID if available */
		AST_STRING_FIELD(cid_num);
		AST_STRING_FIELD(cid_name);
		/*! Hidden Caller ID (i.e. ANI) if appropriate */
		AST_STRING_FIELD(ani);
		/*! DNID */
		AST_STRING_FIELD(dnid);
		/*! RDNIS */
		AST_STRING_FIELD(rdnis);
		/*! Requested Extension */
		AST_STRING_FIELD(exten);
		/*! Expected Username */
		AST_STRING_FIELD(username);
		/*! Expected Secret */
		AST_STRING_FIELD(secret);
		/*! MD5 challenge */
		AST_STRING_FIELD(challenge);
		/*! Public keys permitted keys for incoming authentication */
		AST_STRING_FIELD(inkeys);
		/*! Private key for outgoing authentication */
		AST_STRING_FIELD(outkey);
		/*! Preferred language */
		AST_STRING_FIELD(language);
		/*! Hostname/peername for naming purposes */
		AST_STRING_FIELD(host);

		AST_STRING_FIELD(dproot);
		AST_STRING_FIELD(accountcode);
		AST_STRING_FIELD(mohinterpret);
		AST_STRING_FIELD(mohsuggest);
	);
	
	/*! permitted authentication methods */
	int authmethods;
	/*! permitted encryption methods */
	int encmethods;
	/*! Encryption AES-128 Key */
	aes_encrypt_ctx ecx;
	/*! Decryption AES-128 Key */
	aes_decrypt_ctx dcx;
	/*! 32 bytes of semi-random data */
	unsigned char semirand[32];
	/*! Associated registry */
	struct iax2_registry *reg;
	/*! Associated peer for poking */
	struct iax2_peer *peerpoke;
	/*! IAX_ flags */
	unsigned int flags;
	int adsi;

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
	
	int pingid;			/*!< Transmit PING request */
	int lagid;			/*!< Retransmit lag request */
	int autoid;			/*!< Auto hangup for Dialplan requestor */
	int authid;			/*!< Authentication rejection ID */
	int authfail;			/*!< Reason to report failure */
	int initid;			/*!< Initial peer auto-congest ID (based on qualified peers) */
	int calling_ton;
	int calling_tns;
	int calling_pres;
	int amaflags;
	AST_LIST_HEAD_NOLOCK(, iax2_dpcache) dpentries;
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
	AST_LIST_HEAD(, iax_frame) queue;
	int count;
} iaxq = {
	.queue = AST_LIST_HEAD_INIT_VALUE
};

static AST_LIST_HEAD_STATIC(users, iax2_user);

static AST_LIST_HEAD_STATIC(peers, iax2_peer);

static AST_LIST_HEAD_STATIC(firmwares, iax_firmware);

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

struct iax2_dpcache {
	char peercontext[AST_MAX_CONTEXT];
	char exten[AST_MAX_EXTENSION];
	struct timeval orig;
	struct timeval expiry;
	int flags;
	unsigned short callno;
	int waiters[256];
	AST_LIST_ENTRY(iax2_dpcache) cache_list;
	AST_LIST_ENTRY(iax2_dpcache) peer_list;
};

static AST_LIST_HEAD_STATIC(dpcache, iax2_dpcache);

static void reg_source_db(struct iax2_peer *p);
static struct iax2_peer *realtime_peer(const char *peername, struct sockaddr_in *sin);

static void destroy_peer(struct iax2_peer *peer);
static int ast_cli_netstats(struct mansession *s, int fd, int limit_fmt);

#define IAX_IOSTATE_IDLE		0
#define IAX_IOSTATE_READY		1
#define IAX_IOSTATE_PROCESSING	2
#define IAX_IOSTATE_SCHEDREADY	3

#define IAX_TYPE_POOL    1
#define IAX_TYPE_DYNAMIC 2

struct iax2_thread {
	AST_LIST_ENTRY(iax2_thread) list;
	int type;
	int iostate;
#ifdef SCHED_MULTITHREADED
	void (*schedfunc)(void *);
	void *scheddata;
#endif
#ifdef DEBUG_SCHED_MULTITHREAD
	char curfunc[80];
#endif	
	int actions;
	pthread_t threadid;
	int threadnum;
	struct sockaddr_in iosin;
	unsigned char buf[4096]; 
	int iores;
	int iofd;
	time_t checktime;
	ast_mutex_t lock;
	ast_cond_t cond;
};

/* Thread lists */
static AST_LIST_HEAD_STATIC(idle_list, iax2_thread);
static AST_LIST_HEAD_STATIC(active_list, iax2_thread);
static AST_LIST_HEAD_STATIC(dynamic_list, iax2_thread);

static void *iax2_process_thread(void *data);

static void signal_condition(ast_mutex_t *lock, ast_cond_t *cond)
{
	ast_mutex_lock(lock);
	ast_cond_signal(cond);
	ast_mutex_unlock(lock);
}

static void iax_debug_output(const char *data)
{
	if (iaxdebug)
		ast_verbose("%s", data);
}

static void iax_error_output(const char *data)
{
	ast_log(LOG_WARNING, "%s", data);
}

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

/* XXX We probably should use a mutex when working with this XXX */
static struct chan_iax2_pvt *iaxs[IAX_MAX_CALLS];
static ast_mutex_t iaxsl[IAX_MAX_CALLS];
static struct timeval lastused[IAX_MAX_CALLS];

static enum ast_bridge_result iax2_bridge(struct ast_channel *c0, struct ast_channel *c1, int flags, struct ast_frame **fo, struct ast_channel **rc, int timeoutms);
static int expire_registry(void *data);
static int iax2_answer(struct ast_channel *c);
static int iax2_call(struct ast_channel *c, char *dest, int timeout);
static int iax2_devicestate(void *data);
static int iax2_digit_begin(struct ast_channel *c, char digit);
static int iax2_digit_end(struct ast_channel *c, char digit);
static int iax2_do_register(struct iax2_registry *reg);
static int iax2_fixup(struct ast_channel *oldchannel, struct ast_channel *newchan);
static int iax2_hangup(struct ast_channel *c);
static int iax2_indicate(struct ast_channel *c, int condition, const void *data, size_t datalen);
static int iax2_poke_peer(struct iax2_peer *peer, int heldcall);
static int iax2_provision(struct sockaddr_in *end, int sockfd, char *dest, const char *template, int force);
static int iax2_send(struct chan_iax2_pvt *pvt, struct ast_frame *f, unsigned int ts, int seqno, int now, int transfer, int final);
static int iax2_sendhtml(struct ast_channel *c, int subclass, const char *data, int datalen);
static int iax2_sendimage(struct ast_channel *c, struct ast_frame *img);
static int iax2_sendtext(struct ast_channel *c, const char *text);
static int iax2_setoption(struct ast_channel *c, int option, void *data, int datalen);
static int iax2_transfer(struct ast_channel *c, const char *dest);
static int iax2_write(struct ast_channel *c, struct ast_frame *f);
static int send_trunk(struct iax2_trunk_peer *tpeer, struct timeval *now);
static int send_command(struct chan_iax2_pvt *, char, int, unsigned int, const unsigned char *, int, int);
static int send_command_final(struct chan_iax2_pvt *, char, int, unsigned int, const unsigned char *, int, int);
static int send_command_immediate(struct chan_iax2_pvt *, char, int, unsigned int, const unsigned char *, int, int);
static int send_command_locked(unsigned short callno, char, int, unsigned int, const unsigned char *, int, int);
static int send_command_transfer(struct chan_iax2_pvt *, char, int, unsigned int, const unsigned char *, int);
static struct ast_channel *iax2_request(const char *type, int format, void *data, int *cause);
static struct ast_frame *iax2_read(struct ast_channel *c);
static struct iax2_peer *build_peer(const char *name, struct ast_variable *v, struct ast_variable *alt, int temponly);
static struct iax2_user *build_user(const char *name, struct ast_variable *v, struct ast_variable *alt, int temponly);
static void realtime_update_peer(const char *peername, struct sockaddr_in *sin, time_t regtime);
static void destroy_user(struct iax2_user *user);
static void prune_peers(void);

static const struct ast_channel_tech iax2_tech = {
	.type = "IAX2",
	.description = tdesc,
	.capabilities = IAX_CAPABILITY_FULLBANDWIDTH,
	.properties = AST_CHAN_TP_WANTSJITTER,
	.requester = iax2_request,
	.devicestate = iax2_devicestate,
	.send_digit_begin = iax2_digit_begin,
	.send_digit_end = iax2_digit_end,
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

static struct iax2_thread *find_idle_thread(void)
{
	struct iax2_thread *thread = NULL;

	/* Pop the head of the list off */
	AST_LIST_LOCK(&idle_list);
	thread = AST_LIST_REMOVE_HEAD(&idle_list, list);
	AST_LIST_UNLOCK(&idle_list);

	/* If no idle thread is available from the regular list, try dynamic */
	if (thread == NULL) {
		AST_LIST_LOCK(&dynamic_list);
		thread = AST_LIST_FIRST(&dynamic_list);
		if (thread != NULL) {
			AST_LIST_REMOVE(&dynamic_list, thread, list);
		}
		/* Make sure we absolutely have a thread... if not, try to make one if allowed */
		if (thread == NULL && iaxmaxthreadcount > iaxdynamicthreadcount) {
			/* We need to MAKE a thread! */
			thread = ast_calloc(1, sizeof(*thread));
			if (thread != NULL) {
				thread->threadnum = iaxdynamicthreadcount;
				thread->type = IAX_TYPE_DYNAMIC;
				ast_mutex_init(&thread->lock);
				ast_cond_init(&thread->cond, NULL);
				if (ast_pthread_create(&thread->threadid, NULL, iax2_process_thread, thread)) {
					free(thread);
					thread = NULL;
				} else {
					/* All went well and the thread is up, so increment our count */
					iaxdynamicthreadcount++;
				}
			}
		}
		AST_LIST_UNLOCK(&dynamic_list);
	}

	return thread;
}

#ifdef SCHED_MULTITHREADED
static int __schedule_action(void (*func)(void *data), void *data, const char *funcname)
{
	struct iax2_thread *thread = NULL;
	static time_t lasterror;
	static time_t t;

	thread = find_idle_thread();

	if (thread != NULL) {
		thread->schedfunc = func;
		thread->scheddata = data;
		thread->iostate = IAX_IOSTATE_SCHEDREADY;
#ifdef DEBUG_SCHED_MULTITHREAD
		ast_copy_string(thread->curfunc, funcname, sizeof(thread->curfunc));
#endif
		signal_condition(&thread->lock, &thread->cond);
		return 0;
	}
	time(&t);
	if (t != lasterror) 
		ast_log(LOG_NOTICE, "Out of idle IAX2 threads for scheduling!\n");
	lasterror = t;

	return -1;
}
#define schedule_action(func, data) __schedule_action(func, data, __PRETTY_FUNCTION__)
#endif

static void __send_ping(void *data)
{
	int callno = (long)data;
	send_command_locked(callno, AST_FRAME_IAX, IAX_COMMAND_PING, 0, NULL, 0, -1);
}

static int send_ping(void *data)
{
	int callno = (long)data;
	if (iaxs[callno]) {
#ifdef SCHED_MULTITHREADED
		if (schedule_action(__send_ping, data))
#endif		
			__send_ping(data);
		return 1;
	} else
		return 0;
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

static void __send_lagrq(void *data)
{
	int callno = (long)data;
	/* Ping only if it's real not if it's bridged */
	send_command_locked(callno, AST_FRAME_IAX, IAX_COMMAND_LAGRQ, 0, NULL, 0, -1);
}

static int send_lagrq(void *data)
{
	int callno = (long)data;
	if (iaxs[callno]) {
#ifdef SCHED_MULTITHREADED
		if (schedule_action(__send_lagrq, data))
#endif		
			__send_lagrq(data);
		return 1;
	} else
		return 0;
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
	struct iax2_peer *peer = NULL;

	/* Grab peer from linked list */
	AST_LIST_LOCK(&peers);
	AST_LIST_TRAVERSE(&peers, peer, entry) {
		if (!strcasecmp(peer->name, name)) {
			break;
		}
	}
	AST_LIST_UNLOCK(&peers);

	/* Now go for realtime if applicable */
	if(!peer && realtime)
		peer = realtime_peer(name, NULL);
	return peer;
}

static int iax2_getpeername(struct sockaddr_in sin, char *host, int len, int lockpeer)
{
	struct iax2_peer *peer = NULL;
	int res = 0;

	if (lockpeer)
		AST_LIST_LOCK(&peers);
	AST_LIST_TRAVERSE(&peers, peer, entry) {
		if ((peer->addr.sin_addr.s_addr == sin.sin_addr.s_addr) &&
		    (peer->addr.sin_port == sin.sin_port)) {
			ast_copy_string(host, peer->name, len);
			res = 1;
			break;
		}
	}
	if (lockpeer)
		AST_LIST_UNLOCK(&peers);
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
	jb_conf jbconf;

	if (!(tmp = ast_calloc(1, sizeof(*tmp))))
		return NULL;

	if (ast_string_field_init(tmp, 32)) {
		free(tmp);
		tmp = NULL;
		return NULL;
	}
		
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

	ast_string_field_set(tmp,exten, "s");
	ast_string_field_set(tmp,host, host);

	tmp->jb = jb_new();
	tmp->jbid = -1;
	jbconf.max_jitterbuf = maxjitterbuffer;
	jbconf.resync_threshold = resyncthreshold;
	jbconf.max_contig_interp = maxjitterinterps;
	jb_setconf(tmp->jb,&jbconf);

	AST_LIST_HEAD_INIT_NOLOCK(&tmp->dpentries);

	return tmp;
}

static struct iax_frame *iaxfrdup2(struct iax_frame *fr)
{
	struct iax_frame *new = iax_frame_new(DIRECTION_INGRESS, fr->af.datalen);
	if (new) {
		size_t mallocd_datalen = new->mallocd_datalen;
		memcpy(new, fr, sizeof(*new));
		iax_frame_wrap(new, &fr->af);
		new->mallocd_datalen = mallocd_datalen;
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
	if (option_debug)
		ast_log(LOG_DEBUG, "Made call %d into trunk call %d\n", callno, x);
	/* We move this call from a non-trunked to a trunked call */
	update_max_trunk();
	update_max_nontrunk();
	return res;
}

/*!
 * \todo XXX Note that this function contains a very expensive operation that
 * happens for *every* incoming media frame.  It iterates through every
 * possible call number, locking and unlocking each one, to try to match the
 * incoming frame to an active call.  Call numbers can be up to 2^15, 32768.
 * So, for an call with a local call number of 20000, every incoming audio
 * frame would require 20000 mutex lock and unlock operations.  Ouch.
 *
 * It's a shame that IAX2 media frames carry the source call number instead of
 * the destination call number.  If they did, this lookup wouldn't be needed.
 * However, it's too late to change that now.  Instead, we need to come up with
 * a better way of indexing active calls so that these frequent lookups are not
 * so expensive.
 */
static int find_callno(unsigned short callno, unsigned short dcallno, struct sockaddr_in *sin, int new, int lockpeer, int sockfd)
{
	int res = 0;
	int x;
	struct timeval now;
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
			snprintf(host, sizeof(host), "%s:%d", ast_inet_ntoa(sin->sin_addr), ntohs(sin->sin_port));
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
			ast_copy_flags(iaxs[x], (&globalflags), IAX_NOTRANSFER | IAX_TRANSFERMEDIA | IAX_USEJITTERBUF | IAX_FORCEJITTERBUF);
			
			ast_string_field_set(iaxs[x], accountcode, accountcode);
			ast_string_field_set(iaxs[x], mohinterpret, mohinterpret);
			ast_string_field_set(iaxs[x], mohsuggest, mohsuggest);
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
	struct iax_firmware *cur = NULL;
	int ifd, fd, res, len, chunk;
	struct ast_iax2_firmware_header *fwh, fwh2;
	struct MD5Context md5;
	unsigned char sum[16], buf[1024];
	char *s2, *last;

	if (!(s2 = alloca(strlen(s) + 100))) {
		ast_log(LOG_WARNING, "Alloca failed!\n");
		return -1;
	}

	last = strrchr(s, '/');
	if (last)
		last++;
	else
		last = s;

	snprintf(s2, strlen(s) + 100, "/var/tmp/%s-%ld", last, (unsigned long)ast_random());

	if ((res = stat(s, &stbuf) < 0)) {
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

	AST_LIST_TRAVERSE(&firmwares, cur, list) {
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
	}
	
	if (!cur && ((cur = ast_calloc(1, sizeof(*cur))))) {
		cur->fd = -1;
		AST_LIST_INSERT_TAIL(&firmwares, cur, list);
	}
	
	if (cur) {
		if (cur->fwh)
			munmap(cur->fwh, cur->mmaplen);
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
	struct iax_firmware *cur = NULL;

	if (ast_strlen_zero(dev))
		return 0;

	AST_LIST_LOCK(&firmwares);
	AST_LIST_TRAVERSE(&firmwares, cur, list) {
		if (!strcmp(dev, (char *)cur->fwh->devname)) {
			res = ntohs(cur->fwh->version);
			break;
		}
	}
	AST_LIST_UNLOCK(&firmwares);

	return res;
}

static int iax_firmware_append(struct iax_ie_data *ied, const unsigned char *dev, unsigned int desc)
{
	int res = -1;
	unsigned int bs = desc & 0xff;
	unsigned int start = (desc >> 8) & 0xffffff;
	unsigned int bytes;
	struct iax_firmware *cur;

	if (ast_strlen_zero((char *)dev) || !bs)
		return -1;

	start *= bs;
	
	AST_LIST_LOCK(&firmwares);
	AST_LIST_TRAVERSE(&firmwares, cur, list) {
		if (strcmp((char *)dev, (char *)cur->fwh->devname))
			continue;
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
	AST_LIST_UNLOCK(&firmwares);

	return res;
}


static void reload_firmware(void)
{
	struct iax_firmware *cur = NULL;
	DIR *fwd;
	struct dirent *de;
	char dir[256], fn[256];

	AST_LIST_LOCK(&firmwares);

	/* Mark all as dead */
	AST_LIST_TRAVERSE(&firmwares, cur, list)
		cur->dead = 1;

	/* Now that we have marked them dead... load new ones */
	snprintf(dir, sizeof(dir), "%s/firmware/iax", (char *)ast_config_AST_DATA_DIR);
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
	AST_LIST_TRAVERSE_SAFE_BEGIN(&firmwares, cur, list) {
		if (!cur->dead)
			continue;
		AST_LIST_REMOVE_CURRENT(&firmwares, list);
		destroy_firmware(cur);
	}
	AST_LIST_TRAVERSE_SAFE_END

	AST_LIST_UNLOCK(&firmwares);
}

static int __do_deliver(void *data)
{
	/* Just deliver the packet by using queueing.  This is called by
	  the IAX thread with the iaxsl lock held. */
	struct iax_frame *fr = data;
	fr->retrans = -1;
	fr->af.has_timing_info = 0;
	if (iaxs[fr->callno] && !ast_test_flag(iaxs[fr->callno], IAX_ALREADYGONE))
		iax2_queue_frame(fr->callno, &fr->af);
	/* Free our iax frame */
	iax2_frame_free(fr);
	/* And don't run again */
	return 0;
}

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
				ast_log(LOG_WARNING, "Receive error from %s\n", ast_inet_ntoa(sin->sin_addr));
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
	int callno = f->callno;

	/* Don't send if there was an error, but return error instead */
	if (!callno || !iaxs[callno] || iaxs[callno]->error)
	    return -1;
	
	/* Called with iaxsl held */
	if (option_debug > 2 && iaxdebug)
		ast_log(LOG_DEBUG, "Sending %d on %d/%d to %s:%d\n", f->ts, callno, iaxs[callno]->peercallno, ast_inet_ntoa(iaxs[callno]->addr.sin_addr), ntohs(iaxs[callno]->addr.sin_port));
	if (f->transfer) {
		if (iaxdebug)
			iax_showframe(f, NULL, 0, &iaxs[callno]->transfer, f->datalen - sizeof(struct ast_iax2_full_hdr));
		res = sendto(iaxs[callno]->sockfd, f->data, f->datalen, 0,(struct sockaddr *)&iaxs[callno]->transfer,
					sizeof(iaxs[callno]->transfer));
	} else {
		if (iaxdebug)
			iax_showframe(f, NULL, 0, &iaxs[callno]->addr, f->datalen - sizeof(struct ast_iax2_full_hdr));
		res = sendto(iaxs[callno]->sockfd, f->data, f->datalen, 0,(struct sockaddr *)&iaxs[callno]->addr,
					sizeof(iaxs[callno]->addr));
	}
	if (res < 0) {
		if (option_debug && iaxdebug)
			ast_log(LOG_DEBUG, "Received error: %s\n", strerror(errno));
		handle_error();
	} else
		res = 0;
	return res;
}

static void iax2_destroy_helper(struct chan_iax2_pvt *pvt)
{
	struct iax2_user *user = NULL;

	/* Decrement AUTHREQ count if needed */
	if (ast_test_flag(pvt, IAX_MAXAUTHREQ)) {
		AST_LIST_LOCK(&users);
		AST_LIST_TRAVERSE(&users, user, entry) {
			if (!strcmp(user->name, pvt->username)) {
				user->curauthreq--;
				break;
			}
		}
		AST_LIST_UNLOCK(&users);
		ast_clear_flag(pvt, IAX_MAXAUTHREQ);
	}
	/* No more pings or lagrq's */
	if (pvt->pingid > -1)
		ast_sched_del(sched, pvt->pingid);
	pvt->pingid = -1;
	if (pvt->lagid > -1)
		ast_sched_del(sched, pvt->lagid);
	pvt->lagid = -1;
	if (pvt->autoid > -1)
		ast_sched_del(sched, pvt->autoid);
	pvt->autoid = -1;
	if (pvt->authid > -1)
		ast_sched_del(sched, pvt->authid);
	pvt->authid = -1;
	if (pvt->initid > -1)
		ast_sched_del(sched, pvt->initid);
	pvt->initid = -1;
	if (pvt->jbid > -1)
		ast_sched_del(sched, pvt->jbid);
	pvt->jbid = -1;
}

static int iax2_predestroy(int callno)
{
	struct ast_channel *c = NULL;
	struct chan_iax2_pvt *pvt = iaxs[callno];

	if (!pvt)
		return -1;

	if (!ast_test_flag(pvt, IAX_ALREADYGONE)) {
		iax2_destroy_helper(pvt);
		ast_set_flag(pvt, IAX_ALREADYGONE);	
	}

	if ((c = pvt->owner)) {
		c->_softhangup |= AST_SOFTHANGUP_DEV;
		c->tech_pvt = NULL;
		ast_queue_hangup(c);
		pvt->owner = NULL;
	}

	return 0;
}

static void iax2_destroy(int callno)
{
	struct chan_iax2_pvt *pvt = NULL;
	struct iax_frame *cur = NULL;
	struct ast_channel *owner = NULL;

retry:
	pvt = iaxs[callno];
	gettimeofday(&lastused[callno], NULL);
	
	owner = pvt ? pvt->owner : NULL;

	if (owner) {
		if (ast_mutex_trylock(&owner->lock)) {
			ast_log(LOG_NOTICE, "Avoiding IAX destroy deadlock\n");
			ast_mutex_unlock(&iaxsl[callno]);
			usleep(1);
			ast_mutex_lock(&iaxsl[callno]);
			goto retry;
		}
	}
	if (!owner)
		iaxs[callno] = NULL;
	if (pvt) {
		if (!owner)
			pvt->owner = NULL;
		iax2_destroy_helper(pvt);

		/* Already gone */
		ast_set_flag(pvt, IAX_ALREADYGONE);	

		if (owner) {
			/* If there's an owner, prod it to give up */
			owner->_softhangup |= AST_SOFTHANGUP_DEV;
			ast_queue_hangup(owner);
		}

		AST_LIST_TRAVERSE(&iaxq.queue, cur, list) {
			/* Cancel any pending transmissions */
			if (cur->callno == pvt->callno) 
				cur->retries = -1;
		}
		if (pvt->reg)
			pvt->reg->callno = 0;
		if (!owner) {
			jb_frame frame;
			if (pvt->vars) {
			    ast_variables_destroy(pvt->vars);
			    pvt->vars = NULL;
			}

			while (jb_getall(pvt->jb, &frame) == JB_OK)
				iax2_frame_free(frame.data);
			jb_destroy(pvt->jb);
			/* gotta free up the stringfields */
			ast_string_field_free_pools(pvt);
			free(pvt);
		}
	}
	if (owner) {
		ast_mutex_unlock(&owner->lock);
	}
	if (callno & 0x4000)
		update_max_trunk();
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

static int attempt_transmit(void *data);
static void __attempt_transmit(void *data)
{
	/* Attempt to transmit the frame to the remote peer...
	   Called without iaxsl held. */
	struct iax_frame *f = data;
	int freeme=0;
	int callno = f->callno;
	/* Make sure this call is still active */
	if (callno) 
		ast_mutex_lock(&iaxsl[callno]);
	if (callno && iaxs[callno]) {
		if ((f->retries < 0) /* Already ACK'd */ ||
		    (f->retries >= max_retries) /* Too many attempts */) {
				/* Record an error if we've transmitted too many times */
				if (f->retries >= max_retries) {
					if (f->transfer) {
						/* Transfer timeout */
						send_command(iaxs[callno], AST_FRAME_IAX, IAX_COMMAND_TXREJ, 0, NULL, 0, -1);
					} else if (f->final) {
						if (f->final) 
							iax2_destroy(callno);
					} else {
						if (iaxs[callno]->owner)
							ast_log(LOG_WARNING, "Max retries exceeded to host %s on %s (type = %d, subclass = %d, ts=%d, seqno=%d)\n", ast_inet_ntoa(iaxs[f->callno]->addr.sin_addr),iaxs[f->callno]->owner->name , f->af.frametype, f->af.subclass, f->ts, f->oseqno);
						iaxs[callno]->error = ETIMEDOUT;
						if (iaxs[callno]->owner) {
							struct ast_frame fr = { 0, };
							/* Hangup the fd */
							fr.frametype = AST_FRAME_CONTROL;
							fr.subclass = AST_CONTROL_HANGUP;
							iax2_queue_frame(callno, &fr);
							/* Remember, owner could disappear */
							if (iaxs[callno]->owner)
								iaxs[callno]->owner->hangupcause = AST_CAUSE_DESTINATION_OUT_OF_ORDER;
						} else {
							if (iaxs[callno]->reg) {
								memset(&iaxs[callno]->reg->us, 0, sizeof(iaxs[callno]->reg->us));
								iaxs[callno]->reg->regstate = REG_STATE_TIMEOUT;
								iaxs[callno]->reg->refresh = IAX_DEFAULT_REG_EXPIRE;
							}
							iax2_destroy(callno);
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
		AST_LIST_LOCK(&iaxq.queue);
		AST_LIST_REMOVE(&iaxq.queue, f, list);
		iaxq.count--;
		AST_LIST_UNLOCK(&iaxq.queue);
		f->retrans = -1;
		/* Free the IAX frame */
		iax2_frame_free(f);
	}
}

static int attempt_transmit(void *data)
{
#ifdef SCHED_MULTITHREADED
	if (schedule_action(__attempt_transmit, data))
#endif		
		__attempt_transmit(data);
	return 0;
}

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
			expire_registry((void*)peer->name);
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
		ast_cli(fd, "  Addr->IP     : %s Port %d\n",  peer->addr.sin_addr.s_addr ? ast_inet_ntoa(peer->addr.sin_addr) : "(Unspecified)", ntohs(peer->addr.sin_port));
		ast_cli(fd, "  Defaddr->IP  : %s Port %d\n", ast_inet_ntoa(peer->defaddr.sin_addr), ntohs(peer->defaddr.sin_port));
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
		ast_cli(fd, "  Qualify      : every %dms when OK, every %dms when UNREACHABLE (sample smoothing %s)\n", peer->pokefreqok, peer->pokefreqnotok, peer->smoothing ? "On" : "Off");
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
	struct iax2_peer *p = NULL;
	char *res = NULL;
	int wordlen = strlen(word);

	/* 0 - iax2; 1 - show; 2 - peer; 3 - <peername> */
	if (pos == 3) {
		AST_LIST_LOCK(&peers);
		AST_LIST_TRAVERSE(&peers, p, entry) {
			if (!strncasecmp(p->name, word, wordlen) && ++which > state) {
				res = ast_strdup(p->name);
				break;
			}
		}
		AST_LIST_UNLOCK(&peers);
	}

	return res;
}

static int iax2_show_stats(int fd, int argc, char *argv[])
{
	struct iax_frame *cur;
	int cnt = 0, dead=0, final=0;
	if (argc != 3)
		return RESULT_SHOWUSAGE;
	AST_LIST_TRAVERSE(&iaxq.queue, cur, list) {
		if (cur->retries < 0)
			dead++;
		if (cur->final)
			final++;
		cnt++;
	}
	ast_cli(fd, "    IAX Statistics\n");
	ast_cli(fd, "---------------------\n");
	ast_cli(fd, "Outstanding frames: %d (%d ingress, %d egress)\n", iax_get_frames(), iax_get_iframes(), iax_get_oframes());
	ast_cli(fd, "%d timed and %d untimed transmits; MTU %d/%d/%d\n", trunk_timed, trunk_untimed,
		trunk_maxmtu, trunk_nmaxmtu, global_max_trunk_mtu);

	ast_cli(fd, "Packets in transmit queue: %d dead, %d final, %d total\n\n", dead, final, cnt);

	trunk_timed = trunk_untimed = 0;
	if (trunk_maxmtu > trunk_nmaxmtu)
		trunk_nmaxmtu = trunk_maxmtu;
	
	return RESULT_SUCCESS;
}

/*! \brief Set trunk MTU from CLI */
static int iax2_set_mtu(int fd, int argc, char *argv[])
{
	int mtuv;

	if (argc != 4)
		return RESULT_SHOWUSAGE; 
	if (strncasecmp(argv[3], "default", strlen(argv[3])) == 0) 
		mtuv = MAX_TRUNK_MTU; 
	else                                         
		mtuv = atoi(argv[3]); 

	if (mtuv == 0) {
		ast_cli(fd, "Trunk MTU control disabled (mtu was %d)\n", global_max_trunk_mtu); 
		global_max_trunk_mtu = 0; 
		return RESULT_SUCCESS; 
	}
	if (mtuv < 172 || mtuv > 4000) {
		ast_cli(fd, "Trunk MTU must be between 172 and 4000\n"); 
		return RESULT_SHOWUSAGE; 
	}
	ast_cli(fd, "Trunk MTU changed from %d to %d\n", global_max_trunk_mtu, mtuv); 
	global_max_trunk_mtu = mtuv; 
	return RESULT_SUCCESS;
}

static int iax2_show_cache(int fd, int argc, char *argv[])
{
	struct iax2_dpcache *dp = NULL;
	char tmp[1024], *pc = NULL;
	int s, x, y;
	struct timeval tv;

	gettimeofday(&tv, NULL);

	AST_LIST_LOCK(&dpcache);

	ast_cli(fd, "%-20.20s %-12.12s %-9.9s %-8.8s %s\n", "Peer/Context", "Exten", "Exp.", "Wait.", "Flags");

	AST_LIST_TRAVERSE(&dpcache, dp, cache_list) {
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
	}

	AST_LIST_LOCK(&dpcache);

	return RESULT_SUCCESS;
}

static unsigned int calc_rxstamp(struct chan_iax2_pvt *p, unsigned int offset);

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

static int get_from_jb(void *p);

static void update_jbsched(struct chan_iax2_pvt *pvt)
{
	int when;
	
	when = ast_tvdiff_ms(ast_tvnow(), pvt->rxcore);
	
	when = jb_next(pvt->jb) - when;
	
	if(pvt->jbid > -1) ast_sched_del(sched, pvt->jbid);
	
	if(when <= 0) {
		/* XXX should really just empty until when > 0.. */
		when = 1;
	}
	
	pvt->jbid = ast_sched_add(sched, when, get_from_jb, CALLNO_TO_PTR(pvt->callno));
	
	/* Signal scheduler thread */
	signal_condition(&sched_lock, &sched_cond);
}

static void __get_from_jb(void *p) 
{
	int callno = PTR_TO_CALLNO(p);
	struct chan_iax2_pvt *pvt = NULL;
	struct iax_frame *fr;
	jb_frame frame;
	int ret;
	long now;
	long next;
	struct timeval tv;
	
	/* Make sure we have a valid private structure before going on */
	ast_mutex_lock(&iaxsl[callno]);
	pvt = iaxs[callno];
	if (!pvt) {
		/* No go! */
		ast_mutex_unlock(&iaxsl[callno]);
		return;
	}

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
			fr = frame.data;
			__do_deliver(fr);
			break;
		case JB_INTERP:
		{
			struct ast_frame af;
			
			/* create an interpolation frame */
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
			if (!ast_test_flag(iaxs[callno], IAX_ALREADYGONE))
				iax2_queue_frame(callno, &af);
		}
		break;
		case JB_DROP:
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
	ast_mutex_unlock(&iaxsl[callno]);
}

static int get_from_jb(void *data)
{
#ifdef SCHED_MULTITHREADED
	if (schedule_action(__get_from_jb, data))
#endif		
		__get_from_jb(data);
	return 0;
}

static int schedule_delivery(struct iax_frame *fr, int updatehistory, int fromtrunk, unsigned int *tsout)
{
	int type, len;
	int ret;
	int needfree = 0;

	/* Attempt to recover wrapped timestamps */
	unwrap_timestamp(fr);
	

	/* delivery time is sender's sent timestamp converted back into absolute time according to our clock */
	if ( !fromtrunk && !ast_tvzero(iaxs[fr->callno]->rxcore))
		fr->af.delivery = ast_tvadd(iaxs[fr->callno]->rxcore, ast_samp2tv(fr->ts, 1000));
	else {
#if 0
		if (option_debug)
			ast_log(LOG_DEBUG, "schedule_delivery: set delivery to 0 as we don't have an rxcore yet, or frame is from trunk.\n");
#endif
		fr->af.delivery = ast_tv(0,0);
	}

	type = JB_TYPE_CONTROL;
	len = 0;

	if(fr->af.frametype == AST_FRAME_VOICE) {
		type = JB_TYPE_VOICE;
		len = ast_codec_get_samples(&fr->af) / 8;
	} else if(fr->af.frametype == AST_FRAME_CNG) {
		type = JB_TYPE_SILENCE;
	}

	if ( (!ast_test_flag(iaxs[fr->callno], IAX_USEJITTERBUF)) ) {
		if (tsout)
			*tsout = fr->ts;
		__do_deliver(fr);
		return -1;
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
		if (tsout)
			*tsout = fr->ts;
		__do_deliver(fr);
		return -1;
	}

	/* insert into jitterbuffer */
	/* TODO: Perhaps we could act immediately if it's not droppable and late */
	ret = jb_put(iaxs[fr->callno]->jb, fr, type, len, fr->ts,
			calc_rxstamp(iaxs[fr->callno],fr->ts));
	if (ret == JB_DROP) {
		needfree++;
	} else if (ret == JB_SCHED) {
		update_jbsched(iaxs[fr->callno]);
	}
	if (tsout)
		*tsout = fr->ts;
	if (needfree) {
		/* Free our iax frame */
		iax2_frame_free(fr);
		return -1;
	}
	return 0;
}

static int iax2_transmit(struct iax_frame *fr)
{
	/* Lock the queue and place this packet at the end */
	/* By setting this to 0, the network thread will send it for us, and
	   queue retransmission if necessary */
	fr->sentyet = 0;
	AST_LIST_LOCK(&iaxq.queue);
	AST_LIST_INSERT_TAIL(&iaxq.queue, fr, list);
	iaxq.count++;
	AST_LIST_UNLOCK(&iaxq.queue);
	/* Wake up the network and scheduler thread */
	pthread_kill(netthreadid, SIGURG);
	signal_condition(&sched_lock, &sched_cond);
	return 0;
}



static int iax2_digit_begin(struct ast_channel *c, char digit)
{
	return send_command_locked(PTR_TO_CALLNO(c->tech_pvt), AST_FRAME_DTMF_BEGIN, digit, 0, NULL, 0, -1);
}

static int iax2_digit_end(struct ast_channel *c, char digit)
{
	return send_command_locked(PTR_TO_CALLNO(c->tech_pvt), AST_FRAME_DTMF_END, digit, 0, NULL, 0, -1);
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

static struct iax2_peer *realtime_peer(const char *peername, struct sockaddr_in *sin)
{
	struct ast_variable *var;
	struct ast_variable *tmp;
	struct iax2_peer *peer=NULL;
	time_t regseconds = 0, nowtime;
	int dynamic=0;

	if (peername)
		var = ast_load_realtime("iaxpeers", "name", peername, NULL);
	else {
		char porta[25];
		sprintf(porta, "%d", ntohs(sin->sin_port));
		var = ast_load_realtime("iaxpeers", "ipaddr", ast_inet_ntoa(sin->sin_addr), "port", porta, NULL);
		if (var) {
			/* We'll need the peer name in order to build the structure! */
			for (tmp = var; tmp; tmp = tmp->next) {
				if (!strcasecmp(tmp->name, "name"))
					peername = tmp->value;
			}
		}
	}
	if (!var)
		return NULL;

	peer = build_peer(peername, var, NULL, ast_test_flag((&globalflags), IAX_RTCACHEFRIENDS) ? 0 : 1);
	
	if (!peer)
		return NULL;

	for (tmp = var; tmp; tmp = tmp->next) {
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
			ast_get_time_t(tmp->value, &regseconds, 0, NULL);
		} else if (!strcasecmp(tmp->name, "ipaddr")) {
			inet_aton(tmp->value, &(peer->addr.sin_addr));
		} else if (!strcasecmp(tmp->name, "port")) {
			peer->addr.sin_port = htons(atoi(tmp->value));
		} else if (!strcasecmp(tmp->name, "host")) {
			if (!strcasecmp(tmp->value, "dynamic"))
				dynamic = 1;
		}
	}
	if (!peer)
		return NULL;

	ast_variables_destroy(var);

	if (ast_test_flag((&globalflags), IAX_RTCACHEFRIENDS)) {
		ast_copy_flags(peer, &globalflags, IAX_RTAUTOCLEAR|IAX_RTCACHEFRIENDS);
		if (ast_test_flag(peer, IAX_RTAUTOCLEAR)) {
			if (peer->expire > -1)
				ast_sched_del(sched, peer->expire);
			peer->expire = ast_sched_add(sched, (global_rtautoclear) * 1000, expire_registry, (void*)peer->name);
		}
		AST_LIST_LOCK(&peers);
		AST_LIST_INSERT_HEAD(&peers, peer, entry);
		AST_LIST_UNLOCK(&peers);
		if (ast_test_flag(peer, IAX_DYNAMIC))
			reg_source_db(peer);
	} else {
		ast_set_flag(peer, IAX_TEMPONLY);	
	}

	if (!ast_test_flag(&globalflags, IAX_RTIGNOREREGEXPIRE) && dynamic) {
		time(&nowtime);
		if ((nowtime - regseconds) > IAX_DEFAULT_REG_EXPIRE) {
			memset(&peer->addr, 0, sizeof(peer->addr));
			realtime_update_peer(peer->name, &peer->addr, 0);
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

	user = build_user(username, var, NULL, !ast_test_flag((&globalflags), IAX_RTCACHEFRIENDS));
	if (!user)
		return NULL;

	ast_variables_destroy(var);

	if (ast_test_flag((&globalflags), IAX_RTCACHEFRIENDS)) {
		ast_set_flag(user, IAX_RTCACHEFRIENDS);
		AST_LIST_LOCK(&users);
		AST_LIST_INSERT_HEAD(&users, user, entry);
		AST_LIST_UNLOCK(&users);
	} else {
		ast_set_flag(user, IAX_TEMPONLY);	
	}

	return user;
}

static void realtime_update_peer(const char *peername, struct sockaddr_in *sin, time_t regtime)
{
	char port[10];
	char regseconds[20];
	
	snprintf(regseconds, sizeof(regseconds), "%d", (int)regtime);
	snprintf(port, sizeof(port), "%d", ntohs(sin->sin_port));
	ast_update_realtime("iaxpeers", "name", peername, 
		"ipaddr", ast_inet_ntoa(sin->sin_addr), "port", port, 
		"regseconds", regseconds, NULL);
}

struct create_addr_info {
	int capability;
	unsigned int flags;
	int maxtime;
	int encmethods;
	int found;
	int sockfd;
	int adsi;
	char username[80];
	char secret[80];
	char outkey[80];
	char timezone[80];
	char prefs[32];
	char context[AST_MAX_CONTEXT];
	char peercontext[AST_MAX_CONTEXT];
	char mohinterpret[MAX_MUSICCLASS];
	char mohsuggest[MAX_MUSICCLASS];
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

	ast_copy_flags(cai, peer, IAX_SENDANI | IAX_TRUNK | IAX_NOTRANSFER | IAX_TRANSFERMEDIA | IAX_USEJITTERBUF | IAX_FORCEJITTERBUF);
	cai->maxtime = peer->maxms;
	cai->capability = peer->capability;
	cai->encmethods = peer->encmethods;
	cai->sockfd = peer->sockfd;
	cai->adsi = peer->adsi;
	ast_codec_pref_convert(&peer->prefs, cai->prefs, sizeof(cai->prefs), 1);
	ast_copy_string(cai->context, peer->context, sizeof(cai->context));
	ast_copy_string(cai->peercontext, peer->peercontext, sizeof(cai->peercontext));
	ast_copy_string(cai->username, peer->username, sizeof(cai->username));
	ast_copy_string(cai->timezone, peer->zonetag, sizeof(cai->timezone));
	ast_copy_string(cai->outkey, peer->outkey, sizeof(cai->outkey));
	ast_copy_string(cai->mohinterpret, peer->mohinterpret, sizeof(cai->mohinterpret));
	ast_copy_string(cai->mohsuggest, peer->mohsuggest, sizeof(cai->mohsuggest));
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

static void __auto_congest(void *nothing)
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
}

static int auto_congest(void *data)
{
#ifdef SCHED_MULTITHREADED
	if (schedule_action(__auto_congest, data))
#endif		
		__auto_congest(data);
	return 0;
}

static unsigned int iax2_datetime(const char *tz)
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
	if (!ast_strlen_zero(c->cid.cid_rdnis))
		iax_ie_append_str(&ied, IAX_IE_RDNIS, c->cid.cid_rdnis);

	if (pds.context)
		iax_ie_append_str(&ied, IAX_IE_CALLED_CONTEXT, pds.context);

	if (pds.username)
		iax_ie_append_str(&ied, IAX_IE_USERNAME, pds.username);

	if (cai.encmethods)
		iax_ie_append_short(&ied, IAX_IE_ENCRYPTION, cai.encmethods);

	ast_mutex_lock(&iaxsl[callno]);

	if (!ast_strlen_zero(c->context))
		ast_string_field_set(iaxs[callno], context, c->context);

	if (pds.username)
		ast_string_field_set(iaxs[callno], username, pds.username);

	iaxs[callno]->encmethods = cai.encmethods;

	iaxs[callno]->adsi = cai.adsi;
	
	ast_string_field_set(iaxs[callno], mohinterpret, cai.mohinterpret);
	ast_string_field_set(iaxs[callno], mohsuggest, cai.mohsuggest);

	if (pds.key)
		ast_string_field_set(iaxs[callno], outkey, pds.key);
	if (pds.password)
		ast_string_field_set(iaxs[callno], secret, pds.password);

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

	/* send the command using the appropriate socket for this peer */
	iaxs[callno]->sockfd = cai.sockfd;

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
		if (option_debug)
			ast_log(LOG_DEBUG, "We're hanging up %s now...\n", c->name);
		alreadygone = ast_test_flag(iaxs[callno], IAX_ALREADYGONE);
		/* Send the hangup unless we have had a transmission error or are already gone */
 		iax_ie_append_byte(&ied, IAX_IE_CAUSECODE, (unsigned char)c->hangupcause);
		if (!iaxs[callno]->error && !alreadygone) 
 			send_command_final(iaxs[callno], AST_FRAME_IAX, IAX_COMMAND_HANGUP, 0, ied.buf, ied.pos, -1);
		/* Explicitly predestroy it */
		iax2_predestroy(callno);
		/* If we were already gone to begin with, destroy us now */
		if (alreadygone) {
			if (option_debug)
				ast_log(LOG_DEBUG, "Really destroying %s now...\n", c->name);
			iax2_destroy(callno);
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
	ast_log(LOG_NOTICE, "I should never be called!\n");
	return &ast_null_frame;
}

static int iax2_start_transfer(unsigned short callno0, unsigned short callno1, int mediaonly)
{
	int res;
	struct iax_ie_data ied0;
	struct iax_ie_data ied1;
	unsigned int transferid = (unsigned int)ast_random();
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
	iaxs[callno0]->transferring = mediaonly ? TRANSFER_MBEGIN : TRANSFER_BEGIN;
	iaxs[callno1]->transferring = mediaonly ? TRANSFER_MBEGIN : TRANSFER_BEGIN;
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
	struct ast_channel *who, *other;
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
		if ((c0->tech != &iax2_tech) || (c1->tech != &iax2_tech)) {
			if (option_verbose > 2)
				ast_verbose(VERBOSE_PREFIX_3 "Can't masquerade, we're different...\n");
			/* Remove from native mode */
			if (c0->tech == &iax2_tech) {
				ast_mutex_lock(&iaxsl[callno0]);
				iaxs[callno0]->bridgecallno = 0;
				ast_mutex_unlock(&iaxsl[callno0]);
			}
			if (c1->tech == &iax2_tech) {
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
		if (!transferstarted && !ast_test_flag(iaxs[callno0], IAX_NOTRANSFER) && !ast_test_flag(iaxs[callno1], IAX_NOTRANSFER)) {
			/* Try the transfer */
			if (iax2_start_transfer(callno0, callno1, (flags & (AST_BRIDGE_DTMF_CHANNEL_0 | AST_BRIDGE_DTMF_CHANNEL_1)) ||
							ast_test_flag(iaxs[callno0], IAX_TRANSFERMEDIA) | ast_test_flag(iaxs[callno1], IAX_TRANSFERMEDIA)))
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
		other = (who == c0) ? c1 : c0;  /* the 'other' channel */
		if ((f->frametype == AST_FRAME_VOICE) ||
		    (f->frametype == AST_FRAME_TEXT) ||
		    (f->frametype == AST_FRAME_VIDEO) || 
		    (f->frametype == AST_FRAME_IMAGE) ||
		    (f->frametype == AST_FRAME_DTMF)) {
			/* monitored dtmf take out of the bridge.
			 * check if we monitor the specific source.
			 */
			int monitored_source = (who == c0) ? AST_BRIDGE_DTMF_CHANNEL_0 : AST_BRIDGE_DTMF_CHANNEL_1;
			if (f->frametype == AST_FRAME_DTMF && (flags & monitored_source)) {
				*rc = who;
				*fo = f;
				res = AST_BRIDGE_COMPLETE;
				/* Remove from native mode */
				break;
			}
			/* everything else goes to the other side */
			ast_write(other, f);
		}
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

static int iax2_indicate(struct ast_channel *c, int condition, const void *data, size_t datalen)
{
	unsigned short callno = PTR_TO_CALLNO(c->tech_pvt);

	if (option_debug && iaxdebug)
		ast_log(LOG_DEBUG, "Indicating condition %d\n", condition);

	if (!strcasecmp(iaxs[callno]->mohinterpret, "passthrough"))
		return send_command_locked(callno, AST_FRAME_CONTROL, condition, 0, data, datalen, -1);

	switch (condition) {
	case AST_CONTROL_HOLD:
		ast_moh_start(c, data, iaxs[callno]->mohinterpret);
		return 0;
	case AST_CONTROL_UNHOLD:
		ast_moh_stop(c);
		return 0;
	default:
		return send_command_locked(callno, AST_FRAME_CONTROL, condition, 0, data, datalen, -1);
	}
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
	
static int iax2_getpeertrunk(struct sockaddr_in sin)
{
	struct iax2_peer *peer = NULL;
	int res = 0;

	AST_LIST_LOCK(&peers);
	AST_LIST_TRAVERSE(&peers, peer, entry) {
		if ((peer->addr.sin_addr.s_addr == sin.sin_addr.s_addr) &&
		    (peer->addr.sin_port == sin.sin_port)) {
			res = ast_test_flag(peer, IAX_TRUNK);
			break;
		}
	}
	AST_LIST_UNLOCK(&peers);

	return res;
}

/*! \brief  Create new call, interface with the PBX core */
static struct ast_channel *ast_iax2_new(int callno, int state, int capability)
{
	struct ast_channel *tmp;
	struct chan_iax2_pvt *i;
	struct ast_variable *v = NULL;

	if (!(i = iaxs[callno])) {
		ast_log(LOG_WARNING, "No IAX2 pvt found for callno '%d' !\n", callno);
		return NULL;
	}

	/* Don't hold call lock */
	ast_mutex_unlock(&iaxsl[callno]);
	tmp = ast_channel_alloc(1, state, i->cid_num, i->cid_name, "IAX2/%s-%d", i->host, i->callno);
	ast_mutex_lock(&iaxsl[callno]);
	if (!tmp)
		return NULL;
	tmp->tech = &iax2_tech;
	/* We can support any format by default, until we get restricted */
	tmp->nativeformats = capability;
	tmp->readformat = ast_best_codec(capability);
	tmp->writeformat = ast_best_codec(capability);
	tmp->tech_pvt = CALLNO_TO_PTR(i->callno);

	/* Don't use ast_set_callerid() here because it will
	 * generate a NewCallerID event before the NewChannel event */
	tmp->cid.cid_num = ast_strdup(i->cid_num);
	tmp->cid.cid_name = ast_strdup(i->cid_name);
	if (!ast_strlen_zero(i->ani))
		tmp->cid.cid_ani = ast_strdup(i->ani);
	else
		tmp->cid.cid_ani = ast_strdup(i->cid_num);
	tmp->cid.cid_dnid = ast_strdup(i->dnid);
	tmp->cid.cid_rdnis = ast_strdup(i->rdnis);
	tmp->cid.cid_pres = i->calling_pres;
	tmp->cid.cid_ton = i->calling_ton;
	tmp->cid.cid_tns = i->calling_tns;
	if (!ast_strlen_zero(i->language))
		ast_string_field_set(tmp, language, i->language);
	if (!ast_strlen_zero(i->accountcode))
		ast_string_field_set(tmp, accountcode, i->accountcode);
	if (i->amaflags)
		tmp->amaflags = i->amaflags;
	ast_copy_string(tmp->context, i->context, sizeof(tmp->context));
	ast_copy_string(tmp->exten, i->exten, sizeof(tmp->exten));
	if (i->adsi)
		tmp->adsicpe = i->peeradsicpe;
	else
		tmp->adsicpe = AST_ADSI_UNAVAILABLE;
	i->owner = tmp;
	i->capability = capability;
	if (state != AST_STATE_DOWN) {
		if (ast_pbx_start(tmp)) {
			ast_log(LOG_WARNING, "Unable to start PBX on %s\n", tmp->name);
			ast_hangup(tmp);
			i->owner = NULL;
			return NULL;
		}
	}

	for (v = i->vars ; v ; v = v->next)
		pbx_builtin_setvar_helper(tmp, v->name, v->value);

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
	return ms;
}

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
		if (!test_jitpct || ((100.0 * ast_random() / (RAND_MAX + 1.0)) < test_jitpct)) {
			jit = (int)((float)test_jit * ast_random() / (RAND_MAX + 1.0));
			if ((int)(2.0 * ast_random() / (RAND_MAX + 1.0)))
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
	struct iax2_trunk_peer *tpeer = NULL;
	
	/* Finds and locks trunk peer */
	AST_LIST_LOCK(&tpeers);

	AST_LIST_TRAVERSE(&tpeers, tpeer, list) {
		if (!inaddrcmp(&tpeer->addr, sin)) {
			ast_mutex_lock(&tpeer->lock);
			break;
		}
	}

	if (!tpeer) {
		if ((tpeer = ast_calloc(1, sizeof(*tpeer)))) {
			ast_mutex_init(&tpeer->lock);
			tpeer->lastsent = 9999;
			memcpy(&tpeer->addr, sin, sizeof(tpeer->addr));
			tpeer->trunkact = ast_tvnow();
			ast_mutex_lock(&tpeer->lock);
			tpeer->sockfd = fd;
#ifdef SO_NO_CHECK
			setsockopt(tpeer->sockfd, SOL_SOCKET, SO_NO_CHECK, &nochecksums, sizeof(nochecksums));
#endif
			if (option_debug)
				ast_log(LOG_DEBUG, "Created trunk peer for '%s:%d'\n", ast_inet_ntoa(tpeer->addr.sin_addr), ntohs(tpeer->addr.sin_port));
			AST_LIST_INSERT_TAIL(&tpeers, tpeer, list);
		}
	}

	AST_LIST_UNLOCK(&tpeers);

	return tpeer;
}

static int iax2_trunk_queue(struct chan_iax2_pvt *pvt, struct iax_frame *fr)
{
	struct ast_frame *f;
	struct iax2_trunk_peer *tpeer;
	void *tmp, *ptr;
	struct timeval now;
	int res; 
	struct ast_iax2_meta_trunk_entry *met;
	struct ast_iax2_meta_trunk_mini *mtm;

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
				if (option_debug)
					ast_log(LOG_DEBUG, "Expanded trunk '%s:%d' to %d bytes\n", ast_inet_ntoa(tpeer->addr.sin_addr), ntohs(tpeer->addr.sin_port), tpeer->trunkdataalloc);
			} else {
				ast_log(LOG_WARNING, "Maximum trunk data space exceeded to %s:%d\n", ast_inet_ntoa(tpeer->addr.sin_addr), ntohs(tpeer->addr.sin_port));
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

		/* track the largest mtu we actually have sent */
		if (tpeer->trunkdatalen + f->datalen + 4 > trunk_maxmtu) 
			trunk_maxmtu = tpeer->trunkdatalen + f->datalen + 4 ; 

		/* if we have enough for a full MTU, ship it now without waiting */
		if (global_max_trunk_mtu > 0 && tpeer->trunkdatalen + f->datalen + 4 >= global_max_trunk_mtu) {
			gettimeofday(&now, NULL);
			res = send_trunk(tpeer, &now); 
			trunk_untimed ++; 
		}

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
		
		tmppw = ast_strdupa(iaxs[callno]->secret);
		stringp = tmppw;
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


	if ((ast_test_flag(pvt, IAX_TRUNK) || 
			(((fts & 0xFFFF0000L) == (lastsent & 0xFFFF0000L)) ||
			((fts & 0xFFFF0000L) == ((lastsent + 0x10000) & 0xFFFF0000L))))
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
			if (pvt->transferring == TRANSFER_MEDIAPASS)
				fr->transfer = 1;
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

	struct iax2_user *user = NULL;
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

	ast_cli(fd, FORMAT, "Username", "Secret", "Authen", "Def.Context", "A/C","Codec Pref");
	AST_LIST_LOCK(&users);
	AST_LIST_TRAVERSE(&users, user, entry) {
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
	AST_LIST_UNLOCK(&users);

	if (havepattern)
		regfree(&regexbuf);

	return RESULT_SUCCESS;
#undef FORMAT
#undef FORMAT2
}

static int __iax2_show_peers(int manager, int fd, struct mansession *s, int argc, char *argv[])
{
	regex_t regexbuf;
	int havepattern = 0;
	int total_peers = 0;
	int online_peers = 0;
	int offline_peers = 0;
	int unmonitored_peers = 0;

#define FORMAT2 "%-15.15s  %-15.15s %s  %-15.15s  %-8s  %s %-10s%s"
#define FORMAT "%-15.15s  %-15.15s %s  %-15.15s  %-5d%s  %s %-10s%s"

	struct iax2_peer *peer = NULL;
	char name[256];
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


	if (s)
		astman_append(s, FORMAT2, "Name/Username", "Host", "   ", "Mask", "Port", "   ", "Status", term);
	else
		ast_cli(fd, FORMAT2, "Name/Username", "Host", "   ", "Mask", "Port", "   ", "Status", term);

	AST_LIST_LOCK(&peers);
	AST_LIST_TRAVERSE(&peers, peer, entry) {
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
		
		ast_copy_string(nm, ast_inet_ntoa(peer->mask), sizeof(nm));
		
		snprintf(srch, sizeof(srch), FORMAT, name, 
			 peer->addr.sin_addr.s_addr ? ast_inet_ntoa(peer->addr.sin_addr) : "(Unspecified)",
			 ast_test_flag(peer, IAX_DYNAMIC) ? "(D)" : "(S)",
			 nm,
			 ntohs(peer->addr.sin_port), ast_test_flag(peer, IAX_TRUNK) ? "(T)" : "   ",
			 peer->encmethods ? "(E)" : "   ", status, term);
		
		if (s)
			astman_append(s, FORMAT, name, 
				      peer->addr.sin_addr.s_addr ? ast_inet_ntoa( peer->addr.sin_addr) : "(Unspecified)",
				      ast_test_flag(peer, IAX_DYNAMIC) ? "(D)" : "(S)",
				      nm,
				      ntohs(peer->addr.sin_port), ast_test_flag(peer, IAX_TRUNK) ? "(T)" : "   ",
				      peer->encmethods ? "(E)" : "   ", status, term);
		else
			ast_cli(fd, FORMAT, name, 
				peer->addr.sin_addr.s_addr ? ast_inet_ntoa(peer->addr.sin_addr) : "(Unspecified)",
				ast_test_flag(peer, IAX_DYNAMIC) ? "(D)" : "(S)",
				nm,
				ntohs(peer->addr.sin_port), ast_test_flag(peer, IAX_TRUNK) ? "(T)" : "   ",
				peer->encmethods ? "(E)" : "   ", status, term);
		total_peers++;
	}
	AST_LIST_UNLOCK(&peers);

	if (s)
		astman_append(s,"%d iax2 peers [%d online, %d offline, %d unmonitored]%s", total_peers, online_peers, offline_peers, unmonitored_peers, term);
	else
		ast_cli(fd,"%d iax2 peers [%d online, %d offline, %d unmonitored]%s", total_peers, online_peers, offline_peers, unmonitored_peers, term);

	if (havepattern)
		regfree(&regexbuf);

	return RESULT_SUCCESS;
#undef FORMAT
#undef FORMAT2
}

static int iax2_show_threads(int fd, int argc, char *argv[])
{
	struct iax2_thread *thread = NULL;
	time_t t;
	int threadcount = 0, dynamiccount = 0;
	char type;

	if (argc != 3)
		return RESULT_SHOWUSAGE;
		
	ast_cli(fd, "IAX2 Thread Information\n");
	time(&t);
	ast_cli(fd, "Idle Threads:\n");
	AST_LIST_LOCK(&idle_list);
	AST_LIST_TRAVERSE(&idle_list, thread, list) {
#ifdef DEBUG_SCHED_MULTITHREAD
		ast_cli(fd, "Thread %d: state=%d, update=%d, actions=%d, func ='%s'\n", 
			thread->threadnum, thread->iostate, (int)(t - thread->checktime), thread->actions, thread->curfunc);
#else
		ast_cli(fd, "Thread %d: state=%d, update=%d, actions=%d\n", 
			thread->threadnum, thread->iostate, (int)(t - thread->checktime), thread->actions);
#endif
		threadcount++;
	}
	AST_LIST_UNLOCK(&idle_list);
	ast_cli(fd, "Active Threads:\n");
	AST_LIST_LOCK(&active_list);
	AST_LIST_TRAVERSE(&active_list, thread, list) {
		if (thread->type == IAX_TYPE_DYNAMIC)
			type = 'D';
		else
			type = 'P';
#ifdef DEBUG_SCHED_MULTITHREAD
		ast_cli(fd, "Thread %c%d: state=%d, update=%d, actions=%d, func ='%s'\n", 
			type, thread->threadnum, thread->iostate, (int)(t - thread->checktime), thread->actions, thread->curfunc);
#else
		ast_cli(fd, "Thread %c%d: state=%d, update=%d, actions=%d\n", 
			type, thread->threadnum, thread->iostate, (int)(t - thread->checktime), thread->actions);
#endif
		threadcount++;
	}
	AST_LIST_UNLOCK(&active_list);
	ast_cli(fd, "Dynamic Threads:\n");
        AST_LIST_LOCK(&dynamic_list);
        AST_LIST_TRAVERSE(&dynamic_list, thread, list) {
#ifdef DEBUG_SCHED_MULTITHREAD
                ast_cli(fd, "Thread %d: state=%d, update=%d, actions=%d, func ='%s'\n",
                        thread->threadnum, thread->iostate, (int)(t - thread->checktime), thread->actions, thread->curfunc);
#else
                ast_cli(fd, "Thread %d: state=%d, update=%d, actions=%d\n",
                        thread->threadnum, thread->iostate, (int)(t - thread->checktime), thread->actions);
#endif
		dynamiccount++;
        }
        AST_LIST_UNLOCK(&dynamic_list);
	ast_cli(fd, "%d of %d threads accounted for with %d dynamic threads\n", threadcount, iaxthreadcount, dynamiccount);
	return RESULT_SUCCESS;
}

static int iax2_show_peers(int fd, int argc, char *argv[])
{
	return __iax2_show_peers(0, fd, NULL, argc, argv);
}
static int manager_iax2_show_netstats( struct mansession *s, struct message *m )
{
	ast_cli_netstats(s, -1, 0);
	astman_append(s, "\r\n");
	return RESULT_SUCCESS;
}

static int iax2_show_firmware(int fd, int argc, char *argv[])
{
	struct iax_firmware *cur = NULL;

	if ((argc != 3) && (argc != 4))
		return RESULT_SHOWUSAGE;

	ast_cli(fd, "%-15.15s  %-15.15s %-15.15s\n", "Device", "Version", "Size");
	AST_LIST_LOCK(&firmwares);
	AST_LIST_TRAVERSE(&firmwares, cur, list) {
		if ((argc == 3) || (!strcasecmp(argv[3], (char *)cur->fwh->devname)))  {
			ast_cli(fd, "%-15.15s  %-15d %-15d\n", cur->fwh->devname, 
				ntohs(cur->fwh->version), (int)ntohl(cur->fwh->datalen));
		}
	}
	AST_LIST_UNLOCK(&firmwares);

	return RESULT_SUCCESS;
}

/* JDG: callback to display iax peers in manager */
static int manager_iax2_show_peers( struct mansession *s, struct message *m )
{
	char *a[] = { "iax2", "show", "users" };
	int ret;
	char *id;
	id = astman_get_header(m,"ActionID");
	if (!ast_strlen_zero(id))
		astman_append(s, "ActionID: %s\r\n",id);
	ret = __iax2_show_peers(1, -1, s, 3, a );
	astman_append(s, "\r\n\r\n" );
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
#define FORMAT2 "%-20.20s  %-6.6s  %-10.10s  %-20.20s %8.8s  %s\n"
#define FORMAT  "%-20.20s  %-6.6s  %-10.10s  %-20.20s %8d  %s\n"
	struct iax2_registry *reg = NULL;

	char host[80];
	char perceived[80];
	if (argc != 3)
		return RESULT_SHOWUSAGE;
	AST_LIST_LOCK(&peers);
	ast_cli(fd, FORMAT2, "Host", "dnsmgr", "Username", "Perceived", "Refresh", "State");
	for (reg = registrations;reg;reg = reg->next) {
		snprintf(host, sizeof(host), "%s:%d", ast_inet_ntoa(reg->addr.sin_addr), ntohs(reg->addr.sin_port));
		if (reg->us.sin_addr.s_addr) 
			snprintf(perceived, sizeof(perceived), "%s:%d", ast_inet_ntoa(reg->us.sin_addr), ntohs(reg->us.sin_port));
		else
			ast_copy_string(perceived, "<Unregistered>", sizeof(perceived));
		ast_cli(fd, FORMAT, host, 
					(reg->dnsmgr) ? "Y" : "N", 
					reg->username, perceived, reg->refresh, regstate2str(reg->regstate));
	}
	AST_LIST_UNLOCK(&peers);
	return RESULT_SUCCESS;
#undef FORMAT
#undef FORMAT2
}

static int iax2_show_channels(int fd, int argc, char *argv[])
{
#define FORMAT2 "%-20.20s  %-15.15s  %-10.10s  %-11.11s  %-11.11s  %-7.7s  %-6.6s  %-6.6s  %s\n"
#define FORMAT  "%-20.20s  %-15.15s  %-10.10s  %5.5d/%5.5d  %5.5d/%5.5d  %-5.5dms  %-4.4dms  %-4.4dms  %-6.6s\n"
#define FORMATB "%-20.20s  %-15.15s  %-10.10s  %5.5d/%5.5d  %5.5d/%5.5d  [Native Bridged to ID=%5.5d]\n"
	int x;
	int numchans = 0;

	if (argc != 3)
		return RESULT_SHOWUSAGE;
	ast_cli(fd, FORMAT2, "Channel", "Peer", "Username", "ID (Lo/Rem)", "Seq (Tx/Rx)", "Lag", "Jitter", "JitBuf", "Format");
	for (x=0;x<IAX_MAX_CALLS;x++) {
		ast_mutex_lock(&iaxsl[x]);
		if (iaxs[x]) {
			int lag, jitter, localdelay;
			jb_info jbinfo;
			
			if(ast_test_flag(iaxs[x], IAX_USEJITTERBUF)) {
				jb_getinfo(iaxs[x]->jb, &jbinfo);
				jitter = jbinfo.jitter;
				localdelay = jbinfo.current - jbinfo.min;
			} else {
				jitter = -1;
				localdelay = 0;
			}
			lag = iaxs[x]->remote_rr.delay;
			ast_cli(fd, FORMAT,
				iaxs[x]->owner ? iaxs[x]->owner->name : "(None)",
				ast_inet_ntoa(iaxs[x]->addr.sin_addr), 
				S_OR(iaxs[x]->username, "(None)"),
				iaxs[x]->callno, iaxs[x]->peercallno,
				iaxs[x]->oseqno, iaxs[x]->iseqno,
				lag,
				jitter,
				localdelay,
				ast_getformatname(iaxs[x]->voiceformat) );
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

static int ast_cli_netstats(struct mansession *s, int fd, int limit_fmt)
{
	int x;
	int numchans = 0;
	for (x=0;x<IAX_MAX_CALLS;x++) {
		ast_mutex_lock(&iaxsl[x]);
		if (iaxs[x]) {
			int localjitter, localdelay, locallost, locallosspct, localdropped, localooo;
			char *fmt;
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
			if (limit_fmt)
				fmt = "%-25.25s %4d %4d %4d %5d %3d %5d %4d %6d %4d %4d %5d %3d %5d %4d %6d\n";
			else
				fmt = "%s %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d\n";
			if (s)
				
				astman_append(s, fmt,
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
					      iaxs[x]->remote_rr.packets/1000);
			else
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
	numchans = ast_cli_netstats(NULL, fd, 1);
	ast_cli(fd, "%d active IAX channel%s\n", numchans, (numchans != 1) ? "s" : "");
	return RESULT_SUCCESS;
}

static int iax2_do_debug(int fd, int argc, char *argv[])
{
	if (argc < 2 || argc > 3)
		return RESULT_SHOWUSAGE;
	iaxdebug = 1;
	ast_cli(fd, "IAX2 Debugging Enabled\n");
	return RESULT_SUCCESS;
}

static int iax2_do_trunk_debug(int fd, int argc, char *argv[])
{
	if (argc < 3 || argc > 4)
		return RESULT_SHOWUSAGE;
	iaxtrunkdebug = 1;
	ast_cli(fd, "IAX2 Trunk Debug Requested\n");
	return RESULT_SUCCESS;
}

static int iax2_do_jb_debug(int fd, int argc, char *argv[])
{
	if (argc < 3 || argc > 4)
		return RESULT_SHOWUSAGE;
	jb_setoutput(jb_error_output, jb_warning_output, jb_debug_output);
	ast_cli(fd, "IAX2 Jitterbuffer Debugging Enabled\n");
	return RESULT_SUCCESS;
}

static int iax2_no_debug(int fd, int argc, char *argv[])
{
	if (argc < 3 || argc > 4)
		return RESULT_SHOWUSAGE;
	iaxdebug = 0;
	ast_cli(fd, "IAX2 Debugging Disabled\n");
	return RESULT_SUCCESS;
}

static int iax2_no_trunk_debug(int fd, int argc, char *argv[])
{
	if (argc < 4 || argc > 5)
		return RESULT_SHOWUSAGE;
	iaxtrunkdebug = 0;
	ast_cli(fd, "IAX2 Trunk Debugging Disabled\n");
	return RESULT_SUCCESS;
}

static int iax2_no_jb_debug(int fd, int argc, char *argv[])
{
	if (argc < 4 || argc > 5)
		return RESULT_SHOWUSAGE;
	iaxtrunkdebug = 0;
	ast_cli(fd, "IAX2 Trunk Debugging Disabled\n");
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
			if (option_debug)
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

static int send_command_final(struct chan_iax2_pvt *i, char type, int command, unsigned int ts, const unsigned char *data, int datalen, int seqno)
{
	/* It is assumed that the callno has already been locked */
	iax2_predestroy(i->callno);
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

static int apply_context(struct iax2_context *con, const char *context)
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
	struct iax2_user *user = NULL, *best = NULL;
	int bestscore = 0;
	int gotcapability = 0;
	struct ast_variable *v = NULL, *tmpvar = NULL;

	if (!iaxs[callno])
		return res;
	if (ies->called_number)
		ast_string_field_set(iaxs[callno], exten, ies->called_number);
	if (ies->calling_number) {
		ast_shrink_phone_number(ies->calling_number);
		ast_string_field_set(iaxs[callno], cid_num, ies->calling_number);
	}
	if (ies->calling_name)
		ast_string_field_set(iaxs[callno], cid_name, ies->calling_name);
	if (ies->calling_ani)
		ast_string_field_set(iaxs[callno], ani, ies->calling_ani);
	if (ies->dnid)
		ast_string_field_set(iaxs[callno], dnid, ies->dnid);
	if (ies->rdnis)
		ast_string_field_set(iaxs[callno], rdnis, ies->rdnis);
	if (ies->called_context)
		ast_string_field_set(iaxs[callno], context, ies->called_context);
	if (ies->language)
		ast_string_field_set(iaxs[callno], language, ies->language);
	if (ies->username)
		ast_string_field_set(iaxs[callno], username, ies->username);
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

	/* Use provided preferences until told otherwise for actual preferences */
	if(ies->codec_prefs) {
		ast_codec_pref_convert(&iaxs[callno]->rprefs, ies->codec_prefs, 32, 0);
		ast_codec_pref_convert(&iaxs[callno]->prefs, ies->codec_prefs, 32, 0);
	}

	if (!gotcapability) 
		iaxs[callno]->peercapability = iaxs[callno]->peerformat;
	if (version > IAX_PROTO_VERSION) {
		ast_log(LOG_WARNING, "Peer '%s' has too new a protocol version (%d) for me\n", 
			ast_inet_ntoa(sin->sin_addr), version);
		return res;
	}
	/* Search the userlist for a compatible entry, and fill in the rest */
	AST_LIST_LOCK(&users);
	AST_LIST_TRAVERSE(&users, user, entry) {
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
	}
	AST_LIST_UNLOCK(&users);
	user = best;
	if (!user && !ast_strlen_zero(iaxs[callno]->username)) {
		user = realtime_user(iaxs[callno]->username);
		if (user && !ast_strlen_zero(iaxs[callno]->context) &&			/* No context specified */
		    !apply_context(user->contexts, iaxs[callno]->context)) {		/* Context is permitted */
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
		/* If a max AUTHREQ restriction is in place, activate it */
		if (user->maxauthreq > 0)
			ast_set_flag(iaxs[callno], IAX_MAXAUTHREQ);
		iaxs[callno]->prefs = user->prefs;
		ast_copy_flags(iaxs[callno], user, IAX_CODEC_USER_FIRST);
		ast_copy_flags(iaxs[callno], user, IAX_CODEC_NOPREFS);
		ast_copy_flags(iaxs[callno], user, IAX_CODEC_NOCAP);
		iaxs[callno]->encmethods = user->encmethods;
		/* Store the requested username if not specified */
		if (ast_strlen_zero(iaxs[callno]->username))
			ast_string_field_set(iaxs[callno], username, user->name);
		/* Store whether this is a trunked call, too, of course, and move if appropriate */
		ast_copy_flags(iaxs[callno], user, IAX_TRUNK);
		iaxs[callno]->capability = user->capability;
		/* And use the default context */
		if (ast_strlen_zero(iaxs[callno]->context)) {
			if (user->contexts)
				ast_string_field_set(iaxs[callno], context, user->contexts->context);
			else
				ast_string_field_set(iaxs[callno], context, context);
		}
		/* And any input keys */
		ast_string_field_set(iaxs[callno], inkeys, user->inkeys);
		/* And the permitted authentication methods */
		iaxs[callno]->authmethods = user->authmethods;
		iaxs[callno]->adsi = user->adsi;
		/* If they have callerid, override the given caller id.  Always store the ANI */
		if (!ast_strlen_zero(iaxs[callno]->cid_num) || !ast_strlen_zero(iaxs[callno]->cid_name)) {
			if (ast_test_flag(user, IAX_HASCALLERID)) {
				iaxs[callno]->calling_tns = 0;
				iaxs[callno]->calling_ton = 0;
				ast_string_field_set(iaxs[callno], cid_num, user->cid_num);
				ast_string_field_set(iaxs[callno], cid_name, user->cid_name);
				iaxs[callno]->calling_pres = AST_PRES_ALLOWED_USER_NUMBER_PASSED_SCREEN;
			}
			if (ast_strlen_zero(iaxs[callno]->ani))
				ast_string_field_set(iaxs[callno], ani, user->cid_num);
		} else {
			iaxs[callno]->calling_pres = AST_PRES_NUMBER_NOT_AVAILABLE;
		}
		if (!ast_strlen_zero(user->accountcode))
			ast_string_field_set(iaxs[callno], accountcode, user->accountcode);
		if (!ast_strlen_zero(user->mohinterpret))
			ast_string_field_set(iaxs[callno], mohinterpret, user->mohinterpret);
		if (!ast_strlen_zero(user->mohsuggest))
			ast_string_field_set(iaxs[callno], mohsuggest, user->mohsuggest);
		if (user->amaflags)
			iaxs[callno]->amaflags = user->amaflags;
		if (!ast_strlen_zero(user->language))
			ast_string_field_set(iaxs[callno], language, user->language);
		ast_copy_flags(iaxs[callno], user, IAX_NOTRANSFER | IAX_TRANSFERMEDIA | IAX_USEJITTERBUF | IAX_FORCEJITTERBUF);	
		/* Keep this check last */
		if (!ast_strlen_zero(user->dbsecret)) {
			char *family, *key=NULL;
			char buf[80];
			family = ast_strdupa(user->dbsecret);
			key = strchr(family, '/');
			if (key) {
				*key = '\0';
				key++;
			}
			if (!key || ast_db_get(family, key, buf, sizeof(buf))) {
				ast_log(LOG_WARNING, "Unable to retrieve database password for family/key '%s'!\n", user->dbsecret);
				if (ast_test_flag(user, IAX_TEMPONLY)) {
					destroy_user(user);
					user = NULL;
				}
			} else
				ast_string_field_set(iaxs[callno], secret, buf);
		} else
			ast_string_field_set(iaxs[callno], secret, user->secret);
		res = 0;
	}
	ast_set2_flag(iaxs[callno], iax2_getpeertrunk(*sin), IAX_TRUNK);	
	return res;
}

static int raw_hangup(struct sockaddr_in *sin, unsigned short src, unsigned short dst, int sockfd)
{
	struct ast_iax2_full_hdr fh;
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
			ast_inet_ntoa(sin->sin_addr), ntohs(sin->sin_port), src, dst);
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
	struct iax2_user *user = NULL;
	struct iax_ie_data ied;
	int res = -1, authreq_restrict = 0;
	char challenge[10];

	memset(&ied, 0, sizeof(ied));

	/* If an AUTHREQ restriction is in place, make sure we can send an AUTHREQ back */
	if (ast_test_flag(p, IAX_MAXAUTHREQ)) {
		AST_LIST_LOCK(&users);
		AST_LIST_TRAVERSE(&users, user, entry) {
			if (!strcmp(user->name, p->username)) {
				if (user->curauthreq == user->maxauthreq)
					authreq_restrict = 1;
				else
					user->curauthreq++;
				break;
			}
		}
		AST_LIST_UNLOCK(&users);
	}

	/* If the AUTHREQ limit test failed, send back an error */
	if (authreq_restrict) {
		iax_ie_append_str(&ied, IAX_IE_CAUSE, "Unauthenticated call limit reached");
		iax_ie_append_byte(&ied, IAX_IE_CAUSECODE, AST_CAUSE_CALL_REJECTED);
		send_command_final(p, AST_FRAME_IAX, IAX_COMMAND_REJECT, 0, ied.buf, ied.pos, -1);
		return 0;
	}

	iax_ie_append_short(&ied, IAX_IE_AUTHMETHODS, p->authmethods);
	if (p->authmethods & (IAX_AUTH_MD5 | IAX_AUTH_RSA)) {
		snprintf(challenge, sizeof(challenge), "%d", (int)ast_random());
		ast_string_field_set(p, challenge, challenge);
		/* snprintf(p->challenge, sizeof(p->challenge), "%d", (int)ast_random()); */
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
	struct iax2_user *user = NULL;

	if (ast_test_flag(p, IAX_MAXAUTHREQ)) {
		AST_LIST_LOCK(&users);
		AST_LIST_TRAVERSE(&users, user, entry) {
			if (!strcmp(user->name, p->username)) {
				user->curauthreq--;
				break;
			}
		}
		AST_LIST_UNLOCK(&users);
		ast_clear_flag(p, IAX_MAXAUTHREQ);
	}

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
		
		tmppw = ast_strdupa(p->secret);
		stringp = tmppw;
		while((tmppw = strsep(&stringp, ";"))) {
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
	struct iax2_peer *p;
	struct ast_key *key;
	char *keyn;
	int x;
	int expire = 0;

	ast_clear_flag(&iaxs[callno]->state, IAX_STATE_AUTHENTICATED | IAX_STATE_UNCHANGED);
	/* iaxs[callno]->peer[0] = '\0'; not necc. any more-- stringfield is pre-inited to null string */
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
		ast_log(LOG_NOTICE, "Empty registration from %s\n", ast_inet_ntoa(sin->sin_addr));
		return -1;
	}

	/* SLD: first call to lookup peer during registration */
	p = find_peer(peer, 1);

	if (!p) {
		if (authdebug)
			ast_log(LOG_NOTICE, "No registration for peer '%s' (from %s)\n", peer, ast_inet_ntoa(sin->sin_addr));
		return -1;
	}

	if (!ast_test_flag(p, IAX_DYNAMIC)) {
		if (authdebug)
			ast_log(LOG_NOTICE, "Peer '%s' is not dynamic (from %s)\n", peer, ast_inet_ntoa(sin->sin_addr));
		if (ast_test_flag(p, IAX_TEMPONLY))
			destroy_peer(p);
		return -1;
	}

	if (!ast_apply_ha(p->ha, sin)) {
		if (authdebug)
			ast_log(LOG_NOTICE, "Host %s denied access to register peer '%s'\n", ast_inet_ntoa(sin->sin_addr), p->name);
		if (ast_test_flag(p, IAX_TEMPONLY))
			destroy_peer(p);
		return -1;
	}
	if (!inaddrcmp(&p->addr, sin))
		ast_set_flag(&iaxs[callno]->state, IAX_STATE_UNCHANGED);
	ast_string_field_set(iaxs[callno], secret, p->secret);
	ast_string_field_set(iaxs[callno], inkeys, p->inkeys);
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
	} else if (!ast_strlen_zero(md5secret) && (p->authmethods & IAX_AUTH_MD5) && !ast_strlen_zero(iaxs[callno]->challenge)) {
		struct MD5Context md5;
		unsigned char digest[16];
		char *tmppw, *stringp;
		
		tmppw = ast_strdupa(p->secret);
		stringp = tmppw;
		while((tmppw = strsep(&stringp, ";"))) {
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
				ast_log(LOG_NOTICE, "Host %s failed MD5 authentication for '%s' (%s != %s)\n", ast_inet_ntoa(sin->sin_addr), p->name, requeststr, md5secret);
			if (ast_test_flag(p, IAX_TEMPONLY))
				destroy_peer(p);
			return -1;
		}
	} else if (!ast_strlen_zero(secret) && (p->authmethods & IAX_AUTH_PLAINTEXT)) {
		/* They've provided a plain text password and we support that */
		if (strcmp(secret, p->secret)) {
			if (authdebug)
				ast_log(LOG_NOTICE, "Host %s did not provide proper plaintext password for '%s'\n", ast_inet_ntoa(sin->sin_addr), p->name);
			if (ast_test_flag(p, IAX_TEMPONLY))
				destroy_peer(p);
			return -1;
		} else
			ast_set_flag(&iaxs[callno]->state, IAX_STATE_AUTHENTICATED);
	} else if (!ast_strlen_zero(md5secret) || !ast_strlen_zero(secret)) {
		if (authdebug)
			ast_log(LOG_NOTICE, "Inappropriate authentication received\n");
		if (ast_test_flag(p, IAX_TEMPONLY))
			destroy_peer(p);
		return -1;
	}
	ast_string_field_set(iaxs[callno], peer, peer);
	/* Choose lowest expiry number */
	if (expire && (expire < iaxs[callno]->expiry)) 
		iaxs[callno]->expiry = expire;

	ast_device_state_changed("IAX2/%s", p->name); /* Activate notification */

	if (ast_test_flag(p, IAX_TEMPONLY))
		destroy_peer(p);
	return 0;
	
}

static int authenticate(const char *challenge, const char *secret, const char *keyn, int authmethods, struct iax_ie_data *ied, struct sockaddr_in *sin, aes_encrypt_ctx *ecx, aes_decrypt_ctx *dcx)
{
	int res = -1;
	int x;
	if (!ast_strlen_zero(keyn)) {
		if (!(authmethods & IAX_AUTH_RSA)) {
			if (ast_strlen_zero(secret)) 
				ast_log(LOG_NOTICE, "Asked to authenticate to %s with an RSA key, but they don't allow RSA authentication\n", ast_inet_ntoa(sin->sin_addr));
		} else if (ast_strlen_zero(challenge)) {
			ast_log(LOG_NOTICE, "No challenge provided for RSA authentication to %s\n", ast_inet_ntoa(sin->sin_addr));
		} else {
			char sig[256];
			struct ast_key *key;
			key = ast_key_get(keyn, AST_KEY_PRIVATE);
			if (!key) {
				ast_log(LOG_NOTICE, "Unable to find private key '%s'\n", keyn);
			} else {
				if (ast_sign(key, (char*)challenge, sig)) {
					ast_log(LOG_NOTICE, "Unable to sign challenge with key\n");
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
			ast_log(LOG_NOTICE, "No way to send secret to peer '%s' (their methods: %d)\n", ast_inet_ntoa(sin->sin_addr), authmethods);
	}
	return res;
}

static int authenticate_reply(struct chan_iax2_pvt *p, struct sockaddr_in *sin, struct iax_ies *ies, const char *override, const char *okey)
{
	struct iax2_peer *peer = NULL;
	/* Start pessimistic */
	int res = -1;
	int authmethods = 0;
	struct iax_ie_data ied;
	
	memset(&ied, 0, sizeof(ied));
	
	if (ies->username)
		ast_string_field_set(p, username, ies->username);
	if (ies->challenge)
		ast_string_field_set(p, challenge, ies->challenge);
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
		AST_LIST_LOCK(&peers);
		AST_LIST_TRAVERSE(&peers, peer, entry) {
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
		}
		AST_LIST_UNLOCK(&peers);
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

static void __iax2_do_register_s(void *data)
{
	struct iax2_registry *reg = data;
	reg->expire = -1;
	iax2_do_register(reg);
}

static int iax2_do_register_s(void *data)
{
#ifdef SCHED_MULTITHREADED
	if (schedule_action(__iax2_do_register_s, data))
#endif		
		__iax2_do_register_s(data);
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
		bcopy(ies->apparent_addr, &new, sizeof(new));
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
	int status = CACHE_FLAG_UNKNOWN, expiry = iaxdefaultdpcache, x, matchmore = 0;
	struct iax2_dpcache *dp = NULL;
	
	if (ies->called_number)
		ast_copy_string(exten, ies->called_number, sizeof(exten));
	
	if (ies->dpstatus & IAX_DPSTATUS_EXISTS)
		status = CACHE_FLAG_EXISTS;
	else if (ies->dpstatus & IAX_DPSTATUS_CANEXIST)
		status = CACHE_FLAG_CANEXIST;
	else if (ies->dpstatus & IAX_DPSTATUS_NONEXISTENT)
		status = CACHE_FLAG_NONEXISTENT;

	if (ies->refresh)
		expiry = ies->refresh;
	if (ies->dpstatus & IAX_DPSTATUS_MATCHMORE)
		matchmore = CACHE_FLAG_MATCHMORE;
	
	AST_LIST_LOCK(&dpcache);
	AST_LIST_TRAVERSE_SAFE_BEGIN(&dpcache, dp, peer_list) {
		if (strcmp(dp->exten, exten))
			continue;
		AST_LIST_REMOVE_CURRENT(&dpcache, peer_list);
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
	AST_LIST_TRAVERSE_SAFE_END
	AST_LIST_UNLOCK(&dpcache);

	return 0;
}

static int complete_transfer(int callno, struct iax_ies *ies)
{
	int peercallno = 0;
	struct chan_iax2_pvt *pvt = iaxs[callno];
	struct iax_frame *cur;
	jb_frame frame;

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
	/* reset jitterbuffer */
	while(jb_getall(pvt->jb,&frame) == JB_OK)
		iax2_frame_free(frame.data);
	jb_reset(pvt->jb);
	pvt->lag = 0;
	pvt->last = 0;
	pvt->lastsent = 0;
	pvt->nextpred = 0;
	pvt->pingtime = DEFAULT_RETRY_TIME;
	AST_LIST_LOCK(&iaxq.queue);
	AST_LIST_TRAVERSE(&iaxq.queue, cur, list) {
		/* We must cancel any packets that would have been transmitted
		   because now we're talking to someone new.  It's okay, they
		   were transmitted to someone that didn't care anyway. */
		if (callno == cur->callno) 
			cur->retries = -1;
	}
	AST_LIST_UNLOCK(&iaxq.queue);
	return 0; 
}

/*! \brief Acknowledgment received for OUR registration */
static int iax2_ack_registry(struct iax_ies *ies, struct sockaddr_in *sin, int callno)
{
	struct iax2_registry *reg;
	/* Start pessimistic */
	char peer[256] = "";
	char msgstatus[60];
	int refresh = 0;
	char ourip[256] = "<Unspecified>";
	struct sockaddr_in oldus;
	struct sockaddr_in us;
	int oldmsgs;

	memset(&us, 0, sizeof(us));
	if (ies->apparent_addr)
		bcopy(ies->apparent_addr, &us, sizeof(us));
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
		ast_log(LOG_WARNING, "Received unsolicited registry ack from '%s'\n", ast_inet_ntoa(sin->sin_addr));
		return -1;
	}
	memcpy(&reg->us, &us, sizeof(reg->us));
	if (ies->msgcount >= 0)
		reg->messages = ies->msgcount & 0xffff;		/* only low 16 bits are used in the transmission of the IE */
	/* always refresh the registration at the interval requested by the server
	   we are registering to
	*/
	reg->refresh = refresh;
	if (reg->expire > -1)
		ast_sched_del(sched, reg->expire);
	reg->expire = ast_sched_add(sched, (5 * reg->refresh / 6) * 1000, iax2_do_register_s, reg);
	if (inaddrcmp(&oldus, &reg->us) || (reg->messages != oldmsgs)) {
		if (option_verbose > 2) {
			if (reg->messages > 255)
				snprintf(msgstatus, sizeof(msgstatus), " with %d new and %d old messages waiting", reg->messages & 0xff, reg->messages >> 8);
			else if (reg->messages > 1)
				snprintf(msgstatus, sizeof(msgstatus), " with %d new messages waiting\n", reg->messages);
			else if (reg->messages > 0)
				snprintf(msgstatus, sizeof(msgstatus), " with 1 new message waiting\n");
			else
				snprintf(msgstatus, sizeof(msgstatus), " with no messages waiting\n");
			snprintf(ourip, sizeof(ourip), "%s:%d", ast_inet_ntoa(reg->us.sin_addr), ntohs(reg->us.sin_port));
			ast_verbose(VERBOSE_PREFIX_3 "Registered IAX2 to '%s', who sees us as %s%s\n", ast_inet_ntoa(sin->sin_addr), ourip, msgstatus);
		}
		manager_event(EVENT_FLAG_SYSTEM, "Registry", "ChannelDriver: IAX2\r\nDomain: %s\r\nStatus: Registered\r\n", ast_inet_ntoa(sin->sin_addr));
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
	
	if (!value)
		return -1;
	ast_copy_string(copy, value, sizeof(copy));
	stringp=copy;
	username = strsep(&stringp, "@");
	hostname = strsep(&stringp, "@");
	if (!hostname) {
		ast_log(LOG_WARNING, "Format for registration is user[:secret]@host[:port] at line %d\n", lineno);
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
	if (!(reg = ast_calloc(1, sizeof(*reg))))
		return -1;
	if (ast_dnsmgr_lookup(hostname, &reg->addr.sin_addr, &reg->dnsmgr) < 0) {
		free(reg);
		return -1;
	}
	ast_copy_string(reg->username, username, sizeof(reg->username));
	if (secret)
		ast_copy_string(reg->secret, secret, sizeof(reg->secret));
	reg->expire = -1;
	reg->refresh = IAX_DEFAULT_REG_EXPIRE;
	reg->addr.sin_family = AF_INET;
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
		ast_copy_string(multi, S_OR(peer->regexten, peer->name), sizeof(multi));
		stringp = multi;
		while((ext = strsep(&stringp, "&"))) {
			if (onoff) {
				if (!ast_exists_extension(NULL, regcontext, ext, 1, NULL))
					ast_add_extension(regcontext, 1, ext, 1, NULL, NULL,
							  "Noop", ast_strdup(peer->name), ast_free, "IAX2");
			} else
				ast_context_remove_extension(regcontext, ext, 1, NULL);
		}
	}
}
static void prune_peers(void);

static void __expire_registry(void *data)
{
	char *name = data;
	struct iax2_peer *p = NULL;

	/* Go through and grab this peer... and if it needs to be removed... then do it */
	AST_LIST_LOCK(&peers);
	AST_LIST_TRAVERSE_SAFE_BEGIN(&peers, p, entry) {
		if (!strcasecmp(p->name, name)) {
			p->expire = -1;
			break;
		}
	}
	AST_LIST_TRAVERSE_SAFE_END
	AST_LIST_UNLOCK(&peers);

	/* Peer is already gone for whatever reason */
	if (!p)
		return;

	if (option_debug)
		ast_log(LOG_DEBUG, "Expiring registration for peer '%s'\n", p->name);
	if (ast_test_flag((&globalflags), IAX_RTUPDATE) && (ast_test_flag(p, IAX_TEMPONLY|IAX_RTCACHEFRIENDS)))
		realtime_update_peer(p->name, &p->addr, 0);
	manager_event(EVENT_FLAG_SYSTEM, "PeerStatus", "Peer: IAX2/%s\r\nPeerStatus: Unregistered\r\nCause: Expired\r\n", p->name);
	/* Reset the address */
	memset(&p->addr, 0, sizeof(p->addr));
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
}

static int expire_registry(void *data)
{
#ifdef SCHED_MULTITHREADED
	if (schedule_action(__expire_registry, data))
#endif		
		__expire_registry(data);
	return 0;
}

static int iax2_poke_peer(struct iax2_peer *peer, int heldcall);

static void reg_source_db(struct iax2_peer *p)
{
	char data[80];
	struct in_addr in;
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
						ast_inet_ntoa(in), atoi(c), atoi(d));
					iax2_poke_peer(p, 0);
					p->expiry = atoi(d);
					memset(&p->addr, 0, sizeof(p->addr));
					p->addr.sin_family = AF_INET;
					p->addr.sin_addr = in;
					p->addr.sin_port = htons(atoi(c));
					if (p->expire > -1)
						ast_sched_del(sched, p->expire);
					ast_device_state_changed("IAX2/%s", p->name); /* Activate notification */
					p->expire = ast_sched_add(sched, (p->expiry + 10) * 1000, expire_registry, (void *)p->name);
					if (iax2_regfunk)
						iax2_regfunk(p->name, 1);
					register_peer_exten(p, 1);
				}					
					
			}
		}
	}
}

static int update_registry(const char *name, struct sockaddr_in *sin, int callno, char *devtype, int fd, unsigned short refresh)
{
	/* Called from IAX thread only, with proper iaxsl lock */
	struct iax_ie_data ied;
	struct iax2_peer *p;
	int msgcount;
	char data[80];
	int version;

	memset(&ied, 0, sizeof(ied));

	/* SLD: Another find_peer call during registration - this time when we are really updating our registration */
	if (!(p = find_peer(name, 1))) {
		ast_log(LOG_WARNING, "No such peer '%s'\n", name);
		return -1;
	}

	if (ast_test_flag((&globalflags), IAX_RTUPDATE) && (ast_test_flag(p, IAX_TEMPONLY|IAX_RTCACHEFRIENDS))) {
		if (sin->sin_addr.s_addr) {
			time_t nowtime;
			time(&nowtime);
			realtime_update_peer(name, sin, nowtime);
		} else {
			realtime_update_peer(name, sin, 0);
		}
	}
	if (inaddrcmp(&p->addr, sin)) {
		if (iax2_regfunk)
			iax2_regfunk(p->name, 1);
		/* Stash the IP address from which they registered */
		memcpy(&p->addr, sin, sizeof(p->addr));
		snprintf(data, sizeof(data), "%s:%d:%d", ast_inet_ntoa(sin->sin_addr), ntohs(sin->sin_port), p->expiry);
		if (!ast_test_flag(p, IAX_TEMPONLY) && sin->sin_addr.s_addr) {
			ast_db_put("IAX/Registry", p->name, data);
			if  (option_verbose > 2)
				ast_verbose(VERBOSE_PREFIX_3 "Registered IAX2 '%s' (%s) at %s:%d\n", p->name, 
					    ast_test_flag(&iaxs[callno]->state, IAX_STATE_AUTHENTICATED) ? "AUTHENTICATED" : "UNAUTHENTICATED", ast_inet_ntoa(sin->sin_addr), ntohs(sin->sin_port));
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

	/* Make sure our call still exists, an INVAL at the right point may make it go away */
	if (!iaxs[callno])
		return 0;

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
		p->expire = ast_sched_add(sched, (p->expiry + 10) * 1000, expire_registry, (void *)p->name);
	iax_ie_append_str(&ied, IAX_IE_USERNAME, p->name);
	iax_ie_append_int(&ied, IAX_IE_DATETIME, iax2_datetime(p->zonetag));
	if (sin->sin_addr.s_addr) {
		iax_ie_append_short(&ied, IAX_IE_REFRESH, p->expiry);
		iax_ie_append_addr(&ied, IAX_IE_APPARENT_ADDR, &p->addr);
		if (!ast_strlen_zero(p->mailbox)) {
			int new, old;
			ast_app_inboxcount(p->mailbox, &new, &old);
			if (new > 255)
				new = 255;
			if (old > 255)
				old = 255;
			msgcount = (old << 8) | new;
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

static int registry_authrequest(const char *name, int callno)
{
	struct iax_ie_data ied;
	struct iax2_peer *p;
	char challenge[10];
	/* SLD: third call to find_peer in registration */
	p = find_peer(name, 1);
	if (p) {
		memset(&ied, 0, sizeof(ied));
		iax_ie_append_short(&ied, IAX_IE_AUTHMETHODS, p->authmethods);
		if (p->authmethods & (IAX_AUTH_RSA | IAX_AUTH_MD5)) {
			/* Build the challenge */
			snprintf(challenge, sizeof(challenge), "%d", (int)ast_random());
			ast_string_field_set(iaxs[callno], challenge, challenge);
			/* snprintf(iaxs[callno]->challenge, sizeof(iaxs[callno]->challenge), "%d", (int)ast_random()); */
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
				ast_log(LOG_WARNING, "Received unsolicited registry authenticate request from '%s'\n", ast_inet_ntoa(sin->sin_addr));
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

static void stop_stuff(int callno)
{
	iax2_destroy_helper(iaxs[callno]);
}

static void __auth_reject(void *nothing)
{
	/* Called from IAX thread only, without iaxs lock */
	int callno = (int)(long)(nothing);
	struct iax_ie_data ied;
	ast_mutex_lock(&iaxsl[callno]);
	if (iaxs[callno]) {
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
}

static int auth_reject(void *data)
{
	int callno = (int)(long)(data);
	ast_mutex_lock(&iaxsl[callno]);
	if (iaxs[callno])
		iaxs[callno]->authid = -1;
	ast_mutex_unlock(&iaxsl[callno]);
#ifdef SCHED_MULTITHREADED
	if (schedule_action(__auth_reject, data))
#endif		
		__auth_reject(data);
	return 0;
}

static int auth_fail(int callno, int failcode)
{
	/* Schedule sending the authentication failure in one second, to prevent
	   guessing */
	ast_mutex_lock(&iaxsl[callno]);
	if (iaxs[callno]) {
		iaxs[callno]->authfail = failcode;
		if (delayreject) {
			if (iaxs[callno]->authid > -1)
				ast_sched_del(sched, iaxs[callno]->authid);
			iaxs[callno]->authid = ast_sched_add(sched, 1000, auth_reject, (void *)(long)callno);
		} else
			auth_reject((void *)(long)callno);
	}
	ast_mutex_unlock(&iaxsl[callno]);
	return 0;
}

static void __auto_hangup(void *nothing)
{
	/* Called from IAX thread only, without iaxs lock */
	int callno = (int)(long)(nothing);
	struct iax_ie_data ied;
	ast_mutex_lock(&iaxsl[callno]);
	if (iaxs[callno]) {
		memset(&ied, 0, sizeof(ied));
		iax_ie_append_str(&ied, IAX_IE_CAUSE, "Timeout");
		iax_ie_append_byte(&ied, IAX_IE_CAUSECODE, AST_CAUSE_NO_USER_RESPONSE);
		send_command_final(iaxs[callno], AST_FRAME_IAX, IAX_COMMAND_HANGUP, 0, ied.buf, ied.pos, -1);
	}
	ast_mutex_unlock(&iaxsl[callno]);
}

static int auto_hangup(void *data)
{
	int callno = (int)(long)(data);
	ast_mutex_lock(&iaxsl[callno]);
	if (iaxs[callno]) {
		iaxs[callno]->autoid = -1;
	}
	ast_mutex_unlock(&iaxsl[callno]);
#ifdef SCHED_MULTITHREADED
	if (schedule_action(__auto_hangup, data))
#endif		
		__auto_hangup(data);
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

	AST_LIST_LOCK(&iaxq.queue);
	AST_LIST_TRAVERSE(&iaxq.queue, f, list) {
		/* Send a copy immediately */
		if ((f->callno == callno) && iaxs[f->callno] &&
			(f->oseqno >= last)) {
			send_packet(f);
		}
	}
	AST_LIST_UNLOCK(&iaxq.queue);
}

static void __iax2_poke_peer_s(void *data)
{
	struct iax2_peer *peer = data;
	iax2_poke_peer(peer, 0);
}

static int iax2_poke_peer_s(void *data)
{
	struct iax2_peer *peer = data;
	peer->pokeexpire = -1;
#ifdef SCHED_MULTITHREADED
	if (schedule_action(__iax2_poke_peer_s, data))
#endif		
		__iax2_poke_peer_s(data);
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
		if (option_debug)
			ast_log(LOG_DEBUG, "Trunking %d call chunks in %d bytes to %s:%d, ts=%d\n", calls, fr->datalen, ast_inet_ntoa(tpeer->addr.sin_addr), ntohs(tpeer->addr.sin_port), ntohl(mth->ts));
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
	int res, processed = 0, totalcalls = 0;
	struct iax2_trunk_peer *tpeer = NULL, *drop = NULL;
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
			return 1;
		}
	}
	/* For each peer that supports trunking... */
	AST_LIST_LOCK(&tpeers);
	AST_LIST_TRAVERSE_SAFE_BEGIN(&tpeers, tpeer, list) {
		processed++;
		res = 0;
		ast_mutex_lock(&tpeer->lock);
		/* We can drop a single tpeer per pass.  That makes all this logic
		   substantially easier */
		if (!drop && iax2_trunk_expired(tpeer, &now)) {
			/* Take it out of the list, but don't free it yet, because it
			   could be in use */
			AST_LIST_REMOVE_CURRENT(&tpeers, list);
			drop = tpeer;
		} else {
			res = send_trunk(tpeer, &now);
			trunk_timed++; 
			if (iaxtrunkdebug)
				ast_verbose(" - Trunk peer (%s:%d) has %d call chunk%s in transit, %d bytes backloged and has hit a high water mark of %d bytes\n", ast_inet_ntoa(tpeer->addr.sin_addr), ntohs(tpeer->addr.sin_port), res, (res != 1) ? "s" : "", tpeer->trunkdatalen, tpeer->trunkdataalloc);
		}		
		totalcalls += res;	
		res = 0;
		ast_mutex_unlock(&tpeer->lock);
	}
	AST_LIST_TRAVERSE_SAFE_END
	AST_LIST_UNLOCK(&tpeers);

	if (drop) {
		ast_mutex_lock(&drop->lock);
		/* Once we have this lock, we're sure nobody else is using it or could use it once we release it, 
		   because by the time they could get tpeerlock, we've already grabbed it */
		if (option_debug)
			ast_log(LOG_DEBUG, "Dropping unused iax2 trunk peer '%s:%d'\n", ast_inet_ntoa(drop->addr.sin_addr), ntohs(drop->addr.sin_port));
		free(drop->trunkdata);
		ast_mutex_unlock(&drop->lock);
		ast_mutex_destroy(&drop->lock);
		free(drop);
		
	}

	if (iaxtrunkdebug)
		ast_verbose("Ending trunk processing with %d peers and %d call chunks processed\n", processed, totalcalls);
	iaxtrunkdebug = 0;

	return 1;
}

struct dpreq_data {
	int callno;
	char context[AST_MAX_EXTENSION];
	char callednum[AST_MAX_EXTENSION];
	char *callerid;
};

static void dp_lookup(int callno, const char *context, const char *callednum, const char *callerid, int skiplock)
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

static void spawn_dp_lookup(int callno, const char *context, const char *callednum, const char *callerid)
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
	chan1m = ast_channel_alloc(0, AST_STATE_DOWN, 0, 0, "Parking/%s", chan1->name);
	chan2m = ast_channel_alloc(0, AST_STATE_DOWN, 0, 0, "IAXPeer/%s",chan2->name);
	if (chan2m && chan1m) {
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
		if (!ast_pthread_create_background(&th, NULL, iax_park_thread, d))
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
	struct iax2_thread *thread;
	socklen_t len;
	time_t t;
	static time_t last_errtime=0;

	thread = find_idle_thread();
	if (thread) {
		len = sizeof(thread->iosin);
		thread->iofd = fd;
		thread->iores = recvfrom(fd, thread->buf, sizeof(thread->buf), 0,(struct sockaddr *) &thread->iosin, &len);
		if (thread->iores < 0) {
			if (errno != ECONNREFUSED && errno != EAGAIN)
				ast_log(LOG_WARNING, "Error: %s\n", strerror(errno));
			handle_error();
			AST_LIST_LOCK(&idle_list);
			AST_LIST_INSERT_TAIL(&idle_list, thread, list);
			AST_LIST_UNLOCK(&idle_list);
			return 1;
		}
		if (test_losspct && ((100.0 * ast_random() / (RAND_MAX + 1.0)) < test_losspct)) { /* simulate random loss condition */
			AST_LIST_LOCK(&idle_list);
			AST_LIST_INSERT_TAIL(&idle_list, thread, list);
			AST_LIST_UNLOCK(&idle_list);
			return 1;
		}
		/* Mark as ready and send on its way */
		thread->iostate = IAX_IOSTATE_READY;
#ifdef DEBUG_SCHED_MULTITHREAD
		ast_copy_string(thread->curfunc, "socket_process", sizeof(thread->curfunc));
#endif
		signal_condition(&thread->lock, &thread->cond);
	} else {
		time(&t);
		if (t != last_errtime)
			ast_log(LOG_NOTICE, "Out of idle IAX2 threads for I/O, pausing!\n");
		last_errtime = t;
		usleep(1);
	}
	return 1;
}

static int socket_process(struct iax2_thread *thread)
{
	struct sockaddr_in sin;
	int res;
	int updatehistory=1;
	int new = NEW_PREVENT;
	void *ptr;
	int dcallno = 0;
	struct ast_iax2_full_hdr *fh = (struct ast_iax2_full_hdr *)thread->buf;
	struct ast_iax2_mini_hdr *mh = (struct ast_iax2_mini_hdr *)thread->buf;
	struct ast_iax2_meta_hdr *meta = (struct ast_iax2_meta_hdr *)thread->buf;
	struct ast_iax2_video_hdr *vh = (struct ast_iax2_video_hdr *)thread->buf;
	struct ast_iax2_meta_trunk_hdr *mth;
	struct ast_iax2_meta_trunk_entry *mte;
	struct ast_iax2_meta_trunk_mini *mtm;
	struct iax_frame *fr;
	struct iax_frame *cur;
	struct ast_frame f;
	struct ast_channel *c;
	struct iax2_dpcache *dp;
	struct iax2_peer *peer;
	struct iax2_trunk_peer *tpeer;
	struct timeval rxtrunktime;
	struct iax_ies ies;
	struct iax_ie_data ied0, ied1;
	int format;
	int fd;
	int exists;
	int minivid = 0;
	unsigned int ts;
	char empty[32]="";		/* Safety measure */
	struct iax_frame *duped_fr;
	char host_pref_buf[128];
	char caller_pref_buf[128];
	struct ast_codec_pref pref;
	char *using_prefs = "mine";

	/* allocate an iax_frame with 4096 bytes of data buffer */
	fr = alloca(sizeof(*fr) + 4096);
	fr->callno = 0;

	/* Copy frequently used parameters to the stack */
	res = thread->iores;
	fd = thread->iofd;
	memcpy(&sin, &thread->iosin, sizeof(sin));

	if (res < sizeof(*mh)) {
		ast_log(LOG_WARNING, "midget packet received (%d of %zd min)\n", res, sizeof(*mh));
		return 1;
	}
	if ((vh->zeros == 0) && (ntohs(vh->callno) & 0x8000)) {
		if (res < sizeof(*vh)) {
			ast_log(LOG_WARNING, "Rejecting packet from '%s.%d' that is flagged as a video frame but is too short\n", ast_inet_ntoa(sin.sin_addr), ntohs(sin.sin_port));
			return 1;
		}

		/* This is a video frame, get call number */
		fr->callno = find_callno(ntohs(vh->callno) & ~0x8000, dcallno, &sin, new, 1, fd);
		minivid = 1;
	} else if ((meta->zeros == 0) && !(ntohs(meta->metacmd) & 0x8000)) {
		unsigned char metatype;

		if (res < sizeof(*meta)) {
			ast_log(LOG_WARNING, "Rejecting packet from '%s.%d' that is flagged as a meta frame but is too short\n", ast_inet_ntoa(sin.sin_addr), ntohs(sin.sin_port));
			return 1;
		}

		/* This is a meta header */
		switch(meta->metacmd) {
		case IAX_META_TRUNK:
			if (res < (sizeof(*meta) + sizeof(*mth))) {
				ast_log(LOG_WARNING, "midget meta trunk packet received (%d of %zd min)\n", res,
					sizeof(*meta) + sizeof(*mth));
				return 1;
			}
			mth = (struct ast_iax2_meta_trunk_hdr *)(meta->data);
			ts = ntohl(mth->ts);
			metatype = meta->cmddata;
			res -= (sizeof(*meta) + sizeof(*mth));
			ptr = mth->data;
			tpeer = find_tpeer(&sin, fd);
			if (!tpeer) {
				ast_log(LOG_WARNING, "Unable to accept trunked packet from '%s:%d': No matching peer\n", ast_inet_ntoa(sin.sin_addr), ntohs(sin.sin_port));
				return 1;
			}
			tpeer->trunkact = ast_tvnow();
			if (!ts || ast_tvzero(tpeer->rxtrunktime))
				tpeer->rxtrunktime = tpeer->trunkact;
			rxtrunktime = tpeer->rxtrunktime;
			ast_mutex_unlock(&tpeer->lock);
			while(res >= sizeof(*mte)) {
				/* Process channels */
				unsigned short callno, trunked_ts, len;

				if (metatype == IAX_META_TRUNK_MINI) {
					mtm = (struct ast_iax2_meta_trunk_mini *)ptr;
					ptr += sizeof(*mtm);
					res -= sizeof(*mtm);
					len = ntohs(mtm->len);
					callno = ntohs(mtm->mini.callno);
					trunked_ts = ntohs(mtm->mini.ts);
				} else if (metatype == IAX_META_TRUNK_SUPERMINI) {
					mte = (struct ast_iax2_meta_trunk_entry *)ptr;
					ptr += sizeof(*mte);
					res -= sizeof(*mte);
					len = ntohs(mte->len);
					callno = ntohs(mte->callno);
					trunked_ts = 0;
				} else {
					ast_log(LOG_WARNING, "Unknown meta trunk cmd from '%s:%d': dropping\n", ast_inet_ntoa(sin.sin_addr), ntohs(sin.sin_port));
					break;
				}
				/* Stop if we don't have enough data */
				if (len > res)
					break;
				fr->callno = find_callno(callno & ~IAX_FLAG_FULL, 0, &sin, NEW_PREVENT, 1, fd);
				if (fr->callno) {
					ast_mutex_lock(&iaxsl[fr->callno]);
					/* If it's a valid call, deliver the contents.  If not, we
					   drop it, since we don't have a scallno to use for an INVAL */
					/* Process as a mini frame */
					f.frametype = AST_FRAME_VOICE;
					if (iaxs[fr->callno]) {
						if (iaxs[fr->callno]->voiceformat > 0) {
							f.subclass = iaxs[fr->callno]->voiceformat;
							f.datalen = len;
							if (f.datalen >= 0) {
								if (f.datalen)
									f.data = ptr;
								else
									f.data = NULL;
								if(trunked_ts) {
									fr->ts = (iaxs[fr->callno]->last & 0xFFFF0000L) | (trunked_ts & 0xffff);
								} else
									fr->ts = fix_peerts(&rxtrunktime, fr->callno, ts);
								/* Don't pass any packets until we're started */
								if (ast_test_flag(&iaxs[fr->callno]->state, IAX_STATE_STARTED)) {
									/* Common things */
									f.src = "IAX2";
									f.mallocd = 0;
									f.offset = 0;
									if (f.datalen && (f.frametype == AST_FRAME_VOICE)) 
										f.samples = ast_codec_get_samples(&f);
									else
										f.samples = 0;
									fr->outoforder = 0;
									iax_frame_wrap(fr, &f);
									duped_fr = iaxfrdup2(fr);
									if (duped_fr) {
										schedule_delivery(duped_fr, updatehistory, 1, &fr->ts);
									}
									if (iaxs[fr->callno]->last < fr->ts) {
										iaxs[fr->callno]->last = fr->ts;
#if 1
										if (option_debug && iaxdebug)
											ast_log(LOG_DEBUG, "For call=%d, set last=%d\n", fr->callno, fr->ts);
#endif
									}
								}
							} else {
								ast_log(LOG_WARNING, "Datalen < 0?\n");
							}
						} else {
							ast_log(LOG_WARNING, "Received trunked frame before first full voice frame\n ");
							iax2_vnak(fr->callno);
						}
					}
					ast_mutex_unlock(&iaxsl[fr->callno]);
				}
				ptr += len;
				res -= len;
			}
			
		}
		return 1;
	}

#ifdef DEBUG_SUPPORT
	if (iaxdebug && (res >= sizeof(*fh)))
		iax_showframe(NULL, fh, 1, &sin, res - sizeof(*fh));
#endif
	if (ntohs(mh->callno) & IAX_FLAG_FULL) {
		if (res < sizeof(*fh)) {
			ast_log(LOG_WARNING, "Rejecting packet from '%s.%d' that is flagged as a full frame but is too short\n", ast_inet_ntoa(sin.sin_addr), ntohs(sin.sin_port));
			return 1;
		}

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

	if (!fr->callno)
		fr->callno = find_callno(ntohs(mh->callno) & ~IAX_FLAG_FULL, dcallno, &sin, new, 1, fd);

	if (fr->callno > 0) 
		ast_mutex_lock(&iaxsl[fr->callno]);

	if (!fr->callno || !iaxs[fr->callno]) {
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
		if (fr->callno > 0) 
			ast_mutex_unlock(&iaxsl[fr->callno]);
		return 1;
	}
	if (ast_test_flag(iaxs[fr->callno], IAX_ENCRYPTED)) {
		if (decrypt_frame(fr->callno, fh, &f, &res)) {
			ast_log(LOG_NOTICE, "Packet Decrypt Failed!\n");
			ast_mutex_unlock(&iaxsl[fr->callno]);
			return 1;
		}
#ifdef DEBUG_SUPPORT
		else if (iaxdebug)
			iax_showframe(NULL, fh, 3, &sin, res - sizeof(*fh));
#endif
	}

	/* count this frame */
	iaxs[fr->callno]->frames_received++;

	if (!inaddrcmp(&sin, &iaxs[fr->callno]->addr) && !minivid &&
		f.subclass != IAX_COMMAND_TXCNT &&		/* for attended transfer */
		f.subclass != IAX_COMMAND_TXACC)		/* for attended transfer */
		iaxs[fr->callno]->peercallno = (unsigned short)(ntohs(mh->callno) & ~IAX_FLAG_FULL);
	if (ntohs(mh->callno) & IAX_FLAG_FULL) {
		if (option_debug  && iaxdebug)
			ast_log(LOG_DEBUG, "Received packet %d, (%d, %d)\n", fh->oseqno, f.frametype, f.subclass);
		/* Check if it's out of order (and not an ACK or INVAL) */
		fr->oseqno = fh->oseqno;
		fr->iseqno = fh->iseqno;
		fr->ts = ntohl(fh->ts);
#ifdef IAXTESTS
		if (test_resync) {
			if (option_debug)
				ast_log(LOG_DEBUG, "Simulating frame ts resync, was %u now %u\n", fr->ts, fr->ts + test_resync);
			fr->ts += test_resync;
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
		if ((iaxs[fr->callno]->iseqno != fr->oseqno) &&
			(iaxs[fr->callno]->iseqno ||
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
					iaxs[fr->callno]->iseqno, fr->oseqno, f.frametype, f.subclass);
				if (iaxs[fr->callno]->iseqno > fr->oseqno) {
					/* If we've already seen it, ack it XXX There's a border condition here XXX */
					if ((f.frametype != AST_FRAME_IAX) || 
							((f.subclass != IAX_COMMAND_ACK) && (f.subclass != IAX_COMMAND_INVAL))) {
						if (option_debug)
							ast_log(LOG_DEBUG, "Acking anyway\n");
						/* XXX Maybe we should handle its ack to us, but then again, it's probably outdated anyway, and if
						   we have anything to send, we'll retransmit and get an ACK back anyway XXX */
						send_command_immediate(iaxs[fr->callno], AST_FRAME_IAX, IAX_COMMAND_ACK, fr->ts, NULL, 0,fr->iseqno);
					}
				} else {
					/* Send a VNAK requesting retransmission */
					iax2_vnak(fr->callno);
				}
				ast_mutex_unlock(&iaxsl[fr->callno]);
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
				iaxs[fr->callno]->iseqno++;
		}
		/* A full frame */
		if (res < sizeof(*fh)) {
			ast_log(LOG_WARNING, "midget packet received (%d of %zd min)\n", res, sizeof(*fh));
			ast_mutex_unlock(&iaxsl[fr->callno]);
			return 1;
		}
		f.datalen = res - sizeof(*fh);

		/* Handle implicit ACKing unless this is an INVAL, and only if this is 
		   from the real peer, not the transfer peer */
		if (!inaddrcmp(&sin, &iaxs[fr->callno]->addr) && 
		    ((f.subclass != IAX_COMMAND_INVAL) ||
		     (f.frametype != AST_FRAME_IAX))) {
			unsigned char x;
			/* XXX This code is not very efficient.  Surely there is a better way which still
			       properly handles boundary conditions? XXX */
			/* First we have to qualify that the ACKed value is within our window */
			for (x=iaxs[fr->callno]->rseqno; x != iaxs[fr->callno]->oseqno; x++)
				if (fr->iseqno == x)
					break;
			if ((x != iaxs[fr->callno]->oseqno) || (iaxs[fr->callno]->oseqno == fr->iseqno)) {
				/* The acknowledgement is within our window.  Time to acknowledge everything
				   that it says to */
				for (x=iaxs[fr->callno]->rseqno; x != fr->iseqno; x++) {
					/* Ack the packet with the given timestamp */
					if (option_debug && iaxdebug)
						ast_log(LOG_DEBUG, "Cancelling transmission of packet %d\n", x);
					AST_LIST_LOCK(&iaxq.queue);
					AST_LIST_TRAVERSE(&iaxq.queue, cur, list) {
						/* If it's our call, and our timestamp, mark -1 retries */
						if ((fr->callno == cur->callno) && (x == cur->oseqno)) {
							cur->retries = -1;
							/* Destroy call if this is the end */
							if (cur->final) { 
								if (iaxdebug && option_debug)
									ast_log(LOG_DEBUG, "Really destroying %d, having been acked on final message\n", fr->callno);
								iax2_destroy(fr->callno);
							}
						}
					}
					AST_LIST_UNLOCK(&iaxq.queue);
				}
				/* Note how much we've received acknowledgement for */
				if (iaxs[fr->callno])
					iaxs[fr->callno]->rseqno = fr->iseqno;
				else {
					/* Stop processing now */
					ast_mutex_unlock(&iaxsl[fr->callno]);
					return 1;
				}
			} else {
				if (option_debug)
					ast_log(LOG_DEBUG, "Received iseqno %d not within window %d->%d\n", fr->iseqno, iaxs[fr->callno]->rseqno, iaxs[fr->callno]->oseqno);
			}
		}
		if (inaddrcmp(&sin, &iaxs[fr->callno]->addr) && 
			((f.frametype != AST_FRAME_IAX) || 
			 ((f.subclass != IAX_COMMAND_TXACC) &&
			  (f.subclass != IAX_COMMAND_TXCNT)))) {
			/* Only messages we accept from a transfer host are TXACC and TXCNT */
			ast_mutex_unlock(&iaxsl[fr->callno]);
			return 1;
		}

		if (f.datalen) {
			if (f.frametype == AST_FRAME_IAX) {
				if (iax_parse_ies(&ies, thread->buf + sizeof(*fh), f.datalen)) {
					ast_log(LOG_WARNING, "Undecodable frame received from '%s'\n", ast_inet_ntoa(sin.sin_addr));
					ast_mutex_unlock(&iaxsl[fr->callno]);
					return 1;
				}
				f.data = NULL;
			} else
				f.data = thread->buf + sizeof(*fh);
		} else {
			if (f.frametype == AST_FRAME_IAX)
				f.data = NULL;
			else
				f.data = empty;
			memset(&ies, 0, sizeof(ies));
		}
		if (f.frametype == AST_FRAME_VOICE) {
			if (f.subclass != iaxs[fr->callno]->voiceformat) {
					iaxs[fr->callno]->voiceformat = f.subclass;
					if (option_debug)
						ast_log(LOG_DEBUG, "Ooh, voice format changed to %d\n", f.subclass);
					if (iaxs[fr->callno]->owner) {
						int orignative;
retryowner:
						if (ast_mutex_trylock(&iaxs[fr->callno]->owner->lock)) {
							ast_mutex_unlock(&iaxsl[fr->callno]);
							usleep(1);
							ast_mutex_lock(&iaxsl[fr->callno]);
							if (iaxs[fr->callno] && iaxs[fr->callno]->owner) goto retryowner;
						}
						if (iaxs[fr->callno]) {
							if (iaxs[fr->callno]->owner) {
								orignative = iaxs[fr->callno]->owner->nativeformats;
								iaxs[fr->callno]->owner->nativeformats = f.subclass;
								if (iaxs[fr->callno]->owner->readformat)
									ast_set_read_format(iaxs[fr->callno]->owner, iaxs[fr->callno]->owner->readformat);
								iaxs[fr->callno]->owner->nativeformats = orignative;
								ast_mutex_unlock(&iaxs[fr->callno]->owner->lock);
							}
						} else {
							if (option_debug)
								ast_log(LOG_DEBUG, "Neat, somebody took away the channel at a magical time but i found it!\n");
							ast_mutex_unlock(&iaxsl[fr->callno]);
							return 1;
						}
					}
			}
		}
		if (f.frametype == AST_FRAME_VIDEO) {
			if (f.subclass != iaxs[fr->callno]->videoformat) {
				if (option_debug)
					ast_log(LOG_DEBUG, "Ooh, video format changed to %d\n", f.subclass & ~0x1);
				iaxs[fr->callno]->videoformat = f.subclass & ~0x1;
			}
		}
		if (f.frametype == AST_FRAME_IAX) {
			if (iaxs[fr->callno]->initid > -1) {
				/* Don't auto congest anymore since we've gotten something usefulb ack */
				ast_sched_del(sched, iaxs[fr->callno]->initid);
				iaxs[fr->callno]->initid = -1;
			}
			/* Handle the IAX pseudo frame itself */
			if (option_debug && iaxdebug)
				ast_log(LOG_DEBUG, "IAX subclass %d received\n", f.subclass);

                        /* Update last ts unless the frame's timestamp originated with us. */
			if (iaxs[fr->callno]->last < fr->ts &&
                            f.subclass != IAX_COMMAND_ACK &&
                            f.subclass != IAX_COMMAND_PONG &&
                            f.subclass != IAX_COMMAND_LAGRP) {
				iaxs[fr->callno]->last = fr->ts;
				if (option_debug && iaxdebug)
					ast_log(LOG_DEBUG, "For call=%d, set last=%d\n", fr->callno, fr->ts);
			}

			switch(f.subclass) {
			case IAX_COMMAND_ACK:
				/* Do nothing */
				break;
			case IAX_COMMAND_QUELCH:
				if (ast_test_flag(&iaxs[fr->callno]->state, IAX_STATE_STARTED)) {
				        /* Generate Manager Hold event, if necessary*/
					if (iaxs[fr->callno]->owner) {
						manager_event(EVENT_FLAG_CALL, "Hold",
							"Channel: %s\r\n"
							"Uniqueid: %s\r\n",
							iaxs[fr->callno]->owner->name, 
							iaxs[fr->callno]->owner->uniqueid);
					}

					ast_set_flag(iaxs[fr->callno], IAX_QUELCH);
					if (ies.musiconhold) {
						if (iaxs[fr->callno]->owner && ast_bridged_channel(iaxs[fr->callno]->owner)) {
							const char *mohsuggest = iaxs[fr->callno]->mohsuggest;
							ast_queue_control_data(iaxs[fr->callno]->owner, AST_CONTROL_HOLD, 
								S_OR(mohsuggest, NULL),
								!ast_strlen_zero(mohsuggest) ? strlen(mohsuggest) + 1 : 0);
						}
					}
				}
				break;
			case IAX_COMMAND_UNQUELCH:
				if (ast_test_flag(&iaxs[fr->callno]->state, IAX_STATE_STARTED)) {
				        /* Generate Manager Unhold event, if necessary*/
					if (iaxs[fr->callno]->owner && ast_test_flag(iaxs[fr->callno], IAX_QUELCH)) {
						manager_event(EVENT_FLAG_CALL, "Unhold",
							"Channel: %s\r\n"
							"Uniqueid: %s\r\n",
							iaxs[fr->callno]->owner->name, 
							iaxs[fr->callno]->owner->uniqueid);
					}

					ast_clear_flag(iaxs[fr->callno], IAX_QUELCH);
					if (iaxs[fr->callno]->owner && ast_bridged_channel(iaxs[fr->callno]->owner))
						ast_queue_control(iaxs[fr->callno]->owner, AST_CONTROL_UNHOLD);
				}
				break;
			case IAX_COMMAND_TXACC:
				if (iaxs[fr->callno]->transferring == TRANSFER_BEGIN) {
					/* Ack the packet with the given timestamp */
					AST_LIST_LOCK(&iaxq.queue);
					AST_LIST_TRAVERSE(&iaxq.queue, cur, list) {
						/* Cancel any outstanding txcnt's */
						if ((fr->callno == cur->callno) && (cur->transfer))
							cur->retries = -1;
					}
					AST_LIST_UNLOCK(&iaxq.queue);
					memset(&ied1, 0, sizeof(ied1));
					iax_ie_append_short(&ied1, IAX_IE_CALLNO, iaxs[fr->callno]->callno);
					send_command(iaxs[fr->callno], AST_FRAME_IAX, IAX_COMMAND_TXREADY, 0, ied1.buf, ied1.pos, -1);
					iaxs[fr->callno]->transferring = TRANSFER_READY;
				}
				break;
			case IAX_COMMAND_NEW:
				/* Ignore if it's already up */
				if (ast_test_flag(&iaxs[fr->callno]->state, IAX_STATE_STARTED | IAX_STATE_TBD))
					break;
				if (ies.provverpres && ies.serviceident && sin.sin_addr.s_addr)
					check_provisioning(&sin, fd, ies.serviceident, ies.provver);
				/* If we're in trunk mode, do it now, and update the trunk number in our frame before continuing */
				if (ast_test_flag(iaxs[fr->callno], IAX_TRUNK)) {
					fr->callno = make_trunk(fr->callno, 1);
				}
				/* For security, always ack immediately */
				if (delayreject)
					send_command_immediate(iaxs[fr->callno], AST_FRAME_IAX, IAX_COMMAND_ACK, fr->ts, NULL, 0,fr->iseqno);
				if (check_access(fr->callno, &sin, &ies)) {
					/* They're not allowed on */
					auth_fail(fr->callno, IAX_COMMAND_REJECT);
					if (authdebug)
						ast_log(LOG_NOTICE, "Rejected connect attempt from %s, who was trying to reach '%s@%s'\n", ast_inet_ntoa(sin.sin_addr), iaxs[fr->callno]->exten, iaxs[fr->callno]->context);
					break;
				}
				/* This might re-enter the IAX code and need the lock */
				if (strcasecmp(iaxs[fr->callno]->exten, "TBD")) {
					ast_mutex_unlock(&iaxsl[fr->callno]);
					exists = ast_exists_extension(NULL, iaxs[fr->callno]->context, iaxs[fr->callno]->exten, 1, iaxs[fr->callno]->cid_num);
					ast_mutex_lock(&iaxsl[fr->callno]);
				} else
					exists = 0;
				if (ast_strlen_zero(iaxs[fr->callno]->secret) && ast_strlen_zero(iaxs[fr->callno]->inkeys)) {
					if (strcmp(iaxs[fr->callno]->exten, "TBD") && !exists) {
						memset(&ied0, 0, sizeof(ied0));
						iax_ie_append_str(&ied0, IAX_IE_CAUSE, "No such context/extension");
						iax_ie_append_byte(&ied0, IAX_IE_CAUSECODE, AST_CAUSE_NO_ROUTE_DESTINATION);
						send_command_final(iaxs[fr->callno], AST_FRAME_IAX, IAX_COMMAND_REJECT, 0, ied0.buf, ied0.pos, -1);
						if (authdebug)
							ast_log(LOG_NOTICE, "Rejected connect attempt from %s, request '%s@%s' does not exist\n", ast_inet_ntoa(sin.sin_addr), iaxs[fr->callno]->exten, iaxs[fr->callno]->context);
					} else {
						/* Select an appropriate format */

						if(ast_test_flag(iaxs[fr->callno], IAX_CODEC_NOPREFS)) {
							if(ast_test_flag(iaxs[fr->callno], IAX_CODEC_NOCAP)) {
								using_prefs = "reqonly";
							} else {
								using_prefs = "disabled";
							}
							format = iaxs[fr->callno]->peerformat & iaxs[fr->callno]->capability;
							memset(&pref, 0, sizeof(pref));
							strcpy(caller_pref_buf, "disabled");
							strcpy(host_pref_buf, "disabled");
						} else {
							using_prefs = "mine";
							/* If the information elements are in here... use them */
							if (ies.codec_prefs)
								ast_codec_pref_convert(&iaxs[fr->callno]->rprefs, ies.codec_prefs, 32, 0);
							if (ast_codec_pref_index(&iaxs[fr->callno]->rprefs, 0)) {
								/* If we are codec_first_choice we let the caller have the 1st shot at picking the codec.*/
								if (ast_test_flag(iaxs[fr->callno], IAX_CODEC_USER_FIRST)) {
									pref = iaxs[fr->callno]->rprefs;
									using_prefs = "caller";
								} else {
									pref = iaxs[fr->callno]->prefs;
								}
							} else
								pref = iaxs[fr->callno]->prefs;
							
							format = ast_codec_choose(&pref, iaxs[fr->callno]->capability & iaxs[fr->callno]->peercapability, 0);
							ast_codec_pref_string(&iaxs[fr->callno]->rprefs, caller_pref_buf, sizeof(caller_pref_buf) - 1);
							ast_codec_pref_string(&iaxs[fr->callno]->prefs, host_pref_buf, sizeof(host_pref_buf) - 1);
						}
						if (!format) {
							if(!ast_test_flag(iaxs[fr->callno], IAX_CODEC_NOCAP))
								format = iaxs[fr->callno]->peercapability & iaxs[fr->callno]->capability;
							if (!format) {
								memset(&ied0, 0, sizeof(ied0));
								iax_ie_append_str(&ied0, IAX_IE_CAUSE, "Unable to negotiate codec");
								iax_ie_append_byte(&ied0, IAX_IE_CAUSECODE, AST_CAUSE_BEARERCAPABILITY_NOTAVAIL);
								send_command_final(iaxs[fr->callno], AST_FRAME_IAX, IAX_COMMAND_REJECT, 0, ied0.buf, ied0.pos, -1);
								if (authdebug) {
									if(ast_test_flag(iaxs[fr->callno], IAX_CODEC_NOCAP))
										ast_log(LOG_NOTICE, "Rejected connect attempt from %s, requested 0x%x incompatible with our capability 0x%x.\n", ast_inet_ntoa(sin.sin_addr), iaxs[fr->callno]->peerformat, iaxs[fr->callno]->capability);
									else 
										ast_log(LOG_NOTICE, "Rejected connect attempt from %s, requested/capability 0x%x/0x%x incompatible with our capability 0x%x.\n", ast_inet_ntoa(sin.sin_addr), iaxs[fr->callno]->peerformat, iaxs[fr->callno]->peercapability, iaxs[fr->callno]->capability);
								}
							} else {
								/* Pick one... */
								if(ast_test_flag(iaxs[fr->callno], IAX_CODEC_NOCAP)) {
									if(!(iaxs[fr->callno]->peerformat & iaxs[fr->callno]->capability))
										format = 0;
								} else {
									if(ast_test_flag(iaxs[fr->callno], IAX_CODEC_NOPREFS)) {
										using_prefs = ast_test_flag(iaxs[fr->callno], IAX_CODEC_NOCAP) ? "reqonly" : "disabled";
										memset(&pref, 0, sizeof(pref));
										format = ast_best_codec(iaxs[fr->callno]->peercapability & iaxs[fr->callno]->capability);
										strcpy(caller_pref_buf,"disabled");
										strcpy(host_pref_buf,"disabled");
									} else {
										using_prefs = "mine";
										if (ast_codec_pref_index(&iaxs[fr->callno]->rprefs, 0)) {
											/* Do the opposite of what we tried above. */
											if (ast_test_flag(iaxs[fr->callno], IAX_CODEC_USER_FIRST)) {
												pref = iaxs[fr->callno]->prefs;								
											} else {
												pref = iaxs[fr->callno]->rprefs;
												using_prefs = "caller";
											}
											format = ast_codec_choose(&pref, iaxs[fr->callno]->peercapability & iaxs[fr->callno]->capability, 1);
									
										} else /* if no codec_prefs IE do it the old way */
											format = ast_best_codec(iaxs[fr->callno]->peercapability & iaxs[fr->callno]->capability);	
									}
								}

								if (!format) {
									memset(&ied0, 0, sizeof(ied0));
									iax_ie_append_str(&ied0, IAX_IE_CAUSE, "Unable to negotiate codec");
									iax_ie_append_byte(&ied0, IAX_IE_CAUSECODE, AST_CAUSE_BEARERCAPABILITY_NOTAVAIL);
									ast_log(LOG_ERROR, "No best format in 0x%x???\n", iaxs[fr->callno]->peercapability & iaxs[fr->callno]->capability);
									send_command_final(iaxs[fr->callno], AST_FRAME_IAX, IAX_COMMAND_REJECT, 0, ied0.buf, ied0.pos, -1);
									if (authdebug)
										ast_log(LOG_NOTICE, "Rejected connect attempt from %s, requested/capability 0x%x/0x%x incompatible with our capability 0x%x.\n", ast_inet_ntoa(sin.sin_addr), iaxs[fr->callno]->peerformat, iaxs[fr->callno]->peercapability, iaxs[fr->callno]->capability);
									ast_set_flag(iaxs[fr->callno], IAX_ALREADYGONE);	
									break;
								}
							}
						}
						if (format) {
							/* No authentication required, let them in */
							memset(&ied1, 0, sizeof(ied1));
							iax_ie_append_int(&ied1, IAX_IE_FORMAT, format);
							send_command(iaxs[fr->callno], AST_FRAME_IAX, IAX_COMMAND_ACCEPT, 0, ied1.buf, ied1.pos, -1);
							if (strcmp(iaxs[fr->callno]->exten, "TBD")) {
								ast_set_flag(&iaxs[fr->callno]->state, IAX_STATE_STARTED);
								if (option_verbose > 2) 
									ast_verbose(VERBOSE_PREFIX_3 "Accepting UNAUTHENTICATED call from %s:\n"
												"%srequested format = %s,\n"
												"%srequested prefs = %s,\n"
												"%sactual format = %s,\n"
												"%shost prefs = %s,\n"
												"%spriority = %s\n",
												ast_inet_ntoa(sin.sin_addr), 
												VERBOSE_PREFIX_4,
												ast_getformatname(iaxs[fr->callno]->peerformat), 
												VERBOSE_PREFIX_4,
												caller_pref_buf,
												VERBOSE_PREFIX_4,
												ast_getformatname(format), 
												VERBOSE_PREFIX_4,
												host_pref_buf, 
												VERBOSE_PREFIX_4,
												using_prefs);
								
								if(!(c = ast_iax2_new(fr->callno, AST_STATE_RING, format)))
									iax2_destroy(fr->callno);
							} else {
								ast_set_flag(&iaxs[fr->callno]->state, IAX_STATE_TBD);
								/* If this is a TBD call, we're ready but now what...  */
								if (option_verbose > 2)
									ast_verbose(VERBOSE_PREFIX_3 "Accepted unauthenticated TBD call from %s\n", ast_inet_ntoa(sin.sin_addr));
							}
						}
					}
					break;
				}
				if (iaxs[fr->callno]->authmethods & IAX_AUTH_MD5)
					merge_encryption(iaxs[fr->callno],ies.encmethods);
				else
					iaxs[fr->callno]->encmethods = 0;
				if (!authenticate_request(iaxs[fr->callno]))
					ast_set_flag(&iaxs[fr->callno]->state, IAX_STATE_AUTHENTICATED);
				break;
			case IAX_COMMAND_DPREQ:
				/* Request status in the dialplan */
				if (ast_test_flag(&iaxs[fr->callno]->state, IAX_STATE_TBD) &&
					!ast_test_flag(&iaxs[fr->callno]->state, IAX_STATE_STARTED) && ies.called_number) {
					if (iaxcompat) {
						/* Spawn a thread for the lookup */
						spawn_dp_lookup(fr->callno, iaxs[fr->callno]->context, ies.called_number, iaxs[fr->callno]->cid_num);
					} else {
						/* Just look it up */
						dp_lookup(fr->callno, iaxs[fr->callno]->context, ies.called_number, iaxs[fr->callno]->cid_num, 1);
					}
				}
				break;
			case IAX_COMMAND_HANGUP:
				ast_set_flag(iaxs[fr->callno], IAX_ALREADYGONE);
				if (option_debug)
					ast_log(LOG_DEBUG, "Immediately destroying %d, having received hangup\n", fr->callno);
				/* Set hangup cause according to remote */
				if (ies.causecode && iaxs[fr->callno]->owner)
					iaxs[fr->callno]->owner->hangupcause = ies.causecode;
				/* Send ack immediately, before we destroy */
				send_command_immediate(iaxs[fr->callno], AST_FRAME_IAX, IAX_COMMAND_ACK, fr->ts, NULL, 0,fr->iseqno);
				iax2_destroy(fr->callno);
				break;
			case IAX_COMMAND_REJECT:
				/* Set hangup cause according to remote */
				if (ies.causecode && iaxs[fr->callno]->owner)
					iaxs[fr->callno]->owner->hangupcause = ies.causecode;

				if (!ast_test_flag(iaxs[fr->callno], IAX_PROVISION)) {
					if (iaxs[fr->callno]->owner && authdebug)
						ast_log(LOG_WARNING, "Call rejected by %s: %s\n",
							ast_inet_ntoa(iaxs[fr->callno]->addr.sin_addr),
							ies.cause ? ies.cause : "<Unknown>");
					if (option_debug)
						ast_log(LOG_DEBUG, "Immediately destroying %d, having received reject\n",
							fr->callno);
				}
				/* Send ack immediately, before we destroy */
				send_command_immediate(iaxs[fr->callno], AST_FRAME_IAX, IAX_COMMAND_ACK,
						       fr->ts, NULL, 0, fr->iseqno);
				if (!ast_test_flag(iaxs[fr->callno], IAX_PROVISION))
					iaxs[fr->callno]->error = EPERM;
				iax2_destroy(fr->callno);
				break;
			case IAX_COMMAND_TRANSFER:
				if (iaxs[fr->callno]->owner && ast_bridged_channel(iaxs[fr->callno]->owner) && ies.called_number) {
					/* Set BLINDTRANSFER channel variables */
					pbx_builtin_setvar_helper(iaxs[fr->callno]->owner, "BLINDTRANSFER", ast_bridged_channel(iaxs[fr->callno]->owner)->name);
					pbx_builtin_setvar_helper(ast_bridged_channel(iaxs[fr->callno]->owner), "BLINDTRANSFER", iaxs[fr->callno]->owner->name);
					if (!strcmp(ies.called_number, ast_parking_ext())) {
						if (iax_park(ast_bridged_channel(iaxs[fr->callno]->owner), iaxs[fr->callno]->owner)) {
							ast_log(LOG_WARNING, "Failed to park call on '%s'\n", ast_bridged_channel(iaxs[fr->callno]->owner)->name);
						} else if (ast_bridged_channel(iaxs[fr->callno]->owner)) {
							if (option_debug)
								ast_log(LOG_DEBUG, "Parked call on '%s'\n", ast_bridged_channel(iaxs[fr->callno]->owner)->name);
						}
					} else {
						if (ast_async_goto(ast_bridged_channel(iaxs[fr->callno]->owner), iaxs[fr->callno]->context, ies.called_number, 1))
							ast_log(LOG_WARNING, "Async goto of '%s' to '%s@%s' failed\n", ast_bridged_channel(iaxs[fr->callno]->owner)->name, 
								ies.called_number, iaxs[fr->callno]->context);
						else {
							if (option_debug)
								ast_log(LOG_DEBUG, "Async goto of '%s' to '%s@%s' started\n", ast_bridged_channel(iaxs[fr->callno]->owner)->name, 
									ies.called_number, iaxs[fr->callno]->context);
						}
					}
				} else {
					if (option_debug)
						ast_log(LOG_DEBUG, "Async goto not applicable on call %d\n", fr->callno);
				}
				break;
			case IAX_COMMAND_ACCEPT:
				/* Ignore if call is already up or needs authentication or is a TBD */
				if (ast_test_flag(&iaxs[fr->callno]->state, IAX_STATE_STARTED | IAX_STATE_TBD | IAX_STATE_AUTHENTICATED))
					break;
				if (ast_test_flag(iaxs[fr->callno], IAX_PROVISION)) {
					/* Send ack immediately, before we destroy */
					send_command_immediate(iaxs[fr->callno], AST_FRAME_IAX, IAX_COMMAND_ACK, fr->ts, NULL, 0,fr->iseqno);
					iax2_destroy(fr->callno);
					break;
				}
				if (ies.format) {
					iaxs[fr->callno]->peerformat = ies.format;
				} else {
					if (iaxs[fr->callno]->owner)
						iaxs[fr->callno]->peerformat = iaxs[fr->callno]->owner->nativeformats;
					else
						iaxs[fr->callno]->peerformat = iaxs[fr->callno]->capability;
				}
				if (option_verbose > 2)
					ast_verbose(VERBOSE_PREFIX_3 "Call accepted by %s (format %s)\n", ast_inet_ntoa(iaxs[fr->callno]->addr.sin_addr), ast_getformatname(iaxs[fr->callno]->peerformat));
				if (!(iaxs[fr->callno]->peerformat & iaxs[fr->callno]->capability)) {
					memset(&ied0, 0, sizeof(ied0));
					iax_ie_append_str(&ied0, IAX_IE_CAUSE, "Unable to negotiate codec");
					iax_ie_append_byte(&ied0, IAX_IE_CAUSECODE, AST_CAUSE_BEARERCAPABILITY_NOTAVAIL);
					send_command_final(iaxs[fr->callno], AST_FRAME_IAX, IAX_COMMAND_REJECT, 0, ied0.buf, ied0.pos, -1);
					if (authdebug)
						ast_log(LOG_NOTICE, "Rejected call to %s, format 0x%x incompatible with our capability 0x%x.\n", ast_inet_ntoa(sin.sin_addr), iaxs[fr->callno]->peerformat, iaxs[fr->callno]->capability);
				} else {
					ast_set_flag(&iaxs[fr->callno]->state, IAX_STATE_STARTED);
					if (iaxs[fr->callno]->owner) {
						/* Switch us to use a compatible format */
						iaxs[fr->callno]->owner->nativeformats = iaxs[fr->callno]->peerformat;
						if (option_verbose > 2)
							ast_verbose(VERBOSE_PREFIX_3 "Format for call is %s\n", ast_getformatname(iaxs[fr->callno]->owner->nativeformats));
retryowner2:
						if (ast_mutex_trylock(&iaxs[fr->callno]->owner->lock)) {
							ast_mutex_unlock(&iaxsl[fr->callno]);
							usleep(1);
							ast_mutex_lock(&iaxsl[fr->callno]);
							if (iaxs[fr->callno] && iaxs[fr->callno]->owner) goto retryowner2;
						}
						
						if (iaxs[fr->callno] && iaxs[fr->callno]->owner) {
							/* Setup read/write formats properly. */
							if (iaxs[fr->callno]->owner->writeformat)
								ast_set_write_format(iaxs[fr->callno]->owner, iaxs[fr->callno]->owner->writeformat);	
							if (iaxs[fr->callno]->owner->readformat)
								ast_set_read_format(iaxs[fr->callno]->owner, iaxs[fr->callno]->owner->readformat);	
							ast_mutex_unlock(&iaxs[fr->callno]->owner->lock);
						}
					}
				}
				AST_LIST_LOCK(&dpcache);
				AST_LIST_TRAVERSE(&iaxs[fr->callno]->dpentries, dp, peer_list)
					if (!(dp->flags & CACHE_FLAG_TRANSMITTED))
						iax2_dprequest(dp, fr->callno);
				AST_LIST_UNLOCK(&dpcache);
				break;
			case IAX_COMMAND_POKE:
				/* Send back a pong packet with the original timestamp */
				send_command_final(iaxs[fr->callno], AST_FRAME_IAX, IAX_COMMAND_PONG, fr->ts, NULL, 0, -1);
				break;
			case IAX_COMMAND_PING:
			{
				struct iax_ie_data pingied;
				construct_rr(iaxs[fr->callno], &pingied);
				/* Send back a pong packet with the original timestamp */
				send_command(iaxs[fr->callno], AST_FRAME_IAX, IAX_COMMAND_PONG, fr->ts, pingied.buf, pingied.pos, -1);
			}
				break;
			case IAX_COMMAND_PONG:
				/* Calculate ping time */
				iaxs[fr->callno]->pingtime =  calc_timestamp(iaxs[fr->callno], 0, &f) - fr->ts;
				/* save RR info */
				save_rr(fr, &ies);

				if (iaxs[fr->callno]->peerpoke) {
					peer = iaxs[fr->callno]->peerpoke;
					if ((peer->lastms < 0)  || (peer->historicms > peer->maxms)) {
						if (iaxs[fr->callno]->pingtime <= peer->maxms) {
							ast_log(LOG_NOTICE, "Peer '%s' is now REACHABLE! Time: %d\n", peer->name, iaxs[fr->callno]->pingtime);
							manager_event(EVENT_FLAG_SYSTEM, "PeerStatus", "Peer: IAX2/%s\r\nPeerStatus: Reachable\r\nTime: %d\r\n", peer->name, iaxs[fr->callno]->pingtime); 
							ast_device_state_changed("IAX2/%s", peer->name); /* Activate notification */
						}
					} else if ((peer->historicms > 0) && (peer->historicms <= peer->maxms)) {
						if (iaxs[fr->callno]->pingtime > peer->maxms) {
							ast_log(LOG_NOTICE, "Peer '%s' is now TOO LAGGED (%d ms)!\n", peer->name, iaxs[fr->callno]->pingtime);
							manager_event(EVENT_FLAG_SYSTEM, "PeerStatus", "Peer: IAX2/%s\r\nPeerStatus: Lagged\r\nTime: %d\r\n", peer->name, iaxs[fr->callno]->pingtime); 
							ast_device_state_changed("IAX2/%s", peer->name); /* Activate notification */
						}
					}
					peer->lastms = iaxs[fr->callno]->pingtime;
					if (peer->smoothing && (peer->lastms > -1))
						peer->historicms = (iaxs[fr->callno]->pingtime + peer->historicms) / 2;
					else if (peer->smoothing && peer->lastms < 0)
						peer->historicms = (0 + peer->historicms) / 2;
					else					
						peer->historicms = iaxs[fr->callno]->pingtime;

					/* Remove scheduled iax2_poke_noanswer */
					if (peer->pokeexpire > -1)
						ast_sched_del(sched, peer->pokeexpire);
					/* Schedule the next cycle */
					if ((peer->lastms < 0)  || (peer->historicms > peer->maxms)) 
						peer->pokeexpire = ast_sched_add(sched, peer->pokefreqnotok, iax2_poke_peer_s, peer);
					else
						peer->pokeexpire = ast_sched_add(sched, peer->pokefreqok, iax2_poke_peer_s, peer);
					/* and finally send the ack */
					send_command_immediate(iaxs[fr->callno], AST_FRAME_IAX, IAX_COMMAND_ACK, fr->ts, NULL, 0,fr->iseqno);
					/* And wrap up the qualify call */
					iax2_destroy(fr->callno);
					peer->callno = 0;
					if (option_debug)
						ast_log(LOG_DEBUG, "Peer %s: got pong, lastms %d, historicms %d, maxms %d\n", peer->name, peer->lastms, peer->historicms, peer->maxms);
				}
				break;
			case IAX_COMMAND_LAGRQ:
			case IAX_COMMAND_LAGRP:
				f.src = "LAGRQ";
				f.mallocd = 0;
				f.offset = 0;
				f.samples = 0;
				iax_frame_wrap(fr, &f);
				if(f.subclass == IAX_COMMAND_LAGRQ) {
					/* Received a LAGRQ - echo back a LAGRP */
					fr->af.subclass = IAX_COMMAND_LAGRP;
					iax2_send(iaxs[fr->callno], &fr->af, fr->ts, -1, 0, 0, 0);
				} else {
					/* Received LAGRP in response to our LAGRQ */
					unsigned int ts;
					/* This is a reply we've been given, actually measure the difference */
					ts = calc_timestamp(iaxs[fr->callno], 0, &fr->af);
					iaxs[fr->callno]->lag = ts - fr->ts;
					if (option_debug && iaxdebug)
						ast_log(LOG_DEBUG, "Peer %s lag measured as %dms\n",
							ast_inet_ntoa(iaxs[fr->callno]->addr.sin_addr), iaxs[fr->callno]->lag);
				}
				break;
			case IAX_COMMAND_AUTHREQ:
				if (ast_test_flag(&iaxs[fr->callno]->state, IAX_STATE_STARTED | IAX_STATE_TBD)) {
					ast_log(LOG_WARNING, "Call on %s is already up, can't start on it\n", iaxs[fr->callno]->owner ? iaxs[fr->callno]->owner->name : "<Unknown>");
					break;
				}
				if (authenticate_reply(iaxs[fr->callno], &iaxs[fr->callno]->addr, &ies, iaxs[fr->callno]->secret, iaxs[fr->callno]->outkey)) {
					ast_log(LOG_WARNING, 
						"I don't know how to authenticate %s to %s\n", 
						ies.username ? ies.username : "<unknown>", ast_inet_ntoa(iaxs[fr->callno]->addr.sin_addr));
				}
				break;
			case IAX_COMMAND_AUTHREP:
				/* For security, always ack immediately */
				if (delayreject)
					send_command_immediate(iaxs[fr->callno], AST_FRAME_IAX, IAX_COMMAND_ACK, fr->ts, NULL, 0,fr->iseqno);
				/* Ignore once we've started */
				if (ast_test_flag(&iaxs[fr->callno]->state, IAX_STATE_STARTED | IAX_STATE_TBD)) {
					ast_log(LOG_WARNING, "Call on %s is already up, can't start on it\n", iaxs[fr->callno]->owner ? iaxs[fr->callno]->owner->name : "<Unknown>");
					break;
				}
				if (authenticate_verify(iaxs[fr->callno], &ies)) {
					if (authdebug)
						ast_log(LOG_NOTICE, "Host %s failed to authenticate as %s\n", ast_inet_ntoa(iaxs[fr->callno]->addr.sin_addr), iaxs[fr->callno]->username);
					memset(&ied0, 0, sizeof(ied0));
					auth_fail(fr->callno, IAX_COMMAND_REJECT);
					break;
				}
				if (strcasecmp(iaxs[fr->callno]->exten, "TBD")) {
					/* This might re-enter the IAX code and need the lock */
					exists = ast_exists_extension(NULL, iaxs[fr->callno]->context, iaxs[fr->callno]->exten, 1, iaxs[fr->callno]->cid_num);
				} else
					exists = 0;
				if (strcmp(iaxs[fr->callno]->exten, "TBD") && !exists) {
					if (authdebug)
						ast_log(LOG_NOTICE, "Rejected connect attempt from %s, request '%s@%s' does not exist\n", ast_inet_ntoa(sin.sin_addr), iaxs[fr->callno]->exten, iaxs[fr->callno]->context);
					memset(&ied0, 0, sizeof(ied0));
					iax_ie_append_str(&ied0, IAX_IE_CAUSE, "No such context/extension");
					iax_ie_append_byte(&ied0, IAX_IE_CAUSECODE, AST_CAUSE_NO_ROUTE_DESTINATION);
					send_command_final(iaxs[fr->callno], AST_FRAME_IAX, IAX_COMMAND_REJECT, 0, ied0.buf, ied0.pos, -1);
				} else {
					/* Select an appropriate format */
					if(ast_test_flag(iaxs[fr->callno], IAX_CODEC_NOPREFS)) {
						if(ast_test_flag(iaxs[fr->callno], IAX_CODEC_NOCAP)) {
							using_prefs = "reqonly";
						} else {
							using_prefs = "disabled";
						}
						format = iaxs[fr->callno]->peerformat & iaxs[fr->callno]->capability;
						memset(&pref, 0, sizeof(pref));
						strcpy(caller_pref_buf, "disabled");
						strcpy(host_pref_buf, "disabled");
					} else {
						using_prefs = "mine";
						if (ies.codec_prefs)
							ast_codec_pref_convert(&iaxs[fr->callno]->rprefs, ies.codec_prefs, 32, 0);
						if (ast_codec_pref_index(&iaxs[fr->callno]->rprefs, 0)) {
							if (ast_test_flag(iaxs[fr->callno], IAX_CODEC_USER_FIRST)) {
								pref = iaxs[fr->callno]->rprefs;
								using_prefs = "caller";
							} else {
								pref = iaxs[fr->callno]->prefs;
							}
						} else /* if no codec_prefs IE do it the old way */
							pref = iaxs[fr->callno]->prefs;
					
						format = ast_codec_choose(&pref, iaxs[fr->callno]->capability & iaxs[fr->callno]->peercapability, 0);
						ast_codec_pref_string(&iaxs[fr->callno]->rprefs, caller_pref_buf, sizeof(caller_pref_buf) - 1);
						ast_codec_pref_string(&iaxs[fr->callno]->prefs, host_pref_buf, sizeof(host_pref_buf) - 1);
					}
					if (!format) {
						if(!ast_test_flag(iaxs[fr->callno], IAX_CODEC_NOCAP)) {
							if (option_debug)
								ast_log(LOG_DEBUG, "We don't do requested format %s, falling back to peer capability %d\n", ast_getformatname(iaxs[fr->callno]->peerformat), iaxs[fr->callno]->peercapability);
							format = iaxs[fr->callno]->peercapability & iaxs[fr->callno]->capability;
						}
						if (!format) {
							if (authdebug) {
								if(ast_test_flag(iaxs[fr->callno], IAX_CODEC_NOCAP)) 
									ast_log(LOG_NOTICE, "Rejected connect attempt from %s, requested 0x%x incompatible with our capability 0x%x.\n", ast_inet_ntoa(sin.sin_addr), iaxs[fr->callno]->peerformat, iaxs[fr->callno]->capability);
								else
									ast_log(LOG_NOTICE, "Rejected connect attempt from %s, requested/capability 0x%x/0x%x incompatible with our capability 0x%x.\n", ast_inet_ntoa(sin.sin_addr), iaxs[fr->callno]->peerformat, iaxs[fr->callno]->peercapability, iaxs[fr->callno]->capability);
							}
							memset(&ied0, 0, sizeof(ied0));
							iax_ie_append_str(&ied0, IAX_IE_CAUSE, "Unable to negotiate codec");
							iax_ie_append_byte(&ied0, IAX_IE_CAUSECODE, AST_CAUSE_BEARERCAPABILITY_NOTAVAIL);
							send_command_final(iaxs[fr->callno], AST_FRAME_IAX, IAX_COMMAND_REJECT, 0, ied0.buf, ied0.pos, -1);
						} else {
							/* Pick one... */
							if(ast_test_flag(iaxs[fr->callno], IAX_CODEC_NOCAP)) {
								if(!(iaxs[fr->callno]->peerformat & iaxs[fr->callno]->capability))
									format = 0;
							} else {
								if(ast_test_flag(iaxs[fr->callno], IAX_CODEC_NOPREFS)) {
									using_prefs = ast_test_flag(iaxs[fr->callno], IAX_CODEC_NOCAP) ? "reqonly" : "disabled";
									memset(&pref, 0, sizeof(pref));
									format = ast_test_flag(iaxs[fr->callno], IAX_CODEC_NOCAP) ?
										iaxs[fr->callno]->peerformat : ast_best_codec(iaxs[fr->callno]->peercapability & iaxs[fr->callno]->capability);
									strcpy(caller_pref_buf,"disabled");
									strcpy(host_pref_buf,"disabled");
								} else {
									using_prefs = "mine";
									if (ast_codec_pref_index(&iaxs[fr->callno]->rprefs, 0)) {
										/* Do the opposite of what we tried above. */
										if (ast_test_flag(iaxs[fr->callno], IAX_CODEC_USER_FIRST)) {
											pref = iaxs[fr->callno]->prefs;						
										} else {
											pref = iaxs[fr->callno]->rprefs;
											using_prefs = "caller";
										}
										format = ast_codec_choose(&pref, iaxs[fr->callno]->peercapability & iaxs[fr->callno]->capability, 1);
									} else /* if no codec_prefs IE do it the old way */
										format = ast_best_codec(iaxs[fr->callno]->peercapability & iaxs[fr->callno]->capability);	
								}
							}
							if (!format) {
								ast_log(LOG_ERROR, "No best format in 0x%x???\n", iaxs[fr->callno]->peercapability & iaxs[fr->callno]->capability);
								if (authdebug) {
									if(ast_test_flag(iaxs[fr->callno], IAX_CODEC_NOCAP))
										ast_log(LOG_NOTICE, "Rejected connect attempt from %s, requested 0x%x incompatible with our capability 0x%x.\n", ast_inet_ntoa(sin.sin_addr), iaxs[fr->callno]->peerformat, iaxs[fr->callno]->capability);
									else
										ast_log(LOG_NOTICE, "Rejected connect attempt from %s, requested/capability 0x%x/0x%x incompatible with our capability 0x%x.\n", ast_inet_ntoa(sin.sin_addr), iaxs[fr->callno]->peerformat, iaxs[fr->callno]->peercapability, iaxs[fr->callno]->capability);
								}
								memset(&ied0, 0, sizeof(ied0));
								iax_ie_append_str(&ied0, IAX_IE_CAUSE, "Unable to negotiate codec");
								iax_ie_append_byte(&ied0, IAX_IE_CAUSECODE, AST_CAUSE_BEARERCAPABILITY_NOTAVAIL);
								send_command_final(iaxs[fr->callno], AST_FRAME_IAX, IAX_COMMAND_REJECT, 0, ied0.buf, ied0.pos, -1);
							}
						}
					}
					if (format) {
						/* Authentication received */
						memset(&ied1, 0, sizeof(ied1));
						iax_ie_append_int(&ied1, IAX_IE_FORMAT, format);
						send_command(iaxs[fr->callno], AST_FRAME_IAX, IAX_COMMAND_ACCEPT, 0, ied1.buf, ied1.pos, -1);
						if (strcmp(iaxs[fr->callno]->exten, "TBD")) {
							ast_set_flag(&iaxs[fr->callno]->state, IAX_STATE_STARTED);
							if (option_verbose > 2) 
								ast_verbose(VERBOSE_PREFIX_3 "Accepting AUTHENTICATED call from %s:\n"
											"%srequested format = %s,\n"
											"%srequested prefs = %s,\n"
											"%sactual format = %s,\n"
											"%shost prefs = %s,\n"
											"%spriority = %s\n", 
											ast_inet_ntoa(sin.sin_addr), 
											VERBOSE_PREFIX_4,
											ast_getformatname(iaxs[fr->callno]->peerformat),
											VERBOSE_PREFIX_4,
											caller_pref_buf,
											VERBOSE_PREFIX_4,
											ast_getformatname(format),
											VERBOSE_PREFIX_4,
											host_pref_buf,
											VERBOSE_PREFIX_4,
											using_prefs);

							ast_set_flag(&iaxs[fr->callno]->state, IAX_STATE_STARTED);
							if(!(c = ast_iax2_new(fr->callno, AST_STATE_RING, format)))
								iax2_destroy(fr->callno);
						} else {
							ast_set_flag(&iaxs[fr->callno]->state, IAX_STATE_TBD);
							/* If this is a TBD call, we're ready but now what...  */
							if (option_verbose > 2)
								ast_verbose(VERBOSE_PREFIX_3 "Accepted AUTHENTICATED TBD call from %s\n", ast_inet_ntoa(sin.sin_addr));
						}
					}
				}
				break;
			case IAX_COMMAND_DIAL:
				if (ast_test_flag(&iaxs[fr->callno]->state, IAX_STATE_TBD)) {
					ast_clear_flag(&iaxs[fr->callno]->state, IAX_STATE_TBD);
					ast_string_field_set(iaxs[fr->callno], exten, ies.called_number ? ies.called_number : "s");
					if (!ast_exists_extension(NULL, iaxs[fr->callno]->context, iaxs[fr->callno]->exten, 1, iaxs[fr->callno]->cid_num)) {
						if (authdebug)
							ast_log(LOG_NOTICE, "Rejected dial attempt from %s, request '%s@%s' does not exist\n", ast_inet_ntoa(sin.sin_addr), iaxs[fr->callno]->exten, iaxs[fr->callno]->context);
						memset(&ied0, 0, sizeof(ied0));
						iax_ie_append_str(&ied0, IAX_IE_CAUSE, "No such context/extension");
						iax_ie_append_byte(&ied0, IAX_IE_CAUSECODE, AST_CAUSE_NO_ROUTE_DESTINATION);
						send_command_final(iaxs[fr->callno], AST_FRAME_IAX, IAX_COMMAND_REJECT, 0, ied0.buf, ied0.pos, -1);
					} else {
						ast_set_flag(&iaxs[fr->callno]->state, IAX_STATE_STARTED);
						if (option_verbose > 2) 
							ast_verbose(VERBOSE_PREFIX_3 "Accepting DIAL from %s, formats = 0x%x\n", ast_inet_ntoa(sin.sin_addr), iaxs[fr->callno]->peerformat);
						ast_set_flag(&iaxs[fr->callno]->state, IAX_STATE_STARTED);
						send_command(iaxs[fr->callno], AST_FRAME_CONTROL, AST_CONTROL_PROGRESS, 0, NULL, 0, -1);
						if(!(c = ast_iax2_new(fr->callno, AST_STATE_RING, iaxs[fr->callno]->peerformat)))
							iax2_destroy(fr->callno);
					}
				}
				break;
			case IAX_COMMAND_INVAL:
				iaxs[fr->callno]->error = ENOTCONN;
				if (option_debug)
					ast_log(LOG_DEBUG, "Immediately destroying %d, having received INVAL\n", fr->callno);
				iax2_destroy(fr->callno);
				if (option_debug)
					ast_log(LOG_DEBUG, "Destroying call %d\n", fr->callno);
				break;
			case IAX_COMMAND_VNAK:
				if (option_debug)
					ast_log(LOG_DEBUG, "Received VNAK: resending outstanding frames\n");
				/* Force retransmission */
				vnak_retransmit(fr->callno, fr->iseqno);
				break;
			case IAX_COMMAND_REGREQ:
			case IAX_COMMAND_REGREL:
				/* For security, always ack immediately */
				if (delayreject)
					send_command_immediate(iaxs[fr->callno], AST_FRAME_IAX, IAX_COMMAND_ACK, fr->ts, NULL, 0,fr->iseqno);
				if (register_verify(fr->callno, &sin, &ies)) {
					/* Send delayed failure */
					auth_fail(fr->callno, IAX_COMMAND_REGREJ);
					break;
				}
				if ((ast_strlen_zero(iaxs[fr->callno]->secret) && ast_strlen_zero(iaxs[fr->callno]->inkeys)) || 
						ast_test_flag(&iaxs[fr->callno]->state, IAX_STATE_AUTHENTICATED | IAX_STATE_UNCHANGED)) {
					if (f.subclass == IAX_COMMAND_REGREL)
						memset(&sin, 0, sizeof(sin));
					if (update_registry(iaxs[fr->callno]->peer, &sin, fr->callno, ies.devicetype, fd, ies.refresh))
						ast_log(LOG_WARNING, "Registry error\n");
					if (ies.provverpres && ies.serviceident && sin.sin_addr.s_addr)
						check_provisioning(&sin, fd, ies.serviceident, ies.provver);
					break;
				}
				registry_authrequest(iaxs[fr->callno]->peer, fr->callno);
				break;
			case IAX_COMMAND_REGACK:
				if (iax2_ack_registry(&ies, &sin, fr->callno)) 
					ast_log(LOG_WARNING, "Registration failure\n");
				/* Send ack immediately, before we destroy */
				send_command_immediate(iaxs[fr->callno], AST_FRAME_IAX, IAX_COMMAND_ACK, fr->ts, NULL, 0,fr->iseqno);
				iax2_destroy(fr->callno);
				break;
			case IAX_COMMAND_REGREJ:
				if (iaxs[fr->callno]->reg) {
					if (authdebug) {
						ast_log(LOG_NOTICE, "Registration of '%s' rejected: '%s' from: '%s'\n", iaxs[fr->callno]->reg->username, ies.cause ? ies.cause : "<unknown>", ast_inet_ntoa(sin.sin_addr));
						manager_event(EVENT_FLAG_SYSTEM, "Registry", "ChannelDriver: IAX2\r\nUsername: %s\r\nStatus: Rejected\r\nCause: %s\r\n", iaxs[fr->callno]->reg->username, ies.cause ? ies.cause : "<unknown>");
					}
					iaxs[fr->callno]->reg->regstate = REG_STATE_REJECTED;
				}
				/* Send ack immediately, before we destroy */
				send_command_immediate(iaxs[fr->callno], AST_FRAME_IAX, IAX_COMMAND_ACK, fr->ts, NULL, 0,fr->iseqno);
				iax2_destroy(fr->callno);
				break;
			case IAX_COMMAND_REGAUTH:
				/* Authentication request */
				if (registry_rerequest(&ies, fr->callno, &sin)) {
					memset(&ied0, 0, sizeof(ied0));
					iax_ie_append_str(&ied0, IAX_IE_CAUSE, "No authority found");
					iax_ie_append_byte(&ied0, IAX_IE_CAUSECODE, AST_CAUSE_FACILITY_NOT_SUBSCRIBED);
					send_command_final(iaxs[fr->callno], AST_FRAME_IAX, IAX_COMMAND_REJECT, 0, ied0.buf, ied0.pos, -1);
				}
				break;
			case IAX_COMMAND_TXREJ:
				iaxs[fr->callno]->transferring = 0;
				if (option_verbose > 2) 
					ast_verbose(VERBOSE_PREFIX_3 "Channel '%s' unable to transfer\n", iaxs[fr->callno]->owner ? iaxs[fr->callno]->owner->name : "<Unknown>");
				memset(&iaxs[fr->callno]->transfer, 0, sizeof(iaxs[fr->callno]->transfer));
				if (iaxs[fr->callno]->bridgecallno) {
					if (iaxs[iaxs[fr->callno]->bridgecallno]->transferring) {
						iaxs[iaxs[fr->callno]->bridgecallno]->transferring = 0;
						send_command(iaxs[iaxs[fr->callno]->bridgecallno], AST_FRAME_IAX, IAX_COMMAND_TXREJ, 0, NULL, 0, -1);
					}
				}
				break;
			case IAX_COMMAND_TXREADY:
				if ((iaxs[fr->callno]->transferring == TRANSFER_BEGIN) ||
				    (iaxs[fr->callno]->transferring == TRANSFER_MBEGIN)) {
					if (iaxs[fr->callno]->transferring == TRANSFER_MBEGIN)
						iaxs[fr->callno]->transferring = TRANSFER_MREADY;
					else
						iaxs[fr->callno]->transferring = TRANSFER_READY;
					if (option_verbose > 2) 
						ast_verbose(VERBOSE_PREFIX_3 "Channel '%s' ready to transfer\n", iaxs[fr->callno]->owner ? iaxs[fr->callno]->owner->name : "<Unknown>");
					if (iaxs[fr->callno]->bridgecallno) {
						if ((iaxs[iaxs[fr->callno]->bridgecallno]->transferring == TRANSFER_READY) ||
						    (iaxs[iaxs[fr->callno]->bridgecallno]->transferring == TRANSFER_MREADY)) {
							/* They're both ready, now release them. */
							if (iaxs[fr->callno]->transferring == TRANSFER_MREADY) {
								if (option_verbose > 2) 
									ast_verbose(VERBOSE_PREFIX_3 "Attempting media bridge of %s and %s\n", iaxs[fr->callno]->owner ? iaxs[fr->callno]->owner->name : "<Unknown>",
										iaxs[iaxs[fr->callno]->bridgecallno]->owner ? iaxs[iaxs[fr->callno]->bridgecallno]->owner->name : "<Unknown>");

								iaxs[iaxs[fr->callno]->bridgecallno]->transferring = TRANSFER_MEDIA;
								iaxs[fr->callno]->transferring = TRANSFER_MEDIA;

								memset(&ied0, 0, sizeof(ied0));
								memset(&ied1, 0, sizeof(ied1));
								iax_ie_append_short(&ied0, IAX_IE_CALLNO, iaxs[iaxs[fr->callno]->bridgecallno]->peercallno);
								iax_ie_append_short(&ied1, IAX_IE_CALLNO, iaxs[fr->callno]->peercallno);
								send_command(iaxs[fr->callno], AST_FRAME_IAX, IAX_COMMAND_TXMEDIA, 0, ied0.buf, ied0.pos, -1);
								send_command(iaxs[iaxs[fr->callno]->bridgecallno], AST_FRAME_IAX, IAX_COMMAND_TXMEDIA, 0, ied1.buf, ied1.pos, -1);
							} else {
								if (option_verbose > 2) 
									ast_verbose(VERBOSE_PREFIX_3 "Releasing %s and %s\n", iaxs[fr->callno]->owner ? iaxs[fr->callno]->owner->name : "<Unknown>",
										iaxs[iaxs[fr->callno]->bridgecallno]->owner ? iaxs[iaxs[fr->callno]->bridgecallno]->owner->name : "<Unknown>");

								iaxs[iaxs[fr->callno]->bridgecallno]->transferring = TRANSFER_RELEASED;
								iaxs[fr->callno]->transferring = TRANSFER_RELEASED;
								ast_set_flag(iaxs[iaxs[fr->callno]->bridgecallno], IAX_ALREADYGONE);
								ast_set_flag(iaxs[fr->callno], IAX_ALREADYGONE);

								/* Stop doing lag & ping requests */
								stop_stuff(fr->callno);
								stop_stuff(iaxs[fr->callno]->bridgecallno);

								memset(&ied0, 0, sizeof(ied0));
								memset(&ied1, 0, sizeof(ied1));
								iax_ie_append_short(&ied0, IAX_IE_CALLNO, iaxs[iaxs[fr->callno]->bridgecallno]->peercallno);
								iax_ie_append_short(&ied1, IAX_IE_CALLNO, iaxs[fr->callno]->peercallno);
								send_command(iaxs[fr->callno], AST_FRAME_IAX, IAX_COMMAND_TXREL, 0, ied0.buf, ied0.pos, -1);
								send_command(iaxs[iaxs[fr->callno]->bridgecallno], AST_FRAME_IAX, IAX_COMMAND_TXREL, 0, ied1.buf, ied1.pos, -1);
							}

						}
					}
				}
				break;
			case IAX_COMMAND_TXREQ:
				try_transfer(iaxs[fr->callno], &ies);
				break;
			case IAX_COMMAND_TXCNT:
				if (iaxs[fr->callno]->transferring)
					send_command_transfer(iaxs[fr->callno], AST_FRAME_IAX, IAX_COMMAND_TXACC, 0, NULL, 0);
				break;
			case IAX_COMMAND_TXREL:
				/* Send ack immediately, rather than waiting until we've changed addresses */
				send_command_immediate(iaxs[fr->callno], AST_FRAME_IAX, IAX_COMMAND_ACK, fr->ts, NULL, 0,fr->iseqno);
				complete_transfer(fr->callno, &ies);
				stop_stuff(fr->callno);	/* for attended transfer to work with libiax */
				break;	
			case IAX_COMMAND_TXMEDIA:
				if (iaxs[fr->callno]->transferring == TRANSFER_READY) {
					/* Start sending our media to the transfer address, but otherwise leave the call as-is */
					iaxs[fr->callno]->transferring = TRANSFER_MEDIAPASS;
				}
				break;	
			case IAX_COMMAND_DPREP:
				complete_dpreply(iaxs[fr->callno], &ies);
				break;
			case IAX_COMMAND_UNSUPPORT:
				ast_log(LOG_NOTICE, "Peer did not understand our iax command '%d'\n", ies.iax_unknown);
				break;
			case IAX_COMMAND_FWDOWNL:
				/* Firmware download */
				memset(&ied0, 0, sizeof(ied0));
				res = iax_firmware_append(&ied0, (unsigned char *)ies.devicetype, ies.fwdesc);
				if (res < 0)
					send_command_final(iaxs[fr->callno], AST_FRAME_IAX, IAX_COMMAND_REJECT, 0, ied0.buf, ied0.pos, -1);
				else if (res > 0)
					send_command_final(iaxs[fr->callno], AST_FRAME_IAX, IAX_COMMAND_FWDATA, 0, ied0.buf, ied0.pos, -1);
				else
					send_command(iaxs[fr->callno], AST_FRAME_IAX, IAX_COMMAND_FWDATA, 0, ied0.buf, ied0.pos, -1);
				break;
			default:
				if (option_debug)
					ast_log(LOG_DEBUG, "Unknown IAX command %d on %d/%d\n", f.subclass, fr->callno, iaxs[fr->callno]->peercallno);
				memset(&ied0, 0, sizeof(ied0));
				iax_ie_append_byte(&ied0, IAX_IE_IAX_UNKNOWN, f.subclass);
				send_command(iaxs[fr->callno], AST_FRAME_IAX, IAX_COMMAND_UNSUPPORT, 0, ied0.buf, ied0.pos, -1);
			}
			/* Don't actually pass these frames along */
			if ((f.subclass != IAX_COMMAND_ACK) && 
			  (f.subclass != IAX_COMMAND_TXCNT) && 
			  (f.subclass != IAX_COMMAND_TXACC) && 
			  (f.subclass != IAX_COMMAND_INVAL) &&
			  (f.subclass != IAX_COMMAND_VNAK)) { 
			  	if (iaxs[fr->callno] && iaxs[fr->callno]->aseqno != iaxs[fr->callno]->iseqno)
					send_command_immediate(iaxs[fr->callno], AST_FRAME_IAX, IAX_COMMAND_ACK, fr->ts, NULL, 0,fr->iseqno);
			}
			ast_mutex_unlock(&iaxsl[fr->callno]);
			return 1;
		}
		/* Unless this is an ACK or INVAL frame, ack it */
		if (iaxs[fr->callno]->aseqno != iaxs[fr->callno]->iseqno)
			send_command_immediate(iaxs[fr->callno], AST_FRAME_IAX, IAX_COMMAND_ACK, fr->ts, NULL, 0,fr->iseqno);
	} else if (minivid) {
		f.frametype = AST_FRAME_VIDEO;
		if (iaxs[fr->callno]->videoformat > 0) 
			f.subclass = iaxs[fr->callno]->videoformat | (ntohs(vh->ts) & 0x8000 ? 1 : 0);
		else {
			ast_log(LOG_WARNING, "Received mini frame before first full video frame\n ");
			iax2_vnak(fr->callno);
			ast_mutex_unlock(&iaxsl[fr->callno]);
			return 1;
		}
		f.datalen = res - sizeof(*vh);
		if (f.datalen)
			f.data = thread->buf + sizeof(*vh);
		else
			f.data = NULL;
#ifdef IAXTESTS
		if (test_resync) {
			fr->ts = (iaxs[fr->callno]->last & 0xFFFF8000L) | ((ntohs(mh->ts) + test_resync) & 0x7fff);
		} else
#endif /* IAXTESTS */
		fr->ts = (iaxs[fr->callno]->last & 0xFFFF8000L) | (ntohs(mh->ts) & 0x7fff);
	} else {
		/* A mini frame */
		f.frametype = AST_FRAME_VOICE;
		if (iaxs[fr->callno]->voiceformat > 0)
			f.subclass = iaxs[fr->callno]->voiceformat;
		else {
			ast_log(LOG_WARNING, "Received mini frame before first full voice frame\n ");
			iax2_vnak(fr->callno);
			ast_mutex_unlock(&iaxsl[fr->callno]);
			return 1;
		}
		f.datalen = res - sizeof(struct ast_iax2_mini_hdr);
		if (f.datalen < 0) {
			ast_log(LOG_WARNING, "Datalen < 0?\n");
			ast_mutex_unlock(&iaxsl[fr->callno]);
			return 1;
		}
		if (f.datalen)
			f.data = thread->buf + sizeof(*mh);
		else
			f.data = NULL;
#ifdef IAXTESTS
		if (test_resync) {
			fr->ts = (iaxs[fr->callno]->last & 0xFFFF0000L) | ((ntohs(mh->ts) + test_resync) & 0xffff);
		} else
#endif /* IAXTESTS */
		fr->ts = (iaxs[fr->callno]->last & 0xFFFF0000L) | ntohs(mh->ts);
		/* FIXME? Surely right here would be the right place to undo timestamp wraparound? */
	}
	/* Don't pass any packets until we're started */
	if (!ast_test_flag(&iaxs[fr->callno]->state, IAX_STATE_STARTED)) {
		ast_mutex_unlock(&iaxsl[fr->callno]);
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
	iax_frame_wrap(fr, &f);

	/* If this is our most recent packet, use it as our basis for timestamping */
	if (iaxs[fr->callno]->last < fr->ts) {
		/*iaxs[fr->callno]->last = fr->ts; (do it afterwards cos schedule/forward_delivery needs the last ts too)*/
		fr->outoforder = 0;
	} else {
		if (option_debug && iaxdebug)
			ast_log(LOG_DEBUG, "Received out of order packet... (type=%d, subclass %d, ts = %d, last = %d)\n", f.frametype, f.subclass, fr->ts, iaxs[fr->callno]->last);
		fr->outoforder = -1;
	}
	duped_fr = iaxfrdup2(fr);
	if (duped_fr) {
		schedule_delivery(duped_fr, updatehistory, 0, &fr->ts);
	}
	if (iaxs[fr->callno] && iaxs[fr->callno]->last < fr->ts) {
		iaxs[fr->callno]->last = fr->ts;
#if 1
		if (option_debug && iaxdebug)
			ast_log(LOG_DEBUG, "For call=%d, set last=%d\n", fr->callno, fr->ts);
#endif
	}

	/* Always run again */
	ast_mutex_unlock(&iaxsl[fr->callno]);
	return 1;
}

static void *iax2_process_thread(void *data)
{
	struct iax2_thread *thread = data;
	struct timeval tv;
	struct timespec ts;

	for(;;) {
		/* Wait for something to signal us to be awake */
		ast_mutex_lock(&thread->lock);
		if (thread->type == IAX_TYPE_DYNAMIC) {
			/* Wait to be signalled or time out */
			tv = ast_tvadd(ast_tvnow(), ast_samp2tv(30000, 1000));
			ts.tv_sec = tv.tv_sec;
			ts.tv_nsec = tv.tv_usec * 1000;
			if (ast_cond_timedwait(&thread->cond, &thread->lock, &ts) == ETIMEDOUT) {
				ast_mutex_unlock(&thread->lock);
				AST_LIST_LOCK(&dynamic_list);
				AST_LIST_REMOVE(&dynamic_list, thread, list);
				iaxdynamicthreadcount--;
				AST_LIST_UNLOCK(&dynamic_list);
				break;
			}
		} else {
			ast_cond_wait(&thread->cond, &thread->lock);
		}
		ast_mutex_unlock(&thread->lock);

		/* Add ourselves to the active list now */
		AST_LIST_LOCK(&active_list);
		AST_LIST_INSERT_HEAD(&active_list, thread, list);
		AST_LIST_UNLOCK(&active_list);

		/* See what we need to do */
		switch(thread->iostate) {
		case IAX_IOSTATE_READY:
			thread->actions++;
			thread->iostate = IAX_IOSTATE_PROCESSING;
			socket_process(thread);
			break;
		case IAX_IOSTATE_SCHEDREADY:
			thread->actions++;
			thread->iostate = IAX_IOSTATE_PROCESSING;
#ifdef SCHED_MULTITHREADED
			thread->schedfunc(thread->scheddata);
#endif		
			break;
		}
		time(&thread->checktime);
		thread->iostate = IAX_IOSTATE_IDLE;
#ifdef DEBUG_SCHED_MULTITHREAD
		thread->curfunc[0]='\0';
#endif		

		/* Now... remove ourselves from the active list, and return to the idle list */
		AST_LIST_LOCK(&active_list);
		AST_LIST_REMOVE(&active_list, thread, list);
		AST_LIST_UNLOCK(&active_list);

		/* Go back into our respective list */
		if (thread->type == IAX_TYPE_DYNAMIC) {
			AST_LIST_LOCK(&dynamic_list);
			AST_LIST_INSERT_TAIL(&dynamic_list, thread, list);
			AST_LIST_UNLOCK(&dynamic_list);
		} else {
			AST_LIST_LOCK(&idle_list);
			AST_LIST_INSERT_TAIL(&idle_list, thread, list);
			AST_LIST_UNLOCK(&idle_list);
		}
	}

	return NULL;
}

static int iax2_do_register(struct iax2_registry *reg)
{
	struct iax_ie_data ied;
	if (option_debug && iaxdebug)
		ast_log(LOG_DEBUG, "Sending registration request for '%s'\n", reg->username);

	if (reg->dnsmgr && 
	    ((reg->regstate == REG_STATE_TIMEOUT) || !reg->addr.sin_addr.s_addr)) {
		/* Maybe the IP has changed, force DNS refresh */
		ast_dnsmgr_refresh(reg->dnsmgr);
	}
	
	/*
	 * if IP has Changed, free allocated call to create a new one with new IP
	 * call has the pointer to IP and must be updated to the new one
	 */
	if (reg->dnsmgr && ast_dnsmgr_changed(reg->dnsmgr) && (reg->callno > 0)) {
		iax2_destroy(reg->callno);
		reg->callno = 0;
	}
	if (!reg->addr.sin_addr.s_addr) {
		if (option_debug && iaxdebug)
			ast_log(LOG_DEBUG, "Unable to send registration request for '%s' without IP address\n", reg->username);
		/* Setup the next registration attempt */
		if (reg->expire > -1)
			ast_sched_del(sched, reg->expire);
		reg->expire  = ast_sched_add(sched, (5 * reg->refresh / 6) * 1000, iax2_do_register_s, reg);
		return -1;
	}

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
		if (option_debug)
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
	if (ast_strlen_zero(data))
		data = "default";
	sdata = ast_strdupa(data);
	opts = strchr(sdata, '|');
	if (opts)
		*opts='\0';

	if (chan->tech != &iax2_tech) {
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
		ast_inet_ntoa(iaxs[callno]->addr.sin_addr),
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

static void __iax2_poke_noanswer(void *data)
{
	struct iax2_peer *peer = data;
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
}

static int iax2_poke_noanswer(void *data)
{
	struct iax2_peer *peer = data;
	peer->pokeexpire = -1;
#ifdef SCHED_MULTITHREADED
	if (schedule_action(__iax2_poke_noanswer, data))
#endif		
		__iax2_poke_noanswer(data);
	return 0;
}

static int iax2_poke_peer(struct iax2_peer *peer, int heldcall)
{
	if (!peer->maxms || !peer->addr.sin_addr.s_addr) {
		/* IF we have no IP, or this isn't to be monitored, return
		  immediately after clearing things out */
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

	/* Speed up retransmission times for this qualify call */
	iaxs[peer->callno]->pingtime = peer->maxms / 4 + 1;
	iaxs[peer->callno]->peerpoke = peer;
	
	/* Remove any pending pokeexpire task */
	if (peer->pokeexpire > -1)
		ast_sched_del(sched, peer->pokeexpire);

	/* Queue up a new task to handle no reply */
	/* If the host is already unreachable then use the unreachable interval instead */
	if (peer->lastms < 0) {
		peer->pokeexpire = ast_sched_add(sched, peer->pokefreqnotok, iax2_poke_noanswer, peer);
	} else
		peer->pokeexpire = ast_sched_add(sched, DEFAULT_MAXMS * 2, iax2_poke_noanswer, peer);

	/* And send the poke */
	send_command(iaxs[peer->callno], AST_FRAME_IAX, IAX_COMMAND_POKE, 0, NULL, 0, -1);

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

	ast_copy_flags(&cai, &globalflags, IAX_NOTRANSFER | IAX_TRANSFERMEDIA | IAX_USEJITTERBUF | IAX_FORCEJITTERBUF);

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
	ast_copy_flags(iaxs[callno], &cai, IAX_TRUNK | IAX_SENDANI | IAX_NOTRANSFER | IAX_TRANSFERMEDIA | IAX_USEJITTERBUF | IAX_FORCEJITTERBUF);	
	if (ast_test_flag(&cai, IAX_TRUNK))
		callno = make_trunk(callno, 1);
	iaxs[callno]->maxtime = cai.maxtime;
	if (cai.found)
		ast_string_field_set(iaxs[callno], host, pds.peer);

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

static void *sched_thread(void *ignore)
{
	int count;
	int res;
	struct timeval tv;
	struct timespec ts;

	for (;;) {
		res = ast_sched_wait(sched);
		if ((res > 1000) || (res < 0))
			res = 1000;
		tv = ast_tvadd(ast_tvnow(), ast_samp2tv(res, 1000));
		ts.tv_sec = tv.tv_sec;
		ts.tv_nsec = tv.tv_usec * 1000;

		pthread_testcancel();
		ast_mutex_lock(&sched_lock);
		ast_cond_timedwait(&sched_cond, &sched_lock, &ts);
		ast_mutex_unlock(&sched_lock);
		pthread_testcancel();

		count = ast_sched_runq(sched);
		if (count >= 20 && option_debug)
			ast_log(LOG_DEBUG, "chan_iax2: ast_sched_runq ran %d scheduled tasks all at once\n", count);
	}

	return NULL;
}

static void *network_thread(void *ignore)
{
	/* Our job is simple: Send queued messages, retrying if necessary.  Read frames 
	   from the network, and queue them for delivery to the channels */
	int res, count;
	struct iax_frame *f;

	if (timingfd > -1)
		ast_io_add(io, timingfd, timing_read, AST_IO_IN | AST_IO_PRI, NULL);
	
	for(;;) {
		/* Go through the queue, sending messages which have not yet been
		   sent, and scheduling retransmissions if appropriate */
		AST_LIST_LOCK(&iaxq.queue);
		count = 0;
		AST_LIST_TRAVERSE_SAFE_BEGIN(&iaxq.queue, f, list) {
			if (f->sentyet)
				continue;
			
			/* Try to lock the pvt, if we can't... don't fret - defer it till later */
			if (ast_mutex_trylock(&iaxsl[f->callno]))
				continue;

			f->sentyet++;

			if (iaxs[f->callno]) {
				send_packet(f);
				count++;
			} 

			ast_mutex_unlock(&iaxsl[f->callno]);

			if (f->retries < 0) {
				/* This is not supposed to be retransmitted */
				AST_LIST_REMOVE(&iaxq.queue, f, list);
				iaxq.count--;
				/* Free the iax frame */
				iax_frame_free(f);
			} else {
				/* We need reliable delivery.  Schedule a retransmission */
				f->retries++;
				f->retrans = ast_sched_add(sched, f->retrytime, attempt_transmit, f);
				signal_condition(&sched_lock, &sched_cond);
			}
		}
		AST_LIST_TRAVERSE_SAFE_END
		AST_LIST_UNLOCK(&iaxq.queue);

		if (count >= 20 && option_debug)
			ast_log(LOG_DEBUG, "chan_iax2: Sent %d queued outbound frames all at once\n", count);

		/* Now do the IO, and run scheduled tasks */
		res = ast_io_wait(io, -1);
		if (res >= 0) {
			if (res >= 20 && option_debug)
				ast_log(LOG_DEBUG, "chan_iax2: ast_io_wait ran %d I/Os all at once\n", res);
		}
	}
	return NULL;
}

static int start_network_thread(void)
{
	int threadcount = 0;
	int x;
	for (x = 0; x < iaxthreadcount; x++) {
		struct iax2_thread *thread = ast_calloc(1, sizeof(struct iax2_thread));
		if (thread) {
			thread->type = IAX_TYPE_POOL;
			thread->threadnum = ++threadcount;
			ast_mutex_init(&thread->lock);
			ast_cond_init(&thread->cond, NULL);
			if (ast_pthread_create(&thread->threadid, NULL, iax2_process_thread, thread)) {
				ast_log(LOG_WARNING, "Failed to create new thread!\n");
				free(thread);
				thread = NULL;
			}
			AST_LIST_LOCK(&idle_list);
			AST_LIST_INSERT_TAIL(&idle_list, thread, list);
			AST_LIST_UNLOCK(&idle_list);
		}
	}
	ast_pthread_create_background(&schedthreadid, NULL, sched_thread, NULL);
	ast_pthread_create_background(&netthreadid, NULL, network_thread, NULL);
	if (option_verbose > 1)
		ast_verbose(VERBOSE_PREFIX_2 "%d helper threaads started\n", threadcount);
	return 0;
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
		if (option_debug)
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

	if (!(tmp = ast_strdupa(srcaddr)))
		return -1;

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
		sin.sin_family = AF_INET;
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
		if (option_debug)
			ast_log(LOG_DEBUG, "Using sourceaddress %s for '%s'\n", srcaddr, peer->name);
		return 0;
	}
}

		
/*! \brief Create peer structure based on configuration */
static struct iax2_peer *build_peer(const char *name, struct ast_variable *v, struct ast_variable *alt, int temponly)
{
	struct iax2_peer *peer = NULL;
	struct ast_ha *oldha = NULL;
	int maskfound=0;
	int found=0;
	int firstpass=1;

	AST_LIST_LOCK(&peers);
	if (!temponly) {
		AST_LIST_TRAVERSE(&peers, peer, entry) {
			if (!strcmp(peer->name, name)) {	
				if (!ast_test_flag(peer, IAX_DELME))
					firstpass = 0;
				break;
			}
		}
	} else
		peer = NULL;	
	if (peer) {
		found++;
		if (firstpass) {
			oldha = peer->ha;
			peer->ha = NULL;
		}
		AST_LIST_REMOVE(&peers, peer, entry);
		AST_LIST_UNLOCK(&peers);
 	} else {
		AST_LIST_UNLOCK(&peers);
		if ((peer = ast_calloc(1, sizeof(*peer)))) {
			peer->expire = -1;
			peer->pokeexpire = -1;
			peer->sockfd = defaultsockfd;
			if (ast_string_field_init(peer, 32)) {
				free(peer);
				peer = NULL;
			}
		}
	}
	if (peer) {
		if (firstpass) {
			ast_copy_flags(peer, &globalflags, IAX_USEJITTERBUF | IAX_FORCEJITTERBUF);
			peer->encmethods = iax2_encryption;
			peer->adsi = adsi;
			ast_string_field_set(peer,secret,"");
			if (!found) {
				ast_string_field_set(peer, name, name);
				peer->addr.sin_port = htons(IAX_DEFAULT_PORTNO);
				peer->expiry = min_reg_expire;
			}
			peer->prefs = prefs;
			peer->capability = iax2_capability;
			peer->smoothing = 0;
			peer->pokefreqok = DEFAULT_FREQ_OK;
			peer->pokefreqnotok = DEFAULT_FREQ_NOTOK;
			ast_string_field_set(peer,context,"");
			ast_string_field_set(peer,peercontext,"");
		}

		if (!v) {
			v = alt;
			alt = NULL;
		}
		while(v) {
			if (!strcasecmp(v->name, "secret")) {
				ast_string_field_set(peer, secret, v->value);
			} else if (!strcasecmp(v->name, "mailbox")) {
				ast_string_field_set(peer, mailbox, v->value);
			} else if (!strcasecmp(v->name, "mohinterpret")) {
				ast_string_field_set(peer, mohinterpret, v->value);
			} else if (!strcasecmp(v->name, "mohsuggest")) {
				ast_string_field_set(peer, mohsuggest, v->value);
			} else if (!strcasecmp(v->name, "dbsecret")) {
				ast_string_field_set(peer, dbsecret, v->value);
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
			} else if (!strcasecmp(v->name, "transfer")) {
				if (!strcasecmp(v->value, "mediaonly")) {
					ast_set_flags_to(peer, IAX_NOTRANSFER|IAX_TRANSFERMEDIA, IAX_TRANSFERMEDIA);	
				} else if (ast_true(v->value)) {
					ast_set_flags_to(peer, IAX_NOTRANSFER|IAX_TRANSFERMEDIA, 0);
				} else 
					ast_set_flags_to(peer, IAX_NOTRANSFER|IAX_TRANSFERMEDIA, IAX_NOTRANSFER);
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
						ast_string_field_free_pools(peer);
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
					ast_string_field_free_pools(peer);
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
					ast_string_field_set(peer, context, v->value);
			} else if (!strcasecmp(v->name, "regexten")) {
				ast_string_field_set(peer, regexten, v->value);
			} else if (!strcasecmp(v->name, "peercontext")) {
				if (ast_strlen_zero(peer->peercontext))
					ast_string_field_set(peer, peercontext, v->value);
			} else if (!strcasecmp(v->name, "port")) {
				if (ast_test_flag(peer, IAX_DYNAMIC))
					peer->defaddr.sin_port = htons(atoi(v->value));
				else
					peer->addr.sin_port = htons(atoi(v->value));
			} else if (!strcasecmp(v->name, "username")) {
				ast_string_field_set(peer, username, v->value);
			} else if (!strcasecmp(v->name, "allow")) {
				ast_parse_allow_disallow(&peer->prefs, &peer->capability, v->value, 1);
			} else if (!strcasecmp(v->name, "disallow")) {
				ast_parse_allow_disallow(&peer->prefs, &peer->capability, v->value, 0);
			} else if (!strcasecmp(v->name, "callerid")) {
				char name2[80];
				char num2[80];
				ast_callerid_split(v->value, name2, 80, num2, 80);
				ast_string_field_set(peer, cid_name, name2);
				ast_string_field_set(peer, cid_num, num2);
				ast_set_flag(peer, IAX_HASCALLERID);	
			} else if (!strcasecmp(v->name, "fullname")) {
				ast_string_field_set(peer, cid_name, v->value);
				ast_set_flag(peer, IAX_HASCALLERID);	
			} else if (!strcasecmp(v->name, "cid_number")) {
				ast_string_field_set(peer, cid_num, v->value);
				ast_set_flag(peer, IAX_HASCALLERID);	
			} else if (!strcasecmp(v->name, "sendani")) {
				ast_set2_flag(peer, ast_true(v->value), IAX_SENDANI);	
			} else if (!strcasecmp(v->name, "inkeys")) {
				ast_string_field_set(peer, inkeys, v->value);
			} else if (!strcasecmp(v->name, "outkey")) {
				ast_string_field_set(peer, outkey, v->value);
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
				ast_string_field_set(peer, zonetag, v->value);
			} else if (!strcasecmp(v->name, "adsi")) {
				peer->adsi = ast_true(v->value);
			}/* else if (strcasecmp(v->name,"type")) */
			/*	ast_log(LOG_WARNING, "Ignoring %s\n", v->name); */
			v = v->next;
			if (!v) {
				v = alt;
				alt = NULL;
			}
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
static struct iax2_user *build_user(const char *name, struct ast_variable *v, struct ast_variable *alt, int temponly)
{
	struct iax2_user *user = NULL;
	struct iax2_context *con, *conl = NULL;
	struct ast_ha *oldha = NULL;
	struct iax2_context *oldcon = NULL;
	int format;
	int firstpass=1;
	int oldcurauthreq = 0;
	char *varname = NULL, *varval = NULL;
	struct ast_variable *tmpvar = NULL;
	
	AST_LIST_LOCK(&users);
	if (!temponly) {
		AST_LIST_TRAVERSE(&users, user, entry) {
			if (!strcmp(user->name, name)) {	
				if (!ast_test_flag(user, IAX_DELME))
					firstpass = 0;
				break;
			}
		}
	} else
		user = NULL;

	if (user) {
		if (firstpass) {
			oldcurauthreq = user->curauthreq;
			oldha = user->ha;
			oldcon = user->contexts;
			user->ha = NULL;
			user->contexts = NULL;
		}
		/* Already in the list, remove it and it will be added back (or FREE'd) */
		AST_LIST_REMOVE(&users, user, entry);
		AST_LIST_UNLOCK(&users);
 	} else {
		AST_LIST_UNLOCK(&users);
		/* This is going to memset'd to 0 in the next block */
		user = ast_calloc(sizeof(*user),1);
	}
	
	if (user) {
		if (firstpass) {
			ast_string_field_free_pools(user);
			memset(user, 0, sizeof(struct iax2_user));
			if (ast_string_field_init(user, 32)) {
				free(user);
				user = NULL;
			}
			user->maxauthreq = maxauthreq;
			user->curauthreq = oldcurauthreq;
			user->prefs = prefs;
			user->capability = iax2_capability;
			user->encmethods = iax2_encryption;
			user->adsi = adsi;
			ast_string_field_set(user, name, name);
			ast_string_field_set(user, language, language);
			ast_copy_flags(user, &globalflags, IAX_USEJITTERBUF | IAX_FORCEJITTERBUF | IAX_CODEC_USER_FIRST | IAX_CODEC_NOPREFS | IAX_CODEC_NOCAP);	
		}
		if (!v) {
			v = alt;
			alt = NULL;
		}
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
				if (varname && (varval = strchr(varname,'='))) {
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
			} else if (!strcasecmp(v->name, "transfer")) {
				if (!strcasecmp(v->value, "mediaonly")) {
					ast_set_flags_to(user, IAX_NOTRANSFER|IAX_TRANSFERMEDIA, IAX_TRANSFERMEDIA);	
				} else if (ast_true(v->value)) {
					ast_set_flags_to(user, IAX_NOTRANSFER|IAX_TRANSFERMEDIA, 0);
				} else 
					ast_set_flags_to(user, IAX_NOTRANSFER|IAX_TRANSFERMEDIA, IAX_NOTRANSFER);
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
				ast_string_field_set(user, dbsecret, v->value);
			} else if (!strcasecmp(v->name, "secret")) {
				if (!ast_strlen_zero(user->secret)) {
					char *old = ast_strdupa(user->secret);

					ast_string_field_build(user, secret, "%s;%s", old, v->value);
				} else
					ast_string_field_set(user, secret, v->value);
			} else if (!strcasecmp(v->name, "callerid")) {
				char name2[80];
				char num2[80];
				ast_callerid_split(v->value, name2, 80, num2, 80);
				ast_string_field_set(user, cid_name, name2);
				ast_string_field_set(user, cid_num, num2);
				ast_set_flag(user, IAX_HASCALLERID);	
			} else if (!strcasecmp(v->name, "fullname")) {
				ast_string_field_set(user, cid_name, v->value);
				ast_set_flag(user, IAX_HASCALLERID);	
			} else if (!strcasecmp(v->name, "cid_number")) {
				ast_string_field_set(user, cid_num, v->value);
				ast_set_flag(user, IAX_HASCALLERID);	
			} else if (!strcasecmp(v->name, "accountcode")) {
				ast_string_field_set(user, accountcode, v->value);
			} else if (!strcasecmp(v->name, "mohinterpret")) {
				ast_string_field_set(user, mohinterpret, v->value);
			} else if (!strcasecmp(v->name, "mohsuggest")) {
				ast_string_field_set(user, mohsuggest, v->value);
			} else if (!strcasecmp(v->name, "language")) {
				ast_string_field_set(user, language, v->value);
			} else if (!strcasecmp(v->name, "amaflags")) {
				format = ast_cdr_amaflags2int(v->value);
				if (format < 0) {
					ast_log(LOG_WARNING, "Invalid AMA Flags: %s at line %d\n", v->value, v->lineno);
				} else {
					user->amaflags = format;
				}
			} else if (!strcasecmp(v->name, "inkeys")) {
				ast_string_field_set(user, inkeys, v->value);
			} else if (!strcasecmp(v->name, "maxauthreq")) {
				user->maxauthreq = atoi(v->value);
				if (user->maxauthreq < 0)
					user->maxauthreq = 0;
			} else if (!strcasecmp(v->name, "adsi")) {
				user->adsi = ast_true(v->value);
			}/* else if (strcasecmp(v->name,"type")) */
			/*	ast_log(LOG_WARNING, "Ignoring %s\n", v->name); */
			v = v->next;
			if (!v) {
				v = alt;
				alt = NULL;
			}
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
	struct iax2_user *user = NULL;
	struct iax2_peer *peer = NULL;
	struct iax2_registry *reg, *regl;

	AST_LIST_LOCK(&users);
	AST_LIST_TRAVERSE(&users, user, entry)
		ast_set_flag(user, IAX_DELME);
	AST_LIST_UNLOCK(&users);

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
				iax2_destroy(regl->callno);
			}
			ast_mutex_unlock(&iaxsl[regl->callno]);
		}
		if (regl->dnsmgr)
			ast_dnsmgr_release(regl->dnsmgr);
		free(regl);
	}
	registrations = NULL;

	AST_LIST_LOCK(&peers);
	AST_LIST_TRAVERSE(&peers, peer, entry)
		ast_set_flag(peer, IAX_DELME);
	AST_LIST_UNLOCK(&peers);
}

static void destroy_user(struct iax2_user *user)
{
	ast_free_ha(user->ha);
	free_context(user->contexts);
	if(user->vars) {
		ast_variables_destroy(user->vars);
		user->vars = NULL;
	}
	ast_string_field_free_pools(user);
	free(user);
}

static void prune_users(void)
{
	struct iax2_user *user = NULL;

	AST_LIST_LOCK(&users);
	AST_LIST_TRAVERSE_SAFE_BEGIN(&users, user, entry) {
		if (ast_test_flag(user, IAX_DELME)) {
			destroy_user(user);
			AST_LIST_REMOVE_CURRENT(&users, entry);
		}
	}
	AST_LIST_TRAVERSE_SAFE_END
	AST_LIST_UNLOCK(&users);

}

static void destroy_peer(struct iax2_peer *peer)
{
	ast_free_ha(peer->ha);

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

	ast_string_field_free_pools(peer);

	free(peer);
}

static void prune_peers(void){
	/* Prune peers who still are supposed to be deleted */
	struct iax2_peer *peer = NULL;

	AST_LIST_LOCK(&peers);
	AST_LIST_TRAVERSE_SAFE_BEGIN(&peers, peer, entry) {
		if (ast_test_flag(peer, IAX_DELME)) {
			destroy_peer(peer);
			AST_LIST_REMOVE_CURRENT(&peers, entry);
		}
	}
	AST_LIST_TRAVERSE_SAFE_END
	AST_LIST_UNLOCK(&peers);
}

static void set_timing(void)
{
#ifdef HAVE_ZAPTEL
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
	struct ast_config *cfg, *ucfg;
	int capability=iax2_capability;
	struct ast_variable *v;
	char *cat;
	const char *utype;
	const char *tosval;
	int format;
	int portno = IAX_DEFAULT_PORTNO;
	int  x;
	int mtuv; 
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

	maxauthreq = 3;

	v = ast_variable_browse(cfg, "general");

	/* Seed initial tos value */
	tosval = ast_variable_retrieve(cfg, "general", "tos");
	if (tosval) {
		if (ast_str2tos(tosval, &tos))
			ast_log(LOG_WARNING, "Invalid tos value, see doc/ip-tos.txt for more information.\n");
	}
	while(v) {
		if (!strcasecmp(v->name, "bindport")){ 
			if (reload)
				ast_log(LOG_NOTICE, "Ignoring bindport on reload\n");
			else
				portno = atoi(v->value);
		} else if (!strcasecmp(v->name, "pingtime")) 
			ping_time = atoi(v->value);
		else if (!strcasecmp(v->name, "iaxthreadcount")) {
			if (reload) {
				if (atoi(v->value) != iaxthreadcount)
					ast_log(LOG_NOTICE, "Ignoring any changes to iaxthreadcount during reload\n");
			} else {
				iaxthreadcount = atoi(v->value);
				if (iaxthreadcount < 1) {
					ast_log(LOG_NOTICE, "iaxthreadcount must be at least 1.\n");
					iaxthreadcount = 1;
				} else if (iaxthreadcount > 256) {
					ast_log(LOG_NOTICE, "limiting iaxthreadcount to 256\n");
					iaxthreadcount = 256;
				}
			}
		} else if (!strcasecmp(v->name, "iaxmaxthreadcount")) {
			if (reload) {
				AST_LIST_LOCK(&dynamic_list);
				iaxmaxthreadcount = atoi(v->value);
				AST_LIST_UNLOCK(&dynamic_list);
			} else {
				iaxmaxthreadcount = atoi(v->value);
				if (iaxmaxthreadcount < 0) {
					ast_log(LOG_NOTICE, "iaxmaxthreadcount must be at least 0.\n");
					iaxmaxthreadcount = 0;
				} else if (iaxmaxthreadcount > 256) {
					ast_log(LOG_NOTICE, "Limiting iaxmaxthreadcount to 256\n");
					iaxmaxthreadcount = 256;
				}
			}
		} else if (!strcasecmp(v->name, "nochecksums")) {
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
		else if (!strcasecmp(v->name, "resyncthreshold")) 
			resyncthreshold = atoi(v->value);
		else if (!strcasecmp(v->name, "maxjitterinterps")) 
			maxjitterinterps = atoi(v->value);
		else if (!strcasecmp(v->name, "lagrqtime")) 
			lagrq_time = atoi(v->value);
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
		else if (!strcasecmp(v->name, "transfer")) {
			if (!strcasecmp(v->value, "mediaonly")) {
				ast_set_flags_to((&globalflags), IAX_NOTRANSFER|IAX_TRANSFERMEDIA, IAX_TRANSFERMEDIA);	
			} else if (ast_true(v->value)) {
				ast_set_flags_to((&globalflags), IAX_NOTRANSFER|IAX_TRANSFERMEDIA, 0);
			} else 
				ast_set_flags_to((&globalflags), IAX_NOTRANSFER|IAX_TRANSFERMEDIA, IAX_NOTRANSFER);
		} else if (!strcasecmp(v->name, "codecpriority")) {
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
		} else if (!strcasecmp(v->name, "trunkmtu")) {
			mtuv = atoi(v->value);
			if (mtuv  == 0 )  
				global_max_trunk_mtu = 0; 
			else if (mtuv >= 172 && mtuv < 4000) 
				global_max_trunk_mtu = mtuv; 
			else 
				ast_log(LOG_NOTICE, "trunkmtu value out of bounds (%d) at line %d\n",
					mtuv, v->lineno); 
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
				ast_context_create(NULL, regcontext, "IAX2");
		} else if (!strcasecmp(v->name, "tos")) {
			if (ast_str2tos(v->value, &tos))
				ast_log(LOG_WARNING, "Invalid tos value at line %d, see doc/ip-tos.txt for more information.'\n", v->lineno);
		} else if (!strcasecmp(v->name, "accountcode")) {
			ast_copy_string(accountcode, v->value, sizeof(accountcode));
		} else if (!strcasecmp(v->name, "mohinterpret")) {
			ast_copy_string(mohinterpret, v->value, sizeof(user->mohinterpret));
		} else if (!strcasecmp(v->name, "mohsuggest")) {
			ast_copy_string(mohsuggest, v->value, sizeof(user->mohsuggest));
		} else if (!strcasecmp(v->name, "amaflags")) {
			format = ast_cdr_amaflags2int(v->value);
			if (format < 0) {
				ast_log(LOG_WARNING, "Invalid AMA Flags: %s at line %d\n", v->value, v->lineno);
			} else {
				amaflags = format;
			}
		} else if (!strcasecmp(v->name, "language")) {
			ast_copy_string(language, v->value, sizeof(language));
		} else if (!strcasecmp(v->name, "maxauthreq")) {
			maxauthreq = atoi(v->value);
			if (maxauthreq < 0)
				maxauthreq = 0;
		} else if (!strcasecmp(v->name, "adsi")) {
			adsi = ast_true(v->value);
		} /*else if (strcasecmp(v->name,"type")) */
		/*	ast_log(LOG_WARNING, "Ignoring %s\n", v->name); */
		v = v->next;
	}
	
	if (defaultsockfd < 0) {
		if (!(ns = ast_netsock_bind(netsock, io, "0.0.0.0", portno, tos, socket_read, NULL))) {
			ast_log(LOG_ERROR, "Unable to create network socket: %s\n", strerror(errno));
		} else {
			if (option_verbose > 1)
				ast_verbose(VERBOSE_PREFIX_2 "Binding IAX2 to default address 0.0.0.0:%d\n", portno);
			defaultsockfd = ast_netsock_sockfd(ns);
			ast_netsock_unref(ns);
		}
	}
	
	if (min_reg_expire > max_reg_expire) {
		ast_log(LOG_WARNING, "Minimum registration interval of %d is more than maximum of %d, resetting minimum to %d\n",
			min_reg_expire, max_reg_expire, max_reg_expire);
		min_reg_expire = max_reg_expire;
	}
	iax2_capability = capability;
	
	ucfg = ast_config_load("users.conf");
	if (ucfg) {
		struct ast_variable *gen;
		int genhasiax;
		int genregisteriax;
		const char *hasiax, *registeriax;
		
		genhasiax = ast_true(ast_variable_retrieve(ucfg, "general", "hasiax"));
		genregisteriax = ast_true(ast_variable_retrieve(ucfg, "general", "registeriax"));
		gen = ast_variable_browse(ucfg, "general");
		cat = ast_category_browse(ucfg, NULL);
		while (cat) {
			if (strcasecmp(cat, "general")) {
				hasiax = ast_variable_retrieve(ucfg, cat, "hasiax");
				registeriax = ast_variable_retrieve(ucfg, cat, "registeriax");
				if (ast_true(hasiax) || (!hasiax && genhasiax)) {
					/* Start with general parameters, then specific parameters, user and peer */
					user = build_user(cat, gen, ast_variable_browse(ucfg, cat), 0);
					if (user) {
						AST_LIST_LOCK(&users);
						AST_LIST_INSERT_HEAD(&users, user, entry);
						AST_LIST_UNLOCK(&users);
					}
					peer = build_peer(cat, gen, ast_variable_browse(ucfg, cat), 0);
					if (peer) {
						AST_LIST_LOCK(&peers);
						AST_LIST_INSERT_HEAD(&peers, peer, entry);
						AST_LIST_UNLOCK(&peers);
						if (ast_test_flag(peer, IAX_DYNAMIC))
							reg_source_db(peer);
					}
				}
				if (ast_true(registeriax) || (!registeriax && genregisteriax)) {
					char tmp[256];
					const char *host = ast_variable_retrieve(ucfg, cat, "host");
					const char *username = ast_variable_retrieve(ucfg, cat, "username");
					const char *secret = ast_variable_retrieve(ucfg, cat, "secret");
					if (!host)
						host = ast_variable_retrieve(ucfg, "general", "host");
					if (!username)
						username = ast_variable_retrieve(ucfg, "general", "username");
					if (!secret)
						secret = ast_variable_retrieve(ucfg, "general", "secret");
					if (!ast_strlen_zero(username) && !ast_strlen_zero(host)) {
						if (!ast_strlen_zero(secret))
							snprintf(tmp, sizeof(tmp), "%s:%s@%s", username, secret, host);
						else
							snprintf(tmp, sizeof(tmp), "%s@%s", username, host);
						iax2_register(tmp, 0);
					}
				}
			}
			cat = ast_category_browse(ucfg, cat);
		}
		ast_config_destroy(ucfg);
	}
	
	cat = ast_category_browse(cfg, NULL);
	while(cat) {
		if (strcasecmp(cat, "general")) {
			utype = ast_variable_retrieve(cfg, cat, "type");
			if (utype) {
				if (!strcasecmp(utype, "user") || !strcasecmp(utype, "friend")) {
					user = build_user(cat, ast_variable_browse(cfg, cat), NULL, 0);
					if (user) {
						AST_LIST_LOCK(&users);
						AST_LIST_INSERT_HEAD(&users, user, entry);
						AST_LIST_UNLOCK(&users);
					}
				}
				if (!strcasecmp(utype, "peer") || !strcasecmp(utype, "friend")) {
					peer = build_peer(cat, ast_variable_browse(cfg, cat), NULL, 0);
					if (peer) {
						AST_LIST_LOCK(&peers);
						AST_LIST_INSERT_HEAD(&peers, peer, entry);
						AST_LIST_UNLOCK(&peers);
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
	struct iax2_peer *peer = NULL;

	strcpy(accountcode, "");
	strcpy(language, "");
	strcpy(mohinterpret, "default");
	strcpy(mohsuggest, "");
	global_max_trunk_mtu = MAX_TRUNK_MTU;
	amaflags = 0;
	delayreject = 0;
	ast_clear_flag((&globalflags), IAX_NOTRANSFER);	
	ast_clear_flag((&globalflags), IAX_TRANSFERMEDIA);	
	ast_clear_flag((&globalflags), IAX_USEJITTERBUF);	
	ast_clear_flag((&globalflags), IAX_FORCEJITTERBUF);	
	delete_users();
	set_config(config,1);
	prune_peers();
	prune_users();
	trunk_timed = trunk_untimed = 0; 
	trunk_nmaxmtu = trunk_maxmtu = 0; 

	for (reg = registrations; reg; reg = reg->next)
		iax2_do_register(reg);
	/* Qualify hosts, too */
	AST_LIST_LOCK(&peers);
	AST_LIST_TRAVERSE(&peers, peer, entry)
		iax2_poke_peer(peer, 0);
	AST_LIST_UNLOCK(&peers);
	reload_firmware();
	iax_provision_reload();

	return 0;
}

static int iax2_reload(int fd, int argc, char *argv[])
{
	return reload_config();
}

static int reload(void)
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

	if (option_debug)
		ast_log(LOG_DEBUG, "peer: %s, username: %s, password: %s, context: %s\n",
			pds.peer, pds.username, pds.password, pds.context);

	callno = find_callno(0, 0, &sin, NEW_FORCE, 1, cai.sockfd);
	if (callno < 1) {
		ast_log(LOG_WARNING, "Unable to create call\n");
		return -1;
	}

	ast_mutex_lock(&iaxsl[callno]);
	ast_string_field_set(iaxs[callno], dproot, data);
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
		ast_string_field_set(iaxs[callno], secret, pds.password);
	if (pds.key)
		ast_string_field_set(iaxs[callno], outkey, pds.key);
	/* Start the call going */
	send_command(iaxs[callno], AST_FRAME_IAX, IAX_COMMAND_NEW, 0, ied.buf, ied.pos, -1);

	return callno;
}

static struct iax2_dpcache *find_cache(struct ast_channel *chan, const char *data, const char *context, const char *exten, int priority)
{
	struct iax2_dpcache *dp = NULL;
	struct timeval tv;
	int x, com[2], timeout, old = 0, outfd, abort, callno;
	struct ast_channel *c = NULL;
	struct ast_frame *f = NULL;

	gettimeofday(&tv, NULL);

	AST_LIST_TRAVERSE_SAFE_BEGIN(&dpcache, dp, cache_list) {
		if (ast_tvcmp(tv, dp->expiry) > 0) {
			AST_LIST_REMOVE_CURRENT(&dpcache, cache_list);
			if ((dp->flags & CACHE_FLAG_PENDING) || dp->callno)
				ast_log(LOG_WARNING, "DP still has peer field or pending or callno (flags = %d, peer = blah, callno = %d)\n", dp->flags, dp->callno);
			else
				free(dp);
			continue;
		}
		if (!strcmp(dp->peercontext, data) && !strcmp(dp->exten, exten))
			break;
	}
	AST_LIST_TRAVERSE_SAFE_END

	if (!dp) {
		/* No matching entry.  Create a new one. */
		/* First, can we make a callno? */
		if ((callno = cache_get_callno_locked(data)) < 0) {
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
		dp->flags = CACHE_FLAG_PENDING;
		for (x=0;x<sizeof(dp->waiters) / sizeof(dp->waiters[0]); x++)
			dp->waiters[x] = -1;
		/* Insert into the lists */
		AST_LIST_INSERT_TAIL(&dpcache, dp, cache_list);
		AST_LIST_INSERT_TAIL(&iaxs[callno]->dpentries, dp, peer_list);
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
		AST_LIST_UNLOCK(&dpcache);
		/* Defer any dtmf */
		if (chan)
			old = ast_channel_defer_dtmf(chan);
		abort = 0;
		while(timeout) {
			c = ast_waitfor_nandfds(&chan, chan ? 1 : 0, &com[0], 1, NULL, &outfd, &timeout);
			if (outfd > -1)
				break;
			if (!c)
				continue;
			if (!(f = ast_read(c))) {
				abort = 1;
				break;
			}
			ast_frfree(f);
		}
		if (!timeout) {
			ast_log(LOG_WARNING, "Timeout waiting for %s exten %s\n", data, exten);
		}
		AST_LIST_LOCK(&dpcache);
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
	int res = 0;
	struct iax2_dpcache *dp = NULL;
#if 0
	ast_log(LOG_NOTICE, "iax2_exists: con: %s, exten: %s, pri: %d, cid: %s, data: %s\n", context, exten, priority, callerid ? callerid : "<unknown>", data);
#endif
	if ((priority != 1) && (priority != 2))
		return 0;

	AST_LIST_LOCK(&dpcache);
	if ((dp = find_cache(chan, data, context, exten, priority))) {
		if (dp->flags & CACHE_FLAG_EXISTS)
			res = 1;
	} else {
		ast_log(LOG_WARNING, "Unable to make DP cache\n");
	}
	AST_LIST_UNLOCK(&dpcache);

	return res;
}

/*! \brief part of the IAX2 dial plan switch interface */
static int iax2_canmatch(struct ast_channel *chan, const char *context, const char *exten, int priority, const char *callerid, const char *data)
{
	int res = 0;
	struct iax2_dpcache *dp = NULL;
#if 0
	ast_log(LOG_NOTICE, "iax2_canmatch: con: %s, exten: %s, pri: %d, cid: %s, data: %s\n", context, exten, priority, callerid ? callerid : "<unknown>", data);
#endif
	if ((priority != 1) && (priority != 2))
		return 0;

	AST_LIST_LOCK(&dpcache);
	if ((dp = find_cache(chan, data, context, exten, priority))) {
		if (dp->flags & CACHE_FLAG_CANEXIST)
			res = 1;
	} else {
		ast_log(LOG_WARNING, "Unable to make DP cache\n");
	}
	AST_LIST_UNLOCK(&dpcache);

	return res;
}

/*! \brief Part of the IAX2 Switch interface */
static int iax2_matchmore(struct ast_channel *chan, const char *context, const char *exten, int priority, const char *callerid, const char *data)
{
	int res = 0;
	struct iax2_dpcache *dp = NULL;
#if 0
	ast_log(LOG_NOTICE, "iax2_matchmore: con: %s, exten: %s, pri: %d, cid: %s, data: %s\n", context, exten, priority, callerid ? callerid : "<unknown>", data);
#endif
	if ((priority != 1) && (priority != 2))
		return 0;

	AST_LIST_LOCK(&dpcache);
	if ((dp = find_cache(chan, data, context, exten, priority))) {
		if (dp->flags & CACHE_FLAG_MATCHMORE)
			res = 1;
	} else {
		ast_log(LOG_WARNING, "Unable to make DP cache\n");
	}
	AST_LIST_UNLOCK(&dpcache);

	return res;
}

/*! \brief Execute IAX2 dialplan switch */
static int iax2_exec(struct ast_channel *chan, const char *context, const char *exten, int priority, const char *callerid, const char *data)
{
	char odata[256];
	char req[256];
	char *ncontext;
	struct iax2_dpcache *dp = NULL;
	struct ast_app *dial = NULL;
#if 0
	ast_log(LOG_NOTICE, "iax2_exec: con: %s, exten: %s, pri: %d, cid: %s, data: %s, newstack: %d\n", context, exten, priority, callerid ? callerid : "<unknown>", data, newstack);
#endif
	if (priority == 2) {
		/* Indicate status, can be overridden in dialplan */
		const char *dialstatus = pbx_builtin_getvar_helper(chan, "DIALSTATUS");
		if (dialstatus) {
			dial = pbx_findapp(dialstatus);
			if (dial) 
				pbx_exec(chan, dial, "");
		}
		return -1;
	} else if (priority != 1)
		return -1;

	AST_LIST_LOCK(&dpcache);
	if ((dp = find_cache(chan, data, context, exten, priority))) {
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
			AST_LIST_UNLOCK(&dpcache);
			ast_log(LOG_WARNING, "Can't execute nonexistent extension '%s[@%s]' in data '%s'\n", exten, context, data);
			return -1;
		}
	}
	AST_LIST_UNLOCK(&dpcache);

	if ((dial = pbx_findapp("Dial")))
		return pbx_exec(chan, dial, req);
	else
		ast_log(LOG_WARNING, "No dial application registered\n");

	return -1;
}

static int function_iaxpeer(struct ast_channel *chan, char *cmd, char *data, char *buf, size_t len)
{
	struct iax2_peer *peer;
	char *peername, *colname;

	peername = ast_strdupa(data);

	/* if our channel, return the IP address of the endpoint of current channel */
	if (!strcmp(peername,"CURRENTCHANNEL")) {
	        unsigned short callno;
		if (chan->tech != &iax2_tech)
			return -1;
		callno = PTR_TO_CALLNO(chan->tech_pvt);	
		ast_copy_string(buf, iaxs[callno]->addr.sin_addr.s_addr ? ast_inet_ntoa(iaxs[callno]->addr.sin_addr) : "", len);
		return 0;
	}

	if ((colname = strchr(peername, ':')))	/*! \todo : will be removed after the 1.4 relese */
		*colname++ = '\0';
	else if ((colname = strchr(peername, '|')))
		*colname++ = '\0';
	else
		colname = "ip";

	if (!(peer = find_peer(peername, 1)))
		return -1;

	if (!strcasecmp(colname, "ip")) {
		ast_copy_string(buf, peer->addr.sin_addr.s_addr ? ast_inet_ntoa(peer->addr.sin_addr) : "", len);
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

	return 0;
}

struct ast_custom_function iaxpeer_function = {
	.name = "IAXPEER",
	.synopsis = "Gets IAX peer information",
	.syntax = "IAXPEER(<peername|CURRENTCHANNEL>[|item])",
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
	struct parsed_dial_string pds;
	char *tmp = ast_strdupa(data);
	struct iax2_peer *p;
	int res = AST_DEVICE_INVALID;

	memset(&pds, 0, sizeof(pds));
	parse_dial_string(tmp, &pds);
	if (ast_strlen_zero(pds.peer))
		return res;
	
	if (option_debug > 2)
		ast_log(LOG_DEBUG, "Checking device state for device %s\n", pds.peer);

	/* SLD: FIXME: second call to find_peer during registration */
	if (!(p = find_peer(pds.peer, 1)))
		return res;

	res = AST_DEVICE_UNAVAILABLE;
	if (option_debug > 2) 
		ast_log(LOG_DEBUG, "iax2_devicestate: Found peer. What's device state of %s? addr=%d, defaddr=%d maxms=%d, lastms=%d\n",
			pds.peer, p->addr.sin_addr.s_addr, p->defaddr.sin_addr.s_addr, p->maxms, p->lastms);
	
	if ((p->addr.sin_addr.s_addr || p->defaddr.sin_addr.s_addr) &&
	    (!p->maxms || ((p->lastms > -1) && (p->historicms <= p->maxms)))) {
		/* Peer is registered, or have default IP address
		   and a valid registration */
		if (p->historicms == 0 || p->historicms <= p->maxms)
			/* let the core figure out whether it is in use or not */
			res = AST_DEVICE_UNKNOWN;	
	}

	if (ast_test_flag(p, IAX_TEMPONLY))
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
"Usage: iax2 show stats\n"
"       Display statistics on IAX channel driver.\n";

static char set_mtu_usage[] =
"Usage: iax2 set mtu <value>\n"
"       Set the system-wide IAX IP mtu to <value> bytes net or zero to disable.\n"
"       Disabling means that the operating system must handle fragmentation of UDP packets\n"
"       when the IAX2 trunk packet exceeds the UDP payload size.\n"
"       This is substantially below the IP mtu. Try 1240 on ethernets.\n"
"       Must be 172 or greater for G.711 samples.\n"; 
static char show_cache_usage[] =
"Usage: iax2 show cache\n"
"       Display currently cached IAX Dialplan results.\n";

static char show_peer_usage[] =
"Usage: iax2 show peer <name>\n"
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

static char show_threads_usage[] = 
"Usage: iax2 show threads\n"
"       Lists status of IAX helper threads\n";

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
"Usage: iax2 set debug\n"
"       Enables dumping of IAX packets for debugging purposes\n";

static char no_debug_usage[] = 
"Usage: iax2 set debug off\n"
"       Disables dumping of IAX packets for debugging purposes\n";

static char debug_trunk_usage[] =
"Usage: iax2 set debug trunk\n"
"       Requests current status of IAX trunking\n";

static char no_debug_trunk_usage[] =
"Usage: iax2 set debug trunk off\n"
"       Requests current status of IAX trunking\n";

static char debug_jb_usage[] =
"Usage: iax2 set debug jb\n"
"       Enables jitterbuffer debugging information\n";

static char no_debug_jb_usage[] =
"Usage: iax2 set debug jb off\n"
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

static struct ast_cli_entry cli_iax2[] = {
	{ { "iax2", "show", "cache", NULL },
	iax2_show_cache, "Display IAX cached dialplan",
	show_cache_usage },

	{ { "iax2", "show", "channels", NULL },
	iax2_show_channels, "List active IAX channels",
	show_channels_usage },

	{ { "iax2", "show", "firmware", NULL },
	iax2_show_firmware, "List available IAX firmwares",
	show_firmware_usage },

	{ { "iax2", "show", "netstats", NULL },
	iax2_show_netstats, "List active IAX channel netstats",
	show_netstats_usage },

	{ { "iax2", "show", "peers", NULL },
	iax2_show_peers, "List defined IAX peers",
	show_peers_usage },

	{ { "iax2", "show", "registry", NULL },
	iax2_show_registry, "Display IAX registration status",
	show_reg_usage },

	{ { "iax2", "show", "stats", NULL },
	iax2_show_stats, "Display IAX statistics",
	show_stats_usage },

	{ { "iax2", "show", "threads", NULL },
	iax2_show_threads, "Display IAX helper thread info",
	show_threads_usage },

	{ { "iax2", "set", "mtu", NULL },
	iax2_set_mtu, "Set the IAX systemwide trunking MTU",
	set_mtu_usage, NULL, NULL },

	{ { "iax2", "show", "users", NULL },
	iax2_show_users, "List defined IAX users",
	show_users_usage },

	{ { "iax2", "prune", "realtime", NULL },
	iax2_prune_realtime, "Prune a cached realtime lookup",
	prune_realtime_usage, complete_iax2_show_peer },

	{ { "iax2", "reload", NULL },
	iax2_reload, "Reload IAX configuration",
	iax2_reload_usage },

	{ { "iax2", "show", "peer", NULL },
	iax2_show_peer, "Show details on specific IAX peer",
	show_peer_usage, complete_iax2_show_peer },

	{ { "iax2", "set", "debug", NULL },
	iax2_do_debug, "Enable IAX debugging",
	debug_usage },

	{ { "iax2", "set", "debug", "trunk", NULL },
	iax2_do_trunk_debug, "Enable IAX trunk debugging",
	debug_trunk_usage },

	{ { "iax2", "set", "debug", "jb", NULL },
	iax2_do_jb_debug, "Enable IAX jitterbuffer debugging",
	debug_jb_usage },

	{ { "iax2", "set", "debug", "off", NULL },
	iax2_no_debug, "Disable IAX debugging",
	no_debug_usage },

	{ { "iax2", "set", "debug", "trunk", "off", NULL },
	iax2_no_trunk_debug, "Disable IAX trunk debugging",
	no_debug_trunk_usage },

	{ { "iax2", "set", "debug", "jb", "off", NULL },
	iax2_no_jb_debug, "Disable IAX jitterbuffer debugging",
	no_debug_jb_usage },

	{ { "iax2", "test", "losspct", NULL },
	iax2_test_losspct, "Set IAX2 incoming frame loss percentage",
	iax2_test_losspct_usage },

	{ { "iax2", "provision", NULL },
	iax2_prov_cmd, "Provision an IAX device",
	show_prov_usage, iax2_prov_complete_template_3rd },

#ifdef IAXTESTS
	{ { "iax2", "test", "late", NULL },
	iax2_test_late, "Test the receipt of a late frame",
	iax2_test_late_usage },

	{ { "iax2", "test", "resync", NULL },
	iax2_test_resync, "Test a resync in received timestamps",
	iax2_test_resync_usage },

	{ { "iax2", "test", "jitter", NULL },
	iax2_test_jitter, "Simulates jitter for testing",
	iax2_test_jitter_usage },
#endif /* IAXTESTS */
};

static void thread_free(struct iax2_thread *thread)
{
	ast_mutex_destroy(&thread->lock);
	ast_cond_destroy(&thread->cond);
	free(thread);
}

static int __unload_module(void)
{
	pthread_t threadid = AST_PTHREADT_NULL;
	struct iax2_thread *thread = NULL;
	int x;

	/* Cancel the network thread, close the net socket */
	if (netthreadid != AST_PTHREADT_NULL) {
		pthread_cancel(netthreadid);
		pthread_join(netthreadid, NULL);
	}
	if (schedthreadid != AST_PTHREADT_NULL) {
		pthread_cancel(schedthreadid);
		ast_mutex_lock(&sched_lock);
		ast_cond_signal(&sched_cond);
		ast_mutex_unlock(&sched_lock);
		pthread_join(schedthreadid, NULL);
	}

	/* Call for all threads to halt */
	AST_LIST_LOCK(&idle_list);
	AST_LIST_TRAVERSE_SAFE_BEGIN(&idle_list, thread, list) {
		AST_LIST_REMOVE_CURRENT(&idle_list, list);
		threadid = thread->threadid;
		pthread_cancel(threadid);
		signal_condition(&thread->lock, &thread->cond);
		pthread_join(threadid, NULL);
		thread_free(thread);
	}
	AST_LIST_TRAVERSE_SAFE_END
	AST_LIST_UNLOCK(&idle_list);

	AST_LIST_LOCK(&active_list);
	AST_LIST_TRAVERSE_SAFE_BEGIN(&active_list, thread, list) {
		AST_LIST_REMOVE_CURRENT(&active_list, list);
		threadid = thread->threadid;
		pthread_cancel(threadid);
		signal_condition(&thread->lock, &thread->cond);
		pthread_join(threadid, NULL);
		thread_free(thread);
	}
	AST_LIST_TRAVERSE_SAFE_END
	AST_LIST_UNLOCK(&active_list);

	AST_LIST_LOCK(&dynamic_list);
	AST_LIST_TRAVERSE_SAFE_BEGIN(&dynamic_list, thread, list) {
		AST_LIST_REMOVE_CURRENT(&dynamic_list, list);
		threadid = thread->threadid;
		pthread_cancel(threadid);
		signal_condition(&thread->lock, &thread->cond);
		pthread_join(threadid, NULL);
		thread_free(thread);
	}
	AST_LIST_TRAVERSE_SAFE_END
	AST_LIST_UNLOCK(&dynamic_list);

	ast_netsock_release(netsock);
	for (x=0;x<IAX_MAX_CALLS;x++)
		if (iaxs[x])
			iax2_destroy(x);
	ast_manager_unregister( "IAXpeers" );
	ast_manager_unregister( "IAXnetstats" );
	ast_unregister_application(papp);
	ast_cli_unregister_multiple(cli_iax2, sizeof(cli_iax2) / sizeof(struct ast_cli_entry));
	ast_unregister_switch(&iax2_switch);
	ast_channel_unregister(&iax2_tech);
	delete_users();
	iax_provision_unload();
	sched_context_destroy(sched);
	return 0;
}

static int unload_module(void)
{
	ast_custom_function_unregister(&iaxpeer_function);
	return __unload_module();
}


/*! \brief Load IAX2 module, load configuraiton ---*/
static int load_module(void)
{
	char *config = "iax.conf";
	int x = 0;
	struct iax2_registry *reg = NULL;
	struct iax2_peer *peer = NULL;
	
	ast_custom_function_register(&iaxpeer_function);

	iax_set_output(iax_debug_output);
	iax_set_error(iax_error_output);
	jb_setoutput(jb_error_output, jb_warning_output, NULL);
	
#ifdef HAVE_ZAPTEL
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

	ast_cond_init(&sched_cond, NULL);

	if (!(sched = sched_context_create())) {
		ast_log(LOG_ERROR, "Failed to create scheduler context\n");
		return AST_MODULE_LOAD_FAILURE;
	}

	if (!(io = io_context_create())) {
		ast_log(LOG_ERROR, "Failed to create I/O context\n");
		sched_context_destroy(sched);
		return AST_MODULE_LOAD_FAILURE;
	}

	if (!(netsock = ast_netsock_list_alloc())) {
		ast_log(LOG_ERROR, "Failed to create netsock list\n");
		io_context_destroy(io);
		sched_context_destroy(sched);
		return AST_MODULE_LOAD_FAILURE;
	}

	ast_netsock_init(netsock);
	
	ast_cli_register_multiple(cli_iax2, sizeof(cli_iax2) / sizeof(struct ast_cli_entry));

	ast_register_application(papp, iax2_prov_app, psyn, pdescrip);
	
	ast_manager_register( "IAXpeers", 0, manager_iax2_show_peers, "List IAX Peers" );
	ast_manager_register( "IAXnetstats", 0, manager_iax2_show_netstats, "Show IAX Netstats" );

	if(set_config(config, 0) == -1)
		return AST_MODULE_LOAD_DECLINE;

 	if (ast_channel_register(&iax2_tech)) {
		ast_log(LOG_ERROR, "Unable to register channel class %s\n", "IAX2");
		__unload_module();
		return AST_MODULE_LOAD_FAILURE;
	}

	if (ast_register_switch(&iax2_switch)) 
		ast_log(LOG_ERROR, "Unable to register IAX switch\n");

	if (start_network_thread()) {
		ast_log(LOG_ERROR, "Unable to start network thread\n");
		__unload_module();
		return AST_MODULE_LOAD_FAILURE;
	} else if (option_verbose > 1)
		ast_verbose(VERBOSE_PREFIX_2 "IAX Ready and Listening\n");

	for (reg = registrations; reg; reg = reg->next)
		iax2_do_register(reg);

	AST_LIST_LOCK(&peers);
	AST_LIST_TRAVERSE(&peers, peer, entry) {
		if (peer->sockfd < 0)
			peer->sockfd = defaultsockfd;
		iax2_poke_peer(peer, 0);
	}
	AST_LIST_UNLOCK(&peers);

	reload_firmware();
	iax_provision_reload();

	return AST_MODULE_LOAD_SUCCESS;
}

AST_MODULE_INFO(ASTERISK_GPL_KEY, AST_MODFLAG_DEFAULT, "Inter Asterisk eXchange (Ver 2)",
		.load = load_module,
		.unload = unload_module,
		.reload = reload,
	       );
