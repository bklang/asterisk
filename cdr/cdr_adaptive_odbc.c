/*
 * Asterisk -- An open source telephony toolkit.
 *
 * Copyright (C) 2007, Tilghman Lesher
 *
 * Tilghman Lesher <cdr_adaptive_odbc__v1@the-tilghman.com>
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
 * \brief Adaptive ODBC CDR backend
 * 
 * \author Tilghman Lesher <cdr_adaptive_odbc__v1@the-tilghman.com>
 * \ingroup cdr_drivers
 */

/*** MODULEINFO
	<depend>unixodbc</depend>
 ***/

#include "asterisk.h"

ASTERISK_FILE_VERSION(__FILE__, "$Revision$")

#include <sys/types.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>

#include <sql.h>
#include <sqlext.h>
#include <sqltypes.h>

#include "asterisk/config.h"
#include "asterisk/options.h"
#include "asterisk/channel.h"
#include "asterisk/lock.h"
#include "asterisk/linkedlists.h"
#include "asterisk/res_odbc.h"
#include "asterisk/cdr.h"
#include "asterisk/module.h"
#include "asterisk/logger.h"

#define	CONFIG	"cdr_adaptive_odbc.conf"

static char *name = "Adaptive ODBC";
/* Optimization to reduce number of memory allocations */
static int maxsize = 512, maxsize2 = 512;

struct columns {
	char *name;
	char *cdrname;
	SQLSMALLINT type;
	SQLINTEGER size;
	SQLSMALLINT decimals;
	SQLSMALLINT radix;
	SQLSMALLINT nullable;
	SQLINTEGER octetlen;
	AST_LIST_ENTRY(columns) list;
};

struct tables {
	char *connection;
	char *table;
	AST_LIST_HEAD_NOLOCK(odbc_columns, columns) columns;
	AST_RWLIST_ENTRY(tables) list;
};

static AST_RWLIST_HEAD_STATIC(odbc_tables, tables);

