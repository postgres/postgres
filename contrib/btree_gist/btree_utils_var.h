#ifndef __BTREE_UTILS_VAR_H__
#define __BTREE_UTILS_VAR_H__

#include "btree_gist.h"

#include "mb/pg_wchar.h"

/* Variable length key */
typedef bytea GBT_VARKEY;

/* Better readable key */
typedef struct
{
	bytea	   *lower,
			   *upper;
}	GBT_VARKEY_R;

/* used for key sorting */
typedef struct
{
	int			i;
	GBT_VARKEY *t;
}	Vsrt;

/*
  type description
*/


typedef struct
{

	/* Attribs */

	enum gbtree_type t;			/* data type */
	int32		eml;			/* cached pg_database_encoding_max_length (0:
								 * undefined) */
	bool		trnc;			/* truncate (=compress) key */

	/* Methods */

	bool		(*f_gt) (const void *, const void *);	/* greater then */
	bool		(*f_ge) (const void *, const void *);	/* greater equal */
	bool		(*f_eq) (const void *, const void *);	/* equal */
	bool		(*f_le) (const void *, const void *);	/* less equal */
	bool		(*f_lt) (const void *, const void *);	/* less then */
	int32		(*f_cmp) (const bytea *, const bytea *);		/* node compare */
	GBT_VARKEY *(*f_l2n) (GBT_VARKEY *);		/* convert leaf to node */
}	gbtree_vinfo;



extern GBT_VARKEY_R gbt_var_key_readable(const GBT_VARKEY * k);

extern GBT_VARKEY *gbt_var_key_copy(const GBT_VARKEY_R * u, bool force_node);

extern GISTENTRY *gbt_var_compress(GISTENTRY *entry, const gbtree_vinfo * tinfo);

extern GBT_VARKEY *gbt_var_union(const GistEntryVector *entryvec, int32 *size,
			  const gbtree_vinfo * tinfo);

extern bool gbt_var_same(bool *result, const Datum d1, const Datum d2,
			 const gbtree_vinfo * tinfo);

extern float *gbt_var_penalty(float *res, const GISTENTRY *o, const GISTENTRY *n,
				const gbtree_vinfo * tinfo);

extern bool gbt_var_consistent(GBT_VARKEY_R * key, const void *query,
				   const StrategyNumber *strategy, bool is_leaf,
				   const gbtree_vinfo * tinfo);

extern GIST_SPLITVEC *gbt_var_picksplit(const GistEntryVector *entryvec, GIST_SPLITVEC *v,
				  const gbtree_vinfo * tinfo);
extern void gbt_var_bin_union(Datum *u, GBT_VARKEY * e,
				  const gbtree_vinfo * tinfo);

#endif
