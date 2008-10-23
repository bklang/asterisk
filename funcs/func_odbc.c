/*
 * Asterisk -- An open source telephony toolkit.
 *
 * Copyright (c) 2005, 2006 Tilghman Lesher
 *
 * Tilghman Lesher <func_odbc__200508@the-tilghman.com>
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
 *
 * \brief ODBC lookups
 *
 * \author Tilghman Lesher <func_odbc__200508@the-tilghman.com>
 *
 * \ingroup functions
 */

/*** MODULEINFO
	<depend>unixodbc_or_iodbc</depend>
	<depend>ltdl</depend>
	<depend>res_odbc</depend>
	<use>unixodbc</use>
	<use>iodbc</use>
 ***/

#include "asterisk.h"

ASTERISK_FILE_VERSION(__FILE__, "$Revision$")

#include "asterisk/module.h"
#include "asterisk/file.h"
#include "asterisk/channel.h"
#include "asterisk/pbx.h"
#include "asterisk/config.h"
#include "asterisk/res_odbc.h"
#include "asterisk/app.h"
#include "asterisk/cli.h"

static char *config = "func_odbc.conf";

enum {
	OPT_ESCAPECOMMAS =	(1 << 0),
	OPT_MULTIROW     =	(1 << 1),
} odbc_option_flags;

struct acf_odbc_query {
	AST_RWLIST_ENTRY(acf_odbc_query) list;
	char readhandle[5][30];
	char writehandle[5][30];
	char sql_read[2048];
	char sql_write[2048];
	unsigned int flags;
	int rowlimit;
	struct ast_custom_function *acf;
};

static void odbc_datastore_free(void *data);

struct ast_datastore_info odbc_info = {
	.type = "FUNC_ODBC",
	.destroy = odbc_datastore_free,
};

/* For storing each result row */
struct odbc_datastore_row {
	AST_LIST_ENTRY(odbc_datastore_row) list;
	char data[0];
};

/* For storing each result set */
struct odbc_datastore {
	AST_LIST_HEAD(, odbc_datastore_row);
	char names[0];
};

AST_RWLIST_HEAD_STATIC(queries, acf_odbc_query);

static int resultcount = 0;

AST_THREADSTORAGE(sql_buf);
AST_THREADSTORAGE(coldata_buf);
AST_THREADSTORAGE(colnames_buf);

static void odbc_datastore_free(void *data)
{
	struct odbc_datastore *result = data;
	struct odbc_datastore_row *row;
	AST_LIST_LOCK(result);
	while ((row = AST_LIST_REMOVE_HEAD(result, list))) {
		ast_free(row);
	}
	AST_LIST_UNLOCK(result);
	AST_LIST_HEAD_DESTROY(result);
	ast_free(result);
}

static SQLHSTMT generic_execute(struct odbc_obj *obj, void *data)
{
	int res;
	char *sql = data;
	SQLHSTMT stmt;

	res = SQLAllocHandle (SQL_HANDLE_STMT, obj->con, &stmt);
	if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
		ast_log(LOG_WARNING, "SQL Alloc Handle failed!\n");
		return NULL;
	}

	res = SQLExecDirect(stmt, (unsigned char *)sql, SQL_NTS);
	if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
		ast_log(LOG_WARNING, "SQL Exec Direct failed![%s]\n", sql);
		SQLCloseCursor(stmt);
		SQLFreeHandle (SQL_HANDLE_STMT, stmt);
		return NULL;
	}

	return stmt;
}

/*
 * Master control routine
 */
static int acf_odbc_write(struct ast_channel *chan, const char *cmd, char *s, const char *value)
{
	struct odbc_obj *obj = NULL;
	struct acf_odbc_query *query;
	char *t, varname[15];
	int i, dsn, bogus_chan = 0;
	AST_DECLARE_APP_ARGS(values,
		AST_APP_ARG(field)[100];
	);
	AST_DECLARE_APP_ARGS(args,
		AST_APP_ARG(field)[100];
	);
	SQLHSTMT stmt = NULL;
	SQLLEN rows=0;
	struct ast_str *buf = ast_str_thread_get(&sql_buf, 16);

	if (!buf) {
		return -1;
	}

	AST_RWLIST_RDLOCK(&queries);
	AST_RWLIST_TRAVERSE(&queries, query, list) {
		if (!strcmp(query->acf->name, cmd)) {
			break;
		}
	}

	if (!query) {
		ast_log(LOG_ERROR, "No such function '%s'\n", cmd);
		AST_RWLIST_UNLOCK(&queries);
		ast_free(buf);
		return -1;
	}

	if (!chan) {
		if ((chan = ast_channel_alloc(0, 0, "", "", "", "", "", 0, "Bogus/func_odbc")))
			bogus_chan = 1;
	}

	if (chan)
		ast_autoservice_start(chan);

	ast_str_make_space(&buf, strlen(query->sql_write) * 2 + 300);

	/* Parse our arguments */
	t = value ? ast_strdupa(value) : "";

	if (!s || !t) {
		ast_log(LOG_ERROR, "Out of memory\n");
		AST_RWLIST_UNLOCK(&queries);
		if (chan)
			ast_autoservice_stop(chan);
		if (bogus_chan)
			ast_channel_free(chan);
		ast_free(buf);
		return -1;
	}

	AST_STANDARD_APP_ARGS(args, s);
	for (i = 0; i < args.argc; i++) {
		snprintf(varname, sizeof(varname), "ARG%d", i + 1);
		pbx_builtin_pushvar_helper(chan, varname, args.field[i]);
	}

	/* Parse values, just like arguments */
	AST_STANDARD_APP_ARGS(values, t);
	for (i = 0; i < values.argc; i++) {
		snprintf(varname, sizeof(varname), "VAL%d", i + 1);
		pbx_builtin_pushvar_helper(chan, varname, values.field[i]);
	}

	/* Additionally set the value as a whole (but push an empty string if value is NULL) */
	pbx_builtin_pushvar_helper(chan, "VALUE", value ? value : "");

	pbx_substitute_variables_helper(chan, query->sql_write, buf->str, buf->len - 1);

	/* Restore prior values */
	for (i = 0; i < args.argc; i++) {
		snprintf(varname, sizeof(varname), "ARG%d", i + 1);
		pbx_builtin_setvar_helper(chan, varname, NULL);
	}

	for (i = 0; i < values.argc; i++) {
		snprintf(varname, sizeof(varname), "VAL%d", i + 1);
		pbx_builtin_setvar_helper(chan, varname, NULL);
	}
	pbx_builtin_setvar_helper(chan, "VALUE", NULL);

	for (dsn = 0; dsn < 5; dsn++) {
		if (!ast_strlen_zero(query->writehandle[dsn])) {
			obj = ast_odbc_request_obj(query->writehandle[dsn], 0);
			if (obj)
				stmt = ast_odbc_direct_execute(obj, generic_execute, buf);
		}
		if (stmt)
			break;
	}

	AST_RWLIST_UNLOCK(&queries);

	if (stmt) {
		/* Rows affected */
		SQLRowCount(stmt, &rows);
	}

	/* Output the affected rows, for all cases.  In the event of failure, we
	 * flag this as -1 rows.  Note that this is different from 0 affected rows
	 * which would be the case if we succeeded in our query, but the values did
	 * not change. */
	snprintf(varname, sizeof(varname), "%d", (int)rows);
	pbx_builtin_setvar_helper(chan, "ODBCROWS", varname);

	if (stmt) {
		SQLCloseCursor(stmt);
		SQLFreeHandle(SQL_HANDLE_STMT, stmt);
	}
	if (obj)
		ast_odbc_release_obj(obj);

	if (chan)
		ast_autoservice_stop(chan);
	if (bogus_chan)
		ast_channel_free(chan);
	ast_free(buf);

	return 0;
}

