/*
 * Asterisk -- A telephony toolkit for Linux.
 *
 * Copyright (C) 1999-2005, Digium, Inc.
 * 
 * Manuel Guesdon <mguesdon@oxymium.net> - PostgreSQL RealTime Driver Author/Adaptor
 * Mark Spencer <markster@digium.com>  - Asterisk Author
 * Matthew Boehm <mboehm@cytelcom.com> - MySQL RealTime Driver Author
 *
 * res_config_pgsql.c <PostgreSQL plugin for RealTime configuration engine>
 *
 * v1.0   - (07-11-05) - Initial version based on res_config_mysql v2.0
 */

/*! \file
 *
 * \brief PostgreSQL plugin for Asterisk RealTime Architecture
 *
 * \author Mark Spencer <markster@digium.com>
 * \author Manuel Guesdon <mguesdon@oxymium.net> - PostgreSQL RealTime Driver Author/Adaptor
 *
 * \arg http://www.postgresql.org
 */

/*** MODULEINFO
	<depend>pgsql</depend>
 ***/

#include "asterisk.h"

ASTERISK_FILE_VERSION(__FILE__, "$Revision$")

#include <libpq-fe.h>			/* PostgreSQL */

#include "asterisk/file.h"
#include "asterisk/channel.h"
#include "asterisk/pbx.h"
#include "asterisk/config.h"
#include "asterisk/module.h"
#include "asterisk/lock.h"
#include "asterisk/utils.h"
#include "asterisk/cli.h"

AST_MUTEX_DEFINE_STATIC(pgsql_lock);

#define RES_CONFIG_PGSQL_CONF "res_pgsql.conf"

PGconn *pgsqlConn = NULL;

#define MAX_DB_OPTION_SIZE 64

struct columns {
	char *name;
	char *type;
	int len;
	unsigned int notnull:1;
	unsigned int hasdefault:1;
	AST_LIST_ENTRY(columns) list;
};

struct tables {
	ast_mutex_t lock;
	AST_LIST_HEAD_NOLOCK(psql_columns, columns) columns;
	AST_LIST_ENTRY(tables) list;
	char name[0];
};

static AST_LIST_HEAD_STATIC(psql_tables, tables);

static char dbhost[MAX_DB_OPTION_SIZE] = "";
static char dbuser[MAX_DB_OPTION_SIZE] = "";
static char dbpass[MAX_DB_OPTION_SIZE] = "";
static char dbname[MAX_DB_OPTION_SIZE] = "";
static char dbsock[MAX_DB_OPTION_SIZE] = "";
static int dbport = 5432;
static time_t connect_time = 0;

static int parse_config(int reload);
static int pgsql_reconnect(const char *database);
static char *handle_cli_realtime_pgsql_status(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a);
static char *handle_cli_realtime_pgsql_cache(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a);

enum { RQ_WARN, RQ_CREATECLOSE, RQ_CREATECHAR } requirements;

static struct ast_cli_entry cli_realtime[] = {
	AST_CLI_DEFINE(handle_cli_realtime_pgsql_status, "Shows connection information for the PostgreSQL RealTime driver"),
	AST_CLI_DEFINE(handle_cli_realtime_pgsql_cache, "Shows cached tables within the PostgreSQL realtime driver"),
};

static void destroy_table(struct tables *table)
{
	struct columns *column;
	ast_mutex_lock(&table->lock);
	while ((column = AST_LIST_REMOVE_HEAD(&table->columns, list))) {
		ast_free(column);
	}
	ast_mutex_unlock(&table->lock);
	ast_mutex_destroy(&table->lock);
	ast_free(table);
}

static struct tables *find_table(const char *tablename)
{
	struct columns *column;
	struct tables *table;
	struct ast_str *sql = ast_str_create(330);
	char *pgerror;
	PGresult *result;
	char *fname, *ftype, *flen, *fnotnull, *fdef;
	int i, rows;

	AST_LIST_LOCK(&psql_tables);
	AST_LIST_TRAVERSE(&psql_tables, table, list) {
		if (!strcasecmp(table->name, tablename)) {
			ast_debug(1, "Found table in cache; now locking\n");
			ast_mutex_lock(&table->lock);
			ast_debug(1, "Lock cached table; now returning\n");
			AST_LIST_UNLOCK(&psql_tables);
			return table;
		}
	}

	ast_debug(1, "Table '%s' not found in cache, querying now\n", tablename);

	/* Not found, scan the table */
	ast_str_set(&sql, 0, "SELECT a.attname, t.typname, a.attlen, a.attnotnull, d.adsrc FROM pg_class c, pg_type t, pg_attribute a LEFT OUTER JOIN pg_attrdef d ON a.atthasdef AND d.adrelid = a.attrelid AND d.adnum = a.attnum WHERE c.oid = a.attrelid AND a.atttypid = t.oid AND (a.attnum > 0) AND c.relname = '%s' ORDER BY c.relname, attnum", tablename);
	result = PQexec(pgsqlConn, sql->str);
	ast_debug(1, "Query of table structure complete.  Now retrieving results.\n");
	if (PQresultStatus(result) != PGRES_TUPLES_OK) {
		pgerror = PQresultErrorMessage(result);
		ast_log(LOG_ERROR, "Failed to query database columns: %s\n", pgerror);
		PQclear(result);
		AST_LIST_UNLOCK(&psql_tables);
		return NULL;
	}

	if (!(table = ast_calloc(1, sizeof(*table) + strlen(tablename) + 1))) {
		ast_log(LOG_ERROR, "Unable to allocate memory for new table structure\n");
		AST_LIST_UNLOCK(&psql_tables);
		return NULL;
	}
	strcpy(table->name, tablename); /* SAFE */
	ast_mutex_init(&table->lock);
	AST_LIST_HEAD_INIT_NOLOCK(&table->columns);
	
	rows = PQntuples(result);
	for (i = 0; i < rows; i++) {
		fname = PQgetvalue(result, i, 0);
		ftype = PQgetvalue(result, i, 1);
		flen = PQgetvalue(result, i, 2);
		fnotnull = PQgetvalue(result, i, 3);
		fdef = PQgetvalue(result, i, 4);
		ast_verb(4, "Found column '%s' of type '%s'\n", fname, ftype);

		if (!(column = ast_calloc(1, sizeof(*column) + strlen(fname) + strlen(ftype) + 2))) {
			ast_log(LOG_ERROR, "Unable to allocate column element for %s, %s\n", tablename, fname);
			destroy_table(table);
			AST_LIST_UNLOCK(&psql_tables);
			return NULL;
		}

		sscanf(flen, "%d", &column->len);
		column->name = (char *)column + sizeof(*column);
		column->type = (char *)column + sizeof(*column) + strlen(fname) + 1;
		strcpy(column->name, fname);
		strcpy(column->type, ftype);
		if (*fnotnull == 't') {
			column->notnull = 1;
		} else {
			column->notnull = 0;
		}
		if (!ast_strlen_zero(fdef)) {
			column->hasdefault = 1;
		} else {
			column->hasdefault = 0;
		}
		AST_LIST_INSERT_TAIL(&table->columns, column, list);
	}
	PQclear(result);

