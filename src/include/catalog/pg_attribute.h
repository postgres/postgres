/*-------------------------------------------------------------------------
 *
 * pg_attribute.h
 *	  definition of the system "attribute" relation (pg_attribute)
 *	  along with the relation's initial contents.
 *
 *
 * Portions Copyright (c) 1996-2010, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/catalog/pg_attribute.h,v 1.158 2010/01/22 16:40:19 rhaas Exp $
 *
 * NOTES
 *	  the genbki.pl script reads this file and generates .bki
 *	  information from the DATA() statements.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_ATTRIBUTE_H
#define PG_ATTRIBUTE_H

#include "catalog/genbki.h"

/* ----------------
 *		pg_attribute definition.  cpp turns this into
 *		typedef struct FormData_pg_attribute
 *
 *		If you change the following, make sure you change the structs for
 *		system attributes in catalog/heap.c also.
 *		You may need to change catalog/genbki.pl as well.
 * ----------------
 */
#define AttributeRelationId  1249
#define AttributeRelation_Rowtype_Id  75

CATALOG(pg_attribute,1249) BKI_BOOTSTRAP BKI_WITHOUT_OIDS BKI_ROWTYPE_OID(75) BKI_SCHEMA_MACRO
{
	Oid			attrelid;		/* OID of relation containing this attribute */
	NameData	attname;		/* name of attribute */

	/*
	 * atttypid is the OID of the instance in Catalog Class pg_type that
	 * defines the data type of this attribute (e.g. int4).  Information in
	 * that instance is redundant with the attlen, attbyval, and attalign
	 * attributes of this instance, so they had better match or Postgres will
	 * fail.
	 */
	Oid			atttypid;

	/*
	 * attstattarget is the target number of statistics datapoints to collect
	 * during VACUUM ANALYZE of this column.  A zero here indicates that we do
	 * not wish to collect any stats about this column. A "-1" here indicates
	 * that no value has been explicitly set for this column, so ANALYZE
	 * should use the default setting.
	 */
	int4		attstattarget;

	/*
	 * attlen is a copy of the typlen field from pg_type for this attribute.
	 * See atttypid comments above.
	 */
	int2		attlen;

	/*
	 * attnum is the "attribute number" for the attribute:	A value that
	 * uniquely identifies this attribute within its class. For user
	 * attributes, Attribute numbers are greater than 0 and not greater than
	 * the number of attributes in the class. I.e. if the Class pg_class says
	 * that Class XYZ has 10 attributes, then the user attribute numbers in
	 * Class pg_attribute must be 1-10.
	 *
	 * System attributes have attribute numbers less than 0 that are unique
	 * within the class, but not constrained to any particular range.
	 *
	 * Note that (attnum - 1) is often used as the index to an array.
	 */
	int2		attnum;

	/*
	 * attndims is the declared number of dimensions, if an array type,
	 * otherwise zero.
	 */
	int4		attndims;

	/*
	 * fastgetattr() uses attcacheoff to cache byte offsets of attributes in
	 * heap tuples.  The value actually stored in pg_attribute (-1) indicates
	 * no cached value.  But when we copy these tuples into a tuple
	 * descriptor, we may then update attcacheoff in the copies. This speeds
	 * up the attribute walking process.
	 */
	int4		attcacheoff;

	/*
	 * atttypmod records type-specific data supplied at table creation time
	 * (for example, the max length of a varchar field).  It is passed to
	 * type-specific input and output functions as the third argument. The
	 * value will generally be -1 for types that do not need typmod.
	 */
	int4		atttypmod;

	/*
	 * attbyval is a copy of the typbyval field from pg_type for this
	 * attribute.  See atttypid comments above.
	 */
	bool		attbyval;

	/*----------
	 * attstorage tells for VARLENA attributes, what the heap access
	 * methods can do to it if a given tuple doesn't fit into a page.
	 * Possible values are
	 *		'p': Value must be stored plain always
	 *		'e': Value can be stored in "secondary" relation (if relation
	 *			 has one, see pg_class.reltoastrelid)
	 *		'm': Value can be stored compressed inline
	 *		'x': Value can be stored compressed inline or in "secondary"
	 * Note that 'm' fields can also be moved out to secondary storage,
	 * but only as a last resort ('e' and 'x' fields are moved first).
	 *----------
	 */
	char		attstorage;

	/*
	 * attalign is a copy of the typalign field from pg_type for this
	 * attribute.  See atttypid comments above.
	 */
	char		attalign;

	/* This flag represents the "NOT NULL" constraint */
	bool		attnotnull;

	/* Has DEFAULT value or not */
	bool		atthasdef;

	/* Is dropped (ie, logically invisible) or not */
	bool		attisdropped;

	/* Has a local definition (hence, do not drop when attinhcount is 0) */
	bool		attislocal;

	/* Number of times inherited from direct parent relation(s) */
	int4		attinhcount;

	/*
	 * VARIABLE LENGTH FIELDS start here.  These fields may be NULL, too.
	 *
	 * NOTE: the following fields are not present in tuple descriptors!
	 */

	/* Column-level access permissions */
	aclitem		attacl[1];

	/* Column-level options */
	text		attoptions[1];
} FormData_pg_attribute;

/*
 * ATTRIBUTE_FIXED_PART_SIZE is the size of the fixed-layout,
 * guaranteed-not-null part of a pg_attribute row.  This is in fact as much
 * of the row as gets copied into tuple descriptors, so don't expect you
 * can access fields beyond attinhcount except in a real tuple!
 */
#define ATTRIBUTE_FIXED_PART_SIZE \
	(offsetof(FormData_pg_attribute,attinhcount) + sizeof(int4))

/* ----------------
 *		Form_pg_attribute corresponds to a pointer to a tuple with
 *		the format of pg_attribute relation.
 * ----------------
 */
typedef FormData_pg_attribute *Form_pg_attribute;

/* ----------------
 *		compiler constants for pg_attribute
 * ----------------
 */

#define Natts_pg_attribute				19
#define Anum_pg_attribute_attrelid		1
#define Anum_pg_attribute_attname		2
#define Anum_pg_attribute_atttypid		3
#define Anum_pg_attribute_attstattarget 4
#define Anum_pg_attribute_attlen		5
#define Anum_pg_attribute_attnum		6
#define Anum_pg_attribute_attndims		7
#define Anum_pg_attribute_attcacheoff	8
#define Anum_pg_attribute_atttypmod		9
#define Anum_pg_attribute_attbyval		10
#define Anum_pg_attribute_attstorage	11
#define Anum_pg_attribute_attalign		12
#define Anum_pg_attribute_attnotnull	13
#define Anum_pg_attribute_atthasdef		14
#define Anum_pg_attribute_attisdropped	15
#define Anum_pg_attribute_attislocal	16
#define Anum_pg_attribute_attinhcount	17
#define Anum_pg_attribute_attacl		18
#define Anum_pg_attribute_attoptions	19


/* ----------------
 *		initial contents of pg_attribute
 *
 * The initial contents of pg_attribute are generated at compile time by
 * genbki.pl.  Only "bootstrapped" relations need be included.
 * ----------------
 */

#endif   /* PG_ATTRIBUTE_H */
