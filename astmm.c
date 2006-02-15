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
 *
 * \brief Memory Management
 *
 * \author Mark Spencer <markster@digium.com>
 */

#ifdef __AST_DEBUG_MALLOC

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "asterisk.h"

ASTERISK_FILE_VERSION(__FILE__, "$Revision$")

#include "asterisk/cli.h"
#include "asterisk/logger.h"
#include "asterisk/options.h"
#include "asterisk/lock.h"
#include "asterisk/strings.h"

#define SOME_PRIME 563

enum func_type {
	FUNC_CALLOC = 1,
	FUNC_MALLOC,
	FUNC_REALLOC,
	FUNC_STRDUP,
	FUNC_STRNDUP,
	FUNC_VASPRINTF,
	FUNC_ASPRINTF
};

/* Undefine all our macros */
#undef malloc
#undef calloc
#undef realloc
#undef strdup
#undef strndup
#undef free
#undef vasprintf
#undef asprintf

#define FENCE_MAGIC 0xdeadbeef

static FILE *mmlog;

static struct ast_region {
	struct ast_region *next;
	char file[40];
	char func[40];
	int lineno;
	enum func_type which;
	size_t len;
	unsigned int fence;
	unsigned char data[0];
} *regions[SOME_PRIME];

#define HASH(a) \
	(((unsigned long)(a)) % SOME_PRIME)
	
AST_MUTEX_DEFINE_STATIC(reglock);
AST_MUTEX_DEFINE_STATIC(showmemorylock);

static inline void *__ast_alloc_region(size_t size, const enum func_type which, const char *file, int lineno, const char *func)
{
	struct ast_region *reg;
	void *ptr = NULL;
	unsigned int *fence;
	int hash;
	reg = malloc(size + sizeof(*reg) + sizeof(*fence));
	ast_mutex_lock(&reglock);
	if (reg) {
		ast_copy_string(reg->file, file, sizeof(reg->file));
		reg->file[sizeof(reg->file) - 1] = '\0';
		ast_copy_string(reg->func, func, sizeof(reg->func));
		reg->func[sizeof(reg->func) - 1] = '\0';
		reg->lineno = lineno;
		reg->len = size;
		reg->which = which;
		ptr = reg->data;
		hash = HASH(ptr);
		reg->next = regions[hash];
		regions[hash] = reg;
		reg->fence = FENCE_MAGIC;
		fence = (ptr + reg->len);
		*fence = FENCE_MAGIC;
	}
	ast_mutex_unlock(&reglock);
	if (!reg) {
		fprintf(stderr, "Memory allocation failure\n");
		if (mmlog) {
			fprintf(mmlog, "%ld - Memory allocation failure\n", time(NULL));
			fflush(mmlog);
		}
	}
	return ptr;
}

static inline size_t __ast_sizeof_region(void *ptr)
{
	int hash = HASH(ptr);
	struct ast_region *reg;
	size_t len = 0;
	
	ast_mutex_lock(&reglock);
	reg = regions[hash];
	while (reg) {
		if (reg->data == ptr) {
			len = reg->len;
			break;
		}
		reg = reg->next;
	}
	ast_mutex_unlock(&reglock);
	return len;
}

static void __ast_free_region(void *ptr, const char *file, int lineno, const char *func)
{
	int hash = HASH(ptr);
	struct ast_region *reg, *prev = NULL;
	unsigned int *fence;

	ast_mutex_lock(&reglock);
	reg = regions[hash];
	while (reg) {
		if (reg->data == ptr) {
			if (prev) {
				prev->next = reg->next;
			} else {
				regions[hash] = reg->next;
			}
			break;
		}
		prev = reg;
		reg = reg->next;
	}
	ast_mutex_unlock(&reglock);
	if (reg) {
		fence = (unsigned int *)(reg->data + reg->len);
		if (reg->fence != FENCE_MAGIC) {
			fprintf(stderr, "WARNING: Low fence violation at %p, in %s of %s, line %d\n", reg->data, reg->func, reg->file, reg->lineno);
			if (mmlog) {
				fprintf(mmlog, "%ld - WARNING: Low fence violation at %p, in %s of %s, line %d\n", time(NULL), reg->data, reg->func, reg->file, reg->lineno);
				fflush(mmlog);
			}
		}
		if (*fence != FENCE_MAGIC) {
			fprintf(stderr, "WARNING: High fence violation at %p, in %s of %s, line %d\n", reg->data, reg->func, reg->file, reg->lineno);
			if (mmlog) {
				fprintf(mmlog, "%ld - WARNING: High fence violation at %p, in %s of %s, line %d\n", time(NULL), reg->data, reg->func, reg->file, reg->lineno);
				fflush(mmlog);
			}
		}
		free(reg);
	} else {
		fprintf(stderr, "WARNING: Freeing unused memory at %p, in %s of %s, line %d\n",	ptr, func, file, lineno);
		if (mmlog) {
			fprintf(mmlog, "%ld - WARNING: Freeing unused memory at %p, in %s of %s, line %d\n", time(NULL), ptr, func, file, lineno);
			fflush(mmlog);
		}
	}
}

