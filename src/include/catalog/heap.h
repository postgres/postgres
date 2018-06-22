/*-------------------------------------------------------------------------
 *
 * heap.h
 *	  prototypes for functions in backend/catalog/heap.c
 *
 *
 * Portions Copyright (c) 1996-2018, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/heap.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef HEAP_H
#define HEAP_H

#include "catalog/indexing.h"
#include "catalog/objectaddress.h"
#include "parser/parse_node.h"


typedef struct RawColumnDefault
{
	AttrNumber	attnum;			/* attribute to attach default to */
	Node	   *raw_default;	/* default value (untransformed parse tree) */
	bool		missingMode;	/* true if part of add column processing */
} RawColumnDefault;

typedef struct CookedConstraint
{
	ConstrType	contype;		/* CONSTR_DEFAULT or CONSTR_CHECK */
	Oid			conoid;			/* constr OID if created, otherwise Invalid */
	char	   *name;			/* name, or NULL if none */
	AttrNumber	attnum;			/* which attr (only for DEFAULT) */
	Node	   *expr;			/* transformed default or check expr */
	bool		skip_validation;	/* skip validation? (only for CHECK) */
	bool		is_local;		/* constraint has local (non-inherited) def */
	int			inhcount;		/* number of times constraint is inherited */
	bool		is_no_inherit;	/* constraint has local def and cannot be
								 * inherited */
} CookedConstraint;

extern Relation heap_create(const char *relname,
			Oid relnamespace,
			Oid reltablespace,
			Oid relid,
			Oid relfilenode,
			TupleDesc tupDesc,
			char relkind,
			char relpersistence,
			bool shared_relation,
			bool mapped_relation,
			bool allow_system_table_mods);

extern Oid heap_create_with_catalog(const char *relname,
						 Oid relnamespace,
						 Oid reltablespace,
						 Oid relid,
						 Oid reltypeid,
						 Oid reloftypeid,
						 Oid ownerid,
						 TupleDesc tupdesc,
						 List *cooked_constraints,
						 char relkind,
						 char relpersistence,
						 bool shared_relation,
						 bool mapped_relation,
						 bool oidislocal,
						 int oidinhcount,
						 OnCommitAction oncommit,
						 Datum reloptions,
						 bool use_user_acl,
						 bool allow_system_table_mods,
						 bool is_internal,
						 Oid relrewrite,
						 ObjectAddress *typaddress);

extern void heap_create_init_fork(Relation rel);

extern void heap_drop_with_catalog(Oid relid);

extern void heap_truncate(List *relids);

extern void heap_truncate_one_rel(Relation rel);

extern void heap_truncate_check_FKs(List *relations, bool tempTables);

extern List *heap_truncate_find_FKs(List *relationIds);

extern void InsertPgAttributeTuple(Relation pg_attribute_rel,
					   Form_pg_attribute new_attribute,
					   CatalogIndexState indstate);

extern void InsertPgClassTuple(Relation pg_class_desc,
				   Relation new_rel_desc,
				   Oid new_rel_oid,
				   Datum relacl,
				   Datum reloptions);

extern List *AddRelationNewConstraints(Relation rel,
						  List *newColDefaults,
						  List *newConstraints,
						  bool allow_merge,
						  bool is_local,
						  bool is_internal);

extern void RelationClearMissing(Relation rel);
extern void SetAttrMissing(Oid relid, char *attname, char *value);

extern Oid StoreAttrDefault(Relation rel, AttrNumber attnum,
				 Node *expr, bool is_internal,
				 bool add_column_mode);

extern Node *cookDefault(ParseState *pstate,
			Node *raw_default,
			Oid atttypid,
			int32 atttypmod,
			const char *attname);

extern void DeleteRelationTuple(Oid relid);
extern void DeleteAttributeTuples(Oid relid);
extern void DeleteSystemAttributeTuples(Oid relid);
extern void RemoveAttributeById(Oid relid, AttrNumber attnum);
extern void RemoveAttrDefault(Oid relid, AttrNumber attnum,
				  DropBehavior behavior, bool complain, bool internal);
extern void RemoveAttrDefaultById(Oid attrdefId);
extern void RemoveStatistics(Oid relid, AttrNumber attnum);

extern Form_pg_attribute SystemAttributeDefinition(AttrNumber attno,
						  bool relhasoids);

extern Form_pg_attribute SystemAttributeByName(const char *attname,
					  bool relhasoids);

extern void CheckAttributeNamesTypes(TupleDesc tupdesc, char relkind,
						 bool allow_system_table_mods);

extern void CheckAttributeType(const char *attname,
				   Oid atttypid, Oid attcollation,
				   List *containing_rowtypes,
				   bool allow_system_table_mods);

/* pg_partitioned_table catalog manipulation functions */
extern void StorePartitionKey(Relation rel,
				  char strategy,
				  int16 partnatts,
				  AttrNumber *partattrs,
				  List *partexprs,
				  Oid *partopclass,
				  Oid *partcollation);
extern void RemovePartitionKeyByRelId(Oid relid);
extern void StorePartitionBound(Relation rel, Relation parent,
					PartitionBoundSpec *bound);

#endif							/* HEAP_H */
