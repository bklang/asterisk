/*
 * Asterisk -- A telephony toolkit for Linux.
 *
 * General Asterisk channel definitions.
 * 
 * Copyright (C) 1999, Mark Spencer
 *
 * Mark Spencer <markster@linux-support.net>
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License
 */

#ifndef _ASTERISK_CHANNEL_H
#define _ASTERISK_CHANNEL_H

#include <asterisk/frame.h>
#include <asterisk/sched.h>
#include <setjmp.h>
#include <pthread.h>

#if defined(__cplusplus) || defined(c_plusplus)
extern "C" {
#endif

#include <pthread.h>

#ifdef DEBUG_THREADS

#define TRIES 50

#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>

struct mutex_info {
	pthread_mutex_t *mutex;
	char *file;
	int lineno;
	char *func;
	struct mutex_info *next;
};

static inline int __ast_pthread_mutex_lock(char *filename, int lineno, char *func, pthread_mutex_t *t) {
	int res;
	int tries = TRIES;
	do {
		res = pthread_mutex_trylock(t);
		/* If we can't run, yield */
		if (res) {
			sched_yield();
			usleep(1);
		}
	} while(res && tries--);
	if (res) {
		fprintf(stderr, "%s line %d (%s): Error obtaining mutex: %s\n", 
				filename, lineno, func, strerror(res));
		res = pthread_mutex_lock(t);
		fprintf(stderr, "%s line %d (%s): Got it eventually...\n",
				filename, lineno, func);
	}
	return res;
}

#define ast_pthread_mutex_lock(a) __ast_pthread_mutex_lock(__FILE__, __LINE__, __PRETTY_FUNCTION__, a)

static inline int __ast_pthread_mutex_unlock(char *filename, int lineno, char *func, pthread_mutex_t *t) {
	int res;
	res = pthread_mutex_unlock(t);
	if (res) 
		fprintf(stderr, "%s line %d (%s): Error releasing mutex: %s\n", 
				filename, lineno, func, strerror(res));
	return res;
}
#define ast_pthread_mutex_unlock(a) __ast_pthread_mutex_unlock(__FILE__, __LINE__, __PRETTY_FUNCTION__, a)
#else
#define ast_pthread_mutex_lock pthread_mutex_lock
#define ast_pthread_mutex_unlock pthread_mutex_unlock
#endif

//! Max length of an extension
#define AST_MAX_EXTENSION 80

#include <asterisk/cdr.h>


#define AST_CHANNEL_NAME 80
#define AST_CHANNEL_MAX_STACK 32

#define MAX_LANGUAGE 20


#define AST_MAX_FDS 4

//! Main Channel structure associated with a channel.
/*! 
 * This is the side of it mostly used by the pbx and call management.
 */
struct ast_channel {
	/*! ASCII Description of channel name */
	char name[AST_CHANNEL_NAME];		
	/*! Language requested */
	char language[MAX_LANGUAGE];		
	/*! Type of channel */
	char *type;				
	/*! File descriptor for channel -- Drivers will poll on these file descriptors, so at least one must be non -1.  */
	int fds[AST_MAX_FDS];			
						   
	/*! Who are we bridged to, if we're bridged */
	struct ast_channel *bridge;		
	/*! Channel that will masquerade as us */
	struct ast_channel *masq;		
	/*! Who we are masquerading as */
	struct ast_channel *masqr;		
	/*! Call Detail Record Flags */
	int cdrflags;										   
	/*! Whether or not we're blocking */
	int blocking;				
	/*! Whether or not we have been hung up */
	int softhangup;				
	/*! Non-zero if this is a zombie channel */
	int zombie;					
	/*! Non-zero, set to actual time when channel is to be hung up */
	time_t	whentohangup;
	/*! If anyone is blocking, this is them */
	pthread_t blocker;			
	/*! Lock, can be used to lock a channel for some operations */
	pthread_mutex_t lock;			
	/*! Procedure causing blocking */
	char *blockproc;			

	/*! Current application */
	char *appl;				
	/*! Data passed to current application */
	char *data;				
	
	/*! Has an exception been detected */
	int exception;				
	/*! Which fd had an event detected on */
	int fdno;				
	/*! Schedule context */
	struct sched_context *sched;		
	/*! For streaming playback, the schedule ID */
	int streamid;				
	/*! Stream itself. */
	struct ast_filestream *stream;		
	/*! Original writer format */
	int oldwriteformat;			


	/*! State of line */
	int state;				
	/*! Number of rings so far */
	int rings;				
	/*! Current level of application */
	int stack;				


	/*! Kinds of data this channel can natively handle */
	int nativeformats;			
	/*! Requested read format */
	int readformat;				
	/*! Requested write format */
	int writeformat;			

	
	/*! Malloc'd Dialed Number Identifier */
	char *dnid;				
	/*! Malloc'd Caller ID */
	char *callerid;				
	/*! Malloc'd ANI */
	char *ani;			

	
	/*! Current extension context */
	char context[AST_MAX_EXTENSION];	
	/*! Current extension number */
	char exten[AST_MAX_EXTENSION];		
	/* Current extension priority */
	int priority;						
	/*! Application information -- see assigned numbers */
	void *app[AST_CHANNEL_MAX_STACK];	
	/*! Any/all queued DTMF characters */
	char dtmfq[AST_MAX_EXTENSION];		
	/*! Are DTMF digits being deferred */
	int deferdtmf;				
	/*! DTMF frame */
	struct ast_frame dtmff;			
	/*! Private channel implementation details */
	struct ast_channel_pvt *pvt;

						
	/*! Jump buffer used for returning from applications */
	jmp_buf jmp[AST_CHANNEL_MAX_STACK];	

	struct ast_pbx *pbx;
	/*! Set BEFORE PBX is started to determine AMA flags */
	int 	amaflags;			
	/*! Account code for billing */
	char 	accountcode[20];		
	/*! Call Detail Record */
	struct ast_cdr *cdr;			
	/*! Whether or not ADSI is detected on CPE */
	int	adsicpe;
	/*! Where to forward to if asked to dial on this interface */
	char call_forward[AST_MAX_EXTENSION];
	/*! For easy linking */
	struct ast_channel *next;		

};

#define AST_CDR_TRANSFER	(1 << 0)
#define AST_CDR_FORWARD		(1 << 1)
#define AST_CDR_CALLWAIT	(1 << 2)
#define AST_CDR_CONFERENCE	(1 << 3)

#define AST_ADSI_UNKNOWN	(0)
#define AST_ADSI_AVAILABLE	(1)
#define AST_ADSI_UNAVAILABLE	(2)
#define AST_ADSI_OFFHOOKONLY	(3)

/* Bits 0-15 of state are reserved for the state (up/down) of the line */
/*! Channel is down and available */
#define AST_STATE_DOWN		0		
/*! Channel is down, but reserved */
#define AST_STATE_RESERVED	1		
/*! Channel is off hook */
#define AST_STATE_OFFHOOK	2		
/*! Digits (or equivalent) have been dialed */
#define AST_STATE_DIALING	3		
/*! Line is ringing */
#define AST_STATE_RING		4		
/*! Remote end is ringing */
#define AST_STATE_RINGING	5		
/*! Line is up */
#define AST_STATE_UP		6		
/*! Line is busy */
#define AST_STATE_BUSY  	7		

/* Bits 16-32 of state are reserved for flags */
/*! Do not transmit voice data */
#define AST_STATE_MUTE		(1 << 16)	

//! Requests a channel
/*! 
 * \param type type of channel to request
 * \param format requested channel format
 * \param data data to pass to the channel requester
 * Request a channel of a given type, with data as optional information used 
 * by the low level module
 * Returns an ast_channel on success, NULL on failure.
 */
struct ast_channel *ast_request(char *type, int format, void *data);

//! Registers a channel
/*! 
 * \param type type of channel you are registering
 * \param description short description of the channel
 * \param capabilities a bit mask of the capabilities of the channel
 * \param requester a function pointer that properly responds to a call.  See one of the channel drivers for details.
 * Called by a channel module to register the kind of channels it supports.
 * It supplies a brief type, a longer, but still short description, and a
 * routine that creates a channel
 * Returns 0 on success, -1 on failure.
 */
int ast_channel_register(char *type, char *description, int capabilities, 
			struct ast_channel* (*requester)(char *type, int format, void *data));

//! Unregister a channel class
/*
 * \param type the character string that corresponds to the channel you wish to unregister
 * Basically just unregisters the channel with the asterisk channel system
 * No return value.
 */
void ast_channel_unregister(char *type);

//! Hang up a channel 
/*! 
 * \param chan channel to hang up
 * This function performs a hard hangup on a channel.  Unlike the soft-hangup, this function
 * performs all stream stopping, etc, on the channel that needs to end.
 * chan is no longer valid after this call.
 * Returns 0 on success, -1 on failure.
 */
int ast_hangup(struct ast_channel *chan);

//! Softly hangup up a channel
/*! 
 * \param chan channel to be soft-hung-up
 * Call the protocol layer, but don't destroy the channel structure (use this if you are trying to
 * safely hangup a channel managed by another thread.
 * Returns 0 regardless
 */
int ast_softhangup(struct ast_channel *chan);

//! Check to see if a channel is needing hang up
/*! 
 * \param chan channel on which to check for hang up
 * This function determines if the channel is being requested to be hung up.
 * Returns 0 if not, or 1 if hang up is requested (including time-out).
 */
int ast_check_hangup(struct ast_channel *chan);

//! Set when to hang a channel up
/*! 
 * \param chan channel on which to check for hang up
 * \param offset offset in seconds from current time of when to hang up
 * This function sets the absolute time out on a channel (when to hang up).
 */
void ast_channel_setwhentohangup(struct ast_channel *chan, time_t offset);

//! Answer a ringing call
/*!
 * \param chan channel to answer
 * This function answers a channel and handles all necessary call
 * setup functions.
 * Returns 0 on success, -1 on failure
 */
int ast_answer(struct ast_channel *chan);

//! Make a call
/*! 
 * \param chan which channel to make the call on
 * \param addr destination of the call
 * \param timeout time to wait on for connect
 * Place a call, take no longer than timeout ms.  Returns -1 on failure, 
   0 on not enough time (does not auto matically stop ringing), and  
   the number of seconds the connect took otherwise.
   Returns 0 on success, -1 on failure
   */
int ast_call(struct ast_channel *chan, char *addr, int timeout);

//! Indicates condition of channel
/*! 
 * \param chan channel to change the indication
 * \param condition which condition to indicate on the channel
 * Indicate a condition such as AST_CONTROL_BUSY, AST_CONTROL_RINGING, or AST_CONTROL_CONGESTION on a channel
 * Returns 0 on success, -1 on failure
 */
int ast_indicate(struct ast_channel *chan, int condition);

/* Misc stuff */

//! Wait for input on a channel
/*! 
 * \param chan channel to wait on
 * \param ms length of time to wait on the channel
 * Wait for input on a channel for a given # of milliseconds (<0 for indefinite). 
  Returns < 0 on  failure, 0 if nothing ever arrived, and the # of ms remaining otherwise */
int ast_waitfor(struct ast_channel *chan, int ms);

//! Waits for activity on a group of channels
/*! 
 * \param chan an array of pointers to channels
 * \param n number of channels that are to be waited upon
 * \param fds an array of fds to wait upon
 * \param nfds the number of fds to wait upon
 * \param exception exception flag
 * \param outfd fd that had activity on it
 * \param ms how long the wait was
 * Big momma function here.  Wait for activity on any of the n channels, or any of the nfds
   file descriptors.  Returns the channel with activity, or NULL on error or if an FD
   came first.  If the FD came first, it will be returned in outfd, otherwise, outfd
   will be -1 */
struct ast_channel *ast_waitfor_nandfds(struct ast_channel **chan, int n, int *fds, int nfds, int *exception, int *outfd, int *ms);

//! Waits for input on a group of channels
/*! Wait for input on an array of channels for a given # of milliseconds. Return channel
   with activity, or NULL if none has activity.  time "ms" is modified in-place, if applicable */
struct ast_channel *ast_waitfor_n(struct ast_channel **chan, int n, int *ms);

//! Waits for input on an fd
/*! This version works on fd's only.  Be careful with it. */
int ast_waitfor_n_fd(int *fds, int n, int *ms, int *exception);


//! Reads a frame
/*! 
 * \param chan channel to read a frame from
 * Read a frame.  Returns a frame, or NULL on error.  If it returns NULL, you
   best just stop reading frames and assume the channel has been
   disconnected. */
struct ast_frame *ast_read(struct ast_channel *chan);

//! Write a frame to a channel
/*!
 * \param chan destination channel of the frame
 * \param frame frame that will be written
 * This function writes the given frame to the indicated channel.
 * It returns 0 on success, -1 on failure.
 */
int ast_write(struct ast_channel *chan, struct ast_frame *frame);

//! Sets read format on channel chan
/*! 
 * \param chan channel to change
 * \param format format to change to
 * Set read format for channel to whichever component of "format" is best. 
 * Returns 0 on success, -1 on failure
 */
int ast_set_read_format(struct ast_channel *chan, int format);

//! Sets write format on channel chan
/*! 
 * \param chan channel to change
 * \param format new format for writing
 * Set write format for channel to whichever compoent of "format" is best. 
 * Returns 0 on success, -1 on failure
 */
int ast_set_write_format(struct ast_channel *chan, int format);

//! Sends text to a channel
/*! 
 * \param chan channel to act upon
 * \param text string of text to send on the channel
 * Write text to a display on a channel
 * Returns 0 on success, -1 on failure
 */
int ast_sendtext(struct ast_channel *chan, char *text);

//! Receives a text character from a channel
/*! 
 * \param chan channel to act upon
 * \param timeout timeout in milliseconds (0 for infinite wait)
 * Read a char of text from a channel
 * Returns 0 on success, -1 on failure
 */
int ast_recvchar(struct ast_channel *chan, int timeout);

//! Browse channels in use
/*! 
 * \param prev where you want to start in the channel list
 * Browse the channels currently in use 
 * Returns the next channel in the list, NULL on end.
 */
struct ast_channel *ast_channel_walk(struct ast_channel *prev);

//! Waits for a digit
/*! 
 * \param c channel to wait for a digit on
 * \param ms how many milliseconds to wait
 * Wait for a digit.  Returns <0 on error, 0 on no entry, and the digit on success. */
char ast_waitfordigit(struct ast_channel *c, int ms);

//! Reads multiple digits
/*! 
 * \param c channel to read from
 * \param s string to read in to.  Must be at least the size of your length
 * \param len how many digits to read (maximum)
 * \param timeout how long to timeout between digits
 * \param rtimeout timeout to wait on the first digit
 * \param enders digits to end the string
 * Read in a digit string "s", max length "len", maximum timeout between 
   digits "timeout" (-1 for none), terminated by anything in "enders".  Give them rtimeout
   for the first digit.  Returns 0 on normal return, or 1 on a timeout.  In the case of
   a timeout, any digits that were read before the timeout will still be available in s.  */
int ast_readstring(struct ast_channel *c, char *s, int len, int timeout, int rtimeout, char *enders);

/*! Report DTMF on channel 0 */
#define AST_BRIDGE_DTMF_CHANNEL_0		(1 << 0)		
/*! Report DTMF on channel 1 */
#define AST_BRIDGE_DTMF_CHANNEL_1		(1 << 1)		
/*! Return all voice frames on channel 0 */
#define AST_BRIDGE_REC_CHANNEL_0		(1 << 2)		
/*! Return all voice frames on channel 1 */
#define AST_BRIDGE_REC_CHANNEL_1		(1 << 3)		
/*! Ignore all signal frames except NULL */
#define AST_BRIDGE_IGNORE_SIGS			(1 << 4)		


//! Makes two channel formats compatible
/*! 
 * \param c0 first channel to make compatible
 * \param c1 other channel to make compatible
 * Set two channels to compatible formats -- call before ast_channel_bridge in general .  Returns 0 on success
   and -1 if it could not be done */
int ast_channel_make_compatible(struct ast_channel *c0, struct ast_channel *c1);

//! Bridge two channels together
/*! 
 * \param c0 first channel to bridge
 * \param c1 second channel to bridge
 * \param flags for the channels
 * \param fo destination frame(?)
 * \param rc destination channel(?)
 * Bridge two channels (c0 and c1) together.  If an important frame occurs, we return that frame in
   *rf (remember, it could be NULL) and which channel (0 or 1) in rc */
int ast_channel_bridge(struct ast_channel *c0, struct ast_channel *c1, int flags, struct ast_frame **fo, struct ast_channel **rc);

//! Weird function made for call transfers
/*! 
 * \param original channel to make a copy of
 * \param clone copy of the original channel
 * This is a very strange and freaky function used primarily for transfer.  Suppose that
   "original" and "clone" are two channels in random situations.  This function takes
   the guts out of "clone" and puts them into the "original" channel, then alerts the
   channel driver of the change, asking it to fixup any private information (like the
   p->owner pointer) that is affected by the change.  The physical layer of the original
   channel is hung up.  */
int ast_channel_masquerade(struct ast_channel *original, struct ast_channel *clone);

//! Gives the string form of a given state
/*! 
 * \param state state to get the name of
 * Give a name to a state 
 * Pretty self explanatory.
 * Returns the text form of the binary state given
 */
char *ast_state2str(int state);

/* Options: Some low-level drivers may implement "options" allowing fine tuning of the
   low level channel.  See frame.h for options.  Note that many channel drivers may support
   none or a subset of those features, and you should not count on this if you want your
   asterisk application to be portable.  They're mainly useful for tweaking performance */

//! Sets an option on a channel
/*! 
 * \param channel channel to set options on
 * \param option option to change
 * \param data data specific to option
 * \param datalen length of the data
 * \param block blocking or not
 * Set an option on a channel (see frame.h), optionally blocking awaiting the reply 
 * Returns 0 on success and -1 on failure
 */
int ast_channel_setoption(struct ast_channel *channel, int option, void *data, int datalen, int block);

//! Checks the value of an option
/*! 
 * Query the value of an option, optionally blocking until a reply is received
 * Works similarly to setoption except only reads the options.
 */
struct ast_frame *ast_channel_queryoption(struct ast_channel *channel, int option, void *data, int *datalen, int block);

//! Checks for HTML support on a channel
/*! Returns 0 if channel does not support HTML or non-zero if it does */
int ast_channel_supports_html(struct ast_channel *channel);

//! Sends HTML on given channel
/*! Send HTML or URL on link.  Returns 0 on success or -1 on failure */
int ast_channel_sendhtml(struct ast_channel *channel, int subclass, char *data, int datalen);

//! Sends a URL on a given link
/*! Send URL on link.  Returns 0 on success or -1 on failure */
int ast_channel_sendurl(struct ast_channel *channel, char *url);

//! Defers DTMF
/*! Defer DTMF so that you only read things like hangups and audio.  Returns
   non-zero if channel was already DTMF-deferred or 0 if channel is just now
   being DTMF-deferred */
int ast_channel_defer_dtmf(struct ast_channel *chan);

//! Undeos a defer
/*! Undo defer.  ast_read will return any dtmf characters that were queued */
void ast_channel_undefer_dtmf(struct ast_channel *chan);

#ifdef DO_CRASH
#define CRASH do { *((int *)0) = 0; } while(0)
#else
#define CRASH do { } while(0)
#endif

#define CHECK_BLOCKING(c) { 	 \
							if ((c)->blocking) {\
								ast_log(LOG_WARNING, "Thread %ld Blocking '%s', already blocked by thread %ld in procedure %s\n", pthread_self(), (c)->name, (c)->blocker, (c)->blockproc); \
								CRASH; \
							} else { \
								(c)->blocker = pthread_self(); \
								(c)->blockproc = __PRETTY_FUNCTION__; \
									c->blocking = -1; \
									} }

#if defined(__cplusplus) || defined(c_plusplus)
}
#endif


#endif
