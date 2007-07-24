/*
 * Asterisk -- An open source telephony toolkit.
 *
 * Copyright (C) 1999 - 2006, Digium, Inc.
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
 * \brief General Asterisk channel locking definitions.
 *
 * - See \ref LockDef
 */

/*! \page LockDef Asterisk thread locking models
 *
 * This file provides different implementation of the functions,
 * depending on the platform, the use of DEBUG_THREADS, and the way
 * module-level mutexes are initialized.
 *
 *  - \b static: the mutex is assigned the value AST_MUTEX_INIT_VALUE
 *        this is done at compile time, and is the way used on Linux.
 *        This method is not applicable to all platforms e.g. when the
 *        initialization needs that some code is run.
 *
 *  - \b through constructors: for each mutex, a constructor function is
 *        defined, which then runs when the program (or the module)
 *        starts. The problem with this approach is that there is a
 *        lot of code duplication (a new block of code is created for
 *        each mutex). Also, it does not prevent a user from declaring
 *        a global mutex without going through the wrapper macros,
 *        so sane programming practices are still required.
 */

#ifndef _ASTERISK_LOCK_H
#define _ASTERISK_LOCK_H

#include <pthread.h>
#include <netdb.h>
#include <time.h>
#include <sys/param.h>

#include "asterisk/logger.h"

/* internal macro to profile mutexes. Only computes the delay on
 * non-blocking calls.
 */
#ifndef	HAVE_MTX_PROFILE
#define	__MTX_PROF(a)	return pthread_mutex_lock((a))
#else
#define	__MTX_PROF(a)	do {			\
	int i;					\
	/* profile only non-blocking events */	\
	ast_mark(mtx_prof, 1);			\
	i = pthread_mutex_trylock((a));		\
	ast_mark(mtx_prof, 0);			\
	if (!i)					\
		return i;			\
	else					\
		return pthread_mutex_lock((a)); \
	} while (0)
#endif	/* HAVE_MTX_PROFILE */

#define AST_PTHREADT_NULL (pthread_t) -1
#define AST_PTHREADT_STOP (pthread_t) -2

#if defined(SOLARIS) || defined(BSD)
#define AST_MUTEX_INIT_W_CONSTRUCTORS
#endif /* SOLARIS || BSD */

/* Asterisk REQUIRES recursive (not error checking) mutexes
   and will not run without them. */
#if defined(PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP)
#define PTHREAD_MUTEX_INIT_VALUE	PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP
#define AST_MUTEX_KIND			PTHREAD_MUTEX_RECURSIVE_NP
#else
#define PTHREAD_MUTEX_INIT_VALUE	PTHREAD_MUTEX_INITIALIZER
#define AST_MUTEX_KIND			PTHREAD_MUTEX_RECURSIVE
#endif /* PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP */

#ifdef DEBUG_THREADS

#define __ast_mutex_logger(...)  do { if (canlog) ast_log(LOG_ERROR, __VA_ARGS__); else fprintf(stderr, __VA_ARGS__); } while (0)

#ifdef THREAD_CRASH
#define DO_THREAD_CRASH do { *((int *)(0)) = 1; } while(0)
#else
#define DO_THREAD_CRASH do { } while (0)
#endif

#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>

#define AST_MUTEX_INIT_VALUE { PTHREAD_MUTEX_INIT_VALUE, { NULL }, { 0 }, 0, { NULL }, { 0 } }

#define AST_MAX_REENTRANCY 10

struct ast_mutex_info {
	pthread_mutex_t mutex;
	const char *file[AST_MAX_REENTRANCY];
	int lineno[AST_MAX_REENTRANCY];
	int reentrancy;
	const char *func[AST_MAX_REENTRANCY];
	pthread_t thread[AST_MAX_REENTRANCY];
};

typedef struct ast_mutex_info ast_mutex_t;

typedef pthread_cond_t ast_cond_t;

static pthread_mutex_t empty_mutex;

static void __attribute__((constructor)) init_empty_mutex(void)
{
	memset(&empty_mutex, 0, sizeof(empty_mutex));
}

