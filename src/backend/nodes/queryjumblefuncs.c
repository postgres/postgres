/*-------------------------------------------------------------------------
 *
 * queryjumblefuncs.c
 *	 Query normalization and fingerprinting.
 *
 * Normalization is a process whereby similar queries, typically differing only
 * in their constants (though the exact rules are somewhat more subtle than
 * that) are recognized as equivalent, and are tracked as a single entry.  This
 * is particularly useful for non-prepared queries.
 *
 * Normalization is implemented by fingerprinting queries, selectively
 * serializing those fields of each query tree's nodes that are judged to be
 * essential to the query.  This is referred to as a query jumble.  This is
 * distinct from a regular serialization in that various extraneous
 * information is ignored as irrelevant or not essential to the query, such
 * as the collations of Vars and, most notably, the values of constants.
 *
 * This jumble is acquired at the end of parse analysis of each query, and
 * a 64-bit hash of it is stored into the query's Query.queryId field.
 * The server then copies this value around, making it available in plan
 * tree(s) generated from the query.  The executor can then use this value
 * to blame query costs on the proper queryId.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/nodes/queryjumblefuncs.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/transam.h"
#include "catalog/pg_proc.h"
#include "common/hashfn.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "nodes/queryjumble.h"
#include "utils/lsyscache.h"
#include "parser/scansup.h"

#define JUMBLE_SIZE				1024	/* query serialization buffer size */

/* GUC parameters */
int			compute_query_id = COMPUTE_QUERY_ID_AUTO;

/*
 * True when compute_query_id is ON or AUTO, and a module requests them.
 *
 * Note that IsQueryIdEnabled() should be used instead of checking
 * query_id_enabled or compute_query_id directly when we want to know
 * whether query identifiers are computed in the core or not.
 */
bool		query_id_enabled = false;

static JumbleState *InitJumble(void);
static uint64 DoJumble(JumbleState *jstate, Node *node);
static void AppendJumble(JumbleState *jstate,
						 const unsigned char *value, Size size);
static void FlushPendingNulls(JumbleState *jstate);
static void RecordConstLocation(JumbleState *jstate,
								int location, bool squashed);
static void _jumbleNode(JumbleState *jstate, Node *node);
static void _jumbleElements(JumbleState *jstate, List *elements);
static void _jumbleA_Const(JumbleState *jstate, Node *node);
static void _jumbleList(JumbleState *jstate, Node *node);
static void _jumbleVariableSetStmt(JumbleState *jstate, Node *node);
static void _jumbleRangeTblEntry_eref(JumbleState *jstate,
									  RangeTblEntry *rte,
									  Alias *expr);

/*
 * Given a possibly multi-statement source string, confine our attention to the
 * relevant part of the string.
 */
const char *
CleanQuerytext(const char *query, int *location, int *len)
{
	int			query_location = *location;
	int			query_len = *len;

	/* First apply starting offset, unless it's -1 (unknown). */
	if (query_location >= 0)
	{
		Assert(query_location <= strlen(query));
		query += query_location;
		/* Length of 0 (or -1) means "rest of string" */
		if (query_len <= 0)
			query_len = strlen(query);
		else
			Assert(query_len <= strlen(query));
	}
	else
	{
		/* If query location is unknown, distrust query_len as well */
		query_location = 0;
		query_len = strlen(query);
	}

	/*
	 * Discard leading and trailing whitespace, too.  Use scanner_isspace()
	 * not libc's isspace(), because we want to match the lexer's behavior.
	 *
	 * Note: the parser now strips leading comments and whitespace from the
	 * reported stmt_location, so this first loop will only iterate in the
	 * unusual case that the location didn't propagate to here.  But the
	 * statement length will extend to the end-of-string or terminating
	 * semicolon, so the second loop often does something useful.
	 */
	while (query_len > 0 && scanner_isspace(query[0]))
		query++, query_location++, query_len--;
	while (query_len > 0 && scanner_isspace(query[query_len - 1]))
		query_len--;

	*location = query_location;
	*len = query_len;

	return query;
}

/*
 * JumbleQuery
 *		Recursively process the given Query producing a 64-bit hash value by
 *		hashing the relevant fields and record that value in the Query's queryId
 *		field.  Return the JumbleState object used for jumbling the query.
 */
