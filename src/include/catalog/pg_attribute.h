/*-------------------------------------------------------------------------
 *
 * pg_attribute.h--
 *	  definition of the system "attribute" relation (pg_attribute)
 *	  along with the relation's initial contents.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: pg_attribute.h,v 1.19 1997/11/16 04:36:32 momjian Exp $
 *
 * NOTES
 *	  the genbki.sh script reads this file and generates .bki
 *	  information from the DATA() statements.
 *
 *	  utils/cache/relcache.c requires some hard-coded tuple descriptors
 *	  for some of the system catalogs so if the schema for any of
 *	  these changes, be sure and change the appropriate Schema_xxx
 *	  macros!  -cim 2/5/91
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_ATTRIBUTE_H
#define PG_ATTRIBUTE_H

/* ----------------
 *		postgres.h contains the system type definintions and the
 *		CATALOG(), BOOTSTRAP and DATA() sugar words so this file
 *		can be read by both genbki.sh and the C compiler.
 * ----------------
 */

/* ----------------
 *		pg_attribute definition.  cpp turns this into
 *		typedef struct FormData_pg_attribute
 *
 *		If you change the following, make sure you change the structs for
 *		system attributes in heap.c and index.c also.
 * ----------------
 */
CATALOG(pg_attribute) BOOTSTRAP
{
	Oid			attrelid;
	NameData	attname;
	Oid			atttypid;

	/*
	 * atttypid is the OID of the instance in Catalog Class pg_type that
	 * defines the data type of this attribute (e.g. int4).  Information
	 * in that instance is redundant with the attlen, attbyval, and
	 * attalign attributes of this instance, so they had better match or
	 * Postgres will fail.
	 */
	float4		attdisbursion;
	int2		attlen;

	/*
	 * attlen is a copy of the typlen field from pg_type for this
	 * attribute.  See atttypid above.	See struct TypeTupleFormData for
	 * definition.
	 */
	int2		attnum;

	/*
	 * attnum is the "attribute number" for the attribute:	A value that
	 * uniquely identifies this attribute within its class. For user
	 * attributes, Attribute numbers are greater than 0 and not greater
	 * than the number of attributes in the class. I.e. if the Class
	 * pg_class says that Class XYZ has 10 attributes, then the user
	 * attribute numbers in Class pg_attribute must be 1-10.
	 *
	 * System attributes have attribute numbers less than 0 that are unique
	 * within the class, but not constrained to any particular range.
	 *
	 * Note that (attnum - 1) is often used as the index to an array.
	 */
	int4		attnelems;
	int4		attcacheoff;

	/*
	 * fastgetattr() uses attcacheoff to cache byte offsets of attributes
	 * in heap tuples.	The data actually stored in pg_attribute (-1)
	 * indicates no cached value.  But when we copy these tuples into a
	 * tuple descriptor, we may then update attcacheoff in the copies.
	 * This speeds up the attribute walking process.
	 */
	bool		attbyval;

	/*
	 * attbyval is a copy of the typbyval field from pg_type for this
	 * attribute.  See atttypid above.	See struct TypeTupleFormData for
	 * definition.
	 */
	bool		attisset;
	char		attalign;

	/*
	 * attalign is a copy of the typalign field from pg_type for this
	 * attribute.  See atttypid above.	See struct TypeTupleFormData for
	 * definition.
	 */
	bool		attnotnull;

	/* This flag represents the "NOT NULL" constraint */
	bool		atthasdef;

	/* Has DEFAULT value or not */
} FormData_pg_attribute;

/*
 * someone should figure out how to do this properly. (The problem is
 * the size of the C struct is not the same as the size of the tuple.)
 */
#define ATTRIBUTE_TUPLE_SIZE \
	(offsetof(FormData_pg_attribute,atthasdef) + sizeof(char))

/* ----------------
 *		Form_pg_attribute corresponds to a pointer to a tuple with
 *		the format of pg_attribute relation.
 * ----------------
 */
typedef FormData_pg_attribute *AttributeTupleForm;

/* ----------------
 *		compiler constants for pg_attribute
 * ----------------
 */

