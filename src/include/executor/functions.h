/*-------------------------------------------------------------------------
 *
 * functions.h--
 *
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: functions.h,v 1.3 1997/09/08 02:36:18 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef FUNCTIONS_H
#define FUNCTIONS_H

extern		Datum
ProjectAttribute(TupleDesc TD, TargetEntry * tlist,
				 HeapTuple tup, bool * isnullP);

extern		Datum
postquel_function(Func * funcNode, char **args,
				  bool * isNull, bool * isDone);

#endif							/* FUNCTIONS_H */
