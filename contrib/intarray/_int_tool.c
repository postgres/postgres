#include "_int.h"


bool
inner_int_contains(ArrayType *a, ArrayType *b)
{
	int			na,
				nb;
	int			i,
				j,
				n;
	int		   *da,
			   *db;

	CHECKARRVALID(a);
	CHECKARRVALID(b);

	if (ARRISVOID(a) || ARRISVOID(b))
		return FALSE;

	na = ARRNELEMS(a);
	nb = ARRNELEMS(b);
	da = ARRPTR(a);
	db = ARRPTR(b);

	i = j = n = 0;
	while (i < na && j < nb)
		if (da[i] < db[j])
			i++;
		else if (da[i] == db[j])
		{
			n++;
			i++;
			j++;
		}
		else
			break;

	return (n == nb) ? TRUE : FALSE;
}

bool
inner_int_overlap(ArrayType *a, ArrayType *b)
{
	int			na,
				nb;
	int			i,
				j;
	int		   *da,
			   *db;

	CHECKARRVALID(a);
	CHECKARRVALID(b);

	if (ARRISVOID(a) || ARRISVOID(b))
		return FALSE;

	na = ARRNELEMS(a);
	nb = ARRNELEMS(b);
	da = ARRPTR(a);
	db = ARRPTR(b);

	i = j = 0;
	while (i < na && j < nb)
		if (da[i] < db[j])
			i++;
		else if (da[i] == db[j])
			return TRUE;
		else
			j++;

	return FALSE;
}

ArrayType *
inner_int_union(ArrayType *a, ArrayType *b)
{
	ArrayType  *r = NULL;

	CHECKARRVALID(a);
	CHECKARRVALID(b);

	if (ARRISVOID(a) && ARRISVOID(b))
		return new_intArrayType(0);
	if (ARRISVOID(a))
		r = copy_intArrayType(b);
	if (ARRISVOID(b))
		r = copy_intArrayType(a);

	if (!r)
	{
		int			na = ARRNELEMS(a),
					nb = ARRNELEMS(b);
		int		   *da = ARRPTR(a),
				   *db = ARRPTR(b);
		int			i,
					j,
				   *dr;

		r = new_intArrayType(na + nb);
		dr = ARRPTR(r);

		/* union */
		i = j = 0;
		while (i < na && j < nb)
		{
			if (da[i] == db[j])
			{
				*dr++ = da[i++];
				j++;
			}
			else if (da[i] < db[j])
				*dr++ = da[i++];
			else
				*dr++ = db[j++];
		}

		while (i < na)
			*dr++ = da[i++];
		while (j < nb)
			*dr++ = db[j++];

		r = resize_intArrayType(r, dr - ARRPTR(r));
	}

	if (ARRNELEMS(r) > 1)
		r = _int_unique(r);

	return r;
}

ArrayType *
inner_int_inter(ArrayType *a, ArrayType *b)
{
	ArrayType  *r;
	int			na,
				nb;
	int		   *da,
			   *db,
			   *dr;
	int			i,
				j;

	CHECKARRVALID(a);
	CHECKARRVALID(b);

	if (ARRISVOID(a) || ARRISVOID(b))
		return new_intArrayType(0);

	na = ARRNELEMS(a);
	nb = ARRNELEMS(b);
	da = ARRPTR(a);
	db = ARRPTR(b);
	r = new_intArrayType(Min(na, nb));
	dr = ARRPTR(r);

	i = j = 0;
	while (i < na && j < nb)
		if (da[i] < db[j])
			i++;
		else if (da[i] == db[j])
		{
			if (i + j == 0 || (i + j > 0 && *(dr - 1) != db[j]))
				*dr++ = db[j];
			i++;
			j++;
		}
		else
			j++;

	if ((dr - ARRPTR(r)) == 0)
	{
		pfree(r);
		return new_intArrayType(0);
	}
	else
		return resize_intArrayType(r, dr - ARRPTR(r));
}

void
rt__int_size(ArrayType *a, float *size)
{
	*size = (float) ARRNELEMS(a);

	return;
}


/* len >= 2 */
bool
isort(int4 *a, int len)
{
	int4		tmp,
				index;
	int4	   *cur,
			   *end;
	bool		r = FALSE;

	end = a + len;
	do
	{
		index = 0;
		cur = a + 1;
		while (cur < end)
		{
			if (*(cur - 1) > *cur)
			{
				tmp = *(cur - 1);
				*(cur - 1) = *cur;
				*cur = tmp;
				index = 1;
			}
			else if (!r && *(cur - 1) == *cur)
				r = TRUE;
			cur++;
		}
	} while (index);
	return r;
}