static int acf_odbc_read(struct ast_channel *chan, const char *cmd, char *s, char *buf, size_t len)
{
	struct odbc_obj *obj = NULL;
	struct acf_odbc_query *query;
	char varname[15], rowcount[12] = "-1";
	struct ast_str *colnames = ast_str_thread_get(&colnames_buf, 16);
	int res, x, y, buflen = 0, escapecommas, rowlimit = 1, dsn, bogus_chan = 0;
	AST_DECLARE_APP_ARGS(args,
		AST_APP_ARG(field)[100];
	);
	SQLHSTMT stmt = NULL;
	SQLSMALLINT colcount=0;
	SQLLEN indicator;
	SQLSMALLINT collength;
	struct odbc_datastore *resultset = NULL;
	struct odbc_datastore_row *row = NULL;
	struct ast_str *sql = ast_str_thread_get(&sql_buf, 16);

	if (!sql) {
		return -1;
	}

	AST_RWLIST_RDLOCK(&queries);
	AST_RWLIST_TRAVERSE(&queries, query, list) {
		if (!strcmp(query->acf->name, cmd)) {
			break;
		}
	}

	if (!query) {
		ast_log(LOG_ERROR, "No such function '%s'\n", cmd);
		AST_RWLIST_UNLOCK(&queries);
		pbx_builtin_setvar_helper(chan, "ODBCROWS", rowcount);
		ast_free(sql);
		return -1;
	}

	if (!chan) {
		if ((chan = ast_channel_alloc(0, 0, "", "", "", "", "", 0, "Bogus/func_odbc"))) {
			bogus_chan = 1;
		}
	}

	if (chan) {
		ast_autoservice_start(chan);
	}

	AST_STANDARD_APP_ARGS(args, s);
	for (x = 0; x < args.argc; x++) {
		snprintf(varname, sizeof(varname), "ARG%d", x + 1);
		pbx_builtin_pushvar_helper(chan, varname, args.field[x]);
	}

	ast_str_make_space(&sql, strlen(query->sql_read) * 2 + 300);
	pbx_substitute_variables_helper(chan, query->sql_read, sql->str, sql->len - 1);

	/* Restore prior values */
	for (x = 0; x < args.argc; x++) {
		snprintf(varname, sizeof(varname), "ARG%d", x + 1);
		pbx_builtin_setvar_helper(chan, varname, NULL);
	}

	/* Save these flags, so we can release the lock */
	escapecommas = ast_test_flag(query, OPT_ESCAPECOMMAS);
	if (ast_test_flag(query, OPT_MULTIROW)) {
		resultset = ast_calloc(1, sizeof(*resultset));
		AST_LIST_HEAD_INIT(resultset);
		if (query->rowlimit) {
			rowlimit = query->rowlimit;
		} else {
			rowlimit = INT_MAX;
		}
	}
	AST_RWLIST_UNLOCK(&queries);

	for (dsn = 0; dsn < 5; dsn++) {
		if (!ast_strlen_zero(query->readhandle[dsn])) {
			obj = ast_odbc_request_obj(query->readhandle[dsn], 0);
			if (obj) {
				stmt = ast_odbc_direct_execute(obj, generic_execute, sql->str);
			}
		}
		if (stmt) {
			break;
		}
	}

	if (!stmt) {
		ast_log(LOG_ERROR, "Unable to execute query [%s]\n", sql->str);
		if (obj) {
			ast_odbc_release_obj(obj);
		}
		pbx_builtin_setvar_helper(chan, "ODBCROWS", rowcount);
		if (chan) {
			ast_autoservice_stop(chan);
		}
		if (bogus_chan) {
			ast_channel_free(chan);
		}
		ast_free(sql);
		return -1;
	}

	res = SQLNumResultCols(stmt, &colcount);
	if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
		ast_log(LOG_WARNING, "SQL Column Count error!\n[%s]\n\n", sql->str);
		SQLCloseCursor(stmt);
		SQLFreeHandle (SQL_HANDLE_STMT, stmt);
		ast_odbc_release_obj(obj);
		pbx_builtin_setvar_helper(chan, "ODBCROWS", rowcount);
		if (chan) {
			ast_autoservice_stop(chan);
		}
		if (bogus_chan) {
			ast_channel_free(chan);
		}
		ast_free(sql);
		return -1;
	}

	res = SQLFetch(stmt);
	if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
		int res1 = -1;
		if (res == SQL_NO_DATA) {
			ast_verb(4, "Found no rows [%s]\n", sql->str);
			res1 = 0;
			buf[0] = '\0';
			ast_copy_string(rowcount, "0", sizeof(rowcount));
		} else {
			ast_log(LOG_WARNING, "Error %d in FETCH [%s]\n", res, sql->str);
		}
		SQLCloseCursor(stmt);
		SQLFreeHandle(SQL_HANDLE_STMT, stmt);
		ast_odbc_release_obj(obj);
		pbx_builtin_setvar_helper(chan, "ODBCROWS", rowcount);
		if (chan)
			ast_autoservice_stop(chan);
		if (bogus_chan)
			ast_channel_free(chan);
		ast_free(sql);
		return res1;
	}

	for (y = 0; y < rowlimit; y++) {
		for (x = 0; x < colcount; x++) {
			int i;
			struct ast_str *coldata = ast_str_thread_get(&coldata_buf, 16);

			if (y == 0) {
				char colname[256];
				SQLULEN maxcol;

				res = SQLDescribeCol(stmt, x + 1, (unsigned char *)colname, sizeof(colname), &collength, NULL, &maxcol, NULL, NULL);
				ast_debug(3, "Got collength of %d and maxcol of %d for column '%s' (offset %d)\n", (int)collength, (int)maxcol, colname, x);
				if (((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) || collength == 0) {
					snprintf(colname, sizeof(colname), "field%d", x);
				}

				if (coldata->len < maxcol + 1) {
					ast_str_make_space(&coldata, maxcol + 1);
				}

				if (colnames->used) {
					ast_str_append(&colnames, 0, ",");
				}
				ast_str_make_space(&colnames, strlen(colname) * 2 + 1 + colnames->used);

				/* Copy data, encoding '\' and ',' for the argument parser */
				for (i = 0; i < sizeof(colname); i++) {
					if (escapecommas && (colname[i] == '\\' || colname[i] == ',')) {
						colnames->str[colnames->used++] = '\\';
					}
					colnames->str[colnames->used++] = colname[i];

					if (colname[i] == '\0') {
						break;
					}
				}

				if (resultset) {
					void *tmp = ast_realloc(resultset, sizeof(*resultset) + colnames->used + 1);
					if (!tmp) {
						ast_log(LOG_ERROR, "No space for a new resultset?\n");
						ast_free(resultset);
						SQLCloseCursor(stmt);
						SQLFreeHandle(SQL_HANDLE_STMT, stmt);
						ast_odbc_release_obj(obj);
						pbx_builtin_setvar_helper(chan, "ODBCROWS", rowcount);
						if (chan)
							ast_autoservice_stop(chan);
						if (bogus_chan)
							ast_channel_free(chan);
						ast_free(sql);
						return -1;
					}
					resultset = tmp;
					strcpy((char *)resultset + sizeof(*resultset), colnames->str);
				}
			}

			buflen = strlen(buf);
			res = SQLGetData(stmt, x + 1, SQL_CHAR, coldata->str, coldata->len, &indicator);
			if (indicator == SQL_NULL_DATA) {
				ast_debug(3, "Got NULL data\n");
				ast_str_reset(coldata);
				res = SQL_SUCCESS;
			}

			if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
				ast_log(LOG_WARNING, "SQL Get Data error!\n[%s]\n\n", sql->str);
				y = -1;
				buf[0] = '\0';
				goto end_acf_read;
			}

			ast_debug(2, "Got coldata of '%s'\n", coldata->str);
			coldata->used = strlen(coldata->str);

			/* Copy data, encoding '\' and ',' for the argument parser */
			for (i = 0; i < coldata->used; i++) {
				if (escapecommas && (coldata->str[i] == '\\' || coldata->str[i] == ',')) {
					buf[buflen++] = '\\';
				}
				buf[buflen++] = coldata->str[i];

				if (buflen >= len - 2)
					break;

				if (coldata->str[i] == '\0')
					break;
			}

			buf[buflen++] = ',';
			buf[buflen] = '\0';
			ast_debug(2, "buf is now set to '%s'\n", buf);
		}
		/* Trim trailing comma */
		buf[buflen - 1] = '\0';
		ast_debug(2, "buf is now set to '%s'\n", buf);

		if (resultset) {
			row = ast_calloc(1, sizeof(*row) + buflen);
			if (!row) {
				ast_log(LOG_ERROR, "Unable to allocate space for more rows in this resultset.\n");
				goto end_acf_read;
			}
			strcpy((char *)row + sizeof(*row), buf);
			AST_LIST_INSERT_TAIL(resultset, row, list);

			/* Get next row */
			res = SQLFetch(stmt);
			if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
				if (res != SQL_NO_DATA)
					ast_log(LOG_WARNING, "Error %d in FETCH [%s]\n", res, sql->str);
				y++;
				break;
			}
		}
	}

