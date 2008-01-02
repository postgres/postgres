/* ----------
 *	pg_trace.h
 *
 *	Definitions for the PostgreSQL tracing framework
 *
 *	Copyright (c) 2006-2008, PostgreSQL Global Development Group
 *
 *	$PostgreSQL: pgsql/src/include/pg_trace.h,v 1.3 2008/01/02 02:42:06 momjian Exp $
 * ----------
 */

#ifndef PG_TRACE_H
#define PG_TRACE_H

#ifdef ENABLE_DTRACE

#include <sys/sdt.h>

/*
 * The PG_TRACE macros are mapped to the appropriate macros used by DTrace.
 *
 * Only one DTrace provider called "postgresql" will be used for PostgreSQL,
 * so the name is hard-coded here to avoid having to specify it in the
 * source code.
 */

#define PG_TRACE(name) \
	DTRACE_PROBE(postgresql, name)
#define PG_TRACE1(name, arg1) \
	DTRACE_PROBE1(postgresql, name, arg1)
#define PG_TRACE2(name, arg1, arg2) \
	DTRACE_PROBE2(postgresql, name, arg1, arg2)
#define PG_TRACE3(name, arg1, arg2, arg3) \
	DTRACE_PROBE3(postgresql, name, arg1, arg2, arg3)
#define PG_TRACE4(name, arg1, arg2, arg3, arg4) \
	DTRACE_PROBE4(postgresql, name, arg1, arg2, arg3, arg4)
#define PG_TRACE5(name, arg1, arg2, arg3, arg4, arg5) \
	DTRACE_PROBE5(postgresql, name, arg1, arg2, arg3, arg4, arg5)
#else							/* not ENABLE_DTRACE */

/*
 * Unless DTrace is explicitly enabled with --enable-dtrace, the PG_TRACE
 * macros will expand to no-ops.
 */

#define PG_TRACE(name)
#define PG_TRACE1(name, arg1)
#define PG_TRACE2(name, arg1, arg2)
#define PG_TRACE3(name, arg1, arg2, arg3)
#define PG_TRACE4(name, arg1, arg2, arg3, arg4)
#define PG_TRACE5(name, arg1, arg2, arg3, arg4, arg5)
#endif   /* not ENABLE_DTRACE */

#endif   /* PG_TRACE_H */
