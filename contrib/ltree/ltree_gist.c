/*
 * GiST support for ltree
 * Teodor Sigaev <teodor@stack.net>
 * contrib/ltree/ltree_gist.c
 */
#include "postgres.h"

#include "access/gist.h"
#include "access/reloptions.h"
#include "access/stratnum.h"
#include "crc32.h"
#include "ltree.h"

#define NEXTVAL(x) ( (lquery*)( (char*)(x) + INTALIGN( VARSIZE(x) ) ) )
#define ISEQ(a,b)	( (a)->numlevel == (b)->numlevel && ltree_compare(a,b)==0 )

PG_FUNCTION_INFO_V1(ltree_gist_in);
PG_FUNCTION_INFO_V1(ltree_gist_out);

Datum
ltree_gist_in(PG_FUNCTION_ARGS)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("ltree_gist_in() not implemented")));
	PG_RETURN_DATUM(0);
}

Datum
ltree_gist_out(PG_FUNCTION_ARGS)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("ltree_gist_out() not implemented")));
	PG_RETURN_DATUM(0);
}

ltree_gist *
ltree_gist_alloc(bool isalltrue, BITVECP sign, int siglen,
				 ltree *left, ltree *right)
{
	int32		size = LTG_HDRSIZE + (isalltrue ? 0 : siglen) +
	(left ? VARSIZE(left) + (right ? VARSIZE(right) : 0) : 0);
	ltree_gist *result = palloc(size);

	SET_VARSIZE(result, size);

	if (siglen)
	{
		result->flag = 0;

		if (isalltrue)
			result->flag |= LTG_ALLTRUE;
		else if (sign)
			memcpy(LTG_SIGN(result), sign, siglen);
		else
			memset(LTG_SIGN(result), 0, siglen);

		if (left)
		{
			memcpy(LTG_LNODE(result, siglen), left, VARSIZE(left));

			if (!right || left == right || ISEQ(left, right))
				result->flag |= LTG_NORIGHT;
			else
				memcpy(LTG_RNODE(result, siglen), right, VARSIZE(right));
		}
	}
	else
	{
		Assert(left);
		result->flag = LTG_ONENODE;
		memcpy(LTG_NODE(result), left, VARSIZE(left));
	}

	return result;
}

PG_FUNCTION_INFO_V1(ltree_compress);
PG_FUNCTION_INFO_V1(ltree_decompress);
PG_FUNCTION_INFO_V1(ltree_same);
PG_FUNCTION_INFO_V1(ltree_union);
PG_FUNCTION_INFO_V1(ltree_penalty);
PG_FUNCTION_INFO_V1(ltree_picksplit);
PG_FUNCTION_INFO_V1(ltree_consistent);
PG_FUNCTION_INFO_V1(ltree_gist_options);

#define GETENTRY(vec,pos) ((ltree_gist *) DatumGetPointer((vec)->vector[(pos)].key))

Datum
ltree_compress(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	GISTENTRY  *retval = entry;

	if (entry->leafkey)
	{							/* ltree */
		ltree	   *val = DatumGetLtreeP(entry->key);
		ltree_gist *key = ltree_gist_alloc(false, NULL, 0, val, 0);

		retval = (GISTENTRY *) palloc(sizeof(GISTENTRY));
		gistentryinit(*retval, PointerGetDatum(key),
					  entry->rel, entry->page,
					  entry->offset, false);
	}
	PG_RETURN_POINTER(retval);
}

Datum
ltree_decompress(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	ltree_gist *key = (ltree_gist *) PG_DETOAST_DATUM(entry->key);

	if (PointerGetDatum(key) != entry->key)
	{
		GISTENTRY  *retval = (GISTENTRY *) palloc(sizeof(GISTENTRY));

		gistentryinit(*retval, PointerGetDatum(key),
					  entry->rel, entry->page,
					  entry->offset, false);
		PG_RETURN_POINTER(retval);
	}
	PG_RETURN_POINTER(entry);
}

