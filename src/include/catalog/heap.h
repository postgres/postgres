/*-------------------------------------------------------------------------
 *
 * heap.h
 *	  prototypes for functions in backend/catalog/heap.c
 *
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/catalog/heap.h,v 1.68 2004/07/11 19:52:51 tgl Exp $
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

typedef struct CookedConstraint
{
	ConstrType	contype;		/* CONSTR_DEFAULT or CONSTR_CHECK */
	char	   *name;			/* name, or NULL if none */
	AttrNumber	attnum;			/* which attr (only for DEFAULT) */
	Node	   *expr;			/* transformed default or check expr */
} CookedConstraint;

extern Relation heap_create(const char *relname,
			Oid relnamespace,
			Oid reltablespace,
			TupleDesc tupDesc,
			bool shared_relation,
			bool create_storage,
			bool allow_system_table_mods);

extern Oid heap_create_with_catalog(const char *relname,
						 Oid relnamespace,
						 Oid reltablespace,
						 TupleDesc tupdesc,
						 char relkind,
						 bool shared_relation,
						 bool oidislocal,
						 int oidinhcount,
						 OnCommitAction oncommit,
						 bool allow_system_table_mods);

extern void heap_drop_with_catalog(Oid rid);

extern void heap_truncate(Oid rid);

extern void heap_truncate_check_FKs(Relation rel);

extern List *AddRelationRawConstraints(Relation rel,
						  List *rawColDefaults,
						  List *rawConstraints);

extern void StoreAttrDefault(Relation rel, AttrNumber attnum, char *adbin);

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
extern void RemoveStatistics(Relation rel, AttrNumber attnum);

extern Form_pg_attribute SystemAttributeDefinition(AttrNumber attno,
						  bool relhasoids);

extern Form_pg_attribute SystemAttributeByName(const char *attname,
					  bool relhasoids);

extern void CheckAttributeNamesTypes(TupleDesc tupdesc, char relkind);

extern void CheckAttributeType(const char *attname, Oid atttypid);

#endif   /* HEAP_H */
