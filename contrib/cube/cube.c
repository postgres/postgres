/******************************************************************************
  This file contains routines that can be bound to a Postgres backend and
  called by the backend in the process of processing queries.  The calling
  format for these routines is dictated by Postgres architecture.
******************************************************************************/

#include "postgres.h"

#include <math.h>

#include "access/gist.h"
#include "access/rtree.h"
#include "lib/stringinfo.h"
#include "utils/builtins.h"

#include "cubedata.h"

#define max(a,b)		((a) >	(b) ? (a) : (b))
#define min(a,b)		((a) <= (b) ? (a) : (b))
#define abs(a)			((a) <	(0) ? (-a) : (a))

extern int	cube_yyparse();
extern void cube_yyerror(const char *message);
extern void cube_scanner_init(const char *str);
extern void cube_scanner_finish(void);

/*
** Input/Output routines
*/
NDBOX	   *cube_in(char *str);
NDBOX	   *cube(text *str);
char	   *cube_out(NDBOX * cube);
NDBOX	   *cube_f8(double *);
NDBOX	   *cube_f8_f8(double *, double *);
NDBOX	   *cube_c_f8(NDBOX *, double *);
NDBOX	   *cube_c_f8_f8(NDBOX *, double *, double *);
int4		cube_dim(NDBOX * a);
double	   *cube_ll_coord(NDBOX * a, int4 n);
double	   *cube_ur_coord(NDBOX * a, int4 n);


/*
** GiST support methods
*/
bool		g_cube_consistent(GISTENTRY *entry, NDBOX * query, StrategyNumber strategy);
GISTENTRY  *g_cube_compress(GISTENTRY *entry);
GISTENTRY  *g_cube_decompress(GISTENTRY *entry);
float	   *g_cube_penalty(GISTENTRY *origentry, GISTENTRY *newentry, float *result);
GIST_SPLITVEC *g_cube_picksplit(bytea *entryvec, GIST_SPLITVEC *v);
bool		g_cube_leaf_consistent(NDBOX * key, NDBOX * query, StrategyNumber strategy);
bool		g_cube_internal_consistent(NDBOX * key, NDBOX * query, StrategyNumber strategy);
NDBOX	   *g_cube_union(bytea *entryvec, int *sizep);
NDBOX	   *g_cube_binary_union(NDBOX * r1, NDBOX * r2, int *sizep);
bool	   *g_cube_same(NDBOX * b1, NDBOX * b2, bool *result);

/*
** B-tree support functions
*/
bool		cube_eq(NDBOX * a, NDBOX * b);
bool		cube_ne(NDBOX * a, NDBOX * b);
bool		cube_lt(NDBOX * a, NDBOX * b);
bool		cube_gt(NDBOX * a, NDBOX * b);
bool		cube_le(NDBOX * a, NDBOX * b);
bool		cube_ge(NDBOX * a, NDBOX * b);
int32		cube_cmp(NDBOX * a, NDBOX * b);

/*
** R-tree support functions
*/
bool		cube_contains(NDBOX * a, NDBOX * b);
bool		cube_contained(NDBOX * a, NDBOX * b);
bool		cube_overlap(NDBOX * a, NDBOX * b);
NDBOX	   *cube_union(NDBOX * a, NDBOX * b);
NDBOX	   *cube_inter(NDBOX * a, NDBOX * b);
double	   *cube_size(NDBOX * a);
void		rt_cube_size(NDBOX * a, double *sz);

/*
** These make no sense for this type, but R-tree wants them
*/
bool		cube_over_left(NDBOX * a, NDBOX * b);
bool		cube_over_right(NDBOX * a, NDBOX * b);
bool		cube_left(NDBOX * a, NDBOX * b);
bool		cube_right(NDBOX * a, NDBOX * b);

/*
** miscellaneous
*/
bool		cube_lt(NDBOX * a, NDBOX * b);
bool		cube_gt(NDBOX * a, NDBOX * b);
double	   *cube_distance(NDBOX * a, NDBOX * b);
bool		cube_is_point(NDBOX * a);
NDBOX	   *cube_enlarge(NDBOX * a, double *r, int4 n);


/*
** Auxiliary funxtions
*/
static double distance_1D(double a1, double a2, double b1, double b2);


/*****************************************************************************
 * Input/Output functions
 *****************************************************************************/

