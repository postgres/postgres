/*-------------------------------------------------------------------------
 *
 * parse_node.c
 *	  various routines that make nodes for querytrees
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/parser/parse_node.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/table.h"
#include "catalog/pg_type.h"
#include "mb/pg_wchar.h"
#include "nodes/makefuncs.h"
#include "nodes/miscnodes.h"
#include "nodes/nodeFuncs.h"
#include "nodes/subscripting.h"
#include "parser/parse_coerce.h"
#include "parser/parse_expr.h"
#include "parser/parse_relation.h"
#include "parser/parsetree.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"
#include "utils/varbit.h"

static void pcb_error_callback(void *arg);


/*
 * make_parsestate
 *		Allocate and initialize a new ParseState.
 *
 * Caller should eventually release the ParseState via free_parsestate().
 */
ParseState *
make_parsestate(ParseState *parentParseState)
{
	ParseState *pstate;

	pstate = palloc0(sizeof(ParseState));

	pstate->parentParseState = parentParseState;

	/* Fill in fields that don't start at null/false/zero */
	pstate->p_next_resno = 1;
	pstate->p_resolve_unknowns = true;

	if (parentParseState)
	{
		pstate->p_sourcetext = parentParseState->p_sourcetext;
		/* all hooks are copied from parent */
		pstate->p_pre_columnref_hook = parentParseState->p_pre_columnref_hook;
		pstate->p_post_columnref_hook = parentParseState->p_post_columnref_hook;
		pstate->p_paramref_hook = parentParseState->p_paramref_hook;
		pstate->p_coerce_param_hook = parentParseState->p_coerce_param_hook;
		pstate->p_ref_hook_state = parentParseState->p_ref_hook_state;
		/* query environment stays in context for the whole parse analysis */
		pstate->p_queryEnv = parentParseState->p_queryEnv;
	}

	return pstate;
}

/*
 * free_parsestate
 *		Release a ParseState and any subsidiary resources.
 */
void
free_parsestate(ParseState *pstate)
{
	/*
	 * Check that we did not produce too many resnos; at the very least we
	 * cannot allow more than 2^16, since that would exceed the range of a
	 * AttrNumber. It seems safest to use MaxTupleAttributeNumber.
	 */
	if (pstate->p_next_resno - 1 > MaxTupleAttributeNumber)
		ereport(ERROR,
				(errcode(ERRCODE_TOO_MANY_COLUMNS),
				 errmsg("target lists can have at most %d entries",
						MaxTupleAttributeNumber)));

	if (pstate->p_target_relation != NULL)
		table_close(pstate->p_target_relation, NoLock);

	pfree(pstate);
}


/*
 * parser_errposition
 *		Report a parse-analysis-time cursor position, if possible.
 *
 * This is expected to be used within an ereport() call.  The return value
 * is a dummy (always 0, in fact).
 *
 * The locations stored in raw parsetrees are byte offsets into the source
 * string.  We have to convert them to 1-based character indexes for reporting
 * to clients.  (We do things this way to avoid unnecessary overhead in the
 * normal non-error case: computing character indexes would be much more
 * expensive than storing token offsets.)
 */
int
parser_errposition(ParseState *pstate, int location)
{
	int			pos;

	/* No-op if location was not provided */
	if (location < 0)
		return 0;
	/* Can't do anything if source text is not available */
	if (pstate == NULL || pstate->p_sourcetext == NULL)
		return 0;
	/* Convert offset to character number */
	pos = pg_mbstrlen_with_len(pstate->p_sourcetext, location) + 1;
	/* And pass it to the ereport mechanism */
	return errposition(pos);
}


/*
 * setup_parser_errposition_callback
 *		Arrange for non-parser errors to report an error position
 *
 * Sometimes the parser calls functions that aren't part of the parser
 * subsystem and can't reasonably be passed a ParseState; yet we would
 * like any errors thrown in those functions to be tagged with a parse
 * error location.  Use this function to set up an error context stack
 * entry that will accomplish that.  Usage pattern:
 *
 *		declare a local variable "ParseCallbackState pcbstate"
 *		...
 *		setup_parser_errposition_callback(&pcbstate, pstate, location);
 *		call function that might throw error;
 *		cancel_parser_errposition_callback(&pcbstate);
 */
void
setup_parser_errposition_callback(ParseCallbackState *pcbstate,
								  ParseState *pstate, int location)
{
	/* Setup error traceback support for ereport() */
	pcbstate->pstate = pstate;
	pcbstate->location = location;
	pcbstate->errcallback.callback = pcb_error_callback;
	pcbstate->errcallback.arg = (void *) pcbstate;
	pcbstate->errcallback.previous = error_context_stack;
	error_context_stack = &pcbstate->errcallback;
}

/*
 * Cancel a previously-set-up errposition callback.
 */
void
cancel_parser_errposition_callback(ParseCallbackState *pcbstate)
{
	/* Pop the error context stack */
	error_context_stack = pcbstate->errcallback.previous;
}

