/*-------------------------------------------------------------------------
 *
 * pquery.h--
 *    prototypes for pquery.c.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: pquery.h,v 1.1 1996/08/28 07:27:51 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PQUERY_H
#define PQUERY_H

#include "executor/execdesc.h"
#include "tcop/dest.h"

/* moved to execdesc.h 
extern QueryDesc *CreateQueryDesc(Query *parsetree, Plan *plantree,
				  CommandDest dest);

*/
extern EState *CreateExecutorState();


extern void ProcessPortal(char *portalName, Query *parseTree,
			  Plan *plan, EState *state, TupleDesc attinfo, 
			  CommandDest dest);

extern void ProcessQueryDesc(QueryDesc *queryDesc);

extern void ProcessQuery(Query *parsetree, Plan *plan, char *argv[], 
			 Oid *typev, int nargs, CommandDest dest);

#endif /* pqueryIncluded */