#define Natts_pg_attribute				13
#define Anum_pg_attribute_attrelid		1
#define Anum_pg_attribute_attname		2
#define Anum_pg_attribute_atttypid		3

#define Anum_pg_attribute_attdisbursion 4

#define Anum_pg_attribute_attlen		5
#define Anum_pg_attribute_attnum		6



#define Anum_pg_attribute_attnelems		7
#define Anum_pg_attribute_attcacheoff	8
#define Anum_pg_attribute_attbyval		9
#define Anum_pg_attribute_attisset		10
#define Anum_pg_attribute_attalign		11
#define Anum_pg_attribute_attnotnull	12
#define Anum_pg_attribute_atthasdef		13


/* ----------------
 *		SCHEMA_ macros for declaring hardcoded tuple descriptors.
 *		these are used in utils/cache/relcache.c
 * ----------------
#define SCHEMA_NAME(x) CppConcat(Name_,x)
#define SCHEMA_DESC(x) CppConcat(Desc_,x)
#define SCHEMA_NATTS(x) CppConcat(Natts_,x)
#define SCHEMA_DEF(x) \
	FormData_pg_attribute \
	SCHEMA_DESC(x) [ SCHEMA_NATTS(x) ] = \
	{ \
		CppConcat(Schema_,x) \
	}
 */

/* ----------------
 *		initial contents of pg_attribute
 * ----------------
 */

/* ----------------
 *		pg_type schema
 * ----------------
 */
#define Schema_pg_type \
{ 1247l, {"typname"},	   19l, 0l, NAMEDATALEN,  1, 0l, -1l, '\0', '\0', 'i', '\0', '\0' }, \
{ 1247l, {"typowner"},	   26l, 0l,  4,  2, 0l, -1l, '\001', '\0', 'i', '\0', '\0' }, \
{ 1247l, {"typlen"},	   21l, 0l,  2,  3, 0l, -1l, '\001', '\0', 's', '\0', '\0' }, \
{ 1247l, {"typprtlen"},    21l, 0l,  2,  4, 0l, -1l, '\001', '\0', 's', '\0', '\0' }, \
{ 1247l, {"typbyval"},	   16l, 0l,  1,  5, 0l, -1l, '\001', '\0', 'c', '\0', '\0' }, \
{ 1247l, {"typtype"},	   18l, 0l,  1,  6, 0l, -1l, '\001', '\0', 'c', '\0', '\0' }, \
{ 1247l, {"typisdefined"}, 16l, 0l,  1,  7, 0l, -1l, '\001', '\0', 'c', '\0', '\0' }, \
{ 1247l, {"typdelim"},	   18l, 0l,  1,  8, 0l, -1l, '\001', '\0', 'c', '\0', '\0' }, \
{ 1247l, {"typrelid"},	   26l, 0l,  4,  9, 0l, -1l, '\001', '\0', 'i', '\0', '\0' }, \
{ 1247l, {"typelem"},	   26l, 0l,  4, 10, 0l, -1l, '\001', '\0', 'i', '\0', '\0' }, \
{ 1247l, {"typinput"},	   24l, 0l,  4, 11, 0l, -1l, '\001', '\0', 'i', '\0', '\0' }, \
{ 1247l, {"typoutput"},    24l, 0l,  4, 12, 0l, -1l, '\001', '\0', 'i', '\0', '\0' }, \
{ 1247l, {"typreceive"},   24l, 0l,  4, 13, 0l, -1l, '\001', '\0', 'i', '\0', '\0' }, \
{ 1247l, {"typsend"},	   24l, 0l,  4, 14, 0l, -1l, '\001', '\0', 'i', '\0', '\0' }, \
{ 1247l, {"typalign"},	   18l, 0l,  1, 15, 0l, -1l, '\001', '\0', 'c', '\0', '\0' }, \
{ 1247l, {"typdefault"},   25l, 0l, -1, 16, 0l, -1l, '\0'  , '\0', 'i', '\0', '\0' }

