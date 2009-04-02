#include "hstore.h"
#include <ctype.h>

PG_MODULE_MAGIC;

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
}	HSParser;

#define RESIZEPRSBUF \
do { \
		if ( state->cur - state->word + 1 >= state->wordlen ) \
		{ \
				int4 clen = state->cur - state->word; \
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
get_val(HSParser * state, bool ignoreeq, bool *escaped)
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
				elog(ERROR, "Syntax error near '%c' at postion %d", *(state->ptr), (int4) (state->ptr - state->begin));
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
			elog(ERROR, "Unknown state %d at postion line %d in file '%s'", st, __LINE__, __FILE__);

		state->ptr++;
	}

	return false;
}

#define WKEY	0
#define WVAL	1
#define WEQ 2
#define WGT 3
#define WDEL	4


static void
parse_hstore(HSParser * state)
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
				elog(ERROR, "Unexpectd end of string");
			}
			else if (!isspace((unsigned char) *(state->ptr)))
			{
				elog(ERROR, "Syntax error near '%c' at postion %d", *(state->ptr), (int4) (state->ptr - state->begin));
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
				elog(ERROR, "Unexpectd end of string");
			}
			else
			{
				elog(ERROR, "Syntax error near '%c' at postion %d", *(state->ptr), (int4) (state->ptr - state->begin));
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
				elog(ERROR, "Syntax error near '%c' at postion %d", *(state->ptr), (int4) (state->ptr - state->begin));
			}
		}
		else
			elog(ERROR, "Unknown state %d at line %d in file '%s'", st, __LINE__, __FILE__);

		state->ptr++;
	}
}

int
comparePairs(const void *a, const void *b)
{
	if (((Pairs *) a)->keylen == ((Pairs *) b)->keylen)
	{
		int			res = strncmp(
								  ((Pairs *) a)->key,
								  ((Pairs *) b)->key,
								  ((Pairs *) a)->keylen
		);

		if (res)
			return res;

		/* guarantee that neddfree willl be later */
		if (((Pairs *) b)->needfree == ((Pairs *) a)->needfree)
			return 0;
		else if (((Pairs *) a)->needfree)
			return 1;
		else
			return -1;
	}
	return (((Pairs *) a)->keylen > ((Pairs *) b)->keylen) ? 1 : -1;
}

int
uniquePairs(Pairs * a, int4 l, int4 *buflen)
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
		if (ptr->keylen == res->keylen && strncmp(ptr->key, res->key, res->keylen) == 0)
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

static void
freeHSParse(HSParser * state)
{
	int			i;

	if (state->word)
		pfree(state->word);
	for (i = 0; i < state->pcur; i++)
		if (state->pairs[i].needfree)
		{
			if (state->pairs[i].key)
				pfree(state->pairs[i].key);
			if (state->pairs[i].val)
				pfree(state->pairs[i].val);
		}
	pfree(state->pairs);
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


PG_FUNCTION_INFO_V1(hstore_in);
Datum		hstore_in(PG_FUNCTION_ARGS);
Datum
hstore_in(PG_FUNCTION_ARGS)
{
	HSParser	state;
	int4		len,
				buflen,
				i;
	HStore	   *out;
	HEntry	   *entries;
	char	   *ptr;

	state.begin = PG_GETARG_CSTRING(0);

	parse_hstore(&state);

	if (state.pcur == 0)
	{
		freeHSParse(&state);
		len = CALCDATASIZE(0, 0);
		out = palloc(len);
		out->len = len;
		out->size = 0;
		PG_RETURN_POINTER(out);
	}

	state.pcur = uniquePairs(state.pairs, state.pcur, &buflen);

	len = CALCDATASIZE(state.pcur, buflen);
	out = palloc(len);
	out->len = len;
	out->size = state.pcur;

	entries = ARRPTR(out);
	ptr = STRPTR(out);

	for (i = 0; i < out->size; i++)
	{
		entries[i].keylen = state.pairs[i].keylen;
		entries[i].pos = ptr - STRPTR(out);
		memcpy(ptr, state.pairs[i].key, state.pairs[i].keylen);
		ptr += entries[i].keylen;

		entries[i].valisnull = state.pairs[i].isnull;
		if (entries[i].valisnull)
			entries[i].vallen = 4;		/* null */
		else
		{
			entries[i].vallen = state.pairs[i].vallen;
			memcpy(ptr, state.pairs[i].val, state.pairs[i].vallen);
			ptr += entries[i].vallen;
		}
	}

	freeHSParse(&state);
	PG_RETURN_POINTER(out);
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
				i,
				nnulls=0;
	char	   *out,
			   *ptr;
	char	   *base = STRPTR(in);
	HEntry	   *entries = ARRPTR(in);

	if (in->size == 0)
	{
		out = palloc(1);
		*out = '\0';
		PG_FREE_IF_COPY(in, 0);
		PG_RETURN_CSTRING(out);
	}

	for (i = 0; i < in->size; i++)
		if (entries[i].valisnull)
			nnulls++;
	buflen = (4 /* " */ + 2 /* => */ ) * ( in->size - nnulls ) +
			( 2 /* " */ + 2 /* => */ + 4 /* NULL */ ) * nnulls  +
			2 /* ,  */ * ( in->size - 1 ) +
			2 /* esc */ * (VARSIZE(in) - CALCDATASIZE(in->size, 0)) +
			1 /* \0 */;

	out = ptr = palloc(buflen);
	for (i = 0; i < in->size; i++)
	{
		*ptr++ = '"';
		ptr = cpw(ptr, base + entries[i].pos, entries[i].keylen);
		*ptr++ = '"';
		*ptr++ = '=';
		*ptr++ = '>';
		if (entries[i].valisnull)
		{
			*ptr++ = 'N';
			*ptr++ = 'U';
			*ptr++ = 'L';
			*ptr++ = 'L';
		}
		else
		{
			*ptr++ = '"';
			ptr = cpw(ptr, base + entries[i].pos + entries[i].keylen, entries[i].vallen);
			*ptr++ = '"';
		}

		if (i + 1 != in->size)
		{
			*ptr++ = ',';
			*ptr++ = ' ';
		}
	}
	*ptr = '\0';

	PG_FREE_IF_COPY(in, 0);
	PG_RETURN_CSTRING(out);
}
