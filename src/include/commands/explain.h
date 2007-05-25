/*-------------------------------------------------------------------------
 *
 * explain.h
 *	  prototypes for explain.c
 *
 * Portions Copyright (c) 1996-2007, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994-5, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/commands/explain.h,v 1.31 2007/05/25 17:54:25 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef EXPLAIN_H
#define EXPLAIN_H

#include "executor/executor.h"

/* Hook for plugins to get control in ExplainOneQuery() */
typedef void (*ExplainOneQuery_hook_type) (Query *query,
										   ExplainStmt *stmt,
										   const char *queryString,
										   ParamListInfo params,
										   TupOutputState *tstate);
extern DLLIMPORT ExplainOneQuery_hook_type ExplainOneQuery_hook;

/* Hook for plugins to get control in explain_get_index_name() */
typedef const char * (*explain_get_index_name_hook_type) (Oid indexId);
extern DLLIMPORT explain_get_index_name_hook_type explain_get_index_name_hook;


extern void ExplainQuery(ExplainStmt *stmt, const char *queryString,
						 ParamListInfo params, DestReceiver *dest);

extern TupleDesc ExplainResultDesc(ExplainStmt *stmt);

extern void ExplainOneUtility(Node *utilityStmt, ExplainStmt *stmt,
							  const char *queryString,
							  ParamListInfo params,
							  TupOutputState *tstate);

extern void ExplainOnePlan(PlannedStmt *plannedstmt, ParamListInfo params,
						   ExplainStmt *stmt, TupOutputState *tstate);

#endif   /* EXPLAIN_H */