DATA(insert OID = 0 ( 1247 typname			19 0 NAMEDATALEN   1 0 -1 f f i f f));
DATA(insert OID = 0 ( 1247 typowner			26 0  4   2 0 -1 t f i f f));
DATA(insert OID = 0 ( 1247 typlen			21 0  2   3 0 -1 t f s f f));
DATA(insert OID = 0 ( 1247 typprtlen		21 0  2   4 0 -1 t f s f f));
DATA(insert OID = 0 ( 1247 typbyval			16 0  1   5 0 -1 t f c f f));
DATA(insert OID = 0 ( 1247 typtype			18 0  1   6 0 -1 t f c f f));
DATA(insert OID = 0 ( 1247 typisdefined		16 0  1   7 0 -1 t f c f f));
DATA(insert OID = 0 ( 1247 typdelim			18 0  1   8 0 -1 t f c f f));
DATA(insert OID = 0 ( 1247 typrelid			26 0  4   9 0 -1 t f i f f));
DATA(insert OID = 0 ( 1247 typelem			26 0  4  10 0 -1 t f i f f));
DATA(insert OID = 0 ( 1247 typinput			24 0  4  11 0 -1 t f i f f));
DATA(insert OID = 0 ( 1247 typoutput		24 0  4  12 0 -1 t f i f f));
DATA(insert OID = 0 ( 1247 typreceive		24 0  4  13 0 -1 t f i f f));
DATA(insert OID = 0 ( 1247 typsend			24 0  4  14 0 -1 t f i f f));
DATA(insert OID = 0 ( 1247 typalign			18 0  1  15 0 -1 t f c f f));
DATA(insert OID = 0 ( 1247 typdefault		25 0 -1  16 0 -1 f f i f f));
DATA(insert OID = 0 ( 1247 ctid				27 0  6  -1 0 -1 f f i f f));
DATA(insert OID = 0 ( 1247 oid				26 0  4  -2 0 -1 t f i f f));
DATA(insert OID = 0 ( 1247 xmin				28 0  4  -3 0 -1 f f i f f));
DATA(insert OID = 0 ( 1247 cmin				29 0  4  -4 0 -1 t f i f f));
DATA(insert OID = 0 ( 1247 xmax				28 0  4  -5 0 -1 f f i f f));
DATA(insert OID = 0 ( 1247 cmax				29 0  4  -6 0 -1 t f i f f));

/* ----------------
 *		pg_database
 * ----------------
 */
DATA(insert OID = 0 ( 1262 datname			19 0 NAMEDATALEN   1 0 -1 f f i f f));
DATA(insert OID = 0 ( 1262 datdba			23 0  4   2 0 -1 t f s f f));
DATA(insert OID = 0 ( 1262 datpath			25 0 -1   3 0 -1 f f i f f));
DATA(insert OID = 0 ( 1262 ctid				27 0  6  -1 0 -1 f f i f f));
DATA(insert OID = 0 ( 1262 oid				26 0  4  -2 0 -1 t f i f f));
DATA(insert OID = 0 ( 1262 xmin				28 0  4  -3 0 -1 f f i f f));
DATA(insert OID = 0 ( 1262 cmin				29 0  4  -4 0 -1 t f i f f));
DATA(insert OID = 0 ( 1262 xmax				28 0  4  -5 0 -1 f f i f f));
DATA(insert OID = 0 ( 1262 cmax				29 0  4  -6 0 -1 t f i f f));

/* ----------------
 *		pg_proc
 * ----------------
 */
