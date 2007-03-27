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
 * \brief Comedian Mail - Voicemail System
 *
 * \author Mark Spencer <markster@digium.com>
 *
 * \extref Unixodbc - http://www.unixodbc.org
 * \extref A source distribution of University of Washington's IMAP
c-client (http://www.washington.edu/imap/
 * 
 * \par See also
 * \arg \ref Config_vm
 * \note For information about voicemail IMAP storage, read doc/imapstorage.txt
 * \ingroup applications
 * \note This module requires res_adsi to load. This needs to be optional
 * during compilation.
 *
 *
 *
 * \note  This file is now almost impossible to work with, due to all #ifdefs.
 *        Feels like the database code before realtime. Someone - please come up
 *        with a plan to clean this up.
 */

/*** MODULEINFO
	<depend>res_adsi</depend>
 ***/

/*** MAKEOPTS
<category name="MENUSELECT_OPTS_app_voicemail" displayname="Voicemail Build Options" positive_output="yes" remove_on_change="apps/app_voicemail.o">
	<member name="ODBC_STORAGE" displayname="Storage of Voicemail using ODBC">
		<depend>unixodbc</depend>
		<conflict>IMAP_STORAGE</conflict>
		<defaultenabled>no</defaultenabled>
	</member>
	<member name="IMAP_STORAGE" displayname="Storage of Voicemail using IMAP4">
		<depend>imap_tk</depend>
		<conflict>ODBC_STORAGE</conflict>
		<use>ssl</use>
		<defaultenabled>no</defaultenabled>
	</member>
</category>
 ***/

#include "asterisk.h"

ASTERISK_FILE_VERSION(__FILE__, "$Revision$")

#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <time.h>
#include <dirent.h>

#ifdef IMAP_STORAGE
#include <ctype.h>
#include <signal.h>
#include <pwd.h>
#include "c-client.h"
#include "imap4r1.h"
#include "linkage.h"
#endif

#include "asterisk/lock.h"
#include "asterisk/file.h"
#include "asterisk/logger.h"
#include "asterisk/channel.h"
#include "asterisk/pbx.h"
#include "asterisk/options.h"
#include "asterisk/config.h"
#include "asterisk/say.h"
#include "asterisk/module.h"
#include "asterisk/adsi.h"
#include "asterisk/app.h"
#include "asterisk/manager.h"
#include "asterisk/dsp.h"
#include "asterisk/localtime.h"
#include "asterisk/cli.h"
#include "asterisk/utils.h"
#include "asterisk/stringfields.h"
#include "asterisk/smdi.h"

#ifdef ODBC_STORAGE
#include "asterisk/res_odbc.h"
#endif

#ifdef IMAP_STORAGE
static char imapserver[48];
static char imapport[8];
static char imapflags[128];
static char imapfolder[64];
static char authuser[32];
static char authpassword[42];

static int expungeonhangup = 1;
AST_MUTEX_DEFINE_STATIC(delimiter_lock);
static char delimiter = '\0';

struct vm_state;
struct ast_vm_user;

/* Forward declarations for IMAP */
static int init_mailstream(struct vm_state *vms, int box);
static void write_file(char *filename, char *buffer, unsigned long len);
static void display_body(BODY *body, char *pfx, long i);
static char *get_header_by_tag(char *header, char *tag, char *buf, size_t len);
static void vm_imap_delete(int msgnum, struct vm_state *vms);
static char *get_user_by_mailbox(char *mailbox, char *buf, size_t len);
static struct vm_state *get_vm_state_by_imapuser(char *user, int interactive);
static struct vm_state *get_vm_state_by_mailbox(const char *mailbox, int interactive);
static void vmstate_insert(struct vm_state *vms);
static void vmstate_delete(struct vm_state *vms);
static void set_update(MAILSTREAM * stream);
static void init_vm_state(struct vm_state *vms);
static void check_msgArray(struct vm_state *vms);
static void copy_msgArray(struct vm_state *dst, struct vm_state *src);
static int save_body(BODY *body, struct vm_state *vms, char *section, char *format);
static int make_gsm_file(char *dest, char *imapuser, char *dir, int num);
static void get_mailbox_delimiter(MAILSTREAM *stream);
static void mm_parsequota (MAILSTREAM *stream, unsigned char *msg, QUOTALIST *pquota);
static void imap_mailbox_name(char *spec, struct vm_state *vms, int box, int target);
static int imap_store_file(char *dir, char *mailboxuser, char *mailboxcontext, int msgnum, struct ast_channel *chan, struct ast_vm_user *vmu, char *fmt, int duration, struct vm_state *vms);



struct vmstate {
	struct vm_state *vms;
	AST_LIST_ENTRY(vmstate) list;
};

static AST_LIST_HEAD_STATIC(vmstates, vmstate);

#endif

#define SMDI_MWI_WAIT_TIMEOUT 1000 /* 1 second */

#define COMMAND_TIMEOUT 5000
/* Don't modify these here; set your umask at runtime instead */
#define	VOICEMAIL_DIR_MODE	0777
#define	VOICEMAIL_FILE_MODE	0666
#define	CHUNKSIZE	65536

#define VOICEMAIL_CONFIG "voicemail.conf"
#define ASTERISK_USERNAME "asterisk"

/* Default mail command to mail voicemail. Change it with the
    mailcmd= command in voicemail.conf */
#define SENDMAIL "/usr/sbin/sendmail -t"

#define INTRO "vm-intro"

#define MAXMSG 100
#define MAXMSGLIMIT 9999

#define BASEMAXINLINE 256
#define BASELINELEN 72
#define BASEMAXINLINE 256
#define eol "\r\n"

#define MAX_DATETIME_FORMAT	512
#define MAX_NUM_CID_CONTEXTS 10

#define VM_REVIEW        (1 << 0)
#define VM_OPERATOR      (1 << 1)
#define VM_SAYCID        (1 << 2)
#define VM_SVMAIL        (1 << 3)
#define VM_ENVELOPE      (1 << 4)
#define VM_SAYDURATION   (1 << 5)
#define VM_SKIPAFTERCMD  (1 << 6)
#define VM_FORCENAME     (1 << 7)   /*!< Have new users record their name */
#define VM_FORCEGREET    (1 << 8)   /*!< Have new users record their greetings */
#define VM_PBXSKIP       (1 << 9)
#define VM_DIRECFORWARD  (1 << 10)  /*!< directory_forward */
#define VM_ATTACH        (1 << 11)
#define VM_DELETE        (1 << 12)
#define VM_ALLOCED       (1 << 13)
#define VM_SEARCH        (1 << 14)
#define VM_TEMPGREETWARN (1 << 15)  /*!< Remind user tempgreeting is set */
#define ERROR_LOCK_PATH  -100


enum {
	OPT_SILENT =           (1 << 0),
	OPT_BUSY_GREETING =    (1 << 1),
	OPT_UNAVAIL_GREETING = (1 << 2),
	OPT_RECORDGAIN =       (1 << 3),
	OPT_PREPEND_MAILBOX =  (1 << 4),
	OPT_PRIORITY_JUMP =    (1 << 5),
	OPT_AUTOPLAY =         (1 << 6),
} vm_option_flags;

enum {
	OPT_ARG_RECORDGAIN = 0,
	OPT_ARG_PLAYFOLDER = 1,
	/* This *must* be the last value in this enum! */
	OPT_ARG_ARRAY_SIZE = 2,
} vm_option_args;

AST_APP_OPTIONS(vm_app_options, {
	AST_APP_OPTION('s', OPT_SILENT),
	AST_APP_OPTION('b', OPT_BUSY_GREETING),
	AST_APP_OPTION('u', OPT_UNAVAIL_GREETING),
	AST_APP_OPTION_ARG('g', OPT_RECORDGAIN, OPT_ARG_RECORDGAIN),
	AST_APP_OPTION('p', OPT_PREPEND_MAILBOX),
	AST_APP_OPTION('j', OPT_PRIORITY_JUMP),
	AST_APP_OPTION_ARG('a', OPT_AUTOPLAY, OPT_ARG_PLAYFOLDER),
});

static int load_config(void);

/*! \page vmlang Voicemail Language Syntaxes Supported

	\par Syntaxes supported, not really language codes.
	\arg \b en    - English
	\arg \b de    - German
	\arg \b es    - Spanish
	\arg \b fr    - French
	\arg \b it    - Italian
	\arg \b nl    - Dutch
	\arg \b pt    - Portuguese
	\arg \b pt_BR - Portuguese (Brazil)
	\arg \b gr    - Greek
	\arg \b no    - Norwegian
	\arg \b se    - Swedish

German requires the following additional soundfile:
\arg \b 1F	einE (feminine)

Spanish requires the following additional soundfile:
\arg \b 1M      un (masculine)

Dutch, Portuguese & Spanish require the following additional soundfiles:
\arg \b vm-INBOXs	singular of 'new'
\arg \b vm-Olds		singular of 'old/heard/read'

NB these are plural:
\arg \b vm-INBOX	nieuwe (nl)
\arg \b vm-Old		oude (nl)

Polish uses:
\arg \b vm-new-a	'new', feminine singular accusative
\arg \b vm-new-e	'new', feminine plural accusative
\arg \b vm-new-ych	'new', feminine plural genitive
\arg \b vm-old-a	'old', feminine singular accusative
\arg \b vm-old-e	'old', feminine plural accusative
\arg \b vm-old-ych	'old', feminine plural genitive
\arg \b digits/1-a	'one', not always same as 'digits/1'
\arg \b digits/2-ie	'two', not always same as 'digits/2'

Swedish uses:
\arg \b vm-nytt		singular of 'new'
\arg \b vm-nya		plural of 'new'
\arg \b vm-gammalt	singular of 'old'
\arg \b vm-gamla	plural of 'old'
\arg \b digits/ett	'one', not always same as 'digits/1'

Norwegian uses:
\arg \b vm-ny		singular of 'new'
\arg \b vm-nye		plural of 'new'
\arg \b vm-gammel	singular of 'old'
\arg \b vm-gamle	plural of 'old'

Dutch also uses:
\arg \b nl-om		'at'?

Spanish also uses:
\arg \b vm-youhaveno

Italian requires the following additional soundfile:

For vm_intro_it:
\arg \b vm-nuovo	new
\arg \b vm-nuovi	new plural
\arg \b vm-vecchio	old
\arg \b vm-vecchi	old plural

\note Don't use vm-INBOX or vm-Old, because they are the name of the INBOX and Old folders,
spelled among others when you have to change folder. For the above reasons, vm-INBOX
and vm-Old are spelled plural, to make them sound more as folder name than an adjective.

*/

struct baseio {
	int iocp;
	int iolen;
	int linelength;
	int ateof;
	unsigned char iobuf[BASEMAXINLINE];
};

/*! Structure for linked list of users */
struct ast_vm_user {
	char context[AST_MAX_CONTEXT];   /*!< Voicemail context */
	char mailbox[AST_MAX_EXTENSION]; /*!< Mailbox id, unique within vm context */
	char password[80];               /*!< Secret pin code, numbers only */
	char fullname[80];               /*!< Full name, for directory app */
	char email[80];                  /*!< E-mail address */
	char pager[80];                  /*!< E-mail address to pager (no attachment) */
	char serveremail[80];            /*!< From: Mail address */
	char mailcmd[160];               /*!< Configurable mail command */
	char language[MAX_LANGUAGE];     /*!< Config: Language setting */
	char zonetag[80];                /*!< Time zone */
	char callback[80];
	char dialout[80];
	char uniqueid[20];               /*!< Unique integer identifier */
	char exit[80];
	char attachfmt[20];              /*!< Attachment format */
	unsigned int flags;              /*!< VM_ flags */	
	int saydurationm;
	int maxmsg;                      /*!< Maximum number of msgs per folder for this mailbox */
	int maxsecs;                     /*!< Maximum number of seconds per message for this mailbox */
#ifdef IMAP_STORAGE
	char imapuser[80];               /*!< IMAP server login */
	char imappassword[80];           /*!< IMAP server password if authpassword not defined */
#endif
	double volgain;		         /*!< Volume gain for voicemails sent via email */
	AST_LIST_ENTRY(ast_vm_user) list;
};

/*! Voicemail time zones */
struct vm_zone {
	AST_LIST_ENTRY(vm_zone) list;
	char name[80];
	char timezone[80];
	char msg_format[512];
};

/*! Voicemail mailbox state */
struct vm_state {
	char curbox[80];
	char username[80];
	char curdir[PATH_MAX];
	char vmbox[PATH_MAX];
	char fn[PATH_MAX];
	char fn2[PATH_MAX];
	int *deleted;
	int *heard;
	int curmsg;
	int lastmsg;
	int newmessages;
	int oldmessages;
	int starting;
	int repeats;
#ifdef IMAP_STORAGE
	int updated;                         /*!< decremented on each mail check until 1 -allows delay */
	long msgArray[256];
	MAILSTREAM *mailstream;
	int vmArrayIndex;
	char imapuser[80];                   /*!< IMAP server login */
	int interactive;
	unsigned int quota_limit;
	unsigned int quota_usage;
	struct vm_state *persist_vms;
#endif
};


#ifdef ODBC_STORAGE
static char odbc_database[80];
static char odbc_table[80];
#define RETRIEVE(a,b) retrieve_file(a,b)
#define DISPOSE(a,b) remove_file(a,b)
#define STORE(a,b,c,d,e,f,g,h,i) store_file(a,b,c,d)
#define EXISTS(a,b,c,d) (message_exists(a,b))
#define RENAME(a,b,c,d,e,f,g,h) (rename_file(a,b,c,d,e,f))
#define COPY(a,b,c,d,e,f,g,h) (copy_file(a,b,c,d,e,f))
#define DELETE(a,b,c) (delete_file(a,b))
#else
#ifdef IMAP_STORAGE
#define RETRIEVE(a,b)
#define DISPOSE(a,b)
#define STORE(a,b,c,d,e,f,g,h,i) (imap_store_file(a,b,c,d,e,f,g,h,i))
#define EXISTS(a,b,c,d) (ast_fileexists(c,NULL,d) > 0)
#define RENAME(a,b,c,d,e,f,g,h) (rename_file(g,h));
#define COPY(a,b,c,d,e,f,g,h) (copy_file(g,h));
#define IMAP_DELETE(a,b,c,d) (vm_imap_delete(b,d))
#define DELETE(a,b,c) (vm_delete(c))
#else
#define RETRIEVE(a,b)
#define DISPOSE(a,b)
#define STORE(a,b,c,d,e,f,g,h,i)
#define EXISTS(a,b,c,d) (ast_fileexists(c,NULL,d) > 0)
#define RENAME(a,b,c,d,e,f,g,h) (rename_file(g,h));
#define COPY(a,b,c,d,e,f,g,h) (copy_file(g,h));
#define DELETE(a,b,c) (vm_delete(c))
#endif
#endif

static char VM_SPOOL_DIR[PATH_MAX];

static char ext_pass_cmd[128];

#define PWDCHANGE_INTERNAL (1 << 1)
#define PWDCHANGE_EXTERNAL (1 << 2)
static int pwdchange = PWDCHANGE_INTERNAL;

#ifdef ODBC_STORAGE
#define tdesc "Comedian Mail (Voicemail System) with ODBC Storage"
#else
# ifdef IMAP_STORAGE
# define tdesc "Comedian Mail (Voicemail System) with IMAP Storage"
# else
# define tdesc "Comedian Mail (Voicemail System)"
# endif
#endif

static char userscontext[AST_MAX_EXTENSION] = "default";

static char *addesc = "Comedian Mail";

static char *synopsis_vm = "Leave a Voicemail message";

static char *descrip_vm =
	"  VoiceMail(mailbox[@context][&mailbox[@context]][...][|options]): This\n"
	"application allows the calling party to leave a message for the specified\n"
	"list of mailboxes. When multiple mailboxes are specified, the greeting will\n"
	"be taken from the first mailbox specified. Dialplan execution will stop if the\n"
	"specified mailbox does not exist.\n"
	"  The Voicemail application will exit if any of the following DTMF digits are\n"
	"received:\n"
	"    0 - Jump to the 'o' extension in the current dialplan context.\n"
	"    * - Jump to the 'a' extension in the current dialplan context.\n"
	"  This application will set the following channel variable upon completion:\n"
	"    VMSTATUS - This indicates the status of the execution of the VoiceMail\n"
	"               application. The possible values are:\n"
	"               SUCCESS | USEREXIT | FAILED\n\n"
	"  Options:\n"
	"    b    - Play the 'busy' greeting to the calling party.\n"
	"    g(#) - Use the specified amount of gain when recording the voicemail\n"
	"           message. The units are whole-number decibels (dB).\n"
	"    s    - Skip the playback of instructions for leaving a message to the\n"
	"           calling party.\n"
	"    u    - Play the 'unavailable greeting.\n"
	"    j    - Jump to priority n+101 if the mailbox is not found or some other\n"
	"           error occurs.\n";

static char *synopsis_vmain = "Check Voicemail messages";

static char *descrip_vmain =
	"  VoiceMailMain([mailbox][@context][|options]): This application allows the\n"
	"calling party to check voicemail messages. A specific mailbox, and optional\n"
	"corresponding context, may be specified. If a mailbox is not provided, the\n"
	"calling party will be prompted to enter one. If a context is not specified,\n"
	"the 'default' context will be used.\n\n"
	"  Options:\n"
	"    p    - Consider the mailbox parameter as a prefix to the mailbox that\n"
	"           is entered by the caller.\n"
	"    g(#) - Use the specified amount of gain when recording a voicemail\n"
	"           message. The units are whole-number decibels (dB).\n"
	"    s    - Skip checking the passcode for the mailbox.\n"
	"    a(#) - Skip folder prompt and go directly to folder specified.\n"
	"           Defaults to INBOX\n";

static char *synopsis_vm_box_exists =
"Check to see if Voicemail mailbox exists";

static char *descrip_vm_box_exists =
	"  MailboxExists(mailbox[@context][|options]): Check to see if the specified\n"
	"mailbox exists. If no voicemail context is specified, the 'default' context\n"
	"will be used.\n"
	"  This application will set the following channel variable upon completion:\n"
	"    VMBOXEXISTSSTATUS - This will contain the status of the execution of the\n"
	"                        MailboxExists application. Possible values include:\n"
	"                        SUCCESS | FAILED\n\n"
	"  Options:\n"
	"    j - Jump to priority n+101 if the mailbox is found.\n";

static char *synopsis_vmauthenticate = "Authenticate with Voicemail passwords";

static char *descrip_vmauthenticate =
	"  VMAuthenticate([mailbox][@context][|options]): This application behaves the\n"
	"same way as the Authenticate application, but the passwords are taken from\n"
	"voicemail.conf.\n"
	"  If the mailbox is specified, only that mailbox's password will be considered\n"
	"valid. If the mailbox is not specified, the channel variable AUTH_MAILBOX will\n"
	"be set with the authenticated mailbox.\n\n"
	"  Options:\n"
	"    s - Skip playing the initial prompts.\n";

/* Leave a message */
static char *app = "VoiceMail";

/* Check mail, control, etc */
static char *app2 = "VoiceMailMain";

static char *app3 = "MailboxExists";
static char *app4 = "VMAuthenticate";

static AST_LIST_HEAD_STATIC(users, ast_vm_user);
static AST_LIST_HEAD_STATIC(zones, vm_zone);
static int maxsilence;
static int maxmsg;
static int silencethreshold = 128;
static char serveremail[80];
static char mailcmd[160];	/* Configurable mail cmd */
static char externnotify[160]; 
static struct ast_smdi_interface *smdi_iface = NULL;
static char vmfmts[80];
static double volgain;
static int vmminsecs;
static int vmmaxsecs;
static int maxgreet;
static int skipms;
static int maxlogins;

/* custom password sounds */
static char vm_password[80] = "vm-password";
static char vm_newpassword[80] = "vm-newpassword";
static char vm_passchanged[80] = "vm-passchanged";
static char vm_reenterpassword[80] = "vm-reenterpassword";
static char vm_mismatch[80] = "vm-mismatch";

static struct ast_flags globalflags = {0};

static int saydurationminfo;

static char dialcontext[AST_MAX_CONTEXT];
static char callcontext[AST_MAX_CONTEXT];
static char exitcontext[AST_MAX_CONTEXT];

static char cidinternalcontexts[MAX_NUM_CID_CONTEXTS][64];


static char *emailbody = NULL;
static char *emailsubject = NULL;
static char *pagerbody = NULL;
static char *pagersubject = NULL;
static char fromstring[100];
static char pagerfromstring[100];
static char emailtitle[100];
static char charset[32] = "ISO-8859-1";

static unsigned char adsifdn[4] = "\x00\x00\x00\x0F";
static unsigned char adsisec[4] = "\x9B\xDB\xF7\xAC";
static int adsiver = 1;
static char emaildateformat[32] = "%A, %B %d, %Y at %r";

/* Forward declarations - generic */
static int open_mailbox(struct vm_state *vms, struct ast_vm_user *vmu,int box);
static int advanced_options(struct ast_channel *chan, struct ast_vm_user *vmu, struct vm_state *vms, int msg, int option, signed char record_gain);
static int dialout(struct ast_channel *chan, struct ast_vm_user *vmu, char *num, char *outgoing_context);
static int play_record_review(struct ast_channel *chan, char *playfile, char *recordfile, int maxtime,
			char *fmt, int outsidecaller, struct ast_vm_user *vmu, int *duration, const char *unlockdir,
			signed char record_gain, struct vm_state *vms);
static int vm_tempgreeting(struct ast_channel *chan, struct ast_vm_user *vmu, struct vm_state *vms, char *fmtc, signed char record_gain);
static int vm_play_folder_name(struct ast_channel *chan, char *mbox);
static int notify_new_message(struct ast_channel *chan, struct ast_vm_user *vmu, int msgnum, long duration, char *fmt, char *cidnum, char *cidname);
static void make_email_file(FILE *p, char *srcemail, struct ast_vm_user *vmu, int msgnum, char *context, char *mailbox, char *cidnum, char *cidname, char *attach, char *format, int duration, int attach_user_voicemail, struct ast_channel *chan, const char *category, int imap);
static void apply_options(struct ast_vm_user *vmu, const char *options);

#if !(defined(ODBC_STORAGE) || defined(IMAP_STORAGE))
static int __has_voicemail(const char *context, const char *mailbox, const char *folder, int shortcircuit);
#endif



static void populate_defaults(struct ast_vm_user *vmu)
{
	ast_copy_flags(vmu, (&globalflags), AST_FLAGS_ALL);	
	if (saydurationminfo)
		vmu->saydurationm = saydurationminfo;
	if (callcontext)
		ast_copy_string(vmu->callback, callcontext, sizeof(vmu->callback));
	if (dialcontext)
		ast_copy_string(vmu->dialout, dialcontext, sizeof(vmu->dialout));
	if (exitcontext)
		ast_copy_string(vmu->exit, exitcontext, sizeof(vmu->exit));
	if (vmmaxsecs)
		vmu->maxsecs = vmmaxsecs;
	if (maxmsg)
		vmu->maxmsg = maxmsg;
	vmu->volgain = volgain;
}

static void apply_option(struct ast_vm_user *vmu, const char *var, const char *value)
{
	int x;
	if (!strcasecmp(var, "attach")) {
		ast_set2_flag(vmu, ast_true(value), VM_ATTACH);
	} else if (!strcasecmp(var, "attachfmt")) {
		ast_copy_string(vmu->attachfmt, value, sizeof(vmu->attachfmt));
	} else if (!strcasecmp(var, "serveremail")) {
		ast_copy_string(vmu->serveremail, value, sizeof(vmu->serveremail));
	} else if (!strcasecmp(var, "language")) {
		ast_copy_string(vmu->language, value, sizeof(vmu->language));
	} else if (!strcasecmp(var, "tz")) {
		ast_copy_string(vmu->zonetag, value, sizeof(vmu->zonetag));
#ifdef IMAP_STORAGE
	} else if (!strcasecmp(var, "imapuser")) {
		ast_copy_string(vmu->imapuser, value, sizeof(vmu->imapuser));
	} else if (!strcasecmp(var, "imappassword")) {
		ast_copy_string(vmu->imappassword, value, sizeof(vmu->imappassword));
#endif
	} else if (!strcasecmp(var, "delete") || !strcasecmp(var, "deletevoicemail")) {
		ast_set2_flag(vmu, ast_true(value), VM_DELETE);	
	} else if (!strcasecmp(var, "saycid")){
		ast_set2_flag(vmu, ast_true(value), VM_SAYCID);	
	} else if (!strcasecmp(var,"sendvoicemail")){
		ast_set2_flag(vmu, ast_true(value), VM_SVMAIL);	
	} else if (!strcasecmp(var, "review")){
		ast_set2_flag(vmu, ast_true(value), VM_REVIEW);
	} else if (!strcasecmp(var, "tempgreetwarn")){
		ast_set2_flag(vmu, ast_true(value), VM_TEMPGREETWARN);	
	} else if (!strcasecmp(var, "operator")){
		ast_set2_flag(vmu, ast_true(value), VM_OPERATOR);	
	} else if (!strcasecmp(var, "envelope")){
		ast_set2_flag(vmu, ast_true(value), VM_ENVELOPE);	
	} else if (!strcasecmp(var, "sayduration")){
		ast_set2_flag(vmu, ast_true(value), VM_SAYDURATION);	
	} else if (!strcasecmp(var, "saydurationm")){
		if (sscanf(value, "%d", &x) == 1) {
			vmu->saydurationm = x;
		} else {
			ast_log(LOG_WARNING, "Invalid min duration for say duration\n");
		}
	} else if (!strcasecmp(var, "forcename")){
		ast_set2_flag(vmu, ast_true(value), VM_FORCENAME);	
	} else if (!strcasecmp(var, "forcegreetings")){
		ast_set2_flag(vmu, ast_true(value), VM_FORCEGREET);	
	} else if (!strcasecmp(var, "callback")) {
		ast_copy_string(vmu->callback, value, sizeof(vmu->callback));
	} else if (!strcasecmp(var, "dialout")) {
		ast_copy_string(vmu->dialout, value, sizeof(vmu->dialout));
	} else if (!strcasecmp(var, "exitcontext")) {
		ast_copy_string(vmu->exit, value, sizeof(vmu->exit));
	} else if (!strcasecmp(var, "maxmessage")) {
		if (vmu->maxsecs <= 0) {
			ast_log(LOG_WARNING, "Invalid max message length of %s. Using global value %i\n", value, vmmaxsecs);
			vmu->maxsecs = vmmaxsecs;
		} else {
			vmu->maxsecs = atoi(value);
		}
	} else if (!strcasecmp(var, "maxmsg")) {
		vmu->maxmsg = atoi(value);
		if (vmu->maxmsg <= 0) {
			ast_log(LOG_WARNING, "Invalid number of messages per folder maxmsg=%s. Using default value %i\n", value, MAXMSG);
			vmu->maxmsg = MAXMSG;
		} else if (vmu->maxmsg > MAXMSGLIMIT) {
			ast_log(LOG_WARNING, "Maximum number of messages per folder is %i. Cannot accept value maxmsg=%s\n", MAXMSGLIMIT, value);
			vmu->maxmsg = MAXMSGLIMIT;
		}
	} else if (!strcasecmp(var, "volgain")) {
		sscanf(value, "%lf", &vmu->volgain);
	} else if (!strcasecmp(var, "options")) {
		apply_options(vmu, value);
	}
}

static int change_password_realtime(struct ast_vm_user *vmu, const char *password)
{
	int res;
	if (!ast_strlen_zero(vmu->uniqueid)) {
		res = ast_update_realtime("voicemail", "uniqueid", vmu->uniqueid, "password", password, NULL);
		if (res > 0) {
			ast_copy_string(vmu->password, password, sizeof(vmu->password));
			res = 0;
		} else if (!res) {
			res = -1;
		}
		return res;
	}
	return -1;
}

static void apply_options(struct ast_vm_user *vmu, const char *options)
{	/* Destructively Parse options and apply */
	char *stringp;
	char *s;
	char *var, *value;
	stringp = ast_strdupa(options);
	while ((s = strsep(&stringp, "|"))) {
		value = s;
		if ((var = strsep(&value, "=")) && value) {
			apply_option(vmu, var, value);
		}
	}	
}

static void apply_options_full(struct ast_vm_user *retval, struct ast_variable *var)
{
	struct ast_variable *tmp;
	tmp = var;
	while (tmp) {
		if (!strcasecmp(tmp->name, "vmsecret")) {
			ast_copy_string(retval->password, tmp->value, sizeof(retval->password));
		} else if (!strcasecmp(tmp->name, "secret") || !strcasecmp(tmp->name, "password")) { /* don't overwrite vmsecret if it exists */
			if (ast_strlen_zero(retval->password))
				ast_copy_string(retval->password, tmp->value, sizeof(retval->password));
		} else if (!strcasecmp(tmp->name, "uniqueid")) {
			ast_copy_string(retval->uniqueid, tmp->value, sizeof(retval->uniqueid));
		} else if (!strcasecmp(tmp->name, "pager")) {
			ast_copy_string(retval->pager, tmp->value, sizeof(retval->pager));
		} else if (!strcasecmp(tmp->name, "email")) {
			ast_copy_string(retval->email, tmp->value, sizeof(retval->email));
		} else if (!strcasecmp(tmp->name, "fullname")) {
			ast_copy_string(retval->fullname, tmp->value, sizeof(retval->fullname));
		} else if (!strcasecmp(tmp->name, "context")) {
			ast_copy_string(retval->context, tmp->value, sizeof(retval->context));
#ifdef IMAP_STORAGE
		} else if (!strcasecmp(tmp->name, "imapuser")) {
			ast_copy_string(retval->imapuser, tmp->value, sizeof(retval->imapuser));
		} else if (!strcasecmp(tmp->name, "imappassword")) {
			ast_copy_string(retval->imappassword, tmp->value, sizeof(retval->imappassword));
#endif
		} else
			apply_option(retval, tmp->name, tmp->value);
		tmp = tmp->next;
	} 
}

static struct ast_vm_user *find_user_realtime(struct ast_vm_user *ivm, const char *context, const char *mailbox)
{
	struct ast_variable *var;
	struct ast_vm_user *retval;

	if ((retval = (ivm ? ivm : ast_calloc(1, sizeof(*retval))))) {
		if (!ivm)
			ast_set_flag(retval, VM_ALLOCED);	
		else
			memset(retval, 0, sizeof(*retval));
		if (mailbox) 
			ast_copy_string(retval->mailbox, mailbox, sizeof(retval->mailbox));
		populate_defaults(retval);
		if (!context && ast_test_flag((&globalflags), VM_SEARCH))
			var = ast_load_realtime("voicemail", "mailbox", mailbox, NULL);
		else
			var = ast_load_realtime("voicemail", "mailbox", mailbox, "context", context, NULL);
		if (var) {
			apply_options_full(retval, var);
			ast_variables_destroy(var);
		} else { 
			if (!ivm) 
				free(retval);
			retval = NULL;
		}	
	} 
	return retval;
}

static struct ast_vm_user *find_user(struct ast_vm_user *ivm, const char *context, const char *mailbox)
{
	/* This function could be made to generate one from a database, too */
	struct ast_vm_user *vmu=NULL, *cur;
	AST_LIST_LOCK(&users);

	if (!context && !ast_test_flag((&globalflags), VM_SEARCH))
		context = "default";

	AST_LIST_TRAVERSE(&users, cur, list) {
		if (ast_test_flag((&globalflags), VM_SEARCH) && !strcasecmp(mailbox, cur->mailbox))
			break;
		if (context && (!strcasecmp(context, cur->context)) && (!strcasecmp(mailbox, cur->mailbox)))
			break;
	}
	if (cur) {
		/* Make a copy, so that on a reload, we have no race */
		if ((vmu = (ivm ? ivm : ast_malloc(sizeof(*vmu))))) {
			memcpy(vmu, cur, sizeof(*vmu));
			ast_set2_flag(vmu, !ivm, VM_ALLOCED);
			AST_LIST_NEXT(vmu, list) = NULL;
		}
	} else
		vmu = find_user_realtime(ivm, context, mailbox);
	AST_LIST_UNLOCK(&users);
	return vmu;
}

static int reset_user_pw(const char *context, const char *mailbox, const char *newpass)
{
	/* This function could be made to generate one from a database, too */
	struct ast_vm_user *cur;
	int res = -1;
	AST_LIST_LOCK(&users);
	AST_LIST_TRAVERSE(&users, cur, list) {
		if ((!context || !strcasecmp(context, cur->context)) &&
			(!strcasecmp(mailbox, cur->mailbox)))
				break;
	}
	if (cur) {
		ast_copy_string(cur->password, newpass, sizeof(cur->password));
		res = 0;
	}
	AST_LIST_UNLOCK(&users);
	return res;
}

static void vm_change_password(struct ast_vm_user *vmu, const char *newpassword)
{
	struct ast_config   *cfg=NULL;
	struct ast_variable *var=NULL;
	struct ast_category *cat=NULL;
	char *category=NULL, *value=NULL, *new=NULL;
	const char *tmp=NULL;
					
	if (!change_password_realtime(vmu, newpassword))
		return;

	/* check voicemail.conf */
	if ((cfg = ast_config_load_with_comments(VOICEMAIL_CONFIG))) {
		while ((category = ast_category_browse(cfg, category))) {
			if (!strcasecmp(category, vmu->context)) {
				if (!(tmp = ast_variable_retrieve(cfg, category, vmu->mailbox))) {
					ast_log(LOG_WARNING, "We could not find the mailbox.\n");
					break;
				}
				value = strstr(tmp,",");
				if (!value) {
					ast_log(LOG_WARNING, "variable has bad format.\n");
					break;
				}
				new = alloca((strlen(value)+strlen(newpassword)+1));
				sprintf(new,"%s%s", newpassword, value);
				if (!(cat = ast_category_get(cfg, category))) {
					ast_log(LOG_WARNING, "Failed to get category structure.\n");
					break;
				}
				ast_variable_update(cat, vmu->mailbox, new, NULL);
			}
		}
		/* save the results */
		reset_user_pw(vmu->context, vmu->mailbox, newpassword);
		ast_copy_string(vmu->password, newpassword, sizeof(vmu->password));
		config_text_file_save(VOICEMAIL_CONFIG, cfg, "AppVoicemail");
	}
	category = NULL;
	var = NULL;
	/* check users.conf and update the password stored for the mailbox*/
	/* if no vmsecret entry exists create one. */
	if ((cfg = ast_config_load_with_comments("users.conf"))) {
		if (option_debug > 3)
			ast_log(LOG_DEBUG, "we are looking for %s\n", vmu->mailbox);
		while ((category = ast_category_browse(cfg, category))) {
			if (option_debug > 3)
				ast_log(LOG_DEBUG, "users.conf: %s\n", category);
			if (!strcasecmp(category, vmu->mailbox)) {
				if (!(tmp = ast_variable_retrieve(cfg, category, "vmsecret"))) {
					if (option_debug > 3)
						ast_log(LOG_DEBUG, "looks like we need to make vmsecret!\n");
					var = ast_variable_new("vmsecret", newpassword);
				} 
				new = alloca(strlen(newpassword)+1);
				sprintf(new, "%s", newpassword);
				if (!(cat = ast_category_get(cfg, category))) {
					if (option_debug > 3)
						ast_log(LOG_DEBUG, "failed to get category!\n");
					break;
				}
				if (!var)		
					ast_variable_update(cat, "vmsecret", new, NULL);
				else
					ast_variable_append(cat, var);
			}
		}
		/* save the results and clean things up */
		reset_user_pw(vmu->context, vmu->mailbox, newpassword);	
		ast_copy_string(vmu->password, newpassword, sizeof(vmu->password));
		config_text_file_save("users.conf", cfg, "AppVoicemail");
	}
}

static void vm_change_password_shell(struct ast_vm_user *vmu, char *newpassword)
{
	char buf[255];
	snprintf(buf,255,"%s %s %s %s",ext_pass_cmd,vmu->context,vmu->mailbox,newpassword);
	if (!ast_safe_system(buf))
		ast_copy_string(vmu->password, newpassword, sizeof(vmu->password));
}

static int make_dir(char *dest, int len, const char *context, const char *ext, const char *folder)
{
	return snprintf(dest, len, "%s%s/%s/%s", VM_SPOOL_DIR, context, ext, folder);
}

#ifdef IMAP_STORAGE
static int make_gsm_file(char *dest, char *imapuser, char *dir, int num)
{
	if (mkdir(dir, 01777) && (errno != EEXIST)) {
		ast_log(LOG_WARNING, "mkdir '%s' failed: %s\n", dir, strerror(errno));
		return sprintf(dest, "%s/msg%04d", dir, num);
	}
	/* return sprintf(dest, "%s/s/msg%04d", dir, imapuser, num); */
	return sprintf(dest, "%s/msg%04d", dir, num);
}

static void vm_imap_delete(int msgnum, struct vm_state *vms)
{
	unsigned long messageNum = 0;
	char arg[10];

	/* find real message number based on msgnum */
	/* this may be an index into vms->msgArray based on the msgnum. */

	messageNum = vms->msgArray[msgnum];
	if (messageNum == 0) {
		ast_log(LOG_WARNING, "msgnum %d, mailbox message %lu is zero.\n",msgnum,messageNum);
		return;
	}
	if(option_debug > 2)
		ast_log(LOG_DEBUG, "deleting msgnum %d, which is mailbox message %lu\n",msgnum,messageNum);
	/* delete message */
	sprintf (arg,"%lu",messageNum);
	mail_setflag (vms->mailstream,arg,"\\DELETED");
}

#endif
static int make_file(char *dest, int len, char *dir, int num)
{
	return snprintf(dest, len, "%s/msg%04d", dir, num);
}

/*! \brief basically mkdir -p $dest/$context/$ext/$folder
 * \param dest    String. base directory.
 * \param len     Length of dest.
 * \param context String. Ignored if is null or empty string.
 * \param ext     String. Ignored if is null or empty string.
 * \param folder  String. Ignored if is null or empty string. 
 * \return -1 on failure, 0 on success.
 */
static int create_dirpath(char *dest, int len, const char *context, const char *ext, const char *folder)
{
	mode_t	mode = VOICEMAIL_DIR_MODE;

	if (!ast_strlen_zero(context)) {
		make_dir(dest, len, context, "", "");
		if (mkdir(dest, mode) && errno != EEXIST) {
			ast_log(LOG_WARNING, "mkdir '%s' failed: %s\n", dest, strerror(errno));
			return -1;
		}
	}
	if (!ast_strlen_zero(ext)) {
		make_dir(dest, len, context, ext, "");
		if (mkdir(dest, mode) && errno != EEXIST) {
			ast_log(LOG_WARNING, "mkdir '%s' failed: %s\n", dest, strerror(errno));
			return -1;
		}
	}
	if (!ast_strlen_zero(folder)) {
		make_dir(dest, len, context, ext, folder);
		if (mkdir(dest, mode) && errno != EEXIST) {
			ast_log(LOG_WARNING, "mkdir '%s' failed: %s\n", dest, strerror(errno));
			return -1;
		}
	}
	return 0;
}

/*! \brief Lock file path
    only return failure if ast_lock_path returns 'timeout',
   not if the path does not exist or any other reason
*/
static int vm_lock_path(const char *path)
{
	switch (ast_lock_path(path)) {
	case AST_LOCK_TIMEOUT:
		return -1;
	default:
		return 0;
	}
}


#ifdef ODBC_STORAGE
static int retrieve_file(char *dir, int msgnum)
{
	int x = 0;
	int res;
	int fd=-1;
	size_t fdlen = 0;
	void *fdm = MAP_FAILED;
	SQLSMALLINT colcount=0;
	SQLHSTMT stmt;
	char sql[PATH_MAX];
	char fmt[80]="";
	char *c;
	char coltitle[256];
	SQLSMALLINT collen;
	SQLSMALLINT datatype;
	SQLSMALLINT decimaldigits;
	SQLSMALLINT nullable;
	SQLULEN colsize;
	SQLLEN colsize2;
	FILE *f=NULL;
	char rowdata[80];
	char fn[PATH_MAX];
	char full_fn[PATH_MAX];
	char msgnums[80];
	
	struct odbc_obj *obj;
	obj = ast_odbc_request_obj(odbc_database, 0);
	if (obj) {
		ast_copy_string(fmt, vmfmts, sizeof(fmt));
		c = strchr(fmt, '|');
		if (c)
			*c = '\0';
		if (!strcasecmp(fmt, "wav49"))
			strcpy(fmt, "WAV");
		snprintf(msgnums, sizeof(msgnums),"%d", msgnum);
		if (msgnum > -1)
			make_file(fn, sizeof(fn), dir, msgnum);
		else
			ast_copy_string(fn, dir, sizeof(fn));
		snprintf(full_fn, sizeof(full_fn), "%s.txt", fn);
		
		if (!(f = fopen(full_fn, "w+"))) {
		        ast_log(LOG_WARNING, "Failed to open/create '%s'\n", full_fn);
		        goto yuck;
		}
		
		snprintf(full_fn, sizeof(full_fn), "%s.%s", fn, fmt);
		res = SQLAllocHandle(SQL_HANDLE_STMT, obj->con, &stmt);
		if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
			ast_log(LOG_WARNING, "SQL Alloc Handle failed!\n");
			ast_odbc_release_obj(obj);
			goto yuck;
		}
		snprintf(sql, sizeof(sql), "SELECT * FROM %s WHERE dir=? AND msgnum=?",odbc_table);
		res = SQLPrepare(stmt, (unsigned char *)sql, SQL_NTS);
		if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
			ast_log(LOG_WARNING, "SQL Prepare failed![%s]\n", sql);
			SQLFreeHandle (SQL_HANDLE_STMT, stmt);
			ast_odbc_release_obj(obj);
			goto yuck;
		}
		SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_CHAR, strlen(dir), 0, (void *)dir, 0, NULL);
		SQLBindParameter(stmt, 2, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_CHAR, strlen(msgnums), 0, (void *)msgnums, 0, NULL);
		res = ast_odbc_smart_execute(obj, stmt);
		if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
			ast_log(LOG_WARNING, "SQL Execute error!\n[%s]\n\n", sql);
			SQLFreeHandle (SQL_HANDLE_STMT, stmt);
			ast_odbc_release_obj(obj);
			goto yuck;
		}
		res = SQLFetch(stmt);
		if (res == SQL_NO_DATA) {
			SQLFreeHandle (SQL_HANDLE_STMT, stmt);
			ast_odbc_release_obj(obj);
			goto yuck;
		}
		else if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
			ast_log(LOG_WARNING, "SQL Fetch error!\n[%s]\n\n", sql);
			SQLFreeHandle (SQL_HANDLE_STMT, stmt);
			ast_odbc_release_obj(obj);
			goto yuck;
		}
		fd = open(full_fn, O_RDWR | O_CREAT | O_TRUNC, VOICEMAIL_FILE_MODE);
		if (fd < 0) {
			ast_log(LOG_WARNING, "Failed to write '%s': %s\n", full_fn, strerror(errno));
			SQLFreeHandle (SQL_HANDLE_STMT, stmt);
			ast_odbc_release_obj(obj);
			goto yuck;
		}
		res = SQLNumResultCols(stmt, &colcount);
		if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {	
			ast_log(LOG_WARNING, "SQL Column Count error!\n[%s]\n\n", sql);
			SQLFreeHandle (SQL_HANDLE_STMT, stmt);
			ast_odbc_release_obj(obj);
			goto yuck;
		}
		if (f) 
			fprintf(f, "[message]\n");
		for (x=0;x<colcount;x++) {
			rowdata[0] = '\0';
			collen = sizeof(coltitle);
			res = SQLDescribeCol(stmt, x + 1, (unsigned char *)coltitle, sizeof(coltitle), &collen, 
						&datatype, &colsize, &decimaldigits, &nullable);
			if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
				ast_log(LOG_WARNING, "SQL Describe Column error!\n[%s]\n\n", sql);
				SQLFreeHandle (SQL_HANDLE_STMT, stmt);
				ast_odbc_release_obj(obj);
				goto yuck;
			}
			if (!strcasecmp(coltitle, "recording")) {
				off_t offset;
				res = SQLGetData(stmt, x + 1, SQL_BINARY, rowdata, 0, &colsize2);
				fdlen = colsize2;
				if (fd > -1) {
					char tmp[1]="";
					lseek(fd, fdlen - 1, SEEK_SET);
					if (write(fd, tmp, 1) != 1) {
						close(fd);
						fd = -1;
						continue;
					}
					/* Read out in small chunks */
					for (offset = 0; offset < colsize2; offset += CHUNKSIZE) {
						if ((fdm = mmap(NULL, CHUNKSIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, offset)) == MAP_FAILED) {
							ast_log(LOG_WARNING, "Could not mmap the output file: %s (%d)\n", strerror(errno), errno);
							SQLFreeHandle(SQL_HANDLE_STMT, stmt);
							ast_odbc_release_obj(obj);
							goto yuck;
						} else {
							res = SQLGetData(stmt, x + 1, SQL_BINARY, fdm, CHUNKSIZE, NULL);
							munmap(fdm, CHUNKSIZE);
							if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
								ast_log(LOG_WARNING, "SQL Get Data error!\n[%s]\n\n", sql);
								unlink(full_fn);
								SQLFreeHandle(SQL_HANDLE_STMT, stmt);
								ast_odbc_release_obj(obj);
								goto yuck;
							}
						}
					}
					truncate(full_fn, fdlen);
				}
			} else {
				res = SQLGetData(stmt, x + 1, SQL_CHAR, rowdata, sizeof(rowdata), NULL);
				if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
					ast_log(LOG_WARNING, "SQL Get Data error!\n[%s]\n\n", sql);
					SQLFreeHandle (SQL_HANDLE_STMT, stmt);
					ast_odbc_release_obj(obj);
					goto yuck;
				}
				if (strcasecmp(coltitle, "msgnum") && strcasecmp(coltitle, "dir") && f)
					fprintf(f, "%s=%s\n", coltitle, rowdata);
			}
		}
		SQLFreeHandle (SQL_HANDLE_STMT, stmt);
		ast_odbc_release_obj(obj);
	} else
		ast_log(LOG_WARNING, "Failed to obtain database object for '%s'!\n", odbc_database);
yuck:	
	if (f)
		fclose(f);
	if (fd > -1)
		close(fd);
	return x - 1;
}

static int remove_file(char *dir, int msgnum)
{
	char fn[PATH_MAX];
	char full_fn[PATH_MAX];
	char msgnums[80];
	
	if (msgnum > -1) {
		snprintf(msgnums, sizeof(msgnums), "%d", msgnum);
		make_file(fn, sizeof(fn), dir, msgnum);
	} else
		ast_copy_string(fn, dir, sizeof(fn));
	ast_filedelete(fn, NULL);	
	snprintf(full_fn, sizeof(full_fn), "%s.txt", fn);
	unlink(full_fn);
	return 0;
}

static int last_message_index(struct ast_vm_user *vmu, char *dir)
{
	int x = 0;
	int res;
	SQLHSTMT stmt;
	char sql[PATH_MAX];
	char rowdata[20];
	
	struct odbc_obj *obj;
	obj = ast_odbc_request_obj(odbc_database, 0);
	if (obj) {
		res = SQLAllocHandle(SQL_HANDLE_STMT, obj->con, &stmt);
		if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
			ast_log(LOG_WARNING, "SQL Alloc Handle failed!\n");
			ast_odbc_release_obj(obj);
			goto yuck;
		}
		snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM %s WHERE dir=?",odbc_table);
		res = SQLPrepare(stmt, (unsigned char *)sql, SQL_NTS);
		if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
			ast_log(LOG_WARNING, "SQL Prepare failed![%s]\n", sql);
			SQLFreeHandle (SQL_HANDLE_STMT, stmt);
			ast_odbc_release_obj(obj);
			goto yuck;
		}
		SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_CHAR, strlen(dir), 0, (void *)dir, 0, NULL);
		res = ast_odbc_smart_execute(obj, stmt);
		if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
			ast_log(LOG_WARNING, "SQL Execute error!\n[%s]\n\n", sql);
			SQLFreeHandle (SQL_HANDLE_STMT, stmt);
			ast_odbc_release_obj(obj);
			goto yuck;
		}
		res = SQLFetch(stmt);
		if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
			ast_log(LOG_WARNING, "SQL Fetch error!\n[%s]\n\n", sql);
			SQLFreeHandle (SQL_HANDLE_STMT, stmt);
			ast_odbc_release_obj(obj);
			goto yuck;
		}
		res = SQLGetData(stmt, 1, SQL_CHAR, rowdata, sizeof(rowdata), NULL);
		if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
			ast_log(LOG_WARNING, "SQL Get Data error!\n[%s]\n\n", sql);
			SQLFreeHandle (SQL_HANDLE_STMT, stmt);
			ast_odbc_release_obj(obj);
			goto yuck;
		}
		if (sscanf(rowdata, "%d", &x) != 1)
			ast_log(LOG_WARNING, "Failed to read message count!\n");
		SQLFreeHandle (SQL_HANDLE_STMT, stmt);
		ast_odbc_release_obj(obj);
	} else
		ast_log(LOG_WARNING, "Failed to obtain database object for '%s'!\n", odbc_database);
yuck:	
	return x - 1;
}

static int message_exists(char *dir, int msgnum)
{
	int x = 0;
	int res;
	SQLHSTMT stmt;
	char sql[PATH_MAX];
	char rowdata[20];
	char msgnums[20];
	
	struct odbc_obj *obj;
	obj = ast_odbc_request_obj(odbc_database, 0);
	if (obj) {
		snprintf(msgnums, sizeof(msgnums), "%d", msgnum);
		res = SQLAllocHandle(SQL_HANDLE_STMT, obj->con, &stmt);
		if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
			ast_log(LOG_WARNING, "SQL Alloc Handle failed!\n");
			ast_odbc_release_obj(obj);
			goto yuck;
		}
		snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM %s WHERE dir=? AND msgnum=?",odbc_table);
		res = SQLPrepare(stmt, (unsigned char *)sql, SQL_NTS);
		if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
			ast_log(LOG_WARNING, "SQL Prepare failed![%s]\n", sql);
			SQLFreeHandle (SQL_HANDLE_STMT, stmt);
			ast_odbc_release_obj(obj);
			goto yuck;
		}
		SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_CHAR, strlen(dir), 0, (void *)dir, 0, NULL);
		SQLBindParameter(stmt, 2, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_CHAR, strlen(msgnums), 0, (void *)msgnums, 0, NULL);
		res = ast_odbc_smart_execute(obj, stmt);
		if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
			ast_log(LOG_WARNING, "SQL Execute error!\n[%s]\n\n", sql);
			SQLFreeHandle (SQL_HANDLE_STMT, stmt);
			ast_odbc_release_obj(obj);
			goto yuck;
		}
		res = SQLFetch(stmt);
		if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
			ast_log(LOG_WARNING, "SQL Fetch error!\n[%s]\n\n", sql);
			SQLFreeHandle (SQL_HANDLE_STMT, stmt);
			ast_odbc_release_obj(obj);
			goto yuck;
		}
		res = SQLGetData(stmt, 1, SQL_CHAR, rowdata, sizeof(rowdata), NULL);
		if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
			ast_log(LOG_WARNING, "SQL Get Data error!\n[%s]\n\n", sql);
			SQLFreeHandle (SQL_HANDLE_STMT, stmt);
			ast_odbc_release_obj(obj);
			goto yuck;
		}
		if (sscanf(rowdata, "%d", &x) != 1)
			ast_log(LOG_WARNING, "Failed to read message count!\n");
		SQLFreeHandle (SQL_HANDLE_STMT, stmt);
		ast_odbc_release_obj(obj);
	} else
		ast_log(LOG_WARNING, "Failed to obtain database object for '%s'!\n", odbc_database);
yuck:	
	return x;
}

static int count_messages(struct ast_vm_user *vmu, char *dir)
{
	return last_message_index(vmu, dir) + 1;
}

static void delete_file(char *sdir, int smsg)
{
	int res;
	SQLHSTMT stmt;
	char sql[PATH_MAX];
	char msgnums[20];
	
	struct odbc_obj *obj;
	obj = ast_odbc_request_obj(odbc_database, 0);
	if (obj) {
		snprintf(msgnums, sizeof(msgnums), "%d", smsg);
		res = SQLAllocHandle(SQL_HANDLE_STMT, obj->con, &stmt);
		if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
			ast_log(LOG_WARNING, "SQL Alloc Handle failed!\n");
			ast_odbc_release_obj(obj);
			goto yuck;
		}
		snprintf(sql, sizeof(sql), "DELETE FROM %s WHERE dir=? AND msgnum=?",odbc_table);
		res = SQLPrepare(stmt, (unsigned char *)sql, SQL_NTS);
		if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
			ast_log(LOG_WARNING, "SQL Prepare failed![%s]\n", sql);
			SQLFreeHandle (SQL_HANDLE_STMT, stmt);
			ast_odbc_release_obj(obj);
			goto yuck;
		}
		SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_CHAR, strlen(sdir), 0, (void *)sdir, 0, NULL);
		SQLBindParameter(stmt, 2, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_CHAR, strlen(msgnums), 0, (void *)msgnums, 0, NULL);
		res = ast_odbc_smart_execute(obj, stmt);
		if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
			ast_log(LOG_WARNING, "SQL Execute error!\n[%s]\n\n", sql);
			SQLFreeHandle (SQL_HANDLE_STMT, stmt);
			ast_odbc_release_obj(obj);
			goto yuck;
		}
		SQLFreeHandle (SQL_HANDLE_STMT, stmt);
		ast_odbc_release_obj(obj);
	} else
		ast_log(LOG_WARNING, "Failed to obtain database object for '%s'!\n", odbc_database);
yuck:
	return;	
}

static void copy_file(char *sdir, int smsg, char *ddir, int dmsg, char *dmailboxuser, char *dmailboxcontext)
{
	int res;
	SQLHSTMT stmt;
	char sql[512];
	char msgnums[20];
	char msgnumd[20];
	struct odbc_obj *obj;

	delete_file(ddir, dmsg);
	obj = ast_odbc_request_obj(odbc_database, 0);
	if (obj) {
		snprintf(msgnums, sizeof(msgnums), "%d", smsg);
		snprintf(msgnumd, sizeof(msgnumd), "%d", dmsg);
		res = SQLAllocHandle(SQL_HANDLE_STMT, obj->con, &stmt);
		if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
			ast_log(LOG_WARNING, "SQL Alloc Handle failed!\n");
			ast_odbc_release_obj(obj);
			goto yuck;
		}
		snprintf(sql, sizeof(sql), "INSERT INTO %s (dir, msgnum, context, macrocontext, callerid, origtime, duration, recording, mailboxuser, mailboxcontext) SELECT ?,?,context,macrocontext,callerid,origtime,duration,recording,?,? FROM %s WHERE dir=? AND msgnum=?",odbc_table,odbc_table); 
		res = SQLPrepare(stmt, (unsigned char *)sql, SQL_NTS);
		if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
			ast_log(LOG_WARNING, "SQL Prepare failed![%s]\n", sql);
			SQLFreeHandle (SQL_HANDLE_STMT, stmt);
			ast_odbc_release_obj(obj);
			goto yuck;
		}
		SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_CHAR, strlen(ddir), 0, (void *)ddir, 0, NULL);
		SQLBindParameter(stmt, 2, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_CHAR, strlen(msgnumd), 0, (void *)msgnumd, 0, NULL);
		SQLBindParameter(stmt, 3, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_CHAR, strlen(dmailboxuser), 0, (void *)dmailboxuser, 0, NULL);
		SQLBindParameter(stmt, 4, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_CHAR, strlen(dmailboxcontext), 0, (void *)dmailboxcontext, 0, NULL);
		SQLBindParameter(stmt, 5, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_CHAR, strlen(sdir), 0, (void *)sdir, 0, NULL);
		SQLBindParameter(stmt, 6, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_CHAR, strlen(msgnums), 0, (void *)msgnums, 0, NULL);
		res = ast_odbc_smart_execute(obj, stmt);
		if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
			ast_log(LOG_WARNING, "SQL Execute error!\n[%s] (You probably don't have MySQL 4.1 or later installed)\n\n", sql);
			SQLFreeHandle (SQL_HANDLE_STMT, stmt);
			ast_odbc_release_obj(obj);
			goto yuck;
		}
		SQLFreeHandle (SQL_HANDLE_STMT, stmt);
		ast_odbc_release_obj(obj);
	} else
		ast_log(LOG_WARNING, "Failed to obtain database object for '%s'!\n", odbc_database);
yuck:
	return;	
}

static int store_file(char *dir, char *mailboxuser, char *mailboxcontext, int msgnum)
{
	int x = 0;
	int res;
	int fd = -1;
	void *fdm = MAP_FAILED;
	size_t fdlen = -1;
	SQLHSTMT stmt;
	SQLINTEGER len;
	char sql[PATH_MAX];
	char msgnums[20];
	char fn[PATH_MAX];
	char full_fn[PATH_MAX];
	char fmt[80]="";
	char *c;
	const char *context="", *macrocontext="", *callerid="", *origtime="", *duration="";
	const char *category = "";
	struct ast_config *cfg=NULL;
	struct odbc_obj *obj;

	delete_file(dir, msgnum);
	obj = ast_odbc_request_obj(odbc_database, 0);
	if (obj) {
		ast_copy_string(fmt, vmfmts, sizeof(fmt));
		c = strchr(fmt, '|');
		if (c)
			*c = '\0';
		if (!strcasecmp(fmt, "wav49"))
			strcpy(fmt, "WAV");
		snprintf(msgnums, sizeof(msgnums),"%d", msgnum);
		if (msgnum > -1)
			make_file(fn, sizeof(fn), dir, msgnum);
		else
			ast_copy_string(fn, dir, sizeof(fn));
		snprintf(full_fn, sizeof(full_fn), "%s.txt", fn);
		cfg = ast_config_load(full_fn);
		snprintf(full_fn, sizeof(full_fn), "%s.%s", fn, fmt);
		fd = open(full_fn, O_RDWR);
		if (fd < 0) {
			ast_log(LOG_WARNING, "Open of sound file '%s' failed: %s\n", full_fn, strerror(errno));
			ast_odbc_release_obj(obj);
			goto yuck;
		}
		if (cfg) {
			context = ast_variable_retrieve(cfg, "message", "context");
			if (!context) context = "";
			macrocontext = ast_variable_retrieve(cfg, "message", "macrocontext");
			if (!macrocontext) macrocontext = "";
			callerid = ast_variable_retrieve(cfg, "message", "callerid");
			if (!callerid) callerid = "";
			origtime = ast_variable_retrieve(cfg, "message", "origtime");
			if (!origtime) origtime = "";
			duration = ast_variable_retrieve(cfg, "message", "duration");
			if (!duration) duration = "";
			category = ast_variable_retrieve(cfg, "message", "category");
			if (!category) category = "";
		}
		fdlen = lseek(fd, 0, SEEK_END);
		lseek(fd, 0, SEEK_SET);
		printf("Length is %zd\n", fdlen);
		fdm = mmap(NULL, fdlen, PROT_READ | PROT_WRITE, MAP_SHARED,fd, 0);
		if (fdm == MAP_FAILED) {
			ast_log(LOG_WARNING, "Memory map failed!\n");
			ast_odbc_release_obj(obj);
			goto yuck;
		} 
		res = SQLAllocHandle(SQL_HANDLE_STMT, obj->con, &stmt);
		if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
			ast_log(LOG_WARNING, "SQL Alloc Handle failed!\n");
			ast_odbc_release_obj(obj);
			goto yuck;
		}
		if (!ast_strlen_zero(category)) 
			snprintf(sql, sizeof(sql), "INSERT INTO %s (dir,msgnum,recording,context,macrocontext,callerid,origtime,duration,mailboxuser,mailboxcontext,category) VALUES (?,?,?,?,?,?,?,?,?,?,?)",odbc_table); 
		else
			snprintf(sql, sizeof(sql), "INSERT INTO %s (dir,msgnum,recording,context,macrocontext,callerid,origtime,duration,mailboxuser,mailboxcontext) VALUES (?,?,?,?,?,?,?,?,?,?)",odbc_table);
		res = SQLPrepare(stmt, (unsigned char *)sql, SQL_NTS);
		if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
			ast_log(LOG_WARNING, "SQL Prepare failed![%s]\n", sql);
			SQLFreeHandle (SQL_HANDLE_STMT, stmt);
			ast_odbc_release_obj(obj);
			goto yuck;
		}
		len = fdlen; /* SQL_LEN_DATA_AT_EXEC(fdlen); */
		SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_CHAR, strlen(dir), 0, (void *)dir, 0, NULL);
		SQLBindParameter(stmt, 2, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_CHAR, strlen(msgnums), 0, (void *)msgnums, 0, NULL);
		SQLBindParameter(stmt, 3, SQL_PARAM_INPUT, SQL_C_BINARY, SQL_LONGVARBINARY, fdlen, 0, (void *)fdm, fdlen, &len);
		SQLBindParameter(stmt, 4, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_CHAR, strlen(context), 0, (void *)context, 0, NULL);
		SQLBindParameter(stmt, 5, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_CHAR, strlen(macrocontext), 0, (void *)macrocontext, 0, NULL);
		SQLBindParameter(stmt, 6, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_CHAR, strlen(callerid), 0, (void *)callerid, 0, NULL);
		SQLBindParameter(stmt, 7, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_CHAR, strlen(origtime), 0, (void *)origtime, 0, NULL);
		SQLBindParameter(stmt, 8, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_CHAR, strlen(duration), 0, (void *)duration, 0, NULL);
		SQLBindParameter(stmt, 9, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_CHAR, strlen(mailboxuser), 0, (void *)mailboxuser, 0, NULL);
		SQLBindParameter(stmt, 10, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_CHAR, strlen(mailboxcontext), 0, (void *)mailboxcontext, 0, NULL);
		if (!ast_strlen_zero(category))
			SQLBindParameter(stmt, 11, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_CHAR, strlen(category), 0, (void *)category, 0, NULL);
		res = ast_odbc_smart_execute(obj, stmt);
		if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
			ast_log(LOG_WARNING, "SQL Execute error!\n[%s]\n\n", sql);
			SQLFreeHandle (SQL_HANDLE_STMT, stmt);
			ast_odbc_release_obj(obj);
			goto yuck;
		}
		SQLFreeHandle (SQL_HANDLE_STMT, stmt);
		ast_odbc_release_obj(obj);
	} else
		ast_log(LOG_WARNING, "Failed to obtain database object for '%s'!\n", odbc_database);
yuck:	
	if (cfg)
		ast_config_destroy(cfg);
	if (fdm != MAP_FAILED)
		munmap(fdm, fdlen);
	if (fd > -1)
		close(fd);
	return x;
}

static void rename_file(char *sdir, int smsg, char *mailboxuser, char *mailboxcontext, char *ddir, int dmsg)
{
	int res;
	SQLHSTMT stmt;
	char sql[PATH_MAX];
	char msgnums[20];
	char msgnumd[20];
	struct odbc_obj *obj;

	delete_file(ddir, dmsg);
	obj = ast_odbc_request_obj(odbc_database, 0);
	if (obj) {
		snprintf(msgnums, sizeof(msgnums), "%d", smsg);
		snprintf(msgnumd, sizeof(msgnumd), "%d", dmsg);
		res = SQLAllocHandle(SQL_HANDLE_STMT, obj->con, &stmt);
		if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
			ast_log(LOG_WARNING, "SQL Alloc Handle failed!\n");
			ast_odbc_release_obj(obj);
			goto yuck;
		}
		snprintf(sql, sizeof(sql), "UPDATE %s SET dir=?, msgnum=?, mailboxuser=?, mailboxcontext=? WHERE dir=? AND msgnum=?",odbc_table);
		res = SQLPrepare(stmt, (unsigned char *)sql, SQL_NTS);
		if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
			ast_log(LOG_WARNING, "SQL Prepare failed![%s]\n", sql);
			SQLFreeHandle (SQL_HANDLE_STMT, stmt);
			ast_odbc_release_obj(obj);
			goto yuck;
		}
		SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_CHAR, strlen(ddir), 0, (void *)ddir, 0, NULL);
		SQLBindParameter(stmt, 2, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_CHAR, strlen(msgnumd), 0, (void *)msgnumd, 0, NULL);
		SQLBindParameter(stmt, 3, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_CHAR, strlen(mailboxuser), 0, (void *)mailboxuser, 0, NULL);
		SQLBindParameter(stmt, 4, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_CHAR, strlen(mailboxcontext), 0, (void *)mailboxcontext, 0, NULL);
		SQLBindParameter(stmt, 5, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_CHAR, strlen(sdir), 0, (void *)sdir, 0, NULL);
		SQLBindParameter(stmt, 6, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_CHAR, strlen(msgnums), 0, (void *)msgnums, 0, NULL);
		res = ast_odbc_smart_execute(obj, stmt);
		if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
			ast_log(LOG_WARNING, "SQL Execute error!\n[%s]\n\n", sql);
			SQLFreeHandle (SQL_HANDLE_STMT, stmt);
			ast_odbc_release_obj(obj);
			goto yuck;
		}
		SQLFreeHandle (SQL_HANDLE_STMT, stmt);
		ast_odbc_release_obj(obj);
	} else
		ast_log(LOG_WARNING, "Failed to obtain database object for '%s'!\n", odbc_database);
yuck:
	return;	
}

#else
#ifndef IMAP_STORAGE
static int count_messages(struct ast_vm_user *vmu, char *dir)
{
	/* Find all .txt files - even if they are not in sequence from 0000 */

	int vmcount = 0;
	DIR *vmdir = NULL;
	struct dirent *vment = NULL;

	if (vm_lock_path(dir))
		return ERROR_LOCK_PATH;

	if ((vmdir = opendir(dir))) {
		while ((vment = readdir(vmdir))) {
			if (strlen(vment->d_name) > 7 && !strncmp(vment->d_name + 7, ".txt", 4)) 
				vmcount++;
		}
		closedir(vmdir);
	}
	ast_unlock_path(dir);
	
	return vmcount;
}

static void rename_file(char *sfn, char *dfn)
{
	char stxt[PATH_MAX];
	char dtxt[PATH_MAX];
	ast_filerename(sfn,dfn,NULL);
	snprintf(stxt, sizeof(stxt), "%s.txt", sfn);
	snprintf(dtxt, sizeof(dtxt), "%s.txt", dfn);
	rename(stxt, dtxt);
}

static int copy(char *infile, char *outfile)
{
	int ifd;
	int ofd;
	int res;
	int len;
	char buf[4096];

#ifdef HARDLINK_WHEN_POSSIBLE
	/* Hard link if possible; saves disk space & is faster */
	if (link(infile, outfile)) {
#endif
		if ((ifd = open(infile, O_RDONLY)) < 0) {
			ast_log(LOG_WARNING, "Unable to open %s in read-only mode\n", infile);
			return -1;
		}
		if ((ofd = open(outfile, O_WRONLY | O_TRUNC | O_CREAT, VOICEMAIL_FILE_MODE)) < 0) {
			ast_log(LOG_WARNING, "Unable to open %s in write-only mode\n", outfile);
			close(ifd);
			return -1;
		}
		do {
			len = read(ifd, buf, sizeof(buf));
			if (len < 0) {
				ast_log(LOG_WARNING, "Read failed on %s: %s\n", infile, strerror(errno));
				close(ifd);
				close(ofd);
				unlink(outfile);
			}
			if (len) {
				res = write(ofd, buf, len);
				if (errno == ENOMEM || errno == ENOSPC || res != len) {
					ast_log(LOG_WARNING, "Write failed on %s (%d of %d): %s\n", outfile, res, len, strerror(errno));
					close(ifd);
					close(ofd);
					unlink(outfile);
				}
			}
		} while (len);
		close(ifd);
		close(ofd);
		return 0;
#ifdef HARDLINK_WHEN_POSSIBLE
	} else {
		/* Hard link succeeded */
		return 0;
	}
#endif
}

static void copy_file(char *frompath, char *topath)
{
	char frompath2[PATH_MAX], topath2[PATH_MAX];
	ast_filecopy(frompath, topath, NULL);
	snprintf(frompath2, sizeof(frompath2), "%s.txt", frompath);
	snprintf(topath2, sizeof(topath2), "%s.txt", topath);
	copy(frompath2, topath2);
}
#endif

/*! \brief
 * A negative return value indicates an error.
 * \note Should always be called with a lock already set on dir.
 */
static int last_message_index(struct ast_vm_user *vmu, char *dir)
{
	int x;
	unsigned char map[MAXMSGLIMIT] = "";
	DIR *msgdir;
	struct dirent *msgdirent;
	int msgdirint;

	/* Reading the entire directory into a file map scales better than
	 * doing a stat repeatedly on a predicted sequence.  I suspect this
	 * is partially due to stat(2) internally doing a readdir(2) itself to
	 * find each file. */
	msgdir = opendir(dir);
	while ((msgdirent = readdir(msgdir))) {
		if (sscanf(msgdirent->d_name, "msg%d", &msgdirint) == 1 && msgdirint < MAXMSGLIMIT)
			map[msgdirint] = 1;
	}
	closedir(msgdir);

	for (x = 0; x < vmu->maxmsg; x++) {
		if (map[x] == 0)
			break;
	}

	return x - 1;
}

static int vm_delete(char *file)
{
	char *txt;
	int txtsize = 0;

	txtsize = (strlen(file) + 5)*sizeof(char);
	txt = alloca(txtsize);
	/* Sprintf here would safe because we alloca'd exactly the right length,
	 * but trying to eliminate all sprintf's anyhow
	 */
	snprintf(txt, txtsize, "%s.txt", file);
	unlink(txt);
	return ast_filedelete(file, NULL);
}


#endif
static int inbuf(struct baseio *bio, FILE *fi)
{
	int l;

	if (bio->ateof)
		return 0;

	if ((l = fread(bio->iobuf,1,BASEMAXINLINE,fi)) <= 0) {
		if (ferror(fi))
			return -1;

		bio->ateof = 1;
		return 0;
	}

	bio->iolen= l;
	bio->iocp= 0;

	return 1;
}

static int inchar(struct baseio *bio, FILE *fi)
{
	if (bio->iocp>=bio->iolen) {
		if (!inbuf(bio, fi))
			return EOF;
	}

	return bio->iobuf[bio->iocp++];
}

static int ochar(struct baseio *bio, int c, FILE *so)
{
	if (bio->linelength >= BASELINELEN) {
		if (fputs(eol,so) == EOF)
			return -1;

		bio->linelength= 0;
	}

	if (putc(((unsigned char)c),so) == EOF)
		return -1;

	bio->linelength++;

	return 1;
}

static int base_encode(char *filename, FILE *so)
{
	unsigned char dtable[BASEMAXINLINE];
	int i,hiteof= 0;
	FILE *fi;
	struct baseio bio;

	memset(&bio, 0, sizeof(bio));
	bio.iocp = BASEMAXINLINE;

	if (!(fi = fopen(filename, "rb"))) {
		ast_log(LOG_WARNING, "Failed to open log file: %s: %s\n", filename, strerror(errno));
		return -1;
	}

	for (i= 0; i<9; i++) {
		dtable[i]= 'A'+i;
		dtable[i+9]= 'J'+i;
		dtable[26+i]= 'a'+i;
		dtable[26+i+9]= 'j'+i;
	}
	for (i= 0; i<8; i++) {
		dtable[i+18]= 'S'+i;
		dtable[26+i+18]= 's'+i;
	}
	for (i= 0; i<10; i++) {
		dtable[52+i]= '0'+i;
	}
	dtable[62]= '+';
	dtable[63]= '/';

	while (!hiteof){
		unsigned char igroup[3], ogroup[4];
		int c,n;

		igroup[0]= igroup[1]= igroup[2]= 0;

		for (n= 0;n<3;n++) {
			if ((c = inchar(&bio, fi)) == EOF) {
				hiteof= 1;
				break;
			}

			igroup[n]= (unsigned char)c;
		}

		if (n> 0) {
			ogroup[0]= dtable[igroup[0]>>2];
			ogroup[1]= dtable[((igroup[0]&3)<<4) | (igroup[1]>>4)];
			ogroup[2]= dtable[((igroup[1]&0xF)<<2) | (igroup[2]>>6)];
			ogroup[3]= dtable[igroup[2]&0x3F];

			if (n<3) {
				ogroup[3]= '=';

				if (n<2)
					ogroup[2]= '=';
			}

			for (i= 0;i<4;i++)
				ochar(&bio, ogroup[i], so);
		}
	}

	if (fputs(eol,so) == EOF)
		return 0;

	fclose(fi);

	return 1;
}

static void prep_email_sub_vars(struct ast_channel *ast, struct ast_vm_user *vmu, int msgnum, char *context, char *mailbox, char *cidnum, char *cidname, char *dur, char *date, char *passdata, size_t passdatasize, const char *category)
{
	char callerid[256];
	/* Prepare variables for substitution in email body and subject */
	pbx_builtin_setvar_helper(ast, "VM_NAME", vmu->fullname);
	pbx_builtin_setvar_helper(ast, "VM_DUR", dur);
	snprintf(passdata, passdatasize, "%d", msgnum);
	pbx_builtin_setvar_helper(ast, "VM_MSGNUM", passdata);
	pbx_builtin_setvar_helper(ast, "VM_CONTEXT", context);
	pbx_builtin_setvar_helper(ast, "VM_MAILBOX", mailbox);
	pbx_builtin_setvar_helper(ast, "VM_CALLERID", ast_callerid_merge(callerid, sizeof(callerid), cidname, cidnum, "Unknown Caller"));
	pbx_builtin_setvar_helper(ast, "VM_CIDNAME", (cidname ? cidname : "an unknown caller"));
	pbx_builtin_setvar_helper(ast, "VM_CIDNUM", (cidnum ? cidnum : "an unknown caller"));
	pbx_builtin_setvar_helper(ast, "VM_DATE", date);
	pbx_builtin_setvar_helper(ast, "VM_CATEGORY", category ? ast_strdupa(category) : "no category");
}

static char *quote(const char *from, char *to, size_t len)
{
	char *ptr = to;
	*ptr++ = '"';
	for (; ptr < to + len - 1; from++) {
		if (*from == '"')
			*ptr++ = '\\';
		else if (*from == '\0')
			break;
		*ptr++ = *from;
	}
	if (ptr < to + len - 1)
		*ptr++ = '"';
	*ptr = '\0';
	return to;
}

/*! \brief
 * fill in *tm for current time according to the proper timezone, if any.
 * Return tm so it can be used as a function argument.
 */
static const struct tm *vmu_tm(const struct ast_vm_user *vmu, struct tm *tm)
{
	const struct vm_zone *z = NULL;
	time_t t = time(NULL);

	/* Does this user have a timezone specified? */
	if (!ast_strlen_zero(vmu->zonetag)) {
		/* Find the zone in the list */
		AST_LIST_LOCK(&zones);
		AST_LIST_TRAVERSE(&zones, z, list) {
			if (!strcmp(z->name, vmu->zonetag))
				break;
		}
		AST_LIST_UNLOCK(&zones);
	}
	ast_localtime(&t, tm, z ? z->timezone : NULL);
	return tm;
}

/*! \brief same as mkstemp, but return a FILE * */
static FILE *vm_mkftemp(char *template)
{
	FILE *p = NULL;
	int pfd = mkstemp(template);
	if (pfd > -1) {
		p = fdopen(pfd, "w+");
		if (!p) {
			close(pfd);
			pfd = -1;
		}
	}
	return p;
}

static void make_email_file(FILE *p, char *srcemail, struct ast_vm_user *vmu, int msgnum, char *context, char *mailbox, char *cidnum, char *cidname, char *attach, char *format, int duration, int attach_user_voicemail, struct ast_channel *chan, const char *category, int imap)
{
	char date[256];
	char host[MAXHOSTNAMELEN] = "";
	char who[256];
	char bound[256];
	char fname[256];
	char dur[256];
	char tmpcmd[256];
	struct tm tm;
	char *passdata2;
	size_t len_passdata;
#ifdef IMAP_STORAGE
#define ENDL "\r\n"
#else
#define ENDL "\n"
#endif

	gethostname(host, sizeof(host)-1);
	if (strchr(srcemail, '@'))
		ast_copy_string(who, srcemail, sizeof(who));
	else 
		snprintf(who, sizeof(who), "%s@%s", srcemail, host);
	snprintf(dur, sizeof(dur), "%d:%02d", duration / 60, duration % 60);
	strftime(date, sizeof(date), "%a, %d %b %Y %H:%M:%S %z", vmu_tm(vmu, &tm));
	fprintf(p, "Date: %s" ENDL, date);

	/* Set date format for voicemail mail */
	strftime(date, sizeof(date), emaildateformat, &tm);

	if (!ast_strlen_zero(fromstring)) {
		struct ast_channel *ast;
		if ((ast = ast_channel_alloc(0, AST_STATE_DOWN, 0, 0, 0))) {
			char *passdata;
			int vmlen = strlen(fromstring)*3 + 200;
			if ((passdata = alloca(vmlen))) {
				memset(passdata, 0, vmlen);
				prep_email_sub_vars(ast, vmu, msgnum + 1, context, mailbox, cidnum, cidname, dur, date, passdata, vmlen, category);
				pbx_substitute_variables_helper(ast, fromstring, passdata, vmlen);
				len_passdata = strlen(passdata) * 2 + 3;
				passdata2 = alloca(len_passdata);
				fprintf(p, "From: %s <%s>" ENDL, quote(passdata, passdata2, len_passdata), who);
			} else
				ast_log(LOG_WARNING, "Cannot allocate workspace for variable substitution\n");
			ast_channel_free(ast);
		} else
			ast_log(LOG_WARNING, "Cannot allocate the channel for variables substitution\n");
	} else
		fprintf(p, "From: Asterisk PBX <%s>" ENDL, who);
	len_passdata = strlen(vmu->fullname) * 2 + 3;
	passdata2 = alloca(len_passdata);
	fprintf(p, "To: %s <%s>" ENDL, quote(vmu->fullname, passdata2, len_passdata), vmu->email);
	if (!ast_strlen_zero(emailsubject)) {
		struct ast_channel *ast;
		if ((ast = ast_channel_alloc(0, AST_STATE_DOWN, 0, 0, 0))) {
			char *passdata;
			int vmlen = strlen(emailsubject) * 3 + 200;
			if ((passdata = alloca(vmlen))) {
				memset(passdata, 0, vmlen);
				prep_email_sub_vars(ast, vmu, msgnum + 1, context, mailbox, cidnum, cidname, dur, date, passdata, vmlen, category);
				pbx_substitute_variables_helper(ast, emailsubject, passdata, vmlen);
				fprintf(p, "Subject: %s" ENDL, passdata);
			} else
				ast_log(LOG_WARNING, "Cannot allocate workspace for variable substitution\n");
			ast_channel_free(ast);
		} else
			ast_log(LOG_WARNING, "Cannot allocate the channel for variables substitution\n");
	} else	if (!ast_strlen_zero(emailtitle)) {
		fprintf(p, emailtitle, msgnum + 1, mailbox) ;
		fprintf(p, ENDL) ;
	} else if (ast_test_flag((&globalflags), VM_PBXSKIP))
		fprintf(p, "Subject: New message %d in mailbox %s" ENDL, msgnum + 1, mailbox);
	else
		fprintf(p, "Subject: [PBX]: New message %d in mailbox %s" ENDL, msgnum + 1, mailbox);
	fprintf(p, "Message-ID: <Asterisk-%d-%d-%s-%d@%s>" ENDL, msgnum + 1, (unsigned int)ast_random(), mailbox, getpid(), host);
	if(imap) {
		/* additional information needed for IMAP searching */
		fprintf(p, "X-Asterisk-VM-Message-Num: %d" ENDL, msgnum + 1);
		/* fprintf(p, "X-Asterisk-VM-Orig-Mailbox: %s" ENDL, ext); */
		fprintf(p, "X-Asterisk-VM-Server-Name: %s" ENDL, fromstring);
		fprintf(p, "X-Asterisk-VM-Context: %s" ENDL, context);
		fprintf(p, "X-Asterisk-VM-Extension: %s" ENDL, mailbox);
		fprintf(p, "X-Asterisk-VM-Priority: %d" ENDL, chan->priority);
		fprintf(p, "X-Asterisk-VM-Caller-channel: %s" ENDL, chan->name);
		fprintf(p, "X-Asterisk-VM-Caller-ID-Num: %s" ENDL, cidnum);
		fprintf(p, "X-Asterisk-VM-Caller-ID-Name: %s" ENDL, cidname);
		fprintf(p, "X-Asterisk-VM-Duration: %d" ENDL, duration);
		if (!ast_strlen_zero(category))
			fprintf(p, "X-Asterisk-VM-Category: %s" ENDL, category);
		fprintf(p, "X-Asterisk-VM-Orig-date: %s" ENDL, date);
		fprintf(p, "X-Asterisk-VM-Orig-time: %ld" ENDL, (long)time(NULL));
	}
	if (!ast_strlen_zero(cidnum))
		fprintf(p, "X-Asterisk-CallerID: %s" ENDL, cidnum);
	if (!ast_strlen_zero(cidname))
		fprintf(p, "X-Asterisk-CallerIDName: %s" ENDL, cidname);
	fprintf(p, "MIME-Version: 1.0" ENDL);
	if (attach_user_voicemail) {
		/* Something unique. */
		snprintf(bound, sizeof(bound), "voicemail_%d%s%d%d", msgnum + 1, mailbox, getpid(), (unsigned int)ast_random());

		fprintf(p, "Content-Type: multipart/mixed; boundary=\"%s\"" ENDL ENDL ENDL, bound);

		fprintf(p, "--%s" ENDL, bound);
	}
	fprintf(p, "Content-Type: text/plain; charset=%s" ENDL "Content-Transfer-Encoding: 8bit" ENDL ENDL, charset);
	if (emailbody) {
		struct ast_channel *ast;
		if ((ast = ast_channel_alloc(0, AST_STATE_DOWN, 0, 0, 0))) {
			char *passdata;
			int vmlen = strlen(emailbody)*3 + 200;
			if ((passdata = alloca(vmlen))) {
				memset(passdata, 0, vmlen);
				prep_email_sub_vars(ast, vmu, msgnum + 1, context, mailbox, cidnum, cidname, dur, date, passdata, vmlen, category);
				pbx_substitute_variables_helper(ast, emailbody, passdata, vmlen);
				fprintf(p, "%s" ENDL, passdata);
			} else
				ast_log(LOG_WARNING, "Cannot allocate workspace for variable substitution\n");
			ast_channel_free(ast);
		} else
			ast_log(LOG_WARNING, "Cannot allocate the channel for variables substitution\n");
	} else {
		fprintf(p, "Dear %s:" ENDL ENDL "\tJust wanted to let you know you were just left a %s long message (number %d)" ENDL

		"in mailbox %s from %s, on %s so you might" ENDL
		"want to check it when you get a chance.  Thanks!" ENDL ENDL "\t\t\t\t--Asterisk" ENDL ENDL, vmu->fullname, 
		dur, msgnum + 1, mailbox, (cidname ? cidname : (cidnum ? cidnum : "an unknown caller")), date);
	}
	if (attach_user_voicemail) {
		/* Eww. We want formats to tell us their own MIME type */
		char *ctype = (!strcasecmp(format, "ogg")) ? "application/" : "audio/x-";
		char tmpdir[256], newtmp[256];
		int tmpfd;
	
		create_dirpath(tmpdir, sizeof(tmpdir), vmu->context, vmu->mailbox, "tmp");
		snprintf(newtmp, sizeof(newtmp), "%s/XXXXXX", tmpdir);
		tmpfd = mkstemp(newtmp);
		if (option_debug > 2)
			ast_log(LOG_DEBUG, "newtmp: %s\n", newtmp);
		if (vmu->volgain < -.001 || vmu->volgain > .001) {
			snprintf(tmpcmd, sizeof(tmpcmd), "sox -v %.4f %s.%s %s.%s", vmu->volgain, attach, format, newtmp, format);
			ast_safe_system(tmpcmd);
			attach = newtmp;
			if (option_debug > 2)
				ast_log(LOG_DEBUG, "VOLGAIN: Stored at: %s.%s - Level: %.4f - Mailbox: %s\n", attach, format, vmu->volgain, mailbox);
		}
		fprintf(p, "--%s" ENDL, bound);
		fprintf(p, "Content-Type: %s%s; name=\"msg%04d.%s\"" ENDL, ctype, format, msgnum + 1, format);
		fprintf(p, "Content-Transfer-Encoding: base64" ENDL);
		fprintf(p, "Content-Description: Voicemail sound attachment." ENDL);
		fprintf(p, "Content-Disposition: attachment; filename=\"msg%04d.%s\"" ENDL ENDL, msgnum + 1, format);
		snprintf(fname, sizeof(fname), "%s.%s", attach, format);
		base_encode(fname, p);
		/* only attach if necessary */
		if (imap && !strcmp(format, "gsm")) {
			fprintf(p, "--%s" ENDL, bound);
			fprintf(p, "Content-Type: audio/x-gsm; name=\"msg%04d.%s\"" ENDL, msgnum + 1, format);
			fprintf(p, "Content-Transfer-Encoding: base64" ENDL);
			fprintf(p, "Content-Description: Voicemail sound attachment." ENDL);
			fprintf(p, "Content-Disposition: attachment; filename=\"msg%04d.gsm\"" ENDL ENDL, msgnum + 1);
			snprintf(fname, sizeof(fname), "%s.gsm", attach);
			base_encode(fname, p);
		}
		fprintf(p, ENDL ENDL "--%s--" ENDL "." ENDL, bound);
		if (tmpfd > -1)
			close(tmpfd);
		unlink(newtmp);
	}
#undef ENDL
}

static int sendmail(char *srcemail, struct ast_vm_user *vmu, int msgnum, char *context, char *mailbox, char *cidnum, char *cidname, char *attach, char *format, int duration, int attach_user_voicemail, struct ast_channel *chan, const char *category)
{
	FILE *p=NULL;
	char tmp[80] = "/tmp/astmail-XXXXXX";
	char tmp2[256];

	if (vmu && ast_strlen_zero(vmu->email)) {
		ast_log(LOG_WARNING, "E-mail address missing for mailbox [%s].  E-mail will not be sent.\n", vmu->mailbox);
		return(0);
	}
	if (!strcmp(format, "wav49"))
		format = "WAV";
	if (option_debug > 2)
		ast_log(LOG_DEBUG, "Attaching file '%s', format '%s', uservm is '%d', global is %d\n", attach, format, attach_user_voicemail, ast_test_flag((&globalflags), VM_ATTACH));
	/* Make a temporary file instead of piping directly to sendmail, in case the mail
	   command hangs */
	if ((p = vm_mkftemp(tmp)) == NULL) {
		ast_log(LOG_WARNING, "Unable to launch '%s' (can't create temporary file)\n", mailcmd);
		return -1;
	} else {
		make_email_file(p, srcemail, vmu, msgnum, context, mailbox, cidnum, cidname, attach, format, duration, attach_user_voicemail, chan, category, 0);
		fclose(p);
		snprintf(tmp2, sizeof(tmp2), "( %s < %s ; rm -f %s ) &", mailcmd, tmp, tmp);
		ast_safe_system(tmp2);
		if (option_debug)
			ast_log(LOG_DEBUG, "Sent mail to %s with command '%s'\n", vmu->email, mailcmd);
	}
	return 0;
}

static int sendpage(char *srcemail, char *pager, int msgnum, char *context, char *mailbox, char *cidnum, char *cidname, int duration, struct ast_vm_user *vmu, const char *category)
{
	char date[256];
	char host[MAXHOSTNAMELEN] = "";
	char who[256];
	char dur[PATH_MAX];
	char tmp[80] = "/tmp/astmail-XXXXXX";
	char tmp2[PATH_MAX];
	struct tm tm;
	FILE *p;

	if ((p = vm_mkftemp(tmp)) == NULL) {
		ast_log(LOG_WARNING, "Unable to launch '%s' (can't create temporary file)\n", mailcmd);
		return -1;
	}
	gethostname(host, sizeof(host)-1);
	if (strchr(srcemail, '@'))
		ast_copy_string(who, srcemail, sizeof(who));
	else 
		snprintf(who, sizeof(who), "%s@%s", srcemail, host);
	snprintf(dur, sizeof(dur), "%d:%02d", duration / 60, duration % 60);
	strftime(date, sizeof(date), "%a, %d %b %Y %H:%M:%S %z", vmu_tm(vmu, &tm));
	fprintf(p, "Date: %s\n", date);

	if (*pagerfromstring) {
		struct ast_channel *ast;
		if ((ast = ast_channel_alloc(0, AST_STATE_DOWN, 0, 0, 0))) {
			char *passdata;
			int vmlen = strlen(fromstring)*3 + 200;
			if ((passdata = alloca(vmlen))) {
				memset(passdata, 0, vmlen);
				prep_email_sub_vars(ast, vmu, msgnum + 1, context, mailbox, cidnum, cidname, dur, date, passdata, vmlen, category);
				pbx_substitute_variables_helper(ast, pagerfromstring, passdata, vmlen);
				fprintf(p, "From: %s <%s>\n", passdata, who);
			} else 
				ast_log(LOG_WARNING, "Cannot allocate workspace for variable substitution\n");
			ast_channel_free(ast);
		} else 
			ast_log(LOG_WARNING, "Cannot allocate the channel for variables substitution\n");
	} else
		fprintf(p, "From: Asterisk PBX <%s>\n", who);
	fprintf(p, "To: %s\n", pager);
	if (pagersubject) {
		struct ast_channel *ast;
		if ((ast = ast_channel_alloc(0, AST_STATE_DOWN, 0, 0, 0))) {
			char *passdata;
			int vmlen = strlen(pagersubject) * 3 + 200;
			if ((passdata = alloca(vmlen))) {
				memset(passdata, 0, vmlen);
				prep_email_sub_vars(ast, vmu, msgnum + 1, context, mailbox, cidnum, cidname, dur, date, passdata, vmlen, category);
				pbx_substitute_variables_helper(ast, pagersubject, passdata, vmlen);
				fprintf(p, "Subject: %s\n\n", passdata);
			} else
				ast_log(LOG_WARNING, "Cannot allocate workspace for variable substitution\n");
			ast_channel_free(ast);
		} else
			ast_log(LOG_WARNING, "Cannot allocate the channel for variables substitution\n");
	} else
		fprintf(p, "Subject: New VM\n\n");

	strftime(date, sizeof(date), "%A, %B %d, %Y at %r", &tm);
	if (pagerbody) {
		struct ast_channel *ast;
		if ((ast = ast_channel_alloc(0, AST_STATE_DOWN, 0, 0, 0))) {
			char *passdata;
			int vmlen = strlen(pagerbody)*3 + 200;
			if ((passdata = alloca(vmlen))) {
				memset(passdata, 0, vmlen);
				prep_email_sub_vars(ast, vmu, msgnum + 1, context, mailbox, cidnum, cidname, dur, date, passdata, vmlen, category);
				pbx_substitute_variables_helper(ast, pagerbody, passdata, vmlen);
				fprintf(p, "%s\n", passdata);
			} else
				ast_log(LOG_WARNING, "Cannot allocate workspace for variable substitution\n");
			ast_channel_free(ast);
		} else
			ast_log(LOG_WARNING, "Cannot allocate the channel for variables substitution\n");
	} else {
		fprintf(p, "New %s long msg in box %s\n"
				"from %s, on %s", dur, mailbox, (cidname ? cidname : (cidnum ? cidnum : "unknown")), date);
	}
	fclose(p);
	snprintf(tmp2, sizeof(tmp2), "( %s < %s ; rm -f %s ) &", mailcmd, tmp, tmp);
	ast_safe_system(tmp2);
	if (option_debug)
		ast_log(LOG_DEBUG, "Sent page to %s with command '%s'\n", pager, mailcmd);
	return 0;
}

static int get_date(char *s, int len)
{
	struct tm tm;
	time_t t;
	t = time(0);
	localtime_r(&t,&tm);
	return strftime(s, len, "%a %b %e %r %Z %Y", &tm);
}

static int invent_message(struct ast_channel *chan, char *context, char *ext, int busy, char *ecodes)
{
	int res;
	char fn[PATH_MAX];
	char dest[PATH_MAX];

	snprintf(fn, sizeof(fn), "%s%s/%s/greet", VM_SPOOL_DIR, context, ext);

	if ((res = create_dirpath(dest, sizeof(dest), context, ext, "greet"))) {
		ast_log(LOG_WARNING, "Failed to make directory(%s)\n", fn);
		return -1;
	}

	RETRIEVE(fn, -1);
	if (ast_fileexists(fn, NULL, NULL) > 0) {
		res = ast_stream_and_wait(chan, fn, ecodes);
		if (res) {
			DISPOSE(fn, -1);
			return res;
		}
	} else {
		/* Dispose just in case */
		DISPOSE(fn, -1);
		res = ast_stream_and_wait(chan, "vm-theperson", ecodes);
		if (res)
			return res;
		res = ast_say_digit_str(chan, ext, ecodes, chan->language);
		if (res)
			return res;
	}
	res = ast_stream_and_wait(chan, busy ? "vm-isonphone" : "vm-isunavail", ecodes);
	return res;
}

static void free_user(struct ast_vm_user *vmu)
{
	if (ast_test_flag(vmu, VM_ALLOCED))
		free(vmu);
}

static void free_zone(struct vm_zone *z)
{
	free(z);
}

static const char *mbox(int id)
{
	static const char *msgs[] = {
		"INBOX",
		"Old",
		"Work",
		"Family",
		"Friends",
		"Cust1",
		"Cust2",
		"Cust3",
		"Cust4",
		"Cust5",
	};
	return (id >= 0 && id < (sizeof(msgs)/sizeof(msgs[0]))) ? msgs[id] : "Unknown";
}

#ifdef ODBC_STORAGE
static int inboxcount(const char *mailbox, int *newmsgs, int *oldmsgs)
{
	int x = -1;
	int res;
	SQLHSTMT stmt;
	char sql[PATH_MAX];
	char rowdata[20];
	char tmp[PATH_MAX] = "";
	struct odbc_obj *obj;
	char *context;

	if (newmsgs)
		*newmsgs = 0;
	if (oldmsgs)
		*oldmsgs = 0;

	/* If no mailbox, return immediately */
	if (ast_strlen_zero(mailbox))
		return 0;

	ast_copy_string(tmp, mailbox, sizeof(tmp));
	
	context = strchr(tmp, '@');
	if (context) {
		*context = '\0';
		context++;
	} else
		context = "default";
	
	obj = ast_odbc_request_obj(odbc_database, 0);
	if (obj) {
		res = SQLAllocHandle(SQL_HANDLE_STMT, obj->con, &stmt);
		if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
			ast_log(LOG_WARNING, "SQL Alloc Handle failed!\n");
			ast_odbc_release_obj(obj);
			goto yuck;
		}
		snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM %s WHERE dir = '%s%s/%s/%s'", odbc_table, VM_SPOOL_DIR, context, tmp, "INBOX");
		res = SQLPrepare(stmt, (unsigned char *)sql, SQL_NTS);
		if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
			ast_log(LOG_WARNING, "SQL Prepare failed![%s]\n", sql);
			SQLFreeHandle (SQL_HANDLE_STMT, stmt);
			ast_odbc_release_obj(obj);
			goto yuck;
		}
		res = ast_odbc_smart_execute(obj, stmt);
		if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
			ast_log(LOG_WARNING, "SQL Execute error!\n[%s]\n\n", sql);
			SQLFreeHandle (SQL_HANDLE_STMT, stmt);
			ast_odbc_release_obj(obj);
			goto yuck;
		}
		res = SQLFetch(stmt);
		if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
			ast_log(LOG_WARNING, "SQL Fetch error!\n[%s]\n\n", sql);
			SQLFreeHandle (SQL_HANDLE_STMT, stmt);
			ast_odbc_release_obj(obj);
			goto yuck;
		}
		res = SQLGetData(stmt, 1, SQL_CHAR, rowdata, sizeof(rowdata), NULL);
		if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
			ast_log(LOG_WARNING, "SQL Get Data error!\n[%s]\n\n", sql);
			SQLFreeHandle (SQL_HANDLE_STMT, stmt);
			ast_odbc_release_obj(obj);
			goto yuck;
		}
		*newmsgs = atoi(rowdata);
		SQLFreeHandle (SQL_HANDLE_STMT, stmt);

		res = SQLAllocHandle(SQL_HANDLE_STMT, obj->con, &stmt);
		if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
			ast_log(LOG_WARNING, "SQL Alloc Handle failed!\n");
			ast_odbc_release_obj(obj);
			goto yuck;
		}
		snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM %s WHERE dir = '%s%s/%s/%s'", odbc_table, VM_SPOOL_DIR, context, tmp, "Old");
		res = SQLPrepare(stmt, (unsigned char *)sql, SQL_NTS);
		if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
			ast_log(LOG_WARNING, "SQL Prepare failed![%s]\n", sql);
			SQLFreeHandle (SQL_HANDLE_STMT, stmt);
			ast_odbc_release_obj(obj);
			goto yuck;
		}
		res = ast_odbc_smart_execute(obj, stmt);
		if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
			ast_log(LOG_WARNING, "SQL Execute error!\n[%s]\n\n", sql);
			SQLFreeHandle (SQL_HANDLE_STMT, stmt);
			ast_odbc_release_obj(obj);
			goto yuck;
		}
		res = SQLFetch(stmt);
		if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
			ast_log(LOG_WARNING, "SQL Fetch error!\n[%s]\n\n", sql);
			SQLFreeHandle (SQL_HANDLE_STMT, stmt);
			ast_odbc_release_obj(obj);
			goto yuck;
		}
		res = SQLGetData(stmt, 1, SQL_CHAR, rowdata, sizeof(rowdata), NULL);
		if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
			ast_log(LOG_WARNING, "SQL Get Data error!\n[%s]\n\n", sql);
			SQLFreeHandle (SQL_HANDLE_STMT, stmt);
			ast_odbc_release_obj(obj);
			goto yuck;
		}
		SQLFreeHandle (SQL_HANDLE_STMT, stmt);
		ast_odbc_release_obj(obj);
		*oldmsgs = atoi(rowdata);
		x = 0;
	} else
		ast_log(LOG_WARNING, "Failed to obtain database object for '%s'!\n", odbc_database);
		
yuck:	
	return x;
}

static int messagecount(const char *context, const char *mailbox, const char *folder)
{
	struct odbc_obj *obj = NULL;
	int nummsgs = 0;
	int res;
	SQLHSTMT stmt = NULL;
	char sql[PATH_MAX];
	char rowdata[20];
	if (!folder)
		folder = "INBOX";
	/* If no mailbox, return immediately */
	if (ast_strlen_zero(mailbox))
		return 0;

	obj = ast_odbc_request_obj(odbc_database, 0);
	if (obj) {
		res = SQLAllocHandle(SQL_HANDLE_STMT, obj->con, &stmt);
		if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
			ast_log(LOG_WARNING, "SQL Alloc Handle failed!\n");
			goto yuck;
		}
		snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM %s WHERE dir = '%s%s/%s/%s'", odbc_table, VM_SPOOL_DIR, context, mailbox, folder);
		res = SQLPrepare(stmt, (unsigned char *)sql, SQL_NTS);
		if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
			ast_log(LOG_WARNING, "SQL Prepare failed![%s]\n", sql);
			SQLFreeHandle(SQL_HANDLE_STMT, stmt);
			goto yuck;
		}
		res = ast_odbc_smart_execute(obj, stmt);
		if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
			ast_log(LOG_WARNING, "SQL Execute error!\n[%s]\n\n", sql);
			SQLFreeHandle (SQL_HANDLE_STMT, stmt);
			goto yuck;
		}
		res = SQLFetch(stmt);
		if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
			ast_log(LOG_WARNING, "SQL Fetch error!\n[%s]\n\n", sql);
			SQLFreeHandle (SQL_HANDLE_STMT, stmt);
			goto yuck;
		}
		res = SQLGetData(stmt, 1, SQL_CHAR, rowdata, sizeof(rowdata), NULL);
		if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
			ast_log(LOG_WARNING, "SQL Get Data error!\n[%s]\n\n", sql);
			SQLFreeHandle (SQL_HANDLE_STMT, stmt);
			goto yuck;
		}
		nummsgs = atoi(rowdata);
		SQLFreeHandle (SQL_HANDLE_STMT, stmt);
	} else
		ast_log(LOG_WARNING, "Failed to obtain database object for '%s'!\n", odbc_database);

yuck:
	if (obj)
		ast_odbc_release_obj(obj);
	return nummsgs;
}

static int has_voicemail(const char *mailbox, const char *folder)
{
	char *context, tmp[256];
	ast_copy_string(tmp, mailbox, sizeof(tmp));
	if ((context = strchr(tmp, '@')))
		*context++ = '\0';
	else
		context = "default";

	if (messagecount(context, tmp, folder))
		return 1;
	else
		return 0;
}

#elif defined(IMAP_STORAGE)

static int imap_store_file(char *dir, char *mailboxuser, char *mailboxcontext, int msgnum, struct ast_channel *chan, struct ast_vm_user *vmu, char *fmt, int duration, struct vm_state *vms)
{
	char *myserveremail = serveremail;
	char fn[PATH_MAX];
	char mailbox[256];
	char *stringp;
	FILE *p=NULL;
	char tmp[80] = "/tmp/astmail-XXXXXX";
	long len;
	void *buf;
	STRING str;
	
	/* Attach only the first format */
	fmt = ast_strdupa(fmt);
	stringp = fmt;
	strsep(&stringp, "|");

	if (!ast_strlen_zero(vmu->serveremail))
		myserveremail = vmu->serveremail;

	make_file(fn, sizeof(fn), dir, msgnum);

	if (ast_strlen_zero(vmu->email))
		ast_copy_string(vmu->email, vmu->imapuser, sizeof(vmu->email));

	if (!strcmp(fmt, "wav49"))
		fmt = "WAV";
	if(option_debug > 2)
		ast_log(LOG_DEBUG, "Storing file '%s', format '%s'\n", fn, fmt);

	/* Make a temporary file instead of piping directly to sendmail, in case the mail
	   command hangs */
	if (!(p = vm_mkftemp(tmp))) {
		ast_log(LOG_WARNING, "Unable to store '%s' (can't create temporary file)\n", fn);
		return -1;
	}
	
	make_email_file(p, myserveremail, vmu, msgnum, vmu->context, vmu->mailbox, S_OR(chan->cid.cid_num, NULL), S_OR(chan->cid.cid_name, NULL), fn, fmt, duration, 1, chan, NULL, 1);
	/* read mail file to memory */		
	len = ftell(p);
	rewind(p);
	if (!(buf = ast_malloc(len+1))) {
		ast_log(LOG_ERROR, "Can't allocate %ld bytes to read message\n", len+1);
		return -1;
	}
	fread(buf, len, 1, p);
	((char *)buf)[len] = '\0';
	INIT(&str, mail_string, buf, len);
	init_mailstream(vms, 0);
	imap_mailbox_name(mailbox, vms, 0, 1);
	if(!mail_append(vms->mailstream, mailbox, &str))
		ast_log(LOG_ERROR, "Error while sending the message to %s\n", mailbox);
	fclose(p);
	unlink(tmp);
	ast_free(buf);
	if(option_debug > 2)
		ast_log(LOG_DEBUG, "%s stored\n", fn);
	return 0;

}

static int inboxcount(const char *mailbox, int *newmsgs, int *oldmsgs)
{
	SEARCHPGM *pgm;
	SEARCHHEADER *hdr;
 
 	struct ast_vm_user *vmu;
 	struct vm_state *vms_p;
 	char tmp[256] = "";
 	char *mb, *cur;
 	char *mailboxnc; 
 	char *context;
 	int ret = 0;

 	if (newmsgs)
 		*newmsgs = 0;

 	if (oldmsgs)
 		*oldmsgs = 0;
 
	if(option_debug > 2)
	 	ast_log (LOG_DEBUG,"Mailbox is set to %s\n",mailbox);

 	/* If no mailbox, return immediately */
 	if (ast_strlen_zero(mailbox))
 		return 0;

 	if (strchr(mailbox, ',')) {
 		int tmpnew, tmpold;
		ast_copy_string(tmp, mailbox, sizeof(tmp));
 		mb = tmp;
 		ret = 0;
 		while((cur = strsep(&mb, ", "))) {
 			if (!ast_strlen_zero(cur)) {
 				if (inboxcount(cur, newmsgs ? &tmpnew : NULL, oldmsgs ? &tmpold : NULL))
 					return -1;
 				else {
 					if (newmsgs)
 						*newmsgs += tmpnew; 
 					if (oldmsgs)
 						*oldmsgs += tmpold;
 				}
 			}
 		}
 		return 0;
 	}

	ast_copy_string(tmp, mailbox, sizeof(tmp));

	if ((context = strchr(tmp, '@'))) {
 		*context = '\0';
 		mailboxnc = tmp;
 		context++;
 	} else {
 		context = "default";
 		mailboxnc = (char *)mailbox;
 	}
 
 	/* We have to get the user before we can open the stream! */
 	/*ast_log (LOG_DEBUG,"Before find_user, context is %s and mailbox is %s\n",context,mailbox); */
	if (!(vmu = find_user(NULL, context, mailboxnc))) {
		ast_log(LOG_ERROR, "Couldn't find mailbox %s in context %s\n", mailboxnc, context);
		return -1;
	}
	
	/* No IMAP account available */
	if (vmu->imapuser[0] == '\0') {
		ast_log (LOG_WARNING,"IMAP user not set for mailbox %s\n",vmu->mailbox);
		return -1;
 	}
 
 	/* check if someone is accessing this box right now... */
	if ((vms_p = get_vm_state_by_imapuser(vmu->imapuser, 1)) || (vms_p = get_vm_state_by_mailbox(mailboxnc, 1))) {
		if(option_debug > 2)
			ast_log (LOG_DEBUG,"Returning before search - user is logged in\n");
 		*newmsgs = vms_p->newmessages;
 		*oldmsgs = vms_p->oldmessages;
 		return 0;
 	}
 
 	/* add one if not there... */
	if (!(vms_p = get_vm_state_by_imapuser(vmu->imapuser, 0)) && !(vms_p = get_vm_state_by_mailbox(mailboxnc, 0))) {
		if(option_debug > 2)
			ast_log (LOG_DEBUG,"Adding new vmstate for %s\n",vmu->imapuser);
		if (!(vms_p = ast_calloc(1, sizeof(*vms_p))))
			return -1;
 		ast_copy_string(vms_p->imapuser,vmu->imapuser, sizeof(vms_p->imapuser));
 		ast_copy_string(vms_p->username, mailboxnc, sizeof(vms_p->username)); /* save for access from interactive entry point */
 		vms_p->mailstream = NIL; /* save for access from interactive entry point */
		if(option_debug > 2)
			ast_log (LOG_DEBUG,"Copied %s to %s\n",vmu->imapuser,vms_p->imapuser);
		vms_p->updated = 1;
		/* set mailbox to INBOX! */
		ast_copy_string(vms_p->curbox, mbox(0), sizeof(vms_p->curbox));
 		init_vm_state(vms_p);
 		vmstate_insert(vms_p);
 	}

	/* If no mailstream exists yet and even after attempting to initialize it fails, bail out */
	ret = init_mailstream(vms_p, 0);
	if (!vms_p->mailstream) {
		ast_log (LOG_ERROR,"Houston we have a problem - IMAP mailstream is NULL\n");
		return -1;
	}

	if (!ret && vms_p->updated == 1) {
		if (newmsgs) {
			pgm = mail_newsearchpgm();
			hdr = mail_newsearchheader("X-Asterisk-VM-Extension", (char *)mailboxnc);
			pgm->header = hdr;
			pgm->unseen = 1;
			pgm->seen = 0;
			pgm->undeleted = 1;
			pgm->deleted = 0;
			vms_p->vmArrayIndex = 0;
			mail_search_full(vms_p->mailstream, NULL, pgm, NIL);
			*newmsgs = vms_p->vmArrayIndex;
			vms_p->newmessages = vms_p->vmArrayIndex;
			mail_free_searchpgm(&pgm);
		}
		if (oldmsgs) {
			pgm = mail_newsearchpgm ();
			hdr = mail_newsearchheader ("X-Asterisk-VM-Extension", (char *)mailboxnc);
			pgm->header = hdr;
			pgm->unseen = 0;
			pgm->seen = 1;
			pgm->undeleted = 1;
			pgm->deleted = 0;
			vms_p->vmArrayIndex = 0;
			mail_search_full(vms_p->mailstream, NULL, pgm, NIL);
			*oldmsgs = vms_p->vmArrayIndex;
			vms_p->oldmessages = vms_p->vmArrayIndex;
			mail_free_searchpgm(&pgm);
		}
	}

 	if (vms_p->updated == 1) {  /* changes, so we did the searches above */
 		vms_p->updated = 0;
 	} else if (vms_p->updated > 1) {  /* decrement delay count */
 		vms_p->updated--;
 	} else {  /* no changes, so don't search */
 		mail_ping(vms_p->mailstream);
 		/* Keep the old data */
 		*newmsgs = vms_p->newmessages;
 		*oldmsgs = vms_p->oldmessages;
 	}

 	return 0;
 }

static int has_voicemail(const char *mailbox, const char *folder)
{
	int newmsgs, oldmsgs;
	
	if(inboxcount(mailbox, &newmsgs, &oldmsgs))
		return folder? oldmsgs: newmsgs;
	else
		return 0;
}

static int messagecount(const char *context, const char *mailbox, const char *folder)
{
	int newmsgs, oldmsgs;
 	char tmp[256] = "";
	
 	if (ast_strlen_zero(mailbox))
 		return 0;
	sprintf(tmp,"%s@%s", mailbox, ast_strlen_zero(context)? "default": context);

	if(inboxcount(tmp, &newmsgs, &oldmsgs))
		return folder? oldmsgs: newmsgs;
	else
		return 0;
}

#endif
#ifndef IMAP_STORAGE
/* copy message only used by file storage */
static int copy_message(struct ast_channel *chan, struct ast_vm_user *vmu, int imbox, int msgnum, long duration, struct ast_vm_user *recip, char *fmt, char *dir)
{
	char fromdir[PATH_MAX], todir[PATH_MAX], frompath[PATH_MAX], topath[PATH_MAX];
	const char *frombox = mbox(imbox);
	int recipmsgnum;

	ast_log(LOG_NOTICE, "Copying message from %s@%s to %s@%s\n", vmu->mailbox, vmu->context, recip->mailbox, recip->context);

	create_dirpath(todir, sizeof(todir), recip->context, recip->mailbox, "INBOX");
	
	if (!dir)
		make_dir(fromdir, sizeof(fromdir), vmu->context, vmu->mailbox, frombox);
	else
		ast_copy_string(fromdir, dir, sizeof(fromdir));

	make_file(frompath, sizeof(frompath), fromdir, msgnum);
	make_dir(todir, sizeof(todir), recip->context, recip->mailbox, frombox);

	if (vm_lock_path(todir))
		return ERROR_LOCK_PATH;

	recipmsgnum = last_message_index(recip, todir) + 1;
	if (recipmsgnum < recip->maxmsg) {
		make_file(topath, sizeof(topath), todir, recipmsgnum);
		COPY(fromdir, msgnum, todir, recipmsgnum, recip->mailbox, recip->context, frompath, topath);
	} else {
		ast_log(LOG_ERROR, "Recipient mailbox %s@%s is full\n", recip->mailbox, recip->context);
	}
	ast_unlock_path(todir);
	notify_new_message(chan, recip, recipmsgnum, duration, fmt, S_OR(chan->cid.cid_num, NULL), S_OR(chan->cid.cid_name, NULL));
	
	return 0;
}
#endif
#if !(defined(IMAP_STORAGE) || defined(ODBC_STORAGE))
static int messagecount(const char *context, const char *mailbox, const char *folder)
{
	return __has_voicemail(context, mailbox, folder, 0);
}


static int __has_voicemail(const char *context, const char *mailbox, const char *folder, int shortcircuit)
{
	DIR *dir;
	struct dirent *de;
	char fn[256];
	int ret = 0;

	/* If no mailbox, return immediately */
	if (ast_strlen_zero(mailbox))
		return 0;

	if (ast_strlen_zero(folder))
		folder = "INBOX";
	if (ast_strlen_zero(context))
		context = "default";

	snprintf(fn, sizeof(fn), "%s%s/%s/%s", VM_SPOOL_DIR, context, mailbox, folder);

	if (!(dir = opendir(fn)))
		return 0;

	while ((de = readdir(dir))) {
		if (!strncasecmp(de->d_name, "msg", 3)) {
			if (shortcircuit) {
				ret = 1;
				break;
			} else if (!strncasecmp(de->d_name + 8, "txt", 3))
				ret++;
		}
	}

	closedir(dir);

	return ret;
}


static int has_voicemail(const char *mailbox, const char *folder)
{
	char tmp[256], *tmp2 = tmp, *mbox, *context;
	ast_copy_string(tmp, mailbox, sizeof(tmp));
	while ((mbox = strsep(&tmp2, ","))) {
		if ((context = strchr(mbox, '@')))
			*context++ = '\0';
		else
			context = "default";
		if (__has_voicemail(context, mbox, folder, 1))
			return 1;
	}
	return 0;
}


static int inboxcount(const char *mailbox, int *newmsgs, int *oldmsgs)
{
	char tmp[256];
	char *context;

	/* If no mailbox, return immediately */
	if (ast_strlen_zero(mailbox))
		return 0;

	if (newmsgs)
		*newmsgs = 0;
	if (oldmsgs)
		*oldmsgs = 0;

	if (strchr(mailbox, ',')) {
		int tmpnew, tmpold;
		char *mb, *cur;

		ast_copy_string(tmp, mailbox, sizeof(tmp));
		mb = tmp;
		while ((cur = strsep(&mb, ", "))) {
			if (!ast_strlen_zero(cur)) {
				if (inboxcount(cur, newmsgs ? &tmpnew : NULL, oldmsgs ? &tmpold : NULL))
					return -1;
				else {
					if (newmsgs)
						*newmsgs += tmpnew; 
					if (oldmsgs)
						*oldmsgs += tmpold;
				}
			}
		}
		return 0;
	}

	ast_copy_string(tmp, mailbox, sizeof(tmp));
	
	if ((context = strchr(tmp, '@')))
		*context++ = '\0';
	else
		context = "default";

	if (newmsgs)
		*newmsgs = __has_voicemail(context, tmp, "INBOX", 0);
	if (oldmsgs)
		*oldmsgs = __has_voicemail(context, tmp, "Old", 0);

	return 0;
}

#endif

static void run_externnotify(char *context, char *extension)
{
	char arguments[255];
	char ext_context[256] = "";
	int newvoicemails = 0, oldvoicemails = 0;
	struct ast_smdi_mwi_message *mwi_msg;

	if (!ast_strlen_zero(context))
		snprintf(ext_context, sizeof(ext_context), "%s@%s", extension, context);
	else
		ast_copy_string(ext_context, extension, sizeof(ext_context));

	if (smdi_iface) {
		if (ast_app_has_voicemail(ext_context, NULL)) 
			ast_smdi_mwi_set(smdi_iface, extension);
		else
			ast_smdi_mwi_unset(smdi_iface, extension);

		if ((mwi_msg = ast_smdi_mwi_message_wait(smdi_iface, SMDI_MWI_WAIT_TIMEOUT))) {
			ast_log(LOG_ERROR, "Error executing SMDI MWI change for %s on %s\n", extension, smdi_iface->name);
			if (!strncmp(mwi_msg->cause, "INV", 3))
				ast_log(LOG_ERROR, "Invalid MWI extension: %s\n", mwi_msg->fwd_st);
			else if (!strncmp(mwi_msg->cause, "BLK", 3))
				ast_log(LOG_WARNING, "MWI light was already on or off for %s\n", mwi_msg->fwd_st);
			ast_log(LOG_WARNING, "The switch reported '%s'\n", mwi_msg->cause);
			ASTOBJ_UNREF(mwi_msg, ast_smdi_mwi_message_destroy);
		} else {
			if (option_debug)
				ast_log(LOG_DEBUG, "Successfully executed SMDI MWI change for %s on %s\n", extension, smdi_iface->name);
		}
	}

	if (!ast_strlen_zero(externnotify)) {
		if (inboxcount(ext_context, &newvoicemails, &oldvoicemails)) {
			ast_log(LOG_ERROR, "Problem in calculating number of voicemail messages available for extension %s\n", extension);
		} else {
			snprintf(arguments, sizeof(arguments), "%s %s %s %d&", externnotify, context, extension, newvoicemails);
			if (option_debug)
				ast_log(LOG_DEBUG, "Executing %s\n", arguments);
			ast_safe_system(arguments);
		}
	}
}

struct leave_vm_options {
	unsigned int flags;
	signed char record_gain;
};

static int leave_voicemail(struct ast_channel *chan, char *ext, struct leave_vm_options *options)
{
#ifdef IMAP_STORAGE
	int newmsgs, oldmsgs;
	struct vm_state *vms = NULL;
#endif
	char txtfile[PATH_MAX], tmptxtfile[PATH_MAX];
	char callerid[256];
	FILE *txt;
	char date[256];
	int txtdes;
	int res = 0;
	int msgnum;
	int duration = 0;
	int ausemacro = 0;
	int ousemacro = 0;
	int ouseexten = 0;
	char dir[PATH_MAX], tmpdir[PATH_MAX];
	char dest[PATH_MAX];
	char fn[PATH_MAX];
	char prefile[PATH_MAX] = "";
	char tempfile[PATH_MAX] = "";
	char ext_context[256] = "";
	char fmt[80];
	char *context;
	char ecodes[16] = "#";
	char tmp[256] = "", *tmpptr;
	struct ast_vm_user *vmu;
	struct ast_vm_user svm;
	const char *category = NULL;

	ast_copy_string(tmp, ext, sizeof(tmp));
	ext = tmp;
	if ((context = strchr(tmp, '@'))) {
		*context++ = '\0';
		tmpptr = strchr(context, '&');
	} else {
		tmpptr = strchr(ext, '&');
	}

	if (tmpptr)
		*tmpptr++ = '\0';

	category = pbx_builtin_getvar_helper(chan, "VM_CATEGORY");

	if(option_debug > 2)
		ast_log(LOG_DEBUG, "Before find_user\n");
	if (!(vmu = find_user(&svm, context, ext))) {
		ast_log(LOG_WARNING, "No entry in voicemail config file for '%s'\n", ext);
		if (ast_test_flag(options, OPT_PRIORITY_JUMP) || ast_opt_priority_jumping)
			ast_goto_if_exists(chan, chan->context, chan->exten, chan->priority + 101);
		pbx_builtin_setvar_helper(chan, "VMSTATUS", "FAILED");
		return res;
	}
	/* Setup pre-file if appropriate */
	if (strcmp(vmu->context, "default"))
		snprintf(ext_context, sizeof(ext_context), "%s@%s", ext, vmu->context);
	else
		ast_copy_string(ext_context, vmu->context, sizeof(ext_context));
	if (ast_test_flag(options, OPT_BUSY_GREETING)) {
		res = create_dirpath(dest, sizeof(dest), vmu->context, ext, "busy");
		snprintf(prefile, sizeof(prefile), "%s%s/%s/busy", VM_SPOOL_DIR, vmu->context, ext);
	} else if (ast_test_flag(options, OPT_UNAVAIL_GREETING)) {
		res = create_dirpath(dest, sizeof(dest), vmu->context, ext, "unavail");
		snprintf(prefile, sizeof(prefile), "%s%s/%s/unavail", VM_SPOOL_DIR, vmu->context, ext);
	}
	snprintf(tempfile, sizeof(tempfile), "%s%s/%s/temp", VM_SPOOL_DIR, vmu->context, ext);
	if ((res = create_dirpath(dest, sizeof(dest), vmu->context, ext, "temp"))) {
		ast_log(LOG_WARNING, "Failed to make directory (%s)\n", tempfile);
		return -1;
	}
	RETRIEVE(tempfile, -1);
	if (ast_fileexists(tempfile, NULL, NULL) > 0)
		ast_copy_string(prefile, tempfile, sizeof(prefile));
	DISPOSE(tempfile, -1);
	/* It's easier just to try to make it than to check for its existence */
	create_dirpath(dir, sizeof(dir), vmu->context, ext, "INBOX");
	create_dirpath(tmpdir, sizeof(tmpdir), vmu->context, ext, "tmp");

	/* Check current or macro-calling context for special extensions */
	if (ast_test_flag(vmu, VM_OPERATOR)) {
		if (!ast_strlen_zero(vmu->exit)) {
			if (ast_exists_extension(chan, vmu->exit, "o", 1, chan->cid.cid_num)) {
				strncat(ecodes, "0", sizeof(ecodes) - strlen(ecodes) - 1);
				ouseexten = 1;
			}
		} else if (ast_exists_extension(chan, chan->context, "o", 1, chan->cid.cid_num)) {
			strncat(ecodes, "0", sizeof(ecodes) - strlen(ecodes) - 1);
			ouseexten = 1;
		}
		else if (!ast_strlen_zero(chan->macrocontext) && ast_exists_extension(chan, chan->macrocontext, "o", 1, chan->cid.cid_num)) {
		strncat(ecodes, "0", sizeof(ecodes) - strlen(ecodes) - 1);
		ousemacro = 1;
		}
	}

	if (!ast_strlen_zero(vmu->exit)) {
		if (ast_exists_extension(chan, vmu->exit, "a", 1, chan->cid.cid_num))
			strncat(ecodes, "*", sizeof(ecodes) - strlen(ecodes) - 1);
	} else if (ast_exists_extension(chan, chan->context, "a", 1, chan->cid.cid_num))
		strncat(ecodes, "*", sizeof(ecodes) - strlen(ecodes) - 1);
	else if (!ast_strlen_zero(chan->macrocontext) && ast_exists_extension(chan, chan->macrocontext, "a", 1, chan->cid.cid_num)) {
		strncat(ecodes, "*", sizeof(ecodes) - strlen(ecodes) - 1);
		ausemacro = 1;
	}

	/* Play the beginning intro if desired */
	if (!ast_strlen_zero(prefile)) {
		RETRIEVE(prefile, -1);
		if (ast_fileexists(prefile, NULL, NULL) > 0) {
			if (ast_streamfile(chan, prefile, chan->language) > -1) 
				res = ast_waitstream(chan, ecodes);
		} else {
			if (option_debug)
				ast_log(LOG_DEBUG, "%s doesn't exist, doing what we can\n", prefile);
			res = invent_message(chan, vmu->context, ext, ast_test_flag(options, OPT_BUSY_GREETING), ecodes);
		}
		DISPOSE(prefile, -1);
		if (res < 0) {
			if (option_debug)
				ast_log(LOG_DEBUG, "Hang up during prefile playback\n");
			free_user(vmu);
			pbx_builtin_setvar_helper(chan, "VMSTATUS", "FAILED");
			return -1;
		}
	}
	if (res == '#') {
		/* On a '#' we skip the instructions */
		ast_set_flag(options, OPT_SILENT);
		res = 0;
	}
	if (!res && !ast_test_flag(options, OPT_SILENT)) {
		res = ast_stream_and_wait(chan, INTRO, ecodes);
		if (res == '#') {
			ast_set_flag(options, OPT_SILENT);
			res = 0;
		}
	}
	if (res > 0)
		ast_stopstream(chan);
	/* Check for a '*' here in case the caller wants to escape from voicemail to something
	 other than the operator -- an automated attendant or mailbox login for example */
	if (res == '*') {
		chan->exten[0] = 'a';
		chan->exten[1] = '\0';
		if (!ast_strlen_zero(vmu->exit)) {
			ast_copy_string(chan->context, vmu->exit, sizeof(chan->context));
		} else if (ausemacro && !ast_strlen_zero(chan->macrocontext)) {
			ast_copy_string(chan->context, chan->macrocontext, sizeof(chan->context));
		}
		chan->priority = 0;
		free_user(vmu);
		pbx_builtin_setvar_helper(chan, "VMSTATUS", "USEREXIT");
		return 0;
	}

	/* Check for a '0' here */
	if (res == '0') {
	transfer:
		if (ouseexten || ousemacro) {
			chan->exten[0] = 'o';
			chan->exten[1] = '\0';
			if (!ast_strlen_zero(vmu->exit)) {
				ast_copy_string(chan->context, vmu->exit, sizeof(chan->context));
			} else if (ousemacro && !ast_strlen_zero(chan->macrocontext)) {
				ast_copy_string(chan->context, chan->macrocontext, sizeof(chan->context));
			}
			ast_play_and_wait(chan, "transfer");
			chan->priority = 0;
			free_user(vmu);
			pbx_builtin_setvar_helper(chan, "VMSTATUS", "USEREXIT");
		}
		return 0;
	}
	if (res < 0) {
		free_user(vmu);
		pbx_builtin_setvar_helper(chan, "VMSTATUS", "FAILED");
		return -1;
	}
	/* The meat of recording the message...  All the announcements and beeps have been played*/
	ast_copy_string(fmt, vmfmts, sizeof(fmt));
	if (!ast_strlen_zero(fmt)) {
		msgnum = 0;

#ifdef IMAP_STORAGE
		/* Is ext a mailbox? */
		/* must open stream for this user to get info! */
		vms = get_vm_state_by_mailbox(ext,0);
		if (vms) {
			if(option_debug > 2)
				ast_log(LOG_DEBUG, "Using vm_state, interactive set to %d.\n",vms->interactive);
			newmsgs = vms->newmessages++;
			oldmsgs = vms->oldmessages;
		} else {
			res = inboxcount(ext, &newmsgs, &oldmsgs);
			if(res < 0) {
				ast_log(LOG_NOTICE,"Can not leave voicemail, unable to count messages\n");
				return -1;
			}
			vms = get_vm_state_by_mailbox(ext,0);
		}
		/* here is a big difference! We add one to it later */
		msgnum = newmsgs + oldmsgs;
		if(option_debug > 2)
			ast_log(LOG_DEBUG, "Messagecount set to %d\n",msgnum);
		snprintf(fn, sizeof(fn), "%s/imap/msg%s%04d", VM_SPOOL_DIR, vmu->mailbox, msgnum);
		/* set variable for compatibility */
		pbx_builtin_setvar_helper(chan, "VM_MESSAGEFILE", "IMAP_STORAGE");

		/* Check if mailbox is full */
		if (vms->quota_limit && vms->quota_usage >= vms->quota_limit) {
			if (option_debug)
				ast_log(LOG_DEBUG, "*** QUOTA EXCEEDED!! %u >= %u\n", vms->quota_usage, vms->quota_limit);
			ast_play_and_wait(chan, "vm-mailboxfull");
			return -1;
		}
		/* here is a big difference! We add one to it later */
		msgnum = newmsgs + oldmsgs;
		if(option_debug > 2)
			ast_log(LOG_DEBUG, "Messagecount set to %d\n",msgnum);

#else
		if (count_messages(vmu, dir) >= vmu->maxmsg) {
			res = ast_streamfile(chan, "vm-mailboxfull", chan->language);
			if (!res)
				res = ast_waitstream(chan, "");
			ast_log(LOG_WARNING, "No more messages possible\n");
			pbx_builtin_setvar_helper(chan, "VMSTATUS", "FAILED");
			goto leave_vm_out;
		}

#endif
		snprintf(tmptxtfile, sizeof(tmptxtfile), "%s/XXXXXX", tmpdir);
		txtdes = mkstemp(tmptxtfile);
		if (txtdes < 0) {
			res = ast_streamfile(chan, "vm-mailboxfull", chan->language);
			if (!res)
				res = ast_waitstream(chan, "");
			ast_log(LOG_ERROR, "Unable to create message file: %s\n", strerror(errno));
			pbx_builtin_setvar_helper(chan, "VMSTATUS", "FAILED");
			goto leave_vm_out;
		}

		/* Now play the beep once we have the message number for our next message. */
		if (res >= 0) {
			/* Unless we're *really* silent, try to send the beep */
			res = ast_stream_and_wait(chan, "beep", "");
		}
				
		/* Store information */
		txt = fdopen(txtdes, "w+");
		if (txt) {
			get_date(date, sizeof(date));
			fprintf(txt, 
				";\n"
				"; Message Information file\n"
				";\n"
				"[message]\n"
				"origmailbox=%s\n"
				"context=%s\n"
				"macrocontext=%s\n"
				"exten=%s\n"
				"priority=%d\n"
				"callerchan=%s\n"
				"callerid=%s\n"
				"origdate=%s\n"
				"origtime=%ld\n"
				"category=%s\n",
				ext,
				chan->context,
				chan->macrocontext, 
				chan->exten,
				chan->priority,
				chan->name,
				ast_callerid_merge(callerid, sizeof(callerid), S_OR(chan->cid.cid_name, NULL), S_OR(chan->cid.cid_num, NULL), "Unknown"),
				date, (long)time(NULL),
				category ? category : ""); 
		} else
			ast_log(LOG_WARNING, "Error opening text file for output\n");
#ifdef IMAP_STORAGE
		res = play_record_review(chan, NULL, tmptxtfile, vmu->maxsecs, fmt, 1, vmu, &duration, NULL, options->record_gain, vms);
#else
		res = play_record_review(chan, NULL, tmptxtfile, vmu->maxsecs, fmt, 1, vmu, &duration, NULL, options->record_gain, NULL);
#endif

		if (txt) {
			if (duration < vmminsecs) {
				if (option_verbose > 2) 
					ast_verbose( VERBOSE_PREFIX_3 "Recording was %d seconds long but needs to be at least %d - abandoning\n", duration, vmminsecs);
				ast_filedelete(tmptxtfile, NULL);
				unlink(tmptxtfile);
			} else {
				fprintf(txt, "duration=%d\n", duration);
				fclose(txt);
				if (vm_lock_path(dir)) {
					ast_log(LOG_ERROR, "Couldn't lock directory %s.  Voicemail will be lost.\n", dir);
					/* Delete files */
					ast_filedelete(tmptxtfile, NULL);
					unlink(tmptxtfile);
				} else if (ast_fileexists(tmptxtfile, NULL, NULL) <= 0) {
					if (option_debug) 
						ast_log(LOG_DEBUG, "The recorded media file is gone, so we should remove the .txt file too!\n");
					unlink(tmptxtfile);
					ast_unlock_path(dir);
				} else {
					msgnum = last_message_index(vmu, dir) + 1;
					make_file(fn, sizeof(fn), dir, msgnum);

					/* assign a variable with the name of the voicemail file */ 
#ifndef IMAP_STORAGE
					pbx_builtin_setvar_helper(chan, "VM_MESSAGEFILE", fn);
#else
					pbx_builtin_setvar_helper(chan, "VM_MESSAGEFILE", "IMAP_STORAGE");
#endif

					snprintf(txtfile, sizeof(txtfile), "%s.txt", fn);
					ast_filerename(tmptxtfile, fn, NULL);
					rename(tmptxtfile, txtfile);

					ast_unlock_path(dir);
#ifndef IMAP_STORAGE
					/* Are there to be more recipients of this message? */
					while (tmpptr) {
						struct ast_vm_user recipu, *recip;
						char *exten, *context;
					
						exten = strsep(&tmpptr, "&");
						context = strchr(exten, '@');
						if (context) {
							*context = '\0';
							context++;
						}
						if ((recip = find_user(&recipu, context, exten))) {
							copy_message(chan, vmu, 0, msgnum, duration, recip, fmt, dir);
							free_user(recip);
						}
					}
#endif
					if (ast_fileexists(fn, NULL, NULL)) {
						STORE(dir, vmu->mailbox, vmu->context, msgnum, chan, vmu, fmt, duration, vms);
						notify_new_message(chan, vmu, msgnum, duration, fmt, S_OR(chan->cid.cid_num, NULL), S_OR(chan->cid.cid_name, NULL));
						DISPOSE(dir, msgnum);
					}
				}
			}
		}
		if (res == '0') {
			goto transfer;
		} else if (res > 0)
			res = 0;

		if (duration < vmminsecs)
			/* XXX We should really give a prompt too short/option start again, with leave_vm_out called only after a timeout XXX */
			pbx_builtin_setvar_helper(chan, "VMSTATUS", "FAILED");
		else
			pbx_builtin_setvar_helper(chan, "VMSTATUS", "SUCCESS");
	} else
		ast_log(LOG_WARNING, "No format for saving voicemail?\n");
leave_vm_out:
	free_user(vmu);
	
	return res;
}

#ifndef IMAP_STORAGE
static int resequence_mailbox(struct ast_vm_user *vmu, char *dir)
{
	/* we know max messages, so stop process when number is hit */

	int x,dest;
	char sfn[PATH_MAX];
	char dfn[PATH_MAX];

	if (vm_lock_path(dir))
		return ERROR_LOCK_PATH;

	for (x = 0, dest = 0; x < vmu->maxmsg; x++) {
		make_file(sfn, sizeof(sfn), dir, x);
		if (EXISTS(dir, x, sfn, NULL)) {
			
			if (x != dest) {
				make_file(dfn, sizeof(dfn), dir, dest);
				RENAME(dir, x, vmu->mailbox, vmu->context, dir, dest, sfn, dfn);
			}
			
			dest++;
		}
	}
	ast_unlock_path(dir);

	return 0;
}
#endif

static int say_and_wait(struct ast_channel *chan, int num, const char *language)
{
	int d;
	d = ast_say_number(chan, num, AST_DIGIT_ANY, language, (char *) NULL);
	return d;
}

static int save_to_folder(struct ast_vm_user *vmu, struct vm_state *vms, int msg, int box)
{
#ifdef IMAP_STORAGE
	/* we must use mbox(x) folder names, and copy the message there */
	/* simple. huh? */
	char dbox[256];
	long res;
	char sequence[10];

	/* if save to Old folder, just leave in INBOX */
	if (box == 1) return 10;
	/* get the real IMAP message number for this message */
	sprintf(sequence,"%ld",vms->msgArray[msg]);
	imap_mailbox_name(dbox, vms, box, 1);
	if (option_debug > 2)
		ast_log(LOG_DEBUG, "Copying sequence %s to mailbox %s\n",sequence,dbox);
	res = mail_copy(vms->mailstream, sequence, dbox);
	if (res == 1) return 0;
	return 1;
#else
	char *dir = vms->curdir;
	char *username = vms->username;
	char *context = vmu->context;
	char sfn[PATH_MAX];
	char dfn[PATH_MAX];
	char ddir[PATH_MAX];
	const char *dbox = mbox(box);
	int x;
	make_file(sfn, sizeof(sfn), dir, msg);
	create_dirpath(ddir, sizeof(ddir), context, username, dbox);

	if (vm_lock_path(ddir))
		return ERROR_LOCK_PATH;

	x = last_message_index(vmu, ddir) + 1;
	make_file(dfn, sizeof(dfn), ddir, x);

	if (x >= vmu->maxmsg) {
		ast_unlock_path(ddir);
		return -1;
	}
	if (strcmp(sfn, dfn)) {
		COPY(dir, msg, ddir, x, username, context, sfn, dfn);
	}
	ast_unlock_path(ddir);
#endif
	return 0;
}

static int adsi_logo(unsigned char *buf)
{
	int bytes = 0;
	bytes += ast_adsi_display(buf + bytes, ADSI_COMM_PAGE, 1, ADSI_JUST_CENT, 0, "Comedian Mail", "");
	bytes += ast_adsi_display(buf + bytes, ADSI_COMM_PAGE, 2, ADSI_JUST_CENT, 0, "(C)2002-2006 Digium, Inc.", "");
	return bytes;
}

static int adsi_load_vmail(struct ast_channel *chan, int *useadsi)
{
	unsigned char buf[256];
	int bytes=0;
	int x;
	char num[5];

	*useadsi = 0;
	bytes += ast_adsi_data_mode(buf + bytes);
	ast_adsi_transmit_message(chan, buf, bytes, ADSI_MSG_DISPLAY);

	bytes = 0;
	bytes += adsi_logo(buf);
	bytes += ast_adsi_display(buf + bytes, ADSI_COMM_PAGE, 3, ADSI_JUST_CENT, 0, "Downloading Scripts", "");
#ifdef DISPLAY
	bytes += ast_adsi_display(buf + bytes, ADSI_COMM_PAGE, 4, ADSI_JUST_LEFT, 0, "   .", "");
#endif
	bytes += ast_adsi_set_line(buf + bytes, ADSI_COMM_PAGE, 1);
	bytes += ast_adsi_data_mode(buf + bytes);
	ast_adsi_transmit_message(chan, buf, bytes, ADSI_MSG_DISPLAY);

	if (ast_adsi_begin_download(chan, addesc, adsifdn, adsisec, adsiver)) {
		bytes = 0;
		bytes += ast_adsi_display(buf + bytes, ADSI_COMM_PAGE, 3, ADSI_JUST_CENT, 0, "Load Cancelled.", "");
		bytes += ast_adsi_display(buf + bytes, ADSI_COMM_PAGE, 4, ADSI_JUST_CENT, 0, "ADSI Unavailable", "");
		bytes += ast_adsi_set_line(buf + bytes, ADSI_COMM_PAGE, 1);
		bytes += ast_adsi_voice_mode(buf + bytes, 0);
		ast_adsi_transmit_message(chan, buf, bytes, ADSI_MSG_DISPLAY);
		return 0;
	}

#ifdef DISPLAY
	/* Add a dot */
	bytes = 0;
	bytes += ast_adsi_logo(buf);
	bytes += ast_adsi_display(buf + bytes, ADSI_COMM_PAGE, 3, ADSI_JUST_CENT, 0, "Downloading Scripts", "");
	bytes += ast_adsi_display(buf + bytes, ADSI_COMM_PAGE, 4, ADSI_JUST_LEFT, 0, "   ..", "");
	bytes += ast_adsi_set_line(buf + bytes, ADSI_COMM_PAGE, 1);
	ast_adsi_transmit_message(chan, buf, bytes, ADSI_MSG_DISPLAY);
#endif
	bytes = 0;
	bytes += ast_adsi_load_soft_key(buf + bytes, ADSI_KEY_APPS + 0, "Listen", "Listen", "1", 1);
	bytes += ast_adsi_load_soft_key(buf + bytes, ADSI_KEY_APPS + 1, "Folder", "Folder", "2", 1);
	bytes += ast_adsi_load_soft_key(buf + bytes, ADSI_KEY_APPS + 2, "Advanced", "Advnced", "3", 1);
	bytes += ast_adsi_load_soft_key(buf + bytes, ADSI_KEY_APPS + 3, "Options", "Options", "0", 1);
	bytes += ast_adsi_load_soft_key(buf + bytes, ADSI_KEY_APPS + 4, "Help", "Help", "*", 1);
	bytes += ast_adsi_load_soft_key(buf + bytes, ADSI_KEY_APPS + 5, "Exit", "Exit", "#", 1);
	ast_adsi_transmit_message(chan, buf, bytes, ADSI_MSG_DOWNLOAD);

#ifdef DISPLAY
	/* Add another dot */
	bytes = 0;
	bytes += ast_adsi_display(buf + bytes, ADSI_COMM_PAGE, 4, ADSI_JUST_LEFT, 0, "   ...", "");
	bytes += ast_adsi_voice_mode(buf + bytes, 0);

	bytes += ast_adsi_set_line(buf + bytes, ADSI_COMM_PAGE, 1);
	ast_adsi_transmit_message(chan, buf, bytes, ADSI_MSG_DISPLAY);
#endif

	bytes = 0;
	/* These buttons we load but don't use yet */
	bytes += ast_adsi_load_soft_key(buf + bytes, ADSI_KEY_APPS + 6, "Previous", "Prev", "4", 1);
	bytes += ast_adsi_load_soft_key(buf + bytes, ADSI_KEY_APPS + 8, "Repeat", "Repeat", "5", 1);
	bytes += ast_adsi_load_soft_key(buf + bytes, ADSI_KEY_APPS + 7, "Delete", "Delete", "7", 1);
	bytes += ast_adsi_load_soft_key(buf + bytes, ADSI_KEY_APPS + 9, "Next", "Next", "6", 1);
	bytes += ast_adsi_load_soft_key(buf + bytes, ADSI_KEY_APPS + 10, "Save", "Save", "9", 1);
	bytes += ast_adsi_load_soft_key(buf + bytes, ADSI_KEY_APPS + 11, "Undelete", "Restore", "7", 1);
	ast_adsi_transmit_message(chan, buf, bytes, ADSI_MSG_DOWNLOAD);

#ifdef DISPLAY
	/* Add another dot */
	bytes = 0;
	bytes += ast_adsi_display(buf + bytes, ADSI_COMM_PAGE, 4, ADSI_JUST_LEFT, 0, "   ....", "");
	bytes += ast_adsi_set_line(buf + bytes, ADSI_COMM_PAGE, 1);
	ast_adsi_transmit_message(chan, buf, bytes, ADSI_MSG_DISPLAY);
#endif

	bytes = 0;
	for (x=0;x<5;x++) {
		snprintf(num, sizeof(num), "%d", x);
		bytes += ast_adsi_load_soft_key(buf + bytes, ADSI_KEY_APPS + 12 + x, mbox(x), mbox(x), num, 1);
	}
	bytes += ast_adsi_load_soft_key(buf + bytes, ADSI_KEY_APPS + 12 + 5, "Cancel", "Cancel", "#", 1);
	ast_adsi_transmit_message(chan, buf, bytes, ADSI_MSG_DOWNLOAD);

#ifdef DISPLAY
	/* Add another dot */
	bytes = 0;
	bytes += ast_adsi_display(buf + bytes, ADSI_COMM_PAGE, 4, ADSI_JUST_LEFT, 0, "   .....", "");
	bytes += ast_adsi_set_line(buf + bytes, ADSI_COMM_PAGE, 1);
	ast_adsi_transmit_message(chan, buf, bytes, ADSI_MSG_DISPLAY);
#endif

	if (ast_adsi_end_download(chan)) {
		bytes = 0;
		bytes += ast_adsi_display(buf + bytes, ADSI_COMM_PAGE, 3, ADSI_JUST_CENT, 0, "Download Unsuccessful.", "");
		bytes += ast_adsi_display(buf + bytes, ADSI_COMM_PAGE, 4, ADSI_JUST_CENT, 0, "ADSI Unavailable", "");
		bytes += ast_adsi_set_line(buf + bytes, ADSI_COMM_PAGE, 1);
		bytes += ast_adsi_voice_mode(buf + bytes, 0);
		ast_adsi_transmit_message(chan, buf, bytes, ADSI_MSG_DISPLAY);
		return 0;
	}
	bytes = 0;
	bytes += ast_adsi_download_disconnect(buf + bytes);
	bytes += ast_adsi_voice_mode(buf + bytes, 0);
	ast_adsi_transmit_message(chan, buf, bytes, ADSI_MSG_DOWNLOAD);

	if (option_debug)
		ast_log(LOG_DEBUG, "Done downloading scripts...\n");

#ifdef DISPLAY
	/* Add last dot */
	bytes = 0;
	bytes += ast_adsi_display(buf + bytes, ADSI_COMM_PAGE, 4, ADSI_JUST_CENT, 0, "   ......", "");
	bytes += ast_adsi_set_line(buf + bytes, ADSI_COMM_PAGE, 1);
#endif
	if (option_debug)
		ast_log(LOG_DEBUG, "Restarting session...\n");

	bytes = 0;
	/* Load the session now */
	if (ast_adsi_load_session(chan, adsifdn, adsiver, 1) == 1) {
		*useadsi = 1;
		bytes += ast_adsi_display(buf + bytes, ADSI_COMM_PAGE, 3, ADSI_JUST_CENT, 0, "Scripts Loaded!", "");
	} else
		bytes += ast_adsi_display(buf + bytes, ADSI_COMM_PAGE, 3, ADSI_JUST_CENT, 0, "Load Failed!", "");

	ast_adsi_transmit_message(chan, buf, bytes, ADSI_MSG_DISPLAY);
	return 0;
}

static void adsi_begin(struct ast_channel *chan, int *useadsi)
{
	int x;
	if (!ast_adsi_available(chan))
		return;
	x = ast_adsi_load_session(chan, adsifdn, adsiver, 1);
	if (x < 0)
		return;
	if (!x) {
		if (adsi_load_vmail(chan, useadsi)) {
			ast_log(LOG_WARNING, "Unable to upload voicemail scripts\n");
			return;
		}
	} else
		*useadsi = 1;
}

static void adsi_login(struct ast_channel *chan)
{
	unsigned char buf[256];
	int bytes=0;
	unsigned char keys[8];
	int x;
	if (!ast_adsi_available(chan))
		return;

	for (x=0;x<8;x++)
		keys[x] = 0;
	/* Set one key for next */
	keys[3] = ADSI_KEY_APPS + 3;

	bytes += adsi_logo(buf + bytes);
	bytes += ast_adsi_display(buf + bytes, ADSI_COMM_PAGE, 3, ADSI_JUST_CENT, 0, " ", "");
	bytes += ast_adsi_display(buf + bytes, ADSI_COMM_PAGE, 4, ADSI_JUST_CENT, 0, " ", "");
	bytes += ast_adsi_set_line(buf + bytes, ADSI_COMM_PAGE, 1);
	bytes += ast_adsi_input_format(buf + bytes, 1, ADSI_DIR_FROM_LEFT, 0, "Mailbox: ******", "");
	bytes += ast_adsi_input_control(buf + bytes, ADSI_COMM_PAGE, 4, 1, 1, ADSI_JUST_LEFT);
	bytes += ast_adsi_load_soft_key(buf + bytes, ADSI_KEY_APPS + 3, "Enter", "Enter", "#", 1);
	bytes += ast_adsi_set_keys(buf + bytes, keys);
	bytes += ast_adsi_voice_mode(buf + bytes, 0);
	ast_adsi_transmit_message(chan, buf, bytes, ADSI_MSG_DISPLAY);
}

static void adsi_password(struct ast_channel *chan)
{
	unsigned char buf[256];
	int bytes=0;
	unsigned char keys[8];
	int x;
	if (!ast_adsi_available(chan))
		return;

	for (x=0;x<8;x++)
		keys[x] = 0;
	/* Set one key for next */
	keys[3] = ADSI_KEY_APPS + 3;

	bytes += ast_adsi_set_line(buf + bytes, ADSI_COMM_PAGE, 1);
	bytes += ast_adsi_input_format(buf + bytes, 1, ADSI_DIR_FROM_LEFT, 0, "Password: ******", "");
	bytes += ast_adsi_input_control(buf + bytes, ADSI_COMM_PAGE, 4, 0, 1, ADSI_JUST_LEFT);
	bytes += ast_adsi_set_keys(buf + bytes, keys);
	bytes += ast_adsi_voice_mode(buf + bytes, 0);
	ast_adsi_transmit_message(chan, buf, bytes, ADSI_MSG_DISPLAY);
}

static void adsi_folders(struct ast_channel *chan, int start, char *label)
{
	unsigned char buf[256];
	int bytes=0;
	unsigned char keys[8];
	int x,y;

	if (!ast_adsi_available(chan))
		return;

	for (x=0;x<5;x++) {
		y = ADSI_KEY_APPS + 12 + start + x;
		if (y > ADSI_KEY_APPS + 12 + 4)
			y = 0;
		keys[x] = ADSI_KEY_SKT | y;
	}
	keys[5] = ADSI_KEY_SKT | (ADSI_KEY_APPS + 17);
	keys[6] = 0;
	keys[7] = 0;

	bytes += ast_adsi_display(buf + bytes, ADSI_COMM_PAGE, 1, ADSI_JUST_CENT, 0, label, "");
	bytes += ast_adsi_display(buf + bytes, ADSI_COMM_PAGE, 2, ADSI_JUST_CENT, 0, " ", "");
	bytes += ast_adsi_set_line(buf + bytes, ADSI_COMM_PAGE, 1);
	bytes += ast_adsi_set_keys(buf + bytes, keys);
	bytes += ast_adsi_voice_mode(buf + bytes, 0);

	ast_adsi_transmit_message(chan, buf, bytes, ADSI_MSG_DISPLAY);
}

static void adsi_message(struct ast_channel *chan, struct vm_state *vms)
{
	int bytes=0;
	unsigned char buf[256]; 
	char buf1[256], buf2[256];
	char fn2[PATH_MAX];

	char cid[256]="";
	char *val;
	char *name, *num;
	char datetime[21]="";
	FILE *f;

	unsigned char keys[8];

	int x;

	if (!ast_adsi_available(chan))
		return;

	/* Retrieve important info */
	snprintf(fn2, sizeof(fn2), "%s.txt", vms->fn);
	f = fopen(fn2, "r");
	if (f) {
		while (!feof(f)) {	
			fgets((char *)buf, sizeof(buf), f);
			if (!feof(f)) {
				char *stringp=NULL;
				stringp = (char *)buf;
				strsep(&stringp, "=");
				val = strsep(&stringp, "=");
				if (!ast_strlen_zero(val)) {
					if (!strcmp((char *)buf, "callerid"))
						ast_copy_string(cid, val, sizeof(cid));
					if (!strcmp((char *)buf, "origdate"))
						ast_copy_string(datetime, val, sizeof(datetime));
				}
			}
		}
		fclose(f);
	}
	/* New meaning for keys */
	for (x=0;x<5;x++)
		keys[x] = ADSI_KEY_SKT | (ADSI_KEY_APPS + 6 + x);
	keys[6] = 0x0;
	keys[7] = 0x0;

	if (!vms->curmsg) {
		/* No prev key, provide "Folder" instead */
		keys[0] = ADSI_KEY_SKT | (ADSI_KEY_APPS + 1);
	}
	if (vms->curmsg >= vms->lastmsg) {
		/* If last message ... */
		if (vms->curmsg) {
			/* but not only message, provide "Folder" instead */
			keys[3] = ADSI_KEY_SKT | (ADSI_KEY_APPS + 1);
			bytes += ast_adsi_voice_mode(buf + bytes, 0);

		} else {
			/* Otherwise if only message, leave blank */
			keys[3] = 1;
		}
	}

	if (!ast_strlen_zero(cid)) {
		ast_callerid_parse(cid, &name, &num);
		if (!name)
			name = num;
	} else
		name = "Unknown Caller";

	/* If deleted, show "undeleted" */

	if (vms->deleted[vms->curmsg])
		keys[1] = ADSI_KEY_SKT | (ADSI_KEY_APPS + 11);

	/* Except "Exit" */
	keys[5] = ADSI_KEY_SKT | (ADSI_KEY_APPS + 5);
	snprintf(buf1, sizeof(buf1), "%s%s", vms->curbox,
		strcasecmp(vms->curbox, "INBOX") ? " Messages" : "");
	snprintf(buf2, sizeof(buf2), "Message %d of %d", vms->curmsg + 1, vms->lastmsg + 1);

	bytes += ast_adsi_display(buf + bytes, ADSI_COMM_PAGE, 1, ADSI_JUST_LEFT, 0, buf1, "");
	bytes += ast_adsi_display(buf + bytes, ADSI_COMM_PAGE, 2, ADSI_JUST_LEFT, 0, buf2, "");
	bytes += ast_adsi_display(buf + bytes, ADSI_COMM_PAGE, 3, ADSI_JUST_LEFT, 0, name, "");
	bytes += ast_adsi_display(buf + bytes, ADSI_COMM_PAGE, 4, ADSI_JUST_LEFT, 0, datetime, "");
	bytes += ast_adsi_set_line(buf + bytes, ADSI_COMM_PAGE, 1);
	bytes += ast_adsi_set_keys(buf + bytes, keys);
	bytes += ast_adsi_voice_mode(buf + bytes, 0);

	ast_adsi_transmit_message(chan, buf, bytes, ADSI_MSG_DISPLAY);
}

static void adsi_delete(struct ast_channel *chan, struct vm_state *vms)
{
	int bytes=0;
	unsigned char buf[256];
	unsigned char keys[8];

	int x;

	if (!ast_adsi_available(chan))
		return;

	/* New meaning for keys */
	for (x=0;x<5;x++)
		keys[x] = ADSI_KEY_SKT | (ADSI_KEY_APPS + 6 + x);

	keys[6] = 0x0;
	keys[7] = 0x0;

	if (!vms->curmsg) {
		/* No prev key, provide "Folder" instead */
		keys[0] = ADSI_KEY_SKT | (ADSI_KEY_APPS + 1);
	}
	if (vms->curmsg >= vms->lastmsg) {
		/* If last message ... */
		if (vms->curmsg) {
			/* but not only message, provide "Folder" instead */
			keys[3] = ADSI_KEY_SKT | (ADSI_KEY_APPS + 1);
		} else {
			/* Otherwise if only message, leave blank */
			keys[3] = 1;
		}
	}

	/* If deleted, show "undeleted" */
	if (vms->deleted[vms->curmsg]) 
		keys[1] = ADSI_KEY_SKT | (ADSI_KEY_APPS + 11);

	/* Except "Exit" */
	keys[5] = ADSI_KEY_SKT | (ADSI_KEY_APPS + 5);
	bytes += ast_adsi_set_keys(buf + bytes, keys);
	bytes += ast_adsi_voice_mode(buf + bytes, 0);

	ast_adsi_transmit_message(chan, buf, bytes, ADSI_MSG_DISPLAY);
}

static void adsi_status(struct ast_channel *chan, struct vm_state *vms)
{
	unsigned char buf[256] = "";
	char buf1[256] = "", buf2[256] = "";
	int bytes=0;
	unsigned char keys[8];
	int x;

	char *newm = (vms->newmessages == 1) ? "message" : "messages";
	char *oldm = (vms->oldmessages == 1) ? "message" : "messages";
	if (!ast_adsi_available(chan))
		return;
	if (vms->newmessages) {
		snprintf(buf1, sizeof(buf1), "You have %d new", vms->newmessages);
		if (vms->oldmessages) {
			strncat(buf1, " and", sizeof(buf1) - strlen(buf1) - 1);
			snprintf(buf2, sizeof(buf2), "%d old %s.", vms->oldmessages, oldm);
		} else {
			snprintf(buf2, sizeof(buf2), "%s.", newm);
		}
	} else if (vms->oldmessages) {
		snprintf(buf1, sizeof(buf1), "You have %d old", vms->oldmessages);
		snprintf(buf2, sizeof(buf2), "%s.", oldm);
	} else {
		strcpy(buf1, "You have no messages.");
		buf2[0] = ' ';
		buf2[1] = '\0';
	}
	bytes += ast_adsi_display(buf + bytes, ADSI_COMM_PAGE, 1, ADSI_JUST_LEFT, 0, buf1, "");
	bytes += ast_adsi_display(buf + bytes, ADSI_COMM_PAGE, 2, ADSI_JUST_LEFT, 0, buf2, "");
	bytes += ast_adsi_set_line(buf + bytes, ADSI_COMM_PAGE, 1);

	for (x=0;x<6;x++)
		keys[x] = ADSI_KEY_SKT | (ADSI_KEY_APPS + x);
	keys[6] = 0;
	keys[7] = 0;

	/* Don't let them listen if there are none */
	if (vms->lastmsg < 0)
		keys[0] = 1;
	bytes += ast_adsi_set_keys(buf + bytes, keys);

	bytes += ast_adsi_voice_mode(buf + bytes, 0);

	ast_adsi_transmit_message(chan, buf, bytes, ADSI_MSG_DISPLAY);
}

static void adsi_status2(struct ast_channel *chan, struct vm_state *vms)
{
	unsigned char buf[256] = "";
	char buf1[256] = "", buf2[256] = "";
	int bytes=0;
	unsigned char keys[8];
	int x;

	char *mess = (vms->lastmsg == 0) ? "message" : "messages";

	if (!ast_adsi_available(chan))
		return;

	/* Original command keys */
	for (x=0;x<6;x++)
		keys[x] = ADSI_KEY_SKT | (ADSI_KEY_APPS + x);

	keys[6] = 0;
	keys[7] = 0;

	if ((vms->lastmsg + 1) < 1)
		keys[0] = 0;

	snprintf(buf1, sizeof(buf1), "%s%s has", vms->curbox,
		strcasecmp(vms->curbox, "INBOX") ? " folder" : "");

	if (vms->lastmsg + 1)
		snprintf(buf2, sizeof(buf2), "%d %s.", vms->lastmsg + 1, mess);
	else
		strcpy(buf2, "no messages.");
	bytes += ast_adsi_display(buf + bytes, ADSI_COMM_PAGE, 1, ADSI_JUST_LEFT, 0, buf1, "");
	bytes += ast_adsi_display(buf + bytes, ADSI_COMM_PAGE, 2, ADSI_JUST_LEFT, 0, buf2, "");
	bytes += ast_adsi_display(buf + bytes, ADSI_COMM_PAGE, 3, ADSI_JUST_LEFT, 0, "", "");
	bytes += ast_adsi_set_line(buf + bytes, ADSI_COMM_PAGE, 1);
	bytes += ast_adsi_set_keys(buf + bytes, keys);

	bytes += ast_adsi_voice_mode(buf + bytes, 0);

	ast_adsi_transmit_message(chan, buf, bytes, ADSI_MSG_DISPLAY);
	
}

/*
static void adsi_clear(struct ast_channel *chan)
{
	char buf[256];
	int bytes=0;
	if (!ast_adsi_available(chan))
		return;
	bytes += ast_adsi_set_line(buf + bytes, ADSI_COMM_PAGE, 1);
	bytes += ast_adsi_voice_mode(buf + bytes, 0);

	ast_adsi_transmit_message(chan, buf, bytes, ADSI_MSG_DISPLAY);
}
*/

static void adsi_goodbye(struct ast_channel *chan)
{
	unsigned char buf[256];
	int bytes=0;

	if (!ast_adsi_available(chan))
		return;
	bytes += adsi_logo(buf + bytes);
	bytes += ast_adsi_display(buf + bytes, ADSI_COMM_PAGE, 3, ADSI_JUST_LEFT, 0, " ", "");
	bytes += ast_adsi_display(buf + bytes, ADSI_COMM_PAGE, 4, ADSI_JUST_CENT, 0, "Goodbye", "");
	bytes += ast_adsi_set_line(buf + bytes, ADSI_COMM_PAGE, 1);
	bytes += ast_adsi_voice_mode(buf + bytes, 0);

	ast_adsi_transmit_message(chan, buf, bytes, ADSI_MSG_DISPLAY);
}

/*--- get_folder: Folder menu ---*/
/* Plays "press 1 for INBOX messages" etc
   Should possibly be internationalized
 */
static int get_folder(struct ast_channel *chan, int start)
{
	int x;
	int d;
	char fn[PATH_MAX];
	d = ast_play_and_wait(chan, "vm-press");	/* "Press" */
	if (d)
		return d;
	for (x = start; x< 5; x++) {	/* For all folders */
		if ((d = ast_say_number(chan, x, AST_DIGIT_ANY, chan->language, (char *) NULL)))
			return d;
		d = ast_play_and_wait(chan, "vm-for");	/* "for" */
		if (d)
			return d;
		snprintf(fn, sizeof(fn), "vm-%s", mbox(x));	/* Folder name */
		d = vm_play_folder_name(chan, fn);
		if (d)
			return d;
		d = ast_waitfordigit(chan, 500);
		if (d)
			return d;
	}
	d = ast_play_and_wait(chan, "vm-tocancel"); /* "or pound to cancel" */
	if (d)
		return d;
	d = ast_waitfordigit(chan, 4000);
	return d;
}

static int get_folder2(struct ast_channel *chan, char *fn, int start)
{
	int res = 0;
	res = ast_play_and_wait(chan, fn);	/* Folder name */
	while (((res < '0') || (res > '9')) &&
			(res != '#') && (res >= 0)) {
		res = get_folder(chan, 0);
	}
	return res;
}

static int vm_forwardoptions(struct ast_channel *chan, struct ast_vm_user *vmu, char *curdir, int curmsg, char *vmfmts,
			     char *context, signed char record_gain, long *duration, struct vm_state *vms)
{
	int cmd = 0;
	int retries = 0;
	signed char zero_gain = 0;

	while ((cmd >= 0) && (cmd != 't') && (cmd != '*')) {
		if (cmd)
			retries = 0;
		switch (cmd) {
		case '1': 
			/* prepend a message to the current message, update the metadata and return */
		{
			char msgfile[PATH_MAX];
			char textfile[PATH_MAX];
			int prepend_duration = 0;
			struct ast_config *msg_cfg;
			const char *duration_str;

			make_file(msgfile, sizeof(msgfile), curdir, curmsg);
			strcpy(textfile, msgfile);
			strncat(textfile, ".txt", sizeof(textfile) - 1);
			*duration = 0;

			/* if we can't read the message metadata, stop now */
			if (!(msg_cfg = ast_config_load(textfile))) {
				cmd = 0;
				break;
			}

			if (record_gain)
				ast_channel_setoption(chan, AST_OPTION_RXGAIN, &record_gain, sizeof(record_gain), 0);

			cmd = ast_play_and_prepend(chan, NULL, msgfile, 0, vmfmts, &prepend_duration, 1, silencethreshold, maxsilence);
			if (record_gain)
				ast_channel_setoption(chan, AST_OPTION_RXGAIN, &zero_gain, sizeof(zero_gain), 0);

			
			if ((duration_str = ast_variable_retrieve(msg_cfg, "message", "duration")))
				*duration = atoi(duration_str);

			if (prepend_duration) {
				struct ast_category *msg_cat;
				/* need enough space for a maximum-length message duration */
				char duration_str[12];

				*duration += prepend_duration;
				msg_cat = ast_category_get(msg_cfg, "message");
				snprintf(duration_str, 11, "%ld", *duration);
				if (!ast_variable_update(msg_cat, "duration", duration_str, NULL)) {
					config_text_file_save(textfile, msg_cfg, "app_voicemail");
					STORE(curdir, vmu->mailbox, context, curmsg, chan, vmu, vmfmts, *duration, vms);
				}
			}

			ast_config_destroy(msg_cfg);

			break;
		}
		case '2': 
			cmd = 't';
			break;
		case '*':
			cmd = '*';
			break;
		default: 
			cmd = ast_play_and_wait(chan,"vm-forwardoptions");
				/* "Press 1 to prepend a message or 2 to forward the message without prepending" */
			if (!cmd)
				cmd = ast_play_and_wait(chan,"vm-starmain");
				/* "press star to return to the main menu" */
			if (!cmd)
				cmd = ast_waitfordigit(chan,6000);
			if (!cmd)
				retries++;
			if (retries > 3)
				cmd = 't';
		}
	}
	if (cmd == 't' || cmd == 'S')
		cmd = 0;
	return cmd;
}

static int notify_new_message(struct ast_channel *chan, struct ast_vm_user *vmu, int msgnum, long duration, char *fmt, char *cidnum, char *cidname)
{
	char todir[PATH_MAX], fn[PATH_MAX], ext_context[PATH_MAX], *stringp;
	int newmsgs = 0, oldmsgs = 0;
	const char *category = pbx_builtin_getvar_helper(chan, "VM_CATEGORY");
	char *myserveremail = serveremail;

	make_dir(todir, sizeof(todir), vmu->context, vmu->mailbox, "INBOX");
	make_file(fn, sizeof(fn), todir, msgnum);
	snprintf(ext_context, sizeof(ext_context), "%s@%s", vmu->mailbox, vmu->context);

	if (!ast_strlen_zero(vmu->attachfmt)) {
		if (strstr(fmt, vmu->attachfmt))
			fmt = vmu->attachfmt;
		 else
			ast_log(LOG_WARNING, "Attachment format '%s' is not one of the recorded formats '%s'.  Falling back to default format for '%s@%s'.\n", vmu->attachfmt, fmt, vmu->mailbox, vmu->context);
	}

	/* Attach only the first format */
	fmt = ast_strdupa(fmt);
	stringp = fmt;
	strsep(&stringp, "|");

	if (!ast_strlen_zero(vmu->serveremail))
		myserveremail = vmu->serveremail;

	if (!ast_strlen_zero(vmu->email)) {
		int attach_user_voicemail = ast_test_flag(vmu, VM_ATTACH);
		if (!attach_user_voicemail)
			attach_user_voicemail = ast_test_flag((&globalflags), VM_ATTACH);

		/*XXX possible imap issue, should category be NULL XXX*/
		sendmail(myserveremail, vmu, msgnum, vmu->context, vmu->mailbox, cidnum, cidname, fn, fmt, duration, attach_user_voicemail, chan, category);
	}

	if (!ast_strlen_zero(vmu->pager))
		sendpage(myserveremail, vmu->pager, msgnum, vmu->context, vmu->mailbox, cidnum, cidname, duration, vmu, category);

	if (ast_test_flag(vmu, VM_DELETE))
		DELETE(todir, msgnum, fn);

#ifdef IMAP_STORAGE
	DELETE(todir, msgnum, fn);
#endif
	/* Leave voicemail for someone */
	if (ast_app_has_voicemail(ext_context, NULL)) 
		ast_app_inboxcount(ext_context, &newmsgs, &oldmsgs);

	manager_event(EVENT_FLAG_CALL, "MessageWaiting", "Mailbox: %s@%s\r\nWaiting: %d\r\nNew: %d\r\nOld: %d\r\n", vmu->mailbox, vmu->context, ast_app_has_voicemail(ext_context, NULL), newmsgs, oldmsgs);
	run_externnotify(vmu->context, vmu->mailbox);
	return 0;
}

static int forward_message(struct ast_channel *chan, char *context, struct vm_state *vms, struct ast_vm_user *sender, char *fmt, int flag, signed char record_gain)
{
#ifdef IMAP_STORAGE
	BODY *body;
	char *header_content;
	char *temp;
	char todir[256];
	int todircount=0;
#endif
	char username[70]="";
	int res = 0, cmd = 0;
	struct ast_vm_user *receiver = NULL, *vmtmp;
	AST_LIST_HEAD_NOLOCK_STATIC(extensions, ast_vm_user);
	char *stringp;
	const char *s;
	int saved_messages = 0, found = 0;
	int valid_extensions = 0;
	char *dir;
	int curmsg;

	if (vms == NULL) return -1;
	dir = vms->curdir;
	curmsg = vms->curmsg;
	
	while (!res && !valid_extensions) {
		int use_directory = 0;
		if (ast_test_flag((&globalflags), VM_DIRECFORWARD)) {
			int done = 0;
			int retries = 0;
			cmd=0;
			while ((cmd >= 0) && !done ){
				if (cmd)
					retries = 0;
				switch (cmd) {
				case '1': 
					use_directory = 0;
					done = 1;
					break;
				case '2': 
					use_directory = 1;
					done=1;
					break;
				case '*': 
					cmd = 't';
					done = 1;
					break;
				default: 
					/* Press 1 to enter an extension press 2 to use the directory */
					cmd = ast_play_and_wait(chan,"vm-forward");
					if (!cmd)
						cmd = ast_waitfordigit(chan,3000);
					if (!cmd)
						retries++;
					if (retries > 3)
					{
						cmd = 't';
						done = 1;
					}
					
				}
			}
			if (cmd < 0 || cmd == 't')
				break;
		}
		
		if (use_directory) {
			/* use app_directory */
			
			char old_context[sizeof(chan->context)];
			char old_exten[sizeof(chan->exten)];
			int old_priority;
			struct ast_app* app;

			
			app = pbx_findapp("Directory");
			if (app) {
				char vmcontext[256];
				/* make backup copies */
				memcpy(old_context, chan->context, sizeof(chan->context));
				memcpy(old_exten, chan->exten, sizeof(chan->exten));
				old_priority = chan->priority;
				
				/* call the the Directory, changes the channel */
				sprintf(vmcontext, "%s||v", context ? context : "default");
				res = pbx_exec(chan, app, vmcontext);
				
				ast_copy_string(username, chan->exten, sizeof(username));
				
				/* restore the old context, exten, and priority */
				memcpy(chan->context, old_context, sizeof(chan->context));
				memcpy(chan->exten, old_exten, sizeof(chan->exten));
				chan->priority = old_priority;
				
			} else {
				ast_log(LOG_WARNING, "Could not find the Directory application, disabling directory_forward\n");
				ast_clear_flag((&globalflags), VM_DIRECFORWARD);	
			}
		} else {
			/* Ask for an extension */
			res = ast_streamfile(chan, "vm-extension", chan->language);	/* "extension" */
			if (res)
				break;
			if ((res = ast_readstring(chan, username, sizeof(username) - 1, 2000, 10000, "#") < 0))
				break;
		}
		
		/* start all over if no username */
		if (ast_strlen_zero(username))
			continue;
		stringp = username;
		s = strsep(&stringp, "*");
		/* start optimistic */
		valid_extensions = 1;
		while (s) {
			/* Don't forward to ourselves.  find_user is going to malloc since we have a NULL as first argument */
			if (strcmp(s,sender->mailbox) && (receiver = find_user(NULL, context, s))) {
				AST_LIST_INSERT_HEAD(&extensions, receiver, list);
				found++;
			} else {
				valid_extensions = 0;
				break;
			}
			s = strsep(&stringp, "*");
		}
		/* break from the loop of reading the extensions */
		if (valid_extensions)
			break;
		/* "I am sorry, that's not a valid extension.  Please try again." */
		res = ast_play_and_wait(chan, "pbx-invalid");
	}
	/* check if we're clear to proceed */
	if (AST_LIST_EMPTY(&extensions) || !valid_extensions)
		return res;
	if (flag==1) {
		struct leave_vm_options leave_options;
		char mailbox[AST_MAX_EXTENSION * 2 + 2];
		snprintf(mailbox, sizeof(mailbox), "%s@%s", username, context);

		/* Send VoiceMail */
		memset(&leave_options, 0, sizeof(leave_options));
		leave_options.record_gain = record_gain;
		cmd = leave_voicemail(chan, mailbox, &leave_options);
	} else {

		/* Forward VoiceMail */
		long duration = 0;
#ifdef IMAP_STORAGE
		char *myserveremail = serveremail;
		char buf[1024] = "";
		int attach_user_voicemail = ast_test_flag((&globalflags), VM_ATTACH);
#endif
 		RETRIEVE(dir, curmsg);
		cmd = vm_forwardoptions(chan, sender, dir, curmsg, vmfmts, S_OR(context, "default"), record_gain, &duration, vms);
		if (!cmd) {
			AST_LIST_TRAVERSE_SAFE_BEGIN(&extensions, vmtmp, list) {
#ifdef IMAP_STORAGE
 				/* Need to get message content */
				if(option_debug > 2)
	 				ast_log (LOG_DEBUG,"Before mail_fetchheaders, curmsg is: %d, imap messages is %lu\n",vms->curmsg, vms->msgArray[vms->curmsg]);
 				if (!vms->msgArray[vms->curmsg]) {
 					ast_log (LOG_WARNING,"Trying to access unknown message\n");
 					return -1;
 				}
 
 				/* This will only work for new messages... */
				header_content = mail_fetchheader (vms->mailstream, vms->msgArray[vms->curmsg]);
 				/* empty string means no valid header */
 				if (ast_strlen_zero(header_content)) {
 					ast_log (LOG_ERROR,"Could not fetch header for message number %ld\n",vms->msgArray[vms->curmsg]);
 					return -1;
 				}
 				/* Get header info needed by sendmail */
				if ((temp = get_header_by_tag(header_content, "X-Asterisk-VM-Duration:", buf, sizeof(buf))))
 					duration = atoi(temp);
 				else
 					duration = 0;
				
 				/* Attach only the first format */
				if ((fmt = ast_strdupa(fmt))) {
 					stringp = fmt;
 					strsep(&stringp, "|");
 				} else {
 					ast_log (LOG_ERROR,"audio format not set. Default to WAV\n");
 					fmt = "WAV";
 				}
 				if (!strcasecmp(fmt, "wav49"))
 					fmt = "WAV";
 				if (option_debug > 2)
	 				ast_log (LOG_DEBUG,"**** format set to %s, vmfmts set to %s\n",fmt,vmfmts);
 				/* ast_copy_string(fmt, vmfmts, sizeof(fmt));*/
 				/* if (!ast_strlen_zero(fmt)) { */
				snprintf(todir, sizeof(todir), "%s%s/%s/tmp", VM_SPOOL_DIR, vmtmp->context, vmtmp->mailbox);
 				make_gsm_file(vms->fn, vms->imapuser, todir, vms->curmsg);
				if (option_debug > 2)
	 				ast_log (LOG_DEBUG,"Before mail_fetchstructure, message number is %ld, filename is:%s\n",vms->msgArray[vms->curmsg], vms->fn);
				/*mail_fetchstructure (mailstream, vmArray[0], &body); */
				mail_fetchstructure (vms->mailstream, vms->msgArray[vms->curmsg], &body);
				save_body(body,vms,"3","gsm");
 				/* should not assume "fmt" here! */
				save_body(body,vms,"2",fmt);
 
				STORE(todir, vmtmp->mailbox, vmtmp->context, vms->curmsg, chan, vmtmp, fmt, duration, vms);

				if (!ast_strlen_zero(vmtmp->serveremail))
					myserveremail = vmtmp->serveremail;
				attach_user_voicemail = ast_test_flag(vmtmp, VM_ATTACH);
				/* NULL category for IMAP storage */
				sendmail(myserveremail, vmtmp, todircount, vmtmp->context, vmtmp->mailbox, S_OR(chan->cid.cid_num, NULL), S_OR(chan->cid.cid_name, NULL), vms->fn, fmt, duration, attach_user_voicemail, chan, NULL);

#else
				copy_message(chan, sender, 0, curmsg, duration, vmtmp, fmt, dir);
#endif
				saved_messages++;
				AST_LIST_REMOVE_CURRENT(&extensions, list);
				free_user(vmtmp);
				if (res)
					break;
			}
			AST_LIST_TRAVERSE_SAFE_END;
			if (saved_messages > 0) {
				/* give confirmation that the message was saved */
				/* commented out since we can't forward batches yet
				if (saved_messages == 1)
					res = ast_play_and_wait(chan, "vm-message");
				else
					res = ast_play_and_wait(chan, "vm-messages");
				if (!res)
					res = ast_play_and_wait(chan, "vm-saved"); */
				res = ast_play_and_wait(chan, "vm-msgsaved");
			}	
		}
	}

	/* If anything failed above, we still have this list to free */
	while ((vmtmp = AST_LIST_REMOVE_HEAD(&extensions, list)))
		free_user(vmtmp);
	return res ? res : cmd;
}

static int wait_file2(struct ast_channel *chan, struct vm_state *vms, char *file)
{
	int res;
	if ((res = ast_stream_and_wait(chan, file, AST_DIGIT_ANY)) < 0) 
		ast_log(LOG_WARNING, "Unable to play message %s\n", file); 
	return res;
}

static int wait_file(struct ast_channel *chan, struct vm_state *vms, char *file) 
{
	return ast_control_streamfile(chan, file, "#", "*", "1456789", "0", "2", skipms);
}

static int play_message_category(struct ast_channel *chan, const char *category)
{
	int res = 0;

	if (!ast_strlen_zero(category))
		res = ast_play_and_wait(chan, category);

	if (res) {
		ast_log(LOG_WARNING, "No sound file for category '%s' was found.\n", category);
		res = 0;
	}

	return res;
}

static int play_message_datetime(struct ast_channel *chan, struct ast_vm_user *vmu, const char *origtime, const char *filename)
{
	int res = 0;
	struct vm_zone *the_zone = NULL;
	time_t t;

	if (ast_get_time_t(origtime, &t, 0, NULL)) {
		ast_log(LOG_WARNING, "Couldn't find origtime in %s\n", filename);
		return 0;
	}

	/* Does this user have a timezone specified? */
	if (!ast_strlen_zero(vmu->zonetag)) {
		/* Find the zone in the list */
		struct vm_zone *z;
		AST_LIST_LOCK(&zones);
		AST_LIST_TRAVERSE(&zones, z, list) {
			if (!strcmp(z->name, vmu->zonetag)) {
				the_zone = z;
				break;
			}
		}
		AST_LIST_UNLOCK(&zones);
	}

/* No internal variable parsing for now, so we'll comment it out for the time being */
#if 0
	/* Set the DIFF_* variables */
	localtime_r(&t, &time_now);
	tv_now = ast_tvnow();
	tnow = tv_now.tv_sec;
	localtime_r(&tnow,&time_then);

	/* Day difference */
	if (time_now.tm_year == time_then.tm_year)
		snprintf(temp,sizeof(temp),"%d",time_now.tm_yday);
	else
		snprintf(temp,sizeof(temp),"%d",(time_now.tm_year - time_then.tm_year) * 365 + (time_now.tm_yday - time_then.tm_yday));
	pbx_builtin_setvar_helper(chan, "DIFF_DAY", temp);

	/* Can't think of how other diffs might be helpful, but I'm sure somebody will think of something. */
#endif
	if (the_zone)
		res = ast_say_date_with_format(chan, t, AST_DIGIT_ANY, chan->language, the_zone->msg_format, the_zone->timezone);
	else if (!strcasecmp(chan->language,"pl"))       /* POLISH syntax */
		res = ast_say_date_with_format(chan, t, AST_DIGIT_ANY, chan->language, "'vm-received' Q HM", NULL);
	else if (!strcasecmp(chan->language,"se"))       /* SWEDISH syntax */
		res = ast_say_date_with_format(chan, t, AST_DIGIT_ANY, chan->language, "'vm-received' dB 'digits/at' k 'and' M", NULL);
	else if (!strcasecmp(chan->language,"no"))       /* NORWEGIAN syntax */
		res = ast_say_date_with_format(chan, t, AST_DIGIT_ANY, chan->language, "'vm-received' Q 'digits/at' HM", NULL);
	else if (!strcasecmp(chan->language,"de"))       /* GERMAN syntax */
		res = ast_say_date_with_format(chan, t, AST_DIGIT_ANY, chan->language, "'vm-received' Q 'digits/at' HM", NULL);
	else if (!strcasecmp(chan->language,"nl"))      /* DUTCH syntax */
		res = ast_say_date_with_format(chan, t, AST_DIGIT_ANY, chan->language, "'vm-received' q 'digits/nl-om' HM", NULL);
	else if (!strcasecmp(chan->language,"it"))      /* ITALIAN syntax */
		res = ast_say_date_with_format(chan, t, AST_DIGIT_ANY, chan->language, "'vm-received' q 'digits/at' 'digits/hours' k 'digits/e' M 'digits/minutes'", NULL);
	else if (!strcasecmp(chan->language,"gr"))
		res = ast_say_date_with_format(chan, t, AST_DIGIT_ANY, chan->language, "'vm-received' q  H 'digits/kai' M ", NULL);
	else if (!strcasecmp(chan->language,"pt_BR"))
		res = ast_say_date_with_format(chan, t, AST_DIGIT_ANY, chan->language, "'vm-received' Ad 'digits/pt-de' B 'digits/pt-de' Y 'digits/pt-as' HM ", NULL);
	else
		res = ast_say_date_with_format(chan, t, AST_DIGIT_ANY, chan->language, "'vm-received' q 'digits/at' IMp", NULL);
#if 0
	pbx_builtin_setvar_helper(chan, "DIFF_DAY", NULL);
#endif
	return res;
}



static int play_message_callerid(struct ast_channel *chan, struct vm_state *vms, char *cid, const char *context, int callback)
{
	int res = 0;
	int i;
	char *callerid, *name;
	char prefile[PATH_MAX] = "";
	

	/* If voicemail cid is not enabled, or we didn't get cid or context from the attribute file, leave now. */
	/* BB: Still need to change this so that if this function is called by the message envelope (and someone is explicitly requesting to hear the CID), it does not check to see if CID is enabled in the config file */
	if ((cid == NULL)||(context == NULL))
		return res;

	/* Strip off caller ID number from name */
	if (option_debug)
		ast_log(LOG_DEBUG, "VM-CID: composite caller ID received: %s, context: %s\n", cid, context);
	ast_callerid_parse(cid, &name, &callerid);
	if ((!ast_strlen_zero(callerid)) && strcmp(callerid, "Unknown")) {
		/* Check for internal contexts and only */
		/* say extension when the call didn't come from an internal context in the list */
		for (i = 0 ; i < MAX_NUM_CID_CONTEXTS ; i++){
			if (option_debug)
				ast_log(LOG_DEBUG, "VM-CID: comparing internalcontext: %s\n", cidinternalcontexts[i]);
			if ((strcmp(cidinternalcontexts[i], context) == 0))
				break;
		}
		if (i != MAX_NUM_CID_CONTEXTS){ /* internal context? */
			if (!res) {
				snprintf(prefile, sizeof(prefile), "%s%s/%s/greet", VM_SPOOL_DIR, context, callerid);
				if (!ast_strlen_zero(prefile)) {
				/* See if we can find a recorded name for this person instead of their extension number */
					if (ast_fileexists(prefile, NULL, NULL) > 0) {
						if (option_verbose > 2)
							ast_verbose(VERBOSE_PREFIX_3 "Playing envelope info: CID number '%s' matches mailbox number, playing recorded name\n", callerid);
						if (!callback)
							res = wait_file2(chan, vms, "vm-from");
						res = ast_stream_and_wait(chan, prefile, "");
					} else {
						if (option_verbose > 2)
							ast_verbose(VERBOSE_PREFIX_3 "Playing envelope info: message from '%s'\n", callerid);
						/* BB: Say "from extension" as one saying to sound smoother */
						if (!callback)
							res = wait_file2(chan, vms, "vm-from-extension");
						res = ast_say_digit_str(chan, callerid, "", chan->language);
					}
				}
			}
		}

		else if (!res) {
			if (option_debug)
				ast_log(LOG_DEBUG, "VM-CID: Numeric caller id: (%s)\n", callerid);
			/* BB: Since this is all nicely figured out, why not say "from phone number" in this case" */
			if (!callback)
				res = wait_file2(chan, vms, "vm-from-phonenumber");
			res = ast_say_digit_str(chan, callerid, AST_DIGIT_ANY, chan->language);
		}
	} else {
		/* Number unknown */
		if (option_debug)
			ast_log(LOG_DEBUG, "VM-CID: From an unknown number\n");
		/* Say "from an unknown caller" as one phrase - it is already recorded by "the voice" anyhow */
		res = wait_file2(chan, vms, "vm-unknown-caller");
	}
	return res;
}

static int play_message_duration(struct ast_channel *chan, struct vm_state *vms, const char *duration, int minduration)
{
	int res = 0;
	int durationm;
	int durations;
	/* Verify that we have a duration for the message */
	if (duration == NULL)
		return res;

	/* Convert from seconds to minutes */
	durations=atoi(duration);
	durationm=(durations / 60);

	if (option_debug)
		ast_log(LOG_DEBUG, "VM-Duration: duration is: %d seconds converted to: %d minutes\n", durations, durationm);

	if ((!res) && (durationm >= minduration)) {
		res = wait_file2(chan, vms, "vm-duration");

		/* POLISH syntax */
		if (!strcasecmp(chan->language, "pl")) {
			div_t num = div(durationm, 10);

			if (durationm == 1) {
				res = ast_play_and_wait(chan, "digits/1z");
				res = res ? res : ast_play_and_wait(chan, "vm-minute-ta");
			} else if (num.rem > 1 && num.rem < 5 && num.quot != 1) {
				if (num.rem == 2) {
					if (!num.quot) {
						res = ast_play_and_wait(chan, "digits/2-ie");
					} else {
						res = say_and_wait(chan, durationm - 2 , chan->language);
						res = res ? res : ast_play_and_wait(chan, "digits/2-ie");
					}
				} else {
					res = say_and_wait(chan, durationm, chan->language);
				}
				res = res ? res : ast_play_and_wait(chan, "vm-minute-ty");
			} else {
				res = say_and_wait(chan, durationm, chan->language);
				res = res ? res : ast_play_and_wait(chan, "vm-minute-t");
			}
		/* DEFAULT syntax */
		} else {
			res = ast_say_number(chan, durationm, AST_DIGIT_ANY, chan->language, NULL);
			res = wait_file2(chan, vms, "vm-minutes");
		}
	}
	return res;
}

#ifdef IMAP_STORAGE
static int play_message(struct ast_channel *chan, struct ast_vm_user *vmu, struct vm_state *vms)
{
	BODY *body;
	char *header_content;
	char cid[256];
	char context[256];
	char origtime[32];
	char duration[16];
	char category[32];
	char todir[PATH_MAX];
	int res = 0;
	char *temp;
	char buf[1024];

	vms->starting = 0; 
	if(option_debug > 2)
		ast_log (LOG_DEBUG,"Before mail_fetchheaders, curmsg is: %d, imap messages is %lu\n",vms->curmsg, vms->msgArray[vms->curmsg]);

	if (!vms->msgArray[vms->curmsg]) {
		ast_log (LOG_WARNING,"Trying to access unknown message\n");
		return -1;
	}

	/* This will only work for new messages... */
	header_content = mail_fetchheader (vms->mailstream, vms->msgArray[vms->curmsg]);
	/* empty string means no valid header */
	if (ast_strlen_zero(header_content)) {
		ast_log (LOG_ERROR,"Could not fetch header for message number %ld\n",vms->msgArray[vms->curmsg]);
		return -1;
	}
	snprintf(todir, sizeof(todir), "%s%s/%s/tmp", VM_SPOOL_DIR, vmu->context, vmu->mailbox);
	make_gsm_file(vms->fn, vms->imapuser, todir, vms->curmsg);

	mail_fetchstructure (vms->mailstream,vms->msgArray[vms->curmsg],&body);
	save_body(body,vms,"3","gsm");

	adsi_message(chan, vms);
	if (!vms->curmsg)
		res = wait_file2(chan, vms, "vm-first");	/* "First" */
	else if (vms->curmsg == vms->lastmsg)
		res = wait_file2(chan, vms, "vm-last");		/* "last" */

	if (!res) {
		res = wait_file2(chan, vms, "vm-message");	/* "message" */
		if (vms->curmsg && (vms->curmsg != vms->lastmsg)) {
			if (!res)
				res = ast_say_number(chan, vms->curmsg + 1, AST_DIGIT_ANY, chan->language, (char *) NULL);
		}
	}

	/* Get info from headers!! */
	if ((temp = get_header_by_tag(header_content, "X-Asterisk-VM-Caller-ID-Num:", buf, sizeof(buf))))
		ast_copy_string(cid, temp, sizeof(cid)); 
	else 
		cid[0] = '\0';
	
	if ((temp = get_header_by_tag(header_content, "X-Asterisk-VM-Context:", buf, sizeof(buf))))
		ast_copy_string(context, temp, sizeof(context)); 
	else
		context[0] = '\0';

	if ((temp = get_header_by_tag(header_content, "X-Asterisk-VM-Orig-time:", buf, sizeof(buf))))
		ast_copy_string(origtime, temp, sizeof(origtime));
	else
		origtime[0] = '\0';

	if ((temp = get_header_by_tag(header_content, "X-Asterisk-VM-Duration:", buf, sizeof(buf))))
		ast_copy_string(duration,temp, sizeof(duration));
	else
		duration[0] = '\0';
	
	if ((temp = get_header_by_tag(header_content, "X-Asterisk-VM-Category:", buf, sizeof(buf))))
		ast_copy_string(category,temp, sizeof(category));
	else
		category[0] = '\0';

	/*if (!strncasecmp("macro",context,5)) Macro names in contexts are useless for our needs */
	/*	context = ast_variable_retrieve(msg_cfg, "message","macrocontext"); */
	if (res == '1')
		res = 0;

	if ((!res) && !ast_strlen_zero(category)) {
		res = play_message_category(chan, category);
	}

	if ((!res) && (ast_test_flag(vmu, VM_ENVELOPE)) && origtime[0] != '\0')
		res = play_message_datetime(chan, vmu, origtime, "IMAP_STORAGE");
	if ((!res) && (ast_test_flag(vmu, VM_SAYCID)) && cid[0] !='\0' && context[0] !='\0')
		res = play_message_callerid(chan, vms, cid, context, 0);

	if ((!res) && (ast_test_flag(vmu, VM_SAYDURATION)) && duration[0] != '\0')
		res = play_message_duration(chan, vms, duration, vmu->saydurationm);

	/* Allow pressing '1' to skip envelope / callerid */
	/* if (res == '1')
		res = 0;
	*/
	/*ast_config_destroy(msg_cfg);*/
	res = 0;

	if (!res) {
		vms->heard[vms->curmsg] = 1;
		res = wait_file(chan, vms, vms->fn);
	}
	DISPOSE(vms->curdir, vms->curmsg);
	DELETE(0, 0, vms->fn);
	return res;
}
#else
static int play_message(struct ast_channel *chan, struct ast_vm_user *vmu, struct vm_state *vms)
{
	int res = 0;
	char filename[256], *cid;
	const char *origtime, *context, *category, *duration;
	struct ast_config *msg_cfg;

	vms->starting = 0; 
	make_file(vms->fn, sizeof(vms->fn), vms->curdir, vms->curmsg);
	adsi_message(chan, vms);
	if (!vms->curmsg)
		res = wait_file2(chan, vms, "vm-first");	/* "First" */
	else if (vms->curmsg == vms->lastmsg)
		res = wait_file2(chan, vms, "vm-last");		/* "last" */
	if (!res) {
		/* POLISH syntax */
		if (!strcasecmp(chan->language, "pl")) { 
			if (vms->curmsg && (vms->curmsg != vms->lastmsg)) {
				int ten, one;
				char nextmsg[256];
				ten = (vms->curmsg + 1) / 10;
				one = (vms->curmsg + 1) % 10;
				
				if (vms->curmsg < 20) {
					snprintf(nextmsg, sizeof(nextmsg), "digits/n-%d", vms->curmsg + 1);
					res = wait_file2(chan, vms, nextmsg);
				} else {
					snprintf(nextmsg, sizeof(nextmsg), "digits/n-%d", ten * 10);
					res = wait_file2(chan, vms, nextmsg);
					if (one > 0) {
						if (!res) {
							snprintf(nextmsg, sizeof(nextmsg), "digits/n-%d", one);
							res = wait_file2(chan, vms, nextmsg);
						}
					}
				}
			}
			if (!res)
				res = wait_file2(chan, vms, "vm-message");
		} else {
			if (!strcasecmp(chan->language, "se")) /* SWEDISH syntax */
				res = wait_file2(chan, vms, "vm-meddelandet");  /* "message" */
			else /* DEFAULT syntax */
				res = wait_file2(chan, vms, "vm-message");
			if (vms->curmsg && (vms->curmsg != vms->lastmsg)) {
				if (!res)
					res = ast_say_number(chan, vms->curmsg + 1, AST_DIGIT_ANY, chan->language, NULL);
			}
		}
	}

	/* Retrieve info from VM attribute file */
	make_file(vms->fn2, sizeof(vms->fn2), vms->curdir, vms->curmsg);
	snprintf(filename, sizeof(filename), "%s.txt", vms->fn2);
	RETRIEVE(vms->curdir, vms->curmsg);
	msg_cfg = ast_config_load(filename);
	if (!msg_cfg) {
		ast_log(LOG_WARNING, "No message attribute file?!! (%s)\n", filename);
		return 0;
	}

	if (!(origtime = ast_variable_retrieve(msg_cfg, "message", "origtime"))) {
		ast_log(LOG_WARNING, "No origtime?!\n");
		DISPOSE(vms->curdir, vms->curmsg);
		ast_config_destroy(msg_cfg);
		return 0;
	}

	cid = ast_strdupa(ast_variable_retrieve(msg_cfg, "message", "callerid"));
	duration = ast_variable_retrieve(msg_cfg, "message", "duration");
	category = ast_variable_retrieve(msg_cfg, "message", "category");

	context = ast_variable_retrieve(msg_cfg, "message", "context");
	if (!strncasecmp("macro",context,5)) /* Macro names in contexts are useless for our needs */
		context = ast_variable_retrieve(msg_cfg, "message","macrocontext");
	if (!res)
		res = play_message_category(chan, category);
	if ((!res) && (ast_test_flag(vmu, VM_ENVELOPE)))
		res = play_message_datetime(chan, vmu, origtime, filename);
	if ((!res) && (ast_test_flag(vmu, VM_SAYCID)))
		res = play_message_callerid(chan, vms, cid, context, 0);
	if ((!res) && (ast_test_flag(vmu, VM_SAYDURATION)))
		res = play_message_duration(chan, vms, duration, vmu->saydurationm);
	/* Allow pressing '1' to skip envelope / callerid */
	if (res == '1')
		res = 0;
	ast_config_destroy(msg_cfg);

	if (!res) {
		make_file(vms->fn, sizeof(vms->fn), vms->curdir, vms->curmsg);
		vms->heard[vms->curmsg] = 1;
		res = wait_file(chan, vms, vms->fn);
	}
	DISPOSE(vms->curdir, vms->curmsg);
	return res;
}
#endif

#ifdef IMAP_STORAGE
static void imap_mailbox_name(char *spec, struct vm_state *vms, int box, int use_folder)
{
	char tmp[256], *t = tmp;
	size_t left = sizeof(tmp);

	if (box == 1) {
		ast_copy_string(vms->curbox, mbox(0), sizeof(vms->curbox));
		sprintf(vms->vmbox, "vm-%s", mbox(1));
	} else {
		ast_copy_string(vms->curbox, mbox(box), sizeof(vms->curbox));
		snprintf(vms->vmbox, sizeof(vms->vmbox), "vm-%s", vms->curbox);
	}

	/* Build up server information */
	ast_build_string(&t, &left, "{%s:%s/imap", imapserver, imapport);

	/* Add authentication user if present */
	if (!ast_strlen_zero(authuser))
		ast_build_string(&t, &left, "/%s", authuser);

	/* Add flags if present */
	if (!ast_strlen_zero(imapflags))
		ast_build_string(&t, &left, "/%s", imapflags);

	/* End with username */
	ast_build_string(&t, &left, "/user=%s}", vms->imapuser);

	if(box == 0 || box == 1)
		sprintf(spec, "%s%s", tmp, use_folder? imapfolder: "INBOX");
	else
		sprintf(spec, "%s%s%c%s", tmp, imapfolder, delimiter, mbox(box));
}

static int init_mailstream(struct vm_state *vms, int box)
{
	MAILSTREAM *stream = NIL;
	long debug;
	char tmp[255];
	
	if (!vms) {
		ast_log (LOG_ERROR,"vm_state is NULL!\n");
		return -1;
	}
	if(option_debug > 2)
		ast_log (LOG_DEBUG,"vm_state user is:%s\n",vms->imapuser);
	if (vms->mailstream == NIL || !vms->mailstream) {
		if (option_debug)
			ast_log (LOG_DEBUG,"mailstream not set.\n");
	} else {
		stream = vms->mailstream;
	}
	/* debug = T;  user wants protocol telemetry? */
	debug = NIL;  /* NO protocol telemetry? */

	if (delimiter == '\0') {		/* did not probe the server yet */
		char *cp;
#include "linkage.c"
		/* Connect to INBOX first to get folders delimiter */
		imap_mailbox_name(tmp, vms, 0, 0);
		stream = mail_open(stream, tmp, debug ? OP_DEBUG : NIL);
		if (stream == NIL) {
			ast_log (LOG_ERROR, "Can't connect to imap server %s\n", tmp);
			return NIL;
		}
		get_mailbox_delimiter(stream);
		/* update delimiter in imapfolder */
		for(cp = imapfolder; *cp; cp++) {
			if(*cp == '/')
				*cp = delimiter;
		}
	}
	/* Now connect to the target folder */
	imap_mailbox_name(tmp, vms, box, 1);
	if(option_debug > 2)
		ast_log (LOG_DEBUG,"Before mail_open, server: %s, box:%d\n", tmp, box);
	vms->mailstream = mail_open(stream, tmp, debug ? OP_DEBUG : NIL);
	if (vms->mailstream == NIL) {
		return -1;
	} else {
		return 0;
	}
}

static int open_mailbox(struct vm_state *vms, struct ast_vm_user *vmu, int box)
{
	SEARCHPGM *pgm;
	SEARCHHEADER *hdr;
	int ret;
	char dbox[256];

	ast_copy_string(vms->imapuser,vmu->imapuser, sizeof(vms->imapuser));

	if(option_debug > 2)
		ast_log(LOG_DEBUG,"Before init_mailstream, user is %s\n",vmu->imapuser);

	if ((ret = init_mailstream(vms, box)) || !vms->mailstream) {
		ast_log (LOG_ERROR,"Could not initialize mailstream\n");
		return -1;
	}
	
	/* Check Quota (here for now to test) */
	mail_parameters(NULL, SET_QUOTA, (void *) mm_parsequota);
	imap_mailbox_name(dbox, vms, box, 1);
	imap_getquotaroot(vms->mailstream, dbox);

	pgm = mail_newsearchpgm();

	/* Check IMAP folder for Asterisk messages only... */
	hdr = mail_newsearchheader("X-Asterisk-VM-Extension", vmu->mailbox);
	pgm->header = hdr;
	pgm->deleted = 0;
	pgm->undeleted = 1;

	/* if box = 0, check for new, if box = 1, check for read */
	if (box == 0) {
		pgm->unseen = 1;
		pgm->seen = 0;
	} else if (box == 1) {
		pgm->seen = 1;
		pgm->unseen = 0;
	}

	if(option_debug > 2)
		ast_log(LOG_DEBUG,"Before mail_search_full, user is %s\n",vmu->imapuser);

	vms->vmArrayIndex = 0;
	mail_search_full (vms->mailstream, NULL, pgm, NIL);
	vms->lastmsg = vms->vmArrayIndex - 1;
	mail_free_searchpgm(&pgm);

	return 0;
}
#else
static int open_mailbox(struct vm_state *vms, struct ast_vm_user *vmu,int box)
{
	int res = 0;
	int count_msg, last_msg;

	ast_copy_string(vms->curbox, mbox(box), sizeof(vms->curbox));
	
	/* Rename the member vmbox HERE so that we don't try to return before
	 * we know what's going on.
	 */
	snprintf(vms->vmbox, sizeof(vms->vmbox), "vm-%s", vms->curbox);
	
	/* Faster to make the directory than to check if it exists. */
	create_dirpath(vms->curdir, sizeof(vms->curdir), vmu->context, vms->username, vms->curbox);

	count_msg = count_messages(vmu, vms->curdir);
	if (count_msg < 0)
		return count_msg;
	else
		vms->lastmsg = count_msg - 1;

	/*
	The following test is needed in case sequencing gets messed up.
	There appears to be more than one way to mess up sequence, so
	we will not try to find all of the root causes--just fix it when
	detected.
	*/

	if (vm_lock_path(vms->curdir)) {
		ast_log(LOG_ERROR, "Could not open mailbox %s:  mailbox is locked\n", vms->curdir);
		return -1;
	}

	last_msg = last_message_index(vmu, vms->curdir);
	ast_unlock_path(vms->curdir);

	if (last_msg < 0) 
		return last_msg;
	else if (vms->lastmsg != last_msg)
	{
		ast_log(LOG_NOTICE, "Resequencing Mailbox: %s\n", vms->curdir);
		res = resequence_mailbox(vmu, vms->curdir);
		if (res)
			return res;
	}

	return 0;
}
#endif

static int close_mailbox(struct vm_state *vms, struct ast_vm_user *vmu)
{
	int x = 0;
#ifndef IMAP_STORAGE
	int res = 0, nummsg;
#endif

	if (vms->lastmsg <= -1)
		goto done;

	vms->curmsg = -1; 
#ifndef IMAP_STORAGE
	/* Get the deleted messages fixed */ 
	if (vm_lock_path(vms->curdir))
		return ERROR_LOCK_PATH;
	 
	for (x = 0; x < vmu->maxmsg; x++) { 
		if (!vms->deleted[x] && (strcasecmp(vms->curbox, "INBOX") || !vms->heard[x])) { 
			/* Save this message.  It's not in INBOX or hasn't been heard */ 
			make_file(vms->fn, sizeof(vms->fn), vms->curdir, x); 
			if (!EXISTS(vms->curdir, x, vms->fn, NULL)) 
				break;
			vms->curmsg++; 
			make_file(vms->fn2, sizeof(vms->fn2), vms->curdir, vms->curmsg); 
			if (strcmp(vms->fn, vms->fn2)) { 
				RENAME(vms->curdir, x, vmu->mailbox,vmu->context, vms->curdir, vms->curmsg, vms->fn, vms->fn2);
			} 
		} else if (!strcasecmp(vms->curbox, "INBOX") && vms->heard[x] && !vms->deleted[x]) { 
			/* Move to old folder before deleting */ 
			res = save_to_folder(vmu, vms, x, 1);
			if (res == ERROR_LOCK_PATH) {
				/* If save failed do not delete the message */
				vms->deleted[x] = 0;
				vms->heard[x] = 0;
				--x;
			} 
		} 
	} 

	/* Delete ALL remaining messages */
	nummsg = x - 1;
	for (x = vms->curmsg + 1; x <= nummsg; x++) {
		make_file(vms->fn, sizeof(vms->fn), vms->curdir, x);
		if (EXISTS(vms->curdir, x, vms->fn, NULL))
			DELETE(vms->curdir, x, vms->fn);
	}
	ast_unlock_path(vms->curdir);
#else
	for (x=0;x < vmu->maxmsg;x++) { 
		if (vms->deleted[x]) { 
			if(option_debug > 2)
				ast_log(LOG_DEBUG,"IMAP delete of %d\n",x);
			IMAP_DELETE(vms->curdir, x, vms->fn, vms);
		}
	}
#endif

done:
	if (vms->deleted)
		memset(vms->deleted, 0, vmu->maxmsg * sizeof(int)); 
	if (vms->heard)
		memset(vms->heard, 0, vmu->maxmsg * sizeof(int)); 

	return 0;
}

/* In Greek even though we CAN use a syntax like "friends messages"
 * ("filika mynhmata") it is not elegant. This also goes for "work/family messages"
 * ("ergasiaka/oikogeniaka mynhmata"). Therefore it is better to use a reversed 
 * syntax for the above three categories which is more elegant. 
 */

static int vm_play_folder_name_gr(struct ast_channel *chan, char *mbox)
{
	int cmd;
	char *buf;

	buf = alloca(strlen(mbox)+2); 
	strcpy(buf, mbox);
	strcat(buf,"s");

	if (!strcasecmp(mbox, "vm-INBOX") || !strcasecmp(mbox, "vm-Old")){
		cmd = ast_play_and_wait(chan, buf); /* "NEA / PALIA" */
		return cmd ? cmd : ast_play_and_wait(chan, "vm-messages"); /* "messages" -> "MYNHMATA" */
	} else {
		cmd = ast_play_and_wait(chan, "vm-messages"); /* "messages" -> "MYNHMATA" */
		return cmd ? cmd : ast_play_and_wait(chan, mbox); /* friends/family/work... -> "FILWN"/"OIKOGENIAS"/"DOULEIAS"*/
	}
}

static int vm_play_folder_name_pl(struct ast_channel *chan, char *mbox)
{
	int cmd;

	if (!strcasecmp(mbox, "vm-INBOX") || !strcasecmp(mbox, "vm-Old")) {
		if (!strcasecmp(mbox, "vm-INBOX"))
			cmd = ast_play_and_wait(chan, "vm-new-e");
		else
			cmd = ast_play_and_wait(chan, "vm-old-e");
		return cmd ? cmd : ast_play_and_wait(chan, "vm-messages");
	} else {
		cmd = ast_play_and_wait(chan, "vm-messages");
		return cmd ? cmd : ast_play_and_wait(chan, mbox);
	}
}

static int vm_play_folder_name(struct ast_channel *chan, char *mbox)
{
	int cmd;

	if (!strcasecmp(chan->language, "it") || !strcasecmp(chan->language, "es") || !strcasecmp(chan->language, "fr") || !strcasecmp(chan->language, "pt") || !strcasecmp(chan->language, "pt_BR")) { /* Italian, Spanish, French or Portuguese syntax */
		cmd = ast_play_and_wait(chan, "vm-messages"); /* "messages */
		return cmd ? cmd : ast_play_and_wait(chan, mbox);
	} else if (!strcasecmp(chan->language, "gr")){
		return vm_play_folder_name_gr(chan, mbox);
	} else if (!strcasecmp(chan->language, "pl")){
		return vm_play_folder_name_pl(chan, mbox);
	} else {  /* Default English */
		cmd = ast_play_and_wait(chan, mbox);
		return cmd ? cmd : ast_play_and_wait(chan, "vm-messages"); /* "messages */
	}
}

/* GREEK SYNTAX 
	In greek the plural for old/new is
	different so we need the following files
	We also need vm-denExeteMynhmata because 
	this syntax is different.
	
	-> vm-Olds.wav	: "Palia"
	-> vm-INBOXs.wav : "Nea"
	-> vm-denExeteMynhmata : "den exete mynhmata"
*/
					
	
static int vm_intro_gr(struct ast_channel *chan, struct vm_state *vms)
{
	int res = 0;

	if (vms->newmessages) {
		res = ast_play_and_wait(chan, "vm-youhave");
		if (!res) 
			res = ast_say_number(chan, vms->newmessages, AST_DIGIT_ANY, chan->language, NULL);
		if (!res) {
			if ((vms->newmessages == 1)) {
				res = ast_play_and_wait(chan, "vm-INBOX");
				if (!res)
					res = ast_play_and_wait(chan, "vm-message");
			} else {
				res = ast_play_and_wait(chan, "vm-INBOXs");
				if (!res)
					res = ast_play_and_wait(chan, "vm-messages");
			}
		}
	} else if (vms->oldmessages){
		res = ast_play_and_wait(chan, "vm-youhave");
		if (!res)
			res = ast_say_number(chan, vms->oldmessages, AST_DIGIT_ANY, chan->language, NULL);
		if ((vms->oldmessages == 1)){
			res = ast_play_and_wait(chan, "vm-Old");
			if (!res)
				res = ast_play_and_wait(chan, "vm-message");
		} else {
			res = ast_play_and_wait(chan, "vm-Olds");
			if (!res)
				res = ast_play_and_wait(chan, "vm-messages");
		}
	} else if (!vms->oldmessages && !vms->newmessages) 
		res = ast_play_and_wait(chan, "vm-denExeteMynhmata"); 
	return res;
}
	
/* Default English syntax */
static int vm_intro_en(struct ast_channel *chan, struct vm_state *vms)
{
	int res;

	/* Introduce messages they have */
	res = ast_play_and_wait(chan, "vm-youhave");
	if (!res) {
		if (vms->newmessages) {
			res = say_and_wait(chan, vms->newmessages, chan->language);
			if (!res)
				res = ast_play_and_wait(chan, "vm-INBOX");
			if (vms->oldmessages && !res)
				res = ast_play_and_wait(chan, "vm-and");
			else if (!res) {
				if ((vms->newmessages == 1))
					res = ast_play_and_wait(chan, "vm-message");
				else
					res = ast_play_and_wait(chan, "vm-messages");
			}
				
		}
		if (!res && vms->oldmessages) {
			res = say_and_wait(chan, vms->oldmessages, chan->language);
			if (!res)
				res = ast_play_and_wait(chan, "vm-Old");
			if (!res) {
				if (vms->oldmessages == 1)
					res = ast_play_and_wait(chan, "vm-message");
				else
					res = ast_play_and_wait(chan, "vm-messages");
			}
		}
		if (!res) {
			if (!vms->oldmessages && !vms->newmessages) {
				res = ast_play_and_wait(chan, "vm-no");
				if (!res)
					res = ast_play_and_wait(chan, "vm-messages");
			}
		}
	}
	return res;
}

/* ITALIAN syntax */
static int vm_intro_it(struct ast_channel *chan, struct vm_state *vms)
{
	/* Introduce messages they have */
	int res;
	if (!vms->oldmessages && !vms->newmessages)
		res =	ast_play_and_wait(chan, "vm-no") ||
			ast_play_and_wait(chan, "vm-message");
	else
		res =	ast_play_and_wait(chan, "vm-youhave");
	if (!res && vms->newmessages) {
		res = (vms->newmessages == 1) ?
			ast_play_and_wait(chan, "digits/un") ||
			ast_play_and_wait(chan, "vm-nuovo") ||
			ast_play_and_wait(chan, "vm-message") :
			/* 2 or more new messages */
			say_and_wait(chan, vms->newmessages, chan->language) ||
			ast_play_and_wait(chan, "vm-nuovi") ||
			ast_play_and_wait(chan, "vm-messages");
		if (!res && vms->oldmessages)
			res =	ast_play_and_wait(chan, "vm-and");
	}
	if (!res && vms->oldmessages) {
		res = (vms->oldmessages == 1) ?
			ast_play_and_wait(chan, "digits/un") ||
			ast_play_and_wait(chan, "vm-vecchio") ||
			ast_play_and_wait(chan, "vm-message") :
			/* 2 or more old messages */
			say_and_wait(chan, vms->oldmessages, chan->language) ||
			ast_play_and_wait(chan, "vm-vecchi") ||
			ast_play_and_wait(chan, "vm-messages");
	}
	return res ? -1 : 0;
}

/* POLISH syntax */
static int vm_intro_pl(struct ast_channel *chan, struct vm_state *vms)
{
	/* Introduce messages they have */
	int res;
	div_t num;

	if (!vms->oldmessages && !vms->newmessages) {
		res = ast_play_and_wait(chan, "vm-no");
		res = res ? res : ast_play_and_wait(chan, "vm-messages");
		return res;
	} else {
		res = ast_play_and_wait(chan, "vm-youhave");
	}

	if (vms->newmessages) {
		num = div(vms->newmessages, 10);
		if (vms->newmessages == 1) {
			res = ast_play_and_wait(chan, "digits/1-a");
			res = res ? res : ast_play_and_wait(chan, "vm-new-a");
			res = res ? res : ast_play_and_wait(chan, "vm-message");
		} else if (num.rem > 1 && num.rem < 5 && num.quot != 1) {
			if (num.rem == 2) {
				if (!num.quot) {
					res = ast_play_and_wait(chan, "digits/2-ie");
				} else {
					res = say_and_wait(chan, vms->newmessages - 2 , chan->language);
					res = res ? res : ast_play_and_wait(chan, "digits/2-ie");
				}
			} else {
				res = say_and_wait(chan, vms->newmessages, chan->language);
			}
			res = res ? res : ast_play_and_wait(chan, "vm-new-e");
			res = res ? res : ast_play_and_wait(chan, "vm-messages");
		} else {
			res = say_and_wait(chan, vms->newmessages, chan->language);
			res = res ? res : ast_play_and_wait(chan, "vm-new-ych");
			res = res ? res : ast_play_and_wait(chan, "vm-messages");
		}
		if (!res && vms->oldmessages)
			res = ast_play_and_wait(chan, "vm-and");
	}
	if (!res && vms->oldmessages) {
		num = div(vms->oldmessages, 10);
		if (vms->oldmessages == 1) {
			res = ast_play_and_wait(chan, "digits/1-a");
			res = res ? res : ast_play_and_wait(chan, "vm-old-a");
			res = res ? res : ast_play_and_wait(chan, "vm-message");
		} else if (num.rem > 1 && num.rem < 5 && num.quot != 1) {
			if (num.rem == 2) {
				if (!num.quot) {
					res = ast_play_and_wait(chan, "digits/2-ie");
				} else {
					res = say_and_wait(chan, vms->oldmessages - 2 , chan->language);
					res = res ? res : ast_play_and_wait(chan, "digits/2-ie");
				}
			} else {
				res = say_and_wait(chan, vms->oldmessages, chan->language);
			}
			res = res ? res : ast_play_and_wait(chan, "vm-old-e");
			res = res ? res : ast_play_and_wait(chan, "vm-messages");
		} else {
			res = say_and_wait(chan, vms->oldmessages, chan->language);
			res = res ? res : ast_play_and_wait(chan, "vm-old-ych");
			res = res ? res : ast_play_and_wait(chan, "vm-messages");
		}
	}

	return res;
}

/* SWEDISH syntax */
static int vm_intro_se(struct ast_channel *chan, struct vm_state *vms)
{
	/* Introduce messages they have */
	int res;

	res = ast_play_and_wait(chan, "vm-youhave");
	if (res)
		return res;

	if (!vms->oldmessages && !vms->newmessages) {
		res = ast_play_and_wait(chan, "vm-no");
		res = res ? res : ast_play_and_wait(chan, "vm-messages");
		return res;
	}

	if (vms->newmessages) {
		if ((vms->newmessages == 1)) {
			res = ast_play_and_wait(chan, "digits/ett");
			res = res ? res : ast_play_and_wait(chan, "vm-nytt");
			res = res ? res : ast_play_and_wait(chan, "vm-message");
		} else {
			res = say_and_wait(chan, vms->newmessages, chan->language);
			res = res ? res : ast_play_and_wait(chan, "vm-nya");
			res = res ? res : ast_play_and_wait(chan, "vm-messages");
		}
		if (!res && vms->oldmessages)
			res = ast_play_and_wait(chan, "vm-and");
	}
	if (!res && vms->oldmessages) {
		if (vms->oldmessages == 1) {
			res = ast_play_and_wait(chan, "digits/ett");
			res = res ? res : ast_play_and_wait(chan, "vm-gammalt");
			res = res ? res : ast_play_and_wait(chan, "vm-message");
		} else {
			res = say_and_wait(chan, vms->oldmessages, chan->language);
			res = res ? res : ast_play_and_wait(chan, "vm-gamla");
			res = res ? res : ast_play_and_wait(chan, "vm-messages");
		}
	}

	return res;
}

/* NORWEGIAN syntax */
static int vm_intro_no(struct ast_channel *chan,struct vm_state *vms)
{
	/* Introduce messages they have */
	int res;

	res = ast_play_and_wait(chan, "vm-youhave");
	if (res)
		return res;

	if (!vms->oldmessages && !vms->newmessages) {
		res = ast_play_and_wait(chan, "vm-no");
		res = res ? res : ast_play_and_wait(chan, "vm-messages");
		return res;
	}

	if (vms->newmessages) {
		if ((vms->newmessages == 1)) {
			res = ast_play_and_wait(chan, "digits/1");
			res = res ? res : ast_play_and_wait(chan, "vm-ny");
			res = res ? res : ast_play_and_wait(chan, "vm-message");
		} else {
			res = say_and_wait(chan, vms->newmessages, chan->language);
			res = res ? res : ast_play_and_wait(chan, "vm-nye");
			res = res ? res : ast_play_and_wait(chan, "vm-messages");
		}
		if (!res && vms->oldmessages)
			res = ast_play_and_wait(chan, "vm-and");
	}
	if (!res && vms->oldmessages) {
		if (vms->oldmessages == 1) {
			res = ast_play_and_wait(chan, "digits/1");
			res = res ? res : ast_play_and_wait(chan, "vm-gamel");
			res = res ? res : ast_play_and_wait(chan, "vm-message");
		} else {
			res = say_and_wait(chan, vms->oldmessages, chan->language);
			res = res ? res : ast_play_and_wait(chan, "vm-gamle");
			res = res ? res : ast_play_and_wait(chan, "vm-messages");
		}
	}

	return res;
}

/* GERMAN syntax */
static int vm_intro_de(struct ast_channel *chan,struct vm_state *vms)
{
	/* Introduce messages they have */
	int res;
	res = ast_play_and_wait(chan, "vm-youhave");
	if (!res) {
		if (vms->newmessages) {
			if ((vms->newmessages == 1))
				res = ast_play_and_wait(chan, "digits/1F");
			else
				res = say_and_wait(chan, vms->newmessages, chan->language);
			if (!res)
				res = ast_play_and_wait(chan, "vm-INBOX");
			if (vms->oldmessages && !res)
				res = ast_play_and_wait(chan, "vm-and");
			else if (!res) {
				if ((vms->newmessages == 1))
					res = ast_play_and_wait(chan, "vm-message");
				else
					res = ast_play_and_wait(chan, "vm-messages");
			}
				
		}
		if (!res && vms->oldmessages) {
			if (vms->oldmessages == 1)
				res = ast_play_and_wait(chan, "digits/1F");
			else
				res = say_and_wait(chan, vms->oldmessages, chan->language);
			if (!res)
				res = ast_play_and_wait(chan, "vm-Old");
			if (!res) {
				if (vms->oldmessages == 1)
					res = ast_play_and_wait(chan, "vm-message");
				else
					res = ast_play_and_wait(chan, "vm-messages");
			}
		}
		if (!res) {
			if (!vms->oldmessages && !vms->newmessages) {
				res = ast_play_and_wait(chan, "vm-no");
				if (!res)
					res = ast_play_and_wait(chan, "vm-messages");
			}
		}
	}
	return res;
}

/* SPANISH syntax */
static int vm_intro_es(struct ast_channel *chan,struct vm_state *vms)
{
	/* Introduce messages they have */
	int res;
	if (!vms->oldmessages && !vms->newmessages) {
		res = ast_play_and_wait(chan, "vm-youhaveno");
		if (!res)
			res = ast_play_and_wait(chan, "vm-messages");
	} else {
		res = ast_play_and_wait(chan, "vm-youhave");
	}
	if (!res) {
		if (vms->newmessages) {
			if (!res) {
				if ((vms->newmessages == 1)) {
					res = ast_play_and_wait(chan, "digits/1M");
					if (!res)
						res = ast_play_and_wait(chan, "vm-message");
					if (!res)
						res = ast_play_and_wait(chan, "vm-INBOXs");
				} else {
					res = say_and_wait(chan, vms->newmessages, chan->language);
					if (!res)
						res = ast_play_and_wait(chan, "vm-messages");
					if (!res)
						res = ast_play_and_wait(chan, "vm-INBOX");
				}
			}
			if (vms->oldmessages && !res)
				res = ast_play_and_wait(chan, "vm-and");
		}
		if (vms->oldmessages) {
			if (!res) {
				if (vms->oldmessages == 1) {
					res = ast_play_and_wait(chan, "digits/1M");
					if (!res)
						res = ast_play_and_wait(chan, "vm-message");
					if (!res)
						res = ast_play_and_wait(chan, "vm-Olds");
				} else {
					res = say_and_wait(chan, vms->oldmessages, chan->language);
					if (!res)
						res = ast_play_and_wait(chan, "vm-messages");
					if (!res)
						res = ast_play_and_wait(chan, "vm-Old");
				}
			}
		}
	}
return res;
}

/* BRAZILIAN PORTUGUESE syntax */
static int vm_intro_pt_BR(struct ast_channel *chan,struct vm_state *vms) {
	/* Introduce messages they have */
	int res;
	if (!vms->oldmessages && !vms->newmessages) {
		res = ast_play_and_wait(chan, "vm-nomessages");
		return res;
	}
	else {
		res = ast_play_and_wait(chan, "vm-youhave");
	}
	if (vms->newmessages) {
		if (!res)
			res = ast_say_number(chan, vms->newmessages, AST_DIGIT_ANY, chan->language, "f");
		if ((vms->newmessages == 1)) {
			if (!res)
				res = ast_play_and_wait(chan, "vm-message");
			if (!res)
				res = ast_play_and_wait(chan, "vm-INBOXs");
		}
		else {
			if (!res)
				res = ast_play_and_wait(chan, "vm-messages");
			if (!res)
				res = ast_play_and_wait(chan, "vm-INBOX");
		}
		if (vms->oldmessages && !res)
			res = ast_play_and_wait(chan, "vm-and");
	}
	if (vms->oldmessages) {
		if (!res)
			res = ast_say_number(chan, vms->oldmessages, AST_DIGIT_ANY, chan->language, "f");
		if (vms->oldmessages == 1) {
			if (!res)
				res = ast_play_and_wait(chan, "vm-message");
			if (!res)
				res = ast_play_and_wait(chan, "vm-Olds");
		}
		else {
			if (!res)
		res = ast_play_and_wait(chan, "vm-messages");
			if (!res)
				res = ast_play_and_wait(chan, "vm-Old");
		}
	}
	return res;
}

/* FRENCH syntax */
static int vm_intro_fr(struct ast_channel *chan,struct vm_state *vms)
{
	/* Introduce messages they have */
	int res;
	res = ast_play_and_wait(chan, "vm-youhave");
	if (!res) {
		if (vms->newmessages) {
			res = say_and_wait(chan, vms->newmessages, chan->language);
			if (!res)
				res = ast_play_and_wait(chan, "vm-INBOX");
			if (vms->oldmessages && !res)
				res = ast_play_and_wait(chan, "vm-and");
			else if (!res) {
				if ((vms->newmessages == 1))
					res = ast_play_and_wait(chan, "vm-message");
				else
					res = ast_play_and_wait(chan, "vm-messages");
			}
				
		}
		if (!res && vms->oldmessages) {
			res = say_and_wait(chan, vms->oldmessages, chan->language);
			if (!res) {
				if (vms->oldmessages == 1)
					res = ast_play_and_wait(chan, "vm-message");
				else
					res = ast_play_and_wait(chan, "vm-messages");
			}
			if (!res)
				res = ast_play_and_wait(chan, "vm-Old");
		}
		if (!res) {
			if (!vms->oldmessages && !vms->newmessages) {
				res = ast_play_and_wait(chan, "vm-no");
				if (!res)
					res = ast_play_and_wait(chan, "vm-messages");
			}
		}
	}
	return res;
}

/* DUTCH syntax */
static int vm_intro_nl(struct ast_channel *chan,struct vm_state *vms)
{
	/* Introduce messages they have */
	int res;
	res = ast_play_and_wait(chan, "vm-youhave");
	if (!res) {
		if (vms->newmessages) {
			res = say_and_wait(chan, vms->newmessages, chan->language);
			if (!res) {
				if (vms->newmessages == 1)
					res = ast_play_and_wait(chan, "vm-INBOXs");
				else
					res = ast_play_and_wait(chan, "vm-INBOX");
			}
			if (vms->oldmessages && !res)
				res = ast_play_and_wait(chan, "vm-and");
			else if (!res) {
				if ((vms->newmessages == 1))
					res = ast_play_and_wait(chan, "vm-message");
				else
					res = ast_play_and_wait(chan, "vm-messages");
			}
				
		}
		if (!res && vms->oldmessages) {
			res = say_and_wait(chan, vms->oldmessages, chan->language);
			if (!res) {
				if (vms->oldmessages == 1)
					res = ast_play_and_wait(chan, "vm-Olds");
				else
					res = ast_play_and_wait(chan, "vm-Old");
			}
			if (!res) {
				if (vms->oldmessages == 1)
					res = ast_play_and_wait(chan, "vm-message");
				else
					res = ast_play_and_wait(chan, "vm-messages");
			}
		}
		if (!res) {
			if (!vms->oldmessages && !vms->newmessages) {
				res = ast_play_and_wait(chan, "vm-no");
				if (!res)
					res = ast_play_and_wait(chan, "vm-messages");
			}
		}
	}
	return res;
}

/* PORTUGUESE syntax */
static int vm_intro_pt(struct ast_channel *chan,struct vm_state *vms)
{
	/* Introduce messages they have */
	int res;
	res = ast_play_and_wait(chan, "vm-youhave");
	if (!res) {
		if (vms->newmessages) {
			res = ast_say_number(chan, vms->newmessages, AST_DIGIT_ANY, chan->language, "f");
			if (!res) {
				if ((vms->newmessages == 1)) {
					res = ast_play_and_wait(chan, "vm-message");
					if (!res)
						res = ast_play_and_wait(chan, "vm-INBOXs");
				} else {
					res = ast_play_and_wait(chan, "vm-messages");
					if (!res)
						res = ast_play_and_wait(chan, "vm-INBOX");
				}
			}
			if (vms->oldmessages && !res)
				res = ast_play_and_wait(chan, "vm-and");
		}
		if (!res && vms->oldmessages) {
			res = ast_say_number(chan, vms->oldmessages, AST_DIGIT_ANY, chan->language, "f");
			if (!res) {
				if (vms->oldmessages == 1) {
					res = ast_play_and_wait(chan, "vm-message");
					if (!res)
						res = ast_play_and_wait(chan, "vm-Olds");
				} else {
					res = ast_play_and_wait(chan, "vm-messages");
					if (!res)
						res = ast_play_and_wait(chan, "vm-Old");
				}
			}
		}
		if (!res) {
			if (!vms->oldmessages && !vms->newmessages) {
				res = ast_play_and_wait(chan, "vm-no");
				if (!res)
					res = ast_play_and_wait(chan, "vm-messages");
			}
		}
	}
	return res;
}


/* CZECH syntax */
/* in czech there must be declension of word new and message
 * czech        : english        : czech      : english
 * --------------------------------------------------------
 * vm-youhave   : you have 
 * vm-novou     : one new        : vm-zpravu  : message
 * vm-nove      : 2-4 new        : vm-zpravy  : messages
 * vm-novych    : 5-infinite new : vm-zprav   : messages
 * vm-starou	: one old
 * vm-stare     : 2-4 old 
 * vm-starych   : 5-infinite old
 * jednu        : one	- falling 4. 
 * vm-no        : no  ( no messages )
 */

static int vm_intro_cz(struct ast_channel *chan,struct vm_state *vms)
{
	int res;
	res = ast_play_and_wait(chan, "vm-youhave");
	if (!res) {
		if (vms->newmessages) {
			if (vms->newmessages == 1) {
				res = ast_play_and_wait(chan, "digits/jednu");
			} else {
				res = say_and_wait(chan, vms->newmessages, chan->language);
			}
			if (!res) {
				if ((vms->newmessages == 1))
					res = ast_play_and_wait(chan, "vm-novou");
				if ((vms->newmessages) > 1 && (vms->newmessages < 5))
					res = ast_play_and_wait(chan, "vm-nove");
				if (vms->newmessages > 4)
					res = ast_play_and_wait(chan, "vm-novych");
			}
			if (vms->oldmessages && !res)
				res = ast_play_and_wait(chan, "vm-and");
			else if (!res) {
				if ((vms->newmessages == 1))
					res = ast_play_and_wait(chan, "vm-zpravu");
				if ((vms->newmessages) > 1 && (vms->newmessages < 5))
					res = ast_play_and_wait(chan, "vm-zpravy");
				if (vms->newmessages > 4)
					res = ast_play_and_wait(chan, "vm-zprav");
			}
		}
		if (!res && vms->oldmessages) {
			res = say_and_wait(chan, vms->oldmessages, chan->language);
			if (!res) {
				if ((vms->oldmessages == 1))
					res = ast_play_and_wait(chan, "vm-starou");
				if ((vms->oldmessages) > 1 && (vms->oldmessages < 5))
					res = ast_play_and_wait(chan, "vm-stare");
				if (vms->oldmessages > 4)
					res = ast_play_and_wait(chan, "vm-starych");
			}
			if (!res) {
				if ((vms->oldmessages == 1))
					res = ast_play_and_wait(chan, "vm-zpravu");
				if ((vms->oldmessages) > 1 && (vms->oldmessages < 5))
					res = ast_play_and_wait(chan, "vm-zpravy");
				if (vms->oldmessages > 4)
					res = ast_play_and_wait(chan, "vm-zprav");
			}
		}
		if (!res) {
			if (!vms->oldmessages && !vms->newmessages) {
				res = ast_play_and_wait(chan, "vm-no");
				if (!res)
					res = ast_play_and_wait(chan, "vm-zpravy");
			}
		}
	}
	return res;
}

static int get_lastdigits(int num)
{
	num %= 100;
	return (num < 20) ? num : num % 10;
}

static int vm_intro_ru(struct ast_channel *chan,struct vm_state *vms)
{
	int res;
	int lastnum = 0;
	int dcnum;

	res = ast_play_and_wait(chan, "vm-youhave");
	if (!res && vms->newmessages) {
		lastnum = get_lastdigits(vms->newmessages);
		dcnum = vms->newmessages - lastnum;
		if (dcnum)
			res = say_and_wait(chan, dcnum, chan->language);
		if (!res && lastnum) {
			if (lastnum == 1) 
				res = ast_play_and_wait(chan, "digits/ru/odno");
			else
				res = say_and_wait(chan, lastnum, chan->language);
		}

		if (!res)
			res = ast_play_and_wait(chan, (lastnum == 1) ? "vm-novoe" : "vm-novyh");

		if (!res && vms->oldmessages)
			res = ast_play_and_wait(chan, "vm-and");
	}

	if (!res && vms->oldmessages) {
		lastnum = get_lastdigits(vms->oldmessages);
		dcnum = vms->newmessages - lastnum;
		if (dcnum)
			res = say_and_wait(chan, dcnum, chan->language);
		if (!res && lastnum) {
			if (lastnum == 1) 
				res = ast_play_and_wait(chan, "digits/ru/odno");
			else
				res = say_and_wait(chan, lastnum, chan->language);
		}

		if (!res)
			res = ast_play_and_wait(chan, (lastnum == 1) ? "vm-staroe" : "vm-staryh");
	}

	if (!res && !vms->newmessages && !vms->oldmessages) {
		lastnum = 0;
		res = ast_play_and_wait(chan, "vm-no");
	}

	if (!res) {
		switch (lastnum) {
		case 1:
			res = ast_play_and_wait(chan, "vm-soobshenie");
			break;
		case 2:
		case 3:
		case 4:
			res = ast_play_and_wait(chan, "vm-soobsheniya");
			break;
		default:
			res = ast_play_and_wait(chan, "vm-soobsheniy");
			break;
		}
	}

	return res;
}


static int vm_intro(struct ast_channel *chan, struct ast_vm_user *vmu, struct vm_state *vms)
{
	char prefile[256];
	
	/* Notify the user that the temp greeting is set and give them the option to remove it */
	snprintf(prefile, sizeof(prefile), "%s%s/%s/temp", VM_SPOOL_DIR, vmu->context, vms->username);
	if (ast_test_flag(vmu, VM_TEMPGREETWARN)) {
		if (ast_fileexists(prefile, NULL, NULL) > 0)
			ast_play_and_wait(chan, "vm-tempgreetactive");
	}

	/* Play voicemail intro - syntax is different for different languages */
	if (!strcasecmp(chan->language, "de")) {	/* GERMAN syntax */
		return vm_intro_de(chan, vms);
	} else if (!strcasecmp(chan->language, "es")) { /* SPANISH syntax */
		return vm_intro_es(chan, vms);
	} else if (!strcasecmp(chan->language, "it")) { /* ITALIAN syntax */
		return vm_intro_it(chan, vms);
	} else if (!strcasecmp(chan->language, "fr")) {	/* FRENCH syntax */
		return vm_intro_fr(chan, vms);
	} else if (!strcasecmp(chan->language, "nl")) {	/* DUTCH syntax */
		return vm_intro_nl(chan, vms);
	} else if (!strcasecmp(chan->language, "pt")) {	/* PORTUGUESE syntax */
		return vm_intro_pt(chan, vms);
	} else if (!strcasecmp(chan->language, "pt_BR")) {	/* BRAZILIAN PORTUGUESE syntax */
		return vm_intro_pt_BR(chan, vms);
	} else if (!strcasecmp(chan->language, "cz")) {	/* CZECH syntax */
		return vm_intro_cz(chan, vms);
	} else if (!strcasecmp(chan->language, "gr")) {	/* GREEK syntax */
		return vm_intro_gr(chan, vms);
	} else if (!strcasecmp(chan->language, "pl")) {	/* POLISH syntax */
		return vm_intro_pl(chan, vms);
	} else if (!strcasecmp(chan->language, "se")) {	/* SWEDISH syntax */
		return vm_intro_se(chan, vms);
	} else if (!strcasecmp(chan->language, "no")) {	/* NORWEGIAN syntax */
		return vm_intro_no(chan, vms);
	} else if (!strcasecmp(chan->language, "ru")) { /* RUSSIAN syntax */
		return vm_intro_ru(chan, vms);
	} else {					/* Default to ENGLISH */
		return vm_intro_en(chan, vms);
	}
}

static int vm_instructions(struct ast_channel *chan, struct vm_state *vms, int skipadvanced)
{
	int res = 0;
	/* Play instructions and wait for new command */
	while (!res) {
		if (vms->starting) {
			if (vms->lastmsg > -1) {
				res = ast_play_and_wait(chan, "vm-onefor");
				if (!res)
					res = vm_play_folder_name(chan, vms->vmbox);
			}
			if (!res)
				res = ast_play_and_wait(chan, "vm-opts");
		} else {
			if (vms->curmsg)
				res = ast_play_and_wait(chan, "vm-prev");
			if (!res && !skipadvanced)
				res = ast_play_and_wait(chan, "vm-advopts");
			if (!res)
				res = ast_play_and_wait(chan, "vm-repeat");
			if (!res && (vms->curmsg != vms->lastmsg))
				res = ast_play_and_wait(chan, "vm-next");
			if (!res) {
				if (!vms->deleted[vms->curmsg])
					res = ast_play_and_wait(chan, "vm-delete");
				else
					res = ast_play_and_wait(chan, "vm-undelete");
				if (!res)
					res = ast_play_and_wait(chan, "vm-toforward");
				if (!res)
					res = ast_play_and_wait(chan, "vm-savemessage");
			}
		}
		if (!res)
			res = ast_play_and_wait(chan, "vm-helpexit");
		if (!res)
			res = ast_waitfordigit(chan, 6000);
		if (!res) {
			vms->repeats++;
			if (vms->repeats > 2) {
				res = 't';
			}
		}
	}
	return res;
}

static int vm_newuser(struct ast_channel *chan, struct ast_vm_user *vmu, struct vm_state *vms, char *fmtc, signed char record_gain)
{
	int cmd = 0;
	int duration = 0;
	int tries = 0;
	char newpassword[80] = "";
	char newpassword2[80] = "";
	char prefile[PATH_MAX] = "";
	unsigned char buf[256];
	int bytes=0;

	if (ast_adsi_available(chan)) {
		bytes += adsi_logo(buf + bytes);
		bytes += ast_adsi_display(buf + bytes, ADSI_COMM_PAGE, 3, ADSI_JUST_CENT, 0, "New User Setup", "");
		bytes += ast_adsi_display(buf + bytes, ADSI_COMM_PAGE, 4, ADSI_JUST_CENT, 0, "Not Done", "");
		bytes += ast_adsi_set_line(buf + bytes, ADSI_COMM_PAGE, 1);
		bytes += ast_adsi_voice_mode(buf + bytes, 0);
		ast_adsi_transmit_message(chan, buf, bytes, ADSI_MSG_DISPLAY);
	}

	/* First, have the user change their password 
	   so they won't get here again */
	for (;;) {
		newpassword[1] = '\0';
		newpassword[0] = cmd = ast_play_and_wait(chan, vm_newpassword);
		if (cmd == '#')
			newpassword[0] = '\0';
		if (cmd < 0 || cmd == 't' || cmd == '#')
			return cmd;
		cmd = ast_readstring(chan,newpassword + strlen(newpassword),sizeof(newpassword)-1,2000,10000,"#");
		if (cmd < 0 || cmd == 't' || cmd == '#')
			return cmd;
		newpassword2[1] = '\0';
		newpassword2[0] = cmd = ast_play_and_wait(chan, vm_reenterpassword);
		if (cmd == '#')
			newpassword2[0] = '\0';
		if (cmd < 0 || cmd == 't' || cmd == '#')
			return cmd;
		cmd = ast_readstring(chan,newpassword2 + strlen(newpassword2),sizeof(newpassword2)-1,2000,10000,"#");
		if (cmd < 0 || cmd == 't' || cmd == '#')
			return cmd;
		if (!strcmp(newpassword, newpassword2))
			break;
		ast_log(LOG_NOTICE,"Password mismatch for user %s (%s != %s)\n", vms->username, newpassword, newpassword2);
		cmd = ast_play_and_wait(chan, vm_mismatch);
		if (++tries == 3)
			return -1;
	}
	if (pwdchange & PWDCHANGE_INTERNAL)
		vm_change_password(vmu, newpassword);
	if ((pwdchange & PWDCHANGE_EXTERNAL) && !ast_strlen_zero(ext_pass_cmd))
		vm_change_password_shell(vmu, newpassword);

	if (option_debug)
		ast_log(LOG_DEBUG,"User %s set password to %s of length %d\n",vms->username,newpassword,(int)strlen(newpassword));
	cmd = ast_play_and_wait(chan, vm_passchanged);

	/* If forcename is set, have the user record their name */	
	if (ast_test_flag(vmu, VM_FORCENAME)) {
		snprintf(prefile,sizeof(prefile), "%s%s/%s/greet", VM_SPOOL_DIR, vmu->context, vms->username);
		if (ast_fileexists(prefile, NULL, NULL) < 1) {
			cmd = play_record_review(chan, "vm-rec-name", prefile, maxgreet, fmtc, 0, vmu, &duration, NULL, record_gain, NULL);
			if (cmd < 0 || cmd == 't' || cmd == '#')
				return cmd;
		}
	}

	/* If forcegreetings is set, have the user record their greetings */
	if (ast_test_flag(vmu, VM_FORCEGREET)) {
		snprintf(prefile,sizeof(prefile), "%s%s/%s/unavail", VM_SPOOL_DIR, vmu->context, vms->username);
		if (ast_fileexists(prefile, NULL, NULL) < 1) {
			cmd = play_record_review(chan, "vm-rec-unv", prefile, maxgreet, fmtc, 0, vmu, &duration, NULL, record_gain, NULL);
			if (cmd < 0 || cmd == 't' || cmd == '#')
				return cmd;
		}

		snprintf(prefile,sizeof(prefile), "%s%s/%s/busy", VM_SPOOL_DIR, vmu->context, vms->username);
		if (ast_fileexists(prefile, NULL, NULL) < 1) {
			cmd = play_record_review(chan, "vm-rec-busy", prefile, maxgreet, fmtc, 0, vmu, &duration, NULL, record_gain, NULL);
			if (cmd < 0 || cmd == 't' || cmd == '#')
				return cmd;
		}
	}

	return cmd;
}

static int vm_options(struct ast_channel *chan, struct ast_vm_user *vmu, struct vm_state *vms, char *fmtc, signed char record_gain)
{
	int cmd = 0;
	int retries = 0;
	int duration = 0;
	char newpassword[80] = "";
	char newpassword2[80] = "";
	char prefile[PATH_MAX] = "";
	unsigned char buf[256];
	int bytes=0;

	if (ast_adsi_available(chan))
	{
		bytes += adsi_logo(buf + bytes);
		bytes += ast_adsi_display(buf + bytes, ADSI_COMM_PAGE, 3, ADSI_JUST_CENT, 0, "Options Menu", "");
		bytes += ast_adsi_display(buf + bytes, ADSI_COMM_PAGE, 4, ADSI_JUST_CENT, 0, "Not Done", "");
		bytes += ast_adsi_set_line(buf + bytes, ADSI_COMM_PAGE, 1);
		bytes += ast_adsi_voice_mode(buf + bytes, 0);
		ast_adsi_transmit_message(chan, buf, bytes, ADSI_MSG_DISPLAY);
	}
	while ((cmd >= 0) && (cmd != 't')) {
		if (cmd)
			retries = 0;
		switch (cmd) {
		case '1':
			snprintf(prefile,sizeof(prefile), "%s%s/%s/unavail", VM_SPOOL_DIR, vmu->context, vms->username);
			cmd = play_record_review(chan,"vm-rec-unv",prefile, maxgreet, fmtc, 0, vmu, &duration, NULL, record_gain, NULL);
			break;
		case '2': 
			snprintf(prefile,sizeof(prefile), "%s%s/%s/busy", VM_SPOOL_DIR, vmu->context, vms->username);
			cmd = play_record_review(chan,"vm-rec-busy",prefile, maxgreet, fmtc, 0, vmu, &duration, NULL, record_gain, NULL);
			break;
		case '3': 
			snprintf(prefile,sizeof(prefile), "%s%s/%s/greet", VM_SPOOL_DIR, vmu->context, vms->username);
			cmd = play_record_review(chan,"vm-rec-name",prefile, maxgreet, fmtc, 0, vmu, &duration, NULL, record_gain, NULL);
			break;
		case '4': 
			cmd = vm_tempgreeting(chan, vmu, vms, fmtc, record_gain);
			break;
		case '5':
			if (vmu->password[0] == '-') {
				cmd = ast_play_and_wait(chan, "vm-no");
				break;
			}
			newpassword[1] = '\0';
			newpassword[0] = cmd = ast_play_and_wait(chan, vm_newpassword);
			if (cmd == '#')
				newpassword[0] = '\0';
			else {
				if (cmd < 0)
					break;
				if ((cmd = ast_readstring(chan,newpassword + strlen(newpassword),sizeof(newpassword)-1,2000,10000,"#")) < 0) {
					break;
				}
			}
			newpassword2[1] = '\0';
			newpassword2[0] = cmd = ast_play_and_wait(chan, vm_reenterpassword);
			if (cmd == '#')
				newpassword2[0] = '\0';
			else {
				if (cmd < 0)
					break;

				if ((cmd = ast_readstring(chan,newpassword2 + strlen(newpassword2),sizeof(newpassword2)-1,2000,10000,"#"))) {
					break;
				}
			}
			if (strcmp(newpassword, newpassword2)) {
				ast_log(LOG_NOTICE,"Password mismatch for user %s (%s != %s)\n", vms->username, newpassword, newpassword2);
				cmd = ast_play_and_wait(chan, vm_mismatch);
				break;
			}
			if (pwdchange & PWDCHANGE_INTERNAL)
				vm_change_password(vmu, newpassword);
			if ((pwdchange & PWDCHANGE_EXTERNAL) && !ast_strlen_zero(ext_pass_cmd))
				vm_change_password_shell(vmu, newpassword);

			if (option_debug)
				ast_log(LOG_DEBUG,"User %s set password to %s of length %d\n",vms->username,newpassword,(int)strlen(newpassword));
			cmd = ast_play_and_wait(chan, vm_passchanged);
			break;
		case '*': 
			cmd = 't';
			break;
		default: 
			cmd = ast_play_and_wait(chan,"vm-options");
			if (!cmd)
				cmd = ast_waitfordigit(chan,6000);
			if (!cmd)
				retries++;
			if (retries > 3)
				cmd = 't';
		}
	}
	if (cmd == 't')
		cmd = 0;
	return cmd;
}

static int vm_tempgreeting(struct ast_channel *chan, struct ast_vm_user *vmu, struct vm_state *vms, char *fmtc, signed char record_gain)
{
	int res;
	int cmd = 0;
	int retries = 0;
	int duration = 0;
	char prefile[PATH_MAX] = "";
	unsigned char buf[256];
	char dest[PATH_MAX];
	int bytes = 0;

	if (ast_adsi_available(chan)) {
		bytes += adsi_logo(buf + bytes);
		bytes += ast_adsi_display(buf + bytes, ADSI_COMM_PAGE, 3, ADSI_JUST_CENT, 0, "Temp Greeting Menu", "");
		bytes += ast_adsi_display(buf + bytes, ADSI_COMM_PAGE, 4, ADSI_JUST_CENT, 0, "Not Done", "");
		bytes += ast_adsi_set_line(buf + bytes, ADSI_COMM_PAGE, 1);
		bytes += ast_adsi_voice_mode(buf + bytes, 0);
		ast_adsi_transmit_message(chan, buf, bytes, ADSI_MSG_DISPLAY);
	}

	snprintf(prefile, sizeof(prefile), "%s%s/%s/temp", VM_SPOOL_DIR, vmu->context, vms->username);
	if ((res = create_dirpath(dest, sizeof(dest), vmu->context, vms->username, "temp"))) {
		ast_log(LOG_WARNING, "Failed to create directory (%s).\n", prefile);
		return -1;
	}
	while((cmd >= 0) && (cmd != 't')) {
		if (cmd)
			retries = 0;
		RETRIEVE(prefile, -1);
		if (ast_fileexists(prefile, NULL, NULL) <= 0) {
			play_record_review(chan, "vm-rec-temp", prefile, maxgreet, fmtc, 0, vmu, &duration, NULL, record_gain, NULL);
			cmd = 't';	
		} else {
			switch (cmd) {
			case '1':
				cmd = play_record_review(chan, "vm-rec-temp", prefile, maxgreet, fmtc, 0, vmu, &duration, NULL, record_gain, NULL);
				break;
			case '2':
				DELETE(prefile, -1, prefile);
				ast_play_and_wait(chan, "vm-tempremoved");
				cmd = 't';	
				break;
			case '*': 
				cmd = 't';
				break;
			default:
				cmd = ast_play_and_wait(chan,
					ast_fileexists(prefile, NULL, NULL) > 0 ? /* XXX always true ? */
						"vm-tempgreeting2" : "vm-tempgreeting");
				if (!cmd)
					cmd = ast_waitfordigit(chan,6000);
				if (!cmd)
					retries++;
				if (retries > 3)
					cmd = 't';
			}
		}
		DISPOSE(prefile, -1);
	}
	if (cmd == 't')
		cmd = 0;
	return cmd;
}

/* GREEK SYNTAX */
	
static int vm_browse_messages_gr(struct ast_channel *chan, struct vm_state *vms, struct ast_vm_user *vmu)
{
	int cmd=0;

	if (vms->lastmsg > -1) {
		cmd = play_message(chan, vmu, vms);
	} else {
		cmd = ast_play_and_wait(chan, "vm-youhaveno");
		if (!strcasecmp(vms->vmbox, "vm-INBOX") ||!strcasecmp(vms->vmbox, "vm-Old")){
			if (!cmd) {
				snprintf(vms->fn, sizeof(vms->fn), "vm-%ss", vms->curbox);
				cmd = ast_play_and_wait(chan, vms->fn);
			}
			if (!cmd)
				cmd = ast_play_and_wait(chan, "vm-messages");
		} else {
			if (!cmd)
				cmd = ast_play_and_wait(chan, "vm-messages");
			if (!cmd) {
				snprintf(vms->fn, sizeof(vms->fn), "vm-%s", vms->curbox);
				cmd = ast_play_and_wait(chan, vms->fn);
			}
		}
	} 
	return cmd;
}

/* Default English syntax */
static int vm_browse_messages_en(struct ast_channel *chan, struct vm_state *vms, struct ast_vm_user *vmu)
{
	int cmd=0;

	if (vms->lastmsg > -1) {
		cmd = play_message(chan, vmu, vms);
	} else {
		cmd = ast_play_and_wait(chan, "vm-youhave");
		if (!cmd) 
			cmd = ast_play_and_wait(chan, "vm-no");
		if (!cmd) {
			snprintf(vms->fn, sizeof(vms->fn), "vm-%s", vms->curbox);
			cmd = ast_play_and_wait(chan, vms->fn);
		}
		if (!cmd)
			cmd = ast_play_and_wait(chan, "vm-messages");
	}
	return cmd;
}

/* ITALIAN syntax */
static int vm_browse_messages_it(struct ast_channel *chan, struct vm_state *vms, struct ast_vm_user *vmu)
{
	int cmd=0;

	if (vms->lastmsg > -1) {
		cmd = play_message(chan, vmu, vms);
	} else {
		cmd = ast_play_and_wait(chan, "vm-no");
		if (!cmd)
			cmd = ast_play_and_wait(chan, "vm-message");
		if (!cmd) {
			snprintf(vms->fn, sizeof(vms->fn), "vm-%s", vms->curbox);
			cmd = ast_play_and_wait(chan, vms->fn);
		}
	}
	return cmd;
}

/* SPANISH syntax */
static int vm_browse_messages_es(struct ast_channel *chan, struct vm_state *vms, struct ast_vm_user *vmu)
{
	int cmd=0;

	if (vms->lastmsg > -1) {
		cmd = play_message(chan, vmu, vms);
	} else {
		cmd = ast_play_and_wait(chan, "vm-youhaveno");
		if (!cmd)
			cmd = ast_play_and_wait(chan, "vm-messages");
		if (!cmd) {
			snprintf(vms->fn, sizeof(vms->fn), "vm-%s", vms->curbox);
			cmd = ast_play_and_wait(chan, vms->fn);
		}
	}
	return cmd;
}

/* PORTUGUESE syntax */
static int vm_browse_messages_pt(struct ast_channel *chan, struct vm_state *vms, struct ast_vm_user *vmu)
{
	int cmd=0;

	if (vms->lastmsg > -1) {
		cmd = play_message(chan, vmu, vms);
	} else {
		cmd = ast_play_and_wait(chan, "vm-no");
		if (!cmd) {
			snprintf(vms->fn, sizeof(vms->fn), "vm-%s", vms->curbox);
			cmd = ast_play_and_wait(chan, vms->fn);
		}
		if (!cmd)
			cmd = ast_play_and_wait(chan, "vm-messages");
	}
	return cmd;
}

static int vm_browse_messages(struct ast_channel *chan, struct vm_state *vms, struct ast_vm_user *vmu)
{
	if (!strcasecmp(chan->language, "es")) {	/* SPANISH */
		return vm_browse_messages_es(chan, vms, vmu);
	} else if (!strcasecmp(chan->language, "it")) { /* ITALIAN */
		return vm_browse_messages_it(chan, vms, vmu);
	} else if (!strcasecmp(chan->language, "pt") || !strcasecmp(chan->language, "pt_BR")) {	/* PORTUGUESE */
		return vm_browse_messages_pt(chan, vms, vmu);
	} else if (!strcasecmp(chan->language, "gr")){
		return vm_browse_messages_gr(chan, vms, vmu);   /* GREEK */
	} else {	/* Default to English syntax */
		return vm_browse_messages_en(chan, vms, vmu);
	}
}

static int vm_authenticate(struct ast_channel *chan, char *mailbox, int mailbox_size,
			struct ast_vm_user *res_vmu, const char *context, const char *prefix,
			int skipuser, int maxlogins, int silent)
{
	int useadsi=0, valid=0, logretries=0;
	char password[AST_MAX_EXTENSION]="", *passptr;
	struct ast_vm_user vmus, *vmu = NULL;

	/* If ADSI is supported, setup login screen */
	adsi_begin(chan, &useadsi);
	if (!skipuser && useadsi)
		adsi_login(chan);
	if (!silent && !skipuser && ast_streamfile(chan, "vm-login", chan->language)) {
		ast_log(LOG_WARNING, "Couldn't stream login file\n");
		return -1;
	}
	
	/* Authenticate them and get their mailbox/password */
	
	while (!valid && (logretries < maxlogins)) {
		/* Prompt for, and read in the username */
		if (!skipuser && ast_readstring(chan, mailbox, mailbox_size - 1, 2000, 10000, "#") < 0) {
			ast_log(LOG_WARNING, "Couldn't read username\n");
			return -1;
		}
		if (ast_strlen_zero(mailbox)) {
			if (chan->cid.cid_num) {
				ast_copy_string(mailbox, chan->cid.cid_num, mailbox_size);
			} else {
				if (option_verbose > 2)
					ast_verbose(VERBOSE_PREFIX_3 "Username not entered\n");	
				return -1;
			}
		}
		if (useadsi)
			adsi_password(chan);

		if (!ast_strlen_zero(prefix)) {
			char fullusername[80] = "";
			ast_copy_string(fullusername, prefix, sizeof(fullusername));
			strncat(fullusername, mailbox, sizeof(fullusername) - 1 - strlen(fullusername));
			ast_copy_string(mailbox, fullusername, mailbox_size);
		}

		if (option_debug)
			ast_log(LOG_DEBUG, "Before find user for mailbox %s\n",mailbox);
		vmu = find_user(&vmus, context, mailbox);
		if (vmu && (vmu->password[0] == '\0' || (vmu->password[0] == '-' && vmu->password[1] == '\0'))) {
			/* saved password is blank, so don't bother asking */
			password[0] = '\0';
		} else {
			if (ast_streamfile(chan, vm_password, chan->language)) {
				ast_log(LOG_WARNING, "Unable to stream password file\n");
				return -1;
			}
			if (ast_readstring(chan, password, sizeof(password) - 1, 2000, 10000, "#") < 0) {
				ast_log(LOG_WARNING, "Unable to read password\n");
				return -1;
			}
		}

		if (vmu) {
			passptr = vmu->password;
			if (passptr[0] == '-') passptr++;
		}
		if (vmu && !strcmp(passptr, password))
			valid++;
		else {
			if (option_verbose > 2)
				ast_verbose( VERBOSE_PREFIX_3 "Incorrect password '%s' for user '%s' (context = %s)\n", password, mailbox, context ? context : "default");
			if (!ast_strlen_zero(prefix))
				mailbox[0] = '\0';
		}
		logretries++;
		if (!valid) {
			if (skipuser || logretries >= maxlogins) {
				if (ast_streamfile(chan, "vm-incorrect", chan->language)) {
					ast_log(LOG_WARNING, "Unable to stream incorrect message\n");
					return -1;
				}
			} else {
				if (useadsi)
					adsi_login(chan);
				if (ast_streamfile(chan, "vm-incorrect-mailbox", chan->language)) {
					ast_log(LOG_WARNING, "Unable to stream incorrect mailbox message\n");
					return -1;
				}
			}
			if (ast_waitstream(chan, ""))	/* Channel is hung up */
				return -1;
		}
	}
	if (!valid && (logretries >= maxlogins)) {
		ast_stopstream(chan);
		ast_play_and_wait(chan, "vm-goodbye");
		return -1;
	}
	if (vmu && !skipuser) {
		memcpy(res_vmu, vmu, sizeof(struct ast_vm_user));
	}
	return 0;
}

static int vm_execmain(struct ast_channel *chan, void *data)
{
	/* XXX This is, admittedly, some pretty horrendous code.  For some
	   reason it just seemed a lot easier to do with GOTO's.  I feel
	   like I'm back in my GWBASIC days. XXX */
	int res=-1;
	int cmd=0;
	int valid = 0;
	struct ast_module_user *u;
	char prefixstr[80] ="";
	char ext_context[256]="";
	int box;
	int useadsi = 0;
	int skipuser = 0;
	struct vm_state vms;
	struct ast_vm_user *vmu = NULL, vmus;
	char *context=NULL;
	int silentexit = 0;
	struct ast_flags flags = { 0 };
	signed char record_gain = 0;
	int play_auto = 0;
	int play_folder = 0;
#ifdef IMAP_STORAGE
	int deleted = 0;
#endif
	u = ast_module_user_add(chan);

	/* Add the vm_state to the active list and keep it active */
	memset(&vms, 0, sizeof(vms));
	vms.lastmsg = -1;

	memset(&vmus, 0, sizeof(vmus));

	if (chan->_state != AST_STATE_UP) {
		if (option_debug)
			ast_log(LOG_DEBUG, "Before ast_answer\n");
		ast_answer(chan);
	}

	if (!ast_strlen_zero(data)) {
		char *opts[OPT_ARG_ARRAY_SIZE];
		char *parse;
		AST_DECLARE_APP_ARGS(args,
			AST_APP_ARG(argv0);
			AST_APP_ARG(argv1);
		);

		parse = ast_strdupa(data);

		AST_STANDARD_APP_ARGS(args, parse);

		if (args.argc == 2) {
			if (ast_app_parse_options(vm_app_options, &flags, opts, args.argv1)) {
				ast_module_user_remove(u);
				return -1;
			}
			if (ast_test_flag(&flags, OPT_RECORDGAIN)) {
				int gain;
				if (opts[OPT_ARG_RECORDGAIN]) {
					if (sscanf(opts[OPT_ARG_RECORDGAIN], "%d", &gain) != 1) {
						ast_log(LOG_WARNING, "Invalid value '%s' provided for record gain option\n", opts[OPT_ARG_RECORDGAIN]);
						ast_module_user_remove(u);
						return -1;
					} else {
						record_gain = (signed char) gain;
					}
				} else {
					ast_log(LOG_WARNING, "Invalid Gain level set with option g\n");
				}
			}
			if (ast_test_flag(&flags, OPT_AUTOPLAY) ) {
				play_auto = 1;
				if (opts[OPT_ARG_PLAYFOLDER]) {
					if (sscanf(opts[OPT_ARG_PLAYFOLDER], "%d", &play_folder) != 1) {
						ast_log(LOG_WARNING, "Invalid value '%s' provided for folder autoplay option\n", opts[OPT_ARG_PLAYFOLDER]);
					}
				} else {
					ast_log(LOG_WARNING, "Invalid folder set with option a\n");
				}	
				if ( play_folder > 9 || play_folder < 0) {
					ast_log(LOG_WARNING, "Invalid value '%d' provided for folder autoplay option\n", play_folder);
					play_folder = 0;
				}
			}
		} else {
			/* old style options parsing */
			while (*(args.argv0)) {
				if (*(args.argv0) == 's')
					ast_set_flag(&flags, OPT_SILENT);
				else if (*(args.argv0) == 'p')
					ast_set_flag(&flags, OPT_PREPEND_MAILBOX);
				else 
					break;
				(args.argv0)++;
			}

		}

		valid = ast_test_flag(&flags, OPT_SILENT);

		if ((context = strchr(args.argv0, '@')))
			*context++ = '\0';

		if (ast_test_flag(&flags, OPT_PREPEND_MAILBOX))
			ast_copy_string(prefixstr, args.argv0, sizeof(prefixstr));
		else
			ast_copy_string(vms.username, args.argv0, sizeof(vms.username));

		if (!ast_strlen_zero(vms.username) && (vmu = find_user(&vmus, context ,vms.username)))
			skipuser++;
		else
			valid = 0;
	}

	if (!valid)
		res = vm_authenticate(chan, vms.username, sizeof(vms.username), &vmus, context, prefixstr, skipuser, maxlogins, 0);

	if (option_debug)
		ast_log(LOG_DEBUG, "After vm_authenticate\n");
	if (!res) {
		valid = 1;
		if (!skipuser)
			vmu = &vmus;
	} else {
		res = 0;
	}

	/* If ADSI is supported, setup login screen */
	adsi_begin(chan, &useadsi);

#ifdef IMAP_STORAGE
	vms.interactive = 1;
	vms.updated = 2;
	vmstate_insert(&vms);
	init_vm_state(&vms);
#endif
	if (!valid)
		goto out;

	if (!(vms.deleted = ast_calloc(vmu->maxmsg, sizeof(int)))) {
		/* TODO: Handle memory allocation failure */
	}
	if (!(vms.heard = ast_calloc(vmu->maxmsg, sizeof(int)))) {
		/* TODO: Handle memory allocation failure */
	}
	
	/* Set language from config to override channel language */
	if (!ast_strlen_zero(vmu->language))
		ast_string_field_set(chan, language, vmu->language);
#ifndef IMAP_STORAGE
	create_dirpath(vms.curdir, sizeof(vms.curdir), vmu->context, vms.username, "");
#endif
	/* Retrieve old and new message counts */
	if (option_debug)
		ast_log(LOG_DEBUG, "Before open_mailbox\n");
	res = open_mailbox(&vms, vmu, 1);
	if (res == ERROR_LOCK_PATH)
		goto out;
	vms.oldmessages = vms.lastmsg + 1;
	if (option_debug)
		ast_log(LOG_DEBUG, "Number of old messages: %d\n",vms.oldmessages);
	/* Start in INBOX */
	res = open_mailbox(&vms, vmu, 0);
	if (res == ERROR_LOCK_PATH)
		goto out;
	vms.newmessages = vms.lastmsg + 1;
	if (option_debug)
		ast_log(LOG_DEBUG, "Number of new messages: %d\n",vms.newmessages);
		
	/* Select proper mailbox FIRST!! */
	if (play_auto) {
		res = open_mailbox(&vms, vmu, play_folder);
		if (res == ERROR_LOCK_PATH)
			goto out;

		/* If there are no new messages, inform the user and hangup */
		if (vms.lastmsg == -1) {
			cmd = vm_browse_messages(chan, &vms, vmu);
			res = 0;
			goto out;
		}
	} else {
		if (!vms.newmessages && vms.oldmessages) {
			/* If we only have old messages start here */
			res = open_mailbox(&vms, vmu, 1);
			if (res == ERROR_LOCK_PATH)
				goto out;
		}
	}

	if (useadsi)
		adsi_status(chan, &vms);
	res = 0;

	/* Check to see if this is a new user */
	if (!strcasecmp(vmu->mailbox, vmu->password) && 
		(ast_test_flag(vmu, VM_FORCENAME | VM_FORCEGREET))) {
		if (ast_play_and_wait(chan, "vm-newuser") == -1)
			ast_log(LOG_WARNING, "Couldn't stream new user file\n");
		cmd = vm_newuser(chan, vmu, &vms, vmfmts, record_gain);
		if ((cmd == 't') || (cmd == '#')) {
			/* Timeout */
			res = 0;
			goto out;
		} else if (cmd < 0) {
			/* Hangup */
			res = -1;
			goto out;
		}
	}
#ifdef IMAP_STORAGE
		if(option_debug > 2)
			ast_log(LOG_DEBUG, "Checking quotas: comparing %u to %u\n",vms.quota_usage,vms.quota_limit);
		if (vms.quota_limit && vms.quota_usage >= vms.quota_limit) {
			if (option_debug)
				ast_log(LOG_DEBUG, "*** QUOTA EXCEEDED!!\n");
			cmd = ast_play_and_wait(chan, "vm-mailboxfull");
		}
#endif
	if (play_auto) {
		cmd = '1';
	} else {
		cmd = vm_intro(chan, vmu, &vms);
	}

	vms.repeats = 0;
	vms.starting = 1;
	while ((cmd > -1) && (cmd != 't') && (cmd != '#')) {
		/* Run main menu */
		switch (cmd) {
		case '1':
			vms.curmsg = 0;
			/* Fall through */
		case '5':
			cmd = vm_browse_messages(chan, &vms, vmu);
			break;
		case '2': /* Change folders */
			if (useadsi)
				adsi_folders(chan, 0, "Change to folder...");
			cmd = get_folder2(chan, "vm-changeto", 0);
			if (cmd == '#') {
				cmd = 0;
			} else if (cmd > 0) {
				cmd = cmd - '0';
				res = close_mailbox(&vms, vmu);
				if (res == ERROR_LOCK_PATH)
					goto out;
				res = open_mailbox(&vms, vmu, cmd);
				if (res == ERROR_LOCK_PATH)
					goto out;
				cmd = 0;
			}
			if (useadsi)
				adsi_status2(chan, &vms);
				
			if (!cmd)
				cmd = vm_play_folder_name(chan, vms.vmbox);

			vms.starting = 1;
			break;
		case '3': /* Advanced options */
			cmd = 0;
			vms.repeats = 0;
			while ((cmd > -1) && (cmd != 't') && (cmd != '#')) {
				switch (cmd) {
				case '1': /* Reply */
					if (vms.lastmsg > -1 && !vms.starting) {
						cmd = advanced_options(chan, vmu, &vms, vms.curmsg, 1, record_gain);
						if (cmd == ERROR_LOCK_PATH) {
							res = cmd;
							goto out;
						}
					} else
						cmd = ast_play_and_wait(chan, "vm-sorry");
					cmd = 't';
					break;
				case '2': /* Callback */
					if (option_verbose > 2 && !vms.starting)
						ast_verbose( VERBOSE_PREFIX_3 "Callback Requested\n");
					if (!ast_strlen_zero(vmu->callback) && vms.lastmsg > -1 && !vms.starting) {
						cmd = advanced_options(chan, vmu, &vms, vms.curmsg, 2, record_gain);
						if (cmd == 9) {
							silentexit = 1;
							goto out;
						} else if (cmd == ERROR_LOCK_PATH) {
							res = cmd;
							goto out;
						}
					}
					else 
						cmd = ast_play_and_wait(chan, "vm-sorry");
					cmd = 't';
					break;
				case '3': /* Envelope */
					if (vms.lastmsg > -1 && !vms.starting) {
						cmd = advanced_options(chan, vmu, &vms, vms.curmsg, 3, record_gain);
						if (cmd == ERROR_LOCK_PATH) {
							res = cmd;
							goto out;
						}
					} else
						cmd = ast_play_and_wait(chan, "vm-sorry");
					cmd = 't';
					break;
				case '4': /* Dialout */
					if (!ast_strlen_zero(vmu->dialout)) {
						cmd = dialout(chan, vmu, NULL, vmu->dialout);
						if (cmd == 9) {
							silentexit = 1;
							goto out;
						}
					}
					else 
						cmd = ast_play_and_wait(chan, "vm-sorry");
					cmd = 't';
					break;

				case '5': /* Leave VoiceMail */
					if (ast_test_flag(vmu, VM_SVMAIL)) {
						cmd = forward_message(chan, context, &vms, vmu, vmfmts, 1, record_gain);
						if (cmd == ERROR_LOCK_PATH) {
							res = cmd;
							ast_log(LOG_WARNING, "forward_message failed to lock path.\n");
							goto out;
						}
					} else
						cmd = ast_play_and_wait(chan,"vm-sorry");
					cmd='t';
					break;
					
				case '*': /* Return to main menu */
					cmd = 't';
					break;

				default:
					cmd = 0;
					if (!vms.starting) {
						cmd = ast_play_and_wait(chan, "vm-toreply");
					}
					if (!ast_strlen_zero(vmu->callback) && !vms.starting && !cmd) {
						cmd = ast_play_and_wait(chan, "vm-tocallback");
					}
					if (!cmd && !vms.starting) {
						cmd = ast_play_and_wait(chan, "vm-tohearenv");
					}
					if (!ast_strlen_zero(vmu->dialout) && !cmd) {
						cmd = ast_play_and_wait(chan, "vm-tomakecall");
					}
					if (ast_test_flag(vmu, VM_SVMAIL) && !cmd)
						cmd=ast_play_and_wait(chan, "vm-leavemsg");
					if (!cmd)
						cmd = ast_play_and_wait(chan, "vm-starmain");
					if (!cmd)
						cmd = ast_waitfordigit(chan,6000);
					if (!cmd)
						vms.repeats++;
					if (vms.repeats > 3)
						cmd = 't';
				}
			}
			if (cmd == 't') {
				cmd = 0;
				vms.repeats = 0;
			}
			break;
		case '4':
			if (vms.curmsg) {
				vms.curmsg--;
				cmd = play_message(chan, vmu, &vms);
			} else {
				cmd = ast_play_and_wait(chan, "vm-nomore");
			}
			break;
		case '6':
			if (vms.curmsg < vms.lastmsg) {
				vms.curmsg++;
				cmd = play_message(chan, vmu, &vms);
			} else {
				cmd = ast_play_and_wait(chan, "vm-nomore");
			}
			break;
		case '7':
			vms.deleted[vms.curmsg] = !vms.deleted[vms.curmsg];
			if (useadsi)
				adsi_delete(chan, &vms);
			if (vms.deleted[vms.curmsg]) 
				cmd = ast_play_and_wait(chan, "vm-deleted");
			else
				cmd = ast_play_and_wait(chan, "vm-undeleted");
			if (ast_test_flag((&globalflags), VM_SKIPAFTERCMD)) {
				if (vms.curmsg < vms.lastmsg) {
					vms.curmsg++;
					cmd = play_message(chan, vmu, &vms);
				} else {
					cmd = ast_play_and_wait(chan, "vm-nomore");
				}
			}
#ifdef IMAP_STORAGE
			deleted = 1;
#endif
			break;
	
		case '8':
			if (vms.lastmsg > -1) {
				cmd = forward_message(chan, context, &vms, vmu, vmfmts, 0, record_gain);
				if (cmd == ERROR_LOCK_PATH) {
					res = cmd;
					goto out;
				}
			} else
				cmd = ast_play_and_wait(chan, "vm-nomore");
			break;
		case '9':
			if (useadsi)
				adsi_folders(chan, 1, "Save to folder...");
			cmd = get_folder2(chan, "vm-savefolder", 1);
			box = 0;	/* Shut up compiler */
			if (cmd == '#') {
				cmd = 0;
				break;
			} else if (cmd > 0) {
				box = cmd = cmd - '0';
				cmd = save_to_folder(vmu, &vms, vms.curmsg, cmd);
				if (cmd == ERROR_LOCK_PATH) {
					res = cmd;
					goto out;
#ifdef IMAP_STORAGE
				} else if (cmd == 10) {
					goto out;
#endif
				} else if (!cmd) {
					vms.deleted[vms.curmsg] = 1;
				} else {
					vms.deleted[vms.curmsg] = 0;
					vms.heard[vms.curmsg] = 0;
				}
			}
			make_file(vms.fn, sizeof(vms.fn), vms.curdir, vms.curmsg);
			if (useadsi)
				adsi_message(chan, &vms);
			snprintf(vms.fn, sizeof(vms.fn), "vm-%s", mbox(box));
			if (!cmd) {
				cmd = ast_play_and_wait(chan, "vm-message");
				if (!cmd)
					cmd = say_and_wait(chan, vms.curmsg + 1, chan->language);
				if (!cmd)
					cmd = ast_play_and_wait(chan, "vm-savedto");
				if (!cmd)
					cmd = vm_play_folder_name(chan, vms.fn);
			} else {
				cmd = ast_play_and_wait(chan, "vm-mailboxfull");
			}
			if (ast_test_flag((&globalflags), VM_SKIPAFTERCMD)) {
				if (vms.curmsg < vms.lastmsg) {
					vms.curmsg++;
					cmd = play_message(chan, vmu, &vms);
				} else {
					cmd = ast_play_and_wait(chan, "vm-nomore");
				}
			}
			break;
		case '*':
			if (!vms.starting) {
				cmd = ast_play_and_wait(chan, "vm-onefor");
				if (!cmd)
					cmd = vm_play_folder_name(chan, vms.vmbox);
				if (!cmd)
					cmd = ast_play_and_wait(chan, "vm-opts");
				if (!cmd)
					cmd = vm_instructions(chan, &vms, 1);
			} else
				cmd = 0;
			break;
		case '0':
			cmd = vm_options(chan, vmu, &vms, vmfmts, record_gain);
			if (useadsi)
				adsi_status(chan, &vms);
			break;
		default:	/* Nothing */
			cmd = vm_instructions(chan, &vms, 0);
			break;
		}
	}
	if ((cmd == 't') || (cmd == '#')) {
		/* Timeout */
		res = 0;
	} else {
		/* Hangup */
		res = -1;
	}

out:
	if (res > -1) {
		ast_stopstream(chan);
		adsi_goodbye(chan);
		if (valid) {
			if (silentexit)
				res = ast_play_and_wait(chan, "vm-dialout");
			else 
				res = ast_play_and_wait(chan, "vm-goodbye");
			if (res > 0)
				res = 0;
		}
		if (useadsi)
			ast_adsi_unload_session(chan);
	}
	if (vmu)
		close_mailbox(&vms, vmu);
	if (valid) {
		snprintf(ext_context, sizeof(ext_context), "%s@%s", vms.username, vmu->context);
		manager_event(EVENT_FLAG_CALL, "MessageWaiting", "Mailbox: %s\r\nWaiting: %d\r\n", ext_context, has_voicemail(ext_context, NULL));
		run_externnotify(vmu->context, vmu->mailbox);
	}
#ifdef IMAP_STORAGE
	/* expunge message - use UID Expunge if supported on IMAP server*/
	if(option_debug > 2)
		ast_log(LOG_DEBUG, "*** Checking if we can expunge, deleted set to %d, expungeonhangup set to %d\n",deleted,expungeonhangup);
	if (vmu && deleted == 1 && expungeonhangup == 1) {
#ifdef HAVE_IMAP_TK2006
		if (LEVELUIDPLUS (vms.mailstream)) {
			mail_expunge_full(vms.mailstream,NIL,EX_UID);
		} else 
#endif
			mail_expunge(vms.mailstream);
	}
	/*  before we delete the state, we should copy pertinent info
	 *  back to the persistent model */
	vmstate_delete(&vms);
#endif
	if (vmu)
		free_user(vmu);
	if (vms.deleted)
		free(vms.deleted);
	if (vms.heard)
		free(vms.heard);
	ast_module_user_remove(u);

	return res;
}

static int vm_exec(struct ast_channel *chan, void *data)
{
	int res = 0;
	struct ast_module_user *u;
	char *tmp;
	struct leave_vm_options leave_options;
	struct ast_flags flags = { 0 };
	char *opts[OPT_ARG_ARRAY_SIZE];
	AST_DECLARE_APP_ARGS(args,
		AST_APP_ARG(argv0);
		AST_APP_ARG(argv1);
	);

	u = ast_module_user_add(chan);
	
	memset(&leave_options, 0, sizeof(leave_options));

	if (chan->_state != AST_STATE_UP)
		ast_answer(chan);

	if (!ast_strlen_zero(data)) {
		tmp = ast_strdupa(data);
		AST_STANDARD_APP_ARGS(args, tmp);
		if (args.argc == 2) {
			if (ast_app_parse_options(vm_app_options, &flags, opts, args.argv1)) {
				ast_module_user_remove(u);
				return -1;
			}
			ast_copy_flags(&leave_options, &flags, OPT_SILENT | OPT_BUSY_GREETING | OPT_UNAVAIL_GREETING | OPT_PRIORITY_JUMP);
			if (ast_test_flag(&flags, OPT_RECORDGAIN)) {
				int gain;

				if (sscanf(opts[OPT_ARG_RECORDGAIN], "%d", &gain) != 1) {
					ast_log(LOG_WARNING, "Invalid value '%s' provided for record gain option\n", opts[OPT_ARG_RECORDGAIN]);
					ast_module_user_remove(u);
					return -1;
				} else {
					leave_options.record_gain = (signed char) gain;
				}
			}
		}
	} else {
		char tmp[256];
		res = ast_app_getdata(chan, "vm-whichbox", tmp, sizeof(tmp) - 1, 0);
		if (res < 0) {
			ast_module_user_remove(u);
			return res;
		}
		if (ast_strlen_zero(tmp)) {
			ast_module_user_remove(u);
			return 0;
		}
		args.argv0 = ast_strdupa(tmp);
	}

	res = leave_voicemail(chan, args.argv0, &leave_options);

	if (res == ERROR_LOCK_PATH) {
		ast_log(LOG_ERROR, "Could not leave voicemail. The path is already locked.\n");
		/*Send the call to n+101 priority, where n is the current priority*/
		if (ast_test_flag(&leave_options, OPT_PRIORITY_JUMP) || ast_opt_priority_jumping)
			if (ast_goto_if_exists(chan, chan->context, chan->exten, chan->priority + 101))
				ast_log(LOG_WARNING, "Extension %s, priority %d doesn't exist.\n", chan->exten, chan->priority + 101);
		pbx_builtin_setvar_helper(chan, "VMSTATUS", "FAILED");
		res = 0;
	}
	
	ast_module_user_remove(u);

	return res;
}

static struct ast_vm_user *find_or_create(char *context, char *mbox)
{
	struct ast_vm_user *vmu;
	AST_LIST_TRAVERSE(&users, vmu, list) {
		if (ast_test_flag((&globalflags), VM_SEARCH) && !strcasecmp(mbox, vmu->mailbox))
			break;
		if (context && (!strcasecmp(context, vmu->context)) && (!strcasecmp(mbox, vmu->mailbox)))
			break;
	}
	
	if (!vmu) {
		if ((vmu = ast_calloc(1, sizeof(*vmu)))) {
			ast_copy_string(vmu->context, context, sizeof(vmu->context));
			ast_copy_string(vmu->mailbox, mbox, sizeof(vmu->mailbox));
			AST_LIST_INSERT_TAIL(&users, vmu, list);
		}
	}
	return vmu;
}

static int append_mailbox(char *context, char *mbox, char *data)
{
	/* Assumes lock is already held */
	char *tmp;
	char *stringp;
	char *s;
	struct ast_vm_user *vmu;

	tmp = ast_strdupa(data);

	if ((vmu = find_or_create(context, mbox))) {
		populate_defaults(vmu);

		stringp = tmp;
		if ((s = strsep(&stringp, ","))) 
			ast_copy_string(vmu->password, s, sizeof(vmu->password));
		if (stringp && (s = strsep(&stringp, ","))) 
			ast_copy_string(vmu->fullname, s, sizeof(vmu->fullname));
		if (stringp && (s = strsep(&stringp, ","))) 
			ast_copy_string(vmu->email, s, sizeof(vmu->email));
		if (stringp && (s = strsep(&stringp, ","))) 
			ast_copy_string(vmu->pager, s, sizeof(vmu->pager));
		if (stringp && (s = strsep(&stringp, ","))) 
			apply_options(vmu, s);
	}
	return 0;
}

static int vm_box_exists(struct ast_channel *chan, void *data) 
{
	struct ast_module_user *u;
	struct ast_vm_user svm;
	char *context, *box;
	int priority_jump = 0;
	AST_DECLARE_APP_ARGS(args,
		AST_APP_ARG(mbox);
		AST_APP_ARG(options);
	);
	static int dep_warning = 0;

	if (ast_strlen_zero(data)) {
		ast_log(LOG_ERROR, "MailboxExists requires an argument: (vmbox[@context][|options])\n");
		return -1;
	}

	u = ast_module_user_add(chan);

	if (!dep_warning) {
		dep_warning = 1;
		ast_log(LOG_WARNING, "MailboxExists is deprecated.  Please use ${MAILBOX_EXISTS(%s)} instead.\n", (char *)data);
	}

	box = ast_strdupa(data);

	AST_STANDARD_APP_ARGS(args, box);

	if (args.options) {
		if (strchr(args.options, 'j'))
			priority_jump = 1;
	}

	if ((context = strchr(args.mbox, '@'))) {
		*context = '\0';
		context++;
	}

	if (find_user(&svm, context, args.mbox)) {
		pbx_builtin_setvar_helper(chan, "VMBOXEXISTSSTATUS", "SUCCESS");
		if (priority_jump || ast_opt_priority_jumping)
			if (ast_goto_if_exists(chan, chan->context, chan->exten, chan->priority + 101)) 
				ast_log(LOG_WARNING, "VM box %s@%s exists, but extension %s, priority %d doesn't exist\n", box, context, chan->exten, chan->priority + 101);
	} else
		pbx_builtin_setvar_helper(chan, "VMBOXEXISTSSTATUS", "FAILED");
	ast_module_user_remove(u);
	return 0;
}

static int acf_mailbox_exists(struct ast_channel *chan, const char *cmd, char *args, char *buf, size_t len)
{
	struct ast_vm_user svm;
	AST_DECLARE_APP_ARGS(arg,
		AST_APP_ARG(mbox);
		AST_APP_ARG(context);
	);

	AST_NONSTANDARD_APP_ARGS(arg, args, '@');

	ast_copy_string(buf, find_user(&svm, ast_strlen_zero(arg.context) ? "default" : arg.context, arg.mbox) ? "1" : "0", len);
	return 0;
}

static struct ast_custom_function mailbox_exists_acf = {
	.name = "MAILBOX_EXISTS",
	.synopsis = "Tell if a mailbox is configured",
	.desc =
"Returns a boolean of whether the corresponding mailbox exists.  If context\n"
"is not specified, defaults to the \"default\" context.\n",
	.syntax = "MAILBOX_EXISTS(<vmbox>[@<context>])",
	.read = acf_mailbox_exists,
};

static int vmauthenticate(struct ast_channel *chan, void *data)
{
	struct ast_module_user *u;
	char *s = data, *user=NULL, *context=NULL, mailbox[AST_MAX_EXTENSION] = "";
	struct ast_vm_user vmus;
	char *options = NULL;
	int silent = 0, skipuser = 0;
	int res = -1;

	u = ast_module_user_add(chan);
	
	if (s) {
		s = ast_strdupa(s);
		user = strsep(&s, "|");
		options = strsep(&s, "|");
		if (user) {
			s = user;
			user = strsep(&s, "@");
			context = strsep(&s, "");
			if (!ast_strlen_zero(user))
				skipuser++;
			ast_copy_string(mailbox, user, sizeof(mailbox));
		}
	}

	if (options) {
		silent = (strchr(options, 's')) != NULL;
	}

	if (!vm_authenticate(chan, mailbox, sizeof(mailbox), &vmus, context, NULL, skipuser, 3, silent)) {
		pbx_builtin_setvar_helper(chan, "AUTH_MAILBOX", mailbox);
		pbx_builtin_setvar_helper(chan, "AUTH_CONTEXT", vmus.context);
		ast_play_and_wait(chan, "auth-thankyou");
		res = 0;
	}

	ast_module_user_remove(u);
	return res;
}

static const char voicemail_show_users_help[] =
"Usage: voicemail show users [for <context>]\n"
"       Lists all mailboxes currently set up\n";

static const char voicemail_show_zones_help[] =
"Usage: voicemail show zones\n"
"       Lists zone message formats\n";

/*! \brief Show a list of voicemail users in the CLI */
static int handle_voicemail_show_users(int fd, int argc, char *argv[])
{
	struct ast_vm_user *vmu;
	char *output_format = "%-10s %-5s %-25s %-10s %6s\n";

	if ((argc < 3) || (argc > 5) || (argc == 4))
		return RESULT_SHOWUSAGE;
	if ((argc == 5) && strcmp(argv[3],"for"))
		return RESULT_SHOWUSAGE;

	AST_LIST_LOCK(&users);
	if (AST_LIST_EMPTY(&users)) {
		ast_cli(fd, "There are no voicemail users currently defined\n");
		AST_LIST_UNLOCK(&users);
		return RESULT_FAILURE;
	}
	if (argc == 3)
		ast_cli(fd, output_format, "Context", "Mbox", "User", "Zone", "NewMsg");
	else {
		int count = 0;
		AST_LIST_TRAVERSE(&users, vmu, list) {
			if (!strcmp(argv[4],vmu->context))
				count++;
		}
		if (count) {
			ast_cli(fd, output_format, "Context", "Mbox", "User", "Zone", "NewMsg");
		} else {
			ast_cli(fd, "No such voicemail context \"%s\"\n", argv[4]);
			AST_LIST_UNLOCK(&users);
			return RESULT_FAILURE;
		}
	}
	AST_LIST_TRAVERSE(&users, vmu, list) {
		int newmsgs = 0, oldmsgs = 0;
		char count[12], tmp[256] = "";

		if ((argc == 3) || ((argc == 5) && !strcmp(argv[4],vmu->context))) {
			snprintf(tmp, sizeof(tmp), "%s@%s", vmu->mailbox, ast_strlen_zero(vmu->context) ? "default" : vmu->context);
			inboxcount(tmp, &newmsgs, &oldmsgs);
			snprintf(count,sizeof(count),"%d",newmsgs);
			ast_cli(fd, output_format, vmu->context, vmu->mailbox, vmu->fullname, vmu->zonetag, count);
		}
	}
	AST_LIST_UNLOCK(&users);
	return RESULT_SUCCESS;
}

/*! \brief Show a list of voicemail zones in the CLI */
static int handle_voicemail_show_zones(int fd, int argc, char *argv[])
{
	struct vm_zone *zone;
	char *output_format = "%-15s %-20s %-45s\n";
	int res = RESULT_SUCCESS;

	if (argc != 3)
		return RESULT_SHOWUSAGE;

	AST_LIST_LOCK(&zones);
	if (!AST_LIST_EMPTY(&zones)) {
		ast_cli(fd, output_format, "Zone", "Timezone", "Message Format");
		AST_LIST_TRAVERSE(&zones, zone, list) {
			ast_cli(fd, output_format, zone->name, zone->timezone, zone->msg_format);
		}
	} else {
		ast_cli(fd, "There are no voicemail zones currently defined\n");
		res = RESULT_FAILURE;
	}
	AST_LIST_UNLOCK(&zones);

	return res;
}

static char *complete_voicemail_show_users(const char *line, const char *word, int pos, int state)
{
	int which = 0;
	int wordlen;
	struct ast_vm_user *vmu;
	const char *context = "";

	/* 0 - show; 1 - voicemail; 2 - users; 3 - for; 4 - <context> */
	if (pos > 4)
		return NULL;
	if (pos == 3)
		return (state == 0) ? ast_strdup("for") : NULL;
	wordlen = strlen(word);
	AST_LIST_TRAVERSE(&users, vmu, list) {
		if (!strncasecmp(word, vmu->context, wordlen)) {
			if (context && strcmp(context, vmu->context) && ++which > state)
				return ast_strdup(vmu->context);
			/* ignore repeated contexts ? */
			context = vmu->context;
		}
	}
	return NULL;
}

static struct ast_cli_entry cli_voicemail[] = {
	{ { "voicemail", "show", "users", NULL },
	handle_voicemail_show_users, "List defined voicemail boxes",
	voicemail_show_users_help, complete_voicemail_show_users, NULL },

	{ { "voicemail", "show", "zones", NULL },
	handle_voicemail_show_zones, "List zone message formats",
	voicemail_show_zones_help, NULL, NULL },
};

static int load_config(void)
{
	struct ast_vm_user *cur;
	struct vm_zone *zcur;
	struct ast_config *cfg, *ucfg;
	char *cat;
	struct ast_variable *var;
	const char *notifystr = NULL;
	const char *smdistr = NULL;
	const char *astattach;
	const char *astsearch;
	const char *astsaycid;
	const char *send_voicemail;
#ifdef IMAP_STORAGE
	const char *imap_server;
	const char *imap_port;
	const char *imap_flags;
	const char *imap_folder;
	const char *auth_user;
	const char *auth_password;
	const char *expunge_on_hangup;
#endif
	const char *astcallop;
	const char *astreview;
	const char *asttempgreetwarn;
	const char *astskipcmd;
	const char *asthearenv;
	const char *astsaydurationinfo;
	const char *astsaydurationminfo;
	const char *silencestr;
	const char *maxmsgstr;
	const char *astdirfwd;
	const char *thresholdstr;
	const char *fmt;
	const char *astemail;
	const char *ucontext;
	const char *astmailcmd = SENDMAIL;
	const char *astforcename;
	const char *astforcegreet;
	const char *s;
	char *q,*stringp;
	const char *dialoutcxt = NULL;
	const char *callbackcxt = NULL;	
	const char *exitcxt = NULL;	
	const char *extpc;
	const char *emaildateformatstr;
	const char *volgainstr;
	const char *vm_paswd;
	const char *vm_newpasswd;
	const char *vm_passchange;
	const char *vm_reenterpass;
	const char *vm_mism;
	int x;
	int tmpadsi[4];

	cfg = ast_config_load(VOICEMAIL_CONFIG);

	AST_LIST_LOCK(&users);
	while ((cur = AST_LIST_REMOVE_HEAD(&users, list))) {
		ast_set_flag(cur, VM_ALLOCED);
		free_user(cur);
	}

	AST_LIST_LOCK(&zones);
	while ((zcur = AST_LIST_REMOVE_HEAD(&zones, list))) 
		free_zone(zcur);
	AST_LIST_UNLOCK(&zones);

	memset(ext_pass_cmd, 0, sizeof(ext_pass_cmd));

	if (cfg) {
		/* General settings */

		if (!(ucontext = ast_variable_retrieve(cfg, "general", "userscontext")))
			ucontext = "default";
		ast_copy_string(userscontext, ucontext, sizeof(userscontext));
		/* Attach voice message to mail message ? */
		if (!(astattach = ast_variable_retrieve(cfg, "general", "attach"))) 
			astattach = "yes";
		ast_set2_flag((&globalflags), ast_true(astattach), VM_ATTACH);	

		if (!(astsearch = ast_variable_retrieve(cfg, "general", "searchcontexts")))
			astsearch = "no";
		ast_set2_flag((&globalflags), ast_true(astsearch), VM_SEARCH);

		volgain = 0.0;
		if ((volgainstr = ast_variable_retrieve(cfg, "general", "volgain")))
			sscanf(volgainstr, "%lf", &volgain);

#ifdef ODBC_STORAGE
		strcpy(odbc_database, "asterisk");
		if ((thresholdstr = ast_variable_retrieve(cfg, "general", "odbcstorage"))) {
			ast_copy_string(odbc_database, thresholdstr, sizeof(odbc_database));
		}
		strcpy(odbc_table, "voicemessages");
		if ((thresholdstr = ast_variable_retrieve(cfg, "general", "odbctable"))) {
			ast_copy_string(odbc_table, thresholdstr, sizeof(odbc_table));
		}
#endif		
		/* Mail command */
		strcpy(mailcmd, SENDMAIL);
		if ((astmailcmd = ast_variable_retrieve(cfg, "general", "mailcmd")))
			ast_copy_string(mailcmd, astmailcmd, sizeof(mailcmd)); /* User setting */

		maxsilence = 0;
		if ((silencestr = ast_variable_retrieve(cfg, "general", "maxsilence"))) {
			maxsilence = atoi(silencestr);
			if (maxsilence > 0)
				maxsilence *= 1000;
		}
		
		if (!(maxmsgstr = ast_variable_retrieve(cfg, "general", "maxmsg"))) {
			maxmsg = MAXMSG;
		} else {
			maxmsg = atoi(maxmsgstr);
			if (maxmsg <= 0) {
				ast_log(LOG_WARNING, "Invalid number of messages per folder '%s'. Using default value %i\n", maxmsgstr, MAXMSG);
				maxmsg = MAXMSG;
			} else if (maxmsg > MAXMSGLIMIT) {
				ast_log(LOG_WARNING, "Maximum number of messages per folder is %i. Cannot accept value '%s'\n", MAXMSGLIMIT, maxmsgstr);
				maxmsg = MAXMSGLIMIT;
			}
		}

		/* Load date format config for voicemail mail */
		if ((emaildateformatstr = ast_variable_retrieve(cfg, "general", "emaildateformat"))) {
			ast_copy_string(emaildateformat, emaildateformatstr, sizeof(emaildateformat));
		}

		/* External password changing command */
		if ((extpc = ast_variable_retrieve(cfg, "general", "externpass"))) {
			ast_copy_string(ext_pass_cmd,extpc,sizeof(ext_pass_cmd));
			pwdchange = PWDCHANGE_EXTERNAL;
		} else if ((extpc = ast_variable_retrieve(cfg, "general", "externpassnotify"))) {
			ast_copy_string(ext_pass_cmd,extpc,sizeof(ext_pass_cmd));
			pwdchange = PWDCHANGE_EXTERNAL | PWDCHANGE_INTERNAL;
		}

#ifdef IMAP_STORAGE
		/* IMAP server address */
		if ((imap_server = ast_variable_retrieve(cfg, "general", "imapserver"))) {
			ast_copy_string(imapserver, imap_server, sizeof(imapserver));
		} else {
			ast_copy_string(imapserver,"localhost", sizeof(imapserver));
		}
		/* IMAP server port */
		if ((imap_port = ast_variable_retrieve(cfg, "general", "imapport"))) {
			ast_copy_string(imapport, imap_port, sizeof(imapport));
		} else {
			ast_copy_string(imapport,"143", sizeof(imapport));
		}
		/* IMAP server flags */
		if ((imap_flags = ast_variable_retrieve(cfg, "general", "imapflags"))) {
			ast_copy_string(imapflags, imap_flags, sizeof(imapflags));
		}
		/* IMAP server master username */
		if ((auth_user = ast_variable_retrieve(cfg, "general", "authuser"))) {
			ast_copy_string(authuser, auth_user, sizeof(authuser));
		}
		/* IMAP server master password */
		if ((auth_password = ast_variable_retrieve(cfg, "general", "authpassword"))) {
			ast_copy_string(authpassword, auth_password, sizeof(authpassword));
		}
		/* Expunge on exit */
		if ((expunge_on_hangup = ast_variable_retrieve(cfg, "general", "expungeonhangup"))) {
			if(ast_false(expunge_on_hangup))
				expungeonhangup = 0;
			else
				expungeonhangup = 1;
		} else {
			expungeonhangup = 1;
		}
		/* IMAP voicemail folder */
		if ((imap_folder = ast_variable_retrieve(cfg, "general", "imapfolder"))) {
			ast_copy_string(imapfolder, imap_folder, sizeof(imapfolder));
		} else {
			ast_copy_string(imapfolder,"INBOX", sizeof(imapfolder));
		}
#endif
		/* External voicemail notify application */
		if ((notifystr = ast_variable_retrieve(cfg, "general", "externnotify"))) {
			ast_copy_string(externnotify, notifystr, sizeof(externnotify));
			if (option_debug)
				ast_log(LOG_DEBUG, "found externnotify: %s\n", externnotify);
		} else {
			externnotify[0] = '\0';
		}

		/* SMDI voicemail notification */
		if ((s = ast_variable_retrieve(cfg, "general", "smdienable")) && ast_true(s)) {
			if (option_debug)
				ast_log(LOG_DEBUG, "Enabled SMDI voicemail notification\n");
			if ((smdistr = ast_variable_retrieve(cfg, "general", "smdiport"))) {
				smdi_iface = ast_smdi_interface_find(smdistr);
			} else {
				if (option_debug)
					ast_log(LOG_DEBUG, "No SMDI interface set, trying default (/dev/ttyS0)\n");
				smdi_iface = ast_smdi_interface_find("/dev/ttyS0");
			}
			if (!smdi_iface) {
				ast_log(LOG_ERROR, "No valid SMDI interface specfied, disabling SMDI voicemail notification\n");
			} else {
				if (option_debug)
					ast_log(LOG_DEBUG, "Using SMDI port %s\n", smdi_iface->name);
			}
		}

		/* Silence treshold */
		silencethreshold = 256;
		if ((thresholdstr = ast_variable_retrieve(cfg, "general", "silencethreshold")))
			silencethreshold = atoi(thresholdstr);
		
		if (!(astemail = ast_variable_retrieve(cfg, "general", "serveremail"))) 
			astemail = ASTERISK_USERNAME;
		ast_copy_string(serveremail, astemail, sizeof(serveremail));
		
		vmmaxsecs = 0;
		if ((s = ast_variable_retrieve(cfg, "general", "maxsecs"))) {
			if (sscanf(s, "%d", &x) == 1) {
				vmmaxsecs = x;
			} else {
				ast_log(LOG_WARNING, "Invalid max message time length\n");
			}
		} else if ((s = ast_variable_retrieve(cfg, "general", "maxmessage"))) {
			static int maxmessage_deprecate = 0;
			if (maxmessage_deprecate == 0) {
				maxmessage_deprecate = 1;
				ast_log(LOG_WARNING, "Setting 'maxmessage' has been deprecated in favor of 'maxsecs'.\n");
			}
			if (sscanf(s, "%d", &x) == 1) {
				vmmaxsecs = x;
			} else {
				ast_log(LOG_WARNING, "Invalid max message time length\n");
			}
		}

		vmminsecs = 0;
		if ((s = ast_variable_retrieve(cfg, "general", "minsecs"))) {
			if (sscanf(s, "%d", &x) == 1) {
				vmminsecs = x;
				if (maxsilence <= vmminsecs)
					ast_log(LOG_WARNING, "maxsilence should be less than minmessage or you may get empty messages\n");
			} else {
				ast_log(LOG_WARNING, "Invalid min message time length\n");
			}
		} else if ((s = ast_variable_retrieve(cfg, "general", "minmessage"))) {
			static int maxmessage_deprecate = 0;
			if (maxmessage_deprecate == 0) {
				maxmessage_deprecate = 1;
				ast_log(LOG_WARNING, "Setting 'minmessage' has been deprecated in favor of 'minsecs'.\n");
			}
			if (sscanf(s, "%d", &x) == 1) {
				vmminsecs = x;
				if (maxsilence <= vmminsecs)
					ast_log(LOG_WARNING, "maxsilence should be less than minmessage or you may get empty messages\n");
			} else {
				ast_log(LOG_WARNING, "Invalid min message time length\n");
			}
		}

		fmt = ast_variable_retrieve(cfg, "general", "format");
		if (!fmt)
			fmt = "wav";	
		ast_copy_string(vmfmts, fmt, sizeof(vmfmts));

		skipms = 3000;
		if ((s = ast_variable_retrieve(cfg, "general", "maxgreet"))) {
			if (sscanf(s, "%d", &x) == 1) {
				maxgreet = x;
			} else {
				ast_log(LOG_WARNING, "Invalid max message greeting length\n");
			}
		}

		if ((s = ast_variable_retrieve(cfg, "general", "skipms"))) {
			if (sscanf(s, "%d", &x) == 1) {
				skipms = x;
			} else {
				ast_log(LOG_WARNING, "Invalid skipms value\n");
			}
		}

		maxlogins = 3;
		if ((s = ast_variable_retrieve(cfg, "general", "maxlogins"))) {
			if (sscanf(s, "%d", &x) == 1) {
				maxlogins = x;
			} else {
				ast_log(LOG_WARNING, "Invalid max failed login attempts\n");
			}
		}

		/* Force new user to record name ? */
		if (!(astforcename = ast_variable_retrieve(cfg, "general", "forcename"))) 
			astforcename = "no";
		ast_set2_flag((&globalflags), ast_true(astforcename), VM_FORCENAME);

		/* Force new user to record greetings ? */
		if (!(astforcegreet = ast_variable_retrieve(cfg, "general", "forcegreetings"))) 
			astforcegreet = "no";
		ast_set2_flag((&globalflags), ast_true(astforcegreet), VM_FORCEGREET);

		if ((s = ast_variable_retrieve(cfg, "general", "cidinternalcontexts"))){
			if (option_debug)
				ast_log(LOG_DEBUG,"VM_CID Internal context string: %s\n",s);
			stringp = ast_strdupa(s);
			for (x = 0 ; x < MAX_NUM_CID_CONTEXTS ; x++){
				if (!ast_strlen_zero(stringp)) {
					q = strsep(&stringp,",");
					while ((*q == ' ')||(*q == '\t')) /* Eat white space between contexts */
						q++;
					ast_copy_string(cidinternalcontexts[x], q, sizeof(cidinternalcontexts[x]));
					if (option_debug)
						ast_log(LOG_DEBUG,"VM_CID Internal context %d: %s\n", x, cidinternalcontexts[x]);
				} else {
					cidinternalcontexts[x][0] = '\0';
				}
			}
		}
		if (!(astreview = ast_variable_retrieve(cfg, "general", "review"))){
			if (option_debug)
				ast_log(LOG_DEBUG,"VM Review Option disabled globally\n");
			astreview = "no";
		}
		ast_set2_flag((&globalflags), ast_true(astreview), VM_REVIEW);	

		/*Temporary greeting reminder */
		if (!(asttempgreetwarn = ast_variable_retrieve(cfg, "general", "tempgreetwarn"))) {
			if (option_debug)
				ast_log(LOG_DEBUG, "VM Temporary Greeting Reminder Option disabled globally\n");
			asttempgreetwarn = "no";
		} else {
			if (option_debug)
				ast_log(LOG_DEBUG, "VM Temporary Greeting Reminder Option enabled globally\n");
		}
		ast_set2_flag((&globalflags), ast_true(asttempgreetwarn), VM_TEMPGREETWARN);

		if (!(astcallop = ast_variable_retrieve(cfg, "general", "operator"))){
			if (option_debug)
				ast_log(LOG_DEBUG,"VM Operator break disabled globally\n");
			astcallop = "no";
		}
		ast_set2_flag((&globalflags), ast_true(astcallop), VM_OPERATOR);	

		if (!(astsaycid = ast_variable_retrieve(cfg, "general", "saycid"))) {
			if (option_debug)
				ast_log(LOG_DEBUG,"VM CID Info before msg disabled globally\n");
			astsaycid = "no";
		} 
		ast_set2_flag((&globalflags), ast_true(astsaycid), VM_SAYCID);	

		if (!(send_voicemail = ast_variable_retrieve(cfg,"general", "sendvoicemail"))){
			if (option_debug)
				ast_log(LOG_DEBUG,"Send Voicemail msg disabled globally\n");
			send_voicemail = "no";
		}
		ast_set2_flag((&globalflags), ast_true(send_voicemail), VM_SVMAIL);
	
		if (!(asthearenv = ast_variable_retrieve(cfg, "general", "envelope"))) {
			if (option_debug)
				ast_log(LOG_DEBUG,"ENVELOPE before msg enabled globally\n");
			asthearenv = "yes";
		}
		ast_set2_flag((&globalflags), ast_true(asthearenv), VM_ENVELOPE);	

		if (!(astsaydurationinfo = ast_variable_retrieve(cfg, "general", "sayduration"))) {
			if (option_debug)
				ast_log(LOG_DEBUG,"Duration info before msg enabled globally\n");
			astsaydurationinfo = "yes";
		}
		ast_set2_flag((&globalflags), ast_true(astsaydurationinfo), VM_SAYDURATION);	

		saydurationminfo = 2;
		if ((astsaydurationminfo = ast_variable_retrieve(cfg, "general", "saydurationm"))) {
			if (sscanf(astsaydurationminfo, "%d", &x) == 1) {
				saydurationminfo = x;
			} else {
				ast_log(LOG_WARNING, "Invalid min duration for say duration\n");
			}
		}

		if (!(astskipcmd = ast_variable_retrieve(cfg, "general", "nextaftercmd"))) {
			if (option_debug)
				ast_log(LOG_DEBUG,"We are not going to skip to the next msg after save/delete\n");
			astskipcmd = "no";
		}
		ast_set2_flag((&globalflags), ast_true(astskipcmd), VM_SKIPAFTERCMD);

		if ((dialoutcxt = ast_variable_retrieve(cfg, "general", "dialout"))) {
			ast_copy_string(dialcontext, dialoutcxt, sizeof(dialcontext));
			if (option_debug)
				ast_log(LOG_DEBUG, "found dialout context: %s\n", dialcontext);
		} else {
			dialcontext[0] = '\0';	
		}
		
		if ((callbackcxt = ast_variable_retrieve(cfg, "general", "callback"))) {
			ast_copy_string(callcontext, callbackcxt, sizeof(callcontext));
			if (option_debug)
				ast_log(LOG_DEBUG, "found callback context: %s\n", callcontext);
		} else {
			callcontext[0] = '\0';
		}

		if ((exitcxt = ast_variable_retrieve(cfg, "general", "exitcontext"))) {
			ast_copy_string(exitcontext, exitcxt, sizeof(exitcontext));
			if (option_debug)
				ast_log(LOG_DEBUG, "found operator context: %s\n", exitcontext);
		} else {
			exitcontext[0] = '\0';
		}
		
		/* load password sounds configuration */
		if ((vm_paswd = ast_variable_retrieve(cfg, "general", "vm-password")))
			ast_copy_string(vm_password, vm_paswd, sizeof(vm_password));
		if ((vm_newpasswd = ast_variable_retrieve(cfg, "general", "vm-newpassword")))
			ast_copy_string(vm_newpassword, vm_newpasswd, sizeof(vm_newpassword));
		if ((vm_passchange = ast_variable_retrieve(cfg, "general", "vm-passchanged")))
			ast_copy_string(vm_passchanged, vm_passchange, sizeof(vm_passchanged));
		if ((vm_reenterpass = ast_variable_retrieve(cfg, "general", "vm-reenterpassword")))
			ast_copy_string(vm_reenterpassword, vm_reenterpass, sizeof(vm_reenterpassword));
		if ((vm_mism = ast_variable_retrieve(cfg, "general", "vm-mismatch")))
			ast_copy_string(vm_mismatch, vm_mism, sizeof(vm_mismatch));

		if (!(astdirfwd = ast_variable_retrieve(cfg, "general", "usedirectory"))) 
			astdirfwd = "no";
		ast_set2_flag((&globalflags), ast_true(astdirfwd), VM_DIRECFORWARD);	
		if ((ucfg = ast_config_load("users.conf"))) {	
			for (cat = ast_category_browse(ucfg, NULL); cat ; cat = ast_category_browse(ucfg, cat)) {
				if (!ast_true(ast_config_option(ucfg, cat, "hasvoicemail")))
					continue;
				if ((cur = find_or_create(userscontext, cat))) {
					populate_defaults(cur);
					apply_options_full(cur, ast_variable_browse(ucfg, cat));
					ast_copy_string(cur->context, userscontext, sizeof(cur->context));
				}
			}
			ast_config_destroy(ucfg);
		}
		cat = ast_category_browse(cfg, NULL);
		while (cat) {
			if (strcasecmp(cat, "general")) {
				var = ast_variable_browse(cfg, cat);
				if (strcasecmp(cat, "zonemessages")) {
					/* Process mailboxes in this context */
					while (var) {
						append_mailbox(cat, var->name, var->value);
						var = var->next;
					}
				} else {
					/* Timezones in this context */
					while (var) {
						struct vm_zone *z;
						if ((z = ast_malloc(sizeof(*z)))) {
							char *msg_format, *timezone;
							msg_format = ast_strdupa(var->value);
							timezone = strsep(&msg_format, "|");
							if (msg_format) {
								ast_copy_string(z->name, var->name, sizeof(z->name));
								ast_copy_string(z->timezone, timezone, sizeof(z->timezone));
								ast_copy_string(z->msg_format, msg_format, sizeof(z->msg_format));
								AST_LIST_LOCK(&zones);
								AST_LIST_INSERT_HEAD(&zones, z, list);
								AST_LIST_UNLOCK(&zones);
							} else {
								ast_log(LOG_WARNING, "Invalid timezone definition at line %d\n", var->lineno);
								free(z);
							}
						} else {
							free(z);
							AST_LIST_UNLOCK(&users);
							ast_config_destroy(cfg);
							return -1;
						}
						var = var->next;
					}
				}
			}
			cat = ast_category_browse(cfg, cat);
		}
		memset(fromstring,0,sizeof(fromstring));
		memset(pagerfromstring,0,sizeof(pagerfromstring));
		memset(emailtitle,0,sizeof(emailtitle));
		strcpy(charset, "ISO-8859-1");
		if (emailbody) {
			free(emailbody);
			emailbody = NULL;
		}
		if (emailsubject) {
			free(emailsubject);
			emailsubject = NULL;
		}
		if (pagerbody) {
			free(pagerbody);
			pagerbody = NULL;
		}
		if (pagersubject) {
			free(pagersubject);
			pagersubject = NULL;
		}
		if ((s = ast_variable_retrieve(cfg, "general", "pbxskip")))
			ast_set2_flag((&globalflags), ast_true(s), VM_PBXSKIP);
		if ((s = ast_variable_retrieve(cfg, "general", "fromstring")))
			ast_copy_string(fromstring,s,sizeof(fromstring));
		if ((s = ast_variable_retrieve(cfg, "general", "pagerfromstring")))
			ast_copy_string(pagerfromstring,s,sizeof(pagerfromstring));
		if ((s = ast_variable_retrieve(cfg, "general", "charset")))
			ast_copy_string(charset,s,sizeof(charset));
		if ((s = ast_variable_retrieve(cfg, "general", "adsifdn"))) {
			sscanf(s, "%2x%2x%2x%2x", &tmpadsi[0], &tmpadsi[1], &tmpadsi[2], &tmpadsi[3]);
			for (x = 0; x < 4; x++) {
				memcpy(&adsifdn[x], &tmpadsi[x], 1);
			}
		}
		if ((s = ast_variable_retrieve(cfg, "general", "adsisec"))) {
			sscanf(s, "%2x%2x%2x%2x", &tmpadsi[0], &tmpadsi[1], &tmpadsi[2], &tmpadsi[3]);
			for (x = 0; x < 4; x++) {
				memcpy(&adsisec[x], &tmpadsi[x], 1);
			}
		}
		if ((s = ast_variable_retrieve(cfg, "general", "adsiver")))
			if (atoi(s)) {
				adsiver = atoi(s);
			}
		if ((s = ast_variable_retrieve(cfg, "general", "emailtitle"))) {
			ast_log(LOG_NOTICE, "Keyword 'emailtitle' is DEPRECATED, please use 'emailsubject' instead.\n");
			ast_copy_string(emailtitle,s,sizeof(emailtitle));
		}
		if ((s = ast_variable_retrieve(cfg, "general", "emailsubject")))
			emailsubject = ast_strdup(s);
		if ((s = ast_variable_retrieve(cfg, "general", "emailbody"))) {
			char *tmpread, *tmpwrite;
			emailbody = ast_strdup(s);

			/* substitute strings \t and \n into the appropriate characters */
			tmpread = tmpwrite = emailbody;
			while ((tmpwrite = strchr(tmpread,'\\'))) {
				switch (tmpwrite[1]) {
				case 'r':
					memmove(tmpwrite + 1, tmpwrite + 2, strlen(tmpwrite + 2) + 1);
					*tmpwrite = '\r';
					break;
				case 'n':
					memmove(tmpwrite + 1, tmpwrite + 2, strlen(tmpwrite + 2) + 1);
					*tmpwrite = '\n';
					break;
				case 't':
					memmove(tmpwrite + 1, tmpwrite + 2, strlen(tmpwrite + 2) + 1);
					*tmpwrite = '\t';
					break;
				default:
					ast_log(LOG_NOTICE, "Substitution routine does not support this character: %c\n", tmpwrite[1]);
				}
				tmpread = tmpwrite + 1;
			}
		}
		if ((s = ast_variable_retrieve(cfg, "general", "pagersubject")))
			pagersubject = ast_strdup(s);
		if ((s = ast_variable_retrieve(cfg, "general", "pagerbody"))) {
			char *tmpread, *tmpwrite;
			pagerbody = ast_strdup(s);

			/* substitute strings \t and \n into the appropriate characters */
			tmpread = tmpwrite = pagerbody;
			while ((tmpwrite = strchr(tmpread, '\\'))) {
				switch (tmpwrite[1]) {
				case 'r':
					memmove(tmpwrite + 1, tmpwrite + 2, strlen(tmpwrite + 2) + 1);
					*tmpwrite = '\r';
					break;
				case 'n':
					memmove(tmpwrite + 1, tmpwrite + 2, strlen(tmpwrite + 2) + 1);
					*tmpwrite = '\n';
					break;
				case 't':
					memmove(tmpwrite + 1, tmpwrite + 2, strlen(tmpwrite + 2) + 1);
					*tmpwrite = '\t';
					break;
				default:
					ast_log(LOG_NOTICE, "Substitution routine does not support this character: %c\n", tmpwrite[1]);
				}
				tmpread = tmpwrite + 1;
			}
		}
		AST_LIST_UNLOCK(&users);
		ast_config_destroy(cfg);
		return 0;
	} else {
		AST_LIST_UNLOCK(&users);
		ast_log(LOG_WARNING, "Failed to load configuration file. Module not activated.\n");
		return 0;
	}
}

static int reload(void)
{
	return(load_config());
}

static int unload_module(void)
{
	int res;
	
	res = ast_unregister_application(app);
	res |= ast_unregister_application(app2);
	res |= ast_unregister_application(app3);
	res |= ast_unregister_application(app4);
	res |= ast_custom_function_unregister(&mailbox_exists_acf);
	ast_cli_unregister_multiple(cli_voicemail, sizeof(cli_voicemail) / sizeof(struct ast_cli_entry));
	ast_uninstall_vm_functions();
	
	ast_module_user_hangup_all();

	return res;
}

static int load_module(void)
{
	int res;
	res = ast_register_application(app, vm_exec, synopsis_vm, descrip_vm);
	res |= ast_register_application(app2, vm_execmain, synopsis_vmain, descrip_vmain);
	res |= ast_register_application(app3, vm_box_exists, synopsis_vm_box_exists, descrip_vm_box_exists);
	res |= ast_register_application(app4, vmauthenticate, synopsis_vmauthenticate, descrip_vmauthenticate);
	res |= ast_custom_function_register(&mailbox_exists_acf);
	if (res)
		return(res);

	if ((res=load_config())) {
		return(res);
	}

	ast_cli_register_multiple(cli_voicemail, sizeof(cli_voicemail) / sizeof(struct ast_cli_entry));

	/* compute the location of the voicemail spool directory */
	snprintf(VM_SPOOL_DIR, sizeof(VM_SPOOL_DIR), "%s/voicemail/", ast_config_AST_SPOOL_DIR);

	ast_install_vm_functions(has_voicemail, inboxcount, messagecount);

	return res;
}

static int dialout(struct ast_channel *chan, struct ast_vm_user *vmu, char *num, char *outgoing_context) 
{
	int cmd = 0;
	char destination[80] = "";
	int retries = 0;

	if (!num) {
		if (option_verbose > 2)
			ast_verbose( VERBOSE_PREFIX_3 "Destination number will be entered manually\n");
		while (retries < 3 && cmd != 't') {
			destination[1] = '\0';
			destination[0] = cmd = ast_play_and_wait(chan,"vm-enter-num-to-call");
			if (!cmd)
				destination[0] = cmd = ast_play_and_wait(chan, "vm-then-pound");
			if (!cmd)
				destination[0] = cmd = ast_play_and_wait(chan, "vm-star-cancel");
			if (!cmd) {
				cmd = ast_waitfordigit(chan, 6000);
				if (cmd)
					destination[0] = cmd;
			}
			if (!cmd) {
				retries++;
			} else {

				if (cmd < 0)
					return 0;
				if (cmd == '*') {
					if (option_verbose > 2)
						ast_verbose( VERBOSE_PREFIX_3 "User hit '*' to cancel outgoing call\n");
					return 0;
				}
				if ((cmd = ast_readstring(chan,destination + strlen(destination),sizeof(destination)-1,6000,10000,"#")) < 0) 
					retries++;
				else
					cmd = 't';
			}
		}
		if (retries >= 3) {
			return 0;
		}
		
	} else {
		if (option_verbose > 2)
			ast_verbose( VERBOSE_PREFIX_3 "Destination number is CID number '%s'\n", num);
		ast_copy_string(destination, num, sizeof(destination));
	}

	if (!ast_strlen_zero(destination)) {
		if (destination[strlen(destination) -1 ] == '*')
			return 0; 
		if (option_verbose > 2)
			ast_verbose( VERBOSE_PREFIX_3 "Placing outgoing call to extension '%s' in context '%s' from context '%s'\n", destination, outgoing_context, chan->context);
		ast_copy_string(chan->exten, destination, sizeof(chan->exten));
		ast_copy_string(chan->context, outgoing_context, sizeof(chan->context));
		chan->priority = 0;
		return 9;
	}
	return 0;
}

static int advanced_options(struct ast_channel *chan, struct ast_vm_user *vmu, struct vm_state *vms, int msg, int option, signed char record_gain)
{
	int res = 0;
#ifdef IMAP_STORAGE
	char origtimeS[256], cidS[256], contextS[256];
	char *header_content, *temp;
	char buf[1024];
#endif
	char filename[PATH_MAX];
	struct ast_config *msg_cfg = NULL;
	const char *origtime, *context;
	char *cid, *name, *num;
	int retries = 0;

	vms->starting = 0; 
#ifdef IMAP_STORAGE
	/* START HERE */

	/* get the message info!! */
	if(option_debug > 2)
		ast_log(LOG_DEBUG, "Before mail_fetchheaders, curmsg is: %d, imap messages is %lu\n", vms->curmsg, vms->msgArray[vms->curmsg]);

	if (!vms->msgArray[vms->curmsg]) {
		ast_log(LOG_WARNING, "Trying to access unknown message\n");
		return -1;
	}

	/* This will only work for new messages... */
	if (!(header_content = mail_fetchheader(vms->mailstream, vms->msgArray[vms->curmsg])) || ast_strlen_zero(header_content)) {
		ast_log(LOG_ERROR,"Could not fetch header for message number %ld\n", vms->msgArray[vms->curmsg]);
		return -1;
	}
	
	/* Get info from headers!! */
	if ((temp = get_header_by_tag(header_content, "X-Asterisk-VM-Caller-ID-Num:", buf, sizeof(buf))))
		ast_copy_string(cidS, temp, sizeof(cidS));
	else
		cidS[0] = '\0';
	
	cid = &cidS[0];

	if ((temp = get_header_by_tag(header_content, "X-Asterisk-VM-Context:", buf, sizeof(buf))))
		ast_copy_string(contextS,temp, sizeof(contextS));
	else
		contextS[0] = '\0';
	
	context = &contextS[0];

	if ((temp = get_header_by_tag(header_content, "X-Asterisk-VM-Orig-time:", buf, sizeof(buf))))
		ast_copy_string(origtimeS,temp, sizeof(origtimeS));
	else
		origtimeS[0] = '\0';
	
	origtime = &origtimeS[0];
	
	ast_copy_string(filename, "IMAP_STORAGE", sizeof(filename));
#else
	make_file(vms->fn, sizeof(vms->fn), vms->curdir, msg);

	/* Retrieve info from VM attribute file */

	make_file(vms->fn2, sizeof(vms->fn2), vms->curdir, vms->curmsg);
	snprintf(filename,sizeof(filename), "%s.txt", vms->fn2);
	RETRIEVE(vms->curdir, vms->curmsg);
	msg_cfg = ast_config_load(filename);
	DISPOSE(vms->curdir, vms->curmsg);
	if (!msg_cfg) {
		ast_log(LOG_WARNING, "No message attribute file?!! (%s)\n", filename);
		return 0;
	}

	if (!(origtime = ast_variable_retrieve(msg_cfg, "message", "origtime"))) {
		ast_config_destroy(msg_cfg);
		return 0;
	}

	cid = ast_strdupa(ast_variable_retrieve(msg_cfg, "message", "callerid"));

	context = ast_variable_retrieve(msg_cfg, "message", "context");
	if (!strncasecmp("macro",context,5)) /* Macro names in contexts are useless for our needs */
		context = ast_variable_retrieve(msg_cfg, "message","macrocontext");
#endif
	switch (option) {
	case 3:
		if (!res)
			res = play_message_datetime(chan, vmu, origtime, filename);
		if (!res)
			res = play_message_callerid(chan, vms, cid, context, 0);

		res = 't';
		break;

	case 2:	/* Call back */

		if (ast_strlen_zero(cid))
			break;

		ast_callerid_parse(cid, &name, &num);
		while ((res > -1) && (res != 't')) {
			switch (res) {
			case '1':
				if (num) {
					/* Dial the CID number */
					res = dialout(chan, vmu, num, vmu->callback);
					if (res) {
						ast_config_destroy(msg_cfg);
						return 9;
					}
				} else {
					res = '2';
				}
				break;

			case '2':
				/* Want to enter a different number, can only do this if there's a dialout context for this user */
				if (!ast_strlen_zero(vmu->dialout)) {
					res = dialout(chan, vmu, NULL, vmu->dialout);
					if (res) {
						ast_config_destroy(msg_cfg);
						return 9;
					}
				} else {
					if (option_verbose > 2)
						ast_verbose( VERBOSE_PREFIX_3 "Caller can not specify callback number - no dialout context available\n");
					res = ast_play_and_wait(chan, "vm-sorry");
				}
				ast_config_destroy(msg_cfg);
				return res;
			case '*':
				res = 't';
				break;
			case '3':
			case '4':
			case '5':
			case '6':
			case '7':
			case '8':
			case '9':
			case '0':

				res = ast_play_and_wait(chan, "vm-sorry");
				retries++;
				break;
			default:
				if (num) {
					if (option_verbose > 2)
						ast_verbose( VERBOSE_PREFIX_3 "Confirm CID number '%s' is number to use for callback\n", num);
					res = ast_play_and_wait(chan, "vm-num-i-have");
					if (!res)
						res = play_message_callerid(chan, vms, num, vmu->context, 1);
					if (!res)
						res = ast_play_and_wait(chan, "vm-tocallnum");
					/* Only prompt for a caller-specified number if there is a dialout context specified */
					if (!ast_strlen_zero(vmu->dialout)) {
						if (!res)
							res = ast_play_and_wait(chan, "vm-calldiffnum");
					}
				} else {
					res = ast_play_and_wait(chan, "vm-nonumber");
					if (!ast_strlen_zero(vmu->dialout)) {
						if (!res)
							res = ast_play_and_wait(chan, "vm-toenternumber");
					}
				}
				if (!res)
					res = ast_play_and_wait(chan, "vm-star-cancel");
				if (!res)
					res = ast_waitfordigit(chan, 6000);
				if (!res) {
					retries++;
					if (retries > 3)
						res = 't';
				}
				break; 
				
			}
			if (res == 't')
				res = 0;
			else if (res == '*')
				res = -1;
		}
		break;
		
	case 1:	/* Reply */
		/* Send reply directly to sender */
		if (ast_strlen_zero(cid))
			break;

		ast_callerid_parse(cid, &name, &num);
		if (!num) {
			if (option_verbose > 2)
				ast_verbose(VERBOSE_PREFIX_3 "No CID number available, no reply sent\n");
			if (!res)
				res = ast_play_and_wait(chan, "vm-nonumber");
			ast_config_destroy(msg_cfg);
			return res;
		} else {
			if (find_user(NULL, vmu->context, num)) {
				struct leave_vm_options leave_options;
				char mailbox[AST_MAX_EXTENSION * 2 + 2];
				snprintf(mailbox, sizeof(mailbox), "%s@%s", num, vmu->context);

				if (option_verbose > 2)
					ast_verbose(VERBOSE_PREFIX_3 "Leaving voicemail for '%s' in context '%s'\n", num, vmu->context);
				
				memset(&leave_options, 0, sizeof(leave_options));
				leave_options.record_gain = record_gain;
				res = leave_voicemail(chan, mailbox, &leave_options);
				if (!res)
					res = 't';
				ast_config_destroy(msg_cfg);
				return res;
			} else {
				/* Sender has no mailbox, can't reply */
				if (option_verbose > 2)
					ast_verbose( VERBOSE_PREFIX_3 "No mailbox number '%s' in context '%s', no reply sent\n", num, vmu->context);
				ast_play_and_wait(chan, "vm-nobox");
				res = 't';
				ast_config_destroy(msg_cfg);
				return res;
			}
		} 
		res = 0;

		break;
	}

#ifndef IMAP_STORAGE
	ast_config_destroy(msg_cfg);

	if (!res) {
		make_file(vms->fn, sizeof(vms->fn), vms->curdir, msg);
		vms->heard[msg] = 1;
		res = wait_file(chan, vms, vms->fn);
	}
#endif
	return res;
}

static int play_record_review(struct ast_channel *chan, char *playfile, char *recordfile, int maxtime, char *fmt,
			int outsidecaller, struct ast_vm_user *vmu, int *duration, const char *unlockdir,
			signed char record_gain, struct vm_state *vms)
{
	/* Record message & let caller review or re-record it, or set options if applicable */
	int res = 0;
	int cmd = 0;
	int max_attempts = 3;
	int attempts = 0;
	int recorded = 0;
	int message_exists = 0;
	signed char zero_gain = 0;
	char *acceptdtmf = "#";
	char *canceldtmf = "";

	/* Note that urgent and private are for flagging messages as such in the future */

	/* barf if no pointer passed to store duration in */
	if (duration == NULL) {
		ast_log(LOG_WARNING, "Error play_record_review called without duration pointer\n");
		return -1;
	}

	cmd = '3';  /* Want to start by recording */

	while ((cmd >= 0) && (cmd != 't')) {
		switch (cmd) {
		case '1':
			if (!message_exists) {
				/* In this case, 1 is to record a message */
				cmd = '3';
				break;
			} else {
				/* Otherwise 1 is to save the existing message */
				if (option_verbose > 2)
					ast_verbose(VERBOSE_PREFIX_3 "Saving message as is\n");
				ast_stream_and_wait(chan, "vm-msgsaved", "");
				STORE(recordfile, vmu->mailbox, vmu->context, -1, chan, vmu, fmt, *duration, vms);
				DISPOSE(recordfile, -1);
				cmd = 't';
				return res;
			}
		case '2':
			/* Review */
			if (option_verbose > 2)
				ast_verbose(VERBOSE_PREFIX_3 "Reviewing the message\n");
			cmd = ast_stream_and_wait(chan, recordfile, AST_DIGIT_ANY);
			break;
		case '3':
			message_exists = 0;
			/* Record */
			if (recorded == 1) {
				if (option_verbose > 2)
					ast_verbose(VERBOSE_PREFIX_3 "Re-recording the message\n");
			} else {	
				if (option_verbose > 2)
					ast_verbose(VERBOSE_PREFIX_3 "Recording the message\n");
			}
			if (recorded && outsidecaller) {
				cmd = ast_play_and_wait(chan, INTRO);
				cmd = ast_play_and_wait(chan, "beep");
			}
			recorded = 1;
			/* After an attempt has been made to record message, we have to take care of INTRO and beep for incoming messages, but not for greetings */
			if (record_gain)
				ast_channel_setoption(chan, AST_OPTION_RXGAIN, &record_gain, sizeof(record_gain), 0);
			if (ast_test_flag(vmu, VM_OPERATOR))
				canceldtmf = "0";
			cmd = ast_play_and_record_full(chan, playfile, recordfile, maxtime, fmt, duration, silencethreshold, maxsilence, unlockdir, acceptdtmf, canceldtmf);
			if (record_gain)
				ast_channel_setoption(chan, AST_OPTION_RXGAIN, &zero_gain, sizeof(zero_gain), 0);
			if (cmd == -1) {
			/* User has hung up, no options to give */
				return cmd;
			}
			if (cmd == '0') {
				break;
			} else if (cmd == '*') {
				break;
			} 
#if 0			
			else if (vmu->review && (*duration < 5)) {
				/* Message is too short */
				if (option_verbose > 2)
					ast_verbose(VERBOSE_PREFIX_3 "Message too short\n");
				cmd = ast_play_and_wait(chan, "vm-tooshort");
				cmd = vm_delete(recordfile);
				break;
			}
			else if (vmu->review && (cmd == 2 && *duration < (maxsilence + 3))) {
				/* Message is all silence */
				if (option_verbose > 2)
					ast_verbose(VERBOSE_PREFIX_3 "Nothing recorded\n");
				cmd = vm_delete(recordfile);
				cmd = ast_play_and_wait(chan, "vm-nothingrecorded");
				if (!cmd)
					cmd = ast_play_and_wait(chan, "vm-speakup");
				break;
			}
#endif
			else {
				/* If all is well, a message exists */
				message_exists = 1;
				cmd = 0;
			}
			break;
		case '4':
		case '5':
		case '6':
		case '7':
		case '8':
		case '9':
		case '*':
		case '#':
			cmd = ast_play_and_wait(chan, "vm-sorry");
			break;
#if 0 
/*  XXX Commented out for the moment because of the dangers of deleting
    a message while recording (can put the message numbers out of sync) */
		case '*':
			/* Cancel recording, delete message, offer to take another message*/
			cmd = ast_play_and_wait(chan, "vm-deleted");
			cmd = vm_delete(recordfile);
			if (outsidecaller) {
				res = vm_exec(chan, NULL);
				return res;
			}
			else
				return 1;
#endif
		case '0':
			if (!ast_test_flag(vmu, VM_OPERATOR)) {
				cmd = ast_play_and_wait(chan, "vm-sorry");
				break;
			}
			if (message_exists || recorded) {
				cmd = ast_play_and_wait(chan, "vm-saveoper");
				if (!cmd)
					cmd = ast_waitfordigit(chan, 3000);
				if (cmd == '1') {
					ast_play_and_wait(chan, "vm-msgsaved");
					cmd = '0';
				} else {
					ast_play_and_wait(chan, "vm-deleted");
					DELETE(recordfile, -1, recordfile);
					cmd = '0';
				}
			}
			return cmd;
		default:
			/* If the caller is an ouside caller, and the review option is enabled,
			   allow them to review the message, but let the owner of the box review
			   their OGM's */
			if (outsidecaller && !ast_test_flag(vmu, VM_REVIEW))
				return cmd;
			if (message_exists) {
				cmd = ast_play_and_wait(chan, "vm-review");
			}
			else {
				cmd = ast_play_and_wait(chan, "vm-torerecord");
				if (!cmd)
					cmd = ast_waitfordigit(chan, 600);
			}
			
			if (!cmd && outsidecaller && ast_test_flag(vmu, VM_OPERATOR)) {
				cmd = ast_play_and_wait(chan, "vm-reachoper");
				if (!cmd)
					cmd = ast_waitfordigit(chan, 600);
			}
#if 0
			if (!cmd)
				cmd = ast_play_and_wait(chan, "vm-tocancelmsg");
#endif
			if (!cmd)
				cmd = ast_waitfordigit(chan, 6000);
			if (!cmd) {
				attempts++;
			}
			if (attempts > max_attempts) {
				cmd = 't';
			}
		}
	}
	if (outsidecaller)
		ast_play_and_wait(chan, "vm-goodbye");
	if (cmd == 't')
		cmd = 0;
	return cmd;
}

#ifdef IMAP_STORAGE

static void write_file(char *filename, char *buffer, unsigned long len)
{
	FILE *output;

	output = fopen (filename, "w");
	fwrite (buffer, len, 1, output);
	fclose (output);
}

void mm_searched(MAILSTREAM *stream, unsigned long number)
{
	struct vm_state *vms;
	char *mailbox = stream->mailbox, buf[1024] = "", *user;

	if (!(user = get_user_by_mailbox(mailbox, buf, sizeof(buf))))
		return;

	if (!(vms = get_vm_state_by_imapuser(user, 2))) {
		ast_log(LOG_ERROR, "No state found.\n");
		return;
	}
	
	if(option_debug > 2)
		ast_log(LOG_DEBUG, "saving mailbox message number %lu as message %d. Interactive set to %d\n", number, vms->vmArrayIndex, vms->interactive);

	vms->msgArray[vms->vmArrayIndex++] = number;
}


/* MM display body
 * Accepts: BODY structure pointer
 *	    prefix string
 *	    index
 */
static void display_body(BODY *body, char *pfx, long i)
{
	char tmp[MAILTMPLEN];
	char *s = tmp;
	PARAMETER *par;
	PART *part;			/* multipart doesn't have a row to itself */
	if (body->type == TYPEMULTIPART) {
		/* if not first time, extend prefix */
		if (pfx)
			sprintf (tmp, "%s%ld.", pfx, ++i);
		else
			tmp[0] = '\0';
		for (i = 0, part = body->nested.part; part; part = part->next)
			display_body (&part->body, tmp, i++);
	} else {				/* non-multipart, output oneline descriptor */
		if (!pfx)
			pfx = "";		/* dummy prefix if top level */
		sprintf (s, " %s%ld %s", pfx, ++i, body_types[body->type]);
		if (body->subtype)
			sprintf (s += strlen (s), "/%s", body->subtype);
		if (body->description)
			sprintf (s += strlen (s), " (%s)", body->description);
		if ((par = body->parameter))
			do
				sprintf (s += strlen (s), ";%s=%s", par->attribute, par->value);
			while ((par = par->next));
		if (body->id)
			sprintf (s += strlen (s), ", id = %s", body->id);
		switch (body->type) {			/* bytes or lines depending upon body type */
			case TYPEMESSAGE:	/* encapsulated message */
			case TYPETEXT:		/* plain text */
				sprintf (s += strlen (s), " (%lu lines)", body->size.lines);
				break;
			default:
				sprintf (s += strlen (s), " (%lu bytes)", body->size.bytes);
				break;
		}
		/* ast_log (LOG_NOTICE,tmp);	output this line */
		/* encapsulated message? */
		if ((body->type == TYPEMESSAGE) && !strcmp (body->subtype, "RFC822") && (body = body->nested.msg->body)) {
			if (body->type == TYPEMULTIPART)
				display_body (body, pfx, i - 1);
			else {			/* build encapsulation prefix */
				sprintf (tmp, "%s%ld.", pfx, i);
				display_body (body, tmp, (long) 0);
			}
		}
	}
}

#if 0 /*No need for this. */
/* MM status report
 * Accepts: MAIL stream
 */
static void status(MAILSTREAM *stream)
{
	unsigned long i;
	char *s, date[MAILTMPLEN];
	THREADER *thr;
	AUTHENTICATOR *auth;
	rfc822_date (date);
	ast_log (LOG_NOTICE,"%s\n",date);
	if (stream) {
		if (stream->mailbox)
			ast_log (LOG_NOTICE," %s mailbox: %s, %lu messages, %lu recent\n",
					stream->dtb->name, stream->mailbox, stream->nmsgs,stream->recent);
		else
			ast_log (LOG_NOTICE,"No mailbox is open on this stream\n");
		if (stream->user_flags[0]) {
			ast_log (LOG_NOTICE,"Keywords: %s\n", stream->user_flags[0]);
			for (i = 1; i < NUSERFLAGS && stream->user_flags[i]; ++i)
				ast_log (LOG_NOTICE,"	%s\n", stream->user_flags[i]);
		}
		if (!strcmp (stream->dtb->name, "imap")) {
			if (LEVELIMAP4rev1 (stream))
				s = "IMAP4rev1 (RFC 3501)";
			else if (LEVEL1730 (stream))
				s = "IMAP4 (RFC 1730)";
			else if (LEVELIMAP2bis (stream))
				s = "IMAP2bis";
			else if (LEVEL1176 (stream))
				s = "IMAP2 (RFC 1176)";
			else
				s = "IMAP2 (RFC 1064)";
			ast_log (LOG_NOTICE,"%s server %s\n", s, imap_host (stream));
			if (LEVELIMAP4 (stream)) {
				if ((i = (imap_cap(stream)->auth))) {
					s = "";
					ast_log (LOG_NOTICE,"Mutually-supported SASL mechanisms:\n");
					while ((auth = mail_lookup_auth (find_rightmost_bit (&i) + 1))) {
						ast_log (LOG_NOTICE,"	%s\n", auth->name);
						if (!strcmp (auth->name, "PLAIN"))
							s = "\n  [LOGIN will not be listed here if PLAIN is supported]\n";
					}
					ast_log (LOG_NOTICE,s);
				}
				ast_log (LOG_NOTICE,"Supported standard extensions:\n");
				if (LEVELACL (stream))
					ast_log (LOG_NOTICE,"	Access Control lists (RFC 2086)\n");
				if (LEVELQUOTA (stream))
					ast_log (LOG_NOTICE,"	Quotas (RFC 2087)\n");
				if (LEVELLITERALPLUS (stream))
					ast_log (LOG_NOTICE,"	Non-synchronizing literals (RFC 2088)\n");
				if (LEVELIDLE (stream))
					ast_log (LOG_NOTICE,"	IDLE unsolicited update (RFC 2177)\n");
				if (LEVELMBX_REF (stream))
					ast_log (LOG_NOTICE,"	Mailbox referrals (RFC 2193)\n");
				if (LEVELLOG_REF (stream))
					ast_log (LOG_NOTICE,"	Login referrals (RFC 2221)\n");
				if (LEVELANONYMOUS (stream))
					ast_log (LOG_NOTICE,"	Anonymous access (RFC 2245)\n");
				if (LEVELNAMESPACE (stream))
					ast_log (LOG_NOTICE,"	Multiple namespaces (RFC 2342)\n");
				if (LEVELUIDPLUS (stream))
					ast_log (LOG_NOTICE,"	Extended UID behavior (RFC 2359)\n");
				if (LEVELSTARTTLS (stream))
					ast_log (LOG_NOTICE,"	Transport Layer Security (RFC 2595)\n");
				if (LEVELLOGINDISABLED (stream))
					ast_log (LOG_NOTICE,"	LOGIN command disabled (RFC 2595)\n");
				if (LEVELID (stream))
					ast_log (LOG_NOTICE,"	Implementation identity negotiation (RFC 2971)\n");
				if (LEVELCHILDREN (stream))
					ast_log (LOG_NOTICE,"	LIST children announcement (RFC 3348)\n");
				if (LEVELMULTIAPPEND (stream))
					ast_log (LOG_NOTICE,"	Atomic multiple APPEND (RFC 3502)\n");
				if (LEVELBINARY (stream))
					ast_log (LOG_NOTICE,"	Binary body content (RFC 3516)\n");
				ast_log (LOG_NOTICE,"Supported draft extensions:\n");
				if (LEVELUNSELECT (stream))
					ast_log (LOG_NOTICE,"	Mailbox unselect\n");
				if (LEVELSASLIR (stream))
					ast_log (LOG_NOTICE,"	SASL initial client response\n");
				if (LEVELSORT (stream))
					ast_log (LOG_NOTICE,"	Server-based sorting\n");
				if (LEVELTHREAD (stream)) {
					ast_log (LOG_NOTICE,"	Server-based threading:\n");
					for (thr = imap_cap(stream)->threader; thr; thr = thr->next)
						ast_log (LOG_NOTICE,"		%s\n", thr->name);
				}
				if (LEVELSCAN (stream))
					ast_log (LOG_NOTICE,"	Mailbox text scan\n");
				if ((i = imap_cap(stream)->extlevel)) {
					ast_log (LOG_NOTICE,"Supported BODYSTRUCTURE extensions:\n");
					switch (i) {
						case BODYEXTLOC:
							ast_log (LOG_NOTICE,"	location\n");
						case BODYEXTLANG:
							ast_log (LOG_NOTICE,"	language\n");
						case BODYEXTDSP:
							ast_log (LOG_NOTICE,"	disposition\n");
						case BODYEXTMD5:
							ast_log (LOG_NOTICE,"	MD5\n");
					}
				}
			}else
				ast_log (LOG_NOTICE,"\n");
		}
	}
}
#endif

static struct ast_vm_user *find_user_realtime_imapuser(const char *imapuser)
{
	struct ast_variable *var;
	struct ast_vm_user *vmu;

	vmu = ast_calloc(1, sizeof *vmu);
	if (!vmu)
		return NULL;
	ast_set_flag(vmu, VM_ALLOCED);
	populate_defaults(vmu);

	var = ast_load_realtime("voicemail", "imapuser", imapuser, NULL);
	if (var) {
		apply_options_full(vmu, var);
		ast_variables_destroy(var);
		return vmu;
	} else {
		free(vmu);
		return NULL;
	}
}

/* Interfaces to C-client */

void mm_exists(MAILSTREAM * stream, unsigned long number)
{
	/* mail_ping will callback here if new mail! */
	if(option_debug > 3)
		ast_log (LOG_DEBUG, "Entering EXISTS callback for message %ld\n", number);
	if (number == 0) return;
	set_update(stream);
}


void mm_expunged(MAILSTREAM * stream, unsigned long number)
{
	/* mail_ping will callback here if expunged mail! */
	if(option_debug > 3)
		ast_log (LOG_DEBUG, "Entering EXPUNGE callback for message %ld\n", number);
	if (number == 0) return;
	set_update(stream);
}


void mm_flags(MAILSTREAM * stream, unsigned long number)
{
	/* mail_ping will callback here if read mail! */
	if(option_debug > 3)
		ast_log (LOG_DEBUG, "Entering FLAGS callback for message %ld\n", number);
	if (number == 0) return;
	set_update(stream);
}


void mm_notify(MAILSTREAM * stream, char *string, long errflg)
{
	mm_log (string, errflg);
}


void mm_list(MAILSTREAM * stream, int delim, char *mailbox, long attributes)
{
	if (delimiter == '\0') {
		ast_mutex_lock(&delimiter_lock);
		delimiter = delim;
		ast_mutex_unlock(&delimiter_lock);
	}
	if (option_debug > 4) {
		ast_log(LOG_DEBUG, "Delimiter set to %c and mailbox %s\n",delim, mailbox);
		if (attributes & LATT_NOINFERIORS)
			ast_log(LOG_DEBUG, "no inferiors\n");
		if (attributes & LATT_NOSELECT)
			ast_log(LOG_DEBUG, "no select\n");
		if (attributes & LATT_MARKED)
			ast_log(LOG_DEBUG, "marked\n");
		if (attributes & LATT_UNMARKED)
			ast_log(LOG_DEBUG, "unmarked\n");
	}
}


void mm_lsub(MAILSTREAM * stream, int delimiter, char *mailbox, long attributes)
{
	if (option_debug > 4) {
		ast_log(LOG_DEBUG, "Delimiter set to %c and mailbox %s\n",delimiter, mailbox);
		if (attributes & LATT_NOINFERIORS)
			ast_log(LOG_DEBUG, "no inferiors\n");
		if (attributes & LATT_NOSELECT)
			ast_log(LOG_DEBUG, "no select\n");
		if (attributes & LATT_MARKED)
			ast_log(LOG_DEBUG, "marked\n");
		if (attributes & LATT_UNMARKED)
			ast_log(LOG_DEBUG, "unmarked\n");
	}
}


void mm_status(MAILSTREAM * stream, char *mailbox, MAILSTATUS * status)
{
	ast_log (LOG_NOTICE," Mailbox %s", mailbox);
	if (status->flags & SA_MESSAGES)
		ast_log (LOG_NOTICE,", %lu messages", status->messages);
	if (status->flags & SA_RECENT)
		ast_log (LOG_NOTICE,", %lu recent", status->recent);
	if (status->flags & SA_UNSEEN)
		ast_log (LOG_NOTICE,", %lu unseen", status->unseen);
	if (status->flags & SA_UIDVALIDITY)
		ast_log (LOG_NOTICE,", %lu UID validity", status->uidvalidity);
	if (status->flags & SA_UIDNEXT)
		ast_log (LOG_NOTICE,", %lu next UID", status->uidnext);
	ast_log (LOG_NOTICE,"\n");
}


void mm_log(char *string, long errflg)
{
	switch ((short) errflg) {
		case NIL:
			if(option_debug)
				ast_log(LOG_DEBUG,"IMAP Info: %s\n", string);
			break;
		case PARSE:
		case WARN:
			ast_log (LOG_WARNING,"IMAP Warning: %s\n", string);
			break;
		case ERROR:
			ast_log (LOG_ERROR,"IMAP Error: %s\n", string);
			break;
	}
}


void mm_dlog(char *string)
{
	ast_log (LOG_NOTICE, "%s\n", string);
}


void mm_login(NETMBX * mb, char *user, char *pwd, long trial)
{
	struct ast_vm_user *vmu;

	if(option_debug > 3)
		ast_log(LOG_DEBUG, "Entering callback mm_login\n");

	ast_copy_string(user, mb->user, MAILTMPLEN);

	/* We should only do this when necessary */
	if (!ast_strlen_zero(authpassword)) {
		ast_copy_string(pwd, authpassword, MAILTMPLEN);
	} else {
		AST_LIST_TRAVERSE(&users, vmu, list) {
			if(!strcasecmp(mb->user, vmu->imapuser)) {
				ast_copy_string(pwd, vmu->imappassword, MAILTMPLEN);
				break;
			}
		}
		if (!vmu) {
			if ((vmu = find_user_realtime_imapuser(mb->user))) {
				ast_copy_string(pwd, vmu->imappassword, MAILTMPLEN);
				free_user(vmu);
			}
		}
	}
}


void mm_critical(MAILSTREAM * stream)
{
}


void mm_nocritical(MAILSTREAM * stream)
{
}


long mm_diskerror(MAILSTREAM * stream, long errcode, long serious)
{
	kill (getpid (), SIGSTOP);
	return NIL;
}


void mm_fatal(char *string)
{
	ast_log(LOG_ERROR,"IMAP access FATAL error: %s\n", string);
}

/* C-client callback to handle quota */
static void mm_parsequota(MAILSTREAM *stream, unsigned char *msg, QUOTALIST *pquota)
{
	struct vm_state *vms;
	char *mailbox = stream->mailbox, *user;
	char buf[1024] = "";
	unsigned long usage = 0, limit = 0;
	
	while (pquota) {
		usage = pquota->usage;
		limit = pquota->limit;
		pquota = pquota->next;
	}
	
	if (!(user = get_user_by_mailbox(mailbox, buf, sizeof(buf))) || !(vms = get_vm_state_by_imapuser(user, 2))) {
		ast_log(LOG_ERROR, "No state found.\n");
		return;
	}

	if (option_debug > 2)
		ast_log(LOG_DEBUG, "User %s usage is %lu, limit is %lu\n", user, usage, limit);

	vms->quota_usage = usage;
	vms->quota_limit = limit;
}

static char *get_header_by_tag(char *header, char *tag, char *buf, size_t len)
{
	char *start, *eol_pnt;
	int taglen;

	if (ast_strlen_zero(header) || ast_strlen_zero(tag))
		return NULL;

	taglen = strlen(tag) + 1;
	if (taglen < 1)
		return NULL;

	if (!(start = strstr(header, tag)))
		return NULL;

	/* Since we can be called multiple times we should clear our buffer */
	memset(buf, 0, len);

	ast_copy_string(buf, start+taglen, len);
	eol_pnt = strchr(buf,'\n');
	*eol_pnt = '\0';
	return buf;
}

static char *get_user_by_mailbox(char *mailbox, char *buf, size_t len)
{
	char *start, *quote, *eol_pnt;

	if (ast_strlen_zero(mailbox))
		return NULL;

	if (!(start = strstr(mailbox, "user=")))
		return NULL;

	ast_copy_string(buf, start+5, len);

	if (!(quote = strchr(buf, '\"'))) {
		if (!(eol_pnt = strchr(buf, '/')))
			eol_pnt = strchr(buf,'}');
		*eol_pnt = '\0';
		return buf;
	} else {
		eol_pnt = strchr(buf+1,'\"');
		*eol_pnt = '\0';
		return buf+1;
	}
}

static struct vm_state *get_vm_state_by_imapuser(char *user, int interactive)
{
	struct vmstate *vlist = NULL;

	AST_LIST_TRAVERSE(&vmstates, vlist, list) {
		if (!vlist->vms) {
			if (option_debug > 2)
				ast_log(LOG_DEBUG, "error: vms is NULL for %s\n", user);
			continue;
		}
		if (!vlist->vms->imapuser) {
			if (option_debug > 2)
				ast_log(LOG_DEBUG, "error: imapuser is NULL for %s\n", user);
			continue;
		}

		if (interactive == 2)
			return vlist->vms;
		else if (vlist->vms->interactive == interactive)
			return vlist->vms;
	}

	if(option_debug > 2)
		ast_log(LOG_DEBUG, "%s not found in vmstates\n", user);

	return NULL;
}

static struct vm_state *get_vm_state_by_mailbox(const char *mailbox, int interactive)
{ 
	struct vmstate *vlist = NULL;

	AST_LIST_TRAVERSE(&vmstates, vlist, list) {
		if (!vlist->vms) {
			if (option_debug > 2)
				ast_log(LOG_DEBUG, "error: vms is NULL for %s\n", mailbox);
			continue;
		}
		if (!vlist->vms->username) {
			if (option_debug > 2)
				ast_log(LOG_DEBUG, "error: username is NULL for %s\n", mailbox);
			continue;
		}

		if (option_debug > 2)
			ast_log(LOG_DEBUG, "comparing mailbox %s (i=%d) to vmstate mailbox %s (i=%d)\n", mailbox, interactive, vlist->vms->username, vlist->vms->interactive);
		
		if (!strcmp(vlist->vms->username,mailbox) && vlist->vms->interactive == interactive) {
			if(option_debug > 2)
				ast_log(LOG_DEBUG, "Found it!\n");
			return vlist->vms;
		}
	}

	if (option_debug > 2)
		ast_log(LOG_DEBUG, "%s not found in vmstates\n", mailbox);

	return NULL;
}

static void vmstate_insert(struct vm_state *vms) 
{
	struct vmstate *v;
	struct vm_state *altvms;

	/* If interactive, it probably already exists, and we should
	   use the one we already have since it is more up to date.
	   We can compare the username to find the duplicate */
	if (vms->interactive == 1) {
		altvms = get_vm_state_by_mailbox(vms->username,0);
		if (altvms) {
			if(option_debug > 2)
				ast_log(LOG_DEBUG, "Duplicate mailbox %s, copying message info...\n",vms->username);
			vms->newmessages = altvms->newmessages;
			vms->oldmessages = altvms->oldmessages;
			if(option_debug > 2)
				ast_log(LOG_DEBUG, "check_msgArray before memcpy\n");
			check_msgArray(vms);
			/* memcpy(vms->msgArray, altvms->msgArray, sizeof(long)*256); */
			copy_msgArray(vms, altvms);
			if(option_debug > 2)
				ast_log(LOG_DEBUG, "check_msgArray after memcpy\n");
			check_msgArray(vms);
			vms->vmArrayIndex = altvms->vmArrayIndex;
			vms->lastmsg = altvms->lastmsg;
			vms->curmsg = altvms->curmsg;
			/* get a pointer to the persistent store */
			vms->persist_vms = altvms;
			/* Reuse the mailstream? */
			vms->mailstream = altvms->mailstream;
			/* vms->mailstream = NIL; */
		}
	}

	if (!(v = ast_calloc(1, sizeof(*v))))
		return;

	if(option_debug > 2)
		ast_log(LOG_DEBUG, "Inserting vm_state for user:%s, mailbox %s\n",vms->imapuser,vms->username);

	AST_LIST_LOCK(&vmstates);
	AST_LIST_INSERT_TAIL(&vmstates, v, list);
	AST_LIST_UNLOCK(&vmstates);
}

static void vmstate_delete(struct vm_state *vms) 
{
	struct vmstate *vc = NULL;
	struct vm_state *altvms = NULL;

	/* If interactive, we should copy pertinent info
	   back to the persistent state (to make update immediate) */
	if (vms->interactive == 1 && (altvms = vms->persist_vms)) {
		if(option_debug > 2)
			ast_log(LOG_DEBUG, "Duplicate mailbox %s, copying message info...\n", vms->username);
		altvms->newmessages = vms->newmessages;
		altvms->oldmessages = vms->oldmessages;
		altvms->updated = 2;
	}
	
	if (option_debug > 2)
		ast_log(LOG_DEBUG, "Removing vm_state for user:%s, mailbox %s\n", vms->imapuser, vms->username);
	
	AST_LIST_LOCK(&vmstates);
	AST_LIST_TRAVERSE_SAFE_BEGIN(&vmstates, vc, list) {
		if (vc->vms == vms) {
			AST_LIST_REMOVE_CURRENT(&vmstates, list);
			break;
		}
	}
	AST_LIST_TRAVERSE_SAFE_END
	AST_LIST_UNLOCK(&vmstates);
	
	if (vc)
		free(vc);
	else
		ast_log(LOG_ERROR, "No vmstate found for user:%s, mailbox %s\n", vms->imapuser, vms->username);
}

static void set_update(MAILSTREAM * stream) 
{
	struct vm_state *vms;
	char *mailbox = stream->mailbox, *user;
	char buf[1024] = "";

	if (!(user = get_user_by_mailbox(mailbox, buf, sizeof(buf))) || !(vms = get_vm_state_by_imapuser(user, 0))) {
		if (user && option_debug > 2)
			ast_log(LOG_WARNING, "User %s mailbox not found for update.\n", user);
		return;
	}

	if (option_debug > 2)
		ast_log(LOG_DEBUG, "User %s mailbox set for update.\n", user);
       
	vms->updated = 2; /* Set updated flag since mailbox changed */
}

static void init_vm_state(struct vm_state *vms) 
{
	int x;
	vms->vmArrayIndex = 0;
	for (x = 0; x < 256; x++) {
		vms->msgArray[x] = 0;
	}
}

static void check_msgArray(struct vm_state *vms) 
{
	int x;
	for (x = 0; x<256; x++) {
		if (vms->msgArray[x]!=0) {
			if(option_debug)
				ast_log (LOG_DEBUG, "Item %d set to %ld\n",x,vms->msgArray[x]);
		}
	}
}

static void copy_msgArray(struct vm_state *dst, struct vm_state *src)
{
	int x;
	for (x = 0; x<256; x++) {
		dst->msgArray[x] = src->msgArray[x];
	}
}

static int save_body(BODY *body, struct vm_state *vms, char *section, char *format) 
{
	char *body_content;
	char *body_decoded;
	unsigned long len;
	unsigned long newlen;
	char filename[256];
	
	if (!body || body == NIL)
		return -1;
	display_body (body, NIL, (long) 0);
	body_content = mail_fetchbody (vms->mailstream, vms->msgArray[vms->curmsg], section, &len);
	if (body_content != NIL) {
		sprintf(filename,"%s.%s", vms->fn, format);
		/* ast_log (LOG_DEBUG,body_content); */
		body_decoded = rfc822_base64 ((unsigned char *)body_content, len, &newlen);
		write_file (filename, (char *) body_decoded, newlen);
	}
	return 0;
}

/* get delimiter via mm_list callback */
static void get_mailbox_delimiter(MAILSTREAM *stream) {
	char tmp[50];
	sprintf(tmp, "{%s}", imapserver);
	mail_list(stream, tmp, "*");
}

#endif /* IMAP_STORAGE */

/* This is a workaround so that menuselect displays a proper description
 * AST_MODULE_INFO(, , "Comedian Mail (Voicemail System)"
 */
 
AST_MODULE_INFO(ASTERISK_GPL_KEY, AST_MODFLAG_DEFAULT, tdesc,
		.load = load_module,
		.unload = unload_module,
		.reload = reload,
		);
