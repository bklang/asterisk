/*
 * Asterisk -- A telephony toolkit for Linux.
 *
 * General Definitions for Asterisk top level program
 * 
 * Copyright (C) 1999, Mark Spencer
 *
 * Mark Spencer <markster@linux-support.net>
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License
 */

#ifndef _ASTERISK_H
#define _ASTERISK_H

#define DEFAULT_LANGUAGE "en"

#define AST_CONFIG_DIR "/etc/asterisk"
#define AST_SOCKET		"/var/run/asterisk.ctl"
#define AST_MODULE_DIR "/usr/lib/asterisk/modules"
#define AST_SPOOL_DIR  "/var/spool/asterisk"
#define AST_VAR_DIR    "/var/lib/asterisk"
#define AST_LOG_DIR	   "/var/log/asterisk"
#define AST_AGI_DIR		"/var/lib/asterisk/agi-bin"
#define AST_KEY_DIR	"/var/lib/asterisk/keys"

#define AST_CONFIG_FILE "asterisk.conf"

#define AST_SOUNDS AST_VAR_DIR "/sounds"
#define AST_IMAGES AST_VAR_DIR "/images"

/* Provided by module.c */
extern int load_modules(void);
/* Provided by pbx.c */
extern int load_pbx(void);
/* Provided by logger.c */
extern int init_logger(void);
/* Provided by frame.c */
extern int init_framer(void);
/* Provided by logger.c */
extern int reload_logger(void);
#endif
