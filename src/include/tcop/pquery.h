/*-------------------------------------------------------------------------
 *
 * pquery.h
 *	  prototypes for pquery.c.
 *
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: pquery.h,v 1.20 2002/02/26 22:47:12 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PQUERY_H
#define PQUERY_H

#include "executor/execdesc.h"
#include "utils/portal.h"


extern void ProcessQuery(Query *parsetree, Plan *plan, CommandDest dest,
						 char *completionTag);

extern EState *CreateExecutorState(void);

extern Portal PreparePortal(char *portalName);

#endif   /* PQUERY_H */
