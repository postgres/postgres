/*-------------------------------------------------------------------------
 *
 * explain.h
 *	  prototypes for explain.c
 *
 * Copyright (c) 1994-5, Regents of the University of California
 *
 * $Id: explain.h,v 1.9 1999/07/16 17:07:31 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef EXPLAIN_H
#define EXPLAIN_H

#include "nodes/parsenodes.h"
#include "tcop/dest.h"

extern void ExplainQuery(Query *query, bool verbose, CommandDest dest);

#endif	 /* EXPLAIN_H */
