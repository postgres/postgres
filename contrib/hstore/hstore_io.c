/*
 * contrib/hstore/hstore_io.c
 */
#include "postgres.h"

#include <ctype.h>

#include "access/htup_details.h"
#include "catalog/pg_type.h"
#include "common/jsonapi.h"
#include "funcapi.h"
#include "hstore.h"
#include "lib/stringinfo.h"
#include "libpq/pqformat.h"
#include "nodes/miscnodes.h"
#include "parser/scansup.h"
#include "utils/builtins.h"
#include "utils/json.h"
#include "utils/jsonb.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/typcache.h"

PG_MODULE_MAGIC;

/* old names for C functions */
HSTORE_POLLUTE(hstore_from_text, tconvert);


typedef struct
{
	char	   *begin;
	char	   *ptr;
	char	   *cur;
	char	   *word;
	int			wordlen;
	Node	   *escontext;

	Pairs	   *pairs;
	int			pcur;
	int			plen;
} HSParser;

static bool hstoreCheckKeyLength(size_t len, HSParser *state);
static bool hstoreCheckValLength(size_t len, HSParser *state);


#define RESIZEPRSBUF \
do { \
		if ( state->cur - state->word + 1 >= state->wordlen ) \
		{ \
				int32 clen = state->cur - state->word; \
				state->wordlen *= 2; \
				state->word = (char*)repalloc( (void*)state->word, state->wordlen ); \
				state->cur = state->word + clen; \
		} \
} while (0)

#define PRSSYNTAXERROR return prssyntaxerror(state)

static bool
prssyntaxerror(HSParser *state)
{
	errsave(state->escontext,
			(errcode(ERRCODE_SYNTAX_ERROR),
			 errmsg("syntax error in hstore, near \"%.*s\" at position %d",
					pg_mblen(state->ptr), state->ptr,
					(int) (state->ptr - state->begin))));
	/* In soft error situation, return false as convenience for caller */
	return false;
}

#define PRSEOF return prseof(state)

static bool
prseof(HSParser *state)
{
	errsave(state->escontext,
			(errcode(ERRCODE_SYNTAX_ERROR),
			 errmsg("syntax error in hstore: unexpected end of string")));
	/* In soft error situation, return false as convenience for caller */
	return false;
}


#define GV_WAITVAL 0
#define GV_INVAL 1
#define GV_INESCVAL 2
#define GV_WAITESCIN 3
#define GV_WAITESCESCIN 4

static bool
get_val(HSParser *state, bool ignoreeq, bool *escaped)
{
	int			st = GV_WAITVAL;

	state->wordlen = 32;
	state->cur = state->word = palloc(state->wordlen);
	*escaped = false;

	while (1)
	{
		if (st == GV_WAITVAL)
		{
			if (*(state->ptr) == '"')
			{
				*escaped = true;
				st = GV_INESCVAL;
			}
			else if (*(state->ptr) == '\0')
			{
				return false;
			}
			else if (*(state->ptr) == '=' && !ignoreeq)
			{
				PRSSYNTAXERROR;
			}
			else if (*(state->ptr) == '\\')
			{
				st = GV_WAITESCIN;
			}
			else if (!scanner_isspace((unsigned char) *(state->ptr)))
			{
				*(state->cur) = *(state->ptr);
				state->cur++;
				st = GV_INVAL;
			}
		}
		else if (st == GV_INVAL)
		{
			if (*(state->ptr) == '\\')
			{
				st = GV_WAITESCIN;
			}
			else if (*(state->ptr) == '=' && !ignoreeq)
			{
				state->ptr--;
				return true;
			}
			else if (*(state->ptr) == ',' && ignoreeq)
			{
				state->ptr--;
				return true;
			}
			else if (scanner_isspace((unsigned char) *(state->ptr)))
			{
				return true;
			}
			else if (*(state->ptr) == '\0')
			{
				state->ptr--;
				return true;
			}
			else
			{
				RESIZEPRSBUF;
				*(state->cur) = *(state->ptr);
				state->cur++;
			}
		}
		else if (st == GV_INESCVAL)
		{
			if (*(state->ptr) == '\\')
			{
				st = GV_WAITESCESCIN;
			}
			else if (*(state->ptr) == '"')
			{
				return true;
			}
			else if (*(state->ptr) == '\0')
			{
				PRSEOF;
			}
			else
			{
				RESIZEPRSBUF;
				*(state->cur) = *(state->ptr);
				state->cur++;
			}
		}
		else if (st == GV_WAITESCIN)
		{
			if (*(state->ptr) == '\0')
				PRSEOF;
			RESIZEPRSBUF;
			*(state->cur) = *(state->ptr);
			state->cur++;
			st = GV_INVAL;
		}
		else if (st == GV_WAITESCESCIN)
		{
			if (*(state->ptr) == '\0')
				PRSEOF;
			RESIZEPRSBUF;
			*(state->cur) = *(state->ptr);
			state->cur++;
			st = GV_INESCVAL;
		}
		else
			elog(ERROR, "unrecognized get_val state: %d", st);

		state->ptr++;
	}
}

#define WKEY	0
#define WVAL	1
#define WEQ 2
#define WGT 3
#define WDEL	4


