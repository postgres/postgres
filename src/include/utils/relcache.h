/*-------------------------------------------------------------------------
 *
 * relcache.h
 *	  Relation descriptor cache definitions.
 *
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: relcache.h,v 1.24 2001/01/24 19:43:29 momjian Exp $
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


extern void CreateDummyCaches(void);
extern void DestroyDummyCaches(void);

/*
 * both vacuum.c and relcache.c need to know the name of the relcache init file
 */

#define RELCACHE_INIT_FILENAME	"pg_internal.init"

#endif	 /* RELCACHE_H */
