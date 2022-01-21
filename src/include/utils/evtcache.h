/*-------------------------------------------------------------------------
 *
 * evtcache.h
 *	  Special-purpose cache for event trigger data.
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/include/utils/evtcache.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef EVTCACHE_H
#define EVTCACHE_H

#include "nodes/bitmapset.h"
#include "nodes/pg_list.h"

typedef enum
{
	EVT_DDLCommandStart,
	EVT_DDLCommandEnd,
	EVT_SQLDrop,
	EVT_TableRewrite
} EventTriggerEvent;

typedef struct
{
	Oid			fnoid;			/* function to be called */
	char		enabled;		/* as SESSION_REPLICATION_ROLE_* */
	Bitmapset  *tagset;			/* command tags, or NULL if empty */
} EventTriggerCacheItem;

extern List *EventCacheLookup(EventTriggerEvent event);

#endif							/* EVTCACHE_H */
