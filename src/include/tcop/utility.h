/*-------------------------------------------------------------------------
 *
 * utility.h
 *	  prototypes for utility.c.
 *
 *
 * Portions Copyright (c) 1996-2004, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/tcop/utility.h,v 1.24 2004/08/29 05:06:58 momjian Exp $
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

extern void CheckRelationOwnership(RangeVar *rel, bool noCatalogs);

#endif   /* UTILITY_H */
