/*-------------------------------------------------------------------------
 *
 * jsonb_gin.c
 *	 GIN support functions for jsonb
 *
 * Copyright (c) 2014-2021, PostgreSQL Global Development Group
 *
 * We provide two opclasses for jsonb indexing: jsonb_ops and jsonb_path_ops.
 * For their description see json.sgml and comments in jsonb.h.
 *
 * The operators support, among the others, "jsonb @? jsonpath" and
 * "jsonb @@ jsonpath".  Expressions containing these operators are easily
 * expressed through each other.
 *
 *	jb @? 'path' <=> jb @@ 'EXISTS(path)'
 *	jb @@ 'expr' <=> jb @? '$ ? (expr)'
 *
 * Thus, we're going to consider only @@ operator, while regarding @? operator
 * the same is true for jb @@ 'EXISTS(path)'.
 *
 * Result of jsonpath query extraction is a tree, which leaf nodes are index
 * entries and non-leaf nodes are AND/OR logical expressions.  Basically we
 * extract following statements out of jsonpath:
 *
 *	1) "accessors_chain = const",
 *	2) "EXISTS(accessors_chain)".
 *
 * Accessors chain may consist of .key, [*] and [index] accessors.  jsonb_ops
 * additionally supports .* and .**.
 *
 * For now, both jsonb_ops and jsonb_path_ops supports only statements of
 * the 1st find.  jsonb_ops might also support statements of the 2nd kind,
 * but given we have no statistics keys extracted from accessors chain
 * are likely non-selective.  Therefore, we choose to not confuse optimizer
 * and skip statements of the 2nd kind altogether.  In future versions that
 * might be changed.
 *
 * In jsonb_ops statement of the 1st kind is split into expression of AND'ed
 * keys and const.  Sometimes const might be interpreted as both value or key
 * in jsonb_ops.  Then statement of 1st kind is decomposed into the expression
 * below.
 *
 *	key1 AND key2 AND ... AND keyN AND (const_as_value OR const_as_key)
 *
 * jsonb_path_ops transforms each statement of the 1st kind into single hash
 * entry below.
 *
 *	HASH(key1, key2, ... , keyN, const)
 *
 * Despite statements of the 2nd kind are not supported by both jsonb_ops and
 * jsonb_path_ops, EXISTS(path) expressions might be still supported,
 * when statements of 1st kind could be extracted out of their filters.
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/jsonb_gin.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/gin.h"
#include "access/stratnum.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_type.h"
#include "common/hashfn.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/jsonb.h"
#include "utils/jsonpath.h"
#include "utils/varlena.h"

typedef struct PathHashStack
{
	uint32		hash;
	struct PathHashStack *parent;
} PathHashStack;

/* Buffer for GIN entries */
typedef struct GinEntries
{
	Datum	   *buf;
	int			count;
	int			allocated;
} GinEntries;

typedef enum JsonPathGinNodeType
{
	JSP_GIN_OR,
	JSP_GIN_AND,
	JSP_GIN_ENTRY
} JsonPathGinNodeType;

typedef struct JsonPathGinNode JsonPathGinNode;

/* Node in jsonpath expression tree */
struct JsonPathGinNode
{
	JsonPathGinNodeType type;
	union
	{
		int			nargs;		/* valid for OR and AND nodes */
		int			entryIndex; /* index in GinEntries array, valid for ENTRY
								 * nodes after entries output */
		Datum		entryDatum; /* path hash or key name/scalar, valid for
								 * ENTRY nodes before entries output */
	}			val;
	JsonPathGinNode *args[FLEXIBLE_ARRAY_MEMBER];	/* valid for OR and AND
													 * nodes */
};

/*
 * jsonb_ops entry extracted from jsonpath item.  Corresponding path item
 * may be: '.key', '.*', '.**', '[index]' or '[*]'.
 * Entry type is stored in 'type' field.
 */
typedef struct JsonPathGinPathItem
{
	struct JsonPathGinPathItem *parent;
	Datum		keyName;		/* key name (for '.key' path item) or NULL */
	JsonPathItemType type;		/* type of jsonpath item */
} JsonPathGinPathItem;

/* GIN representation of the extracted json path */
typedef union JsonPathGinPath
{
	JsonPathGinPathItem *items; /* list of path items (jsonb_ops) */
	uint32		hash;			/* hash of the path (jsonb_path_ops) */
} JsonPathGinPath;

typedef struct JsonPathGinContext JsonPathGinContext;

/* Callback, which stores information about path item into JsonPathGinPath */
typedef bool (*JsonPathGinAddPathItemFunc) (JsonPathGinPath *path,
											JsonPathItem *jsp);

/*
 * Callback, which extracts set of nodes from statement of 1st kind
 * (scalar != NULL) or statement of 2nd kind (scalar == NULL).
 */
typedef List *(*JsonPathGinExtractNodesFunc) (JsonPathGinContext *cxt,
											  JsonPathGinPath path,
											  JsonbValue *scalar,
											  List *nodes);

/* Context for jsonpath entries extraction */
struct JsonPathGinContext
{
	JsonPathGinAddPathItemFunc add_path_item;
	JsonPathGinExtractNodesFunc extract_nodes;
	bool		lax;
};

static Datum make_text_key(char flag, const char *str, int len);
static Datum make_scalar_key(const JsonbValue *scalarVal, bool is_key);

static JsonPathGinNode *extract_jsp_bool_expr(JsonPathGinContext *cxt,
											  JsonPathGinPath path, JsonPathItem *jsp, bool not);


