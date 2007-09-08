/*
 * Asterisk -- An open source telephony toolkit.
 *
 * Copyright (C) 2005, Anthony Minessale II
 *
 * Anthony Minessale <anthmct@yahoo.com>
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
 * \brief A machine to gather up arbitrary frames and convert them
 * to raw slinear on demand.
 */

#ifndef _ASTERISK_SLINFACTORY_H
#define _ASTERISK_SLINFACTORY_H
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#if defined(__cplusplus) || defined(c_plusplus)
extern "C" {
#endif

struct ast_slinfactory {
	AST_LIST_HEAD_NOLOCK(, ast_frame) queue;
	struct ast_trans_pvt *trans;
	short hold[1280];
	short *offset;
	size_t holdlen;			/*!< in samples */
	unsigned int size;		/*!< in samples */
	unsigned int format;
};

void ast_slinfactory_init(struct ast_slinfactory *sf);

/*!
 * \brief Destroy the contents of a slinfactory
 *
 * \arg sf the slinfactory that is no longer needed
 *
 * This function will free any memory allocated for the contents of the
 * slinfactory.  It does not free the slinfactory itself.  If the sf is
 * malloc'd, then it must be explicitly free'd after calling this function.
 *
 * \return nothing
 */
void ast_slinfactory_destroy(struct ast_slinfactory *sf);

int ast_slinfactory_feed(struct ast_slinfactory *sf, struct ast_frame *f);
int ast_slinfactory_read(struct ast_slinfactory *sf, short *buf, size_t samples);
unsigned int ast_slinfactory_available(const struct ast_slinfactory *sf);
void ast_slinfactory_flush(struct ast_slinfactory *sf);

#if defined(__cplusplus) || defined(c_plusplus)
}
#endif

#endif /* _ASTERISK_SLINFACTORY_H */
