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
 * \brief Device state management
 *
 *
 * \author Mark Spencer <markster@digium.com> 
 *
 *	\arg \ref AstExtState
 */
/*! \page AstExtState Extension and device states in Asterisk
 *
 *	Asterisk has an internal system that reports states
 *	for an extension. By using the dialplan priority -1,
 *	also called a \b hint, a connection can be made from an
 *	extension to one or many devices. The state of the extension
 *	now depends on the combined state of the devices.
 *
 *	The device state is basically based on the current calls.
 *	If the devicestate engine can find a call from or to the
 *	device, it's in use.
 *	
 *	Some channel drivers implement a callback function for 
 *	a better level of reporting device states. The SIP channel
 *	has a complicated system for this, which is improved 
 *	by adding call limits to the configuration.
 * 
 *	Functions that want to check the status of an extension
 *	register themself as a \b watcher.
 *	Watchers in this system can subscribe either to all extensions
 *	or just a specific extensions.
 *
 *	For non-device related states, there's an API called
 *	devicestateproviders. This is an extendable system for
 *	delivering state information from outside sources or
 *	functions within Asterisk. Currently we have providers
 *	for app_meetme.c - the conference bridge - and call
 *	parking (metermaids).
 *
 *	There are manly three subscribers to extension states 
 *	within Asterisk:
 *	- AMI, the manager interface
 *	- app_queue.c - the Queue dialplan application
 *	- SIP subscriptions, a.k.a. "blinking lamps" or 
 *	  "buddy lists"
 *
 *	The CLI command "show hints" show last known state
 *
 *	\note None of these handle user states, like an IM presense
 *	system. res_jabber.c can subscribe and watch such states
 *	in jabber/xmpp based systems.
 *
 *	\section AstDevStateArch Architecture for devicestates
 *
 *	When a channel driver or asterisk app changes state for 
 *	a watched object, it alerts the core. The core queues
 *	a change. When the change is processed, there's a query
 *	sent to the channel driver/provider if there's a function
 *	to handle that, otherwise a channel walk is issued to find
 *	a channel that involves the object.
 *	
 *	The changes are queued and processed by a separate thread.
 *	This thread calls the watchers subscribing to status 
 *	changes for the object. For manager, this results 
 *	in events. For SIP, NOTIFY requests.
 *
 *	- Device states
 *		\arg \ref devicestate.c 
 *		\arg \ref devicestate.h 
 *
 *	\section AstExtStateArch Architecture for extension states
 *	
 *	Hints are connected to extension. If an extension changes state
 *	it checks the hint devices. If there is a hint, the callbacks into
 *	device states are checked. The aggregated state is set for the hint
 *	and reported back.
 *
 *	- Extension states
 *		\arg \ref enum ast_extension_states
 *		\arg \ref pbx.c 
 *		\arg \ref pbx.h 
 *	- Structures
 *		- \ref struct ast_state_cb  Callbacks for watchers
 *		- Callback ast_state_cb_type
 *		- \ref struct ast_hint
 * 	- Functions
 *		- ast_extension_state_add()
 *		- ast_extension_state_del()
 *		- ast_get_hint()
 *	
 */

#include "asterisk.h"

ASTERISK_FILE_VERSION(__FILE__, "$Revision$")

#include <sys/types.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "asterisk/channel.h"
#include "asterisk/utils.h"
#include "asterisk/lock.h"
#include "asterisk/linkedlists.h"
#include "asterisk/logger.h"
#include "asterisk/devicestate.h"
#include "asterisk/pbx.h"
#include "asterisk/app.h"
#include "asterisk/options.h"

/*! \brief Device state strings for printing */
static const char *devstatestring[] = {
	/* 0 AST_DEVICE_UNKNOWN */	"Unknown",	/*!< Valid, but unknown state */
	/* 1 AST_DEVICE_NOT_INUSE */	"Not in use",	/*!< Not used */
	/* 2 AST_DEVICE IN USE */	"In use",	/*!< In use */
	/* 3 AST_DEVICE_BUSY */		"Busy",		/*!< Busy */
	/* 4 AST_DEVICE_INVALID */	"Invalid",	/*!< Invalid - not known to Asterisk */
	/* 5 AST_DEVICE_UNAVAILABLE */	"Unavailable",	/*!< Unavailable (not registred) */
	/* 6 AST_DEVICE_RINGING */	"Ringing",	/*!< Ring, ring, ring */
	/* 7 AST_DEVICE_RINGINUSE */	"Ring+Inuse",	/*!< Ring and in use */
	/* 8 AST_DEVICE_ONHOLD */	"On Hold"	/*!< On Hold */
};

