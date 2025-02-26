/*-------------------------------------------------------------------------
 *
 * stat_utils.h
 *	  Extended statistics and selectivity estimation functions.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
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
extern bool stats_check_arg_array(FunctionCallInfo fcinfo,
								  struct StatsArgInfo *arginfo, int argnum);
extern bool stats_check_arg_pair(FunctionCallInfo fcinfo,
								 struct StatsArgInfo *arginfo,
								 int argnum1, int argnum2);

extern void stats_lock_check_privileges(Oid reloid);

extern bool stats_fill_fcinfo_from_arg_pairs(FunctionCallInfo pairs_fcinfo,
											 FunctionCallInfo positional_fcinfo,
											 struct StatsArgInfo *arginfo);

#endif							/* STATS_UTILS_H */