static bool
parse_hstore(HSParser *state)
{
	int			st = WKEY;
	bool		escaped = false;

	state->plen = 16;
	state->pairs = (Pairs *) palloc(sizeof(Pairs) * state->plen);
	state->pcur = 0;
	state->ptr = state->begin;
	state->word = NULL;

	while (1)
	{
		if (st == WKEY)
		{
			if (!get_val(state, false, &escaped))
			{
				if (SOFT_ERROR_OCCURRED(state->escontext))
					return false;
				return true;	/* EOF, all okay */
			}
			if (state->pcur >= state->plen)
			{
				state->plen *= 2;
				state->pairs = (Pairs *) repalloc(state->pairs, sizeof(Pairs) * state->plen);
			}
			if (!hstoreCheckKeyLength(state->cur - state->word, state))
				return false;
			state->pairs[state->pcur].key = state->word;
			state->pairs[state->pcur].keylen = state->cur - state->word;
			state->pairs[state->pcur].val = NULL;
			state->word = NULL;
			st = WEQ;
		}
		else if (st == WEQ)
		{
			if (*(state->ptr) == '=')
			{
				st = WGT;
			}
			else if (*(state->ptr) == '\0')
			{
				PRSEOF;
			}
			else if (!scanner_isspace((unsigned char) *(state->ptr)))
			{
				PRSSYNTAXERROR;
			}
		}
		else if (st == WGT)
		{
			if (*(state->ptr) == '>')
			{
				st = WVAL;
			}
			else if (*(state->ptr) == '\0')
			{
				PRSEOF;
			}
			else
			{
				PRSSYNTAXERROR;
			}
		}
		else if (st == WVAL)
		{
			if (!get_val(state, true, &escaped))
			{
				if (SOFT_ERROR_OCCURRED(state->escontext))
					return false;
				PRSEOF;
			}
			if (!hstoreCheckValLength(state->cur - state->word, state))
				return false;
			state->pairs[state->pcur].val = state->word;
			state->pairs[state->pcur].vallen = state->cur - state->word;
			state->pairs[state->pcur].isnull = false;
			state->pairs[state->pcur].needfree = true;
			if (state->cur - state->word == 4 && !escaped)
			{
				state->word[4] = '\0';
				if (pg_strcasecmp(state->word, "null") == 0)
					state->pairs[state->pcur].isnull = true;
			}
			state->word = NULL;
			state->pcur++;
			st = WDEL;
		}
		else if (st == WDEL)
		{
			if (*(state->ptr) == ',')
			{
				st = WKEY;
			}
			else if (*(state->ptr) == '\0')
			{
				return true;
			}
			else if (!scanner_isspace((unsigned char) *(state->ptr)))
			{
				PRSSYNTAXERROR;
			}
		}
		else
			elog(ERROR, "unrecognized parse_hstore state: %d", st);

		state->ptr++;
	}
}

static int
comparePairs(const void *a, const void *b)
{
	const Pairs *pa = a;
	const Pairs *pb = b;

	if (pa->keylen == pb->keylen)
	{
		int			res = memcmp(pa->key, pb->key, pa->keylen);

		if (res)
			return res;

		/* guarantee that needfree will be later */
		if (pb->needfree == pa->needfree)
			return 0;
		else if (pa->needfree)
			return 1;
		else
			return -1;
	}
	return (pa->keylen > pb->keylen) ? 1 : -1;
}

/*
 * this code still respects pairs.needfree, even though in general
 * it should never be called in a context where anything needs freeing.
 * we keep it because (a) those calls are in a rare code path anyway,
 * and (b) who knows whether they might be needed by some caller.
 */
int
hstoreUniquePairs(Pairs *a, int32 l, int32 *buflen)
{
	Pairs	   *ptr,
			   *res;

	*buflen = 0;
	if (l < 2)
	{
		if (l == 1)
			*buflen = a->keylen + ((a->isnull) ? 0 : a->vallen);
		return l;
	}

	qsort(a, l, sizeof(Pairs), comparePairs);

	/*
	 * We can't use qunique here because we have some clean-up code to run on
	 * removed elements.
	 */
	ptr = a + 1;
	res = a;
	while (ptr - a < l)
	{
		if (ptr->keylen == res->keylen &&
			memcmp(ptr->key, res->key, res->keylen) == 0)
		{
			if (ptr->needfree)
			{
				pfree(ptr->key);
				pfree(ptr->val);
			}
		}
		else
		{
			*buflen += res->keylen + ((res->isnull) ? 0 : res->vallen);
			res++;
			if (res != ptr)
				memcpy(res, ptr, sizeof(Pairs));
		}

		ptr++;
	}

	*buflen += res->keylen + ((res->isnull) ? 0 : res->vallen);
	return res + 1 - a;
}

size_t
hstoreCheckKeyLen(size_t len)
{
	if (len > HSTORE_MAX_KEY_LEN)
		ereport(ERROR,
				(errcode(ERRCODE_STRING_DATA_RIGHT_TRUNCATION),
				 errmsg("string too long for hstore key")));
	return len;
}

static bool
hstoreCheckKeyLength(size_t len, HSParser *state)
{
	if (len > HSTORE_MAX_KEY_LEN)
		ereturn(state->escontext, false,
				(errcode(ERRCODE_STRING_DATA_RIGHT_TRUNCATION),
				 errmsg("string too long for hstore key")));
	return true;
}

size_t
hstoreCheckValLen(size_t len)
{
	if (len > HSTORE_MAX_VALUE_LEN)
		ereport(ERROR,
				(errcode(ERRCODE_STRING_DATA_RIGHT_TRUNCATION),
				 errmsg("string too long for hstore value")));
	return len;
}

static bool
hstoreCheckValLength(size_t len, HSParser *state)
{
	if (len > HSTORE_MAX_VALUE_LEN)
		ereturn(state->escontext, false,
				(errcode(ERRCODE_STRING_DATA_RIGHT_TRUNCATION),
				 errmsg("string too long for hstore value")));
	return true;
}


