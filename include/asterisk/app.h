/*
 * Asterisk -- A telephony toolkit for Linux.
 *
 * Application convenience functions, designed to give consistent
 * look and feel to asterisk apps.
 * 
 * Copyright (C) 1999, Mark Spencer
 *
 * Mark Spencer <markster@linux-support.net>
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License
 */

#ifndef _ASTERISK_APP_H
#define _ASTERISK_APP_H

#if defined(__cplusplus) || defined(c_plusplus)
extern "C" {
#endif
//! Plays a stream and gets DTMF data from a channel
/*!
 * \param c Which channel one is interacting with
 * \param prompt File to pass to ast_streamfile (the one that you wish to play)
 * \param s The location where the DTMF data will be stored
 * \param maxlen Max Length of the data
 * \param timeout Timeout length waiting for data(in milliseconds).  Set to 0 for standard timeout(six seconds), or -1 for no time out.
 *
 *  This function was designed for application programmers for situations where they need 
 *  to play a message and then get some DTMF data in response to the message.  If a digit 
 *  is pressed during playback, it will immediately break out of the message and continue
 *  execution of your code.
 */
extern int ast_app_getdata(struct ast_channel *c, char *prompt, char *s, int maxlen, int timeout);

#if defined(__cplusplus) || defined(c_plusplus)
}
#endif

#endif
