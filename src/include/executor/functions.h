/*-------------------------------------------------------------------------
 *
 * functions.h--
 *
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: functions.h,v 1.7 1998/02/26 04:41:20 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef FUNCTIONS_H
#define FUNCTIONS_H

#include "access/tupdesc.h"
#include "nodes/parsenodes.h"
#include "nodes/primnodes.h"
#include "utils/syscache.h"

extern Datum
ProjectAttribute(TupleDesc TD, TargetEntry *tlist,
				 HeapTuple tup, bool *isnullP);

extern Datum
postquel_function(Func *funcNode, char **args,
				  bool *isNull, bool *isDone);

#endif							/* FUNCTIONS_H */
