/*-------------------------------------------------------------------------
 *
 * parse_coerce.h
 *
 *	Routines for type coercion.
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: parse_coerce.h,v 1.43 2002/05/12 23:43:04 tgl Exp $
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


extern bool IsBinaryCompatible(Oid type1, Oid type2);
extern bool IsPreferredType(CATEGORY category, Oid type);
extern CATEGORY TypeCategory(Oid type);

extern bool can_coerce_type(int nargs, Oid *input_typeids, Oid *func_typeids,
							bool isExplicit);
extern Node *coerce_type(ParseState *pstate, Node *node, Oid inputTypeId,
			Oid targetTypeId, int32 atttypmod, bool isExplicit);
extern Node *coerce_type_typmod(ParseState *pstate, Node *node,
				   Oid targetTypeId, int32 atttypmod);

extern Node *coerce_to_boolean(Node *node, const char *constructName);

extern Oid	select_common_type(List *typeids, const char *context);
extern Node *coerce_to_common_type(ParseState *pstate, Node *node,
					  Oid targetTypeId,
					  const char *context);

#endif   /* PARSE_COERCE_H */