Datum
ltree_same(PG_FUNCTION_ARGS)
{
	ltree_gist *a = (ltree_gist *) PG_GETARG_POINTER(0);
	ltree_gist *b = (ltree_gist *) PG_GETARG_POINTER(1);
	bool	   *result = (bool *) PG_GETARG_POINTER(2);
	int			siglen = LTREE_GET_ASIGLEN();

	*result = false;
	if (LTG_ISONENODE(a) != LTG_ISONENODE(b))
		PG_RETURN_POINTER(result);

	if (LTG_ISONENODE(a))
		*result = (ISEQ(LTG_NODE(a), LTG_NODE(b))) ? true : false;
	else
	{
		int32		i;
		BITVECP		sa = LTG_SIGN(a),
					sb = LTG_SIGN(b);

		if (LTG_ISALLTRUE(a) != LTG_ISALLTRUE(b))
			PG_RETURN_POINTER(result);

		if (!ISEQ(LTG_LNODE(a, siglen), LTG_LNODE(b, siglen)))
			PG_RETURN_POINTER(result);
		if (!ISEQ(LTG_RNODE(a, siglen), LTG_RNODE(b, siglen)))
			PG_RETURN_POINTER(result);

		*result = true;
		if (!LTG_ISALLTRUE(a))
		{
			LOOPBYTE(siglen)
			{
				if (sa[i] != sb[i])
				{
					*result = false;
					break;
				}
			}
		}
	}

	PG_RETURN_POINTER(result);
}

static void
hashing(BITVECP sign, ltree *t, int siglen)
{
	int			tlen = t->numlevel;
	ltree_level *cur = LTREE_FIRST(t);
	int			hash;

	while (tlen > 0)
	{
		hash = ltree_crc32_sz(cur->name, cur->len);
		HASH(sign, hash, siglen);
		cur = LEVEL_NEXT(cur);
		tlen--;
	}
}

Datum
ltree_union(PG_FUNCTION_ARGS)
{
	GistEntryVector *entryvec = (GistEntryVector *) PG_GETARG_POINTER(0);
	int		   *size = (int *) PG_GETARG_POINTER(1);
	int			siglen = LTREE_GET_ASIGLEN();
	BITVECP		base = palloc0(siglen);
	int32		i,
				j;
	ltree_gist *result,
			   *cur;
	ltree	   *left = NULL,
			   *right = NULL,
			   *curtree;
	bool		isalltrue = false;

	for (j = 0; j < entryvec->n; j++)
	{
		cur = GETENTRY(entryvec, j);
		if (LTG_ISONENODE(cur))
		{
			curtree = LTG_NODE(cur);
			hashing(base, curtree, siglen);
			if (!left || ltree_compare(left, curtree) > 0)
				left = curtree;
			if (!right || ltree_compare(right, curtree) < 0)
				right = curtree;
		}
		else
		{
			if (isalltrue || LTG_ISALLTRUE(cur))
				isalltrue = true;
			else
			{
				BITVECP		sc = LTG_SIGN(cur);

				LOOPBYTE(siglen)
					((unsigned char *) base)[i] |= sc[i];
			}

			curtree = LTG_LNODE(cur, siglen);
			if (!left || ltree_compare(left, curtree) > 0)
				left = curtree;
			curtree = LTG_RNODE(cur, siglen);
			if (!right || ltree_compare(right, curtree) < 0)
				right = curtree;
		}
	}

	if (isalltrue == false)
	{
		isalltrue = true;
		LOOPBYTE(siglen)
		{
			if (((unsigned char *) base)[i] != 0xff)
			{
				isalltrue = false;
				break;
			}
		}
	}

	result = ltree_gist_alloc(isalltrue, base, siglen, left, right);

	*size = VARSIZE(result);

	PG_RETURN_POINTER(result);
}

Datum
ltree_penalty(PG_FUNCTION_ARGS)
{
	ltree_gist *origval = (ltree_gist *) DatumGetPointer(((GISTENTRY *) PG_GETARG_POINTER(0))->key);
	ltree_gist *newval = (ltree_gist *) DatumGetPointer(((GISTENTRY *) PG_GETARG_POINTER(1))->key);
	float	   *penalty = (float *) PG_GETARG_POINTER(2);
	int			siglen = LTREE_GET_ASIGLEN();
	int32		cmpr,
				cmpl;

	cmpl = ltree_compare(LTG_GETLNODE(origval, siglen), LTG_GETLNODE(newval, siglen));
	cmpr = ltree_compare(LTG_GETRNODE(newval, siglen), LTG_GETRNODE(origval, siglen));

	*penalty = Max(cmpl, 0) + Max(cmpr, 0);

	PG_RETURN_POINTER(penalty);
}

/* used for sorting */
typedef struct rix
{
	int			index;
	ltree	   *r;
} RIX;

static int
treekey_cmp(const void *a, const void *b)
{
	return ltree_compare(((const RIX *) a)->r,
						 ((const RIX *) b)->r);
}


