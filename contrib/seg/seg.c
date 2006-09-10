/******************************************************************************
  This file contains routines that can be bound to a Postgres backend and
  called by the backend in the process of processing queries.  The calling
  format for these routines is dictated by Postgres architecture.
******************************************************************************/

#include "postgres.h"

#include <float.h>

#include "access/gist.h"
#include "access/skey.h"
#include "utils/builtins.h"

#include "segdata.h"

/*
#define GIST_DEBUG
#define GIST_QUERY_DEBUG
*/

PG_MODULE_MAGIC;

extern int	seg_yyparse();
extern void seg_yyerror(const char *message);
extern void seg_scanner_init(const char *str);
extern void seg_scanner_finish(void);

/*
extern int	 seg_yydebug;
*/

/*
** Input/Output routines
*/
SEG		   *seg_in(char *str);
char	   *seg_out(SEG * seg);
float32		seg_lower(SEG * seg);
float32		seg_upper(SEG * seg);
float32		seg_center(SEG * seg);

/*
** GiST support methods
*/
bool		gseg_consistent(GISTENTRY *entry, SEG * query, StrategyNumber strategy);
GISTENTRY  *gseg_compress(GISTENTRY *entry);
GISTENTRY  *gseg_decompress(GISTENTRY *entry);
float	   *gseg_penalty(GISTENTRY *origentry, GISTENTRY *newentry, float *result);
GIST_SPLITVEC *gseg_picksplit(GistEntryVector *entryvec, GIST_SPLITVEC *v);
bool		gseg_leaf_consistent(SEG * key, SEG * query, StrategyNumber strategy);
bool		gseg_internal_consistent(SEG * key, SEG * query, StrategyNumber strategy);
SEG		   *gseg_union(GistEntryVector *entryvec, int *sizep);
SEG		   *gseg_binary_union(SEG * r1, SEG * r2, int *sizep);
bool	   *gseg_same(SEG * b1, SEG * b2, bool *result);


/*
** R-tree support functions
*/
bool		seg_same(SEG * a, SEG * b);
bool		seg_contains_int(SEG * a, int *b);
bool		seg_contains_float4(SEG * a, float4 *b);
bool		seg_contains_float8(SEG * a, float8 *b);
bool		seg_contains(SEG * a, SEG * b);
bool		seg_contained(SEG * a, SEG * b);
bool		seg_overlap(SEG * a, SEG * b);
bool		seg_left(SEG * a, SEG * b);
bool		seg_over_left(SEG * a, SEG * b);
bool		seg_right(SEG * a, SEG * b);
bool		seg_over_right(SEG * a, SEG * b);
SEG		   *seg_union(SEG * a, SEG * b);
SEG		   *seg_inter(SEG * a, SEG * b);
void		rt_seg_size(SEG * a, float *sz);
float	   *seg_size(SEG * a);

/*
** Various operators
*/
int32		seg_cmp(SEG * a, SEG * b);
bool		seg_lt(SEG * a, SEG * b);
bool		seg_le(SEG * a, SEG * b);
bool		seg_gt(SEG * a, SEG * b);
bool		seg_ge(SEG * a, SEG * b);
bool		seg_different(SEG * a, SEG * b);

/*
** Auxiliary funxtions
*/
static int	restore(char *s, float val, int n);
int			significant_digits(char *s);


/*****************************************************************************
 * Input/Output functions
 *****************************************************************************/

SEG *
seg_in(char *str)
{
	SEG		   *result = palloc(sizeof(SEG));

	seg_scanner_init(str);

	if (seg_yyparse(result) != 0)
		seg_yyerror("bogus input");

	seg_scanner_finish();

	return (result);
}

