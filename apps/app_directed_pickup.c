/*
 * Asterisk -- An open source telephony toolkit.
 *
 * Copyright (C) 2005, Joshua Colp
 *
 * Joshua Colp <jcolp@digium.com>
 *
 * Portions merged from app_pickupchan, which was
 * Copyright (C) 2008, Gary Cook
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
 * \brief Directed Call Pickup Support
 *
 * \author Joshua Colp <jcolp@digium.com>
 * \author Gary Cook
 *
 * \ingroup applications
 */

#include "asterisk.h"

ASTERISK_FILE_VERSION(__FILE__, "$Revision$")

#include "asterisk/file.h"
#include "asterisk/channel.h"
#include "asterisk/pbx.h"
#include "asterisk/module.h"
#include "asterisk/lock.h"
#include "asterisk/app.h"
#include "asterisk/features.h"

#define PICKUPMARK "PICKUPMARK"

/*** DOCUMENTATION
	<application name="Pickup" language="en_US">
		<synopsis>
			Directed extension call pickup.
		</synopsis>
		<syntax argsep="&amp;">
			<parameter name="ext" argsep="@" required="true">
				<argument name="extension" required="true"/>
				<argument name="context" />
			</parameter>
			<parameter name="ext2" argsep="@" multiple="true">
				<argument name="extension2" required="true"/>
				<argument name="context2"/>
			</parameter>
		</syntax>
		<description>
			<para>This application can pickup any ringing channel that is calling
			the specified <replaceable>extension</replaceable>. If no <replaceable>context</replaceable>
			is specified, the current context will be used. If you use the special string <literal>PICKUPMARK</literal>
			for the context parameter, for example 10@PICKUPMARK, this application
			tries to find a channel which has defined a <variable>PICKUPMARK</variable>
			channel variable with the same value as <replaceable>extension</replaceable>
			(in this example, <literal>10</literal>). When no parameter is specified, the application
			will pickup a channel matching the pickup group of the active channel.</para>
		</description>
	</application>
	<application name="PickupChan" language="en_US">
		<synopsis>
			Pickup a ringing channel.
		</synopsis>
		<syntax>
			<parameter name="channel" required="true" />
			<parameter name="channel2" multiple="true" />
		</syntax>
		<description>
			<para>This will pickup a specified <replaceable>channel</replaceable> if ringing.</para>
		</description>
	</application>
 ***/

static const char *app = "Pickup";
static const char *app2 = "PickupChan";
/*! \todo This application should return a result code, like PICKUPRESULT */

/* Perform actual pickup between two channels */
static int pickup_do(struct ast_channel *chan, struct ast_channel *target)
{
	int res = 0;

	ast_debug(1, "Call pickup on '%s' by '%s'\n", target->name, chan->name);

	if ((res = ast_answer(chan))) {
		ast_log(LOG_WARNING, "Unable to answer '%s'\n", chan->name);
		return -1;
	}

	if ((res = ast_queue_control(chan, AST_CONTROL_ANSWER))) {
		ast_log(LOG_WARNING, "Unable to queue answer on '%s'\n", chan->name);
		return -1;
	}

	if ((res = ast_channel_masquerade(target, chan))) {
		ast_log(LOG_WARNING, "Unable to masquerade '%s' into '%s'\n", chan->name, target->name);
		return -1;
	}

	return res;
}

/* Helper function that determines whether a channel is capable of being picked up */
static int can_pickup(struct ast_channel *chan)
{
	if (!chan->pbx && (chan->_state == AST_STATE_RINGING || chan->_state == AST_STATE_RING))
		return 1;
	else
		return 0;
}

/*! \brief Helper Function to walk through ALL channels checking NAME and STATE */
static struct ast_channel *my_ast_get_channel_by_name_locked(char *channame)
{
	struct ast_channel *chan;
	char *chkchan = alloca(strlen(channame) + 2);

	/* need to append a '-' for the comparison so we check full channel name,
	 * i.e SIP/hgc- , use a temporary variable so original stays the same for
	 * debugging.
	 */
	strcpy(chkchan, channame);
	strcat(chkchan, "-");