/* NdBox = [(lowerleft),(upperright)] */
/* [(xLL(1)...xLL(N)),(xUR(1)...xUR(n))] */
NDBOX *
cube_in(char *str)
{
	void	   *result;

	cube_scanner_init(str);

	if (cube_yyparse(&result) != 0)
		cube_yyerror("bogus input");

	cube_scanner_finish();

	return ((NDBOX *) result);
}

/* Allow conversion from text to cube to allow input of computed strings */
/* There may be issues with toasted data here. I don't know enough to be sure.*/
NDBOX *
cube(text *str)
{
	return cube_in(DatumGetCString(DirectFunctionCall1(textout,
												 PointerGetDatum(str))));
}

char *
cube_out(NDBOX * cube)
{
	StringInfoData buf;
	bool		equal = true;
	int			dim = cube->dim;
	int			i;
	int			ndig;

	initStringInfo(&buf);

	/*
	 * Get the number of digits to display.
	 */
	ndig = DBL_DIG + extra_float_digits;
	if (ndig < 1)
		ndig = 1;

	/*
	 * while printing the first (LL) corner, check if it is equal to the
	 * second one
	 */
	appendStringInfoChar(&buf, '(');
	for (i = 0; i < dim; i++)
	{
		if (i > 0)
			appendStringInfo(&buf, ", ");
		appendStringInfo(&buf, "%.*g", ndig, cube->x[i]);
		if (cube->x[i] != cube->x[i + dim])
			equal = false;
	}
	appendStringInfoChar(&buf, ')');

	if (!equal)
	{
		appendStringInfo(&buf, ",(");
		for (i = 0; i < dim; i++)
		{
			if (i > 0)
				appendStringInfo(&buf, ", ");
			appendStringInfo(&buf, "%.*g", ndig, cube->x[i + dim]);
		}
		appendStringInfoChar(&buf, ')');
	}

	return buf.data;
}


/*****************************************************************************
 *						   GiST functions
 *****************************************************************************/

/*
** The GiST Consistent method for boxes
** Should return false if for all data items x below entry,
** the predicate x op query == FALSE, where op is the oper
** corresponding to strategy in the pg_amop table.
*/
bool
g_cube_consistent(GISTENTRY *entry,
				  NDBOX * query,
				  StrategyNumber strategy)
{
	/*
	 * if entry is not leaf, use g_cube_internal_consistent, else use
	 * g_cube_leaf_consistent
	 */
	if (GIST_LEAF(entry))
		return g_cube_leaf_consistent((NDBOX *) DatumGetPointer(entry->key),
									  query, strategy);
	else
		return g_cube_internal_consistent((NDBOX *) DatumGetPointer(entry->key),
										  query, strategy);
}


/*
** The GiST Union method for boxes
** returns the minimal bounding box that encloses all the entries in entryvec
*/
NDBOX *
g_cube_union(bytea *entryvec, int *sizep)
{
	int			numranges,
				i;
	NDBOX	   *out = (NDBOX *) NULL;
	NDBOX	   *tmp;

	/*
	 * fprintf(stderr, "union\n");
	 */
	numranges = (VARSIZE(entryvec) - VARHDRSZ) / sizeof(GISTENTRY);
	tmp = (NDBOX *) DatumGetPointer((((GISTENTRY *) (VARDATA(entryvec)))[0]).key);

	/*
	 * sizep = sizeof(NDBOX); -- NDBOX has variable size
	 */
	*sizep = tmp->size;

	for (i = 1; i < numranges; i++)
	{
		out = g_cube_binary_union(tmp, (NDBOX *)
		   DatumGetPointer((((GISTENTRY *) (VARDATA(entryvec)))[i]).key),
								  sizep);
		if (i > 1)
			pfree(tmp);
		tmp = out;
	}

	return (out);
}

/*
** GiST Compress and Decompress methods for boxes
** do not do anything.
*/
GISTENTRY *
g_cube_compress(GISTENTRY *entry)
{
	return (entry);
}

GISTENTRY *
g_cube_decompress(GISTENTRY *entry)
{
	return (entry);
}

/*
** The GiST Penalty method for boxes
** As in the R-tree paper, we use change in area as our penalty metric
*/
float *
g_cube_penalty(GISTENTRY *origentry, GISTENTRY *newentry, float *result)
{
	NDBOX	   *ud;
	double		tmp1,
				tmp2;

	ud = cube_union((NDBOX *) DatumGetPointer(origentry->key),
					(NDBOX *) DatumGetPointer(newentry->key));
	rt_cube_size(ud, &tmp1);
	rt_cube_size((NDBOX *) DatumGetPointer(origentry->key), &tmp2);
	*result = (float) (tmp1 - tmp2);
	pfree(ud);

	/*
	 * fprintf(stderr, "penalty\n"); fprintf(stderr, "\t%g\n", *result);
	 */
	return (result);
}



