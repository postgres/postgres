/*-------------------------------------------------------------------------
 *
 * explain.h--
 *	  prototypes for explain.c
 *
 * Copyright (c) 1994-5, Regents of the University of California
 *
 * $Id: explain.h,v 1.7 1998/09/01 04:35:31 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef EXPLAIN_H
#define EXPLAIN_H

#include "tcop/dest.h"
#include "nodes/parsenodes.h"

extern void ExplainQuery(Query *query, bool verbose, CommandDest dest);

#endif	 /* EXPLAIN_H */
