/*-------------------------------------------------------------------------
 *
 * index.h
 *	  prototypes for index.c.
 *
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: index.h,v 1.42 2001/10/28 06:25:59 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef INDEX_H
#define INDEX_H

#include "access/itup.h"
#include "nodes/execnodes.h"


/* Typedef for callback function for IndexBuildHeapScan */
typedef void (*IndexBuildCallback) (Relation index,
												HeapTuple htup,
												Datum *attdata,
												char *nulls,
												bool tupleIsAlive,
												void *state);


extern Form_pg_am AccessMethodObjectIdGetForm(Oid accessMethodObjectId,
							MemoryContext resultCxt);

extern Oid index_create(char *heapRelationName,
			 char *indexRelationName,
			 IndexInfo *indexInfo,
			 Oid accessMethodObjectId,
			 Oid *classObjectId,
			 bool primary,
			 bool allow_system_table_mods);

extern void index_drop(Oid indexId);

extern IndexInfo *BuildIndexInfo(HeapTuple indexTuple);

extern void FormIndexDatum(IndexInfo *indexInfo,
			   HeapTuple heapTuple,
			   TupleDesc heapDescriptor,
			   MemoryContext resultCxt,
			   Datum *datum,
			   char *nullv);

extern void UpdateStats(Oid relid, double reltuples);
extern bool IndexesAreActive(Oid relid, bool comfirmCommitted);
extern void setRelhasindex(Oid relid, bool hasindex,
			   bool isprimary, Oid reltoastidxid);

extern void setNewRelfilenode(Relation relation);

extern bool SetReindexProcessing(bool processing);
extern bool IsReindexProcessing(void);

extern void index_build(Relation heapRelation, Relation indexRelation,
			IndexInfo *indexInfo);

extern double IndexBuildHeapScan(Relation heapRelation,
				   Relation indexRelation,
				   IndexInfo *indexInfo,
				   IndexBuildCallback callback,
				   void *callback_state);

extern bool reindex_index(Oid indexId, bool force, bool inplace);
extern bool activate_indexes_of_a_table(Oid relid, bool activate);
extern bool reindex_relation(Oid relid, bool force);

#endif	 /* INDEX_H */
