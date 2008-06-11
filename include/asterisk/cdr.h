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
 * \brief Call Detail Record API 
 */

#ifndef _ASTERISK_CDR_H
#define _ASTERISK_CDR_H

#include <sys/time.h>
#define AST_CDR_FLAG_KEEP_VARS			(1 << 0)
#define AST_CDR_FLAG_POSTED			(1 << 1)
#define AST_CDR_FLAG_LOCKED			(1 << 2)
#define AST_CDR_FLAG_CHILD			(1 << 3)
#define AST_CDR_FLAG_POST_DISABLED		(1 << 4)
#define AST_CDR_FLAG_POST_ENABLE                (1 << 5)

/*! \name CDR Flags */
/*@{ */
#define AST_CDR_NULL                0
#define AST_CDR_FAILED				(1 << 0)
#define AST_CDR_BUSY				(1 << 1)
#define AST_CDR_NOANSWER			(1 << 2)
#define AST_CDR_ANSWERED			(1 << 3)
/*@} */

/*! \name CDR AMA Flags */
/*@{ */
#define AST_CDR_OMIT				(1)
#define AST_CDR_BILLING				(2)
#define AST_CDR_DOCUMENTATION			(3)
/*@} */

#define AST_MAX_USER_FIELD			256
#define AST_MAX_ACCOUNT_CODE			20

/* Include channel.h after relevant declarations it will need */
#include "asterisk/channel.h"
#include "asterisk/utils.h"

/*! \brief Responsible for call detail data */
struct ast_cdr {
	/*! Caller*ID with text */
	char clid[AST_MAX_EXTENSION];
	/*! Caller*ID number */
	char src[AST_MAX_EXTENSION];		
	/*! Destination extension */
	char dst[AST_MAX_EXTENSION];		
	/*! Destination context */
	char dcontext[AST_MAX_EXTENSION];	
	
	char channel[AST_MAX_EXTENSION];
	/*! Destination channel if appropriate */
	char dstchannel[AST_MAX_EXTENSION];	
	/*! Last application if appropriate */
	char lastapp[AST_MAX_EXTENSION];	
	/*! Last application data */
	char lastdata[AST_MAX_EXTENSION];	
	
	struct timeval start;
	
	struct timeval answer;
	
	struct timeval end;
	/*! Total time in system, in seconds */
	long int duration;				
	/*! Total time call is up, in seconds */
	long int billsec;				
	/*! What happened to the call */
	long int disposition;			
	/*! What flags to use */
	long int amaflags;				
	/*! What account number to use */
	char accountcode[AST_MAX_ACCOUNT_CODE];			
	/*! flags */
	unsigned int flags;				
	/*! Unique Channel Identifier
	 * 150 = 127 (max systemname) + "-" + 10 (epoch timestamp) + "." + 10 (monotonically incrementing integer) + NULL */
	char uniqueid[150];
	/*! User field */
	char userfield[AST_MAX_USER_FIELD];

	/*! A linked list for variables */
	struct varshead varshead;

	struct ast_cdr *next;
};

void ast_cdr_getvar(struct ast_cdr *cdr, const char *name, char **ret, char *workspace, int workspacelen, int recur, int raw);
int ast_cdr_setvar(struct ast_cdr *cdr, const char *name, const char *value, int recur);
int ast_cdr_serialize_variables(struct ast_cdr *cdr, struct ast_str **buf, char delim, char sep, int recur);
void ast_cdr_free_vars(struct ast_cdr *cdr, int recur);
int ast_cdr_copy_vars(struct ast_cdr *to_cdr, struct ast_cdr *from_cdr);

typedef int (*ast_cdrbe)(struct ast_cdr *cdr);

/*! \brief Return TRUE if CDR subsystem is enabled */
int check_cdr_enabled(void);

/*! 
 * \brief Allocate a CDR record 
 * \retval a malloc'd ast_cdr structure
 * \retval NULL on error (malloc failure)
 */
struct ast_cdr *ast_cdr_alloc(void);

/*! 
 * \brief Duplicate a record 
 * \retval a malloc'd ast_cdr structure, 
 * \retval NULL on error (malloc failure)
 */
struct ast_cdr *ast_cdr_dup(struct ast_cdr *cdr);

/*! 
 * \brief Free a CDR record 
 * \param cdr ast_cdr structure to free
 * Returns nothing
 */
void ast_cdr_free(struct ast_cdr *cdr);

/*! 
 * \brief Discard and free a CDR record 
 * \param cdr ast_cdr structure to free
 * Returns nothing  -- same as free, but no checks or complaints
 */
void ast_cdr_discard(struct ast_cdr *cdr);

/*! 
 * \brief Initialize based on a channel
 * \param cdr Call Detail Record to use for channel
 * \param chan Channel to bind CDR with
 * Initializes a CDR and associates it with a particular channel
 * \return 0 by default
 */
int ast_cdr_init(struct ast_cdr *cdr, struct ast_channel *chan);

/*! 
 * \brief Initialize based on a channel 
 * \param cdr Call Detail Record to use for channel
 * \param chan Channel to bind CDR with
 * Initializes a CDR and associates it with a particular channel
 * \return 0 by default
 */
int ast_cdr_setcid(struct ast_cdr *cdr, struct ast_channel *chan);

/*! 
 * \brief Register a CDR handling engine 
 * \param name name associated with the particular CDR handler
 * \param desc description of the CDR handler
 * \param be function pointer to a CDR handler
 * Used to register a Call Detail Record handler.
 * \retval 0 on success.
 * \retval -1 on error
 */
int ast_cdr_register(const char *name, const char *desc, ast_cdrbe be);

