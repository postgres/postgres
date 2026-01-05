/*-------------------------------------------------------------------------
 *
 * stat_utils.h
 *	  Extended statistics and selectivity estimation functions.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/statistics/stat_utils.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef STATS_UTILS_H
#define STATS_UTILS_H

#include "access/attnum.h"
#include "fmgr.h"

/* avoid including primnodes.h here */
typedef struct RangeVar RangeVar;

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

extern void RangeVarCallbackForStats(const RangeVar *relation,
									 Oid relId, Oid oldRelId, void *arg);

extern bool stats_fill_fcinfo_from_arg_pairs(FunctionCallInfo pairs_fcinfo,
											 FunctionCallInfo positional_fcinfo,
											 struct StatsArgInfo *arginfo);

extern void statatt_get_type(Oid reloid, AttrNumber attnum,
							 Oid *atttypid, int32 *atttypmod,
							 char *atttyptype, Oid *atttypcoll,
							 Oid *eq_opr, Oid *lt_opr);
extern void statatt_init_empty_tuple(Oid reloid, int16 attnum, bool inherited,
									 Datum *values, bool *nulls, bool *replaces);

extern void statatt_set_slot(Datum *values, bool *nulls, bool *replaces,
							 int16 stakind, Oid staop, Oid stacoll,
							 Datum stanumbers, bool stanumbers_isnull,
							 Datum stavalues, bool stavalues_isnull);

extern Datum statatt_build_stavalues(const char *staname, FmgrInfo *array_in, Datum d,
									 Oid typid, int32 typmod, bool *ok);
extern bool statatt_get_elem_type(Oid atttypid, char atttyptype,
								  Oid *elemtypid, Oid *elem_eq_opr);

#endif							/* STATS_UTILS_H */