#define Schema_pg_proc \
{ 1255l, {"proname"},		19l, 0l, NAMEDATALEN,  1, 0l, -1l, '\0', '\0', 'i', '\0', '\0' }, \
{ 1255l, {"proowner"},		26l, 0l,  4,  2, 0l, -1l, '\001', '\0', 'i', '\0', '\0' }, \
{ 1255l, {"prolang"},		26l, 0l,  4,  3, 0l, -1l, '\001', '\0', 'i', '\0', '\0' }, \
{ 1255l, {"proisinh"},		16l, 0l,  1,  4, 0l, -1l, '\001', '\0', 'c', '\0', '\0' }, \
{ 1255l, {"proistrusted"},	16l, 0l,  1,  5, 0l, -1l, '\001', '\0', 'c', '\0', '\0' }, \
{ 1255l, {"proiscachable"}, 16l, 0l,  1,  6, 0l, -1l, '\001', '\0', 'c', '\0', '\0' }, \
{ 1255l, {"pronargs"},		21l, 0l,  2,  7, 0l, -1l, '\001', '\0', 's', '\0', '\0' }, \
{ 1255l, {"proretset"},		16l, 0l,  1,  8, 0l, -1l, '\001', '\0', 'c', '\0', '\0' }, \
{ 1255l, {"prorettype"},	26l, 0l,  4,  9, 0l, -1l, '\001', '\0', 'i', '\0', '\0' }, \
{ 1255l, {"proargtypes"},	30l, 0l, 32, 10, 0l, -1l,	'\0', '\0', 'i', '\0', '\0' }, \
{ 1255l, {"probyte_pct"},	23l, 0l,  4, 11, 0l, -1l, '\001', '\0', 'i', '\0', '\0' }, \
{ 1255l, {"properbyte_cpu"},   23l, 0l,  4, 12, 0l, -1l, '\001', '\0', 'i', '\0', '\0' }, \
{ 1255l, {"propercall_cpu"},   23l, 0l,  4, 13, 0l, -1l, '\001', '\0', 'i', '\0', '\0' }, \
{ 1255l, {"prooutin_ratio"},   23l, 0l,  4, 14, 0l, -1l, '\001', '\0', 'i', '\0', '\0' }, \
{ 1255l, {"prosrc"},		25l, 0l, -1,  15,	0l, -1l, '\0', '\0', 'i', '\0', '\0' }, \
{ 1255l, {"probin"},		17l, 0l, -1,  16,	0l, -1l, '\0', '\0', 'i', '\0', '\0' }

DATA(insert OID = 0 ( 1255 proname			19 0 NAMEDATALEN   1 0 -1 f f i f f));
DATA(insert OID = 0 ( 1255 proowner			26 0  4   2 0 -1 t f i f f));
DATA(insert OID = 0 ( 1255 prolang			26 0  4   3 0 -1 t f i f f));
DATA(insert OID = 0 ( 1255 proisinh			16 0  1   4 0 -1 t f c f f));
DATA(insert OID = 0 ( 1255 proistrusted		16 0  1   5 0 -1 t f c f f));
DATA(insert OID = 0 ( 1255 proiscachable	16 0  1   6 0 -1 t f c f f));
DATA(insert OID = 0 ( 1255 pronargs			21 0  2   7 0 -1 t f s f f));
DATA(insert OID = 0 ( 1255 proretset		16 0  1   8 0 -1 t f c f f));
DATA(insert OID = 0 ( 1255 prorettype		26 0  4   9 0 -1 t f i f f));
DATA(insert OID = 0 ( 1255 proargtypes		30 0 32  10 0 -1 f f i f f));
DATA(insert OID = 0 ( 1255 probyte_pct		23 0  4  11 0 -1 t f i f f));
DATA(insert OID = 0 ( 1255 properbyte_cpu	23 0  4  12 0 -1 t f i f f));
DATA(insert OID = 0 ( 1255 propercall_cpu	23 0  4  13 0 -1 t f i f f));
DATA(insert OID = 0 ( 1255 prooutin_ratio	23 0  4  14 0 -1 t f i f f));
DATA(insert OID = 0 ( 1255 prosrc			25 0 -1  15 0 -1 f f i f f));
DATA(insert OID = 0 ( 1255 probin			17 0 -1  16 0 -1 f f i f f));
DATA(insert OID = 0 ( 1255 ctid				27 0  6  -1 0 -1 f f i f f));
DATA(insert OID = 0 ( 1255 oid				26 0  4  -2 0 -1 t f i f f));
DATA(insert OID = 0 ( 1255 xmin				28 0  4  -3 0 -1 f f i f f));
DATA(insert OID = 0 ( 1255 cmin				29 0  4  -4 0 -1 t f i f f));
DATA(insert OID = 0 ( 1255 xmax				28 0  4  -5 0 -1 f f i f f));
DATA(insert OID = 0 ( 1255 cmax				29 0  4  -6 0 -1 t f i f f));

/* ----------------
 *		pg_user
 * ----------------
 */