Datum
ltree_picksplit(PG_FUNCTION_ARGS)
{
	GistEntryVector *entryvec = (GistEntryVector *) PG_GETARG_POINTER(0);
	GIST_SPLITVEC *v = (GIST_SPLITVEC *) PG_GETARG_POINTER(1);
	int			siglen = LTREE_GET_ASIGLEN();
	OffsetNumber j;
	int32		i;
	RIX		   *array;
	OffsetNumber maxoff;
	int			nbytes;
	ltree	   *lu_l,
			   *lu_r,
			   *ru_l,
			   *ru_r;
	ltree_gist *lu,
			   *ru;
	BITVECP		ls = palloc0(siglen),
				rs = palloc0(siglen);
	bool		lisat = false,
				risat = false;

	maxoff = entryvec->n - 1;
	nbytes = (maxoff + 2) * sizeof(OffsetNumber);
	v->spl_left = (OffsetNumber *) palloc(nbytes);
	v->spl_right = (OffsetNumber *) palloc(nbytes);
	v->spl_nleft = 0;
	v->spl_nright = 0;
	array = (RIX *) palloc(sizeof(RIX) * (maxoff + 1));

	/* copy the data into RIXes, and sort the RIXes */
	for (j = FirstOffsetNumber; j <= maxoff; j = OffsetNumberNext(j))
	{
		array[j].index = j;
		lu = GETENTRY(entryvec, j); /* use as tmp val */
		array[j].r = LTG_GETLNODE(lu, siglen);
	}

	qsort((void *) &array[FirstOffsetNumber], maxoff - FirstOffsetNumber + 1,
		  sizeof(RIX), treekey_cmp);

	lu_l = lu_r = ru_l = ru_r = NULL;
	for (j = FirstOffsetNumber; j <= maxoff; j = OffsetNumberNext(j))
	{
		lu = GETENTRY(entryvec, array[j].index);	/* use as tmp val */
		if (j <= (maxoff - FirstOffsetNumber + 1) / 2)
		{
			v->spl_left[v->spl_nleft] = array[j].index;
			v->spl_nleft++;
			if (lu_r == NULL || ltree_compare(LTG_GETRNODE(lu, siglen), lu_r) > 0)
				lu_r = LTG_GETRNODE(lu, siglen);
			if (LTG_ISONENODE(lu))
				hashing(ls, LTG_NODE(lu), siglen);
			else
			{
				if (lisat || LTG_ISALLTRUE(lu))
					lisat = true;
				else
				{
					BITVECP		sc = LTG_SIGN(lu);

					LOOPBYTE(siglen)
						((unsigned char *) ls)[i] |= sc[i];
				}
			}
		}
		else
		{
			v->spl_right[v->spl_nright] = array[j].index;
			v->spl_nright++;
			if (ru_r == NULL || ltree_compare(LTG_GETRNODE(lu, siglen), ru_r) > 0)
				ru_r = LTG_GETRNODE(lu, siglen);
			if (LTG_ISONENODE(lu))
				hashing(rs, LTG_NODE(lu), siglen);
			else
			{
				if (risat || LTG_ISALLTRUE(lu))
					risat = true;
				else
				{
					BITVECP		sc = LTG_SIGN(lu);

					LOOPBYTE(siglen)
						((unsigned char *) rs)[i] |= sc[i];
				}
			}
		}
	}

	if (lisat == false)
	{
		lisat = true;
		LOOPBYTE(siglen)
		{
			if (((unsigned char *) ls)[i] != 0xff)
			{
				lisat = false;
				break;
			}
		}
	}

	if (risat == false)
	{
		risat = true;
		LOOPBYTE(siglen)
		{
			if (((unsigned char *) rs)[i] != 0xff)
			{
				risat = false;
				break;
			}
		}
	}

	lu_l = LTG_GETLNODE(GETENTRY(entryvec, array[FirstOffsetNumber].index), siglen);
	lu = ltree_gist_alloc(lisat, ls, siglen, lu_l, lu_r);

	ru_l = LTG_GETLNODE(GETENTRY(entryvec, array[1 + ((maxoff - FirstOffsetNumber + 1) / 2)].index), siglen);
	ru = ltree_gist_alloc(risat, rs, siglen, ru_l, ru_r);

	pfree(ls);
	pfree(rs);

	v->spl_ldatum = PointerGetDatum(lu);
	v->spl_rdatum = PointerGetDatum(ru);

	PG_RETURN_POINTER(v);
}

static bool
gist_isparent(ltree_gist *key, ltree *query, int siglen)
{
	int32		numlevel = query->numlevel;
	int			i;

	for (i = query->numlevel; i >= 0; i--)
	{
		query->numlevel = i;
		if (ltree_compare(query, LTG_GETLNODE(key, siglen)) >= 0 &&
			ltree_compare(query, LTG_GETRNODE(key, siglen)) <= 0)
		{
			query->numlevel = numlevel;
			return true;
		}
	}

	query->numlevel = numlevel;
	return false;
}

