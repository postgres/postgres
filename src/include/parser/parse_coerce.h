/*-------------------------------------------------------------------------
 *
 * parse_coerce.h
 *	Routines for type coercion.
 *
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/parser/parse_coerce.h,v 1.66 2006/10/04 00:30:09 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PARSE_COERCE_H
#define PARSE_COERCE_H

#include "parser/parse_node.h"


typedef enum CATEGORY
{
	INVALID_TYPE,
	UNKNOWN_TYPE,
	GENERIC_TYPE,
	BOOLEAN_TYPE,
	STRING_TYPE,
	BITSTRING_TYPE,
	NUMERIC_TYPE,
	DATETIME_TYPE,
	TIMESPAN_TYPE,
	GEOMETRIC_TYPE,
	NETWORK_TYPE,
	USER_TYPE
} CATEGORY;


extern bool IsBinaryCoercible(Oid srctype, Oid targettype);
extern bool IsPreferredType(CATEGORY category, Oid type);
extern CATEGORY TypeCategory(Oid type);

extern Node *coerce_to_target_type(ParseState *pstate,
					  Node *expr, Oid exprtype,
					  Oid targettype, int32 targettypmod,
					  CoercionContext ccontext,
					  CoercionForm cformat);
extern bool can_coerce_type(int nargs, Oid *input_typeids, Oid *target_typeids,
				CoercionContext ccontext);
extern Node *coerce_type(ParseState *pstate, Node *node,
			Oid inputTypeId, Oid targetTypeId, int32 targetTypeMod,
			CoercionContext ccontext, CoercionForm cformat);
extern Node *coerce_to_domain(Node *arg, Oid baseTypeId, int32 baseTypeMod,
				 Oid typeId,
				 CoercionForm cformat, bool hideInputCoercion,
				 bool lengthCoercionDone);

extern Node *coerce_to_boolean(ParseState *pstate, Node *node,
				  const char *constructName);
extern Node *coerce_to_integer(ParseState *pstate, Node *node,
				  const char *constructName);
extern Node *coerce_to_bigint(ParseState *pstate, Node *node,
				 const char *constructName);

extern Oid	select_common_type(List *typeids, const char *context);
extern Node *coerce_to_common_type(ParseState *pstate, Node *node,
					  Oid targetTypeId,
					  const char *context);

extern bool check_generic_type_consistency(Oid *actual_arg_types,
							   Oid *declared_arg_types,
							   int nargs);
extern Oid enforce_generic_type_consistency(Oid *actual_arg_types,
								 Oid *declared_arg_types,
								 int nargs,
								 Oid rettype);
extern Oid resolve_generic_type(Oid declared_type,
					 Oid context_actual_type,
					 Oid context_declared_type);

extern bool find_coercion_pathway(Oid targetTypeId, Oid sourceTypeId,
					  CoercionContext ccontext,
					  Oid *funcid);
extern Oid	find_typmod_coercion_function(Oid typeId);

#endif   /* PARSE_COERCE_H */
