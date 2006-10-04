/*
 * GiST support for ltree
 * Teodor Sigaev <teodor@stack.net>
 * $PostgreSQL: pgsql/contrib/ltree/ltree_gist.c,v 1.19 2006/10/04 00:29:45 momjian Exp $
 */

#include "ltree.h"
#include "access/gist.h"
#include "access/nbtree.h"
#include "access/skey.h"
#include "utils/array.h"
#include "crc32.h"

#define NEXTVAL(x) ( (lquery*)( (char*)(x) + INTALIGN( VARSIZE(x) ) ) )

PG_FUNCTION_INFO_V1(ltree_gist_in);
Datum		ltree_gist_in(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(ltree_gist_out);
Datum		ltree_gist_out(PG_FUNCTION_ARGS);

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

PG_FUNCTION_INFO_V1(ltree_compress);
Datum		ltree_compress(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(ltree_decompress);
Datum		ltree_decompress(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(ltree_same);
Datum		ltree_same(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(ltree_union);
Datum		ltree_union(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(ltree_penalty);
Datum		ltree_penalty(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(ltree_picksplit);
Datum		ltree_picksplit(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(ltree_consistent);
Datum		ltree_consistent(PG_FUNCTION_ARGS);

#define ISEQ(a,b)	( (a)->numlevel == (b)->numlevel && ltree_compare(a,b)==0 )
#define GETENTRY(vec,pos) ((ltree_gist *) DatumGetPointer((vec)->vector[(pos)].key))

Datum
ltree_compress(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	GISTENTRY  *retval = entry;

	if (entry->leafkey)
	{							/* ltree */
		ltree_gist *key;
		ltree	   *val = (ltree *) DatumGetPointer(PG_DETOAST_DATUM(entry->key));
		int4		len = LTG_HDRSIZE + val->len;

		key = (ltree_gist *) palloc(len);
		key->len = len;
		key->flag = LTG_ONENODE;
		memcpy((void *) LTG_NODE(key), (void *) val, val->len);

		retval = (GISTENTRY *) palloc(sizeof(GISTENTRY));
		gistentryinit(*retval, PointerGetDatum(key),
					  entry->rel, entry->page,
					  entry->offset, FALSE);
	}
	PG_RETURN_POINTER(retval);
}

Datum
ltree_decompress(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	ltree_gist *key = (ltree_gist *) DatumGetPointer(PG_DETOAST_DATUM(entry->key));

	if (PointerGetDatum(key) != entry->key)
	{
		GISTENTRY  *retval = (GISTENTRY *) palloc(sizeof(GISTENTRY));

		gistentryinit(*retval, PointerGetDatum(key),
					  entry->rel, entry->page,
					  entry->offset, FALSE);
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

	*result = false;
	if (LTG_ISONENODE(a) != LTG_ISONENODE(b))
		PG_RETURN_POINTER(result);

	if (LTG_ISONENODE(a))
		*result = (ISEQ(LTG_NODE(a), LTG_NODE(b))) ? true : false;
	else
	{
		int4		i;
		BITVECP		sa = LTG_SIGN(a),
					sb = LTG_SIGN(b);

		if (LTG_ISALLTRUE(a) != LTG_ISALLTRUE(b))
			PG_RETURN_POINTER(result);

		if (!ISEQ(LTG_LNODE(a), LTG_LNODE(b)))
			PG_RETURN_POINTER(result);
		if (!ISEQ(LTG_RNODE(a), LTG_RNODE(b)))
			PG_RETURN_POINTER(result);

		*result = true;
		if (!LTG_ISALLTRUE(a))
			LOOPBYTE(
					 if (sa[i] != sb[i])
					 {
				*result = false;
				break;
			}
		);
	}

	PG_RETURN_POINTER(result);
}

static void
hashing(BITVECP sign, ltree * t)
{
	int			tlen = t->numlevel;
	ltree_level *cur = LTREE_FIRST(t);
	int			hash;

	while (tlen > 0)
	{
		hash = ltree_crc32_sz(cur->name, cur->len);
		HASH(sign, hash);
		cur = LEVEL_NEXT(cur);
		tlen--;
	}
}

Datum
ltree_union(PG_FUNCTION_ARGS)
{
	GistEntryVector *entryvec = (GistEntryVector *) PG_GETARG_POINTER(0);
	int		   *size = (int *) PG_GETARG_POINTER(1);
	BITVEC		base;
	int4		i,
				j;
	ltree_gist *result,
			   *cur;
	ltree	   *left = NULL,
			   *right = NULL,
			   *curtree;
	bool		isalltrue = false;
	bool		isleqr;

	MemSet((void *) base, 0, sizeof(BITVEC));
	for (j = 0; j < entryvec->n; j++)
	{
		cur = GETENTRY(entryvec, j);
		if (LTG_ISONENODE(cur))
		{
			curtree = LTG_NODE(cur);
			hashing(base, curtree);
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

				LOOPBYTE(
						 ((unsigned char *) base)[i] |= sc[i];
				);
			}

			curtree = LTG_LNODE(cur);
			if (!left || ltree_compare(left, curtree) > 0)
				left = curtree;
			curtree = LTG_RNODE(cur);
			if (!right || ltree_compare(right, curtree) < 0)
				right = curtree;
		}
	}

	if (isalltrue == false)
	{
		isalltrue = true;
		LOOPBYTE(
				 if (((unsigned char *) base)[i] != 0xff)
				 {
			isalltrue = false;
			break;
		}
		);
	}

	isleqr = (left == right || ISEQ(left, right)) ? true : false;
	*size = LTG_HDRSIZE + ((isalltrue) ? 0 : SIGLEN) + left->len + ((isleqr) ? 0 : right->len);

	result = (ltree_gist *) palloc(*size);
	result->len = *size;
	result->flag = 0;

	if (isalltrue)
		result->flag |= LTG_ALLTRUE;
	else
		memcpy((void *) LTG_SIGN(result), base, SIGLEN);

	memcpy((void *) LTG_LNODE(result), (void *) left, left->len);
	if (isleqr)
		result->flag |= LTG_NORIGHT;
	else
		memcpy((void *) LTG_RNODE(result), (void *) right, right->len);

	PG_RETURN_POINTER(result);
}

Datum
ltree_penalty(PG_FUNCTION_ARGS)
{
	ltree_gist *origval = (ltree_gist *) DatumGetPointer(((GISTENTRY *) PG_GETARG_POINTER(0))->key);
	ltree_gist *newval = (ltree_gist *) DatumGetPointer(((GISTENTRY *) PG_GETARG_POINTER(1))->key);
	float	   *penalty = (float *) PG_GETARG_POINTER(2);
	int4		cmpr,
				cmpl;

	cmpl = ltree_compare(LTG_GETLNODE(origval), LTG_GETLNODE(newval));
	cmpr = ltree_compare(LTG_GETRNODE(newval), LTG_GETRNODE(origval));

	*penalty = Max(cmpl, 0) + Max(cmpr, 0);

	PG_RETURN_POINTER(penalty);
}

/* used for sorting */
typedef struct rix
{
	int			index;
	ltree	   *r;
}	RIX;

static int
treekey_cmp(const void *a, const void *b)
{
	return ltree_compare(
						 ((RIX *) a)->r,
						 ((RIX *) b)->r
		);
}


Datum
ltree_picksplit(PG_FUNCTION_ARGS)
{
	GistEntryVector *entryvec = (GistEntryVector *) PG_GETARG_POINTER(0);
	GIST_SPLITVEC *v = (GIST_SPLITVEC *) PG_GETARG_POINTER(1);
	OffsetNumber j;
	int4		i;
	RIX		   *array;
	OffsetNumber maxoff;
	int			nbytes;
	int			size;
	ltree	   *lu_l,
			   *lu_r,
			   *ru_l,
			   *ru_r;
	ltree_gist *lu,
			   *ru;
	BITVEC		ls,
				rs;
	bool		lisat = false,
				risat = false,
				isleqr;

	memset((void *) ls, 0, sizeof(BITVEC));
	memset((void *) rs, 0, sizeof(BITVEC));
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
		lu = GETENTRY(entryvec, j);		/* use as tmp val */
		array[j].r = LTG_GETLNODE(lu);
	}

	qsort((void *) &array[FirstOffsetNumber], maxoff - FirstOffsetNumber + 1,
		  sizeof(RIX), treekey_cmp);

	lu_l = lu_r = ru_l = ru_r = NULL;
	for (j = FirstOffsetNumber; j <= maxoff; j = OffsetNumberNext(j))
	{
		lu = GETENTRY(entryvec, array[j].index);		/* use as tmp val */
		if (j <= (maxoff - FirstOffsetNumber + 1) / 2)
		{
			v->spl_left[v->spl_nleft] = array[j].index;
			v->spl_nleft++;
			if (lu_r == NULL || ltree_compare(LTG_GETRNODE(lu), lu_r) > 0)
				lu_r = LTG_GETRNODE(lu);
			if (LTG_ISONENODE(lu))
				hashing(ls, LTG_NODE(lu));
			else
			{
				if (lisat || LTG_ISALLTRUE(lu))
					lisat = true;
				else
				{
					BITVECP		sc = LTG_SIGN(lu);

					LOOPBYTE(
							 ((unsigned char *) ls)[i] |= sc[i];
					);
				}
			}
		}
		else
		{
			v->spl_right[v->spl_nright] = array[j].index;
			v->spl_nright++;
			if (ru_r == NULL || ltree_compare(LTG_GETRNODE(lu), ru_r) > 0)
				ru_r = LTG_GETRNODE(lu);
			if (LTG_ISONENODE(lu))
				hashing(rs, LTG_NODE(lu));
			else
			{
				if (risat || LTG_ISALLTRUE(lu))
					risat = true;
				else
				{
					BITVECP		sc = LTG_SIGN(lu);

					LOOPBYTE(
							 ((unsigned char *) rs)[i] |= sc[i];
					);
				}
			}
		}
	}

	if (lisat == false)
	{
		lisat = true;
		LOOPBYTE(
				 if (((unsigned char *) ls)[i] != 0xff)
				 {
			lisat = false;
			break;
		}
		);
	}

	if (risat == false)
	{
		risat = true;
		LOOPBYTE(
				 if (((unsigned char *) rs)[i] != 0xff)
				 {
			risat = false;
			break;
		}
		);
	}

	lu_l = LTG_GETLNODE(GETENTRY(entryvec, array[FirstOffsetNumber].index));
	isleqr = (lu_l == lu_r || ISEQ(lu_l, lu_r)) ? true : false;
	size = LTG_HDRSIZE + ((lisat) ? 0 : SIGLEN) + lu_l->len + ((isleqr) ? 0 : lu_r->len);
	lu = (ltree_gist *) palloc(size);
	lu->len = size;
	lu->flag = 0;
	if (lisat)
		lu->flag |= LTG_ALLTRUE;
	else
		memcpy((void *) LTG_SIGN(lu), ls, SIGLEN);
	memcpy((void *) LTG_LNODE(lu), (void *) lu_l, lu_l->len);
	if (isleqr)
		lu->flag |= LTG_NORIGHT;
	else
		memcpy((void *) LTG_RNODE(lu), (void *) lu_r, lu_r->len);


	ru_l = LTG_GETLNODE(GETENTRY(entryvec, array[1 + ((maxoff - FirstOffsetNumber + 1) / 2)].index));
	isleqr = (ru_l == ru_r || ISEQ(ru_l, ru_r)) ? true : false;
	size = LTG_HDRSIZE + ((risat) ? 0 : SIGLEN) + ru_l->len + ((isleqr) ? 0 : ru_r->len);
	ru = (ltree_gist *) palloc(size);
	ru->len = size;
	ru->flag = 0;
	if (risat)
		ru->flag |= LTG_ALLTRUE;
	else
		memcpy((void *) LTG_SIGN(ru), rs, SIGLEN);
	memcpy((void *) LTG_LNODE(ru), (void *) ru_l, ru_l->len);
	if (isleqr)
		ru->flag |= LTG_NORIGHT;
	else
		memcpy((void *) LTG_RNODE(ru), (void *) ru_r, ru_r->len);

	v->spl_ldatum = PointerGetDatum(lu);
	v->spl_rdatum = PointerGetDatum(ru);

	PG_RETURN_POINTER(v);
}

static bool
gist_isparent(ltree_gist * key, ltree * query)
{
	int4		numlevel = query->numlevel;
	int			i;

	for (i = query->numlevel; i >= 0; i--)
	{
		query->numlevel = i;
		if (ltree_compare(query, LTG_GETLNODE(key)) >= 0 && ltree_compare(query, LTG_GETRNODE(key)) <= 0)
		{
			query->numlevel = numlevel;
			return true;
		}
	}

	query->numlevel = numlevel;
	return false;
}

static ltree *
copy_ltree(ltree * src)
{
	ltree	   *dst = (ltree *) palloc(src->len);

	memcpy(dst, src, src->len);
	return dst;
}

static bool
gist_ischild(ltree_gist * key, ltree * query)
{
	ltree	   *left = copy_ltree(LTG_GETLNODE(key));
	ltree	   *right = copy_ltree(LTG_GETRNODE(key));
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
gist_qe(ltree_gist * key, lquery * query)
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
				if (GETBIT(sign, HASHVAL(curv->val)))
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
gist_tqcmp(ltree * t, lquery * q)
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
		if ((res = strncmp(al->name, bl->name, Min(al->len, bl->len))) == 0)
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
gist_between(ltree_gist * key, lquery * query)
{
	if (query->firstgood == 0)
		return true;

	if (gist_tqcmp(LTG_GETLNODE(key), query) > 0)
		return false;

	if (gist_tqcmp(LTG_GETRNODE(key), query) < 0)
		return false;

	return true;
}

static bool
checkcondition_bit(void *checkval, ITEM * val)
{
	return (FLG_CANLOOKSIGN(val->flag)) ? GETBIT(checkval, HASHVAL(val->val)) : true;
}

static bool
gist_qtxt(ltree_gist * key, ltxtquery * query)
{
	if (LTG_ISALLTRUE(key))
		return true;

	return ltree_execute(
						 GETQUERY(query),
						 (void *) LTG_SIGN(key), false,
						 checkcondition_bit
		);
}

static bool
arrq_cons(ltree_gist * key, ArrayType *_query)
{
	lquery	   *query = (lquery *) ARR_DATA_PTR(_query);
	int			num = ArrayGetNItems(ARR_NDIM(_query), ARR_DIMS(_query));

	if (ARR_NDIM(_query) != 1)
		ereport(ERROR,
				(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
				 errmsg("array must be one-dimensional")));
	if (ARR_HASNULL(_query))
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("array must not contain nulls")));

	while (num > 0)
	{
		if (gist_qe(key, query) && gist_between(key, query))
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
	void	   *query = NULL;
	ltree_gist *key = (ltree_gist *) DatumGetPointer(entry->key);
	StrategyNumber strategy = (StrategyNumber) PG_GETARG_UINT16(2);
	bool		res = false;

	switch (strategy)
	{
		case BTLessStrategyNumber:
			query = PG_GETARG_LTREE(1);
			res = (GIST_LEAF(entry)) ?
				(ltree_compare((ltree *) query, LTG_NODE(key)) > 0)
				:
				(ltree_compare((ltree *) query, LTG_GETLNODE(key)) >= 0);
			break;
		case BTLessEqualStrategyNumber:
			query = PG_GETARG_LTREE(1);
			res = (ltree_compare((ltree *) query, LTG_GETLNODE(key)) >= 0);
			break;
		case BTEqualStrategyNumber:
			query = PG_GETARG_LTREE(1);
			if (GIST_LEAF(entry))
				res = (ltree_compare((ltree *) query, LTG_NODE(key)) == 0);
			else
				res = (
					   ltree_compare((ltree *) query, LTG_GETLNODE(key)) >= 0
					   &&
					   ltree_compare((ltree *) query, LTG_GETRNODE(key)) <= 0
					);
			break;
		case BTGreaterEqualStrategyNumber:
			query = PG_GETARG_LTREE(1);
			res = (ltree_compare((ltree *) query, LTG_GETRNODE(key)) <= 0);
			break;
		case BTGreaterStrategyNumber:
			query = PG_GETARG_LTREE(1);
			res = (GIST_LEAF(entry)) ?
				(ltree_compare((ltree *) query, LTG_GETRNODE(key)) < 0)
				:
				(ltree_compare((ltree *) query, LTG_GETRNODE(key)) <= 0);
			break;
		case 10:
			query = PG_GETARG_LTREE_COPY(1);
			res = (GIST_LEAF(entry)) ?
				inner_isparent((ltree *) query, LTG_NODE(key))
				:
				gist_isparent(key, (ltree *) query);
			break;
		case 11:
			query = PG_GETARG_LTREE(1);
			res = (GIST_LEAF(entry)) ?
				inner_isparent(LTG_NODE(key), (ltree *) query)
				:
				gist_ischild(key, (ltree *) query);
			break;
		case 12:
		case 13:
			query = PG_GETARG_LQUERY(1);
			if (GIST_LEAF(entry))
				res = DatumGetBool(DirectFunctionCall2(ltq_regex,
											  PointerGetDatum(LTG_NODE(key)),
											PointerGetDatum((lquery *) query)
													   ));
			else
				res = (gist_qe(key, (lquery *) query) && gist_between(key, (lquery *) query));
			break;
		case 14:
		case 15:
			query = PG_GETARG_LQUERY(1);
			if (GIST_LEAF(entry))
				res = DatumGetBool(DirectFunctionCall2(ltxtq_exec,
											  PointerGetDatum(LTG_NODE(key)),
											PointerGetDatum((lquery *) query)
													   ));
			else
				res = gist_qtxt(key, (ltxtquery *) query);
			break;
		case 16:
		case 17:
			query = DatumGetPointer(PG_DETOAST_DATUM(PG_GETARG_DATUM(1)));
			if (GIST_LEAF(entry))
				res = DatumGetBool(DirectFunctionCall2(lt_q_regex,
											  PointerGetDatum(LTG_NODE(key)),
										 PointerGetDatum((ArrayType *) query)
													   ));
			else
				res = arrq_cons(key, (ArrayType *) query);
			break;
		default:
			/* internal error */
			elog(ERROR, "unrecognized StrategyNumber: %d", strategy);
	}

	PG_FREE_IF_COPY(query, 1);
	PG_RETURN_BOOL(res);
}