end_acf_read:
	snprintf(rowcount, sizeof(rowcount), "%d", y);
	pbx_builtin_setvar_helper(chan, "ODBCROWS", rowcount);
	pbx_builtin_setvar_helper(chan, "~ODBCFIELDS~", colnames->str);
	if (resultset) {
		int uid;
		struct ast_datastore *odbc_store;
		uid = ast_atomic_fetchadd_int(&resultcount, +1) + 1;
		snprintf(buf, len, "%d", uid);
		odbc_store = ast_datastore_alloc(&odbc_info, buf);
		if (!odbc_store) {
			ast_log(LOG_ERROR, "Rows retrieved, but unable to store it in the channel.  Results fail.\n");
			odbc_datastore_free(resultset);
			SQLCloseCursor(stmt);
			SQLFreeHandle(SQL_HANDLE_STMT, stmt);
			ast_odbc_release_obj(obj);
			if (chan)
				ast_autoservice_stop(chan);
			if (bogus_chan)
				ast_channel_free(chan);
			ast_free(sql);
			return -1;
		}
		odbc_store->data = resultset;
		ast_channel_datastore_add(chan, odbc_store);
	}
	SQLCloseCursor(stmt);
	SQLFreeHandle(SQL_HANDLE_STMT, stmt);
	ast_odbc_release_obj(obj);
	if (chan)
		ast_autoservice_stop(chan);
	if (bogus_chan)
		ast_channel_free(chan);
	ast_free(sql);
	return 0;
}

