/*-------------------------------------------------------------------------
 *
 * pquery.h
 *	  prototypes for pquery.c.
 *
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: pquery.h,v 1.28 2003/08/04 00:43:32 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PQUERY_H
#define PQUERY_H

#include "utils/portal.h"


extern void ProcessQuery(Query *parsetree,
			 Plan *plan,
			 ParamListInfo params,
			 DestReceiver *dest,
			 char *completionTag);

extern PortalStrategy ChoosePortalStrategy(List *parseTrees);

extern void PortalStart(Portal portal, ParamListInfo params);

extern void PortalSetResultFormat(Portal portal, int nFormats,
					  int16 *formats);

extern bool PortalRun(Portal portal, long count,
		  DestReceiver *dest, DestReceiver *altdest,
		  char *completionTag);

extern long PortalRunFetch(Portal portal,
			   FetchDirection fdirection,
			   long count,
			   DestReceiver *dest);

#endif   /* PQUERY_H */
