/*
 * Asterisk -- An open source telephony toolkit.
 *
 * Copyright (C) 2010, Digium, Inc.
 *
 * Tilghman Lesher <tlesher AT digium DOT com>
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

/*!\file
 * \brief Bitfield expansions for ast_select
 */

#ifndef __AST_SELECT_H
#define __AST_SELECT_H

#include <sys/select.h>
#include <errno.h>
#include "asterisk/utils.h"

#ifdef __cplusplus
extern "C" {
#endif

extern unsigned int ast_FD_SETSIZE;

#if !defined(HAVE_VARIABLE_FDSET) && defined(CONFIGURE_RAN_AS_ROOT)
#define ast_fdset fd_set
#else
typedef struct {
	long fds_bits[4096 / sizeof(long)]; /* 32768 bits */
} ast_fdset;

#undef FD_ZERO
#define FD_ZERO(a) \
	do { \
		long *bytes = (long *) a; \
		int i; \
		for (i = 0; i < sizeof(*(a)) / sizeof(long); i++) { \
			bytes[i] = 0; \
		} \
	} while (0)
#undef FD_SET
#define FD_SET(fd, fds) \
	do { \
		long *bytes = (long *) fds; \
		if (fd / sizeof(*bytes) + ((fd + 1) % sizeof(*bytes) ? 1 : 0) < sizeof(*(fds))) { \
			bytes[fd / (sizeof(*bytes))] |= 1L << (fd % sizeof(*bytes)); \
		} else { \
			ast_log(LOG_ERROR, "FD %d exceeds the maximum size of ast_fdset!\n", fd); \
		} \
	} while (0)
#endif /* HAVE_VARIABLE_FDSET */

/*! \brief Waits for activity on a group of channels 
 * \param nfds the maximum number of file descriptors in the sets
 * \param rfds file descriptors to check for read availability
 * \param wfds file descriptors to check for write availability
 * \param efds file descriptors to check for exceptions (OOB data)
 * \param tvp timeout while waiting for events
 * This is the same as a standard select(), except it guarantees the
 * behaviour where the passed struct timeval is updated with how much
 * time was not slept while waiting for the specified events
 */
static inline int ast_select(int nfds, ast_fdset *rfds, ast_fdset *wfds, ast_fdset *efds, struct timeval *tvp)
{
#ifdef __linux__
	ast_assert((unsigned int) nfds <= ast_FD_SETSIZE);
	return select(nfds, (fd_set *) rfds, (fd_set *) wfds, (fd_set *) efds, tvp);
#else
	int save_errno = 0;

	ast_assert((unsigned int) nfds <= ast_FD_SETSIZE);
	if (tvp) {
		struct timeval tv, tvstart, tvend, tvlen;
		int res;

		tv = *tvp;
		gettimeofday(&tvstart, NULL);
		res = select(nfds, (fd_set *) rfds, (fd_set *) wfds, (fd_set *) efds, tvp);
		save_errno = errno;
		gettimeofday(&tvend, NULL);
		timersub(&tvend, &tvstart, &tvlen);
		timersub(&tv, &tvlen, tvp);
		if (tvp->tv_sec < 0 || (tvp->tv_sec == 0 && tvp->tv_usec < 0)) {
			tvp->tv_sec = 0;
			tvp->tv_usec = 0;
		}
		errno = save_errno;
		return res;
	}
	else
		return select(nfds, (fd_set *) rfds, (fd_set *) wfds, (fd_set *) efds, NULL);
#endif
}

#ifdef __cplusplus
}
#endif

#endif /* __AST_SELECT_H */