JumbleState *
JumbleQuery(Query *query)
{
	JumbleState *jstate;

	Assert(IsQueryIdEnabled());

	jstate = InitJumble();

	query->queryId = DoJumble(jstate, (Node *) query);

	/*
	 * If we are unlucky enough to get a hash of zero, use 1 instead for
	 * normal statements and 2 for utility queries.
	 */
	if (query->queryId == UINT64CONST(0))
	{
		if (query->utilityStmt)
			query->queryId = UINT64CONST(2);
		else
			query->queryId = UINT64CONST(1);
	}

	return jstate;
}

/*
 * Enables query identifier computation.
 *
 * Third-party plugins can use this function to inform core that they require
 * a query identifier to be computed.
 */
void
EnableQueryId(void)
{
	if (compute_query_id != COMPUTE_QUERY_ID_OFF)
		query_id_enabled = true;
}

/*
 * InitJumble
 *		Allocate a JumbleState object and make it ready to jumble.
 */
static JumbleState *
InitJumble(void)
{
	JumbleState *jstate;

	jstate = (JumbleState *) palloc(sizeof(JumbleState));

	/* Set up workspace for query jumbling */
	jstate->jumble = (unsigned char *) palloc(JUMBLE_SIZE);
	jstate->jumble_len = 0;
	jstate->clocations_buf_size = 32;
	jstate->clocations = (LocationLen *) palloc(jstate->clocations_buf_size *
												sizeof(LocationLen));
	jstate->clocations_count = 0;
	jstate->highest_extern_param_id = 0;
	jstate->pending_nulls = 0;
#ifdef USE_ASSERT_CHECKING
	jstate->total_jumble_len = 0;
#endif

	return jstate;
}

/*
 * DoJumble
 *		Jumble the given Node using the given JumbleState and return the resulting
 *		jumble hash.
 */
static uint64
DoJumble(JumbleState *jstate, Node *node)
{
	/* Jumble the given node */
	_jumbleNode(jstate, node);

	/* Flush any pending NULLs before doing the final hash */
	if (jstate->pending_nulls > 0)
		FlushPendingNulls(jstate);

	/* Process the jumble buffer and produce the hash value */
	return DatumGetUInt64(hash_any_extended(jstate->jumble,
											jstate->jumble_len,
											0));
}

/*
 * AppendJumbleInternal: Internal function for appending to the jumble buffer
 *
 * Note: Callers must ensure that size > 0.
 */
static pg_attribute_always_inline void
AppendJumbleInternal(JumbleState *jstate, const unsigned char *item,
					 Size size)
{
	unsigned char *jumble = jstate->jumble;
	Size		jumble_len = jstate->jumble_len;

	/* Ensure the caller didn't mess up */
	Assert(size > 0);

	/*
	 * Fast path for when there's enough space left in the buffer.  This is
	 * worthwhile as means the memcpy can be inlined into very efficient code
	 * when 'size' is a compile-time constant.
	 */
	if (likely(size <= JUMBLE_SIZE - jumble_len))
	{
		memcpy(jumble + jumble_len, item, size);
		jstate->jumble_len += size;

#ifdef USE_ASSERT_CHECKING
		jstate->total_jumble_len += size;
#endif

		return;
	}

	/*
	 * Whenever the jumble buffer is full, we hash the current contents and
	 * reset the buffer to contain just that hash value, thus relying on the
	 * hash to summarize everything so far.
	 */
	do
	{
		Size		part_size;

		if (unlikely(jumble_len >= JUMBLE_SIZE))
		{
			uint64		start_hash;

			start_hash = DatumGetUInt64(hash_any_extended(jumble,
														  JUMBLE_SIZE, 0));
			memcpy(jumble, &start_hash, sizeof(start_hash));
			jumble_len = sizeof(start_hash);
		}
		part_size = Min(size, JUMBLE_SIZE - jumble_len);
		memcpy(jumble + jumble_len, item, part_size);
		jumble_len += part_size;
		item += part_size;
		size -= part_size;

#ifdef USE_ASSERT_CHECKING
		jstate->total_jumble_len += part_size;
#endif
	} while (size > 0);

	jstate->jumble_len = jumble_len;
}

