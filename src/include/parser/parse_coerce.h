/*-------------------------------------------------------------------------
 *
 * parse_coerce.h
 *
 *	Routines for type coercion.
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: parse_coerce.h,v 1.22 2000/04/12 17:16:45 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PARSE_COERCE_H
#define PARSE_COERCE_H

#include "catalog/pg_type.h"
#include "parser/parse_node.h"

typedef enum CATEGORY
{
	INVALID_TYPE,
	UNKNOWN_TYPE,
	BOOLEAN_TYPE,
	STRING_TYPE,
	NUMERIC_TYPE,
	DATETIME_TYPE,
	TIMESPAN_TYPE,
	GEOMETRIC_TYPE,
	NETWORK_TYPE,
	USER_TYPE,
	MIXED_TYPE
} CATEGORY;


/* IS_BUILTIN_TYPE()
 * Check for types which are in the core distribution.
 * The built-in types can have more explicit support for type coersion, etc,
 *	since we know apriori how they should behave.
 * - thomas 1998-05-13
 */
#define IS_BUILTIN_TYPE(t) \
		  (((t) == OIDOID) \
		|| ((t) == BOOLOID) \
		|| ((t) == BPCHAROID) \
		|| ((t) == VARCHAROID) \
		|| ((t) == TEXTOID) \
		|| ((t) == INT4OID) \
		|| ((t) == INT8OID) \
		|| ((t) == FLOAT8OID) \
		|| ((t) == NUMERICOID) \
		|| ((t) == TIMESTAMPOID) \
		|| ((t) == INTERVALOID) \
		|| ((t) == ABSTIMEOID) \
		|| ((t) == RELTIMEOID) \
		|| ((t) == DATEOID) \
		|| ((t) == TIMEOID) \
		|| ((t) == TIMETZOID) \
		|| ((t) == CHAROID) \
		|| ((t) == NAMEOID) \
		|| ((t) == CASHOID) \
		|| ((t) == POINTOID) \
		|| ((t) == LSEGOID) \
		|| ((t) == LINEOID) \
		|| ((t) == BOXOID) \
		|| ((t) == PATHOID) \
		|| ((t) == POLYGONOID) \
		|| ((t) == CIRCLEOID) \
		|| ((t) == INETOID) \
		|| ((t) == CIDROID) )


/* IS_BINARY_COMPATIBLE()
 * Check for types with the same underlying binary representation.
 * This allows us to cheat and directly exchange values without
 *	going through the trouble of calling a conversion function.
 * Remove equivalencing of FLOAT8 and TIMESTAMP. They really are not
 *	close enough in behavior, with the TIMESTAMP reserved values
 *	and special formatting. - thomas 1999-01-24
 */
#define IS_BINARY_COMPATIBLE(a,b) \
		  (((a) == BPCHAROID && (b) == TEXTOID) \
		|| ((a) == BPCHAROID && (b) == VARCHAROID) \
		|| ((a) == VARCHAROID && (b) == TEXTOID) \
		|| ((a) == VARCHAROID && (b) == BPCHAROID) \
		|| ((a) == TEXTOID && (b) == BPCHAROID) \
		|| ((a) == TEXTOID && (b) == VARCHAROID) \
		|| ((a) == OIDOID && (b) == INT4OID) \
		|| ((a) == OIDOID && (b) == REGPROCOID) \
		|| ((a) == INT4OID && (b) == OIDOID) \
		|| ((a) == INT4OID && (b) == REGPROCOID) \
		|| ((a) == REGPROCOID && (b) == OIDOID) \
		|| ((a) == REGPROCOID && (b) == INT4OID) \
		|| ((a) == ABSTIMEOID && (b) == INT4OID) \
		|| ((a) == INT4OID && (b) == ABSTIMEOID) \
		|| ((a) == RELTIMEOID && (b) == INT4OID) \
		|| ((a) == INT4OID && (b) == RELTIMEOID) \
		|| ((a) == INETOID && (b) == CIDROID) \
		|| ((a) == CIDROID && (b) == INETOID))

/* IS_HIGHER_TYPE()
 * These types are the most general in each of the type categories.
 */
#define IS_HIGHER_TYPE(t) \
		  (((t) == TEXTOID) \
		|| ((t) == FLOAT8OID) \
		|| ((t) == INTERVALOID) \
		|| ((t) == TIMESTAMPOID) \
		|| ((t) == POLYGONOID) \
		|| ((t) == INETOID) )

/* IS_HIGHEST_TYPE()
 * These types are the most general in each of the type categories.
 * Since interval and timestamp overload so many functions, let's
 *	give timestamp the preference.
 * Since text is a generic string type let's leave it out too.
 */
#define IS_HIGHEST_TYPE(t) \
		  (((t) == FLOAT8OID) \
		|| ((t) == TIMESTAMPOID) \
		|| ((t) == INTERVALOID))


extern bool IsPreferredType(CATEGORY category, Oid type);
extern CATEGORY TypeCategory(Oid type);

extern bool can_coerce_type(int nargs, Oid *input_typeids, Oid *func_typeids);
extern Node *coerce_type(ParseState *pstate, Node *node, Oid inputTypeId,
			Oid targetTypeId, int32 atttypmod);
extern Node *coerce_type_typmod(ParseState *pstate, Node *node,
				   Oid targetTypeId, int32 atttypmod);

#endif	 /* PARSE_COERCE_H */
