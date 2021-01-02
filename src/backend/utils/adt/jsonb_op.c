/*-------------------------------------------------------------------------
 *
 * jsonb_op.c
 *	 Special operators for jsonb only, used by various index access methods
 *
 * Copyright (c) 2014-2021, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/jsonb_op.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_type.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/jsonb.h"

Datum
jsonb_exists(PG_FUNCTION_ARGS)
{
	Jsonb	   *jb = PG_GETARG_JSONB_P(0);
	text	   *key = PG_GETARG_TEXT_PP(1);
	JsonbValue	kval;
	JsonbValue *v = NULL;

	/*
	 * We only match Object keys (which are naturally always Strings), or
	 * string elements in arrays.  In particular, we do not match non-string
	 * scalar elements.  Existence of a key/element is only considered at the
	 * top level.  No recursion occurs.
	 */
	kval.type = jbvString;
	kval.val.string.val = VARDATA_ANY(key);
	kval.val.string.len = VARSIZE_ANY_EXHDR(key);

	v = findJsonbValueFromContainer(&jb->root,
									JB_FOBJECT | JB_FARRAY,
									&kval);

	PG_RETURN_BOOL(v != NULL);
}

Datum
jsonb_exists_any(PG_FUNCTION_ARGS)
{
	Jsonb	   *jb = PG_GETARG_JSONB_P(0);
	ArrayType  *keys = PG_GETARG_ARRAYTYPE_P(1);
	int			i;
	Datum	   *key_datums;
	bool	   *key_nulls;
	int			elem_count;

	deconstruct_array(keys, TEXTOID, -1, false, TYPALIGN_INT,
					  &key_datums, &key_nulls, &elem_count);

	for (i = 0; i < elem_count; i++)
	{
		JsonbValue	strVal;

		if (key_nulls[i])
			continue;

		strVal.type = jbvString;
		strVal.val.string.val = VARDATA(key_datums[i]);
		strVal.val.string.len = VARSIZE(key_datums[i]) - VARHDRSZ;

		if (findJsonbValueFromContainer(&jb->root,
										JB_FOBJECT | JB_FARRAY,
										&strVal) != NULL)
			PG_RETURN_BOOL(true);
	}

	PG_RETURN_BOOL(false);
}

Datum
jsonb_exists_all(PG_FUNCTION_ARGS)
{
	Jsonb	   *jb = PG_GETARG_JSONB_P(0);
	ArrayType  *keys = PG_GETARG_ARRAYTYPE_P(1);
	int			i;
	Datum	   *key_datums;
	bool	   *key_nulls;
	int			elem_count;

	deconstruct_array(keys, TEXTOID, -1, false, TYPALIGN_INT,
					  &key_datums, &key_nulls, &elem_count);

	for (i = 0; i < elem_count; i++)
	{
		JsonbValue	strVal;

		if (key_nulls[i])
			continue;

		strVal.type = jbvString;
		strVal.val.string.val = VARDATA(key_datums[i]);
		strVal.val.string.len = VARSIZE(key_datums[i]) - VARHDRSZ;

		if (findJsonbValueFromContainer(&jb->root,
										JB_FOBJECT | JB_FARRAY,
										&strVal) == NULL)
			PG_RETURN_BOOL(false);
	}

	PG_RETURN_BOOL(true);
}

Datum
jsonb_contains(PG_FUNCTION_ARGS)
{
	Jsonb	   *val = PG_GETARG_JSONB_P(0);
	Jsonb	   *tmpl = PG_GETARG_JSONB_P(1);

	JsonbIterator *it1,
			   *it2;

	if (JB_ROOT_IS_OBJECT(val) != JB_ROOT_IS_OBJECT(tmpl))
		PG_RETURN_BOOL(false);

	it1 = JsonbIteratorInit(&val->root);
	it2 = JsonbIteratorInit(&tmpl->root);

	PG_RETURN_BOOL(JsonbDeepContains(&it1, &it2));
}

Datum
jsonb_contained(PG_FUNCTION_ARGS)
{
	/* Commutator of "contains" */
	Jsonb	   *tmpl = PG_GETARG_JSONB_P(0);
	Jsonb	   *val = PG_GETARG_JSONB_P(1);

	JsonbIterator *it1,
			   *it2;

	if (JB_ROOT_IS_OBJECT(val) != JB_ROOT_IS_OBJECT(tmpl))
		PG_RETURN_BOOL(false);

	it1 = JsonbIteratorInit(&val->root);
	it2 = JsonbIteratorInit(&tmpl->root);

	PG_RETURN_BOOL(JsonbDeepContains(&it1, &it2));
}

Datum
jsonb_ne(PG_FUNCTION_ARGS)
{
	Jsonb	   *jba = PG_GETARG_JSONB_P(0);
	Jsonb	   *jbb = PG_GETARG_JSONB_P(1);
	bool		res;

	res = (compareJsonbContainers(&jba->root, &jbb->root) != 0);

	PG_FREE_IF_COPY(jba, 0);
	PG_FREE_IF_COPY(jbb, 1);
	PG_RETURN_BOOL(res);
}

/*
 * B-Tree operator class operators, support function
 */
Datum
jsonb_lt(PG_FUNCTION_ARGS)
{
	Jsonb	   *jba = PG_GETARG_JSONB_P(0);
	Jsonb	   *jbb = PG_GETARG_JSONB_P(1);
	bool		res;

	res = (compareJsonbContainers(&jba->root, &jbb->root) < 0);

	PG_FREE_IF_COPY(jba, 0);
	PG_FREE_IF_COPY(jbb, 1);
	PG_RETURN_BOOL(res);
}