/*
** The GiST PickSplit method for boxes
** We use Guttman's poly time split algorithm
*/
GIST_SPLITVEC *
g_cube_picksplit(bytea *entryvec,
				 GIST_SPLITVEC *v)
{
	OffsetNumber i,
				j;
	NDBOX	   *datum_alpha,
			   *datum_beta;
	NDBOX	   *datum_l,
			   *datum_r;
	NDBOX	   *union_d,
			   *union_dl,
			   *union_dr;
	NDBOX	   *inter_d;
	bool		firsttime;
	double		size_alpha,
				size_beta,
				size_union,
				size_inter;
	double		size_waste,
				waste;
	double		size_l,
				size_r;
	int			nbytes;
	OffsetNumber seed_1 = 0,
				seed_2 = 0;
	OffsetNumber *left,
			   *right;
	OffsetNumber maxoff;

	/*
	 * fprintf(stderr, "picksplit\n");
	 */
	maxoff = ((VARSIZE(entryvec) - VARHDRSZ) / sizeof(GISTENTRY)) - 2;
	nbytes = (maxoff + 2) * sizeof(OffsetNumber);
	v->spl_left = (OffsetNumber *) palloc(nbytes);
	v->spl_right = (OffsetNumber *) palloc(nbytes);

	firsttime = true;
	waste = 0.0;

	for (i = FirstOffsetNumber; i < maxoff; i = OffsetNumberNext(i))
	{
		datum_alpha = (NDBOX *) DatumGetPointer(((GISTENTRY *) (VARDATA(entryvec)))[i].key);
		for (j = OffsetNumberNext(i); j <= maxoff; j = OffsetNumberNext(j))
		{
			datum_beta = (NDBOX *) DatumGetPointer(((GISTENTRY *) (VARDATA(entryvec)))[j].key);

			/* compute the wasted space by unioning these guys */
			/* size_waste = size_union - size_inter; */
			union_d = cube_union(datum_alpha, datum_beta);
			rt_cube_size(union_d, &size_union);
			inter_d = cube_inter(datum_alpha, datum_beta);
			rt_cube_size(inter_d, &size_inter);
			size_waste = size_union - size_inter;

			pfree(union_d);

			if (inter_d != (NDBOX *) NULL)
				pfree(inter_d);

			/*
			 * are these a more promising split than what we've already
			 * seen?
			 */

			if (size_waste > waste || firsttime)
			{
				waste = size_waste;
				seed_1 = i;
				seed_2 = j;
				firsttime = false;
			}
		}
	}

	left = v->spl_left;
	v->spl_nleft = 0;
	right = v->spl_right;
	v->spl_nright = 0;

	datum_alpha = (NDBOX *) DatumGetPointer(((GISTENTRY *) (VARDATA(entryvec)))[seed_1].key);
	datum_l = cube_union(datum_alpha, datum_alpha);
	rt_cube_size(datum_l, &size_l);
	datum_beta = (NDBOX *) DatumGetPointer(((GISTENTRY *) (VARDATA(entryvec)))[seed_2].key);
	datum_r = cube_union(datum_beta, datum_beta);
	rt_cube_size(datum_r, &size_r);

	/*
	 * Now split up the regions between the two seeds.	An important
	 * property of this split algorithm is that the split vector v has the
	 * indices of items to be split in order in its left and right
	 * vectors.  We exploit this property by doing a merge in the code
	 * that actually splits the page.
	 *
	 * For efficiency, we also place the new index tuple in this loop. This
	 * is handled at the very end, when we have placed all the existing
	 * tuples and i == maxoff + 1.
	 */

	maxoff = OffsetNumberNext(maxoff);
	for (i = FirstOffsetNumber; i <= maxoff; i = OffsetNumberNext(i))
	{
		/*
		 * If we've already decided where to place this item, just put it
		 * on the right list.  Otherwise, we need to figure out which page
		 * needs the least enlargement in order to store the item.
		 */

		if (i == seed_1)
		{
			*left++ = i;
			v->spl_nleft++;
			continue;
		}
		else if (i == seed_2)
		{
			*right++ = i;
			v->spl_nright++;
			continue;
		}

		/* okay, which page needs least enlargement? */
		datum_alpha = (NDBOX *) DatumGetPointer(((GISTENTRY *) (VARDATA(entryvec)))[i].key);
		union_dl = cube_union(datum_l, datum_alpha);
		union_dr = cube_union(datum_r, datum_alpha);
		rt_cube_size(union_dl, &size_alpha);
		rt_cube_size(union_dr, &size_beta);

		/* pick which page to add it to */
		if (size_alpha - size_l < size_beta - size_r)
		{
			pfree(datum_l);
			pfree(union_dr);
			datum_l = union_dl;
			size_l = size_alpha;
			*left++ = i;
			v->spl_nleft++;
		}
		else
		{
			pfree(datum_r);
			pfree(union_dl);
			datum_r = union_dr;
			size_r = size_alpha;
			*right++ = i;
			v->spl_nright++;
		}
	}
	*left = *right = FirstOffsetNumber; /* sentinel value, see dosplit() */

	v->spl_ldatum = PointerGetDatum(datum_l);
	v->spl_rdatum = PointerGetDatum(datum_r);

	return v;
}

