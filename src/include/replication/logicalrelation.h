/*-------------------------------------------------------------------------
 *
 * logicalrelation.h
 *	  Relation definitions for logical replication relation mapping.
 *
 * Portions Copyright (c) 2016-2019, PostgreSQL Global Development Group
 *
 * src/include/replication/logicalrelation.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef LOGICALRELATION_H
#define LOGICALRELATION_H

#include "replication/logicalproto.h"

typedef struct LogicalRepRelMapEntry
{
	LogicalRepRelation remoterel;	/* key is remoterel.remoteid */

	/* Mapping to local relation, filled as needed. */
	Oid			localreloid;	/* local relation id */
	Relation	localrel;		/* relcache entry */
	AttrNumber *attrmap;		/* map of local attributes to remote ones */
	bool		updatable;		/* Can apply updates/deletes? */

	/* Sync state. */
	char		state;
	XLogRecPtr	statelsn;
} LogicalRepRelMapEntry;

extern void logicalrep_relmap_update(LogicalRepRelation *remoterel);

extern LogicalRepRelMapEntry *logicalrep_rel_open(LogicalRepRelId remoteid,
												  LOCKMODE lockmode);
extern void logicalrep_rel_close(LogicalRepRelMapEntry *rel,
								 LOCKMODE lockmode);

extern void logicalrep_typmap_update(LogicalRepTyp *remotetyp);
extern char *logicalrep_typmap_gettypname(Oid remoteid);

#endif							/* LOGICALRELATION_H */