/* Initialize GinEntries struct */
static void
init_gin_entries(GinEntries *entries, int preallocated)
{
	entries->allocated = preallocated;
	entries->buf = preallocated ? palloc(sizeof(Datum) * preallocated) : NULL;
	entries->count = 0;
}

/* Add new entry to GinEntries */
static int
add_gin_entry(GinEntries *entries, Datum entry)
{
	int			id = entries->count;

	if (entries->count >= entries->allocated)
	{
		if (entries->allocated)
		{
			entries->allocated *= 2;
			entries->buf = repalloc(entries->buf,
									sizeof(Datum) * entries->allocated);
		}
		else
		{
			entries->allocated = 8;
			entries->buf = palloc(sizeof(Datum) * entries->allocated);
		}
	}

	entries->buf[entries->count++] = entry;

	return id;
}

/*
 *
 * jsonb_ops GIN opclass support functions
 *
 */

Datum
gin_compare_jsonb(PG_FUNCTION_ARGS)
{
	text	   *arg1 = PG_GETARG_TEXT_PP(0);
	text	   *arg2 = PG_GETARG_TEXT_PP(1);
	int32		result;
	char	   *a1p,
			   *a2p;
	int			len1,
				len2;

	a1p = VARDATA_ANY(arg1);
	a2p = VARDATA_ANY(arg2);

	len1 = VARSIZE_ANY_EXHDR(arg1);
	len2 = VARSIZE_ANY_EXHDR(arg2);

	/* Compare text as bttextcmp does, but always using C collation */
	result = varstr_cmp(a1p, len1, a2p, len2, C_COLLATION_OID);

	PG_FREE_IF_COPY(arg1, 0);
	PG_FREE_IF_COPY(arg2, 1);

	PG_RETURN_INT32(result);
}

Datum
gin_extract_jsonb(PG_FUNCTION_ARGS)
{
	Jsonb	   *jb = (Jsonb *) PG_GETARG_JSONB_P(0);
	int32	   *nentries = (int32 *) PG_GETARG_POINTER(1);
	int			total = JB_ROOT_COUNT(jb);
	JsonbIterator *it;
	JsonbValue	v;
	JsonbIteratorToken r;
	GinEntries	entries;

	/* If the root level is empty, we certainly have no keys */
	if (total == 0)
	{
		*nentries = 0;
		PG_RETURN_POINTER(NULL);
	}

	/* Otherwise, use 2 * root count as initial estimate of result size */
	init_gin_entries(&entries, 2 * total);

	it = JsonbIteratorInit(&jb->root);

	while ((r = JsonbIteratorNext(&it, &v, false)) != WJB_DONE)
	{
		switch (r)
		{
			case WJB_KEY:
				add_gin_entry(&entries, make_scalar_key(&v, true));
				break;
			case WJB_ELEM:
				/* Pretend string array elements are keys, see jsonb.h */
				add_gin_entry(&entries, make_scalar_key(&v, v.type == jbvString));
				break;
			case WJB_VALUE:
				add_gin_entry(&entries, make_scalar_key(&v, false));
				break;
			default:
				/* we can ignore structural items */
				break;
		}
	}

	*nentries = entries.count;

	PG_RETURN_POINTER(entries.buf);
}

/* Append JsonPathGinPathItem to JsonPathGinPath (jsonb_ops) */
static bool
jsonb_ops__add_path_item(JsonPathGinPath *path, JsonPathItem *jsp)
{
	JsonPathGinPathItem *pentry;
	Datum		keyName;

	switch (jsp->type)
	{
		case jpiRoot:
			path->items = NULL; /* reset path */
			return true;

		case jpiKey:
			{
				int			len;
				char	   *key = jspGetString(jsp, &len);

				keyName = make_text_key(JGINFLAG_KEY, key, len);
				break;
			}

		case jpiAny:
		case jpiAnyKey:
		case jpiAnyArray:
		case jpiIndexArray:
			keyName = PointerGetDatum(NULL);
			break;

		default:
			/* other path items like item methods are not supported */
			return false;
	}

	pentry = palloc(sizeof(*pentry));

	pentry->type = jsp->type;
	pentry->keyName = keyName;
	pentry->parent = path->items;

	path->items = pentry;

	return true;
}

/* Combine existing path hash with next key hash (jsonb_path_ops) */
static bool
jsonb_path_ops__add_path_item(JsonPathGinPath *path, JsonPathItem *jsp)
{
	switch (jsp->type)
	{
		case jpiRoot:
			path->hash = 0;		/* reset path hash */
			return true;

		case jpiKey:
			{
				JsonbValue	jbv;

				jbv.type = jbvString;
				jbv.val.string.val = jspGetString(jsp, &jbv.val.string.len);

				JsonbHashScalarValue(&jbv, &path->hash);
				return true;
			}

		case jpiIndexArray:
		case jpiAnyArray:
			return true;		/* path hash is unchanged */

		default:
			/* other items (wildcard paths, item methods) are not supported */
			return false;
	}
}

static JsonPathGinNode *
make_jsp_entry_node(Datum entry)
{
	JsonPathGinNode *node = palloc(offsetof(JsonPathGinNode, args));

	node->type = JSP_GIN_ENTRY;
	node->val.entryDatum = entry;

	return node;
}