	AST_LIST_INSERT_TAIL(&psql_tables, table, list);
	ast_mutex_lock(&table->lock);
	AST_LIST_UNLOCK(&psql_tables);
	return table;
}

static struct ast_variable *realtime_pgsql(const char *database, const char *table, va_list ap)
{
	PGresult *result = NULL;
	int num_rows = 0, pgerror;
	char sql[256], escapebuf[513];
	char *stringp;
	char *chunk;
	char *op;
	const char *newparam, *newval;
	struct ast_variable *var = NULL, *prev = NULL;

	if (!table) {
		ast_log(LOG_WARNING, "PostgreSQL RealTime: No table specified.\n");
		return NULL;
	}

	/* Get the first parameter and first value in our list of passed paramater/value pairs */
	newparam = va_arg(ap, const char *);
	newval = va_arg(ap, const char *);
	if (!newparam || !newval) {
		ast_log(LOG_WARNING,
				"PostgreSQL RealTime: Realtime retrieval requires at least 1 parameter and 1 value to search on.\n");
		if (pgsqlConn) {
			PQfinish(pgsqlConn);
			pgsqlConn = NULL;
		};
		return NULL;
	}

	/* Create the first part of the query using the first parameter/value pairs we just extracted
	   If there is only 1 set, then we have our query. Otherwise, loop thru the list and concat */
	op = strchr(newparam, ' ') ? "" : " =";

	PQescapeStringConn(pgsqlConn, escapebuf, newval, (sizeof(escapebuf) - 1) / 2, &pgerror);
	if (pgerror) {
		ast_log(LOG_ERROR, "Postgres detected invalid input: '%s'\n", newval);
		va_end(ap);
		return NULL;
	}

	snprintf(sql, sizeof(sql), "SELECT * FROM %s WHERE %s%s '%s'", table, newparam, op,
			 escapebuf);
	while ((newparam = va_arg(ap, const char *))) {
		newval = va_arg(ap, const char *);
		if (!strchr(newparam, ' '))
			op = " =";
		else
			op = "";

		PQescapeStringConn(pgsqlConn, escapebuf, newval, (sizeof(escapebuf) - 1) / 2, &pgerror);
		if (pgerror) {
			ast_log(LOG_ERROR, "Postgres detected invalid input: '%s'\n", newval);
			va_end(ap);
			return NULL;
		}

		snprintf(sql + strlen(sql), sizeof(sql) - strlen(sql), " AND %s%s '%s'", newparam,
				 op, escapebuf);
	}
	va_end(ap);

	/* We now have our complete statement; Lets connect to the server and execute it. */
	ast_mutex_lock(&pgsql_lock);
	if (!pgsql_reconnect(database)) {
		ast_mutex_unlock(&pgsql_lock);
		return NULL;
	}

	if (!(result = PQexec(pgsqlConn, sql))) {
		ast_log(LOG_WARNING,
				"PostgreSQL RealTime: Failed to query database. Check debug for more info.\n");
		ast_debug(1, "PostgreSQL RealTime: Query: %s\n", sql);
		ast_debug(1, "PostgreSQL RealTime: Query Failed because: %s\n", PQerrorMessage(pgsqlConn));
		ast_mutex_unlock(&pgsql_lock);
		return NULL;
	} else {
		ExecStatusType result_status = PQresultStatus(result);
		if (result_status != PGRES_COMMAND_OK
			&& result_status != PGRES_TUPLES_OK
			&& result_status != PGRES_NONFATAL_ERROR) {
			ast_log(LOG_WARNING,
					"PostgreSQL RealTime: Failed to query database. Check debug for more info.\n");
			ast_debug(1, "PostgreSQL RealTime: Query: %s\n", sql);
			ast_debug(1, "PostgreSQL RealTime: Query Failed because: %s (%s)\n",
						PQresultErrorMessage(result), PQresStatus(result_status));
			ast_mutex_unlock(&pgsql_lock);
			return NULL;
		}
	}

	ast_debug(1, "PostgreSQL RealTime: Result=%p Query: %s\n", result, sql);

	if ((num_rows = PQntuples(result)) > 0) {
		int i = 0;
		int rowIndex = 0;
		int numFields = PQnfields(result);
		char **fieldnames = NULL;

		ast_debug(1, "PostgreSQL RealTime: Found %d rows.\n", num_rows);

		if (!(fieldnames = ast_calloc(1, numFields * sizeof(char *)))) {
			ast_mutex_unlock(&pgsql_lock);
			PQclear(result);
			return NULL;
		}
		for (i = 0; i < numFields; i++)
			fieldnames[i] = PQfname(result, i);
		for (rowIndex = 0; rowIndex < num_rows; rowIndex++) {
			for (i = 0; i < numFields; i++) {
				stringp = PQgetvalue(result, rowIndex, i);
				while (stringp) {
					chunk = strsep(&stringp, ";");
					if (!ast_strlen_zero(ast_strip(chunk))) {
						if (prev) {
							prev->next = ast_variable_new(fieldnames[i], chunk, "");
							if (prev->next) {
								prev = prev->next;
							}
						} else {
							prev = var = ast_variable_new(fieldnames[i], chunk, "");
						}
					}
				}
			}
		}
		ast_free(fieldnames);
	} else {
		ast_debug(1, "Postgresql RealTime: Could not find any rows in table %s.\n", table);
	}

	ast_mutex_unlock(&pgsql_lock);
	PQclear(result);

	return var;
}

static struct ast_config *realtime_multi_pgsql(const char *database, const char *table, va_list ap)
{
	PGresult *result = NULL;
	int num_rows = 0, pgerror;
	char sql[256], escapebuf[513];
	const char *initfield = NULL;
	char *stringp;
	char *chunk;
	char *op;
	const char *newparam, *newval;
	struct ast_variable *var = NULL;
	struct ast_config *cfg = NULL;
	struct ast_category *cat = NULL;

	if (!table) {
		ast_log(LOG_WARNING, "PostgreSQL RealTime: No table specified.\n");
		return NULL;
	}

	if (!(cfg = ast_config_new()))
		return NULL;

	/* Get the first parameter and first value in our list of passed paramater/value pairs */
	newparam = va_arg(ap, const char *);
	newval = va_arg(ap, const char *);
	if (!newparam || !newval) {
		ast_log(LOG_WARNING,
				"PostgreSQL RealTime: Realtime retrieval requires at least 1 parameter and 1 value to search on.\n");
		if (pgsqlConn) {
			PQfinish(pgsqlConn);
			pgsqlConn = NULL;
		};
		return NULL;
	}

