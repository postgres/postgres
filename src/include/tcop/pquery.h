/*-------------------------------------------------------------------------
 *
 * pquery.h
 *	  prototypes for pquery.c.
 *
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: pquery.h,v 1.16 2001/01/24 19:43:28 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PQUERY_H
#define PQUERY_H

#include "executor/execdesc.h"
#include "utils/portal.h"


extern void ProcessQuery(Query *parsetree, Plan *plan, CommandDest dest);

extern EState *CreateExecutorState(void);

extern Portal PreparePortal(char *portalName);

#endif	 /* PQUERY_H */