void *__ast_calloc(size_t nmemb, size_t size, const char *file, int lineno, const char *func) 
{
	void *ptr;
	if ((ptr = __ast_alloc_region(size * nmemb, FUNC_CALLOC, file, lineno, func))) 
		memset(ptr, 0, size * nmemb);
	return ptr;
}

void *__ast_malloc(size_t size, const char *file, int lineno, const char *func) 
{
	return __ast_alloc_region(size, FUNC_MALLOC, file, lineno, func);
}

void __ast_free(void *ptr, const char *file, int lineno, const char *func) 
{
	__ast_free_region(ptr, file, lineno, func);
}

void *__ast_realloc(void *ptr, size_t size, const char *file, int lineno, const char *func) 
{
	void *tmp;
	size_t len = 0;
	if (ptr && !(len = __ast_sizeof_region(ptr))) {
		fprintf(stderr, "WARNING: Realloc of unalloced memory at %p, in %s of %s, line %d\n", ptr, func, file, lineno);
		if (mmlog) {
			fprintf(mmlog, "%ld - WARNING: Realloc of unalloced memory at %p, in %s of %s, line %d\n", time(NULL), ptr, func, file, lineno);
			fflush(mmlog);
		}
		return NULL;
	}
	if ((tmp = __ast_alloc_region(size, FUNC_REALLOC, file, lineno, func))) {
		if (len > size)
			len = size;
		if (ptr) {
			memcpy(tmp, ptr, len);
			__ast_free_region(ptr, file, lineno, func);
		}
	}
	return tmp;
}

char *__ast_strdup(const char *s, const char *file, int lineno, const char *func) 
{
	size_t len;
	void *ptr;
	if (!s)
		return NULL;
	len = strlen(s) + 1;
	if ((ptr = __ast_alloc_region(len, FUNC_STRDUP, file, lineno, func)))
		strcpy(ptr, s);
	return ptr;
}

char *__ast_strndup(const char *s, size_t n, const char *file, int lineno, const char *func) 
{
	size_t len;
	void *ptr;
	if (!s)
		return NULL;
	len = strlen(s) + 1;
	if (len > n)
		len = n;
	if ((ptr = __ast_alloc_region(len, FUNC_STRNDUP, file, lineno, func)))
		strcpy(ptr, s);
	return ptr;
}

int __ast_asprintf(const char *file, int lineno, const char *func, char **strp, const char *fmt, ...)
{
	int size;
	va_list ap, ap2;
	char s;

	*strp = NULL;
	va_start(ap, fmt);
	va_copy(ap2, ap);
	size = vsnprintf(&s, 1, fmt, ap2);
	va_end(ap2);
	if (!(*strp = __ast_alloc_region(size + 1, FUNC_ASPRINTF, file, lineno, func))) {
		va_end(ap);
		return -1;
	}
	vsnprintf(*strp, size + 1, fmt, ap);
	va_end(ap);

	return size;
}

int __ast_vasprintf(char **strp, const char *fmt, va_list ap, const char *file, int lineno, const char *func) 
{
	int size;
	va_list ap2;
	char s;

	*strp = NULL;
	va_copy(ap2, ap);
	size = vsnprintf(&s, 1, fmt, ap2);
	va_end(ap2);
	if (!(*strp = __ast_alloc_region(size + 1, FUNC_VASPRINTF, file, lineno, func))) {
		va_end(ap);
		return -1;
	}
	vsnprintf(*strp, size + 1, fmt, ap);

	return size;
}

