/*-------------------------------------------------------------------------
 *
 * functions.h--
 *
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: functions.h,v 1.2 1997/09/07 04:57:48 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef FUNCTIONS_H
#define FUNCTIONS_H

extern Datum
ProjectAttribute(TupleDesc TD, TargetEntry * tlist,
				 HeapTuple tup, bool * isnullP);

extern Datum
postquel_function(Func * funcNode, char **args,
				  bool * isNull, bool * isDone);

#endif							/* FUNCTIONS_H */