DATA(insert OID = 0 ( 1260 usename			19 0 NAMEDATALEN   1 0 -1 f f i f f));
DATA(insert OID = 0 ( 1260 usesysid			23 0  4   2 0 -1 t f s f f));
DATA(insert OID = 0 ( 1260 usecreatedb		16 0  1   3 0 -1 t f c f f));
DATA(insert OID = 0 ( 1260 usetrace			16 0  1   4 0 -1 t f c f f));
DATA(insert OID = 0 ( 1260 usesuper			16 0  1   5 0 -1 t f c f f));
DATA(insert OID = 0 ( 1260 usecatupd		16 0  1   6 0 -1 t f c f f));
DATA(insert OID = 0 ( 1260 ctid				27 0  6  -1 0 -1 f f i f f));
DATA(insert OID = 0 ( 1260 oid				26 0  4  -2 0 -1 t f i f f));
DATA(insert OID = 0 ( 1260 xmin				28 0  4  -3 0 -1 f f i f f));
DATA(insert OID = 0 ( 1260 cmin				29 0  4  -4 0 -1 t f i f f));
DATA(insert OID = 0 ( 1260 xmax				28 0  4  -5 0 -1 f f i f f));
DATA(insert OID = 0 ( 1260 cmax				29 0  4  -6 0 -1 t f i f f));

/* ----------------
 *		pg_group
 * ----------------
 */
DATA(insert OID = 0 ( 1261 groname			19 0 NAMEDATALEN  1 0 -1 f f i f f));
DATA(insert OID = 0 ( 1261 grosysid			23 0  4   2 0 -1 t f s f f));
DATA(insert OID = 0 ( 1261 grolist		  1007 0 -1   3 0 -1 f f i f f));
DATA(insert OID = 0 ( 1261 ctid				27 0  6  -1 0 -1 f f i f f));
DATA(insert OID = 0 ( 1261 oid				26 0  4  -2 0 -1 t f i f f));
DATA(insert OID = 0 ( 1261 xmin				28 0  4  -3 0 -1 f f i f f));
DATA(insert OID = 0 ( 1261 cmin				29 0  4  -4 0 -1 t f i f f));
DATA(insert OID = 0 ( 1261 xmax				28 0  4  -5 0 -1 f f i f f));
DATA(insert OID = 0 ( 1261 cmax				29 0  4  -6 0 -1 t f i f f));

/* ----------------
 *		pg_attribute
 * ----------------
 */
#define Schema_pg_attribute \
{ 1249l, {"attrelid"},	  26l, 0l,	4,	1, 0l, -1l, '\001', '\0', 'i', '\0', '\0' }, \
{ 1249l, {"attname"},	  19l, 0l, NAMEDATALEN,  2, 0l, -1l, '\0', '\0', 'i', '\0', '\0' }, \
{ 1249l, {"atttypid"},	  26l, 0l,	4,	3, 0l, -1l, '\001', '\0', 'i', '\0', '\0' }, \
{ 1249l, {"attdisbursion"},   700l, 0l,  4,  4, 0l, -1l, '\0', '\0', 'i', '\0', '\0' }, \
{ 1249l, {"attlen"},	  21l, 0l,	2,	5, 0l, -1l, '\001', '\0', 's', '\0', '\0' }, \
{ 1249l, {"attnum"},	  21l, 0l,	2,	6, 0l, -1l, '\001', '\0', 's', '\0', '\0' }, \
{ 1249l, {"attnelems"},   23l, 0l,	4,	7, 0l, -1l, '\001', '\0', 'i', '\0', '\0' }, \
{ 1249l, {"attcacheoff"}, 23l, 0l,	4,	8, 0l, -1l, '\001', '\0', 'i', '\0', '\0' }, \
{ 1249l, {"attbyval"},	  16l, 0l,	1,	9, 0l, -1l, '\001', '\0', 'c', '\0', '\0' }, \
{ 1249l, {"attisset"},	  16l, 0l,	1, 10, 0l, -1l, '\001', '\0', 'c', '\0', '\0' }, \
{ 1249l, {"attalign"},	  18l, 0l,	1, 11, 0l, -1l, '\001', '\0', 'c', '\0', '\0' }, \
{ 12491, {"attnotnull"},  16l, 0l,	1, 12, 0l, -1l, '\001', '\0', 'c', '\0', '\0' }, \
{ 12491, {"atthasdef"},   16l, 0l,	1, 13, 0l, -1l, '\001', '\0', 'c', '\0', '\0' }