static JsonPathGinNode *
make_jsp_entry_node_scalar(JsonbValue *scalar, bool iskey)
{
	return make_jsp_entry_node(make_scalar_key(scalar, iskey));
}

static JsonPathGinNode *
make_jsp_expr_node(JsonPathGinNodeType type, int nargs)
{
	JsonPathGinNode *node = palloc(offsetof(JsonPathGinNode, args) +
								   sizeof(node->args[0]) * nargs);

	node->type = type;
	node->val.nargs = nargs;

	return node;
}

static JsonPathGinNode *
make_jsp_expr_node_args(JsonPathGinNodeType type, List *args)
{
	JsonPathGinNode *node = make_jsp_expr_node(type, list_length(args));
	ListCell   *lc;
	int			i = 0;

	foreach(lc, args)
		node->args[i++] = lfirst(lc);

	return node;
}

static JsonPathGinNode *
make_jsp_expr_node_binary(JsonPathGinNodeType type,
						  JsonPathGinNode *arg1, JsonPathGinNode *arg2)
{
	JsonPathGinNode *node = make_jsp_expr_node(type, 2);

	node->args[0] = arg1;
	node->args[1] = arg2;

	return node;
}

/* Append a list of nodes from the jsonpath (jsonb_ops). */
static List *
jsonb_ops__extract_nodes(JsonPathGinContext *cxt, JsonPathGinPath path,
						 JsonbValue *scalar, List *nodes)
{
	JsonPathGinPathItem *pentry;

	if (scalar)
	{
		JsonPathGinNode *node;

		/*
		 * Append path entry nodes only if scalar is provided.  See header
		 * comment for details.
		 */
		for (pentry = path.items; pentry; pentry = pentry->parent)
		{
			if (pentry->type == jpiKey) /* only keys are indexed */
				nodes = lappend(nodes, make_jsp_entry_node(pentry->keyName));
		}

		/* Append scalar node for equality queries. */
		if (scalar->type == jbvString)
		{
			JsonPathGinPathItem *last = path.items;
			GinTernaryValue key_entry;

			/*
			 * Assuming that jsonb_ops interprets string array elements as
			 * keys, we may extract key or non-key entry or even both.  In the
			 * latter case we create OR-node.  It is possible in lax mode
			 * where arrays are automatically unwrapped, or in strict mode for
			 * jpiAny items.
			 */

			if (cxt->lax)
				key_entry = GIN_MAYBE;
			else if (!last)		/* root ($) */
				key_entry = GIN_FALSE;
			else if (last->type == jpiAnyArray || last->type == jpiIndexArray)
				key_entry = GIN_TRUE;
			else if (last->type == jpiAny)
				key_entry = GIN_MAYBE;
			else
				key_entry = GIN_FALSE;

			if (key_entry == GIN_MAYBE)
			{
				JsonPathGinNode *n1 = make_jsp_entry_node_scalar(scalar, true);
				JsonPathGinNode *n2 = make_jsp_entry_node_scalar(scalar, false);

				node = make_jsp_expr_node_binary(JSP_GIN_OR, n1, n2);
			}
			else
			{
				node = make_jsp_entry_node_scalar(scalar,
												  key_entry == GIN_TRUE);
			}
		}
		else
		{
			node = make_jsp_entry_node_scalar(scalar, false);
		}

		nodes = lappend(nodes, node);
	}

	return nodes;
}

/* Append a list of nodes from the jsonpath (jsonb_path_ops). */
static List *
jsonb_path_ops__extract_nodes(JsonPathGinContext *cxt, JsonPathGinPath path,
							  JsonbValue *scalar, List *nodes)
{
	if (scalar)
	{
		/* append path hash node for equality queries */
		uint32		hash = path.hash;

		JsonbHashScalarValue(scalar, &hash);

		return lappend(nodes,
					   make_jsp_entry_node(UInt32GetDatum(hash)));
	}
	else
	{
		/* jsonb_path_ops doesn't support EXISTS queries => nothing to append */
		return nodes;
	}
}

/*
 * Extract a list of expression nodes that need to be AND-ed by the caller.
 * Extracted expression is 'path == scalar' if 'scalar' is non-NULL, and
 * 'EXISTS(path)' otherwise.
 */
static List *
extract_jsp_path_expr_nodes(JsonPathGinContext *cxt, JsonPathGinPath path,
							JsonPathItem *jsp, JsonbValue *scalar)
{
	JsonPathItem next;
	List	   *nodes = NIL;

	for (;;)
	{
		switch (jsp->type)
		{
			case jpiCurrent:
				break;

			case jpiFilter:
				{
					JsonPathItem arg;
					JsonPathGinNode *filter;

					jspGetArg(jsp, &arg);

					filter = extract_jsp_bool_expr(cxt, path, &arg, false);

					if (filter)
						nodes = lappend(nodes, filter);

					break;
				}

			default:
				if (!cxt->add_path_item(&path, jsp))

					/*
					 * Path is not supported by the index opclass, return only
					 * the extracted filter nodes.
					 */
					return nodes;
				break;
		}

		if (!jspGetNext(jsp, &next))
			break;

		jsp = &next;
	}

	/*
	 * Append nodes from the path expression itself to the already extracted
	 * list of filter nodes.
	 */
	return cxt->extract_nodes(cxt, path, scalar, nodes);
}

/*
 * Extract an expression node from one of following jsonpath path expressions:
 *   EXISTS(jsp)    (when 'scalar' is NULL)
 *   jsp == scalar  (when 'scalar' is not NULL).
 *
 * The current path (@) is passed in 'path'.
 */
