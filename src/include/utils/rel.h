/*-------------------------------------------------------------------------
 *
 * rel.h
 *	  POSTGRES relation descriptor (a/k/a relcache entry) definitions.
 *
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: rel.h,v 1.68 2003/09/24 18:54:01 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef REL_H
#define REL_H

#include "access/strat.h"
#include "access/tupdesc.h"
#include "catalog/pg_am.h"
#include "catalog/pg_class.h"
#include "catalog/pg_index.h"
#include "rewrite/prs2lock.h"
#include "storage/block.h"
#include "storage/fd.h"
#include "storage/relfilenode.h"


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
	Oid			tgoid;			/* OID of trigger (pg_trigger row) */
	/* Remaining fields are copied from pg_trigger, see pg_trigger.h */
	char	   *tgname;
	Oid			tgfoid;
	int16		tgtype;
	bool		tgenabled;
	bool		tgisconstraint;
	Oid			tgconstrrelid;
	bool		tgdeferrable;
	bool		tginitdeferred;
	int16		tgnargs;
	int16		tgattr[FUNC_MAX_ARGS];
	char	  **tgargs;
} Trigger;

typedef struct TriggerDesc
{
	/*
	 * Index data to identify which triggers are which.  Since each
	 * trigger can appear in more than one class, for each class we
	 * provide a list of integer indexes into the triggers array.
	 */
#define TRIGGER_NUM_EVENT_CLASSES  3

	uint16		n_before_statement[TRIGGER_NUM_EVENT_CLASSES];
	uint16		n_before_row[TRIGGER_NUM_EVENT_CLASSES];
	uint16		n_after_row[TRIGGER_NUM_EVENT_CLASSES];
	uint16		n_after_statement[TRIGGER_NUM_EVENT_CLASSES];
	int		   *tg_before_statement[TRIGGER_NUM_EVENT_CLASSES];
	int		   *tg_before_row[TRIGGER_NUM_EVENT_CLASSES];
	int		   *tg_after_row[TRIGGER_NUM_EVENT_CLASSES];
	int		   *tg_after_statement[TRIGGER_NUM_EVENT_CLASSES];

	/* The actual array of triggers is here */
	Trigger    *triggers;
	int			numtriggers;
} TriggerDesc;


/* ----------
 * Same for the statistics collector data in Relation and scan data.
 * ----------
 */
typedef struct PgStat_Info
{
	void	   *tabentry;
	bool		no_stats;
	bool		heap_scan_counted;
	bool		index_scan_counted;
} PgStat_Info;

/*
 * Here are the contents of a relation cache entry.
 */

typedef struct RelationData
{
	File		rd_fd;			/* open file descriptor, or -1 if none */
	RelFileNode rd_node;		/* file node (physical identifier) */
	BlockNumber rd_nblocks;		/* number of blocks in rel */
	BlockNumber rd_targblock;	/* current insertion target block, or
								 * InvalidBlockNumber */
	int			rd_refcnt;		/* reference count */
	bool		rd_isnew;		/* rel was created in current xact */

	/*
	 * NOTE: rd_isnew should be relied on only for optimization purposes;
	 * it is possible for new-ness to be "forgotten" (eg, after CLUSTER).
	 */
	bool		rd_istemp;		/* rel uses the local buffer mgr */
	char		rd_isnailed;	/* rel is nailed in cache: 0 = no, 1 = yes,
								 * 2 = yes but possibly invalid */
	char		rd_indexvalid;	/* state of rd_indexlist: 0 = not valid,
								 * 1 = valid, 2 = temporarily forced */
	Form_pg_class rd_rel;		/* RELATION tuple */
	TupleDesc	rd_att;			/* tuple descriptor */
	Oid			rd_id;			/* relation's object id */
	List	   *rd_indexlist;	/* list of OIDs of indexes on relation */
	LockInfoData rd_lockInfo;	/* lock mgr's info for locking relation */
	RuleLock   *rd_rules;		/* rewrite rules */
	MemoryContext rd_rulescxt;	/* private memory cxt for rd_rules, if any */
	TriggerDesc *trigdesc;		/* Trigger info, or NULL if rel has none */

	/* These are non-NULL only for an index relation: */
	Form_pg_index rd_index;		/* pg_index tuple describing this index */
	struct HeapTupleData *rd_indextuple;		/* all of pg_index tuple */
	/* "struct HeapTupleData *" avoids need to include htup.h here	*/
	Form_pg_am	rd_am;			/* pg_am tuple for index's AM */

	/* index access support info (used only for an index relation) */
	MemoryContext rd_indexcxt;	/* private memory cxt for this stuff */
	IndexStrategy rd_istrat;	/* operator strategy map */
	Oid		   *rd_operator;	/* OIDs of index operators */
	RegProcedure *rd_support;	/* OIDs of support procedures */
	struct FmgrInfo *rd_supportinfo;	/* lookup info for support
										 * procedures */
	/* "struct FmgrInfo" avoids need to include fmgr.h here */
	List	   *rd_indexprs;	/* index expression trees, if any */
	List	   *rd_indpred;		/* index predicate tree, if any */

	/* statistics collection area */
	PgStat_Info pgstat_info;
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
 * RelationGetRelationName
 *
 *	  Returns the rel's name.
 *
 * Note that the name is only unique within the containing namespace.
 */
#define RelationGetRelationName(relation) \
	(NameStr((relation)->rd_rel->relname))

/*
 * RelationGetNamespace
 *
 *	  Returns the rel's namespace OID.
 */
#define RelationGetNamespace(relation) \
	((relation)->rd_rel->relnamespace)

#endif   /* REL_H */
