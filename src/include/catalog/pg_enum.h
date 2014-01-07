/*-------------------------------------------------------------------------
 *
 * pg_enum.h
 *	  definition of the system "enum" relation (pg_enum)
 *	  along with the relation's initial contents.
 *
 *
 * Copyright (c) 2006-2014, PostgreSQL Global Development Group
 *
 * src/include/catalog/pg_enum.h
 *
 * NOTES
 *	  the genbki.pl script reads this file and generates .bki
 *	  information from the DATA() statements.
 *
 *	  XXX do NOT break up DATA() statements into multiple lines!
 *		  the scripts are not as smart as you might think...
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_ENUM_H
#define PG_ENUM_H

#include "catalog/genbki.h"
#include "nodes/pg_list.h"

/* ----------------
 *		pg_enum definition.  cpp turns this into
 *		typedef struct FormData_pg_enum
 * ----------------
 */
#define EnumRelationId	3501

CATALOG(pg_enum,3501)
{
	Oid			enumtypid;		/* OID of owning enum type */
	float4		enumsortorder;	/* sort position of this enum value */
	NameData	enumlabel;		/* text representation of enum value */
} FormData_pg_enum;

/* ----------------
 *		Form_pg_enum corresponds to a pointer to a tuple with
 *		the format of pg_enum relation.
 * ----------------
 */
typedef FormData_pg_enum *Form_pg_enum;

/* ----------------
 *		compiler constants for pg_enum
 * ----------------
 */
#define Natts_pg_enum					3
#define Anum_pg_enum_enumtypid			1
#define Anum_pg_enum_enumsortorder		2
#define Anum_pg_enum_enumlabel			3

/* ----------------
 *		pg_enum has no initial contents
 * ----------------
 */

/*
 * prototypes for functions in pg_enum.c
 */
extern void EnumValuesCreate(Oid enumTypeOid, List *vals);
extern void EnumValuesDelete(Oid enumTypeOid);
extern void AddEnumLabel(Oid enumTypeOid, const char *newVal,
			 const char *neighbor, bool newValIsAfter,
			 bool skipIfExists);

#endif   /* PG_ENUM_H */