/*
** Equality method
*/
bool *
g_cube_same(NDBOX * b1, NDBOX * b2, bool *result)
{
	if (cube_eq(b1, b2))
		*result = TRUE;
	else
		*result = FALSE;

	/*
	 * fprintf(stderr, "same: %s\n", (*result ? "TRUE" : "FALSE" ));
	 */
	return (result);
}

/*
** SUPPORT ROUTINES
*/
bool
g_cube_leaf_consistent(NDBOX * key,
					   NDBOX * query,
					   StrategyNumber strategy)
{
	bool		retval;

	/*
	 * fprintf(stderr, "leaf_consistent, %d\n", strategy);
	 */
	switch (strategy)
	{
		case RTLeftStrategyNumber:
			retval = (bool) cube_left(key, query);
			break;
		case RTOverLeftStrategyNumber:
			retval = (bool) cube_over_left(key, query);
			break;
		case RTOverlapStrategyNumber:
			retval = (bool) cube_overlap(key, query);
			break;
		case RTOverRightStrategyNumber:
			retval = (bool) cube_over_right(key, query);
			break;
		case RTRightStrategyNumber:
			retval = (bool) cube_right(key, query);
			break;
		case RTSameStrategyNumber:
			retval = (bool) cube_eq(key, query);
			break;
		case RTContainsStrategyNumber:
			retval = (bool) cube_contains(key, query);
			break;
		case RTContainedByStrategyNumber:
			retval = (bool) cube_contained(key, query);
			break;
		default:
			retval = FALSE;
	}
	return (retval);
}

bool
g_cube_internal_consistent(NDBOX * key,
						   NDBOX * query,
						   StrategyNumber strategy)
{
	bool		retval;

	/*
	 * fprintf(stderr, "internal_consistent, %d\n", strategy);
	 */
	switch (strategy)
	{
		case RTLeftStrategyNumber:
		case RTOverLeftStrategyNumber:
			retval = (bool) cube_over_left(key, query);
			break;
		case RTOverlapStrategyNumber:
			retval = (bool) cube_overlap(key, query);
			break;
		case RTOverRightStrategyNumber:
		case RTRightStrategyNumber:
			retval = (bool) cube_right(key, query);
			break;
		case RTSameStrategyNumber:
		case RTContainsStrategyNumber:
			retval = (bool) cube_contains(key, query);
			break;
		case RTContainedByStrategyNumber:
			retval = (bool) cube_overlap(key, query);
			break;
		default:
			retval = FALSE;
	}
	return (retval);
}

NDBOX *
g_cube_binary_union(NDBOX * r1, NDBOX * r2, int *sizep)
{
	NDBOX	   *retval;

	retval = cube_union(r1, r2);
	*sizep = retval->size;

	return (retval);
}


