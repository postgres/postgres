/*-------------------------------------------------------------------------
 *
 * relcache.h
 *	  Relation descriptor cache definitions.
 *
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: relcache.h,v 1.37 2003/09/24 18:54:02 tgl Exp $
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
extern Relation RelationSysNameGetRelation(const char *relationName);

/* finds an existing cache entry, but won't make a new one */
extern Relation RelationIdCacheGetRelation(Oid relationId);
extern Relation RelationNodeCacheGetRelation(RelFileNode rnode);

extern void RelationClose(Relation relation);

/*
 * Routines to compute/retrieve additional cached information
 */
extern List *RelationGetIndexList(Relation relation);
extern List *RelationGetIndexExpressions(Relation relation);
extern List *RelationGetIndexPredicate(Relation relation);

extern void RelationSetIndexList(Relation relation, List *indexIds);

extern void RelationInitIndexAccessInfo(Relation relation);

/*
 * Routines for backend startup
 */
extern void RelationCacheInitialize(void);
extern void RelationCacheInitializePhase2(void);
extern void RelationCacheInitializePhase3(void);

/*
 * Routine to create a relcache entry for an about-to-be-created relation
 */
extern Relation RelationBuildLocalRelation(const char *relname,
						   Oid relnamespace,
						   TupleDesc tupDesc,
						   Oid relid, Oid dbid,
						   RelFileNode rnode,
						   bool nailit);

/*
 * Routines for flushing/rebuilding relcache entries in various scenarios
 */
extern void RelationForgetRelation(Oid rid);

extern void RelationIdInvalidateRelationCacheByRelationId(Oid relationId);

extern void RelationCacheInvalidate(void);

extern void AtEOXact_RelationCache(bool commit);

/*
 * Routines to help manage rebuilding of relcache init file
 */
extern bool RelationIdIsInInitFile(Oid relationId);
extern void RelationCacheInitFileInvalidate(bool beforeSend);

/* XLOG support */
extern void CreateDummyCaches(void);
extern void DestroyDummyCaches(void);


/* should be used only by relcache.c and catcache.c */
extern bool criticalRelcachesBuilt;

#endif   /* RELCACHE_H */