DATA(insert OID = 0 ( 1249 attrelid			26 0  4   1 0 -1 t f i f f));
DATA(insert OID = 0 ( 1249 attname			19 0 NAMEDATALEN  2 0 -1 f f i f f));
DATA(insert OID = 0 ( 1249 atttypid			26 0  4   3 0 -1 t f i f f));
DATA(insert OID = 0 ( 1249 attdisbursion   700 0  4   4 0 -1 f f i f f));
DATA(insert OID = 0 ( 1249 attlen			21 0  2   5 0 -1 t f s f f));
DATA(insert OID = 0 ( 1249 attnum			21 0  2   6 0 -1 t f s f f));
DATA(insert OID = 0 ( 1249 attnelems		23 0  4   7 0 -1 t f i f f));
DATA(insert OID = 0 ( 1249 attcacheoff		23 0  4   8 0 -1 t f i f f));
DATA(insert OID = 0 ( 1249 attbyval			16 0  1   9 0 -1 t f c f f));
DATA(insert OID = 0 ( 1249 attisset			16 0  1  10 0 -1 t f c f f));
DATA(insert OID = 0 ( 1249 attalign			18 0  1  11 0 -1 t f c f f));
DATA(insert OID = 0 ( 1249 attnotnull		16 0  1  12 0 -1 t f c f f));
DATA(insert OID = 0 ( 1249 atthasdef		16 0  1  13 0 -1 t f c f f));
DATA(insert OID = 0 ( 1249 ctid				27 0  6  -1 0 -1 f f i f f));
DATA(insert OID = 0 ( 1249 oid				26 0  4  -2 0 -1 t f i f f));
DATA(insert OID = 0 ( 1249 xmin				28 0  4  -3 0 -1 f f i f f));
DATA(insert OID = 0 ( 1249 cmin				29 0  4  -4 0 -1 t f i f f));
DATA(insert OID = 0 ( 1249 xmax				28 0  4  -5 0 -1 f f i f f));
DATA(insert OID = 0 ( 1249 cmax				29 0  4  -6 0 -1 t f i f f));

/* ----------------
 *		pg_class
 * ----------------
 */
#define Schema_pg_class \
{ 1259l, {"relname"},	   19l, 0l, NAMEDATALEN,  1, 0l, -1l, '\0', '\0', 'i', '\0', '\0' }, \
{ 1259l, {"reltype"},	   26l, 0l,  4,  2, 0l, -1l, '\001', '\0', 'i', '\0', '\0' }, \
{ 1259l, {"relowner"},	   26l, 0l,  4,  2, 0l, -1l, '\001', '\0', 'i', '\0', '\0' }, \
{ 1259l, {"relam"},		   26l, 0l,  4,  3, 0l, -1l, '\001', '\0', 'i', '\0', '\0' }, \
{ 1259l, {"relpages"},	   23,	0l,  4,  4, 0l, -1l, '\001', '\0', 'i', '\0', '\0' }, \
{ 1259l, {"reltuples"},    23,	0l,  4,  5, 0l, -1l, '\001', '\0', 'i', '\0', '\0' }, \
{ 1259l, {"relexpires"},   702, 0l,  4,  6, 0l, -1l, '\001', '\0', 'i', '\0', '\0' }, \
{ 1259l, {"relpreserved"}, 703, 0l,  4,  7, 0l, -1l, '\001', '\0', 'i', '\0', '\0' }, \
{ 1259l, {"relhasindex"},  16,	0l,  1,  8, 0l, -1l, '\001', '\0', 'c', '\0', '\0' }, \
{ 1259l, {"relisshared"},  16,	0l,  1,  9, 0l, -1l, '\001', '\0', 'c', '\0', '\0' }, \
{ 1259l, {"relkind"},	   18,	0l,  1, 10, 0l, -1l, '\001', '\0', 'c', '\0', '\0' }, \
{ 1259l, {"relarch"},	   18,	0l,  1, 11, 0l, -1l, '\001', '\0', 'c', '\0', '\0' }, \
{ 1259l, {"relnatts"},	   21,	0l,  2, 12, 0l, -1l, '\001', '\0', 's', '\0', '\0' }, \
{ 1259l, {"relsmgr"},	   210l, 0l, 2, 13, 0l, -1l, '\001', '\0', 's', '\0', '\0' }, \
{ 1259l, {"relchecks"},    21l, 0l,  2, 14, 0l, -1l, '\001', '\0', 's', '\0', '\0' }, \
{ 1259l, {"reltriggers"},  21l, 0l,  2, 15, 0l, -1l, '\001', '\0', 's', '\0', '\0' }, \
{ 1259l, {"relhasrules"},  16,	0l,  1, 16, 0l, -1l, '\001', '\0', 'c', '\0', '\0' }, \
{ 1259l, {"relacl"},	 1034l, 0l, -1, 17, 0l, -1l,   '\0', '\0', 'i', '\0', '\0' }

