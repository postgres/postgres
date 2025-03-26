/*
 * contrib/citext/citext.c
 */
#include "postgres.h"

#include "catalog/pg_collation.h"
#include "common/hashfn.h"
#include "fmgr.h"
#include "utils/formatting.h"
#include "utils/varlena.h"
#include "varatt.h"

PG_MODULE_MAGIC_EXT(
					.name = "citext",
					.version = PG_VERSION
);

/*
 *		====================
 *		FORWARD DECLARATIONS
 *		====================
 */

static int32 citextcmp(text *left, text *right, Oid collid);
static int32 internal_citext_pattern_cmp(text *left, text *right, Oid collid);

/*
 *		=================
 *		UTILITY FUNCTIONS
 *		=================
 */

/*
 * citextcmp()
 * Internal comparison function for citext strings.
 * Returns int32 negative, zero, or positive.
 */
static int32
citextcmp(text *left, text *right, Oid collid)
{
	char	   *lcstr,
			   *rcstr;
	int32		result;

	/*
	 * We must do our str_tolower calls with DEFAULT_COLLATION_OID, not the
	 * input collation as you might expect.  This is so that the behavior of
	 * citext's equality and hashing functions is not collation-dependent.  We
	 * should change this once the core infrastructure is able to cope with
	 * collation-dependent equality and hashing functions.
	 */

	lcstr = str_tolower(VARDATA_ANY(left), VARSIZE_ANY_EXHDR(left), DEFAULT_COLLATION_OID);
	rcstr = str_tolower(VARDATA_ANY(right), VARSIZE_ANY_EXHDR(right), DEFAULT_COLLATION_OID);

	result = varstr_cmp(lcstr, strlen(lcstr),
						rcstr, strlen(rcstr),
						collid);

	pfree(lcstr);
	pfree(rcstr);

	return result;
}

/*
 * citext_pattern_cmp()
 * Internal character-by-character comparison function for citext strings.
 * Returns int32 negative, zero, or positive.
 */
static int32
internal_citext_pattern_cmp(text *left, text *right, Oid collid)
{
	char	   *lcstr,
			   *rcstr;
	int			llen,
				rlen;
	int32		result;

	lcstr = str_tolower(VARDATA_ANY(left), VARSIZE_ANY_EXHDR(left), DEFAULT_COLLATION_OID);
	rcstr = str_tolower(VARDATA_ANY(right), VARSIZE_ANY_EXHDR(right), DEFAULT_COLLATION_OID);

	llen = strlen(lcstr);
	rlen = strlen(rcstr);

	result = memcmp(lcstr, rcstr, Min(llen, rlen));
	if (result == 0)
	{
		if (llen < rlen)
			result = -1;
		else if (llen > rlen)
			result = 1;
	}

	pfree(lcstr);
	pfree(rcstr);

	return result;
}

/*
 *		==================
 *		INDEXING FUNCTIONS
 *		==================
 */

PG_FUNCTION_INFO_V1(citext_cmp);

Datum
citext_cmp(PG_FUNCTION_ARGS)
{
	text	   *left = PG_GETARG_TEXT_PP(0);
	text	   *right = PG_GETARG_TEXT_PP(1);
	int32		result;

	result = citextcmp(left, right, PG_GET_COLLATION());

	PG_FREE_IF_COPY(left, 0);
	PG_FREE_IF_COPY(right, 1);

	PG_RETURN_INT32(result);
}

PG_FUNCTION_INFO_V1(citext_pattern_cmp);

Datum
citext_pattern_cmp(PG_FUNCTION_ARGS)
{
	text	   *left = PG_GETARG_TEXT_PP(0);
	text	   *right = PG_GETARG_TEXT_PP(1);
	int32		result;

	result = internal_citext_pattern_cmp(left, right, PG_GET_COLLATION());

	PG_FREE_IF_COPY(left, 0);
	PG_FREE_IF_COPY(right, 1);

	PG_RETURN_INT32(result);
}

PG_FUNCTION_INFO_V1(citext_hash);