/* cube_union */
NDBOX *
cube_union(NDBOX * a, NDBOX * b)
{
	int			i;
	NDBOX	   *result;

	if (a->dim >= b->dim)
	{
		result = palloc(a->size);
		memset(result, 0, a->size);
		result->size = a->size;
		result->dim = a->dim;
	}
	else
	{
		result = palloc(b->size);
		memset(result, 0, b->size);
		result->size = b->size;
		result->dim = b->dim;
	}

	/* swap the box pointers if needed */
	if (a->dim < b->dim)
	{
		NDBOX	   *tmp = b;

		b = a;
		a = tmp;
	}

	/*
	 * use the potentially smaller of the two boxes (b) to fill in the
	 * result, padding absent dimensions with zeroes
	 */
	for (i = 0; i < b->dim; i++)
	{
		result->x[i] = min(b->x[i], b->x[i + b->dim]);
		result->x[i + a->dim] = max(b->x[i], b->x[i + b->dim]);
	}
	for (i = b->dim; i < a->dim; i++)
	{
		result->x[i] = 0;
		result->x[i + a->dim] = 0;
	}

	/* compute the union */
	for (i = 0; i < a->dim; i++)
	{
		result->x[i] =
			min(min(a->x[i], a->x[i + a->dim]), result->x[i]);
		result->x[i + a->dim] = max(max(a->x[i],
							   a->x[i + a->dim]), result->x[i + a->dim]);
	}

	return (result);
}

/* cube_inter */
NDBOX *
cube_inter(NDBOX * a, NDBOX * b)
{
	int			i;
	NDBOX	   *result;

	if (a->dim >= b->dim)
	{
		result = palloc(a->size);
		memset(result, 0, a->size);
		result->size = a->size;
		result->dim = a->dim;
	}
	else
	{
		result = palloc(b->size);
		memset(result, 0, b->size);
		result->size = b->size;
		result->dim = b->dim;
	}

	/* swap the box pointers if needed */
	if (a->dim < b->dim)
	{
		NDBOX	   *tmp = b;

		b = a;
		a = tmp;
	}

	/*
	 * use the potentially	smaller of the two boxes (b) to fill in the
	 * result, padding absent dimensions with zeroes
	 */
	for (i = 0; i < b->dim; i++)
	{
		result->x[i] = min(b->x[i], b->x[i + b->dim]);
		result->x[i + a->dim] = max(b->x[i], b->x[i + b->dim]);
	}
	for (i = b->dim; i < a->dim; i++)
	{
		result->x[i] = 0;
		result->x[i + a->dim] = 0;
	}

	/* compute the intersection */
	for (i = 0; i < a->dim; i++)
	{
		result->x[i] =
			max(min(a->x[i], a->x[i + a->dim]), result->x[i]);
		result->x[i + a->dim] = min(max(a->x[i],
							   a->x[i + a->dim]), result->x[i + a->dim]);
	}

	/*
	 * Is it OK to return a non-null intersection for non-overlapping
	 * boxes?
	 */
	return (result);
}

/* cube_size */
double *
cube_size(NDBOX * a)
{
	int			i,
				j;
	double	   *result;

	result = (double *) palloc(sizeof(double));

	*result = 1.0;
	for (i = 0, j = a->dim; i < a->dim; i++, j++)
		*result = (*result) * abs((a->x[j] - a->x[i]));

	return (result);
}

void
rt_cube_size(NDBOX * a, double *size)
{
	int			i,
				j;

	if (a == (NDBOX *) NULL)
		*size = 0.0;
	else
	{
		*size = 1.0;
		for (i = 0, j = a->dim; i < a->dim; i++, j++)
			*size = (*size) * abs((a->x[j] - a->x[i]));
	}
	return;
}

/* The following four methods compare the projections of the boxes
   onto the 0-th coordinate axis. These methods are useless for dimensions
   larger than 2, but it seems that R-tree requires all its strategies
   map to real functions that return something */

/*	is the right edge of (a) located to the left of
	the right edge of (b)? */
bool
cube_over_left(NDBOX * a, NDBOX * b)
{
	if ((a == NULL) || (b == NULL))
		return (FALSE);

	return (min(a->x[a->dim - 1], a->x[2 * a->dim - 1]) <=
			min(b->x[b->dim - 1], b->x[2 * b->dim - 1]) &&
			!cube_left(a, b) && !cube_right(a, b));
}

/*	is the left edge of (a) located to the right of
	the left edge of (b)? */
bool
cube_over_right(NDBOX * a, NDBOX * b)
{
	if ((a == NULL) || (b == NULL))
		return (FALSE);

	return (min(a->x[a->dim - 1], a->x[2 * a->dim - 1]) >=
			min(b->x[b->dim - 1], b->x[2 * b->dim - 1]) &&
			!cube_left(a, b) && !cube_right(a, b));
}


