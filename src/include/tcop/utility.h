/*-------------------------------------------------------------------------
 *
 * utility.h
 *	  prototypes for utility.c.
 *
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
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

extern bool UtilityReturnsTuples(Node *parsetree);

extern TupleDesc UtilityTupleDescriptor(Node *parsetree);

extern Query *UtilityContainsQuery(Node *parsetree);

extern const char *CreateCommandTag(Node *parsetree);

extern LogStmtLevel GetCommandLogLevel(Node *parsetree);

extern bool CommandIsReadOnly(PlannedStmt *pstmt);

#endif							/* UTILITY_H */