/*
 * AppendJumble
 *		Add 'size' bytes of the given jumble 'value' to the jumble state
 */
static pg_noinline void
AppendJumble(JumbleState *jstate, const unsigned char *value, Size size)
{
	if (jstate->pending_nulls > 0)
		FlushPendingNulls(jstate);

	AppendJumbleInternal(jstate, value, size);
}

/*
 * AppendJumbleNull
 *		For jumbling NULL pointers
 */
static pg_attribute_always_inline void
AppendJumbleNull(JumbleState *jstate)
{
	jstate->pending_nulls++;
}

/*
 * AppendJumble8
 *		Add the first byte from the given 'value' pointer to the jumble state
 */
static pg_noinline void
AppendJumble8(JumbleState *jstate, const unsigned char *value)
{
	if (jstate->pending_nulls > 0)
		FlushPendingNulls(jstate);

	AppendJumbleInternal(jstate, value, 1);
}

/*
 * AppendJumble16
 *		Add the first 2 bytes from the given 'value' pointer to the jumble
 *		state.
 */
static pg_noinline void
AppendJumble16(JumbleState *jstate, const unsigned char *value)
{
	if (jstate->pending_nulls > 0)
		FlushPendingNulls(jstate);

	AppendJumbleInternal(jstate, value, 2);
}

/*
 * AppendJumble32
 *		Add the first 4 bytes from the given 'value' pointer to the jumble
 *		state.
 */
static pg_noinline void
AppendJumble32(JumbleState *jstate, const unsigned char *value)
{
	if (jstate->pending_nulls > 0)
		FlushPendingNulls(jstate);

	AppendJumbleInternal(jstate, value, 4);
}

/*
 * AppendJumble64
 *		Add the first 8 bytes from the given 'value' pointer to the jumble
 *		state.
 */
static pg_noinline void
AppendJumble64(JumbleState *jstate, const unsigned char *value)
{
	if (jstate->pending_nulls > 0)
		FlushPendingNulls(jstate);

	AppendJumbleInternal(jstate, value, 8);
}

/*
 * FlushPendingNulls
 *		Incorporate the pending_nulls value into the jumble buffer.
 *
 * Note: Callers must ensure that there's at least 1 pending NULL.
 */
static pg_attribute_always_inline void
FlushPendingNulls(JumbleState *jstate)
{
	Assert(jstate->pending_nulls > 0);

	AppendJumbleInternal(jstate,
						 (const unsigned char *) &jstate->pending_nulls, 4);
	jstate->pending_nulls = 0;
}


/*
 * Record location of constant within query string of query tree that is
 * currently being walked.
 *
 * 'squashed' signals that the constant represents the first or the last
 * element in a series of merged constants, and everything but the first/last
 * element contributes nothing to the jumble hash.
 */
static void
RecordConstLocation(JumbleState *jstate, int location, bool squashed)
{
	/* -1 indicates unknown or undefined location */
	if (location >= 0)
	{
		/* enlarge array if needed */
		if (jstate->clocations_count >= jstate->clocations_buf_size)
		{
			jstate->clocations_buf_size *= 2;
			jstate->clocations = (LocationLen *)
				repalloc(jstate->clocations,
						 jstate->clocations_buf_size *
						 sizeof(LocationLen));
		}
		jstate->clocations[jstate->clocations_count].location = location;
		/* initialize lengths to -1 to simplify third-party module usage */
		jstate->clocations[jstate->clocations_count].squashed = squashed;
		jstate->clocations[jstate->clocations_count].length = -1;
		jstate->clocations_count++;
	}
}

/*
 * Subroutine for _jumbleElements: Verify a few simple cases where we can
 * deduce that the expression is a constant:
 *
 * - Ignore a possible wrapping RelabelType and CoerceViaIO.
 * - If it's a FuncExpr, check that the function is an implicit
 *   cast and its arguments are Const.
 * - Otherwise test if the expression is a simple Const.
 */
