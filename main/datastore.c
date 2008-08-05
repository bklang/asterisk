/*
 * Asterisk -- An open source telephony toolkit.
 *
 * Copyright (C) 2007 - 2008, Digium, Inc.
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
 * \brief Asterisk datastore objects
 */

#include "asterisk.h"

ASTERISK_FILE_VERSION(__FILE__, "$Revision$")

#include "asterisk/_private.h"

#include "asterisk/datastore.h"
#include "asterisk/utils.h"

struct ast_datastore *ast_datastore_alloc(const struct ast_datastore_info *info, const char *uid)
{
	struct ast_datastore *datastore = NULL;

	/* Make sure we at least have type so we can identify this */
	if (!info) {
		return NULL;
	}

	/* Allocate memory for datastore and clear it */
	datastore = ast_calloc(1, sizeof(*datastore));
	if (!datastore) {
		return NULL;
	}

	datastore->info = info;

	datastore->uid = ast_strdup(uid);

	return datastore;
}

int ast_datastore_free(struct ast_datastore *datastore)
{
	int res = 0;

	/* Using the destroy function (if present) destroy the data */
	if (datastore->info->destroy != NULL && datastore->data != NULL) {
		datastore->info->destroy(datastore->data);
		datastore->data = NULL;
	}

	/* Free allocated UID memory */
	if (datastore->uid != NULL) {
		ast_free((void *) datastore->uid);
		datastore->uid = NULL;
	}

	/* Finally free memory used by ourselves */
	ast_free(datastore);

	return res;
}
