/*-------------------------------------------------------------------------
 *
 * relcache.h--
 *    Relation descriptor cache definitions.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: relcache.h,v 1.2 1996/10/31 09:51:30 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef	RELCACHE_H
#define RELCACHE_H

#include <sys/types.h>

#include "utils/rel.h"

/*
 * relation lookup routines
 */
extern Relation RelationIdCacheGetRelation(Oid relationId);
extern Relation RelationNameCacheGetRelation(char *relationName);
extern Relation RelationIdGetRelation(Oid relationId);
extern Relation RelationNameGetRelation(char *relationName);
extern Relation getreldesc(char *relationName);

extern void RelationClose(Relation relation);
extern void RelationFlushRelation(Relation *relationPtr,
				  bool	onlyFlushReferenceCountZero);
extern void RelationIdInvalidateRelationCacheByRelationId(Oid relationId);

extern void 
RelationIdInvalidateRelationCacheByAccessMethodId(Oid accessMethodId);

extern void RelationCacheInvalidate(bool onlyFlushReferenceCountZero);

extern void RelationRegisterRelation(Relation relation);
extern void RelationPurgeLocalRelation(bool xactComitted);
extern void RelationInitialize();
extern void init_irels();
extern void write_irels();


#endif	/* RELCACHE_H */