	initfield = ast_strdupa(newparam);
	if ((op = strchr(initfield, ' '))) {
		*op = '\0';
	}

	/* Create the first part of the query using the first parameter/value pairs we just extracted
	   If there is only 1 set, then we have our query. Otherwise, loop thru the list and concat */

	if (!strchr(newparam, ' '))
		op = " =";
	else
		op = "";

	PQescapeStringConn(pgsqlConn, escapebuf, newval, (sizeof(escapebuf) - 1) / 2, &pgerror);
	if (pgerror) {
		ast_log(LOG_ERROR, "Postgres detected invalid input: '%s'\n", newval);
		va_end(ap);
		return NULL;
	}

	snprintf(sql, sizeof(sql), "SELECT * FROM %s WHERE %s%s '%s'", table, newparam, op,
			 escapebuf);
	while ((newparam = va_arg(ap, const char *))) {
		newval = va_arg(ap, const char *);
		if (!strchr(newparam, ' '))
			op = " =";
		else
			op = "";

		PQescapeStringConn(pgsqlConn, escapebuf, newval, (sizeof(escapebuf) - 1) / 2, &pgerror);
		if (pgerror) {
			ast_log(LOG_ERROR, "Postgres detected invalid input: '%s'\n", newval);
			va_end(ap);
			return NULL;
		}

		snprintf(sql + strlen(sql), sizeof(sql) - strlen(sql), " AND %s%s '%s'", newparam,
				 op, escapebuf);
	}

	if (initfield) {
		snprintf(sql + strlen(sql), sizeof(sql) - strlen(sql), " ORDER BY %s", initfield);
	}

	va_end(ap);

	/* We now have our complete statement; Lets connect to the server and execute it. */
	ast_mutex_lock(&pgsql_lock);
	if (!pgsql_reconnect(database)) {
		ast_mutex_unlock(&pgsql_lock);
		return NULL;
	}

	if (!(result = PQexec(pgsqlConn, sql))) {
		ast_log(LOG_WARNING,
				"PostgreSQL RealTime: Failed to query database. Check debug for more info.\n");
		ast_debug(1, "PostgreSQL RealTime: Query: %s\n", sql);
		ast_debug(1, "PostgreSQL RealTime: Query Failed because: %s\n", PQerrorMessage(pgsqlConn));
		ast_mutex_unlock(&pgsql_lock);
		return NULL;
	} else {
		ExecStatusType result_status = PQresultStatus(result);
		if (result_status != PGRES_COMMAND_OK
			&& result_status != PGRES_TUPLES_OK
			&& result_status != PGRES_NONFATAL_ERROR) {
			ast_log(LOG_WARNING,
					"PostgreSQL RealTime: Failed to query database. Check debug for more info.\n");
			ast_debug(1, "PostgreSQL RealTime: Query: %s\n", sql);
			ast_debug(1, "PostgreSQL RealTime: Query Failed because: %s (%s)\n",
						PQresultErrorMessage(result), PQresStatus(result_status));
			ast_mutex_unlock(&pgsql_lock);
			return NULL;
		}
	}

	ast_debug(1, "PostgreSQL RealTime: Result=%p Query: %s\n", result, sql);

	if ((num_rows = PQntuples(result)) > 0) {
		int numFields = PQnfields(result);
		int i = 0;
		int rowIndex = 0;
		char **fieldnames = NULL;

		ast_debug(1, "PostgreSQL RealTime: Found %d rows.\n", num_rows);

		if (!(fieldnames = ast_calloc(1, numFields * sizeof(char *)))) {
			ast_mutex_unlock(&pgsql_lock);
			PQclear(result);
			return NULL;
		}
		for (i = 0; i < numFields; i++)
			fieldnames[i] = PQfname(result, i);

		for (rowIndex = 0; rowIndex < num_rows; rowIndex++) {
			var = NULL;
			if (!(cat = ast_category_new("","",99999)))
				continue;
			for (i = 0; i < numFields; i++) {
				stringp = PQgetvalue(result, rowIndex, i);
				while (stringp) {
					chunk = strsep(&stringp, ";");
					if (!ast_strlen_zero(ast_strip(chunk))) {
						if (initfield && !strcmp(initfield, fieldnames[i])) {
							ast_category_rename(cat, chunk);
						}
						var = ast_variable_new(fieldnames[i], chunk, "");
						ast_variable_append(cat, var);
					}
				}
			}
			ast_category_append(cfg, cat);
		}
		ast_free(fieldnames);
	} else {
		ast_log(LOG_WARNING,
				"PostgreSQL RealTime: Could not find any rows in table %s.\n", table);
	}

	ast_mutex_unlock(&pgsql_lock);
	PQclear(result);

	return cfg;
}

