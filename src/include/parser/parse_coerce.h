/*-------------------------------------------------------------------------
 *
 * parse_coerce.h
 *
 *	Routines for type coercion.
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: parse_coerce.h,v 1.40 2002/03/19 02:18:24 momjian Exp $
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
	BITSTRING_TYPE,
	NUMERIC_TYPE,
	DATETIME_TYPE,
	TIMESPAN_TYPE,
	GEOMETRIC_TYPE,
	NETWORK_TYPE,
	USER_TYPE,
	MIXED_TYPE
} CATEGORY;


/* IS_BINARY_COMPATIBLE()
 * Check for types with the same underlying binary representation.
 * This allows us to cheat and directly exchange values without
 *	going through the trouble of calling a conversion function.
 *
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
		|| ((a) == CIDROID && (b) == INETOID) \
		|| ((a) == BITOID && (b) == VARBITOID) \
		|| ((a) == VARBITOID && (b) == BITOID))


extern bool IsPreferredType(CATEGORY category, Oid type);
extern CATEGORY TypeCategory(Oid type);

extern bool can_coerce_type(int nargs, Oid *input_typeids, Oid *func_typeids);
extern Node *coerce_type(ParseState *pstate, Node *node, Oid inputTypeId,
			Oid targetTypeId, int32 atttypmod);
extern Node *coerce_type_typmod(ParseState *pstate, Node *node,
				   Oid targetTypeId, int32 atttypmod);

extern bool coerce_to_boolean(ParseState *pstate, Node **pnode);

extern Oid	select_common_type(List *typeids, const char *context);
extern Node *coerce_to_common_type(ParseState *pstate, Node *node,
					  Oid targetTypeId,
					  const char *context);
extern Oid getBaseType(Oid inType);

#endif   /* PARSE_COERCE_H */
