/*-------------------------------------------------------------------------
 *
 * relcache.h
 *	  Relation descriptor cache definitions.
 *
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: relcache.h,v 1.22 2000/10/23 04:10:24 vadim Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef RELCACHE_H
#define RELCACHE_H

#include "utils/rel.h"

/*
 * relation lookup routines
 */
extern Relation RelationIdGetRelation(Oid relationId);
extern Relation RelationNameGetRelation(const char *relationName);
extern Relation RelationNodeCacheGetRelation(RelFileNode rnode);

/* finds an existing cache entry, but won't make a new one */
extern Relation RelationIdCacheGetRelation(Oid relationId);

extern void RelationClose(Relation relation);
extern void RelationForgetRelation(Oid rid);

/*
 * Routines to compute/retrieve additional cached information
 */
extern List *RelationGetIndexList(Relation relation);

/*
 * Routines for backend startup
 */
extern void RelationCacheInitialize(void);
extern void RelationCacheInitializePhase2(void);

/*
 * Routines for flushing/rebuilding relcache entries in various scenarios
 */
extern void RelationIdInvalidateRelationCacheByRelationId(Oid relationId);

extern void RelationCacheInvalidate(void);

extern void RelationRegisterRelation(Relation relation);
extern void RelationPurgeLocalRelation(bool xactComitted);

extern void RelationCacheAbort(void);

/*
 * both vacuum.c and relcache.c need to know the name of the relcache init file
 */

#define RELCACHE_INIT_FILENAME	"pg_internal.init"

#endif	 /* RELCACHE_H */
