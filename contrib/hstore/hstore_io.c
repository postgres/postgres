/*
 * contrib/hstore/hstore_io.c
 */
#include "postgres.h"

#include <ctype.h>

#include "access/htup_details.h"
#include "catalog/pg_type.h"
#include "funcapi.h"
#include "lib/stringinfo.h"
#include "libpq/pqformat.h"
#include "utils/builtins.h"
#include "utils/json.h"
#include "utils/lsyscache.h"
#include "utils/typcache.h"

#include "hstore.h"

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

	Pairs	   *pairs;
	int			pcur;
	int			plen;
} HSParser;

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
				elog(ERROR, "Syntax error near '%c' at position %d", *(state->ptr), (int32) (state->ptr - state->begin));
			}
			else if (*(state->ptr) == '\\')
			{
				st = GV_WAITESCIN;
			}
			else if (!isspace((unsigned char) *(state->ptr)))
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
			else if (isspace((unsigned char) *(state->ptr)))
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
				elog(ERROR, "Unexpected end of string");
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
				elog(ERROR, "Unexpected end of string");
			RESIZEPRSBUF;
			*(state->cur) = *(state->ptr);
			state->cur++;
			st = GV_INVAL;
		}
		else if (st == GV_WAITESCESCIN)
		{
			if (*(state->ptr) == '\0')
				elog(ERROR, "Unexpected end of string");
			RESIZEPRSBUF;
			*(state->cur) = *(state->ptr);
			state->cur++;
			st = GV_INESCVAL;
		}
		else
			elog(ERROR, "Unknown state %d at position line %d in file '%s'", st, __LINE__, __FILE__);

		state->ptr++;
	}
}

#define WKEY	0
#define WVAL	1
#define WEQ 2
#define WGT 3
#define WDEL	4


static void
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
				return;
			if (state->pcur >= state->plen)
			{
				state->plen *= 2;
				state->pairs = (Pairs *) repalloc(state->pairs, sizeof(Pairs) * state->plen);
			}
			state->pairs[state->pcur].key = state->word;
			state->pairs[state->pcur].keylen = hstoreCheckKeyLen(state->cur - state->word);
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
				elog(ERROR, "Unexpected end of string");
			}
			else if (!isspace((unsigned char) *(state->ptr)))
			{
				elog(ERROR, "Syntax error near '%c' at position %d", *(state->ptr), (int32) (state->ptr - state->begin));
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
				elog(ERROR, "Unexpected end of string");
			}
			else
			{
				elog(ERROR, "Syntax error near '%c' at position %d", *(state->ptr), (int32) (state->ptr - state->begin));
			}
		}
		else if (st == WVAL)
		{
			if (!get_val(state, true, &escaped))
				elog(ERROR, "Unexpected end of string");
			state->pairs[state->pcur].val = state->word;
			state->pairs[state->pcur].vallen = hstoreCheckValLen(state->cur - state->word);
			state->pairs[state->pcur].isnull = false;
			state->pairs[state->pcur].needfree = true;
			if (state->cur - state->word == 4 && !escaped)
			{
				state->word[4] = '\0';
				if (0 == pg_strcasecmp(state->word, "null"))
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
				return;
			}
			else if (!isspace((unsigned char) *(state->ptr)))
			{
				elog(ERROR, "Syntax error near '%c' at position %d", *(state->ptr), (int32) (state->ptr - state->begin));
			}
		}
		else
			elog(ERROR, "Unknown state %d at line %d in file '%s'", st, __LINE__, __FILE__);

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

	qsort((void *) a, l, sizeof(Pairs), comparePairs);
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

