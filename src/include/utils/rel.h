/*-------------------------------------------------------------------------
 *
 * rel.h
 *	  POSTGRES relation descriptor (a/k/a relcache entry) definitions.
 *
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: rel.h,v 1.57 2002/03/26 19:16:58 tgl Exp $
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
	Oid			tgoid;
	char	   *tgname;
	Oid			tgfoid;
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
	/*
	 * Index data to identify which triggers are which.  Since each
	 * trigger can appear in more than one class, for each class we
	 * provide a list of integer indexes into the triggers array.
	 */
#define TRIGGER_NUM_EVENT_CLASSES  4

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
	bool		rd_myxactonly;	/* rel uses the local buffer mgr */
	bool		rd_isnailed;	/* rel is nailed in cache */
	bool		rd_indexfound;	/* true if rd_indexlist is valid */
	bool		rd_uniqueindex; /* true if rel is a UNIQUE index */
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
	Form_pg_am	rd_am;			/* pg_am tuple for index's AM */

	/* index access support info (used only for an index relation) */
	MemoryContext rd_indexcxt;	/* private memory cxt for this stuff */
	IndexStrategy rd_istrat;	/* operator strategy map */
	Oid		   *rd_operator;	/* OIDs of index operators */
	RegProcedure *rd_support;	/* OIDs of support procedures */
	struct FmgrInfo *rd_supportinfo;	/* lookup info for support
										 * procedures */
	/* "struct FmgrInfo" avoids need to include fmgr.h here */

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
 * Handle temp relations
 */
#define PG_TEMP_REL_PREFIX "pg_temp"
#define PG_TEMP_REL_PREFIX_LEN 7

#define is_temp_relname(relname) \
		(strncmp(relname, PG_TEMP_REL_PREFIX, PG_TEMP_REL_PREFIX_LEN) == 0)

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
	is_temp_relname(RelationGetPhysicalRelationName(relation)) \
	? \
		get_temp_rel_by_physicalname( \
			RelationGetPhysicalRelationName(relation)) \
	: \
		RelationGetPhysicalRelationName(relation) \
)

/*
 * RelationGetNamespace
 *
 *	  Returns the rel's namespace OID.
 */
#define RelationGetNamespace(relation) \
	((relation)->rd_rel->relnamespace)

/* added to prevent circular dependency.  bjm 1999/11/15 */
extern char *get_temp_rel_by_physicalname(const char *relname);

#endif   /* REL_H */
