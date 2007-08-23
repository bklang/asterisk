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
 * \brief Call Parking and Pickup API 
 * Includes code and algorithms from the Zapata library.
 */

#ifndef _AST_FEATURES_H
#define _AST_FEATURES_H

#define FEATURE_MAX_LEN		11
#define FEATURE_APP_LEN		64
#define FEATURE_APP_ARGS_LEN	256
#define FEATURE_SNAME_LEN	32
#define FEATURE_EXTEN_LEN	32
#define FEATURE_MOH_LEN		80  /* same as MAX_MUSICCLASS from channel.h */

/*! \brief main call feature structure */
struct ast_call_feature {
	int feature_mask;
	char *fname;
	char sname[FEATURE_SNAME_LEN];
	char exten[FEATURE_MAX_LEN];
	char default_exten[FEATURE_MAX_LEN];
	int (*operation)(struct ast_channel *chan, struct ast_channel *peer, struct ast_bridge_config *config, char *code, int sense, void *data);
	unsigned int flags;
	char app[FEATURE_APP_LEN];		
	char app_args[FEATURE_APP_ARGS_LEN];
	char moh_class[FEATURE_MOH_LEN];
	AST_LIST_ENTRY(ast_call_feature) feature_entry;
};



/*!
 * \brief Park a call and read back parked location 
 * \param chan the channel to actually be parked
 * \param host the channel which will have the parked location read to.
 * \param timeout is a timeout in milliseconds
 * \param extout is a parameter to an int that will hold the parked location, or NULL if you want
 * Park the channel chan, and read back the parked location to the host. 
 * If the call is not picked up within a specified period of time, 
 * then the call will return to the last step that it was in 
 * (in terms of exten, priority and context)
 * \retval 0 on success.
 * \retval -1 on failure.
*/
int ast_park_call(struct ast_channel *chan, struct ast_channel *host, int timeout, int *extout);

/*! 
 * \brief Park a call via a masqueraded channel
 * \param rchan the real channel to be parked
 * \param host the channel to have the parking read to.
 * \param timeout is a timeout in milliseconds
 * \param extout is a parameter to an int that will hold the parked location, or NULL if you want
 * Masquerade the channel rchan into a new, empty channel which is then parked with ast_park_call
 * \retval 0 on success.
 * \retval -1 on failure.
*/
int ast_masq_park_call(struct ast_channel *rchan, struct ast_channel *host, int timeout, int *extout);

/*! 
 * \brief Determine system parking extension
 * \returns the call parking extension for drivers that provide special call parking help 
*/
const char *ast_parking_ext(void);

/*! \brief Determine system call pickup extension */
const char *ast_pickup_ext(void);

/*! \brief Bridge a call, optionally allowing redirection */
int ast_bridge_call(struct ast_channel *chan, struct ast_channel *peer,struct ast_bridge_config *config);

/*! \brief Pickup a call */
int ast_pickup_call(struct ast_channel *chan);

/*! \brief register new feature into feature_set 
   \param feature an ast_call_feature object which contains a keysequence
   and a callback function which is called when this keysequence is pressed
   during a call. */
void ast_register_feature(struct ast_call_feature *feature);

/*! \brief unregister feature from feature_set
    \param feature the ast_call_feature object which was registered before*/
void ast_unregister_feature(struct ast_call_feature *feature);

/*! \brief look for a call feature entry by its sname
	\param name a string ptr, should match "automon", "blindxfer", "atxfer", etc. */
struct ast_call_feature *ast_find_call_feature(const char *name);

void ast_rdlock_call_features(void);
void ast_unlock_call_features(void);

#endif /* _AST_FEATURES_H */