static int update_pgsql(const char *database, const char *tablename, const char *keyfield,
						const char *lookup, va_list ap)
{
	PGresult *result = NULL;
	int numrows = 0, pgerror;
	char escapebuf[513];
	const char *newparam, *newval;
	struct ast_str *sql = ast_str_create(100);
	struct tables *table;
	struct columns *column = NULL;

	if (!tablename) {
		ast_log(LOG_WARNING, "PostgreSQL RealTime: No table specified.\n");
		ast_free(sql);
		return -1;
	}

	if (!(table = find_table(tablename))) {
		ast_log(LOG_ERROR, "Table '%s' does not exist!!\n", tablename);
		ast_free(sql);
		return -1;
	}

	/* Get the first parameter and first value in our list of passed paramater/value pairs */
	newparam = va_arg(ap, const char *);
	newval = va_arg(ap, const char *);
	if (!newparam || !newval) {
		ast_log(LOG_WARNING,
				"PostgreSQL RealTime: Realtime retrieval requires at least 1 parameter and 1 value to search on.\n");
		if (pgsqlConn) {
			PQfinish(pgsqlConn);
			pgsqlConn = NULL;
		};
		ast_mutex_unlock(&table->lock);
		ast_free(sql);
		return -1;
	}

	/* Check that the column exists in the table */
	AST_LIST_TRAVERSE(&table->columns, column, list) {
		if (strcmp(column->name, newparam) == 0) {
			break;
		}
	}

	if (!column) {
		ast_log(LOG_ERROR, "PostgreSQL RealTime: Updating on column '%s', but that column does not exist within the table '%s'!\n", newparam, tablename);
		ast_mutex_unlock(&table->lock);
		ast_free(sql);
		return -1;
	}

	/* Create the first part of the query using the first parameter/value pairs we just extracted
	   If there is only 1 set, then we have our query. Otherwise, loop thru the list and concat */

	PQescapeStringConn(pgsqlConn, escapebuf, newval, (sizeof(escapebuf) - 1) / 2, &pgerror);
	if (pgerror) {
		ast_log(LOG_ERROR, "Postgres detected invalid input: '%s'\n", newval);
		va_end(ap);
		ast_mutex_unlock(&table->lock);
		ast_free(sql);
		return -1;
	}
	ast_str_set(&sql, 0, "UPDATE %s SET %s = '%s'", tablename, newparam, escapebuf);

	while ((newparam = va_arg(ap, const char *))) {
		newval = va_arg(ap, const char *);

		/* If the column is not within the table, then skip it */
		AST_LIST_TRAVERSE(&table->columns, column, list) {
			if (strcmp(column->name, newparam) == 0) {
				break;
			}
		}

		if (!column) {
			ast_log(LOG_WARNING, "Attempted to update column '%s' in table '%s', but column does not exist!\n", newparam, tablename);
			continue;
		}

		PQescapeStringConn(pgsqlConn, escapebuf, newval, (sizeof(escapebuf) - 1) / 2, &pgerror);
		if (pgerror) {
			ast_log(LOG_ERROR, "Postgres detected invalid input: '%s'\n", newval);
			va_end(ap);
			ast_mutex_unlock(&table->lock);
			ast_free(sql);
			return -1;
		}

		ast_str_append(&sql, 0, ", %s = '%s'", newparam, escapebuf);
	}
	va_end(ap);
	ast_mutex_unlock(&table->lock);

	PQescapeStringConn(pgsqlConn, escapebuf, lookup, (sizeof(escapebuf) - 1) / 2, &pgerror);
	if (pgerror) {
		ast_log(LOG_ERROR, "Postgres detected invalid input: '%s'\n", lookup);
		va_end(ap);
		ast_free(sql);
		return -1;
	}

	ast_str_append(&sql, 0, " WHERE %s = '%s'", keyfield, escapebuf);

	ast_debug(1, "PostgreSQL RealTime: Update SQL: %s\n", sql->str);

	/* We now have our complete statement; Lets connect to the server and execute it. */
	ast_mutex_lock(&pgsql_lock);
	if (!pgsql_reconnect(database)) {
		ast_mutex_unlock(&pgsql_lock);
		ast_free(sql);
		return -1;
	}

	if (!(result = PQexec(pgsqlConn, sql->str))) {
		ast_log(LOG_WARNING,
				"PostgreSQL RealTime: Failed to query database. Check debug for more info.\n");
		ast_debug(1, "PostgreSQL RealTime: Query: %s\n", sql->str);
		ast_debug(1, "PostgreSQL RealTime: Query Failed because: %s\n", PQerrorMessage(pgsqlConn));
		ast_mutex_unlock(&pgsql_lock);
		ast_free(sql);
		return -1;
	} else {
		ExecStatusType result_status = PQresultStatus(result);
		if (result_status != PGRES_COMMAND_OK
			&& result_status != PGRES_TUPLES_OK
			&& result_status != PGRES_NONFATAL_ERROR) {
			ast_log(LOG_WARNING,
					"PostgreSQL RealTime: Failed to query database. Check debug for more info.\n");
			ast_debug(1, "PostgreSQL RealTime: Query: %s\n", sql->str);
			ast_debug(1, "PostgreSQL RealTime: Query Failed because: %s (%s)\n",
						PQresultErrorMessage(result), PQresStatus(result_status));
			ast_mutex_unlock(&pgsql_lock);
			ast_free(sql);
			return -1;
		}
	}

	numrows = atoi(PQcmdTuples(result));
	ast_mutex_unlock(&pgsql_lock);
	ast_free(sql);

	ast_debug(1, "PostgreSQL RealTime: Updated %d rows on table: %s\n", numrows, tablename);

	/* From http://dev.pgsql.com/doc/pgsql/en/pgsql-affected-rows.html
	 * An integer greater than zero indicates the number of rows affected
	 * Zero indicates that no records were updated
	 * -1 indicates that the query returned an error (although, if the query failed, it should have been caught above.)
	 */

	if (numrows >= 0)
		return (int) numrows;

	return -1;
}

static int store_pgsql(const char *database, const char *table, va_list ap)
{
	PGresult *result = NULL;
	Oid insertid;
	char sql[256];
	char params[256];
	char vals[256];
	char buf[256];
	int pgresult;
	const char *newparam, *newval;

	if (!table) {
		ast_log(LOG_WARNING, "PostgreSQL RealTime: No table specified.\n");
		return -1;
	}

	/* Get the first parameter and first value in our list of passed paramater/value pairs */
	newparam = va_arg(ap, const char *);
	newval = va_arg(ap, const char *);
	if (!newparam || !newval) {
		ast_log(LOG_WARNING,
				"PostgreSQL RealTime: Realtime storage requires at least 1 parameter and 1 value to store.\n");
		if (pgsqlConn) {
			PQfinish(pgsqlConn);
			pgsqlConn = NULL;
		};
		return -1;
	}

	/* Must connect to the server before anything else, as the escape function requires the connection handle.. */
	ast_mutex_lock(&pgsql_lock);
	if (!pgsql_reconnect(database)) {
		ast_mutex_unlock(&pgsql_lock);
		return -1;
	}

	/* Create the first part of the query using the first parameter/value pairs we just extracted
	   If there is only 1 set, then we have our query. Otherwise, loop thru the list and concat */
	PQescapeStringConn(pgsqlConn, buf, newparam, sizeof(newparam), &pgresult);
	snprintf(params, sizeof(params), "%s", buf);
	PQescapeStringConn(pgsqlConn, buf, newval, sizeof(newval), &pgresult);
	snprintf(vals, sizeof(vals), "'%s'", buf);
	while ((newparam = va_arg(ap, const char *))) {
		newval = va_arg(ap, const char *);
		PQescapeStringConn(pgsqlConn, buf, newparam, sizeof(newparam), &pgresult);
		snprintf(params + strlen(params), sizeof(params) - strlen(params), ", %s", buf);
		PQescapeStringConn(pgsqlConn, buf, newval, sizeof(newval), &pgresult);
		snprintf(vals + strlen(vals), sizeof(vals) - strlen(vals), ", '%s'", buf);
	}
	va_end(ap);
	snprintf(sql, sizeof(sql), "INSERT INTO (%s) VALUES (%s)", params, vals);

	ast_debug(1, "PostgreSQL RealTime: Insert SQL: %s\n", sql);

	if (!(result = PQexec(pgsqlConn, sql))) {
		ast_log(LOG_WARNING,
				"PostgreSQL RealTime: Failed to query database. Check debug for more info.\n");
		ast_debug(1, "PostgreSQL RealTime: Query: %s\n", sql);
		ast_debug(1, "PostgreSQL RealTime: Query Failed because: %s\n", PQerrorMessage(pgsqlConn));
		ast_mutex_unlock(&pgsql_lock);
		return -1;
	} else {
		ExecStatusType result_status = PQresultStatus(result);
		if (result_status != PGRES_COMMAND_OK
			&& result_status != PGRES_TUPLES_OK
			&& result_status != PGRES_NONFATAL_ERROR) {
			ast_log(LOG_WARNING,
					"PostgreSQL RealTime: Failed to query database. Check debug for more info.\n");
			ast_debug(1, "PostgreSQL RealTime: Query: %s\n", sql);
			ast_debug(1, "PostgreSQL RealTime: Query Failed because: %s (%s)\n",
						PQresultErrorMessage(result), PQresStatus(result_status));
			ast_mutex_unlock(&pgsql_lock);
			return -1;
		}
	}

	insertid = PQoidValue(result);
	ast_mutex_unlock(&pgsql_lock);

	ast_debug(1, "PostgreSQL RealTime: row inserted on table: %s, id: %u\n", table, insertid);

	/* From http://dev.pgsql.com/doc/pgsql/en/pgsql-affected-rows.html
	 * An integer greater than zero indicates the number of rows affected
	 * Zero indicates that no records were updated
	 * -1 indicates that the query returned an error (although, if the query failed, it should have been caught above.)
	 */

	if (insertid >= 0)
		return (int) insertid;

	return -1;
}

