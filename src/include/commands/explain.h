/*-------------------------------------------------------------------------
 *
 * explain.h
 *	  prototypes for explain.c
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994-5, Regents of the University of California
 *
 * $Id: explain.h,v 1.12 2001/09/18 01:59:06 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef EXPLAIN_H
#define EXPLAIN_H

#include "nodes/parsenodes.h"
#include "tcop/dest.h"

extern void ExplainQuery(Query *query, bool verbose, bool analyze, CommandDest dest);

#endif	 /* EXPLAIN_H */
