/*
 * Asterisk -- A telephony toolkit for Linux.
 *
 * General Asterisk channel definitions.
 * 
 * Copyright (C) 1999, Mark Spencer
 *
 * Mark Spencer <markster@linux-support.net>
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License
 */

#ifndef _ASTERISK_LOCK_H
#define _ASTERISK_LOCK_H

#include <pthread.h>

#define AST_PTHREADT_NULL (pthread_t) -1
#define AST_PTHREADT_STOP (pthread_t) -2

/* From now on, Asterisk REQUIRES Recursive (not error checking) mutexes
   and will not run without them. */

#ifdef PTHREAD_MUTEX_RECURSIVE_NP
#define AST_MUTEX_INITIALIZER      PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP
#define AST_MUTEX_KIND             PTHREAD_MUTEX_RECURSIVE_NP
#else
#define AST_MUTEX_INITIALIZER      PTHREAD_MUTEX_INITIALIZER
#define AST_MUTEX_KIND             PTHREAD_MUTEX_RECURSIVE
#endif

#ifdef DEBUG_THREADS

#ifdef THREAD_CRASH
#define DO_THREAD_CRASH do { *((int *)(0)) = 1; } while(0)
#endif

#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>

struct ast_mutex_info {
	pthread_mutex_t mutex;
	char *file;
	int lineno;
	char *func;
	pthread_t thread;
};

typedef struct ast_mutex_info ast_mutex_t;

static inline int ast_mutex_init(ast_mutex_t *t) {
	static pthread_mutexattr_t  attr;
	static int  init = 1;
	int res;
	extern int  pthread_mutexattr_setkind_np(pthread_mutexattr_t *, int);

	if (init) {
		pthread_mutexattr_init(&attr);
		pthread_mutexattr_setkind_np(&attr, AST_MUTEX_KIND);
		init = 0;
	}
	res = pthread_mutex_init(&t->mutex, &attr);
	t->file = NULL;
	t->lineno = 0;
	t->func = 0;
	t->thread  = 0;
	return res;
}

static inline int ast_pthread_mutex_init(ast_mutex_t *t, pthread_mutexattr_t *attr) 
{
	int res;
	res = pthread_mutex_init(&t->mutex, attr);
	t->file = NULL;
	t->lineno = 0;
	t->func = 0;
	t->thread  = 0;
	return res;
}

static inline int __ast_pthread_mutex_lock(char *filename, int lineno, char *func, ast_mutex_t *t) {
	int res;
	res = pthread_mutex_lock(&t->mutex);
	if (!res) {
		t->file = filename;
		t->lineno = lineno;
		t->func = func;
		t->thread = pthread_self();
	} else {
		fprintf(stderr, "%s line %d (%s): Error obtaining mutex: %s\n",
			filename, lineno, func, strerror(errno));
#ifdef THREAD_CRASH
		DO_THREAD_CRASH;
#endif
	}
	return res;
}

#define ast_mutex_lock(a) __ast_pthread_mutex_lock(__FILE__, __LINE__, __PRETTY_FUNCTION__, a)

static inline int __ast_pthread_mutex_trylock(char *filename, int lineno, char *func, ast_mutex_t *t) {
	int res;
	res = pthread_mutex_trylock(&t->mutex);
	if (!res) {
		t->file = filename;
		t->lineno = lineno;
		t->func = func;
		t->thread = pthread_self();
	}
	return res;
}

#define ast_mutex_trylock(a) __ast_pthread_mutex_trylock(__FILE__, __LINE__, __PRETTY_FUNCTION__, a)

static inline int __ast_pthread_mutex_unlock(char *filename, int lineno, char *func, ast_mutex_t *t) {
	int res;
	/* Assumes lock is actually held */
	t->file = NULL;
	t->lineno = 0;
	t->func = NULL;
	t->thread = 0;
	res = pthread_mutex_unlock(&t->mutex);
	if (res) {
		fprintf(stderr, "%s line %d (%s): Error releasing mutex: %s\n", 
				filename, lineno, func, strerror(res));
#ifdef THREAD_CRASH
		DO_THREAD_CRASH;
#endif
	}
	return res;
}

#define ast_mutex_unlock(a) __ast_pthread_mutex_unlock(__FILE__, __LINE__, __PRETTY_FUNCTION__, a)

static inline int __ast_pthread_mutex_destroy(char *filename, int lineno, char *func, ast_mutex_t *t)
{
	int res;
	t->file = NULL;
	t->lineno = 0;
	t->func = NULL;
	t->thread = 0;
	res = pthread_mutex_destroy(&t->mutex);
	if (res) 
		fprintf(stderr, "%s line %d (%s): Error destroying mutex: %s\n",
				filename, lineno, func, strerror(res));
	return res;
}

#define ast_mutex_destroy(a) __ast_pthread_mutex_destroy(__FILE__, __LINE__, __PRETTY_FUNCTION__, a)

#define pthread_mutex_t use_ast_mutex_t_instead_of_pthread_mutex_t
#define pthread_mutex_lock use_ast_mutex_lock_instead_of_pthread_mutex_lock
#define pthread_mutex_unlock use_ast_mutex_unlock_instead_of_pthread_mutex_unlock
#define pthread_mutex_trylock use_ast_mutex_trylock_instead_of_pthread_mutex_trylock
#define pthread_mutex_init use_ast_pthread_mutex_init_instead_of_pthread_mutex_init
#define pthread_mutex_destroy use_ast_pthread_mutex_destroy_instead_of_pthread_mutex_destroy

#else /* DEBUG_THREADS */

typedef pthread_mutex_t ast_mutex_t;

#define ast_mutex_lock(t) pthread_mutex_lock(t)
#define ast_mutex_unlock(t) pthread_mutex_unlock(t)
#define ast_mutex_trylock(t) pthread_mutex_trylock(t)
static inline int ast_mutex_init(ast_mutex_t *t)
{
	pthread_mutexattr_t attr;
	pthread_mutexattr_init(&attr);
	pthread_mutexattr_settype(&attr, AST_MUTEX_KIND);
	return pthread_mutex_init(t, &attr);
}
#define ast_pthread_mutex_init(t,a) pthread_mutex_init(t,a)
#define ast_mutex_destroy(t) pthread_mutex_destroy(t)

#endif /* DEBUG_THREADS */


#endif