static JsonPathGinNode *
extract_jsp_path_expr(JsonPathGinContext *cxt, JsonPathGinPath path,
					  JsonPathItem *jsp, JsonbValue *scalar)
{
	/* extract a list of nodes to be AND-ed */
	List	   *nodes = extract_jsp_path_expr_nodes(cxt, path, jsp, scalar);

	if (list_length(nodes) <= 0)
		/* no nodes were extracted => full scan is needed for this path */
		return NULL;

	if (list_length(nodes) == 1)
		return linitial(nodes); /* avoid extra AND-node */

	/* construct AND-node for path with filters */
	return make_jsp_expr_node_args(JSP_GIN_AND, nodes);
}

/* Recursively extract nodes from the boolean jsonpath expression. */
static JsonPathGinNode *
extract_jsp_bool_expr(JsonPathGinContext *cxt, JsonPathGinPath path,
					  JsonPathItem *jsp, bool not)
{
	check_stack_depth();

	switch (jsp->type)
	{
		case jpiAnd:			/* expr && expr */
		case jpiOr:				/* expr || expr */
			{
				JsonPathItem arg;
				JsonPathGinNode *larg;
				JsonPathGinNode *rarg;
				JsonPathGinNodeType type;

				jspGetLeftArg(jsp, &arg);
				larg = extract_jsp_bool_expr(cxt, path, &arg, not);

				jspGetRightArg(jsp, &arg);
				rarg = extract_jsp_bool_expr(cxt, path, &arg, not);

				if (!larg || !rarg)
				{
					if (jsp->type == jpiOr)
						return NULL;

					return larg ? larg : rarg;
				}

				type = not ^ (jsp->type == jpiAnd) ? JSP_GIN_AND : JSP_GIN_OR;

				return make_jsp_expr_node_binary(type, larg, rarg);
			}

		case jpiNot:			/* !expr  */
			{
				JsonPathItem arg;

				jspGetArg(jsp, &arg);

				/* extract child expression inverting 'not' flag */
				return extract_jsp_bool_expr(cxt, path, &arg, !not);
			}

		case jpiExists:			/* EXISTS(path) */
			{
				JsonPathItem arg;

				if (not)
					return NULL;	/* NOT EXISTS is not supported */

				jspGetArg(jsp, &arg);

				return extract_jsp_path_expr(cxt, path, &arg, NULL);
			}

		case jpiNotEqual:

			/*
			 * 'not' == true case is not supported here because '!(path !=
			 * scalar)' is not equivalent to 'path == scalar' in the general
			 * case because of sequence comparison semantics: 'path == scalar'
			 * === 'EXISTS (path, @ == scalar)', '!(path != scalar)' ===
			 * 'FOR_ALL(path, @ == scalar)'. So, we should translate '!(path
			 * != scalar)' into GIN query 'path == scalar || EMPTY(path)', but
			 * 'EMPTY(path)' queries are not supported by the both jsonb
			 * opclasses.  However in strict mode we could omit 'EMPTY(path)'
			 * part if the path can return exactly one item (it does not
			 * contain wildcard accessors or item methods like .keyvalue()
			 * etc.).
			 */
			return NULL;

		case jpiEqual:			/* path == scalar */
			{
				JsonPathItem left_item;
				JsonPathItem right_item;
				JsonPathItem *path_item;
				JsonPathItem *scalar_item;
				JsonbValue	scalar;

				if (not)
					return NULL;

				jspGetLeftArg(jsp, &left_item);
				jspGetRightArg(jsp, &right_item);

				if (jspIsScalar(left_item.type))
				{
					scalar_item = &left_item;
					path_item = &right_item;
				}
				else if (jspIsScalar(right_item.type))
				{
					scalar_item = &right_item;
					path_item = &left_item;
				}
				else
					return NULL;	/* at least one operand should be a scalar */

				switch (scalar_item->type)
				{
					case jpiNull:
						scalar.type = jbvNull;
						break;
					case jpiBool:
						scalar.type = jbvBool;
						scalar.val.boolean = !!*scalar_item->content.value.data;
						break;
					case jpiNumeric:
						scalar.type = jbvNumeric;
						scalar.val.numeric =
							(Numeric) scalar_item->content.value.data;
						break;
					case jpiString:
						scalar.type = jbvString;
						scalar.val.string.val = scalar_item->content.value.data;
						scalar.val.string.len =
							scalar_item->content.value.datalen;
						break;
					default:
						elog(ERROR, "invalid scalar jsonpath item type: %d",
							 scalar_item->type);
						return NULL;
				}

				return extract_jsp_path_expr(cxt, path, path_item, &scalar);
			}

		default:
			return NULL;		/* not a boolean expression */
	}
}

/* Recursively emit all GIN entries found in the node tree */
static void
emit_jsp_gin_entries(JsonPathGinNode *node, GinEntries *entries)
{
	check_stack_depth();

	switch (node->type)
	{
		case JSP_GIN_ENTRY:
			/* replace datum with its index in the array */
			node->val.entryIndex = add_gin_entry(entries, node->val.entryDatum);
			break;

		case JSP_GIN_OR:
		case JSP_GIN_AND:
			{
				int			i;

				for (i = 0; i < node->val.nargs; i++)
					emit_jsp_gin_entries(node->args[i], entries);

				break;
			}
	}
}

