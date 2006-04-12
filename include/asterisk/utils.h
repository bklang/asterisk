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
 * \brief Utility functions
 */

#ifndef _ASTERISK_UTILS_H
#define _ASTERISK_UTILS_H

#include "asterisk/compat.h"

#include <stdlib.h>
#include <netinet/in.h>
#include <arpa/inet.h>	/* we want to override inet_ntoa */
#include <netdb.h>
#include <limits.h>

#include "asterisk/lock.h"
#include "asterisk/time.h"
#include "asterisk/strings.h"
#include "asterisk/logger.h"

/*! \note
 \verbatim
   Note:
   It is very important to use only unsigned variables to hold
   bit flags, as otherwise you can fall prey to the compiler's
   sign-extension antics if you try to use the top two bits in
   your variable.

   The flag macros below use a set of compiler tricks to verify
   that the caller is using an "unsigned int" variable to hold
   the flags, and nothing else. If the caller uses any other
   type of variable, a warning message similar to this:

   warning: comparison of distinct pointer types lacks cast
   will be generated.

   The "dummy" variable below is used to make these comparisons.

   Also note that at -O2 or above, this type-safety checking
   does _not_ produce any additional object code at all.
 \endverbatim
*/

extern unsigned int __unsigned_int_flags_dummy;

#define ast_test_flag(p,flag) 		({ \
					typeof ((p)->flags) __p = (p)->flags; \
					typeof (__unsigned_int_flags_dummy) __x = 0; \
					(void) (&__p == &__x); \
					((p)->flags & (flag)); \
					})

#define ast_set_flag(p,flag) 		do { \
					typeof ((p)->flags) __p = (p)->flags; \
					typeof (__unsigned_int_flags_dummy) __x = 0; \
					(void) (&__p == &__x); \
					((p)->flags |= (flag)); \
					} while(0)

#define ast_clear_flag(p,flag) 		do { \
					typeof ((p)->flags) __p = (p)->flags; \
					typeof (__unsigned_int_flags_dummy) __x = 0; \
					(void) (&__p == &__x); \
					((p)->flags &= ~(flag)); \
					} while(0)

#define ast_copy_flags(dest,src,flagz)	do { \
					typeof ((dest)->flags) __d = (dest)->flags; \
					typeof ((src)->flags) __s = (src)->flags; \
					typeof (__unsigned_int_flags_dummy) __x = 0; \
					(void) (&__d == &__x); \
					(void) (&__s == &__x); \
					(dest)->flags &= ~(flagz); \
					(dest)->flags |= ((src)->flags & (flagz)); \
					} while (0)

#define ast_set2_flag(p,value,flag)	do { \
					typeof ((p)->flags) __p = (p)->flags; \
					typeof (__unsigned_int_flags_dummy) __x = 0; \
					(void) (&__p == &__x); \
					if (value) \
						(p)->flags |= (flag); \
					else \
						(p)->flags &= ~(flag); \
					} while (0)

/* Non-type checking variations for non-unsigned int flags.  You
   should only use non-unsigned int flags where required by 
   protocol etc and if you know what you're doing :)  */
#define ast_test_flag_nonstd(p,flag) 		({ \
					((p)->flags & (flag)); \
					})

#define ast_set_flag_nonstd(p,flag) 		do { \
					((p)->flags |= (flag)); \
					} while(0)

#define ast_clear_flag_nonstd(p,flag) 		do { \
					((p)->flags &= ~(flag)); \
					} while(0)

#define ast_copy_flags_nonstd(dest,src,flagz)	do { \
					(dest)->flags &= ~(flagz); \
					(dest)->flags |= ((src)->flags & (flagz)); \
					} while (0)

#define ast_set2_flag_nonstd(p,value,flag)	do { \
					if (value) \
						(p)->flags |= (flag); \
					else \
						(p)->flags &= ~(flag); \
					} while (0)

#define AST_FLAGS_ALL UINT_MAX

struct ast_flags {
	unsigned int flags;
};

struct ast_hostent {
	struct hostent hp;
	char buf[1024];
};

struct hostent *ast_gethostbyname(const char *host, struct ast_hostent *hp);

/* ast_md5_hash 
	\brief Produces MD5 hash based on input string */
void ast_md5_hash(char *output, char *input);
/* ast_sha1_hash
	\brief Produces SHA1 hash based on input string */
