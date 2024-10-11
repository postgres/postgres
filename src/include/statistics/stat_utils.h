/*-------------------------------------------------------------------------
 *
 * stat_utils.h
 *	  Extended statistics and selectivity estimation functions.
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/statistics/stat_utils.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef STATS_UTILS_H
#define STATS_UTILS_H

#include "fmgr.h"

struct StatsArgInfo
{
	const char *argname;
	Oid			argtype;
};

extern void stats_check_required_arg(FunctionCallInfo fcinfo,
									 struct StatsArgInfo *arginfo,
									 int argnum);
extern void stats_lock_check_privileges(Oid reloid);

#endif							/* STATS_UTILS_H */