/*
 * Error context callback for inserting parser error location.
 *
 * Note that this will be called for *any* error occurring while the
 * callback is installed.  We avoid inserting an irrelevant error location
 * if the error is a query cancel --- are there any other important cases?
 */
static void
pcb_error_callback(void *arg)
{
	ParseCallbackState *pcbstate = (ParseCallbackState *) arg;

	if (geterrcode() != ERRCODE_QUERY_CANCELED)
		(void) parser_errposition(pcbstate->pstate, pcbstate->location);
}


/*
 * transformContainerType()
 *		Identify the actual container type for a subscripting operation.
 *
 * containerType/containerTypmod are modified if necessary to identify
 * the actual container type and typmod.  This mainly involves smashing
 * any domain to its base type, but there are some special considerations.
 * Note that caller still needs to check if the result type is a container.
 */
void
transformContainerType(Oid *containerType, int32 *containerTypmod)
{
	/*
	 * If the input is a domain, smash to base type, and extract the actual
	 * typmod to be applied to the base type. Subscripting a domain is an
	 * operation that necessarily works on the base container type, not the
	 * domain itself. (Note that we provide no method whereby the creator of a
	 * domain over a container type could hide its ability to be subscripted.)
	 */
	*containerType = getBaseTypeAndTypmod(*containerType, containerTypmod);

	/*
	 * We treat int2vector and oidvector as though they were domains over
	 * int2[] and oid[].  This is needed because array slicing could create an
	 * array that doesn't satisfy the dimensionality constraints of the
	 * xxxvector type; so we want the result of a slice operation to be
	 * considered to be of the more general type.
	 */
	if (*containerType == INT2VECTOROID)
		*containerType = INT2ARRAYOID;
	else if (*containerType == OIDVECTOROID)
		*containerType = OIDARRAYOID;
}

/*
 * transformContainerSubscripts()
 *		Transform container (array, etc) subscripting.  This is used for both
 *		container fetch and container assignment.
 *
 * In a container fetch, we are given a source container value and we produce
 * an expression that represents the result of extracting a single container
 * element or a container slice.
 *
 * Container assignments are treated basically the same as container fetches
 * here.  The caller will modify the result node to insert the source value
 * that is to be assigned to the element or slice that a fetch would have
 * retrieved.  The execution result will be a new container value with
 * the source value inserted into the right part of the container.
 *
 * For both cases, if the source is of a domain-over-container type, the
 * result is the same as if it had been of the container type; essentially,
 * we must fold a domain to its base type before applying subscripting.
 * (Note that int2vector and oidvector are treated as domains here.)
 *
 * pstate			Parse state
 * containerBase	Already-transformed expression for the container as a whole
 * containerType	OID of container's datatype (should match type of
 *					containerBase, or be the base type of containerBase's
 *					domain type)
 * containerTypMod	typmod for the container
 * indirection		Untransformed list of subscripts (must not be NIL)
 * isAssignment		True if this will become a container assignment.
 */
SubscriptingRef *
transformContainerSubscripts(ParseState *pstate,
							 Node *containerBase,
							 Oid containerType,
							 int32 containerTypMod,
							 List *indirection,
							 bool isAssignment)
{
	SubscriptingRef *sbsref;
	const SubscriptRoutines *sbsroutines;
	Oid			elementType;
	bool		isSlice = false;
	ListCell   *idx;

	/*
	 * Determine the actual container type, smashing any domain.  In the
	 * assignment case the caller already did this, since it also needs to
	 * know the actual container type.
	 */
	if (!isAssignment)
		transformContainerType(&containerType, &containerTypMod);

	/*
	 * Verify that the container type is subscriptable, and get its support
	 * functions and typelem.
	 */
	sbsroutines = getSubscriptingRoutines(containerType, &elementType);
	if (!sbsroutines)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("cannot subscript type %s because it does not support subscripting",
						format_type_be(containerType)),
				 parser_errposition(pstate, exprLocation(containerBase))));

	/*
	 * Detect whether any of the indirection items are slice specifiers.
	 *
	 * A list containing only simple subscripts refers to a single container
	 * element.  If any of the items are slice specifiers (lower:upper), then
	 * the subscript expression means a container slice operation.
	 */
	foreach(idx, indirection)
	{
		A_Indices  *ai = lfirst_node(A_Indices, idx);

		if (ai->is_slice)
		{
			isSlice = true;
			break;
		}
	}

	/*
	 * Ready to build the SubscriptingRef node.
	 */
	sbsref = makeNode(SubscriptingRef);

	sbsref->refcontainertype = containerType;
	sbsref->refelemtype = elementType;
	/* refrestype is to be set by container-specific logic */
	sbsref->reftypmod = containerTypMod;
	/* refcollid will be set by parse_collate.c */
	/* refupperindexpr, reflowerindexpr are to be set by container logic */
	sbsref->refexpr = (Expr *) containerBase;
	sbsref->refassgnexpr = NULL;	/* caller will fill if it's an assignment */

	/*
	 * Call the container-type-specific logic to transform the subscripts and
	 * determine the subscripting result type.
	 */
	sbsroutines->transform(sbsref, indirection, pstate,
						   isSlice, isAssignment);

	/*
	 * Verify we got a valid type (this defends, for example, against someone
	 * using array_subscript_handler as typsubscript without setting typelem).
	 */
	if (!OidIsValid(sbsref->refrestype))
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("cannot subscript type %s because it does not support subscripting",
						format_type_be(containerType))));

	return sbsref;
}