/*
 * Recursively extract GIN entries from jsonpath query.
 * Root expression node is put into (*extra_data)[0].
 */
static Datum *
extract_jsp_query(JsonPath *jp, StrategyNumber strat, bool pathOps,
				  int32 *nentries, Pointer **extra_data)
{
	JsonPathGinContext cxt;
	JsonPathItem root;
	JsonPathGinNode *node;
	JsonPathGinPath path = {0};
	GinEntries	entries = {0};

	cxt.lax = (jp->header & JSONPATH_LAX) != 0;

	if (pathOps)
	{
		cxt.add_path_item = jsonb_path_ops__add_path_item;
		cxt.extract_nodes = jsonb_path_ops__extract_nodes;
	}
	else
	{
		cxt.add_path_item = jsonb_ops__add_path_item;
		cxt.extract_nodes = jsonb_ops__extract_nodes;
	}

	jspInit(&root, jp);

	node = strat == JsonbJsonpathExistsStrategyNumber
		? extract_jsp_path_expr(&cxt, path, &root, NULL)
		: extract_jsp_bool_expr(&cxt, path, &root, false);

	if (!node)
	{
		*nentries = 0;
		return NULL;
	}

	emit_jsp_gin_entries(node, &entries);

	*nentries = entries.count;
	if (!*nentries)
		return NULL;

	*extra_data = palloc0(sizeof(**extra_data) * entries.count);
	**extra_data = (Pointer) node;

	return entries.buf;
}

/*
 * Recursively execute jsonpath expression.
 * 'check' is a bool[] or a GinTernaryValue[] depending on 'ternary' flag.
 */
static GinTernaryValue
execute_jsp_gin_node(JsonPathGinNode *node, void *check, bool ternary)
{
	GinTernaryValue res;
	GinTernaryValue v;
	int			i;

	switch (node->type)
	{
		case JSP_GIN_AND:
			res = GIN_TRUE;
			for (i = 0; i < node->val.nargs; i++)
			{
				v = execute_jsp_gin_node(node->args[i], check, ternary);
				if (v == GIN_FALSE)
					return GIN_FALSE;
				else if (v == GIN_MAYBE)
					res = GIN_MAYBE;
			}
			return res;

		case JSP_GIN_OR:
			res = GIN_FALSE;
			for (i = 0; i < node->val.nargs; i++)
			{
				v = execute_jsp_gin_node(node->args[i], check, ternary);
				if (v == GIN_TRUE)
					return GIN_TRUE;
				else if (v == GIN_MAYBE)
					res = GIN_MAYBE;
			}
			return res;

		case JSP_GIN_ENTRY:
			{
				int			index = node->val.entryIndex;

				if (ternary)
					return ((GinTernaryValue *) check)[index];
				else
					return ((bool *) check)[index] ? GIN_TRUE : GIN_FALSE;
			}

		default:
			elog(ERROR, "invalid jsonpath gin node type: %d", node->type);
			return GIN_FALSE;	/* keep compiler quiet */
	}
}

Datum
gin_extract_jsonb_query(PG_FUNCTION_ARGS)
{
	int32	   *nentries = (int32 *) PG_GETARG_POINTER(1);
	StrategyNumber strategy = PG_GETARG_UINT16(2);
	int32	   *searchMode = (int32 *) PG_GETARG_POINTER(6);
	Datum	   *entries;

	if (strategy == JsonbContainsStrategyNumber)
	{
		/* Query is a jsonb, so just apply gin_extract_jsonb... */
		entries = (Datum *)
			DatumGetPointer(DirectFunctionCall2(gin_extract_jsonb,
												PG_GETARG_DATUM(0),
												PointerGetDatum(nentries)));
		/* ...although "contains {}" requires a full index scan */
		if (*nentries == 0)
			*searchMode = GIN_SEARCH_MODE_ALL;
	}
	else if (strategy == JsonbExistsStrategyNumber)
	{
		/* Query is a text string, which we treat as a key */
		text	   *query = PG_GETARG_TEXT_PP(0);

		*nentries = 1;
		entries = (Datum *) palloc(sizeof(Datum));
		entries[0] = make_text_key(JGINFLAG_KEY,
								   VARDATA_ANY(query),
								   VARSIZE_ANY_EXHDR(query));
	}
	else if (strategy == JsonbExistsAnyStrategyNumber ||
			 strategy == JsonbExistsAllStrategyNumber)
	{
		/* Query is a text array; each element is treated as a key */
		ArrayType  *query = PG_GETARG_ARRAYTYPE_P(0);
		Datum	   *key_datums;
		bool	   *key_nulls;
		int			key_count;
		int			i,
					j;

		deconstruct_array(query,
						  TEXTOID, -1, false, TYPALIGN_INT,
						  &key_datums, &key_nulls, &key_count);

		entries = (Datum *) palloc(sizeof(Datum) * key_count);

		for (i = 0, j = 0; i < key_count; i++)
		{
			/* Nulls in the array are ignored */
			if (key_nulls[i])
				continue;
			entries[j++] = make_text_key(JGINFLAG_KEY,
										 VARDATA(key_datums[i]),
										 VARSIZE(key_datums[i]) - VARHDRSZ);
		}

		*nentries = j;
		/* ExistsAll with no keys should match everything */
		if (j == 0 && strategy == JsonbExistsAllStrategyNumber)
			*searchMode = GIN_SEARCH_MODE_ALL;
	}
	else if (strategy == JsonbJsonpathPredicateStrategyNumber ||
			 strategy == JsonbJsonpathExistsStrategyNumber)
	{
		JsonPath   *jp = PG_GETARG_JSONPATH_P(0);
		Pointer   **extra_data = (Pointer **) PG_GETARG_POINTER(4);

		entries = extract_jsp_query(jp, strategy, false, nentries, extra_data);

		if (!entries)
			*searchMode = GIN_SEARCH_MODE_ALL;
	}
	else
	{
		elog(ERROR, "unrecognized strategy number: %d", strategy);
		entries = NULL;			/* keep compiler quiet */
	}

	PG_RETURN_POINTER(entries);
}

