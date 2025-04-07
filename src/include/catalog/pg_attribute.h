/*-------------------------------------------------------------------------
 *
 * pg_attribute.h
 *	  definition of the "attribute" system catalog (pg_attribute)
 *
 * The initial contents of pg_attribute are generated at compile time by
 * genbki.pl, so there is no pg_attribute.dat file.  Only "bootstrapped"
 * relations need be included.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_attribute.h
 *
 * NOTES
 *	  The Catalog.pm module reads this file and derives schema
 *	  information.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_ATTRIBUTE_H
#define PG_ATTRIBUTE_H

#include "catalog/genbki.h"
#include "catalog/pg_attribute_d.h" /* IWYU pragma: export */

/* ----------------
 *		pg_attribute definition.  cpp turns this into
 *		typedef struct FormData_pg_attribute
 *
 *		If you change the following, make sure you change the structs for
 *		system attributes in catalog/heap.c also.
 *		You may need to change catalog/genbki.pl as well.
 * ----------------
 */
CATALOG(pg_attribute,1249,AttributeRelationId) BKI_BOOTSTRAP BKI_ROWTYPE_OID(75,AttributeRelation_Rowtype_Id) BKI_SCHEMA_MACRO
{
	Oid			attrelid BKI_LOOKUP(pg_class);	/* OID of relation containing
												 * this attribute */
	NameData	attname;		/* name of attribute */

	/*
	 * atttypid is the OID of the instance in Catalog Class pg_type that
	 * defines the data type of this attribute (e.g. int4).  Information in
	 * that instance is redundant with the attlen, attbyval, and attalign
	 * attributes of this instance, so they had better match or Postgres will
	 * fail.  In an entry for a dropped column, this field is set to zero
	 * since the pg_type entry may no longer exist; but we rely on attlen,
	 * attbyval, and attalign to still tell us how large the values in the
	 * table are.
	 */
	Oid			atttypid BKI_LOOKUP_OPT(pg_type);

	/*
	 * attlen is a copy of the typlen field from pg_type for this attribute.
	 * See atttypid comments above.
	 */
	int16		attlen;

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
	int16		attnum;

	/*
	 * atttypmod records type-specific data supplied at table creation time
	 * (for example, the max length of a varchar field).  It is passed to
	 * type-specific input and output functions as the third argument. The
	 * value will generally be -1 for types that do not need typmod.
	 */
	int32		atttypmod BKI_DEFAULT(-1);

	/*
	 * attndims is the declared number of dimensions, if an array type,
	 * otherwise zero.
	 */
	int16		attndims;

	/*
	 * attbyval is a copy of the typbyval field from pg_type for this
	 * attribute.  See atttypid comments above.
	 */
	bool		attbyval;

	/*
	 * attalign is a copy of the typalign field from pg_type for this
	 * attribute.  See atttypid comments above.
	 */
	char		attalign;

	/*----------
	 * attstorage tells for VARLENA attributes, what the heap access
	 * methods can do to it if a given tuple doesn't fit into a page.
	 * Possible values are as for pg_type.typstorage (see TYPSTORAGE macros).
	 *----------
	 */
	char		attstorage;

	/*
	 * attcompression sets the current compression method of the attribute.
	 * Typically this is InvalidCompressionMethod ('\0') to specify use of the
	 * current default setting (see default_toast_compression).  Otherwise,
	 * 'p' selects pglz compression, while 'l' selects LZ4 compression.
	 * However, this field is ignored whenever attstorage does not allow
	 * compression.
	 */
	char		attcompression BKI_DEFAULT('\0');

	/*
	 * Whether a (possibly invalid) not-null constraint exists for the column
	 */
	bool		attnotnull;

	/* Has DEFAULT value or not */
	bool		atthasdef BKI_DEFAULT(f);

	/* Has a missing value or not */
	bool		atthasmissing BKI_DEFAULT(f);

	/* One of the ATTRIBUTE_IDENTITY_* constants below, or '\0' */
	char		attidentity BKI_DEFAULT('\0');

	/* One of the ATTRIBUTE_GENERATED_* constants below, or '\0' */
	char		attgenerated BKI_DEFAULT('\0');

	/* Is dropped (ie, logically invisible) or not */
	bool		attisdropped BKI_DEFAULT(f);

	/*
	 * This flag specifies whether this column has ever had a local
	 * definition.  It is set for normal non-inherited columns, but also for
	 * columns that are inherited from parents if also explicitly listed in
	 * CREATE TABLE INHERITS.  It is also set when inheritance is removed from
	 * a table with ALTER TABLE NO INHERIT.  If the flag is set, the column is
	 * not dropped by a parent's DROP COLUMN even if this causes the column's
	 * attinhcount to become zero.
	 */
	bool		attislocal BKI_DEFAULT(t);

	/* Number of times inherited from direct parent relation(s) */
	int16		attinhcount BKI_DEFAULT(0);

	/* attribute's collation, if any */
	Oid			attcollation BKI_LOOKUP_OPT(pg_collation);

#ifdef CATALOG_VARLEN			/* variable-length/nullable fields start here */
	/* NOTE: The following fields are not present in tuple descriptors. */

	/*
	 * attstattarget is the target number of statistics datapoints to collect
	 * during VACUUM ANALYZE of this column.  A zero here indicates that we do
	 * not wish to collect any stats about this column. A null value here
	 * indicates that no value has been explicitly set for this column, so
	 * ANALYZE should use the default setting.
	 *
	 * int16 is sufficient for the current max value (MAX_STATISTICS_TARGET).
	 */
	int16		attstattarget BKI_DEFAULT(_null_) BKI_FORCE_NULL;

	/* Column-level access permissions */
	aclitem		attacl[1] BKI_DEFAULT(_null_);

	/* Column-level options */
	text		attoptions[1] BKI_DEFAULT(_null_);

	/* Column-level FDW options */
	text		attfdwoptions[1] BKI_DEFAULT(_null_);

	/*
	 * Missing value for added columns. This is a one element array which lets
	 * us store a value of the attribute type here.
	 */
	anyarray	attmissingval BKI_DEFAULT(_null_);
#endif
} FormData_pg_attribute;

