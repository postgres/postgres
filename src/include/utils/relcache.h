/*-------------------------------------------------------------------------
 *
 * relcache.h--
 *	  Relation descriptor cache definitions.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: relcache.h,v 1.7 1997/09/07 05:03:00 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef RELCACHE_H
#define RELCACHE_H

#include <utils/rel.h>

/*
 * relation lookup routines
 */
extern Relation RelationIdCacheGetRelation(Oid relationId);
extern Relation RelationIdGetRelation(Oid relationId);
extern Relation RelationNameGetRelation(char *relationName);

extern void		RelationClose(Relation relation);
extern void		RelationForgetRelation(Oid rid);
extern void		RelationIdInvalidateRelationCacheByRelationId(Oid relationId);

extern void
				RelationIdInvalidateRelationCacheByAccessMethodId(Oid accessMethodId);

extern void		RelationCacheInvalidate(bool onlyFlushReferenceCountZero);

extern void		RelationRegisterRelation(Relation relation);
extern void		RelationPurgeLocalRelation(bool xactComitted);
extern void		RelationInitialize(void);

#endif							/* RELCACHE_H */
