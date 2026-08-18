/*
 * contrib/btree_gist/btree_utils_var.h
 *
 * Declarations for btree_gist code working with varlena indexed data types.
 */
#ifndef __BTREE_UTILS_VAR_H__
#define __BTREE_UTILS_VAR_H__

#include "access/gist.h"
#include "btree_gist.h"

/*
 * An internal index key (also called a node in some places) includes a
 * lower and an upper bound, both of which are varlena datums, packed into
 * a wrapper varlena datum.  We also work with leaf keys, which contain a
 * single varlena datum wrapped in another varlena; this case can be
 * distinguished by noting that the wrapper isn't long enough to hold a
 * second datum.  For simplicity we consider the wrapper to be a bytea.
 */
typedef bytea GBT_VARKEY;

/*
 * To actually work on a key, we use this more readable struct containing
 * pointers directly to the lower and upper values.  gbt_var_key_readable()
 * and gbt_var_key_copy() convert between these two representations.
 */
typedef struct
{
	const bytea *lower,
			   *upper;
} GBT_VARKEY_R;

/*
 * type description
 *
 * For types that set the "trnc" flag, the representation of keys on internal
 * pages may be different from that of keys on leaf pages (which match the
 * data type's normal representation).  The methods f_gt, f_ge, f_eq, f_le,
 * f_lt expect to work on the normal representation, and therefore can be used
 * only with leaf-page index entries.  The f_cmp method expects to work on the
 * representation used on internal pages, so it must not be used with leaf
 * entries.  The f_l2n method converts the leaf-page representation to the
 * internal-page representation; if that pointer is NULL, they're the same.
 *
 * Currently, btree_utils_var.c effectively assumes that internal-page keys
 * of "trnc" types are equivalent to bytea in representation and semantics.
 * The truncation process shortens the lower+upper bounds of a downlink node
 * to be of length equal to their common prefix's length plus one byte.
 * This would not work for types with comparison semantics more complex than
 * bytewise comparison.  Even then, we need a hack to deal with the fact that
 * shortening the upper bound would normally lead to its being considered less
 * than the original maximum leaf-page entry.  We handle that by considering
 * any search key that matches the bound for the bound's full length to be a
 * potential match, even if it's longer (see gbt_var_node_pf_match and its
 * callers).
 */
typedef struct
{

	/* Attribs */

	enum gbtree_type t;			/* data type */
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

/*
 * Free ptr1 in case it's a copy of ptr2.
 *
 * This is adapted from varlena's PG_FREE_IF_COPY, though doesn't require
 * fcinfo access.
 */
#define GBT_FREE_IF_COPY(ptr1, ptr2) \
	do { \
		if ((ptr1) != DatumGetPointer(ptr2)) \
			pfree(ptr1); \
	} while (0)

extern GBT_VARKEY_R gbt_var_key_readable(const GBT_VARKEY *k);

extern GBT_VARKEY *gbt_var_key_copy(const GBT_VARKEY_R *u);

extern GISTENTRY *gbt_var_compress(GISTENTRY *entry, const gbtree_vinfo *tinfo);

extern GBT_VARKEY *gbt_var_union(const GistEntryVector *entryvec, int32 *size,
								 Oid collation, const gbtree_vinfo *tinfo, FmgrInfo *flinfo);

extern bool gbt_var_same(Datum d1, Datum d2, Oid collation,
						 const gbtree_vinfo *tinfo, FmgrInfo *flinfo);

extern float *gbt_var_penalty(float *res, const GISTENTRY *o, const GISTENTRY *n,
							  Oid collation, const gbtree_vinfo *tinfo, FmgrInfo *flinfo);

extern bool gbt_var_consistent(const GBT_VARKEY_R *key, const void *query,
							   StrategyNumber strategy, Oid collation, bool is_leaf,
							   const gbtree_vinfo *tinfo, FmgrInfo *flinfo);

extern GIST_SPLITVEC *gbt_var_picksplit(const GistEntryVector *entryvec, GIST_SPLITVEC *v,
										Oid collation, const gbtree_vinfo *tinfo, FmgrInfo *flinfo);

extern void gbt_var_bin_union(Datum *u, GBT_VARKEY *e, Oid collation,
							  const gbtree_vinfo *tinfo, FmgrInfo *flinfo);

#endif