/*
 * ATTRIBUTE_FIXED_PART_SIZE is the size of the fixed-layout,
 * guaranteed-not-null part of a pg_attribute row.  This is in fact as much
 * of the row as gets copied into tuple descriptors, so don't expect you
 * can access the variable-length fields except in a real tuple!
 */
#define ATTRIBUTE_FIXED_PART_SIZE \
	(offsetof(FormData_pg_attribute,attcollation) + sizeof(Oid))

/* ----------------
 *		Form_pg_attribute corresponds to a pointer to a tuple with
 *		the format of pg_attribute relation.
 * ----------------
 */
typedef FormData_pg_attribute *Form_pg_attribute;

/*
 * FormExtraData_pg_attribute contains (some of) the fields that are not in
 * FormData_pg_attribute because they are excluded by CATALOG_VARLEN.  It is
 * meant to be used by DDL code so that the combination of
 * FormData_pg_attribute (often via tuple descriptor) and
 * FormExtraData_pg_attribute can be used to pass around all the information
 * about an attribute.  Fields can be included here as needed.
 */
typedef struct FormExtraData_pg_attribute
{
	NullableDatum attstattarget;
	NullableDatum attoptions;
} FormExtraData_pg_attribute;

DECLARE_UNIQUE_INDEX(pg_attribute_relid_attnam_index, 2658, AttributeRelidNameIndexId, pg_attribute, btree(attrelid oid_ops, attname name_ops));
DECLARE_UNIQUE_INDEX_PKEY(pg_attribute_relid_attnum_index, 2659, AttributeRelidNumIndexId, pg_attribute, btree(attrelid oid_ops, attnum int2_ops));

MAKE_SYSCACHE(ATTNAME, pg_attribute_relid_attnam_index, 32);
MAKE_SYSCACHE(ATTNUM, pg_attribute_relid_attnum_index, 128);

#ifdef EXPOSE_TO_CLIENT_CODE

#define		  ATTRIBUTE_IDENTITY_ALWAYS		'a'
#define		  ATTRIBUTE_IDENTITY_BY_DEFAULT 'd'

#define		  ATTRIBUTE_GENERATED_STORED	's'
#define		  ATTRIBUTE_GENERATED_VIRTUAL	'v'

#endif							/* EXPOSE_TO_CLIENT_CODE */

#endif							/* PG_ATTRIBUTE_H */