/*! 
 * \brief Unregister a CDR handling engine 
 * \param name name of CDR handler to unregister
 * Unregisters a CDR by it's name
 */
void ast_cdr_unregister(const char *name);

/*! 
 * \brief Start a call 
 * \param cdr the cdr you wish to associate with the call
 * Starts all CDR stuff necessary for monitoring a call
 * Returns nothing
 */
void ast_cdr_start(struct ast_cdr *cdr);

/*! \brief Answer a call 
 * \param cdr the cdr you wish to associate with the call
 * Starts all CDR stuff necessary for doing CDR when answering a call
 * \note NULL argument is just fine.
 */
void ast_cdr_answer(struct ast_cdr *cdr);

/*! 
 * \brief A call wasn't answered 
 * \param cdr the cdr you wish to associate with the call
 * Marks the channel disposition as "NO ANSWER"
 */
extern void ast_cdr_noanswer(struct ast_cdr *cdr);

/*! 
 * \brief Busy a call 
 * \param cdr the cdr you wish to associate with the call
 * Returns nothing
 */
void ast_cdr_busy(struct ast_cdr *cdr);

/*! 
 * \brief Fail a call 
 * \param cdr the cdr you wish to associate with the call
 * Returns nothing
 */
void ast_cdr_failed(struct ast_cdr *cdr);

/*! 
 * \brief Save the result of the call based on the AST_CAUSE_*
 * \param cdr the cdr you wish to associate with the call
 * \param cause the AST_CAUSE_*
 * Returns nothing
 */
int ast_cdr_disposition(struct ast_cdr *cdr, int cause);
	
/*! 
 * \brief End a call
 * \param cdr the cdr you have associated the call with
 * Registers the end of call time in the cdr structure.
 * Returns nothing
 */
void ast_cdr_end(struct ast_cdr *cdr);

/*! 
 * \brief Detaches the detail record for posting (and freeing) either now or at a
 * later time in bulk with other records during batch mode operation.
 * \param cdr Which CDR to detach from the channel thread
 * Prevents the channel thread from blocking on the CDR handling
 * Returns nothing
 */
void ast_cdr_detach(struct ast_cdr *cdr);

/*! 
 * \brief Spawns (possibly) a new thread to submit a batch of CDRs to the backend engines 
 * \param shutdown Whether or not we are shutting down
 * Blocks the asterisk shutdown procedures until the CDR data is submitted.
 * Returns nothing
 */
void ast_cdr_submit_batch(int shutdown);

/*! 
 * \brief Set the destination channel, if there was one 
 * \param cdr Which cdr it's applied to
 * \param chan Channel to which dest will be
 * Sets the destination channel the CDR is applied to
 * Returns nothing
 */
void ast_cdr_setdestchan(struct ast_cdr *cdr, const char *chan);

/*! 
 * \brief Set the last executed application 
 * \param cdr which cdr to act upon
 * \param app the name of the app you wish to change it to
 * \param data the data you want in the data field of app you set it to
 * Changes the value of the last executed app
 * Returns nothing
 */
void ast_cdr_setapp(struct ast_cdr *cdr, char *app, char *data);

/*! 
 * \brief Convert a string to a detail record AMA flag 
 * \param flag string form of flag
 * Converts the string form of the flag to the binary form.
 * \return the binary form of the flag
 */
int ast_cdr_amaflags2int(const char *flag);

/*! 
 * \brief Disposition to a string 
 * \param disposition input binary form
 * Converts the binary form of a disposition to string form.
 * \return a pointer to the string form
 */
char *ast_cdr_disp2str(int disposition);

/*! 
 * \brief Reset the detail record, optionally posting it first 
 * \param cdr which cdr to act upon
 * \param flags |AST_CDR_FLAG_POSTED whether or not to post the cdr first before resetting it
 *              |AST_CDR_FLAG_LOCKED whether or not to reset locked CDR's
 */
void ast_cdr_reset(struct ast_cdr *cdr, struct ast_flags *flags);

/*! 
 * \brief Flags to a string 
 * \param flags binary flag
 * Converts binary flags to string flags
 * Returns string with flag name
 */
char *ast_cdr_flags2str(int flags);

/*! 
 * \brief Move the non-null data from the "from" cdr to the "to" cdr
 * \param to the cdr to get the goodies
 * \param from the cdr to give the goodies
 */
void ast_cdr_merge(struct ast_cdr *to, struct ast_cdr *from);

/*! \brief Set account code, will generate AMI event */
int ast_cdr_setaccount(struct ast_channel *chan, const char *account);

/*! \brief Set AMA flags for channel */
int ast_cdr_setamaflags(struct ast_channel *chan, const char *amaflags);

/*! \brief Set CDR user field for channel (stored in CDR) */
int ast_cdr_setuserfield(struct ast_channel *chan, const char *userfield);
/*! \brief Append to CDR user field for channel (stored in CDR) */
int ast_cdr_appenduserfield(struct ast_channel *chan, const char *userfield);


/*! Update CDR on a channel */
int ast_cdr_update(struct ast_channel *chan);


extern int ast_default_amaflags;

extern char ast_default_accountcode[AST_MAX_ACCOUNT_CODE];

struct ast_cdr *ast_cdr_append(struct ast_cdr *cdr, struct ast_cdr *newcdr);

/*! \brief Reload the configuration file cdr.conf and start/stop CDR scheduling thread */
int ast_cdr_engine_reload(void);

/*! \brief Load the configuration file cdr.conf and possibly start the CDR scheduling thread */
int ast_cdr_engine_init(void);

/*! Submit any remaining CDRs and prepare for shutdown */
void ast_cdr_engine_term(void);

#endif /* _ASTERISK_CDR_H */