static ltree *
copy_ltree(ltree *src)
{
	ltree	   *dst = (ltree *) palloc0(VARSIZE(src));

	memcpy(dst, src, VARSIZE(src));
	return dst;
}

static bool
gist_ischild(ltree_gist *key, ltree *query, int siglen)
{
	ltree	   *left = copy_ltree(LTG_GETLNODE(key, siglen));
	ltree	   *right = copy_ltree(LTG_GETRNODE(key, siglen));
	bool		res = true;

	if (left->numlevel > query->numlevel)
		left->numlevel = query->numlevel;

	if (ltree_compare(query, left) < 0)
		res = false;

	if (right->numlevel > query->numlevel)
		right->numlevel = query->numlevel;

	if (res && ltree_compare(query, right) > 0)
		res = false;

	pfree(left);
	pfree(right);

	return res;
}

static bool
gist_qe(ltree_gist *key, lquery *query, int siglen)
{
	lquery_level *curq = LQUERY_FIRST(query);
	BITVECP		sign = LTG_SIGN(key);
	int			qlen = query->numlevel;

	if (LTG_ISALLTRUE(key))
		return true;

	while (qlen > 0)
	{
		if (curq->numvar && LQL_CANLOOKSIGN(curq))
		{
			bool		isexist = false;
			int			vlen = curq->numvar;
			lquery_variant *curv = LQL_FIRST(curq);

			while (vlen > 0)
			{
				if (GETBIT(sign, HASHVAL(curv->val, siglen)))
				{
					isexist = true;
					break;
				}
				curv = LVAR_NEXT(curv);
				vlen--;
			}
			if (!isexist)
				return false;
		}

		curq = LQL_NEXT(curq);
		qlen--;
	}

	return true;
}

static int
gist_tqcmp(ltree *t, lquery *q)
{
	ltree_level *al = LTREE_FIRST(t);
	lquery_level *ql = LQUERY_FIRST(q);
	lquery_variant *bl;
	int			an = t->numlevel;
	int			bn = q->firstgood;
	int			res = 0;

	while (an > 0 && bn > 0)
	{
		bl = LQL_FIRST(ql);
		if ((res = memcmp(al->name, bl->name, Min(al->len, bl->len))) == 0)
		{
			if (al->len != bl->len)
				return al->len - bl->len;
		}
		else
			return res;
		an--;
		bn--;
		al = LEVEL_NEXT(al);
		ql = LQL_NEXT(ql);
	}

	return Min(t->numlevel, q->firstgood) - q->firstgood;
}

static bool
gist_between(ltree_gist *key, lquery *query, int siglen)
{
	if (query->firstgood == 0)
		return true;

	if (gist_tqcmp(LTG_GETLNODE(key, siglen), query) > 0)
		return false;

	if (gist_tqcmp(LTG_GETRNODE(key, siglen), query) < 0)
		return false;

	return true;
}

typedef struct LtreeSignature
{
	BITVECP		sign;
	int			siglen;
} LtreeSignature;

static bool
checkcondition_bit(void *cxt, ITEM *val)
{
	LtreeSignature *sig = cxt;

	return (FLG_CANLOOKSIGN(val->flag)) ? GETBIT(sig->sign, HASHVAL(val->val, sig->siglen)) : true;
}

static bool
gist_qtxt(ltree_gist *key, ltxtquery *query, int siglen)
{
	LtreeSignature sig;

	if (LTG_ISALLTRUE(key))
		return true;

	sig.sign = LTG_SIGN(key);
	sig.siglen = siglen;

	return ltree_execute(GETQUERY(query),
						 &sig, false,
						 checkcondition_bit);
}

static bool
arrq_cons(ltree_gist *key, ArrayType *_query, int siglen)
{
	lquery	   *query = (lquery *) ARR_DATA_PTR(_query);
	int			num = ArrayGetNItems(ARR_NDIM(_query), ARR_DIMS(_query));

	if (ARR_NDIM(_query) > 1)
		ereport(ERROR,
				(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
				 errmsg("array must be one-dimensional")));
	if (array_contains_nulls(_query))
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("array must not contain nulls")));

	while (num > 0)
	{
		if (gist_qe(key, query, siglen) && gist_between(key, query, siglen))
			return true;
		num--;
		query = NEXTVAL(query);
	}
	return false;
}