Datum
jsonb_gt(PG_FUNCTION_ARGS)
{
	Jsonb	   *jba = PG_GETARG_JSONB_P(0);
	Jsonb	   *jbb = PG_GETARG_JSONB_P(1);
	bool		res;

	res = (compareJsonbContainers(&jba->root, &jbb->root) > 0);

	PG_FREE_IF_COPY(jba, 0);
	PG_FREE_IF_COPY(jbb, 1);
	PG_RETURN_BOOL(res);
}

Datum
jsonb_le(PG_FUNCTION_ARGS)
{
	Jsonb	   *jba = PG_GETARG_JSONB_P(0);
	Jsonb	   *jbb = PG_GETARG_JSONB_P(1);
	bool		res;

	res = (compareJsonbContainers(&jba->root, &jbb->root) <= 0);

	PG_FREE_IF_COPY(jba, 0);
	PG_FREE_IF_COPY(jbb, 1);
	PG_RETURN_BOOL(res);
}

Datum
jsonb_ge(PG_FUNCTION_ARGS)
{
	Jsonb	   *jba = PG_GETARG_JSONB_P(0);
	Jsonb	   *jbb = PG_GETARG_JSONB_P(1);
	bool		res;

	res = (compareJsonbContainers(&jba->root, &jbb->root) >= 0);

	PG_FREE_IF_COPY(jba, 0);
	PG_FREE_IF_COPY(jbb, 1);
	PG_RETURN_BOOL(res);
}

Datum
jsonb_eq(PG_FUNCTION_ARGS)
{
	Jsonb	   *jba = PG_GETARG_JSONB_P(0);
	Jsonb	   *jbb = PG_GETARG_JSONB_P(1);
	bool		res;

	res = (compareJsonbContainers(&jba->root, &jbb->root) == 0);

	PG_FREE_IF_COPY(jba, 0);
	PG_FREE_IF_COPY(jbb, 1);
	PG_RETURN_BOOL(res);
}

Datum
jsonb_cmp(PG_FUNCTION_ARGS)
{
	Jsonb	   *jba = PG_GETARG_JSONB_P(0);
	Jsonb	   *jbb = PG_GETARG_JSONB_P(1);
	int			res;

	res = compareJsonbContainers(&jba->root, &jbb->root);

	PG_FREE_IF_COPY(jba, 0);
	PG_FREE_IF_COPY(jbb, 1);
	PG_RETURN_INT32(res);
}

/*
 * Hash operator class jsonb hashing function
 */
Datum
jsonb_hash(PG_FUNCTION_ARGS)
{
	Jsonb	   *jb = PG_GETARG_JSONB_P(0);
	JsonbIterator *it;
	JsonbValue	v;
	JsonbIteratorToken r;
	uint32		hash = 0;

	if (JB_ROOT_COUNT(jb) == 0)
		PG_RETURN_INT32(0);

	it = JsonbIteratorInit(&jb->root);

	while ((r = JsonbIteratorNext(&it, &v, false)) != WJB_DONE)
	{
		switch (r)
		{
				/* Rotation is left to JsonbHashScalarValue() */
			case WJB_BEGIN_ARRAY:
				hash ^= JB_FARRAY;
				break;
			case WJB_BEGIN_OBJECT:
				hash ^= JB_FOBJECT;
				break;
			case WJB_KEY:
			case WJB_VALUE:
			case WJB_ELEM:
				JsonbHashScalarValue(&v, &hash);
				break;
			case WJB_END_ARRAY:
			case WJB_END_OBJECT:
				break;
			default:
				elog(ERROR, "invalid JsonbIteratorNext rc: %d", (int) r);
		}
	}

	PG_FREE_IF_COPY(jb, 0);
	PG_RETURN_INT32(hash);
}

Datum
jsonb_hash_extended(PG_FUNCTION_ARGS)
{
	Jsonb	   *jb = PG_GETARG_JSONB_P(0);
	uint64		seed = PG_GETARG_INT64(1);
	JsonbIterator *it;
	JsonbValue	v;
	JsonbIteratorToken r;
	uint64		hash = 0;

	if (JB_ROOT_COUNT(jb) == 0)
		PG_RETURN_UINT64(seed);

	it = JsonbIteratorInit(&jb->root);

	while ((r = JsonbIteratorNext(&it, &v, false)) != WJB_DONE)
	{
		switch (r)
		{
				/* Rotation is left to JsonbHashScalarValueExtended() */
			case WJB_BEGIN_ARRAY:
				hash ^= ((uint64) JB_FARRAY) << 32 | JB_FARRAY;
				break;
			case WJB_BEGIN_OBJECT:
				hash ^= ((uint64) JB_FOBJECT) << 32 | JB_FOBJECT;
				break;
			case WJB_KEY:
			case WJB_VALUE:
			case WJB_ELEM:
				JsonbHashScalarValueExtended(&v, &hash, seed);
				break;
			case WJB_END_ARRAY:
			case WJB_END_OBJECT:
				break;
			default:
				elog(ERROR, "invalid JsonbIteratorNext rc: %d", (int) r);
		}
	}

	PG_FREE_IF_COPY(jb, 0);
	PG_RETURN_UINT64(hash);
}
