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
 * \brief Scheduler Routines (derived from cheops)
 */

#ifndef _ASTERISK_SCHED_H
#define _ASTERISK_SCHED_H

#if defined(__cplusplus) || defined(c_plusplus)
extern "C" {
#endif

/*! \brief Max num of schedule structs
 * \note The max number of schedule structs to keep around
 * for use.  Undefine to disable schedule structure
 * caching. (Only disable this on very low memory
 * machines)
 */
#define SCHED_MAX_CACHE 128

struct sched_context;

/*! \brief New schedule context
 * \note Create a scheduling context
 * \return Returns a malloc'd sched_context structure, NULL on failure
 */
struct sched_context *sched_context_create(void);

/*! \brief destroys a schedule context
 * Destroys (free's) the given sched_context structure
 * \param c Context to free
 * \return Returns 0 on success, -1 on failure
 */
void sched_context_destroy(struct sched_context *c);

/*! \brief callback for a cheops scheduler
 * A cheops scheduler callback takes a pointer with callback data and
 * \return returns a 0 if it should not be run again, or non-zero if it should be
 * rescheduled to run again
 */
typedef int (*ast_sched_cb)(void *data);
#define AST_SCHED_CB(a) ((ast_sched_cb)(a))

/*! \brief Adds a scheduled event
 * Schedule an event to take place at some point in the future.  callback
 * will be called with data as the argument, when milliseconds into the
 * future (approximately)
 * If callback returns 0, no further events will be re-scheduled
 * \param con Scheduler context to add
 * \param when how many milliseconds to wait for event to occur
 * \param callback function to call when the amount of time expires
 * \param data data to pass to the callback
 * \return Returns a schedule item ID on success, -1 on failure
 */
int ast_sched_add(struct sched_context *con, int when, ast_sched_cb callback, void *data);

/*!
 * \brief replace a scheduler entry
 *
 * This deletes the scheduler entry for old_id if it exists, and then
 * calls ast_sched_add to create a new entry.  A negative old_id will
 * be ignored.
 *
 * \retval -1 failure
 * \retval otherwise, returns scheduled item ID
 */
int ast_sched_replace(int old_id, struct sched_context *con, int when, ast_sched_cb callback, void *data);

/*!Adds a scheduled event with rescheduling support
 * \param con Scheduler context to add
 * \param when how many milliseconds to wait for event to occur
 * \param callback function to call when the amount of time expires
 * \param data data to pass to the callback
 * \param variable If true, the result value of callback function will be
 *       used for rescheduling
 * Schedule an event to take place at some point in the future.  Callback
 * will be called with data as the argument, when milliseconds into the
 * future (approximately)
 * If callback returns 0, no further events will be re-scheduled
 * \return Returns a schedule item ID on success, -1 on failure
 */
int ast_sched_add_variable(struct sched_context *con, int when, ast_sched_cb callback, void *data, int variable);

/*!
 * \brief replace a scheduler entry
 *
 * This deletes the scheduler entry for old_id if it exists, and then
 * calls ast_sched_add to create a new entry.  A negative old_id will
 * be ignored.
 *
 * \retval -1 failure
 * \retval otherwise, returns scheduled item ID
 */
int ast_sched_replace_variable(int old_id, struct sched_context *con, int when, ast_sched_cb callback, void *data, int variable);

/*! \brief Deletes a scheduled event
 * Remove this event from being run.  A procedure should not remove its
 * own event, but return 0 instead.
 * \param con scheduling context to delete item from
 * \param id ID of the scheduled item to delete
 * \return Returns 0 on success, -1 on failure
 */
int ast_sched_del(struct sched_context *con, int id);

/*! \brief Determines number of seconds until the next outstanding event to take place
 * Determine the number of seconds until the next outstanding event
 * should take place, and return the number of milliseconds until
 * it needs to be run.  This value is perfect for passing to the poll
 * call.
 * \param con context to act upon
 * \return Returns "-1" if there is nothing there are no scheduled events
 * (and thus the poll should not timeout)
 */
int ast_sched_wait(struct sched_context *con);

/*! \brief Runs the queue
 * \param con Scheduling context to run
 * Run the queue, executing all callbacks which need to be performed
 * at this time.
 * \param con context to act upon
 * \return Returns the number of events processed.
 */
int ast_sched_runq(struct sched_context *con);

/*! \brief Dumps the scheduler contents
 * Debugging: Dump the contents of the scheduler to stderr
 * \param con Context to dump
 */
void ast_sched_dump(const struct sched_context *con);

/*! \brief Returns the number of seconds before an event takes place
 * \param con Context to use
 * \param id Id to dump
 */
long ast_sched_when(struct sched_context *con,int id);

/*!
 * \brief Convenience macro for objects and reference (add)
 *
 */
#define ast_sched_add_object(obj,con,when,callback) ast_sched_add((con),(when),(callback), ASTOBJ_REF((obj)))

/*!
 * \brief Convenience macro for objects and reference (del)
 *
 */
#define ast_sched_del_object(obj,destructor,con,id) do { \
	if ((id) > -1) { \
		ast_sched_del((con),(id)); \
		(id) = -1; \
		ASTOBJ_UNREF((obj),(destructor)); \
	} \
} while(0)

#if defined(__cplusplus) || defined(c_plusplus)
}
#endif

#endif /* _ASTERISK_SCHED_H */