char *
seg_out(SEG * seg)
{
	char	   *result;
	char	   *p;

	if (seg == NULL)
		return (NULL);

	p = result = (char *) palloc(40);

	if (seg->l_ext == '>' || seg->l_ext == '<' || seg->l_ext == '~')
		p += sprintf(p, "%c", seg->l_ext);

	if (seg->lower == seg->upper && seg->l_ext == seg->u_ext)
	{
		/*
		 * indicates that this interval was built by seg_in off a single point
		 */
		p += restore(p, seg->lower, seg->l_sigd);
	}
	else
	{
		if (seg->l_ext != '-')
		{
			/* print the lower boudary if exists */
			p += restore(p, seg->lower, seg->l_sigd);
			p += sprintf(p, " ");
		}
		p += sprintf(p, "..");
		if (seg->u_ext != '-')
		{
			/* print the upper boudary if exists */
			p += sprintf(p, " ");
			if (seg->u_ext == '>' || seg->u_ext == '<' || seg->l_ext == '~')
				p += sprintf(p, "%c", seg->u_ext);
			p += restore(p, seg->upper, seg->u_sigd);
		}
	}

	return (result);
}

float32
seg_center(SEG * seg)
{
	float32		result = (float32) palloc(sizeof(float32data));

	if (!seg)
		return (float32) NULL;

	*result = ((float) seg->lower + (float) seg->upper) / 2.0;
	return (result);
}

float32
seg_lower(SEG * seg)
{
	float32		result = (float32) palloc(sizeof(float32data));

	if (!seg)
		return (float32) NULL;

	*result = (float) seg->lower;
	return (result);
}

float32
seg_upper(SEG * seg)
{
	float32		result = (float32) palloc(sizeof(float32data));

	if (!seg)
		return (float32) NULL;

	*result = (float) seg->upper;
	return (result);
}


/*****************************************************************************
 *						   GiST functions
 *****************************************************************************/

/*
** The GiST Consistent method for segments
** Should return false if for all data items x below entry,
** the predicate x op query == FALSE, where op is the oper
** corresponding to strategy in the pg_amop table.
*/
bool
gseg_consistent(GISTENTRY *entry,
				SEG * query,
				StrategyNumber strategy)
{
	/*
	 * if entry is not leaf, use gseg_internal_consistent, else use
	 * gseg_leaf_consistent
	 */
	if (GIST_LEAF(entry))
		return (gseg_leaf_consistent((SEG *) DatumGetPointer(entry->key), query, strategy));
	else
		return (gseg_internal_consistent((SEG *) DatumGetPointer(entry->key), query, strategy));
}

/*
** The GiST Union method for segments
** returns the minimal bounding seg that encloses all the entries in entryvec
*/
SEG *
gseg_union(GistEntryVector *entryvec, int *sizep)
{
	int			numranges,
				i;
	SEG		   *out = (SEG *) NULL;
	SEG		   *tmp;

#ifdef GIST_DEBUG
	fprintf(stderr, "union\n");
#endif

	numranges = entryvec->n;
	tmp = (SEG *) DatumGetPointer(entryvec->vector[0].key);
	*sizep = sizeof(SEG);

	for (i = 1; i < numranges; i++)
	{
		out = gseg_binary_union(tmp, (SEG *)
								DatumGetPointer(entryvec->vector[i].key),
								sizep);
		tmp = out;
	}

	return (out);
}

/*
** GiST Compress and Decompress methods for segments
** do not do anything.
*/
GISTENTRY *
gseg_compress(GISTENTRY *entry)
{
	return (entry);
}

GISTENTRY *
gseg_decompress(GISTENTRY *entry)
{
	return (entry);
}

/*
** The GiST Penalty method for segments
** As in the R-tree paper, we use change in area as our penalty metric
*/
float *
gseg_penalty(GISTENTRY *origentry, GISTENTRY *newentry, float *result)
{
	SEG		   *ud;
	float		tmp1,
				tmp2;

	ud = seg_union((SEG *) DatumGetPointer(origentry->key),
				   (SEG *) DatumGetPointer(newentry->key));
	rt_seg_size(ud, &tmp1);
	rt_seg_size((SEG *) DatumGetPointer(origentry->key), &tmp2);
	*result = tmp1 - tmp2;

#ifdef GIST_DEBUG
	fprintf(stderr, "penalty\n");
	fprintf(stderr, "\t%g\n", *result);
#endif

	return (result);
}