Datum
gin_consistent_jsonb(PG_FUNCTION_ARGS)
{
	bool	   *check = (bool *) PG_GETARG_POINTER(0);
	StrategyNumber strategy = PG_GETARG_UINT16(1);

	/* Jsonb	   *query = PG_GETARG_JSONB_P(2); */
	int32		nkeys = PG_GETARG_INT32(3);

	Pointer    *extra_data = (Pointer *) PG_GETARG_POINTER(4);
	bool	   *recheck = (bool *) PG_GETARG_POINTER(5);
	bool		res = true;
	int32		i;

	if (strategy == JsonbContainsStrategyNumber)
	{
		/*
		 * We must always recheck, since we can't tell from the index whether
		 * the positions of the matched items match the structure of the query
		 * object.  (Even if we could, we'd also have to worry about hashed
		 * keys and the index's failure to distinguish keys from string array
		 * elements.)  However, the tuple certainly doesn't match unless it
		 * contains all the query keys.
		 */
		*recheck = true;
		for (i = 0; i < nkeys; i++)
		{
			if (!check[i])
			{
				res = false;
				break;
			}
		}
	}
	else if (strategy == JsonbExistsStrategyNumber)
	{
		/*
		 * Although the key is certainly present in the index, we must recheck
		 * because (1) the key might be hashed, and (2) the index match might
		 * be for a key that's not at top level of the JSON object.  For (1),
		 * we could look at the query key to see if it's hashed and not
		 * recheck if not, but the index lacks enough info to tell about (2).
		 */
		*recheck = true;
		res = true;
	}
	else if (strategy == JsonbExistsAnyStrategyNumber)
	{
		/* As for plain exists, we must recheck */
		*recheck = true;
		res = true;
	}
	else if (strategy == JsonbExistsAllStrategyNumber)
	{
		/* As for plain exists, we must recheck */
		*recheck = true;
		/* ... but unless all the keys are present, we can say "false" */
		for (i = 0; i < nkeys; i++)
		{
			if (!check[i])
			{
				res = false;
				break;
			}
		}
	}
	else if (strategy == JsonbJsonpathPredicateStrategyNumber ||
			 strategy == JsonbJsonpathExistsStrategyNumber)
	{
		*recheck = true;

		if (nkeys > 0)
		{
			Assert(extra_data && extra_data[0]);
			res = execute_jsp_gin_node((JsonPathGinNode *) extra_data[0], check,
									   false) != GIN_FALSE;
		}
	}
	else
		elog(ERROR, "unrecognized strategy number: %d", strategy);

	PG_RETURN_BOOL(res);
}

Datum
gin_triconsistent_jsonb(PG_FUNCTION_ARGS)
{
	GinTernaryValue *check = (GinTernaryValue *) PG_GETARG_POINTER(0);
	StrategyNumber strategy = PG_GETARG_UINT16(1);

	/* Jsonb	   *query = PG_GETARG_JSONB_P(2); */
	int32		nkeys = PG_GETARG_INT32(3);
	Pointer    *extra_data = (Pointer *) PG_GETARG_POINTER(4);
	GinTernaryValue res = GIN_MAYBE;
	int32		i;

	/*
	 * Note that we never return GIN_TRUE, only GIN_MAYBE or GIN_FALSE; this
	 * corresponds to always forcing recheck in the regular consistent
	 * function, for the reasons listed there.
	 */
	if (strategy == JsonbContainsStrategyNumber ||
		strategy == JsonbExistsAllStrategyNumber)
	{
		/* All extracted keys must be present */
		for (i = 0; i < nkeys; i++)
		{
			if (check[i] == GIN_FALSE)
			{
				res = GIN_FALSE;
				break;
			}
		}
	}
	else if (strategy == JsonbExistsStrategyNumber ||
			 strategy == JsonbExistsAnyStrategyNumber)
	{
		/* At least one extracted key must be present */
		res = GIN_FALSE;
		for (i = 0; i < nkeys; i++)
		{
			if (check[i] == GIN_TRUE ||
				check[i] == GIN_MAYBE)
			{
				res = GIN_MAYBE;
				break;
			}
		}
	}
	else if (strategy == JsonbJsonpathPredicateStrategyNumber ||
			 strategy == JsonbJsonpathExistsStrategyNumber)
	{
		if (nkeys > 0)
		{
			Assert(extra_data && extra_data[0]);
			res = execute_jsp_gin_node((JsonPathGinNode *) extra_data[0], check,
									   true);

			/* Should always recheck the result */
			if (res == GIN_TRUE)
				res = GIN_MAYBE;
		}
	}
	else
		elog(ERROR, "unrecognized strategy number: %d", strategy);

	PG_RETURN_GIN_TERNARY_VALUE(res);
}

