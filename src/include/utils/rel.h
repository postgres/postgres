/*-------------------------------------------------------------------------
 *
 * rel.h--
 *	  POSTGRES relation descriptor definitions.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: rel.h,v 1.21 1999/02/02 03:45:28 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef REL_H
#define REL_H

#include <catalog/pg_am.h>
#include <catalog/pg_class.h>
#include <access/strat.h>
#include <access/tupdesc.h>
#include <rewrite/prs2lock.h>
#include <storage/fd.h>

typedef struct Trigger
{
	char	   *tgname;
	Oid			tgfoid;
	FmgrInfo	tgfunc;
	int16		tgtype;
	int16		tgnargs;
	int16		tgattr[8];
	char	  **tgargs;
} Trigger;

typedef struct TriggerDesc
{
	uint16		n_before_statement[4];
	uint16		n_before_row[4];
	uint16		n_after_row[4];
	uint16		n_after_statement[4];
	Trigger   **tg_before_statement[4];
	Trigger   **tg_before_row[4];
	Trigger   **tg_after_row[4];
	Trigger   **tg_after_statement[4];
	Trigger    *triggers;
} TriggerDesc;

typedef struct RelationData
{
	File		rd_fd;			/* open file descriptor */
	int			rd_nblocks;		/* number of blocks in rel */
	uint16		rd_refcnt;		/* reference count */
	bool		rd_myxactonly;	/* uses the local buffer mgr */
	bool		rd_isnailed;	/* rel is nailed in cache */
	bool		rd_isnoname;	/* rel has no name */
	bool		rd_nonameunlinked; /* noname rel already unlinked */
	Form_pg_am	rd_am;			/* AM tuple */
	Form_pg_class rd_rel;		/* RELATION tuple */
	Oid			rd_id;			/* relations's object id */
	Pointer		lockInfo;		/* ptr. to misc. info. */
	TupleDesc	rd_att;			/* tuple desciptor */
	RuleLock   *rd_rules;		/* rewrite rules */
	IndexStrategy rd_istrat;
	RegProcedure *rd_support;
	TriggerDesc *trigdesc;
} RelationData;

typedef RelationData *Relation;

/* ----------------
 *		RelationPtr is used in the executor to support index scans
 *		where we have to keep track of several index relations in an
 *		array.	-cim 9/10/89
 * ----------------
 */
typedef Relation *RelationPtr;

#define InvalidRelation ((Relation)NULL)

/*
 * RelationIsValid --
 *		True iff relation descriptor is valid.
 */
#define RelationIsValid(relation) PointerIsValid(relation)

/*
 * RelationGetSystemPort --
 *		Returns system port of a relation.
 *
 * Note:
 *		Assumes relation descriptor is valid.
 */
#define RelationGetSystemPort(relation) ((relation)->rd_fd)

/*
 * RelationGetLockInfo --
 *		Returns the lock information structure in the reldesc
 *
 */
#define RelationGetLockInfo(relation) ((relation)->lockInfo)

/*
 * RelationHasReferenceCountZero --
 *		True iff relation reference count is zero.
 *
 * Note:
 *		Assumes relation descriptor is valid.
 */
#define RelationHasReferenceCountZero(relation) \
		((bool)((relation)->rd_refcnt == 0))

/*
 * RelationSetReferenceCount --
 *		Sets relation reference count.
 */
#define RelationSetReferenceCount(relation,count) ((relation)->rd_refcnt = count)

/*
 * RelationIncrementReferenceCount --
 *		Increments relation reference count.
 */
#define RelationIncrementReferenceCount(relation) ((relation)->rd_refcnt += 1);

/*
 * RelationDecrementReferenceCount --
 *		Decrements relation reference count.
 */
#define RelationDecrementReferenceCount(relation) ((relation)->rd_refcnt -= 1)

/*
 * RelationGetForm --
 *		Returns relation attribute values for a relation.
 *
 * Note:
 *		Assumes relation descriptor is valid.
 */
#define RelationGetForm(relation) ((relation)->rd_rel)


/*
 * RelationGetRelid --
 *
 *	returns the object id of the relation
 *
 */
#define RelationGetRelid(relation) ((relation)->rd_id)

/*
 * RelationGetFile --
 *
 *	  Returns the open File decscriptor
 */
#define RelationGetFile(relation) ((relation)->rd_fd)


/*
 * RelationGetRelationName --
 *
 *	  Returns a Relation Name
 */
#define RelationGetRelationName(relation) (&(relation)->rd_rel->relname)

/*
 * RelationGetRelationName --
 *
 *	  Returns a the number of attributes.
 */
#define RelationGetNumberOfAttributes(relation) ((relation)->rd_rel->relnatts)

/*
 * RelationGetDescr --
 *		Returns tuple descriptor for a relation.
 *
 * Note:
 *		Assumes relation descriptor is valid.
 */
#define RelationGetDescr(relation) ((relation)->rd_att)

extern IndexStrategy RelationGetIndexStrategy(Relation relation);

extern void RelationSetIndexSupport(Relation relation, IndexStrategy strategy,
						RegProcedure *support);

#endif	 /* REL_H */