static inline int __ast_pthread_mutex_init_attr(const char *filename, int lineno, const char *func,
						const char *mutex_name, ast_mutex_t *t,
						pthread_mutexattr_t *attr) 
{
#ifdef AST_MUTEX_INIT_W_CONSTRUCTORS
	int canlog = strcmp(filename, "logger.c");

	if ((t->mutex) != ((pthread_mutex_t) PTHREAD_MUTEX_INITIALIZER)) {
		if ((t->mutex) != (empty_mutex)) {
			__ast_mutex_logger("%s line %d (%s): Error: mutex '%s' is already initialized.\n",
					   filename, lineno, func, mutex_name);
			__ast_mutex_logger("%s line %d (%s): Error: previously initialization of mutex '%s'.\n",
					   t->file[0], t->lineno[0], t->func[0], mutex_name);
			DO_THREAD_CRASH;
			return 0;
		}
	}
#endif

	t->file[0] = filename;
	t->lineno[0] = lineno;
	t->func[0] = func;
	t->thread[0]  = 0;
	t->reentrancy = 0;

	return pthread_mutex_init(&t->mutex, attr);
}

static inline int __ast_pthread_mutex_init(const char *filename, int lineno, const char *func,
					   const char *mutex_name, ast_mutex_t *t)
{
	static pthread_mutexattr_t  attr;

	pthread_mutexattr_init(&attr);
	pthread_mutexattr_settype(&attr, AST_MUTEX_KIND);

	return __ast_pthread_mutex_init_attr(filename, lineno, func, mutex_name, t, &attr);
}
#define ast_mutex_init(pmutex) __ast_pthread_mutex_init(__FILE__, __LINE__, __PRETTY_FUNCTION__, #pmutex, pmutex)

static inline int __ast_pthread_mutex_destroy(const char *filename, int lineno, const char *func,
						const char *mutex_name, ast_mutex_t *t)
{
	int res;
	int canlog = strcmp(filename, "logger.c");

#ifdef AST_MUTEX_INIT_W_CONSTRUCTORS
	if ((t->mutex) == ((pthread_mutex_t) PTHREAD_MUTEX_INITIALIZER)) {
		__ast_mutex_logger("%s line %d (%s): Error: mutex '%s' is uninitialized.\n",
				   filename, lineno, func, mutex_name);
	}
#endif

	res = pthread_mutex_trylock(&t->mutex);
	switch (res) {
	case 0:
		pthread_mutex_unlock(&t->mutex);
		break;
	case EINVAL:
		__ast_mutex_logger("%s line %d (%s): Error: attempt to destroy invalid mutex '%s'.\n",
				  filename, lineno, func, mutex_name);
		break;
	case EBUSY:
		__ast_mutex_logger("%s line %d (%s): Error: attempt to destroy locked mutex '%s'.\n",
				   filename, lineno, func, mutex_name);
		__ast_mutex_logger("%s line %d (%s): Error: '%s' was locked here.\n",
				   t->file[t->reentrancy-1], t->lineno[t->reentrancy-1], t->func[t->reentrancy-1], mutex_name);
		break;
	}

	if ((res = pthread_mutex_destroy(&t->mutex)))
		__ast_mutex_logger("%s line %d (%s): Error destroying mutex: %s\n",
				   filename, lineno, func, strerror(res));
#ifndef PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP
	else
		t->mutex = PTHREAD_MUTEX_INIT_VALUE;
#endif
	t->file[0] = filename;
	t->lineno[0] = lineno;
	t->func[0] = func;

	return res;
}