/*! \brief  A device state provider (not a channel) */
struct devstate_prov {
	char label[40];
	ast_devstate_prov_cb_type callback;
	AST_RWLIST_ENTRY(devstate_prov) list;
};

/*! \brief A list of providers */
static AST_RWLIST_HEAD_STATIC(devstate_provs, devstate_prov);

/*! \brief  A device state watcher (callback) */
struct devstate_cb {
	void *data;
	ast_devstate_cb_type callback;	/*!< Where to report when state changes */
	AST_RWLIST_ENTRY(devstate_cb) list;
};

/*! \brief A device state watcher list */
static AST_RWLIST_HEAD_STATIC(devstate_cbs, devstate_cb);

struct state_change {
	AST_LIST_ENTRY(state_change) list;
	char device[1];
};

/*! \brief The state change queue. State changes are queued
	for processing by a separate thread */
static AST_LIST_HEAD_STATIC(state_changes, state_change);

/*! \brief The device state change notification thread */
static pthread_t change_thread = AST_PTHREADT_NULL;

/*! \brief Flag for the queue */
static ast_cond_t change_pending;

/* Forward declarations */
static int getproviderstate(const char *provider, const char *address);

/*! \brief Find devicestate as text message for output */
const char *devstate2str(enum ast_device_state devstate) 
{
	return devstatestring[devstate];
}

const char *ast_devstate_str(enum ast_device_state state)
{
	const char *res = "UNKNOWN";

	switch (state) {
	case AST_DEVICE_UNKNOWN:
		break;
	case AST_DEVICE_NOT_INUSE:
		res = "NOT_INUSE";
		break;
	case AST_DEVICE_INUSE:
		res = "INUSE";
		break;
	case AST_DEVICE_BUSY:
		res = "BUSY";
		break;
	case AST_DEVICE_INVALID:
		res = "INVALID";
		break;
	case AST_DEVICE_UNAVAILABLE:
		res = "UNAVAILABLE";
		break;
	case AST_DEVICE_RINGING:
		res = "RINGING";
		break;
	case AST_DEVICE_RINGINUSE:
		res = "RINGINUSE";
		break;
	case AST_DEVICE_ONHOLD:
		res = "ONHOLD";
		break;
	}

	return res;
}

enum ast_device_state ast_devstate_val(const char *val)
{
	if (!strcasecmp(val, "NOT_INUSE"))
		return AST_DEVICE_NOT_INUSE;
	else if (!strcasecmp(val, "INUSE"))
		return AST_DEVICE_INUSE;
	else if (!strcasecmp(val, "BUSY"))
		return AST_DEVICE_BUSY;
	else if (!strcasecmp(val, "INVALID"))
		return AST_DEVICE_INVALID;
	else if (!strcasecmp(val, "UNAVAILABLE"))
		return AST_DEVICE_UNAVAILABLE;
	else if (!strcasecmp(val, "RINGING"))
		return AST_DEVICE_RINGING;
	else if (!strcasecmp(val, "RINGINUSE"))
		return AST_DEVICE_RINGINUSE;
	else if (!strcasecmp(val, "ONHOLD"))
		return AST_DEVICE_ONHOLD;

	return AST_DEVICE_UNKNOWN;
}

/*! \brief Find out if device is active in a call or not 
	\note find channels with the device's name in it
	This function is only used for channels that does not implement 
	devicestate natively
*/
enum ast_device_state ast_parse_device_state(const char *device)
{
	struct ast_channel *chan;
	char match[AST_CHANNEL_NAME];
	enum ast_device_state res;

	ast_copy_string(match, device, sizeof(match)-1);
	strcat(match, "-");
	chan = ast_get_channel_by_name_prefix_locked(match, strlen(match));