static int destroy_pgsql(const char *database, const char *table, const char *keyfield, const char *lookup, va_list ap)
{
	PGresult *result = NULL;
	int numrows = 0;
	int pgresult;
	char sql[256];
	char buf[256], buf2[256];
	const char *newparam, *newval;

	if (!table) {
		ast_log(LOG_WARNING, "PostgreSQL RealTime: No table specified.\n");
		return -1;
	}

	/* Get the first parameter and first value in our list of passed paramater/value pairs */
	/*newparam = va_arg(ap, const char *);
	newval = va_arg(ap, const char *);
	if (!newparam || !newval) {*/
	if (ast_strlen_zero(keyfield) || ast_strlen_zero(lookup))  {
		ast_log(LOG_WARNING,
				"PostgreSQL RealTime: Realtime destroy requires at least 1 parameter and 1 value to search on.\n");
		if (pgsqlConn) {
			PQfinish(pgsqlConn);
			pgsqlConn = NULL;
		};
		return -1;
	}

	/* Must connect to the server before anything else, as the escape function requires the connection handle.. */
	ast_mutex_lock(&pgsql_lock);
	if (!pgsql_reconnect(database)) {
		ast_mutex_unlock(&pgsql_lock);
		return -1;
	}


	/* Create the first part of the query using the first parameter/value pairs we just extracted
	   If there is only 1 set, then we have our query. Otherwise, loop thru the list and concat */

	PQescapeStringConn(pgsqlConn, buf, keyfield, sizeof(keyfield), &pgresult);
	PQescapeStringConn(pgsqlConn, buf2, lookup, sizeof(lookup), &pgresult);
	snprintf(sql, sizeof(sql), "DELETE FROM %s WHERE %s = '%s'", table, buf, buf2);
	while ((newparam = va_arg(ap, const char *))) {
		newval = va_arg(ap, const char *);
		PQescapeStringConn(pgsqlConn, buf, newparam, sizeof(newparam), &pgresult);
		PQescapeStringConn(pgsqlConn, buf2, newval, sizeof(newval), &pgresult);
		snprintf(sql + strlen(sql), sizeof(sql) - strlen(sql), " AND %s = '%s'", buf, buf2);
	}
	va_end(ap);

	ast_debug(1, "PostgreSQL RealTime: Delete SQL: %s\n", sql);

	if (!(result = PQexec(pgsqlConn, sql))) {
		ast_log(LOG_WARNING,
				"PostgreSQL RealTime: Failed to query database. Check debug for more info.\n");
		ast_debug(1, "PostgreSQL RealTime: Query: %s\n", sql);
		ast_debug(1, "PostgreSQL RealTime: Query Failed because: %s\n", PQerrorMessage(pgsqlConn));
		ast_mutex_unlock(&pgsql_lock);
		return -1;
	} else {
		ExecStatusType result_status = PQresultStatus(result);
		if (result_status != PGRES_COMMAND_OK
			&& result_status != PGRES_TUPLES_OK
			&& result_status != PGRES_NONFATAL_ERROR) {
			ast_log(LOG_WARNING,
					"PostgreSQL RealTime: Failed to query database. Check debug for more info.\n");
			ast_debug(1, "PostgreSQL RealTime: Query: %s\n", sql);
			ast_debug(1, "PostgreSQL RealTime: Query Failed because: %s (%s)\n",
						PQresultErrorMessage(result), PQresStatus(result_status));
			ast_mutex_unlock(&pgsql_lock);
			return -1;
		}
	}

	numrows = atoi(PQcmdTuples(result));
	ast_mutex_unlock(&pgsql_lock);

	ast_debug(1, "PostgreSQL RealTime: Deleted %d rows on table: %s\n", numrows, table);

	/* From http://dev.pgsql.com/doc/pgsql/en/pgsql-affected-rows.html
	 * An integer greater than zero indicates the number of rows affected
	 * Zero indicates that no records were updated
	 * -1 indicates that the query returned an error (although, if the query failed, it should have been caught above.)
	 */

	if (numrows >= 0)
		return (int) numrows;

	return -1;
}


static struct ast_config *config_pgsql(const char *database, const char *table,
									   const char *file, struct ast_config *cfg,
									   struct ast_flags flags, const char *suggested_incl, const char *who_asked)
{
	PGresult *result = NULL;
	long num_rows;
	struct ast_variable *new_v;
	struct ast_category *cur_cat = NULL;
	char sqlbuf[1024] = "";
	char *sql = sqlbuf;
	size_t sqlleft = sizeof(sqlbuf);
	char last[80] = "";
	int last_cat_metric = 0;

	last[0] = '\0';

	if (!file || !strcmp(file, RES_CONFIG_PGSQL_CONF)) {
		ast_log(LOG_WARNING, "PostgreSQL RealTime: Cannot configure myself.\n");
		return NULL;
	}

	ast_build_string(&sql, &sqlleft, "SELECT category, var_name, var_val, cat_metric FROM %s ", table);
	ast_build_string(&sql, &sqlleft, "WHERE filename='%s' and commented=0", file);
	ast_build_string(&sql, &sqlleft, "ORDER BY cat_metric DESC, var_metric ASC, category, var_name ");

	ast_debug(1, "PostgreSQL RealTime: Static SQL: %s\n", sqlbuf);

	/* We now have our complete statement; Lets connect to the server and execute it. */
	ast_mutex_lock(&pgsql_lock);
	if (!pgsql_reconnect(database)) {
		ast_mutex_unlock(&pgsql_lock);
		return NULL;
	}

