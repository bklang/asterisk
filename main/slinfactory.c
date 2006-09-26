/*
 * Asterisk -- An open source telephony toolkit.
 *
 * Copyright (C) 2005, Anthony Minessale II.
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
 *
 * \brief A machine to gather up arbitrary frames and convert them
 * to raw slinear on demand.
 *
 * \author Anthony Minessale <anthmct@yahoo.com>
 */

#include "asterisk.h"

ASTERISK_FILE_VERSION(__FILE__, "$Revision$")

#include <string.h>

#include "asterisk/frame.h"
#include "asterisk/slinfactory.h"
#include "asterisk/logger.h"
#include "asterisk/translate.h"

void ast_slinfactory_init(struct ast_slinfactory *sf) 
{
	memset(sf, 0, sizeof(*sf));
	sf->offset = sf->hold;
}

void ast_slinfactory_destroy(struct ast_slinfactory *sf) 
{
	struct ast_frame *f;

	if (sf->trans) {
		ast_translator_free_path(sf->trans);
		sf->trans = NULL;
	}

	while ((f = AST_LIST_REMOVE_HEAD(&sf->queue, frame_list)))
		ast_frfree(f);
}

int ast_slinfactory_feed(struct ast_slinfactory *sf, struct ast_frame *f)
{
	struct ast_frame *frame, *frame_ptr;
	unsigned int x;

	if (f->subclass != AST_FORMAT_SLINEAR) {
		if (sf->trans && f->subclass != sf->format) {
			ast_translator_free_path(sf->trans);
			sf->trans = NULL;
		}
		if (!sf->trans) {
			if ((sf->trans = ast_translator_build_path(AST_FORMAT_SLINEAR, f->subclass)) == NULL) {
				ast_log(LOG_WARNING, "Cannot build a path from %s to slin\n", ast_getformatname(f->subclass));
				return 0;
			} else {
				sf->format = f->subclass;
			}
		}
	}

	if (!(frame = ast_frdup( (sf->trans) ? ast_translate(sf->trans, f, 0) : f )))
		return 0;

	x = 0;
	AST_LIST_TRAVERSE(&sf->queue, frame_ptr, frame_list)
		x++;

	AST_LIST_INSERT_TAIL(&sf->queue, frame, frame_list);

	sf->size += frame->samples;

	return x;
}

int ast_slinfactory_read(struct ast_slinfactory *sf, short *buf, size_t samples) 
{
	struct ast_frame *frame_ptr;
	unsigned int sofar = 0, ineed, remain;
	short *frame_data, *offset = buf;

	while (sofar < samples) {
		ineed = samples - sofar;

		if (sf->holdlen) {
			if ((sofar + sf->holdlen) <= ineed) {
				memcpy(offset, sf->hold, sf->holdlen * sizeof(*offset));
				sofar += sf->holdlen;
				offset += sf->holdlen;
				sf->holdlen = 0;
				sf->offset = sf->hold;
			} else {
				remain = sf->holdlen - ineed;
				memcpy(offset, sf->offset, ineed * sizeof(*offset));
				sofar += ineed;
				sf->offset += ineed;
				sf->holdlen = remain;
			}
			continue;
		}
		
		if ((frame_ptr = AST_LIST_REMOVE_HEAD(&sf->queue, frame_list))) {
			frame_data = frame_ptr->data;
			
			if ((sofar + frame_ptr->samples) <= ineed) {
				memcpy(offset, frame_data, frame_ptr->samples * sizeof(*offset));
				sofar += frame_ptr->samples;
				offset += frame_ptr->samples;
			} else {
				remain = frame_ptr->samples - ineed;
				memcpy(offset, frame_data, ineed * sizeof(*offset));
				sofar += ineed;
				frame_data += ineed;
				memcpy(sf->hold, frame_data, remain * sizeof(*offset));
				sf->holdlen = remain;
			}
			ast_frfree(frame_ptr);
		} else {
			break;
		}
	}

	sf->size -= sofar;
	return sofar;
}

unsigned int ast_slinfactory_available(const struct ast_slinfactory *sf)
{
	return sf->size;
}