/* return 'true' if the projection of 'a' is
   entirely on the left of the projection of 'b' */
bool
cube_left(NDBOX * a, NDBOX * b)
{
	if ((a == NULL) || (b == NULL))
		return (FALSE);

	return (min(a->x[a->dim - 1], a->x[2 * a->dim - 1]) <
			min(b->x[0], b->x[b->dim]));
}

/* return 'true' if the projection of 'a' is
   entirely on the right  of the projection of 'b' */
bool
cube_right(NDBOX * a, NDBOX * b)
{
	if ((a == NULL) || (b == NULL))
		return (FALSE);

	return (min(a->x[0], a->x[a->dim]) >
			min(b->x[b->dim - 1], b->x[2 * b->dim - 1]));
}

/* make up a metric in which one box will be 'lower' than the other
   -- this can be useful for sorting and to determine uniqueness */
int32
cube_cmp(NDBOX * a, NDBOX * b)
{
	int			i;
	int			dim;

	dim = min(a->dim, b->dim);

	/* compare the common dimensions */
	for (i = 0; i < dim; i++)
	{
		if (min(a->x[i], a->x[a->dim + i]) >
			min(b->x[i], b->x[b->dim + i]))
			return 1;
		if (min(a->x[i], a->x[a->dim + i]) <
			min(b->x[i], b->x[b->dim + i]))
			return -1;
	}
	for (i = 0; i < dim; i++)
	{
		if (max(a->x[i], a->x[a->dim + i]) >
			max(b->x[i], b->x[b->dim + i]))
			return 1;
		if (max(a->x[i], a->x[a->dim + i]) <
			max(b->x[i], b->x[b->dim + i]))
			return -1;
	}

	/* compare extra dimensions to zero */
	if (a->dim > b->dim)
	{
		for (i = dim; i < a->dim; i++)
		{
			if (min(a->x[i], a->x[a->dim + i]) > 0)
				return 1;
			if (min(a->x[i], a->x[a->dim + i]) < 0)
				return -1;
		}
		for (i = dim; i < a->dim; i++)
		{
			if (max(a->x[i], a->x[a->dim + i]) > 0)
				return 1;
			if (max(a->x[i], a->x[a->dim + i]) < 0)
				return -1;
		}

		/*
		 * if all common dimensions are equal, the cube with more
		 * dimensions wins
		 */
		return 1;
	}
	if (a->dim < b->dim)
	{
		for (i = dim; i < b->dim; i++)
		{
			if (min(b->x[i], b->x[b->dim + i]) > 0)
				return -1;
			if (min(b->x[i], b->x[b->dim + i]) < 0)
				return 1;
		}
		for (i = dim; i < b->dim; i++)
		{
			if (max(b->x[i], b->x[b->dim + i]) > 0)
				return -1;
			if (max(b->x[i], b->x[b->dim + i]) < 0)
				return 1;
		}

		/*
		 * if all common dimensions are equal, the cube with more
		 * dimensions wins
		 */
		return -1;
	}

	/* They're really equal */
	return 0;
}


bool
cube_eq(NDBOX * a, NDBOX * b)
{
	return (cube_cmp(a, b) == 0);
}

bool
cube_ne(NDBOX * a, NDBOX * b)
{
	return (cube_cmp(a, b) != 0);
}

bool
cube_lt(NDBOX * a, NDBOX * b)
{
	return (cube_cmp(a, b) < 0);
}

bool
cube_gt(NDBOX * a, NDBOX * b)
{
	return (cube_cmp(a, b) > 0);
}

bool
cube_le(NDBOX * a, NDBOX * b)
{
	return (cube_cmp(a, b) <= 0);
}

bool
cube_ge(NDBOX * a, NDBOX * b)
{
	return (cube_cmp(a, b) >= 0);
}


/* Contains */
/* Box(A) CONTAINS Box(B) IFF pt(A) < pt(B) */
bool
cube_contains(NDBOX * a, NDBOX * b)
{
	int			i;

	if ((a == NULL) || (b == NULL))
		return (FALSE);

	if (a->dim < b->dim)
	{
		/*
		 * the further comparisons will make sense if the excess
		 * dimensions of (b) were zeroes Since both UL and UR coordinates
		 * must be zero, we can check them all without worrying about
		 * which is which.
		 */
		for (i = a->dim; i < b->dim; i++)
		{
			if (b->x[i] != 0)
				return (FALSE);
			if (b->x[i + b->dim] != 0)
				return (FALSE);
		}
	}

	/* Can't care less about the excess dimensions of (a), if any */
	for (i = 0; i < min(a->dim, b->dim); i++)
	{
		if (min(a->x[i], a->x[a->dim + i]) >
			min(b->x[i], b->x[b->dim + i]))
			return (FALSE);
		if (max(a->x[i], a->x[a->dim + i]) <
			max(b->x[i], b->x[b->dim + i]))
			return (FALSE);
	}

	return (TRUE);
}