static int acf_escape(struct ast_channel *chan, const char *cmd, char *data, char *buf, size_t len)
{
	char *out = buf;

	for (; *data && out - buf < len; data++) {
		if (*data == '\'') {
			*out = '\'';
			out++;
		}
		*out++ = *data;
	}
	*out = '\0';

	return 0;
}

static struct ast_custom_function escape_function = {
	.name = "SQL_ESC",
	.synopsis = "Escapes single ticks for use in SQL statements",
	.syntax = "SQL_ESC(<string>)",
	.desc =
"Used in SQL templates to escape data which may contain single ticks (') which\n"
"are otherwise used to delimit data.  For example:\n"
"SELECT foo FROM bar WHERE baz='${SQL_ESC(${ARG1})}'\n",
	.read = acf_escape,
	.write = NULL,
};

static int acf_fetch(struct ast_channel *chan, const char *cmd, char *data, char *buf, size_t len)
{
	struct ast_datastore *store;
	struct odbc_datastore *resultset;
	struct odbc_datastore_row *row;
	store = ast_channel_datastore_find(chan, &odbc_info, data);
	if (!store) {
		pbx_builtin_setvar_helper(chan, "ODBC_FETCH_STATUS", "FAILURE");
		return -1;
	}
	resultset = store->data;
	AST_LIST_LOCK(resultset);
	row = AST_LIST_REMOVE_HEAD(resultset, list);
	AST_LIST_UNLOCK(resultset);
	if (!row) {
		/* Cleanup datastore */
		ast_channel_datastore_remove(chan, store);
		ast_datastore_free(store);
		pbx_builtin_setvar_helper(chan, "ODBC_FETCH_STATUS", "FAILURE");
		return -1;
	}
	pbx_builtin_setvar_helper(chan, "~ODBCFIELDS~", resultset->names);
	ast_copy_string(buf, row->data, len);
	ast_free(row);
	pbx_builtin_setvar_helper(chan, "ODBC_FETCH_STATUS", "SUCCESS");
	return 0;
}

static struct ast_custom_function fetch_function = {
	.name = "ODBC_FETCH",
	.synopsis = "Fetch a row from a multirow query",
	.syntax = "ODBC_FETCH(<result-id>)",
	.desc =
"For queries which are marked as mode=multirow, the original query returns a\n"
"result-id from which results may be fetched.  This function implements the\n"
"actual fetch of the results.\n"
"This function also sets ODBC_FETCH_STATUS to one of \"SUCCESS\" or \"FAILURE\",\n"
"depending upon whether there were rows available or not.\n",
	.read = acf_fetch,
	.write = NULL,
};

static char *app_odbcfinish = "ODBCFinish";
static char *syn_odbcfinish = "Clear the resultset of a successful multirow query";
static char *desc_odbcfinish =
"ODBCFinish(<result-id>)\n"
"  Clears any remaining rows of the specified resultset\n";


static int exec_odbcfinish(struct ast_channel *chan, void *data)
{
	struct ast_datastore *store = ast_channel_datastore_find(chan, &odbc_info, data);
	if (!store) /* Already freed; no big deal. */
		return 0;
	ast_channel_datastore_remove(chan, store);
	ast_datastore_free(store);
	return 0;
}