static bool
IsSquashableConst(Node *element)
{
	if (IsA(element, RelabelType))
		element = (Node *) ((RelabelType *) element)->arg;

	if (IsA(element, CoerceViaIO))
		element = (Node *) ((CoerceViaIO *) element)->arg;

	if (IsA(element, FuncExpr))
	{
		FuncExpr   *func = (FuncExpr *) element;
		ListCell   *temp;

		if (func->funcformat != COERCE_IMPLICIT_CAST &&
			func->funcformat != COERCE_EXPLICIT_CAST)
			return false;

		if (func->funcid > FirstGenbkiObjectId)
			return false;

		foreach(temp, func->args)
		{
			Node	   *arg = lfirst(temp);

			if (!IsA(arg, Const))	/* XXX we could recurse here instead */
				return false;
		}

		return true;
	}

	if (!IsA(element, Const))
		return false;

	return true;
}

/*
 * Subroutine for _jumbleElements: Verify whether the provided list
 * can be squashed, meaning it contains only constant expressions.
 *
 * Return value indicates if squashing is possible.
 *
 * Note that this function searches only for explicit Const nodes with
 * possibly very simple decorations on top, and does not try to simplify
 * expressions.
 */
static bool
IsSquashableConstList(List *elements, Node **firstExpr, Node **lastExpr)
{
	ListCell   *temp;

	/*
	 * If squashing is disabled, or the list is too short, we don't try to
	 * squash it.
	 */
	if (list_length(elements) < 2)
		return false;

	foreach(temp, elements)
	{
		if (!IsSquashableConst(lfirst(temp)))
			return false;
	}

	*firstExpr = linitial(elements);
	*lastExpr = llast(elements);

	return true;
}

#define JUMBLE_NODE(item) \
	_jumbleNode(jstate, (Node *) expr->item)
#define JUMBLE_ELEMENTS(list) \
	_jumbleElements(jstate, (List *) expr->list)
#define JUMBLE_LOCATION(location) \
	RecordConstLocation(jstate, expr->location, false)
#define JUMBLE_FIELD(item) \
do { \
	if (sizeof(expr->item) == 8) \
		AppendJumble64(jstate, (const unsigned char *) &(expr->item)); \
	else if (sizeof(expr->item) == 4) \
		AppendJumble32(jstate, (const unsigned char *) &(expr->item)); \
	else if (sizeof(expr->item) == 2) \
		AppendJumble16(jstate, (const unsigned char *) &(expr->item)); \
	else if (sizeof(expr->item) == 1) \
		AppendJumble8(jstate, (const unsigned char *) &(expr->item)); \
	else \
		AppendJumble(jstate, (const unsigned char *) &(expr->item), sizeof(expr->item)); \
} while (0)
#define JUMBLE_STRING(str) \
do { \
	if (expr->str) \
		AppendJumble(jstate, (const unsigned char *) (expr->str), strlen(expr->str) + 1); \
	else \
		AppendJumbleNull(jstate); \
} while(0)
/* Function name used for the node field attribute custom_query_jumble. */
#define JUMBLE_CUSTOM(nodetype, item) \
	_jumble##nodetype##_##item(jstate, expr, expr->item)

#include "queryjumblefuncs.funcs.c"

/*
 * We jumble lists of constant elements as one individual item regardless
 * of how many elements are in the list.  This means different queries
 * jumble to the same query_id, if the only difference is the number of
 * elements in the list.
 */
static void
_jumbleElements(JumbleState *jstate, List *elements)
{
	Node	   *first,
			   *last;

	if (IsSquashableConstList(elements, &first, &last))
	{
		/*
		 * If this list of elements is squashable, keep track of the location
		 * of its first and last elements.  When reading back the locations
		 * array, we'll see two consecutive locations with ->squashed set to
		 * true, indicating the location of initial and final elements of this
		 * list.
		 *
		 * For the limited set of cases we support now (implicit coerce via
		 * FuncExpr, Const) it's fine to use exprLocation of the 'last'
		 * expression, but if more complex composite expressions are to be
		 * supported (e.g., OpExpr or FuncExpr as an explicit call), more
		 * sophisticated tracking will be needed.
		 */
		RecordConstLocation(jstate, exprLocation(first), true);
		RecordConstLocation(jstate, exprLocation(last), true);
	}
	else
	{
		_jumbleNode(jstate, (Node *) elements);
	}
}