/*
 *
 * jsonb_path_ops GIN opclass support functions
 *
 * In a jsonb_path_ops index, the GIN keys are uint32 hashes, one per JSON
 * value; but the JSON key(s) leading to each value are also included in its
 * hash computation.  This means we can only support containment queries,
 * but the index can distinguish, for example, {"foo": 42} from {"bar": 42}
 * since different hashes will be generated.
 *
 */

Datum
gin_extract_jsonb_path(PG_FUNCTION_ARGS)
{
	Jsonb	   *jb = PG_GETARG_JSONB_P(0);
	int32	   *nentries = (int32 *) PG_GETARG_POINTER(1);
	int			total = JB_ROOT_COUNT(jb);
	JsonbIterator *it;
	JsonbValue	v;
	JsonbIteratorToken r;
	PathHashStack tail;
	PathHashStack *stack;
	GinEntries	entries;

	/* If the root level is empty, we certainly have no keys */
	if (total == 0)
	{
		*nentries = 0;
		PG_RETURN_POINTER(NULL);
	}

	/* Otherwise, use 2 * root count as initial estimate of result size */
	init_gin_entries(&entries, 2 * total);

	/* We keep a stack of partial hashes corresponding to parent key levels */
	tail.parent = NULL;
	tail.hash = 0;
	stack = &tail;

	it = JsonbIteratorInit(&jb->root);

	while ((r = JsonbIteratorNext(&it, &v, false)) != WJB_DONE)
	{
		PathHashStack *parent;

		switch (r)
		{
			case WJB_BEGIN_ARRAY:
			case WJB_BEGIN_OBJECT:
				/* Push a stack level for this object */
				parent = stack;
				stack = (PathHashStack *) palloc(sizeof(PathHashStack));

				/*
				 * We pass forward hashes from outer nesting levels so that
				 * the hashes for nested values will include outer keys as
				 * well as their own keys.
				 *
				 * Nesting an array within another array will not alter
				 * innermost scalar element hash values, but that seems
				 * inconsequential.
				 */
				stack->hash = parent->hash;
				stack->parent = parent;
				break;
			case WJB_KEY:
				/* mix this key into the current outer hash */
				JsonbHashScalarValue(&v, &stack->hash);
				/* hash is now ready to incorporate the value */
				break;
			case WJB_ELEM:
			case WJB_VALUE:
				/* mix the element or value's hash into the prepared hash */
				JsonbHashScalarValue(&v, &stack->hash);
				/* and emit an index entry */
				add_gin_entry(&entries, UInt32GetDatum(stack->hash));
				/* reset hash for next key, value, or sub-object */
				stack->hash = stack->parent->hash;
				break;
			case WJB_END_ARRAY:
			case WJB_END_OBJECT:
				/* Pop the stack */
				parent = stack->parent;
				pfree(stack);
				stack = parent;
				/* reset hash for next key, value, or sub-object */
				if (stack->parent)
					stack->hash = stack->parent->hash;
				else
					stack->hash = 0;
				break;
			default:
				elog(ERROR, "invalid JsonbIteratorNext rc: %d", (int) r);
		}
	}

	*nentries = entries.count;

	PG_RETURN_POINTER(entries.buf);
}

Datum
gin_extract_jsonb_query_path(PG_FUNCTION_ARGS)
{
	int32	   *nentries = (int32 *) PG_GETARG_POINTER(1);
	StrategyNumber strategy = PG_GETARG_UINT16(2);
	int32	   *searchMode = (int32 *) PG_GETARG_POINTER(6);
	Datum	   *entries;

	if (strategy == JsonbContainsStrategyNumber)
	{
		/* Query is a jsonb, so just apply gin_extract_jsonb_path ... */
		entries = (Datum *)
			DatumGetPointer(DirectFunctionCall2(gin_extract_jsonb_path,
												PG_GETARG_DATUM(0),
												PointerGetDatum(nentries)));

		/* ... although "contains {}" requires a full index scan */
		if (*nentries == 0)
			*searchMode = GIN_SEARCH_MODE_ALL;
	}
	else if (strategy == JsonbJsonpathPredicateStrategyNumber ||
			 strategy == JsonbJsonpathExistsStrategyNumber)
	{
		JsonPath   *jp = PG_GETARG_JSONPATH_P(0);
		Pointer   **extra_data = (Pointer **) PG_GETARG_POINTER(4);

		entries = extract_jsp_query(jp, strategy, true, nentries, extra_data);

		if (!entries)
			*searchMode = GIN_SEARCH_MODE_ALL;
	}
	else
	{
		elog(ERROR, "unrecognized strategy number: %d", strategy);
		entries = NULL;
	}

	PG_RETURN_POINTER(entries);
}

