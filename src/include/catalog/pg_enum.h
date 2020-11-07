/*-------------------------------------------------------------------------
 *
 * pg_enum.h
 *	  definition of the "enum" system catalog (pg_enum)
 *
 *
 * Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_enum.h
 *
 * NOTES
 *	  The Catalog.pm module reads this file and derives schema
 *	  information.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_ENUM_H
#define PG_ENUM_H

#include "catalog/genbki.h"
#include "catalog/pg_enum_d.h"

#include "nodes/pg_list.h"

/* ----------------
 *		pg_enum definition.  cpp turns this into
 *		typedef struct FormData_pg_enum
 * ----------------
 */
CATALOG(pg_enum,3501,EnumRelationId)
{
	Oid			oid;			/* oid */
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

DECLARE_UNIQUE_INDEX(pg_enum_oid_index, 3502, on pg_enum using btree(oid oid_ops));
#define EnumOidIndexId	3502
DECLARE_UNIQUE_INDEX(pg_enum_typid_label_index, 3503, on pg_enum using btree(enumtypid oid_ops, enumlabel name_ops));
#define EnumTypIdLabelIndexId 3503
DECLARE_UNIQUE_INDEX(pg_enum_typid_sortorder_index, 3534, on pg_enum using btree(enumtypid oid_ops, enumsortorder float4_ops));
#define EnumTypIdSortOrderIndexId 3534

/*
 * prototypes for functions in pg_enum.c
 */
extern void EnumValuesCreate(Oid enumTypeOid, List *vals);
extern void EnumValuesDelete(Oid enumTypeOid);
extern void AddEnumLabel(Oid enumTypeOid, const char *newVal,
						 const char *neighbor, bool newValIsAfter,
						 bool skipIfExists);
extern void RenameEnumLabel(Oid enumTypeOid,
							const char *oldVal, const char *newVal);
extern bool EnumBlacklisted(Oid enum_id);
extern Size EstimateEnumBlacklistSpace(void);
extern void SerializeEnumBlacklist(void *space, Size size);
extern void RestoreEnumBlacklist(void *space);
extern void AtEOXact_Enum(void);

#endif							/* PG_ENUM_H */