static int init_acf_query(struct ast_config *cfg, char *catg, struct acf_odbc_query **query)
{
	const char *tmp;
	int i;

	if (!cfg || !catg) {
		return EINVAL;
	}

	*query = ast_calloc(1, sizeof(struct acf_odbc_query));
	if (! (*query))
		return ENOMEM;

	if (((tmp = ast_variable_retrieve(cfg, catg, "writehandle"))) || ((tmp = ast_variable_retrieve(cfg, catg, "dsn")))) {
		char *tmp2 = ast_strdupa(tmp);
		AST_DECLARE_APP_ARGS(writeconf,
			AST_APP_ARG(dsn)[5];
		);
		AST_STANDARD_APP_ARGS(writeconf, tmp2);
		for (i = 0; i < 5; i++) {
			if (!ast_strlen_zero(writeconf.dsn[i]))
				ast_copy_string((*query)->writehandle[i], writeconf.dsn[i], sizeof((*query)->writehandle[i]));
		}
	}

	if ((tmp = ast_variable_retrieve(cfg, catg, "readhandle"))) {
		char *tmp2 = ast_strdupa(tmp);
		AST_DECLARE_APP_ARGS(readconf,
			AST_APP_ARG(dsn)[5];
		);
		AST_STANDARD_APP_ARGS(readconf, tmp2);
		for (i = 0; i < 5; i++) {
			if (!ast_strlen_zero(readconf.dsn[i]))
				ast_copy_string((*query)->readhandle[i], readconf.dsn[i], sizeof((*query)->readhandle[i]));
		}
	} else {
		/* If no separate readhandle, then use the writehandle for reading */
		for (i = 0; i < 5; i++) {
			if (!ast_strlen_zero((*query)->writehandle[i]))
				ast_copy_string((*query)->readhandle[i], (*query)->writehandle[i], sizeof((*query)->readhandle[i]));
		}
 	}

	if ((tmp = ast_variable_retrieve(cfg, catg, "readsql")))
		ast_copy_string((*query)->sql_read, tmp, sizeof((*query)->sql_read));
	else if ((tmp = ast_variable_retrieve(cfg, catg, "read"))) {
		ast_log(LOG_WARNING, "Parameter 'read' is deprecated for category %s.  Please use 'readsql' instead.\n", catg);
		ast_copy_string((*query)->sql_read, tmp, sizeof((*query)->sql_read));
	}

	if (!ast_strlen_zero((*query)->sql_read) && ast_strlen_zero((*query)->readhandle[0])) {
		ast_free(*query);
		*query = NULL;
		ast_log(LOG_ERROR, "There is SQL, but no ODBC class to be used for reading: %s\n", catg);
		return EINVAL;
	}

	if ((tmp = ast_variable_retrieve(cfg, catg, "writesql")))
		ast_copy_string((*query)->sql_write, tmp, sizeof((*query)->sql_write));
	else if ((tmp = ast_variable_retrieve(cfg, catg, "write"))) {
		ast_log(LOG_WARNING, "Parameter 'write' is deprecated for category %s.  Please use 'writesql' instead.\n", catg);
		ast_copy_string((*query)->sql_write, tmp, sizeof((*query)->sql_write));
	}

	if (!ast_strlen_zero((*query)->sql_write) && ast_strlen_zero((*query)->writehandle[0])) {
		ast_free(*query);
		*query = NULL;
		ast_log(LOG_ERROR, "There is SQL, but no ODBC class to be used for writing: %s\n", catg);
		return EINVAL;
	}

	/* Allow escaping of embedded commas in fields to be turned off */
	ast_set_flag((*query), OPT_ESCAPECOMMAS);
	if ((tmp = ast_variable_retrieve(cfg, catg, "escapecommas"))) {
		if (ast_false(tmp))
			ast_clear_flag((*query), OPT_ESCAPECOMMAS);
	}

	if ((tmp = ast_variable_retrieve(cfg, catg, "mode"))) {
		if (strcasecmp(tmp, "multirow") == 0)
			ast_set_flag((*query), OPT_MULTIROW);
		if ((tmp = ast_variable_retrieve(cfg, catg, "rowlimit")))
			sscanf(tmp, "%d", &((*query)->rowlimit));
	}

	(*query)->acf = ast_calloc(1, sizeof(struct ast_custom_function));
	if (! (*query)->acf) {
		ast_free(*query);
		*query = NULL;
		return ENOMEM;
	}

	if ((tmp = ast_variable_retrieve(cfg, catg, "prefix")) && !ast_strlen_zero(tmp)) {
		asprintf((char **)&((*query)->acf->name), "%s_%s", tmp, catg);
	} else {
		asprintf((char **)&((*query)->acf->name), "ODBC_%s", catg);
	}

	if (!((*query)->acf->name)) {
		ast_free((*query)->acf);
		ast_free(*query);
		*query = NULL;
		return ENOMEM;
	}

	if ((tmp = ast_variable_retrieve(cfg, catg, "syntax")) && !ast_strlen_zero(tmp)) {
		asprintf((char **)&((*query)->acf->syntax), "%s(%s)", (*query)->acf->name, tmp);
	} else {
		asprintf((char **)&((*query)->acf->syntax), "%s(<arg1>[...[,<argN>]])", (*query)->acf->name);
	}

	if (!((*query)->acf->syntax)) {
		ast_free((char *)(*query)->acf->name);
		ast_free((*query)->acf);
		ast_free(*query);
		*query = NULL;
		return ENOMEM;
	}

	if ((tmp = ast_variable_retrieve(cfg, catg, "synopsis")) && !ast_strlen_zero(tmp)) {
		(*query)->acf->synopsis = ast_strdup(tmp);
	} else {
		(*query)->acf->synopsis = ast_strdup("Runs the referenced query with the specified arguments");
	}

	if (!((*query)->acf->synopsis)) {
		ast_free((char *)(*query)->acf->name);
		ast_free((char *)(*query)->acf->syntax);
		ast_free((*query)->acf);
		ast_free(*query);
		*query = NULL;
		return ENOMEM;
	}

	if (!ast_strlen_zero((*query)->sql_read) && !ast_strlen_zero((*query)->sql_write)) {
		asprintf((char **)&((*query)->acf->desc),
					"Runs the following query, as defined in func_odbc.conf, performing\n"
				   	"substitution of the arguments into the query as specified by ${ARG1},\n"
					"${ARG2}, ... ${ARGn}.  When setting the function, the values are provided\n"
					"either in whole as ${VALUE} or parsed as ${VAL1}, ${VAL2}, ... ${VALn}.\n"
					"\nRead:\n%s\n\nWrite:\n%s\n",
					(*query)->sql_read,
					(*query)->sql_write);
	} else if (!ast_strlen_zero((*query)->sql_read)) {
		asprintf((char **)&((*query)->acf->desc),
					"Runs the following query, as defined in func_odbc.conf, performing\n"
				   	"substitution of the arguments into the query as specified by ${ARG1},\n"
					"${ARG2}, ... ${ARGn}.  This function may only be read, not set.\n\nSQL:\n%s\n",
					(*query)->sql_read);
	} else if (!ast_strlen_zero((*query)->sql_write)) {
		asprintf((char **)&((*query)->acf->desc),
					"Runs the following query, as defined in func_odbc.conf, performing\n"
				   	"substitution of the arguments into the query as specified by ${ARG1},\n"
					"${ARG2}, ... ${ARGn}.  The values are provided either in whole as\n"
					"${VALUE} or parsed as ${VAL1}, ${VAL2}, ... ${VALn}.\n"
					"This function may only be set.\nSQL:\n%s\n",
					(*query)->sql_write);
	} else {
		ast_free((char *)(*query)->acf->synopsis);
		ast_free((char *)(*query)->acf->syntax);
		ast_free((char *)(*query)->acf->name);
		ast_free((*query)->acf);
		ast_free(*query);
		ast_log(LOG_WARNING, "Section %s was found, but there was no SQL to execute.  Ignoring.\n", catg);
		return EINVAL;
	}

	if (! ((*query)->acf->desc)) {
		ast_free((char *)(*query)->acf->synopsis);
		ast_free((char *)(*query)->acf->syntax);
		ast_free((char *)(*query)->acf->name);
		ast_free((*query)->acf);
		ast_free(*query);
		*query = NULL;
		return ENOMEM;
	}

	if (ast_strlen_zero((*query)->sql_read)) {
		(*query)->acf->read = NULL;
	} else {
		(*query)->acf->read = acf_odbc_read;
	}

	if (ast_strlen_zero((*query)->sql_write)) {
		(*query)->acf->write = NULL;
	} else {
		(*query)->acf->write = acf_odbc_write;
	}

	return 0;
}