Datum
citext_hash(PG_FUNCTION_ARGS)
{
	text	   *txt = PG_GETARG_TEXT_PP(0);
	char	   *str;
	Datum		result;

	str = str_tolower(VARDATA_ANY(txt), VARSIZE_ANY_EXHDR(txt), DEFAULT_COLLATION_OID);
	result = hash_any((unsigned char *) str, strlen(str));
	pfree(str);

	/* Avoid leaking memory for toasted inputs */
	PG_FREE_IF_COPY(txt, 0);

	PG_RETURN_DATUM(result);
}

PG_FUNCTION_INFO_V1(citext_hash_extended);

Datum
citext_hash_extended(PG_FUNCTION_ARGS)
{
	text	   *txt = PG_GETARG_TEXT_PP(0);
	uint64		seed = PG_GETARG_INT64(1);
	char	   *str;
	Datum		result;

	str = str_tolower(VARDATA_ANY(txt), VARSIZE_ANY_EXHDR(txt), DEFAULT_COLLATION_OID);
	result = hash_any_extended((unsigned char *) str, strlen(str), seed);
	pfree(str);

	/* Avoid leaking memory for toasted inputs */
	PG_FREE_IF_COPY(txt, 0);

	PG_RETURN_DATUM(result);
}

/*
 *		==================
 *		OPERATOR FUNCTIONS
 *		==================
 */

PG_FUNCTION_INFO_V1(citext_eq);

Datum
citext_eq(PG_FUNCTION_ARGS)
{
	text	   *left = PG_GETARG_TEXT_PP(0);
	text	   *right = PG_GETARG_TEXT_PP(1);
	char	   *lcstr,
			   *rcstr;
	bool		result;

	/* We can't compare lengths in advance of downcasing ... */

	lcstr = str_tolower(VARDATA_ANY(left), VARSIZE_ANY_EXHDR(left), DEFAULT_COLLATION_OID);
	rcstr = str_tolower(VARDATA_ANY(right), VARSIZE_ANY_EXHDR(right), DEFAULT_COLLATION_OID);

	/*
	 * Since we only care about equality or not-equality, we can avoid all the
	 * expense of strcoll() here, and just do bitwise comparison.
	 */
	result = (strcmp(lcstr, rcstr) == 0);

	pfree(lcstr);
	pfree(rcstr);
	PG_FREE_IF_COPY(left, 0);
	PG_FREE_IF_COPY(right, 1);

	PG_RETURN_BOOL(result);
}

PG_FUNCTION_INFO_V1(citext_ne);

Datum
citext_ne(PG_FUNCTION_ARGS)
{
	text	   *left = PG_GETARG_TEXT_PP(0);
	text	   *right = PG_GETARG_TEXT_PP(1);
	char	   *lcstr,
			   *rcstr;
	bool		result;

	/* We can't compare lengths in advance of downcasing ... */

	lcstr = str_tolower(VARDATA_ANY(left), VARSIZE_ANY_EXHDR(left), DEFAULT_COLLATION_OID);
	rcstr = str_tolower(VARDATA_ANY(right), VARSIZE_ANY_EXHDR(right), DEFAULT_COLLATION_OID);

	/*
	 * Since we only care about equality or not-equality, we can avoid all the
	 * expense of strcoll() here, and just do bitwise comparison.
	 */
	result = (strcmp(lcstr, rcstr) != 0);

	pfree(lcstr);
	pfree(rcstr);
	PG_FREE_IF_COPY(left, 0);
	PG_FREE_IF_COPY(right, 1);

	PG_RETURN_BOOL(result);
}

PG_FUNCTION_INFO_V1(citext_lt);

Datum
citext_lt(PG_FUNCTION_ARGS)
{
	text	   *left = PG_GETARG_TEXT_PP(0);
	text	   *right = PG_GETARG_TEXT_PP(1);
	bool		result;

	result = citextcmp(left, right, PG_GET_COLLATION()) < 0;

	PG_FREE_IF_COPY(left, 0);
	PG_FREE_IF_COPY(right, 1);

	PG_RETURN_BOOL(result);
}

PG_FUNCTION_INFO_V1(citext_le);

Datum
citext_le(PG_FUNCTION_ARGS)
{
	text	   *left = PG_GETARG_TEXT_PP(0);
	text	   *right = PG_GETARG_TEXT_PP(1);
	bool		result;

	result = citextcmp(left, right, PG_GET_COLLATION()) <= 0;

	PG_FREE_IF_COPY(left, 0);
	PG_FREE_IF_COPY(right, 1);

	PG_RETURN_BOOL(result);
}

