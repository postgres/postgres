/*-------------------------------------------------------------------------
 *
 * rel.h
 *	  POSTGRES relation descriptor (a/k/a relcache entry) definitions.
 *
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: rel.h,v 1.45 2001/03/22 04:01:14 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef REL_H
#define REL_H

#include "access/strat.h"
#include "access/tupdesc.h"
#include "catalog/pg_am.h"
#include "catalog/pg_class.h"
#include "rewrite/prs2lock.h"
#include "storage/relfilenode.h"
#include "storage/fd.h"

/* added to prevent circular dependency.  bjm 1999/11/15 */
extern char *get_temp_rel_by_physicalname(const char *relname);

/*
 * LockRelId and LockInfo really belong to lmgr.h, but it's more convenient
 * to declare them here so we can have a LockInfoData field in a Relation.
 */

typedef struct LockRelId
{
	Oid			relId;			/* a relation identifier */
	Oid			dbId;			/* a database identifier */
} LockRelId;

typedef struct LockInfoData
{
	LockRelId	lockRelId;
} LockInfoData;

typedef LockInfoData *LockInfo;

/*
 * Likewise, this struct really belongs to trigger.h, but for convenience
 * we put it here.
 */

typedef struct Trigger
{
	Oid			tgoid;
	char	   *tgname;
	Oid			tgfoid;
	FmgrInfo	tgfunc;
	int16		tgtype;
	bool		tgenabled;
	bool		tgisconstraint;
	bool		tgdeferrable;
	bool		tginitdeferred;
	int16		tgnargs;
	int16		tgattr[FUNC_MAX_ARGS];
	char	  **tgargs;
} Trigger;

typedef struct TriggerDesc
{
	/* index data to identify which triggers are which */
	uint16		n_before_statement[4];
	uint16		n_before_row[4];
	uint16		n_after_row[4];
	uint16		n_after_statement[4];
	Trigger   **tg_before_statement[4];
	Trigger   **tg_before_row[4];
	Trigger   **tg_after_row[4];
	Trigger   **tg_after_statement[4];
	/* the actual array of triggers is here */
	Trigger    *triggers;
	int			numtriggers;
} TriggerDesc;

/*
 * Here are the contents of a relation cache entry.
 */

typedef struct RelationData
{
	File		rd_fd;			/* open file descriptor, or -1 if none */
	RelFileNode rd_node;		/* relation file node */
	int			rd_nblocks;		/* number of blocks in rel */
	uint16		rd_refcnt;		/* reference count */
	bool		rd_myxactonly;	/* rel uses the local buffer mgr */
	bool		rd_isnailed;	/* rel is nailed in cache */
	bool		rd_indexfound;	/* true if rd_indexlist is valid */
	bool		rd_uniqueindex; /* true if rel is a UNIQUE index */
	Form_pg_am	rd_am;			/* AM tuple */
	Form_pg_class rd_rel;		/* RELATION tuple */
	Oid			rd_id;			/* relation's object id */
	List	   *rd_indexlist;	/* list of OIDs of indexes on relation */
	LockInfoData rd_lockInfo;	/* lock mgr's info for locking relation */
	TupleDesc	rd_att;			/* tuple descriptor */
	RuleLock   *rd_rules;		/* rewrite rules */
	MemoryContext rd_rulescxt;	/* private memory cxt for rd_rules, if any */
	IndexStrategy rd_istrat;	/* info needed if rel is an index */
	RegProcedure *rd_support;
	TriggerDesc *trigdesc;		/* Trigger info, or NULL if rel has none */
} RelationData;

typedef RelationData *Relation;


/* ----------------
 *		RelationPtr is used in the executor to support index scans
 *		where we have to keep track of several index relations in an
 *		array.	-cim 9/10/89
 * ----------------
 */
typedef Relation *RelationPtr;


/*
 * RelationIsValid
 *		True iff relation descriptor is valid.
 */
#define RelationIsValid(relation) PointerIsValid(relation)

#define InvalidRelation ((Relation) NULL)

/*
 * RelationHasReferenceCountZero
 *		True iff relation reference count is zero.
 *
 * Note:
 *		Assumes relation descriptor is valid.
 */
#define RelationHasReferenceCountZero(relation) \
		((bool)((relation)->rd_refcnt == 0))

/*
 * RelationSetReferenceCount
 *		Sets relation reference count.
 */
#define RelationSetReferenceCount(relation,count) \
	((relation)->rd_refcnt = (count))

/*
 * RelationIncrementReferenceCount
 *		Increments relation reference count.
 */
#define RelationIncrementReferenceCount(relation) \
	((relation)->rd_refcnt += 1)

/*
 * RelationDecrementReferenceCount
 *		Decrements relation reference count.
 */
#define RelationDecrementReferenceCount(relation) \
	(AssertMacro((relation)->rd_refcnt > 0), \
	 (relation)->rd_refcnt -= 1)

/*
 * RelationGetForm
 *		Returns pg_class tuple for a relation.
 *
 * Note:
 *		Assumes relation descriptor is valid.
 */
#define RelationGetForm(relation) ((relation)->rd_rel)

/*
 * RelationGetRelid
 *
 *	returns the OID of the relation
 */
#define RelationGetRelid(relation) ((relation)->rd_id)

/*
 * RelationGetFile
 *
 *	  Returns the open file descriptor for the rel
 */
#define RelationGetFile(relation) ((relation)->rd_fd)

/*
 * RelationGetRelationName
 *
 *	  Returns the relation's logical name (as seen by the user).
 *
 * If the rel is a temp rel, the temp name will be returned.  Therefore,
 * this name is not unique.  But it is the name to use in heap_openr(),
 * for example.
 */
#define RelationGetRelationName(relation) \
(\
	(strncmp(RelationGetPhysicalRelationName(relation), \
			 "pg_temp.", 8) != 0) \
	? \
		RelationGetPhysicalRelationName(relation) \
	: \
		get_temp_rel_by_physicalname( \
			RelationGetPhysicalRelationName(relation)) \
)


/*
 * RelationGetPhysicalRelationName
 *
 *	  Returns the rel's physical name, ie, the name appearing in pg_class.
 *
 * While this name is unique across all rels in the database, it is not
 * necessarily useful for accessing the rel, since a temp table of the
 * same name might mask the rel.  It is useful mainly for determining if
 * the rel is a shared system rel or not.
 *
 * The macro is rather unfortunately named, since the pg_class name no longer
 * has anything to do with the file name used for physical storage of the rel.
 */
#define RelationGetPhysicalRelationName(relation) \
	(NameStr((relation)->rd_rel->relname))

/*
 * RelationGetNumberOfAttributes
 *
 *	  Returns the number of attributes.
 */
#define RelationGetNumberOfAttributes(relation) ((relation)->rd_rel->relnatts)

/*
 * RelationGetDescr
 *		Returns tuple descriptor for a relation.
 */
#define RelationGetDescr(relation) ((relation)->rd_att)

/*
 * RelationGetIndexStrategy
 *		Returns index strategy for a relation.
 *
 * Note:
 *		Assumes relation descriptor is valid.
 *		Assumes relation descriptor is for an index relation.
 */
#define RelationGetIndexStrategy(relation) ((relation)->rd_istrat)

/*
 * Routines in utils/cache/rel.c
 */
extern void RelationSetIndexSupport(Relation relation,
						IndexStrategy strategy,
						RegProcedure *support);

#endif	 /* REL_H */