/* Contained */
/* Box(A) Contained by Box(B) IFF Box(B) Contains Box(A) */
bool
cube_contained(NDBOX * a, NDBOX * b)
{
	if (cube_contains(b, a) == TRUE)
		return (TRUE);
	else
		return (FALSE);
}

/* Overlap */
/* Box(A) Overlap Box(B) IFF (pt(a)LL < pt(B)UR) && (pt(b)LL < pt(a)UR) */
bool
cube_overlap(NDBOX * a, NDBOX * b)
{
	int			i;

	/*
	 * This *very bad* error was found in the source: if ( (a==NULL) ||
	 * (b=NULL) ) return(FALSE);
	 */
	if ((a == NULL) || (b == NULL))
		return (FALSE);

	/* swap the box pointers if needed */
	if (a->dim < b->dim)
	{
		NDBOX	   *tmp = b;

		b = a;
		a = tmp;
	}

	/* compare within the dimensions of (b) */
	for (i = 0; i < b->dim; i++)
	{
		if (min(a->x[i], a->x[a->dim + i]) >
			max(b->x[i], b->x[b->dim + i]))
			return (FALSE);
		if (max(a->x[i], a->x[a->dim + i]) <
			min(b->x[i], b->x[b->dim + i]))
			return (FALSE);
	}

	/* compare to zero those dimensions in (a) absent in (b) */
	for (i = b->dim; i < a->dim; i++)
	{
		if (min(a->x[i], a->x[a->dim + i]) > 0)
			return (FALSE);
		if (max(a->x[i], a->x[a->dim + i]) < 0)
			return (FALSE);
	}

	return (TRUE);
}


/* Distance */
/* The distance is computed as a per axis sum of the squared distances
   between 1D projections of the boxes onto Cartesian axes. Assuming zero
   distance between overlapping projections, this metric coincides with the
   "common sense" geometric distance */
double *
cube_distance(NDBOX * a, NDBOX * b)
{
	int			i;
	double		d,
				distance;
	double	   *result;

	result = (double *) palloc(sizeof(double));

	/* swap the box pointers if needed */
	if (a->dim < b->dim)
	{
		NDBOX	   *tmp = b;

		b = a;
		a = tmp;
	}

	distance = 0.0;
	/* compute within the dimensions of (b) */
	for (i = 0; i < b->dim; i++)
	{
		d = distance_1D(a->x[i], a->x[i + a->dim], b->x[i], b->x[i + b->dim]);
		distance += d * d;
	}

	/* compute distance to zero for those dimensions in (a) absent in (b) */
	for (i = b->dim; i < a->dim; i++)
	{
		d = distance_1D(a->x[i], a->x[i + a->dim], 0.0, 0.0);
		distance += d * d;
	}

	*result = (double) sqrt(distance);

	return (result);
}

static double
distance_1D(double a1, double a2, double b1, double b2)
{
	/* interval (a) is entirely on the left of (b) */
	if ((a1 <= b1) && (a2 <= b1) && (a1 <= b2) && (a2 <= b2))
		return (min(b1, b2) - max(a1, a2));

	/* interval (a) is entirely on the right of (b) */
	if ((a1 > b1) && (a2 > b1) && (a1 > b2) && (a2 > b2))
		return (min(a1, a2) - max(b1, b2));

	/* the rest are all sorts of intersections */
	return (0.0);
}

/* Test if a box is also a point */
bool
cube_is_point(NDBOX * a)
{
	int			i,
				j;

	for (i = 0, j = a->dim; i < a->dim; i++, j++)
	{
		if (a->x[i] != a->x[j])
			return FALSE;
	}

	return TRUE;
}

/* Return dimensions in use in the data structure */
int4
cube_dim(NDBOX * a)
{
	/* Other things will break before unsigned int doesn't fit. */
	return a->dim;
}