static int load_config(void)
{
	struct ast_config *cfg;
	struct ast_variable *var;
	const char *tmp, *catg;
	struct tables *tableptr;
	struct columns *entry;
	struct odbc_obj *obj;
	char columnname[80];
	char connection[40];
	char table[40];
	int lenconnection, lentable;
	SQLLEN sqlptr;
	int res = 0;
	SQLHSTMT stmt = NULL;

	cfg = ast_config_load(CONFIG);
	if (!cfg) {
		ast_log(LOG_WARNING, "Unable to load " CONFIG ".  No adaptive ODBC CDRs.\n");
		return -1;
	}

	for (catg = ast_category_browse(cfg, NULL); catg; catg = ast_category_browse(cfg, catg)) {
		var = ast_variable_browse(cfg, catg);
		if (!var)
			continue;

		if (ast_strlen_zero(tmp = ast_variable_retrieve(cfg, catg, "connection"))) {
			ast_log(LOG_WARNING, "No connection parameter found in '%s'.  Skipping.\n", catg);
			continue;
		}
		ast_copy_string(connection, tmp, sizeof(connection));
		lenconnection = strlen(connection);

		/* When loading, we want to be sure we can connect. */
		obj = ast_odbc_request_obj(connection, 1);
		if (!obj) {
			ast_log(LOG_WARNING, "No such connection '%s' in the '%s' section of " CONFIG ".  Check res_odbc.conf.\n", connection, catg);
			continue;
		}

		if (ast_strlen_zero(tmp = ast_variable_retrieve(cfg, catg, "table"))) {
			ast_log(LOG_NOTICE, "No table name found.  Assuming 'cdr'.\n");
			tmp = "cdr";
		}
		ast_copy_string(table, tmp, sizeof(table));
		lentable = strlen(table);

		res = SQLAllocHandle(SQL_HANDLE_STMT, obj->con, &stmt);
		if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
			ast_log(LOG_WARNING, "SQL Alloc Handle failed on connection '%s'!\n", connection);
			ast_odbc_release_obj(obj);
			continue;
		}

		res = SQLColumns(stmt, NULL, 0, NULL, 0, (unsigned char *)table, SQL_NTS, (unsigned char *)"%", SQL_NTS);
		if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
			ast_log(LOG_ERROR, "Unable to query database columns on connection '%s'.  Skipping.\n", connection);
			ast_odbc_release_obj(obj);
			continue;
		}

		tableptr = ast_calloc(sizeof(char), sizeof(*tableptr) + lenconnection + 1 + lentable + 1);
		if (!tableptr) {
			ast_log(LOG_ERROR, "Out of memory creating entry for table '%s' on connection '%s'\n", table, connection);
			ast_odbc_release_obj(obj);
			res = -1;
			break;
		}

		tableptr->connection = (char *)tableptr + sizeof(*tableptr);
		tableptr->table = (char *)tableptr + sizeof(*tableptr) + lenconnection + 1;
		ast_copy_string(tableptr->connection, connection, lenconnection + 1);
		ast_copy_string(tableptr->table, table, lentable + 1);

		if (option_verbose > 2)
			ast_verbose(VERBOSE_PREFIX_3 "Found adaptive CDR table %s@%s.\n", tableptr->table, tableptr->connection);

		while ((res = SQLFetch(stmt)) != SQL_NO_DATA && res != SQL_ERROR) {
			char *cdrvar = "";

			SQLGetData(stmt,  4, SQL_C_CHAR, columnname, sizeof(columnname), &sqlptr);

			/* Is there an alias for this column? */

			/* NOTE: This seems like a non-optimal parse method, but I'm going
			 * for user configuration readability, rather than fast parsing. We
			 * really don't parse this file all that often, anyway.
			 */
			for (var = ast_variable_browse(cfg, catg); var; var = var->next) {
				if (strcasecmp(var->value, columnname) == 0) {
					char *tmp = ast_strdupa(var->name + 5);
					cdrvar = ast_strip(tmp);
					if (option_verbose > 2)
						ast_verbose(VERBOSE_PREFIX_3 "Found alias %s for column %s in %s@%s\n", cdrvar, columnname, tableptr->table, tableptr->connection);
					break;
				}
			}
			entry = ast_calloc(sizeof(char), sizeof(*entry) + strlen(columnname) + 1 + strlen(cdrvar) + 1);
			if (!entry) {
				ast_log(LOG_ERROR, "Out of memory creating entry for column '%s' in table '%s' on connection '%s'\n", columnname, table, connection);
				res = -1;
				break;
			}
			entry->name = (char *)entry + sizeof(*entry);
			strcpy(entry->name, columnname);

			if (!ast_strlen_zero(cdrvar)) {
				entry->cdrname = entry->name + strlen(columnname) + 1;
				strcpy(entry->cdrname, cdrvar);
			} else /* Point to same place as the column name */
				entry->cdrname = (char *)entry + sizeof(*entry);

			SQLGetData(stmt,  5, SQL_C_SHORT, &entry->type, sizeof(entry->type), NULL);
			SQLGetData(stmt,  7, SQL_C_LONG, &entry->size, sizeof(entry->size), NULL);
			SQLGetData(stmt,  9, SQL_C_SHORT, &entry->decimals, sizeof(entry->decimals), NULL);
			SQLGetData(stmt, 10, SQL_C_SHORT, &entry->radix, sizeof(entry->radix), NULL);
			SQLGetData(stmt, 11, SQL_C_SHORT, &entry->nullable, sizeof(entry->nullable), NULL);
			SQLGetData(stmt, 16, SQL_C_LONG, &entry->octetlen, sizeof(entry->octetlen), NULL);

			/* Insert column info into column list */
			AST_LIST_INSERT_TAIL(&(tableptr->columns), entry, list);
			res = 0;
		}

		ast_odbc_release_obj(obj);

		if (AST_LIST_FIRST(&(tableptr->columns)))
			AST_RWLIST_INSERT_TAIL(&odbc_tables, tableptr, list);
		else
			free(tableptr);
	}
	return res;
}

static int free_config(void)
{
	struct tables *table;
	struct columns *entry;
	while ((table = AST_RWLIST_REMOVE_HEAD(&odbc_tables, list))) {
		while ((entry = AST_LIST_REMOVE_HEAD(&(table->columns), list))) {
			ast_free(entry);
		}
		ast_free(table);
	}
	return 0;
}

