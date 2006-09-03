/*-------------------------------------------------------------------------
 *
 * pquery.h
 *	  prototypes for pquery.c.
 *
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/tcop/pquery.h,v 1.39 2006/09/03 03:19:45 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PQUERY_H
#define PQUERY_H

#include "utils/portal.h"


extern DLLIMPORT Portal ActivePortal;


extern PortalStrategy ChoosePortalStrategy(List *parseTrees);

extern List *FetchPortalTargetList(Portal portal);

extern void PortalStart(Portal portal, ParamListInfo params,
			Snapshot snapshot);

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