DATA(insert OID = 0 ( 1259 relname			19 0 NAMEDATALEN   1 0 -1 f f i f f));
DATA(insert OID = 0 ( 1259 reltype			26 0  4   2 0 -1 t f i f f));
DATA(insert OID = 0 ( 1259 relowner			26 0  4   2 0 -1 t f i f f));
DATA(insert OID = 0 ( 1259 relam			26 0  4   3 0 -1 t f i f f));
DATA(insert OID = 0 ( 1259 relpages			23 0  4   4 0 -1 t f i f f));
DATA(insert OID = 0 ( 1259 reltuples		23 0  4   5 0 -1 t f i f f));
DATA(insert OID = 0 ( 1259 relexpires	   702 0  4   6 0 -1 t f i f f));
DATA(insert OID = 0 ( 1259 relpreserved    702 0  4   7 0 -1 t f i f f));
DATA(insert OID = 0 ( 1259 relhasindex		16 0  1   8 0 -1 t f c f f));
DATA(insert OID = 0 ( 1259 relisshared		16 0  1   9 0 -1 t f c f f));
DATA(insert OID = 0 ( 1259 relkind			18 0  1  10 0 -1 t f c f f));
DATA(insert OID = 0 ( 1259 relarch			18 0  1  11 0 -1 t f c f f));
DATA(insert OID = 0 ( 1259 relnatts			21 0  2  12 0 -1 t f s f f));
DATA(insert OID = 0 ( 1259 relsmgr		   210 0  2  13 0 -1 t f s f f));
DATA(insert OID = 0 ( 1259 relchecks		21 0  2  14 0 -1 t f s f f));
DATA(insert OID = 0 ( 1259 reltriggers		21 0  2  15 0 -1 t f s f f));
DATA(insert OID = 0 ( 1259 relhasrules		16 0  1  16 0 -1 t f c f f));
DATA(insert OID = 0 ( 1259 relacl		  1034 0 -1  17 0 -1 f f i f f));
DATA(insert OID = 0 ( 1259 ctid				27 0  6  -1 0 -1 f f i f f));
DATA(insert OID = 0 ( 1259 oid				26 0  4  -2 0 -1 t f i f f));
DATA(insert OID = 0 ( 1259 xmin				28 0  4  -3 0 -1 f f i f f));
DATA(insert OID = 0 ( 1259 cmin				29 0  4  -4 0 -1 t f i f f));
DATA(insert OID = 0 ( 1259 xmax				28 0  4  -5 0 -1 f f i f f));
DATA(insert OID = 0 ( 1259 cmax				29 0  4  -6 0 -1 t f i f f));

/* ----------------
 *		pg_attrdef
 * ----------------
 */
DATA(insert OID = 0 ( 1215 adrelid			26 0  4   1 0 -1 t f i f f));
DATA(insert OID = 0 ( 1215 adnum			21 0  2   2 0 -1 t f s f f));
DATA(insert OID = 0 ( 1215 adbin			25 0 -1   3 0 -1 f f i f f));
DATA(insert OID = 0 ( 1215 adsrc			25 0 -1   4 0 -1 f f i f f));
DATA(insert OID = 0 ( 1215 ctid				27 0  6  -1 0 -1 f f i f f));
DATA(insert OID = 0 ( 1215 oid				26 0  4  -2 0 -1 t f i f f));
DATA(insert OID = 0 ( 1215 xmin				28 0  4  -3 0 -1 f f i f f));
DATA(insert OID = 0 ( 1215 cmin				29 0  4  -4 0 -1 t f i f f));
DATA(insert OID = 0 ( 1215 xmax				28 0  4  -5 0 -1 f f i f f));
DATA(insert OID = 0 ( 1215 cmax				29 0  4  -6 0 -1 t f i f f));

