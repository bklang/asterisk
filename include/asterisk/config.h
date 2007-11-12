/*
 * Asterisk -- An open source telephony toolkit.
 *
 * Copyright (C) 1999 - 2005, Digium, Inc.
 *
 * Mark Spencer <markster@digium.com>
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
 * \brief Configuration File Parser
 */

#ifndef _ASTERISK_CONFIG_H
#define _ASTERISK_CONFIG_H

#if defined(__cplusplus) || defined(c_plusplus)
extern "C" {
#endif

#include "asterisk/utils.h"
#include <stdarg.h>

struct ast_config;

struct ast_category;

/*! Options for ast_config_load()
 */
enum {
	/*! Load the configuration, including comments */
	CONFIG_FLAG_WITHCOMMENTS  = (1 << 0),
	/*! On a reload, give us a -1 if the file hasn't changed. */
	CONFIG_FLAG_FILEUNCHANGED = (1 << 1),
	/*! Don't attempt to cache mtime on this config file. */
	CONFIG_FLAG_NOCACHE       = (1 << 2),
};

#define	CONFIG_STATUS_FILEUNCHANGED	(void *)-1

/*! \brief Structure for variables, used for configurations and for channel variables 
*/
struct ast_variable {
	char *name;
	char *value;
	char *file;
	int lineno;
	int object;		/*!< 0 for variable, 1 for object */
	int blanklines; 	/*!< Number of blanklines following entry */
	struct ast_comment *precomments;
	struct ast_comment *sameline;
	struct ast_comment *trailing; /*!< the last object in the list will get assigned any trailing comments when EOF is hit */
	struct ast_variable *next;
	char stuff[0];
};

typedef struct ast_config *config_load_func(const char *database, const char *table, const char *configfile, struct ast_config *config, struct ast_flags flags, const char *suggested_include_file);
typedef struct ast_variable *realtime_var_get(const char *database, const char *table, va_list ap);
typedef struct ast_config *realtime_multi_get(const char *database, const char *table, va_list ap);
typedef int realtime_update(const char *database, const char *table, const char *keyfield, const char *entity, va_list ap);
typedef int realtime_store(const char *database, const char *table, va_list ap);
typedef int realtime_destroy(const char *database, const char *table, const char *keyfield, const char *entity, va_list ap);

/*! \brief Configuration engine structure, used to define realtime drivers */
struct ast_config_engine {
	char *name;
	config_load_func *load_func;
	realtime_var_get *realtime_func;
	realtime_multi_get *realtime_multi_func;
	realtime_update *update_func;
	realtime_store *store_func;
	realtime_destroy *destroy_func;
	struct ast_config_engine *next;
};

/*! \brief Load a config file 
 * \param filename path of file to open.  If no preceding '/' character, path is considered relative to AST_CONFIG_DIR
 * Create a config structure from a given configuration file.
 * \param flags Optional flags:
 * CONFIG_FLAG_WITHCOMMENTS - load the file with comments intact;
 * CONFIG_FLAG_FILEUNCHANGED - check the file mtime and return CONFIG_STATUS_FILEUNCHANGED if the mtime is the same; or
 * CONFIG_FLAG_NOCACHE - don't cache file mtime (main purpose of this option is to save memory on temporary files).
 *
 * \retval an ast_config data structure on success
 * \retval NULL on error
 */
struct ast_config *ast_config_load(const char *filename, struct ast_flags flags);

/*! \brief Destroys a config 
 * \param config pointer to config data structure
 * Free memory associated with a given config
 *
 */
void ast_config_destroy(struct ast_config *config);

/*! \brief returns the root ast_variable of a config
 * \param config pointer to an ast_config data structure
 * \param cat name of the category for which you want the root
 *
 * Returns the category specified
 */
struct ast_variable *ast_category_root(struct ast_config *config, char *cat);

/*! \brief Goes through categories 
 * \param config Which config structure you wish to "browse"
 * \param prev A pointer to a previous category.
 * This function is kind of non-intuitive in it's use.  To begin, one passes NULL as the second argument.  It will return a pointer to the string of the first category in the file.  From here on after, one must then pass the previous usage's return value as the second pointer, and it will return a pointer to the category name afterwards.
 *
 * \retval a category on success
 * \retval NULL on failure/no-more-categories
 */
char *ast_category_browse(struct ast_config *config, const char *prev);

/*! 
 * \brief Goes through variables
 * Somewhat similar in intent as the ast_category_browse.
 * List variables of config file category
 *
 * \retval ast_variable list on success
 * \retval NULL on failure
 */
struct ast_variable *ast_variable_browse(const struct ast_config *config, const char *category);

/*! 
 * \brief Gets a variable 
 * \param config which (opened) config to use
 * \param category category under which the variable lies
 * \param variable which variable you wish to get the data for
 * Goes through a given config file in the given category and searches for the given variable
 *
 * \retval The variable value on success 
 * \retval NULL if unable to find it.
 */
const char *ast_variable_retrieve(const struct ast_config *config, const char *category, const char *variable);

/*! 
 * \brief Retrieve a category if it exists
 * \param config which config to use
 * \param category_name name of the category you're looking for
 * This will search through the categories within a given config file for a match.
 *
 * \retval pointer to category if found
 * \retval NULL if not.
 */
struct ast_category *ast_category_get(const struct ast_config *config, const char *category_name);

/*! 
 * \brief Check for category duplicates 
 * \param config which config to use
 * \param category_name name of the category you're looking for
 * This will search through the categories within a given config file for a match.
 *
 * \return non-zero if found
 */
int ast_category_exist(const struct ast_config *config, const char *category_name);

/*! 
 * \brief Retrieve realtime configuration 
 * \param family which family/config to lookup
 * This will use builtin configuration backends to look up a particular 
 * entity in realtime and return a variable list of its parameters.  Note
 * that unlike the variables in ast_config, the resulting list of variables
 * MUST be freed with ast_variables_destroy() as there is no container.
 */
struct ast_variable *ast_load_realtime(const char *family, ...);
struct ast_variable *ast_load_realtime_all(const char *family, ...);

/*! 
 * \brief Retrieve realtime configuration 
 * \param family which family/config to lookup
 * This will use builtin configuration backends to look up a particular 
 * entity in realtime and return a variable list of its parameters. Unlike
 * the ast_load_realtime, this function can return more than one entry and
 * is thus stored inside a taditional ast_config structure rather than 
 * just returning a linked list of variables.
 */
struct ast_config *ast_load_realtime_multientry(const char *family, ...);

/*! 
 * \brief Update realtime configuration 
 * \param family which family/config to be updated
 * \param keyfield which field to use as the key
 * \param lookup which value to look for in the key field to match the entry.
 * This function is used to update a parameter in realtime configuration space.
 *
 */
int ast_update_realtime(const char *family, const char *keyfield, const char *lookup, ...);

/*! 
 * \brief Create realtime configuration 
 * \param family which family/config to be created
 * This function is used to create a parameter in realtime configuration space.
 *
 */
int ast_store_realtime(const char *family, ...);

/*! 
 * \brief Destroy realtime configuration 
 * \param family which family/config to be destroyed
 * \param keyfield which field to use as the key
 * \param lookup which value to look for in the key field to match the entry.
 * This function is used to destroy an entry in realtime configuration space.
 * Additional params are used as keys.
 *
 */
int ast_destroy_realtime(const char *family, const char *keyfield, const char *lookup, ...);

/*! 
 * \brief Check if realtime engine is configured for family 
 * \param family which family/config to be checked
 * \return 1 if family is configured in realtime and engine exists
*/
int ast_check_realtime(const char *family);

/*! \brief Check if there's any realtime engines loaded */
int ast_realtime_enabled(void);

/*! \brief Free variable list 
 * \param var the linked list of variables to free
 * This function frees a list of variables.
 */
void ast_variables_destroy(struct ast_variable *var);

/*! \brief Register config engine */
int ast_config_engine_register(struct ast_config_engine *newconfig);

/*! \brief Deegister config engine */
int ast_config_engine_deregister(struct ast_config_engine *del);

int register_config_cli(void);
int read_config_maps(void);

struct ast_config *ast_config_new(void);
struct ast_category *ast_config_get_current_category(const struct ast_config *cfg);
void ast_config_set_current_category(struct ast_config *cfg, const struct ast_category *cat);
const char *ast_config_option(struct ast_config *cfg, const char *cat, const char *var);

struct ast_category *ast_category_new(const char *name, const char *in_file, int lineno);
void ast_category_append(struct ast_config *config, struct ast_category *cat);
int ast_category_delete(struct ast_config *cfg, const char *category);
void ast_category_destroy(struct ast_category *cat);
struct ast_variable *ast_category_detach_variables(struct ast_category *cat);
void ast_category_rename(struct ast_category *cat, const char *name);

struct ast_variable *ast_variable_new(const char *name, const char *value, const char *filename);
struct ast_config_include *ast_include_new(struct ast_config *conf, const char *from_file, const char *included_file, int is_exec, const char *exec_file, int from_lineno, char *real_included_file_name, int real_included_file_name_size);
struct ast_config_include *ast_include_find(struct ast_config *conf, const char *included_file);
void ast_include_rename(struct ast_config *conf, const char *from_file, const char *to_file);
void ast_variable_append(struct ast_category *category, struct ast_variable *variable);
int ast_variable_delete(struct ast_category *category, const char *variable, const char *match);
int ast_variable_update(struct ast_category *category, const char *variable, 
						const char *value, const char *match, unsigned int object);

int config_text_file_save(const char *filename, const struct ast_config *cfg, const char *generator);

struct ast_config *ast_config_internal_load(const char *configfile, struct ast_config *cfg, struct ast_flags flags, const char *suggested_incl_file);
/*! \brief Support code to parse config file arguments
 *
 * The function ast_parse_arg() provides a generic interface to parse
 * strings (e.g. numbers, network addresses and so on) in a flexible
 * way, e.g. by doing proper error and bound checks, provide default
 * values, and so on.
 * The function (described later) takes a string as an argument,
 * a set of flags to specify the result format and checks to perform,
 * a pointer to the result, and optionally some additional arguments.
 * It returns 0 on success, != 0 otherwise.
 *
 */
enum ast_parse_flags {
	/* low 4 bits of flags are used for the operand type */
	PARSE_TYPE	=	0x000f,
	/* numeric types, with optional default value and bound checks.
	 * Additional arguments are passed by value.
	 */
	PARSE_INT16	= 	0x0001,
	PARSE_INT32	= 	0x0002,
	PARSE_UINT16	= 	0x0003,
	PARSE_UINT32	= 	0x0004,
	/* Returns a struct sockaddr_in, with optional default value
	 * (passed by reference) and port handling (accept, ignore,
	 * require, forbid). The format is 'host.name[:port]'
	 */
	PARSE_INADDR	= 	0x0005,

	/* Other data types can be added as needed */

	/* If PARSE_DEFAULT is set, next argument is a default value
	 * which is returned in case of error. The argument is passed
	 * by value in case of numeric types, by reference in other cases.
 	 */
	PARSE_DEFAULT	=	0x0010,	/* assign default on error */

	/* Request a range check, applicable to numbers. Two additional
	 * arguments are passed by value, specifying the low-high end of
	 * the range (inclusive). An error is returned if the value
	 * is outside or inside the range, respectively.
	 */
	PARSE_IN_RANGE =	0x0020,	/* accept values inside a range */
	PARSE_OUT_RANGE =	0x0040,	/* accept values outside a range */

	/* Port handling, for sockaddr_in. accept/ignore/require/forbid
	 * port number after the hostname or address.
	 */
	PARSE_PORT_MASK =	0x0300, /* 0x000: accept port if present */
	PARSE_PORT_IGNORE =	0x0100, /* 0x100: ignore port if present */
	PARSE_PORT_REQUIRE =	0x0200, /* 0x200: require port number */
	PARSE_PORT_FORBID =	0x0300, /* 0x100: forbid port number */
};

/*! \brief The argument parsing routine.
 * \param arg the string to parse. It is not modified.
 * \param flags combination of ast_parse_flags to specify the
 *	return type and additional checks.
 * \param result pointer to the result. NULL is valid here, and can
 *	be used to perform only the validity checks.
 * \param ... extra arguments are required according to flags.
 * \retval 0 in case of success, != 0 otherwise.
 * \retval result returns the parsed value in case of success,
 *	the default value in case of error, or it is left unchanged
 *	in case of error and no default specified. Note that in certain
 *	cases (e.g. sockaddr_in, with multi-field return values) some
 *	of the fields in result may be changed even if an error occurs.
 *
 * Examples of use:
 *	ast_parse_arg("223", PARSE_INT32|PARSE_IN_RANGE,
 *		&a, -1000, 1000); 
 *              returns 0, a = 223
 *	ast_parse_arg("22345", PARSE_INT32|PARSE_IN_RANGE|PARSE_DEFAULT,
 *		&a, 9999, 10, 100);
 *              returns 1, a = 9999
 *      ast_parse_arg("22345ssf", PARSE_UINT32|PARSE_IN_RANGE, &b, 10, 100);
 *		returns 1, b unchanged
 *      ast_parse_arg("www.foo.biz:44", PARSE_INADDR, &sa);
 *		returns 0, sa contains address and port
 *      ast_parse_arg("www.foo.biz", PARSE_INADDR|PARSE_PORT_REQUIRE, &sa);
 *		returns 1 because port is missing, sa contains address
 */
int ast_parse_arg(const char *arg, enum ast_parse_flags flags,
        void *result, ...);

#if defined(__cplusplus) || defined(c_plusplus)
}
#endif

#endif /* _ASTERISK_CONFIG_H */