/* Return a specific normalized LL coordinate */
double *
cube_ll_coord(NDBOX * a, int4 n)
{
	double	   *result;

	result = (double *) palloc(sizeof(double));
	*result = 0;
	if (a->dim >= n && n > 0)
		*result = min(a->x[n - 1], a->x[a->dim + n - 1]);
	return result;
}

/* Return a specific normalized UR coordinate */
double *
cube_ur_coord(NDBOX * a, int4 n)
{
	double	   *result;

	result = (double *) palloc(sizeof(double));
	*result = 0;
	if (a->dim >= n && n > 0)
		*result = max(a->x[n - 1], a->x[a->dim + n - 1]);
	return result;
}

/* Increase or decrease box size by a radius in at least n dimensions. */
NDBOX *
cube_enlarge(NDBOX * a, double *r, int4 n)
{
	NDBOX	   *result;
	int			dim = 0;
	int			size;
	int			i,
				j,
				k;

	if (n > CUBE_MAX_DIM)
		n = CUBE_MAX_DIM;
	if (*r > 0 && n > 0)
		dim = n;
	if (a->dim > dim)
		dim = a->dim;
	size = offsetof(NDBOX, x[0]) + sizeof(double) * dim * 2;
	result = (NDBOX *) palloc(size);
	memset(result, 0, size);
	result->size = size;
	result->dim = dim;
	for (i = 0, j = dim, k = a->dim; i < a->dim; i++, j++, k++)
	{
		if (a->x[i] >= a->x[k])
		{
			result->x[i] = a->x[k] - *r;
			result->x[j] = a->x[i] + *r;
		}
		else
		{
			result->x[i] = a->x[i] - *r;
			result->x[j] = a->x[k] + *r;
		}
		if (result->x[i] > result->x[j])
		{
			result->x[i] = (result->x[i] + result->x[j]) / 2;
			result->x[j] = result->x[i];
		}
	}
	/* dim > a->dim only if r > 0 */
	for (; i < dim; i++, j++)
	{
		result->x[i] = -*r;
		result->x[j] = *r;
	}
	return result;
}

/* Create a one dimensional box with identical upper and lower coordinates */
NDBOX *
cube_f8(double *x1)
{
	NDBOX	   *result;
	int			size;

	size = offsetof(NDBOX, x[0]) + sizeof(double) * 2;
	result = (NDBOX *) palloc(size);
	memset(result, 0, size);
	result->size = size;
	result->dim = 1;
	result->x[0] = *x1;
	result->x[1] = *x1;
	return result;
}

/* Create a one dimensional box */
NDBOX *
cube_f8_f8(double *x1, double *x2)
{
	NDBOX	   *result;
	int			size;

	size = offsetof(NDBOX, x[0]) + sizeof(double) * 2;
	result = (NDBOX *) palloc(size);
	memset(result, 0, size);
	result->size = size;
	result->dim = 1;
	result->x[0] = *x1;
	result->x[1] = *x2;
	return result;
}

/* Add a dimension to an existing cube with the same values for the new
   coordinate */
NDBOX *
cube_c_f8(NDBOX * c, double *x1)
{
	NDBOX	   *result;
	int			size;
	int			i;

	size = offsetof(NDBOX, x[0]) + sizeof(double) * (c->dim + 1) *2;
	result = (NDBOX *) palloc(size);
	memset(result, 0, size);
	result->size = size;
	result->dim = c->dim + 1;
	for (i = 0; i < c->dim; i++)
	{
		result->x[i] = c->x[i];
		result->x[result->dim + i] = c->x[c->dim + i];
	}
	result->x[result->dim - 1] = *x1;
	result->x[2 * result->dim - 1] = *x1;
	return result;
}

/* Add a dimension to an existing cube */
NDBOX *
cube_c_f8_f8(NDBOX * c, double *x1, double *x2)
{
	NDBOX	   *result;
	int			size;
	int			i;

	size = offsetof(NDBOX, x[0]) + sizeof(double) * (c->dim + 1) *2;
	result = (NDBOX *) palloc(size);
	memset(result, 0, size);
	result->size = size;
	result->dim = c->dim + 1;
	for (i = 0; i < c->dim; i++)
	{
		result->x[i] = c->x[i];
		result->x[result->dim + i] = c->x[c->dim + i];
	}
	result->x[result->dim - 1] = *x1;
	result->x[2 * result->dim - 1] = *x2;
	return result;
}