/* ----------------
 *		pg_relcheck
 * ----------------
 */
DATA(insert OID = 0 ( 1216 rcrelid			26 0  4   1 0 -1 t f i f f));
DATA(insert OID = 0 ( 1216 rcname			19 0  NAMEDATALEN  2 0 -1 f f i f f));
DATA(insert OID = 0 ( 1216 rcbin			25 0 -1   3 0 -1 f f i f f));
DATA(insert OID = 0 ( 1216 rcsrc			25 0 -1   4 0 -1 f f i f f));
DATA(insert OID = 0 ( 1216 ctid				27 0  6  -1 0 -1 f f i f f));
DATA(insert OID = 0 ( 1216 oid				26 0  4  -2 0 -1 t f i f f));
DATA(insert OID = 0 ( 1216 xmin				28 0  4  -3 0 -1 f f i f f));
DATA(insert OID = 0 ( 1216 cmin				29 0  4  -4 0 -1 t f i f f));
DATA(insert OID = 0 ( 1216 xmax				28 0  4  -5 0 -1 f f i f f));
DATA(insert OID = 0 ( 1216 cmax				29 0  4  -6 0 -1 t f i f f));

/* ----------------
 *		pg_trigger
 * ----------------
 */
DATA(insert OID = 0 ( 1219 tgrelid			26 0  4   1 0 -1 t f i f f));
DATA(insert OID = 0 ( 1219 tgname			19 0  NAMEDATALEN  2 0 -1 f f i f f));
DATA(insert OID = 0 ( 1219 tgfoid			26 0  4   3 0 -1 t f i f f));
DATA(insert OID = 0 ( 1219 tgtype			21 0  2   4 0 -1 t f s f f));
DATA(insert OID = 0 ( 1219 tgnargs			21 0  2   5 0 -1 t f s f f));
DATA(insert OID = 0 ( 1219 tgattr			22 0 16   6 0 -1 f f i f f));
DATA(insert OID = 0 ( 1219 tgargs			17 0 -1   7 0 -1 f f i f f));
DATA(insert OID = 0 ( 1219 ctid				27 0  6  -1 0 -1 f f i f f));
DATA(insert OID = 0 ( 1219 oid				26 0  4  -2 0 -1 t f i f f));
DATA(insert OID = 0 ( 1219 xmin				28 0  4  -3 0 -1 f f i f f));
DATA(insert OID = 0 ( 1219 cmin				29 0  4  -4 0 -1 t f i f f));
DATA(insert OID = 0 ( 1219 xmax				28 0  4  -5 0 -1 f f i f f));
DATA(insert OID = 0 ( 1219 cmax				29 0  4  -6 0 -1 t f i f f));

/* ----------------
 *		pg_variable - this relation is modified by special purpose access
 *				  method code.	The following is garbage but is needed
 *				  so that the reldesc code works properly.
 * ----------------
 */
#define Schema_pg_variable \
{ 1264l, {"varfoo"},  26l, 0l, 4, 1, 0l, -1l, '\001', '\0', 'i', '\0', '\0' }

DATA(insert OID = 0 ( 1264 varfoo			26 0  4   1 0 -1 t f i f f));

/* ----------------
 *		pg_log - this relation is modified by special purpose access
 *				  method code.	The following is garbage but is needed
 *				  so that the reldesc code works properly.
 * ----------------
 */
#define Schema_pg_log \
{ 1269l, {"logfoo"},  26l, 0l, 4, 1, 0l, -1l, '\001', '\0', 'i', '\0', '\0' }

DATA(insert OID = 0 ( 1269 logfoo			26 0  4   1 0 -1 t f i f f));

#endif							/* PG_ATTRIBUTE_H */
