/*-------------------------------------------------------------------------
 *
 * functions.h
 *
 *
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: functions.h,v 1.13 2000/08/08 15:42:39 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef FUNCTIONS_H
#define FUNCTIONS_H

#include "nodes/parsenodes.h"
#include "utils/syscache.h"

extern Datum postquel_function(FunctionCallInfo fcinfo,
							   FunctionCachePtr fcache,
							   bool *isDone);

#endif	 /* FUNCTIONS_H */