HStore *
hstorePairs(Pairs *pairs, int32 pcount, int32 buflen)
{
	HStore	   *out;
	HEntry	   *entry;
	char	   *ptr;
	char	   *buf;
	int32		len;
	int32		i;

	len = CALCDATASIZE(pcount, buflen);
	out = palloc(len);
	SET_VARSIZE(out, len);
	HS_SETCOUNT(out, pcount);

	if (pcount == 0)
		return out;

	entry = ARRPTR(out);
	buf = ptr = STRPTR(out);

	for (i = 0; i < pcount; i++)
		HS_ADDITEM(entry, buf, ptr, pairs[i]);

	HS_FINALIZE(out, pcount, buf, ptr);

	return out;
}


PG_FUNCTION_INFO_V1(hstore_in);
Datum
hstore_in(PG_FUNCTION_ARGS)
{
	char	   *str = PG_GETARG_CSTRING(0);
	Node	   *escontext = fcinfo->context;
	HSParser	state;
	int32		buflen;
	HStore	   *out;

	state.begin = str;
	state.escontext = escontext;

	if (!parse_hstore(&state))
		PG_RETURN_NULL();

	state.pcur = hstoreUniquePairs(state.pairs, state.pcur, &buflen);

	out = hstorePairs(state.pairs, state.pcur, buflen);

	PG_RETURN_POINTER(out);
}


PG_FUNCTION_INFO_V1(hstore_recv);
Datum
hstore_recv(PG_FUNCTION_ARGS)
{
	int32		buflen;
	HStore	   *out;
	Pairs	   *pairs;
	int32		i;
	int32		pcount;
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);

	pcount = pq_getmsgint(buf, 4);

	if (pcount == 0)
	{
		out = hstorePairs(NULL, 0, 0);
		PG_RETURN_POINTER(out);
	}

	if (pcount < 0 || pcount > MaxAllocSize / sizeof(Pairs))
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("number of pairs (%d) exceeds the maximum allowed (%d)",
						pcount, (int) (MaxAllocSize / sizeof(Pairs)))));
	pairs = palloc(pcount * sizeof(Pairs));

	for (i = 0; i < pcount; ++i)
	{
		int			rawlen = pq_getmsgint(buf, 4);
		int			len;

		if (rawlen < 0)
			ereport(ERROR,
					(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
					 errmsg("null value not allowed for hstore key")));

		pairs[i].key = pq_getmsgtext(buf, rawlen, &len);
		pairs[i].keylen = hstoreCheckKeyLen(len);
		pairs[i].needfree = true;

		rawlen = pq_getmsgint(buf, 4);
		if (rawlen < 0)
		{
			pairs[i].val = NULL;
			pairs[i].vallen = 0;
			pairs[i].isnull = true;
		}
		else
		{
			pairs[i].val = pq_getmsgtext(buf, rawlen, &len);
			pairs[i].vallen = hstoreCheckValLen(len);
			pairs[i].isnull = false;
		}
	}

	pcount = hstoreUniquePairs(pairs, pcount, &buflen);

	out = hstorePairs(pairs, pcount, buflen);

	PG_RETURN_POINTER(out);
}


PG_FUNCTION_INFO_V1(hstore_from_text);
Datum
hstore_from_text(PG_FUNCTION_ARGS)
{
	text	   *key;
	text	   *val = NULL;
	Pairs		p;
	HStore	   *out;

	if (PG_ARGISNULL(0))
		PG_RETURN_NULL();

	p.needfree = false;
	key = PG_GETARG_TEXT_PP(0);
	p.key = VARDATA_ANY(key);
	p.keylen = hstoreCheckKeyLen(VARSIZE_ANY_EXHDR(key));

	if (PG_ARGISNULL(1))
	{
		p.vallen = 0;
		p.isnull = true;
	}
	else
	{
		val = PG_GETARG_TEXT_PP(1);
		p.val = VARDATA_ANY(val);
		p.vallen = hstoreCheckValLen(VARSIZE_ANY_EXHDR(val));
		p.isnull = false;
	}

	out = hstorePairs(&p, 1, p.keylen + p.vallen);

	PG_RETURN_POINTER(out);
}