static inline int __ast_pthread_mutex_lock(const char *filename, int lineno, const char *func,
                                           const char* mutex_name, ast_mutex_t *t)
{
	int res;
	int canlog = strcmp(filename, "logger.c");

#if defined(AST_MUTEX_INIT_W_CONSTRUCTORS)
	if ((t->mutex) == ((pthread_mutex_t) PTHREAD_MUTEX_INITIALIZER)) {
		__ast_mutex_logger("%s line %d (%s): Error: mutex '%s' is uninitialized.\n",
				 filename, lineno, func, mutex_name);
		ast_mutex_init(t);
	}
#endif /* AST_MUTEX_INIT_W_CONSTRUCTORS */

#ifdef DETECT_DEADLOCKS
	{
		time_t seconds = time(NULL);
		time_t current;
		do {
#ifdef	HAVE_MTX_PROFILE
			ast_mark(mtx_prof, 1);
#endif
			res = pthread_mutex_trylock(&t->mutex);
#ifdef	HAVE_MTX_PROFILE
			ast_mark(mtx_prof, 0);
#endif
			if (res == EBUSY) {
				current = time(NULL);
				if ((current - seconds) && (!((current - seconds) % 5))) {
					__ast_mutex_logger("%s line %d (%s): Deadlock? waited %d sec for mutex '%s'?\n",
							   filename, lineno, func, (int)(current - seconds), mutex_name);
					__ast_mutex_logger("%s line %d (%s): '%s' was locked here.\n",
							   t->file[t->reentrancy-1], t->lineno[t->reentrancy-1],
							   t->func[t->reentrancy-1], mutex_name);
				}
				usleep(200);
			}
		} while (res == EBUSY);
	}
#else
#ifdef	HAVE_MTX_PROFILE
	ast_mark(mtx_prof, 1);
	res = pthread_mutex_trylock(&t->mutex);
	ast_mark(mtx_prof, 0);
	if (res)
#endif
	res = pthread_mutex_lock(&t->mutex);
#endif /* DETECT_DEADLOCKS */

	if (!res) {
		if (t->reentrancy < AST_MAX_REENTRANCY) {
			t->file[t->reentrancy] = filename;
			t->lineno[t->reentrancy] = lineno;
			t->func[t->reentrancy] = func;
			t->thread[t->reentrancy] = pthread_self();
			t->reentrancy++;
		} else {
			__ast_mutex_logger("%s line %d (%s): '%s' really deep reentrancy!\n",
							   filename, lineno, func, mutex_name);
		}
	} else {
		__ast_mutex_logger("%s line %d (%s): Error obtaining mutex: %s\n",
				   filename, lineno, func, strerror(res));
		DO_THREAD_CRASH;
	}

	return res;
}

static inline int __ast_pthread_mutex_trylock(const char *filename, int lineno, const char *func,
                                              const char* mutex_name, ast_mutex_t *t)
{
	int res;
	int canlog = strcmp(filename, "logger.c");

#if defined(AST_MUTEX_INIT_W_CONSTRUCTORS)
	if ((t->mutex) == ((pthread_mutex_t) PTHREAD_MUTEX_INITIALIZER)) {
		__ast_mutex_logger("%s line %d (%s): Error: mutex '%s' is uninitialized.\n",
				   filename, lineno, func, mutex_name);
		ast_mutex_init(t);
	}
#endif /* AST_MUTEX_INIT_W_CONSTRUCTORS */

	if (!(res = pthread_mutex_trylock(&t->mutex))) {
		if (t->reentrancy < AST_MAX_REENTRANCY) {
			t->file[t->reentrancy] = filename;
			t->lineno[t->reentrancy] = lineno;
			t->func[t->reentrancy] = func;
			t->thread[t->reentrancy] = pthread_self();
			t->reentrancy++;
		} else {
			__ast_mutex_logger("%s line %d (%s): '%s' really deep reentrancy!\n",
							   filename, lineno, func, mutex_name);
		}
	}

	return res;
}