size_t
hstoreCheckValLen(size_t len)
{
	if (len > HSTORE_MAX_VALUE_LEN)
		ereport(ERROR,
				(errcode(ERRCODE_STRING_DATA_RIGHT_TRUNCATION),
				 errmsg("string too long for hstore value")));
	return len;
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
Datum		hstore_in(PG_FUNCTION_ARGS);
Datum
hstore_in(PG_FUNCTION_ARGS)
{
	HSParser	state;
	int32		buflen;
	HStore	   *out;

	state.begin = PG_GETARG_CSTRING(0);

	parse_hstore(&state);

	state.pcur = hstoreUniquePairs(state.pairs, state.pcur, &buflen);

	out = hstorePairs(state.pairs, state.pcur, buflen);

	PG_RETURN_POINTER(out);
}


PG_FUNCTION_INFO_V1(hstore_recv);
Datum		hstore_recv(PG_FUNCTION_ARGS);
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
Datum		hstore_from_text(PG_FUNCTION_ARGS);
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
Datum		hstore_from_arrays(PG_FUNCTION_ARGS);
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

	deconstruct_array(key_array,
					  TEXTOID, -1, false, 'i',
					  &key_datums, &key_nulls, &key_count);

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

		deconstruct_array(value_array,
						  TEXTOID, -1, false, 'i',
						  &value_datums, &value_nulls, &value_count);

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
			pairs[i].key = VARDATA_ANY(key_datums[i]);
			pairs[i].val = NULL;
			pairs[i].keylen = hstoreCheckKeyLen(VARSIZE_ANY_EXHDR(key_datums[i]));
			pairs[i].vallen = 4;
			pairs[i].isnull = true;
			pairs[i].needfree = false;
		}
		else
		{
			pairs[i].key = VARDATA_ANY(key_datums[i]);
			pairs[i].val = VARDATA_ANY(value_datums[i]);
			pairs[i].keylen = hstoreCheckKeyLen(VARSIZE_ANY_EXHDR(key_datums[i]));
			pairs[i].vallen = hstoreCheckValLen(VARSIZE_ANY_EXHDR(value_datums[i]));
			pairs[i].isnull = false;
			pairs[i].needfree = false;
		}
	}

	key_count = hstoreUniquePairs(pairs, key_count, &buflen);

	out = hstorePairs(pairs, key_count, buflen);

	PG_RETURN_POINTER(out);
}


