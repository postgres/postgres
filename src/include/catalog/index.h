/*-------------------------------------------------------------------------
 *
 * index.h
 *	  prototypes for index.c.
 *
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: index.h,v 1.53 2003/09/24 18:54:01 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef INDEX_H
#define INDEX_H

#include "access/itup.h"
#include "catalog/pg_index.h"
#include "nodes/execnodes.h"


#define DEFAULT_INDEX_TYPE	"btree"

/* Typedef for callback function for IndexBuildHeapScan */
typedef void (*IndexBuildCallback) (Relation index,
												HeapTuple htup,
												Datum *attdata,
												char *nulls,
												bool tupleIsAlive,
												void *state);


extern Oid index_create(Oid heapRelationId,
			 const char *indexRelationName,
			 IndexInfo *indexInfo,
			 Oid accessMethodObjectId,
			 Oid *classObjectId,
			 bool primary,
			 bool isconstraint,
			 bool allow_system_table_mods);

extern void index_drop(Oid indexId);

extern IndexInfo *BuildIndexInfo(Relation index);

extern void FormIndexDatum(IndexInfo *indexInfo,
			   HeapTuple heapTuple,
			   TupleDesc heapDescriptor,
			   EState *estate,
			   Datum *datum,
			   char *nullv);

extern void UpdateStats(Oid relid, double reltuples);

extern void setRelhasindex(Oid relid, bool hasindex,
			   bool isprimary, Oid reltoastidxid);

extern void setNewRelfilenode(Relation relation);

extern void index_build(Relation heapRelation, Relation indexRelation,
			IndexInfo *indexInfo);

extern double IndexBuildHeapScan(Relation heapRelation,
				   Relation indexRelation,
				   IndexInfo *indexInfo,
				   IndexBuildCallback callback,
				   void *callback_state);

extern void reindex_index(Oid indexId);
extern bool reindex_relation(Oid relid);

#endif   /* INDEX_H */