static SQLHSTMT generic_prepare(struct odbc_obj *obj, void *data)
{
	int res, i;
	char *sql = data;
	SQLHSTMT stmt;
	SQLINTEGER nativeerror = 0, numfields = 0;
	SQLSMALLINT diagbytes = 0;
	unsigned char state[10], diagnostic[256];

	res = SQLAllocHandle (SQL_HANDLE_STMT, obj->con, &stmt);
	if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
		ast_log(LOG_WARNING, "SQL Alloc Handle failed!\n");
		return NULL;
	}

	res = SQLPrepare(stmt, (unsigned char *)sql, SQL_NTS);
	if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
		ast_log(LOG_WARNING, "SQL Prepare failed![%s]\n", sql);
		SQLGetDiagField(SQL_HANDLE_STMT, stmt, 1, SQL_DIAG_NUMBER, &numfields, SQL_IS_INTEGER, &diagbytes);
		for (i = 0; i < numfields; i++) {
			SQLGetDiagRec(SQL_HANDLE_STMT, stmt, i + 1, state, &nativeerror, diagnostic, sizeof(diagnostic), &diagbytes);
			ast_log(LOG_WARNING, "SQL Execute returned an error %d: %s: %s (%d)\n", res, state, diagnostic, diagbytes);
			if (i > 10) {
				ast_log(LOG_WARNING, "Oh, that was good.  There are really %d diagnostics?\n", (int)numfields);
				break;
			}
		}
		SQLFreeHandle (SQL_HANDLE_STMT, stmt);
		return NULL;
	}

	return stmt;
}

#define LENGTHEN_BUF1(size)														\
			do {																\
				/* Lengthen buffer, if necessary */								\
				if ((newsize = lensql + (size) + 3) > sizesql) {	\
					if ((tmp = ast_realloc(sql, (newsize / 512 + 1) * 512))) {	\
						sql = tmp;												\
						sizesql = (newsize / 512 + 1) * 512;					\
					} else {													\
						ast_log(LOG_ERROR, "Unable to allocate sufficient memory.  Insert CDR '%s:%s' failed.\n", tableptr->connection, tableptr->table); \
						ast_free(sql);											\
						ast_free(sql2);											\
						AST_RWLIST_UNLOCK(&odbc_tables);						\
						return -1;												\
					}															\
				}																\
			} while (0)

#define LENGTHEN_BUF2(size)														\
			do {																\
				if ((newsize = lensql2 + (size) + 3) > sizesql2) {				\
					if ((tmp = ast_realloc(sql2, (newsize / 512 + 1) * 512))) {	\
						sql2 = tmp;												\
						sizesql2 = (newsize / 512 + 1) * 512;					\
					} else {													\
						ast_log(LOG_ERROR, "Unable to allocate sufficient memory.  Insert CDR '%s:%s' failed.\n", tableptr->connection, tableptr->table);	\
						ast_free(sql);											\
						ast_free(sql2);											\
						AST_RWLIST_UNLOCK(&odbc_tables);						\
						return -1;												\
					}															\
				}																\
			} while (0)