/*
** The GiST PickSplit method for segments
** We use Guttman's poly time split algorithm
*/
GIST_SPLITVEC *
gseg_picksplit(GistEntryVector *entryvec,
			   GIST_SPLITVEC *v)
{
	OffsetNumber i,
				j;
	SEG		   *datum_alpha,
			   *datum_beta;
	SEG		   *datum_l,
			   *datum_r;
	SEG		   *union_d,
			   *union_dl,
			   *union_dr;
	SEG		   *inter_d;
	bool		firsttime;
	float		size_alpha,
				size_beta,
				size_union,
				size_inter;
	float		size_waste,
				waste;
	float		size_l,
				size_r;
	int			nbytes;
	OffsetNumber seed_1 = 1,
				seed_2 = 2;
	OffsetNumber *left,
			   *right;
	OffsetNumber maxoff;

#ifdef GIST_DEBUG
	fprintf(stderr, "picksplit\n");
#endif

	maxoff = entryvec->n - 2;
	nbytes = (maxoff + 2) * sizeof(OffsetNumber);
	v->spl_left = (OffsetNumber *) palloc(nbytes);
	v->spl_right = (OffsetNumber *) palloc(nbytes);

	firsttime = true;
	waste = 0.0;

	for (i = FirstOffsetNumber; i < maxoff; i = OffsetNumberNext(i))
	{
		datum_alpha = (SEG *) DatumGetPointer(entryvec->vector[i].key);
		for (j = OffsetNumberNext(i); j <= maxoff; j = OffsetNumberNext(j))
		{
			datum_beta = (SEG *) DatumGetPointer(entryvec->vector[j].key);

			/* compute the wasted space by unioning these guys */
			/* size_waste = size_union - size_inter; */
			union_d = seg_union(datum_alpha, datum_beta);
			rt_seg_size(union_d, &size_union);
			inter_d = seg_inter(datum_alpha, datum_beta);
			rt_seg_size(inter_d, &size_inter);
			size_waste = size_union - size_inter;

			/*
			 * are these a more promising split that what we've already seen?
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

	datum_alpha = (SEG *) DatumGetPointer(entryvec->vector[seed_1].key);
	datum_l = seg_union(datum_alpha, datum_alpha);
	rt_seg_size(datum_l, &size_l);
	datum_beta = (SEG *) DatumGetPointer(entryvec->vector[seed_2].key);
	datum_r = seg_union(datum_beta, datum_beta);
	rt_seg_size(datum_r, &size_r);

	/*
	 * Now split up the regions between the two seeds.	An important property
	 * of this split algorithm is that the split vector v has the indices of
	 * items to be split in order in its left and right vectors.  We exploit
	 * this property by doing a merge in the code that actually splits the
	 * page.
	 *
	 * For efficiency, we also place the new index tuple in this loop. This is
	 * handled at the very end, when we have placed all the existing tuples
	 * and i == maxoff + 1.
	 */

	maxoff = OffsetNumberNext(maxoff);
	for (i = FirstOffsetNumber; i <= maxoff; i = OffsetNumberNext(i))
	{
		/*
		 * If we've already decided where to place this item, just put it on
		 * the right list.	Otherwise, we need to figure out which page needs
		 * the least enlargement in order to store the item.
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
		datum_alpha = (SEG *) DatumGetPointer(entryvec->vector[i].key);
		union_dl = seg_union(datum_l, datum_alpha);
		union_dr = seg_union(datum_r, datum_alpha);
		rt_seg_size(union_dl, &size_alpha);
		rt_seg_size(union_dr, &size_beta);

		/* pick which page to add it to */
		if (size_alpha - size_l < size_beta - size_r)
		{
			datum_l = union_dl;
			size_l = size_alpha;
			*left++ = i;
			v->spl_nleft++;
		}
		else
		{
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
** Equality methods
*/
bool *
gseg_same(SEG * b1, SEG * b2, bool *result)
{
	if (seg_same(b1, b2))
		*result = TRUE;
	else
		*result = FALSE;

#ifdef GIST_DEBUG
	fprintf(stderr, "same: %s\n", (*result ? "TRUE" : "FALSE"));
#endif

	return (result);
}

/*
** SUPPORT ROUTINES
*/
bool
gseg_leaf_consistent(SEG * key,
					 SEG * query,
					 StrategyNumber strategy)
{
	bool		retval;

#ifdef GIST_QUERY_DEBUG
	fprintf(stderr, "leaf_consistent, %d\n", strategy);
#endif

	switch (strategy)
	{
		case RTLeftStrategyNumber:
			retval = (bool) seg_left(key, query);
			break;
		case RTOverLeftStrategyNumber:
			retval = (bool) seg_over_left(key, query);
			break;
		case RTOverlapStrategyNumber:
			retval = (bool) seg_overlap(key, query);
			break;
		case RTOverRightStrategyNumber:
			retval = (bool) seg_over_right(key, query);
			break;
		case RTRightStrategyNumber:
			retval = (bool) seg_right(key, query);
			break;
		case RTSameStrategyNumber:
			retval = (bool) seg_same(key, query);
			break;
		case RTContainsStrategyNumber:
		case RTOldContainsStrategyNumber:
			retval = (bool) seg_contains(key, query);
			break;
		case RTContainedByStrategyNumber:
		case RTOldContainedByStrategyNumber:
			retval = (bool) seg_contained(key, query);
			break;
		default:
			retval = FALSE;
	}
	return (retval);
}

bool
gseg_internal_consistent(SEG * key,
						 SEG * query,
						 StrategyNumber strategy)
{
	bool		retval;

#ifdef GIST_QUERY_DEBUG
	fprintf(stderr, "internal_consistent, %d\n", strategy);
#endif

	switch (strategy)
	{
		case RTLeftStrategyNumber:
			retval = (bool) !seg_over_right(key, query);
			break;
		case RTOverLeftStrategyNumber:
			retval = (bool) !seg_right(key, query);
			break;
		case RTOverlapStrategyNumber:
			retval = (bool) seg_overlap(key, query);
			break;
		case RTOverRightStrategyNumber:
			retval = (bool) !seg_left(key, query);
			break;
		case RTRightStrategyNumber:
			retval = (bool) !seg_over_left(key, query);
			break;
		case RTSameStrategyNumber:
		case RTContainsStrategyNumber:
		case RTOldContainsStrategyNumber:
			retval = (bool) seg_contains(key, query);
			break;
		case RTContainedByStrategyNumber:
		case RTOldContainedByStrategyNumber:
			retval = (bool) seg_overlap(key, query);
			break;
		default:
			retval = FALSE;
	}
	return (retval);
}

SEG *
gseg_binary_union(SEG * r1, SEG * r2, int *sizep)
{
	SEG		   *retval;

	retval = seg_union(r1, r2);
	*sizep = sizeof(SEG);

	return (retval);
}


bool
seg_contains(SEG * a, SEG * b)
{
	return ((a->lower <= b->lower) && (a->upper >= b->upper));
}

bool
seg_contained(SEG * a, SEG * b)
{
	return (seg_contains(b, a));
}

/*****************************************************************************
 * Operator class for R-tree indexing
 *****************************************************************************/

bool
seg_same(SEG * a, SEG * b)
{
	return seg_cmp(a, b) == 0;
}

/*	seg_overlap -- does a overlap b?
 */
bool
seg_overlap(SEG * a, SEG * b)
{
	return (
			((a->upper >= b->upper) && (a->lower <= b->upper))
			||
			((b->upper >= a->upper) && (b->lower <= a->upper))
		);
}

/*	seg_overleft -- is the right edge of (a) located at or left of the right edge of (b)?
 */
bool
seg_over_left(SEG * a, SEG * b)
{
	return (a->upper <= b->upper);
}

/*	seg_left -- is (a) entirely on the left of (b)?
 */
bool
seg_left(SEG * a, SEG * b)
{
	return (a->upper < b->lower);
}

/*	seg_right -- is (a) entirely on the right of (b)?
 */
bool
seg_right(SEG * a, SEG * b)
{
	return (a->lower > b->upper);
}

/*	seg_overright -- is the left edge of (a) located at or right of the left edge of (b)?
 */
bool
seg_over_right(SEG * a, SEG * b)
{
	return (a->lower >= b->lower);
}


SEG *
seg_union(SEG * a, SEG * b)
{
	SEG		   *n;

	n = (SEG *) palloc(sizeof(*n));

	/* take max of upper endpoints */
	if (a->upper > b->upper)
	{
		n->upper = a->upper;
		n->u_sigd = a->u_sigd;
		n->u_ext = a->u_ext;
	}
	else
	{
		n->upper = b->upper;
		n->u_sigd = b->u_sigd;
		n->u_ext = b->u_ext;
	}

	/* take min of lower endpoints */
	if (a->lower < b->lower)
	{
		n->lower = a->lower;
		n->l_sigd = a->l_sigd;
		n->l_ext = a->l_ext;
	}
	else
	{
		n->lower = b->lower;
		n->l_sigd = b->l_sigd;
		n->l_ext = b->l_ext;
	}

	return (n);
}


SEG *
seg_inter(SEG * a, SEG * b)
{
	SEG		   *n;

	n = (SEG *) palloc(sizeof(*n));

	/* take min of upper endpoints */
	if (a->upper < b->upper)
	{
		n->upper = a->upper;
		n->u_sigd = a->u_sigd;
		n->u_ext = a->u_ext;
	}
	else
	{
		n->upper = b->upper;
		n->u_sigd = b->u_sigd;
		n->u_ext = b->u_ext;
	}

	/* take max of lower endpoints */
	if (a->lower > b->lower)
	{
		n->lower = a->lower;
		n->l_sigd = a->l_sigd;
		n->l_ext = a->l_ext;
	}
	else
	{
		n->lower = b->lower;
		n->l_sigd = b->l_sigd;
		n->l_ext = b->l_ext;
	}

	return (n);
}

void
rt_seg_size(SEG * a, float *size)
{
	if (a == (SEG *) NULL || a->upper <= a->lower)
		*size = 0.0;
	else
		*size = (float) Abs(a->upper - a->lower);

	return;
}

float *
seg_size(SEG * a)
{
	float	   *result;

	result = (float *) palloc(sizeof(float));

	*result = (float) Abs(a->upper - a->lower);

	return (result);
}


/*****************************************************************************
 *				   Miscellaneous operators
 *****************************************************************************/
int32
seg_cmp(SEG * a, SEG * b)
{
	/*
	 * First compare on lower boundary position
	 */
	if (a->lower < b->lower)
		return -1;
	if (a->lower > b->lower)
		return 1;

	/*
	 * a->lower == b->lower, so consider type of boundary.
	 *
	 * A '-' lower bound is < any other kind (this could only be relevant if
	 * -HUGE_VAL is used as a regular data value). A '<' lower bound is < any
	 * other kind except '-'. A '>' lower bound is > any other kind.
	 */
	if (a->l_ext != b->l_ext)
	{
		if (a->l_ext == '-')
			return -1;
		if (b->l_ext == '-')
			return 1;
		if (a->l_ext == '<')
			return -1;
		if (b->l_ext == '<')
			return 1;
		if (a->l_ext == '>')
			return 1;
		if (b->l_ext == '>')
			return -1;
	}

	/*
	 * For other boundary types, consider # of significant digits first.
	 */
	if (a->l_sigd < b->l_sigd)	/* (a) is blurred and is likely to include (b) */
		return -1;
	if (a->l_sigd > b->l_sigd)	/* (a) is less blurred and is likely to be
								 * included in (b) */
		return 1;

	/*
	 * For same # of digits, an approximate boundary is more blurred than
	 * exact.
	 */
	if (a->l_ext != b->l_ext)
	{
		if (a->l_ext == '~')	/* (a) is approximate, while (b) is exact */
			return -1;
		if (b->l_ext == '~')
			return 1;
		/* can't get here unless data is corrupt */
		elog(ERROR, "bogus lower boundary types %d %d",
			 (int) a->l_ext, (int) b->l_ext);
	}

	/* at this point, the lower boundaries are identical */

	/*
	 * First compare on upper boundary position
	 */
	if (a->upper < b->upper)
		return -1;
	if (a->upper > b->upper)
		return 1;

	/*
	 * a->upper == b->upper, so consider type of boundary.
	 *
	 * A '-' upper bound is > any other kind (this could only be relevant if
	 * HUGE_VAL is used as a regular data value). A '<' upper bound is < any
	 * other kind. A '>' upper bound is > any other kind except '-'.
	 */
	if (a->u_ext != b->u_ext)
	{
		if (a->u_ext == '-')
			return 1;
		if (b->u_ext == '-')
			return -1;
		if (a->u_ext == '<')
			return -1;
		if (b->u_ext == '<')
			return 1;
		if (a->u_ext == '>')
			return 1;
		if (b->u_ext == '>')
			return -1;
	}

	/*
	 * For other boundary types, consider # of significant digits first. Note
	 * result here is converse of the lower-boundary case.
	 */
	if (a->u_sigd < b->u_sigd)	/* (a) is blurred and is likely to include (b) */
		return 1;
	if (a->u_sigd > b->u_sigd)	/* (a) is less blurred and is likely to be
								 * included in (b) */
		return -1;

	/*
	 * For same # of digits, an approximate boundary is more blurred than
	 * exact.  Again, result is converse of lower-boundary case.
	 */
	if (a->u_ext != b->u_ext)
	{
		if (a->u_ext == '~')	/* (a) is approximate, while (b) is exact */
			return 1;
		if (b->u_ext == '~')
			return -1;
		/* can't get here unless data is corrupt */
		elog(ERROR, "bogus upper boundary types %d %d",
			 (int) a->u_ext, (int) b->u_ext);
	}

	return 0;
}

bool
seg_lt(SEG * a, SEG * b)
{
	return seg_cmp(a, b) < 0;
}

bool
seg_le(SEG * a, SEG * b)
{
	return seg_cmp(a, b) <= 0;
}

bool
seg_gt(SEG * a, SEG * b)
{
	return seg_cmp(a, b) > 0;
}

bool
seg_ge(SEG * a, SEG * b)
{
	return seg_cmp(a, b) >= 0;
}

bool
seg_different(SEG * a, SEG * b)
{
	return seg_cmp(a, b) != 0;
}



/*****************************************************************************
 *				   Auxiliary functions
 *****************************************************************************/

/* The purpose of this routine is to print the floating point
 * value with exact number of significant digits. Its behaviour
 * is similar to %.ng except it prints 8.00 where %.ng would
 * print 8
 */
static int
restore(char *result, float val, int n)
{
	static char efmt[8] = {'%', '-', '1', '5', '.', '#', 'e', 0};
	char		buf[25] = {
		'0', '0', '0', '0', '0',
		'0', '0', '0', '0', '0',
		'0', '0', '0', '0', '0',
		'0', '0', '0', '0', '0',
		'0', '0', '0', '0', '\0'
	};
	char	   *p;
	char	   *mant;
	int			exp;
	int			i,
				dp,
				sign;

	/*
	 * put a cap on the number of siugnificant digits to avoid nonsense in the
	 * output
	 */
	n = Min(n, FLT_DIG);

	/* remember the sign */
	sign = (val < 0 ? 1 : 0);

	efmt[5] = '0' + (n - 1) % 10;		/* makes %-15.(n-1)e -- this format
										 * guarantees that the exponent is
										 * always present */

	sprintf(result, efmt, val);

	/* trim the spaces left by the %e */
	for (p = result; *p != ' '; p++);
	*p = '\0';

	/* get the exponent */
	mant = (char *) strtok(strdup(result), "e");
	exp = atoi(strtok(NULL, "e"));

	if (exp == 0)
	{
		/* use the supplied mantyssa with sign */
		strcpy((char *) strchr(result, 'e'), "");
	}
	else
	{
		if (Abs(exp) <= 4)
		{
			/*
			 * remove the decimal point from the mantyssa and write the digits
			 * to the buf array
			 */
			for (p = result + sign, i = 10, dp = 0; *p != 'e'; p++, i++)
			{
				buf[i] = *p;
				if (*p == '.')
				{
					dp = i--;	/* skip the decimal point */
				}
			}
			if (dp == 0)
				dp = i--;		/* no decimal point was found in the above
								 * for() loop */

			if (exp > 0)
			{
				if (dp - 10 + exp >= n)
				{
					/*
					 * the decimal point is behind the last significant digit;
					 * the digits in between must be converted to the exponent
					 * and the decimal point placed after the first digit
					 */
					exp = dp - 10 + exp - n;
					buf[10 + n] = '\0';

					/* insert the decimal point */
					if (n > 1)
					{
						dp = 11;
						for (i = 23; i > dp; i--)
							buf[i] = buf[i - 1];
						buf[dp] = '.';
					}

					/*
					 * adjust the exponent by the number of digits after the
					 * decimal point
					 */
					if (n > 1)
						sprintf(&buf[11 + n], "e%d", exp + n - 1);
					else
						sprintf(&buf[11], "e%d", exp + n - 1);

					if (sign)
					{
						buf[9] = '-';
						strcpy(result, &buf[9]);
					}
					else
						strcpy(result, &buf[10]);
				}
				else
				{				/* insert the decimal point */
					dp += exp;
					for (i = 23; i > dp; i--)
						buf[i] = buf[i - 1];
					buf[11 + n] = '\0';
					buf[dp] = '.';
					if (sign)
					{
						buf[9] = '-';
						strcpy(result, &buf[9]);
					}
					else
						strcpy(result, &buf[10]);
				}
			}
			else
			{					/* exp <= 0 */
				dp += exp - 1;
				buf[10 + n] = '\0';
				buf[dp] = '.';
				if (sign)
				{
					buf[dp - 2] = '-';
					strcpy(result, &buf[dp - 2]);
				}
				else
					strcpy(result, &buf[dp - 1]);
			}
		}

		/* do nothing for Abs(exp) > 4; %e must be OK */
		/* just get rid of zeroes after [eE]- and +zeroes after [Ee]. */

		/* ... this is not done yet. */
	}
	return (strlen(result));
}


/*
** Miscellany
*/

bool
seg_contains_int(SEG * a, int *b)
{
	return ((a->lower <= *b) && (a->upper >= *b));
}

bool
seg_contains_float4(SEG * a, float4 *b)
{
	return ((a->lower <= *b) && (a->upper >= *b));
}

bool
seg_contains_float8(SEG * a, float8 *b)
{
	return ((a->lower <= *b) && (a->upper >= *b));
}

/* find out the number of significant digits in a string representing
 * a floating point number
 */
int
significant_digits(char *s)
{
	char	   *p = s;
	int			n,
				c,
				zeroes;

	zeroes = 1;
	/* skip leading zeroes and sign */
	for (c = *p; (c == '0' || c == '+' || c == '-') && c != 0; c = *(++p));

	/* skip decimal point and following zeroes */
	for (c = *p; (c == '0' || c == '.') && c != 0; c = *(++p))
	{
		if (c != '.')
			zeroes++;
	}

	/* count significant digits (n) */
	for (c = *p, n = 0; c != 0; c = *(++p))
	{
		if (!((c >= '0' && c <= '9') || (c == '.')))
			break;
		if (c != '.')
			n++;
	}

	if (!n)
		return (zeroes);

	return (n);
}
