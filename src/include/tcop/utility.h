/*-------------------------------------------------------------------------
 *
 * utility.h
 *	  prototypes for utility.c.
 *
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/tcop/utility.h,v 1.26 2004/12/31 22:03:44 pgsql Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef UTILITY_H
#define UTILITY_H

#include "executor/execdesc.h"


extern void ProcessUtility(Node *parsetree, ParamListInfo params,
			   DestReceiver *dest, char *completionTag);

extern bool UtilityReturnsTuples(Node *parsetree);

extern TupleDesc UtilityTupleDescriptor(Node *parsetree);

extern const char *CreateCommandTag(Node *parsetree);

extern const char *CreateQueryTag(Query *parsetree);

extern bool QueryIsReadOnly(Query *parsetree);

extern void CheckRelationOwnership(RangeVar *rel, bool noCatalogs);

#endif   /* UTILITY_H */
