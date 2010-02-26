/*-------------------------------------------------------------------------
 *
 * pg_enum.h
 *	  definition of the system "enum" relation (pg_enum)
 *	  along with the relation's initial contents.
 *
 *
 * Copyright (c) 2006-2010, PostgreSQL Global Development Group
 *
 * $PostgreSQL: pgsql/src/include/catalog/pg_enum.h,v 1.9 2010/02/26 02:01:21 momjian Exp $
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
#define Natts_pg_enum					2
#define Anum_pg_enum_enumtypid			1
#define Anum_pg_enum_enumlabel			2

/* ----------------
 *		pg_enum has no initial contents
 * ----------------
 */

/*
 * prototypes for functions in pg_enum.c
 */
extern void EnumValuesCreate(Oid enumTypeOid, List *vals,
				 Oid binary_upgrade_next_pg_enum_oid);
extern void EnumValuesDelete(Oid enumTypeOid);

#endif   /* PG_ENUM_H */
