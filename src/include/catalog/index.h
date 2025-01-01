/*-------------------------------------------------------------------------
 *
 * index.h
 *	  prototypes for catalog/index.c.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/index.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef INDEX_H
#define INDEX_H

#include "catalog/objectaddress.h"
#include "nodes/execnodes.h"


#define DEFAULT_INDEX_TYPE	"btree"

/* Action code for index_set_state_flags */
typedef enum
{
	INDEX_CREATE_SET_READY,
	INDEX_CREATE_SET_VALID,
	INDEX_DROP_CLEAR_VALID,
	INDEX_DROP_SET_DEAD,
} IndexStateFlagsAction;

/* options for REINDEX */
typedef struct ReindexParams
{
	bits32		options;		/* bitmask of REINDEXOPT_* */
	Oid			tablespaceOid;	/* New tablespace to move indexes to.
								 * InvalidOid to do nothing. */
} ReindexParams;

/* flag bits for ReindexParams->flags */
#define REINDEXOPT_VERBOSE		0x01	/* print progress info */
#define REINDEXOPT_REPORT_PROGRESS 0x02 /* report pgstat progress */
#define REINDEXOPT_MISSING_OK 	0x04	/* skip missing relations */
#define REINDEXOPT_CONCURRENTLY	0x08	/* concurrent mode */

/* state info for validate_index bulkdelete callback */
typedef struct ValidateIndexState
{
	Tuplesortstate *tuplesort;	/* for sorting the index TIDs */
	/* statistics (for debug purposes only): */
	double		htups,
				itups,
				tups_inserted;
} ValidateIndexState;

extern void index_check_primary_key(Relation heapRel,
									const IndexInfo *indexInfo,
									bool is_alter_table,
									const IndexStmt *stmt);

#define	INDEX_CREATE_IS_PRIMARY				(1 << 0)
#define	INDEX_CREATE_ADD_CONSTRAINT			(1 << 1)
#define	INDEX_CREATE_SKIP_BUILD				(1 << 2)
#define	INDEX_CREATE_CONCURRENT				(1 << 3)
#define	INDEX_CREATE_IF_NOT_EXISTS			(1 << 4)
#define	INDEX_CREATE_PARTITIONED			(1 << 5)
#define INDEX_CREATE_INVALID				(1 << 6)

extern Oid	index_create(Relation heapRelation,
						 const char *indexRelationName,
						 Oid indexRelationId,
						 Oid parentIndexRelid,
						 Oid parentConstraintId,
						 RelFileNumber relFileNumber,
						 IndexInfo *indexInfo,
						 const List *indexColNames,
						 Oid accessMethodId,
						 Oid tableSpaceId,
						 const Oid *collationIds,
						 const Oid *opclassIds,
						 const Datum *opclassOptions,
						 const int16 *coloptions,
						 const NullableDatum *stattargets,
						 Datum reloptions,
						 bits16 flags,
						 bits16 constr_flags,
						 bool allow_system_table_mods,
						 bool is_internal,
						 Oid *constraintId);

#define	INDEX_CONSTR_CREATE_MARK_AS_PRIMARY	(1 << 0)
#define	INDEX_CONSTR_CREATE_DEFERRABLE		(1 << 1)
#define	INDEX_CONSTR_CREATE_INIT_DEFERRED	(1 << 2)
#define	INDEX_CONSTR_CREATE_UPDATE_INDEX	(1 << 3)
#define	INDEX_CONSTR_CREATE_REMOVE_OLD_DEPS	(1 << 4)
#define	INDEX_CONSTR_CREATE_WITHOUT_OVERLAPS (1 << 5)

extern Oid	index_concurrently_create_copy(Relation heapRelation,
										   Oid oldIndexId,
										   Oid tablespaceOid,
										   const char *newName);

extern void index_concurrently_build(Oid heapRelationId,
									 Oid indexRelationId);

extern void index_concurrently_swap(Oid newIndexId,
									Oid oldIndexId,
									const char *oldName);

