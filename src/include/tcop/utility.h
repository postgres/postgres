/*-------------------------------------------------------------------------
 *
 * utility.h
 *	  prototypes for utility.c.
 *
 *
 * Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/tcop/utility.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef UTILITY_H
#define UTILITY_H

#include "tcop/tcopprot.h"

typedef enum
{
	PROCESS_UTILITY_TOPLEVEL,	/* toplevel interactive command */
	PROCESS_UTILITY_QUERY,		/* a complete query, but not toplevel */
	PROCESS_UTILITY_QUERY_NONATOMIC,	/* a complete query, nonatomic
										 * execution context */
	PROCESS_UTILITY_SUBCOMMAND	/* a portion of a query */
} ProcessUtilityContext;

/* Info needed when recursing from ALTER TABLE */
typedef struct AlterTableUtilityContext
{
	PlannedStmt *pstmt;			/* PlannedStmt for outer ALTER TABLE command */
	const char *queryString;	/* its query string */
	Oid			relid;			/* OID of ALTER's target table */
	ParamListInfo params;		/* any parameters available to ALTER TABLE */
	QueryEnvironment *queryEnv; /* execution environment for ALTER TABLE */
} AlterTableUtilityContext;

/* Hook for plugins to get control in ProcessUtility() */
typedef void (*ProcessUtility_hook_type) (PlannedStmt *pstmt,
										  const char *queryString, ProcessUtilityContext context,
										  ParamListInfo params,
										  QueryEnvironment *queryEnv,
										  DestReceiver *dest, char *completionTag);
extern PGDLLIMPORT ProcessUtility_hook_type ProcessUtility_hook;

extern void ProcessUtility(PlannedStmt *pstmt, const char *queryString,
						   ProcessUtilityContext context, ParamListInfo params,
						   QueryEnvironment *queryEnv,
						   DestReceiver *dest, char *completionTag);
extern void standard_ProcessUtility(PlannedStmt *pstmt, const char *queryString,
									ProcessUtilityContext context, ParamListInfo params,
									QueryEnvironment *queryEnv,
									DestReceiver *dest, char *completionTag);

extern void ProcessUtilityForAlterTable(Node *stmt,
										AlterTableUtilityContext *context);

extern bool UtilityReturnsTuples(Node *parsetree);

extern TupleDesc UtilityTupleDescriptor(Node *parsetree);

extern Query *UtilityContainsQuery(Node *parsetree);

extern const char *CreateCommandTag(Node *parsetree);

extern LogStmtLevel GetCommandLogLevel(Node *parsetree);

extern bool CommandIsReadOnly(PlannedStmt *pstmt);

#endif							/* UTILITY_H */