	if (!(result = PQexec(pgsqlConn, sqlbuf))) {
		ast_log(LOG_WARNING,
				"PostgreSQL RealTime: Failed to query database. Check debug for more info.\n");
		ast_debug(1, "PostgreSQL RealTime: Query: %s\n", sql);
		ast_debug(1, "PostgreSQL RealTime: Query Failed because: %s\n", PQerrorMessage(pgsqlConn));
		ast_mutex_unlock(&pgsql_lock);
		return NULL;
	} else {
		ExecStatusType result_status = PQresultStatus(result);
		if (result_status != PGRES_COMMAND_OK
			&& result_status != PGRES_TUPLES_OK
			&& result_status != PGRES_NONFATAL_ERROR) {
			ast_log(LOG_WARNING,
					"PostgreSQL RealTime: Failed to query database. Check debug for more info.\n");
			ast_debug(1, "PostgreSQL RealTime: Query: %s\n", sql);
			ast_debug(1, "PostgreSQL RealTime: Query Failed because: %s (%s)\n",
						PQresultErrorMessage(result), PQresStatus(result_status));
			ast_mutex_unlock(&pgsql_lock);
			return NULL;
		}
	}

	if ((num_rows = PQntuples(result)) > 0) {
		int rowIndex = 0;

		ast_debug(1, "PostgreSQL RealTime: Found %ld rows.\n", num_rows);

		for (rowIndex = 0; rowIndex < num_rows; rowIndex++) {
			char *field_category = PQgetvalue(result, rowIndex, 0);
			char *field_var_name = PQgetvalue(result, rowIndex, 1);
			char *field_var_val = PQgetvalue(result, rowIndex, 2);
			char *field_cat_metric = PQgetvalue(result, rowIndex, 3);
			if (!strcmp(field_var_name, "#include")) {
				if (!ast_config_internal_load(field_var_val, cfg, flags, "", who_asked)) {
					PQclear(result);
					ast_mutex_unlock(&pgsql_lock);
					return NULL;
				}
				continue;
			}

			if (strcmp(last, field_category) || last_cat_metric != atoi(field_cat_metric)) {
				cur_cat = ast_category_new(field_category, "", 99999);
				if (!cur_cat)
					break;
				strcpy(last, field_category);
				last_cat_metric = atoi(field_cat_metric);
				ast_category_append(cfg, cur_cat);
			}
			new_v = ast_variable_new(field_var_name, field_var_val, "");
			ast_variable_append(cur_cat, new_v);
		}
	} else {
		ast_log(LOG_WARNING,
				"PostgreSQL RealTime: Could not find config '%s' in database.\n", file);
	}

	PQclear(result);
	ast_mutex_unlock(&pgsql_lock);

	return cfg;
}

static int require_pgsql(const char *database, const char *tablename, va_list ap)
{
	struct columns *column;
	struct tables *table = find_table(tablename);
	char *elm;
	int type, size, res = 0;

	if (!table) {
		ast_log(LOG_WARNING, "Table %s not found in database.  This table should exist if you're using realtime.\n", tablename);
		return -1;
	}

	while ((elm = va_arg(ap, char *))) {
		type = va_arg(ap, require_type);
		size = va_arg(ap, int);
		AST_LIST_TRAVERSE(&table->columns, column, list) {
			if (strcmp(column->name, elm) == 0) {
				/* Char can hold anything, as long as it is large enough */
				if ((strncmp(column->type, "char", 4) == 0 || strncmp(column->type, "varchar", 7) == 0 || strcmp(column->type, "bpchar") == 0)) {
					if ((size > column->len) && column->len != -1) {
						ast_log(LOG_WARNING, "Column '%s' should be at least %d long, but is only %d long.\n", column->name, size, column->len);
						res = -1;
					}
				} else if (strncmp(column->type, "int", 3) == 0) {
					int typesize = atoi(column->type + 3);
					/* Integers can hold only other integers */
					if (type == RQ_INTEGER && ((typesize == 2 && size > 4) || (typesize == 4 && size > 10))) {
						ast_log(LOG_WARNING, "Column '%s' may not be large enough for the required data length: %d\n", column->name, size);
						res = -1;
					} else if (type != RQ_INTEGER) {
						ast_log(LOG_WARNING, "Column '%s' is of the incorrect type: (need %s(%d) but saw %s)\n", column->name, type == RQ_CHAR ? "char" : "something else ", size, column->type);
						res = -1;
					}
				} else if (strncmp(column->type, "float", 5) == 0 && type != RQ_INTEGER && type != RQ_FLOAT) {
					ast_log(LOG_WARNING, "Column %s cannot be a %s\n", column->name, column->type);
					res = -1;
				} else { /* There are other types that no module implements yet */
					ast_log(LOG_WARNING, "Possibly unsupported column type '%s' on column '%s'\n", column->type, column->name);
					res = -1;
				}
				break;
			}
		}

		if (!column) {
			if (requirements == RQ_WARN) {
				ast_log(LOG_WARNING, "Table %s requires a column '%s' of size '%d', but no such column exists.\n", tablename, elm, size);
			} else {
				struct ast_str *sql = ast_str_create(100), *fieldtype = ast_str_create(16);
				PGresult *res;

				if (requirements == RQ_CREATECHAR || type == RQ_CHAR) {
					ast_str_set(&fieldtype, 0, "CHAR(%d)", size);
				} else if (type == RQ_INTEGER) {
					ast_str_set(&fieldtype, 0, "INT%d", size < 5 ? 2 : (size < 11 ? 4 : 8));
				} else if (type == RQ_FLOAT) {
					ast_str_set(&fieldtype, 0, "FLOAT8");
				} else if (type == RQ_DATE) {
					ast_str_set(&fieldtype, 0, "DATE");
				} else if (type == RQ_DATETIME) {
					ast_str_set(&fieldtype, 0, "TIMESTAMP");
				} else {
					ast_free(sql);
					ast_free(fieldtype);
					continue;
				}
				ast_str_set(&sql, 0, "ALTER TABLE %s ADD COLUMN %s %s", tablename, elm, fieldtype->str);
				ast_debug(1, "About to lock pgsql_lock (running alter on table '%s' to add column '%s')\n", tablename, elm);

				ast_mutex_lock(&pgsql_lock);
				if (!pgsql_reconnect(database)) {
					ast_mutex_unlock(&pgsql_lock);
					ast_log(LOG_ERROR, "Unable to add column: %s\n", sql->str);
					ast_free(sql);
					ast_free(fieldtype);
					continue;
				}

				ast_debug(1, "About to run ALTER query on table '%s' to add column '%s'\n", tablename, elm);
				res = PQexec(pgsqlConn, sql->str);
				ast_debug(1, "Finished running ALTER query on table '%s'\n", tablename);
				if (PQresultStatus(res) != PGRES_COMMAND_OK) {
					ast_log(LOG_ERROR, "Unable to add column: %s\n", sql->str);
				}
				PQclear(res);
				ast_mutex_unlock(&pgsql_lock);

				ast_free(sql);
				ast_free(fieldtype);
			}
		}
	}
	ast_mutex_unlock(&table->lock);
	return res;
}