static inline int __ast_pthread_mutex_unlock(const char *filename, int lineno, const char *func,
					     const char *mutex_name, ast_mutex_t *t)
{
	int res;
	int canlog = strcmp(filename, "logger.c");

#ifdef AST_MUTEX_INIT_W_CONSTRUCTORS
	if ((t->mutex) == ((pthread_mutex_t) PTHREAD_MUTEX_INITIALIZER)) {
		__ast_mutex_logger("%s line %d (%s): Error: mutex '%s' is uninitialized.\n",
				   filename, lineno, func, mutex_name);
	}
#endif

	if (t->reentrancy && (t->thread[t->reentrancy-1] != pthread_self())) {
		__ast_mutex_logger("%s line %d (%s): attempted unlock mutex '%s' without owning it!\n",
				   filename, lineno, func, mutex_name);
		__ast_mutex_logger("%s line %d (%s): '%s' was locked here.\n",
				   t->file[t->reentrancy-1], t->lineno[t->reentrancy-1], t->func[t->reentrancy-1], mutex_name);
		DO_THREAD_CRASH;
	}

	if (--t->reentrancy < 0) {
		__ast_mutex_logger("%s line %d (%s): mutex '%s' freed more times than we've locked!\n",
				   filename, lineno, func, mutex_name);
		t->reentrancy = 0;
	}

	if (t->reentrancy < AST_MAX_REENTRANCY) {
		t->file[t->reentrancy] = NULL;
		t->lineno[t->reentrancy] = 0;
		t->func[t->reentrancy] = NULL;
		t->thread[t->reentrancy] = 0;
	}

	if ((res = pthread_mutex_unlock(&t->mutex))) {
		__ast_mutex_logger("%s line %d (%s): Error releasing mutex: %s\n", 
				   filename, lineno, func, strerror(res));
		DO_THREAD_CRASH;
	}

	return res;
}

static inline int __ast_cond_init(const char *filename, int lineno, const char *func,
				  const char *cond_name, ast_cond_t *cond, pthread_condattr_t *cond_attr)
{
	return pthread_cond_init(cond, cond_attr);
}

static inline int __ast_cond_signal(const char *filename, int lineno, const char *func,
				    const char *cond_name, ast_cond_t *cond)
{
	return pthread_cond_signal(cond);
}

static inline int __ast_cond_broadcast(const char *filename, int lineno, const char *func,
				       const char *cond_name, ast_cond_t *cond)
{
	return pthread_cond_broadcast(cond);
}

static inline int __ast_cond_destroy(const char *filename, int lineno, const char *func,
				     const char *cond_name, ast_cond_t *cond)
{
	return pthread_cond_destroy(cond);
}

static inline int __ast_cond_wait(const char *filename, int lineno, const char *func,
				  const char *cond_name, const char *mutex_name,
				  ast_cond_t *cond, ast_mutex_t *t)
{
	int res;
	int canlog = strcmp(filename, "logger.c");

#ifdef AST_MUTEX_INIT_W_CONSTRUCTORS
	if ((t->mutex) == ((pthread_mutex_t) PTHREAD_MUTEX_INITIALIZER)) {
		__ast_mutex_logger("%s line %d (%s): Error: mutex '%s' is uninitialized.\n",
				   filename, lineno, func, mutex_name);
	}
#endif

	if (t->reentrancy && (t->thread[t->reentrancy-1] != pthread_self())) {
		__ast_mutex_logger("%s line %d (%s): attempted unlock mutex '%s' without owning it!\n",
				   filename, lineno, func, mutex_name);
		__ast_mutex_logger("%s line %d (%s): '%s' was locked here.\n",
				   t->file[t->reentrancy-1], t->lineno[t->reentrancy-1], t->func[t->reentrancy-1], mutex_name);
		DO_THREAD_CRASH;
	}

	if (--t->reentrancy < 0) {
		__ast_mutex_logger("%s line %d (%s): mutex '%s' freed more times than we've locked!\n",
				   filename, lineno, func, mutex_name);
		t->reentrancy = 0;
	}

	if (t->reentrancy < AST_MAX_REENTRANCY) {
		t->file[t->reentrancy] = NULL;
		t->lineno[t->reentrancy] = 0;
		t->func[t->reentrancy] = NULL;
		t->thread[t->reentrancy] = 0;
	}

	if ((res = pthread_cond_wait(cond, &t->mutex))) {
		__ast_mutex_logger("%s line %d (%s): Error waiting on condition mutex '%s'\n", 
				   filename, lineno, func, strerror(res));
		DO_THREAD_CRASH;
	} else {
		if (t->reentrancy < AST_MAX_REENTRANCY) {
			t->file[t->reentrancy] = filename;
			t->lineno[t->reentrancy] = lineno;
			t->func[t->reentrancy] = func;
			t->thread[t->reentrancy] = pthread_self();
			t->reentrancy++;
		} else {
			__ast_mutex_logger("%s line %d (%s): '%s' really deep reentrancy!\n",
							   filename, lineno, func, mutex_name);
		}
	}

	return res;
}

