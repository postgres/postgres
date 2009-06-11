/*
 * $PostgreSQL: pgsql/contrib/btree_gist/btree_utils_num.h,v 1.14 2009/06/11 14:48:50 momjian Exp $
 */
#ifndef __BTREE_UTILS_NUM_H__
#define __BTREE_UTILS_NUM_H__

#include "btree_gist.h"
#include "utils/rel.h"

#include <math.h>
#include <float.h>

typedef char GBT_NUMKEY;

/* Better readable key */
typedef struct
{
	const GBT_NUMKEY *lower,
			   *upper;
} GBT_NUMKEY_R;


/* for sorting */
typedef struct
{
	int			i;
	GBT_NUMKEY *t;
} Nsrt;


/* type description */

typedef struct
{

	/* Attribs */

	enum gbtree_type t;			/* data type */
	int32		size;			/* size of type , 0 means variable */

	/* Methods */

	bool		(*f_gt) (const void *, const void *);	/* greater then */
	bool		(*f_ge) (const void *, const void *);	/* greater equal */
	bool		(*f_eq) (const void *, const void *);	/* equal */
	bool		(*f_le) (const void *, const void *);	/* less equal */
	bool		(*f_lt) (const void *, const void *);	/* less then */
	int			(*f_cmp) (const void *, const void *);	/* key compare function */
} gbtree_ninfo;


/*
 *	Numeric btree functions
*/



/*
 * Note: The factor 0.49 in following macro avoids floating point overflows
*/
#define penalty_num(result,olower,oupper,nlower,nupper) do { \
  double	tmp = 0.0F; \
  (*(result))	= 0.0F; \
  if ( (nupper) > (oupper) ) \
	  tmp += ( ((double)nupper)*0.49F - ((double)oupper)*0.49F ); \
  if (	(olower) > (nlower)  ) \
	  tmp += ( ((double)olower)*0.49F - ((double)nlower)*0.49F ); \
  if (tmp > 0.0F) \
  { \
	(*(result)) += FLT_MIN; \
	(*(result)) += (float) ( ((double)(tmp)) / ( (double)(tmp) + ( ((double)(oupper))*0.49F - ((double)(olower))*0.49F ) ) ); \
	(*(result)) *= (FLT_MAX / (((GISTENTRY *) PG_GETARG_POINTER(0))->rel->rd_att->natts + 1)); \
  } \
} while (0);


/*
 * Convert an Interval to an approximate equivalent number of seconds
 * (as a double).  Here because we need it for time/timetz as well as
 * interval.  See interval_cmp_internal for comparison.
 */
#ifdef HAVE_INT64_TIMESTAMP
#define INTERVAL_TO_SEC(ivp) \
	(((double) (ivp)->time) / ((double) USECS_PER_SEC) + \
	 (ivp)->day * (24.0 * SECS_PER_HOUR) + \
	 (ivp)->month * (30.0 * SECS_PER_DAY))
#else
#define INTERVAL_TO_SEC(ivp) \
	((ivp)->time + \
	 (ivp)->day * (24.0 * SECS_PER_HOUR) + \
	 (ivp)->month * (30.0 * SECS_PER_DAY))
#endif


extern bool gbt_num_consistent(const GBT_NUMKEY_R *key, const void *query,
				   const StrategyNumber *strategy, bool is_leaf,
				   const gbtree_ninfo *tinfo);

extern GIST_SPLITVEC *gbt_num_picksplit(const GistEntryVector *entryvec, GIST_SPLITVEC *v,
				  const gbtree_ninfo *tinfo);

extern GISTENTRY *gbt_num_compress(GISTENTRY *retval, GISTENTRY *entry,
				 const gbtree_ninfo *tinfo);


extern void *gbt_num_union(GBT_NUMKEY *out, const GistEntryVector *entryvec,
			  const gbtree_ninfo *tinfo);

extern bool gbt_num_same(const GBT_NUMKEY *a, const GBT_NUMKEY *b,
			 const gbtree_ninfo *tinfo);

extern void gbt_num_bin_union(Datum *u, GBT_NUMKEY *e,
				  const gbtree_ninfo *tinfo);

#endif