ArrayType *
new_intArrayType(int num)
{
	ArrayType  *r;
	int			nbytes = ARR_OVERHEAD_NONULLS(NDIM) + sizeof(int) * num;

	r = (ArrayType *) palloc0(nbytes);

	ARR_SIZE(r) = nbytes;
	ARR_NDIM(r) = NDIM;
	r->dataoffset = 0;			/* marker for no null bitmap */
	ARR_ELEMTYPE(r) = INT4OID;
	*((int *) ARR_DIMS(r)) = num;
	*((int *) ARR_LBOUND(r)) = 1;

	return r;
}

ArrayType *
resize_intArrayType(ArrayType *a, int num)
{
	int			nbytes = ARR_OVERHEAD_NONULLS(NDIM) + sizeof(int) * num;

	if (num == ARRNELEMS(a))
		return a;

	a = (ArrayType *) repalloc(a, nbytes);

	a->size = nbytes;
	*((int *) ARR_DIMS(a)) = num;
	return a;
}

ArrayType *
copy_intArrayType(ArrayType *a)
{
	ArrayType  *r;

	r = new_intArrayType(ARRNELEMS(a));
	memmove(r, a, VARSIZE(r));
	return r;
}

/* num for compressed key */
int
internal_size(int *a, int len)
{
	int			i,
				size = 0;

	for (i = 0; i < len; i += 2)
		if (!i || a[i] != a[i - 1])		/* do not count repeated range */
			size += a[i + 1] - a[i] + 1;

	return size;
}

/* r is sorted and size of r > 1 */
ArrayType *
_int_unique(ArrayType *r)
{
	int		   *tmp,
			   *dr,
			   *data;
	int			num = ARRNELEMS(r);

	CHECKARRVALID(r);

	if (num < 2)
		return r;

	data = tmp = dr = ARRPTR(r);
	while (tmp - data < num)
		if (*tmp != *dr)
			*(++dr) = *tmp++;
		else
			tmp++;
	return resize_intArrayType(r, dr + 1 - ARRPTR(r));
}

void
gensign(BITVEC sign, int *a, int len)
{
	int			i;

	/* we assume that the sign vector is previously zeroed */
	for (i = 0; i < len; i++)
	{
		HASH(sign, *a);
		a++;
	}
}

int32
intarray_match_first(ArrayType *a, int32 elem)
{
	int32	   *aa,
				c,
				i;

	CHECKARRVALID(a);
	if (ARRISVOID(a))
		return 0;
	c = ARRNELEMS(a);
	aa = ARRPTR(a);
	for (i = 0; i < c; i++)
		if (aa[i] == elem)
			return (i + 1);
	return 0;
}

ArrayType *
intarray_add_elem(ArrayType *a, int32 elem)
{
	ArrayType  *result;
	int32	   *r;
	int32		c;

	CHECKARRVALID(a);
	c = (ARRISVOID(a)) ? 0 : ARRNELEMS(a);
	result = new_intArrayType(c + 1);
	r = ARRPTR(result);
	if (c > 0)
		memcpy(r, ARRPTR(a), c * sizeof(int32));
	r[c] = elem;
	return result;
}

ArrayType *
intarray_concat_arrays(ArrayType *a, ArrayType *b)
{
	ArrayType  *result;
	int32		ac = (ARRISVOID(a)) ? 0 : ARRNELEMS(a);
	int32		bc = (ARRISVOID(b)) ? 0 : ARRNELEMS(b);

	CHECKARRVALID(a);
	CHECKARRVALID(b);
	result = new_intArrayType(ac + bc);
	if (ac)
		memcpy(ARRPTR(result), ARRPTR(a), ac * sizeof(int32));
	if (bc)
		memcpy(ARRPTR(result) + ac, ARRPTR(b), bc * sizeof(int32));
	return result;
}

ArrayType *
int_to_intset(int32 n)
{
	ArrayType  *result;
	int32	   *aa;

	result = new_intArrayType(1);
	aa = ARRPTR(result);
	aa[0] = n;
	return result;
}

int
compASC(const void *a, const void *b)
{
	if (*(int4 *) a == *(int4 *) b)
		return 0;
	return (*(int4 *) a > *(int4 *) b) ? 1 : -1;
}

int
compDESC(const void *a, const void *b)
{
	if (*(int4 *) a == *(int4 *) b)
		return 0;
	return (*(int4 *) a < *(int4 *) b) ? 1 : -1;
}