	if (!chan)
		return AST_DEVICE_UNKNOWN;

	if (chan->_state == AST_STATE_RINGING)
		res = AST_DEVICE_RINGING;
	else
		res = AST_DEVICE_INUSE;
	
	ast_channel_unlock(chan);

	return res;
}

/*! \brief Check device state through channel specific function or generic function */
enum ast_device_state ast_device_state(const char *device)
{
	char *buf;
	char *number;
	const struct ast_channel_tech *chan_tech;
	enum ast_device_state res = AST_DEVICE_UNKNOWN;
	/*! \brief Channel driver that provides device state */
	char *tech;
	/*! \brief Another provider of device state */
	char *provider = NULL;
	
	buf = ast_strdupa(device);
	tech = strsep(&buf, "/");
	if (!(number = buf)) {
		if (!(provider = strsep(&tech, ":")))
			return AST_DEVICE_INVALID;
		/* We have a provider */
		number = tech;
		tech = NULL;
	}

	if (provider)  {
		if (option_debug > 2)
			ast_log(LOG_DEBUG, "Checking if I can find provider for \"%s\" - number: %s\n", provider, number);
		return getproviderstate(provider, number);
	}

	if (option_debug > 3)
		ast_log(LOG_DEBUG, "No provider found, checking channel drivers for %s - %s\n", tech, number);

	if (!(chan_tech = ast_get_channel_tech(tech)))
		return AST_DEVICE_INVALID;

	if (!(chan_tech->devicestate)) /* Does the channel driver support device state notification? */
		return ast_parse_device_state(device); /* No, try the generic function */

	res = chan_tech->devicestate(number);

	if (res != AST_DEVICE_UNKNOWN)
		return res;

	res = ast_parse_device_state(device);

	if (res == AST_DEVICE_UNKNOWN)
		return AST_DEVICE_NOT_INUSE;

	return res;
}

/*! \brief Add device state provider */
int ast_devstate_prov_add(const char *label, ast_devstate_prov_cb_type callback)
{
	struct devstate_prov *devprov;

	if (!callback || !(devprov = ast_calloc(1, sizeof(*devprov))))
		return -1;

	devprov->callback = callback;
	ast_copy_string(devprov->label, label, sizeof(devprov->label));

	AST_RWLIST_WRLOCK(&devstate_provs);
	AST_RWLIST_INSERT_HEAD(&devstate_provs, devprov, list);
	AST_RWLIST_UNLOCK(&devstate_provs);

	return 0;
}

/*! \brief Remove device state provider */
int ast_devstate_prov_del(const char *label)
{
	struct devstate_prov *devcb;
	int res = -1;

	AST_RWLIST_WRLOCK(&devstate_provs);
	AST_RWLIST_TRAVERSE_SAFE_BEGIN(&devstate_provs, devcb, list) {
		if (!strcasecmp(devcb->label, label)) {
			AST_RWLIST_REMOVE_CURRENT(&devstate_provs, list);
			free(devcb);
			res = 0;
			break;
		}
	}
	AST_RWLIST_TRAVERSE_SAFE_END;
	AST_RWLIST_UNLOCK(&devstate_provs);

	return res;
}

/*! \brief Get provider device state */
static int getproviderstate(const char *provider, const char *address)
{
	struct devstate_prov *devprov;
	int res = AST_DEVICE_INVALID;


	AST_RWLIST_RDLOCK(&devstate_provs);
	AST_RWLIST_TRAVERSE(&devstate_provs, devprov, list) {
		if (option_debug > 4)
			ast_log(LOG_DEBUG, "Checking provider %s with %s\n", devprov->label, provider);

		if (!strcasecmp(devprov->label, provider)) {
			res = devprov->callback(address);
			break;
		}
	}
	AST_RWLIST_UNLOCK(&devstate_provs);
	return res;
}

/*! \brief Add device state watcher */
int ast_devstate_add(ast_devstate_cb_type callback, void *data)
{
	struct devstate_cb *devcb;

	if (!callback || !(devcb = ast_calloc(1, sizeof(*devcb))))
		return -1;

	devcb->data = data;
	devcb->callback = callback;

	AST_RWLIST_WRLOCK(&devstate_cbs);
	AST_RWLIST_INSERT_HEAD(&devstate_cbs, devcb, list);
	AST_RWLIST_UNLOCK(&devstate_cbs);

	return 0;
}

