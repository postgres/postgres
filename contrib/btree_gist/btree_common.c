#include "btree_gist.h"

PG_FUNCTION_INFO_V1(btree_decompress);
Datum		btree_decompress(PG_FUNCTION_ARGS);

/*
** GiST DeCompress methods
** do not do anything.
*/
Datum
btree_decompress(PG_FUNCTION_ARGS)
{
	PG_RETURN_POINTER(PG_GETARG_POINTER(0));
}


/**************************************************
 * Common btree-function (for all ops)
 **************************************************/

/*
** The GiST PickSplit method
*/
extern GIST_SPLITVEC *
btree_picksplit(bytea *entryvec, GIST_SPLITVEC *v, BINARY_UNION bu, CMPFUNC cmp)
{
	OffsetNumber i;
	RIX		   *array;
	OffsetNumber maxoff;
	int			nbytes;

	maxoff = ((VARSIZE(entryvec) - VARHDRSZ) / sizeof(GISTENTRY)) - 1;
	nbytes = (maxoff + 2) * sizeof(OffsetNumber);
	v->spl_left = (OffsetNumber *) palloc(nbytes);
	v->spl_right = (OffsetNumber *) palloc(nbytes);
	v->spl_nleft = 0;
	v->spl_nright = 0;
	v->spl_ldatum = PointerGetDatum(0);
	v->spl_rdatum = PointerGetDatum(0);
	array = (RIX *) palloc(sizeof(RIX) * (maxoff + 1));

	/* copy the data into RIXes, and sort the RIXes */
	for (i = FirstOffsetNumber; i <= maxoff; i = OffsetNumberNext(i))
	{
		array[i].index = i;
		array[i].r = (char *) DatumGetPointer((((GISTENTRY *) (VARDATA(entryvec)))[i].key));
	}
	qsort((void *) &array[FirstOffsetNumber], maxoff - FirstOffsetNumber + 1,
		  sizeof(RIX), cmp);

	for (i = FirstOffsetNumber; i <= maxoff; i = OffsetNumberNext(i))
	{
		if (i <= (maxoff - FirstOffsetNumber + 1) / 2)
		{
			v->spl_left[v->spl_nleft] = array[i].index;
			v->spl_nleft++;
			(*bu) (&v->spl_ldatum, array[i].r);
		}
		else
		{
			v->spl_right[v->spl_nright] = array[i].index;
			v->spl_nright++;
			(*bu) (&v->spl_rdatum, array[i].r);
		}
	}
	pfree(array);

	return (v);
}
