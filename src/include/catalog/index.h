/*-------------------------------------------------------------------------
 *
 * index.h
 *	  prototypes for index.c.
 *
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: index.h,v 1.25 2000/06/17 23:41:51 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef INDEX_H
#define INDEX_H

#include "access/itup.h"
#include "nodes/execnodes.h"

extern Form_pg_am AccessMethodObjectIdGetForm(Oid accessMethodObjectId);

extern void UpdateIndexPredicate(Oid indexoid, Node *oldPred, Node *predicate);

extern void InitIndexStrategy(int numatts,
				  Relation indexRelation,
				  Oid accessMethodObjectId);

extern void index_create(char *heapRelationName,
			 char *indexRelationName,
			 FuncIndexInfo *funcInfo,
			 List *attributeList,
			 Oid accessMethodObjectId,
			 int numatts,
			 AttrNumber *attNums,
			 Oid *classObjectId,
			 Node *predicate,
			 bool islossy,
			 bool unique,
			 bool primary);

extern void index_drop(Oid indexId);

extern void FormIndexDatum(int numberOfAttributes,
			   AttrNumber *attributeNumber, HeapTuple heapTuple,
			   TupleDesc heapDescriptor, Datum *datum,
			   char *nullv, FuncIndexInfoPtr fInfo);

extern void UpdateStats(Oid relid, long reltuples, bool inplace);
extern bool IndexesAreActive(Oid relid, bool comfirmCommitted);
extern void setRelhasindexInplace(Oid relid, bool hasindex, bool immediate);
extern bool SetReindexProcessing(bool processing);
extern bool IsReindexProcessing(void);

extern void FillDummyExprContext(ExprContext *econtext, TupleTableSlot *slot,
					 TupleDesc tupdesc, Buffer buffer);

extern void index_build(Relation heapRelation, Relation indexRelation,
						int numberOfAttributes, AttrNumber *attributeNumber,
						FuncIndexInfo *funcInfo, PredInfo *predInfo,
						bool unique);

extern bool IndexIsUnique(Oid indexId);

extern bool reindex_index(Oid indexId, bool force);
extern bool activate_indexes_of_a_table(Oid relid, bool activate);
extern bool reindex_relation(Oid relid, bool force);

#endif	 /* INDEX_H */