static inline int __ast_cond_timedwait(const char *filename, int lineno, const char *func,
				       const char *cond_name, const char *mutex_name, ast_cond_t *cond,
				       ast_mutex_t *t, const struct timespec *abstime)
{
	int res;
	int canlog = strcmp(filename, "logger.c");

#ifdef AST_MUTEX_INIT_W_CONSTRUCTORS
	if ((t->mutex) == ((pthread_mutex_t) PTHREAD_MUTEX_INITIALIZER)) {
		__ast_mutex_logger("%s line %d (%s): Error: mutex '%s' is uninitialized.\n",
				   filename, lineno, func, mutex_name);
	}
#endif

	if (t->reentrancy && (t->thread[t->reentrancy-1] != pthread_self())) {
		__ast_mutex_logger("%s line %d (%s): attempted unlock mutex '%s' without owning it!\n",
				   filename, lineno, func, mutex_name);
		__ast_mutex_logger("%s line %d (%s): '%s' was locked here.\n",
				   t->file[t->reentrancy-1], t->lineno[t->reentrancy-1], t->func[t->reentrancy-1], mutex_name);
		DO_THREAD_CRASH;
	}

	if (--t->reentrancy < 0) {
		__ast_mutex_logger("%s line %d (%s): mutex '%s' freed more times than we've locked!\n",
				   filename, lineno, func, mutex_name);
		t->reentrancy = 0;
	}

	if (t->reentrancy < AST_MAX_REENTRANCY) {
		t->file[t->reentrancy] = NULL;
		t->lineno[t->reentrancy] = 0;
		t->func[t->reentrancy] = NULL;
		t->thread[t->reentrancy] = 0;
	}

	if ((res = pthread_cond_timedwait(cond, &t->mutex, abstime)) && (res != ETIMEDOUT)) {
		__ast_mutex_logger("%s line %d (%s): Error waiting on condition mutex '%s'\n", 
				   filename, lineno, func, strerror(res));
		DO_THREAD_CRASH;
	} else {
		if (t->reentrancy < AST_MAX_REENTRANCY) {
			t->file[t->reentrancy] = filename;
			t->lineno[t->reentrancy] = lineno;
			t->func[t->reentrancy] = func;
			t->thread[t->reentrancy] = pthread_self();
			t->reentrancy++;
		} else {
			__ast_mutex_logger("%s line %d (%s): '%s' really deep reentrancy!\n",
							   filename, lineno, func, mutex_name);
		}
	}

	return res;
}

#define ast_mutex_destroy(a) __ast_pthread_mutex_destroy(__FILE__, __LINE__, __PRETTY_FUNCTION__, #a, a)
#define ast_mutex_lock(a) __ast_pthread_mutex_lock(__FILE__, __LINE__, __PRETTY_FUNCTION__, #a, a)
#define ast_mutex_unlock(a) __ast_pthread_mutex_unlock(__FILE__, __LINE__, __PRETTY_FUNCTION__, #a, a)
#define ast_mutex_trylock(a) __ast_pthread_mutex_trylock(__FILE__, __LINE__, __PRETTY_FUNCTION__, #a, a)
#define ast_cond_init(cond, attr) __ast_cond_init(__FILE__, __LINE__, __PRETTY_FUNCTION__, #cond, cond, attr)
#define ast_cond_destroy(cond) __ast_cond_destroy(__FILE__, __LINE__, __PRETTY_FUNCTION__, #cond, cond)
#define ast_cond_signal(cond) __ast_cond_signal(__FILE__, __LINE__, __PRETTY_FUNCTION__, #cond, cond)
#define ast_cond_broadcast(cond) __ast_cond_broadcast(__FILE__, __LINE__, __PRETTY_FUNCTION__, #cond, cond)
#define ast_cond_wait(cond, mutex) __ast_cond_wait(__FILE__, __LINE__, __PRETTY_FUNCTION__, #cond, #mutex, cond, mutex)
#define ast_cond_timedwait(cond, mutex, time) __ast_cond_timedwait(__FILE__, __LINE__, __PRETTY_FUNCTION__, #cond, #mutex, cond, mutex, time)