Datum
ltree_consistent(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	StrategyNumber strategy = (StrategyNumber) PG_GETARG_UINT16(2);

	/* Oid		subtype = PG_GETARG_OID(3); */
	bool	   *recheck = (bool *) PG_GETARG_POINTER(4);
	int			siglen = LTREE_GET_ASIGLEN();
	ltree_gist *key = (ltree_gist *) DatumGetPointer(entry->key);
	void	   *query = NULL;
	bool		res = false;

	/* All cases served by this function are exact */
	*recheck = false;

	switch (strategy)
	{
		case BTLessStrategyNumber:
			query = PG_GETARG_LTREE_P(1);
			res = (GIST_LEAF(entry)) ?
				(ltree_compare((ltree *) query, LTG_NODE(key)) > 0)
				:
				(ltree_compare((ltree *) query, LTG_GETLNODE(key, siglen)) >= 0);
			break;
		case BTLessEqualStrategyNumber:
			query = PG_GETARG_LTREE_P(1);
			res = (ltree_compare((ltree *) query, LTG_GETLNODE(key, siglen)) >= 0);
			break;
		case BTEqualStrategyNumber:
			query = PG_GETARG_LTREE_P(1);
			if (GIST_LEAF(entry))
				res = (ltree_compare((ltree *) query, LTG_NODE(key)) == 0);
			else
				res = (ltree_compare((ltree *) query, LTG_GETLNODE(key, siglen)) >= 0
					   &&
					   ltree_compare((ltree *) query, LTG_GETRNODE(key, siglen)) <= 0);
			break;
		case BTGreaterEqualStrategyNumber:
			query = PG_GETARG_LTREE_P(1);
			res = (ltree_compare((ltree *) query, LTG_GETRNODE(key, siglen)) <= 0);
			break;
		case BTGreaterStrategyNumber:
			query = PG_GETARG_LTREE_P(1);
			res = (GIST_LEAF(entry)) ?
				(ltree_compare((ltree *) query, LTG_GETRNODE(key, siglen)) < 0)
				:
				(ltree_compare((ltree *) query, LTG_GETRNODE(key, siglen)) <= 0);
			break;
		case 10:
			query = PG_GETARG_LTREE_P_COPY(1);
			res = (GIST_LEAF(entry)) ?
				inner_isparent((ltree *) query, LTG_NODE(key))
				:
				gist_isparent(key, (ltree *) query, siglen);
			break;
		case 11:
			query = PG_GETARG_LTREE_P(1);
			res = (GIST_LEAF(entry)) ?
				inner_isparent(LTG_NODE(key), (ltree *) query)
				:
				gist_ischild(key, (ltree *) query, siglen);
			break;
		case 12:
		case 13:
			query = PG_GETARG_LQUERY_P(1);
			if (GIST_LEAF(entry))
				res = DatumGetBool(DirectFunctionCall2(ltq_regex,
													   PointerGetDatum(LTG_NODE(key)),
													   PointerGetDatum((lquery *) query)
													   ));
			else
				res = (gist_qe(key, (lquery *) query, siglen) &&
					   gist_between(key, (lquery *) query, siglen));
			break;
		case 14:
		case 15:
			query = PG_GETARG_LTXTQUERY_P(1);
			if (GIST_LEAF(entry))
				res = DatumGetBool(DirectFunctionCall2(ltxtq_exec,
													   PointerGetDatum(LTG_NODE(key)),
													   PointerGetDatum((ltxtquery *) query)
													   ));
			else
				res = gist_qtxt(key, (ltxtquery *) query, siglen);
			break;
		case 16:
		case 17:
			query = PG_GETARG_ARRAYTYPE_P(1);
			if (GIST_LEAF(entry))
				res = DatumGetBool(DirectFunctionCall2(lt_q_regex,
													   PointerGetDatum(LTG_NODE(key)),
													   PointerGetDatum((ArrayType *) query)
													   ));
			else
				res = arrq_cons(key, (ArrayType *) query, siglen);
			break;
		default:
			/* internal error */
			elog(ERROR, "unrecognized StrategyNumber: %d", strategy);
	}

	PG_FREE_IF_COPY(query, 1);
	PG_RETURN_BOOL(res);
}

Datum
ltree_gist_options(PG_FUNCTION_ARGS)
{
	local_relopts *relopts = (local_relopts *) PG_GETARG_POINTER(0);

	init_local_reloptions(relopts, sizeof(LtreeGistOptions));
	add_local_int_reloption(relopts, "siglen",
							"signature length in bytes",
							SIGLEN_DEFAULT, 1, SIGLEN_MAX,
							offsetof(LtreeGistOptions, siglen));

	PG_RETURN_VOID();
}