/*! \brief Remove device state watcher */
void ast_devstate_del(ast_devstate_cb_type callback, void *data)
{
	struct devstate_cb *devcb;

	AST_RWLIST_WRLOCK(&devstate_cbs);
	AST_RWLIST_TRAVERSE_SAFE_BEGIN(&devstate_cbs, devcb, list) {
		if ((devcb->callback == callback) && (devcb->data == data)) {
			AST_RWLIST_REMOVE_CURRENT(&devstate_cbs, list);
			free(devcb);
			break;
		}
	}
	AST_RWLIST_TRAVERSE_SAFE_END;
	AST_RWLIST_UNLOCK(&devstate_cbs);
}

/*! \brief Notify callback watchers of change, and notify PBX core for hint updates
	Normally executed within a separate thread
*/
static void do_state_change(const char *device)
{
	int state;
	struct devstate_cb *devcb;

	state = ast_device_state(device);
	if (option_debug > 2)
		ast_log(LOG_DEBUG, "Changing state for %s - state %d (%s)\n", device, state, devstate2str(state));

	AST_RWLIST_RDLOCK(&devstate_cbs);
	AST_RWLIST_TRAVERSE(&devstate_cbs, devcb, list)
		devcb->callback(device, state, devcb->data);
	AST_RWLIST_UNLOCK(&devstate_cbs);

	ast_hint_state_changed(device);
}

static int __ast_device_state_changed_literal(char *buf)
{
	char *device;
	struct state_change *change;

	if (option_debug > 2)
		ast_log(LOG_DEBUG, "Notification of state change to be queued on device/channel %s\n", buf);

	device = buf;

	if (!strncasecmp(device, "Zap", 3)) {
		char *tmp = strrchr(device, '-');
		if (tmp)
			*tmp = '\0';
	}

	if (change_thread == AST_PTHREADT_NULL || !(change = ast_calloc(1, sizeof(*change) + strlen(device)))) {
		/* we could not allocate a change struct, or */
		/* there is no background thread, so process the change now */
		do_state_change(device);
	} else {
		/* queue the change */
		strcpy(change->device, device);
		AST_LIST_LOCK(&state_changes);
		AST_LIST_INSERT_TAIL(&state_changes, change, list);
		ast_cond_signal(&change_pending);
		AST_LIST_UNLOCK(&state_changes);
	}

	return 1;
}

int ast_device_state_changed_literal(const char *dev)
{
	char *buf;
	buf = ast_strdupa(dev);
	return __ast_device_state_changed_literal(buf);
}

/*! \brief Accept change notification, add it to change queue */
int ast_device_state_changed(const char *fmt, ...) 
{
	char buf[AST_MAX_EXTENSION];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	return __ast_device_state_changed_literal(buf);
}

/*! \brief Go through the dev state change queue and update changes in the dev state thread */
static void *do_devstate_changes(void *data)
{
	struct state_change *next, *current;

	for (;;) {
		/* This basically pops off any state change entries, resets the list back to NULL, unlocks, and processes each state change */
		AST_LIST_LOCK(&state_changes);
		if (AST_LIST_EMPTY(&state_changes))
			ast_cond_wait(&change_pending, &state_changes.lock);
		next = AST_LIST_FIRST(&state_changes);
		AST_LIST_HEAD_INIT_NOLOCK(&state_changes);
		AST_LIST_UNLOCK(&state_changes);

		/* Process each state change */
		while ((current = next)) {
			next = AST_LIST_NEXT(current, list);
			do_state_change(current->device);
			free(current);
		}
	}

	return NULL;
}

/*! \brief Initialize the device state engine in separate thread */
int ast_device_state_engine_init(void)
{
	ast_cond_init(&change_pending, NULL);
	if (ast_pthread_create_background(&change_thread, NULL, do_devstate_changes, NULL) < 0) {
		ast_log(LOG_ERROR, "Unable to start device state change thread.\n");
		return -1;
	}

	return 0;
}
