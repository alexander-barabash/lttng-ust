#ifndef _LTTNG_TRACEPOINT_INTERNAL_H
#define _LTTNG_TRACEPOINT_INTERNAL_H

/*
 * Copyright (c) 2011 - Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License.
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

#include <urcu/list.h>
#include <urcu-bp.h>
#include <lttng/tracepoint-types.h>

#define TRACE_DEFAULT	TRACE_DEBUG_LINE

struct tracepoint_lib {
	struct cds_list_head list;
	struct tracepoint * const *tracepoints_start;
	int tracepoints_count;
};

extern int tracepoint_probe_register_noupdate(const char *name,
		void (*callback)(void), void *priv,
		const char *signature);
extern int tracepoint_probe_unregister_noupdate(const char *name,
		void (*callback)(void), void *priv);
extern void tracepoint_probe_update_all(void);

/*
 * call after disconnection of last probe implemented within a
 * shared object before unmapping the library that contains the probe.
 */
static inline void tracepoint_synchronize_unregister(void)
{
	synchronize_rcu_bp();
}

extern void init_tracepoint(void);
extern void exit_tracepoint(void);

#endif /* _LTTNG_TRACEPOINT_INTERNAL_H */