static void
_jumbleNode(JumbleState *jstate, Node *node)
{
	Node	   *expr = node;
#ifdef USE_ASSERT_CHECKING
	Size		prev_jumble_len = jstate->total_jumble_len;
#endif

	if (expr == NULL)
	{
		AppendJumbleNull(jstate);
		return;
	}

	/* Guard against stack overflow due to overly complex expressions */
	check_stack_depth();

	/*
	 * We always emit the node's NodeTag, then any additional fields that are
	 * considered significant, and then we recurse to any child nodes.
	 */
	JUMBLE_FIELD(type);

	switch (nodeTag(expr))
	{
#include "queryjumblefuncs.switch.c"

		case T_List:
		case T_IntList:
		case T_OidList:
		case T_XidList:
			_jumbleList(jstate, expr);
			break;

		default:
			/* Only a warning, since we can stumble along anyway */
			elog(WARNING, "unrecognized node type: %d",
				 (int) nodeTag(expr));
			break;
	}

	/* Special cases to handle outside the automated code */
	switch (nodeTag(expr))
	{
		case T_Param:
			{
				Param	   *p = (Param *) node;

				/*
				 * Update the highest Param id seen, in order to start
				 * normalization correctly.
				 */
				if (p->paramkind == PARAM_EXTERN &&
					p->paramid > jstate->highest_extern_param_id)
					jstate->highest_extern_param_id = p->paramid;
			}
			break;
		default:
			break;
	}

	/* Ensure we added something to the jumble buffer */
	Assert(jstate->total_jumble_len > prev_jumble_len);
}

static void
_jumbleList(JumbleState *jstate, Node *node)
{
	List	   *expr = (List *) node;
	ListCell   *l;

	switch (expr->type)
	{
		case T_List:
			foreach(l, expr)
				_jumbleNode(jstate, lfirst(l));
			break;
		case T_IntList:
			foreach(l, expr)
				AppendJumble32(jstate, (const unsigned char *) &lfirst_int(l));
			break;
		case T_OidList:
			foreach(l, expr)
				AppendJumble32(jstate, (const unsigned char *) &lfirst_oid(l));
			break;
		case T_XidList:
			foreach(l, expr)
				AppendJumble32(jstate, (const unsigned char *) &lfirst_xid(l));
			break;
		default:
			elog(ERROR, "unrecognized list node type: %d",
				 (int) expr->type);
			return;
	}
}

static void
_jumbleA_Const(JumbleState *jstate, Node *node)
{
	A_Const    *expr = (A_Const *) node;

	JUMBLE_FIELD(isnull);
	if (!expr->isnull)
	{
		JUMBLE_FIELD(val.node.type);
		switch (nodeTag(&expr->val))
		{
			case T_Integer:
				JUMBLE_FIELD(val.ival.ival);
				break;
			case T_Float:
				JUMBLE_STRING(val.fval.fval);
				break;
			case T_Boolean:
				JUMBLE_FIELD(val.boolval.boolval);
				break;
			case T_String:
				JUMBLE_STRING(val.sval.sval);
				break;
			case T_BitString:
				JUMBLE_STRING(val.bsval.bsval);
				break;
			default:
				elog(ERROR, "unrecognized node type: %d",
					 (int) nodeTag(&expr->val));
				break;
		}
	}
}

static void
_jumbleVariableSetStmt(JumbleState *jstate, Node *node)
{
	VariableSetStmt *expr = (VariableSetStmt *) node;

	JUMBLE_FIELD(kind);
	JUMBLE_STRING(name);

	/*
	 * Account for the list of arguments in query jumbling only if told by the
	 * parser.
	 */
	if (expr->jumble_args)
		JUMBLE_NODE(args);
	JUMBLE_FIELD(is_local);
	JUMBLE_LOCATION(location);
}

/*
 * Custom query jumble function for RangeTblEntry.eref.
 */
static void
_jumbleRangeTblEntry_eref(JumbleState *jstate,
						  RangeTblEntry *rte,
						  Alias *expr)
{
	JUMBLE_FIELD(type);

	/*
	 * This includes only the table name, the list of column names is ignored.
	 */
	JUMBLE_STRING(aliasname);
}