/*
 * make_const
 *
 *	Convert an A_Const node (as returned by the grammar) to a Const node
 *	of the "natural" type for the constant.  Note that this routine is
 *	only used when there is no explicit cast for the constant, so we
 *	have to guess what type is wanted.
 *
 *	For string literals we produce a constant of type UNKNOWN ---- whose
 *	representation is the same as cstring, but it indicates to later type
 *	resolution that we're not sure yet what type it should be considered.
 *	Explicit "NULL" constants are also typed as UNKNOWN.
 *
 *	For integers and floats we produce int4, int8, or numeric depending
 *	on the value of the number.  XXX We should produce int2 as well,
 *	but additional cleanup is needed before we can do that; there are
 *	too many examples that fail if we try.
 */
Const *
make_const(ParseState *pstate, A_Const *aconst)
{
	Const	   *con;
	Datum		val;
	Oid			typeid;
	int			typelen;
	bool		typebyval;
	ParseCallbackState pcbstate;

	if (aconst->isnull)
	{
		/* return a null const */
		con = makeConst(UNKNOWNOID,
						-1,
						InvalidOid,
						-2,
						(Datum) 0,
						true,
						false);
		con->location = aconst->location;
		return con;
	}

	switch (nodeTag(&aconst->val))
	{
		case T_Integer:
			val = Int32GetDatum(intVal(&aconst->val));

			typeid = INT4OID;
			typelen = sizeof(int32);
			typebyval = true;
			break;

		case T_Float:
			{
				/* could be an oversize integer as well as a float ... */

				ErrorSaveContext escontext = {T_ErrorSaveContext};
				int64		val64;

				val64 = pg_strtoint64_safe(aconst->val.fval.fval, (Node *) &escontext);
				if (!escontext.error_occurred)
				{
					/*
					 * It might actually fit in int32. Probably only INT_MIN
					 * can occur, but we'll code the test generally just to be
					 * sure.
					 */
					int32		val32 = (int32) val64;

					if (val64 == (int64) val32)
					{
						val = Int32GetDatum(val32);

						typeid = INT4OID;
						typelen = sizeof(int32);
						typebyval = true;
					}
					else
					{
						val = Int64GetDatum(val64);

						typeid = INT8OID;
						typelen = sizeof(int64);
						typebyval = FLOAT8PASSBYVAL;	/* int8 and float8 alike */
					}
				}
				else
				{
					/* arrange to report location if numeric_in() fails */
					setup_parser_errposition_callback(&pcbstate, pstate, aconst->location);
					val = DirectFunctionCall3(numeric_in,
											  CStringGetDatum(aconst->val.fval.fval),
											  ObjectIdGetDatum(InvalidOid),
											  Int32GetDatum(-1));
					cancel_parser_errposition_callback(&pcbstate);

					typeid = NUMERICOID;
					typelen = -1;	/* variable len */
					typebyval = false;
				}
				break;
			}

		case T_Boolean:
			val = BoolGetDatum(boolVal(&aconst->val));

			typeid = BOOLOID;
			typelen = 1;
			typebyval = true;
			break;

		case T_String:

			/*
			 * We assume here that UNKNOWN's internal representation is the
			 * same as CSTRING
			 */
			val = CStringGetDatum(strVal(&aconst->val));

			typeid = UNKNOWNOID;	/* will be coerced later */
			typelen = -2;		/* cstring-style varwidth type */
			typebyval = false;
			break;

		case T_BitString:
			/* arrange to report location if bit_in() fails */
			setup_parser_errposition_callback(&pcbstate, pstate, aconst->location);
			val = DirectFunctionCall3(bit_in,
									  CStringGetDatum(aconst->val.bsval.bsval),
									  ObjectIdGetDatum(InvalidOid),
									  Int32GetDatum(-1));
			cancel_parser_errposition_callback(&pcbstate);
			typeid = BITOID;
			typelen = -1;
			typebyval = false;
			break;

		default:
			elog(ERROR, "unrecognized node type: %d", (int) nodeTag(&aconst->val));
			return NULL;		/* keep compiler quiet */
	}

	con = makeConst(typeid,
					-1,			/* typmod -1 is OK for all cases */
					InvalidOid, /* all cases are uncollatable types */
					typelen,
					val,
					false,
					typebyval);
	con->location = aconst->location;

	return con;
}