#else /* !DEBUG_THREADS */


typedef pthread_mutex_t ast_mutex_t;

#define AST_MUTEX_INIT_VALUE	((ast_mutex_t) PTHREAD_MUTEX_INIT_VALUE)

static inline int ast_mutex_init(ast_mutex_t *pmutex)
{
	pthread_mutexattr_t attr;

	pthread_mutexattr_init(&attr);
	pthread_mutexattr_settype(&attr, AST_MUTEX_KIND);

	return pthread_mutex_init(pmutex, &attr);
}

#define ast_pthread_mutex_init(pmutex,a) pthread_mutex_init(pmutex,a)

static inline int ast_mutex_unlock(ast_mutex_t *pmutex)
{
	return pthread_mutex_unlock(pmutex);
}

static inline int ast_mutex_destroy(ast_mutex_t *pmutex)
{
	return pthread_mutex_destroy(pmutex);
}

static inline int ast_mutex_lock(ast_mutex_t *pmutex)
{
	__MTX_PROF(pmutex);
}

static inline int ast_mutex_trylock(ast_mutex_t *pmutex)
{
	return pthread_mutex_trylock(pmutex);
}

typedef pthread_cond_t ast_cond_t;

static inline int ast_cond_init(ast_cond_t *cond, pthread_condattr_t *cond_attr)
{
	return pthread_cond_init(cond, cond_attr);
}

static inline int ast_cond_signal(ast_cond_t *cond)
{
	return pthread_cond_signal(cond);
}

static inline int ast_cond_broadcast(ast_cond_t *cond)
{
	return pthread_cond_broadcast(cond);
}

static inline int ast_cond_destroy(ast_cond_t *cond)
{
	return pthread_cond_destroy(cond);
}

static inline int ast_cond_wait(ast_cond_t *cond, ast_mutex_t *t)
{
	return pthread_cond_wait(cond, t);
}

static inline int ast_cond_timedwait(ast_cond_t *cond, ast_mutex_t *t, const struct timespec *abstime)
{
	return pthread_cond_timedwait(cond, t, abstime);
}

#endif /* !DEBUG_THREADS */

#if defined(AST_MUTEX_INIT_W_CONSTRUCTORS)
/* If AST_MUTEX_INIT_W_CONSTRUCTORS is defined, use file scope
 constructors/destructors to create/destroy mutexes.  */
#define __AST_MUTEX_DEFINE(scope, mutex) \
	scope ast_mutex_t mutex = AST_MUTEX_INIT_VALUE; \
static void  __attribute__ ((constructor)) init_##mutex(void) \
{ \
	ast_mutex_init(&mutex); \
} \
static void  __attribute__ ((destructor)) fini_##mutex(void) \
{ \
	ast_mutex_destroy(&mutex); \
}
#else /* !AST_MUTEX_INIT_W_CONSTRUCTORS */
/* By default, use static initialization of mutexes. */ 
#define __AST_MUTEX_DEFINE(scope, mutex) \
	scope ast_mutex_t mutex = AST_MUTEX_INIT_VALUE
#endif /* AST_MUTEX_INIT_W_CONSTRUCTORS */

