/*-------------------------------------------------------------------------
 *
 * explain.h
 *	  prototypes for explain.c
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994-5, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/commands/explain.h,v 1.28 2006/10/04 00:30:08 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef EXPLAIN_H
#define EXPLAIN_H

#include "executor/executor.h"


extern void ExplainQuery(ExplainStmt *stmt, ParamListInfo params,
			 DestReceiver *dest);

extern TupleDesc ExplainResultDesc(ExplainStmt *stmt);

extern void ExplainOnePlan(QueryDesc *queryDesc, ExplainStmt *stmt,
			   TupOutputState *tstate);

#endif   /* EXPLAIN_H */