static int free_acf_query(struct acf_odbc_query *query)
{
	if (query) {
		if (query->acf) {
			if (query->acf->name)
				ast_free((char *)query->acf->name);
			if (query->acf->syntax)
				ast_free((char *)query->acf->syntax);
			if (query->acf->synopsis)
				ast_free((char *)query->acf->synopsis);
			if (query->acf->desc)
				ast_free((char *)query->acf->desc);
			ast_free(query->acf);
		}
		ast_free(query);
	}
	return 0;
}

static char *cli_odbc_read(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	AST_DECLARE_APP_ARGS(args,
		AST_APP_ARG(field)[100];
	);
	struct ast_str *sql = ast_str_thread_get(&sql_buf, 16);
	char *char_args, varname[10];
	struct acf_odbc_query *query;
	struct ast_channel *chan;
	int i;

	switch (cmd) {
	case CLI_INIT:
		e->command = "odbc read";
		e->usage =
			"Usage: odbc read <name> <args> [exec]\n"
			"       Evaluates the SQL provided in the ODBC function <name>, and\n"
			"       optionally executes the function.  This function is intended for\n"
			"       testing purposes.  Remember to quote arguments containing spaces.\n";
		return NULL;
	case CLI_GENERATE:
		if (a->pos == 2) {
			int wordlen = strlen(a->word), which = 0;
			/* Complete function name */
			AST_RWLIST_RDLOCK(&queries);
			AST_RWLIST_TRAVERSE(&queries, query, list) {
				if (!strncasecmp(query->acf->name, a->word, wordlen)) {
					if (++which > a->n) {
						char *res = ast_strdup(query->acf->name);
						AST_RWLIST_UNLOCK(&queries);
						return res;
					}
				}
			}
			AST_RWLIST_UNLOCK(&queries);
			return NULL;
		} else if (a->pos == 4) {
			return a->n == 0 ? ast_strdup("exec") : NULL;
		} else {
			return NULL;
		}
	}

	if (a->argc < 4 || a->argc > 5) {
		return CLI_SHOWUSAGE;
	}

	AST_RWLIST_RDLOCK(&queries);
	AST_RWLIST_TRAVERSE(&queries, query, list) {
		if (!strcmp(query->acf->name, a->argv[2])) {
			break;
		}
	}

	if (!query) {
		ast_cli(a->fd, "No such query '%s'\n", a->argv[2]);
		AST_RWLIST_UNLOCK(&queries);
		return CLI_SHOWUSAGE;
	}

	if (ast_strlen_zero(query->sql_read)) {
		ast_cli(a->fd, "The function %s has no writesql parameter.\n", a->argv[2]);
		AST_RWLIST_UNLOCK(&queries);
		return CLI_SUCCESS;
	}

	ast_str_make_space(&sql, strlen(query->sql_read) * 2 + 300);

	/* Evaluate function */
	char_args = ast_strdupa(a->argv[3]);

	chan = ast_channel_alloc(0, 0, "", "", "", "", "", 0, "Bogus/func_odbc");

	AST_STANDARD_APP_ARGS(args, char_args);
	for (i = 0; i < args.argc; i++) {
		snprintf(varname, sizeof(varname), "ARG%d", i + 1);
		pbx_builtin_pushvar_helper(chan, varname, args.field[i]);
	}

	/*!\note This does not set sql->used, so don't try to use that value. */
	pbx_substitute_variables_helper(chan, query->sql_read, sql->str, sql->len - 1);
	ast_channel_free(chan);

	if (a->argc == 5 && !strcmp(a->argv[4], "exec")) {
		/* Execute the query */
		struct odbc_obj *obj = NULL;
		int dsn, executed = 0;
		SQLHSTMT stmt;
		int rows = 0, res, x;
		SQLSMALLINT colcount = 0, collength;
		SQLLEN indicator;
		struct ast_str *coldata = ast_str_thread_get(&coldata_buf, 16);
		char colname[256];
		SQLULEN maxcol;

		for (dsn = 0; dsn < 5; dsn++) {
			if (ast_strlen_zero(query->readhandle[dsn])) {
				continue;
			}
			ast_debug(1, "Found handle %s\n", query->readhandle[dsn]);
			if (!(obj = ast_odbc_request_obj(query->readhandle[dsn], 0))) {
				continue;
			}

			ast_debug(1, "Got obj\n");
			if (!(stmt = ast_odbc_direct_execute(obj, generic_execute, sql->str))) {
				ast_odbc_release_obj(obj);
				continue;
			}

			executed = 1;

			res = SQLNumResultCols(stmt, &colcount);
			if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
				ast_cli(a->fd, "SQL Column Count error!\n[%s]\n\n", sql->str);
				SQLCloseCursor(stmt);
				SQLFreeHandle (SQL_HANDLE_STMT, stmt);
				ast_odbc_release_obj(obj);
				AST_RWLIST_UNLOCK(&queries);
				return CLI_SUCCESS;
			}

			res = SQLFetch(stmt);
			if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
				SQLCloseCursor(stmt);
				SQLFreeHandle(SQL_HANDLE_STMT, stmt);
				ast_odbc_release_obj(obj);
				if (res == SQL_NO_DATA) {
					ast_cli(a->fd, "Returned %d rows.  Query executed on handle %d:%s [%s]\n", rows, dsn, query->readhandle[dsn], sql->str);
					break;
				} else {
					ast_cli(a->fd, "Error %d in FETCH [%s]\n", res, sql->str);
				}
				AST_RWLIST_UNLOCK(&queries);
				return CLI_SUCCESS;
			}
			for (;;) {
				for (x = 0; x < colcount; x++) {
					res = SQLDescribeCol(stmt, x + 1, (unsigned char *)colname, sizeof(colname), &collength, NULL, &maxcol, NULL, NULL);
					if (((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) || collength == 0) {
						snprintf(colname, sizeof(colname), "field%d", x);
					}

					if (coldata->len < maxcol + 1) {
						ast_str_make_space(&coldata, maxcol + 1);
					}

					res = SQLGetData(stmt, x + 1, SQL_CHAR, coldata->str, coldata->len, &indicator);
					if (indicator == SQL_NULL_DATA) {
						ast_str_set(&coldata, 0, "(nil)");
						res = SQL_SUCCESS;
					}

					if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
						ast_cli(a->fd, "SQL Get Data error %d!\n[%s]\n\n", res, sql->str);
						SQLCloseCursor(stmt);
						SQLFreeHandle(SQL_HANDLE_STMT, stmt);
						ast_odbc_release_obj(obj);
						AST_RWLIST_UNLOCK(&queries);
						return CLI_SUCCESS;
					}

					ast_cli(a->fd, "%-20.20s  %s\n", colname, coldata->str);
				}
				/* Get next row */
				res = SQLFetch(stmt);
				if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
					break;
				}
				ast_cli(a->fd, "%-20.20s  %s\n", "----------", "----------");
				rows++;
			}
			SQLCloseCursor(stmt);
			SQLFreeHandle(SQL_HANDLE_STMT, stmt);
			ast_odbc_release_obj(obj);
			ast_cli(a->fd, "Returned %d rows.  Query executed on handle %d [%s]\n", rows, dsn, query->readhandle[dsn]);
			break;
		}
		ast_odbc_release_obj(obj);

		if (!executed) {
			ast_cli(a->fd, "Failed to execute query. [%s]\n", sql->str);
		}
	} else { /* No execution, just print out the resulting SQL */
		ast_cli(a->fd, "%s\n", sql->str);
	}
	AST_RWLIST_UNLOCK(&queries);
	return CLI_SUCCESS;
}

