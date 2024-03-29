#ifndef _USTERR_H
#define _USTERR_H

/*
 * Copyright (C) 2009  Pierre-Marc Fournier
 * Copyright (C) 2011  Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; version 2.1 of
 * the License.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA
 */

#include <string.h>
#include <sys/types.h>
#include <sys/syscall.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>

#include "share.h"

enum ust_loglevel {
	UST_LOGLEVEL_UNKNOWN = 0,
	UST_LOGLEVEL_NORMAL,
	UST_LOGLEVEL_DEBUG,
};

extern volatile enum ust_loglevel ust_loglevel;
void init_usterr(void);

static inline int ust_debug(void)
{
	return ust_loglevel == UST_LOGLEVEL_DEBUG;
}

#ifndef UST_COMPONENT
#define UST_COMPONENT libust
#endif

/* To stringify the expansion of a define */
#define XSTR(d) STR(d)
#define STR(s) #s

#define UST_STR_COMPONENT XSTR(UST_COMPONENT)

#define ERRMSG(fmt, args...)					\
	do {							\
		fprintf(stderr, UST_STR_COMPONENT "[%ld/%ld]: " fmt " (in %s() at " __FILE__ ":" XSTR(__LINE__) ")\n",				\
			(long) getpid(),			\
			(long) syscall(SYS_gettid),		\
			## args,				\
			__func__);				\
	} while(0)

#ifdef LTTNG_UST_DEBUG
# define DBG(fmt, args...)		ERRMSG(fmt, ## args)
# define DBG_raw(fmt, args...)					\
	do {							\
		 fprintf(stderr, fmt, ## args);			\
	} while(0)
#else
# define DBG(fmt, args...)					\
	do {							\
		if (ust_debug())				\
			ERRMSG(fmt, ## args);			\
	} while (0)
# define DBG_raw(fmt, args...)					\
	do {							\
		if (ust_debug()) {				\
			fprintf(stderr, fmt, ## args);		\
		}						\
	} while(0)
#endif

#define WARN(fmt, args...) ERRMSG("Warning: " fmt, ## args)
#define ERR(fmt, args...) ERRMSG("Error: " fmt, ## args)
#define BUG(fmt, args...) ERRMSG("BUG: " fmt, ## args)

#if (_POSIX_C_SOURCE >= 200112L || _XOPEN_SOURCE >= 600) && !defined(_GNU_SOURCE)
#define PERROR(call, args...)\
	do { \
		char buf[200] = "Error in strerror_r()"; \
		strerror_r(errno, buf, sizeof(buf)); \
		ERRMSG("Error: " call ": %s", ## args, buf); \
	} while(0);
#else
#define PERROR(call, args...)\
	do { \
		char *buf; \
		char tmp[200]; \
		buf = strerror_r(errno, tmp, sizeof(tmp)); \
		ERRMSG("Error: " call ": %s", ## args, buf); \
	} while(0);
#endif

#define BUG_ON(condition)					\
	do {							\
		if (caa_unlikely(condition))			\
			ERR("condition not respected (BUG) on line %s:%d", __FILE__, __LINE__);	\
	} while(0)
#define WARN_ON(condition)					\
	do {							\
		if (caa_unlikely(condition))			\
			WARN("condition not respected on line %s:%d", __FILE__, __LINE__); \
	} while(0)
#define WARN_ON_ONCE(condition) WARN_ON(condition)

#endif /* _USTERR_H */