PG_FUNCTION_INFO_V1(hstore_from_arrays);
Datum
hstore_from_arrays(PG_FUNCTION_ARGS)
{
	int32		buflen;
	HStore	   *out;
	Pairs	   *pairs;
	Datum	   *key_datums;
	bool	   *key_nulls;
	int			key_count;
	Datum	   *value_datums;
	bool	   *value_nulls;
	int			value_count;
	ArrayType  *key_array;
	ArrayType  *value_array;
	int			i;

	if (PG_ARGISNULL(0))
		PG_RETURN_NULL();

	key_array = PG_GETARG_ARRAYTYPE_P(0);

	Assert(ARR_ELEMTYPE(key_array) == TEXTOID);

	/*
	 * must check >1 rather than != 1 because empty arrays have 0 dimensions,
	 * not 1
	 */

	if (ARR_NDIM(key_array) > 1)
		ereport(ERROR,
				(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
				 errmsg("wrong number of array subscripts")));

	deconstruct_array_builtin(key_array, TEXTOID, &key_datums, &key_nulls, &key_count);

	/* see discussion in hstoreArrayToPairs() */
	if (key_count > MaxAllocSize / sizeof(Pairs))
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("number of pairs (%d) exceeds the maximum allowed (%d)",
						key_count, (int) (MaxAllocSize / sizeof(Pairs)))));

	/* value_array might be NULL */

	if (PG_ARGISNULL(1))
	{
		value_array = NULL;
		value_count = key_count;
		value_datums = NULL;
		value_nulls = NULL;
	}
	else
	{
		value_array = PG_GETARG_ARRAYTYPE_P(1);

		Assert(ARR_ELEMTYPE(value_array) == TEXTOID);

		if (ARR_NDIM(value_array) > 1)
			ereport(ERROR,
					(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
					 errmsg("wrong number of array subscripts")));

		if ((ARR_NDIM(key_array) > 0 || ARR_NDIM(value_array) > 0) &&
			(ARR_NDIM(key_array) != ARR_NDIM(value_array) ||
			 ARR_DIMS(key_array)[0] != ARR_DIMS(value_array)[0] ||
			 ARR_LBOUND(key_array)[0] != ARR_LBOUND(value_array)[0]))
			ereport(ERROR,
					(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
					 errmsg("arrays must have same bounds")));

		deconstruct_array_builtin(value_array, TEXTOID, &value_datums, &value_nulls, &value_count);

		Assert(key_count == value_count);
	}

	pairs = palloc(key_count * sizeof(Pairs));

	for (i = 0; i < key_count; ++i)
	{
		if (key_nulls[i])
			ereport(ERROR,
					(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
					 errmsg("null value not allowed for hstore key")));

		if (!value_nulls || value_nulls[i])
		{
			pairs[i].key = VARDATA(key_datums[i]);
			pairs[i].val = NULL;
			pairs[i].keylen =
				hstoreCheckKeyLen(VARSIZE(key_datums[i]) - VARHDRSZ);
			pairs[i].vallen = 4;
			pairs[i].isnull = true;
			pairs[i].needfree = false;
		}
		else
		{
			pairs[i].key = VARDATA(key_datums[i]);
			pairs[i].val = VARDATA(value_datums[i]);
			pairs[i].keylen =
				hstoreCheckKeyLen(VARSIZE(key_datums[i]) - VARHDRSZ);
			pairs[i].vallen =
				hstoreCheckValLen(VARSIZE(value_datums[i]) - VARHDRSZ);
			pairs[i].isnull = false;
			pairs[i].needfree = false;
		}
	}

	key_count = hstoreUniquePairs(pairs, key_count, &buflen);

	out = hstorePairs(pairs, key_count, buflen);

	PG_RETURN_POINTER(out);
}


PG_FUNCTION_INFO_V1(hstore_from_array);
Datum
hstore_from_array(PG_FUNCTION_ARGS)
{
	ArrayType  *in_array = PG_GETARG_ARRAYTYPE_P(0);
	int			ndims = ARR_NDIM(in_array);
	int			count;
	int32		buflen;
	HStore	   *out;
	Pairs	   *pairs;
	Datum	   *in_datums;
	bool	   *in_nulls;
	int			in_count;
	int			i;

	Assert(ARR_ELEMTYPE(in_array) == TEXTOID);

	switch (ndims)
	{
		case 0:
			out = hstorePairs(NULL, 0, 0);
			PG_RETURN_POINTER(out);

		case 1:
			if ((ARR_DIMS(in_array)[0]) % 2)
				ereport(ERROR,
						(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
						 errmsg("array must have even number of elements")));
			break;

		case 2:
			if ((ARR_DIMS(in_array)[1]) != 2)
				ereport(ERROR,
						(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
						 errmsg("array must have two columns")));
			break;

		default:
			ereport(ERROR,
					(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
					 errmsg("wrong number of array subscripts")));
	}

	deconstruct_array_builtin(in_array, TEXTOID, &in_datums, &in_nulls, &in_count);

	count = in_count / 2;

	/* see discussion in hstoreArrayToPairs() */
	if (count > MaxAllocSize / sizeof(Pairs))
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("number of pairs (%d) exceeds the maximum allowed (%d)",
						count, (int) (MaxAllocSize / sizeof(Pairs)))));

	pairs = palloc(count * sizeof(Pairs));

	for (i = 0; i < count; ++i)
	{
		if (in_nulls[i * 2])
			ereport(ERROR,
					(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
					 errmsg("null value not allowed for hstore key")));

		if (in_nulls[i * 2 + 1])
		{
			pairs[i].key = VARDATA(in_datums[i * 2]);
			pairs[i].val = NULL;
			pairs[i].keylen =
				hstoreCheckKeyLen(VARSIZE(in_datums[i * 2]) - VARHDRSZ);
			pairs[i].vallen = 4;
			pairs[i].isnull = true;
			pairs[i].needfree = false;
		}
		else
		{
			pairs[i].key = VARDATA(in_datums[i * 2]);
			pairs[i].val = VARDATA(in_datums[i * 2 + 1]);
			pairs[i].keylen =
				hstoreCheckKeyLen(VARSIZE(in_datums[i * 2]) - VARHDRSZ);
			pairs[i].vallen =
				hstoreCheckValLen(VARSIZE(in_datums[i * 2 + 1]) - VARHDRSZ);
			pairs[i].isnull = false;
			pairs[i].needfree = false;
		}
	}

	count = hstoreUniquePairs(pairs, count, &buflen);

	out = hstorePairs(pairs, count, buflen);

	PG_RETURN_POINTER(out);
}

/* most of hstore_from_record is shamelessly swiped from record_out */

/*
 * structure to cache metadata needed for record I/O
 */
typedef struct ColumnIOData
{
	Oid			column_type;
	Oid			typiofunc;
	Oid			typioparam;
	FmgrInfo	proc;
} ColumnIOData;

typedef struct RecordIOData
{
	Oid			record_type;
	int32		record_typmod;
	/* this field is used only if target type is domain over composite: */
	void	   *domain_info;	/* opaque cache for domain checks */
	int			ncolumns;
	ColumnIOData columns[FLEXIBLE_ARRAY_MEMBER];
} RecordIOData;