static int handle_show_memory(int fd, int argc, char *argv[])
{
	char *fn = NULL;
	int x;
	struct ast_region *reg;
	unsigned int len = 0;
	int count = 0;
	unsigned int *fence;
	if (argc > 3) 
		fn = argv[3];

	/* try to lock applications list ... */
	ast_mutex_lock(&showmemorylock);

	for (x = 0; x < SOME_PRIME; x++) {
		reg = regions[x];
		while (reg) {
			if (!fn || !strcasecmp(fn, reg->file) || !strcasecmp(fn, "anomolies")) {
				fence = (unsigned int *)(reg->data + reg->len);
				if (reg->fence != FENCE_MAGIC) {
					fprintf(stderr, "WARNING: Low fence violation at %p, in %s of %s, line %d\n", reg->data, reg->func, reg->file, reg->lineno);
					if (mmlog) {
						fprintf(mmlog, "%ld - WARNING: Low fence violation at %p, in %s of %s, line %d\n", time(NULL), reg->data, reg->func, reg-> file, reg->lineno);
						fflush(mmlog);
					}
				}
				if (*fence != FENCE_MAGIC) {
					fprintf(stderr, "WARNING: High fence violation at %p, in %s of %s, line %d\n", reg->data, reg->func, reg->file, reg->lineno);
					if (mmlog) {
						fprintf(mmlog, "%ld - WARNING: High fence violation at %p, in %s of %s, line %d\n", time(NULL), reg->data, reg->func, reg->file, reg->lineno);
						fflush(mmlog);
					}
				}
			}
			if (!fn || !strcasecmp(fn, reg->file)) {
				ast_cli(fd, "%10d bytes allocated in %20s at line %5d of %s\n", (int) reg->len, reg->func, reg->lineno, reg->file);
				len += reg->len;
				count++;
			}
			reg = reg->next;
		}
	}
	ast_cli(fd, "%d bytes allocated %d units total\n", len, count);
	ast_mutex_unlock(&showmemorylock);
	return RESULT_SUCCESS;
}

struct file_summary {
	char fn[80];
	int len;
	int count;
	struct file_summary *next;
};

static int handle_show_memory_summary(int fd, int argc, char *argv[])
{
	char *fn = NULL;
	int x;
	struct ast_region *reg;
	unsigned int len = 0;
	int count = 0;
	struct file_summary *list = NULL, *cur;
	
	if (argc > 3) 
		fn = argv[3];

	/* try to lock applications list ... */
	ast_mutex_lock(&reglock);

	for (x = 0; x < SOME_PRIME; x++) {
		reg = regions[x];
		while (reg) {
			if (!fn || !strcasecmp(fn, reg->file)) {
				cur = list;
				while (cur) {
					if ((!fn && !strcmp(cur->fn, reg->file)) || (fn && !strcmp(cur->fn, reg->func)))
						break;
					cur = cur->next;
				}
				if (!cur) {
					cur = alloca(sizeof(*cur));
					memset(cur, 0, sizeof(*cur));
					ast_copy_string(cur->fn, fn ? reg->func : reg->file, sizeof(cur->fn));
					cur->next = list;
					list = cur;
				}
				cur->len += reg->len;
				cur->count++;
			}
			reg = reg->next;
		}
	}
	ast_mutex_unlock(&reglock);
	
	/* Dump the whole list */
	while (list) {
		cur = list;
		len += list->len;
		count += list->count;
		if (fn) {
			ast_cli(fd, "%10d bytes in %5d allocations in function '%s' of '%s'\n", list->len, list->count, list->fn, fn);
		} else {
			ast_cli(fd, "%10d bytes in %5d allocations in file '%s'\n", list->len, list->count, list->fn);
		}
		list = list->next;
#if 0
		free(cur);
#endif		
	}
	ast_cli(fd, "%d bytes allocated %d units total\n", len, count);
	return RESULT_SUCCESS;
}

static char show_memory_help[] = 
"Usage: show memory allocations [<file>]\n"
"       Dumps a list of all segments of allocated memory, optionally\n"
"limited to those from a specific file\n";

static char show_memory_summary_help[] = 
"Usage: show memory summary [<file>]\n"
"       Summarizes heap memory allocations by file, or optionally\n"
"by function, if a file is specified\n";

static struct ast_cli_entry show_memory_allocations_cli = 
	{ { "show", "memory", "allocations", NULL }, 
	handle_show_memory, "Display outstanding memory allocations",
	show_memory_help };

static struct ast_cli_entry show_memory_summary_cli = 
	{ { "show", "memory", "summary", NULL }, 
	handle_show_memory_summary, "Summarize outstanding memory allocations",
	show_memory_summary_help };

void __ast_mm_init(void)
{
	char filename[80] = "";
	ast_cli_register(&show_memory_allocations_cli);
	ast_cli_register(&show_memory_summary_cli);
	
	snprintf(filename, sizeof(filename), "%s/mmlog", (char *)ast_config_AST_LOG_DIR);
	mmlog = fopen(filename, "a+");
	if (option_verbose)
		ast_verbose("Asterisk Malloc Debugger Started (see %s))\n", filename);
	if (mmlog) {
		fprintf(mmlog, "%ld - New session\n", time(NULL));
		fflush(mmlog);
	}
}

#endif
