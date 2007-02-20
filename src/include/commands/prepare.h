/*-------------------------------------------------------------------------
 *
 * prepare.h
 *	  PREPARE, EXECUTE and DEALLOCATE commands, and prepared-stmt storage
 *
 *
 * Copyright (c) 2002-2007, PostgreSQL Global Development Group
 *
 * $PostgreSQL: pgsql/src/include/commands/prepare.h,v 1.24 2007/02/20 17:32:17 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PREPARE_H
#define PREPARE_H

#include "executor/executor.h"
#include "utils/timestamp.h"

/*
 * The data structure representing a prepared statement
 *
 * A prepared statement might be fully planned, or only parsed-and-rewritten.
 * If fully planned, stmt_list contains PlannedStmts and/or utility statements;
 * if not, it contains Query nodes.
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
	List	   *stmt_list;		/* list of statement or Query nodes */
	List	   *argtype_list;	/* list of parameter type OIDs */
	bool		fully_planned;	/* what is in stmt_list, exactly? */
	bool		from_sql;		/* prepared via SQL, not FE/BE protocol? */
	TimestampTz prepare_time;	/* the time when the stmt was prepared */
	MemoryContext context;		/* context containing this query */
} PreparedStatement;


/* Utility statements PREPARE, EXECUTE, DEALLOCATE, EXPLAIN EXECUTE */
extern void PrepareQuery(PrepareStmt *stmt);
extern void ExecuteQuery(ExecuteStmt *stmt, ParamListInfo params,
			 DestReceiver *dest, char *completionTag);
extern void DeallocateQuery(DeallocateStmt *stmt);
extern void ExplainExecuteQuery(ExplainStmt *stmt, ParamListInfo params,
					TupOutputState *tstate);

/* Low-level access to stored prepared statements */
extern void StorePreparedStatement(const char *stmt_name,
					   const char *query_string,
					   const char *commandTag,
					   List *stmt_list,
					   List *argtype_list,
					   bool fully_planned,
					   bool from_sql);
extern PreparedStatement *FetchPreparedStatement(const char *stmt_name,
					   bool throwError);
extern void DropPreparedStatement(const char *stmt_name, bool showError);
extern List *FetchPreparedStatementParams(const char *stmt_name);
extern TupleDesc FetchPreparedStatementResultDesc(PreparedStatement *stmt);
extern bool PreparedStatementReturnsTuples(PreparedStatement *stmt);
extern List *FetchPreparedStatementTargetList(PreparedStatement *stmt);

#endif   /* PREPARE_H */