PG_FUNCTION_INFO_V1(hstore_from_record);
Datum
hstore_from_record(PG_FUNCTION_ARGS)
{
	HeapTupleHeader rec;
	int32		buflen;
	HStore	   *out;
	Pairs	   *pairs;
	Oid			tupType;
	int32		tupTypmod;
	TupleDesc	tupdesc;
	HeapTupleData tuple;
	RecordIOData *my_extra;
	int			ncolumns;
	int			i,
				j;
	Datum	   *values;
	bool	   *nulls;

	if (PG_ARGISNULL(0))
	{
		Oid			argtype = get_fn_expr_argtype(fcinfo->flinfo, 0);

		/*
		 * We have no tuple to look at, so the only source of type info is the
		 * argtype --- which might be domain over composite, but we don't care
		 * here, since we have no need to be concerned about domain
		 * constraints.  The lookup_rowtype_tupdesc_domain call below will
		 * error out if we don't have a known composite type oid here.
		 */
		tupType = argtype;
		tupTypmod = -1;

		rec = NULL;
	}
	else
	{
		rec = PG_GETARG_HEAPTUPLEHEADER(0);

		/*
		 * Extract type info from the tuple itself -- this will work even for
		 * anonymous record types.
		 */
		tupType = HeapTupleHeaderGetTypeId(rec);
		tupTypmod = HeapTupleHeaderGetTypMod(rec);
	}

	tupdesc = lookup_rowtype_tupdesc_domain(tupType, tupTypmod, false);
	ncolumns = tupdesc->natts;

	/*
	 * We arrange to look up the needed I/O info just once per series of
	 * calls, assuming the record type doesn't change underneath us.
	 */
	my_extra = (RecordIOData *) fcinfo->flinfo->fn_extra;
	if (my_extra == NULL ||
		my_extra->ncolumns != ncolumns)
	{
		fcinfo->flinfo->fn_extra =
			MemoryContextAlloc(fcinfo->flinfo->fn_mcxt,
							   offsetof(RecordIOData, columns) +
							   ncolumns * sizeof(ColumnIOData));
		my_extra = (RecordIOData *) fcinfo->flinfo->fn_extra;
		my_extra->record_type = InvalidOid;
		my_extra->record_typmod = 0;
	}

	if (my_extra->record_type != tupType ||
		my_extra->record_typmod != tupTypmod)
	{
		MemSet(my_extra, 0,
			   offsetof(RecordIOData, columns) +
			   ncolumns * sizeof(ColumnIOData));
		my_extra->record_type = tupType;
		my_extra->record_typmod = tupTypmod;
		my_extra->ncolumns = ncolumns;
	}

	Assert(ncolumns <= MaxTupleAttributeNumber);	/* thus, no overflow */
	pairs = palloc(ncolumns * sizeof(Pairs));

	if (rec)
	{
		/* Build a temporary HeapTuple control structure */
		tuple.t_len = HeapTupleHeaderGetDatumLength(rec);
		ItemPointerSetInvalid(&(tuple.t_self));
		tuple.t_tableOid = InvalidOid;
		tuple.t_data = rec;

		values = (Datum *) palloc(ncolumns * sizeof(Datum));
		nulls = (bool *) palloc(ncolumns * sizeof(bool));

		/* Break down the tuple into fields */
		heap_deform_tuple(&tuple, tupdesc, values, nulls);
	}
	else
	{
		values = NULL;
		nulls = NULL;
	}

	for (i = 0, j = 0; i < ncolumns; ++i)
	{
		ColumnIOData *column_info = &my_extra->columns[i];
		Form_pg_attribute att = TupleDescAttr(tupdesc, i);
		Oid			column_type = att->atttypid;
		char	   *value;

		/* Ignore dropped columns in datatype */
		if (att->attisdropped)
			continue;

		pairs[j].key = NameStr(att->attname);
		pairs[j].keylen = hstoreCheckKeyLen(strlen(NameStr(att->attname)));

		if (!nulls || nulls[i])
		{
			pairs[j].val = NULL;
			pairs[j].vallen = 4;
			pairs[j].isnull = true;
			pairs[j].needfree = false;
			++j;
			continue;
		}

		/*
		 * Convert the column value to text
		 */
		if (column_info->column_type != column_type)
		{
			bool		typIsVarlena;

			getTypeOutputInfo(column_type,
							  &column_info->typiofunc,
							  &typIsVarlena);
			fmgr_info_cxt(column_info->typiofunc, &column_info->proc,
						  fcinfo->flinfo->fn_mcxt);
			column_info->column_type = column_type;
		}

		value = OutputFunctionCall(&column_info->proc, values[i]);

		pairs[j].val = value;
		pairs[j].vallen = hstoreCheckValLen(strlen(value));
		pairs[j].isnull = false;
		pairs[j].needfree = false;
		++j;
	}

	ncolumns = hstoreUniquePairs(pairs, j, &buflen);

	out = hstorePairs(pairs, ncolumns, buflen);

	ReleaseTupleDesc(tupdesc);

	PG_RETURN_POINTER(out);
}


