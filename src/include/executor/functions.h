/*-------------------------------------------------------------------------
 *
 * functions.h
 *
 *
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: functions.h,v 1.12 2000/05/28 17:56:18 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef FUNCTIONS_H
#define FUNCTIONS_H

#include "nodes/parsenodes.h"
#include "utils/syscache.h"

extern Datum ProjectAttribute(TupleDesc TD, TargetEntry *tlist,
							  HeapTuple tup, bool *isnullP);

extern Datum postquel_function(FunctionCallInfo fcinfo,
							   FunctionCachePtr fcache,
							   List *func_tlist,
							   bool *isDone);

#endif	 /* FUNCTIONS_H */
