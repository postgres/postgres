/*-------------------------------------------------------------------------
 *
 * indexing.h--
 *	  This include provides some definitions to support indexing
 *	  on system catalogs
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: indexing.h,v 1.12 1997/11/17 16:59:34 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef INDEXING_H
#define INDEXING_H

#include <access/htup.h>
#include <utils/rel.h>

/*
 * Some definitions for indices on pg_attribute
 */
#define Num_pg_attr_indices		3
#define Num_pg_proc_indices		3
#define Num_pg_type_indices		2
#define Num_pg_class_indices	2
#define Num_pg_attrdef_indices	1
#define Num_pg_relcheck_indices 1
#define Num_pg_trigger_indices	1
#define Num_pg_description_indices	1


/*
 * Names of indices on system catalogs
 */
#define AttributeNameIndex "pg_attribute_mkoidname_index"
#define AttributeNumIndex  "pg_attribute_mkoidint2_index"
#define AttributeRelidIndex "pg_attribute_attrelid_index"
#define ProcedureOidIndex  "pg_proc_oid_index"
#define ProcedureNameIndex "pg_proc_proname_index"
#define ProcedureSrcIndex  "pg_proc_prosrc_index"
#define TypeOidIndex	   "pg_type_oid_index"
#define TypeNameIndex	   "pg_type_typname_index"
#define ClassOidIndex	   "pg_class_oid_index"
#define ClassNameIndex	   "pg_class_relname_index"
#define AttrDefaultIndex   "pg_attrdef_adrelid_index"
#define RelCheckIndex	   "pg_relcheck_rcrelid_index"
#define TriggerRelidIndex  "pg_trigger_tgrelid_index"
#define DescriptionObjIndex "pg_description_objoid_index"

extern char *Name_pg_attr_indices[];
extern char *Name_pg_proc_indices[];
extern char *Name_pg_type_indices[];
extern char *Name_pg_class_indices[];
extern char *Name_pg_attrdef_indices[];
extern char *Name_pg_relcheck_indices[];
extern char *Name_pg_trigger_indices[];
extern char *Name_pg_description_indices[];

extern char *IndexedCatalogNames[];

/*
 * indexing.c prototypes
 *
 * Functions for each index to perform the necessary scan on a cache miss.
 */
extern void CatalogOpenIndices(int nIndices, char *names[], Relation idescs[]);
extern void CatalogCloseIndices(int nIndices, Relation *idescs);
extern void
CatalogIndexInsert(Relation *idescs,
				   int nIndices,
				   Relation heapRelation,
				   HeapTuple heapTuple);
extern bool CatalogHasIndex(char *catName, Oid catId);

extern HeapTuple
AttributeNameIndexScan(Relation heapRelation,
					   Oid relid,
					   char *attname);

extern HeapTuple
AttributeNumIndexScan(Relation heapRelation,
					  Oid relid,
					  AttrNumber attnum);
extern HeapTuple ProcedureOidIndexScan(Relation heapRelation, Oid procId);
extern HeapTuple
ProcedureNameIndexScan(Relation heapRelation,
					   char *procName, int nargs, Oid *argTypes);
extern HeapTuple ProcedureSrcIndexScan(Relation heapRelation, text *procSrc);
extern HeapTuple TypeOidIndexScan(Relation heapRelation, Oid typeId);
extern HeapTuple TypeNameIndexScan(Relation heapRelation, char *typeName);
extern HeapTuple ClassNameIndexScan(Relation heapRelation, char *relName);
extern HeapTuple ClassOidIndexScan(Relation heapRelation, Oid relId);


/*
 * What follows are lines processed by genbki.sh to create the statements
 * the bootstrap parser will turn into DefineIndex commands.
 *
 * The keyword is DECLARE_INDEX every thing after that is just like in a
 * normal specification of the 'define index' POSTQUEL command.
 */
DECLARE_INDEX(pg_attribute_mkoidname_index on pg_attribute using btree(mkoidname(attrelid, attname) oidname_ops));
DECLARE_INDEX(pg_attribute_mkoidint2_index on pg_attribute using btree(mkoidint2(attrelid, attnum) oidint2_ops));
DECLARE_INDEX(pg_attribute_attrelid_index on pg_attribute using btree(attrelid oid_ops));

DECLARE_INDEX(pg_proc_oid_index on pg_proc using btree(oid oid_ops));
DECLARE_INDEX(pg_proc_proname_index on pg_proc using btree(proname name_ops));
DECLARE_INDEX(pg_proc_prosrc_index on pg_proc using btree(prosrc text_ops));

DECLARE_INDEX(pg_type_oid_index on pg_type using btree(oid oid_ops));
DECLARE_INDEX(pg_type_typname_index on pg_type using btree(typname name_ops));

DECLARE_INDEX(pg_class_oid_index on pg_class using btree(oid oid_ops));
DECLARE_INDEX(pg_class_relname_index on pg_class using btree(relname name_ops));

DECLARE_INDEX(pg_attrdef_adrelid_index on pg_attrdef using btree(adrelid oid_ops));

DECLARE_INDEX(pg_relcheck_rcrelid_index on pg_relcheck using btree(rcrelid oid_ops));

DECLARE_INDEX(pg_trigger_tgrelid_index on pg_trigger using btree(tgrelid oid_ops));

DECLARE_INDEX(pg_description_objoid_index on pg_description using btree(objoid oid_ops));

/* now build indices in the initialization scripts */
BUILD_INDICES

#endif							/* INDEXING_H */