void ast_sha1_hash(char *output, char *input);

int ast_base64encode(char *dst, const unsigned char *src, int srclen, int max);
int ast_base64decode(unsigned char *dst, const char *src, int max);

/*! ast_uri_encode
	\brief Turn text string to URI-encoded %XX version 
 	At this point, we're converting from ISO-8859-x (8-bit), not UTF8
	as in the SIP protocol spec 
	If doreserved == 1 we will convert reserved characters also.
	RFC 2396, section 2.4
	outbuf needs to have more memory allocated than the instring
	to have room for the expansion. Every char that is converted
	is replaced by three ASCII characters.
	\param string	String to be converted
	\param outbuf	Resulting encoded string
	\param buflen	Size of output buffer
	\param doreserved	Convert reserved characters
*/

char *ast_uri_encode(const char *string, char *outbuf, int buflen, int doreserved);

/*!	\brief Decode URI, URN, URL (overwrite string)
	\param s	String to be decoded 
 */
void ast_uri_decode(char *s);

static force_inline void ast_slinear_saturated_add(short *input, short *value)
{
	int res;

	res = (int) *input + *value;
	if (res > 32767)
		*input = 32767;
	else if (res < -32767)
		*input = -32767;
	else
		*input = (short) res;
}
	
static force_inline void ast_slinear_saturated_multiply(short *input, short *value)
{
	int res;

	res = (int) *input * *value;
	if (res > 32767)
		*input = 32767;
	else if (res < -32767)
		*input = -32767;
	else
		*input = (short) res;
}

static force_inline void ast_slinear_saturated_divide(short *input, short *value)
{
	*input /= *value;
}

int test_for_thread_safety(void);

const char *ast_inet_ntoa(char *buf, int bufsiz, struct in_addr ia);

#ifdef inet_ntoa
#undef inet_ntoa
#endif
#define inet_ntoa __dont__use__inet_ntoa__use__ast_inet_ntoa__instead__

int ast_utils_init(void);
int ast_wait_for_input(int fd, int ms);

/*! Compares the source address and port of two sockaddr_in */
static force_inline int inaddrcmp(const struct sockaddr_in *sin1, const struct sockaddr_in *sin2)
{
	return ((sin1->sin_addr.s_addr != sin2->sin_addr.s_addr) 
		|| (sin1->sin_port != sin2->sin_port));
}

#define AST_STACKSIZE 256 * 1024

void ast_register_thread(char *name);
void ast_unregister_thread(void *id);

#define ast_pthread_create(a,b,c,d) ast_pthread_create_stack(a,b,c,d,0, \
	 __FILE__, __FUNCTION__, __LINE__, #c)
int ast_pthread_create_stack(pthread_t *thread, pthread_attr_t *attr, void *(*start_routine)(void *), void *data, size_t stacksize,
	const char *file, const char *caller, int line, const char *start_fn);

/*!
	\brief Process a string to find and replace characters
	\param start The string to analyze
	\param find The character to find
	\param replace_with The character that will replace the one we are looking for
*/
char *ast_process_quotes_and_slashes(char *start, char find, char replace_with);

#ifndef HAVE_GETLOADAVG
int getloadavg(double *list, int nelem);
#endif

#ifdef linux
#define ast_random random
#else
long int ast_random(void);
#endif

#ifndef __AST_DEBUG_MALLOC

/*!
  \brief A wrapper for malloc()

  ast_malloc() is a wrapper for malloc() that will generate an Asterisk log
  message in the case that the allocation fails.

  The argument and return value are the same as malloc()
*/
#define ast_malloc(len) \
	_ast_malloc((len), __FILE__, __LINE__, __PRETTY_FUNCTION__)

AST_INLINE_API(
void *_ast_malloc(size_t len, const char *file, int lineno, const char *func),
{
	void *p;

	p = malloc(len);

	if (!p)
		ast_log(LOG_ERROR, "Memory Allocation Failure - '%d' bytes in function %s at line %d of %s\n", (int)len, func, lineno, file);

	return p;
}
)

/*!
  \brief A wrapper for calloc()

  ast_calloc() is a wrapper for calloc() that will generate an Asterisk log
  message in the case that the allocation fails.

  The arguments and return value are the same as calloc()
*/
#define ast_calloc(num, len) \
	_ast_calloc((num), (len), __FILE__, __LINE__, __PRETTY_FUNCTION__)

