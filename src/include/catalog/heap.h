/*-------------------------------------------------------------------------
 *
 * heap.h
 *	  prototypes for functions in backend/catalog/heap.c
 *
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: heap.h,v 1.62 2003/09/19 21:04:20 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef HEAP_H
#define HEAP_H

#include "catalog/pg_attribute.h"
#include "nodes/parsenodes.h"
#include "parser/parse_node.h"
#include "utils/rel.h"


typedef struct RawColumnDefault
{
	AttrNumber	attnum;			/* attribute to attach default to */
	Node	   *raw_default;	/* default value (untransformed parse
								 * tree) */
} RawColumnDefault;

extern Relation heap_create(const char *relname,
			Oid relnamespace,
			TupleDesc tupDesc,
			bool shared_relation,
			bool storage_create,
			bool allow_system_table_mods);

extern void heap_storage_create(Relation rel);

extern Oid heap_create_with_catalog(const char *relname,
						 Oid relnamespace,
						 TupleDesc tupdesc,
						 char relkind,
						 bool shared_relation,
						 OnCommitAction oncommit,
						 bool allow_system_table_mods);

extern void heap_drop_with_catalog(Oid rid);

extern void heap_truncate(Oid rid);

extern void heap_truncate_check_FKs(Relation rel);

extern void AddRelationRawConstraints(Relation rel,
						  List *rawColDefaults,
						  List *rawConstraints);

extern Node *cookDefault(ParseState *pstate,
			Node *raw_default,
			Oid atttypid,
			int32 atttypmod,
			char *attname);

extern int RemoveRelConstraints(Relation rel, const char *constrName,
					 DropBehavior behavior);

extern void DeleteRelationTuple(Oid relid);
extern void DeleteAttributeTuples(Oid relid);
extern void RemoveAttributeById(Oid relid, AttrNumber attnum);
extern void RemoveAttrDefault(Oid relid, AttrNumber attnum,
				  DropBehavior behavior, bool complain);
extern void RemoveAttrDefaultById(Oid attrdefId);

extern Form_pg_attribute SystemAttributeDefinition(AttrNumber attno,
						  bool relhasoids);

extern Form_pg_attribute SystemAttributeByName(const char *attname,
					  bool relhasoids);

extern void CheckAttributeNamesTypes(TupleDesc tupdesc, char relkind);

extern void CheckAttributeType(const char *attname, Oid atttypid);

#endif   /* HEAP_H */