PG_FUNCTION_INFO_V1(hstore_populate_record);
Datum
hstore_populate_record(PG_FUNCTION_ARGS)
{
	Oid			argtype = get_fn_expr_argtype(fcinfo->flinfo, 0);
	HStore	   *hs;
	HEntry	   *entries;
	char	   *ptr;
	HeapTupleHeader rec;
	Oid			tupType;
	int32		tupTypmod;
	TupleDesc	tupdesc;
	HeapTupleData tuple;
	HeapTuple	rettuple;
	RecordIOData *my_extra;
	int			ncolumns;
	int			i;
	Datum	   *values;
	bool	   *nulls;

	if (!type_is_rowtype(argtype))
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("first argument must be a rowtype")));

	if (PG_ARGISNULL(0))
	{
		if (PG_ARGISNULL(1))
			PG_RETURN_NULL();

		rec = NULL;

		/*
		 * We have no tuple to look at, so the only source of type info is the
		 * argtype.  The lookup_rowtype_tupdesc_domain call below will error
		 * out if we don't have a known composite type oid here.
		 */
		tupType = argtype;
		tupTypmod = -1;
	}
	else
	{
		rec = PG_GETARG_HEAPTUPLEHEADER(0);

		if (PG_ARGISNULL(1))
			PG_RETURN_POINTER(rec);

		/*
		 * Extract type info from the tuple itself -- this will work even for
		 * anonymous record types.
		 */
		tupType = HeapTupleHeaderGetTypeId(rec);
		tupTypmod = HeapTupleHeaderGetTypMod(rec);
	}

	hs = PG_GETARG_HSTORE_P(1);
	entries = ARRPTR(hs);
	ptr = STRPTR(hs);

	/*
	 * if the input hstore is empty, we can only skip the rest if we were
	 * passed in a non-null record, since otherwise there may be issues with
	 * domain nulls.
	 */

	if (HS_COUNT(hs) == 0 && rec)
		PG_RETURN_POINTER(rec);

	/*
	 * Lookup the input record's tupdesc.  For the moment, we don't worry
	 * about whether it is a domain over composite.
	 */
	tupdesc = lookup_rowtype_tupdesc_domain(tupType, tupTypmod, false);
	ncolumns = tupdesc->natts;

	if (rec)
	{
		/* Build a temporary HeapTuple control structure */
		tuple.t_len = HeapTupleHeaderGetDatumLength(rec);
		ItemPointerSetInvalid(&(tuple.t_self));
		tuple.t_tableOid = InvalidOid;
		tuple.t_data = rec;
	}

	/*
	 * We arrange to look up the needed I/O info just once per series of
	 * calls, assuming the record type doesn't change underneath us.
	 */
	my_extra = (RecordIOData *) fcinfo->flinfo->fn_extra;
	if (my_extra == NULL ||
		my_extra->ncolumns != ncolumns)
	{
		fcinfo->flinfo->fn_extra =
			MemoryContextAlloc(fcinfo->flinfo->fn_mcxt,
							   offsetof(RecordIOData, columns) +
							   ncolumns * sizeof(ColumnIOData));
		my_extra = (RecordIOData *) fcinfo->flinfo->fn_extra;
		my_extra->record_type = InvalidOid;
		my_extra->record_typmod = 0;
		my_extra->domain_info = NULL;
	}

	if (my_extra->record_type != tupType ||
		my_extra->record_typmod != tupTypmod)
	{
		MemSet(my_extra, 0,
			   offsetof(RecordIOData, columns) +
			   ncolumns * sizeof(ColumnIOData));
		my_extra->record_type = tupType;
		my_extra->record_typmod = tupTypmod;
		my_extra->ncolumns = ncolumns;
	}

	values = (Datum *) palloc(ncolumns * sizeof(Datum));
	nulls = (bool *) palloc(ncolumns * sizeof(bool));

	if (rec)
	{
		/* Break down the tuple into fields */
		heap_deform_tuple(&tuple, tupdesc, values, nulls);
	}
	else
	{
		for (i = 0; i < ncolumns; ++i)
		{
			values[i] = (Datum) 0;
			nulls[i] = true;
		}
	}

	for (i = 0; i < ncolumns; ++i)
	{
		ColumnIOData *column_info = &my_extra->columns[i];
		Form_pg_attribute att = TupleDescAttr(tupdesc, i);
		Oid			column_type = att->atttypid;
		char	   *value;
		int			idx;
		int			vallen;

		/* Ignore dropped columns in datatype */
		if (att->attisdropped)
		{
			nulls[i] = true;
			continue;
		}

		idx = hstoreFindKey(hs, 0,
							NameStr(att->attname),
							strlen(NameStr(att->attname)));

		/*
		 * we can't just skip here if the key wasn't found since we might have
		 * a domain to deal with. If we were passed in a non-null record
		 * datum, we assume that the existing values are valid (if they're
		 * not, then it's not our fault), but if we were passed in a null,
		 * then every field which we don't populate needs to be run through
		 * the input function just in case it's a domain type.
		 */
		if (idx < 0 && rec)
			continue;

		/*
		 * Prepare to convert the column value from text
		 */
		if (column_info->column_type != column_type)
		{
			getTypeInputInfo(column_type,
							 &column_info->typiofunc,
							 &column_info->typioparam);
			fmgr_info_cxt(column_info->typiofunc, &column_info->proc,
						  fcinfo->flinfo->fn_mcxt);
			column_info->column_type = column_type;
		}

		if (idx < 0 || HSTORE_VALISNULL(entries, idx))
		{
			/*
			 * need InputFunctionCall to happen even for nulls, so that domain
			 * checks are done
			 */
			values[i] = InputFunctionCall(&column_info->proc, NULL,
										  column_info->typioparam,
										  att->atttypmod);
			nulls[i] = true;
		}
		else
		{
			vallen = HSTORE_VALLEN(entries, idx);
			value = palloc(1 + vallen);
			memcpy(value, HSTORE_VAL(entries, ptr, idx), vallen);
			value[vallen] = 0;

			values[i] = InputFunctionCall(&column_info->proc, value,
										  column_info->typioparam,
										  att->atttypmod);
			nulls[i] = false;
		}
	}

	rettuple = heap_form_tuple(tupdesc, values, nulls);

	/*
	 * If the target type is domain over composite, all we know at this point
	 * is that we've made a valid value of the base composite type.  Must
	 * check domain constraints before deciding we're done.
	 */
	if (argtype != tupdesc->tdtypeid)
		domain_check(HeapTupleGetDatum(rettuple), false,
					 argtype,
					 &my_extra->domain_info,
					 fcinfo->flinfo->fn_mcxt);

	ReleaseTupleDesc(tupdesc);

	PG_RETURN_DATUM(HeapTupleGetDatum(rettuple));
}


