/*
 * Asterisk -- An open source telephony toolkit.
 *
 * See http://www.asterisk.org for more information about
 * the Asterisk project. Please do not directly contact
 * any of the maintainers of this project for assistance;
 * the project provides a web site, mailing lists and IRC
 * channels for your use.
 */

/*! \file
 * \brief BSD Telephony Of Mexico "Tormenta" Tone Zone Support 2/22/01
 * 
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA. 
 *
 * Primary Author: Pauline Middelink <middelink@polyware.nl>
 *
 */

#ifndef _ASTERISK_INDICATIONS_H
#define _ASTERISK_INDICATIONS_H

#include "asterisk/lock.h"

/*! \brief Description is a series of tones of the format:
	   [!]freq1[+freq2][/duration] separated by commas.  There
	   are no spaces.  The sequence is repeated back to the 
	   first tone description not preceeded by !. Duration is
	   specified in milliseconds */
struct ind_tone_zone_sound {
	const char *name;			/*!< Identifing name */
	const char *data;			/*!< Actual zone description */
	AST_LIST_ENTRY(ind_tone_zone_sound) list;
};

struct ind_tone_zone {
	AST_RWLIST_ENTRY(ind_tone_zone) list;
	char country[5];				/*!< Country code */
	char alias[5];					/*!< is this an alias? */
	char description[40];				/*!< Description */
	int  nrringcadence;				/*!< # registered ringcadence elements */
	int *ringcadence;				/*!< Ring cadence */
	AST_LIST_HEAD_NOLOCK(, ind_tone_zone_sound) tones;		/*!< The known tones for this zone */
};

/*! \brief set the default tone country */
int ast_set_indication_country(const char *country);

/*! \brief locate tone_zone, given the country. if country == NULL, use the default country */
struct ind_tone_zone *ast_get_indication_zone(const char *country);
/*! \brief locate a tone_zone_sound, given the tone_zone. if tone_zone == NULL, use the default tone_zone */
struct ind_tone_zone_sound *ast_get_indication_tone(const struct ind_tone_zone *zone, const char *indication);
/*! \brief deallocate the passed tone zone */
void ast_destroy_indication_zone(struct ind_tone_zone *zone);

/*! \brief add a new country, if country exists, it will be replaced. */
int ast_register_indication_country(struct ind_tone_zone *country);
/*! \brief remove an existing country and all its indications, country must exist */
int ast_unregister_indication_country(const char *country);
/*! \brief add a new indication to a tone_zone. tone_zone must exist. if the indication already
 * exists, it will be replaced. */
int ast_register_indication(struct ind_tone_zone *zone, const char *indication, const char *tonelist);
/*! \brief remove an existing tone_zone's indication. tone_zone must exist */
int ast_unregister_indication(struct ind_tone_zone *zone, const char *indication);

/*! \brief Start a tone-list going */
int ast_playtones_start(struct ast_channel *chan, int vol, const char* tonelist, int interruptible);
/*! \brief Stop the tones from playing */
void ast_playtones_stop(struct ast_channel *chan);

/*! \brief support for walking through a list of indications */
struct ind_tone_zone *ast_walk_indications(const struct ind_tone_zone *cur);

#if 0
extern struct ind_tone_zone *tone_zones;
extern ast_mutex_t tzlock;
#endif

#endif /* _ASTERISK_INDICATIONS_H */
