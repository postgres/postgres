/*-------------------------------------------------------------------------
 *
 * pg_cast.h
 *	  definition of the "type casts" system catalog (pg_cast)
 *
 * As of Postgres 8.0, pg_cast describes not only type coercion functions
 * but also length coercion functions.
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_cast.h
 *
 * NOTES
 *	  The Catalog.pm module reads this file and derives schema
 *	  information.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_CAST_H
#define PG_CAST_H

#include "catalog/dependency.h"
#include "catalog/genbki.h"
#include "catalog/pg_cast_d.h"

/* ----------------
 *		pg_cast definition.  cpp turns this into
 *		typedef struct FormData_pg_cast
 * ----------------
 */
CATALOG(pg_cast,2605,CastRelationId)
{
	Oid			oid;			/* oid */

	/* source datatype for cast */
	Oid			castsource BKI_LOOKUP(pg_type);

	/* destination datatype for cast */
	Oid			casttarget BKI_LOOKUP(pg_type);

	/* cast function; 0 = binary coercible */
	Oid			castfunc BKI_LOOKUP_OPT(pg_proc);

	/* contexts in which cast can be used */
	char		castcontext;

	/* cast method */
	char		castmethod;
} FormData_pg_cast;

/* ----------------
 *		Form_pg_cast corresponds to a pointer to a tuple with
 *		the format of pg_cast relation.
 * ----------------
 */
typedef FormData_pg_cast *Form_pg_cast;

DECLARE_UNIQUE_INDEX_PKEY(pg_cast_oid_index, 2660, on pg_cast using btree(oid oid_ops));
#define CastOidIndexId	2660
DECLARE_UNIQUE_INDEX(pg_cast_source_target_index, 2661, on pg_cast using btree(castsource oid_ops, casttarget oid_ops));
#define CastSourceTargetIndexId  2661

#ifdef EXPOSE_TO_CLIENT_CODE

/*
 * The allowable values for pg_cast.castcontext are specified by this enum.
 * Since castcontext is stored as a "char", we use ASCII codes for human
 * convenience in reading the table.  Note that internally to the backend,
 * these values are converted to the CoercionContext enum (see primnodes.h),
 * which is defined to sort in a convenient order; the ASCII codes don't
 * have to sort in any special order.
 */

typedef enum CoercionCodes
{
	COERCION_CODE_IMPLICIT = 'i',	/* coercion in context of expression */
	COERCION_CODE_ASSIGNMENT = 'a', /* coercion in context of assignment */
	COERCION_CODE_EXPLICIT = 'e'	/* explicit cast operation */
} CoercionCodes;

/*
 * The allowable values for pg_cast.castmethod are specified by this enum.
 * Since castmethod is stored as a "char", we use ASCII codes for human
 * convenience in reading the table.
 */
typedef enum CoercionMethod
{
	COERCION_METHOD_FUNCTION = 'f', /* use a function */
	COERCION_METHOD_BINARY = 'b',	/* types are binary-compatible */
	COERCION_METHOD_INOUT = 'i' /* use input/output functions */
} CoercionMethod;

#endif							/* EXPOSE_TO_CLIENT_CODE */


extern ObjectAddress CastCreate(Oid sourcetypeid,
								Oid targettypeid,
								Oid funcid,
								char castcontext,
								char castmethod,
								DependencyType behavior);

#endif							/* PG_CAST_H */