AST_INLINE_API(
void *_ast_calloc(size_t num, size_t len, const char *file, int lineno, const char *func),
{
	void *p;

	p = calloc(num, len);

	if (!p)
		ast_log(LOG_ERROR, "Memory Allocation Failure - '%d' bytes in function %s at line %d of %s\n", (int)len, func, lineno, file);

	return p;
}
)

/*!
  \brief A wrapper for realloc()

  ast_realloc() is a wrapper for realloc() that will generate an Asterisk log
  message in the case that the allocation fails.

  The arguments and return value are the same as realloc()
*/
#define ast_realloc(p, len) \
	_ast_realloc((p), (len), __FILE__, __LINE__, __PRETTY_FUNCTION__)

AST_INLINE_API(
void *_ast_realloc(void *p, size_t len, const char *file, int lineno, const char *func),
{
	void *newp;

	newp = realloc(p, len);

	if (!newp)
		ast_log(LOG_ERROR, "Memory Allocation Failure - '%d' bytes in function %s at line %d of %s\n", (int)len, func, lineno, file);

	return newp;
}
)

/*!
  \brief A wrapper for strdup()

  ast_strdup() is a wrapper for strdup() that will generate an Asterisk log
  message in the case that the allocation fails.

  ast_strdup(), unlike strdup(), can safely accept a NULL argument. If a NULL
  argument is provided, ast_strdup will return NULL without generating any
  kind of error log message.

  The argument and return value are the same as strdup()
*/
#define ast_strdup(str) \
	_ast_strdup((str), __FILE__, __LINE__, __PRETTY_FUNCTION__)

AST_INLINE_API(
char *_ast_strdup(const char *str, const char *file, int lineno, const char *func),
{
	char *newstr = NULL;

	if (str) {
		newstr = strdup(str);

		if (!newstr)
			ast_log(LOG_ERROR, "Memory Allocation Failure - Could not duplicate '%s' in function %s at line %d of %s\n", str, func, lineno, file);
	}

	return newstr;
}
)

/*!
  \brief A wrapper for strndup()

  ast_strndup() is a wrapper for strndup() that will generate an Asterisk log
  message in the case that the allocation fails.

  ast_strndup(), unlike strndup(), can safely accept a NULL argument for the
  string to duplicate. If a NULL argument is provided, ast_strdup will return  
  NULL without generating any kind of error log message.

  The arguments and return value are the same as strndup()
*/
#define ast_strndup(str, len) \
	_ast_strndup((str), (len), __FILE__, __LINE__, __PRETTY_FUNCTION__)

AST_INLINE_API(
char *_ast_strndup(const char *str, size_t len, const char *file, int lineno, const char *func),
{
	char *newstr = NULL;

	if (str) {
		newstr = strndup(str, len);

		if (!newstr)
			ast_log(LOG_ERROR, "Memory Allocation Failure - Could not duplicate '%d' bytes of '%s' in function %s at line %d of %s\n", (int)len, str, func, lineno, file);
	}

	return newstr;
}
)

#else

/* If astmm is in use, let it handle these.  Otherwise, it will report that
   all allocations are coming from this header file */

#define ast_malloc(a)		malloc(a)
#define ast_calloc(a,b)		calloc(a,b)
#define ast_realloc(a,b)	realloc(a,b)
#define ast_strdup(a)		strdup(a)
#define ast_strndup(a,b)	strndup(a,b)

#endif /* AST_DEBUG_MALLOC */

#if !defined(ast_strdupa) && defined(__GNUC__)
/*!
  \brief duplicate a string in memory from the stack
  \param s The string to duplicate

  This macro will duplicate the given string.  It returns a pointer to the stack
  allocatted memory for the new string.
*/
#define ast_strdupa(s)                                                    \
	(__extension__                                                    \
	({                                                                \
		const char *__old = (s);                                  \
		size_t __len = strlen(__old) + 1;                         \
		char *__new = __builtin_alloca(__len);                    \
		if (__builtin_expect(!__new, 0))                          \
			ast_log(LOG_ERROR, "Stack Allocation Error in"    \
				"function '%s' at line '%d' of '%s'!\n",  \
				__PRETTY_FUNCTION__, __LINE__, __FILE__); \
		else                                                      \
			memcpy (__new, __old, __len);                     \
		__new;                                                    \
	}))
#endif

#endif /* _ASTERISK_UTILS_H */
