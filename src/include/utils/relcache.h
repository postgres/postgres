/*-------------------------------------------------------------------------
 *
 * relcache.h
 *	  Relation descriptor cache definitions.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: relcache.h,v 1.13 1999/07/15 23:04:23 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef RELCACHE_H
#define RELCACHE_H

#include "utils/rel.h"

/*
 * relation lookup routines
 */
extern Relation RelationIdCacheGetRelation(Oid relationId);
extern Relation RelationIdGetRelation(Oid relationId);
extern Relation RelationNameGetRelation(char *relationName);

extern void RelationClose(Relation relation);
extern void RelationForgetRelation(Oid rid);
extern void RelationIdInvalidateRelationCacheByRelationId(Oid relationId);

extern void RelationIdInvalidateRelationCacheByAccessMethodId(Oid accessMethodId);

extern void RelationCacheInvalidate(bool onlyFlushReferenceCountZero);

extern void RelationRegisterRelation(Relation relation);
extern void RelationPurgeLocalRelation(bool xactComitted);
extern void RelationInitialize(void);

/*
 * both vacuum.c and relcache.c need to know the name of the relcache init file
 */

#define RELCACHE_INIT_FILENAME	"pg_internal.init"

#endif	 /* RELCACHE_H */
