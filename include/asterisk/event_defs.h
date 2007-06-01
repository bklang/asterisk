/*
 * Asterisk -- An open source telephony toolkit.
 *
 * Copyright (C) 2007, Digium, Inc.
 *
 * Russell Bryant <russell@digium.com>
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
 * \author Russell Bryant <russell@digium.com>
 * \brief Generic event system
 */

#ifndef AST_EVENT_DEFS_H
#define AST_EVENT_DEFS_H

/*! \brief Event types
 * \note These values can *never* change. */
enum ast_event_type {
	/*! Reserved to provide the ability to subscribe to all events.  A specific
	    event should never have a payload of 0. */
	AST_EVENT_ALL    = 0x00,
	/*! This event type is reserved for use by third-party modules to create
	    custom events without having to modify this file. 
	    \note There are no "custom" IE types, because IEs only have to be
	    unique to the event itself, not necessarily across all events. */
	AST_EVENT_CUSTOM = 0x01,
	/*! Voicemail message waiting indication */
	AST_EVENT_MWI          = 0x02,
	/*! Someone has subscribed to events */
	AST_EVENT_SUB          = 0x03,
	/*! Someone has unsubscribed from events */
	AST_EVENT_UNSUB        = 0x04,
	/*! The state of a device has changed */
	AST_EVENT_DEVICE_STATE = 0x05,
	/*! Number of event types.  This should be the last event type + 1 */
	AST_EVENT_TOTAL        = 0x06,
};

/*! \brief Event Information Element types */
enum ast_event_ie_type {
	/*! Used to terminate the arguments to event functions */
	AST_EVENT_IE_END       = -1,

	/*! 
	 * \brief Number of new messages
	 * Used by: AST_EVENT_MWI 
	 * Payload type: UINT
	 */
	AST_EVENT_IE_NEWMSGS   = 0x01,
	/*! 
	 * \brief Number of
	 * Used by: AST_EVENT_MWI 
	 * Payload type: UINT
	 */
	AST_EVENT_IE_OLDMSGS   = 0x02,
	/*! 
	 * \brief Mailbox name \verbatim (mailbox[@context]) \endverbatim
	 * Used by: AST_EVENT_MWI 
	 * Payload type: STR
	 */
	AST_EVENT_IE_MAILBOX   = 0x03,
	/*! 
	 * \brief Unique ID
	 * Used by: AST_EVENT_SUB, AST_EVENT_UNSUB
	 * Payload type: UINT
	 */
	AST_EVENT_IE_UNIQUEID  = 0x04,
	/*! 
	 * \brief Event type 
	 * Used by: AST_EVENT_SUB, AST_EVENT_UNSUB
	 * Payload type: UINT
	 */
	AST_EVENT_IE_EVENTTYPE = 0x05,
	/*!
	 * \brief Hint that someone cares that an IE exists
	 * Used by: AST_EVENT_SUB
	 * Payload type: UINT (ast_event_ie_type)
	 */
	AST_EVENT_IE_EXISTS    = 0x06,
	/*!
	 * \brief Device Name
	 * Used by AST_EVENT_DEVICE_STATE
	 * Payload type: STR
	 */
	AST_EVENT_IE_DEVICE    = 0x07,
	/*!
	 * \brief Generic State IE
	 * Used by AST_EVENT_DEVICE_STATE
	 * Payload type: UINT
	 * The actual state values depend on the event which
	 * this IE is a part of.
	 */
	 AST_EVENT_IE_STATE    = 0x08,
};

/*!
 * \brief Payload types for event information elements
 */
enum ast_event_ie_pltype {
	/*! Just check if it exists, not the value */
	AST_EVENT_IE_PLTYPE_EXISTS,
	/*! Unsigned Integer (Can be used for signed, too ...) */
	AST_EVENT_IE_PLTYPE_UINT,
	/*! String */
	AST_EVENT_IE_PLTYPE_STR,
};

/*!
 * \brief Results for checking for subscribers
 *
 * \ref ast_event_check_subscriber()
 */
enum ast_event_subscriber_res {
	/*! No subscribers exist */
	AST_EVENT_SUB_NONE,
	/*! At least one subscriber exists */
	AST_EVENT_SUB_EXISTS,
};

struct ast_event;
struct ast_event_ie;
struct ast_event_sub;

#endif /* AST_EVENT_DEFS_H */
