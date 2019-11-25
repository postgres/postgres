/*
 * contrib/btree_gist/btree_utils_var.h
 */
#ifndef __BTREE_UTILS_VAR_H__
#define __BTREE_UTILS_VAR_H__

#include "access/gist.h"
#include "btree_gist.h"
#include "mb/pg_wchar.h"

/* Variable length key */
typedef bytea GBT_VARKEY;

/* Better readable key */
typedef struct
{
	bytea	   *lower,
			   *upper;
} GBT_VARKEY_R;

/*
 * type description
 */
typedef struct
{

	/* Attribs */

	enum gbtree_type t;			/* data type */
	int32		eml;			/* cached pg_database_encoding_max_length (0:
								 * undefined) */
	bool		trnc;			/* truncate (=compress) key */

	/* Methods */

	bool		(*f_gt) (const void *, const void *, Oid, FmgrInfo *);	/* greater than */
	bool		(*f_ge) (const void *, const void *, Oid, FmgrInfo *);	/* greater equal */
	bool		(*f_eq) (const void *, const void *, Oid, FmgrInfo *);	/* equal */
	bool		(*f_le) (const void *, const void *, Oid, FmgrInfo *);	/* less equal */
	bool		(*f_lt) (const void *, const void *, Oid, FmgrInfo *);	/* less than */
	int32		(*f_cmp) (const void *, const void *, Oid, FmgrInfo *); /* compare */
	GBT_VARKEY *(*f_l2n) (GBT_VARKEY *, FmgrInfo *flinfo);	/* convert leaf to node */
} gbtree_vinfo;



extern GBT_VARKEY_R gbt_var_key_readable(const GBT_VARKEY *k);

extern GBT_VARKEY *gbt_var_key_copy(const GBT_VARKEY_R *u);

extern GISTENTRY *gbt_var_compress(GISTENTRY *entry, const gbtree_vinfo *tinfo);

extern GBT_VARKEY *gbt_var_union(const GistEntryVector *entryvec, int32 *size,
								 Oid collation, const gbtree_vinfo *tinfo, FmgrInfo *flinfo);

extern bool gbt_var_same(Datum d1, Datum d2, Oid collation,
						 const gbtree_vinfo *tinfo, FmgrInfo *flinfo);

extern float *gbt_var_penalty(float *res, const GISTENTRY *o, const GISTENTRY *n,
							  Oid collation, const gbtree_vinfo *tinfo, FmgrInfo *flinfo);

extern bool gbt_var_consistent(GBT_VARKEY_R *key, const void *query,
							   StrategyNumber strategy, Oid collation, bool is_leaf,
							   const gbtree_vinfo *tinfo, FmgrInfo *flinfo);

extern GIST_SPLITVEC *gbt_var_picksplit(const GistEntryVector *entryvec, GIST_SPLITVEC *v,
										Oid collation, const gbtree_vinfo *tinfo, FmgrInfo *flinfo);

extern void gbt_var_bin_union(Datum *u, GBT_VARKEY *e, Oid collation,
							  const gbtree_vinfo *tinfo, FmgrInfo *flinfo);

#endif