static int unload_pgsql(const char *database, const char *tablename)
{
	struct tables *cur;
	ast_debug(1, "About to lock table cache list\n");
	AST_LIST_LOCK(&psql_tables);
	ast_debug(1, "About to traverse table cache list\n");
	AST_LIST_TRAVERSE_SAFE_BEGIN(&psql_tables, cur, list) {
		if (strcmp(cur->name, tablename) == 0) {
			ast_debug(1, "About to remove matching cache entry\n");
			AST_LIST_REMOVE_CURRENT(list);
			ast_debug(1, "About to destroy matching cache entry\n");
			destroy_table(cur);
			ast_debug(1, "Cache entry destroyed\n");
			break;
		}
	}
	AST_LIST_TRAVERSE_SAFE_END
	AST_LIST_UNLOCK(&psql_tables);
	ast_debug(1, "About to return\n");
	return cur ? 0 : -1;
}

static struct ast_config_engine pgsql_engine = {
	.name = "pgsql",
	.load_func = config_pgsql,
	.realtime_func = realtime_pgsql,
	.realtime_multi_func = realtime_multi_pgsql,
	.store_func = store_pgsql,
	.destroy_func = destroy_pgsql,
	.update_func = update_pgsql,
	.require_func = require_pgsql,
	.unload_func = unload_pgsql,
};

static int load_module(void)
{
	if(!parse_config(0))
		return AST_MODULE_LOAD_DECLINE;

	ast_config_engine_register(&pgsql_engine);
	ast_verb(1, "PostgreSQL RealTime driver loaded.\n");
	ast_cli_register_multiple(cli_realtime, sizeof(cli_realtime) / sizeof(struct ast_cli_entry));

	return 0;
}

static int unload_module(void)
{
	struct tables *table;
	/* Acquire control before doing anything to the module itself. */
	ast_mutex_lock(&pgsql_lock);

	if (pgsqlConn) {
		PQfinish(pgsqlConn);
		pgsqlConn = NULL;
	}
	ast_cli_unregister_multiple(cli_realtime, sizeof(cli_realtime) / sizeof(struct ast_cli_entry));
	ast_config_engine_deregister(&pgsql_engine);
	ast_verb(1, "PostgreSQL RealTime unloaded.\n");

	/* Destroy cached table info */
	AST_LIST_LOCK(&psql_tables);
	while ((table = AST_LIST_REMOVE_HEAD(&psql_tables, list))) {
		destroy_table(table);
	}
	AST_LIST_UNLOCK(&psql_tables);

	/* Unlock so something else can destroy the lock. */
	ast_mutex_unlock(&pgsql_lock);

	return 0;
}

static int reload(void)
{
	parse_config(1);

	return 0;
}

static int parse_config(int reload)
{
	struct ast_config *config;
	const char *s;
	struct ast_flags config_flags = { reload ? CONFIG_FLAG_FILEUNCHANGED : 0 };

	if ((config = ast_config_load(RES_CONFIG_PGSQL_CONF, config_flags)) == CONFIG_STATUS_FILEUNCHANGED)
		return 0;

	if (!config) {
		ast_log(LOG_WARNING, "Unable to load config %s\n", RES_CONFIG_PGSQL_CONF);
		return 0;
	}

	ast_mutex_lock(&pgsql_lock);

	if (pgsqlConn) {
		PQfinish(pgsqlConn);
		pgsqlConn = NULL;
	}

	if (!(s = ast_variable_retrieve(config, "general", "dbuser"))) {
		ast_log(LOG_WARNING,
				"PostgreSQL RealTime: No database user found, using 'asterisk' as default.\n");
		strcpy(dbuser, "asterisk");
	} else {
		ast_copy_string(dbuser, s, sizeof(dbuser));
	}

	if (!(s = ast_variable_retrieve(config, "general", "dbpass"))) {
		ast_log(LOG_WARNING,
				"PostgreSQL RealTime: No database password found, using 'asterisk' as default.\n");
		strcpy(dbpass, "asterisk");
	} else {
		ast_copy_string(dbpass, s, sizeof(dbpass));
	}

	if (!(s = ast_variable_retrieve(config, "general", "dbhost"))) {
		ast_log(LOG_WARNING,
				"PostgreSQL RealTime: No database host found, using localhost via socket.\n");
		dbhost[0] = '\0';
	} else {
		ast_copy_string(dbhost, s, sizeof(dbhost));
	}

	if (!(s = ast_variable_retrieve(config, "general", "dbname"))) {
		ast_log(LOG_WARNING,
				"PostgreSQL RealTime: No database name found, using 'asterisk' as default.\n");
		strcpy(dbname, "asterisk");
	} else {
		ast_copy_string(dbname, s, sizeof(dbname));
	}

	if (!(s = ast_variable_retrieve(config, "general", "dbport"))) {
		ast_log(LOG_WARNING,
				"PostgreSQL RealTime: No database port found, using 5432 as default.\n");
		dbport = 5432;
	} else {
		dbport = atoi(s);
	}

	if (!ast_strlen_zero(dbhost)) {
		/* No socket needed */
	} else if (!(s = ast_variable_retrieve(config, "general", "dbsock"))) {
		ast_log(LOG_WARNING,
				"PostgreSQL RealTime: No database socket found, using '/tmp/pgsql.sock' as default.\n");
		strcpy(dbsock, "/tmp/pgsql.sock");
	} else {
		ast_copy_string(dbsock, s, sizeof(dbsock));
	}

	if (!(s = ast_variable_retrieve(config, "general", "requirements"))) {
		ast_log(LOG_WARNING,
				"PostgreSQL RealTime: no requirements setting found, using 'warn' as default.\n");
		requirements = RQ_WARN;
	} else if (!strcasecmp(s, "createclose")) {
		requirements = RQ_CREATECLOSE;
	} else if (!strcasecmp(s, "createchar")) {
		requirements = RQ_CREATECHAR;
	}

	ast_config_destroy(config);

	if (option_debug) {
		if (!ast_strlen_zero(dbhost)) {
			ast_debug(1, "PostgreSQL RealTime Host: %s\n", dbhost);
			ast_debug(1, "PostgreSQL RealTime Port: %i\n", dbport);
		} else {
			ast_debug(1, "PostgreSQL RealTime Socket: %s\n", dbsock);
		}
		ast_debug(1, "PostgreSQL RealTime User: %s\n", dbuser);
		ast_debug(1, "PostgreSQL RealTime Password: %s\n", dbpass);
		ast_debug(1, "PostgreSQL RealTime DBName: %s\n", dbname);
	}

	if (!pgsql_reconnect(NULL)) {
		ast_log(LOG_WARNING,
				"PostgreSQL RealTime: Couldn't establish connection. Check debug.\n");
		ast_debug(1, "PostgreSQL RealTime: Cannot Connect: %s\n", PQerrorMessage(pgsqlConn));
	}

	ast_verb(2, "PostgreSQL RealTime reloaded.\n");

	/* Done reloading. Release lock so others can now use driver. */
	ast_mutex_unlock(&pgsql_lock);

	return 1;
}

