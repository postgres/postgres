/*-------------------------------------------------------------------------
 *
 * pquery.h
 *	  prototypes for pquery.c.
 *
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: pquery.h,v 1.15 2000/06/28 03:33:28 tgl Exp $
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