PG_FUNCTION_INFO_V1(hstore_from_array);
Datum		hstore_from_array(PG_FUNCTION_ARGS);
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

	deconstruct_array(in_array,
					  TEXTOID, -1, false, 'i',
					  &in_datums, &in_nulls, &in_count);

	count = in_count / 2;

	pairs = palloc(count * sizeof(Pairs));

	for (i = 0; i < count; ++i)
	{
		if (in_nulls[i * 2])
			ereport(ERROR,
					(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
					 errmsg("null value not allowed for hstore key")));

		if (in_nulls[i * 2 + 1])
		{
			pairs[i].key = VARDATA_ANY(in_datums[i * 2]);
			pairs[i].val = NULL;
			pairs[i].keylen = hstoreCheckKeyLen(VARSIZE_ANY_EXHDR(in_datums[i * 2]));
			pairs[i].vallen = 4;
			pairs[i].isnull = true;
			pairs[i].needfree = false;
		}
		else
		{
			pairs[i].key = VARDATA_ANY(in_datums[i * 2]);
			pairs[i].val = VARDATA_ANY(in_datums[i * 2 + 1]);
			pairs[i].keylen = hstoreCheckKeyLen(VARSIZE_ANY_EXHDR(in_datums[i * 2]));
			pairs[i].vallen = hstoreCheckValLen(VARSIZE_ANY_EXHDR(in_datums[i * 2 + 1]));
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
	int			ncolumns;
	ColumnIOData columns[1];	/* VARIABLE LENGTH ARRAY */
} RecordIOData;

PG_FUNCTION_INFO_V1(hstore_from_record);
Datum		hstore_from_record(PG_FUNCTION_ARGS);
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
		 * have no tuple to look at, so the only source of type info is the
		 * argtype. The lookup_rowtype_tupdesc call below will error out if we
		 * don't have a known composite type oid here.
		 */
		tupType = argtype;
		tupTypmod = -1;

		rec = NULL;
	}
	else
	{
		rec = PG_GETARG_HEAPTUPLEHEADER(0);

		/* Extract type info from the tuple itself */
		tupType = HeapTupleHeaderGetTypeId(rec);
		tupTypmod = HeapTupleHeaderGetTypMod(rec);
	}

	tupdesc = lookup_rowtype_tupdesc(tupType, tupTypmod);
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
							   sizeof(RecordIOData) - sizeof(ColumnIOData)
							   + ncolumns * sizeof(ColumnIOData));
		my_extra = (RecordIOData *) fcinfo->flinfo->fn_extra;
		my_extra->record_type = InvalidOid;
		my_extra->record_typmod = 0;
	}

	if (my_extra->record_type != tupType ||
		my_extra->record_typmod != tupTypmod)
	{
		MemSet(my_extra, 0,
			   sizeof(RecordIOData) - sizeof(ColumnIOData)
			   + ncolumns * sizeof(ColumnIOData));
		my_extra->record_type = tupType;
		my_extra->record_typmod = tupTypmod;
		my_extra->ncolumns = ncolumns;
	}

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
		Oid			column_type = tupdesc->attrs[i]->atttypid;
		char	   *value;

		/* Ignore dropped columns in datatype */
		if (tupdesc->attrs[i]->attisdropped)
			continue;

		pairs[j].key = NameStr(tupdesc->attrs[i]->attname);
		pairs[j].keylen = hstoreCheckKeyLen(strlen(NameStr(tupdesc->attrs[i]->attname)));

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
Datum		hstore_populate_record(PG_FUNCTION_ARGS);
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
		 * have no tuple to look at, so the only source of type info is the
		 * argtype. The lookup_rowtype_tupdesc call below will error out if we
		 * don't have a known composite type oid here.
		 */
		tupType = argtype;
		tupTypmod = -1;
	}
	else
	{
		rec = PG_GETARG_HEAPTUPLEHEADER(0);

		if (PG_ARGISNULL(1))
			PG_RETURN_POINTER(rec);

		/* Extract type info from the tuple itself */
		tupType = HeapTupleHeaderGetTypeId(rec);
		tupTypmod = HeapTupleHeaderGetTypMod(rec);
	}

	hs = PG_GETARG_HS(1);
	entries = ARRPTR(hs);
	ptr = STRPTR(hs);

	/*
	 * if the input hstore is empty, we can only skip the rest if we were
	 * passed in a non-null record, since otherwise there may be issues with
	 * domain nulls.
	 */

	if (HS_COUNT(hs) == 0 && rec)
		PG_RETURN_POINTER(rec);

	tupdesc = lookup_rowtype_tupdesc(tupType, tupTypmod);
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
							   sizeof(RecordIOData) - sizeof(ColumnIOData)
							   + ncolumns * sizeof(ColumnIOData));
		my_extra = (RecordIOData *) fcinfo->flinfo->fn_extra;
		my_extra->record_type = InvalidOid;
		my_extra->record_typmod = 0;
	}

	if (my_extra->record_type != tupType ||
		my_extra->record_typmod != tupTypmod)
	{
		MemSet(my_extra, 0,
			   sizeof(RecordIOData) - sizeof(ColumnIOData)
			   + ncolumns * sizeof(ColumnIOData));
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
		Oid			column_type = tupdesc->attrs[i]->atttypid;
		char	   *value;
		int			idx;
		int			vallen;

		/* Ignore dropped columns in datatype */
		if (tupdesc->attrs[i]->attisdropped)
		{
			nulls[i] = true;
			continue;
		}

		idx = hstoreFindKey(hs, 0,
							NameStr(tupdesc->attrs[i]->attname),
							strlen(NameStr(tupdesc->attrs[i]->attname)));

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

		if (idx < 0 || HS_VALISNULL(entries, idx))
		{
			/*
			 * need InputFunctionCall to happen even for nulls, so that domain
			 * checks are done
			 */
			values[i] = InputFunctionCall(&column_info->proc, NULL,
										  column_info->typioparam,
										  tupdesc->attrs[i]->atttypmod);
			nulls[i] = true;
		}
		else
		{
			vallen = HS_VALLEN(entries, idx);
			value = palloc(1 + vallen);
			memcpy(value, HS_VAL(entries, ptr, idx), vallen);
			value[vallen] = 0;

			values[i] = InputFunctionCall(&column_info->proc, value,
										  column_info->typioparam,
										  tupdesc->attrs[i]->atttypmod);
			nulls[i] = false;
		}
	}

	rettuple = heap_form_tuple(tupdesc, values, nulls);

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
Datum		hstore_out(PG_FUNCTION_ARGS);
Datum
hstore_out(PG_FUNCTION_ARGS)
{
	HStore	   *in = PG_GETARG_HS(0);
	int			buflen,
				i;
	int			count = HS_COUNT(in);
	char	   *out,
			   *ptr;
	char	   *base = STRPTR(in);
	HEntry	   *entries = ARRPTR(in);

	if (count == 0)
	{
		out = palloc(1);
		*out = '\0';
		PG_RETURN_CSTRING(out);
	}

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
		buflen += 6 + 2 * HS_KEYLEN(entries, i);
		/* include "" only if nonnull */
		buflen += 2 + (HS_VALISNULL(entries, i)
					   ? 2
					   : 2 * HS_VALLEN(entries, i));
	}

	out = ptr = palloc(buflen);

	for (i = 0; i < count; i++)
	{
		*ptr++ = '"';
		ptr = cpw(ptr, HS_KEY(entries, base, i), HS_KEYLEN(entries, i));
		*ptr++ = '"';
		*ptr++ = '=';
		*ptr++ = '>';
		if (HS_VALISNULL(entries, i))
		{
			*ptr++ = 'N';
			*ptr++ = 'U';
			*ptr++ = 'L';
			*ptr++ = 'L';
		}
		else
		{
			*ptr++ = '"';
			ptr = cpw(ptr, HS_VAL(entries, base, i), HS_VALLEN(entries, i));
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
Datum		hstore_send(PG_FUNCTION_ARGS);
Datum
hstore_send(PG_FUNCTION_ARGS)
{
	HStore	   *in = PG_GETARG_HS(0);
	int			i;
	int			count = HS_COUNT(in);
	char	   *base = STRPTR(in);
	HEntry	   *entries = ARRPTR(in);
	StringInfoData buf;

	pq_begintypsend(&buf);

	pq_sendint(&buf, count, 4);

	for (i = 0; i < count; i++)
	{
		int32		keylen = HS_KEYLEN(entries, i);

		pq_sendint(&buf, keylen, 4);
		pq_sendtext(&buf, HS_KEY(entries, base, i), keylen);
		if (HS_VALISNULL(entries, i))
		{
			pq_sendint(&buf, -1, 4);
		}
		else
		{
			int32		vallen = HS_VALLEN(entries, i);

			pq_sendint(&buf, vallen, 4);
			pq_sendtext(&buf, HS_VAL(entries, base, i), vallen);
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
Datum		hstore_to_json_loose(PG_FUNCTION_ARGS);
Datum
hstore_to_json_loose(PG_FUNCTION_ARGS)
{
	HStore	   *in = PG_GETARG_HS(0);
	int			buflen,
				i;
	int			count = HS_COUNT(in);
	char	   *out,
			   *ptr;
	char	   *base = STRPTR(in);
	HEntry	   *entries = ARRPTR(in);
	bool		is_number;
	StringInfo	src,
				dst;

	if (count == 0)
	{
		out = palloc(1);
		*out = '\0';
		PG_RETURN_TEXT_P(cstring_to_text(out));
	}

	buflen = 3;

	/*
	 * Formula adjusted slightly from the logic in hstore_out. We have to take
	 * account of out treatment of booleans to be a bit more pessimistic about
	 * the length of values.
	 */

	for (i = 0; i < count; i++)
	{
		/* include "" and colon-space and comma-space */
		buflen += 6 + 2 * HS_KEYLEN(entries, i);
		/* include "" only if nonnull */
		buflen += 3 + (HS_VALISNULL(entries, i)
					   ? 1
					   : 2 * HS_VALLEN(entries, i));
	}

	out = ptr = palloc(buflen);

	src = makeStringInfo();
	dst = makeStringInfo();

	*ptr++ = '{';

	for (i = 0; i < count; i++)
	{
		resetStringInfo(src);
		resetStringInfo(dst);
		appendBinaryStringInfo(src, HS_KEY(entries, base, i), HS_KEYLEN(entries, i));
		escape_json(dst, src->data);
		strncpy(ptr, dst->data, dst->len);
		ptr += dst->len;
		*ptr++ = ':';
		*ptr++ = ' ';
		resetStringInfo(dst);
		if (HS_VALISNULL(entries, i))
			appendStringInfoString(dst, "null");
		/* guess that values of 't' or 'f' are booleans */
		else if (HS_VALLEN(entries, i) == 1 && *(HS_VAL(entries, base, i)) == 't')
			appendStringInfoString(dst, "true");
		else if (HS_VALLEN(entries, i) == 1 && *(HS_VAL(entries, base, i)) == 'f')
			appendStringInfoString(dst, "false");
		else
		{
			is_number = false;
			resetStringInfo(src);
			appendBinaryStringInfo(src, HS_VAL(entries, base, i), HS_VALLEN(entries, i));

			/*
			 * don't treat something with a leading zero followed by another
			 * digit as numeric - could be a zip code or similar
			 */
			if (src->len > 0 &&
				!(src->data[0] == '0' &&
				  isdigit((unsigned char) src->data[1])) &&
				strspn(src->data, "+-0123456789Ee.") == src->len)
			{
				/*
				 * might be a number. See if we can input it as a numeric
				 * value. Ignore any actual parsed value.
				 */
				char	   *endptr = "junk";
				long		lval;

				lval = strtol(src->data, &endptr, 10);
				(void) lval;
				if (*endptr == '\0')
				{
					/*
					 * strol man page says this means the whole string is
					 * valid
					 */
					is_number = true;
				}
				else
				{
					/* not an int - try a double */
					double		dval;

					dval = strtod(src->data, &endptr);
					(void) dval;
					if (*endptr == '\0')
						is_number = true;
				}
			}
			if (is_number)
				appendBinaryStringInfo(dst, src->data, src->len);
			else
				escape_json(dst, src->data);
		}
		strncpy(ptr, dst->data, dst->len);
		ptr += dst->len;

		if (i + 1 != count)
		{
			*ptr++ = ',';
			*ptr++ = ' ';
		}
	}
	*ptr++ = '}';
	*ptr = '\0';

	PG_RETURN_TEXT_P(cstring_to_text(out));
}

PG_FUNCTION_INFO_V1(hstore_to_json);
Datum		hstore_to_json(PG_FUNCTION_ARGS);
Datum
hstore_to_json(PG_FUNCTION_ARGS)
{
	HStore	   *in = PG_GETARG_HS(0);
	int			buflen,
				i;
	int			count = HS_COUNT(in);
	char	   *out,
			   *ptr;
	char	   *base = STRPTR(in);
	HEntry	   *entries = ARRPTR(in);
	StringInfo	src,
				dst;

	if (count == 0)
	{
		out = palloc(1);
		*out = '\0';
		PG_RETURN_TEXT_P(cstring_to_text(out));
	}

	buflen = 3;

	/*
	 * Formula adjusted slightly from the logic in hstore_out. We have to take
	 * account of out treatment of booleans to be a bit more pessimistic about
	 * the length of values.
	 */

	for (i = 0; i < count; i++)
	{
		/* include "" and colon-space and comma-space */
		buflen += 6 + 2 * HS_KEYLEN(entries, i);
		/* include "" only if nonnull */
		buflen += 3 + (HS_VALISNULL(entries, i)
					   ? 1
					   : 2 * HS_VALLEN(entries, i));
	}

	out = ptr = palloc(buflen);

	src = makeStringInfo();
	dst = makeStringInfo();

	*ptr++ = '{';

	for (i = 0; i < count; i++)
	{
		resetStringInfo(src);
		resetStringInfo(dst);
		appendBinaryStringInfo(src, HS_KEY(entries, base, i), HS_KEYLEN(entries, i));
		escape_json(dst, src->data);
		strncpy(ptr, dst->data, dst->len);
		ptr += dst->len;
		*ptr++ = ':';
		*ptr++ = ' ';
		resetStringInfo(dst);
		if (HS_VALISNULL(entries, i))
			appendStringInfoString(dst, "null");
		else
		{
			resetStringInfo(src);
			appendBinaryStringInfo(src, HS_VAL(entries, base, i), HS_VALLEN(entries, i));
			escape_json(dst, src->data);
		}
		strncpy(ptr, dst->data, dst->len);
		ptr += dst->len;

		if (i + 1 != count)
		{
			*ptr++ = ',';
			*ptr++ = ' ';
		}
	}
	*ptr++ = '}';
	*ptr = '\0';

	PG_RETURN_TEXT_P(cstring_to_text(out));
}
