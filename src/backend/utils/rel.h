/*-------------------------------------------------------------------------
 *
 * rel.h--
 *    POSTGRES relation descriptor definitions.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: rel.h,v 1.1.1.1.2.1 1996/10/24 07:38:08 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef	REL_H
#define REL_H

#include "postgres.h"

#include "storage/fd.h"
#include "access/strat.h"	
#include "access/tupdesc.h"

#include "catalog/pg_am.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_class.h"

#include "rewrite/prs2lock.h"

typedef struct RelationData {
    File		rd_fd; 		/* open file descriptor */
    int                 rd_nblocks;	/* number of blocks in rel */
    uint16		rd_refcnt; 	/* reference count */
    bool		rd_islocal; 	/* uses the local buffer mgr */
    bool		rd_isnailed; 	/* rel is nailed in cache */
    bool		rd_istemp;	/* rel is a temp rel */
    bool		rd_tmpunlinked;	/* temp rel already unlinked */
    Form_pg_am 		rd_am; 		/* AM tuple */
    Form_pg_class	rd_rel;		/* RELATION tuple */
    Oid			rd_id;		/* relations's object id */
    Pointer		lockInfo; 	/* ptr. to misc. info. */
    TupleDesc           rd_att;		/* tuple desciptor */
    RuleLock		*rd_rules;	/* rewrite rules */
    IndexStrategy       rd_istrat;    
    RegProcedure*       rd_support;
} RelationData;

typedef RelationData	*Relation;

/* ----------------
 *	RelationPtr is used in the executor to support index scans
 *	where we have to keep track of several index relations in an
 *	array.  -cim 9/10/89
 * ----------------
 */
typedef Relation	*RelationPtr;

#define InvalidRelation	((Relation)NULL)

typedef char	ArchiveMode;

/*
 * RelationIsValid --
 *	True iff relation descriptor is valid.
 */
#define	RelationIsValid(relation) PointerIsValid(relation)

/*
 * RelationGetSystemPort --
 *	Returns system port of a relation.
 *
 * Note:
 *	Assumes relation descriptor is valid.
 */
#define RelationGetSystemPort(relation) ((relation)->rd_fd)

/*
 * RelationGetLockInfo --
 *      Returns the lock information structure in the reldesc
 *
 */
#define RelationGetLockInfo(relation) ((relation)->lockInfo)

/*
 * RelationHasReferenceCountZero --
 *	True iff relation reference count is zero.
 *
 * Note:
 *	Assumes relation descriptor is valid.
 */
#define RelationHasReferenceCountZero(relation) \
	((bool)((relation)->rd_refcnt == 0))

/*
 * RelationSetReferenceCount --
 *	Sets relation reference count.
 */
#define RelationSetReferenceCount(relation,count) ((relation)->rd_refcnt = count)

/*
 * RelationIncrementReferenceCount --
 *	Increments relation reference count.
 */
#define RelationIncrementReferenceCount(relation) ((relation)->rd_refcnt += 1);

/*
 * RelationDecrementReferenceCount --
 *	Decrements relation reference count.
 */
#define RelationDecrementReferenceCount(relation) ((relation)->rd_refcnt -= 1)

/*
 * RelationGetAccessMethodTupleForm --
 *	Returns access method attribute values for a relation.
 *
 * Note:
 *	Assumes relation descriptor is valid.
 */
#define RelationGetAccessMethodTupleForm(relation) ((relation)->rd_am)

/*
 * RelationGetRelationTupleForm --
 *	Returns relation attribute values for a relation.
 *
 * Note:
 *	Assumes relation descriptor is valid.
 */
#define RelationGetRelationTupleForm(relation) ((relation)->rd_rel)


/* 
 * RelationGetRelationId --
 *
 *  returns the object id of the relation
 *
 */
#define RelationGetRelationId(relation) ((relation)->rd_id)

/*
 * RelationGetFile --
 *
 *    Returns the open File decscriptor
 */
#define RelationGetFile(relation) ((relation)->rd_fd)


/*
 * RelationGetRelationName --
 *
 *    Returns a Relation Name
 */
#define RelationGetRelationName(relation) (&(relation)->rd_rel->relname)

/*
 * RelationGetRelationName --
 *
 *    Returns a the number of attributes.
 */
#define RelationGetNumberOfAttributes(relation) ((relation)->rd_rel->relnatts)

/*
 * RelationGetTupleDescriptor --
 *	Returns tuple descriptor for a relation.
 *
 * Note:
 *	Assumes relation descriptor is valid.
 */
#define RelationGetTupleDescriptor(relation) ((relation)->rd_att)

extern IndexStrategy RelationGetIndexStrategy(Relation relation);

extern void RelationSetIndexSupport(Relation relation, IndexStrategy strategy,
			     RegProcedure *support);
#endif	/* REL_H */