	for (chan = ast_walk_channel_by_name_prefix_locked(NULL, channame, strlen(channame));
		 chan;
		 chan = ast_walk_channel_by_name_prefix_locked(chan, channame, strlen(channame))) {
		if (!strncasecmp(chan->name, chkchan, strlen(chkchan)) && can_pickup(chan))
			return chan;
		ast_channel_unlock(chan);
	}
	return NULL;
}

/*! \brief Attempt to pick up specified channel named , does not use context */
static int pickup_by_channel(struct ast_channel *chan, char *pickup)
{
	int res = 0;
	struct ast_channel *target;

	if (!(target = my_ast_get_channel_by_name_locked(pickup)))
		return -1;

	/* Just check that we are not picking up the SAME as target */
	if (chan->name != target->name && chan != target) {
		res = pickup_do(chan, target);
		ast_channel_unlock(target);
	}

	return res;
}

/* Attempt to pick up specified extension with context */
static int pickup_by_exten(struct ast_channel *chan, const char *exten, const char *context)
{
	int res = -1;
	struct ast_channel *target = NULL;

	while ((target = ast_channel_walk_locked(target))) {
		if ((!strcasecmp(target->macroexten, exten) || !strcasecmp(target->exten, exten)) &&
			!strcasecmp(target->dialcontext, context) &&
			can_pickup(target)) {
			res = pickup_do(chan, target);
			ast_channel_unlock(target);
			break;
		}
		ast_channel_unlock(target);
	}

	return res;
}

/* Attempt to pick up specified mark */
static int pickup_by_mark(struct ast_channel *chan, const char *mark)
{
	int res = -1;
	const char *tmp = NULL;
	struct ast_channel *target = NULL;

	while ((target = ast_channel_walk_locked(target))) {
		if ((tmp = pbx_builtin_getvar_helper(target, PICKUPMARK)) &&
			!strcasecmp(tmp, mark) &&
			can_pickup(target)) {
			res = pickup_do(chan, target);
			ast_channel_unlock(target);
			break;
		}
		ast_channel_unlock(target);
	}

	return res;
}

/* application entry point for Pickup() */
static int pickup_exec(struct ast_channel *chan, void *data)
{
	int res = 0;
	char *tmp = ast_strdupa(data);
	char *exten = NULL, *context = NULL;

	if (ast_strlen_zero(data)) {
		res = ast_pickup_call(chan);
		return res;
	}
	
	/* Parse extension (and context if there) */
	while (!ast_strlen_zero(tmp) && (exten = strsep(&tmp, "&"))) {
		if ((context = strchr(exten, '@')))
			*context++ = '\0';
		if (!ast_strlen_zero(context) && !strcasecmp(context, PICKUPMARK)) {
			if (!pickup_by_mark(chan, exten))
				break;
		} else {
			if (!pickup_by_exten(chan, exten, !ast_strlen_zero(context) ? context : chan->context))
				break;
		}
		ast_log(LOG_NOTICE, "No target channel found for %s.\n", exten);
	}

	return res;
}

/* application entry point for PickupChan() */
static int pickupchan_exec(struct ast_channel *chan, void *data)
{
	int res = 0;
	char *tmp = ast_strdupa(data);
	char *pickup = NULL;

	if (ast_strlen_zero(data)) {
		ast_log(LOG_WARNING, "PickupChan requires an argument (channel)!\n");
		return -1;	
	}

	/* Parse channel */
	while (!ast_strlen_zero(tmp) && (pickup = strsep(&tmp, "&"))) {
		if (!strncasecmp(chan->name, pickup, strlen(pickup))) {
			ast_log(LOG_NOTICE, "Cannot pickup your own channel %s.\n", pickup);
		} else {
			if (!pickup_by_channel(chan, pickup)) {
				break;
			}
			ast_log(LOG_NOTICE, "No target channel found for %s.\n", pickup);
		}
	}

	return res;
}

static int unload_module(void)
{
	int res;

	res = ast_unregister_application(app);
	res |= ast_unregister_application(app2);

	return res;
}

static int load_module(void)
{
	int res;

	res = ast_register_application_xml(app, pickup_exec);
	res |= ast_register_application_xml(app2, pickupchan_exec);

	return res;
}

AST_MODULE_INFO_STANDARD(ASTERISK_GPL_KEY, "Directed Call Pickup Application");