static char *cli_odbc_write(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	AST_DECLARE_APP_ARGS(values,
		AST_APP_ARG(field)[100];
	);
	AST_DECLARE_APP_ARGS(args,
		AST_APP_ARG(field)[100];
	);
	struct ast_str *sql = ast_str_thread_get(&sql_buf, 16);
	char *char_args, *char_values, varname[10];
	struct acf_odbc_query *query;
	struct ast_channel *chan;
	int i;

	switch (cmd) {
	case CLI_INIT:
		e->command = "odbc write";
		e->usage =
			"Usage: odbc write <name> <args> <value> [exec]\n"
			"       Evaluates the SQL provided in the ODBC function <name>, and\n"
			"       optionally executes the function.  This function is intended for\n"
			"       testing purposes.  Remember to quote arguments containing spaces.\n";
		return NULL;
	case CLI_GENERATE:
		if (a->pos == 2) {
			int wordlen = strlen(a->word), which = 0;
			/* Complete function name */
			AST_RWLIST_RDLOCK(&queries);
			AST_RWLIST_TRAVERSE(&queries, query, list) {
				if (!strncasecmp(query->acf->name, a->word, wordlen)) {
					if (++which > a->n) {
						char *res = ast_strdup(query->acf->name);
						AST_RWLIST_UNLOCK(&queries);
						return res;
					}
				}
			}
			AST_RWLIST_UNLOCK(&queries);
			return NULL;
		} else if (a->pos == 5) {
			return a->n == 0 ? ast_strdup("exec") : NULL;
		} else {
			return NULL;
		}
	}

	if (a->argc < 5 || a->argc > 6) {
		return CLI_SHOWUSAGE;
	}

	AST_RWLIST_RDLOCK(&queries);
	AST_RWLIST_TRAVERSE(&queries, query, list) {
		if (!strcmp(query->acf->name, a->argv[2])) {
			break;
		}
	}

	if (!query) {
		ast_cli(a->fd, "No such query '%s'\n", a->argv[2]);
		AST_RWLIST_UNLOCK(&queries);
		return CLI_SHOWUSAGE;
	}

	if (ast_strlen_zero(query->sql_write)) {
		ast_cli(a->fd, "The function %s has no writesql parameter.\n", a->argv[2]);
		AST_RWLIST_UNLOCK(&queries);
		return CLI_SUCCESS;
	}

	ast_str_make_space(&sql, strlen(query->sql_write) * 2 + 300);

	/* Evaluate function */
	char_args = ast_strdupa(a->argv[3]);
	char_values = ast_strdupa(a->argv[4]);

	chan = ast_channel_alloc(0, 0, "", "", "", "", "", 0, "Bogus/func_odbc");

	AST_STANDARD_APP_ARGS(args, char_args);
	for (i = 0; i < args.argc; i++) {
		snprintf(varname, sizeof(varname), "ARG%d", i + 1);
		pbx_builtin_pushvar_helper(chan, varname, args.field[i]);
	}

	/* Parse values, just like arguments */
	AST_STANDARD_APP_ARGS(values, char_values);
	for (i = 0; i < values.argc; i++) {
		snprintf(varname, sizeof(varname), "VAL%d", i + 1);
		pbx_builtin_pushvar_helper(chan, varname, values.field[i]);
	}

	/* Additionally set the value as a whole (but push an empty string if value is NULL) */
	pbx_builtin_pushvar_helper(chan, "VALUE", S_OR(a->argv[4], ""));
	pbx_substitute_variables_helper(chan, query->sql_write, sql->str, sql->len - 1);
	ast_debug(1, "SQL is %s\n", sql->str);
	ast_channel_free(chan);

	if (a->argc == 6 && !strcmp(a->argv[5], "exec")) {
		/* Execute the query */
		struct odbc_obj *obj = NULL;
		int dsn, executed = 0;
		SQLHSTMT stmt;
		SQLLEN rows = -1;

		for (dsn = 0; dsn < 5; dsn++) {
			if (ast_strlen_zero(query->writehandle[dsn])) {
				continue;
			}
			if (!(obj = ast_odbc_request_obj(query->writehandle[dsn], 0))) {
				continue;
			}
			if (!(stmt = ast_odbc_direct_execute(obj, generic_execute, sql->str))) {
				ast_odbc_release_obj(obj);
				continue;
			}

			SQLRowCount(stmt, &rows);
			SQLCloseCursor(stmt);
			SQLFreeHandle(SQL_HANDLE_STMT, stmt);
			ast_odbc_release_obj(obj);
			ast_cli(a->fd, "Affected %d rows.  Query executed on handle %d [%s]\n", (int)rows, dsn, query->writehandle[dsn]);
			executed = 1;
			break;
		}

		if (!executed) {
			ast_cli(a->fd, "Failed to execute query.\n");
		}
	} else { /* No execution, just print out the resulting SQL */
		ast_cli(a->fd, "%s\n", sql->str);
	}
	AST_RWLIST_UNLOCK(&queries);
	return CLI_SUCCESS;
}