PG_FUNCTION_INFO_V1(citext_gt);

Datum
citext_gt(PG_FUNCTION_ARGS)
{
	text	   *left = PG_GETARG_TEXT_PP(0);
	text	   *right = PG_GETARG_TEXT_PP(1);
	bool		result;

	result = citextcmp(left, right, PG_GET_COLLATION()) > 0;

	PG_FREE_IF_COPY(left, 0);
	PG_FREE_IF_COPY(right, 1);

	PG_RETURN_BOOL(result);
}

PG_FUNCTION_INFO_V1(citext_ge);

Datum
citext_ge(PG_FUNCTION_ARGS)
{
	text	   *left = PG_GETARG_TEXT_PP(0);
	text	   *right = PG_GETARG_TEXT_PP(1);
	bool		result;

	result = citextcmp(left, right, PG_GET_COLLATION()) >= 0;

	PG_FREE_IF_COPY(left, 0);
	PG_FREE_IF_COPY(right, 1);

	PG_RETURN_BOOL(result);
}

PG_FUNCTION_INFO_V1(citext_pattern_lt);

Datum
citext_pattern_lt(PG_FUNCTION_ARGS)
{
	text	   *left = PG_GETARG_TEXT_PP(0);
	text	   *right = PG_GETARG_TEXT_PP(1);
	bool		result;

	result = internal_citext_pattern_cmp(left, right, PG_GET_COLLATION()) < 0;

	PG_FREE_IF_COPY(left, 0);
	PG_FREE_IF_COPY(right, 1);

	PG_RETURN_BOOL(result);
}

PG_FUNCTION_INFO_V1(citext_pattern_le);

Datum
citext_pattern_le(PG_FUNCTION_ARGS)
{
	text	   *left = PG_GETARG_TEXT_PP(0);
	text	   *right = PG_GETARG_TEXT_PP(1);
	bool		result;

	result = internal_citext_pattern_cmp(left, right, PG_GET_COLLATION()) <= 0;

	PG_FREE_IF_COPY(left, 0);
	PG_FREE_IF_COPY(right, 1);

	PG_RETURN_BOOL(result);
}

PG_FUNCTION_INFO_V1(citext_pattern_gt);

Datum
citext_pattern_gt(PG_FUNCTION_ARGS)
{
	text	   *left = PG_GETARG_TEXT_PP(0);
	text	   *right = PG_GETARG_TEXT_PP(1);
	bool		result;

	result = internal_citext_pattern_cmp(left, right, PG_GET_COLLATION()) > 0;

	PG_FREE_IF_COPY(left, 0);
	PG_FREE_IF_COPY(right, 1);

	PG_RETURN_BOOL(result);
}

PG_FUNCTION_INFO_V1(citext_pattern_ge);

Datum
citext_pattern_ge(PG_FUNCTION_ARGS)
{
	text	   *left = PG_GETARG_TEXT_PP(0);
	text	   *right = PG_GETARG_TEXT_PP(1);
	bool		result;

	result = internal_citext_pattern_cmp(left, right, PG_GET_COLLATION()) >= 0;

	PG_FREE_IF_COPY(left, 0);
	PG_FREE_IF_COPY(right, 1);

	PG_RETURN_BOOL(result);
}

/*
 *		===================
 *		AGGREGATE FUNCTIONS
 *		===================
 */

PG_FUNCTION_INFO_V1(citext_smaller);

Datum
citext_smaller(PG_FUNCTION_ARGS)
{
	text	   *left = PG_GETARG_TEXT_PP(0);
	text	   *right = PG_GETARG_TEXT_PP(1);
	text	   *result;

	result = citextcmp(left, right, PG_GET_COLLATION()) < 0 ? left : right;
	PG_RETURN_TEXT_P(result);
}

PG_FUNCTION_INFO_V1(citext_larger);

Datum
citext_larger(PG_FUNCTION_ARGS)
{
	text	   *left = PG_GETARG_TEXT_PP(0);
	text	   *right = PG_GETARG_TEXT_PP(1);
	text	   *result;

	result = citextcmp(left, right, PG_GET_COLLATION()) > 0 ? left : right;
	PG_RETURN_TEXT_P(result);
}
