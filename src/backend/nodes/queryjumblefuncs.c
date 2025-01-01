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

#include "common/hashfn.h"
#include "miscadmin.h"
#include "nodes/queryjumble.h"
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

static void AppendJumble(JumbleState *jstate,
						 const unsigned char *item, Size size);
static void RecordConstLocation(JumbleState *jstate, int location);
static void _jumbleNode(JumbleState *jstate, Node *node);
static void _jumbleA_Const(JumbleState *jstate, Node *node);
static void _jumbleList(JumbleState *jstate, Node *node);
static void _jumbleVariableSetStmt(JumbleState *jstate, Node *node);

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

JumbleState *
JumbleQuery(Query *query)
{
	JumbleState *jstate = NULL;

	Assert(IsQueryIdEnabled());

	jstate = (JumbleState *) palloc(sizeof(JumbleState));

	/* Set up workspace for query jumbling */
	jstate->jumble = (unsigned char *) palloc(JUMBLE_SIZE);
	jstate->jumble_len = 0;
	jstate->clocations_buf_size = 32;
	jstate->clocations = (LocationLen *)
		palloc(jstate->clocations_buf_size * sizeof(LocationLen));
	jstate->clocations_count = 0;
	jstate->highest_extern_param_id = 0;

	/* Compute query ID and mark the Query node with it */
	_jumbleNode(jstate, (Node *) query);
	query->queryId = DatumGetUInt64(hash_any_extended(jstate->jumble,
													  jstate->jumble_len,
													  0));

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
 * AppendJumble: Append a value that is substantive in a given query to
 * the current jumble.
 */
static void
AppendJumble(JumbleState *jstate, const unsigned char *item, Size size)
{
	unsigned char *jumble = jstate->jumble;
	Size		jumble_len = jstate->jumble_len;

	/*
	 * Whenever the jumble buffer is full, we hash the current contents and
	 * reset the buffer to contain just that hash value, thus relying on the
	 * hash to summarize everything so far.
	 */
	while (size > 0)
	{
		Size		part_size;

		if (jumble_len >= JUMBLE_SIZE)
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
	}
	jstate->jumble_len = jumble_len;
}

/*
 * Record location of constant within query string of query tree
 * that is currently being walked.
 */
static void
RecordConstLocation(JumbleState *jstate, int location)
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
		jstate->clocations[jstate->clocations_count].length = -1;
		jstate->clocations_count++;
	}
}

#define JUMBLE_NODE(item) \
	_jumbleNode(jstate, (Node *) expr->item)
#define JUMBLE_LOCATION(location) \
	RecordConstLocation(jstate, expr->location)
#define JUMBLE_FIELD(item) \
	AppendJumble(jstate, (const unsigned char *) &(expr->item), sizeof(expr->item))
#define JUMBLE_FIELD_SINGLE(item) \
	AppendJumble(jstate, (const unsigned char *) &(item), sizeof(item))
#define JUMBLE_STRING(str) \
do { \
	if (expr->str) \
		AppendJumble(jstate, (const unsigned char *) (expr->str), strlen(expr->str) + 1); \
} while(0)

#include "queryjumblefuncs.funcs.c"

static void
_jumbleNode(JumbleState *jstate, Node *node)
{
	Node	   *expr = node;

	if (expr == NULL)
		return;

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
				JUMBLE_FIELD_SINGLE(lfirst_int(l));
			break;
		case T_OidList:
			foreach(l, expr)
				JUMBLE_FIELD_SINGLE(lfirst_oid(l));
			break;
		case T_XidList:
			foreach(l, expr)
				JUMBLE_FIELD_SINGLE(lfirst_xid(l));
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
