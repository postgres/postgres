/*-------------------------------------------------------------------------
 *
 * pquery.h--
 *	  prototypes for pquery.c.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: pquery.h,v 1.9 1998/02/26 04:43:40 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PQUERY_H
#define PQUERY_H

#include <executor/execdesc.h>

/* moved to execdesc.h
extern QueryDesc *CreateQueryDesc(Query *parsetree, Plan *plantree,
								  CommandDest dest);

*/
extern EState *CreateExecutorState(void);


extern void
ProcessPortal(char *portalName, Query *parseTree,
			  Plan *plan, EState *state, TupleDesc attinfo,
			  CommandDest dest);

extern void
ProcessQuery(Query *parsetree, Plan *plan, char *argv[],
			 Oid *typev, int nargs, CommandDest dest);

#endif							/* pqueryIncluded */
