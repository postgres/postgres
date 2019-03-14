/*-------------------------------------------------------------------------
 *
 * pg_class.h
 *	  definition of the "relation" system catalog (pg_class)
 *
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_class.h
 *
 * NOTES
 *	  The Catalog.pm module reads this file and derives schema
 *	  information.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_CLASS_H
#define PG_CLASS_H

#include "catalog/genbki.h"
#include "catalog/pg_class_d.h"

/* ----------------
 *		pg_class definition.  cpp turns this into
 *		typedef struct FormData_pg_class
 * ----------------
 */
CATALOG(pg_class,1259,RelationRelationId) BKI_BOOTSTRAP BKI_ROWTYPE_OID(83,RelationRelation_Rowtype_Id) BKI_SCHEMA_MACRO
{
	/* oid */
	Oid			oid;

	/* class name */
	NameData	relname;

	/* OID of namespace containing this class */
	Oid			relnamespace BKI_DEFAULT(PGNSP);

	/* OID of entry in pg_type for table's implicit row type */
	Oid			reltype BKI_LOOKUP(pg_type);

	/* OID of entry in pg_type for underlying composite type */
	Oid			reloftype BKI_DEFAULT(0) BKI_LOOKUP(pg_type);

	/* class owner */
	Oid			relowner BKI_DEFAULT(PGUID);

	/* access method; 0 if not a table / index */
	Oid			relam BKI_LOOKUP(pg_am);

	/* identifier of physical storage file */
	/* relfilenode == 0 means it is a "mapped" relation, see relmapper.c */
	Oid			relfilenode;

	/* identifier of table space for relation (0 means default for database) */
	Oid			reltablespace BKI_DEFAULT(0) BKI_LOOKUP(pg_tablespace);

	/* # of blocks (not always up-to-date) */
	int32		relpages;

	/* # of tuples (not always up-to-date) */
	float4		reltuples;

	/* # of all-visible blocks (not always up-to-date) */
	int32		relallvisible;

	/* OID of toast table; 0 if none */
	Oid			reltoastrelid;

	/* T if has (or has had) any indexes */
	bool		relhasindex;

	/* T if shared across databases */
	bool		relisshared;

	/* see RELPERSISTENCE_xxx constants below */
	char		relpersistence;

	/* see RELKIND_xxx constants below */
	char		relkind;

	/* number of user attributes */
	int16		relnatts;

	/*
	 * Class pg_attribute must contain exactly "relnatts" user attributes
	 * (with attnums ranging from 1 to relnatts) for this class.  It may also
	 * contain entries with negative attnums for system attributes.
	 */

	/* # of CHECK constraints for class */
	int16		relchecks;

	/* has (or has had) any rules */
	bool		relhasrules;

	/* has (or has had) any TRIGGERs */
	bool		relhastriggers;

	/* has (or has had) child tables or indexes */
	bool		relhassubclass;

	/* row security is enabled or not */
	bool		relrowsecurity;

	/* row security forced for owners or not */
	bool		relforcerowsecurity;

	/* matview currently holds query results */
	bool		relispopulated;

	/* see REPLICA_IDENTITY_xxx constants */
	char		relreplident;

	/* is relation a partition? */
	bool		relispartition;

	/* heap for rewrite during DDL, link to original rel */
	Oid			relrewrite BKI_DEFAULT(0);

	/* all Xids < this are frozen in this rel */
	TransactionId relfrozenxid;

	/* all multixacts in this rel are >= this; it is really a MultiXactId */
	TransactionId relminmxid;

#ifdef CATALOG_VARLEN			/* variable-length fields start here */
	/* NOTE: These fields are not present in a relcache entry's rd_rel field. */
	/* access permissions */
	aclitem		relacl[1];

	/* access-method-specific options */
	text		reloptions[1];

	/* partition bound node tree */
	pg_node_tree relpartbound;
#endif
} FormData_pg_class;

/* Size of fixed part of pg_class tuples, not counting var-length fields */
#define CLASS_TUPLE_SIZE \
	 (offsetof(FormData_pg_class,relminmxid) + sizeof(TransactionId))

/* ----------------
 *		Form_pg_class corresponds to a pointer to a tuple with
 *		the format of pg_class relation.
 * ----------------
 */
typedef FormData_pg_class *Form_pg_class;

#ifdef EXPOSE_TO_CLIENT_CODE

#define		  RELKIND_RELATION		  'r'	/* ordinary table */
#define		  RELKIND_INDEX			  'i'	/* secondary index */
#define		  RELKIND_SEQUENCE		  'S'	/* sequence object */
#define		  RELKIND_TOASTVALUE	  't'	/* for out-of-line values */
#define		  RELKIND_VIEW			  'v'	/* view */
#define		  RELKIND_MATVIEW		  'm'	/* materialized view */
#define		  RELKIND_COMPOSITE_TYPE  'c'	/* composite type */
#define		  RELKIND_FOREIGN_TABLE   'f'	/* foreign table */
#define		  RELKIND_PARTITIONED_TABLE 'p' /* partitioned table */
#define		  RELKIND_PARTITIONED_INDEX 'I' /* partitioned index */

#define		  RELPERSISTENCE_PERMANENT	'p' /* regular table */
#define		  RELPERSISTENCE_UNLOGGED	'u' /* unlogged permanent table */
#define		  RELPERSISTENCE_TEMP		't' /* temporary table */

/* default selection for replica identity (primary key or nothing) */
#define		  REPLICA_IDENTITY_DEFAULT	'd'
/* no replica identity is logged for this relation */
#define		  REPLICA_IDENTITY_NOTHING	'n'
/* all columns are logged as replica identity */
#define		  REPLICA_IDENTITY_FULL		'f'
/*
 * an explicitly chosen candidate key's columns are used as replica identity.
 * Note this will still be set if the index has been dropped; in that case it
 * has the same meaning as 'd'.
 */
#define		  REPLICA_IDENTITY_INDEX	'i'

/*
 * Relation kinds that have physical storage. These relations normally have
 * relfilenode set to non-zero, but it can also be zero if the relation is
 * mapped.
 */
#define RELKIND_HAS_STORAGE(relkind) \
	((relkind) == RELKIND_RELATION || \
	 (relkind) == RELKIND_INDEX || \
	 (relkind) == RELKIND_SEQUENCE || \
	 (relkind) == RELKIND_TOASTVALUE || \
	 (relkind) == RELKIND_MATVIEW)


#endif							/* EXPOSE_TO_CLIENT_CODE */

#endif							/* PG_CLASS_H */