static struct ast_cli_entry cli_func_odbc[] = {
	AST_CLI_DEFINE(cli_odbc_write, "Test setting a func_odbc function"),
	AST_CLI_DEFINE(cli_odbc_read, "Test reading a func_odbc function"),
};

static int load_module(void)
{
	int res = 0;
	struct ast_config *cfg;
	char *catg;
	struct ast_flags config_flags = { 0 };

	res |= ast_custom_function_register(&fetch_function);
	res |= ast_register_application(app_odbcfinish, exec_odbcfinish, syn_odbcfinish, desc_odbcfinish);
	AST_RWLIST_WRLOCK(&queries);

	cfg = ast_config_load(config, config_flags);
	if (!cfg || cfg == CONFIG_STATUS_FILEINVALID) {
		ast_log(LOG_NOTICE, "Unable to load config for func_odbc: %s\n", config);
		AST_RWLIST_UNLOCK(&queries);
		return AST_MODULE_LOAD_DECLINE;
	}

	for (catg = ast_category_browse(cfg, NULL);
	     catg;
	     catg = ast_category_browse(cfg, catg)) {
		struct acf_odbc_query *query = NULL;
		int err;

		if ((err = init_acf_query(cfg, catg, &query))) {
			if (err == ENOMEM)
				ast_log(LOG_ERROR, "Out of memory\n");
			else if (err == EINVAL)
				ast_log(LOG_ERROR, "Invalid parameters for category %s\n", catg);
			else
				ast_log(LOG_ERROR, "%s (%d)\n", strerror(err), err);
		} else {
			AST_RWLIST_INSERT_HEAD(&queries, query, list);
			ast_custom_function_register(query->acf);
		}
	}

	ast_config_destroy(cfg);
	res |= ast_custom_function_register(&escape_function);
	ast_cli_register_multiple(cli_func_odbc, ARRAY_LEN(cli_func_odbc));

	AST_RWLIST_UNLOCK(&queries);
	return res;
}

static int unload_module(void)
{
	struct acf_odbc_query *query;
	int res = 0;

	AST_RWLIST_WRLOCK(&queries);
	while (!AST_RWLIST_EMPTY(&queries)) {
		query = AST_RWLIST_REMOVE_HEAD(&queries, list);
		ast_custom_function_unregister(query->acf);
		free_acf_query(query);
	}

	res |= ast_custom_function_unregister(&escape_function);
	res |= ast_custom_function_unregister(&fetch_function);
	res |= ast_unregister_application(app_odbcfinish);
	ast_cli_unregister_multiple(cli_func_odbc, ARRAY_LEN(cli_func_odbc));

	/* Allow any threads waiting for this lock to pass (avoids a race) */
	AST_RWLIST_UNLOCK(&queries);
	usleep(1);
	AST_RWLIST_WRLOCK(&queries);

	AST_RWLIST_UNLOCK(&queries);
	return 0;
}

static int reload(void)
{
	int res = 0;
	struct ast_config *cfg;
	struct acf_odbc_query *oldquery;
	char *catg;
	struct ast_flags config_flags = { CONFIG_FLAG_FILEUNCHANGED };

	cfg = ast_config_load(config, config_flags);
	if (cfg == CONFIG_STATUS_FILEUNCHANGED || cfg == CONFIG_STATUS_FILEINVALID)
		return 0;

	AST_RWLIST_WRLOCK(&queries);

	while (!AST_RWLIST_EMPTY(&queries)) {
		oldquery = AST_RWLIST_REMOVE_HEAD(&queries, list);
		ast_custom_function_unregister(oldquery->acf);
		free_acf_query(oldquery);
	}

	if (!cfg) {
		ast_log(LOG_WARNING, "Unable to load config for func_odbc: %s\n", config);
		goto reload_out;
	}

	for (catg = ast_category_browse(cfg, NULL);
	     catg;
	     catg = ast_category_browse(cfg, catg)) {
		struct acf_odbc_query *query = NULL;

		if (init_acf_query(cfg, catg, &query)) {
			ast_log(LOG_ERROR, "Cannot initialize query %s\n", catg);
		} else {
			AST_RWLIST_INSERT_HEAD(&queries, query, list);
			ast_custom_function_register(query->acf);
		}
	}

	ast_config_destroy(cfg);
reload_out:
	AST_RWLIST_UNLOCK(&queries);
	return res;
}

/* XXX need to revise usecount - set if query_lock is set */

AST_MODULE_INFO(ASTERISK_GPL_KEY, AST_MODFLAG_DEFAULT, "ODBC lookups",
		.load = load_module,
		.unload = unload_module,
		.reload = reload,
	       );

