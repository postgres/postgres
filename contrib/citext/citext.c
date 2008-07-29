/*
 * $PostgreSQL: pgsql/contrib/citext/citext.c,v 1.1 2008/07/29 18:31:20 tgl Exp $
 */
#include "postgres.h"

#include "access/hash.h"
#include "fmgr.h"
#include "utils/builtins.h"
#include "utils/formatting.h"

#ifdef PG_MODULE_MAGIC
PG_MODULE_MAGIC;
#endif

/*
 *      ====================
 *      FORWARD DECLARATIONS
 *      ====================
 */

static int32  citextcmp      (text *left, text *right);
extern Datum  citext_cmp     (PG_FUNCTION_ARGS);
extern Datum  citext_hash    (PG_FUNCTION_ARGS);
extern Datum  citext_eq      (PG_FUNCTION_ARGS);
extern Datum  citext_ne      (PG_FUNCTION_ARGS);
extern Datum  citext_gt      (PG_FUNCTION_ARGS);
extern Datum  citext_ge      (PG_FUNCTION_ARGS);
extern Datum  citext_lt      (PG_FUNCTION_ARGS);
extern Datum  citext_le      (PG_FUNCTION_ARGS);
extern Datum  citext_smaller (PG_FUNCTION_ARGS);
extern Datum  citext_larger  (PG_FUNCTION_ARGS);

/*
 *      =================
 *      UTILITY FUNCTIONS
 *      =================
 */

/*
 * citextcmp()
 * Internal comparison function for citext strings.
 * Returns int32 negative, zero, or positive.
 */
static int32
citextcmp (text *left, text *right)
{
   char   *lcstr, *rcstr;
   int32	result;

   lcstr = str_tolower(VARDATA_ANY(left), VARSIZE_ANY_EXHDR(left));
   rcstr = str_tolower(VARDATA_ANY(right), VARSIZE_ANY_EXHDR(right));

   result = varstr_cmp(lcstr, strlen(lcstr),
					   rcstr, strlen(rcstr));

   pfree(lcstr);
   pfree(rcstr);

   return result;
}

/*
 *      ==================
 *      INDEXING FUNCTIONS
 *      ==================
 */

PG_FUNCTION_INFO_V1(citext_cmp);

Datum
citext_cmp(PG_FUNCTION_ARGS)
{
   text *left  = PG_GETARG_TEXT_PP(0);
   text *right = PG_GETARG_TEXT_PP(1);
   int32 result;

   result = citextcmp(left, right);

   PG_FREE_IF_COPY(left, 0);
   PG_FREE_IF_COPY(right, 1);

   PG_RETURN_INT32(result);
}

PG_FUNCTION_INFO_V1(citext_hash);

Datum
citext_hash(PG_FUNCTION_ARGS)
{
   text       *txt = PG_GETARG_TEXT_PP(0);
   char       *str;
   Datum       result;

   str    = str_tolower(VARDATA_ANY(txt), VARSIZE_ANY_EXHDR(txt));
   result = hash_any((unsigned char *) str, strlen(str));
   pfree(str);

   /* Avoid leaking memory for toasted inputs */
   PG_FREE_IF_COPY(txt, 0);

   PG_RETURN_DATUM(result);
}

/*
 *      ==================
 *      OPERATOR FUNCTIONS
 *      ==================
 */

PG_FUNCTION_INFO_V1(citext_eq);

Datum
citext_eq(PG_FUNCTION_ARGS)
{
   text *left  = PG_GETARG_TEXT_PP(0);
   text *right = PG_GETARG_TEXT_PP(1);
   char *lcstr, *rcstr;
   bool  result;

   /* We can't compare lengths in advance of downcasing ... */

   lcstr = str_tolower(VARDATA_ANY(left), VARSIZE_ANY_EXHDR(left));
   rcstr = str_tolower(VARDATA_ANY(right), VARSIZE_ANY_EXHDR(right));

   /*
    * Since we only care about equality or not-equality, we can
    * avoid all the expense of strcoll() here, and just do bitwise
    * comparison.
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
   text *left  = PG_GETARG_TEXT_PP(0);
   text *right = PG_GETARG_TEXT_PP(1);
   char *lcstr, *rcstr;
   bool  result;

   /* We can't compare lengths in advance of downcasing ... */

   lcstr = str_tolower(VARDATA_ANY(left), VARSIZE_ANY_EXHDR(left));
   rcstr = str_tolower(VARDATA_ANY(right), VARSIZE_ANY_EXHDR(right));

   /*
    * Since we only care about equality or not-equality, we can
    * avoid all the expense of strcoll() here, and just do bitwise
    * comparison.
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
   text *left  = PG_GETARG_TEXT_PP(0);
   text *right = PG_GETARG_TEXT_PP(1);
   bool  result;

   result = citextcmp(left, right) < 0;

   PG_FREE_IF_COPY(left, 0);
   PG_FREE_IF_COPY(right, 1);

   PG_RETURN_BOOL(result);
}

PG_FUNCTION_INFO_V1(citext_le);

Datum
citext_le(PG_FUNCTION_ARGS)
{
   text *left  = PG_GETARG_TEXT_PP(0);
   text *right = PG_GETARG_TEXT_PP(1);
   bool  result;

   result = citextcmp(left, right) <= 0;

   PG_FREE_IF_COPY(left, 0);
   PG_FREE_IF_COPY(right, 1);

   PG_RETURN_BOOL(result);
}

PG_FUNCTION_INFO_V1(citext_gt);

Datum
citext_gt(PG_FUNCTION_ARGS)
{
   text *left  = PG_GETARG_TEXT_PP(0);
   text *right = PG_GETARG_TEXT_PP(1);
   bool  result;

   result = citextcmp(left, right) > 0;

   PG_FREE_IF_COPY(left, 0);
   PG_FREE_IF_COPY(right, 1);

   PG_RETURN_BOOL(result);
}

PG_FUNCTION_INFO_V1(citext_ge);

Datum
citext_ge(PG_FUNCTION_ARGS)
{
   text *left  = PG_GETARG_TEXT_PP(0);
   text *right = PG_GETARG_TEXT_PP(1);
   bool  result;

   result = citextcmp(left, right) >= 0;

   PG_FREE_IF_COPY(left, 0);
   PG_FREE_IF_COPY(right, 1);

   PG_RETURN_BOOL(result);
}

/*
 *      ===================
 *      AGGREGATE FUNCTIONS
 *      ===================
 */

PG_FUNCTION_INFO_V1(citext_smaller);

Datum
citext_smaller(PG_FUNCTION_ARGS)
{
   text *left  = PG_GETARG_TEXT_PP(0);
   text *right = PG_GETARG_TEXT_PP(1);
   text *result;

   result = citextcmp(left, right) < 0 ? left : right;
   PG_RETURN_TEXT_P(result);
}

PG_FUNCTION_INFO_V1(citext_larger);

Datum
citext_larger(PG_FUNCTION_ARGS)
{
   text *left  = PG_GETARG_TEXT_PP(0);
   text *right = PG_GETARG_TEXT_PP(1);
   text *result;

   result = citextcmp(left, right) > 0 ? left : right;
   PG_RETURN_TEXT_P(result);
}