static int odbc_log(struct ast_cdr *cdr)
{
	struct tables *tableptr;
	struct columns *entry;
	struct odbc_obj *obj;
	int lensql, lensql2, sizesql = maxsize, sizesql2 = maxsize2, newsize;
	/* Allocated, so we can realloc() */
	char *sql = ast_calloc(sizeof(char), sizesql), *sql2 = ast_calloc(sizeof(char), sizesql2), *tmp;
	char colbuf[1024], *colptr;
	SQLHSTMT stmt = NULL;
	SQLINTEGER rows = 0;

	if (AST_RWLIST_RDLOCK(&odbc_tables)) {
		ast_log(LOG_ERROR, "Unable to lock table list.  Insert CDR(s) failed.\n");
		return -1;
	}

	AST_LIST_TRAVERSE(&odbc_tables, tableptr, list) {
		lensql = snprintf(sql, sizesql, "INSERT INTO %s (", tableptr->table);
		lensql2 = snprintf(sql2, sizesql2, " VALUES (");

		AST_LIST_TRAVERSE(&(tableptr->columns), entry, list) {
			/* Check if we have a similarly named variable */
			ast_cdr_getvar(cdr, entry->cdrname, &colptr, colbuf, sizeof(colbuf), 0,
				(strcasecmp(entry->cdrname, "start") == 0 ||
				 strcasecmp(entry->cdrname, "answer") == 0 ||
				 strcasecmp(entry->cdrname, "end") == 0) ? 0 : 1);

			if (colptr) {
				LENGTHEN_BUF1(strlen(entry->name));

				switch (entry->type) {
				case SQL_CHAR:
				case SQL_VARCHAR:
				case SQL_LONGVARCHAR:
				case SQL_BINARY:
				case SQL_VARBINARY:
				case SQL_LONGVARBINARY:
				case SQL_GUID:
					/* For these two field names, get the rendered form, instead of the raw
					 * form (but only when we're dealing with a character-based field).
					 */
					if (strcasecmp(entry->name, "disposition") == 0)
						ast_cdr_getvar(cdr, entry->name, &colptr, colbuf, sizeof(colbuf), 0, 0);
					else if (strcasecmp(entry->name, "amaflags") == 0)
						ast_cdr_getvar(cdr, entry->name, &colptr, colbuf, sizeof(colbuf), 0, 0);

					/* Truncate too-long fields */
					if (entry->type != SQL_GUID) {
						if (strlen(colptr) > entry->octetlen)
							colptr[entry->octetlen] = '\0';
					}

					lensql += snprintf(sql + lensql, sizesql - lensql, "%s,", entry->name);
					LENGTHEN_BUF2(strlen(colptr));

					/* Encode value, with escaping */
					strcpy(sql2 + lensql2, "'");
					lensql2++;
					for (tmp = colptr; *tmp; tmp++) {
						if (*tmp == '\'') {
							strcpy(sql2 + lensql2, "''");
							lensql2 += 2;
						} else if (*tmp == '\\') {
							strcpy(sql2 + lensql2, "\\\\");
							lensql2 += 2;
						} else {
							sql2[lensql2++] = *tmp;
							sql2[lensql2] = '\0';
						}
					}
					strcpy(sql2 + lensql2, "',");
					lensql2 += 2;
					break;
				case SQL_TYPE_DATE:
					{
						int year = 0, month = 0, day = 0;
						if (sscanf(colptr, "%d-%d-%d", &year, &month, &day) != 3 || year <= 0 ||
							month <= 0 || month > 12 || day < 0 || day > 31 ||
							((month == 4 || month == 6 || month == 9 || month == 11) && day == 31) ||
							(month == 2 && year % 400 == 0 && day > 29) ||
							(month == 2 && year % 100 == 0 && day > 28) ||
							(month == 2 && year % 4 == 0 && day > 29) ||
							(month == 2 && year % 4 != 0 && day > 28)) {
							ast_log(LOG_WARNING, "CDR variable %s is not a valid date ('%s').\n", entry->name, colptr);
							break;
						}

						if (year > 0 && year < 100)
							year += 2000;

						lensql += snprintf(sql + lensql, sizesql - lensql, "%s,", entry->name);
						LENGTHEN_BUF2(10);
						lensql2 += snprintf(sql2 + lensql2, sizesql2 - lensql2, "'%04d-%02d-%02d',", year, month, day);
					}
					break;
				case SQL_TYPE_TIME:
					{
						int hour = 0, minute = 0, second = 0;
						int count = sscanf(colptr, "%d:%d:%d", &hour, &minute, &second);

						if ((count != 2 && count != 3) || hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 59) {
							ast_log(LOG_WARNING, "CDR variable %s is not a valid time ('%s').\n", entry->name, colptr);
							break;
						}

						lensql += snprintf(sql + lensql, sizesql - lensql, "%s,", entry->name);
						LENGTHEN_BUF2(8);
						lensql2 += snprintf(sql2 + lensql2, sizesql2 - lensql2, "'%02d:%02d:%02d',", hour, minute, second);
					}
					break;
				case SQL_TYPE_TIMESTAMP:
				case SQL_TIMESTAMP:
					{
						int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
						int count = sscanf(colptr, "%d-%d-%d %d:%d:%d", &year, &month, &day, &hour, &minute, &second);

						if ((count != 3 && count != 5 && count != 6) || year <= 0 ||
							month <= 0 || month > 12 || day < 0 || day > 31 ||
							((month == 4 || month == 6 || month == 9 || month == 11) && day == 31) ||
							(month == 2 && year % 400 == 0 && day > 29) ||
							(month == 2 && year % 100 == 0 && day > 28) ||
							(month == 2 && year % 4 == 0 && day > 29) ||
							(month == 2 && year % 4 != 0 && day > 28) ||
							hour > 23 || minute > 59 || second > 59 || hour < 0 || minute < 0 || second < 0) {
							ast_log(LOG_WARNING, "CDR variable %s is not a valid timestamp ('%s').\n", entry->name, colptr);
							break;
						}

						if (year > 0 && year < 100)
							year += 2000;

						lensql += snprintf(sql + lensql, sizesql - lensql, "%s,", entry->name);
						LENGTHEN_BUF2(19);
						lensql2 += snprintf(sql2 + lensql2, sizesql2 - lensql2, "'%04d-%02d-%02d %02d:%02d:%02d',", year, month, day, hour, minute, second);
					}
					break;
				case SQL_INTEGER:
					{
						int integer = 0;
						if (sscanf(colptr, "%d", &integer) != 1) {
							ast_log(LOG_WARNING, "CDR variable %s is not an integer.\n", entry->name);
							break;
						}

						lensql += snprintf(sql + lensql, sizesql - lensql, "%s,", entry->name);
						LENGTHEN_BUF2(12);
						lensql2 += snprintf(sql2 + lensql2, sizesql2 - lensql2, "%d,", integer);
					}
					break;
				case SQL_BIGINT:
					{
						long long integer = 0;
						if (sscanf(colptr, "%lld", &integer) != 1) {
							ast_log(LOG_WARNING, "CDR variable %s is not an integer.\n", entry->name);
							break;
						}

						lensql += snprintf(sql + lensql, sizesql - lensql, "%s,", entry->name);
						LENGTHEN_BUF2(24);
						lensql2 += snprintf(sql2 + lensql2, sizesql2 - lensql2, "%lld,", integer);
					}
					break;
				case SQL_SMALLINT:
					{
						short integer = 0;
						if (sscanf(colptr, "%hd", &integer) != 1) {
							ast_log(LOG_WARNING, "CDR variable %s is not an integer.\n", entry->name);
							break;
						}

						lensql += snprintf(sql + lensql, sizesql - lensql, "%s,", entry->name);
						LENGTHEN_BUF2(6);
						lensql2 += snprintf(sql2 + lensql2, sizesql2 - lensql2, "%d,", integer);
					}
					break;
				case SQL_TINYINT:
					{
						char integer = 0;
						if (sscanf(colptr, "%hhd", &integer) != 1) {
							ast_log(LOG_WARNING, "CDR variable %s is not an integer.\n", entry->name);
							break;
						}

						lensql += snprintf(sql + lensql, sizesql - lensql, "%s,", entry->name);
						LENGTHEN_BUF2(4);
						lensql2 += snprintf(sql2 + lensql2, sizesql2 - lensql2, "%d,", integer);
					}
					break;
				case SQL_BIT:
					{
						char integer = 0;
						if (sscanf(colptr, "%hhd", &integer) != 1) {
							ast_log(LOG_WARNING, "CDR variable %s is not an integer.\n", entry->name);
							break;
						}
						if (integer != 0)
							integer = 1;

						lensql += snprintf(sql + lensql, sizesql - lensql, "%s,", entry->name);
						LENGTHEN_BUF2(2);
						lensql2 += snprintf(sql2 + lensql2, sizesql2 - lensql2, "%d,", integer);
					}
					break;
				case SQL_NUMERIC:
				case SQL_DECIMAL:
					{
						double number = 0.0;
						if (sscanf(colptr, "%lf", &number) != 1) {
							ast_log(LOG_WARNING, "CDR variable %s is not an numeric type.\n", entry->name);
							break;
						}

						lensql += snprintf(sql + lensql, sizesql - lensql, "%s,", entry->name);
						LENGTHEN_BUF2(entry->decimals);
						lensql2 += snprintf(sql2 + lensql2, sizesql2 - lensql2, "%*.*lf,", entry->decimals, entry->radix, number);
					}
					break;
				case SQL_FLOAT:
				case SQL_REAL:
				case SQL_DOUBLE:
					{
						double number = 0.0;
						if (sscanf(colptr, "%lf", &number) != 1) {
							ast_log(LOG_WARNING, "CDR variable %s is not an numeric type.\n", entry->name);
							break;
						}

						lensql += snprintf(sql + lensql, sizesql - lensql, "%s,", entry->name);
						LENGTHEN_BUF2(entry->decimals);
						lensql2 += snprintf(sql2 + lensql2, sizesql2 - lensql2, "%lf,", number);
					}
					break;
				default:
					ast_log(LOG_WARNING, "Column type %d (field '%s:%s:%s') is unsupported at this time.\n", entry->type, tableptr->connection, tableptr->table, entry->name);
				}
			}
		}

		/* Concatenate the two constructed buffers */
		LENGTHEN_BUF1(lensql2);
		sql[lensql - 1] = ')';
		sql2[lensql2 - 1] = ')';
		strcat(sql + lensql, sql2);

		if (option_verbose > 10)
			ast_verbose(VERBOSE_PREFIX_4 "[%s]\n", sql);
		/* No need to check the connection now; we'll handle any failure in prepare_and_execute */
		obj = ast_odbc_request_obj(tableptr->connection, 0);
		if (obj) {
			stmt = ast_odbc_prepare_and_execute(obj, generic_prepare, sql);
			if (stmt) {
				SQLRowCount(stmt, &rows);
				SQLFreeHandle(SQL_HANDLE_STMT, stmt);
			}
			if (rows == 0) {
				ast_log(LOG_WARNING, "cdr_adaptive_odbc: Insert failed on '%s:%s'.  CDR failed: %s\n", tableptr->connection, tableptr->table, sql);
			}
			ast_odbc_release_obj(obj);
		} else {
			ast_log(LOG_WARNING, "cdr_adaptive_odbc: Unable to retrieve database handle for '%s:%s'.  CDR failed: %s\n", tableptr->connection, tableptr->table, sql);
		}
	}
	AST_RWLIST_UNLOCK(&odbc_tables);

	/* Next time, just allocate buffers that are that big to start with. */
	if (sizesql > maxsize)
		maxsize = sizesql;
	if (sizesql2 > maxsize2)
		maxsize2 = sizesql2;

	ast_free(sql);
	ast_free(sql2);
	return 0;
}

