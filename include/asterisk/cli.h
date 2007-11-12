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
 * \brief Standard Command Line Interface
 */

#ifndef _ASTERISK_CLI_H
#define _ASTERISK_CLI_H

#if defined(__cplusplus) || defined(c_plusplus)
extern "C" {
#endif

#include <stdarg.h>

#include "asterisk/linkedlists.h"

void ast_cli(int fd, const char *fmt, ...)
	__attribute__ ((format (printf, 2, 3)));

#define RESULT_SUCCESS		0
#define RESULT_SHOWUSAGE	1
#define RESULT_FAILURE		2

#define CLI_SUCCESS	(char *)RESULT_SUCCESS
#define CLI_SHOWUSAGE	(char *)RESULT_SHOWUSAGE
#define CLI_FAILURE	(char *)RESULT_FAILURE

#define AST_MAX_CMD_LEN 	16

#define AST_MAX_ARGS 64

#define AST_CLI_COMPLETE_EOF	"_EOF_"

/*!
 * In many cases we need to print singular or plural
 * words depending on a count. This macro helps us e.g.
 *     printf("we have %d object%s", n, ESS(n));
 */
#define ESS(x) ((x) == 1 ? "" : "s")

/*! \page CLI_command_API CLI command API

   CLI commands are described by a struct ast_cli_entry that contains
   all the components for their implementation.

   In the "old-style" format, the record must contain:
   - a NULL-terminated array of words constituting the command, e.g.
	{ "set", "debug", "on", NULL },
   - a summary string (short) and a usage string (longer);
   - a handler which implements the command itself, invoked with
     a file descriptor and argc/argv as typed by the user
   - a 'generator' function which, given a partial string, can
     generate legal completions for it.
   An example is

	int old_setdebug(int fd, int argc, char *argv[]);
	char *dbg_complete(const char *line, const char *word, int pos, int n);

	{ { "set", "debug", "on", NULL }, do_setdebug, "Enable debugging",
	set_debug_usage, dbg_complete },

   In the "new-style" format, all the above functionalities are implemented
   by a single function, and the arguments tell which output is required.
   The prototype is the following:

	char *new_setdebug(const struct ast_cli_entry *e, int cmd, struct ast_cli_args *a);

	...
	// this is how we create the entry to register 
	AST_CLI_DEFINE(new_setdebug, "short description")
	...

   To help the transition, we make the pointer to the struct ast_cli_entry
   available to old-style handlers via argv[-1].

   An example of new-style handler is the following

\code
static char *test_new_cli(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	static char *choices = { "one", "two", "three", NULL };

        switch (cmd) {
        case CLI_INIT:
		e->command = "do this well";
                e->usage =
			"Usage: do this well <arg>\n"
			"	typically multiline with body indented\n";
		return NULL;

        case CLI_GENERATE:
                if (a->pos > e->args)
                        return NULL;
        	return ast_cli_complete(a->word, choices, a->n);

        default:        
                // we are guaranteed to be called with argc >= e->args;
                if (a->argc > e->args + 1) // we accept one extra argument
                        return CLI_SHOWUSAGE;
                ast_cli(a->fd, "done this well for %s\n", e->args[argc-1]);
                return CLI_SUCCESS;
        }
}

\endcode
 
 */

/*! \brief calling arguments for new-style handlers. 
* \arg \ref CLI_command_API
*/
enum ast_cli_fn {
	CLI_INIT = -2,		/* return the usage string */
	CLI_GENERATE = -3,	/* behave as 'generator', remap argv to struct ast_cli_args */
	CLI_HANDLER = -4,	/* run the normal handler */
};

/* argument for new-style CLI handler */
struct ast_cli_args {
	int fd;
	int argc;
	char **argv;
	const char *line;	/* the current input line */
	const char *word;	/* the word we want to complete */
	int pos;		/* position of the word to complete */
	int n;			/* the iteration count (n-th entry we generate) */
};

struct ast_cli_entry;
typedef int (*old_cli_fn)(int fd, int argc, char *argv[]);
typedef char *(*new_cli_fn)(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a);

/*! \brief descriptor for a cli entry. 
 * \arg \ref CLI_command_API
 */
struct ast_cli_entry {
	char * const cmda[AST_MAX_CMD_LEN];	/*!< words making up the command.
		* set the first entry to NULL for a new-style entry. */

	/*! Handler for the command (fd for output, # of args, argument list).
	  Returns RESULT_SHOWUSAGE for improper arguments.
	  argv[] has argc 'useful' entries, and an additional NULL entry
	  at the end so that clients requiring NULL terminated arrays
	  can use it without need for copies.
	  You can overwrite argv or the strings it points to, but remember
	  that this memory is deallocated after the handler returns.
	 */
	old_cli_fn handler;

	const char *summary; /*!< Summary of the command (< 60 characters) */
	const char *usage; /*!< Detailed usage information */

	/*! Generate the n-th (starting from 0) possible completion
	  for a given 'word' following 'line' in position 'pos'.
	  'line' and 'word' must not be modified.
	  Must return a malloc'ed string with the n-th value when available,
	  or NULL if the n-th completion does not exist.
	  Typically, the function is called with increasing values for n
	  until a NULL is returned.
	 */
	char *(*generator)(const char *line, const char *word, int pos, int n);
	struct ast_cli_entry *deprecate_cmd;

	int inuse; /*!< For keeping track of usage */
	struct module *module;	/*!< module this belongs to */
	char *_full_cmd;	/*!< built at load time from cmda[] */
	int cmdlen;		/*!< len up to the first invalid char [<{% */
	/*! \brief This gets set in ast_cli_register()
	  It then gets set to something different when the deprecated command
	  is run for the first time (ie; after we warn the user that it's deprecated)
	 */
	int args;		/*!< number of non-null entries in cmda */
	char *command;		/*!< command, non-null for new-style entries */
	int deprecated;
	new_cli_fn new_handler;
	char *_deprecated_by;	/*!< copied from the "parent" _full_cmd, on deprecated commands */
	/*! For linking */
	AST_LIST_ENTRY(ast_cli_entry) list;
};

/* XXX the parser in gcc 2.95 gets confused if you don't put a space
 * between the last arg before VA_ARGS and the comma */
#define AST_CLI_DEFINE(fn, txt , ... )	{ .new_handler = fn, .summary = txt, ## __VA_ARGS__ }

/*!
 * Helper function to generate cli entries from a NULL-terminated array.
 * Returns the n-th matching entry from the array, or NULL if not found.
 * Can be used to implement generate() for static entries as below
 * (in this example we complete the word in position 2):
  \code
    char *my_generate(const char *line, const char *word, int pos, int n)
    {
        static char *choices = { "one", "two", "three", NULL };
	if (pos == 2)
        	return ast_cli_complete(word, choices, n);
	else
		return NULL;
    }
  \endcode
 */
char *ast_cli_complete(const char *word, char *const choices[], int pos);

/*! 
 * \brief Interprets a command
 * Interpret a command s, sending output to fd
 * \param fd pipe
 * \param s incoming string
 * \retval 0 on success
 * \retval -1 on failure
 */
int ast_cli_command(int fd, const char *s);

/*! 
 * \brief Executes multiple CLI commands
 * Interpret strings separated by NULL and execute each one, sending output to fd
 * \param fd pipe
 * \param size is the total size of the string
 * \param s incoming string
 * \retval number of commands executed
 */
int ast_cli_command_multiple(int fd, size_t size, const char *s);

/*! \brief Registers a command or an array of commands
 * \param e which cli entry to register.
 * Register your own command
 * \retval 0 on success
 * \retval -1 on failure
 */
int ast_cli_register(struct ast_cli_entry *e);

/*!
 * \brief Register multiple commands
 * \param e pointer to first cli entry to register
 * \param len number of entries to register
 */
int ast_cli_register_multiple(struct ast_cli_entry *e, int len);

/*! 
 * \brief Unregisters a command or an array of commands
 * \param e which cli entry to unregister
 * Unregister your own command.  You must pass a completed ast_cli_entry structure
 * \return 0
 */
int ast_cli_unregister(struct ast_cli_entry *e);

/*!
 * \brief Unregister multiple commands
 * \param e pointer to first cli entry to unregister
 * \param len number of entries to unregister
 */
int ast_cli_unregister_multiple(struct ast_cli_entry *e, int len);

/*! 
 * \brief Readline madness
 * Useful for readline, that's about it
 * \retval 0 on success
 * \retval -1 on failure
 */
char *ast_cli_generator(const char *, const char *, int);

int ast_cli_generatornummatches(const char *, const char *);

/*!
 * \brief Generates a NULL-terminated array of strings that
 * 1) begin with the string in the second parameter, and
 * 2) are valid in a command after the string in the first parameter.
 *
 * The first entry (offset 0) of the result is the longest common substring
 * in the results, useful to extend the string that has been completed.
 * Subsequent entries are all possible values, followed by a NULL.
 * All strings and the array itself are malloc'ed and must be freed
 * by the caller.
 */
char **ast_cli_completion_matches(const char *, const char *);

/*!
 * \brief Command completion for the list of active channels.
 *
 * This can be called from a CLI command completion function that wants to
 * complete from the list of active channels.  'rpos' is the required
 * position in the command.  This function will return NULL immediately if
 * 'rpos' is not the same as the current position, 'pos'.
 */
char *ast_complete_channels(const char *line, const char *word, int pos, int state, int rpos);

#if defined(__cplusplus) || defined(c_plusplus)
}
#endif

#endif /* _ASTERISK_CLI_H */
