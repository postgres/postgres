/*-------------------------------------------------------------------------
 *
 * utility.h
 *	  prototypes for utility.c.
 *
 *
 * Portions Copyright (c) 1996-2010, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/tcop/utility.h,v 1.40 2010/02/26 02:01:28 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef UTILITY_H
#define UTILITY_H

#include "tcop/tcopprot.h"


/* Hook for plugins to get control in ProcessUtility() */
typedef void (*ProcessUtility_hook_type) (Node *parsetree,
			  const char *queryString, ParamListInfo params, bool isTopLevel,
									DestReceiver *dest, char *completionTag);
extern PGDLLIMPORT ProcessUtility_hook_type ProcessUtility_hook;

extern void ProcessUtility(Node *parsetree, const char *queryString,
			   ParamListInfo params, bool isTopLevel,
			   DestReceiver *dest, char *completionTag);
extern void standard_ProcessUtility(Node *parsetree, const char *queryString,
						ParamListInfo params, bool isTopLevel,
						DestReceiver *dest, char *completionTag);

extern bool UtilityReturnsTuples(Node *parsetree);

extern TupleDesc UtilityTupleDescriptor(Node *parsetree);

extern const char *CreateCommandTag(Node *parsetree);

extern LogStmtLevel GetCommandLogLevel(Node *parsetree);

extern bool CommandIsReadOnly(Node *parsetree);

extern void CheckRelationOwnership(Oid relOid, bool noCatalogs);

#endif   /* UTILITY_H */