Datum
gin_consistent_jsonb_path(PG_FUNCTION_ARGS)
{
	bool	   *check = (bool *) PG_GETARG_POINTER(0);
	StrategyNumber strategy = PG_GETARG_UINT16(1);

	/* Jsonb	   *query = PG_GETARG_JSONB_P(2); */
	int32		nkeys = PG_GETARG_INT32(3);
	Pointer    *extra_data = (Pointer *) PG_GETARG_POINTER(4);
	bool	   *recheck = (bool *) PG_GETARG_POINTER(5);
	bool		res = true;
	int32		i;

	if (strategy == JsonbContainsStrategyNumber)
	{
		/*
		 * jsonb_path_ops is necessarily lossy, not only because of hash
		 * collisions but also because it doesn't preserve complete
		 * information about the structure of the JSON object.  Besides, there
		 * are some special rules around the containment of raw scalars in
		 * arrays that are not handled here.  So we must always recheck a
		 * match.  However, if not all of the keys are present, the tuple
		 * certainly doesn't match.
		 */
		*recheck = true;
		for (i = 0; i < nkeys; i++)
		{
			if (!check[i])
			{
				res = false;
				break;
			}
		}
	}
	else if (strategy == JsonbJsonpathPredicateStrategyNumber ||
			 strategy == JsonbJsonpathExistsStrategyNumber)
	{
		*recheck = true;

		if (nkeys > 0)
		{
			Assert(extra_data && extra_data[0]);
			res = execute_jsp_gin_node((JsonPathGinNode *) extra_data[0], check,
									   false) != GIN_FALSE;
		}
	}
	else
		elog(ERROR, "unrecognized strategy number: %d", strategy);

	PG_RETURN_BOOL(res);
}

Datum
gin_triconsistent_jsonb_path(PG_FUNCTION_ARGS)
{
	GinTernaryValue *check = (GinTernaryValue *) PG_GETARG_POINTER(0);
	StrategyNumber strategy = PG_GETARG_UINT16(1);

	/* Jsonb	   *query = PG_GETARG_JSONB_P(2); */
	int32		nkeys = PG_GETARG_INT32(3);
	Pointer    *extra_data = (Pointer *) PG_GETARG_POINTER(4);
	GinTernaryValue res = GIN_MAYBE;
	int32		i;

	if (strategy == JsonbContainsStrategyNumber)
	{
		/*
		 * Note that we never return GIN_TRUE, only GIN_MAYBE or GIN_FALSE;
		 * this corresponds to always forcing recheck in the regular
		 * consistent function, for the reasons listed there.
		 */
		for (i = 0; i < nkeys; i++)
		{
			if (check[i] == GIN_FALSE)
			{
				res = GIN_FALSE;
				break;
			}
		}
	}
	else if (strategy == JsonbJsonpathPredicateStrategyNumber ||
			 strategy == JsonbJsonpathExistsStrategyNumber)
	{
		if (nkeys > 0)
		{
			Assert(extra_data && extra_data[0]);
			res = execute_jsp_gin_node((JsonPathGinNode *) extra_data[0], check,
									   true);

			/* Should always recheck the result */
			if (res == GIN_TRUE)
				res = GIN_MAYBE;
		}
	}
	else
		elog(ERROR, "unrecognized strategy number: %d", strategy);

	PG_RETURN_GIN_TERNARY_VALUE(res);
}

/*
 * Construct a jsonb_ops GIN key from a flag byte and a textual representation
 * (which need not be null-terminated).  This function is responsible
 * for hashing overlength text representations; it will add the
 * JGINFLAG_HASHED bit to the flag value if it does that.
 */
static Datum
make_text_key(char flag, const char *str, int len)
{
	text	   *item;
	char		hashbuf[10];

	if (len > JGIN_MAXLENGTH)
	{
		uint32		hashval;

		hashval = DatumGetUInt32(hash_any((const unsigned char *) str, len));
		snprintf(hashbuf, sizeof(hashbuf), "%08x", hashval);
		str = hashbuf;
		len = 8;
		flag |= JGINFLAG_HASHED;
	}

	/*
	 * Now build the text Datum.  For simplicity we build a 4-byte-header
	 * varlena text Datum here, but we expect it will get converted to short
	 * header format when stored in the index.
	 */
	item = (text *) palloc(VARHDRSZ + len + 1);
	SET_VARSIZE(item, VARHDRSZ + len + 1);

	*VARDATA(item) = flag;

	memcpy(VARDATA(item) + 1, str, len);

	return PointerGetDatum(item);
}

/*
 * Create a textual representation of a JsonbValue that will serve as a GIN
 * key in a jsonb_ops index.  is_key is true if the JsonbValue is a key,
 * or if it is a string array element (since we pretend those are keys,
 * see jsonb.h).
 */
static Datum
make_scalar_key(const JsonbValue *scalarVal, bool is_key)
{
	Datum		item;
	char	   *cstr;

	switch (scalarVal->type)
	{
		case jbvNull:
			Assert(!is_key);
			item = make_text_key(JGINFLAG_NULL, "", 0);
			break;
		case jbvBool:
			Assert(!is_key);
			item = make_text_key(JGINFLAG_BOOL,
								 scalarVal->val.boolean ? "t" : "f", 1);
			break;
		case jbvNumeric:
			Assert(!is_key);

			/*
			 * A normalized textual representation, free of trailing zeroes,
			 * is required so that numerically equal values will produce equal
			 * strings.
			 *
			 * It isn't ideal that numerics are stored in a relatively bulky
			 * textual format.  However, it's a notationally convenient way of
			 * storing a "union" type in the GIN B-Tree, and indexing Jsonb
			 * strings takes precedence.
			 */
			cstr = numeric_normalize(scalarVal->val.numeric);
			item = make_text_key(JGINFLAG_NUM, cstr, strlen(cstr));
			pfree(cstr);
			break;
		case jbvString:
			item = make_text_key(is_key ? JGINFLAG_KEY : JGINFLAG_STR,
								 scalarVal->val.string.val,
								 scalarVal->val.string.len);
			break;
		default:
			elog(ERROR, "unrecognized jsonb scalar type: %d", scalarVal->type);
			item = 0;			/* keep compiler quiet */
			break;
	}

	return item;
}