static int pgsql_reconnect(const char *database)
{
	char my_database[50];

	ast_copy_string(my_database, S_OR(database, dbname), sizeof(my_database));

	/* mutex lock should have been locked before calling this function. */

	if (pgsqlConn && PQstatus(pgsqlConn) != CONNECTION_OK) {
		PQfinish(pgsqlConn);
		pgsqlConn = NULL;
	}

	/* DB password can legitimately be 0-length */
	if ((!pgsqlConn) && (!ast_strlen_zero(dbhost) || !ast_strlen_zero(dbsock)) && !ast_strlen_zero(dbuser) && !ast_strlen_zero(my_database)) {
		struct ast_str *connInfo = ast_str_create(32);

		ast_str_set(&connInfo, 0, "host=%s port=%d dbname=%s user=%s",
			dbhost, dbport, my_database, dbuser);
		if (!ast_strlen_zero(dbpass))
			ast_str_append(&connInfo, 0, " password=%s", dbpass);

		ast_debug(1, "%u connInfo=%s\n", (unsigned int)connInfo->len, connInfo->str);
		pgsqlConn = PQconnectdb(connInfo->str);
		ast_debug(1, "%u connInfo=%s\n", (unsigned int)connInfo->len, connInfo->str);
		ast_free(connInfo);
		connInfo = NULL;

		ast_debug(1, "pgsqlConn=%p\n", pgsqlConn);
		if (pgsqlConn && PQstatus(pgsqlConn) == CONNECTION_OK) {
			ast_debug(1, "PostgreSQL RealTime: Successfully connected to database.\n");
			connect_time = time(NULL);
			return 1;
		} else {
			ast_log(LOG_ERROR,
					"PostgreSQL RealTime: Failed to connect database %s on %s: %s\n",
					dbname, dbhost, PQresultErrorMessage(NULL));
			return 0;
		}
	} else {
		ast_debug(1, "PostgreSQL RealTime: One or more of the parameters in the config does not pass our validity checks.\n");
		return 1;
	}
}

static char *handle_cli_realtime_pgsql_cache(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	struct tables *cur;
	int l, which;
	char *ret = NULL;

	switch (cmd) {
	case CLI_INIT:
		e->command = "realtime pgsql cache";
		e->usage =
			"Usage: realtime pgsql cache [<table>]\n"
			"       Shows table cache for the PostgreSQL RealTime driver\n";
		return NULL;
	case CLI_GENERATE:
		if (a->argc != 3) {
			return NULL;
		}
		l = strlen(a->word);
		which = 0;
		AST_LIST_LOCK(&psql_tables);
		AST_LIST_TRAVERSE(&psql_tables, cur, list) {
			if (!strncasecmp(a->word, cur->name, l) && ++which > a->n) {
				ret = ast_strdup(cur->name);
				break;
			}
		}
		AST_LIST_UNLOCK(&psql_tables);
		return ret;
	}

	if (a->argc == 3) {
		/* List of tables */
		AST_LIST_LOCK(&psql_tables);
		AST_LIST_TRAVERSE(&psql_tables, cur, list) {
			ast_cli(a->fd, "%s\n", cur->name);
		}
		AST_LIST_UNLOCK(&psql_tables);
	} else if (a->argc == 4) {
		/* List of columns */
		if ((cur = find_table(a->argv[3]))) {
			struct columns *col;
			ast_cli(a->fd, "Columns for Table Cache '%s':\n", a->argv[3]);
			ast_cli(a->fd, "%-20.20s %-20.20s %-3.3s %-8.8s\n", "Name", "Type", "Len", "Nullable");
			AST_LIST_TRAVERSE(&cur->columns, col, list) {
				ast_cli(a->fd, "%-20.20s %-20.20s %3d %-8.8s\n", col->name, col->type, col->len, col->notnull ? "NOT NULL" : "");
			}
			ast_mutex_unlock(&cur->lock);
		} else {
			ast_cli(a->fd, "No such table '%s'\n", a->argv[3]);
		}
	}
	return 0;
}

static char *handle_cli_realtime_pgsql_status(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	char status[256], status2[100] = "";
	int ctime = time(NULL) - connect_time;

	switch (cmd) {
	case CLI_INIT:
		e->command = "realtime pgsql status";
		e->usage =
			"Usage: realtime pgsql status\n"
			"       Shows connection information for the PostgreSQL RealTime driver\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}

	if (a->argc != 3)
		return CLI_SHOWUSAGE;

	if (pgsqlConn && PQstatus(pgsqlConn) == CONNECTION_OK) {
		if (!ast_strlen_zero(dbhost))
			snprintf(status, 255, "Connected to %s@%s, port %d", dbname, dbhost, dbport);
		else if (!ast_strlen_zero(dbsock))
			snprintf(status, 255, "Connected to %s on socket file %s", dbname, dbsock);
		else
			snprintf(status, 255, "Connected to %s@%s", dbname, dbhost);

		if (!ast_strlen_zero(dbuser))
			snprintf(status2, 99, " with username %s", dbuser);

		if (ctime > 31536000)
			ast_cli(a->fd, "%s%s for %d years, %d days, %d hours, %d minutes, %d seconds.\n",
					status, status2, ctime / 31536000, (ctime % 31536000) / 86400,
					(ctime % 86400) / 3600, (ctime % 3600) / 60, ctime % 60);
		else if (ctime > 86400)
			ast_cli(a->fd, "%s%s for %d days, %d hours, %d minutes, %d seconds.\n", status,
					status2, ctime / 86400, (ctime % 86400) / 3600, (ctime % 3600) / 60,
					ctime % 60);
		else if (ctime > 3600)
			ast_cli(a->fd, "%s%s for %d hours, %d minutes, %d seconds.\n", status, status2,
					ctime / 3600, (ctime % 3600) / 60, ctime % 60);
		else if (ctime > 60)
			ast_cli(a->fd, "%s%s for %d minutes, %d seconds.\n", status, status2, ctime / 60,
					ctime % 60);
		else
			ast_cli(a->fd, "%s%s for %d seconds.\n", status, status2, ctime);

		return CLI_SUCCESS;
	} else {
		return CLI_FAILURE;
	}
}

/* needs usecount semantics defined */
AST_MODULE_INFO(ASTERISK_GPL_KEY, AST_MODFLAG_GLOBAL_SYMBOLS, "PostgreSQL RealTime Configuration Driver",
		.load = load_module,
		.unload = unload_module,
		.reload = reload
	       );