static int unload_module(void)
{
	ast_cdr_unregister(name);
	usleep(1);
	if (AST_RWLIST_WRLOCK(&odbc_tables)) {
		ast_cdr_register(name, ast_module_info->description, odbc_log);
		ast_log(LOG_ERROR, "Unable to lock column list.  Unload failed.\n");
		return -1;
	}

	free_config();
	AST_RWLIST_UNLOCK(&odbc_tables);
	return 0;
}

static int load_module(void)
{
	if (AST_RWLIST_WRLOCK(&odbc_tables)) {
		ast_log(LOG_ERROR, "Unable to lock column list.  Load failed.\n");
		return 0;
	}

	load_config();
	AST_RWLIST_UNLOCK(&odbc_tables);
	ast_cdr_register(name, ast_module_info->description, odbc_log);
	return 0;
}

static int reload(void)
{
	if (AST_RWLIST_WRLOCK(&odbc_tables)) {
		ast_log(LOG_ERROR, "Unable to lock column list.  Reload failed.\n");
		return -1;
	}

	free_config();
	load_config();
	AST_RWLIST_UNLOCK(&odbc_tables);
	return 0;
}

AST_MODULE_INFO(ASTERISK_GPL_KEY, AST_MODFLAG_DEFAULT, "Adaptive ODBC CDR backend",
	.load = load_module,
	.unload = unload_module,
	.reload = reload,
);

