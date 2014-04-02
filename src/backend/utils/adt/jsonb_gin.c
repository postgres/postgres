/*-------------------------------------------------------------------------
 *
 * jsonb_gin.c
 *	 GIN support functions for jsonb
 *
 * Copyright (c) 2014, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/jsonb_gin.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/gin.h"
#include "access/skey.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_type.h"
#include "utils/builtins.h"
#include "utils/jsonb.h"

typedef struct PathHashStack
{
	uint32	hash;
	struct PathHashStack *parent;
}	PathHashStack;

static text *make_text_key(const char *str, int len, char flag);
static text *make_scalar_key(const JsonbValue * scalarVal, char flag);

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
	Jsonb	   *jb = (Jsonb *) PG_GETARG_JSONB(0);
	int32	   *nentries = (int32 *) PG_GETARG_POINTER(1);
	Datum	   *entries = NULL;
	int			total = 2 * JB_ROOT_COUNT(jb);
	int			i = 0,
				r;
	JsonbIterator *it;
	JsonbValue	v;

	if (total == 0)
	{
		*nentries = 0;
		PG_RETURN_POINTER(NULL);
	}

	entries = (Datum *) palloc(sizeof(Datum) * total);

	it = JsonbIteratorInit(VARDATA(jb));

	while ((r = JsonbIteratorNext(&it, &v, false)) != WJB_DONE)
	{
		if (i >= total)
		{
			total *= 2;
			entries = (Datum *) repalloc(entries, sizeof(Datum) * total);
		}

		/*
		 * Serialize keys and elements equivalently,  but only when elements
		 * are Jsonb strings.  Otherwise, serialize elements as values.  Array
		 * elements are indexed as keys, for the benefit of
		 * JsonbExistsStrategyNumber.  Our definition of existence does not
		 * allow for checking the existence of a non-jbvString element (just
		 * like the definition of the underlying operator), because the
		 * operator takes a text rhs argument (which is taken as a proxy for an
		 * equivalent Jsonb string).
		 *
		 * The way existence is represented does not preclude an alternative
		 * existence operator, that takes as its rhs value an arbitrarily
		 * internally-typed Jsonb.  The only reason that isn't the case here is
		 * that the existence operator is only really intended to determine if
		 * an object has a certain key (object pair keys are of course
		 * invariably strings), which is extended to jsonb arrays.  You could
		 * think of the default Jsonb definition of existence as being
		 * equivalent to a definition where all types of scalar array elements
		 * are keys that we can check the existence of, while just forbidding
		 * non-string notation.  This inflexibility prevents the user from
		 * having to qualify that the rhs string is a raw scalar string (that
		 * is, naturally no internal string quoting in required for the text
		 * argument), and allows us to not set the reset flag for
		 * JsonbExistsStrategyNumber, since we know that keys are strings for
		 * both objects and arrays, and don't have to further account for type
		 * mismatch.  Not having to set the reset flag makes it less than
		 * tempting to tighten up the definition of existence to preclude array
		 * elements entirely, which would arguably be a simpler alternative.
		 * In any case the infrastructure used to implement the existence
		 * operator could trivially support this hypothetical, slightly
		 * distinct definition of existence.
		 */
		switch (r)
		{
			case WJB_KEY:
				/* Serialize key separately, for existence strategies */
				entries[i++] = PointerGetDatum(make_scalar_key(&v, JKEYELEM));
				break;
			case WJB_ELEM:
				if (v.type == jbvString)
					entries[i++] = PointerGetDatum(make_scalar_key(&v, JKEYELEM));
				else
					entries[i++] = PointerGetDatum(make_scalar_key(&v, JVAL));
				break;
			case WJB_VALUE:
				entries[i++] = PointerGetDatum(make_scalar_key(&v, JVAL));
				break;
			default:
				continue;
		}
	}

	*nentries = i;

	PG_RETURN_POINTER(entries);
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
		if (entries == NULL)
			*searchMode = GIN_SEARCH_MODE_ALL;
	}
	else if (strategy == JsonbExistsStrategyNumber)
	{
		text	   *query = PG_GETARG_TEXT_PP(0);
		text	   *item;

		*nentries = 1;
		entries = (Datum *) palloc(sizeof(Datum));
		item = make_text_key(VARDATA_ANY(query), VARSIZE_ANY_EXHDR(query),
							 JKEYELEM);
		entries[0] = PointerGetDatum(item);
	}
	else if (strategy == JsonbExistsAnyStrategyNumber ||
			 strategy == JsonbExistsAllStrategyNumber)
	{
		ArrayType  *query = PG_GETARG_ARRAYTYPE_P(0);
		Datum	   *key_datums;
		bool	   *key_nulls;
		int			key_count;
		int			i,
					j;
		text	   *item;

		deconstruct_array(query,
						  TEXTOID, -1, false, 'i',
						  &key_datums, &key_nulls, &key_count);

		entries = (Datum *) palloc(sizeof(Datum) * key_count);

		for (i = 0, j = 0; i < key_count; ++i)
		{
			/* Nulls in the array are ignored */
			if (key_nulls[i])
				continue;
			item = make_text_key(VARDATA(key_datums[i]),
								 VARSIZE(key_datums[i]) - VARHDRSZ,
								 JKEYELEM);
			entries[j++] = PointerGetDatum(item);
		}

		*nentries = j;
		/* ExistsAll with no keys should match everything */
		if (j == 0 && strategy == JsonbExistsAllStrategyNumber)
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

	/* Jsonb	   *query = PG_GETARG_JSONB(2); */
	int32		nkeys = PG_GETARG_INT32(3);

	/* Pointer	   *extra_data = (Pointer *) PG_GETARG_POINTER(4); */
	bool	   *recheck = (bool *) PG_GETARG_POINTER(5);
	bool		res = true;
	int32		i;

	if (strategy == JsonbContainsStrategyNumber)
	{
		/*
		 * Index doesn't have information about correspondence of Jsonb keys
		 * and values (as distinct from GIN keys, which a key/value pair is
		 * stored as), so invariably we recheck.  Besides, there are some
		 * special rules around the containment of raw scalar arrays and
		 * regular arrays that are not represented here.  However, if all of
		 * the keys are not present, that's sufficient reason to return false
		 * and finish immediately.
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
		/* Existence of key guaranteed in default search mode */
		*recheck = false;
		res = true;
	}
	else if (strategy == JsonbExistsAnyStrategyNumber)
	{
		/* Existence of key guaranteed in default search mode */
		*recheck = false;
		res = true;
	}
	else if (strategy == JsonbExistsAllStrategyNumber)
	{
		/* Testing for the presence of all keys gives an exact result */
		*recheck = false;
		for (i = 0; i < nkeys; i++)
		{
			if (!check[i])
			{
				res = false;
				break;
			}
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
	/* Jsonb	   *query = PG_GETARG_JSONB(2); */
	int32		nkeys = PG_GETARG_INT32(3);
	/* Pointer	   *extra_data = (Pointer *) PG_GETARG_POINTER(4); */
	GinTernaryValue res = GIN_TRUE;

	int32		i;

	if (strategy == JsonbContainsStrategyNumber)
	{
		bool	has_maybe = false;

		/*
		 * All extracted keys must be present.  Combination of GIN_MAYBE and
		 * GIN_TRUE gives GIN_MAYBE result because then all keys may be
		 * present.
		 */
		for (i = 0; i < nkeys; i++)
		{
			if (check[i] == GIN_FALSE)
			{
				res = GIN_FALSE;
				break;
			}
			if (check[i] == GIN_MAYBE)
			{
				res = GIN_MAYBE;
				has_maybe = true;
			}
		}

		/*
		 * Index doesn't have information about correspondence of Jsonb keys
		 * and values (as distinct from GIN keys, which a key/value pair is
		 * stored as), so invariably we recheck.  This is also reflected in how
		 * GIN_MAYBE is given in response to there being no GIN_MAYBE input.
		 */
		if (!has_maybe && res == GIN_TRUE)
			res = GIN_MAYBE;
	}
	else if (strategy == JsonbExistsStrategyNumber ||
			 strategy == JsonbExistsAnyStrategyNumber)
	{
		/* Existence of key guaranteed in default search mode */
		res = GIN_FALSE;
		for (i = 0; i < nkeys; i++)
		{
			if (check[i] == GIN_TRUE)
			{
				res = GIN_TRUE;
				break;
			}
			if (check[i] == GIN_MAYBE)
			{
				res = GIN_MAYBE;
			}
		}
	}
	else if (strategy == JsonbExistsAllStrategyNumber)
	{
		/* Testing for the presence of all keys gives an exact result */
		for (i = 0; i < nkeys; i++)
		{
			if (check[i] == GIN_FALSE)
			{
				res = GIN_FALSE;
				break;
			}
			if (check[i] == GIN_MAYBE)
			{
				res = GIN_MAYBE;
			}
		}
	}
	else
		elog(ERROR, "unrecognized strategy number: %d", strategy);

	PG_RETURN_GIN_TERNARY_VALUE(res);
}

/*
 *
 * jsonb_hash_ops GIN opclass support functions
 *
 */
Datum
gin_consistent_jsonb_hash(PG_FUNCTION_ARGS)
{
	bool	   *check = (bool *) PG_GETARG_POINTER(0);
	StrategyNumber strategy = PG_GETARG_UINT16(1);
	/* Jsonb	   *query = PG_GETARG_JSONB(2); */
	int32		nkeys = PG_GETARG_INT32(3);
	/* Pointer	   *extra_data = (Pointer *) PG_GETARG_POINTER(4); */
	bool	   *recheck = (bool *) PG_GETARG_POINTER(5);
	bool		res = true;
	int32		i;

	if (strategy != JsonbContainsStrategyNumber)
		elog(ERROR, "unrecognized strategy number: %d", strategy);

	/*
	 * jsonb_hash_ops index doesn't have information about correspondence
	 * of Jsonb keys and values (as distinct from GIN keys, which a
	 * key/value pair is stored as), so invariably we recheck.  Besides,
	 * there are some special rules around the containment of raw scalar
	 * arrays and regular arrays that are not represented here.  However,
	 * if all of the keys are not present, that's sufficient reason to
	 * return false and finish immediately.
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

	PG_RETURN_BOOL(res);
}

Datum
gin_triconsistent_jsonb_hash(PG_FUNCTION_ARGS)
{
	GinTernaryValue *check = (GinTernaryValue *) PG_GETARG_POINTER(0);
	StrategyNumber strategy = PG_GETARG_UINT16(1);
	/* Jsonb	   *query = PG_GETARG_JSONB(2); */
	int32		nkeys = PG_GETARG_INT32(3);
	/* Pointer	   *extra_data = (Pointer *) PG_GETARG_POINTER(4); */
	GinTernaryValue res = GIN_TRUE;
	int32			i;
	bool			has_maybe = false;

	if (strategy != JsonbContainsStrategyNumber)
		elog(ERROR, "unrecognized strategy number: %d", strategy);

	/*
	 * All extracted keys must be present.  A combination of GIN_MAYBE and
	 * GIN_TRUE induces a GIN_MAYBE result, because then all keys may be
	 * present.
	 */
	for (i = 0; i < nkeys; i++)
	{
		if (check[i] == GIN_FALSE)
		{
			res = GIN_FALSE;
			break;
		}
		if (check[i] == GIN_MAYBE)
		{
			res = GIN_MAYBE;
			has_maybe = true;
		}
	}

	/*
	 * jsonb_hash_ops index doesn't have information about correspondence of
	 * Jsonb keys and values (as distinct from GIN keys, which for this opclass
	 * are a hash of a pair, or a hash of just an element), so invariably we
	 * recheck.  This is also reflected in how GIN_MAYBE is given in response
	 * to there being no GIN_MAYBE input.
	 */
	if (!has_maybe && res == GIN_TRUE)
		res = GIN_MAYBE;

	PG_RETURN_GIN_TERNARY_VALUE(res);
}

Datum
gin_extract_jsonb_hash(PG_FUNCTION_ARGS)
{
	Jsonb	   *jb = PG_GETARG_JSONB(0);
	int32	   *nentries = (int32 *) PG_GETARG_POINTER(1);
	int			total = 2 * JB_ROOT_COUNT(jb);
	JsonbIterator *it;
	JsonbValue	v;
	PathHashStack tail;
	PathHashStack *stack;
	int			i = 0,
				r;
	Datum	   *entries = NULL;

	if (total == 0)
	{
		*nentries = 0;
		PG_RETURN_POINTER(NULL);
	}

	entries = (Datum *) palloc(sizeof(Datum) * total);

	it = JsonbIteratorInit(VARDATA(jb));

	tail.parent = NULL;
	tail.hash = 0;
	stack = &tail;

	while ((r = JsonbIteratorNext(&it, &v, false)) != WJB_DONE)
	{
		PathHashStack  *tmp;

		if (i >= total)
		{
			total *= 2;
			entries = (Datum *) repalloc(entries, sizeof(Datum) * total);
		}

		switch (r)
		{
			case WJB_BEGIN_ARRAY:
			case WJB_BEGIN_OBJECT:
				tmp = stack;
				stack = (PathHashStack *) palloc(sizeof(PathHashStack));

				/*
				 * Nesting an array within another array will not alter
				 * innermost scalar element hash values, but that seems
				 * inconsequential
				 */
				if (tmp->parent)
				{
					/*
					 * We pass forward hashes from previous container nesting
					 * levels so that nested arrays with an outermost nested
					 * object will have element hashes mixed with the outermost
					 * key.  It's also somewhat useful to have nested objects
					 * innermost values have hashes that are a function of not
					 * just their own key, but outer keys too.
					 */
					stack->hash = tmp->hash;
				}
				else
				{
					/*
					 * At least nested level, initialize with stable container
					 * type proxy value
					 */
					stack->hash = (r == WJB_BEGIN_ARRAY)? JB_FARRAY:JB_FOBJECT;
				}
				stack->parent = tmp;
				break;
			case WJB_KEY:
				/* Initialize hash from parent */
				stack->hash = stack->parent->hash;
				JsonbHashScalarValue(&v, &stack->hash);
				break;
			case WJB_ELEM:
				/* Elements have parent hash mixed in separately */
				stack->hash = stack->parent->hash;
			case WJB_VALUE:
				/* Element/value case */
				JsonbHashScalarValue(&v, &stack->hash);
				entries[i++] = stack->hash;
				break;
			case WJB_END_ARRAY:
			case WJB_END_OBJECT:
				/* Pop the stack */
				tmp = stack->parent;
				pfree(stack);
				stack = tmp;
				break;
			default:
				elog(ERROR, "invalid JsonbIteratorNext rc: %d", r);
		}
	}

	*nentries = i;

	PG_RETURN_POINTER(entries);
}

Datum
gin_extract_jsonb_query_hash(PG_FUNCTION_ARGS)
{
	int32	   *nentries = (int32 *) PG_GETARG_POINTER(1);
	StrategyNumber strategy = PG_GETARG_UINT16(2);
	int32	   *searchMode = (int32 *) PG_GETARG_POINTER(6);
	Datum	   *entries;

	if (strategy != JsonbContainsStrategyNumber)
		elog(ERROR, "unrecognized strategy number: %d", strategy);

	/* Query is a jsonb, so just apply gin_extract_jsonb... */
	entries = (Datum *)
		DatumGetPointer(DirectFunctionCall2(gin_extract_jsonb_hash,
											PG_GETARG_DATUM(0),
											PointerGetDatum(nentries)));

	/* ...although "contains {}" requires a full index scan */
	if (entries == NULL)
		*searchMode = GIN_SEARCH_MODE_ALL;

	PG_RETURN_POINTER(entries);
}

/*
 * Build a text value from a cstring and flag suitable for storage as a key
 * value
 */
static text *
make_text_key(const char *str, int len, char flag)
{
	text	   *item;

	item = (text *) palloc(VARHDRSZ + len + 1);
	SET_VARSIZE(item, VARHDRSZ + len + 1);

	*VARDATA(item) = flag;

	memcpy(VARDATA(item) + 1, str, len);

	return item;
}

/*
 * Create a textual representation of a jsonbValue for GIN storage.
 */
static text *
make_scalar_key(const JsonbValue * scalarVal, char flag)
{
	text	   *item;
	char	   *cstr;

	switch (scalarVal->type)
	{
		case jbvNull:
			item = make_text_key("n", 1, flag);
			break;
		case jbvBool:
			item = make_text_key(scalarVal->val.boolean ? "t" : "f", 1, flag);
			break;
		case jbvNumeric:
			/*
			 * A normalized textual representation, free of trailing zeroes is
			 * is required.
			 *
			 * It isn't ideal that numerics are stored in a relatively bulky
			 * textual format.  However, it's a notationally convenient way of
			 * storing a "union" type in the GIN B-Tree, and indexing Jsonb
			 * strings takes precedence.
			 */
			cstr = numeric_normalize(scalarVal->val.numeric);
			item = make_text_key(cstr, strlen(cstr), flag);
			pfree(cstr);
			break;
		case jbvString:
			item = make_text_key(scalarVal->val.string.val, scalarVal->val.string.len,
								 flag);
			break;
		default:
			elog(ERROR, "invalid jsonb scalar type");
	}

	return item;
}
