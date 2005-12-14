/*-------------------------------------------------------------------------
 *
 * prepare.h
 *	  PREPARE, EXECUTE and DEALLOCATE commands, and prepared-stmt storage
 *
 *
 * Copyright (c) 2002-2003, PostgreSQL Global Development Group
 *
 * $Id: prepare.h,v 1.8.4.1 2005/12/14 17:07:00 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PREPARE_H
#define PREPARE_H

#include "executor/executor.h"
#include "nodes/parsenodes.h"
#include "tcop/dest.h"


/*
 * The data structure representing a prepared statement
 *
 * Note: all subsidiary storage lives in the context denoted by the context
 * field.  However, the string referenced by commandTag is not subsidiary
 * storage; it is assumed to be a compile-time-constant string.  As with
 * portals, commandTag shall be NULL if and only if the original query string
 * (before rewriting) was an empty string.
 */
typedef struct
{
	/* dynahash.c requires key to be first field */
	char		stmt_name[NAMEDATALEN];
	char	   *query_string;	/* text of query, or NULL */
	const char *commandTag;		/* command tag (a constant!), or NULL */
	List	   *query_list;		/* list of queries */
	List	   *plan_list;		/* list of plans */
	List	   *argtype_list;	/* list of parameter type OIDs */
	MemoryContext context;		/* context containing this query */
} PreparedStatement;


/* Utility statements PREPARE, EXECUTE, DEALLOCATE, EXPLAIN EXECUTE */
extern void PrepareQuery(PrepareStmt *stmt);
extern void ExecuteQuery(ExecuteStmt *stmt, DestReceiver *dest);
extern void DeallocateQuery(DeallocateStmt *stmt);
extern void ExplainExecuteQuery(ExplainStmt *stmt, TupOutputState *tstate);

/* Low-level access to stored prepared statements */
extern void StorePreparedStatement(const char *stmt_name,
					   const char *query_string,
					   const char *commandTag,
					   List *query_list,
					   List *plan_list,
					   List *argtype_list);
extern PreparedStatement *FetchPreparedStatement(const char *stmt_name,
					   bool throwError);
extern void DropPreparedStatement(const char *stmt_name, bool showError);
extern List *FetchPreparedStatementParams(const char *stmt_name);
extern TupleDesc FetchPreparedStatementResultDesc(PreparedStatement *stmt);
extern bool PreparedStatementReturnsTuples(PreparedStatement *stmt);

#endif   /* PREPARE_H */
