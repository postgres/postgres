/*-------------------------------------------------------------------------
 *
 * utility.h
 *	  prototypes for utility.c.
 *
 *
 * Portions Copyright (c) 1996-2004, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/tcop/utility.h,v 1.25 2004/09/13 20:08:08 tgl Exp $
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