extern void index_concurrently_set_dead(Oid heapId,
										Oid indexId);

extern ObjectAddress index_constraint_create(Relation heapRelation,
											 Oid indexRelationId,
											 Oid parentConstraintId,
											 const IndexInfo *indexInfo,
											 const char *constraintName,
											 char constraintType,
											 bits16 constr_flags,
											 bool allow_system_table_mods,
											 bool is_internal);

extern void index_drop(Oid indexId, bool concurrent, bool concurrent_lock_mode);

extern IndexInfo *BuildIndexInfo(Relation index);

extern IndexInfo *BuildDummyIndexInfo(Relation index);

extern bool CompareIndexInfo(const IndexInfo *info1, const IndexInfo *info2,
							 const Oid *collations1, const Oid *collations2,
							 const Oid *opfamilies1, const Oid *opfamilies2,
							 const AttrMap *attmap);

extern void BuildSpeculativeIndexInfo(Relation index, IndexInfo *ii);

extern void FormIndexDatum(IndexInfo *indexInfo,
						   TupleTableSlot *slot,
						   EState *estate,
						   Datum *values,
						   bool *isnull);

extern void index_build(Relation heapRelation,
						Relation indexRelation,
						IndexInfo *indexInfo,
						bool isreindex,
						bool parallel);

extern void validate_index(Oid heapId, Oid indexId, Snapshot snapshot);

extern void index_set_state_flags(Oid indexId, IndexStateFlagsAction action);

extern Oid	IndexGetRelation(Oid indexId, bool missing_ok);

extern void reindex_index(const ReindexStmt *stmt, Oid indexId,
						  bool skip_constraint_checks, char persistence,
						  const ReindexParams *params);

/* Flag bits for reindex_relation(): */
#define REINDEX_REL_PROCESS_TOAST			0x01
#define REINDEX_REL_SUPPRESS_INDEX_USE		0x02
#define REINDEX_REL_CHECK_CONSTRAINTS		0x04
#define REINDEX_REL_FORCE_INDEXES_UNLOGGED	0x08
#define REINDEX_REL_FORCE_INDEXES_PERMANENT 0x10

extern bool reindex_relation(const ReindexStmt *stmt, Oid relid, int flags,
							 const ReindexParams *params);

extern bool ReindexIsProcessingHeap(Oid heapOid);
extern bool ReindexIsProcessingIndex(Oid indexOid);

extern void ResetReindexState(int nestLevel);
extern Size EstimateReindexStateSpace(void);
extern void SerializeReindexState(Size maxsize, char *start_address);
extern void RestoreReindexState(const void *reindexstate);

extern void IndexSetParentIndex(Relation partitionIdx, Oid parentOid);


/*
 * itemptr_encode - Encode ItemPointer as int64/int8
 *
 * This representation must produce values encoded as int64 that sort in the
 * same order as their corresponding original TID values would (using the
 * default int8 opclass to produce a result equivalent to the default TID
 * opclass).
 *
 * As noted in validate_index(), this can be significantly faster.
 */
static inline int64
itemptr_encode(ItemPointer itemptr)
{
	BlockNumber block = ItemPointerGetBlockNumber(itemptr);
	OffsetNumber offset = ItemPointerGetOffsetNumber(itemptr);
	int64		encoded;

	/*
	 * Use the 16 least significant bits for the offset.  32 adjacent bits are
	 * used for the block number.  Since remaining bits are unused, there
	 * cannot be negative encoded values (We assume a two's complement
	 * representation).
	 */
	encoded = ((uint64) block << 16) | (uint16) offset;

	return encoded;
}

/*
 * itemptr_decode - Decode int64/int8 representation back to ItemPointer
 */
static inline void
itemptr_decode(ItemPointer itemptr, int64 encoded)
{
	BlockNumber block = (BlockNumber) (encoded >> 16);
	OffsetNumber offset = (OffsetNumber) (encoded & 0xFFFF);

	ItemPointerSet(itemptr, block, offset);
}

#endif							/* INDEX_H */
