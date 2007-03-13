/*-------------------------------------------------------------------------
 *
 * explain.h
 *	  prototypes for explain.c
 *
 * Portions Copyright (c) 1996-2007, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994-5, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/commands/explain.h,v 1.30 2007/03/13 00:33:43 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef EXPLAIN_H
#define EXPLAIN_H

#include "executor/executor.h"


extern void ExplainQuery(ExplainStmt *stmt, const char *queryString,
						 ParamListInfo params, DestReceiver *dest);

extern TupleDesc ExplainResultDesc(ExplainStmt *stmt);

extern void ExplainOneUtility(Node *utilityStmt, ExplainStmt *stmt,
							  const char *queryString,
							  ParamListInfo params,
							  TupOutputState *tstate);

extern void ExplainOnePlan(QueryDesc *queryDesc, ExplainStmt *stmt,
			   TupOutputState *tstate);

#endif   /* EXPLAIN_H */