static char *
cpw(char *dst, char *src, int len)
{
	char	   *ptr = src;

	while (ptr - src < len)
	{
		if (*ptr == '"' || *ptr == '\\')
			*dst++ = '\\';
		*dst++ = *ptr++;
	}
	return dst;
}

PG_FUNCTION_INFO_V1(hstore_out);
Datum
hstore_out(PG_FUNCTION_ARGS)
{
	HStore	   *in = PG_GETARG_HSTORE_P(0);
	int			buflen,
				i;
	int			count = HS_COUNT(in);
	char	   *out,
			   *ptr;
	char	   *base = STRPTR(in);
	HEntry	   *entries = ARRPTR(in);

	if (count == 0)
		PG_RETURN_CSTRING(pstrdup(""));

	buflen = 0;

	/*
	 * this loop overestimates due to pessimistic assumptions about escaping,
	 * so very large hstore values can't be output. this could be fixed, but
	 * many other data types probably have the same issue. This replaced code
	 * that used the original varlena size for calculations, which was wrong
	 * in some subtle ways.
	 */

	for (i = 0; i < count; i++)
	{
		/* include "" and => and comma-space */
		buflen += 6 + 2 * HSTORE_KEYLEN(entries, i);
		/* include "" only if nonnull */
		buflen += 2 + (HSTORE_VALISNULL(entries, i)
					   ? 2
					   : 2 * HSTORE_VALLEN(entries, i));
	}

	out = ptr = palloc(buflen);

	for (i = 0; i < count; i++)
	{
		*ptr++ = '"';
		ptr = cpw(ptr, HSTORE_KEY(entries, base, i), HSTORE_KEYLEN(entries, i));
		*ptr++ = '"';
		*ptr++ = '=';
		*ptr++ = '>';
		if (HSTORE_VALISNULL(entries, i))
		{
			*ptr++ = 'N';
			*ptr++ = 'U';
			*ptr++ = 'L';
			*ptr++ = 'L';
		}
		else
		{
			*ptr++ = '"';
			ptr = cpw(ptr, HSTORE_VAL(entries, base, i), HSTORE_VALLEN(entries, i));
			*ptr++ = '"';
		}

		if (i + 1 != count)
		{
			*ptr++ = ',';
			*ptr++ = ' ';
		}
	}
	*ptr = '\0';

	PG_RETURN_CSTRING(out);
}


PG_FUNCTION_INFO_V1(hstore_send);
Datum
hstore_send(PG_FUNCTION_ARGS)
{
	HStore	   *in = PG_GETARG_HSTORE_P(0);
	int			i;
	int			count = HS_COUNT(in);
	char	   *base = STRPTR(in);
	HEntry	   *entries = ARRPTR(in);
	StringInfoData buf;

	pq_begintypsend(&buf);

	pq_sendint32(&buf, count);

	for (i = 0; i < count; i++)
	{
		int32		keylen = HSTORE_KEYLEN(entries, i);

		pq_sendint32(&buf, keylen);
		pq_sendtext(&buf, HSTORE_KEY(entries, base, i), keylen);
		if (HSTORE_VALISNULL(entries, i))
		{
			pq_sendint32(&buf, -1);
		}
		else
		{
			int32		vallen = HSTORE_VALLEN(entries, i);

			pq_sendint32(&buf, vallen);
			pq_sendtext(&buf, HSTORE_VAL(entries, base, i), vallen);
		}
	}

	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}


/*
 * hstore_to_json_loose
 *
 * This is a heuristic conversion to json which treats
 * 't' and 'f' as booleans and strings that look like numbers as numbers,
 * as long as they don't start with a leading zero followed by another digit
 * (think zip codes or phone numbers starting with 0).
 */
PG_FUNCTION_INFO_V1(hstore_to_json_loose);
Datum
hstore_to_json_loose(PG_FUNCTION_ARGS)
{
	HStore	   *in = PG_GETARG_HSTORE_P(0);
	int			i;
	int			count = HS_COUNT(in);
	char	   *base = STRPTR(in);
	HEntry	   *entries = ARRPTR(in);
	StringInfoData dst;

	if (count == 0)
		PG_RETURN_TEXT_P(cstring_to_text_with_len("{}", 2));

	initStringInfo(&dst);

	appendStringInfoChar(&dst, '{');

	for (i = 0; i < count; i++)
	{
		escape_json_with_len(&dst,
							 HSTORE_KEY(entries, base, i),
							 HSTORE_KEYLEN(entries, i));
		appendStringInfoString(&dst, ": ");
		if (HSTORE_VALISNULL(entries, i))
			appendStringInfoString(&dst, "null");
		/* guess that values of 't' or 'f' are booleans */
		else if (HSTORE_VALLEN(entries, i) == 1 &&
				 *(HSTORE_VAL(entries, base, i)) == 't')
			appendStringInfoString(&dst, "true");
		else if (HSTORE_VALLEN(entries, i) == 1 &&
				 *(HSTORE_VAL(entries, base, i)) == 'f')
			appendStringInfoString(&dst, "false");
		else
		{
			char	   *str = HSTORE_VAL(entries, base, i);
			int			len = HSTORE_VALLEN(entries, i);

			if (IsValidJsonNumber(str, len))
				appendBinaryStringInfo(&dst, str, len);
			else
				escape_json_with_len(&dst, str, len);
		}

		if (i + 1 != count)
			appendStringInfoString(&dst, ", ");
	}
	appendStringInfoChar(&dst, '}');

	PG_RETURN_TEXT_P(cstring_to_text_with_len(dst.data, dst.len));
}