#define pthread_mutex_t use_ast_mutex_t_instead_of_pthread_mutex_t
#define pthread_mutex_lock use_ast_mutex_lock_instead_of_pthread_mutex_lock
#define pthread_mutex_unlock use_ast_mutex_unlock_instead_of_pthread_mutex_unlock
#define pthread_mutex_trylock use_ast_mutex_trylock_instead_of_pthread_mutex_trylock
#define pthread_mutex_init use_ast_mutex_init_instead_of_pthread_mutex_init
#define pthread_mutex_destroy use_ast_mutex_destroy_instead_of_pthread_mutex_destroy
#define pthread_cond_t use_ast_cond_t_instead_of_pthread_cond_t
#define pthread_cond_init use_ast_cond_init_instead_of_pthread_cond_init
#define pthread_cond_destroy use_ast_cond_destroy_instead_of_pthread_cond_destroy
#define pthread_cond_signal use_ast_cond_signal_instead_of_pthread_cond_signal
#define pthread_cond_broadcast use_ast_cond_broadcast_instead_of_pthread_cond_broadcast
#define pthread_cond_wait use_ast_cond_wait_instead_of_pthread_cond_wait
#define pthread_cond_timedwait use_ast_cond_timedwait_instead_of_pthread_cond_timedwait

#define AST_MUTEX_DEFINE_STATIC(mutex) __AST_MUTEX_DEFINE(static, mutex)

#define AST_MUTEX_INITIALIZER __use_AST_MUTEX_DEFINE_STATIC_rather_than_AST_MUTEX_INITIALIZER__

#define gethostbyname __gethostbyname__is__not__reentrant__use__ast_gethostbyname__instead__

#ifndef __linux__
#define pthread_create __use_ast_pthread_create_instead__
#endif

typedef pthread_rwlock_t ast_rwlock_t;

static inline int ast_rwlock_init(ast_rwlock_t *prwlock)
{
	pthread_rwlockattr_t attr;

	pthread_rwlockattr_init(&attr);
	
#ifdef HAVE_PTHREAD_RWLOCK_PREFER_WRITER_NP
	pthread_rwlockattr_setkind_np(&attr, PTHREAD_RWLOCK_PREFER_WRITER_NP);
#endif
	
	return pthread_rwlock_init(prwlock, &attr);
}

static inline int ast_rwlock_destroy(ast_rwlock_t *prwlock)
{
	return pthread_rwlock_destroy(prwlock);
}

static inline int ast_rwlock_unlock(ast_rwlock_t *prwlock)
{
	return pthread_rwlock_unlock(prwlock);
}

static inline int ast_rwlock_rdlock(ast_rwlock_t *prwlock)
{
	return pthread_rwlock_rdlock(prwlock);
}

static inline int ast_rwlock_tryrdlock(ast_rwlock_t *prwlock)
{
	return pthread_rwlock_tryrdlock(prwlock);
}

static inline int ast_rwlock_wrlock(ast_rwlock_t *prwlock)
{
	return pthread_rwlock_wrlock(prwlock);
}

static inline int ast_rwlock_trywrlock(ast_rwlock_t *prwlock)
{
	return pthread_rwlock_trywrlock(prwlock);
}

/* Statically declared read/write locks */

#ifndef HAVE_PTHREAD_RWLOCK_INITIALIZER
#define __AST_RWLOCK_DEFINE(scope, rwlock) \
        scope ast_rwlock_t rwlock; \
static void  __attribute__ ((constructor)) init_##rwlock(void) \
{ \
        ast_rwlock_init(&rwlock); \
} \
static void  __attribute__ ((destructor)) fini_##rwlock(void) \
{ \
        ast_rwlock_destroy(&rwlock); \
}
#else
#define AST_RWLOCK_INIT_VALUE PTHREAD_RWLOCK_INITIALIZER
#define __AST_RWLOCK_DEFINE(scope, rwlock) \
        scope ast_rwlock_t rwlock = AST_RWLOCK_INIT_VALUE
#endif

#define AST_RWLOCK_DEFINE_STATIC(rwlock) __AST_RWLOCK_DEFINE(static, rwlock)

/*
 * Initial support for atomic instructions.
 * For platforms that have it, use the native cpu instruction to
 * implement them. For other platforms, resort to a 'slow' version
 * (defined in utils.c) that protects the atomic instruction with
 * a single lock.
 * The slow versions is always available, for testing purposes,
 * as ast_atomic_fetchadd_int_slow()
 */

int ast_atomic_fetchadd_int_slow(volatile int *p, int v);

#include "asterisk/inline_api.h"

