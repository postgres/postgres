/*-------------------------------------------------------------------------
 *
 * indexing.h--
 *    This include provides some definitions to support indexing 
 *    on system catalogs
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: indexing.h,v 1.4 1997/08/31 09:55:20 vadim Exp $
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
#define Num_pg_attr_indices	3
#define Num_pg_proc_indices	3
#define Num_pg_type_indices	2
#define Num_pg_class_indices	2
#define Num_pg_attrdef_indices	1
#define Num_pg_relcheck_indices	1
#define Num_pg_trigger_indices	1


/*
 * Names of indices on system catalogs
 */
#define AttributeNameIndex "pg_attnameind"
#define AttributeNumIndex  "pg_attnumind"
#define AttributeRelidIndex "pg_attrelidind"
#define ProcedureNameIndex "pg_procnameind"
#define ProcedureOidIndex  "pg_procidind"
#define ProcedureSrcIndex  "pg_procsrcind"
#define TypeNameIndex      "pg_typenameind"
#define TypeOidIndex       "pg_typeidind"
#define ClassNameIndex     "pg_classnameind"
#define ClassOidIndex      "pg_classoidind"
#define AttrDefaultIndex   "pg_attrdefind"
#define RelCheckIndex      "pg_relcheckind"
#define TriggerRelidIndex  "pg_trigrelidind"

extern char *Name_pg_attr_indices[];
extern char *Name_pg_proc_indices[];
extern char *Name_pg_type_indices[];
extern char *Name_pg_class_indices[];
extern char *Name_pg_attrdef_indices[];
extern char *Name_pg_relcheck_indices[];
extern char *Name_pg_trigger_indices[];

extern char *IndexedCatalogNames[];

/*
 * indexing.c prototypes 
 *
 * Functions for each index to perform the necessary scan on a cache miss.
 */
extern void CatalogOpenIndices(int nIndices, char *names[], Relation idescs[]);
extern void CatalogCloseIndices(int nIndices, Relation *idescs);
extern void CatalogIndexInsert(Relation *idescs,
			       int nIndices,
			       Relation heapRelation,
			       HeapTuple heapTuple);
extern bool CatalogHasIndex(char *catName, Oid catId);

extern HeapTuple AttributeNameIndexScan(Relation heapRelation,
					Oid relid,
					char *attname);

extern HeapTuple AttributeNumIndexScan(Relation heapRelation,
				       Oid relid,
				       AttrNumber attnum);
extern HeapTuple ProcedureOidIndexScan(Relation heapRelation, Oid procId);
extern HeapTuple ProcedureNameIndexScan(Relation heapRelation,
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
DECLARE_INDEX(pg_attnameind on pg_attribute using btree (mkoidname(attrelid, attname) oidname_ops));
DECLARE_INDEX(pg_attnumind  on pg_attribute using btree (mkoidint2(attrelid, attnum) oidint2_ops));
DECLARE_INDEX(pg_attrelidind on pg_attribute using btree (attrelid oid_ops));

DECLARE_INDEX(pg_procidind on pg_proc using btree (Oid oid_ops));
DECLARE_INDEX(pg_procnameind on pg_proc using btree (proname name_ops));
DECLARE_INDEX(pg_procsrcind on pg_proc using btree (prosrc text_ops));

DECLARE_INDEX(pg_typeidind on pg_type using btree (Oid oid_ops));
DECLARE_INDEX(pg_typenameind on pg_type using btree (typname name_ops));

DECLARE_INDEX(pg_classnameind on pg_class using btree (relname name_ops));
DECLARE_INDEX(pg_classoidind on pg_class using btree (Oid oid_ops));

DECLARE_INDEX(pg_attrdefind on pg_attrdef using btree (adrelid oid_ops));
DECLARE_INDEX(pg_relcheckind on pg_relcheck using btree (rcrelid oid_ops));

DECLARE_INDEX(pg_trigrelidind on pg_trigger using btree (tgrelid oid_ops));

/* now build indices in the initialization scripts */
BUILD_INDICES

#endif /* INDEXING_H */