PG_FUNCTION_INFO_V1(hstore_to_json);
Datum
hstore_to_json(PG_FUNCTION_ARGS)
{
	HStore	   *in = PG_GETARG_HSTORE_P(0);
	int			i;
	int			count = HS_COUNT(in);
	char	   *base = STRPTR(in);
	HEntry	   *entries = ARRPTR(in);
	StringInfoData dst;

	if (count == 0)
		PG_RETURN_TEXT_P(cstring_to_text_with_len("{}", 2));

	initStringInfo(&dst);

	appendStringInfoChar(&dst, '{');

	for (i = 0; i < count; i++)
	{
		escape_json_with_len(&dst,
							 HSTORE_KEY(entries, base, i),
							 HSTORE_KEYLEN(entries, i));
		appendStringInfoString(&dst, ": ");
		if (HSTORE_VALISNULL(entries, i))
			appendStringInfoString(&dst, "null");
		else
		{
			escape_json_with_len(&dst,
								 HSTORE_VAL(entries, base, i),
								 HSTORE_VALLEN(entries, i));
		}

		if (i + 1 != count)
			appendStringInfoString(&dst, ", ");
	}
	appendStringInfoChar(&dst, '}');

	PG_RETURN_TEXT_P(cstring_to_text_with_len(dst.data, dst.len));
}

PG_FUNCTION_INFO_V1(hstore_to_jsonb);
Datum
hstore_to_jsonb(PG_FUNCTION_ARGS)
{
	HStore	   *in = PG_GETARG_HSTORE_P(0);
	int			i;
	int			count = HS_COUNT(in);
	char	   *base = STRPTR(in);
	HEntry	   *entries = ARRPTR(in);
	JsonbParseState *state = NULL;
	JsonbValue *res;

	(void) pushJsonbValue(&state, WJB_BEGIN_OBJECT, NULL);

	for (i = 0; i < count; i++)
	{
		JsonbValue	key,
					val;

		key.type = jbvString;
		key.val.string.len = HSTORE_KEYLEN(entries, i);
		key.val.string.val = HSTORE_KEY(entries, base, i);

		(void) pushJsonbValue(&state, WJB_KEY, &key);

		if (HSTORE_VALISNULL(entries, i))
		{
			val.type = jbvNull;
		}
		else
		{
			val.type = jbvString;
			val.val.string.len = HSTORE_VALLEN(entries, i);
			val.val.string.val = HSTORE_VAL(entries, base, i);
		}
		(void) pushJsonbValue(&state, WJB_VALUE, &val);
	}

	res = pushJsonbValue(&state, WJB_END_OBJECT, NULL);

	PG_RETURN_POINTER(JsonbValueToJsonb(res));
}

PG_FUNCTION_INFO_V1(hstore_to_jsonb_loose);
Datum
hstore_to_jsonb_loose(PG_FUNCTION_ARGS)
{
	HStore	   *in = PG_GETARG_HSTORE_P(0);
	int			i;
	int			count = HS_COUNT(in);
	char	   *base = STRPTR(in);
	HEntry	   *entries = ARRPTR(in);
	JsonbParseState *state = NULL;
	JsonbValue *res;
	StringInfoData tmp;

	initStringInfo(&tmp);

	(void) pushJsonbValue(&state, WJB_BEGIN_OBJECT, NULL);

	for (i = 0; i < count; i++)
	{
		JsonbValue	key,
					val;

		key.type = jbvString;
		key.val.string.len = HSTORE_KEYLEN(entries, i);
		key.val.string.val = HSTORE_KEY(entries, base, i);

		(void) pushJsonbValue(&state, WJB_KEY, &key);

		if (HSTORE_VALISNULL(entries, i))
		{
			val.type = jbvNull;
		}
		/* guess that values of 't' or 'f' are booleans */
		else if (HSTORE_VALLEN(entries, i) == 1 &&
				 *(HSTORE_VAL(entries, base, i)) == 't')
		{
			val.type = jbvBool;
			val.val.boolean = true;
		}
		else if (HSTORE_VALLEN(entries, i) == 1 &&
				 *(HSTORE_VAL(entries, base, i)) == 'f')
		{
			val.type = jbvBool;
			val.val.boolean = false;
		}
		else
		{
			resetStringInfo(&tmp);
			appendBinaryStringInfo(&tmp, HSTORE_VAL(entries, base, i),
								   HSTORE_VALLEN(entries, i));
			if (IsValidJsonNumber(tmp.data, tmp.len))
			{
				Datum		numd;

				val.type = jbvNumeric;
				numd = DirectFunctionCall3(numeric_in,
										   CStringGetDatum(tmp.data),
										   ObjectIdGetDatum(InvalidOid),
										   Int32GetDatum(-1));
				val.val.numeric = DatumGetNumeric(numd);
			}
			else
			{
				val.type = jbvString;
				val.val.string.len = HSTORE_VALLEN(entries, i);
				val.val.string.val = HSTORE_VAL(entries, base, i);
			}
		}
		(void) pushJsonbValue(&state, WJB_VALUE, &val);
	}

	res = pushJsonbValue(&state, WJB_END_OBJECT, NULL);

	PG_RETURN_POINTER(JsonbValueToJsonb(res));
}