#if defined(HAVE_OSX_ATOMICS)
#include "libkern/OSAtomic.h"
#endif

/*! \brief Atomically add v to *p and return * the previous value of *p.
 * This can be used to handle reference counts, and the return value
 * can be used to generate unique identifiers.
 */

#if defined(HAVE_GCC_ATOMICS)
AST_INLINE_API(int ast_atomic_fetchadd_int(volatile int *p, int v),
{
	return __sync_fetch_and_add(p, v);
})
#elif defined(HAVE_OSX_ATOMICS) && (SIZEOF_INT == 4)
AST_INLINE_API(int ast_atomic_fetchadd_int(volatile int *p, int v),
{
	return OSAtomicAdd32(v, (int32_t *) p);
})
#elif defined(HAVE_OSX_ATOMICS) && (SIZEOF_INT == 8)
AST_INLINE_API(int ast_atomic_fetchadd_int(volatile int *p, int v),
{
	return OSAtomicAdd64(v, (int64_t *) p);
#elif defined (__i386__)
AST_INLINE_API(int ast_atomic_fetchadd_int(volatile int *p, int v),
{
	__asm __volatile (
	"       lock   xaddl   %0, %1 ;        "
	: "+r" (v),                     /* 0 (result) */   
	  "=m" (*p)                     /* 1 */
	: "m" (*p));                    /* 2 */
	return (v);
})
#else   /* low performance version in utils.c */
AST_INLINE_API(int ast_atomic_fetchadd_int(volatile int *p, int v),
{
	return ast_atomic_fetchadd_int_slow(p, v);
})
#endif

/*! \brief decrement *p by 1 and return true if the variable has reached 0.
 * Useful e.g. to check if a refcount has reached 0.
 */
#if defined(HAVE_GCC_ATOMICS)
AST_INLINE_API(int ast_atomic_dec_and_test(volatile int *p),
{
	return __sync_sub_and_fetch(p, 1) == 0;
})
#elif defined(HAVE_OSX_ATOMICS) && (SIZEOF_INT == 4)
AST_INLINE_API(int ast_atomic_dec_and_test(volatile int *p),
{
	return OSAtomicAdd32( -1, (int32_t *) p) == 0;
})
#elif defined(HAVE_OSX_ATOMICS) && (SIZEOF_INT == 8)
AST_INLINE_API(int ast_atomic_dec_and_test(volatile int *p),
{
	return OSAtomicAdd64( -1, (int64_t *) p) == 0;
#else
AST_INLINE_API(int ast_atomic_dec_and_test(volatile int *p),
{
	int a = ast_atomic_fetchadd_int(p, -1);
	return a == 1; /* true if the value is 0 now (so it was 1 previously) */
})
#endif

#ifndef DEBUG_CHANNEL_LOCKS
/*! \brief Lock a channel. If DEBUG_CHANNEL_LOCKS is defined 
	in the Makefile, print relevant output for debugging */
#define ast_channel_lock(x)		ast_mutex_lock(&x->lock)
/*! \brief Unlock a channel. If DEBUG_CHANNEL_LOCKS is defined 
	in the Makefile, print relevant output for debugging */
#define ast_channel_unlock(x)		ast_mutex_unlock(&x->lock)
/*! \brief Try locking a channel. If DEBUG_CHANNEL_LOCKS is defined 
	in the Makefile, print relevant output for debugging */
#define ast_channel_trylock(x)		ast_mutex_trylock(&x->lock)
#else

struct ast_channel;

/*! \brief Lock AST channel (and print debugging output)
\note You need to enable DEBUG_CHANNEL_LOCKS for this function */
int ast_channel_lock(struct ast_channel *chan);

/*! \brief Unlock AST channel (and print debugging output)
\note You need to enable DEBUG_CHANNEL_LOCKS for this function
*/
int ast_channel_unlock(struct ast_channel *chan);

/*! \brief Lock AST channel (and print debugging output)
\note   You need to enable DEBUG_CHANNEL_LOCKS for this function */
int ast_channel_trylock(struct ast_channel *chan);
#endif

#endif /* _ASTERISK_LOCK_H */
